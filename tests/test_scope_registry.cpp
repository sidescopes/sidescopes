#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "app/scope_registry.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

// Two hand-built module entries that both claim 'W'. The first establishes the
// letter; the second, registered after it, collides. Neither create is
// reached - the scope registry reads only descriptors - so create returns null.
bool trueInit()
{
    return true;
}

void noopDeinit()
{
}

uint32_t oneScope()
{
    return 1;
}

SsScopeInstance* nullCreate(const char*, const SsHost*)
{
    return nullptr;
}

const SsScopeDescriptor HolderWDescriptor{
    "org.sidescopes.test.holderw", "Holder W", 'W', 0, 0, 0u, nullptr, 0u,
};

const SsScopeDescriptor* holderWDescriptor(uint32_t index)
{
    return index == 0 ? &HolderWDescriptor : nullptr;
}

const SsModuleEntry HolderWModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, holderWDescriptor, nullCreate,
};

const SsScopeDescriptor CollideWDescriptor{
    "org.sidescopes.test.collidew", "Collide W", 'W', 0, 0, 0u, nullptr, 0u,
};

const SsScopeDescriptor* collideWDescriptor(uint32_t index)
{
    return index == 0 ? &CollideWDescriptor : nullptr;
}

const SsModuleEntry CollideWModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, collideWDescriptor, nullCreate,
};

}  // namespace

TEST_CASE("The scope registry lists the built-ins then the color picker")
{
    const ScopeRegistry registry{builtinModules()};
    const std::vector<HostScope>& scopes = registry.scopes();
    REQUIRE(scopes.size() == 5);

    CHECK(scopes[0].id == "org.sidescopes.vectorscope");
    CHECK(scopes[1].id == "org.sidescopes.waveform");
    CHECK(scopes[2].id == "org.sidescopes.parade");
    CHECK(scopes[3].id == "org.sidescopes.histogram");
    CHECK(scopes[4].id == "org.sidescopes.colorpicker");

    CHECK(scopes[0].letter == 'V');
    CHECK(scopes[1].letter == 'W');
    CHECK(scopes[2].letter == 'R');
    CHECK(scopes[3].letter == 'H');
    CHECK(scopes[4].letter == 'C');
}

TEST_CASE("The scope registry resolves scopes by id, letter, and index")
{
    const ScopeRegistry registry{builtinModules()};

    const HostScope* waveform = registry.byId("org.sidescopes.waveform");
    REQUIRE(waveform != nullptr);
    CHECK(waveform->letter == 'W');
    CHECK_FALSE(waveform->host);
    CHECK(waveform->descriptor != nullptr);

    CHECK(registry.byLetter('R') == registry.byId("org.sidescopes.parade"));
    CHECK(registry.byId("org.sidescopes.nonesuch") == nullptr);
    CHECK(registry.byLetter('Z') == nullptr);
    CHECK(registry.byLetter(0) == nullptr);

    CHECK(registry.indexOf("org.sidescopes.vectorscope") == 0);
    CHECK(registry.indexOf("org.sidescopes.histogram") == 3);
    CHECK(registry.indexOf("org.sidescopes.colorpicker") == 4);
    CHECK(registry.indexOf("org.sidescopes.nonesuch") == -1);
}

TEST_CASE("The color picker is a host scope with no descriptor")
{
    const ScopeRegistry registry{builtinModules()};
    const HostScope* picker = registry.byId(ColorPickerScopeId);
    REQUIRE(picker != nullptr);
    CHECK(picker->host);
    CHECK(picker->descriptor == nullptr);
    CHECK(picker->letter == 'C');
    CHECK(registry.byLetter('C') == picker);
}

TEST_CASE("A colliding letter registers the scope letterless but reachable")
{
    ModuleRegistry modules;
    REQUIRE(modules.registerModule(HolderWModuleEntry));
    REQUIRE(modules.registerModule(CollideWModuleEntry));

    const ScopeRegistry registry{modules};

    const HostScope* holder = registry.byId("org.sidescopes.test.holderw");
    REQUIRE(holder != nullptr);
    CHECK(holder->letter == 'W');

    const HostScope* collide = registry.byId("org.sidescopes.test.collidew");
    REQUIRE(collide != nullptr);
    // The letter was already taken, so this scope carries none.
    CHECK(collide->letter == 0);
    // It is still a real module scope, reachable by id and present in order.
    CHECK(collide->descriptor != nullptr);
    CHECK(registry.indexOf("org.sidescopes.test.collidew") == 1);

    // 'W' resolves to the first claimant, never the letterless collider.
    CHECK(registry.byLetter('W') == holder);
}

}  // namespace sidescopes
