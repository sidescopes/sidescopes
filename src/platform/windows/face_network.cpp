#include "platform/windows/face_network.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <utility>

#include "platform/windows/face_model_data.h"
#include "platform/windows/face_network_geometry.h"

namespace sidescopes {
namespace {

constexpr float ScoreThreshold = 0.8f;
constexpr float OverlapThreshold = 0.3f;
constexpr int MaximumProposals = 5000;
constexpr std::size_t MaximumFaces = 8;
constexpr std::array<int, 3> FeatureStrides{8, 16, 32};

struct Prediction
{
    cv::Rect2f bounds;
    float score = 0.0f;
};

// YuNet predicts one classification score, object score and four box values
// per grid cell at each stride. Landmarks are not needed to select a region.
constexpr std::size_t OutputCount = 9;

const float* tensorValues(const cv::Mat& tensor, std::size_t expectedValues)
{
    if (tensor.type() != CV_32FC1 || !tensor.isContinuous() || tensor.total() != expectedValues) {
        throw std::runtime_error("Unexpected face model tensor shape");
    }
    return tensor.ptr<float>();
}

Prediction predictionAt(const float* offsets, int column, int row, int stride, float score)
{
    const float step = static_cast<float>(stride);
    const float centerX = (static_cast<float>(column) + offsets[0]) * step;
    const float centerY = (static_cast<float>(row) + offsets[1]) * step;
    const float width = static_cast<float>(std::exp(static_cast<double>(offsets[2])) * stride);
    const float height = static_cast<float>(std::exp(static_cast<double>(offsets[3])) * stride);
    const cv::Rect2f bounds{centerX - width * 0.5f, centerY - height * 0.5f, width, height};
    if (!face_network::validNmsBounds(bounds.x, bounds.y, bounds.width, bounds.height)) {
        throw std::runtime_error("Invalid face model geometry");
    }
    return {bounds, score};
}

std::vector<Prediction> decodePredictions(const std::vector<cv::Mat>& tensors, cv::Size paddedSize)
{
    if (tensors.size() != OutputCount) {
        throw std::runtime_error("Unexpected face model output count");
    }
    std::vector<Prediction> predictions;
    for (std::size_t level = 0; level < FeatureStrides.size(); ++level) {
        const int stride = FeatureStrides[level];
        const int columns = paddedSize.width / stride;
        const int cells = columns * (paddedSize.height / stride);
        const float* classifications = tensorValues(tensors[level], static_cast<std::size_t>(cells));
        const float* objects = tensorValues(tensors[level + 3], static_cast<std::size_t>(cells));
        const float* boxes = tensorValues(tensors[level + 6], static_cast<std::size_t>(cells) * 4);
        for (int cell = 0; cell < cells; ++cell) {
            if (!std::isfinite(classifications[cell]) || !std::isfinite(objects[cell])) {
                throw std::runtime_error("Invalid face model score");
            }
            const float score =
                std::sqrt(std::clamp(classifications[cell], 0.0f, 1.0f) * std::clamp(objects[cell], 0.0f, 1.0f));
            if (score >= ScoreThreshold) {
                predictions.push_back(predictionAt(boxes + static_cast<std::size_t>(cell) * 4, cell % columns,
                                                   cell / columns, stride, score));
            }
        }
    }
    return predictions;
}

std::vector<cv::Rect2f> suppressOverlaps(const std::vector<Prediction>& predictions)
{
    if (predictions.empty()) {
        return {};
    }
    if (predictions.size() == 1) {
        return {predictions.front().bounds};
    }
    std::vector<cv::Rect> rectangles;
    std::vector<float> scores;
    rectangles.reserve(predictions.size());
    scores.reserve(predictions.size());
    for (const Prediction& prediction : predictions) {
        const cv::Rect2f& bounds = prediction.bounds;
        // Truncation here is part of YuNet's suppression contract. Preserve
        // the original float coordinates for mapping back to the capture.
        rectangles.emplace_back(static_cast<int>(bounds.x), static_cast<int>(bounds.y), static_cast<int>(bounds.width),
                                static_cast<int>(bounds.height));
        scores.push_back(prediction.score);
    }
    std::vector<int> selected;
    cv::dnn::NMSBoxes(rectangles, scores, ScoreThreshold, OverlapThreshold, selected, 1.0f, MaximumProposals);
    std::vector<cv::Rect2f> faces;
    faces.reserve(selected.size());
    for (int index : selected) {
        faces.push_back(predictions.at(static_cast<std::size_t>(index)).bounds);
    }
    return faces;
}

std::vector<IntRect> sourceRectangles(const std::vector<cv::Rect2f>& boxes, const FrameView& frame, cv::Size inputSize,
                                      double minimumPixels)
{
    const double scaleX = static_cast<double>(frame.width) / inputSize.width;
    const double scaleY = static_cast<double>(frame.height) / inputSize.height;
    std::vector<IntRect> faces;
    for (const cv::Rect2f& box : boxes) {
        const double width = box.width * scaleX;
        const double height = box.height * scaleY;
        if (width < minimumPixels || height < minimumPixels) {
            continue;
        }
        const double left = std::clamp(box.x * scaleX, 0.0, static_cast<double>(frame.width));
        const double top = std::clamp(box.y * scaleY, 0.0, static_cast<double>(frame.height));
        const double right =
            std::clamp((box.x + static_cast<double>(box.width)) * scaleX, 0.0, static_cast<double>(frame.width));
        const double bottom =
            std::clamp((box.y + static_cast<double>(box.height)) * scaleY, 0.0, static_cast<double>(frame.height));
        const int x = static_cast<int>(std::lround(left));
        const int y = static_cast<int>(std::lround(top));
        const IntRect face{x, y, static_cast<int>(std::lround(right)) - x, static_cast<int>(std::lround(bottom)) - y};
        if (!face.empty()) {
            faces.push_back(face);
        }
    }
    std::stable_sort(faces.begin(), faces.end(), [](const IntRect& first, const IntRect& second) {
        return static_cast<int64_t>(first.width) * first.height > static_cast<int64_t>(second.width) * second.height;
    });
    if (faces.size() > MaximumFaces) {
        faces.resize(MaximumFaces);
    }
    return faces;
}

}  // namespace

struct FaceNetwork::Implementation
{
    cv::dnn::Net network;
    const std::vector<cv::String> outputNames{"cls_8",  "cls_16", "cls_32",  "obj_8",  "obj_16",
                                              "obj_32", "bbox_8", "bbox_16", "bbox_32"};
    cv::Mat fullBgr;
    cv::Mat input;
    cv::Mat padded;
    cv::Mat blob;
};

FaceNetwork::FaceNetwork()
    : m_implementation(std::make_unique<Implementation>())
{
    // Face detection is the only OpenCV consumer. Configure its global pool
    // once before any session loads a Net; application analysis threads keep
    // their own scheduling. OpenCV provides no per-Net CPU thread-count API.
    static std::once_flag configured;
    std::call_once(configured, [] { cv::setNumThreads(1); });
    const auto model = faceModelBytes();
    m_implementation->network = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(model.data()), model.size());
    if (m_implementation->network.empty()) {
        throw std::runtime_error("Cannot load the embedded face model");
    }
    m_implementation->network.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    m_implementation->network.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
}

