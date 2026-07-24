// The keyboard bindings and the single map from a key to the action it means.
// Nothing here touches application state: the shell applies what this returns.

#include "app/shortcut_resolver.h"

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "app/scope_registry.h"

namespace sidescopes {

namespace {

// The preset digits, named the way the key probe names every other key.
constexpr std::array<std::string_view, LayoutPresetSlots> DigitNames{"1", "2", "3", "4", "5", "6", "7", "8", "9"};

ShortcutAction chooseScopeAction(std::string id, bool stack)
{
    ShortcutAction action;
    action.kind = ShortcutAction::Kind::ChooseScope;
    action.scopeId = std::move(id);
    action.stack = stack;

    return action;
}

ShortcutAction zoomAction(int level)
{
    ShortcutAction action;
    action.kind = ShortcutAction::Kind::SetZoom;
    action.zoomLevel = level;

    return action;
}

// The window chords the platform decides rather than the preferences. Each
// stays silent where its platform seam says the chord is not the local
// convention, and none of them survives a second modifier.
ShortcutAction resolveWindowChord(const ShortcutContext& context, const ModifierState& modifiers,
                                  const ShortcutKeyPressed& pressed)
{
    if (context.hidesWindowOnCommandW && modifiers.command && !modifiers.control && !modifiers.option) {
        // Command+W dismisses through the system hide - the machinery behind
        // Command+H, so the Dock click or Command+Tab restores every window
        // natively, the border included.
        if (pressed("W")) {
            return ShortcutAction::plain(ShortcutAction::Kind::HideApplication);
        }
        // Command+comma opens settings everywhere on macOS.
        if (pressed("Comma")) {
            return ShortcutAction::plain(ShortcutAction::Kind::OpenSettings);
        }
    }
    if (modifiers.control && !modifiers.command && !modifiers.option) {
        if (context.minimizesWindowOnControlW && pressed("W")) {
            return ShortcutAction::plain(ShortcutAction::Kind::MinimizeWindow);
        }
        if (context.quitsOnControlQ && pressed("Q")) {
            return ShortcutAction::plain(ShortcutAction::Kind::QuitWindow);
        }
    }

    return {};
}

// Digit N loads preset slot N; Shift+N saves the live layout into it.
void appendPresetDigits(bool shift, const ShortcutKeyPressed& pressed, std::vector<ShortcutAction>& actions)
{
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        if (pressed(DigitNames[static_cast<std::size_t>(slot - 1)])) {
            actions.push_back(ShortcutAction::preset(slot, shift));
        }
    }
}

}  // namespace

ShortcutAction ShortcutAction::pick(RegionPickerMode mode)
{
    ShortcutAction action;
    action.kind = Kind::RequestPick;
    action.pickMode = mode;

    return action;
}

ShortcutAction ShortcutAction::preset(int slot, bool save)
{
    ShortcutAction action;
    action.kind = save ? Kind::SavePreset : Kind::LoadPreset;
    action.presetSlot = slot;

    return action;
}

ShortcutAction ShortcutAction::plain(Kind kind)
{
    ShortcutAction action;
    action.kind = kind;

    return action;
}

ShortcutResolver::ShortcutResolver(const ScopeRegistry& registry)
    : m_registry(registry)
{
}

void ShortcutResolver::restore(const ShortcutBindings& bindings, std::map<std::string, std::string> scopeOverrides)
{
    m_bindings = bindings;
    m_scopeOverrides = std::move(scopeOverrides);
}

const ShortcutBindings& ShortcutResolver::bindings() const
{
    return m_bindings;
}

const std::map<std::string, std::string>& ShortcutResolver::scopeOverrides() const
{
    return m_scopeOverrides;
}

std::string ShortcutResolver::bindingFor(std::string_view id) const
{
    if (const auto custom = m_scopeOverrides.find(std::string{id}); custom != m_scopeOverrides.end()) {
        return custom->second;
    }
    const HostScope* scope = m_registry.byId(id);

    return scope != nullptr && scope->letter != 0 ? std::string(1, scope->letter) : std::string{};
}

