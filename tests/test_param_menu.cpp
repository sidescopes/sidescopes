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

    const SsParamInfo* response = findParam(descriptorOf(registry, VectorscopeScopeId), "response");
    REQUIRE(response != nullptr);
    CHECK(response->kind == SS_PARAM_CHOICE);

    // An unknown key and the descriptorless color picker both come back null.
    CHECK(findParam(descriptorOf(registry, VectorscopeScopeId), "no-such-key") == nullptr);
    CHECK(findParam(descriptorOf(registry, ColorPickerScopeId), "response") == nullptr);
}

TEST_CASE("Choice submenus strip the scope-name prefix and check the current value")
{
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    // Defaults (empty params): response defaults to Boosted (choice 0).
    appendScopeChoiceMenus(*descriptorOf(registry, VectorscopeScopeId), {}, false, items, actions);

    REQUIRE(items.size() == 4);
    CHECK(items[0].kind == Kind::SubmenuBegin);
    CHECK(items[0].label == "Trace Response");  // no scope prefix, unchanged
    CHECK(items[1].label == "Boosted");
    CHECK(items[1].checked);
    CHECK(items[2].label == "Linear");
    CHECK_FALSE(items[2].checked);
    CHECK(items[3].kind == Kind::SubmenuEnd);

    // The side table pairs each choice action with its (scope, key, value),
    // ided from ParamMenuActionBase upward.
    REQUIRE(actions.size() == 2);
    CHECK(items[1].actionId == ParamMenuActionBase);
    CHECK(items[2].actionId == ParamMenuActionBase + 1);
    CHECK(actions[0].scopeId == "org.sidescopes.vectorscope");
    CHECK(actions[0].paramKey == "response");
    CHECK(actions[0].value == 0.0);
    CHECK(actions[1].paramKey == "response");
    CHECK(actions[1].value == 1.0);
}

TEST_CASE("Stored parameter values drive the checkmarks")
{
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    const std::map<std::string, double> params{{"response", 1.0}};
    appendScopeChoiceMenus(*descriptorOf(registry, VectorscopeScopeId), params, false, items, actions);

    REQUIRE(items.size() == 4);
    CHECK_FALSE(items[1].checked);
    CHECK(items[2].label == "Linear");
    CHECK(items[2].checked);  // response == 1
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

TEST_CASE("Neither histogram contributes a menu option of its own")
{
    // The two plots are scopes now rather than one scope's style, so neither
    // declares a choice parameter and a generic walk over either emits
    // nothing. A style menu here would offer a scope the chance to become the
    // other one.
    const ScopeRegistry registry{builtinModules()};
    for (const std::string_view id : {HistogramScopeId, CombinedHistogramScopeId}) {
        std::vector<NativeMenuItem> items;
        std::vector<ParamMenuAction> actions;
        appendScopeChoiceMenus(*descriptorOf(registry, id), {}, false, items, actions);
        CHECK(items.empty());
        CHECK(actions.empty());
    }
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

    // A stack of waveform then vectorscope, each nested under its own name.
    appendScopeChoiceMenus(*descriptorOf(registry, WaveformScopeId), {}, true, items, actions);
    appendScopeChoiceMenus(*descriptorOf(registry, VectorscopeScopeId), {}, true, items, actions);

    REQUIRE(actions.size() == 5);  // three waveform modes, two trace responses
    CHECK(actions[0].scopeId == "org.sidescopes.waveform");
    CHECK(actions[0].paramKey == "mode");
    CHECK(actions[3].scopeId == "org.sidescopes.vectorscope");
    CHECK(actions[3].paramKey == "response");
    // The vectorscope's first action id follows the waveform's three.
    CHECK(items.back().actionId == ParamMenuActionBase + 4);
}

}  // namespace sidescopes
