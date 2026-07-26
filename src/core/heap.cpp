#include "core/heap.h"

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(_WIN32) || defined(__linux__)
#include <malloc.h>
#endif

namespace sidescopes {

void releaseFreeHeap()
{
#if defined(__APPLE__)
    // Null asks every zone, which is what the system's own memory-pressure
    // handling does; the second argument is a goal of zero, meaning all of it.
    malloc_zone_pressure_relief(nullptr, 0);
#elif defined(_WIN32)
    (void)_heapmin();
#elif defined(__linux__)
    (void)malloc_trim(0);
#endif
}

}  // namespace sidescopes
