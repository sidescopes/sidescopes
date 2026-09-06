// ScreenCaptureKit backend.
//
// Hard-won specifics encoded here:
//  - The content filter excludes exactly one thing: this application.
//    Application-level exclusion tracks the app's windows dynamically, so
//    the scope window, the region border, and the picker overlay can
//    never leak into the sampled pixels. Window-list exclusion variants
//    snapshot at creation time and must not be used for anything that can
//    appear later (Quick Look previews are hosted by an on-demand service
//    process, for example) - so nothing else is excluded.
//  - Streams die when the display configuration blinks (lock screen,
//    display sleep). Death is reported through the status callback and the
//    application restarts capture; this backend never retries on its own.
//  - Frames are requested in sRGB, so the OS converts from the display's
//    color space and the scopes read honest sRGB values.
//  - Frames are requested at ten bits a channel. The compositor carries
//    more than a byte resolves and a BGRA capture threw it away; the
//    request is not load-bearing, since every delivery is stamped with
//    the format it actually arrived in.

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "core/diagnostics.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "platform/desktop.h"
#include "platform/macos/capture_completion.h"
#include "platform/macos/capture_frame.h"
#include "platform/pixel_average.h"
#include "platform/screen_capture.h"

@interface SidescopesStreamHandler : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::shared_ptr<sidescopes::SckCallbackState> callbacks;
}
@end

namespace sidescopes {
namespace {

// The lock must be released even when the destination's page allocation fails.
class PixelBufferReadLock
{
public:
    explicit PixelBufferReadLock(CVPixelBufferRef image)
        : m_image(image)
    {
        m_locked = CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess;
    }

    ~PixelBufferReadLock()
    {
        if (m_locked) {
            CVPixelBufferUnlockBaseAddress(m_image, kCVPixelBufferLock_ReadOnly);
        }
    }

    PixelBufferReadLock(const PixelBufferReadLock&) = delete;
    PixelBufferReadLock& operator=(const PixelBufferReadLock&) = delete;

    [[nodiscard]] bool locked() const
    {
        return m_locked;
    }

private:
    CVPixelBufferRef m_image;
    bool m_locked = false;
};

SCShareableContent* fetchShareableContent()
{
    try {
        const auto completion = std::make_shared<CaptureCompletion<SCShareableContent*>>();
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
          try {
              (void)completion->complete(error ? nil : content);
          } catch (...) {  // Keep C++ bookkeeping failures inside the framework callback.
              diagEmit(DiagChannel::Perf, "shareable content callback failed");
          }
        }];
        auto result = completion->wait(CaptureCompletionTimeout);
        if (!result) {
            diagEmit(DiagChannel::Perf, "shareable content request timed out");
        }
        return result.value_or(nil);
    } catch (...) {
        return nil;
    }
}

SCDisplay* findCaptureDisplay(SCShareableContent* content, const std::string& identifier)
{
    SCDisplay* display = nil;
    for (SCDisplay* candidate in content.displays) {
        if (std::to_string(candidate.displayID) == identifier) {
            display = candidate;
        }
    }

    return display;
}

