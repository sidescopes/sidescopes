#include "core/heap.h"

#include <cstdio>
#include <cstdlib>
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
#elif defined(__EMSCRIPTEN__)
    // Emscripten's allocator has no trim, and there is no operating system
    // underneath to hand pages back to: the heap is one linear memory that
    // only ever grows. Releasing is a no-op rather than an omission.
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

#if defined(__EMSCRIPTEN__)

namespace {

// The browser build's files live in the bundled in-memory filesystem, where
// mmap has nothing real to map. Reading the whole file is the same thing
// here: the heap is one linear memory that only grows, so the pages were
// never going back to an operating system anyway.
MappedFile readWholeFile(const char* path)
{
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return MappedFile{};
    }
    long length = 0;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        length = std::ftell(file);
        std::rewind(file);
    }
    if (length <= 0) {
        (void)std::fclose(file);

        return MappedFile{};
    }
    auto* bytes = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(length)));
    if (bytes == nullptr) {
        (void)std::fclose(file);

        return MappedFile{};
    }
    const std::size_t read = std::fread(bytes, 1, static_cast<std::size_t>(length), file);
    (void)std::fclose(file);
    if (read != static_cast<std::size_t>(length)) {
        std::free(bytes);

        return MappedFile{};
    }

    return MappedFile{bytes, static_cast<std::size_t>(length)};
}

}  // namespace

#endif

#if defined(_WIN32)

namespace {

/// A read-only view of the whole file, through a file mapping. The view
/// keeps the pages alive on its own, so neither handle outlives this call.
MappedFile mapWholeFile(const char* path)
{
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
}

}  // namespace

#elif !defined(__EMSCRIPTEN__)

namespace {

/// A read-only mapping of the whole file. The mapping holds its own
/// reference, so the descriptor is closed straight away.
MappedFile mapWholeFile(const char* path)
{
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
}

}  // namespace

#endif

MappedFile mapFileReadOnly(const char* path)
{
    if (path == nullptr) {
        return MappedFile{};
    }
#if defined(__EMSCRIPTEN__)
    return readWholeFile(path);
#else
    return mapWholeFile(path);
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
