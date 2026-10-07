#ifndef SNOW_SHOT_PLATFORM_MACOS_SYSTEMNOTIFICATIONSERVICE_H
#define SNOW_SHOT_PLATFORM_MACOS_SYSTEMNOTIFICATIONSERVICE_H

#include <QObject>
#include <QString>
#include <memory>
#include "snow_shot/platform/systemnotification.h"

#ifdef __OBJC__
@protocol SnowShotNotificationCenter;
#endif

namespace snow_shot::platform::macos {

class SystemNotificationService final : public QObject {
    Q_OBJECT
  public:
    using Action = SystemNotificationAction;
    using Result = SystemNotificationResult;
    using Completion = SystemNotificationCompletion;

    explicit SystemNotificationService(QObject* parent = nullptr);
#ifdef __OBJC__
    // Inject native operations without requesting permission or delivering system alerts in tests.
    SystemNotificationService(id<SnowShotNotificationCenter> center, QObject* parent);
#endif
    ~SystemNotificationService() override;
    void show(SystemNotificationRequest request, Completion completed = {});

  signals:
    void activated(snow_shot::platform::SystemNotificationAction action, const QString& filePath);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::platform::macos
#endif
