#include "snow_shot/presentation/components/storagestatussettingswidget.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"

#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/descriptions.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QLayout>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace presentation = snow_shot::presentation;
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

settings::TranslatableText text(const char* source) {
    return {"StorageStatusWidgetTests", source};
}

class FakeSettingsBackend final : public settings::SettingsBackend {
  public:
    FakeSettingsBackend() {
        m_status.writeAvailable = true;
        m_status.effectiveMode = storage::StorageMode::ApplicationData;
        m_status.effectiveDirectory = QStringLiteral("C:/storage-status-widget-tests");
        m_status.historyUsage.entryCount = 3;
        m_status.appUsage.historyBytes = 1024;
        m_status.appUsage.pinnedWindowBytes = 2048;
        m_status.appUsage.ocrAssetBytes = 5 * 1024 * 1024;
        m_status.appUsage.thumbnailCacheBytes = 512;
        m_status.appUsage.recordingTempBytes = 3 * 1024 * 1024;
        m_status.appUsage.otherBytes = 128;
    }

    QVariant selectValue(settings::SettingsSelectBinding) const override {
        return {};
    }
    QVector<settings::SettingsRuntimeOption>
    dynamicSelectOptions(settings::SettingsSelectBinding) const override {
        return {};
    }
    bool applySelectValue(settings::SettingsSelectBinding, const QVariant&) override {
        return false;
    }
    bool switchValue(settings::SettingsSwitchBinding) const override {
        return false;
    }
    bool switchEnabled(settings::SettingsSwitchBinding) const override {
        return true;
    }
    bool applySwitchValue(settings::SettingsSwitchBinding, bool) override {
        return false;
    }
    QVariantList multiSelectValue(settings::SettingsMultiSelectBinding) const override {
        return {};
    }
    bool applyMultiSelectValue(settings::SettingsMultiSelectBinding, const QVariantList&) override {
        return false;
    }
    int integerValue(settings::SettingsIntegerBinding) const override {
        return 0;
    }
    bool applyIntegerValue(settings::SettingsIntegerBinding, int) override {
        return false;
    }
    int sliderValue(settings::SettingsSliderBinding) const override {
        return 0;
    }
    bool applySliderValue(settings::SettingsSliderBinding, int) override {
        return false;
    }
    QColor colorValue(settings::SettingsColorBinding) const override {
        return {};
    }
    bool applyColorValue(settings::SettingsColorBinding, const QColor&) override {
        return false;
    }
    QVariant radioValue(settings::SettingsRadioBinding) const override {
        return {};
    }
    bool applyRadioValue(settings::SettingsRadioBinding, const QVariant&) override {
        return false;
    }
    QString filePathValue(settings::SettingsFilePathBinding) const override {
        return {};
    }
    bool applyFilePathValue(settings::SettingsFilePathBinding, const QString&) override {
        return false;
    }
    QString directoryPathValue(settings::SettingsDirectoryPathBinding) const override {
        return {};
    }
    bool applyDirectoryPathValue(settings::SettingsDirectoryPathBinding, const QString&) override {
        return false;
    }
    QString textValue(settings::SettingsTextBinding) const override {
        return {};
    }
    bool applyTextValue(settings::SettingsTextBinding, const QString&) override {
        return false;
    }
    storage::ScreenshotToolbarLayout toolbarLayout() const override {
        return {};
    }
    bool applyToolbarLayout(const storage::ScreenshotToolbarLayout&) override {
        return false;
    }
    presentation::GlobalShortcutRegistrationState
    shortcutState(presentation::GlobalShortcutAction) const override {
        return {};
    }
    presentation::GlobalShortcutValidationResult validateShortcut(const QString&) const override {
        return {};
    }
    bool applyShortcuts(presentation::GlobalShortcutAction, const QStringList&) override {
        return false;
    }
    QStringList localShortcuts(settings::SettingsLocalShortcutScope,
                               const QString&) const override {
        return {};
    }
    presentation::GlobalShortcutValidationResult
    validateLocalShortcut(settings::SettingsLocalShortcutScope, const QString&,
                          const QString&) const override {
        return {};
    }
    bool applyLocalShortcuts(settings::SettingsLocalShortcutScope, const QString&,
                             const QStringList&) override {
        return false;
    }
    settings::SettingsActionState actionState(settings::SettingsActionBinding) const override {
        return {true, false};
    }
    bool triggerAction(settings::SettingsActionBinding) override {
        return true;
    }
    storage::StorageStatus storageStatus() const override {
        return m_status;
    }
    void refreshStorageStatus() override {
        ++m_refreshCount;
    }
    bool resetSection(settings::SettingsSectionReset) override {
        return false;
    }

