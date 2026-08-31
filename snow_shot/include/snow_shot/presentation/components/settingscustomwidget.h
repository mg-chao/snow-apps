#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SETTINGSCUSTOMWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SETTINGSCUSTOMWIDGET_H

#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include <QWidget>

namespace snow_shot::presentation::settings {
class SettingsRegistry;
class SettingsRuntimeSession;
}

class SettingsCustomWidget : public QWidget {
  public:
    explicit SettingsCustomWidget(QWidget* parent = nullptr) : QWidget(parent) {}
    ~SettingsCustomWidget() override = default;

    virtual void applyTheme(
        const snow_shot::presentation::styles::ThemeColorScheme& scheme) = 0;
    virtual void retranslateUi() = 0;
};

[[nodiscard]] SettingsCustomWidget* createSettingsCustomWidget(
    snow_shot::presentation::settings::SettingsCustomRenderer renderer,
    const snow_shot::presentation::settings::SettingsRegistry& registry,
    const snow_shot::presentation::settings::SettingsItemDefinition& definition,
    snow_shot::presentation::settings::SettingsRuntimeSession& runtimeSession,
    QWidget* parent = nullptr);

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_SETTINGSCUSTOMWIDGET_H
