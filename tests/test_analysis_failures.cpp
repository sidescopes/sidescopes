#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#include "core/analysis_worker.h"
#include "modules/module_registry.h"
#include "test_frame.h"

namespace sidescopes {
namespace {
constexpr char ScopeId[] = "com.example.fallible";
constexpr SsScopeDescriptor Descriptor{ScopeId, "Fallible", 'X', 0, 0, 0, nullptr, 0, 1.0f};

struct FallibleScope
{
    SsScopeInstance api{};
    bool createFails = false;
    bool configureFails = false;
    bool accumulateFails = false;
    bool invalidImage = false;
    int configureCalls = 0;
    int accumulateCalls = 0;
    std::array<uint8_t, 4> pixels{17, 31, 63, 255};
};

// Each test owns the module state until its worker has stopped. The module
// boundary has no user-data argument on create, so the fixture supplies it here.
FallibleScope* g_scope = nullptr;

FallibleScope& state(const SsScopeInstance* instance)
{
    return *static_cast<FallibleScope*>(instance->instance_data);
}

SsScopeInstance* create(const char*, const SsHost*)
{
    auto& scope = *g_scope;
    if (scope.createFails) {
        return nullptr;
    }
    scope.api.instance_data = &scope;
    scope.api.configure = [](SsScopeInstance* instance, const SsParamValue*, uint32_t) {
        auto& current = state(instance);
        ++current.configureCalls;
        return !current.configureFails;
    };
    scope.api.accumulate = [](SsScopeInstance* instance, const SsFrameView*, SsRect) {
        auto& current = state(instance);
        ++current.accumulateCalls;
        return !current.accumulateFails;
    };
    scope.api.image = [](const SsScopeInstance* instance) {
        const auto& current = state(instance);
        return SsImageView{current.invalidImage ? nullptr : current.pixels.data(), 1, 1,
                           static_cast<uint64_t>(current.accumulateCalls)};
    };
    scope.api.graticule = [](const SsScopeInstance*, SsGraticulePrimitive*, uint32_t) { return 0u; };
    scope.api.markers = [](const SsScopeInstance*, SsColor, SsMarker*, uint32_t) { return 0u; };
    scope.api.get_extension = [](const SsScopeInstance*, const char*) -> const void* { return nullptr; };
    scope.api.destroy = [](SsScopeInstance*) {};
    return &scope.api;
}

const SsModuleEntry Entry{SS_ABI_MAJOR, SS_ABI_MINOR,      [] { return true; },
                          [] {},        [] { return 1u; }, [](uint32_t) { return &Descriptor; },
                          create};

struct Fixture
{
    FallibleScope scope;
    ModuleRegistry registry;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox, registry};
    AnalysisWorker::Output output;
    AnalysisSettings settings;
    uint64_t seen = 0;
    uint64_t frame = 0;

    Fixture()
    {
        g_scope = &scope;
        REQUIRE(registry.registerModule(Entry));
        settings.enabledScopes = {ScopeId};
        settings.region = RegionOfInterest{};
        worker.updateSettings(settings);
        worker.startInline();
    }

    void pump()
    {
        mailbox.publish(test::makeSolidFrameBuffer(4, 4, Color{100, 50, 20}, ++frame));
        worker.pump();
        REQUIRE(worker.fetchOutput(seen, output));
    }
};
}  // namespace

TEST_CASE("An unsuccessful module configuration is retried before analysis")
{
    Fixture fixture;
    fixture.scope.configureFails = true;
    fixture.pump();
    CHECK(fixture.scope.accumulateCalls == 0);
    CHECK(fixture.output.images.at(ScopeId).rgba.empty());
    fixture.scope.configureFails = false;
    fixture.pump();
    CHECK(fixture.scope.configureCalls >= 2);
    CHECK(fixture.scope.accumulateCalls == 1);
    CHECK(fixture.output.images.at(ScopeId).rgba.size() == 4);
}

TEST_CASE("A failed module pass clears its old image and retries unchanged content")
{
    Fixture fixture;
    fixture.pump();
    REQUIRE(fixture.output.images.at(ScopeId).rgba.size() == 4);
    // A settings change asks for a new pass even though its pixels match.
    fixture.scope.accumulateFails = true;
    fixture.worker.hold(true);
    fixture.worker.hold(false);
    fixture.pump();
    CHECK(fixture.output.images.at(ScopeId).rgba.empty());
    fixture.scope.accumulateFails = false;
    fixture.pump();
    CHECK(fixture.output.images.at(ScopeId).rgba.size() == 4);
    CHECK(fixture.scope.accumulateCalls == 3);
}

TEST_CASE("A module image without pixel storage is declined and retried")
{
    Fixture fixture;
    fixture.scope.invalidImage = true;
    fixture.pump();
    CHECK(fixture.output.images.at(ScopeId).rgba.empty());
    fixture.scope.invalidImage = false;
    fixture.pump();
    CHECK(fixture.output.images.at(ScopeId).rgba.size() == 4);
}

TEST_CASE("A scope that cannot reopen withdraws its old image until creation succeeds")
{
    Fixture fixture;
    fixture.pump();
    REQUIRE(fixture.output.images.at(ScopeId).rgba.size() == 4);

    fixture.settings.enabledScopes.clear();
    fixture.worker.updateSettings(fixture.settings);
    fixture.worker.pump();
    fixture.scope.createFails = true;
    fixture.settings.enabledScopes = {ScopeId};
    fixture.worker.updateSettings(fixture.settings);
    fixture.pump();
    CHECK(fixture.output.images.at(ScopeId).rgba.empty());

    fixture.scope.createFails = false;
    fixture.pump();
    CHECK(fixture.output.images.at(ScopeId).rgba.size() == 4);
}

TEST_CASE("Consumed frame progress is published only after the pass completes")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = RegionOfInterest{};
    settings.enabledScopes = {"org.sidescopes.vectorscope"};
    worker.updateSettings(settings);
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    worker.setOutputCallback([&] {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    });
    worker.start();
    mailbox.publish(test::makeSolidFrameBuffer(4, 4, Color{100, 50, 20}, 1));
    {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
        CHECK(worker.consumedFrameSequence() == 0);
        released = true;
    }
    changed.notify_all();
    worker.stop();
    CHECK(worker.consumedFrameSequence() == 1);
}

}  // namespace sidescopes
