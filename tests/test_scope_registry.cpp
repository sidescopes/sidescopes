#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app/scope_registry.h"
#include "app/stack_tokens.h"
#include "core/diagnostics.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"
#include "temp_file.h"

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
    "org.sidescopes.test.holderw", "Holder W", 'W', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor* holderWDescriptor(uint32_t index)
{
    return index == 0 ? &HolderWDescriptor : nullptr;
}

const SsModuleEntry HolderWModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, holderWDescriptor, nullCreate,
};

const SsScopeDescriptor CollideWDescriptor{
    "org.sidescopes.test.collidew", "Collide W", 'W', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor* collideWDescriptor(uint32_t index)
{
    return index == 0 ? &CollideWDescriptor : nullptr;
}

const SsModuleEntry CollideWModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, collideWDescriptor, nullCreate,
};

}  // namespace

TEST_CASE("The module registry orders the built-ins canonically in every build")
{
    // Static registration keeps link order; the dynamic loader hands modules
    // back in file-name order. The registry imposes one canonical order over
    // both, so this holds identically in the static and dynamic configurations.
    const std::vector<RegisteredScope>& scopes = builtinModules().scopes();
    REQUIRE(scopes.size() == 4);
    CHECK(std::string(scopes[0].descriptor->id) == "org.sidescopes.vectorscope");
    CHECK(std::string(scopes[1].descriptor->id) == "org.sidescopes.waveform");
    CHECK(std::string(scopes[2].descriptor->id) == "org.sidescopes.parade");
    CHECK(std::string(scopes[3].descriptor->id) == "org.sidescopes.histogram");
}

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

TEST_CASE("A refused letter is stated to a recording started later")
{
    // The letters are handed out once, while the application starts; a support
    // log opened afterwards still has to answer why a scope has no shortcut.
    const test::TempFile log("scope-registry-letters.log");
    ModuleRegistry modules;
    REQUIRE(modules.registerModule(HolderWModuleEntry));
    REQUIRE(modules.registerModule(CollideWModuleEntry));
    const ScopeRegistry registry{modules};

    diagConfigure({"modules", log.path().string()});
    diagConfigure({});

    std::ifstream file(log.path());
    std::stringstream content;
    content << file.rdbuf();
    CHECK(content.str().find("letter 'W' for org.sidescopes.test.collidew unavailable") != std::string::npos);
}

TEST_CASE("Every registered scope round-trips through the stack")
{
    // The regression guard for the persistence bug: a scope added after the
    // fixed V/W/R/H/C set was written to the preferences file and read back as
    // the vectorscope. Every scope the build registers is checked, so the next
    // one is covered the day it is added rather than the day it is noticed.
    const ScopeRegistry registry{builtinModules()};
    for (const HostScope& scope : registry.scopes()) {
        CAPTURE(scope.id);
        const std::string token = formatStackTokens(registry, {scope.id});
        CHECK_FALSE(token.empty());
        CHECK(parseStackTokens(registry, token) == std::vector<std::string>{scope.id});
    }
}

TEST_CASE("A saved stack naming a scope this build dropped still loads")
{
    // Scopes come and go before 1.0, and a preferences file outlives them: a
    // stack naming one that is gone must lose that scope and keep the rest,
    // and one naming nothing left must fall back rather than load empty.
    const ScopeRegistry registry{builtinModules()};
    REQUIRE(registry.byLetter('N') == nullptr);

    CHECK(parseStackTokens(registry, "VNH") == std::vector<std::string>{VectorscopeScopeId, HistogramScopeId});
    CHECK(parseStackTokens(registry, "[org.sidescopes.neutral]") == std::vector<std::string>{VectorscopeScopeId});
    CHECK(parseStackTokens(registry, "N") == std::vector<std::string>{VectorscopeScopeId});
}

}  // namespace sidescopes
