#pragma once

#include <filesystem>

#include "core/analysis_worker.h"

namespace sidescopes {

// Everything worth remembering between sessions. Serialized as a small
// key=value text file: trivially diffable, no dependency, and unknown keys
// are ignored so older builds tolerate newer files.
struct Preferences {
    float vectorscope_gain = 3.0f;
    float waveform_gain = 0.05f;
    int vectorscope_stride = 1;
    int waveform_stride = 1;
    float histogram_gain = 1.0f;
    int histogram_stride = 1;
    float vectorscope_smoothing_ms = 75.0f;
    float waveform_smoothing_ms = 100.0f;
    ChromaMatrix matrix = ChromaMatrix::Bt601;
    WaveformMode waveform_mode = WaveformMode::Rgb;
    // Bit set of scopes stacked in the window: 1 vectorscope, 2 waveform,
    // 4 histogram. At least one bit is always set.
    int visible_scopes = 1;
    bool show_graticule = true;
    bool values_as_percent = true;
    int window_x = -1;  // negative: let the system place the window
    int window_y = -1;
    int window_width = 440;
    int window_height = 500;
};

// Missing or unreadable files yield the defaults; malformed lines and
// unknown keys are skipped.
Preferences LoadPreferences(const std::filesystem::path& file);

// Creates parent directories as needed. Returns false when writing failed.
bool SavePreferences(const Preferences& preferences, const std::filesystem::path& file);

}  // namespace sidescopes
