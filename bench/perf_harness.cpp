// The performance harness: a deterministic, capture-free sweep of the analysis
// pipeline that runs on any machine and prints its results as JSON.
//
// Three tiers, each answering a different question:
//   engine  - what one accumulate pass costs, per scope, per region size, per
//             display image size. The `fixed` variant runs a one-pixel region,
//             so it isolates the per-pass cost that no amount of sampling can
//             reduce; the difference against the full region is the scatter.
//   hash    - what change detection costs on its own, per region size.
//   worker  - what the whole analysis thread costs in CPU seconds while frames
//             arrive at the capture cadence, with static and with moving
//             content. This is the number the idle machine feels.
//
// Everything is synthetic and fixed-seed, so two runs on one machine compare
// directly and two machines compare through cost-per-megapixel.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// Without this windows.h defines min and max as macros, and every std::min
// below turns into a syntax error.
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/frame_mailbox.h"
#include "core/region_hash.h"
#include "core/scopes/histogram.h"
#include "core/scopes/neutral.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"

namespace sidescopes {
namespace {

// The capture cadence the app requests, mirrored here so the worker tier runs
// against the same arrival rate the shipping app produces.
constexpr int CaptureFramesPerSecond = 30;

// How long each worker-tier scenario feeds frames by default. Long enough to
// average out scheduling noise, short enough that the sweep stays interactive.
constexpr double DefaultWorkerSeconds = 4.0;

// Engine-tier sampling: every measurement takes at least this many samples,
// and keeps going until the budget is spent, so a cheap pass is measured many
// times and an expensive one does not stall the sweep.
constexpr int MinimumSamples = 7;
constexpr double SampleBudgetMs = 400.0;

// The synthetic display. A capture always delivers the whole screen and the
// region is a sub-rectangle of it, so the frame stays this size throughout and
// only the region changes - exactly the shape the worker sees.
constexpr int FrameWidth = 3456;
constexpr int FrameHeight = 2234;

struct RegionCase
{
    const char* name;
    int width;
    int height;
};

// Small enough to be a face or a swatch, a typical photo-editor canvas, and
// the whole display.
constexpr RegionCase RegionCases[] = {
    {"small", 200, 200},
    {"medium", 1000, 700},
    {"full", FrameWidth, FrameHeight},
};

// The display image sizes AdaptiveDetail actually asks for at a small and a
// large pane, so the sweep brackets what the app runs rather than inventing
// resolutions.
struct PaneCase
{
    const char* name;
    int vectorscopeSize;
    int waveformColumns;
    int waveformHeight;
    int histogramWidth;
    int histogramHeight;
    int neutralSize;
};

constexpr PaneCase PaneCases[] = {
    {"small", 256, 512, 256, 512, 384, 256},
    {"large", 512, 2048, 512, 2048, 768, 512},
};

/// One measured number, plus the dimensions it was measured at. Serialized as
/// a JSON object; `metric` is the key scripts/bench-compare.py diffs on, so it
/// must stay stable across runs.
struct MetricRow
{
    std::string metric;
    double value = 0.0;
    std::string unit;
    std::vector<std::pair<std::string, std::string>> tags;
};

// A gradient-plus-noise BGRA frame. The ramp spreads chroma and luma across
// every scope, and the fixed-seed jitter populates the fine bin structure the
// density-correction paths work on. No clock or environment enters the pixels.
std::vector<uint8_t> makeFramePixels(int width, int height)
{
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> jitter(-12, 12);
    const int spanBase = width + height - 2;
    const auto toByte = [](int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            pixel[0] = toByte((x + y) * 255 / spanBase + jitter(rng));
            pixel[1] = toByte(y * 255 / (height - 1) + jitter(rng));
            pixel[2] = toByte(x * 255 / (width - 1) + jitter(rng));
            pixel[3] = 255;
        }
    }

    return pixels;
}

// The region of the given size, centered in the frame.
IntRect centeredRegion(const RegionCase& region)
{
    const int width = std::min(region.width, FrameWidth);
    const int height = std::min(region.height, FrameHeight);

    return IntRect{(FrameWidth - width) / 2, (FrameHeight - height) / 2, width, height};
}

double medianOf(std::vector<double> samples)
{
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());

    return samples[samples.size() / 2];
}

