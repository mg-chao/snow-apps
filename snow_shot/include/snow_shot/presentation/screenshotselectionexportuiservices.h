#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTUISERVICES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTUISERVICES_H

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class QScreen;
class ScreenshotOcrRecognitionPort;
class ScreenshotQrRecognitionPort;
class SnowShotApiClient;
class ScreenshotPinnedWindowPool;
class QTextDocument;

class ScreenshotSelectionExportUiServices final : public ScreenshotSelectionExportDestinationPort {
  public:
    explicit ScreenshotSelectionExportUiServices(
        ScreenshotOcrRecognitionPort* recognition = nullptr,
        ScreenshotQrRecognitionPort* qrRecognition = nullptr,
        SnowShotApiClient* tableRecognition = nullptr,
        std::function<void()> showMainWindowRequested = {});
    ~ScreenshotSelectionExportUiServices() override;

    [[nodiscard]] bool publishClipboard(QObject* receiver, ScreenshotClipboardPayload payload,
                                        ClipboardCompletion completion) override;
    [[nodiscard]] bool publishClipboard(QObject* receiver, ScreenshotClipboardPayload payload,
                                        ClipboardCompletion completion,
                                        quint64 publicationId) override;
    void cancelClipboardPublication();
    void setClipboardImage(const QImage& image);
    // Prepares an off-screen pinned shell for content whose dimensions are not
    // available until decoding or rendering completes.
    void prewarmPinnedWindow(QScreen* screen = nullptr);
    // A null image is accepted when imageLoader is provided and
    // fullResolutionScaleBasis supplies the known canvas dimensions.
    [[nodiscard]] bool presentPinnedImage(const QImage& image, QScreen* screen,
                                          const QRect& nativeGeometry,
                                          const QSize& fullResolutionScaleBasis = {},
                                          std::shared_ptr<QTextDocument> formattedTextDocument = {},
                                          const QString& formattedPlainText = {},
                                          qreal formattedTextDevicePixelRatio = 1.0,
                                          ScreenshotClipboardOriginalContent originalContent = {},
                                          ScreenshotImageLoader imageLoader = {},
                                          PinnedCompletion completion = {});
    [[nodiscard]] bool
    presentPinnedSelection(const ScreenshotPinnedSelectionRequest& request,
                           PinnedCompletion completion) override;

  private:
    ScreenshotOcrRecognitionPort* m_recognition = nullptr;
    ScreenshotQrRecognitionPort* m_qrRecognition = nullptr;
    SnowShotApiClient* m_tableRecognition = nullptr;
    std::function<void()> m_showMainWindowRequested;
    std::unique_ptr<ScreenshotPinnedWindowPool> m_windowPool;
    std::vector<ScreenshotClipboardCommitHandle> m_clipboardCommits;
    std::vector<std::shared_ptr<std::atomic_bool>> m_clipboardCompletionEnabled;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTUISERVICES_H
