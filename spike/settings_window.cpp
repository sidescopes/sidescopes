// SideScopes settings-window look-check spike.
//
// A throwaway, standalone program that renders a SAMPLE settings window so
// the owner can judge, on macOS and Windows, whether a system-adjacent ImGui
// restyle reads acceptably beside native apps. Nothing here is wired to the
// real app: every value is fake, held in locals and globals. Only the LOOK
// and the recorder INTERACTION are meant to be faithful.
//
// Build: cmake -B build -DSIDESCOPES_SPIKE_SETTINGS=ON
//        cmake --build build --target sidescopes_spike_settings
//
// Renders through imgui_impl_glfw + imgui_impl_opengl3 on both platforms
// (deprecated GL on macOS is fine for a spike and gives identical pixels).

#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "advapi32.lib")
#endif

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

namespace {

// --- palette -------------------------------------------------------------

// Every colour the window paints. Two hand-tuned instances (light and dark)
// approximate macOS System Settings / Windows 11 Settings.
struct Palette
{
    ImVec4 contentBg;  // the pane behind the cards
    ImVec4 sidebarBg;  // the navigation column
    ImVec4 cardBg;     // grouped rounded cards
    ImVec4 fieldBg;    // combos and slider tracks
    ImVec4 fieldHover;
    ImVec4 fieldActive;
    ImVec4 switchOff;  // a toggle in its off position
    ImVec4 text;
    ImVec4 textMuted;
    ImVec4 accent;     // selection, checkmarks, slider grabs
    ImVec4 separator;  // hairlines and card borders
    ImVec4 hover;      // sidebar row hover wash
    ImVec4 conflict;   // the shortcut-conflict warning
};

ImVec4 Rgb(int r, int g, int b, float a = 1.0f)
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

Palette LightPalette()
{
    Palette p;
    p.contentBg = Rgb(245, 245, 247);
    p.sidebarBg = Rgb(233, 233, 237);
    p.cardBg = Rgb(255, 255, 255);
    p.fieldBg = Rgb(239, 239, 242);
    p.fieldHover = Rgb(229, 229, 234);
    p.fieldActive = Rgb(221, 221, 226);
    p.switchOff = Rgb(225, 225, 229);
    p.text = Rgb(29, 29, 31);
    p.textMuted = Rgb(138, 138, 143);
    p.accent = Rgb(10, 132, 255);
    p.separator = Rgb(214, 214, 219);
    p.hover = Rgb(0, 0, 0, 0.05f);
    p.conflict = Rgb(207, 52, 52);

    return p;
}

Palette DarkPalette()
{
    Palette p;
    p.contentBg = Rgb(30, 30, 30);
    p.sidebarBg = Rgb(38, 38, 40);
    p.cardBg = Rgb(44, 44, 46);
    p.fieldBg = Rgb(58, 58, 60);
    p.fieldHover = Rgb(68, 68, 70);
    p.fieldActive = Rgb(78, 78, 80);
    p.switchOff = Rgb(72, 72, 74);
    p.text = Rgb(245, 245, 247);
    p.textMuted = Rgb(142, 142, 147);
    p.accent = Rgb(10, 132, 255);
    p.separator = Rgb(58, 58, 60);
    p.hover = Rgb(255, 255, 255, 0.06f);
    p.conflict = Rgb(255, 105, 97);

    return p;
}

// --- global spike state --------------------------------------------------

float g_uiScale = 1.0f;  // logical-point -> ImGui-unit factor (see main)
Palette g_pal = DarkPalette();
ImFont* g_font = nullptr;

int g_tab = 0;  // 0 General, 1 Scopes, 2 Shortcuts, 3 About

// General tab.
int g_appearance = 2;  // 0 Light, 1 Dark, 2 System
bool g_showGraticule = true;
bool g_resumeCapture = false;
int g_display = 0;

// Scopes tab (fake, per-scope).
struct ScopeState
{
    float intensity;
    float detail;
    float smoothing;
    int styleA;
    int styleB;
};

ScopeState g_vectorscope = {68.0f, 55.0f, 30.0f, 1, 0};
ScopeState g_waveform = {60.0f, 48.0f, 25.0f, 0, 0};
ScopeState g_histogram = {72.0f, 50.0f, 20.0f, 0, 0};

// Shortcuts tab.
struct Shortcut
{
    const char* action;
    char key;
};

Shortcut g_shortcuts[] = {
    {"Vectorscope", 'V'},  {"Waveform", 'W'}, {"RGB Parade", 'R'}, {"Histogram", 'H'},
    {"Color Picker", 'C'}, {"Pin", 'P'},      {"Zoom", 'Z'},       {"Pick Window", 'A'},
};
const int g_shortcutCount = static_cast<int>(sizeof(g_shortcuts) / sizeof(g_shortcuts[0]));

int g_recordingRow = -1;  // row awaiting a key, -1 when idle
int g_conflictRow = -1;   // the OTHER row the pending key already belongs to
char g_pendingKey = 0;    // the key just pressed, held while a conflict shows

// A convenience: convert a palette colour to a packed draw-list colour.
ImU32 Col(const ImVec4& c)
{
    return ImGui::GetColorU32(c);
}

float Scaled(float value)
{
    return value * g_uiScale;
}

// --- system appearance ---------------------------------------------------

bool DetectSystemDark()
{
#if defined(__APPLE__)
    FILE* pipe = popen("defaults read -g AppleInterfaceStyle 2>/dev/null", "r");
    if (pipe == nullptr) {
        return false;
    }
    char buffer[64] = {0};
    bool dark = false;
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        dark = std::strstr(buffer, "Dark") != nullptr;
    }
    pclose(pipe);

