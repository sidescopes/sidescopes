#pragma once

#include <string>
#include <vector>

namespace sidescopes {

/// A rectangle in frame pixels. The pin does all of its geometry in frame
/// pixels because they are isotropic; display percentages are not, and
/// distance gates would skew. The App converts at the boundary.
struct PinRect
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

/// A detected face reduced to the anchor the pin follows: its centre and
/// width, in frame pixels. Both platforms anchor on the detector's box -
/// deliberately no landmark refinement, so macOS and Windows behave the
/// same.
struct FaceAnchor
{
    double centerX = 0.0;
    double centerY = 0.0;
    double width = 0.0;
};

/// What one probe decided, with the reason spelled out for the decision log
/// the owner grades during the spike. While the pin is hunting - the face
/// is not confirmed where the region sits - the border hides rather than
/// outline stale content, and reappears on the next confirmed position.
struct FacePinDecision
{
    bool adopt = false;
    bool hunting = false;
    std::string reason;
};

/// A face-pinned region's persistent state: the USER'S crop relative to the
/// face anchor - never the detector box itself - plus the last adopted
/// anchor and the stability bookkeeping. The crop is stored in units of the
/// anchor width, so pan and zoom reproduce exactly the rectangle the user
/// shaped (the forehead, not the face box).
struct FacePinState
{
    double offsetX = 0.0;  ///< crop centre minus anchor centre, in anchor widths
    double offsetY = 0.0;
    double sizeX = 1.0;  ///< crop size in anchor widths
    double sizeY = 1.0;
    FaceAnchor lastAnchor;
    // The candidate awaiting a consecutive agreeing sighting.
    FaceAnchor pendingAnchor;
    int pendingCount = 0;
    // Consecutive probes that found nothing near the anchor; past the
    // recovery threshold the search widens to the whole attached window.
    int missCount = 0;
};

/// The pure decision core of face-pinned regions. The pin is deliberately
/// calm: it adopts only positions confirmed by two consecutive agreeing
/// probes, so a pan or zoom in progress freezes the region and one clean
/// snap follows when the photo settles. A pin that has lost its face
/// searches the whole attached window with the size gate relaxed - zoom
/// legitimately resizes faces - but adoption still needs a unique, stable
/// candidate.
namespace face_pin {

/// A fresh pin: the picked face box is both the anchor and the initial
/// crop, and counts as the first sighting of itself.
[[nodiscard]] FacePinState makePin(const FaceAnchor& anchor, const PinRect& crop);

/// The user's crop mapped through @p anchor: the region the pin wants when
/// the face is there.
[[nodiscard]] PinRect mapRegion(const FacePinState& state, const FaceAnchor& anchor);

/// A border edit while pinned: re-derives the crop against the last adopted
/// anchor, so the pin follows the user's new rectangle from here on.
void rebindCrop(FacePinState& state, const PinRect& crop);

/// A window translation carries the face with it: the anchors ride along so
/// the probe keeps searching where the face actually is.
void translate(FacePinState& state, double dxPixels, double dyPixels);

/// Whether the pin has lost its face and the probe should search the whole
/// attached window instead of the patch around the anchor.
[[nodiscard]] bool searchingWide(const FacePinState& state);

/// Whether the face has been missing long enough that the pin gives up:
/// the region is removed instead of sitting somewhere wrong, and the user
/// starts again with a fresh pick.
[[nodiscard]] bool givenUp(const FacePinState& state);

/// Whether a detector box can be believed. Only a box flush against the
/// probe's LEFT or RIGHT edge is rejected: a side clip corrupts the box's
/// width - the anchor's one scale reference. A face nearing the top or
/// bottom edge keeps its width and stays trackable; portraits often fill
/// the window.
[[nodiscard]] bool trustworthyBox(const PinRect& box, const PinRect& bounds);

/// One probe's verdict over the detected candidates. Adoption moves
/// @p state's anchor; every hold leaves it untouched and names its reason.
[[nodiscard]] FacePinDecision decide(FacePinState& state, const std::vector<FaceAnchor>& candidates);

}  // namespace face_pin

}  // namespace sidescopes
