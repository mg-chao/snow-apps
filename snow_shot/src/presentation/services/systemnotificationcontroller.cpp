#include "snow_shot/presentation/systemnotificationcontroller.h"

#include <QPointer>
#include <utility>

namespace snow_shot::presentation {
SystemNotificationController::SystemNotificationController(
    platform::SystemNotificationDelivery delivery, QObject* parent)
    : QObject(parent), m_delivery(std::move(delivery)) {}

void SystemNotificationController::show(platform::SystemNotificationRequest request) {
    const QPointer<SystemNotificationController> guard(this);
    const auto completed = [guard, request](platform::SystemNotificationResult result) {
        if (!guard)
            return;
        // Backends may finish on any thread. Queue even synchronous failures so callers
        // can finish their operation before feedback activates an application window.
        QMetaObject::invokeMethod(
            guard,
            [guard, request, result = std::move(result)] {
                if (guard)
                    emit guard->deliveryFinished(request, result);
            },
            Qt::QueuedConnection);
    };
    if (m_delivery)
        m_delivery(request, completed);
    else
        completed({platform::SystemNotificationResult::Status::Unavailable,
                   QStringLiteral("No system notification backend is available.")});
}
} // namespace snow_shot::presentation