    return dark;
#elif defined(_WIN32)
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status == ERROR_SUCCESS) {
        return value == 0;
    }

    return false;
#else
    return false;
#endif
}

bool ResolveDark(int appearance)
{
    if (appearance == 0) {
        return false;
    }
    if (appearance == 1) {
        return true;
    }

    return DetectSystemDark();
}

// --- style ---------------------------------------------------------------

// Sets metrics once, in logical points, then scales the whole style to the
// display. Colours are applied separately so an appearance change never
// re-scales the metrics.
void ApplyMetrics(float scale)
{
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.WindowPadding = ImVec2(16.0f, 12.0f);  // card interior breathing room
    style.FramePadding = ImVec2(11.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 9.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.WindowRounding = 6.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.GrabMinSize = 18.0f;
    style.FrameBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.WindowBorderSize = 0.0f;
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 6.0f;
    style.ScaleAllSizes(scale);
}

void ApplyPalette(const Palette& p)
{
    g_pal = p;
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_Text] = p.text;
    colors[ImGuiCol_TextDisabled] = p.textMuted;
    colors[ImGuiCol_WindowBg] = p.contentBg;
    colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_PopupBg] = p.cardBg;
    colors[ImGuiCol_Border] = p.separator;
    colors[ImGuiCol_FrameBg] = p.fieldBg;
    colors[ImGuiCol_FrameBgHovered] = p.fieldHover;
    colors[ImGuiCol_FrameBgActive] = p.fieldActive;
    colors[ImGuiCol_Button] = p.fieldBg;
    colors[ImGuiCol_ButtonHovered] = p.fieldHover;
    colors[ImGuiCol_ButtonActive] = p.fieldActive;
    colors[ImGuiCol_Header] = p.accent;
    colors[ImGuiCol_HeaderHovered] = p.fieldHover;
    colors[ImGuiCol_HeaderActive] = p.accent;
    colors[ImGuiCol_CheckMark] = p.accent;
    colors[ImGuiCol_SliderGrab] = p.accent;
    colors[ImGuiCol_SliderGrabActive] = p.accent;
    colors[ImGuiCol_Separator] = p.separator;
    colors[ImGuiCol_SeparatorHovered] = p.separator;
    colors[ImGuiCol_SeparatorActive] = p.accent;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = p.fieldHover;
    colors[ImGuiCol_ScrollbarGrabHovered] = p.fieldActive;
    colors[ImGuiCol_ScrollbarGrabActive] = p.textMuted;
}

