#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"

#include <QApplication>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;
namespace shortcuts = snow_shot::shortcuts;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void printShortcutsReset(settings::SettingsRuntimeSession& session) {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const shortcuts::ShortcutBindingList defaults{QStringLiteral("Ctrl+P")};
    for (const auto scope : {settings::SettingsLocalShortcutScope::Screenshot,
                             settings::SettingsLocalShortcutScope::PinToScreen}) {
        const auto reset = scope == settings::SettingsLocalShortcutScope::Screenshot
                               ? settings::SettingsSectionReset::ScreenshotEditorShortcuts
                               : settings::SettingsSectionReset::PinToScreenShortcuts;
        const auto before = configuration.snapshot();
        require(session.localShortcuts(scope, QStringLiteral("print")) == defaults,
                "print shortcuts must initially default to Ctrl+P");
        for (const auto& value : {shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+Alt+F11"),
                                                                 QStringLiteral("Ctrl+Alt+F12")},
                                  shortcuts::ShortcutBindingList{}}) {
            require(session.applyLocalShortcuts(scope, QStringLiteral("print"), value) &&
                        session.localShortcuts(scope, QStringLiteral("print")) == value,
                    "print shortcuts must support custom keys and disabling");
            require(session.reset(reset) &&
                        session.localShortcuts(scope, QStringLiteral("print")) == defaults &&
                        configuration.snapshot() == before,
                    "reset must restore customized and disabled print shortcuts to Ctrl+P");
        }
    }
}

void allLocalShortcutSectionsReset(settings::SettingsRuntimeSession& session) {
    const auto& registry = settings::builtInSettingsRegistry();
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    for (const auto reset : {settings::SettingsSectionReset::ScreenshotEditorShortcuts,
                             settings::SettingsSectionReset::ScreenshotOtherShortcuts,
                             settings::SettingsSectionReset::DrawingShortcuts,
                             settings::SettingsSectionReset::PinToScreenShortcuts,
                             settings::SettingsSectionReset::ScreenRecordingShortcuts}) {
        // Disabled siblings make preservation observable across scopes and the
        // screenshot bindings shared by Screenshot, Annotation, and Image Recognition.
        for (const auto& field : registry.fields()) {
            if (field.kind != settings::SettingsFieldKind::LocalShortcut) {
                continue;
            }
            const auto& local =
                std::get<settings::SettingsLocalShortcutDefinition>(field.definition->payload);
            require(session.applyLocalShortcuts(local.scope, local.shortcutId, {}),
                    "disable local shortcuts before checking section isolation");
        }
        const auto& indexes = registry.fieldsForReset(reset);
        require(!indexes.isEmpty(), "every local shortcut reset section must have fields");
        auto expected = configuration.snapshot();
        for (const int index : indexes) {
            const auto& field = registry.fields().at(index);
            require(field.kind == settings::SettingsFieldKind::LocalShortcut,
                    "application shortcut sections must contain local shortcut fields");
            const auto normalized = storage::ConfigurationSchema::normalize(
                field.configurationKey,
                storage::ConfigurationSchema::defaultValue(field.configurationKey));
            require(normalized.valid, "local shortcut defaults must be valid");
            expected.insert(field.configurationKey, normalized.value);
        }
        for (const int index : indexes) {
            const auto& field = registry.fields().at(index);
            const auto& local =
                std::get<settings::SettingsLocalShortcutDefinition>(field.definition->payload);
            for (const auto& value :
                 {shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+Alt+F11"),
                                                 QStringLiteral("Ctrl+Alt+F12")},
                  shortcuts::ShortcutBindingList{}}) {
                require(session.applyLocalShortcuts(local.scope, local.shortcutId, value),
                        qPrintable(QStringLiteral("customize local shortcut: %1").arg(field.id)));
                require(session.reset(reset) && configuration.snapshot() == expected,
                        qPrintable(QStringLiteral("reset entire section and preserve siblings: %1")
                                       .arg(field.id)));
                for (const int resetIndex : indexes) {
                    const auto& resetField = registry.fields().at(resetIndex);
                    const auto& resetLocal = std::get<settings::SettingsLocalShortcutDefinition>(
                        resetField.definition->payload);
                    const auto bindings = shortcuts::shortcutBindingsFromJson(
                        expected.value(resetField.configurationKey),
                        resetLocal.scope == settings::SettingsLocalShortcutScope::Screenshot, 2,
                        nullptr, nullptr, resetLocal.shortcutId == QStringLiteral("toggle_guides"));
                    require(session.localShortcuts(resetLocal.scope, resetLocal.shortcutId) ==
                                    bindings &&
                                session.state(resetField.id).phase ==
                                    settings::SettingsWritePhase::Clean,
                            "reset must refresh every shortcut field in the runtime session");
                }
            }
        }
    }
}

