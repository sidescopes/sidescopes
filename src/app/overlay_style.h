#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "sidescopes/module.h"

namespace sidescopes {

/// Packs an RGBA color the way Dear ImGui's IM_COL32 does with its default
/// channel order, so the pure styling layer can name colors without depending
/// on the toolkit header; the render shim hands these straight to ImDrawList.
constexpr uint32_t packColor(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

// Every scope speaks with one graticule voice: the same champagne gold at a
// handful of strengths, plus the label ink.
constexpr uint32_t GraticuleMinor = packColor(205, 172, 110, 70);
constexpr uint32_t GraticuleMajor = packColor(205, 172, 110, 130);
constexpr uint32_t GraticuleAccent = packColor(218, 175, 95, 180);
constexpr uint32_t GraticuleLabel = packColor(226, 198, 145, 200);
constexpr uint32_t GraticuleSkinTone = packColor(230, 170, 140, 160);

/// The live cursor's point marker on the vectorscope: white, to read over the
/// trace whatever its color.
constexpr uint32_t CursorPointColor = packColor(255, 255, 255, 255);
/// The luma waveform's single cursor level: gold, the accent for a whole-color
/// marker that carries no per-channel meaning.
constexpr uint32_t CursorLevelColor = packColor(255, 220, 80, 220);
/// Pinned vectorscope reference markers: amber, host state drawn over the trace.
constexpr uint32_t PinnedPointColor = packColor(230, 170, 90, 230);

/// The vectorscope draws its 30-degree tick marks a touch heavier than the rest
/// of the grid; the waveform and histogram keep every scale line at one weight.
constexpr float VectorscopeMajorLineWidth = 1.5f;
constexpr float DefaultLineWidth = 1.0f;

/// Minor scale labels crowd a short pane, so the host draws them only when the
/// pane is at least this tall.
constexpr float RoomyPaneHeight = 140.0f;

/// Merges cursor markers that fold onto one another into the color of the mix -
/// gray for all three channels, yellow, magenta, or cyan for a pair - rather
/// than whichever channel drew last. The mask unions the merged channels:
/// 1 red, 2 green, 4 blue.
[[nodiscard]] uint32_t channelMaskColor(uint32_t mask);

/// Host styling the ABI deliberately leaves out of the primitive data.
struct GraticuleStyle
{
    float majorLineWidth = DefaultLineWidth;
    float minorLineWidth = DefaultLineWidth;
    bool roomy = false;  ///< Whether pane-crowding minor labels are drawn.
};

enum class GraticuleOp
{
    Line,
    Circle,
    Rect,
    Label
};

/// One resolved draw instruction in normalized [0,1] scope coordinates. The
/// styling - color, width, box size, segment count, visibility - is decided
/// here; the render shim maps the geometry to the screen and calls the toolkit.
struct GraticuleDrawCommand
{
    GraticuleOp op = GraticuleOp::Line;
    float x0 = 0.0f;  ///< Line from / circle center / rect center / label anchor.
    float y0 = 0.0f;
    float x1 = 0.0f;  ///< Line to (x) / circle radius (normalized).
    float y1 = 0.0f;  ///< Line to (y).
    uint32_t color = 0;
    float width = DefaultLineWidth;  ///< Line thickness.
    int segments = 0;                ///< Circle segment count.
    float halfBox = 0.0f;            ///< Rect half-extent, pixels.
    float offsetX = 0.0f;            ///< Label offset from its anchor, pixels.
    float offsetY = 0.0f;
    char label[16] = {};
};

/// The commands a primitive resolves to: a target box also emits its label, so
/// a primitive can produce up to two.
struct StyledGraticule
{
    GraticuleDrawCommand commands[2];
    int count = 0;
};

/// Resolves one graticule primitive to its draw commands under @p style. A
/// major-only text primitive resolves to nothing unless the style is roomy.
[[nodiscard]] StyledGraticule styleGraticulePrimitive(const SsGraticulePrimitive& primitive,
                                                      const GraticuleStyle& style);

/// One resolved cursor marker: its kind, position, band confinement, and color.
struct StyledMarker
{
    uint32_t kind = 0;
    float x = 0.0f;
    float y = 0.0f;
    float bandFrom = 0.0f;
    float bandTo = 1.0f;
    uint32_t color = 0;
};

/// Styles the markers a scope returned for one cursor color. With @p
/// overrideColor set, every marker takes that color (the vectorscope's pinned
/// references). A single marker is a whole-color accent colored by its kind;
/// several markers are per-channel, so coincident ones sharing a band merge and
/// take the color of the union of their channels.
[[nodiscard]] std::vector<StyledMarker> styleMarkers(const std::vector<SsMarker>& markers,
                                                     std::optional<uint32_t> overrideColor);

}  // namespace sidescopes
