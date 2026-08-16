#pragma once

#include <vector>

#include "app/guided_tour.h"
#include "app/shortcut_resolver.h"

namespace sidescopes {

/// The stops, in the order a first visit wants them: what the thing IS, then
/// the one gesture that matters, then what it produced, then how to change it.
///
/// Nothing here counts anything. An earlier draft said "these four
/// photographs" and "six scopes to choose from", and both are facts about
/// today's build rather than about the application: the samples are a list in
/// the page, and the scopes are whatever the registry carries. Text that
/// counts things goes stale the first time somebody adds one, silently, in a
/// place nobody thinks to look. Nor does it name the scopes ON SCREEN, which
/// are whatever the visitor last left there.
///
/// The keys come from the BINDINGS rather than from the prose, for the same
/// reason: they are preferences, and a tour that insisted on D after somebody
/// rebound it would be teaching a control that no longer exists.
///
/// A unit of its own because it is CONTENT, not machinery: the words a visitor
/// reads, and which of them are quoted from the bindings in force. Nothing
/// here touches the lab's state, so it belongs beside the walk-through rather
/// than inside the shell that happens to start it - and so it can be tested,
/// which is what stops the keys drifting back to the shipped defaults.
[[nodiscard]] std::vector<TourStep> labTourSteps(const ShortcutResolver& shortcuts);

}  // namespace sidescopes
