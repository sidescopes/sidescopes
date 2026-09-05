#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <thread>

#include "allocation_failure.h"
#include "app/frame_pacing.h"
#include "core/analysis_worker.h"
#include "core/diagnostics.h"
#include "modules/module_registry.h"
#include "temp_file.h"
#include "test_frame.h"

namespace sidescopes {
namespace {
using test::AllocationFailure;
constexpr std::array ScopeIds{"com.example.allocation.first", "com.example.allocation.second"};
constexpr char Parameter[] = "parameter_long_enough_to_require_storage";
constexpr SsParamInfo Param{Parameter, "Value", SS_PARAM_FLOAT, 1.0, 100.0, 1.0, 0.0, nullptr, nullptr};
constexpr std::array<SsScopeDescriptor, 2> Descriptors{{
    {ScopeIds[0], "First", 'X', 0, 0, 0, &Param, 1, 1.0f},
    {ScopeIds[1], "Second", 'Y', 0, 0, 0, &Param, 1, 1.0f},
}};

// Module storage is fixed and never allocates. Every injected failure therefore
// comes from the host's records, settings, parameter assembly, or output copy.
struct ProbeScope
{
    SsScopeInstance api{};
    std::array<uint8_t, 64> pixels{};
    int value = 1;
    uint64_t sequence = 0;
    int created = 0;
    int destroyed = 0;
};

std::array<ProbeScope, 2>* g_scopes = nullptr;

ProbeScope& state(const SsScopeInstance* instance)
{
    return *static_cast<ProbeScope*>(instance->instance_data);
}

const SsOutlineExtension Outline{[](const SsScopeInstance* instance, float* out, uint32_t capacity) {
    const auto& scope = state(instance);
    const uint32_t count = scope.value >= 42 ? 128u : 3u;
    if (out) {
        std::fill_n(out, std::min(count, capacity), static_cast<float>(scope.value));
    }
    return count;
}};

SsScopeInstance* create(const char* id, const SsHost*)
{
    auto& scope = (*g_scopes)[std::strcmp(id, ScopeIds[0]) == 0 ? 0 : 1];
    ++scope.created;
    scope.api.instance_data = &scope;
    scope.api.configure = [](SsScopeInstance* instance, const SsParamValue* values, uint32_t count) {
        state(instance).value = count == 1 ? static_cast<int>(values[0].value) : 1;
        return true;
    };
    scope.api.accumulate = [](SsScopeInstance* instance, const SsFrameView*, SsRect) {
        auto& current = state(instance);
        current.pixels.fill(static_cast<uint8_t>(current.value));
        ++current.sequence;
        return true;
    };
    scope.api.image = [](const SsScopeInstance* instance) {
        const auto& current = state(instance);
        const int edge = current.value >= 42 ? 4 : 1;
        return SsImageView{current.pixels.data(), edge, edge, current.sequence};
    };
    scope.api.graticule = [](const SsScopeInstance*, SsGraticulePrimitive*, uint32_t) { return 0u; };
    scope.api.markers = [](const SsScopeInstance*, SsColor, SsMarker*, uint32_t) { return 0u; };
    scope.api.get_extension = [](const SsScopeInstance*, const char* extension) -> const void* {
        return std::strcmp(extension, OutlineExtension) == 0 ? &Outline : nullptr;
    };
    scope.api.destroy = [](SsScopeInstance* instance) { ++state(instance).destroyed; };
    return &scope.api;
}

const SsModuleEntry Entry{SS_ABI_MAJOR, SS_ABI_MINOR,      [] { return true; },
                          [] {},        [] { return 2u; }, [](uint32_t index) { return &Descriptors[index]; },
                          create};

struct Observation
{
    std::size_t attempts = 0;
    std::size_t failures = 0;
    bool threw = false;
};

template <typename Operation>
Observation observe(std::size_t failAt, Operation operation)
{
    AllocationFailure failure(failAt);
    bool threw = false;
    try {
        operation();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    failure.disarm();
    return {failure.attempts(), failure.failures(), threw};
}

template <typename Predicate>
bool await(Predicate ready)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
}

struct Fixture
{
    std::array<ProbeScope, 2> scopes;
    ModuleRegistry registry;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox, registry};
    AnalysisSettings settings;
    AnalysisWorker::Output output;
    uint64_t seen = 0;
    uint64_t frame = 0;
    int notifications = 0;
    bool allocateInNotification = false;
    bool failNotification = false;
    std::vector<uint8_t> notificationStorage;

