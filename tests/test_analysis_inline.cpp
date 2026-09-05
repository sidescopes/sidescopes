#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>

#include "core/analysis_worker.h"
#include "test_frame.h"

namespace sidescopes {
using namespace test;

namespace {
const std::string VectorscopeId = "org.sidescopes.vectorscope";
constexpr RegionOfInterest WholeFrame{0.0, 0.0, 100.0, 100.0};
}  // namespace

// The inline mode exists for a host with no threads to give: a page, where
// threads would mean SharedArrayBuffer and cross-origin isolation. The passes
// have to be the SAME passes, so these assert against the threaded mode's own
// behaviour rather than against a lower bar.

TEST_CASE("An inline worker analyses on the caller's thread")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.startInline();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));

    // No thread runs, so nothing has happened yet: the pass belongs to the
    // caller and has not been asked for.
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    CHECK_FALSE(worker.fetchOutput(seen, output));

    worker.pump();

    REQUIRE(worker.fetchOutput(seen, output));
    REQUIRE(output.images.count(VectorscopeId) == 1);
    CHECK(output.images.at(VectorscopeId).width > 0);
    CHECK(output.framesProcessed == 1);
}

TEST_CASE("An inline pump never blocks waiting for a frame")
{
    // The caller is a frame loop. A pump with nothing to take must return, not
    // wait out a timeout the way the threaded pass may.
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.startInline();

    const auto before = std::chrono::steady_clock::now();
    worker.pump();
    const auto elapsed = std::chrono::steady_clock::now() - before;

    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 50);
}

TEST_CASE("Pumping a worker that was never started inline does nothing")
{
    // A host may pump unconditionally, so this has to be inert rather than a
    // second way to run a pass behind the thread's back.
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    worker.pump();

    uint64_t seen = 0;
    AnalysisWorker::Output output;
    CHECK_FALSE(worker.fetchOutput(seen, output));
}

TEST_CASE("An inline worker skips a frame whose scoped content is unchanged")
{
    // The same content-hash skip the threaded pass makes. Asserted because the
    // skip is loop-carried state, and hoisting it out of run() into the pass
    // is exactly the kind of move that quietly drops it.
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.startInline();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    worker.pump();
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(worker.fetchOutput(seen, output));

    // Identical pixels: taken, hashed, and found to say nothing new.
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 2));
    worker.pump();
    CHECK_FALSE(worker.fetchOutput(seen, output));

    // Different pixels: analysed.
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{0, 191, 0}, 3));
    worker.pump();
    CHECK(worker.fetchOutput(seen, output));
}

TEST_CASE("An inline worker stops pumping until restarted")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.startInline();
    worker.stop();
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    worker.pump();
    CHECK(worker.consumedFrameSequence() == 0);
    worker.startInline();
    worker.pump();
    CHECK(worker.consumedFrameSequence() == 1);
}

TEST_CASE("Starting a thread cannot replace an active inline worker")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.startInline();
    worker.start();
    // A threaded implementation would consume this before the explicit pump.
    mailbox.publish(makeSolidFrameBuffer(16, 16, Color{191, 0, 0}, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(worker.consumedFrameSequence() == 0);
    worker.pump();
    CHECK(worker.consumedFrameSequence() == 1);
}

TEST_CASE("Removing a configured parameter restores the descriptor default")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.startInline();
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    worker.pump();
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(worker.fetchOutput(seen, output));
    const auto original = output.images.at(VectorscopeId).rgba;
    settings.scopeParams[VectorscopeId]["gamma"] = 1.4;
    worker.updateSettings(settings);
    worker.pump();
    REQUIRE(worker.fetchOutput(seen, output));
    CHECK(output.images.at(VectorscopeId).rgba != original);
    settings.scopeParams.clear();
    worker.updateSettings(settings);
    worker.pump();
    REQUIRE(worker.fetchOutput(seen, output));
    CHECK(output.images.at(VectorscopeId).rgba == original);
}

TEST_CASE("An output callback can fetch the image it announces")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    bool fetched = false;
    worker.setOutputCallback([&] {
        uint64_t seen = 0;
        AnalysisWorker::Output output;
        fetched = worker.fetchOutput(seen, output);
    });
    worker.startInline();
    mailbox.publish(makeSolidFrameBuffer(16, 16, Color{191, 0, 0}, 1));
    worker.pump();
    CHECK(fetched);
}

}  // namespace sidescopes
