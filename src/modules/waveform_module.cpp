// One module, two scopes: the waveform monitor and the RGB parade both
// wrap the same engine behind the C vtable. The parade is the waveform
// engine pinned to its side-by-side mode, so it needs no parameter for
// what it always is. The engine stays idiomatic C++; only this file
// speaks both languages, and no exception ever crosses.

#include <cstdio>
#include <cstring>
#include <string>

#include "core/scopes/graticule.h"
#include "core/scopes/waveform.h"
#include "modules/module_export.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

// The two scopes share every vtable function; only which fixed mode the
// parade forces and which marker layout each draws differs, so a single
// instance type carries a flag rather than duplicating the shim.
struct WaveformInstance
{
    SsScopeInstance vtable{};
    Waveform engine;
    WaveformSettings settings;
    bool parade = false;
};

WaveformInstance* impl(SsScopeInstance* instance)
{
    return static_cast<WaveformInstance*>(instance->instance_data);
}

const WaveformInstance* impl(const SsScopeInstance* instance)
{
    return static_cast<const WaveformInstance*>(instance->instance_data);
}

WaveformMode modeOf(double choice)
{
    switch (static_cast<int>(choice)) {
    case 1:
        return WaveformMode::Luma;
    case 2:
        return WaveformMode::ColoredLuma;
    default:
        return WaveformMode::Rgb;
    }
}

