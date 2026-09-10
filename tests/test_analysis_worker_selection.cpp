#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "core/analysis_worker.h"
#include "modules/module_registry.h"
#include "test_frame.h"

namespace sidescopes {
namespace {

constexpr char ScopeId[] = "com.example.region.selection";
constexpr RegionOfInterest Left{0.0, 0.0, 50.0, 100.0};
constexpr RegionOfInterest Right{50.0, 0.0, 100.0, 100.0};
constexpr SsScopeDescriptor Descriptor{ScopeId, "Region", 'X', 0, 0, 0, nullptr, 0, 1.0f};

struct Probe
{
    SsScopeInstance api{};
    std::array<uint8_t, 4> pixel{};
    uint64_t accumulations = 0;
    IntRect region;
    int configurations = 0;
    bool accumulateFails = false;
    bool invalidImage = false;
    std::function<void()> onAccumulate;
};

Probe* g_currentProbe = nullptr;

Probe& probe(const SsScopeInstance* instance)
{
    return *static_cast<Probe*>(instance->instance_data);
}

SsScopeInstance* createProbe(const char*, const SsHost*)
{
    auto& scope = *g_currentProbe;
    scope.api.instance_data = &scope;
    scope.api.configure = [](SsScopeInstance* instance, const SsParamValue*, uint32_t) {
        ++probe(instance).configurations;
        return true;
    };
    scope.api.accumulate = [](SsScopeInstance* instance, const SsFrameView* frame, SsRect region) {
        auto& state = probe(instance);
        const auto* pixel = frame->pixels + static_cast<std::size_t>(region.y) * frame->stride_bytes +
                            static_cast<std::size_t>(region.x) * 4;
        state.pixel = {pixel[2], pixel[1], pixel[0], uint8_t{255}};
        state.region = {region.x, region.y, region.width, region.height};
        ++state.accumulations;
        if (state.onAccumulate) {
            state.onAccumulate();
        }
        return !state.accumulateFails;
    };
    scope.api.image = [](const SsScopeInstance* instance) {
        const auto& state = probe(instance);
        return SsImageView{state.invalidImage ? nullptr : state.pixel.data(), 1, 1, state.accumulations};
    };
    scope.api.graticule = [](const SsScopeInstance*, SsGraticulePrimitive*, uint32_t) { return 0u; };
    scope.api.markers = [](const SsScopeInstance*, SsColor, SsMarker*, uint32_t) { return 0u; };
    scope.api.get_extension = [](const SsScopeInstance*, const char*) -> const void* { return nullptr; };
    scope.api.destroy = [](SsScopeInstance*) {};
    return &scope.api;
}

const SsModuleEntry Entry{SS_ABI_MAJOR, SS_ABI_MINOR,      [] { return true; },
                          [] {},        [] { return 1u; }, [](uint32_t) { return &Descriptor; },
                          createProbe};

FrameBuffer splitFrame(uint64_t sequence, Color left = {191, 0, 0}, Color right = {0, 0, 191})
{
    auto frame = test::makeSolidFrameBuffer(64, 64, left, sequence);
    frame.stamp = {3, 17, static_cast<double>(sequence) / 10.0};
    for (int row = 0; row < 64; ++row) {
        for (int column = 32; column < 64; ++column) {
            auto* pixel = frame.data.data() + (static_cast<std::size_t>(row) * 64 + column) * 4;
            pixel[0] = right.b;
            pixel[1] = right.g;
            pixel[2] = right.r;
        }
    }
    return frame;
}

struct Fixture
{
    Probe scope;
    ModuleRegistry registry;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox, registry};
    AnalysisSettings settings;
    AnalysisWorker::Output output;
    uint64_t seen = 0;

    Fixture()
    {
        g_currentProbe = &scope;
        REQUIRE(registry.registerModule(Entry));
        settings.region = Left;
        settings.selectionRevision = 1;
        settings.enabledScopes = {ScopeId};
        worker.updateSettings(settings);
        worker.startInline();
    }

    ~Fixture()
    {
        worker.stop();
        g_currentProbe = nullptr;
    }

    void publish(FrameBuffer frame)
    {
        mailbox.publish(std::move(frame));
        worker.pump();
    }

    bool fetch()
    {
        return worker.fetchOutput(seen, output);
    }
};

}  // namespace

TEST_CASE("Fetching a stale selection leaves the caller reading and version untouched")
{
    Fixture fixture;
    fixture.publish(splitFrame(1));
    fixture.output.version = 91;
    fixture.output.selectionRevision = 92;
    fixture.output.frameSequence = 93;
    fixture.output.region = Right;
    CHECK_FALSE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 2));
    CHECK(fixture.seen == 0u);
    CHECK(fixture.output.version == 91u);
    CHECK(fixture.output.selectionRevision == 92u);
    CHECK(fixture.output.frameSequence == 93u);
    CHECK(fixture.output.region == Right);
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 1));
    CHECK(fixture.output.selectionRevision == 1u);
    CHECK(fixture.output.frameSequence == 1u);
    CHECK(fixture.output.region == Left);
    CHECK(fixture.seen == fixture.output.version);
}

