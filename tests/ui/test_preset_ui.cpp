// Headless asserts on the preset picker's popup: the REAL picker, controller
// and rows - not a copy of them - driven through the Test Engine. The list
// closes when a slot is loaded and stays open while a name is being edited;
// those are choices this code makes (a Selectable flag and a rename field),
// so they get tests against the code that ships.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/icon_textures.h"
#include "app/layout_preset_picker.h"
#include "app/layout_presets.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/analysis_worker.h"
#include "imgui.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "modules/module_registry.h"
#include "platform/graphics.h"
#include "ui_test_harness.h"

namespace sidescopes {

/// The icon cache asks for pixels the null backend will never render, so the
/// real rasterizer - which lives in the platform layer this suite excludes -
/// is stood in for by a transparent buffer of the right size.
std::vector<uint8_t> rasterizeIcon(Icon, int sizePixels)
{
    return std::vector<uint8_t>(static_cast<std::size_t>(sizePixels) * static_cast<std::size_t>(sizePixels) * 4U);
}

namespace {

/// A texture that satisfies the interface and draws nothing, which on the
/// null backend is exactly what every texture does: draw data is never
/// rendered, so the id is never dereferenced.
class NullScopeTexture : public ScopeTexture
{
public:
    NullScopeTexture(int width, int height)
        : m_width(width),
          m_height(height)
    {
    }

    void upload(const ScopeImage&) override
    {
    }

    [[nodiscard]] ImTextureID textureId() const override
    {
        return ImTextureID{};
    }

    [[nodiscard]] int width() const override
    {
        return m_width;
    }

    [[nodiscard]] int height() const override
    {
        return m_height;
    }

private:
    int m_width;
    int m_height;
};

/// The backend the icon cache is given on the headless run. It exists so the
/// suite can drive the real picker - whose rows rasterize real icons - without
/// a device; nothing here is ever asked to present.
class NullGraphics : public GraphicsBackend
{
public:
    void setWindowHints() override
    {
    }

    [[nodiscard]] bool init(GLFWwindow*) override
    {
        return true;
    }

    void shutdown() override
    {
    }

    std::unique_ptr<ScopeTexture> createScopeTexture(int width, int height) override
    {
        return std::make_unique<NullScopeTexture>(width, height);
    }

    [[nodiscard]] bool beginFrame(int, int) override
    {
        return true;
    }

    void endFrame() override
    {
    }

    [[nodiscard]] void* nativeWindowHandle() const override
    {
        return nullptr;
    }
};

/// The picker exactly as the toolbar owns it, plus what the popup reported
/// this frame. A function-local static keeps it reachable from the engine's
/// captureless GuiFunc and TestFunc.
struct PresetHarness
{
    NullGraphics graphics;
    IconTextures icons{graphics};
    ScopeRegistry registry{builtinModules()};
    ScopeView view{registry};
    AnalysisSettings analysis;
    LayoutPresetController controller{view, registry, analysis};
    LayoutPresetPicker picker{controller};
    /// Whether the slot list stood open on the frame just drawn.
    bool popupOpen = false;
};

PresetHarness& harness()
{
    static PresetHarness instance;

    return instance;
}

void presetBarGui(ImGuiTestContext*)
{
    PresetHarness& h = harness();
    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_Always);
    ImGui::Begin("PresetBar", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    (void)h.picker.draw(h.icons);
    h.popupOpen = ImGui::IsPopupOpen("##preset-popup");
    ImGui::End();
}

/// Opens the slot list from the toolbar button and proves it opened.
void openList(ImGuiTestContext* ctx)
{
    ctx->SetRef("PresetBar");
    ctx->ItemClick("##preset-picker");
    ctx->Yield(2);
    IM_CHECK(harness().popupOpen);
    ctx->SetRef("//$FOCUSED");
}

/// SYMPTOM IF BROKEN: loading a preset leaves the list standing until it is
/// clicked away. A row's click does one thing - it loads - and the gesture is
/// then over; only renaming keeps the list, because the field lives in it.
/// The rows carried the scope selector's keep-open flag, whose list is a set
/// of checkboxes a user toggles several of; a preset load is terminal.
void loadingASlotClosesTheList(ImGuiTestContext* ctx)
{
    PresetHarness& h = harness();
    openList(ctx);

    const std::string row = "**/" + presetDisplayName(3, h.controller.at(3));
    ctx->ItemClick(row.c_str());
    ctx->Yield(2);

    IM_CHECK_EQ(h.controller.activeSlot(), 3);
    IM_CHECK(!h.popupOpen);
}

/// SYMPTOM IF BROKEN: clicking a row's pen closes the list before the field
/// it opens can be typed into - renaming becomes impossible with the mouse.
void renamingKeepsTheListOpen(ImGuiTestContext* ctx)
{
    PresetHarness& h = harness();
    openList(ctx);

    ctx->ItemClick("**/##rename");
    ctx->Yield(2);

    IM_CHECK(h.popupOpen);
}

void registerPresetTests(ImGuiTestEngine* engine)
{
    ImGuiTest* closes = IM_REGISTER_TEST(engine, "preset", "loading_a_slot_closes_the_list");
    closes->GuiFunc = presetBarGui;
    closes->TestFunc = loadingASlotClosesTheList;

    ImGuiTest* rename = IM_REGISTER_TEST(engine, "preset", "renaming_keeps_the_list_open");
    rename->GuiFunc = presetBarGui;
    rename->TestFunc = renamingKeepsTheListOpen;
}

}  // namespace
}  // namespace sidescopes

int main()
{
    using namespace sidescopes;

    return uitest::runSuite("preset", registerPresetTests, /*expectedTests=*/2);
}
