#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include "boundary_input.h"
#include "boundary_properties.h"
#include "core/preferences.h"

namespace sidescopes::test {
namespace {

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    requireBoundary(static_cast<bool>(input), "preferences: cannot read fixture");
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void checkWeights(const std::map<std::string, double>& weights)
{
    for (const auto& [id, value] : weights) {
        requireBoundary(!id.empty() && std::isfinite(value) && value > 0.0 &&
                            value <= std::numeric_limits<float>::max() && static_cast<float>(value) > 0.0f,
                        "preferences: unusable pane weight");
    }
}

void checkValues(const Preferences& preferences)
{
    requireBoundary(preferences.windowWidth > 0 && preferences.windowHeight > 0,
                    "preferences: nonpositive window size");
    requireBoundary(preferences.layoutActiveSlot >= 1 && preferences.layoutActiveSlot <= LayoutPresetSlots,
                    "preferences: invalid active slot");
    requireBoundary(preferences.layoutOrientation >= 0 && preferences.layoutOrientation <= 2,
                    "preferences: invalid orientation");
    requireBoundary(
        preferences.vectorscopeZoom == 1 || preferences.vectorscopeZoom == 2 || preferences.vectorscopeZoom == 4,
        "preferences: invalid zoom");
    requireBoundary(preferences.pins.size() <= MaximumPins &&
                        (preferences.pinComparator == -1 ||
                         (preferences.pinComparator >= 0 &&
                          static_cast<std::size_t>(preferences.pinComparator) < preferences.pins.size())),
                    "preferences: invalid pin reference");
    for (const auto& [id, params] : preferences.scopeParams) {
        for (const auto& [key, value] : params) {
            requireBoundary(!id.empty() && !key.empty() && std::isfinite(value),
                            "preferences: invalid numeric parameter");
        }
    }
    checkWeights(preferences.layoutWeights);
    for (const auto& preset : preferences.layoutPresets) {
        checkWeights(preset.weights);
        requireBoundary(preset.orientation >= 0 && preset.orientation <= 2, "preferences: invalid preset orientation");
        requireBoundary(preset.name == sanitizedPresetName(preset.name), "preferences: preset name is not stable");
    }
}

}  // namespace

void checkPreferencesProperties(std::string_view input, const std::filesystem::path& directory)
{
    const auto raw = directory / "input";
    const auto first = directory / "first";
    const auto second = directory / "second";
    std::ofstream output(raw, std::ios::binary | std::ios::trunc);
    output.write(input.data(), static_cast<std::streamsize>(input.size()));
    output.close();
    requireBoundary(static_cast<bool>(output), "preferences: cannot write fixture");
    const Preferences loaded = loadPreferences(raw);
    checkValues(loaded);
    requireBoundary(savePreferences(loaded, first), "preferences: cannot save parsed input");
    const Preferences reloaded = loadPreferences(first);
    checkValues(reloaded);
    requireBoundary(savePreferences(reloaded, second), "preferences: cannot save roundtrip");
    requireBoundary(readText(first) == readText(second), "preferences: serialization does not stabilize");
    const std::string name = sanitizedPresetName(input);
    requireBoundary(name.size() <= MaximumPresetNameLength && name == sanitizedPresetName(name),
                    "preferences: name sanitation is not bounded and idempotent");
}

}  // namespace sidescopes::test
