#import <Foundation/Foundation.h>

#include <dlfcn.h>
#include <sys/mman.h>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>

#include "allocation_failure.h"
#include "core/frame_mailbox.h"
#include "core/hdr.h"
#include "core/scrgb.h"
#include "platform/desktop.h"
#include "platform/macos/capture_completion.h"
#include "platform/macos/capture_frame.h"
#include "platform/macos/system_observation.h"

namespace {
// PixelStorage bypasses operator new. This executable alone substitutes the
// page-allocation syscall, failing one tiny owned conversion on this thread.
std::atomic<bool> g_failImagePages{false};
std::atomic<int> g_imagePageFailures{0};
std::thread::id g_imageAllocationThread;
using Map = void* (*)(void*, size_t, int, int, int, off_t);

Map originalMap()
{
    static const auto original = reinterpret_cast<Map>(dlsym(RTLD_NEXT, "mmap"));
    if (!original) {
        std::abort();
    }
    return original;
}

CVPixelBufferRef g_countedPixelBuffer = nullptr;
int g_pixelLocks = 0;
int g_pixelUnlocks = 0;
using PixelLock = CVReturn (*)(CVPixelBufferRef, CVPixelBufferLockFlags);

PixelLock originalPixelLock()
{
    static const auto original = reinterpret_cast<PixelLock>(dlsym(RTLD_NEXT, "CVPixelBufferLockBaseAddress"));
    if (!original) {
        std::abort();
    }
    return original;
}

PixelLock originalPixelUnlock()
{
    static const auto original = reinterpret_cast<PixelLock>(dlsym(RTLD_NEXT, "CVPixelBufferUnlockBaseAddress"));
    if (!original) {
        std::abort();
    }
    return original;
}
}  // namespace

