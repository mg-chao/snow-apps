#include "snow_shot/presentation/components/applicationsearchwidget.h"
#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/drawingtoolbareditorsettingswidget.h"
#include "snow_shot/presentation/components/infotooltipicon.h"
#include "snow_shot/presentation/components/maincontentheaderwidget.h"
#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/components/sectionheaderwidget.h"
#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/components/shortcutkeyrow.h"
#include "snow_shot/presentation/components/sidebarwidget.h"
#include "snow_shot/presentation/components/storagestatussettingswidget.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"

#include "antd_icons.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/carousel.h"
#include "widgets/date_picker.h"
#include "widgets/descriptions.h"
#include "widgets/divider.h"
#include "widgets/image.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_number.h"
#include "widgets/input_search_edit.h"
#include "widgets/modal.h"
#include "widgets/multi_select.h"
#include "widgets/navigation_menu.h"
#include "widgets/pagination.h"
#include "widgets/popconfirm.h"
#include "widgets/radio.h"
#include "widgets/scroll_area.h"
#include "widgets/select.h"
#include "widgets/switch.h"
#include "widgets/tabs.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QImage>
#include <QMimeData>
#include <QScrollBar>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTranslator>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QPointer>
#include <QUuid>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace settings = snow_shot::presentation::settings;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

const QHash<QString, QStringList>& screenshotShortcutDefaults() {
    static const QHash<QString, QStringList> defaults{
        {QStringLiteral("move_tool"), {QStringLiteral("M")}},
        {QStringLiteral("move_cursor_up"), {QStringLiteral("W"), QStringLiteral("Up")}},
        {QStringLiteral("move_cursor_down"), {QStringLiteral("S"), QStringLiteral("Down")}},
        {QStringLiteral("move_cursor_left"), {QStringLiteral("A"), QStringLiteral("Left")}},
        {QStringLiteral("move_cursor_right"), {QStringLiteral("D"), QStringLiteral("Right")}},
        {QStringLiteral("move_entire_selection"), {QStringLiteral("Space")}},
        {QStringLiteral("keep_selection_width_and_height_consistent"),
         {QStringLiteral("Shift")}},
        {QStringLiteral("switch_selection_between_window_and_window_sub_element"),
         {QStringLiteral("Tab")}},
        {QStringLiteral("previous_screenshot_history"), {QStringLiteral(",")}},
        {QStringLiteral("next_screenshot_history"), {QStringLiteral(".")}},
        {QStringLiteral("select_previously_selected_area"), {QStringLiteral("R")}},
        {QStringLiteral("copy_color"), {QStringLiteral("C")}},
        {QStringLiteral("table_recognition"), {QStringLiteral("Ctrl+X")}},
        {QStringLiteral("qr_code_recognition"), {QStringLiteral("Ctrl+Q")}},
        {QStringLiteral("video_recording"), {QStringLiteral("Ctrl+R")}},
        {QStringLiteral("text_recognition"), {QStringLiteral("Ctrl+D")}},
        {QStringLiteral("text_translation"), {QStringLiteral("Ctrl+T")}},
        {QStringLiteral("scrolling_screenshot"), {QStringLiteral("L")}},
        {QStringLiteral("save_as_file"), {QStringLiteral("Ctrl+S")}},
        {QStringLiteral("pin_to_screen"), {QStringLiteral("Ctrl+F")}},
        {QStringLiteral("cancel_screenshot"), {QStringLiteral("Esc")}},
        {QStringLiteral("copy_to_clipboard"), {QStringLiteral("Ctrl+C")}},
        {QStringLiteral("undo"), {QStringLiteral("Ctrl+Z")}},
        {QStringLiteral("redo"), {QStringLiteral("Ctrl+Y")}},
    };
    return defaults;
}

const QStringList& screenshotCategoryShortcutIds() {
    static const QStringList ids{
        QStringLiteral("move_tool"),
        QStringLiteral("move_cursor_up"),
        QStringLiteral("move_cursor_down"),
        QStringLiteral("move_cursor_left"),
        QStringLiteral("move_cursor_right"),
        QStringLiteral("move_entire_selection"),
        QStringLiteral("keep_selection_width_and_height_consistent"),
        QStringLiteral("switch_selection_between_window_and_window_sub_element"),
        QStringLiteral("previous_screenshot_history"),
        QStringLiteral("next_screenshot_history"),
        QStringLiteral("select_previously_selected_area"),
        QStringLiteral("copy_color"),
        QStringLiteral("pin_to_screen"),
        QStringLiteral("video_recording"),
        QStringLiteral("scrolling_screenshot"),
        QStringLiteral("save_as_file"),
        QStringLiteral("cancel_screenshot"),
        QStringLiteral("copy_to_clipboard"),
    };
    return ids;
}

const QStringList& otherShortcutIds() {
    static const QStringList ids{
        QStringLiteral("table_recognition"), QStringLiteral("qr_code_recognition"),
        QStringLiteral("text_recognition"),  QStringLiteral("text_translation"),
        QStringLiteral("undo"),              QStringLiteral("redo"),
    };
    return ids;
}

const QHash<QString, QStringList>& pinToScreenShortcutDefaults() {
    static const QHash<QString, QStringList> defaults{
        {QStringLiteral("copy_to_clipboard"), {QStringLiteral("Ctrl+C")}},
        {QStringLiteral("copy_original_content"), {QStringLiteral("Ctrl+Shift+C")}},
        {QStringLiteral("save_as_file"), {QStringLiteral("Ctrl+S")}},
        {QStringLiteral("show_text_recognition_results"), {QStringLiteral("Ctrl+D")}},
        {QStringLiteral("drawing_mode"), {QStringLiteral("Ctrl+E")}},
        {QStringLiteral("thumbnail_mode"), {QStringLiteral("R")}},
        {QStringLiteral("close_window"), {QStringLiteral("Esc")}},
        {QStringLiteral("move_cursor_up"), {QStringLiteral("W"), QStringLiteral("Up")}},
        {QStringLiteral("move_cursor_down"), {QStringLiteral("S"), QStringLiteral("Down")}},
        {QStringLiteral("move_cursor_left"), {QStringLiteral("A"), QStringLiteral("Left")}},
        {QStringLiteral("move_cursor_right"), {QStringLiteral("D"), QStringLiteral("Right")}},
    };
    return defaults;
}

class FakeSettingsBackend final : public settings::SettingsBackend {
  public:
    FakeSettingsBackend() {
        m_storageStatus.writeAvailable = true;
        m_storageStatus.effectiveMode = snow_shot::storage::StorageMode::ApplicationData;
        m_storageStatus.effectiveDirectory = QStringLiteral("C:/settings-test-storage");
        m_storageStatus.historyUsage.entryCount = 2;
        m_storageStatus.historyUsage.totalBytes = 2048;
        m_shortcutStates.insert(snow_shot::presentation::GlobalShortcutAction::Screenshot,
                                {snow_shot::presentation::GlobalShortcutAction::Screenshot,
                                 {QStringLiteral("Ctrl+Shift+S")},
                                 snow_shot::presentation::GlobalShortcutStatus::Registered,
                                 {}});
        m_shortcutStates.insert(snow_shot::presentation::GlobalShortcutAction::OpenSettings,
                                {snow_shot::presentation::GlobalShortcutAction::OpenSettings,
                                 {},
                                 snow_shot::presentation::GlobalShortcutStatus::Unset,
                                 {}});
        m_selectValues = {
            {settings::SettingsSelectBinding::ScreenshotOcrAction,
             QStringLiteral("no_action")},
            {settings::SettingsSelectBinding::ScreenshotDoubleClickAction,
             QStringLiteral("copy")},
            {settings::SettingsSelectBinding::ScreenshotMiddleClickAction,
             QStringLiteral("pin")},
            {settings::SettingsSelectBinding::PinMouseWheelZoomMode,
             QStringLiteral("mouse_position")},
            {settings::SettingsSelectBinding::ScreenRecordingClarity, QStringLiteral("1080p")},
            {settings::SettingsSelectBinding::ScreenRecordingFrameRate, 30},
            {settings::SettingsSelectBinding::AnimatedImageClarity,
             QStringLiteral("1080p")},
            {settings::SettingsSelectBinding::AnimatedImageFrameRate, 10},
            {settings::SettingsSelectBinding::AnimatedImageFormat, QStringLiteral("gif")},
            {settings::SettingsSelectBinding::ScreenRecordingEncoder, QStringLiteral("h264")},
            {settings::SettingsSelectBinding::ScreenRecordingEncodingPreset,
             QStringLiteral("veryfast")},
            {settings::SettingsSelectBinding::ScreenshotImageFormat, QStringLiteral("png")},
            {settings::SettingsSelectBinding::TrayLeftClickAction,
             QStringLiteral("screenshot")},
            {settings::SettingsSelectBinding::Proxy, QStringLiteral("none")},
        };
        m_switchValues = {
            {settings::SettingsSwitchBinding::ScreenshotAutoSaveAfterCopy, false},
            {settings::SettingsSwitchBinding::ScreenshotCopyImageFileToClipboard, false},
            {settings::SettingsSwitchBinding::PinAutomaticTextRecognition, true},
            {settings::SettingsSwitchBinding::PinAutoResizeWindow, true},
            {settings::SettingsSwitchBinding::ScreenRecordingHideToolbar, true},
            {settings::SettingsSwitchBinding::DisableHotkeysOnFocusedFullscreen, false},
            {settings::SettingsSwitchBinding::AutoStartAtBoot, false},
        };
        m_multiSelectValues.insert(
            settings::SettingsMultiSelectBinding::DrawingQuickSelectionDisabledTools,
            {QStringLiteral("free-draw"), QStringLiteral("pen-filter")});
        m_multiSelectValues.insert(
            settings::SettingsMultiSelectBinding::TrayMenuOptions,
            {QStringLiteral("quick.screenshot"),
             QStringLiteral("quick.screenshot-delay"),
             QStringLiteral("quick.screenshot-fixed"),
             QStringLiteral("quick.screenshot-ocr"),
             QStringLiteral("quick.screenshot-copy"),
             QStringLiteral("quick.screen-record"),
             QStringLiteral("quick.pin-clipboard-content"),
             QStringLiteral("tray.disable-shortcut-functions"),
             QStringLiteral("tray.show-main-window"),
             QStringLiteral("tray.exit")});
        m_directoryPaths = {
            {settings::SettingsDirectoryPathBinding::ScreenshotImageDirectory,
             QStringLiteral("C:/Pictures/SnowShot")},
            {settings::SettingsDirectoryPathBinding::ScreenRecordingVideoDirectory,
             QStringLiteral("C:/Videos/SnowShot")},
        };
        m_textValues = {
            {settings::SettingsTextBinding::ScreenshotManualFilenameFormat,
             QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}")},
            {settings::SettingsTextBinding::ScreenshotAutoFilenameFormat,
             QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}")},
            {settings::SettingsTextBinding::ScreenRecordingVideoFilenameFormat,
             QStringLiteral("SnowShot_Video_{YYYY-MM-DD_HH-mm-ss}")},
        };
        m_localShortcuts = {
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("select")),
             {QStringLiteral("V")}},
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("shape")),
             {QStringLiteral("1")}},
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("arrow")),
             {QStringLiteral("2")}},
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("brush")),
             {QStringLiteral("3"), QStringLiteral("P")}},
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("highlight")),
             {QStringLiteral("4"), QStringLiteral("H")}},
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("text")),
             {QStringLiteral("5"), QStringLiteral("T")}},
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("serial_number")),
             {QStringLiteral("6"), QStringLiteral("N")}},
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("filter")),
             {QStringLiteral("7"), QStringLiteral("F")}},
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("eraser")),
             {QStringLiteral("8"), QStringLiteral("E")}},
            {localShortcutKey(settings::SettingsLocalShortcutScope::Drawing,
                              QStringLiteral("watermark")),
             {QStringLiteral("9")}},
        };
        for (auto it = screenshotShortcutDefaults().cbegin();
             it != screenshotShortcutDefaults().cend(); ++it) {
            m_localShortcuts.insert(
                localShortcutKey(settings::SettingsLocalShortcutScope::Screenshot, it.key()),
                it.value());
        }
        for (auto it = pinToScreenShortcutDefaults().cbegin();
             it != pinToScreenShortcutDefaults().cend(); ++it) {
            m_localShortcuts.insert(
                localShortcutKey(settings::SettingsLocalShortcutScope::PinToScreen, it.key()),
                it.value());
        }
    }

    QVariant selectValue(settings::SettingsSelectBinding binding) const override {
        switch (binding) {
        case settings::SettingsSelectBinding::Theme:
            return m_theme;
        case settings::SettingsSelectBinding::Language:
            return m_language;
        case settings::SettingsSelectBinding::ApplicationPriority:
            return m_applicationPriority;
        case settings::SettingsSelectBinding::ScreenshotToolbarSize:
            return m_toolbarSize;
        case settings::SettingsSelectBinding::ColorPickerDisplayMode:
            return m_colorPickerDisplayMode;
        default:
            return m_selectValues.value(binding);
        }
        return {};
    }

    QVector<settings::SettingsRuntimeOption>
    dynamicSelectOptions(settings::SettingsSelectBinding binding) const override {
        if (binding != settings::SettingsSelectBinding::Language) {
            return {};
        }
        return {{QStringLiteral("en_US"), QStringLiteral("English")}};
    }

    bool applySelectValue(settings::SettingsSelectBinding binding, const QVariant& value) override {
        if (!acceptWrites) {
            return false;
        }
        if (binding == settings::SettingsSelectBinding::Theme) {
            m_theme = value.toString();
        } else if (binding == settings::SettingsSelectBinding::Language) {
            m_language = value.toString();
        } else if (binding == settings::SettingsSelectBinding::ApplicationPriority) {
            m_applicationPriority = value.toString();
        } else if (binding == settings::SettingsSelectBinding::ScreenshotToolbarSize) {
            m_toolbarSize = value.toString();
        } else if (binding == settings::SettingsSelectBinding::ColorPickerDisplayMode) {
            m_colorPickerDisplayMode = value.toString();
        } else {
            m_selectValues.insert(binding, value);
        }
        emit synchronized();
        return true;
    }

    bool switchValue(settings::SettingsSwitchBinding binding) const override {
        switch (binding) {
        case settings::SettingsSwitchBinding::HistoryEnabled:
            return m_historyEnabled;
        case settings::SettingsSwitchBinding::SmartSelection:
            return m_smartSelection;
        case settings::SettingsSwitchBinding::DirectMlAcceleration:
            return m_directMlAcceleration;
        case settings::SettingsSwitchBinding::SelectionTransitionAnimation:
            return m_selectionTransitionAnimation;
        case settings::SettingsSwitchBinding::TrayEnabled:
            return m_trayEnabled;
        default:
            return m_switchValues.value(binding, false);
        }
    }

    bool switchEnabled(settings::SettingsSwitchBinding binding) const override {
        return binding != settings::SettingsSwitchBinding::DirectMlAcceleration ||
               directMlSupported;
    }

    bool applySwitchValue(settings::SettingsSwitchBinding binding, bool value) override {
        if (!acceptWrites) {
            return false;
        }
        ++switchApplyCount;
        if (binding == settings::SettingsSwitchBinding::SmartSelection) {
            m_smartSelection = value;
        } else if (binding == settings::SettingsSwitchBinding::DirectMlAcceleration) {
            m_directMlAcceleration = value;
        } else if (binding == settings::SettingsSwitchBinding::SelectionTransitionAnimation) {
            m_selectionTransitionAnimation = value;
        } else if (binding == settings::SettingsSwitchBinding::TrayEnabled) {
            m_trayEnabled = value;
        } else if (binding == settings::SettingsSwitchBinding::HistoryEnabled) {
            m_historyEnabled = value;
        } else {
            m_switchValues.insert(binding, value);
        }
        emit synchronized();
        return true;
    }

    QVariantList multiSelectValue(settings::SettingsMultiSelectBinding binding) const override {
        return m_multiSelectValues.value(binding);
    }

    bool applyMultiSelectValue(settings::SettingsMultiSelectBinding binding,
                               const QVariantList& value) override {
        if (!acceptWrites) {
            return false;
        }
        m_multiSelectValues.insert(binding, value);
        emit synchronized();
        return true;
    }

    int integerValue(settings::SettingsIntegerBinding binding) const override {
        switch (binding) {
        case settings::SettingsIntegerBinding::HistoryRetentionDays:
            return m_retentionDays;
        case settings::SettingsIntegerBinding::HistoryMaxEntries:
            return m_maxEntries;
        case settings::SettingsIntegerBinding::HistoryMaxDiskMiB:
            return m_maxDiskMiB;
        case settings::SettingsIntegerBinding::ScreenshotDelaySeconds:
            return m_screenshotDelaySeconds;
        }
        return 0;
    }

    bool applyIntegerValue(settings::SettingsIntegerBinding binding, int value) override {
        if (!acceptWrites) {
            return false;
        }
        switch (binding) {
        case settings::SettingsIntegerBinding::HistoryRetentionDays:
            m_retentionDays = value;
            break;
        case settings::SettingsIntegerBinding::HistoryMaxEntries:
            m_maxEntries = value;
            break;
        case settings::SettingsIntegerBinding::HistoryMaxDiskMiB:
            m_maxDiskMiB = value;
            break;
        case settings::SettingsIntegerBinding::ScreenshotDelaySeconds:
            m_screenshotDelaySeconds = value;
            break;
        }
        emit synchronized();
        return true;
    }

    int sliderValue(settings::SettingsSliderBinding) const override {
        return m_shortcutHintOpacity;
    }

    bool applySliderValue(settings::SettingsSliderBinding, int value) override {
        if (!acceptWrites) {
            return false;
        }
        m_shortcutHintOpacity = value;
        emit synchronized();
        return true;
    }

    QColor colorValue(settings::SettingsColorBinding binding) const override {
        return m_colors.value(binding, QColor(QStringLiteral("#1677FFFF")));
    }

    bool applyColorValue(settings::SettingsColorBinding binding, const QColor& value) override {
        if (!acceptWrites) {
            return false;
        }
        m_colors.insert(binding, value);
        emit synchronized();
        return true;
    }

    QVariant radioValue(settings::SettingsRadioBinding) const override {
        return m_trayIcon;
    }

    bool applyRadioValue(settings::SettingsRadioBinding, const QVariant& value) override {
        if (!acceptWrites) {
            return false;
        }
        m_trayIcon = value.toString();
        emit synchronized();
        return true;
    }

    QString filePathValue(settings::SettingsFilePathBinding) const override {
        return m_trayCustomIcon;
    }

    bool applyFilePathValue(settings::SettingsFilePathBinding, const QString& value) override {
        if (!acceptWrites) {
            return false;
        }
        m_trayCustomIcon = value;
        emit synchronized();
        return true;
    }

    QString directoryPathValue(settings::SettingsDirectoryPathBinding binding) const override {
        return m_directoryPaths.value(binding);
    }

    bool applyDirectoryPathValue(settings::SettingsDirectoryPathBinding binding,
                                 const QString& value) override {
        if (!acceptWrites) {
            return false;
        }
        m_directoryPaths.insert(binding, value);
        emit synchronized();
        return true;
    }

    QString textValue(settings::SettingsTextBinding binding) const override {
        return m_textValues.value(binding);
    }

    bool applyTextValue(settings::SettingsTextBinding binding, const QString& value) override {
        if (!acceptWrites) {
            return false;
        }
        m_textValues.insert(binding, value);
        emit synchronized();
        return true;
    }

    snow_shot::storage::ScreenshotToolbarLayout toolbarLayout() const override {
        return m_toolbarLayout;
    }

    bool applyToolbarLayout(const snow_shot::storage::ScreenshotToolbarLayout& layout) override {
        if (!acceptWrites) {
            return false;
        }
        m_toolbarLayout = snow_shot::presentation::toolbar_layout::normalizedLayout(layout);
        ++toolbarLayoutApplyCount;
        emit synchronized();
        return true;
    }

    snow_shot::presentation::GlobalShortcutRegistrationState
    shortcutState(snow_shot::presentation::GlobalShortcutAction action) const override {
        return m_shortcutStates.value(action);
    }

    snow_shot::presentation::GlobalShortcutValidationResult
    validateShortcut(const QString& shortcut) const override {
        return {shortcut, true, snow_shot::presentation::GlobalShortcutFailureReason::None};
    }

    bool applyShortcuts(snow_shot::presentation::GlobalShortcutAction action,
                        const QStringList& shortcuts) override {
        if (!acceptWrites) {
            return false;
        }
        auto state = m_shortcutStates.value(action);
        state.shortcuts = shortcuts;
        m_shortcutStates.insert(action, state);
        emit shortcutStateChanged(action, state);
        return true;
    }

    QStringList localShortcuts(settings::SettingsLocalShortcutScope scope,
                               const QString& shortcutId) const override {
        return m_localShortcuts.value(localShortcutKey(scope, shortcutId));
    }

    snow_shot::presentation::GlobalShortcutValidationResult
    validateLocalShortcut(settings::SettingsLocalShortcutScope, const QString&,
                          const QString& shortcut) const override {
        return {shortcut, true, snow_shot::presentation::GlobalShortcutFailureReason::None};
    }

    bool applyLocalShortcuts(settings::SettingsLocalShortcutScope scope, const QString& shortcutId,
                             const QStringList& shortcuts) override {
        if (!acceptWrites) {
            return false;
        }
        m_localShortcuts.insert(localShortcutKey(scope, shortcutId), shortcuts);
        emit synchronized();
        return true;
    }

    settings::SettingsActionState actionState(settings::SettingsActionBinding) const override {
        return {m_storageStatus.writeAvailable && !m_storageStatus.historyClearing &&
                    m_storageStatus.historyUsage.entryCount > 0,
                m_storageStatus.historyClearing};
    }

    bool triggerAction(settings::SettingsActionBinding binding) override {
        triggeredAction = binding;
        actionTriggered = acceptWrites;
        return acceptWrites;
    }

    snow_shot::storage::StorageStatus storageStatus() const override {
        return m_storageStatus;
    }

    bool resetSection(settings::SettingsSectionReset reset) override {
        resetRequested = reset;
        if (!acceptWrites) {
            return false;
        }
        if (reset == settings::SettingsSectionReset::HistoryPolicy) {
            m_historyEnabled = true;
            m_retentionDays = 7;
            m_maxEntries = 100;
            m_maxDiskMiB = 1024;
        } else if (reset == settings::SettingsSectionReset::ScreenshotSettings) {
            m_smartSelection = true;
        } else if (reset == settings::SettingsSectionReset::ScreenshotEditorShortcuts) {
            for (const QString& actionId : screenshotCategoryShortcutIds()) {
                m_localShortcuts.insert(
                    localShortcutKey(settings::SettingsLocalShortcutScope::Screenshot, actionId),
                    screenshotShortcutDefaults().value(actionId));
            }
        } else if (reset == settings::SettingsSectionReset::ScreenshotOtherShortcuts) {
            for (const QString& actionId : otherShortcutIds()) {
                m_localShortcuts.insert(
                    localShortcutKey(settings::SettingsLocalShortcutScope::Screenshot, actionId),
                    screenshotShortcutDefaults().value(actionId));
            }
        } else if (reset == settings::SettingsSectionReset::PinToScreenShortcuts) {
            for (auto it = pinToScreenShortcutDefaults().cbegin();
                 it != pinToScreenShortcutDefaults().cend(); ++it) {
                m_localShortcuts.insert(
                    localShortcutKey(settings::SettingsLocalShortcutScope::PinToScreen, it.key()),
                    it.value());
            }
        } else if (reset == settings::SettingsSectionReset::ScreenshotOutput) {
            m_directoryPaths.insert(
                settings::SettingsDirectoryPathBinding::ScreenshotImageDirectory,
                QStringLiteral("C:/Pictures/SnowShot"));
            m_selectValues.insert(settings::SettingsSelectBinding::ScreenshotImageFormat,
                                  QStringLiteral("png"));
            m_textValues.insert(
                settings::SettingsTextBinding::ScreenshotManualFilenameFormat,
                QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"));
            m_textValues.insert(
                settings::SettingsTextBinding::ScreenshotAutoFilenameFormat,
                QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"));
        } else if (reset == settings::SettingsSectionReset::ScreenRecordingOutput) {
            m_directoryPaths.insert(
                settings::SettingsDirectoryPathBinding::ScreenRecordingVideoDirectory,
                QStringLiteral("C:/Videos/SnowShot"));
            m_textValues.insert(
                settings::SettingsTextBinding::ScreenRecordingVideoFilenameFormat,
                QStringLiteral("SnowShot_Video_{YYYY-MM-DD_HH-mm-ss}"));
        }
        emit synchronized();
        return true;
    }

    void setStorageState(bool writeAvailable, bool historyClearing) {
        m_storageStatus.writeAvailable = writeAvailable;
        m_storageStatus.historyClearing = historyClearing;
        emit synchronized();
    }

    bool acceptWrites = true;
    bool directMlSupported = true;
    int switchApplyCount = 0;
    int toolbarLayoutApplyCount = 0;
    bool actionTriggered = false;
    settings::SettingsActionBinding triggeredAction =
        settings::SettingsActionBinding::ClearCaptureHistory;
    settings::SettingsSectionReset resetRequested = settings::SettingsSectionReset::None;

  private:
    static QString localShortcutKey(settings::SettingsLocalShortcutScope scope,
                                    const QString& shortcutId) {
        const QString prefix = scope == settings::SettingsLocalShortcutScope::Screenshot
                                   ? QStringLiteral("screenshot:")
                                   : scope == settings::SettingsLocalShortcutScope::Drawing
                                         ? QStringLiteral("drawing:")
                                         : QStringLiteral("pin-to-screen:");
        return prefix + shortcutId;
    }

    QString m_theme = QStringLiteral("system");
    QString m_language = QStringLiteral("en_US");
    QString m_applicationPriority = QStringLiteral("above_normal");
    QString m_toolbarSize = QStringLiteral("normal");
    QString m_colorPickerDisplayMode = QStringLiteral("hex");
    QHash<settings::SettingsSelectBinding, QVariant> m_selectValues;
    QHash<settings::SettingsMultiSelectBinding, QVariantList> m_multiSelectValues;
    QHash<settings::SettingsSwitchBinding, bool> m_switchValues;
    bool m_historyEnabled = true;
    bool m_smartSelection = true;
    bool m_directMlAcceleration = true;
    bool m_selectionTransitionAnimation = true;
    bool m_trayEnabled = true;
    int m_retentionDays = 7;
    int m_maxEntries = 100;
    int m_maxDiskMiB = 1024;
    int m_screenshotDelaySeconds = 3;
    int m_shortcutHintOpacity = 80;
    QHash<settings::SettingsColorBinding, QColor> m_colors;
    QString m_trayIcon = QStringLiteral("default");
    QString m_trayCustomIcon;
    QHash<settings::SettingsDirectoryPathBinding, QString> m_directoryPaths;
    QHash<settings::SettingsTextBinding, QString> m_textValues;
    snow_shot::storage::ScreenshotToolbarLayout m_toolbarLayout{
        snow_shot::presentation::toolbar_layout::defaultPositions()};
    snow_shot::storage::StorageStatus m_storageStatus;
    QHash<snow_shot::presentation::GlobalShortcutAction,
          snow_shot::presentation::GlobalShortcutRegistrationState>
        m_shortcutStates;
    QHash<QString, QStringList> m_localShortcuts;
};

class PageTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* sourceText, const char*,
                      int) const override {
        if (QString::fromLatin1(context) == QStringLiteral("SettingsCatalog") &&
            QString::fromUtf8(sourceText) == QStringLiteral("Theme")) {
            return QStringLiteral("Localized Theme");
        }
        if (QString::fromLatin1(context) == QStringLiteral("SettingsCatalog") &&
            QString::fromUtf8(sourceText) == QStringLiteral("Follow System")) {
            return QStringLiteral("Localized System Theme");
        }
        return {};
    }
};

