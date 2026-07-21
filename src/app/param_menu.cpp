#include "app/param_menu.h"

#include <cmath>

namespace sidescopes {
namespace {

/// The submenu title for @p param: its menu_label with the leading scope name
/// and its trailing space removed, since the submenu always sits under that
/// scope's own section. "Waveform Style" reads "Style"; "Trace Response", with
/// no scope prefix, is unchanged.
std::string strippedMenuLabel(const SsScopeDescriptor& descriptor, const SsParamInfo& param)
{
    std::string label = param.menu_label != nullptr ? param.menu_label : "";
    if (descriptor.name != nullptr) {
        const std::string prefix = std::string(descriptor.name) + " ";
        if (label.rfind(prefix, 0) == 0) {
            label.erase(0, prefix.size());
        }
    }

    return label;
}

uint32_t countChoiceParams(const SsScopeDescriptor& descriptor)
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < descriptor.param_count; ++index) {
        if (descriptor.params[index].kind == SS_PARAM_CHOICE) {
            ++count;
        }
    }

    return count;
}

}  // namespace

const SsParamInfo* firstParamOfKind(const SsScopeDescriptor* descriptor, uint32_t kind)
{
    if (descriptor == nullptr) {
        return nullptr;
    }
    for (uint32_t index = 0; index < descriptor->param_count; ++index) {
        if (descriptor->params[index].kind == kind) {
            return &descriptor->params[index];
        }
    }

    return nullptr;
}

const SsParamInfo* findParam(const SsScopeDescriptor* descriptor, const std::string& key)
{
    if (descriptor == nullptr) {
        return nullptr;
    }
    for (uint32_t index = 0; index < descriptor->param_count; ++index) {
        if (key == descriptor->params[index].key) {
            return &descriptor->params[index];
        }
    }

    return nullptr;
}

void appendScopeChoiceMenus(const SsScopeDescriptor& descriptor, const std::map<std::string, double>& params,
                            bool flatten, std::vector<NativeMenuItem>& items, std::vector<ParamMenuAction>& actions)
{
    using Kind = NativeMenuItem::Kind;

    // A lone choice under a scope-name submenu flattens: the enclosing submenu
    // titles the choice, so the choice keeps no submenu of its own.
    const bool bareChoice = flatten && countChoiceParams(descriptor) == 1;
    for (uint32_t index = 0; index < descriptor.param_count; ++index) {
        const SsParamInfo& param = descriptor.params[index];
        if (param.kind != SS_PARAM_CHOICE) {
            continue;
        }

        double value = param.default_value;
        const auto stored = params.find(param.key);
        if (stored != params.end()) {
            value = stored->second;
        }
        const int current = static_cast<int>(std::lround(value));

        if (!bareChoice) {
            items.push_back({Kind::SubmenuBegin, strippedMenuLabel(descriptor, param), -1, false, ""});
        }
        for (int choice = 0; param.choices != nullptr && param.choices[choice] != nullptr; ++choice) {
            const int actionId = ParamMenuActionBase + static_cast<int>(actions.size());
            actions.push_back(ParamMenuAction{descriptor.id, param.key, static_cast<double>(choice)});
            items.push_back({Kind::Action, param.choices[choice], actionId, choice == current, ""});
        }
        if (!bareChoice) {
            items.push_back({Kind::SubmenuEnd, "", -1, false, ""});
        }
    }
}

}  // namespace sidescopes