// --- small drawn widgets -------------------------------------------------

// A rounded, coloured icon tile with a simple white glyph, in the spirit of
// the colour-tile icons macOS System Settings shows beside each category.
void DrawNavIcon(ImDrawList* dl, ImVec2 tileMin, float size, int kind, ImU32 tileColor)
{
    const ImVec2 tileMax = ImVec2(tileMin.x + size, tileMin.y + size);
    dl->AddRectFilled(tileMin, tileMax, tileColor, size * 0.28f);
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    const ImVec2 c = ImVec2(tileMin.x + size * 0.5f, tileMin.y + size * 0.5f);
    const float t = Scaled(1.4f);
    const float r = size * 0.26f;
    if (kind == 0) {  // General: a gear-ish ring
        dl->AddCircle(c, r, white, 16, t);
        dl->AddCircleFilled(c, size * 0.08f, white);
    } else if (kind == 1) {  // Scopes: a vectorscope circle with a crosshair
        dl->AddCircle(c, r, white, 20, t);
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), white, t);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), white, t);
    } else if (kind == 2) {  // Shortcuts: a key cap
        dl->AddRect(ImVec2(c.x - size * 0.24f, c.y - size * 0.18f), ImVec2(c.x + size * 0.24f, c.y + size * 0.18f),
                    white, size * 0.12f, 0, t);
        dl->AddCircleFilled(c, size * 0.05f, white);
    } else {  // About: an info "i"
        dl->AddCircleFilled(ImVec2(c.x, c.y - size * 0.17f), size * 0.055f, white);
        dl->AddLine(ImVec2(c.x, c.y - size * 0.02f), ImVec2(c.x, c.y + size * 0.2f), white, t * 1.4f);
    }
}

// A full-width sidebar navigation row with hover and selected states.
bool NavItem(const char* label, int kind, const ImVec4& tileColor, bool selected)
{
    ImGui::PushID(label);
    const float pad = Scaled(6.0f);
    const float tile = Scaled(22.0f);
    const float rowH = tile + pad * 2.0f;
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("hit", ImVec2(w, rowH));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float round = Scaled(7.0f);
    if (selected) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), Col(g_pal.accent), round);
    } else if (hovered) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), Col(g_pal.hover), round);
    }
    DrawNavIcon(dl, ImVec2(p.x + pad, p.y + pad), tile, kind, Col(tileColor));
    const ImU32 textCol = selected ? IM_COL32(255, 255, 255, 255) : Col(g_pal.text);
    const float th = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(p.x + pad + tile + pad * 1.4f, p.y + (rowH - th) * 0.5f), textCol, label);
    ImGui::PopID();

    return clicked;
}

// An iOS/macOS-style toggle switch of the given width.
bool ToggleSwitch(const char* id, bool* value, float width)
{
    ImGui::PushID(id);
    const float frameH = ImGui::GetFrameHeight();
    const bool clicked = ImGui::InvisibleButton("sw", ImVec2(width, frameH));
    if (clicked) {
        *value = !*value;
    }
    const ImVec2 p = ImGui::GetItemRectMin();
    const float h = frameH * 0.82f;
    const float cy = p.y + frameH * 0.5f;
    const ImVec2 sMin = ImVec2(p.x, cy - h * 0.5f);
    const ImVec2 sMax = ImVec2(p.x + width, cy + h * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(sMin, sMax, *value ? Col(g_pal.accent) : Col(g_pal.switchOff), h * 0.5f);
    const float knobR = h * 0.5f - Scaled(2.0f);
    const float knobX = *value ? (sMax.x - knobR - Scaled(2.0f)) : (sMin.x + knobR + Scaled(2.0f));
    dl->AddCircleFilled(ImVec2(knobX, cy), knobR, IM_COL32(255, 255, 255, 255));
    ImGui::PopID();

    return clicked;
}

// --- card rows -----------------------------------------------------------

// Lays out a label on the left and positions the cursor for a right-aligned
// control of the given width, the way a grouped settings list reads.
void BeginRow(const char* label, float controlW)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > controlW) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - controlW));
    }
    ImGui::SetNextItemWidth(controlW);
}

