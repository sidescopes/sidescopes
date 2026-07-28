#include "app/layout_presets.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "app/imgui_ui.h"
#include "app/menu_rows.h"
#include "app/param_menu.h"
#include "app/scope_layout.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "imgui.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

// The width every row of the picker shares, from the widest name it will show,
// so the names left-align and the digits right-align to one edge with room to
// breathe between. Measured from the name column rightward, which is where the
// row's Selectable starts.
float presetRowWidth(const std::array<LayoutPreset, LayoutPresetSlots>& presets)
{
    float maxName = 0.0f;
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const std::string name = presetDisplayName(slot, presets[static_cast<std::size_t>(slot - 1)]);
        maxName = std::max(maxName, ImGui::CalcTextSize(name.c_str()).x);
    }
    const float em = ImGui::GetFontSize();

    return maxName + em * 2.0f + ImGui::CalcTextSize("9").x + em * 0.75f;
}

}  // namespace

LayoutPresetController::LayoutPresetController(ScopeView& view, const ScopeRegistry& registry,
                                               AnalysisSettings& analysis)
    : m_view(view),
      m_registry(registry),
      m_analysis(analysis)
{
}

void LayoutPresetController::restore(const std::array<LayoutPreset, LayoutPresetSlots>& presets, int activeSlot)
{
    m_store.restore(presets, activeSlot);
}

const std::array<LayoutPreset, LayoutPresetSlots>& LayoutPresetController::all() const
{
    return m_store.all();
}

int LayoutPresetController::activeSlot() const
{
    return m_store.activeSlot();
}

std::map<std::string, double> LayoutPresetController::currentStackWeights() const
{
    // A self-contained snapshot: every scope on screen with its current weight,
    // so a loaded preset reproduces the exact split even for scopes left at the
    // default weight.
    std::map<std::string, double> weights;
    for (const std::string& id : m_view.stack().ids()) {
        weights[id] = m_view.layout().weight(id);
    }

    return weights;
}

const std::map<std::string, double>& LayoutPresetController::paramsOf(std::string_view id) const
{
    static const std::map<std::string, double> noParams;
    const auto stored = m_analysis.scopeParams.find(std::string{id});

    return stored != m_analysis.scopeParams.end() ? stored->second : noParams;
}

std::map<std::string, std::map<std::string, double>> LayoutPresetController::currentStackStyles() const
{
    std::map<std::string, std::map<std::string, double>> styles;
    for (const std::string& scopeId : m_view.stack().ids()) {
        const HostScope* hostScope = m_registry.byId(scopeId);
        if (hostScope == nullptr || hostScope->descriptor == nullptr) {
            continue;
        }
        const std::map<std::string, double>& params = paramsOf(scopeId);
        for (uint32_t index = 0; index < hostScope->descriptor->param_count; ++index) {
            const SsParamInfo& info = hostScope->descriptor->params[index];
            if (info.kind != SS_PARAM_CHOICE) {
                continue;
            }
            const auto current = params.find(info.key);
            styles[scopeId][info.key] = current != params.end() ? current->second : info.default_value;
        }
    }

    return styles;
}

void LayoutPresetController::applyStyles(const std::map<std::string, std::map<std::string, double>>& styles)
{
    for (const auto& [scopeId, params] : styles) {
        const HostScope* hostScope = m_registry.byId(scopeId);
        if (hostScope == nullptr || hostScope->descriptor == nullptr) {
            continue;
        }
        for (const auto& [key, value] : params) {
            const SsParamInfo* info = findParam(hostScope->descriptor, key);
            if (info == nullptr || info->kind != SS_PARAM_CHOICE) {
                continue;
            }
            m_analysis.scopeParams[scopeId][key] = std::clamp(value, info->min_value, info->max_value);
        }
    }
}

LayoutPreset LayoutPresetController::capture() const
{
    LayoutPreset preset;
    preset.stack = m_view.stack().tokens();
    preset.orientation = orientationToInt(m_view.layout().orientation());
    preset.weights = currentStackWeights();
    preset.styles = currentStackStyles();

    return preset;
}

bool LayoutPresetController::activeDirty() const
{
    return m_store.isDirty(capture());
}

LayoutPresetOutcome LayoutPresetController::save(int slot)
{
    m_store.save(slot, capture());

    // Read back after the save, which keeps the slot's name: a slot is
    // reported by what the user calls it, the way every list of them names it.
    return LayoutPresetOutcome{presetDisplayName(slot, m_store.at(slot)) + " saved", false, true};
}

LayoutPresetOutcome LayoutPresetController::load(int slot)
{
    const LayoutPreset& preset = m_store.at(slot);
    const std::string name = presetDisplayName(slot, preset);
    if (preset.stack.empty()) {
        return LayoutPresetOutcome{name + " is empty", false, false};
    }
    m_view.stack().restore(preset.stack);
    m_view.layout().setOrientation(orientationFromInt(preset.orientation));
    m_view.layout().setWeights(preset.weights);
    applyStyles(preset.styles);
    m_store.markLoaded(slot);
    m_analysis.enabledScopes = m_view.stack().enabledScopeIds();

    return LayoutPresetOutcome{name + " loaded", true, false};
}

