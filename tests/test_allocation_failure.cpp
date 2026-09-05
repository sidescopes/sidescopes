#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <new>

#include "allocation_failure.h"

namespace sidescopes::test {
namespace {
constexpr std::align_val_t Alignment{64};

struct AllocationForm
{
    const char* name;
    void* (*allocate)(std::size_t);
    void (*release)(void*);
    std::size_t alignment;
    bool nothrow;
    void (*releaseAfterFailure)(void*) = nullptr;
};

const std::array Forms{
    AllocationForm{"scalar", [](std::size_t size) { return ::operator new(size); },
                   [](void* memory) { ::operator delete(memory); }, alignof(std::max_align_t), false},
    AllocationForm{"array", [](std::size_t size) { return ::operator new[](size); },
                   [](void* memory) { ::operator delete[](memory); }, alignof(std::max_align_t), false},
    AllocationForm{"nothrow scalar", [](std::size_t size) { return ::operator new(size, std::nothrow); },
                   [](void* memory) { ::operator delete(memory); }, alignof(std::max_align_t), true,
                   [](void* memory) { ::operator delete(memory, std::nothrow); }},
    AllocationForm{"nothrow array", [](std::size_t size) { return ::operator new[](size, std::nothrow); },
                   [](void* memory) { ::operator delete[](memory); }, alignof(std::max_align_t), true,
                   [](void* memory) { ::operator delete[](memory, std::nothrow); }},
    AllocationForm{"aligned scalar", [](std::size_t size) { return ::operator new(size, Alignment); },
                   [](void* memory) { ::operator delete(memory, Alignment); }, std::size_t{64}, false},
    AllocationForm{"aligned array", [](std::size_t size) { return ::operator new[](size, Alignment); },
                   [](void* memory) { ::operator delete[](memory, Alignment); }, std::size_t{64}, false},
    AllocationForm{"aligned nothrow scalar",
                   [](std::size_t size) { return ::operator new(size, Alignment, std::nothrow); },
                   [](void* memory) { ::operator delete(memory, Alignment); }, std::size_t{64}, true,
                   [](void* memory) { ::operator delete(memory, Alignment, std::nothrow); }},
    AllocationForm{"aligned nothrow array",
                   [](std::size_t size) { return ::operator new[](size, Alignment, std::nothrow); },
                   [](void* memory) { ::operator delete[](memory, Alignment); }, std::size_t{64}, true,
                   [](void* memory) { ::operator delete[](memory, Alignment, std::nothrow); }},
};

void checkAllocation(const AllocationForm& form, std::size_t size, void (*release)(void*))
{
    CAPTURE(size);
    AllocationFailure counter(AllocationFailure::CountOnly);
    void* memory = form.allocate(size);
    counter.disarm();
    const bool present = memory != nullptr;
    const bool aligned = reinterpret_cast<std::uintptr_t>(memory) % form.alignment == 0;
    release(memory);
    CHECK(present);
    CHECK(aligned);
    CHECK(counter.attempts() == 1);
    CHECK(counter.failures() == 0);
}

}  // namespace

TEST_CASE("Allocation overrides pair every allocation form with ordinary cleanup")
{
    for (const AllocationForm& form : Forms) {
        CAPTURE(form.name);
        for (const std::size_t size : {std::size_t{0}, std::size_t{37}}) {
            // A successful nothrow allocation is released by ordinary delete,
            // just as stable_sort's temporary storage is in the standard library.
            checkAllocation(form, size, form.release);
        }
        if (form.releaseAfterFailure) {
            checkAllocation(form, 37, form.releaseAfterFailure);
        }

        AllocationFailure failure(0);
        void* memory = nullptr;
        bool threw = false;
        try {
            memory = form.allocate(37);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        failure.disarm();
        const bool empty = memory == nullptr;
        form.release(memory);
        CHECK(empty);
        CHECK(threw == !form.nothrow);
        CHECK(failure.attempts() == 1);
        CHECK(failure.failures() == 1);
    }
}

}  // namespace sidescopes::test
