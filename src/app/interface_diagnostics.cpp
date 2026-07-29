#include "app/interface_diagnostics.h"

#include <string>

#include "core/diagnostics.h"

namespace sidescopes {

void reportInterfaceError(const char* message)
{
    // DiagOnChange rather than a plain flag, and not optionally: an error
    // raised inside a draw is raised again on every frame that draw runs, so
    // an undeduped line would fill the recording with one repeated sentence
    // and bury everything else. It holds the last message ANNOUNCED and
    // forgets on each new recording, so a fault that began before recording
    // started is still stated to the recording that follows.
    static DiagOnChange<std::string> announced{DiagChannel::Interface};
    const std::string text = message != nullptr ? message : "";
    if (!announced.shouldLog(text)) {
        return;
    }
    SS_DIAG(Interface, "toolkit_error msg=\"%s\"", text.c_str());
}

}  // namespace sidescopes
