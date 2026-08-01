// The PipeWire consumer behind the portal session. Only shared-memory
// buffers are offered - a scopes tool reads every pixel on the CPU, so a
// DMA-BUF path would add a GPU import step just to copy back out - and only
// the BGRA-family formats, which every compositor's screencast serves.
//
// Delivery is damage-driven: a still screen under a still pointer sends
// nothing, which the frame loop already treats as the screen being still.
// POINTER MOTION COUNTS AS DAMAGE once cursor metadata is asked for, and that
// is the point rather than a cost: on a Wayland session the compositor is the
// ONLY thing that knows where the pointer is, and this stream is the only
// channel it tells us over.

#include "platform/linux/pipewire_stream.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>

#include <cstring>

#include "core/diagnostics.h"
#include "platform/linux/stream_pointer.h"

namespace sidescopes {

/// Everything the callbacks touch, owned across the stream's life. A plain
/// struct behind a pointer keeps the PipeWire headers out of the platform
/// header.
struct PipeWireStreamState
{
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook streamListener{};
    spa_video_info_raw format{};
    bool formatKnown = false;
    FrameMailbox* mailbox = nullptr;
    FrameBuffer buffer;
    uint64_t sequence = 0;
    /// Where this stream sits on the desktop, so a cursor position stated in
    /// frame pixels can leave as a desktop point. The frame size is filled in
    /// once the format is negotiated.
    StreamPlacement placement;
    /// Whether the buffers carry a usable cursor position, stated once and
    /// restated on change rather than per buffer.
    DiagOnChange<int> cursorMetaState{DiagChannel::Perf};
    PipeWireVideoStream::ErrorCallback onError;
};

namespace {

/// What a cursor metadata block costs for a bitmap of @p side pixels square,
/// in the layout the SPA headers define. The size is the whole negotiation -
/// see CursorBitmapSide, which is where the number that matters is pinned.
constexpr int cursorMetaBytes(int side)
{
    return static_cast<int>(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap)) + side * side * 4;
}

/// One process-wide pw_init; the library tolerates but counts them.
void ensurePipeWireInit()
{
    static const bool initialized = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    (void)initialized;
}

void onStreamParamChanged(void* data, uint32_t id, const spa_pod* param)
{
    auto* state = static_cast<PipeWireStreamState*>(data);
    if (param == nullptr || id != SPA_PARAM_Format) {
        return;
    }
    uint32_t mediaType = 0;
    uint32_t mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video ||
        mediaSubtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
    if (spa_format_video_raw_parse(param, &state->format) < 0) {
        return;
    }
    state->formatKnown = true;
    state->placement.frameWidth = static_cast<int>(state->format.size.width);
    state->placement.frameHeight = static_cast<int>(state->format.size.height);

    // Answer with the buffer contract: shared memory only, one data plane, and
    // cursor metadata beside the pixels. A compositor offering no cursor
    // metadata simply attaches none, and the probe falls back to X.
    uint8_t podStorage[512];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podStorage, sizeof(podStorage));
    const spa_pod* params[2];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers,
                                                                       SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
                                                                       SPA_POD_Int(1 << SPA_DATA_MemPtr)));
    params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(cursorMetaBytes(64), cursorMetaBytes(1), cursorMetaBytes(CursorBitmapSide))));
    pw_stream_update_params(state->stream, params, 2);
}

/// States what the buffers carry - once, and again on any change. Whether the
/// pointer can be followed at all on a Wayland session rests entirely on this
/// one block being present.
void reportCursorMeta(PipeWireStreamState* state, spa_buffer* buffer, const spa_meta_cursor* cursor)
{
    const int carried = cursor == nullptr ? 0 : (spa_meta_cursor_is_valid(cursor) != 0 ? 2 : 1);
    if (!state->cursorMetaState.shouldLog(carried)) {
        return;
    }
    SS_DIAG(Perf, "stream cursor metadata %s (%u metas on the buffer)",
            carried == 2 ? "carries a position" : (carried == 1 ? "present but not over this source" : "ABSENT"),
            buffer->n_metas);
}

