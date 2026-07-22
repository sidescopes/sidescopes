#pragma once

#include <cstddef>
#include <vector>

#include "core/frame.h"

namespace sidescopes {

/// The pinned reference colors: a small ring the user pins sampled colors
/// into, one of which can be the active comparison reference, plus the pin the
/// management popup currently targets. The colors and the comparison reference
/// are saved with the preferences and come back through @ref restore; the
/// managed pin is popup state and lives only for the session.
class PinBoard
{
public:
    /// Capacity of the pin ring; pinning past it drops the oldest pin.
    static constexpr std::size_t Maximum = 8;

    /// @return Whether nothing is pinned.
    [[nodiscard]] bool empty() const;

    /// @return The number of pinned colors.
    [[nodiscard]] std::size_t size() const;

    /// @return The pinned color at @p index, where 0 is the oldest.
    [[nodiscard]] const FloatColor& color(std::size_t index) const;

    /// @return Every pinned color, oldest first.
    [[nodiscard]] const std::vector<FloatColor>& colors() const;

    /// Pins @p color as the new comparison reference, dropping the oldest pin
    /// when the ring is already full.
    void pin(const FloatColor& color);

    /// Restores a saved board: @p colors become the pins, oldest first, and
    /// @p comparator selects the comparison reference among them, or -1 for
    /// none. A longer list keeps its first @ref Maximum colors, and a
    /// comparator indexing none of them selects nothing.
    void restore(const std::vector<FloatColor>& colors, int comparator);

    /// Removes the pin at @p index, keeping the comparator selection valid.
    void removeAt(std::size_t index);

    /// Removes every pin and clears the comparison reference.
    void clear();

    /// @return The comparison reference's index within colors(), or -1 for none.
    [[nodiscard]] int comparator() const;

    /// @return Whether a comparison reference is selected.
    [[nodiscard]] bool hasComparator() const;

    /// @return The comparison reference color; only valid when hasComparator().
    [[nodiscard]] const FloatColor& comparatorColor() const;

    /// Selects the comparison reference by index, or -1 for none.
    void selectComparator(int index);

    /// @return The pin the management popup targets, or -1 when it is closed.
    [[nodiscard]] int managed() const;

    /// Sets the pin the management popup targets, or -1 to close it.
    void manage(int index);

private:
    std::vector<FloatColor> m_colors;
    int m_comparator = -1;
    int m_managed = -1;
};

}  // namespace sidescopes