settings::TranslatableText text(const char* source) {
    return {"SettingsPageTests", source};
}

snow_shot::storage::CaptureHistoryDraft
historyDraft(const QDateTime& createdUtc, snow_shot::storage::CaptureHistorySource source) {
    snow_shot::storage::CaptureHistoryDraft draft;
    draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    draft.createdUtc = createdUtc;
    draft.canvasBounds = QRect(0, 0, 96, 64);
    draft.selection.rectangle = QRect(8, 10, 72, 42);
    draft.selection.cornerRadius = 4;
    draft.selection.shadowWidth = 2;
    draft.selection.shadowColor = QColor(0, 0, 0, 96);
    draft.canvasHistory = QByteArrayLiteral("{\"schemaVersion\":1,\"document\":{},\"history\":{}}");
    draft.source = source;
    QImage image(96, 64, QImage::Format_RGBA8888);
    image.fill(source == snow_shot::storage::CaptureHistorySource::PinnedToScreen
                   ? QColor(QStringLiteral("#52C41A"))
                   : QColor(QStringLiteral("#1677FF")));
    draft.displays.push_back(
        {QStringLiteral("display-1"), QStringLiteral("Primary display"), image});
    return draft;
}

class FakeHistoryDataSource final : public ScreenshotHistoryPageDataSource {
  public:
    using ScreenshotHistoryPageDataSource::ScreenshotHistoryPageDataSource;

    struct PendingDisplayRequest final {
        QVector<snow_shot::storage::CaptureHistoryRecord> records;
        quint64 generation = 0;
    };

    struct PendingResultRequest final {
        snow_shot::storage::CaptureHistoryRecord record;
        quint64 generation = 0;
    };

    QVector<snow_shot::storage::CaptureHistoryRecord> records() const override {
        ++recordsCalls;
        return currentRecords;
    }

    std::optional<snow_shot::storage::CaptureHistoryAssetSet>
    displayAssets(const snow_shot::storage::CaptureHistoryRecord& record) const override {
        ++assetCalls;
        return assets.value(record.id);
    }

    bool supportsAsyncDisplayAssets() const override { return asyncDisplayAssets; }

    void requestDisplayAssets(
        const QVector<snow_shot::storage::CaptureHistoryRecord>& records,
        quint64 generation) override {
        pendingDisplayRequests.push_back({records, generation});
    }

    void requestResultImage(const snow_shot::storage::CaptureHistoryRecord& record,
                            quint64 generation) override {
        ++resultRequests;
        if (deferResultCallbacks) {
            pendingResultRequests.push_back({record, generation});
            return;
        }
        const std::optional<QImage> image =
            resultImages.contains(record.id)
                ? std::optional<QImage>{resultImages.value(record.id)}
                : std::nullopt;
        QMetaObject::invokeMethod(
            this,
            [this, generation, recordId = record.id, image]() {
                emit resultImageReady(
                    generation, ScreenshotHistoryResultResolution{recordId, image});
            },
            Qt::QueuedConnection);
    }

    void cancelPending() override { ++cancelCalls; }

    void deliverDisplayAssets(int requestIndex = 0) {
        if (requestIndex < 0 || requestIndex >= pendingDisplayRequests.size()) {
            return;
        }
        const PendingDisplayRequest request = pendingDisplayRequests.at(requestIndex);
        QVector<ScreenshotHistoryAssetResolution> resolutions;
        resolutions.reserve(request.records.size());
        for (const auto& record : request.records) {
            resolutions.push_back({record.id, assets.value(record.id)});
        }
        emit displayAssetsReady(request.generation, resolutions);
    }

    void deliverResultImage(int requestIndex = 0) {
        if (requestIndex < 0 || requestIndex >= pendingResultRequests.size()) {
            return;
        }
        const PendingResultRequest request = pendingResultRequests.at(requestIndex);
        const std::optional<QImage> image =
            resultImages.contains(request.record.id)
                ? std::optional<QImage>{resultImages.value(request.record.id)}
                : std::nullopt;
        emit resultImageReady(
            request.generation, ScreenshotHistoryResultResolution{request.record.id, image});
    }

    void remove(const QString& id) override {
        removedIds.push_back(id);
    }

    bool requestClear() override {
        ++clearCalls;
        return true;
    }

    void notifyChanged() {
        emit historyChanged();
    }

    mutable int recordsCalls = 0;
    mutable int assetCalls = 0;
    int resultRequests = 0;
    int cancelCalls = 0;
    int clearCalls = 0;
    bool asyncDisplayAssets = false;
    bool deferResultCallbacks = false;
    QVector<QString> removedIds;
    QVector<snow_shot::storage::CaptureHistoryRecord> currentRecords;
    QHash<QString, snow_shot::storage::CaptureHistoryAssetSet> assets;
    QHash<QString, QImage> resultImages;
    QVector<PendingDisplayRequest> pendingDisplayRequests;
    QVector<PendingResultRequest> pendingResultRequests;
};

