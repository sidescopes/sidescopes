#include <catch2/catch_test_macros.hpp>
#include <map>
#include <string>
#include <vector>

#include "app/param_menu.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "modules/module_registry.h"
#include "platform/native_menu.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

using Kind = NativeMenuItem::Kind;

const SsScopeDescriptor* descriptorOf(const ScopeRegistry& registry, std::string_view id)
{
    const HostScope* scope = registry.byId(id);
    return scope != nullptr ? scope->descriptor : nullptr;
}

}  // namespace

TEST_CASE("firstParamOfKind finds a scope's intensity and integer parameters")
{
    const ScopeRegistry registry{builtinModules()};

    const SsParamInfo* gain = firstParamOfKind(descriptorOf(registry, VectorscopeScopeId), SS_PARAM_INTENSITY);
    REQUIRE(gain != nullptr);
    CHECK(std::string(gain->key) == "gain");
    CHECK(gain->default_value == 3.0);
    CHECK(gain->intensity_shift == 20.0);

    const SsParamInfo* stride = firstParamOfKind(descriptorOf(registry, VectorscopeScopeId), SS_PARAM_INT);
    REQUIRE(stride != nullptr);
    CHECK(std::string(stride->key) == "stride");
    CHECK(stride->min_value == 1.0);
    CHECK(stride->max_value == 8.0);

    // The histogram exposes no intensity, and the color picker no descriptor.
    CHECK(firstParamOfKind(descriptorOf(registry, HistogramScopeId), SS_PARAM_INTENSITY) == nullptr);
    CHECK(firstParamOfKind(descriptorOf(registry, ColorPickerScopeId), SS_PARAM_INTENSITY) == nullptr);
}

TEST_CASE("Built-in descriptors declare pin targeting and pane aspects")
{
    // The pin tool and the automatic layout read these declarations instead
    // of hard-coding scope ids, so modules can opt in the same way.
    const ScopeRegistry registry{builtinModules()};

    const SsScopeDescriptor* vectorscope = descriptorOf(registry, VectorscopeScopeId);
    REQUIRE(vectorscope != nullptr);
    CHECK((vectorscope->flags & SS_SCOPE_PIN_TARGET) != 0u);
    CHECK(vectorscope->preferred_aspect == 1.0f);

    const SsScopeDescriptor* waveform = descriptorOf(registry, WaveformScopeId);
    REQUIRE(waveform != nullptr);
    CHECK((waveform->flags & SS_SCOPE_PIN_TARGET) == 0u);
    CHECK(waveform->preferred_aspect == 3.0f);
}

TEST_CASE("findParam resolves a descriptor parameter by key")
{
    const ScopeRegistry registry{builtinModules()};

    const SsParamInfo* matrix = findParam(descriptorOf(registry, VectorscopeScopeId), "matrix");
    REQUIRE(matrix != nullptr);
    CHECK(matrix->kind == SS_PARAM_CHOICE);

    // An unknown key and the descriptorless color picker both come back null.
    CHECK(findParam(descriptorOf(registry, VectorscopeScopeId), "no-such-key") == nullptr);
    CHECK(findParam(descriptorOf(registry, ColorPickerScopeId), "matrix") == nullptr);
}

TEST_CASE("Choice submenus strip the scope-name prefix and check the current value")
{
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    // Defaults (empty params): matrix defaults to BT.709 (choice 1), response
    // to Boosted (choice 0).
    appendScopeChoiceMenus(*descriptorOf(registry, VectorscopeScopeId), {}, false, items, actions);

    REQUIRE(items.size() == 8);
    CHECK(items[0].kind == Kind::SubmenuBegin);
    CHECK(items[0].label == "Matrix");  // "Vectorscope Matrix" with the prefix stripped
    CHECK(items[1].label == "BT.601");
    CHECK_FALSE(items[1].checked);
    CHECK(items[2].label == "BT.709");
    CHECK(items[2].checked);
    CHECK(items[3].kind == Kind::SubmenuEnd);
    CHECK(items[4].kind == Kind::SubmenuBegin);
    CHECK(items[4].label == "Trace Response");  // no scope prefix, unchanged
    CHECK(items[5].label == "Boosted");
    CHECK(items[5].checked);
    CHECK(items[6].label == "Linear");
    CHECK_FALSE(items[6].checked);
    CHECK(items[7].kind == Kind::SubmenuEnd);

    // The side table pairs each choice action with its (scope, key, value),
    // ided from ParamMenuActionBase upward.
    REQUIRE(actions.size() == 4);
    CHECK(items[1].actionId == ParamMenuActionBase);
    CHECK(items[2].actionId == ParamMenuActionBase + 1);
    CHECK(items[5].actionId == ParamMenuActionBase + 2);
    CHECK(items[6].actionId == ParamMenuActionBase + 3);
    CHECK(actions[0].scopeId == "org.sidescopes.vectorscope");
    CHECK(actions[0].paramKey == "matrix");
    CHECK(actions[0].value == 0.0);
    CHECK(actions[3].paramKey == "response");
    CHECK(actions[3].value == 1.0);
}

