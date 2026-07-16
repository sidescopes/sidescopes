#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "app/overlay_style.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

SsGraticulePrimitive line(uint32_t stroke)
{
    SsGraticulePrimitive primitive{};
    primitive.kind = SS_PRIMITIVE_LINE;
    primitive.stroke = stroke;
    primitive.x0 = 0.1f;
    primitive.y0 = 0.2f;
    primitive.x1 = 0.3f;
    primitive.y1 = 0.4f;
    return primitive;
}

SsMarker level(float y, uint32_t mask, float from, float to)
{
    SsMarker marker{};
    marker.kind = SS_MARKER_LEVEL;
    marker.y = y;
    marker.channel_mask = mask;
    marker.band_from = from;
    marker.band_to = to;
    return marker;
}

SsMarker value(float x, uint32_t mask, float from, float to)
{
    SsMarker marker{};
    marker.kind = SS_MARKER_VALUE;
    marker.x = x;
    marker.channel_mask = mask;
    marker.band_from = from;
    marker.band_to = to;
    return marker;
}

}  // namespace

TEST_CASE("channelMaskColor colors a mask by the mix of its channels")
{
    CHECK(channelMaskColor(0b001) == packColor(255, 90, 90, 230));    // red
    CHECK(channelMaskColor(0b010) == packColor(90, 255, 90, 230));    // green
    CHECK(channelMaskColor(0b100) == packColor(110, 110, 255, 230));  // blue
    CHECK(channelMaskColor(0b011) == packColor(255, 235, 90, 230));   // yellow
    CHECK(channelMaskColor(0b101) == packColor(255, 90, 255, 230));   // magenta
    CHECK(channelMaskColor(0b110) == packColor(90, 235, 255, 230));   // cyan
    CHECK(channelMaskColor(0b111) == packColor(235, 235, 235, 230));  // gray
}

TEST_CASE("A line takes its stroke color and the weight for its class")
{
    const GraticuleStyle vectorscope{VectorscopeMajorLineWidth, DefaultLineWidth, false};

    const StyledGraticule major = styleGraticulePrimitive(line(SS_STROKE_GRID_MAJOR), vectorscope);
    REQUIRE(major.count == 1);
    CHECK(major.commands[0].op == GraticuleOp::Line);
    CHECK(major.commands[0].color == GraticuleMajor);
    CHECK(major.commands[0].width == VectorscopeMajorLineWidth);
    // The geometry passes through untouched.
    CHECK(major.commands[0].x0 == 0.1f);
    CHECK(major.commands[0].y1 == 0.4f);

    const StyledGraticule minor = styleGraticulePrimitive(line(SS_STROKE_GRID), vectorscope);
    REQUIRE(minor.count == 1);
    CHECK(minor.commands[0].color == GraticuleMinor);
    CHECK(minor.commands[0].width == DefaultLineWidth);

    // The waveform and histogram draw even their major scale lines at one weight.
    const StyledGraticule flat = styleGraticulePrimitive(line(SS_STROKE_GRID_MAJOR), GraticuleStyle{});
    CHECK(flat.commands[0].color == GraticuleMajor);
    CHECK(flat.commands[0].width == DefaultLineWidth);
}

TEST_CASE("A circle keeps its normalized radius and the fixed segment count")
{
    SsGraticulePrimitive circle{};
    circle.kind = SS_PRIMITIVE_CIRCLE;
    circle.stroke = SS_STROKE_GRID;
    circle.x0 = 0.5f;
    circle.y0 = 0.5f;
    circle.x1 = 0.25f;

    const StyledGraticule styled = styleGraticulePrimitive(circle, GraticuleStyle{});
    REQUIRE(styled.count == 1);
    CHECK(styled.commands[0].op == GraticuleOp::Circle);
    CHECK(styled.commands[0].segments == 64);
    CHECK(styled.commands[0].x1 == 0.25f);
    CHECK(styled.commands[0].color == GraticuleMinor);
}

