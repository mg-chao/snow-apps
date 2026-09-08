#ifndef SNOW_SHOT_PRESENTATION_GLOBALMOUSETYPES_H
#define SNOW_SHOT_PRESENTATION_GLOBALMOUSETYPES_H

#include <QMetaType>
#include <QString>
#include <QStringList>

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

Q_DECLARE_METATYPE(snow_shot::presentation::settings::SettingsGlobalMouseCombination)
#endif
