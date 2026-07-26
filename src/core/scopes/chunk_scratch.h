#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sidescopes {

/// @brief Room for the private bin sets a split accumulate gives each chunk.
///
/// Those sets are several times the size of the bins they merge into - a
/// waveform at its widest holds fifty megabytes of them against twelve of
/// bins - and an engine that keeps its own holds them for its whole life
/// rather than for the pass. Scopes accumulate strictly one at a time on one
/// thread, so the host can lend the same room to each in turn and a stack then
/// holds what its largest scope needs instead of the sum.
///
/// An engine built without a host - a test, a benchmark - keeps room of its
/// own, so nothing about a pass depends on the lending having happened.
class ChunkScratch
{
public:
    /// How the host lends: room for @p count values at @p context, contents
    /// unspecified, or null when it has none to lend.
    using Lender = std::uint32_t* (*)(const void* context, std::size_t count);

    /// Takes room from @p lender rather than from its own. A null @p lender
    /// puts it back on its own.
    void lendFrom(Lender lender, const void* context)
    {
        m_lender = lender;
        m_context = context;
    }

    /// @return Room for @p count values, contents unspecified, valid until the
    ///         next borrow. Never null: a lender that declines is answered
    ///         from its own room.
    [[nodiscard]] std::uint32_t* borrow(std::size_t count)
    {
        if (m_lender != nullptr) {
            std::uint32_t* lent = m_lender(m_context, count);
            if (lent != nullptr) {
                return lent;
            }
        }
        m_own.resize(count);

        return m_own.data();
    }

private:
    Lender m_lender = nullptr;
    const void* m_context = nullptr;
    std::vector<std::uint32_t> m_own;
};

}  // namespace sidescopes
