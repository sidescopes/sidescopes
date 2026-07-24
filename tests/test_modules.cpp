#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "modules/module_registry.h"
#include "scope_image.h"
#include "sidescopes/module.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

namespace {

// Minimal module-entry pieces for the registry's rejection paths. Neither the
// descriptor nor create is reached: registration is refused before it looks at
// them, so they exist only to fill valid function pointers.
bool trueInit()
{
    return true;
}

bool falseInit()
{
    return false;
}

void noopDeinit()
{
}

uint32_t oneScope()
{
    return 1;
}

const SsScopeDescriptor FakeDescriptor{
    "org.sidescopes.test.fake", "Fake", 'X', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor* fakeDescriptor(uint32_t index)
{
    return index == 0 ? &FakeDescriptor : nullptr;
}

SsScopeInstance* nullCreate(const char*, const SsHost*)
{
    return nullptr;
}

// A fake instance whose destroy() records the call, so the RAII wrapper can be
// shown to destroy exactly once across a chain of moves.
struct CountingInstance
{
    SsScopeInstance vtable{};
    int destroyed = 0;
};

void countingDestroy(SsScopeInstance* instance)
{
    ++static_cast<CountingInstance*>(instance->instance_data)->destroyed;
}

// Paired with fakeDescriptor, which answers only index 0, this makes a module
// that promises more scopes than it hands descriptors for.
uint32_t twoScopes()
{
    return 2;
}

// The brightest pixel's three channels, so a trace can be read for both how
// strong it is and what color it came out.
std::array<int, 3> peakPixel(const SsImageView& image)
{
    const std::pair<int, int> peak = brightestPixel(image);
    if (peak.first < 0) {
        return {0, 0, 0};
    }
    const uint8_t* pixel = image.rgba + (static_cast<std::size_t>(peak.second) * image.width + peak.first) * 4;

    return {pixel[0], pixel[1], pixel[2]};
}

// Every lit channel byte added up: the whole trace's weight, which the
// intensity parameters lift while the peak stays pinned by normalization.
int totalInk(const SsImageView& image)
{
    int total = 0;
    const auto pixels = static_cast<std::size_t>(image.width) * image.height;
    for (std::size_t index = 0; index < pixels; ++index) {
        const uint8_t* pixel = image.rgba + index * 4;
        total += pixel[0] + pixel[1] + pixel[2];
    }

    return total;
}

// How many image columns carry any trace at all: the sampling stride thins
// these out without dimming what survives.
int litColumns(const SsImageView& image)
{
    int columns = 0;
    for (int px = 0; px < image.width; ++px) {
        for (int py = 0; py < image.height; ++py) {
            const uint8_t* pixel = image.rgba + (static_cast<std::size_t>(py) * image.width + px) * 4;
            if (pixel[0] + pixel[1] + pixel[2] > 0) {
                ++columns;

                break;
            }
        }
    }

    return columns;
}

// A horizontal ramp: every column a different gray, so a stride that skips
// columns visibly narrows what the scopes see.
TestFrame grayRamp(int width, int height)
{
    TestFrame frame(width, height, 255);
    for (int px = 0; px < width; ++px) {
        const auto level = static_cast<uint8_t>(px * 255 / (width - 1));
        for (int py = 0; py < height; ++py) {
            frame.setColor(px, py, Color{level, level, level});
        }
    }

    return frame;
}

SsFrameView viewOf(const TestFrame& frame)
{
    return SsFrameView{frame.pixels.data(), frame.width * 4, frame.width, frame.height, SS_COLOR_SPACE_SRGB, 1};
}

}  // namespace

TEST_CASE("Registry serves the vectorscope through the module boundary")
{
    ModuleRegistry& registry = builtinModules();
    const RegisteredScope* scope = registry.findScope("org.sidescopes.vectorscope");
    REQUIRE(scope != nullptr);
    CHECK(scope->descriptor->letter == 'V');
    CHECK(scope->descriptor->param_count == 4);

    ScopeInstance instance = registry.createInstance("org.sidescopes.vectorscope");
    REQUIRE(instance.valid());

    // Defaults through the boundary must match the engine's: BT.709
    // puts 75% red at bin (109, 43).
    TestFrame red(8, 8, 0);
    red.fill(0, 8, Color{191, 0, 0});
    const SsFrameView frame{red.pixels.data(), 8 * 4, 8, 8, SS_COLOR_SPACE_SRGB, 1};
    REQUIRE(instance.accumulate(frame, SsRect{0, 0, 8, 8}));
    const SsImageView image = instance.image();
    REQUIRE(image.width == 256);
    CHECK(brightestPixel(image) == std::pair<int, int>{109, 43});

    // Reconfigure to BT.601 through parameters: the peak moves to (100, 43).
    std::vector<SsParamValue> values{{"matrix", 0.0}};
    REQUIRE(instance.configure(values));
    REQUIRE(instance.accumulate(frame, SsRect{0, 0, 8, 8}));
    CHECK(brightestPixel(instance.image()) == std::pair<int, int>{100, 43});

    // Overlays and markers arrive as declarative data.
    CHECK(instance.graticule().size() > 8);
    const std::vector<SsMarker> markers = instance.markers(SsColor{191.0f, 0.0f, 0.0f});
    REQUIRE(markers.size() == 1);
    CHECK(markers[0].kind == SS_MARKER_POINT);
    CHECK(markers[0].channel_mask == 0x7u);

    // The adaptive-image extension resizes the display grid.
    const auto* adaptive = static_cast<const SsAdaptiveImageExtension*>(instance.getExtension(AdaptiveImageExtension));
    REQUIRE(adaptive != nullptr);
}

TEST_CASE("Registry serves the waveform through the module boundary")
{
    ModuleRegistry& registry = builtinModules();
    const RegisteredScope* scope = registry.findScope("org.sidescopes.waveform");
    REQUIRE(scope != nullptr);
    CHECK(scope->descriptor->letter == 'W');
    CHECK(scope->descriptor->image_width == 1024);
    CHECK(scope->descriptor->image_height == 256);
    CHECK(scope->descriptor->param_count == 3);

    ScopeInstance instance = registry.createInstance("org.sidescopes.waveform");
    REQUIRE(instance.valid());

    // Luma mode (choice 1): mid gray sits at luma 128, which the engine
    // plots on image row 255 - 128 = 127.
    REQUIRE(instance.configure(std::vector<SsParamValue>{{"mode", 1.0}}));
    TestFrame gray(32, 16, 128);
    const SsFrameView frame{gray.pixels.data(), 32 * 4, 32, 16, SS_COLOR_SPACE_SRGB, 1};
    REQUIRE(instance.accumulate(frame, SsRect{0, 0, 32, 16}));
    const SsImageView image = instance.image();
    REQUIRE(image.height == 256);
    CHECK(brightestRow(image, 0) == 127);

    // Luma carries a single full-width level marker.
    const std::vector<SsMarker> luma = instance.markers(SsColor{128.0f, 128.0f, 128.0f});
    REQUIRE(luma.size() == 1);
    CHECK(luma[0].kind == SS_MARKER_LEVEL);
    CHECK(luma[0].channel_mask == 0x7u);

    // RGB mode (choice 0) returns one level per channel.
    REQUIRE(instance.configure(std::vector<SsParamValue>{{"mode", 0.0}}));
    const std::vector<SsMarker> rgb = instance.markers(SsColor{128.0f, 128.0f, 128.0f});
    REQUIRE(rgb.size() == 3);
    for (const SsMarker& marker : rgb) {
        CHECK(marker.kind == SS_MARKER_LEVEL);
    }
}

TEST_CASE("Registry serves the parade through the module boundary")
{
    ModuleRegistry& registry = builtinModules();
    const RegisteredScope* scope = registry.findScope("org.sidescopes.parade");
    REQUIRE(scope != nullptr);
    CHECK(scope->descriptor->letter == 'R');
    CHECK(scope->descriptor->param_count == 2);

    ScopeInstance instance = registry.createInstance("org.sidescopes.parade");
    REQUIRE(instance.valid());

    // Three per-channel levels, each confined to its own third; the green
    // channel's band opens at 1/3.
    const std::vector<SsMarker> markers = instance.markers(SsColor{10.0f, 150.0f, 240.0f});
    REQUIRE(markers.size() == 3);
    for (const SsMarker& marker : markers) {
        CHECK(marker.kind == SS_MARKER_LEVEL);
    }
    CHECK(markers[1].band_from == Catch::Approx(1.0f / 3.0f));
    CHECK(markers[1].band_to == Catch::Approx(2.0f / 3.0f));
}

TEST_CASE("Registry serves the histogram through the module boundary")
{
    ModuleRegistry& registry = builtinModules();
    const RegisteredScope* scope = registry.findScope("org.sidescopes.histogram");
    REQUIRE(scope != nullptr);
    CHECK(scope->descriptor->letter == 'H');
    CHECK(scope->descriptor->param_count == 2);

    ScopeInstance instance = registry.createInstance("org.sidescopes.histogram");
    REQUIRE(instance.valid());

    TestFrame gray(32, 16, 128);
    const SsFrameView frame{gray.pixels.data(), 32 * 4, 32, 16, SS_COLOR_SPACE_SRGB, 1};
    REQUIRE(instance.accumulate(frame, SsRect{0, 0, 32, 16}));

    // The outline extension hands back three channels of bin heights.
    const auto* outline = static_cast<const SsOutlineExtension*>(instance.getExtension(OutlineExtension));
    REQUIRE(outline != nullptr);
    std::vector<float> heights(static_cast<std::size_t>(3) * 256);
    const uint32_t total = outline->heights(instance.raw(), heights.data(), static_cast<uint32_t>(heights.size()));
    CHECK(total == 3u * 256u);

    // Three vertical value markers, one per channel.
    const std::vector<SsMarker> markers = instance.markers(SsColor{128.0f, 128.0f, 128.0f});
    REQUIRE(markers.size() == 3);
    for (const SsMarker& marker : markers) {
        CHECK(marker.kind == SS_MARKER_VALUE);
    }
}

TEST_CASE("The histogram's graticule is a quartered grid")
{
    const ScopeInstance histogram = builtinModules().createInstance("org.sidescopes.histogram");
    REQUIRE(histogram.valid());

    // A vertical at every quarter of the code range, the two ends and the
    // midpoint drawn heavier than the quarters between them.
    const std::vector<SsGraticulePrimitive> primitives = histogram.graticule();
    REQUIRE(primitives.size() == 5);
    for (std::size_t quarter = 0; quarter < primitives.size(); ++quarter) {
        const SsGraticulePrimitive& line = primitives[quarter];
        const float x = static_cast<float>(quarter) / 4.0f;
        CHECK(line.kind == SS_PRIMITIVE_LINE);
        CHECK(line.x0 == Catch::Approx(x));
        CHECK(line.x1 == Catch::Approx(x));
        CHECK(line.y0 == Catch::Approx(0.0f));
        CHECK(line.y1 == Catch::Approx(1.0f));
        CHECK(line.stroke == (quarter % 2 == 0 ? SS_STROKE_GRID_MAJOR : SS_STROKE_GRID));
    }
}

TEST_CASE("The waveform's graticule labels every scale line it draws")
{
    const ScopeInstance waveform = builtinModules().createInstance("org.sidescopes.waveform");
    REQUIRE(waveform.valid());

    // Eleven levels from 100 at the top down to 0, each a full-width rule
    // followed by its own label.
    const std::vector<SsGraticulePrimitive> primitives = waveform.graticule();
    REQUIRE(primitives.size() == 22);
    for (std::size_t step = 0; step < 11; ++step) {
        const SsGraticulePrimitive& rule = primitives[step * 2];
        const SsGraticulePrimitive& label = primitives[step * 2 + 1];
        const float y = static_cast<float>(step) / 10.0f;
        CHECK(rule.kind == SS_PRIMITIVE_LINE);
        CHECK(rule.x0 == Catch::Approx(0.0f));
        CHECK(rule.x1 == Catch::Approx(1.0f));
        CHECK(rule.y0 == Catch::Approx(y));
        CHECK(rule.y1 == Catch::Approx(y));

        CHECK(label.kind == SS_PRIMITIVE_TEXT);
        CHECK(label.y0 == Catch::Approx(y));
        CHECK(std::string(label.label) == std::to_string(100 - static_cast<int>(step) * 10));

        // Every tenth is drawn, but only every fiftieth is heavy - and the
        // light ones ask the host to drop their labels where they would crowd.
        const bool major = step % 5 == 0;
        CHECK(rule.stroke == (major ? SS_STROKE_GRID_MAJOR : SS_STROKE_GRID));
        CHECK(label.stroke == rule.stroke);
        CHECK(((label.flags & SS_PRIMITIVE_FLAG_TEXT_MAJOR_ONLY) != 0U) == !major);
    }
}

TEST_CASE("A graticule reports the room it needs past the buffer it was given")
{
    // The host offers a fixed buffer and regrows on the answer, so a module
    // must count every primitive it would draw while writing only as many as
    // it was given room for.
    const ScopeInstance waveform = builtinModules().createInstance("org.sidescopes.waveform");
    REQUIRE(waveform.valid());

    constexpr uint32_t Offered = 4;
    std::vector<SsGraticulePrimitive> buffer(8);
    for (SsGraticulePrimitive& primitive : buffer) {
        primitive.kind = 0xBADU;
    }
    const uint32_t needed = waveform.raw()->graticule(waveform.raw(), buffer.data(), Offered);

    CHECK(needed == 22U);
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        const bool written = index < Offered;
        CHECK((buffer[index].kind != 0xBADU) == written);
    }
}