void screenshotHistoryLifecycleAndIdentityDiff() {
    using namespace snow_shot::storage;
    CaptureHistoryRecord record;
    record.id = QStringLiteral("history-row-1");
    record.createdUtc = QDateTime::currentDateTimeUtc();
    record.canvasBounds = QRect(0, 0, 100, 80);
    record.selection.rectangle = QRect(5, 6, 70, 50);
    record.displays.push_back(
        {QStringLiteral("display-1"), QStringLiteral("Primary"), QSize(100, 80), 128});
    record.result = CaptureHistoryResultRecord{QSize(70, 50), 96};
    record.totalBytes = 256;

    CaptureHistoryAssetSet assetSet;
    assetSet.recordId = record.id;
    assetSet.result = CaptureHistoryResultAsset{
        record.id, QSize(70, 50),
        QUrl::fromLocalFile(QStringLiteral("C:/missing-history-result.png"))};
    assetSet.displays.push_back({record.id, QStringLiteral("display-1"), QStringLiteral("Primary"),
                                 QSize(100, 80),
                                 QUrl::fromLocalFile(QStringLiteral("C:/missing-history.png"))});

    QImage resultImage(QSize(70, 50), QImage::Format_RGBA8888);
    resultImage.fill(QColor(QStringLiteral("#D4380D")));

    FakeHistoryDataSource source;
    source.currentRecords = {record};
    source.assets.insert(record.id, assetSet);
    source.resultImages.insert(record.id, resultImage);
    ScreenshotHistoryPageWidget page(&source, nullptr);
    page.resize(760, 520);
    require(source.recordsCalls == 0 && source.assetCalls == 0,
            "history construction queried its data source");
    source.notifyChanged();
    flushEvents();
    require(source.recordsCalls == 0 && source.assetCalls == 0,
            "inactive history changes triggered reconciliation");

    page.show();
    flushEvents();
    require(source.recordsCalls == 1 && source.assetCalls == 1 && page.totalCount() == 1,
            "first history activation did not reconcile metadata once");
    QWidget* initialRow =
        page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-history-row-1"));
    require(initialRow != nullptr, "history activation did not create the visible row");
    auto* editButton = initialRow->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotHistoryEntryEdit"));
    auto* copyButton = initialRow->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotHistoryEntryCopy"));
    auto* deleteButton = initialRow->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotHistoryEntryDelete"));
    const auto previewImages =
        initialRow->findChildren<adqt::widgets::AdImage*>(QStringLiteral("screenshotHistoryImage"));
    QString editedRecordId;
    QObject::connect(&page, &ScreenshotHistoryPageWidget::editRequested, &page,
                     [&editedRecordId](const QString& recordId) { editedRecordId = recordId; });
    require(editButton != nullptr && copyButton != nullptr && deleteButton != nullptr &&
                editButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                editButton->accentRole() == adqt::widgets::AdButton::AccentRole::Primary &&
                adqt::icons::describeIcon(editButton->iconRef()).key.name ==
                    QStringLiteral("edit") &&
                editButton->mapTo(initialRow, QPoint()).x() <
                    copyButton->mapTo(initialRow, QPoint()).x() &&
                copyButton->mapTo(initialRow, QPoint()).x() <
                    deleteButton->mapTo(initialRow, QPoint()).x(),
            "history actions must place Edit and Copy before Delete");
    require(previewImages.size() == 2 &&
                previewImages[0]->altText() == QStringLiteral("Screenshot result") &&
                previewImages[0]->previewRow() == 0 && previewImages[1]->previewRow() == 1,
            "history preview must place the screenshot result before display images");
    editButton->click();
    require(editedRecordId == record.id, "history Edit did not emit the selected record ID");

    QApplication::clipboard()->clear();
    copyButton->click();
    require(source.resultRequests == 1 && !copyButton->isEnabled(),
            "history Copy did not start one asynchronous result load");
    flushEvents();
    const QImage copiedImage = QApplication::clipboard()->image();
    require(copyButton->isEnabled() && !copiedImage.isNull() &&
                copiedImage.size() == resultImage.size() &&
                copiedImage.pixelColor(4, 5) == resultImage.pixelColor(4, 5),
            "history Copy did not publish the stored result pixels or re-enable its action");

    source.notifyChanged();
    source.notifyChanged();
    flushEvents();
    QWidget* retainedRow =
        page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-history-row-1"));
    require(source.recordsCalls == 2 && source.assetCalls == 1 && retainedRow == initialRow,
            "active changes were not coalesced, revalidated assets, or recreated an unchanged row");

    page.hide();
    flushEvents();
    require(source.cancelCalls > 0, "hiding history must cancel pending page work");
    source.notifyChanged();
    flushEvents();
    require(source.recordsCalls == 2, "hidden history changes performed a metadata reconciliation");
    page.show();
    flushEvents();
    require(source.recordsCalls == 3, "dirty history metadata was not refreshed on reactivation");

    CaptureHistoryRecord resultLessRecord = record;
    resultLessRecord.id = QStringLiteral("history-row-without-result");
    resultLessRecord.result.reset();
    CaptureHistoryAssetSet resultLessAssets = assetSet;
    resultLessAssets.recordId = resultLessRecord.id;
    resultLessAssets.result.reset();
    resultLessAssets.displays[0].recordId = resultLessRecord.id;
    FakeHistoryDataSource resultLessSource;
    resultLessSource.currentRecords = {resultLessRecord};
    resultLessSource.assets.insert(resultLessRecord.id, resultLessAssets);
    ScreenshotHistoryPageWidget resultLessPage(&resultLessSource, nullptr);
    resultLessPage.resize(760, 520);
    resultLessPage.show();
    flushEvents();
    QWidget* resultLessRow = resultLessPage.findChild<QWidget*>(
        QStringLiteral("screenshotHistoryEntry-history-row-without-result"));
    auto* resultLessCopy = resultLessRow != nullptr
                               ? resultLessRow->findChild<adqt::widgets::AdButton*>(
                                     QStringLiteral("screenshotHistoryEntryCopy"))
                               : nullptr;
    require(resultLessCopy != nullptr && resultLessCopy->isHidden(),
            "history Copy must stay hidden when a record has no stored result image");
}

void screenshotHistoryLateCallbacksAreIgnoredAfterReplacement() {
    using namespace snow_shot::storage;
    CaptureHistoryRecord record;
    record.id = QStringLiteral("history-cancel-row");
    record.createdUtc = QDateTime::currentDateTimeUtc();
    record.canvasBounds = QRect(0, 0, 100, 80);
    record.selection.rectangle = QRect(5, 6, 70, 50);
    record.displays.push_back(
        {QStringLiteral("display-1"), QStringLiteral("Primary"), QSize(100, 80), 128});
    record.result = CaptureHistoryResultRecord{QSize(70, 50), 96};

    CaptureHistoryAssetSet assetSet;
    assetSet.recordId = record.id;
    assetSet.displays.push_back({record.id, QStringLiteral("display-1"), QStringLiteral("Primary"),
                                 QSize(100, 80),
                                 QUrl::fromLocalFile(QStringLiteral("C:/late-history.png"))});

    FakeHistoryDataSource source;
    source.asyncDisplayAssets = true;
    source.deferResultCallbacks = true;
    source.currentRecords = {record};
    source.assets.insert(record.id, assetSet);
    source.resultImages.insert(record.id, QImage(QSize(70, 50), QImage::Format_RGBA8888));

    auto* oldPage = new ScreenshotHistoryPageWidget(&source, nullptr);
    oldPage->resize(760, 520);
    oldPage->show();
    flushEvents();
    require(source.pendingDisplayRequests.size() == 1,
            "history cancellation test did not queue an asset request");
    auto* oldRow = oldPage->findChild<QWidget*>(
        QStringLiteral("screenshotHistoryEntry-history-cancel-row"));
    auto* oldCopy = oldRow != nullptr
                        ? oldRow->findChild<adqt::widgets::AdButton*>(
                              QStringLiteral("screenshotHistoryEntryCopy"))
                        : nullptr;
    require(oldCopy != nullptr, "history cancellation test could not find the copy action");
    oldCopy->click();
    require(source.pendingResultRequests.size() == 1,
            "history cancellation test did not queue a result request");

    QPointer<ScreenshotHistoryPageWidget> oldPageGuard(oldPage);
    oldPage->hide();
    oldPage->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(oldPageGuard == nullptr,
            "history cancellation test did not destroy the previous page");
    require(source.cancelCalls > 0,
            "history cancellation test did not cancel the previous page work");

    FakeHistoryDataSource replacementSource;
    ScreenshotHistoryPageWidget replacement(&replacementSource, nullptr);
    replacement.resize(760, 520);
    replacement.show();
    flushEvents();
    require(replacement.totalCount() == 0 &&
                replacement.findChildren<adqt::widgets::AdImage*>().isEmpty(),
            "replacement history page did not start empty");

    // These callbacks belong to the deleted page and must not reach the replacement.
    source.deliverDisplayAssets();
    source.deliverResultImage();
    flushEvents();
    require(replacement.totalCount() == 0 &&
                replacement.findChildren<adqt::widgets::AdImage*>().isEmpty(),
            "late canceled history callbacks modified the replacement page");
}

void screenshotHistoryEmptyToPopulatedGeometryIsStable() {
    using namespace snow_shot::storage;
    CaptureHistoryRecord record;
    record.id = QStringLiteral("geometry-row-1");
    record.createdUtc = QDateTime::currentDateTimeUtc();
    record.canvasBounds = QRect(0, 0, 100, 80);
    record.selection.rectangle = QRect(5, 6, 70, 50);
    record.displays.push_back(
        {QStringLiteral("display-1"), QStringLiteral("Primary"), QSize(100, 80), 128});
    record.totalBytes = 256;

    FakeHistoryDataSource source;
    ScreenshotHistoryPageWidget page(&source, nullptr);
    page.resize(760, 520);
    page.show();
    flushEvents();
    auto* container =
        page.findChild<PageContainerWidget*>(QStringLiteral("screenshotHistoryPageContainer"));
    auto* content = container != nullptr ? container->contentWidget() : nullptr;
    auto* entries = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntries"));
    auto* scroll = container != nullptr ? container->scrollArea() : nullptr;
    require(container != nullptr && content != nullptr && entries != nullptr && scroll != nullptr,
            "history geometry test could not find its shared page container");
    const int emptyContentHeight = content->height();
    const int emptyEntriesHeight = entries->height();
    source.currentRecords = {record};
    source.notifyChanged();
    flushEvents();
    require(content->height() == emptyContentHeight && entries->height() == emptyEntriesHeight &&
                page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-geometry-row-1")) !=
                    nullptr,
            "history empty-to-populated transition must preserve its layout footprint");
    source.currentRecords.clear();
    source.notifyChanged();
    flushEvents();
    require(content->height() == emptyContentHeight && entries->height() == emptyEntriesHeight &&
                page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-geometry-row-1")) ==
                    nullptr,
            "history populated-to-empty transition must preserve its layout footprint");

    page.resize(520, 520);
    flushEvents();
    const int narrowEmptyContentHeight = content->height();
    const int narrowEmptyEntriesHeight = entries->height();
    source.currentRecords = {record};
    source.notifyChanged();
    flushEvents();
    require(content->height() == narrowEmptyContentHeight &&
                entries->height() == narrowEmptyEntriesHeight,
            "history narrow empty-to-populated transition must preserve its layout footprint");

    FakeHistoryDataSource initiallyPopulatedSource;
    initiallyPopulatedSource.currentRecords = {record};
    ScreenshotHistoryPageWidget initiallyPopulatedPage(&initiallyPopulatedSource, nullptr);
    initiallyPopulatedPage.resize(760, 520);
    initiallyPopulatedPage.show();
    flushEvents();
    auto* initiallyPopulatedContainer = initiallyPopulatedPage.findChild<PageContainerWidget*>(
        QStringLiteral("screenshotHistoryPageContainer"));
    auto* initiallyPopulatedContent = initiallyPopulatedContainer != nullptr
                                          ? initiallyPopulatedContainer->contentWidget()
                                          : nullptr;
    auto* initiallyPopulatedEntries =
        initiallyPopulatedPage.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntries"));
    require(initiallyPopulatedContent != nullptr && initiallyPopulatedEntries != nullptr,
            "initially populated history geometry test could not find its content");
    const int initiallyPopulatedContentHeight = initiallyPopulatedContent->height();
    const int initiallyPopulatedEntriesHeight = initiallyPopulatedEntries->height();
    initiallyPopulatedSource.currentRecords.clear();
    initiallyPopulatedSource.notifyChanged();
    flushEvents();
    require(initiallyPopulatedContent->height() == initiallyPopulatedContentHeight &&
                initiallyPopulatedEntries->height() == initiallyPopulatedEntriesHeight,
            "history initially populated-to-empty transition must preserve its layout footprint");
}

void screenshotHistoryPageUsesRepositoryAndAntDesignComponents() {
    auto& repository = snow_shot::storage::ApplicationStorage::instance().captureHistory();
    require(repository.requestClear().get().success, "history test setup must clear storage");
    const QDateTime today = QDateTime::currentDateTimeUtc();
    require(
        repository
                .publish(historyDraft(today.addSecs(-1),
                                      snow_shot::storage::CaptureHistorySource::CopiedToClipboard))
                .get()
                .storage.success &&
            repository
                .publish(
                    historyDraft(today, snow_shot::storage::CaptureHistorySource::PinnedToScreen))
                .get()
                .storage.success,
        "history fixtures must publish");

    ScreenshotHistoryPageWidget page;
    page.resize(760, 520);
    page.show();
    flushEvents();

    auto* sourceFilter =
        page.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenshotHistorySourceFilter"));
    auto* dateRangeFilter = page.findChild<adqt::widgets::AdDateRangePicker*>(
        QStringLiteral("screenshotHistoryDateRangeFilter"));
    auto* pagination =
        page.findChild<adqt::widgets::AdPagination*>(QStringLiteral("screenshotHistoryPagination"));
    auto* deleteAll =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryDeleteAll"));
    auto* refresh =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryRefresh"));
    auto* title = page.findChild<QLabel*>(QStringLiteral("screenshotHistoryTitle"));
    auto* countLabel = page.findChild<QLabel*>(QStringLiteral("screenshotHistoryCountLabel"));
    auto* filters = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryFilters"));
    auto* historyContainer =
        page.findChild<PageContainerWidget*>(QStringLiteral("screenshotHistoryPageContainer"));
    auto* confirmation = page.findChild<adqt::widgets::AdPopconfirm*>(
        QStringLiteral("screenshotHistoryDeleteAllConfirm"));
    auto* displayDescription =
        page.findChild<QLabel*>(QStringLiteral("screenshotHistoryDisplayName"));
    bool carouselSlidesAvailable = true;
    const auto carousels = page.findChildren<adqt::widgets::AdCarousel*>();
    for (adqt::widgets::AdCarousel* carousel : carousels) {
        carouselSlidesAvailable = carouselSlidesAvailable && carousel->count() == 1;
    }
    require(sourceFilter != nullptr && dateRangeFilter != nullptr && pagination != nullptr &&
                deleteAll != nullptr && refresh != nullptr && confirmation != nullptr &&
                title != nullptr && countLabel != nullptr && filters != nullptr,
            "history page must expose its filters, actions, labels, and pagination");
    require(page.totalCount() == 2 && page.filteredCount() == 2 &&
                sourceFilter->options().size() == 4 && sourceFilter->currentValues().isEmpty() &&
                pagination->total() == 2 && deleteAll->isEnabled() &&
                displayDescription == nullptr && historyContainer != nullptr &&
                title->parentWidget()->layout() != nullptr &&
                title->parentWidget()->layout()->spacing() ==
                    snow_shot::presentation::styles::ThemeManager::instance()
                        .themeColorScheme()
                        .metricAlias.marginXXS &&
                historyContainer->contentLayout()->itemAt(1)->spacerItem() != nullptr &&
                historyContainer->contentLayout()->itemAt(1)->spacerItem()->sizeHint().height() ==
                    snow_shot::presentation::styles::ThemeManager::instance()
                        .themeColorScheme()
                        .metricAlias.marginSM &&
                filters->mapTo(&page, QPoint()).y() >=
                    countLabel->mapTo(&page, QPoint()).y() + countLabel->height() &&
                carousels.size() == 2 && carouselSlidesAvailable,
            "history page must expose filters below its title plus management, carousels, and "
            "pagination");

    const auto metric =
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias;
    require(title->font().pixelSize() == metric.fontSizeHeading4 &&
                title->font().weight() == QFont::DemiBold &&
                countLabel->font().pixelSize() == metric.fontSize,
            "history title typography must use Ant Design heading and body tokens");

    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend shortcutBackend;
    settings::SettingsRuntimeSession shortcutSession(registry, shortcutBackend);
    SettingsPageWidget shortcutPage(registry, QStringLiteral("quick-functions"),
                                    shortcutSession);
    auto* resetButton =
        shortcutPage.findChild<adqt::widgets::AdButton*>(QStringLiteral("sectionResetButton"));
    require(resetButton != nullptr && deleteAll->size() == resetButton->size() &&
                refresh->size() == resetButton->size() &&
                deleteAll->buttonStyle() == resetButton->buttonStyle() &&
                refresh->buttonStyle() == resetButton->buttonStyle() &&
                refresh->accentRole() == resetButton->accentRole() &&
                deleteAll->accentRole() == adqt::widgets::AdButton::AccentRole::Danger &&
                deleteAll->shape() == resetButton->shape() &&
                refresh->shape() == resetButton->shape(),
            "history actions must match the quick-functions reset button styling while keeping "
            "the delete danger color");

    page.resize(520, 520);
    flushEvents();
    require(dateRangeFilter->mapTo(filters, QPoint()).y() ==
                sourceFilter->mapTo(filters, QPoint()).y(),
            "history filters must remain tiled in one row at narrow widths");
    page.resize(760, 520);
    flushEvents();
    require(dateRangeFilter->mapTo(filters, QPoint()).y() ==
                sourceFilter->mapTo(filters, QPoint()).y(),
            "history filters must share a row inside their own region at wide widths");

    sourceFilter->setCurrentValues({QStringLiteral("pinned")});
    flushEvents();
    require(page.filteredCount() == 1 && pagination->total() == 1,
            "source filtering must rebuild the visible repository records");

    sourceFilter->setCurrentValues({});
    const QDate captureDate = today.toLocalTime().date();
    dateRangeFilter->setRange(captureDate.addDays(-1), captureDate);
    flushEvents();
    require(page.filteredCount() == 2,
            "date range filtering must include both local calendar date boundaries");

    dateRangeFilter->setRange(captureDate.addDays(1), captureDate.addDays(2));
    flushEvents();
    require(page.filteredCount() == 0,
            "date range filtering must exclude captures outside the selected local dates");

    dateRangeFilter->clear();
    flushEvents();
    require(page.filteredCount() == 2,
            "clearing the date range must restore all source-matching captures");

    require(repository.requestClear().get().success, "history test cleanup must clear storage");
}

