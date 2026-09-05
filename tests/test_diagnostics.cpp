#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/diagnostics.h"
#include "temp_file.h"

namespace {

std::string readAll(const std::string& path)
{
    std::ifstream file(std::filesystem::path{std::u8string(path.begin(), path.end())});
    std::stringstream content;
    content << file.rdbuf();

    return content.str();
}

bool fileExists(const std::string& path)
{
    return std::ifstream(std::filesystem::path{std::u8string(path.begin(), path.end())}).good();
}

// The rotated name keeps the extension: diag-test-x.log becomes
// diag-test-x.prev.log.
std::string previousOf(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');

    return path.substr(0, dot) + ".prev" + path.substr(dot);
}

// Own both the active and rotated logs, and close the sink before TempDir
// removes them. Destruction also runs when an assertion unwinds the test.
struct DiagnosticFiles
{
    sidescopes::test::TempDir directory{"diagnostics"};

    DiagnosticFiles()
    {
        sidescopes::diagConfigure({});
    }

    ~DiagnosticFiles()
    {
        sidescopes::diagConfigure({});
    }

    std::string path(std::string_view name) const
    {
        const auto file = directory.path() / std::filesystem::path{std::u8string(name.begin(), name.end())};
        const auto utf8 = file.u8string();
        return {utf8.begin(), utf8.end()};
    }
};

}  // namespace

TEST_CASE("A channel list enables exactly the named channels")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-list.log");
    sidescopes::diagConfigure({"attach", path});
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Border));
}

TEST_CASE("The word all enables every channel")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-all.log");
    sidescopes::diagConfigure({"all", path});
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Border));
    // The menu's Record toggle records with "all", so the perf channel it
    // never names individually still comes on with the rest.
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Perf));
}

TEST_CASE("The perf channel records timing lines under its own name")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-perf.log");
    sidescopes::diagConfigure({"perf", path});
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Perf));
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    SS_DIAG(Perf, "frame body_ms=%.1f present_ms=%.1f", 4.0, 12.0);
    sidescopes::diagConfigure({});
    const std::string content = readAll(path);
    CHECK(content.find(" perf frame body_ms=4.0 present_ms=12.0\n") != std::string::npos);
}

TEST_CASE("An empty configuration opens no sink and writes no file")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-off.log");
    sidescopes::diagConfigure({"", path});
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Border));
    CHECK_FALSE(fileExists(path));
}

TEST_CASE("Unknown tokens and spaces are tolerated, and the header names the outcome")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-tokens.log");
    sidescopes::diagConfigure({" attach , bogus", path});
    CHECK(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Border));
    const std::string content = readAll(path);
    CHECK(content.rfind("# sidescopes diagnostics", 0) == 0);
    CHECK(content.find("channels=attach\n") != std::string::npos);
}

TEST_CASE("A list of only unknown channels leaves the header but records off")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-unknown.log");
    sidescopes::diagConfigure({"bogus", path});
    CHECK_FALSE(sidescopes::diagRecording());
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Attach));
    CHECK(readAll(path).find("channels=(none)") != std::string::npos);
}

TEST_CASE("A logged line carries the timestamp, channel, and message")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-line.log");
    sidescopes::diagConfigure({"all", path});
    SS_DIAG(Attach, "value=%d", 7);
    sidescopes::diagConfigure({});  // close, so the read sees flushed content
    const std::string content = readAll(path);
    CHECK(content.find("t=") != std::string::npos);
    CHECK(content.find(" attach value=7\n") != std::string::npos);
}

TEST_CASE("A disabled channel stays out of the file even when called directly")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-direct.log");
    sidescopes::diagConfigure({"attach", path});
    sidescopes::diagEmit(sidescopes::DiagChannel::Border, "must not appear");
    sidescopes::diagConfigure({});
    const std::string content = readAll(path);
    CHECK(content.find("must not appear") == std::string::npos);
}

