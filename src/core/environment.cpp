#include "core/environment.h"

#include <cstdlib>
#include <memory>

namespace sidescopes {

std::string environmentValue(const char* name)
{
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::string();
    }
    const std::unique_ptr<char, decltype(&std::free)> ownedValue(value, std::free);
    return std::string(value);
#else
    const char* value = std::getenv(name);

    return value ? std::string(value) : std::string();
#endif
}

}  // namespace sidescopes
