#ifndef SNOW_SHOT_STORAGE_SETTINGSADAPTERS_H
#define SNOW_SHOT_STORAGE_SETTINGSADAPTERS_H

#include "snow_shot/storage/capturehistorytypes.h"

#include <QColor>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <future>

namespace snow_shot::storage {
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

[[nodiscard]] QColor colorFromRgbaString(const QString& value);
[[nodiscard]] QString colorToRgbaString(const QColor& color);

class InterfaceSettings final {
  public:
    [[nodiscard]] QString themeMode() const;
    bool setThemeMode(const QString& mode) const;
    [[nodiscard]] QString language() const;
    bool setLanguage(const QString& language) const;
    [[nodiscard]] bool sidebarCollapsed() const;
    bool setSidebarCollapsed(bool collapsed) const;
};

class ShortcutSettings final {
  public:
    [[nodiscard]] QStringList screenshot() const;
    bool setScreenshot(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList screenshotDelay() const;
    bool setScreenshotDelay(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList screenshotFixed() const;
    bool setScreenshotFixed(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList screenshotOcr() const;
    bool setScreenshotOcr(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList screenshotTranslation() const;
    bool setScreenshotTranslation(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList screenshotCopy() const;
    bool setScreenshotCopy(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList screenshotFullScreen() const;
    bool setScreenshotFullScreen(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList screenshotFocusedWindow() const;
    bool setScreenshotFocusedWindow(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList screenRecord() const;
    bool setScreenRecord(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList screenRecordCopy() const;
    bool setScreenRecordCopy(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList openCaptureHistory() const;
    bool setOpenCaptureHistory(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList openSettings() const;
    bool setOpenSettings(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList pinClipboardContent() const;
    bool setPinClipboardContent(const QStringList& shortcuts) const;
};

class GlobalShortcutSettings final {
  public:
    [[nodiscard]] bool disableOnFocusedFullscreenWindow() const;
    bool setDisableOnFocusedFullscreenWindow(bool disabled) const;
};

class ScreenshotSettings final {
  public:
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
    [[nodiscard]] static bool isReservedShortcut(const QString& shortcut);
    [[nodiscard]] static bool isReservedShortcutAllowed(const QString& actionId,
                                                         const QString& shortcut);

    [[nodiscard]] QStringList moveTool() const;
    bool setMoveTool(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList moveCursorUp() const;
    bool setMoveCursorUp(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList moveCursorDown() const;
    bool setMoveCursorDown(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList moveCursorLeft() const;
    bool setMoveCursorLeft(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList moveCursorRight() const;
    bool setMoveCursorRight(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList moveEntireSelection() const;
    bool setMoveEntireSelection(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList keepSelectionWidthAndHeightConsistent() const;
    bool setKeepSelectionWidthAndHeightConsistent(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList switchSelectionBetweenWindowAndWindowSubElement() const;
    bool setSwitchSelectionBetweenWindowAndWindowSubElement(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList previousScreenshotHistory() const;
    bool setPreviousScreenshotHistory(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList nextScreenshotHistory() const;
    bool setNextScreenshotHistory(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList selectPreviouslySelectedArea() const;
    bool setSelectPreviouslySelectedArea(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList copyColor() const;
    bool setCopyColor(const QStringList& shortcuts) const;

    [[nodiscard]] QStringList shortcuts(const QString& actionId) const;
    bool setShortcuts(const QString& actionId, const QStringList& shortcuts) const;
    [[nodiscard]] QMap<QString, QStringList> allShortcuts() const;
    bool setAllShortcutsAtomic(const QMap<QString, QStringList>& shortcutsByAction) const;
};

class DrawingShortcutSettings final {
  public:
    [[nodiscard]] static bool isReservedShortcut(const QString& shortcut);

    [[nodiscard]] QStringList select() const;
    bool setSelect(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList shape() const;
    bool setShape(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList arrow() const;
    bool setArrow(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList brush() const;
    bool setBrush(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList highlight() const;
    bool setHighlight(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList text() const;
    bool setText(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList serialNumber() const;
    bool setSerialNumber(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList filter() const;
    bool setFilter(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList eraser() const;
    bool setEraser(const QStringList& shortcuts) const;
    [[nodiscard]] QStringList watermark() const;
    bool setWatermark(const QStringList& shortcuts) const;

    [[nodiscard]] QStringList shortcuts(const QString& toolId) const;
    bool setShortcuts(const QString& toolId, const QStringList& shortcuts) const;
    [[nodiscard]] QMap<QString, QStringList> allShortcuts() const;
    bool setAllShortcutsAtomic(const QMap<QString, QStringList>& shortcutsByTool) const;
};

class PinToScreenShortcutSettings final {
  public:
    [[nodiscard]] QStringList shortcuts(const QString& actionId) const;
    bool setShortcuts(const QString& actionId, const QStringList& shortcuts) const;
    [[nodiscard]] QMap<QString, QStringList> allShortcuts() const;
    bool setAllShortcutsAtomic(const QMap<QString, QStringList>& shortcutsByAction) const;
};

struct ScreenshotTranslationConfiguration {
    QString sourceLanguage;
    QString targetLanguage;
    QString modelId;

    friend bool operator==(const ScreenshotTranslationConfiguration& first,
                           const ScreenshotTranslationConfiguration& second) = default;
};

class ScreenshotTranslationSettings final {
  public:
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
    [[nodiscard]] QString animatedImageFormat() const;
    bool setAnimatedImageFormat(const QString& format) const;
    [[nodiscard]] QString encoder() const;
    bool setEncoder(const QString& encoder) const;
    [[nodiscard]] QString encodingPreset() const;
    bool setEncodingPreset(const QString& preset) const;
    [[nodiscard]] bool hideToolbarInRecording() const;
    bool setHideToolbarInRecording(bool hide) const;
    [[nodiscard]] QString videoSaveDirectory() const;
    bool setVideoSaveDirectory(const QString& directory) const;
    [[nodiscard]] QString videoFilenameFormat() const;
    bool setVideoFilenameFormat(const QString& format) const;
};

class ScreenshotToolbarSettings final {
  public:
    [[nodiscard]] QString arrowLineTool() const;
    bool setArrowLineTool(const QString& tool) const;
    [[nodiscard]] QString highlightTool() const;
    bool setHighlightTool(const QString& tool) const;
    [[nodiscard]] QString tableQrTool() const;
    bool setTableQrTool(const QString& tool) const;
    [[nodiscard]] ScreenshotToolbarLayout layout() const;
    bool setLayout(const ScreenshotToolbarLayout& layout) const;
};

class PinToScreenSettings final {
  public:
    [[nodiscard]] QColor borderColor() const;
    bool setBorderColor(const QColor& color) const;
    [[nodiscard]] QString mouseWheelZoomMode() const;
    bool setMouseWheelZoomMode(const QString& mode) const;
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
    bool setLeftClickAction(const QString& action) const;
    [[nodiscard]] QStringList menuOptions() const;
    bool setMenuOptions(const QStringList& options) const;
};

class SystemSettings final {
  public:
    [[nodiscard]] bool autoStartAtBoot() const;
    bool setAutoStartAtBoot(bool enabled) const;
};

class NetworkSettings final {
  public:
    [[nodiscard]] QString proxy() const;
    bool setProxy(const QString& proxy) const;
};

class HistorySettings final {
  public:
    [[nodiscard]] CaptureHistoryPolicy policy() const;
    [[nodiscard]] std::shared_future<StorageResult>
    setPolicy(const CaptureHistoryPolicy& policy) const;
    [[nodiscard]] std::shared_future<StorageResult> setEnabled(bool enabled) const;
    [[nodiscard]] std::shared_future<StorageResult> setRetentionDays(int days) const;
    [[nodiscard]] std::shared_future<StorageResult> setMaxEntries(int entries) const;
    [[nodiscard]] std::shared_future<StorageResult> setMaxDiskMiB(int mebibytes) const;
};
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_SETTINGSADAPTERS_H
