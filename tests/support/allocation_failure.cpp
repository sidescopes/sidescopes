#include "allocation_failure.h"

#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
std::atomic<sidescopes::test::AllocationFailure*> g_failure{nullptr};
std::atomic<std::size_t> g_readers{0};

void checkFailure()
{
    g_readers.fetch_add(1, std::memory_order_seq_cst);
    auto* failure = g_failure.load(std::memory_order_seq_cst);
    const bool fail = failure && failure->shouldFail();
    g_readers.fetch_sub(1, std::memory_order_seq_cst);
    if (fail) {
        throw std::bad_alloc{};
    }
}

void* allocate(std::size_t size)
{
    checkFailure();
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

void* allocateAligned(std::size_t size, std::size_t alignment)
{
    checkFailure();
    void* memory = nullptr;
#ifdef _WIN32
    memory = _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
    if (posix_memalign(&memory, alignment, size == 0 ? 1 : size) != 0) {
        memory = nullptr;
    }
#endif
    if (!memory) {
        throw std::bad_alloc{};
    }
    return memory;
}

void freeAligned(void* memory) noexcept
{
#ifdef _WIN32
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
}  // namespace

namespace sidescopes::test {
AllocationFailure::AllocationFailure(std::size_t failAt, Thread thread, bool sustained)
    : m_failAt(failAt),
      m_thread(thread),
      m_sustained(sustained)
{
    AllocationFailure* expected = nullptr;
    if (!g_failure.compare_exchange_strong(expected, this, std::memory_order_release)) {
        std::abort();  // A nested probe would invalidate both allocation counts.
    }
}

AllocationFailure::~AllocationFailure()
{
    disarm();
}

void AllocationFailure::disarm()
{
    // A reader may have loaded this probe just before it was disarmed. Drain
    // those checks before its state can be destroyed on the controlling thread.
    AllocationFailure* expected = this;
    if (g_failure.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst)) {
        while (g_readers.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }
    }
}

std::size_t AllocationFailure::attempts() const
{
    return m_attempts.load(std::memory_order_relaxed);
}

std::size_t AllocationFailure::failures() const
{
    return m_failures.load(std::memory_order_acquire);
}

bool AllocationFailure::shouldFail()
{
    const bool current = std::this_thread::get_id() == m_owner;
    if (current != (m_thread == Thread::Current)) {
        return false;
    }
    const std::size_t attempt = m_attempts.fetch_add(1, std::memory_order_relaxed);
    if (attempt != m_failAt && (!m_sustained || attempt < m_failAt)) {
        return false;
    }
    m_failures.fetch_add(1, std::memory_order_release);
    return true;
}
}  // namespace sidescopes::test

void* operator new(std::size_t size)
{
    return allocate(size);
}

void* operator new[](std::size_t size)
{
    return allocate(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory, std::align_val_t) noexcept
{
    freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
    freeAligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    freeAligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    freeAligned(memory);
}