TEST_CASE("The histogram's combined style lifts its markers to full height")
{
    ScopeInstance histogram = builtinModules().createInstance("org.sidescopes.histogram");
    REQUIRE(histogram.valid());

    // Per channel (the default) stacks the three plots, so each marker is
    // confined to its own third.
    const std::vector<SsMarker> banded = histogram.markers(SsColor{10.0f, 150.0f, 240.0f});
    REQUIRE(banded.size() == 3);
    CHECK(banded[1].band_from == Catch::Approx(1.0f / 3.0f));
    CHECK(banded[1].band_to == Catch::Approx(2.0f / 3.0f));
    CHECK(banded[2].x == Catch::Approx(240.0f / 255.0f));
    CHECK(banded[2].channel_mask == 0x4U);

    // Combined overlays the plots, so every marker spans the whole plot.
    REQUIRE(histogram.configure(std::vector<SsParamValue>{{"style", 1.0}}));
    const std::vector<SsMarker> combined = histogram.markers(SsColor{10.0f, 150.0f, 240.0f});
    REQUIRE(combined.size() == 3);
    for (const SsMarker& marker : combined) {
        CHECK(marker.kind == SS_MARKER_VALUE);
        CHECK(marker.band_from == Catch::Approx(0.0f));
        CHECK(marker.band_to == Catch::Approx(1.0f));
    }
}

