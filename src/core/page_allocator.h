#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sidescopes {

/// Takes @p bytes of whole pages from the operating system, or throws
/// std::bad_alloc. Defined in heap.cpp so the platform headers stay out of
/// every translation unit that carries a frame.
[[nodiscard]] void* allocatePages(std::size_t bytes);

/// Returns pages taken by allocatePages. @p bytes must be what was asked for.
void freePages(void* memory, std::size_t bytes) noexcept;

/// An allocator that takes whole pages from the operating system and gives
/// them straight back.
///
/// The system allocator does not: freeing a large block returns it to a cache
/// that keeps the pages resident for the next allocation of that size, which
/// is right in a steady state and wrong for the frame buffers. Those are the
/// largest thing this application holds - a whole display each, three of them
/// - and when capture pauses it will not want them again until it resumes.
/// Measured on macOS 26: three 14 MB buffers freed to the system allocator
/// leave the process 42 MB heavier for the rest of its life, and no pressure
/// call takes them back.
///
/// Only worth using for allocations far larger than a page; every one is
/// rounded up to one.
template <typename T>
struct PageAllocator
{
    using value_type = T;

    PageAllocator() = default;

    template <typename U>
    constexpr explicit PageAllocator(const PageAllocator<U>&) noexcept
    {
    }

    [[nodiscard]] T* allocate(std::size_t count)
    {
        return static_cast<T*>(allocatePages(count * sizeof(T)));
    }

    void deallocate(T* memory, std::size_t count) noexcept
    {
        freePages(memory, count * sizeof(T));
    }

    template <typename U>
    bool operator==(const PageAllocator<U>&) const noexcept
    {
        return true;
    }
};

/// Pixels taken from the operating system a page at a time, for the buffers
/// large enough to need it: a captured frame and a one-shot display scan.
using PixelStorage = std::vector<uint8_t, PageAllocator<uint8_t>>;

}  // namespace sidescopes
