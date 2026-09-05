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

TEST_CASE("loadModulesFrom reports a missing directory with a Unicode path")
{
    ModuleRegistry registry;
    const TempDir base("module-loader-unicode");
    const auto missing = base.path() / u8"zażółć-目录";

    CHECK_FALSE(loadModulesFrom(missing, registry));
    CHECK(registry.scopes().empty());
}

TEST_CASE("Temporary module directories with the same name coexist independently")
{
    const TempDir first("module-loader-shared");
    const auto module = first.file(std::string("first") + ModuleExtension);
    std::ofstream(module) << "first module";
    std::filesystem::path secondPath;
    {
        const TempDir second("module-loader-shared");
        secondPath = second.path();
        REQUIRE(first.path() != second.path());
        CHECK(std::filesystem::exists(module));
        CHECK(std::filesystem::is_empty(second.path()));
    }
    CHECK_FALSE(std::filesystem::exists(secondPath));
    CHECK(std::filesystem::exists(module));
}

}  // namespace sidescopes
