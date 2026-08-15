#include "web/demo_picture.h"

#include <cstddef>

namespace sidescopes {
namespace {

/// Red and blue exchanged, which is the whole difference between the order the
/// capture seam speaks and the order a texture wants. Alpha and green sit in
/// the same place in both, so this is the only conversion either direction
/// needs - and it is its own function because it is performed twice, once each
/// way, and a hand-inlined second copy is how the two would come to disagree.
void swapRedAndBlue(const std::vector<uint8_t>& from, std::vector<uint8_t>& into)
{
    into.resize(from.size());
    for (std::size_t at = 0; at + 3 < from.size(); at += 4) {
        into[at] = from[at + 2];
        into[at + 1] = from[at + 1];
        into[at + 2] = from[at];
        into[at + 3] = from[at + 3];
    }
}

}  // namespace

uint8_t* DemoPicture::decodeInto(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    m_width = width;
    m_height = height;
    m_decoded.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0u);

    return m_decoded.data();
}

void DemoPicture::adoptDecoded()
{
    swapRedAndBlue(m_decoded, m_original);
    m_adjustDue = true;
    rebuild();
}

bool DemoPicture::setAdjustments(const ImageAdjustments& wanted)
{
    if (wanted == m_adjustments) {
        return false;
    }
    m_adjustments = wanted;
    m_adjustDue = true;

    return true;
}

bool DemoPicture::refresh()
{
    if (!m_adjustDue) {
        return false;
    }
    rebuild();

    return true;
}

void DemoPicture::rebuild()
{
    m_adjustDue = false;
    if (m_original.empty()) {
        return;
    }
    m_analysed.resize(m_original.size());
    applyAdjustments(m_original.data(), m_analysed.data(), m_original.size() / 4u, m_adjustments);

    // ONE pass, two consumers: the canvas gets the same pixels the engines
    // were given, in the order a texture wants them.
    swapRedAndBlue(m_analysed, m_display.rgba);
    m_display.width = m_width;
    m_display.height = m_height;
    m_display.sequence += 1u;
    m_pixelsFresh = true;
}

bool DemoPicture::hasFreshPixels() const
{
    return m_pixelsFresh && !m_analysed.empty();
}

void DemoPicture::pixelsTaken()
{
    m_pixelsFresh = false;
}

bool DemoPicture::empty() const
{
    return m_analysed.empty();
}

int DemoPicture::width() const
{
    return m_width;
}

int DemoPicture::height() const
{
    return m_height;
}

const std::vector<uint8_t>& DemoPicture::analysed() const
{
    return m_analysed;
}

const ScopeImage& DemoPicture::display() const
{
    return m_display;
}

}  // namespace sidescopes
