#ifndef SNOW_SHOT_PLATFORM_WINDOWS_SCROLLINPUT_H
#define SNOW_SHOT_PLATFORM_WINDOWS_SCROLLINPUT_H

#include <QPoint>
#include <QRect>

namespace snow_shot::platform::windows {
struct ScrollInputResult {
    enum class Status {
        Posted,
        InvalidRequest,
        TargetNotFound,
        CoordinateFailure,
        PostFailed,
        Unsupported
    };
    Status status = Status::Unsupported;
    quint32 error = 0;
};
ScrollInputResult sendScrollingWheelStep(const QRect& physicalSelection, const QPoint& wheelDelta);
} // namespace snow_shot::platform::windows

#endif // SNOW_SHOT_PLATFORM_WINDOWS_SCROLLINPUT_H
