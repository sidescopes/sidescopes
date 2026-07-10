#include "core/photo_region_detector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>

namespace sidescopes {
namespace {

// The frame is scored on a coarse grid. Chrome is recognized by what it
// really is: the dominant flat color family of the frame. Editors draw
// neutral, uniform chrome so it does not bias color judgment; everything
// that is either textured, colored, or flat-but-not-chrome-colored is
// content. This matters because viewers downscale photographs to fit the
// window, which averages smooth regions (skies, defocused backgrounds) down
// to rendering-flat - flatness alone cannot separate them from chrome, but
// their color can.
constexpr int kBlockSize = 16;

constexpr double kVarianceThreshold = 1.0;  // luma variance above rendering-flat
constexpr double kChromaThreshold = 4.0;    // mean max channel spread
constexpr int kDistinctColorThreshold = 4;  // of max 512 coarse buckets

// A flat block counts as chrome when its mean color is within this distance
// (per channel) of one of the frame's dominant flat colors.
constexpr int kChromeColorTolerance = 20;

// Candidates overlapping an earlier one are slivers of the same region.
constexpr double kMaximumOverlapFraction = 0.3;

struct MeanColor {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

struct FrameAnalysis {
    int columns = 0;
    int rows = 0;
    std::vector<uint8_t> rich;        // 1 = content
    std::vector<MeanColor> means;     // per block
    std::vector<uint8_t> flat;        // 1 = texture-flat (regardless of color)
    std::array<MeanColor, 2> chrome;  // dominant flat colors
    int chrome_count = 0;

