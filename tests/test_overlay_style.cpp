#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
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

uint32_t alphaOf(uint32_t color)
{
    return (color >> 24) & 0xFFu;
}

/// Every ink the graticule is drawn with, so a sweep covers the palette rather
/// than one colour that happens to be convenient.
const std::vector<uint32_t> EveryInk = {GraticuleMinor, GraticuleMajor, GraticuleAccent, GraticuleLabel,
                                        GraticuleSkinTone};

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

TEST_CASE("Strength scales an ink's alpha and leaves its color alone")
{
    for (const uint32_t ink : EveryInk) {
        // The graded strength is the ink itself, to the byte: every test above
        // that names a colour is also the guard on that.
        CHECK(graticuleInk(ink, DefaultGraticuleStrength) == ink);
        // Dimming and brightening move only the alpha.
        CHECK((graticuleInk(ink, 0.5f) & 0x00FFFFFFu) == (ink & 0x00FFFFFFu));
        CHECK(alphaOf(graticuleInk(ink, 0.5f)) == static_cast<uint32_t>(std::lround(alphaOf(ink) * 0.5)));
        CHECK(alphaOf(graticuleInk(ink, 1.25f)) == static_cast<uint32_t>(std::lround(alphaOf(ink) * 1.25)));
    }
    // Opacity is the ceiling, not an overflow.
    CHECK(alphaOf(graticuleInk(GraticuleLabel, 8.0f)) == 255u);
}

TEST_CASE("The quietest graticule is still one the tool asks you to read")
{
    // THE FLOOR'S WHOLE ARGUMENT. At the lowest strength the lines that carry
    // the reading - the majors - land within a tenth of the alpha the SHIPPING
    // DEFAULT gives its minor lines, a level the instrument already relies on
    // being legible, and the labels and target boxes land above it. Nothing can
    // vanish, which is what lets a strength replace an on/off.
    const float floor = GraticuleStrengths.front();
    CHECK(floor < DefaultGraticuleStrength);
    CHECK(alphaOf(graticuleInk(GraticuleMajor, floor)) * 10u >= alphaOf(GraticuleMinor) * 9u);
    CHECK(alphaOf(graticuleInk(GraticuleLabel, floor)) >= alphaOf(GraticuleMinor));
    CHECK(alphaOf(graticuleInk(GraticuleAccent, floor)) >= alphaOf(GraticuleMinor));
    for (const uint32_t ink : EveryInk) {
        CHECK(alphaOf(graticuleInk(ink, floor)) >= 32u);
    }
}

TEST_CASE("The loudest graticule keeps its hierarchy")
{
    // THE CEILING'S ARGUMENT: the top step is where the brightest ink is about
    // to reach full opacity. No class may clamp there, or two elements the
    // palette deliberately separates would arrive at the same alpha and a
    // target box would stop reading louder than a grid line.
    const float ceiling = GraticuleStrengths.back();
    CHECK(ceiling > DefaultGraticuleStrength);
    for (const uint32_t ink : EveryInk) {
        CHECK(alphaOf(graticuleInk(ink, ceiling)) == static_cast<uint32_t>(std::lround(alphaOf(ink) * ceiling)));
        CHECK(alphaOf(graticuleInk(ink, ceiling)) < 255u);
    }
    CHECK(alphaOf(graticuleInk(GraticuleAccent, ceiling)) > alphaOf(graticuleInk(GraticuleMajor, ceiling)));
    CHECK(alphaOf(graticuleInk(GraticuleMajor, ceiling)) > alphaOf(graticuleInk(GraticuleMinor, ceiling)));
}

TEST_CASE("Every strength has one name, and the ladder climbs")
{
    // The words are a ladder of weight - Faint below Normal below Strong below
    // Bold - so they only tell the truth while the values climb in the same
    // order. Reordering or inserting a step without touching the words would
    // leave the menu describing the wrong ink, which nothing else would catch.
    REQUIRE(GraticuleStrengthNames.size() == GraticuleStrengths.size());
    for (std::size_t step = 1; step < GraticuleStrengths.size(); ++step) {
        INFO("step " << step << " named " << GraticuleStrengthNames[step]);
        CHECK(GraticuleStrengths[step] > GraticuleStrengths[step - 1]);
    }
    for (const char* name : GraticuleStrengthNames) {
        REQUIRE(name != nullptr);
        CHECK(std::string(name).find('%') == std::string::npos);
    }

    // NORMAL MUST NAME THE DEFAULT. The menu marks no step as the shipped one,
    // so this word is the only way back to what the scopes were graded at; put
    // it on a different rung and a user reaching for stock lands one step off,
    // with nothing on screen to tell them. The first wording shipped for review
    // did exactly that.
    const auto normal = std::find(GraticuleStrengthNames.begin(), GraticuleStrengthNames.end(), std::string("Normal"));
    REQUIRE(normal != GraticuleStrengthNames.end());
    const auto step = static_cast<std::size_t>(std::distance(GraticuleStrengthNames.begin(), normal));
    CHECK(GraticuleStrengths[step] == DefaultGraticuleStrength);
}

