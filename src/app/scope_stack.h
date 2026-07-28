#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "app/scope_order.h"
#include "app/scope_registry.h"

namespace sidescopes {

/// @brief The scopes on screen, in the user's preferred order.
///
/// Keyed by scope id. Letters and mask membership are resolved through the
/// registry it is constructed with; where a scope lands among the panes is the
/// ScopeOrder's answer, not the sequence the scopes were switched on in, so a
/// scope brought back returns to the place it was left.
class ScopeStack
{
public:
    /// Both references must outlive the stack.
    ScopeStack(const ScopeRegistry& registry, const ScopeOrder& order);

    /// @return Whether @p id is on screen.
    [[nodiscard]] bool shows(std::string_view id) const;

    /// @return The scope ids on screen, in the preferred order.
    [[nodiscard]] const std::vector<std::string>& ids() const;

    /// Adds @p id, or removes it when already shown. The last scope stays,
    /// so the window is never empty.
    /// @return Whether @p id became newly visible, so the caller can refresh
    ///         its image.
    bool toggle(std::string_view id);

    /// Stacks @p id onto the current scopes when @p stack, otherwise solos it.
    /// @return Whether @p id became newly visible.
    bool choose(std::string_view id, bool stack);

    /// Re-seats the panes into the preferred order, for when that order has
    /// itself changed. Every other change sorts as it is made.
    void applyOrder();

    /// @return The scope ids the worker should compute for what is on screen:
    ///         the visible scopes minus the host-only ones (the color picker
    ///         reads the sampled cursor color, so it asks nothing of the
    ///         worker), in the preferred order.
    [[nodiscard]] std::vector<std::string> enabledScopeIds() const;

    /// Restores the stack from a preference token string, in the format
    /// parseStackTokens reads.
    void restore(const std::string& tokens);

    /// @return The stack as a preference token string, in the format
    ///         formatStackTokens writes.
    [[nodiscard]] std::string tokens() const;

private:
    const ScopeRegistry& m_registry;
    const ScopeOrder& m_order;
    std::vector<std::string> m_ids{VectorscopeScopeId};
};

}  // namespace sidescopes