TEST_CASE("The waveform's colored luma keeps the trace's own hue")
{
    ScopeInstance waveform = builtinModules().createInstance("org.sidescopes.waveform");
    REQUIRE(waveform.valid());

    TestFrame red(32, 16, 255);
    red.fill(Color{200, 0, 0});
    const SsFrameView frame = viewOf(red);

    // Plain luma (choice 1) plots a neutral trace.
    REQUIRE(waveform.configure(std::vector<SsParamValue>{{"mode", 1.0}}));
    REQUIRE(waveform.accumulate(frame, SsRect{0, 0, 32, 16}));
    const std::array<int, 3> neutral = peakPixel(waveform.image());
    CHECK(neutral[0] == neutral[2]);

    // Colored luma (choice 2) plots the same level in the color that made it.
    REQUIRE(waveform.configure(std::vector<SsParamValue>{{"mode", 2.0}}));
    REQUIRE(waveform.accumulate(frame, SsRect{0, 0, 32, 16}));
    const std::array<int, 3> colored = peakPixel(waveform.image());
    CHECK(colored[0] > colored[2]);

    // Both luma flavors carry the one full-width level marker.
    const std::vector<SsMarker> markers = waveform.markers(SsColor{200.0f, 0.0f, 0.0f});
    REQUIRE(markers.size() == 1);
    CHECK(markers[0].kind == SS_MARKER_LEVEL);
    CHECK(markers[0].channel_mask == 0x7U);
}

