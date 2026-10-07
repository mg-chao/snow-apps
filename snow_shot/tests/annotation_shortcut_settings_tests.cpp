#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QJsonArray>
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
        std::exit(1);
    }
}

void annotationShortcutsPersistValidateAndReset(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager manager;
    settings::BuiltInSettingsBackend backend(manager);
    constexpr auto drawing = settings::SettingsLocalShortcutScope::Drawing;
    constexpr auto screenshot = settings::SettingsLocalShortcutScope::Screenshot;
    const auto drawingDefaults = storage::DrawingShortcutSettings().allShortcuts();
    const auto undoDefault = backend.localShortcuts(screenshot, QStringLiteral("undo"));
    const auto redoDefault = backend.localShortcuts(screenshot, QStringLiteral("redo"));

    for (const auto* id : {"line", "spotlight", "distance"}) {
        require(drawingDefaults.contains(QString::fromLatin1(id)) &&
                    backend.localShortcuts(drawing, QString::fromLatin1(id)).isEmpty(),
                "annotation tools without default shortcuts must start unassigned");
    }
    const shortcuts::ShortcutBindingList line{QStringLiteral("Ctrl+Alt+L"),
                                              QStringLiteral("Ctrl+Shift+L")};
    const shortcuts::ShortcutBindingList spotlight{QStringLiteral("Ctrl+Alt+S")};
    require(backend.applyLocalShortcuts(drawing, QStringLiteral("line"), line) &&
                backend.applyLocalShortcuts(drawing, QStringLiteral("spotlight"), spotlight),
            "all annotation tools must support custom bindings");
    require(!backend.validateLocalShortcut(drawing, QStringLiteral("spotlight"), line.first())
                    .supported &&
                !backend.applyLocalShortcuts(drawing, QStringLiteral("spotlight"), line) &&
                backend.localShortcuts(drawing, QStringLiteral("spotlight")) == spotlight,
            "new annotation tools must reject conflicting bindings without changing settings");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "annotation shortcut settings must flush");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(shortcuts::shortcutBindingsFromJson(
                reloaded.value(QStringLiteral("drawing_shortcuts/line")), false) == line &&
                shortcuts::shortcutBindingsFromJson(
                    reloaded.value(QStringLiteral("drawing_shortcuts/spotlight")), false) ==
                    spotlight,
            "new annotation shortcut settings must survive configuration reload");
    require(backend.applyLocalShortcuts(drawing, QStringLiteral("line"), {}) &&
                backend.localShortcuts(drawing, QStringLiteral("line")).isEmpty(),
            "configured annotation shortcuts must be clearable");

    const shortcuts::ShortcutBindingList undo{QStringLiteral("Ctrl+Alt+U")};
    const shortcuts::ShortcutBindingList redo{QStringLiteral("Ctrl+Alt+R")};
    require(backend.applyLocalShortcuts(screenshot, QStringLiteral("undo"), undo) &&
                backend.applyLocalShortcuts(screenshot, QStringLiteral("redo"), redo),
            "Undo and Redo must retain their existing configuration keys");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotOtherShortcuts) &&
                backend.localShortcuts(screenshot, QStringLiteral("undo")) == undo &&
                backend.localShortcuts(screenshot, QStringLiteral("redo")) == redo &&
                backend.localShortcuts(drawing, QStringLiteral("spotlight")) == spotlight,
            "resetting Image Recognition must preserve Annotation shortcuts");
    const shortcuts::ShortcutBindingList recognition{QStringLiteral("Ctrl+Alt+O")};
    require(
        backend.applyLocalShortcuts(screenshot, QStringLiteral("text_recognition"), recognition) &&
            backend.resetSection(settings::SettingsSectionReset::DrawingShortcuts) &&
            storage::DrawingShortcutSettings().allShortcuts() == drawingDefaults &&
            backend.localShortcuts(screenshot, QStringLiteral("undo")) == undoDefault &&
            backend.localShortcuts(screenshot, QStringLiteral("redo")) == redoDefault &&
            backend.localShortcuts(screenshot, QStringLiteral("text_recognition")) == recognition,
        "resetting Annotation must restore every tool and history binding independently");
}

