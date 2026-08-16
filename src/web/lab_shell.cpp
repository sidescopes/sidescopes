#include "web/lab_shell.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "platform/desktop.h"

namespace sidescopes {
namespace shell {
namespace {

/// A binding name to the Dear ImGui key it stands for. The names are the
/// ones ShortcutResolver uses, so this is a translation and not a second
/// opinion about what any key means.
[[nodiscard]] ImGuiKey keyFor(std::string_view name)
{
    if (name.size() == 1) {
        const char one = name.front();
        if (one >= 'A' && one <= 'Z') {
            return static_cast<ImGuiKey>(ImGuiKey_A + (one - 'A'));
        }
        if (one >= 'a' && one <= 'z') {
            return static_cast<ImGuiKey>(ImGuiKey_A + (one - 'a'));
        }
        if (one >= '1' && one <= '9') {
            return static_cast<ImGuiKey>(ImGuiKey_1 + (one - '1'));
        }
    }
    if (name == "Escape") {
        return ImGuiKey_Escape;
    }
    if (name == "Comma") {
        return ImGuiKey_Comma;
    }

    return ImGuiKey_None;
}

}  // namespace

bool keyPressed(std::string_view name)
{
    const ImGuiKey key = keyFor(name);

    return key != ImGuiKey_None && ImGui::IsKeyPressed(key, /*repeat=*/false);
}

ModifierState modifiers()
{
    // Through the platform seam, exactly as App does. Reading Dear ImGui here
    // instead would give the web build two answers to one question while
    // every other platform has one, and the seam is the one the shortcut
    // resolver is written against.
    return currentModifiers();
}

std::optional<FloatColor> sampleAt(const ImVec2& point, const RegionEditor::Placement& placement,
                                   const std::vector<uint8_t>& rgba, int width, int height)
{
    if (rgba.empty() || width <= 0 || height <= 0 || placement.scale <= 0.0f) {
        return std::nullopt;
    }
    const int x = static_cast<int>(std::floor((point.x - placement.origin.x) / placement.scale));
    const int y = static_cast<int>(std::floor((point.y - placement.origin.y) / placement.scale));
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return std::nullopt;
    }
    const std::size_t at =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u;
    if (at + 2 >= rgba.size()) {
        return std::nullopt;
    }

