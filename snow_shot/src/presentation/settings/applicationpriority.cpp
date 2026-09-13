#include "snow_shot/presentation/settings/applicationpriority.h"

#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <cerrno>
#include <sys/resource.h>
#endif

namespace snow_shot::presentation::settings {

QString applicationPriorityValue(ApplicationPriority priority) {
    switch (priority) {
    case ApplicationPriority::Normal:
        return QStringLiteral("normal");
    case ApplicationPriority::AboveNormal:
        return QStringLiteral("above_normal");
    case ApplicationPriority::High:
        return QStringLiteral("high");
    case ApplicationPriority::Realtime:
        return QStringLiteral("real_time");
    }
    return QStringLiteral("normal");
}

std::optional<ApplicationPriority> applicationPriorityForValue(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("normal")) {
        return ApplicationPriority::Normal;
    }
    if (normalized == QStringLiteral("above_normal")) {
        return ApplicationPriority::AboveNormal;
    }
    if (normalized == QStringLiteral("high")) {
        return ApplicationPriority::High;
    }
    if (normalized == QStringLiteral("real_time")) {
        return ApplicationPriority::Realtime;
    }
    return std::nullopt;
}

bool applyApplicationPriority(ApplicationPriority priority) {
#if defined(Q_OS_WIN)
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    switch (priority) {
    case ApplicationPriority::Normal:
        priorityClass = NORMAL_PRIORITY_CLASS;
        break;
    case ApplicationPriority::AboveNormal:
        priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
        break;
    case ApplicationPriority::High:
        priorityClass = HIGH_PRIORITY_CLASS;
        break;
    case ApplicationPriority::Realtime:
        priorityClass = REALTIME_PRIORITY_CLASS;
        break;
    }
    return SetPriorityClass(GetCurrentProcess(), priorityClass) != FALSE;
#else
    int niceValue = 0;
    switch (priority) {
    case ApplicationPriority::Normal:
        niceValue = 0;
        break;
    case ApplicationPriority::AboveNormal:
        niceValue = -5;
        break;
    case ApplicationPriority::High:
        niceValue = -10;
        break;
    case ApplicationPriority::Realtime:
        niceValue = -20;
        break;
    }
    errno = 0;
    return setpriority(PRIO_PROCESS, 0, niceValue) == 0;
#endif
}

bool applyConfiguredApplicationPriority() {
    auto& storage = storage::ApplicationStorage::instance();
    if (!storage.isInitialized()) {
        static_cast<void>(storage.initialize());
    }
    QString value =
        storage.configuration().value(QStringLiteral("system/application_priority")).toString();
    if (value.isEmpty()) {
        value = storage::ConfigurationSchema::defaultValue(
                    QStringLiteral("system/application_priority"))
                    .toString();
    }
    const auto priority = applicationPriorityForValue(value);
    return priority.has_value() && applyApplicationPriority(*priority);
}

} // namespace snow_shot::presentation::settings
