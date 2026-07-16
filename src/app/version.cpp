#include "app/version.h"

#include <string>
#include <string_view>

namespace sidescopes {

VersionInfo describeVersion(const char* projectVersion, const char* gitDescribe)
{
    const std::string version = projectVersion != nullptr ? projectVersion : "";
    const std::string describe = gitDescribe != nullptr ? gitDescribe : "";
    const std::string tag = "v" + version;

    // A release build: no describe at all (a tarball with no git), or the
    // checkout sits exactly on this version's tag.
    if (describe.empty() || describe == tag) {
        return {version, false};
    }

    constexpr std::string_view DirtySuffix = "-dirty";
    bool dirty = false;
    std::string_view core = describe;
    if (core.size() >= DirtySuffix.size() && core.substr(core.size() - DirtySuffix.size()) == DirtySuffix) {
        dirty = true;
        core.remove_suffix(DirtySuffix.size());
    }

    // With the dirty marker stripped the checkout may still sit on the tag
    // (`v<version>-dirty`): local changes only, no commits since the tag, so
    // there is no hash to show - just the marker.
    std::string hash;
    if (core != tag) {
        const auto marker = core.rfind("-g");
        if (marker != std::string_view::npos) {
            // `v<version>-<n>-g<hash>`: the short hash is the token after the
            // last `-g`, up to any following separator.
            const std::string_view rest = core.substr(marker + 2);
            const auto separator = rest.find('-');
            hash = std::string(separator == std::string_view::npos ? rest : rest.substr(0, separator));
        } else {
            // No `-g`: `git describe --always` fell back to a bare short hash
            // because no tag was reachable.
            hash = std::string(core);
        }
    }

    std::string display = version + " (" + hash;
    if (dirty) {
        display += '*';
    }
    display += ')';

    return {display, true};
}

}  // namespace sidescopes
