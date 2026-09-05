#include "web/lab_storage.h"

#include <emscripten/emscripten.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/page_allocator.h"
#include "platform/desktop.h"

namespace sidescopes {
namespace storage {
namespace {

// The key is namespaced and versioned. Namespaced because a page shares its
// origin's storage with everything else the site may ever put there;
// versioned because the preferences format is explicitly allowed to change
// shape before 1.0 without a migration, and a stale value should be
// discarded rather than half-read.
constexpr const char* StorageKey = "sidescopes.lab.preferences.v1";

// EM_JS rather than EM_ASM: the calls are declared once with their types,
// so a mismatched argument is a compile error rather than a surprise at
// runtime. Both guard on localStorage being reachable at all - a private
// window can refuse it, and throwing out of C++ here would take the whole
// lab down over a preference.
// clang-format off
// The bodies below are JAVASCRIPT. clang-format reads them as C++ and
// will happily turn `===` into `== =`, which fails at link time in
// Emscripten's JS parser rather than in the compiler - so it is off here.
EM_JS(char*, jsReadSaved, (const char* key), {
    try {
        const value = window.localStorage.getItem(UTF8ToString(key));
        if (value === null) {
            return 0;
        }

        return stringToNewUTF8(value);
    } catch (error) {
        return 0;
    }
});

EM_JS(void, jsWriteSaved, (const char* key, const char* text), {
    try {
        window.localStorage.setItem(UTF8ToString(key), UTF8ToString(text));
    } catch (error) {
        // Storage full, or refused. Forgetting is the acceptable failure.
    }
});

// clang-format on

std::string readSaved()
{
    char* value = jsReadSaved(StorageKey);
    if (value == nullptr) {
        return {};
    }
    std::string text(value);
    std::free(value);

    return text;
}

}  // namespace

std::optional<Preferences> load()
{
    const std::string text = readSaved();
    if (text.empty()) {
        return std::nullopt;
    }
    const std::string path = preferencesFilePath();
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    if (error) {
        return std::nullopt;
    }
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            return std::nullopt;
        }
    }

    return loadPreferences(path);
}

void save(const Preferences& preferences)
{
    const std::string path = preferencesFilePath();
    if (!savePreferences(preferences, path)) {
        return;
    }
    const MappedFile file = mapFileReadOnly(path.c_str());
    if (file.valid()) {
        const std::string text(reinterpret_cast<const char*>(file.data), file.size);
        jsWriteSaved(StorageKey, text.c_str());
    }
}

}  // namespace storage
}  // namespace sidescopes
