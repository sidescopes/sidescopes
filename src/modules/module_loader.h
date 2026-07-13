#pragma once

// The dev/CI dynamic path (-DSIDESCOPES_MODULES_DYNAMIC=ON). Release
// builds register the modules statically and never compile this file.

#include <filesystem>

namespace sidescopes {

class ModuleRegistry;

/// @brief Scans a directory for module files and registers what it finds.
///
/// Scans @p directory for the platform's module files (.dylib / .dll / .so),
/// loads each, resolves the "ss_module_entry" symbol, and registers it.
/// Failures (missing symbol, ABI mismatch, dlopen error) are logged and
/// skipped; the scan never aborts and never throws. Loaded handles stay open
/// for the process lifetime: scope instances may outlive any point at which
/// unloading would be safe, so the modules are never unloaded.
///
/// @param directory Directory to scan for module files.
/// @param registry  Registry that each discovered module is registered with.
/// @return True if the directory was scanned (even with zero modules
///         registered), false if it could not be opened.
bool LoadModulesFrom(const std::filesystem::path& directory, ModuleRegistry& registry);

}  // namespace sidescopes
