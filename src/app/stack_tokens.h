#pragma once

#include <string>
#include <vector>

namespace sidescopes {

class ScopeRegistry;

/// Reads a preference token string into scope ids. A token is a bracketed
/// `[id]`; a token the registry does not know is dropped, and duplicates
/// collapse. A string naming nothing valid yields nothing: this is the plain
/// reading, with no notion of a default, so a list that may legitimately be
/// empty - the menu order - never has a scope invented for it.
///
/// SCOPES ARE NAMED BY ID AND NEVER BY LETTER. A letter is a property of a
/// scope, not its identity: the registry hands one out only if it is still
/// free, so a letter collision or a change in the order modules register in
/// would silently re-point every token already written - a stored arrangement
/// quietly becoming a different set of scopes, with nothing to tell the user.
/// Users can also rebind keys. Ids are what the module declares and what the
/// ABI carries, and a longer file is worth far less than that.
[[nodiscard]] std::vector<std::string> parseScopeTokens(const ScopeRegistry& registry, const std::string& text);

/// Reads a preference token string as a scope STACK: @ref parseScopeTokens,
/// falling back to @ref DefaultScopeStack when it names nothing valid, because
/// the window is never empty.
[[nodiscard]] std::vector<std::string> parseStackTokens(const ScopeRegistry& registry, const std::string& text);

/// @return @p stack as a preference token string: one bracketed `[id]` per
///         scope, skipping any the registry does not know.
[[nodiscard]] std::string formatStackTokens(const ScopeRegistry& registry, const std::vector<std::string>& stack);

}  // namespace sidescopes
