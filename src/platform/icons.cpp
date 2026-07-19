#include "platform/icons.h"

#include <cstring>
#include <string>

// NanoSVG ships as headers whose implementation lands in exactly this
// translation unit; its code predates our warning wall.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#if defined(_MSC_VER)
#pragma warning(pop)
// C4702 (unreachable code) is an optimizer-stage warning that ignores
// in-source pragma state entirely; the build adds /wd4702 for this file
// on MSVC instead.
#else
#pragma GCC diagnostic pop
#endif

namespace sidescopes {
namespace {

// Lucide icons (https://lucide.dev, ISC license), embedded verbatim with
// the stroke colour resolved to the border chrome's near-white. All are
// 24 x 24, stroke-width 2, round caps and joins.
constexpr const char* PinSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#f7f7f7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 17v5" /><path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z" /></svg>)svg";

constexpr const char* PipetteSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#f7f7f7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m12 9-8.414 8.414A2 2 0 0 0 3 18.828v1.344a2 2 0 0 1-.586 1.414A2 2 0 0 1 3.828 21h1.344a2 2 0 0 0 1.414-.586L15 12" /><path d="m18 9 .4.4a1 1 0 1 1-3 3l-3.8-3.8a1 1 0 1 1 3-3l.4.4 3.4-3.4a1 1 0 1 1 3 3z" /><path d="m2 22 .414-.414" /></svg>)svg";

constexpr const char* ExpandSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#f7f7f7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M8 3H5a2 2 0 0 0-2 2v3" /><path d="M21 8V5a2 2 0 0 0-2-2h-3" /><path d="M3 16v3a2 2 0 0 0 2 2h3" /><path d="M16 21h3a2 2 0 0 0 2-2v-3" /></svg>)svg";

constexpr const char* SquarePenSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#f7f7f7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7" /><path d="M18.375 2.625a1 1 0 0 1 3 3l-9.013 9.014a2 2 0 0 1-.853.505l-2.873.84a.5.5 0 0 1-.62-.62l.84-2.873a2 2 0 0 1 .506-.852z" /></svg>)svg";

constexpr const char* PinOffSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#f7f7f7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 17v5" /><path d="M15 9.34V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H7.89" /><path d="m2 2 20 20" /><path d="M9 9v1.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h11" /></svg>)svg";

constexpr const char* PaperclipSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#f7f7f7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m16 6-8.414 8.586a2 2 0 0 0 2.829 2.829l8.414-8.586a4 4 0 1 0-5.657-5.657l-8.379 8.551a6 6 0 1 0 8.485 8.485l8.379-8.551" /></svg>)svg";

constexpr const char* UserSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#f7f7f7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 21v-2a4 4 0 0 0-4-4H9a4 4 0 0 0-4 4v2" /><circle cx="12" cy="7" r="4" /></svg>)svg";

const char* sourceFor(Icon icon)
{
    switch (icon) {
    case Icon::Pin:
        return PinSvg;
    case Icon::PinOff:
        return PinOffSvg;
    case Icon::Paperclip:
        return PaperclipSvg;
    case Icon::SquarePen:
        return SquarePenSvg;
    case Icon::User:
        return UserSvg;
    case Icon::Pipette:
        return PipetteSvg;
    case Icon::Expand:
        return ExpandSvg;
    }

    return PinSvg;
}

}  // namespace

std::vector<uint8_t> rasterizeIcon(Icon icon, int sizePixels)
{
    std::vector<uint8_t> pixels;
    if (sizePixels <= 0) {
        return pixels;
    }
    // nsvgParse mutates its input, so the embedded literal is copied.
    std::string source = sourceFor(icon);
    NSVGimage* image = nsvgParse(source.data(), "px", 96.0f);
    if (image == nullptr) {
        return pixels;
    }
    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    if (rasterizer == nullptr) {
        nsvgDelete(image);

        return pixels;
    }
    pixels.resize(static_cast<std::size_t>(sizePixels) * sizePixels * 4);
    const float scale = static_cast<float>(sizePixels) / (image->width > 0 ? image->width : 24.0f);
    nsvgRasterize(rasterizer, image, 0.0f, 0.0f, scale, pixels.data(), sizePixels, sizePixels, sizePixels * 4);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);

    return pixels;
}

}  // namespace sidescopes
