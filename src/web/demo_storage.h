#pragma once

#include <string>

namespace sidescopes {
namespace storage {

/// @brief The demo's preferences, kept between visits.
///
/// The desktop writes a small key=value file and finds it again next time.
/// A page has nowhere to put a file that survives a reload, so the same text
/// is mirrored into the browser's local storage: the application still saves
/// and loads through its own `savePreferences` and `loadPreferences`, against
/// the path `preferencesFilePath()` reports - the SAME seam every platform
/// answers - and these two carry that file's contents in and out of the
/// place a browser will keep it around those calls.
///
/// Local storage rather than IndexedDB deliberately. The file is a couple of
/// kilobytes of text, it is read once at start and written on a change, and
/// it is per-origin - which is exactly the scope a preferences file has. The
/// asynchronous store would buy nothing here and would need the save path to
/// grow a completion.
///
/// Nothing here leaves the browser. It is the same promise the rest of the
/// demo makes about the pictures.

/// The saved text, or empty when this browser has none yet.
[[nodiscard]] std::string readSaved();

/// Keeps @p text for next time. Silently does nothing where local storage is
/// unavailable - a private window with storage disabled, say - because a
/// demo that refused to run without somewhere to save would be worse than
/// one that simply forgets.
void writeSaved(const std::string& text);

}  // namespace storage
}  // namespace sidescopes
