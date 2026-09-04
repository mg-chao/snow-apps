#include "snow_shot/presentation/components/storagestatussettingswidget.h"

#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include "antd_icons.h"
#include "widgets/button.h"
#include "widgets/descriptions.h"

#include <QEvent>
#include <QFont>
#include <QLabel>
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

void addStatusItem(adqt::widgets::AdDescriptions* descriptions, const QString& key,
                   QLabel* value) {
    adqt::widgets::AdDescriptions::Item item;
    item.key = key;
    item.contentWidget = value;
    descriptions->addItem(item);
}
} // namespace

StorageStatusSettingsWidget::StorageStatusSettingsWidget(
    snow_shot::presentation::settings::SettingsRuntimeSession& runtimeSession,
    QWidget* parent)
    : SettingsCustomWidget(parent), m_runtimeSession(runtimeSession),
      m_colorScheme(snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme()) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

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
    addStatusItem(m_descriptions, QStringLiteral("location"), m_locationValue);
    addStatusItem(m_descriptions, QStringLiteral("mode"), m_modeValue);
    addStatusItem(m_descriptions, QStringLiteral("error"), m_errorValue);
    layout->addWidget(m_descriptions);

    connect(m_refreshButton, &QAbstractButton::clicked, this,
            [this]() { m_runtimeSession.refreshStorageStatus(); });
    connect(&m_runtimeSession,
            &snow_shot::presentation::settings::SettingsRuntimeSession::storageStateChanged,
            this, [this](const snow_shot::storage::StorageStatus& status) {
                syncStatus(status);
            });

    retranslateUi();
    syncStatus(m_runtimeSession.storageStatus());
    applyTheme(m_colorScheme);
}

void StorageStatusSettingsWidget::applyTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    m_colorScheme = scheme;
    const QVector<QLabel*> valueLabels{
        m_totalValue,      m_historyValue,    m_entryCountValue, m_pinnedValue,
        m_ocrValue,        m_thumbnailsValue, m_recordingTempValue,
        m_otherValue,      m_locationValue,   m_modeValue,       m_errorValue};
    for (QLabel* label : valueLabels) {
        QFont font = label->font();
        font.setPixelSize(scheme.metricAlias.fontSize);
        font.setWeight(QFont::Normal);
        label->setFont(font);
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, scheme.map.colorText);
        label->setPalette(palette);
    }
    QPalette errorPalette = m_errorValue->palette();
    errorPalette.setColor(QPalette::WindowText,
                          m_errorValue->property("hasError").toBool()
                              ? scheme.map.colorError
                              : scheme.map.colorTextSecondary);
    m_errorValue->setPalette(errorPalette);
    update();
}

void StorageStatusSettingsWidget::retranslateUi() {
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
    m_descriptions->setItemLabel(m_descriptions->indexOf(QStringLiteral("ocr")),
                                 tr("OCR assets"));
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
    m_runtimeSession.refreshStorageStatus();
}

void StorageStatusSettingsWidget::syncStatus(
    const snow_shot::storage::StorageStatus& status) {
    const snow_shot::storage::AppStorageUsage& usage = status.appUsage;
    m_totalValue->setText(usage.scanning ? tr("Scanning…") : formattedBytes(usage.totalBytes()));
    m_historyValue->setText(formattedBytes(usage.historyBytes));
    m_entryCountValue->setText(QString::number(status.historyUsage.entryCount));
    m_pinnedValue->setText(formattedBytes(usage.pinnedWindowBytes));
    m_ocrValue->setText(formattedBytes(usage.ocrAssetBytes));
    m_thumbnailsValue->setText(formattedBytes(usage.thumbnailCacheBytes));
    m_recordingTempValue->setText(formattedBytes(usage.recordingTempBytes));
    m_otherValue->setText(formattedBytes(usage.otherBytes));
    m_locationValue->setText(status.effectiveDirectory.isEmpty() ? tr("Unavailable")
                                                                 : status.effectiveDirectory);
    m_modeValue->setText(modeText(status.effectiveMode));
    QString latestError = !status.lastHistoryError.isEmpty()
                              ? status.lastHistoryError
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