TEST_CASE("A span logs its scope's duration in milliseconds")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-span.log");
    sidescopes::diagConfigure({"all", path});
    {
        SS_DIAG_SPAN(Attach, "work");
    }
    sidescopes::diagConfigure({});
    const std::string content = readAll(path);
    CHECK(content.find(" attach work_ms=") != std::string::npos);
}

TEST_CASE("A span on a disabled channel logs nothing")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-span-off.log");
    sidescopes::diagConfigure({"attach", path});
    {
        SS_DIAG_SPAN(Border, "quiet");
    }
    sidescopes::diagConfigure({});
    CHECK(readAll(path).find("quiet_ms=") == std::string::npos);
}

TEST_CASE("Timestamps carry microsecond precision")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-precision.log");
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
}

TEST_CASE("With flushing on close only, lines still land there")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-noflush.log");
    sidescopes::diagConfigure({"all", path, sidescopes::DiagFlush::OnClose});
    SS_DIAG(Attach, "buffered=1");
    sidescopes::diagConfigure({});
    CHECK(readAll(path).find("buffered=1") != std::string::npos);
}

TEST_CASE("Per-line flushing shows a line before the sink closes")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-perline.log");
    sidescopes::diagConfigure({"all", path, sidescopes::DiagFlush::EveryLine});
    SS_DIAG(Attach, "eager=1");
    CHECK(readAll(path).find("eager=1") != std::string::npos);
    sidescopes::diagConfigure({});
}

TEST_CASE("Interval flushing lands a line once the interval passes")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-interval.log");
    sidescopes::diagConfigure({"all", path});
    SS_DIAG(Attach, "beat=1");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    SS_DIAG(Attach, "beat=2");
    // The second line crossed the interval, so both are flushed while
    // the sink stays open.
    const std::string content = readAll(path);
    CHECK(content.find("beat=1") != std::string::npos);
    CHECK(content.find("beat=2") != std::string::npos);
    sidescopes::diagConfigure({});
}

TEST_CASE("A path into a missing directory creates it")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-dir/nested.log");
    sidescopes::diagConfigure({"all", path});
    CHECK(sidescopes::diagRecording());
    CHECK(fileExists(path));
    sidescopes::diagConfigure({});
}

TEST_CASE("Recording state and path follow the configuration")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-state.log");
    sidescopes::diagConfigure({"all", path});
    CHECK(sidescopes::diagRecording());
    CHECK(sidescopes::diagLogPath() == path);
    sidescopes::diagConfigure({});
    CHECK_FALSE(sidescopes::diagRecording());
    CHECK_FALSE(sidescopes::diagLogPath().empty());
}

TEST_CASE("A recording is told what settled before it opened")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-report.log");
    // The state settles with nothing recording - a capture format decided in
    // the first second of a run, long before anyone reaches the menu.
    int depth = 10;
    const sidescopes::DiagRegistration report =
        sidescopes::diagAddStateReport([depth] { SS_DIAG(Attach, "depth=%d", depth); });

    sidescopes::diagConfigure({"all", path});
    sidescopes::diagConfigure({});
    const std::string content = readAll(path);
    CHECK(content.find("# state when this recording opened") != std::string::npos);
    CHECK(content.find(" attach depth=10\n") != std::string::npos);
}

TEST_CASE("Every recording is told, not only the first")
{
    const DiagnosticFiles files;
    const std::string first = files.path("diag-test-report-first.log");
    const std::string second = files.path("diag-test-report-second.log");
    const sidescopes::DiagRegistration report = sidescopes::diagAddStateReport([] { SS_DIAG(Attach, "in force"); });

    sidescopes::diagConfigure({"all", first});
    sidescopes::diagConfigure({});
    sidescopes::diagConfigure({"all", second});
    sidescopes::diagConfigure({});
    CHECK(readAll(first).find("in force") != std::string::npos);
    CHECK(readAll(second).find("in force") != std::string::npos);
}

TEST_CASE("A report goes when its registration does")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-report-gone.log");
    {
        const sidescopes::DiagRegistration report = sidescopes::diagAddStateReport([] { SS_DIAG(Attach, "stale"); });
    }
    sidescopes::diagConfigure({"all", path});
    sidescopes::diagConfigure({});
    CHECK(readAll(path).find("stale") == std::string::npos);
}

