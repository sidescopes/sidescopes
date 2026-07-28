#include "app/scope_order.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "app/scope_registry.h"
#include "app/stack_tokens.h"

namespace sidescopes {

ScopeOrder::ScopeOrder(const ScopeRegistry& registry)
    : m_registry(registry)
{
    restore({});
}

const std::vector<std::string>& ScopeOrder::ids() const
{
    return m_ids;
}

std::size_t ScopeOrder::rank(std::string_view id) const
{
    const auto at = std::find(m_ids.begin(), m_ids.end(), id);

    return static_cast<std::size_t>(std::distance(m_ids.begin(), at));
}

std::vector<std::string> ScopeOrder::sorted(std::vector<std::string> ids) const
{
    std::stable_sort(ids.begin(), ids.end(),
                     [this](const std::string& left, const std::string& right) { return rank(left) < rank(right); });

    return ids;
}

bool ScopeOrder::move(int from, int gap)
{
    const int count = static_cast<int>(m_ids.size());
    // A gap either side of the row it came from puts it back where it was.
    if (from < 0 || from >= count || gap < 0 || gap > count || gap == from || gap == from + 1) {
        return false;
    }
    std::string moved = std::move(m_ids[static_cast<std::size_t>(from)]);
    m_ids.erase(m_ids.begin() + from);
    // Removing the row shifts every later slot down by one.
    m_ids.insert(m_ids.begin() + (gap > from ? gap - 1 : gap), std::move(moved));

    return true;
}

void ScopeOrder::restore(const std::string& tokens)
{
    m_ids = parseScopeTokens(m_registry, tokens);
    for (const HostScope& scope : m_registry.scopes()) {
        if (std::find(m_ids.begin(), m_ids.end(), scope.id) == m_ids.end()) {
            m_ids.push_back(scope.id);
        }
    }
}

std::string ScopeOrder::tokens() const
{
    return formatStackTokens(m_registry, m_ids);
}

}  // namespace sidescopes
