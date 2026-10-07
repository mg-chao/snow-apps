#ifndef SNOW_SHOT_PRESENTATION_SYSTEMNOTIFICATIONFEEDBACK_H
#define SNOW_SHOT_PRESENTATION_SYSTEMNOTIFICATIONFEEDBACK_H

#include "snow_shot/platform/systemnotification.h"

class QWidget;
namespace adqt::widgets {
class AdMessageHandle;
}
namespace snow_shot::presentation {
// The provider creates/activates the application window only for rejected delivery.
adqt::widgets::AdMessageHandle*
presentSystemNotificationResult(const platform::SystemNotificationRequest& request,
                                const platform::SystemNotificationResult& result,
                                const std::function<QWidget*()>& showFallbackWindow);
} // namespace snow_shot::presentation
#endif