/// Publishes where this buffer says the pointer is, or clears the position when
/// it says nothing. A compositor reports the cursor only while it is over the
/// captured source, so an absent or invalid block is the honest "not here" that
/// sends the probe back to X.
void publishBufferCursor(PipeWireStreamState* state, spa_buffer* buffer)
{
    auto* cursor =
        static_cast<spa_meta_cursor*>(spa_buffer_find_meta_data(buffer, SPA_META_Cursor, sizeof(spa_meta_cursor)));
    // A cursor metadata block that never arrives is this path's whole failure
    // mode, and it arrives or not by SIZE NEGOTIATION - silently, with no
    // error anywhere. So a recording states what the producer allocated, once,
    // and says so again whenever that changes. SPA_META_Cursor is type 5.
    reportCursorMeta(state, buffer, cursor);
    if (cursor == nullptr || spa_meta_cursor_is_valid(cursor) == 0) {
        publishStreamPointer(std::nullopt);
        return;
    }
    publishStreamPointer(streamPointToDesktop(state->placement, cursor->position.x, cursor->position.y));
}

/// Copies one dequeued buffer into the recycled frame and publishes it.
void onStreamProcess(void* data)
{
    auto* state = static_cast<PipeWireStreamState*>(data);
    if (!state->formatKnown) {
        return;
    }
    pw_buffer* dequeued = pw_stream_dequeue_buffer(state->stream);
    if (dequeued == nullptr) {
        return;
    }
    publishBufferCursor(state, dequeued->buffer);
    const spa_data& plane = dequeued->buffer->datas[0];
    const auto* source = static_cast<const uint8_t*>(plane.data);
    const int width = static_cast<int>(state->format.size.width);
    const int height = static_cast<int>(state->format.size.height);
    const int sourceStride = plane.chunk->stride != 0 ? plane.chunk->stride : width * 4;
    if (source != nullptr && width > 0 && height > 0) {
        const int stride = width * 4;
        state->buffer.sizeTo(static_cast<std::size_t>(stride) * height);
        for (int row = 0; row < height; ++row) {
            std::memcpy(state->buffer.data.data() + static_cast<std::size_t>(row) * stride,
                        source + static_cast<std::size_t>(row) * sourceStride, static_cast<std::size_t>(stride));
        }
        state->buffer.strideBytes = stride;
        state->buffer.width = width;
        state->buffer.height = height;
        state->buffer.colorSpace = ColorSpaceHint::Srgb;
        state->buffer.format = PixelFormat::Bgra8;
        state->buffer.sequence = ++state->sequence;
        state->buffer.sourceX = 0;
        state->buffer.sourceY = 0;
        state->buffer.sourceWidth = 0;
        state->buffer.sourceHeight = 0;
        state->buffer = state->mailbox->publish(std::move(state->buffer));
    }
    pw_stream_queue_buffer(state->stream, dequeued);
}

void onStreamStateChanged(void* data, pw_stream_state, pw_stream_state current, const char* errorMessage)
{
    auto* state = static_cast<PipeWireStreamState*>(data);
    if (current == PW_STREAM_STATE_ERROR || current == PW_STREAM_STATE_UNCONNECTED) {
        if (state->onError) {
            state->onError(errorMessage != nullptr ? errorMessage : "capture stream ended");
        }
    }
}

constexpr pw_stream_events StreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = onStreamStateChanged,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = onStreamParamChanged,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
    .process = onStreamProcess,
    .drained = nullptr,
    .command = nullptr,
    .trigger_done = nullptr,
};