FaceNetwork::~FaceNetwork() = default;

std::vector<IntRect> FaceNetwork::detect(const FrameView& frame, double minimumPixels, int maximumInputEdge)
{
    auto& state = *m_implementation;
    const double scale = std::min(1.0, static_cast<double>(maximumInputEdge) / std::max(frame.width, frame.height));
    const cv::Size inputSize{std::max(1, static_cast<int>(std::lround(frame.width * scale))),
                             std::max(1, static_cast<int>(std::lround(frame.height * scale)))};
    // Eight-bit rows can be wrapped without copying; deeper rows need the
    // same rounded conversion used by other consumers of captured pixels.
    std::vector<uint8_t> converted;
    const uint8_t* pixels = frame.pixels;
    std::size_t stride = static_cast<std::size_t>(frame.strideBytes);
    if (frame.format == PixelFormat::Argb2101010) {
        converted = copyAsBgra8(frame);
        if (converted.empty()) {
            throw std::runtime_error("Cannot convert the face detection frame");
        }
        pixels = converted.data();
        stride = static_cast<std::size_t>(frame.width) * 4;
    }
    const cv::Mat bgra(frame.height, frame.width, CV_8UC4, const_cast<uint8_t*>(pixels), stride);
    cv::cvtColor(bgra, state.fullBgr, cv::COLOR_BGRA2BGR);
    cv::resize(state.fullBgr, state.input, inputSize, 0.0, 0.0, cv::INTER_AREA);
    const int bottom = face_network::paddedInputEdge(inputSize.height) - inputSize.height;
    const int right = face_network::paddedInputEdge(inputSize.width) - inputSize.width;
    cv::copyMakeBorder(state.input, state.padded, 0, bottom, 0, right, cv::BORDER_CONSTANT, 0);
    cv::dnn::blobFromImage(state.padded, state.blob);
    state.network.setInput(state.blob);
    std::vector<cv::Mat> outputs;
    state.network.forward(outputs, state.outputNames);
    return sourceRectangles(suppressOverlaps(decodePredictions(outputs, state.padded.size())), frame, inputSize,
                            minimumPixels);
}

}  // namespace sidescopes
