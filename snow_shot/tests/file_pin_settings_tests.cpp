#include "snow_shot/presentation/screenshotpinsourcetracker.h"
#include <QClipboard>
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"
#include <QApplication>
#include <QPalette>
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include <QHash>
#include <QTemporaryDir>
#include <algorithm>
#include <iterator>
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
void pinnedTextStyleSettings() {
    const storage::PinToScreenSettings stored;
    GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
    settings::BuiltInSettingsBackend backend(manager);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    const auto* page =
        settings::builtInSettingsRegistry().catalog().page(QStringLiteral("pinned-windows"));
    require(page && page->sections.size() == 4 &&
                page->sections.at(2).id == QStringLiteral("pin-to-screen-text-style") &&
                page->sections.at(2).items.size() == 5 &&
                page->sections.at(2).reset == settings::SettingsSectionReset::PinTextStyle,
            "Text Style is a separate five-control section before Action Toolbar");
    require(stored.textMaximumWidth() == 1024 && stored.textFontSize() == 16 &&
                stored.textFontFamily().isEmpty() && !stored.textColor().isValid() &&
                !stored.textBackgroundColor().isValid(),
            "initial text style uses legacy dimensions and theme colors");
    require(backend.colorValue(settings::SettingsColorBinding::PinTextColor) ==
                    QApplication::palette().color(QPalette::Text) &&
                backend.colorValue(settings::SettingsColorBinding::PinTextBackgroundColor) ==
                    QApplication::palette().color(QPalette::Base),
            "system color swatches resolve the current palette");
    for (const int width : {100, 2000})
        require(session.applyIntegerValue(settings::SettingsIntegerBinding::PinTextMaximumWidth,
                                          width) &&
                    stored.textMaximumWidth() == width,
                "both maximum-width boundaries are accepted");
    for (const int width : {99, 2001})
        require(!backend.applyIntegerValue(settings::SettingsIntegerBinding::PinTextMaximumWidth,
                                           width),
                "out-of-range widths are rejected");
    for (const int size : {8, 96})
        require(
            session.applyIntegerValue(settings::SettingsIntegerBinding::PinTextFontSize, size) &&
                stored.textFontSize() == size,
            "both font-size boundaries are accepted");
    for (const int size : {7, 97})
        require(!backend.applyIntegerValue(settings::SettingsIntegerBinding::PinTextFontSize, size),
                "out-of-range font sizes are rejected");
    const QString missingFont = QStringLiteral("Snow Shot Missing Test Font");
    require(session.applySelectValue(settings::SettingsSelectBinding::PinTextFont, missingFont),
            "an unavailable font preference remains portable");
    session.requestFontOptions();
    bool foundMissing = false;
    for (const auto& option :
         session.dynamicSelectOptions(settings::SettingsSelectBinding::PinTextFont))
        foundMissing |= option.value == missingFont;
    require(foundMissing && stored.textFontFamily() == missingFont,
            "font options preserve an unavailable saved family");
    require(session.applySelectValue(settings::SettingsSelectBinding::PinTextFont, QString()),
            "System default can be selected again");
    const QColor foreground(170, 20, 80, 128), background(10, 30, 60, 64);
    require(session.applyColorValue(settings::SettingsColorBinding::PinTextColor, foreground) &&
                session.applyColorValue(settings::SettingsColorBinding::PinTextBackgroundColor,
                                        background),
            "both RGBA defaults can be customized");
    const auto captured = ScreenshotClipboardContentReader::capturePinnedTextStyle();
    require(captured.isValid() && captured.maximumWidth == 2000 && captured.fontSize == 96 &&
                captured.textColor == foreground && captured.backgroundColor == background,
            "GUI style capture resolves all configured defaults");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.configuration().flushNow().success, "text preferences flush");
    storage::ConfigurationStore reloaded(applicationStorage.configurationDirectory() +
                                             QStringLiteral("/config.json"),
                                         true, true, 60000);
    require(reloaded.value(QStringLiteral("pin_to_screen/text_maximum_width")).toInt() == 2000 &&
                reloaded.value(QStringLiteral("pin_to_screen/text_color")).toString() ==
                    storage::colorToRgbaString(foreground),
            "text style preferences survive reload");
    require(stored.setShowWindowButtons(false) &&
                backend.resetSection(settings::SettingsSectionReset::PinTextStyle) &&
                !stored.showWindowButtons() && stored.textMaximumWidth() == 1024 &&
                stored.textFontSize() == 16 && stored.textFontFamily().isEmpty() &&
                !stored.textColor().isValid() && !stored.textBackgroundColor().isValid(),
            "Text Style reset restores only its own fields");
    require(captured.textColor == foreground && captured.fontSize == 96 &&
                stored.setShowWindowButtons(true),
            "captured styles remain unchanged after reset");
    require(!storage::ConfigurationSchema::normalize(QStringLiteral("pin_to_screen/text_color"),
                                                     QStringLiteral("invalid"))
                    .valid &&
                storage::ConfigurationSchema::normalize(QStringLiteral("pin_to_screen/text_color"),
                                                        QStringLiteral("system"))
                    .valid,
            "system colors are accepted and malformed colors rejected");
}

