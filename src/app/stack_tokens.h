#pragma once

#include <string>
#include <vector>

namespace sidescopes {

class ScopeRegistry;

/// Reads a preference token string into scope ids. A token is a bracketed
/// `[id]` resolved by id, or a bare letter resolved through the registry; a
/// token the registry does not know is dropped, and duplicates collapse. A
/// string naming nothing valid yields nothing: this is the plain reading, with
/// no notion of a default, so a list that may legitimately be empty - the menu
/// order - never has a scope invented for it.
[[nodiscard]] std::vector<std::string> parseScopeTokens(const ScopeRegistry& registry, const std::string& text);

/// Reads a preference token string as a scope STACK: @ref parseScopeTokens,
/// falling back to the vectorscope when it names nothing valid, because the
/// window is never empty.
[[nodiscard]] std::vector<std::string> parseStackTokens(const ScopeRegistry& registry, const std::string& text);

/// @return @p stack as a preference token string: one token per scope, a
///         bracketed `[id]` for a letterless scope and its letter otherwise,
///         so a letterless scope survives a save.
[[nodiscard]] std::string formatStackTokens(const ScopeRegistry& registry, const std::vector<std::string>& stack);

}  // namespace sidescopes
