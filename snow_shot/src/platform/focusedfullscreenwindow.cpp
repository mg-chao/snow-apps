#include "snow_shot/platform/focusedfullscreenwindow.h"

#include <algorithm>
#include <cmath>

namespace snow_shot::platform {
namespace {
bool coordinateMatches(qreal first, qreal second) {
    return std::abs(first - second) <= 1.0;
}

bool coversDisplay(const QRectF& window, const QRectF& display) {
    return coordinateMatches(window.left(), display.left()) &&
           coordinateMatches(window.top(), display.top()) &&
           coordinateMatches(window.right(), display.right()) &&
           coordinateMatches(window.bottom(), display.bottom());
}
} // namespace

bool focusedWindowCoversDisplay(qint64 frontmostProcessId,
                                const QVector<FocusedWindowSnapshot>& orderedWindows,
                                const QVector<QRectF>& displayBounds) {
    if (frontmostProcessId <= 0 || displayBounds.isEmpty()) {
        return false;
    }
    const auto focused = std::find_if(
        orderedWindows.cbegin(), orderedWindows.cend(), [frontmostProcessId](const auto& window) {
            return window.ownerProcessId == frontmostProcessId && window.layer == 0 &&
                   window.alpha > 0.0 && window.bounds.isValid() && !window.bounds.isEmpty();
        });
    return focused != orderedWindows.cend() &&
           std::any_of(displayBounds.cbegin(), displayBounds.cend(),
                       [focused](const QRectF& display) {
                           return display.isValid() && !display.isEmpty() &&
                                  coversDisplay(focused->bounds, display);
                       });
}

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
bool focusedFullscreenWindowExists() {
    return false;
}
#endif

} // namespace snow_shot::platform
