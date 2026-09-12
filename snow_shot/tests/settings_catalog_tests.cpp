#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/storage/configurationschema.h"

#include "antd_icons.h"

#include <QCoreApplication>
#include <QSet>
#include <QTranslator>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;
namespace presentation = snow_shot::presentation;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

settings::TranslatableText text(const char* source) {
    return {"SettingsCatalogTests", source};
}

bool insertUnique(QSet<QString>* values, const QString& value) {
    if (values->contains(value)) {
        return false;
    }
    values->insert(value);
    return true;
}

class CatalogTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* sourceText, const char*,
                      int) const override {
        if (QString::fromLatin1(context) != QStringLiteral("SettingsCatalog")) {
            return {};
        }
        const QString source = QString::fromUtf8(sourceText);
        if (source == QStringLiteral("Middle Mouse Button Action")) {
            return QStringLiteral("Localized Middle Action");
        }
        if (source == QStringLiteral("Reset Zoom")) {
            return QStringLiteral("Localized Zoom Reset");
        }
        if (source == QStringLiteral("Theme")) {
            return QStringLiteral("Localized Theme");
        }
        if (source == QStringLiteral("Dark")) {
            return QStringLiteral("Night Mode");
        }
        if (source == QStringLiteral("Small V4")) {
            return QStringLiteral("Localized Small V4");
        }
        if (source == QStringLiteral("Appearance")) {
            return QStringLiteral("Visual Style");
        }
        return {};
    }
};