TEST_CASE("A selection changed during accumulation prevents the old output publication")
{
    Fixture fixture;
    fixture.scope.onAccumulate = [&] {
        if (fixture.settings.selectionRevision == 1u) {
            fixture.settings.selectionRevision = 2;
            fixture.worker.updateSettings(fixture.settings);
        }
    };
    fixture.publish(splitFrame(1));
    CHECK(fixture.scope.accumulations == 1u);
    CHECK_FALSE(fixture.fetch());
    fixture.worker.pump();
    REQUIRE(fixture.fetch());
    CHECK(fixture.output.selectionRevision == 2u);
    CHECK(fixture.scope.accumulations == 2u);
}

TEST_CASE("Unchanged scoped content keeps the completed reading provenance")
{
    Fixture fixture;
    fixture.publish(splitFrame(1));
    REQUIRE(fixture.fetch());
    const auto version = fixture.seen;
    fixture.publish(splitFrame(2));
    CHECK(fixture.worker.consumedFrameSequence() == 2u);
    CHECK_FALSE(fixture.fetch());
    CHECK(fixture.seen == version);
    CHECK(fixture.output.frameSequence == 1u);
    CHECK(fixture.output.frameStamp.receivedSeconds == 0.1);
}

TEST_CASE("A changed selection publishes its own region on identical pixels")
{
    Fixture fixture;
    fixture.publish(splitFrame(1, {191, 0, 0}, {191, 0, 0}));
    REQUIRE(fixture.fetch());
    fixture.settings.region = Right;
    fixture.settings.selectionRevision = 2;
    fixture.worker.updateSettings(fixture.settings);
    fixture.worker.pump();
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 2));
    CHECK(fixture.output.region == Right);
    CHECK(fixture.output.selectionRevision == 2u);
    CHECK(fixture.output.frameSequence == 1u);
    CHECK(fixture.scope.region == IntRect{32, 0, 32, 64});
}

TEST_CASE("A color picker only pass publishes completed region metadata without scopes")
{
    Fixture fixture;
    fixture.settings.enabledScopes.clear();
    fixture.worker.updateSettings(fixture.settings);
    fixture.publish(splitFrame(1));
    REQUIRE(fixture.fetch());
    CHECK(fixture.output.region == Left);
    CHECK(fixture.output.selectionRevision == 1u);
    CHECK(fixture.output.frameStamp.captureEpoch == 3u);
    CHECK(fixture.output.frameStamp.displayId == 17u);
    CHECK(fixture.output.frameSequence == 1u);
    CHECK(fixture.output.images.empty());
    CHECK(fixture.scope.accumulations == 0u);
    CHECK(fixture.scope.configurations == 0);
}

TEST_CASE("An unsuccessful scope withholds completed metadata and retries unchanged pixels")
{
    for (const bool invalidImage : {false, true}) {
        Fixture fixture;
        fixture.publish(splitFrame(1));
        REQUIRE(fixture.fetch());
        fixture.scope.invalidImage = invalidImage;
        fixture.scope.accumulateFails = !invalidImage;
        fixture.settings.region = Right;
        fixture.worker.updateSettings(fixture.settings);
        fixture.publish(splitFrame(2));
        REQUIRE(fixture.fetch());
        CHECK_FALSE(fixture.output.region);
        CHECK(fixture.output.frameSequence == 0u);
        CHECK(fixture.output.selectionRevision == 1u);
        CHECK(fixture.output.images.at(ScopeId).rgba.empty());
        fixture.scope.invalidImage = false;
        fixture.scope.accumulateFails = false;
        fixture.publish(splitFrame(3));
        REQUIRE(fixture.fetch());
        CHECK(fixture.output.region == Right);
        CHECK(fixture.output.frameSequence == 3u);
        CHECK_FALSE(fixture.output.images.at(ScopeId).rgba.empty());
    }
}

