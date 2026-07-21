/* SideScopes scope-module ABI.
 *
 * One C boundary carries every analysis scope - the built-ins included.
 * Modules are pure analysis: deterministic functions of (frame, region,
 * settings) producing an image plus declarative overlay data. The host
 * owns capture, region math, change detection, textures, panes,
 * gestures, menus, preferences, and all styling; nothing platform- or
 * toolkit-specific ever crosses this boundary.
 *
 * Shape: a tiny core that freezes at 1.0, growing only through
 * extensions queried by string id on both sides (the CLAP discipline).
 * While the ABI version below is 0.x there is no stability promise.
 *
 * Conventions:
 * - C99 subset. Fixed-width integers; no enums, bools, or bitfields in
 *   structs (fields are uint32_t, values are named constants).
 * - No allocation crosses the boundary. Frames are host-owned and valid
 *   only during the call. Images are module-owned and valid until the
 *   next accumulate or destroy on that instance. Overlay queries fill
 *   caller-provided arrays. Descriptor strings are module-owned and
 *   valid until the module's deinit.
 * - No exceptions or longjmp cross the boundary, either direction.
 * - Each side frees only what it allocated.
 * - Thread contracts are stated per function in brackets:
 *     [init-thread]   the single thread that calls init and deinit;
 *     [owning-thread] the thread that created a given instance;
 *     [any-thread]    callable from any thread, no ordering assumed;
 *     [thread-safe]   callable concurrently from several threads.
 */

#ifndef SIDESCOPES_MODULE_H
#define SIDESCOPES_MODULE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The host accepts a module when the majors are equal; the minor may
 * differ, since a newer minor only adds optional, backward-compatible
 * extensions. Until 1.0 the layout itself may still change under a minor
 * bump; rebuild modules against the current header. */
#define SS_ABI_MAJOR 0u
#define SS_ABI_MINOR 2u

/* ---- core types ---------------------------------------------------- */

#define SS_COLOR_SPACE_UNKNOWN 0u
#define SS_COLOR_SPACE_SRGB 1u

/* Host-owned; every field valid only for the duration of the call.
 * `sequence` strictly increases per captured frame, so a scope handed the
 * same frame twice can compare it to skip recomputation. */
typedef struct SsFrameView
{
    const uint8_t* bgra;
    int32_t stride_bytes;
    int32_t width;
    int32_t height;
    uint32_t color_space;
    uint64_t sequence;
} SsFrameView;

typedef struct SsRect
{
    int32_t x, y, width, height;
} SsRect;

/* Channels in 0..255; fractional values carry sub-code precision. */
typedef struct SsColor
{
    float r, g, b;
} SsColor;

/* Module-owned; valid until the next accumulate or destroy on the
 * instance that returned it. The host copies out what it keeps and
 * re-uploads its texture only when `sequence` changes, so a module
 * advances `sequence` when the image content changes and leaves it as is
 * to mean "identical to the last image". */
typedef struct SsImageView
{
    const uint8_t* rgba;
    int32_t width;
    int32_t height;
    uint64_t sequence;
} SsImageView;

/* ---- declarative parameters ---------------------------------------- */

/* Mapped through the host's exponential intensity gesture (scroll,
 * double-click reset, percent display); persisted raw. */
#define SS_PARAM_INTENSITY 1u
/* Integer slider, typically the sampling stride. */
#define SS_PARAM_INT 2u
/* A labeled context-menu submenu of exclusive choices; the value is the
 * zero-based choice index. */
#define SS_PARAM_CHOICE 3u

typedef struct SsParamInfo
{
    const char* key;   /* persisted by the host as "<scope_id>.<key>" */
    const char* label; /* user-facing */
    uint32_t kind;     /* SS_PARAM_* */
    double min_value;
    double max_value;
    double default_value;
    double intensity_shift;     /* INTENSITY only: display headroom */
    const char* menu_label;     /* CHOICE only: the submenu's title */
    const char* const* choices; /* CHOICE only: null-terminated labels */
} SsParamInfo;

typedef struct SsParamValue
{
    const char* key;
    double value;
} SsParamValue;

/* ---- declarative overlay data --------------------------------------
 * Normalized [0,1] scope-image coordinates. The host applies stroke
 * widths, colors, fonts, and theme; modules state only geometry and
 * meaning. */

#define SS_PRIMITIVE_LINE 1u
#define SS_PRIMITIVE_CIRCLE 2u
#define SS_PRIMITIVE_TARGET_BOX 3u
#define SS_PRIMITIVE_TEXT 4u

#define SS_STROKE_GRID 0u
#define SS_STROKE_GRID_MAJOR 1u
#define SS_STROKE_ACCENT 2u
#define SS_STROKE_SKIN_TONE 3u

#define SS_PRIMITIVE_FLAG_TARGET_PRIMARY 0x1u
/* TEXT drawn only when the pane is roomy (the host decides roomy). */
#define SS_PRIMITIVE_FLAG_TEXT_MAJOR_ONLY 0x2u

typedef struct SsGraticulePrimitive
{
    uint32_t kind;   /* SS_PRIMITIVE_* */
    uint32_t stroke; /* SS_STROKE_* */
    /* LINE: (x0,y0)-(x1,y1). CIRCLE: center (x0,y0), radius x1.
     * TARGET_BOX and TEXT: anchor (x0,y0). */
    float x0, y0, x1, y1;
    uint32_t flags;
    char label[16]; /* TEXT and labeled targets; NUL-terminated */
} SsGraticulePrimitive;

