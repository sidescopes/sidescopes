#pragma once

#include <filesystem>
#include <string>

#include "core/analysis_worker.h"

namespace sidescopes {

// Keyboard bindings for every shortcut the application offers, editable
// in the preferences file. Each is a single letter A-Z or "Escape";
// anything else falls back to the default. Shift composes where an
// action documents it (stacking scopes, pinning the region average).
struct ShortcutBindings {
    std::string vectorscope = "V";
    std::string waveform = "W";
    std::string parade = "R";
    std::string histogram = "H";
    std::string color_picker = "C";
    std::string pick_window = "A";
    std::string draw_region = "D";
    std::string pick_faces = "F";
    std::string pin_color = "P";
    std::string vectorscope_zoom = "Z";
    std::string full_region = "Escape";
};

// Everything worth remembering between sessions. Serialized as a small
// key=value text file: trivially diffable, no dependency, and unknown keys
// are ignored so older builds tolerate newer files.
struct Preferences {
    float vectorscope_gain = 3.0f;
    float waveform_gain = 0.05f;
    int vectorscope_stride = 1;
    int waveform_stride = 1;
    int histogram_stride = 1;
    float vectorscope_smoothing_ms = 75.0f;
    float waveform_smoothing_ms = 100.0f;
    ChromaMatrix matrix = ChromaMatrix::Bt709;
    TraceResponse trace_response = TraceResponse::Boosted;
    // The waveform scope's style; the parade is its own scope.
    WaveformMode waveform_mode = WaveformMode::Rgb;
    bool histogram_per_channel = true;
    // The scopes on screen, one letter each in stacking order: V
    // vectorscope, W RGB waveform, L luma waveform, R RGB parade, H
    // histogram. Never empty.
    std::string scope_stack = "V";
    bool show_graticule = true;
    // Magnify-view factor for the vectorscope: 1, 2, or 4.
    int vectorscope_zoom = 1;
    bool values_as_percent = true;
    int window_x = -1;  // negative: let the system place the window
    int window_y = -1;
    int window_width = 440;
    int window_height = 500;
    ShortcutBindings shortcuts;
};

// Missing or unreadable files yield the defaults; malformed lines and
// unknown keys are skipped.
Preferences LoadPreferences(const std::filesystem::path& file);

// Creates parent directories as needed. Returns false when writing failed.
bool SavePreferences(const Preferences& preferences, const std::filesystem::path& file);

}  // namespace sidescopes
