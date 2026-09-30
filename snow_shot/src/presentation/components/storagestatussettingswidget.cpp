#include "snow_shot/presentation/components/storagestatussettingswidget.h"

#include "snow_shot/presentation/components/pathinput.h"
#include "snow_shot/presentation/components/settingspageutils.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include "antd_icons.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/descriptions.h"
#include "widgets/form.h"
#include "widgets/modal.h"
#include "widgets/input.h"
#include "widgets/switch.h"
#include "widgets/spin.h"
#include "widgets/message.h"
#include <QFileDialog>
#include <QDir>
#include <QPushButton>
#include <QToolButton>
#include <QTimer>

#include <QEvent>
#include <QFont>
#include <QLabel>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPalette>
#include <QShowEvent>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace {
QString formattedBytes(qint64 bytes) {
    constexpr qint64 kibibyte = 1024;
    constexpr qint64 mebibyte = kibibyte * 1024;
    constexpr qint64 gibibyte = mebibyte * 1024;
    if (bytes >= gibibyte) {
        return QStringLiteral("%1 GiB").arg(static_cast<double>(bytes) / gibibyte, 0, 'f', 2);
    }
    if (bytes >= mebibyte) {
        return QStringLiteral("%1 MiB").arg(static_cast<double>(bytes) / mebibyte, 0, 'f', 2);
    }
    if (bytes >= kibibyte) {
        return QStringLiteral("%1 KiB").arg(static_cast<double>(bytes) / kibibyte, 0, 'f', 2);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

QString modeText(snow_shot::storage::StorageMode mode) {
    using snow_shot::storage::StorageMode;
    switch (mode) {
    case StorageMode::ApplicationData:
        return StorageStatusSettingsWidget::tr("Application data");
    case StorageMode::Custom:
        return StorageStatusSettingsWidget::tr("Custom directory");
    case StorageMode::Portable:
        return StorageStatusSettingsWidget::tr("Portable");
    case StorageMode::FutureVersionReadOnly:
        return StorageStatusSettingsWidget::tr("Read-only (newer configuration)");
    case StorageMode::Degraded:
    default:
        return StorageStatusSettingsWidget::tr("Unavailable");
    }
}

QLabel* createStatusValue(adqt::widgets::AdDescriptions* descriptions, const QString& id) {
    auto* value = new QLabel(descriptions);
    value->setObjectName(snow_shot::presentation::settings::generatedObjectName(
        QStringLiteral("settings-status-value"), id));
    value->setAlignment(Qt::AlignRight | Qt::AlignTop);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setWordWrap(true);
    value->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return value;
}

void addStatusItem(adqt::widgets::AdDescriptions* descriptions, const QString& key, QLabel* value) {
    adqt::widgets::AdDescriptions::Item item;
    item.key = key;
    item.contentWidget = value;
    descriptions->addItem(item);
}
} // namespace

StorageStatusSettingsWidget::StorageStatusSettingsWidget(
    snow_shot::presentation::settings::SettingsRuntimeSession& runtimeSession, QWidget* parent)
    : SettingsCustomWidget(parent), m_runtimeSession(runtimeSession),
      m_colorScheme(snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme()) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

#ifdef Q_OS_WIN
    m_directoryButton = new adqt::widgets::AdButton(this);
    m_directoryButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
    m_directoryButton->setSizeClass(adqt::widgets::AdButton::SizeClass::Medium);
    m_directoryButton->setObjectName(QStringLiteral("settings-storage-directory-choose"));
    auto* directoryRow = snow_shot::presentation::components::createSettingItemRow(
        this, m_colorScheme.metricAlias, &m_directoryTitle, &m_directoryDescription,
        m_directoryButton, QStringLiteral("settings-storage-directory-row"));
    m_directoryTitle->setObjectName(QStringLiteral("settings-storage-directory-title"));
    m_directoryDescription->setObjectName(QStringLiteral("settings-storage-directory-location"));
    m_directoryDescription->setWordWrap(true);
    m_directoryDescription->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(directoryRow);
    layout->addSpacing(m_colorScheme.metricAlias.marginLG);
    connect(m_directoryButton, &QAbstractButton::clicked, this,
            &StorageStatusSettingsWidget::openDirectoryDialog);
    connect(&m_runtimeSession,
            &snow_shot::presentation::settings::SettingsRuntimeSession::directoryChangeProgress,
            this, [this](const snow_shot::storage::StorageDirectoryProgress& progress) {
                m_directoryProgress = progress;
                updateDirectoryProgress();
            });
    connect(&m_runtimeSession,
            &snow_shot::presentation::settings::SettingsRuntimeSession::directoryChangeFinished,
            this, &StorageStatusSettingsWidget::finishDirectoryChange);
#endif
    m_descriptions = new adqt::widgets::AdDescriptions(this);
    m_descriptions->setObjectName(QStringLiteral("settings-storage-status-descriptions"));
    m_descriptions->setBordered(false);
    m_descriptions->setColumn(1);
    m_refreshButton = new adqt::widgets::AdButton(m_descriptions);
    m_refreshButton->setObjectName(QStringLiteral("settings-storage-status-refresh"));
    m_refreshButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    m_refreshButton->setSizeClass(adqt::widgets::AdButton::SizeClass::Small);
    m_refreshButton->setIconRef(adqt::icons::antd::outlined::Reload());
    m_descriptions->setExtraWidget(m_refreshButton);
    m_totalValue = createStatusValue(m_descriptions, QStringLiteral("total"));
    m_historyValue = createStatusValue(m_descriptions, QStringLiteral("history"));
    m_entryCountValue = createStatusValue(m_descriptions, QStringLiteral("entries"));
    m_pinnedValue = createStatusValue(m_descriptions, QStringLiteral("pinned"));
    m_ocrValue = createStatusValue(m_descriptions, QStringLiteral("ocr"));
    m_thumbnailsValue = createStatusValue(m_descriptions, QStringLiteral("thumbnails"));
    m_recordingTempValue = createStatusValue(m_descriptions, QStringLiteral("recording-temp"));
    m_otherValue = createStatusValue(m_descriptions, QStringLiteral("other"));
    m_diagnosticsValue = createStatusValue(m_descriptions, QStringLiteral("diagnostics"));
    m_logLocationValue = createStatusValue(m_descriptions, QStringLiteral("log-location"));
    m_logStatusValue = createStatusValue(m_descriptions, QStringLiteral("log-status"));
    m_locationValue = createStatusValue(m_descriptions, QStringLiteral("location"));
    m_modeValue = createStatusValue(m_descriptions, QStringLiteral("mode"));
    m_errorValue = createStatusValue(m_descriptions, QStringLiteral("error"));
    addStatusItem(m_descriptions, QStringLiteral("total"), m_totalValue);
    addStatusItem(m_descriptions, QStringLiteral("history"), m_historyValue);
    addStatusItem(m_descriptions, QStringLiteral("entries"), m_entryCountValue);
    addStatusItem(m_descriptions, QStringLiteral("pinned"), m_pinnedValue);
    addStatusItem(m_descriptions, QStringLiteral("ocr"), m_ocrValue);
    addStatusItem(m_descriptions, QStringLiteral("thumbnails"), m_thumbnailsValue);
    addStatusItem(m_descriptions, QStringLiteral("recordingTemp"), m_recordingTempValue);
    addStatusItem(m_descriptions, QStringLiteral("other"), m_otherValue);
    addStatusItem(m_descriptions, QStringLiteral("diagnostics"), m_diagnosticsValue);
    addStatusItem(m_descriptions, QStringLiteral("logLocation"), m_logLocationValue);
    addStatusItem(m_descriptions, QStringLiteral("logStatus"), m_logStatusValue);
    addStatusItem(m_descriptions, QStringLiteral("location"), m_locationValue);
    addStatusItem(m_descriptions, QStringLiteral("mode"), m_modeValue);
    addStatusItem(m_descriptions, QStringLiteral("error"), m_errorValue);
    layout->addWidget(m_descriptions);
    auto* actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    m_copyLogFeedback = new QLabel(this);
    m_copyLogFeedback->setObjectName(QStringLiteral("settings-storage-log-copy-feedback"));
    m_copyLogFeedback->setWordWrap(true);
    m_copyLogButton = new adqt::widgets::AdButton(this);
    m_copyLogButton->setObjectName(QStringLiteral("settings-storage-copy-today-log"));
    m_copyLogButton->setIconRef(adqt::icons::antd::outlined::Copy());
    m_copyLogButton->setSizeClass(adqt::widgets::AdButton::SizeClass::Small);
    layout->addWidget(m_copyLogFeedback);
    actions->addStretch(1);
    actions->addWidget(m_copyLogButton);
    layout->addLayout(actions);
    connect(m_copyLogButton, &QAbstractButton::clicked, this, [this] {
        m_copyLogOutcome = 0;
        m_copyLogFeedback->clear();
        static_cast<void>(m_runtimeSession.triggerAction(
            snow_shot::presentation::settings::SettingsActionBinding::CopyTodayLog));
        syncStatus(m_runtimeSession.storageStatus());
    });
    connect(&m_runtimeSession,
            &snow_shot::presentation::settings::SettingsRuntimeSession::actionFinished, this,
            [this](snow_shot::presentation::settings::SettingsActionBinding action, bool success,
                   const QString& error) {
                if (action !=
                    snow_shot::presentation::settings::SettingsActionBinding::CopyTodayLog)
                    return;
                m_copyLogOutcome = success ? 1 : -1;
                m_copyLogError = error;
                retranslateUi();
                syncStatus(m_runtimeSession.storageStatus());
            });

    connect(m_refreshButton, &QAbstractButton::clicked, this,
            [this]() { m_runtimeSession.refreshStorageStatus(); });
    connect(&m_runtimeSession,
            &snow_shot::presentation::settings::SettingsRuntimeSession::storageStateChanged, this,
            [this](const snow_shot::storage::StorageStatus& status) { syncStatus(status); });

    retranslateUi();
    syncStatus(m_runtimeSession.storageStatus());
    applyTheme(m_colorScheme);
}

