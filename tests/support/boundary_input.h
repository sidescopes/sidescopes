#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace sidescopes::test {

inline void requireBoundary(bool condition, const char* property)
{
    if (!condition) {
        throw std::runtime_error(property);
    }
}

// The encoding is fixed across standard libraries and platforms. Exhausted
// input supplies zero bytes; no value is ever interpreted as an address.
class BoundaryInput
{
public:
    explicit BoundaryInput(std::span<const uint8_t> input)
        : m_input(input)
    {
    }

    uint8_t byte()
    {
        if (m_input.empty()) {
            return 0;
        }
        const uint8_t value = m_input.front();
        m_input = m_input.subspan(1);
        return value;
    }

    uint64_t word(unsigned bytes)
    {
        uint64_t value = 0;
        for (unsigned index = 0; index < bytes; ++index) {
            value |= static_cast<uint64_t>(byte()) << (index * 8);
        }
        return value;
    }

    int integer()
    {
        return std::bit_cast<int32_t>(static_cast<uint32_t>(word(4)));
    }

    double number()
    {
        static constexpr std::array Edges{0.0,
                                          -0.0,
                                          0.5,
                                          -1.0,
                                          1.0,
                                          8.0,
                                          std::numeric_limits<double>::min(),
                                          std::numeric_limits<double>::denorm_min(),
                                          std::numeric_limits<double>::max(),
                                          -std::numeric_limits<double>::max(),
                                          std::numeric_limits<double>::infinity(),
                                          std::numeric_limits<double>::quiet_NaN()};
        const uint8_t selector = byte();
        return selector % 2 == 0 ? Edges[(selector / 2) % Edges.size()] : std::bit_cast<double>(word(8));
    }

private:
    std::span<const uint8_t> m_input;
};

}  // namespace sidescopes::test
