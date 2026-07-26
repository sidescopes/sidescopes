#pragma once

#include <string>

namespace sidescopes {

/// The value of an environment variable, empty when it is unset.
///
/// std::getenv is a hard error under MSVC's warnings-as-errors, so the annex
/// Microsoft accepts is written once, here, rather than at every reader.
[[nodiscard]] std::string environmentValue(const char* name);

}  // namespace sidescopes