void annotationResetRejectsHistoryCollisionsAtomically() {
    snow_shot::presentation::GlobalShortcutManager manager;
    settings::BuiltInSettingsBackend backend(manager);
    constexpr auto drawing = settings::SettingsLocalShortcutScope::Drawing;
    constexpr auto screenshot = settings::SettingsLocalShortcutScope::Screenshot;
    const auto drawingDefaults = storage::DrawingShortcutSettings().allShortcuts();
    const auto undoDefault = backend.localShortcuts(screenshot, QStringLiteral("undo"));
    const auto redoDefault = backend.localShortcuts(screenshot, QStringLiteral("redo"));
    const shortcuts::ShortcutBindingList customUndo{QStringLiteral("Ctrl+Alt+U")};
    const shortcuts::ShortcutBindingList customLine{QStringLiteral("Ctrl+Alt+L")};
    require(backend.applyLocalShortcuts(drawing, QStringLiteral("line"), customLine) &&
                backend.applyLocalShortcuts(screenshot, QStringLiteral("undo"), customUndo) &&
                backend.applyLocalShortcuts(screenshot, QStringLiteral("redo"), {}) &&
                backend.applyLocalShortcuts(screenshot, QStringLiteral("text_recognition"),
                                            redoDefault),
            "configure a valid assignment that conflicts with the default Redo shortcut");

    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const auto before = configuration.snapshot();
    const auto revision = configuration.revision();
    int notifications = 0;
    const auto connection =
        QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &backend,
                         [&](const QString&, const QJsonValue&) { ++notifications; });
    require(!backend.resetSection(settings::SettingsSectionReset::DrawingShortcuts),
            "Annotation reset must reject a default history shortcut used outside its section");
    require(configuration.snapshot() == before && configuration.revision() == revision &&
                notifications == 0,
            "rejected Annotation resets must preserve all scopes and emit no changes");
    QObject::disconnect(connection);

    const shortcuts::ShortcutBindingList customSave{QStringLiteral("Ctrl+Alt+F10")};
    require(backend.applyLocalShortcuts(screenshot, QStringLiteral("save_as_file"), customSave),
            "unrelated screenshot shortcuts must remain editable after a rejected reset");
    require(backend.applyLocalShortcuts(screenshot, QStringLiteral("text_recognition"), {}),
            "free the default Redo shortcut before retrying");
    auto expectedScreenshot = storage::ScreenshotShortcutSettings().allShortcuts();
    expectedScreenshot.insert(QStringLiteral("undo"), undoDefault);
    expectedScreenshot.insert(QStringLiteral("redo"), redoDefault);
    const auto retryRevision = configuration.revision();
    int retryNotifications = 0;
    const auto retryConnection = QObject::connect(
        &configuration, &storage::ConfigurationStore::valueChanged, &backend,
        [&](const QString&, const QJsonValue&) {
            ++retryNotifications;
            require(storage::DrawingShortcutSettings().allShortcuts() == drawingDefaults &&
                        storage::ScreenshotShortcutSettings().allShortcuts() == expectedScreenshot,
                    "reset observers must see every scope committed together");
        });
    require(backend.resetSection(settings::SettingsSectionReset::DrawingShortcuts) &&
                configuration.revision() == retryRevision + 1 && retryNotifications > 0 &&
                storage::DrawingShortcutSettings().allShortcuts() == drawingDefaults &&
                storage::ScreenshotShortcutSettings().allShortcuts() == expectedScreenshot,
            "retrying Annotation reset must restore defaults in one transaction");
    QObject::disconnect(retryConnection);
}

void screenshotSectionResetsRejectPreservedShortcutCollisions() {
    snow_shot::presentation::GlobalShortcutManager manager;
    settings::BuiltInSettingsBackend backend(manager);
    constexpr auto scope = settings::SettingsLocalShortcutScope::Screenshot;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const auto original = configuration.snapshot();
    const auto drawingBefore = storage::DrawingShortcutSettings().allShortcuts();
    struct ResetScenario {
        settings::SettingsSectionReset reset;
        const char* restoredAction;
        const char* preservedAction;
    };
    const ResetScenario scenarios[] = {
        {settings::SettingsSectionReset::ScreenshotOtherShortcuts, "text_recognition", "redo"},
        {settings::SettingsSectionReset::ScreenshotEditorShortcuts, "save_as_file",
         "text_recognition"},
    };
    for (const auto& scenario : scenarios) {
        require(configuration.applySnapshot(original), "restore section reset fixture");
        const QString restored = QString::fromLatin1(scenario.restoredAction);
        const QString preserved = QString::fromLatin1(scenario.preservedAction);
        const auto defaultBindings = shortcuts::shortcutBindingsFromJson(
            storage::ConfigurationSchema::defaultValue(QStringLiteral("screenshot_shortcuts/") +
                                                       restored),
            true);
        const shortcuts::ShortcutBindingList custom{QStringLiteral("Ctrl+Alt+F12")};
        require(backend.applyLocalShortcuts(scope, restored, custom) &&
                    backend.applyLocalShortcuts(scope, preserved, defaultBindings),
                "configure a preserved shortcut that occupies another section's default");
        const auto before = configuration.snapshot();
        const auto revision = configuration.revision();
        require(!backend.resetSection(scenario.reset) && configuration.snapshot() == before &&
                    configuration.revision() == revision,
                "screenshot section resets must reject preserved collisions without mutations");
        require(backend.applyLocalShortcuts(scope, preserved, {}) &&
                    backend.resetSection(scenario.reset) &&
                    backend.localShortcuts(scope, restored) == defaultBindings &&
                    backend.localShortcuts(scope, preserved).isEmpty() &&
                    storage::DrawingShortcutSettings().allShortcuts() == drawingBefore,
                "section resets must succeed once the conflicting preserved binding is freed");
    }
    require(configuration.applySnapshot(original), "restore shortcut section reset assignments");
}

