#pragma once

#include <string>

namespace sidescopes {

/// A version string ready for display, plus whether this is a development
/// build (any checkout that is not sitting cleanly on a release tag).
struct VersionInfo
{
    std::string display;
    bool development = false;
};

/// Derives the display version from the compiled-in project version and the
/// build-time `git describe --tags --always --dirty` output. A release build
/// - an empty describe, or one that is exactly `v<projectVersion>` - shows
/// the bare version. Anything else is a development build showing the short
/// commit hash, with a trailing `*` when the working tree was dirty.
[[nodiscard]] VersionInfo describeVersion(const char* projectVersion, const char* gitDescribe);

}  // namespace sidescopes
