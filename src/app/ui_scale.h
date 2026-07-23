#pragma once

struct GLFWwindow;

namespace sidescopes {

/// Owns the interface scale: the factor everything on screen is drawn at, and
/// the user's size preference folded into it. What that factor should be on a
/// given window is the pure ui_scaling policy; this holds the state, applies
/// the result to the style, and is the one path the startup, the menu, and the
/// monitor-change sites all take, so a window crossing displays never loses the
/// preference.
class UiScaleController
{
public:
    /// @return The factor the interface is drawn at right now.
    [[nodiscard]] float scale() const;

    /// @return The user's interface-size factor, a multiplier on the OS scale
    ///         (1.0 = match system). One of UiScaleSteps.
    [[nodiscard]] float userFactor() const;

    /// Adopts @p savedFactor from preferences, cleaned to an offered step, and
    /// reapplies the scale on @p window.
    void restore(float savedFactor, GLFWwindow* window);

    /// Adopts the offered size step at @p index and reapplies the scale on
    /// @p window. An index no step answers to changes nothing.
    void selectStep(int index, GLFWwindow* window);

    /// Reapplies the interface scale from @p window's current monitor and the
    /// user's size factor. Returns whether it changed. The one path that folds
    /// the OS scale and the preference together, so both the startup and the
    /// monitor-change sites stay in agreement.
    bool refresh(GLFWwindow* window);

private:
    void apply(float scale);

    float m_scale = 1.0f;
    /// User interface-size factor, a multiplier on the OS scale (1.0 = match
    /// system). One of UiScaleSteps; multiplied into m_scale by refresh.
    float m_userFactor = 1.0f;
};

}  // namespace sidescopes
