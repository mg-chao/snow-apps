#ifndef SNOW_SHOT_PLATFORM_APPLICATIONQOS_H
#define SNOW_SHOT_PLATFORM_APPLICATIONQOS_H

#include <QString>
#include <QThread>

#include <optional>

namespace snow_shot::platform {

enum class ApplicationQoS : unsigned int {
    Responsive = 0,
    UserInitiated = 1,
    Default = 2,
    Utility = 3,
    Background = 4,
};

[[nodiscard]] inline std::optional<ApplicationQoS> applicationQoSForValue(const QString& value) {
    if (value == QStringLiteral("user_interactive"))
        return ApplicationQoS::Responsive;
    if (value == QStringLiteral("user_initiated"))
        return ApplicationQoS::UserInitiated;
    if (value == QStringLiteral("default"))
        return ApplicationQoS::Default;
    if (value == QStringLiteral("utility"))
        return ApplicationQoS::Utility;
    if (value == QStringLiteral("background"))
        return ApplicationQoS::Background;
    return std::nullopt;
}

#ifdef Q_OS_MACOS
extern "C" int snow_application_qos_initialize(unsigned int level, void (*reportError)(int));
extern "C" int snow_application_qos_apply_current_thread();
extern "C" unsigned int snow_application_qos_active();

void initializeApplicationQoS(ApplicationQoS qos);
#endif

inline void applyApplicationQoSToCurrentThread() {
#ifdef Q_OS_MACOS
    // The shared policy reports native failures through the diagnostics callback.
    static_cast<void>(snow_application_qos_apply_current_thread());
#endif
}

inline void configureApplicationQoSThread(QThread* thread) {
#ifdef Q_OS_MACOS
    QObject::connect(
        thread, &QThread::started, thread, [] { applyApplicationQoSToCurrentThread(); },
        Qt::DirectConnection);
#else
    Q_UNUSED(thread);
#endif
}

} // namespace snow_shot::platform

#endif