void ToggleRow(const char* label, const char* id, bool* value)
{
    const float w = Scaled(42.0f);
    BeginRow(label, w);
    ToggleSwitch(id, value, w);
}

void ComboRow(const char* label, const char* id, int* value, const char* const* items, int count, float width)
{
    BeginRow(label, width);
    ImGui::Combo(id, value, items, count);
}

void SliderRow(const char* label, const char* id, float* value, float width)
{
    BeginRow(label, width);
    ImGui::SliderFloat(id, value, 0.0f, 100.0f, "%.0f");
}

bool BeginCard(const char* id, float width)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_pal.cardBg);
    ImGui::PushStyleColor(ImGuiCol_Border, g_pal.separator);
    const bool open = ImGui::BeginChild(id, ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

    return open;
}

void EndCard()
{
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

void SectionHeader(const char* text)
{
    ImGui::Dummy(ImVec2(0.0f, Scaled(8.0f)));
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.textMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, Scaled(2.0f)));
}

// Opens a tab body with a large title and a left indent, returning the width
// a card should span.
float BeginTabBody(const char* title)
{
    ImGui::Dummy(ImVec2(0.0f, Scaled(16.0f)));
    ImGui::Indent(Scaled(28.0f));
    ImGui::PushFont(g_font, Scaled(22.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, Scaled(8.0f)));

    return ImGui::GetContentRegionAvail().x - Scaled(28.0f);
}

void EndTabBody()
{
    ImGui::Unindent(Scaled(28.0f));
    ImGui::Dummy(ImVec2(0.0f, Scaled(20.0f)));
}

// --- tabs ----------------------------------------------------------------

void DrawGeneralTab()
{
    static const char* const appearanceItems[] = {"Light", "Dark", "System"};
    static const char* const displayItems[] = {"Built-in Retina Display", "LG UltraFine 5K", "DELL U2720Q"};
    const float cardW = BeginTabBody("General");
    if (BeginCard("general-card", cardW)) {
        ToggleRow("Show graticule", "##graticule", &g_showGraticule);
        ImGui::Separator();
        ToggleRow("Resume capture at launch", "##resume", &g_resumeCapture);
        ImGui::Separator();
        ComboRow("Appearance", "##appearance", &g_appearance, appearanceItems, 3, Scaled(150.0f));
        ImGui::Separator();
        ComboRow("Display", "##display", &g_display, displayItems, 3, Scaled(230.0f));
    }
    EndCard();
    EndTabBody();
}

void DrawScopeCard(const char* name, ScopeState* state, const char* labelA, const char* const* itemsA, int countA,
                   const char* labelB, const char* const* itemsB, int countB, float cardW)
{
    SectionHeader(name);
    ImGui::PushID(name);
    if (BeginCard("card", cardW)) {
        SliderRow("Intensity", "##intensity", &state->intensity, Scaled(220.0f));
        ImGui::Separator();
        SliderRow("Detail", "##detail", &state->detail, Scaled(220.0f));
        ImGui::Separator();
        SliderRow("Smoothing", "##smoothing", &state->smoothing, Scaled(220.0f));
        ImGui::Separator();
        ComboRow(labelA, "##styleA", &state->styleA, itemsA, countA, Scaled(180.0f));
        if (labelB != nullptr) {
            ImGui::Separator();
            ComboRow(labelB, "##styleB", &state->styleB, itemsB, countB, Scaled(180.0f));
        }
    }
    EndCard();
    ImGui::PopID();
}

