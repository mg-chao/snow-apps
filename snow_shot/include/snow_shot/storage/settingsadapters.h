#ifndef SNOW_SHOT_STORAGE_SETTINGSADAPTERS_H
#define SNOW_SHOT_STORAGE_SETTINGSADAPTERS_H

#include "snow_shot/customaimodelconfiguration.h"
#include "snow_shot/shortcuts/shortcutbinding.h"

#include <QColor>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMetaType>

namespace snow_shot::storage {
class ExtendedFeaturesSettings final {
  public:
    [[nodiscard]] bool translationPageEnabled() const;
    bool setTranslationPageEnabled(bool enabled) const;
    [[nodiscard]] bool standaloneTranslationWindow() const;
    bool setStandaloneTranslationWindow(bool enabled) const;
};

class TextRecognitionSettings final {
  public:
    [[nodiscard]] bool saveRecognitionResultAsImage() const;
    bool setSaveRecognitionResultAsImage(bool enabled) const;
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
};

[[nodiscard]] QColor colorFromRgbaString(const QString& value);
[[nodiscard]] QString colorToRgbaString(const QColor& color);

class ApiConfigurationSettings final {
  public:
    [[nodiscard]] CustomAiModels customModels() const;
    bool setCustomModels(const CustomAiModels& models) const;
};

class InterfaceSettings final {
  public:
    [[nodiscard]] QColor themePrimaryColor() const;
    bool setThemePrimaryColor(const QColor& color) const;
    [[nodiscard]] QString themeMode() const;
    bool setThemeMode(const QString& mode) const;
    [[nodiscard]] QString language() const;
    bool setLanguage(const QString& language) const;
    [[nodiscard]] bool sidebarCollapsed() const;
    bool setSidebarCollapsed(bool collapsed) const;
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
    [[nodiscard]] shortcuts::ShortcutBindingList screenshotTranslation() const;
    bool setScreenshotTranslation(const shortcuts::ShortcutBindingList& bindings) const;
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
    [[nodiscard]] shortcuts::ShortcutBindingList openSettings() const;
    bool setOpenSettings(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList pinClipboardContent() const;
    bool setPinClipboardContent(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList pinSelectedFiles() const;
    bool setPinSelectedFiles(const shortcuts::ShortcutBindingList& bindings) const;
    [[nodiscard]] shortcuts::ShortcutBindingList translateSelectedText() const;
    bool setTranslateSelectedText(const shortcuts::ShortcutBindingList& bindings) const;
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
    [[nodiscard]] bool captureCursor() const;
    bool setCaptureCursor(bool enabled) const;
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
    [[nodiscard]] QString manualSaveFilenameFormat() const;
    bool setManualSaveFilenameFormat(const QString& format) const;
    [[nodiscard]] QString autoSaveFilenameFormat() const;
    bool setAutoSaveFilenameFormat(const QString& format) const;
};

class DrawingSettings final {
  public:
    [[nodiscard]] QStringList quickSelectionDisabledTools() const;
    bool setQuickSelectionDisabledTools(const QStringList& tools) const;
};

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
    [[nodiscard]] shortcuts::ShortcutBindingList copyColor() const;

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

    friend bool operator==(const ScreenshotTranslationConfiguration& first,
                           const ScreenshotTranslationConfiguration& second) = default;
};

class ScreenshotImageConversionSettings final {
  public:
    [[nodiscard]] QString visionModel() const;
    bool setVisionModel(const QString& model) const;
};

class ScreenshotTranslationSettings final {
  public:
    [[nodiscard]] bool originalImageTranslationEnabled() const;
    bool setOriginalImageTranslationEnabled(bool enabled) const;
    [[nodiscard]] QString layoutProcessing() const;
    bool setLayoutProcessing(const QString& mode) const;
    [[nodiscard]] ScreenshotTranslationConfiguration configuration() const;
    bool setConfiguration(const ScreenshotTranslationConfiguration& configuration) const;
};

class ScreenshotUiSettings final {
  public:
    [[nodiscard]] QString toolbarSize() const;
    bool setToolbarSize(const QString& size) const;
    [[nodiscard]] bool selectionTransitionAnimationEnabled() const;
    bool setSelectionTransitionAnimationEnabled(bool enabled) const;
    [[nodiscard]] QString colorPickerDisplayMode() const;
    bool setColorPickerDisplayMode(const QString& mode) const;
    [[nodiscard]] QString colorPickerFormat() const;
    bool setColorPickerFormat(const QString& format) const;
    [[nodiscard]] QColor selectionMaskColor() const;
    bool setSelectionMaskColor(const QColor& color) const;
    [[nodiscard]] int shortcutHintOpacity() const;
    bool setShortcutHintOpacity(int opacity) const;
    [[nodiscard]] QColor cursorGuideLineColor() const;
    bool setCursorGuideLineColor(const QColor& color) const;
    [[nodiscard]] QColor monitorCenterGuideLineColor() const;
    bool setMonitorCenterGuideLineColor(const QColor& color) const;
    [[nodiscard]] QColor colorPickerCenterGuideLineColor() const;
    bool setColorPickerCenterGuideLineColor(const QColor& color) const;
};

class RecordingSettings final {
  public:
    [[nodiscard]] bool microphoneEnabled() const;
    bool setMicrophoneEnabled(bool enabled) const;
    [[nodiscard]] bool systemAudioEnabled() const;
    bool setSystemAudioEnabled(bool enabled) const;
    [[nodiscard]] QString screenRecordingClarity() const;
    bool setScreenRecordingClarity(const QString& clarity) const;
    [[nodiscard]] int frameRate() const;
    bool setFrameRate(int frameRate) const;
    [[nodiscard]] QString animatedImageClarity() const;
    bool setAnimatedImageClarity(const QString& clarity) const;
    [[nodiscard]] int animatedImageFrameRate() const;
    bool setAnimatedImageFrameRate(int frameRate) const;
    [[nodiscard]] bool loopAnimatedImages() const;
    bool setLoopAnimatedImages(bool enabled) const;
    [[nodiscard]] QString outputFormat() const;
    bool setOutputFormat(const QString& format) const;
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
    [[nodiscard]] bool showCursor() const;
    bool setShowCursor(bool show) const;
    [[nodiscard]] QString encoder() const;
    bool setEncoder(const QString& encoder) const;
    [[nodiscard]] QString encodingPreset() const;
    bool setEncodingPreset(const QString& preset) const;
    [[nodiscard]] bool captureToolbarInRecording() const;
    bool setCaptureToolbarInRecording(bool capture) const;
    [[nodiscard]] QString videoSaveDirectory() const;
    bool setVideoSaveDirectory(const QString& directory) const;
    [[nodiscard]] QString videoFilenameFormat() const;
    bool setVideoFilenameFormat(const QString& format) const;
};

class ScreenshotToolbarSettings final {
  public:
    [[nodiscard]] QString tableQrTool() const;
    bool setTableQrTool(const QString& tool) const;
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

class PinToScreenSettings final {
  public:
    [[nodiscard]] QString doubleClickAction() const;
    bool setDoubleClickAction(const QString& action) const;
    [[nodiscard]] QString middleMouseButtonAction() const;
    bool setMiddleMouseButtonAction(const QString& action) const;
    [[nodiscard]] QColor borderColor() const;
    bool setBorderColor(const QColor& color) const;
    [[nodiscard]] QColor borderActiveColor() const;
    bool setBorderActiveColor(const QColor& color) const;
    [[nodiscard]] QString mouseWheelZoomMode() const;
    bool setMouseWheelZoomMode(const QString& mode) const;
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
