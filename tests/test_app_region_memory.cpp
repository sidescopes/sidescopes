#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "core/app_region_memory.h"

namespace sidescopes {
namespace {

std::filesystem::path TemporaryFile(const char* name) {
    return std::filesystem::temp_directory_path() / "sidescopes-tests" / name;
}

}  // namespace

TEST_CASE("App region memory round-trips through a file") {
    AppRegionMemory memory;
    memory.Remember("Lightroom Classic", RegionOfInterest{10.0, 20.0, 80.0, 90.0});
    memory.Remember("Capture One", RegionOfInterest{5.0, 5.0, 50.0, 60.0});

    const auto file = TemporaryFile("app-regions.txt");
    REQUIRE(memory.Save(file));

    const AppRegionMemory loaded = AppRegionMemory::Load(file);
    REQUIRE(loaded.All().size() == 2);
    CHECK(loaded.All().front().application == "Capture One");  // most recent first
    const auto region = loaded.Lookup("Lightroom Classic");
    REQUIRE(region.has_value());
    CHECK(region->left_percent == 10.0);
    CHECK(region->bottom_percent == 90.0);
    CHECK_FALSE(loaded.Lookup("GIMP").has_value());

    std::filesystem::remove(file);
}

TEST_CASE("App region memory replaces an application's earlier entry") {
    AppRegionMemory memory;
    memory.Remember("Lightroom Classic", RegionOfInterest{10.0, 20.0, 80.0, 90.0});
    memory.Remember("Capture One", RegionOfInterest{5.0, 5.0, 50.0, 60.0});
    memory.Remember("Lightroom Classic", RegionOfInterest{15.0, 25.0, 85.0, 95.0});

    REQUIRE(memory.All().size() == 2);
    CHECK(memory.All().front().application == "Lightroom Classic");
    CHECK(memory.Lookup("Lightroom Classic")->left_percent == 15.0);
}

TEST_CASE("App region memory drops the least recent entry past the cap") {
    AppRegionMemory memory;
    for (int index = 0; index < 40; ++index)
        memory.Remember("App " + std::to_string(index), RegionOfInterest{1.0, 1.0, 50.0, 50.0});

    CHECK(memory.All().size() == 32);
    CHECK(memory.Lookup("App 39").has_value());
    CHECK_FALSE(memory.Lookup("App 0").has_value());
}

TEST_CASE("App region memory rejects nonsense") {
    AppRegionMemory memory;
    memory.Remember("", RegionOfInterest{10.0, 20.0, 80.0, 90.0});
    memory.Remember("Inverted", RegionOfInterest{80.0, 20.0, 10.0, 90.0});
    memory.Remember("Out of range", RegionOfInterest{-5.0, 0.0, 120.0, 100.0});
    CHECK(memory.All().empty());
}

TEST_CASE("App region memory skips malformed lines and tolerates a missing file") {
    CHECK(AppRegionMemory::Load(TemporaryFile("does-not-exist.txt")).All().empty());

    const auto file = TemporaryFile("malformed-app-regions.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "no tab separator\n"
                        << "\t10 20 80 90\n"
                        << "Short Line\t10 20\n"
                        << "Fine App\t10 20 80 90\n";

    const AppRegionMemory loaded = AppRegionMemory::Load(file);
    REQUIRE(loaded.All().size() == 1);
    CHECK(loaded.All().front().application == "Fine App");

    std::filesystem::remove(file);
}

}  // namespace sidescopes
