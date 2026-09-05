#include <cstddef>
#include <cstdint>
#include <string_view>

#include "boundary_properties.h"
#include "temp_file.h"

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer requires this entry point.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    static sidescopes::test::TempDir directory{"preferences-fuzz"};
    sidescopes::test::checkPreferencesProperties(std::string_view{reinterpret_cast<const char*>(data), size},
                                                 directory.path());
    return 0;
}