#define SS_MARKER_POINT 1u /* both coordinates */
#define SS_MARKER_LEVEL 2u /* horizontal line at y */
#define SS_MARKER_VALUE 3u /* vertical line at x */

typedef struct SsMarker
{
    uint32_t kind; /* SS_MARKER_* */
    float x, y;
    /* Horizontal confinement for banded layouts (parade thirds,
     * per-channel histogram bands); 0..1 covers the full width. */
    float band_from, band_to;
    /* 1 = red, 2 = green, 4 = blue; the host merges coincident markers
     * and derives their color from the union of masks. */
    uint32_t channel_mask;
} SsMarker;

/* ---- host ----------------------------------------------------------- */

#define SS_LOG_INFO 0u
#define SS_LOG_WARNING 1u
#define SS_LOG_ERROR 2u

typedef struct SsHost
{
    uint32_t abi_major;
    uint32_t abi_minor;
    void* host_data;
    /* [thread-safe] Extension vtables are host-owned and valid for the
     * host's lifetime; null when the id is unknown. */
    const void* (*get_extension)(const struct SsHost* host, const char* id);
    /* [thread-safe] */
    void (*log)(const struct SsHost* host, uint32_t level, const char* message);
} SsHost;

/* ---- scope instance -------------------------------------------------
 * Single-threaded: the creating thread owns the instance. The host
 * creates one instance for analysis (worker thread) and a separate one
 * for overlays and projections (main thread), configuring both with
 * identical values whenever settings change. The host destroys every
 * instance before it calls the module's deinit, and the SsHost passed to
 * create outlives all instances. */

typedef struct SsScopeInstance SsScopeInstance;

struct SsScopeInstance
{
    void* instance_data;

    /* [owning-thread] Applies a full parameter set; unknown keys are
     * ignored, omitted keys keep their current values. Idempotent. */
    bool (*configure)(SsScopeInstance* instance, const SsParamValue* values, uint32_t count);

    /* [owning-thread] A pure function of the frame, region, and the
     * configured parameters: rebuilds the image from scratch. `region` is
     * given in frame-buffer pixel coordinates. The host may skip calls
     * arbitrarily (change detection) and call again after any number of
     * skipped frames. */
    bool (*accumulate)(SsScopeInstance* instance, const SsFrameView* frame, SsRect region);

    /* [owning-thread] Module-owned; valid until the next accumulate or
     * destroy on this instance. */
    SsImageView (*image)(const SsScopeInstance* instance);

    /* [owning-thread] Fills up to `capacity` primitives; returns the
     * count the full graticule needs (call again with more room when
     * the return exceeds capacity). Reflects configured parameters. */
    uint32_t (*graticule)(const SsScopeInstance* instance, SsGraticulePrimitive* primitives, uint32_t capacity);

    /* [owning-thread] Marker positions for one color, same capacity
     * pattern. The host styles, merges, and smooths. */
    uint32_t (*markers)(const SsScopeInstance* instance, SsColor color, SsMarker* markers, uint32_t capacity);

    /* [owning-thread] Instance extensions, queried by string id; null when
     * unknown. The returned vtable is module-owned and valid for the
     * instance's lifetime (not the host's). */
    const void* (*get_extension)(const SsScopeInstance* instance, const char* id);

    /* [owning-thread] */
    void (*destroy)(SsScopeInstance* instance);
};

/* ---- descriptor ------------------------------------------------------
 * Module-owned; valid from init to deinit. */

#define SS_SCOPE_KEEP_ASPECT 0x1u
/* The scope's image can hold the host's pin markers; the pin tool stands
 * up whenever a declaring scope is on screen. */
#define SS_SCOPE_PIN_TARGET 0x2u

typedef struct SsScopeDescriptor
{
    const char* id;       /* reverse-DNS, e.g. "org.sidescopes.vectorscope" */
    const char* name;     /* menu and tooltip text */
    char letter;          /* toolbar chip / hotkey / prefs letter; 0 = none */
    int32_t image_width;  /* 0 = dynamic */
    int32_t image_height; /* 0 = dynamic */
    uint32_t flags;       /* SS_SCOPE_* */
    const SsParamInfo* params;
    uint32_t param_count;
    /* The pane width-to-height shape the trace reads best at, scored by the
     * host's automatic layout; 0 lets the host choose. */
    float preferred_aspect;
} SsScopeDescriptor;

/* ---- module entry: the one exported symbol -------------------------- */

typedef struct SsModuleEntry
{
    uint32_t abi_major;
    uint32_t abi_minor;

    /* [init-thread] Called once before anything else; a false return
     * means the module is unusable and only deinit may follow. */
    bool (*init)(void);
    /* [init-thread] Matched with init, called exactly once. */
    void (*deinit)(void);

    /* [thread-safe] Constant between init and deinit. */
    uint32_t (*scope_count)(void);
    /* [thread-safe] index < scope_count(). */
    const SsScopeDescriptor* (*descriptor)(uint32_t index);

    /* [any-thread] Inert creation: must not call host functions (the
     * host pointer is retained for later use). Returns null on unknown
     * ids or failure. */
    SsScopeInstance* (*create)(const char* scope_id, const SsHost* host);
} SsModuleEntry;

/* Dynamically loaded modules export exactly this symbol:
 *
 *   extern const SsModuleEntry ss_module_entry;
 *
 * Statically registered modules hand the same struct to the host's
 * registry. */

#ifdef __cplusplus
}
#endif

#endif /* SIDESCOPES_MODULE_H */
