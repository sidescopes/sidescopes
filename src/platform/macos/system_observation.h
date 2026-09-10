#pragma once

#import <Foundation/Foundation.h>

#include <functional>
#include <memory>

namespace sidescopes {

// Registers a main-queue notification whose callback and token are retired by
// unobserveSystemEvents. Kept separate from NSWorkspace so ownership can be
// checked with an ordinary notification center without a desktop session.
void observeSystemNotification(NSNotificationCenter* center, NSNotificationName name,
                               std::shared_ptr<std::function<void()>> callback);

}  // namespace sidescopes
