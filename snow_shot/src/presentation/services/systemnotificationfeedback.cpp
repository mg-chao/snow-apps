#include "snow_shot/presentation/systemnotificationfeedback.h"
#include "widgets/message.h"

#include <utility>

namespace snow_shot::presentation {
adqt::widgets::AdMessageHandle*
presentSystemNotificationResult(const platform::SystemNotificationRequest& request,
                                const platform::SystemNotificationResult& result,
                                const std::function<QWidget*()>& showFallbackWindow) {
    if (result.accepted() || !showFallbackWindow)
        return nullptr;
    QWidget* window = showFallbackWindow();
    if (!window)
        return nullptr;
    adqt::widgets::AdMessage::Request feedback;
    feedback.content = request.title.isEmpty() ? request.body
                       : request.body.isEmpty()
                           ? request.title
                           : request.title + QStringLiteral("\n") + request.body;
    switch (request.severity) {
    case platform::SystemNotificationSeverity::Information:
        return adqt::widgets::AdMessageService::info(std::move(feedback), window);
    case platform::SystemNotificationSeverity::Warning:
        return adqt::widgets::AdMessageService::warning(std::move(feedback), window);
    case platform::SystemNotificationSeverity::Error:
        return adqt::widgets::AdMessageService::error(std::move(feedback), window);
    }
    return nullptr;
}
} // namespace snow_shot::presentation
