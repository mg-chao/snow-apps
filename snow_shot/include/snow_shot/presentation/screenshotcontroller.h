#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H

#include <QObject>
#include <QString>

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
    void captureCurrentMonitor();
    void captureFocusedWindow();
    void captureAndStartScreenRecording();
    void startOrStopScreenRecordingAndCopy();
    void editHistoryRecord(const QString& recordId);
    void pinClipboardContentToScreen();

  signals:
    void showMainWindowRequested();

  private:
    struct Impl;

    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H
