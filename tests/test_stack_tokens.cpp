#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "app/scope_registry.h"
#include "app/stack_tokens.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

// Three hand-built scopes, so the cases below never depend on which modules
// this build registers: two on letters of their own, and one asking for the
// host-reserved 'C', which the registry turns down and registers letterless.
constexpr char AlphaId[] = "org.sidescopes.test.alpha";
constexpr char BetaId[] = "org.sidescopes.test.beta";
constexpr char LetterlessId[] = "org.sidescopes.test.letterless";

bool trueInit()
{
    return true;
}

void noopDeinit()
{
}

SsScopeInstance* nullCreate(const char*, const SsHost*)
{
    return nullptr;
}

const SsScopeDescriptor AlphaDescriptor{
    AlphaId, "Alpha", 'V', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor BetaDescriptor{
    BetaId, "Beta", 'W', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor LetterlessDescriptor{
    LetterlessId, "Letterless", 'C', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const std::array<const SsScopeDescriptor*, 3> TestDescriptors{&AlphaDescriptor, &BetaDescriptor, &LetterlessDescriptor};

const SsScopeDescriptor* testDescriptor(uint32_t index)
{
    return index < TestDescriptors.size() ? TestDescriptors[index] : nullptr;
}

uint32_t testScopeCount()
{
    return static_cast<uint32_t>(TestDescriptors.size());
}

const SsModuleEntry TestModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, testScopeCount, testDescriptor, nullCreate,
};

// The one module, registered once into a registry that outlives every case.
const ModuleRegistry& testModules()
{
    static ModuleRegistry modules;
    static const bool registered = [] {
        (void)modules.registerModule(TestModuleEntry);

        return true;
    }();
    (void)registered;

    return modules;
}

// The registry the tokens resolve through: immutable once built, so one
// instance serves the whole file.
const ScopeRegistry& registry()
{
    static const ScopeRegistry instance{testModules()};

    return instance;
}

}  // namespace

TEST_CASE("A bracketed token names its scope by id")
{
    CHECK(parseStackTokens(registry(), std::string("[") + BetaId + "]") == std::vector<std::string>{BetaId});
}

TEST_CASE("A letter names nothing: identity is the id alone")
{
    // A letter is a property of a scope, not its identity. The registry hands
    // one out only if it is still free, so a collision or a change in the
    // order modules register in would silently re-point every token already
    // written - an arrangement quietly becoming a different set of scopes.
    REQUIRE(registry().byLetter('V') != nullptr);
    CHECK(parseStackTokens(registry(), "VW") == std::vector<std::string>{VectorscopeScopeId});
}

TEST_CASE("A token the registry does not know is dropped")
{
    const std::string alpha = std::string("[") + AlphaId + "]";
    const std::string beta = std::string("[") + BetaId + "]";
    CHECK(parseStackTokens(registry(), alpha + "[org.sidescopes.test.absent]" + beta) ==
          std::vector<std::string>{AlphaId, BetaId});
}

TEST_CASE("Repeated tokens collapse to one scope")
{
    const std::string alpha = std::string("[") + AlphaId + "]";
    const std::string beta = std::string("[") + BetaId + "]";
    CHECK(parseStackTokens(registry(), alpha + beta + alpha) == std::vector<std::string>{AlphaId, BetaId});
}

TEST_CASE("A token string naming nothing valid falls back")
{
    // The fallback is the app's default stack rather than a registry lookup, so
    // it holds even here, where the registry has no vectorscope at all.
    CHECK(parseStackTokens(registry(), "") == std::vector<std::string>{VectorscopeScopeId});
    CHECK(parseStackTokens(registry(), "zzz") == std::vector<std::string>{VectorscopeScopeId});
    // An unterminated bracket ends the parse where it stands.
    CHECK(parseStackTokens(registry(), "[org.sidescopes") == std::vector<std::string>{VectorscopeScopeId});
}

TEST_CASE("A letterless scope round-trips as an id token")
{
    // Nothing about a letterless scope is special any more: every scope is
    // written the same way, by the id it registered under.
    const HostScope* scope = registry().byId(LetterlessId);
    REQUIRE(scope != nullptr);
    REQUIRE(scope->letter == 0);

    const std::string token = std::string("[") + LetterlessId + "]";
    CHECK(parseStackTokens(registry(), token) == std::vector<std::string>{LetterlessId});
    CHECK(formatStackTokens(registry(), {LetterlessId}) == token);
}

TEST_CASE("Formatting a parsed stack writes the same tokens")
{
    const std::string tokens = std::string("[") + AlphaId + "][" + BetaId + "][" + LetterlessId + "]";
    CHECK(formatStackTokens(registry(), parseStackTokens(registry(), tokens)) == tokens);
}

}  // namespace sidescopes
