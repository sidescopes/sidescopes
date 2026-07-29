#pragma once

#include <string>
#include <string_view>

#include "app/scope_registry.h"

namespace sidescopes::testing {

/// Spells a stack or menu order the way the preferences file does - one
/// bracketed scope id per scope - from the letters a reader of these tests
/// still thinks in.
///
/// The FILE never carries a letter: a letter is a property of a scope rather
/// than its identity, and the registry hands one out only if it is still free,
/// so a collision or a change in the order modules register in would silently
/// re-point every token already written. The shorthand lives here, in the
/// tests, where it costs nothing and keeps a case about ordering readable as
/// one about ordering.
[[nodiscard]] inline std::string idTokens(std::string_view letters)
{
    std::string tokens;
    for (const char letter : letters) {
        std::string_view id;
        switch (letter) {
        case 'V':
            id = VectorscopeScopeId;
            break;
        case 'W':
            id = WaveformScopeId;
            break;
        case 'L':
            id = LumaWaveformScopeId;
            break;
        case 'R':
            id = ParadeScopeId;
            break;
        case 'H':
            id = HistogramScopeId;
            break;
        case 'C':
            id = ColorPickerScopeId;
            break;
        default:
            continue;
        }
        tokens += '[';
        tokens += id;
        tokens += ']';
    }

    return tokens;
}

}  // namespace sidescopes::testing
