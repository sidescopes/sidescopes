#import <AppKit/AppKit.h>

#include <cctype>
#include <string>

#include "platform/native_menu.h"

@interface SidescopesMenuTarget : NSObject
@property(nonatomic, assign) int picked;
- (void)pick:(NSMenuItem*)sender;
@end

@implementation SidescopesMenuTarget
- (void)pick:(NSMenuItem*)sender {
    self.picked = static_cast<int>(sender.tag);
}
@end

namespace sidescopes {

int ShowNativeContextMenu(const std::vector<NativeMenuItem>& items) {
    SidescopesMenuTarget* target = [[SidescopesMenuTarget alloc] init];
    target.picked = -1;

    NSMenu* root = [[NSMenu alloc] initWithTitle:@""];
    root.autoenablesItems = NO;
    NSMutableArray<NSMenu*>* stack = [NSMutableArray arrayWithObject:root];

    for (const NativeMenuItem& item : items) {
        NSMenu* current = stack.lastObject;
        switch (item.kind) {
            case NativeMenuItem::Kind::Separator:
                [current addItem:[NSMenuItem separatorItem]];
                break;
            case NativeMenuItem::Kind::SubmenuBegin: {
                NSMenuItem* holder = [[NSMenuItem alloc]
                    initWithTitle:[NSString stringWithUTF8String:item.label.c_str()]
                           action:nil
                    keyEquivalent:@""];
                NSMenu* submenu = [[NSMenu alloc] initWithTitle:holder.title];
                submenu.autoenablesItems = NO;
                holder.submenu = submenu;
                [current addItem:holder];
                [stack addObject:submenu];
                break;
            }
            case NativeMenuItem::Kind::SubmenuEnd:
                if (stack.count > 1) [stack removeLastObject];
                break;
            case NativeMenuItem::Kind::Action: {
                NSMenuItem* entry = [[NSMenuItem alloc]
                    initWithTitle:[NSString stringWithUTF8String:item.label.c_str()]
                           action:@selector(pick:)
                    keyEquivalent:@""];
                entry.target = target;
                entry.tag = item.action_id;
                entry.state = item.checked ? NSControlStateValueOn : NSControlStateValueOff;
                if (!item.shortcut.empty()) {
                    // The key equivalent renders right-aligned the way
                    // every macOS menu draws shortcuts. A zeroed modifier
                    // mask shows the bare key; the application's own key
                    // handling stays the single source of behavior.
                    std::string key = item.shortcut;
                    NSEventModifierFlags modifiers = 0;
                    if (key.rfind("Shift+", 0) == 0) {
                        modifiers = NSEventModifierFlagShift;
                        key = key.substr(6);
                    }
                    if (key == "Esc") {
                        entry.keyEquivalent = @"\x1B";
                    } else if (key.size() == 1) {
                        const char lower = static_cast<char>(std::tolower(key[0]));
                        entry.keyEquivalent = [NSString stringWithFormat:@"%c", lower];
                    }
                    entry.keyEquivalentModifierMask = modifiers;
                }
                [current addItem:entry];
                break;
            }
        }
    }

    [root popUpMenuPositioningItem:nil atLocation:[NSEvent mouseLocation] inView:nil];
    return target.picked;
}

}  // namespace sidescopes