bool configure(SsScopeInstance* instance, const SsParamValue* values, uint32_t count)
{
    try {
        WaveformInstance* self = impl(instance);
        for (uint32_t index = 0; index < count; ++index) {
            const SsParamValue& value = values[index];
            if (std::strcmp(value.key, "gain") == 0) {
                self->settings.gain = static_cast<float>(value.value);
            } else if (std::strcmp(value.key, "stride") == 0) {
                self->settings.samplingStride = static_cast<int>(value.value);
            } else if (std::strcmp(value.key, "mode") == 0 && !self->parade) {
                self->settings.mode = modeOf(value.value);
            }
        }

        // The parade is defined by its mode; no configuration path may
        // move it off the side-by-side layout it exists to show.
        if (self->parade) {
            self->settings.mode = WaveformMode::RgbParade;
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
        for (const WaveformScaleLine& line : buildWaveformScale()) {
            const uint32_t stroke = line.major ? SS_STROKE_GRID_MAJOR : SS_STROKE_GRID;
            SsGraticulePrimitive scale{};
            scale.kind = SS_PRIMITIVE_LINE;
            scale.stroke = stroke;
            scale.x0 = 0.0f;
            scale.y0 = line.y;
            scale.x1 = 1.0f;
            scale.y1 = line.y;
            emit(scale);
            SsGraticulePrimitive text{};
            text.kind = SS_PRIMITIVE_TEXT;
            text.stroke = stroke;
            text.x0 = 0.0f;
            text.y0 = line.y;
            // Minor labels crowd a small pane, so they carry the flag that
            // asks the host to draw them only where there is room.
            if (!line.major) {
                text.flags |= SS_PRIMITIVE_FLAG_TEXT_MAJOR_ONLY;
            }
            std::snprintf(text.label, sizeof(text.label), "%s", line.label.c_str());
            emit(text);
        }
        return needed;
    } catch (...) {
        return 0;
    }
}

// One level marker per channel at the channel's own value: the level of a
// channel is its value, mirroring the engine's own per-channel placement.
uint32_t channelLevels(SsColor color, SsMarker* out, uint32_t capacity, bool parade)
{
    const float channels[3] = {color.r, color.g, color.b};
    for (uint32_t channel = 0; channel < 3; ++channel) {
        if (channel >= capacity) {
            break;
        }
        SsMarker marker{};
        marker.kind = SS_MARKER_LEVEL;
        marker.y = (255.0f - channels[channel]) / 255.0f;
        // The parade splits the channels into thirds across the width, so
        // each marker stays inside its own column; the overlaid waveform
        // spans the full width.
        marker.band_from = parade ? static_cast<float>(channel) / 3.0f : 0.0f;
        marker.band_to = parade ? static_cast<float>(channel + 1) / 3.0f : 1.0f;
        marker.channel_mask = 1u << channel;
        out[channel] = marker;
    }
    return 3;
}

uint32_t markers(const SsScopeInstance* instance, SsColor color, SsMarker* out, uint32_t capacity)
{
    try {
        const WaveformInstance* self = impl(instance);
        if (self->parade) {
            return channelLevels(color, out, capacity, true);
        }

        // Luma flavors carry a single level line at the color's luma; the
        // RGB overlay carries one line per channel.
        if (self->settings.mode == WaveformMode::Luma || self->settings.mode == WaveformMode::ColoredLuma) {
            const NormalizedPoint point = self->engine.project(FloatColor{color.r, color.g, color.b});
            if (capacity >= 1) {
                SsMarker marker{};
                marker.kind = SS_MARKER_LEVEL;
                marker.y = point.y;
                marker.band_from = 0.0f;
                marker.band_to = 1.0f;
                marker.channel_mask = 0x7;
                out[0] = marker;
            }
            return 1;
        }
        return channelLevels(color, out, capacity, false);
    } catch (...) {
        return 0;
    }
}

void setImageSize(SsScopeInstance* instance, int32_t width, int32_t height)
{
    try {
        WaveformInstance* self = impl(instance);
        self->settings.columns = width;
        self->settings.imageHeight = height;
        self->engine.configure(self->settings);
    } catch (...) {
        // The boundary must not throw; a failed resize keeps the previous grid.
        return;
    }
}

constexpr SsAdaptiveImageExtension AdaptiveImage{setImageSize};

const void* getExtension(const SsScopeInstance*, const char* id)
{
    if (std::strcmp(id, AdaptiveImageExtension) == 0) {
        return &AdaptiveImage;
    }
    return nullptr;
}

void destroy(SsScopeInstance* instance)
{
    delete impl(instance);
}

const char* const ModeChoices[] = {"RGB", "Luma", "Luma (Colored)", nullptr};

const SsParamInfo WaveformParams[] = {
    {"gain", "Intensity", SS_PARAM_INTENSITY, 0.0, 0.0, 0.05, 0.0, nullptr, nullptr},
    {"stride", "Sampling stride", SS_PARAM_INT, 1.0, 8.0, 1.0, 0.0, nullptr, nullptr},
    {"mode", "Style", SS_PARAM_CHOICE, 0.0, 2.0, 0.0, 0.0, "Waveform Style", ModeChoices},
};

const SsParamInfo ParadeParams[] = {
    {"gain", "Intensity", SS_PARAM_INTENSITY, 0.0, 0.0, 0.05, 0.0, nullptr, nullptr},
    {"stride", "Sampling stride", SS_PARAM_INT, 1.0, 8.0, 1.0, 0.0, nullptr, nullptr},
};

const SsScopeDescriptor WaveformDescriptor{
    "org.sidescopes.waveform",
    "Waveform",
    'W',
    Waveform::Columns,
    Waveform::Levels,
    0u,
    WaveformParams,
    static_cast<uint32_t>(sizeof(WaveformParams) / sizeof(WaveformParams[0])),
    3.0f,
};

const SsScopeDescriptor ParadeDescriptor{
    "org.sidescopes.parade",
    "RGB Parade",
    'R',
    Waveform::Columns,
    Waveform::Levels,
    0u,
    ParadeParams,
    static_cast<uint32_t>(sizeof(ParadeParams) / sizeof(ParadeParams[0])),
    3.0f,
};

bool moduleInit()
{
    return true;
}

void moduleDeinit()
{
}

uint32_t scopeCount()
{
    return 2;
}

const SsScopeDescriptor* descriptor(uint32_t index)
{
    if (index == 0) {
        return &WaveformDescriptor;
    }
    if (index == 1) {
        return &ParadeDescriptor;
    }
    return nullptr;
}

SsScopeInstance* create(const char* scopeId, const SsHost*)
{
    try {
        const bool waveform = std::strcmp(scopeId, WaveformDescriptor.id) == 0;
        const bool parade = std::strcmp(scopeId, ParadeDescriptor.id) == 0;
        if (!waveform && !parade) {
            return nullptr;
        }

        auto* self = new WaveformInstance;
        self->parade = parade;
        if (parade) {
            self->settings.mode = WaveformMode::RgbParade;
        }
        self->engine.configure(self->settings);
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

const SsModuleEntry WaveformModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, moduleInit, moduleDeinit, scopeCount, descriptor, create,
};

#ifdef SIDESCOPES_MODULE_DYNAMIC
// The loader finds this by name; it aliases the same entry the static
// registry uses.
extern "C" SS_MODULE_EXPORT const SsModuleEntry ss_module_entry = WaveformModuleEntry;
#endif

}  // namespace sidescopes
