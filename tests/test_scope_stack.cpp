#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "app/scope_order.h"
#include "app/scope_registry.h"
#include "app/scope_stack.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"
#include "support/scope_tokens.h"

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

TEST_CASE("A stack starts on the vectorscope and RGB waveform")
{
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    CHECK(stack.shows(VectorscopeScopeId));
    CHECK(stack.shows(WaveformScopeId));
    CHECK_FALSE(stack.shows(HistogramScopeId));
    CHECK(stack.ids().size() == 2);
}

TEST_CASE("Toggling adds a scope and reports it newly visible")
{
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    CHECK(stack.toggle(HistogramScopeId));
    CHECK(stack.shows(HistogramScopeId));
    CHECK(stack.ids().size() == 3);
    // Toggling it back off is not an activation.
    CHECK_FALSE(stack.toggle(HistogramScopeId));
    CHECK_FALSE(stack.shows(HistogramScopeId));
}

TEST_CASE("The last scope cannot be toggled away")
{
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    stack.restore(testing::idTokens("V"));
    REQUIRE(stack.ids().size() == 1);
    CHECK_FALSE(stack.toggle(VectorscopeScopeId));
    CHECK(stack.ids().size() == 1);
    CHECK(stack.shows(VectorscopeScopeId));
}

TEST_CASE("Choosing solos a scope unless stacking")
{
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    stack.restore(testing::idTokens("V"));
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

TEST_CASE("The panes take the preferred order, not the order switched on")
{
    // The whole point of a stable menu: a scope brought back returns to its
    // place rather than to the end, so checking and unchecking several never
    // rearranges the panes underneath.
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    order.restore(testing::idTokens("HWV"));
    stack.restore(testing::idTokens("V"));

    stack.toggle(WaveformScopeId);
    CHECK(stack.ids() == std::vector<std::string>{WaveformScopeId, VectorscopeScopeId});
    stack.toggle(HistogramScopeId);
    CHECK(stack.ids() == std::vector<std::string>{HistogramScopeId, WaveformScopeId, VectorscopeScopeId});
    CHECK(stack.tokens() == testing::idTokens("HWV"));

    // Off and on again lands in the same seat, not at the end.
    stack.toggle(WaveformScopeId);
    stack.toggle(WaveformScopeId);
    CHECK(stack.ids() == std::vector<std::string>{HistogramScopeId, WaveformScopeId, VectorscopeScopeId});
}

TEST_CASE("Restoring a stack seats it in the preferred order")
{
    // The token string a preset or an older preferences file carries states
    // which scopes, not where they sit: the order answers that.
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    order.restore(testing::idTokens("HWV"));
    stack.restore(testing::idTokens("VWH"));
    CHECK(stack.ids() == std::vector<std::string>{HistogramScopeId, WaveformScopeId, VectorscopeScopeId});
}

TEST_CASE("A change to the preferred order re-seats the panes")
{
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    stack.restore(testing::idTokens("VWH"));
    const std::vector<std::string> original{VectorscopeScopeId, WaveformScopeId, HistogramScopeId};
    REQUIRE(stack.ids() == original);

    // The histogram is dragged to the front of the menu; the panes follow.
    REQUIRE(order.move(static_cast<int>(order.rank(HistogramScopeId)), 0));
    stack.applyOrder();
    CHECK(stack.ids() == std::vector<std::string>{HistogramScopeId, VectorscopeScopeId, WaveformScopeId});
    CHECK(stack.tokens() == testing::idTokens("HVW"));
}

TEST_CASE("The enabled ids cover the whole stack")
{
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    stack.restore(testing::idTokens("V"));
    CHECK(stack.enabledScopeIds() == std::vector<std::string>{VectorscopeScopeId});

    stack.restore(testing::idTokens("VH"));
    CHECK(stack.enabledScopeIds() == std::vector<std::string>{VectorscopeScopeId, HistogramScopeId});
}

TEST_CASE("The color picker asks nothing of the worker")
{
    // It reads the sampled cursor color, not worker output, so it
    // contributes no id to the enabled set.
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    stack.restore(testing::idTokens("C"));
    CHECK(stack.enabledScopeIds().empty());
    // The scope is still on screen; only the worker is spared.
    CHECK(stack.shows(ColorPickerScopeId));
}

TEST_CASE("The stack round-trips through its preference tokens")
{
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    stack.restore(testing::idTokens("VWRHC"));
    CHECK(stack.tokens() == testing::idTokens("VWRHC"));
    CHECK(stack.ids().size() == 5);

    SECTION("unknown tokens are ignored")
    {
        stack.restore(testing::idTokens("V") + "[org.sidescopes.absent]" + testing::idTokens("H"));
        CHECK(stack.tokens() == testing::idTokens("VH"));
    }

    SECTION("naming nothing valid falls back to the default pair")
    {
        stack.restore("zzz");
        CHECK(stack.tokens() == testing::idTokens("VW"));
        stack.restore(testing::idTokens(""));
        CHECK(stack.tokens() == testing::idTokens("VW"));
    }
}

TEST_CASE("The stack reads scopes by bracketed id token")
{
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
    stack.restore("[org.sidescopes.histogram][org.sidescopes.waveform]");
    // Which scopes is the token string's answer; where they sit is the
    // preferred order's, which here is still the registration order.
    CHECK(stack.ids() == std::vector<std::string>{WaveformScopeId, HistogramScopeId});
    CHECK(stack.tokens() == testing::idTokens("WH"));
}

TEST_CASE("A stack drops a token the registry rejects but keeps a known id")
{
    // R11: letter validation runs through the registry, never a fixed string,
    // so an unknown letter falls out while a valid id token is kept.
    ScopeOrder order{registry()};
    ScopeStack stack{registry(), order};
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

    ScopeOrder order{letterless};
    ScopeStack stack{letterless, order};
    stack.restore(std::string("[") + LetterlessId + "]");
    REQUIRE(stack.ids() == std::vector<std::string>{LetterlessId});
    CHECK(stack.tokens() == std::string("[") + LetterlessId + "]");
}

TEST_CASE("A stack remains drawable when the default modules are unavailable")
{
    const ModuleRegistry modules;
    const ScopeRegistry available{modules};
    ScopeOrder order{available};
    ScopeStack stack{available, order};
    CHECK(stack.ids() == std::vector<std::string>{ColorPickerScopeId});
    CHECK_FALSE(stack.choose(VectorscopeScopeId, false));
    CHECK_FALSE(stack.toggle("org.example.absent"));
    CHECK(stack.ids() == std::vector<std::string>{ColorPickerScopeId});
    stack.restore("[org.example.absent]");
    CHECK(stack.ids() == std::vector<std::string>{ColorPickerScopeId});
}

}  // namespace sidescopes
