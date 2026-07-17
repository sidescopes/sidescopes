#include "app/overlay_style.h"

#include <cmath>
#include <cstring>

namespace sidescopes {
namespace {

uint32_t strokeColor(uint32_t stroke)
{
    switch (stroke) {
    case SS_STROKE_GRID_MAJOR:
        return GraticuleMajor;
    case SS_STROKE_ACCENT:
        return GraticuleAccent;
    case SS_STROKE_SKIN_TONE:
        return GraticuleSkinTone;
    default:
        return GraticuleMinor;
    }
}

// A whole-color marker - one marker with no per-channel meaning - takes a
// scope-appropriate accent: white for the vectorscope's point, gold for the
// luma waveform's level.
uint32_t accentColor(uint32_t kind)
{
    switch (kind) {
    case SS_MARKER_POINT:
        return CursorPointColor;
    case SS_MARKER_LEVEL:
        return CursorLevelColor;
    default:
        return channelMaskColor(0x7);
    }
}

float markerAxis(const SsMarker& marker)
{
    return marker.kind == SS_MARKER_VALUE ? marker.x : marker.y;
}

void styleGraticuleLine(StyledGraticule& out, const SsGraticulePrimitive& primitive, const GraticuleStyle& style)
{
    GraticuleDrawCommand& command = out.commands[out.count++];
    command.op = GraticuleOp::Line;
    command.x0 = primitive.x0;
    command.y0 = primitive.y0;
    command.x1 = primitive.x1;
    command.y1 = primitive.y1;
    command.color = strokeColor(primitive.stroke);
    command.width = primitive.stroke == SS_STROKE_GRID_MAJOR ? style.majorLineWidth : style.minorLineWidth;
}

void styleGraticuleCircle(StyledGraticule& out, const SsGraticulePrimitive& primitive)
{
    GraticuleDrawCommand& command = out.commands[out.count++];
    command.op = GraticuleOp::Circle;
    command.x0 = primitive.x0;
    command.y0 = primitive.y0;
    command.x1 = primitive.x1;  // normalized radius
    command.color = strokeColor(primitive.stroke);
    command.segments = 64;
}

void styleGraticuleTargetBox(StyledGraticule& out, const SsGraticulePrimitive& primitive)
{
    GraticuleDrawCommand& box = out.commands[out.count++];
    box.op = GraticuleOp::Rect;
    box.x0 = primitive.x0;
    box.y0 = primitive.y0;
    box.color = GraticuleAccent;
    box.halfBox = (primitive.flags & SS_PRIMITIVE_FLAG_TARGET_PRIMARY) != 0u ? 5.0f : 3.0f;
    if (primitive.label[0] != '\0') {
        GraticuleDrawCommand& label = out.commands[out.count++];
        label.op = GraticuleOp::Label;
        label.x0 = primitive.x0;
        label.y0 = primitive.y0;
        label.color = GraticuleLabel;
        label.offsetX = 7.0f;
        label.offsetY = -7.0f;
        std::memcpy(label.label, primitive.label, sizeof(label.label));
    }
}

void styleGraticuleText(StyledGraticule& out, const SsGraticulePrimitive& primitive, const GraticuleStyle& style)
{
    const bool majorOnly = (primitive.flags & SS_PRIMITIVE_FLAG_TEXT_MAJOR_ONLY) != 0u;
    if (!majorOnly || style.roomy) {
        GraticuleDrawCommand& command = out.commands[out.count++];
        command.op = GraticuleOp::Label;
        command.x0 = primitive.x0;
        command.y0 = primitive.y0;
        command.color = GraticuleLabel;
        command.offsetX = 4.0f;
        command.offsetY = 1.0f;
        std::memcpy(command.label, primitive.label, sizeof(command.label));
    }
}

// Per-channel markers: merge coincident ones that share a band, greedily in
// channel order, so a neutral color reads as one line rather than three.
// Positions are averaged and channel masks unioned, exactly as the previous
// per-scope code did in the 0..255 value space - the axis is affine in the
// value, so an epsilon of two codes maps to 2/255 here.
std::vector<StyledMarker> mergePerChannelMarkers(const std::vector<SsMarker>& markers)
{
    constexpr float MergeEpsilon = 2.0f / 255.0f;
    std::vector<StyledMarker> styled;
    std::vector<bool> grouped(markers.size(), false);
    for (std::size_t i = 0; i < markers.size(); ++i) {
        if (grouped[i]) {
            continue;
        }
        uint32_t mask = markers[i].channel_mask;
        float sum = markerAxis(markers[i]);
        int members = 1;
        for (std::size_t j = i + 1; j < markers.size(); ++j) {
            if (grouped[j]) {
                continue;
            }
            if (markers[j].band_from == markers[i].band_from && markers[j].band_to == markers[i].band_to &&
                std::abs(markerAxis(markers[j]) - markerAxis(markers[i])) <= MergeEpsilon) {
                grouped[j] = true;
                mask |= markers[j].channel_mask;
                sum += markerAxis(markers[j]);
                ++members;
            }
        }
        const float axis = sum / static_cast<float>(members);
        StyledMarker marker;
        marker.kind = markers[i].kind;
        marker.bandFrom = markers[i].band_from;
        marker.bandTo = markers[i].band_to;
        marker.color = channelMaskColor(mask);
        if (markers[i].kind == SS_MARKER_VALUE) {
            marker.x = axis;
            marker.y = markers[i].y;
        } else {
            marker.y = axis;
            marker.x = markers[i].x;
        }
        styled.push_back(marker);
    }

    return styled;
}

}  // namespace

uint32_t channelMaskColor(uint32_t mask)
{
    switch (mask) {
    case 0b001:
        return packColor(255, 90, 90, 230);  // red
    case 0b010:
        return packColor(90, 255, 90, 230);  // green
    case 0b100:
        return packColor(110, 110, 255, 230);  // blue
    case 0b011:
        return packColor(255, 235, 90, 230);  // red+green: yellow
    case 0b101:
        return packColor(255, 90, 255, 230);  // red+blue: magenta
    case 0b110:
        return packColor(90, 235, 255, 230);  // green+blue: cyan
    default:
        return packColor(235, 235, 235, 230);  // all three: gray
    }
}

StyledGraticule styleGraticulePrimitive(const SsGraticulePrimitive& primitive, const GraticuleStyle& style)
{
    StyledGraticule out;
    switch (primitive.kind) {
    case SS_PRIMITIVE_LINE:
        styleGraticuleLine(out, primitive, style);
        break;
    case SS_PRIMITIVE_CIRCLE:
        styleGraticuleCircle(out, primitive);
        break;
    case SS_PRIMITIVE_TARGET_BOX:
        styleGraticuleTargetBox(out, primitive);
        break;
    case SS_PRIMITIVE_TEXT:
        styleGraticuleText(out, primitive, style);
        break;
    default:
        break;
    }

    return out;
}

std::vector<StyledMarker> styleMarkers(const std::vector<SsMarker>& markers, std::optional<uint32_t> overrideColor)
{
    std::vector<StyledMarker> styled;
    if (markers.empty()) {
        return styled;
    }
    if (overrideColor) {
        for (const SsMarker& marker : markers) {
            styled.push_back(
                StyledMarker{marker.kind, marker.x, marker.y, marker.band_from, marker.band_to, *overrideColor});
        }

        return styled;
    }
    if (markers.size() == 1) {
        const SsMarker& marker = markers.front();
        styled.push_back(
            StyledMarker{marker.kind, marker.x, marker.y, marker.band_from, marker.band_to, accentColor(marker.kind)});

        return styled;
    }

    return mergePerChannelMarkers(markers);
}

}  // namespace sidescopes
