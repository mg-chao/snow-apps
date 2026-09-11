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
    QHash<int, QString> registrations;
    void setActivationHandler(ActivationHandler value) override {
        handler = std::move(value);
    }
    GlobalShortcutValidationResult validateShortcut(const QString& value) const override {
        return {value, true, GlobalShortcutFailureReason::None};
    }
    GlobalShortcutBackendResult registerShortcut(int id, const QString& value) override {
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
void shortcutSettings() {
    const auto action = GlobalShortcutAction::PinSelectedFiles;
    const QString id = QStringLiteral("quick.pin-selected-files");
    const storage::ShortcutSettings stored;
    require(stored.pinSelectedFiles().isEmpty(), "selected files starts unassigned");
    const storage::TraySettings tray;
    const auto defaultMenu = tray.menuOptions();
    require(!defaultMenu.contains(id), "new action must remain optional in tray menu");
    auto menu = defaultMenu;
    menu.append(id);
    require(tray.setMenuOptions(menu) && tray.menuOptions() == menu,
            "new tray option must persist");
    const QStringList keys{QStringLiteral("Ctrl+Alt+P"), QStringLiteral("Ctrl+Shift+P")};
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
        require(session.applyShortcuts(action, {QStringLiteral("F3")}),
                "conflicting binding is stored");
        require(manager.state(action).status == GlobalShortcutStatus::Failed &&
                    manager.state(action).bindings.first().failureReason ==
                        GlobalShortcutFailureReason::AlreadyInUse,
                "clipboard shortcut conflict must be reported");
        require(backend.resetSection(settings::SettingsSectionReset::OtherShortcuts),
                "Other reset succeeds");
        require(stored.pinSelectedFiles().isEmpty() &&
                    manager.state(action).status == GlobalShortcutStatus::Unset,
                "Other reset must clear the new shortcut");
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
    shortcutSettings();
    storage.shutdown();
    return 0;
}
