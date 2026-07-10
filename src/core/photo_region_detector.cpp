#include "core/photo_region_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace sidescopes {
namespace {

// A pixel boundary is an edge when adjacent pixels differ this much in luma
// or in any channel. Antialiasing spreads a hard boundary over two or three
// pixels, which still clears these thresholds comfortably; gentle gradients
// inside photographs do not.
constexpr int kLumaEdgeThreshold = 14;
constexpr int kChannelEdgeThreshold = 24;

// Edge runs with fewer visible edge pixels than this cannot border a
// photo-sized rectangle; this also discards the clutter of text lines and
// small controls.
constexpr int kMinimumRunLength = 96;

// Gaps this small inside a run are bridged (antialiasing, content crossing
// the boundary line).
constexpr int kRunGapTolerance = 6;

// A run is a rectangle border only when it is the extremity of an edge band:
// the row boundary directly above or below it must be mostly quiet. Busy
// content (a photograph's texture) is edges on every row; its runs are
// sandwiched between other edge rows and are rejected, while a border always
// faces the calm interior or the calm surroundings on one side.
constexpr float kQuietNeighborFraction = 0.3f;

// Two runs bound the same rectangle when their ends align within this many
// pixels.
constexpr int kAlignmentTolerance = 10;

// Run ends locate a vertical side only roughly (corner content may not
// contrast with the surroundings), so the true side column is searched for
// within this distance of the ends.
constexpr int kSideSearchRadius = 20;

constexpr int kMinimumRectWidth = 96;
constexpr int kMinimumRectHeight = 72;

// A rectangle qualifies when this fraction of the visible part of its left
// and right sides lies on real vertical edges (the top and bottom are
// covered by construction).
constexpr float kMinimumSideCoverage = 0.6f;

// Candidates whose edges coincide within this distance are the same
// rectangle reported twice.
constexpr int kDuplicateTolerance = 8;

inline int Luma(const uint8_t* pixel) {
    return (54 * pixel[2] + 183 * pixel[1] + 19 * pixel[0]) >> 8;
}

inline bool IsEdge(const uint8_t* a, const uint8_t* b) {
    if (std::abs(Luma(a) - Luma(b)) >= kLumaEdgeThreshold) return true;
    return std::abs(a[0] - b[0]) >= kChannelEdgeThreshold ||
           std::abs(a[1] - b[1]) >= kChannelEdgeThreshold ||
           std::abs(a[2] - b[2]) >= kChannelEdgeThreshold;
}

inline bool Contains(const IntRect& rect, int x, int y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

bool Masked(const std::vector<IntRect>& masked_regions, int x, int y) {
    for (const IntRect& region : masked_regions) {
        if (Contains(region, x, y)) return true;
    }
    return false;
}

// A maximal horizontal stretch of edge pixels along one row boundary.
struct EdgeRun {
    int y = 0;   // the boundary sits between rows y and y+1
    int x0 = 0;  // inclusive
    int x1 = 0;  // exclusive
};

// Per-column tallies along the boundary between rows y and y+1, as prefix
// sums: edges among visible (unmasked) pixels, and visible pixels themselves.
struct BoundaryRow {
    std::vector<int> edge_prefix;
    std::vector<int> visible_prefix;

    [[nodiscard]] bool Empty() const { return edge_prefix.empty(); }
    [[nodiscard]] int EdgesIn(int x0, int x1) const {
        return edge_prefix[static_cast<std::size_t>(x1)] -
               edge_prefix[static_cast<std::size_t>(x0)];
    }
    [[nodiscard]] int VisibleIn(int x0, int x1) const {
        return visible_prefix[static_cast<std::size_t>(x1)] -
               visible_prefix[static_cast<std::size_t>(x0)];
    }
};

void ComputeBoundaryRow(const FrameView& frame, const std::vector<IntRect>& masked_regions, int y,
                        BoundaryRow& row) {
    row.edge_prefix.resize(static_cast<std::size_t>(frame.width) + 1);
    row.visible_prefix.resize(static_cast<std::size_t>(frame.width) + 1);
    row.edge_prefix[0] = 0;
    row.visible_prefix[0] = 0;
    for (int x = 0; x < frame.width; ++x) {
        const bool masked = Masked(masked_regions, x, y) || Masked(masked_regions, x, y + 1);
        const bool edge = !masked && IsEdge(frame.PixelAt(x, y), frame.PixelAt(x, y + 1));
        row.edge_prefix[static_cast<std::size_t>(x) + 1] = row.edge_prefix[x] + (edge ? 1 : 0);
        row.visible_prefix[static_cast<std::size_t>(x) + 1] =
            row.visible_prefix[x] + (masked ? 0 : 1);
    }
}

// A span is quiet when its visible pixels are mostly not edges. A boundary
// outside the frame or entirely masked proves nothing and counts as quiet.
bool QuietSpan(const BoundaryRow& row, int x0, int x1) {
    if (row.Empty()) return true;
    const int visible = row.VisibleIn(x0, x1);
    if (visible == 0) return true;
    return static_cast<float>(row.EdgesIn(x0, x1)) <
           kQuietNeighborFraction * static_cast<float>(visible);
}

// Edge runs along one boundary. Masked pixels extend a run without counting
// toward its required visible-edge length, so a border passing beneath an
// occluding window survives, while content wholly inside the mask does not.
void AppendRunsFromBoundary(const BoundaryRow& row, int y, int width, std::vector<EdgeRun>& runs) {
    int run_start = -1;
    int visible_edges = 0;
    int gap = 0;
    const auto close_run = [&](int end) {
        if (run_start >= 0 && visible_edges >= kMinimumRunLength)
            runs.push_back(EdgeRun{y, run_start, end});
        run_start = -1;
        visible_edges = 0;
        gap = 0;
    };
    for (int x = 0; x < width; ++x) {
        const bool masked = row.VisibleIn(x, x + 1) == 0;
        const bool edge = row.EdgesIn(x, x + 1) != 0;
        if (edge) {
            if (run_start < 0) run_start = x;
            ++visible_edges;
            gap = 0;
        } else if (masked) {
            gap = 0;  // proves nothing; bridge
        } else if (run_start >= 0 && ++gap > kRunGapTolerance) {
            close_run(x - gap + 1);
        }
    }
    close_run(width);
}

// Border runs: edge runs with a quiet row boundary directly above or below.
std::vector<EdgeRun> HorizontalBorderRuns(const FrameView& frame,
                                          const std::vector<IntRect>& masked_regions) {
    std::vector<EdgeRun> borders;
    // Rolling window of three boundary rows: above (y-1), current (y),
    // below (y+1). An empty row stands for a boundary outside the frame.
    BoundaryRow above;
    BoundaryRow current;
    BoundaryRow below;
    if (frame.height >= 2) ComputeBoundaryRow(frame, masked_regions, 0, current);
    std::vector<EdgeRun> row_runs;
    for (int y = 0; y + 1 < frame.height; ++y) {
        if (y + 2 < frame.height) {
            ComputeBoundaryRow(frame, masked_regions, y + 1, below);
        } else {
            below.edge_prefix.clear();
            below.visible_prefix.clear();
        }
        row_runs.clear();
        AppendRunsFromBoundary(current, y, frame.width, row_runs);
        for (const EdgeRun& run : row_runs) {
            if (QuietSpan(above, run.x0, run.x1) || QuietSpan(below, run.x0, run.x1))
                borders.push_back(run);
        }
        std::swap(above, current);
        std::swap(current, below);
    }
    return borders;
}

// Fraction of the visible span between two rows that lies on a vertical edge
// at column x, or nothing when too little of the side is visible to judge.
std::optional<float> VerticalSideCoverage(const FrameView& frame,
                                          const std::vector<IntRect>& masked_regions, int x, int y0,
                                          int y1) {
    if (x <= 0 || x >= frame.width || y1 <= y0) return std::nullopt;
    int edges = 0;
    int visible = 0;
    for (int y = y0; y < y1; ++y) {
        if (Masked(masked_regions, x - 1, y) || Masked(masked_regions, x, y)) continue;
        ++visible;
        if (IsEdge(frame.PixelAt(x - 1, y), frame.PixelAt(x, y))) ++edges;
    }
    if (visible < std::max(4, (y1 - y0) / 8)) return std::nullopt;
    return static_cast<float>(edges) / static_cast<float>(visible);
}

// The run ends only suggest where a side is; the actual side is the nearest
// column around them whose visible span is covered by vertical edges.
struct SideMatch {
    int x = 0;
    float coverage = 0.0f;
};

std::optional<SideMatch> FindSide(const FrameView& frame,
                                  const std::vector<IntRect>& masked_regions, int guess, int y0,
                                  int y1) {
    for (int distance = 0; distance <= kSideSearchRadius; ++distance) {
        for (const int x : {guess - distance, guess + distance}) {
            const auto coverage = VerticalSideCoverage(frame, masked_regions, x, y0, y1);
            if (coverage && *coverage >= kMinimumSideCoverage) return SideMatch{x, *coverage};
            if (distance == 0) break;
        }
    }
    return std::nullopt;
}

bool SameRectangle(const IntRect& a, const IntRect& b) {
    return std::abs(a.x - b.x) <= kDuplicateTolerance &&
           std::abs(a.y - b.y) <= kDuplicateTolerance &&
           std::abs(a.x + a.width - (b.x + b.width)) <= kDuplicateTolerance &&
           std::abs(a.y + a.height - (b.y + b.height)) <= kDuplicateTolerance;
}

}  // namespace

std::vector<RegionCandidate> DetectPhotoRegions(const FrameView& frame,
                                                const std::vector<IntRect>& masked_regions,
                                                int max_candidates) {
    std::vector<RegionCandidate> candidates;
    if (frame.width < kMinimumRectWidth || frame.height < kMinimumRectHeight) return candidates;

    const std::vector<EdgeRun> borders = HorizontalBorderRuns(frame, masked_regions);

    // Every pair of x-aligned border runs is a potential top and bottom of a
    // rectangle; the sides must then be real vertical edges.
    for (std::size_t top = 0; top < borders.size(); ++top) {
        for (std::size_t bottom = top + 1; bottom < borders.size(); ++bottom) {
            const EdgeRun& a = borders[top];
            const EdgeRun& b = borders[bottom];
            if (b.y - a.y < kMinimumRectHeight) continue;
            if (std::abs(a.x0 - b.x0) > kAlignmentTolerance + kSideSearchRadius ||
                std::abs(a.x1 - b.x1) > kAlignmentTolerance + kSideSearchRadius)
                continue;

            const auto left = FindSide(frame, masked_regions, std::max(a.x0, b.x0), a.y + 1, b.y);
            if (!left) continue;
            const auto right = FindSide(frame, masked_regions, std::min(a.x1, b.x1), a.y + 1, b.y);
            if (!right) continue;
            if (right->x - left->x < kMinimumRectWidth) continue;

            const IntRect rect{left->x, a.y + 1, right->x - left->x, b.y - a.y};
            const float confidence = (left->coverage + right->coverage) / 2.0f;
            bool duplicate = false;
            for (RegionCandidate& existing : candidates) {
                if (SameRectangle(existing.rect, rect)) {
                    existing.confidence = std::max(existing.confidence, confidence);
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) candidates.push_back(RegionCandidate{rect, confidence});
        }
    }

    // Largest first: the picker resolves nesting by hover, and when the list
    // must be cut, small panels lose before windows and photos.
    std::sort(candidates.begin(), candidates.end(),
              [](const RegionCandidate& a, const RegionCandidate& b) {
                  return static_cast<int64_t>(a.rect.width) * a.rect.height >
                         static_cast<int64_t>(b.rect.width) * b.rect.height;
              });
    if (static_cast<int>(candidates.size()) > max_candidates)
        candidates.resize(static_cast<std::size_t>(max_candidates));
    return candidates;
}

}  // namespace sidescopes
