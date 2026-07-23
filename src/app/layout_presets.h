#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>

#include "app/layout_preset_store.h"
#include "core/analysis_worker.h"
#include "core/preferences.h"

namespace sidescopes {

class ScopeRegistry;
class ScopeView;

/// What one preset action asks of the host. The controller moves the view and
/// the stored slots itself; what travels here is the shell state only the host
/// can reach.
struct LayoutPresetOutcome
{
    /// The line for the status strip, empty when there is nothing to say.
    std::string status;
    /// The scopes on screen changed: the host pushes the settings to the
    /// worker.
    bool analysisDirty = false;
    /// A persisted value changed: the host schedules a preferences save.
    bool preferencesSaveDue = false;
};

/// Owns the layout preset slots and the capture and apply over them: what a
/// slot records of the live layout, whether the live layout has drifted from
/// the active slot, and the toolbar chip that loads and saves. It reads and
/// writes the view and the settings it is constructed with; the status line
/// and the persistence clocks travel back as a LayoutPresetOutcome the host
/// applies.
class LayoutPresetController
{
public:
    /// @p view is the live layout a preset is captured from and applied to,
    /// @p registry declares the choice parameters a preset recalls, and
    /// @p analysis holds those values and the enabled scope list. All three
    /// must outlive the controller.
    LayoutPresetController(ScopeView& view, const ScopeRegistry& registry, AnalysisSettings& analysis);

    /// Replaces the slots and the active slot, as loaded from preferences.
    void restore(const std::array<LayoutPreset, LayoutPresetSlots>& presets, int activeSlot);

    /// @return All slots, for the context menu to list and for persistence.
    [[nodiscard]] const std::array<LayoutPreset, LayoutPresetSlots>& all() const;

    /// @return The active slot (1-based), or 0 when none is active.
    [[nodiscard]] int activeSlot() const;

    /// Records the live layout in @p slot (1-based).
    [[nodiscard]] LayoutPresetOutcome save(int slot);

    /// Puts @p slot's (1-based) stored layout on screen - the stack, the
    /// split, the weights, and the styles. An empty slot only says so.
    [[nodiscard]] LayoutPresetOutcome load(int slot);

    /// The toolbar preset dropdown: the preview names the active slot
    /// (starred when dirty); the popup loads on click, saves on Shift+click.
    [[nodiscard]] LayoutPresetOutcome drawPicker();

private:
    /// The live layout as it would save into a preset slot.
    [[nodiscard]] LayoutPreset capture() const;

    /// Whether the live layout has drifted from the active preset slot; false
    /// when no preset is active.
    [[nodiscard]] bool activeDirty() const;

    [[nodiscard]] std::map<std::string, double> currentStackWeights() const;

    /// The stacked scopes' choice-parameter values - the style menus' state -
    /// for a preset to recall alongside the geometry.
    [[nodiscard]] std::map<std::string, std::map<std::string, double>> currentStackStyles() const;

    [[nodiscard]] const std::map<std::string, double>& paramsOf(std::string_view id) const;

    /// Applies a preset's stored choice values through the same write the
    /// style menus use, skipping keys the descriptors no longer declare and
    /// clamping each value to its parameter's range.
    void applyStyles(const std::map<std::string, std::map<std::string, double>>& styles);

    ScopeView& m_view;
    const ScopeRegistry& m_registry;
    AnalysisSettings& m_analysis;
    LayoutPresetStore m_store;
};

}  // namespace sidescopes
