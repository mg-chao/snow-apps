#include "snow_shot/presentation/screenshotprintservice.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "nativeprintbackend.h"
#include "screenshotprintdiagnostics.h"

#include <QApplication>
#include <QCoreApplication>
#include <QColorSpace>
#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QPainter>
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <QWidget>
#include <QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

using snow_shot::print_detail::logPrintEvent;
using snow_shot::print_detail::printStatusName;

struct ScreenshotPrintService::Request {
    snow_shot::runtime::RuntimeActivityLease activity =
        snow_shot::runtime::RuntimeActivityTracker::shared().acquire();
    QString operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QElapsedTimer timer;
    QPointer<QObject> receiver;
    QPointer<QWidget> owner;
    QImage image;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    Completion completion;
    bool legacy = false;
    bool completed = false;
    QMetaObject::Connection ownerDestroyed;
    QMetaObject::Connection receiverDestroyed;

    QJsonObject fields() const {
        return {{QStringLiteral("operation"), operation},
                {QStringLiteral("duration_ms"), timer.elapsed()},
                {QStringLiteral("backend"),
                 legacy ? QStringLiteral("legacy") : QStringLiteral("primary")}};
    }

    ~Request() {
        cancelled->store(true, std::memory_order_release);
        QObject::disconnect(ownerDestroyed);
        QObject::disconnect(receiverDestroyed);
    }
};

ScreenshotPrintService::ScreenshotPrintService(Backend primary, Backend legacy, QObject* parent)
    : QObject(parent), m_primary(std::move(primary)), m_legacy(std::move(legacy)) {}

ScreenshotPrintService& ScreenshotPrintService::shared() {
    static QPointer<ScreenshotPrintService> service;
    if (!service) {
        service = new ScreenshotPrintService(screenshotNativePrintBackend(false),
                                             screenshotNativePrintBackend(true), qApp);
    }
    return *service;
}

bool ScreenshotPrintService::busy() const {
    return m_request != nullptr;
}

QImage ScreenshotPrintService::opaqueImage(const QImage& image) {
    if (image.isNull())
        return {};
    QImage result(image.size(), QImage::Format_RGB32);
    if (result.isNull())
        return {};
    result.fill(Qt::white);
    QPainter painter(&result);
    auto source = image;
    if (source.colorSpace().isValid())
        source.convertToColorSpace(QColorSpace::SRgb);
    source.setDevicePixelRatio(1.0);
    painter.drawImage(QPoint(), source);
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}

QRectF ScreenshotPrintService::fittedRect(QSize imageSize, const QRectF& printableRect) {
    if (imageSize.isEmpty() || printableRect.isEmpty() || !std::isfinite(printableRect.width()) ||
        !std::isfinite(printableRect.height()) || !std::isfinite(printableRect.x()) ||
        !std::isfinite(printableRect.y()))
        return {};
    const qreal scale = std::min(printableRect.width() / imageSize.width(),
                                 printableRect.height() / imageSize.height());
    const QSizeF size = QSizeF(imageSize) * scale;
    return {printableRect.center() - QPointF(size.width() / 2, size.height() / 2), size};
}