    [[nodiscard]] bool MatchesChrome(const MeanColor& color) const {
        for (int i = 0; i < chrome_count; ++i) {
            if (std::abs(color.r - chrome[i].r) <= kChromeColorTolerance &&
                std::abs(color.g - chrome[i].g) <= kChromeColorTolerance &&
                std::abs(color.b - chrome[i].b) <= kChromeColorTolerance)
                return true;
        }
        return false;
    }
};

FrameAnalysis AnalyzeBlocks(const FrameView& frame) {
    FrameAnalysis analysis;
    analysis.columns = std::max(1, frame.width / kBlockSize);
    analysis.rows = std::max(1, frame.height / kBlockSize);
    const std::size_t block_count = static_cast<std::size_t>(analysis.columns) * analysis.rows;
    analysis.rich.assign(block_count, 0);
    analysis.flat.assign(block_count, 0);
    analysis.means.assign(block_count, MeanColor{});

    std::vector<uint8_t> color_seen(512);
    for (int cy = 0; cy < analysis.rows; ++cy) {
        for (int cx = 0; cx < analysis.columns; ++cx) {
            double luma_sum = 0.0;
            double luma_squared = 0.0;
            double chroma_sum = 0.0;
            double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
            std::fill(color_seen.begin(), color_seen.end(), uint8_t{0});
            int distinct_colors = 0;

            const int y0 = cy * kBlockSize;
            const int x0 = cx * kBlockSize;
            for (int py = y0; py < y0 + kBlockSize; ++py) {
                const uint8_t* pixel = frame.PixelAt(x0, py);
                for (int px = 0; px < kBlockSize; ++px, pixel += 4) {
                    const int b = pixel[0], g = pixel[1], r = pixel[2];
                    const double luma = (54 * r + 183 * g + 19 * b) / 256.0;
                    luma_sum += luma;
                    luma_squared += luma * luma;
                    chroma_sum += std::max({r, g, b}) - std::min({r, g, b});
                    sum_r += r;
                    sum_g += g;
                    sum_b += b;
                    const int bucket = ((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5);
                    if (!color_seen[bucket]) {
                        color_seen[bucket] = 1;
                        ++distinct_colors;
                    }
                }
            }

            constexpr double kSamples = kBlockSize * kBlockSize;
            const double mean = luma_sum / kSamples;
            const double variance = luma_squared / kSamples - mean * mean;
            const double mean_chroma = chroma_sum / kSamples;
            const std::size_t index = static_cast<std::size_t>(cy) * analysis.columns + cx;
            analysis.means[index] = MeanColor{sum_r / kSamples, sum_g / kSamples, sum_b / kSamples};
            const bool textured = variance >= kVarianceThreshold ||
                                  mean_chroma >= kChromaThreshold ||
                                  distinct_colors >= kDistinctColorThreshold;
            analysis.flat[index] = textured ? 0 : 1;
            analysis.rich[index] = textured ? 1 : 0;
        }
    }

    // The chrome palette: the two most common flat colors (quantized), which
    // covers two-tone interfaces (panel gray plus canvas gray).
    std::map<int, std::pair<int, MeanColor>> flat_colors;
    for (std::size_t index = 0; index < block_count; ++index) {
        if (!analysis.flat[index]) continue;
        const MeanColor& mean = analysis.means[index];
        const int key = (static_cast<int>(mean.r) / 16 << 8) |
                        (static_cast<int>(mean.g) / 16 << 4) | (static_cast<int>(mean.b) / 16);
        auto& entry = flat_colors[key];
        ++entry.first;
        entry.second = mean;
    }
    std::array<std::pair<int, MeanColor>, 2> top{};
    for (const auto& [key, entry] : flat_colors) {
        (void)key;
        if (entry.first > top[0].first) {
            top[1] = top[0];
            top[0] = entry;
        } else if (entry.first > top[1].first) {
            top[1] = entry;
        }
    }
    // The dominant flat color is always chrome. A second tone qualifies only
    // when it rivals the first in coverage (panel gray next to canvas gray);
    // otherwise it is more likely a large smooth patch of a photograph
    // claiming the slot, which would punch that patch out of its own photo.
    if (top[0].first > 0) analysis.chrome[analysis.chrome_count++] = top[0].second;
    if (top[1].first * 2 >= top[0].first && top[1].first > 0)
        analysis.chrome[analysis.chrome_count++] = top[1].second;

    // Flat blocks whose color is not chrome are content: a rendered-smooth
    // sky is flat, but it is not editor gray.
    for (std::size_t index = 0; index < block_count; ++index) {
        if (analysis.flat[index] && !analysis.MatchesChrome(analysis.means[index]))
            analysis.rich[index] = 1;
    }
    return analysis;
}

// A connected region of content blocks. The bounding box - not a largest
// all-rich rectangle - is the right shape for a photograph: any remaining
// holes belong to the photo, not to the chrome around it.
struct GridComponent {
    int min_cx = 0, min_cy = 0, max_cx = 0, max_cy = 0;
    int rich_blocks = 0;

    [[nodiscard]] int Columns() const { return max_cx - min_cx + 1; }
    [[nodiscard]] int Rows() const { return max_cy - min_cy + 1; }
};

std::vector<GridComponent> RichComponents(const FrameAnalysis& analysis) {
    std::vector<GridComponent> components;
    std::vector<uint8_t> visited(analysis.rich.size(), 0);
    std::vector<std::pair<int, int>> queue;

    for (int seed_cy = 0; seed_cy < analysis.rows; ++seed_cy) {
        for (int seed_cx = 0; seed_cx < analysis.columns; ++seed_cx) {
            const std::size_t seed_index =
                static_cast<std::size_t>(seed_cy) * analysis.columns + seed_cx;
            if (!analysis.rich[seed_index] || visited[seed_index]) continue;

            GridComponent component{seed_cx, seed_cy, seed_cx, seed_cy, 0};
            queue.clear();
            queue.emplace_back(seed_cx, seed_cy);
            visited[seed_index] = 1;
            while (!queue.empty()) {
                const auto [cx, cy] = queue.back();
                queue.pop_back();
                ++component.rich_blocks;
                component.min_cx = std::min(component.min_cx, cx);
                component.min_cy = std::min(component.min_cy, cy);
                component.max_cx = std::max(component.max_cx, cx);
                component.max_cy = std::max(component.max_cy, cy);
                // Neighbors within two blocks bridge narrow flat seams.
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int nx = cx + dx;
                        const int ny = cy + dy;
                        if (nx < 0 || nx >= analysis.columns || ny < 0 || ny >= analysis.rows)
                            continue;
                        const std::size_t index =
                            static_cast<std::size_t>(ny) * analysis.columns + nx;
                        if (!analysis.rich[index] || visited[index]) continue;
                        visited[index] = 1;
                        queue.emplace_back(nx, ny);
                    }
                }
            }
            components.push_back(component);
        }
    }
    return components;
}

// A one-pixel line is content when it varies or when its color is not
// chrome; used for pixel-exact edge refinement and border probing.
bool LineIsContent(const FrameView& frame, const FrameAnalysis& analysis, int x0, int x1, int y0,
                   int y1) {
    double luma_sum = 0.0;
    double luma_squared = 0.0;
    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    int samples = 0;
    for (int py = y0; py < y1; ++py) {
        const uint8_t* pixel = frame.PixelAt(x0, py);
        for (int px = x0; px < x1; ++px, pixel += 4) {
            const double luma = (54 * pixel[2] + 183 * pixel[1] + 19 * pixel[0]) / 256.0;
            luma_sum += luma;
            luma_squared += luma * luma;
            sum_r += pixel[2];
            sum_g += pixel[1];
            sum_b += pixel[0];
            ++samples;
        }
    }
    if (samples == 0) return false;
    const double mean = luma_sum / samples;
    if (luma_squared / samples - mean * mean >= kVarianceThreshold) return true;
    return !analysis.MatchesChrome(MeanColor{sum_r / samples, sum_g / samples, sum_b / samples});
}

// Shrinks each edge through mixed border blocks, then grows to the exact
// content boundary.
IntRect RefineEdges(const FrameView& frame, const FrameAnalysis& analysis, IntRect rect) {
    const int limit = kBlockSize;
    const auto row_is_content = [&](int py) {
        return LineIsContent(frame, analysis, rect.x, rect.x + rect.width, py, py + 1);
    };
    const auto column_is_content = [&](int px) {
        return LineIsContent(frame, analysis, px, px + 1, rect.y, rect.y + rect.height);
    };

    for (int step = 0; step < limit && rect.height > limit && !row_is_content(rect.y); ++step) {
        ++rect.y;
        --rect.height;
    }
    for (int step = 0; step < limit && rect.y > 0 && row_is_content(rect.y - 1); ++step) {
        --rect.y;
        ++rect.height;
    }
    for (int step = 0;
         step < limit && rect.height > limit && !row_is_content(rect.y + rect.height - 1); ++step) {
        --rect.height;
    }
    for (int step = 0; step < limit && rect.y + rect.height < frame.height &&
                       row_is_content(rect.y + rect.height);
         ++step) {
        ++rect.height;
    }
    for (int step = 0; step < limit && rect.width > limit && !column_is_content(rect.x); ++step) {
        ++rect.x;
        --rect.width;
    }
    for (int step = 0; step < limit && rect.x > 0 && column_is_content(rect.x - 1); ++step) {
        --rect.x;
        ++rect.width;
    }
    for (int step = 0;
         step < limit && rect.width > limit && !column_is_content(rect.x + rect.width - 1);
         ++step) {
        --rect.width;
    }
    for (int step = 0; step < limit && rect.x + rect.width < frame.width &&
                       column_is_content(rect.x + rect.width);
         ++step) {
        ++rect.width;
    }
    return rect;
}

// Flatness of the chrome immediately outside the rectangle, 0..1.
float BorderFlatness(const FrameView& frame, const FrameAnalysis& analysis, const IntRect& rect) {
    int flat = 0;
    int total = 0;
    const auto probe = [&](int x0, int x1, int y0, int y1) {
        if (x0 < 0 || y0 < 0 || x1 > frame.width || y1 > frame.height || x0 >= x1 || y0 >= y1)
            return;
        ++total;
        if (!LineIsContent(frame, analysis, x0, x1, y0, y1)) ++flat;
    };
    constexpr int kProbe = 4;
    probe(rect.x, rect.x + rect.width, rect.y - kProbe, rect.y);
    probe(rect.x, rect.x + rect.width, rect.y + rect.height, rect.y + rect.height + kProbe);
    probe(rect.x - kProbe, rect.x, rect.y, rect.y + rect.height);
    probe(rect.x + rect.width, rect.x + rect.width + kProbe, rect.y, rect.y + rect.height);
    // A rect flush with the frame edge has fewer probes; that is fine.
    return total == 0 ? 1.0f : static_cast<float>(flat) / static_cast<float>(total);
}

double OverlapFraction(const IntRect& a, const IntRect& b) {
    const int left = std::max(a.x, b.x);
    const int top = std::max(a.y, b.y);
    const int right = std::min(a.x + a.width, b.x + b.width);
    const int bottom = std::min(a.y + a.height, b.y + b.height);
    if (right <= left || bottom <= top) return 0.0;
    const double intersection =
        static_cast<double>(right - left) * static_cast<double>(bottom - top);
    const double smaller =
        std::min(static_cast<double>(a.width) * a.height, static_cast<double>(b.width) * b.height);
    return smaller > 0.0 ? intersection / smaller : 0.0;
}

}  // namespace

std::vector<RegionCandidate> DetectPhotoRegions(const FrameView& frame, int max_candidates) {
    std::vector<RegionCandidate> candidates;
    if (frame.width < 2 * kBlockSize || frame.height < 2 * kBlockSize) return candidates;

    const FrameAnalysis analysis = AnalyzeBlocks(frame);
    const int minimum_blocks =
        std::max(4, analysis.columns * analysis.rows / 100);  // ignore specks under ~1%

    std::vector<GridComponent> components = RichComponents(analysis);
    std::sort(components.begin(), components.end(),
              [](const GridComponent& a, const GridComponent& b) {
                  return a.rich_blocks > b.rich_blocks;
              });

    for (const GridComponent& component : components) {
        if (static_cast<int>(candidates.size()) >= max_candidates) break;
        if (component.rich_blocks < minimum_blocks) continue;

        const float interior = static_cast<float>(component.rich_blocks) /
                               static_cast<float>(component.Columns() * component.Rows());
        if (interior < 0.25f) continue;  // disconnected UI speckle, not a photo

        IntRect rect{component.min_cx * kBlockSize, component.min_cy * kBlockSize,
                     component.Columns() * kBlockSize, component.Rows() * kBlockSize};
        rect = RefineEdges(frame, analysis, rect);

        bool sliver_of_previous = false;
        for (const RegionCandidate& earlier : candidates) {
            if (OverlapFraction(rect, earlier.rect) > kMaximumOverlapFraction) {
                sliver_of_previous = true;
                break;
            }
        }
        if (sliver_of_previous) continue;

        RegionCandidate candidate;
        candidate.rect = rect;
        candidate.confidence = interior * BorderFlatness(frame, analysis, rect);
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
