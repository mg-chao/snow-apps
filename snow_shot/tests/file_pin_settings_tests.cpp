#include "snow_shot/presentation/screenshotpinsourcetracker.h"
#include <QClipboard>
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"
#include <QApplication>
#include <QHash>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

namespace {
using namespace snow_shot::presentation;
namespace storage = snow_shot::storage;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
class Backend final : public GlobalShortcutBackend {
  public:
    ActivationHandler handler;
    QHash<int, snow_shot::shortcuts::ShortcutBinding> registrations;
    void setActivationHandler(ActivationHandler value) override {
        handler = std::move(value);
    }
    GlobalShortcutValidationResult
    validateShortcut(const snow_shot::shortcuts::ShortcutBinding& value) const override {
        return {value.portableText, true, GlobalShortcutFailureReason::None, value};
    }
    GlobalShortcutBackendResult
    registerShortcut(int id, const snow_shot::shortcuts::ShortcutBinding& value) override {
        if (registrations.values().contains(value)) {
            return {false, GlobalShortcutFailureReason::AlreadyInUse};
        }
        registrations.insert(id, value);
        return {true};
    }
    void unregisterShortcut(int id) override {
        registrations.remove(id);
    }
};
void windowButtonSettings() {
    const storage::PinToScreenSettings stored;
    const auto binding = settings::SettingsSwitchBinding::PinShowWindowButtons;
    GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
    settings::BuiltInSettingsBackend backend(manager);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    const auto* field = settings::builtInSettingsRegistry().fieldForSwitch(binding);
    require(field && field->pageId == QStringLiteral("function-settings") &&
                field->sectionId == QStringLiteral("pin-to-screen-settings") &&
                field->id == QStringLiteral("pin-to-screen.show-window-buttons") &&
                field->configurationKey == QStringLiteral("pin_to_screen/show_window_buttons") &&
                field->reset == settings::SettingsSectionReset::PinToScreenBehavior,
            "window buttons switch belongs to Pin to screen in function settings");
    require(stored.showWindowButtons() && backend.switchValue(binding) &&
                session.state(field->id).visible && session.state(field->id).enabled,
            "window buttons default on and their setting is available");
    require(session.applySwitchValue(binding, false) && !stored.showWindowButtons() &&
                !backend.switchValue(binding),
            "the settings switch must disable both window buttons");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.configuration().flushNow().success,
            "window button preferences must be flushable");
    storage::ConfigurationStore reloaded(applicationStorage.configurationDirectory() +
                                             QStringLiteral("/config.json"),
                                         true, true, 60000);
    require(!reloaded.value(QStringLiteral("pin_to_screen/show_window_buttons")).toBool(true),
            "disabled window buttons must survive a configuration reload");
    require(session.applySwitchValue(binding, true) && stored.showWindowButtons(),
            "the settings switch must enable window buttons again");
    require(session.applySwitchValue(binding, false) &&
                backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                stored.showWindowButtons() && backend.switchValue(binding),
            "resetting pin behavior must restore visible window buttons");
}

void textSelectionSettings() {
    const storage::PinToScreenSettings stored;
    const auto binding = settings::SettingsSelectBinding::PinTextSelectionOnRecognitionResults;
    GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
    settings::BuiltInSettingsBackend backend(manager);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    require(stored.textSelectionOnRecognitionResults() == QStringLiteral("only_when_displayed"),
            "hidden text selection defaults off");
    const auto* field = settings::builtInSettingsRegistry().fieldForSelect(binding);
    require(field &&
                field->id ==
                    QStringLiteral("pin-to-screen.text-selection-on-recognition-results") &&
                field->reset == settings::SettingsSectionReset::PinToScreenBehavior,
            "select is registered in the pin behavior section");
    const auto& select = std::get<settings::SettingsSelectDefinition>(field->definition->payload);
    require(select.options.size() == 2 &&
                select.options[0].value == QStringLiteral("only_when_displayed") &&
                select.options[0].label.translated() == QStringLiteral("Only when displayed") &&
                select.options[1].value == QStringLiteral("always") &&
                select.options[1].label.translated() == QStringLiteral("Always"),
            "dropdown options match the specified labels and order");
    const auto* section =
        settings::builtInSettingsRegistry().catalog().section(field->pageId, field->sectionId);
    require(section != nullptr, "pin section exists");
    int index = -1;
    for (int i = 0; i < section->items.size(); ++i) {
        if (section->items[i].id == field->id)
            index = i;
    }
    require(index > 0 && section->items[index - 1].id ==
                             QStringLiteral("pin-to-screen.automatic-text-recognition"),
            "selection setting follows automatic recognition");
    require(session.applySelectValue(binding, QStringLiteral("always")) &&
                stored.textSelectionOnRecognitionResults() == QStringLiteral("always"),
            "settings session writes selection policy through backend");
    require(!stored.setTextSelectionOnRecognitionResults(QStringLiteral("invalid")) &&
                stored.textSelectionOnRecognitionResults() == QStringLiteral("always"),
            "schema rejects unsupported selection modes");
    require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                stored.textSelectionOnRecognitionResults() == QStringLiteral("only_when_displayed"),
            "pin behavior reset restores default selection policy");
}

