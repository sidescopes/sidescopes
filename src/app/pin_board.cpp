#include "app/pin_board.h"

namespace sidescopes {

bool PinBoard::empty() const
{
    return m_colors.empty();
}

std::size_t PinBoard::size() const
{
    return m_colors.size();
}

const FloatColor& PinBoard::color(std::size_t index) const
{
    return m_colors[index];
}

const std::vector<FloatColor>& PinBoard::colors() const
{
    return m_colors;
}

void PinBoard::pin(const FloatColor& color)
{
    if (m_colors.size() >= Maximum) {
        m_colors.erase(m_colors.begin());
    }
    m_colors.push_back(color);
    // A fresh pin is the reference the user wants to compare against.
    m_comparator = static_cast<int>(m_colors.size()) - 1;
}

void PinBoard::restore(const std::vector<FloatColor>& colors, int comparator)
{
    m_colors = colors;
    if (m_colors.size() > Maximum) {
        m_colors.resize(Maximum);
    }
    const bool selects = comparator >= 0 && comparator < static_cast<int>(m_colors.size());
    m_comparator = selects ? comparator : -1;
}

void PinBoard::removeAt(std::size_t index)
{
    if (index >= m_colors.size()) {
        return;
    }
    m_colors.erase(m_colors.begin() + static_cast<std::ptrdiff_t>(index));
    const int removed = static_cast<int>(index);
    if (m_comparator == removed) {
        m_comparator = -1;
    } else if (m_comparator > removed) {
        --m_comparator;
    }
}

void PinBoard::clear()
{
    m_colors.clear();
    m_comparator = -1;
}

int PinBoard::comparator() const
{
    return m_comparator;
}

bool PinBoard::hasComparator() const
{
    return m_comparator >= 0;
}

const FloatColor& PinBoard::comparatorColor() const
{
    return m_colors[static_cast<std::size_t>(m_comparator)];
}

void PinBoard::selectComparator(int index)
{
    m_comparator = index;
}

int PinBoard::managed() const
{
    return m_managed;
}

void PinBoard::manage(int index)
{
    m_managed = index;
}

}  // namespace sidescopes
