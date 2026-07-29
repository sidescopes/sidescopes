#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "app/interface_diagnostics.h"
#include "core/diagnostics.h"

namespace {

std::string readAll(const std::string& path)
{
    std::ifstream file(path);
    std::stringstream content;
    content << file.rdbuf();

    return content.str();
}

std::size_t occurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) {
        ++count;
    }

    return count;
}

void cleanup(const std::string& path)
{
    sidescopes::diagConfigure({});
    std::remove(path.c_str());
}

}  // namespace

TEST_CASE("A toolkit error is recorded on its own channel")
{
    const std::string path = "diag-test-interface.log";
    sidescopes::diagConfigure({"interface", path});
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Interface));
    // It is nobody else's subject: a recording asked for modules must not be
    // told about the interface, and one asked for the interface must not have
    // to read the rest to find it.
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Modules));

    sidescopes::reportInterfaceError("Code uses SetCursorPos() to extend boundaries.");
    sidescopes::diagConfigure({});

    const std::string content = readAll(path);
    CHECK(content.find(" interface toolkit_error msg=\"Code uses SetCursorPos() to extend boundaries.\"\n") !=
          std::string::npos);
    cleanup(path);
}

TEST_CASE("The same toolkit error repeating every frame is recorded once")
{
    // THE REASON THIS IS NOT OPTIONAL. These arrive from inside a draw, so the
    // one that shipped would have repeated for every frame the popup was open
    // - thousands of identical lines burying whatever else the recording was
    // opened to catch.
    const std::string path = "diag-test-interface-repeat.log";
    sidescopes::diagConfigure({"interface", path});

    for (int frame = 0; frame < 200; ++frame) {
        sidescopes::reportInterfaceError("Missing End()");
    }
    // A DIFFERENT error is a different fault and is always worth a line.
    sidescopes::reportInterfaceError("Missing EndChild()");
    sidescopes::diagConfigure({});

    const std::string content = readAll(path);
    CHECK(occurrences(content, "Missing End()") == 1);
    CHECK(occurrences(content, "Missing EndChild()") == 1);
    cleanup(path);
}

TEST_CASE("A fault that began before recording is stated to the recording")
{
    // The half of the dedupe that is easy to lose: a value that advanced while
    // nothing was recording would otherwise be held as already told, and the
    // reader of the new recording concludes nothing was wrong.
    const std::string path = "diag-test-interface-restate.log";
    sidescopes::reportInterfaceError("Missing End()");  // nothing is recording

    sidescopes::diagConfigure({"interface", path});
    sidescopes::reportInterfaceError("Missing End()");
    sidescopes::diagConfigure({});

    CHECK(occurrences(readAll(path), "Missing End()") == 1);
    cleanup(path);
}
