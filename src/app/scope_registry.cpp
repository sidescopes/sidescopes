#include "app/scope_registry.h"

#include <cstdio>

#include "modules/module_registry.h"

namespace sidescopes {

ScopeRegistry::ScopeRegistry(const ModuleRegistry& modules)
    : m_stateReport(diagAddStateReport([this] { reportState(); }))
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

void ScopeRegistry::reportState() const
{
    // Derived, not remembered: the descriptor still carries the letter the
    // module asked for, so a refusal answers for itself however long after
    // the assignment the recording begins.
    for (const HostScope& scope : m_scopes) {
        if (scope.descriptor == nullptr || scope.descriptor->letter == scope.letter) {
            continue;
        }
        SS_DIAG(Modules, "letter '%c' for %s unavailable; registered letterless", scope.descriptor->letter,
                scope.id.c_str());
    }
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

bool anyPinTarget(const ScopeRegistry& registry, const std::vector<std::string>& scopeIds)
{
    for (const std::string& scopeId : scopeIds) {
        if (scopeId == ColorPickerScopeId) {
            return true;
        }
        const HostScope* hostScope = registry.byId(scopeId);
        if (hostScope != nullptr && hostScope->descriptor != nullptr &&
            (hostScope->descriptor->flags & SS_SCOPE_PIN_TARGET) != 0u) {
            return true;
        }
    }

    return false;
}

bool scopeReadsRegion(const ScopeRegistry& registry, std::string_view scopeId)
{
    // A module descriptor is what asks the worker for a pass over the region;
    // the color picker has none and follows the pointer instead.
    const HostScope* hostScope = registry.byId(scopeId);

    return hostScope != nullptr && hostScope->descriptor != nullptr;
}

}  // namespace sidescopes
