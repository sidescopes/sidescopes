#include <catch2/catch_test_macros.hpp>
#include <map>
#include <string>
#include <vector>

#include "app/param_menu.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/scopes/vectorscope.h"
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

// A descriptor of its own for the unprefixed menu_label rule, which no
// built-in exercises now that the vectorscope's trace response is retired: the
// luma waveform's "Luma Waveform Style" is the prefixed case, and this is the
// bare one. Its default is choice 1, so a checkmark test cannot pass by
// reading zero.
const char* const TintChoices[] = {"Warm", "Cool", nullptr};

const SsParamInfo TintParams[] = {
    {"tint", "Tint", SS_PARAM_CHOICE, 0.0, 1.0, 1.0, 0.0, "Tint", TintChoices},
};

const SsScopeDescriptor TintScope{
    "org.sidescopes.test.tint", "Test Scope", 'T', 256, 256, 0u, TintParams, 1u, 1.0f,
};

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

TEST_CASE("The vectorscope's declared gamma is the engine's own range")
{
    // The slider is drawn from the descriptor and the engine clamps to its own
    // constants, so a descriptor that disagreed would offer a setting the
    // engine silently refuses. Only the vectorscope declares a continuous
    // parameter today.
    const ScopeRegistry registry{builtinModules()};

    const SsParamInfo* gamma = firstParamOfKind(descriptorOf(registry, VectorscopeScopeId), SS_PARAM_FLOAT);
    REQUIRE(gamma != nullptr);
    CHECK(std::string(gamma->key) == "gamma");
    CHECK(static_cast<float>(gamma->min_value) == MinTraceGamma);
    CHECK(static_cast<float>(gamma->max_value) == MaxTraceGamma);
    // The default is the waveform's fixed lift, which is what keeps an
    // untouched vectorscope rendering exactly as it always has.
    CHECK(static_cast<float>(gamma->default_value) == MidDensityGamma);

    for (const std::string_view id : {WaveformScopeId, LumaWaveformScopeId, HistogramScopeId}) {
        CHECK(firstParamOfKind(descriptorOf(registry, id), SS_PARAM_FLOAT) == nullptr);
    }
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

    const SsParamInfo* gamma = findParam(descriptorOf(registry, VectorscopeScopeId), "gamma");
    REQUIRE(gamma != nullptr);
    CHECK(gamma->kind == SS_PARAM_FLOAT);

    // An unknown key and the descriptorless color picker both come back null.
    CHECK(findParam(descriptorOf(registry, VectorscopeScopeId), "no-such-key") == nullptr);
    CHECK(findParam(descriptorOf(registry, ColorPickerScopeId), "gamma") == nullptr);
}

TEST_CASE("A choice submenu titled without the scope name keeps its label")
{
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    // Defaults (empty params): the choice starts on its declared default, 1.
    appendScopeChoiceMenus(TintScope, {}, false, items, actions);

    REQUIRE(items.size() == 4);
    CHECK(items[0].kind == Kind::SubmenuBegin);
    CHECK(items[0].label == "Tint");  // no scope prefix, unchanged
    CHECK(items[1].label == "Warm");
    CHECK_FALSE(items[1].checked);
    CHECK(items[2].label == "Cool");
    CHECK(items[2].checked);
    CHECK(items[3].kind == Kind::SubmenuEnd);

    // The side table pairs each choice action with its (scope, key, value),
    // ided from ParamMenuActionBase upward.
    REQUIRE(actions.size() == 2);
    CHECK(items[1].actionId == ParamMenuActionBase);
    CHECK(items[2].actionId == ParamMenuActionBase + 1);
    CHECK(actions[0].scopeId == "org.sidescopes.test.tint");
    CHECK(actions[0].paramKey == "tint");
    CHECK(actions[0].value == 0.0);
    CHECK(actions[1].paramKey == "tint");
    CHECK(actions[1].value == 1.0);
}

TEST_CASE("Stored parameter values drive the checkmarks")
{
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    // The stored value differs from the declared default, so the checkmark
    // moving proves the stored one is what the walk reads.
    const std::map<std::string, double> params{{"tint", 0.0}};
    appendScopeChoiceMenus(TintScope, params, false, items, actions);

    REQUIRE(items.size() == 4);
    CHECK(items[1].label == "Warm");
    CHECK(items[1].checked);
    CHECK_FALSE(items[2].checked);
}

