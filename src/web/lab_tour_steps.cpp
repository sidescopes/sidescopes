#include "web/lab_tour_steps.h"

#include <string>

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
std::vector<TourStep> labTourSteps(const ShortcutResolver& shortcuts)
{
    const std::string pinKey = shortcutLabel(shortcuts.bindings().pinColor);

    return {
        TourStep{"picture", "The Lab analyzes this image",
                 "The desktop application measures screen pixels. In the Lab, the selected image supplies the "
                 "available pixels on a virtual display.",
                 /*halo=*/0.0f},
        TourStep{"region", "Move the global region",
                 "The region stays fixed when you change images and can extend beyond one. Only its overlap with the "
                 "image reaches the scopes. Drag the band to move it, or a handle to resize it.",
                 // Clear of what the region already wears outside itself: a
                 // twelve-point band, and a close badge beyond that again.
                 /*halo=*/26.0f},
        TourStep{"scopes", "Read the same pixels in different ways",
                 "Every visible scope follows the region. Use the selector to combine instruments; a scope's letter "
                 "shows it alone, while Shift and the letter adds or removes it.",
                 // Snug: the toolbar sits directly above the panes and the
                 // status bar directly below, and a standoff crosses both.
                 /*halo=*/3.0f},
        TourStep{"adjust", "Observe an adjustment",
                 "Move a color or tone control and watch the traces update. These controls demonstrate common effects; "
                 "they do not reproduce a particular editor's processing.",
                 // Above the canvas entirely, like the strip below.
                 /*halo=*/0.0f},
        TourStep{"pin", "Keep a color for comparison",
                 "Open the pin tool and click to keep the color under the pointer. Press " + pinKey +
                     " to open it; hold Shift while pinning to keep the tool active for additional references.",
                 /*halo=*/4.0f},
        TourStep{"strip", "Use a sample or your own image",
                 "Choose an image from the strip or use the plus button to load one. Images you load are processed "
                 "locally and are not uploaded.",
                 // Above the canvas entirely, so nothing here is left bright;
                 // the page lights the button itself.
                 /*halo=*/0.0f},
    };
}

}  // namespace sidescopes
