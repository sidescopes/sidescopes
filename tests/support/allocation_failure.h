#pragma once

#include <atomic>
#include <cstddef>
#include <limits>
#include <thread>

namespace sidescopes::test {

// Linked only into the allocation-test executable. The replacement global
// allocation functions fail an actual host allocation; production containers
// and module interfaces need no allocator or failure switches.
class AllocationFailure
{
public:
    enum class Thread
    {
        Current,
        Other
    };

    static constexpr std::size_t CountOnly = std::numeric_limits<std::size_t>::max();

    explicit AllocationFailure(std::size_t failAt, Thread thread = Thread::Current, bool sustained = false);
    ~AllocationFailure();
    AllocationFailure(const AllocationFailure&) = delete;
    AllocationFailure& operator=(const AllocationFailure&) = delete;

    void disarm();
    [[nodiscard]] std::size_t attempts() const;
    [[nodiscard]] std::size_t failures() const;

    // Called by the replacement allocation functions, without allocating.
    [[nodiscard]] bool shouldFail();

private:
    std::thread::id m_owner = std::this_thread::get_id();
    std::size_t m_failAt;
    Thread m_thread;
    bool m_sustained;
    std::atomic<std::size_t> m_attempts{0};
    std::atomic<std::size_t> m_failures{0};
};

}  // namespace sidescopes::test
