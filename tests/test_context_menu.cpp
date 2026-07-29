#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/attach_controller.h"
#include "app/context_menu.h"
#include "app/overlay_style.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/shortcut_resolver.h"
#include "app/ui_scaling.h"
#include "core/preferences.h"
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

// A whole menu built over a live view, so a case can read what the entries say
// rather than only what their ids decode to.
struct MenuUnderTest
{
    ScopeView view{registry()};
    ShortcutResolver shortcuts{registry()};
    std::map<std::string, std::map<std::string, double>> scopeParams;
    AttachController attach;
    std::array<LayoutPreset, LayoutPresetSlots> presets;
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> paramActions;

    // Builds the background menu (no pane clicked), which carries every list.
    void build()
    {
        const ContextMenuModel model{
            view, registry(), shortcuts, scopeParams, attach, presets, true, 0, 1.0f, QualityLevel::Standard, false};
        items.clear();
        paramActions.clear();
        buildContextMenu(model, -1, items, paramActions);
    }

    // The action labels directly inside the submenu named @p title, in the
    // order the menu states them; entries of any nested submenu are skipped.
    [[nodiscard]] std::vector<std::string> submenu(const char* title) const
    {
        std::vector<std::string> labels;
        int depth = 0;
        for (const NativeMenuItem& item : items) {
            if (depth == 0) {
                depth = item.kind == NativeMenuItem::Kind::SubmenuBegin && item.label == title ? 1 : 0;
                continue;
            }
            if (item.kind == NativeMenuItem::Kind::SubmenuBegin) {
                ++depth;
            } else if (item.kind == NativeMenuItem::Kind::SubmenuEnd && --depth == 0) {
                break;
            } else if (depth == 1 && item.kind == NativeMenuItem::Kind::Action) {
                labels.push_back(item.label);
            }
        }

        return labels;
    }
};

// A scope's name as every menu spells it.
const char* menuName(const HostScope& scope)
{
    return scope.descriptor != nullptr ? scope.descriptor->name : "Color Picker";
}

}  // namespace

TEST_CASE("The scopes submenu lists them in the user's own order")
{
    // Both scope lists read the same: the toolbar selector and this one are
    // the same choice offered twice, so one order settles both.
    MenuUnderTest menu;
    menu.build();
    const std::vector<std::string> registered = menu.submenu("Scopes");
    REQUIRE(registered.size() == registry().scopes().size());

    const int last = static_cast<int>(registered.size()) - 1;
    REQUIRE(menu.view.reorderScopes(last, 0));
    menu.build();
    const std::vector<std::string> moved = menu.submenu("Scopes");
    REQUIRE(moved.size() == registered.size());
    CHECK(moved.front() == registered.back());
    CHECK(moved[1] == registered.front());
}

TEST_CASE("A reordered scopes submenu still toggles the scope it names")
{
    // The entry ids are registry indices, which no reorder touches: moving a
    // row must not hand its click to the scope that used to be there.
    MenuUnderTest menu;
    const int last = static_cast<int>(registry().scopes().size()) - 1;
    REQUIRE(menu.view.reorderScopes(last, 0));
    menu.build();

    int checked = 0;
    for (const NativeMenuItem& item : menu.items) {
        if (item.actionId < MenuShowScopeBase || item.actionId >= MenuGraticuleBase) {
            continue;
        }
        const std::optional<std::string> toggled = menuScopeToggle(item.actionId, registry());
        REQUIRE(toggled);
        const HostScope* scope = registry().byId(*toggled);
        REQUIRE(scope != nullptr);
        INFO("entry " << item.label);
        CHECK(item.label == menuName(*scope));
        ++checked;
    }
    CHECK(checked == static_cast<int>(registry().scopes().size()));
}

TEST_CASE("The preset menus name their slots rather than spell them out")
{
    MenuUnderTest menu;
    menu.presets[0].stack = "VWH";
    menu.presets[0].orientation = 1;
    menu.presets[2].name = "Skin tones";
    menu.build();

    const std::vector<std::string> loads = menu.submenu("Presets");
    REQUIRE(loads.size() == static_cast<std::size_t>(LayoutPresetSlots));
    // A stack summary is not a name: "VWH Vertical" told the user nothing they
    // had not already chosen.
    CHECK(loads[0] == "Preset 1");
    CHECK(loads[2] == "Skin tones (empty)");
    CHECK(loads[8] == "Preset 9 (empty)");

    // There is one list, not two. Saving was a second copy of these same nine
    // names and went with the explicit save it existed for.
    CHECK(menu.submenu("Save Current To").empty());
}

TEST_CASE("The graticule steps are offered as words, not percentages")
{
    // What the user picks is how the lines read over a trace; a percentage
    // states the arithmetic instead and leaves them to work out which is
    // heavier. No step is marked as the default: Normal is the word for it,
    // and a checkmark already says where the user is.
    MenuUnderTest menu;
    menu.build();

    CHECK(menu.submenu("Graticule") == std::vector<std::string>{"Faint", "Soft", "Normal", "Bold"});
}

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
    }
    // Slot zero is no slot: the base itself names nothing.
    CHECK_FALSE(menuShortcutAction(MenuLoadPresetBase));
    CHECK_FALSE(menuShortcutAction(MenuLoadPresetBase + LayoutPresetSlots + 1));
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
