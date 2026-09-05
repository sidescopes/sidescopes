// One module, three scopes: the RGB waveform, the luma waveform and the
// RGB parade all wrap the same engine behind the C vtable. Each is the
// engine pinned to what it plots, so none needs a parameter for what it
// always is; the luma waveform alone carries a style, because plain and
// tinted luma are the same curve painted two ways. The engine stays
// idiomatic C++; only this file speaks both languages, and no exception
// ever crosses.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "core/scopes/graticule.h"
#include "core/scopes/waveform.h"
#include "core/scopes/waveform_bins.h"
#include "modules/module_export.h"
#include "modules/module_frame.h"
#include "modules/module_params.h"
#include "modules/module_registry.h"
#include "modules/module_scratch.h"
#include "modules/module_shared_state.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

/// The bins every waveform-family scope of one host scatters into.
///
/// The waveform and the parade are this module over the same region at the same
/// geometry - the application gives them one image size deliberately - so their
/// scatters agree bin for bin, and everything after the scatter genuinely
/// differs. Sharing one of these pays for it once.
struct SharedWaveformBins
{
    WaveformBins bins;
};

/// Which of the module's scopes an instance is. Each plots something the
/// others do not, and no configuration path may move one onto another's.
enum class WaveformScope
{
    Rgb,
    Luma,
    Parade,
};

// The three scopes share every vtable function; only what each is pinned to
// and which marker layout it draws differ, so a single instance type carries
// which scope it is rather than duplicating the shim.
struct WaveformInstance
{
    SsScopeInstance vtable{};
    Waveform engine;
    WaveformSettings settings;
    WaveformScope scope = WaveformScope::Rgb;
    /// The host that created this instance, kept for the shared
    /// accumulation arena it lends. Null when there is none.
    const SsHost* host = nullptr;
    /// The bins shared with this host's other waveform-family scopes, taken on
    /// the first pass. Held rather than looked up per pass, and held only by
    /// the instances that really accumulate: a projection instance never asks,
    /// so a stack with no region goes on holding no bins at all.
    std::shared_ptr<SharedWaveformBins> shared;
};

WaveformInstance* impl(SsScopeInstance* instance)
{
    return static_cast<WaveformInstance*>(instance->instance_data);
}

const WaveformInstance* impl(const SsScopeInstance* instance)
{
    return static_cast<const WaveformInstance*>(instance->instance_data);
}

/// The mode @p scope plots in. The luma waveform's @p colored style decides
/// only how its own trace is painted; the other two have no choice to make.
WaveformMode modeOf(WaveformScope scope, bool colored)
{
    switch (scope) {
    case WaveformScope::Luma:
        return colored ? WaveformMode::ColoredLuma : WaveformMode::Luma;
    case WaveformScope::Parade:
        return WaveformMode::RgbParade;
    case WaveformScope::Rgb:
    default:
        return WaveformMode::Rgb;
    }
}

bool configure(SsScopeInstance* instance, const SsParamValue* values, uint32_t count)
{
    if (!validParameters(values, count)) {
        return false;
    }
    try {
        WaveformInstance* self = impl(instance);
        bool colored = self->settings.mode == WaveformMode::ColoredLuma;
        for (uint32_t index = 0; index < count; ++index) {
            const SsParamValue& value = values[index];
            if (std::strcmp(value.key, "gain") == 0) {
                self->settings.gain = parameterGain(value.value);
            } else if (std::strcmp(value.key, "stride") == 0) {
                self->settings.samplingStride = static_cast<int>(std::clamp(value.value, 1.0, 8.0));
            } else if (std::strcmp(value.key, "style") == 0) {
                colored = value.value >= 0.5;
            }
        }

        // Each scope is defined by what it plots, so the mode is resolved from
        // the scope rather than carried: a stale key from a file written while
        // these were one scope's styles can never move one onto another's.
        self->settings.mode = modeOf(self->scope, colored);
        self->engine.configure(self->settings);
        return true;
    } catch (...) {
        return false;
    }
}

