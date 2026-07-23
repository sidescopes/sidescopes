#include "app/scope_stack.h"

#include <algorithm>

#include "app/stack_tokens.h"

namespace sidescopes {

ScopeStack::ScopeStack(const ScopeRegistry& registry)
    : m_registry(registry)
{
}

bool ScopeStack::shows(std::string_view id) const
{
    return std::find(m_ids.begin(), m_ids.end(), id) != m_ids.end();
}

const std::vector<std::string>& ScopeStack::ids() const
{
    return m_ids;
}

bool ScopeStack::toggle(std::string_view id)
{
    const auto at = std::find(m_ids.begin(), m_ids.end(), id);
    if (at != m_ids.end()) {
        // The last scope stays: the window never goes empty.
        if (m_ids.size() > 1) {
            m_ids.erase(at);
        }
        return false;
    }
    m_ids.emplace_back(id);

    return true;
}

bool ScopeStack::choose(std::string_view id, bool stack)
{
    if (stack) {
        return toggle(id);
    }
    const bool wasShown = shows(id);
    m_ids.assign(1, std::string{id});

    return !wasShown;
}

std::vector<std::string> ScopeStack::enabledScopeIds() const
{
    std::vector<std::string> ids;
    for (const std::string& id : m_ids) {
        const HostScope* scope = m_registry.byId(id);
        if (scope && !scope->host) {
            ids.push_back(id);
        }
    }

    return ids;
}

void ScopeStack::restore(const std::string& tokens)
{
    m_ids = parseStackTokens(m_registry, tokens);
}

std::string ScopeStack::tokens() const
{
    return formatStackTokens(m_registry, m_ids);
}

}  // namespace sidescopes
