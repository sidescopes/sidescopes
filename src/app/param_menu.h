#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "platform/native_menu.h"
#include "sidescopes/module.h"

namespace sidescopes {

/// Dynamic context-menu action ids begin here, past the host's fixed
/// MenuAction range; a returned id at or above this value indexes the side
/// table @ref appendScopeChoiceMenus fills.
inline constexpr int ParamMenuActionBase = 1000;

/// One entry of the per-open side table: the scope parameter a dynamic menu
/// action sets and the value it sets it to. The action's id is
/// @ref ParamMenuActionBase plus the entry's index.
struct ParamMenuAction
{
    std::string scopeId;   ///< The scope whose parameter the action changes.
    std::string paramKey;  ///< The descriptor parameter key.
    double value;          ///< The choice index the action selects.
};

/// @return The first descriptor parameter of @p kind, or null when the
///         descriptor is null or declares none.
[[nodiscard]] const SsParamInfo* firstParamOfKind(const SsScopeDescriptor* descriptor, uint32_t kind);

/// Appends @p descriptor's choice parameters to @p items as checkable submenus
/// and records each choice in @p actions.
///
/// Every SS_PARAM_CHOICE parameter becomes a submenu titled by its menu_label
/// with the scope name prefix removed (so "Waveform Style" reads "Style"),
/// holding one action per choice. The checked action is the one whose index
/// equals the scope's current value for that key in @p params, defaulting to
/// the parameter's default_value when the key is unset. Each action's id is
/// @ref ParamMenuActionBase plus its index in @p actions, which is appended to
/// across calls so a whole menu's choices share one side table.
///
/// When @p flatten is set and the descriptor declares exactly one choice
/// parameter, its actions are emitted without their own submenu, so an
/// enclosing scope-name submenu titles them instead - reproducing the
/// hand-built menu this walk replaces.
void appendScopeChoiceMenus(const SsScopeDescriptor& descriptor, const std::map<std::string, double>& params,
                            bool flatten, std::vector<NativeMenuItem>& items, std::vector<ParamMenuAction>& actions);

}  // namespace sidescopes
