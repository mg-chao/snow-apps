#include "snow_shot/presentation/apppermissionservice.h"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <AVFoundation/AVFoundation.h>
#include <QDesktopServices>
#include <QUrl>

namespace snow_shot::presentation {
namespace {
class MacAppPermissionBackend final : public AppPermissionBackend {
  public:
    AppPermissionSnapshot query() override {
        using S = AppPermissionStatus;
        AppPermissionSnapshot result;
        result.statuses = {CGPreflightScreenCaptureAccess() ? S::Granted : S::Missing,
                           AXIsProcessTrusted() ? S::Granted : S::Missing,
                           CGPreflightListenEventAccess() ? S::Granted : S::Missing, S::Error};
        switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusNotDetermined:
            result.statuses[3] = S::NotDetermined;
            break;
        case AVAuthorizationStatusRestricted:
            result.statuses[3] = S::Restricted;
            break;
        case AVAuthorizationStatusDenied:
            result.statuses[3] = S::Denied;
            break;
        case AVAuthorizationStatusAuthorized:
            result.statuses[3] = S::Granted;
            break;
        }
        if (![NSBundle.mainBundle objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"])
            result.statuses[3] = S::Error;
        return result;
    }
    void request(AppPermission permission, std::function<void()> completion) override {
        switch (permission) {
        case AppPermission::ScreenRecording:
            static_cast<void>(CGRequestScreenCaptureAccess());
            break;
        case AppPermission::InputMonitoring:
            static_cast<void>(CGRequestListenEventAccess());
            break;
        case AppPermission::Accessibility: {
            const void* keys[] = {kAXTrustedCheckOptionPrompt};
            const void* values[] = {kCFBooleanTrue};
            CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                         &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
            static_cast<void>(AXIsProcessTrustedWithOptions(options));
            CFRelease(options);
            break;
        }
        case AppPermission::Microphone:
            // AVFoundation terminates a host that requests access without usage metadata.
            if (![NSBundle.mainBundle objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"]) {
                completion();
                return;
            }
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL) {
                                       completion();
                                     }];
            return;
        }
        completion();
    }
    bool openSettings(AppPermission permission) override {
        QString pane;
        switch (permission) {
        case AppPermission::ScreenRecording:
            pane = QStringLiteral("Privacy_ScreenCapture");
            break;
        case AppPermission::Accessibility:
            pane = QStringLiteral("Privacy_Accessibility");
            break;
        case AppPermission::InputMonitoring:
            pane = QStringLiteral("Privacy_ListenEvent");
            break;
        case AppPermission::Microphone:
            pane = QStringLiteral("Privacy_Microphone");
            break;
        }
        return QDesktopServices::openUrl(QUrl(
            QStringLiteral("x-apple.systempreferences:com.apple.preference.security?") + pane));
    }
};
} // namespace
std::unique_ptr<AppPermissionBackend> createAppPermissionBackend() {
    return std::make_unique<MacAppPermissionBackend>();
}
} // namespace snow_shot::presentation
