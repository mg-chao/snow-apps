#ifndef SNOW_SHOT_CUSTOMAIMODELSSETTINGSWIDGET_H
#define SNOW_SHOT_CUSTOMAIMODELSSETTINGSWIDGET_H

#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/customaimodelconfiguration.h"
#include <QPointer>
#include <array>

class QVBoxLayout;
class QLabel;
namespace adqt::widgets {
class AdAlert;
class AdButton;
class AdModal;
class AdFormItem;
class AdLineEdit;
class AdSwitch;
} // namespace adqt::widgets

class CustomAiModelsSettingsWidget final : public SettingsCustomWidget {
    Q_OBJECT
  public:
    explicit CustomAiModelsSettingsWidget(
        snow_shot::presentation::settings::SettingsRuntimeSession& session,
        QWidget* parent = nullptr);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) override;
    void retranslateUi() override;

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void rebuild();
    void openEditor(const QString& id = {});
    void copyModel(const QString& id);
    void deleteModel(const QString& id);
    bool save(const snow_shot::CustomAiModels& models);
    void submitEditor(bool saveChanges = true);
    void translateModal();
    snow_shot::presentation::settings::SettingsRuntimeSession& m_session;
    snow_shot::presentation::styles::ThemeColorScheme m_scheme;
    QVBoxLayout* m_rows = nullptr;
    QLabel* m_title = nullptr;
    adqt::widgets::AdButton* m_add = nullptr;
    adqt::widgets::AdAlert* m_error = nullptr;
    QPointer<adqt::widgets::AdModal> m_modal;
    QPointer<adqt::widgets::AdModal> m_deleteModal;
    adqt::widgets::AdAlert* m_modalError = nullptr;
    std::array<adqt::widgets::AdLineEdit*, 4> m_inputs{};
    std::array<adqt::widgets::AdFormItem*, 5> m_fields{};
    adqt::widgets::AdSwitch* m_vision = nullptr;
    QString m_editId;
    QString m_deleteId;
    bool m_editing = false;
};
#endif
