// A window of known geometry showing generated content, so that a measurement
// of the running application does not depend on whatever happens to be on the
// screen. It is the scenario harness's stand-in for the photo editor beside
// which SideScopes is used, and the X11 counterpart of content_window.m.
//
//   content_window --rect X,Y,W,H [--pattern NAME[,NAME...]]
//                  [--mode still|switch|animate|video] [--period SECONDS] [--fps N]
//                  [--title TEXT]
//
// The rectangle is the CONTENT rectangle in root-window coordinates: origin top
// left, y downwards, in pixels. A window manager may place the window elsewhere
// to fit its decoration on screen, so the achieved rectangle is printed and is
// what the caller must aim its region at.
//
// Written to stdout, one per line, once the window is on screen:
//   pid <n>
//   content_rect <x>,<y>,<w>,<h>
//   images <count>
//   ready
//
// The patterns are the ones content_window.m paints, pixel for pixel: the same
// generator, the same constants, the same order. A measurement taken on either
// system is therefore taken over the same picture, which is the whole reason
// the content is generated rather than photographed.
//
// Unlike the macOS window this one reads no image files. There is no decoder to
// link against here that the closed dependency list would allow, so a run asked
// for photographs degrades to generated content and records that it did.
//
// Build: cc -O2 -Wall -Wextra -o content_window content_window.c -lX11 -lm

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

// How much wider than the window the generated images are. The surplus is what
// the animate mode pans across, so that content changes every frame without
// redrawing anything on the processor: the window shows a moving sub-rectangle
// of a pixmap the X server already holds. The window shows the reciprocal of it
// at a time, so content is displayed at one image pixel per screen pixel.
static const double PanSurplus = 1.25;

// How far the video mode's pan travels between frames, in image pixels. A
// constant whole number of them, so that every frame the application captures
// differs from the one before by construction rather than by luck of geometry.
// The animate mode's pan follows a sine and slows to nothing at each turning
// point, so how much of it moves less than a pixel a frame depends on the
// window size it is given - and a frame that did not move is one the
// application is right to skip, which is not what watching footage costs.
static const double VideoPanPixelsPerFrame = 4.0;

// How often the parent is checked for, and how long the window manager is given
// to place the window before its position is read back.
static const double ParentCheckSeconds = 1.0;
static const double PlacementSeconds = 0.6;

#define MaxImages 16

// --- Deterministic noise ----------------------------------------------------

// A linear congruential generator, so that every machine and every run paints
// byte-identical content. rand() is not specified to agree across platforms.
static uint32_t nextRandom(uint32_t* state)
{
    *state = (*state * 1664525u) + 1013904223u;

    return *state;
}

static double randomUnit(uint32_t* state)
{
    return (double)(nextRandom(state) >> 8) / (double)(1u << 24);
}

// --- Pixel helpers ----------------------------------------------------------

typedef struct
{
    double r;
    double g;
    double b;
} Rgb;

// Where each channel sits in a pixel word, taken from the visual rather than
// assumed: an X server states its own channel order, and writing bytes in the
// order the macOS window uses would paint this one in the wrong channels on
// half of them.
static int RedShift = 16;
static int GreenShift = 8;
static int BlueShift = 0;

static uint8_t clampByte(double value)
{
    const double scaled = value * 255.0;
    if (scaled <= 0.0) {
        return 0;
    }
    if (scaled >= 255.0) {
        return 255;
    }

    return (uint8_t)(scaled + 0.5);
}

static Rgb hsvToRgb(double hue, double saturation, double value)
{
    const double sector = fmod(fmax(hue, 0.0), 360.0) / 60.0;
    const double fraction = sector - floor(sector);
    const double p = value * (1.0 - saturation);
    const double q = value * (1.0 - (saturation * fraction));
    const double t = value * (1.0 - (saturation * (1.0 - fraction)));
    switch ((int)sector) {
        case 0:
            return (Rgb){value, t, p};
        case 1:
            return (Rgb){q, value, p};
        case 2:
            return (Rgb){p, value, t};
        case 3:
            return (Rgb){p, q, value};
        case 4:
            return (Rgb){t, p, value};
        default:
            return (Rgb){value, p, q};
    }
}

