#include "snow_shot/presentation/settings/settingscatalog.h"

#include "antd_icons.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/storage/configurationschema.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace snow_shot::presentation::settings {
namespace {
namespace outlined_icons = adqt::icons::antd::outlined;
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;
namespace custom_twotone_icons = snow_shot::presentation::icons::custom::twotone;

constexpr TranslatableText settingsText(const char* source) {
    return {"SettingsCatalog", source};
}

constexpr auto QUICK_PAGE_ID = "quick-functions";
constexpr auto HISTORY_PAGE_ID = "screenshot-history";
constexpr auto FUNCTION_PAGE_ID = "function-settings";
constexpr auto INTERFACE_PAGE_ID = "interface-settings";
constexpr auto STORAGE_PAGE_ID = "storage-and-privacy";
constexpr auto SYSTEM_PAGE_ID = "system-settings";
constexpr auto HOTKEY_PAGE_ID = "hotkey-settings";

SettingsItemDefinition screenshotItem() {
    SettingsShortcutActionDefinition payload;
    payload.shortcutAction = GlobalShortcutAction::Screenshot;
    payload.command = {SettingsCommandKind::CaptureScreenshot, {}};
    payload.iconFactory = []() { return custom_twotone_icons::ScreenshotFeature(); };
    return {
        QStringLiteral("quick.screenshot"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot")),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Take a screenshot")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Capture")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screen capture"))},
        QStringLiteral("global_shortcuts/screenshot"),
        payload,
    };
}

SettingsItemDefinition
quickActionItem(const QString& id, const char* title, const char* description,
                QVector<TranslatableText> aliases, GlobalShortcutAction shortcutAction,
                const QString& configurationKey, std::function<adqt::icons::IconRef()> iconFactory,
                SettingsShortcutAdjustment adjustment = SettingsShortcutAdjustment::None) {
    SettingsShortcutActionDefinition payload;
    payload.shortcutAction = shortcutAction;
    payload.command = {
        SettingsCommandKind::ExecuteQuickAction,
        {},
        shortcutAction,
    };
    payload.iconFactory = std::move(iconFactory);
    payload.adjustment = adjustment;
    return {
        id,
        settingsText(title),
        settingsText(description),
        std::move(aliases),
        configurationKey,
        std::move(payload),
    };
}

SettingsItemDefinition screenshotDelayItem() {
    return quickActionItem(
        QStringLiteral("quick.screenshot-delay"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Delay %1s to Execute"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Take a screenshot after the configured delay"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Delayed screenshot")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Delay capture"))},
        GlobalShortcutAction::ScreenshotDelay, QStringLiteral("global_shortcuts/screenshot_delay"),
        []() { return custom_outlined_icons::ScreenshotDelay(); },
        SettingsShortcutAdjustment::ScreenshotDelaySeconds);
}

SettingsItemDefinition screenshotFixedItem() {
    return quickActionItem(
        QStringLiteral("quick.screenshot-fixed"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Pin to Screen"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Pin the confirmed screenshot selection to the screen"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Fixed screenshot")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pin selection"))},
        GlobalShortcutAction::ScreenshotFixed, QStringLiteral("global_shortcuts/screenshot_fixed"),
        []() { return custom_outlined_icons::PinToScreen(); });
}

SettingsItemDefinition screenshotOcrItem() {
    return quickActionItem(
        QStringLiteral("quick.screenshot-ocr"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Text Recognition"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Recognize text in the confirmed screenshot selection"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "OCR")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Recognize text"))},
        GlobalShortcutAction::ScreenshotOcr, QStringLiteral("global_shortcuts/screenshot_ocr"),
        []() { return custom_outlined_icons::ToolRecognizeText(); });
}

SettingsItemDefinition screenshotTranslationItem() {
    return quickActionItem(
        QStringLiteral("quick.screenshot-translation"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Text Translation"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Translate text in the confirmed screenshot selection"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Translate text"))},
        GlobalShortcutAction::ScreenshotTranslation,
        QStringLiteral("global_shortcuts/screenshot_translation"),
        []() { return custom_outlined_icons::OcrTranslate(); });
}

SettingsItemDefinition screenshotCopyItem() {
    return quickActionItem(
        QStringLiteral("quick.screenshot-copy"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Copy to Clipboard"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Copy the confirmed screenshot selection to the clipboard"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Copy screenshot")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Clipboard"))},
        GlobalShortcutAction::ScreenshotCopy, QStringLiteral("global_shortcuts/screenshot_copy"),
        []() { return custom_outlined_icons::ScreenshotCopy(); });
}

SettingsItemDefinition screenshotFullScreenItem() {
    return quickActionItem(
        QStringLiteral("quick.screenshot-full-screen"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Current Monitor"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Capture every monitor and copy the monitor under the pointer"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Full screen")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Monitor capture"))},
        GlobalShortcutAction::ScreenshotFullScreen,
        QStringLiteral("global_shortcuts/screenshot_full_screen"),
        []() { return custom_outlined_icons::ScreenshotFullScreen(); });
}

SettingsItemDefinition screenshotFocusedWindowItem() {
    return quickActionItem(
        QStringLiteral("quick.screenshot-focused-window"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Focused Window"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Capture and copy the currently focused window"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Active window")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Window capture"))},
        GlobalShortcutAction::ScreenshotFocusedWindow,
        QStringLiteral("global_shortcuts/screenshot_focused_window"),
        []() { return custom_outlined_icons::ScreenshotFocusedWindow(); });
}

SettingsItemDefinition screenRecordItem() {
    return quickActionItem(
        QStringLiteral("quick.screen-record"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Screen Recording"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Start a screen recording from a confirmed selection"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Record screen")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screen recording"))},
        GlobalShortcutAction::ScreenRecord, QStringLiteral("global_shortcuts/screen_record"),
        []() { return custom_outlined_icons::RecordScreen(); });
}

SettingsItemDefinition screenRecordCopyItem() {
    return quickActionItem(
        QStringLiteral("quick.screen-record-copy"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Start Screen Recording / Stop and Copy Video"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Start a screen recording, or stop and copy the current video"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Copy video")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Recording toggle"))},
        GlobalShortcutAction::ScreenRecordCopy, QStringLiteral("global_shortcuts/screen_record_copy"),
        []() { return custom_outlined_icons::ScreenshotCopy(); },
        SettingsShortcutAdjustment::None);
}

SettingsItemDefinition openCaptureHistoryItem() {
    return quickActionItem(
        QStringLiteral("quick.open-capture-history"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot History"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Open the screenshot history page in the main window"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Capture history")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Saved screenshots"))},
        GlobalShortcutAction::OpenCaptureHistory,
        QStringLiteral("global_shortcuts/open_capture_history"),
        []() { return outlined_icons::History(); });
}

