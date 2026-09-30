#include "screenshotpinneddragexport.h"
#include "snow_shot/presentation/screenshotencodingsettings.h"
#include "snow_shot/storage/settingsadapters.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <utility>

namespace {
QString queueError() {
    return QCoreApplication::translate("ScreenshotController",
                                       "The screenshot export queue is full");
}

screenshot_pinned_drag_export::FileRetention* retainedDragFiles() {
    static QPointer<screenshot_pinned_drag_export::FileRetention> retention;
    if (!retention)
        retention = new screenshot_pinned_drag_export::FileRetention(qApp);
    return retention;
}
} // namespace

namespace screenshot_pinned_drag_export {
struct FileRetention::Lease {
    std::shared_ptr<QTemporaryDir> directory;
    qint64 bytes = 0;
    std::optional<qint64> expiresAt;
};

FileRetention::FileRetention(QObject* parent, Clock clock, RemoveDirectory removeDirectory)
    : QObject(parent), m_clock(std::move(clock)), m_removeDirectory(std::move(removeDirectory)) {
    if (!m_clock) {
        m_clock = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
    }
    if (!m_removeDirectory)
        m_removeDirectory = [](QTemporaryDir& directory) { return directory.remove(); };
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &FileRetention::expire);
}

FileRetention::LeaseHandle FileRetention::reserve(std::shared_ptr<QTemporaryDir> directory,
                                                  qint64 bytes) {
    expire();
    if (!directory || !directory->isValid() || bytes < 0 || m_leases.size() >= MaximumFileCount ||
        bytes > MaximumBytes - m_bytes)
        return {};
    auto lease = std::make_shared<Lease>();
    lease->directory = std::move(directory);
    lease->bytes = bytes;
    m_bytes += bytes;
    m_leases.append(lease);
    return lease;
}

void FileRetention::complete(const LeaseHandle& lease, Qt::DropAction action) {
    if (!lease || !m_leases.contains(lease))
        return;
    if (action == Qt::IgnoreAction) {
        lease->expiresAt = m_clock();
        expire();
        return;
    }
    // QDrag has no receiver-consumption acknowledgement. The lease starts after the native
    // loop returns so a long-running drag never consumes a receiver's transfer grace period.
    lease->expiresAt = m_clock() + TransferGraceMilliseconds;
    scheduleExpiry();
}

void FileRetention::expire() {
    const qint64 now = m_clock();
    for (auto it = m_leases.begin(); it != m_leases.end();) {
        const auto& lease = *it;
        if (!lease->expiresAt || *lease->expiresAt > now) {
            ++it;
            continue;
        }
        if (QFileInfo::exists(lease->directory->path()) && !m_removeDirectory(*lease->directory)) {
            // A receiver can still hold a Windows file handle. Keep its budget reserved until
            // deletion succeeds instead of admitting more files and losing the cleanup owner.
            lease->expiresAt = now + 60 * 1000;
            ++it;
            continue;
        }
        m_bytes -= lease->bytes;
        it = m_leases.erase(it);
    }
    scheduleExpiry();
}

qsizetype FileRetention::retainedFileCount() const {
    return m_leases.size();
}

qint64 FileRetention::retainedBytes() const {
    return m_bytes;
}

void FileRetention::scheduleExpiry() {
    m_timer.stop();
    std::optional<qint64> first;
    for (const auto& lease : m_leases) {
        if (lease->expiresAt && (!first || *lease->expiresAt < *first))
            first = lease->expiresAt;
    }
    if (first) {
        const qint64 delay =
            std::clamp(*first - m_clock(), qint64(1), qint64(std::numeric_limits<int>::max()));
        m_timer.start(static_cast<int>(delay));
    }
}
} // namespace screenshot_pinned_drag_export

struct ScreenshotPinnedDragExport::Request {
    std::shared_ptr<ScreenshotExportArtifact> artifact;
    std::shared_ptr<QTemporaryDir> directory;
    ScreenshotExportJobHandle job;
    QImage image;
    Completion completion;
    std::function<bool()> canStart;
};

ScreenshotPinnedDragExport::ScreenshotPinnedDragExport(QObject* parent) : QObject(parent) {
    m_executor = [](QDrag& drag) { return drag.exec(Qt::CopyAction, Qt::CopyAction); };
}
ScreenshotPinnedDragExport::~ScreenshotPinnedDragExport() {
    cancel();
}
bool ScreenshotPinnedDragExport::busy() const {
    return m_request != nullptr;
}
bool ScreenshotPinnedDragExport::dragging() const {
    return m_dragging;
}
void ScreenshotPinnedDragExport::setExecutor(Executor executor) {
    m_executor = std::move(executor);
}

