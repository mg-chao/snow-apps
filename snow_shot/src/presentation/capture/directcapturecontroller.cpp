#include "snow_shot/presentation/directcapturecontroller.h"

#include "directcapturenative.h"
#include "camerashuttersound.h"
#include "snow_shot/presentation/directcapturehistory.h"
#include "snow_shot/presentation/directcaptureworkflow.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snowimageqtcodec.h"

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
    QString path;
    std::shared_ptr<ScreenshotClipboardPayload> payload;
};
} // namespace

class DirectCaptureController::Impl {
  public:
    explicit Impl(DirectCaptureController& controller)
        : owner(controller), worker(new QObject),
          workflow(DirectCapturePorts{
              [this](const auto& request, auto done) {
                  return submit<DirectCaptureFrame>(
                      [request]() {
                          auto frame = captureDirectTarget(request);
                          if (frame.isValid() &&
                              (!request.copyFile || request.historyEnabled ||
                               ScreenshotImageFileService::formatForKey(request.imageFormat) ==
                                   ScreenshotImageFileFormat::Png)) {
                              frame.canonicalPng =
                                  image_codec::encodePng(image_codec::srgbRowSource(frame.image));
                              if (frame.canonicalPng.isEmpty()) {
                                  frame.error = DirectCaptureController::tr(
                                      "The image could not be prepared for the clipboard");
                              }
                          }
                          return frame;
                      },
                      std::move(done));
              },
              [this](const auto& request, const auto& frame, auto done) {
                  return submit<OutputResult>(
                      [request, frame]() {
                          const auto format =
                              ScreenshotImageFileService::formatForKey(request.imageFormat);
                          const auto png = storage::PreparedPngImage::fromBytes(frame.image.size(),
                                                                                frame.canonicalPng);
                          const auto saved =
                              format == ScreenshotImageFileFormat::Png && png.has_value()
                                  ? ScreenshotImageFileService::saveAutomatically(
                                        *png, request.directories, request.filenameFormat,
                                        request.requestedAt)
                                  : ScreenshotImageFileService::saveAutomatically(
                                        frame.image, request.directories, format,
                                        request.filenameFormat, request.requestedAt);
                          return OutputResult{saved.error, saved.path, {}};
                      },
                      [done = std::move(done)](OutputResult result) {
                          done(result.path, result.error);
                      });
              },
              [this](const auto& request, const auto& frame, const auto& path, auto done) {
                  return copy(request, frame, path, std::move(done));
              },
              [this](const auto& request, const auto& frame, auto done) {
                  auto* repository = &storage::ApplicationStorage::instance().captureHistory();
                  return submit<OutputResult>(
                      [request, frame, repository]() {
                          const auto future =
                              repository->publish(directCaptureHistoryDraft(request, frame));
                          if (!future.valid())
                              return OutputResult{DirectCaptureController::tr(
                                                      "History publication could not be queued"),
                                                  {},
                                                  {}};
                          const auto result = future.get();
                          return OutputResult{
                              result.storage.success ? QString() : result.storage.error, {}, {}};
                      },
                      [done = std::move(done)](OutputResult result) { done(result.error); });
              },
              [this](const QString& error, bool warning) {
                  qWarning("Direct capture failed: %s", qPrintable(error));
                  emit owner.operationFailed(
                      DirectCaptureController::tr("Capture failed: %1").arg(error), warning);
              },
              []() { playCameraShutterSound(); },
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

    bool copy(const DirectCaptureRequest&, const DirectCaptureFrame& frame, const QString& path,
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
        return submit<OutputResult>(
            [frame]() {
                auto payload = std::make_shared<ScreenshotClipboardPayload>(
                    ScreenshotClipboardService::prepareImage(frame.image, frame.canonicalPng));
                return OutputResult{payload->isValid()
                                        ? QString()
                                        : DirectCaptureController::tr(
                                              "The image could not be prepared for the clipboard"),
                                    {},
                                    payload};
            },
            [this, done = std::move(done)](OutputResult result) {
                if (!result.error.isEmpty()) {
                    done(result.error);
                    return;
                }
                clipboard = ScreenshotClipboardService::commit(
                    QApplication::clipboard(), &owner, std::move(*result.payload),
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
        result.filenameFormat = settings.autoSaveFilenameFormat();
        return result;
    }

    void shutdown() {
        workflow.shutdown();
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
    ScreenshotClipboardCommitHandle clipboard;
    DirectCaptureWorkflow workflow;
};

DirectCaptureController::DirectCaptureController(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}
DirectCaptureController::~DirectCaptureController() = default;
void DirectCaptureController::shutdown() {
    m_impl->shutdown();
}

bool DirectCaptureController::blocksApplicationUpdate() const {
    return m_impl->workflow.pendingCount() > 0;
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
    m_impl->workflow.enqueue(std::move(request));
}
} // namespace snow_shot::presentation