extern "C" void* mmap(void* address, size_t size, int protection, int flags, int descriptor, off_t offset)
{
    if (g_failImagePages.load() && std::this_thread::get_id() == g_imageAllocationThread && size == 4 &&
        (flags & MAP_ANONYMOUS) != 0) {
        g_failImagePages.store(false);
        ++g_imagePageFailures;
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return originalMap()(address, size, protection, flags, descriptor, offset);
}

extern "C" CVReturn CVPixelBufferLockBaseAddress(CVPixelBufferRef image, CVPixelBufferLockFlags flags)
{
    const auto result = originalPixelLock()(image, flags);
    if (image == g_countedPixelBuffer && result == kCVReturnSuccess) {
        ++g_pixelLocks;
    }
    return result;
}

extern "C" CVReturn CVPixelBufferUnlockBaseAddress(CVPixelBufferRef image, CVPixelBufferLockFlags flags)
{
    const auto result = originalPixelUnlock()(image, flags);
    if (image == g_countedPixelBuffer && result == kCVReturnSuccess) {
        ++g_pixelUnlocks;
    }
    return result;
}

// An inert Objective-C receiver: the production bridge sends the real SDK
// selectors, but these tests never create a capture stream or read a display.
@interface CompletionStream : NSObject {
@public
    std::shared_ptr<sidescopes::SckCallbackState> observedCallbacks;
}
@property(nonatomic, copy) void (^startReply)(NSError*);
@property(nonatomic, copy) void (^stopReply)(NSError*);
@property(nonatomic, assign) NSInteger startMode;
@property(nonatomic, assign) NSInteger stopMode;
@property(nonatomic, assign) NSInteger stops;
@property(nonatomic, assign) BOOL retiredBeforeStop;
@end

@implementation CompletionStream

- (void)startCaptureWithCompletionHandler:(void (^)(NSError*))reply
{
    self.startReply = reply;
    if (self.startMode == 3) {
        throw std::bad_alloc{};
    }
    if (self.startMode != 0) {
        reply(self.startMode == 1 ? nil : [NSError errorWithDomain:@"test" code:1 userInfo:nil]);
    }
}

- (void)stopCaptureWithCompletionHandler:(void (^)(NSError*))reply
{
    ++self.stops;
    self.retiredBeforeStop = !observedCallbacks || observedCallbacks->owner == nullptr;
    self.stopReply = reply;
    if (reply && self.stopMode != 0) {
        reply(self.stopMode == 1 ? nil : [NSError errorWithDomain:@"test" code:2 userInfo:nil]);
    }
}

@end

namespace sidescopes {
namespace {

using namespace std::chrono_literals;

SCStream* sdkStream(CompletionStream* stream)
{
    // Objective-C dispatch uses selectors, not the receiver's declared class.
    return (SCStream*)stream;  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
}

struct ImagePixels
{
    std::atomic<int>* releases;
    unsigned char bytes[4] = {10, 20, 30, 255};
};

CaptureImageOwner makeImage(std::atomic<int>& releases)
{
    auto pixels = std::make_unique<ImagePixels>();
    pixels->releases = &releases;
    CGDataProviderRef provider = CGDataProviderCreateWithData(pixels.get(), pixels->bytes, sizeof(pixels->bytes),
                                                              [](void* info, const void*, size_t) {
                                                                  auto* owned = static_cast<ImagePixels*>(info);
                                                                  ++*owned->releases;
                                                                  delete owned;
                                                              });
    if (!provider) {
        return {nullptr, CGImageRelease};
    }
    (void)pixels.release();  // NOLINT(bugprone-unused-return-value): the successful provider now owns these pixels
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = CGImageCreate(1, 1, 8, 32, 4, space,
                                     static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) |
                                         static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little),
                                     provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    return {image, CGImageRelease};
}

}  // namespace

TEST_CASE("Capture completion chooses one winner and moves its payload", "[native][completion]")
{
    CaptureCompletion<std::unique_ptr<int>> completed;
    REQUIRE(completed.complete(std::make_unique<int>(7)));
    CHECK_FALSE(completed.complete(std::make_unique<int>(9)));
    auto value = completed.wait(0ms);
    REQUIRE(value);
    CHECK(**value == 7);
    CHECK_FALSE(completed.abandoned());
    CHECK_FALSE(completed.wait(0ms));

    CaptureCompletion<std::unique_ptr<int>> expired;
    CHECK_FALSE(expired.wait(0ms));
    CHECK(expired.abandoned());
    CHECK_FALSE(expired.complete(std::make_unique<int>(4)));
    CHECK_FALSE(expired.wait(0ms));
}

TEST_CASE("Capture completion wakes its waiter without a timing race", "[native][completion]")
{
    const auto completion = std::make_shared<CaptureCompletion<bool>>();
    std::thread callback([completion] { (void)completion->complete(false); });
    const auto result = completion->wait(5s);
    callback.join();
    REQUIRE(result);
    CHECK_FALSE(*result);  // An explicit error is completed, not a timeout.
    CHECK_FALSE(completion->abandoned());
}

TEST_CASE("Content completion releases delivered and abandoned ARC objects", "[native][completion]")
{
    CaptureCompletion<NSObject*> failed;
    REQUIRE(failed.complete(nil));
    const auto error = failed.wait(0ms);
    REQUIRE(error);
    CHECK(*error == nil);  // An error completion does not wait for expiry.

    __weak NSObject* delivered;
    const auto completion = std::make_shared<CaptureCompletion<NSObject*>>();
    @autoreleasepool {
        NSObject* payload = [[NSObject alloc] init];
        delivered = payload;
        REQUIRE(completion->complete(payload));
    }
    REQUIRE(delivered != nil);
    @autoreleasepool {
        const auto result = completion->wait(0ms);
        REQUIRE(result);
        CHECK(*result == delivered);
    }
    CHECK(delivered == nil);  // The retained callback state no longer owns it.

    const auto expired = std::make_shared<CaptureCompletion<NSObject*>>();
    REQUIRE_FALSE(expired->wait(0ms));
    __weak NSObject* late;
    @autoreleasepool {
        NSObject* payload = [[NSObject alloc] init];
        late = payload;
        CHECK_FALSE(expired->complete(payload));
    }
    CHECK(late == nil);
}

TEST_CASE("Snapshot completion releases images after timeout and conversion", "[native][completion]")
{
    CaptureCompletion<CaptureImageOwner> failed;
    REQUIRE(failed.complete(CaptureImageOwner(nullptr, CGImageRelease)));
    auto error = failed.wait(0ms);
    REQUIRE(error);
    CHECK_FALSE(*error);

    std::atomic<int> releases{0};
    auto completion = std::make_shared<CaptureCompletion<CaptureImageOwner>>();
    REQUIRE_FALSE(completion->wait(0ms));
    auto image = makeImage(releases);
    REQUIRE(image);
    CHECK_FALSE(completion->complete(CaptureImageOwner(CGImageRetain(image.get()), CGImageRelease)));
    image.reset();
    CHECK(releases == 1);

    auto delivered = std::make_shared<CaptureCompletion<CaptureImageOwner>>();
    REQUIRE(delivered->complete(makeImage(releases)));
    {
        auto result = delivered->wait(0ms);
        REQUIRE(result);
        const auto pixels = convertCaptureImage(result->get());
        REQUIRE(pixels);
        CHECK(pixels->bgra.size() == 4);
    }
    CHECK(releases == 2);
}

TEST_CASE("Snapshot conversion allocation failure returns an empty image", "[native][completion][allocation]")
{
    std::atomic<int> releases{0};
    auto image = makeImage(releases);
    REQUIRE(image);
    (void)originalMap();  // Resolve forwarding before the fault is armed.
    g_imageAllocationThread = std::this_thread::get_id();
    g_imagePageFailures.store(0);
    g_failImagePages.store(true);
    const auto result = convertCaptureImage(image.get());
    g_failImagePages.store(false);
    CHECK(g_imagePageFailures == 1);
    CHECK_FALSE(result);
    CHECK(convertCaptureImage(image.get()).has_value());
    image.reset();
    CHECK(releases == 1);
}

TEST_CASE("Streaming allocation failure unlocks without publication and recovers", "[native][allocation]")
{
    unsigned char pixels[4] = {11, 22, 33, 255};
    CVPixelBufferRef raw = nullptr;
    REQUIRE(CVPixelBufferCreateWithBytes(kCFAllocatorDefault, 1, 1, kCVPixelFormatType_32BGRA, pixels, 4, nullptr,
                                         nullptr, nullptr, &raw) == kCVReturnSuccess);
    const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)> image(
        raw, CVPixelBufferRelease);
    FrameBuffer buffer;
    buffer.width = 1;
    buffer.height = 1;
    buffer.strideBytes = 4;
    buffer.format = PixelFormat::Bgra8;
    FrameMailbox mailbox;
    (void)originalMap();
    (void)originalPixelLock();
    (void)originalPixelUnlock();
    g_countedPixelBuffer = image.get();
    g_pixelLocks = g_pixelUnlocks = 0;
    g_imageAllocationThread = std::this_thread::get_id();
    g_imagePageFailures.store(0);
    g_failImagePages.store(true);
    const bool failedDelivery = deliverCapturePixels(image.get(), buffer, mailbox);
    g_failImagePages.store(false);
    g_countedPixelBuffer = nullptr;
    CHECK_FALSE(failedDelivery);
    CHECK(g_imagePageFailures == 1);
    CHECK(g_pixelLocks == 1);
    CHECK(g_pixelUnlocks == 1);
    CHECK_FALSE(mailbox.takeLatest(0ms));

    g_countedPixelBuffer = image.get();
    g_pixelLocks = g_pixelUnlocks = 0;
    const bool recovered = deliverCapturePixels(image.get(), buffer, mailbox);
    g_countedPixelBuffer = nullptr;
    REQUIRE(recovered);
    CHECK(g_pixelLocks == 1);
    CHECK(g_pixelUnlocks == 1);
    const auto delivered = mailbox.takeLatest(0ms);
    REQUIRE(delivered);
    REQUIRE(delivered->data.size() == 4);
    for (std::size_t index = 0; index < 4; ++index) {
        CHECK(delivered->data[index] == pixels[index]);
    }
}

