#pragma once

#include <string>

namespace sidescopes {

/// The border label wears the window title (the filename, usually), capped so
/// a pathological title cannot dwarf the region. Cut at a UTF-8 code point
/// boundary, never inside a character.
[[nodiscard]] std::string borderLabelFrom(const std::string& title, const std::string& fallback);

}  // namespace sidescopes