void screenshotHistorySurvivesSidebarWidthTransitions() {
    auto& repository = snow_shot::storage::ApplicationStorage::instance().captureHistory();
    require(repository.requestClear().get().success,
            "history collapse test setup must clear storage");
    require(repository
                .publish(historyDraft(QDateTime::currentDateTimeUtc(),
                                      snow_shot::storage::CaptureHistorySource::CopiedToClipboard))
                .get()
                .storage.success,
            "history collapse test fixture must publish");

    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    QWidget window;
    auto* rootLayout = new QHBoxLayout(&window);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* sidebar = new SidebarWidget(registry, &window);
    rootLayout->addWidget(sidebar, 0);

    auto* contentShell = new QWidget(&window);
    auto* contentLayout = new QVBoxLayout(contentShell);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    auto* header = new MainContentHeaderWidget(
        registry,
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias,
        contentShell);
    auto* content = new ContentCardWidget(registry, session, contentShell);
    contentLayout->addWidget(header, 0);
    contentLayout->addWidget(content, 1);
    rootLayout->addWidget(contentShell, 1);

    content->setCurrentRoute(QStringLiteral("/history"));
    sidebar->setCurrentRoute(QStringLiteral("/history"));
    header->setSections(content->currentSections());
    window.resize(900, 556);
    window.show();
    flushEvents();

    auto* tabs = header->findChild<adqt::widgets::AdTabs*>(QStringLiteral("mainSectionTabs"));
    auto* historyPage =
        content->findChild<ScreenshotHistoryPageWidget*>(QStringLiteral("screenshotHistoryPage"));
    require(tabs != nullptr && tabs->count() == 0 && tabs->isHidden() && historyPage != nullptr &&
                historyPage->totalCount() == 1,
            "history route must remove its empty section tabs before responsive layout changes");

    sidebar->setCollapsed(true);
    flushEvents();
    require(sidebar->isCollapsed() && tabs->isHidden() && historyPage->totalCount() == 1,
            "collapsing the sidebar must keep populated screenshot history stable");

    sidebar->setCollapsed(false);
    flushEvents();
    require(!sidebar->isCollapsed() && tabs->isHidden() && historyPage->totalCount() == 1,
            "expanding the sidebar must keep populated screenshot history stable");

    window.hide();
    require(repository.requestClear().get().success,
            "history collapse test cleanup must clear storage");
}

void allPagesShareBaseContainerSpacingAndScrollbar() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    const auto metric =
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias;
    SettingsPageWidget settingsPage(registry, QStringLiteral("interface-settings"), session);
    ScreenshotHistoryPageWidget historyPage;

    auto* settingsContainer = settingsPage.findChild<PageContainerWidget*>(
        QStringLiteral("settings-container-interface-settings"));
    auto* historyContainer = historyPage.findChild<PageContainerWidget*>(
        QStringLiteral("screenshotHistoryPageContainer"));
    require(settingsContainer != nullptr && historyContainer != nullptr,
            "every page must render inside the shared page container");

    const QMargins expectedMargins(metric.paddingLG, metric.paddingXXS, metric.paddingLG,
                                   metric.paddingLG);
    require(settingsContainer->contentLayout()->contentsMargins() == expectedMargins &&
                historyContainer->contentLayout()->contentsMargins() == expectedMargins,
            "all pages must use the same theme-driven base inner spacing");

    auto* settingsHeader = settingsPage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-interface-settings-general"));
    QLayout* historyHeaderLayout = historyContainer->contentLayout()->itemAt(0)->layout();
    require(!historyPage.autoFillBackground() && settingsHeader != nullptr &&
                settingsHeader->layout() != nullptr && historyHeaderLayout != nullptr &&
                settingsHeader->layout()->contentsMargins() ==
                    historyHeaderLayout->contentsMargins(),
            "history must preserve the rounded card background and match the first-title inset");

    auto* settingsScrollArea = settingsContainer->scrollArea();
    auto* historyScrollArea = historyContainer->scrollArea();
    require(settingsScrollArea != nullptr && historyScrollArea != nullptr &&
                settingsScrollArea->scrollBarThickness() == metric.scrollbarThickness &&
                historyScrollArea->scrollBarThickness() == metric.scrollbarThickness &&
                settingsScrollArea->overlayVerticalScrollBar() != nullptr &&
                historyScrollArea->overlayVerticalScrollBar() != nullptr &&
                settingsScrollArea->overlayVerticalScrollBar()->overlayMargins() ==
                    historyScrollArea->overlayVerticalScrollBar()->overlayMargins(),
            "all pages must use the same themed overlay scrollbar");
}

settings::SettingsCatalog expandedCatalog() {
    const auto& builtIn = settings::builtInSettingsRegistry().catalog();
    QVector<settings::SettingsPageDefinition> pages = builtIn.pages();
    settings::SettingsSelectDefinition select;
    select.options = {
        {QStringLiteral("system"), text("Follow System")},
        {QStringLiteral("light"), text("Light")},
        {QStringLiteral("dark"), text("Dark")},
    };
    pages.push_back({QStringLiteral("extra-page"),
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
                         select}}}}});
    QVector<settings::SettingsNavigationNode> navigation = builtIn.navigation();
    navigation.push_back(settings::SettingsNavigationPageDefinition{
        QStringLiteral("nav.extra-page"), QStringLiteral("extra-page"),
        []() { return adqt::icons::antd::outlined::Appstore(); }});
    return {std::move(pages), std::move(navigation), builtIn.defaultLocation()};
}

void registryPageGenerationUsesCompiledPlan() {
    const settings::SettingsRegistry registry = settings::buildBuiltInSettingsRegistry();
    const auto* plan = registry.pagePlan(QStringLiteral("interface-settings"));
    require(plan != nullptr && plan->providerId == QStringLiteral("built-in"),
            "registry page-generation coverage requires a built-in compiled plan");

    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, plan->id, session);
    require(page.property("settingsProviderId").toString() == plan->providerId &&
                page.property("settingsPagePlanIndex").toInt() == plan->pageIndex,
            "registry-backed pages must expose their compiled provider and page-plan identity");

    int expectedFields = 0;
    for (const settings::SettingsSectionPlan& sectionPlan : plan->sectionPlans) {
        expectedFields += static_cast<int>(sectionPlan.fieldIndexes.size());
        for (const int fieldIndex : sectionPlan.fieldIndexes) {
            require(fieldIndex >= 0 && fieldIndex < registry.fields().size(),
                    "registry-backed page generation must use valid compiled field indexes");
            const auto& descriptor = registry.fields().at(fieldIndex);
            const auto anchors = page.findChildren<QWidget*>(
                settings::generatedObjectName(QStringLiteral("settings-item"), descriptor.id));
            require(anchors.size() == 1 &&
                        anchors.constFirst()->property("settingsFieldIndex").toInt() == fieldIndex &&
                        anchors.constFirst()->property("settingsFieldKind").toInt() ==
                            static_cast<int>(descriptor.kind) &&
                        anchors.constFirst()->property("settingsProviderId").toString() ==
                            descriptor.providerId,
                    "generated rows must retain their compiled descriptor identity");
        }
    }
    require(page.findChildren<QWidget*>(QRegularExpression(QStringLiteral("^settings-item-")))
                    .size() == expectedFields,
            "registry-backed pages must render exactly the fields in their page plan");
}

