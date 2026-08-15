#include "web/demo_tour_steps.h"

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
std::vector<TourStep> demoTourSteps(const ShortcutResolver& shortcuts)
{
    const std::string drawKey = shortcutLabel(shortcuts.bindings().drawRegion);
    const std::string pinKey = shortcutLabel(shortcuts.bindings().pinColor);

    return {
        TourStep{"picture", "SideScopes reads your screen",
                 "On a desktop it sits above your editor and watches part of the screen while you work. "
                 "Here, this picture is the screen.",
                 /*halo=*/0.0f},
        TourStep{"region", "You choose what it measures",
                 "Only the pixels inside the region reach the scopes. Drag the striped band to move it, or a dot "
                 "on its edge to resize. The x in the corner clears it.",
                 // Clear of what the region already wears outside itself: a
                 // twelve-point band, and a close badge beyond that again.
                 /*halo=*/26.0f},
        TourStep{"scopes", "The scopes follow the region",
                 "Each one shows those same pixels a different way, and they all redraw as you move it. These are "
                 "the engines the desktop application runs, not a simpler stand-in.",
                 // Snug: the toolbar sits directly above the panes and the
                 // status bar directly below, and a standoff crosses both.
                 /*halo=*/3.0f},
        TourStep{"chooser", "Choose which scopes to show",
                 "Vectorscopes, waveforms, parades and histograms, in any combination. Each has a letter key of "
                 "its own, and holding Shift adds one alongside the others instead of replacing them.",
                 /*halo=*/4.0f},
        TourStep{"tools", "Draw a region of your own",
                 "The pencil starts a new one: drag across the picture to place it, or press " + drawKey +
                     ". The icon beside it clears the region.",
                 /*halo=*/4.0f},
        TourStep{"pin", "Pin a colour to compare against",
                 "The dropper keeps the colour under the pointer, so you can hold it against another part of the "
                 "picture. Press " +
                     pinKey + ", and hold Shift while clicking to pin several.",
                 /*halo=*/4.0f},
        TourStep{"adjust", "Watch a scope answer",
                 "Move any of these and the scopes redraw as you go. Exposure and contrast walk the waveform up and "
                 "down, warmth carries the vectorscope's cloud off centre, and saturation pushes it outward. This is "
                 "what the instruments are for.",
                 // Above the canvas entirely, like the strip below.
                 /*halo=*/0.0f},
        TourStep{"strip", "Try it on your own pictures",
                 "The strip above swaps between the samples, and the + takes a picture from your computer. It "
                 "never leaves the browser: there is no code here that could send it anywhere.",
                 // Above the canvas entirely, so nothing here is left bright;
                 // the page lights the button itself.
                 /*halo=*/0.0f},
    };
}

}  // namespace sidescopes
