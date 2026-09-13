#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>

#include "snow_shot/platform/autostartregistration.h"
#include "autostartregistration_p.h"

#include <QCoreApplication>
#include <QGuiApplication>

namespace snow_shot::platform {
namespace {
using macos::detail::LoginItemApi;
using macos::detail::LoginItemStatus;

bool installedApplication() {
    return QGuiApplication::platformName() == QStringLiteral("cocoa") &&
           !QCoreApplication::applicationName().contains(QStringLiteral("-e2e-")) &&
           [NSBundle.mainBundle.bundleURL.pathExtension isEqualToString:@"app"];
}

LoginItemApi nativeApi() {
    return {[] {
                if (!installedApplication())
                    return LoginItemStatus::Unavailable;
                switch (SMAppService.mainAppService.status) {
                case SMAppServiceStatusEnabled:
                    return LoginItemStatus::Enabled;
                case SMAppServiceStatusRequiresApproval:
                    return LoginItemStatus::RequiresApproval;
                case SMAppServiceStatusNotRegistered:
                    return LoginItemStatus::Unregistered;
                case SMAppServiceStatusNotFound:
                    return LoginItemStatus::Unavailable;
                }
                return LoginItemStatus::Unavailable;
            },
            [](bool enabled, QString* error) {
                NSError* nativeError = nil;
                const bool accepted =
                    enabled ? [SMAppService.mainAppService registerAndReturnError:&nativeError]
                            : [SMAppService.mainAppService unregisterAndReturnError:&nativeError];
                if (!accepted && error != nullptr) {
                    *error = QString::fromNSString(nativeError.localizedDescription);
                }
                return accepted;
            },
            [] { [SMAppService openSystemSettingsLoginItems]; }};
}
} // namespace

bool AutoStartRegistration::isSupported() {
    return true;
}

AutoStartRegistrationSnapshot AutoStartRegistration::snapshot() {
    const auto status = nativeApi().status();
    AutoStartRegistrationSnapshot result;
    result.valid = status != LoginItemStatus::Unavailable;
    result.exists =
        status == LoginItemStatus::Enabled || status == LoginItemStatus::RequiresApproval;
    result.nativeType = static_cast<quint32>(status);
    if (!result.valid)
        result.error = QStringLiteral("Login items require an installed application bundle");
    return result;
}

bool AutoStartRegistration::matchesExpectedCommand() {
    return nativeApi().status() == LoginItemStatus::Enabled;
}

bool AutoStartRegistration::setEnabled(bool enabled, QString* error) {
    return macos::detail::setLoginItemEnabled(nativeApi(), enabled, error);
}

bool AutoStartRegistration::restore(const AutoStartRegistrationSnapshot& previous, QString* error) {
    if (!previous.valid)
        return false;
    return setEnabled(previous.exists, error);
}
} // namespace snow_shot::platform
