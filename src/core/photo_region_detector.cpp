#include "core/photo_region_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace sidescopes {
namespace {

// A pixel boundary is an edge when the compared pixels differ this much in
// luma or in any channel; gentle gradients inside photographs do not clear
// it. Contrast is per pixel pair and does not scale with density.
// Antialiasing halves adjacent differences by spreading a step across
// pixels, which is why edges are also measured across a wide baseline (see
// BoundaryRow).
constexpr int kLumaEdgeThreshold = 14;
constexpr int kChannelEdgeThreshold = 24;

// Geometric tolerances in points, scaled to pixels per frame density.

// Edge runs with fewer visible edge pixels than this cannot border a
// photo-sized rectangle; this also discards the clutter of text lines and
// small controls.
constexpr int kMinimumRunLengthPoints = 96;

// Gaps this small inside a run are bridged: content crossing the boundary
// line, and stretches where the two sides genuinely match in tone - a gray
// pavement meeting a gray canvas drops out for up to ~25 points at a time
// while the border plainly continues. Side verification keeps bridged runs
// honest.
constexpr int kRunGapTolerancePoints = 28;

// A bridged gap at least this wide may also be a real gutter between two
// separate rectangles (side-by-side photos in a compare view), so the
// segments around it are offered for pairing alongside the bridged whole.
constexpr int kRunSplitPoints = 10;

// Two runs bound the same rectangle when their ends align within this many
// points.
constexpr int kAlignmentTolerancePoints = 10;

// Run ends locate a vertical side only roughly - rounded window corners
// (about 12 points on macOS) leave no contrast along their arc - so the
// true side column is searched for within this distance of the ends.
constexpr int kSideSearchRadiusPoints = 20;

constexpr int kMinimumRectWidthPoints = 96;
constexpr int kMinimumRectHeightPoints = 72;

// Candidates whose edges coincide within this distance are the same
// rectangle reported twice.
constexpr int kDuplicateTolerancePoints = 8;

// A run is a rectangle border only when it is the extremity of an edge band:
// the row boundary directly above or below it must be mostly quiet. Busy
// content (a photograph's texture) is edges on every row; its runs are
// sandwiched between other edge rows and are rejected, while a border always
// faces the calm interior or the calm surroundings on one side.
constexpr float kQuietNeighborFraction = 0.3f;

// A rectangle qualifies when this fraction of the visible part of its left
// and right sides lies on real vertical edges (the top and bottom are
// covered by construction).
constexpr float kMinimumSideCoverage = 0.6f;

// The point-based tolerances above, resolved to pixels.
struct Tolerances {
    int run_length;
    int run_gap;
    int run_split;
    int alignment;
    int side_radius;
    int rect_width;
    int rect_height;
    int duplicate;

    explicit Tolerances(float pixels_per_point) {
        const auto scaled = [pixels_per_point](int points) {
            return std::max(
                1, static_cast<int>(std::lround(static_cast<float>(points) * pixels_per_point)));
        };
        run_length = scaled(kMinimumRunLengthPoints);
        run_gap = scaled(kRunGapTolerancePoints);
        run_split = scaled(kRunSplitPoints);
        alignment = scaled(kAlignmentTolerancePoints);
        side_radius = scaled(kSideSearchRadiusPoints);
        rect_width = scaled(kMinimumRectWidthPoints);
        rect_height = scaled(kMinimumRectHeightPoints);
        duplicate = scaled(kDuplicateTolerancePoints);
    }
};

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
// sums. Edges are counted twice, over two baselines: adjacent rows, and a
// three-row span (y-1 against y+2). Antialiasing spreads a boundary step
// across pixels, halving every adjacent difference - a low-contrast border
// (a photograph on a barely different gray canvas) vanishes from the
// adjacent baseline but keeps its full step across the wide one. Runs and
// side coverage accept either baseline; the quiet tests use the adjacent
// one only, because the wide baseline smears a border's own step into its
// neighbor boundaries and would flunk every border's extremity check.
struct BoundaryRow {
    std::vector<int> edge_prefix;
    std::vector<int> direct_edge_prefix;
    std::vector<int> visible_prefix;

