#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationarchive.h"

#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/components/sectionheaderwidget.h"
#include "widgets/button.h"
#include "widgets/popconfirm.h"
#include "widgets/switch.h"
#include "theme/theme_manager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QMouseEvent>
#include <QDir>
#include <QHash>
#include <QGridLayout>
#include <QImage>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void clickWidget(QWidget* button) {
    const QPointF local = button->rect().center();
    const QPointF global = button->mapToGlobal(local.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(button, &press);
    QApplication::sendEvent(button, &release);
}
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool thumbIsOnRight(adqt::widgets::AdSwitch* toggle) {
    const QImage image = toggle->grab().toImage();
    int left = 0;
    int right = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == QColor(Qt::magenta)) {
                (x < image.width() / 2 ? left : right)++;
            }
        }
    }
    require(left + right > 0, "rendered switch contains the test thumb color");
    return right > left;
}
class FakeTranslationHotkeyBackend final : public snow_shot::presentation::GlobalShortcutBackend {
  public:
    void setActivationHandler(ActivationHandler value) override {
        handler = std::move(value);
    }
    snow_shot::presentation::GlobalShortcutValidationResult
    validateShortcut(const snow_shot::shortcuts::ShortcutBinding& shortcut) const override {
        return {shortcut.portableText, true,
                snow_shot::presentation::GlobalShortcutFailureReason::None, shortcut};
    }
    snow_shot::presentation::GlobalShortcutBackendResult
    registerShortcut(int id, const snow_shot::shortcuts::ShortcutBinding& shortcut) override {
        if (registrations.values().contains(shortcut)) {
            return {false, snow_shot::presentation::GlobalShortcutFailureReason::AlreadyInUse};
        }
        registrations.insert(id, shortcut);
        return {true};
    }
    void unregisterShortcut(int id) override {
        registrations.remove(id);
    }
    ActivationHandler handler;
    QHash<int, snow_shot::shortcuts::ShortcutBinding> registrations;
};

void requireCompactTrayOptions(QWidget* widget) {
    auto* options = widget->findChild<QWidget*>(QStringLiteral("settings-tray-menu-options-grid"));
    auto* grid = qobject_cast<QGridLayout*>(options->layout());
    require(grid != nullptr, "tray options use a grid");
    widget->resize(1000, widget->sizeHint().height());
    widget->show();
    QCoreApplication::processEvents();
    int row = 0;
    bool firstGroup = true;
    for (const auto& group :
         snow_shot::presentation::settings::builtInSettingsRegistry().catalog().trayMenuGroups()) {
        int visibleCount = 0;
        for (const auto& option : group.options) {
            auto* checkbox = widget->findChild<QAbstractButton*>(
                QStringLiteral("settings-tray-menu-option-%1").arg(option.id));
            require(checkbox != nullptr, "tray option retains its widget");
            if (checkbox->isHidden()) {
                continue;
            }
            if (visibleCount == 0 && !firstGroup) {
                ++row; // Group divider.
            }
            int actualRow = -1;
            int column = -1;
            int rowSpan = 0;
            int columnSpan = 0;
            grid->getItemPosition(grid->indexOf(checkbox), &actualRow, &column, &rowSpan,
                                  &columnSpan);
            require(actualRow == row + visibleCount / 2 && column == visibleCount % 2,
                    "visible tray options fill consecutive cells without hidden-option gaps");
            ++visibleCount;
        }
        if (visibleCount > 0) {
            row += (visibleCount + 1) / 2;
            firstGroup = false;
        }
    }
}

