// The portable entry point: constructs the application shell, runs its frame
// loop, and tears it down. Everything the shell owns lives in app.cpp behind
// the App class; this file only bootstraps it so the same main() serves every
// platform.

#include "app/app.h"

int main()
{
    sidescopes::App app;
    if (!app.init()) {
        return 1;
    }
    app.run();
    app.shutdown();

    return 0;
}
