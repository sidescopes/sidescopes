// The RGB histogram behind the module boundary. Alongside the adaptive
// image size every scope carries, it exports the outline extension: the
// engine's normalized curve heights, which the host strokes at display
// resolution over the filled texture. The engine stays idiomatic C++;
// only this file speaks both languages, and no exception ever crosses.

#include <algorithm>
#include <cstring>
#include <string>

#include "core/scopes/histogram.h"
#include "modules/module_export.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

struct HistogramInstance
{
    SsScopeInstance vtable{};
    Histogram engine;
    HistogramSettings settings;
};

HistogramInstance* impl(SsScopeInstance* instance)
{
    return static_cast<HistogramInstance*>(instance->instance_data);
}

const HistogramInstance* impl(const SsScopeInstance* instance)
{
    return static_cast<const HistogramInstance*>(instance->instance_data);
}

bool configure(SsScopeInstance* instance, const SsParamValue* values, uint32_t count)
{
    try {
        HistogramInstance* self = impl(instance);
        for (uint32_t index = 0; index < count; ++index) {
            const SsParamValue& value = values[index];
            if (std::strcmp(value.key, "stride") == 0) {
                self->settings.samplingStride = static_cast<int>(value.value);
            } else if (std::strcmp(value.key, "style") == 0) {
                // Choice 0 is the per-channel default; 1 overlays the
                // channels additively into one combined plot.
                self->settings.style = value.value < 0.5 ? HistogramStyle::PerChannel : HistogramStyle::Combined;
            }
        }
        self->engine.configure(self->settings);

        return true;
    } catch (...) {
        return false;
    }
}

bool accumulate(SsScopeInstance* instance, const SsFrameView* frame, SsRect region)
{
    try {
        const FrameView view{frame->bgra,
                             frame->stride_bytes,
                             frame->width,
                             frame->height,
                             frame->color_space == SS_COLOR_SPACE_SRGB ? ColorSpaceHint::Srgb : ColorSpaceHint::Unknown,
                             frame->sequence};
        impl(instance)->engine.accumulate(view, IntRect{region.x, region.y, region.width, region.height});

        return true;
    } catch (...) {
        return false;
    }
}

SsImageView image(const SsScopeInstance* instance)
{
    const ScopeImage& image = impl(instance)->engine.image();

    return SsImageView{image.rgba.data(), image.width, image.height, image.sequence};
}

uint32_t graticule(const SsScopeInstance*, SsGraticulePrimitive* primitives, uint32_t capacity)
{
    try {
        uint32_t needed = 0;
        const auto emit = [&](const SsGraticulePrimitive& primitive) {
            if (needed < capacity) {
                primitives[needed] = primitive;
            }
            ++needed;
        };
        // Verticals at every quarter; the quarters at 0/50/100 are major.
        for (int quarter = 0; quarter <= 4; ++quarter) {
            const float x = static_cast<float>(quarter) / 4.0f;
            SsGraticulePrimitive line{};
            line.kind = SS_PRIMITIVE_LINE;
            line.stroke = quarter % 2 == 0 ? SS_STROKE_GRID_MAJOR : SS_STROKE_GRID;
            line.x0 = x;
            line.y0 = 0.0f;
            line.x1 = x;
            line.y1 = 1.0f;
            emit(line);
        }

        return needed;
    } catch (...) {
        return 0;
    }
}

uint32_t markers(const SsScopeInstance* instance, SsColor color, SsMarker* markers, uint32_t capacity)
{
    try {
        const bool bands = impl(instance)->settings.style == HistogramStyle::PerChannel;
        const float channels[3] = {color.r, color.g, color.b};
        for (uint32_t channel = 0; channel < 3; ++channel) {
            if (channel >= capacity) {
                break;
            }
            SsMarker marker{};
            marker.kind = SS_MARKER_VALUE;
            marker.x = channels[channel] / 255.0f;
            // Per-channel stacks the channels into thirds; combined draws
            // them across the full height. The host reads the band as a
            // vertical confinement for value markers.
            marker.band_from = bands ? static_cast<float>(channel) / 3.0f : 0.0f;
            marker.band_to = bands ? static_cast<float>(channel + 1) / 3.0f : 1.0f;
            marker.channel_mask = 1u << channel;
            markers[channel] = marker;
        }

        return 3;
    } catch (...) {
        return 0;
    }
}

void setImageSize(SsScopeInstance* instance, int32_t width, int32_t height)
{
    try {
        HistogramInstance* self = impl(instance);
        self->settings.imageWidth = width;
        self->settings.imageHeight = height;
        self->engine.configure(self->settings);
    } catch (...) {
    }
}

constexpr SsAdaptiveImageExtension AdaptiveImage{setImageSize};

uint32_t outlineHeights(const SsScopeInstance* instance, float* out, uint32_t capacity)
{
    try {
        const std::vector<float>& heights = impl(instance)->engine.outlineHeights();
        const uint32_t total = static_cast<uint32_t>(heights.size());
        const uint32_t copied = std::min(total, capacity);
        for (uint32_t index = 0; index < copied; ++index) {
            out[index] = heights[index];
        }

        return total;
    } catch (...) {
        return 0;
    }
}

constexpr SsOutlineExtension Outline{outlineHeights};

const void* getExtension(const SsScopeInstance*, const char* id)
{
    if (std::strcmp(id, AdaptiveImageExtension) == 0) {
        return &AdaptiveImage;
    }
    if (std::strcmp(id, OutlineExtension) == 0) {
        return &Outline;
    }

    return nullptr;
}

void destroy(SsScopeInstance* instance)
{
    delete impl(instance);
}

const char* const StyleChoices[] = {"Per Channel", "Combined", nullptr};

const SsParamInfo Params[] = {
    {"stride", "Sampling stride", SS_PARAM_INT, 1.0, 8.0, 1.0, 0.0, nullptr, nullptr},
    {"style", "Style", SS_PARAM_CHOICE, 0.0, 1.0, 0.0, 0.0, "Histogram Style", StyleChoices},
};

const SsScopeDescriptor HistogramDescriptor{
    "org.sidescopes.histogram", "Histogram", 'H',    Histogram::ImageWidth,
    Histogram::Height,          0u,          Params, static_cast<uint32_t>(sizeof(Params) / sizeof(Params[0])),
};

bool moduleInit(void)
{
    return true;
}

void moduleDeinit(void)
{
}

uint32_t scopeCount(void)
{
    return 1;
}

const SsScopeDescriptor* descriptor(uint32_t index)
{
    return index == 0 ? &HistogramDescriptor : nullptr;
}

SsScopeInstance* create(const char* scopeId, const SsHost*)
{
    try {
        if (std::strcmp(scopeId, HistogramDescriptor.id) != 0) {
            return nullptr;
        }

        auto* self = new HistogramInstance;
        self->vtable.instance_data = self;
        self->vtable.configure = configure;
        self->vtable.accumulate = accumulate;
        self->vtable.image = image;
        self->vtable.graticule = graticule;
        self->vtable.markers = markers;
        self->vtable.get_extension = getExtension;
        self->vtable.destroy = destroy;

        return &self->vtable;
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

const SsModuleEntry HistogramModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, moduleInit, moduleDeinit, scopeCount, descriptor, create,
};

#ifdef SIDESCOPES_MODULE_DYNAMIC
// The loader finds this by name; it aliases the same entry the static
// registry uses.
extern "C" SS_MODULE_EXPORT const SsModuleEntry ss_module_entry = HistogramModuleEntry;
#endif

}  // namespace sidescopes