SettingsItemDefinition themeItem() {
    SettingsSelectDefinition payload;
    payload.options = {
        {QStringLiteral("system"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Follow System"))},
        {QStringLiteral("light"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Light"))},
        {QStringLiteral("dark"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Dark"))},
    };
    return {
        QStringLiteral("interface.theme"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Theme")),
        settingsText(QT_TRANSLATE_NOOP(
            "SettingsCatalog", "Match your system appearance or choose a light or dark theme")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Appearance")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Color mode"))},
        QStringLiteral("interface/theme_mode"),
        payload,
    };
}

SettingsItemDefinition languageItem() {
    SettingsSelectDefinition payload;
    payload.binding = SettingsSelectBinding::Language;
    payload.source = SettingsSelectSource::LanguageCatalog;
    payload.options = {
        {QStringLiteral("system"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Follow System"))},
    };
    return {
        QStringLiteral("interface.language"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Language")),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                       "Select the language used throughout the application")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Locale")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Translation"))},
        QStringLiteral("interface/language"),
        payload,
    };
}

SettingsItemDefinition screenshotToolbarSizeItem() {
    SettingsSelectDefinition payload;
    payload.binding = SettingsSelectBinding::ScreenshotToolbarSize;
    payload.options = {
        {QStringLiteral("small"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Small"))},
        {QStringLiteral("normal"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Normal"))},
    };
    return {QStringLiteral("interface.screenshot.toolbar-size"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Toolbar size")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                           "Choose the size of the screenshot, pinned, and "
                                           "recording toolbars")),
            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot toolbar")),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pinned toolbar")),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Recording toolbar"))},
            QStringLiteral("screenshot_ui/toolbar_size"),
            payload};
}

SettingsItemDefinition selectionTransitionAnimationItem() {
    return {QStringLiteral("interface.screenshot.selection-transition-animation"),
            settingsText(
                QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot selection transition animation")),
            settingsText(QT_TRANSLATE_NOOP(
                "SettingsCatalog", "Animate transitions between smart screenshot selections")),
            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Selection animation"))},
            QStringLiteral("screenshot_ui/selection_transition_animation"),
            SettingsSwitchDefinition{SettingsSwitchBinding::SelectionTransitionAnimation}};
}

SettingsItemDefinition colorPickerDisplayModeItem() {
    SettingsSelectDefinition payload;
    payload.binding = SettingsSelectBinding::ColorPickerDisplayMode;
    payload.options = {
        {QStringLiteral("hide_outside_selection"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Hide when outside selection"))},
        {QStringLiteral("always_show"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Always show"))},
        {QStringLiteral("always_hide"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Always hide"))},
    };
    return {QStringLiteral("interface.screenshot.color-picker-display-mode"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Color picker display mode")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                           "Control when the screenshot color picker is visible")),
            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Magnifier visibility"))},
            QStringLiteral("screenshot_ui/color_picker_display_mode"),
            payload};
}

SettingsItemDefinition screenshotColorItem(const QString& id, const char* title,
                                           const char* description, const QString& key,
                                           SettingsColorBinding binding,
                                           QVector<TranslatableText> aliases = {}) {
    return {id,
            settingsText(title),
            settingsText(description),
            std::move(aliases),
            key,
            SettingsColorDefinition{binding, true}};
}

SettingsItemDefinition shortcutHintOpacityItem() {
    return {QStringLiteral("interface.screenshot.shortcut-hint-opacity"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Shortcut hint opacity")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                           "Set the overall opacity of screenshot shortcut hints")),
            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Hotkey hint opacity"))},
            QStringLiteral("screenshot_ui/shortcut_hint_opacity"),
            SettingsSliderDefinition{SettingsSliderBinding::ShortcutHintOpacity,
                                     settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "%"))}};
}

SettingsItemDefinition drawingToolbarEditorItem() {
    return {QStringLiteral("interface.toolbar.drawing-toolbar-editor"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Drawing toolbar settings")),
            settingsText(QT_TRANSLATE_NOOP(
                "SettingsCatalog",
                "Drag drawing tools to reorder them or stack them in the same toolbar position.")),
            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Tool positions")),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Stack drawing tools"))},
            QStringLiteral("screenshot_toolbar/layout"),
            SettingsCustomDefinition{SettingsCustomRenderer::DrawingToolbarEditor}};
}

SettingsItemDefinition pinBorderColorItem() {
    return screenshotColorItem(
        QStringLiteral("interface.pin-to-screen.border-color"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Border color"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Set the border color of pinned screenshots"),
        QStringLiteral("pin_to_screen/border_color"), SettingsColorBinding::PinBorderColor,
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pinned window border"))});
}

SettingsItemDefinition trayEnabledItem() {
    return {QStringLiteral("interface.tray.enabled"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Enable tray")),
            settingsText(QT_TRANSLATE_NOOP(
                "SettingsCatalog", "Show the application icon and menu in the system tray")),
            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "System tray"))},
            QStringLiteral("tray/enabled"),
            SettingsSwitchDefinition{SettingsSwitchBinding::TrayEnabled}};
}

SettingsItemDefinition trayIconItem() {
    SettingsRadioDefinition payload;
    payload.options = {
        {QStringLiteral("default"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Default")),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-default.png")},
        {QStringLiteral("light"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Light")),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-light.png")},
        {QStringLiteral("dark"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Dark")),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-dark.png")},
        {QStringLiteral("snow-default"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Snowflake")),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-snow-default.png")},
        {QStringLiteral("snow-light"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Snowflake Light")),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-snow-light.png")},
        {QStringLiteral("snow-dark"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Snowflake Dark")),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-snow-dark.png")},
    };
    return {QStringLiteral("interface.tray.icon"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Icon")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                           "Choose the bundled icon used in the system tray")),
            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Tray appearance"))},
            QStringLiteral("tray/icon"),
            payload};
}

SettingsItemDefinition trayCustomIconItem() {
    return {
        QStringLiteral("interface.tray.custom-icon"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Custom icon")),
        settingsText(QT_TRANSLATE_NOOP(
            "SettingsCatalog",
            "Enter or browse to a PNG or ICO file; invalid files use the selected bundled icon")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Tray icon path"))},
        QStringLiteral("tray/custom_icon"),
        SettingsFilePathDefinition{
            SettingsFilePathBinding::TrayCustomIcon,
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Browse")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Select tray icon")),
            settingsText(QT_TRANSLATE_NOOP(
                "SettingsCatalog",
                "Image files (*.png *.ico);;PNG images (*.png);;Icon files (*.ico)"))}};
}

SettingsItemDefinition applicationPriorityItem() {
    SettingsSelectDefinition payload;
    payload.options = {
        {QStringLiteral("normal"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Normal"))},
        {QStringLiteral("above_normal"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Above normal"))},
        {QStringLiteral("high"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "High"))},
        {QStringLiteral("real_time"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Real-time"))},
    };
    payload.binding = SettingsSelectBinding::ApplicationPriority;
    return {
        QStringLiteral("system.application-priority"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Application priority")),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                       "Choose how much execution time the application receives")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Process priority")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Execution order"))},
        QStringLiteral("system/application_priority"),
        payload,
    };
}

SettingsItemDefinition proxyItem() {
    SettingsSelectDefinition payload;
    payload.options = {
        {QStringLiteral("none"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "No proxy"))},
        {QStringLiteral("system"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Use system proxy"))},
    };
    payload.binding = SettingsSelectBinding::Proxy;
    return {
        QStringLiteral("network.proxy"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Proxy")),
        settingsText(QT_TRANSLATE_NOOP(
            "SettingsCatalog", "Choose whether network requests use the system proxy")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Network proxy"))},
        QStringLiteral("network/proxy"),
        payload,
    };
}

SettingsItemDefinition historyEnabledItem() {
    return {
        QStringLiteral("history.enabled"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Persistent capture history")),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                       "Keep captures available after the application closes")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Save history"))},
        QStringLiteral("capture_history/enabled"),
        SettingsSwitchDefinition{},
    };
}

SettingsItemDefinition smartSelectionItem() {
    return {
        QStringLiteral("screenshot.smart-selection"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Smart Selection")),
        settingsText(QT_TRANSLATE_NOOP(
            "SettingsCatalog", "Select child elements within a window while taking a screenshot")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Child elements")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "MSAA"))},
        QStringLiteral("screenshot_selection/smart_selection"),
        SettingsSwitchDefinition{SettingsSwitchBinding::SmartSelection},
    };
}

SettingsItemDefinition trayMenuOptionsItem() {
    return {
        QStringLiteral("tray.menu-options"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Menu Options")),
        settingsText(QT_TRANSLATE_NOOP(
            "SettingsCatalog", "Choose the functions shown in the system tray menu")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Tray menu"))},
        QStringLiteral("tray/menu_options"),
        SettingsCustomDefinition{SettingsCustomRenderer::TrayMenuOptions},
    };
}

SettingsItemDefinition pinClipboardContentItem() {
    return quickActionItem(
        QStringLiteral("quick.pin-clipboard-content"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Pin Clipboard Content to Screen"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Pin an image, formatted text, or HTML from the clipboard to the screen"),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Clipboard content")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pin clipboard"))},
        GlobalShortcutAction::PinClipboardContent,
        QStringLiteral("global_shortcuts/pin_clipboard_content"),
        []() { return custom_outlined_icons::PinToScreen(); });
}

SettingsItemDefinition fixedSelectItem(
    const QString& id, const char* title, const char* description, const QString& key,
    SettingsSelectBinding binding, QVector<SettingsOptionDefinition> options,
    QVector<TranslatableText> aliases = {}) {
    SettingsSelectDefinition payload;
    payload.binding = binding;
    payload.options = std::move(options);
    return {id, settingsText(title), settingsText(description), std::move(aliases), key,
            std::move(payload)};
}

SettingsItemDefinition switchItem(const QString& id, const char* title,
                                  const char* description, const QString& key,
                                  SettingsSwitchBinding binding,
                                  QVector<TranslatableText> aliases = {}) {
    return {id, settingsText(title), settingsText(description), std::move(aliases), key,
            SettingsSwitchDefinition{binding}};
}

SettingsItemDefinition directoryPathItem(const QString& id, const char* title,
                                         const char* description, const QString& key,
                                         SettingsDirectoryPathBinding binding,
                                         const char* dialogTitle) {
    return {id,
            settingsText(title),
            settingsText(description),
            {},
            key,
            SettingsDirectoryPathDefinition{
                binding,
                settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Browse")),
                settingsText(dialogTitle)}};
}

SettingsItemDefinition textFormatItem(const QString& id, const char* title,
                                      const char* description, const QString& key,
                                      SettingsTextBinding binding) {
    return {id, settingsText(title), settingsText(description), {}, key,
            SettingsTextDefinition{binding}};
}

SettingsItemDefinition screenshotImageFormatItem() {
    return fixedSelectItem(
        QStringLiteral("screenshot-output.image-format"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Image format"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Choose the format used for automatically saved screenshot files"),
        QStringLiteral("screenshot/image_format"), SettingsSelectBinding::ScreenshotImageFormat,
        {{QStringLiteral("png"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "PNG"))},
         {QStringLiteral("jpeg"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "JPEG"))},
         {QStringLiteral("webp"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "WebP"))},
         {QStringLiteral("jxl"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "JPEG XL"))},
         {QStringLiteral("avif"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "AVIF"))}});
}

QVector<SettingsItemDefinition> screenshotOutputItems() {
    return {
        directoryPathItem(
            QStringLiteral("screenshot-output.image-save-directory"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Image save directory"),
            QT_TRANSLATE_NOOP(
                "SettingsCatalog",
                "Choose where images are written for automatic save and copy-file actions"),
            QStringLiteral("screenshot/image_save_directory"),
            SettingsDirectoryPathBinding::ScreenshotImageDirectory,
            QT_TRANSLATE_NOOP("SettingsCatalog", "Select image save directory")),
        screenshotImageFormatItem(),
        textFormatItem(
            QStringLiteral("screenshot-output.manual-filename-format"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Manual save screenshot filename format"),
            QT_TRANSLATE_NOOP(
                "SettingsCatalog",
                "Set the generated filename used when saving a screenshot as a file"),
            QStringLiteral("screenshot/manual_save_filename_format"),
            SettingsTextBinding::ScreenshotManualFilenameFormat),
        textFormatItem(
            QStringLiteral("screenshot-output.auto-filename-format"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Auto-save screenshot filename format"),
            QT_TRANSLATE_NOOP(
                "SettingsCatalog",
                "Set the generated filename used by automatic screenshot file saves"),
            QStringLiteral("screenshot/auto_save_filename_format"),
            SettingsTextBinding::ScreenshotAutoFilenameFormat),
    };
}

QVector<SettingsItemDefinition> videoOutputItems() {
    return {
        directoryPathItem(
            QStringLiteral("screen-recording-output.video-save-directory"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Video save directory"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Choose where recording output files are written"),
            QStringLiteral("screen_recording/video_save_directory"),
            SettingsDirectoryPathBinding::ScreenRecordingVideoDirectory,
            QT_TRANSLATE_NOOP("SettingsCatalog", "Select video save directory")),
        textFormatItem(
            QStringLiteral("screen-recording-output.video-filename-format"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Video filename format"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Set the generated filename used for recording output files"),
            QStringLiteral("screen_recording/video_filename_format"),
            SettingsTextBinding::ScreenRecordingVideoFilenameFormat),
    };
}

SettingsItemDefinition screenshotOcrActionItem() {
    return fixedSelectItem(
        QStringLiteral("screenshot.auto-execute-after-text-recognition"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Auto execute after text recognition"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Choose what happens automatically when text recognition completes"),
        QStringLiteral("screenshot/auto_execute_after_text_recognition"),
        SettingsSelectBinding::ScreenshotOcrAction,
        {
            {QStringLiteral("no_action"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "No action"))},
            {QStringLiteral("copy_text"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Copy text"))},
            {QStringLiteral("copy_text_and_end_screenshot"),
             settingsText(
                 QT_TRANSLATE_NOOP("SettingsCatalog", "Copy text and end screenshot"))},
            {QStringLiteral("quick_copy_text"),
             settingsText(
                 QT_TRANSLATE_NOOP("SettingsCatalog", "Copy text (Quick function)"))},
            {QStringLiteral("quick_copy_text_and_end_screenshot"),
             settingsText(QT_TRANSLATE_NOOP(
                 "SettingsCatalog", "Copy text and end screenshot (Quick function)"))},
            {QStringLiteral("enable_edit_mode"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Enable edit mode"))},
        });
}

QVector<SettingsOptionDefinition> screenshotPointerActionOptions() {
    return {
        {QStringLiteral("copy"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Copy to clipboard"))},
        {QStringLiteral("save"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Save as file"))},
        {QStringLiteral("pin"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pin to screen"))},
        {QStringLiteral("none"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "None"))},
    };
}

SettingsItemDefinition screenshotDoubleClickActionItem() {
    return fixedSelectItem(
        QStringLiteral("screenshot.double-click-action"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Double-click action"),
        QT_TRANSLATE_NOOP(
            "SettingsCatalog",
            "Choose the action for double-clicking while moving or drawing in a screenshot"),
        QStringLiteral("screenshot/double_click_action"),
        SettingsSelectBinding::ScreenshotDoubleClickAction, screenshotPointerActionOptions());
}

SettingsItemDefinition screenshotMiddleClickActionItem() {
    return fixedSelectItem(
        QStringLiteral("screenshot.middle-mouse-button-action"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Middle mouse button action"),
        QT_TRANSLATE_NOOP(
            "SettingsCatalog",
            "Choose the action for middle-clicking while moving or drawing in a screenshot"),
        QStringLiteral("screenshot/middle_mouse_button_action"),
        SettingsSelectBinding::ScreenshotMiddleClickAction, screenshotPointerActionOptions());
}

SettingsItemDefinition screenshotAutoSaveAfterCopyItem() {
    return switchItem(
        QStringLiteral("screenshot.auto-save-after-copy"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Auto save after copy"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Save a PNG file automatically whenever a screenshot is copied"),
        QStringLiteral("screenshot/auto_save_after_copy"),
        SettingsSwitchBinding::ScreenshotAutoSaveAfterCopy);
}

SettingsItemDefinition screenshotCopyFileItem() {
    return switchItem(
        QStringLiteral("screenshot.copy-image-file-to-clipboard"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Copy image file to clipboard"),
        QT_TRANSLATE_NOOP(
            "SettingsCatalog",
            "Write the screenshot to a file and copy that file to the clipboard"),
        QStringLiteral("screenshot/copy_image_file_to_clipboard"),
        SettingsSwitchBinding::ScreenshotCopyImageFileToClipboard);
}

SettingsItemDefinition drawingQuickSelectionItem() {
    SettingsMultiSelectDefinition payload;
    payload.options = {
        {QStringLiteral("shape"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Shape tool"))},
        {QStringLiteral("arrow"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Arrow"))},
        {QStringLiteral("line"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Line"))},
        {QStringLiteral("free-draw"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Brush"))},
        {QStringLiteral("rectangle-highlight"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Rectangle highlight"))},
        {QStringLiteral("pen-highlight"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pen highlight"))},
        {QStringLiteral("spotlight"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Spotlight"))},
        {QStringLiteral("rectangle-filter"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Rectangle filter"))},
        {QStringLiteral("pen-filter"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pen filter"))},
        {QStringLiteral("text"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Text"))},
        {QStringLiteral("serial-number"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Serial number"))},
        {QStringLiteral("eraser"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Eraser"))},
        {QStringLiteral("watermark"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Watermark"))},
    };
    return {
        QStringLiteral("drawing.quick-selection-disabled-tools"),
        settingsText(QT_TRANSLATE_NOOP(
            "SettingsCatalog", "Tools that forbid quick selection of same-type elements")),
        settingsText(QT_TRANSLATE_NOOP(
            "SettingsCatalog",
            "Prevent left-click selection of matching elements while these tools are active")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Quick selection"))},
        QStringLiteral("drawing/quick_selection_disabled_tools"),
        std::move(payload),
    };
}

SettingsItemDefinition pinZoomModeItem() {
    return fixedSelectItem(
        QStringLiteral("pin-to-screen.mouse-wheel-zoom-mode"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Mouse wheel zoom mode"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Choose the fixed point used when zooming a pinned screenshot"),
        QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode"),
        SettingsSelectBinding::PinMouseWheelZoomMode,
        {
            {QStringLiteral("mouse_position"),
             settingsText(
                 QT_TRANSLATE_NOOP("SettingsCatalog", "Center on mouse position"))},
            {QStringLiteral("top_left"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Fix top-left corner"))},
            {QStringLiteral("top_right"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Fix top-right corner"))},
            {QStringLiteral("bottom_left"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Fix bottom-left corner"))},
            {QStringLiteral("bottom_right"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Fix bottom-right corner"))},
            {QStringLiteral("center"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Fix center point"))},
        });
}

SettingsItemDefinition pinAutomaticOcrItem() {
    return switchItem(
        QStringLiteral("pin-to-screen.automatic-text-recognition"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Automatic text recognition"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Recognize text automatically when a pinned screenshot is created"),
        QStringLiteral("pin_to_screen/automatic_text_recognition"),
        SettingsSwitchBinding::PinAutomaticTextRecognition);
}

SettingsItemDefinition pinAutoResizeItem() {
    return switchItem(
        QStringLiteral("pin-to-screen.auto-resize-window"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Auto resize window"),
        QT_TRANSLATE_NOOP(
            "SettingsCatalog",
            "Resize scrolling screenshots automatically to remain inside the monitor"),
        QStringLiteral("pin_to_screen/auto_resize_window"),
        SettingsSwitchBinding::PinAutoResizeWindow);
}

SettingsItemDefinition trayLeftClickItem() {
    return fixedSelectItem(
        QStringLiteral("tray.left-click-action"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Left-click action"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Choose what left-clicking the tray icon does"),
        QStringLiteral("tray/left_click_action"), SettingsSelectBinding::TrayLeftClickAction,
        {
            {QStringLiteral("screenshot"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot"))},
            {QStringLiteral("show_main_window"),
             settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Show main window"))},
        });
}

QVector<SettingsOptionDefinition> clarityOptions(bool includeHighRes) {
    QVector<SettingsOptionDefinition> options;
    if (includeHighRes) {
        options.push_back(
            {QStringLiteral("4k"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "4K"))});
        options.push_back(
            {QStringLiteral("2k"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "2K"))});
    }
    options.push_back({QStringLiteral("1080p"),
                       settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "1080p"))});
    options.push_back({QStringLiteral("720p"),
                       settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "720p"))});
    options.push_back({QStringLiteral("480p"),
                       settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "480p"))});
    return options;
}

QVector<SettingsOptionDefinition> frameRateOptions(std::initializer_list<int> frameRates) {
    QVector<SettingsOptionDefinition> options;
    options.reserve(static_cast<qsizetype>(frameRates.size()));
    for (int frameRate : frameRates) {
        options.push_back({frameRate, {}});
        options.last().label = frameRate == 83
                                   ? settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "83"))
                               : frameRate == 120
                                   ? settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "120"))
                               : frameRate == 60
                                   ? settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "60"))
                               : frameRate == 30
                                   ? settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "30"))
                               : frameRate == 24
                                   ? settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "24"))
                               : frameRate == 15
                                   ? settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "15"))
                                   : settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "10"));
    }
    return options;
}

QVector<SettingsItemDefinition> screenRecordingItems() {
    return {
        fixedSelectItem(
            QStringLiteral("screen-recording.clarity"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Screen recording clarity"),
            QT_TRANSLATE_NOOP("SettingsCatalog",
                              "Scale recordings that exceed the selected maximum resolution"),
            QStringLiteral("screen_recording/clarity"),
            SettingsSelectBinding::ScreenRecordingClarity, clarityOptions(true)),
        fixedSelectItem(
            QStringLiteral("screen-recording.frame-rate"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Frame rate"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Set the screen recording frame rate"),
            QStringLiteral("screen_recording/frame_rate"),
            SettingsSelectBinding::ScreenRecordingFrameRate, frameRateOptions({10, 15, 24, 30, 60, 120, 83})),
        fixedSelectItem(
            QStringLiteral("screen-recording.animated-image-clarity"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Animated image clarity"),
            QT_TRANSLATE_NOOP("SettingsCatalog",
                              "Set the maximum resolution of exported animated images"),
            QStringLiteral("screen_recording/animated_image_clarity"),
            SettingsSelectBinding::AnimatedImageClarity, clarityOptions(false)),
        fixedSelectItem(
            QStringLiteral("screen-recording.animated-image-frame-rate"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Animated image frame rate"),
            QT_TRANSLATE_NOOP("SettingsCatalog",
                              "Set the frame rate of exported animated images"),
            QStringLiteral("screen_recording/animated_image_frame_rate"),
            SettingsSelectBinding::AnimatedImageFrameRate, frameRateOptions({10, 15, 24})),
        fixedSelectItem(
            QStringLiteral("screen-recording.animated-image-format"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Animated image format"),
            QT_TRANSLATE_NOOP("SettingsCatalog",
                              "Choose the format used to export animated images"),
            QStringLiteral("screen_recording/animated_image_format"),
            SettingsSelectBinding::AnimatedImageFormat,
            {{QStringLiteral("gif"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "GIF"))},
             {QStringLiteral("apng"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "APNG"))},
             {QStringLiteral("webp"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "WebP"))}}),
        fixedSelectItem(
            QStringLiteral("screen-recording.encoder"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Encoder"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Choose the video encoder"),
            QStringLiteral("screen_recording/encoder"), SettingsSelectBinding::ScreenRecordingEncoder,
            {{QStringLiteral("h264"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "H.264"))},
             {QStringLiteral("h265"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "H.265"))}}),
        fixedSelectItem(
            QStringLiteral("screen-recording.encoding-preset"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Encoding preset"),
            QT_TRANSLATE_NOOP("SettingsCatalog",
                              "Balance encoding speed against compression efficiency"),
            QStringLiteral("screen_recording/encoding_preset"),
            SettingsSelectBinding::ScreenRecordingEncodingPreset,
            {{QStringLiteral("ultrafast"),
              settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Ultra fast"))},
             {QStringLiteral("veryfast"),
              settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Very fast"))},
             {QStringLiteral("medium"),
              settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Medium"))},
             {QStringLiteral("veryslow"),
              settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Very slow"))},
             {QStringLiteral("placebo"),
              settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Maximum compression"))}}),
        switchItem(
            QStringLiteral("screen-recording.hide-toolbar"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Hide toolbar in recording"),
            QT_TRANSLATE_NOOP("SettingsCatalog",
                              "Exclude the screen recording toolbar from captured video"),
            QStringLiteral("screen_recording/hide_toolbar_in_recording"),
            SettingsSwitchBinding::ScreenRecordingHideToolbar),
    };
}

SettingsItemDefinition fullscreenHotkeySuppressionItem() {
    return switchItem(
        QStringLiteral("global-hotkeys.disable-on-focused-fullscreen-window"),
        QT_TRANSLATE_NOOP(
            "SettingsCatalog",
            "Automatically disable when a focused fullscreen window exists"),
        QT_TRANSLATE_NOOP(
            "SettingsCatalog",
            "Ignore global hotkeys while the focused window occupies an entire monitor"),
        QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window"),
        SettingsSwitchBinding::DisableHotkeysOnFocusedFullscreen);
}

SettingsItemDefinition autoStartItem() {
    return switchItem(
        QStringLiteral("system.auto-start-at-boot"),
        QT_TRANSLATE_NOOP("SettingsCatalog", "Auto start at boot"),
        QT_TRANSLATE_NOOP("SettingsCatalog",
                          "Start Snow Shot in the background when Windows starts"),
        QStringLiteral("system/auto_start_at_boot"), SettingsSwitchBinding::AutoStartAtBoot);
}

SettingsItemDefinition localShortcutItem(
    SettingsLocalShortcutScope scope, const QString& shortcutId, const char* title,
    std::function<adqt::icons::IconRef()> iconFactory) {
    const bool screenshotShortcut = scope == SettingsLocalShortcutScope::Screenshot;
    const bool drawingShortcut = scope == SettingsLocalShortcutScope::Drawing;
    const QString scopeName = screenshotShortcut
                                  ? QStringLiteral("screenshot")
                                  : drawingShortcut ? QStringLiteral("drawing")
                                                    : QStringLiteral("pin-to-screen");
    const QString configurationPrefix = screenshotShortcut
                                            ? QStringLiteral("screenshot_shortcuts/")
                                            : drawingShortcut
                                                  ? QStringLiteral("drawing_shortcuts/")
                                                  : QStringLiteral("pin_to_screen_shortcuts/");
    return {
        QStringLiteral("%1-shortcut.%2").arg(scopeName, shortcutId),
        settingsText(title),
        screenshotShortcut
            ? settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                             "Set up to two keys for this screenshot action"))
            : drawingShortcut
                  ? settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Set up to two keys for this screenshot drawing tool"))
                  : settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Set up to two keys for this pinned window action")),
        {settingsText(screenshotShortcut
                          ? QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot shortcut")
                          : drawingShortcut
                                ? QT_TRANSLATE_NOOP("SettingsCatalog", "Drawing shortcut")
                                : QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "Pin to Screen shortcut"))},
        configurationPrefix + shortcutId,
        SettingsLocalShortcutDefinition{shortcutId, std::move(iconFactory), scope},
    };
}

QVector<SettingsItemDefinition> screenshotShortcutItems() {
    return {
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("move_tool"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Edit selection"),
            []() { return custom_outlined_icons::ToolMove(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("move_cursor_up"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Move cursor up"),
            []() { return outlined_icons::ArrowUp(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("move_cursor_down"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Move cursor down"),
            []() { return outlined_icons::ArrowDown(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("move_cursor_left"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Move cursor left"),
            []() { return outlined_icons::ArrowLeft(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("move_cursor_right"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Move cursor right"),
            []() { return outlined_icons::ArrowRight(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("move_entire_selection"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Move Entire Selection"),
            []() { return custom_outlined_icons::ToolMove(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot,
            QStringLiteral("keep_selection_width_and_height_consistent"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Keep Selection Width and Height Consistent"),
            []() { return outlined_icons::Control(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot,
            QStringLiteral("switch_selection_between_window_and_window_sub_element"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Select Window/Window Sub-element"),
            []() { return outlined_icons::Function(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("previous_screenshot_history"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Previous Screenshot History"),
            []() { return outlined_icons::History(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("next_screenshot_history"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Next Screenshot History"),
            []() { return outlined_icons::History(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("select_previously_selected_area"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Select Previously Selected Area"),
            []() { return outlined_icons::Rest(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("copy_color"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Copy Color"),
            []() { return outlined_icons::Copy(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("pin_to_screen"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Pin to Screen"),
            []() { return custom_outlined_icons::PinToScreen(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("video_recording"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Video Recording"),
            []() { return custom_outlined_icons::RecordScreen(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("scrolling_screenshot"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Scrolling Screenshot"),
            []() { return custom_outlined_icons::ScrollingScreenshot(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("save_as_file"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Save as File"),
            []() { return custom_outlined_icons::Save(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("cancel_screenshot"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Cancel Screenshot"),
            []() { return outlined_icons::Close(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("copy_to_clipboard"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Copy to Clipboard"),
            []() { return outlined_icons::Copy(); }),
    };
}

QVector<SettingsItemDefinition> screenshotOtherShortcutItems() {
    return {
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("table_recognition"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Table Recognition"),
            []() { return custom_outlined_icons::TableRecognition(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("qr_code_recognition"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "QR Code Recognition"),
            []() { return custom_outlined_icons::ScanQrcode(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("text_recognition"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Text Recognition"),
            []() { return custom_outlined_icons::ToolRecognizeText(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Screenshot, QStringLiteral("text_translation"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Text Translation"),
            []() { return custom_outlined_icons::OcrTranslate(); }),
        localShortcutItem(SettingsLocalShortcutScope::Screenshot, QStringLiteral("undo"),
                          QT_TRANSLATE_NOOP("SettingsCatalog", "Undo"),
                          []() { return outlined_icons::Undo(); }),
        localShortcutItem(SettingsLocalShortcutScope::Screenshot, QStringLiteral("redo"),
                          QT_TRANSLATE_NOOP("SettingsCatalog", "Redo"),
                          []() { return outlined_icons::Redo(); }),
    };
}

QVector<SettingsItemDefinition> drawingShortcutItems() {
    return {
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("select"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Select tool"),
            []() { return custom_outlined_icons::ToolSelect(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("shape"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Shape tool"),
            []() { return custom_outlined_icons::ToolRectangle(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("arrow"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Arrow"),
            []() { return custom_outlined_icons::ToolArrow(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("brush"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Brush"),
            []() { return custom_outlined_icons::ToolFreeDraw(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("highlight"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Highlight"),
            []() { return custom_outlined_icons::ToolHighlight(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("text"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Text"),
            []() { return custom_outlined_icons::ToolText(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("serial_number"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Serial number"),
            []() { return custom_outlined_icons::ToolSerialNumber(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("filter"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Filter"),
            []() { return custom_outlined_icons::ToolFilter(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("eraser"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Eraser"),
            []() { return custom_outlined_icons::ToolEraser(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::Drawing, QStringLiteral("watermark"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Watermark"),
            []() { return custom_outlined_icons::ToolWatermark(); }),
    };
}

QVector<SettingsItemDefinition> pinToScreenShortcutItems() {
    return {
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen, QStringLiteral("copy_to_clipboard"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Copy to Clipboard"),
            []() { return outlined_icons::Copy(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen, QStringLiteral("copy_original_content"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Copy Original Content"),
            []() { return outlined_icons::FileImage(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen,
            QStringLiteral("show_text_recognition_results"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Show Text Recognition Results"),
            []() { return custom_outlined_icons::ToolRecognizeText(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen, QStringLiteral("drawing_mode"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Drawing Mode"),
            []() { return outlined_icons::Edit(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen, QStringLiteral("thumbnail_mode"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Thumbnail Mode"),
            []() { return outlined_icons::Compress(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen, QStringLiteral("close_window"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Close Window"),
            []() { return outlined_icons::Close(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen, QStringLiteral("move_cursor_up"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Move cursor up"),
            []() { return outlined_icons::ArrowUp(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen, QStringLiteral("move_cursor_down"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Move cursor down"),
            []() { return outlined_icons::ArrowDown(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen, QStringLiteral("move_cursor_left"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Move cursor left"),
            []() { return outlined_icons::ArrowLeft(); }),
        localShortcutItem(
            SettingsLocalShortcutScope::PinToScreen, QStringLiteral("move_cursor_right"),
            QT_TRANSLATE_NOOP("SettingsCatalog", "Move cursor right"),
            []() { return outlined_icons::ArrowRight(); }),
    };
}

SettingsItemDefinition directMlAccelerationItem() {
    return {
        QStringLiteral("text-recognition.direct-ml-acceleration"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Direct ML acceleration")),
        settingsText(
            QT_TRANSLATE_NOOP("SettingsCatalog",
                              "Use Direct ML for GPU-accelerated text recognition when available")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "DirectML")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "GPU acceleration"))},
        QStringLiteral("text_recognition/direct_ml_acceleration"),
        SettingsSwitchDefinition{SettingsSwitchBinding::DirectMlAcceleration},
    };
}

SettingsItemDefinition historyIntegerItem(const QString& id, TranslatableText title,
                                          TranslatableText description, const QString& key,
                                          SettingsIntegerBinding binding, TranslatableText suffix,
                                          QVector<TranslatableText> aliases = {}) {
    return {
        id, title, description, aliases, key, SettingsIntegerDefinition{binding, suffix},
    };
}

SettingsItemDefinition clearHistoryItem() {
    SettingsActionDefinition payload;
    payload.buttonText = settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Clear history"));
    payload.accent = SettingsActionAccent::Danger;
    payload.iconFactory = []() { return outlined_icons::Rest(); };
    payload.confirmation = {
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Clear capture history?")),
        settingsText(QT_TRANSLATE_NOOP(
            "SettingsCatalog", "All capture history and quarantined records will be removed")),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Clear history")),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Cancel")),
    };
    return {
        QStringLiteral("history.clear"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Clear capture history")),
        settingsText(QT_TRANSLATE_NOOP(
            "SettingsCatalog", "Permanently remove all saved captures and quarantined records")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Delete history")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Remove captures"))},
        {},
        payload,
    };
}

SettingsItemDefinition storageStatusItem() {
    return {
        QStringLiteral("storage.status"),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Storage status")),
        settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                       "Current storage location, mode, usage, and latest errors")),
        {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Disk usage")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Storage location")),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Storage error"))},
        {},
        SettingsCustomDefinition{},
    };
}

QVector<SettingsPageDefinition> builtInPages() {
    return {
        {
            QString::fromLatin1(QUICK_PAGE_ID),
            QStringLiteral("/"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Quick Functions")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Quick Functions page")),
            {
                {
                    QStringLiteral("screenshot"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot")),
                    settingsText(
                        QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot shortcuts and actions")),
                    SettingsSectionReset::ScreenshotShortcuts,
                    {
                        screenshotItem(),
                        screenshotDelayItem(),
                        screenshotFixedItem(),
                        screenshotOcrItem(),
                        screenshotTranslationItem(),
                        screenshotCopyItem(),
                        screenshotFullScreenItem(),
                        screenshotFocusedWindowItem(),
                    },
                },
                {
                    QStringLiteral("screen-recording"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screen Recording")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "Screen recording shortcuts and actions")),
                    SettingsSectionReset::None,
                    {
                        screenRecordItem(),
                        screenRecordCopyItem(),
                    },
                },
                {
                    QStringLiteral("other"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Other")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "Other application shortcuts and actions")),
                    SettingsSectionReset::OtherShortcuts,
                    {
                        openCaptureHistoryItem(),
                        pinClipboardContentItem(),
                    },
                },
            },
        },
        {
            QString::fromLatin1(HISTORY_PAGE_ID),
            QStringLiteral("/history"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot History")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                           "Preview and manage saved screenshot history")),
            {},
            SettingsPageKind::ScreenshotHistory,
        },
        {
            QString::fromLatin1(FUNCTION_PAGE_ID),
            QStringLiteral("/settings/functionSettings"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Function Settings")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Configure screenshot behavior")),
            {
                {
                    QStringLiteral("screenshot-settings"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot")),
                    settingsText(
                        QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot selection behavior")),
                    SettingsSectionReset::ScreenshotSettings,
                    {smartSelectionItem(), screenshotOcrActionItem(),
                     screenshotDoubleClickActionItem(), screenshotMiddleClickActionItem(),
                     screenshotAutoSaveAfterCopyItem(), screenshotCopyFileItem()},
                },
                {
                    QStringLiteral("pin-to-screen-settings"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pin to Screen")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "Pinned screenshot window appearance settings")),
                    SettingsSectionReset::PinToScreenBehavior,
                    {pinZoomModeItem(), pinAutomaticOcrItem(), pinAutoResizeItem()},
                },
                {
                    QStringLiteral("drawing-settings"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Drawing")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog",
                        "Configure drawing tools and the screenshot drawing toolbar")),
                    SettingsSectionReset::DrawingQuickSelection,
                    {drawingQuickSelectionItem()},
                },
                {
                    QStringLiteral("screen-recording-settings"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screen Recording")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "Screen recording and animated image export settings")),
                    SettingsSectionReset::ScreenRecording,
                    screenRecordingItems(),
                },
                {
                    QStringLiteral("tray-settings"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Tray")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "System tray availability and icon settings")),
                    SettingsSectionReset::TrayBehavior,
                    {trayLeftClickItem(), trayMenuOptionsItem()},
                },
                {
                    QStringLiteral("global-hotkeys"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Global Hotkeys")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "Global hotkey activation behavior")),
                    SettingsSectionReset::GlobalHotkeys,
                    {fullscreenHotkeySuppressionItem()},
                },
            },
        },
        {
            QString::fromLatin1(INTERFACE_PAGE_ID),
            QStringLiteral("/settings/generalSettings"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Interface Settings")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Interface Settings page")),
            {
                {
                    QStringLiteral("general"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "General")),
                    settingsText(
                        QT_TRANSLATE_NOOP("SettingsCatalog", "Appearance and language settings")),
                    SettingsSectionReset::GeneralSettings,
                    {themeItem(), languageItem()},
                },
                {
                    QStringLiteral("interface-screenshot"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Screenshot interface and visual guidance settings")),
                    SettingsSectionReset::ScreenshotInterfaceSettings,
                    {
                        selectionTransitionAnimationItem(),
                        colorPickerDisplayModeItem(),
                        screenshotColorItem(
                            QStringLiteral("interface.screenshot.selection-mask-color"),
                            QT_TRANSLATE_NOOP("SettingsCatalog", "Selection mask color"),
                            QT_TRANSLATE_NOOP(
                                "SettingsCatalog",
                                "Set the color and opacity outside the screenshot selection"),
                            QStringLiteral("screenshot_ui/selection_mask_color"),
                            SettingsColorBinding::SelectionMaskColor),
                        shortcutHintOpacityItem(),
                        screenshotColorItem(
                            QStringLiteral("interface.screenshot.cursor-guide-line-color"),
                            QT_TRANSLATE_NOOP("SettingsCatalog", "Cursor guide line color"),
                            QT_TRANSLATE_NOOP(
                                "SettingsCatalog",
                                "Draw a dashed crosshair at the pointer while selecting"),
                            QStringLiteral("screenshot_ui/cursor_guide_line_color"),
                            SettingsColorBinding::CursorGuideLineColor),
                        screenshotColorItem(
                            QStringLiteral("interface.screenshot.monitor-center-guide-line-color"),
                            QT_TRANSLATE_NOOP("SettingsCatalog", "Monitor center guide line color"),
                            QT_TRANSLATE_NOOP("SettingsCatalog",
                                              "Draw a solid crosshair at the active monitor center "
                                              "while selecting"),
                            QStringLiteral("screenshot_ui/monitor_center_guide_line_color"),
                            SettingsColorBinding::MonitorCenterGuideLineColor),
                        screenshotColorItem(
                            QStringLiteral(
                                "interface.screenshot.color-picker-center-guide-line-color"),
                            QT_TRANSLATE_NOOP("SettingsCatalog",
                                              "Color picker center guide line color"),
                            QT_TRANSLATE_NOOP(
                                "SettingsCatalog",
                                "Draw four guide segments around the sampled center pixel"),
                            QStringLiteral("screenshot_ui/color_picker_center_guide_line_color"),
                            SettingsColorBinding::ColorPickerCenterGuideLineColor),
                    },
                },
                {
                    QStringLiteral("toolbar"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Toolbar")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog",
                        "Configure the screenshot, pinned, and recording toolbars")),
                    SettingsSectionReset::Toolbar,
                    {screenshotToolbarSizeItem()},
                },
                {
                    QStringLiteral("drawing"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Drawing")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog",
                        "Configure drawing tools and the screenshot drawing toolbar")),
                    SettingsSectionReset::DrawingToolbar,
                    {drawingToolbarEditorItem()},
                },
                {
                    QStringLiteral("pin-to-screen"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pin to Screen")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "Pinned screenshot window appearance settings")),
                    SettingsSectionReset::PinToScreen,
                    {pinBorderColorItem()},
                },
                {
                    QStringLiteral("tray"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Tray")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "System tray availability and icon settings")),
                    SettingsSectionReset::Tray,
                    {trayEnabledItem(), trayIconItem(), trayCustomIconItem()},
                },
            },
        },
        {
            QString::fromLatin1(STORAGE_PAGE_ID),
            QStringLiteral("/settings/storageAndPrivacy"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Storage and Privacy")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Storage and Privacy settings page")),
            {
                {
                    QStringLiteral("screenshots"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshots")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Screenshot output locations, formats, and filenames")),
                    SettingsSectionReset::ScreenshotOutput,
                    screenshotOutputItems(),
                },
                {
                    QStringLiteral("screen-recording-output"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screen Recording")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Recording output location and filename settings")),
                    SettingsSectionReset::ScreenRecordingOutput,
                    videoOutputItems(),
                },
                {
                    QStringLiteral("history"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "History")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Capture history retention and cleanup settings")),
                    SettingsSectionReset::HistoryPolicy,
                    {
                        historyEnabledItem(),
                        historyIntegerItem(
                            QStringLiteral("history.retention-days"),
                            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Retention period")),
                            settingsText(QT_TRANSLATE_NOOP(
                                "SettingsCatalog", "Delete captures after they reach this age")),
                            QStringLiteral("capture_history/retention_days"),
                            SettingsIntegerBinding::HistoryRetentionDays,
                            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", " days")),
                            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Age"))}),
                        historyIntegerItem(
                            QStringLiteral("history.max-entries"),
                            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Maximum entries")),
                            settingsText(QT_TRANSLATE_NOOP(
                                "SettingsCatalog",
                                "Remove the oldest captures when this limit is exceeded")),
                            QStringLiteral("capture_history/max_entries"),
                            SettingsIntegerBinding::HistoryMaxEntries, {},
                            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Capture count"))}),
                        historyIntegerItem(
                            QStringLiteral("history.max-disk-mib"),
                            settingsText(
                                QT_TRANSLATE_NOOP("SettingsCatalog", "Maximum disk usage")),
                            settingsText(QT_TRANSLATE_NOOP(
                                "SettingsCatalog",
                                "Limit how much disk space capture history can use")),
                            QStringLiteral("capture_history/max_disk_mib"),
                            SettingsIntegerBinding::HistoryMaxDiskMiB,
                            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", " MiB")),
                            {settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Disk limit"))}),
                        clearHistoryItem(),
                    },
                },
                {
                    QStringLiteral("storage-status"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Storage Status")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Current storage location, mode, usage, and errors")),
                    SettingsSectionReset::None,
                    {storageStatusItem()},
                },
            },
        },
        {
            QString::fromLatin1(SYSTEM_PAGE_ID),
            QStringLiteral("/settings/systemSettings"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "System Settings")),
            settingsText(
                QT_TRANSLATE_NOOP("SettingsCatalog", "Configure application process behavior")),
            {
                {
                    QStringLiteral("system-general"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "General")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "General system integration settings")),
                    SettingsSectionReset::SystemGeneral,
                    {autoStartItem()},
                },
                {
                    QStringLiteral("network"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Network")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Configure proxy use for network requests")),
                    SettingsSectionReset::Network,
                    {proxyItem()},
                },
                {
                    QStringLiteral("text-recognition"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Text Recognition")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "Configure text recognition acceleration")),
                    SettingsSectionReset::TextRecognition,
                    {directMlAccelerationItem()},
                },
                {
                    QStringLiteral("core"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Core")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Core application settings")),
                    SettingsSectionReset::SystemSettings,
                    {applicationPriorityItem()},
                },
            },
        },
        {
            QString::fromLatin1(HOTKEY_PAGE_ID),
            QStringLiteral("/settings/hotKeySettings"),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Hotkey Settings")),
            settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                           "Configure screenshot editor shortcut keys")),
            {
                {
                    QStringLiteral("screenshot-shortcuts"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Screenshot")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Shortcut keys for screenshot tools and cursor movement")),
                    SettingsSectionReset::ScreenshotEditorShortcuts,
                    screenshotShortcutItems(),
                    SettingsSectionItemLayout::TwoColumnGrid,
                },
                {
                    QStringLiteral("drawing-shortcuts"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Drawing")),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog",
                                                   "Shortcut keys for drawing tools")),
                    SettingsSectionReset::DrawingShortcuts,
                    drawingShortcutItems(),
                    SettingsSectionItemLayout::TwoColumnGrid,
                },
                {
                    QStringLiteral("pin-to-screen-shortcuts"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Pin to Screen")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Shortcut keys for pinned-to-screen windows")),
                    SettingsSectionReset::PinToScreenShortcuts,
                    pinToScreenShortcutItems(),
                    SettingsSectionItemLayout::TwoColumnGrid,
                },
                {
                    QStringLiteral("other-shortcuts"),
                    settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Other")),
                    settingsText(QT_TRANSLATE_NOOP(
                        "SettingsCatalog", "Shortcut keys for recognition and capture actions")),
                    SettingsSectionReset::ScreenshotOtherShortcuts,
                    screenshotOtherShortcutItems(),
                    SettingsSectionItemLayout::TwoColumnGrid,
                },
            },
        },
    };
}

QVector<SettingsNavigationNode> builtInNavigation() {
    SettingsNavigationPageDefinition quick;
    quick.id = QStringLiteral("nav.quick-functions");
    quick.pageId = QString::fromLatin1(QUICK_PAGE_ID);
    quick.iconFactory = []() { return outlined_icons::Thunderbolt(); };

    SettingsNavigationPageDefinition history;
    history.id = QStringLiteral("nav.screenshot-history");
    history.pageId = QString::fromLatin1(HISTORY_PAGE_ID);
    history.iconFactory = []() { return outlined_icons::History(); };

    SettingsNavigationGroupDefinition settingsGroup;
    settingsGroup.id = QStringLiteral("nav.settings");
    settingsGroup.title = settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Settings"));
    settingsGroup.iconFactory = []() { return outlined_icons::Setting(); };
    settingsGroup.pages = {
        {
            QStringLiteral("nav.interface-settings"),
            QString::fromLatin1(INTERFACE_PAGE_ID),
            []() { return outlined_icons::Control(); },
        },
        {
            QStringLiteral("nav.function-settings"),
            QString::fromLatin1(FUNCTION_PAGE_ID),
            []() { return outlined_icons::Function(); },
        },
        {
            QStringLiteral("nav.hotkey-settings"),
            QString::fromLatin1(HOTKEY_PAGE_ID),
            []() { return custom_outlined_icons::Keyboard(); },
        },
        {
            QStringLiteral("nav.storage-and-privacy"),
            QString::fromLatin1(STORAGE_PAGE_ID),
            []() { return outlined_icons::Lock(); },
        },
        {
            QStringLiteral("nav.system-settings"),
            QString::fromLatin1(SYSTEM_PAGE_ID),
            []() { return outlined_icons::Control(); },
        },
    };

    return {quick, history, settingsGroup};
}

QString locationText(const SettingsLocation& location) {
    return QStringLiteral("%1/%2/%3").arg(location.pageId, location.sectionId, location.itemId);
}

void addUnique(QStringList* errors, QSet<QString>* values, const QString& value,
               const QString& kind) {
    if (value.trimmed().isEmpty()) {
        errors->push_back(QStringLiteral("%1 must not be empty").arg(kind));
    } else if (values->contains(value)) {
        errors->push_back(QStringLiteral("duplicate %1: %2").arg(kind, value));
    } else {
        values->insert(value);
    }
}

QString shortcutConfigurationKey(GlobalShortcutAction action) {
    switch (action) {
    case GlobalShortcutAction::Screenshot:
        return QStringLiteral("global_shortcuts/screenshot");
    case GlobalShortcutAction::ScreenshotDelay:
        return QStringLiteral("global_shortcuts/screenshot_delay");
    case GlobalShortcutAction::ScreenshotFixed:
        return QStringLiteral("global_shortcuts/screenshot_fixed");
    case GlobalShortcutAction::ScreenshotOcr:
        return QStringLiteral("global_shortcuts/screenshot_ocr");
    case GlobalShortcutAction::ScreenshotTranslation:
        return QStringLiteral("global_shortcuts/screenshot_translation");
    case GlobalShortcutAction::ScreenshotCopy:
        return QStringLiteral("global_shortcuts/screenshot_copy");
    case GlobalShortcutAction::ScreenshotFullScreen:
        return QStringLiteral("global_shortcuts/screenshot_full_screen");
    case GlobalShortcutAction::ScreenshotFocusedWindow:
        return QStringLiteral("global_shortcuts/screenshot_focused_window");
    case GlobalShortcutAction::ScreenRecord:
        return QStringLiteral("global_shortcuts/screen_record");
    case GlobalShortcutAction::ScreenRecordCopy:
        return QStringLiteral("global_shortcuts/screen_record_copy");
    case GlobalShortcutAction::OpenCaptureHistory:
        return QStringLiteral("global_shortcuts/open_capture_history");
    case GlobalShortcutAction::OpenSettings:
        return QStringLiteral("global_shortcuts/open_settings");
    case GlobalShortcutAction::PinClipboardContent:
        return QStringLiteral("global_shortcuts/pin_clipboard_content");
    }
    return {};
}

} // namespace

bool TranslatableText::isValid() const {
    return context != nullptr && source != nullptr && *context != '\0' && *source != '\0';
}

QString TranslatableText::translated() const {
    return isValid() ? QCoreApplication::translate(context, source) : QString();
}

bool SettingsLocation::isEmpty() const {
    return pageId.trimmed().isEmpty();
}

SettingsCatalog::SettingsCatalog(QVector<SettingsPageDefinition> pages,
                                 QVector<SettingsNavigationNode> navigation,
                                 SettingsLocation defaultLocation)
    : m_pages(std::move(pages)), m_navigation(std::move(navigation)),
      m_defaultLocation(std::move(defaultLocation)) {}

const QVector<SettingsPageDefinition>& SettingsCatalog::pages() const {
    return m_pages;
}

const QVector<SettingsNavigationNode>& SettingsCatalog::navigation() const {
    return m_navigation;
}

const SettingsLocation& SettingsCatalog::defaultLocation() const {
    return m_defaultLocation;
}

const SettingsPageDefinition* SettingsCatalog::page(const QString& pageId) const {
    const auto found = std::find_if(m_pages.cbegin(), m_pages.cend(),
                                    [&pageId](const auto& item) { return item.id == pageId; });
    return found == m_pages.cend() ? nullptr : &*found;
}

const SettingsPageDefinition* SettingsCatalog::pageForRoute(const QString& route) const {
    const auto found = std::find_if(m_pages.cbegin(), m_pages.cend(),
                                    [&route](const auto& item) { return item.route == route; });
    return found == m_pages.cend() ? nullptr : &*found;
}

const SettingsSectionDefinition* SettingsCatalog::section(const QString& pageId,
                                                          const QString& sectionId) const {
    const SettingsPageDefinition* foundPage = page(pageId);
    if (foundPage == nullptr) {
        return nullptr;
    }
    const auto found =
        std::find_if(foundPage->sections.cbegin(), foundPage->sections.cend(),
                     [&sectionId](const auto& item) { return item.id == sectionId; });
    return found == foundPage->sections.cend() ? nullptr : &*found;
}

const SettingsItemDefinition* SettingsCatalog::item(const SettingsLocation& location) const {
    const SettingsSectionDefinition* foundSection = section(location.pageId, location.sectionId);
    if (foundSection == nullptr || location.itemId.isEmpty()) {
        return nullptr;
    }
    const auto found =
        std::find_if(foundSection->items.cbegin(), foundSection->items.cend(),
                     [&location](const auto& item) { return item.id == location.itemId; });
    return found == foundSection->items.cend() ? nullptr : &*found;
}

std::optional<SettingsCommand>
SettingsCatalog::commandForShortcut(GlobalShortcutAction action) const {
    const SettingsItemDefinition* itemDefinition = itemForShortcut(action);
    if (itemDefinition != nullptr) {
        return std::get<SettingsShortcutActionDefinition>(itemDefinition->payload).command;
    }
    return std::nullopt;
}

const SettingsItemDefinition*
SettingsCatalog::itemForShortcut(GlobalShortcutAction action) const {
    for (const SettingsPageDefinition& pageDefinition : m_pages) {
        for (const SettingsSectionDefinition& sectionDefinition : pageDefinition.sections) {
            for (const SettingsItemDefinition& itemDefinition : sectionDefinition.items) {
                const auto* shortcut =
                    std::get_if<SettingsShortcutActionDefinition>(&itemDefinition.payload);
                if (shortcut != nullptr && shortcut->shortcutAction == action) {
                    return &itemDefinition;
                }
            }
        }
    }
    return nullptr;
}

QString SettingsCatalog::shortcutActionTitle(GlobalShortcutAction action,
                                             int screenshotDelaySeconds) const {
    const SettingsItemDefinition* itemDefinition = itemForShortcut(action);
    if (itemDefinition == nullptr) {
        return {};
    }
    const auto* shortcut =
        std::get_if<SettingsShortcutActionDefinition>(&itemDefinition->payload);
    Q_ASSERT(shortcut != nullptr);
    QString title = itemDefinition->title.translated();
    if (shortcut->adjustment == SettingsShortcutAdjustment::ScreenshotDelaySeconds) {
        title = title.arg(std::clamp(screenshotDelaySeconds, 1, 10));
    }
    return title;
}

QVector<SettingsTrayMenuGroupDefinition> SettingsCatalog::trayMenuGroups() const {
    QVector<SettingsTrayMenuGroupDefinition> groups;
    const SettingsPageDefinition* quickPage = page(QString::fromLatin1(QUICK_PAGE_ID));
    if (quickPage != nullptr) {
        groups.reserve(quickPage->sections.size() + 1);
        for (const SettingsSectionDefinition& sectionDefinition : quickPage->sections) {
            SettingsTrayMenuGroupDefinition group;
            group.id = sectionDefinition.id;
            for (const SettingsItemDefinition& itemDefinition : sectionDefinition.items) {
                const auto* shortcut =
                    std::get_if<SettingsShortcutActionDefinition>(&itemDefinition.payload);
                if (shortcut == nullptr) {
                    continue;
                }
                group.options.push_back({itemDefinition.id,
                                          itemDefinition.title,
                                          SettingsTrayMenuOptionKind::QuickAction,
                                          shortcut->shortcutAction, shortcut->iconFactory});
            }
            if (!group.options.isEmpty()) {
                groups.push_back(std::move(group));
            }
        }
    }

    SettingsTrayMenuGroupDefinition systemGroup;
    systemGroup.id = QStringLiteral("system");
    systemGroup.options = {
        {QStringLiteral("tray.disable-shortcut-functions"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Disable Shortcut Functions")),
         SettingsTrayMenuOptionKind::DisableShortcutFunctions, GlobalShortcutAction::Screenshot,
         []() { return custom_outlined_icons::Disabled(); }},
        {QStringLiteral("tray.show-main-window"),
         settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Show Main Interface")),
         SettingsTrayMenuOptionKind::ShowMainWindow, GlobalShortcutAction::Screenshot,
         []() { return custom_outlined_icons::Window(); }},
        {QStringLiteral("tray.exit"), settingsText(QT_TRANSLATE_NOOP("SettingsCatalog", "Exit")),
         SettingsTrayMenuOptionKind::Exit, GlobalShortcutAction::Screenshot,
         []() { return custom_outlined_icons::Exit(); }},
    };
    groups.push_back(std::move(systemGroup));
    return groups;
}

SettingsLocation SettingsCatalog::resolveLocation(const SettingsLocation& requested) const {
    const SettingsPageDefinition* foundPage = page(requested.pageId);
    if (foundPage == nullptr) {
        return m_defaultLocation;
    }
    SettingsLocation resolved{foundPage->id, {}, {}};
    const SettingsSectionDefinition* foundSection = section(foundPage->id, requested.sectionId);
    if (foundSection == nullptr) {
        if (foundPage->sections.isEmpty()) {
            return foundPage->kind == SettingsPageKind::ScreenshotHistory ? resolved
                                                                          : m_defaultLocation;
        }
        foundSection = &foundPage->sections.constFirst();
    }
    resolved.sectionId = foundSection->id;
    if (!requested.itemId.isEmpty()) {
        const SettingsLocation itemLocation{resolved.pageId, resolved.sectionId, requested.itemId};
        if (item(itemLocation) != nullptr) {
            resolved.itemId = requested.itemId;
        }
    }
    return resolved;
}

QVector<SettingsSectionSummary> SettingsCatalog::sectionSummaries(const QString& pageId) const {
    QVector<SettingsSectionSummary> result;
    const SettingsPageDefinition* foundPage = page(pageId);
    if (foundPage == nullptr) {
        return result;
    }
    result.reserve(foundPage->sections.size());
    for (const SettingsSectionDefinition& sectionDefinition : foundPage->sections) {
        result.push_back({sectionDefinition.id, sectionDefinition.title.translated()});
    }
    return result;
}

QStringList SettingsCatalog::validationErrors() const {
    QStringList errors;
    QSet<QString> pageIds;
    QSet<QString> routes;
    QSet<QString> sectionIds;
    QSet<QString> itemIds;
    QSet<QString> navigationIds;
    QSet<QString> searchIds;
    QSet<QString> objectNames;
    QSet<GlobalShortcutAction> shortcutActions;

    for (const SettingsPageDefinition& pageDefinition : m_pages) {
        addUnique(&errors, &pageIds, pageDefinition.id, QStringLiteral("page id"));
        addUnique(&errors, &routes, pageDefinition.route, QStringLiteral("route"));
        if (!pageDefinition.route.startsWith(u'/')) {
            errors.push_back(
                QStringLiteral("page route must be absolute: %1").arg(pageDefinition.route));
        }
        if (pageDefinition.sections.isEmpty() &&
            pageDefinition.kind == SettingsPageKind::GeneratedSettings) {
            errors.push_back(QStringLiteral("page has no sections: %1").arg(pageDefinition.id));
        }
        addUnique(&errors, &searchIds, QStringLiteral("page:%1").arg(pageDefinition.id),
                  QStringLiteral("generated search id"));
        addUnique(&errors, &objectNames,
                  generatedObjectName(QStringLiteral("settings-page"), pageDefinition.id),
                  QStringLiteral("generated object name"));
        if (!pageDefinition.title.isValid() || !pageDefinition.description.isValid()) {
            errors.push_back(QStringLiteral("page text is incomplete: %1").arg(pageDefinition.id));
        }
        for (const SettingsSectionDefinition& sectionDefinition : pageDefinition.sections) {
            addUnique(&errors, &sectionIds, sectionDefinition.id, QStringLiteral("section id"));
            addUnique(&errors, &searchIds,
                      QStringLiteral("section:%1/%2").arg(pageDefinition.id, sectionDefinition.id),
                      QStringLiteral("generated search id"));
            addUnique(&errors, &objectNames,
                      generatedObjectName(
                          QStringLiteral("settings-section"),
                          QStringLiteral("%1-%2").arg(pageDefinition.id, sectionDefinition.id)),
                      QStringLiteral("generated object name"));
            if (!sectionDefinition.title.isValid() ||
                !sectionDefinition.searchDescription.isValid()) {
                errors.push_back(QStringLiteral("section text is incomplete: %1/%2")
                                     .arg(pageDefinition.id, sectionDefinition.id));
            }
            if (sectionDefinition.items.isEmpty()) {
                errors.push_back(QStringLiteral("section has no items: %1/%2")
                                     .arg(pageDefinition.id, sectionDefinition.id));
            }
            for (const SettingsItemDefinition& itemDefinition : sectionDefinition.items) {
                addUnique(&errors, &itemIds, itemDefinition.id, QStringLiteral("item id"));
                addUnique(&errors, &searchIds, QStringLiteral("item:%1").arg(itemDefinition.id),
                          QStringLiteral("generated search id"));
                addUnique(&errors, &objectNames,
                          generatedObjectName(QStringLiteral("settings-item"), itemDefinition.id),
                          QStringLiteral("generated object name"));
                if (!itemDefinition.title.isValid() || !itemDefinition.description.isValid()) {
                    errors.push_back(
                        QStringLiteral("item text is incomplete: %1").arg(itemDefinition.id));
                }
                for (const TranslatableText& alias : itemDefinition.aliases) {
                    if (!alias.isValid()) {
                        errors.push_back(QStringLiteral("item alias text is incomplete: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                const auto* schemaEntry =
                    itemDefinition.configurationKey.isEmpty()
                        ? nullptr
                        : storage::ConfigurationSchema::entry(itemDefinition.configurationKey);
                if (!itemDefinition.configurationKey.isEmpty() && schemaEntry == nullptr) {
                    errors.push_back(QStringLiteral("unknown configuration key for %1: %2")
                                         .arg(itemDefinition.id, itemDefinition.configurationKey));
                }
                if (const auto* select =
                        std::get_if<SettingsSelectDefinition>(&itemDefinition.payload);
                    select != nullptr) {
                    QString expectedKey;
                    SettingsSelectSource expectedSource = SettingsSelectSource::Fixed;
                    switch (select->binding) {
                    case SettingsSelectBinding::Theme:
                        expectedKey = QStringLiteral("interface/theme_mode");
                        break;
                    case SettingsSelectBinding::Language:
                        expectedKey = QStringLiteral("interface/language");
                        expectedSource = SettingsSelectSource::LanguageCatalog;
                        break;
                    case SettingsSelectBinding::ApplicationPriority:
                        expectedKey = QStringLiteral("system/application_priority");
                        break;
                    case SettingsSelectBinding::Proxy:
                        expectedKey = QStringLiteral("network/proxy");
                        break;
                    case SettingsSelectBinding::ScreenshotToolbarSize:
                        expectedKey = QStringLiteral("screenshot_ui/toolbar_size");
                        break;
                    case SettingsSelectBinding::ColorPickerDisplayMode:
                        expectedKey = QStringLiteral("screenshot_ui/color_picker_display_mode");
                        break;
                    case SettingsSelectBinding::ScreenshotOcrAction:
                        expectedKey =
                            QStringLiteral("screenshot/auto_execute_after_text_recognition");
                        break;
                    case SettingsSelectBinding::ScreenshotDoubleClickAction:
                        expectedKey = QStringLiteral("screenshot/double_click_action");
                        break;
                    case SettingsSelectBinding::ScreenshotMiddleClickAction:
                        expectedKey = QStringLiteral("screenshot/middle_mouse_button_action");
                        break;
                    case SettingsSelectBinding::PinMouseWheelZoomMode:
                        expectedKey = QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode");
                        break;
                    case SettingsSelectBinding::ScreenRecordingClarity:
                        expectedKey = QStringLiteral("screen_recording/clarity");
                        break;
                    case SettingsSelectBinding::ScreenRecordingFrameRate:
                        expectedKey = QStringLiteral("screen_recording/frame_rate");
                        break;
                    case SettingsSelectBinding::AnimatedImageClarity:
                        expectedKey =
                            QStringLiteral("screen_recording/animated_image_clarity");
                        break;
                    case SettingsSelectBinding::AnimatedImageFrameRate:
                        expectedKey =
                            QStringLiteral("screen_recording/animated_image_frame_rate");
                        break;
                    case SettingsSelectBinding::AnimatedImageFormat:
                        expectedKey = QStringLiteral("screen_recording/animated_image_format");
                        break;
                    case SettingsSelectBinding::ScreenRecordingEncoder:
                        expectedKey = QStringLiteral("screen_recording/encoder");
                        break;
                    case SettingsSelectBinding::ScreenRecordingEncodingPreset:
                        expectedKey = QStringLiteral("screen_recording/encoding_preset");
                        break;
                    case SettingsSelectBinding::ScreenshotImageFormat:
                        expectedKey = QStringLiteral("screenshot/image_format");
                        break;
                    case SettingsSelectBinding::TrayLeftClickAction:
                        expectedKey = QStringLiteral("tray/left_click_action");
                        break;
                    }
                    if (schemaEntry == nullptr ||
                        (schemaEntry->valueKind != storage::ConfigurationValueKind::String &&
                         schemaEntry->valueKind != storage::ConfigurationValueKind::Integer) ||
                        select->options.isEmpty()) {
                        errors.push_back(
                            QStringLiteral("select item is incomplete: %1").arg(itemDefinition.id));
                    }
                    if (itemDefinition.configurationKey != expectedKey ||
                        select->source != expectedSource) {
                        errors.push_back(QStringLiteral("select binding is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                    QSet<QString> configuredValues;
                    for (const SettingsOptionDefinition& option : select->options) {
                        configuredValues.insert(option.value.toString());
                        if (!option.label.isValid()) {
                            errors.push_back(QStringLiteral("select option text is incomplete: %1")
                                                 .arg(itemDefinition.id));
                        }
                        if (schemaEntry != nullptr &&
                            !storage::ConfigurationSchema::normalize(
                                 itemDefinition.configurationKey,
                                 QJsonValue::fromVariant(option.value))
                                 .valid) {
                            errors.push_back(QStringLiteral("select option is invalid: %1")
                                                 .arg(itemDefinition.id));
                        }
                    }
                    QSet<QString> allowed;
                    if (schemaEntry != nullptr) {
                        for (const QString& allowedValue : schemaEntry->allowedStringValues) {
                            allowed.insert(allowedValue);
                        }
                    }
                    if (!allowed.isEmpty() && configuredValues != allowed) {
                        errors.push_back(QStringLiteral("select options do not match schema: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* switchDefinition =
                        std::get_if<SettingsSwitchDefinition>(&itemDefinition.payload)) {
                    QString expectedKey;
                    switch (switchDefinition->binding) {
                    case SettingsSwitchBinding::HistoryEnabled:
                        expectedKey = QStringLiteral("capture_history/enabled");
                        break;
                    case SettingsSwitchBinding::SmartSelection:
                        expectedKey = QStringLiteral("screenshot_selection/smart_selection");
                        break;
                    case SettingsSwitchBinding::DirectMlAcceleration:
                        expectedKey = QStringLiteral("text_recognition/direct_ml_acceleration");
                        break;
                    case SettingsSwitchBinding::SelectionTransitionAnimation:
                        expectedKey =
                            QStringLiteral("screenshot_ui/selection_transition_animation");
                        break;
                    case SettingsSwitchBinding::TrayEnabled:
                        expectedKey = QStringLiteral("tray/enabled");
                        break;
                    case SettingsSwitchBinding::ScreenshotAutoSaveAfterCopy:
                        expectedKey = QStringLiteral("screenshot/auto_save_after_copy");
                        break;
                    case SettingsSwitchBinding::ScreenshotCopyImageFileToClipboard:
                        expectedKey =
                            QStringLiteral("screenshot/copy_image_file_to_clipboard");
                        break;
                    case SettingsSwitchBinding::PinAutomaticTextRecognition:
                        expectedKey =
                            QStringLiteral("pin_to_screen/automatic_text_recognition");
                        break;
                    case SettingsSwitchBinding::PinAutoResizeWindow:
                        expectedKey = QStringLiteral("pin_to_screen/auto_resize_window");
                        break;
                    case SettingsSwitchBinding::ScreenRecordingHideToolbar:
                        expectedKey =
                            QStringLiteral("screen_recording/hide_toolbar_in_recording");
                        break;
                    case SettingsSwitchBinding::DisableHotkeysOnFocusedFullscreen:
                        expectedKey = QStringLiteral(
                            "global_shortcuts/disable_on_focused_fullscreen_window");
                        break;
                    case SettingsSwitchBinding::AutoStartAtBoot:
                        expectedKey = QStringLiteral("system/auto_start_at_boot");
                        break;
                    }
                    if (itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::Boolean) {
                        errors.push_back(QStringLiteral("switch binding is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* multi =
                        std::get_if<SettingsMultiSelectDefinition>(&itemDefinition.payload)) {
                    const QString expectedKey =
                        multi->binding ==
                                SettingsMultiSelectBinding::DrawingQuickSelectionDisabledTools
                            ? QStringLiteral("drawing/quick_selection_disabled_tools")
                            : QString();
                    QSet<QString> configuredValues;
                    for (const SettingsOptionDefinition& option : multi->options) {
                        configuredValues.insert(option.value.toString());
                        if (!option.label.isValid()) {
                            errors.push_back(QStringLiteral("multi-select option text is incomplete: %1")
                                                 .arg(itemDefinition.id));
                        }
                    }
                    QSet<QString> allowed;
                    if (schemaEntry != nullptr) {
                        allowed = QSet<QString>(schemaEntry->allowedStringValues.cbegin(),
                                                schemaEntry->allowedStringValues.cend());
                    }
                    if (itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::StringList ||
                        multi->options.isEmpty() || configuredValues != allowed) {
                        errors.push_back(QStringLiteral("multi-select binding is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* integer =
                        std::get_if<SettingsIntegerDefinition>(&itemDefinition.payload)) {
                    QString expectedKey;
                    switch (integer->binding) {
                    case SettingsIntegerBinding::HistoryRetentionDays:
                        expectedKey = QStringLiteral("capture_history/retention_days");
                        break;
                    case SettingsIntegerBinding::HistoryMaxEntries:
                        expectedKey = QStringLiteral("capture_history/max_entries");
                        break;
                    case SettingsIntegerBinding::HistoryMaxDiskMiB:
                        expectedKey = QStringLiteral("capture_history/max_disk_mib");
                        break;
                    case SettingsIntegerBinding::ScreenshotDelaySeconds:
                        expectedKey = QStringLiteral("screenshot/delay_seconds");
                        break;
                    }
                    if (itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::Integer ||
                        !schemaEntry->integerRange.has_value()) {
                        errors.push_back(QStringLiteral("integer binding is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* shortcut =
                        std::get_if<SettingsShortcutActionDefinition>(&itemDefinition.payload)) {
                    const QString expectedKey = shortcutConfigurationKey(shortcut->shortcutAction);
                    if (schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::StringList ||
                        schemaEntry->maximumListItems != 2 || !shortcut->iconFactory ||
                        itemDefinition.configurationKey != expectedKey) {
                        errors.push_back(QStringLiteral("shortcut item is incomplete: %1")
                                             .arg(itemDefinition.id));
                    }
                    if (shortcutActions.contains(shortcut->shortcutAction)) {
                        errors.push_back(
                            QStringLiteral("duplicate shortcut action: %1").arg(itemDefinition.id));
                    } else {
                        shortcutActions.insert(shortcut->shortcutAction);
                    }
                    if (shortcut->command.kind == SettingsCommandKind::Navigate) {
                        if (shortcut->command.location.isEmpty() ||
                            resolveLocation(shortcut->command.location) !=
                                shortcut->command.location) {
                            errors.push_back(QStringLiteral("shortcut navigation is invalid: %1")
                                                 .arg(itemDefinition.id));
                        }
                    } else if (shortcut->command.location != SettingsLocation{}) {
                        errors.push_back(
                            QStringLiteral("shortcut command location is unexpected: %1")
                                .arg(itemDefinition.id));
                    }
                    const SettingsCommandKind expectedCommand =
                        shortcut->shortcutAction == GlobalShortcutAction::Screenshot
                            ? SettingsCommandKind::CaptureScreenshot
                        : shortcut->shortcutAction == GlobalShortcutAction::OpenSettings
                            ? SettingsCommandKind::Navigate
                            : SettingsCommandKind::ExecuteQuickAction;
                    if (shortcut->command.kind != expectedCommand) {
                        errors.push_back(QStringLiteral("shortcut command is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                    if (shortcut->command.kind == SettingsCommandKind::ExecuteQuickAction &&
                        shortcut->command.shortcutAction != shortcut->shortcutAction) {
                        errors.push_back(QStringLiteral("quick action command is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                    const bool isDelayAction =
                        shortcut->shortcutAction == GlobalShortcutAction::ScreenshotDelay;
                    if (shortcut->adjustment ==
                        SettingsShortcutAdjustment::ScreenshotDelaySeconds) {
                        const auto* delaySchema = storage::ConfigurationSchema::entry(
                            QStringLiteral("screenshot/delay_seconds"));
                        if (!isDelayAction || delaySchema == nullptr ||
                            delaySchema->valueKind != storage::ConfigurationValueKind::Integer ||
                            !delaySchema->integerRange.has_value() ||
                            delaySchema->integerRange->minimum != 1 ||
                            delaySchema->integerRange->maximum != 10) {
                            errors.push_back(
                                QStringLiteral("shortcut adjustment is incompatible: %1")
                                    .arg(itemDefinition.id));
                        }
                    } else if (isDelayAction) {
                        errors.push_back(QStringLiteral("delay shortcut adjustment is missing: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* local =
                        std::get_if<SettingsLocalShortcutDefinition>(&itemDefinition.payload)) {
                    const QString expectedKey =
                        (local->scope == SettingsLocalShortcutScope::Screenshot
                             ? QStringLiteral("screenshot_shortcuts/")
                             : local->scope == SettingsLocalShortcutScope::Drawing
                                   ? QStringLiteral("drawing_shortcuts/")
                                   : QStringLiteral("pin_to_screen_shortcuts/")) +
                        local->shortcutId;
                    if (local->shortcutId.isEmpty() || !local->iconFactory ||
                        itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::StringList ||
                        schemaEntry->maximumListItems != 2) {
                        errors.push_back(QStringLiteral("local shortcut item is incomplete: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* slider =
                        std::get_if<SettingsSliderDefinition>(&itemDefinition.payload)) {
                    QString expectedKey;
                    switch (slider->binding) {
                    case SettingsSliderBinding::ShortcutHintOpacity:
                        expectedKey = QStringLiteral("screenshot_ui/shortcut_hint_opacity");
                        break;
                    }
                    if (itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::Integer ||
                        !schemaEntry->integerRange.has_value() || !slider->suffix.isValid()) {
                        errors.push_back(QStringLiteral("slider binding is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* color =
                        std::get_if<SettingsColorDefinition>(&itemDefinition.payload)) {
                    QString expectedKey;
                    switch (color->binding) {
                    case SettingsColorBinding::SelectionMaskColor:
                        expectedKey = QStringLiteral("screenshot_ui/selection_mask_color");
                        break;
                    case SettingsColorBinding::CursorGuideLineColor:
                        expectedKey = QStringLiteral("screenshot_ui/cursor_guide_line_color");
                        break;
                    case SettingsColorBinding::MonitorCenterGuideLineColor:
                        expectedKey =
                            QStringLiteral("screenshot_ui/monitor_center_guide_line_color");
                        break;
                    case SettingsColorBinding::ColorPickerCenterGuideLineColor:
                        expectedKey =
                            QStringLiteral("screenshot_ui/color_picker_center_guide_line_color");
                        break;
                    case SettingsColorBinding::PinBorderColor:
                        expectedKey = QStringLiteral("pin_to_screen/border_color");
                        break;
                    }
                    if (itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::String ||
                        !color->alphaChannelEnabled) {
                        errors.push_back(QStringLiteral("color binding is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* radio =
                        std::get_if<SettingsRadioDefinition>(&itemDefinition.payload)) {
                    QString expectedKey;
                    switch (radio->binding) {
                    case SettingsRadioBinding::TrayIcon:
                        expectedKey = QStringLiteral("tray/icon");
                        break;
                    }
                    QSet<QString> configuredValues;
                    for (const SettingsRadioOptionDefinition& option : radio->options) {
                        configuredValues.insert(option.value.toString());
                        if (!option.label.isValid() || option.iconResource.isEmpty()) {
                            errors.push_back(QStringLiteral("radio option is incomplete: %1")
                                                 .arg(itemDefinition.id));
                        }
                    }
                    QSet<QString> allowed;
                    if (schemaEntry != nullptr) {
                        for (const QString& value : schemaEntry->allowedStringValues) {
                            allowed.insert(value);
                        }
                    }
                    if (itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::String ||
                        radio->options.isEmpty() || configuredValues != allowed) {
                        errors.push_back(QStringLiteral("radio binding is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* filePath =
                        std::get_if<SettingsFilePathDefinition>(&itemDefinition.payload)) {
                    QString expectedKey;
                    switch (filePath->binding) {
                    case SettingsFilePathBinding::TrayCustomIcon:
                        expectedKey = QStringLiteral("tray/custom_icon");
                        break;
                    }
                    if (itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::String ||
                        !filePath->buttonText.isValid() || !filePath->dialogTitle.isValid() ||
                        !filePath->fileFilter.isValid()) {
                        errors.push_back(QStringLiteral("file path binding is incompatible: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* directoryPath =
                        std::get_if<SettingsDirectoryPathDefinition>(&itemDefinition.payload)) {
                    QString expectedKey;
                    switch (directoryPath->binding) {
                    case SettingsDirectoryPathBinding::ScreenshotImageDirectory:
                        expectedKey = QStringLiteral("screenshot/image_save_directory");
                        break;
                    case SettingsDirectoryPathBinding::ScreenRecordingVideoDirectory:
                        expectedKey = QStringLiteral("screen_recording/video_save_directory");
                        break;
                    }
                    if (itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::String ||
                        !directoryPath->buttonText.isValid() ||
                        !directoryPath->dialogTitle.isValid()) {
                        errors.push_back(
                            QStringLiteral("directory path binding is incompatible: %1")
                                .arg(itemDefinition.id));
                    }
                }
                if (const auto* text =
                        std::get_if<SettingsTextDefinition>(&itemDefinition.payload)) {
                    QString expectedKey;
                    switch (text->binding) {
                    case SettingsTextBinding::ScreenshotManualFilenameFormat:
                        expectedKey = QStringLiteral("screenshot/manual_save_filename_format");
                        break;
                    case SettingsTextBinding::ScreenshotAutoFilenameFormat:
                        expectedKey = QStringLiteral("screenshot/auto_save_filename_format");
                        break;
                    case SettingsTextBinding::ScreenRecordingVideoFilenameFormat:
                        expectedKey = QStringLiteral("screen_recording/video_filename_format");
                        break;
                    }
                    if (itemDefinition.configurationKey != expectedKey || schemaEntry == nullptr ||
                        schemaEntry->valueKind != storage::ConfigurationValueKind::String) {
                        errors.push_back(
                            QStringLiteral("text binding is incompatible: %1")
                                .arg(itemDefinition.id));
                    }
                }
                if (const auto* action =
                        std::get_if<SettingsActionDefinition>(&itemDefinition.payload)) {
                    if (!itemDefinition.configurationKey.isEmpty() ||
                        !action->buttonText.isValid() || !action->iconFactory) {
                        errors.push_back(
                            QStringLiteral("action item is incomplete: %1").arg(itemDefinition.id));
                    }
                    if (action->confirmation.has_value() &&
                        (!action->confirmation->title.isValid() ||
                         !action->confirmation->message.isValid() ||
                         !action->confirmation->acceptText.isValid() ||
                         !action->confirmation->rejectText.isValid())) {
                        errors.push_back(QStringLiteral("action confirmation is incomplete: %1")
                                             .arg(itemDefinition.id));
                    }
                }
                if (const auto* custom =
                        std::get_if<SettingsCustomDefinition>(&itemDefinition.payload)) {
                    bool rendererSupported = false;
                    QString expectedKey;
                    storage::ConfigurationValueKind expectedKind =
                        storage::ConfigurationValueKind::Structured;
                    switch (custom->renderer) {
                    case SettingsCustomRenderer::StorageStatus:
                        rendererSupported = true;
                        break;
                    case SettingsCustomRenderer::DrawingToolbarEditor:
                        rendererSupported = true;
                        expectedKey = QStringLiteral("screenshot_toolbar/layout");
                        expectedKind = storage::ConfigurationValueKind::Structured;
                        break;
                    case SettingsCustomRenderer::TrayMenuOptions:
                        rendererSupported = true;
                        expectedKey = QStringLiteral("tray/menu_options");
                        expectedKind = storage::ConfigurationValueKind::StringList;
                        break;
                    }
                    if (itemDefinition.configurationKey != expectedKey || !rendererSupported ||
                        (custom->renderer != SettingsCustomRenderer::StorageStatus &&
                         (schemaEntry == nullptr || schemaEntry->valueKind != expectedKind))) {
                        errors.push_back(
                            QStringLiteral("custom item is incomplete: %1").arg(itemDefinition.id));
                    }
                }
            }
        }
    }

    QSet<QString> navigatedPages;
    const auto validateNavigationPage = [&](const SettingsNavigationPageDefinition& navPage) {
        addUnique(&errors, &navigationIds, navPage.id, QStringLiteral("navigation id"));
        if (page(navPage.pageId) == nullptr) {
            errors.push_back(
                QStringLiteral("navigation references unknown page: %1").arg(navPage.pageId));
        } else if (navigatedPages.contains(navPage.pageId)) {
            errors.push_back(QStringLiteral("page appears more than once in navigation: %1")
                                 .arg(navPage.pageId));
        } else {
            navigatedPages.insert(navPage.pageId);
        }
        if (!navPage.iconFactory) {
            errors.push_back(
                QStringLiteral("navigation icon factory is missing: %1").arg(navPage.id));
        }
    };
    for (const SettingsNavigationNode& node : m_navigation) {
        if (const auto* navPage = std::get_if<SettingsNavigationPageDefinition>(&node)) {
            validateNavigationPage(*navPage);
        } else if (const auto* group = std::get_if<SettingsNavigationGroupDefinition>(&node)) {
            addUnique(&errors, &navigationIds, group->id, QStringLiteral("navigation id"));
            if (!group->title.isValid() || !group->iconFactory || group->pages.isEmpty()) {
                errors.push_back(
                    QStringLiteral("navigation group is incomplete: %1").arg(group->id));
            }
            for (const SettingsNavigationPageDefinition& groupedPage : group->pages) {
                validateNavigationPage(groupedPage);
            }
        }
    }
    for (const SettingsPageDefinition& pageDefinition : m_pages) {
        if (!navigatedPages.contains(pageDefinition.id)) {
            errors.push_back(
                QStringLiteral("page is absent from navigation: %1").arg(pageDefinition.id));
        }
    }

    const SettingsPageDefinition* defaultPage = page(m_defaultLocation.pageId);
    const SettingsSectionDefinition* defaultSection =
        section(m_defaultLocation.pageId, m_defaultLocation.sectionId);
    const bool defaultItemValid =
        m_defaultLocation.itemId.isEmpty() || item(m_defaultLocation) != nullptr;
    if (m_defaultLocation.isEmpty() || defaultPage == nullptr || defaultSection == nullptr ||
        !defaultItemValid) {
        errors.push_back(
            QStringLiteral("invalid default location: %1").arg(locationText(m_defaultLocation)));
    }
    return errors;
}

SettingsCatalog buildBuiltInSettingsCatalog() {
    return {builtInPages(),
            builtInNavigation(),
            {QString::fromLatin1(QUICK_PAGE_ID), QStringLiteral("screenshot"),
             QStringLiteral("quick.screenshot")}};
}

const SettingsCatalog& builtInSettingsCatalog() {
    static const SettingsCatalog catalog = buildBuiltInSettingsCatalog();
    static const bool validated = []() {
        const QStringList errors = catalog.validationErrors();
        if (!errors.isEmpty()) {
            qFatal("Invalid built-in settings catalog:\n%s", qPrintable(errors.join(u'\n')));
        }
        return true;
    }();
    Q_UNUSED(validated)
    return catalog;
}

QString generatedObjectName(const QString& prefix, const QString& stableId) {
    QString result = stableId.toLower();
    result.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    result.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return QStringLiteral("%1-%2").arg(prefix, result);
}

} // namespace snow_shot::presentation::settings
