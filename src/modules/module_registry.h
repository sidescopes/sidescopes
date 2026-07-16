#pragma once

#include <string>
#include <vector>

#include "sidescopes/module.h"

namespace sidescopes {

/// @brief RAII ownership of one module scope instance.
///
/// Move-only; the wrapper calls destroy() and never lets an exception
/// cross the boundary. Every accessor dereferences the instance, so the
/// caller must check valid() before using one that may be empty.
class ScopeInstance
{
public:
    ScopeInstance() = default;

    explicit ScopeInstance(SsScopeInstance* instance)
        : m_instance(instance)
    {
    }

    ScopeInstance(ScopeInstance&& other) noexcept
        : m_instance(other.m_instance)
    {
        other.m_instance = nullptr;
    }

    ScopeInstance& operator=(ScopeInstance&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_instance = other.m_instance;
            other.m_instance = nullptr;
        }
        return *this;
    }

    ScopeInstance(const ScopeInstance&) = delete;
    ScopeInstance& operator=(const ScopeInstance&) = delete;

    ~ScopeInstance()
    {
        reset();
    }

    [[nodiscard]] bool valid() const
    {
        return m_instance != nullptr;
    }

    [[nodiscard]] bool configure(const std::vector<SsParamValue>& values) const
    {
        return m_instance->configure(m_instance, values.data(), static_cast<uint32_t>(values.size()));
    }

    [[nodiscard]] bool accumulate(const SsFrameView& frame, SsRect region) const
    {
        return m_instance->accumulate(m_instance, &frame, region);
    }

    [[nodiscard]] SsImageView image() const
    {
        return m_instance->image(m_instance);
    }

    [[nodiscard]] std::vector<SsGraticulePrimitive> graticule() const
    {
        constexpr uint32_t GraticuleCapacity = 32;
        std::vector<SsGraticulePrimitive> primitives(GraticuleCapacity);
        const uint32_t needed = m_instance->graticule(m_instance, primitives.data(), GraticuleCapacity);
        primitives.resize(needed);
        if (needed > GraticuleCapacity) {
            m_instance->graticule(m_instance, primitives.data(), needed);
        }
        return primitives;
    }

    [[nodiscard]] std::vector<SsMarker> markers(SsColor color) const
    {
        constexpr uint32_t MarkerCapacity = 8;
        std::vector<SsMarker> markers(MarkerCapacity);
        const uint32_t needed = m_instance->markers(m_instance, color, markers.data(), MarkerCapacity);
        markers.resize(needed);
        if (needed > MarkerCapacity) {
            m_instance->markers(m_instance, color, markers.data(), needed);
        }
        return markers;
    }

    [[nodiscard]] const void* getExtension(const char* id) const
    {
        return m_instance->get_extension(m_instance, id);
    }

    /// The raw handle, for extension calls that take the instance back.
    [[nodiscard]] const SsScopeInstance* raw() const
    {
        return m_instance;
    }

    [[nodiscard]] SsScopeInstance* raw()
    {
        return m_instance;
    }

private:
    void reset()
    {
        if (m_instance) {
            m_instance->destroy(m_instance);
        }

        m_instance = nullptr;
    }

    SsScopeInstance* m_instance = nullptr;
};

struct RegisteredScope
{
    const SsScopeDescriptor* descriptor = nullptr;
    const SsModuleEntry* module = nullptr;
};

/// @brief The host's collection of scopes.
///
/// Modules register (statically for the built-ins, through the loader
/// later); the registry gates on the ABI major, initializes each module
/// once, and creates instances by id.
class ModuleRegistry
{
public:
    ModuleRegistry();
    ~ModuleRegistry();
    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;

    /// Registers a module; false when the ABI major differs or the module's init failed.
    [[nodiscard]] bool registerModule(const SsModuleEntry& entry);

    [[nodiscard]] const std::vector<RegisteredScope>& scopes() const
    {
        return m_scopes;
    }

    [[nodiscard]] const RegisteredScope* findScope(const std::string& id) const;

    /// Creates an instance by id; an invalid instance means the id is unknown or creation failed.
    [[nodiscard]] ScopeInstance createInstance(const std::string& id) const;

private:
    std::vector<const SsModuleEntry*> m_modules;
    std::vector<RegisteredScope> m_scopes;
    SsHost m_host{};
};

/// The application's registry with every built-in module registered.
ModuleRegistry& builtinModules();

/// Built-in module entries, defined in their modules' translation units.
/// The extern declarations give the const definitions external linkage.
/// In the dynamic configuration the module objects are not linked into
/// core, so these definitions do not exist: the loader supplies the
/// entries instead (see BuiltinModules).
#ifndef SIDESCOPES_MODULES_DYNAMIC
extern const SsModuleEntry VectorscopeModuleEntry;
extern const SsModuleEntry WaveformModuleEntry;
extern const SsModuleEntry HistogramModuleEntry;
#endif

/// Instance extension: the host drives adaptive display resolution through
/// this, keeping image sizing out of the parameter list users see. Extension
/// vtables are module-owned, valid for the instance's lifetime.
inline constexpr char AdaptiveImageExtension[] = "sidescopes.adaptive_image/1";

struct SsAdaptiveImageExtension
{
    void (*setImageSize)(SsScopeInstance* instance, int32_t width, int32_t height);
};

/// Instance extension: normalized curve heights for display-resolution
/// stroking (the histogram's outline).
inline constexpr char OutlineExtension[] = "sidescopes.outline/1";

struct SsOutlineExtension
{
    uint32_t (*heights)(const SsScopeInstance* instance, float* out, uint32_t capacity);
};

}  // namespace sidescopes
