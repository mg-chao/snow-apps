#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
namespace presentation = snow_shot::presentation;
namespace settings = presentation::settings;
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class FakeShortcutBackend final : public presentation::GlobalShortcutBackend {
  public:
    QMap<int, QString> registrations;
    ActivationHandler activationHandler;

    void setActivationHandler(ActivationHandler handler) override {
        activationHandler = std::move(handler);
    }
    presentation::GlobalShortcutValidationResult
    validateShortcut(const QString& shortcut) const override {
        return {shortcut, true, presentation::GlobalShortcutFailureReason::None};
    }
    presentation::GlobalShortcutBackendResult registerShortcut(int id,
                                                               const QString& shortcut) override {
        registrations.insert(id, shortcut);
        return {true, presentation::GlobalShortcutFailureReason::None, 0};
    }
    void unregisterShortcut(int id) override {
        registrations.remove(id);
    }
};

void catalogExposesOnlyAvailableIntegrations() {
    const auto& registry = settings::builtInSettingsRegistry();
    const auto& catalog = registry.catalog();
    require(catalog.validationErrors().isEmpty(), "the filtered macOS catalog must validate");
    require(catalog.page(QStringLiteral("global-mouse")) == nullptr,
            "the unavailable global mouse page must not appear in navigation");
    for (const auto& key :
         {"system/application_priority", "system/auto_start_at_boot", "updates/mode",
          "screenshot/api_mode", "screenshot/window_element_api",
          "screenshot/restore_original_screen_colors", "screenshot_selection/smart_selection",
          "text_recognition/direct_ml_acceleration",
          "global_shortcuts/disable_on_focused_fullscreen_window",
          "global_shortcuts/translate_selected_text",
          "screenshot_shortcuts/switch_selection_between_window_and_window_sub_element",
          "global_mouse/screenshot_copy", "global_mouse/screenshot_fixed",
          "global_mouse/screenshot_ocr", "global_mouse/screenshot_translation",
          "global_mouse/screenshot_save", "global_mouse/screenshot_quick_save",
          "global_mouse/screen_recording"}) {
        const auto configurationKey = QString::fromLatin1(key);
        require(storage::ConfigurationSchema::entry(configurationKey) != nullptr,
                "inactive Windows settings must remain portable schema entries");
        require(registry.fieldForConfigurationKey(configurationKey) == nullptr,
                "unsupported integrations must not generate settings controls");
    }
    for (const auto& page : catalog.pages()) {
        require(page.kind != settings::SettingsPageKind::GeneratedSettings ||
                    !page.sections.isEmpty(),
                "filtering must remove empty generated pages");
        for (const auto& section : page.sections) {
            require(!section.items.isEmpty(),
                    "filtering must remove empty sections and reset buttons");
        }
    }
    for (const auto& key :
         {"screenshot/capture_cursor", "text_recognition/model_type", "global_shortcuts/screenshot",
          "global_shortcuts/screen_record", "extended_features/translation_page_enabled",
          "capture_history/enabled"}) {
        require(
            registry.fieldForConfigurationKey(QString::fromLatin1(key)) != nullptr,
            "supported screenshot, recording, OCR, translation and history settings must remain");
    }
    settings::SettingsSearchIndex index(registry);
    for (const auto& query : {"Windows starts", "DXGI", "UIA", "MSAA", "DirectML",
                              "Translate Selected Text", "Software updates"}) {
        require(index.search(QString::fromLatin1(query)).isEmpty(),
                "search must not expose unavailable platform settings or stale Windows wording");
    }
    require(registry.fieldForSwitch(settings::SettingsSwitchBinding::TranslationPageEnabled)
                    ->definition->description.translated() ==
                QStringLiteral("Enable the Translation page."),
            "the supported translation page must not advertise unsupported selected-text capture");
    const auto manifest = settings::buildBuiltInTrayCommandManifest();
    require(
        manifest.shortcutActionTitle(presentation::GlobalShortcutAction::TranslateSelectedText, 3)
            .isEmpty(),
        "the compact tray manifest must not expose selected-text capture");
    require(catalog.trayMenuGroups().size() == manifest.groups.size(),
            "compact and settings tray projections must stay consistent");
}

