#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "app/context_menu.h"
#include "app/overlay_style.h"
#include "app/scope_registry.h"
#include "app/ui_scaling.h"
#include "modules/module_registry.h"

namespace sidescopes {
namespace {

const ScopeRegistry& registry()
{
    static const ScopeRegistry instance{builtinModules()};

    return instance;
}

// How many of the pure decoders claim @p chosen. The ranges are laid out to be
// disjoint, so this is never more than one.
int claimsOf(int chosen)
{
    int claims = 0;
    const std::vector<ParamMenuAction> noParams;
    claims += menuScopeParam(chosen, noParams) != nullptr ? 1 : 0;
    claims += menuScopeToggle(chosen, registry()) ? 1 : 0;
    claims += menuShortcutAction(chosen) ? 1 : 0;
    claims += menuGraticuleStrength(chosen) ? 1 : 0;
    claims += menuOrientation(chosen) ? 1 : 0;
    claims += menuUiScaleStep(chosen) ? 1 : 0;
    claims += menuQuality(chosen) ? 1 : 0;

    return claims;
}

}  // namespace

TEST_CASE("No menu id means two things at once")
{
    // Past the last fixed range every id is a scope parameter, so the sweep
    // stops where the dynamic table takes over.
    for (int chosen = 0; chosen < ParamMenuActionBase; ++chosen) {
        INFO("menu id " << chosen);
        CHECK(claimsOf(chosen) <= 1);
    }
}

TEST_CASE("The diagnostics entries belong to no other decoder")
{
    for (const int chosen :
         {MenuToggleCaptureVisibility, MenuToggleDiagRecording, MenuShowDiagLog, MenuResetDiagnostics}) {
        INFO("menu id " << chosen);
        CHECK(claimsOf(chosen) == 0);
    }
}

TEST_CASE("An id in no range at all is nobody's")
{
    CHECK(claimsOf(0) == 0);
    CHECK(claimsOf(MenuDrawRegion - 1) == 0);
    CHECK(applyDiagnosticsMenu(0) == false);
    CHECK(applyDiagnosticsMenu(MenuAbout) == false);
}

TEST_CASE("Every registered scope has a toggle id, and nothing beyond has one")
{
    const std::vector<HostScope>& scopes = registry().scopes();
    for (std::size_t index = 0; index < scopes.size(); ++index) {
        const auto id = menuScopeToggle(MenuShowScopeBase + static_cast<int>(index), registry());
        REQUIRE(id);
        CHECK(*id == scopes[index].id);
    }
    CHECK_FALSE(menuScopeToggle(MenuShowScopeBase - 1, registry()));
    CHECK_FALSE(menuScopeToggle(MenuShowScopeBase + static_cast<int>(scopes.size()), registry()));
}

TEST_CASE("The stepped ranges decode to the step they were built from")
{
    for (std::size_t step = 0; step < GraticuleStrengths.size(); ++step) {
        const auto strength = menuGraticuleStrength(MenuGraticuleBase + static_cast<int>(step));
        REQUIRE(strength);
        CHECK(*strength == GraticuleStrengths[step]);
    }
    CHECK_FALSE(menuGraticuleStrength(MenuGraticuleBase - 1));
    CHECK_FALSE(menuGraticuleStrength(MenuGraticuleBase + static_cast<int>(GraticuleStrengths.size())));

    for (std::size_t step = 0; step < UiScaleSteps.size(); ++step) {
        const auto chosen = menuUiScaleStep(MenuUiScaleBase + static_cast<int>(step));
        REQUIRE(chosen);
        CHECK(*chosen == static_cast<int>(step));
    }
    CHECK_FALSE(menuUiScaleStep(MenuUiScaleBase - 1));
    CHECK_FALSE(menuUiScaleStep(MenuUiScaleBase + static_cast<int>(UiScaleSteps.size())));

    for (std::size_t step = 0; step < QualityLevels.size(); ++step) {
        const auto level = menuQuality(MenuQualityBase + static_cast<int>(step));
        REQUIRE(level);
        CHECK(*level == QualityLevels[step]);
    }
    CHECK_FALSE(menuQuality(MenuQualityBase - 1));
    CHECK_FALSE(menuQuality(MenuQualityBase + static_cast<int>(QualityLevels.size())));
}

TEST_CASE("The region tools mean what their keys mean")
{
    const auto attach = menuShortcutAction(MenuAttachWindow);
    REQUIRE(attach);
    CHECK(attach->kind == ShortcutAction::Kind::RequestPick);
    CHECK(attach->pickMode == RegionPickerMode::AttachWindow);

    const auto draw = menuShortcutAction(MenuDrawRegion);
    REQUIRE(draw);
    CHECK(draw->pickMode == RegionPickerMode::DrawGlobal);

    const auto face = menuShortcutAction(MenuAttachFace);
    REQUIRE(face);
    CHECK(face->pickMode == RegionPickerMode::AttachFace);

    const auto pin = menuShortcutAction(MenuPinColor);
    REQUIRE(pin);
    CHECK(pin->pickMode == RegionPickerMode::PinColor);
}

TEST_CASE("Clear Region and Detach All both clear every region")
{
    const auto clear = menuShortcutAction(MenuClearRegion);
    REQUIRE(clear);
    CHECK(clear->kind == ShortcutAction::Kind::ClearRegion);

    const auto detachAll = menuShortcutAction(MenuDetachAll);
    REQUIRE(detachAll);
    CHECK(detachAll->kind == ShortcutAction::Kind::ClearRegion);
}

TEST_CASE("The zoom entries set their level outright")
{
    const std::array<std::pair<int, int>, 3> entries{std::pair{MenuZoom1, 1}, std::pair{MenuZoom2, 2},
                                                     std::pair{MenuZoom4, 4}};
    for (const auto& [chosen, level] : entries) {
        const auto action = menuShortcutAction(chosen);
        REQUIRE(action);
        CHECK(action->kind == ShortcutAction::Kind::SetZoom);
        CHECK(action->zoomLevel == level);
    }
}

TEST_CASE("Every preset slot loads and saves through its own id")
{
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const auto load = menuShortcutAction(MenuLoadPresetBase + slot);
        REQUIRE(load);
        CHECK(load->kind == ShortcutAction::Kind::LoadPreset);
        CHECK(load->presetSlot == slot);

        const auto save = menuShortcutAction(MenuSavePresetBase + slot);
        REQUIRE(save);
        CHECK(save->kind == ShortcutAction::Kind::SavePreset);
        CHECK(save->presetSlot == slot);
    }
    // Slot zero is no slot: the bases themselves name nothing.
    CHECK_FALSE(menuShortcutAction(MenuLoadPresetBase));
    CHECK_FALSE(menuShortcutAction(MenuSavePresetBase));
    CHECK_FALSE(menuShortcutAction(MenuLoadPresetBase + LayoutPresetSlots + 1));
    CHECK_FALSE(menuShortcutAction(MenuSavePresetBase + LayoutPresetSlots + 1));
}

