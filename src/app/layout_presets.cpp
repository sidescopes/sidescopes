#include "app/layout_presets.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "app/context_menu.h"
#include "app/imgui_ui.h"
#include "app/param_menu.h"
#include "app/scope_layout.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "imgui.h"
#include "sidescopes/module.h"

namespace sidescopes {

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

    return LayoutPresetOutcome{"preset " + std::to_string(slot) + " saved", false, true};
}

LayoutPresetOutcome LayoutPresetController::load(int slot)
{
    const LayoutPreset& preset = m_store.at(slot);
    if (preset.stack.empty()) {
        return LayoutPresetOutcome{"preset " + std::to_string(slot) + " is empty", false, false};
    }
    m_view.stack().restore(preset.stack);
    m_view.layout().setOrientation(orientationFromInt(preset.orientation));
    m_view.layout().setWeights(preset.weights);
    applyStyles(preset.styles);
    m_store.markLoaded(slot);
    m_analysis.enabledScopes = m_view.stack().enabledScopeIds();

    return LayoutPresetOutcome{"preset " + std::to_string(slot) + " loaded", true, false};
}

LayoutPresetOutcome LayoutPresetController::drawPicker()
{
    // A chip like the scope letters, leading the row: the label names the
    // active slot (starred once the live layout drifts; "-" when none), and
    // clicking opens the slot list - the mouse mirror of the digit keys.
    const bool dirty = activeDirty();
    char preview[8] = "-";
    if (m_store.activeSlot() != 0) {
        std::snprintf(preview, sizeof(preview), "%d%s", m_store.activeSlot(), dirty ? "*" : "");
    }
    if (scopeToggleButton("##preset-picker", preview, false, "Layout presets - digits load, Shift+digits save")) {
        ImGui::OpenPopup("##preset-popup");
    }
    const ImVec2 chipMin = ImGui::GetItemRectMin();
    const ImVec2 chipMax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(chipMin.x, chipMax.y + 2.0f));
    LayoutPresetOutcome outcome;
    if (ImGui::BeginPopup("##preset-popup")) {
        for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
            const LayoutPreset& preset = m_store.at(slot);
            if (ImGui::Selectable(presetLabel(slot, preset).c_str(), slot == m_store.activeSlot())) {
                outcome = ImGui::GetIO().KeyShift ? save(slot) : load(slot);
            }
        }
        ImGui::TextDisabled("click loads - Shift+click saves");
        ImGui::EndPopup();
    }

    return outcome;
}

}  // namespace sidescopes
