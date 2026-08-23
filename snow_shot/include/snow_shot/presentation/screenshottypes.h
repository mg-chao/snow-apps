#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTYPES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTYPES_H

#include <QImage>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <optional>

class QScreen;
class ScreenshotOverlayWindow;

enum class ScreenshotSessionState {
    IdleCold,
    IdlePrepared,
    Capturing,
    OverlayVisible,
    Editing,
    Releasing,
};

enum class ScreenshotOverlayShowMode {
    PreparedPreview,
    CapturedImage,
};

enum class ScreenshotCaptureBackend {
    Auto = 0,
    Dxgi = 1,
    WindowsGraphicsCapture = 2,
    Gdi = 3,
};

struct ScreenshotWindowCaptureFrame {
    QImage image;
    QRect physicalRect;
    ScreenshotCaptureBackend backend = ScreenshotCaptureBackend::Auto;

    [[nodiscard]] bool isValid() const {
        return !image.isNull() && !physicalRect.isEmpty() && physicalRect.size() == image.size();
    }
};

struct ScreenshotCaptureRequest {
    quint64 requestId = 0;
    bool refreshLayout = false;
    quintptr focusedWindowHandle = 0;
};

struct ScreenshotDisplayPresentationState {
    ScreenshotOverlayWindow* overlay = nullptr;
};

struct CapturedDisplayModel {
    QString stableId;
    QString name;
    QRect physicalRect;
    QRect canvasRect;
    QRect imageSourceCanvasRect;
    QRect logicalRect;
    QPointer<QScreen> screen;
    QImage image;
    bool active = false;
    ScreenshotCaptureBackend backend = ScreenshotCaptureBackend::Auto;
};

struct ScreenshotCaptureResult {
    quint64 requestId = 0;
    QVector<CapturedDisplayModel> displays;
    std::optional<ScreenshotWindowCaptureFrame> focusedWindow;
    QString errorMessage;
    bool succeeded = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTYPES_H
