#pragma once

#include <array>
#include <cstddef>

#include "app/icon_textures.h"
#include "app/layout_presets.h"
#include "core/preferences.h"

namespace sidescopes {

/// @brief The toolbar's preset control: the button that names the active slot
///        and the list it opens.
///
/// The mouse mirror of the digit keys. It owns nothing a preset is made of -
/// every load, save and rename goes to the controller - and holds only what
/// belongs to the list itself: which row is being renamed and what has been
/// typed into it.
class LayoutPresetPicker
{
public:
    /// @p presets is the slots this draws and drives; it must outlive the
    /// picker.
    explicit LayoutPresetPicker(LayoutPresetController& presets);

    /// Draws the button and, while it is open, the slot list: one row a slot,
    /// which a click loads. Renaming and saving are buttons leading the row,
    /// shown on the row under the pointer - so a click on a row never has to
    /// mean two things, and neither action is reachable only by knowing about
    /// a modifier. The loaded slot is the tinted row. @p icons is the shared
    /// glyph cache every one of those glyphs draws from.
    [[nodiscard]] LayoutPresetOutcome draw(IconTextures& icons);

private:
    /// Draws one slot's row - its name, its key hint, and the button that
    /// renames it - or, while that slot is being renamed, the field it is
    /// renamed in.
    void drawSlotRow(int slot, float width, IconTextures& icons, LayoutPresetOutcome& outcome);

    /// Draws the field the name being renamed is edited in, committing on
    /// Enter or on the focus leaving it.
    void drawRenameField(float width, LayoutPresetOutcome& outcome);

    /// Puts @p slot into rename mode with its current name in the field.
    void beginRename(int slot);

    /// Takes the edited name to the controller and leaves rename mode.
    LayoutPresetOutcome commitRename();

    LayoutPresetController& m_presets;
    /// The slot being renamed (1-based), or 0 while none is. The name is
    /// edited in a fixed buffer a byte longer than a name may be, so the field
    /// stops taking input where the cleaning would have cut it.
    int m_renamingSlot = 0;
    /// Whether the field still has to be handed the keyboard, which is true
    /// for the one frame after a rename opens.
    bool m_renameFocusDue = false;
    std::array<char, MaximumPresetNameLength + 1> m_renameBuffer{};
};

}  // namespace sidescopes