void windowButtonSettings() {
    const storage::PinToScreenSettings stored;
    const auto binding = settings::SettingsSwitchBinding::PinShowWindowButtons;
    GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
    settings::BuiltInSettingsBackend backend(manager);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    const auto* field = settings::builtInSettingsRegistry().fieldForSwitch(binding);
    require(field && field->pageId == QStringLiteral("pinned-windows") &&
                field->sectionId == QStringLiteral("pin-to-screen-settings") &&
                field->id == QStringLiteral("pin-to-screen.show-window-buttons") &&
                field->configurationKey == QStringLiteral("pin_to_screen/show_window_buttons") &&
                field->reset == settings::SettingsSectionReset::PinToScreenBehavior,
            "window buttons switch belongs to the Pin to screen preferences");
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

void exitAnnotationModeBeforeClosingSettings() {
    const storage::PinToScreenSettings stored;
    constexpr auto binding = settings::SettingsSwitchBinding::PinExitAnnotationModeBeforeClosing;
    GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
    settings::BuiltInSettingsBackend backend(manager);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    const auto* field = settings::builtInSettingsRegistry().fieldForSwitch(binding);
    require(field && field->pageId == QStringLiteral("pinned-windows") &&
                field->sectionId == QStringLiteral("pin-to-screen-settings") &&
                field->id == QStringLiteral("pin-to-screen.exit-annotation-mode-before-closing") &&
                field->configurationKey ==
                    QStringLiteral("pin_to_screen/exit_annotation_mode_before_closing") &&
                field->definition->title.translated() ==
                    QStringLiteral("Exit annotation mode before closing") &&
                field->reset == settings::SettingsSectionReset::PinToScreenBehavior &&
                session.state(field->id).visible && session.state(field->id).enabled,
            "exit annotation mode switch belongs to available pin behavior settings");
    const auto* section =
        settings::builtInSettingsRegistry().catalog().section(field->pageId, field->sectionId);
    require(section, "pin behavior section exists");
    int index = -1;
    for (int i = 0; i < section->items.size(); ++i) {
        if (section->items[i].id == field->id)
            index = i;
    }
    require(index >= 0 && index + 1 < section->items.size() &&
                section->items[index + 1].id ==
                    QStringLiteral("pin-to-screen.confirm-before-closing-window"),
            "exit annotation mode setting immediately precedes close confirmation");
    require(!stored.exitAnnotationModeBeforeClosing() && !session.switchValue(binding),
            "exit annotation mode before closing defaults off");
    require(
        !storage::ConfigurationSchema::normalize(field->configurationKey, QStringLiteral("enabled"))
             .valid,
        "exit annotation mode preference must reject nonboolean values");
    require(session.applySwitchValue(binding, true) && stored.exitAnnotationModeBeforeClosing() &&
                backend.switchValue(binding),
            "settings session enables exiting annotation mode before closing");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.configuration().flushNow().success,
            "exit annotation mode preference must be flushable");
    storage::ConfigurationStore reloaded(applicationStorage.configurationDirectory() +
                                             QStringLiteral("/config.json"),
                                         true, true, 60000);
    require(reloaded.value(field->configurationKey).toBool(),
            "exit annotation mode preference survives configuration reload");
    require(session.reset(settings::SettingsSectionReset::PinToScreen) &&
                stored.exitAnnotationModeBeforeClosing(),
            "appearance reset preserves exit annotation mode preference");
    require(session.applySwitchValue(binding, false) && !stored.exitAnnotationModeBeforeClosing() &&
                session.applySwitchValue(binding, true) &&
                session.reset(settings::SettingsSectionReset::PinToScreenBehavior) &&
                !stored.exitAnnotationModeBeforeClosing() && !session.switchValue(binding),
            "pin behavior reset restores exit annotation mode default");
}

