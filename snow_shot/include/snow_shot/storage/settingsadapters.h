#ifndef SNOW_SHOT_STORAGE_SETTINGSADAPTERS_H
#define SNOW_SHOT_STORAGE_SETTINGSADAPTERS_H

#include "snow_shot/app/edition.h"
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
#include "snow_shot/customaimodelconfiguration.h"
#include "snow_shot/texttranslationconfiguration.h"
#endif
#include "snow_shot/shortcuts/shortcutbinding.h"
#include "snow_shot/storage/persistedwindowgeometry.h"

#include <QColor>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMetaType>

#include <optional>

namespace snow_shot::storage {
#if SNOW_SHOT_ENABLE_EXTENDED_FEATURES
class ExtendedFeaturesSettings final {
  public:
    [[nodiscard]] bool translationPageEnabled() const;
    bool setTranslationPageEnabled(bool enabled) const;
    [[nodiscard]] bool jumpToTranslationPage() const;
    bool setJumpToTranslationPage(bool enabled) const;
    [[nodiscard]] bool standaloneTranslationWindow() const;
    bool setStandaloneTranslationWindow(bool enabled) const;
};
#endif

class TextRecognitionSettings final {
  public:
    [[nodiscard]] bool saveRecognitionResultAsImage() const;
    bool setSaveRecognitionResultAsImage(bool enabled) const;
    [[nodiscard]] bool showOriginalImagePreview() const;
    bool setShowOriginalImagePreview(bool enabled) const;
    [[nodiscard]] QString defaultFormatting() const;
    bool setDefaultFormatting(const QString& value) const;
    [[nodiscard]] QString defaultPunctuation() const;
    bool setDefaultPunctuation(const QString& value) const;
};

struct ScreenshotSavePathShortcut {
    QString name;
    QString path;
    friend bool operator==(const ScreenshotSavePathShortcut&,
                           const ScreenshotSavePathShortcut&) = default;
};

struct ScreenshotToolbarLayout {
    QVector<QStringList> positions;
    QStringList hidden;

