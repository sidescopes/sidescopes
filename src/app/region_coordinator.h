#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/analysis_worker.h"
#include "core/region_kind.h"

namespace sidescopes {

class AttachController;
class CaptureController;
class FaceLockController;
class RegionPicker;

/// @return The kind of the region the scopes are reading right now: the
///         active attached window's, or the global one. A narrower
///         question than AttachController::attached(), which reports
///         whether ANY window holds a region - an attached window that is
///         not focused leaves the global kind in effect. Orthogonal to
///         RegionCoordinator::isFullScreen(), which measures a region's
///         extent rather than what it is bound to.
[[nodiscard]] RegionKind regionKind(uint64_t activeWindowIdentity);

/// What a region decision asks of the host. The coordinator owns the global
/// region, but neither the analysis settings the worker reads nor the clock
/// the shell measures idleness against, so those changes travel back instead
/// of being made in place.
struct RegionOutcome
{
    /// The region the scopes should read from here on; empty when the
    /// decision left them reading where they were.
    std::optional<RegionOfInterest> region;
    /// Every attached window was let go: the host drops the active window
    /// along with its motion watch and motion flags.
    bool detachedAll = false;
    /// Interaction worth marking: the host stamps its activity clock.
    bool activity = false;
};

/// What the region border's live edit asks of the host - at most one of them,
/// applied in this order: the border's close affordances, its attach toggle,
/// then a drag that moved or resized the region it outlines.
struct RegionBorderEditOutcome
{
    bool dismissed = false;
    bool attachToggled = false;
    std::optional<RegionOfInterest> edited;
};

/// The shell state one border sync reads, gathered fresh per call: the label
/// an attached border wears, which window the region is routed to, whether
/// the active window is moving or this application's own window is
/// minimized, and the frame clock the face-lock content watch settles
/// against.
struct RegionBorderState
{
    const std::string& attachedLabel;
    uint64_t activeWindowIdentity;
    bool windowMoving;
    bool windowMinimized;
    double now;
};

/// Owns the region truth the whole shell shares: the global region the
/// analysis falls back to whenever no attached window is active, the label
/// the global border wears, and which region a border drag in flight began
/// on. It keeps the region border in step with what the scopes read, and
/// answers how far that region reaches. Regions are host-wide rather than
/// per-surface state, which is why this sits beside the scope view rather
/// than inside it. It reads the attach, capture, picker and face-lock
/// controllers it is constructed with and drives the platform border seams
/// directly; what it cannot carry out itself travels back as a RegionOutcome
/// the host applies.
class RegionCoordinator
{
public:
    /// @p attach holds the attached windows and lets them go, @p capture
    /// names the captured display the border is drawn on, @p picker says
    /// whether a pick is in flight, @p faceLock whether a locked face is
    /// unverified or its content unsettled, and @p region is the region the
    /// scopes are reading right now. All must outlive the coordinator.
    RegionCoordinator(AttachController& attach, const CaptureController& capture, const RegionPicker& picker,
                      const FaceLockController& faceLock, const RegionOfInterest& region);

    /// @return Whether the region the scopes are reading covers the whole
    ///         captured display - the state Watch Full Screen restores. An
    ///         extent, not a kind: a region attached to a window filling the
    ///         display reads full here too.
    [[nodiscard]] bool isFullScreen() const;

    /// @return The global region: the one bound to no window, which the
    ///         analysis falls back to whenever no attached window is active.
    [[nodiscard]] const RegionOfInterest& globalRegion() const;

    /// Replaces the global region. Storing it is all this does; putting it in
    /// force is useRegion's job.
    void setGlobalRegion(const RegionOfInterest& region);

    /// Reads @p region with the scopes. @return The change for the host to
    ///         put in force, empty when the scopes are already reading it -
    ///         a no-op nudges neither the worker nor the border.
    [[nodiscard]] RegionOutcome useRegion(const RegionOfInterest& region) const;

    /// Drops all selection - a pending pick, every attached window, and the
    /// global region alike - and returns the scopes to the whole display.
    [[nodiscard]] RegionOutcome resetToFullScreen();

    /// Reconciles the region border with what the scopes read. Called every
    /// frame; the platform side makes the unchanged case free.
    void syncBorder(const RegionBorderState& state);

    /// One poll of the live region border, whose edges, corners, and move tab
    /// adjust the region it outlines. @p activeWindowIdentity is the focused
    /// attached window, 0 when the global region is in force.
    [[nodiscard]] RegionBorderEditOutcome pollBorderEdit(uint64_t activeWindowIdentity);

    /// @return Whether a border drag is in flight; the focus routing freezes
    ///         on the active window while one is.
    [[nodiscard]] bool borderEditing() const;

    /// @return The window the border drag in flight began on, or 0 when it
    ///         began on the global region.
    [[nodiscard]] uint64_t borderEditIdentity() const;

private:
    AttachController& m_attach;
    const CaptureController& m_capture;
    const RegionPicker& m_picker;
    const FaceLockController& m_faceLock;
    const RegionOfInterest& m_region;

    RegionOfInterest m_globalRegion;

    /// The global region's border label: the captured display's name,
    /// refreshed when the captured display changes.
    std::string m_displayLabel;
    uint32_t m_displayLabelId = 0;

    // Which region the border edit in flight started on (0 = the global
    // one), latched at the drag's first frame: no focus race can reroute a
    // grabbed border, and an attached edit can never convert to global.
    bool m_borderEditing = false;
    uint64_t m_borderEditIdentity = 0;
};

}  // namespace sidescopes
