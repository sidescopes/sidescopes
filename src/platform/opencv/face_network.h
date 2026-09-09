#pragma once

#include <memory>
#include <vector>

#include "core/frame.h"

namespace sidescopes {

/// An offline YuNet model and its reusable input storage, owned by one caller.
class FaceNetwork
{
public:
    FaceNetwork();
    ~FaceNetwork();
    FaceNetwork(const FaceNetwork&) = delete;
    FaceNetwork& operator=(const FaceNetwork&) = delete;

    /// Returns bounded face boxes in source pixels. Throws on invalid native
    /// output or an allocation failure; the caller handles detection failures.
    [[nodiscard]] std::vector<IntRect> detect(const FrameView& frame, double minimumPixels, int maximumInputEdge);

private:
    struct Implementation;
    std::unique_ptr<Implementation> m_implementation;
};

}  // namespace sidescopes