TEST_CASE("An unknown style choice leaves the waveform on RGB")
{
    ScopeInstance waveform = builtinModules().createInstance("org.sidescopes.waveform");
    REQUIRE(waveform.valid());

    // A choice outside the declared range must not strand the scope in a mode
    // it cannot name; RGB is the fallback, which its three markers report.
    REQUIRE(waveform.configure(std::vector<SsParamValue>{{"mode", 7.0}}));
    CHECK(waveform.markers(SsColor{128.0f, 128.0f, 128.0f}).size() == 3);
}

TEST_CASE("The parade refuses to be configured off its own layout")
{
    ScopeInstance parade = builtinModules().createInstance("org.sidescopes.parade");
    REQUIRE(parade.valid());

    // The parade is defined by its side-by-side mode and declares no style
    // parameter; a stray one from another scope's settings must not move it.
    REQUIRE(parade.configure(std::vector<SsParamValue>{{"mode", 1.0}}));
    const std::vector<SsMarker> markers = parade.markers(SsColor{10.0f, 150.0f, 240.0f});

    REQUIRE(markers.size() == 3);
    CHECK(markers[0].band_from == Catch::Approx(0.0f));
    CHECK(markers[0].band_to == Catch::Approx(1.0f / 3.0f));
}

TEST_CASE("The sampling stride reaches every engine that declares it")
{
    const TestFrame ramp = grayRamp(64, 8);
    const SsFrameView frame = viewOf(ramp);

    ScopeInstance waveform = builtinModules().createInstance("org.sidescopes.waveform");
    REQUIRE(waveform.valid());
    REQUIRE(waveform.accumulate(frame, SsRect{0, 0, 64, 8}));
    const int denseColumns = litColumns(waveform.image());

    // Every eighth pixel means a eighth of the ramp's columns are ever
    // sampled, and the waveform's per-row normalization keeps what survives
    // just as bright.
    REQUIRE(waveform.configure(std::vector<SsParamValue>{{"stride", 8.0}}));
    REQUIRE(waveform.accumulate(frame, SsRect{0, 0, 64, 8}));
    const int sparseColumns = litColumns(waveform.image());
    CHECK(sparseColumns * 4 < denseColumns);

    ScopeInstance histogram = builtinModules().createInstance("org.sidescopes.histogram");
    REQUIRE(histogram.valid());
    REQUIRE(histogram.accumulate(frame, SsRect{0, 0, 64, 8}));
    const int denseHistogram = litColumns(histogram.image());

    REQUIRE(histogram.configure(std::vector<SsParamValue>{{"stride", 8.0}}));
    REQUIRE(histogram.accumulate(frame, SsRect{0, 0, 64, 8}));
    CHECK(litColumns(histogram.image()) < denseHistogram);
}

