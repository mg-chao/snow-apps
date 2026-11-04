// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotairdropservice.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "nativeairdropbackend.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include <QFile>

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>

#include <atomic>
#include <utility>

struct ScreenshotAirDropService::Request {
    snow_shot::runtime::RuntimeActivityLease activity =
        snow_shot::runtime::RuntimeActivityTracker::shared().acquire();
    QPointer<QObject> receiver;
    QPointer<QWidget> owner;
    std::shared_ptr<QTemporaryDir> directory;
    std::atomic_bool cancelled{false};
    Completion completion;
    QMetaObject::Connection ownerDestroyed;
    QMetaObject::Connection receiverDestroyed;
    bool completed = false;
    ScreenshotExportJobHandle preparation;

    ~Request() {
        QObject::disconnect(ownerDestroyed);
        QObject::disconnect(receiverDestroyed);
    }
};

ScreenshotAirDropService::ScreenshotAirDropService(Backend backend, QObject* parent)
    : QObject(parent), m_backend(std::move(backend)) {}

ScreenshotAirDropService::~ScreenshotAirDropService() {
    if (m_request)
        finish(m_request, {Status::Cancelled, {}});
}

ScreenshotAirDropService& ScreenshotAirDropService::shared() {
    static QPointer<ScreenshotAirDropService> service;
    if (!service)
        service = new ScreenshotAirDropService(screenshotNativeAirDropBackend(), qApp);
    return *service;
}

bool ScreenshotAirDropService::busy() const {
    return m_request != nullptr;
}

void ScreenshotAirDropService::finish(const std::shared_ptr<Request>& request, Result result) {
    if (m_request != request || request->completed)
        return;
    // Copy before resetting: the destructor can pass m_request itself.
    const auto active = request;
    active->completed = true;
    active->cancelled.store(true, std::memory_order_release);
    active->preparation.cancel();
    QObject::disconnect(active->ownerDestroyed);
    QObject::disconnect(active->receiverDestroyed);
    m_request.reset();
    auto completion = std::move(active->completion);
    if (active->receiver && completion)
        completion(std::move(result));
}

bool ScreenshotAirDropService::shareImage(QObject* receiver, QWidget* owner, QImage snapshot,
                                          QString filename, Completion completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (busy() || !receiver || !owner || snapshot.isNull() || !completion)
        return false;
    auto request = std::make_shared<Request>();
    request->receiver = receiver;
    request->owner = owner;
    request->completion = std::move(completion);
    m_request = request;
    const auto abandon = [this, request] { finish(request, {Status::Cancelled, {}}); };
    request->ownerDestroyed = connect(owner, &QObject::destroyed, this, abandon);
    if (receiver != owner)
        request->receiverDestroyed = connect(receiver, &QObject::destroyed, this, abandon);

    // Filename formatting is supplied by the source's normal export settings. Only
    // the basename may enter this private directory; the format is always PNG.
    filename = QFileInfo(filename).fileName();
    if (filename.isEmpty() || filename == QStringLiteral(".") || filename == QStringLiteral(".."))
        filename = QStringLiteral("SnowShot.png");
    if (!filename.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        filename += QStringLiteral(".png");
    struct Prepared {
        std::shared_ptr<QTemporaryDir> directory;
        QString path;
        QString error;
    };
    auto prepared = std::make_shared<Prepared>();
    auto completedPreparation = [this, request, prepared](ScreenshotExportTaskResult result) {
        if (m_request != request || request->completed)
            return;
        if (!result.succeeded() || prepared->path.isEmpty()) {
            if (result.failureStage == ScreenshotExportFailureStage::Cancelled) {
                finish(request, {Status::Cancelled, {}});
                return;
            }
            if (prepared->error.isEmpty())
                prepared->error = result.error;
            finish(request, {Status::Failed, QCoreApplication::translate(
                                                 "ScreenshotAirDropService",
                                                 "The image could not be prepared for AirDrop: %1")
                                                 .arg(prepared->error)});
            return;
        }
        request->directory = std::move(prepared->directory);
        const QPointer<ScreenshotAirDropService> guard(this);
        const auto completed = [guard, request](Result result) {
            // Dispatch through the application so a worker callback never races
            // destruction of the service while selecting its invocation receiver.
            if (!qApp)
                return;
            QMetaObject::invokeMethod(
                qApp,
                [guard, request, result = std::move(result)]() mutable {
                    if (guard)
                        guard->finish(request, std::move(result));
                },
                Qt::QueuedConnection);
        };
        if (m_backend)
            m_backend(request->owner, prepared->path, completed);
        else
            completed({Status::Unavailable,
                       QCoreApplication::translate("ScreenshotAirDropService",
                                                   "AirDrop is unavailable on this Mac")});
    };
    const qint64 workingBytes = qint64(snapshot.width()) * snapshot.height() * 8 + 16 * 1024 * 1024;
    request->preparation = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [request, prepared, snapshot = std::move(snapshot),
         filename = std::move(filename)](const ScreenshotExportCancellation& cancellation) mutable {
            const auto cancelled = [&] {
                return cancellation.isCancellationRequested() ||
                       request->cancelled.load(std::memory_order_acquire);
            };
            const auto stopped = [] {
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled,
                                                           {});
            };
            if (cancelled())
                return stopped();
            prepared->directory = std::make_shared<QTemporaryDir>(
                QDir::tempPath() + QStringLiteral("/snow-shot-airdrop-XXXXXX"));
            if (!prepared->directory->isValid())
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                           prepared->directory->errorString());
            const QString path = prepared->directory->filePath(filename);
            // Qt's PNG writer retains the source profile and alpha. Abort its incremental
            // writes when the source disappears; the private directory owns partial output.
            class Output final : public QIODevice {
              public:
                Output(QString path, std::function<bool()> cancelled)
                    : file(std::move(path)), stopped(std::move(cancelled)) {
                    if (file.open(QIODevice::WriteOnly))
                        open(QIODevice::WriteOnly);
                    else
                        setErrorString(file.errorString());
                }
                bool isSequential() const override {
                    return true;
                }
                bool finish() {
                    return file.flush();
                }

              protected:
                qint64 readData(char*, qint64) override {
                    return -1;
                }
                qint64 writeData(const char* data, qint64 size) override {
                    if (stopped())
                        return -1;
                    const auto written = file.write(data, size);
                    if (written != size)
                        setErrorString(file.errorString());
                    return written;
                }

              private:
                QFile file;
                std::function<bool()> stopped;
            } output(path, cancelled);
            snapshot.setDevicePixelRatio(1.0);
            QImageWriter writer(&output, "png");
            const bool written = output.isOpen() && writer.write(snapshot) && output.finish();
            if (cancelled())
                return stopped();
            if (!written)
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                           writer.errorString());
            prepared->path = path;
            return ScreenshotExportTaskResult{};
        },
        completedPreparation, workingBytes);
    if (!request->preparation.isValid())
        QMetaObject::invokeMethod(
            this,
            [completedPreparation] {
                completedPreparation(ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Queue,
                    QCoreApplication::translate("ScreenshotController",
                                                "The screenshot export queue is full")));
            },
            Qt::QueuedConnection);
    return true;
}