    friend bool operator==(const ScreenshotToolbarLayout& first,
                           const ScreenshotToolbarLayout& second) {
        return first.positions == second.positions && first.hidden == second.hidden;
    }
    friend bool operator!=(const ScreenshotToolbarLayout& first,
                           const ScreenshotToolbarLayout& second) {
        return !(first == second);
    }
};

enum class ScreenshotToolbarLayoutKind {
    PinnedActionTools,
    DrawingTools,
    ActionTools,
    FloatingTools,
};

[[nodiscard]] QColor colorFromRgbaString(const QString& value);
[[nodiscard]] QString colorToRgbaString(const QColor& color);

#if SNOW_SHOT_ENABLE_API_CONFIGURATION
class ApiConfigurationSettings final {
  public:
    [[nodiscard]] QString serverUrl() const;
    bool setServerUrl(const QString& value) const;
    [[nodiscard]] CustomAiModels customModels() const;
    bool setCustomModels(const CustomAiModels& models) const;
    [[nodiscard]] TextTranslationConfigurations textTranslationConfigurations() const;
    bool setTextTranslationConfigurations(const TextTranslationConfigurations& values) const;
};
#endif

class InterfaceSettings final {
  public:
    [[nodiscard]] QColor themePrimaryColor() const;
    bool setThemePrimaryColor(const QColor& color) const;
    [[nodiscard]] QString appFontFamily() const;
    bool setAppFontFamily(const QString& family) const;
    [[nodiscard]] int appFontSizePercentage() const;
    bool setAppFontSizePercentage(int percentage) const;
    [[nodiscard]] QString themeMode() const;
    bool setThemeMode(const QString& mode) const;
    [[nodiscard]] QString language() const;
    bool setLanguage(const QString& language) const;
    [[nodiscard]] QString skinPath() const;
    bool setSkinPath(const QString& path) const;
    [[nodiscard]] QString skinPosition() const;
    bool setSkinPosition(const QString& position) const;
    [[nodiscard]] QString toolbarSkinPath() const;
    bool setToolbarSkinPath(const QString& path) const;
    [[nodiscard]] QString toolbarSkinPosition() const;
    bool setToolbarSkinPosition(const QString& position) const;
    [[nodiscard]] QString trayMenuSkinPath() const;
    bool setTrayMenuSkinPath(const QString& path) const;
    [[nodiscard]] QString trayMenuSkinPosition() const;
    bool setTrayMenuSkinPosition(const QString& position) const;
    [[nodiscard]] QString skinDisplayMode() const;
    bool setSkinDisplayMode(const QString& mode) const;
    [[nodiscard]] int skinOpacity() const;
    bool setSkinOpacity(int opacity) const;
    [[nodiscard]] int skinBlurLevel() const;
    bool setSkinBlurLevel(int level) const;
    [[nodiscard]] int skinMaskOpacity() const;
    bool setSkinMaskOpacity(int opacity) const;
    [[nodiscard]] bool sidebarCollapsed() const;
    bool setSidebarCollapsed(bool collapsed) const;
};

class WindowMemorySettings final {
  public:
    [[nodiscard]] std::optional<PersistedWindowGeometry> mainWindowGeometry() const;
    bool setMainWindowGeometry(const QRect& normalGeometry, bool maximized) const;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    [[nodiscard]] std::optional<QSize> translationWindowSize() const;
    bool setTranslationWindowSize(const QSize& size) const;
#endif
};

class ShortcutSettings final {
  public:
    [[nodiscard]] shortcuts::ShortcutBindingList screenshot() const;
    bool setScreenshot(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList screenshotDelay() const;
    bool setScreenshotDelay(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList screenshotFixed() const;
    bool setScreenshotFixed(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList screenshotOcr() const;
    bool setScreenshotOcr(const shortcuts::ShortcutBindingList& bindings) const;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    [[nodiscard]] shortcuts::ShortcutBindingList screenshotTranslation() const;
    bool setScreenshotTranslation(const shortcuts::ShortcutBindingList& bindings) const;
#endif

    [[nodiscard]] shortcuts::ShortcutBindingList screenshotCopy() const;
    bool setScreenshotCopy(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList screenshotFullScreen() const;
    bool setScreenshotFullScreen(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList screenshotFocusedWindow() const;
    bool setScreenshotFocusedWindow(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList screenRecord() const;
    bool setScreenRecord(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList screenRecordCopy() const;
    bool setScreenRecordCopy(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList openScreenRecordingFolder() const;
    bool setOpenScreenRecordingFolder(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList openCaptureHistory() const;
    bool setOpenCaptureHistory(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList switchWindowGroup() const;
    bool setSwitchWindowGroup(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList globalCanvas() const;
    bool setGlobalCanvas(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList openPinToScreenManagement() const;
    bool setOpenPinToScreenManagement(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList openSettings() const;
    bool setOpenSettings(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList pinClipboardContent() const;
    bool setPinClipboardContent(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList pinSelectedFiles() const;
    bool setPinSelectedFiles(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList restoreLastClosedWindows() const;
    bool setRestoreLastClosedWindows(const shortcuts::ShortcutBindingList& bindings) const;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    [[nodiscard]] shortcuts::ShortcutBindingList translateSelectedText() const;
    bool setTranslateSelectedText(const shortcuts::ShortcutBindingList& bindings) const;
#endif

    [[nodiscard]] shortcuts::ShortcutBindingList toggleGlobalHotkeys() const;
    bool setToggleGlobalHotkeys(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList toggleDisableOnFocusedFullscreenWindow() const;
    bool
    setToggleDisableOnFocusedFullscreenWindow(const shortcuts::ShortcutBindingList& bindings) const;
};

class GlobalShortcutSettings final {
  public:
    [[nodiscard]] bool disableOnFocusedFullscreenWindow() const;
    bool setDisableOnFocusedFullscreenWindow(bool disabled) const;
};

class ScreenshotSettings final {
  public:
    [[nodiscard]] bool shutterSoundNotification() const;
    bool setShutterSoundNotification(bool enabled) const;
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    [[nodiscard]] bool autoRecognizeQrCode() const;
    bool setAutoRecognizeQrCode(bool enabled) const;
#endif
    [[nodiscard]] bool confirmBeforeExitingViaShortcut() const;
    bool setConfirmBeforeExitingViaShortcut(bool enabled) const;
    [[nodiscard]] bool showCursor() const;
    bool setShowCursor(bool enabled) const;
    [[nodiscard]] int scrollingAutoScrollIntervalMs() const;
    bool setScrollingAutoScrollIntervalMs(int milliseconds) const;
    [[nodiscard]] bool captureUiInScrollingScreenshot() const;
    bool setCaptureUiInScrollingScreenshot(bool enabled) const;
    [[nodiscard]] bool restoreOriginalScreenColors() const;
    bool setRestoreOriginalScreenColors(bool enabled) const;
    [[nodiscard]] QString apiMode() const;
    bool setApiMode(const QString& mode) const;
    [[nodiscard]] QString windowElementApi() const;
    bool setWindowElementApi(const QString& api) const;
    [[nodiscard]] int delaySeconds() const;
    bool setDelaySeconds(int seconds) const;
    [[nodiscard]] QString autoExecuteAfterTextRecognition() const;
    bool setAutoExecuteAfterTextRecognition(const QString& action) const;
    [[nodiscard]] QString doubleClickAction() const;
    bool setDoubleClickAction(const QString& action) const;
    [[nodiscard]] QString middleMouseButtonAction() const;
    bool setMiddleMouseButtonAction(const QString& action) const;
    [[nodiscard]] bool quickSelectionModification() const;
    bool setQuickSelectionModification(bool enabled) const;
    [[nodiscard]] QString selectionResizeMode() const;
    bool setSelectionResizeMode(const QString& mode) const;
    [[nodiscard]] bool autoSaveAfterCopy() const;
    bool setAutoSaveAfterCopy(bool enabled) const;
    [[nodiscard]] bool copyImageFileToClipboard() const;
    bool setCopyImageFileToClipboard(bool enabled) const;
    [[nodiscard]] QString imageSaveDirectory() const;
    bool setImageSaveDirectory(const QString& directory) const;
    [[nodiscard]] QString lastManualSaveDirectory() const;
    bool setLastManualSaveDirectory(const QString& directory) const;
    [[nodiscard]] QString lastManualSaveFormat() const;
    bool setLastManualSaveFormat(const QString& format) const;
    [[nodiscard]] QString saveAsFileDialog() const;
    bool setSaveAsFileDialog(const QString& dialog) const;
    [[nodiscard]] QVector<ScreenshotSavePathShortcut> savePathShortcuts() const;
    bool setSavePathShortcuts(const QVector<ScreenshotSavePathShortcut>& shortcuts) const;
    [[nodiscard]] QString pdfPageSize() const;
    bool setPdfPageSize(const QString& pageSize) const;
    [[nodiscard]] QString imageFormat() const;
    bool setImageFormat(const QString& format) const;
    [[nodiscard]] QString compressionLevel() const;
    bool setCompressionLevel(const QString& level) const;
    [[nodiscard]] int imageQuality() const;
    bool setImageQuality(int quality) const;
    [[nodiscard]] QJsonObject manualSaveFormatOptions() const;
    bool setManualSaveFormatOptions(const QJsonObject& options) const;
    bool setLastManualSaveState(const QString& format, const QJsonObject& options) const;
    [[nodiscard]] QString manualSaveFilenameFormat() const;
    bool setManualSaveFilenameFormat(const QString& format) const;
    [[nodiscard]] QString autoSaveFilenameFormat() const;
    bool setAutoSaveFilenameFormat(const QString& format) const;
};

class DrawingSettings final {
  public:
    [[nodiscard]] QStringList quickSelectionDisabledTools() const;
    bool setQuickSelectionDisabledTools(const QStringList& tools) const;
    [[nodiscard]] bool rememberLastUsedTool() const;
    bool setRememberLastUsedTool(bool enabled) const;
};

// Applies partial updates across local shortcut scopes in one commit. Each affected
// scope is validated against its complete resulting assignment before any values change.
[[nodiscard]] bool setLocalShortcutValuesAtomic(const QMap<QString, QJsonValue>& values);

class ScreenshotShortcutSettings final {
  public:
    [[nodiscard]] static bool isReservedShortcut(const shortcuts::ShortcutBinding& shortcut);
    [[nodiscard]] static bool isReservedShortcutAllowed(const QString& actionId,
                                                        const shortcuts::ShortcutBinding& shortcut);

    [[nodiscard]] shortcuts::ShortcutBindingList moveTool() const;
    bool setMoveTool(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList moveCursorUp() const;
    bool setMoveCursorUp(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList moveCursorDown() const;
    [[nodiscard]] shortcuts::ShortcutBindingList moveCursorLeft() const;
    [[nodiscard]] shortcuts::ShortcutBindingList moveCursorRight() const;
    bool setMoveCursorRight(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList moveEntireSelection() const;
    [[nodiscard]] shortcuts::ShortcutBindingList keepSelectionWidthAndHeightConsistent() const;
    bool
    setKeepSelectionWidthAndHeightConsistent(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList
    switchSelectionBetweenWindowAndWindowSubElement() const;
    [[nodiscard]] shortcuts::ShortcutBindingList previousScreenshotHistory() const;
    [[nodiscard]] shortcuts::ShortcutBindingList nextScreenshotHistory() const;
    [[nodiscard]] shortcuts::ShortcutBindingList selectPreviouslySelectedArea() const;
    [[nodiscard]] shortcuts::ShortcutBindingList recapture() const;
    [[nodiscard]] shortcuts::ShortcutBindingList copyColor() const;
    [[nodiscard]] shortcuts::ShortcutBindingList toggleCoordinateMode() const;
    [[nodiscard]] shortcuts::ShortcutBindingList toggleGuides() const;

    [[nodiscard]] shortcuts::ShortcutBindingList shortcuts(const QString& actionId) const;
    bool setShortcuts(const QString& actionId,
                      const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingMap allShortcuts() const;
    bool setAllShortcutsAtomic(const shortcuts::ShortcutBindingMap& shortcutsByAction) const;
};

class DrawingShortcutSettings final {
  public:
    [[nodiscard]] static bool isReservedShortcut(const shortcuts::ShortcutBinding& shortcut);

    [[nodiscard]] shortcuts::ShortcutBindingList select() const;
    bool setSelect(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList shape() const;
    bool setShape(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList arrow() const;
    bool setArrow(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList watermark() const;
    bool setWatermark(const shortcuts::ShortcutBindingList& bindings) const;

    [[nodiscard]] shortcuts::ShortcutBindingList shortcuts(const QString& toolId) const;
    bool setShortcuts(const QString& toolId, const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingMap allShortcuts() const;
    bool setAllShortcutsAtomic(const shortcuts::ShortcutBindingMap& shortcutsByTool) const;
};

class PinToScreenShortcutSettings final {
  public:
    [[nodiscard]] shortcuts::ShortcutBindingList shortcuts(const QString& actionId) const;
    bool setShortcuts(const QString& actionId,
                      const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingMap allShortcuts() const;
    bool setAllShortcutsAtomic(const shortcuts::ShortcutBindingMap& shortcutsByAction) const;
};

class ScreenRecordingShortcutSettings final {
  public:
    [[nodiscard]] shortcuts::ShortcutBindingList shortcuts(const QString& actionId) const;
    bool setShortcuts(const QString& actionId,
                      const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingMap allShortcuts() const;
    bool setAllShortcutsAtomic(const shortcuts::ShortcutBindingMap& shortcutsByAction) const;
};

struct ScreenshotTranslationConfiguration {
    QString sourceLanguage;
    QString targetLanguage;
    QString modelId;
    QString layoutProcessing = QStringLiteral("smart_merge");
    QString secondaryTargetLanguage = QStringLiteral("en");

    friend bool operator==(const ScreenshotTranslationConfiguration& first,
                           const ScreenshotTranslationConfiguration& second) = default;
};

#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
class ScreenshotImageConversionSettings final {
  public:
    [[nodiscard]] QString visionModel() const;
    bool setVisionModel(const QString& model) const;
};
#endif

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
class ScreenshotTranslationSettings final {
  public:
    [[nodiscard]] bool originalImageTranslationEnabled() const;
    bool setOriginalImageTranslationEnabled(bool enabled) const;
    [[nodiscard]] QString layoutProcessing() const;
    bool setLayoutProcessing(const QString& mode) const;
    [[nodiscard]] ScreenshotTranslationConfiguration configuration() const;
    bool setConfiguration(const ScreenshotTranslationConfiguration& configuration) const;
};
#endif

class ScreenshotUiSettings final {
  public:
    [[nodiscard]] QString toolbarSize() const;
    bool setToolbarSize(const QString& size) const;
    [[nodiscard]] bool selectionTransitionAnimationEnabled() const;
    bool setSelectionTransitionAnimationEnabled(bool enabled) const;
    [[nodiscard]] QString selectionDisplayUnit() const;
    bool setSelectionDisplayUnit(const QString& unit) const;
    [[nodiscard]] bool selectionToolbarHidden() const;
    bool setSelectionToolbarHidden(bool hidden) const;
    [[nodiscard]] QString colorPickerDisplayMode() const;
    bool setColorPickerDisplayMode(const QString& mode) const;
    [[nodiscard]] QString colorPickerCoordinateMode() const;
    bool setColorPickerCoordinateMode(const QString& mode) const;
    [[nodiscard]] QString colorPickerFormat() const;
    bool setColorPickerFormat(const QString& format) const;
    [[nodiscard]] QColor selectionBorderColor() const;
    bool setSelectionBorderColor(const QColor& color) const;
    [[nodiscard]] QColor selectionMaskColor() const;
    bool setSelectionMaskColor(const QColor& color) const;
    [[nodiscard]] int shortcutHintOpacity() const;
    bool setShortcutHintOpacity(int opacity) const;
    [[nodiscard]] bool screenshotAreaTypeHintEnabled() const;
    bool setScreenshotAreaTypeHintEnabled(bool enabled) const;
    [[nodiscard]] bool showGuidesByDefault() const;
    bool setShowGuidesByDefault(bool enabled) const;
    [[nodiscard]] QColor cursorGuideLineColor() const;
    bool setCursorGuideLineColor(const QColor& color) const;
    [[nodiscard]] QColor selectionCenterGuideLineColor() const;
    bool setSelectionCenterGuideLineColor(const QColor& color) const;
    [[nodiscard]] QColor monitorCenterGuideLineColor() const;
    bool setMonitorCenterGuideLineColor(const QColor& color) const;
    [[nodiscard]] QColor colorPickerCenterGuideLineColor() const;
    bool setColorPickerCenterGuideLineColor(const QColor& color) const;
    [[nodiscard]] bool showEditSelectionToolbar() const;
    bool setShowEditSelectionToolbar(bool show) const;
};

class RecordingSettings final {
  public:
    [[nodiscard]] QString apiMode() const;
    bool setApiMode(const QString& mode) const;
    [[nodiscard]] bool microphoneEnabled() const;
    bool setMicrophoneEnabled(bool enabled) const;
    [[nodiscard]] bool systemAudioEnabled() const;
    bool setSystemAudioEnabled(bool enabled) const;
    [[nodiscard]] int microphoneGainDb() const;
    bool setMicrophoneGainDb(int gainDb) const;
    [[nodiscard]] int systemAudioGainDb() const;
    bool setSystemAudioGainDb(int gainDb) const;
    [[nodiscard]] QString screenRecordingClarity() const;
    bool setScreenRecordingClarity(const QString& clarity) const;
    [[nodiscard]] int frameRate() const;
    bool setFrameRate(int frameRate) const;
    [[nodiscard]] QString animatedImageClarity() const;
    bool setAnimatedImageClarity(const QString& clarity) const;
    [[nodiscard]] int animatedImageFrameRate() const;
    bool setAnimatedImageFrameRate(int frameRate) const;
    [[nodiscard]] bool separateAudioTracks() const;
    bool setSeparateAudioTracks(bool enabled) const;
    [[nodiscard]] bool loopAnimatedImages() const;
    bool setLoopAnimatedImages(bool enabled) const;
    [[nodiscard]] QString outputFormat() const;
    bool setOutputFormat(const QString& format) const;
    [[nodiscard]] bool postProcessingEnabled() const;
    bool setPostProcessingEnabled(bool enabled) const;
    [[nodiscard]] QString postProcessingEffect() const;
    bool setPostProcessingEffect(const QString& effect) const;
    [[nodiscard]] QColor progressBarColor() const;
    bool setProgressBarColor(const QColor& color) const;
    [[nodiscard]] int mouseTrailDurationMs() const;
    bool setMouseTrailDurationMs(int duration) const;
    [[nodiscard]] int keyboardSize() const;
    bool setKeyboardSize(int size) const;
    [[nodiscard]] QColor keyboardBackgroundColor() const;
    bool setKeyboardBackgroundColor(const QColor& color) const;
    [[nodiscard]] QColor keyboardForegroundColor() const;
    bool setKeyboardForegroundColor(const QColor& color) const;
    [[nodiscard]] QColor mouseTrailColor() const;
    bool setMouseTrailColor(const QColor& color) const;
    [[nodiscard]] QColor mouseClickColor() const;
    bool setMouseClickColor(const QColor& color) const;
    [[nodiscard]] bool showKeyboard() const;
    bool setShowKeyboard(bool show) const;
    [[nodiscard]] bool mouseHighlightEnabled() const;
    bool setMouseHighlightEnabled(bool enabled) const;
    [[nodiscard]] bool recordMouseClicks() const;
    bool setRecordMouseClicks(bool enabled) const;
    [[nodiscard]] QColor mouseHighlightColor() const;
    bool setMouseHighlightColor(const QColor& color) const;
    [[nodiscard]] bool showCursor() const;
    bool setShowCursor(bool show) const;
    [[nodiscard]] QString encoder() const;
    bool setEncoder(const QString& encoder) const;
    [[nodiscard]] int videoQuality() const;
    bool setVideoQuality(int quality) const;
    [[nodiscard]] QString encodingPreset() const;
    bool setEncodingPreset(const QString& preset) const;
    [[nodiscard]] bool captureToolbarInRecording() const;
    bool setCaptureToolbarInRecording(bool capture) const;
    [[nodiscard]] int startDelaySeconds() const;
    bool setStartDelaySeconds(int seconds) const;
    [[nodiscard]] QString videoSaveDirectory() const;
    bool setVideoSaveDirectory(const QString& directory) const;
    [[nodiscard]] QString videoFilenameFormat() const;
    bool setVideoFilenameFormat(const QString& format) const;
};

class ScreenshotToolbarSettings final {
  public:
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_QR_RECOGNITION
    [[nodiscard]] QString tableQrTool() const;
    bool setTableQrTool(const QString& tool) const;
#endif
    [[nodiscard]] QString lastFilterTool() const;
    bool setLastFilterTool(const QString& tool) const;
    [[nodiscard]] QString lastEraserTool() const;
    bool setLastEraserTool(const QString& tool) const;
    [[nodiscard]] QString lastHighlightTool() const;
    bool setLastHighlightTool(const QString& tool) const;
    [[nodiscard]] QString lastDrawingTool() const;
    bool setLastDrawingTool(const QString& tool) const;
    [[nodiscard]] ScreenshotToolbarLayout layout(ScreenshotToolbarLayoutKind kind) const;
    bool setLayout(ScreenshotToolbarLayoutKind kind, const ScreenshotToolbarLayout& layout) const;
};

struct WatermarkTemplate {
    QString name;
    QString value;

    friend bool operator==(const WatermarkTemplate&, const WatermarkTemplate&) = default;
};

class WatermarkTemplateSettings final {
  public:
    [[nodiscard]] QVector<WatermarkTemplate> templates() const;
    bool setTemplates(const QVector<WatermarkTemplate>& templates) const;
};

struct DrawTemplate {
    QString name;
    QByteArray payload;

    friend bool operator==(const DrawTemplate&, const DrawTemplate&) = default;
};

class DrawTemplateSettings final {
  public:
    [[nodiscard]] QVector<DrawTemplate> templates() const;
    bool setTemplates(const QVector<DrawTemplate>& templates) const;
};

class PinToScreenSettings final {
  public:
    [[nodiscard]] bool showWindowButtons() const;
    bool setShowWindowButtons(bool enabled) const;
    [[nodiscard]] QString doubleClickAction() const;
    bool setDoubleClickAction(const QString& action) const;
    [[nodiscard]] QString middleMouseButtonAction() const;
    bool setMiddleMouseButtonAction(const QString& action) const;
    [[nodiscard]] QColor borderColor() const;
    bool setBorderColor(const QColor& color) const;
    [[nodiscard]] QColor borderActiveColor() const;
    [[nodiscard]] QColor lockedBorderColor() const;
    bool setLockedBorderColor(const QColor& color) const;
    bool setBorderActiveColor(const QColor& color) const;
    [[nodiscard]] QString mouseWheelZoomMode() const;
    bool setMouseWheelZoomMode(const QString& mode) const;
    [[nodiscard]] QString duplicateContentAction() const;
    bool setDuplicateContentAction(const QString& value) const;
    [[nodiscard]] QString textSelectionOnRecognitionResults() const;
    [[nodiscard]] bool setTextSelectionOnRecognitionResults(const QString& mode) const;
    [[nodiscard]] bool automaticTextRecognition() const;
    bool setAutomaticTextRecognition(bool enabled) const;
    [[nodiscard]] bool autoResizeWindow() const;
    bool setAutoResizeWindow(bool enabled) const;
};

class TraySettings final {
  public:
    [[nodiscard]] bool enabled() const;
    bool setEnabled(bool enabled) const;
    [[nodiscard]] QString icon() const;
    bool setIcon(const QString& icon) const;
    [[nodiscard]] QString customIcon() const;
    bool setCustomIcon(const QString& path) const;
    [[nodiscard]] QString leftClickAction() const;
    [[nodiscard]] QString middleClickAction() const;
    bool setMiddleClickAction(const QString& action) const;
    bool setLeftClickAction(const QString& action) const;
    [[nodiscard]] QStringList menuOptions() const;
    bool setMenuOptions(const QStringList& options) const;
};

class SystemSettings final {
  public:
    [[nodiscard]] bool autoStartAtBoot() const;
    [[nodiscard]] bool launchAsAdministrator() const;
    [[nodiscard]] bool setLaunchAsAdministrator(bool enabled) const;
    bool setAutoStartAtBoot(bool enabled) const;
};

class NetworkSettings final {
  public:
    [[nodiscard]] QString proxy() const;
    bool setProxy(const QString& proxy) const;
};
} // namespace snow_shot::storage

Q_DECLARE_METATYPE(snow_shot::storage::ScreenshotToolbarLayout)

#endif // SNOW_SHOT_STORAGE_SETTINGSADAPTERS_H