TEST_CASE("HDR capture validates PQ metadata and converts padded native rows", "[native][hdr]")
{
    uint16_t pixels[24] = {0x3C00, 0x3C00, 0x3C00, 0x3C00};
    pixels[16] = pixels[17] = pixels[18] = pixels[19] = 0x3C00;
    CVPixelBufferRef raw = nullptr;
    REQUIRE(CVPixelBufferCreateWithBytes(kCFAllocatorDefault, 2, 2, kCVPixelFormatType_64RGBAHalf, pixels, 24, nullptr,
                                         nullptr, nullptr, &raw) == kCVReturnSuccess);
    const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)> image(
        raw, CVPixelBufferRelease);
    CHECK_FALSE(capturePixelFormat(image.get(), true));
    CVBufferSetAttachment(image.get(), kCVImageBufferCGColorSpaceKey, CFSTR("invalid"),
                          kCVAttachmentMode_ShouldPropagate);
    CHECK_FALSE(capturePixelFormat(image.get(), true));
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3_PQ);
    CVBufferSetAttachment(image.get(), kCVImageBufferCGColorSpaceKey, space, kCVAttachmentMode_ShouldPropagate);
    CGColorSpaceRelease(space);
    CHECK(capturePixelFormat(image.get(), true) == PixelFormat::Argb2101010);
    CHECK_FALSE(capturePixelFormat(image.get(), false));
    CVBufferRemoveAttachment(image.get(), kCVImageBufferCGColorSpaceKey);
    CVBufferSetAttachment(image.get(), kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_P3_D65,
                          kCVAttachmentMode_ShouldPropagate);
    CHECK_FALSE(capturePixelFormat(image.get(), true));
    CVBufferSetAttachment(image.get(), kCVImageBufferTransferFunctionKey,
                          kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ, kCVAttachmentMode_ShouldPropagate);
    CHECK(capturePixelFormat(image.get(), true) == PixelFormat::Argb2101010);
    CVBufferSetAttachment(image.get(), kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_sRGB,
                          kCVAttachmentMode_ShouldPropagate);
    CHECK_FALSE(capturePixelFormat(image.get(), true));
    CVBufferSetAttachment(image.get(), kCVImageBufferTransferFunctionKey,
                          kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ, kCVAttachmentMode_ShouldPropagate);
    FrameBuffer buffer;
    buffer.width = buffer.height = 2;
    buffer.strideBytes = 8;
    buffer.format = PixelFormat::Argb2101010;
    FrameMailbox mailbox;
    CHECK_FALSE(deliverCapturePixels(image.get(), buffer, mailbox));
    PqCaptureDecoder decoder;
    REQUIRE(deliverCapturePixels(image.get(), buffer, mailbox, &decoder));
    const auto delivered = mailbox.takeLatest(0ms);
    REQUIRE(delivered);
    REQUIRE(delivered->hdrLuminance.size() == 4);
    REQUIRE(delivered->hdrLinear.size() == 12);
    CHECK(floatFromHalf(delivered->hdrLinear[0]) > 99.9f);
    CHECK(floatFromHalf(delivered->hdrLinear[3]) == 0.0f);
    CHECK(delivered->hdrLuminance[0] > 99.9f);
    CHECK(delivered->hdrLuminance[1] == 0.0f);
    CHECK(delivered->hdrLuminance[2] == 0.0f);
    CHECK(delivered->hdrLuminance[3] > 99.9f);
    CHECK(delivered->hdrWhiteNits == 100.0);
    CHECK(delivered->view().sampleAt(0, 0).r == 1023);
    CHECK(delivered->view().sampleAt(1, 1).r == 1023);
}