bool accumulate(SsScopeInstance* instance, const SsFrameView* frame, SsRect region)
{
    if (!frame || !validBoundaryFrame(*frame)) {
        return false;
    }
    try {
        WaveformInstance* self = impl(instance);
        const FrameView view = frameFromBoundary(*frame);
        if (!self->shared) {
            self->shared = sharedStateFor<SharedWaveformBins>(self->host);
            self->engine.lendBins(self->shared ? &self->shared->bins : nullptr);
        }
        // After the bins, so the arena reaches the set actually in use. Both
        // are re-applied every pass, so neither depends on the other having
        // happened first on any particular one.
        lendHostScratch(self->engine, self->host);
        self->engine.accumulate(view, IntRect{region.x, region.y, region.width, region.height});
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
        if (self->scope == WaveformScope::Parade) {
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

void setSampleThinning(SsScopeInstance* instance, int32_t divisor)
{
    try {
        WaveformInstance* self = impl(instance);
        self->settings.sampleThinning = divisor;
        self->engine.configure(self->settings);
    } catch (...) {
        // The boundary must not throw; the previous rate stands.
        return;
    }
}

constexpr SsSampleThinningExtension SampleThinning{setSampleThinning};

const void* getExtension(const SsScopeInstance*, const char* id)
{
    if (std::strcmp(id, AdaptiveImageExtension) == 0) {
        return &AdaptiveImage;
    }
    if (std::strcmp(id, SampleThinningExtension) == 0) {
        return &SampleThinning;
    }
    return nullptr;
}

void destroy(SsScopeInstance* instance)
{
    delete impl(instance);
}

// Plain and tinted luma are the same curve on the same axis, one painted in
// the average colour of the pixels that put it there. Nobody wants both on
// screen, so this stays a style where RGB and luma became scopes.
const char* const LumaStyleChoices[] = {"Plain", "Colored", nullptr};

const SsParamInfo TraceParams[] = {
    {"gain", "Intensity", SS_PARAM_INTENSITY, 0.0, 0.0, 0.05, 0.0, nullptr, nullptr},
    {"stride", "Sampling stride", SS_PARAM_INT, 1.0, 8.0, 1.0, 0.0, nullptr, nullptr},
};

const SsParamInfo LumaParams[] = {
    {"gain", "Intensity", SS_PARAM_INTENSITY, 0.0, 0.0, 0.05, 0.0, nullptr, nullptr},
    {"stride", "Sampling stride", SS_PARAM_INT, 1.0, 8.0, 1.0, 0.0, nullptr, nullptr},
    {"style", "Style", SS_PARAM_CHOICE, 0.0, 1.0, 0.0, 0.0, "Luma Waveform Style", LumaStyleChoices},
};

constexpr uint32_t TraceParamCount = static_cast<uint32_t>(sizeof(TraceParams) / sizeof(TraceParams[0]));
constexpr uint32_t LumaParamCount = static_cast<uint32_t>(sizeof(LumaParams) / sizeof(LumaParams[0]));

const SsScopeDescriptor WaveformDescriptor{
    "org.sidescopes.waveform", "Waveform", 'W', Waveform::Columns, Waveform::Levels, 0u, TraceParams,
    TraceParamCount,           3.0f,
};

// One reads exposure and the other reads balance, and a colourist wants both
// on screen: two instruments sharing an implementation rather than one drawn
// two ways. L was this exact scope before it was folded into a style.
const SsScopeDescriptor LumaDescriptor{
    "org.sidescopes.waveform.luma",
    "Luma Waveform",
    'L',
    Waveform::Columns,
    Waveform::Levels,
    0u,
    LumaParams,
    LumaParamCount,
    3.0f,
};

const SsScopeDescriptor ParadeDescriptor{
    "org.sidescopes.parade", "RGB Parade", 'R', Waveform::Columns, Waveform::Levels, 0u, TraceParams,
    TraceParamCount,         3.0f,
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
    return 3;
}

const SsScopeDescriptor* descriptor(uint32_t index)
{
    if (index == 0) {
        return &WaveformDescriptor;
    }
    if (index == 1) {
        return &LumaDescriptor;
    }
    if (index == 2) {
        return &ParadeDescriptor;
    }
    return nullptr;
}

/// Which scope @p scopeId names, or nothing when it names none of them.
std::optional<WaveformScope> scopeOf(const char* scopeId)
{
    if (std::strcmp(scopeId, WaveformDescriptor.id) == 0) {
        return WaveformScope::Rgb;
    }
    if (std::strcmp(scopeId, LumaDescriptor.id) == 0) {
        return WaveformScope::Luma;
    }
    if (std::strcmp(scopeId, ParadeDescriptor.id) == 0) {
        return WaveformScope::Parade;
    }

    return std::nullopt;
}

SsScopeInstance* create(const char* scopeId, const SsHost* host)
{
    try {
        const std::optional<WaveformScope> scope = scopeOf(scopeId);
        if (!scope) {
            return nullptr;
        }

        auto self = std::make_unique<WaveformInstance>();
        self->scope = *scope;
        self->settings.mode = modeOf(*scope, false);
        self->engine.configure(self->settings);
        self->host = host;
        self->vtable.instance_data = self.get();
        self->vtable.configure = configure;
        self->vtable.accumulate = accumulate;
        self->vtable.image = image;
        self->vtable.graticule = graticule;
        self->vtable.markers = markers;
        self->vtable.get_extension = getExtension;
        self->vtable.destroy = destroy;
        return &self.release()->vtable;
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