void builtInCatalogIsCompleteAndValid() {
    const settings::SettingsCatalog& catalog = settings::builtInSettingsRegistry().catalog();
    require(catalog.validationErrors().isEmpty(), "built-in settings catalog must validate");
    const auto* primary =
        catalog.item({QStringLiteral("interface-settings"), QStringLiteral("general"),
                      QStringLiteral("interface.theme-primary-color")});
    require(primary != nullptr &&
                primary->configurationKey == QStringLiteral("interface/theme_primary_color") &&
                std::get<settings::SettingsColorDefinition>(primary->payload).binding ==
                    settings::SettingsColorBinding::ThemePrimaryColor &&
                !std::get<settings::SettingsColorDefinition>(primary->payload).alphaChannelEnabled,
            "general settings must expose an opaque theme primary color picker");
    require(catalog.pages().size() == 12, "catalog must contain twelve pages");

    const auto* extended = catalog.page(QStringLiteral("extended-features"));
    const auto* translationToggle =
        catalog.item({QStringLiteral("extended-features"), QStringLiteral("translation"),
                      QStringLiteral("extended-features.translation-page")});
    require(extended != nullptr &&
                extended->route == QStringLiteral("/settings/extended-features") &&
                translationToggle != nullptr &&
                translationToggle->title.translated() == QStringLiteral("Translation Page") &&
                !snow_shot::storage::ConfigurationSchema::defaultValue(
                     translationToggle->configurationKey)
                     .toBool(true),
            "extended translation page exposes a persisted default-off toggle");
    qsizetype sectionCount = 0;
    qsizetype itemCount = 0;
    bool foundUpdates = false;
    QSet<QString> objectNames;
    for (const auto& page : catalog.pages()) {
        sectionCount += page.sections.size();
        require(!page.id.isEmpty() && !page.route.isEmpty() && page.title.isValid() &&
                    page.description.isValid(),
                "page metadata must be complete");
        require(insertUnique(&objectNames, settings::generatedObjectName(
                                               QStringLiteral("settings-page"), page.id)),
                "generated page object names must be unique");
        for (const auto& section : page.sections) {
            itemCount += section.items.size();
            require(
                insertUnique(&objectNames, settings::generatedObjectName(
                                               QStringLiteral("settings-section"),
                                               QStringLiteral("%1-%2").arg(page.id, section.id))),
                "generated section object names must be unique");
            for (const auto& item : section.items) {
                require(insertUnique(&objectNames, settings::generatedObjectName(
                                                       QStringLiteral("settings-item"), item.id)),
                        "generated item object names must be unique");
                if (!item.configurationKey.isEmpty()) {
                    require(storage::ConfigurationSchema::entry(item.configurationKey) != nullptr,
                            "catalog persistence keys must resolve through ConfigurationSchema");
                }
                if (item.configurationKey == QStringLiteral("updates/mode")) {
                    const auto& select = std::get<settings::SettingsSelectDefinition>(item.payload);
                    require(
                        select.binding == settings::SettingsSelectBinding::UpdateMode &&
                            select.options.size() == 3 &&
                            select.options[0].value == QStringLiteral("manual") &&
                            select.options[1].value == QStringLiteral("check") &&
                            select.options[2].value == QStringLiteral("download") &&
                            storage::ConfigurationSchema::defaultValue(item.configurationKey) ==
                                QStringLiteral("download"),
                        "update policy exposes all three modes with automatic download default");
                    foundUpdates = true;
                }
            }
        }
    }
    require(sectionCount == 36 && itemCount == 146 && foundUpdates,
            "catalog must contain thirty-six sections and one hundred forty-six items");
    const auto* pinnedEditor =
        catalog.item({QStringLiteral("interface-settings"), QStringLiteral("pin-to-screen"),
                      QStringLiteral("interface.pin-to-screen.pinned-toolbar-editor")});
    require(pinnedEditor != nullptr &&
                pinnedEditor->configurationKey ==
                    QStringLiteral("pin_to_screen/action_tools_layout") &&
                std::get<settings::SettingsCustomDefinition>(pinnedEditor->payload).renderer ==
                    settings::SettingsCustomRenderer::PinnedToolbarEditor,
            "Interface Settings must expose the pinned editor in Pin to Screen");
    const auto* history =
        catalog.section(QStringLiteral("storage-and-privacy"), QStringLiteral("history"));
    require(history != nullptr && history->items.size() >= 2 &&
                history->items[0].id == QStringLiteral("history.enabled") &&
                history->items[1].id == QStringLiteral("history.keep-permanently"),
            "permanent history must follow persistent history in Storage and Privacy");
    const auto& permanent = history->items[1];
    require(permanent.title.translated() == QStringLiteral("Keep records permanently") &&
                std::get<settings::SettingsSwitchDefinition>(permanent.payload).binding ==
                    settings::SettingsSwitchBinding::HistoryKeepPermanently &&
                storage::ConfigurationSchema::defaultValue(permanent.configurationKey) == false,
            "permanent history must be a default-off switch with the requested label");
    const auto* fill = catalog.item({QStringLiteral("interface-settings"),
                                     QStringLiteral("interface-text-recognition"),
                                     QStringLiteral("interface.text-recognition.fill-style")});
    require(fill != nullptr && fill->title.translated() == QStringLiteral("Fill Style"),
            "Interface Settings must expose the OCR Fill Style setting");
    const auto& fillSelect = std::get<settings::SettingsSelectDefinition>(fill->payload);
    require(fillSelect.binding == settings::SettingsSelectBinding::OcrFillStyle &&
                fillSelect.options.size() == 2 &&
                fillSelect.options[0].value == QStringLiteral("blur") &&
                fillSelect.options[1].value == QStringLiteral("background_fill") &&
                storage::ConfigurationSchema::defaultValue(fill->configurationKey) ==
                    QStringLiteral("background_fill"),
            "OCR Fill Style must offer Blur and Background Fill, defaulting to Background Fill");
    const auto* saveDialog =
        catalog.item({QStringLiteral("function-settings"), QStringLiteral("screenshot-settings"),
                      QStringLiteral("screenshot.save-as-file-dialog")});
    require(saveDialog &&
                saveDialog->configurationKey == QStringLiteral("screenshot/save_as_file_dialog"),
            "Save as file dialog must be in Function screenshot settings");
    const auto& saveSelect = std::get<settings::SettingsSelectDefinition>(saveDialog->payload);
    require(saveSelect.binding == settings::SettingsSelectBinding::ScreenshotSaveAsFileDialog &&
                saveSelect.options.size() == 2 &&
                saveSelect.options[0].value == QStringLiteral("system") &&
                saveSelect.options[1].value == QStringLiteral("snow_shot"),
            "save dialog options are incorrect");
    const auto* apiMode =
        catalog.item({QStringLiteral("system-settings"), QStringLiteral("screenshot-capture"),
                      QStringLiteral("screenshot.api-mode")});
    require(apiMode != nullptr &&
                apiMode->configurationKey == QStringLiteral("screenshot/api_mode") &&
                std::get<settings::SettingsSelectDefinition>(apiMode->payload).binding ==
                    settings::SettingsSelectBinding::ScreenshotApiMode,
            "capture API and color restoration must share the system screenshot section");
    const auto* windowElementApi =
        catalog.item({QStringLiteral("system-settings"), QStringLiteral("screenshot-capture"),
                      QStringLiteral("screenshot.window-element-api")});
    const auto* windowElementSelect =
        windowElementApi != nullptr
            ? std::get_if<settings::SettingsSelectDefinition>(&windowElementApi->payload)
            : nullptr;
    require(windowElementApi != nullptr && windowElementSelect != nullptr &&
                windowElementApi->title.translated() == QStringLiteral("Window Element API") &&
                windowElementApi->configurationKey ==
                    QStringLiteral("screenshot/window_element_api") &&
                windowElementSelect->binding == settings::SettingsSelectBinding::WindowElementApi &&
                windowElementSelect->options.size() == 2 &&
                windowElementSelect->options.at(0).value == QStringLiteral("msaa") &&
                windowElementSelect->options.at(0).label.translated() == QStringLiteral("MSAA") &&
                windowElementSelect->options.at(1).value == QStringLiteral("uia") &&
                windowElementSelect->options.at(1).label.translated() == QStringLiteral("UIA") &&
                storage::ConfigurationSchema::defaultValue(windowElementApi->configurationKey) ==
                    QStringLiteral("msaa"),
            "system Screenshot settings must expose MSAA and UIA with MSAA as the default");
    const auto* colorRestoration =
        catalog.item({QStringLiteral("system-settings"), QStringLiteral("screenshot-capture"),
                      QStringLiteral("screenshot.restore-original-screen-colors")});
    require(colorRestoration != nullptr &&
                colorRestoration->configurationKey ==
                    QStringLiteral("screenshot/restore_original_screen_colors") &&
                std::get<settings::SettingsSwitchDefinition>(colorRestoration->payload).binding ==
                    settings::SettingsSwitchBinding::ScreenshotRestoreOriginalScreenColors,
            "screen color restoration must be a system screenshot switch");
    const auto* captureCursor =
        catalog.item({QStringLiteral("system-settings"), QStringLiteral("screenshot-capture"),
                      QStringLiteral("screenshot.capture-cursor")});
    const auto* screenshotCaptureSection =
        catalog.section(QStringLiteral("system-settings"), QStringLiteral("screenshot-capture"));
    require(
        captureCursor != nullptr && screenshotCaptureSection != nullptr &&
            screenshotCaptureSection->items.size() == 4 &&
            screenshotCaptureSection->items.at(2).id ==
                QStringLiteral("screenshot.restore-original-screen-colors") &&
            screenshotCaptureSection->items.at(3).id ==
                QStringLiteral("screenshot.capture-cursor") &&
            captureCursor->title.translated() == QStringLiteral("Capture cursor") &&
            captureCursor->description.translated() ==
                QStringLiteral("Include the mouse cursor in normal screenshots.") &&
            captureCursor->configurationKey == QStringLiteral("screenshot/capture_cursor") &&
            std::get<settings::SettingsSwitchDefinition>(captureCursor->payload).binding ==
                settings::SettingsSwitchBinding::ScreenshotCaptureCursor &&
            !storage::ConfigurationSchema::defaultValue(captureCursor->configurationKey).toBool(),
        "cursor capture must be the disabled final switch in system Screenshot settings");
    const auto* functionPage = catalog.page(QStringLiteral("function-settings"));
    const auto* shutterSound =
        catalog.item({QStringLiteral("function-settings"), QStringLiteral("screenshot-settings"),
                      QStringLiteral("screenshot.shutter-sound-notification")});
    const auto* screenshotSettings =
        catalog.section(QStringLiteral("function-settings"), QStringLiteral("screenshot-settings"));
    require(screenshotSettings != nullptr && !screenshotSettings->items.isEmpty() &&
                screenshotSettings->items.constLast().id ==
                    QStringLiteral("screenshot.shutter-sound-notification"),
            "shutter notification must be the final item in Function Screenshot settings");
    require(shutterSound != nullptr &&
                shutterSound->title.translated() == QStringLiteral("Shutter Sound Notification") &&
                shutterSound->configurationKey ==
                    QStringLiteral("screenshot/shutter_sound_notification") &&
                std::get<settings::SettingsSwitchDefinition>(shutterSound->payload).binding ==
                    settings::SettingsSwitchBinding::ScreenshotShutterSoundNotification &&
                storage::ConfigurationSchema::defaultValue(shutterSound->configurationKey).toBool(),
            "Function Screenshot settings must expose the enabled shutter notification switch");
    const auto* smartSelection =
        catalog.item({QStringLiteral("function-settings"), QStringLiteral("screenshot-settings"),
                      QStringLiteral("screenshot.smart-selection")});
    require(functionPage != nullptr &&
                functionPage->route == QStringLiteral("/settings/functionSettings") &&
                smartSelection != nullptr &&
                smartSelection->configurationKey ==
                    QStringLiteral("screenshot_selection/smart_selection") &&
                std::get<settings::SettingsSwitchDefinition>(smartSelection->payload).binding ==
                    settings::SettingsSwitchBinding::SmartSelection,
            "Function settings must expose the persisted Smart selection switch");
    const QStringList expectedSections{QStringLiteral("screenshot-settings"),
                                       QStringLiteral("pin-to-screen-settings"),
                                       QStringLiteral("text-recognition-settings"),
                                       QStringLiteral("translation-settings"),
                                       QStringLiteral("drawing-settings"),
                                       QStringLiteral("screen-recording-settings"),
                                       QStringLiteral("tray-settings"),
                                       QStringLiteral("global-hotkeys")};
    require(functionPage->sections.size() == expectedSections.size(), "function section count");
    for (qsizetype i = 0; i < expectedSections.size(); ++i)
        require(functionPage->sections.at(i).id == expectedSections.at(i),
                "function section order");
    const auto& recognition = functionPage->sections.at(2);
    require(recognition.reset == settings::SettingsSectionReset::TextRecognitionBehavior &&
                recognition.title.translated() == QStringLiteral("Text Recognition") &&
                recognition.items.size() == 1,
            "dedicated recognition save section");
    const auto& recognitionSave = recognition.items.front();
    require(
        recognitionSave.title.translated() == QStringLiteral("Save recognition result as image") &&
            std::get<settings::SettingsSwitchDefinition>(recognitionSave.payload).binding ==
                settings::SettingsSwitchBinding::SaveRecognitionResultAsImage &&
            storage::ConfigurationSchema::defaultValue(recognitionSave.configurationKey).toBool(),
        "recognition save switch must default on");
    const auto* translation =
        catalog.item({QStringLiteral("function-settings"), QStringLiteral("translation-settings"),
                      QStringLiteral("translation.original-image")});
    require(translation != nullptr &&
                translation->title.translated() == QStringLiteral("Original Image Translation") &&
                translation->configurationKey ==
                    QStringLiteral("screenshot_translation/original_image_translation") &&
                std::get<settings::SettingsSwitchDefinition>(translation->payload).binding ==
                    settings::SettingsSwitchBinding::OriginalImageTranslation &&
                functionPage->sections.at(3).reset == settings::SettingsSectionReset::Translation &&
                storage::ConfigurationSchema::defaultValue(translation->configurationKey).toBool(),
            "Translation should expose its own default-on switch and section reset");
    const auto* layout =
        catalog.item({QStringLiteral("function-settings"), QStringLiteral("translation-settings"),
                      QStringLiteral("translation.layout-processing")});
    require(
        layout != nullptr && layout->title.translated() == QStringLiteral("Layout Processing") &&
            layout->configurationKey == QStringLiteral("screenshot_translation/layout_processing"),
        "Translation should expose layout processing");
    const auto& layoutOptions =
        std::get<settings::SettingsSelectDefinition>(layout->payload).options;
    require(layoutOptions.size() == 2 && layoutOptions[0].value == QStringLiteral("smart_merge") &&
                layoutOptions[0].label.translated() == QStringLiteral("Smart Merge") &&
                layoutOptions[1].value == QStringLiteral("original") &&
                layoutOptions[1].label.translated() == QStringLiteral("Original"),
            "layout processing must expose Smart Merge and Original");
    const auto* encodingPreset = catalog.item({QStringLiteral("function-settings"),
                                               QStringLiteral("screen-recording-settings"),
                                               QStringLiteral("screen-recording.encoding-preset")});
    require(encodingPreset != nullptr,
            "Function settings must expose the video encoding preset selector");
    const auto& encodingPresetOptions =
        std::get<settings::SettingsSelectDefinition>(encodingPreset->payload).options;
    require(encodingPresetOptions.size() == 5 &&
                encodingPresetOptions.at(0).value == QStringLiteral("ultrafast") &&
                encodingPresetOptions.at(0).label.translated() == QStringLiteral("Ultra fast") &&
                encodingPresetOptions.at(1).value == QStringLiteral("veryfast") &&
                encodingPresetOptions.at(1).label.translated() == QStringLiteral("Very fast") &&
                encodingPresetOptions.at(2).value == QStringLiteral("medium") &&
                encodingPresetOptions.at(2).label.translated() == QStringLiteral("Medium") &&
                encodingPresetOptions.at(3).value == QStringLiteral("veryslow") &&
                encodingPresetOptions.at(3).label.translated() == QStringLiteral("Very slow") &&
                encodingPresetOptions.at(4).value == QStringLiteral("placebo") &&
                encodingPresetOptions.at(4).label.translated() ==
                    QStringLiteral("Maximum compression"),
            "encoding preset labels must be localized independently of their persisted values");
    require(
        catalog.item({QStringLiteral("function-settings"), QStringLiteral("pin-to-screen-settings"),
                      QStringLiteral("pin-to-screen.mouse-wheel-zoom-mode")}) != nullptr &&
            catalog.item({QStringLiteral("function-settings"),
                          QStringLiteral("pin-to-screen-settings"),
                          QStringLiteral("pin-to-screen.automatic-text-recognition")}) != nullptr &&
            catalog.item({QStringLiteral("function-settings"),
                          QStringLiteral("pin-to-screen-settings"),
                          QStringLiteral("pin-to-screen.auto-resize-window")}) != nullptr &&
            catalog.item({QStringLiteral("function-settings"), QStringLiteral("drawing-settings"),
                          QStringLiteral("drawing.quick-selection-disabled-tools")}) != nullptr &&
            catalog.item({QStringLiteral("function-settings"), QStringLiteral("tray-settings"),
                          QStringLiteral("tray.left-click-action")}) != nullptr &&
            catalog.item({QStringLiteral("function-settings"), QStringLiteral("tray-settings"),
                          QStringLiteral("tray.menu-options")}) != nullptr,
        "Function settings must own the moved Pin to screen, Drawing, and Tray controls");

    const auto& traySection = functionPage->sections.at(6);
    require(traySection.items.size() == 3 &&
                traySection.items.at(0).id == QStringLiteral("tray.left-click-action") &&
                traySection.items.at(1).id == QStringLiteral("tray.middle-click-action") &&
                traySection.items.at(2).id == QStringLiteral("tray.menu-options"),
            "middle-click action must appear immediately below left-click action");
    const auto& leftTray =
        std::get<settings::SettingsSelectDefinition>(traySection.items.at(0).payload);
    const auto& middleTray =
        std::get<settings::SettingsSelectDefinition>(traySection.items.at(1).payload);
    require(leftTray.binding == settings::SettingsSelectBinding::TrayLeftClickAction &&
                middleTray.binding == settings::SettingsSelectBinding::TrayMiddleClickAction &&
                leftTray.options.size() == 5 && middleTray.options.size() == 5,
            "tray selectors must expose independent bindings and five options");
    const QStringList trayActionValues{
        QStringLiteral("screenshot"), QStringLiteral("show_main_window"),
        QStringLiteral("screenshot_copy"), QStringLiteral("screenshot_fixed"),
        QStringLiteral("open_function_settings")};
    for (int index = 0; index < trayActionValues.size(); ++index) {
        require(leftTray.options.at(index).value == trayActionValues.at(index) &&
                    middleTray.options.at(index).value == trayActionValues.at(index) &&
                    leftTray.options.at(index).label.translated() ==
                        middleTray.options.at(index).label.translated(),
                "tray selectors must share ordered values and labels");
    }

    const auto* pinDoubleClick =
        catalog.item({QStringLiteral("function-settings"), QStringLiteral("pin-to-screen-settings"),
                      QStringLiteral("pin-to-screen.double-click-action")});
    require(pinDoubleClick != nullptr &&
                pinDoubleClick->title.translated() == QStringLiteral("Double-click Action"),
            "Pin to Screen must expose Double-click Action");
    const auto& pinSelect = std::get<settings::SettingsSelectDefinition>(pinDoubleClick->payload);
    require(pinSelect.binding == settings::SettingsSelectBinding::PinDoubleClickAction &&
                pinSelect.options.size() == 3 &&
                pinSelect.options[0].value == QStringLiteral("none") &&
                pinSelect.options[0].label.translated() == QStringLiteral("None") &&
                pinSelect.options[1].value == QStringLiteral("thumbnail_mode") &&
                pinSelect.options[1].label.translated() == QStringLiteral("Thumbnail Mode") &&
                pinSelect.options[2].value == QStringLiteral("close") &&
                pinSelect.options[2].label.translated() == QStringLiteral("Close"),
            "pinned double-click options must preserve the specified order, labels and values");
    require(functionPage->sections.at(1).items.at(1).id == pinDoubleClick->id,
            "pinned double-click must follow mouse wheel zoom mode");

    const auto* pinMiddleClick =
        catalog.item({QStringLiteral("function-settings"), QStringLiteral("pin-to-screen-settings"),
                      QStringLiteral("pin-to-screen.middle-mouse-button-action")});
    require(pinMiddleClick != nullptr &&
                pinMiddleClick->title.translated() == QStringLiteral("Middle Mouse Button Action"),
            "Pin to Screen must expose Middle Mouse Button Action");
    const auto& middleSelect =
        std::get<settings::SettingsSelectDefinition>(pinMiddleClick->payload);
    require(middleSelect.binding == settings::SettingsSelectBinding::PinMiddleClickAction &&
                middleSelect.options.size() == 4 &&
                middleSelect.options[0].value == QStringLiteral("none") &&
                middleSelect.options[0].label.translated() == QStringLiteral("None") &&
                middleSelect.options[1].value == QStringLiteral("reset_zoom") &&
                middleSelect.options[1].label.translated() == QStringLiteral("Reset Zoom") &&
                middleSelect.options[2].value == QStringLiteral("thumbnail_mode") &&
                middleSelect.options[2].label.translated() == QStringLiteral("Thumbnail Mode") &&
                middleSelect.options[3].value == QStringLiteral("close") &&
                middleSelect.options[3].label.translated() == QStringLiteral("Close"),
            "pinned middle-click options must preserve the specified order, labels and values");
    require(functionPage->sections.at(1).items.at(2).id == pinMiddleClick->id,
            "pinned middle-click must follow double-click action");

    const auto* storagePage = catalog.page(QStringLiteral("storage-and-privacy"));
    const auto* imageFormat =
        catalog.item({QStringLiteral("storage-and-privacy"), QStringLiteral("screenshots"),
                      QStringLiteral("screenshot-output.image-format")});
    const auto* imageDirectory =
        catalog.item({QStringLiteral("storage-and-privacy"), QStringLiteral("screenshots"),
                      QStringLiteral("screenshot-output.image-save-directory")});
    const auto* videoFilename = catalog.item(
        {QStringLiteral("storage-and-privacy"), QStringLiteral("screen-recording-output"),
         QStringLiteral("screen-recording-output.video-filename-format")});
    require(
        storagePage != nullptr && storagePage->sections.size() == 4 &&
            storagePage->sections.at(0).id == QStringLiteral("screenshots") &&
            storagePage->sections.at(1).id == QStringLiteral("screen-recording-output") &&
            storagePage->sections.at(2).id == QStringLiteral("history") &&
            storagePage->sections.at(2).title.source != nullptr &&
            QString::fromLatin1(storagePage->sections.at(2).title.source) ==
                QStringLiteral("Screenshot history") &&
            storagePage->sections.at(3).id == QStringLiteral("storage-status") &&
            imageFormat != nullptr && imageDirectory != nullptr && videoFilename != nullptr &&
            std::get<settings::SettingsSelectDefinition>(imageFormat->payload).options.size() ==
                6 &&
            std::get<settings::SettingsSelectDefinition>(imageFormat->payload)
                    .options.at(2)
                    .value == QStringLiteral("bmp") &&
            std::get<settings::SettingsDirectoryPathDefinition>(imageDirectory->payload).binding ==
                settings::SettingsDirectoryPathBinding::ScreenshotImageDirectory &&
            std::get<settings::SettingsTextDefinition>(videoFilename->payload).binding ==
                settings::SettingsTextBinding::ScreenRecordingVideoFilenameFormat,
        "Storage and privacy must expose ordered screenshot and recording output settings");

    const auto* systemPage = catalog.page(QStringLiteral("system-settings"));
    const auto* proxy = catalog.item({QStringLiteral("system-settings"), QStringLiteral("network"),
                                      QStringLiteral("network.proxy")});
    const auto* proxySelect = proxy != nullptr
                                  ? std::get_if<settings::SettingsSelectDefinition>(&proxy->payload)
                                  : nullptr;
    const auto* textRecognition =
        catalog.section(QStringLiteral("system-settings"), QStringLiteral("text-recognition"));
    const auto* modelType =
        catalog.item({QStringLiteral("system-settings"), QStringLiteral("text-recognition"),
                      QStringLiteral("text-recognition.model-type")});
    const auto* modelTypeSelect =
        modelType != nullptr ? std::get_if<settings::SettingsSelectDefinition>(&modelType->payload)
                             : nullptr;
    require(
        systemPage != nullptr && systemPage->sections.size() == 5 &&
            systemPage->sections.at(0).id == QStringLiteral("system-general") &&
            systemPage->sections.at(1).id == QStringLiteral("screenshot-capture") &&
            systemPage->sections.at(1).reset == settings::SettingsSectionReset::ScreenshotCapture &&
            systemPage->sections.at(2).id == QStringLiteral("network") &&
            systemPage->sections.at(3).id == QStringLiteral("text-recognition") &&
            systemPage->sections.at(4).id == QStringLiteral("core") && proxy != nullptr &&
            proxy->configurationKey == QStringLiteral("network/proxy") && proxySelect != nullptr &&
            proxySelect->binding == settings::SettingsSelectBinding::Proxy &&
            proxySelect->options.size() == 2 &&
            proxySelect->options.at(0).value == QStringLiteral("none") &&
            proxySelect->options.at(1).value == QStringLiteral("system") &&
            textRecognition != nullptr &&
            textRecognition->reset == settings::SettingsSectionReset::TextRecognition &&
            textRecognition->items.size() == 2 &&
            textRecognition->items.at(0).id == QStringLiteral("text-recognition.model-type") &&
            textRecognition->items.at(1).id ==
                QStringLiteral("text-recognition.direct-ml-acceleration") &&
            modelType != nullptr &&
            modelType->configurationKey == QStringLiteral("text_recognition/model_type") &&
            modelTypeSelect != nullptr &&
            modelTypeSelect->binding == settings::SettingsSelectBinding::OcrModelType &&
            modelTypeSelect->options.size() == 7 &&
            modelTypeSelect->options.at(0).value == QStringLiteral("extra_small") &&
            modelTypeSelect->options.at(1).value == QStringLiteral("small") &&
            modelTypeSelect->options.at(2).value == QStringLiteral("medium") &&
            modelTypeSelect->options.at(3).value == QStringLiteral("small_v5") &&
            modelTypeSelect->options.at(4).value == QStringLiteral("medium_v5") &&
            modelTypeSelect->options.at(5).value == QStringLiteral("small_v4") &&
            modelTypeSelect->options.at(6).value == QStringLiteral("medium_v4"),
        "System settings must expose the ordered OCR model and acceleration controls");
    const QStringList modelLabels{QStringLiteral("Ultra Small V6"), QStringLiteral("Small V6"),
                                  QStringLiteral("Medium V6"),      QStringLiteral("Small V5"),
                                  QStringLiteral("Medium V5"),      QStringLiteral("Small V4"),
                                  QStringLiteral("Medium V4")};
    for (qsizetype index = 0; index < modelLabels.size(); ++index) {
        require(modelTypeSelect->options.at(index).label.translated() == modelLabels.at(index),
                "OCR model labels must distinguish model generation and size");
    }

    const settings::SettingsNavigationGroupDefinition* settingsGroup = nullptr;
    for (const auto& node : catalog.navigation()) {
        if (const auto* group = std::get_if<settings::SettingsNavigationGroupDefinition>(&node);
            group != nullptr && group->id == QStringLiteral("nav.settings")) {
            settingsGroup = group;
            break;
        }
    }
    require(settingsGroup != nullptr && settingsGroup->pages.size() >= 2 &&
                settingsGroup->pages.at(0).pageId == QStringLiteral("interface-settings") &&
                settingsGroup->pages.at(1).pageId == QStringLiteral("function-settings"),
            "Function settings must appear below Interface settings in the Settings navigation");
    require(settingsGroup->title.translated() == QStringLiteral("Settings") &&
                settingsGroup->pages.size() == 7 &&
                settingsGroup->pages.at(3).pageId == QStringLiteral("storage-and-privacy") &&
                settingsGroup->pages.at(4).pageId == QStringLiteral("api-configuration") &&
                settingsGroup->pages.at(5).pageId == QStringLiteral("extended-features") &&
                settingsGroup->pages.at(2).pageId == QStringLiteral("application-shortcuts") &&
                settingsGroup->pages.constLast().pageId == QStringLiteral("system-settings"),
            "Settings navigation group must expose Application shortcuts and System settings");
    const auto* applicationShortcutsPage = catalog.page(QStringLiteral("application-shortcuts"));
    require(applicationShortcutsPage != nullptr &&
                applicationShortcutsPage->route ==
                    QStringLiteral("/settings/applicationShortcuts") &&
                applicationShortcutsPage->title.translated() ==
                    QStringLiteral("Application shortcuts") &&
                settingsGroup->pages.at(2).id == QStringLiteral("nav.application-shortcuts"),
            "Application shortcuts must expose the renamed title, route, and navigation");
    const auto* drawingShortcuts = catalog.section(QStringLiteral("application-shortcuts"),
                                                   QStringLiteral("drawing-shortcuts"));
    const auto* screenshotShortcuts = catalog.section(QStringLiteral("application-shortcuts"),
                                                      QStringLiteral("screenshot-shortcuts"));
    const auto* otherShortcutSection =
        catalog.section(QStringLiteral("application-shortcuts"), QStringLiteral("other-shortcuts"));
    const auto* pinToScreenShortcuts = catalog.section(QStringLiteral("application-shortcuts"),
                                                       QStringLiteral("pin-to-screen-shortcuts"));
    const auto* recordingShortcuts = catalog.section(QStringLiteral("application-shortcuts"),
                                                     QStringLiteral("screen-recording-shortcuts"));
    require(recordingShortcuts != nullptr && recordingShortcuts->items.size() == 4 &&
                recordingShortcuts->reset ==
                    settings::SettingsSectionReset::ScreenRecordingShortcuts,
            "recording shortcuts must expose a resettable settings section");
    const QStringList recordingActions = {
        QStringLiteral("export"), QStringLiteral("toggle_recording"),
        QStringLiteral("copy_to_clipboard"), QStringLiteral("end_recording")};
    for (qsizetype index = 0; index < recordingActions.size(); ++index) {
        const auto& item = recordingShortcuts->items.at(index);
        require(item.id ==
                        QStringLiteral("screen-recording-shortcut.") + recordingActions.at(index) &&
                    item.configurationKey == QStringLiteral("screen_recording_shortcuts/") +
                                                 recordingActions.at(index) &&
                    std::get<settings::SettingsLocalShortcutDefinition>(item.payload).scope ==
                        settings::SettingsLocalShortcutScope::ScreenRecording,
                "recording shortcut fields must use stable IDs and a dedicated local scope");
    }
    const bool everyHotkeySectionUsesTwoColumns =
        applicationShortcutsPage != nullptr &&
        std::all_of(
            applicationShortcutsPage->sections.cbegin(), applicationShortcutsPage->sections.cend(),
            [](const settings::SettingsSectionDefinition& section) {
                return section.itemLayout == settings::SettingsSectionItemLayout::TwoColumnGrid;
            });
    struct ScreenshotShortcutContract {
        int index;
        const char* id;
        const char* configurationKey;
    };
    const ScreenshotShortcutContract newScreenshotShortcutContracts[] = {
        {5, "screenshot-shortcut.move_entire_selection",
         "screenshot_shortcuts/move_entire_selection"},
        {6, "screenshot-shortcut.keep_selection_width_and_height_consistent",
         "screenshot_shortcuts/keep_selection_width_and_height_consistent"},
        {7, "screenshot-shortcut.switch_selection_between_window_and_window_sub_element",
         "screenshot_shortcuts/switch_selection_between_window_and_window_sub_element"},
        {8, "screenshot-shortcut.previous_screenshot_history",
         "screenshot_shortcuts/previous_screenshot_history"},
        {9, "screenshot-shortcut.next_screenshot_history",
         "screenshot_shortcuts/next_screenshot_history"},
        {10, "screenshot-shortcut.select_previously_selected_area",
         "screenshot_shortcuts/select_previously_selected_area"},
        {11, "screenshot-shortcut.copy_color", "screenshot_shortcuts/copy_color"},
        {12, "screenshot-shortcut.pin_to_screen", "screenshot_shortcuts/pin_to_screen"},
        {13, "screenshot-shortcut.video_recording", "screenshot_shortcuts/video_recording"},
        {14, "screenshot-shortcut.scrolling_screenshot",
         "screenshot_shortcuts/scrolling_screenshot"},
        {15, "screenshot-shortcut.quick_save", "screenshot_shortcuts/quick_save"},
        {16, "screenshot-shortcut.save_as_file", "screenshot_shortcuts/save_as_file"},
        {17, "screenshot-shortcut.cancel_screenshot", "screenshot_shortcuts/cancel_screenshot"},
        {18, "screenshot-shortcut.copy_to_clipboard", "screenshot_shortcuts/copy_to_clipboard"},
    };
    bool newScreenshotShortcutContractsMatch = screenshotShortcuts != nullptr;
    for (const ScreenshotShortcutContract& contract : newScreenshotShortcutContracts) {
        newScreenshotShortcutContractsMatch =
            newScreenshotShortcutContractsMatch &&
            screenshotShortcuts->items.size() > contract.index &&
            screenshotShortcuts->items.at(contract.index).id == QString::fromLatin1(contract.id) &&
            screenshotShortcuts->items.at(contract.index).configurationKey ==
                QString::fromLatin1(contract.configurationKey);
    }
    require(
        applicationShortcutsPage != nullptr && applicationShortcutsPage->sections.size() == 5 &&
            everyHotkeySectionUsesTwoColumns && screenshotShortcuts != nullptr &&
            screenshotShortcuts->items.size() == 19 &&
            screenshotShortcuts->itemLayout == settings::SettingsSectionItemLayout::TwoColumnGrid &&
            screenshotShortcuts->items.constFirst().id ==
                QStringLiteral("screenshot-shortcut.move_tool") &&
            screenshotShortcuts->items.at(1).configurationKey ==
                QStringLiteral("screenshot_shortcuts/move_cursor_up") &&
            screenshotShortcuts->items.at(5).title.translated() ==
                QStringLiteral("Move entire selection") &&
            screenshotShortcuts->items.at(6).title.translated() ==
                QStringLiteral("Keep selection width and height consistent") &&
            screenshotShortcuts->items.at(7).title.translated() ==
                QStringLiteral("Select window/window sub-element") &&
            screenshotShortcuts->items.at(8).title.translated() ==
                QStringLiteral("Previous screenshot history") &&
            screenshotShortcuts->items.at(9).title.translated() ==
                QStringLiteral("Next screenshot history") &&
            screenshotShortcuts->items.at(10).title.translated() ==
                QStringLiteral("Select previously selected area") &&
            screenshotShortcuts->items.at(11).title.translated() == QStringLiteral("Copy color") &&
            screenshotShortcuts->items.at(12).title.translated() ==
                QStringLiteral("Pin to screen") &&
            screenshotShortcuts->items.at(13).title.translated() ==
                QStringLiteral("Video recording") &&
            screenshotShortcuts->items.at(14).title.translated() ==
                QStringLiteral("Scrolling screenshot") &&
            screenshotShortcuts->items.at(15).title.translated() == QStringLiteral("Quick save") &&
            screenshotShortcuts->items.at(16).title.translated() ==
                QStringLiteral("Save as file") &&
            screenshotShortcuts->items.at(17).title.translated() ==
                QStringLiteral("Cancel screenshot") &&
            screenshotShortcuts->items.at(18).title.translated() ==
                QStringLiteral("Copy to clipboard") &&
            newScreenshotShortcutContractsMatch &&
            std::get<settings::SettingsLocalShortcutDefinition>(
                screenshotShortcuts->items.constFirst().payload)
                    .scope == settings::SettingsLocalShortcutScope::Screenshot &&
            drawingShortcuts != nullptr && drawingShortcuts->items.size() == 10 &&
            drawingShortcuts->itemLayout == settings::SettingsSectionItemLayout::TwoColumnGrid &&
            pinToScreenShortcuts != nullptr && pinToScreenShortcuts->items.size() == 11 &&
            pinToScreenShortcuts->itemLayout ==
                settings::SettingsSectionItemLayout::TwoColumnGrid &&
            pinToScreenShortcuts->title.translated() == QStringLiteral("Pin to screen") &&
            pinToScreenShortcuts->reset == settings::SettingsSectionReset::PinToScreenShortcuts &&
            pinToScreenShortcuts->items.constFirst().id ==
                QStringLiteral("pin-to-screen-shortcut.copy_to_clipboard") &&
            pinToScreenShortcuts->items.at(2).configurationKey ==
                QStringLiteral("pin_to_screen_shortcuts/save_as_file") &&
            pinToScreenShortcuts->items.at(3).configurationKey ==
                QStringLiteral("pin_to_screen_shortcuts/show_text_recognition_results") &&
            std::get<settings::SettingsLocalShortcutDefinition>(
                pinToScreenShortcuts->items.constFirst().payload)
                    .scope == settings::SettingsLocalShortcutScope::PinToScreen &&
            otherShortcutSection != nullptr && otherShortcutSection->items.size() == 6 &&
            otherShortcutSection->itemLayout ==
                settings::SettingsSectionItemLayout::TwoColumnGrid &&
            otherShortcutSection->title.translated() == QStringLiteral("Other") &&
            otherShortcutSection->items.at(4).id == QStringLiteral("screenshot-shortcut.undo") &&
            otherShortcutSection->items.at(5).id == QStringLiteral("screenshot-shortcut.redo") &&
            drawingShortcuts->items.constFirst().id == QStringLiteral("drawing-shortcut.select") &&
            drawingShortcuts->items.at(1).id == QStringLiteral("drawing-shortcut.shape"),
        "Application shortcuts must expose Screenshot before Drawing with stable local shortcuts");

    const auto* interfacePage = catalog.page(QStringLiteral("interface-settings"));
    require(interfacePage != nullptr && interfacePage->sections.size() == 7 &&
                interfacePage->sections.at(1).id == QStringLiteral("interface-screenshot") &&
                interfacePage->sections.at(2).id == QStringLiteral("interface-text-recognition") &&
                interfacePage->sections.at(3).id == QStringLiteral("toolbar") &&
                interfacePage->sections.at(4).id == QStringLiteral("drawing") &&
                interfacePage->sections.at(5).id == QStringLiteral("pin-to-screen") &&
                interfacePage->sections.at(6).id == QStringLiteral("tray"),
            "Interface settings must place Text Recognition immediately below Screenshot");
    const auto* toolbarSize =
        catalog.item({QStringLiteral("interface-settings"), QStringLiteral("toolbar"),
                      QStringLiteral("interface.screenshot.toolbar-size")});
    const auto* toolbarEditor =
        catalog.item({QStringLiteral("interface-settings"), QStringLiteral("drawing"),
                      QStringLiteral("interface.toolbar.drawing-toolbar-editor")});
    const auto* screenshotToolbarEditor =
        catalog.item({QStringLiteral("interface-settings"), QStringLiteral("interface-screenshot"),
                      QStringLiteral("interface.screenshot.screenshot-toolbar-editor")});
    const auto& screenshotSection = interfacePage->sections.at(1);
    const auto& toolbarSection = interfacePage->sections.at(3);
    const auto* trayIcon =
        catalog.item({QStringLiteral("interface-settings"), QStringLiteral("tray"),
                      QStringLiteral("interface.tray.icon")});
    require(
        toolbarSize != nullptr && toolbarEditor != nullptr && screenshotToolbarEditor != nullptr &&
            trayIcon != nullptr &&
            screenshotSection.items.constLast().id == screenshotToolbarEditor->id &&
            catalog.item({QStringLiteral("interface-settings"), QStringLiteral("drawing"),
                          QStringLiteral("drawing.quick-selection-disabled-tools")}) == nullptr &&
            catalog.item({QStringLiteral("interface-settings"), QStringLiteral("pin-to-screen"),
                          QStringLiteral("pin-to-screen.mouse-wheel-zoom-mode")}) == nullptr &&
            catalog.item({QStringLiteral("interface-settings"), QStringLiteral("tray"),
                          QStringLiteral("tray.left-click-action")}) == nullptr &&
            toolbarSize->configurationKey == QStringLiteral("screenshot_ui/toolbar_size") &&
            toolbarEditor->configurationKey == QStringLiteral("screenshot_toolbar/layout") &&
            toolbarSection.title.translated() == QStringLiteral("Toolbar") &&
            toolbarSection.searchDescription.translated() ==
                QStringLiteral("Configure the screenshot, pinned, and recording toolbars") &&
            toolbarEditor->title.translated() == QStringLiteral("Drawing toolbar settings") &&
            toolbarEditor->description.translated() ==
                QStringLiteral("Drag drawing tools to reorder them or stack them in the same "
                               "toolbar position.") &&
            toolbarEditor->aliases.size() == 2 &&
            toolbarEditor->aliases.at(0).translated() == QStringLiteral("Tool positions") &&
            toolbarEditor->aliases.at(1).translated() == QStringLiteral("Stack drawing tools") &&
            screenshotToolbarEditor->configurationKey ==
                QStringLiteral("screenshot_toolbar/action_tools_layout") &&
            screenshotToolbarEditor->title.translated() ==
                QStringLiteral("Screenshot toolbar settings") &&
            screenshotToolbarEditor->description.translated() ==
                QStringLiteral("Drag screenshot tools to reorder them or stack them in the same "
                               "toolbar position.") &&
            screenshotToolbarEditor->aliases.size() == 3 &&
            screenshotToolbarEditor->aliases.at(0).translated() ==
                QStringLiteral("Custom screenshot toolbar") &&
            screenshotToolbarEditor->aliases.at(1).translated() ==
                QStringLiteral("Tool positions") &&
            screenshotToolbarEditor->aliases.at(2).translated() ==
                QStringLiteral("Stack screenshot tools") &&
            std::get<settings::SettingsCustomDefinition>(screenshotToolbarEditor->payload)
                    .renderer == settings::SettingsCustomRenderer::ScreenshotToolbarEditor &&
            screenshotSection.reset ==
                settings::SettingsSectionReset::ScreenshotInterfaceSettings &&
            trayIcon->configurationKey == QStringLiteral("tray/icon") &&
            std::get<settings::SettingsRadioDefinition>(trayIcon->payload).options.size() == 6,
        "new Interface settings controls must retain their schema contracts");

    const auto& pinSection = interfacePage->sections.at(5);
    const auto* pinBorderActiveColor =
        catalog.item({QStringLiteral("interface-settings"), QStringLiteral("pin-to-screen"),
                      QStringLiteral("interface.pin-to-screen.border-active-color")});
    require(
        pinSection.items.size() == 3 && pinBorderActiveColor != nullptr &&
            pinBorderActiveColor->configurationKey ==
                QStringLiteral("pin_to_screen/border_active_color") &&
            std::get<settings::SettingsColorDefinition>(pinBorderActiveColor->payload).binding ==
                settings::SettingsColorBinding::PinBorderActiveColor &&
            storage::ConfigurationSchema::defaultValue(pinBorderActiveColor->configurationKey) ==
                QStringLiteral("#69B1FFFF"),
        "pin to screen must expose a border active color defaulting to #69b1ff");

    const auto* retention =
        storage::ConfigurationSchema::entry(QStringLiteral("capture_history/retention_days"));
    const auto* shortcuts =
        storage::ConfigurationSchema::entry(QStringLiteral("global_shortcuts/screenshot"));
    require(retention != nullptr &&
                retention->valueKind == storage::ConfigurationValueKind::Integer &&
                retention->integerRange.has_value() && retention->integerRange->minimum == 1 &&
                retention->integerRange->maximum == 365,
            "integer schema metadata must expose renderer constraints");
    require(shortcuts != nullptr &&
                shortcuts->valueKind == storage::ConfigurationValueKind::StringList &&
                shortcuts->maximumListItems == 2,
            "shortcut schema metadata must expose list limits");
}