int ShortcutResolver::cycledZoom(int current)
{
    return current >= 4 ? 1 : current * 2;
}

std::vector<ShortcutAction> ShortcutResolver::resolvePressed(const ShortcutContext& context,
                                                             const ModifierState& modifiers,
                                                             const ShortcutKeyPressed& pressed) const
{
    if (context.wantsTextInput) {
        return {};
    }
    ShortcutAction chord = resolveWindowChord(context, modifiers, pressed);
    if (chord.kind != ShortcutAction::Kind::None) {
        return {chord};
    }
    // Command, Control, and Option chords belong to the system and the window,
    // so any of them silences the plain keys. Shift alone stays meaningful: it
    // stacks.
    if (modifiers.command || modifiers.control || modifiers.option) {
        return {};
    }

    return resolvePlainKeys(context, modifiers.shift, pressed);
}

ShortcutAction ShortcutResolver::resolveNamed(const std::string& key, bool shift, const ShortcutContext& context) const
{
    for (const HostScope& scope : m_registry.scopes()) {
        const std::string binding = bindingFor(scope.id);
        if (!binding.empty() && binding == key) {
            return chooseScopeAction(scope.id, shift);
        }
    }
    if (key == m_bindings.attachWindow) {
        return ShortcutAction::pick(RegionPickerMode::AttachWindow);
    }
    if (key == m_bindings.drawRegion) {
        return ShortcutAction::pick(RegionPickerMode::DrawGlobal);
    }
    if (key == m_bindings.attachFace && context.faceDetectionSupported) {
        return ShortcutAction::pick(RegionPickerMode::AttachFace);
    }
    if (key == m_bindings.pinColor && context.pinsAvailable) {
        // One pin tool; each click inside decides between pin-and-close and
        // Shift's pin-and-continue.
        return ShortcutAction::pick(RegionPickerMode::PinColor);
    }
    if (key == m_bindings.vectorscopeZoom) {
        return zoomAction(cycledZoom(context.vectorscopeZoom));
    }
    if (key == m_bindings.fullScreen) {
        return ShortcutAction::plain(context.settingsOpen ? ShortcutAction::Kind::CloseSettings
                                                          : ShortcutAction::Kind::ResetToFullScreen);
    }

    return {};
}

std::vector<ShortcutAction> ShortcutResolver::resolvePlainKeys(const ShortcutContext& context, bool shift,
                                                               const ShortcutKeyPressed& pressed) const
{
    // Every group has its say, and each collects every key of its own that is
    // down: two scope letters in one frame both show their scope, and a letter
    // beside a digit switches the stack and loads the preset.
    std::vector<ShortcutAction> actions;
    appendScopeKeys(shift, pressed, actions);
    // The bound action keys in the order the shell has always checked them; a
    // key whose action is unavailable resolves to nothing and lets the rest of
    // the scan run.
    for (const std::string& binding : {m_bindings.attachWindow, m_bindings.drawRegion, m_bindings.attachFace,
                                       m_bindings.pinColor, m_bindings.vectorscopeZoom, m_bindings.fullScreen}) {
        if (!pressed(binding)) {
            continue;
        }
        ShortcutAction action = resolveNamed(binding, shift, context);
        if (action.kind != ShortcutAction::Kind::None) {
            actions.push_back(std::move(action));
        }
    }
    appendPresetDigits(shift, pressed, actions);

    return actions;
}

void ShortcutResolver::appendScopeKeys(bool shift, const ShortcutKeyPressed& pressed,
                                       std::vector<ShortcutAction>& actions) const
{
    // Each scope's key is resolved by id; a letterless scope has an empty
    // binding, which never matches a press.
    for (const HostScope& scope : m_registry.scopes()) {
        const std::string binding = bindingFor(scope.id);
        if (!binding.empty() && pressed(binding)) {
            actions.push_back(chooseScopeAction(scope.id, shift));
        }
    }
}

}  // namespace sidescopes
