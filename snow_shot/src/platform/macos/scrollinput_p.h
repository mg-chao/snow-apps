#pragma once

#include "snow_shot/platform/windows/scrollinput.h"

#include <CoreGraphics/CoreGraphics.h>
#include <QRectF>
#include <QVector>
#include <functional>
#include <optional>

namespace snow_shot::platform::macos::detail {
using ScrollInputResult = windows::ScrollInputResult;

struct ScrollDisplay {
    QRectF logicalBounds;
    qreal backingScale = 1;
};
struct ScrollWindow {
    pid_t process = 0;
    CGWindowID id = 0;
    QRectF logicalBounds;
    qreal opacity = 1;
    int layer = 0;
};
struct ScrollRequest {
    pid_t process = 0;
    CGWindowID window = 0;
    QPointF position;
    QPoint lines;
};
struct ScrollNativeApi {
    pid_t ownProcess = 0;
    std::function<bool()> mayPost;
    std::function<QVector<ScrollDisplay>()> displays;
    std::function<QVector<ScrollWindow>()> windows;
    std::function<ScrollInputResult(const ScrollRequest&)> post;
};

[[nodiscard]] std::optional<QPointF> logicalScrollPosition(QPoint physicalPoint,
                                                           const QVector<ScrollDisplay>& displays);
[[nodiscard]] ScrollInputResult
dispatchScrollingWheelStep(const QRect& selection, const QPoint& delta, const ScrollNativeApi& api);
// Returns an owned event for dispatch or inspection; this function never posts it.
[[nodiscard]] CGEventRef createScrollEvent(const ScrollRequest& request);
} // namespace snow_shot::platform::macos::detail

namespace snow_shot::platform::macos {
[[nodiscard]] windows::ScrollInputResult sendScrollingWheelStep(const QRect& selection,
                                                                const QPoint& delta);
}
