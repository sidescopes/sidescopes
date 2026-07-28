#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sidescopes {

class ScopeRegistry;

/// @brief The order the user keeps the scopes in.
///
/// Every registered scope sits here exactly once, whether or not it is on
/// screen. The menu lists them in this order, so checking and unchecking a
/// scope never moves a row under the pointer, and the panes follow it, so a
/// scope brought back returns to the place it was left. Dragging a menu row is
/// what changes it, and it persists.
class ScopeOrder
{
public:
    /// @p registry supplies the scopes and the order they start in, and must
    /// outlive this.
    explicit ScopeOrder(const ScopeRegistry& registry);

    /// @return Every registered scope id, in the user's order.
    [[nodiscard]] const std::vector<std::string>& ids() const;

    /// @return @p id's place in the order, or its size for a scope the order
    ///         does not name, so an unknown scope sorts last rather than
    ///         first.
    [[nodiscard]] std::size_t rank(std::string_view id) const;

    /// @return @p ids in this order. Scopes the order does not name keep their
    ///         relative sequence, at the end.
    [[nodiscard]] std::vector<std::string> sorted(std::vector<std::string> ids) const;

    /// Lifts the scope at @p from and reinserts it at the @p gap slot, counting
    /// the gaps between rows: 0 before the first, size() after the last. An
    /// index out of range, and a gap either side of @p from, leave the order
    /// alone.
    /// @return Whether the order changed.
    bool move(int from, int gap);

    /// Restores the order from a preference token string, in the format
    /// parseScopeTokens reads. Tokens the registry does not know are dropped,
    /// and every registered scope the string leaves out follows the ones it
    /// names, in registration order - so a file written before a scope existed
    /// still names every scope once it is loaded.
    void restore(const std::string& tokens);

    /// @return The order as a preference token string, in the format
    ///         formatStackTokens writes.
    [[nodiscard]] std::string tokens() const;

private:
    const ScopeRegistry& m_registry;
    std::vector<std::string> m_ids;
};

}  // namespace sidescopes
