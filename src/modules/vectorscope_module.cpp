// The vectorscope behind the module boundary: the hourglass shim that
// wraps the C++ engine in the C vtable. The engine stays idiomatic C++;
// only this file speaks both languages, and no exception ever crosses.

#include <cstdio>
#include <cstring>
#include <string>

#include "core/scopes/graticule.h"
#include "core/scopes/vectorscope.h"
#include "modules/module_export.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

struct VectorscopeInstance
{
    SsScopeInstance vtable{};
    Vectorscope engine;
    VectorscopeSettings settings;
};

VectorscopeInstance* impl(SsScopeInstance* instance)
{
    return static_cast<VectorscopeInstance*>(instance->instance_data);
}

const VectorscopeInstance* impl(const SsScopeInstance* instance)
{
    return static_cast<const VectorscopeInstance*>(instance->instance_data);
}

bool configure(SsScopeInstance* instance, const SsParamValue* values, uint32_t count)
{
    try {
        VectorscopeInstance* self = impl(instance);
        for (uint32_t index = 0; index < count; ++index) {
            const SsParamValue& value = values[index];
            if (std::strcmp(value.key, "gain") == 0) {
                self->settings.gain = static_cast<float>(value.value);
            } else if (std::strcmp(value.key, "stride") == 0) {
                self->settings.samplingStride = static_cast<int>(value.value);
            } else if (std::strcmp(value.key, "matrix") == 0) {
                self->settings.matrix = value.value < 0.5 ? ChromaMatrix::Bt601 : ChromaMatrix::Bt709;
            } else if (std::strcmp(value.key, "response") == 0) {
                self->settings.response = value.value < 0.5 ? TraceResponse::Boosted : TraceResponse::Linear;
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

uint32_t strokeOf(GraticuleStroke stroke)
{
    switch (stroke) {
    case GraticuleStroke::GridMajor:
        return SS_STROKE_GRID_MAJOR;
    case GraticuleStroke::Accent:
        return SS_STROKE_ACCENT;
    case GraticuleStroke::SkinTone:
        return SS_STROKE_SKIN_TONE;
    case GraticuleStroke::Grid:
        break;
    }
    return SS_STROKE_GRID;
}

uint32_t graticule(const SsScopeInstance* instance, SsGraticulePrimitive* primitives, uint32_t capacity)
{
    try {
        const VectorscopeGraticule layout = buildVectorscopeGraticule(impl(instance)->engine);
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
            primitive.stroke = strokeOf(line.stroke);
            primitive.x0 = line.from.x;
            primitive.y0 = line.from.y;
            primitive.x1 = line.to.x;
            primitive.y1 = line.to.y;
            emit(primitive);
        }

        for (const GraticuleCircle& circle : layout.circles) {
            SsGraticulePrimitive primitive{};
            primitive.kind = SS_PRIMITIVE_CIRCLE;
            primitive.stroke = strokeOf(circle.stroke);
            primitive.x0 = circle.center.x;
            primitive.y0 = circle.center.y;
            primitive.x1 = circle.radius;
            emit(primitive);
        }

        for (const GraticuleTarget& target : layout.targets) {
            SsGraticulePrimitive primitive{};
            primitive.kind = SS_PRIMITIVE_TARGET_BOX;
            primitive.stroke = SS_STROKE_ACCENT;
            primitive.x0 = target.center.x;
            primitive.y0 = target.center.y;
            if (target.primary) {
                primitive.flags |= SS_PRIMITIVE_FLAG_TARGET_PRIMARY;
            }
            std::snprintf(primitive.label, sizeof(primitive.label), "%s", target.label.c_str());
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
        VectorscopeInstance* self = impl(instance);
        self->settings.size = height;
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

const char* const MatrixChoices[] = {"BT.601", "BT.709", nullptr};
const char* const ResponseChoices[] = {"Boosted", "Linear", nullptr};

const SsParamInfo Params[] = {
    {"gain", "Intensity", SS_PARAM_INTENSITY, 0.0, 0.0, 3.0, 20.0, nullptr, nullptr},
    {"stride", "Sampling stride", SS_PARAM_INT, 1.0, 8.0, 1.0, 0.0, nullptr, nullptr},
    {"matrix", "Matrix", SS_PARAM_CHOICE, 0.0, 1.0, 1.0, 0.0, "Vectorscope Matrix", MatrixChoices},
    {"response", "Trace response", SS_PARAM_CHOICE, 0.0, 1.0, 0.0, 0.0, "Trace Response", ResponseChoices},
};

const SsScopeDescriptor VectorscopeDescriptor{
    "org.sidescopes.vectorscope",
    "Vectorscope",
    'V',
    Vectorscope::CodeGridSize,
    Vectorscope::CodeGridSize,
    SS_SCOPE_KEEP_ASPECT | SS_SCOPE_PIN_TARGET,
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
    return index == 0 ? &VectorscopeDescriptor : nullptr;
}

SsScopeInstance* create(const char* scopeId, const SsHost*)
{
    try {
        if (std::strcmp(scopeId, VectorscopeDescriptor.id) != 0) {
            return nullptr;
        }

        auto* self = new VectorscopeInstance;
        // Push the constructed defaults through the engine explicitly, so the
        // instance never relies on the engine's own ctor defaults matching.
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

const SsModuleEntry VectorscopeModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, moduleInit, moduleDeinit, scopeCount, descriptor, create,
};

#ifdef SIDESCOPES_MODULE_DYNAMIC
// The loader finds this by name; it aliases the same entry the static
// registry uses.
extern "C" SS_MODULE_EXPORT const SsModuleEntry ss_module_entry = VectorscopeModuleEntry;
#endif

}  // namespace sidescopes