void generatedPagesRenderEveryItemTypeAndResynchronize() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend bindings;
    settings::SettingsRuntimeSession session(registry, bindings);

    SettingsPageWidget quick(registry, QStringLiteral("quick-functions"), session);
    SettingsPageWidget interfacePage(registry, QStringLiteral("interface-settings"), session);
    SettingsPageWidget storagePage(registry, QStringLiteral("storage-and-privacy"), session);
    SettingsPageWidget functionPage(registry, QStringLiteral("function-settings"), session);
    SettingsPageWidget systemPage(registry, QStringLiteral("system-settings"), session);
    SettingsPageWidget hotkeyPage(registry, QStringLiteral("hotkey-settings"), session);
    interfacePage.resize(720, 360);
    quick.resize(720, 520);
    storagePage.resize(720, 480);
    functionPage.resize(720, 240);
    systemPage.resize(720, 240);
    hotkeyPage.resize(720, 360);
    interfacePage.show();
    quick.show();
    storagePage.show();
    functionPage.show();
    systemPage.show();
    hotkeyPage.show();
    flushEvents();

    auto* theme = interfacePage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-interface-theme"));
    auto* language = interfacePage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-interface-language"));
    require(theme != nullptr && language != nullptr && theme->options().size() == 3 &&
                language->options().size() == 2 &&
                theme->currentValue() == QStringLiteral("system") &&
                language->currentValue() == QStringLiteral("en_US"),
            "select renderers must use catalog options and binding values");
    require(!theme->accessibleName().isEmpty() && !theme->accessibleDescription().isEmpty() &&
                !language->accessibleName().isEmpty(),
            "generated controls must expose catalog accessibility metadata");

    auto* ocrAction = functionPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-screenshot-auto-execute-after-text-recognition"));
    auto* doubleClickAction = functionPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-screenshot-double-click-action"));
    auto* middleClickAction = functionPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-screenshot-middle-mouse-button-action"));
    auto* screenRecordingClarity = functionPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-screen-recording-clarity"));
    auto* screenRecordingFrameRate = functionPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-screen-recording-frame-rate"));
    auto* animatedFormat = functionPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-screen-recording-animated-image-format"));
    auto* drawingExclusions = functionPage.findChild<adqt::widgets::AdMultiSelect*>(
        QStringLiteral("settings-control-drawing-quick-selection-disabled-tools"));
    auto* pinZoomMode = functionPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-pin-to-screen-mouse-wheel-zoom-mode"));
    auto* pinAutomaticOcr = functionPage.findChild<adqt::widgets::AdSwitch*>(
        QStringLiteral("settings-control-pin-to-screen-automatic-text-recognition"));
    auto* pinAutoResize = functionPage.findChild<adqt::widgets::AdSwitch*>(
        QStringLiteral("settings-control-pin-to-screen-auto-resize-window"));
    auto* trayLeftClick = functionPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-tray-left-click-action"));
    auto* trayMenuOptions = functionPage.findChild<QWidget*>(
        QStringLiteral("settings-item-tray-menu-options"));
    require(ocrAction != nullptr && ocrAction->options().size() == 6 &&
                doubleClickAction != nullptr && doubleClickAction->options().size() == 4 &&
                middleClickAction != nullptr && middleClickAction->options().size() == 4 &&
                screenRecordingClarity != nullptr && screenRecordingClarity->options().size() == 5 &&
                screenRecordingFrameRate != nullptr && screenRecordingFrameRate->options().size() == 7 &&
                animatedFormat != nullptr && animatedFormat->options().size() == 3 &&
                drawingExclusions != nullptr && drawingExclusions->options().size() == 13 &&
                pinZoomMode != nullptr && pinZoomMode->options().size() == 6 &&
                pinAutomaticOcr != nullptr && pinAutoResize != nullptr &&
                trayLeftClick != nullptr && trayLeftClick->options().size() == 2 &&
                trayMenuOptions != nullptr,
            "new select and multi-select controls must render every advertised option");
    const auto trayMenuCheckboxes =
        trayMenuOptions != nullptr
            ? trayMenuOptions->findChildren<adqt::widgets::AdCheckbox*>()
            : QList<adqt::widgets::AdCheckbox*>{};
    const auto trayMenuSeparators =
        trayMenuOptions != nullptr
            ? trayMenuOptions->findChildren<adqt::widgets::AdDivider*>(
                  QRegularExpression(QStringLiteral("^settings-tray-menu-options-separator-")))
            : QList<adqt::widgets::AdDivider*>{};
    auto* trayMenuGrid = trayMenuOptions != nullptr
                             ? trayMenuOptions->findChild<QWidget*>(
                                   QStringLiteral("settings-tray-menu-options-grid"))
                             : nullptr;
    require(trayMenuCheckboxes.size() == 15 && trayMenuSeparators.size() == 3 &&
                trayMenuGrid != nullptr && trayMenuGrid->layout() != nullptr &&
                qobject_cast<QGridLayout*>(trayMenuGrid->layout())->columnCount() == 2 &&
                std::all_of(trayMenuSeparators.cbegin(), trayMenuSeparators.cend(),
                            [](const auto* separator) {
                                return separator != nullptr &&
                                       separator->dividerSize() ==
                                           adqt::widgets::AdDivider::Size::Small;
                            }) &&
                std::all_of(trayMenuCheckboxes.cbegin(), trayMenuCheckboxes.cend(),
                            [](const auto* checkbox) { return checkbox != nullptr; }),
            "tray Menu Options must render fourteen generated checkboxes in four groups");
    require(interfacePage.findChild<adqt::widgets::AdMultiSelect*>(
                 QStringLiteral("settings-control-drawing-quick-selection-disabled-tools")) ==
                nullptr &&
                interfacePage.findChild<adqt::widgets::AdSelect*>(
                    QStringLiteral("settings-control-pin-to-screen-mouse-wheel-zoom-mode")) ==
                nullptr &&
                interfacePage.findChild<adqt::widgets::AdSelect*>(
                    QStringLiteral("settings-control-tray-left-click-action")) == nullptr,
            "moved controls must no longer render on Interface Settings");

    ocrAction->setCurrentValue(QStringLiteral("copy_text"));
    screenRecordingFrameRate->setCurrentValue(83);
    pinZoomMode->setCurrentValue(QStringLiteral("top_right"));
    pinAutomaticOcr->setChecked(false);
    pinAutoResize->setChecked(false);
    trayLeftClick->setCurrentValue(QStringLiteral("show_main_window"));
    auto* hiddenMenuOption = functionPage.findChild<adqt::widgets::AdCheckbox*>(
        QStringLiteral("settings-tray-menu-option-quick.screenshot-full-screen"));
    require(hiddenMenuOption != nullptr && !hiddenMenuOption->isChecked(),
            "unchecked tray menu options must start hidden");
    hiddenMenuOption->click();
    drawingExclusions->setSelectedValues(
        {QStringLiteral("free-draw"), QStringLiteral("pen-filter")});
    require(bindings.selectValue(settings::SettingsSelectBinding::ScreenshotOcrAction) ==
                    QStringLiteral("copy_text") &&
                bindings.selectValue(settings::SettingsSelectBinding::ScreenRecordingFrameRate).toInt() ==
                    83 &&
                bindings.selectValue(settings::SettingsSelectBinding::PinMouseWheelZoomMode) ==
                    QStringLiteral("top_right") &&
                !bindings.switchValue(settings::SettingsSwitchBinding::PinAutomaticTextRecognition) &&
                !bindings.switchValue(settings::SettingsSwitchBinding::PinAutoResizeWindow) &&
                bindings.selectValue(settings::SettingsSelectBinding::TrayLeftClickAction) ==
                    QStringLiteral("show_main_window") &&
                bindings.multiSelectValue(
                    settings::SettingsMultiSelectBinding::DrawingQuickSelectionDisabledTools) ==
                    QVariantList{QStringLiteral("free-draw"), QStringLiteral("pen-filter")} &&
                bindings.multiSelectValue(settings::SettingsMultiSelectBinding::TrayMenuOptions)
                        .contains(QStringLiteral("quick.screenshot-full-screen")),
            "new select and multi-select values must flow through runtime bindings");

    auto* screenshotShortcutList = hotkeyPage.findChild<QWidget*>(
        QStringLiteral("settings-section-list-hotkey-settings-screenshot-shortcuts"));
    auto* drawingShortcutList = hotkeyPage.findChild<QWidget*>(
        QStringLiteral("settings-section-list-hotkey-settings-drawing-shortcuts"));
    auto* pinToScreenShortcutList = hotkeyPage.findChild<QWidget*>(
        QStringLiteral("settings-section-list-hotkey-settings-pin-to-screen-shortcuts"));
    auto* otherShortcutList = hotkeyPage.findChild<QWidget*>(
        QStringLiteral("settings-section-list-hotkey-settings-other-shortcuts"));
    const auto screenshotShortcutRows =
        screenshotShortcutList != nullptr
            ? screenshotShortcutList->findChildren<ShortcutKeyRow*>()
            : QList<ShortcutKeyRow*>{};
    const auto drawingShortcutRows =
        drawingShortcutList != nullptr ? drawingShortcutList->findChildren<ShortcutKeyRow*>()
                                       : QList<ShortcutKeyRow*>{};
    const auto pinToScreenShortcutRows =
        pinToScreenShortcutList != nullptr
            ? pinToScreenShortcutList->findChildren<ShortcutKeyRow*>()
            : QList<ShortcutKeyRow*>{};
    const auto otherShortcutRows =
        otherShortcutList != nullptr ? otherShortcutList->findChildren<ShortcutKeyRow*>()
                                     : QList<ShortcutKeyRow*>{};
    const auto localShortcutRows = hotkeyPage.findChildren<ShortcutKeyRow*>();
    require(screenshotShortcutRows.size() == 18 && drawingShortcutRows.size() == 10 &&
                pinToScreenShortcutRows.size() == 11 && otherShortcutRows.size() == 6 &&
                localShortcutRows.size() == 45 &&
                std::all_of(localShortcutRows.cbegin(), localShortcutRows.cend(),
                            [](const ShortcutKeyRow* row) {
                                const auto* status =
                                    row != nullptr
                                        ? row->findChild<InfoTooltipIcon*>(QStringLiteral(
                                              "shortcutRegistrationStatusTooltipTrigger"))
                                        : nullptr;
                                return status != nullptr && status->isHidden();
                            }),
            "Hotkey Settings must render every generated shortcut category without global "
            "status");
    const auto settingMetrics =
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias;
    const auto hasNaturalCategoryTitleHeight = [](const SettingsPageWidget& page) {
        const auto headers = page.findChildren<SectionHeaderWidget*>();
        return !headers.isEmpty() &&
               std::all_of(headers.cbegin(), headers.cend(), [](const SectionHeaderWidget* header) {
                   return header != nullptr &&
                          header->sizePolicy().verticalPolicy() == QSizePolicy::Fixed &&
                          header->height() == header->sizeHint().height();
               });
    };
    require(hasNaturalCategoryTitleHeight(interfacePage) &&
                hasNaturalCategoryTitleHeight(functionPage) &&
                hasNaturalCategoryTitleHeight(storagePage),
            "Interface, Function, and Storage category titles must not absorb spare page height");
    constexpr int expectedControlWidth = 230;
    require(theme->width() == expectedControlWidth && language->width() == expectedControlWidth &&
                drawingExclusions->width() == expectedControlWidth,
            "settings controls must use the expanded right-column width");
    auto* screenshotShortcutGrid =
        screenshotShortcutList != nullptr && screenshotShortcutList->layout() != nullptr &&
                screenshotShortcutList->layout()->count() == 1
            ? qobject_cast<QGridLayout*>(screenshotShortcutList->layout()->itemAt(0)->layout())
            : nullptr;
    auto* drawingShortcutGrid =
        drawingShortcutList != nullptr && drawingShortcutList->layout() != nullptr &&
                drawingShortcutList->layout()->count() == 1
            ? qobject_cast<QGridLayout*>(drawingShortcutList->layout()->itemAt(0)->layout())
            : nullptr;
    auto* pinToScreenShortcutGrid =
        pinToScreenShortcutList != nullptr && pinToScreenShortcutList->layout() != nullptr &&
                pinToScreenShortcutList->layout()->count() == 1
            ? qobject_cast<QGridLayout*>(pinToScreenShortcutList->layout()->itemAt(0)->layout())
            : nullptr;
    auto* otherShortcutGrid =
        otherShortcutList != nullptr && otherShortcutList->layout() != nullptr &&
                otherShortcutList->layout()->count() == 1
            ? qobject_cast<QGridLayout*>(otherShortcutList->layout()->itemAt(0)->layout())
            : nullptr;
    require(screenshotShortcutGrid != nullptr && screenshotShortcutGrid->count() == 18 &&
                screenshotShortcutGrid->columnCount() == 2 &&
                screenshotShortcutGrid->rowCount() == 9 && drawingShortcutGrid != nullptr &&
                drawingShortcutGrid->count() == 10 &&
                drawingShortcutGrid->columnCount() == 2 && drawingShortcutGrid->rowCount() == 5 &&
                pinToScreenShortcutGrid != nullptr && pinToScreenShortcutGrid->count() == 11 &&
                pinToScreenShortcutGrid->columnCount() == 2 &&
                pinToScreenShortcutGrid->rowCount() == 6 && otherShortcutGrid != nullptr &&
                otherShortcutGrid->count() == 6 && otherShortcutGrid->columnCount() == 2 &&
                otherShortcutGrid->rowCount() == 3 &&
                drawingShortcutGrid->horizontalSpacing() == settingMetrics.marginLG &&
                drawingShortcutGrid->verticalSpacing() == settingMetrics.marginLG &&
                std::all_of(drawingShortcutRows.cbegin(), drawingShortcutRows.cend(),
                            [&settingMetrics](const ShortcutKeyRow* row) {
                                const auto* label =
                                    row != nullptr
                                        ? row->findChild<QLabel*>(
                                              QStringLiteral("shortcutTitleLabel"))
                                        : nullptr;
                                const auto* button =
                                    row != nullptr
                                        ? row->findChild<adqt::widgets::AdButton*>(
                                              QStringLiteral("shortcutKeyButton"))
                                        : nullptr;
                                return row != nullptr &&
                                       row->height() == settingMetrics.controlHeight &&
                                       row->cursor().shape() == Qt::ArrowCursor && label != nullptr &&
                                       label->text().endsWith(QLatin1Char(':')) &&
                                       label->font().pixelSize() == settingMetrics.fontSize &&
                                       label->font().weight() == QFont::Normal && button != nullptr &&
                                       button->buttonStyle() ==
                                           adqt::widgets::AdButton::ButtonStyle::Outline &&
                                       button->accentRole() ==
                                           adqt::widgets::AdButton::AccentRole::Neutral;
                            }),
            "Generated shortcut categories must use the reference two-column title and key-button "
            "presentation without changing the category header or shared page shell");

    auto* generalList = interfacePage.findChild<QWidget*>(
        QStringLiteral("settings-section-list-interface-settings-general"));
    auto* screenshotActions = quick.findChild<QWidget*>(
        QStringLiteral("settings-section-list-quick-functions-screenshot"));
    auto* themeRow =
        interfacePage.findChild<QWidget*>(QStringLiteral("settings-item-interface-theme"));
    auto* languageRow =
        interfacePage.findChild<QWidget*>(QStringLiteral("settings-item-interface-language"));
    const auto contentHeight = [](const QWidget* row) {
        int height = 0;
        if (row != nullptr && row->layout() != nullptr) {
            for (int index = 0; index < row->layout()->count(); ++index) {
                if (QWidget* child = row->layout()->itemAt(index)->widget(); child != nullptr) {
                    height = std::max(height, child->height());
                }
            }
        }
        return height;
    };
    require(generalList != nullptr && generalList->layout() != nullptr &&
                generalList->layout()->count() == 2 && screenshotActions != nullptr &&
                screenshotActions->layout() != nullptr &&
                screenshotActions->layout()->count() == 8 &&
                generalList->layout()->spacing() == settingMetrics.paddingLG &&
                screenshotActions->layout()->spacing() == settingMetrics.padding &&
                themeRow != nullptr && themeRow->layout() != nullptr &&
                themeRow->layout()->contentsMargins().top() == 0 &&
                themeRow->layout()->contentsMargins().bottom() == 0 &&
                themeRow->height() == contentHeight(themeRow) && languageRow != nullptr &&
                languageRow->height() == contentHeight(languageRow),
            "quick actions and settings must use list spacing without divider components");

    auto* trayIconRadio = interfacePage.findChild<adqt::widgets::AdRadio*>(
        QStringLiteral("settings-control-interface-tray-icon"));
    auto* trayIconRow =
        interfacePage.findChild<QWidget*>(QStringLiteral("settings-item-interface-tray-icon"));
    const auto trayIconRadios = trayIconRadio != nullptr && trayIconRadio->parentWidget() != nullptr
                                    ? trayIconRadio->parentWidget()->findChildren<adqt::widgets::AdRadio*>()
                                    : QList<adqt::widgets::AdRadio*>();
    int rightmostRadioEdge = 0;
    int leftmostRadioEdge = 0;
    if (!trayIconRadios.isEmpty() && trayIconRow != nullptr) {
        leftmostRadioEdge = trayIconRadios.constFirst()->mapTo(
                                trayIconRow, QPoint(0, 0))
                                .x();
        for (const auto* radio : trayIconRadios) {
            rightmostRadioEdge = std::max(
                rightmostRadioEdge,
                radio->mapTo(trayIconRow, QPoint(radio->width() - 1, 0)).x());
        }
    }
    require(trayIconRadio != nullptr && trayIconRow != nullptr &&
                trayIconRadios.size() > 1 && rightmostRadioEdge == trayIconRow->contentsRect().right() &&
                std::all_of(trayIconRadios.cbegin(), trayIconRadios.cend(),
                            [trayIconRow, leftmostRadioEdge](const auto* radio) {
                                return radio->mapTo(trayIconRow, QPoint(0, 0)).x() ==
                                       leftmostRadioEdge;
                            }),
            "radio group must align to the right while its controls share a left edge");

    theme->setCurrentValue(QStringLiteral("light"));
    require(bindings.selectValue(settings::SettingsSelectBinding::Theme) == QStringLiteral("light"),
            "accepted select writes must flow through runtime bindings");
    bindings.acceptWrites = false;
    theme->setCurrentValue(QStringLiteral("dark"));
    const settings::SettingsFieldState rejectedTheme =
        session.state(QStringLiteral("interface.theme"));
    require(theme->currentValue() == QStringLiteral("dark") && rejectedTheme.dirty &&
                !rejectedTheme.busy &&
                rejectedTheme.phase == settings::SettingsWritePhase::Rejected,
            "rejected select writes must retain the visible draft for retry or discard");
    bindings.acceptWrites = true;

    auto* applicationPriority = systemPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-system-application-priority"));
    require(applicationPriority != nullptr && applicationPriority->options().size() == 4 &&
                applicationPriority->currentValue() == QStringLiteral("above_normal") &&
                applicationPriority->currentText() == QStringLiteral("Above normal") &&
                applicationPriority->selectedModelIndexes().size() == 1,
            "application priority must resolve its stored value to the labeled selected option");
    applicationPriority->setCurrentValue(QStringLiteral("high"));
    require(bindings.selectValue(settings::SettingsSelectBinding::ApplicationPriority) ==
                    QStringLiteral("high") &&
                applicationPriority->currentText() == QStringLiteral("High") &&
                applicationPriority->selectedModelIndexes().size() == 1,
            "application priority changes must update both its label and dropdown selection");

    auto* proxy = systemPage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-network-proxy"));
    require(proxy != nullptr && proxy->options().size() == 2 &&
                proxy->currentValue() == QStringLiteral("none") &&
                proxy->currentText() == QStringLiteral("No proxy"),
            "network proxy must render as a two-option select defaulting to no proxy");
    proxy->setCurrentValue(QStringLiteral("system"));
    require(bindings.selectValue(settings::SettingsSelectBinding::Proxy) ==
                    QStringLiteral("system") &&
                proxy->currentText() == QStringLiteral("Use system proxy"),
            "network proxy changes must flow through runtime bindings");

    auto* directMlAcceleration = systemPage.findChild<adqt::widgets::AdSwitch*>(
        QStringLiteral("settings-control-text-recognition-direct-ml-acceleration"));
    require(directMlAcceleration != nullptr && directMlAcceleration->isEnabled() &&
                directMlAcceleration->isChecked(),
            "supported Direct ML acceleration must render enabled and on by default");
    directMlAcceleration->setChecked(false);
    require(!bindings.switchValue(settings::SettingsSwitchBinding::DirectMlAcceleration),
            "Direct ML acceleration changes must flow through runtime bindings");
    bindings.directMlSupported = false;
    emit bindings.synchronized();
    flushEvents();
    require(!directMlAcceleration->isEnabled(),
            "Direct ML acceleration must be disabled when the environment is unsupported");

    auto* imageDirectory = storagePage.findChild<adqt::widgets::AdSearchEdit*>(
        QStringLiteral("settings-control-screenshot-output-image-save-directory"));
    auto* imageFormat = storagePage.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-screenshot-output-image-format"));
    auto* manualFilename = storagePage.findChild<adqt::widgets::AdLineEdit*>(
        QStringLiteral("settings-control-screenshot-output-manual-filename-format"));
    auto* autoFilename = storagePage.findChild<adqt::widgets::AdLineEdit*>(
        QStringLiteral("settings-control-screenshot-output-auto-filename-format"));
    auto* videoDirectory = storagePage.findChild<adqt::widgets::AdSearchEdit*>(
        QStringLiteral("settings-control-screen-recording-output-video-save-directory"));
    auto* videoFilename = storagePage.findChild<adqt::widgets::AdLineEdit*>(
        QStringLiteral("settings-control-screen-recording-output-video-filename-format"));
    require(imageDirectory != nullptr && imageFormat != nullptr &&
                manualFilename != nullptr && autoFilename != nullptr &&
                videoDirectory != nullptr && videoFilename != nullptr &&
                imageDirectory->text() == QStringLiteral("C:/Pictures/SnowShot") &&
                imageDirectory->searchButtonText() == QStringLiteral("Browse") &&
                imageFormat->options().size() == 5 &&
                imageFormat->currentValue() == QStringLiteral("png") &&
                manualFilename->text() ==
                    QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}") &&
                autoFilename->text() ==
                    QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}") &&
                videoDirectory->text() == QStringLiteral("C:/Videos/SnowShot") &&
                videoFilename->text() ==
                    QStringLiteral("SnowShot_Video_{YYYY-MM-DD_HH-mm-ss}"),
            "Storage and Privacy must render all screenshot and recording output controls");

    imageDirectory->setText(QStringLiteral("D:/Captures"));
    require(QMetaObject::invokeMethod(imageDirectory, "editingFinished",
                                      Qt::DirectConnection),
            "image directory editor did not expose its commit signal");
    imageFormat->setCurrentValue(QStringLiteral("webp"));
    manualFilename->setText(QStringLiteral("Manual_{yyyyMMdd_HHmmss}"));
    require(QMetaObject::invokeMethod(manualFilename, "editingFinished", Qt::DirectConnection),
            "manual filename editor did not expose its commit signal");
    videoFilename->setText(QStringLiteral("Recording_{yyyyMMdd_HHmmss}"));
    require(QMetaObject::invokeMethod(videoFilename, "editingFinished", Qt::DirectConnection),
            "video filename editor did not expose its commit signal");
    require(bindings.directoryPathValue(
                settings::SettingsDirectoryPathBinding::ScreenshotImageDirectory) ==
                QStringLiteral("D:/Captures") &&
                bindings.selectValue(settings::SettingsSelectBinding::ScreenshotImageFormat) ==
                    QStringLiteral("webp") &&
                bindings.textValue(
                    settings::SettingsTextBinding::ScreenshotManualFilenameFormat) ==
                    QStringLiteral("Manual_{yyyyMMdd_HHmmss}") &&
                bindings.textValue(
                    settings::SettingsTextBinding::ScreenRecordingVideoFilenameFormat) ==
                    QStringLiteral("Recording_{yyyyMMdd_HHmmss}"),
            "output settings must flow through their runtime bindings");

    auto* historySwitch = storagePage.findChild<adqt::widgets::AdSwitch*>(
        QStringLiteral("settings-control-history-enabled"));
    auto* retention = storagePage.findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("settings-control-history-retention-days"));
    auto* entries = storagePage.findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("settings-control-history-max-entries"));
    auto* disk = storagePage.findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("settings-control-history-max-disk-mib"));
    auto* clear = storagePage.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("settings-control-history-clear"));
    require(historySwitch != nullptr && retention != nullptr && entries != nullptr &&
                disk != nullptr && clear != nullptr,
            "switch, integer, and action renderers must all be generated");
    auto* smartSelection = functionPage.findChild<adqt::widgets::AdSwitch*>(
        QStringLiteral("settings-control-screenshot-smart-selection"));
    require(smartSelection != nullptr && smartSelection->isChecked(),
            "Smart Selection must render as an enabled switch by default");
    require(retention->minimum() == 1 && retention->maximum() == 365 && entries->minimum() == 1 &&
                entries->maximum() == 1000 && disk->minimum() == 128 && disk->maximum() == 10240,
            "integer constraints must come from ConfigurationSchema metadata");
    historySwitch->setChecked(false);
    smartSelection->setChecked(false);
    retention->setValue(30);
    require(!bindings.switchValue(settings::SettingsSwitchBinding::HistoryEnabled) &&
                !bindings.switchValue(settings::SettingsSwitchBinding::SmartSelection) &&
                bindings.integerValue(settings::SettingsIntegerBinding::HistoryRetentionDays) == 30,
            "generated switch and integer controls must submit changes through runtime bindings");

    auto* historyHeader = storagePage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-storage-and-privacy-history"));
    auto* screenshotOutputHeader = storagePage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-storage-and-privacy-screenshots"));
    auto* statusHeader = storagePage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-storage-and-privacy-storage-status"));
    require(
        historyHeader != nullptr && screenshotOutputHeader != nullptr &&
            statusHeader != nullptr &&
            historyHeader->layout()->contentsMargins().top() ==
                historyHeader->layout()->contentsMargins().bottom() &&
            statusHeader->layout()->contentsMargins().top() ==
                statusHeader->layout()->contentsMargins().bottom() &&
            historyHeader->findChild<adqt::widgets::AdButton*>(QStringLiteral("sectionResetButton"))
                ->isVisible() &&
            statusHeader->findChild<adqt::widgets::AdButton*>(QStringLiteral("sectionResetButton"))
                ->isHidden(),
            "section headers must use equal vertical spacing and catalog reset visibility");
    const int switchApplyCountBeforeHistoryReset = bindings.switchApplyCount;
    require(QMetaObject::invokeMethod(historyHeader, "resetRequested", Qt::DirectConnection) &&
                bindings.resetRequested == settings::SettingsSectionReset::HistoryPolicy &&
                historySwitch->isChecked() && retention->value() == 7 &&
                bindings.switchApplyCount == switchApplyCountBeforeHistoryReset,
            "section reset must be catalog-configured and resynchronize all policy controls");
    require(QMetaObject::invokeMethod(screenshotOutputHeader, "resetRequested",
                                      Qt::DirectConnection) &&
                bindings.resetRequested == settings::SettingsSectionReset::ScreenshotOutput &&
                imageDirectory->text() == QStringLiteral("C:/Pictures/SnowShot") &&
                imageFormat->currentValue() == QStringLiteral("png") &&
                manualFilename->text() ==
                    QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"),
            "Screenshot output reset must restore every configured default");
    auto* functionHeader = functionPage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-function-settings-screenshot-settings"));
    require(functionHeader != nullptr &&
                QMetaObject::invokeMethod(functionHeader, "resetRequested", Qt::DirectConnection) &&
                bindings.resetRequested == settings::SettingsSectionReset::ScreenshotSettings &&
                smartSelection->isChecked(),
            "Screenshot settings reset must restore Smart Selection to enabled");

    auto* screenshotShortcutHeader = hotkeyPage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-hotkey-settings-screenshot-shortcuts"));
    bool screenshotShortcutDefaultsMutated = true;
    for (const QString& actionId : screenshotCategoryShortcutIds()) {
        screenshotShortcutDefaultsMutated =
            screenshotShortcutDefaultsMutated &&
            bindings.applyLocalShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                         actionId, {QStringLiteral("Alt+Q")}) &&
            bindings.localShortcuts(settings::SettingsLocalShortcutScope::Screenshot, actionId) ==
                QStringList{QStringLiteral("Alt+Q")};
    }
    const auto screenshotShortcutDefaultsAreRestored = [&bindings]() {
        for (const QString& actionId : screenshotCategoryShortcutIds()) {
            if (bindings.localShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                        actionId) != screenshotShortcutDefaults().value(actionId)) {
                return false;
            }
        }
        return true;
    };
    require(screenshotShortcutHeader != nullptr && screenshotShortcutDefaultsMutated &&
                QMetaObject::invokeMethod(screenshotShortcutHeader, "resetRequested",
                                          Qt::DirectConnection) &&
                bindings.resetRequested ==
                    settings::SettingsSectionReset::ScreenshotEditorShortcuts &&
                screenshotShortcutDefaultsAreRestored(),
            "Screenshot shortcut reset must restore every Screenshot shortcut default");

    auto* otherShortcutHeader = hotkeyPage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-hotkey-settings-other-shortcuts"));
    bool otherShortcutDefaultsMutated = true;
    for (const QString& actionId : otherShortcutIds()) {
        otherShortcutDefaultsMutated =
            otherShortcutDefaultsMutated &&
            bindings.applyLocalShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                         actionId, {QStringLiteral("Alt+W")});
    }
    const auto otherShortcutDefaultsAreRestored = [&bindings]() {
        for (const QString& actionId : otherShortcutIds()) {
            if (bindings.localShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                        actionId) != screenshotShortcutDefaults().value(actionId)) {
                return false;
            }
        }
        return true;
    };
    require(otherShortcutHeader != nullptr && otherShortcutDefaultsMutated &&
                QMetaObject::invokeMethod(otherShortcutHeader, "resetRequested",
                                          Qt::DirectConnection) &&
                bindings.resetRequested ==
                    settings::SettingsSectionReset::ScreenshotOtherShortcuts &&
                otherShortcutDefaultsAreRestored(),
            "Other shortcut reset must restore recognition, Undo, and Redo defaults");

    auto* pinToScreenShortcutHeader = hotkeyPage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-hotkey-settings-pin-to-screen-shortcuts"));
    bool pinToScreenShortcutDefaultsMutated = true;
    for (auto it = pinToScreenShortcutDefaults().cbegin();
         it != pinToScreenShortcutDefaults().cend(); ++it) {
        pinToScreenShortcutDefaultsMutated =
            pinToScreenShortcutDefaultsMutated &&
            bindings.applyLocalShortcuts(settings::SettingsLocalShortcutScope::PinToScreen,
                                         it.key(), {QStringLiteral("Alt+Q")}) &&
            bindings.localShortcuts(settings::SettingsLocalShortcutScope::PinToScreen, it.key()) ==
                QStringList{QStringLiteral("Alt+Q")};
    }
    const auto pinToScreenShortcutDefaultsAreRestored = [&bindings]() {
        for (auto it = pinToScreenShortcutDefaults().cbegin();
             it != pinToScreenShortcutDefaults().cend(); ++it) {
            if (bindings.localShortcuts(settings::SettingsLocalShortcutScope::PinToScreen,
                                        it.key()) != it.value()) {
                return false;
            }
        }
        return true;
    };
    require(pinToScreenShortcutHeader != nullptr && pinToScreenShortcutDefaultsMutated &&
                QMetaObject::invokeMethod(pinToScreenShortcutHeader, "resetRequested",
                                          Qt::DirectConnection) &&
                bindings.resetRequested == settings::SettingsSectionReset::PinToScreenShortcuts &&
                pinToScreenShortcutDefaultsAreRestored(),
            "Pin to Screen shortcut reset must restore every pinned-window shortcut default");

    bindings.setStorageState(false, true);
    flushEvents();
    auto* historyReset =
        historyHeader->findChild<adqt::widgets::AdButton*>(QStringLiteral("sectionResetButton"));
    require(!historySwitch->isEnabled() && !retention->isEnabled() && !entries->isEnabled() &&
                !disk->isEnabled() && clear->busy() && !clear->isEnabled() &&
                !imageDirectory->isEnabled() && !imageFormat->isEnabled() &&
                !manualFilename->isEnabled() && !videoDirectory->isEnabled() &&
                !videoFilename->isEnabled() &&
                historyReset != nullptr && !historyReset->isEnabled(),
            "read-only and busy binding state must resynchronize generated history controls");
    bindings.setStorageState(true, false);
    flushEvents();

    auto* status = storagePage.findChild<StorageStatusSettingsWidget*>(
        QStringLiteral("settings-item-storage-status"));
    auto* descriptions = status != nullptr
                             ? status->findChild<adqt::widgets::AdDescriptions*>(
                                   QStringLiteral("settings-storage-status-descriptions"))
                             : nullptr;
    auto* entryCount =
        storagePage.findChild<QLabel*>(QStringLiteral("settings-status-value-entries"));
    auto* diskUsage =
        storagePage.findChild<QLabel*>(QStringLiteral("settings-status-value-disk-usage"));
    QLabel* entriesLabel = nullptr;
    if (descriptions != nullptr) {
        const auto labels = descriptions->findChildren<QLabel*>();
        for (QLabel* label : labels) {
            if (label->text() == QStringLiteral("Entries") &&
                label->accessibleDescription() == QStringLiteral("Description label")) {
                entriesLabel = label;
                break;
            }
        }
    }
    const int descriptionFontSize = snow_shot::presentation::styles::ThemeManager::instance()
                                        .themeColorScheme()
                                        .metricAlias.fontSize;
    require(status != nullptr && descriptions != nullptr && descriptions->column() == 1 &&
                descriptions->count() == 5 && entryCount != nullptr &&
                entryCount->text() == QStringLiteral("2") && diskUsage != nullptr &&
                diskUsage->text() == QStringLiteral("2.00 KiB") && entriesLabel != nullptr &&
                entryCount->font().pixelSize() == descriptionFontSize &&
                entryCount->font().pixelSize() == entriesLabel->font().pixelSize(),
            "custom Storage Status rendering must use the Descriptions content typography");

    clear->click();
    flushEvents();
    auto* modal = storagePage.findChild<adqt::widgets::AdModal*>(
        QStringLiteral("settings-modal-history-clear"));
    require(modal != nullptr && modal->isOpen() && !bindings.actionTriggered,
            "destructive actions must wait for configured confirmation");
    modal->accept();
    flushEvents();
    require(bindings.actionTriggered,
            "confirmed action rows must invoke their runtime action binding");

    auto* screenshot =
        quick.findChild<ShortcutKeyRow*>(QStringLiteral("settings-item-quick-screenshot"));
    auto* screenshotDelay =
        quick.findChild<ShortcutKeyRow*>(QStringLiteral("settings-item-quick-screenshot-delay"));
    auto* screenshotDelayTitle = screenshotDelay != nullptr
                                     ? screenshotDelay->findChild<QLabel*>(
                                           QStringLiteral("delayTitleLabel"))
                                     : nullptr;
    auto* trayScreenshotDelay = functionPage.findChild<adqt::widgets::AdCheckbox*>(
        QStringLiteral("settings-tray-menu-option-quick.screenshot-delay"));
    auto* trayRecordingToggle = functionPage.findChild<adqt::widgets::AdCheckbox*>(
        QStringLiteral("settings-tray-menu-option-quick.screen-record-copy"));
    bool delayTitleIsRendered = false;
    if (screenshotDelay != nullptr) {
        for (const QLabel* label : screenshotDelay->findChildren<QLabel*>()) {
            if (label->text() == QStringLiteral("Delay 3s to Execute")) {
                delayTitleIsRendered = true;
                break;
            }
        }
    }
    const QString expectedDelayTitle = registry.catalog().shortcutActionTitle(
        snow_shot::presentation::GlobalShortcutAction::ScreenshotDelay, 3);
    const QString expectedRecordingTitle = registry.catalog().shortcutActionTitle(
        snow_shot::presentation::GlobalShortcutAction::ScreenRecordCopy);
    require(screenshot != nullptr && screenshotDelay != nullptr &&
                screenshotDelayTitle != nullptr && screenshotDelay->delaySeconds() == 3 &&
                delayTitleIsRendered &&
                screenshotDelay->cursor().shape() == Qt::PointingHandCursor &&
                screenshotDelayTitle->cursor().shape() == Qt::SplitVCursor,
            "shortcut rows must render their registry title and configured interaction affordances");
    require(trayScreenshotDelay != nullptr &&
                trayScreenshotDelay->text() == expectedDelayTitle,
            "the tray delay option must use the canonical registry shortcut title");
    require(trayRecordingToggle != nullptr &&
                trayRecordingToggle->text() == expectedRecordingTitle,
            "the tray recording option must use the canonical registry shortcut title");

    QWheelEvent increaseDelay(
        QPointF(screenshotDelayTitle->rect().center()),
        QPointF(screenshotDelayTitle->mapToGlobal(screenshotDelayTitle->rect().center())),
        QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(screenshotDelayTitle, &increaseDelay);
    require(screenshotDelay->delaySeconds() == 4 &&
                bindings.integerValue(settings::SettingsIntegerBinding::ScreenshotDelaySeconds) ==
                    4 &&
                trayScreenshotDelay->text() == QStringLiteral("Delay 4s to Execute"),
            "delay changes must update both shortcut and tray-option titles");
    settings::SettingsCommand command;
    bool commandEmitted = false;
    QObject::connect(&quick, &SettingsPageWidget::commandRequested, &quick,
                     [&command, &commandEmitted](const auto& requested) {
                         command = requested;
                         commandEmitted = true;
                     });
    screenshot->click();
    require(commandEmitted && command.kind == settings::SettingsCommandKind::CaptureScreenshot,
            "shortcut row clicks must emit their configured command");

    PageTranslator translator;
    require(QCoreApplication::installTranslator(&translator), "page translator must install");
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&interfacePage, &languageChange);
    require(theme->accessibleName() == QStringLiteral("Localized Theme") &&
                theme->options().constFirst().label == QStringLiteral("Localized System Theme"),
            "generated controls and options must retranslate catalog metadata");
    QCoreApplication::removeTranslator(&translator);
    QCoreApplication::sendEvent(&interfacePage, &languageChange);

    interfacePage.raise();
    interfacePage.activateWindow();
    interfacePage.reveal({QStringLiteral("interface-settings"), QStringLiteral("general"),
                          QStringLiteral("interface.language")});
    flushEvents();
    QWidget* focused = QApplication::focusWidget();
    require(focused == language || (focused != nullptr && language->isAncestorOf(focused)),
            "structured item navigation must reveal and focus an appropriate generated control");
}

