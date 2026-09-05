#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace sidescopes::test {

// Shared by fixed-seed Catch tests and optional coverage-guided fuzz targets.
// Violated properties throw with a stable label; the fuzzer retains the input.
void checkPreferencesProperties(std::string_view input, const std::filesystem::path& directory);
void checkGeometryProperties(std::span<const uint8_t> input);
void checkModuleProperties(std::span<const uint8_t> input);

}  // namespace sidescopes::test
