// The neutral / white-balance-cast scope behind the module boundary: the
// hourglass shim wrapping the C++ engine in the C vtable. The engine stays
// idiomatic C++; only this file speaks both languages, and no exception ever
// crosses.

#include <cstdio>
#include <cstring>

#include "core/scopes/neutral.h"
#include "modules/module_export.h"
#include "modules/module_registry.h"
#include "modules/module_scratch.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

struct NeutralInstance
{
    SsScopeInstance vtable{};
    Neutral engine;
    NeutralSettings settings;
    /// The host that created this instance, kept for the shared
    /// accumulation arena it lends. Null when there is none.
    const SsHost* host = nullptr;
};

NeutralInstance* impl(SsScopeInstance* instance)
{
    return static_cast<NeutralInstance*>(instance->instance_data);
}

const NeutralInstance* impl(const SsScopeInstance* instance)
{
    return static_cast<const NeutralInstance*>(instance->instance_data);
}

bool configure(SsScopeInstance* instance, const SsParamValue* values, uint32_t count)
{
    try {
        NeutralInstance* self = impl(instance);
        for (uint32_t index = 0; index < count; ++index) {
            const SsParamValue& value = values[index];
            if (std::strcmp(value.key, "gain") == 0) {
                self->settings.gain = static_cast<float>(value.value);
            } else if (std::strcmp(value.key, "stride") == 0) {
                self->settings.samplingStride = static_cast<int>(value.value);
            } else if (std::strcmp(value.key, "neutral") == 0) {
                self->settings.neutralChroma = static_cast<float>(value.value);
            } else if (std::strcmp(value.key, "range") == 0) {
                self->settings.range = value.value < 0.5   ? NeutralRange::Fine
                                       : value.value < 1.5 ? NeutralRange::Normal
                                                           : NeutralRange::Wide;
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
        lendHostScratch(impl(instance)->engine, impl(instance)->host);
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
        const NeutralGraticule layout = buildNeutralGraticule();
        uint32_t needed = 0;
        const auto emit = [&](const SsGraticulePrimitive& primitive) {
            if (needed < capacity) {
                primitives[needed] = primitive;
            }
            ++needed;
        };
        for (const GraticuleLine& line : layout.lines) {
            SsGraticulePrimitive primitive{};
            primitive.kind = SS_PRIMITIVE_LINE;
            primitive.stroke = line.stroke == GraticuleStroke::GridMajor ? SS_STROKE_GRID_MAJOR : SS_STROKE_GRID;
            primitive.x0 = line.from.x;
            primitive.y0 = line.from.y;
            primitive.x1 = line.to.x;
            primitive.y1 = line.to.y;
            emit(primitive);
        }

        for (const GraticuleCircle& circle : layout.circles) {
            SsGraticulePrimitive primitive{};
            primitive.kind = SS_PRIMITIVE_CIRCLE;
            primitive.stroke = SS_STROKE_GRID;
            primitive.x0 = circle.center.x;
            primitive.y0 = circle.center.y;
            primitive.x1 = circle.radius;
            emit(primitive);
        }

        for (const NeutralLabel& label : layout.labels) {
            SsGraticulePrimitive primitive{};
            primitive.kind = SS_PRIMITIVE_TEXT;
            primitive.stroke = SS_STROKE_GRID;
            primitive.x0 = label.at.x;
            primitive.y0 = label.at.y;
            primitive.flags = SS_PRIMITIVE_FLAG_TEXT_MAJOR_ONLY;
            std::snprintf(primitive.label, sizeof(primitive.label), "%s", label.text.c_str());
            emit(primitive);
        }
        return needed;
    } catch (...) {
        return 0;
    }
}

uint32_t markers(const SsScopeInstance* instance, SsColor color, SsMarker* out, uint32_t capacity)
{
    try {
        const NormalizedPoint point = impl(instance)->engine.project(FloatColor{color.r, color.g, color.b});
        if (capacity >= 1) {
            SsMarker marker{};
            marker.kind = SS_MARKER_POINT;
            marker.x = point.x;
            marker.y = point.y;
            marker.band_from = 0.0f;
            marker.band_to = 1.0f;
            marker.channel_mask = 0x7;
            out[0] = marker;
        }
        return 1;
    } catch (...) {
        return 0;
    }
}

void setImageSize(SsScopeInstance* instance, int32_t, int32_t height)
{
    try {
        NeutralInstance* self = impl(instance);
        self->settings.size = height;
        self->engine.configure(self->settings);
    } catch (...) {
        // The boundary must not throw; a failed resize keeps the previous plane.
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

const char* const RangeChoices[] = {"Fine", "Normal", "Wide", nullptr};

const SsParamInfo Params[] = {
    {"gain", "Intensity", SS_PARAM_INTENSITY, 0.0, 0.0, 1.0, 20.0, nullptr, nullptr},
    {"stride", "Sampling stride", SS_PARAM_INT, 1.0, 8.0, 1.0, 0.0, nullptr, nullptr},
    {"neutral", "Neutral threshold", SS_PARAM_INT, 4.0, 30.0, 12.0, 0.0, nullptr, nullptr},
    {"range", "Range", SS_PARAM_CHOICE, 0.0, 2.0, 1.0, 0.0, "Cast Range", RangeChoices},
};

const SsScopeDescriptor NeutralDescriptor{
    "org.sidescopes.neutral",
    "Neutral",
    'N',
    Neutral::CodeGridSize,
    Neutral::CodeGridSize,
    SS_SCOPE_KEEP_ASPECT,
    Params,
    static_cast<uint32_t>(sizeof(Params) / sizeof(Params[0])),
    1.0f,
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
    return 1;
}

const SsScopeDescriptor* descriptor(uint32_t index)
{
    return index == 0 ? &NeutralDescriptor : nullptr;
}

SsScopeInstance* create(const char* scopeId, const SsHost* host)
{
    try {
        if (std::strcmp(scopeId, NeutralDescriptor.id) != 0) {
            return nullptr;
        }

        auto* self = new NeutralInstance;
        self->engine.configure(self->settings);
        self->host = host;
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

const SsModuleEntry NeutralModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, moduleInit, moduleDeinit, scopeCount, descriptor, create,
};

#ifdef SIDESCOPES_MODULE_DYNAMIC
// The loader finds this by name; it aliases the same entry the static registry
// uses.
extern "C" SS_MODULE_EXPORT const SsModuleEntry ss_module_entry = NeutralModuleEntry;
#endif

}  // namespace sidescopes
