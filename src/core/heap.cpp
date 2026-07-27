#include "core/heap.h"

#include <new>
#include <string>

#include "core/page_allocator.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

MappedFile mapFileReadOnly(const char* path)
{
    if (path == nullptr) {
        return MappedFile{};
    }
#if defined(_WIN32)
    const int wide = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wide <= 0) {
        return MappedFile{};
    }
    std::wstring widePath(static_cast<std::size_t>(wide), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath.data(), wide);
    const HANDLE file = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return MappedFile{};
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0) {
        (void)CloseHandle(file);

        return MappedFile{};
    }
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    // The view keeps the pages alive on its own, so neither handle is needed
    // once it exists.
    (void)CloseHandle(file);
    if (mapping == nullptr) {
        return MappedFile{};
    }
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    (void)CloseHandle(mapping);
    if (view == nullptr) {
        return MappedFile{};
    }

    return MappedFile{static_cast<const unsigned char*>(view), static_cast<std::size_t>(size.QuadPart)};
#else
    const int file = ::open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return MappedFile{};
    }
    struct stat status = {};
    if (fstat(file, &status) != 0 || status.st_size <= 0) {
        (void)::close(file);

        return MappedFile{};
    }
    const auto size = static_cast<std::size_t>(status.st_size);
    void* view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
    // The mapping holds its own reference to the file.
    (void)::close(file);
    if (view == MAP_FAILED) {
        return MappedFile{};
    }

    return MappedFile{static_cast<const unsigned char*>(view), size};
#endif
}

void unmapFile(MappedFile mapping) noexcept
{
    if (!mapping.valid()) {
        return;
    }
#if defined(_WIN32)
    (void)UnmapViewOfFile(mapping.data);
#else
    (void)munmap(const_cast<unsigned char*>(mapping.data), mapping.size);
#endif
}

}  // namespace sidescopes
