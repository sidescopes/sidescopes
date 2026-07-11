#include "core/photo_region_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// Run ends locate a vertical side only roughly, and they only ever
// UNDER-extend: a run starts at its first edge pixel, so wherever the
// border tone locally matches the canvas near a corner (or a rounded
// window corner leaves no contrast), the end falls short of the true
// side. The search therefore reaches much farther outward than inward.
constexpr int kSideSearchInwardPoints = 20;
constexpr int kSideSearchOutwardPoints = 56;

constexpr int kMinimumRectWidthPoints = 96;
constexpr int kMinimumRectHeightPoints = 72;

// Candidates whose edges coincide within this distance are the same
// rectangle reported twice.
constexpr int kDuplicateTolerancePoints = 8;

// An occluding window casts a shadow onto the surrounding canvas, so the
// pixels near its frame are unreliable for the same reason the occluded
// ones are. Platform shadows are biased downward: faint above, moderate
// beside, long below.
constexpr int kOccluderHaloAbovePoints = 8;
constexpr int kOccluderHaloBesidePoints = 24;
constexpr int kOccluderHaloBelowPoints = 48;

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

// Diagnostic tracing for corpus triage, gated by an environment variable.
// MSVC deprecates getenv (C4996), so its secure variant answers there.
bool TraceEnabled() {
    static const bool enabled = [] {
#ifdef _MSC_VER
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "SIDESCOPES_DETECTOR_TRACE") != 0 || value == nullptr)
            return false;
        std::free(value);
        return true;
#else
        return std::getenv("SIDESCOPES_DETECTOR_TRACE") != nullptr;
#endif
    }();
    return enabled;
}

// The point-based tolerances above, resolved to pixels.
struct Tolerances {
    int run_length;
    int run_gap;
    int run_split;
    int alignment;
    int side_inward;
    int side_outward;
    int rect_width;
    int rect_height;
    int duplicate;
    int halo_above;
    int halo_beside;
    int halo_below;

