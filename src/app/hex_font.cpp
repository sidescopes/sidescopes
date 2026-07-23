#include "app/hex_font.h"

namespace sidescopes {

void pushHexFont(ImFont* font)
{
    if (font) {
        ImGui::PushFont(font);
    }
}

void popHexFont(ImFont* font)
{
    if (font) {
        ImGui::PopFont();
    }
}

float hexFontWidth(ImFont* font, const char* text)
{
    pushHexFont(font);
    const float measured = ImGui::CalcTextSize(text).x;
    popHexFont(font);

    return measured;
}

float hexFontBaselineDrop(ImFont* font)
{
    const float interfaceAscent = ImGui::GetFontBaked()->Ascent;
    pushHexFont(font);
    const float drop = interfaceAscent - ImGui::GetFontBaked()->Ascent;
    popHexFont(font);

    return drop;
}

void hexFontText(ImFont* font, const char* text)
{
    const float drop = hexFontBaselineDrop(font);
    pushHexFont(font);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + drop);
    ImGui::TextUnformatted(text);
    popHexFont(font);
}

}  // namespace sidescopes
