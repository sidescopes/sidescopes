// A window of known geometry showing generated content, so that a measurement
// of the running application does not depend on whatever happens to be on the
// screen. It is the scenario harness's stand-in for the photo editor beside
// which SideScopes is used.
//
//   content_window --rect X,Y,W,H [--pattern NAME[,NAME...]] [--image PATH[,PATH...]]
//                  [--mode still|switch|animate|video] [--period SECONDS] [--fps N]
//                  [--title TEXT]
//
// The rectangle is the CONTENT rectangle in the global point space the pointer
// uses: origin top left of the primary display, y downwards. The window may be
// moved by the system to fit its title bar on screen, so the achieved rectangle
// is printed and is what the caller must aim its region at.
//
// Written to stdout, one per line, once the window is on screen:
//   pid <n>
//   content_rect <x>,<y>,<w>,<h>
//   images <count>
//   ready
//
// The process is an accessory, so it never steals keyboard focus from the
// application under measurement. It moves only by its title bar, deliberately:
// a drag across the content is how a region is drawn over it, and a window that
// followed those drags would slide off the screen the first time a shortcut was
// missed.
//
// Build: cc -fobjc-arc -O2 -framework Cocoa -o content_window content_window.m

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <math.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// How much wider than the window the generated images are. The surplus is what
// the animate mode pans across, so that content changes every frame without
// redrawing anything on the processor: the layer shows a moving sub-rectangle
// of an image it already holds.
static const double PanSurplus = 1.25;

// The fraction of an image the window shows at once, the reciprocal of the
// surplus, so that content is displayed at one image pixel per screen pixel.
static const double PanWindow = 1.0 / PanSurplus;

// How far the video mode's pan travels between frames, in image pixels. A
// constant whole number of them, so that every frame the application captures
// differs from the one before by construction rather than by luck of geometry.
// The animate mode's pan follows a sine and slows to nothing at each turning
// point, so how much of it moves less than a pixel a frame depends on the
// window size it is given - and a frame that did not move is one the
// application is right to skip, which is not what watching footage costs.
static const double VideoPanPixelsPerFrame = 4.0;

#pragma mark - Deterministic noise

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

#pragma mark - Pixel helpers

typedef struct
{
    double r;
    double g;
    double b;
} Rgb;

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
    row[(x * 4) + 0] = clampByte(color.r);
    row[(x * 4) + 1] = clampByte(color.g);
    row[(x * 4) + 2] = clampByte(color.b);
    row[(x * 4) + 3] = 255;
}

#pragma mark - Patterns

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

#pragma mark - Images

static CGImageRef newImageFromPainter(PatternPainter painter, size_t width, size_t height)
{
    const size_t stride = width * 4;
    uint8_t* pixels = calloc(height, stride);
    if (pixels == NULL) {
        return NULL;
    }
    painter(pixels, width, height, stride);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels, width, height, 8, stride, space,
                                                 kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault);
    CGColorSpaceRelease(space);
    if (context == NULL) {
        free(pixels);
        return NULL;
    }
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    free(pixels);

    return image;
}

// Scales a file image to the generated size, filling the frame and cropping the
// overflow, so a photograph and a pattern occupy the window identically.
static CGImageRef newImageFromFile(NSString* path, size_t width, size_t height)
{
    NSImage* loaded = [[NSImage alloc] initWithContentsOfFile:path];
    if (loaded == nil) {
        return NULL;
    }
    CGImageRef source = [loaded CGImageForProposedRect:NULL context:nil hints:nil];
    if (source == NULL) {
        return NULL;
    }
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(NULL, width, height, 8, width * 4, space,
                                                 kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault);
    CGColorSpaceRelease(space);
    if (context == NULL) {
        return NULL;
    }
    const double sourceWidth = (double)CGImageGetWidth(source);
    const double sourceHeight = (double)CGImageGetHeight(source);
    const double scale = fmax((double)width / sourceWidth, (double)height / sourceHeight);
    const double drawnWidth = sourceWidth * scale;
    const double drawnHeight = sourceHeight * scale;
    CGContextDrawImage(context, CGRectMake(((double)width - drawnWidth) * 0.5, ((double)height - drawnHeight) * 0.5,
                                           drawnWidth, drawnHeight),
                       source);
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);

    return image;
}

#pragma mark - The window

@interface ContentController : NSObject
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSMutableArray* images;  // CGImageRef, boxed as id
@property(nonatomic, assign) NSUInteger index;
@property(nonatomic, assign) double phase;
// Where the video mode's pan has reached, in image pixels, and which way it is
// going. Held in pixels rather than in the layer's normalized units so that the
// step per frame is a whole number of pixels whatever the image size.
@property(nonatomic, assign) double panPixels;
@property(nonatomic, assign) double panDirection;
@property(nonatomic, assign) double panTravelPixels;
@property(nonatomic, assign) pid_t parent;
@end