static void writePixel(uint8_t* row, size_t x, Rgb color)
{
    const uint32_t packed = ((uint32_t)clampByte(color.r) << RedShift) | ((uint32_t)clampByte(color.g) << GreenShift) |
                            ((uint32_t)clampByte(color.b) << BlueShift);
    memcpy(row + (x * 4), &packed, sizeof(packed));
}

// --- Patterns ---------------------------------------------------------------

// Colour bars: large flat fields and hard edges. The cheap end of the range -
// a histogram of it holds a handful of spikes - and the case where a resolution
// change reads as a loss, because all of the gradient energy is in six edges.
static void paintBars(uint8_t* pixels, size_t width, size_t height, size_t stride)
{
    static const Rgb Bars[] = {{0.75, 0.75, 0.75}, {0.75, 0.75, 0.0}, {0.0, 0.75, 0.75}, {0.0, 0.75, 0.0},
                               {0.75, 0.0, 0.75},  {0.75, 0.0, 0.0},  {0.0, 0.0, 0.75},  {0.0, 0.0, 0.0}};
    const size_t barCount = sizeof(Bars) / sizeof(Bars[0]);
    const size_t split = (height * 2) / 3;
    const size_t steps = 11;
    for (size_t y = 0; y < height; ++y) {
        uint8_t* row = pixels + (y * stride);
        for (size_t x = 0; x < width; ++x) {
            if (y < split) {
                writePixel(row, x, Bars[(x * barCount) / width]);
            } else {
                const double level = (double)((x * steps) / width) / (double)(steps - 1);
                writePixel(row, x, (Rgb){level, level, level});
            }
        }
    }
}

// Smooth ramps: a neutral wedge over a hue sweep. Nothing here has an edge, so
// it is the shape of content that shows banding and quantisation.
static void paintRamp(uint8_t* pixels, size_t width, size_t height, size_t stride)
{
    const size_t split = height / 2;
    for (size_t y = 0; y < height; ++y) {
        uint8_t* row = pixels + (y * stride);
        for (size_t x = 0; x < width; ++x) {
            const double across = (double)x / (double)(width - 1);
            if (y < split) {
                writePixel(row, x, (Rgb){across, across, across});
            } else {
                const double down = (double)(y - split) / (double)(height - split);
                writePixel(row, x, hsvToRgb(across * 360.0, 0.35 + (0.6 * down), 0.85));
            }
        }
    }
}

// Skin tones around a neutral card: the content this product is pointed at most
// often, and what puts a trace on the vectorscope's skin line.
static void paintSkin(uint8_t* pixels, size_t width, size_t height, size_t stride)
{
    uint32_t noise = 0x5eed1234u;
    const double centreX = (double)width * 0.5;
    const double centreY = (double)height * 0.5;
    const double radius = fmin((double)width, (double)height) * 0.42;
    for (size_t y = 0; y < height; ++y) {
        uint8_t* row = pixels + (y * stride);
        for (size_t x = 0; x < width; ++x) {
            const double dx = ((double)x - centreX) / radius;
            const double dy = ((double)y - centreY) / radius;
            const double distance = sqrt((dx * dx) + (dy * dy));
            const double shade = 0.55 + (0.3 * cos(fmin(distance, 1.0) * M_PI * 0.5));
            const double grain = (randomUnit(&noise) - 0.5) * 0.03;
            Rgb color = hsvToRgb(22.0 + (8.0 * dx), 0.34 - (0.06 * dy), shade + grain);
            // An 18% card and a white patch, so a run always contains a known
            // neutral and a known clipping point.
            if (x < width / 8 && y < height / 8) {
                color = (Rgb){0.18, 0.18, 0.18};
            } else if (x >= width - (width / 8) && y < height / 8) {
                color = (Rgb){0.95, 0.95, 0.95};
            }
            writePixel(row, x, color);
        }
    }
}

// Full-amplitude noise: every bin populated, no coherence to exploit. The
// expensive end of the range.
static void paintNoise(uint8_t* pixels, size_t width, size_t height, size_t stride)
{
    uint32_t state = 0x13579bdfu;
    for (size_t y = 0; y < height; ++y) {
        uint8_t* row = pixels + (y * stride);
        for (size_t x = 0; x < width; ++x) {
            writePixel(row, x, (Rgb){randomUnit(&state), randomUnit(&state), randomUnit(&state)});
        }
    }
}

