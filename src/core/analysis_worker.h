#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/frame_mailbox.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

/// The scoped part of the screen, as percentages of the captured frame, so a
/// selection survives capture resolution changes.
struct RegionOfInterest
{
    double leftPercent = 0.0;
    double topPercent = 0.0;
    double rightPercent = 100.0;
    double bottomPercent = 100.0;

    [[nodiscard]] IntRect toPixels(int frameWidth, int frameHeight) const;
};

/// What the worker runs, keyed entirely by module scope id. Parameters and
/// display image sizes are the modules' own declarative vocabulary, so the
/// worker special-cases no scope; the host translates its typed preferences
/// into this map and back.
struct AnalysisSettings
{
    /// Per-scope parameter values: scope id -> parameter key -> value. Keys
    /// that a scope's descriptor does not declare are ignored.
    std::map<std::string, std::map<std::string, double>> scopeParams;
    /// Per-scope display image size: scope id -> {width, height}. A scope
    /// without an entry keeps its module's default resolution.
    std::map<std::string, std::pair<int, int>> imageSizes;
    /// The scope ids to compute this pass; an empty list computes nothing (the
    /// color-picker-only view asks nothing of the worker).
    std::vector<std::string> enabledScopes;
    RegionOfInterest region;
    /// The application's own window in frame pixels, masked out of change
    /// detection so its own redraws never re-trigger analysis.
    IntRect maskedWindow;
};

/// Runs the scope engines on a dedicated thread: takes the newest frame from
/// the mailbox, skips frames whose scoped content is unchanged, and publishes
/// double-buffered scope images. The UI thread pulls output when the version
/// advances and samples cursor colors from the most recent frame.
///
/// Threading: start, stop, updateSettings, fetchOutput, sampleFrameColor,
/// latestFrameSize, withLatestFrame, and consumedFrameSequence make up the
/// caller-thread surface and are safe to call while the worker runs. run()
/// exclusively owns the worker thread and is never called directly.
class AnalysisWorker
{
public:
    struct Output
    {
        /// Each computed scope's image, keyed by scope id. The map is kept
        /// stable across frames: a disabled scope's entry simply stops
        /// advancing rather than being cleared.
        std::map<std::string, ScopeImage> images;
        /// The outline-carrying scope's curve, stroked by the interface at
        /// display resolution (three channels of normalized heights). Only
        /// the histogram exports the outline extension today.
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

    /// Copies the latest output when it is newer than @p lastSeenVersion,
    /// advancing the version. Returns false when nothing new was produced.
    [[nodiscard]] bool fetchOutput(uint64_t& lastSeenVersion, Output& output) const;

    /// Averaged color around a pixel of the most recent frame, if any.
    [[nodiscard]] std::optional<FloatColor> sampleFrameColor(int px, int py, int radius = 1) const;

    struct FrameSize
    {
        int width = 0;
        int height = 0;
    };

    [[nodiscard]] std::optional<FrameSize> latestFrameSize() const;

    /// Runs @p reader on the most recent frame under the frame lock; returns
    /// false when no frame has arrived yet. Intended for occasional,
    /// interactive work (the picker's photo detection), not per-frame use.
    /// @p reader runs while the frame lock is held, so it must not call back
    /// into any AnalysisWorker frame accessor (sampleFrameColor,
    /// latestFrameSize, withLatestFrame) - that would self-deadlock on the
    /// non-recursive mutex - and it must return promptly, since it blocks the
    /// worker's next frame swap.
    [[nodiscard]] bool withLatestFrame(const std::function<void(const FrameView&)>& reader) const;

    /// The sequence number of the most recent frame the worker has taken from
    /// the mailbox and stored. Lets tests await the moment a published frame
    /// has been consumed, so a negative assertion need not sleep on a wall
    /// clock to be sure the worker has caught up.
    [[nodiscard]] uint64_t consumedFrameSequence() const;

private:
    void run();

    /// Takes the newest frame from the mailbox into m_latestFrame, returning
    /// whether a frame arrived this pass. Runs only on the worker thread.
    [[nodiscard]] bool takeLatestFrame();

    /// Copies pending settings into @p settings under the settings lock,
    /// advancing @p seenSettingsVersion and returning whether they changed.
    [[nodiscard]] bool syncSettings(AnalysisSettings& settings, uint64_t& seenSettingsVersion);

    /// Whether a frame is stored and either it or the settings just changed,
    /// so this pass has analysis to do.
    [[nodiscard]] bool hasWork(bool newFrame, bool settingsChanged) const;

    FrameMailbox& m_mailbox;
    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};

    mutable std::mutex m_settingsMutex;
    AnalysisSettings m_settings;
    uint64_t m_settingsVersion = 1;

    mutable std::mutex m_frameMutex;
    FrameBuffer m_latestFrame;
    bool m_hasFrame = false;
    std::atomic<uint64_t> m_consumedSequence{0};

    mutable std::mutex m_outputMutex;
    Output m_output;
};

}  // namespace sidescopes
