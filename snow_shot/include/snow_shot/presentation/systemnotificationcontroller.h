#ifndef SNOW_SHOT_PRESENTATION_SYSTEMNOTIFICATIONCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SYSTEMNOTIFICATIONCONTROLLER_H

#include "snow_shot/platform/systemnotification.h"
#include <QObject>

namespace snow_shot::presentation {
class SystemNotificationController final : public QObject {
    Q_OBJECT
  public:
    explicit SystemNotificationController(platform::SystemNotificationDelivery delivery,
                                          QObject* parent = nullptr);
    void show(platform::SystemNotificationRequest request);

  signals:
    void deliveryFinished(const snow_shot::platform::SystemNotificationRequest& request,
                          const snow_shot::platform::SystemNotificationResult& result);

  private:
    platform::SystemNotificationDelivery m_delivery;
};
} // namespace snow_shot::presentation
#endif
