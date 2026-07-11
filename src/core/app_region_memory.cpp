#include "core/app_region_memory.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sidescopes {
namespace {

// Enough for every editor anyone runs, small enough to scan linearly.
constexpr std::size_t kMaximumEntries = 32;

bool PlausibleRegion(const RegionOfInterest& region) {
    const auto in_range = [](double value) { return value >= 0.0 && value <= 100.0; };
    return in_range(region.left_percent) && in_range(region.top_percent) &&
           in_range(region.right_percent) && in_range(region.bottom_percent) &&
           region.left_percent < region.right_percent && region.top_percent < region.bottom_percent;
}

}  // namespace

void AppRegionMemory::Remember(std::string application, const RegionOfInterest& region) {
    if (application.empty() || !PlausibleRegion(region)) return;
    // Tabs separate the fields on disk; an application name is free text
    // otherwise.
    std::replace(application.begin(), application.end(), '\t', ' ');
    std::erase_if(entries_,
                  [&](const RememberedRegion& entry) { return entry.application == application; });
    entries_.insert(entries_.begin(), RememberedRegion{std::move(application), region});
    if (entries_.size() > kMaximumEntries) entries_.resize(kMaximumEntries);
}

std::optional<RegionOfInterest> AppRegionMemory::Lookup(const std::string& application) const {
    for (const RememberedRegion& entry : entries_) {
        if (entry.application == application) return entry.region;
    }
    return std::nullopt;
}

AppRegionMemory AppRegionMemory::Load(const std::filesystem::path& file) {
    AppRegionMemory memory;
    std::ifstream input(file);
    if (!input) return memory;

    std::string line;
    while (std::getline(input, line) && memory.entries_.size() < kMaximumEntries) {
        const auto separator = line.find('\t');
        if (separator == std::string::npos || separator == 0) continue;
        RememberedRegion entry;
        entry.application = line.substr(0, separator);
        std::istringstream numbers(line.substr(separator + 1));
        if (!(numbers >> entry.region.left_percent >> entry.region.top_percent >>
              entry.region.right_percent >> entry.region.bottom_percent))
            continue;
        if (!PlausibleRegion(entry.region)) continue;
        memory.entries_.push_back(std::move(entry));
    }
    return memory;
}

bool AppRegionMemory::Save(const std::filesystem::path& file) const {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);

    std::ostringstream out;
    for (const RememberedRegion& entry : entries_) {
        out << entry.application << '\t' << entry.region.left_percent << ' '
            << entry.region.top_percent << ' ' << entry.region.right_percent << ' '
            << entry.region.bottom_percent << '\n';
    }

    std::ofstream output(file, std::ios::trunc);
    if (!output) return false;
    output << out.str();
    return static_cast<bool>(output);
}

}  // namespace sidescopes
