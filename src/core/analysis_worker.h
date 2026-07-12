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
struct RegionOfInterest {
    double left_percent = 0.0;
    double top_percent = 0.0;
    double right_percent = 100.0;
    double bottom_percent = 100.0;

    [[nodiscard]] IntRect ToPixels(int frame_width, int frame_height) const;
};

// Which scopes the worker computes. The waveform's RGB/Luma style is a
// setting on the one waveform scope; the parade is its own scope, since
// stacking a luma waveform over the parade is a working combination.
enum ScopeBit : uint32_t {
    kScopeVectorscope = 1u << 0,
    kScopeWaveform = 1u << 1,
    kScopeWaveformParade = 1u << 2,
    kScopeHistogram = 1u << 3,
};

struct AnalysisSettings {
    VectorscopeSettings vectorscope;
    // The waveform's mode selects its style (RGB or Luma); the parade
    // shares its gain and stride.
    WaveformSettings waveform;
    HistogramSettings histogram;
    uint32_t enabled_scopes = ~0u;
    RegionOfInterest region;
    // The application's own window in frame pixels, masked out of change
    // detection so its own redraws never re-trigger analysis.
    IntRect masked_window;
};

// Runs the scope engines on a dedicated thread: takes the newest frame from
// the mailbox, skips frames whose scoped content is unchanged, and publishes
// double-buffered scope images. The UI thread pulls output when the version
// advances and samples cursor colors from the most recent frame.
class AnalysisWorker {
public:
    struct Output {
        ScopeImage vectorscope_image;
        ScopeImage waveform_image;
        ScopeImage waveform_parade_image;
        ScopeImage histogram_image;
        // The histogram's curve, stroked by the interface at display
        // resolution (three channels of normalized heights).
        std::vector<float> histogram_outline;
        double accumulate_milliseconds = 0.0;
        uint64_t frames_processed = 0;
        uint64_t version = 0;
    };

    explicit AnalysisWorker(FrameMailbox& mailbox);
    ~AnalysisWorker();

    AnalysisWorker(const AnalysisWorker&) = delete;
    AnalysisWorker& operator=(const AnalysisWorker&) = delete;

    void Start();
    void Stop();

    void UpdateSettings(const AnalysisSettings& settings);

    // Copies the latest output when it is newer than `last_seen_version`,
    // advancing the version. Returns false when nothing new was produced.
    bool FetchOutput(uint64_t& last_seen_version, Output& output) const;

    // Averaged color around a pixel of the most recent frame, if any.
    [[nodiscard]] std::optional<FloatColor> SampleFrameColor(int px, int py, int radius = 1) const;

    struct FrameSize {
        int width = 0;
        int height = 0;
    };
    [[nodiscard]] std::optional<FrameSize> LatestFrameSize() const;

    // Runs `reader` on the most recent frame under the frame lock; returns
    // false when no frame has arrived yet. Intended for occasional,
    // interactive work (the picker's photo detection), not per-frame use.
    bool WithLatestFrame(const std::function<void(const FrameView&)>& reader) const;

private:
    void Run();

    FrameMailbox& mailbox_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex settings_mutex_;
    AnalysisSettings settings_;
    uint64_t settings_version_ = 1;

    mutable std::mutex frame_mutex_;
    FrameBuffer latest_frame_;
    bool has_frame_ = false;

    mutable std::mutex output_mutex_;
    Output output_;
};

}  // namespace sidescopes
