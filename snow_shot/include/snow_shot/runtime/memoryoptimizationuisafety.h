#ifndef SNOW_SHOT_RUNTIME_MEMORYOPTIMIZATIONUISAFETY_H
#define SNOW_SHOT_RUNTIME_MEMORYOPTIMIZATIONUISAFETY_H

#include <QApplication>
#include <QWidget>
#include <functional>

namespace snow_shot::runtime {
// Only explicitly known static surfaces may remain visible during trimming.
inline bool memoryOptimizationUiIsSafe(const std::function<bool(const QWidget*)>& isIdleSurface) {
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget() ||
        QWidget::mouseGrabber() || QWidget::keyboardGrabber())
        return false;
    for (const auto* window : QApplication::topLevelWidgets()) {
        if (!window->isVisible() || window->isMinimized() || window->windowType() == Qt::ToolTip)
            continue;
        if (!isIdleSurface || !isIdleSurface(window))
            return false;
    }
    return true;
}
} // namespace snow_shot::runtime
#endif
