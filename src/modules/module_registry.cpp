#include "modules/module_registry.h"

#include <cstdio>

namespace sidescopes {
namespace {

void HostLog(const SsHost*, uint32_t level, const char* message)
{
    std::fprintf(stderr, "sidescopes module [%s]: %s\n",
                 level == SS_LOG_ERROR     ? "error"
                 : level == SS_LOG_WARNING ? "warning"
                                           : "info",
                 message);
}

const void* HostGetExtension(const SsHost*, const char*)
{
    return nullptr;  // no host extensions yet
}

}  // namespace

ModuleRegistry::ModuleRegistry()
{
    m_host.abi_major = SS_ABI_MAJOR;
    m_host.abi_minor = SS_ABI_MINOR;
    m_host.host_data = this;
    m_host.get_extension = HostGetExtension;
    m_host.log = HostLog;
}

ModuleRegistry::~ModuleRegistry()
{
    for (const SsModuleEntry* module : m_modules) {
        module->deinit();
    }
}

bool ModuleRegistry::RegisterModule(const SsModuleEntry& entry)
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
        m_scopes.push_back(RegisteredScope{entry.descriptor(index), &entry});
    }

    return true;
}

const RegisteredScope* ModuleRegistry::FindScope(const std::string& id) const
{
    for (const RegisteredScope& scope : m_scopes) {
        if (id == scope.descriptor->id) {
            return &scope;
        }
    }

    return nullptr;
}

ScopeInstance ModuleRegistry::CreateInstance(const std::string& id) const
{
    const RegisteredScope* scope = FindScope(id);
    if (!scope) {
        return ScopeInstance{};
    }

    return ScopeInstance{scope->module->create(id.c_str(), &m_host)};
}

ModuleRegistry& BuiltinModules()
{
    static ModuleRegistry registry;
    static const bool registered = [] {
        registry.RegisterModule(VectorscopeModuleEntry);
        return true;
    }();
    (void)registered;

    return registry;
}

}  // namespace sidescopes