// A flat mid grey. The floor: one populated bin, and a frame the change
// detector should recognise as unchanged from the last.
static void paintFlat(uint8_t* pixels, size_t width, size_t height, size_t stride)
{
    for (size_t y = 0; y < height; ++y) {
        uint8_t* row = pixels + (y * stride);
        for (size_t x = 0; x < width; ++x) {
            writePixel(row, x, (Rgb){0.5, 0.5, 0.5});
        }
    }
}

// A photograph's mixture: a smooth sky, a textured middle, a skin subject and
// two flat blocks. Varied everywhere, which is what a pointer sweeping across
// it needs - a previous measurement circled uniform content, so the colour
// under the pointer barely changed and the readout cost nothing.
static void paintPhotoish(uint8_t* pixels, size_t width, size_t height, size_t stride)
{
    uint32_t noise = 0xa5a51357u;
    const double subjectX = (double)width * 0.62;
    const double subjectY = (double)height * 0.58;
    const double subjectRadius = fmin((double)width, (double)height) * 0.28;
    for (size_t y = 0; y < height; ++y) {
        uint8_t* row = pixels + (y * stride);
        const double down = (double)y / (double)(height - 1);
        for (size_t x = 0; x < width; ++x) {
            const double across = (double)x / (double)(width - 1);
            // A smooth field: three sinusoids per channel, so no two places on
            // the image hold the same colour.
            Rgb color = {0.45 + (0.30 * sin((across * 5.1) + 0.4)) + (0.12 * sin(down * 7.3)),
                         0.48 + (0.26 * sin((across * 3.7) + 2.1)) + (0.14 * sin((down * 4.9) + 1.2)),
                         0.55 + (0.28 * sin((across * 2.3) + 4.2)) + (0.10 * sin((down * 6.1) + 2.4))};
            if (down < 0.3) {
                // Sky: smooth, low detail, the flat part of a photograph.
                const double sky = 0.55 + (0.35 * (1.0 - (down / 0.3)));
                color = (Rgb){sky * 0.72, sky * 0.86, sky};
            } else {
                // Foliage-grade texture: fine grain over the smooth field.
                const double grain = (randomUnit(&noise) - 0.5) * 0.22;
                color.r += grain;
                color.g += grain * 1.1;
                color.b += grain * 0.8;
            }
            const double dx = ((double)x - subjectX) / subjectRadius;
            const double dy = ((double)y - subjectY) / subjectRadius;
            const double distance = sqrt((dx * dx) + (dy * dy));
            if (distance < 1.0) {
                const double shade = 0.58 + (0.28 * cos(distance * M_PI * 0.5));
                color = hsvToRgb(24.0 + (6.0 * dx), 0.33, shade + ((randomUnit(&noise) - 0.5) * 0.02));
            }
            if (down > 0.86 && across < 0.16) {
                color = (Rgb){0.03, 0.03, 0.035};
            } else if (down > 0.86 && across > 0.84) {
                color = (Rgb){0.97, 0.97, 0.94};
            }
            writePixel(row, x, color);
        }
    }
}

typedef void (*PatternPainter)(uint8_t*, size_t, size_t, size_t);

static PatternPainter painterNamed(const char* name)
{
    if (strcmp(name, "bars") == 0) {
        return paintBars;
    }
    if (strcmp(name, "ramp") == 0) {
        return paintRamp;
    }
    if (strcmp(name, "skin") == 0) {
        return paintSkin;
    }
    if (strcmp(name, "noise") == 0) {
        return paintNoise;
    }
    if (strcmp(name, "flat") == 0) {
        return paintFlat;
    }
    if (strcmp(name, "photoish") == 0) {
        return paintPhotoish;
    }

    return NULL;
}

// --- Options ----------------------------------------------------------------

typedef struct
{
    int x;
    int y;
    int width;
    int height;
    const char* patterns;
    const char* mode;
    const char* title;
    double period;
    double fps;
} Options;

