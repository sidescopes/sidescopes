#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/scope_registry.h"
#include "app/stack_tokens.h"
#include "core/diagnostics.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"
#include "support/scope_tokens.h"
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

const SsScopeDescriptor ReservedHostDescriptor{
    ColorPickerScopeId, "External picker", 'X', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor* reservedHostDescriptor(uint32_t index)
{
    return index == 0 ? &ReservedHostDescriptor : nullptr;
}

const SsModuleEntry ReservedHostModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, reservedHostDescriptor, nullCreate,
};

}  // namespace

TEST_CASE("The module registry orders the built-ins canonically in every build")
{
    // Static registration keeps link order; the dynamic loader hands modules
    // back in file-name order. The registry imposes one canonical order over
    // both, so this holds identically in the static and dynamic configurations.
    const std::vector<RegisteredScope>& scopes = builtinModules().scopes();
    REQUIRE(scopes.size() == 5);
    CHECK(std::string(scopes[0].descriptor->id) == "org.sidescopes.vectorscope");
    CHECK(std::string(scopes[1].descriptor->id) == "org.sidescopes.waveform");
    CHECK(std::string(scopes[2].descriptor->id) == "org.sidescopes.waveform.luma");
    CHECK(std::string(scopes[3].descriptor->id) == "org.sidescopes.parade");
    CHECK(std::string(scopes[4].descriptor->id) == "org.sidescopes.histogram");
}

TEST_CASE("A scope shares its family with everything its module registers")
{
    // The family is what stops a scope being given an image size of its own.
    // The scopes of one module wrap one engine at one geometry and share one
    // set of bins, so the host has to decide their size together - and a
    // member the list leaves out is handed its own, which DRAWS CORRECTLY
    // while re-laying those bins at every scope of every frame. Nothing else
    // would fail if the list fell behind the module.
    std::map<const SsModuleEntry*, bool> families;
    for (const RegisteredScope& scope : builtinModules().scopes()) {
        const std::string_view id = scope.descriptor->id;
        const bool family = inWaveformFamily(id);
        const auto seen = families.find(scope.module);
        if (seen == families.end()) {
            families.emplace(scope.module, family);
            continue;
        }
        INFO("scope " << id << " is in a different family from its module's first scope");
        CHECK(seen->second == family);
    }
}

TEST_CASE("The scope registry lists the built-ins then the color picker")
{
    const ScopeRegistry registry{builtinModules()};
    const std::vector<HostScope>& scopes = registry.scopes();
    REQUIRE(scopes.size() == 6);

    CHECK(scopes[0].id == "org.sidescopes.vectorscope");
    CHECK(scopes[1].id == "org.sidescopes.waveform");
    CHECK(scopes[2].id == "org.sidescopes.waveform.luma");
    CHECK(scopes[3].id == "org.sidescopes.parade");
    CHECK(scopes[4].id == "org.sidescopes.histogram");
    CHECK(scopes[5].id == "org.sidescopes.colorpicker");

    CHECK(scopes[0].letter == 'V');
    CHECK(scopes[1].letter == 'W');
    CHECK(scopes[2].letter == 'L');
    CHECK(scopes[3].letter == 'R');
    CHECK(scopes[4].letter == 'H');
    CHECK(scopes[5].letter == 'C');
}

TEST_CASE("No two shipped scopes claim one letter")
{
    // A letter names exactly one scope wherever it is read: the stack token a
    // preferences file holds, the selector's shortcut column, and the key press
    // itself. Two scopes on one letter is not a resolvable state - the plain key
    // shows whichever the scan reached last while Shift stacks both - and it
    // shipped once, because every scope's own descriptor still read correctly
    // and nothing asked whether any two agreed.
    //
    // A letter already taken is refused, so the registry answers a collision by
    // registering the loser letterless: the scope that did not get the letter it
    // asked for is the one that names the clash.
    const ScopeRegistry registry{builtinModules()};
    std::string taken;
    for (const HostScope& scope : registry.scopes()) {
        INFO("scope " << scope.id);
        if (scope.descriptor != nullptr) {
            CHECK(scope.letter == scope.descriptor->letter);
        }
        REQUIRE(scope.letter != 0);
        CHECK(taken.find(scope.letter) == std::string::npos);
        taken.push_back(scope.letter);
    }
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
    CHECK(registry.indexOf("org.sidescopes.histogram") == 4);
    CHECK(registry.indexOf("org.sidescopes.colorpicker") == 5);
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
    REQUIRE(registry.byId("org.sidescopes.neutral") == nullptr);

    CHECK(parseStackTokens(registry, testing::idTokens("V") + "[org.sidescopes.neutral]" + testing::idTokens("H")) ==
          std::vector<std::string>{VectorscopeScopeId, HistogramScopeId});
    CHECK(parseStackTokens(registry, "[org.sidescopes.neutral]") ==
          std::vector<std::string>{VectorscopeScopeId, WaveformScopeId});
}

TEST_CASE("Pins mark the scopes that declare themselves targets")
{
    const ScopeRegistry registry{builtinModules()};

    // Nothing on screen, nothing to pin into.
    CHECK_FALSE(anyPinTarget(registry, {}));
    // The host's colour picker takes pins without a module descriptor to say
    // so, which is why the question is not the flag alone.
    CHECK(anyPinTarget(registry, {std::string{ColorPickerScopeId}}));
    // The vectorscope is the module scope that declares the flag.
    CHECK(anyPinTarget(registry, {std::string{VectorscopeScopeId}}));
    CHECK_FALSE(anyPinTarget(registry, {std::string{WaveformScopeId}, std::string{HistogramScopeId}}));
    // One target anywhere in the stack is enough.
    CHECK(anyPinTarget(registry, {std::string{WaveformScopeId}, std::string{VectorscopeScopeId}}));
    // A scope the registry has never heard of cannot take a pin.
    CHECK_FALSE(anyPinTarget(registry, {std::string{"org.example.unknown"}}));
}

TEST_CASE("A module cannot replace the host color picker identity")
{
    ModuleRegistry modules;
    REQUIRE(modules.registerModule(ReservedHostModuleEntry));
    const ScopeRegistry registry{modules};
    REQUIRE(registry.scopes().size() == 1);
    const HostScope* picker = registry.byId(ColorPickerScopeId);
    REQUIRE(picker != nullptr);
    CHECK(picker->host);
    CHECK(picker->descriptor == nullptr);
    CHECK(picker->letter == ColorPickerLetter);
}

}  // namespace sidescopes