TEST_CASE("Start completion distinguishes success error timeout and duplicate", "[native][completion]")
{
    @autoreleasepool {
        for (const NSInteger mode : {1, 2, 0}) {
            CompletionStream* stream = [[CompletionStream alloc] init];
            stream.startMode = mode;
            const auto callbacks = std::make_shared<SckCallbackState>();
            int owner = 0;
            callbacks->owner = reinterpret_cast<SckScreenCaptureSource*>(&owner);
            stream->observedCallbacks = callbacks;
            const auto result = startCaptureWithDeadline(sdkStream(stream), callbacks, 0ms);
            if (mode == 1) {
                CHECK(result == CaptureStartResult::Started);
                stream.startReply(nil);  // Duplicate success must not cancel a healthy stream.
                CHECK(stream.stops == 0);
                CHECK(callbacks->owner != nullptr);
            } else {
                CHECK(result == (mode == 2 ? CaptureStartResult::Failed : CaptureStartResult::TimedOut));
                CHECK(stream.stops == 1);
                CHECK(stream.retiredBeforeStop);
                CHECK(callbacks->owner == nullptr);
            }
        }
    }
}

TEST_CASE("Timed out start cancels late success without retaining the stream", "[native][completion]")
{
    __weak CompletionStream* weakStream;
    void (^lateReply)(NSError*) = nil;
    @autoreleasepool {
        CompletionStream* stream = [[CompletionStream alloc] init];
        weakStream = stream;
        const auto callbacks = std::make_shared<SckCallbackState>();
        REQUIRE(startCaptureWithDeadline(sdkStream(stream), callbacks, 0ms) == CaptureStartResult::TimedOut);
        CHECK(stream.stops == 1);
        lateReply = stream.startReply;
        lateReply([NSError errorWithDomain:@"test" code:1 userInfo:nil]);
        CHECK(stream.stops == 1);  // Late error needs no second cancellation.
        lateReply(nil);
        CHECK(stream.stops == 2);
    }
    CHECK(weakStream == nil);  // No stream -> completion -> stream cycle.
    lateReply(nil);            // A dead stream is not resurrected or dereferenced.
}

