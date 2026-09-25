#include "snow_shot/presentation/directcapturecontroller.h"
#include "snow_shot/presentation/screenshotencodingsettings.h"

#include "directcapturenative.h"
#include "snow_shot/platform/screenshotnative.h"
#include "camerashuttersound.h"
#include "snow_shot/presentation/directcapturehistory.h"
#include "snow_shot/presentation/directcaptureworkflow.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/presentation/screenshotexportartifact.h"

#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QMimeData>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <exception>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace snow_shot::presentation {
namespace {

struct OutputResult {
    QString error;
};
} // namespace

class DirectCaptureController::Impl {
  public:
    explicit Impl(DirectCaptureController& controller)
        : owner(controller), worker(new QObject),
          workflow(DirectCapturePorts{
              [this](const auto& request, auto done) {
                  artifact.reset();
                  return submit<DirectCaptureFrame>(
                      [request]() { return captureDirectTarget(request); },
                      [this, request, done = std::move(done)](DirectCaptureFrame frame) mutable {
                          if (frame.isValid()) {
                              artifact = std::make_unique<ScreenshotExportArtifact>(
                                  ScreenshotExportSource::fromImage(frame.image),
                                  request.encoding.compressionLevel);
                          }
                          done(std::move(frame));
                      });
              },
              [this](const auto& request, const auto&, auto done) {
                  return artifact &&
                         artifact->requestAutomaticSave(
                             &owner, request.directories,
                             ScreenshotImageFileService::formatForKey(request.imageFormat),
                             request.filenameFormat, request.encoding,
                             [done = std::move(done)](ScreenshotExportTaskResult result) {
                                 done(result.savedPath, result.error);
                             },
                             request.pdf, request.requestedAt);
              },
              [this](const auto& request, const auto& frame, const auto& path, auto done) {
                  return copy(request, frame, path, std::move(done));
              },
              [this](const auto& request, const auto& frame, auto done) {
                  return artifact &&
                         artifact->requestCanonicalPng(
                             &owner, [this, request, frame, done = std::move(done)](
                                         ScreenshotExportEncodingResult encoded) mutable {
                                 if (!encoded.succeeded()) {
                                     done(encoded.error);
                                     return;
                                 }
                                 auto draft = directCaptureHistoryDraft(request, frame,
                                                                        std::move(encoded.image));
                                 auto* repository =
                                     &storage::ApplicationStorage::instance().captureHistory();
                                 auto completion = std::make_shared<DirectCapturePorts::Completion>(
                                     std::move(done));
                                 if (!submit<OutputResult>(
                                         [repository, draft = std::move(draft)]() mutable {
                                             const auto future =
                                                 repository->publish(std::move(draft));
                                             if (!future.valid())
                                                 return OutputResult{DirectCaptureController::tr(
                                                     "History publication could not be queued")};
                                             const auto result = future.get();
                                             return OutputResult{result.storage.success
                                                                     ? QString()
                                                                     : result.storage.error};
                                         },
                                         [completion](OutputResult result) {
                                             (*completion)(result.error);
                                         })) {
                                     (*completion)(DirectCaptureController::tr(
                                         "History publication could not be queued"));
                                 }
                             });
              },
              [this](const QString& error, bool warning) {
                  qWarning("Direct capture failed: %s", qPrintable(error));
                  emit owner.operationFailed(
                      DirectCaptureController::tr("Capture failed: %1").arg(error), warning);
              },
              []() { playCameraShutterSound(); },
              [this]() {
                  // Completion can run inside the artifact's own callback dispatch.
                  if (artifact)
                      artifact.release()->deleteLater();
              },
          }) {
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.setObjectName(QStringLiteral("direct-capture"));
        thread.start();
    }

    ~Impl() {
        shutdown();
    }

