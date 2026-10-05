#include "snow_shot/app/edition.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/translationpagecontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationarchive.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/translation/translationservice.h"

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
namespace presentation = snow_shot::presentation;
namespace settings = presentation::settings;
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// Resetting global shortcuts initializes their manager. This fixture exercises
// persistence/reconciliation while keeping every native shortcut operation inert.
class InertShortcutBackend final : public presentation::GlobalShortcutBackend {
  public:
    void setActivationHandler(ActivationHandler) override {}
    presentation::GlobalShortcutValidationResult
    validateShortcut(const snow_shot::shortcuts::ShortcutBinding& binding) const override {
        return {binding.portableText, !binding.portableText.isEmpty(),
                presentation::GlobalShortcutFailureReason::None, binding};
    }
    presentation::GlobalShortcutBackendResult
    registerShortcut(int, const snow_shot::shortcuts::ShortcutBinding&) override {
        return {true, presentation::GlobalShortcutFailureReason::None, 0};
    }
    void unregisterShortcut(int) override {}
};

template <typename T>
concept CompleteType = requires { sizeof(T); };
static_assert(!CompleteType<SnowShotApiClient>);
static_assert(!CompleteType<snow_shot::translation::TranslationService>);
static_assert(!CompleteType<snow_shot::translation::TranslationJob>);
static_assert(!CompleteType<presentation::TranslationPageController>);

template <typename T>
concept HasApiModelConfiguration = requires(const T& settings) { settings.customAiModels(); };
template <typename T>
concept HasTranslationConfiguration =
    requires(const T& settings) { settings.textTranslationConfigurations(); };
template <typename T>
concept HasTranslationShortcut = requires(const T& settings) { settings.screenshotTranslation(); };
template <typename T>
concept HasAutomaticQrRecognition = requires(const T& settings) { settings.autoRecognizeQrCode(); };
template <typename T>
concept HasTableQrEntry = requires(const T& settings) { settings.tableQrTool(); };
template <typename T>
concept HasCredentialPreservation = requires(T& result, const QMap<QString, QJsonValue>& snapshot) {
    result.preserveOmittedCredentials(snapshot);
};
// Mini retains cloud upload, including credential preservation during archive imports.
static_assert(HasCredentialPreservation<storage::ConfigurationArchiveReadResult>);
static_assert(!HasApiModelConfiguration<settings::SettingsBackend>);
static_assert(!HasApiModelConfiguration<settings::SettingsRuntimeSession>);
static_assert(!HasTranslationConfiguration<settings::SettingsBackend>);
static_assert(!HasTranslationConfiguration<settings::SettingsRuntimeSession>);
static_assert(!HasTranslationShortcut<storage::ShortcutSettings>);
static_assert(!HasAutomaticQrRecognition<storage::ScreenshotSettings>);
static_assert(!HasTableQrEntry<storage::ScreenshotToolbarSettings>);

void unsupportedSettingsRemainInert(settings::BuiltInSettingsBackend& backend) {
    const auto before = storage::ApplicationStorage::instance().configuration().snapshot();
    for (const auto binding : {settings::SettingsSwitchBinding::TranslationPageEnabled,
                               settings::SettingsSwitchBinding::JumpToTranslationPage,
                               settings::SettingsSwitchBinding::StandaloneTranslationWindow,
                               settings::SettingsSwitchBinding::OriginalImageTranslation,
                               settings::SettingsSwitchBinding::ScreenshotAutoRecognizeQrCode}) {
        require(!backend.switchEnabled(binding) && !backend.switchValue(binding) &&
                    !backend.applySwitchValue(binding, true),
                "Mini backend must reject unsupported switches even when addressed directly");
    }
    require(!backend.applySelectValue(settings::SettingsSelectBinding::TranslationLayoutProcessing,
                                      QStringLiteral("smart_merge")) &&
                !backend.applyTextValue(settings::SettingsTextBinding::ServerUrl,
                                        QStringLiteral("https://example.test")),
            "Mini backend must reject unavailable translation and API configuration writes");
    for (const auto binding : {settings::SettingsSelectBinding::TranslationSourceLanguage,
                               settings::SettingsSelectBinding::TranslationPrimaryTargetLanguage,
                               settings::SettingsSelectBinding::TranslationSecondaryTargetLanguage,
                               settings::SettingsSelectBinding::TranslationService}) {
        require(!backend.selectValue(binding).isValid() &&
                    !backend.applySelectValue(binding, QStringLiteral("en")),
                "Mini backend must reject shared translation settings");
    }
    require(!storage::ConfigurationSchema::contains(
                QStringLiteral("screenshot_translation/secondary_target_language")),
            "Mini schema must omit secondary translation targets");
    for (const auto reset :
         {settings::SettingsSectionReset::Translation, settings::SettingsSectionReset::Server,
          settings::SettingsSectionReset::CustomAiModels,
          settings::SettingsSectionReset::TextTranslationConfigurations,
          settings::SettingsSectionReset::ExtendedTranslation}) {
        require(!backend.resetSection(reset),
                "Mini backend must reject resets of unavailable configuration sections");
    }
    require(storage::ApplicationStorage::instance().configuration().snapshot() == before,
            "unsupported settings requests must leave Mini configuration unchanged");
}

