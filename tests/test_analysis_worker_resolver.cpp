#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "core/analysis_worker.h"
#include "modules/module_registry.h"
#include "test_frame.h"

namespace sidescopes {
namespace {

constexpr char ScopeId[] = "com.example.region.resolver";
constexpr RegionOfInterest Left{0.0, 0.0, 50.0, 100.0};
constexpr RegionOfInterest Right{50.0, 0.0, 100.0, 100.0};
constexpr SsScopeDescriptor Descriptor{ScopeId, "Region", 'X', 0, 0, 0, nullptr, 0, 1.0f};
using Mode = FrameRegionResolution::Mode;

struct Probe
{
    SsScopeInstance api{};
    std::array<uint8_t, 4> pixel{};
    uint64_t accumulations = 0;
    uint64_t sourceSequence = 0;
    IntRect region;
    int configurations = 0;
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
        state.sourceSequence = frame->sequence;
        state.region = {region.x, region.y, region.width, region.height};
        ++state.accumulations;
        if (state.onAccumulate) {
            state.onAccumulate();
        }
        return true;
    };
    scope.api.image = [](const SsScopeInstance* instance) {
        const auto& state = probe(instance);
        return SsImageView{state.pixel.data(), 1, 1, state.accumulations};
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
    }

    ~Fixture()
    {
        worker.stop();
        g_currentProbe = nullptr;
    }

