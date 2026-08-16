#pragma once

#include <cstdint>
#include <vector>

#include "core/scopes/scope_types.h"
#include "web/image_adjust.h"

namespace sidescopes {

/// The photograph the lab shows, in the three forms it has to exist in, and
/// the one rule that keeps them honest.
///
/// A page decodes a picture into this; the scopes measure it; the canvas draws
/// it. Those last two must never be different pixels. If the picture on screen
/// and the pixels reaching the engines could drift apart, the lab would teach
/// something false about what a scope is measuring - and would look entirely
/// correct while doing it. So there is ONE adjustment pass and both consumers
/// read its result.
///
/// The three forms, and why each exists:
///
///   - the DECODE, exactly as the page wrote it, kept and never written to
///     again. Every adjustment is computed from here, so dragging a control
///     back and forth cannot degrade the photograph and leave the scopes
///     reporting that damage as the picture's own.
///   - the ANALYSED copy, in the byte order the capture seam speaks, which is
///     what the engines read.
///   - the DISPLAY copy, in the byte order a texture wants, which is what the
///     canvas draws.
///
/// Deliberately holds no texture and knows no graphics backend: uploading is
/// the shell's business, and keeping this free of it is what lets the rule
/// above be tested rather than only looked at.
class LabPicture
{
public:
    /// Room for a decode of this size, for the page to write RGBA into.
    /// Returns nullptr for a size that cannot hold a picture.
    [[nodiscard]] uint8_t* decodeInto(int width, int height);

    /// The decode is complete: take it as the pristine original. The adjusted
    /// and display copies are rebuilt on the next refresh, so a new photograph
    /// inherits whatever the controls are already set to - which is what a
    /// visitor comparing two pictures under one adjustment expects.
    void adoptDecoded();

    /// @return whether this actually changed anything.
    bool setAdjustments(const ImageAdjustments& wanted);

    /// Rebuilds the adjusted and display copies if either is due.
    /// @return whether the display changed, so the caller can upload it.
    bool refresh();

    /// Pixels the analysis has not been given yet.
    [[nodiscard]] bool hasFreshPixels() const;
    void pixelsTaken();

    [[nodiscard]] bool empty() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    /// What the ENGINES read.
    [[nodiscard]] const std::vector<uint8_t>& analysed() const;
    /// What the CANVAS draws. The same pass, in the other byte order.
    [[nodiscard]] const ScopeImage& display() const;

private:
    void rebuild();

    std::vector<uint8_t> m_decoded;
    std::vector<uint8_t> m_original;
    std::vector<uint8_t> m_analysed;
    ScopeImage m_display;
    ImageAdjustments m_adjustments;
    int m_width = 0;
    int m_height = 0;
    bool m_adjustDue = false;
    bool m_pixelsFresh = false;
};

}  // namespace sidescopes
