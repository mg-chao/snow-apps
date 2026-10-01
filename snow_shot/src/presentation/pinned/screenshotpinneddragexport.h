#ifndef SCREENSHOTPINNEDDRAGEXPORT_H
#define SCREENSHOTPINNEDDRAGEXPORT_H

#include "snow_shot/presentation/screenshotexportartifact.h"
#include <QDrag>
#include <QList>
#include <QPointer>
#include <QTimer>

class QTemporaryDir;

namespace screenshot_pinned_drag_export {
// URL receivers can begin reading after the native drag finishes. Reserve their temporary files
// for a supported transfer window, admitting new drags only while count and byte budgets allow.
class FileRetention final : public QObject {
  public:
    using Clock = std::function<qint64()>;
    using RemoveDirectory = std::function<bool(QTemporaryDir&)>;
    struct Lease;
    using LeaseHandle = std::shared_ptr<Lease>;
    static constexpr int MaximumFileCount = 64;
    static constexpr qint64 MaximumBytes = 512LL * 1024 * 1024;
    static constexpr qint64 TransferGraceMilliseconds = 30 * 60 * 1000;

    explicit FileRetention(QObject* parent = nullptr, Clock clock = {},
                           RemoveDirectory removeDirectory = {});
    [[nodiscard]] LeaseHandle reserve(std::shared_ptr<QTemporaryDir> directory, qint64 bytes);
    void complete(const LeaseHandle& lease, Qt::DropAction action);
    void expire();
    [[nodiscard]] qsizetype retainedFileCount() const;
    [[nodiscard]] qint64 retainedBytes() const;

  private:
    void scheduleExpiry();
    Clock m_clock;
    RemoveDirectory m_removeDirectory;
    QTimer m_timer;
    QList<LeaseHandle> m_leases;
    qint64 m_bytes = 0;
};
} // namespace screenshot_pinned_drag_export

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
    void finish(const std::shared_ptr<Request>& request, const ScreenshotExportTaskResult& result);
};

#endif