void requireDefault(const QString& key) {
    const auto expected = storage::ConfigurationSchema::normalize(
        key, storage::ConfigurationSchema::defaultValue(key));
    require(expected.valid && storage::ApplicationStorage::instance().configuration().value(key) ==
                                  expected.value,
            "Mini section reset must restore every retained setting default");
}

void shortcutAndMouseResets(settings::BuiltInSettingsBackend& backend) {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValue(QStringLiteral("global_shortcuts/screenshot"),
                                   QJsonArray{QStringLiteral("Ctrl+Alt+F8")}) &&
                configuration.setValue(QStringLiteral("global_shortcuts/screenshot_ocr"),
                                       QJsonArray{QStringLiteral("Ctrl+Alt+F9")}) &&
                configuration.setValue(QStringLiteral("screenshot/delay_seconds"), 7),
            "customize retained screenshot shortcuts before resetting");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotShortcuts),
            "Mini screenshot shortcut reset must skip unavailable translation");
    requireDefault(QStringLiteral("global_shortcuts/screenshot"));
    requireDefault(QStringLiteral("global_shortcuts/screenshot_ocr"));
    requireDefault(QStringLiteral("screenshot/delay_seconds"));

    require(configuration.setValue(QStringLiteral("global_shortcuts/global_canvas"),
                                   QJsonArray{QStringLiteral("Ctrl+Alt+F10")}),
            "customize retained other shortcut before resetting");
    require(backend.resetSection(settings::SettingsSectionReset::OtherShortcuts),
            "Mini other shortcut reset must skip unavailable selected-text translation");
    requireDefault(QStringLiteral("global_shortcuts/global_canvas"));

    require(backend.applyGlobalMouseCombination(
                settings::SettingsGlobalMouseAction::ScreenshotCopy,
                {{QStringLiteral("shift")}, QStringLiteral("left_drag")}),
            "customize retained global mouse action before resetting");
    require(backend.resetSection(settings::SettingsSectionReset::GlobalMouse),
            "Mini global mouse reset must omit unavailable translation atomically");
    requireDefault(QStringLiteral("global_mouse/screenshot_copy"));
    requireDefault(QStringLiteral("global_mouse/screenshot_ocr"));

    require(backend.applyLocalShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                        QStringLiteral("text_recognition"),
                                        {QStringLiteral("Ctrl+Alt+F8")}),
            "customize retained manual OCR shortcut before resetting");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotOtherShortcuts),
            "Mini local shortcut reset must omit unavailable table, QR and translation entries");
    requireDefault(QStringLiteral("screenshot_shortcuts/text_recognition"));
    requireDefault(QStringLiteral("screenshot_shortcuts/undo"));
    for (const QString& key : {QStringLiteral("global_shortcuts/screenshot_translation"),
                               QStringLiteral("global_shortcuts/translate_selected_text"),
                               QStringLiteral("global_mouse/screenshot_translation"),
                               QStringLiteral("screenshot_shortcuts/table_recognition"),
                               QStringLiteral("screenshot_shortcuts/qr_code_recognition"),
                               QStringLiteral("screenshot_shortcuts/text_translation")}) {
        require(!storage::ConfigurationSchema::contains(key) && configuration.value(key).isNull(),
                "Mini resets must not reintroduce unavailable shortcut or mouse keys");
    }
}

void annotationResetRejectsHistoryCollisions(settings::BuiltInSettingsBackend& backend) {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const auto original = configuration.snapshot();
    const auto drawingDefaults = storage::DrawingShortcutSettings().allShortcuts();
    constexpr auto drawing = settings::SettingsLocalShortcutScope::Drawing;
    constexpr auto screenshot = settings::SettingsLocalShortcutScope::Screenshot;
    const auto redoDefault = backend.localShortcuts(screenshot, QStringLiteral("redo"));
    require(backend.applyLocalShortcuts(drawing, QStringLiteral("line"),
                                        {QStringLiteral("Ctrl+Alt+F11")}) &&
                backend.applyLocalShortcuts(screenshot, QStringLiteral("redo"), {}) &&
                backend.applyLocalShortcuts(screenshot, QStringLiteral("text_recognition"),
                                            redoDefault),
            "configure a Mini annotation reset collision with manual OCR");
    const auto before = configuration.snapshot();
    const auto revision = configuration.revision();
    require(!backend.resetSection(settings::SettingsSectionReset::DrawingShortcuts) &&
                configuration.snapshot() == before && configuration.revision() == revision,
            "Mini Annotation reset must reject history collisions without changing any scope");
    require(backend.applyLocalShortcuts(screenshot, QStringLiteral("text_recognition"), {}) &&
                backend.resetSection(settings::SettingsSectionReset::DrawingShortcuts) &&
                storage::DrawingShortcutSettings().allShortcuts() == drawingDefaults &&
                backend.localShortcuts(screenshot, QStringLiteral("redo")) == redoDefault &&
                backend.localShortcuts(screenshot, QStringLiteral("text_recognition")).isEmpty(),
            "Mini Annotation reset must restore only its available drawing and history defaults");
    require(configuration.applySnapshot(original), "restore Mini annotation reset fixture");
}