    int refreshCount() const {
        return m_refreshCount;
    }

    void publish(storage::StorageStatus status) {
        m_status = std::move(status);
        emit synchronized();
    }

    void notify() {
        emit synchronized();
    }

  private:
    storage::StorageStatus m_status;
    int m_refreshCount = 0;
};

settings::SettingsRegistry storageStatusRegistry() {
    settings::SettingsCustomDefinition storageStatus;
    storageStatus.renderer = settings::SettingsCustomRenderer::StorageStatus;
    settings::SettingsSectionDefinition section{QStringLiteral("storage"),
                                                text("Storage"),
                                                text("Storage settings"),
                                                settings::SettingsSectionReset::None,
                                                {{QStringLiteral("storage-status"),
                                                  text("Storage status"),
                                                  text("Storage status"),
                                                  {},
                                                  {},
                                                  storageStatus}}};
    settings::SettingsPageDefinition page{QStringLiteral("storage-page"),
                                          QStringLiteral("/storage"),
                                          text("Storage"),
                                          text("Storage settings"),
                                          {section}};
    settings::SettingsNavigationPageDefinition navigation{
        QStringLiteral("nav.storage"), page.id,
        []() { return adqt::icons::antd::outlined::Appstore(); }};
    return settings::SettingsRegistry::fromCatalog(
        settings::SettingsCatalog({page}, {navigation}, {page.id, section.id, {}}),
        QStringLiteral("test-provider"));
}

void widgetUsesDescriptionsTitleAndThemeSpacing() {
    const settings::SettingsRegistry registry = storageStatusRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    StorageStatusSettingsWidget widget(session);

    auto* descriptions = widget.findChild<adqt::widgets::AdDescriptions*>(
        QStringLiteral("settings-storage-status-descriptions"));
    require(descriptions != nullptr, "the storage status form must be an AdDescriptions");
    require(descriptions->column() == 1, "the storage status form must be single column");
    require(!descriptions->bordered(), "the storage status form must not be bordered");

    QLabel* title = descriptions->findChild<QLabel*>(QStringLiteral("adDescriptionsTitle"));
    require(title != nullptr, "the overall title must be rendered by the descriptions component");
    require(title->text() == QStringLiteral("App storage usage"),
            "the descriptions title must say App storage usage");
    QAbstractButton* refresh =
        widget.findChild<QAbstractButton*>(QStringLiteral("settings-storage-status-refresh"));
    require(descriptions->extraWidget() == refresh,
            "the refresh button must be the descriptions extra widget");

    QWidget* header = descriptions->findChild<QWidget*>(QStringLiteral("adDescriptionsHeader"));
    require(header != nullptr && header->layout() != nullptr,
            "the descriptions header must exist");
    const QMargins headerMargins = header->layout()->contentsMargins();
    const adqt::theme::ResolvedTheme theme =
        adqt::theme::ThemeManager::instance().resolve(descriptions);
    const int expectedTitleMargin =
        std::max(0, qRound(theme.theme.metrics.fontSizeSM * theme.theme.metrics.lineHeightSM));
    require(headerMargins.bottom() - headerMargins.top() == expectedTitleMargin,
            "the overall title bottom margin must follow the theme metric");
    require(expectedTitleMargin > 0, "the theme metric must reserve space below the title");

    QLabel* totalLabel = nullptr;
    const QList<QLabel*> labels = descriptions->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text() == QStringLiteral("Total app storage")) {
            totalLabel = label;
            break;
        }
    }
    require(totalLabel != nullptr, "the total row label must be rendered by the descriptions");
    require((totalLabel->alignment() & Qt::AlignHorizontal_Mask) == Qt::AlignLeft,
            "row titles must be aligned left");
    QLabel* total = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-total"));
    require(total != nullptr && (total->alignment() & Qt::AlignHorizontal_Mask) == Qt::AlignRight,
            "row values must be aligned right");
}