void windowConfirmationSettings() {
    const storage::PinToScreenSettings stored;
    constexpr auto closeBinding = settings::SettingsSwitchBinding::PinConfirmBeforeClosingWindow;
    constexpr auto destroyBinding =
        settings::SettingsSwitchBinding::PinConfirmBeforeDestroyingWindow;
    GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
    settings::BuiltInSettingsBackend backend(manager);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    const auto* closeField = settings::builtInSettingsRegistry().fieldForSwitch(closeBinding);
    const auto* destroyField = settings::builtInSettingsRegistry().fieldForSwitch(destroyBinding);
    require(closeField && destroyField, "both window confirmation switches must be registered");
    for (const auto* field : {closeField, destroyField}) {
        require(field->pageId == QStringLiteral("pinned-windows") &&
                    field->sectionId == QStringLiteral("pin-to-screen-settings") &&
                    field->reset == settings::SettingsSectionReset::PinToScreenBehavior &&
                    session.state(field->id).visible && session.state(field->id).enabled,
                "window confirmation switches belong to available pin behavior settings");
        require(!storage::ConfigurationSchema::normalize(field->configurationKey,
                                                         QStringLiteral("enabled"))
                     .valid,
                "window confirmation preferences must reject nonboolean values");
    }
    require(closeField->id == QStringLiteral("pin-to-screen.confirm-before-closing-window") &&
                closeField->configurationKey ==
                    QStringLiteral("pin_to_screen/confirm_before_closing_window") &&
                closeField->definition->title.translated() ==
                    QStringLiteral("Confirm before closing window") &&
                destroyField->id ==
                    QStringLiteral("pin-to-screen.confirm-before-destroying-window") &&
                destroyField->configurationKey ==
                    QStringLiteral("pin_to_screen/confirm_before_destroying_window") &&
                destroyField->definition->title.translated() ==
                    QStringLiteral("Confirm before destroying window"),
            "both confirmation settings must have the expected labels and configuration keys");
    require(!stored.confirmBeforeClosingWindow() && stored.confirmBeforeDestroyingWindow() &&
                !session.switchValue(closeBinding) && session.switchValue(destroyBinding),
            "close confirmation defaults off and destroy confirmation defaults on");
    require(session.applySwitchValue(closeBinding, true) &&
                session.applySwitchValue(destroyBinding, false) &&
                stored.confirmBeforeClosingWindow() && !stored.confirmBeforeDestroyingWindow(),
            "confirmation switches must apply independently through the settings session");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.configuration().flushNow().success,
            "window confirmation preferences must be flushable");
    storage::ConfigurationStore reloaded(applicationStorage.configurationDirectory() +
                                             QStringLiteral("/config.json"),
                                         true, true, 60000);
    require(reloaded.value(closeField->configurationKey).toBool() &&
                !reloaded.value(destroyField->configurationKey).toBool(true),
            "both window confirmation preferences must survive configuration reload");
    require(session.reset(settings::SettingsSectionReset::PinToScreen) &&
                stored.confirmBeforeClosingWindow() && !stored.confirmBeforeDestroyingWindow(),
            "appearance reset must preserve confirmation preferences");
    require(session.reset(settings::SettingsSectionReset::PinToScreenBehavior) &&
                !stored.confirmBeforeClosingWindow() && stored.confirmBeforeDestroyingWindow() &&
                !session.switchValue(closeBinding) && session.switchValue(destroyBinding),
            "pin behavior reset must restore both confirmation defaults");
}

