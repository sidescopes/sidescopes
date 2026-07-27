#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "sidescopes/module.h"

namespace sidescopes {

class ModuleRegistry;

/// Well-known module scope ids the host special-cases: the parade-to-waveform
/// control alias and the default stack. Every other identity flows through the
/// registry by string.
inline constexpr char VectorscopeScopeId[] = "org.sidescopes.vectorscope";
inline constexpr char WaveformScopeId[] = "org.sidescopes.waveform";
inline constexpr char ParadeScopeId[] = "org.sidescopes.parade";
inline constexpr char HistogramScopeId[] = "org.sidescopes.histogram";

/// The host color picker's reserved scope id; it has no module descriptor and
/// asks nothing of the analysis worker.
inline constexpr char ColorPickerScopeId[] = "org.sidescopes.colorpicker";

/// The letter the host keeps for the color picker; a module descriptor that
/// claims it is registered letterless.
inline constexpr char ColorPickerLetter = 'C';

/// One scope in the host's toolbar order: a module scope, or the host color
/// picker. Identity is the string @ref id; the letter drives the toolbar chip,
/// the hotkey, and the preference string.
struct HostScope
{
    std::string id;                       ///< Module id, or the reserved host id.
    char letter;                          ///< Assigned shortcut letter; 0 = letterless.
    const SsScopeDescriptor* descriptor;  ///< Null for the host color picker.
    bool host;                            ///< True only for the color picker.
};

/// @brief The host's scope identity layer over the module registry.
///
/// Built once from the module registry in registration order, it assigns each
/// module scope its descriptor letter unless the letter is already taken or
/// reserved for the color picker, and appends the host color picker last. It is
/// the single place that answers "which scopes exist, in what order, under what
/// letters" for the toolbar, the shortcuts, the menus, and the preference
/// string.
class ScopeRegistry
{
public:
    explicit ScopeRegistry(const ModuleRegistry& modules);

    /// @return Every scope in toolbar order: the module scopes, then the color
    ///         picker.
    [[nodiscard]] const std::vector<HostScope>& scopes() const;

    /// @return The scope with id @p id, or null when none matches.
    [[nodiscard]] const HostScope* byId(std::string_view id) const;

    /// @return The scope assigned letter @p letter, or null when none is.
    [[nodiscard]] const HostScope* byLetter(char letter) const;

    /// @return The index of the scope with id @p id, or -1 when unknown.
    [[nodiscard]] int indexOf(std::string_view id) const;

private:
    std::vector<HostScope> m_scopes;
};

}  // namespace sidescopes
