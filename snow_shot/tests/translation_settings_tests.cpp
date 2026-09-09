#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QDir>
#include <QHash>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
class FakeTranslationHotkeyBackend final : public snow_shot::presentation::GlobalShortcutBackend {
  public:
    void setActivationHandler(ActivationHandler value) override {
        handler = std::move(value);
    }
    snow_shot::presentation::GlobalShortcutValidationResult
    validateShortcut(const QString& shortcut) const override {
        return {shortcut, true, snow_shot::presentation::GlobalShortcutFailureReason::None};
    }
    snow_shot::presentation::GlobalShortcutBackendResult
    registerShortcut(int id, const QString& shortcut) override {
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
    QHash<int, QString> registrations;
};

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
    const QStringList keys{QStringLiteral("Ctrl+Alt+T"), QStringLiteral("Ctrl+Shift+T")};
    {
        auto native = std::make_unique<FakeTranslationHotkeyBackend>();
        auto* input = native.get();
        bool fullscreen = false;
        GlobalShortcutManager manager(std::move(native), nullptr, [&]() { return fullscreen; });
        settings::BuiltInSettingsBackend backend(manager);
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        manager.initialize();
        require(manager.state(action).status == GlobalShortcutStatus::Unset,
                "unassigned action does not register a native shortcut");
        require(session.applyShortcuts(action, keys) && persisted.translateSelectedText() == keys &&
                    manager.state(action).status == GlobalShortcutStatus::Registered,
                "editing selected text bindings updates storage and native registrations");
        int activations = 0;
        QObject::connect(
            &manager, &GlobalShortcutManager::activated, &manager,
            [&](GlobalShortcutAction activated) { activations += activated == action; });
        const int id = input->registrations.key(keys.first());
        require(id != 0, "selected text shortcut has a native registration");
        input->handler(id);
        require(activations == 1, "native activation dispatches selected text translation");
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
        manager.setShortcuts(action, {QStringLiteral("F3")});
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
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated translation settings storage");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create translation settings executable directory");
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success,
            "initialize translation settings storage");
    selectedTextShortcutSettings();
    {
        snow_shot::presentation::GlobalShortcutManager shortcuts;
        settings::BuiltInSettingsBackend backend(shortcuts);
        const auto recognitionSave = settings::SettingsSwitchBinding::SaveRecognitionResultAsImage;
        require(backend.switchValue(recognitionSave), "recognition image export defaults on");
        require(backend.applySwitchValue(recognitionSave, false) &&
                    !storage::TextRecognitionSettings().saveRecognitionResultAsImage(),
                "recognition image export persists disabled setting");
        const auto oldFill =
            applicationStorage.configuration().value(QStringLiteral("text_recognition/fill_style"));
        require(applicationStorage.configuration().setValue(
                    QStringLiteral("text_recognition/fill_style"), QStringLiteral("blur")) &&
                    backend.resetSection(settings::SettingsSectionReset::TextRecognitionBehavior) &&
                    backend.switchValue(recognitionSave) &&
                    applicationStorage.configuration().value(
                        QStringLiteral("text_recognition/fill_style")) == QStringLiteral("blur"),
                "recognition save reset restores only its own setting");
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

        require(backend.applySelectValue(settings::SettingsSelectBinding::OcrModelType,
                                         QStringLiteral("medium")) &&
                    backend.selectValue(settings::SettingsSelectBinding::OcrModelType).toString() ==
                        QStringLiteral("medium") &&
                    applicationStorage.configuration().setValue(
                        QStringLiteral("text_recognition/direct_ml_acceleration"), true) &&
                    backend.resetSection(settings::SettingsSectionReset::TextRecognition) &&
                    backend.selectValue(settings::SettingsSelectBinding::OcrModelType).toString() ==
                        QStringLiteral("small") &&
                    !applicationStorage.configuration()
                         .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                         .toBool(),
                "reset Text Recognition should restore Small and disable DirectML acceleration");
    }
    require(storage::PinToScreenSettings().setDoubleClickAction(QStringLiteral("close")),
            "save pinned double-click action before restart");
    require(storage::PinToScreenSettings().setMiddleMouseButtonAction(QStringLiteral("none")),
            "save pinned middle-click action before restart");
    const QStringList selectedTextKeys{QStringLiteral("Ctrl+Alt+T"),
                                       QStringLiteral("Ctrl+Shift+T")};
    require(storage::ShortcutSettings().setTranslateSelectedText(selectedTextKeys),
            "save selected text shortcut bindings before restart");
    applicationStorage.shutdown();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success &&
                storage::PinToScreenSettings().doubleClickAction() == QStringLiteral("close"),
            "pinned double-click action must survive a storage restart");
    require(storage::ShortcutSettings().translateSelectedText() == selectedTextKeys,
            "both selected text shortcut bindings survive a storage restart");
    require(storage::PinToScreenSettings().middleMouseButtonAction() == QStringLiteral("none"),
            "pinned middle-click action must survive a storage restart");
    applicationStorage.shutdown();
    return 0;
}