/// Runs @p body until both the sample floor and the time budget are met,
/// returning each run's wall time in nanoseconds. One untimed warm-up runs
/// first so allocation and first-touch costs stay out of the samples.
template <typename Body>
std::vector<double> timeIterations(const Body& body)
{
    body();
    std::vector<double> samples;
    const auto sweepStarted = std::chrono::steady_clock::now();
    for (;;) {
        const auto started = std::chrono::steady_clock::now();
        body();
        const auto finished = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(finished - started).count());
        const double spentMs = std::chrono::duration<double, std::milli>(finished - sweepStarted).count();
        if (static_cast<int>(samples.size()) >= MinimumSamples && spentMs >= SampleBudgetMs) {
            break;
        }
    }

    return samples;
}

/// The process's user+system CPU time in seconds. The worker tier reports cost
/// in cores rather than wall time, because a background thread that never
/// blocks the interface is still heat.
double processCpuSeconds()
{
#if defined(_WIN32)
    FILETIME creation;
    FILETIME exit;
    FILETIME kernel;
    FILETIME user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0) {
        return 0.0;
    }
    const auto toSeconds = [](const FILETIME& time) {
        ULARGE_INTEGER value;
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;

        return static_cast<double>(value.QuadPart) * 1e-7;
    };

    return toSeconds(kernel) + toSeconds(user);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    const auto toSeconds = [](const timeval& time) {
        return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_usec) * 1e-6;
    };

    return toSeconds(usage.ru_utime) + toSeconds(usage.ru_stime);
#endif
}

}  // namespace

// The engine tier. Each scope is measured through its own class, which is the
// same code the module shim calls one function pointer deeper.
namespace {

struct EngineSweep
{
    std::vector<MetricRow>* rows;
    FrameView frame;
};

void record(std::vector<MetricRow>& rows, const std::string& metric, double value, const char* unit,
            std::vector<std::pair<std::string, std::string>> tags)
{
    rows.push_back(MetricRow{metric, value, unit, std::move(tags)});
}

// Times one scope's accumulate over one region at one image size, and records
// both the pass time and its cost per megapixel of region.
template <typename Accumulate>
void measureEngine(EngineSweep& sweep, const char* scope, const RegionCase& region, const PaneCase& pane,
                   const Accumulate& accumulate)
{
    const IntRect rect = centeredRegion(region);
    const double nanoseconds = medianOf(timeIterations([&]() { accumulate(sweep.frame, rect); }));
    const std::string suffix = std::string(scope) + "/" + region.name + "/" + pane.name;
    const std::vector<std::pair<std::string, std::string>> tags{
        {"scope", scope}, {"region", region.name}, {"pane", pane.name}};
    record(*sweep.rows, "engine " + suffix, nanoseconds, "ns", tags);

    const double megapixels = static_cast<double>(rect.width) * rect.height / 1e6;
    if (megapixels > 0.0) {
        record(*sweep.rows, "engine-per-mpx " + suffix, nanoseconds / megapixels, "ns", tags);
    }
}

// The floor: one pixel of region, so the scatter contributes nothing and what
// is left is the per-pass cost of turning bins into an image. Everything above
// this floor is what sampling can reduce; the floor itself is paid per frame
// whatever the region does.
template <typename Accumulate>
void measureEngineFloor(EngineSweep& sweep, const char* scope, const PaneCase& pane, const Accumulate& accumulate)
{
    const IntRect single{FrameWidth / 2, FrameHeight / 2, 1, 1};
    const double nanoseconds = medianOf(timeIterations([&]() { accumulate(sweep.frame, single); }));
    record(*sweep.rows, "engine-fixed " + std::string(scope) + "/" + pane.name, nanoseconds, "ns",
           {{"scope", scope}, {"region", "one-pixel"}, {"pane", pane.name}});
}

void sweepVectorscope(EngineSweep& sweep, const PaneCase& pane)
{
    Vectorscope vectorscope;
    VectorscopeSettings settings;
    settings.size = pane.vectorscopeSize;
    vectorscope.configure(settings);
    const auto accumulate = [&](const FrameView& frame, IntRect region) { vectorscope.accumulate(frame, region); };
    measureEngineFloor(sweep, "vectorscope", pane, accumulate);
    for (const RegionCase& region : RegionCases) {
        measureEngine(sweep, "vectorscope", region, pane, accumulate);
    }
}

void sweepWaveform(EngineSweep& sweep, const PaneCase& pane, WaveformMode mode, const char* scope)
{
    Waveform waveform;
    WaveformSettings settings;
    settings.mode = mode;
    settings.columns = pane.waveformColumns;
    settings.imageHeight = pane.waveformHeight;
    waveform.configure(settings);
    const auto accumulate = [&](const FrameView& frame, IntRect region) { waveform.accumulate(frame, region); };
    measureEngineFloor(sweep, scope, pane, accumulate);
    for (const RegionCase& region : RegionCases) {
        measureEngine(sweep, scope, region, pane, accumulate);
    }
}

void sweepHistogram(EngineSweep& sweep, const PaneCase& pane)
{
    Histogram histogram;
    HistogramSettings settings;
    settings.imageWidth = pane.histogramWidth;
    settings.imageHeight = pane.histogramHeight;
    histogram.configure(settings);
    const auto accumulate = [&](const FrameView& frame, IntRect region) { histogram.accumulate(frame, region); };
    measureEngineFloor(sweep, "histogram", pane, accumulate);
    for (const RegionCase& region : RegionCases) {
        measureEngine(sweep, "histogram", region, pane, accumulate);
    }
}

void sweepNeutral(EngineSweep& sweep, const PaneCase& pane)
{
    Neutral neutral;
    NeutralSettings settings;
    settings.size = pane.neutralSize;
    neutral.configure(settings);
    const auto accumulate = [&](const FrameView& frame, IntRect region) { neutral.accumulate(frame, region); };
    measureEngineFloor(sweep, "neutral", pane, accumulate);
    for (const RegionCase& region : RegionCases) {
        measureEngine(sweep, "neutral", region, pane, accumulate);
    }
}

void sweepEngines(std::vector<MetricRow>& rows, const FrameView& frame)
{
    EngineSweep sweep{&rows, frame};
    for (const PaneCase& pane : PaneCases) {
        sweepVectorscope(sweep, pane);
        // The three waveform styles the module actually offers. RgbAndLuma is
        // a struct default no configuration path selects, so measuring it
        // would measure code the app never runs.
        sweepWaveform(sweep, pane, WaveformMode::Rgb, "waveform");
        sweepWaveform(sweep, pane, WaveformMode::Luma, "waveform-luma");
        sweepWaveform(sweep, pane, WaveformMode::RgbParade, "parade");
        sweepHistogram(sweep, pane);
        sweepNeutral(sweep, pane);
    }
}

/// Consumes a value the caller does not otherwise use, so the work that
/// produced it cannot be folded away. Change detection returns nothing the
/// sweep needs; a link-time-optimizing build could otherwise see that and drop
/// the hash entirely. The store goes through a volatile pointer rather than to
/// a volatile variable, which compilers report as set-but-unused.
void keepAlive(uint64_t value)
{
    static uint64_t storage = 0;
    *static_cast<volatile uint64_t*>(&storage) = value;
}

void sweepHash(std::vector<MetricRow>& rows, const FrameView& frame)
{
    for (const RegionCase& region : RegionCases) {
        const IntRect rect = centeredRegion(region);
        uint64_t folded = 0;
        const double nanoseconds = medianOf(timeIterations([&]() { folded ^= hashRegion(frame, rect, IntRect{}); }));
        keepAlive(folded);
        const double megapixels = static_cast<double>(rect.width) * rect.height / 1e6;
        record(rows, std::string("hash ") + region.name, nanoseconds, "ns", {{"region", region.name}});
        if (megapixels > 0.0) {
            record(rows, std::string("hash-per-mpx ") + region.name, nanoseconds / megapixels, "ns",
                   {{"region", region.name}});
        }
    }
}

}  // namespace

