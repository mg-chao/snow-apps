#ifndef SNOW_SHOT_PLATFORM_MACOS_CAPTUREWINDOWLAYERS_P_H
#define SNOW_SHOT_PLATFORM_MACOS_CAPTUREWINDOWLAYERS_P_H

#include <CoreGraphics/CoreGraphics.h>
#include <QVariant>
#include <QWindow>
#include <algorithm>
#include <array>

class QWidget;

namespace snow_shot::platform::detail {
inline constexpr auto kScreenshotLayer = "snowScreenshotWindowLayer";
inline constexpr auto kCaptureFamily = "snowCaptureWindowFamily";
inline constexpr int kOverlayLayer = 0;
inline constexpr int kRecognitionLayer = 1;
inline constexpr int kToolbarLayer = 2;
inline constexpr int kPopupLayer = 3;
inline constexpr int kCaptureBandSize = 128;

enum class CaptureFamily { Screenshot, Recording, GlobalCanvas, Pinned, Count };
using ModalFloors = std::array<int, static_cast<std::size_t>(CaptureFamily::Count)>;

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
        if (family == CaptureFamily::Screenshot)
            return layer;
        const int band = family == CaptureFamily::Pinned ? 2 : 1;
        return std::min(layer, kCaptureBandSize - 1) - band * kCaptureBandSize;
    }
};

// Keep system chrome < pins < the shared recording/canvas band < screenshots.
// Each band includes its tools, dialogs, and nested popups. Descendants must
// inherit the same policy without crossing into another capture family's band.
inline CGWindowLevel captureWindowLevel(CaptureLayer role) {
    return CGWindowLevelForKey(kCGScreenSaverWindowLevelKey) + role.offset();
}
inline CGWindowLevel pinnedWindowLevel() {
    return captureWindowLevel({CaptureFamily::Pinned, kOverlayLayer});
}

// PinnedWindowPlatform retains geometry/input responsibilities; stacking and
// transient descendants use the same policy as recording and screenshots.
void setPinnedWindowLayer(QWidget* widget, bool enabled);

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
} // namespace snow_shot::platform::detail
#endif
