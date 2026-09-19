#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTYPES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTYPES_H

#include <QImage>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <optional>
#include <memory>

class QScreen;
class ScreenshotOverlayWindow;
struct SnowCaptureCursorSnapshotImpl;

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
    // Creates the native window, backing store, and DWM layered surface at
    // opacity 0 after capture is dispatched, so first-show cost overlaps frame
    // acquisition instead of sitting on the reveal path.
    WarmSurface,
    // Reveals the captured image like CapturedImage, but the first frame is
    // published by the next queued paint instead of a synchronous commit.
    // Used while a global-mouse drag is in progress so paced drag updates are
    // not queued behind the full-surface first paint.
    CapturedImageFramePaced,
};

enum class ScreenshotCaptureBackend {
    Auto = 0,
    Dxgi = 1,
    WindowsGraphicsCapture = 2,
    Gdi = 3,
    ScreenCaptureKit = 4,
};

enum class ScreenshotCapturePurpose {
    Initial,
    Recapture,
};

struct ScreenshotCaptureRequest {
    quint64 requestId = 0;
    bool refreshLayout = false;
    bool restoreOriginalScreenColors = false;
    bool captureCursor = false;
    ScreenshotCapturePurpose purpose = ScreenshotCapturePurpose::Initial;
    // Owned before worker dispatch; native snapshot data is immutable.
    std::shared_ptr<SnowCaptureCursorSnapshotImpl> cursorSnapshot;
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
    // Desktop points and image pixels are independent on macOS. physicalRect
    // remains the per-display pixel coordinate contract used by native selectors.
    QRect capturedLogicalRect;
    quint32 nativeDisplayId = 0;
    qreal backingScale = 1.0;
    bool canvasUsesPoints = false;
};

struct ScreenshotCaptureResult {
    quint64 requestId = 0;
    QVector<CapturedDisplayModel> displays;
    QString errorMessage;
    bool succeeded = false;
    ScreenshotCapturePurpose purpose = ScreenshotCapturePurpose::Initial;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTYPES_H
