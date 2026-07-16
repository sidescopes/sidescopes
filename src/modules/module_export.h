#pragma once

// The one exported symbol a dynamically loaded module presents (see
// module.h). Static builds never define SIDESCOPES_MODULE_DYNAMIC, so the
// export blocks guarded by it compile to nothing and each module's entry
// keeps ordinary external linkage for the static registry to reference.
//
// Note the near-twin names: SIDESCOPES_MODULE_DYNAMIC (singular) is defined
// per module target and gates this export; SIDESCOPES_MODULES_DYNAMIC
// (plural) is defined for core and switches the registry to the loader.
//
// SS_MODULE_EXPORT marks that symbol visible past the module boundary:
// dllexport on Windows, default ELF/Mach-O visibility elsewhere (the
// module targets build with hidden visibility, so this attribute is what
// keeps ss_module_entry findable by dlsym/GetProcAddress).
#if defined(_WIN32)
#define SS_MODULE_EXPORT __declspec(dllexport)
#else
#define SS_MODULE_EXPORT __attribute__((visibility("default")))
#endif
