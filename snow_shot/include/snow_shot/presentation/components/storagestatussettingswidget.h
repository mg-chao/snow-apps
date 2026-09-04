#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_STORAGESTATUSSETTINGSWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_STORAGESTATUSSETTINGSWIDGET_H

#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/storage/applicationstorage.h"

class QEvent;
class QLabel;
class QVBoxLayout;

namespace adqt::widgets {
class AdButton;
class AdDescriptions;
}
namespace snow_shot::presentation::settings {
class SettingsRuntimeSession;
}

class StorageStatusSettingsWidget final : public SettingsCustomWidget {
    Q_OBJECT

  public:
    explicit StorageStatusSettingsWidget(
        snow_shot::presentation::settings::SettingsRuntimeSession& runtimeSession,
        QWidget* parent = nullptr);

    void applyTheme(
        const snow_shot::presentation::styles::ThemeColorScheme& scheme) override;
    void retranslateUi() override;

  protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncStatus(const snow_shot::storage::StorageStatus& status);

    adqt::widgets::AdDescriptions* m_descriptions = nullptr;
    adqt::widgets::AdButton* m_refreshButton = nullptr;
    QLabel* m_totalValue = nullptr;
    QLabel* m_historyValue = nullptr;
    QLabel* m_entryCountValue = nullptr;
    QLabel* m_pinnedValue = nullptr;
    QLabel* m_ocrValue = nullptr;
    QLabel* m_thumbnailsValue = nullptr;
    QLabel* m_recordingTempValue = nullptr;
    QLabel* m_otherValue = nullptr;
    QLabel* m_locationValue = nullptr;
    QLabel* m_modeValue = nullptr;
    QLabel* m_errorValue = nullptr;
    snow_shot::presentation::settings::SettingsRuntimeSession& m_runtimeSession;
    snow_shot::presentation::styles::ThemeColorScheme m_colorScheme;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_STORAGESTATUSSETTINGSWIDGET_H
