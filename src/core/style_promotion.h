#pragma once

#include "core/preferences.h"

namespace sidescopes {

/// Rewrites @p preferences from the file format that carried a scope's plots as
/// STYLES into the one that carries each of them as its own SCOPE.
///
/// PER USER, not a fixed table: W means the luma waveform for somebody whose
/// waveform sat in its Luma style and the RGB waveform for everybody else, and
/// H means whatever that user's histogram style says - H for the per-channel
/// plot, G for the combined one. Every arrangement the file describes is
/// rewritten by that reading - the stack, the menu order, each preset's own
/// stack, styles and pane weights, the live pane weights and the shortcut
/// overrides - so the same panes come back in the same order showing the same
/// thing.
///
/// THE RETIRED KEYS ARE ERASED AS THEY ARE READ, which is what makes this run
/// once. Left in place they would be read again on the next load, and a second
/// reading would fold a scope the user had since added back onto its sibling -
/// the arrangement change this exists to prevent. It is the consume-must-apply
/// rule in its correct form: the reading and the erasure are one step.
void promoteScopeStyles(Preferences& preferences);

}  // namespace sidescopes