void LayoutPresetController::beginRename(int slot)
{
    m_renamingSlot = slot;
    m_renameFocusDue = true;
    const std::string name = presetDisplayName(slot, m_store.at(slot));
    const std::size_t length = std::min(name.size(), m_renameBuffer.size() - 1);
    std::copy_n(name.begin(), length, m_renameBuffer.begin());
    m_renameBuffer[length] = '\0';
}

LayoutPresetOutcome LayoutPresetController::commitRename()
{
    const int slot = m_renamingSlot;
    m_renamingSlot = 0;
    m_renameFocusDue = false;
    if (slot == 0) {
        return LayoutPresetOutcome{};
    }
    // A slot renamed back to what it would be called anyway is a slot with no
    // name of its own, so the default follows it if it is ever renumbered.
    const std::string typed{m_renameBuffer.data()};
    m_store.rename(slot, typed == presetDisplayName(slot, LayoutPreset{}) ? std::string{} : typed);

    return LayoutPresetOutcome{"", false, true};
}

void LayoutPresetController::drawRenameField(float width, LayoutPresetOutcome& outcome)
{
    // The field takes the keyboard as it opens and gives it back on Enter or
    // on a click elsewhere; either way what was typed is what lands, so a
    // rename is never lost by clicking away from it. While it holds the
    // keyboard every plain-letter shortcut stands down, so a name can carry
    // any letter or digit.
    ImGui::SetNextItemWidth(width);
    if (m_renameFocusDue) {
        ImGui::SetKeyboardFocusHere();
        m_renameFocusDue = false;
    }
    const bool entered = ImGui::InputText("##rename", m_renameBuffer.data(), m_renameBuffer.size(),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (entered || ImGui::IsItemDeactivated()) {
        outcome = commitRename();
    }
}

void LayoutPresetController::drawSlotRow(int slot, float width, IconTextures& icons, LayoutPresetOutcome& outcome)
{
    const LayoutPreset& preset = m_store.at(slot);
    drawMenuRowHover(ImGui::GetCursorScreenPos().y);
    ImGui::PushID(slot);
    if (slot == m_renamingSlot) {
        // The field spans the name column AND the button's, so the row keeps
        // its width and nothing beside it shifts while a name is typed.
        drawRenameField(width + menuRowIconWidth(), outcome);
        ImGui::PopID();

        return;
    }
    // A radio in the checkbox's column: exactly one slot is loaded, where any
    // number of scopes can be shown, and clicking it loads that slot.
    if (ImGui::RadioButton("##active", slot == m_store.activeSlot())) {
        outcome = load(slot);
    }
    ImGui::SameLine(ImGui::GetFrameHeight() + ImGui::GetFontSize());
    const std::string name = presetDisplayName(slot, preset);
    const ImVec2 rowSize(width, ImGui::GetFrameHeight());
    if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups, rowSize)) {
        outcome = ImGui::GetIO().KeyShift ? save(slot) : load(slot);
    }
    // A double-click on the name renames too. It is the second way in, not the
    // only one: the button beside it is what says the slot can be renamed at
    // all, and a gesture nothing on screen mentions teaches nobody.
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        beginRename(slot);
    }
    drawMenuRowAccelerator(std::to_string(slot).c_str(), ImGui::GetFontSize() * 0.75f);
    ImGui::SameLine(0.0f, 0.0f);
    const std::string tooltip = "Rename " + name;
    if (menuRowIconButton("##rename", icons.textureId(Icon::PenLine, iconPixelSize()), tooltip.c_str())) {
        beginRename(slot);
    }
    ImGui::PopID();
}

LayoutPresetOutcome LayoutPresetController::drawPicker(IconTextures& icons)
{
    // A chip like the scope letters, leading the row: the label names the
    // active slot (starred once the live layout drifts; "-" when none), and
    // clicking opens the slot list - the mouse mirror of the digit keys. The
    // list is shaped like the scope selector's, because it is the same gesture.
    const bool dirty = activeDirty();
    char preview[8] = "-";
    if (m_store.activeSlot() != 0) {
        std::snprintf(preview, sizeof(preview), "%d%s", m_store.activeSlot(), dirty ? "*" : "");
    }
    const std::string tooltip = m_store.activeSlot() != 0
                                    ? presetDisplayName(m_store.activeSlot(), m_store.at(m_store.activeSlot())) +
                                          " - digits load, Shift+digits save"
                                    : std::string{"Layout presets - digits load, Shift+digits save"};
    if (scopeToggleButton("##preset-picker", preview, false, tooltip.c_str())) {
        ImGui::OpenPopup("##preset-popup");
    }
    const ImVec2 chipMin = ImGui::GetItemRectMin();
    const ImVec2 chipMax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(chipMin.x, chipMax.y + 2.0f));
    LayoutPresetOutcome outcome;
    if (ImGui::BeginPopup("##preset-popup")) {
        pushMenuRowStyle();
        const float width = presetRowWidth(m_store.all());
        for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
            drawSlotRow(slot, width, icons, outcome);
        }
        ImGui::TextDisabled("click loads - Shift+click saves");
        popMenuRowStyle();
        ImGui::EndPopup();
    } else {
        m_renamingSlot = 0;
    }

    return outcome;
}

}  // namespace sidescopes