/// The formats offered: BGRA first, then BGRx whose undefined fourth byte
/// reads as alpha nothing consults. Size is left to the source; the cadence
/// range asks the compositor for at most the quality level's rate.
const spa_pod* buildFormatParam(spa_pod_builder* builder, int maxFramesPerSecond)
{
    const spa_rectangle sizeDefault{1920, 1080};
    const spa_rectangle sizeMin{1, 1};
    const spa_rectangle sizeMax{16384, 16384};
    const spa_fraction rateDefault{static_cast<uint32_t>(maxFramesPerSecond), 1};
    const spa_fraction rateMin{0, 1};
    spa_pod_frame frame;
    spa_pod_builder_push_object(builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_format,
                        SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx),
                        0);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_size,
                        SPA_POD_CHOICE_RANGE_Rectangle(&sizeDefault, &sizeMin, &sizeMax), 0);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_CHOICE_RANGE_Fraction(&rateDefault, &rateMin, &rateDefault), 0);
    return static_cast<const spa_pod*>(spa_pod_builder_pop(builder, &frame));
}

}  // namespace

PipeWireVideoStream::PipeWireVideoStream() = default;

PipeWireVideoStream::~PipeWireVideoStream()
{
    stop();
}

bool PipeWireVideoStream::start(const PortalStream& source, int maxFramesPerSecond, FrameMailbox& mailbox,
                                ErrorCallback onError)
{
    ensurePipeWireInit();
    auto* state = new PipeWireStreamState;
    state->mailbox = &mailbox;
    state->onError = std::move(onError);
    state->placement.originX = source.originX;
    state->placement.originY = source.originY;
    state->placement.widthPoints = source.widthPoints;
    state->placement.heightPoints = source.heightPoints;
    m_state = state;

    state->loop = pw_thread_loop_new("sidescopes-capture", nullptr);
    if (state->loop == nullptr || pw_thread_loop_start(state->loop) != 0) {
        stop();
        return false;
    }

    pw_thread_loop_lock(state->loop);
    state->context = pw_context_new(pw_thread_loop_get_loop(state->loop), nullptr, 0);
    state->core =
        state->context != nullptr ? pw_context_connect_fd(state->context, source.pipewireFd, nullptr, 0) : nullptr;
    if (state->core == nullptr) {
        pw_thread_loop_unlock(state->loop);
        stop();
        return false;
    }

    pw_properties* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                  PW_KEY_MEDIA_ROLE, "Screen", nullptr);
    state->stream = pw_stream_new(state->core, "sidescopes", properties);
    if (state->stream == nullptr) {
        pw_thread_loop_unlock(state->loop);
        stop();
        return false;
    }
    pw_stream_add_listener(state->stream, &state->streamListener, &StreamEvents, state);

    uint8_t podStorage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podStorage, sizeof(podStorage));
    const spa_pod* params[1];
    params[0] = buildFormatParam(&builder, maxFramesPerSecond);
    const int connected = pw_stream_connect(
        state->stream, PW_DIRECTION_INPUT, source.nodeId,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    pw_thread_loop_unlock(state->loop);
    if (connected != 0) {
        stop();
        return false;
    }
    return true;
}

void PipeWireVideoStream::stop()
{
    PipeWireStreamState* state = m_state;
    if (state == nullptr) {
        return;
    }
    m_state = nullptr;
    // Nobody is watching the pointer once this stream is gone, and a position
    // left behind describes a pointer free to move unobserved ever since.
    publishStreamPointer(std::nullopt);
    // The error callback reaches into the capture thread's owner; silence it
    // before teardown so a stream dying of being stopped reports nothing.
    if (state->loop != nullptr) {
        pw_thread_loop_lock(state->loop);
        state->onError = nullptr;
        if (state->stream != nullptr) {
            pw_stream_destroy(state->stream);
        }
        if (state->core != nullptr) {
            pw_core_disconnect(state->core);
        }
        if (state->context != nullptr) {
            pw_context_destroy(state->context);
        }
        pw_thread_loop_unlock(state->loop);
        pw_thread_loop_stop(state->loop);
        pw_thread_loop_destroy(state->loop);
    }
    delete state;
}

}  // namespace sidescopes
