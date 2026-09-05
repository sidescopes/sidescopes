#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

#include "core/diagnostics.h"
#include "modules/module_registry.h"
#include "temp_file.h"

namespace sidescopes {
namespace {

// Minimal module-entry pieces for the registry's acceptance and rejection
// paths. Creation is unused by descriptor tests and returns no instance.
bool trueInit()
{
    return true;
}

bool falseInit()
{
    return false;
}

void noopDeinit()
{
}

uint32_t oneScope()
{
    return 1;
}

const SsScopeDescriptor FakeDescriptor{
    "org.sidescopes.test.fake", "Fake", 'X', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor* fakeDescriptor(uint32_t index)
{
    return index == 0 ? &FakeDescriptor : nullptr;
}

SsScopeInstance* nullCreate(const char*, const SsHost*)
{
    return nullptr;
}

// A fake instance whose destroy() records the call, so the RAII wrapper can be
// shown to destroy exactly once across a chain of moves.
struct CountingInstance
{
    SsScopeInstance vtable{};
    int destroyed = 0;
};

void countingDestroy(SsScopeInstance* instance)
{
    ++static_cast<CountingInstance*>(instance->instance_data)->destroyed;
}

// Paired with fakeDescriptor, which answers only index 0, this makes a module
// that promises more scopes than it hands descriptors for.
uint32_t twoScopes()
{
    return 2;
}

// A module descriptor belongs to the module; its C entry has no context
// argument. Keep the supplied test metadata alive through registry teardown.
std::size_t registeredDescriptorCount(const SsScopeDescriptor& descriptor)
{
    static const SsScopeDescriptor* supplied = nullptr;
    supplied = &descriptor;

    struct DescriptorScope
    {
        ~DescriptorScope()
        {
            supplied = nullptr;
        }
    } descriptorScope;

    // Registry teardown runs first, while the module can still describe its
    // scope. The guard clears the borrow on normal and exceptional exits.
    const SsModuleEntry entry{
        SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, [](uint32_t) { return supplied; }, nullCreate};
    ModuleRegistry registry;
    REQUIRE(registry.registerModule(entry));
    return registry.scopes().size();
}

// Everything a diagnostic log holds, for the cases that read one back.
std::string readLog(const std::string& path)
{
    std::ifstream file(path);
    std::stringstream content;
    content << file.rdbuf();

    return content.str();
}

}  // namespace

TEST_CASE("Registry skips a scope its module never describes")
{
    // A module that counts two scopes but describes one would otherwise be
    // dereferenced at lookup time.
    const SsModuleEntry entry{SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, twoScopes, fakeDescriptor, nullCreate};
    ModuleRegistry registry;

    REQUIRE(registry.registerModule(entry));
    CHECK(registry.scopes().size() == 1);
    CHECK(registry.findScope("org.sidescopes.test.fake") != nullptr);
}

TEST_CASE("A recording opened after the modules registered is told about them")
{
    // Modules register from a static, before the application can open a log,
    // and being static they never register again. Nothing is recording here,
    // which is the ordinary case.
    const test::TempFile log("modules-report.log");
    const SsModuleEntry wrongAbi{99u, 0u, trueInit, noopDeinit, oneScope, fakeDescriptor, nullCreate};
    const SsModuleEntry sound{SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, fakeDescriptor, nullCreate};
    ModuleRegistry registry;
    CHECK_FALSE(registry.registerModule(wrongAbi));
    REQUIRE(registry.registerModule(sound));

    diagConfigure({"modules", log.path().string()});
    diagConfigure({});

    const std::string content = readLog(log.path().string());
    CHECK(content.find("scope id=org.sidescopes.test.fake") != std::string::npos);
    CHECK(content.find("rejected ABI 99.0") != std::string::npos);
}

TEST_CASE("Registry rejects a module built for another ABI major")
{
    const SsModuleEntry entry{99u, 0u, trueInit, noopDeinit, oneScope, fakeDescriptor, nullCreate};
    ModuleRegistry registry;
    CHECK_FALSE(registry.registerModule(entry));
    CHECK(registry.scopes().empty());
}

TEST_CASE("Registry rejects a module whose init fails")
{
    const SsModuleEntry entry{SS_ABI_MAJOR, SS_ABI_MINOR, falseInit, noopDeinit, oneScope, fakeDescriptor, nullCreate};
    ModuleRegistry registry;
    CHECK_FALSE(registry.registerModule(entry));
    CHECK(registry.scopes().empty());
}

TEST_CASE("Registry reports unknown scopes and instances as absent")
{
    ModuleRegistry& registry = builtinModules();
    CHECK(registry.findScope("org.sidescopes.nonesuch") == nullptr);
    CHECK_FALSE(registry.createInstance("org.sidescopes.nonesuch").valid());
}

TEST_CASE("ScopeInstance owns its handle across moves and destroys it once")
{
    CountingInstance fake;
    fake.vtable.instance_data = &fake;
    fake.vtable.destroy = countingDestroy;

    {
        ScopeInstance first(&fake.vtable);
        REQUIRE(first.valid());

        // Move-construct: ownership transfers and the source empties.
        ScopeInstance second(std::move(first));
        CHECK(second.valid());
        // Reading the moved-from state is the whole point of the check.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move,bugprone-use-after-move)
        CHECK_FALSE(first.valid());

        // Move-assign into an empty instance.
        ScopeInstance third;
        third = std::move(second);
        CHECK(third.valid());
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move,bugprone-use-after-move)
        CHECK_FALSE(second.valid());

        // Self-move must neither destroy nor invalidate the handle. The
        // indirection dodges the compiler's self-move diagnostic while still
        // exercising the guarded a = std::move(a) path.
        ScopeInstance* alias = &third;
        third = std::move(*alias);
        CHECK(third.valid());

        CHECK(fake.destroyed == 0);  // nothing destroyed while an owner is live
    }