TEST_CASE("Changing capture source rejects stored pixels and already completed readings")
{
    Fixture fixture;
    fixture.settings.source = AnalysisSettings::Source{3, 17};
    fixture.worker.updateSettings(fixture.settings);
    fixture.publish(splitFrame(1));
    REQUIRE(fixture.scope.accumulations == 1u);
    fixture.settings.source = AnalysisSettings::Source{4, 18};
    fixture.worker.updateSettings(fixture.settings);
    CHECK_FALSE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 1, fixture.settings.source));
    CHECK(fixture.seen == 0u);
    fixture.worker.pump();
    CHECK(fixture.scope.accumulations == 1u);
    CHECK_FALSE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 1, fixture.settings.source));

    auto wrongEpoch = splitFrame(2);
    wrongEpoch.stamp.displayId = 18;
    fixture.publish(std::move(wrongEpoch));
    auto wrongDisplay = splitFrame(3);
    wrongDisplay.stamp.captureEpoch = 4;
    fixture.publish(std::move(wrongDisplay));
    CHECK(fixture.scope.accumulations == 1u);
    CHECK_FALSE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 1, fixture.settings.source));

    auto current = splitFrame(4);
    current.stamp = {4, 18, 0.4};
    fixture.publish(std::move(current));
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 1, fixture.settings.source));
    CHECK(fixture.scope.accumulations == 2u);
    CHECK(fixture.output.frameStamp.captureEpoch == 4u);
    CHECK(fixture.output.frameStamp.displayId == 18u);
    CHECK(fixture.output.frameSequence == 4u);
}

TEST_CASE("A capture source changed during accumulation prevents its old publication")
{
    Fixture fixture;
    fixture.settings.source = AnalysisSettings::Source{3, 17};
    fixture.worker.updateSettings(fixture.settings);
    fixture.scope.onAccumulate = [&] {
        fixture.settings.source = AnalysisSettings::Source{4, 17};
        fixture.worker.updateSettings(fixture.settings);
    };
    fixture.publish(splitFrame(1));
    CHECK(fixture.scope.accumulations == 1u);
    CHECK_FALSE(fixture.fetch());
    fixture.scope.onAccumulate = {};
    fixture.worker.pump();
    CHECK(fixture.scope.accumulations == 1u);
    auto current = splitFrame(2);
    current.stamp.captureEpoch = 4;
    fixture.publish(std::move(current));
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 1, fixture.settings.source));
    CHECK(fixture.scope.accumulations == 2u);
    CHECK(fixture.output.frameStamp.captureEpoch == 4u);
}

TEST_CASE("Frame sampling rejects pixels from an earlier stream or another display")
{
    Fixture fixture;
    const AnalysisSettings::Source expected{4, 18};
    fixture.settings.source = expected;
    fixture.worker.updateSettings(fixture.settings);
    int reads = 0;
    const auto reader = [&](const FrameView&) { ++reads; };
    CHECK_FALSE(fixture.worker.sampleDisplayColor(10, 10, 0, expected));
    CHECK_FALSE(fixture.worker.withLatestFrame(reader, expected));

    for (const FrameStamp stamp : std::array{FrameStamp{3, 17, 0.1}, FrameStamp{3, 18, 0.2}, FrameStamp{4, 17, 0.3}}) {
        auto frame = splitFrame(1);
        frame.stamp = stamp;
        fixture.publish(std::move(frame));
        CHECK_FALSE(fixture.worker.sampleDisplayColor(10, 10, 0, expected));
        CHECK_FALSE(fixture.worker.withLatestFrame(reader, expected));
        CHECK(reads == 0);
    }

    // Unstamped hosts retain access when they do not request a source check.
    CHECK(fixture.worker.sampleDisplayColor(10, 10, 0));
    CHECK(fixture.worker.withLatestFrame(reader));
    CHECK(reads == 1);

    auto current = splitFrame(2);
    current.stamp = {expected.captureEpoch, expected.displayId, 0.4};
    fixture.publish(std::move(current));
    CHECK(fixture.worker.sampleDisplayColor(10, 10, 0, expected));
    CHECK(fixture.worker.withLatestFrame(reader, expected));
    CHECK(reads == 2);
}

TEST_CASE("An incomplete scope reading cannot cross a capture source change")
{
    Fixture fixture;
    fixture.settings.source = AnalysisSettings::Source{3, 17};
    fixture.worker.updateSettings(fixture.settings);
    fixture.scope.accumulateFails = true;
    fixture.publish(splitFrame(1));
    fixture.settings.source = AnalysisSettings::Source{4, 17};
    fixture.worker.updateSettings(fixture.settings);
    CHECK_FALSE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 1, fixture.settings.source));
    CHECK(fixture.seen == 0u);
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 1, AnalysisSettings::Source{3, 17}));
    CHECK_FALSE(fixture.output.region);
    CHECK(fixture.output.frameSequence == 0u);
    CHECK(fixture.output.frameStamp.captureEpoch == 3u);
    CHECK(fixture.output.frameStamp.displayId == 17u);
    CHECK(fixture.output.images.at(ScopeId).rgba.empty());

    fixture.scope.accumulateFails = false;
    auto current = splitFrame(2);
    current.stamp.captureEpoch = 4;
    fixture.publish(std::move(current));
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output, 1, fixture.settings.source));
    CHECK(fixture.output.frameSequence == 2u);
}

}  // namespace sidescopes