TEST_CASE("Nothing is reported while nothing records")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-report-off.log");
    const sidescopes::DiagRegistration report = sidescopes::diagAddStateReport([] { SS_DIAG(Attach, "unwanted"); });

    sidescopes::diagConfigure({"", path});
    CHECK_FALSE(fileExists(path));
}

TEST_CASE("A value seen while nothing records is stated to the recording that follows")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-onchange-late.log");
    sidescopes::DiagOnChange<int> depth(sidescopes::DiagChannel::Attach);
    // Nothing is recording, so nothing is stated - and nothing is remembered
    // either, which is what leaves something to say later.
    CHECK_FALSE(depth.shouldLog(10));
    CHECK_FALSE(depth.shouldLog(10));

    sidescopes::diagConfigure({"all", path});
    CHECK(depth.shouldLog(10));
    sidescopes::diagConfigure({});
}

TEST_CASE("A second recording is told the value the first was told")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-onchange-again.log");
    sidescopes::DiagOnChange<int> depth(sidescopes::DiagChannel::Attach);

    sidescopes::diagConfigure({"all", path});
    CHECK(depth.shouldLog(10));
    CHECK_FALSE(depth.shouldLog(10));
    sidescopes::diagConfigure({});

    // Unchanged all the while: without a recording of its own to measure
    // against, the second log would open silent about a value that is as true
    // as it ever was.
    sidescopes::diagConfigure({"all", path});
    CHECK(depth.shouldLog(10));
    sidescopes::diagConfigure({});
}

TEST_CASE("A value is stated once to a recording, and again when it changes")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-onchange-dedupe.log");
    sidescopes::DiagOnChange<int> depth(sidescopes::DiagChannel::Attach);

    sidescopes::diagConfigure({"all", path});
    CHECK(depth.shouldLog(10));
    CHECK_FALSE(depth.shouldLog(10));
    CHECK(depth.shouldLog(8));
    CHECK_FALSE(depth.shouldLog(8));
    CHECK(depth.shouldLog(10));
    sidescopes::diagConfigure({});
}

TEST_CASE("Reconfiguring rotates the previous log, extension kept")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-rotate.log");
    sidescopes::diagConfigure({"attach", path});
    SS_DIAG(Attach, "first run");
    sidescopes::diagConfigure({"attach", path});
    CHECK(fileExists(previousOf(path)));
    CHECK(readAll(previousOf(path)).find("first run") != std::string::npos);
    CHECK(readAll(path).find("first run") == std::string::npos);
}

TEST_CASE("A report captured before deregistration cannot invoke its destroyed owner")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-report-snapshot.log");
    sidescopes::diagConfigure({});
    std::promise<void> entered;
    std::promise<void> release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    const auto first = sidescopes::diagAddStateReport([&] {
        entered.set_value();
        releaseFuture.wait();
    });
    int calls = 0;
    auto second = sidescopes::diagAddStateReport([&] { ++calls; });
    std::thread configure([&] { sidescopes::diagConfigure({"all", path}); });
    const auto started = enteredFuture.wait_for(std::chrono::seconds(2));
    second = {};
    release.set_value();
    configure.join();
    CHECK(started == std::future_status::ready);
    CHECK(calls == 0);
}

TEST_CASE("Deregistering joins an in-flight report before its captured state can die")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-report-join.log");
    sidescopes::diagConfigure({});
    std::promise<void> entered;
    std::promise<void> release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    std::optional report{sidescopes::diagAddStateReport([&] {
        entered.set_value();
        releaseFuture.wait();
        SS_DIAG(Attach, "owner still alive");
    })};
    std::thread configure([&] { sidescopes::diagConfigure({"all", path}); });
    const auto started = enteredFuture.wait_for(std::chrono::seconds(2));
    std::promise<void> retiring;
    auto retiringFuture = retiring.get_future();
    auto retired = std::async(std::launch::async, [&] {
        retiring.set_value();
        report.reset();
    });
    retiringFuture.wait();
    const auto premature = retired.wait_for(std::chrono::milliseconds(50));
    release.set_value();
    retired.get();
    configure.join();
    sidescopes::diagConfigure({});
    CHECK(started == std::future_status::ready);
    CHECK(premature == std::future_status::timeout);
    CHECK(readAll(path).find("owner still alive") != std::string::npos);
}