void quickActionCommandsDispatchThroughContentCard() {
    using Action = snow_shot::presentation::GlobalShortcutAction;

    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend bindings;
    settings::SettingsRuntimeSession session(registry, bindings);
    ContentCardWidget content(registry, session);
    content.setCurrentRoute(QStringLiteral("/"));

    QVector<Action> requestedActions;
    int screenshotRequests = 0;
    QObject::connect(&content, &ContentCardWidget::quickActionRequested, &content,
                     [&requestedActions](Action action) { requestedActions.push_back(action); });
    QObject::connect(&content, &ContentCardWidget::screenshotRequested, &content,
                     [&screenshotRequests]() { ++screenshotRequests; });

    const QVector<QPair<QString, Action>> genericActions{
        {QStringLiteral("quick-screenshot-delay"), Action::ScreenshotDelay},
        {QStringLiteral("quick-screenshot-fixed"), Action::ScreenshotFixed},
        {QStringLiteral("quick-screenshot-ocr"), Action::ScreenshotOcr},
        {QStringLiteral("quick-screenshot-translation"), Action::ScreenshotTranslation},
        {QStringLiteral("quick-screenshot-copy"), Action::ScreenshotCopy},
        {QStringLiteral("quick-screenshot-full-screen"), Action::ScreenshotFullScreen},
        {QStringLiteral("quick-screenshot-focused-window"), Action::ScreenshotFocusedWindow},
        {QStringLiteral("quick-screen-record"), Action::ScreenRecord},
        {QStringLiteral("quick-screen-record-copy"), Action::ScreenRecordCopy},
        {QStringLiteral("quick-open-capture-history"), Action::OpenCaptureHistory},
        {QStringLiteral("quick-pin-clipboard-content"), Action::PinClipboardContent},
    };
    for (const auto& [objectId, action] : genericActions) {
        auto* row = content.findChild<ShortcutKeyRow*>(QStringLiteral("settings-item-") + objectId);
        require(row != nullptr, "every generic quick action must render a shortcut row");
        row->click();
        require(!requestedActions.isEmpty() && requestedActions.constLast() == action,
                "ContentCardWidget must emit the action encoded by each generic command");
    }
    require(requestedActions.size() == genericActions.size() && screenshotRequests == 0,
            "generic quick actions must use only the typed quick-action signal");

    auto* screenshot =
        content.findChild<ShortcutKeyRow*>(QStringLiteral("settings-item-quick-screenshot"));
    require(screenshot != nullptr, "the standard screenshot action must render");
    screenshot->click();
    require(screenshotRequests == 1 && requestedActions.size() == genericActions.size(),
            "the standard screenshot command must preserve its dedicated signal");

    require(content.findChild<ShortcutKeyRow*>(
                QStringLiteral("settings-item-quick-open-interface-settings")) == nullptr,
            "Open Interface Settings must not render in Quick Functions");
    content.showInterfaceSettings();
    require(content.currentLocation() ==
                    settings::SettingsLocation{
                        QStringLiteral("interface-settings"), QStringLiteral("general"), {}} &&
                requestedActions.size() == genericActions.size() && screenshotRequests == 1,
            "the external settings action must navigate without emitting execution signals");
}

void mainWindowIsDisposableConfigurationSurface() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    QPointer<MainWindow> window = new MainWindow(registry, session);
    require(window != nullptr && window->testAttribute(Qt::WA_DeleteOnClose),
            "the main interface must be a disposable configuration window");

    int screenshotRequests = 0;
    int historyEditRequests = 0;
    QObject::connect(window, &MainWindow::screenshotRequested,
                     [&screenshotRequests]() { ++screenshotRequests; });
    QObject::connect(window, &MainWindow::screenshotHistoryEditRequested,
                     [&historyEditRequests](const QString&) { ++historyEditRequests; });

    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(window == nullptr,
            "closing the main interface must destroy only the configuration window");
    require(backend.parent() == nullptr && session.parent() == nullptr &&
                screenshotRequests == 0 && historyEditRequests == 0,
            "destroying the main interface must not own or invoke background runtime services");

    QPointer<MainWindow> replacement = new MainWindow(registry, session);
    require(replacement != nullptr && replacement->testAttribute(Qt::WA_DeleteOnClose),
            "the configuration window must be recreatable after close");
    replacement->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(replacement == nullptr, "the recreated configuration window must be disposable");
}

void actionsMayExecuteWithoutConfirmation() {
    FakeSettingsBackend bindings;
    settings::SettingsActionDefinition action;
    action.buttonText = text("Run action");
    action.iconFactory = []() { return adqt::icons::antd::outlined::Rest(); };
    const settings::SettingsCatalog catalog(
        {{QStringLiteral("actions"),
          QStringLiteral("/actions"),
          text("Actions"),
          text("Actions page"),
          {{QStringLiteral("commands"),
            text("Commands"),
            text("Action commands"),
            settings::SettingsSectionReset::None,
            {{QStringLiteral("action.direct"),
              text("Direct action"),
              text("Execute immediately"),
              {},
              {},
              action}}}}}},
        {settings::SettingsNavigationPageDefinition{
            QStringLiteral("nav.actions"), QStringLiteral("actions"),
            []() { return adqt::icons::antd::outlined::Appstore(); }}},
        {QStringLiteral("actions"), QStringLiteral("commands"), QStringLiteral("action.direct")});
    require(catalog.validationErrors().isEmpty(),
            "an action without confirmation metadata must be valid");

    const auto registry = settings::SettingsRegistry::fromCatalog(
        catalog, QStringLiteral("actions-test"));
    settings::SettingsRuntimeSession session(registry, bindings);
    SettingsPageWidget page(registry, QStringLiteral("actions"), session);
    auto* button =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("settings-control-action-direct"));
    require(button != nullptr, "an action without confirmation must render");
    button->click();
    require(bindings.actionTriggered && page.findChild<adqt::widgets::AdModal*>() == nullptr,
            "an action without confirmation must execute directly");
}

