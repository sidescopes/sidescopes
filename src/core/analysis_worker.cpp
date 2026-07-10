#include "core/analysis_worker.h"

#include <chrono>

#include "core/marker_smoother.h"
#include "core/region_hash.h"

namespace sidescopes {

IntRect RegionOfInterest::ToPixels(int frame_width, int frame_height) const {
    const auto scale_x = [&](double percent) {
        return static_cast<int>(percent * frame_width / 100.0);
    };
    const auto scale_y = [&](double percent) {
        return static_cast<int>(percent * frame_height / 100.0);
    };
    return IntRect{scale_x(left_percent), scale_y(top_percent),
                   scale_x(right_percent - left_percent), scale_y(bottom_percent - top_percent)};
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

std::optional<AnalysisWorker::FrameSize> AnalysisWorker::LatestFrameSize() const {
    std::lock_guard lock(frame_mutex_);
    if (!has_frame_) return std::nullopt;
    return FrameSize{latest_frame_.width, latest_frame_.height};
}

void AnalysisWorker::Run() {
    Vectorscope vectorscope;
    Waveform waveform;
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
        }

        const auto started = std::chrono::steady_clock::now();
        vectorscope.Accumulate(view, region);
        waveform.Accumulate(view, region);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        if (new_frame) ++frames_processed;

        std::lock_guard lock(output_mutex_);
        output_.vectorscope_image = vectorscope.Image();
        output_.waveform_image = waveform.Image();
        output_.accumulate_milliseconds = elapsed_ms;
        output_.frames_processed = frames_processed;
        ++output_.version;
    }
}

}  // namespace sidescopes
