#include "app/border_label.h"

#include <cstddef>

namespace sidescopes {

std::string borderLabelFrom(const std::string& title, const std::string& fallback)
{
    const std::string& label = title.empty() ? fallback : title;
    // A sanity bound only: the platform draw truncates visually to fit.
    constexpr std::size_t MaxLabelBytes = 96;
    if (label.size() <= MaxLabelBytes) {
        return label;
    }
    std::size_t cut = MaxLabelBytes;
    while (cut > 0 && (static_cast<unsigned char>(label[cut]) & 0xC0) == 0x80) {
        --cut;
    }

    return label.substr(0, cut) + "...";
}

}  // namespace sidescopes