    template <typename Result>
    bool submit(std::function<Result()> work, std::function<void(Result)> done) {
        if (stopped.load())
            return false;
        return QMetaObject::invokeMethod(
            worker,
            [this, work = std::move(work), done = std::move(done)]() mutable {
                if (stopped.load())
                    return;
                Result result;
                try {
                    result = work();
                } catch (const std::exception& error) {
                    result.error = QString::fromUtf8(error.what());
                } catch (...) {
                    result.error = DirectCaptureController::tr("The capture operation failed");
                }
                if (stopped.load())
                    return;
                QMetaObject::invokeMethod(
                    &owner,
                    [this, result = std::move(result), done = std::move(done)]() mutable {
                        if (!stopped.load())
                            done(std::move(result));
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

    bool copy(const DirectCaptureRequest&, const DirectCaptureFrame&, const QString& path,
              DirectCapturePorts::Completion done) {
        if (!path.isEmpty()) {
            auto* mime = new QMimeData;
            mime->setUrls({QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath())});
            clipboard = ScreenshotClipboardService::commitMimeData(
                QApplication::clipboard(), &owner, mime,
                [done](ScreenshotClipboardCommitResult result) {
                    done(result.succeeded() ? QString() : result.errorString());
                });
            return clipboard.isValid();
        }
        return artifact &&
               artifact->requestClipboard(
                   &owner, [this, done = std::move(done)](ScreenshotExportClipboardResult result) {
                       if (!result.succeeded()) {
                           done(result.error);
                           return;
                       }
                       clipboard = ScreenshotClipboardService::commit(
                           QApplication::clipboard(), &owner, std::move(result.payload),
                           [done](ScreenshotClipboardCommitResult commit) {
                               done(commit.succeeded() ? QString() : commit.errorString());
                           });
                       if (!clipboard.isValid())
                           done(DirectCaptureController::tr(
                               "The clipboard publication could not be queued"));
                   });
    }

    DirectCaptureRequest request(DirectCaptureTarget target) {
        const storage::ScreenshotSettings settings;
        DirectCaptureRequest result;
        result.target = target;
        result.shutterSoundNotification = settings.shutterSoundNotification();
        result.restoreOriginalScreenColors = settings.restoreOriginalScreenColors();
        result.requestedAt = QDateTime::currentDateTime();
        result.autoSave = settings.autoSaveAfterCopy();
        result.copyFile = settings.copyImageFileToClipboard();
        result.historyEnabled =
            storage::ApplicationStorage::instance().captureHistory().policy().enabled;
        result.directories =
            ScreenshotImageFileService::automaticDirectories(settings.imageSaveDirectory());
        result.imageFormat = settings.imageFormat();
        result.encoding = screenshotEncodingOptions(settings);
        result.historyDisplayCompressionLevel = ScreenshotImageFileService::compressionLevelForKey(
            storage::ApplicationStorage::instance()
                .configuration()
                .value(QStringLiteral("capture_history/compression_level"))
                .toString());
        result.pdf.pageSize = screenshot_pdf::pageSizeForKey(settings.pdfPageSize());
        result.filenameFormat = settings.autoSaveFilenameFormat();
        return result;
    }

    void shutdown() {
        if (mcpCancellation)
            mcpCancellation->store(true, std::memory_order_release);
        mcpActive = false;
        workflow.shutdown();
        artifact.reset();
        clipboard.cancel();
        if (stopped.exchange(true))
            return;
        thread.quit();
        thread.wait();
    }

    DirectCaptureController& owner;
    QThread thread;
    QObject* worker;
    std::atomic_bool stopped = false;
    bool mcpActive = false;
    std::shared_ptr<std::atomic_bool> mcpCancellation;
    ScreenshotClipboardCommitHandle clipboard;
    DirectCaptureWorkflow workflow;
    std::unique_ptr<ScreenshotExportArtifact> artifact;
};

DirectCaptureController::DirectCaptureController(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}
DirectCaptureController::~DirectCaptureController() = default;
void DirectCaptureController::shutdown() {
    m_impl->shutdown();
}

bool DirectCaptureController::blocksApplicationUpdate() const {
    return m_impl->workflow.pendingCount() > 0 || m_impl->mcpActive;
}

void DirectCaptureController::captureFocusedWindow() {
    auto request = m_impl->request(DirectCaptureTarget::FocusedWindow);
#if defined(Q_OS_WIN)
    HWND target = GetForegroundWindow();
    if (target != nullptr) {
        const HWND root = GetAncestor(target, GA_ROOT);
        request.window = reinterpret_cast<quintptr>(root != nullptr ? root : target);
    }
#endif
#ifdef Q_OS_MACOS
    request.window = platform::screenshotFocusedWindow();
#endif
    m_impl->workflow.enqueue(std::move(request));
}

void DirectCaptureController::captureCurrentMonitor() {
    auto request = m_impl->request(DirectCaptureTarget::CurrentMonitor);
#if defined(Q_OS_WIN)
    POINT cursor{};
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetCursorPos(&cursor) && GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL),
                                                 reinterpret_cast<MONITORINFO*>(&info))) {
        request.monitorName = QString::fromWCharArray(info.szDevice);
    }
#endif
#ifdef Q_OS_MACOS
    const quint32 id = platform::screenshotDisplayAtCursor();
    if (id)
        request.monitorName = QStringLiteral("display:%1").arg(id);
#endif
    m_impl->workflow.enqueue(std::move(request));
}

bool DirectCaptureController::mcpCapture(
    const QJsonObject& options, std::function<void(QImage, QJsonObject, QString)> completion) {
    if (blocksApplicationUpdate())
        return false;
    const auto target =
        options.value(QStringLiteral("target")).toString(QStringLiteral("current_monitor"));
    if (target != QStringLiteral("current_monitor") && target != QStringLiteral("focused_window"))
        return false;
    DirectCaptureRequest request;
    request.target = target == QStringLiteral("focused_window")
                         ? DirectCaptureTarget::FocusedWindow
                         : DirectCaptureTarget::CurrentMonitor;
    request.restoreOriginalScreenColors =
        storage::ScreenshotSettings().restoreOriginalScreenColors();
    request.captureCursor = options.value(QStringLiteral("capture_cursor")).toBool(false);
#if defined(Q_OS_WIN)
    if (request.target == DirectCaptureTarget::FocusedWindow) {
        HWND window = GetForegroundWindow();
        if (window)
            request.window = reinterpret_cast<quintptr>(GetAncestor(window, GA_ROOT));
    } else {
        POINT cursor{};
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetCursorPos(&cursor) &&
            GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST),
                            reinterpret_cast<MONITORINFO*>(&info)))
            request.monitorName = QString::fromWCharArray(info.szDevice);
    }