void globalMouseSettingsHaveStableContracts() {
    using Action = settings::SettingsGlobalMouseAction;
    struct Expectation {
        const char* id;
        const char* title;
        const char* key;
        Action action;
    };
    const Expectation expectations[] = {
        {"global-mouse.screenshot-copy", "Copy to clipboard", "global_mouse/screenshot_copy",
         Action::ScreenshotCopy},
        {"global-mouse.screenshot-fixed", "Pin to screen", "global_mouse/screenshot_fixed",
         Action::ScreenshotFixed},
        {"global-mouse.screenshot-ocr", "Text recognition", "global_mouse/screenshot_ocr",
         Action::ScreenshotOcr},
        {"global-mouse.screenshot-translation", "Text translation",
         "global_mouse/screenshot_translation", Action::ScreenshotTranslation},
        {"global-mouse.screenshot-save", "Save as file", "global_mouse/screenshot_save",
         Action::ScreenshotSave},
        {"global-mouse.screenshot-quick-save", "Quick save", "global_mouse/screenshot_quick_save",
         Action::ScreenshotQuickSave},
        {"global-mouse.screen-recording", "Screen recording", "global_mouse/screen_recording",
         Action::ScreenRecording},
    };

    const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
    const settings::SettingsCatalog& catalog = registry.catalog();
    const auto* page = catalog.page(QStringLiteral("global-mouse"));
    const auto* section =
        catalog.section(QStringLiteral("global-mouse"), QStringLiteral("screenshot"));
    require(page != nullptr && page->route == QStringLiteral("/global-mouse") &&
                page->sections.size() == 2 && section == &page->sections.constFirst(),
            "Global mouse must be a stable top-level page with two categories");
    const auto* recording =
        catalog.section(QStringLiteral("global-mouse"), QStringLiteral("screen-recording"));
    require(recording != nullptr && recording->items.size() == 1 &&
                recording->title.translated() == QStringLiteral("Screen recording") &&
                recording->reset == settings::SettingsSectionReset::GlobalMouse &&
                recording->itemLayout == settings::SettingsSectionItemLayout::VerticalList,
            "Screen recording must have its own category and one configurable function");
    require(section->reset == settings::SettingsSectionReset::GlobalMouse &&
                section->itemLayout == settings::SettingsSectionItemLayout::VerticalList &&
                section->items.size() == 6,
            "Global mouse Screenshot must use action containers and reset all six fields");

    const auto& navigation = catalog.navigation();
    const auto* quick = std::get_if<settings::SettingsNavigationPageDefinition>(&navigation.at(0));
    const auto* globalMouse =
        std::get_if<settings::SettingsNavigationPageDefinition>(&navigation.at(1));
    const auto* history =
        std::get_if<settings::SettingsNavigationPageDefinition>(&navigation.at(2));
    const auto* translation =
        std::get_if<settings::SettingsNavigationPageDefinition>(&navigation.at(3));
    require(
        quick != nullptr && quick->pageId == QStringLiteral("global-hotkeys") &&
            globalMouse != nullptr && globalMouse->id == QStringLiteral("nav.global-mouse") &&
            globalMouse->pageId == QStringLiteral("global-mouse") && globalMouse->iconFactory &&
            globalMouse->iconFactory() ==
                snow_shot::presentation::icons::custom::outlined::WheelMouse() &&
            translation != nullptr && translation->pageId == QStringLiteral("translation") &&
            translation->iconFactory && history != nullptr &&
            history->pageId == QStringLiteral("screenshot-history"),
        "navigation order must be Global hotkeys, Global mouse, Screenshot history, Translation");

    for (qsizetype index = 0; index < std::size(expectations); ++index) {
        const auto& expected = expectations[index];
        const auto& item = index < section->items.size() ? section->items.at(index)
                                                         : recording->items.constFirst();
        const auto* payload =
            std::get_if<settings::SettingsGlobalMouseActionDefinition>(&item.payload);
        const auto* descriptor = registry.field(QString::fromLatin1(expected.id));
        require(
            item.id == QString::fromLatin1(expected.id) &&
                item.title.translated() == QString::fromLatin1(expected.title) &&
                item.description.translated() == QString::fromLatin1(expected.title) &&
                item.configurationKey == QString::fromLatin1(expected.key) && payload != nullptr &&
                payload->action == expected.action && descriptor != nullptr &&
                descriptor->kind == settings::SettingsFieldKind::GlobalMouseAction &&
                descriptor->defaultValue ==
                    storage::ConfigurationSchema::defaultValue(QString::fromLatin1(expected.key)) &&
                registry.fieldForGlobalMouseAction(expected.action) == descriptor,
            "Global mouse action metadata and typed registry bindings must remain stable");
    }
    require(registry.fieldsForReset(settings::SettingsSectionReset::GlobalMouse).size() == 7,
            "Global mouse reset metadata must cover every Screenshot action");
}

