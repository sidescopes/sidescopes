#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/analysis_worker.h"

namespace sidescopes {

/// A scope the user can put on screen.
enum class ScopeGlyph
{
    Vectorscope,
    Waveform,
    WaveformParade,
    Histogram,
    ColorPicker
};

/// Everything stackable, in the fixed toolbar order.
constexpr ScopeGlyph AllScopes[] = {ScopeGlyph::Vectorscope, ScopeGlyph::Waveform, ScopeGlyph::WaveformParade,
                                    ScopeGlyph::Histogram, ScopeGlyph::ColorPicker};

/// Letter chips and preference letters share one alphabet.
constexpr char scopeLetter(ScopeGlyph kind)
{
    switch (kind) {
    case ScopeGlyph::Vectorscope:
        return 'V';
    case ScopeGlyph::Waveform:
        return 'W';
    case ScopeGlyph::WaveformParade:
        return 'R';
    case ScopeGlyph::Histogram:
        return 'H';
    case ScopeGlyph::ColorPicker:
        return 'C';
    }
    return 'V';
}

/// @return The worker's enable bit for @p kind, or zero when the scope asks
///         nothing of the worker.
constexpr uint32_t scopeEnableBit(ScopeGlyph kind)
{
    switch (kind) {
    case ScopeGlyph::Vectorscope:
        return ScopeVectorscope;
    case ScopeGlyph::Waveform:
        return ScopeWaveform;
    case ScopeGlyph::WaveformParade:
        return ScopeWaveformParade;
    case ScopeGlyph::Histogram:
        return ScopeHistogram;
    case ScopeGlyph::ColorPicker:
        // Reads the sampled cursor color; asks nothing of the worker.
        return 0;
    }
    return 0;
}

/// Which trace intensity a gesture adjusts. The waveform and its parade share
/// one intensity, so they share one control.
enum class TraceControl
{
    Vectorscope,
    Waveform
};

/// Which trace's intensity readout is flashing, and until when. Replaces the
/// address comparison the flash used to identify its scope with.
class TraceFlash
{
public:
    /// Shows @p control's readout until @p until, a glfwGetTime stamp.
    void show(TraceControl control, double until);

    /// @return Whether @p control's readout is still on screen at @p now.
    [[nodiscard]] bool showing(TraceControl control, double now) const;

private:
    std::optional<TraceControl> m_control;
    double m_until = 0.0;
};

/// What the user has on screen: the activation-ordered stack of scopes, the
/// view toggles, and each trace's intensity and marker smoothing.
class ScopeView
{
public:
    /// @return Whether @p kind is on screen.
    [[nodiscard]] bool shows(ScopeGlyph kind) const;

    /// @return The scopes on screen, in activation order.
    [[nodiscard]] const std::vector<ScopeGlyph>& stack() const;

    /// Adds @p kind, or removes it when already shown. The last scope stays,
    /// so the window is never empty.
    /// @return Whether @p kind became newly visible, so the caller can refresh
    ///         its image.
    bool toggle(ScopeGlyph kind);

    /// Stacks @p kind onto the current scopes when @p stack, otherwise solos it.
    /// @return Whether @p kind became newly visible.
    bool choose(ScopeGlyph kind, bool stack);

    /// @return The worker's enabled-scopes mask for what is on screen.
    [[nodiscard]] uint32_t enabledMask() const;

    /// Restores the stack from a preference letter string, falling back to the
    /// vectorscope when it names nothing valid.
    void restoreStack(const std::string& letters);

    /// @return The stack as a preference letter string.
    [[nodiscard]] std::string stackLetters() const;

    [[nodiscard]] bool graticule() const;
    void setGraticule(bool on);

    [[nodiscard]] bool percentValues() const;
    void setPercentValues(bool on);

    [[nodiscard]] int zoom() const;
    void setZoom(int level);

    [[nodiscard]] float intensity(TraceControl control) const;
    void setIntensity(TraceControl control, float percent);

    [[nodiscard]] float smoothing(TraceControl control) const;
    void setSmoothing(TraceControl control, float milliseconds);

private:
    std::vector<ScopeGlyph> m_stack{ScopeGlyph::Vectorscope};
    bool m_graticule = true;
    bool m_percentValues = false;
    int m_zoom = 1;
    float m_vectorscopeIntensity = 0.0f;
    float m_waveformIntensity = 0.0f;
    float m_vectorscopeSmoothing = 0.0f;
    float m_waveformSmoothing = 0.0f;
};

}  // namespace sidescopes