    // The buffer handed here is the RGBA copy kept for showing the picture,
    // so no swizzle: the engines read the other one.
    return FloatColor{static_cast<float>(rgba[at]), static_cast<float>(rgba[at + 1]), static_cast<float>(rgba[at + 2])};
}

namespace {

// What the canvas's cursor property currently holds, so a value written
// every frame crosses into JavaScript only when it changes. The pin cursor
// records itself here as a marker rather than as its (kilobytes-long) CSS,
// which keeps one owner of the property and one comparison.
std::string g_cursor = "\x01";  // no CSS value, so the first call always writes

// clang-format off
// JavaScript, not C++ - see lab_storage.cpp.
EM_JS(void, jsSetCursor, (const char* css), {
    const canvas = document.getElementById("canvas");
    if (canvas) {
        canvas.style.cursor = css ? UTF8ToString(css) : "";
    }
});

// The pin cursor, drawn into an image the compositor carries.
//
// BOTH desktop platforms already do this, with these same numbers and
// tones - region_selection.mm via NSCursor, region_selection.cpp via
// CreateIconIndirect - so all three put the same thing under the same
// pointer: a crosshair with a gap at its centre, a wide dark pass under a
// narrow light one so it survives any background, and the swatch hanging
// below-right of the hotspot. Each also rebuilds only when the colour
// changes, which is what makes drawing into a cursor affordable at all.
//
// Rendered at the display's device pixel ratio and declared with image-set,
// because a data-URI cursor is otherwise measured in CSS pixels and would
// land soft on every HiDPI screen. That puts this WITH WINDOWS rather than
// with macOS: CreateIconIndirect and a CSS cursor both take raw pixels and
// need the scale passed in, while NSImage carries its own. Where image-set
// is unavailable the 1x image is used instead - correct, just less crisp -
// and the keyword after the comma is the last resort.
//
// rgb is -1 when the pointer is off the picture: crosshair, no swatch.
EM_JS(void, jsSetPinCursor, (int rgb), {
    const canvas = document.getElementById("canvas");
    if (!canvas) {
        return;
    }
    // The image is a function of the COLOUR alone. The pointer's position is
    // the compositor's business, which is the whole point of drawing here.
    if (canvas.__pinCursorRgb === rgb) {
        return;
    }
    canvas.__pinCursorRgb = rgb;

    const HOTSPOT = 12, ARM = 8, GAP = 2, OFFSET = 7, SWATCH = 13;
    const side = HOTSPOT + OFFSET + SWATCH + 2;
    const ratio = window.devicePixelRatio || 1;

    const draw = (scale) => {
        const surface = document.createElement("canvas");
        surface.width = Math.round(side * scale);
        surface.height = Math.round(side * scale);
        const pen = surface.getContext("2d");
        pen.scale(scale, scale);

        const arms = () => {
            pen.beginPath();
            pen.moveTo(HOTSPOT, HOTSPOT - GAP); pen.lineTo(HOTSPOT, HOTSPOT - ARM);
            pen.moveTo(HOTSPOT, HOTSPOT + GAP); pen.lineTo(HOTSPOT, HOTSPOT + ARM);
            pen.moveTo(HOTSPOT - GAP, HOTSPOT); pen.lineTo(HOTSPOT - ARM, HOTSPOT);
            pen.moveTo(HOTSPOT + GAP, HOTSPOT); pen.lineTo(HOTSPOT + ARM, HOTSPOT);
            pen.stroke();
        };
        pen.lineWidth = 3.2; pen.strokeStyle = "rgba(26,26,26,0.85)"; arms();
        pen.lineWidth = 1.5; pen.strokeStyle = "rgba(204,204,204,0.95)"; arms();

        if (rgb >= 0) {
            const x = HOTSPOT + OFFSET, y = HOTSPOT + OFFSET;
            pen.fillStyle = "rgb(" + ((rgb >> 16) & 255) + "," + ((rgb >> 8) & 255) + "," + (rgb & 255) + ")";
            pen.fillRect(x, y, SWATCH, SWATCH);
            pen.lineWidth = 2; pen.strokeStyle = "rgba(26,26,26,0.7)";
            pen.strokeRect(x, y, SWATCH, SWATCH);
            pen.lineWidth = 1; pen.strokeStyle = "rgba(247,247,247,0.95)";
            pen.strokeRect(x, y, SWATCH, SWATCH);
        }

        return surface.toDataURL();
    };

    const hotspot = " " + HOTSPOT + " " + HOTSPOT + ", crosshair";
    const sharp = 'image-set(url("' + draw(ratio) + '") ' + ratio + "x)" + hotspot;
    canvas.style.cursor = sharp;
    // An unparseable value leaves the property at its previous contents, so
    // the fallback has to be chosen by reading back rather than by assuming.
    if (canvas.style.cursor !== sharp) {
        canvas.style.cursor = 'url("' + draw(1) + '")' + hotspot;
    }
});

// clang-format on

}  // namespace

void setCanvasCursor(const char* css)
{
    const std::string wanted = css == nullptr ? std::string{} : std::string{css};
    if (wanted == g_cursor) {
        return;
    }
    g_cursor = wanted;
    jsSetCursor(css);
}

void setPinCursor(const std::optional<FloatColor>& colour)
{
    const int rgb = colour.has_value()
                        ? ((static_cast<int>(colour->r) & 255) << 16) | ((static_cast<int>(colour->g) & 255) << 8) |
                              (static_cast<int>(colour->b) & 255)
                        : -1;
    // Marked rather than stored: the CSS is kilobytes of data URI, and
    // marshalling it across the boundary every frame to discover it has not
    // changed would cost more than the drawing does.
    const std::string marker = "\x02" + std::to_string(rgb);
    if (marker == g_cursor) {
        return;
    }
    g_cursor = marker;
    jsSetPinCursor(rgb);
}

std::optional<FloatColor> averageOver(const SsRect& rect, const std::vector<uint8_t>& rgba, int width, int height)
{
    if (rgba.empty() || rect.width <= 0 || rect.height <= 0) {
        return std::nullopt;
    }
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    std::size_t counted = 0;
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        if (y < 0 || y >= height) {
            continue;
        }
        for (int x = rect.x; x < rect.x + rect.width; ++x) {
            if (x < 0 || x >= width) {
                continue;
            }
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u;
            if (at + 2 >= rgba.size()) {
                continue;
            }
            red += rgba[at];
            green += rgba[at + 1];
            blue += rgba[at + 2];
            ++counted;
        }
    }
    if (counted == 0) {
        return std::nullopt;
    }
    const double scale = 1.0 / static_cast<double>(counted);

    return FloatColor{static_cast<float>(red * scale), static_cast<float>(green * scale),
                      static_cast<float>(blue * scale)};
}

}  // namespace shell
}  // namespace sidescopes
