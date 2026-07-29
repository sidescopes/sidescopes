#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "app/scope_order.h"
#include "app/scope_registry.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

// Three hand-built scopes, so no case depends on which modules this build
// registers - and, deliberately, NO vectorscope: the order must never invent
// the stack's fallback scope, which is the one difference between reading a
// token string as an order and reading it as a stack.
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
    AlphaId, "Alpha", 'A', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor BetaDescriptor{
    BetaId, "Beta", 'B', 0, 0, 0u, nullptr, 0u, 0.0f,
};

// Asks for the host-reserved 'C', so the registry turns it down and registers
// it letterless: its id is the only token that can carry it.
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

// The registry every case orders: immutable once built, so one serves the file.
// The host color picker is appended to whatever the modules registered, so the
// four scopes are Alpha, Beta, the letterless one, and the picker.
const ScopeRegistry& registry()
{
    static const ScopeRegistry instance{testModules()};

    return instance;
}

// Every registered scope id, in the order the registry states them.
std::vector<std::string> registered()
{
    std::vector<std::string> ids;
    for (const HostScope& scope : registry().scopes()) {
        ids.push_back(scope.id);
    }

    return ids;
}

// One scope as the preferences file spells it. Ids, never letters: a letter is
// handed out only if it is still free, so a collision would re-point a token
// already written.
std::string token(std::string_view id)
{
    return std::string("[") + std::string{id} + "]";
}

}  // namespace

TEST_CASE("A fresh order lists every scope in registration order")
{
    const ScopeOrder order{registry()};
    CHECK(order.ids() == registered());
    CHECK(order.ids().size() == 4);
}

TEST_CASE("A restored order leads with the scopes it names")
{
    ScopeOrder order{registry()};
    order.restore(token(BetaId) + token(AlphaId));
    // Named first, in the string's own sequence; the rest follow in
    // registration order, so a scope the file predates is never lost.
    REQUIRE(order.ids().size() == 4);
    CHECK(order.ids()[0] == BetaId);
    CHECK(order.ids()[1] == AlphaId);
    CHECK(order.ids()[2] == LetterlessId);
    CHECK(order.ids()[3] == ColorPickerScopeId);
}

TEST_CASE("An order names every scope even when its string names none")
{
    // The guard the order exists for: read as a STACK, a string naming nothing
    // valid falls back to the vectorscope - which this registry does not have,
    // so it would put a scope in the menu that cannot be drawn.
    ScopeOrder order{registry()};

    SECTION("an empty string")
    {
        order.restore("");
        CHECK(order.ids() == registered());
    }

    SECTION("nothing the registry knows")
    {
        order.restore("zzz");
        CHECK(order.ids() == registered());
    }

    SECTION("an unterminated bracket")
    {
        order.restore("[org.sidescopes.test");
        CHECK(order.ids() == registered());
    }

    CHECK(std::find(order.ids().begin(), order.ids().end(), VectorscopeScopeId) == order.ids().end());
}

TEST_CASE("A restored order drops what the registry does not know")
{
    ScopeOrder order{registry()};
    order.restore(token(BetaId) + token(AlphaId) + token("org.sidescopes.test.absent"));
    CHECK(order.ids() == std::vector<std::string>{BetaId, AlphaId, LetterlessId, ColorPickerScopeId});
}

TEST_CASE("A repeated scope takes its first place only")
{
    ScopeOrder order{registry()};
    order.restore(token(BetaId) + token(AlphaId) + token(BetaId));
    CHECK(order.ids() == std::vector<std::string>{BetaId, AlphaId, LetterlessId, ColorPickerScopeId});
}

TEST_CASE("An order round-trips through its tokens")
{
    ScopeOrder order{registry()};
    order.restore(token(BetaId) + token(AlphaId));
    // Every scope is named, so the string states the whole order rather than a
    // prefix of it.
    const std::string tokens = token(BetaId) + token(AlphaId) + token(LetterlessId) + token(ColorPickerScopeId);
    CHECK(order.tokens() == tokens);

    ScopeOrder reloaded{registry()};
    reloaded.restore(tokens);
    CHECK(reloaded.ids() == order.ids());
}

TEST_CASE("A scope the order does not name ranks last")
{
    ScopeOrder order{registry()};
    CHECK(order.rank(AlphaId) == std::size_t{0});
    CHECK(order.rank(ColorPickerScopeId) == std::size_t{3});
    // Not a registered scope: it sorts after everything rather than before it.
    CHECK(order.rank("org.sidescopes.test.absent") == order.ids().size());
}

TEST_CASE("Sorting seats known scopes and trails the rest")
{
    ScopeOrder order{registry()};
    order.restore(token(BetaId) + token(AlphaId));

    CHECK(order.sorted({AlphaId, BetaId}) == std::vector<std::string>{BetaId, AlphaId});

    // Unknown scopes rank alike, and the sort is stable, so they keep the
    // sequence they arrived in rather than being shuffled among themselves.
    const std::vector<std::string> mixed{"org.sidescopes.test.x", AlphaId, "org.sidescopes.test.y", BetaId};
    CHECK(order.sorted(mixed) ==
          std::vector<std::string>{BetaId, AlphaId, "org.sidescopes.test.x", "org.sidescopes.test.y"});
}

TEST_CASE("Moving a row lands it in the gap it was dropped in")
{
    ScopeOrder order{registry()};
    const std::vector<std::string> start = registered();
    REQUIRE(start.size() == 4);

    SECTION("down the list, past the rows it skips")
    {
        // Slot 3 is the gap between the third and fourth rows: removing the
        // first row shifts every later gap down by one, so the row lands third.
        CHECK(order.move(0, 3));
        CHECK(order.ids() == std::vector<std::string>{start[1], start[2], start[0], start[3]});
    }

    SECTION("up the list, where no slot shifts")
    {
        CHECK(order.move(3, 1));
        CHECK(order.ids() == std::vector<std::string>{start[0], start[3], start[1], start[2]});
    }

    SECTION("to the very end")
    {
        CHECK(order.move(0, 4));
        CHECK(order.ids() == std::vector<std::string>{start[1], start[2], start[3], start[0]});
    }
}

TEST_CASE("A move that changes nothing is refused")
{
    ScopeOrder order{registry()};
    const std::vector<std::string> start = registered();

    SECTION("either gap beside the row it came from")
    {
        CHECK_FALSE(order.move(1, 1));
        CHECK_FALSE(order.move(1, 2));
        CHECK(order.ids() == start);
    }

    SECTION("a row or a gap out of range")
    {
        CHECK_FALSE(order.move(-1, 0));
        CHECK_FALSE(order.move(4, 0));
        CHECK_FALSE(order.move(0, -1));
        CHECK_FALSE(order.move(0, 5));
        CHECK(order.ids() == start);
    }
}

}  // namespace sidescopes
