#pragma once

#include <array>
#include <string_view>
#include <utility>

namespace sidescopes {

/// How much of the machine the analysis may spend on precision and on how often
/// it refreshes. One choice for the whole pipeline: the rate the screen is read
/// at, the resolution every scope image is computed at, and how densely the
/// region is sampled.
enum class QualityLevel
{
    /// As little as the scopes can be read at. Every image that is a display
    /// resolution over a fixed accumulation grid is taken down a step, the
    /// samples are thinned, and the screen is read ten times a second.
    Low,
    /// The shipped behaviour, and the default.
    Standard,
    /// Precision and refresh rate first, up to the point measurement stops
    /// showing a difference - and no further.
    High,
};

/// The levels in the order they are offered, ascending.
inline constexpr std::array<QualityLevel, 3> QualityLevels{QualityLevel::Low, QualityLevel::Standard,
                                                           QualityLevel::High};

/// The magnification Standard tolerates. The detail policy's pane thresholds
/// are stated at it, so a level that tolerates less reads them proportionally
/// lower and reaches the larger step on a smaller pane.
inline constexpr float StandardMagnificationTolerance = 1.4f;

/// What one level asks of the analysis. Every field is a number measurement
/// chose; see notes on each for what raising or lowering it costs.
struct QualityProfile
{
    /// How often the screen is read. Bounded above by how often the interface
    /// redraws: a pass computed between two drawn frames is never shown.
    int captureFramesPerSecond = 15;

    /// How much the display may magnify a scope image before the level counts
    /// it soft. Applies to the axes that are display resolution over a fixed
    /// accumulation grid - the vectorscope's image, the histogram's plot, the
    /// neutral plane - where a finer image costs compose time and no data.
    float magnificationTolerance = 1.4f;

    /// The same, for the waveform's columns. Held separately because a column
    /// is a place in the region rather than a resolution: halving them moves
    /// the trace by 5.5 to 10.6 of 255 where every other axis costs 0.03 to
    /// 2.8, so no level ever tolerates more magnification here than Standard
    /// does. Only High lowers it.
    float columnTolerance = 1.4f;

    /// The tallest waveform image. Height only draws a finer spline through
    /// levels that eight-bit input fixes at 256, and it is priced like real
    /// data - 14.5 ms a pass over a whole display for 2.5% more resolved
    /// detail - so no level raises this above the shipped step.
    int waveformHeightCeiling = 512;

    /// The largest histogram plot, bins by height. Both are plot resolution
    /// only: halving the width costs 0.013 of 255, the cheapest lever measured.
    std::pair<int, int> histogramCeiling{4096, 768};

    /// The largest vectorscope image. Never raised above the shipped step:
    /// 256, 512 and 1024 magnify to the same measured detail on a photograph
    /// and 1024 reads worse on saturated content, because the limit is the
    /// 256-code accumulation grid and not the image.
    int vectorscopeCeiling = 512;

    /// The largest neutral plane. Unlike the others this is real data - the
    /// cloud is accumulated at this resolution - which is why it is also the
    /// largest single memory item a level can give back.
    int neutralCeiling = 1024;

    /// The divisor on the samples per bin the scopes that offer the thinning
    /// extension take. Measured at 1024 columns, halving them is 21% of the
    /// waveform's pass for a mean of 1.43 of 255.
    int sampleThinning = 1;

    /// Whether the scopes drop to a coarser image while the user drags the
    /// region across the picture. A scan is read while it moves, so this is a
    /// judgement about what that reading is worth: High keeps full detail
    /// under the hand and pays for it.
    bool coarsenWhileDragged = true;

    [[nodiscard]] bool operator==(const QualityProfile&) const = default;
};

/// The numbers @p level asks for.
[[nodiscard]] const QualityProfile& profileFor(QualityLevel level);

/// The word @p level persists as, in the preferences file's own lower case.
[[nodiscard]] std::string_view qualityToken(QualityLevel level);

/// The level @p token names, Standard for anything else. The preferences file
/// is hand-editable, so an unknown word is a missing setting rather than an
/// error.
[[nodiscard]] QualityLevel qualityFromToken(std::string_view token);

/// The name @p level is offered under in the menu.
[[nodiscard]] const char* qualityLabel(QualityLevel level);

}  // namespace sidescopes
