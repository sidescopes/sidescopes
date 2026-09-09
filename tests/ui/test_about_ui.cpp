// Drive the shipped About and license reader using only the embedded catalog.
// The test executable has its own directory, with no adjacent notice files.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

#include "app/about_window.h"
#include "app/license_notices.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "platform/desktop.h"
#include "ui_test_harness.h"

namespace sidescopes {
namespace {

AboutWindow g_about;
int g_externalRequests = 0;

void aboutGui(ImGuiTestContext*)
{
    static const VersionInfo version{"0.7.0", false};
    g_about.draw(version);
}

void openReader(ImGuiTestContext* ctx)
{
    g_about = AboutWindow{};
    g_externalRequests = 0;
    g_about.open();
    ctx->Yield(2);
    ctx->WindowMove("//About SideScopes", ImVec2(30.0f, 30.0f));
    ctx->SetRef("About SideScopes");
    ctx->ItemClick("Licenses");
    ctx->Yield(2);
    ctx->SetRef("Licenses");
    IM_CHECK(ctx->ItemExists("Component"));
    IM_CHECK(ctx->ItemExists("Copy text"));
    ctx->WindowMove("//Licenses", ImVec2(380.0f, 40.0f));
}

void selectAndCopy(ImGuiTestContext* ctx, const LicenseNotice& notice)
{
    ctx->SetRef("Licenses");
    const std::string item = std::string("Component/") + notice.name;
    ctx->ComboClick(item.c_str());
    ImGui::SetClipboardText("previous clipboard contents");
    ctx->ItemClick("Copy text");
    const char* copied = ImGui::GetClipboardText();
    IM_CHECK(copied != nullptr);
    IM_CHECK_EQ(std::string_view(copied).size(), notice.text.size());
    IM_CHECK(std::string_view(copied) == notice.text);
}

void everyNoticeCanBeReadAndCopied(ImGuiTestContext* ctx)
{
    openReader(ctx);
    const auto notices = licenseNotices();
    IM_CHECK_EQ(notices.size(), 10U);
    IM_CHECK(std::string_view(notices.front().name) == "SideScopes");
    IM_CHECK_GT(notices.front().text.size(), 30000U);
    IM_CHECK(notices.front().text.find("How to Apply These Terms to Your New Programs") != std::string_view::npos);

    // The full GPL must also remain scrollable; copying must not depend on
    // which part of this long notice is currently visible.
    const ImGuiTestItemInfo reader = ctx->WindowInfo("License text");
    IM_CHECK(reader.Window != nullptr);
    ctx->ScrollToBottom(reader.ID);
    IM_CHECK_GT(reader.Window->Scroll.y, 0.0f);
    for (const LicenseNotice& notice : notices) {
        selectAndCopy(ctx, notice);
    }
    IM_CHECK_EQ(g_externalRequests, 0);
}

void readerSurvivesAboutCloseAndReopens(ImGuiTestContext* ctx)
{
    openReader(ctx);
    const auto notices = licenseNotices();
    IM_CHECK(!notices.empty());
    selectAndCopy(ctx, notices.back());

    ctx->WindowClose("//About SideScopes");
    ctx->Yield(2);
    IM_CHECK(!ctx->ItemExists("//About SideScopes/Licenses"));
    selectAndCopy(ctx, notices.front());

    ctx->WindowClose("//Licenses");
    ctx->Yield(2);
    IM_CHECK(!ctx->ItemExists("//Licenses/Copy text"));
    g_about.open();
    ctx->Yield(2);
    ctx->SetRef("About SideScopes");
    ctx->ItemClick("Licenses");
    ctx->Yield(2);
    selectAndCopy(ctx, notices.back());
    IM_CHECK_EQ(g_externalRequests, 0);
}

void registerAboutTests(ImGuiTestEngine* engine)
{
    ImGuiTest* catalog = IM_REGISTER_TEST(engine, "about", "all_embedded_notices_can_be_read_and_copied");
    catalog->GuiFunc = aboutGui;
    catalog->TestFunc = everyNoticeCanBeReadAndCopied;

    ImGuiTest* lifecycle = IM_REGISTER_TEST(engine, "about", "reader_survives_about_close_and_reopens");
    lifecycle->GuiFunc = aboutGui;
    lifecycle->TestFunc = readerSurvivesAboutCloseAndReopens;
}

}  // namespace

// No browser or native desktop service is available in this headless suite.
void openUrl(const char*)
{
    ++g_externalRequests;
}

}  // namespace sidescopes

int main()
{
    try {
        if (std::filesystem::exists("licenses") || std::filesystem::exists("LICENSE")) {
            std::fprintf(stderr, "About UI suite requires a directory without external license files.\n");
            return 1;
        }
        return sidescopes::uitest::runSuite("about", sidescopes::registerAboutTests, /*expectedTests=*/2);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "About UI suite failed: %s\n", error.what());
        return 1;
    }
}
