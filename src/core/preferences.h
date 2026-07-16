#pragma once

#include <filesystem>
#include <string>

#include "core/scopes/scope_types.h"

namespace sidescopes {

// Keyboard bindings for every shortcut the application offers, editable
// in the preferences file. Each is a single letter A-Z or "Escape";
// anything else falls back to the default. Shift composes where an
// action documents it (stacking scopes, pinning the region average).
struct ShortcutBindings
{
    std::string vectorscope = "V";
    std::string waveform = "W";
    std::string parade = "R";
    std::string histogram = "H";
    std::string colorPicker = "C";
    std::string pickWindow = "A";
    std::string drawRegion = "D";
    std::string pickFaces = "F";
    std::string pinColor = "P";
    std::string vectorscopeZoom = "Z";
    std::string fullRegion = "Escape";
};

// Everything worth remembering between sessions. Serialized as a small
// key=value text file: trivially diffable, no dependency, and unknown keys
// are ignored so older builds tolerate newer files.
struct Preferences
{
    float vectorscopeGain = 3.0f;
    float waveformGain = 0.05f;
    int vectorscopeStride = 1;
    int waveformStride = 1;
    int histogramStride = 1;
    float vectorscopeSmoothingMs = 75.0f;
    float waveformSmoothingMs = 100.0f;
    ChromaMatrix matrix = ChromaMatrix::Bt709;
    TraceResponse traceResponse = TraceResponse::Boosted;
    // The waveform scope's style; the parade is its own scope.
    WaveformMode waveformMode = WaveformMode::Rgb;
    bool histogramPerChannel = true;
    // The scopes on screen, one letter each in stacking order: V
    // vectorscope, W waveform, R RGB parade, H histogram, C color picker.
    // Never empty. The retired L (a separate luma waveform) is accepted
    // only as a legacy migration input, folded into W in its Luma style.
    std::string scopeStack = "V";
    bool showGraticule = true;
    // Magnify-view factor for the vectorscope: 1, 2, or 4.
    int vectorscopeZoom = 1;
    int windowX = -1;  // negative: let the system place the window
    int windowY = -1;
    int windowWidth = 440;
    int windowHeight = 500;
    ShortcutBindings shortcuts;
};

// Missing or unreadable files yield the defaults; malformed lines and
// unknown keys are skipped.
[[nodiscard]] Preferences loadPreferences(const std::filesystem::path& file);

// Creates parent directories as needed. Returns false when writing failed.
[[nodiscard]] bool savePreferences(const Preferences& preferences, const std::filesystem::path& file);

}  // namespace sidescopes
