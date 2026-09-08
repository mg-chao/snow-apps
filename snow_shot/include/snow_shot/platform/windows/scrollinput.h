#ifndef SNOW_SHOT_PLATFORM_WINDOWS_SCROLLINPUT_H
#define SNOW_SHOT_PLATFORM_WINDOWS_SCROLLINPUT_H

#include <QPoint>
#include <QRect>

namespace snow_shot::platform::windows {
void sendScrollingWheelStep(const QRect& physicalSelection, const QPoint& wheelDelta);
}

#endif // SNOW_SHOT_PLATFORM_WINDOWS_SCROLLINPUT_H
