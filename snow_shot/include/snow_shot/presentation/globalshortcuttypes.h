#ifndef SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTTYPES_H
#define SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTTYPES_H

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
    OpenCaptureHistory,
    OpenSettings,
    PinClipboardContent,
};

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
    quint32 nativeErrorCode = 0;
};

struct GlobalShortcutRegistrationState {
    GlobalShortcutAction action = GlobalShortcutAction::Screenshot;
    QStringList shortcuts;
    GlobalShortcutStatus status = GlobalShortcutStatus::Unset;
    QVector<GlobalShortcutBindingResult> bindings;
};

struct GlobalShortcutBackendResult {
    bool registered = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::None;
    quint32 nativeErrorCode = 0;
};

struct GlobalShortcutValidationResult {
    QString shortcut;
    bool supported = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::InvalidShortcut;
};
} // namespace snow_shot::presentation

Q_DECLARE_METATYPE(snow_shot::presentation::GlobalShortcutAction)
Q_DECLARE_METATYPE(snow_shot::presentation::GlobalShortcutRegistrationState)

#endif // SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTTYPES_H
