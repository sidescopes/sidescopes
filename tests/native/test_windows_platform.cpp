#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "core/diagnostics.h"
#include "platform/face_detection.h"
#include "platform/region_selection.h"
#include "platform/windows/region_border_view.h"
#include "platform/windows/region_picker_view.h"
#include "temp_file.h"

namespace sidescopes {
namespace {

class PickersScope
{
public:
    PickersScope(PickerState& first, PickerState& second)
    {
        g_pickers = {&first, &second};
    }

    ~PickersScope()
    {
        // These states have no windows and belong to the test's stack.
        g_pickers.clear();
    }

    PickersScope(const PickersScope&) = delete;
    PickersScope& operator=(const PickersScope&) = delete;
};

}  // namespace

TEST_CASE("An attached drag owns the preview across displays", "[native]")
{
    PickerState stale;
    stale.displayId = 1;
    stale.width = 1000;
    stale.height = 800;
    stale.facesMode = true;
    stale.hoveredSuggestion = 0;
    stale.suggestions.emplace_back(Gdiplus::RectF(0, 0, 1000, 800), L"Other display");

    PickerState dragged;
    dragged.displayId = 2;
    dragged.width = 1000;
    dragged.height = 800;
    dragged.dragging = true;
    dragged.pickDragging = true;
    dragged.dragStart = {100, 100};
    dragged.dragCurrent = {400, 300};
    dragged.hoveredSuggestion = 0;
    dragged.suggestions.emplace_back(Gdiplus::RectF(0, 0, 1000, 800), L"Attached window");
    const PickersScope pickers(stale, dragged);

    const auto poll = pollRegionPick();
    REQUIRE(poll.active);
    REQUIRE(poll.preview);
    CHECK(poll.displayId == dragged.displayId);
    CHECK(poll.preview->leftPercent == Catch::Approx(10));
    CHECK(poll.preview->topPercent == Catch::Approx(12.5));
    CHECK(poll.preview->rightPercent == Catch::Approx(40));
    CHECK(poll.preview->bottomPercent == Catch::Approx(37.5));
    CHECK(stale.hoveredSuggestion == -1);
}

TEST_CASE("Losing border mouse capture cancels pending interactions", "[native]")
{
    g_border.dragZone = ZoneLeft;
    g_border.closePressed = true;
    g_border.bindingPressed = true;
    g_borderEditing = true;

    borderProc(nullptr, WM_CAPTURECHANGED, 0, 0);

    CHECK(g_border.dragZone == ZoneNone);
    CHECK_FALSE(g_border.closePressed);
    CHECK_FALSE(g_border.bindingPressed);
    CHECK_FALSE(g_borderEditing);
}

TEST_CASE("Face detection survives repeated independent COM apartments", "[native]")
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!supportsFaceDetection() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!supportsFaceDetection()) {
        SKIP("The system face detector is unavailable");
    }

    const test::TempFile log("native-face-detection.log");

    struct RecordingScope
    {
        ~RecordingScope()
        {
            diagConfigure({});
        }
    } recording;

    diagConfigure({"facelock", log.path().string()});

    // Each call activates the real Windows detector on a fresh worker and
    // tears its apartment down. A stale cached factory can fail on re-entry.
    constexpr int Edge = 128;
    const std::vector<uint8_t> pixels(Edge * Edge * 4, 0);
    const FrameView frame{pixels.data(), Edge * 4, Edge, Edge};
    for (int iteration = 0; iteration < 3; ++iteration) {
        CHECK(detectFaces(frame, 1.0f).empty());
    }
    diagConfigure({});
    std::ifstream input(log.path());
    REQUIRE(input.is_open());
    const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::size_t completed = 0;
    std::size_t position = 0;
    while ((position = content.find("face_detection completed", position)) != std::string::npos) {
        ++completed;
        ++position;
    }
    CHECK(completed == 3);
    CHECK(content.find("face_detection failed") == std::string::npos);
}

}  // namespace sidescopes