void localShortcutBatchesValidateEveryAffectedScope() {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const auto original = configuration.snapshot();
    const QString lineKey = QStringLiteral("drawing_shortcuts/line");
    const auto validLine = QJsonArray{QStringLiteral("Ctrl+Alt+F11")};
    const auto reject = [&](const QMap<QString, QJsonValue>& values) {
        const auto before = configuration.snapshot();
        const auto revision = configuration.revision();
        require(!storage::setLocalShortcutValuesAtomic(values) &&
                    configuration.snapshot() == before && configuration.revision() == revision,
                "invalid local shortcut batches must reject every value without a commit");
    };
    reject({{lineKey, validLine},
            {QStringLiteral("screenshot_shortcuts/redo"), QJsonArray{QStringLiteral("Ctrl+Z")}}});
    reject({{lineKey, validLine},
            {QStringLiteral("screenshot_shortcuts/text_recognition"),
             configuration.value(QStringLiteral("screenshot_shortcuts/redo"))}});
    reject({{lineKey, configuration.value(QStringLiteral("drawing_shortcuts/shape"))},
            {QStringLiteral("screenshot_shortcuts/text_recognition"), validLine}});
    reject({{lineKey, validLine},
            {QStringLiteral("screen_recording_shortcuts/toggle_recording"),
             configuration.value(QStringLiteral("screen_recording_shortcuts/export"))}});
    reject({{lineKey, validLine},
            {QStringLiteral("pin_to_screen_shortcuts/resize_window"),
             configuration.value(QStringLiteral("pin_to_screen_shortcuts/copy_to_clipboard"))}});
    reject({{lineKey, validLine}, {QStringLiteral("drawing_shortcuts/spotlight"), validLine}});
    reject({{lineKey, QJsonArray{QStringLiteral("Ctrl+C")}}});
    reject(
        {{lineKey, validLine}, {QStringLiteral("screenshot_shortcuts/text_recognition"), false}});
    reject({{lineKey, validLine}, {QStringLiteral("drawing_shortcuts/unsupported"), validLine}});
    reject({{lineKey, validLine}, {QStringLiteral("drawing/remember_last_used_tool"), true}});

    const QMap<QString, QJsonValue> values{
        {lineKey, validLine},
        {QStringLiteral("screenshot_shortcuts/text_recognition"), validLine},
        {QStringLiteral("screen_recording_shortcuts/export"), validLine},
        {QStringLiteral("pin_to_screen_shortcuts/resize_window"), validLine},
    };
    configuration.suspendWrites(true);
    reject(values);
    configuration.suspendWrites(false);
    const auto revision = configuration.revision();
    require(storage::setLocalShortcutValuesAtomic(values) &&
                configuration.revision() == revision + 1,
            "distinct scopes may share a shortcut and must commit together");
    const shortcuts::ShortcutBindingList expected{QStringLiteral("Ctrl+Alt+F11")};
    require(storage::DrawingShortcutSettings().shortcuts(QStringLiteral("line")) == expected &&
                storage::ScreenshotShortcutSettings().shortcuts(
                    QStringLiteral("text_recognition")) == expected &&
                storage::ScreenRecordingShortcutSettings().shortcuts(QStringLiteral("export")) ==
                    expected &&
                storage::PinToScreenShortcutSettings().shortcuts(QStringLiteral("resize_window")) ==
                    expected,
            "shared batch writes must normalize all scopes for their typed adapters");
    require(configuration.applySnapshot(original), "restore mixed shortcut batch assignments");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create isolated annotation settings directory");
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage
                .initialize({temporary.filePath(QStringLiteral("bin")),
                             temporary.filePath(QStringLiteral("data")), 60000})
                .success,
            "failed to initialize isolated annotation settings");
    annotationShortcutsPersistValidateAndReset(
        temporary.filePath(QStringLiteral("data/config.json")));
    annotationResetRejectsHistoryCollisionsAtomically();
    screenshotSectionResetsRejectPreservedShortcutCollisions();
    localShortcutBatchesValidateEveryAffectedScope();
    appStorage.shutdown();
    return 0;
}
