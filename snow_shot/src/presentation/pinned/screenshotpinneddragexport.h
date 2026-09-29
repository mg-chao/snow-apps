#ifndef SCREENSHOTPINNEDDRAGEXPORT_H
#define SCREENSHOTPINNEDDRAGEXPORT_H

#include "snow_shot/presentation/screenshotexportartifact.h"
#include <QDrag>
#include <QPointer>

// Owns preparation only. Native drag ownership and published files outlive the pin.
class ScreenshotPinnedDragExport final : public QObject {
  public:
    using Executor = std::function<Qt::DropAction(QDrag&)>;
    using Completion = std::function<void(QString)>;
    explicit ScreenshotPinnedDragExport(QObject* parent = nullptr);
    ~ScreenshotPinnedDragExport() override;
    void start(
        std::shared_ptr<ScreenshotExportArtifact> artifact, Completion completion,
        std::function<bool()> canStart = [] { return true; });
    void cancel();
    bool busy() const;
    bool dragging() const;
    void setExecutor(Executor executor);

  private:
    struct Request;
    std::shared_ptr<Request> m_request;
    Executor m_executor;
    bool m_dragging = false;
    void finish(const std::shared_ptr<Request>& request, ScreenshotExportTaskResult result);
};

#endif
