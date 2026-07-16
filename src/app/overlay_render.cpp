#include "app/overlay_render.h"

namespace sidescopes {
namespace {

// A whole-color point marker: a bright ring over a dark one, so it reads on any
// trace. Radii and weights are carried verbatim from the previous host draw.
void drawPointMarker(const DrawnScope& scope, float nx, float ny, ImU32 color)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = at(scope, nx, ny);
    draw->AddCircle(center, 5.0f, color, 0, 2.0f);
    draw->AddCircle(center, 6.5f, IM_COL32(0, 0, 0, 200), 0, 1.0f);
}

void drawLevelMarker(const DrawnScope& scope, float normalizedY, ImU32 color, float fromX, float toX)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float y = scope.origin.y + normalizedY * scope.size.y;
    draw->AddLine(ImVec2(scope.origin.x + fromX * scope.size.x, y), ImVec2(scope.origin.x + toX * scope.size.x, y),
                  color, 1.5f);
}

void drawValueMarker(const DrawnScope& scope, float normalizedX, ImU32 color, float fromY, float toY)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float x = scope.origin.x + normalizedX * scope.size.x;
    draw->AddLine(ImVec2(x, scope.origin.y + fromY * scope.size.y), ImVec2(x, scope.origin.y + toY * scope.size.y),
                  color, 1.5f);
}

}  // namespace

ImVec2 at(const DrawnScope& scope, float nx, float ny)
{
    const float x = (nx - 0.5f) * scope.zoom + 0.5f;
    const float y = (ny - 0.5f) * scope.zoom + 0.5f;
    return ImVec2(scope.origin.x + x * scope.size.x, scope.origin.y + y * scope.size.y);
}

void drawGraticule(const DrawnScope& scope, const std::vector<SsGraticulePrimitive>& primitives, GraticuleStyle style)
{
    style.roomy = scope.size.y >= RoomyPaneHeight;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (const SsGraticulePrimitive& primitive : primitives) {
        const StyledGraticule styled = styleGraticulePrimitive(primitive, style);
        for (int index = 0; index < styled.count; ++index) {
            const GraticuleDrawCommand& command = styled.commands[index];
            switch (command.op) {
            case GraticuleOp::Line:
                draw->AddLine(at(scope, command.x0, command.y0), at(scope, command.x1, command.y1), command.color,
                              command.width);
                break;
            case GraticuleOp::Circle:
                draw->AddCircle(at(scope, command.x0, command.y0), command.x1 * scope.size.x * scope.zoom,
                                command.color, command.segments);
                break;
            case GraticuleOp::Rect: {
                const ImVec2 center = at(scope, command.x0, command.y0);
                draw->AddRect(ImVec2(center.x - command.halfBox, center.y - command.halfBox),
                              ImVec2(center.x + command.halfBox, center.y + command.halfBox), command.color);
                break;
            }
            case GraticuleOp::Label: {
                const ImVec2 anchor = at(scope, command.x0, command.y0);
                draw->AddText(ImVec2(anchor.x + command.offsetX, anchor.y + command.offsetY), command.color,
                              command.label);
                break;
            }
            }
        }
    }
}

void drawMarkers(const DrawnScope& scope, const std::vector<SsMarker>& markers, std::optional<uint32_t> overrideColor)
{
    for (const StyledMarker& marker : styleMarkers(markers, overrideColor)) {
        switch (marker.kind) {
        case SS_MARKER_POINT:
            drawPointMarker(scope, marker.x, marker.y, marker.color);
            break;
        case SS_MARKER_LEVEL:
            drawLevelMarker(scope, marker.y, marker.color, marker.bandFrom, marker.bandTo);
            break;
        case SS_MARKER_VALUE:
            drawValueMarker(scope, marker.x, marker.color, marker.bandFrom, marker.bandTo);
            break;
        default:
            break;
        }
    }
}

}  // namespace sidescopes
