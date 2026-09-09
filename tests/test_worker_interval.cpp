#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

#include "../bench/worker_interval.h"
#include "core/analysis_worker.h"
#include "modules/module_registry.h"
#include "test_frame.h"

namespace sidescopes {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr char ScopeId[] = "com.example.worker.interval";
constexpr SsScopeDescriptor Descriptor{ScopeId, "Interval", 'X', 0, 0, 0, nullptr, 0, 1.0f};

struct BlockingScope
{
    SsScopeInstance api{};
    std::array<uint8_t, 4> pixel{100, 50, 20, 255};
    std::function<void()> onAccumulate;
};

BlockingScope* g_scope = nullptr;

SsScopeInstance* createScope(const char*, const SsHost*)
{
    auto& scope = *g_scope;
    scope.api.instance_data = &scope;
    scope.api.configure = [](SsScopeInstance*, const SsParamValue*, uint32_t) { return true; };
    scope.api.accumulate = [](SsScopeInstance* instance, const SsFrameView*, SsRect) {
        static_cast<BlockingScope*>(instance->instance_data)->onAccumulate();
        return true;
    };
    scope.api.image = [](const SsScopeInstance* instance) {
        return SsImageView{static_cast<const BlockingScope*>(instance->instance_data)->pixel.data(), 1, 1, 1};
    };
    scope.api.graticule = [](const SsScopeInstance*, SsGraticulePrimitive*, uint32_t) { return 0u; };
    scope.api.markers = [](const SsScopeInstance*, SsColor, SsMarker*, uint32_t) { return 0u; };
    scope.api.get_extension = [](const SsScopeInstance*, const char*) -> const void* { return nullptr; };
    scope.api.destroy = [](SsScopeInstance*) {};
    return &scope.api;
}

const SsModuleEntry Entry{SS_ABI_MAJOR, SS_ABI_MINOR,      [] { return true; },
                          [] {},        [] { return 1u; }, [](uint32_t) { return &Descriptor; },
                          createScope};

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
    BlockingScope scope;
    ModuleRegistry registry;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox, registry};
    std::thread stopper;

    HeldFrame()
    {
        g_scope = &scope;
        REQUIRE(registry.registerModule(Entry));
    }

    ~HeldFrame()
    {
        release();
        if (stopper.joinable()) {
            stopper.join();
        }
        worker.stop();
        g_scope = nullptr;
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
        // Block one module's accumulation so the real worker publication is
        // still in flight when shutdown begins.
        scope.onAccumulate = [this] {
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            timedOut = !changed.wait_for(lock, 5s, [this] { return released; });
        };
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
