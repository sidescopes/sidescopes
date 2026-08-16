#include "app/layout_presets.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "app/pane_layout.h"
#include "app/param_menu.h"
#include "app/scope_layout.h"
#include "app/scope_order.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/stack_tokens.h"
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

const LayoutPreset& LayoutPresetController::at(int slot) const
{
    return m_store.at(slot);
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

std::map<std::string, double> LayoutPresetController::declaredStyles(std::string_view scopeId) const
{
    std::map<std::string, double> values;
    const HostScope* hostScope = m_registry.byId(scopeId);
    if (hostScope == nullptr || hostScope->descriptor == nullptr) {
        return values;
    }
    for (uint32_t index = 0; index < hostScope->descriptor->param_count; ++index) {
        const SsParamInfo& info = hostScope->descriptor->params[index];
        if (info.kind == SS_PARAM_CHOICE) {
            values[info.key] = info.default_value;
        }
    }

    return values;
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
    preset.order = m_view.order().tokens();
    preset.orientation = orientationToInt(m_view.layout().orientation());
    preset.weights = currentStackWeights();
    preset.styles = currentStackStyles();

    return preset;
}

LayoutPreset LayoutPresetController::defaultLayout() const
{
    // Built field by field the way capture() builds one, over the same scope:
    // the tokens through the registry that writes them, the weight the layout
    // hands out unasked, and the styles the descriptor declares. Anything less
    // exact and a slot loaded from this would be written back to at once.
    LayoutPreset preset;
    std::vector<std::string> defaultIds;
    for (const std::string_view id : DefaultScopeStack) {
        defaultIds.emplace_back(id);
    }
    preset.stack = formatStackTokens(m_registry, defaultIds);
    // Every registered scope once, as the modules register them - written out
    // rather than left empty so that a slot restored from this reads back
    // through capture() identical, and is not written to on the next frame.
    preset.order = ScopeOrder{m_registry}.tokens();
    preset.orientation = orientationToInt(LayoutOrientation::Automatic);
    for (const std::string& id : defaultIds) {
        preset.weights[id] = DefaultPaneWeight;
        std::map<std::string, double> styles = declaredStyles(id);
        if (!styles.empty()) {
            preset.styles[id] = std::move(styles);
        }
    }

    return preset;
}

LayoutPresetOutcome LayoutPresetController::saveInto(int slot)
{
    LayoutPreset live = capture();
    if (sameLayout(live, m_store.effective(slot, defaultLayout()))) {
        return LayoutPresetOutcome{};
    }
    m_store.store(slot, std::move(live));

    // Read back after the write, which keeps the slot's name: a slot is
    // reported by what the user calls it, the way every list of them names it.
    return LayoutPresetOutcome{"Saved " + quotedPresetName(slot, m_store.at(slot)), false, true};
}

bool LayoutPresetController::activeDirty() const
{
    // Compared against what the slot would RESTORE rather than against what it
    // literally holds, so a slot nothing has been saved into does not read as
    // changed the moment it is opened on the arrangement it restores.
    return !sameLayout(capture(), m_store.effective(m_store.activeSlot(), defaultLayout()));
}

LayoutPresetOutcome LayoutPresetController::load(int slot)
{
    // Every slot loads. One holding nothing yet restores the arrangement the
    // application opens on, which is the only reading of a click on it that is
    // not an error message where an action was offered.
    const LayoutPreset preset = m_store.effective(slot, defaultLayout());
    // The order first: the stack seats its scopes by it as it restores them.
    m_view.order().restore(preset.order);
    m_view.stack().restore(preset.stack);
    m_view.layout().setOrientation(orientationFromInt(preset.orientation));
    m_view.layout().setWeights(preset.weights);
    applyStyles(preset.styles);
    m_store.markLoaded(slot);
    m_analysis.enabledScopes = m_view.stack().enabledScopeIds();

    return LayoutPresetOutcome{"Loaded " + quotedPresetName(slot, preset), true, false};
}

LayoutPresetOutcome LayoutPresetController::rename(int slot, std::string_view typed)
{
    // A slot renamed back to what it would be called anyway is a slot with no
    // name of its own, so the default follows it if it is ever renumbered.
    const bool wroteTheDefault = typed == presetDisplayName(slot, LayoutPreset{});
    m_store.rename(slot, wroteTheDefault ? std::string_view{} : typed);

    return LayoutPresetOutcome{"", false, true};
}

}  // namespace sidescopes
