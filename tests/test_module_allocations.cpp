#include <array>
#include <catch2/catch_test_macros.hpp>
#include <new>

#include "allocation_failure.h"
#include "modules/module_registry.h"

namespace sidescopes {
namespace {

constexpr SsScopeDescriptor ExistingDescriptor{"com.example.existing", "Existing", 'X', 0, 0, 0, nullptr, 0, 1.0f};
constexpr std::array<SsScopeDescriptor, 2> NewDescriptors{{
    {"org.sidescopes.waveform", "Waveform", 'W', 0, 0, 0, nullptr, 0, 1.0f},
    {"com.example.additional", "Additional", 'A', 0, 0, 0, nullptr, 0, 1.0f},
}};

int g_initializations = 0;
int g_deinitializations = 0;

const SsModuleEntry ExistingEntry{SS_ABI_MAJOR,
                                  SS_ABI_MINOR,
                                  [] { return true; },
                                  [] {},
                                  [] { return 1u; },
                                  [](uint32_t) { return &ExistingDescriptor; },
                                  [](const char*, const SsHost*) -> SsScopeInstance* { return nullptr; }};

const SsModuleEntry NewEntry{SS_ABI_MAJOR,
                             SS_ABI_MINOR,
                             [] {
                                 ++g_initializations;
                                 return true;
                             },
                             [] { ++g_deinitializations; },
                             [] { return 2u; },
                             [](uint32_t index) { return &NewDescriptors[index]; },
                             [](const char*, const SsHost*) -> SsScopeInstance* { return nullptr; }};

std::size_t registrationAttempts(std::size_t failAt)
{
    g_initializations = 0;
    g_deinitializations = 0;
    std::size_t attempts = 0;
    {
        ModuleRegistry registry;
        REQUIRE(registry.registerModule(ExistingEntry));

        test::AllocationFailure failure(failAt);
        bool interrupted = false;
        bool registered = false;
        try {
            registered = registry.registerModule(NewEntry);
        } catch (const std::bad_alloc&) {
            interrupted = true;
        }
        failure.disarm();
        attempts = failure.attempts();

        REQUIRE(registry.findScope(ExistingDescriptor.id) != nullptr);
        if (interrupted) {
            CHECK(g_initializations == g_deinitializations);
            CHECK(registry.scopes().size() == 1);
            for (const auto& descriptor : NewDescriptors) {
                CHECK(registry.findScope(descriptor.id) == nullptr);
            }
            REQUIRE(registry.registerModule(NewEntry));
        } else {
            REQUIRE(registered);
        }

        REQUIRE(registry.scopes().size() == 3);
        CHECK(registry.scopes()[0].descriptor == &NewDescriptors[0]);
        CHECK(registry.scopes()[1].descriptor == &ExistingDescriptor);
        CHECK(registry.scopes()[2].descriptor == &NewDescriptors[1]);
        CHECK(g_initializations == g_deinitializations + 1);
        const int initializedBeforeRepeat = g_initializations;
        REQUIRE(registry.registerModule(NewEntry));
        CHECK(g_initializations == initializedBeforeRepeat);
    }
    CHECK(g_initializations == g_deinitializations);
    return attempts;
}

}  // namespace

TEST_CASE("Module registration recovers completely from every allocation interruption")
{
    const std::size_t attempts = registrationAttempts(test::AllocationFailure::CountOnly);
    REQUIRE(attempts > 0);
    for (std::size_t failAt = 0; failAt < attempts; ++failAt) {
        CAPTURE(failAt);
        (void)registrationAttempts(failAt);
    }
}

}  // namespace sidescopes
