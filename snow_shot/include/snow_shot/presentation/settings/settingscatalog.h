#ifndef SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSCATALOG_H
#define SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSCATALOG_H

#include "icon_core.h"
#include "snow_shot/presentation/globalshortcutmanager.h"

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVariant>

#include <functional>
#include <optional>
#include <variant>

namespace snow_shot::presentation::settings {

struct TranslatableText {
    const char* context = nullptr;
    const char* source = nullptr;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QString translated() const;
};

struct SettingsLocation {
    QString pageId;
    QString sectionId;
    QString itemId;

    [[nodiscard]] bool isEmpty() const;
    friend bool operator==(const SettingsLocation& first, const SettingsLocation& second) {
        return first.pageId == second.pageId && first.sectionId == second.sectionId &&
               first.itemId == second.itemId;
    }
    friend bool operator!=(const SettingsLocation& first, const SettingsLocation& second) {
        return !(first == second);
    }
};

enum class SettingsCommandKind {
    CaptureScreenshot,
    ExecuteQuickAction,
    Navigate,
};

struct SettingsCommand {
    SettingsCommandKind kind = SettingsCommandKind::Navigate;
    SettingsLocation location;
    // The action to dispatch when kind is ExecuteQuickAction.  Keeping this on
    // the command lets the settings surface use the same dispatcher as a
    // global shortcut activation without introducing one signal per action.
    GlobalShortcutAction shortcutAction = GlobalShortcutAction::Screenshot;
};

struct SettingsOptionDefinition {
    QVariant value;
    TranslatableText label;
};

enum class SettingsSelectSource {
    Fixed,
    LanguageCatalog,
};

enum class SettingsSelectBinding {
    Theme,
    Language,
    ApplicationPriority,
    Proxy,
    ScreenshotToolbarSize,
    ColorPickerDisplayMode,
    ScreenshotOcrAction,
    ScreenshotDoubleClickAction,
    ScreenshotMiddleClickAction,
    PinMouseWheelZoomMode,
    ScreenRecordingClarity,
    ScreenRecordingFrameRate,
    AnimatedImageClarity,
    AnimatedImageFrameRate,
    AnimatedImageFormat,
    ScreenRecordingEncoder,
    ScreenRecordingEncodingPreset,
    ScreenshotImageFormat,
    TrayLeftClickAction,
};

struct SettingsSelectDefinition {
    SettingsSelectBinding binding = SettingsSelectBinding::Theme;
    SettingsSelectSource source = SettingsSelectSource::Fixed;
    QVector<SettingsOptionDefinition> options;
};

enum class SettingsSwitchBinding {
    HistoryEnabled,
    SmartSelection,
    DirectMlAcceleration,
    SelectionTransitionAnimation,
    TrayEnabled,
    ScreenshotAutoSaveAfterCopy,
    ScreenshotCopyImageFileToClipboard,
    PinAutomaticTextRecognition,
    PinAutoResizeWindow,
    ScreenRecordingHideToolbar,
    DisableHotkeysOnFocusedFullscreen,
    AutoStartAtBoot,
};

struct SettingsSwitchDefinition {
    SettingsSwitchBinding binding = SettingsSwitchBinding::HistoryEnabled;
};

enum class SettingsIntegerBinding {
    HistoryRetentionDays,
    HistoryMaxEntries,
    HistoryMaxDiskMiB,
    ScreenshotDelaySeconds,
};

enum class SettingsMultiSelectBinding {
    DrawingQuickSelectionDisabledTools,
    TrayMenuOptions,
};

struct SettingsMultiSelectDefinition {
    SettingsMultiSelectBinding binding =
        SettingsMultiSelectBinding::DrawingQuickSelectionDisabledTools;
    QVector<SettingsOptionDefinition> options;
};

struct SettingsIntegerDefinition {
    SettingsIntegerBinding binding = SettingsIntegerBinding::HistoryRetentionDays;
    TranslatableText suffix;
};

enum class SettingsSliderBinding {
    ShortcutHintOpacity,
};

struct SettingsSliderDefinition {
    SettingsSliderBinding binding = SettingsSliderBinding::ShortcutHintOpacity;
    TranslatableText suffix;
};

enum class SettingsColorBinding {
    SelectionMaskColor,
    CursorGuideLineColor,
    MonitorCenterGuideLineColor,
    ColorPickerCenterGuideLineColor,
    PinBorderColor,
};

struct SettingsColorDefinition {
    SettingsColorBinding binding = SettingsColorBinding::SelectionMaskColor;
    bool alphaChannelEnabled = true;
};

enum class SettingsRadioBinding {
    TrayIcon,
};

struct SettingsRadioOptionDefinition {
    QVariant value;
    TranslatableText label;
    QString iconResource;
};

struct SettingsRadioDefinition {
    SettingsRadioBinding binding = SettingsRadioBinding::TrayIcon;
    QVector<SettingsRadioOptionDefinition> options;
};

enum class SettingsFilePathBinding {
    TrayCustomIcon,
};

struct SettingsFilePathDefinition {
    SettingsFilePathBinding binding = SettingsFilePathBinding::TrayCustomIcon;
    TranslatableText buttonText;
    TranslatableText dialogTitle;
    TranslatableText fileFilter;
};

enum class SettingsDirectoryPathBinding {
    ScreenshotImageDirectory,
    ScreenRecordingVideoDirectory,
};

struct SettingsDirectoryPathDefinition {
    SettingsDirectoryPathBinding binding =
        SettingsDirectoryPathBinding::ScreenshotImageDirectory;
    TranslatableText buttonText;
    TranslatableText dialogTitle;
};

enum class SettingsTextBinding {
    ScreenshotManualFilenameFormat,
    ScreenshotAutoFilenameFormat,
    ScreenRecordingVideoFilenameFormat,
};

struct SettingsTextDefinition {
    SettingsTextBinding binding = SettingsTextBinding::ScreenshotManualFilenameFormat;
};

enum class SettingsShortcutAdjustment {
    None,
    ScreenshotDelaySeconds,
};

struct SettingsShortcutActionDefinition {
    GlobalShortcutAction shortcutAction = GlobalShortcutAction::Screenshot;
    SettingsCommand command;
    std::function<adqt::icons::IconRef()> iconFactory;
    SettingsShortcutAdjustment adjustment = SettingsShortcutAdjustment::None;
};

enum class SettingsLocalShortcutScope {
    Screenshot,
    Drawing,
    PinToScreen,
};

struct SettingsLocalShortcutDefinition {
    QString shortcutId;
    std::function<adqt::icons::IconRef()> iconFactory;
    SettingsLocalShortcutScope scope = SettingsLocalShortcutScope::Drawing;
};

enum class SettingsActionBinding {
    ClearCaptureHistory,
};

enum class SettingsActionAccent {
    Neutral,
    Danger,
};

struct SettingsConfirmationDefinition {
    TranslatableText title;
    TranslatableText message;
    TranslatableText acceptText;
    TranslatableText rejectText;
};

struct SettingsActionDefinition {
    SettingsActionBinding binding = SettingsActionBinding::ClearCaptureHistory;
    TranslatableText buttonText;
    SettingsActionAccent accent = SettingsActionAccent::Neutral;
    std::function<adqt::icons::IconRef()> iconFactory;
    std::optional<SettingsConfirmationDefinition> confirmation;
};

enum class SettingsCustomRenderer {
    StorageStatus,
    DrawingToolbarEditor,
    TrayMenuOptions,
};

struct SettingsCustomDefinition {
    SettingsCustomRenderer renderer = SettingsCustomRenderer::StorageStatus;
};

enum class SettingsTrayMenuOptionKind {
    QuickAction,
    DisableShortcutFunctions,
    ShowMainWindow,
    Exit,
};

struct SettingsTrayMenuOptionDefinition {
    QString id;
    TranslatableText label;
    SettingsTrayMenuOptionKind kind = SettingsTrayMenuOptionKind::QuickAction;
    GlobalShortcutAction shortcutAction = GlobalShortcutAction::Screenshot;
    std::function<adqt::icons::IconRef()> iconFactory;
};

struct SettingsTrayMenuGroupDefinition {
    QString id;
    QVector<SettingsTrayMenuOptionDefinition> options;
};

using SettingsItemPayload =
    std::variant<SettingsSelectDefinition, SettingsSwitchDefinition, SettingsIntegerDefinition,
                 SettingsMultiSelectDefinition, SettingsSliderDefinition,
                 SettingsColorDefinition, SettingsRadioDefinition, SettingsFilePathDefinition,
                 SettingsDirectoryPathDefinition, SettingsTextDefinition,
                 SettingsShortcutActionDefinition, SettingsLocalShortcutDefinition,
                 SettingsActionDefinition, SettingsCustomDefinition>;

struct SettingsItemDefinition {
    QString id;
    TranslatableText title;
    TranslatableText description;
    QVector<TranslatableText> aliases;
    QString configurationKey;
    SettingsItemPayload payload;
};

enum class SettingsSectionReset {
    None,
    ScreenshotShortcuts,
    OtherShortcuts,
    GeneralSettings,
    HistoryPolicy,
    ScreenshotSettings,
    ScreenshotOutput,
    ScreenshotInterfaceSettings,
    Toolbar,
    DrawingToolbar,
    DrawingQuickSelection,
    ScreenshotEditorShortcuts,
    ScreenshotOtherShortcuts,
    DrawingShortcuts,
    PinToScreenShortcuts,
    PinToScreen,
    PinToScreenBehavior,
    Tray,
    TrayBehavior,
    ScreenRecording,
    ScreenRecordingOutput,
    GlobalHotkeys,
    SystemGeneral,
    Network,
    SystemSettings,
    TextRecognition,
};

enum class SettingsSectionItemLayout {
    VerticalList,
    TwoColumnGrid,
};

struct SettingsSectionDefinition {
    QString id;
    TranslatableText title;
    TranslatableText searchDescription;
    SettingsSectionReset reset = SettingsSectionReset::None;
    QVector<SettingsItemDefinition> items;
    SettingsSectionItemLayout itemLayout = SettingsSectionItemLayout::VerticalList;
};

enum class SettingsPageKind {
    GeneratedSettings,
    ScreenshotHistory,
};

struct SettingsPageDefinition {
    QString id;
    QString route;
    TranslatableText title;
    TranslatableText description;
    QVector<SettingsSectionDefinition> sections;
    SettingsPageKind kind = SettingsPageKind::GeneratedSettings;
};

struct SettingsNavigationPageDefinition {
    QString id;
    QString pageId;
    std::function<adqt::icons::IconRef()> iconFactory;
};

struct SettingsNavigationGroupDefinition {
    QString id;
    TranslatableText title;
    std::function<adqt::icons::IconRef()> iconFactory;
    QVector<SettingsNavigationPageDefinition> pages;
};

using SettingsNavigationNode =
    std::variant<SettingsNavigationPageDefinition, SettingsNavigationGroupDefinition>;

struct SettingsSectionSummary {
    QString id;
    QString label;
};

class SettingsCatalog final {
  public:
    SettingsCatalog(QVector<SettingsPageDefinition> pages,
                    QVector<SettingsNavigationNode> navigation,
                    SettingsLocation defaultLocation);

