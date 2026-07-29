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
/// slot records of the live layout, and the writing back that keeps the active
/// slot equal to it. It reads and writes the view and the settings it is
/// constructed with; the status line and the persistence clocks travel back as
/// a LayoutPresetOutcome the host applies. The picker that draws all this is a
/// class of its own, so nothing here depends on the toolkit.
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

    /// @return What @p slot (1-based) holds, empty stack and all.
    [[nodiscard]] const LayoutPreset& at(int slot) const;

    /// @return The active slot, always one of the nine: the application is
    ///         always on a preset.
    [[nodiscard]] int activeSlot() const;

    /// Writes the live layout into the active slot if it has moved since the
    /// last look. Called once a frame: there is no explicit save, so a slot IS
    /// whatever is on screen while you are on it.
    /// @return Whether anything was written, so the host can schedule the
    ///         preferences file's own debounced write.
    [[nodiscard]] bool syncActiveSlot();

    /// Copies the live layout into @p slot (1-based) and STAYS WHERE IT IS.
    ///
    /// The keyboard's Shift+digit, and deliberately not in the interface. It
    /// is the one way to make a stable reference point under auto-save: every
    /// slot you are ON keeps changing under you, so stamping the current
    /// arrangement into a slot you are NOT on is what leaves something that
    /// holds still. Following the copy would defeat it - you would land on the
    /// copy and immediately start editing it. The destination keeps its own
    /// name; a name outlives the layout it was given to.
    [[nodiscard]] LayoutPresetOutcome copyInto(int slot);

    /// Puts @p slot's (1-based) layout on screen - the stack, the split, the
    /// weights, and the styles - and makes it the active slot. A slot holding
    /// nothing yet restores @ref defaultLayout, so no click on a slot is ever
    /// refused.
    ///
    /// The slot being LEFT keeps whatever was on screen: it was written there
    /// as it was arranged, so switching away is not a discard and there is
    /// nothing to confirm.
    [[nodiscard]] LayoutPresetOutcome load(int slot);

    /// Calls @p slot (1-based) @p typed, or puts it back on its default name
    /// when that is what was typed.
    [[nodiscard]] LayoutPresetOutcome rename(int slot, std::string_view typed);

    /// The arrangement the application opens on and a slot holding nothing
    /// restores: the vectorscope alone, split automatically, at the styles its
    /// module declares. Built to the shape @ref capture produces, so a slot
    /// restored from it reads back identical and needs no writing back.
    [[nodiscard]] LayoutPreset defaultLayout() const;

private:
    /// The live layout as it would save into a preset slot.
    [[nodiscard]] LayoutPreset capture() const;

    [[nodiscard]] std::map<std::string, double> currentStackWeights() const;

    /// The stacked scopes' choice-parameter values - the style menus' state -
    /// for a preset to recall alongside the geometry.
    [[nodiscard]] std::map<std::string, std::map<std::string, double>> currentStackStyles() const;

    /// @p scopeId's choice parameters at the values its own descriptor
    /// declares, which is what the scope shows before anything is chosen for
    /// it.
    [[nodiscard]] std::map<std::string, double> declaredStyles(std::string_view scopeId) const;

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