TEST_CASE("The intensity and response parameters reach the vectorscope")
{
    // A dominant color with a thin second one beside it: the peak normalizes
    // to the dense bin either way, so what gain and response move is how far
    // the sparse trace is lifted towards it.
    TestFrame mixed(16, 16, 255);
    mixed.fill(Color{191, 0, 0});
    mixed.setColor(0, 0, Color{0, 191, 0});
    const SsFrameView frame = viewOf(mixed);

    ScopeInstance vectorscope = builtinModules().createInstance("org.sidescopes.vectorscope");
    REQUIRE(vectorscope.valid());

    // A sparse trace is what the boosted response exists for: linear leaves
    // it dimmer than the default boost does.
    REQUIRE(vectorscope.configure(std::vector<SsParamValue>{{"response", 1.0}}));
    REQUIRE(vectorscope.accumulate(frame, SsRect{0, 0, 16, 16}));
    const int linear = totalInk(vectorscope.image());

    REQUIRE(vectorscope.configure(std::vector<SsParamValue>{{"response", 0.0}}));
    REQUIRE(vectorscope.accumulate(frame, SsRect{0, 0, 16, 16}));
    const int boosted = totalInk(vectorscope.image());
    CHECK(boosted > linear);

    // Gain scales the same trace on top of whichever response is in force.
    REQUIRE(vectorscope.configure(std::vector<SsParamValue>{{"gain", 0.005}}));
    REQUIRE(vectorscope.accumulate(frame, SsRect{0, 0, 16, 16}));
    const int quiet = totalInk(vectorscope.image());
    REQUIRE(vectorscope.configure(std::vector<SsParamValue>{{"gain", 0.5}}));
    REQUIRE(vectorscope.accumulate(frame, SsRect{0, 0, 16, 16}));
    CHECK(totalInk(vectorscope.image()) > quiet);
}

