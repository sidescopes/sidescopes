#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "core/diagnostics.h"
#include "platform/face_detection.h"
#include "platform/region_selection.h"
#include "platform/windows/face_network_geometry.h"
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
    g_border.bindingPressed = true;
    g_borderEditing = true;

    borderProc(nullptr, WM_CAPTURECHANGED, 0, 0);

    CHECK(g_border.dragZone == ZoneNone);
    CHECK_FALSE(g_border.bindingPressed);
    CHECK_FALSE(g_borderEditing);
}

TEST_CASE("Decoded face boxes cannot overflow pairwise integer NMS areas", "[native]")
{
    // Each 40000-square box fits an int area by itself, but NMS adds the
    // two areas in int. Reject both before that native overlap operation.
    CHECK_FALSE(face_network::validNmsBounds(0, 0, 40000, 40000));
    CHECK_FALSE(face_network::validNmsBounds(1, 1, 40000, 40000));
    CHECK(face_network::validNmsBounds(0, 0, 32767, 32767));
    CHECK(face_network::validNmsBounds(0, 0, 1280, 1280));
    CHECK(face_network::validNmsBounds(-20, -20, 80, 80));
    CHECK_FALSE(face_network::validNmsBounds(0, 0, 0, 100));
    CHECK_FALSE(face_network::validNmsBounds(0, 0, 100, -1));
    CHECK_FALSE(face_network::validNmsBounds(std::numeric_limits<double>::quiet_NaN(), 0, 100, 100));
    CHECK_FALSE(face_network::validNmsBounds(0, 0, std::numeric_limits<double>::infinity(), 100));
}

TEST_CASE("Face detection survives repeated independent sessions", "[native]")
{
    REQUIRE(supportsFaceDetection());

    const test::TempFile log("native-face-detection.log");

    struct RecordingScope
    {
        ~RecordingScope()
        {
            diagConfigure({});
        }
    } recording;

    diagConfigure({"facelock", log.path().string()});

    // Repeated picker calls own independent models and release their native
    // state. A blank image must complete successfully each time.
    constexpr int Edge = 128;
    const std::vector<uint8_t> pixels(static_cast<std::size_t>(Edge) * Edge * 4, 0);
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

TEST_CASE("Face sessions reject invalid frames and preserve reuse", "[native]")
{
    auto session = createFaceDetectionSession();
    REQUIRE(session);
    CHECK(session->detect({}, 36).status == FaceDetectionStatus::Failed);

    constexpr int Width = 128;
    constexpr int Height = 96;
    constexpr int Stride = Width * 4 + 16;
    const std::vector<uint8_t> pixels(static_cast<std::size_t>(Stride) * Height, 0);
    FrameView frame{pixels.data(), Stride, Width, Height};
    CHECK(session->detect(frame, -1).status == FaceDetectionStatus::Failed);
    CHECK(session->detect(frame, std::numeric_limits<double>::infinity()).status == FaceDetectionStatus::Failed);
    CHECK(session->detect(frame, std::numeric_limits<double>::quiet_NaN()).status == FaceDetectionStatus::Failed);
    frame.strideBytes = Width * 4 - 1;
    CHECK(session->detect(frame, 36).status == FaceDetectionStatus::Failed);
    frame.strideBytes = Stride;
    for (const PixelFormat format : {PixelFormat::Bgra8, PixelFormat::Argb2101010, PixelFormat::Bgra8}) {
        frame.format = format;
        const auto result = session->detect(frame, 36);
        CHECK(result.status == FaceDetectionStatus::Completed);
        CHECK(result.faces.empty());
    }
}

}  // namespace sidescopes
