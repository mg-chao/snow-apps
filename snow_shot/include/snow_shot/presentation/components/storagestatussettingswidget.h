#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_STORAGESTATUSSETTINGSWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_STORAGESTATUSSETTINGSWIDGET_H

#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QPointer>

class QEvent;
class QLabel;
class DirectoryPathInput;

namespace adqt::widgets {
class AdModal;
class AdAlert;
class AdFormItem;
class AdSwitch;
class AdSpin;
class AdButton;
class AdDescriptions;
} // namespace adqt::widgets
namespace snow_shot::presentation::settings {
class SettingsRuntimeSession;
}

class StorageStatusSettingsWidget final : public SettingsCustomWidget {
    Q_OBJECT

  public:
    explicit StorageStatusSettingsWidget(
        snow_shot::presentation::settings::SettingsRuntimeSession& runtimeSession,
        QWidget* parent = nullptr);

    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) override;
    void retranslateUi() override;

  protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void openDirectoryDialog();
    void setDirectoryBusy(bool busy);
    void updateDirectoryProgress();
    void finishDirectoryChange(const snow_shot::storage::StorageDirectoryChangeResult& result);
    adqt::widgets::AdButton* m_directoryButton = nullptr;
    QLabel* m_directoryTitle = nullptr;
    QLabel* m_directoryDescription = nullptr;
    QPointer<adqt::widgets::AdModal> m_directoryModal;
    QPointer<adqt::widgets::AdModal> m_directoryConfirmation;
    DirectoryPathInput* m_directoryInput = nullptr;
    adqt::widgets::AdSwitch* m_migrateSwitch = nullptr;
    adqt::widgets::AdFormItem* m_directoryField = nullptr;
    adqt::widgets::AdFormItem* m_migrateField = nullptr;
    adqt::widgets::AdAlert* m_directoryError = nullptr;
    QLabel* m_directoryProgressLabel = nullptr;
    QWidget* m_directoryForm = nullptr;
    QWidget* m_directoryProgressBody = nullptr;
    bool m_directoryBusy = false;
    snow_shot::storage::StorageDirectoryProgress m_directoryProgress;
    void syncStatus(const snow_shot::storage::StorageStatus& status);

    adqt::widgets::AdDescriptions* m_descriptions = nullptr;
    adqt::widgets::AdButton* m_refreshButton = nullptr;
    adqt::widgets::AdButton* m_copyLogButton = nullptr;
    QLabel* m_diagnosticsValue = nullptr;
    QLabel* m_logLocationValue = nullptr;
    QLabel* m_logStatusValue = nullptr;
    QLabel* m_copyLogFeedback = nullptr;
    int m_copyLogOutcome = 0;
    QString m_copyLogError;
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
