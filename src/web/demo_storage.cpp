#include "web/demo_storage.h"

#include <emscripten/emscripten.h>

#include <cstdlib>

namespace sidescopes {
namespace storage {
namespace {

// The key is namespaced and versioned. Namespaced because a page shares its
// origin's storage with everything else the site may ever put there;
// versioned because the preferences format is explicitly allowed to change
// shape before 1.0 without a migration, and a stale value should be
// discarded rather than half-read.
constexpr const char* StorageKey = "sidescopes.demo.preferences.v1";

// EM_JS rather than EM_ASM: the calls are declared once with their types,
// so a mismatched argument is a compile error rather than a surprise at
// runtime. Both guard on localStorage being reachable at all - a private
// window can refuse it, and throwing out of C++ here would take the whole
// demo down over a preference.
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

}  // namespace

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

void writeSaved(const std::string& text)
{
    jsWriteSaved(StorageKey, text.c_str());
}

}  // namespace storage
}  // namespace sidescopes
