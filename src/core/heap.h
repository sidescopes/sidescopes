#pragma once

namespace sidescopes {

/// Hands memory the allocator is holding free back to the operating system.
///
/// Freeing a large buffer does not shrink the process: the allocator keeps its
/// pages cached for the next allocation of that size, and they stay resident
/// and counted against the process. That is the right default in a steady
/// state and the wrong one after the pipeline lets go of whole display frames
/// - three of them, forty megabytes on a common display - which it will not
/// want again until capture resumes. Called on the analysis thread, after the
/// last of the three releases, so the cost never lands on a drawn frame.
void releaseFreeHeap();

}  // namespace sidescopes
