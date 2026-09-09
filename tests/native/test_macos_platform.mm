#import <AppKit/AppKit.h>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>

#include "platform/desktop.h"
#include "platform/macos/region_border_view.h"
#include "platform/macos/region_picker_view.h"
#include "platform/region_geometry.h"

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

NSEvent* mouseEvent(NSEventType type, NSPoint point)
{
    return [NSEvent mouseEventWithType:type
                              location:point
                         modifierFlags:0
                             timestamp:0
                          windowNumber:0
                               context:nil
                           eventNumber:0
                            clickCount:1
                              pressure:0];
}

class BorderEventsScope
{
public:
    ~BorderEventsScope()
    {
        g_borderEditing = m_editing;
        g_borderEditChanged = m_changed;
        g_borderClosed = m_closed;
        g_borderBindingToggled = m_binding;
        [m_cursor set];
    }

private:
    bool m_editing = std::exchange(g_borderEditing, false);
    bool m_changed = std::exchange(g_borderEditChanged, false);
    bool m_closed = std::exchange(g_borderClosed, false);
    bool m_binding = std::exchange(g_borderBindingToggled, false);
    NSCursor* m_cursor = NSCursor.currentCursor;
};

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

        [view keyDown:keyEvent(@"\x1b", 0, 53)];
        presses = drainBorderKeyPresses();
        REQUIRE(presses.size() == 1);
        CHECK(presses.front().escape);
        CHECK(presses.front().key.empty());
        CHECK_FALSE(presses.front().shift);
        CHECK(drainBorderKeyPresses().empty());
        CHECK(receiver.received == 3);
    }
}

TEST_CASE("A border close requires a matching release and is consumed once", "[native][border-close]")
{
    @autoreleasepool {
        const BorderEventsScope events;
        SidescopesBorderView* view =
            [[SidescopesBorderView alloc] initWithFrame:NSMakeRect(0, 0, 200 + 2 * WindowPad, 100 + 2 * WindowPad)];
        REQUIRE(view.window == nil);
        const NSPoint close = NSMakePoint(WindowPad + 200 + BorderPad - CloseCornerInset,
                                          WindowPad + 100 + BorderPad - CloseCornerInset + EdgeRing);
        [view mouseDown:mouseEvent(NSEventTypeLeftMouseDown, close)];
        REQUIRE(view.closePressed);
        CHECK_FALSE(pollRegionBorderEdit().closed);

        SECTION("Releasing inside closes exactly once")
        {
            [view mouseUp:mouseEvent(NSEventTypeLeftMouseUp, close)];
            CHECK(pollRegionBorderEdit().closed);
        }
        SECTION("Releasing outside cancels the press")
        {
            [view mouseUp:mouseEvent(NSEventTypeLeftMouseUp, NSMakePoint(WindowPad + 100, WindowPad + 50))];
            CHECK_FALSE(pollRegionBorderEdit().closed);
        }
        SECTION("Hiding discards an already queued close")
        {
            [view mouseUp:mouseEvent(NSEventTypeLeftMouseUp, close)];
            REQUIRE(g_borderClosed);
            // This checks the shared event latch. The windowless view is not
            // the private window owned and reset by hideRegionBorder.
            hideRegionBorder();
            CHECK_FALSE(pollRegionBorderEdit().closed);
        }
        CHECK_FALSE(view.closePressed);
        CHECK_FALSE(pollRegionBorderEdit().closed);
        [view mouseUp:mouseEvent(NSEventTypeLeftMouseUp, close)];
        CHECK_FALSE(pollRegionBorderEdit().closed);
        CHECK_FALSE(g_borderBindingToggled);
        CHECK_FALSE(g_borderEditing);
        CHECK(view.window == nil);
    }
}

TEST_CASE("Border corner presses keep resizing narrow regions", "[native][border-close]")
{
    @autoreleasepool {
        const BorderEventsScope events;
        for (const double width : {MinimumRegionSize, MinimumRegionWidthForClose}) {
            for (const double labelBand : {0.0, LabelBand}) {
                CAPTURE(width, labelBand);
                SidescopesBorderView* view = [[SidescopesBorderView alloc]
                    initWithFrame:NSMakeRect(0, 0, width + 2 * WindowPad, 100 + 2 * WindowPad + labelBand)];
                view.labelBand = labelBand;
                REQUIRE(view.window == nil);
                const NSPoint corner = NSMakePoint(WindowPad + width + 1, WindowPad + 100 + 1);
                [view mouseDown:mouseEvent(NSEventTypeLeftMouseDown, corner)];
                CHECK(view.dragZone == (ZoneTop | ZoneRight));
                CHECK(g_borderEditing);
                CHECK_FALSE(view.closePressed);
                [view mouseUp:mouseEvent(NSEventTypeLeftMouseUp, corner)];
                CHECK(view.dragZone == ZoneNone);
                CHECK_FALSE(g_borderEditing);
                CHECK_FALSE(pollRegionBorderEdit().closed);
            }
        }
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