void DrawScopesTab()
{
    static const char* const matrixItems[] = {"BT.601", "BT.709"};
    static const char* const traceItems[] = {"Boosted", "Linear"};
    static const char* const waveItems[] = {"RGB", "Luma", "Colored Luma"};
    static const char* const histItems[] = {"Combined", "Per Channel"};
    const float cardW = BeginTabBody("Scopes");
    DrawScopeCard("Vectorscope", &g_vectorscope, "Matrix", matrixItems, 2, "Trace Response", traceItems, 2, cardW);
    DrawScopeCard("Waveform", &g_waveform, "Waveform Style", waveItems, 3, nullptr, nullptr, 0, cardW);
    DrawScopeCard("Histogram", &g_histogram, "Histogram Style", histItems, 2, nullptr, nullptr, 0, cardW);
    EndTabBody();
}

// Returns the pressed A-Z / 0-9 key as a character this frame, or 0 for none.
char CapturedKey()
{
    for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) {
            return static_cast<char>('A' + (k - ImGuiKey_A));
        }
    }
    for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) {
            return static_cast<char>('0' + (k - ImGuiKey_0));
        }
    }

    return 0;
}

// Advances the recorder state machine while a row is listening for a key.
void UpdateRecorder()
{
    if (g_recordingRow < 0) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_recordingRow = -1;
        g_conflictRow = -1;
        g_pendingKey = 0;

        return;
    }
    const char key = CapturedKey();
    if (key == 0) {
        return;
    }
    int other = -1;
    for (int i = 0; i < g_shortcutCount; ++i) {
        if (i != g_recordingRow && g_shortcuts[i].key == key) {
            other = i;
        }
    }
    if (other < 0) {
        g_shortcuts[g_recordingRow].key = key;
        g_recordingRow = -1;
        g_conflictRow = -1;
        g_pendingKey = 0;

        return;
    }
    g_pendingKey = key;
    g_conflictRow = other;
}

// One key cell: a keycap that starts recording on click, or the live prompt.
void KeyCell(int row)
{
    const bool recording = g_recordingRow == row;
    const float w = Scaled(120.0f);
    const float h = ImGui::GetFrameHeight();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w));
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(row);
    const bool clicked = ImGui::InvisibleButton("key", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q = ImVec2(p.x + w, p.y + h);
    const ImU32 bg = recording ? Col(ImVec4(g_pal.accent.x, g_pal.accent.y, g_pal.accent.z, 0.16f))
                               : Col(hovered ? g_pal.fieldHover : g_pal.fieldBg);
    dl->AddRectFilled(p, q, bg, Scaled(6.0f));
    dl->AddRect(p, q, recording ? Col(g_pal.accent) : Col(g_pal.separator), Scaled(6.0f), 0, Scaled(1.0f));
    char label[24];
    if (recording) {
        std::snprintf(label, sizeof(label), "press a key...");
    } else {
        std::snprintf(label, sizeof(label), "%c", g_shortcuts[row].key);
    }
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImU32 textCol = recording ? Col(g_pal.accent) : Col(g_pal.text);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), textCol, label);
    if (clicked) {
        if (recording) {
            g_recordingRow = -1;
            g_conflictRow = -1;
            g_pendingKey = 0;
        } else {
            g_recordingRow = row;
            g_conflictRow = -1;
            g_pendingKey = 0;
        }
    }
}

