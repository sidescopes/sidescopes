#include "app/scope_registry.h"

#include <cstdio>

#include "modules/module_registry.h"

namespace sidescopes {

ScopeRegistry::ScopeRegistry(const ModuleRegistry& modules)
{
    std::string assigned;
    for (const RegisteredScope& scope : modules.scopes()) {
        const SsScopeDescriptor* descriptor = scope.descriptor;
        char letter = descriptor->letter;
        if (letter != 0) {
            const bool reserved = letter == ColorPickerLetter;
            const bool taken = assigned.find(letter) != std::string::npos;
            if (reserved || taken) {
                std::fprintf(stderr, "sidescopes module: letter '%c' for %s unavailable; registered letterless\n",
                             letter, descriptor->id);
                letter = 0;
            } else {
                assigned.push_back(letter);
            }
        }
        m_scopes.push_back(HostScope{descriptor->id, letter, descriptor, false});
    }
    m_scopes.push_back(HostScope{ColorPickerScopeId, ColorPickerLetter, nullptr, true});
}

const std::vector<HostScope>& ScopeRegistry::scopes() const
{
    return m_scopes;
}

const HostScope* ScopeRegistry::byId(std::string_view id) const
{
    for (const HostScope& scope : m_scopes) {
        if (scope.id == id) {
            return &scope;
        }
    }

    return nullptr;
}

const HostScope* ScopeRegistry::byLetter(char letter) const
{
    if (letter == 0) {
        return nullptr;
    }
    for (const HostScope& scope : m_scopes) {
        if (scope.letter == letter) {
            return &scope;
        }
    }

    return nullptr;
}

int ScopeRegistry::indexOf(std::string_view id) const
{
    for (std::size_t index = 0; index < m_scopes.size(); ++index) {
        if (m_scopes[index].id == id) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

}  // namespace sidescopes