// The content filter for a display, excluding this application's own windows.
// Test harnesses lift the self-exclusion so captures include them.
SCContentFilter* buildContentFilter(SCShareableContent* content, SCDisplay* display)
{
    SCRunningApplication* selfApplication = nil;
    for (SCRunningApplication* application in content.applications) {
        if (application.processID == NSProcessInfo.processInfo.processIdentifier) {
            selfApplication = application;
            break;
        }
    }
    if (selfApplication && !captureExclusionDisabled()) {
        return [[SCContentFilter alloc] initWithDisplay:display
                                  excludingApplications:@[ selfApplication ]
                                       exceptingWindows:@[]];
    }

    return [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
}

CGFloat pixelScaleOf(SCContentFilter* filter)
{
    return filter.pointPixelScale > 0 ? filter.pointPixelScale : 2.0;
}

// Narrows @p configuration to @p crop, which is in display PIXELS while
// sourceRect is in points - the conversion is the whole reason this is a function
// and not two lines. width and height stay in pixels and become the crop's own,
// so the delivered buffer is exactly the region and the copy shrinks with it.
void narrowConfiguration(SCStreamConfiguration* configuration, IntRect crop, CGFloat scale)
{
    configuration.sourceRect =
        CGRectMake(static_cast<CGFloat>(crop.x) / scale, static_cast<CGFloat>(crop.y) / scale,
                   static_cast<CGFloat>(crop.width) / scale, static_cast<CGFloat>(crop.height) / scale);
    configuration.width = static_cast<size_t>(crop.width);
    configuration.height = static_cast<size_t>(crop.height);
}

SCStreamConfiguration* makeStreamConfiguration(SCDisplay* display, SCContentFilter* filter, int maxFramesPerSecond,
                                               const std::optional<IntRect>& crop)
{
    SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
    const CGFloat scale = pixelScaleOf(filter);
    configuration.width = static_cast<size_t>(static_cast<CGFloat>(display.width) * scale);
    configuration.height = static_cast<size_t>(static_cast<CGFloat>(display.height) * scale);
    if (crop) {
        narrowConfiguration(configuration, *crop, scale);
    }
    // Ten bits a channel. The compositor carries about two bits more than a
    // byte resolves - measured against Lightroom Classic and Capture One, on
    // the same RAW, at both fit and 1:1 zoom - and a BGRA capture is what threw
    // them away. It costs no memory: both layouts are four bytes a pixel.
    //
    // Asked for rather than assumed. Nothing here fails if the system declines
    // it: every delivery is stamped with the format it actually arrived in, so
    // an unsupported request degrades to whatever does arrive.
    configuration.pixelFormat = kCVPixelFormatType_ARGB2101010LEPacked;
    configuration.minimumFrameInterval = CMTimeMake(1, maxFramesPerSecond);
    configuration.showsCursor = NO;
    configuration.queueDepth = 5;
    configuration.colorSpaceName = kCGColorSpaceSRGB;

    return configuration;
}

// The layout a delivered buffer really holds. Read from the buffer rather than
// taken from what the configuration asked for: the two need not agree, and a
// frame read in the wrong layout is a plausible-looking trace built from the
// wrong bits.
std::optional<PixelFormat> formatOfBuffer(CVImageBufferRef image)
{
    switch (CVPixelBufferGetPixelFormatType(image)) {
    case kCVPixelFormatType_32BGRA:
        return PixelFormat::Bgra8;
    case kCVPixelFormatType_ARGB2101010LEPacked:
        return PixelFormat::Argb2101010;
    default:
        return std::nullopt;
    }
}

// Redrawing into a known-layout bitmap sidesteps whatever byte order the
// capture returned, then averages the tiny neighborhood to one color. The
// average is weighted by coverage - see averagePremultiplied, which is where
// the reason lives.
std::optional<FloatColor> averageCapturedImage(CGImageRef image)
{
    constexpr size_t Pixels = 8;
    uint8_t pixels[Pixels * Pixels * 4] = {};
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        pixels, Pixels, Pixels, 8, Pixels * 4, srgb,
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(srgb);
    if (!context) {
        return std::nullopt;
    }
    CGContextDrawImage(context, CGRectMake(0, 0, Pixels, Pixels), image);
    CGContextRelease(context);

    return averagePremultiplied(pixels, Pixels * Pixels);
}

}  // namespace

bool deliverCapturePixels(CVPixelBufferRef image, FrameBuffer& buffer, FrameMailbox& mailbox) noexcept
{
    try {
        {
            const PixelBufferReadLock lock(image);
            if (!lock.locked()) {
                return false;
            }
            const auto* source = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
            if (!source) {
                return false;
            }
            const auto sourceStride = CVPixelBufferGetBytesPerRow(image);
            buffer.sizeTo(static_cast<std::size_t>(buffer.strideBytes) * buffer.height);
            for (int py = 0; py < buffer.height; ++py) {
                std::memcpy(buffer.data.data() + static_cast<std::size_t>(py) * buffer.strideBytes,
                            source + static_cast<std::size_t>(py) * sourceStride,
                            static_cast<std::size_t>(buffer.width) * 4);
            }
        }  // Release the native surface before publishing the owned copy.
        buffer = mailbox.publish(std::move(buffer));
        return true;
    } catch (...) {
        // Drop this frame; a later delivery can allocate and publish normally.
        return false;
    }
}

