#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTSERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTSERVICE_H

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"

#include <QImage>
#include <QRect>

#include <memory>

class ScreenshotDisplaySession;
class QObject;
class QThread;
class SnowCanvasRuntime;

struct ScreenshotExportServiceContext {
    const ScreenshotDisplaySession& displaySession;
    SnowCanvasRuntime& runtime;
    const ScreenshotGeometryMapper& geometry;
};

class ScreenshotExportService final : public ScreenshotSelectionImageComposerPort {
  public:
    explicit ScreenshotExportService(ScreenshotExportServiceContext context);
    ~ScreenshotExportService() override;

    [[nodiscard]] bool requestSelectionResult(const QRect& selection,
                                              const ScreenshotResultStyle& style, QObject* receiver,
                                              ImageCallback callback) override;
    [[nodiscard]] bool requestSelectionClipboard(const QRect& selection,
                                                 const ScreenshotResultStyle& style,
                                                 QObject* receiver,
                                                 ClipboardCallback callback) override;
    [[nodiscard]] bool requestSelectionResultAtScale(const QRect& selection,
                                                     const ScreenshotResultStyle& style,
                                                     qreal scale, QObject* receiver,
                                                     ImageCallback callback);
    [[nodiscard]] std::optional<ScreenshotPinnedSelectionRequest>
    preparePinnedSelection(const QRect& selection,
                           const ScreenshotResultStyle& style) const override;
    [[nodiscard]] bool schedulePinnedSelection(ScreenshotPinnedSelectionRequest request,
                                               QObject* receiver,
                                               PinRequestCallback callback) override;

  private:
    ScreenshotExportServiceContext m_context;
    std::unique_ptr<QThread> m_thread;
    QObject* m_worker = nullptr;
    QObject* m_completionContext = nullptr;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTSERVICE_H