void globalHotkeyShortcutsHaveStableContracts() {
    using Action = snow_shot::presentation::GlobalShortcutAction;
    const auto* hotkeysPage =
        settings::builtInSettingsRegistry().catalog().page(QStringLiteral("global-hotkeys"));
    require(hotkeysPage != nullptr &&
                hotkeysPage->title.translated() == QStringLiteral("Global hotkeys") &&
                hotkeysPage->description.translated() == QStringLiteral("Global hotkeys page"),
            "global hotkeys must expose the renamed page and description");

    struct ShortcutExpectation {
        Action action;
        const char* sectionId;
        const char* itemId;
        const char* configurationKey;
        settings::SettingsCommandKind commandKind;
        settings::SettingsShortcutAdjustment adjustment =
            settings::SettingsShortcutAdjustment::None;
    };

    const QVector<ShortcutExpectation> expectations{
        {Action::Screenshot, "screenshot", "quick.screenshot", "global_shortcuts/screenshot",
         settings::SettingsCommandKind::CaptureScreenshot},
        {Action::ScreenshotDelay, "screenshot", "quick.screenshot-delay",
         "global_shortcuts/screenshot_delay", settings::SettingsCommandKind::ExecuteQuickAction,
         settings::SettingsShortcutAdjustment::ScreenshotDelaySeconds},
        {Action::ScreenshotFixed, "screenshot", "quick.screenshot-fixed",
         "global_shortcuts/screenshot_fixed", settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::ScreenshotOcr, "screenshot", "quick.screenshot-ocr",
         "global_shortcuts/screenshot_ocr", settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::ScreenshotTranslation, "screenshot", "quick.screenshot-translation",
         "global_shortcuts/screenshot_translation",
         settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::ScreenshotCopy, "screenshot", "quick.screenshot-copy",
         "global_shortcuts/screenshot_copy", settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::ScreenshotFullScreen, "screenshot", "quick.screenshot-full-screen",
         "global_shortcuts/screenshot_full_screen",
         settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::ScreenshotFocusedWindow, "screenshot", "quick.screenshot-focused-window",
         "global_shortcuts/screenshot_focused_window",
         settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::ScreenRecord, "screen-recording", "quick.screen-record",
         "global_shortcuts/screen_record", settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::ScreenRecordCopy, "screen-recording", "quick.screen-record-copy",
         "global_shortcuts/screen_record_copy", settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::OpenCaptureHistory, "other", "quick.open-capture-history",
         "global_shortcuts/open_capture_history",
         settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::TranslateSelectedText, "other", "quick.translate-selected-text",
         "global_shortcuts/translate_selected_text",
         settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::PinClipboardContent, "other", "quick.pin-clipboard-content",
         "global_shortcuts/pin_clipboard_content",
         settings::SettingsCommandKind::ExecuteQuickAction},
        {Action::PinSelectedFiles, "other", "quick.pin-selected-files",
         "global_shortcuts/pin_selected_files", settings::SettingsCommandKind::ExecuteQuickAction},
    };

    const settings::SettingsCatalog& catalog = settings::builtInSettingsRegistry().catalog();
    QSet<Action> actions;
    for (const ShortcutExpectation& expectation : expectations) {
        const settings::SettingsLocation location{QStringLiteral("global-hotkeys"),
                                                  QString::fromLatin1(expectation.sectionId),
                                                  QString::fromLatin1(expectation.itemId)};
        const auto* item = catalog.item(location);
        require(item != nullptr, "every global-hotkey shortcut item must exist");
        require(item->configurationKey == QString::fromLatin1(expectation.configurationKey),
                "global-hotkey shortcut persistence keys must remain stable");

        const auto* shortcut =
            std::get_if<settings::SettingsShortcutActionDefinition>(&item->payload);
        require(shortcut != nullptr && shortcut->shortcutAction == expectation.action &&
                    shortcut->command.kind == expectation.commandKind &&
                    shortcut->adjustment == expectation.adjustment,
                "global-hotkey shortcut payloads must match their declared action contracts");
        require(!actions.contains(shortcut->shortcutAction),
                "global-hotkey shortcut actions must be unique");
        actions.insert(shortcut->shortcutAction);

        const auto command = catalog.commandForShortcut(expectation.action);
        require(command.has_value() && command->kind == expectation.commandKind,
                "every global-hotkey shortcut must resolve to its configured command");
        if (expectation.commandKind == settings::SettingsCommandKind::CaptureScreenshot ||
            expectation.commandKind == settings::SettingsCommandKind::ExecuteQuickAction) {
            require(shortcut->command.shortcutAction == expectation.action &&
                        command->shortcutAction == expectation.action,
                    "action commands must dispatch their originating shortcut action");
        }
    }

    require(actions.size() == expectations.size() && expectations.size() == 14,
            "the global-hotkeys catalog must expose all fourteen shortcut actions exactly once");
    require(!catalog.commandForShortcut(Action::OpenSettings).has_value(),
            "Open Interface settings must not appear in Global hotkeys");

    const auto trayGroups = catalog.trayMenuGroups();
    QStringList trayOptionIds;
    for (const auto& group : trayGroups) {
        for (const auto& option : group.options) {
            trayOptionIds.push_back(option.id);
            if (option.kind == settings::SettingsTrayMenuOptionKind::QuickAction) {
                const auto command = catalog.commandForShortcut(option.shortcutAction);
                require(command.has_value(),
                        "every tray quick action must resolve to a shortcut command");
                const auto* quickItem = catalog.itemForShortcut(option.shortcutAction);
                require(quickItem != nullptr && quickItem->title.source == option.label.source,
                        "tray quick-action labels must share the shortcut title source");
                const auto title = catalog.shortcutActionTitle(option.shortcutAction, 7);
                require(!title.isEmpty() && !title.contains(QStringLiteral("%1")),
                        "tray quick-action labels must come from resolved canonical titles");
            }
        }
    }
    const auto* trayMenuSchema =
        storage::ConfigurationSchema::entry(QStringLiteral("tray/menu_options"));
    require(trayGroups.size() == 4 && trayGroups.at(0).id == QStringLiteral("screenshot") &&
                trayGroups.at(0).options.size() == 8 &&
                trayGroups.at(1).id == QStringLiteral("screen-recording") &&
                trayGroups.at(1).options.size() == 2 &&
                trayGroups.at(2).id == QStringLiteral("other") &&
                trayGroups.at(2).options.size() == 4 &&
                trayGroups.at(3).id == QStringLiteral("system") &&
                trayGroups.at(3).options.size() == 4 && trayOptionIds.size() == 18 &&
                trayOptionIds.at(12) == QStringLiteral("quick.pin-selected-files") &&
                trayOptionIds.at(13) == QStringLiteral("quick.translate-selected-text") &&
                trayOptionIds.at(14) == QStringLiteral("tray.window-grouping") &&
                trayGroups.at(3).options.at(0).kind ==
                    settings::SettingsTrayMenuOptionKind::WindowGrouping &&
                trayOptionIds.at(15) == QStringLiteral("tray.disable-shortcut-functions") &&
                trayOptionIds.at(16) == QStringLiteral("tray.show-main-window") &&
                trayOptionIds.at(17) == QStringLiteral("tray.exit") && trayMenuSchema != nullptr &&
                trayMenuSchema->allowedStringValues == trayOptionIds,
            "tray menu options must derive all global-hotkey groups and append system commands");

    const auto* delaySchema =
        storage::ConfigurationSchema::entry(QStringLiteral("screenshot/delay_seconds"));
    require(delaySchema != nullptr &&
                delaySchema->valueKind == storage::ConfigurationValueKind::Integer &&
                delaySchema->defaultValue.toInt() == 3 && delaySchema->integerRange.has_value() &&
                delaySchema->integerRange->minimum == 1 && delaySchema->integerRange->maximum == 10,
            "delayed screenshots must use the persisted 3-second default and 1-10 second range");

    const auto* ocrItem =
        catalog.item({QStringLiteral("global-hotkeys"), QStringLiteral("screenshot"),
                      QStringLiteral("quick.screenshot-ocr")});
    const auto* ocrShortcut =
        ocrItem != nullptr
            ? std::get_if<settings::SettingsShortcutActionDefinition>(&ocrItem->payload)
            : nullptr;
    require(ocrShortcut != nullptr && ocrShortcut->iconFactory &&
                ocrShortcut->iconFactory() ==
                    snow_shot::presentation::icons::custom::outlined::ToolRecognizeText(),
            "Text recognition quick action must use the screenshot toolbar OCR icon");
    const auto* translationItem =
        catalog.item({QStringLiteral("global-hotkeys"), QStringLiteral("screenshot"),
                      QStringLiteral("quick.screenshot-translation")});
    const auto* translationShortcut =
        translationItem != nullptr
            ? std::get_if<settings::SettingsShortcutActionDefinition>(&translationItem->payload)
            : nullptr;
    const auto* screenshotSection =
        catalog.section(QStringLiteral("global-hotkeys"), QStringLiteral("screenshot"));
    require(translationItem != nullptr && translationItem->title.source != nullptr &&
                QString::fromLatin1(translationItem->title.source) ==
                    QStringLiteral("Text translation") &&
                translationShortcut != nullptr && translationShortcut->iconFactory &&
                translationShortcut->iconFactory() ==
                    snow_shot::presentation::icons::custom::outlined::OcrTranslate() &&
                screenshotSection != nullptr && screenshotSection->items.size() >= 5 &&
                screenshotSection->items.at(3).id == QStringLiteral("quick.screenshot-ocr") &&
                screenshotSection->items.at(4).id == QStringLiteral("quick.screenshot-translation"),
            "Text translation must appear directly after Text recognition with its toolbar icon");

    const auto* recordingSection =
        catalog.section(QStringLiteral("global-hotkeys"), QStringLiteral("screen-recording"));
    require(recordingSection != nullptr && recordingSection->title.source != nullptr &&
                QString::fromLatin1(recordingSection->title.source) ==
                    QStringLiteral("Screen recording") &&
                recordingSection->items.size() == 2,
            "Screen recording must expose exactly its two quick actions");

    const auto* screenRecord =
        catalog.item({QStringLiteral("global-hotkeys"), QStringLiteral("screen-recording"),
                      QStringLiteral("quick.screen-record")});
    const auto* screenRecordCopy =
        catalog.item({QStringLiteral("global-hotkeys"), QStringLiteral("screen-recording"),
                      QStringLiteral("quick.screen-record-copy")});
    const auto* openHistory =
        catalog.item({QStringLiteral("global-hotkeys"), QStringLiteral("other"),
                      QStringLiteral("quick.open-capture-history")});
    const auto* pinClipboard =
        catalog.item({QStringLiteral("global-hotkeys"), QStringLiteral("other"),
                      QStringLiteral("quick.pin-clipboard-content")});
    const auto* otherShortcuts =
        catalog.section(QStringLiteral("global-hotkeys"), QStringLiteral("other"));
    require(otherShortcuts != nullptr && otherShortcuts->items.size() == 4 &&
                otherShortcuts->items.at(0).id == QStringLiteral("quick.open-capture-history") &&
                otherShortcuts->items.at(1).id == QStringLiteral("quick.pin-clipboard-content") &&
                otherShortcuts->items.at(2).id == QStringLiteral("quick.pin-selected-files") &&
                otherShortcuts->items.at(3).id == QStringLiteral("quick.translate-selected-text"),
            "Other quick actions expose history, clipboard pinning and selected text translation");
    const auto shortcutPayload = [](const settings::SettingsItemDefinition* item) {
        return item != nullptr
                   ? std::get_if<settings::SettingsShortcutActionDefinition>(&item->payload)
                   : nullptr;
    };
    const auto* screenRecordShortcut = shortcutPayload(screenRecord);
    const auto* screenRecordCopyShortcut = shortcutPayload(screenRecordCopy);
    const auto* openHistoryShortcut = shortcutPayload(openHistory);
    const auto* pinClipboardShortcut = shortcutPayload(pinClipboard);
    require(screenRecord != nullptr && screenRecord->title.source != nullptr &&
                QString::fromLatin1(screenRecord->title.source) ==
                    QStringLiteral("Screen recording") &&
                screenRecordShortcut != nullptr && screenRecordShortcut->iconFactory &&
                screenRecordShortcut->iconFactory() ==
                    snow_shot::presentation::icons::custom::outlined::RecordScreen(),
            "Screen recording must use the screenshot toolbar recording icon");
    require(screenRecordCopy != nullptr && screenRecordCopy->title.source != nullptr &&
                QString::fromLatin1(screenRecordCopy->title.source) ==
                    QStringLiteral("Record/Copy Video") &&
                screenRecordCopyShortcut != nullptr && screenRecordCopyShortcut->iconFactory &&
                screenRecordCopyShortcut->iconFactory() ==
                    snow_shot::presentation::icons::custom::outlined::ScreenshotCopy(),
            "recording toggle must use the exact screenshot-copy icon");
    require(openHistory != nullptr && openHistory->title.source != nullptr &&
                QString::fromLatin1(openHistory->title.source) ==
                    QStringLiteral("Screenshot history") &&
                openHistoryShortcut != nullptr && openHistoryShortcut->iconFactory &&
                openHistoryShortcut->iconFactory() == adqt::icons::antd::outlined::History(),
            "Screenshot history must use the History outlined icon");
    require(pinClipboard != nullptr && pinClipboard->title.source != nullptr &&
                QString::fromLatin1(pinClipboard->title.source) ==
                    QStringLiteral("Pin clipboard content to screen") &&
                pinClipboardShortcut != nullptr && pinClipboardShortcut->iconFactory &&
                pinClipboardShortcut->iconFactory() ==
                    snow_shot::presentation::icons::custom::outlined::PinToScreen(),
            "clipboard pinning must use the Pin to screen outlined icon");
}

