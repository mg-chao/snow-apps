// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotairdropservice.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "nativeairdropbackend.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageWriter>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <QtConcurrentRun>

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
    auto* watcher = new QFutureWatcher<Prepared>(this);
    connect(watcher, &QFutureWatcher<Prepared>::finished, this, [this, request, watcher] {
        auto prepared = watcher->result();
        watcher->deleteLater();
        if (m_request != request || request->completed)
            return;
        if (prepared.path.isEmpty()) {
            finish(request, {Status::Failed, QCoreApplication::translate(
                                                 "ScreenshotAirDropService",
                                                 "The image could not be prepared for AirDrop: %1")
                                                 .arg(prepared.error)});
            return;
        }
        request->directory = std::move(prepared.directory);
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
            m_backend(request->owner, prepared.path, completed);
        else
            completed({Status::Unavailable,
                       QCoreApplication::translate("ScreenshotAirDropService",
                                                   "AirDrop is unavailable on this Mac")});
    });
    watcher->setFuture(QtConcurrent::run(snow_shot::runtime::trackRuntimeWork(
        [request, snapshot = std::move(snapshot), filename = std::move(filename)]() mutable {
            Prepared result;
            if (request->cancelled.load(std::memory_order_acquire))
                return result;
            result.directory = std::make_shared<QTemporaryDir>(
                QDir::tempPath() + QStringLiteral("/snow-shot-airdrop-XXXXXX"));
            if (!result.directory->isValid()) {
                result.error = result.directory->errorString();
                return result;
            }
            const QString path = result.directory->filePath(filename);
            // Preserve alpha and color-space metadata; printing's white flattening
            // is deliberately not part of sharing a screenshot.
            snapshot.setDevicePixelRatio(1.0);
            QImageWriter writer(path, "png");
            if (!writer.write(snapshot)) {
                result.error = writer.errorString();
                return result;
            }
            if (!request->cancelled.load(std::memory_order_acquire))
                result.path = path;
            return result;
        })));
    return true;
}
