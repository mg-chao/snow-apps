#include "snow_shot/presentation/screenshotprintservice.h"
#include "nativeprintbackend.h"

#include <QApplication>
#include <QCoreApplication>
#include <QColorSpace>
#include <QPainter>
#include <QPointer>
#include <QThread>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <utility>

struct ScreenshotPrintService::Request {
    QPointer<QObject> receiver;
    QPointer<QWidget> owner;
    QImage image;
    Completion completion;
    bool legacy = false;
    bool completed = false;
    QMetaObject::Connection ownerDestroyed;
    QMetaObject::Connection receiverDestroyed;

    ~Request() {
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
    if (busy() || !receiver || !owner || snapshot.isNull() || !completion)
        return false;
    m_request = std::make_shared<Request>();
    m_request->receiver = receiver;
    m_request->owner = owner;
    m_request->image = opaqueImage(snapshot);
    m_request->completion = std::move(completion);
    const auto request = m_request;
    const auto abandon = [this, request] {
        if (m_request != request || request->completed)
            return;
        request->completed = true;
        m_request.reset();
        QObject::disconnect(request->ownerDestroyed);
        QObject::disconnect(request->receiverDestroyed);
        if (request->receiver)
            request->completion({Status::Cancelled, {}});
    };
    request->ownerDestroyed = connect(owner, &QObject::destroyed, this, abandon);
    if (receiver != owner)
        request->receiverDestroyed = connect(receiver, &QObject::destroyed, this, abandon);
    QMetaObject::invokeMethod(
        this, [this, request] { startBackend(request, false); }, Qt::QueuedConnection);
    return true;
}

void ScreenshotPrintService::startBackend(const std::shared_ptr<Request>& request, bool legacy) {
    if (m_request != request || request->completed)
        return;
    if (!request->receiver || !request->owner) {
        request->completed = true;
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
        backend(request->owner, request->image, finished);
    } else {
        finished({Status::Unavailable, {}});
    }
}