void selectedTextShortcutSettings() {
    using namespace snow_shot::presentation;
    namespace storage = snow_shot::storage;
    const auto action = GlobalShortcutAction::TranslateSelectedText;
    const storage::ShortcutSettings persisted;
    require(persisted.translateSelectedText().isEmpty(),
            "selected text shortcut is unassigned by default");
    const storage::TraySettings tray;
    const QStringList defaultMenu = tray.menuOptions();
    const QString menuId = QStringLiteral("quick.translate-selected-text");
    require(!defaultMenu.contains(menuId), "selected text translation is optional in the tray");
    QStringList menuWithTranslation = defaultMenu;
    menuWithTranslation.append(menuId);
    require(tray.setMenuOptions(menuWithTranslation) && tray.menuOptions().contains(menuId),
            "tray settings allow the selected text translation action");
    require(tray.setMenuOptions(defaultMenu), "restore default tray actions");
    const snow_shot::shortcuts::ShortcutBindingList keys{QStringLiteral("Ctrl+Alt+T"),
                                                         QStringLiteral("Ctrl+Shift+T")};
    {
        auto native = std::make_unique<FakeTranslationHotkeyBackend>();
        auto* input = native.get();
        bool fullscreen = false;
        GlobalShortcutManager manager(std::move(native), nullptr, [&]() { return fullscreen; });
        settings::BuiltInSettingsBackend backend(manager);
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        manager.initialize();
        const auto standaloneBinding = settings::SettingsSwitchBinding::StandaloneTranslationWindow;
        const QString standaloneId =
            QStringLiteral("extended-features.standalone-translation-window");
        const auto jumpBinding = settings::SettingsSwitchBinding::JumpToTranslationPage;
        const QString jumpId = QStringLiteral("extended-features.jump-to-translation-page");
        require(!storage::ExtendedFeaturesSettings().standaloneTranslationWindow() &&
                    session.state(standaloneId).visible && !session.state(standaloneId).enabled &&
                    !backend.applySwitchValue(standaloneBinding, true),
                "standalone defaults off and is visible but disabled under master off");
        require(!storage::ExtendedFeaturesSettings().jumpToTranslationPage() &&
                    session.state(jumpId).visible && !session.state(jumpId).enabled &&
                    !backend.applySwitchValue(jumpBinding, true),
                "OCR translation jump defaults off and is disabled under master off");
        require(manager.state(action).status == GlobalShortcutStatus::Unset,
                "unassigned action does not register a native shortcut");
        require(!storage::ExtendedFeaturesSettings().translationPageEnabled(),
                "translation page defaults off");
        require(!session.state(QStringLiteral("quick.translate-selected-text")).visible,
                "selected text shortcut is hidden while disabled");
        require(tray.setMenuOptions(menuWithTranslation),
                "preserve selected translation tray preference");
        QCoreApplication::processEvents();
        const auto& registry = settings::builtInSettingsRegistry();
        std::unique_ptr<SettingsCustomWidget> trayWidget(createSettingsCustomWidget(
            settings::SettingsCustomRenderer::TrayMenuOptions, registry,
            *registry.field(QStringLiteral("tray.menu-options"))->definition, session));
        auto* translationCheckbox = trayWidget->findChild<QAbstractButton*>(
            QStringLiteral("settings-tray-menu-option-quick.translate-selected-text"));
        auto* screenshotCheckbox = trayWidget->findChild<QAbstractButton*>(
            QStringLiteral("settings-tray-menu-option-quick.screenshot"));
        require(translationCheckbox != nullptr && screenshotCheckbox != nullptr &&
                    translationCheckbox->isHidden() && translationCheckbox->isChecked(),
                "disabled tray customization hides translation but retains its checked preference");
        requireCompactTrayOptions(trayWidget.get());
        screenshotCheckbox->setChecked(!screenshotCheckbox->isChecked());
        require(tray.menuOptions().contains(menuId),
                "editing another tray option preserves the hidden translation preference");
        manager.setShortcuts(action, keys);
        require(persisted.translateSelectedText() == keys &&
                    !input->registrations.values().contains(keys.first()),
                "disabled feature preserves configured keys without native registration");
        require(
            backend.applySwitchValue(settings::SettingsSwitchBinding::TranslationPageEnabled, true),
            "enable translation page through settings backend");
        QCoreApplication::processEvents();
        require(backend.applySwitchValue(jumpBinding, true),
                "enable OCR translation jump before category reset");
        require(!translationCheckbox->isHidden() && translationCheckbox->isChecked(),
                "live enabling restores the selected tray customization checkbox");
        requireCompactTrayOptions(trayWidget.get());
        require(tray.setMenuOptions(defaultMenu),
                "restore tray configuration after feature checks");
        require(session.state(QStringLiteral("quick.translate-selected-text")).visible,
                "enabling feature reveals shortcut controls");
        require(session.applyShortcuts(action, keys) && persisted.translateSelectedText() == keys &&
                    manager.state(action).status == GlobalShortcutStatus::Registered,
                "editing selected text bindings updates storage and native registrations");
        {
            SettingsPageWidget page(registry, QStringLiteral("text-recognition-translation"),
                                    session);
            page.resize(880, 760);
            page.show();
            page.reveal({page.pageId(), QStringLiteral("translation"),
                         QStringLiteral("extended-features.translation-page")});
            QCoreApplication::processEvents();
            auto* header = page.findChild<SectionHeaderWidget*>(
                QStringLiteral("settings-section-text-recognition-translation-translation"));
            require(header != nullptr, "translation section exposes its own header");
            auto* reset =
                header->findChild<adqt::widgets::AdButton*>(QStringLiteral("sectionResetButton"));
            auto* confirmation = header->findChild<adqt::widgets::AdPopconfirm*>();
            auto* toggle = page.findChild<adqt::widgets::AdSwitch*>(
                QStringLiteral("settings-control-extended-features-translation-page"));
            require(reset != nullptr && reset->isVisible() && reset->isEnabled() &&
                        confirmation != nullptr && toggle != nullptr && toggle->isChecked() &&
                        reset->geometry().right() == header->contentsRect().right(),
                    "extended category uses the shared right-aligned reset and generated toggle");
            adqt::widgets::AdSwitch::ComponentTokens tokens;
            tokens.colors.thumb = QColor(Qt::magenta);
            toggle->setComponentTokens(tokens);
            require(thumbIsOnRight(toggle), "enabled translation renders the thumb on the right");
            clickWidget(reset);
            QCoreApplication::processEvents();
            require(confirmation->isVisible(), "extended reset opens shared confirmation");
            confirmation->button(adqt::widgets::AdPopconfirm::StandardButton::Cancel)->click();
            require(storage::ExtendedFeaturesSettings().translationPageEnabled() &&
                        storage::ExtendedFeaturesSettings().jumpToTranslationPage(),
                    "cancel extended category reset preserves feature opt-ins");
            clickWidget(reset);
            QCoreApplication::processEvents();
            confirmation->button(adqt::widgets::AdPopconfirm::StandardButton::Ok)->click();
            QCoreApplication::processEvents();
            require(!storage::ExtendedFeaturesSettings().translationPageEnabled() &&
                        !storage::ExtendedFeaturesSettings().jumpToTranslationPage() &&
                        !toggle->isChecked() &&
                        !session.state(QStringLiteral("extended-features.translation-page"))
                             .acceptedValue.toBool() &&
                        !session.state(QStringLiteral("quick.translate-selected-text")).visible &&
                        !input->registrations.values().contains(keys.first()) &&
                        persisted.translateSelectedText() == keys,
                    "reset disables translation, refreshes UI and hotkeys, and preserves bindings");
            require(!thumbIsOnRight(toggle), "reset moves the translation thumb to the left");
            requireCompactTrayOptions(trayWidget.get());
            for (const bool enabled : {true, false, true}) {
                clickWidget(toggle);
                QCoreApplication::processEvents();
                require(toggle->isChecked() == enabled && thumbIsOnRight(toggle) == enabled &&
                            storage::ExtendedFeaturesSettings().translationPageEnabled() == enabled,
                        "clicking translation after reset updates both rendering and storage");
                requireCompactTrayOptions(trayWidget.get());
            }
            QCoreApplication::processEvents();
        }
        require(session.state(standaloneId).enabled && session.state(jumpId).enabled,
                "master on enables both dependent translation switches live");
        require(backend.applySwitchValue(jumpBinding, true) &&
                    storage::ExtendedFeaturesSettings().jumpToTranslationPage(),
                "OCR translation jump persists through the settings backend");
        for (const bool enabled : {true, false, true}) {
            require(backend.applySwitchValue(standaloneBinding, enabled) &&
                        storage::ExtendedFeaturesSettings().standaloneTranslationWindow() ==
                            enabled,
                    "standalone switch persists through backend");
            QCoreApplication::processEvents();
            require(manager.state(action).status == GlobalShortcutStatus::Registered &&
                        session.state(QStringLiteral("quick.translate-selected-text")).visible &&
                        !translationCheckbox->isHidden(),
                    "standalone flag never gates shortcut or tray availability");
        }
        int activations = 0;
        QObject::connect(
            &manager, &GlobalShortcutManager::activated, &manager,
            [&](GlobalShortcutAction activated) { activations += activated == action; });
        int id = input->registrations.key(keys.first());
        require(id != 0, "selected text shortcut has a native registration");
        input->handler(id);
        require(activations == 1, "native activation dispatches selected text translation");
        require(storage::ExtendedFeaturesSettings().setTranslationPageEnabled(false),
                "disable feature live");
        QCoreApplication::processEvents();
        require(!session.state(standaloneId).enabled && !session.state(jumpId).enabled &&
                    storage::ExtendedFeaturesSettings().standaloneTranslationWindow() &&
                    storage::ExtendedFeaturesSettings().jumpToTranslationPage(),
                "master off disables child controls without clearing either preference");
        input->handler(id);
        require(activations == 1 && !input->registrations.values().contains(keys.first()) &&
                    persisted.translateSelectedText() == keys,
                "disable unregisters and suppresses stale activation without deleting keys");
        require(storage::ExtendedFeaturesSettings().setTranslationPageEnabled(true),
                "re-enable feature live");
        require(input->registrations.values().contains(keys.first()),
                "re-enable restores native registration");
        id = input->registrations.key(keys.first());
        manager.setGlobalHotkeysEnabled(false);
        input->handler(id);
        require(activations == 1, "disabled global hotkeys suppress selected text translation");
        manager.setGlobalHotkeysEnabled(true);
        auto& store = storage::ApplicationStorage::instance().configuration();
        const QString fullscreenKey =
            QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window");
        const auto previousFullscreen = store.value(fullscreenKey);
        require(store.setValue(fullscreenKey, true), "enable fullscreen suppression");
        fullscreen = true;
        input->handler(id);
        require(activations == 1, "fullscreen suppression applies to selected text translation");
        fullscreen = false;
        require(store.setValue(fullscreenKey, previousFullscreen), "restore fullscreen preference");
        const auto conflict = persisted.pinClipboardContent();
        require(!conflict.isEmpty(), "pin clipboard default provides a conflict fixture");
        manager.setShortcuts(action, {conflict.first()});
        require(manager.state(action).status == GlobalShortcutStatus::Failed &&
                    manager.state(action).bindings.first().failureReason ==
                        GlobalShortcutFailureReason::AlreadyInUse,
                "selected text shortcut reports native conflicts through existing status");
        require(session.reset(settings::SettingsSectionReset::OtherShortcuts) &&
                    persisted.translateSelectedText().isEmpty() &&
                    manager.state(action).status == GlobalShortcutStatus::Unset,
                "Other reset clears selected text bindings and unregisters them");
        require(session.applyShortcuts(action, keys), "prepare selected text bindings for reload");
    }
    {
        GlobalShortcutManager reloaded(std::make_unique<FakeTranslationHotkeyBackend>(), nullptr,
                                       []() { return false; });
        reloaded.initialize();
        require(reloaded.state(action).shortcuts == keys &&
                    reloaded.state(action).status == GlobalShortcutStatus::Registered,
                "a recreated shortcut manager loads both persisted selected text bindings");
        reloaded.setShortcuts(action, {});
    }
}
void originalImagePreviewDefaultsPersistsAndResets() {
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const storage::TextRecognitionSettings recognition;
    constexpr auto binding = settings::SettingsSwitchBinding::ShowOriginalImagePreview;
    const QString key = QStringLiteral("text_recognition/show_original_image_preview");
    const QString itemId = QStringLiteral("text-recognition.show-original-image-preview");
    require(storage::ConfigurationSchema::defaultValue(key).toBool() &&
                backend.switchEnabled(binding) && backend.switchValue(binding) &&
                recognition.showOriginalImagePreview(),
            "original image preview must default on through schema, backend and typed settings");
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    require(session.state(itemId).acceptedValue.toBool() && session.state(itemId).enabled,
            "original image preview is an enabled function settings switch");
    require(session.submitDraft(itemId, false) && !backend.switchValue(binding) &&
                !recognition.showOriginalImagePreview() &&
                applicationStorage.configuration().flushNow().success,
            "the preview switch must persist a disabled value through the runtime session");
    storage::ConfigurationStore reloaded(
        QDir(applicationStorage.configurationDirectory()).filePath(QStringLiteral("config.json")),
        true, true, 60000);
    require(!reloaded.value(key).toBool(), "disabled preview survives configuration reload");
    const QString archivePath =
        QDir(applicationStorage.configurationDirectory()).filePath(QStringLiteral("preview.zip"));
    require(storage::ConfigurationArchive::write(
                archivePath, {{key, false}}, storage::ConfigurationStore::currentSchemaVersion())
                .isEmpty(),
            "the preview preference must be exportable in configuration archives");
    const auto imported = storage::ConfigurationArchive::read(archivePath);
    require(imported.isValid() && imported.values.contains(key) &&
                !imported.values.value(key).toBool() &&
                reloaded.applySnapshot(imported.values, imported.schemaVersion) &&
                !reloaded.value(key).toBool(),
            "imported configuration restores the disabled preview preference");
    require(reloaded.applySnapshot({{QStringLiteral("text_recognition/default_formatting"),
                                     QStringLiteral("keep")}}) &&
                reloaded.value(key).toBool(),
            "older configuration snapshots missing the preference use the enabled default");
    require(!applicationStorage.configuration().setValue(key, QStringLiteral("false")) &&
                !recognition.showOriginalImagePreview(),
            "the preview setting must reject nonboolean writes without changing the preference");
    require(session.reset(settings::SettingsSectionReset::TextRecognitionBehavior) &&
                backend.switchValue(binding) && recognition.showOriginalImagePreview(),
            "resetting Text Recognition behavior restores the enabled preview default");
    require(recognition.setShowOriginalImagePreview(false), "typed preview setter disables");
    session.refreshAll();
    require(!session.state(itemId).acceptedValue.toBool(),
            "externally changed preview preference refreshes the settings switch");
    require(recognition.setShowOriginalImagePreview(true), "restore enabled preview default");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    auto themeConfig = adqt::theme::ThemeManager::instance().config();
    themeConfig.motion = false;
    adqt::theme::ThemeManager::instance().setConfig(themeConfig);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated translation settings storage");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create translation settings executable directory");
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success,
            "initialize translation settings storage");
    if (application.arguments().contains(QStringLiteral("--original-image-preview-only"))) {
        originalImagePreviewDefaultsPersistsAndResets();
        applicationStorage.shutdown();
        return 0;
    }
    originalImagePreviewDefaultsPersistsAndResets();
    selectedTextShortcutSettings();
    {
        snow_shot::presentation::GlobalShortcutManager shortcuts;
        settings::BuiltInSettingsBackend backend(shortcuts);
        require(storage::RecordingSettings().setMicrophoneGainDb(-12) &&
                    storage::RecordingSettings().setSystemAudioGainDb(9) &&
                    backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                    storage::RecordingSettings().microphoneGainDb() == 0 &&
                    storage::RecordingSettings().systemAudioGainDb() == 0,
                "recording section reset restores both gains to unity");
        const auto separateAudio = settings::SettingsSwitchBinding::SeparateRecordingAudioTracks;
        require(backend.switchEnabled(separateAudio) && !backend.switchValue(separateAudio) &&
                    backend.applySwitchValue(separateAudio, true) &&
                    storage::RecordingSettings().separateAudioTracks() &&
                    backend.switchValue(separateAudio),
                "separate audio tracks default off and persist enabled");
        require(backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                    !backend.switchValue(separateAudio) &&
                    !storage::RecordingSettings().separateAudioTracks(),
                "screen recording reset restores mixed audio");
        const storage::RecordingSettings recording;
        require(recording.setPostProcessingEnabled(true) &&
                    recording.setPostProcessingEffect(QStringLiteral("playback_time")) &&
                    recording.setProgressBarColor(QColor(12, 34, 56, 78)) &&
                    backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                    !recording.postProcessingEnabled() &&
                    recording.postProcessingEffect() == QStringLiteral("progress_bar") &&
                    recording.progressBarColor() == QColor(22, 119, 255),
                "screen recording reset restores automatic real-time mode and overlay defaults");
        const auto loopImages = settings::SettingsSwitchBinding::LoopAnimatedImages;
        require(backend.switchEnabled(loopImages) && backend.switchValue(loopImages) &&
                    backend.applySwitchValue(loopImages, false) &&
                    !storage::RecordingSettings().loopAnimatedImages() &&
                    !backend.switchValue(loopImages),
                "animated image loop switch defaults on and persists disabled");
        require(backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                    backend.switchValue(loopImages),
                "screen recording reset restores animated image looping");
        const auto recognitionSave = settings::SettingsSwitchBinding::SaveRecognitionResultAsImage;
        require(backend.switchValue(recognitionSave), "recognition image export defaults on");
        require(backend.applySwitchValue(recognitionSave, false) &&
                    !storage::TextRecognitionSettings().saveRecognitionResultAsImage(),
                "recognition image export persists disabled setting");
        const auto defaultFormatting = settings::SettingsSelectBinding::OcrDefaultFormatting;
        const auto defaultPunctuation = settings::SettingsSelectBinding::OcrDefaultPunctuation;
        require(backend.selectValue(defaultFormatting).toString() == QStringLiteral("none") &&
                    backend.selectValue(defaultPunctuation).toString() == QStringLiteral("none") &&
                    backend.applySelectValue(defaultFormatting, QStringLiteral("remove")) &&
                    backend.applySelectValue(defaultPunctuation, QStringLiteral("full")) &&
                    storage::TextRecognitionSettings().defaultFormatting() ==
                        QStringLiteral("remove") &&
                    storage::TextRecognitionSettings().defaultPunctuation() ==
                        QStringLiteral("full"),
                "recognized-text defaults persist through the settings backend");
        require(backend.applySelectValue(defaultFormatting, QStringLiteral("smart")) &&
                    backend.selectValue(defaultFormatting).toString() == QStringLiteral("smart") &&
                    storage::TextRecognitionSettings().defaultFormatting() ==
                        QStringLiteral("smart"),
                "Smart Typesetting persists through the settings backend and schema");
        const auto oldFill =
            applicationStorage.configuration().value(QStringLiteral("text_recognition/fill_style"));
        require(applicationStorage.configuration().setValue(
                    QStringLiteral("text_recognition/fill_style"), QStringLiteral("blur")) &&
                    backend.resetSection(settings::SettingsSectionReset::TextRecognitionBehavior) &&
                    backend.switchValue(recognitionSave) &&
                    backend.selectValue(defaultFormatting).toString() == QStringLiteral("none") &&
                    backend.selectValue(defaultPunctuation).toString() == QStringLiteral("none") &&
                    applicationStorage.configuration().value(
                        QStringLiteral("text_recognition/fill_style")) == QStringLiteral("blur"),
                "recognition behavior reset restores defaults without changing appearance");
        require(applicationStorage.configuration().setValue(
                    QStringLiteral("text_recognition/fill_style"), oldFill),
                "restore recognition appearance fixture");

        require(!applicationStorage.configuration()
                        .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                        .toBool() &&
                    !backend.switchValue(settings::SettingsSwitchBinding::DirectMlAcceleration),
                "DirectML acceleration should be disabled by default");
        const auto binding = settings::SettingsSwitchBinding::OriginalImageTranslation;
        const storage::ScreenshotTranslationSettings translation;
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        const auto resident = settings::SettingsSwitchBinding::OcrResidentProcess;
        const auto hot = settings::SettingsSwitchBinding::OcrModelHotStart;
        const QString hotId = QStringLiteral("text-recognition.model-hot-start");
        const QString residentId = QStringLiteral("text-recognition.resident-process");
        require(!backend.switchValue(resident) && !backend.switchValue(hot) &&
                    !session.state(hotId).enabled && session.state(residentId).enabled,
                "OCR residency settings default off and hot start is disabled");
        require(backend.applySwitchValue(resident, true) && backend.applySwitchValue(hot, true),
                "OCR settings must persist without attempting resource preparation");
        QCoreApplication::processEvents();
        require(session.state(hotId).enabled && session.state(hotId).acceptedValue.toBool(),
                "enabling residency must enable hot start immediately");
        require(backend.applySwitchValue(resident, false), "resident preference can be disabled");
        QCoreApplication::processEvents();
        require(!session.state(hotId).enabled && session.state(hotId).acceptedValue.toBool() &&
                    backend.switchValue(hot) && session.state(hotId).error.isEmpty() &&
                    session.state(residentId).error.isEmpty(),
                "resident-off retains the checked hot-start value without a settings failure");
        require(backend.applySwitchValue(resident, true), "resident preference can be restored");
        QCoreApplication::processEvents();
        require(session.state(hotId).enabled && session.state(hotId).acceptedValue.toBool(),
                "restoring residency restores the saved hot-start preference");
        bool resetObserved = false;
        auto resetConnection = QObject::connect(
            &applicationStorage.configuration(), &storage::ConfigurationStore::valueChanged,
            &application, [&](const QString& key, const QJsonValue&) {
                if (key == QStringLiteral("text_recognition/resident_process") ||
                    key == QStringLiteral("text_recognition/model_hot_start")) {
                    resetObserved = true;
                    require(!backend.switchValue(resident) && !backend.switchValue(hot),
                            "all observers must see both values reset atomically");
                }
            });
        require(backend.resetSection(settings::SettingsSectionReset::TextRecognition),
                "OCR category reset must succeed");
        QObject::disconnect(resetConnection);
        QCoreApplication::processEvents();
        require(resetObserved && !backend.switchValue(resident) && !backend.switchValue(hot) &&
                    !session.state(hotId).enabled,
                "OCR category reset must clear both preferences and refresh the disabled control");
        const auto pinBinding = settings::SettingsSelectBinding::PinDoubleClickAction;
        const QString pinId = QStringLiteral("pin-to-screen.double-click-action");
        require(backend.selectValue(pinBinding).toString() == QStringLiteral("thumbnail_mode") &&
                    session.state(pinId).enabled,
                "pinned double-click defaults to the enabled thumbnail selector");
        for (const QString& action :
             {QStringLiteral("none"), QStringLiteral("thumbnail_mode"), QStringLiteral("close")}) {
            require(backend.applySelectValue(pinBinding, action) &&
                        backend.selectValue(pinBinding).toString() == action,
                    "pinned double-click backend must apply every option");
        }
        require(!backend.applySelectValue(pinBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(pinBinding).toString() == QStringLiteral("close"),
                "pinned double-click backend must reject invalid actions");
        require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                    backend.selectValue(pinBinding).toString() == QStringLiteral("thumbnail_mode"),
                "resetting pin behavior must restore thumbnail double-click");
        const auto middleBinding = settings::SettingsSelectBinding::PinMiddleClickAction;
        const QString middleId = QStringLiteral("pin-to-screen.middle-mouse-button-action");
        require(backend.selectValue(middleBinding).toString() == QStringLiteral("reset_zoom") &&
                    session.state(middleId).enabled,
                "pinned middle-click defaults to the enabled reset zoom selector");
        for (const QString& action : {QStringLiteral("none"), QStringLiteral("reset_zoom"),
                                      QStringLiteral("thumbnail_mode"), QStringLiteral("close")}) {
            require(backend.applySelectValue(middleBinding, action) &&
                        backend.selectValue(middleBinding).toString() == action,
                    "pinned middle-click backend must apply every option");
        }
        require(!backend.applySelectValue(middleBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(middleBinding).toString() == QStringLiteral("close"),
                "pinned middle-click backend must reject invalid actions");
        require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                    backend.selectValue(middleBinding).toString() == QStringLiteral("reset_zoom"),
                "resetting pin behavior must restore reset zoom middle-click");
        const auto fillBinding = settings::SettingsSelectBinding::OcrFillStyle;
        require(backend.selectValue(fillBinding).toString() == QStringLiteral("background_fill"),
                "OCR fill defaults to Background Fill");
        require(backend.applySelectValue(fillBinding, QStringLiteral("blur")) &&
                    backend.selectValue(fillBinding).toString() == QStringLiteral("blur"),
                "OCR fill selection must persist");
        require(!backend.applySelectValue(fillBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(fillBinding).toString() == QStringLiteral("blur"),
                "unsupported fill styles must not replace the saved choice");
        require(backend.resetSection(
                    settings::SettingsSectionReset::TextRecognitionInterfaceSettings) &&
                    backend.selectValue(fillBinding).toString() ==
                        QStringLiteral("background_fill"),
                "resetting Text Recognition appearance restores Background Fill");
        const auto layoutBinding = settings::SettingsSelectBinding::TranslationLayoutProcessing;
        const QString layoutId = QStringLiteral("translation.layout-processing");
        require(backend.selectValue(layoutBinding).toString() == QStringLiteral("smart_merge") &&
                    session.state(layoutId).enabled,
                "Smart Merge is the enabled default");
        require(backend.applySelectValue(layoutBinding, QStringLiteral("original")),
                "set Original layout");
        require(backend.applySwitchValue(binding, false), "disable original-image translation");
        session.refreshAll();
        require(!session.state(layoutId).enabled &&
                    translation.layoutProcessing() == QStringLiteral("original"),
                "disabled layout selector retains its choice");
        require(!backend.applySelectValue(layoutBinding, QStringLiteral("unsupported")),
                "reject unknown mode");
        require(backend.resetSection(settings::SettingsSectionReset::Translation),
                "reset translation layout");
        session.refreshAll();
        require(session.state(layoutId).enabled &&
                    translation.layoutProcessing() == QStringLiteral("smart_merge"),
                "reset enables original-image translation and restores Smart Merge");
        const storage::ScreenshotTranslationConfiguration languages{
            QStringLiteral("ja"), QStringLiteral("zh-Hant"), QStringLiteral("chosen-model")};
        require(backend.switchEnabled(binding) && backend.switchValue(binding),
                "backend should expose an enabled, default-on translation switch");
        require(translation.setConfiguration(languages) &&
                    backend.applySwitchValue(binding, false) && !backend.switchValue(binding),
                "backend should persist the display toggle");
        require(backend.resetSection(settings::SettingsSectionReset::Translation) &&
                    backend.switchValue(binding) && translation.configuration() == languages,
                "reset Translation should restore only the display toggle");

        const auto resizeBinding = settings::SettingsSelectBinding::OcrDetectorResizePolicy;
        require(backend.selectValue(resizeBinding).toString() == QStringLiteral("max") &&
                    backend.applySelectValue(resizeBinding, QStringLiteral("min")) &&
                    backend.selectValue(resizeBinding).toString() == QStringLiteral("min") &&
                    !backend.applySelectValue(resizeBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(resizeBinding).toString() == QStringLiteral("min"),
                "detector scaling must default to max and persist only supported policies");
        require(backend.applySelectValue(settings::SettingsSelectBinding::OcrModelType,
                                         QStringLiteral("medium")) &&
                    backend.selectValue(settings::SettingsSelectBinding::OcrModelType).toString() ==
                        QStringLiteral("medium") &&
                    applicationStorage.configuration().setValue(
                        QStringLiteral("text_recognition/direct_ml_acceleration"), true) &&
                    backend.resetSection(settings::SettingsSectionReset::TextRecognition) &&
                    backend.selectValue(settings::SettingsSelectBinding::OcrModelType).toString() ==
                        QStringLiteral("small") &&
                    backend.selectValue(resizeBinding).toString() == QStringLiteral("max") &&
                    !applicationStorage.configuration()
                         .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                         .toBool(),
                "reset Text Recognition should restore Small, max scaling, and CPU mode");
    }
    require(storage::PinToScreenSettings().setDoubleClickAction(QStringLiteral("close")),
            "save pinned double-click action before restart");
    require(storage::PinToScreenSettings().setMiddleMouseButtonAction(QStringLiteral("none")),
            "save pinned middle-click action before restart");
    const snow_shot::shortcuts::ShortcutBindingList selectedTextKeys{
        QStringLiteral("Ctrl+Alt+T"), QStringLiteral("Ctrl+Shift+T")};
    require(storage::ShortcutSettings().setTranslateSelectedText(selectedTextKeys),
            "save selected text shortcut bindings before restart");
    applicationStorage.shutdown();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success &&
                storage::PinToScreenSettings().doubleClickAction() == QStringLiteral("close"),
            "pinned double-click action must survive a storage restart");
    require(storage::ExtendedFeaturesSettings().translationPageEnabled(),
            "feature opt-in survives storage restart");
    require(storage::ShortcutSettings().translateSelectedText() == selectedTextKeys,
            "both selected text shortcut bindings survive a storage restart");
    require(storage::PinToScreenSettings().middleMouseButtonAction() == QStringLiteral("none"),
            "pinned middle-click action must survive a storage restart");
    applicationStorage.shutdown();
    return 0;
}