CaptureStartResult startCaptureWithDeadline(SCStream* stream, const std::shared_ptr<SckCallbackState>& callbacks,
                                            std::chrono::milliseconds timeout)
{
    std::optional<bool> result{false};
    std::shared_ptr<CaptureCompletion<bool>> completion;
    try {
        completion = std::make_shared<CaptureCompletion<bool>>();
        __weak SCStream* weakStream = stream;
        [stream startCaptureWithCompletionHandler:^(NSError* error) {
          try {
              if (!completion->complete(error == nil) && completion->abandoned() && error == nil) {
                  // Do not retain stream through its own completion, and never
                  // wait on this callback queue when cancelling a late start.
                  SCStream* lateStream = weakStream;
                  [lateStream stopCaptureWithCompletionHandler:nil];
              }
          } catch (...) {
              completion->abandon();
              SCStream* lateStream = weakStream;
              [lateStream stopCaptureWithCompletionHandler:nil];
          }
        }];
        result = completion->wait(timeout);
    } catch (...) {
        // Allocation/submission failure follows the same retirement path.
        if (completion) {
            completion->abandon();
        }
    }
    if (result.value_or(false)) {
        return CaptureStartResult::Started;
    }
    callbacks->retire();
    // A timed-out start has not set m_running. Cancel it regardless, without
    // waiting again; its late completion independently cancels late success.
    [stream stopCaptureWithCompletionHandler:nil];
    return result ? CaptureStartResult::Failed : CaptureStartResult::TimedOut;
}

bool stopCaptureWithDeadline(SCStream* stream, std::chrono::milliseconds timeout)
{
    try {
        const auto completion = std::make_shared<CaptureCompletion<bool>>();
        [stream stopCaptureWithCompletionHandler:^(NSError* error) {
          try {
              (void)completion->complete(error == nil);
          } catch (...) {  // A missing completion still has a bounded caller.
              diagEmit(DiagChannel::Perf, "capture stop callback failed");
          }
        }];
        return completion->wait(timeout).value_or(false);
    } catch (...) {
        [stream stopCaptureWithCompletionHandler:nil];
        return false;
    }
}

class SckScreenCaptureSource final : public ScreenCaptureSource
{
public:
    ~SckScreenCaptureSource() override
    {
        stop();
    }

    CapturePermission requestPermission() override
    {
        if (CGPreflightScreenCaptureAccess()) {
            return CapturePermission::Granted;
        }
        return CGRequestScreenCaptureAccess() ? CapturePermission::Granted : CapturePermission::Denied;
    }

    std::vector<CaptureTarget> listTargets() override
    {
        std::vector<CaptureTarget> targets;
        SCShareableContent* content = fetchShareableContent();
        if (!content) {
            return targets;
        }
        for (SCDisplay* display in content.displays) {
            CaptureTarget target;
            target.identifier = std::to_string(display.displayID);
            // CGDirectDisplayID is the identity the desktop services use.
            target.displayId = display.displayID;
            target.widthPoints = static_cast<int>(display.width);
            target.heightPoints = static_cast<int>(display.height);
            target.description = "Display " + target.identifier + " (" + std::to_string(target.widthPoints) + "x" +
                                 std::to_string(target.heightPoints) + ")";
            targets.push_back(std::move(target));
        }
        return targets;
    }

