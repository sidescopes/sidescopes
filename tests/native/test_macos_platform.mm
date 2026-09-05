#import <AppKit/AppKit.h>

#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "platform/desktop.h"
#include "platform/macos/region_border_view.h"
#include "platform/macos/region_picker_view.h"

@interface KeyReceiver : NSResponder
@property(nonatomic, assign) int received;
@end

@implementation KeyReceiver

- (void)keyDown:(NSEvent*)event
{
    (void)event;
    ++self.received;
}

@end

namespace sidescopes {
namespace {

NSEvent* keyEvent(NSString* key, NSEventModifierFlags modifiers, unsigned short keyCode)
{
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:modifiers
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:key
         charactersIgnoringModifiers:key
                           isARepeat:NO
                             keyCode:keyCode];
}

class SystemEventsScope
{
public:
    ~SystemEventsScope()
    {
        unobserveSystemEvents();
        unobserveForegroundChanges();
    }
};

}  // namespace

TEST_CASE("System observations release callbacks at shutdown", "[native]")
{
    std::weak_ptr<int> retained;
    @autoreleasepool {
        const SystemEventsScope observations;
        auto count = std::make_shared<int>(0);
        retained = count;
        observeSystemWake([count] { ++*count; });
        observeSystemSleep([count] { ++*count; });
        observeEscapeWithoutKeyWindow([count] { ++*count; });
        observeForegroundChanges([count] { ++*count; });
        NSNotificationCenter* workspace = [[NSWorkspace sharedWorkspace] notificationCenter];
        [workspace postNotificationName:NSWorkspaceScreensDidWakeNotification object:nil];
        [workspace postNotificationName:NSWorkspaceScreensDidSleepNotification object:nil];
        [workspace postNotificationName:NSWorkspaceDidActivateApplicationNotification object:nil];
        CHECK(*count == 3);

        unobserveSystemEvents();
        unobserveForegroundChanges();
        [workspace postNotificationName:NSWorkspaceScreensDidWakeNotification object:nil];
        [workspace postNotificationName:NSWorkspaceScreensDidSleepNotification object:nil];
        [workspace postNotificationName:NSWorkspaceDidActivateApplicationNotification object:nil];
        CHECK(*count == 3);
        count.reset();
    }
    CHECK(retained.expired());
}

TEST_CASE("A border forwards plain and shifted keys without consuming chords", "[native]")
{
    @autoreleasepool {
        (void)drainBorderKeyPresses();
        SidescopesBorderView* view = [[SidescopesBorderView alloc] initWithFrame:NSMakeRect(0, 0, 200, 150)];
        KeyReceiver* receiver = [[KeyReceiver alloc] init];
        view.nextResponder = receiver;

        [view keyDown:keyEvent(@"v", 0, 9)];
        [view keyDown:keyEvent(@"w", NSEventModifierFlagShift, 13)];
        auto presses = drainBorderKeyPresses();
        REQUIRE(presses.size() == 2);
        CHECK(presses[0].key == "V");
        CHECK_FALSE(presses[0].shift);
        CHECK(presses[1].key == "W");
        CHECK(presses[1].shift);

        for (const auto modifier :
             {NSEventModifierFlagCommand, NSEventModifierFlagControl, NSEventModifierFlagOption}) {
            [view keyDown:keyEvent(@"v", modifier, 9)];
        }
        CHECK(drainBorderKeyPresses().empty());
        CHECK(receiver.received == 3);
    }
}

TEST_CASE("Picker mode chords reach the responder chain", "[native]")
{
    @autoreleasepool {
        SidescopesPickerView* view = [[SidescopesPickerView alloc] initWithFrame:NSMakeRect(0, 0, 200, 150)];
        KeyReceiver* receiver = [[KeyReceiver alloc] init];
        view.nextResponder = receiver;

        for (const auto modifier :
             {NSEventModifierFlagCommand, NSEventModifierFlagControl, NSEventModifierFlagOption}) {
            [view keyDown:keyEvent(@"d", modifier, 2)];
        }
        CHECK(receiver.received == 3);
        CHECK_FALSE(view.finished);
        [view keyDown:keyEvent(@"\x1b", 0, 53)];
        CHECK(view.finished);
        CHECK_FALSE(view.picked);
    }
}

}  // namespace sidescopes
