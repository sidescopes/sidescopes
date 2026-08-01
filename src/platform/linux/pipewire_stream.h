#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/frame_mailbox.h"
#include "platform/linux/portal_screencast.h"

namespace sidescopes {

struct PipeWireStreamState;

/// A CPU-consuming PipeWire video stream: connects to the node a portal
/// session handed over, negotiates BGRA-family shared-memory buffers at the
/// requested cadence, and publishes every delivered frame into the mailbox.
/// The library runs its own loop thread; start and stop are called from the
/// capture thread.
class PipeWireVideoStream
{
public:
    using ErrorCallback = std::function<void(const std::string& message)>;

    PipeWireVideoStream();
    ~PipeWireVideoStream();

    PipeWireVideoStream(const PipeWireVideoStream&) = delete;
    PipeWireVideoStream& operator=(const PipeWireVideoStream&) = delete;

    /// Takes ownership of @p source's descriptor. The whole portal stream is
    /// taken rather than its node alone because the frames are not the only
    /// thing it carries: where the source sits on the desktop is what turns a
    /// cursor position stated in frame pixels into a desktop point. False when
    /// the connection or stream cannot be built; later stream death arrives
    /// through @p onError.
    [[nodiscard]] bool start(const PortalStream& source, int maxFramesPerSecond, FrameMailbox& mailbox,
                             ErrorCallback onError);

    void stop();

private:
    PipeWireStreamState* m_state = nullptr;
};

}  // namespace sidescopes
