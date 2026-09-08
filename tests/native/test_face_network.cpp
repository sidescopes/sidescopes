#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <opencv2/dnn.hpp>
#include <vector>

#include "platform/windows/face_model_data.h"
#include "platform/windows/face_network.h"
#include "platform/windows/face_network_geometry.h"

namespace sidescopes {
namespace {

std::vector<uint8_t> texturedPixels(int width, int height, int stride, PixelFormat format)
{
    std::vector<uint8_t> pixels(static_cast<std::size_t>(height) * stride, 0xA5);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::array<uint8_t, 3> channels{static_cast<uint8_t>((x * 3 + y * 5) % 256),
                                                  static_cast<uint8_t>((x * 7 + y) % 256),
                                                  static_cast<uint8_t>((x + y * 11) % 256)};
            const auto offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4;
            uint32_t packed = 0;
            if (format == PixelFormat::Argb2101010) {
                packed = 3u << 30;
                for (std::size_t channel = 0; channel < channels.size(); ++channel) {
                    const uint32_t value = (static_cast<uint32_t>(channels[channel]) * 1023u + 127u) / 255u;
                    packed |= value << (channel * 10);
                }
            } else {
                packed = static_cast<uint32_t>(channels[0]) | static_cast<uint32_t>(channels[1]) << 8 |
                         static_cast<uint32_t>(channels[2]) << 16 | 255u << 24;
            }
            for (std::size_t byte = 0; byte < 4; ++byte) {
                pixels[offset + byte] = static_cast<uint8_t>(packed >> (byte * 8));
            }
        }
    }
    return pixels;
}

void checkBounded(const std::vector<IntRect>& faces, const FrameView& frame)
{
    for (const auto& face : faces) {
        CHECK_FALSE(face.empty());
        CHECK(face == face.clampedTo(frame.width, frame.height));
    }
}

}  // namespace

// These procedural frames exercise the real importer and inference path on
// every supported build host; they are not a face-recognition accuracy corpus.
TEST_CASE("The embedded face model runs at both input caps", "[face-network]")
{
    FaceNetwork network;
    for (const uint8_t level : {uint8_t{0}, uint8_t{127}, uint8_t{255}}) {
        const std::vector<uint8_t> pixels(std::size_t{640} * 480 * 4, level);
        const FrameView frame{pixels.data(), 640 * 4, 640, 480};
        for (const int cap : {320, 1280}) {
            CAPTURE(level, cap);
            CHECK(network.detect(frame, 24, cap).empty());
        }
    }
}

TEST_CASE("A face network reuses storage across input shapes", "[face-network]")
{
    FaceNetwork network;
    const auto anchorPixels = texturedPixels(320, 240, 320 * 4, PixelFormat::Bgra8);
    const FrameView anchor{anchorPixels.data(), 320 * 4, 320, 240};
    const auto expected = network.detect(anchor, 24, 320);
    const std::array<IntRect, 14> shapes{{{0, 0, 1, 1},
                                          {0, 0, 31, 33},
                                          {0, 0, 32, 32},
                                          {0, 0, 33, 31},
                                          {0, 0, 319, 321},
                                          {0, 0, 321, 319},
                                          {0, 0, 17, 641},
                                          {0, 0, 641, 17},
                                          {0, 0, 32, 641},
                                          {0, 0, 641, 32},
                                          {0, 0, 33, 641},
                                          {0, 0, 641, 33},
                                          {0, 0, 1281, 721},
                                          {0, 0, 3840, 2160}}};
    for (const auto& shape : shapes) {
        const auto pixels = texturedPixels(shape.width, shape.height, shape.width * 4, PixelFormat::Bgra8);
        const FrameView frame{pixels.data(), shape.width * 4, shape.width, shape.height};
        for (const int cap : {320, 1280}) {
            CAPTURE(shape.width, shape.height, cap);
            const auto faces = network.detect(frame, 24, cap);
            checkBounded(faces, frame);
            CHECK(network.detect(frame, 24, cap) == faces);
        }
    }
    CHECK(network.detect(anchor, 24, 320) == expected);
}

TEST_CASE("Face input padding avoids singleton convolution shapes", "[face-network]")
{
    const auto model = faceModelBytes();
    const auto network = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(model.data()), model.size());
    // Check the actual model through OpenCV's public shape API, so a model
    // upgrade cannot silently invalidate the input padding requirement.
    for (const int edge : {1, 31, 32, 33, 63, 64, 65}) {
        CAPTURE(edge);
        const int padded = face_network::paddedInputEdge(edge);
        const cv::dnn::MatShape input{1, 3, padded, padded};
        std::vector<int> ids;
        std::vector<std::vector<cv::dnn::MatShape>> inputs, outputs;
        network.getLayersShapes(input, ids, inputs, outputs);
        REQUIRE(inputs.size() == ids.size());
        std::size_t convolutions = 0;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (network.getLayer(ids[i])->type != "Convolution") {
                continue;
            }
            ++convolutions;
            REQUIRE_FALSE(inputs[i].empty());
            for (const auto& shape : inputs[i]) {
                REQUIRE(shape.size() == 4);
                CHECK(shape[2] >= 2);
                CHECK(shape[3] >= 2);
            }
        }
        REQUIRE(convolutions > 0);
    }
}

TEST_CASE("Face inference preserves padded and ten-bit input", "[face-network]")
{
    FaceNetwork network;
    constexpr int Width = 321;
    constexpr int Height = 239;
    constexpr int Stride = Width * 4 + 28;
    const auto packed = texturedPixels(Width, Height, Width * 4, PixelFormat::Bgra8);
    const FrameView reference{packed.data(), Width * 4, Width, Height};
    for (const auto format : {PixelFormat::Bgra8, PixelFormat::Argb2101010}) {
        auto pixels = texturedPixels(Width, Height, Stride, format);
        const auto original = pixels;
        FrameView frame{pixels.data(), Stride, Width, Height};
        frame.format = format;
        REQUIRE(copyAsBgra8(frame) == packed);
        for (const int cap : {320, 1280}) {
            CAPTURE(format, cap);
            CHECK(network.detect(frame, 24, cap) == network.detect(reference, 24, cap));
            CHECK(pixels == original);
        }
    }
}

}  // namespace sidescopes
