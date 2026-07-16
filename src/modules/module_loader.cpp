#include "modules/module_loader.h"

#include <algorithm>
#include <cstdio>
#include <system_error>
#include <vector>

#include "modules/module_registry.h"
#include "sidescopes/module.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace sidescopes {
namespace {

#if defined(_WIN32)
constexpr const char* ModuleExtension = ".dll";
#elif defined(__APPLE__)
constexpr const char* ModuleExtension = ".dylib";
#else
constexpr const char* ModuleExtension = ".so";
#endif

// Loads one module file and registers its entry. Logged failures are the
// caller's cue to keep scanning; nothing here throws.
void loadOne(const std::filesystem::path& file, ModuleRegistry& registry)
{
    const std::string path = file.string();
#if defined(_WIN32)
    // The wide entry point matches the rest of the platform layer; a
    // module directory is never a search path, so the plain load suffices.
    const std::wstring wide = file.wstring();
    HMODULE handle = LoadLibraryW(wide.c_str());
    if (!handle) {
        std::fprintf(stderr, "sidescopes loader: LoadLibrary failed for %s\n", path.c_str());
        return;
    }
    auto* entry = reinterpret_cast<const SsModuleEntry*>(GetProcAddress(handle, "ss_module_entry"));
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "sidescopes loader: dlopen failed for %s: %s\n", path.c_str(), dlerror());
        return;
    }
    auto* entry = reinterpret_cast<const SsModuleEntry*>(dlsym(handle, "ss_module_entry"));
#endif
    if (!entry) {
        std::fprintf(stderr, "sidescopes loader: no ss_module_entry in %s\n", path.c_str());
        // The handle intentionally leaks: see the never-unload note below.
        return;
    }
    // registerModule gates the ABI and logs its own rejections, so the
    // result needs no handling here beyond acknowledging it.
    (void)registry.registerModule(*entry);
    // The handle is deliberately never closed: the module's scope instances
    // may outlive any sensible unload point, so modules live for the process.
}

}  // namespace

bool loadModulesFrom(const std::filesystem::path& directory, ModuleRegistry& registry)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(directory, ec);
    if (ec) {
        std::fprintf(stderr, "sidescopes loader: cannot scan %s: %s\n", directory.string().c_str(),
                     ec.message().c_str());
        return false;
    }

    // The range-for form advances with the throwing operator++, so a
    // mid-scan filesystem error would escape through the static initializer
    // that builds the registry and terminate the process. Advance with the
    // error_code overload instead and stop on the first error. Collect the
    // matches first, then sort: the order the filesystem hands entries back
    // is unspecified, and both the module registration order and the UI
    // order derived from it must be deterministic.
    std::vector<std::filesystem::path> modules;
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            std::fprintf(stderr, "sidescopes loader: scan of %s interrupted: %s\n", directory.string().c_str(),
                         ec.message().c_str());
            break;
        }
        if (it->path().extension() == ModuleExtension) {
            modules.push_back(it->path());
        }
    }

    std::sort(modules.begin(), modules.end());
    for (const std::filesystem::path& file : modules) {
        loadOne(file, registry);
    }
    return true;
}

}  // namespace sidescopes