    bool start(const CaptureTarget& target, int maxFramesPerSecond, FrameMailbox& mailbox) override
    {
        stop();
        m_mailbox = &mailbox;

        SCShareableContent* content = fetchShareableContent();
        if (!content) {
            return fail("shareable content unavailable (permission?)");
        }
        SCDisplay* display = findCaptureDisplay(content, target.identifier);
        if (!display) {
            return fail("capture target no longer present");
        }

        SCContentFilter* filter = buildContentFilter(content, display);
        SCStreamConfiguration* configuration =
            makeStreamConfiguration(display, filter, maxFramesPerSecond, std::nullopt);
        m_display = display;
        m_filter = filter;
        m_maxFramesPerSecond = maxFramesPerSecond;
        {
            // A stream starts on the whole display; narrowing is asked for later,
            // once a region has settled.
            std::lock_guard lock(m_geometryMutex);
            m_crop.reset();
            m_displayPixelWidth = static_cast<int>(configuration.width);
            m_displayPixelHeight = static_cast<int>(configuration.height);
        }

        m_handler = [[SidescopesStreamHandler alloc] init];
        m_callbacks = std::make_shared<SckCallbackState>();
        m_callbacks->owner = this;
        m_handler->callbacks = m_callbacks;
        m_stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:m_handler];
        m_queue = dispatch_queue_create("sidescopes.capture", DISPATCH_QUEUE_SERIAL);

        NSError* error = nil;
        if (![m_stream addStreamOutput:m_handler
                                  type:SCStreamOutputTypeScreen
                    sampleHandlerQueue:m_queue
                                 error:&error]) {
            return fail("adding the stream output failed");
        }

        const CaptureStartResult started = startCaptureWithDeadline(m_stream, m_callbacks);
        if (started != CaptureStartResult::Started) {
            resetStreamState();
            return fail(started == CaptureStartResult::TimedOut ? "starting the capture stream timed out"
                                                                : "starting the capture stream failed");
        }

