#include "core/region_hash.h"

#include <algorithm>
#include <cstring>

namespace sidescopes {
namespace {

constexpr uint64_t FnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t FnvPrime = 1099511628211ull;

// Folds one row's [x0, x1) pixels into the running hash, eight bytes at a
// time; a span with no width leaves it alone.
//
// The bytes are hashed as they lie, whatever layout they are in: the question
// is only whether this region's pixels changed, and every format packs four
// bytes per pixel. A capture that switches format answers "changed" once,
// which is exactly right - the samples really are different numbers.
uint64_t hashSpan(uint64_t hash, const FrameView& frame, int x0, int x1, int py)
{
    if (x1 <= x0) {
        return hash;
    }
    const uint8_t* cursor = frame.rawPixelAt(x0, py);
    const uint8_t* end = frame.rawPixelAt(x1, py);
    for (; end - cursor >= 8; cursor += 8) {
        uint64_t chunk = 0;
        std::memcpy(&chunk, cursor, sizeof(chunk));
        hash = (hash ^ chunk) * FnvPrime;
    }
    if (cursor != end) {
        uint32_t pixel = 0;
        std::memcpy(&pixel, cursor, sizeof(pixel));
        hash = (hash ^ pixel) * FnvPrime;
    }

    return hash;
}

}  // namespace

uint64_t hashRegion(const FrameView& frame, IntRect region, IntRect masked)
{
    region = region.clampedTo(frame.width, frame.height);
    masked = masked.clampedTo(frame.width, frame.height);

    uint64_t hash = FnvOffsetBasis;
    if (!frame.pixels || region.empty()) {
        return hash;
    }
    for (const int component : {region.width, region.height, static_cast<int>(frame.format)}) {
        hash = (hash ^ static_cast<uint64_t>(component)) * FnvPrime;
    }
    const bool hasMask = !masked.empty();
    for (int64_t row = region.y; row < static_cast<int64_t>(region.y) + region.height; row += 4) {
        const int py = static_cast<int>(row);
        if (hasMask && py >= masked.y && py < masked.y + masked.height) {
            hash = hashSpan(hash, frame, region.x, std::min(region.x + region.width, masked.x), py);
            hash = hashSpan(hash, frame, std::max(region.x, masked.x + masked.width), region.x + region.width, py);
        } else {
            hash = hashSpan(hash, frame, region.x, region.x + region.width, py);
        }
    }
    return hash;
}

}  // namespace sidescopes
