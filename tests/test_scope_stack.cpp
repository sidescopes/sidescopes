#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "app/scope_registry.h"
#include "app/scope_stack.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

// The built-in scope registry, shared across the cases: it is immutable, so one
// instance serves every stack under test.
const ScopeRegistry& registry()
{
    static const ScopeRegistry instance{builtinModules()};

    return instance;
}

// A scope whose requested letter is the host-reserved 'C', so the registry
// registers it letterless: the only way it can appear in the stack is by id.
constexpr char LetterlessId[] = "org.sidescopes.test.letterless";

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

const SsScopeDescriptor LetterlessDescriptor{
    LetterlessId, "Letterless", 'C', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor* letterlessDescriptor(uint32_t index)
{
    return index == 0 ? &LetterlessDescriptor : nullptr;
}

const SsModuleEntry LetterlessModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, letterlessDescriptor, nullCreate,
};

// A registry whose one module scope is letterless, alongside the appended host
// color picker; a letterless scope can only ride the stack as an id token.
ScopeRegistry letterlessRegistry()
{
    ModuleRegistry modules;
    (void)modules.registerModule(LetterlessModuleEntry);

    return ScopeRegistry{modules};
}

}  // namespace

TEST_CASE("A stack starts on the vectorscope")
{
    ScopeStack stack{registry()};
    CHECK(stack.shows(VectorscopeScopeId));
    CHECK_FALSE(stack.shows(HistogramScopeId));
    CHECK(stack.ids().size() == 1);
}

TEST_CASE("Toggling adds a scope and reports it newly visible")
{
    ScopeStack stack{registry()};
    CHECK(stack.toggle(HistogramScopeId));
    CHECK(stack.shows(HistogramScopeId));
    CHECK(stack.ids().size() == 2);
    // Toggling it back off is not an activation.
    CHECK_FALSE(stack.toggle(HistogramScopeId));
    CHECK_FALSE(stack.shows(HistogramScopeId));
}

TEST_CASE("The last scope cannot be toggled away")
{
    ScopeStack stack{registry()};
    REQUIRE(stack.ids().size() == 1);
    CHECK_FALSE(stack.toggle(VectorscopeScopeId));
    CHECK(stack.ids().size() == 1);
    CHECK(stack.shows(VectorscopeScopeId));
}

TEST_CASE("Choosing solos a scope unless stacking")
{
    ScopeStack stack{registry()};
    stack.toggle(WaveformScopeId);
    REQUIRE(stack.ids().size() == 2);

    SECTION("solo replaces the stack")
    {
        CHECK(stack.choose(HistogramScopeId, false));
        CHECK(stack.ids().size() == 1);
        CHECK(stack.shows(HistogramScopeId));
        CHECK_FALSE(stack.shows(VectorscopeScopeId));
    }

    SECTION("soloing an already-shown scope is not an activation")
    {
        CHECK_FALSE(stack.choose(WaveformScopeId, false));
        CHECK(stack.ids().size() == 1);
        CHECK(stack.shows(WaveformScopeId));
    }

    SECTION("stacking keeps the others")
    {
        CHECK(stack.choose(HistogramScopeId, true));
        CHECK(stack.ids().size() == 3);
    }
}

TEST_CASE("The enabled ids cover the whole stack")
{
    ScopeStack stack{registry()};
    stack.restore("V");
    CHECK(stack.enabledScopeIds() == std::vector<std::string>{VectorscopeScopeId});

    stack.restore("VH");
    CHECK(stack.enabledScopeIds() == std::vector<std::string>{VectorscopeScopeId, HistogramScopeId});
}

TEST_CASE("The color picker asks nothing of the worker")
{
    // It reads the sampled cursor color, not worker output, so it
    // contributes no id to the enabled set.
    ScopeStack stack{registry()};
    stack.restore("C");
    CHECK(stack.enabledScopeIds().empty());
    // The scope is still on screen; only the worker is spared.
    CHECK(stack.shows(ColorPickerScopeId));
}

TEST_CASE("The stack round-trips through preference letters")
{
    ScopeStack stack{registry()};
    stack.restore("VWRHC");
    CHECK(stack.tokens() == "VWRHC");
    CHECK(stack.ids().size() == 5);

    SECTION("unknown letters are ignored")
    {
        stack.restore("VxH");
        CHECK(stack.tokens() == "VH");
    }

    SECTION("naming nothing valid falls back to the vectorscope")
    {
        stack.restore("zzz");
        CHECK(stack.tokens() == "V");
        stack.restore("");
        CHECK(stack.tokens() == "V");
    }
}

TEST_CASE("The stack reads scopes by bracketed id token")
{
    ScopeStack stack{registry()};
    stack.restore("[org.sidescopes.histogram][org.sidescopes.waveform]");
    CHECK(stack.ids() == std::vector<std::string>{HistogramScopeId, WaveformScopeId});
    // These built-ins carry letters, so they persist back as bare letters.
    CHECK(stack.tokens() == "HW");
}

TEST_CASE("A stack drops a letter the registry rejects but keeps a known id")
{
    // R11: letter validation runs through the registry, never a fixed string,
    // so an unknown letter falls out while a valid id token is kept.
    ScopeStack stack{registry()};
    stack.restore("Q[org.sidescopes.histogram]");
    CHECK(stack.ids() == std::vector<std::string>{HistogramScopeId});
}

TEST_CASE("A letterless scope survives a save as an id token")
{
    // The only carrier for a letterless scope is its id: it must both restore
    // from and persist back to a bracketed token, or it is silently dropped.
    const ScopeRegistry letterless = letterlessRegistry();
    const HostScope* scope = letterless.byId(LetterlessId);
    REQUIRE(scope != nullptr);
    REQUIRE(scope->letter == 0);

    ScopeStack stack{letterless};
    stack.restore(std::string("[") + LetterlessId + "]");
    REQUIRE(stack.ids() == std::vector<std::string>{LetterlessId});
    CHECK(stack.tokens() == std::string("[") + LetterlessId + "]");
}

}  // namespace sidescopes