TEST_CASE("A state report does not hold the sink while waiting for a producer")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-report-lock-order.log");
    sidescopes::diagConfigure({});
    std::promise<void> entered;
    std::promise<void> release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    const auto report = sidescopes::diagAddStateReport([&] {
        entered.set_value();
        releaseFuture.wait();
    });
    std::thread configure([&] { sidescopes::diagConfigure({"all", path}); });
    const auto started = enteredFuture.wait_for(std::chrono::seconds(2));
    auto producer = std::async(std::launch::async, [] { SS_DIAG(Perf, "producer progressed"); });
    const auto progressed = producer.wait_for(std::chrono::seconds(2));
    release.set_value();
    producer.get();
    configure.join();
    sidescopes::diagConfigure({});
    CHECK(started == std::future_status::ready);
    CHECK(progressed == std::future_status::ready);
    CHECK(readAll(path).find("producer progressed") != std::string::npos);
}

TEST_CASE("Concurrent emitters can survive repeated recording replacement")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-concurrent.log");
    sidescopes::diagConfigure({"all", path});
    std::atomic<bool> stop{false};
    std::atomic<int> emitted{0};
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            while (!stop.load(std::memory_order_relaxed)) {
                SS_DIAG(Perf, "worker=%d payload=complete", worker);
                (void)sidescopes::diagRecording();
                (void)sidescopes::diagLogPath();
                emitted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (emitted.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }
    for (int opening = 0; opening < 40; ++opening) {
        sidescopes::diagConfigure({});
        sidescopes::diagConfigure(
            {"all", path, opening % 2 == 0 ? sidescopes::DiagFlush::EveryLine : sidescopes::DiagFlush::Interval});
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& worker : workers) {
        worker.join();
    }
    SS_DIAG(Attach, "final recording");
    sidescopes::diagConfigure({});
    const std::string log = readAll(path);
    CHECK(log.starts_with("# sidescopes diagnostics"));
    CHECK(log.find(" attach final recording\n") != std::string::npos);
    std::istringstream lines(log);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("worker=") != std::string::npos) {
            CHECK(line.starts_with("t="));
            CHECK(line.ends_with("payload=complete"));
        }
    }
}

TEST_CASE("Invalid diagnostic channel values are disabled")
{
    sidescopes::diagConfigure({});
    CHECK_FALSE(sidescopes::diagEnabled(sidescopes::DiagChannel::Count));
    CHECK_FALSE(sidescopes::diagEnabled(static_cast<sidescopes::DiagChannel>(-1)));
    sidescopes::diagEmit(sidescopes::DiagChannel::Count, "ignored");
}

TEST_CASE("Diagnostic files and rotations accept UTF-8 paths")
{
    const DiagnosticFiles files;
    const std::string path = files.path("diag-test-\xC5\x82\xC3\xB3.log");
    const std::filesystem::path file = std::u8string(path.begin(), path.end());
    const std::string previousText = previousOf(path);
    const std::filesystem::path previous = std::u8string(previousText.begin(), previousText.end());
    sidescopes::diagConfigure({"attach", path});
    CHECK(sidescopes::diagRecording());
    SS_DIAG(Attach, "previous recording");
    sidescopes::diagConfigure({"attach", path});
    SS_DIAG(Attach, "current recording");
    sidescopes::diagConfigure({});
    CHECK(std::filesystem::exists(file));
    CHECK(std::filesystem::exists(previous));
    std::ifstream rotated(previous);
    std::stringstream content;
    content << rotated.rdbuf();
    CHECK(content.str().find("previous recording") != std::string::npos);
    rotated.close();
}