// The worker tier: the real AnalysisWorker on its own thread, fed at the
// capture cadence, measured in CPU seconds.
namespace {

struct WorkerScenario
{
    const char* name;
    bool moving;
    std::vector<std::string> scopes;
};

AnalysisSettings settingsFor(const WorkerScenario& scenario, const RegionCase& region, const PaneCase& pane)
{
    AnalysisSettings settings;
    settings.enabledScopes = scenario.scopes;
    const IntRect rect = centeredRegion(region);
    settings.region.leftPercent = 100.0 * rect.x / FrameWidth;
    settings.region.topPercent = 100.0 * rect.y / FrameHeight;
    settings.region.rightPercent = 100.0 * (rect.x + rect.width) / FrameWidth;
    settings.region.bottomPercent = 100.0 * (rect.y + rect.height) / FrameHeight;
    settings.imageSizes["org.sidescopes.vectorscope"] = {pane.vectorscopeSize, pane.vectorscopeSize};
    settings.imageSizes["org.sidescopes.waveform"] = {pane.waveformColumns, pane.waveformHeight};
    settings.imageSizes["org.sidescopes.parade"] = {pane.waveformColumns, pane.waveformHeight};
    settings.imageSizes["org.sidescopes.histogram"] = {pane.histogramWidth, pane.histogramHeight};
    settings.imageSizes["org.sidescopes.neutral"] = {pane.neutralSize, pane.neutralSize};

    return settings;
}

// Rewrites a band of rows with fresh noise, which is what makes a scenario
// "moving": the scoped content genuinely changes, so change detection cannot
// skip the pass, and the harness's own cost stays a rounding error. The band
// walks inside the region, never outside it - a disturbance the region does
// not contain would be skipped, and would measure change detection instead of
// the scenario asked for.
void disturbFrame(std::vector<uint8_t>& pixels, IntRect region, uint64_t tick)
{
    const int bandRows = std::min(16, region.height);
    const int span = std::max(1, region.height - bandRows + 1);
    const int firstRow = region.y + static_cast<int>(tick % static_cast<uint64_t>(span));
    auto value = static_cast<uint32_t>(tick * 2654435761u + 1u);
    for (int y = firstRow; y < firstRow + bandRows; ++y) {
        uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * FrameWidth * 4;
        for (int x = region.x; x < region.x + region.width; ++x) {
            value = value * 1664525u + 1013904223u;
            row[static_cast<std::size_t>(x) * 4 + 1] = static_cast<uint8_t>(value >> 24);
        }
    }
}

struct WorkerResult
{
    double cpuSeconds = 0.0;
    double wallSeconds = 0.0;
    uint64_t delivered = 0;
    uint64_t processed = 0;
    double accumulateMilliseconds = 0.0;
};

// Feeds the mailbox at the capture cadence for WorkerSeconds and reports what
// the run cost. The producer only ever memcpys into recycled storage, so the
// CPU it adds is the same in every scenario and cancels out of comparisons.
WorkerResult runWorkerScenario(const WorkerScenario& scenario, const AnalysisSettings& settings,
                               const std::vector<uint8_t>& sourcePixels, IntRect region, double seconds)
{
    std::vector<uint8_t> pixels = sourcePixels;
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.updateSettings(settings);
    worker.start();

    FrameBuffer storage;
    WorkerResult result;
    const auto interval = std::chrono::nanoseconds(1'000'000'000 / CaptureFramesPerSecond);
    const auto started = std::chrono::steady_clock::now();
    const double cpuBefore = processCpuSeconds();
    for (uint64_t tick = 0;; ++tick) {
        std::this_thread::sleep_until(started + interval * static_cast<int64_t>(tick));
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= seconds) {
            break;
        }
        if (scenario.moving) {
            disturbFrame(pixels, region, tick);
        }
        storage.data = pixels;
        storage.strideBytes = FrameWidth * 4;
        storage.width = FrameWidth;
        storage.height = FrameHeight;
        storage.colorSpace = ColorSpaceHint::Srgb;
        storage.sequence = tick + 1;
        storage = mailbox.publish(std::move(storage));
        ++result.delivered;
    }
    result.cpuSeconds = processCpuSeconds() - cpuBefore;
    result.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    uint64_t seen = 0;
    AnalysisWorker::Output output;
    (void)worker.fetchOutput(seen, output);
    result.processed = output.framesProcessed;
    result.accumulateMilliseconds = output.accumulateMilliseconds;
    worker.stop();

    return result;
}

void recordWorker(std::vector<MetricRow>& rows, const WorkerScenario& scenario, const RegionCase& region,
                  const PaneCase& pane, const WorkerResult& result)
{
    const std::string suffix = std::string(scenario.name) + "/" + region.name + "/" + pane.name;
    const std::vector<std::pair<std::string, std::string>> tags{
        {"scenario", scenario.name}, {"region", region.name}, {"pane", pane.name}};
    const double cores = result.wallSeconds > 0.0 ? result.cpuSeconds / result.wallSeconds : 0.0;
    record(rows, "worker-cores " + suffix, cores, "cores", tags);
    record(rows, "worker-processed " + suffix, static_cast<double>(result.processed), "frames", tags);
    const double skipped = result.delivered > 0 ? 100.0 * static_cast<double>(result.delivered - result.processed) /
                                                      static_cast<double>(result.delivered)
                                                : 0.0;
    record(rows, "worker-skipped " + suffix, skipped, "percent", tags);
    record(rows, "worker-accumulate-ms " + suffix, result.accumulateMilliseconds, "ms", tags);
}

std::vector<WorkerScenario> workerScenarios()
{
    const std::vector<std::string> stack{"org.sidescopes.vectorscope", "org.sidescopes.waveform",
                                         "org.sidescopes.histogram"};

    return {
        // The producer's own cost, with the worker computing nothing: every
        // other row is only meaningful above this floor.
        {"idle-nothing", false, {}},
        {"idle-stack", false, stack},
        {"moving-stack", true, stack},
        {"moving-vectorscope", true, {"org.sidescopes.vectorscope"}},
        {"moving-waveform", true, {"org.sidescopes.waveform"}},
        {"moving-histogram", true, {"org.sidescopes.histogram"}},
    };
}

void sweepWorker(std::vector<MetricRow>& rows, const std::vector<uint8_t>& pixels, double seconds)
{
    // One pane size here: the worker tier is about the thread's steady cost,
    // and the engine tier already separates image size from region size.
    const PaneCase& pane = PaneCases[1];
    for (const WorkerScenario& scenario : workerScenarios()) {
        for (const RegionCase& region : RegionCases) {
            std::fprintf(stderr, "perf:   %s/%s\n", scenario.name, region.name);
            const AnalysisSettings settings = settingsFor(scenario, region, pane);
            const WorkerResult result = runWorkerScenario(scenario, settings, pixels, centeredRegion(region), seconds);
            recordWorker(rows, scenario, region, pane, result);
        }
    }
}

}  // namespace