void compactTrayManifestMatchesRegistryCatalog() {
    const settings::TrayCommandManifest compact = settings::builtInTrayCommandManifest();
    const auto& catalog = settings::builtInSettingsRegistry().catalog();
    const auto projectedGroups = catalog.trayMenuGroups();
    require(compact.groups.size() == projectedGroups.size(),
            "the compact tray manifest must preserve catalog group count");
    for (int groupIndex = 0; groupIndex < compact.groups.size(); ++groupIndex) {
        const auto& compactGroup = compact.groups.at(groupIndex);
        const auto& projectedGroup = projectedGroups.at(groupIndex);
        require(compactGroup.id == projectedGroup.id &&
                    compactGroup.options.size() == projectedGroup.options.size(),
                "the compact tray manifest must preserve group and option ordering");
        for (int optionIndex = 0; optionIndex < compactGroup.options.size(); ++optionIndex) {
            const auto& compactOption = compactGroup.options.at(optionIndex);
            const auto& projectedOption = projectedGroup.options.at(optionIndex);
            const bool optionMatches =
                compactOption.id == projectedOption.id &&
                compactOption.kind == projectedOption.kind &&
                compactOption.shortcutAction == projectedOption.shortcutAction &&
                QString::fromLatin1(compactOption.label.context) ==
                    QString::fromLatin1(projectedOption.label.context) &&
                QString::fromLatin1(compactOption.label.source) ==
                    QString::fromLatin1(projectedOption.label.source) &&
                compactOption.label.translated() == projectedOption.label.translated() &&
                compactOption.iconFactory && projectedOption.iconFactory &&
                compactOption.iconFactory() == projectedOption.iconFactory();
            require(optionMatches,
                    "the compact tray manifest must preserve option behavior and presentation");
        }
    }

    for (const presentation::GlobalShortcutAction action :
         {presentation::GlobalShortcutAction::Screenshot,
          presentation::GlobalShortcutAction::ScreenshotDelay,
          presentation::GlobalShortcutAction::ScreenshotFixed,
          presentation::GlobalShortcutAction::ScreenshotOcr,
          presentation::GlobalShortcutAction::ScreenshotTranslation,
          presentation::GlobalShortcutAction::ScreenshotCopy,
          presentation::GlobalShortcutAction::ScreenshotFullScreen,
          presentation::GlobalShortcutAction::ScreenshotFocusedWindow,
          presentation::GlobalShortcutAction::ScreenRecord,
          presentation::GlobalShortcutAction::ScreenRecordCopy,
          presentation::GlobalShortcutAction::OpenCaptureHistory,
          presentation::GlobalShortcutAction::PinClipboardContent,
          presentation::GlobalShortcutAction::PinSelectedFiles,
          presentation::GlobalShortcutAction::TranslateSelectedText}) {
        require(compact.shortcutActionTitle(action, 0) == catalog.shortcutActionTitle(action, 0) &&
                    compact.shortcutActionTitle(action, 99) ==
                        catalog.shortcutActionTitle(action, 99),
                "compact tray titles must match catalog titles and clamp runtime values");
    }
}

