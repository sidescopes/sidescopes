#include <cstddef>
#include <cstdint>
#include <span>

#include "boundary_properties.h"

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer requires this entry point.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    sidescopes::test::checkGeometryProperties(std::span<const uint8_t>{data, size});
    return 0;
}