void clipboardSourceIdentity() {
    auto* clipboard = QApplication::clipboard();
    ScreenshotPinSourceTracker tracker(clipboard);
    const auto first = tracker.clipboardIdentity();
    require(first.isValid() && tracker.clipboardIdentity() == first,
            "reading a clipboard identity neither reads nor changes its content");
    clipboard->setText(QStringLiteral("duplicate pin fixture"));
    const auto second = tracker.clipboardIdentity();
    require(second != first, "a clipboard change creates a new identity");
    clipboard->setText(QStringLiteral("duplicate pin fixture"));
    require(tracker.clipboardIdentity() != second, "copying identical text creates a new source");
    ScreenshotPinSourceTracker nextSession(clipboard);
    require(nextSession.clipboardIdentity() != tracker.clipboardIdentity(),
            "clipboard identities cannot match a different application session");
}

void duplicateContentSettings() {
    const storage::PinToScreenSettings stored;
    const auto binding = settings::SettingsSelectBinding::PinDuplicateContentAction;
    GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
    settings::BuiltInSettingsBackend backend(manager);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    require(stored.duplicateContentAction() == QStringLiteral("shake_window"),
            "duplicate pins default to shaking");
    const auto* field = settings::builtInSettingsRegistry().fieldForSelect(binding);
    require(field && field->id == QStringLiteral("pin-to-screen.duplicate-content-action") &&
                field->reset == settings::SettingsSectionReset::PinToScreenBehavior &&
                field->definition->title.translated() ==
                    QStringLiteral("When pinning duplicate content"),
            "duplicate policy belongs to Pin to screen behavior");
    const auto& select = std::get<settings::SettingsSelectDefinition>(field->definition->payload);
    const QStringList values{QStringLiteral("none"), QStringLiteral("shake_window"),
                             QStringLiteral("restore_last_closed_window"),
                             QStringLiteral("repeat_action")};
    const QStringList labels{QStringLiteral("None"), QStringLiteral("Shake Window"),
                             QStringLiteral("Restore Last Closed Window"),
                             QStringLiteral("Repeat Action")};
    require(select.options.size() == values.size(), "duplicate policy has four choices");
    for (int i = 0; i < values.size(); ++i) {
        require(select.options[i].value == values[i] &&
                    select.options[i].label.translated() == labels[i],
                "duplicate choices retain their specified order and labels");
        require(session.applySelectValue(binding, values[i]) &&
                    stored.duplicateContentAction() == values[i],
                "every duplicate policy persists through the settings backend");
    }
    require(!stored.setDuplicateContentAction(QStringLiteral("invalid")) &&
                stored.duplicateContentAction() == QStringLiteral("repeat_action"),
            "invalid duplicate policies are rejected");
    require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                stored.duplicateContentAction() == QStringLiteral("shake_window"),
            "pin behavior reset restores shake policy");
}

