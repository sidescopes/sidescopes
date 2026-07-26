#pragma once

namespace sidescopes {

/// Runs @p body with a drain point open around it, and closes it afterwards.
///
/// Cocoa hands objects back autoreleased - owned by whatever pool is open on
/// the thread - and an application driving its own loop has to open one per
/// iteration. Without it every object a frame touched lives as long as the
/// process: measured on macOS, each region pick left a whole display of window
/// backing store behind, so a session grew by that much every time the picker
/// was used.
///
/// A function rather than a scope guard because the pool is the platform's
/// own construct and belongs in the platform's own language; @p body takes
/// @p context so nothing needs to be allocated to call it. Nothing outside
/// macOS defers releases this way, so it costs a call there.
void runInFramePool(void (*body)(void* context), void* context);

}  // namespace sidescopes
