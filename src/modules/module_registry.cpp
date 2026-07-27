#include "modules/module_registry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>
#include <vector>

#ifdef SIDESCOPES_MODULES_DYNAMIC
#include "modules/module_loader.h"
#endif

namespace sidescopes {
namespace {

// The built-in scopes in their toolbar order; every other id ranks after them.
// A stable ordering by this rank makes the scope order identical whether the
// modules register in link order (static) or in load order (dynamic, sorted by
// file name), so the toolbar, letters, and stack never depend on the build.
int canonicalRank(std::string_view id)
{
    static constexpr std::string_view Order[] = {
        "org.sidescopes.vectorscope",
        "org.sidescopes.waveform",
        "org.sidescopes.parade",
        "org.sidescopes.histogram",
    };
    for (int index = 0; index < static_cast<int>(std::size(Order)); ++index) {
        if (Order[index] == id) {
            return index;
        }
    }

    return static_cast<int>(std::size(Order));
}

void hostLog(const SsHost*, uint32_t level, const char* message)
{
    std::fprintf(stderr, "sidescopes module [%s]: %s\n",
                 level == SS_LOG_ERROR     ? "error"
                 : level == SS_LOG_WARNING ? "warning"
                                           : "info",
                 message);
}

// The room a split accumulate needs for its per-chunk bins, lent to whichever
// scope is running. One arena per thread that ever asks: scopes accumulate
// strictly one at a time on the analysis thread, so that is one arena for the
// whole stack, and a thread that never accumulates never has one. It grows to
// the largest pass it has served and is not given back, which is the point -
// it stands in for the buffer every engine used to hold for its own life.
uint32_t* hostBorrowScratch(const SsHost*, uint64_t count)
{
    static thread_local std::vector<uint32_t> arena;
    if (count == 0) {
        // Nothing to lend, answered with the arena as it stands - which is
        // also how a caller asks whether this thread has one at all, since a
        // thread that never accumulated never grew one.
        return arena.empty() ? nullptr : arena.data();
    }
    try {
        if (arena.size() < count) {
            arena.resize(static_cast<std::size_t>(count));
        }
    } catch (const std::bad_alloc&) {
        // Declining is a supported answer: the engine falls back to room of
        // its own, which is what it held before there was an arena at all.
        return nullptr;
    }

    return arena.data();
}

constexpr SsHostScratch HostScratch{hostBorrowScratch};

const void* hostGetExtension(const SsHost*, const char* id)
{
    if (std::strcmp(id, SS_EXT_HOST_SCRATCH) == 0) {
        return &HostScratch;
    }

    return nullptr;
}

}  // namespace

ModuleRegistry::ModuleRegistry()
{
    m_host.abi_major = SS_ABI_MAJOR;
    m_host.abi_minor = SS_ABI_MINOR;
    m_host.host_data = this;
    m_host.get_extension = hostGetExtension;
    m_host.log = hostLog;
}

ModuleRegistry::~ModuleRegistry()
{
    for (const SsModuleEntry* module : m_modules) {
        module->deinit();
    }
}

bool ModuleRegistry::registerModule(const SsModuleEntry& entry)
{
    if (entry.abi_major != SS_ABI_MAJOR) {
        std::fprintf(stderr, "sidescopes module: rejected ABI %u.%u (host %u.%u)\n", entry.abi_major, entry.abi_minor,
                     SS_ABI_MAJOR, SS_ABI_MINOR);
        return false;
    }
    if (!entry.init()) {
        return false;
    }

    m_modules.push_back(&entry);
    const uint32_t count = entry.scope_count();
    for (uint32_t index = 0; index < count; ++index) {
        const SsScopeDescriptor* descriptor = entry.descriptor(index);
        if (!descriptor) {
            // A misbehaving module that returns null below its own
            // scope_count() would otherwise be dereferenced at lookup time.
            std::fprintf(stderr, "sidescopes module: null descriptor at index %u of %u\n", index, count);
            continue;
        }
        m_scopes.push_back(RegisteredScope{descriptor, &entry});
    }

    // Keep the scopes in one canonical order regardless of registration order:
    // built-ins first in their toolbar order, then any third-party scopes in
    // the order they registered. A stable sort preserves that trailing order,
    // which is what the letter-collision rule (earlier registration keeps the
    // letter) depends on.
    std::stable_sort(m_scopes.begin(), m_scopes.end(), [](const RegisteredScope& a, const RegisteredScope& b) {
        return canonicalRank(a.descriptor->id) < canonicalRank(b.descriptor->id);
    });

    return true;
}

const RegisteredScope* ModuleRegistry::findScope(const std::string& id) const
{
    for (const RegisteredScope& scope : m_scopes) {
        if (id == scope.descriptor->id) {
            return &scope;
        }
    }
    return nullptr;
}

ScopeInstance ModuleRegistry::createInstance(const std::string& id) const
{
    const RegisteredScope* scope = findScope(id);
    if (!scope) {
        return ScopeInstance{};
    }
    return ScopeInstance{scope->module->create(id.c_str(), &m_host)};
}

ModuleRegistry& builtinModules()
{
    static ModuleRegistry registry;
    static const bool registered = [] {
#ifdef SIDESCOPES_MODULES_DYNAMIC
        // Dev/CI: the modules are separate shared objects the build stamped
        // into this directory. The loader registers whatever it finds.
        loadModulesFrom(SIDESCOPES_MODULES_DIR, registry);
#else
        (void)registry.registerModule(VectorscopeModuleEntry);
        (void)registry.registerModule(WaveformModuleEntry);
        (void)registry.registerModule(HistogramModuleEntry);
#endif
        return true;
    }();
    (void)registered;
    return registry;
}

}  // namespace sidescopes
