#ifndef SNOW_SHOT_CLOUDUPLOADSETTINGSWIDGET_H
#define SNOW_SHOT_CLOUDUPLOADSETTINGSWIDGET_H
#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/clouduploadconfiguration.h"
#include <QPointer>
#include <array>
#include <optional>
class QLabel;
class QVBoxLayout;
namespace adqt::widgets {
class AdButton;
class AdAlert;
class AdModal;
class AdComboBox;
} // namespace adqt::widgets
namespace snow_shot::presentation::components::form_fields {
class FormField;
}
class CloudUploadSettingsWidget final : public SettingsCustomWidget {
    Q_OBJECT
  public:
    explicit CloudUploadSettingsWidget(
        snow_shot::presentation::settings::SettingsRuntimeSession& session,
        QWidget* parent = nullptr);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) override;
    void retranslateUi() override;

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void rebuild(bool force = false);
    void openEditor(const QString& id = {});
    void deleteConfiguration(const QString& id);
    void copyConfiguration(const QString& id);
    void submitEditor(bool validateOnly = false);
    void translateEditor();
    bool save(const snow_shot::CloudUploadSettings& values);
    snow_shot::presentation::settings::SettingsRuntimeSession& m_session;
    snow_shot::presentation::styles::ThemeColorScheme m_scheme;
    QVBoxLayout* m_rows = nullptr;
    std::optional<QVector<snow_shot::CloudUploadConfiguration>> m_renderedConfigurations;
    QLabel* m_title = nullptr;
    adqt::widgets::AdButton* m_add = nullptr;
    adqt::widgets::AdAlert* m_error = nullptr;
    snow_shot::presentation::components::form_fields::FormField* m_default = nullptr;
    QPointer<adqt::widgets::AdModal> m_modal;
    QPointer<adqt::widgets::AdModal> m_deleteModal;
    adqt::widgets::AdAlert* m_modalError = nullptr;
    std::array<snow_shot::presentation::components::form_fields::FormField*, 9> m_fields{};
    snow_shot::presentation::components::form_fields::FormField* m_addressing = nullptr;
    snow_shot::presentation::components::form_fields::FormField* m_protocol = nullptr;
    QString m_editId;
    bool m_editing = false;
    bool m_editorValidationAttempted = false;
};
#endif
