#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTSERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTSERVICE_H

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"

#include <QImage>
#include <QRect>

#include <functional>
#include <memory>

class ScreenshotDisplaySession;
class QObject;
class QThread;
class SnowCanvasRuntime;

struct ScreenshotExportServiceContext {
    const ScreenshotDisplaySession& displaySession;
    SnowCanvasRuntime& runtime;
    const ScreenshotGeometryMapper& geometry;
    std::function<void()> becameIdle;
};

class ScreenshotExportService final : public ScreenshotSelectionImageComposerPort {
  public:
    explicit ScreenshotExportService(ScreenshotExportServiceContext context);
    ~ScreenshotExportService() override;

    [[nodiscard]] bool hasPendingRequests() const noexcept;
    [[nodiscard]] bool releaseRetainedIdleResources(std::function<void(bool released)> completion);

    // Consumed by the next result or clipboard request. Focused-window capture uses this to export
    // the WGC surface directly while display snapshots remain untouched for history.
    void setNextSelectionSourceImage(QImage image);
    void clearNextSelectionSourceImage();

    [[nodiscard]] bool requestSelectionResult(const QRect& selection,
                                              const ScreenshotResultStyle& style, QObject* receiver,
                                              ImageCallback callback) override;
    [[nodiscard]] bool requestSelectionClipboard(const QRect& selection,
                                                 const ScreenshotResultStyle& style,
                                                 QObject* receiver,
                                                 ClipboardCallback callback) override;
    [[nodiscard]] std::optional<ScreenshotPinnedSelectionRequest>
    preparePinnedSelection(const QRect& selection,
                           const ScreenshotResultStyle& style) const override;
    [[nodiscard]] bool schedulePinnedSelection(ScreenshotPinnedSelectionRequest request,
                                               QObject* receiver,
                                               PinRequestCallback callback) override;

  private:
    class PendingRequest;
    struct PendingRequestState;

    [[nodiscard]] bool ensureWorkerRunning();

    ScreenshotExportServiceContext m_context;
    std::unique_ptr<QThread> m_thread;
    QObject* m_worker = nullptr;
    QObject* m_completionContext = nullptr;
    std::shared_ptr<PendingRequestState> m_pendingRequestState;
    QImage m_nextSelectionSourceImage;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTSERVICE_H
