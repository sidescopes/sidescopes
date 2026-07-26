#include "platform/frame_pool.h"

namespace sidescopes {

void runInFramePool(void (*body)(void* context), void* context)
{
    body(context);
}

}  // namespace sidescopes