void textSelectionSettings() {
    const storage::PinToScreenSettings stored;
    const auto binding = settings::SettingsSelectBinding::PinTextSelectionOnRecognitionResults;
    GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
    settings::BuiltInSettingsBackend backend(manager);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    require(stored.textSelectionOnRecognitionResults() == QStringLiteral("always"),
            "recognition result text selection defaults to always");
    const auto quickBinding = settings::SettingsSwitchBinding::PinQuickAnnotationForSelectedText;
    require(stored.quickAnnotationForSelectedText() && session.switchValue(quickBinding) &&
                session.switchEnabled(quickBinding),
            "quick annotation defaults on and is available with Always selection");
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
    const auto* quickField = settings::builtInSettingsRegistry().fieldForSwitch(quickBinding);
    require(quickField &&
                quickField->reset == settings::SettingsSectionReset::PinToScreenBehavior &&
                index + 1 < section->items.size() && section->items[index + 1].id == quickField->id,
            "quick annotation switch follows the text selection policy");
    require(session.applySwitchValue(quickBinding, false) &&
                !stored.quickAnnotationForSelectedText(),
            "quick annotation switch persists its own preference");
    require(session.applySwitchValue(quickBinding, true), "restore quick annotation preference");
    require(session.applySelectValue(binding, QStringLiteral("only_when_displayed")) &&
                stored.textSelectionOnRecognitionResults() == QStringLiteral("only_when_displayed"),
            "settings session writes selection policy through backend");
    require(!session.switchEnabled(quickBinding) && stored.quickAnnotationForSelectedText() &&
                !backend.applySwitchValue(quickBinding, false),
            "Only when displayed disables the switch without forgetting its preference");
    require(!stored.setTextSelectionOnRecognitionResults(QStringLiteral("invalid")) &&
                stored.textSelectionOnRecognitionResults() == QStringLiteral("only_when_displayed"),
            "schema rejects unsupported selection modes");
    require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                stored.textSelectionOnRecognitionResults() == QStringLiteral("always") &&
                stored.quickAnnotationForSelectedText() && session.switchEnabled(quickBinding),
            "pin behavior reset restores selection and quick annotation defaults");
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

void mouseWheelStepSettings() {
    const storage::PinToScreenSettings stored;
    GlobalShortcutManager manager(std::make_unique<Backend>(), nullptr, [] { return false; });
    settings::BuiltInSettingsBackend backend(manager);
    const auto& registry = settings::builtInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    const auto* page = registry.catalog().page(QStringLiteral("pinned-windows"));
    require(page != nullptr, "pinned settings page exists");
    const auto& items = page->sections.at(0).items;
    const auto duplicate = std::find_if(items.cbegin(), items.cend(), [](const auto& item) {
        return item.id == QStringLiteral("pin-to-screen.duplicate-content-action");
    });
    require(duplicate != items.cend() && std::distance(duplicate, items.cend()) >= 3 &&
                (duplicate + 1)->id == QStringLiteral("pin-to-screen.mouse-wheel-opacity-step") &&
                (duplicate + 2)->id == QStringLiteral("pin-to-screen.mouse-wheel-zoom-step"),
            "opacity and zoom step sliders immediately follow the duplicate content setting");
    require(stored.mouseWheelOpacityStep() == 5 && stored.mouseWheelZoomStep() == 10,
            "wheel defaults preserve five-point opacity and ten-point zoom steps");
    for (const auto binding : {settings::SettingsSliderBinding::PinMouseWheelOpacityStep,
                               settings::SettingsSliderBinding::PinMouseWheelZoomStep}) {
        const bool opacity = binding == settings::SettingsSliderBinding::PinMouseWheelOpacityStep;
        const auto* field = registry.fieldForSlider(binding);
        const QString key = opacity ? QStringLiteral("pin_to_screen/mouse_wheel_opacity_step")
                                    : QStringLiteral("pin_to_screen/mouse_wheel_zoom_step");
        const QString title = opacity ? QStringLiteral("Mouse wheel opacity step")
                                      : QStringLiteral("Mouse wheel zoom step");
        const auto* schema = storage::ConfigurationSchema::entry(key);
        require(field && field->pageId == QStringLiteral("pinned-windows") &&
                    field->sectionId == QStringLiteral("pin-to-screen-settings") &&
                    field->reset == settings::SettingsSectionReset::PinToScreenBehavior &&
                    field->configurationKey == key &&
                    field->definition->title.translated() == title &&
                    session.state(field->id).visible && session.state(field->id).enabled,
                "wheel sliders are available in Pin to screen interaction settings");
        const auto& slider =
            std::get<settings::SettingsSliderDefinition>(field->definition->payload);
        require(slider.suffix.translated() == QStringLiteral("%") && schema &&
                    schema->integerRange && schema->integerRange->minimum == 1 &&
                    schema->integerRange->maximum == 20 && schema->integerRange->step == 1,
                "wheel sliders expose an integer range of one to twenty percent");
        for (const int value : {1, 7, 20}) {
            require(session.applySliderValue(binding, value) &&
                        backend.sliderValue(binding) == value &&
                        (opacity ? stored.mouseWheelOpacityStep() : stored.mouseWheelZoomStep()) ==
                            value,
                    "wheel steps persist through the settings session and adapters");
        }
        for (const int value : {0, 21}) {
            require(
                !backend.applySliderValue(binding, value) && backend.sliderValue(binding) == 20,
                "wheel steps outside the allowed range are rejected without changing the value");
        }
        require(!storage::ConfigurationSchema::normalize(key, 1.5).valid &&
                    !storage::ConfigurationSchema::normalize(key, QStringLiteral("7")).valid,
                "wheel steps must be integer settings");
    }
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.configuration().flushNow().success, "wheel preferences flush");
    storage::ConfigurationStore reloaded(applicationStorage.configurationDirectory() +
                                             QStringLiteral("/config.json"),
                                         true, true, 60000);
    require(reloaded.value(QStringLiteral("pin_to_screen/mouse_wheel_opacity_step")).toInt() ==
                    20 &&
                reloaded.value(QStringLiteral("pin_to_screen/mouse_wheel_zoom_step")).toInt() == 20,
            "wheel preferences survive configuration reload");
    require(backend.resetSection(settings::SettingsSectionReset::PinToScreen) &&
                stored.mouseWheelOpacityStep() == 20 && stored.mouseWheelZoomStep() == 20,
            "window appearance reset preserves wheel steps");
    require(session.reset(settings::SettingsSectionReset::PinToScreenBehavior) &&
                stored.mouseWheelOpacityStep() == 5 && stored.mouseWheelZoomStep() == 10,
            "interaction reset restores both wheel step defaults");
}

