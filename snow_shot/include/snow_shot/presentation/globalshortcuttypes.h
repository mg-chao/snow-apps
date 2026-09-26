#ifndef SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTTYPES_H
#define SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTTYPES_H

#include "snow_shot/shortcuts/shortcutbinding.h"

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace snow_shot::presentation {
enum class GlobalShortcutAction {
    Screenshot,
    ScreenshotDelay,
    ScreenshotFixed,
    ScreenshotOcr,
    ScreenshotTranslation,
    ScreenshotCopy,
    ScreenshotFullScreen,
    ScreenshotFocusedWindow,
    ScreenRecord,
    ScreenRecordCopy,
    OpenScreenRecordingFolder,
    OpenCaptureHistory,
    OpenSettings,
    PinClipboardContent,
    TranslateSelectedText,
    PinSelectedFiles,
    RestoreLastClosedWindows,
    ToggleGlobalHotkeys,
    ToggleDisableOnFocusedFullscreenWindow,
    OpenPinToScreenManagement,
    FullscreenCanvas,
};

[[nodiscard]] constexpr bool controlsGlobalHotkeyGates(GlobalShortcutAction action) {
    return action == GlobalShortcutAction::ToggleGlobalHotkeys ||
           action == GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow;
}

enum class GlobalShortcutStatus {
    Unset,
    Registered,
    PartiallyRegistered,
    Failed,
};

enum class GlobalShortcutFailureReason {
    None,
    InvalidShortcut,
    AlreadyInUse,
    UnsupportedPlatform,
    SystemError,
};

struct GlobalShortcutBindingResult {
    QString shortcut;
    bool registered = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::None;
    qint64 nativeErrorCode = 0;
    snow_shot::shortcuts::ShortcutBinding binding;
};

struct GlobalShortcutRegistrationState {
    GlobalShortcutAction action = GlobalShortcutAction::Screenshot;
    snow_shot::shortcuts::ShortcutBindingList shortcuts;
    GlobalShortcutStatus status = GlobalShortcutStatus::Unset;
    QVector<GlobalShortcutBindingResult> bindings;
};

struct GlobalShortcutBackendResult {
    bool registered = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::None;
    qint64 nativeErrorCode = 0;
};

struct GlobalShortcutValidationResult {
    QString shortcut;
    bool supported = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::InvalidShortcut;
    snow_shot::shortcuts::ShortcutBinding binding;
};
} // namespace snow_shot::presentation

Q_DECLARE_METATYPE(snow_shot::presentation::GlobalShortcutAction)
Q_DECLARE_METATYPE(snow_shot::presentation::GlobalShortcutRegistrationState)

#endif // SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTTYPES_H
