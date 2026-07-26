#include "core/environment.h"

#include <cstdlib>

namespace sidescopes {

std::string environmentValue(const char* name)
{
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::string();
    }
    std::string result(value);
    std::free(value);

    return result;
#else
    const char* value = std::getenv(name);

    return value ? std::string(value) : std::string();
#endif
}

}  // namespace sidescopes
