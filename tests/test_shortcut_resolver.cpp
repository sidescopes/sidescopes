#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "app/scope_registry.h"
#include "app/shortcut_resolver.h"
#include "modules/module_registry.h"
#include "platform/desktop.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

// Two hand-built scopes on the letters the shipped stack uses, so the cases
// below never depend on which modules this build registers. The host color
// picker joins them on 'C' by the registry's own hand.
constexpr char AlphaId[] = "org.sidescopes.test.alpha";
constexpr char BetaId[] = "org.sidescopes.test.beta";

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

const SsScopeDescriptor AlphaDescriptor{
    AlphaId, "Alpha", 'V', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor* alphaDescriptor(uint32_t index)
{
    return index == 0 ? &AlphaDescriptor : nullptr;
}

const SsModuleEntry AlphaModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, alphaDescriptor, nullCreate,
};

const SsScopeDescriptor BetaDescriptor{
    BetaId, "Beta", 'W', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor* betaDescriptor(uint32_t index)
{
    return index == 0 ? &BetaDescriptor : nullptr;
}

const SsModuleEntry BetaModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, betaDescriptor, nullCreate,
};

// The two modules, registered once into a registry that outlives every
// resolver below.
const ModuleRegistry& testModules()
{
    static ModuleRegistry modules;
    static const bool registered = [] {
        (void)modules.registerModule(AlphaModuleEntry);
        (void)modules.registerModule(BetaModuleEntry);

        return true;
    }();
    (void)registered;

    return modules;
}

// The registry the resolver reads: immutable once built, so one instance
// serves the whole file.
const ScopeRegistry& registry()
{
    static const ScopeRegistry instance{testModules()};

    return instance;
}

// A resolver on the default bindings: A attach, D draw, F face, P pin, Z zoom,
// Escape full screen, and the scope letters from the registry.
ShortcutResolver defaultResolver()
{
    ShortcutResolver resolver{registry()};
    resolver.restore(ShortcutBindings{}, {});

    return resolver;
}

// A key probe reporting exactly @p key down this frame.
ShortcutKeyPressed pressing(std::string_view key)
{
    return [key](std::string_view name) { return name == key; };
}

// Everything available: faces detectable, a scope taking pins on screen, no
// settings window, and every platform window chord in force, so a case that
// cares about one of them turns that one off.
ShortcutContext readyContext()
{
    ShortcutContext context;
    context.faceDetectionSupported = true;
    context.pinsAvailable = true;
    context.hidesWindowOnCommandW = true;
    context.minimizesWindowOnControlW = true;
    context.quitsOnControlQ = true;

    return context;
}

}  // namespace

TEST_CASE("A scope letter shows that scope alone")
{
    const ShortcutResolver resolver = defaultResolver();
    const ShortcutAction action = resolver.resolvePressed(readyContext(), ModifierState{}, pressing("V"));

    REQUIRE(action.kind == ShortcutAction::Kind::ChooseScope);
    CHECK(action.scopeId == AlphaId);
    // The letters choose where the menu toggles: a plain press replaces the
    // stack rather than adding to it.
    CHECK_FALSE(action.stack);
}

TEST_CASE("Shift on a scope letter stacks instead of replacing")
{
    const ShortcutResolver resolver = defaultResolver();
    ModifierState modifiers;
    modifiers.shift = true;
    const ShortcutAction action = resolver.resolvePressed(readyContext(), modifiers, pressing("W"));

    REQUIRE(action.kind == ShortcutAction::Kind::ChooseScope);
    CHECK(action.scopeId == BetaId);
    CHECK(action.stack);
}

TEST_CASE("A per-scope override replaces the registry letter")
{
    ShortcutResolver resolver{registry()};
    resolver.restore(ShortcutBindings{}, {{AlphaId, "Q"}});

    CHECK(resolver.bindingFor(AlphaId) == "Q");
    CHECK(resolver.bindingFor(BetaId) == "W");
    // The letter the override left behind no longer reaches its scope.
    CHECK(resolver.resolvePressed(readyContext(), ModifierState{}, pressing("V")).kind == ShortcutAction::Kind::None);

    const ShortcutAction action = resolver.resolvePressed(readyContext(), ModifierState{}, pressing("Q"));
    REQUIRE(action.kind == ShortcutAction::Kind::ChooseScope);
    CHECK(action.scopeId == AlphaId);
}

