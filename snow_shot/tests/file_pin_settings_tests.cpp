#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
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
        require(backend.resetSection(settings::SettingsSectionReset::GlobalPinToScreenShortcuts),
                "Pin to screen reset succeeds");
        require(stored.pinSelectedFiles().isEmpty() &&
                    manager.state(action).status == GlobalShortcutStatus::Unset,
                "Pin to screen reset must clear the new shortcut");
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
    textSelectionSettings();
    shortcutSettings();
    storage.shutdown();
    return 0;
}