static int parseOptions(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const char* flag = argv[i];
        const char* value = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (value == NULL) {
            fprintf(stderr, "content_window: %s needs a value\n", flag);

            return 0;
        }
        ++i;
        if (strcmp(flag, "--rect") == 0) {
            double x = 0.0;
            double y = 0.0;
            double width = 0.0;
            double height = 0.0;
            if (sscanf(value, "%lf,%lf,%lf,%lf", &x, &y, &width, &height) != 4) {
                fprintf(stderr, "content_window: --rect wants X,Y,W,H\n");

                return 0;
            }
            options->x = (int)x;
            options->y = (int)y;
            options->width = (int)width;
            options->height = (int)height;
        } else if (strcmp(flag, "--pattern") == 0) {
            options->patterns = value;
        } else if (strcmp(flag, "--image") == 0) {
            // Named rather than rejected: the caller decides what to do about
            // it, and a flag that vanished silently would leave a run showing
            // content nobody asked for without saying so.
            fprintf(stderr, "content_window: this window reads no image files; use --pattern\n");

            return 0;
        } else if (strcmp(flag, "--mode") == 0) {
            options->mode = value;
        } else if (strcmp(flag, "--title") == 0) {
            options->title = value;
        } else if (strcmp(flag, "--period") == 0) {
            options->period = atof(value);
        } else if (strcmp(flag, "--fps") == 0) {
            options->fps = atof(value);
        } else {
            fprintf(stderr, "content_window: unknown option %s\n", flag);

            return 0;
        }
    }

    return options->width > 0 && options->height > 0;
}

// --- The window -------------------------------------------------------------

typedef struct
{
    Display* display;
    Window window;
    Atom deleteWindow;
    GC context;
    Pixmap images[MaxImages];
    int imageCount;
    int imageWidth;
    int imageHeight;
    int windowWidth;
    int windowHeight;
    int index;
    double phase;
    // Where the video mode's pan has reached, in image pixels, and which way it
    // is going. Held in pixels rather than in a fraction so that the step per
    // frame is a whole number of pixels whatever the image size.
    double panPixels;
    double panDirection;
    double panTravelPixels;
    double panOffset;
    pid_t parent;
} Content;

static double nowSeconds(void)
{
    struct timeval clock;
    gettimeofday(&clock, NULL);

    return (double)clock.tv_sec + ((double)clock.tv_usec / 1e6);
}

// Copies the visible sub-rectangle of the current image to the window. The
// server does the blit from a pixmap it already holds, so panning costs this
// process nothing per frame - which is the point, since what it costs would
// otherwise land in the same measurement as the application.
static void present(Content* content)
{
    if (content->imageCount == 0) {
        return;
    }
    int offset = (int)(content->panOffset * (double)(content->imageWidth - content->windowWidth));
    if (offset < 0) {
        offset = 0;
    }
    if (offset > content->imageWidth - content->windowWidth) {
        offset = content->imageWidth - content->windowWidth;
    }
    XCopyArea(content->display, content->images[content->index], content->window, content->context, offset, 0,
              (unsigned int)content->windowWidth, (unsigned int)content->windowHeight, 0, 0);
    XFlush(content->display);
}

static void advanceImage(Content* content)
{
    content->index = (content->index + 1) % content->imageCount;
    present(content);
}

// Pans the visible sub-rectangle back and forth, which changes every pixel the
// region sees on every tick.
static void advancePan(Content* content)
{
    content->phase += 0.06;
    content->panOffset = 0.5 * (1.0 + sin(content->phase));
    present(content);
}

// Pans at a CONSTANT speed, turning round at each end of the surplus, which is
// the difference between this and advancePan: a constant speed has no turning
// point at which the content stands still, so every frame the application
// captures really is different from the one before. Watching footage is the one
// workload where nothing the application skips can be skipped, and a scenario
// meant to price that must not hand it frames it is entitled to skip.
static void advanceVideo(Content* content)
{
    content->panPixels += VideoPanPixelsPerFrame * content->panDirection;
    if (content->panPixels >= content->panTravelPixels) {
        content->panPixels = content->panTravelPixels;
        content->panDirection = -1.0;
    } else if (content->panPixels <= 0.0) {
        content->panPixels = 0.0;
        content->panDirection = 1.0;
    }
    content->panOffset = content->panTravelPixels > 0.0 ? content->panPixels / content->panTravelPixels : 0.0;
    present(content);
}