    Fixture()
    {
        g_scopes = &scopes;
        REQUIRE(registry.registerModule(Entry));
        settings.region = RegionOfInterest{};
        settings.enabledScopes.assign(ScopeIds.begin(), ScopeIds.end());
        setValue(7);
        worker.updateSettings(settings);
        worker.setOutputCallback([this] {
            ++notifications;
            if (failNotification) {
                const AllocationFailure failure(0);
                notificationStorage.resize(128);
            } else if (allocateInNotification) {
                notificationStorage.resize(128);
            }
        });
    }

    ~Fixture()
    {
        // Stop before callback state declared after the worker is destroyed,
        // including when an assertion exits a threaded case early.
        worker.stop();
    }

    void setValue(int value)
    {
        for (const char* id : ScopeIds) {
            settings.scopeParams[id][Parameter] = value;
        }
    }

    void publishFrame()
    {
        mailbox.publish(test::makeSolidFrameBuffer(4, 4, Color{100, 50, 20}, ++frame));
    }

    void warm()
    {
        worker.startInline();
        publishFrame();
        worker.pump();
        REQUIRE(worker.fetchOutput(seen, output));
        checkValue(7);
    }

    void changeSettings()
    {
        setValue(42);
        settings.scopeParams[ScopeIds[0]]["another_parameter_requiring_storage"] = 13.0;
        settings.imageSizes[ScopeIds[0]] = {4, 4};
    }

    void checkValue(int value) const
    {
        REQUIRE(output.images.size() == 2);
        REQUIRE(output.outlines.size() == 2);
        for (const char* id : ScopeIds) {
            const auto& image = output.images.at(id);
            REQUIRE(image.width == (value >= 42 ? 4 : 1));
            REQUIRE(image.rgba.size() == (value >= 42 ? 64 : 4));
            CHECK(std::all_of(image.rgba.begin(), image.rgba.end(), [=](uint8_t byte) { return byte == value; }));
            const auto& outline = output.outlines.at(id);
            REQUIRE(outline.size() == (value >= 42 ? 128 : 3));
            CHECK(outline.front() == static_cast<float>(value));
        }
    }

    void checkStopped()
    {
        worker.stop();
        for (const auto& scope : scopes) {
            CHECK(scope.created == scope.destroyed);
        }
        const uint64_t consumed = worker.consumedFrameSequence();
        worker.pump();
        CHECK(worker.consumedFrameSequence() == consumed);
    }
};

struct Recording
{
    test::TempFile log{"analysis-allocations.log"};

    Recording()
    {
        diagConfigure({"perf", log.path().string(), DiagFlush::EveryLine});
    }

    ~Recording()
    {
        diagConfigure({});
    }

    bool containsFailure() const
    {
        std::ifstream input(log.path());
        const std::string content{std::istreambuf_iterator<char>{input}, {}};
        return content.find("allocation failed") != std::string::npos;
    }
};

// Count the successful path first, then fail every allocation it made. The
// fixed cap makes an accidental unbounded probe fail before starting its sweep.
void checkCount(const Observation& observed)
{
    REQUIRE_FALSE(observed.threw);
    REQUIRE(observed.attempts > 0);
    REQUIRE(observed.attempts < 128);
}

std::size_t firstPassAllocations()
{
    // Process-wide library/static initialization must not be counted only in
    // the measuring pass; each injected pass needs the same starting state.
    Fixture warmup;
    warmup.warm();
    warmup.checkStopped();
    Fixture fixture;
    fixture.worker.startInline();
    fixture.publishFrame();
    const auto measured = observe(AllocationFailure::CountOnly, [&] { fixture.worker.pump(); });
    checkCount(measured);
    return measured.attempts;
}

void checkFetchRetryScheduled(bool changed)
{
    FrameClocks clocks;
    clocks.noteFrameBegun(100.0);
    CHECK_FALSE(frameWorthDrawing(clocks.redrawInputs({}, 100.0)));
    // The same branch as App::drawFrame: even the withdrawal marks activity.
    if (changed) {
        clocks.noteActivity(100.0);
    }
    CHECK(frameWorthDrawing(clocks.redrawInputs({}, 100.05)));
    CHECK(frameWaitFor(clocks.pacingInputs(100.05, false, false, false)).kind == FrameWait::None);
}
}  // namespace

