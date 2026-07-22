#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/analysis_worker.h"  // RegionOfInterest

namespace sidescopes {

/// A window's rectangle in global desktop points (top-left origin), the space
/// the desktop seams report geometry in. The controller reasons here and in
/// the display's own rectangle, mapping each window-relative region to the
/// display-percent RegionOfInterest that capture and the region border speak.
struct AttachWindowRect
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// A display's rectangle in the same global desktop points, so an absolute
/// window rectangle becomes display percentages.
struct AttachDisplayRect
{
    double originX = 0.0;
    double originY = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// One attached window's state this frame, gathered by the host from the
/// desktop seams: its rectangle (nothing once it has closed), whether it is
/// minimized or otherwise off screen, and the display it sits on. The display
/// fields are only meaningful for a visible window.
struct AttachedWindowObservation
{
    uint64_t identity = 0;
    std::optional<AttachWindowRect> windowRect;
    bool minimized = false;
    uint32_t displayId = 0;
    AttachDisplayRect display;
    /// The window's current title, for the border's label; may be empty.
    std::string title;
};

/// The per-frame verdict: the active window's mapped region (present exactly
/// when the focused window is a visible attached one), where that window
/// sits, and how many attached windows closed this frame. With no region the
/// host falls back to the single global region - the two kinds never mix.
struct AttachDecision
{
    std::optional<RegionOfInterest> region;
    /// The focused attached window, or 0 when the focused window is not a
    /// visible attached one - the window whose region the scopes analyze and
    /// whose border wears the attached style and label.
    uint64_t activeIdentity = 0;
    int64_t activeOwnerPid = 0;
    uint32_t activeDisplayId = 0;
    std::optional<AttachWindowRect> activeRect;
    /// The active window's current title, or empty; the host prefers it over
    /// the application name for the border's label.
    std::string activeTitle;
    /// Attached windows that closed and were pruned this frame.
    std::size_t closedCount = 0;
    /// The closures emptied the attached set; the host tells the user and the
    /// analysis falls back to the global region.
    bool detachedAll = false;
};

/// The brain behind attached regions. Pure logic: it holds a set of
/// attached windows - each with its region as an ABSOLUTE screen
/// rectangle, mechanically bound to its window: a window MOVE translates the
/// region exactly; a window RESIZE leaves it glued to the screen, with an
/// arriving window edge pushing it (position, permanently) and walls closing
/// below its size squeezing it (elastically - the size returns when the
/// window grows back). Nothing else ever moves or rescales a region, which
/// is the owner-chosen answer to fit-to-window content not scaling with the
/// window: what the region does is always visible mechanics, never a model.
/// The controller consumes per-frame observations the host gathers from the
/// desktop seams and emits region decisions. An attached region is effective
/// exactly while ITS window is the focused one: the active window is the
/// focused attached window, switching focus between attached windows
/// switches the analyzed region, and any other focus (SideScopes itself, an
/// unattached window) yields no region here - the host falls back to the
/// single global region. A hidden or minimized attached window keeps its
/// region and simply cannot be active. The application's own visibility is
/// none of this class's business, and neither is the global region. The
/// controller never calls a platform function itself, so every transition is
/// unit-testable.
class AttachController
{
public:
    /// @return Whether any window is currently attached.
    [[nodiscard]] bool attached() const;

    /// @return How many windows are currently attached.
    [[nodiscard]] std::size_t attachedCount() const;

    /// @return The identities of all attached windows, in attach order, for
    ///         the host's per-frame observation sweep.
    [[nodiscard]] std::vector<uint64_t> attachedIdentities() const;

    /// @return Whether the window with @p identity is attached.
    [[nodiscard]] bool isAttached(uint64_t identity) const;

    /// @return The active window's platform identity, or 0 when no attached
    ///         window is visible.
    [[nodiscard]] uint64_t activeIdentity() const;

    /// @return The active window's application name, or an empty string when
    ///         the focused window is not a visible attached one. The border's
    ///         label wears it.
    [[nodiscard]] std::string activeApplicationName() const;