TEST_CASE("Retired start callbacks cannot affect a replacement operation", "[native][completion]")
{
    @autoreleasepool {
        CompletionStream* old = [[CompletionStream alloc] init];
        REQUIRE(startCaptureWithDeadline(sdkStream(old), std::make_shared<SckCallbackState>(), 0ms) ==
                CaptureStartResult::TimedOut);
        CompletionStream* replacement = [[CompletionStream alloc] init];
        replacement.startMode = 1;
        REQUIRE(startCaptureWithDeadline(sdkStream(replacement), std::make_shared<SckCallbackState>(), 0ms) ==
                CaptureStartResult::Started);
        old.startReply(nil);
        CHECK(old.stops == 2);
        CHECK(replacement.stops == 0);
    }
}

TEST_CASE("A failed submission abandons a callback already retained by the stream", "[native][completion]")
{
    @autoreleasepool {
        CompletionStream* stream = [[CompletionStream alloc] init];
        stream.startMode = 3;
        const auto callbacks = std::make_shared<SckCallbackState>();
        REQUIRE(startCaptureWithDeadline(sdkStream(stream), callbacks, 0ms) == CaptureStartResult::Failed);
        CHECK(stream.stops == 1);
        stream.startReply(nil);
        CHECK(stream.stops == 2);
    }
}

TEST_CASE("Stop completion handles missing error and late replies", "[native][completion]")
{
    @autoreleasepool {
        for (const NSInteger mode : {1, 2, 0}) {
            CompletionStream* stream = [[CompletionStream alloc] init];
            stream.stopMode = mode;
            CHECK(stopCaptureWithDeadline(sdkStream(stream), 0ms) == (mode == 1));
            CHECK(stream.stops == 1);
            stream.stopReply(nil);  // Safe after return for every outcome.
        }
    }
}

TEST_CASE("Completion state allocation failure still cancels start and stop", "[native][completion][allocation]")
{
    @autoreleasepool {
        CompletionStream* stream = [[CompletionStream alloc] init];
        const auto callbacks = std::make_shared<SckCallbackState>();
        test::AllocationFailure startFailure(0);
        const auto result = startCaptureWithDeadline(sdkStream(stream), callbacks, 0ms);
        startFailure.disarm();
        CHECK(startFailure.failures() == 1);
        CHECK(result == CaptureStartResult::Failed);
        CHECK(stream.stops == 1);
        test::AllocationFailure stopFailure(0);
        const bool stopped = stopCaptureWithDeadline(sdkStream(stream), 0ms);
        stopFailure.disarm();
        CHECK(stopFailure.failures() == 1);
        CHECK_FALSE(stopped);
        CHECK(stream.stops == 2);
    }
}

TEST_CASE("Observer registration failure leaves no unowned callback", "[native][allocation]")
{
    @autoreleasepool {
        unobserveSystemEvents();
        NSNotificationCenter* center = [[NSNotificationCenter alloc] init];
        NSNotificationName name = @"SideScopesObservationTest";
        // Warm Foundation without registering a desktop event or changing the
        // production token list. Only its first vector allocation is failed.
        id token = [center addObserverForName:name
                                       object:nil
                                        queue:[NSOperationQueue mainQueue]
                                   usingBlock:^(NSNotification*){
                                   }];
        [center removeObserver:token];
        auto calls = std::make_shared<int>(0);
        const std::weak_ptr<int> retained = calls;
        auto callback = std::make_shared<std::function<void()>>([calls] { ++*calls; });
        bool threw = false;
        test::AllocationFailure failure(0);
        try {
            observeSystemNotification(center, name, callback);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        failure.disarm();
        unobserveSystemEvents();
        [center postNotificationName:name object:nil];
        CHECK(threw);
        CHECK(failure.failures() == 1);
        CHECK(*calls == 0);

        observeSystemNotification(center, name, callback);
        [center postNotificationName:name object:nil];
        CHECK(*calls == 1);
        unobserveSystemEvents();
        [center postNotificationName:name object:nil];
        CHECK(*calls == 1);
        callback.reset();
        calls.reset();
        CHECK(retained.expired());
    }
}

}  // namespace sidescopes
