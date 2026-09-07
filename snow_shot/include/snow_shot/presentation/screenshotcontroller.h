#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H

#include <QObject>
#include <QPoint>
#include <QString>
#include "snow_shot/presentation/globalmousetypes.h"

#include <memory>

namespace snow_shot::presentation {
class PinnedWindowGroupManager;
}
class ScreenshotOcrRecognitionService;

class ScreenshotController : public QObject {
    Q_OBJECT

  public:
    explicit ScreenshotController(
        QObject* parent = nullptr,
        snow_shot::presentation::PinnedWindowGroupManager* groupManager = nullptr,
        ScreenshotOcrRecognitionService* sharedOcrRecognition = nullptr);
    ~ScreenshotController() override;
    [[nodiscard]] bool captureAvailable() const;
    [[nodiscard]] bool
    beginGlobalMouseCapture(snow_shot::presentation::settings::SettingsGlobalMouseAction action,
                            quint64 gestureId, const QPoint& physicalStart);
    void updateGlobalMouseCapture(quint64 gestureId, const QPoint& physicalPoint);
    void finishGlobalMouseCapture(quint64 gestureId, const QPoint& physicalPoint);
    void cancelGlobalMouseCapture(quint64 gestureId);

  public slots:
    void prewarmResources();
    void restorePinnedWindows();
    void restoreActivePinnedGroupWindows();
    void startCapture();
    void startDelayedCapture(int delaySeconds);
    void captureAndPinSelection();
    void captureAndRecognizeText();
    void captureAndTranslateText();
    void captureAndCopySelection();
    void captureAndStartScreenRecording();
    void startOrStopScreenRecordingAndCopy();
    void editHistoryRecord(const QString& recordId);
    void pinClipboardContentToScreen();

  signals:
    void showMainWindowRequested();
    void captureAvailabilityChanged(bool available);
    void globalMouseCaptureEnded(quint64 gestureId);

  private:
    struct Impl;

    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H