void screenshotBehaviorReset(settings::BuiltInSettingsBackend& backend) {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    for (const QString& key : {QStringLiteral("screenshot_selection/smart_selection"),
                               QStringLiteral("screenshot/shutter_sound_notification"),
                               QStringLiteral("screenshot/confirm_before_exiting_via_shortcut")}) {
        require(
            configuration.setValue(key, !storage::ConfigurationSchema::defaultValue(key).toBool()),
            "customize retained screenshot behavior before resetting");
    }
    require(configuration.setValue(QStringLiteral("screenshot/double_click_action"),
                                   QStringLiteral("none")),
            "customize screenshot click action before resetting");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotSettings),
            "Mini screenshot behavior reset must omit unavailable automatic QR recognition");
    for (const QString& key : {QStringLiteral("screenshot_selection/smart_selection"),
                               QStringLiteral("screenshot/shutter_sound_notification"),
                               QStringLiteral("screenshot/confirm_before_exiting_via_shortcut"),
                               QStringLiteral("screenshot/double_click_action")})
        requireDefault(key);
    require(!storage::ConfigurationSchema::contains(
                QStringLiteral("screenshot/auto_recognize_qr_code")),
            "Mini screenshot reset must not restore automatic QR recognition");
}

void recognitionOptInResets(settings::BuiltInSettingsBackend& backend) {
    namespace layout = presentation::toolbar_layout;
    for (const auto kind : {storage::ScreenshotToolbarLayoutKind::ActionTools,
                            storage::ScreenshotToolbarLayoutKind::PinnedActionTools}) {
        const auto original = backend.toolbarLayout(kind);
        require(original.hidden.contains(QStringLiteral("text-recognition")),
                "Mini toolbars initially hide manual OCR");
        const auto enabled =
            layout::moveItemToPosition(original, kind, QStringLiteral("text-recognition"), 0);
        require(
            backend.applyToolbarLayout(kind, enabled) &&
                !backend.toolbarLayout(kind).hidden.contains(QStringLiteral("text-recognition")),
            "Mini toolbar OCR can be explicitly enabled");
        if (kind == storage::ScreenshotToolbarLayoutKind::ActionTools) {
            require(
                backend.resetSection(settings::SettingsSectionReset::ScreenshotInterfaceSettings) &&
                    backend.toolbarLayout(kind) == enabled,
                "Mini capture interface reset must preserve the custom action toolbar");
        }
        const auto reset = kind == storage::ScreenshotToolbarLayoutKind::ActionTools
                               ? settings::SettingsSectionReset::ScreenshotActionToolbar
                               : settings::SettingsSectionReset::PinToScreenToolbar;
        require(backend.resetSection(reset) &&
                    backend.toolbarLayout(kind).hidden.contains(QStringLiteral("text-recognition")),
                "Mini toolbar reset must restore hidden default OCR");
    }
    require(!backend.switchValue(settings::SettingsSwitchBinding::PinAutomaticTextRecognition),
            "Mini automatic pinned OCR initially defaults off");
    require(backend.applySwitchValue(settings::SettingsSwitchBinding::PinAutomaticTextRecognition,
                                     true) &&
                backend.switchValue(settings::SettingsSwitchBinding::PinAutomaticTextRecognition),
            "Mini automatic pinned OCR can be explicitly enabled");
    require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                !backend.switchValue(settings::SettingsSwitchBinding::PinAutomaticTextRecognition),
            "Mini pin behavior reset must restore automatic OCR off");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    require(snow_shot::app::edition::isMini, "settings reset fixture must be compiled as Mini");
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated Mini reset storage");
    const QString executable = temporary.filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create isolated Mini reset executable directory");
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize({executable, temporary.path(), 60000}).success,
            "initialize isolated Mini reset storage");
    {
        presentation::GlobalShortcutManager shortcuts(std::make_unique<InertShortcutBackend>());
        settings::BuiltInSettingsBackend backend(shortcuts);
        unsupportedSettingsRemainInert(backend);
        shortcutAndMouseResets(backend);
        annotationResetRejectsHistoryCollisions(backend);
        screenshotBehaviorReset(backend);
        recognitionOptInResets(backend);
    }
    appStorage.shutdown();
    return 0;
}