TEST_CASE("The vectorscope contributes no menu option of its own")
{
    // Its trace curve is a setting on a scale, not a mode, so the scope
    // declares no choice at all and a generic walk over it emits nothing. What
    // is left under the vectorscope's own section is host state: zoom and pins.
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    appendScopeChoiceMenus(*descriptorOf(registry, VectorscopeScopeId), {}, false, items, actions);

    CHECK(items.empty());
    CHECK(actions.empty());
}

TEST_CASE("A lone choice is a Style submenu unprefixed but flattens when nested")
{
    const ScopeRegistry registry{builtinModules()};

    // Unprefixed (a pane click): the single choice keeps its own submenu,
    // titled by the stripped menu_label.
    {
        std::vector<NativeMenuItem> items;
        std::vector<ParamMenuAction> actions;
        appendScopeChoiceMenus(*descriptorOf(registry, LumaWaveformScopeId), {}, false, items, actions);
        REQUIRE(items.size() == 4);
        CHECK(items[0].kind == Kind::SubmenuBegin);
        CHECK(items[0].label == "Style");  // "Luma Waveform Style" stripped
        CHECK(items[1].label == "Plain");
        CHECK(items[1].checked);  // style defaults to Plain (choice 0)
        CHECK(items[2].label == "Colored");
        CHECK(items[3].kind == Kind::SubmenuEnd);
    }

    // Nested under the scope-name submenu (a global click): the lone choice's
    // own submenu is dropped, so its actions sit directly under "Waveform".
    {
        std::vector<NativeMenuItem> items;
        std::vector<ParamMenuAction> actions;
        appendScopeChoiceMenus(*descriptorOf(registry, LumaWaveformScopeId), {}, true, items, actions);
        REQUIRE(items.size() == 2);
        CHECK(items[0].kind == Kind::Action);
        CHECK(items[0].label == "Plain");
        CHECK(items[1].label == "Colored");
    }
}

TEST_CASE("The histogram offers its two plots as one style choice")
{
    // One measurement drawn two ways, so the plot is a style on the scope
    // rather than a second scope. Per Channel leads and is the default.
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;
    appendScopeChoiceMenus(*descriptorOf(registry, HistogramScopeId), {}, false, items, actions);

    REQUIRE(items.size() == 4);
    CHECK(items[0].kind == Kind::SubmenuBegin);
    CHECK(items[0].label == "Style");  // "Histogram Style" stripped
    CHECK(items[1].label == "Per Channel");
    CHECK(items[1].checked);
    CHECK(items[2].label == "Combined");
    CHECK_FALSE(items[2].checked);
    CHECK(items[3].kind == Kind::SubmenuEnd);

    REQUIRE(actions.size() == 2);
    CHECK(actions[0].scopeId == HistogramScopeId);
    CHECK(actions[0].paramKey == "style");
    CHECK(actions[1].value == 1.0);
}

TEST_CASE("Neither the RGB waveform nor the parade contributes a menu option")
{
    // Each is defined by what it plots and neither declares a choice, so a
    // generic walk over either emits nothing. A style menu here would offer a
    // scope the chance to become one of its siblings.
    const ScopeRegistry registry{builtinModules()};
    for (const std::string_view id : {WaveformScopeId, ParadeScopeId}) {
        std::vector<NativeMenuItem> items;
        std::vector<ParamMenuAction> actions;
        appendScopeChoiceMenus(*descriptorOf(registry, id), {}, false, items, actions);
        CHECK(items.empty());
        CHECK(actions.empty());
    }
}

TEST_CASE("Side-table ids continue across scopes in one menu build")
{
    const ScopeRegistry registry{builtinModules()};
    std::vector<NativeMenuItem> items;
    std::vector<ParamMenuAction> actions;

    // A stack of luma waveform then the tint scope, each nested under its own
    // name.
    appendScopeChoiceMenus(*descriptorOf(registry, LumaWaveformScopeId), {}, true, items, actions);
    appendScopeChoiceMenus(TintScope, {}, true, items, actions);

    REQUIRE(actions.size() == 4);  // two luma styles, two tints
    CHECK(actions[0].scopeId == "org.sidescopes.waveform.luma");
    CHECK(actions[0].paramKey == "style");
    CHECK(actions[2].scopeId == "org.sidescopes.test.tint");
    CHECK(actions[2].paramKey == "tint");
    // The second scope's first action id follows the luma waveform's two.
    CHECK(items.back().actionId == ParamMenuActionBase + 3);
}

}  // namespace sidescopes
