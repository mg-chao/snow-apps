#include "snow_shot/presentation/settings/settingsruntimebindings.h"
#include "snow_shot/presentation/settings/applicationpriority.h"
#include "snow_shot/presentation/settings/textrecognitionacceleration.h"
#include "snow_shot/platform/windows/autostartregistration.h"

#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QJsonArray>

namespace snow_shot::presentation::settings {
namespace {
QString themeModeValue(styles::ThemeMode mode) {
    switch (mode) {
    case styles::ThemeMode::Light:
        return QStringLiteral("light");
    case styles::ThemeMode::Dark:
        return QStringLiteral("dark");
    case styles::ThemeMode::FollowSystem:
    default:
        return QStringLiteral("system");
    }
}

styles::ThemeMode themeModeForValue(const QVariant& value) {
    const QString key = value.toString();
    if (key == QStringLiteral("light")) {
        return styles::ThemeMode::Light;
    }
    if (key == QStringLiteral("dark")) {
        return styles::ThemeMode::Dark;
    }
    return styles::ThemeMode::FollowSystem;
}

QStringList stringListDefault(const QString& key) {
    QStringList result;
    const QJsonArray values = storage::ConfigurationSchema::defaultValue(key).toArray();
    result.reserve(values.size());
    for (const QJsonValue& value : values) {
        result.push_back(value.toString());
    }
    return result;
}

QString localShortcutKey(SettingsLocalShortcutScope scope, const QString& shortcutId) {
    const QString prefix = scope == SettingsLocalShortcutScope::Screenshot
                               ? QStringLiteral("screenshot_shortcuts/")
                               : scope == SettingsLocalShortcutScope::Drawing
                                     ? QStringLiteral("drawing_shortcuts/")
                                     : QStringLiteral("pin_to_screen_shortcuts/");
    return prefix + shortcutId;
}

storage::CaptureHistoryPolicy defaultHistoryPolicy() {
    storage::CaptureHistoryPolicy policy;
    policy.enabled = storage::ConfigurationSchema::defaultValue(
                         QStringLiteral("capture_history/enabled"))
                         .toBool();
    policy.retentionDays = storage::ConfigurationSchema::defaultValue(
                               QStringLiteral("capture_history/retention_days"))
                               .toInt();
    policy.maxEntries = storage::ConfigurationSchema::defaultValue(
                            QStringLiteral("capture_history/max_entries"))
                            .toInt();
    policy.maxDiskMiB = storage::ConfigurationSchema::defaultValue(
                            QStringLiteral("capture_history/max_disk_mib"))
                            .toInt();
    return policy;
}

bool applyAutoStartAtBoot(bool enabled) {
    using snow_shot::platform::windows::AutoStartRegistration;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const bool previousConfiguredValue =
        configuration.value(QStringLiteral("system/auto_start_at_boot")).toBool();
    const auto previous = AutoStartRegistration::snapshot();
    if (!previous.valid || !AutoStartRegistration::setEnabled(enabled)) {
        return false;
    }
    if (storage::SystemSettings().setAutoStartAtBoot(enabled) &&
        configuration.flushNow().success) {
        return true;
    }
    static_cast<void>(AutoStartRegistration::restore(previous));
    if (configuration.value(QStringLiteral("system/auto_start_at_boot")).toBool() !=
        previousConfiguredValue) {
        static_cast<void>(configuration.setValue(
            QStringLiteral("system/auto_start_at_boot"), previousConfiguredValue));
        static_cast<void>(configuration.flushNow());
    }
    return false;
}
} // namespace

BuiltInSettingsRuntimeBindings::BuiltInSettingsRuntimeBindings(
    GlobalShortcutManager& shortcutManager, QObject* parent)
    : SettingsRuntimeBindings(parent), m_shortcutManager(shortcutManager) {
    auto& themeManager = styles::ThemeManager::instance();
    connect(&themeManager, &styles::ThemeManager::themeModeChanged, this,
            [this](styles::ThemeMode) { emit synchronized(); });

    auto& languageManager = LanguageManager::instance();
    connect(&languageManager, &LanguageManager::languageChanged, this,
            [this](const QString&, const QLocale&) { emit synchronized(); });
    connect(&languageManager, &LanguageManager::languageChangeFailed, this,
            [this](const QString&) { emit synchronized(); });

    connect(&m_shortcutManager, &GlobalShortcutManager::stateChanged, this,
            [this](GlobalShortcutAction action,
                   const GlobalShortcutRegistrationState& state) {
                emit shortcutStateChanged(action, state);
                emit synchronized();
            });

    auto& applicationStorage = storage::ApplicationStorage::instance();
    if (!applicationStorage.isInitialized()) {
        static_cast<void>(applicationStorage.initialize());
    }
    connect(&applicationStorage, &storage::ApplicationStorage::storageStatusChanged, this,
            [this](const storage::StorageStatus&) { emit synchronized(); });
    connect(&applicationStorage,
            &storage::ApplicationStorage::captureHistoryClearFinished, this,
            [this](bool, const QString&) { emit synchronized(); });
    connect(&applicationStorage, &storage::ApplicationStorage::smartSelectionChanged, this,
            [this](bool) { emit synchronized(); });
    connect(&applicationStorage.configuration(), &storage::ConfigurationStore::valueChanged,
            this, [this](const QString&, const QJsonValue&) { emit synchronized(); });
}

QVariant BuiltInSettingsRuntimeBindings::selectValue(SettingsSelectBinding binding) const {
    switch (binding) {
    case SettingsSelectBinding::Theme:
        return themeModeValue(styles::ThemeManager::instance().themeMode());
    case SettingsSelectBinding::Language:
        return LanguageManager::instance().languagePreference();
    case SettingsSelectBinding::ApplicationPriority: {
        auto& storage = storage::ApplicationStorage::instance();
        if (!storage.isInitialized()) {
            static_cast<void>(storage.initialize());
        }
        return storage.configuration().value(QStringLiteral("system/application_priority"))
            .toString();
    }
    case SettingsSelectBinding::Proxy:
        return storage::NetworkSettings().proxy();
    case SettingsSelectBinding::ScreenshotToolbarSize:
        return storage::ScreenshotUiSettings().toolbarSize();
    case SettingsSelectBinding::ColorPickerDisplayMode:
        return storage::ScreenshotUiSettings().colorPickerDisplayMode();
    case SettingsSelectBinding::ScreenshotOcrAction:
        return storage::ScreenshotSettings().autoExecuteAfterTextRecognition();
    case SettingsSelectBinding::ScreenshotDoubleClickAction:
        return storage::ScreenshotSettings().doubleClickAction();
    case SettingsSelectBinding::ScreenshotMiddleClickAction:
        return storage::ScreenshotSettings().middleMouseButtonAction();
    case SettingsSelectBinding::PinMouseWheelZoomMode:
        return storage::PinToScreenSettings().mouseWheelZoomMode();
    case SettingsSelectBinding::ScreenRecordingClarity:
        return storage::RecordingSettings().screenRecordingClarity();
    case SettingsSelectBinding::ScreenRecordingFrameRate:
        return storage::RecordingSettings().frameRate();
    case SettingsSelectBinding::AnimatedImageClarity:
        return storage::RecordingSettings().animatedImageClarity();
    case SettingsSelectBinding::AnimatedImageFrameRate:
        return storage::RecordingSettings().animatedImageFrameRate();
    case SettingsSelectBinding::AnimatedImageFormat:
        return storage::RecordingSettings().animatedImageFormat();
    case SettingsSelectBinding::ScreenRecordingEncoder:
        return storage::RecordingSettings().encoder();
    case SettingsSelectBinding::ScreenRecordingEncodingPreset:
        return storage::RecordingSettings().encodingPreset();
    case SettingsSelectBinding::ScreenshotImageFormat:
        return storage::ScreenshotSettings().imageFormat();
    case SettingsSelectBinding::TrayLeftClickAction:
        return storage::TraySettings().leftClickAction();
    }
    return {};
}

QVector<SettingsRuntimeOption>
BuiltInSettingsRuntimeBindings::dynamicSelectOptions(SettingsSelectBinding binding) const {
    QVector<SettingsRuntimeOption> result;
    if (binding != SettingsSelectBinding::Language) {
        return result;
    }
    const QList<LanguageCatalog> languages = LanguageManager::instance().availableLanguages();
    result.reserve(languages.size());
    for (const LanguageCatalog& language : languages) {
        result.push_back({language.localeName, language.nativeName});
    }
    return result;
}

bool BuiltInSettingsRuntimeBindings::applySelectValue(SettingsSelectBinding binding,
                                                      const QVariant& value) {
    switch (binding) {
    case SettingsSelectBinding::Theme: {
        const auto requested = themeModeForValue(value);
        styles::ThemeManager::instance().setThemeMode(requested);
        return styles::ThemeManager::instance().themeMode() == requested;
    }
    case SettingsSelectBinding::Language:
        return LanguageManager::instance().setLanguage(value.toString());
    case SettingsSelectBinding::ApplicationPriority: {
        const auto requested = applicationPriorityForValue(value.toString());
        if (!requested.has_value()) {
            return false;
        }
        auto& storage = storage::ApplicationStorage::instance();
        if (!storage.isInitialized()) {
            static_cast<void>(storage.initialize());
        }
        const auto previous = applicationPriorityForValue(
            storage.configuration().value(QStringLiteral("system/application_priority"))
                .toString());
        if (!applyApplicationPriority(*requested)) {
            return false;
        }
        const bool persisted = storage.configuration().setValue(
            QStringLiteral("system/application_priority"), applicationPriorityValue(*requested));
        if (persisted) {
            emit synchronized();
        } else if (previous.has_value()) {
            static_cast<void>(applyApplicationPriority(*previous));
        }
        return persisted;
    }
    case SettingsSelectBinding::Proxy:
        return storage::NetworkSettings().setProxy(value.toString());
    case SettingsSelectBinding::ScreenshotToolbarSize:
        return storage::ScreenshotUiSettings().setToolbarSize(value.toString());
    case SettingsSelectBinding::ColorPickerDisplayMode:
        return storage::ScreenshotUiSettings().setColorPickerDisplayMode(value.toString());
    case SettingsSelectBinding::ScreenshotOcrAction:
        return storage::ScreenshotSettings().setAutoExecuteAfterTextRecognition(value.toString());
    case SettingsSelectBinding::ScreenshotDoubleClickAction:
        return storage::ScreenshotSettings().setDoubleClickAction(value.toString());
    case SettingsSelectBinding::ScreenshotMiddleClickAction:
        return storage::ScreenshotSettings().setMiddleMouseButtonAction(value.toString());
    case SettingsSelectBinding::PinMouseWheelZoomMode:
        return storage::PinToScreenSettings().setMouseWheelZoomMode(value.toString());
    case SettingsSelectBinding::ScreenRecordingClarity:
        return storage::RecordingSettings().setScreenRecordingClarity(value.toString());
    case SettingsSelectBinding::ScreenRecordingFrameRate:
        return storage::RecordingSettings().setFrameRate(value.toInt());
    case SettingsSelectBinding::AnimatedImageClarity:
        return storage::RecordingSettings().setAnimatedImageClarity(value.toString());
    case SettingsSelectBinding::AnimatedImageFrameRate:
        return storage::RecordingSettings().setAnimatedImageFrameRate(value.toInt());
    case SettingsSelectBinding::AnimatedImageFormat:
        return storage::RecordingSettings().setAnimatedImageFormat(value.toString());
    case SettingsSelectBinding::ScreenRecordingEncoder:
        return storage::RecordingSettings().setEncoder(value.toString());
    case SettingsSelectBinding::ScreenRecordingEncodingPreset:
        return storage::RecordingSettings().setEncodingPreset(value.toString());
    case SettingsSelectBinding::ScreenshotImageFormat:
        return storage::ScreenshotSettings().setImageFormat(value.toString());
    case SettingsSelectBinding::TrayLeftClickAction:
        return storage::TraySettings().setLeftClickAction(value.toString());
    }
    return false;
}

bool BuiltInSettingsRuntimeBindings::switchValue(SettingsSwitchBinding binding) const {
    switch (binding) {
    case SettingsSwitchBinding::HistoryEnabled:
        return storage::ApplicationStorage::instance().captureHistoryPolicy().enabled;
    case SettingsSwitchBinding::SmartSelection:
        return storage::ApplicationStorage::instance().smartSelectionEnabled();
    case SettingsSwitchBinding::DirectMlAcceleration:
        return directMlTextRecognitionSupported() &&
               storage::ApplicationStorage::instance()
                   .configuration()
                   .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                   .toBool();
    case SettingsSwitchBinding::SelectionTransitionAnimation:
        return storage::ScreenshotUiSettings().selectionTransitionAnimationEnabled();
    case SettingsSwitchBinding::TrayEnabled:
        return storage::TraySettings().enabled();
    case SettingsSwitchBinding::ScreenshotAutoSaveAfterCopy:
        return storage::ScreenshotSettings().autoSaveAfterCopy();
    case SettingsSwitchBinding::ScreenshotCopyImageFileToClipboard:
        return storage::ScreenshotSettings().copyImageFileToClipboard();
    case SettingsSwitchBinding::PinAutomaticTextRecognition:
        return storage::PinToScreenSettings().automaticTextRecognition();
    case SettingsSwitchBinding::PinAutoResizeWindow:
        return storage::PinToScreenSettings().autoResizeWindow();
    case SettingsSwitchBinding::ScreenRecordingHideToolbar:
        return storage::RecordingSettings().hideToolbarInRecording();
    case SettingsSwitchBinding::DisableHotkeysOnFocusedFullscreen:
        return storage::GlobalShortcutSettings().disableOnFocusedFullscreenWindow();
    case SettingsSwitchBinding::AutoStartAtBoot:
        return storage::SystemSettings().autoStartAtBoot();
    }
    return false;
}

bool BuiltInSettingsRuntimeBindings::switchEnabled(SettingsSwitchBinding binding) const {
    if (binding == SettingsSwitchBinding::AutoStartAtBoot) {
        return snow_shot::platform::windows::AutoStartRegistration::isSupported();
    }
    return binding != SettingsSwitchBinding::DirectMlAcceleration ||
           directMlTextRecognitionSupported();
}

bool BuiltInSettingsRuntimeBindings::applySwitchValue(SettingsSwitchBinding binding, bool value) {
    if (binding == SettingsSwitchBinding::SmartSelection) {
        return storage::ApplicationStorage::instance().requestSmartSelection(value);
    }
    if (binding == SettingsSwitchBinding::DirectMlAcceleration) {
        if (!directMlTextRecognitionSupported()) {
            return false;
        }
        auto& storage = storage::ApplicationStorage::instance();
        const bool accepted = storage.configuration().setValue(
            QStringLiteral("text_recognition/direct_ml_acceleration"), value);
        if (accepted) {
            emit synchronized();
        }
        return accepted;
    }

    if (binding == SettingsSwitchBinding::SelectionTransitionAnimation) {
        return storage::ScreenshotUiSettings().setSelectionTransitionAnimationEnabled(value);
    }
    if (binding == SettingsSwitchBinding::TrayEnabled) {
        return storage::TraySettings().setEnabled(value);
    }
    if (binding == SettingsSwitchBinding::ScreenshotAutoSaveAfterCopy) {
        return storage::ScreenshotSettings().setAutoSaveAfterCopy(value);
    }
    if (binding == SettingsSwitchBinding::ScreenshotCopyImageFileToClipboard) {
        return storage::ScreenshotSettings().setCopyImageFileToClipboard(value);
    }
    if (binding == SettingsSwitchBinding::PinAutomaticTextRecognition) {
        return storage::PinToScreenSettings().setAutomaticTextRecognition(value);
    }
    if (binding == SettingsSwitchBinding::PinAutoResizeWindow) {
        return storage::PinToScreenSettings().setAutoResizeWindow(value);
    }
    if (binding == SettingsSwitchBinding::ScreenRecordingHideToolbar) {
        return storage::RecordingSettings().setHideToolbarInRecording(value);
    }
    if (binding == SettingsSwitchBinding::DisableHotkeysOnFocusedFullscreen) {
        return storage::GlobalShortcutSettings().setDisableOnFocusedFullscreenWindow(value);
    }
    if (binding == SettingsSwitchBinding::AutoStartAtBoot) {
        return applyAutoStartAtBoot(value);
    }

    auto policy = storage::ApplicationStorage::instance().captureHistoryPolicy();
    switch (binding) {
    case SettingsSwitchBinding::HistoryEnabled:
        policy.enabled = value;
        break;
    case SettingsSwitchBinding::SmartSelection:
        return false;
    case SettingsSwitchBinding::DirectMlAcceleration:
        return false;
    case SettingsSwitchBinding::SelectionTransitionAnimation:
    case SettingsSwitchBinding::TrayEnabled:
    case SettingsSwitchBinding::ScreenshotAutoSaveAfterCopy:
    case SettingsSwitchBinding::ScreenshotCopyImageFileToClipboard:
    case SettingsSwitchBinding::PinAutomaticTextRecognition:
    case SettingsSwitchBinding::PinAutoResizeWindow:
    case SettingsSwitchBinding::ScreenRecordingHideToolbar:
    case SettingsSwitchBinding::DisableHotkeysOnFocusedFullscreen:
    case SettingsSwitchBinding::AutoStartAtBoot:
        return false;
    }
    return storage::ApplicationStorage::instance().requestCaptureHistoryPolicy(policy);
}

int BuiltInSettingsRuntimeBindings::integerValue(SettingsIntegerBinding binding) const {
    const storage::CaptureHistoryPolicy policy =
        storage::ApplicationStorage::instance().captureHistoryPolicy();
    switch (binding) {
    case SettingsIntegerBinding::HistoryRetentionDays:
        return policy.retentionDays;
    case SettingsIntegerBinding::HistoryMaxEntries:
        return policy.maxEntries;
    case SettingsIntegerBinding::HistoryMaxDiskMiB:
        return policy.maxDiskMiB;
    case SettingsIntegerBinding::ScreenshotDelaySeconds:
        return storage::ScreenshotSettings().delaySeconds();
    }
    return 0;
}

bool BuiltInSettingsRuntimeBindings::applyIntegerValue(SettingsIntegerBinding binding, int value) {
    auto policy = storage::ApplicationStorage::instance().captureHistoryPolicy();
    switch (binding) {
    case SettingsIntegerBinding::HistoryRetentionDays:
        policy.retentionDays = value;
        break;
    case SettingsIntegerBinding::HistoryMaxEntries:
        policy.maxEntries = value;
        break;
    case SettingsIntegerBinding::HistoryMaxDiskMiB:
        policy.maxDiskMiB = value;
        break;
    case SettingsIntegerBinding::ScreenshotDelaySeconds: {
        const auto* schema = storage::ConfigurationSchema::entry(
            QStringLiteral("screenshot/delay_seconds"));
        if (schema == nullptr || !schema->integerRange.has_value() ||
            value < schema->integerRange->minimum || value > schema->integerRange->maximum) {
            return false;
        }
        const bool accepted = storage::ScreenshotSettings().setDelaySeconds(value);
        if (accepted) {
            emit synchronized();
        }
        return accepted;
    }
    }
    return storage::ApplicationStorage::instance().requestCaptureHistoryPolicy(policy);
}

QVariantList BuiltInSettingsRuntimeBindings::multiSelectValue(
    SettingsMultiSelectBinding binding) const {
    QVariantList values;
    if (binding == SettingsMultiSelectBinding::DrawingQuickSelectionDisabledTools) {
        for (const QString& value : storage::DrawingSettings().quickSelectionDisabledTools()) {
            values.push_back(value);
        }
    } else if (binding == SettingsMultiSelectBinding::TrayMenuOptions) {
        for (const QString& value : storage::TraySettings().menuOptions()) {
            values.push_back(value);
        }
    }
    return values;
}

bool BuiltInSettingsRuntimeBindings::applyMultiSelectValue(
    SettingsMultiSelectBinding binding, const QVariantList& value) {
    QStringList tools;
    tools.reserve(value.size());
    for (const QVariant& item : value) {
        tools.push_back(item.toString());
    }
    if (binding == SettingsMultiSelectBinding::DrawingQuickSelectionDisabledTools) {
        return storage::DrawingSettings().setQuickSelectionDisabledTools(tools);
    }
    if (binding == SettingsMultiSelectBinding::TrayMenuOptions) {
        return storage::TraySettings().setMenuOptions(tools);
    }
    return false;
}

int BuiltInSettingsRuntimeBindings::sliderValue(SettingsSliderBinding binding) const {
    switch (binding) {
    case SettingsSliderBinding::ShortcutHintOpacity:
        return storage::ScreenshotUiSettings().shortcutHintOpacity();
    }
    return 0;
}

bool BuiltInSettingsRuntimeBindings::applySliderValue(SettingsSliderBinding binding, int value) {
    switch (binding) {
    case SettingsSliderBinding::ShortcutHintOpacity:
        return storage::ScreenshotUiSettings().setShortcutHintOpacity(value);
    }
    return false;
}

QColor BuiltInSettingsRuntimeBindings::colorValue(SettingsColorBinding binding) const {
    const storage::ScreenshotUiSettings screenshot;
    switch (binding) {
    case SettingsColorBinding::SelectionMaskColor:
        return screenshot.selectionMaskColor();
    case SettingsColorBinding::CursorGuideLineColor:
        return screenshot.cursorGuideLineColor();
    case SettingsColorBinding::MonitorCenterGuideLineColor:
        return screenshot.monitorCenterGuideLineColor();
    case SettingsColorBinding::ColorPickerCenterGuideLineColor:
        return screenshot.colorPickerCenterGuideLineColor();
    case SettingsColorBinding::PinBorderColor:
        return storage::PinToScreenSettings().borderColor();
    }
    return {};
}

bool BuiltInSettingsRuntimeBindings::applyColorValue(SettingsColorBinding binding,
                                                      const QColor& value) {
    const storage::ScreenshotUiSettings screenshot;
    switch (binding) {
    case SettingsColorBinding::SelectionMaskColor:
        return screenshot.setSelectionMaskColor(value);
    case SettingsColorBinding::CursorGuideLineColor:
        return screenshot.setCursorGuideLineColor(value);
    case SettingsColorBinding::MonitorCenterGuideLineColor:
        return screenshot.setMonitorCenterGuideLineColor(value);
    case SettingsColorBinding::ColorPickerCenterGuideLineColor:
        return screenshot.setColorPickerCenterGuideLineColor(value);
    case SettingsColorBinding::PinBorderColor:
        return storage::PinToScreenSettings().setBorderColor(value);
    }
    return false;
}

QVariant BuiltInSettingsRuntimeBindings::radioValue(SettingsRadioBinding binding) const {
    switch (binding) {
    case SettingsRadioBinding::TrayIcon:
        return storage::TraySettings().icon();
    }
    return {};
}

bool BuiltInSettingsRuntimeBindings::applyRadioValue(SettingsRadioBinding binding,
                                                      const QVariant& value) {
    switch (binding) {
    case SettingsRadioBinding::TrayIcon:
        return storage::TraySettings().setIcon(value.toString());
    }
    return false;
}

QString BuiltInSettingsRuntimeBindings::filePathValue(SettingsFilePathBinding binding) const {
    switch (binding) {
    case SettingsFilePathBinding::TrayCustomIcon:
        return storage::TraySettings().customIcon();
    }
    return {};
}

bool BuiltInSettingsRuntimeBindings::applyFilePathValue(SettingsFilePathBinding binding,
                                                         const QString& value) {
    switch (binding) {
    case SettingsFilePathBinding::TrayCustomIcon:
        return storage::TraySettings().setCustomIcon(value);
    }
    return false;
}

QString BuiltInSettingsRuntimeBindings::directoryPathValue(
    SettingsDirectoryPathBinding binding) const {
    switch (binding) {
    case SettingsDirectoryPathBinding::ScreenshotImageDirectory:
        return storage::ScreenshotSettings().imageSaveDirectory();
    case SettingsDirectoryPathBinding::ScreenRecordingVideoDirectory:
        return storage::RecordingSettings().videoSaveDirectory();
    }
    return {};
}

bool BuiltInSettingsRuntimeBindings::applyDirectoryPathValue(
    SettingsDirectoryPathBinding binding, const QString& value) {
    switch (binding) {
    case SettingsDirectoryPathBinding::ScreenshotImageDirectory:
        return storage::ScreenshotSettings().setImageSaveDirectory(value);
    case SettingsDirectoryPathBinding::ScreenRecordingVideoDirectory:
        return storage::RecordingSettings().setVideoSaveDirectory(value);
    }
    return false;
}

QString BuiltInSettingsRuntimeBindings::textValue(SettingsTextBinding binding) const {
    switch (binding) {
    case SettingsTextBinding::ScreenshotManualFilenameFormat:
        return storage::ScreenshotSettings().manualSaveFilenameFormat();
    case SettingsTextBinding::ScreenshotAutoFilenameFormat:
        return storage::ScreenshotSettings().autoSaveFilenameFormat();
    case SettingsTextBinding::ScreenRecordingVideoFilenameFormat:
        return storage::RecordingSettings().videoFilenameFormat();
    }
    return {};
}

bool BuiltInSettingsRuntimeBindings::applyTextValue(SettingsTextBinding binding,
                                                    const QString& value) {
    switch (binding) {
    case SettingsTextBinding::ScreenshotManualFilenameFormat:
        return storage::ScreenshotSettings().setManualSaveFilenameFormat(value);
    case SettingsTextBinding::ScreenshotAutoFilenameFormat:
        return storage::ScreenshotSettings().setAutoSaveFilenameFormat(value);
    case SettingsTextBinding::ScreenRecordingVideoFilenameFormat:
        return storage::RecordingSettings().setVideoFilenameFormat(value);
    }
    return false;
}

storage::ScreenshotToolbarLayout BuiltInSettingsRuntimeBindings::toolbarLayout() const {
    return storage::ScreenshotToolbarSettings().layout();
}

bool BuiltInSettingsRuntimeBindings::applyToolbarLayout(
    const storage::ScreenshotToolbarLayout& layout) {
    return storage::ScreenshotToolbarSettings().setLayout(layout);
}

GlobalShortcutRegistrationState
BuiltInSettingsRuntimeBindings::shortcutState(GlobalShortcutAction action) const {
    return m_shortcutManager.state(action);
}

GlobalShortcutValidationResult
BuiltInSettingsRuntimeBindings::validateShortcut(const QString& shortcut) const {
    return m_shortcutManager.validateShortcut(shortcut);
}

bool BuiltInSettingsRuntimeBindings::applyShortcuts(GlobalShortcutAction action,
                                                   const QStringList& shortcuts) {
    m_shortcutManager.setShortcuts(action, shortcuts);
    return m_shortcutManager.state(action).shortcuts == shortcuts;
}

QStringList BuiltInSettingsRuntimeBindings::localShortcuts(SettingsLocalShortcutScope scope,
                                                            const QString& shortcutId) const {
    if (scope == SettingsLocalShortcutScope::Screenshot) {
        return storage::ScreenshotShortcutSettings().shortcuts(shortcutId);
    }
    if (scope == SettingsLocalShortcutScope::Drawing) {
        return storage::DrawingShortcutSettings().shortcuts(shortcutId);
    }
    return storage::PinToScreenShortcutSettings().shortcuts(shortcutId);
}

GlobalShortcutValidationResult BuiltInSettingsRuntimeBindings::validateLocalShortcut(
    SettingsLocalShortcutScope scope, const QString& shortcutId, const QString& shortcut) const {
    const QString key = localShortcutKey(scope, shortcutId);
    const storage::ConfigurationNormalization normalized = storage::ConfigurationSchema::normalize(
        key, QJsonArray{shortcut});
    if (!normalized.valid || normalized.value.toArray().isEmpty()) {
        return {shortcut, false, GlobalShortcutFailureReason::InvalidShortcut};
    }
    const QString canonical = normalized.value.toArray().first().toString();
    if (scope != SettingsLocalShortcutScope::PinToScreen &&
        storage::ScreenshotShortcutSettings::isReservedShortcut(canonical) &&
        (scope != SettingsLocalShortcutScope::Screenshot ||
         !storage::ScreenshotShortcutSettings::isReservedShortcutAllowed(shortcutId, canonical))) {
        return {canonical, false, GlobalShortcutFailureReason::InvalidShortcut};
    }
    const auto all = scope == SettingsLocalShortcutScope::Screenshot
                         ? storage::ScreenshotShortcutSettings().allShortcuts()
                         : scope == SettingsLocalShortcutScope::Drawing
                               ? storage::DrawingShortcutSettings().allShortcuts()
                               : storage::PinToScreenShortcutSettings().allShortcuts();
    for (auto it = all.cbegin(); it != all.cend(); ++it) {
        if (it.key() == shortcutId) {
            continue;
        }
        for (const QString& existing : it.value()) {
            if (existing.compare(canonical, Qt::CaseInsensitive) == 0) {
                return {canonical, false, GlobalShortcutFailureReason::AlreadyInUse};
            }
        }
    }
    return {canonical, true, GlobalShortcutFailureReason::None};
}

bool BuiltInSettingsRuntimeBindings::applyLocalShortcuts(
    SettingsLocalShortcutScope scope, const QString& shortcutId, const QStringList& shortcuts) {
    for (const QString& shortcut : shortcuts) {
        const GlobalShortcutValidationResult validation =
            validateLocalShortcut(scope, shortcutId, shortcut);
        if (!validation.supported) {
            return false;
        }
    }
    if (scope == SettingsLocalShortcutScope::Screenshot) {
        return storage::ScreenshotShortcutSettings().setShortcuts(shortcutId, shortcuts);
    }
    if (scope == SettingsLocalShortcutScope::Drawing) {
        return storage::DrawingShortcutSettings().setShortcuts(shortcutId, shortcuts);
    }
    return storage::PinToScreenShortcutSettings().setShortcuts(shortcutId, shortcuts);
}

SettingsActionState
BuiltInSettingsRuntimeBindings::actionState(SettingsActionBinding binding) const {
    const storage::StorageStatus status = storage::ApplicationStorage::instance().status();
    switch (binding) {
    case SettingsActionBinding::ClearCaptureHistory:
        return {
            status.writeAvailable && !status.historyClearing &&
                (status.historyUsage.entryCount > 0 || status.historyUsage.totalBytes > 0),
            status.historyClearing,
        };
    }
    return {};
}

bool BuiltInSettingsRuntimeBindings::triggerAction(SettingsActionBinding binding) {
    switch (binding) {
    case SettingsActionBinding::ClearCaptureHistory:
        return storage::ApplicationStorage::instance().requestCaptureHistoryClear();
    }
    return false;
}

storage::StorageStatus BuiltInSettingsRuntimeBindings::storageStatus() const {
    return storage::ApplicationStorage::instance().status();
}

bool BuiltInSettingsRuntimeBindings::resetSection(SettingsSectionReset reset) {
    switch (reset) {
    case SettingsSectionReset::ScreenshotShortcuts: {
        bool accepted = true;
        const auto resetShortcut = [this, &accepted](GlobalShortcutAction action,
                                                      const QString& key) {
            accepted = applyShortcuts(action, stringListDefault(key)) && accepted;
        };
        resetShortcut(GlobalShortcutAction::Screenshot,
                      QStringLiteral("global_shortcuts/screenshot"));
        resetShortcut(GlobalShortcutAction::ScreenshotDelay,
                      QStringLiteral("global_shortcuts/screenshot_delay"));
        resetShortcut(GlobalShortcutAction::ScreenshotFixed,
                      QStringLiteral("global_shortcuts/screenshot_fixed"));
        resetShortcut(GlobalShortcutAction::ScreenshotOcr,
                      QStringLiteral("global_shortcuts/screenshot_ocr"));
        resetShortcut(GlobalShortcutAction::ScreenshotTranslation,
                      QStringLiteral("global_shortcuts/screenshot_translation"));
        resetShortcut(GlobalShortcutAction::ScreenshotCopy,
                      QStringLiteral("global_shortcuts/screenshot_copy"));
        resetShortcut(GlobalShortcutAction::ScreenshotFullScreen,
                      QStringLiteral("global_shortcuts/screenshot_full_screen"));
        resetShortcut(GlobalShortcutAction::ScreenshotFocusedWindow,
                      QStringLiteral("global_shortcuts/screenshot_focused_window"));
        accepted = applyIntegerValue(
                       SettingsIntegerBinding::ScreenshotDelaySeconds,
                       storage::ConfigurationSchema::defaultValue(
                           QStringLiteral("screenshot/delay_seconds"))
                           .toInt()) &&
                   accepted;
        return accepted;
    }
    case SettingsSectionReset::OtherShortcuts: {
        bool accepted = true;
        const auto resetShortcut = [this, &accepted](GlobalShortcutAction action,
                                                      const QString& key) {
            accepted = applyShortcuts(action, stringListDefault(key)) && accepted;
        };
        resetShortcut(GlobalShortcutAction::OpenCaptureHistory,
                      QStringLiteral("global_shortcuts/open_capture_history"));
        resetShortcut(GlobalShortcutAction::PinClipboardContent,
                      QStringLiteral("global_shortcuts/pin_clipboard_content"));
        return accepted;
    }
    case SettingsSectionReset::GeneralSettings: {
        const bool themeAccepted = applySelectValue(
            SettingsSelectBinding::Theme,
            storage::ConfigurationSchema::defaultValue(QStringLiteral("interface/theme_mode")));
        const bool languageAccepted = applySelectValue(
            SettingsSelectBinding::Language,
            storage::ConfigurationSchema::defaultValue(QStringLiteral("interface/language")));
        return themeAccepted && languageAccepted;
    }
    case SettingsSectionReset::HistoryPolicy:
        return storage::ApplicationStorage::instance().requestCaptureHistoryPolicy(
            defaultHistoryPolicy());
    case SettingsSectionReset::ScreenshotSettings:
        return storage::ApplicationStorage::instance().requestSmartSelection(
                   storage::ConfigurationSchema::defaultValue(
                       QStringLiteral("screenshot_selection/smart_selection"))
                       .toBool()) &&
               storage::ApplicationStorage::instance().configuration().setValues({
                   {QStringLiteral("screenshot/auto_execute_after_text_recognition"),
                    storage::ConfigurationSchema::defaultValue(
                        QStringLiteral("screenshot/auto_execute_after_text_recognition"))},
                   {QStringLiteral("screenshot/double_click_action"),
                    storage::ConfigurationSchema::defaultValue(
                        QStringLiteral("screenshot/double_click_action"))},
                   {QStringLiteral("screenshot/middle_mouse_button_action"),
                    storage::ConfigurationSchema::defaultValue(
                        QStringLiteral("screenshot/middle_mouse_button_action"))},
                   {QStringLiteral("screenshot/auto_save_after_copy"),
                    storage::ConfigurationSchema::defaultValue(
                        QStringLiteral("screenshot/auto_save_after_copy"))},
                   {QStringLiteral("screenshot/copy_image_file_to_clipboard"),
                    storage::ConfigurationSchema::defaultValue(
                        QStringLiteral("screenshot/copy_image_file_to_clipboard"))},
               });
    case SettingsSectionReset::ScreenshotOutput:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("screenshot/image_save_directory"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot/image_save_directory"))},
            {QStringLiteral("screenshot/image_format"),
             storage::ConfigurationSchema::defaultValue(QStringLiteral("screenshot/image_format"))},
            {QStringLiteral("screenshot/manual_save_filename_format"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot/manual_save_filename_format"))},
            {QStringLiteral("screenshot/auto_save_filename_format"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot/auto_save_filename_format"))},
        });
    case SettingsSectionReset::ScreenshotInterfaceSettings:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("screenshot_ui/selection_transition_animation"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot_ui/selection_transition_animation"))},
            {QStringLiteral("screenshot_ui/color_picker_display_mode"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot_ui/color_picker_display_mode"))},
            {QStringLiteral("screenshot_ui/selection_mask_color"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot_ui/selection_mask_color"))},
            {QStringLiteral("screenshot_ui/shortcut_hint_opacity"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot_ui/shortcut_hint_opacity"))},
            {QStringLiteral("screenshot_ui/cursor_guide_line_color"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot_ui/cursor_guide_line_color"))},
            {QStringLiteral("screenshot_ui/monitor_center_guide_line_color"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot_ui/monitor_center_guide_line_color"))},
            {QStringLiteral("screenshot_ui/color_picker_center_guide_line_color"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot_ui/color_picker_center_guide_line_color"))},
        });
    case SettingsSectionReset::Toolbar:
        return storage::ApplicationStorage::instance().configuration().setValue(
            QStringLiteral("screenshot_ui/toolbar_size"),
            storage::ConfigurationSchema::defaultValue(
                QStringLiteral("screenshot_ui/toolbar_size")));
    case SettingsSectionReset::DrawingToolbar:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("screenshot_toolbar/layout"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screenshot_toolbar/layout"))},
        });
    case SettingsSectionReset::DrawingQuickSelection:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("drawing/quick_selection_disabled_tools"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("drawing/quick_selection_disabled_tools"))},
        });
    case SettingsSectionReset::ScreenshotEditorShortcuts: {
        QMap<QString, QStringList> defaults;
        for (const QString& actionId : {QStringLiteral("move_tool"),
                                        QStringLiteral("move_cursor_up"),
                                        QStringLiteral("move_cursor_down"),
                                        QStringLiteral("move_cursor_left"),
                                        QStringLiteral("move_cursor_right"),
                                        QStringLiteral("move_entire_selection"),
                                        QStringLiteral("keep_selection_width_and_height_consistent"),
                                        QStringLiteral(
                                            "switch_selection_between_window_and_window_sub_element"),
                                        QStringLiteral("previous_screenshot_history"),
                                        QStringLiteral("next_screenshot_history"),
                                        QStringLiteral("select_previously_selected_area"),
                                        QStringLiteral("copy_color"),
                                        QStringLiteral("pin_to_screen"),
                                        QStringLiteral("video_recording"),
                                        QStringLiteral("scrolling_screenshot"),
                                        QStringLiteral("save_as_file"),
                                        QStringLiteral("cancel_screenshot"),
                                        QStringLiteral("copy_to_clipboard")}) {
            defaults.insert(
                actionId,
                stringListDefault(QStringLiteral("screenshot_shortcuts/") + actionId));
        }
        QMap<QString, QStringList> all = storage::ScreenshotShortcutSettings().allShortcuts();
        for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
            all.insert(it.key(), it.value());
        }
        return storage::ScreenshotShortcutSettings().setAllShortcutsAtomic(all);
    }
    case SettingsSectionReset::ScreenshotOtherShortcuts: {
        QMap<QString, QStringList> defaults;
        for (const QString& actionId : {QStringLiteral("table_recognition"),
                                        QStringLiteral("qr_code_recognition"),
                                        QStringLiteral("text_recognition"),
                                        QStringLiteral("text_translation"),
                                        QStringLiteral("undo"),
                                        QStringLiteral("redo")}) {
            defaults.insert(
                actionId,
                stringListDefault(QStringLiteral("screenshot_shortcuts/") + actionId));
        }
        QMap<QString, QStringList> all = storage::ScreenshotShortcutSettings().allShortcuts();
        for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
            all.insert(it.key(), it.value());
        }
        return storage::ScreenshotShortcutSettings().setAllShortcutsAtomic(all);
    }
    case SettingsSectionReset::DrawingShortcuts: {
        QMap<QString, QStringList> defaults;
        for (const QString& toolId : {QStringLiteral("select"), QStringLiteral("shape"),
                                     QStringLiteral("arrow"),
                                     QStringLiteral("brush"), QStringLiteral("highlight"),
                                     QStringLiteral("text"), QStringLiteral("serial_number"),
                                     QStringLiteral("filter"), QStringLiteral("eraser"),
                                     QStringLiteral("watermark")}) {
            defaults.insert(toolId,
                            stringListDefault(QStringLiteral("drawing_shortcuts/") + toolId));
        }
        return storage::DrawingShortcutSettings().setAllShortcutsAtomic(defaults);
    }
    case SettingsSectionReset::PinToScreenShortcuts: {
        QMap<QString, QStringList> defaults;
        for (const QString& actionId : {QStringLiteral("copy_to_clipboard"),
                                        QStringLiteral("copy_original_content"),
                                        QStringLiteral("show_text_recognition_results"),
                                        QStringLiteral("drawing_mode"),
                                        QStringLiteral("thumbnail_mode"),
                                        QStringLiteral("close_window"),
                                        QStringLiteral("move_cursor_up"),
                                        QStringLiteral("move_cursor_down"),
                                        QStringLiteral("move_cursor_left"),
                                        QStringLiteral("move_cursor_right")}) {
            defaults.insert(actionId, stringListDefault(QStringLiteral("pin_to_screen_shortcuts/") +
                                                        actionId));
        }
        return storage::PinToScreenShortcutSettings().setAllShortcutsAtomic(defaults);
    }
    case SettingsSectionReset::PinToScreen:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("pin_to_screen/border_color"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("pin_to_screen/border_color"))},
        });
    case SettingsSectionReset::PinToScreenBehavior:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode"))},
            {QStringLiteral("pin_to_screen/automatic_text_recognition"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("pin_to_screen/automatic_text_recognition"))},
            {QStringLiteral("pin_to_screen/auto_resize_window"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("pin_to_screen/auto_resize_window"))},
        });
    case SettingsSectionReset::Tray:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("tray/enabled"),
             storage::ConfigurationSchema::defaultValue(QStringLiteral("tray/enabled"))},
            {QStringLiteral("tray/icon"),
             storage::ConfigurationSchema::defaultValue(QStringLiteral("tray/icon"))},
            {QStringLiteral("tray/custom_icon"),
             storage::ConfigurationSchema::defaultValue(QStringLiteral("tray/custom_icon"))},
        });
    case SettingsSectionReset::TrayBehavior:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("tray/left_click_action"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("tray/left_click_action"))},
            {QStringLiteral("tray/menu_options"),
             storage::ConfigurationSchema::defaultValue(QStringLiteral("tray/menu_options"))},
        });
    case SettingsSectionReset::ScreenRecording:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("screen_recording/clarity"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/clarity"))},
            {QStringLiteral("screen_recording/frame_rate"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/frame_rate"))},
            {QStringLiteral("screen_recording/animated_image_clarity"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/animated_image_clarity"))},
            {QStringLiteral("screen_recording/animated_image_frame_rate"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/animated_image_frame_rate"))},
            {QStringLiteral("screen_recording/animated_image_format"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/animated_image_format"))},
            {QStringLiteral("screen_recording/encoder"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/encoder"))},
            {QStringLiteral("screen_recording/encoding_preset"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/encoding_preset"))},
            {QStringLiteral("screen_recording/hide_toolbar_in_recording"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/hide_toolbar_in_recording"))},
        });
    case SettingsSectionReset::ScreenRecordingOutput:
        return storage::ApplicationStorage::instance().configuration().setValues({
            {QStringLiteral("screen_recording/video_save_directory"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/video_save_directory"))},
            {QStringLiteral("screen_recording/video_filename_format"),
             storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("screen_recording/video_filename_format"))},
        });
    case SettingsSectionReset::GlobalHotkeys:
        return storage::GlobalShortcutSettings().setDisableOnFocusedFullscreenWindow(
            storage::ConfigurationSchema::defaultValue(
                QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window"))
                .toBool());
    case SettingsSectionReset::SystemGeneral:
        return applyAutoStartAtBoot(
            storage::ConfigurationSchema::defaultValue(
                QStringLiteral("system/auto_start_at_boot"))
                .toBool());
    case SettingsSectionReset::Network:
        return applySelectValue(
            SettingsSelectBinding::Proxy,
            storage::ConfigurationSchema::defaultValue(QStringLiteral("network/proxy")));
    case SettingsSectionReset::SystemSettings: {
        auto& storage = storage::ApplicationStorage::instance();
        if (!storage.isInitialized()) {
            static_cast<void>(storage.initialize());
        }
        const auto previous = applicationPriorityForValue(
            storage.configuration().value(QStringLiteral("system/application_priority"))
                .toString());
        const auto priority = applicationPriorityForValue(
            storage::ConfigurationSchema::defaultValue(
                QStringLiteral("system/application_priority"))
                .toString());
        const bool accepted =
            priority.has_value() && applyApplicationPriority(*priority) &&
            storage.configuration().setValue(
                QStringLiteral("system/application_priority"), applicationPriorityValue(*priority));
        if (accepted) {
            emit synchronized();
        } else if (previous.has_value()) {
            static_cast<void>(applyApplicationPriority(*previous));
        }
        return accepted;
    }
    case SettingsSectionReset::TextRecognition: {
        auto& storage = storage::ApplicationStorage::instance();
        const bool accepted = storage.configuration().setValue(
            QStringLiteral("text_recognition/direct_ml_acceleration"),
            storage::ConfigurationSchema::defaultValue(
                QStringLiteral("text_recognition/direct_ml_acceleration")));
        if (accepted) {
            emit synchronized();
        }
        return accepted;
    }
    case SettingsSectionReset::None:
        return true;
    }
    return false;
}

} // namespace snow_shot::presentation::settings