void catalogExpansionUpdatesAllConsumers() {
    settings::SettingsCatalog catalog = expandedCatalog();
    require(catalog.validationErrors().isEmpty(), "expanded integration catalog must validate");
    const auto registry = settings::SettingsRegistry::fromCatalog(
        catalog, QStringLiteral("expanded-test"));

    FakeSettingsBackend bindings;
    settings::SettingsRuntimeSession session(registry, bindings);
    ContentCardWidget content(registry, session);
    SidebarWidget sidebar(registry);
    MainContentHeaderWidget header(
        registry,
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias);
    content.resize(720, 420);
    content.show();
    sidebar.show();
    header.show();
    flushEvents();

    auto* stack = content.findChild<QStackedWidget*>();
    auto* menu = sidebar.findChild<adqt::widgets::AdNavigationMenu*>();
    auto* search = header.findChild<ApplicationSearchWidget*>(QStringLiteral("globalTopSearchBar"));
    auto* searchSelect =
        search != nullptr ? search->findChild<adqt::widgets::AdSelect*>() : nullptr;
    const auto* defaultPage = catalog.page(catalog.defaultLocation().pageId);
    auto* activeDefaultPage = stack != nullptr
                                  ? dynamic_cast<SettingsPageWidget*>(stack->currentWidget())
                                  : nullptr;
    require(stack != nullptr && stack->count() == 1 && defaultPage != nullptr &&
                activeDefaultPage != nullptr && activeDefaultPage->pageId() == defaultPage->id,
            "route stack must mount only the default catalog page at startup");
    require(content.findChild<ScreenshotHistoryPageWidget*>(
                QStringLiteral("screenshotHistoryPage")) == nullptr,
            "main-content construction eagerly instantiated screenshot history");
    require(menu != nullptr && menu->model() != nullptr && menu->model()->rowCount() == 5,
            "sidebar must add a catalog navigation node automatically");
    require(searchSelect != nullptr && searchSelect->options().size() == 8,
            "application search must add every catalog page to its default results");

    content.setCurrentRoute(QStringLiteral("/history"));
    flushEvents();
    require(content.currentRoute() == QStringLiteral("/history") &&
                content.findChild<ScreenshotHistoryPageWidget*>(
                    QStringLiteral("screenshotHistoryPage")) != nullptr,
            "the custom screenshot history route must participate in the shared content stack");

    content.setCurrentRoute(QStringLiteral("/extra"));
    header.setSections(content.currentSections());
    header.setCurrentSection(content.currentLocation().sectionId);
    flushEvents();
    auto* tabs = header.findChild<adqt::widgets::AdTabs*>(QStringLiteral("mainSectionTabs"));
    require(content.currentRoute() == QStringLiteral("/extra") &&
                content.currentLocation() ==
                    settings::SettingsLocation{
                        QStringLiteral("extra-page"), QStringLiteral("extra-section"), {}} &&
                content.findChild<SettingsPageWidget*>(
                    QStringLiteral("settings-page-extra-page")) != nullptr &&
                content.findChild<adqt::widgets::AdSelect*>(
                    QStringLiteral("settings-control-extra-item")) != nullptr,
            "content routes, generated page, and item anchors must follow the expanded catalog");
    require(tabs != nullptr && tabs->count() == 1 &&
                tabs->tabKey(0) == QStringLiteral("extra-section"),
                "header tabs must follow the current generated page sections");
}

void contentCardStrictlyLazyLoadsAndDestroysRoutes() {
    const settings::SettingsCatalog catalog = expandedCatalog();
    const auto registry = settings::SettingsRegistry::fromCatalog(
        catalog, QStringLiteral("lazy-routing-test"));
    FakeSettingsBackend bindings;
    settings::SettingsRuntimeSession session(registry, bindings);
    ContentCardWidget content(registry, session);
    auto* stack = content.findChild<QStackedWidget*>();
    require(stack != nullptr && stack->count() == 1,
            "strict lazy routing must mount exactly one page at construction");

    const auto* defaultPage = catalog.page(catalog.defaultLocation().pageId);
    const settings::SettingsPageDefinition* secondPage = nullptr;
    const settings::SettingsPageDefinition* historyPageDefinition = nullptr;
    for (const auto& page : catalog.pages()) {
        if (page.kind == settings::SettingsPageKind::ScreenshotHistory) {
            historyPageDefinition = &page;
        } else if (secondPage == nullptr && defaultPage != nullptr && page.id != defaultPage->id) {
            secondPage = &page;
        }
    }
    require(defaultPage != nullptr && secondPage != nullptr && historyPageDefinition != nullptr,
            "lifecycle coverage requires default, generated, and history routes");

    auto* initialPage = dynamic_cast<SettingsPageWidget*>(stack->currentWidget());
    QPointer<SettingsPageWidget> initialPageGuard(initialPage);
    require(initialPage != nullptr && initialPage->pageId() == defaultPage->id,
            "the default route must create its generated page lazily");

    int routeChanges = 0;
    int sectionListChanges = 0;
    int locationChanges = 0;
    QObject::connect(&content, &ContentCardWidget::routeChanged, &content,
                     [&routeChanges](const QString&) { ++routeChanges; });
    QObject::connect(&content, &ContentCardWidget::sectionListChanged, &content,
                     [&sectionListChanges]() { ++sectionListChanges; });
    QObject::connect(&content, &ContentCardWidget::locationChanged, &content,
                     [&locationChanges](const settings::SettingsLocation&) {
                         ++locationChanges;
                     });

    content.setCurrentRoute(secondPage->route);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* secondPageWidget = dynamic_cast<SettingsPageWidget*>(stack->currentWidget());
    QPointer<SettingsPageWidget> secondPageGuard(secondPageWidget);
    require(initialPageGuard == nullptr && stack->count() == 1 && secondPageWidget != nullptr &&
                secondPageWidget->pageId() == secondPage->id,
            "changing routes must destroy the previous generated page and mount one new page");
    require(routeChanges == 1 && sectionListChanges == 1 && locationChanges == 1,
            "changing routes must preserve the existing routing signal contract");

    if (!secondPage->sections.isEmpty()) {
        content.activateSection(secondPage->sections.constFirst().id);
        require(secondPageGuard != nullptr && stack->currentWidget() == secondPageWidget,
                "section navigation within a route must reuse its active page");
        require(routeChanges == 1 && sectionListChanges == 1 && locationChanges == 2,
                "section navigation must update location without recreating the route");
    }

    content.setCurrentRoute(historyPageDefinition->route);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* historyWidget =
        dynamic_cast<ScreenshotHistoryPageWidget*>(stack->currentWidget());
    QPointer<ScreenshotHistoryPageWidget> historyGuard(historyWidget);
    if (historyWidget != nullptr) {
        // Keep the child-destruction assertion meaningful even when the
        // repository fixture has no records and the page creates no previews.
        new adqt::widgets::AdImage(historyWidget);
    }
    const auto historyImages = historyWidget != nullptr
                                  ? historyWidget->findChildren<adqt::widgets::AdImage*>()
                                  : QList<adqt::widgets::AdImage*>{};
    const QList<QPointer<adqt::widgets::AdImage>> historyImageGuards = [&historyImages]() {
        QList<QPointer<adqt::widgets::AdImage>> guards;
        for (auto* image : historyImages) {
            guards.push_back(image);
        }
        return guards;
    }();
    require(secondPageGuard == nullptr && stack->count() == 1 && historyWidget != nullptr,
            "activating history must destroy the generated page and create history on demand");
    require(routeChanges == 2 && sectionListChanges == 2 &&
                locationChanges == (secondPage->sections.isEmpty() ? 2 : 3),
            "history activation must emit one route transition and one location update");

    content.setCurrentRoute(secondPage->route);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* recreatedSecondPage = dynamic_cast<SettingsPageWidget*>(stack->currentWidget());
    require(historyGuard == nullptr && recreatedSecondPage != nullptr &&
                stack->count() == 1,
            "leaving history must destroy it and recreate generated pages on return");
    for (const auto& imageGuard : historyImageGuards) {
        require(imageGuard == nullptr,
                "leaving history must destroy every history AdImage child");
    }

    content.setCurrentRoute(historyPageDefinition->route);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* recreatedHistoryWidget =
        dynamic_cast<ScreenshotHistoryPageWidget*>(stack->currentWidget());
    require(historyGuard == nullptr && recreatedHistoryWidget != nullptr && stack->count() == 1,
            "returning to history must create a fresh page instance");
}

void pinToScreenShortcutSettingsRenderAndReset() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend bindings;
    settings::SettingsRuntimeSession session(registry, bindings);
    SettingsPageWidget hotkeyPage(registry, QStringLiteral("hotkey-settings"), session);
    hotkeyPage.resize(720, 480);
    hotkeyPage.show();
    flushEvents();

    auto* shortcutList = hotkeyPage.findChild<QWidget*>(
        QStringLiteral("settings-section-list-hotkey-settings-pin-to-screen-shortcuts"));
    const auto shortcutRows = shortcutList != nullptr
                                  ? shortcutList->findChildren<ShortcutKeyRow*>()
                                  : QList<ShortcutKeyRow*>{};
    auto* shortcutGrid = shortcutList != nullptr && shortcutList->layout() != nullptr &&
                                 shortcutList->layout()->count() == 1
                             ? qobject_cast<QGridLayout*>(
                                   shortcutList->layout()->itemAt(0)->layout())
                             : nullptr;
    require(shortcutRows.size() == 11 && shortcutGrid != nullptr &&
                shortcutGrid->count() == 11 && shortcutGrid->columnCount() == 2 &&
                shortcutGrid->rowCount() == 6,
            "Pin to Screen shortcut settings must render eleven actions in a two-column grid");

    for (auto it = pinToScreenShortcutDefaults().cbegin();
         it != pinToScreenShortcutDefaults().cend(); ++it) {
        require(bindings.applyLocalShortcuts(settings::SettingsLocalShortcutScope::PinToScreen,
                                             it.key(), {QStringLiteral("Alt+Q")}),
                "Pin to Screen shortcut fixtures must be mutable");
    }
    auto* shortcutHeader = hotkeyPage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-hotkey-settings-pin-to-screen-shortcuts"));
    require(shortcutHeader != nullptr &&
                QMetaObject::invokeMethod(shortcutHeader, "resetRequested",
                                          Qt::DirectConnection) &&
                bindings.resetRequested == settings::SettingsSectionReset::PinToScreenShortcuts,
            "Pin to Screen shortcut settings must expose their own reset action");
    for (auto it = pinToScreenShortcutDefaults().cbegin();
         it != pinToScreenShortcutDefaults().cend(); ++it) {
        require(bindings.localShortcuts(settings::SettingsLocalShortcutScope::PinToScreen,
                                        it.key()) == it.value(),
                "Pin to Screen shortcut reset must restore every default");
    }
}

void screenshotShortcutSettingsRenderAndReset() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend bindings;
    settings::SettingsRuntimeSession session(registry, bindings);
    SettingsPageWidget hotkeyPage(registry, QStringLiteral("hotkey-settings"), session);
    hotkeyPage.resize(720, 640);
    hotkeyPage.show();
    flushEvents();

    auto* screenshotList = hotkeyPage.findChild<QWidget*>(
        QStringLiteral("settings-section-list-hotkey-settings-screenshot-shortcuts"));
    auto* otherList = hotkeyPage.findChild<QWidget*>(
        QStringLiteral("settings-section-list-hotkey-settings-other-shortcuts"));
    const auto shortcutGrid = [](QWidget* list) {
        return list != nullptr && list->layout() != nullptr && list->layout()->count() == 1
                   ? qobject_cast<QGridLayout*>(list->layout()->itemAt(0)->layout())
                   : nullptr;
    };
    QGridLayout* screenshotGrid = shortcutGrid(screenshotList);
    QGridLayout* otherGrid = shortcutGrid(otherList);
    require(screenshotList != nullptr && screenshotGrid != nullptr &&
                screenshotList->findChildren<ShortcutKeyRow*>().size() == 18 &&
                screenshotGrid->count() == 18 && screenshotGrid->columnCount() == 2 &&
                screenshotGrid->rowCount() == 9 && otherList != nullptr &&
                otherGrid != nullptr && otherList->findChildren<ShortcutKeyRow*>().size() == 6 &&
                otherGrid->count() == 6 && otherGrid->columnCount() == 2 &&
                otherGrid->rowCount() == 3,
            "Screenshot and Other shortcut categories must render their complete two-column grids");

    const QStringList screenshotActions{
        QStringLiteral("pin_to_screen"),       QStringLiteral("video_recording"),
        QStringLiteral("scrolling_screenshot"), QStringLiteral("save_as_file"),
        QStringLiteral("cancel_screenshot"),   QStringLiteral("copy_to_clipboard"),
    };
    for (const QString& actionId : screenshotActions) {
        auto* row = hotkeyPage.findChild<ShortcutKeyRow*>(settings::generatedObjectName(
            QStringLiteral("settings-item"), QStringLiteral("screenshot-shortcut.") + actionId));
        require(row != nullptr && screenshotList->isAncestorOf(row),
                "requested screenshot shortcut action must render in the Screenshot category");
    }
    for (const QString& actionId : {QStringLiteral("undo"), QStringLiteral("redo")}) {
        auto* row = hotkeyPage.findChild<ShortcutKeyRow*>(settings::generatedObjectName(
            QStringLiteral("settings-item"), QStringLiteral("screenshot-shortcut.") + actionId));
        require(row != nullptr && otherList->isAncestorOf(row),
                "Undo and Redo must render in the Other shortcut category");
    }

    for (const QString& actionId : screenshotCategoryShortcutIds()) {
        require(bindings.applyLocalShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                             actionId, {QStringLiteral("Alt+Q")}),
                "Screenshot shortcut fixtures must be mutable");
    }
    for (const QString& actionId : otherShortcutIds()) {
        require(bindings.applyLocalShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                             actionId, {QStringLiteral("Alt+W")}),
                "Other shortcut fixtures must be mutable");
    }

    auto* screenshotHeader = hotkeyPage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-hotkey-settings-screenshot-shortcuts"));
    require(screenshotHeader != nullptr &&
                QMetaObject::invokeMethod(screenshotHeader, "resetRequested",
                                          Qt::DirectConnection) &&
                bindings.resetRequested ==
                    settings::SettingsSectionReset::ScreenshotEditorShortcuts,
            "Screenshot shortcut settings must expose their category reset action");
    for (const QString& actionId : screenshotCategoryShortcutIds()) {
        require(bindings.localShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                        actionId) == screenshotShortcutDefaults().value(actionId),
                "Screenshot shortcut reset must restore every category default");
    }
    for (const QString& actionId : otherShortcutIds()) {
        require(bindings.localShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                        actionId) == QStringList{QStringLiteral("Alt+W")},
                "Screenshot shortcut reset must preserve the Other category");
    }

    auto* otherHeader = hotkeyPage.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-hotkey-settings-other-shortcuts"));
    require(otherHeader != nullptr &&
                QMetaObject::invokeMethod(otherHeader, "resetRequested", Qt::DirectConnection) &&
                bindings.resetRequested ==
                    settings::SettingsSectionReset::ScreenshotOtherShortcuts,
            "Other shortcut settings must expose their category reset action");
    for (const QString& actionId : otherShortcutIds()) {
        require(bindings.localShortcuts(settings::SettingsLocalShortcutScope::Screenshot,
                                        actionId) == screenshotShortcutDefaults().value(actionId),
                "Other shortcut reset must restore recognition, Undo, and Redo defaults");
    }
}

void generatedSettingsPagesHaveNoSyntheticBottomSpace() {
    const auto& registry = settings::builtInSettingsRegistry();
    const auto& catalog = registry.catalog();
    FakeSettingsBackend bindings;
    settings::SettingsRuntimeSession session(registry, bindings);
    const QVector<QSize> pageSizes{{720, 260}, {520, 420}};

    for (const settings::SettingsPageDefinition& definition : catalog.pages()) {
        if (definition.kind != settings::SettingsPageKind::GeneratedSettings) {
            continue;
        }

        require(!definition.sections.isEmpty(),
                "every generated settings page must have at least one section");
        for (const QSize& pageSize : pageSizes) {
            SettingsPageWidget page(registry, definition.id, session);
            page.resize(pageSize);
            page.show();
            flushEvents();
            flushEvents();

            auto* container = page.findChild<PageContainerWidget*>(
                settings::generatedObjectName(QStringLiteral("settings-container"),
                                              definition.id));
            QWidget* content = container != nullptr ? container->contentWidget() : nullptr;
            QVBoxLayout* contentLayout = container != nullptr ? container->contentLayout() : nullptr;
            adqt::widgets::AdScrollArea* scrollArea =
                container != nullptr ? container->scrollArea() : nullptr;
            QWidget* lastSectionList = page.findChild<QWidget*>(settings::generatedObjectName(
                QStringLiteral("settings-section-list"),
                QStringLiteral("%1-%2")
                    .arg(definition.id, definition.sections.constLast().id)));
            require(content != nullptr && contentLayout != nullptr && scrollArea != nullptr &&
                        scrollArea->viewport() != nullptr &&
                        scrollArea->verticalScrollBar() != nullptr && lastSectionList != nullptr,
                    "generated page geometry test could not find its shared layout objects");

            contentLayout->activate();
            const int lastSectionBottom =
                lastSectionList->mapTo(content, QPoint(0, 0)).y() + lastSectionList->height();
            const int naturalContentHeight =
                lastSectionBottom + contentLayout->contentsMargins().bottom();
            const int trailingLayoutSlack = content->height() - naturalContentHeight;
            const int expectedScrollMaximum =
                std::max(0, content->height() - scrollArea->viewport()->height());
            QLayoutItem* finalLayoutItem =
                contentLayout->count() > 0 ? contentLayout->itemAt(contentLayout->count() - 1)
                                           : nullptr;
            const auto* syntheticTail = page.findChild<QWidget*>(
                settings::generatedObjectName(QStringLiteral("settings-section-scroll-space"),
                                              definition.id));

            require(syntheticTail == nullptr &&
                        contentLayout->count() ==
                            static_cast<int>(definition.sections.size()) * 2 &&
                        finalLayoutItem != nullptr &&
                        finalLayoutItem->widget() == lastSectionList && trailingLayoutSlack >= 0 &&
                        trailingLayoutSlack <= contentLayout->contentsMargins().bottom() &&
                        scrollArea->verticalScrollBar()->maximum() == expectedScrollMaximum,
                    "a generated settings page exposes scrollable space after its last section");
        }
    }
}