void StorageStatusSettingsWidget::applyTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    m_colorScheme = scheme;
    const QVector<QLabel*> valueLabels{m_totalValue,         m_historyValue,   m_entryCountValue,
                                       m_pinnedValue,        m_ocrValue,       m_thumbnailsValue,
                                       m_recordingTempValue, m_otherValue,     m_locationValue,
                                       m_modeValue,          m_errorValue,     m_diagnosticsValue,
                                       m_logLocationValue,   m_logStatusValue, m_copyLogFeedback};
    QVector<QLabel*> labels = valueLabels;
    if (m_directoryModal)
        labels << m_directoryProgressLabel;
    for (QLabel* label : labels) {
        QFont font = label->font();
        font.setPixelSize(scheme.metricAlias.fontSize);
        font.setWeight(QFont::Normal);
        label->setFont(font);
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, scheme.map.colorText);
        label->setPalette(palette);
    }
    if (m_directoryTitle) {
        snow_shot::presentation::components::applySettingItemTheme(m_directoryTitle,
                                                                   m_directoryDescription, scheme);
    }
    QPalette errorPalette = m_errorValue->palette();
    errorPalette.setColor(QPalette::WindowText, m_errorValue->property("hasError").toBool()
                                                    ? scheme.map.colorError
                                                    : scheme.map.colorTextSecondary);
    m_errorValue->setPalette(errorPalette);
    update();
}