    [[nodiscard]] bool Empty() const { return edge_prefix.empty(); }
    [[nodiscard]] int EdgesIn(int x0, int x1) const {
        return edge_prefix[static_cast<std::size_t>(x1)] -
               edge_prefix[static_cast<std::size_t>(x0)];
    }
    [[nodiscard]] int DirectEdgesIn(int x0, int x1) const {
        return direct_edge_prefix[static_cast<std::size_t>(x1)] -
               direct_edge_prefix[static_cast<std::size_t>(x0)];
    }
    [[nodiscard]] int VisibleIn(int x0, int x1) const {
        return visible_prefix[static_cast<std::size_t>(x1)] -
               visible_prefix[static_cast<std::size_t>(x0)];
    }
};

void ComputeBoundaryRow(const FrameView& frame, const std::vector<IntRect>& masked_regions, int y,
                        BoundaryRow& row) {
    row.edge_prefix.resize(static_cast<std::size_t>(frame.width) + 1);
    row.direct_edge_prefix.resize(static_cast<std::size_t>(frame.width) + 1);
    row.visible_prefix.resize(static_cast<std::size_t>(frame.width) + 1);
    row.edge_prefix[0] = 0;
    row.direct_edge_prefix[0] = 0;
    row.visible_prefix[0] = 0;
    const int wide_top = std::max(0, y - 1);
    const int wide_bottom = std::min(frame.height - 1, y + 2);
    for (int x = 0; x < frame.width; ++x) {
        const bool masked = Masked(masked_regions, x, y) || Masked(masked_regions, x, y + 1);
        const bool direct = !masked && IsEdge(frame.PixelAt(x, y), frame.PixelAt(x, y + 1));
        const bool wide = !masked && (direct || IsEdge(frame.PixelAt(x, wide_top),
                                                       frame.PixelAt(x, wide_bottom)));
        row.edge_prefix[static_cast<std::size_t>(x) + 1] = row.edge_prefix[x] + (wide ? 1 : 0);
        row.direct_edge_prefix[static_cast<std::size_t>(x) + 1] =
            row.direct_edge_prefix[x] + (direct ? 1 : 0);
        row.visible_prefix[static_cast<std::size_t>(x) + 1] =
            row.visible_prefix[x] + (masked ? 0 : 1);
    }
}

// A span is quiet when its visible pixels are mostly not edges (adjacent
// baseline). A boundary outside the frame or entirely masked proves nothing
// and counts as quiet.
bool QuietSpan(const BoundaryRow& row, int x0, int x1) {
    if (row.Empty()) return true;
    const int visible = row.VisibleIn(x0, x1);
    if (visible == 0) return true;
    return static_cast<float>(row.DirectEdgesIn(x0, x1)) <
           kQuietNeighborFraction * static_cast<float>(visible);
}

// Edge runs along one boundary. Masked pixels extend a run without counting
// toward its required visible-edge length, so a border passing beneath an
// occluding window survives, while content wholly inside the mask does not.
// A gap wide enough to be a gutter splits the run as well as bridging it:
// the whole and its segments all reach pairing, so side-by-side photos are
// found individually and as their union, and the picker's hover resolves
// the ambiguity.
void AppendRunsFromBoundary(const BoundaryRow& row, const Tolerances& tolerances, int y, int width,
                            std::vector<EdgeRun>& runs) {
    int run_start = -1;
    int segment_start = -1;
    int visible_edges = 0;
    int gap = 0;
    bool split = false;
    const auto close_segment = [&](int end) {
        if (split && segment_start >= 0 && row.EdgesIn(segment_start, end) >= tolerances.run_length)
            runs.push_back(EdgeRun{y, segment_start, end});
        segment_start = -1;
    };
    const auto close_run = [&](int end) {
        close_segment(end);
        if (run_start >= 0 && visible_edges >= tolerances.run_length)
            runs.push_back(EdgeRun{y, run_start, end});
        run_start = -1;
        visible_edges = 0;
        gap = 0;
        split = false;
    };
    for (int x = 0; x < width; ++x) {
        const bool masked = row.VisibleIn(x, x + 1) == 0;
        const bool edge = row.EdgesIn(x, x + 1) != 0;
        if (edge) {
            if (run_start < 0) run_start = x;
            if (segment_start < 0) segment_start = x;
            if (gap > tolerances.run_split && segment_start >= 0 && segment_start < x - gap) {
                // The gap behind us was gutter-sized: emit the segment
                // before it and start a fresh one here.
                split = true;
                close_segment(x - gap + 1);
                segment_start = x;
            }
            ++visible_edges;
            gap = 0;
        } else if (masked) {
            gap = 0;  // proves nothing; bridge
        } else if (run_start >= 0 && ++gap > tolerances.run_gap) {
            close_run(x - gap + 1);
        }
    }
    close_run(width);
}

// Border runs: edge runs with a quiet row boundary directly above or below.
std::vector<EdgeRun> HorizontalBorderRuns(const FrameView& frame,
                                          const std::vector<IntRect>& masked_regions,
                                          const Tolerances& tolerances) {
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
        AppendRunsFromBoundary(current, tolerances, y, frame.width, row_runs);
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
// The wide baseline mirrors the row logic: it sees antialiased low-contrast
// sides at full strength. The quiet checks pass wide=false for the adjacent
// baseline only.
std::optional<float> VerticalSideCoverage(const FrameView& frame,
                                          const std::vector<IntRect>& masked_regions, int x, int y0,
                                          int y1, bool wide) {
    if (x <= 0 || x >= frame.width || y1 <= y0) return std::nullopt;
    const int wide_left = std::max(0, x - 2);
    const int wide_right = std::min(frame.width - 1, x + 1);
    int edges = 0;
    int visible = 0;
    for (int y = y0; y < y1; ++y) {
        if (Masked(masked_regions, x - 1, y) || Masked(masked_regions, x, y)) continue;
        ++visible;
        if (IsEdge(frame.PixelAt(x - 1, y), frame.PixelAt(x, y)) ||
            (wide && IsEdge(frame.PixelAt(wide_left, y), frame.PixelAt(wide_right, y))))
            ++edges;
    }
    if (visible < std::max(4, (y1 - y0) / 8)) return std::nullopt;
    return static_cast<float>(edges) / static_cast<float>(visible);
}

// The run ends only suggest where a side is; the actual side is the nearest
// column around them whose visible span is covered by vertical edges. The
// mirror of the quiet-extremity rule for rows disambiguates it from busy
// content: immediately outside a real side the desktop is calm, while a
// column inside a photograph's texture is flanked by edges on both sides.
// `outside` is -1 for a left side, +1 for a right side.
struct SideMatch {
    int x = 0;
    float coverage = 0.0f;
};

std::optional<SideMatch> FindSide(const FrameView& frame,
                                  const std::vector<IntRect>& masked_regions,
                                  const Tolerances& tolerances, int guess, int outside, int y0,
                                  int y1) {
    for (int distance = 0; distance <= tolerances.side_radius; ++distance) {
        for (const int x : {guess - distance, guess + distance}) {
            const auto coverage =
                VerticalSideCoverage(frame, masked_regions, x, y0, y1, /*wide=*/true);
            if (coverage && *coverage >= kMinimumSideCoverage) {
                // The adjacent baseline judges the outside: the wide one
                // would smear the side's own step into its neighbor columns.
                const auto beyond = VerticalSideCoverage(frame, masked_regions, x + outside, y0, y1,
                                                         /*wide=*/false);
                if (!beyond || *beyond < kQuietNeighborFraction) return SideMatch{x, *coverage};
            }
            if (distance == 0) break;
        }
    }
    return std::nullopt;
}

bool SameRectangle(const IntRect& a, const IntRect& b, int tolerance) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.x + a.width - (b.x + b.width)) <= tolerance &&
           std::abs(a.y + a.height - (b.y + b.height)) <= tolerance;
}

}  // namespace

