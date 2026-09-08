#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/dnn/all_layers.hpp>
#include <span>
#include <vector>

namespace sidescopes {
namespace {

constexpr int Channels = 2;
constexpr std::size_t Guard = 256;
constexpr std::array<float, 9> Kernel{1, -2, 3, -4, 5, -6, 7, -8, 9};
constexpr std::array<float, Channels> Bias{5, -7};

float weight(int channel, int ky, int kx)
{
    return Kernel[static_cast<std::size_t>(ky) * 3 + kx] * (channel == 0 ? 1.0f : -1.0f);
}

float sample(int channel, int y, int x, int phase)
{
    return static_cast<float>(channel * 23 + (x * 3 + y * 5 + phase * 7) % 17 - 8);
}

float expected(int channel, int y, int x, int height, int width, int phase, bool relu)
{
    // Small integer operands make the independent zero-padding sum exact in float.
    float value = Bias[static_cast<std::size_t>(channel)];
    for (int ky = 0; ky < 3; ++ky) {
        for (int kx = 0; kx < 3; ++kx) {
            const int iy = y + ky - 1;
            const int ix = x + kx - 1;
            if (iy >= 0 && iy < height && ix >= 0 && ix < width) {
                value += sample(channel, iy, ix, phase) * weight(channel, ky, kx);
            }
        }
    }
    return relu ? std::max(0.0f, value) : value;
}

cv::Ptr<cv::dnn::BaseConvolutionLayer> createLayer(bool relu)
{
    cv::dnn::LayerParams params;
    params.set("kernel_size", 3);
    params.set("stride", 1);
    params.set("pad", 1);
    params.set("dilation", 1);
    params.set("group", Channels);
    params.set("num_output", Channels);
    params.set("bias_term", true);
    const std::array<int, 4> dims{Channels, 1, 3, 3};
    cv::Mat weights(4, dims.data(), CV_32F);
    for (int channel = 0; channel < Channels; ++channel) {
        for (int ky = 0; ky < 3; ++ky) {
            for (int kx = 0; kx < 3; ++kx) {
                weights.ptr<float>()[channel * 9 + ky * 3 + kx] = weight(channel, ky, kx);
            }
        }
    }
    cv::Mat bias(1, Channels, CV_32F);
    std::copy(Bias.begin(), Bias.end(), bias.ptr<float>());
    params.blobs = {weights, bias};
    auto layer = cv::dnn::ConvolutionLayer::create(params);
    REQUIRE(static_cast<bool>(layer));
    layer->preferableTarget = cv::dnn::DNN_TARGET_CPU;
    if (relu) {
        cv::dnn::LayerParams activation;
        activation.set("negative_slope", 0.0f);
        REQUIRE(layer->setActivation(cv::dnn::ReLULayer::create(activation)));
    }
    return layer;
}

void checkShape(const cv::Ptr<cv::dnn::BaseConvolutionLayer>& layer, int width, int height, bool relu)
{
    const std::size_t count = static_cast<std::size_t>(Channels) * width * height;
    // Nonzero surrounding storage exposes reads outside the logical input,
    // including padding errors that cross into the adjacent channel.
    std::vector<float> source(count + 2 * Guard, 4096.0f);
    std::vector<float> destination(count + 2 * Guard, -2048.0f);
    const std::array<int, 4> dims{1, Channels, height, width};
    std::vector<cv::Mat> inputs{cv::Mat(4, dims.data(), CV_32F, source.data() + Guard)};
    std::vector<cv::Mat> outputs{cv::Mat(4, dims.data(), CV_32F, destination.data() + Guard)};
    std::vector<cv::Mat> internals;
    std::vector<cv::dnn::MatShape> outputShapes, internalShapes;
    const cv::dnn::MatShape shape(dims.begin(), dims.end());
    layer->getMemoryShapes({shape}, 1, outputShapes, internalShapes);
    REQUIRE(outputShapes == std::vector<cv::dnn::MatShape>{shape});
    REQUIRE(internalShapes.empty());
    layer->finalize(cv::_InputArray(inputs), cv::_OutputArray(outputs));
    for (int phase = 0; phase < 2; ++phase) {
        CAPTURE(phase);
        for (int channel = 0; channel < Channels; ++channel) {
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const auto offset = (static_cast<std::size_t>(channel) * height + y) * width + x;
                    source[Guard + offset] = sample(channel, y, x, phase);
                }
            }
        }
        const auto before = source;
        layer->forward(cv::_InputArray(inputs), cv::_OutputArray(outputs), cv::_OutputArray(internals));
        CHECK(source == before);
        REQUIRE(outputs.front().data == reinterpret_cast<unsigned char*>(destination.data() + Guard));
        const auto untouched = [](float value) { return value == -2048.0f; };
        CHECK(std::ranges::all_of(std::span(destination).first(Guard), untouched));
        CHECK(std::ranges::all_of(std::span(destination).last(Guard), untouched));
        for (int channel = 0; channel < Channels; ++channel) {
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    CAPTURE(channel, x, y);
                    const auto offset = (static_cast<std::size_t>(channel) * height + y) * width + x;
                    const float actual = destination[Guard + offset];
                    CHECK(std::isfinite(actual));
                    CHECK(actual == expected(channel, y, x, height, width, phase, relu));
                }
            }
        }
    }
}

}  // namespace

TEST_CASE("Depthwise convolution preserves singleton padding", "[face-network][face-convolution]")
{
    cv::setNumThreads(1);
    const std::array<cv::Size, 6> shapes{{{1, 1}, {21, 1}, {1, 21}, {2, 2}, {23, 21}, {21, 23}}};
    for (const bool relu : {false, true}) {
        auto layer = createLayer(relu);
        for (int direction = 0; direction < 2; ++direction) {
            for (std::size_t i = 0; i < shapes.size(); ++i) {
                const auto shape = shapes[direction == 0 ? i : shapes.size() - i - 1];
                CAPTURE(relu, direction, shape.width, shape.height);
                checkShape(layer, shape.width, shape.height, relu);
            }
        }
    }
}

}  // namespace sidescopes
