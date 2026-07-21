#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
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
