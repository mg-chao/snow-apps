#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTUISERVICES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTUISERVICES_H

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"

#include <atomic>
#include <functional>
#include <memory>

class SnowCanvasRuntime;
class QScreen;
class ScreenshotOcrRecognitionPort;
class ScreenshotQrRecognitionPort;
class SnowShotApiClient;
class ScreenshotPinnedWindowPool;
class QTextDocument;

class ScreenshotSelectionExportUiServices final : public ScreenshotSelectionExportDestinationPort {
  public:
    explicit ScreenshotSelectionExportUiServices(
        SnowCanvasRuntime& runtime, ScreenshotOcrRecognitionPort* recognition = nullptr,
        ScreenshotQrRecognitionPort* qrRecognition = nullptr,
        SnowShotApiClient* tableRecognition = nullptr,
        std::function<void()> showApplicationInterfaceRequested = {},
        std::function<void()> pinnedWindowPresented = {},
        std::function<void()> pinnedWindowDestroyed = {});
    ~ScreenshotSelectionExportUiServices() override;

    [[nodiscard]] bool publishClipboard(QObject* receiver, ScreenshotClipboardPayload payload,
                                        ClipboardCompletion completion) override;
    void cancelClipboardPublication();
    void setClipboardImage(const QImage& image);
    [[nodiscard]] bool presentPinnedImage(const QImage& image, QScreen* screen,
                                          const QRect& nativeGeometry,
                                          const QSize& fullResolutionScaleBasis = {},
                                          std::shared_ptr<QTextDocument> formattedTextDocument = {},
                                          const QString& formattedPlainText = {},
                                          qreal formattedTextDevicePixelRatio = 1.0,
                                          ScreenshotClipboardOriginalContent originalContent = {});
    [[nodiscard]] bool
    presentPinnedSelection(const ScreenshotPinnedSelectionRequest& request) override;
    [[nodiscard]] bool hasLivePresentedWindows() const;

  private:
    SnowCanvasRuntime& m_runtime;
    ScreenshotOcrRecognitionPort* m_recognition = nullptr;
    ScreenshotQrRecognitionPort* m_qrRecognition = nullptr;
    SnowShotApiClient* m_tableRecognition = nullptr;
    std::function<void()> m_showApplicationInterfaceRequested;
    std::function<void()> m_pinnedWindowPresented;
    std::function<void()> m_pinnedWindowDestroyed;
    std::unique_ptr<ScreenshotPinnedWindowPool> m_windowPool;
    ScreenshotClipboardCommitHandle m_clipboardCommit;
    std::shared_ptr<std::atomic_bool> m_clipboardCompletionEnabled;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTUISERVICES_H
