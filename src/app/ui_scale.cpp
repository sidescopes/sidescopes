#include "app/ui_scale.h"

#include <cstddef>

#include "app/app_startup.h"
#include "app/ui_scaling.h"
#include "imgui.h"

namespace sidescopes {

float UiScaleController::scale() const
{
    return m_scale;
}

float UiScaleController::userFactor() const
{
    return m_userFactor;
}

void UiScaleController::apply(float scale)
{
    m_scale = scale;
    // Scaling an already scaled style would compound, so rebuild from the base
    // theme each time.
    applyTheme();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;
}

bool UiScaleController::refresh(GLFWwindow* window)
{
    // The OS scale is the baseline - it already carries the monitor's own
    // recommendation - and the user factor asks for more or less on top. Folding
    // them in one place keeps the startup and monitor-change sites in step, so a
    // window crossing displays never loses the preference.
    const float target = computeUiScale(window) * m_userFactor;
    if (target == m_scale) {
        return false;
    }
    apply(target);

    return true;
}

void UiScaleController::restore(float savedFactor, GLFWwindow* window)
{
    m_userFactor = cleanedUiScaleFactor(savedFactor);
    refresh(window);
}

void UiScaleController::selectStep(int index, GLFWwindow* window)
{
    if (index < 0 || index >= static_cast<int>(UiScaleSteps.size())) {
        return;
    }
    m_userFactor = UiScaleSteps[static_cast<std::size_t>(index)];
    refresh(window);
}

}  // namespace sidescopes