TEST_CASE("The three splits decode to the orientation they name")
{
    REQUIRE(menuOrientation(MenuLayoutAuto));
    CHECK(*menuOrientation(MenuLayoutAuto) == LayoutOrientation::Automatic);
    REQUIRE(menuOrientation(MenuLayoutVertical));
    CHECK(*menuOrientation(MenuLayoutVertical) == LayoutOrientation::Vertical);
    REQUIRE(menuOrientation(MenuLayoutHorizontal));
    CHECK(*menuOrientation(MenuLayoutHorizontal) == LayoutOrientation::Horizontal);
}

TEST_CASE("A scope parameter resolves through the table its open built")
{
    std::vector<ParamMenuAction> actions;
    actions.push_back(ParamMenuAction{"org.sidescopes.waveform", "mode", 2.0});
    actions.push_back(ParamMenuAction{"org.sidescopes.histogram", "style", 1.0});

    const ParamMenuAction* first = menuScopeParam(ParamMenuActionBase, actions);
    REQUIRE(first != nullptr);
    CHECK(first->scopeId == "org.sidescopes.waveform");
    CHECK(first->value == 2.0);

    const ParamMenuAction* second = menuScopeParam(ParamMenuActionBase + 1, actions);
    REQUIRE(second != nullptr);
    CHECK(second->paramKey == "style");

    // A stale id past the table this open built resolves to nothing rather
    // than reading off the end of it.
    CHECK(menuScopeParam(ParamMenuActionBase + 2, actions) == nullptr);
    CHECK(menuScopeParam(ParamMenuActionBase - 1, actions) == nullptr);
}

}  // namespace sidescopes
