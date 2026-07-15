#include "core/region_hash.h"

#include <algorithm>
#include <cstring>

namespace sidescopes {
namespace {

constexpr uint64_t FnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t FnvPrime = 1099511628211ull;

}  // namespace

uint64_t hashRegion(const FrameView& frame, IntRect region, IntRect masked)
{
    region = region.clampedTo(frame.width, frame.height);

    uint64_t hash = FnvOffsetBasis;
    const auto hashSpan = [&](int x0, int x1, int py) {
        if (x1 <= x0) {
            return;
        }
        const uint8_t* cursor = frame.pixelAt(x0, py);
        const uint8_t* end = frame.pixelAt(x1, py);
        for (; cursor + 8 <= end; cursor += 8) {
            uint64_t chunk = 0;
            std::memcpy(&chunk, cursor, sizeof(chunk));
            hash = (hash ^ chunk) * FnvPrime;
        }
    };

    const bool hasMask = !masked.empty();
    for (int py = region.y; py < region.y + region.height; py += 4) {
        if (hasMask && py >= masked.y && py < masked.y + masked.height) {
            hashSpan(region.x, std::min(region.x + region.width, masked.x), py);
            hashSpan(std::max(region.x, masked.x + masked.width), region.x + region.width, py);
        } else {
            hashSpan(region.x, region.x + region.width, py);
        }
    }
    return hash;
}

}  // namespace sidescopes
