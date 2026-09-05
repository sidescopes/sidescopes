#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace sidescopes {

/// Takes @p bytes of whole pages from the operating system, or throws
/// std::bad_alloc. Defined in heap.cpp so the platform headers stay out of
/// every translation unit that carries a frame.
[[nodiscard]] void* allocatePages(std::size_t bytes);

/// Returns pages taken by allocatePages. @p bytes must be what was asked for.
void freePages(void* memory, std::size_t bytes) noexcept;

/// A file mapped into the address space read-only.
struct MappedFile
{
    const unsigned char* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] bool valid() const
    {
        return data != nullptr && size > 0;
    }
};

/// Maps @p path read-only, or returns an invalid mapping if it cannot be read.
///
/// The pages are clean and file-backed: they are shared with every other
/// process mapping the same file, the system can evict them under pressure
/// without writing anything, and they are NOT charged to phys_footprint the
/// way a heap copy of the same bytes is. That is the whole reason this exists
/// - the interface font is 4.27 MB the process would otherwise hold as dirty
/// private memory for its entire life.
///
/// READ-ONLY IS LOAD-BEARING, not defensive: a write to the mapping is a fault
/// rather than a slow path, which is exactly what makes it safe to hand these
/// bytes to a library that promises not to modify them. Defined in heap.cpp so
/// the platform headers stay out of every translation unit.
[[nodiscard]] MappedFile mapFileReadOnly(const char* path);

/// Releases a mapping taken by mapFileReadOnly. Safe on an invalid mapping.
void unmapFile(MappedFile mapping) noexcept;

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
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
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
