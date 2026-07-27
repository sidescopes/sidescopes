#pragma once

#include "core/preferences.h"

namespace sidescopes {

class PinBoard;
class ScopeView;
class ShortcutResolver;
struct AnalysisSettings;

/// Puts a saved session back into the objects that hold it while the
/// application runs: the pinned colors, the scope stack and everything the
/// view draws it with, the traces' intensity and smoothing, the key bindings,
/// and the scope parameters and enabled list the worker reads.
///
/// The file and the running application state the same choices in two
/// vocabularies, and this unit is the whole of the translation between them -
/// both directions side by side, so a value added to one has one place to be
/// added to the other. The halves used to sit in two files and a hundred lines
/// apart, which is how a saved value comes to be restored and never written
/// back.
///
/// What stays with the shell is what cannot be said without a window: the
/// window's own placement, the interface scale, and the preset slots, whose
/// controller reads the view as it restores.
void restorePreferences(const Preferences& saved, ScopeView& view, PinBoard& pins, ShortcutResolver& shortcuts,
                        AnalysisSettings& analysis);

/// Reads the live objects back out into a session worth saving, the exact
/// inverse of @ref restorePreferences. The caller fills in the fields that
/// unit does not restore.
[[nodiscard]] Preferences capturePreferences(const ScopeView& view, const PinBoard& pins,
                                             const ShortcutResolver& shortcuts, const AnalysisSettings& analysis);

}  // namespace sidescopes
