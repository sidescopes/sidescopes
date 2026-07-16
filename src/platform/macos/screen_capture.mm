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

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include "platform/desktop.h"
#include "platform/screen_capture.h"

namespace sidescopes {
class SckScreenCaptureSource;
}

@interface SidescopesStreamHandler : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) sidescopes::SckScreenCaptureSource* owner;
@end

namespace sidescopes {
namespace {

SCShareableContent* fetchShareableContent()
{
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block SCShareableContent* result = nil;
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
      (void)error;
      result = content;
      dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return result;
}

}  // namespace

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
        SCDisplay* display = nil;
        for (SCDisplay* candidate in content.displays) {
            if (std::to_string(candidate.displayID) == target.identifier) {
                display = candidate;
            }
        }
        if (!display) {
            return fail("capture target no longer present");
        }

        SCRunningApplication* selfApplication = nil;
        for (SCRunningApplication* application in content.applications) {
            if (application.processID == NSProcessInfo.processInfo.processIdentifier) {
                selfApplication = application;
                break;
            }
        }
        // Test harnesses lift the self-exclusion so captures include this
        // application's own windows.
        SCContentFilter* filter = (selfApplication && !captureExclusionDisabled())
                                      ? [[SCContentFilter alloc] initWithDisplay:display
                                                           excludingApplications:@[ selfApplication ]
                                                                exceptingWindows:@[]]
                                      : [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];

        SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
        const CGFloat scale = filter.pointPixelScale > 0 ? filter.pointPixelScale : 2.0;
        configuration.width = static_cast<size_t>(display.width * scale);
        configuration.height = static_cast<size_t>(display.height * scale);
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        configuration.minimumFrameInterval = CMTimeMake(1, maxFramesPerSecond);
        configuration.showsCursor = NO;
        configuration.queueDepth = 5;
        configuration.colorSpaceName = kCGColorSpaceSRGB;

        m_handler = [[SidescopesStreamHandler alloc] init];
        m_handler.owner = this;
        m_stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:m_handler];
        m_queue = dispatch_queue_create("sidescopes.capture", DISPATCH_QUEUE_SERIAL);

        NSError* error = nil;
        if (![m_stream addStreamOutput:m_handler
                                  type:SCStreamOutputTypeScreen
                    sampleHandlerQueue:m_queue
                                 error:&error]) {
            return fail("adding the stream output failed");
        }

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block BOOL started = YES;
        [m_stream startCaptureWithCompletionHandler:^(NSError* startError) {
          started = startError == nil;
          dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (!started) {
            return fail("starting the capture stream failed");
        }

        m_running.store(true);
        return true;
    }

    void stop() override
    {
        if (!m_running.exchange(false)) {
            resetStreamState();
            return;
        }
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [m_stream stopCaptureWithCompletionHandler:^(NSError*) {
          dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
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
        if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            return;
        }

        const int width = static_cast<int>(CVPixelBufferGetWidth(image));
        const int height = static_cast<int>(CVPixelBufferGetHeight(image));
        const int sourceStride = static_cast<int>(CVPixelBufferGetBytesPerRow(image));
        const auto* source = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));

        m_buffer.width = width;
        m_buffer.height = height;
        m_buffer.strideBytes = width * 4;  // repacked tightly, surface padding dropped
        m_buffer.colorSpace = ColorSpaceHint::Srgb;
        m_buffer.sequence = ++m_sequence;
        m_buffer.data.resize(static_cast<std::size_t>(m_buffer.strideBytes) * height);
        for (int py = 0; py < height; ++py) {
            std::memcpy(m_buffer.data.data() + static_cast<std::size_t>(py) * m_buffer.strideBytes,
                        source + static_cast<std::size_t>(py) * sourceStride, static_cast<std::size_t>(width) * 4);
        }
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);

        m_buffer = m_mailbox->publish(std::move(m_buffer));
    }

    void handleStopped(NSError* error)
    {
        m_running.store(false);
        if (m_status) {
            m_status(error ? std::string("capture stopped: ") + error.localizedDescription.UTF8String
                           : "capture stopped");
        }
    }

private:
    // Drops the stream, its handler, and the mailbox link. Shared by both
    // stop() paths - the already-stopped early return and the normal
    // teardown after the stream has been asked to stop.
    void resetStreamState()
    {
        m_handler.owner = nullptr;
        m_stream = nil;
        m_handler = nil;
        m_mailbox = nullptr;
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
    FrameMailbox* m_mailbox = nullptr;
    FrameBuffer m_buffer;  // recycled storage, touched only on the capture queue
    StatusCallback m_status;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_sequence{0};
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

// The cursor readout away from the tracked display: a one-shot capture
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
    SCRunningApplication* selfApplication = nil;
    for (SCRunningApplication* application in g_samplerContent.applications) {
        if (application.processID == NSProcessInfo.processInfo.processIdentifier) {
            selfApplication = application;
            break;
        }
    }
    SCContentFilter* filter = (selfApplication && !captureExclusionDisabled())
                                  ? [[SCContentFilter alloc] initWithDisplay:display
                                                       excludingApplications:@[ selfApplication ]
                                                            exceptingWindows:@[]]
                                  : [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];

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
                                if (!image || error) {
                                    (*shared)(std::nullopt);
                                    return;
                                }
                                // Redrawing into a known-layout bitmap sidesteps whatever
                                // byte order the capture returned.
                                uint8_t pixels[Pixels * Pixels * 4] = {};
                                CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                                CGContextRef context =
                                    CGBitmapContextCreate(pixels, Pixels, Pixels, 8, Pixels * 4, srgb,
                                                          static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
                                                              static_cast<uint32_t>(kCGBitmapByteOrder32Big));
                                CGColorSpaceRelease(srgb);
                                if (!context) {
                                    (*shared)(std::nullopt);
                                    return;
                                }
                                CGContextDrawImage(context, CGRectMake(0, 0, Pixels, Pixels), image);
                                CGContextRelease(context);
                                double sumR = 0;
                                double sumG = 0;
                                double sumB = 0;
                                for (size_t index = 0; index < Pixels * Pixels; ++index) {
                                    sumR += pixels[index * 4 + 0];
                                    sumG += pixels[index * 4 + 1];
                                    sumB += pixels[index * 4 + 2];
                                }
                                constexpr double Count = static_cast<double>(Pixels * Pixels);
                                (*shared)(FloatColor{static_cast<float>(sumR / Count), static_cast<float>(sumG / Count),
                                                     static_cast<float>(sumB / Count)});
                              }];
}

}  // namespace sidescopes

@implementation SidescopesStreamHandler

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeScreen) {
        return;
    }
    if (auto* owner = self.owner) {
        owner->handleSample(sampleBuffer);
    }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    (void)stream;
    if (auto* owner = self.owner) {
        owner->handleStopped(error);
    }
}

@end
