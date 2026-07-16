#include <catch2/catch_test_macros.hpp>

#include "app/version.h"

namespace sidescopes {
namespace {

TEST_CASE("An empty describe is a clean release build")
{
    const VersionInfo info = describeVersion("0.1.0", "");
    CHECK(info.display == "0.1.0");
    CHECK_FALSE(info.development);
}

TEST_CASE("Sitting exactly on the release tag is a clean release build")
{
    const VersionInfo info = describeVersion("0.1.0", "v0.1.0");
    CHECK(info.display == "0.1.0");
    CHECK_FALSE(info.development);
}

TEST_CASE("Commits past the tag show the short hash")
{
    const VersionInfo info = describeVersion("0.1.0", "v0.1.0-37-g68524d2");
    CHECK(info.display == "0.1.0 (68524d2)");
    CHECK(info.development);
}

TEST_CASE("A dirty tree past the tag marks the hash with an asterisk")
{
    const VersionInfo info = describeVersion("0.1.0", "v0.1.0-37-g68524d2-dirty");
    CHECK(info.display == "0.1.0 (68524d2*)");
    CHECK(info.development);
}

TEST_CASE("The project version, not the tag name, drives the display")
{
    const VersionInfo info = describeVersion("0.2.0", "v0.2.0-12-gabc1234");
    CHECK(info.display == "0.2.0 (abc1234)");
    CHECK(info.development);
}

TEST_CASE("A bare hash from --always shows without a tag")
{
    const VersionInfo info = describeVersion("0.1.0", "68524d2");
    CHECK(info.display == "0.1.0 (68524d2)");
    CHECK(info.development);
}

TEST_CASE("A dirty bare hash keeps the asterisk")
{
    const VersionInfo info = describeVersion("0.1.0", "68524d2-dirty");
    CHECK(info.display == "0.1.0 (68524d2*)");
    CHECK(info.development);
}

TEST_CASE("Local changes on the tag show only the dirty marker")
{
    const VersionInfo info = describeVersion("0.1.0", "v0.1.0-dirty");
    CHECK(info.display == "0.1.0 (*)");
    CHECK(info.development);
}

TEST_CASE("A null describe degrades to a release build")
{
    const VersionInfo info = describeVersion("0.1.0", nullptr);
    CHECK(info.display == "0.1.0");
    CHECK_FALSE(info.development);
}

}  // namespace
}  // namespace sidescopes
