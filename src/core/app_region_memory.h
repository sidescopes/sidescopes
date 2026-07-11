#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/analysis_worker.h"

namespace sidescopes {

// One application's remembered scoped region.
struct RememberedRegion {
    std::string application;
    RegionOfInterest region;
};

// Remembers the last confirmed region per application, most recent first,
// so returning to an editor re-offers the region that was scoped there
// before. Remembering never switches the region by itself; the entries
// surface as picker suggestions.
class AppRegionMemory {
public:
    // Replaces any earlier entry for the application and moves it to the
    // front; the least recently used entry falls off past the cap.
    void Remember(std::string application, const RegionOfInterest& region);

    [[nodiscard]] std::optional<RegionOfInterest> Lookup(const std::string& application) const;

    [[nodiscard]] const std::vector<RememberedRegion>& All() const { return entries_; }

    // A missing or unreadable file yields an empty memory; malformed lines
    // are skipped.
    static AppRegionMemory Load(const std::filesystem::path& file);

    // Creates parent directories as needed. Returns false when writing
    // failed.
    [[nodiscard]] bool Save(const std::filesystem::path& file) const;

private:
    std::vector<RememberedRegion> entries_;
};

}  // namespace sidescopes
