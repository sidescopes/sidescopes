#include "core/photo_region_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sidescopes {
namespace {

// The frame is scored on a coarse grid: photo content is "rich" (varied),
// editor chrome is "flat". Block statistics are cheap and robust; pixel
// precision comes later from edge refinement.
constexpr int kBlockSize = 16;

// A block is rich when its luma varies enough or it holds enough distinct
// coarse colors. Both thresholds are deliberately permissive: missing part
// of a photo hurts more than including a busy toolbar block, because the
// rectangle step wants solid photo cores.
constexpr double kVarianceThreshold = 40.0;  // luma variance
constexpr int kDistinctColorThreshold = 12;  // of max 512 coarse buckets

struct BlockGrid {
    int columns = 0;
    int rows = 0;
    std::vector<uint8_t> rich;  // 1 = photo-like

    [[nodiscard]] bool At(int cx, int cy) const {
        return rich[static_cast<std::size_t>(cy) * columns + cx] != 0;
    }
};

BlockGrid ScoreBlocks(const FrameView& frame) {
    BlockGrid grid;
    grid.columns = std::max(1, frame.width / kBlockSize);
    grid.rows = std::max(1, frame.height / kBlockSize);
    grid.rich.assign(static_cast<std::size_t>(grid.columns) * grid.rows, 0);

    std::vector<uint8_t> color_seen(512);
    for (int cy = 0; cy < grid.rows; ++cy) {
        for (int cx = 0; cx < grid.columns; ++cx) {
            double sum = 0.0;
            double sum_squared = 0.0;
            std::fill(color_seen.begin(), color_seen.end(), uint8_t{0});
            int distinct_colors = 0;

            const int y0 = cy * kBlockSize;
            const int x0 = cx * kBlockSize;
            for (int py = y0; py < y0 + kBlockSize; ++py) {
                const uint8_t* pixel = frame.PixelAt(x0, py);
                for (int px = 0; px < kBlockSize; ++px, pixel += 4) {
                    const int b = pixel[0], g = pixel[1], r = pixel[2];
                    const double luma = (54 * r + 183 * g + 19 * b) / 256.0;
                    sum += luma;
                    sum_squared += luma * luma;
                    // 3 bits per channel: 512 coarse color buckets.
                    const int bucket = ((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5);
                    if (!color_seen[bucket]) {
                        color_seen[bucket] = 1;
                        ++distinct_colors;
                    }
                }
            }
            constexpr double kSamples = kBlockSize * kBlockSize;
            const double mean = sum / kSamples;
            const double variance = sum_squared / kSamples - mean * mean;
            if (variance >= kVarianceThreshold || distinct_colors >= kDistinctColorThreshold) {
                grid.rich[static_cast<std::size_t>(cy) * grid.columns + cx] = 1;
            }
        }
    }
    return grid;
}

// Largest all-rich rectangle in the grid (histogram-of-heights method).
struct GridRect {
    int cx = 0, cy = 0, columns = 0, rows = 0;
    [[nodiscard]] int Area() const { return columns * rows; }
};

GridRect LargestRichRectangle(const BlockGrid& grid) {
    GridRect best;
    std::vector<int> heights(static_cast<std::size_t>(grid.columns), 0);
    std::vector<int> stack;
    for (int cy = 0; cy < grid.rows; ++cy) {
        for (int cx = 0; cx < grid.columns; ++cx)
            heights[cx] = grid.At(cx, cy) ? heights[cx] + 1 : 0;

        stack.clear();
        for (int cx = 0; cx <= grid.columns; ++cx) {
            const int height = cx < grid.columns ? heights[cx] : 0;
            while (!stack.empty() && heights[stack.back()] >= height) {
                const int top_height = heights[stack.back()];
                stack.pop_back();
                const int left = stack.empty() ? 0 : stack.back() + 1;
                const int width = cx - left;
                if (top_height * width > best.Area()) {
                    best = GridRect{left, cy - top_height + 1, width, top_height};
                }
            }
            stack.push_back(cx);
        }
    }
    return best;
}

// A row or column segment is photo-like when its luma varies along the run.
bool SegmentIsRich(const FrameView& frame, int x0, int x1, int y0, int y1) {
    double sum = 0.0;
    double sum_squared = 0.0;
    int samples = 0;
    for (int py = y0; py < y1; ++py) {
        const uint8_t* pixel = frame.PixelAt(x0, py);
        for (int px = x0; px < x1; ++px, pixel += 4) {
            const double luma = (54 * pixel[2] + 183 * pixel[1] + 19 * pixel[0]) / 256.0;
            sum += luma;
            sum_squared += luma * luma;
            ++samples;
        }
    }
    if (samples == 0) return false;
    const double mean = sum / samples;
    return (sum_squared / samples - mean * mean) >= kVarianceThreshold;
}

// Refines each rectangle edge to the exact canvas boundary, one pixel line
// at a time. Block scoring is 16-pixel coarse and edge blocks are mixed, so
// each side first SHRINKS while the rect's own boundary line is flat (chrome
// captured by a partial block), then GROWS while the line just outside is
// still photo content.
IntRect RefineEdges(const FrameView& frame, IntRect rect) {
    const int limit = kBlockSize;
    const auto top_line_rich = [&] {
        return SegmentIsRich(frame, rect.x, rect.x + rect.width, rect.y, rect.y + 1);
    };
    const auto bottom_line_rich = [&] {
        return SegmentIsRich(frame, rect.x, rect.x + rect.width, rect.y + rect.height - 1,
                             rect.y + rect.height);
    };
    const auto left_line_rich = [&] {
        return SegmentIsRich(frame, rect.x, rect.x + 1, rect.y, rect.y + rect.height);
    };
    const auto right_line_rich = [&] {
        return SegmentIsRich(frame, rect.x + rect.width - 1, rect.x + rect.width, rect.y,
                             rect.y + rect.height);
    };

    for (int step = 0; step < limit && rect.height > limit && !top_line_rich(); ++step) {
        ++rect.y;
        --rect.height;
    }
    for (int step = 0; step < limit && rect.y > 0; ++step) {
        if (!SegmentIsRich(frame, rect.x, rect.x + rect.width, rect.y - 1, rect.y)) break;
        --rect.y;
        ++rect.height;
    }
    for (int step = 0; step < limit && rect.height > limit && !bottom_line_rich(); ++step) {
        --rect.height;
    }
    for (int step = 0; step < limit && rect.y + rect.height < frame.height; ++step) {
        if (!SegmentIsRich(frame, rect.x, rect.x + rect.width, rect.y + rect.height,
                           rect.y + rect.height + 1))
            break;
        ++rect.height;
    }
    for (int step = 0; step < limit && rect.width > limit && !left_line_rich(); ++step) {
        ++rect.x;
        --rect.width;
    }
    for (int step = 0; step < limit && rect.x > 0; ++step) {
        if (!SegmentIsRich(frame, rect.x - 1, rect.x, rect.y, rect.y + rect.height)) break;
        --rect.x;
        ++rect.width;
    }
    for (int step = 0; step < limit && rect.width > limit && !right_line_rich(); ++step) {
        --rect.width;
    }
    for (int step = 0; step < limit && rect.x + rect.width < frame.width; ++step) {
        if (!SegmentIsRich(frame, rect.x + rect.width, rect.x + rect.width + 1, rect.y,
                           rect.y + rect.height))
            break;
        ++rect.width;
    }
    return rect;
}

// Flatness of the chrome immediately outside the rectangle, 0..1.
float BorderFlatness(const FrameView& frame, const IntRect& rect) {
    int flat = 0;
    int total = 0;
    const auto check = [&](int x0, int x1, int y0, int y1) {
        if (x0 < 0 || y0 < 0 || x1 > frame.width || y1 > frame.height || x0 >= x1 || y0 >= y1)
            return;
        ++total;
        if (!SegmentIsRich(frame, x0, x1, y0, y1)) ++flat;
    };
    constexpr int kProbe = 4;
    check(rect.x, rect.x + rect.width, rect.y - kProbe, rect.y);
    check(rect.x, rect.x + rect.width, rect.y + rect.height, rect.y + rect.height + kProbe);
    check(rect.x - kProbe, rect.x, rect.y, rect.y + rect.height);
    check(rect.x + rect.width, rect.x + rect.width + kProbe, rect.y, rect.y + rect.height);
    // A rect flush with the frame edge has fewer probes; that is fine.
    return total == 0 ? 1.0f : static_cast<float>(flat) / static_cast<float>(total);
}

}  // namespace

std::vector<RegionCandidate> DetectPhotoRegions(const FrameView& frame, int max_candidates) {
    std::vector<RegionCandidate> candidates;
    if (frame.width < 2 * kBlockSize || frame.height < 2 * kBlockSize) return candidates;

    BlockGrid grid = ScoreBlocks(frame);
    const int minimum_blocks =
        std::max(4, grid.columns * grid.rows / 100);  // ignore specks under ~1% of the frame

    for (int round = 0; round < max_candidates; ++round) {
        const GridRect core = LargestRichRectangle(grid);
        if (core.Area() < minimum_blocks) break;

        // Interior richness before this round's blocks are consumed.
        int rich_inside = 0;
        for (int cy = core.cy; cy < core.cy + core.rows; ++cy)
            for (int cx = core.cx; cx < core.cx + core.columns; ++cx)
                rich_inside += grid.At(cx, cy) ? 1 : 0;
        const float interior = static_cast<float>(rich_inside) / static_cast<float>(core.Area());

        // Consume the found blocks so the next round finds the next region.
        for (int cy = core.cy; cy < core.cy + core.rows; ++cy)
            for (int cx = core.cx; cx < core.cx + core.columns; ++cx)
                grid.rich[static_cast<std::size_t>(cy) * grid.columns + cx] = 0;

        IntRect rect{core.cx * kBlockSize, core.cy * kBlockSize, core.columns * kBlockSize,
                     core.rows * kBlockSize};
        rect = RefineEdges(frame, rect);

        RegionCandidate candidate;
        candidate.rect = rect;
        candidate.confidence = interior * BorderFlatness(frame, rect);
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const RegionCandidate& a, const RegionCandidate& b) {
                  return a.confidence * static_cast<float>(a.rect.width) *
                             static_cast<float>(a.rect.height) >
                         b.confidence * static_cast<float>(b.rect.width) *
                             static_cast<float>(b.rect.height);
              });
    return candidates;
}

}  // namespace sidescopes