void resetPreservesInactivePreferences(settings::BuiltInSettingsBackend& backend) {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    QMap<QString, QJsonValue> inactive{
        {QStringLiteral("system/application_priority"), QStringLiteral("real_time")},
        {QStringLiteral("system/auto_start_at_boot"), true},
        {QStringLiteral("updates/mode"), QStringLiteral("manual")},
        {QStringLiteral("screenshot/api_mode"), QStringLiteral("wgc")},
        {QStringLiteral("screenshot/window_element_api"), QStringLiteral("uia")},
        {QStringLiteral("screenshot/restore_original_screen_colors"), true},
        {QStringLiteral("screenshot_selection/smart_selection"), false},
        {QStringLiteral("text_recognition/direct_ml_acceleration"), true},
        {QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window"), true},
        {QStringLiteral("global_mouse/screenshot_copy"),
         QJsonObject{{QStringLiteral("activation_key"), QStringLiteral("ctrl")},
                     {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}}},
        {QStringLiteral("global_shortcuts/translate_selected_text"),
         QJsonArray{QStringLiteral("Ctrl+F18")}},
        {QStringLiteral(
             "screenshot_shortcuts/switch_selection_between_window_and_window_sub_element"),
         QJsonArray{QStringLiteral("F18")}},
    };
    require(configuration.setValues(inactive), "seed portable Windows preferences");
    require(configuration.setValue(QStringLiteral("screenshot/capture_cursor"), true),
            "seed a supported screenshot setting");
    require(configuration.setValue(QStringLiteral("text_recognition/model_type"),
                                   QStringLiteral("medium")),
            "seed a supported OCR model setting");
    for (const auto reset : {settings::SettingsSectionReset::GlobalMouse,
                             settings::SettingsSectionReset::GlobalHotkeys,
                             settings::SettingsSectionReset::SystemGeneral,
                             settings::SettingsSectionReset::SystemSettings,
                             settings::SettingsSectionReset::ScreenshotCapture,
                             settings::SettingsSectionReset::ScreenshotSettings,
                             settings::SettingsSectionReset::TextRecognition,
                             settings::SettingsSectionReset::OtherShortcuts,
                             settings::SettingsSectionReset::ScreenshotEditorShortcuts}) {
        require(backend.resetSection(reset),
                "reset must skip unavailable integrations successfully");
    }
    for (auto it = inactive.cbegin(); it != inactive.cend(); ++it) {
        require(configuration.value(it.key()) == it.value(),
                "reset must preserve inactive cross-platform preferences");
    }
    for (const auto& key : {QStringLiteral("screenshot/capture_cursor"),
                            QStringLiteral("text_recognition/model_type")}) {
        require(configuration.value(key) == storage::ConfigurationSchema::defaultValue(key),
                "reset must still restore available settings to defaults");
    }
    for (const auto binding :
         {settings::SettingsSwitchBinding::SmartSelection,
          settings::SettingsSwitchBinding::ScreenshotRestoreOriginalScreenColors,
          settings::SettingsSwitchBinding::DisableHotkeysOnFocusedFullscreen,
          settings::SettingsSwitchBinding::AutoStartAtBoot,
          settings::SettingsSwitchBinding::DirectMlAcceleration}) {
        require(!backend.switchEnabled(binding) && !backend.applySwitchValue(binding, true),
                "unsupported switches must reject stale UI writes");
    }
    using Select = settings::SettingsSelectBinding;
    for (const auto& [binding, value] :
         {std::pair{Select::ApplicationPriority, QStringLiteral("normal")},
          std::pair{Select::UpdateMode, QStringLiteral("check")},
          std::pair{Select::ScreenshotApiMode, QStringLiteral("gdi")},
          std::pair{Select::WindowElementApi, QStringLiteral("msaa")}}) {
        require(!backend.applySelectValue(binding, value),
                "unsupported selectors must reject even otherwise valid stale UI writes");
    }
    for (const auto& api : {QStringLiteral("auto"), QStringLiteral("dxgi"), QStringLiteral("wgc"),
                            QStringLiteral("gdi")}) {
        require(configuration.setValue(QStringLiteral("screenshot/api_mode"), api),
                "all portable screenshot API preferences remain accepted");
        require(storage::ScreenshotSettings().apiMode() == QStringLiteral("auto") &&
                    configuration.value(QStringLiteral("screenshot/api_mode")) == api,
                "macOS capture must use Auto without overwriting an imported Windows preference");
    }
}

void selectedTextShortcutNeverRegisters(presentation::GlobalShortcutManager& manager,
                                        FakeShortcutBackend& native) {
    require(storage::ExtendedFeaturesSettings().setTranslationPageEnabled(true),
            "the supported translation page can be enabled");
    manager.setShortcuts(presentation::GlobalShortcutAction::TranslateSelectedText,
                         {QStringLiteral("Ctrl+F18")});
    manager.setShortcuts(presentation::GlobalShortcutAction::Screenshot,
                         {QStringLiteral("Ctrl+F19")});
    QCoreApplication::processEvents();
    require(!native.registrations.values().contains(QStringLiteral("Ctrl+F18")) &&
                manager.state(presentation::GlobalShortcutAction::TranslateSelectedText)
                    .bindings.isEmpty(),
            "enabling Translation must never register unavailable selected-text capture");
    require(native.registrations.values().contains(QStringLiteral("Ctrl+F19")),
            "ordinary screenshot shortcuts must still register");
    int activated = 0;
    QObject::connect(&manager, &presentation::GlobalShortcutManager::activated,
                     [&activated](auto action) {
                         if (action == presentation::GlobalShortcutAction::Screenshot)
                             ++activated;
                     });
    native.activationHandler(native.registrations.key(QStringLiteral("Ctrl+F19")));
    require(activated == 1, "supported shortcut dispatch must remain functional");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    catalogExposesOnlyAvailableIntegrations();
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated settings storage");
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated settings storage");
    {
        auto native = std::make_unique<FakeShortcutBackend>();
        auto* nativePointer = native.get();
        presentation::GlobalShortcutManager shortcuts(std::move(native), nullptr,
                                                      [] { return false; });
        shortcuts.initialize();
        settings::BuiltInSettingsBackend backend(shortcuts);
        resetPreservesInactivePreferences(backend);
        selectedTextShortcutNeverRegisters(shortcuts, *nativePointer);
    }
    appStorage.shutdown();
    std::cout << "macOS settings capabilities, portable resets and shortcut registration passed\n";
}