@implementation ContentController

- (void)showImageAtIndex:(NSUInteger)index
{
    if (self.images.count == 0) {
        return;
    }
    self.index = index % self.images.count;
    CALayer* layer = self.window.contentView.layer;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contents = self.images[self.index];
    [CATransaction commit];
}

- (void)advanceImage:(NSTimer*)timer
{
    (void)timer;
    [self showImageAtIndex:self.index + 1];
}

// Pans the visible sub-rectangle back and forth, which changes every pixel the
// region sees on every tick without touching a pixel buffer.
- (void)advancePan:(NSTimer*)timer
{
    (void)timer;
    self.phase += 0.06;
    const double surplus = 1.0 - PanWindow;
    const double offset = surplus * 0.5 * (1.0 + sin(self.phase));
    CALayer* layer = self.window.contentView.layer;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contentsRect = CGRectMake(offset, 0.0, PanWindow, 1.0);
    [CATransaction commit];
}

// Pans at a CONSTANT speed, turning round at each end of the surplus, which is
// the difference between this and advancePan: a constant speed has no turning
// point at which the content stands still, so every frame the application
// captures really is different from the one before. Watching footage is the one
// workload where nothing the application skips can be skipped, and a scenario
// meant to price that must not hand it frames it is entitled to skip.
- (void)advanceVideo:(NSTimer*)timer
{
    (void)timer;
    self.panPixels += VideoPanPixelsPerFrame * self.panDirection;
    if (self.panPixels >= self.panTravelPixels) {
        self.panPixels = self.panTravelPixels;
        self.panDirection = -1.0;
    } else if (self.panPixels <= 0.0) {
        self.panPixels = 0.0;
        self.panDirection = 1.0;
    }
    const double surplus = 1.0 - PanWindow;
    const double offset = self.panTravelPixels > 0.0 ? surplus * (self.panPixels / self.panTravelPixels) : 0.0;
    CALayer* layer = self.window.contentView.layer;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contentsRect = CGRectMake(offset, 0.0, PanWindow, 1.0);
    [CATransaction commit];
}

// Leaves the screen when whoever started this is gone. A harness that dies
// mid-run must not leave a window sitting over the user's desktop.
- (void)checkParent:(NSTimer*)timer
{
    (void)timer;
    if (self.parent > 0 && kill(self.parent, 0) != 0) {
        exit(0);
    }
}

@end

// The primary display's height, which converts between the global point space
// the pointer uses (origin top left, y down) and Cocoa's (origin bottom left).
static double primaryHeight(void)
{
    return CGDisplayBounds(CGMainDisplayID()).size.height;
}

static NSRect contentRectInGlobalPoints(NSWindow* window)
{
    const NSRect frame = window.frame;
    const NSRect content = [window contentRectForFrameRect:frame];

    return NSMakeRect(content.origin.x, primaryHeight() - content.origin.y - content.size.height, content.size.width,
                      content.size.height);
}

typedef struct
{
    double x;
    double y;
    double width;
    double height;
    const char* patterns;
    const char* files;
    const char* mode;
    const char* title;
    double period;
    double fps;
} Options;

static int parseOptions(int argc, const char** argv, Options* options)
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
            if (sscanf(value, "%lf,%lf,%lf,%lf", &options->x, &options->y, &options->width, &options->height) != 4) {
                fprintf(stderr, "content_window: --rect wants X,Y,W,H\n");

                return 0;
            }
        } else if (strcmp(flag, "--pattern") == 0) {
            options->patterns = value;
        } else if (strcmp(flag, "--image") == 0) {
            options->files = value;
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

    return options->width > 0.0 && options->height > 0.0;
}