void structuredFallbackIsDeterministic() {
    const settings::SettingsCatalog& catalog = settings::builtInSettingsRegistry().catalog();
    require(catalog.resolveLocation({QStringLiteral("interface-settings"), {}, {}}) ==
                settings::SettingsLocation{
                    QStringLiteral("interface-settings"), QStringLiteral("general"), {}},
            "page locations must reveal their first section");
    require(catalog.resolveLocation({QStringLiteral("storage-and-privacy"),
                                     QStringLiteral("missing"), QStringLiteral("missing")}) ==
                settings::SettingsLocation{
                    QStringLiteral("storage-and-privacy"), QStringLiteral("screenshots"), {}},
            "invalid section and item locations must fall back within their page");
    require(catalog.resolveLocation({QStringLiteral("storage-and-privacy"),
                                     QStringLiteral("history"), QStringLiteral("missing")}) ==
                settings::SettingsLocation{
                    QStringLiteral("storage-and-privacy"), QStringLiteral("history"), {}},
            "invalid item locations must retain their valid section");
    require(catalog.resolveLocation({QStringLiteral("missing"), {}, {}}) ==
                catalog.defaultLocation(),
            "invalid page locations must use the configured default location");
    require(catalog.resolveLocation({QStringLiteral("screenshot-history"), {}, {}}) ==
                settings::SettingsLocation{QStringLiteral("screenshot-history"), {}, {}},
            "custom pages without generated sections must retain their route location");
}

void invalidCatalogReportsAllConformanceErrors() {
    const settings::SettingsCatalog& builtIn = settings::builtInSettingsRegistry().catalog();
    QVector<settings::SettingsPageDefinition> pages = builtIn.pages();
    QVector<settings::SettingsNavigationNode> navigation = builtIn.navigation();

    const auto mutablePage = [&pages](const QString& id) -> settings::SettingsPageDefinition& {
        const auto found = std::find_if(pages.begin(), pages.end(),
                                        [&id](const auto& page) { return page.id == id; });
        require(found != pages.end(), "validation fixture page must exist");
        return *found;
    };
    auto& interfacePage = mutablePage(QStringLiteral("interface-settings"));
    auto& storagePage = mutablePage(QStringLiteral("storage-and-privacy"));
    interfacePage.route = pages[0].route;
    interfacePage.sections[0].items[0].configurationKey = QStringLiteral("interface/language");
    interfacePage.sections[0].items[1].id = QStringLiteral("interface-theme");
    storagePage.sections[0].items[1].configurationKey = QStringLiteral("missing/key");
    auto& custom =
        std::get<settings::SettingsCustomDefinition>(storagePage.sections[3].items[0].payload);
    custom.renderer = static_cast<settings::SettingsCustomRenderer>(999);
    pages.push_back({QStringLiteral("empty-page"),
                     QStringLiteral("relative-route"),
                     text("Empty Page"),
                     text("Empty page description"),
                     {}});

    settings::SettingsNavigationGroupDefinition* group = nullptr;
    for (auto& node : navigation) {
        if (auto* candidate = std::get_if<settings::SettingsNavigationGroupDefinition>(&node);
            candidate != nullptr && candidate->id == QStringLiteral("nav.settings")) {
            group = candidate;
            break;
        }
    }
    require(group != nullptr, "built-in Settings navigation group must exist");
    group->pages[0].pageId = QStringLiteral("missing-page");

    const settings::SettingsCatalog invalid(
        std::move(pages), std::move(navigation),
        {QStringLiteral("missing-page"), QStringLiteral("missing-section"), {}});
    const QString errors = invalid.validationErrors().join(u'\n');
    require(errors.contains(QStringLiteral("duplicate route")),
            "catalog validation must report route uniqueness errors");
    require(errors.contains(QStringLiteral("generated object name")),
            "catalog validation must report generated object-name collisions");
    require(errors.contains(QStringLiteral("select binding is incompatible")),
            "catalog validation must report binding-to-schema incompatibility");
    require(errors.contains(QStringLiteral("unknown configuration key")),
            "catalog validation must report unknown persistence keys");
    require(errors.contains(QStringLiteral("custom item is incomplete")),
            "catalog validation must report unsupported custom renderers");
    require(errors.contains(QStringLiteral("page has no sections")) &&
                errors.contains(QStringLiteral("page route must be absolute")),
            "catalog validation must report invalid hierarchy metadata");
    require(errors.contains(QStringLiteral("navigation references unknown page")) &&
                errors.contains(QStringLiteral("page is absent from navigation")),
            "catalog validation must report unresolved navigation references");
    require(errors.contains(QStringLiteral("invalid default location")),
            "catalog validation must report an invalid default location");
}

