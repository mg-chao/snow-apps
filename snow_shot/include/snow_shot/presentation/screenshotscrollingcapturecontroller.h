#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGCAPTURECONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGCAPTURECONTROLLER_H

#include "snow_shot/presentation/screenshotscrollingtypes.h"
#include "snow_shot/presentation/screenshotscrollingsnapshot.h"

#include <QObject>
#include <QRect>
#include <QSize>

#include <functional>
#include <memory>

class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotOverlayCoordinator;

struct ScreenshotScrollingCaptureControllerContext {
    ScreenshotDisplaySession& displaySession;
    const ScreenshotGeometryMapper& geometry;
    ScreenshotOverlayCoordinator& overlayCoordinator;
};

class ScreenshotScrollingCaptureController final : public QObject {
  public:
    using SnapshotResultCallback = std::function<void(ScreenshotScrollingSnapshot)>;

    explicit ScreenshotScrollingCaptureController(
        ScreenshotScrollingCaptureControllerContext context, QObject* parent = nullptr);
    ~ScreenshotScrollingCaptureController() override;

    [[nodiscard]] bool
    start(const QRect& canvasSelection,
          ScreenshotScrollingRecognitionMode mode = ScreenshotScrollingRecognitionMode::Vertical);
    [[nodiscard]] bool setRecognitionMode(ScreenshotScrollingRecognitionMode mode);
    [[nodiscard]] ScreenshotScrollingRecognitionMode recognitionMode() const;
    void stop(bool restoreScreenshotPresentation);
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool hasResult() const;
    [[nodiscard]] QSize trimmedSize() const;
    [[nodiscard]] bool requestTrimmedSnapshot(SnapshotResultCallback callback);
    [[nodiscard]] QRect canvasSelection() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGCAPTURECONTROLLER_H
