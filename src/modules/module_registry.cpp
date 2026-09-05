#include "modules/module_registry.h"

#include <algorithm>
#include <cmath>
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
        "org.sidescopes.vectorscope", "org.sidescopes.waveform",  "org.sidescopes.waveform.luma",
        "org.sidescopes.parade",      "org.sidescopes.histogram",
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
    const char* severity = level == SS_LOG_ERROR ? "error" : level == SS_LOG_WARNING ? "warning" : "info";
    // Both: a terminal is where a developer looks, and the recorded log is the
    // only place a user launching from Finder or Explorer can be asked to.
    std::fprintf(stderr, "sidescopes module [%s]: %s\n", severity, message);
    SS_DIAG(Modules, "module [%s] %s", severity, message);
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
    if (count > arena.max_size()) {
        return nullptr;
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

bool validParameter(const SsParamInfo& parameter)
{
    if (!parameter.key || !parameter.key[0] || !parameter.label || !std::isfinite(parameter.min_value) ||
        !std::isfinite(parameter.max_value) || !std::isfinite(parameter.default_value) ||
        parameter.min_value > parameter.max_value) {
        return false;
    }
    if (parameter.kind == SS_PARAM_CHOICE) {
        return parameter.menu_label && parameter.choices && parameter.choices[0];
    }
    return parameter.kind != SS_PARAM_INTENSITY || std::isfinite(parameter.intensity_shift);
}

bool validDescriptor(const SsScopeDescriptor* descriptor)
{
    if (!descriptor || !descriptor->id || !descriptor->id[0] || !descriptor->name ||
        (descriptor->param_count != 0 && !descriptor->params)) {
        return false;
    }
    for (uint32_t index = 0; index < descriptor->param_count; ++index) {
        if (!validParameter(descriptor->params[index])) {
            return false;
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (std::strcmp(descriptor->params[index].key, descriptor->params[previous].key) == 0) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

ModuleRegistry::ModuleRegistry()
    : m_stateReport(diagAddStateReport([this] { reportState(); }))
{
    m_host.abi_major = SS_ABI_MAJOR;
    m_host.abi_minor = SS_ABI_MINOR;
    m_host.host_data = this;
    m_host.get_extension = hostGetExtension;
    m_host.log = hostLog;
}

void ModuleRegistry::reportState() const
{
    SS_DIAG(Modules, "registered modules=%d scopes=%d", static_cast<int>(m_modules.size()),
            static_cast<int>(m_scopes.size()));
    for (const RegisteredScope& scope : m_scopes) {
        SS_DIAG(Modules, "scope id=%s letter=%c", scope.descriptor->id,
                scope.descriptor->letter == 0 ? '-' : scope.descriptor->letter);
    }
    for (const std::string& failure : m_failures) {
        SS_DIAG(Modules, "%s", failure.c_str());
    }
}

void ModuleRegistry::recordFailure(const std::string& message)
{
    m_failures.push_back(message);
    std::fprintf(stderr, "sidescopes modules: %s\n", message.c_str());
    SS_DIAG(Modules, "%s", message.c_str());
}

ModuleRegistry::~ModuleRegistry()
{
    m_stateReport = {};
    for (const SsModuleEntry* module : m_modules) {
        module->deinit();
    }
}

bool ModuleRegistry::registerModule(const SsModuleEntry& entry)
{
    if (std::find(m_modules.begin(), m_modules.end(), &entry) != m_modules.end()) {
        return true;
    }
    if (entry.abi_major != SS_ABI_MAJOR || (SS_ABI_MAJOR == 0 && entry.abi_minor != SS_ABI_MINOR)) {
        recordFailure("rejected ABI " + std::to_string(entry.abi_major) + "." + std::to_string(entry.abi_minor) +
                      " (host " + std::to_string(SS_ABI_MAJOR) + "." + std::to_string(SS_ABI_MINOR) + ")");
        return false;
    }
    if (!entry.init || !entry.deinit || !entry.scope_count || !entry.descriptor || !entry.create) {
        recordFailure("a module is missing required entry points");
        return false;
    }
    // Reserve before initialization so ownership cannot be lost to an allocation
    // failure between init and registration.
    m_modules.reserve(m_modules.size() + 1);
    if (!entry.init()) {
        entry.deinit();
        recordFailure("a module refused to initialize");
        return false;
    }

    m_modules.push_back(&entry);
    const uint32_t count = entry.scope_count();
    for (uint32_t index = 0; index < count; ++index) {
        registerScope(entry, index, count);
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

void ModuleRegistry::registerScope(const SsModuleEntry& entry, uint32_t index, uint32_t count)
{
    const SsScopeDescriptor* descriptor = entry.descriptor(index);
    if (!validDescriptor(descriptor)) {
        recordFailure("invalid descriptor at index " + std::to_string(index) + " of " + std::to_string(count));
        return;
    }
    if (findScope(descriptor->id)) {
        recordFailure("duplicate scope id " + std::string(descriptor->id));
        return;
    }
    m_scopes.push_back(RegisteredScope{descriptor, &entry});
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
    SsScopeInstance* instance = scope->module->create(id.c_str(), &m_host);
    if (instance && (!instance->configure || !instance->accumulate || !instance->image || !instance->graticule ||
                     !instance->markers || !instance->get_extension || !instance->destroy)) {
        // A rejected handle can be reclaimed only through its own destroy
        // function. Never let a missing required operation reach a caller.
        if (instance->destroy) {
            instance->destroy(instance);
        }
        SS_DIAG(Modules, "scope id=%s returned an incomplete instance", id.c_str());
        return ScopeInstance{};
    }
    return ScopeInstance{instance};
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
