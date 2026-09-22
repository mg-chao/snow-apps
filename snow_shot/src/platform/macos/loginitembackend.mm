#include "snow_shot/platform/macos/loginitemservice.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <Security/Security.h>
#import <ServiceManagement/ServiceManagement.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace snow_shot::platform::macos {
namespace {
QString text(const char* source) {
    return QCoreApplication::translate("LoginItemService", source);
}
bool initialLoginLaunch = false;
id launchObserver = nil;
QString markerPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/SnowShot/macos-login-item.ini");
}
LoginItemSnapshot query() {
    @autoreleasepool {
        NSBundle* bundle = NSBundle.mainBundle;
        const QString path =
            QFileInfo(QString::fromNSString(bundle.bundlePath)).canonicalFilePath();
        const QString applications =
            QFileInfo(QDir::homePath() + QStringLiteral("/Applications")).canonicalFilePath();
        NSNumber* readOnly = nil;
        [bundle.bundleURL getResourceValue:&readOnly forKey:NSURLVolumeIsReadOnlyKey error:nil];
        if (![bundle.bundleIdentifier isEqualToString:@"com.snowshot.snow_shot"] ||
            !loginItemLocationAllowed(path, applications) || readOnly == nil ||
            readOnly.boolValue) {
            return {LoginItemStatus::Unavailable,
                    text(QT_TRANSLATE_NOOP("LoginItemService",
                                           "Move the signed Snow Shot app to /Applications or "
                                           "~/Applications to use launch at login."))};
        }
        SecStaticCodeRef code = nullptr;
        OSStatus signature = SecStaticCodeCreateWithPath(
            reinterpret_cast<CFURLRef>(bundle.bundleURL), kSecCSDefaultFlags, &code);
        if (signature == errSecSuccess) {
            signature = SecStaticCodeCheckValidity(code, kSecCSDefaultFlags, nullptr);
            CFRelease(code);
        }
        if (signature != errSecSuccess)
            return {LoginItemStatus::Unavailable,
                    text(QT_TRANSLATE_NOOP("LoginItemService",
                                           "Snow Shot needs a valid code signature to use launch "
                                           "at login. Reinstall the signed app."))};
        switch (SMAppService.mainAppService.status) {
        case SMAppServiceStatusNotRegistered:
            return {LoginItemStatus::Unregistered, {}};
        case SMAppServiceStatusEnabled:
            return {LoginItemStatus::Enabled, {}};
        case SMAppServiceStatusRequiresApproval:
            return {LoginItemStatus::ApprovalRequired, {}};
        case SMAppServiceStatusNotFound:
            break;
        }
        return {LoginItemStatus::Unavailable,
                text(QT_TRANSLATE_NOOP("LoginItemService",
                                       "macOS could not find Snow Shot's login item. Reinstall the "
                                       "app in Applications."))};
    }
}
} // namespace
LoginItemService& loginItemService() {
    static LoginItemService service(
        {query,
         [](bool enabled) -> LoginItemResult {
             @autoreleasepool {
                 NSError* error = nil;
                 const BOOL success =
                     enabled ? [SMAppService.mainAppService registerAndReturnError:&error]
                             : [SMAppService.mainAppService unregisterAndReturnError:&error];
                 return {static_cast<bool>(success),
                         success ? QString()
                                 : text(QT_TRANSLATE_NOOP("LoginItemService",
                                                          "Could not change launch at login: %1"))
                                       .arg(QString::fromNSString(error.localizedDescription))};
             }
         },
         [] {
             QSettings settings(markerPath(), QSettings::IniFormat);
             return settings.value(QStringLiteral("initialized"), false).toBool();
         },
         [] {
             QSettings settings(markerPath(), QSettings::IniFormat);
             settings.setValue(QStringLiteral("initialized"), true);
             settings.sync();
             return settings.status() == QSettings::NoError;
         },
         [](bool enabled) {
             return storage::SystemSettings().setAutoStartAtBoot(enabled) &&
                    storage::ApplicationStorage::instance().configuration().flushNow().success;
         },
         [] { [SMAppService openSystemSettingsLoginItems]; }});
    return service;
}
void observeNativeLoginItemLaunch() {
    if (launchObserver != nil)
        return;
    initialLoginLaunch = false;
    // Install before QApplication: AppKit may finish launching during Qt setup.
    launchObserver = [NSNotificationCenter.defaultCenter
        addObserverForName:NSApplicationDidFinishLaunchingNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  initialLoginLaunch = isNativeLoginItemLaunch();
                }];
}
bool initialNativeLoginItemLaunch() {
    // Qt initializes AppKit on its first event-loop pass. Capture the launch event
    // while AppKit handles it, before forwarding to an existing instance.
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if (launchObserver != nil) {
        [NSNotificationCenter.defaultCenter removeObserver:launchObserver];
        launchObserver = nil;
    }
    return initialLoginLaunch || isNativeLoginItemLaunch();
}
bool isNativeLoginItemLaunch() {
    NSAppleEventDescriptor* event = NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
    return event.eventID == kAEOpenApplication &&
           [event paramDescriptorForKeyword:keyAEPropData].enumCodeValue ==
               keyAELaunchedAsLogInItem;
}
} // namespace snow_shot::platform::macos