void widgetRendersAppUsageBreakdown() {
    const settings::SettingsRegistry registry = storageStatusRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    StorageStatusSettingsWidget widget(session);

    QLabel* total = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-total"));
    QLabel* history = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-history"));
    QLabel* entries = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-entries"));
    QLabel* pinned = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-pinned"));
    QLabel* ocr = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-ocr"));
    QLabel* thumbnails =
        widget.findChild<QLabel*>(QStringLiteral("settings-status-value-thumbnails"));
    QLabel* recordingTemp =
        widget.findChild<QLabel*>(QStringLiteral("settings-status-value-recording-temp"));
    QLabel* other = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-other"));
    QLabel* location = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-location"));
    QLabel* mode = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-mode"));
    QLabel* error = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-error"));
    require(total != nullptr && history != nullptr && entries != nullptr && pinned != nullptr &&
                ocr != nullptr && thumbnails != nullptr && recordingTemp != nullptr &&
                other != nullptr && location != nullptr && mode != nullptr && error != nullptr,
            "all storage status rows must exist");

    require(total->text() == QStringLiteral("8.00 MiB"), "total usage must be formatted");
    require(history->text() == QStringLiteral("1.00 KiB"), "history usage must be formatted");
    require(entries->text() == QStringLiteral("3"), "entry count must be rendered");
    require(pinned->text() == QStringLiteral("2.00 KiB"), "pinned window usage must be formatted");
    require(ocr->text() == QStringLiteral("5.00 MiB"), "ocr usage must be formatted");
    require(thumbnails->text() == QStringLiteral("512 B"),
            "thumbnail cache usage must be formatted");
    require(recordingTemp->text() == QStringLiteral("3.00 MiB"),
            "recording temp usage must be formatted");
    require(other->text() == QStringLiteral("128 B"), "other usage must be formatted");
    require(location->text() == QStringLiteral("C:/storage-status-widget-tests"),
            "storage location must be rendered");
    require(mode->text() == QStringLiteral("Application data"), "storage mode must be rendered");
    require(error->text() == QStringLiteral("None"), "a clean status must show no error");
}

void widgetShowsScanningStateAndForwardsRefresh() {
    const settings::SettingsRegistry registry = storageStatusRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    StorageStatusSettingsWidget widget(session);
    widget.show();
    flushEvents();
    require(backend.refreshCount() >= 1,
            "showing the widget must request a storage status refresh");

    QLabel* total = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-total"));
    QAbstractButton* refresh =
        widget.findChild<QAbstractButton*>(QStringLiteral("settings-storage-status-refresh"));
    require(refresh != nullptr, "the refresh button must exist");
    require(refresh->isEnabled(), "the refresh button must be enabled when idle");

    const int refreshCount = backend.refreshCount();
    refresh->click();
    require(backend.refreshCount() == refreshCount + 1,
            "clicking refresh must be forwarded to the backend");

    storage::StorageStatus scanning = backend.storageStatus();
    scanning.appUsage.scanning = true;
    backend.publish(std::move(scanning));
    flushEvents();
    require(total->text() == QStringLiteral("Scanning…"),
            "a scanning status must replace the total usage");
    require(!refresh->isEnabled(), "the refresh button must be disabled while scanning");

    storage::StorageStatus clearing = backend.storageStatus();
    clearing.appUsage.scanning = false;
    clearing.cacheClearing = true;
    backend.publish(std::move(clearing));
    flushEvents();
    require(!refresh->isEnabled(),
            "the refresh button must be disabled while a cache clear is running");
    require(total->text() != QStringLiteral("Scanning…"),
            "a settled status must restore the total usage");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("storage_status_widget_tests"));
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "temporary storage directory should be available");
    static_cast<void>(storage::ApplicationStorage::instance().initialize(
        {storageDirectory.path(), storageDirectory.path(), 8000}));
    widgetUsesDescriptionsTitleAndThemeSpacing();
    widgetRendersAppUsageBreakdown();
    widgetShowsScanningStateAndForwardsRefresh();
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
