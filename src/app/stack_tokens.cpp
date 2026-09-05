#include "app/stack_tokens.h"

#include <algorithm>
#include <cstddef>

#include "app/scope_registry.h"

namespace sidescopes {

std::vector<std::string> parseScopeTokens(const ScopeRegistry& registry, const std::string& text)
{
    std::vector<std::string> scopes;
    for (std::size_t at = text.find('['); at != std::string::npos; at = text.find('[', at)) {
        const auto close = text.find(']', at);
        if (close == std::string::npos) {
            break;
        }
        const HostScope* scope = registry.byId(text.substr(at + 1, close - at - 1));
        at = close + 1;
        if (scope != nullptr && std::find(scopes.begin(), scopes.end(), scope->id) == scopes.end()) {
            scopes.push_back(scope->id);
        }
    }

    return scopes;
}

std::vector<std::string> parseStackTokens(const ScopeRegistry& registry, const std::string& text)
{
    std::vector<std::string> stack = parseScopeTokens(registry, text);
    if (stack.empty()) {
        for (const std::string_view id : DefaultScopeStack) {
            if (registry.byId(id) != nullptr) {
                stack.emplace_back(id);
            }
        }
        if (stack.empty()) {
            stack.emplace_back(ColorPickerScopeId);
        }
    }

    return stack;
}

std::string formatStackTokens(const ScopeRegistry& registry, const std::vector<std::string>& stack)
{
    std::string tokens;
    for (const std::string& id : stack) {
        if (registry.byId(id) == nullptr) {
            continue;
        }
        tokens += '[';
        tokens += id;
        tokens += ']';
    }

    return tokens;
}

}  // namespace sidescopes