    CHECK(fake.destroyed == 1);  // destroyed exactly once at the final scope exit
}

TEST_CASE("Registry rejects incompatible pre-release minors before initialization")
{
    const SsModuleEntry entry{SS_ABI_MAJOR, SS_ABI_MINOR + 1, trueInit,  noopDeinit,
                              oneScope,     fakeDescriptor,   nullCreate};
    ModuleRegistry registry;
    CHECK_FALSE(registry.registerModule(entry));
    CHECK(registry.scopes().empty());
}

TEST_CASE("Registry requires a complete module entry")
{
    const SsModuleEntry entry{SS_ABI_MAJOR, SS_ABI_MINOR, nullptr, noopDeinit, oneScope, fakeDescriptor, nullCreate};
    ModuleRegistry registry;
    CHECK_FALSE(registry.registerModule(entry));
    CHECK(registry.scopes().empty());
}

TEST_CASE("Registry matches unsuccessful initialization with one deinitialization")
{
    static int deinitializations = 0;
    deinitializations = 0;
    const SsModuleEntry entry{SS_ABI_MAJOR, SS_ABI_MINOR,   falseInit, [] { ++deinitializations; },
                              oneScope,     fakeDescriptor, nullCreate};
    {
        ModuleRegistry registry;
        CHECK_FALSE(registry.registerModule(entry));
        CHECK(deinitializations == 1);
    }
    CHECK(deinitializations == 1);
}

TEST_CASE("Registering one module twice initializes and deinitializes it once")
{
    static int initializations = 0;
    static int deinitializations = 0;
    initializations = 0;
    deinitializations = 0;
    const SsModuleEntry entry{SS_ABI_MAJOR,
                              SS_ABI_MINOR,
                              [] {
                                  ++initializations;
                                  return true;
                              },
                              [] { ++deinitializations; },
                              oneScope,
                              fakeDescriptor,
                              nullCreate};
    {
        ModuleRegistry registry;
        REQUIRE(registry.registerModule(entry));
        REQUIRE(registry.registerModule(entry));
        CHECK(registry.scopes().size() == 1);
        CHECK(initializations == 1);
        CHECK(deinitializations == 0);
    }
    CHECK(deinitializations == 1);
}

TEST_CASE("Registry rejects duplicate scope identities")
{
    const SsModuleEntry first{SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, fakeDescriptor, nullCreate};
    const SsModuleEntry second = first;
    ModuleRegistry registry;
    REQUIRE(registry.registerModule(first));
    REQUIRE(registry.registerModule(second));
    REQUIRE(registry.scopes().size() == 1);
    CHECK(registry.scopes().front().module == &first);
}

TEST_CASE("Registry ignores descriptors without a usable identity")
{
    const SsModuleEntry entry{SS_ABI_MAJOR,
                              SS_ABI_MINOR,
                              trueInit,
                              noopDeinit,
                              oneScope,
                              [](uint32_t) -> const SsScopeDescriptor* {
                                  static const SsScopeDescriptor invalid{};
                                  return &invalid;
                              },
                              nullCreate};
    ModuleRegistry registry;
    REQUIRE(registry.registerModule(entry));
    CHECK(registry.scopes().empty());
}

TEST_CASE("Registry requires storage for every declared parameter")
{
    SsScopeDescriptor descriptor = FakeDescriptor;
    descriptor.param_count = 1;
    CHECK(registeredDescriptorCount(descriptor) == 0);
}

TEST_CASE("Registry requires named and distinct parameters")
{
    SsParamInfo parameters[] = {{"first", "First", SS_PARAM_FLOAT, 0, 1, 0.5, 0, nullptr, nullptr},
                                {"second", "Second", SS_PARAM_FLOAT, 0, 1, 0.5, 0, nullptr, nullptr}};
    SsScopeDescriptor descriptor = FakeDescriptor;
    descriptor.params = parameters;
    descriptor.param_count = 2;
    REQUIRE(registeredDescriptorCount(descriptor) == 1);

    SECTION("a key is missing")
    {
        parameters[0].key = nullptr;
    }
    SECTION("a key is empty")
    {
        parameters[0].key = "";
    }
    SECTION("a label is missing")
    {
        parameters[0].label = nullptr;
    }
    SECTION("two entries declare the same key")
    {
        parameters[1].key = "first";
    }
    CHECK(registeredDescriptorCount(descriptor) == 0);
}

TEST_CASE("Registry requires finite and ordered numeric parameter metadata")
{
    SsParamInfo parameter{"amount", "Amount", SS_PARAM_FLOAT, 0, 1, 0.5, 0, nullptr, nullptr};
    SsScopeDescriptor descriptor = FakeDescriptor;
    descriptor.params = &parameter;
    descriptor.param_count = 1;
    REQUIRE(registeredDescriptorCount(descriptor) == 1);

    SECTION("the lower bound is not finite")
    {
        parameter.min_value = std::numeric_limits<double>::quiet_NaN();
    }
    SECTION("the upper bound is not finite")
    {
        parameter.max_value = std::numeric_limits<double>::infinity();
    }
    SECTION("the default is not finite")
    {
        parameter.default_value = std::numeric_limits<double>::quiet_NaN();
    }
    SECTION("the range is reversed")
    {
        parameter.min_value = 2;
    }
    CHECK(registeredDescriptorCount(descriptor) == 0);
}

TEST_CASE("Registry requires usable choice-menu metadata")
{
    const char* choices[]{"First", "Second", nullptr};
    SsParamInfo parameter{"style", "Style", SS_PARAM_CHOICE, 0, 1, 0, 0, "Style", choices};
    SsScopeDescriptor descriptor = FakeDescriptor;
    descriptor.params = &parameter;
    descriptor.param_count = 1;
    REQUIRE(registeredDescriptorCount(descriptor) == 1);

    SECTION("the menu title is missing")
    {
        parameter.menu_label = nullptr;
    }
    SECTION("the choice array is missing")
    {
        parameter.choices = nullptr;
    }
    SECTION("the choice array is empty")
    {
        choices[0] = nullptr;
    }
    CHECK(registeredDescriptorCount(descriptor) == 0);
}

TEST_CASE("Registry rejects a non-finite intensity shift")
{
    SsParamInfo parameter{"gain", "Intensity", SS_PARAM_INTENSITY, 0, 100, 1, 0, nullptr, nullptr};
    SsScopeDescriptor descriptor = FakeDescriptor;
    descriptor.params = &parameter;
    descriptor.param_count = 1;
    REQUIRE(registeredDescriptorCount(descriptor) == 1);

    for (const double shift : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        parameter.intensity_shift = shift;
        CHECK(registeredDescriptorCount(descriptor) == 0);
    }
}

TEST_CASE("Registry rejects incomplete instance operations and destroys the rejected handle")
{
    static int destroyed = 0;
    destroyed = 0;
    const SsModuleEntry entry{
        SS_ABI_MAJOR,
        SS_ABI_MINOR,
        trueInit,
        noopDeinit,
        oneScope,
        fakeDescriptor,
        [](const char*, const SsHost*) -> SsScopeInstance* {
            static SsScopeInstance instance{};
            instance.configure = [](SsScopeInstance*, const SsParamValue*, uint32_t) { return true; };
            instance.accumulate = nullptr;
            instance.image = [](const SsScopeInstance*) { return SsImageView{}; };
            instance.graticule = [](const SsScopeInstance*, SsGraticulePrimitive*, uint32_t) { return 0u; };
            instance.markers = [](const SsScopeInstance*, SsColor, SsMarker*, uint32_t) { return 0u; };
            instance.get_extension = [](const SsScopeInstance*, const char*) -> const void* { return nullptr; };
            instance.destroy = [](SsScopeInstance*) { ++destroyed; };
            return &instance;
        }};
    ModuleRegistry registry;
    REQUIRE(registry.registerModule(entry));
    CHECK_FALSE(registry.createInstance(FakeDescriptor.id).valid());
    CHECK(destroyed == 1);
}

}  // namespace sidescopes
