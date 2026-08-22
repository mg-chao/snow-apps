#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SETTINGSPAGEWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SETTINGSPAGEWIDGET_H

#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include <QWidget>

#include <memory>

class QEvent;

namespace snow_shot::presentation::settings {
class SettingsRuntimeBindings;
}

class SettingsPageWidget final : public QWidget {
    Q_OBJECT

  public:
    SettingsPageWidget(
        const snow_shot::presentation::settings::SettingsCatalog& catalog,
        const QString& pageId,
        snow_shot::presentation::settings::SettingsRuntimeBindings& runtimeBindings,
        QWidget* parent = nullptr);
    ~SettingsPageWidget() override;

    [[nodiscard]] QString pageId() const;
    void reveal(const snow_shot::presentation::settings::SettingsLocation& location);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void retranslateUi();

  signals:
    void commandRequested(
        const snow_shot::presentation::settings::SettingsCommand& command);
    void visibleSectionChanged(const QString& sectionId);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_SETTINGSPAGEWIDGET_H
