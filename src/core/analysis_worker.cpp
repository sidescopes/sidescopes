#include "core/analysis_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/marker_smoother.h"
#include "core/region_hash.h"

namespace sidescopes {

IntRect RegionOfInterest::ToPixels(int frame_width, int frame_height) const {
    // Edges round INWARD: truncation used to include the pixel row just
    // outside the region at some fractional positions, and the region
    // border's own bright ring lives exactly there - it flickered into
    // the waveform as a phantom line near the top. A boundary pixel
    // belongs to the sample only when it is entirely inside.
    const auto floor_edge = [](double percent, int extent) {
        return static_cast<int>(std::floor(percent * extent / 100.0));
    };
    const auto ceil_edge = [](double percent, int extent) {
        return static_cast<int>(std::ceil(percent * extent / 100.0));
    };
    const int left = ceil_edge(left_percent, frame_width);
    const int top = ceil_edge(top_percent, frame_height);
    const int right = std::max(left, floor_edge(right_percent, frame_width));
    const int bottom = std::max(top, floor_edge(bottom_percent, frame_height));
    return IntRect{left, top, right - left, bottom - top};
}

AnalysisWorker::AnalysisWorker(FrameMailbox& mailbox) : mailbox_(mailbox) {}

AnalysisWorker::~AnalysisWorker() {
    Stop();
}

void AnalysisWorker::Start() {
    if (thread_.joinable()) return;
    stop_requested_.store(false);
    thread_ = std::thread(&AnalysisWorker::Run, this);
}

void AnalysisWorker::Stop() {
    if (!thread_.joinable()) return;
    stop_requested_.store(true);
    thread_.join();
}

void AnalysisWorker::UpdateSettings(const AnalysisSettings& settings) {
    std::lock_guard lock(settings_mutex_);
    settings_ = settings;
    ++settings_version_;
}

bool AnalysisWorker::FetchOutput(uint64_t& last_seen_version, Output& output) const {
    std::lock_guard lock(output_mutex_);
    if (output_.version == last_seen_version) return false;
    output = output_;
    last_seen_version = output_.version;
    return true;
}

std::optional<FloatColor> AnalysisWorker::SampleFrameColor(int px, int py, int radius) const {
    std::lock_guard lock(frame_mutex_);
    if (!has_frame_) return std::nullopt;
    const FrameView view = latest_frame_.View();
    if (px < 0 || px >= view.width || py < 0 || py >= view.height) return std::nullopt;
    return AverageNeighborhood(view, px, py, radius);
}

bool AnalysisWorker::WithLatestFrame(const std::function<void(const FrameView&)>& reader) const {
    std::lock_guard lock(frame_mutex_);
    if (!has_frame_) return false;
    reader(latest_frame_.View());
    return true;
}

std::optional<AnalysisWorker::FrameSize> AnalysisWorker::LatestFrameSize() const {
    std::lock_guard lock(frame_mutex_);
    if (!has_frame_) return std::nullopt;
    return FrameSize{latest_frame_.width, latest_frame_.height};
}

void AnalysisWorker::Run() {
    Vectorscope vectorscope;
    Waveform waveform;
    Waveform waveform_parade;
    Histogram histogram;
    AnalysisSettings settings;
    uint64_t seen_settings_version = 0;
    uint64_t last_content_hash = 0;
    uint64_t frames_processed = 0;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        bool new_frame = false;
        if (auto frame = mailbox_.TakeLatest(std::chrono::milliseconds(100))) {
            std::lock_guard lock(frame_mutex_);
            if (has_frame_) mailbox_.ReturnStorage(std::move(latest_frame_));
            latest_frame_ = std::move(*frame);
            has_frame_ = true;
            new_frame = true;
        }

        bool settings_changed = false;
        {
            std::lock_guard lock(settings_mutex_);
            if (settings_version_ != seen_settings_version) {
                settings = settings_;
                seen_settings_version = settings_version_;
                settings_changed = true;
            }
        }

        {
            std::lock_guard lock(frame_mutex_);
            if (!has_frame_ || (!new_frame && !settings_changed)) continue;
        }

        // Reading the frame without the lock is safe: this thread is the
        // only writer, and readers on other threads take the mutex only for
        // the brief sampling reads that tolerate the previous frame.
        const FrameView view = latest_frame_.View();
        const IntRect region =
            settings.region.ToPixels(view.width, view.height).ClampedTo(view.width, view.height);

        // The hash is computed on every pass — including settings-only ones —
        // so it always corresponds to the current region and mask. Skipping
        // it on any path leaves a stale value that defeats the next
        // unchanged-content comparison.
        const uint64_t content_hash = HashRegion(view, region, settings.masked_window);
        if (!settings_changed && content_hash == last_content_hash) continue;
        last_content_hash = content_hash;

        if (settings_changed) {
            vectorscope.Configure(settings.vectorscope);
            waveform.Configure(settings.waveform);
            WaveformSettings parade = settings.waveform;
            parade.mode = WaveformMode::RgbParade;
            waveform_parade.Configure(parade);
            histogram.Configure(settings.histogram);
        }

        // Only the scopes on screen cost anything; a disabled scope's
        // image simply goes stale and the UI never draws it.
        const uint32_t enabled = settings.enabled_scopes;
        const auto started = std::chrono::steady_clock::now();

        // The average samples a bounded grid regardless of region size,
        // so it costs the same on a laptop corner and a 6K full screen.
        FloatColor region_average;
        bool region_average_valid = false;
        {
            const int stride = std::max(1, std::max(region.width, region.height) / 256);
            double sum_r = 0.0;
            double sum_g = 0.0;
            double sum_b = 0.0;
            uint64_t samples = 0;
            for (int py = region.y; py < region.y + region.height; py += stride) {
                for (int px = region.x; px < region.x + region.width; px += stride) {
                    const Color color = view.ColorAt(px, py);
                    sum_r += color.r;
                    sum_g += color.g;
                    sum_b += color.b;
                    ++samples;
                }
            }
            if (samples > 0) {
                region_average =
                    FloatColor{static_cast<float>(sum_r / static_cast<double>(samples)),
                               static_cast<float>(sum_g / static_cast<double>(samples)),
                               static_cast<float>(sum_b / static_cast<double>(samples))};
                region_average_valid = true;
            }
        }

        if (enabled & kScopeVectorscope) vectorscope.Accumulate(view, region);
        if (enabled & kScopeWaveform) waveform.Accumulate(view, region);
        if (enabled & kScopeWaveformParade) waveform_parade.Accumulate(view, region);
        if (enabled & kScopeHistogram) histogram.Accumulate(view, region);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        if (new_frame) ++frames_processed;

        std::lock_guard lock(output_mutex_);
        if (enabled & kScopeVectorscope) output_.vectorscope_image = vectorscope.Image();
        if (enabled & kScopeWaveform) output_.waveform_image = waveform.Image();
        if (enabled & kScopeWaveformParade) output_.waveform_parade_image = waveform_parade.Image();
        if (enabled & kScopeHistogram) output_.histogram_image = histogram.Image();
        output_.region_average = region_average;
        output_.region_average_valid = region_average_valid;
        output_.accumulate_milliseconds = elapsed_ms;
        output_.frames_processed = frames_processed;
        ++output_.version;
    }
}

}  // namespace sidescopes
