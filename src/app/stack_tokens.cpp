#include "app/stack_tokens.h"

#include <algorithm>
#include <cstddef>

#include "app/scope_registry.h"

namespace sidescopes {

std::vector<std::string> parseScopeTokens(const ScopeRegistry& registry, const std::string& text)
{
    std::vector<std::string> scopes;
    for (std::size_t at = 0; at < text.size();) {
        const HostScope* scope = nullptr;
        if (text[at] == '[') {
            const auto close = text.find(']', at);
            if (close == std::string::npos) {
                break;
            }
            scope = registry.byId(text.substr(at + 1, close - at - 1));
            at = close + 1;
        } else {
            scope = registry.byLetter(text[at]);
            ++at;
        }
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
        stack.emplace_back(VectorscopeScopeId);
    }

    return stack;
}

std::string formatStackTokens(const ScopeRegistry& registry, const std::vector<std::string>& stack)
{
    std::string tokens;
    for (const std::string& id : stack) {
        const HostScope* scope = registry.byId(id);
        if (scope != nullptr && scope->letter != 0) {
            tokens += scope->letter;
        } else {
            tokens += '[';
            tokens += id;
            tokens += ']';
        }
    }

    return tokens;
}

}  // namespace sidescopes
