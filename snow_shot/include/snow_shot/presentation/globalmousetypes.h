#ifndef SNOW_SHOT_PRESENTATION_GLOBALMOUSETYPES_H
#define SNOW_SHOT_PRESENTATION_GLOBALMOUSETYPES_H

#include <QMetaType>
#include <QFlags>
#include <optional>
#include <QString>
#include <QStringList>

namespace snow_shot::presentation {
// Physical modifier identities. These deliberately do not use Qt's macOS
// Control/Meta swapping, which is appropriate for shortcuts, not mouse chords.
enum class GlobalMouseCoordinateSpace { PhysicalPixels, DesktopPoints };

enum class GlobalMouseModifier { Control = 1, Alt = 2, Shift = 4, Super = 8, Command = 16 };
Q_DECLARE_FLAGS(GlobalMouseModifiers, GlobalMouseModifier)
Q_DECLARE_OPERATORS_FOR_FLAGS(GlobalMouseModifiers)

inline QStringList globalMouseActivationKeys() {
#ifdef Q_OS_MACOS
    return {QStringLiteral("command"), QStringLiteral("control"), QStringLiteral("option"),
            QStringLiteral("shift")};
#else
    return {QStringLiteral("windows"), QStringLiteral("ctrl"), QStringLiteral("alt"),
            QStringLiteral("shift")};
#endif
}
inline std::optional<GlobalMouseModifier> globalMouseModifier(const QString& key) {
#ifdef Q_OS_MACOS
    if (key == u"command")
        return GlobalMouseModifier::Command;
    if (key == u"control")
        return GlobalMouseModifier::Control;
    if (key == u"option")
        return GlobalMouseModifier::Alt;
#else
    if (key == u"windows")
        return GlobalMouseModifier::Super;
    if (key == u"ctrl")
        return GlobalMouseModifier::Control;
    if (key == u"alt")
        return GlobalMouseModifier::Alt;
#endif
    if (key == u"shift")
        return GlobalMouseModifier::Shift;
    return std::nullopt;
}
} // namespace snow_shot::presentation

namespace snow_shot::presentation::settings {
enum class SettingsGlobalMouseAction {
    ScreenshotCopy,
    ScreenshotFixed,
    ScreenshotOcr,
    ScreenshotTranslation,
    ScreenshotQuickSave,
    ScreenshotSave,
    ScreenRecording,
};

struct SettingsGlobalMouseCombination {
    QStringList activationKeys;
    QString mouseButton;

    [[nodiscard]] bool isUnset() const {
        return activationKeys.isEmpty() && mouseButton.isEmpty();
    }
    [[nodiscard]] QStringList sortedActivationKeys() const {
        QStringList keys = activationKeys;
        keys.removeDuplicates();
        keys.sort();
        return keys;
    }
    friend bool operator==(const SettingsGlobalMouseCombination& first,
                           const SettingsGlobalMouseCombination& second) {
        return first.mouseButton == second.mouseButton &&
               first.sortedActivationKeys() == second.sortedActivationKeys();
    }
};
} // namespace snow_shot::presentation::settings

namespace snow_shot::presentation {
struct GlobalMousePermissionState {
    enum class Status {
        Unknown,
        Ready,
        ListenRequired,
        AccessibilityRequired,
        Unavailable,
        Suspended
    };
    Status status = Status::Unknown;
    bool listenGranted = false;
    bool accessibilityGranted = false;
    bool tapAvailable = false;
    friend bool operator==(const GlobalMousePermissionState&,
                           const GlobalMousePermissionState&) = default;
};
} // namespace snow_shot::presentation

Q_DECLARE_METATYPE(snow_shot::presentation::settings::SettingsGlobalMouseCombination)
Q_DECLARE_METATYPE(snow_shot::presentation::GlobalMousePermissionState)
#endif
