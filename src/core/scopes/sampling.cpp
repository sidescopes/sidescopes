#include "core/scopes/sampling.h"

#include <algorithm>

namespace sidescopes {
namespace {

// The same 1..8 range the sampling-stride parameter offers, so the automatic
// thinning can never take a region past what a user could ask for by hand.
constexpr int MaximumStride = 8;

int countAt(int extent, int stride)
{
    return extent <= 0 ? 0 : (extent + stride - 1) / stride;
}

}  // namespace

SampleGrid sampleGridFor(int requestedStride, IntRect region, long long budget)
{
    const int columnStride = std::clamp(requestedStride, 1, MaximumStride);
    if (region.empty()) {
        return SampleGrid{columnStride, columnStride, 0, 0};
    }

    const int columnsPerRow = countAt(region.width, columnStride);
    int rowStride = columnStride;
    // Hoisted out of the loop condition: written inline, the two comparisons
    // around the && read to clang-format as template brackets, and it formats
    // them as such.
    const auto overBudget = [&]() {
        return static_cast<long long>(countAt(region.height, rowStride)) * columnsPerRow > budget;
    };
    while (budget > 0 && rowStride < MaximumStride && overBudget()) {
        ++rowStride;
    }

    return SampleGrid{rowStride, columnStride, countAt(region.height, rowStride), columnsPerRow};
}

}  // namespace sidescopes
