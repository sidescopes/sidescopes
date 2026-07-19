#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/diagnostics.h"

namespace {

std::string readAll(const std::string& path)
{
    std::ifstream file(path);
    std::stringstream content;
    content << file.rdbuf();

    return content.str();
}

bool fileExists(const std::string& path)
{
    return std::ifstream(path).good();
}

// Each case uses its own file name: ctest may run cases as parallel
// processes, and a shared sink would interleave runs.
void cleanup(const std::string& path)
{
    sidescopes::diagConfigure({});
    std::remove(path.c_str());
    std::remove((path + ".prev").c_str());
}

}  // namespace

TEST_CASE("A channel list enables exactly the named channels")
{
    const std::string path = "diag-test-list.log";
    sidescopes::diagConfigure({"attach", path});
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Border));
    cleanup(path);
}

TEST_CASE("The word all enables every channel")
{
    const std::string path = "diag-test-all.log";
    sidescopes::diagConfigure({"all", path});
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Border));
    cleanup(path);
}

TEST_CASE("An empty configuration opens no sink and writes no file")
{
    const std::string path = "diag-test-off.log";
    sidescopes::diagConfigure({"", path});
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Border));
    CHECK_FALSE(fileExists(path));
    cleanup(path);
}

TEST_CASE("Unknown tokens and spaces are tolerated, and the header names the outcome")
{
    const std::string path = "diag-test-tokens.log";
    sidescopes::diagConfigure({" attach , bogus", path});
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Border));
    const std::string content = readAll(path);
    CHECK(content.rfind("# sidescopes diagnostics", 0) == 0);
    CHECK(content.find("channels=attach\n") != std::string::npos);
    cleanup(path);
}

TEST_CASE("A logged line carries the timestamp, channel, and message")
{
    const std::string path = "diag-test-line.log";
    sidescopes::diagConfigure({"all", path});
    SS_DIAG(Attach, "value=%d", 7);
    sidescopes::diagConfigure({});  // close, so the read sees flushed content
    const std::string content = readAll(path);
    CHECK(content.find("t=") != std::string::npos);
    CHECK(content.find(" attach value=7\n") != std::string::npos);
    cleanup(path);
}

TEST_CASE("A disabled channel stays out of the file even when called directly")
{
    const std::string path = "diag-test-direct.log";
    sidescopes::diagConfigure({"attach", path});
    sidescopes::diagLogf(sidescopes::DiagChannel::Border, "must not appear");
    sidescopes::diagConfigure({});
    const std::string content = readAll(path);
    CHECK(content.find("must not appear") == std::string::npos);
    cleanup(path);
}

TEST_CASE("A span logs its scope's duration in milliseconds")
{
    const std::string path = "diag-test-span.log";
    sidescopes::diagConfigure({"all", path});
    {
        SS_DIAG_SPAN(Attach, "work");
    }
    sidescopes::diagConfigure({});
    const std::string content = readAll(path);
    CHECK(content.find(" attach work_ms=") != std::string::npos);
    cleanup(path);
}

TEST_CASE("A span on a disabled channel logs nothing")
{
    const std::string path = "diag-test-span-off.log";
    sidescopes::diagConfigure({"attach", path});
    {
        SS_DIAG_SPAN(Border, "quiet");
    }
    sidescopes::diagConfigure({});
    CHECK(readAll(path).find("quiet_ms=") == std::string::npos);
    cleanup(path);
}

TEST_CASE("Timestamps carry microsecond precision")
{
    const std::string path = "diag-test-precision.log";
    sidescopes::diagConfigure({"all", path});
    SS_DIAG(Attach, "mark");
    sidescopes::diagConfigure({});
    const std::string content = readAll(path);
    const std::size_t stamp = content.find("\nt=");
    REQUIRE(stamp != std::string::npos);
    const std::size_t point = content.find('.', stamp);
    const std::size_t space = content.find(' ', stamp);
    REQUIRE(point != std::string::npos);
    REQUIRE(space != std::string::npos);
    CHECK(space - point - 1 == 6);
    cleanup(path);
}

TEST_CASE("With flushing off, lines still land once the sink closes")
{
    const std::string path = "diag-test-noflush.log";
    sidescopes::diagConfigure({"all", path, false});
    SS_DIAG(Attach, "buffered=1");
    sidescopes::diagConfigure({});
    CHECK(readAll(path).find("buffered=1") != std::string::npos);
    cleanup(path);
}

TEST_CASE("Reconfiguring rotates the previous log to .prev")
{
    const std::string path = "diag-test-rotate.log";
    sidescopes::diagConfigure({"attach", path});
    SS_DIAG(Attach, "first run");
    sidescopes::diagConfigure({"attach", path});
    CHECK(fileExists(path + ".prev"));
    CHECK(readAll(path + ".prev").find("first run") != std::string::npos);
    CHECK(readAll(path).find("first run") == std::string::npos);
    cleanup(path);
}