bool ScreenshotPrintService::printImage(QObject* receiver, QWidget* owner, QImage snapshot,
                                        Completion completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (busy() || !receiver || !owner || snapshot.isNull() || !completion) {
        const auto reason = busy()              ? QStringLiteral("busy")
                            : !receiver         ? QStringLiteral("missing_receiver")
                            : !owner            ? QStringLiteral("missing_owner")
                            : snapshot.isNull() ? QStringLiteral("empty_image")
                                                : QStringLiteral("missing_completion");
        logPrintEvent("print.rejected", {{QStringLiteral("reason"), reason}},
                      busy() ? QtInfoMsg : QtWarningMsg);
        return false;
    }
    m_request = std::make_shared<Request>();
    m_request->timer.start();
    m_request->receiver = receiver;
    m_request->owner = owner;
    m_request->completion = std::move(completion);
    const auto request = m_request;
    auto fields = request->fields();
    fields.insert(QStringLiteral("width"), snapshot.width());
    fields.insert(QStringLiteral("height"), snapshot.height());
    fields.insert(QStringLiteral("pixel_format"), static_cast<int>(snapshot.format()));
    fields.insert(QStringLiteral("dpr"), snapshot.devicePixelRatio());
    logPrintEvent("print.accepted", fields);
    const auto abandon = [this, request](const char* reason) {
        if (m_request != request || request->completed)
            return;
        request->completed = true;
        const auto activity = std::move(request->activity);
        auto fields = request->fields();
        fields.insert(QStringLiteral("status"), printStatusName(Status::Cancelled));
        fields.insert(QStringLiteral("reason"), QString::fromLatin1(reason));
        logPrintEvent("print.completed", fields);
        request->cancelled->store(true, std::memory_order_release);
        m_request.reset();
        QObject::disconnect(request->ownerDestroyed);
        QObject::disconnect(request->receiverDestroyed);
        if (request->receiver)
            request->completion({Status::Cancelled, {}});
    };
    request->ownerDestroyed =
        connect(owner, &QObject::destroyed, this, [abandon] { abandon("owner_destroyed"); });
    if (receiver != owner)
        request->receiverDestroyed = connect(receiver, &QObject::destroyed, this,
                                             [abandon] { abandon("receiver_destroyed"); });
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, request, watcher] {
        const QImage prepared = watcher->result();
        watcher->deleteLater();
        if (m_request != request || request->completed)
            return;
        request->image = prepared;
        auto fields = request->fields();
        fields.insert(QStringLiteral("width"), prepared.width());
        fields.insert(QStringLiteral("height"), prepared.height());
        logPrintEvent(prepared.isNull() ? "print.preparation_failed" : "print.prepared", fields,
                      prepared.isNull() ? QtWarningMsg : QtInfoMsg);
        startBackend(request, false);
    });
    watcher->setFuture(QtConcurrent::run(snow_shot::runtime::trackRuntimeWork(
        [snapshot = std::move(snapshot), cancelled = request->cancelled] {
            if (cancelled->load(std::memory_order_acquire))
                return QImage{};
            QImage prepared = opaqueImage(snapshot);
            return cancelled->load(std::memory_order_acquire) ? QImage{} : prepared;
        })));
    return true;
}

void ScreenshotPrintService::startBackend(const std::shared_ptr<Request>& request, bool legacy) {
    if (m_request != request || request->completed)
        return;
    if (!request->receiver || !request->owner) {
        auto fields = request->fields();
        fields.insert(QStringLiteral("status"), printStatusName(Status::Cancelled));
        fields.insert(QStringLiteral("reason"), QStringLiteral("target_destroyed"));
        logPrintEvent("print.completed", fields);
        request->completed = true;
        const auto activity = std::move(request->activity);
        m_request.reset();
        return;
    }
    request->legacy = legacy;
    const QPointer<ScreenshotPrintService> guard(this);
    const auto finished = [guard, request, legacy](Result result) {
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, request, legacy, result = std::move(result)]() mutable {
                if (!guard || guard->m_request != request || request->completed ||
                    request->legacy != legacy)
                    return;
                if (result.status == Status::Unavailable && !legacy && guard->m_legacy &&
                    request->receiver && request->owner) {
                    auto fields = request->fields();
                    fields.insert(QStringLiteral("status"), printStatusName(result.status));
                    logPrintEvent("print.fallback", fields, QtWarningMsg);
                    guard->startBackend(request, true);
                    return;
                }
                if (result.status == Status::Unavailable) {
                    result.status = Status::Failed;
                    if (result.error.isEmpty())
                        result.error = QCoreApplication::translate(
                            "ScreenshotPrintService", "The native print interface is unavailable");
                }
                request->completed = true;
                const auto activity = std::move(request->activity);
                auto fields = request->fields();
                fields.insert(QStringLiteral("status"), printStatusName(result.status));
                logPrintEvent("print.completed", fields,
                              result.status == Status::Failed ? QtWarningMsg : QtInfoMsg);
                guard->m_request.reset();
                QObject::disconnect(request->ownerDestroyed);
                QObject::disconnect(request->receiverDestroyed);
                if (request->receiver)
                    request->completion(std::move(result));
            },
            Qt::QueuedConnection);
    };
    const auto& backend = legacy ? m_legacy : m_primary;
    if (request->image.isNull()) {
        finished({Status::Failed,
                  QCoreApplication::translate("ScreenshotPrintService",
                                              "The image could not be prepared for printing")});
    } else if (backend) {
        logPrintEvent("print.backend_started", request->fields());
        backend(request->owner, request->image, finished);
    } else {
        logPrintEvent("print.backend_unavailable", request->fields(), QtWarningMsg);
        finished({Status::Unavailable, {}});
    }
}
