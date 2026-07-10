#include "core/region_hash.h"

#include <algorithm>
#include <cstring>

namespace sidescopes {
namespace {

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

}  // namespace

uint64_t HashRegion(const FrameView& frame, IntRect region, IntRect masked) {
    region = region.ClampedTo(frame.width, frame.height);

    uint64_t hash = kFnvOffsetBasis;
    const auto hash_span = [&](int x0, int x1, int py) {
        if (x1 <= x0) return;
        const uint8_t* cursor = frame.PixelAt(x0, py);
        const uint8_t* end = frame.PixelAt(x1, py);
        for (; cursor + 8 <= end; cursor += 8) {
            uint64_t chunk = 0;
            std::memcpy(&chunk, cursor, sizeof(chunk));
            hash = (hash ^ chunk) * kFnvPrime;
        }
    };

    const bool has_mask = !masked.Empty();
    for (int py = region.y; py < region.y + region.height; py += 4) {
        if (has_mask && py >= masked.y && py < masked.y + masked.height) {
            hash_span(region.x, std::min(region.x + region.width, masked.x), py);
            hash_span(std::max(region.x, masked.x + masked.width), region.x + region.width, py);
        } else {
            hash_span(region.x, region.x + region.width, py);
        }
    }
    return hash;
}

}  // namespace sidescopes