TEST_CASE("A system chord silences the plain keys")
{
    const ShortcutResolver resolver = defaultResolver();
    ShortcutContext context = readyContext();
    // Option is nobody's window chord, so only the silencing is under test.
    ModifierState modifiers;
    modifiers.option = true;

    CHECK(resolver.resolvePressed(context, modifiers, pressing("V")).kind == ShortcutAction::Kind::None);

    // Command and Control silence them just the same, once their own chords
    // have had their look at the press.
    context.hidesWindowOnCommandW = false;
    context.minimizesWindowOnControlW = false;
    context.quitsOnControlQ = false;
    ModifierState command;
    command.command = true;
    CHECK(resolver.resolvePressed(context, command, pressing("V")).kind == ShortcutAction::Kind::None);
    ModifierState control;
    control.control = true;
    CHECK(resolver.resolvePressed(context, control, pressing("D")).kind == ShortcutAction::Kind::None);
}

TEST_CASE("Text input silences every shortcut")
{
    const ShortcutResolver resolver = defaultResolver();
    ShortcutContext context = readyContext();
    context.wantsTextInput = true;
    ModifierState modifiers;
    modifiers.command = true;

    CHECK(resolver.resolvePressed(context, ModifierState{}, pressing("V")).kind == ShortcutAction::Kind::None);
    CHECK(resolver.resolvePressed(context, ModifierState{}, pressing("3")).kind == ShortcutAction::Kind::None);
    // The window chords stand down with the rest of them.
    CHECK(resolver.resolvePressed(context, modifiers, pressing("W")).kind == ShortcutAction::Kind::None);
}

TEST_CASE("Each region key opens its own tool")
{
    const ShortcutResolver resolver = defaultResolver();
    const ShortcutContext context = readyContext();

    const ShortcutAction attach = resolver.resolvePressed(context, ModifierState{}, pressing("A"));
    REQUIRE(attach.kind == ShortcutAction::Kind::RequestPick);
    CHECK(attach.pickMode == RegionPickerMode::AttachWindow);

    const ShortcutAction draw = resolver.resolvePressed(context, ModifierState{}, pressing("D"));
    REQUIRE(draw.kind == ShortcutAction::Kind::RequestPick);
    CHECK(draw.pickMode == RegionPickerMode::DrawGlobal);

    const ShortcutAction face = resolver.resolvePressed(context, ModifierState{}, pressing("F"));
    REQUIRE(face.kind == ShortcutAction::Kind::RequestPick);
    CHECK(face.pickMode == RegionPickerMode::AttachFace);

    const ShortcutAction pin = resolver.resolvePressed(context, ModifierState{}, pressing("P"));
    REQUIRE(pin.kind == ShortcutAction::Kind::RequestPick);
    CHECK(pin.pickMode == RegionPickerMode::PinColor);
}

TEST_CASE("The face key stands down where faces cannot be detected")
{
    const ShortcutResolver resolver = defaultResolver();
    ShortcutContext context = readyContext();
    context.faceDetectionSupported = false;

    CHECK(resolver.resolvePressed(context, ModifierState{}, pressing("F")).kind == ShortcutAction::Kind::None);
    // The rest of the tools are unaffected.
    CHECK(resolver.resolvePressed(context, ModifierState{}, pressing("D")).kind == ShortcutAction::Kind::RequestPick);
}

TEST_CASE("The pin key stands down without a scope that takes pins")
{
    const ShortcutResolver resolver = defaultResolver();
    ShortcutContext context = readyContext();
    context.pinsAvailable = false;

    CHECK(resolver.resolvePressed(context, ModifierState{}, pressing("P")).kind == ShortcutAction::Kind::None);
}

TEST_CASE("The zoom key cycles through the magnifications")
{
    const ShortcutResolver resolver = defaultResolver();
    ShortcutContext context = readyContext();

    context.vectorscopeZoom = 1;
    const ShortcutAction first = resolver.resolvePressed(context, ModifierState{}, pressing("Z"));
    REQUIRE(first.kind == ShortcutAction::Kind::SetZoom);
    CHECK(first.zoomLevel == 2);

    context.vectorscopeZoom = 2;
    CHECK(resolver.resolvePressed(context, ModifierState{}, pressing("Z")).zoomLevel == 4);

    // The top of the cycle wraps back to unmagnified.
    context.vectorscopeZoom = 4;
    CHECK(resolver.resolvePressed(context, ModifierState{}, pressing("Z")).zoomLevel == 1);
}