TEST_CASE("Worker thread setup allocation failure publishes empty output and retries")
{
    Recording recording;
    Fixture fixture;
    // Keep a later idle pass from publishing a recovered reading before the
    // test has inspected the failure. Setup still runs while analysis is held.
    fixture.worker.hold(true);
    fixture.publishFrame();
    AllocationFailure failure(0, AllocationFailure::Thread::Other);
    fixture.worker.start();
    const bool completed = await([&] { return fixture.worker.consumedFrameSequence() == 1; });
    failure.disarm();
    CHECK(completed);
    CHECK(failure.failures() == 1);
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
    CHECK(fixture.output.images.empty());
    CHECK(fixture.output.outlines.empty());
    fixture.worker.hold(false);
    fixture.publishFrame();
    REQUIRE(await([&] { return fixture.worker.consumedFrameSequence() == 2; }));
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
    fixture.checkValue(7);
    fixture.checkStopped();
    CHECK(recording.containsFailure());
}

TEST_CASE("Worker shutdown completes while its setup allocations continue failing")
{
    Recording recording;
    Fixture fixture;
    AllocationFailure failure(0, AllocationFailure::Thread::Other, true);
    fixture.worker.start();
    const bool failed = await([&] { return failure.failures() > 0; });
    fixture.worker.stop();
    failure.disarm();
    CHECK(failed);
    CHECK(recording.containsFailure());
    fixture.worker.startInline();
    fixture.publishFrame();
    fixture.worker.pump();
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
    fixture.checkValue(7);
    fixture.checkStopped();
}

TEST_CASE("Caller-side worker start allocation failure leaves startup retryable")
{
    for (bool threaded : {false, true}) {
        Fixture fixture;
        const auto result = observe(0, [&] {
            if (threaded) {
                fixture.worker.start();
            } else {
                fixture.worker.startInline();
            }
        });
        CHECK(result.threw);
        CHECK(result.failures == 1);
        fixture.checkStopped();
        fixture.worker.startInline();
        fixture.publishFrame();
        fixture.worker.pump();
        REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
        fixture.checkValue(7);
        fixture.checkStopped();
    }
}

TEST_CASE("Every first-pass host allocation failure is contained and retryable")
{
    Recording recording;
    const std::size_t count = firstPassAllocations();
    for (std::size_t index = 0; index < count; ++index) {
        CAPTURE(index, count);
        Fixture fixture;
        fixture.worker.startInline();
        fixture.publishFrame();
        const auto result = observe(index, [&] { fixture.worker.pump(); });
        CHECK_FALSE(result.threw);
        REQUIRE(result.failures == 1);
        CHECK(fixture.worker.consumedFrameSequence() == 1);
        REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
        CHECK(fixture.output.images.empty());
        CHECK(fixture.output.outlines.empty());
        fixture.publishFrame();
        fixture.worker.pump();
        REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
        fixture.checkValue(7);
        fixture.checkStopped();
    }
    CHECK(recording.containsFailure());
}

TEST_CASE("Shutdown releases every scope after a partially initialized worker pass")
{
    const std::size_t count = firstPassAllocations();
    for (std::size_t index = 0; index < count; ++index) {
        CAPTURE(index, count);
        Fixture fixture;
        fixture.worker.startInline();
        fixture.publishFrame();
        const auto result = observe(index, [&] { fixture.worker.pump(); });
        CHECK_FALSE(result.threw);
        REQUIRE(result.failures == 1);
        // Stop directly from the failure, before a retry could finish setup
        // and hide lost ownership of an already-created module instance.
        fixture.checkStopped();
    }
}

TEST_CASE("Settings and output allocation failures withdraw an older successful pass")
{
    Recording recording;
    std::size_t count = 0;
    {
        Fixture fixture;
        fixture.warm();
        fixture.changeSettings();
        fixture.worker.updateSettings(fixture.settings);
        const auto measured = observe(AllocationFailure::CountOnly, [&] { fixture.worker.pump(); });
        checkCount(measured);
        count = measured.attempts;
    }
    for (std::size_t index = 0; index < count; ++index) {
        CAPTURE(index, count);
        Fixture fixture;
        fixture.warm();
        fixture.changeSettings();
        fixture.worker.updateSettings(fixture.settings);
        fixture.notifications = 0;
        const auto result = observe(index, [&] { fixture.worker.pump(); });
        CHECK_FALSE(result.threw);
        REQUIRE(result.failures == 1);
        CHECK(fixture.notifications == 1);
        REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
        CHECK(fixture.output.images.empty());
        CHECK(fixture.output.outlines.empty());
        CHECK(fixture.output.accumulateMilliseconds == 0.0);
        fixture.publishFrame();
        fixture.worker.pump();
        REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
        fixture.checkValue(42);
        fixture.checkStopped();
    }
    CHECK(recording.containsFailure());
}

