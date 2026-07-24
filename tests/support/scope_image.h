#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "core/scopes/scope_types.h"
#include "sidescopes/module.h"

namespace sidescopes::test {

// Shared probes over a scope's rendered image, replacing the copies the scope
// suites hand-rolled. They read either a host ScopeImage or the module
// boundary's SsImageView, both of which lay pixels out as tightly-packed RGBA.

// The (x, y) of the pixel with the greatest R+G+B, or {-1, -1} for an
// all-black image. Smoothing spreads a single-color trace over a small
// neighborhood, and the peak must stay on the exact coordinate.
inline std::pair<int, int> brightestPixel(const uint8_t* rgba, int width, int height)
{
    std::pair<int, int> brightest{-1, -1};
    int best = 0;
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            const uint8_t* pixel = rgba + (static_cast<std::size_t>(py) * width + px) * 4;
            const int sum = pixel[0] + pixel[1] + pixel[2];
            if (sum > best) {
                best = sum;
                brightest = {px, py};
            }
        }
    }
    return brightest;
}

inline std::pair<int, int> brightestPixel(const ScopeImage& image)
{
    if (image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) {
        std::fprintf(stderr, "scope-image probe: brightestPixel on %dx%d image with %zu rgba bytes\n", image.width,
                     image.height, image.rgba.size());
        return {-1, -1};
    }
    return brightestPixel(image.rgba.data(), image.width, image.height);
}

inline std::pair<int, int> brightestPixel(const SsImageView& image)
{
    return brightestPixel(image.rgba, image.width, image.height);
}

// The row whose brightest pixel in one channel (0=r, 1=g, 2=b) is the
// strongest, or -1 when the channel is dark everywhere.
inline int brightestRow(const uint8_t* rgba, int width, int height, int channel)
{
    int bestRow = -1;
    int best = 0;
    for (int py = 0; py < height; ++py) {
        int rowMax = 0;
        for (int px = 0; px < width; ++px) {
            rowMax =
                std::max(rowMax, static_cast<int>(rgba[(static_cast<std::size_t>(py) * width + px) * 4 + channel]));
        }
        if (rowMax > best) {
            best = rowMax;
            bestRow = py;
        }
    }
    return bestRow;
}

inline int brightestRow(const SsImageView& image, int channel)
{
    return brightestRow(image.rgba, image.width, image.height, channel);
}

// Whether (px, py) is a real pixel of @p image whose four RGBA bytes are backed
// by the buffer. A degenerate image - empty or short rgba, or a coordinate off
// the grid - is reported by name so the per-pixel probes fail legibly instead
// of dereferencing past the buffer into a wild near-null read.
inline bool probeInBounds(const ScopeImage& image, int px, int py)
{
    if (px >= 0 && py >= 0 && px < image.width && py < image.height) {
        const std::size_t offset = (static_cast<std::size_t>(py) * image.width + px) * 4;
        if (image.rgba.size() >= offset + 4) {
            return true;
        }
    }
    std::fprintf(stderr, "scope-image probe: pixel (%d,%d) outside %dx%d image with %zu rgba bytes\n", px, py,
                 image.width, image.height, image.rgba.size());
    return false;
}

// Whether any channel is lit at a pixel.
inline bool pixelLit(const ScopeImage& image, int px, int py)
{
    if (!probeInBounds(image, px, py)) {
        return false;
    }
    const uint8_t* rgba = image.rgba.data() + (static_cast<std::size_t>(py) * image.width + px) * 4;
    return rgba[0] + rgba[1] + rgba[2] > 0;
}

// One channel byte at a pixel (0=r, 1=g, 2=b, 3=a).
inline uint8_t channelAt(const ScopeImage& image, int px, int py, int channel)
{
    if (!probeInBounds(image, px, py)) {
        return 0;
    }
    return image.rgba[(static_cast<std::size_t>(py) * image.width + px) * 4 + channel];
}

// The rows that are local brightness peaks in one channel. Smoothing spreads
// a flat color's trace one row up and down; what must stay exact is where the
// peaks sit.
inline std::vector<int> peakRows(const ScopeImage& image, int channel)
{
    if (image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) {
        std::fprintf(stderr, "scope-image probe: peakRows on %dx%d image with %zu rgba bytes\n", image.width,
                     image.height, image.rgba.size());
        return {};
    }
    std::vector<int> brightness(static_cast<std::size_t>(image.height), 0);
    for (int py = 0; py < image.height; ++py) {
        int rowMax = 0;
        for (int px = 0; px < image.width; ++px) {
            const uint8_t* rgba = image.rgba.data() + (static_cast<std::size_t>(py) * image.width + px) * 4;
            rowMax = std::max(rowMax, static_cast<int>(rgba[channel]));
        }
        brightness[static_cast<std::size_t>(py)] = rowMax;
    }
    std::vector<int> peaks;
    for (int py = 0; py < image.height; ++py) {
        const int value = brightness[static_cast<std::size_t>(py)];
        if (value == 0) {
            continue;
        }
        const int above = py > 0 ? brightness[static_cast<std::size_t>(py) - 1] : 0;
        const int below = py + 1 < image.height ? brightness[static_cast<std::size_t>(py) + 1] : 0;
        if ((value >= above && value > below) || (value > above && value >= below)) {
            peaks.push_back(py);
        }
    }
    return peaks;
}

}  // namespace sidescopes::test