// One image, painted into client memory and handed to the server once. Held as
// a pixmap rather than as an XImage because every later frame is a blit from
// it: keeping the pixels here would put a copy of the whole image on this
// process's bill at every tick of the pan.
static Pixmap newImage(Display* display, Window window, GC context, PatternPainter painter, int width, int height)
{
    const size_t stride = (size_t)width * 4;
    uint8_t* pixels = calloc((size_t)height, stride);
    if (pixels == NULL) {
        return None;
    }
    painter(pixels, (size_t)width, (size_t)height, stride);
    const int screen = DefaultScreen(display);
    XImage* image = XCreateImage(display, DefaultVisual(display, screen), (unsigned int)DefaultDepth(display, screen),
                                 ZPixmap, 0, (char*)pixels, (unsigned int)width, (unsigned int)height, 32, 0);
    if (image == NULL) {
        free(pixels);

        return None;
    }
    const Pixmap pixmap =
        XCreatePixmap(display, window, (unsigned int)width, (unsigned int)height,
                      (unsigned int)DefaultDepth(display, screen));
    XPutImage(display, pixmap, context, image, 0, 0, 0, 0, (unsigned int)width, (unsigned int)height);
    // Frees the pixel buffer with it, which is why the buffer is not freed here.
    XDestroyImage(image);

    return pixmap;
}

// Splits a comma list in place, filling `parts` with its non-empty entries.
static int splitList(char* list, const char** parts, int limit)
{
    int count = 0;
    char* cursor = list;
    while (cursor != NULL && *cursor != '\0' && count < limit) {
        char* comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        if (*cursor != '\0') {
            parts[count++] = cursor;
        }
        cursor = (comma != NULL) ? comma + 1 : NULL;
    }

    return count;
}

static void buildImages(Content* content, const char* patterns)
{
    char* editable = strdup(patterns != NULL ? patterns : "");
    if (editable == NULL) {
        return;
    }
    const char* names[MaxImages];
    const int count = splitList(editable, names, MaxImages);
    for (int index = 0; index < count; ++index) {
        PatternPainter painter = painterNamed(names[index]);
        if (painter == NULL) {
            fprintf(stderr, "content_window: unknown pattern %s\n", names[index]);
            continue;
        }
        const Pixmap image = newImage(content->display, content->window, content->context, painter,
                                      content->imageWidth, content->imageHeight);
        if (image != None) {
            content->images[content->imageCount++] = image;
        }
    }
    free(editable);
}

// The channel positions this server's visual uses, so that a colour written by
// the painters lands in the channel it names.
static int readVisualShifts(Display* display)
{
    Visual* visual = DefaultVisual(display, DefaultScreen(display));
    if (visual->red_mask == 0 || visual->green_mask == 0 || visual->blue_mask == 0) {
        return 0;
    }
    RedShift = __builtin_ctzl(visual->red_mask);
    GreenShift = __builtin_ctzl(visual->green_mask);
    BlueShift = __builtin_ctzl(visual->blue_mask);

    return 1;
}

// Whether a pixel of the screen's depth is four bytes, which is what the
// painters write. Asked of the server rather than inferred from the depth: a
// 24-bit screen may pack three bytes to the pixel, and a run that assumed four
// would paint a diagonal smear and measure it.
static int hasFourBytePixels(Display* display)
{
    int count = 0;
    XPixmapFormatValues* formats = XListPixmapFormats(display, &count);
    if (formats == NULL) {
        return 0;
    }
    int found = 0;
    for (int index = 0; index < count; ++index) {
        if (formats[index].depth == DefaultDepth(display, DefaultScreen(display))) {
            found = formats[index].bits_per_pixel == 32;
        }
    }
    XFree(formats);

    return found;
}

// Asks not to be given the keyboard, so the application under measurement keeps
// the focus its plain-letter shortcuts need. WM_HINTS says the window takes no
// input and the user time says it was not opened by a user action; between them
// every window manager this will meet leaves the focus where it was.
static void refuseFocus(Display* display, Window window)
{
    XWMHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = InputHint;
    hints.input = False;
    XSetWMHints(display, window, &hints);
    const unsigned long none = 0;
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_USER_TIME", False), XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char*)&none, 1);
}

