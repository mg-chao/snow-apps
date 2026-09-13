#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/components/storagestatussettingswidget.h"
#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/presentation/globalshortcutmanager.h"

#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/descriptions.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QClipboard>
#include <QMimeData>
#include <QFile>
#include <QElapsedTimer>
#include <QThread>
#include <QUrl>
#include <QLabel>
#include <QLayout>
#include <QTemporaryDir>
#include <QTranslator>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>

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

class ToolbarEditorTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* sourceText, const char*,
                      int) const override {
        const QString translationContext = QString::fromLatin1(context);
        const QString source = QString::fromUtf8(sourceText);
        if (translationContext == QStringLiteral("DrawingToolbarEditorSettingsWidget")) {
            if (source == QStringLiteral("Shape")) {
                return QStringLiteral("Translated drawing shape");
            }
            if (source == QStringLiteral("Drawing toolbar preview")) {
                return QStringLiteral("Translated drawing preview");
            }
        }
        if (translationContext == QStringLiteral("ScreenshotToolbarEditorSettingsWidget")) {
            if (source == QStringLiteral("Barcode recognition")) {
                return QStringLiteral("Translated barcode recognition");
            }
            if (source == QStringLiteral("Screenshot toolbar preview")) {
                return QStringLiteral("Translated screenshot preview");
            }
        }
        return {};
    }
};

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
    storage::ScreenshotToolbarLayout
    toolbarLayout(storage::ScreenshotToolbarLayoutKind) const override {
        return {};
    }
    bool applyToolbarLayout(storage::ScreenshotToolbarLayoutKind,
                            const storage::ScreenshotToolbarLayout&) override {
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
    settings::SettingsActionState
    actionState(settings::SettingsActionBinding binding) const override {
        if (binding == settings::SettingsActionBinding::CopyTodayLog)
            return {!m_status.diagnostics.exporting, m_status.diagnostics.exporting};
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
    void refreshStorageStatusIfStale() override {
        ++m_staleRefreshCount;
    }
    bool resetSection(settings::SettingsSectionReset) override {
        return false;
    }

    int refreshCount() const {
        return m_refreshCount;
    }

    int staleRefreshCount() const {
        return m_staleRefreshCount;
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
    int m_staleRefreshCount = 0;
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
    require(header != nullptr && header->layout() != nullptr, "the descriptions header must exist");
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
    require(backend.staleRefreshCount() >= 1,
            "showing the widget must request a staleness-aware storage status refresh");
    require(backend.refreshCount() == 0,
            "showing the widget must not force an unconditional rescan");

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

void pinnedToolbarSectionResetRefreshesEditor() {
    namespace layout = snow_shot::presentation::toolbar_layout;
    const auto kind = storage::ScreenshotToolbarLayoutKind::PinnedActionTools;
    presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto& registry = settings::builtInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    const auto renderer = settings::SettingsCustomRenderer::PinnedToolbarEditor;
    const auto* field = registry.fieldForCustom(renderer);
    require(field != nullptr, "pinned editor field must exist");
    std::unique_ptr<SettingsCustomWidget> editor(
        createSettingsCustomWidget(renderer, registry, *field->definition, session));
    const auto screenshotLayout =
        backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools);
    const storage::ScreenshotToolbarLayout hidden{{}, layout::defaultOrder(kind)};
    require(session.applyToolbarLayout(kind, hidden),
            "pinned editor writes must reach the real backend");
    flushEvents();
    auto* table = editor->findChild<QAbstractButton*>(
        QStringLiteral("settings-pinned-toolbar-item-table-recognition"));
    require(backend.toolbarLayout(kind) == hidden && table != nullptr &&
                !table->property("screenshotToolbarMainButton").toBool(),
            "accepted hidden layout must update the editor preview");
    require(backend.resetSection(settings::SettingsSectionReset::PinToScreen),
            "Pin to Screen section reset must succeed");
    flushEvents();
    require(backend.toolbarLayout(kind) == layout::normalizedLayout({}, kind) &&
                session.toolbarLayout(kind) == backend.toolbarLayout(kind) &&
                table->property("screenshotToolbarMainButton").toBool() &&
                backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools) ==
                    screenshotLayout,
            "section reset must restore the pinned preview without changing the screenshot layout");
}

void toolbarEditorsUseSeparateDefinitionsAndRetranslate() {
    const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    const auto createEditor = [&](settings::SettingsCustomRenderer renderer) {
        const settings::SettingsFieldDescriptor* field = registry.fieldForCustom(renderer);
        require(field != nullptr && field->definition != nullptr,
                "the built-in toolbar editor field must exist");
        return std::unique_ptr<SettingsCustomWidget>(
            createSettingsCustomWidget(renderer, registry, *field->definition, session));
    };

    std::unique_ptr<SettingsCustomWidget> drawingEditor =
        createEditor(settings::SettingsCustomRenderer::DrawingToolbarEditor);
    std::unique_ptr<SettingsCustomWidget> screenshotEditor =
        createEditor(settings::SettingsCustomRenderer::ScreenshotToolbarEditor);
    require(drawingEditor != nullptr && screenshotEditor != nullptr,
            "both custom toolbar renderers must create an editor");

    QAbstractButton* shape = drawingEditor->findChild<QAbstractButton*>(
        QStringLiteral("settings-drawing-toolbar-item-shape"));
    QAbstractButton* barcode = screenshotEditor->findChild<QAbstractButton*>(
        QStringLiteral("settings-screenshot-toolbar-item-barcode-recognition"));
    QAbstractButton* table = screenshotEditor->findChild<QAbstractButton*>(
        QStringLiteral("settings-screenshot-toolbar-item-table-recognition"));
    QWidget* drawingPreview =
        drawingEditor->findChild<QWidget*>(QStringLiteral("settings-drawing-toolbar-surface"));
    QWidget* screenshotPreview = screenshotEditor->findChild<QWidget*>(
        QStringLiteral("settings-screenshot-toolbar-surface"));
    require(shape != nullptr && barcode != nullptr && table != nullptr &&
                drawingPreview != nullptr && screenshotPreview != nullptr &&
                drawingEditor->findChild<QAbstractButton*>(QStringLiteral(
                    "settings-drawing-toolbar-item-barcode-recognition")) == nullptr &&
                screenshotEditor->findChild<QAbstractButton*>(
                    QStringLiteral("settings-screenshot-toolbar-item-shape")) == nullptr,
            "drawing and screenshot editors must expose only their own stable tool definitions");
    require(!barcode->property("screenshotToolbarMainButton").toBool() &&
                table->property("screenshotToolbarMainButton").toBool(),
            "the default screenshot preview must stack Barcode below the visible Table trigger");

    auto pinnedEditor = createEditor(settings::SettingsCustomRenderer::PinnedToolbarEditor);
    require(pinnedEditor != nullptr, "the pinned renderer must reuse the toolbar editor");
    for (const QString& id :
         {QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
          QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html"),
          QStringLiteral("text-recognition"), QStringLiteral("text-translation")}) {
        require(pinnedEditor->findChild<QAbstractButton*>(
                    QStringLiteral("settings-pinned-toolbar-item-%1").arg(id)) != nullptr,
                "the pinned editor must expose each of its six tools");
    }
    require(pinnedEditor->findChild<QAbstractButton*>(
                QStringLiteral("settings-pinned-toolbar-item-save-as-file")) == nullptr &&
                pinnedEditor->findChild<QAbstractButton*>(
                    QStringLiteral("settings-pinned-toolbar-item-record-screen")) == nullptr,
            "the pinned editor must not offer fixed or screenshot-only actions");
    auto* pinnedTable = pinnedEditor->findChild<QAbstractButton*>(
        QStringLiteral("settings-pinned-toolbar-item-table-recognition"));
    require(pinnedTable->property("screenshotToolbarMainButton").toBool(),
            "pinned preview must show Table as the default stack entry");

    ToolbarEditorTranslator translator;
    require(QCoreApplication::installTranslator(&translator),
            "the toolbar editor test translator must install");
    QEvent pinnedLanguageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(pinnedEditor.get(), &pinnedLanguageChange);
    require(pinnedEditor
                    ->findChild<QAbstractButton*>(
                        QStringLiteral("settings-pinned-toolbar-item-barcode-recognition"))
                    ->accessibleName() == QStringLiteral("Translated barcode recognition"),
            "pinned tool labels must retranslate on language change");
    QEvent drawingLanguageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(drawingEditor.get(), &drawingLanguageChange);
    QEvent screenshotLanguageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(screenshotEditor.get(), &screenshotLanguageChange);
    require(shape->accessibleName() == QStringLiteral("Translated drawing shape") &&
                barcode->accessibleName() == QStringLiteral("Translated barcode recognition") &&
                drawingPreview->accessibleName() == QStringLiteral("Translated drawing preview") &&
                screenshotPreview->accessibleName() ==
                    QStringLiteral("Translated screenshot preview"),
            "LanguageChange must refresh labels and accessibility text in both toolbar editors");
    QCoreApplication::removeTranslator(&translator);
}

void diagnosticsStateAndCopyFeedback() {
    const auto registry = storageStatusRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    StorageStatusSettingsWidget widget(session);
    auto status = backend.storageStatus();
    status.diagnostics.directory = QStringLiteral("C:/very-long-storage-directory/").repeated(10);
    status.diagnostics.loggingAvailable = true;
    status.diagnostics.exporting = true;
    status.appUsage.diagnosticsBytes = 2048;
    status.lastHistoryError = QStringLiteral("history error");
    status.diagnostics.lastError = QStringLiteral("collector error");
    backend.publish(status);
    flushEvents();
    auto* location =
        widget.findChild<QLabel*>(QStringLiteral("settings-status-value-log-location"));
    auto* usage = widget.findChild<QLabel*>(QStringLiteral("settings-status-value-diagnostics"));
    auto* copy =
        widget.findChild<QAbstractButton*>(QStringLiteral("settings-storage-copy-today-log"));
    require(location && location->text() == status.diagnostics.directory && location->wordWrap(),
            "log path wraps without shortening");
    require(location->textInteractionFlags().testFlag(Qt::TextSelectableByMouse),
            "log path selectable");
    require(usage && usage->text() == QStringLiteral("2.00 KiB"), "diagnostics bytes visible");
    require(copy && !copy->isEnabled(), "repeat activation blocked during export");
    status.diagnostics.exporting = false;
    backend.publish(status);
    flushEvents();
    require(copy->isEnabled(), "copy enabled after completion");
    emit backend.actionFinished(settings::SettingsActionBinding::CopyTodayLog, true, {});
    auto* feedback =
        widget.findChild<QLabel*>(QStringLiteral("settings-storage-log-copy-feedback"));
    require(feedback && feedback->text() == QStringLiteral("Log file copied."),
            "async completion reaches widget");
    QEvent language(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&widget, &language);
    require(!copy->accessibleName().isEmpty(), "copy action has an accessible translated name");
}
void copyPublishesStableFileAndPreservesClipboardOnFailure() {
    QTemporaryDir directory;
    auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
    snow_shot::diagnostics::DiagnosticsOptions options;
    options.directories = {directory.path()};
    options.enableCrashCapture = false;
    options.installMessageHandler = false;
    require(diagnostics.initialize(options), "clipboard diagnostics initialize");
    presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    int completions = 0;
    bool succeeded = false;
    QObject::connect(&backend, &settings::SettingsBackend::actionFinished, &backend,
                     [&](settings::SettingsActionBinding, bool success, const QString&) {
                         ++completions;
                         succeeded = success;
                     });
    const auto copy = settings::SettingsActionBinding::CopyTodayLog;
    require(backend.triggerAction(copy), "copy action starts");
    require(!backend.triggerAction(copy), "concurrent copy rejected");
    QElapsedTimer timer;
    timer.start();
    while (completions == 0 && timer.elapsed() < 5000) {
        flushEvents();
        QThread::msleep(5);
    }
    require(completions == 1 && succeeded, "copy completes successfully");
    const auto urls = QApplication::clipboard()->mimeData()->urls();
    require(urls.size() == 1 && urls.front().isLocalFile(), "copy publishes a file attachment");
    QFile snapshot(urls.front().toLocalFile());
    require(snapshot.open(QIODevice::ReadOnly), "copied attachment exists");
    const auto content = snapshot.readAll();
    snapshot.close();
    diagnostics.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("after.copy"));
    require(diagnostics.flush(), "later records flushed");
    require(snapshot.open(QIODevice::ReadOnly) && snapshot.readAll() == content,
            "attachment is immutable");
    snapshot.close();
    diagnostics.shutdown();
    QTemporaryDir failedDirectory;
    options.directories = {failedDirectory.path()};
    require(diagnostics.initialize(options), "failed-export logger starts");
    QFile blocker(QDir(failedDirectory.path()).filePath(QStringLiteral("exports")));
    require(blocker.open(QIODevice::WriteOnly), "create export failure fixture");
    blocker.close();
    require(backend.triggerAction(copy), "failing export starts");
    timer.restart();
    while (completions < 2 && timer.elapsed() < 5000) {
        flushEvents();
        QThread::msleep(5);
    }
    require(completions == 2 && !succeeded, "export failure reported");
    require(QApplication::clipboard()->mimeData()->urls() == urls,
            "failed export preserves previous clipboard");
    diagnostics.shutdown();
    QApplication::clipboard()->clear();
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
    toolbarEditorsUseSeparateDefinitionsAndRetranslate();
    pinnedToolbarSectionResetRefreshesEditor();
    diagnosticsStateAndCopyFeedback();
    copyPublishesStableFileAndPreservesClipboardOnFailure();
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
