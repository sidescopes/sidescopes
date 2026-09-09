#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <limits>
#include <thread>
#include <vector>

#include "platform/face_detection.h"

namespace sidescopes {

TEST_CASE("Native face capability becomes available without a detection", "[native][face-detection]")
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!supportsFaceDetection() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    REQUIRE(supportsFaceDetection());
}

TEST_CASE("One-shot face detection rejects malformed input", "[native][face-detection]")
{
    CHECK(detectFaces({}, 1.0f).status == FaceDetectionStatus::Failed);
    constexpr int Edge = 128;
    const std::vector<uint8_t> pixels(std::size_t{Edge} * Edge * 4, 0);
    FrameView frame{pixels.data(), Edge * 4, Edge, Edge};
    for (const float density :
         {-1.0f, 0.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        CHECK(detectFaces(frame, density).status == FaceDetectionStatus::Failed);
    }
    frame.strideBytes = Edge * 4 - 1;
    CHECK(detectFaces(frame, 1.0f).status == FaceDetectionStatus::Failed);
    frame.strideBytes = -1;
    CHECK(detectFaces(frame, 1.0f).status == FaceDetectionStatus::Failed);
    frame.strideBytes = Edge * 4;
    frame.format = static_cast<PixelFormat>(-1);
    CHECK(detectFaces(frame, 1.0f).status == FaceDetectionStatus::Failed);
}

TEST_CASE("One-shot face detection completes repeatedly with padded and deep frames", "[native][face-detection]")
{
    constexpr int Width = 128;
    constexpr int Height = 96;
    constexpr int Stride = Width * 4 + 16;
    for (const PixelFormat format : {PixelFormat::Bgra8, PixelFormat::Argb2101010, PixelFormat::Bgra8}) {
        for (const uint8_t level : {uint8_t{0}, uint8_t{255}}) {
            CAPTURE(format, level);
            std::vector<uint8_t> pixels(std::size_t{Stride} * Height, 0xA5);
            for (int row = 0; row < Height; ++row) {
                std::fill_n(pixels.data() + std::size_t{Stride} * row, Width * 4, level);
            }
            const auto original = pixels;
            FrameView frame{pixels.data(), Stride, Width, Height};
            frame.format = format;
            const auto result = detectFaces(frame, 1.0f);
            REQUIRE(result.status == FaceDetectionStatus::Completed);
            CHECK(result.faces.empty());
            CHECK(pixels == original);
        }
    }
}

}  // namespace sidescopes