    [[nodiscard]] const QVector<SettingsPageDefinition>& pages() const;
    [[nodiscard]] const QVector<SettingsNavigationNode>& navigation() const;
    [[nodiscard]] const SettingsLocation& defaultLocation() const;
    [[nodiscard]] const SettingsPageDefinition* page(const QString& pageId) const;
    [[nodiscard]] const SettingsPageDefinition* pageForRoute(const QString& route) const;
    [[nodiscard]] const SettingsSectionDefinition* section(const QString& pageId,
                                                           const QString& sectionId) const;
    [[nodiscard]] const SettingsItemDefinition* item(const SettingsLocation& location) const;
    [[nodiscard]] const SettingsItemDefinition*
    itemForShortcut(GlobalShortcutAction action) const;
    [[nodiscard]] QString shortcutActionTitle(GlobalShortcutAction action,
                                               int screenshotDelaySeconds = 3) const;
    [[nodiscard]] std::optional<SettingsCommand>
    commandForShortcut(GlobalShortcutAction action) const;
    [[nodiscard]] QVector<SettingsTrayMenuGroupDefinition> trayMenuGroups() const;
    [[nodiscard]] SettingsLocation resolveLocation(const SettingsLocation& requested) const;
    [[nodiscard]] QVector<SettingsSectionSummary> sectionSummaries(const QString& pageId) const;
    [[nodiscard]] QStringList validationErrors() const;

  private:
    QVector<SettingsPageDefinition> m_pages;
    QVector<SettingsNavigationNode> m_navigation;
    SettingsLocation m_defaultLocation;
};

[[nodiscard]] SettingsCatalog buildBuiltInSettingsCatalog();
[[nodiscard]] const SettingsCatalog& builtInSettingsCatalog();
[[nodiscard]] QString generatedObjectName(const QString& prefix, const QString& stableId);

} // namespace snow_shot::presentation::settings

Q_DECLARE_METATYPE(snow_shot::presentation::settings::SettingsLocation)
Q_DECLARE_METATYPE(snow_shot::presentation::settings::SettingsCommand)

#endif // SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSCATALOG_H