    explicit Tolerances(float pixels_per_point) {
        const auto scaled = [pixels_per_point](int points) {
            return std::max(
                1, static_cast<int>(std::lround(static_cast<float>(points) * pixels_per_point)));
        };
        run_length = scaled(kMinimumRunLengthPoints);
        run_gap = scaled(kRunGapTolerancePoints);
        run_split = scaled(kRunSplitPoints);
        alignment = scaled(kAlignmentTolerancePoints);
        side_inward = scaled(kSideSearchInwardPoints);
        side_outward = scaled(kSideSearchOutwardPoints);
        rect_width = scaled(kMinimumRectWidthPoints);
        rect_height = scaled(kMinimumRectHeightPoints);
        duplicate = scaled(kDuplicateTolerancePoints);
        halo_above = scaled(kOccluderHaloAbovePoints);
        halo_beside = scaled(kOccluderHaloBesidePoints);
        halo_below = scaled(kOccluderHaloBelowPoints);
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

// Whether the point lies within an occluder or its shadow halo.
bool NearMasked(const std::vector<IntRect>& masked_regions, const Tolerances& tolerances, int x,
                int y) {
    for (const IntRect& region : masked_regions) {
        if (x >= region.x - tolerances.halo_beside &&
            x < region.x + region.width + tolerances.halo_beside &&
            y >= region.y - tolerances.halo_above &&
            y < region.y + region.height + tolerances.halo_below)
            return true;
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
    // Gap bridging must not manufacture borders out of periodic content:
    // rows of list text sit closer together than the gap tolerance, so
    // their glyph fragments would bridge into arbitrarily long runs. A
    // real border keeps most of its span on edges even across dropouts;
    // bridged text is mostly gap.
    const auto dense = [&](int start, int end) {
        return row.EdgesIn(start, end) * 2 >= row.VisibleIn(start, end);
    };
    const auto close_segment = [&](int end) {
        if (split && segment_start >= 0 &&
            row.EdgesIn(segment_start, end) >= tolerances.run_length && dense(segment_start, end))
            runs.push_back(EdgeRun{y, segment_start, end});
        segment_start = -1;
    };
    const auto close_run = [&](int end) {
        close_segment(end);
        if (run_start >= 0 && visible_edges >= tolerances.run_length && dense(run_start, end))
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
    int streak = 0;
    int longest_streak = 0;
    for (int y = y0; y < y1; ++y) {
        if (Masked(masked_regions, x - 1, y) || Masked(masked_regions, x, y)) continue;
        ++visible;
        if (IsEdge(frame.PixelAt(x - 1, y), frame.PixelAt(x, y)) ||
            (wide && IsEdge(frame.PixelAt(wide_left, y), frame.PixelAt(wide_right, y)))) {
            ++edges;
            ++streak;
            longest_streak = std::max(longest_streak, streak);
        } else {
            streak = 0;
        }
    }
    if (visible < std::max(4, (y1 - y0) / 8)) return std::nullopt;
    // A real side is a line; a column through a list of text accumulates
    // the same edge fraction from glyph fragments, but never contiguously.
    if (wide && longest_streak * 3 < visible) return 0.0f;
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
    const int limit = std::max(tolerances.side_inward, tolerances.side_outward);
    for (int distance = 0; distance <= limit; ++distance) {
        for (const int direction : {outside, -outside}) {
            const int reach =
                direction == outside ? tolerances.side_outward : tolerances.side_inward;
            if (distance > reach) continue;
            const int x = guess + direction * distance;
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

// One axis-aligned detection pass: horizontal border runs paired into
// rectangle tops and bottoms, sides verified or - when exactly one side
// has no vertical contrast - taken from the two runs' aligned endpoints,
// which are corner evidence in their own right. Called twice: once on the
// frame and once on its transpose, so rectangles whose horizontal borders
// have vanished into the canvas are still assembled from their sides.
void DetectAxisAligned(const FrameView& frame, const std::vector<IntRect>& masked_regions,
                       const Tolerances& tolerances, std::vector<RegionCandidate>& candidates) {
    std::vector<EdgeRun> borders = HorizontalBorderRuns(frame, masked_regions, tolerances);
    if (TraceEnabled()) {
        std::printf("axis %dx%d pre-banding:\n", frame.width, frame.height);
        for (const EdgeRun& run : borders)
            std::printf("  raw y=%d x=[%d..%d)\n", run.y, run.x0, run.x1);
    }

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

    // Field diagnosis: SIDESCOPES_DETECTOR_TRACE=1 prints the qualified
    // border runs and every assembled rectangle.
    const bool trace = TraceEnabled();
    if (trace) {
        std::printf("axis %dx%d: %zu border runs\n", frame.width, frame.height, borders.size());
        for (const EdgeRun& run : borders)
            std::printf("  run y=%d x=[%d..%d)\n", run.y, run.x0, run.x1);
    }

    // Every pair of x-aligned border runs is a potential top and bottom of a
    // rectangle; the sides must then be real vertical edges.
    for (std::size_t top = 0; top < borders.size(); ++top) {
        for (std::size_t bottom = top + 1; bottom < borders.size(); ++bottom) {
            const EdgeRun& a = borders[top];
            const EdgeRun& b = borders[bottom];
            if (b.y - a.y < tolerances.rect_height) continue;
            const int end_slack = tolerances.alignment + tolerances.side_outward;
            if (std::abs(a.x0 - b.x0) > end_slack || std::abs(a.x1 - b.x1) > end_slack) continue;

            auto left =
                FindSide(frame, masked_regions, tolerances, std::max(a.x0, b.x0), -1, a.y + 1, b.y);
            auto right =
                FindSide(frame, masked_regions, tolerances, std::min(a.x1, b.x1), +1, a.y + 1, b.y);
            // Exactly one side without vertical contrast (a photo edge that
            // happens to match the canvas tone): the two runs terminating
            // at the same column is corner evidence enough. Both sides
            // missing is too little to build on.
            if (!left && right && std::abs(a.x0 - b.x0) <= tolerances.alignment)
                left = SideMatch{(a.x0 + b.x0) / 2, right->coverage * 0.6f};
            if (left && !right && std::abs(a.x1 - b.x1) <= tolerances.alignment)
                right = SideMatch{(a.x1 + b.x1) / 2, left->coverage * 0.6f};
            if (!left || !right) continue;
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
            if (trace)
                std::printf("  rect %d,%d %dx%d conf=%.2f (tops y=%d,%d)\n", rect.x, rect.y,
                            rect.width, rect.height, confidence, a.y, b.y);
        }
    }
}

// Editor canvases are perfectly uniform, and the photograph is a hole in
// that uniformity - the one boundary signal that survives when the
// photo's edge tone matches the canvas: near-matching content is still
// textured, the canvas is byte-flat. Finds the dominant flat colors,
// takes each one's largest connected region, and if that region encloses
// a single rectangular hole, the hole is a candidate.
void DetectCanvasHoles(const FrameView& frame, const std::vector<IntRect>& masked_regions,
                       const Tolerances& tolerances, std::vector<RegionCandidate>& holes) {
    const int width = frame.width;
    const int height = frame.height;
    const auto pixel = [&](int x, int y) { return frame.PixelAt(x, y); };

    // Dominant flat colors, from a sparse grid of flat samples. Each
    // bucket carries the average of its samples: quantization alone would
    // re-center the color on the bucket floor, and one stored pixel
    // misrepresents a bucket where two real tones meet (a 48-gray panel
    // and a 51-gray canvas share a 4-wide bucket; the window around
    // either one clips the other's noise out of membership).
    struct FlatColor {
        uint32_t key;
        int count;
        int64_t sum[3];
    };
    std::vector<FlatColor> flats;
    for (int y = 4; y < height - 4; y += 4) {
        for (int x = 4; x < width - 4; x += 4) {
            const uint8_t* p = pixel(x, y);
            const uint8_t* r = pixel(x + 2, y);
            const uint8_t* d = pixel(x, y + 2);
            bool flat = true;
            for (int c = 0; c < 3 && flat; ++c)
                flat = std::abs(p[c] - r[c]) <= 2 && std::abs(p[c] - d[c]) <= 2;
            if (!flat) continue;
            const uint32_t key = (static_cast<uint32_t>(p[2] >> 2) << 12) |
                                 (static_cast<uint32_t>(p[1] >> 2) << 6) |
                                 static_cast<uint32_t>(p[0] >> 2);
            bool merged = false;
            for (FlatColor& entry : flats) {
                if (entry.key == key) {
                    ++entry.count;
                    entry.sum[0] += p[0];
                    entry.sum[1] += p[1];
                    entry.sum[2] += p[2];
                    merged = true;
                    break;
                }
            }
            if (!merged) flats.push_back(FlatColor{key, 1, {p[0], p[1], p[2]}});
        }
    }
    std::sort(flats.begin(), flats.end(),
              [](const FlatColor& a, const FlatColor& b) { return a.count > b.count; });
    const int minimum_samples = (tolerances.rect_width / 4) * (tolerances.rect_height / 4) / 4;

    std::vector<uint8_t> mask(static_cast<std::size_t>(width) * height);
    std::vector<int> stack;
    // Editors surround the canvas with several panel grays that can each
    // out-sample the canvas itself, so the canvas tone is not always among
    // the very top flats; five attempts cost one cheap flood each.
    for (std::size_t rank = 0; rank < flats.size() && rank < 5; ++rank) {
        if (flats[rank].count < minimum_samples) break;
        const uint8_t canvas_r = static_cast<uint8_t>(flats[rank].sum[2] / flats[rank].count);
        const uint8_t canvas_g = static_cast<uint8_t>(flats[rank].sum[1] / flats[rank].count);
        const uint8_t canvas_b = static_cast<uint8_t>(flats[rank].sum[0] / flats[rank].count);

        // 1 = canvas-colored, 2 = member of the chosen connected region.
        std::size_t seed = 0;
        const auto matches = [&](int x, int y) {
            const uint8_t* p = pixel(x, y);
            return std::abs(p[2] - canvas_r) <= 3 && std::abs(p[1] - canvas_g) <= 3 &&
                   std::abs(p[0] - canvas_b) <= 3;
        };
        // An occluded pixel (this application's own window) hides canvas
        // as likely as photograph; counting it as canvas keeps the
        // occluder out of the hole, and the extent vote below discards
        // hole rows that merely lean against it. The occluder's shadow
        // halo joins it: the shadow darkens the surrounding canvas into a
        // non-canvas ring that would otherwise bridge the photograph to
        // whatever lies past the window and leak the hole across it.
        const auto member = [&](int x, int y) {
            return NearMasked(masked_regions, tolerances, x, y) || matches(x, y);
        };
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                // Canvas membership needs the neighborhood, not just the
                // pixel: photo content drifting through the canvas tone is
                // still textured, so its neighbors give it away and the
                // canvas region cannot erode into the photograph.
                bool match = member(x, y);
                if (match) {
                    match = (x == 0 || member(x - 1, y)) && (x + 1 == width || member(x + 1, y)) &&
                            (y == 0 || member(x, y - 1)) && (y + 1 == height || member(x, y + 1));
                }
                mask[static_cast<std::size_t>(y) * width + x] = match ? 1 : 0;
            }
        }
        // Largest connected canvas region (4-connected flood fills).
        // Regions are ranked by their pixels of this actual color: the
        // occluder joins every color's region as neutral filler, and by
        // raw size it alone would out-vote any real canvas.
        int best_size = 0;
        for (std::size_t start = 0; start < mask.size(); ++start) {
            if (mask[start] != 1) continue;
            stack.clear();
            stack.push_back(static_cast<int>(start));
            mask[start] = 3;
            int size = 0;
            while (!stack.empty()) {
                const int at = stack.back();
                stack.pop_back();
                const int x = at % width;
                const int y = at / width;
                if (!Masked(masked_regions, x, y)) ++size;
                const int neighbors[4] = {at - 1, at + 1, at - width, at + width};
                const bool valid[4] = {x > 0, x + 1 < width, y > 0, y + 1 < height};
                for (int n = 0; n < 4; ++n) {
                    if (valid[n] && mask[static_cast<std::size_t>(neighbors[n])] == 1) {
                        mask[static_cast<std::size_t>(neighbors[n])] = 3;
                        stack.push_back(neighbors[n]);
                    }
                }
            }
            if (size > best_size) {
                best_size = size;
                seed = start;
            }
        }
        if (best_size < minimum_samples * 16) continue;
        // Re-flood the winner with label 2 (everything is 3 now).
        stack.clear();
        stack.push_back(static_cast<int>(seed));
        mask[seed] = 2;
        while (!stack.empty()) {
            const int at = stack.back();
            stack.pop_back();
            const int x = at % width;
            const int y = at / width;
            const int neighbors[4] = {at - 1, at + 1, at - width, at + width};
            const bool valid[4] = {x > 0, x + 1 < width, y > 0, y + 1 < height};
            for (int n = 0; n < 4; ++n) {
                if (valid[n] && mask[static_cast<std::size_t>(neighbors[n])] == 3) {
                    mask[static_cast<std::size_t>(neighbors[n])] = 2;
                    stack.push_back(neighbors[n]);
                }
            }
        }

        // The canvas region's bounding box, then the hole: the bounding box
        // of everything inside that is not canvas.
        int cx0 = width, cy0 = height, cx1 = 0, cy1 = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (mask[static_cast<std::size_t>(y) * width + x] != 2) continue;
                cx0 = std::min(cx0, x);
                cy0 = std::min(cy0, y);
                cx1 = std::max(cx1, x + 1);
                cy1 = std::max(cy1, y + 1);
            }
        }
        if (TraceEnabled())
            std::printf("canvas %d,%d,%d region %d px bbox %d,%d..%d,%d\n", canvas_r, canvas_g,
                        canvas_b, best_size, cx0, cy0, cx1, cy1);
        if (cx1 - cx0 < tolerances.rect_width || cy1 - cy0 < tolerances.rect_height) continue;
        // The hole is the LARGEST connected non-canvas component inside the
        // canvas bounding box - a faint canvas gradient or stray dust forms
        // its own slivers and must not stretch the photograph's bounds.
        // The photograph is a solid rectangle, so its bounds come from the
        // component's ROBUST row extents: a canvas-toned flat sky patch or
        // a thin connector to some toolbar leaks the flood, but leak rows
        // are a minority and the medians ignore them.
        int hx0 = cx1, hy0 = cy1, hx1 = cx0, hy1 = cy0;
        int hole_best = 0;
        std::vector<int> row_min(static_cast<std::size_t>(height));
        std::vector<int> row_max(static_cast<std::size_t>(height));
        std::vector<int> best_row_min;
        std::vector<int> best_row_max;
        int best_y0 = 0;
        for (int sy = cy0; sy < cy1; ++sy) {
            for (int sx = cx0; sx < cx1; ++sx) {
                const std::size_t at = static_cast<std::size_t>(sy) * width + sx;
                if (mask[at] == 2 || mask[at] == 4) continue;
                stack.clear();
                stack.push_back(static_cast<int>(at));
                mask[at] = 4;
                int size = 0;
                int by0 = sy, by1 = sy + 1;
                std::fill(row_min.begin(), row_min.end(), width);
                std::fill(row_max.begin(), row_max.end(), -1);
                while (!stack.empty()) {
                    const int cell = stack.back();
                    stack.pop_back();
                    ++size;
                    const int x = cell % width;
                    const int y = cell / width;
                    by0 = std::min(by0, y);
                    by1 = std::max(by1, y + 1);
                    row_min[static_cast<std::size_t>(y)] =
                        std::min(row_min[static_cast<std::size_t>(y)], x);
                    row_max[static_cast<std::size_t>(y)] =
                        std::max(row_max[static_cast<std::size_t>(y)], x + 1);
                    const int neighbors[4] = {cell - 1, cell + 1, cell - width, cell + width};
                    const bool valid[4] = {x > cx0, x + 1 < cx1, y > cy0, y + 1 < cy1};
                    for (int n = 0; n < 4; ++n) {
                        const std::size_t next = static_cast<std::size_t>(neighbors[n]);
                        if (valid[n] && mask[next] != 2 && mask[next] != 4) {
                            mask[next] = 4;
                            stack.push_back(neighbors[n]);
                        }
                    }
                }
                if (size > hole_best) {
                    hole_best = size;
                    best_y0 = by0;
                    best_row_min.assign(row_min.begin() + by0, row_min.begin() + by1);
                    best_row_max.assign(row_max.begin() + by0, row_max.begin() + by1);
                }
            }
        }
        if (hole_best > 0) {
            // Rows carrying at least half the widest span vote; the median
            // of their extents is the rectangle.
            int widest = 0;
            for (std::size_t i = 0; i < best_row_min.size(); ++i)
                widest = std::max(widest, best_row_max[i] - best_row_min[i]);
            // Rows ending against clean canvas read the side exactly;
            // rows cut short by an occluder or its shadow halo read a
            // lower bound - the photograph provably reaches the occluder
            // there. Each population gets its own median and the side
            // takes the farther of the two, so a handful of clean-ending
            // sub-features (an info overlay's text rows) cannot pull the
            // side in from where truncated rows prove content to be.
            std::vector<int> lefts;
            std::vector<int> rights;
            std::vector<int> lefts_truncated;
            std::vector<int> rights_truncated;
            int first_full = -1;
            int last_full = -1;
            for (std::size_t i = 0; i < best_row_min.size(); ++i) {
                if (best_row_max[i] - best_row_min[i] < widest / 2) continue;
                const int y = best_y0 + static_cast<int>(i);
                if (best_row_min[i] == 0 ||
                    !NearMasked(masked_regions, tolerances, best_row_min[i] - 1, y))
                    lefts.push_back(best_row_min[i]);
                else
                    lefts_truncated.push_back(best_row_min[i]);
                if (best_row_max[i] == width ||
                    !NearMasked(masked_regions, tolerances, best_row_max[i], y))
                    rights.push_back(best_row_max[i]);
                else
                    rights_truncated.push_back(best_row_max[i]);
                if (first_full < 0) first_full = static_cast<int>(i);
                last_full = static_cast<int>(i);
            }
            const auto median_of = [](std::vector<int>& values) {
                std::sort(values.begin(), values.end());
                return values[values.size() / 2];
            };
            if (!lefts.empty() || !lefts_truncated.empty()) {
                hx0 = lefts.empty() ? median_of(lefts_truncated)
                      : lefts_truncated.empty()
                          ? median_of(lefts)
                          : std::min(median_of(lefts), median_of(lefts_truncated));
            }
            if (!rights.empty() || !rights_truncated.empty()) {
                hx1 = rights.empty() ? median_of(rights_truncated)
                      : rights_truncated.empty()
                          ? median_of(rights)
                          : std::max(median_of(rights), median_of(rights_truncated));
            }
            if (first_full >= 0) {
                // Where the photograph itself drifts through the canvas
                // tone (a black sky on a black canvas), the melted side
                // drops those rows below the voting width even though the
                // visible remainder still marks the true edge. Extend
                // through contiguous rows whose extent stays inside the
                // voted span and keeps substantial width - a leak row
                // reaching outside the span still ends the walk.
                const auto edge_row = [&](int i) {
                    const std::size_t at = static_cast<std::size_t>(i);
                    return best_row_min[at] >= hx0 - tolerances.duplicate &&
                           best_row_max[at] <= hx1 + tolerances.duplicate &&
                           best_row_max[at] - best_row_min[at] >= (hx1 - hx0) / 4;
                };
                while (first_full > 0 && edge_row(first_full - 1)) --first_full;
                while (last_full + 1 < static_cast<int>(best_row_min.size()) &&
                       edge_row(last_full + 1))
                    ++last_full;
                hy0 = best_y0 + first_full;
                hy1 = best_y0 + last_full + 1;
            }
        }
        if (TraceEnabled()) std::printf("  hole %d,%d..%d,%d\n", hx0, hy0, hx1, hy1);
        if (hx1 - hx0 < tolerances.rect_width || hy1 - hy0 < tolerances.rect_height) continue;
        // Enclosure: the hole must sit strictly inside the canvas region,
        // not lean on its bounding box (which would mean the "hole" is
        // just whatever lies beyond the canvas).
        if (hx0 <= cx0 || hy0 <= cy0 || hx1 >= cx1 || hy1 >= cy1) continue;

        const IntRect rect{hx0, hy0, hx1 - hx0, hy1 - hy0};
        bool duplicate = false;
        for (const RegionCandidate& existing : holes) {
            if (SameRectangle(existing.rect, rect, tolerances.duplicate)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) holes.push_back(RegionCandidate{rect, 1.0f});
        if (TraceEnabled())
            std::printf("canvas hole %d,%d %dx%d (canvas %d,%d,%d)\n", rect.x, rect.y, rect.width,
                        rect.height, canvas_r, canvas_g, canvas_b);
    }
}

}  // namespace

std::vector<RegionCandidate> DetectPhotoRegions(const FrameView& frame,
                                                const std::vector<IntRect>& masked_regions,
                                                float pixels_per_point, int max_candidates) {
    std::vector<RegionCandidate> candidates;
    const Tolerances tolerances(pixels_per_point);
    if (frame.width < tolerances.rect_width || frame.height < tolerances.rect_height)
        return candidates;

    DetectAxisAligned(frame, masked_regions, tolerances, candidates);

    // Second pass on the transposed frame: a rectangle whose top or bottom
    // has melted into the canvas still has sides, and in the transpose
    // those sides become the horizontal borders the assembly knows how to
    // pair. The copy costs a few milliseconds once per picker opening.
    std::vector<uint8_t> transposed(static_cast<std::size_t>(frame.width) * frame.height * 4);
    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* row = frame.bgra + static_cast<std::size_t>(y) * frame.stride_bytes;
        for (int x = 0; x < frame.width; ++x) {
            uint8_t* out = transposed.data() + (static_cast<std::size_t>(x) * frame.height + y) * 4;
            const uint8_t* in = row + static_cast<std::size_t>(x) * 4;
            out[0] = in[0];
            out[1] = in[1];
            out[2] = in[2];
            out[3] = in[3];
        }
    }
    const FrameView transposed_frame{transposed.data(), frame.height * 4,  frame.height,
                                     frame.width,       frame.color_space, frame.sequence};
    std::vector<IntRect> transposed_masks;
    transposed_masks.reserve(masked_regions.size());
    for (const IntRect& mask : masked_regions)
        transposed_masks.push_back(IntRect{mask.y, mask.x, mask.height, mask.width});
    std::vector<RegionCandidate> sideways;
    DetectAxisAligned(transposed_frame, transposed_masks, tolerances, sideways);
    for (const RegionCandidate& candidate : sideways) {
        const IntRect rect{candidate.rect.y, candidate.rect.x, candidate.rect.height,
                           candidate.rect.width};
        bool duplicate = false;
        for (RegionCandidate& existing : candidates) {
            if (SameRectangle(existing.rect, rect, tolerances.duplicate)) {
                existing.confidence = std::max(existing.confidence, candidate.confidence);
                duplicate = true;
                break;
            }
        }
        if (!duplicate) candidates.push_back(RegionCandidate{rect, candidate.confidence});
    }

    // A canvas hole is measured pixel by pixel, while an edge pairing
    // carries endpoint slack, so a hole outranks edge geometry of equal
    // confidence. An edge rectangle that coincides with a hole is the same
    // find twice and yields to it.
    std::vector<RegionCandidate> holes;
    DetectCanvasHoles(frame, masked_regions, tolerances, holes);
    struct Ranked {
        RegionCandidate candidate;
        bool hole;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(candidates.size() + holes.size());
    for (const RegionCandidate& candidate : candidates) {
        bool duplicate = false;
        for (const RegionCandidate& hole : holes) {
            if (SameRectangle(candidate.rect, hole.rect, tolerances.duplicate)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) ranked.push_back(Ranked{candidate, false});
    }
    for (const RegionCandidate& hole : holes) ranked.push_back(Ranked{hole, true});

    // A rectangle mostly inside a measured photograph is photograph
    // content - texture that paired into a phantom border, or a flat
    // patch that read as its own canvas - and stealing the hover from the
    // photograph is all it would do. A hole spanning most of the frame is
    // ambient (a dark application theme reads as one big canvas around
    // everything) and vouches for nothing.
    const int64_t frame_area = static_cast<int64_t>(frame.width) * frame.height;
    std::erase_if(ranked, [&](const Ranked& entry) {
        const IntRect& c = entry.candidate.rect;
        const int64_t area = static_cast<int64_t>(c.width) * c.height;
        for (const RegionCandidate& hole : holes) {
            const IntRect& h = hole.rect;
            const int64_t hole_area = static_cast<int64_t>(h.width) * h.height;
            if (hole_area * 5 >= frame_area * 3) continue;  // ambient
            if (entry.hole && area >= hole_area) continue;  // itself, or a peer
            const int64_t overlap_w = std::min(c.x + c.width, h.x + h.width) - std::max(c.x, h.x);
            const int64_t overlap_h = std::min(c.y + c.height, h.y + h.height) - std::max(c.y, h.y);
            if (overlap_w <= 0 || overlap_h <= 0) continue;
            if (overlap_w * overlap_h * 20 >= area * 13) return true;  // >= 65% inside
            // A band crossing clean through the photograph - vertically
            // inside it while spanning past both its sides, or the
            // transpose - is a phantom pairing between a photo border and
            // a content line whose ends drifted into the panels.
            const bool rows_inside = overlap_h * 10 >= static_cast<int64_t>(c.height) * 9;
            const bool spans_columns = c.x <= h.x && c.x + c.width >= h.x + h.width;
            const bool cols_inside = overlap_w * 10 >= static_cast<int64_t>(c.width) * 9;
            const bool spans_rows = c.y <= h.y && c.y + c.height >= h.y + h.height;
            if ((rows_inside && spans_columns) || (cols_inside && spans_rows)) return true;
            // A border running through the photograph's interior was paired
            // off photo content (a long contour inside the picture); the
            // real windows a screen offers come from the platform's window
            // list, never from borders found inside a photograph.
            const int margin = tolerances.duplicate;
            const auto row_through = [&](int y) {
                return y > h.y + margin && y < h.y + h.height - margin &&
                       overlap_w * 2 >= static_cast<int64_t>(h.width);
            };
            const auto column_through = [&](int x) {
                return x > h.x + margin && x < h.x + h.width - margin &&
                       overlap_h * 2 >= static_cast<int64_t>(h.height);
            };
            if (!entry.hole && (row_through(c.y) || row_through(c.y + c.height) ||
                                column_through(c.x) || column_through(c.x + c.width)))
                return true;
        }
        return false;
    });

    // Strongest first - confidence, then measurement, then size: the
    // diversity pass keeps the first member of every near-copy cluster,
    // and that seat belongs to the best-established geometry, not merely
    // the biggest.
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.candidate.confidence != b.candidate.confidence)
            return a.candidate.confidence > b.candidate.confidence;
        if (a.hole != b.hole) return a.hole;
        return static_cast<int64_t>(a.candidate.rect.width) * a.candidate.rect.height >
               static_cast<int64_t>(b.candidate.rect.width) * b.candidate.rect.height;
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
    for (const Ranked& entry : ranked) {
        const auto& c = entry.candidate.rect;
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
        (near_copy ? redundant : kept).push_back(entry.candidate);
    }
    kept.insert(kept.end(), redundant.begin(), redundant.end());
    if (static_cast<int>(kept.size()) > max_candidates)
        kept.resize(static_cast<std::size_t>(max_candidates));
    return kept;
}

}  // namespace sidescopes