namespace {

void writeJson(std::FILE* out, const std::vector<MetricRow>& rows, const std::string& machine,
               const std::string& osName, const std::string& commit)
{
    std::fprintf(out, "[\n");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const MetricRow& row = rows[i];
        std::fprintf(out, "  {\n");
        std::fprintf(out, "    \"machine\": \"%s\",\n", machine.c_str());
        std::fprintf(out, "    \"os\": \"%s\",\n", osName.c_str());
        std::fprintf(out, "    \"commit\": \"%s\",\n", commit.c_str());
        std::fprintf(out, "    \"metric\": \"%s\",\n", row.metric.c_str());
        std::fprintf(out, "    \"value\": %.6g,\n", row.value);
        std::fprintf(out, "    \"unit\": \"%s\"", row.unit.c_str());
        for (const auto& tag : row.tags) {
            std::fprintf(out, ",\n    \"%s\": \"%s\"", tag.first.c_str(), tag.second.c_str());
        }
        std::fprintf(out, "\n  }%s\n", i + 1 < rows.size() ? "," : "");
    }
    std::fprintf(out, "]\n");
}

struct Options
{
    std::string machine = "unknown";
    std::string osName = "unknown";
    std::string commit = "unknown";
    std::string outPath;
    std::string tiers = "engine,hash,worker";
    double workerSeconds = DefaultWorkerSeconds;
};

