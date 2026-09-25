#ifndef SNOW_SHOT_PLATFORM_MACOS_CAPTUREWINDOWLAYERS_P_H
#define SNOW_SHOT_PLATFORM_MACOS_CAPTUREWINDOWLAYERS_P_H

#include <QVariant>
#include <QWindow>
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