// Splits a comma list into its parts, empty entries dropped.
static NSArray<NSString*>* splitList(const char* list)
{
    if (list == NULL) {
        return @[];
    }
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    for (NSString* part in [@(list) componentsSeparatedByString:@","]) {
        NSString* trimmed = [part stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (trimmed.length > 0) {
            [parts addObject:trimmed];
        }
    }

    return parts;
}

static NSMutableArray* buildImages(const Options* options, size_t width, size_t height)
{
    NSMutableArray* images = [NSMutableArray array];
    for (NSString* path in splitList(options->files)) {
        CGImageRef image = newImageFromFile(path, width, height);
        if (image == NULL) {
            fprintf(stderr, "content_window: cannot read image %s\n", path.UTF8String);
            continue;
        }
        [images addObject:(__bridge_transfer id)image];
    }
    for (NSString* name in splitList(options->patterns)) {
        PatternPainter painter = painterNamed(name.UTF8String);
        if (painter == NULL) {
            fprintf(stderr, "content_window: unknown pattern %s\n", name.UTF8String);
            continue;
        }
        CGImageRef image = newImageFromPainter(painter, width, height);
        if (image != NULL) {
            [images addObject:(__bridge_transfer id)image];
        }
    }

    return images;
}

// The window and its layer-backed view, on screen at the requested content
// rectangle. Only the title bar moves it: a drag across the content is how a
// region is drawn over it.
static NSWindow* newContentWindow(const Options* options)
{
    const NSRect content = NSMakeRect(options->x, primaryHeight() - options->y - options->height, options->width,
                                      options->height);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:content
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title = @(options->title);
    window.releasedWhenClosed = NO;

    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, options->width, options->height)];
    view.wantsLayer = YES;
    window.contentView = view;
    [window orderFrontRegardless];
    view.layer.contentsScale = window.backingScaleFactor;
    view.layer.contentsGravity = kCAGravityResize;
    view.layer.magnificationFilter = kCAFilterNearest;
    view.layer.contentsRect = CGRectMake((1.0 - PanWindow) * 0.5, 0.0, PanWindow, 1.0);

    return window;
}

// The mode's own timer, plus the watch that leaves the screen if the harness
// that started this is gone.
static void startTimers(ContentController* controller, const Options* options)
{
    [NSTimer scheduledTimerWithTimeInterval:1.0
                                     target:controller
                                   selector:@selector(checkParent:)
                                   userInfo:nil
                                    repeats:YES];
    if (strcmp(options->mode, "switch") == 0) {
        [NSTimer scheduledTimerWithTimeInterval:fmax(options->period, 0.05)
                                         target:controller
                                       selector:@selector(advanceImage:)
                                       userInfo:nil
                                        repeats:YES];
    } else if (strcmp(options->mode, "animate") == 0) {
        [NSTimer scheduledTimerWithTimeInterval:1.0 / fmax(options->fps, 1.0)
                                         target:controller
                                       selector:@selector(advancePan:)
                                       userInfo:nil
                                        repeats:YES];
    } else if (strcmp(options->mode, "video") == 0) {
        [NSTimer scheduledTimerWithTimeInterval:1.0 / fmax(options->fps, 1.0)
                                         target:controller
                                       selector:@selector(advanceVideo:)
                                       userInfo:nil
                                        repeats:YES];
    }
}

// The rectangle the window actually achieved is what the caller must aim a
// region at, so it is reported rather than assumed.
static void reportReady(NSWindow* window, NSUInteger imageCount)
{
    const NSRect achieved = contentRectInGlobalPoints(window);
    printf("pid %d\n", getpid());
    printf("content_rect %.0f,%.0f,%.0f,%.0f\n", achieved.origin.x, achieved.origin.y, achieved.size.width,
           achieved.size.height);
    printf("images %lu\n", (unsigned long)imageCount);
    printf("ready\n");
    fflush(stdout);
}

int main(int argc, const char** argv)
{
    @autoreleasepool {
        Options options = {.x = 100.0,
                           .y = 100.0,
                           .width = 1200.0,
                           .height = 800.0,
                           .patterns = "photoish",
                           .files = NULL,
                           .mode = "still",
                           .title = "SideScopes measurement content",
                           .period = 2.0,
                           .fps = 30.0};
        if (!parseOptions(argc, argv, &options)) {
            return 2;
        }

        [NSApplication sharedApplication];
        // An accessory never activates, so the application under measurement
        // keeps the keyboard focus its shortcuts need.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        NSWindow* window = newContentWindow(&options);
        const double backing = window.backingScaleFactor;
        NSMutableArray* images = buildImages(&options, (size_t)(options.width * backing * PanSurplus),
                                             (size_t)(options.height * backing));
        if (images.count == 0) {
            fprintf(stderr, "content_window: no content to show\n");

            return 3;
        }

        ContentController* controller = [[ContentController alloc] init];
        controller.window = window;
        controller.images = images;
        controller.parent = getppid();
        // The video pan's travel, in the image's own pixels. Reported below so
        // that a run records how far the content moved between frames rather
        // than leaving a reader to work it out from three constants.
        controller.panTravelPixels =
            floor((1.0 - PanWindow) * (double)CGImageGetWidth((__bridge CGImageRef)images[0]));
        controller.panDirection = 1.0;
        [controller showImageAtIndex:0];
        startTimers(controller, &options);
        if (strcmp(options.mode, "video") == 0) {
            // Before the ready line, which is where the caller stops reading.
            printf("pan %.0f,%.0f,%.1f\n", VideoPanPixelsPerFrame, controller.panTravelPixels, options.fps);
        }
        reportReady(window, images.count);

        [NSApp run];
    }

    return 0;
}
