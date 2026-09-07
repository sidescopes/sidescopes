#pragma once

#include <span>

namespace sidescopes {

/// The verified YuNet ONNX model embedded by the build, with static lifetime.
[[nodiscard]] std::span<const unsigned char> faceModelBytes();

}  // namespace sidescopes
