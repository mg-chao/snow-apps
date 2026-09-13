#pragma once

#include <QString>
#include <functional>

namespace snow_shot::platform::macos::detail {
enum class LoginItemStatus { Unregistered, Enabled, RequiresApproval, Unavailable };
struct LoginItemApi {
    std::function<LoginItemStatus()> status;
    std::function<bool(bool, QString*)> changeRegistration;
    std::function<void()> openSettings;
};

inline bool setLoginItemEnabled(const LoginItemApi& api, bool enabled, QString* error) {
    const auto status = api.status();
    if (status == LoginItemStatus::Unavailable) {
        if (error != nullptr) {
            *error = QStringLiteral("Login items require an installed application bundle");
        }
        return false;
    }
    if ((!enabled && status == LoginItemStatus::Unregistered) ||
        (enabled && status == LoginItemStatus::Enabled)) {
        return true;
    }
    if (enabled && status == LoginItemStatus::RequiresApproval) {
        api.openSettings();
        return true;
    }
    if (!api.changeRegistration(enabled, error)) {
        return false;
    }
    const auto result = api.status();
    if (enabled && result == LoginItemStatus::RequiresApproval) {
        api.openSettings();
        return true;
    }
    const bool applied =
        enabled ? result == LoginItemStatus::Enabled : result == LoginItemStatus::Unregistered;
    if (!applied && error != nullptr) {
        *error = QStringLiteral("The login item registration did not reach the requested state");
    }
    return applied;
}
} // namespace snow_shot::platform::macos::detail