TEST_CASE("A primary target box carries its accent box and offset label")
{
    SsGraticulePrimitive target{};
    target.kind = SS_PRIMITIVE_TARGET_BOX;
    target.flags = SS_PRIMITIVE_FLAG_TARGET_PRIMARY;
    target.x0 = 0.6f;
    target.y0 = 0.3f;
    std::snprintf(target.label, sizeof(target.label), "R");

    const StyledGraticule styled = styleGraticulePrimitive(target, GraticuleStyle{});
    REQUIRE(styled.count == 2);
    CHECK(styled.commands[0].op == GraticuleOp::Rect);
    CHECK(styled.commands[0].halfBox == 5.0f);
    CHECK(styled.commands[0].color == GraticuleAccent);
    CHECK(styled.commands[1].op == GraticuleOp::Label);
    CHECK(styled.commands[1].offsetX == 7.0f);
    CHECK(styled.commands[1].offsetY == -7.0f);
    CHECK(styled.commands[1].color == GraticuleLabel);
    CHECK(std::string(styled.commands[1].label) == "R");
}

TEST_CASE("A secondary target box is a smaller unlabeled box")
{
    SsGraticulePrimitive target{};
    target.kind = SS_PRIMITIVE_TARGET_BOX;
    target.x0 = 0.6f;
    target.y0 = 0.3f;

    const StyledGraticule styled = styleGraticulePrimitive(target, GraticuleStyle{});
    REQUIRE(styled.count == 1);
    CHECK(styled.commands[0].op == GraticuleOp::Rect);
    CHECK(styled.commands[0].halfBox == 3.0f);
}

TEST_CASE("Minor scale labels appear only when the pane is roomy")
{
    SsGraticulePrimitive text{};
    text.kind = SS_PRIMITIVE_TEXT;
    text.x0 = 0.0f;
    text.y0 = 0.5f;
    std::snprintf(text.label, sizeof(text.label), "50");

    // Major labels (no flag) always draw, in the label ink, at the fixed offset.
    const StyledGraticule major = styleGraticulePrimitive(text, GraticuleStyle{});
    REQUIRE(major.count == 1);
    CHECK(major.commands[0].op == GraticuleOp::Label);
    CHECK(major.commands[0].offsetX == 4.0f);
    CHECK(major.commands[0].offsetY == 1.0f);
    CHECK(major.commands[0].color == GraticuleLabel);

    text.flags = SS_PRIMITIVE_FLAG_TEXT_MAJOR_ONLY;
    GraticuleStyle tight{};
    tight.roomy = false;
    CHECK(styleGraticulePrimitive(text, tight).count == 0);
    GraticuleStyle roomy{};
    roomy.roomy = true;
    CHECK(styleGraticulePrimitive(text, roomy).count == 1);
}

TEST_CASE("A single marker is a whole-color accent colored by its kind")
{
    SsMarker point{};
    point.kind = SS_MARKER_POINT;
    point.channel_mask = 0x7;
    const std::vector<StyledMarker> whitePoint = styleMarkers({point}, std::nullopt);
    REQUIRE(whitePoint.size() == 1);
    CHECK(whitePoint[0].kind == SS_MARKER_POINT);
    CHECK(whitePoint[0].color == CursorPointColor);

    const std::vector<StyledMarker> goldLevel = styleMarkers({level(0.4f, 0x7, 0.0f, 1.0f)}, std::nullopt);
    REQUIRE(goldLevel.size() == 1);
    CHECK(goldLevel[0].color == CursorLevelColor);
    CHECK(goldLevel[0].y == 0.4f);
}

TEST_CASE("An override color paints every marker without merging")
{
    // Pinned references: two points, both amber, neither merged away.
    SsMarker first{};
    first.kind = SS_MARKER_POINT;
    SsMarker second{};
    second.kind = SS_MARKER_POINT;
    const std::vector<StyledMarker> styled = styleMarkers({first, second}, PinnedPointColor);
    REQUIRE(styled.size() == 2);
    CHECK(styled[0].color == PinnedPointColor);
    CHECK(styled[1].color == PinnedPointColor);
}