void conflictingResetRemainsAtomic(settings::SettingsRuntimeSession& session) {
    constexpr auto scope = settings::SettingsLocalShortcutScope::Screenshot;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(
        session.applyLocalShortcuts(scope, QStringLiteral("print"), {}) &&
            session.applyLocalShortcuts(scope, QStringLiteral("undo"), {QStringLiteral("Ctrl+P")}),
        "assign print's default key to an Annotation shortcut in the screenshot scope");
    const auto before = configuration.snapshot();
    require(!session.reset(settings::SettingsSectionReset::ScreenshotEditorShortcuts) &&
                configuration.snapshot() == before,
            "a conflicting section reset must reject the entire write and preserve siblings");
    require(session.reset(settings::SettingsSectionReset::DrawingShortcuts) &&
                session.reset(settings::SettingsSectionReset::ScreenshotEditorShortcuts) &&
                session.localShortcuts(scope, QStringLiteral("print")) ==
                    shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+P")},
            "print reset must succeed after resetting the conflicting sibling section");
}

void annotationResetPreservesPrintShortcut(settings::SettingsRuntimeSession& session) {
    constexpr auto screenshot = settings::SettingsLocalShortcutScope::Screenshot;
    constexpr auto drawing = settings::SettingsLocalShortcutScope::Drawing;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const auto redoDefault = session.localShortcuts(screenshot, QStringLiteral("redo"));
    require(session.applyLocalShortcuts(screenshot, QStringLiteral("redo"), {}) &&
                session.applyLocalShortcuts(screenshot, QStringLiteral("print"), redoDefault) &&
                session.applyLocalShortcuts(drawing, QStringLiteral("line"),
                                            {QStringLiteral("Ctrl+Alt+F10")}),
            "assign Redo's default to Print while customizing an Annotation tool");
    const auto before = configuration.snapshot();
    const auto revision = configuration.revision();
    int notifications = 0;
    const auto connection =
        QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &session,
                         [&](const QString&, const QJsonValue&) { ++notifications; });
    require(!session.reset(settings::SettingsSectionReset::DrawingShortcuts) &&
                configuration.snapshot() == before && configuration.revision() == revision &&
                notifications == 0,
            "a Print collision must reject the Annotation reset across both shortcut scopes");
    QObject::disconnect(connection);
    require(session.reset(settings::SettingsSectionReset::ScreenshotEditorShortcuts) &&
                session.localShortcuts(screenshot, QStringLiteral("redo")).isEmpty() &&
                session.localShortcuts(drawing, QStringLiteral("line")) ==
                    shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+Alt+F10")},
            "resetting Print must preserve both scopes of the Annotation section");
    require(session.reset(settings::SettingsSectionReset::DrawingShortcuts) &&
                session.localShortcuts(screenshot, QStringLiteral("redo")) == redoDefault &&
                session.localShortcuts(screenshot, QStringLiteral("print")) ==
                    shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+P")},
            "Annotation reset must restore Redo and preserve Print once its default is freed");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated local shortcut settings directory");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    const storage::StorageInitializationOptions options{temporary.filePath(QStringLiteral("bin")),
                                                        temporary.path(), 60000};
    require(applicationStorage.initialize(options).success,
            "initialize local shortcut settings storage");
    {
        snow_shot::presentation::GlobalShortcutManager manager;
        settings::BuiltInSettingsBackend backend(manager);
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        printShortcutsReset(session);
        allLocalShortcutSectionsReset(session);
        conflictingResetRemainsAtomic(session);
        annotationResetPreservesPrintShortcut(session);
    }
    const auto expected = applicationStorage.configuration().snapshot();
    require(applicationStorage.flushNow().success, "persist reset local shortcut settings");
    applicationStorage.shutdown();
    require(applicationStorage.initialize(options).success &&
                applicationStorage.configuration().snapshot() == expected,
            "reset shortcuts and preserved siblings must survive restart");
    applicationStorage.shutdown();
    return 0;
}