void StorageStatusSettingsWidget::retranslateUi() {
    if (m_directoryButton) {
        m_directoryTitle->setText(tr("Storage directory"));
        m_directoryButton->setText(tr("Choose directory"));
        m_directoryButton->setAccessibleName(tr("Choose storage directory"));
        m_directoryDescription->setText(
            tr("Current storage location: %1")
                .arg(
                    QDir::toNativeSeparators(m_runtimeSession.storageStatus().effectiveDirectory)));
    }
    if (m_directoryModal) {
        m_directoryModal->setWindowTitle(tr("Storage directory"));
        m_directoryModal->setAcceptText(tr("OK"));
        m_directoryModal->setRejectText(tr("Cancel"));
        m_directoryField->setLabel(tr("Storage directory"));
        m_directoryInput->setAccessibleName(tr("Storage directory"));
        m_directoryInput->lineEdit()->setAccessibleName(tr("Storage directory"));
        m_directoryInput->setBrowseButtonText(tr("Choose storage directory"));
        m_migrateField->setLabel(tr("Migrate existing data"));
        m_migrateSwitch->setAccessibleName(tr("Migrate existing data"));
        if (!m_directoryField->errorMessages().isEmpty())
            static_cast<void>(m_directoryField->validate());
        updateDirectoryProgress();
    }
    if (m_directoryConfirmation) {
        m_directoryConfirmation->setWindowTitle(tr("Change storage directory?"));
        m_directoryConfirmation->setText(
            m_migrateSwitch->isChecked()
                ? tr("Existing data will be migrated to %1. Verified files will be removed from "
                     "the old directory after the switch. Continue?")
                      .arg(m_directoryInput->text())
                : tr("Settings and open pinned windows will be copied to %1. Screenshot and closed "
                     "pinned history will stay in the old directory. Continue?")
                      .arg(m_directoryInput->text()));
        m_directoryConfirmation->setAcceptText(tr("Proceed"));
        m_directoryConfirmation->setRejectText(tr("Cancel"));
    }
    m_copyLogButton->setText(tr("Copy today's log file"));
    m_copyLogButton->setAccessibleName(tr("Copy today's log file"));
    m_copyLogFeedback->setText(
        m_copyLogOutcome > 0   ? tr("Log file copied.")
        : m_copyLogOutcome < 0 ? tr("Could not copy the log file: %1")
                                     .arg(QCoreApplication::translate(
                                         "DiagnosticsService", m_copyLogError.toUtf8().constData()))
                               : QString());
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("diagnostics")),
                                 tr("Logs and crash reports"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("logLocation")),
                                 tr("Log location"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("logStatus")),
                                 tr("Diagnostics status"));
    m_diagnosticsValue->setAccessibleName(tr("Logs and crash reports disk usage"));
    m_logLocationValue->setAccessibleName(tr("Effective log location"));
    m_logStatusValue->setAccessibleName(tr("Diagnostics status"));
    m_descriptions->setTitle(tr("App storage usage"));
    m_refreshButton->setText(tr("Refresh"));
    m_refreshButton->setAccessibleName(tr("Refresh storage usage"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("total")),
                                 tr("Total app storage"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("history")),
                                 tr("Screenshot history"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("entries")),
                                 tr("History entries"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("pinned")),
                                 tr("Pinned windows"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("ocr")), tr("OCR assets"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("thumbnails")),
                                 tr("Thumbnail cache"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("recordingTemp")),
                                 tr("Recording temporary files"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("other")),
                                 tr("Other files"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("location")),
                                 tr("Storage location"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("mode")),
                                 tr("Storage mode"));
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("error")),
                                 tr("Latest error"));
    m_totalValue->setAccessibleName(tr("Total app storage usage"));
    m_historyValue->setAccessibleName(tr("Screenshot history disk usage"));
    m_entryCountValue->setAccessibleName(tr("History entry count"));
    m_pinnedValue->setAccessibleName(tr("Pinned windows disk usage"));
    m_ocrValue->setAccessibleName(tr("OCR asset disk usage"));
    m_thumbnailsValue->setAccessibleName(tr("Thumbnail cache disk usage"));
    m_recordingTempValue->setAccessibleName(tr("Recording temporary disk usage"));
    m_otherValue->setAccessibleName(tr("Other app data disk usage"));
    m_locationValue->setAccessibleName(tr("Effective storage location"));
    m_modeValue->setAccessibleName(tr("Effective storage mode"));
    m_errorValue->setAccessibleName(tr("Latest storage error"));
}

void StorageStatusSettingsWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        syncStatus(m_runtimeSession.storageStatus());
    }
}

void StorageStatusSettingsWidget::showEvent(QShowEvent* event) {
    SettingsCustomWidget::showEvent(event);
    // Repeated show events (flipping between settings pages) reuse the cached
    // usage snapshot; the refresh button performs an unconditional rescan.
    m_runtimeSession.refreshStorageStatusIfStale();
}

void StorageStatusSettingsWidget::syncStatus(const snow_shot::storage::StorageStatus& status) {
    if (m_directoryButton) {
        m_directoryButton->setEnabled(status.writeAvailable && !status.directoryChanging);
        m_directoryDescription->setText(
            tr("Current storage location: %1")
                .arg(QDir::toNativeSeparators(status.effectiveDirectory)));
    }
    const snow_shot::storage::AppStorageUsage& usage = status.appUsage;
    m_totalValue->setText(usage.scanning ? tr("Scanning…") : formattedBytes(usage.totalBytes()));
    m_historyValue->setText(formattedBytes(usage.historyBytes));
    m_entryCountValue->setText(QString::number(status.historyUsage.entryCount));
    m_pinnedValue->setText(formattedBytes(usage.pinnedWindowBytes));
    m_ocrValue->setText(formattedBytes(usage.ocrAssetBytes));
    m_thumbnailsValue->setText(formattedBytes(usage.thumbnailCacheBytes));
    m_recordingTempValue->setText(formattedBytes(usage.recordingTempBytes));
    m_otherValue->setText(formattedBytes(usage.otherBytes));
    m_diagnosticsValue->setText(formattedBytes(usage.diagnosticsBytes));
    m_logLocationValue->setText(
        status.diagnostics.directory.isEmpty() ? tr("Unavailable") : status.diagnostics.directory);
    QString diagnosticsState = !status.diagnostics.loggingAvailable ? tr("File logging unavailable")
                               : status.diagnostics.crashCaptureAvailable
                                   ? tr("File logging and crash capture active")
                                   : tr("File logging active; crash capture unavailable");
    if (!status.diagnostics.fallbackReason.isEmpty())
        diagnosticsState += u'\n' + QCoreApplication::translate(
                                        "DiagnosticsService",
                                        status.diagnostics.fallbackReason.toUtf8().constData());
    if (!status.diagnostics.lastError.isEmpty())
        diagnosticsState +=
            u'\n' + QCoreApplication::translate("DiagnosticsService",
                                                status.diagnostics.lastError.toUtf8().constData());
    m_logStatusValue->setText(diagnosticsState);
    const auto copyState = m_runtimeSession.actionState(
        snow_shot::presentation::settings::SettingsActionBinding::CopyTodayLog);
    m_copyLogButton->setEnabled(copyState.enabled && !copyState.busy);
    m_locationValue->setText(status.effectiveDirectory.isEmpty() ? tr("Unavailable")
                                                                 : status.effectiveDirectory);
    m_modeValue->setText(modeText(status.effectiveMode));
    QString latestError = !status.lastHistoryError.isEmpty() ? status.lastHistoryError
                                                             : status.lastConfigurationError;
    if (latestError.isEmpty()) {
        latestError = status.fallbackReason;
    }
    const bool hasError = !latestError.isEmpty();
    m_errorValue->setProperty("hasError", hasError);
    m_errorValue->setText(hasError ? latestError : tr("None"));
    m_refreshButton->setEnabled(!usage.scanning && !status.cacheClearing);
    applyTheme(m_colorScheme);
}