TEST_CASE("A stored strength snaps to an offered step")
{
    for (const float step : GraticuleStrengths) {
        CHECK(cleanedGraticuleStrength(step) == step);
    }
    CHECK(cleanedGraticuleStrength(0.72f) == 0.75f);
    // A hand-edited extreme lands on the nearest bound rather than off scale.
    CHECK(cleanedGraticuleStrength(0.01f) == GraticuleStrengths.front());
    CHECK(cleanedGraticuleStrength(40.0f) == GraticuleStrengths.back());
    // What says nothing about strength says nothing: back to the default.
    CHECK(cleanedGraticuleStrength(0.0f) == DefaultGraticuleStrength);
    CHECK(cleanedGraticuleStrength(-2.0f) == DefaultGraticuleStrength);
    CHECK(cleanedGraticuleStrength(std::numeric_limits<float>::quiet_NaN()) == DefaultGraticuleStrength);
}

TEST_CASE("Every styled primitive is laid down at the chosen strength")
{
    // The scopes differ in WHICH primitives they emit, not in how loud one
    // is: the strength lands on the stroke class each module chose, so a
    // vectorscope target and a waveform scale line each dim on their own ink.
    GraticuleStyle quiet;
    quiet.strength = GraticuleStrengths.front();
    quiet.roomy = true;

    CHECK(styleGraticulePrimitive(line(SS_STROKE_GRID), quiet).commands[0].color ==
          graticuleInk(GraticuleMinor, quiet.strength));
    CHECK(styleGraticulePrimitive(line(SS_STROKE_GRID_MAJOR), quiet).commands[0].color ==
          graticuleInk(GraticuleMajor, quiet.strength));
    CHECK(styleGraticulePrimitive(line(SS_STROKE_SKIN_TONE), quiet).commands[0].color ==
          graticuleInk(GraticuleSkinTone, quiet.strength));

    SsGraticulePrimitive circle{};
    circle.kind = SS_PRIMITIVE_CIRCLE;
    circle.stroke = SS_STROKE_GRID;
    CHECK(styleGraticulePrimitive(circle, quiet).commands[0].color == graticuleInk(GraticuleMinor, quiet.strength));

    SsGraticulePrimitive target{};
    target.kind = SS_PRIMITIVE_TARGET_BOX;
    target.flags = SS_PRIMITIVE_FLAG_TARGET_PRIMARY;
    std::snprintf(target.label, sizeof(target.label), "R");
    const StyledGraticule box = styleGraticulePrimitive(target, quiet);
    REQUIRE(box.count == 2);
    CHECK(box.commands[0].color == graticuleInk(GraticuleAccent, quiet.strength));
    CHECK(box.commands[1].color == graticuleInk(GraticuleLabel, quiet.strength));

    SsGraticulePrimitive text{};
    text.kind = SS_PRIMITIVE_TEXT;
    std::snprintf(text.label, sizeof(text.label), "50");
    CHECK(styleGraticulePrimitive(text, quiet).commands[0].color == graticuleInk(GraticuleLabel, quiet.strength));
}

TEST_CASE("A quieter graticule is drawn, not thinned out")
{
    // Strength is not a filter: the same primitives resolve to the same
    // commands at every step, so quietening the graticule can never take an
    // element away. What decides whether a minor label is drawn stays the
    // pane's room, which the strength does not touch.
    SsGraticulePrimitive text{};
    text.kind = SS_PRIMITIVE_TEXT;
    text.flags = SS_PRIMITIVE_FLAG_TEXT_MAJOR_ONLY;
    std::snprintf(text.label, sizeof(text.label), "40");

    for (const float strength : GraticuleStrengths) {
        GraticuleStyle roomy;
        roomy.roomy = true;
        roomy.strength = strength;
        CHECK(styleGraticulePrimitive(text, roomy).count == 1);
        CHECK(styleGraticulePrimitive(line(SS_STROKE_GRID), roomy).count == 1);

        GraticuleStyle tight = roomy;
        tight.roomy = false;
        CHECK(styleGraticulePrimitive(text, tight).count == 0);
        CHECK(styleGraticulePrimitive(line(SS_STROKE_GRID), tight).count == 1);
    }
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