    /// Attaches to a window, or re-picks the region of one already attached,
    /// and makes it active. @p identity is its platform handle, @p ownerPid
    /// the process id of its application, @p applicationName its name,
    /// @p windowRect its current rectangle on @p display, and
    /// @p absoluteRegion the region the user confirmed. The region is
    /// stored in absolute desktop coordinates, screen-glued: window moves
    /// translate it, resizes leave it in place, and it is clamped inside
    /// the window.
    /// @return The clamped absolute region actually stored, for immediate
    ///         analysis feedback.
    RegionOfInterest attach(uint64_t identity, int64_t ownerPid, std::string applicationName,
                            AttachWindowRect windowRect, AttachDisplayRect display,
                            const RegionOfInterest& absoluteRegion);

    /// Records an in-window region edit (a border drag or a fresh draw) on
    /// the ACTIVE window: re-anchors the stored absolute rectangle to
    /// @p newAbsoluteRegion against @p windowRect, clamped so the region
    /// stays inside the window.
    /// @return The clamped absolute region actually stored, or
    ///         @p newAbsoluteRegion unchanged when no window is active.
    RegionOfInterest editRegion(const RegionOfInterest& newAbsoluteRegion, AttachWindowRect windowRect,
                                AttachDisplayRect display);

    /// One frame of observation while attached. @p windows carries one entry
    /// per attached window (closed ones are pruned); @p focusedIdentity names
    /// the window the user is working in - the foreground application's
    /// frontmost ordinary window - or nothing when that is unknown. Emits
    /// the active window's mapped region exactly when the focused window is
    /// a visible attached one, plus the closures. A no-op verdict when
    /// nothing is attached.
    AttachDecision observe(const std::vector<AttachedWindowObservation>& windows,
                           std::optional<uint64_t> focusedIdentity);

    /// Detaches one window; the remaining set keeps working.
    void remove(uint64_t identity);

    /// Detaches everything - the user pressed Escape - and the caller
    /// returns the region to full screen.
    void detachAll();

private:
    struct AttachedWindow
    {
        uint64_t identity = 0;
        int64_t ownerPid = 0;
        std::string applicationName;
        // The region as an absolute desktop rectangle. Its size only ever
        // changes by the user's hand: window edges push its position, and
        // only the emitted mapping clips, so a squeezed region re-expands
        // when the window grows back.
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        // Re-appearance settle: while set, rect changes rebaseline silently
        // instead of binding - the OS animates hidden windows back on
        // screen (Quick Look zooms its panel open), and those transitional
        // rectangles must never push the stored region. Binding resumes
        // once the rectangle holds still for two consecutive observations.
        bool settling = false;
        // The window rectangle at the last observation, telling a move (same
        // size: the region rides along) from a resize (the region stays
        // screen-glued and only edges push it).
        AttachWindowRect lastWindowRect;
        bool observedOnce = false;
    };

    [[nodiscard]] AttachedWindow* find(uint64_t identity);
    [[nodiscard]] const AttachedWindow* find(uint64_t identity) const;

    /// Stores @p window's absolute rectangle from a display-percent region,
    /// clamped into the window, and remembers the window rectangle it was
    /// set against.
    static void setStoredFromAbsolute(AttachedWindow& window, const RegionOfInterest& absoluteRegion,
                                      const AttachWindowRect& windowRect, const AttachDisplayRect& display);

    /// Applies one window observation to the stored rectangle: a move (same
    /// size) translates it; a resize leaves it screen-glued and pushes its
    /// position just enough to keep it inside (permanently), per axis.
    static void bindStoredToWindow(AttachedWindow& window, const AttachWindowRect& windowRect);

    /// Advances every attached window's stored rectangle from this frame's
    /// observations; minimized and closed windows are left untouched.
    void updateAttached(const std::vector<AttachedWindowObservation>& windows);

    /// @p window's stored rectangle clipped to the window - elastically, the
    /// stored size survives - as display percentages on @p display.
    [[nodiscard]] static RegionOfInterest toAbsolute(const AttachedWindow& window, const AttachWindowRect& windowRect,
                                                     const AttachDisplayRect& display);

    /// Prunes attached windows whose observation reports them closed.
    void pruneClosed(const std::vector<AttachedWindowObservation>& windows, AttachDecision& decision);

    /// Maps the active window's region into the decision.
    void updateRegion(const std::vector<AttachedWindowObservation>& windows, AttachDecision& decision);

    std::vector<AttachedWindow> m_windows;
    uint64_t m_activeIdentity = 0;
};

}  // namespace sidescopes
