#include "core/style_promotion.h"

#include <cstddef>
#include <initializer_list>
#include <map>
#include <string>

namespace sidescopes {
namespace {

// The ids this rewrites between. Core cannot see the application's scope
// registry, so the ones it needs are named here, the way the loader names the
// scopes whose retired keys it migrates.
constexpr char HistogramId[] = "org.sidescopes.histogram";
constexpr char CombinedHistogramId[] = "org.sidescopes.histogram.combined";

// The retired choice key, and the value from which it named the combined
// plot: halfway, which is where the module itself rounded a choice.
constexpr char HistogramStyleKey[] = "style";
constexpr double CombinedStyleFrom = 0.5;

using StyleMap = std::map<std::string, std::map<std::string, double>>;

/// The retired choices one arrangement was saved with. A preset carries its
/// own; the live arrangement's are the scope parameters themselves.
struct RetiredChoices
{
    double histogramStyle = 0.0;
};

/// Which scope each retired letter names, for one arrangement.
struct Promotion
{
    bool histogramIsCombined = false;
};

double choiceOr(const StyleMap& styles, const char* id, const char* key, double fallback)
{
    const auto scope = styles.find(id);
    if (scope == styles.end()) {
        return fallback;
    }
    const auto value = scope->second.find(key);

    return value != scope->second.end() ? value->second : fallback;
}

RetiredChoices choicesFrom(const StyleMap& styles, const RetiredChoices& fallback)
{
    return RetiredChoices{choiceOr(styles, HistogramId, HistogramStyleKey, fallback.histogramStyle)};
}

Promotion promotionOf(const RetiredChoices& choices)
{
    return Promotion{choices.histogramStyle >= CombinedStyleFrom};
}

/// @p tokens with each retired letter replaced by the scope it now names.
///
/// Only bare letters are rewritten: a token is bracketed only for a scope with
/// no letter of its own, and every scope here has one.
std::string rewriteTokens(const std::string& tokens, const Promotion& promotion)
{
    std::string rewritten;
    for (std::size_t at = 0; at < tokens.size();) {
        if (tokens[at] == '[') {
            const auto close = tokens.find(']', at);
            if (close == std::string::npos) {
                break;
            }
            rewritten += tokens.substr(at, close - at + 1);
            at = close + 1;
            continue;
        }
        const char letter = tokens[at];
        ++at;
        rewritten += (letter == 'H' && promotion.histogramIsCombined) ? 'G' : letter;
    }

    return rewritten;
}

/// Moves whatever @p byId holds under the retired scope to the one it became,
/// so a pane weight or a shortcut override follows the scope it was set on. An
/// entry already under the new id is left alone: the file names it explicitly
/// and knows better than this does.
template <typename Value>
void rewriteKeys(std::map<std::string, Value>& byId, const Promotion& promotion)
{
    const auto move = [&byId](const char* from, const char* to) {
        const auto at = byId.find(from);
        if (at == byId.end() || byId.count(to) != 0) {
            return;
        }
        byId.emplace(to, at->second);
        byId.erase(at);
    };
    if (promotion.histogramIsCombined) {
        move(HistogramId, CombinedHistogramId);
    }
}

/// Drops @p key from @p id's parameters, and the scope's entry with it when
/// that was all it held.
void eraseChoice(StyleMap& styles, const char* id, const char* key)
{
    const auto scope = styles.find(id);
    if (scope == styles.end()) {
        return;
    }
    scope->second.erase(key);
    if (scope->second.empty()) {
        styles.erase(scope);
    }
}

/// Copies the parameters a promoted scope inherits from the one it was a style
/// of, so a scope the user tuned comes back looking the way it did. Keys the
/// file already names for the new scope stand: it is a newer statement than
/// anything derived here.
void inherit(StyleMap& params, const char* from, const char* to, std::initializer_list<const char*> keys)
{
    const auto source = params.find(from);
    if (source == params.end()) {
        return;
    }
    for (const char* key : keys) {
        const auto value = source->second.find(key);
        if (value != source->second.end() && params[to].count(key) == 0) {
            params[to][key] = value->second;
        }
    }
}

void promotePreset(LayoutPreset& preset, const RetiredChoices& live)
{
    if (preset.stack.empty()) {
        return;
    }
    // A preset states the styles it was saved with, so it is read by its own -
    // falling back to the live ones for a slot saved before styles were
    // captured at all.
    const Promotion promotion = promotionOf(choicesFrom(preset.styles, live));
    preset.stack = rewriteTokens(preset.stack, promotion);
    rewriteKeys(preset.weights, promotion);
    eraseChoice(preset.styles, HistogramId, HistogramStyleKey);
}

}  // namespace

void promoteScopeStyles(Preferences& preferences)
{
    const RetiredChoices live = choicesFrom(preferences.scopeParams, RetiredChoices{});
    const Promotion promotion = promotionOf(live);

    preferences.scopeStack = rewriteTokens(preferences.scopeStack, promotion);
    preferences.scopeOrder = rewriteTokens(preferences.scopeOrder, promotion);
    rewriteKeys(preferences.layoutWeights, promotion);
    rewriteKeys(preferences.scopeShortcuts, promotion);
    for (LayoutPreset& preset : preferences.layoutPresets) {
        promotePreset(preset, live);
    }

    inherit(preferences.scopeParams, HistogramId, CombinedHistogramId, {"stride"});
    eraseChoice(preferences.scopeParams, HistogramId, HistogramStyleKey);
}

}  // namespace sidescopes