    void install(FrameRegionResolver resolver)
    {
        worker.setFrameRegionResolverFactory([resolver = std::move(resolver)] { return resolver; });
        worker.startInline();
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

bool consumed(AnalysisWorker& worker, uint64_t sequence)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (worker.consumedFrameSequence() != sequence && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return worker.consumedFrameSequence() == sequence;
}

}  // namespace

TEST_CASE("Frame region resolution analyzes the same owned pixels without reconfiguring scopes")
{
    Fixture fixture;
    std::vector<uint64_t> resolvedSequences;
    fixture.install([&](const FrameRegionRequest& request) {
        resolvedSequences.push_back(request.frame.sequence);
        const auto region = request.frame.srgbAt(0, 0).r > 100.0f ? Right : Left;
        return FrameRegionResolution{Mode::Override, region, request.selectionRevision};
    });
    fixture.publish(splitFrame(1));
    REQUIRE(fixture.fetch());
    CHECK(fixture.scope.region == IntRect{32, 0, 32, 64});
    CHECK(fixture.output.images.at(ScopeId).rgba == std::vector<uint8_t>{0, 0, 191, 255});
    fixture.publish(splitFrame(2, {0, 191, 0}, {191, 0, 0}));
    REQUIRE(fixture.fetch());
    CHECK(resolvedSequences == std::vector<uint64_t>{1, 2});
    CHECK(fixture.scope.sourceSequence == 2u);
    CHECK(fixture.scope.region == IntRect{0, 0, 32, 64});
    CHECK(fixture.output.images.at(ScopeId).rgba == std::vector<uint8_t>{0, 191, 0, 255});
    CHECK(fixture.scope.configurations == 1);
    CHECK(fixture.output.region == Left);
    CHECK(fixture.output.selectionRevision == 1u);
    CHECK(fixture.output.frameSequence == 2u);
    CHECK(fixture.output.frameStamp.captureEpoch == 3u);
    CHECK(fixture.output.frameStamp.displayId == 17u);
    CHECK(fixture.output.frameStamp.receivedSeconds == 0.2);
}

TEST_CASE("Scope settings reuse a frame resolution while a crop revision sees old pixels explicitly")
{
    Fixture fixture;
    std::vector<bool> fresh;
    fixture.install([&](const FrameRegionRequest& request) {
        fresh.push_back(request.freshFrame);
        return FrameRegionResolution{Mode::Override, request.selectionRevision == 1u ? Right : Left,
                                     request.selectionRevision};
    });
    fixture.publish(splitFrame(1));
    REQUIRE(fixture.fetch());
    fixture.settings.sampleThinning = 2;
    fixture.worker.updateSettings(fixture.settings);
    fixture.worker.pump();
    REQUIRE(fixture.fetch());
    CHECK(fresh == std::vector<bool>{true});
    CHECK(fixture.scope.region == IntRect{32, 0, 32, 64});
    fixture.settings.selectionRevision = 2;
    fixture.worker.updateSettings(fixture.settings);
    fixture.worker.pump();
    REQUIRE(fixture.fetch());
    CHECK(fresh == std::vector<bool>{true, false});
    CHECK(fixture.output.region == Left);
    CHECK(fixture.output.selectionRevision == 2u);
    CHECK(fixture.output.frameSequence == 1u);
    CHECK(fixture.output.framesProcessed == 1u);
}

TEST_CASE("A moved resolved region republishes identical pixels without reconfiguring scopes")
{
    Fixture fixture;
    fixture.install([](const FrameRegionRequest& request) {
        return FrameRegionResolution{Mode::Override, request.frame.sequence == 1u ? Left : Right,
                                     request.selectionRevision};
    });
    fixture.publish(splitFrame(1, {191, 0, 0}, {191, 0, 0}));
    REQUIRE(fixture.fetch());
    fixture.publish(splitFrame(2, {191, 0, 0}, {191, 0, 0}));
    REQUIRE(fixture.fetch());
    CHECK(fixture.scope.accumulations == 2u);
    CHECK(fixture.scope.configurations == 1);
    CHECK(fixture.scope.region == IntRect{32, 0, 32, 64});
    CHECK(fixture.output.region == Right);
    CHECK(fixture.output.frameSequence == 2u);
    CHECK(fixture.output.selectionRevision == 1u);
}

TEST_CASE("Unchanged scoped content keeps output provenance while resolution advances")
{
    Fixture fixture;
    int calls = 0;
    fixture.install([&](const FrameRegionRequest& request) {
        ++calls;
        return FrameRegionResolution{Mode::Override, Right, request.selectionRevision};
    });
    fixture.publish(splitFrame(1));
    REQUIRE(fixture.fetch());
    const auto version = fixture.seen;
    fixture.publish(splitFrame(2));
    CHECK(calls == 2);
    CHECK(fixture.worker.consumedFrameSequence() == 2u);
    CHECK_FALSE(fixture.fetch());
    CHECK(fixture.seen == version);
    CHECK(fixture.output.frameSequence == 1u);
    CHECK(fixture.output.frameStamp.receivedSeconds == 0.1);
    fixture.worker.pump();
    CHECK(calls == 2);
}

TEST_CASE("An unchanged photo keeps its cached region across an arbitrarily long capture gap")
{
    Fixture fixture;
    std::vector<bool> fresh;
    fixture.install([&](const FrameRegionRequest& request) {
        fresh.push_back(request.freshFrame);
        return FrameRegionResolution{Mode::Override, Right, request.selectionRevision};
    });
    auto initial = splitFrame(1);
    initial.stamp.receivedSeconds = 10.0;
    fixture.publish(std::move(initial));
    REQUIRE(fixture.fetch());
    fixture.settings.sampleThinning = 2;
    fixture.worker.updateSettings(fixture.settings);
    fixture.worker.pump();
    REQUIRE(fixture.fetch());
    CHECK(fixture.output.region == Right);
    CHECK(fixture.output.frameStamp.receivedSeconds == 10.0);
    CHECK(fresh == std::vector<bool>{true});
    auto changed = splitFrame(2, {0, 191, 0}, {191, 0, 0});
    changed.stamp.receivedSeconds = 7210.0;
    fixture.publish(std::move(changed));
    REQUIRE(fixture.fetch());
    CHECK(fresh == std::vector<bool>{true, true});
    CHECK(fixture.output.frameStamp.receivedSeconds == 7210.0);
}

TEST_CASE("Resolver freshness includes capture identity and ignores duplicate source frames")
{
    Fixture fixture;
    std::vector<bool> fresh;
    fixture.install([&](const FrameRegionRequest& request) {
        fresh.push_back(request.freshFrame);
        return FrameRegionResolution{Mode::Configured, {}, request.selectionRevision};
    });
    fixture.publish(splitFrame(1));
    fixture.publish(splitFrame(1));
    auto restarted = splitFrame(1);
    restarted.stamp.captureEpoch = 4;
    fixture.publish(std::move(restarted));
    auto otherDisplay = splitFrame(1);
    otherDisplay.stamp.captureEpoch = 4;
    otherDisplay.stamp.displayId = 18;
    fixture.publish(std::move(otherDisplay));
    CHECK(fresh == std::vector<bool>{true, true, true});
}

TEST_CASE("Resolver configured override and skip outcomes are distinct")
{
    Fixture fixture;
    fixture.install([](const FrameRegionRequest& request) {
        if (request.frame.sequence == 1u) {
            return FrameRegionResolution{Mode::Configured, Right, request.selectionRevision};
        }
        return FrameRegionResolution{
            request.frame.sequence == 2u ? Mode::Skip : Mode::Override, {}, request.selectionRevision};
    });
    fixture.publish(splitFrame(1));
    REQUIRE(fixture.fetch());
    CHECK(fixture.output.region == Left);
    for (uint64_t sequence : {2u, 3u}) {
        fixture.publish(splitFrame(sequence, {0, 191, 0}));
        CHECK(fixture.worker.consumedFrameSequence() == sequence);
        CHECK_FALSE(fixture.fetch());
    }
    CHECK(fixture.scope.accumulations == 1u);
}

TEST_CASE("A stale resolver revision cannot answer for the configured selection")
{
    Fixture fixture;
    fixture.install([](const FrameRegionRequest& request) {
        return FrameRegionResolution{Mode::Override, Right, request.selectionRevision + 1u};
    });
    fixture.publish(splitFrame(1));
    CHECK_FALSE(fixture.fetch());
    CHECK(fixture.scope.accumulations == 0u);
    CHECK(fixture.worker.consumedFrameSequence() == 1u);
}

TEST_CASE("Fetching a stale selection leaves the caller reading and version untouched")
{
    Fixture fixture;
    fixture.install([](const FrameRegionRequest& request) {
        return FrameRegionResolution{Mode::Configured, {}, request.selectionRevision};
    });
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

TEST_CASE("A resolver may reenter worker accessors and invalidate its own selection")
{
    Fixture fixture;
    bool accessorsWorked = false;
    std::vector<bool> fresh;
    fixture.install([&](const FrameRegionRequest& request) {
        fresh.push_back(request.freshFrame);
        accessorsWorked =
            fixture.worker.latestFrameSize().has_value() && fixture.worker.sampleDisplayColor(0, 0).has_value();
        (void)fixture.worker.withLatestFrame([&](const FrameView& view) {
            accessorsWorked = accessorsWorked && view.sequence == request.frame.sequence;
        });
        AnalysisWorker::Output previous;
        uint64_t seen = 0;
        (void)fixture.worker.fetchOutput(seen, previous);
        if (request.selectionRevision == 1u) {
            fixture.settings.selectionRevision = 2;
            fixture.worker.updateSettings(fixture.settings);
        }
        return FrameRegionResolution{Mode::Override, Right, request.selectionRevision};
    });
    fixture.publish(splitFrame(1));
    CHECK(accessorsWorked);
    CHECK_FALSE(fixture.fetch());
    CHECK(fixture.scope.accumulations == 0u);
    fixture.worker.pump();
    REQUIRE(fixture.fetch());
    CHECK(fresh == std::vector<bool>{true, false});
    CHECK(fixture.output.selectionRevision == 2u);
}

TEST_CASE("A selection changed during accumulation prevents the old output publication")
{
    Fixture fixture;
    fixture.install([](const FrameRegionRequest& request) {
        return FrameRegionResolution{Mode::Override, Right, request.selectionRevision};
    });
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

TEST_CASE("Holding and releasing analysis preserves resolver freshness boundaries")
{
    Fixture fixture;
    std::vector<bool> fresh;
    fixture.install([&](const FrameRegionRequest& request) {
        fresh.push_back(request.freshFrame);
        return FrameRegionResolution{request.freshFrame ? Mode::Override : Mode::Skip, Right,
                                     request.selectionRevision};
    });
    fixture.publish(splitFrame(1));
    REQUIRE(fixture.fetch());
    fixture.worker.hold(true);
    fixture.publish(splitFrame(2, {0, 191, 0}));
    CHECK(fresh == std::vector<bool>{true});
    CHECK_FALSE(fixture.fetch());
    fixture.worker.hold(false);
    fixture.worker.pump();
    CHECK(fresh == std::vector<bool>{true, false});
    CHECK_FALSE(fixture.fetch());
    fixture.publish(splitFrame(3, {0, 191, 0}, {191, 0, 0}));
    REQUIRE(fixture.fetch());
    CHECK(fresh == std::vector<bool>{true, false, true});
    CHECK(fixture.output.frameSequence == 3u);
}

TEST_CASE("Releasing stored pixels invalidates the resolver frame cache")
{
    Fixture fixture;
    int calls = 0;
    fixture.install([&](const FrameRegionRequest& request) {
        ++calls;
        return FrameRegionResolution{Mode::Configured, {}, request.selectionRevision};
    });
    fixture.publish(splitFrame(1));
    fixture.worker.releaseFrame();
    fixture.worker.pump();
    CHECK_FALSE(fixture.worker.latestFrameSize());
    fixture.publish(splitFrame(1));
    CHECK(calls == 2);
}

TEST_CASE("Resolver construction and call failures are contained without repeating old pixels")
{
    for (const bool failFactory : {false, true}) {
        Fixture fixture;
        int attempts = 0, calls = 0;
        fixture.worker.setFrameRegionResolverFactory([&]() -> FrameRegionResolver {
            ++attempts;
            if (failFactory && attempts == 1) {
                throw std::runtime_error("construction failure");
            }
            return [&](const FrameRegionRequest& request) {
                ++calls;
                if (!failFactory && calls == 1) {
                    throw std::runtime_error("resolution failure");
                }
                return FrameRegionResolution{Mode::Configured, {}, request.selectionRevision};
            };
        });
        fixture.worker.startInline();
        REQUIRE_NOTHROW(fixture.publish(splitFrame(1)));
        CHECK_FALSE(fixture.fetch());
        fixture.worker.updateSettings(fixture.settings);
        REQUIRE_NOTHROW(fixture.worker.pump());
        CHECK(attempts == 1);
        CHECK_FALSE(fixture.fetch());
        fixture.publish(splitFrame(2));
        REQUIRE(fixture.fetch());
        CHECK(attempts == 2);
    }
}

TEST_CASE("Resolver factory callable and destruction belong to the pass thread")
{
    for (const bool threaded : {false, true}) {
        std::thread::id created, called, destroyed;

        struct Lifetime
        {
            explicit Lifetime(std::thread::id& result)
                : destroyed(result)
            {
            }

            std::thread::id& destroyed;

            ~Lifetime()
            {
                destroyed = std::this_thread::get_id();
            }
        };

        Fixture fixture;
        fixture.worker.setFrameRegionResolverFactory([&] {
            created = std::this_thread::get_id();
            auto lifetime = std::make_shared<Lifetime>(destroyed);
            return [&, lifetime](const FrameRegionRequest& request) {
                (void)lifetime;
                called = std::this_thread::get_id();
                return FrameRegionResolution{Mode::Configured, {}, request.selectionRevision};
            };
        });
        if (threaded) {
            fixture.worker.start();
        } else {
            fixture.worker.startInline();
        }
        CHECK_THROWS_AS(fixture.worker.setFrameRegionResolverFactory({}), std::logic_error);
        fixture.mailbox.publish(splitFrame(1));
        if (threaded) {
            REQUIRE(consumed(fixture.worker, 1));
        } else {
            fixture.worker.pump();
        }
        fixture.worker.stop();
        CHECK(created == called);
        CHECK(destroyed == created);
        CHECK((created == std::this_thread::get_id()) == !threaded);
        fixture.worker.pump();
        CHECK(fixture.worker.consumedFrameSequence() == 1u);
    }
}

}  // namespace sidescopes