void DrawShortcutsTab()
{
    const float cardW = BeginTabBody("Shortcuts");
    UpdateRecorder();
    if (BeginCard("shortcuts-card", cardW)) {
        if (ImGui::BeginTable("shortcuts", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, Scaled(130.0f));
            for (int i = 0; i < g_shortcutCount; ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(g_shortcuts[i].action);
                ImGui::TableSetColumnIndex(1);
                KeyCell(i);
            }
            ImGui::EndTable();
        }
    }
    EndCard();

    // The inline conflict line and its Swap action, plus a recording hint.
    if (g_recordingRow >= 0 && g_conflictRow >= 0) {
        ImGui::Dummy(ImVec2(0.0f, Scaled(4.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text, g_pal.conflict);
        ImGui::Text("%c is already used by %s.", g_pendingKey, g_shortcuts[g_conflictRow].action);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Swap")) {
            const char previous = g_shortcuts[g_recordingRow].key;
            g_shortcuts[g_recordingRow].key = g_pendingKey;
            g_shortcuts[g_conflictRow].key = previous;
            g_recordingRow = -1;
            g_conflictRow = -1;
            g_pendingKey = 0;
        }
    } else if (g_recordingRow >= 0) {
        ImGui::Dummy(ImVec2(0.0f, Scaled(4.0f)));
        ImGui::TextDisabled("Press a letter or number, or Esc to cancel.");
    }
    EndTabBody();
}

void DrawAboutTab()
{
    const float cardW = BeginTabBody("About");
    if (BeginCard("about-card", cardW)) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float tile = Scaled(52.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawNavIcon(dl, p, tile, 1, Col(g_pal.accent));
        ImGui::Dummy(ImVec2(tile, tile));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(g_font, Scaled(19.0f));
        ImGui::TextUnformatted("SideScopes");
        ImGui::PopFont();
        ImGui::TextDisabled("Version 0.2.0");
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0.0f, Scaled(6.0f)));
        ImGui::TextWrapped("Real-time vectorscope, waveform and histogram for a selected screen region.");
        ImGui::Dummy(ImVec2(0.0f, Scaled(2.0f)));
        ImGui::TextDisabled("GPL-3.0-or-later");
    }
    EndCard();
    EndTabBody();
}

// --- shell ---------------------------------------------------------------

void DrawSidebar()
{
    ImGui::Dummy(ImVec2(0.0f, Scaled(6.0f)));

    // App header: an icon tile and the product name.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float tile = Scaled(26.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawNavIcon(dl, p, tile, 1, Col(g_pal.accent));
        const float nameSize = Scaled(15.0f);
        dl->AddText(g_font, nameSize, ImVec2(p.x + tile + Scaled(9.0f), p.y + (tile - nameSize) * 0.5f),
                    Col(g_pal.text), "SideScopes");
        ImGui::Dummy(ImVec2(0.0f, tile));
    }

    ImGui::Dummy(ImVec2(0.0f, Scaled(12.0f)));

    if (NavItem("General", 0, Rgb(142, 142, 147), g_tab == 0)) {
        g_tab = 0;
    }
    if (NavItem("Scopes", 1, Rgb(10, 132, 255), g_tab == 1)) {
        g_tab = 1;
    }
    if (NavItem("Shortcuts", 2, Rgb(94, 92, 230), g_tab == 2)) {
        g_tab = 2;
    }
    if (NavItem("About", 3, Rgb(120, 120, 128), g_tab == 3)) {
        g_tab = 3;
    }
}

void DrawContent()
{
    // A hairline at the sidebar/content boundary, fixed against scrolling.
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddLine(wp, ImVec2(wp.x, wp.y + ws.y), Col(g_pal.separator), Scaled(1.0f));

    switch (g_tab) {
    case 0:
        DrawGeneralTab();
        break;
    case 1:
        DrawScopesTab();
        break;
    case 2:
        DrawShortcutsTab();
        break;
    default:
        DrawAboutTab();
        break;
    }
}

