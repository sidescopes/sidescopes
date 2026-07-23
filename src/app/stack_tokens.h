#pragma once

#include <string>
#include <vector>

namespace sidescopes {

class ScopeRegistry;

/// Reads a preference token string into scope ids, falling back to the
/// vectorscope when it names nothing valid. A token is a bracketed `[id]`
/// resolved by id, or a bare letter resolved through the registry; a token
/// the registry does not know is dropped, and duplicates collapse.
[[nodiscard]] std::vector<std::string> parseStackTokens(const ScopeRegistry& registry, const std::string& text);

/// @return @p stack as a preference token string: one token per scope, a
///         bracketed `[id]` for a letterless scope and its letter otherwise,
///         so a letterless scope survives a save.
[[nodiscard]] std::string formatStackTokens(const ScopeRegistry& registry, const std::vector<std::string>& stack);

}  // namespace sidescopes
