#ifndef SNOW_SHOT_PLATFORM_MACOS_CAPTUREWINDOWLAYERS_P_H
#define SNOW_SHOT_PLATFORM_MACOS_CAPTUREWINDOWLAYERS_P_H

#include <CoreGraphics/CoreGraphics.h>
#include <QVariant>
#include <QWindow>
#include <QWidget>
#include "widgets/window_creation_context.h"
#include <algorithm>
#include <array>

namespace snow_shot::platform::detail {
inline constexpr auto kScreenshotLayer = "snowScreenshotWindowLayer";
inline constexpr auto kCaptureFamily = "snowCaptureWindowFamily";
inline constexpr int kOverlayLayer = 0;
inline constexpr int kRecognitionLayer = 1;
inline constexpr int kToolbarLayer = 2;
inline constexpr int kPopupLayer = 3;
inline constexpr int kRecordingBandSize = 128;

enum class CaptureFamily { Screenshot, Recording };
using ModalFloors = std::array<int, 2>;

struct CaptureLayer {
    CaptureFamily family = CaptureFamily::Screenshot;
    int layer = -1;

    bool valid() const {
        return layer >= 0;
    }
    std::size_t index() const {
        return static_cast<std::size_t>(family);
    }
    int offset() const {
        return family == CaptureFamily::Recording
                   ? std::min(layer, kRecordingBandSize - 1) - kRecordingBandSize
                   : layer;
    }
};

// Keep system chrome < pins < the entire recording band < screenshots.
// Derive the pin level from the recording floor so changes to the reserved band
// cannot accidentally let pins cover a capture surface.
inline CGWindowLevel captureWindowLevel(CaptureLayer role) {
    return CGWindowLevelForKey(kCGScreenSaverWindowLevelKey) + role.offset();
}
inline CGWindowLevel pinnedWindowLevel() {
    return captureWindowLevel({CaptureFamily::Recording, kOverlayLayer}) - 1;
}

// Explicit roles survive on QWidget; inherited roles follow the current Qt
// transient owner, including pooled popups moving between capture families.
inline CaptureLayer captureLayer(QWindow* window, const ModalFloors& floors = {}) {
    if (!window)
        return {};
    CaptureLayer result;
    const QVariant role = window->property(kScreenshotLayer);
    if (role.isValid()) {
        result = {static_cast<CaptureFamily>(window->property(kCaptureFamily).toInt()),
                  role.toInt()};
    } else {
        result = captureLayer(window->transientParent(), floors);
        if (!result.valid())
            return result;
        result.layer = std::max(kPopupLayer, result.layer + 1);
    }
    if (window->modality() != Qt::NonModal)
        result.layer = std::max(result.layer, floors[result.index()]);
    return result;
}

// QWidget owns explicit roles before its first QWindow/native surface exists.
// A live transient owner takes precedence over a QObject parent used for pooling.
inline CaptureLayer widgetCaptureLayer(QWidget* widget, const ModalFloors& floors = {}) {
    if (!widget)
        return {};
    CaptureLayer result;
    const QVariant role = widget->property(kScreenshotLayer);
    if (role.isValid()) {
        result = {static_cast<CaptureFamily>(widget->property(kCaptureFamily).toInt()),
                  role.toInt()};
    } else if (const auto owner = adqt::widgets::ScopedWindowCreationOwner::ownerFor(widget)) {
        result = widgetCaptureLayer(owner->data(), floors);
        if (result.valid())
            result.layer = std::max(kPopupLayer, result.layer + 1);
    } else {
        QWindow* handle = widget->windowHandle();
        result = captureLayer(handle, floors);
        if (!result.valid() && (!handle || !handle->transientParent()) && widget->parentWidget()) {
            result = widgetCaptureLayer(widget->parentWidget()->window(), floors);
            if (result.valid())
                result.layer = std::max(kPopupLayer, result.layer + 1);
        }
    }
    if (result.valid() && widget->windowModality() != Qt::NonModal)
        result.layer = std::max(result.layer, floors[result.index()]);
    return result;
}
} // namespace snow_shot::platform::detail
#endif
