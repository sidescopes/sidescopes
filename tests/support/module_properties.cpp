#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "boundary_input.h"
#include "boundary_properties.h"
#include "modules/module_registry.h"

namespace sidescopes::test {
namespace {

thread_local const SsScopeDescriptor* g_currentDescriptor = nullptr;

struct DescriptorScope
{
    explicit DescriptorScope(const SsScopeDescriptor& descriptor)
    {
        g_currentDescriptor = &descriptor;
    }

    ~DescriptorScope()
    {
        g_currentDescriptor = nullptr;
    }
};

const SsModuleEntry TestModule{SS_ABI_MAJOR,
                               SS_ABI_MINOR,
                               [] { return true; },
                               [] {},
                               [] { return 1u; },
                               [](uint32_t) { return g_currentDescriptor; },
                               [](const char*, const SsHost*) -> SsScopeInstance* { return nullptr; }};

void checkDescriptor(BoundaryInput& input)
{
    const char* choices[] = {"First", "Second", nullptr};
    std::array<SsParamInfo, 2> params{{{"gain", "Gain", SS_PARAM_FLOAT, 0, 1, 0.5, 0, nullptr, nullptr},
                                       {"stride", "Stride", SS_PARAM_INT, 1, 8, 1, 0, nullptr, nullptr}}};
    std::string id = "org.boundary.scope" + std::to_string(input.byte());
    SsScopeDescriptor descriptor{id.c_str(),
                                 "Boundary scope",
                                 'Q',
                                 input.integer(),
                                 input.integer(),
                                 0,
                                 params.data(),
                                 static_cast<uint32_t>(params.size()),
                                 0};
    const auto mutation = input.byte() % 12;
    bool expected = mutation == 0;
    switch (mutation) {
    case 0:
        break;
    case 1:
        descriptor.id = nullptr;
        break;
    case 2:
        descriptor.id = "";
        break;
    case 3:
        descriptor.name = nullptr;
        break;
    case 4:
        descriptor.params = nullptr;
        break;
    case 5:
        params[0].key = nullptr;
        break;
    case 6:
        params[0].key = "";
        break;
    case 7:
        params[0].label = nullptr;
        break;
    case 8:
        params[1].key = params[0].key;
        break;
    case 9:
        params[0].min_value = 2;
        break;
    case 10:
        params[0].default_value = std::numeric_limits<double>::infinity();
        break;
    default:
        params[0].kind = SS_PARAM_CHOICE;
        params[0].menu_label = "Style";
        params[0].choices = choices;
        choices[0] = nullptr;
        break;
    }
    // All arrays and strings remain live for the complete registry lifetime.
    // Nulls above are documented rejection cases, never fabricated addresses.
    DescriptorScope active{descriptor};
    ModuleRegistry registry;
    requireBoundary(registry.registerModule(TestModule), "modules: valid entry rejected");
    requireBoundary(registry.scopes().size() == static_cast<std::size_t>(expected),
                    "modules: malformed descriptor accepted or valid descriptor rejected");
}

std::vector<uint8_t> render(const ScopeInstance& instance, SsFrameView frame)
{
    requireBoundary(instance.accumulate(frame, {0, 0, frame.width, frame.height}), "modules: valid frame rejected");
    const auto image = instance.image();
    requireBoundary(image.width > 0 && image.height > 0 && image.width <= 1024 && image.height <= 1024 && image.rgba,
                    "modules: unusable image after bounded sizing");
    const auto size = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4;
    return {image.rgba, image.rgba + size};
}

void checkParameters(BoundaryInput& input)
{
    auto& registry = builtinModules();
    requireBoundary(!registry.scopes().empty(), "modules: no built-in descriptors available");
    const auto* descriptor = registry.scopes()[input.byte() % registry.scopes().size()].descriptor;
    auto instance = registry.createInstance(descriptor->id);
    requireBoundary(instance.valid(), "modules: cannot create built-in instance");
    const auto* sizing = static_cast<const SsAdaptiveImageExtension*>(instance.getExtension(AdaptiveImageExtension));
    requireBoundary(sizing != nullptr, "modules: built-in sizing extension missing");
    sizing->setImageSize(instance.raw(), 32, 32);
    std::vector<SsParamValue> values;
    for (uint32_t index = 0; index < descriptor->param_count; ++index) {
        const double candidate = input.number();
        values.push_back({descriptor->params[index].key, std::isfinite(candidate) ? candidate : 0.0});
    }
    requireBoundary(instance.configure(values), "modules: finite parameter batch rejected");
    std::array<uint8_t, std::size_t{16} * 16 * 4> pixels{};
    for (auto& pixel : pixels) {
        pixel = input.byte();
    }
    SsFrameView frame{pixels.data(), 64, 16, 16, SS_COLOR_SPACE_SRGB, 1, input.byte() % 2u};
    const auto before = render(instance, frame);
    requireBoundary(instance.configure(values), "modules: repeat parameter batch rejected");
    ++frame.sequence;
    requireBoundary(before == render(instance, frame), "modules: parameter application is not idempotent");
    for (auto& value : values) {
        value.value = value.value == 0 ? 1 : 0;
    }
    const std::size_t invalid = input.byte() % (values.size() + 1);
    values.insert(values.begin() + static_cast<std::ptrdiff_t>(invalid),
                  {input.byte() % 2 == 0 ? nullptr : "unknown", std::numeric_limits<double>::quiet_NaN()});
    requireBoundary(!instance.configure(values), "modules: malformed parameter batch accepted");
    ++frame.sequence;
    requireBoundary(before == render(instance, frame), "modules: rejected batch partially changed settings");
}

}  // namespace

void checkModuleProperties(std::span<const uint8_t> bytes)
{
    BoundaryInput input{bytes};
    checkDescriptor(input);
    checkParameters(input);
}

}  // namespace sidescopes::test