std::vector<RegionCandidate> DetectPhotoRegions(const FrameView& frame,
                                                const std::vector<IntRect>& masked_regions,
                                                float pixels_per_point, int max_candidates) {
    std::vector<RegionCandidate> candidates;
    const Tolerances tolerances(pixels_per_point);
    if (frame.width < tolerances.rect_width || frame.height < tolerances.rect_height)
        return candidates;

    std::vector<EdgeRun> borders = HorizontalBorderRuns(frame, masked_regions, tolerances);

    // The wide edge baseline reports one border on up to three consecutive
    // boundaries; pairing cost is quadratic in runs, so the band collapses
    // to its first row before assembly (the sides absorb the pixel or two
    // of resulting slack).
    std::vector<EdgeRun> distinct;
    for (const EdgeRun& run : borders) {
        bool banded = false;
        for (auto it = distinct.rbegin(); it != distinct.rend() && run.y - it->y <= 2; ++it) {
            if (std::abs(run.x0 - it->x0) <= tolerances.alignment &&
                std::abs(run.x1 - it->x1) <= tolerances.alignment) {
                banded = true;
                break;
            }
        }
        if (!banded) distinct.push_back(run);
    }
    borders = std::move(distinct);

    // Every pair of x-aligned border runs is a potential top and bottom of a
    // rectangle; the sides must then be real vertical edges.
    for (std::size_t top = 0; top < borders.size(); ++top) {
        for (std::size_t bottom = top + 1; bottom < borders.size(); ++bottom) {
            const EdgeRun& a = borders[top];
            const EdgeRun& b = borders[bottom];
            if (b.y - a.y < tolerances.rect_height) continue;
            const int end_slack = tolerances.alignment + tolerances.side_radius;
            if (std::abs(a.x0 - b.x0) > end_slack || std::abs(a.x1 - b.x1) > end_slack) continue;

            const auto left =
                FindSide(frame, masked_regions, tolerances, std::max(a.x0, b.x0), -1, a.y + 1, b.y);
            if (!left) continue;
            const auto right =
                FindSide(frame, masked_regions, tolerances, std::min(a.x1, b.x1), +1, a.y + 1, b.y);
            if (!right) continue;
            if (right->x - left->x < tolerances.rect_width) continue;

            const IntRect rect{left->x, a.y + 1, right->x - left->x, b.y - a.y};
            const float confidence = (left->coverage + right->coverage) / 2.0f;
            bool duplicate = false;
            for (RegionCandidate& existing : candidates) {
                if (SameRectangle(existing.rect, rect, tolerances.duplicate)) {
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

    // ...but distinct before redundant. Application chrome stacks several
    // horizontal lines above one content area; every line pairs with the
    // same bottom into a near-copy of the whole window, and the lines pair
    // among themselves into slices of it. Under a plain largest-first cap
    // this crowd starves the smaller photograph the user actually wants.
    // A candidate that mostly coincides with one already kept, or that is
    // a contained slice sharing its column or row span, waits until the
    // distinct ones have their slots. A photograph nested in its window is
    // neither: its sides sit well inside the window's.
    std::vector<RegionCandidate> kept;
    std::vector<RegionCandidate> redundant;
    for (const RegionCandidate& candidate : candidates) {
        const auto& c = candidate.rect;
        bool near_copy = false;
        for (const RegionCandidate& existing : kept) {
            const auto& e = existing.rect;
            const int64_t overlap_w = std::min(c.x + c.width, e.x + e.width) - std::max(c.x, e.x);
            const int64_t overlap_h = std::min(c.y + c.height, e.y + e.height) - std::max(c.y, e.y);
            if (overlap_w <= 0 || overlap_h <= 0) continue;
            const int64_t intersection = overlap_w * overlap_h;
            const int64_t area = static_cast<int64_t>(c.width) * c.height;
            const int64_t union_area =
                area + static_cast<int64_t>(e.width) * e.height - intersection;
            if (intersection * 5 > union_area * 4) {  // intersection/union > 0.8
                near_copy = true;
                break;
            }
            const bool contained = intersection * 10 > area * 9;
            const bool same_columns =
                std::abs(c.x - e.x) <= tolerances.duplicate &&
                std::abs(c.x + c.width - (e.x + e.width)) <= tolerances.duplicate;
            const bool same_rows =
                std::abs(c.y - e.y) <= tolerances.duplicate &&
                std::abs(c.y + c.height - (e.y + e.height)) <= tolerances.duplicate;
            if (contained && (same_columns || same_rows)) {
                near_copy = true;
                break;
            }
        }
        (near_copy ? redundant : kept).push_back(candidate);
    }
    kept.insert(kept.end(), redundant.begin(), redundant.end());
    if (static_cast<int>(kept.size()) > max_candidates)
        kept.resize(static_cast<std::size_t>(max_candidates));
    return kept;
}

}  // namespace sidescopes
