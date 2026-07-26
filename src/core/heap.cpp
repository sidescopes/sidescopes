#include "core/heap.h"

#include <new>

#include "core/page_allocator.h"

#if defined(_WIN32)
// Both are required before windows.h anywhere in this project: the macros it
// otherwise defines shadow std::min and std::max, and the full header pulls in
// far more than these two calls need.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <malloc.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#include <sys/mman.h>
#else
#include <malloc.h>
#include <sys/mman.h>
#endif

#if !defined(_WIN32) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
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
#else
    (void)malloc_trim(0);
#endif
}

void* allocatePages(std::size_t bytes)
{
#if defined(_WIN32)
    void* memory = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        memory = nullptr;
    }
#endif
    if (memory == nullptr) {
        throw std::bad_alloc();
    }

    return memory;
}

void freePages(void* memory, std::size_t bytes) noexcept
{
#if defined(_WIN32)
    (void)bytes;
    (void)VirtualFree(memory, 0, MEM_RELEASE);
#else
    (void)munmap(memory, bytes);
#endif
}

}  // namespace sidescopes