        m_running.store(true);
        return true;
    }

    void narrowTo(const std::optional<IntRect>& rect) override
    {
        if (!m_running.load() || m_stream == nil || m_display == nil) {
            return;
        }
        {
            std::lock_guard lock(m_geometryMutex);
            if (m_crop == rect) {
                return;
            }
            m_crop = rect;
        }
        SCStreamConfiguration* configuration = makeStreamConfiguration(m_display, m_filter, m_maxFramesPerSecond, rect);
        const auto callbacks = m_callbacks;
        // Fire and forget: frames keep arriving at the old geometry until this
        // lands, and each frame is stamped from the size actually delivered, so
        // nothing downstream depends on when that happens.
        [m_stream updateConfiguration:configuration
                    completionHandler:^(NSError* error) {
                      try {
                          const std::lock_guard lock(callbacks->mutex);
                          if (callbacks->owner != nullptr) {
                              callbacks->owner->handleConfigurationError(error);
                          }
                      } catch (...) {
                          // Reporting a capture error must not escape the SDK callback.
                          diagEmit(DiagChannel::Perf, "capture configuration callback failed");
                      }
                    }];
    }

    void stop() override
    {
        m_running.store(false);
        if (m_callbacks) {
            m_callbacks->retire();
        }
        if (m_stream && !stopCaptureWithDeadline(m_stream)) {
            diagEmit(DiagChannel::Perf, "stopping the capture stream failed or timed out");
        }
        resetStreamState();
    }

    void setStatusCallback(StatusCallback callback) override
    {
        m_status = std::move(callback);
    }

    // Called on the capture queue.
    void handleSample(CMSampleBufferRef sample)
    {
        if (!m_running.load() || m_mailbox == nullptr) {
            return;
        }

        // Only complete frames carry new content; idle deliveries are
        // dropped so a static screen produces no downstream work.
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
            NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
            NSNumber* status = info[SCStreamFrameInfoStatus];
            if (status && static_cast<SCFrameStatus>(status.intValue) != SCFrameStatusComplete) {
                return;
            }
        }

        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        if (!image) {
            return;
        }
        // A layout no scope can read is dropped rather than guessed at.
        const std::optional<PixelFormat> format = formatOfBuffer(image);
        if (!format) {
            return;
        }
        const int width = static_cast<int>(CVPixelBufferGetWidth(image));
        const int height = static_cast<int>(CVPixelBufferGetHeight(image));

        // Which part of the display these pixels are is decided by the size
        // actually delivered, not by what was last asked for: a narrowing takes
        // effect some frames after the request, and a frame whose origin we could
        // only guess at would put every scope on the wrong pixels. A delivery
        // matching neither geometry is one in flight across a change, and is
        // dropped rather than guessed at - one frame, thirty times a second.
        const std::optional<IntRect> stamp = geometryFor(width, height);
        if (!stamp) {
            return;
        }

        stampBuffer(width, height, *format, *stamp);
        (void)deliverCapturePixels(image, m_buffer, *m_mailbox);
    }

    // Describes the delivery in the recycled buffer. Every
    // field is written on every frame, never only on the ones that changed: the
    // buffer comes back from the mailbox holding the previous delivery's
    // answers, and one left alone mislabels these pixels.
    void stampBuffer(int width, int height, PixelFormat format, IntRect stamp)
    {
        m_buffer.width = width;
        m_buffer.height = height;
        m_buffer.strideBytes = width * 4;  // repacked tightly, surface padding dropped
        m_buffer.colorSpace = ColorSpaceHint::Srgb;
        m_buffer.format = format;
        m_buffer.sequence = ++m_sequence;
        m_buffer.sourceX = stamp.x;
        m_buffer.sourceY = stamp.y;
        m_buffer.sourceWidth = stamp.width;
        m_buffer.sourceHeight = stamp.height;
        // Logged when it changes rather than thirty times a second, and stated
        // afresh to every recording: capture settles its format in the first
        // second of a run, and a log switched on later still has to say what
        // depth is being delivered.
        if (m_loggedFormat.shouldLog(format)) {
            SS_DIAG(Perf, "capture format %s", format == PixelFormat::Argb2101010 ? "10-bit" : "8-bit");
        }
    }

    // The source stamp for a delivery of @p width by @p height: the crop's origin
    // with the display's extents when it is the narrowed geometry, all zeros when
    // it is the whole display, and nothing when it is neither.
    //
    // Telling the two apart by their dimensions is only sound because one crop is
    // never asked for while another is in force - the settle rule on CropTracker
    // sends the capture back to the whole display in between.
    //
    // Returned as a rectangle carrying origin and the DISPLAY's extents, which is
    // what a frame records - not the crop's own size, which is the frame's.
    [[nodiscard]] std::optional<IntRect> geometryFor(int width, int height) const
    {
        std::lock_guard lock(m_geometryMutex);
        const bool whole = width == m_displayPixelWidth && height == m_displayPixelHeight;
        if (whole) {
            return IntRect{};
        }
        if (m_crop && width == m_crop->width && height == m_crop->height) {
            return IntRect{m_crop->x, m_crop->y, m_displayPixelWidth, m_displayPixelHeight};
        }

        return std::nullopt;
    }

    void handleStopped(NSError* error)
    {
        m_running.store(false);
        if (m_status) {
            m_status(error ? std::string("capture stopped: ") + error.localizedDescription.UTF8String
                           : "capture stopped");
        }
    }

    void handleConfigurationError(NSError* error)
    {
        if (error != nil && m_status) {
            m_status(std::string("narrowing the capture failed: ") + error.localizedDescription.UTF8String);
        }
    }

