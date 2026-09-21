#ifndef SNOW_SHOT_PLATFORM_MACOS_SCREENSHOTINPUTREGION_P_H
#define SNOW_SHOT_PLATFORM_MACOS_SCREENSHOTINPUTREGION_P_H

#include <QRegion>
#include <Qt>

namespace snow_shot::platform::detail {
// Native transparency is window-wide. Retain a drag that began on this overlay
// (including the thumbnail) until its last button is released.
struct ScreenshotInputRegion {
    QRegion passThrough;
    quint64 heldButtons = 0;

    void press(unsigned button) {
        if (button < 64)
            heldButtons |= quint64{1} << button;
    }
    void release(unsigned button) {
        if (button < 64)
            heldButtons &= ~(quint64{1} << button);
    }
    [[nodiscard]] bool transparentAt(const QPoint& localPosition, bool visible) const {
        return visible && heldButtons == 0 && passThrough.contains(localPosition);
    }
};
} // namespace snow_shot::platform::detail

#endif
