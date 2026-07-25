#include "core/scopes/neutral.h"

#include <algorithm>
#include <cmath>

#include "core/color_lab.h"

namespace sidescopes {
namespace {

// The near-neutral cloud's tone: a cool grey, faint, so the bright average dot
// reads on top of it. Density drives the alpha; this is only the hue.
constexpr uint8_t CloudR = 176;
constexpr uint8_t CloudG = 190;
constexpr uint8_t CloudB = 214;
constexpr float CloudMaxAlpha = 150.0f;

// The average-cast dot: a bright fill inside a dark ring, so it stands off both
// the cloud and a light pane.
constexpr float DotRadiusFraction = 0.022f;
constexpr float DotRingWidth = 1.6f;

int clampInt(int value, int low, int high)
{
    return std::max(low, std::min(high, value));
}

}  // namespace

Neutral::Neutral()
{
    resize(DefaultNeutralSize);
    configure(m_settings);
}

float Neutral::axisRange() const
{
    switch (m_settings.range) {
    case NeutralRange::Fine:
        return 20.0f;
    case NeutralRange::Wide:
        return 80.0f;
    case NeutralRange::Normal:
        break;
    }

    return 40.0f;
}

void Neutral::configure(const NeutralSettings& settings)
{
    m_settings = settings;
    m_settings.gain = std::max(0.0f, settings.gain);
    m_settings.samplingStride = clampInt(settings.samplingStride, 1, 8);
    m_settings.neutralChroma = std::max(0.0f, settings.neutralChroma);
    m_axisRange = axisRange();
    resize(std::max(16, settings.size));
    renderImage();
}

void Neutral::resize(int size)
{
    if (size == m_imageSize && !m_cloud.empty()) {
        return;
    }
    m_imageSize = size;
    m_cloud.assign(static_cast<std::size_t>(size) * size, 0.0f);
    m_image.width = size;
    m_image.height = size;
    m_image.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
}

NormalizedPoint Neutral::projectAb(float a, float b) const
{
    // b* (blue-yellow) is the temperature axis, warm to the right; a*
    // (green-magenta) is the tint axis, magenta up. Image y grows downward.
    const float x = 0.5f + b / (2.0f * m_axisRange);
    const float y = 0.5f - a / (2.0f * m_axisRange);

    return {std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f)};
}

NormalizedPoint Neutral::project(const FloatColor& color) const
{
    const LabColor lab = labFromSrgb(color);

    return projectAb(lab.a, lab.b);
}

void Neutral::accumulate(const FrameView& frame, IntRect region)
{
    region = region.clampedTo(frame.width, frame.height);
    std::fill(m_cloud.begin(), m_cloud.end(), 0.0f);
    double sumA = 0.0;
    double sumB = 0.0;
    uint64_t count = 0;
    uint64_t neutral = 0;
    const int stride = m_settings.samplingStride;
    for (int py = region.y; py < region.y + region.height; py += stride) {
        const uint8_t* pixel = frame.pixelAt(region.x, py);
        const uint8_t* rowEnd = frame.pixelAt(region.x + region.width, py);
        for (; pixel < rowEnd; pixel += static_cast<std::ptrdiff_t>(4) * stride) {
            // The byte-sourced conversion: bit-identical here, where every
            // channel arrives as a code, and it does not evaluate the
            // transfer function once per channel per pixel.
            const LabColor lab = labFromSrgb8(Color{pixel[2], pixel[1], pixel[0]});
            sumA += lab.a;
            sumB += lab.b;
            ++count;
            if (chromaOf(lab) <= m_settings.neutralChroma) {
                splatNeutral(projectAb(lab.a, lab.b));
                ++neutral;
            }
        }
    }

    m_hasData = count > 0;
    m_neutralCount = neutral;
    m_average = count > 0 ? projectAb(static_cast<float>(sumA / static_cast<double>(count)),
                                      static_cast<float>(sumB / static_cast<double>(count)))
                          : NormalizedPoint{0.5f, 0.5f};
    renderImage();
}

void Neutral::splatNeutral(NormalizedPoint at)
{
    const int cx =
        clampInt(static_cast<int>(std::lround(at.x * static_cast<float>(m_imageSize - 1))), 0, m_imageSize - 1);
    const int cy =
        clampInt(static_cast<int>(std::lround(at.y * static_cast<float>(m_imageSize - 1))), 0, m_imageSize - 1);
    for (int dy = -1; dy <= 1; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= m_imageSize) {
            continue;
        }
        for (int dx = -1; dx <= 1; ++dx) {
            const int x = cx + dx;
            if (x < 0 || x >= m_imageSize) {
                continue;
            }
            const float weight = (dx == 0 && dy == 0) ? 1.0f : (dx == 0 || dy == 0) ? 0.5f : 0.25f;
            m_cloud[static_cast<std::size_t>(y) * m_imageSize + x] += weight;
        }
    }
}

