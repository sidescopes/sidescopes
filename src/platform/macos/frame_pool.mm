#import <Foundation/Foundation.h>

#include "platform/frame_pool.h"

namespace sidescopes {

void runInFramePool(void (*body)(void* context), void* context)
{
    @autoreleasepool {
        body(context);
    }
}

}  // namespace sidescopes