void searchIndexIsGeneratedAndRanked() {
    settings::SettingsSearchIndex index(settings::builtInSettingsRegistry());
    require(index.entries().size() == 194 && index.search(QString()).size() == 194,
            "search must generate all catalog nodes in catalog order");
    const auto selectedFiles = index.search(QStringLiteral("Pin Selected Files to Screen"));
    require(!selectedFiles.isEmpty() &&
                selectedFiles.first().location.itemId == QStringLiteral("quick.pin-selected-files"),
            "selected-file pinning must be searchable by its action title");
    const auto updates = index.search(QStringLiteral("Software updates"));
    require(!updates.isEmpty() &&
                updates.constFirst().location.itemId == QStringLiteral("updates.mode"),
            "update policy is directly discoverable through settings search");
    const auto selectedText = index.search(QStringLiteral("Translate Selected Text"));
    require(!selectedText.isEmpty() &&
                selectedText.constFirst().location ==
                    settings::SettingsLocation{QStringLiteral("global-hotkeys"),
                                               QStringLiteral("other"),
                                               QStringLiteral("quick.translate-selected-text")},
            "search navigates directly to the selected text shortcut under Other");
    const auto middle = index.search(QStringLiteral("Reset Zoom"));
    require(!middle.isEmpty() && middle.constFirst().location.itemId ==
                                     QStringLiteral("pin-to-screen.middle-mouse-button-action"),
            "search must find the pinned middle-click selector by its Reset Zoom option");
    const auto translation = index.search(QStringLiteral("original image translation"));
    require(!translation.isEmpty() && translation.constFirst().location.itemId ==
                                          QStringLiteral("translation.original-image"),
            "search should navigate directly to the original image translation toggle");
    const auto recognition = index.search(QStringLiteral("Save recognition result as image"));
    require(!recognition.isEmpty() &&
                recognition.constFirst().location.itemId ==
                    QStringLiteral("text-recognition.save-recognition-result-as-image"),
            "search should navigate directly to the recognition image saving toggle");

    int pages = 0;
    int sections = 0;
    int items = 0;
    QSet<QString> ids;
    for (const auto& entry : index.entries()) {
        require(!entry.id.isEmpty() && insertUnique(&ids, entry.id),
                "generated search ids must be stable and unique");
        require(!entry.title.contains(QStringLiteral("%1")) &&
                    !entry.description.contains(QStringLiteral("%1")) &&
                    !entry.path.contains(QStringLiteral("%1")),
                "search display fields must never expose unresolved placeholders");
        switch (entry.kind) {
        case settings::SettingsSearchNodeKind::Page:
            ++pages;
            require(entry.path == QStringLiteral("Pages"),
                    "page search results must use the Pages path");
            break;
        case settings::SettingsSearchNodeKind::Section:
            ++sections;
            break;
        case settings::SettingsSearchNodeKind::Item:
            ++items;
            break;
        }
    }
    require(pages == 12 && sections == 36 && items == 146,
            "search node counts must match catalog page, section, and item counts");

    const auto captureCursor = index.search(QStringLiteral("Capture cursor"));
    require(!captureCursor.isEmpty() && captureCursor.constFirst().location.itemId ==
                                            QStringLiteral("screenshot.capture-cursor"),
            "search must find the cursor capture setting");

    const auto theme = index.search(QStringLiteral("theme"));
    require(!theme.isEmpty() && theme.constFirst().id == QStringLiteral("item:interface.theme"),
            "exact item titles must rank ahead of descriptions and paths");
    require(index.search(QStringLiteral("preferences")).isEmpty(),
            "removed global hotkeys must not remain in search");
    const auto option = index.search(QStringLiteral("dark"));
    require(!option.isEmpty() &&
                option.constFirst().location.itemId == QStringLiteral("interface.theme"),
            "select option labels must be indexed");
    const auto windowElementApi = index.search(QStringLiteral("UIA"));
    require(!windowElementApi.isEmpty() && windowElementApi.constFirst().location.itemId ==
                                               QStringLiteral("screenshot.window-element-api"),
            "window element API options must find the system Screenshot setting");
    const auto ocrModel = index.search(QStringLiteral("Ultra Small V6 OCR model"));
    require(!ocrModel.isEmpty() && ocrModel.constFirst().location.itemId ==
                                       QStringLiteral("text-recognition.model-type"),
            "OCR model labels and aliases must find the Model Type setting");
    const auto multipleTokens = index.search(QStringLiteral("storage error"));
    require(!multipleTokens.isEmpty() &&
                multipleTokens.constFirst().location.itemId == QStringLiteral("storage.status"),
            "every query token must match an indexed field");
    require(index.search(QStringLiteral("storage nonexistent-token")).isEmpty(),
            "a query must be rejected when any token does not match");
    const auto drawingToolbar = index.search(QStringLiteral("stack drawing tools"));
    require(!drawingToolbar.isEmpty() &&
                drawingToolbar.constFirst().location.itemId ==
                    QStringLiteral("interface.toolbar.drawing-toolbar-editor"),
            "drawing toolbar position and stack terminology must be indexed");
    const auto pinnedToolbar = index.search(QStringLiteral("custom pinned toolbar"));
    require(!pinnedToolbar.isEmpty() &&
                pinnedToolbar.constFirst().location.itemId ==
                    QStringLiteral("interface.pin-to-screen.pinned-toolbar-editor"),
            "pinned toolbar customization must be indexed");
    const auto screenshotToolbar = index.search(QStringLiteral("custom screenshot toolbar"));
    require(!screenshotToolbar.isEmpty() &&
                screenshotToolbar.constFirst().location.itemId ==
                    QStringLiteral("interface.screenshot.screenshot-toolbar-editor"),
            "screenshot toolbar customization terminology must be indexed");

    index.setRuntimeValues({7});
    const auto delayedScreenshot = index.search(QStringLiteral("delay 7s"));
    require(!delayedScreenshot.isEmpty() &&
                delayedScreenshot.constFirst().id ==
                    QStringLiteral("item:quick.screenshot-delay") &&
                delayedScreenshot.constFirst().title == QStringLiteral("Delay 7s to execute") &&
                !delayedScreenshot.constFirst().title.contains(QStringLiteral("%1")),
            "search must resolve runtime placeholders before indexing and displaying item titles");
}

void searchIndexRebuildsLocalizedFields() {
    settings::SettingsSearchIndex index(settings::builtInSettingsRegistry());
    CatalogTranslator translator;
    require(QCoreApplication::installTranslator(&translator), "test translator must install");
    index.rebuild();
    const auto localizedModel = index.search(QStringLiteral("Localized Small V4"));
    require(!localizedModel.isEmpty() && localizedModel.constFirst().location.itemId ==
                                             QStringLiteral("text-recognition.model-type"),
            "language changes must refresh versioned OCR model options in search");
    require(!index.search(QStringLiteral("localized theme")).isEmpty() &&
                index.search(QStringLiteral("localized theme")).constFirst().id ==
                    QStringLiteral("item:interface.theme"),
            "search rebuilds must replace localized titles");
    require(!index.search(QStringLiteral("night mode")).isEmpty() &&
                index.search(QStringLiteral("night mode")).constFirst().id ==
                    QStringLiteral("item:interface.theme"),
            "search rebuilds must replace localized select-option labels");
    require(!index.search(QStringLiteral("visual style")).isEmpty() &&
                index.search(QStringLiteral("visual style")).constFirst().id ==
                    QStringLiteral("item:interface.theme"),
            "search rebuilds must replace localized aliases");
    for (const QString& query :
         {QStringLiteral("Localized Middle Action"), QStringLiteral("Localized Zoom Reset")}) {
        const auto result = index.search(query);
        require(!result.isEmpty() && result.constFirst().location.itemId ==
                                         QStringLiteral("pin-to-screen.middle-mouse-button-action"),
                "language changes must refresh middle-click titles and options in search");
    }
    QCoreApplication::removeTranslator(&translator);
    index.rebuild();
    require(index.search(QStringLiteral("localized theme")).isEmpty(),
            "removing a translator must remove stale normalized search fields");
}

void addingCatalogNodesAutomaticallyExpandsSearch() {
    const settings::SettingsCatalog& builtIn = settings::builtInSettingsRegistry().catalog();
    QVector<settings::SettingsPageDefinition> pages = builtIn.pages();
    settings::SettingsSelectDefinition select;
    select.options = {
        {QStringLiteral("system"), text("Follow system")},
        {QStringLiteral("light"), text("Light")},
        {QStringLiteral("dark"), text("Dark")},
    };
    pages.push_back({
        QStringLiteral("extra-page"),
        QStringLiteral("/extra"),
        text("Extra Page"),
        text("Extra page description"),
        {{QStringLiteral("extra-section"),
          text("Extra Section"),
          text("Extra section description"),
          settings::SettingsSectionReset::None,
          {{QStringLiteral("extra.item"),
            text("Extra Item"),
            text("Extra item description"),
            {},
            QStringLiteral("interface/theme_mode"),
            select}}}},
    });
    QVector<settings::SettingsNavigationNode> navigation = builtIn.navigation();
    navigation.push_back(settings::SettingsNavigationPageDefinition{
        QStringLiteral("nav.extra-page"),
        QStringLiteral("extra-page"),
        []() { return adqt::icons::antd::outlined::Appstore(); },
    });
    const settings::SettingsCatalog expanded(std::move(pages), std::move(navigation),
                                             builtIn.defaultLocation());
    require(expanded.validationErrors().isEmpty(),
            "a normal additional catalog page must validate without consumer changes");
    const auto registry =
        settings::SettingsRegistry::fromCatalog(expanded, QStringLiteral("search-substring"));
    settings::SettingsSearchIndex index(registry);
    settings::SettingsSearchIndex builtInIndex(settings::builtInSettingsRegistry());
    require(index.entries().size() == builtInIndex.entries().size() + 3,
            "adding one page, section, and item must automatically add three search entries");
    require(index.search(QStringLiteral("extra item")).constFirst().location ==
                settings::SettingsLocation{QStringLiteral("extra-page"),
                                           QStringLiteral("extra-section"),
                                           QStringLiteral("extra.item")},
            "new catalog items must be searchable without consumer changes");
}

void searchIndexPreservesInteriorSubstringMatches() {
    const settings::SettingsCatalog& builtIn = settings::builtInSettingsRegistry().catalog();
    QVector<settings::SettingsPageDefinition> pages = builtIn.pages();
    settings::SettingsTextDefinition textDefinition;
    textDefinition.binding = settings::SettingsTextBinding::ScreenshotManualFilenameFormat;
    pages.push_back({QStringLiteral("substring-page"),
                     QStringLiteral("/substring"),
                     text("Substring Page"),
                     text("Substring search fixture"),
                     {{QStringLiteral("substring-section"),
                       text("Substring"),
                       text("Substring"),
                       settings::SettingsSectionReset::None,
                       {{QStringLiteral("substring.item"),
                         text("Restore format"),
                         text("A value containing the search token in the middle"),
                         {},
                         QStringLiteral("screenshot/manual_save_filename_format"),
                         textDefinition}}}}});
    QVector<settings::SettingsNavigationNode> navigation = builtIn.navigation();
    navigation.push_back(settings::SettingsNavigationPageDefinition{
        QStringLiteral("nav.substring-page"), QStringLiteral("substring-page"),
        []() { return adqt::icons::antd::outlined::Search(); }});
    const settings::SettingsCatalog expanded(std::move(pages), std::move(navigation),
                                             builtIn.defaultLocation());
    require(expanded.validationErrors().isEmpty(),
            "substring search fixture must satisfy catalog validation");
    const auto registry =
        settings::SettingsRegistry::fromCatalog(expanded, QStringLiteral("search-extra"));
    settings::SettingsSearchIndex index(registry);
    const auto result = index.search(QStringLiteral("store"));
    require(std::any_of(result.cbegin(), result.cend(),
                        [](const auto& entry) {
                            return entry.location.itemId == QStringLiteral("substring.item");
                        }),
            "indexed search must retain interior substring matches that are not prefixes");
    const auto punctuationResult = index.search(QStringLiteral("idd"));
    const bool descriptionMatch =
        std::any_of(punctuationResult.cbegin(), punctuationResult.cend(), [](const auto& entry) {
            return entry.location.itemId == QStringLiteral("substring.item");
        });
    require(descriptionMatch,
            "indexed search must retain matches found in descriptions alongside other fields");
}

void sectionIdsMayRepeatAcrossPages() {
    const settings::SettingsCatalog& builtIn = settings::builtInSettingsRegistry().catalog();
    QVector<settings::SettingsPageDefinition> pages = builtIn.pages();
    pages[2].sections[0].id = pages[0].sections[0].id;
    const settings::SettingsCatalog expanded(std::move(pages), builtIn.navigation(),
                                             builtIn.defaultLocation());
    require(
        !expanded.validationErrors().join(u'\n').contains(QStringLiteral("duplicate section id")),
        "section IDs must be scoped to their containing page");
}

void catalogRejectsReservedIndexDelimiters() {
    const settings::SettingsCatalog& builtIn = settings::builtInSettingsRegistry().catalog();
    QVector<settings::SettingsPageDefinition> pages = builtIn.pages();
    pages[0].id = QStringLiteral("invalid\x1fpage");
    const settings::SettingsCatalog invalid(std::move(pages), builtIn.navigation(),
                                            builtIn.defaultLocation());
    require(invalid.validationErrors().join(u'\n').contains(
                QStringLiteral("reserved settings index delimiter")),
            "catalog validation must reject IDs that corrupt indexed hierarchy keys");
}

bool descriptorBelongsTo(const settings::SettingsRegistry& registry,
                         const settings::SettingsFieldDescriptor& descriptor) {
    const auto* item =
        registry.catalog().item({descriptor.pageId, descriptor.sectionId, descriptor.id});
    return item != nullptr && descriptor.definition == item;
}

