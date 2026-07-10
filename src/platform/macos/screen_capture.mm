// ScreenCaptureKit backend.
//
// Hard-won specifics encoded here:
//  - The content filter excludes NOTHING. Every SCContentFilter exclusion
//    variant snapshots its list at creation time, so windows or applications
//    appearing later are silently absent from the capture (Quick Look
//    previews are hosted by an on-demand service process, for example).
//    Self-capture feedback is handled by the analysis-side window mask.
//  - Streams die when the display configuration blinks (lock screen,
//    display sleep). Death is reported through the status callback and the
//    application restarts capture; this backend never retries on its own.
//  - Frames are requested in sRGB, so the OS converts from the display's
//    color space and the scopes read honest sRGB values.

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <cstring>
#include <string>

#include "platform/screen_capture.h"

namespace sidescopes {
class SckScreenCaptureSource;
}

@interface SidescopesStreamHandler : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) sidescopes::SckScreenCaptureSource* owner;
@end

namespace sidescopes {
namespace {

SCShareableContent* FetchShareableContent() {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block SCShareableContent* result = nil;
    [SCShareableContent
        getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
          (void)error;
          result = content;
          dispatch_semaphore_signal(done);
        }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return result;
}

}  // namespace

class SckScreenCaptureSource final : public ScreenCaptureSource {
public:
    ~SckScreenCaptureSource() override { Stop(); }

    CapturePermission RequestPermission() override {
        if (CGPreflightScreenCaptureAccess()) return CapturePermission::Granted;
        return CGRequestScreenCaptureAccess() ? CapturePermission::Granted
                                              : CapturePermission::Denied;
    }

    std::vector<CaptureTarget> ListTargets() override {
        std::vector<CaptureTarget> targets;
        SCShareableContent* content = FetchShareableContent();
        if (!content) return targets;
        for (SCDisplay* display in content.displays) {
            CaptureTarget target;
            target.identifier = std::to_string(display.displayID);
            target.width_points = static_cast<int>(display.width);
            target.height_points = static_cast<int>(display.height);
            target.description = "Display " + target.identifier + " (" +
                                 std::to_string(target.width_points) + "x" +
                                 std::to_string(target.height_points) + ")";
            targets.push_back(std::move(target));
        }
        return targets;
    }

    bool Start(const CaptureTarget& target, int max_frames_per_second,
               FrameMailbox& mailbox) override {
        Stop();
        mailbox_ = &mailbox;

        SCShareableContent* content = FetchShareableContent();
        if (!content) return Fail("shareable content unavailable (permission?)");
        SCDisplay* display = nil;
        for (SCDisplay* candidate in content.displays) {
            if (std::to_string(candidate.displayID) == target.identifier) display = candidate;
        }
        if (!display) return Fail("capture target no longer present");

        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                                          excludingWindows:@[]];

        SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
        const CGFloat scale = filter.pointPixelScale > 0 ? filter.pointPixelScale : 2.0;
        configuration.width = static_cast<size_t>(display.width * scale);
        configuration.height = static_cast<size_t>(display.height * scale);
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        configuration.minimumFrameInterval = CMTimeMake(1, max_frames_per_second);
        configuration.showsCursor = NO;
        configuration.queueDepth = 5;
        configuration.colorSpaceName = kCGColorSpaceSRGB;

        handler_ = [[SidescopesStreamHandler alloc] init];
        handler_.owner = this;
        stream_ = [[SCStream alloc] initWithFilter:filter
                                     configuration:configuration
                                          delegate:handler_];
        queue_ = dispatch_queue_create("sidescopes.capture", DISPATCH_QUEUE_SERIAL);

        NSError* error = nil;
        if (![stream_ addStreamOutput:handler_
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:queue_
                                error:&error]) {
            return Fail("adding the stream output failed");
        }

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block BOOL started = YES;
        [stream_ startCaptureWithCompletionHandler:^(NSError* start_error) {
          started = start_error == nil;
          dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (!started) return Fail("starting the capture stream failed");

        running_.store(true);
        return true;
    }

    void Stop() override {
        if (!running_.exchange(false)) {
            handler_.owner = nullptr;
            stream_ = nil;
            handler_ = nil;
            mailbox_ = nullptr;
            return;
        }
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [stream_ stopCaptureWithCompletionHandler:^(NSError*) {
          dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        handler_.owner = nullptr;
        stream_ = nil;
        handler_ = nil;
        mailbox_ = nullptr;
    }

    void SetStatusCallback(StatusCallback callback) override { status_ = std::move(callback); }

    // Called on the capture queue.
    void HandleSample(CMSampleBufferRef sample) {
        if (!running_.load() || mailbox_ == nullptr) return;

        // Only complete frames carry new content; idle deliveries are
        // dropped so a static screen produces no downstream work.
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
            NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
            NSNumber* status = info[SCStreamFrameInfoStatus];
            if (status && static_cast<SCFrameStatus>(status.intValue) != SCFrameStatusComplete)
                return;
        }

        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        if (!image) return;
        if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
            return;

        const int width = static_cast<int>(CVPixelBufferGetWidth(image));
        const int height = static_cast<int>(CVPixelBufferGetHeight(image));
        const int source_stride = static_cast<int>(CVPixelBufferGetBytesPerRow(image));
        const auto* source = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));

        buffer_.width = width;
        buffer_.height = height;
        buffer_.stride_bytes = width * 4;  // repacked tightly, surface padding dropped
        buffer_.color_space = ColorSpaceHint::Srgb;
        buffer_.sequence = ++sequence_;
        buffer_.data.resize(static_cast<std::size_t>(buffer_.stride_bytes) * height);
        for (int py = 0; py < height; ++py) {
            std::memcpy(buffer_.data.data() + static_cast<std::size_t>(py) * buffer_.stride_bytes,
                        source + static_cast<std::size_t>(py) * source_stride,
                        static_cast<std::size_t>(width) * 4);
        }
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);

        buffer_ = mailbox_->Publish(std::move(buffer_));
    }

    void HandleStopped(NSError* error) {
        running_.store(false);
        if (status_) {
            status_(error ? std::string("capture stopped: ") + error.localizedDescription.UTF8String
                          : "capture stopped");
        }
    }

private:
    bool Fail(const std::string& message) {
        if (status_) status_(message);
        return false;
    }

    SCStream* stream_ = nil;
    SidescopesStreamHandler* handler_ = nil;
    dispatch_queue_t queue_ = nil;
    FrameMailbox* mailbox_ = nullptr;
    FrameBuffer buffer_;  // recycled storage, touched only on the capture queue
    StatusCallback status_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> sequence_{0};
};

std::unique_ptr<ScreenCaptureSource> CreateScreenCaptureSource() {
    return std::make_unique<SckScreenCaptureSource>();
}

}  // namespace sidescopes

@implementation SidescopesStreamHandler

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type != SCStreamOutputTypeScreen) return;
    if (auto* owner = self.owner) owner->HandleSample(sampleBuffer);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    if (auto* owner = self.owner) owner->HandleStopped(error);
}

@end
