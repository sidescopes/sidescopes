// Where the capture stream says the pointer is.
//
// On a Wayland session this is the only true answer available. X is told where
// the pointer is ONLY while it sits over an X surface, so the moment it crosses
// onto a native Wayland window - which on a GNOME desktop is nearly every
// window - XQueryPointer freezes at the last place it saw. That stale position
// is what a live probe would go on reading, and a colour that never changes
// looks exactly like a colour that is not changing.
//
// It is also why the pin picker appears to work when nothing else does: its
// sheets are full-screen X windows, so while a pick is up the pointer is over
// an X surface wherever it goes, and X tracks it again.

#include "platform/linux/stream_pointer.h"

#include <mutex>

namespace sidescopes {
namespace {

/// The published position under its lock. Written by the PipeWire thread,
/// read by the frame loop.
struct PublishedPointer
{
    std::mutex mutex;
    std::optional<DesktopPoint> position;
};

PublishedPointer& published()
{
    static PublishedPointer pointer;
    return pointer;
}

/// What one frame pixel is worth in desktop points along one axis. A portal
/// that named no extent leaves the frame speaking for itself.
double pointsPerPixel(double extentPoints, int framePixels)
{
    if (extentPoints <= 0.0) {
        return 1.0;
    }

    return extentPoints / framePixels;
}

}  // namespace

std::optional<DesktopPoint> streamPointToDesktop(const StreamPlacement& placement, int frameX, int frameY)
{
    if (placement.frameWidth <= 0 || placement.frameHeight <= 0) {
        return std::nullopt;
    }
    if (frameX < 0 || frameY < 0 || frameX >= placement.frameWidth || frameY >= placement.frameHeight) {
        return std::nullopt;
    }

    return DesktopPoint{placement.originX + frameX * pointsPerPixel(placement.widthPoints, placement.frameWidth),
                        placement.originY + frameY * pointsPerPixel(placement.heightPoints, placement.frameHeight)};
}

void publishStreamPointer(const std::optional<DesktopPoint>& pointer)
{
    PublishedPointer& holder = published();
    std::lock_guard lock(holder.mutex);
    holder.position = pointer;
}

std::optional<DesktopPoint> streamPointer()
{
    PublishedPointer& holder = published();
    std::lock_guard lock(holder.mutex);

    return holder.position;
}

}  // namespace sidescopes