Options parseOptions(int argc, char** argv)
{
    Options options;
    const std::map<std::string, std::string*> targets{{"--machine", &options.machine},
                                                      {"--os", &options.osName},
                                                      {"--commit", &options.commit},
                                                      {"--out", &options.outPath},
                                                      {"--tiers", &options.tiers}};
    for (int i = 1; i + 1 < argc; i += 2) {
        const auto target = targets.find(argv[i]);
        if (target != targets.end()) {
            *target->second = argv[i + 1];
        }
        if (std::string(argv[i]) == "--worker-seconds") {
            options.workerSeconds = std::max(0.5, std::atof(argv[i + 1]));
        }
    }

    return options;
}

bool wants(const std::string& tiers, const char* tier)
{
    return tiers.find(tier) != std::string::npos;
}

}  // namespace
}  // namespace sidescopes

int main(int argc, char** argv)
{
    using namespace sidescopes;

    const Options options = parseOptions(argc, argv);
    const std::vector<uint8_t> pixels = makeFramePixels(FrameWidth, FrameHeight);
    const FrameView frame{pixels.data(), FrameWidth * 4, FrameWidth, FrameHeight, ColorSpaceHint::Srgb, 1};

    std::vector<MetricRow> rows;
    if (wants(options.tiers, "engine")) {
        std::fprintf(stderr, "perf: engine tier\n");
        sweepEngines(rows, frame);
    }
    if (wants(options.tiers, "hash")) {
        std::fprintf(stderr, "perf: hash tier\n");
        sweepHash(rows, frame);
    }
    if (wants(options.tiers, "worker")) {
        std::fprintf(stderr, "perf: worker tier\n");
        sweepWorker(rows, pixels, options.workerSeconds);
    }

    std::FILE* out = stdout;
    if (!options.outPath.empty()) {
#if defined(_MSC_VER)
        if (fopen_s(&out, options.outPath.c_str(), "w") != 0 || out == nullptr) {
#else
        out = std::fopen(options.outPath.c_str(), "w");
        if (out == nullptr) {
#endif
            std::fprintf(stderr, "perf: cannot write %s\n", options.outPath.c_str());

            return 1;
        }
    }
    writeJson(out, rows, options.machine, options.osName, options.commit);
    if (out != stdout) {
        std::fclose(out);
        std::fprintf(stderr, "perf: wrote %s\n", options.outPath.c_str());
    }

    return 0;
}
