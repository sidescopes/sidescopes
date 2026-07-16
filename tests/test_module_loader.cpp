#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "modules/module_loader.h"
#include "modules/module_registry.h"
#include "temp_file.h"

namespace sidescopes {

using namespace test;

namespace {

#if defined(_WIN32)
constexpr const char* ModuleExtension = ".dll";
#elif defined(__APPLE__)
constexpr const char* ModuleExtension = ".dylib";
#else
constexpr const char* ModuleExtension = ".so";
#endif

}  // namespace

TEST_CASE("loadModulesFrom reports a missing directory")
{
    ModuleRegistry registry;
    const TempDir base("module-loader");
    const std::filesystem::path missing = base.file("not-here");

    CHECK_FALSE(loadModulesFrom(missing, registry));
    CHECK(registry.scopes().empty());
}

TEST_CASE("loadModulesFrom scans a directory and skips a junk module file")
{
    ModuleRegistry registry;
    TempDir dir("module-loader-junk");
    std::ofstream(dir.file(std::string("not-a-module") + ModuleExtension)) << "garbage, not a shared object";

    // The scan succeeds even though its one candidate cannot be loaded: the
    // bad file is logged and skipped, nothing is registered, and nothing
    // throws.
    CHECK(loadModulesFrom(dir.path(), registry));
    CHECK(registry.scopes().empty());
}

}  // namespace sidescopes
