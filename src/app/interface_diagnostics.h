#pragma once

namespace sidescopes {

/// Records @p message, an error the interface toolkit reported about its own
/// use, on the Interface diagnostic channel.
///
/// The toolkit checks things no test of ours can - that a window was asked to
/// grow around something submitted, that every Begin has its End - and says so
/// in a window over the interface. That window is compiled out by a flag, it
/// is only ever seen by whoever is sitting in front of the build, and one of
/// these went unread long enough to ship. In a recording it is a line in the
/// file the user attaches to a report.
///
/// These arrive from inside a draw, so the same error repeats every frame for
/// as long as the code that raises it runs. Only a message the current
/// recording has not been told is written.
void reportInterfaceError(const char* message);

}  // namespace sidescopes
