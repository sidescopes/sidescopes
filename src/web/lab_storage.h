#pragma once

#include <optional>

#include "core/preferences.h"

namespace sidescopes {
namespace storage {

/// @brief The lab's preferences, kept between visits.
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
/// lab makes about the pictures.

/// Restores browser storage through the shared preferences parser. Nothing
/// saved, or storage unavailable, returns no previous session.
[[nodiscard]] std::optional<Preferences> load();

/// Writes through the shared preferences serializer and mirrors the file into
/// browser storage. Storage failures leave the current session usable.
void save(const Preferences& preferences);

}  // namespace storage
}  // namespace sidescopes