TEST_CASE("A failed settings submission preserves the last complete settings")
{
    std::size_t count = 0;
    {
        Fixture fixture;
        fixture.warm();
        fixture.changeSettings();
        const auto measured =
            observe(AllocationFailure::CountOnly, [&] { fixture.worker.updateSettings(fixture.settings); });
        checkCount(measured);
        count = measured.attempts;
    }
    for (std::size_t index = 0; index < count; ++index) {
        CAPTURE(index, count);
        Fixture fixture;
        fixture.warm();
        fixture.changeSettings();
        const auto result = observe(index, [&] { fixture.worker.updateSettings(fixture.settings); });
        REQUIRE(result.failures == 1);
        CHECK(result.threw);
        fixture.worker.hold(true);
        fixture.worker.hold(false);
        fixture.worker.pump();
        REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
        fixture.checkValue(7);
        fixture.worker.updateSettings(fixture.settings);
        fixture.worker.pump();
        REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
        fixture.checkValue(42);
        fixture.checkStopped();
    }
}

TEST_CASE("A failed output fetch clears the caller image and retries the same version")
{
    std::size_t count = 0;
    {
        Fixture fixture;
        fixture.warm();
        fixture.changeSettings();
        fixture.worker.updateSettings(fixture.settings);
        fixture.worker.pump();
        const auto measured = observe(AllocationFailure::CountOnly,
                                      [&] { (void)fixture.worker.fetchOutput(fixture.seen, fixture.output); });
        checkCount(measured);
        count = measured.attempts;
    }
    for (std::size_t index = 0; index < count; ++index) {
        CAPTURE(index, count);
        Fixture fixture;
        fixture.warm();
        fixture.changeSettings();
        fixture.worker.updateSettings(fixture.settings);
        fixture.worker.pump();
        const uint64_t previous = fixture.seen;
        bool changed = false;
        const auto result = observe(index, [&] { changed = fixture.worker.fetchOutput(fixture.seen, fixture.output); });
        REQUIRE(result.failures == 1);
        CHECK_FALSE(result.threw);
        CHECK(changed);
        CHECK(fixture.seen == previous);
        CHECK(fixture.output.images.empty());
        CHECK(fixture.output.outlines.empty());
        CHECK(fixture.output.accumulateMilliseconds == 0.0);
        CHECK(fixture.output.framesProcessed == 0);
        CHECK(fixture.output.version == previous);
        checkFetchRetryScheduled(changed);
        // Neither another frame nor another worker pass is needed for retry.
        REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
        CHECK(fixture.seen > previous);
        fixture.checkValue(42);
        CHECK_FALSE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
        fixture.checkStopped();
    }
}

TEST_CASE("A notification allocation failure preserves a completed analysis")
{
    Recording recording;
    Fixture fixture;
    fixture.failNotification = true;
    fixture.warm();
    CHECK(fixture.notifications == 1);
    CHECK(fixture.notificationStorage.empty());
    fixture.publishFrame();
    fixture.worker.pump();
    CHECK_FALSE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
    CHECK(fixture.notifications == 1);
    fixture.checkValue(7);
    fixture.checkStopped();
    CHECK(recording.containsFailure());
}

TEST_CASE("Allocation failure in a recovery notification cannot escape the worker pass")
{
    Recording recording;
    Fixture fixture;
    fixture.warm();
    fixture.changeSettings();
    fixture.worker.updateSettings(fixture.settings);
    fixture.allocateInNotification = true;
    AllocationFailure failure(0, AllocationFailure::Thread::Current, true);
    bool threw = false;
    try {
        fixture.worker.pump();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    failure.disarm();
    CHECK_FALSE(threw);
    CHECK(failure.failures() >= 2);
    CHECK(fixture.notifications == 2);
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
    CHECK(fixture.output.images.empty());
    CHECK(fixture.output.outlines.empty());
    fixture.publishFrame();
    fixture.worker.pump();
    REQUIRE(fixture.worker.fetchOutput(fixture.seen, fixture.output));
    fixture.checkValue(42);
    CHECK(fixture.notificationStorage.size() == 128);
    fixture.checkStopped();
}

}  // namespace sidescopes
