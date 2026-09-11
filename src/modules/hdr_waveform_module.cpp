#include "modules/hdr_waveform_module.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "core/scopes/scope_types.h"
#include "modules/module_export.h"
#include "modules/module_frame.h"
#include "modules/module_params.h"
#include "modules/module_registry.h"

namespace sidescopes {
namespace {

constexpr int Columns = 512;
constexpr int Levels = 385;  // includes an exact central row for SDR white

struct HdrInstance
{
    SsScopeInstance vtable{};
    ScopeImage image;
    std::vector<uint32_t> bins;
    std::array<char, 160> reading{};
    double gain = 0.05;
    int columns = Columns;
    int levels = Levels;
};

HdrInstance* impl(SsScopeInstance* instance)
{
    return static_cast<HdrInstance*>(instance->instance_data);
}

const HdrInstance* impl(const SsScopeInstance* instance)
{
    return static_cast<const HdrInstance*>(instance->instance_data);
}

bool configure(SsScopeInstance* instance, const SsParamValue* values, uint32_t count)
{
    if (!validParameters(values, count)) {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (std::strcmp(values[index].key, "gain") == 0) {
            impl(instance)->gain = parameterGain(values[index].value);
        }
    }
    return true;
}

struct Reading
{
    float peak = 0.0f;
    uint64_t valid = 0;
    uint64_t above = 0;
    uint64_t invalid = 0;
};

Reading scatter(HdrInstance& self, const FrameView& frame, IntRect region)
{
    Reading reading;
    for (int y = region.y; y < region.y + region.height; ++y) {
        const float* row = frame.hdrLuminance + static_cast<std::size_t>(y) * frame.width;
        for (int x = 0; x < region.width; ++x) {
            const float value = row[region.x + x];
            if (!std::isfinite(value) || value < 0.0f) {
                ++reading.invalid;
                continue;
            }
            ++reading.valid;
            reading.above += value > 1.0f ? 1 : 0;
            reading.peak = std::max(reading.peak, value);
            const float stops = std::log2(std::clamp(value, 1.0f / 64.0f, 64.0f));
            const int level =
                static_cast<int>(std::lround((6.0f - stops) * static_cast<float>(self.levels - 1) / 12.0f));
            const int column = static_cast<int>(static_cast<int64_t>(x) * self.columns / region.width);
            ++self.bins[static_cast<std::size_t>(level) * self.columns + column];
        }
    }
    return reading;
}

void compose(HdrInstance& self)
{
    for (std::size_t bin = 0; bin < self.bins.size(); ++bin) {
        const double density = 1.0 - std::exp(-self.gain * self.bins[bin]);
        const bool highlight =
            bin / static_cast<std::size_t>(self.columns) < static_cast<std::size_t>((self.levels - 1) / 2);
        const auto ink = static_cast<uint8_t>(std::lround(255.0 * density));
        uint8_t* pixel = self.image.rgba.data() + bin * 4;
        pixel[0] = ink;
        pixel[1] = highlight ? static_cast<uint8_t>(ink * 0.78) : ink;
        pixel[2] = highlight ? static_cast<uint8_t>(ink * 0.38) : ink;
        pixel[3] = 255;
    }
    ++self.image.sequence;
}

void describe(HdrInstance& self, const Reading& reading)
{
    if (reading.valid == 0) {
        std::snprintf(self.reading.data(), self.reading.size(), "HDR luminance: no valid pixels");
        return;
    }
    char peak[48]{};
    if (reading.peak > 0.0f) {
        const double stops = std::log2(reading.peak);
        std::snprintf(peak, sizeof(peak), "Peak %+.2f stops", std::abs(stops) < 0.005 ? 0.0 : stops);
    } else {
        std::snprintf(peak, sizeof(peak), "Peak black");
    }
    std::snprintf(self.reading.data(), self.reading.size(), "%s | %.2f%% above white%s%s", peak,
                  100.0 * static_cast<double>(reading.above) / static_cast<double>(reading.valid),
                  reading.peak > 64.0f ? " | above plot range" : "",
                  reading.invalid > 0 ? " | invalid pixels excluded" : "");
}

bool accumulate(SsScopeInstance* instance, const SsFrameView* frame, SsRect rectangle)
{
    if (!frame || !validBoundaryFrame(*frame)) {
        return false;
    }
    try {
        HdrInstance& self = *impl(instance);
        self.image.width = self.columns;
        self.image.height = self.levels;
        self.image.rgba.resize(static_cast<std::size_t>(self.columns) * self.levels * 4);
        self.bins.assign(static_cast<std::size_t>(self.columns) * self.levels, 0);
        const FrameView view = frameFromBoundary(*frame);
        const IntRect region =
            IntRect{rectangle.x, rectangle.y, rectangle.width, rectangle.height}.clampedTo(view.width, view.height);
        if (!view.hdrLuminance) {
            std::snprintf(self.reading.data(), self.reading.size(), "HDR luminance unavailable for this capture");
        } else {
            describe(self, scatter(self, view, region));
        }
        compose(self);
        return true;
    } catch (...) {
        return false;
    }
}

SsImageView image(const SsScopeInstance* instance)
{
    const auto& image = impl(instance)->image;
    return {image.rgba.data(), image.width, image.height, image.sequence};
}

uint32_t graticule(const SsScopeInstance*, SsGraticulePrimitive* out, uint32_t capacity)
{
    uint32_t count = 0;
    for (int stops = -6; stops <= 6; stops += 2) {
        SsGraticulePrimitive line{};
        line.kind = SS_PRIMITIVE_LINE;
        line.stroke = stops == 0 ? SS_STROKE_ACCENT : SS_STROKE_GRID;
        line.y0 = line.y1 = (6.0f - static_cast<float>(stops)) / 12.0f;
        line.x1 = 1.0f;
        if (count < capacity) {
            out[count] = line;
        }
        ++count;
        line.kind = SS_PRIMITIVE_TEXT;
        if (stops != 0 && stops != -6 && stops != 6) {
            line.flags |= SS_PRIMITIVE_FLAG_TEXT_MAJOR_ONLY;
        }
        if (stops == 0) {
            std::snprintf(line.label, sizeof(line.label), "0 SDR white");
        } else {
            std::snprintf(line.label, sizeof(line.label), "%s%+d stops",
                          stops == -6  ? "<="
                          : stops == 6 ? ">="
                                       : "",
                          stops);
        }
        if (count < capacity) {
            out[count] = line;
        }
        ++count;
    }
    return count;
}

uint32_t markers(const SsScopeInstance*, SsColor, SsMarker*, uint32_t)
{
    // An SDR swatch has lost highlight information. Projecting it onto an
    // HDR instrument would imply a measurement that the swatch cannot carry.
    return 0;
}

const char* reading(const SsScopeInstance* instance)
{
    return impl(instance)->reading.data();
}

constexpr SsReadingExtension ReadingExtensionVtable{reading};

void setImageSize(SsScopeInstance* instance, int32_t width, int32_t height)
{
    impl(instance)->columns = std::clamp(width, 32, 2048);
    impl(instance)->levels = std::clamp(height | 1, 33, 1025);
}

constexpr SsAdaptiveImageExtension AdaptiveImage{setImageSize};

const void* extension(const SsScopeInstance*, const char* id)
{
    if (std::strcmp(id, AdaptiveImageExtension) == 0) {
        return &AdaptiveImage;
    }
    return std::strcmp(id, ReadingExtension) == 0 ? &ReadingExtensionVtable : nullptr;
}

void destroy(SsScopeInstance* instance)
{
    delete impl(instance);
}

const SsParamInfo Params[] = {
    {"gain", "Intensity", SS_PARAM_INTENSITY, 0.0, 0.0, 0.05, 0.0, nullptr, nullptr},
};

const SsScopeDescriptor Descriptor{
    "org.sidescopes.waveform.hdr", "HDR Luminance", 'E', Columns, Levels, 0u, Params, 1, 3.0f,
};

}  // namespace

const SsScopeDescriptor* hdrWaveformDescriptor()
{
    return &Descriptor;
}

SsScopeInstance* createHdrWaveform()
{
    try {
        auto self = std::make_unique<HdrInstance>();
        self->vtable = {self.get(), configure, accumulate, image, graticule, markers, extension, destroy};
        return &self.release()->vtable;
    } catch (...) {
        return nullptr;
    }
}

constexpr SsModuleEntry HdrModuleEntry{
    SS_ABI_MAJOR,
    SS_ABI_MINOR,
    [] { return true; },
    [] {},
    [] { return 1u; },
    [](uint32_t index) { return index == 0 ? &Descriptor : nullptr; },
    [](const char* id, const SsHost*) { return std::strcmp(id, Descriptor.id) == 0 ? createHdrWaveform() : nullptr; },
};

#ifdef SIDESCOPES_MODULE_DYNAMIC
extern "C" SS_MODULE_EXPORT const SsModuleEntry ss_module_entry = HdrModuleEntry;
#endif

}  // namespace sidescopes
