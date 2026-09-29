#include "screenshotpinneddragexport.h"
#include "snow_shot/presentation/screenshotencodingsettings.h"
#include "snow_shot/storage/settingsadapters.h"
#include <QApplication>
#include <QDir>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <utility>

namespace {
QString queueError() {
    return QCoreApplication::translate("ScreenshotController",
                                       "The screenshot export queue is full");
}
} // namespace

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
                [this, request](ScreenshotExportTaskResult saved) {
                    finish(request, std::move(saved));
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
                                        ScreenshotExportTaskResult result) {
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
    // Some receivers open URLs after exec() returns. Keep published files until
    // application teardown, independently of this service and its source window.
    QObject::connect(qApp, &QObject::destroyed,
                     [directory = request->directory] { static_cast<void>(directory); });
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
    execute(*drag);
    if (!guard)
        return;
    m_dragging = false;
    if (m_request == request) {
        m_request.reset();
        request->completion({});
    }
}