TEST_CASE("The adaptive-image extension sizes the vectorscope by its height")
{
    ScopeInstance vectorscope = builtinModules().createInstance("org.sidescopes.vectorscope");
    REQUIRE(vectorscope.valid());

    // The vectorscope is square by construction, so it takes the height and
    // ignores the width the host offers.
    const auto* adaptive =
        static_cast<const SsAdaptiveImageExtension*>(vectorscope.getExtension(AdaptiveImageExtension));
    REQUIRE(adaptive != nullptr);
    adaptive->setImageSize(vectorscope.raw(), 900, 384);

    const SsImageView resized = vectorscope.image();
    CHECK(resized.width == 384);
    CHECK(resized.height == 384);
}

TEST_CASE("The adaptive-image extension resizes the histogram's plot")
{
    ScopeInstance histogram = builtinModules().createInstance("org.sidescopes.histogram");
    REQUIRE(histogram.valid());
    CHECK(histogram.getExtension("sidescopes.not-an-extension") == nullptr);

    // The histogram's bins are fixed at 256 codes; what the host sizes is the
    // plot it strokes them into.
    const auto* adaptive = static_cast<const SsAdaptiveImageExtension*>(histogram.getExtension(AdaptiveImageExtension));
    REQUIRE(adaptive != nullptr);
    adaptive->setImageSize(histogram.raw(), 1024, 384);

    const SsImageView resized = histogram.image();
    CHECK(resized.width == 1024);
    CHECK(resized.height == 384);
}

TEST_CASE("A module refuses to create a scope it does not own")
{
    // Creation is inert and answers null for an unknown id, which is what
    // lets a host offer an id to its modules in turn.
    const ModuleRegistry& registry = builtinModules();
    const RegisteredScope* waveform = registry.findScope("org.sidescopes.waveform");
    const RegisteredScope* histogram = registry.findScope("org.sidescopes.histogram");
    REQUIRE(waveform != nullptr);
    REQUIRE(histogram != nullptr);

    CHECK(waveform->module->create("org.sidescopes.histogram", nullptr) == nullptr);
    CHECK(histogram->module->create("org.sidescopes.waveform", nullptr) == nullptr);
}

TEST_CASE("Registry skips a scope its module never describes")
{
    // A module that counts two scopes but describes one would otherwise be
    // dereferenced at lookup time.
    const SsModuleEntry entry{SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, twoScopes, fakeDescriptor, nullCreate};
    ModuleRegistry registry;

    REQUIRE(registry.registerModule(entry));
    CHECK(registry.scopes().size() == 1);
    CHECK(registry.findScope("org.sidescopes.test.fake") != nullptr);
}

