#ifndef SNOW_SHOT_PLATFORM_SYSTEMNOTIFICATION_H
#define SNOW_SHOT_PLATFORM_SYSTEMNOTIFICATION_H

#include <QMetaType>
#include <QString>
#include <functional>

namespace snow_shot::platform {
enum class SystemNotificationAction { None, OpenAbout, OpenRecording };
enum class SystemNotificationSeverity { Information, Warning, Error };

struct SystemNotificationRequest {
    QString title;
    QString body;
    SystemNotificationSeverity severity = SystemNotificationSeverity::Information;
    SystemNotificationAction action = SystemNotificationAction::None;
    QString filePath = {};
};

struct SystemNotificationResult {
    // Accepted means the submission API accepted the request, not that it displayed a banner.
    // QSystemTrayIcon can only confirm dispatch, without a native delivery acknowledgement.
    // Focus, sharing and the user's presentation preferences remain authoritative.
    enum class Status { Accepted, Unavailable, Denied, Failed };
    Status status = Status::Accepted;
    QString diagnostic;
    [[nodiscard]] bool accepted() const {
        return status == Status::Accepted;
    }
};

using SystemNotificationCompletion = std::function<void(SystemNotificationResult)>;
using SystemNotificationDelivery =
    std::function<void(const SystemNotificationRequest&, SystemNotificationCompletion)>;
} // namespace snow_shot::platform

Q_DECLARE_METATYPE(snow_shot::platform::SystemNotificationAction)
Q_DECLARE_METATYPE(snow_shot::platform::SystemNotificationRequest)
Q_DECLARE_METATYPE(snow_shot::platform::SystemNotificationResult)
#endif
