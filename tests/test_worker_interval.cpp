#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>

#include "../bench/worker_interval.h"
#include "core/analysis_worker.h"
#include "test_frame.h"

namespace sidescopes {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr char ScopeId[] = "org.sidescopes.vectorscope";

struct HeldFrame
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool timedOut = false;
    bool stopEntered = false;
    bool endpointBeforePublication = false;
    int publications = 0;
    Clock::time_point publishedAt;
    Clock::time_point endpoint;
    std::exception_ptr stopError;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    std::thread stopper;

    ~HeldFrame()
    {
        release();
        if (stopper.joinable()) {
            stopper.join();
        }
        worker.stop();
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }

    void start()
    {
        AnalysisSettings settings;
        settings.region = RegionOfInterest{};
        settings.enabledScopes = {ScopeId};
        worker.updateSettings(settings);
        // Hold the real worker before hashing, built-in accumulation and
        // publication, without replacing any scope or output implementation.
        worker.setFrameRegionResolverFactory([this] {
            return [this](const FrameRegionRequest& request) {
                std::unique_lock lock(mutex);
                entered = true;
                changed.notify_all();
                timedOut = !changed.wait_for(lock, 5s, [this] { return released; });
                return FrameRegionResolution{FrameRegionResolution::Mode::Configured, {}, request.selectionRevision};
            };
        });
        worker.setOutputCallback([this] {
            std::lock_guard lock(mutex);
            ++publications;
            publishedAt = Clock::now();
        });
        worker.start();
        // One delivered input; the source sequence deliberately differs.
        mailbox.publish(test::makeSolidFrameBuffer(8, 8, Color{100, 50, 20}, 41));
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 5s, [this] { return entered; }));
    }

    // Announces entry to the real join. The held frame cannot finish before
    // this signal, so reading the endpoint first fails deterministically.
    void stop()
    {
        {
            std::lock_guard lock(mutex);
            stopEntered = true;
            changed.notify_all();
        }
        worker.stop();
    }

    void finishMeasurement()
    {
        stopper = std::thread([this] {
            try {
                benchmark::stopAndMeasure(*this, [this] {
                    std::lock_guard lock(mutex);
                    endpointBeforePublication = publications == 0;
                    endpoint = Clock::now();
                });
            } catch (...) {
                stopError = std::current_exception();
            }
        });
        {
            std::unique_lock lock(mutex);
            REQUIRE(changed.wait_for(lock, 5s, [this] { return stopEntered; }));
            CHECK(publications == 0);
        }
        release();
        stopper.join();
        if (stopError) {
            std::rethrow_exception(stopError);
        }
    }
};

}  // namespace

TEST_CASE("Worker measurement includes an in-flight publication before its endpoint")
{
    HeldFrame fixture;
    fixture.start();
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE_FALSE(fixture.worker.fetchOutput(seen, output));
    REQUIRE(fixture.worker.consumedFrameSequence() == 0);
    const auto feedEnded = Clock::now();

    fixture.finishMeasurement();

    REQUIRE_FALSE(fixture.timedOut);
    INFO("endpoint preceded the in-flight publication");
    CHECK_FALSE(fixture.endpointBeforePublication);
    REQUIRE(fixture.publications == 1);
    CHECK(fixture.endpoint >= fixture.publishedAt);
    CHECK(fixture.publishedAt >= feedEnded);
    REQUIRE(fixture.worker.fetchOutput(seen, output));
    CHECK(seen == 1);
    CHECK(output.framesProcessed == 1);
    CHECK(output.frameSequence == 41);
    REQUIRE(output.images.count(ScopeId) == 1);
    CHECK_FALSE(output.images.at(ScopeId).rgba.empty());
    CHECK(fixture.worker.consumedFrameSequence() == 41);
    CHECK_FALSE(fixture.worker.fetchOutput(seen, output));
    CHECK(output.framesProcessed == 1);
}

}  // namespace sidescopes
