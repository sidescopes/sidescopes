#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "app/scope_registry.h"

namespace sidescopes {

/// @brief The scopes on screen, in activation order.
///
/// Keyed by scope id. Letters, order, and mask membership are resolved through
/// the registry it is constructed with.
class ScopeStack
{
public:
    explicit ScopeStack(const ScopeRegistry& registry);

    /// @return Whether @p id is on screen.
    [[nodiscard]] bool shows(std::string_view id) const;

    /// @return The scope ids on screen, in activation order.
    [[nodiscard]] const std::vector<std::string>& ids() const;

    /// Adds @p id, or removes it when already shown. The last scope stays,
    /// so the window is never empty.
    /// @return Whether @p id became newly visible, so the caller can refresh
    ///         its image.
    bool toggle(std::string_view id);

    /// Stacks @p id onto the current scopes when @p stack, otherwise solos it.
    /// @return Whether @p id became newly visible.
    bool choose(std::string_view id, bool stack);

    /// @return The scope ids the worker should compute for what is on screen:
    ///         the visible scopes minus the host-only ones (the color picker
    ///         reads the sampled cursor color, so it asks nothing of the
    ///         worker), in activation order.
    [[nodiscard]] std::vector<std::string> enabledScopeIds() const;

    /// Restores the stack from a preference token string, in the format
    /// parseStackTokens reads.
    void restore(const std::string& tokens);

    /// @return The stack as a preference token string, in the format
    ///         formatStackTokens writes.
    [[nodiscard]] std::string tokens() const;

private:
    const ScopeRegistry& m_registry;
    std::vector<std::string> m_ids{VectorscopeScopeId};
};

}  // namespace sidescopes