TEST_CASE("Distinct channel levels each take their own channel color")
{
    // Three levels at different heights across the full width: red, green, blue.
    const std::vector<StyledMarker> styled = styleMarkers(
        {level(0.2f, 0b001, 0.0f, 1.0f), level(0.5f, 0b010, 0.0f, 1.0f), level(0.8f, 0b100, 0.0f, 1.0f)}, std::nullopt);
    REQUIRE(styled.size() == 3);
    CHECK(styled[0].color == channelMaskColor(0b001));
    CHECK(styled[1].color == channelMaskColor(0b010));
    CHECK(styled[2].color == channelMaskColor(0b100));
}

TEST_CASE("Coincident channel levels merge into the mix within a shared band")
{
    // A neutral color: three coincident full-width levels fold into one gray
    // line at the shared height.
    const std::vector<StyledMarker> merged = styleMarkers(
        {level(0.5f, 0b001, 0.0f, 1.0f), level(0.5f, 0b010, 0.0f, 1.0f), level(0.5f, 0b100, 0.0f, 1.0f)}, std::nullopt);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].color == channelMaskColor(0b111));
    CHECK(merged[0].y == 0.5f);
}

TEST_CASE("Banded markers never merge across bands, even at the same value")
{
    // The parade splits the channels into thirds: three coincident levels in
    // separate bands stay three lines, each its own channel color.
    const std::vector<StyledMarker> parade =
        styleMarkers({level(0.5f, 0b001, 0.0f, 1.0f / 3.0f), level(0.5f, 0b010, 1.0f / 3.0f, 2.0f / 3.0f),
                      level(0.5f, 0b100, 2.0f / 3.0f, 1.0f)},
                     std::nullopt);
    REQUIRE(parade.size() == 3);
    CHECK(parade[0].color == channelMaskColor(0b001));
    CHECK(parade[1].color == channelMaskColor(0b010));
    CHECK(parade[2].color == channelMaskColor(0b100));
}

TEST_CASE("Coincident value markers merge like level markers")
{
    // The combined histogram: three coincident value markers over the full
    // height fold into one.
    const std::vector<StyledMarker> merged = styleMarkers(
        {value(0.3f, 0b001, 0.0f, 1.0f), value(0.3f, 0b010, 0.0f, 1.0f), value(0.3f, 0b100, 0.0f, 1.0f)}, std::nullopt);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].kind == SS_MARKER_VALUE);
    CHECK(merged[0].color == channelMaskColor(0b111));
    CHECK(merged[0].x == 0.3f);
}

namespace {

// A synthetic instance whose graticule reports more primitives than the RAII
// wrapper's 32-entry first pass, so the wrapper must re-query with more room.
// Each primitive carries its index in x0, so a dropped one shows up as a gap.
constexpr uint32_t OversizeGraticule = 40;

uint32_t oversizeGraticule(const SsScopeInstance*, SsGraticulePrimitive* out, uint32_t capacity)
{
    for (uint32_t index = 0; index < OversizeGraticule && index < capacity; ++index) {
        out[index] = SsGraticulePrimitive{};
        out[index].kind = SS_PRIMITIVE_LINE;
        out[index].x0 = static_cast<float>(index);
    }
    return OversizeGraticule;
}

void noopDestroy(SsScopeInstance*)
{
}

}  // namespace

TEST_CASE("A graticule larger than the first pass survives the re-query intact")
{
    SsScopeInstance vtable{};
    vtable.graticule = oversizeGraticule;
    vtable.destroy = noopDestroy;
    const ScopeInstance instance(&vtable);

    const std::vector<SsGraticulePrimitive> primitives = instance.graticule();
    REQUIRE(primitives.size() == OversizeGraticule);
    // Every index is present and in order: nothing past the 32-entry first pass
    // was lost when the wrapper re-queried.
    for (uint32_t index = 0; index < OversizeGraticule; ++index) {
        CHECK(primitives[index].x0 == static_cast<float>(index));
    }
}

}  // namespace sidescopes
