#pragma once

#include "app/version.h"

namespace sidescopes {

/// The About window: the version the body leads with, the project links, and
/// the license. It owns whether it is on screen - the View menu opens it, its
/// own close button shuts it - so the shell only asks for it and draws it.
class AboutWindow
{
public:
    /// Puts the window on screen.
    void open();

    /// Draws the window when it is open, leading with @p version.
    void draw(const VersionInfo& version);

private:
    bool m_open = false;
};

}  // namespace sidescopes