// The position asked for is the CONTENT's, and a window manager positions the
// decoration instead unless it is told the coordinates are the user's own and
// that the frame is to be added around them rather than over them.
static void placeExactly(Display* display, Window window, const Options* options)
{
    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = USPosition | PPosition | USSize | PSize | PWinGravity;
    hints.x = options->x;
    hints.y = options->y;
    hints.width = options->width;
    hints.height = options->height;
    hints.win_gravity = StaticGravity;
    XSetWMNormalHints(display, window, &hints);
}

static void setTitle(Display* display, Window window, const char* title)
{
    XStoreName(display, window, title);
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_NAME", False),
                    XInternAtom(display, "UTF8_STRING", False), 8, PropModeReplace, (const unsigned char*)title,
                    (int)strlen(title));
}

// Where the window's content actually ended up, in root coordinates. Read back
// rather than assumed: a window manager may refuse the position or the size,
// and the caller aims a measurement region at this rectangle.
static void contentRect(Display* display, Window window, int* x, int* y, int* width, int* height)
{
    Window child = None;
    XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, x, y, &child);
    XWindowAttributes attributes;
    if (XGetWindowAttributes(display, window, &attributes)) {
        *width = attributes.width;
        *height = attributes.height;
    }
}

// Maps the window and puts its content where it was asked for, correcting once
// for whatever the window manager did with the request. One correction rather
// than a loop: a window manager that refuses the position twice will refuse it
// for ever, and the achieved rectangle is reported either way.
static void showWindow(Content* content, const Options* options)
{
    Display* display = content->display;
    XMapWindow(display, content->window);
    XSync(display, False);
    const double deadline = nowSeconds() + PlacementSeconds;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    while (nowSeconds() < deadline) {
        XWindowAttributes attributes;
        if (XGetWindowAttributes(display, content->window, &attributes) && attributes.map_state == IsViewable) {
            break;
        }
        usleep(20000);
    }
    contentRect(display, content->window, &x, &y, &width, &height);
    if (x != options->x || y != options->y) {
        XMoveWindow(display, content->window, options->x + (options->x - x), options->y + (options->y - y));
        XSync(display, False);
    }
}

static Window newContentWindow(Display* display, const Options* options)
{
    const int screen = DefaultScreen(display);
    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.background_pixel = BlackPixel(display, screen);
    attributes.event_mask = ExposureMask | StructureNotifyMask;
    const Window window = XCreateWindow(display, RootWindow(display, screen), options->x, options->y,
                                        (unsigned int)options->width, (unsigned int)options->height, 0,
                                        DefaultDepth(display, screen), InputOutput, DefaultVisual(display, screen),
                                        CWBackPixel | CWEventMask, &attributes);
    if (window == None) {
        return None;
    }
    setTitle(display, window, options->title);
    XSetClassHint(display, window, &(XClassHint){(char*)"content_window", (char*)"ContentWindow"});
    placeExactly(display, window, options);
    refuseFocus(display, window);

    return window;
}

// --- The loop ---------------------------------------------------------------

// Drains whatever the server has to say. Only two things matter: a redraw, and
// a request to go away.
static int drainEvents(Content* content)
{
    while (XPending(content->display) > 0) {
        XEvent event;
        XNextEvent(content->display, &event);
        if (event.type == Expose && event.xexpose.count == 0) {
            present(content);
        } else if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == content->deleteWindow) {
            return 0;
        }
    }

    return 1;
}

// Waits until the next thing is due, on the X connection or on the clock,
// rather than polling. A content window that spun would be charged to the same
// machine as the application it exists to give something to look at.
static void waitUntil(Display* display, double until)
{
    const double remaining = until - nowSeconds();
    if (remaining <= 0.0) {
        return;
    }
    const int connection = ConnectionNumber(display);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(connection, &readable);
    struct timeval timeout;
    timeout.tv_sec = (time_t)remaining;
    timeout.tv_usec = (suseconds_t)((remaining - (double)timeout.tv_sec) * 1e6);
    select(connection + 1, &readable, NULL, NULL, &timeout);
}