TEST_CASE("The full-screen key peels the settings window first")
{
    const ShortcutResolver resolver = defaultResolver();
    ShortcutContext context = readyContext();

    context.settingsOpen = true;
    CHECK(resolver.resolvePressed(context, ModifierState{}, pressing("Escape")).kind ==
          ShortcutAction::Kind::CloseSettings);

    // With nothing stacked above them, the regions are what the key drops.
    context.settingsOpen = false;
    CHECK(resolver.resolvePressed(context, ModifierState{}, pressing("Escape")).kind ==
          ShortcutAction::Kind::ResetToFullScreen);
}

TEST_CASE("A preset digit loads its slot and Shift saves into it")
{
    const ShortcutResolver resolver = defaultResolver();
    const ShortcutContext context = readyContext();

    const ShortcutAction load = resolver.resolvePressed(context, ModifierState{}, pressing("3"));
    REQUIRE(load.kind == ShortcutAction::Kind::LoadPreset);
    CHECK(load.presetSlot == 3);

    ModifierState modifiers;
    modifiers.shift = true;
    const ShortcutAction save = resolver.resolvePressed(context, modifiers, pressing("7"));
    REQUIRE(save.kind == ShortcutAction::Kind::SavePreset);
    CHECK(save.presetSlot == 7);
}

TEST_CASE("The window chords follow the platform that owns them")
{
    const ShortcutResolver resolver = defaultResolver();
    ShortcutContext context = readyContext();
    ModifierState command;
    command.command = true;
    ModifierState control;
    control.control = true;

    CHECK(resolver.resolvePressed(context, command, pressing("W")).kind == ShortcutAction::Kind::HideApplication);
    CHECK(resolver.resolvePressed(context, command, pressing("Comma")).kind == ShortcutAction::Kind::OpenSettings);
    CHECK(resolver.resolvePressed(context, control, pressing("W")).kind == ShortcutAction::Kind::MinimizeWindow);
    CHECK(resolver.resolvePressed(context, control, pressing("Q")).kind == ShortcutAction::Kind::QuitWindow);

    // Where the platform does not claim a chord, the press means nothing.
    context.hidesWindowOnCommandW = false;
    context.minimizesWindowOnControlW = false;
    context.quitsOnControlQ = false;
    CHECK(resolver.resolvePressed(context, command, pressing("W")).kind == ShortcutAction::Kind::None);
    CHECK(resolver.resolvePressed(context, command, pressing("Comma")).kind == ShortcutAction::Kind::None);
    CHECK(resolver.resolvePressed(context, control, pressing("W")).kind == ShortcutAction::Kind::None);
    CHECK(resolver.resolvePressed(context, control, pressing("Q")).kind == ShortcutAction::Kind::None);
}

TEST_CASE("A forwarded key resolves through the same map")
{
    const ShortcutResolver resolver = defaultResolver();
    const ShortcutContext context = readyContext();

    const ShortcutAction scope = resolver.resolveNamed("W", /*shift=*/true, context);
    REQUIRE(scope.kind == ShortcutAction::Kind::ChooseScope);
    CHECK(scope.scopeId == BetaId);
    CHECK(scope.stack);

    const ShortcutAction tool = resolver.resolveNamed("A", false, context);
    REQUIRE(tool.kind == ShortcutAction::Kind::RequestPick);
    CHECK(tool.pickMode == RegionPickerMode::AttachWindow);

    // A key bound to nothing matched nothing, which is what None says.
    CHECK(resolver.resolveNamed("K", false, context).kind == ShortcutAction::Kind::None);
}

TEST_CASE("The bindings survive the round trip to preferences")
{
    ShortcutBindings bindings;
    bindings.drawRegion = "G";
    ShortcutResolver resolver{registry()};
    resolver.restore(bindings, {{BetaId, "B"}});

    CHECK(resolver.bindings().drawRegion == "G");
    CHECK(resolver.scopeOverrides().at(BetaId) == "B");
    // The rebound key is the one the tool now answers to.
    CHECK(resolver.resolvePressed(readyContext(), ModifierState{}, pressing("G")).kind ==
          ShortcutAction::Kind::RequestPick);
    CHECK(resolver.resolvePressed(readyContext(), ModifierState{}, pressing("D")).kind == ShortcutAction::Kind::None);
}

}  // namespace sidescopes
