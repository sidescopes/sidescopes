#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "core/frame_mailbox.h"
#include "core/scopes/histogram.h"
#include "core/scopes/scope_types.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"

namespace sidescopes {

// The scoped part of the screen, as percentages of the captured frame, so a
// selection survives capture resolution changes.
struct RegionOfInterest
{
    double leftPercent = 0.0;
    double topPercent = 0.0;
    double rightPercent = 100.0;
    double bottomPercent = 100.0;

    [[nodiscard]] IntRect toPixels(int frameWidth, int frameHeight) const;
};

// Which scopes the worker computes. The waveform's RGB/Luma style is a
// setting on the one waveform scope; the parade is its own scope, since
// stacking a luma waveform over the parade is a working combination.
enum ScopeBit : uint32_t
{
    ScopeVectorscope = 1u << 0,
    ScopeWaveform = 1u << 1,
    ScopeWaveformParade = 1u << 2,
    ScopeHistogram = 1u << 3,
};

struct AnalysisSettings
{
    VectorscopeSettings vectorscope;
    // The waveform's mode selects its style (RGB or Luma); the parade
    // shares its gain and stride.
    WaveformSettings waveform;
    HistogramSettings histogram;
    uint32_t enabledScopes = ~0u;
    RegionOfInterest region;
    // The application's own window in frame pixels, masked out of change
    // detection so its own redraws never re-trigger analysis.
    IntRect maskedWindow;
};

// Runs the scope engines on a dedicated thread: takes the newest frame from
// the mailbox, skips frames whose scoped content is unchanged, and publishes
// double-buffered scope images. The UI thread pulls output when the version
// advances and samples cursor colors from the most recent frame.
class AnalysisWorker
{
public:
    struct Output
    {
        ScopeImage vectorscopeImage;
        ScopeImage waveformImage;
        ScopeImage waveformParadeImage;
        ScopeImage histogramImage;
        // The histogram's curve, stroked by the interface at display
        // resolution (three channels of normalized heights).
        std::vector<float> histogramOutline;
        double accumulateMilliseconds = 0.0;
        uint64_t framesProcessed = 0;
        uint64_t version = 0;
    };

    explicit AnalysisWorker(FrameMailbox& mailbox);
    ~AnalysisWorker();

    AnalysisWorker(const AnalysisWorker&) = delete;
    AnalysisWorker& operator=(const AnalysisWorker&) = delete;

    void start();
    void stop();

    void updateSettings(const AnalysisSettings& settings);

    // Copies the latest output when it is newer than `last_seen_version`,
    // advancing the version. Returns false when nothing new was produced.
    bool fetchOutput(uint64_t& lastSeenVersion, Output& output) const;

    // Averaged color around a pixel of the most recent frame, if any.
    [[nodiscard]] std::optional<FloatColor> sampleFrameColor(int px, int py, int radius = 1) const;

    struct FrameSize
    {
        int width = 0;
        int height = 0;
    };

    [[nodiscard]] std::optional<FrameSize> latestFrameSize() const;

    // Runs `reader` on the most recent frame under the frame lock; returns
    // false when no frame has arrived yet. Intended for occasional,
    // interactive work (the picker's photo detection), not per-frame use.
    bool withLatestFrame(const std::function<void(const FrameView&)>& reader) const;

private:
    void run();

    FrameMailbox& m_mailbox;
    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};

    mutable std::mutex m_settingsMutex;
    AnalysisSettings m_settings;
    uint64_t m_settingsVersion = 1;

    mutable std::mutex m_frameMutex;
    FrameBuffer m_latestFrame;
    bool m_hasFrame = false;

    mutable std::mutex m_outputMutex;
    Output m_output;
};

}  // namespace sidescopes