void registryCompilesOwnedIndexesAndProviderPlans() {
    const settings::SettingsRegistry registry = settings::builtInSettingsRegistry();
    require(registry.isValid(), "the built-in settings registry must validate");
    require(registry.providerIds() == QStringList{QStringLiteral("built-in")},
            "the built-in registry must retain its provider ownership");
    require(registry.providerIdForPage(QStringLiteral("global-hotkeys")) ==
                QStringLiteral("built-in"),
            "page plans must expose their contributing provider");
    require(registry.providerIdForPage(QStringLiteral("missing-page")).isEmpty(),
            "unknown page plans must not report a provider");
    qsizetype catalogFieldCount = 0;
    for (const settings::SettingsPageDefinition& page : registry.catalog().pages()) {
        for (const settings::SettingsSectionDefinition& section : page.sections) {
            catalogFieldCount += section.items.size();
        }
    }
    require(registry.fields().size() == catalogFieldCount,
            "the registry must compile one descriptor for every catalog item");

    for (const settings::SettingsFieldDescriptor& descriptor : registry.fields()) {
        require(descriptorBelongsTo(registry, descriptor),
                "field descriptors must point into their owning catalog");
        if (!descriptor.configurationKey.isEmpty()) {
            require(descriptor.defaultValue ==
                        storage::ConfigurationSchema::defaultValue(descriptor.configurationKey),
                    "descriptor defaults must come from ConfigurationSchema");
            require(registry.fieldForConfigurationKey(descriptor.configurationKey) != nullptr,
                    "every persistence key must be indexed");
        }
        const auto* plan = registry.pagePlan(descriptor.pageId);
        require(plan != nullptr && plan->providerId == descriptor.providerId &&
                    plan->fieldIndexes.contains(&descriptor - registry.fields().constData()),
                "page plans must own each descriptor index");

        switch (descriptor.kind) {
        case settings::SettingsFieldKind::Select:
            require(registry.fieldForSelect(
                        std::get<settings::SettingsSelectDefinition>(descriptor.definition->payload)
                            .binding) == &descriptor,
                    "select bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::Switch:
            require(registry.fieldForSwitch(
                        std::get<settings::SettingsSwitchDefinition>(descriptor.definition->payload)
                            .binding) == &descriptor,
                    "switch bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::Integer:
            require(registry.fieldForInteger(std::get<settings::SettingsIntegerDefinition>(
                                                 descriptor.definition->payload)
                                                 .binding) == &descriptor,
                    "integer bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::MultiSelect:
            require(registry.fieldForMultiSelect(std::get<settings::SettingsMultiSelectDefinition>(
                                                     descriptor.definition->payload)
                                                     .binding) == &descriptor,
                    "multi-select bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::Slider:
            require(registry.fieldForSlider(
                        std::get<settings::SettingsSliderDefinition>(descriptor.definition->payload)
                            .binding) == &descriptor,
                    "slider bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::Color:
            require(registry.fieldForColor(
                        std::get<settings::SettingsColorDefinition>(descriptor.definition->payload)
                            .binding) == &descriptor,
                    "color bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::Radio:
            require(registry.fieldForRadio(
                        std::get<settings::SettingsRadioDefinition>(descriptor.definition->payload)
                            .binding) == &descriptor,
                    "radio bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::FilePath:
            require(registry.fieldForFilePath(std::get<settings::SettingsFilePathDefinition>(
                                                  descriptor.definition->payload)
                                                  .binding) == &descriptor,
                    "file path bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::DirectoryPath:
            require(
                registry.fieldForDirectoryPath(std::get<settings::SettingsDirectoryPathDefinition>(
                                                   descriptor.definition->payload)
                                                   .binding) == &descriptor,
                "directory path bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::Text:
            require(registry.fieldForText(
                        std::get<settings::SettingsTextDefinition>(descriptor.definition->payload)
                            .binding) == &descriptor,
                    "text bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::ShortcutAction: {
            const auto& payload = std::get<settings::SettingsShortcutActionDefinition>(
                descriptor.definition->payload);
            require(registry.fieldForShortcut(payload.shortcutAction) == &descriptor,
                    "global shortcut bindings must resolve through the registry index");
            break;
        }
        case settings::SettingsFieldKind::LocalShortcut: {
            const auto& payload =
                std::get<settings::SettingsLocalShortcutDefinition>(descriptor.definition->payload);
            require(registry.fieldForLocalShortcut(payload.scope, payload.shortcutId) ==
                        &descriptor,
                    "local shortcut bindings must resolve through the registry index");
            break;
        }
        case settings::SettingsFieldKind::GlobalMouseAction: {
            const auto& payload = std::get<settings::SettingsGlobalMouseActionDefinition>(
                descriptor.definition->payload);
            require(registry.fieldForGlobalMouseAction(payload.action) == &descriptor,
                    "global mouse bindings must resolve through the registry index");
            break;
        }
        case settings::SettingsFieldKind::Action:
            require(registry.fieldForAction(
                        std::get<settings::SettingsActionDefinition>(descriptor.definition->payload)
                            .binding) == &descriptor,
                    "action bindings must resolve through the registry index");
            break;
        case settings::SettingsFieldKind::Custom:
            require(registry.fieldForCustom(
                        std::get<settings::SettingsCustomDefinition>(descriptor.definition->payload)
                            .renderer) == &descriptor,
                    "custom renderers must resolve through the registry index");
            break;
        }
    }

    for (const settings::SettingsPersistenceSpec& spec : registry.persistence()) {
        const auto* descriptor = registry.field(spec.fieldId);
        require(descriptor != nullptr && descriptor->configurationKey == spec.key &&
                    descriptor->defaultValue == spec.defaultValue,
                "persistence specs must retain their descriptor and schema default");
    }
    for (const settings::SettingsSectionReset reset : {
             settings::SettingsSectionReset::ScreenshotShortcuts,
             settings::SettingsSectionReset::GlobalMouse,
             settings::SettingsSectionReset::GeneralSettings,
             settings::SettingsSectionReset::HistoryPolicy,
             settings::SettingsSectionReset::Tray,
         }) {
        for (const int index : registry.fieldsForReset(reset)) {
            require(index >= 0 && index < registry.fields().size() &&
                        registry.fields().at(index).reset == reset,
                    "reset indexes must contain only fields from their reset group");
        }
    }

    for (const settings::SettingsPagePlan& plan : registry.pagePlans()) {
        const auto* page = registry.catalog().page(plan.id);
        require(page != nullptr && plan.pageIndex >= 0 &&
                    plan.pageIndex < registry.catalog().pages().size() &&
                    &registry.catalog().pages().at(plan.pageIndex) == page &&
                    plan.sectionPlans.size() == page->sections.size(),
                "page plans must retain one ordered section plan per catalog section");
        for (int sectionPlanIndex = 0; sectionPlanIndex < plan.sectionPlans.size();
             ++sectionPlanIndex) {
            const settings::SettingsSectionPlan& sectionPlan =
                plan.sectionPlans.at(sectionPlanIndex);
            require(sectionPlan.sectionIndex >= 0 &&
                        sectionPlan.sectionIndex < page->sections.size(),
                    "section plans must contain valid catalog section indexes");
            const auto& section = page->sections.at(sectionPlan.sectionIndex);
            require(sectionPlan.sectionIndex == sectionPlanIndex && sectionPlan.id == section.id &&
                        sectionPlan.reset == section.reset &&
                        sectionPlan.itemLayout == section.itemLayout &&
                        sectionPlan.fieldIndexes.size() == section.items.size(),
                    "section plans must preserve catalog order and section metadata");
            for (int itemIndex = 0; itemIndex < sectionPlan.fieldIndexes.size(); ++itemIndex) {
                const int fieldIndex = sectionPlan.fieldIndexes.at(itemIndex);
                require(fieldIndex >= 0 && fieldIndex < registry.fields().size() &&
                            registry.fields().at(fieldIndex).pageId == plan.id &&
                            registry.fields().at(fieldIndex).sectionId == section.id &&
                            registry.fields().at(fieldIndex).definition ==
                                &section.items.at(itemIndex),
                        "section plans must point to their owning compiled descriptors");
            }
        }
    }
}

void registryCopiesAndMovesRebaseDescriptorPointers() {
    const settings::SettingsRegistry original = settings::builtInSettingsRegistry();
    settings::SettingsRegistry copied = original;
    settings::SettingsRegistry moved = std::move(copied);
    settings::SettingsRegistry assigned;
    assigned = original;
    settings::SettingsRegistry moveAssigned;
    moveAssigned = std::move(assigned);

    const QVector<const settings::SettingsRegistry*> registries{&original, &moved, &moveAssigned};
    for (const settings::SettingsRegistry* registry : registries) {
        require(registry->isValid() && registry->fields().size() == original.fields().size(),
                "copied and moved registries must preserve compiled contents");
        for (const settings::SettingsFieldDescriptor& descriptor : registry->fields()) {
            require(descriptorBelongsTo(*registry, descriptor),
                    "copy and move operations must rebase descriptor pointers");
        }
    }
}

void registryReportsDuplicatesDeterministically() {
    const settings::SettingsCatalog& builtIn = settings::builtInSettingsRegistry().catalog();
    QVector<settings::SettingsPageDefinition> pages = builtIn.pages();
    auto duplicateItem = pages[0].sections[0].items.at(0);
    duplicateItem.id = QStringLiteral("duplicate-theme");
    pages[0].sections[0].items.push_back(duplicateItem);
    const settings::SettingsCatalog duplicateCatalog(std::move(pages), builtIn.navigation(),
                                                     builtIn.defaultLocation());
    const settings::SettingsRegistry duplicateRegistry =
        settings::SettingsRegistry::fromCatalog(duplicateCatalog, QStringLiteral("test"));
    const QString errors = duplicateRegistry.validationErrors().join(u'\n');
    require(errors.contains(QStringLiteral("duplicate settings persistence key")) &&
                errors.contains(QStringLiteral("duplicate shortcut action")) &&
                duplicateRegistry.fieldForShortcut(presentation::GlobalShortcutAction::Screenshot)
                        ->id == QStringLiteral("quick.screenshot"),
            "duplicate indexes must report diagnostics while preserving first-entry lookup");

    settings::SettingsRegistryBuilder builder;
    builder.addCatalog(builtIn, QStringLiteral("duplicate-provider"));
    builder.addCatalog(builtIn, QStringLiteral("duplicate-provider"));
    const settings::SettingsRegistry duplicateProviders = builder.build();
    require(builder.validationErrors().join(u'\n').contains(
                QStringLiteral("duplicate settings provider id")) &&
                duplicateProviders.validationErrors().join(u'\n').contains(
                    QStringLiteral("duplicate settings page plan id")),
            "duplicate providers and their page plans must be diagnosed");
}

void emptyRegistryBuilderIsExplicitlyInvalid() {
    const settings::SettingsRegistryBuilder builder;
    const QString expected = QStringLiteral("settings registry requires at least one provider");
    require(builder.validationErrors() == QStringList{expected},
            "an empty registry builder must expose one deterministic validation error");
    const settings::SettingsRegistry registry = builder.build();
    require(!registry.isValid() && registry.validationErrors().contains(expected) &&
                registry.pages().isEmpty() && registry.fields().isEmpty(),
            "building without providers must produce an invalid, empty registry");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    builtInCatalogIsCompleteAndValid();
    globalMouseSettingsHaveStableContracts();
    globalHotkeyShortcutsHaveStableContracts();
    compactTrayManifestMatchesRegistryCatalog();
    structuredFallbackIsDeterministic();
    invalidCatalogReportsAllConformanceErrors();
    searchIndexIsGeneratedAndRanked();
    searchIndexRebuildsLocalizedFields();
    searchIndexPreservesInteriorSubstringMatches();
    catalogRejectsReservedIndexDelimiters();
    sectionIdsMayRepeatAcrossPages();
    addingCatalogNodesAutomaticallyExpandsSearch();
    registryCompilesOwnedIndexesAndProviderPlans();
    registryCopiesAndMovesRebaseDescriptorPointers();
    registryReportsDuplicatesDeterministically();
    emptyRegistryBuilderIsExplicitlyInvalid();
    return 0;
}