TEST_CASE("Registry rejects a module built for another ABI major")
{
    const SsModuleEntry entry{99u, 0u, trueInit, noopDeinit, oneScope, fakeDescriptor, nullCreate};
    ModuleRegistry registry;
    CHECK_FALSE(registry.registerModule(entry));
    CHECK(registry.scopes().empty());
}

TEST_CASE("Registry rejects a module whose init fails")
{
    const SsModuleEntry entry{SS_ABI_MAJOR, SS_ABI_MINOR, falseInit, noopDeinit, oneScope, fakeDescriptor, nullCreate};
    ModuleRegistry registry;
    CHECK_FALSE(registry.registerModule(entry));
    CHECK(registry.scopes().empty());
}

TEST_CASE("Registry reports unknown scopes and instances as absent")
{
    ModuleRegistry& registry = builtinModules();
    CHECK(registry.findScope("org.sidescopes.nonesuch") == nullptr);
    CHECK_FALSE(registry.createInstance("org.sidescopes.nonesuch").valid());
}

TEST_CASE("A scope instance reports only the extensions it implements")
{
    ModuleRegistry& registry = builtinModules();

    const ScopeInstance vectorscope = registry.createInstance("org.sidescopes.vectorscope");
    REQUIRE(vectorscope.valid());
    CHECK(vectorscope.getExtension("sidescopes.not-an-extension") == nullptr);
    CHECK(vectorscope.getExtension(OutlineExtension) == nullptr);
    CHECK(vectorscope.getExtension(AdaptiveImageExtension) != nullptr);

    const ScopeInstance waveform = registry.createInstance("org.sidescopes.waveform");
    REQUIRE(waveform.valid());
    CHECK(waveform.getExtension(OutlineExtension) == nullptr);
    CHECK(waveform.getExtension(AdaptiveImageExtension) != nullptr);

    const ScopeInstance histogram = registry.createInstance("org.sidescopes.histogram");
    REQUIRE(histogram.valid());
    CHECK(histogram.getExtension(AdaptiveImageExtension) != nullptr);
    CHECK(histogram.getExtension(OutlineExtension) != nullptr);
}

TEST_CASE("The adaptive-image extension resizes a scope's output through the boundary")
{
    ModuleRegistry& registry = builtinModules();
    ScopeInstance waveform = registry.createInstance("org.sidescopes.waveform");
    REQUIRE(waveform.valid());

    const auto* adaptive = static_cast<const SsAdaptiveImageExtension*>(waveform.getExtension(AdaptiveImageExtension));
    REQUIRE(adaptive != nullptr);

    // The waveform's default grid is 1024 columns by 256 levels.
    CHECK(waveform.image().width == 1024);
    CHECK(waveform.image().height == 256);

    adaptive->setImageSize(waveform.raw(), 512, 512);
    const SsImageView resized = waveform.image();
    CHECK(resized.width == 512);
    CHECK(resized.height == 512);
}

TEST_CASE("ScopeInstance owns its handle across moves and destroys it once")
{
    CountingInstance fake;
    fake.vtable.instance_data = &fake;
    fake.vtable.destroy = countingDestroy;

    {
        ScopeInstance first(&fake.vtable);
        REQUIRE(first.valid());

        // Move-construct: ownership transfers and the source empties.
        ScopeInstance second(std::move(first));
        CHECK(second.valid());
        // Reading the moved-from state is the whole point of the check.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move,bugprone-use-after-move)
        CHECK_FALSE(first.valid());

        // Move-assign into an empty instance.
        ScopeInstance third;
        third = std::move(second);
        CHECK(third.valid());
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move,bugprone-use-after-move)
        CHECK_FALSE(second.valid());

        // Self-move must neither destroy nor invalidate the handle. The
        // indirection dodges the compiler's self-move diagnostic while still
        // exercising the guarded a = std::move(a) path.
        ScopeInstance* alias = &third;
        third = std::move(*alias);
        CHECK(third.valid());

        CHECK(fake.destroyed == 0);  // nothing destroyed while an owner is live
    }

    CHECK(fake.destroyed == 1);  // destroyed exactly once at the final scope exit
}

}  // namespace sidescopes