void ScreenshotPinnedDragExport::cancel() {
    auto request = std::exchange(m_request, {});
    if (request) {
        request->artifact->cancel();
        request->job.cancel();
    }
    if (m_dragging)
        QDrag::cancel();
}

void ScreenshotPinnedDragExport::start(std::shared_ptr<ScreenshotExportArtifact> artifact,
                                       Completion completion, std::function<bool()> canStart) {
    cancel();
    auto request = std::make_shared<Request>();
    request->artifact = std::move(artifact);
    request->completion = std::move(completion);
    request->canStart = std::move(canStart);
    m_request = request;
    const snow_shot::storage::ScreenshotSettings settings;
    const auto format = ScreenshotImageFileService::formatForKey(settings.imageFormat());
    const auto encoding = snow_shot::presentation::screenshotEncodingOptions(settings);
    const ScreenshotPdfOptions pdf{screenshot_pdf::pageSizeForKey(settings.pdfPageSize())};
    const QString filenameFormat = settings.autoSaveFilenameFormat();
    const QDateTime requestedAt = QDateTime::currentDateTime();
    const bool accepted =
        request->artifact->requestImage(this, [this, request, format, encoding, pdf, filenameFormat,
                                               requestedAt](ScreenshotExportImageResult result) {
            if (m_request != request)
                return;
            if (!result.succeeded()) {
                finish(request, ScreenshotExportTaskResult::failure(
                                    ScreenshotExportFailureStage::Render, result.error));
                return;
            }
            request->image = std::move(result.image);
            request->directory = std::make_shared<QTemporaryDir>(
                QDir::temp().filePath(QStringLiteral("snow-shot-drag-XXXXXX")));
            request->job = ScreenshotExportCoordinator::shared().submit(
                this, ScreenshotExportCoordinator::Priority::Foreground,
                [directory = request->directory, image = request->image, format, encoding, pdf,
                 filenameFormat, requestedAt](const ScreenshotExportCancellation& cancellation) {
                    if (cancellation.isCancellationRequested())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Cancelled, {});
                    if (!directory->isValid())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::File, directory->errorString());
                    const auto saved = ScreenshotImageFileService::saveAutomatically(
                        image, {directory->path()}, format, filenameFormat, requestedAt, pdf,
                        encoding);
                    ScreenshotExportTaskResult output;
                    output.savedPath = saved.path;
                    output.error = saved.error;
                    if (!saved.succeeded())
                        output.failureStage = ScreenshotExportFailureStage::File;
                    return output;
                },
                [this, request](const ScreenshotExportTaskResult& saved) {
                    finish(request, saved);
                });
            if (!request->job.isValid())
                finish(request, ScreenshotExportTaskResult::failure(
                                    ScreenshotExportFailureStage::Queue, queueError()));
        });
    if (!accepted)
        finish(request, ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Queue,
                                                            queueError()));
}

void ScreenshotPinnedDragExport::finish(const std::shared_ptr<Request>& request,
                                        const ScreenshotExportTaskResult& result) {
    if (m_request != request)
        return;
    if (!request->canStart()) {
        m_request.reset();
        request->completion({});
        return;
    }
    if (!result.succeeded()) {
        m_request.reset();
        request->completion(result.error);
        return;
    }
    QPointer<screenshot_pinned_drag_export::FileRetention> retention = retainedDragFiles();
    const auto lease = retention->reserve(request->directory, QFileInfo(result.savedPath).size());
    if (!lease) {
        m_request.reset();
        request->completion(queueError());
        return;
    }
    auto drag = std::make_unique<QDrag>(qApp);
    auto* mime = new QMimeData;
    mime->setImageData(request->image);
    mime->setUrls({QUrl::fromLocalFile(result.savedPath)});
    drag->setMimeData(mime);
    drag->setPixmap(QPixmap::fromImage(
        request->image.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    drag->setHotSpot(QPoint(drag->pixmap().width() / 2, drag->pixmap().height() / 2));
    const auto execute = m_executor;
    QPointer<ScreenshotPinnedDragExport> guard(this);
    m_dragging = true;
    const Qt::DropAction action = execute(*drag);
    // Only the file lease outlives the drag; it never retains source pixels or export artifacts.
    if (retention)
        retention->complete(lease, action);
    if (!guard)
        return;
    m_dragging = false;
    if (m_request == request) {
        m_request.reset();
        request->completion({});
    }
}