void shortcutSettings() {
    const auto action = GlobalShortcutAction::PinSelectedFiles;
    const QString id = QStringLiteral("quick.pin-selected-files");
    const storage::ShortcutSettings stored;
    const auto* item = settings::builtInSettingsRegistry().catalog().item(
        {QStringLiteral("global-hotkeys"), QStringLiteral("pin-to-screen"), id});
    require(item != nullptr, "file pin action must remain in global shortcut settings");
#ifdef Q_OS_MACOS
    require(item->description.translated() ==
                QStringLiteral("Pin selected image files from Finder or the desktop to the screen"),
            "macOS file pin description must identify Finder");
#else
    require(item->description.translated() ==
                QStringLiteral(
                    "Pin selected image files from File Explorer or the desktop to the screen"),
            "Windows file pin description must identify File Explorer");
#endif
    require(stored.pinSelectedFiles().isEmpty(), "selected files starts unassigned");
    const storage::TraySettings tray;
    const auto defaultMenu = tray.menuOptions();
    require(!defaultMenu.contains(id), "new action must remain optional in tray menu");
    auto menu = defaultMenu;
    menu.append(id);
    require(tray.setMenuOptions(menu) && tray.menuOptions() == menu,
            "new tray option must persist");
    const snow_shot::shortcuts::ShortcutBindingList keys{QStringLiteral("Ctrl+Alt+P"),
                                                         QStringLiteral("Ctrl+Shift+P")};
    {
        auto native = std::make_unique<Backend>();
        auto* input = native.get();
        GlobalShortcutManager manager(std::move(native), nullptr, [] { return false; });
        settings::BuiltInSettingsBackend backend(manager);
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        manager.initialize();
        require(manager.state(action).status == GlobalShortcutStatus::Unset,
                "default must not register");
        require(session.state(id).visible,
                "file pin action must be visible without optional features");
        require(session.applyShortcuts(action, keys), "new shortcut must be configurable");
        require(stored.pinSelectedFiles() == keys &&
                    manager.state(action).status == GlobalShortcutStatus::Registered,
                "both shortcuts must persist and register");
        int activated = 0;
        QObject::connect(&manager, &GlobalShortcutManager::activated, &manager,
                         [&](GlobalShortcutAction value) {
                             if (value == action) {
                                 ++activated;
                             }
                         });
        input->handler(input->registrations.key(keys.first()));
        require(activated == 1, "native activation must dispatch selected-file action");
        const auto clipboardBindings = stored.pinClipboardContent();
        require(!clipboardBindings.isEmpty(), "clipboard pinning must have a default shortcut");
        require(session.applyShortcuts(action, clipboardBindings), "conflicting binding is stored");
        require(manager.state(action).status == GlobalShortcutStatus::Failed &&
                    manager.state(action).bindings.first().failureReason ==
                        GlobalShortcutFailureReason::AlreadyInUse,
                "clipboard shortcut conflict must be reported");
        const auto managementAction = GlobalShortcutAction::OpenPinToScreenManagement;
        const snow_shot::shortcuts::ShortcutBindingList managementKeys{
            QStringLiteral("Ctrl+Alt+Shift+M")};
        require(session.applyShortcuts(managementAction, managementKeys),
                "management shortcut must be configurable");
        require(backend.resetSection(settings::SettingsSectionReset::OtherShortcuts) &&
                    manager.state(managementAction).shortcuts == managementKeys,
                "Other reset must preserve the management shortcut in Pin to screen");
        require(session.applyShortcuts(GlobalShortcutAction::SwitchWindowGroup,
                                       {QStringLiteral("Ctrl+Alt+F8")}),
                "assign group switch shortcut");
        require(backend.resetSection(settings::SettingsSectionReset::GlobalPinToScreenShortcuts),
                "Pin to screen reset succeeds");
        require(stored.pinSelectedFiles().isEmpty() &&
                    manager.state(action).status == GlobalShortcutStatus::Unset,
                "Pin to screen reset must clear the new shortcut");
        require(manager.state(GlobalShortcutAction::SwitchWindowGroup).shortcuts.isEmpty(),
                "pin section reset clears group switch shortcut");
        require(manager.state(managementAction).shortcuts.isEmpty(),
                "Pin to screen reset must clear the management shortcut");
        require(session.applyShortcuts(action, keys), "prepare reload");
    }
    {
        GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
        manager.initialize();
        require(manager.state(action).shortcuts == keys &&
                    manager.state(action).status == GlobalShortcutStatus::Registered,
                "manager reload must restore both new bindings");
    }
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(
        storage.initialize({directory.filePath(QStringLiteral("bin")), directory.path()}).success,
        "temporary storage must initialize");
    windowButtonSettings();
    textSelectionSettings();
    duplicateContentSettings();
    clipboardSourceIdentity();
    shortcutSettings();
    storage.shutdown();
    return 0;
}