TEST_CASE("Stored parameter values drive the checkmarks")
{
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    const std::map<std::string, double> params{{"matrix", 0.0}, {"response", 1.0}};
    appendScopeChoiceMenus(*descriptorOf(registry, VectorscopeScopeId), params, false, items, actions);

    REQUIRE(items.size() == 8);
    CHECK(items[1].label == "BT.601");
    CHECK(items[1].checked);  // matrix == 0
    CHECK_FALSE(items[2].checked);
    CHECK_FALSE(items[5].checked);
    CHECK(items[6].label == "Linear");
    CHECK(items[6].checked);  // response == 1
}

TEST_CASE("A lone choice is a Style submenu unprefixed but flattens when nested")
{
    const ScopeRegistry registry{builtinModules()};

    // Unprefixed (a pane click): the single choice keeps its own submenu,
    // titled by the stripped menu_label.
    {
        std::vector<NativeMenuItem> items;
        std::vector<ParamMenuAction> actions;
        appendScopeChoiceMenus(*descriptorOf(registry, WaveformScopeId), {}, false, items, actions);
        REQUIRE(items.size() == 5);
        CHECK(items[0].kind == Kind::SubmenuBegin);
        CHECK(items[0].label == "Style");  // "Waveform Style" stripped
        CHECK(items[1].label == "RGB");
        CHECK(items[1].checked);  // mode defaults to RGB (choice 0)
        CHECK(items[2].label == "Luma");
        CHECK(items[3].label == "Luma (Colored)");
        CHECK(items[4].kind == Kind::SubmenuEnd);
    }

    // Nested under the scope-name submenu (a global click): the lone choice's
    // own submenu is dropped, so its actions sit directly under "Waveform".
    {
        std::vector<NativeMenuItem> items;
        std::vector<ParamMenuAction> actions;
        appendScopeChoiceMenus(*descriptorOf(registry, WaveformScopeId), {}, true, items, actions);
        REQUIRE(items.size() == 3);
        CHECK(items[0].kind == Kind::Action);
        CHECK(items[0].label == "RGB");
        CHECK(items[1].label == "Luma");
        CHECK(items[2].label == "Luma (Colored)");
    }
}

TEST_CASE("The histogram style choice strips its prefix like the waveform")
{
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    appendScopeChoiceMenus(*descriptorOf(registry, HistogramScopeId), {}, false, items, actions);
    REQUIRE(items.size() == 4);
    CHECK(items[0].label == "Style");  // "Histogram Style" stripped
    CHECK(items[1].label == "Per Channel");
    CHECK(items[1].checked);  // style defaults to Per Channel (choice 0)
    CHECK(items[2].label == "Combined");
    CHECK(items[3].kind == Kind::SubmenuEnd);
}

TEST_CASE("The parade contributes no menu options of its own")
{
    // R4: the parade shares the waveform's controls and exposes no choice
    // parameters, so a generic walk emits nothing - no duplicate control.
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    appendScopeChoiceMenus(*descriptorOf(registry, ParadeScopeId), {}, false, items, actions);
    CHECK(items.empty());
    CHECK(actions.empty());
}

TEST_CASE("Side-table ids continue across scopes in one menu build")
{
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    // A stack of waveform then histogram, each nested under its own name.
    appendScopeChoiceMenus(*descriptorOf(registry, WaveformScopeId), {}, true, items, actions);
    appendScopeChoiceMenus(*descriptorOf(registry, HistogramScopeId), {}, true, items, actions);

    REQUIRE(actions.size() == 5);  // three waveform modes, two histogram styles
    CHECK(actions[0].scopeId == "org.sidescopes.waveform");
    CHECK(actions[0].paramKey == "mode");
    CHECK(actions[3].scopeId == "org.sidescopes.histogram");
    CHECK(actions[3].paramKey == "style");
    // The histogram's first action id follows the waveform's three.
    CHECK(items.back().actionId == ParamMenuActionBase + 4);
}

}  // namespace sidescopes
