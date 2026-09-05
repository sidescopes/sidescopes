#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <random>
#include <string>

#include "boundary_properties.h"
#include "temp_file.h"

namespace {

constexpr uint64_t BoundarySeed = 0x5C0FE20260905;

template <typename Check>
void randomBytes(Check check)
{
    std::mt19937_64 random{BoundarySeed};
    std::array<uint8_t, 1200> bytes{};
    for (unsigned trial = 0; trial < 128; ++trial) {
        for (auto& byte : bytes) {
            byte = static_cast<uint8_t>(random());
        }
        INFO("seed=" << BoundarySeed << " trial=" << trial);
        REQUIRE_NOTHROW(check(bytes));
    }
}

}  // namespace

TEST_CASE("Randomized preferences remain valid and stabilize after serialization", "[preferences][property]")
{
    sidescopes::test::TempDir directory{"preference-properties"};
    constexpr std::array Keys{"scope_stack",
                              "scope_order",
                              "window_x",
                              "window_y",
                              "window_width",
                              "window_height",
                              "graticule_strength",
                              "vectorscope_zoom",
                              "layout_orientation",
                              "layout_weights",
                              "layout_active_slot",
                              "ui_scale_factor",
                              "quality",
                              "pins",
                              "pin_comparator",
                              "layout.preset1.stack",
                              "layout.preset1.name",
                              "layout.preset1.weights",
                              "layout.preset1.styles",
                              "com.example.scope.gain",
                              "shortcut_com.example.scope"};
    constexpr std::array Values{"",
                                "0",
                                "-1",
                                "1",
                                "2147483648",
                                "1.7976931348623157e308",
                                "1e-999",
                                "nan",
                                "inf",
                                "1junk",
                                "0.10000000000000001",
                                "A",
                                "[com.example.scope]",
                                "com.example.scope:0.125",
                                "com.example.scope.style:1",
                                "#FF0080,00FF80",
                                "  Long preset name with extra spaces  ",
                                "line\r\r",
                                "[[nested]][]"};
    std::mt19937_64 random{BoundarySeed};
    for (unsigned trial = 0; trial < 128; ++trial) {
        std::string input;
        for (unsigned line = 0; line < 16; ++line) {
            input += Keys[random() % Keys.size()];
            input += '=';
            input += Values[random() % Values.size()];
            input += '\n';
        }
        INFO("seed=" << BoundarySeed << " trial=" << trial << " input=" << input);
        REQUIRE_NOTHROW(sidescopes::test::checkPreferencesProperties(input, directory.path()));
    }
}

TEST_CASE("Randomized region geometry preserves intersections and coordinate conversions", "[geometry][property]")
{
    randomBytes(sidescopes::test::checkGeometryProperties);
}

TEST_CASE("Randomized module boundaries validate descriptors and apply parameter batches atomically",
          "[modules][property]")
{
    randomBytes(sidescopes::test::checkModuleProperties);
}
