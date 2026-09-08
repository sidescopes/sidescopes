#pragma once

#include <cstddef>

namespace sidescopes {

/// An offline reader for the license notices embedded in the application.
class LicenseWindow
{
public:
    void open();
    void draw();

private:
    bool m_open = false;
    std::size_t m_selected = 0;
};

}  // namespace sidescopes