private:
    // Drops the stream, its handler, and the mailbox link after callback
    // retirement. Shared by failed starts and normal teardown.
    void resetStreamState()
    {
        if (m_callbacks) {
            m_callbacks->retire();
        }
        m_callbacks.reset();
        m_stream = nil;
        m_handler = nil;
        m_queue = nil;
        m_mailbox = nullptr;
        // The recycled buffer is a whole display of pixels; a stopped stream
        // keeps it warm for deliveries that are not coming. All callbacks
        // have finished or lost access to this stream's state by here.
        m_buffer = FrameBuffer{};
        m_display = nil;
        m_filter = nil;
    }

    bool fail(const std::string& message)
    {
        if (m_status) {
            m_status(message);
        }
        return false;
    }

    SCStream* m_stream = nil;
    SidescopesStreamHandler* m_handler = nil;
    dispatch_queue_t m_queue = nil;
    std::shared_ptr<SckCallbackState> m_callbacks;
    FrameMailbox* m_mailbox = nullptr;
    FrameBuffer m_buffer;  // recycled storage, touched only on the capture queue
    // The layout this recording has been told about, read on the capture queue
    // and forgotten whenever a recording opens.
    DiagOnChange<PixelFormat> m_loggedFormat{DiagChannel::Perf};
    StatusCallback m_status;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_sequence{0};

    // What a narrowing needs to rebuild a configuration, kept from the start that
    // succeeded.
    SCDisplay* m_display = nil;
    SCContentFilter* m_filter = nil;
    int m_maxFramesPerSecond = 0;

    // The geometry the delivered frames are stamped with. Written on whatever
    // thread asks for a narrowing, read on the capture queue for every frame, so
    // it is guarded; the lock is held for four reads a frame.
    mutable std::mutex m_geometryMutex;
    std::optional<IntRect> m_crop;
    int m_displayPixelWidth = 0;
    int m_displayPixelHeight = 0;
};

std::unique_ptr<ScreenCaptureSource> createScreenCaptureSource()
{
    return std::make_unique<SckScreenCaptureSource>();
}

namespace {

// Shareable content is an XPC round trip, far too heavy per sample; the
// one-shot sampler caches it and refreshes only when the wanted display
// is missing or its geometry went stale (a resolution change keeps the
// display id). Main-thread only, like the sampler itself.
SCShareableContent* g_samplerContent = nil;

SCDisplay* samplerDisplay(CGDirectDisplayID displayId)
{
    const CGRect bounds = CGDisplayBounds(displayId);
    const auto lookup = [&]() -> SCDisplay* {
        for (SCDisplay* candidate in g_samplerContent.displays) {
            if (candidate.displayID == displayId && candidate.width == static_cast<NSInteger>(bounds.size.width) &&
                candidate.height == static_cast<NSInteger>(bounds.size.height)) {
                return candidate;
            }
        }
        return nil;
    };
    if (SCDisplay* display = lookup()) {
        return display;
    }
    g_samplerContent = fetchShareableContent();
    return lookup();
}

}  // namespace

// The cursor readout away from the captured display: a one-shot capture
// of a tiny rectangle around the point, excluding this application the
// way the main stream does, requested in sRGB for the same honest
// values.
void sampleScreenColorAsync(DesktopPoint point, std::function<void(std::optional<FloatColor>)> callback)
{
    CGDirectDisplayID displayId = 0;
    uint32_t matches = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(point.x, point.y), 1, &displayId, &matches) != kCGErrorSuccess ||
        matches == 0) {
        callback(std::nullopt);
        return;
    }
    SCDisplay* display = samplerDisplay(displayId);
    if (!display) {
        callback(std::nullopt);
        return;
    }
    SCContentFilter* filter = buildContentFilter(g_samplerContent, display);

    // A small neighborhood around the point, clamped inside the display,
    // in the display's own point coordinates.
    const CGRect bounds = CGDisplayBounds(displayId);
    constexpr double Side = 5.0;
    constexpr size_t Pixels = 8;
    const double localX = std::clamp(point.x - bounds.origin.x - Side / 2, 0.0, bounds.size.width - Side);
    const double localY = std::clamp(point.y - bounds.origin.y - Side / 2, 0.0, bounds.size.height - Side);

    SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
    configuration.sourceRect = CGRectMake(localX, localY, Side, Side);
    configuration.width = Pixels;
    configuration.height = Pixels;
    configuration.showsCursor = NO;
    configuration.pixelFormat = kCVPixelFormatType_32BGRA;
    configuration.colorSpaceName = kCGColorSpaceSRGB;

    auto shared = std::make_shared<std::function<void(std::optional<FloatColor>)>>(std::move(callback));
    [SCScreenshotManager captureImageWithFilter:filter
                                  configuration:configuration
                              completionHandler:^(CGImageRef image, NSError* error) {
                                try {
                                    if (!image || error) {
                                        (*shared)(std::nullopt);
                                        return;
                                    }
                                    (*shared)(averageCapturedImage(image));
                                } catch (...) {
                                    // Caller failures must not unwind the framework's completion queue.
                                    diagEmit(DiagChannel::Perf, "color sample callback failed");
                                }
                              }];
}