void shortcutSettings() {
    const auto action = GlobalShortcutAction::PinSelectedFiles;
    const QString id = QStringLiteral("quick.pin-selected-files");
    const storage::ShortcutSettings stored;
    const auto* item = settings::builtInSettingsRegistry().catalog().item(
        {QStringLiteral("global-hotkeys"), QStringLiteral("pin-to-screen"), id});
    require(item != nullptr, "file pin action must remain in global shortcut settings");
#ifdef Q_OS_MACOS
    require(
        item->description.translated() ==
            QStringLiteral(
                "Pin selected image or document files from Finder or the desktop to the screen"),
        "macOS file pin description must identify Finder");
#else
    require(item->description.translated() ==
                QStringLiteral("Pin selected image or document files from File Explorer or the "
                               "desktop to the screen"),
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
        require(stored.hideShowAllWindows().isEmpty() &&
                    session.applyShortcuts(GlobalShortcutAction::HideShowAllWindows,
                                           {QStringLiteral("Ctrl+Alt+F9")}) &&
                    manager.state(GlobalShortcutAction::HideShowAllWindows).status ==
                        GlobalShortcutStatus::Registered,
                "hide/show all windows starts unassigned and accepts a shortcut");
        require(backend.resetSection(settings::SettingsSectionReset::GlobalPinToScreenShortcuts),
                "Pin to screen reset succeeds");
        require(stored.pinSelectedFiles().isEmpty() &&
                    manager.state(action).status == GlobalShortcutStatus::Unset,
                "Pin to screen reset must clear the new shortcut");
        require(manager.state(GlobalShortcutAction::SwitchWindowGroup).shortcuts.isEmpty(),
                "pin section reset clears group switch shortcut");
        require(manager.state(managementAction).shortcuts.isEmpty(),
                "Pin to screen reset must clear the management shortcut");
        require(stored.hideShowAllWindows().isEmpty() &&
                    manager.state(GlobalShortcutAction::HideShowAllWindows).status ==
                        GlobalShortcutStatus::Unset,
                "Pin to screen reset must clear the hide/show all windows shortcut");
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
    if (app.arguments().contains(QStringLiteral("--quick-text-annotation-only"))) {
        textSelectionSettings();
        require(settings::builtInSettingsRegistry().catalog().validationErrors().isEmpty(),
                "quick annotation settings preserve catalog validity");
        storage.shutdown();
        return 0;
    }
    pinnedTextStyleSettings();
    windowButtonSettings();
    exitAnnotationModeBeforeClosingSettings();
    windowConfirmationSettings();
    textSelectionSettings();
    duplicateContentSettings();
    mouseWheelStepSettings();
    clipboardSourceIdentity();
    shortcutSettings();
    storage.shutdown();
    return 0;
}
