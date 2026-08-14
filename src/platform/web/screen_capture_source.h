#pragma once

#include <cstdint>

namespace sidescopes {

/// @brief What the browser build captures instead of a screen.
///
/// A page cannot read another program's window and never will - that is the
/// security model working. So this platform's capture source is fed by its
/// HOST rather than by an operating system: the demo hands it the photograph
/// the visitor chose, and a browser extension would hand it frames from
/// `tabCapture`. Either way they arrive through the ordinary
/// ScreenCaptureSource and reach the analysis the same way a desktop's do,
/// so nothing above this line knows the difference.
///
/// The pixels are BGRA, bottom-to-top order irrelevant - the same layout the
/// desktop backends deliver, so no scope needs a second reading of them.
///
/// Does nothing until a capture has been started, which is what makes it safe
/// to call from a page's own event handlers: a picture that arrives before
/// the application is ready is simply the frame nobody asked for.
///
/// @param bgra   the pixels, @p width * @p height * 4 bytes.
/// @param width  in pixels.
/// @param height in pixels.
void submitCapturedPicture(const uint8_t* bgra, int width, int height);

}  // namespace sidescopes