void StorageStatusSettingsWidget::openDirectoryDialog() {
#ifdef Q_OS_WIN
    using namespace adqt::widgets;
    if (m_directoryModal)
        return;
    auto* modal = new AdModal(this);
    m_directoryModal = modal;
    modal->setObjectName(QStringLiteral("settings-storage-directory-modal"));
    modal->setMode(AdModal::Mode::Overlay);
    modal->setOwnerWindow(window());
    modal->setCentered(true);
    modal->setPreferredWidth(580);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    modal->setStandardButtons(AdModal::StandardButton::Ok | AdModal::StandardButton::Cancel);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    m_directoryForm = new QWidget(body);
    m_directoryForm->setObjectName(QStringLiteral("storage-directory-form"));
    auto* grid = new QGridLayout(m_directoryForm);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(m_colorScheme.metricAlias.marginLG);
    grid->setVerticalSpacing(0);
    grid->setColumnStretch(0, 1);
    m_directoryInput = new DirectoryPathInput(m_directoryForm);
    m_directoryInput->setObjectName(QStringLiteral("storage-directory-path-input"));
    m_directoryInput->lineEdit()->setObjectName(QStringLiteral("storage-directory-input"));
    m_directoryInput->browseButton()->setObjectName(QStringLiteral("storage-directory-browse"));
    m_directoryInput->setText(
        QDir::toNativeSeparators(m_runtimeSession.storageStatus().effectiveDirectory));
    m_directoryField =
        new AdFormItem(QString(), m_directoryInput, QStringLiteral("directory"), m_directoryForm);
    m_directoryField->setObjectName(QStringLiteral("storage-directory-field"));
    m_directoryField->setItemLayout(AdFormItem::ItemLayout::Vertical);
    m_directoryField->setControlValueProperty(QStringLiteral("text"));
    m_directoryField->setValidateOnChange(false);
    m_directoryField->setFormValidator([this](const QVariant& value, AdFormItem*) {
        AdFormItem::ValidationResult result;
        const auto validation = snow_shot::storage::validateStorageDirectory(
            m_runtimeSession.storageStatus().effectiveDirectory,
            QDir::fromNativeSeparators(value.toString().trimmed()));
        if (!validation.success) {
            result.status = AdFormItem::ValidateStatus::Error;
            result.errors.push_back(validation.error);
        }
        return result;
    });
    grid->addWidget(m_directoryField, 0, 0, Qt::AlignTop);
    m_migrateSwitch = new AdSwitch(m_directoryForm);
    m_migrateSwitch->setObjectName(QStringLiteral("storage-directory-migrate"));
    m_migrateSwitch->setChecked(true);
    m_migrateField =
        new AdFormItem(QString(), m_migrateSwitch, QStringLiteral("migrate"), m_directoryForm);
    m_migrateField->setObjectName(QStringLiteral("storage-directory-migrate-field"));
    m_migrateField->setItemLayout(AdFormItem::ItemLayout::Vertical);
    m_migrateField->setControlValueProperty(QStringLiteral("checked"));
    grid->addWidget(m_migrateField, 1, 0, Qt::AlignTop);
    layout->addWidget(m_directoryForm);
    m_directoryError = new AdAlert(body);
    m_directoryError->setObjectName(QStringLiteral("storage-directory-error"));
    m_directoryError->setSeverity(AdAlert::Severity::Error);
    m_directoryError->hide();
    layout->addWidget(m_directoryError);
    m_directoryProgressBody = new QWidget(body);
    auto* progressLayout = new QVBoxLayout(m_directoryProgressBody);
    auto* spin = new AdSpin(m_directoryProgressBody);
    spin->setObjectName(QStringLiteral("storage-directory-spinner"));
    spin->setSpinning(true);
    progressLayout->addWidget(spin, 0, Qt::AlignHCenter);
    m_directoryProgressLabel = new QLabel(m_directoryProgressBody);
    m_directoryProgressLabel->setObjectName(QStringLiteral("storage-directory-progress"));
    m_directoryProgressLabel->setWordWrap(true);
    m_directoryProgressLabel->setAlignment(Qt::AlignCenter);
    progressLayout->addWidget(m_directoryProgressLabel);
    layout->addWidget(m_directoryProgressBody);
    m_directoryProgressBody->hide();
    modal->setContentWidget(body);
    modal->setInitialFocusWidget(m_directoryInput->lineEdit());
    connect(m_directoryInput, &DirectoryPathInput::browseRequested, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(
            m_directoryModal->contentWidget()->window(), tr("Choose storage directory"),
            m_directoryInput->text());
        if (!directory.isEmpty())
            m_directoryInput->setText(QDir::toNativeSeparators(directory));
    });
    connect(modal, &AdModal::closeRequested, this, [this, modal](AdModal::CloseReason reason) {
        if (m_directoryBusy || m_directoryConfirmation)
            return;
        if (reason != AdModal::CloseReason::OkAction) {
            modal->reject();
            return;
        }
        if (!m_directoryField->validate()) {
            m_directoryInput->lineEdit()->setFocus();
            return;
        }
        m_directoryError->setText(QString());
        m_directoryError->hide();
        auto* confirmation = new AdModal(modal->contentWidget());
        m_directoryConfirmation = confirmation;
        confirmation->setObjectName(QStringLiteral("storage-directory-confirmation"));
        confirmation->setMode(AdModal::Mode::Window);
        confirmation->setOwnerWindow(modal->contentWidget()->window());
        confirmation->setCentered(true);
        confirmation->setCloseOnMaskClick(false);
        confirmation->setPreset(AdModal::Preset::Confirm);
        confirmation->setAcceptAccentRole(AdButton::AccentRole::Danger);
        confirmation->setStandardButtons(AdModal::StandardButton::Ok |
                                         AdModal::StandardButton::Cancel);
        retranslateUi();
        connect(confirmation, &AdModal::accepted, this, [this] {
            const QString directory = m_directoryInput->text();
            const bool migrate = m_migrateSwitch->isChecked();
            setDirectoryBusy(true);
            m_directoryProgress = {};
            updateDirectoryProgress();
            QTimer::singleShot(0, this, [this, directory, migrate] {
                const auto result = m_runtimeSession.changeStorageDirectory(directory, migrate);
                if (!result.success)
                    finishDirectoryChange({false, result.error, {}});
            });
        });
        connect(confirmation, &AdModal::finished, this, [this, confirmation](AdModal::DialogCode) {
            m_directoryConfirmation = nullptr;
            confirmation->deleteLater();
        });
        confirmation->setOpen(true);
    });
    connect(modal, &AdModal::finished, this, [this, modal](AdModal::DialogCode) {
        if (m_directoryConfirmation)
            m_directoryConfirmation->reject();
        m_directoryModal = nullptr;
        m_directoryForm = nullptr;
        m_directoryField = nullptr;
        m_migrateField = nullptr;
        m_directoryInput = nullptr;
        m_migrateSwitch = nullptr;
        m_directoryError = nullptr;
        m_directoryProgressBody = nullptr;
        m_directoryProgressLabel = nullptr;
        m_directoryBusy = false;
        modal->deleteLater();
    });
    retranslateUi();
    applyTheme(m_colorScheme);
    body->ensurePolished();
    const auto children = body->findChildren<QWidget*>();
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        (*it)->ensurePolished();
        if ((*it)->layout())
            (*it)->layout()->activate();
    }
    body->layout()->activate();
    modal->setOpen(true);