void generatedSettingsRowsUseTheirWidthAwareNaturalHeight() {
    const auto& registry = settings::builtInSettingsRegistry();
    const auto& catalog = registry.catalog();
    const auto* definition = catalog.page(QStringLiteral("storage-and-privacy"));
    require(definition != nullptr,
            "width-aware row geometry test requires the Storage and Privacy page");

    FakeSettingsBackend bindings;
    settings::SettingsRuntimeSession session(registry, bindings);
    SettingsPageWidget page(registry, definition->id, session);
    page.resize(720, 260);
    page.show();
    flushEvents();
    flushEvents();

    auto* pageContainer = page.findChild<PageContainerWidget*>(
        QStringLiteral("settings-container-storage-and-privacy"));
    QWidget* content = pageContainer != nullptr ? pageContainer->contentWidget() : nullptr;
    require(content != nullptr && content->hasHeightForWidth() &&
                content->height() == content->heightForWidth(content->width()),
            "fitted settings content must use its natural height at the viewport width");

    for (const settings::SettingsSectionDefinition& section : definition->sections) {
        if (section.itemLayout == settings::SettingsSectionItemLayout::TwoColumnGrid ||
            section.items.size() < 2) {
            continue;
        }

        auto* list = page.findChild<QWidget*>(settings::generatedObjectName(
            QStringLiteral("settings-section-list"),
            QStringLiteral("%1-%2").arg(definition->id, section.id)));
        auto* layout = list != nullptr ? qobject_cast<QVBoxLayout*>(list->layout()) : nullptr;
        require(layout != nullptr && layout->count() == section.items.size() &&
                    list->hasHeightForWidth() &&
                    list->height() == list->heightForWidth(list->width()),
                "generated settings sections must use their natural height at the fitted width");

        for (int itemIndex = 0; itemIndex < layout->count(); ++itemIndex) {
            QWidget* current = layout->itemAt(itemIndex)->widget();
            require(current != nullptr && current->hasHeightForWidth() &&
                        current->height() == current->heightForWidth(current->width()),
                    "generated setting rows must not retain a size hint from another width");
            if (itemIndex == 0) {
                continue;
            }
            QWidget* previous = layout->itemAt(itemIndex - 1)->widget();
            const int effectiveGap =
                current->geometry().top() - previous->geometry().bottom() - 1;
            require(effectiveGap == layout->spacing(),
                    "generated setting rows must keep the section's declared spacing");
        }
    }
}

void sectionTabsAndScrollingStaySynchronized() {
    const auto& registry = settings::builtInSettingsRegistry();
    const auto& catalog = registry.catalog();
    const auto* pageDefinition = catalog.page(QStringLiteral("storage-and-privacy"));
    require(pageDefinition != nullptr && !pageDefinition->sections.isEmpty(),
            "section navigation integration requires a non-empty storage page");
    const QString firstSectionId = pageDefinition->sections.constFirst().id;
    FakeSettingsBackend bindings;
    settings::SettingsRuntimeSession session(registry, bindings);
    ContentCardWidget content(registry, session);
    MainContentHeaderWidget header(
        registry,
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias);

    QObject::connect(&header, &MainContentHeaderWidget::sectionRequested, &content,
                     &ContentCardWidget::activateSection);
    QObject::connect(&content, &ContentCardWidget::sectionListChanged, &header,
                     [&content, &header]() { header.setSections(content.currentSections()); });
    QObject::connect(&content, &ContentCardWidget::locationChanged, &header,
                     [&header](const settings::SettingsLocation& location) {
                         header.setCurrentSection(location.sectionId);
                     });

    content.resize(720, 260);
    content.show();
    header.show();
    content.setCurrentRoute(QStringLiteral("/settings/storageAndPrivacy"));
    header.setSections(content.currentSections());
    header.setCurrentSection(content.currentLocation().sectionId);
    flushEvents();

    auto* tabs = header.findChild<adqt::widgets::AdTabs*>(QStringLiteral("mainSectionTabs"));
    auto* scrollArea = content.findChild<adqt::widgets::AdScrollArea*>(
        QStringLiteral("settings-scroll-storage-and-privacy"));
    auto* storageSection = content.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-storage-and-privacy-storage-status"));
    auto* storageSectionList = content.findChild<QWidget*>(
        QStringLiteral("settings-section-list-storage-and-privacy-storage-status"));
    require(tabs != nullptr && scrollArea != nullptr && storageSection != nullptr &&
                storageSectionList != nullptr &&
                scrollArea->contentWidget() != nullptr &&
                scrollArea->verticalScrollBar() != nullptr,
            "section navigation integration must expose tabs, anchors, and a scrollbar");

    QScrollBar* scrollBar = scrollArea->verticalScrollBar();
    QWidget* scrollContent = scrollArea->contentWidget();
    const QMargins contentMargins = scrollContent->layout()->contentsMargins();
    const int topInset = contentMargins.top();
    const int storageSectionTop =
        storageSection->mapTo(scrollContent, QPoint(0, 0)).y();
    const int storageSectionBottom =
        storageSectionList->mapTo(scrollContent, QPoint(0, 0)).y() +
        storageSectionList->height();
    const int naturalContentHeight = storageSectionBottom + contentMargins.bottom();
    const int trailingLayoutSlack = scrollContent->height() - naturalContentHeight;
    const int naturalScrollMaximum =
        std::max(0, scrollContent->height() - scrollArea->viewport()->height());
    const int requestedSectionPosition = storageSectionTop - topInset;
    const int reachableSectionPosition =
        std::min(requestedSectionPosition, naturalScrollMaximum);
    require(trailingLayoutSlack >= 0 && trailingLayoutSlack <= contentMargins.bottom() &&
                scrollBar->maximum() == naturalScrollMaximum,
            "section navigation must use the page's natural content height without a blank tail");

    tabs->tabClicked(QStringLiteral("storage-status"));
    flushEvents();
    require(scrollBar->value() == reachableSectionPosition,
            "clicking a tab must scroll its section to the nearest naturally reachable position");
    require(header.currentSection() == QStringLiteral("storage-status") &&
                content.currentLocation().sectionId == QStringLiteral("storage-status"),
            "tab navigation must keep the header and content location synchronized");

    scrollBar->setValue(0);
    flushEvents();
    require(header.currentSection() == firstSectionId &&
                content.currentLocation().sectionId == firstSectionId,
            "scrolling back to the first section must select its tab automatically");

    scrollBar->setValue(scrollBar->maximum());
    flushEvents();
    require(header.currentSection() == QStringLiteral("storage-status") &&
                content.currentLocation().sectionId == QStringLiteral("storage-status"),
            "scrolling to a later section must select its tab automatically");
}

void drawingToolbarEditorPersistsDropsAndRetainsRejectedChanges() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession runtime(registry, backend);
    const auto* toolbarField =
        registry.fieldForCustom(settings::SettingsCustomRenderer::DrawingToolbarEditor);
    require(toolbarField != nullptr, "the registry must expose the drawing toolbar field");
    DrawingToolbarEditorSettingsWidget editor(runtime);
    editor.resize(960, 320);
    editor.show();
    flushEvents();

    QWidget* surface =
        editor.findChild<QWidget*>(QStringLiteral("settings-drawing-toolbar-surface"));
    QWidget* hiddenZone =
        editor.findChild<QWidget*>(QStringLiteral("settings-drawing-toolbar-hidden-zone"));
    auto* hiddenTitle =
        editor.findChild<QLabel*>(QStringLiteral("settings-drawing-toolbar-hidden-title"));
    const auto drawingButtons = editor.findChildren<adqt::widgets::AdButton*>(
        QRegularExpression(QStringLiteral("^settings-drawing-toolbar-item-")));
    const auto metric =
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias;
    require(surface != nullptr && hiddenZone != nullptr && hiddenTitle != nullptr &&
                hiddenTitle->font().pixelSize() == metric.fontSizeLG &&
                hiddenTitle->font().weight() == QFont::DemiBold && drawingButtons.size() == 11 &&
                editor.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("settings-drawing-toolbar-item-highlighter")) != nullptr &&
                editor.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("settings-drawing-toolbar-item-pen-highlight")) == nullptr &&
                editor.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("settings-drawing-toolbar-item-rectangle-highlight")) == nullptr &&
                editor.findChildren<adqt::widgets::AdPopover*>().isEmpty(),
            "drawing toolbar settings should use themed hidden-title typography and expose one "
            "generic Highlighter Tool among eleven direct tools");

    const auto drop = [](QWidget* target, const QString& itemId, const QPointF& position) {
        QMimeData mimeData;
        mimeData.setData("application/x-snow-shot-toolbar-item", itemId.toUtf8());
        QDragEnterEvent enter(position.toPoint(), Qt::MoveAction, &mimeData, Qt::LeftButton,
                              Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QDropEvent event(position, Qt::MoveAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
        flushEvents();
        return event.isAccepted();
    };

    QWidget* shapePosition =
        editor.findChild<QWidget*>(QStringLiteral("settings-drawing-toolbar-position-0"));
    require(shapePosition != nullptr,
            "drawing toolbar editor should expose stable position widgets");
    const QPoint stackAboveShape =
        shapePosition->mapTo(surface, QPoint(shapePosition->width() / 2, 0));
    require(drop(surface, QStringLiteral("watermark"), stackAboveShape) &&
                runtime.toolbarLayout().positions.constFirst() ==
                    QStringList{QStringLiteral("watermark"), QStringLiteral("shape")},
            "dropping any drawing tool above another should stack it in that position");

    shapePosition =
        editor.findChild<QWidget*>(QStringLiteral("settings-drawing-toolbar-position-0"));
    require(shapePosition != nullptr && shapePosition->height() > 32,
            "stacked drawing tools should render as vertical direct buttons");
    const QPoint unstackAtEnd(surface->width() - 2, surface->height() - 20);
    require(drop(surface, QStringLiteral("watermark"), unstackAtEnd) &&
                runtime.toolbarLayout().positions.constFirst() ==
                    QStringList{QStringLiteral("shape")} &&
                runtime.toolbarLayout().positions.constLast() ==
                    QStringList{QStringLiteral("watermark")},
            "dropping a stacked tool beside the toolbar should unstack it into a position");

    require(drop(hiddenZone, QStringLiteral("watermark"), hiddenZone->rect().center()) &&
                runtime.toolbarLayout().hidden ==
                    QStringList{QStringLiteral("watermark")} &&
                std::none_of(runtime.toolbarLayout().positions.cbegin(),
                             runtime.toolbarLayout().positions.cend(),
                             [](const QStringList& position) {
                                 return position.contains(QStringLiteral("watermark"));
                             }),
            "dropping a visible tool into the hidden well should remove its toolbar position");
    auto* hiddenWatermark = editor.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("settings-drawing-toolbar-item-watermark"));
    require(hiddenWatermark != nullptr && hiddenWatermark->parentWidget() == hiddenZone &&
                hiddenWatermark->isVisibleTo(&editor),
            "hidden tools should remain directly visible and draggable in settings");

    require(drop(surface, QStringLiteral("watermark"), unstackAtEnd) &&
                runtime.toolbarLayout().hidden.isEmpty() &&
                runtime.toolbarLayout().positions.constLast() ==
                    QStringList{QStringLiteral("watermark")},
            "dragging a hidden tool back to the preview should restore its toolbar position");

    const snow_shot::storage::ScreenshotToolbarLayout accepted = runtime.toolbarLayout();
    backend.acceptWrites = false;
    require(drop(hiddenZone, QStringLiteral("watermark"), hiddenZone->rect().center()),
            "the hidden well must accept a toolbar drop before persistence is attempted");
    const snow_shot::storage::ScreenshotToolbarLayout rejectedDraft = runtime.toolbarLayout();
    const settings::SettingsFieldState rejectedState = runtime.state(toolbarField->id);
    require(backend.toolbarLayout() == accepted &&
                rejectedDraft.hidden == QStringList{QStringLiteral("watermark")} &&
                rejectedState.acceptedValue.value<
                    snow_shot::storage::ScreenshotToolbarLayout>() == accepted &&
                rejectedState.draftValue.value<
                    snow_shot::storage::ScreenshotToolbarLayout>() == rejectedDraft &&
                rejectedState.dirty && !rejectedState.busy &&
                rejectedState.phase == settings::SettingsWritePhase::Rejected &&
                hiddenWatermark->parentWidget() == hiddenZone,
            "a rejected toolbar write must retain one coherent draft in the session and editor");

    require(runtime.discard(toolbarField->id) && runtime.toolbarLayout() == accepted &&
                backend.toolbarLayout() == accepted &&
                hiddenWatermark->parentWidget() != hiddenZone &&
                runtime.state(toolbarField->id).phase == settings::SettingsWritePhase::Clean,
            "discarding the rejected toolbar draft must restore the accepted layout everywhere");
}
} // namespace

int main(int argc, char** argv) {
    bool drawingToolbarEditorOnly = false;
    bool screenshotHistoryOnly = false;
    bool settingsLayoutOnly = false;
    bool pinnedShortcutsOnly = false;
    bool screenshotShortcutsOnly = false;
    bool mainWindowLifecycleOnly = false;
    bool lazyRoutingOnly = false;
    bool registryGenerationOnly = false;
    for (int argumentIndex = 1; argumentIndex < argc; ++argumentIndex) {
        if (QString::fromLocal8Bit(argv[argumentIndex]) ==
            QStringLiteral("--drawing-toolbar-editor-only")) {
            drawingToolbarEditorOnly = true;
        } else if (QString::fromLocal8Bit(argv[argumentIndex]) ==
                   QStringLiteral("--screenshot-history-only")) {
            screenshotHistoryOnly = true;
        } else if (QString::fromLocal8Bit(argv[argumentIndex]) ==
                   QStringLiteral("--settings-layout-only")) {
            settingsLayoutOnly = true;
        } else if (QString::fromLocal8Bit(argv[argumentIndex]) ==
                   QStringLiteral("--pinned-shortcuts-only")) {
            pinnedShortcutsOnly = true;
        } else if (QString::fromLocal8Bit(argv[argumentIndex]) ==
                   QStringLiteral("--screenshot-shortcuts-only")) {
            screenshotShortcutsOnly = true;
        } else if (QString::fromLocal8Bit(argv[argumentIndex]) ==
                   QStringLiteral("--main-window-lifecycle-only")) {
            mainWindowLifecycleOnly = true;
        } else if (QString::fromLocal8Bit(argv[argumentIndex]) ==
                   QStringLiteral("--lazy-routing-only")) {
            lazyRoutingOnly = true;
        } else if (QString::fromLocal8Bit(argv[argumentIndex]) ==
                   QStringLiteral("--registry-generation-only")) {
            registryGenerationOnly = true;
        }
    }
#if defined(Q_OS_WIN)
    if (drawingToolbarEditorOnly || settingsLayoutOnly || pinnedShortcutsOnly ||
        screenshotShortcutsOnly || lazyRoutingOnly || registryGenerationOnly) {
        qunsetenv("QT_QPA_PLATFORM");
    } else if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#else
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("settings_page_tests"));
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "temporary storage directory must be available");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({storageDirectory.path(), storageDirectory.path(), 60000})
                .success,
            "application storage must initialize for generated page integration tests");
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);

    if (drawingToolbarEditorOnly) {
        drawingToolbarEditorPersistsDropsAndRetainsRejectedChanges();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (screenshotHistoryOnly) {
        screenshotHistoryLifecycleAndIdentityDiff();
        screenshotHistoryLateCallbacksAreIgnoredAfterReplacement();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (settingsLayoutOnly) {
        generatedSettingsPagesHaveNoSyntheticBottomSpace();
        generatedSettingsRowsUseTheirWidthAwareNaturalHeight();
        sectionTabsAndScrollingStaySynchronized();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (pinnedShortcutsOnly) {
        pinToScreenShortcutSettingsRenderAndReset();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (screenshotShortcutsOnly) {
        screenshotShortcutSettingsRenderAndReset();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (mainWindowLifecycleOnly) {
        mainWindowIsDisposableConfigurationSurface();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (lazyRoutingOnly) {
        contentCardStrictlyLazyLoadsAndDestroysRoutes();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (registryGenerationOnly) {
        registryPageGenerationUsesCompiledPlan();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    registryPageGenerationUsesCompiledPlan();
    generatedPagesRenderEveryItemTypeAndResynchronize();
    mainWindowIsDisposableConfigurationSurface();
    quickActionCommandsDispatchThroughContentCard();
    contentCardStrictlyLazyLoadsAndDestroysRoutes();
    actionsMayExecuteWithoutConfirmation();
    screenshotHistoryPageUsesRepositoryAndAntDesignComponents();
    screenshotHistorySurvivesSidebarWidthTransitions();
    screenshotHistoryLifecycleAndIdentityDiff();
    screenshotHistoryLateCallbacksAreIgnoredAfterReplacement();
    screenshotHistoryEmptyToPopulatedGeometryIsStable();
    catalogExpansionUpdatesAllConsumers();
    generatedSettingsPagesHaveNoSyntheticBottomSpace();
    generatedSettingsRowsUseTheirWidthAwareNaturalHeight();
    sectionTabsAndScrollingStaySynchronized();
    drawingToolbarEditorPersistsDropsAndRetainsRejectedChanges();

    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
