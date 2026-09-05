#import <AppKit/AppKit.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "platform/desktop.h"
#include "platform/macos/region_picker_view.h"
#include "stress_config.h"

namespace sidescopes {
namespace {

struct SystemEventsScope
{
    ~SystemEventsScope()
    {
        unobserveSystemEvents();
        unobserveForegroundChanges();
    }
};

void postLifecycleNotifications()
{
    NSNotificationCenter* workspace = [[NSWorkspace sharedWorkspace] notificationCenter];
    [workspace postNotificationName:NSWorkspaceScreensDidWakeNotification object:nil];
    [workspace postNotificationName:NSWorkspaceSessionDidBecomeActiveNotification object:nil];
    [workspace postNotificationName:NSWorkspaceScreensDidSleepNotification object:nil];
    [workspace postNotificationName:NSWorkspaceSessionDidResignActiveNotification object:nil];
    [workspace postNotificationName:NSWorkspaceDidActivateApplicationNotification object:nil];
    [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidChangeScreenParametersNotification
                                                        object:nil];
}

}  // namespace

TEST_CASE("Native observer and offscreen picker lifetimes survive repeated teardown", "[stress][native]")
{
    const uint32_t cycles = test::stressSetting("SIDESCOPES_STRESS_CYCLES", 512, 100000);
    const SystemEventsScope observations;
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        CAPTURE(cycle);
        std::weak_ptr<uint32_t> retained;
        std::weak_ptr<uint32_t> replaced;
        @autoreleasepool {
            auto previous = std::make_shared<uint32_t>(0);
            replaced = previous;
            observeForegroundChanges([previous] { ++*previous; });
            observeEscapeWithoutKeyWindow([previous] { ++*previous; });

            auto events = std::make_shared<uint32_t>(0);
            retained = events;
            observeSystemWake([events] { ++*events; });
            observeSystemSleep([events] { ++*events; });
            observeForegroundChanges([events] { ++*events; });
            observeEscapeWithoutKeyWindow([events] { ++*events; });
            previous.reset();
            postLifecycleNotifications();
            CHECK(*events == 6);

            SidescopesPickerView* view = [[SidescopesPickerView alloc] initWithFrame:NSMakeRect(0, 0, 200, 150)];
            NSEvent* escape = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                               location:NSZeroPoint
                                          modifierFlags:0
                                              timestamp:0
                                           windowNumber:0
                                                context:nil
                                             characters:@"\x1b"
                            charactersIgnoringModifiers:@"\x1b"
                                              isARepeat:NO
                                                keyCode:53];
            [view keyDown:escape];
            CHECK(view.finished);
            CHECK_FALSE(view.picked);
            unobserveSystemEvents();
            unobserveForegroundChanges();
            postLifecycleNotifications();
            CHECK(*events == 6);
            events.reset();
        }
        CHECK(retained.expired());
        CHECK(replaced.expired());
    }
    std::fprintf(stderr, "native_lifecycle cycles=%u synthetic_notifications=%u offscreen_picker_cancels=%u\n", cycles,
                 cycles * 12, cycles);
}

TEST_CASE("Queued native notifications lose their callback before teardown returns", "[stress][native]")
{
    using namespace std::chrono_literals;
    const uint32_t cycles = test::stressSetting("SIDESCOPES_STRESS_CYCLES", 512, 100000);
    const SystemEventsScope observations;
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        CAPTURE(cycle);
        std::weak_ptr<uint32_t> retained;
        @autoreleasepool {
            auto calls = std::make_shared<uint32_t>(0);
            retained = calls;
            observeSystemWake([calls] { ++*calls; });
            NSNotificationCenter* center = [[NSWorkspace sharedWorkspace] notificationCenter];
            std::atomic<bool> posted{false};
            std::thread poster([center, &posted] {
                @autoreleasepool {
                    [center postNotificationName:NSWorkspaceScreensDidWakeNotification object:nil];
                }
                posted.store(true);
            });
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while ([NSOperationQueue mainQueue].operationCount == 0 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            const bool queued = [NSOperationQueue mainQueue].operationCount > 0;
            unobserveSystemEvents();
            while (!posted.load()) {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
            }
            poster.join();
            CHECK(queued);
            CHECK(*calls == 0);
            calls.reset();
        }
        CHECK(retained.expired());
    }
    std::fprintf(stderr, "native_queued_teardown cycles=%u\n", cycles);
}

}  // namespace sidescopes
