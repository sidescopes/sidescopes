#pragma once

namespace sidescopes {

/// Which of the two kinds a region is. Everything about regions hangs off one
/// question: is this region bound to a window, or not?
enum class RegionKind
{
    /// Bound to nothing. Lives on a display, survives focus changes, and is
    /// what the application falls back to.
    Global,
    /// Bound to a specific window. Follows it, shows only while that window is
    /// focused, and dies with it.
    Attached
};

}  // namespace sidescopes