void DrawSettingsWindow()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##host", nullptr, flags);
    ImGui::PopStyleVar(3);

    const float footerH = Scaled(30.0f);
    const float sidebarW = Scaled(196.0f);

    ImGui::BeginChild("body", ImVec2(0.0f, -footerH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_pal.sidebarBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Scaled(10.0f), Scaled(10.0f)));
    ImGui::BeginChild("sidebar", ImVec2(sidebarW, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    DrawSidebar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 0.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_pal.contentBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("content", ImVec2(0.0f, 0.0f));
    DrawContent();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::EndChild();  // body

    // Footer bar with a top hairline and the muted version string.
    {
        const ImVec2 fp = ImGui::GetCursorScreenPos();
        const float fw = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(fp, ImVec2(fp.x + fw, fp.y + footerH), Col(g_pal.contentBg));
        dl->AddLine(fp, ImVec2(fp.x + fw, fp.y), Col(g_pal.separator), Scaled(1.0f));
        const float th = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(fp.x + Scaled(20.0f), fp.y + (footerH - th) * 0.5f), Col(g_pal.textMuted),
                    "SideScopes 0.2.0");
    }

    ImGui::End();
}

// --- setup ---------------------------------------------------------------

const char* const* InterfaceFontFiles()
{
#if defined(__APPLE__)
    static const char* const files[] = {"/System/Library/Fonts/HelveticaNeue.ttc", "/System/Library/Fonts/SFNS.ttf",
                                        "/System/Library/Fonts/Supplemental/Arial.ttf", nullptr};
#elif defined(_WIN32)
    static const char* const files[] = {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf", nullptr};
#else
    static const char* const files[] = {nullptr};
#endif

    return files;
}

// Loads the system UI font at native size for the given display scales. The
// glyph texture is rasterized at the framebuffer density so text stays crisp
// on Retina and high-DPI monitors; the nominal size carries the content
// scale so the physical size matches native controls on both platforms.
void LoadFont(float contentScale, float framebufferScale)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config;
    config.RasterizerDensity = framebufferScale;
    const float sizePixels = 13.0f * (contentScale / framebufferScale);
    for (const char* const* path = InterfaceFontFiles(); *path != nullptr; ++path) {
        g_font = io.Fonts->AddFontFromFileTTF(*path, sizePixels, &config);
        if (g_font != nullptr) {
            break;
        }
    }
    if (g_font == nullptr) {
        g_font = io.Fonts->AddFontDefault();
    }
}

void GlfwErrorCallback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

}  // namespace

int main()
{
    glfwSetErrorCallback(GlfwErrorCallback);
    if (glfwInit() == GLFW_FALSE) {
        std::fprintf(stderr, "Failed to initialize GLFW.\n");

        return 1;
    }

#if defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    const char* glslVersion = "#version 130";
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(720, 520, "Settings", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "Failed to create window.\n");
        glfwTerminate();

        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    float contentScaleX = 1.0f;
    float contentScaleY = 1.0f;
    glfwGetWindowContentScale(window, &contentScaleX, &contentScaleY);
    int windowW = 0;
    int windowH = 0;
    int framebufferW = 0;
    int framebufferH = 0;
    glfwGetWindowSize(window, &windowW, &windowH);
    glfwGetFramebufferSize(window, &framebufferW, &framebufferH);
    const float framebufferScale = windowW > 0 ? framebufferW / static_cast<float>(windowW) : 1.0f;
    const float contentScale = contentScaleX > 0.0f ? contentScaleX : 1.0f;
    g_uiScale = framebufferScale > 0.0f ? contentScale / framebufferScale : 1.0f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ApplyMetrics(g_uiScale);
    ApplyPalette(ResolveDark(g_appearance) ? DarkPalette() : LightPalette());
    LoadFont(contentScale, framebufferScale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    int lastAppearance = g_appearance;
    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
            glfwWaitEventsTimeout(0.1);

            continue;
        }

        if (g_appearance != lastAppearance) {
            ApplyPalette(ResolveDark(g_appearance) ? DarkPalette() : LightPalette());
            lastAppearance = g_appearance;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        DrawSettingsWindow();
        ImGui::Render();

        int displayW = 0;
        int displayH = 0;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(g_pal.contentBg.x, g_pal.contentBg.y, g_pal.contentBg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
