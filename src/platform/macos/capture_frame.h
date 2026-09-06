#pragma once

#import <CoreVideo/CoreVideo.h>

namespace sidescopes {

struct FrameBuffer;
class FrameMailbox;

// Copies a supported, geometry-checked image into its already stamped frame.
// An unavailable surface or allocation failure drops this delivery. The native
// read lock is always released before return or mailbox publication.
bool deliverCapturePixels(CVPixelBufferRef image, FrameBuffer& buffer, FrameMailbox& mailbox) noexcept;

}  // namespace sidescopes
