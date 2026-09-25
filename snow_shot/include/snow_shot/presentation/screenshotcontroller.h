#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H

#include <QObject>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include "snow_shot/presentation/globalmousetypes.h"

#include <memory>
#include <functional>

namespace snow_shot::platform {
struct SelectedFileTarget;
}
namespace snow_shot::presentation {
class PinnedWindowGroupManager;
}
class ScreenshotOcrRecognitionService;
class SnowShotApiClient;
class ScreenshotExportArtifact;

class ScreenshotController : public QObject {
    Q_OBJECT

  public:
    explicit ScreenshotController(
        QObject* parent = nullptr,
        snow_shot::presentation::PinnedWindowGroupManager* groupManager = nullptr,
        ScreenshotOcrRecognitionService* sharedOcrRecognition = nullptr,
        SnowShotApiClient* sharedApiClient = nullptr);
    ~ScreenshotController() override;
    void pinSelectedFilesToScreen(snow_shot::platform::SelectedFileTarget target);
    [[nodiscard]] bool captureAvailable() const;
    [[nodiscard]] bool blocksApplicationUpdate() const;
    [[nodiscard]] bool beginGlobalMouseCapture(
        snow_shot::presentation::settings::SettingsGlobalMouseAction action, quint64 gestureId,
        const QPointF& position,
        snow_shot::presentation::GlobalMouseCoordinateSpace space =
            snow_shot::presentation::GlobalMouseCoordinateSpace::PhysicalPixels);
    void updateGlobalMouseCapture(quint64 gestureId, const QPointF& position);
    void finishGlobalMouseCapture(quint64 gestureId, const QPointF& position);
    void cancelGlobalMouseCapture(quint64 gestureId);

    void setRecordingPermissionCheck(std::function<bool(bool, bool, bool)> check);

    [[nodiscard]] QJsonObject mcpState() const;
    using McpCompletion = std::function<void(QJsonObject, QString)>;
    void mcpCommand(const QString& method, const QJsonObject& params, McpCompletion completion);
    void mcpCancelCommand();
    void mcpDetached();
    [[nodiscard]] bool mcpBegin(const QJsonObject& options, QString* error);
    [[nodiscard]] bool mcpSetSelection(const QJsonObject& params, QString* error);
    [[nodiscard]] bool mcpSetTool(const QString& tool, QString* error);
    [[nodiscard]] bool mcpApplyAnnotations(const QByteArray& payload, QJsonObject* result,
                                           QString* error);
    [[nodiscard]] std::shared_ptr<ScreenshotExportArtifact> mcpExportArtifact(qreal scale);
    [[nodiscard]] bool mcpPinArtifact(std::shared_ptr<ScreenshotExportArtifact> artifact,
                                      std::function<void(bool)> completion);

  public slots:
    void prewarmResources();
    void restorePinnedWindows();
    void restoreLastClosedPinnedWindow();
    void showPinnedRecord(const QString& id);
    void destroyPinnedRecords(const QVector<QString>& ids);
    void restoreActivePinnedGroupWindows();
    void startCapture();
    void startDelayedCapture(int delaySeconds);
    void captureAndPinSelection();
    void captureAndRecognizeText();
    void captureAndTranslateText();
    void captureAndCopySelection();
    void captureAndStartScreenRecording();
    void startOrStopScreenRecordingAndCopy();
    void openScreenRecordingFolder();
    void editHistoryRecord(const QString& recordId);
    void pinHistoryRecord(const QString& recordId);
    void pinClipboardContentToScreen();
    void pinSelectedFilesToScreen();
    // MCP uses the same controller actions as the toolbar. These narrow
    // adapters preserve the controller's GUI-thread affinity.
    void mcpCancelCapture();
    void mcpCopySelectionToClipboard();
    void mcpPinSelectionToScreen();
    void mcpUndoCanvasEdit();
    void mcpRedoCanvasEdit();

  signals:
    void selectedFilePinFailed(const QString& message);
    void showMainWindowRequested();
    void accessibilityPermissionRequested();
    void translationPageRequested(const QString& text);
    void captureAvailabilityChanged(bool available);
    void globalMouseCaptureEnded(quint64 gestureId);
    void mcpCapturePresented();
    void mcpCaptureTerminated();
    void mcpCanvasChanged();

  private:
    struct Impl;

    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H
