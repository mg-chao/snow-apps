#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_GLOBALMOUSEROW_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_GLOBALMOUSEROW_H

#include "snow_shot/presentation/components/shortcutconfigurationbutton.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QEvent;
class QMouseEvent;
namespace adqt::widgets {
class AdFormItem;
class AdModal;
class AdSelect;
} // namespace adqt::widgets
namespace snow_shot::presentation::settings {
class SettingsRuntimeSession;
}

class GlobalMouseRow final : public adqt::widgets::AdButton {
    Q_OBJECT

  public:
    GlobalMouseRow(const QString& title,
                   snow_shot::presentation::settings::SettingsGlobalMouseAction action,
                   snow_shot::presentation::settings::SettingsRuntimeSession& runtimeSession,
                   const snow_shot::presentation::styles::ThemeColorScheme& colorScheme,
                   QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setCombination(
        const snow_shot::presentation::settings::SettingsGlobalMouseCombination& combination);
    [[nodiscard]] ShortcutConfigurationButton* configurationButton() const;
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void retranslateUi();

  signals:
    void dragRequested(snow_shot::presentation::settings::SettingsGlobalMouseAction action);

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    void openConfigurationDialog();
    void syncButton();
    void syncModalText();
    void syncModalValidation();
    void syncTitleColor();
    [[nodiscard]] snow_shot::presentation::settings::SettingsGlobalMouseCombination
    modalCombination() const;
    [[nodiscard]] QString activationKeyLabel(const QString& value) const;
    [[nodiscard]] QString mouseButtonLabel(const QString& value) const;

    QString m_title;
    snow_shot::presentation::settings::SettingsGlobalMouseAction m_action;
    snow_shot::presentation::settings::SettingsRuntimeSession& m_runtimeSession;
    snow_shot::presentation::settings::SettingsGlobalMouseCombination m_combination;
    snow_shot::presentation::styles::ThemeColorScheme m_colorScheme;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_titleIcon = nullptr;
    ShortcutConfigurationButton* m_button = nullptr;
    QPointer<adqt::widgets::AdModal> m_modal;
    QPointer<adqt::widgets::AdFormItem> m_activationField;
    QPointer<adqt::widgets::AdFormItem> m_mouseButtonField;
    QPointer<QLabel> m_validationLabel;
    QPointer<adqt::widgets::AdSelect> m_activationSelect;
    QPointer<adqt::widgets::AdSelect> m_mouseButtonSelect;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_GLOBALMOUSEROW_H