#elif defined(Q_OS_MACOS)
    if (request.target == DirectCaptureTarget::FocusedWindow)
        request.window = platform::screenshotFocusedWindow();
    else
        request.monitorName =
            QStringLiteral("display:%1").arg(platform::screenshotDisplayAtCursor());
#endif
    const double scale = options.value(QStringLiteral("scale")).toDouble(1);
    if (!std::isfinite(scale) || scale < 0.1 || scale > 4)
        return false;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_impl->mcpCancellation = cancellation;
    m_impl->mcpActive = true;
    const bool started = m_impl->submit<DirectCaptureFrame>(
        [request, scale, cancellation] {
            if (cancellation->load(std::memory_order_acquire))
                return DirectCaptureFrame{};
            auto frame = captureDirectTarget(request);
            if (cancellation->load(std::memory_order_acquire))
                return DirectCaptureFrame{};
            if (!frame.image.isNull() && scale != 1) {
                const QSize size(qMax(1, qRound(frame.image.width() * scale)),
                                 qMax(1, qRound(frame.image.height() * scale)));
                if (static_cast<qint64>(size.width()) * size.height() > 100000000) {
                    frame.image = {};
                } else
                    frame.image =
                        frame.image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            return frame;
        },
        [this, cancellation, completion = std::move(completion)](DirectCaptureFrame frame) {
            if (cancellation->load(std::memory_order_acquire))
                return;
            m_impl->mcpActive = false;
            QJsonObject metadata{
                {QStringLiteral("identity"), frame.identity},
                {QStringLiteral("native_backend"), frame.backend},
                {QStringLiteral("physical_bounds"),
                 QJsonArray{frame.physicalBounds.x(), frame.physicalBounds.y(),
                            frame.physicalBounds.width(), frame.physicalBounds.height()}},
                {QStringLiteral("logical_bounds"),
                 QJsonArray{frame.logicalBounds.x(), frame.logicalBounds.y(),
                            frame.logicalBounds.width(), frame.logicalBounds.height()}}};
            completion(std::move(frame.image), metadata, frame.error);
        });
    if (!started) {
        m_impl->mcpActive = false;
        m_impl->mcpCancellation.reset();
    }
    return started;
}
void DirectCaptureController::cancelMcpCapture() {
    if (m_impl == nullptr)
        return;
    if (m_impl->mcpCancellation)
        m_impl->mcpCancellation->store(true, std::memory_order_release);
    m_impl->mcpActive = false;
}
} // namespace snow_shot::presentation