// The mode's own tick, plus the watch that leaves the screen if the harness
// that started this is gone.
static void runLoop(Content* content, const Options* options)
{
    const int switching = strcmp(options->mode, "switch") == 0;
    const int animating = strcmp(options->mode, "animate") == 0;
    const int playing = strcmp(options->mode, "video") == 0;
    const double interval = (switching ? fmax(options->period, 0.05) : 1.0 / fmax(options->fps, 1.0));
    double nextTick = nowSeconds() + interval;
    double nextParentCheck = nowSeconds() + ParentCheckSeconds;
    while (1) {
        waitUntil(content->display, fmin(nextTick, nextParentCheck));
        if (!drainEvents(content)) {
            return;
        }
        const double now = nowSeconds();
        if (now >= nextParentCheck) {
            // A harness that dies mid-run must not leave a window sitting over
            // the desktop.
            if (content->parent > 1 && kill(content->parent, 0) != 0) {
                return;
            }
            nextParentCheck = now + ParentCheckSeconds;
        }
        if (now >= nextTick) {
            if (switching) {
                advanceImage(content);
            } else if (animating) {
                advancePan(content);
            } else if (playing) {
                advanceVideo(content);
            }
            nextTick = now + interval;
        }
    }
}

// The rectangle the window actually achieved is what the caller must aim a
// region at, so it is reported rather than assumed.
static void reportReady(Content* content)
{
    int x = 0;
    int y = 0;
    int width = content->windowWidth;
    int height = content->windowHeight;
    contentRect(content->display, content->window, &x, &y, &width, &height);
    printf("pid %d\n", (int)getpid());
    printf("content_rect %d,%d,%d,%d\n", x, y, width, height);
    printf("images %d\n", content->imageCount);
    printf("ready\n");
    fflush(stdout);
}

// The screen this needs: true colour, four bytes to the pixel, and channel
// positions it can read rather than guess.
static int openScreen(Display** display)
{
    *display = XOpenDisplay(NULL);
    if (*display == NULL) {
        fprintf(stderr, "content_window: cannot open the X display\n");

        return 0;
    }
    if (DefaultDepth(*display, DefaultScreen(*display)) < 24 || !hasFourBytePixels(*display) ||
        !readVisualShifts(*display)) {
        fprintf(stderr, "content_window: this display is not a true colour screen at four bytes a pixel\n");

        return 0;
    }

    return 1;
}

// The window, its images and everything the pan is measured in.
static int buildContent(Content* content, Display* display, const Options* options)
{
    memset(content, 0, sizeof(*content));
    content->display = display;
    content->window = newContentWindow(display, options);
    if (content->window == None) {
        fprintf(stderr, "content_window: cannot create a window\n");

        return 0;
    }
    // Told to the window manager so that a close from the desktop is a request
    // this process can act on rather than a connection dropped underneath it.
    content->deleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, content->window, &content->deleteWindow, 1);
    content->context = XCreateGC(display, content->window, 0, NULL);
    content->windowWidth = options->width;
    content->windowHeight = options->height;
    content->imageWidth = (int)((double)options->width * PanSurplus);
    content->imageHeight = options->height;
    content->panDirection = 1.0;
    content->panTravelPixels = (double)(content->imageWidth - content->windowWidth);
    content->panOffset = 0.5;
    content->parent = getppid();

    buildImages(content, options->patterns);
    if (content->imageCount == 0) {
        fprintf(stderr, "content_window: no content to show\n");

        return 0;
    }

    return 1;
}

int main(int argc, char** argv)
{
    Options options = {.x = 100,
                       .y = 100,
                       .width = 1200,
                       .height = 800,
                       .patterns = "photoish",
                       .mode = "still",
                       .title = "SideScopes measurement content",
                       .period = 2.0,
                       .fps = 30.0};
    if (!parseOptions(argc, argv, &options)) {
        return 2;
    }

    Display* display = NULL;
    Content content;
    if (!openScreen(&display) || !buildContent(&content, display, &options)) {
        return 3;
    }

    showWindow(&content, &options);
    present(&content);
    if (strcmp(options.mode, "video") == 0) {
        // Before the ready line, which is where the caller stops reading.
        printf("pan %.0f,%.0f,%.1f\n", VideoPanPixelsPerFrame, content.panTravelPixels, options.fps);
    }
    reportReady(&content);

    runLoop(&content, &options);
    XCloseDisplay(display);

    return 0;
}