void Neutral::blendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, float alpha)
{
    if (alpha <= 0.0f || x < 0 || y < 0 || x >= m_imageSize || y >= m_imageSize) {
        return;
    }
    const std::size_t i = (static_cast<std::size_t>(y) * m_imageSize + x) * 4;
    const float dstA = static_cast<float>(m_image.rgba[i + 3]) / 255.0f;
    const float outA = alpha + dstA * (1.0f - alpha);
    if (outA <= 0.0f) {
        return;
    }
    const auto mix = [&](uint8_t src, uint8_t dst) {
        const float value = (static_cast<float>(src) * alpha + static_cast<float>(dst) * dstA * (1.0f - alpha)) / outA;

        return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
    };
    m_image.rgba[i + 0] = mix(r, m_image.rgba[i + 0]);
    m_image.rgba[i + 1] = mix(g, m_image.rgba[i + 1]);
    m_image.rgba[i + 2] = mix(b, m_image.rgba[i + 2]);
    m_image.rgba[i + 3] = static_cast<uint8_t>(std::clamp(outA * 255.0f, 0.0f, 255.0f));
}

void Neutral::drawCloud()
{
    const float maxDensity = m_cloud.empty() ? 0.0f : *std::max_element(m_cloud.begin(), m_cloud.end());
    if (maxDensity <= 0.0f) {
        return;
    }
    const float scale = m_settings.gain / maxDensity;
    for (int y = 0; y < m_imageSize; ++y) {
        for (int x = 0; x < m_imageSize; ++x) {
            const float density = m_cloud[static_cast<std::size_t>(y) * m_imageSize + x];
            if (density <= 0.0f) {
                continue;
            }
            const float alpha = std::min(1.0f, density * scale) * (CloudMaxAlpha / 255.0f);
            blendPixel(x, y, CloudR, CloudG, CloudB, alpha);
        }
    }
}

void Neutral::drawDot()
{
    if (!m_hasData) {
        return;
    }
    const float cx = m_average.x * static_cast<float>(m_imageSize - 1);
    const float cy = m_average.y * static_cast<float>(m_imageSize - 1);
    const float radius = std::max(2.5f, static_cast<float>(m_imageSize) * DotRadiusFraction);
    const float ring = radius + DotRingWidth;
    const int lo = static_cast<int>(std::floor(-ring));
    const int hi = static_cast<int>(std::ceil(ring));
    for (int dy = lo; dy <= hi; ++dy) {
        for (int dx = lo; dx <= hi; ++dx) {
            const float distance = std::hypot(static_cast<float>(dx), static_cast<float>(dy));
            const int x = static_cast<int>(std::lround(cx)) + dx;
            const int y = static_cast<int>(std::lround(cy)) + dy;
            if (distance <= ring) {
                // The dark ring first, then the bright fill with a 1px feather.
                blendPixel(x, y, 12, 14, 18, std::clamp(ring - distance, 0.0f, 1.0f));
                blendPixel(x, y, 245, 246, 250, std::clamp(radius - distance + 0.5f, 0.0f, 1.0f));
            }
        }
    }
}

void Neutral::renderImage()
{
    std::fill(m_image.rgba.begin(), m_image.rgba.end(), static_cast<uint8_t>(0));
    drawCloud();
    drawDot();
    ++m_image.sequence;
}

NeutralGraticule buildNeutralGraticule()
{
    const NormalizedPoint center{0.5f, 0.5f};
    NeutralGraticule graticule;
    graticule.lines.push_back({{0.0f, 0.5f}, {1.0f, 0.5f}, GraticuleStroke::GridMajor});
    graticule.lines.push_back({{0.5f, 0.0f}, {0.5f, 1.0f}, GraticuleStroke::GridMajor});
    graticule.circles.push_back({center, 0.25f, GraticuleStroke::Grid});
    graticule.circles.push_back({center, 0.5f, GraticuleStroke::Grid});
    graticule.labels.push_back({{0.87f, 0.5f}, "Warm"});
    graticule.labels.push_back({{0.13f, 0.5f}, "Cool"});
    graticule.labels.push_back({{0.5f, 0.09f}, "Magenta"});
    graticule.labels.push_back({{0.5f, 0.91f}, "Green"});

    return graticule;
}

}  // namespace sidescopes