#endif
}

void StorageStatusSettingsWidget::setDirectoryBusy(bool busy) {
    m_directoryBusy = busy;
    if (!m_directoryModal)
        return;
    m_directoryModal->acceptButton()->setEnabled(!busy);
    m_directoryModal->rejectButton()->setEnabled(!busy);
    m_directoryModal->setCloseButtonVisible(!busy);
    m_directoryModal->setCloseOnEscape(!busy);
    m_directoryModal->setCloseOnMaskClick(false);
    m_directoryForm->setVisible(!busy);
    m_directoryError->setVisible(!busy && !m_directoryError->text().isEmpty());
    m_directoryProgressBody->setVisible(busy);
}

void StorageStatusSettingsWidget::updateDirectoryProgress() {
    if (!m_directoryProgressLabel)
        return;
    using Stage = snow_shot::storage::StorageDirectoryProgress::Stage;
    QString text;
    switch (m_directoryProgress.stage) {
    case Stage::Preparing:
        text = tr("Preparing migration…");
        break;
    case Stage::Switching:
        text = tr("Switching storage directory…");
        break;
    case Stage::Cleaning:
        text = tr("Removing old files — %1/%2")
                   .arg(m_directoryProgress.completed)
                   .arg(m_directoryProgress.total);
        break;
    case Stage::Verifying:
        text = tr("Verifying data — %1/%2")
                   .arg(m_directoryProgress.completed)
                   .arg(m_directoryProgress.total);
        break;
    case Stage::Copying:
        text = m_directoryProgress.category == u"history"
                   ? tr("Migrating screenshot history — %1/%2")
               : m_directoryProgress.category == u"pinned" ? tr("Migrating pinned windows — %1/%2")
               : m_directoryProgress.category == u"ocr"    ? tr("Migrating OCR assets — %1/%2")
               : m_directoryProgress.category == u"logs"   ? tr("Migrating logs — %1/%2")
                                                           : tr("Migrating other data — %1/%2");
        text = text.arg(m_directoryProgress.completed).arg(m_directoryProgress.total);
        break;
    }
    m_directoryProgressLabel->setText(text);
}

void StorageStatusSettingsWidget::finishDirectoryChange(
    const snow_shot::storage::StorageDirectoryChangeResult& result) {
    if (!m_directoryModal || !m_directoryBusy)
        return;
    using namespace adqt::widgets;
    AdMessage::Request message;
    if (result.success) {
        message.content =
            result.warning.isEmpty() ? tr("Storage migration complete.") : result.warning;
        m_directoryBusy = false;
        m_directoryModal->accept();
        if (result.warning.isEmpty())
            AdMessageService::success(std::move(message), window());
        else
            AdMessageService::warning(std::move(message), window());
        syncStatus(m_runtimeSession.storageStatus());
    } else {
        setDirectoryBusy(false);
        message.content =
            result.error + (result.warning.isEmpty() ? QString() : u'\n' + result.warning);
        m_directoryError->setText(message.content);
        m_directoryError->show();
        AdMessageService::error(std::move(message), window());
    }
}