namespace {

// Redraws the captured image into a known BGRA layout - top-down, tightly
// packed - so the detector reads it exactly like a live capture frame,
// whatever byte order the screenshot returned.
std::optional<CapturedImage> imageToBgra(CGImageRef image)
{
    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0) {
        return std::nullopt;
    }
    CapturedImage captured;
    captured.width = static_cast<int>(width);
    captured.height = static_cast<int>(height);
    captured.bgra.resize(width * height * 4);
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        captured.bgra.data(), width, height, 8, width * 4, srgb,
        static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(srgb);
    if (!context) {
        return std::nullopt;
    }
    CGContextDrawImage(context, CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)), image);
    CGContextRelease(context);

    return captured;
}

}  // namespace

std::optional<CapturedImage> convertCaptureImage(CGImageRef image) noexcept
{
    if (!image) {
        return std::nullopt;
    }
    try {
        return imageToBgra(image);
    } catch (...) {
        return std::nullopt;
    }
}

// A full-display one-shot for off-stream analysis: fresh shareable content
// (this runs on a background thread, so it must not touch the main-thread
// sampler cache), the same self-exclusion and sRGB request the stream uses,
// captured through the same bounded completion as fetchShareableContent.
std::optional<CapturedImage> captureDisplayImage(uint32_t displayId)
{
    try {
        @autoreleasepool {
            SCShareableContent* content = fetchShareableContent();
            if (!content) {
                return std::nullopt;
            }
            SCDisplay* display = findCaptureDisplay(content, std::to_string(displayId));
            if (!display) {
                return std::nullopt;
            }
            SCContentFilter* filter = buildContentFilter(content, display);
            SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
            const CGFloat scale = filter.pointPixelScale > 0 ? filter.pointPixelScale : 2.0;
            configuration.width = static_cast<size_t>(static_cast<CGFloat>(display.width) * scale);
            configuration.height = static_cast<size_t>(static_cast<CGFloat>(display.height) * scale);
            configuration.showsCursor = NO;
            configuration.pixelFormat = kCVPixelFormatType_32BGRA;
            configuration.colorSpaceName = kCGColorSpaceSRGB;

            const auto completion = std::make_shared<CaptureCompletion<CaptureImageOwner>>();
            [SCScreenshotManager captureImageWithFilter:filter
                                          configuration:configuration
                                      completionHandler:^(CGImageRef image, NSError* error) {
                                        try {
                                            (void)completion->complete(CaptureImageOwner(
                                                image && !error ? CGImageRetain(image) : nullptr, CGImageRelease));
                                        } catch (...) {
                                            diagEmit(DiagChannel::Perf, "display image callback failed");
                                        }
                                      }];
            auto image = completion->wait(CaptureCompletionTimeout);
            if (!image || !*image) {
                return std::nullopt;
            }
            // Conversion allocates. Keep it outside the framework callback and
            // release the retained image even when allocation fails.
            return convertCaptureImage(image->get());
        }
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace sidescopes

@implementation SidescopesStreamHandler

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeScreen) {
        return;
    }
    try {
        const std::lock_guard lock(callbacks->mutex);
        if (auto* owner = callbacks->owner) {
            owner->handleSample(sampleBuffer);
        }
    } catch (...) {
        // Never unwind C++ failures through the framework's delivery queue.
        sidescopes::diagEmit(sidescopes::DiagChannel::Perf, "capture frame callback failed");
    }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    (void)stream;
    try {
        const std::lock_guard lock(callbacks->mutex);
        if (auto* owner = callbacks->owner) {
            owner->handleStopped(error);
        }
    } catch (...) {
        // handleStopped clears the running state before attempting notification.
        sidescopes::diagEmit(sidescopes::DiagChannel::Perf, "capture failure callback failed");
    }
}

@end
