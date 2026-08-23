#include "snow_shot/storage/settingsadapters.h"

#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationstore.h"

#include "capturehistorypolicy_p.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QLocale>
#include <QSet>

namespace snow_shot::storage {
namespace {
ConfigurationStore& cache() {
    auto& storage = ApplicationStorage::instance();
    if (!storage.isInitialized()) {
        static_cast<void>(storage.initialize());
    }
    return storage.configuration();
}

QStringList stringList(const QJsonValue& value) {
    QStringList result;
    for (const QJsonValue& item : value.toArray()) {
        result.push_back(item.toString());
    }
    return result;
}

QJsonArray stringArray(const QStringList& values) {
    QJsonArray result;
    for (const QString& value : values) {
        result.push_back(value);
    }
    return result;
}

QStringList shortcutValue(const QString& key) {
    return stringList(cache().value(key));
}

bool setShortcutValue(const QString& key, const QStringList& shortcuts) {
    return cache().setValue(key, stringArray(shortcuts));
}

const QStringList& drawingShortcutToolIds() {
    static const QStringList ids = {
        QStringLiteral("select"),        QStringLiteral("shape"),  QStringLiteral("arrow"),
        QStringLiteral("brush"),         QStringLiteral("highlight"),
        QStringLiteral("text"),          QStringLiteral("serial_number"),
        QStringLiteral("filter"),        QStringLiteral("eraser"),
        QStringLiteral("watermark"),
    };
    return ids;
}

const QStringList& screenshotShortcutActionIds() {
    static const QStringList ids = {
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
        QStringLiteral("table_recognition"),
        QStringLiteral("qr_code_recognition"),
        QStringLiteral("video_recording"),
        QStringLiteral("text_recognition"),
        QStringLiteral("text_translation"),
        QStringLiteral("scrolling_screenshot"),
        QStringLiteral("save_as_file"),
        QStringLiteral("pin_to_screen"),
        QStringLiteral("cancel_screenshot"),
        QStringLiteral("copy_to_clipboard"),
        QStringLiteral("undo"),
        QStringLiteral("redo"),
    };
    return ids;
}

const QStringList& pinToScreenShortcutActionIds() {
    static const QStringList ids = {
        QStringLiteral("copy_to_clipboard"),
        QStringLiteral("copy_original_content"),
        QStringLiteral("show_text_recognition_results"),
        QStringLiteral("drawing_mode"),
        QStringLiteral("thumbnail_mode"),
        QStringLiteral("close_window"),
        QStringLiteral("move_cursor_up"),
        QStringLiteral("move_cursor_down"),
        QStringLiteral("move_cursor_left"),
        QStringLiteral("move_cursor_right"),
    };
    return ids;
}

QString drawingShortcutKey(const QString& toolId) {
    return drawingShortcutToolIds().contains(toolId)
               ? QStringLiteral("drawing_shortcuts/") + toolId
               : QString();
}

QString screenshotShortcutKey(const QString& actionId) {
    return screenshotShortcutActionIds().contains(actionId)
               ? QStringLiteral("screenshot_shortcuts/") + actionId
               : QString();
}

QString pinToScreenShortcutKey(const QString& actionId) {
    return pinToScreenShortcutActionIds().contains(actionId)
               ? QStringLiteral("pin_to_screen_shortcuts/") + actionId
               : QString();
}

bool screenshotHistoryShortcutAllowed(const QString& actionId, const QString& shortcut) {
    const bool historyAction = actionId == QStringLiteral("previous_screenshot_history") ||
                               actionId == QStringLiteral("next_screenshot_history");
    return historyAction &&
           (shortcut == QStringLiteral(",") || shortcut == QStringLiteral("."));
}

bool isReservedLocalShortcut(const QString& shortcut) {
    QKeySequence sequence = QKeySequence::fromString(shortcut, QKeySequence::PortableText);
    if (sequence.isEmpty()) {
        sequence = QKeySequence::fromString(shortcut, QKeySequence::NativeText);
    }
    if (sequence.count() != 1) {
        return false;
    }

    const QKeyCombination combination = sequence[0];
    const Qt::Key key = combination.key();
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    if (key == Qt::Key_Escape || key == Qt::Key_Backspace || key == Qt::Key_Delete ||
        key == Qt::Key_F4) {
        return true;
    }
    if ((key == Qt::Key_Comma || key == Qt::Key_Period) && modifiers == Qt::NoModifier) {
        return true;
    }
    if (key == Qt::Key_C && modifiers.testFlag(Qt::ControlModifier) &&
        !modifiers.testFlag(Qt::AltModifier) && !modifiers.testFlag(Qt::MetaModifier)) {
        return true;
    }
    return key == Qt::Key_Z && modifiers.testFlag(Qt::ControlModifier);
}

QVector<QStringList> stringListArray(const QJsonValue& value) {
    QVector<QStringList> result;
    for (const QJsonValue& item : value.toArray()) {
        if (item.isArray()) {
            result.push_back(stringList(item));
        }
    }
    return result;
}

QJsonArray stringArrayArray(const QVector<QStringList>& values) {
    QJsonArray result;
    for (const QStringList& value : values) {
        result.push_back(stringArray(value));
    }
    return result;
}

QColor colorValue(const QString& key) {
    return colorFromRgbaString(cache().value(key).toString());
}

bool setColorValue(const QString& key, const QColor& color) {
    return color.isValid() && cache().setValue(key, colorToRgbaString(color));
}
} // namespace

QColor colorFromRgbaString(const QString& value) {
    const QString normalized = value.trimmed();
    if (normalized.size() != 9 || !normalized.startsWith(u'#')) {
        return {};
    }
    bool valid = false;
    const uint rgba = normalized.sliced(1).toUInt(&valid, 16);
    if (!valid) {
        return {};
    }
    return QColor(static_cast<int>((rgba >> 24) & 0xffU), static_cast<int>((rgba >> 16) & 0xffU),
                  static_cast<int>((rgba >> 8) & 0xffU), static_cast<int>(rgba & 0xffU));
}

QString colorToRgbaString(const QColor& color) {
    if (!color.isValid()) {
        return {};
    }
    return QStringLiteral("#%1%2%3%4")
        .arg(color.red(), 2, 16, QLatin1Char('0'))
        .arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(), 2, 16, QLatin1Char('0'))
        .arg(color.alpha(), 2, 16, QLatin1Char('0'))
        .toUpper();
}

QString InterfaceSettings::themeMode() const {
    return cache().value(QStringLiteral("interface/theme_mode")).toString();
}

bool InterfaceSettings::setThemeMode(const QString& mode) const {
    return cache().setValue(QStringLiteral("interface/theme_mode"), mode);
}

QString InterfaceSettings::language() const {
    return cache().value(QStringLiteral("interface/language")).toString();
}

bool InterfaceSettings::setLanguage(const QString& language) const {
    return cache().setValue(QStringLiteral("interface/language"), language);
}

bool InterfaceSettings::sidebarCollapsed() const {
    return cache().value(QStringLiteral("interface/sidebar_collapsed")).toBool();
}

bool InterfaceSettings::setSidebarCollapsed(bool collapsed) const {
    return cache().setValue(QStringLiteral("interface/sidebar_collapsed"), collapsed);
}

QStringList ShortcutSettings::screenshot() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screenshot"));
}

bool ShortcutSettings::setScreenshot(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screenshot"), shortcuts);
}

QStringList ShortcutSettings::screenshotDelay() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screenshot_delay"));
}

bool ShortcutSettings::setScreenshotDelay(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screenshot_delay"), shortcuts);
}

QStringList ShortcutSettings::screenshotFixed() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screenshot_fixed"));
}

bool ShortcutSettings::setScreenshotFixed(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screenshot_fixed"), shortcuts);
}

QStringList ShortcutSettings::screenshotOcr() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screenshot_ocr"));
}

bool ShortcutSettings::setScreenshotOcr(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screenshot_ocr"), shortcuts);
}

QStringList ShortcutSettings::screenshotTranslation() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screenshot_translation"));
}

bool ShortcutSettings::setScreenshotTranslation(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screenshot_translation"), shortcuts);
}

QStringList ShortcutSettings::screenshotCopy() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screenshot_copy"));
}

bool ShortcutSettings::setScreenshotCopy(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screenshot_copy"), shortcuts);
}

QStringList ShortcutSettings::screenshotFullScreen() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screenshot_full_screen"));
}

bool ShortcutSettings::setScreenshotFullScreen(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screenshot_full_screen"), shortcuts);
}

QStringList ShortcutSettings::screenshotFocusedWindow() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screenshot_focused_window"));
}

bool ShortcutSettings::setScreenshotFocusedWindow(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screenshot_focused_window"),
                            shortcuts);
}

QStringList ShortcutSettings::screenRecord() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screen_record"));
}

bool ShortcutSettings::setScreenRecord(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screen_record"), shortcuts);
}

QStringList ShortcutSettings::screenRecordCopy() const {
    return shortcutValue(QStringLiteral("global_shortcuts/screen_record_copy"));
}

bool ShortcutSettings::setScreenRecordCopy(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/screen_record_copy"), shortcuts);
}

QStringList ShortcutSettings::openCaptureHistory() const {
    return shortcutValue(QStringLiteral("global_shortcuts/open_capture_history"));
}

bool ShortcutSettings::setOpenCaptureHistory(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/open_capture_history"), shortcuts);
}

QStringList ShortcutSettings::openSettings() const {
    return shortcutValue(QStringLiteral("global_shortcuts/open_settings"));
}

bool ShortcutSettings::setOpenSettings(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/open_settings"), shortcuts);
}

QStringList ShortcutSettings::pinClipboardContent() const {
    return shortcutValue(QStringLiteral("global_shortcuts/pin_clipboard_content"));
}

bool ShortcutSettings::setPinClipboardContent(const QStringList& shortcuts) const {
    return setShortcutValue(QStringLiteral("global_shortcuts/pin_clipboard_content"), shortcuts);
}

bool GlobalShortcutSettings::disableOnFocusedFullscreenWindow() const {
    return cache()
        .value(QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window"))
        .toBool();
}

bool GlobalShortcutSettings::setDisableOnFocusedFullscreenWindow(bool disabled) const {
    return cache().setValue(
        QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window"), disabled);
}

int ScreenshotSettings::delaySeconds() const {
    return cache().value(QStringLiteral("screenshot/delay_seconds")).toInt();
}

bool ScreenshotSettings::setDelaySeconds(int seconds) const {
    return cache().setValue(QStringLiteral("screenshot/delay_seconds"), seconds);
}

QString ScreenshotSettings::autoExecuteAfterTextRecognition() const {
    return cache()
        .value(QStringLiteral("screenshot/auto_execute_after_text_recognition"))
        .toString();
}

bool ScreenshotSettings::setAutoExecuteAfterTextRecognition(const QString& action) const {
    return cache().setValue(QStringLiteral("screenshot/auto_execute_after_text_recognition"),
                            action);
}

QString ScreenshotSettings::doubleClickAction() const {
    return cache().value(QStringLiteral("screenshot/double_click_action")).toString();
}

bool ScreenshotSettings::setDoubleClickAction(const QString& action) const {
    return cache().setValue(QStringLiteral("screenshot/double_click_action"), action);
}

QString ScreenshotSettings::middleMouseButtonAction() const {
    return cache().value(QStringLiteral("screenshot/middle_mouse_button_action")).toString();
}

bool ScreenshotSettings::setMiddleMouseButtonAction(const QString& action) const {
    return cache().setValue(QStringLiteral("screenshot/middle_mouse_button_action"), action);
}

bool ScreenshotSettings::autoSaveAfterCopy() const {
    return cache().value(QStringLiteral("screenshot/auto_save_after_copy")).toBool();
}

bool ScreenshotSettings::setAutoSaveAfterCopy(bool enabled) const {
    return cache().setValue(QStringLiteral("screenshot/auto_save_after_copy"), enabled);
}

bool ScreenshotSettings::copyImageFileToClipboard() const {
    return cache().value(QStringLiteral("screenshot/copy_image_file_to_clipboard")).toBool();
}

bool ScreenshotSettings::setCopyImageFileToClipboard(bool enabled) const {
    return cache().setValue(QStringLiteral("screenshot/copy_image_file_to_clipboard"), enabled);
}

QString ScreenshotSettings::imageSaveDirectory() const {
    return cache().value(QStringLiteral("screenshot/image_save_directory")).toString();
}

bool ScreenshotSettings::setImageSaveDirectory(const QString& directory) const {
    return cache().setValue(QStringLiteral("screenshot/image_save_directory"), directory);
}

QString ScreenshotSettings::lastManualSaveDirectory() const {
    return cache().value(QStringLiteral("screenshot/last_manual_save_directory")).toString();
}

bool ScreenshotSettings::setLastManualSaveDirectory(const QString& directory) const {
    return cache().setValue(QStringLiteral("screenshot/last_manual_save_directory"), directory);
}

QString ScreenshotSettings::imageFormat() const {
    return cache().value(QStringLiteral("screenshot/image_format")).toString();
}

bool ScreenshotSettings::setImageFormat(const QString& format) const {
    return cache().setValue(QStringLiteral("screenshot/image_format"), format);
}

QString ScreenshotSettings::manualSaveFilenameFormat() const {
    return cache().value(QStringLiteral("screenshot/manual_save_filename_format")).toString();
}

bool ScreenshotSettings::setManualSaveFilenameFormat(const QString& format) const {
    return cache().setValue(QStringLiteral("screenshot/manual_save_filename_format"), format);
}

QString ScreenshotSettings::autoSaveFilenameFormat() const {
    return cache().value(QStringLiteral("screenshot/auto_save_filename_format")).toString();
}

bool ScreenshotSettings::setAutoSaveFilenameFormat(const QString& format) const {
    return cache().setValue(QStringLiteral("screenshot/auto_save_filename_format"), format);
}

QStringList DrawingSettings::quickSelectionDisabledTools() const {
    return stringList(cache().value(QStringLiteral("drawing/quick_selection_disabled_tools")));
}

bool DrawingSettings::setQuickSelectionDisabledTools(const QStringList& tools) const {
    return cache().setValue(QStringLiteral("drawing/quick_selection_disabled_tools"),
                            stringArray(tools));
}

QStringList ScreenshotShortcutSettings::moveTool() const {
    return shortcuts(QStringLiteral("move_tool"));
}

bool ScreenshotShortcutSettings::setMoveTool(const QStringList& value) const {
    return setShortcuts(QStringLiteral("move_tool"), value);
}

QStringList ScreenshotShortcutSettings::moveCursorUp() const {
    return shortcuts(QStringLiteral("move_cursor_up"));
}

bool ScreenshotShortcutSettings::setMoveCursorUp(const QStringList& value) const {
    return setShortcuts(QStringLiteral("move_cursor_up"), value);
}

QStringList ScreenshotShortcutSettings::moveCursorDown() const {
    return shortcuts(QStringLiteral("move_cursor_down"));
}

bool ScreenshotShortcutSettings::setMoveCursorDown(const QStringList& value) const {
    return setShortcuts(QStringLiteral("move_cursor_down"), value);
}

QStringList ScreenshotShortcutSettings::moveCursorLeft() const {
    return shortcuts(QStringLiteral("move_cursor_left"));
}

bool ScreenshotShortcutSettings::setMoveCursorLeft(const QStringList& value) const {
    return setShortcuts(QStringLiteral("move_cursor_left"), value);
}

QStringList ScreenshotShortcutSettings::moveCursorRight() const {
    return shortcuts(QStringLiteral("move_cursor_right"));
}

bool ScreenshotShortcutSettings::setMoveCursorRight(const QStringList& value) const {
    return setShortcuts(QStringLiteral("move_cursor_right"), value);
}

QStringList ScreenshotShortcutSettings::moveEntireSelection() const {
    return shortcuts(QStringLiteral("move_entire_selection"));
}

bool ScreenshotShortcutSettings::setMoveEntireSelection(const QStringList& value) const {
    return setShortcuts(QStringLiteral("move_entire_selection"), value);
}

QStringList ScreenshotShortcutSettings::keepSelectionWidthAndHeightConsistent() const {
    return shortcuts(QStringLiteral("keep_selection_width_and_height_consistent"));
}

bool ScreenshotShortcutSettings::setKeepSelectionWidthAndHeightConsistent(
    const QStringList& value) const {
    return setShortcuts(QStringLiteral("keep_selection_width_and_height_consistent"), value);
}

QStringList ScreenshotShortcutSettings::switchSelectionBetweenWindowAndWindowSubElement() const {
    return shortcuts(
        QStringLiteral("switch_selection_between_window_and_window_sub_element"));
}

bool ScreenshotShortcutSettings::setSwitchSelectionBetweenWindowAndWindowSubElement(
    const QStringList& value) const {
    return setShortcuts(
        QStringLiteral("switch_selection_between_window_and_window_sub_element"), value);
}

QStringList ScreenshotShortcutSettings::previousScreenshotHistory() const {
    return shortcuts(QStringLiteral("previous_screenshot_history"));
}

bool ScreenshotShortcutSettings::setPreviousScreenshotHistory(const QStringList& value) const {
    return setShortcuts(QStringLiteral("previous_screenshot_history"), value);
}

QStringList ScreenshotShortcutSettings::nextScreenshotHistory() const {
    return shortcuts(QStringLiteral("next_screenshot_history"));
}

bool ScreenshotShortcutSettings::setNextScreenshotHistory(const QStringList& value) const {
    return setShortcuts(QStringLiteral("next_screenshot_history"), value);
}

QStringList ScreenshotShortcutSettings::selectPreviouslySelectedArea() const {
    return shortcuts(QStringLiteral("select_previously_selected_area"));
}

bool ScreenshotShortcutSettings::setSelectPreviouslySelectedArea(const QStringList& value) const {
    return setShortcuts(QStringLiteral("select_previously_selected_area"), value);
}

QStringList ScreenshotShortcutSettings::copyColor() const {
    return shortcuts(QStringLiteral("copy_color"));
}

bool ScreenshotShortcutSettings::setCopyColor(const QStringList& value) const {
    return setShortcuts(QStringLiteral("copy_color"), value);
}

bool ScreenshotShortcutSettings::isReservedShortcut(const QString& shortcut) {
    return isReservedLocalShortcut(shortcut);
}

bool ScreenshotShortcutSettings::isReservedShortcutAllowed(const QString& actionId,
                                                            const QString& shortcut) {
    if (screenshotHistoryShortcutAllowed(actionId, shortcut)) {
        return true;
    }

    QKeySequence sequence = QKeySequence::fromString(shortcut, QKeySequence::PortableText);
    if (sequence.isEmpty()) {
        sequence = QKeySequence::fromString(shortcut, QKeySequence::NativeText);
    }
    if (sequence.count() != 1) {
        return false;
    }

    const QKeyCombination combination = sequence[0];
    const Qt::Key key = combination.key();
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    if (actionId == QStringLiteral("cancel_screenshot")) {
        return key == Qt::Key_Escape;
    }
    if (actionId == QStringLiteral("copy_to_clipboard")) {
        return key == Qt::Key_C && modifiers.testFlag(Qt::ControlModifier) &&
               !modifiers.testFlag(Qt::AltModifier) && !modifiers.testFlag(Qt::MetaModifier);
    }
    if (actionId == QStringLiteral("undo")) {
        return key == Qt::Key_Z && modifiers.testFlag(Qt::ControlModifier);
    }
    return false;
}

QStringList ScreenshotShortcutSettings::shortcuts(const QString& actionId) const {
    const QString key = screenshotShortcutKey(actionId);
    return key.isEmpty() ? QStringList{} : shortcutValue(key);
}

bool ScreenshotShortcutSettings::setShortcuts(const QString& actionId,
                                               const QStringList& value) const {
    if (screenshotShortcutKey(actionId).isEmpty()) {
        return false;
    }
    QMap<QString, QStringList> next = allShortcuts();
    next.insert(actionId, value);
    return setAllShortcutsAtomic(next);
}

QMap<QString, QStringList> ScreenshotShortcutSettings::allShortcuts() const {
    QMap<QString, QStringList> result;
    for (const QString& actionId : screenshotShortcutActionIds()) {
        result.insert(actionId, shortcutValue(screenshotShortcutKey(actionId)));
    }
    return result;
}

bool ScreenshotShortcutSettings::setAllShortcutsAtomic(
    const QMap<QString, QStringList>& shortcutsByAction) const {
    if (shortcutsByAction.size() != screenshotShortcutActionIds().size()) {
        return false;
    }
    QMap<QString, QJsonValue> values;
    QSet<QString> seen;
    for (const QString& actionId : screenshotShortcutActionIds()) {
        if (!shortcutsByAction.contains(actionId)) {
            return false;
        }
        const QString key = screenshotShortcutKey(actionId);
        const ConfigurationNormalization normalized =
            ConfigurationSchema::normalize(key, stringArray(shortcutsByAction.value(actionId)));
        if (!normalized.valid) {
            return false;
        }
        for (const QJsonValue& item : normalized.value.toArray()) {
            const QString shortcut = item.toString();
            const QString binding = shortcut.toCaseFolded();
            if ((isReservedShortcut(shortcut) &&
                 !isReservedShortcutAllowed(actionId, shortcut)) ||
                seen.contains(binding)) {
                return false;
            }
            seen.insert(binding);
        }
        values.insert(key, normalized.value);
    }
    return cache().setValues(values);
}

QStringList DrawingShortcutSettings::select() const {
    return shortcuts(QStringLiteral("select"));
}

bool DrawingShortcutSettings::setSelect(const QStringList& value) const {
    return setShortcuts(QStringLiteral("select"), value);
}

QStringList DrawingShortcutSettings::shape() const {
    return shortcuts(QStringLiteral("shape"));
}

bool DrawingShortcutSettings::setShape(const QStringList& value) const {
    return setShortcuts(QStringLiteral("shape"), value);
}

QStringList DrawingShortcutSettings::arrow() const {
    return shortcuts(QStringLiteral("arrow"));
}

bool DrawingShortcutSettings::setArrow(const QStringList& value) const {
    return setShortcuts(QStringLiteral("arrow"), value);
}

QStringList DrawingShortcutSettings::brush() const {
    return shortcuts(QStringLiteral("brush"));
}

bool DrawingShortcutSettings::setBrush(const QStringList& value) const {
    return setShortcuts(QStringLiteral("brush"), value);
}

QStringList DrawingShortcutSettings::highlight() const {
    return shortcuts(QStringLiteral("highlight"));
}

bool DrawingShortcutSettings::setHighlight(const QStringList& value) const {
    return setShortcuts(QStringLiteral("highlight"), value);
}

QStringList DrawingShortcutSettings::text() const {
    return shortcuts(QStringLiteral("text"));
}

bool DrawingShortcutSettings::setText(const QStringList& value) const {
    return setShortcuts(QStringLiteral("text"), value);
}

QStringList DrawingShortcutSettings::serialNumber() const {
    return shortcuts(QStringLiteral("serial_number"));
}

bool DrawingShortcutSettings::setSerialNumber(const QStringList& value) const {
    return setShortcuts(QStringLiteral("serial_number"), value);
}

QStringList DrawingShortcutSettings::filter() const {
    return shortcuts(QStringLiteral("filter"));
}

bool DrawingShortcutSettings::setFilter(const QStringList& value) const {
    return setShortcuts(QStringLiteral("filter"), value);
}

QStringList DrawingShortcutSettings::eraser() const {
    return shortcuts(QStringLiteral("eraser"));
}

bool DrawingShortcutSettings::setEraser(const QStringList& value) const {
    return setShortcuts(QStringLiteral("eraser"), value);
}

QStringList DrawingShortcutSettings::watermark() const {
    return shortcuts(QStringLiteral("watermark"));
}

bool DrawingShortcutSettings::setWatermark(const QStringList& value) const {
    return setShortcuts(QStringLiteral("watermark"), value);
}

bool DrawingShortcutSettings::isReservedShortcut(const QString& shortcut) {
    return isReservedLocalShortcut(shortcut);
}

QStringList DrawingShortcutSettings::shortcuts(const QString& toolId) const {
    const QString key = drawingShortcutKey(toolId);
    return key.isEmpty() ? QStringList{} : shortcutValue(key);
}

bool DrawingShortcutSettings::setShortcuts(const QString& toolId,
                                           const QStringList& value) const {
    if (drawingShortcutKey(toolId).isEmpty()) {
        return false;
    }
    QMap<QString, QStringList> next = allShortcuts();
    next.insert(toolId, value);
    return setAllShortcutsAtomic(next);
}

QMap<QString, QStringList> DrawingShortcutSettings::allShortcuts() const {
    QMap<QString, QStringList> result;
    for (const QString& toolId : drawingShortcutToolIds()) {
        result.insert(toolId, shortcutValue(drawingShortcutKey(toolId)));
    }
    return result;
}

bool DrawingShortcutSettings::setAllShortcutsAtomic(
    const QMap<QString, QStringList>& shortcutsByTool) const {
    if (shortcutsByTool.size() != drawingShortcutToolIds().size()) {
        return false;
    }
    QMap<QString, QJsonValue> values;
    QSet<QString> seen;
    for (const QString& toolId : drawingShortcutToolIds()) {
        if (!shortcutsByTool.contains(toolId)) {
            return false;
        }
        const QString key = drawingShortcutKey(toolId);
        const ConfigurationNormalization normalized =
            ConfigurationSchema::normalize(key, stringArray(shortcutsByTool.value(toolId)));
        if (!normalized.valid) {
            return false;
        }
        for (const QJsonValue& item : normalized.value.toArray()) {
            const QString shortcut = item.toString();
            if (isReservedShortcut(shortcut)) {
                return false;
            }
            const QString binding = shortcut.toCaseFolded();
            if (seen.contains(binding)) {
                return false;
            }
            seen.insert(binding);
        }
        values.insert(key, normalized.value);
    }
    return cache().setValues(values);
}

QStringList PinToScreenShortcutSettings::shortcuts(const QString& actionId) const {
    const QString key = pinToScreenShortcutKey(actionId);
    return key.isEmpty() ? QStringList{} : shortcutValue(key);
}

bool PinToScreenShortcutSettings::setShortcuts(const QString& actionId,
                                               const QStringList& value) const {
    if (pinToScreenShortcutKey(actionId).isEmpty()) {
        return false;
    }
    QMap<QString, QStringList> next = allShortcuts();
    next.insert(actionId, value);
    return setAllShortcutsAtomic(next);
}

QMap<QString, QStringList> PinToScreenShortcutSettings::allShortcuts() const {
    QMap<QString, QStringList> result;
    for (const QString& actionId : pinToScreenShortcutActionIds()) {
        result.insert(actionId, shortcutValue(pinToScreenShortcutKey(actionId)));
    }
    return result;
}

bool PinToScreenShortcutSettings::setAllShortcutsAtomic(
    const QMap<QString, QStringList>& shortcutsByAction) const {
    if (shortcutsByAction.size() != pinToScreenShortcutActionIds().size()) {
        return false;
    }
    QMap<QString, QJsonValue> values;
    QSet<QString> seen;
    for (const QString& actionId : pinToScreenShortcutActionIds()) {
        if (!shortcutsByAction.contains(actionId)) {
            return false;
        }
        const QString key = pinToScreenShortcutKey(actionId);
        const ConfigurationNormalization normalized =
            ConfigurationSchema::normalize(key, stringArray(shortcutsByAction.value(actionId)));
        if (!normalized.valid) {
            return false;
        }
        for (const QJsonValue& item : normalized.value.toArray()) {
            const QString binding = item.toString().toCaseFolded();
            if (seen.contains(binding)) {
                return false;
            }
            seen.insert(binding);
        }
        values.insert(key, normalized.value);
    }
    return cache().setValues(values);
}

QString ScreenshotUiSettings::toolbarSize() const {
    return cache().value(QStringLiteral("screenshot_ui/toolbar_size")).toString();
}

bool ScreenshotUiSettings::setToolbarSize(const QString& size) const {
    return cache().setValue(QStringLiteral("screenshot_ui/toolbar_size"), size);
}

bool ScreenshotUiSettings::selectionTransitionAnimationEnabled() const {
    return cache().value(QStringLiteral("screenshot_ui/selection_transition_animation")).toBool();
}

bool ScreenshotUiSettings::setSelectionTransitionAnimationEnabled(bool enabled) const {
    return cache().setValue(QStringLiteral("screenshot_ui/selection_transition_animation"),
                            enabled);
}

QString ScreenshotUiSettings::colorPickerDisplayMode() const {
    return cache().value(QStringLiteral("screenshot_ui/color_picker_display_mode")).toString();
}

bool ScreenshotUiSettings::setColorPickerDisplayMode(const QString& mode) const {
    return cache().setValue(QStringLiteral("screenshot_ui/color_picker_display_mode"), mode);
}

QColor ScreenshotUiSettings::selectionMaskColor() const {
    return colorValue(QStringLiteral("screenshot_ui/selection_mask_color"));
}

bool ScreenshotUiSettings::setSelectionMaskColor(const QColor& color) const {
    return setColorValue(QStringLiteral("screenshot_ui/selection_mask_color"), color);
}

int ScreenshotUiSettings::shortcutHintOpacity() const {
    return cache().value(QStringLiteral("screenshot_ui/shortcut_hint_opacity")).toInt();
}

bool ScreenshotUiSettings::setShortcutHintOpacity(int opacity) const {
    return cache().setValue(QStringLiteral("screenshot_ui/shortcut_hint_opacity"), opacity);
}

QColor ScreenshotUiSettings::cursorGuideLineColor() const {
    return colorValue(QStringLiteral("screenshot_ui/cursor_guide_line_color"));
}

bool ScreenshotUiSettings::setCursorGuideLineColor(const QColor& color) const {
    return setColorValue(QStringLiteral("screenshot_ui/cursor_guide_line_color"), color);
}

QColor ScreenshotUiSettings::monitorCenterGuideLineColor() const {
    return colorValue(QStringLiteral("screenshot_ui/monitor_center_guide_line_color"));
}

bool ScreenshotUiSettings::setMonitorCenterGuideLineColor(const QColor& color) const {
    return setColorValue(QStringLiteral("screenshot_ui/monitor_center_guide_line_color"), color);
}

QColor ScreenshotUiSettings::colorPickerCenterGuideLineColor() const {
    return colorValue(QStringLiteral("screenshot_ui/color_picker_center_guide_line_color"));
}

bool ScreenshotUiSettings::setColorPickerCenterGuideLineColor(const QColor& color) const {
    return setColorValue(QStringLiteral("screenshot_ui/color_picker_center_guide_line_color"),
                         color);
}

bool RecordingSettings::microphoneEnabled() const {
    return cache().value(QStringLiteral("screen_recording/enable_microphone")).toBool();
}

bool RecordingSettings::setMicrophoneEnabled(bool enabled) const {
    return cache().setValue(QStringLiteral("screen_recording/enable_microphone"), enabled);
}

bool RecordingSettings::systemAudioEnabled() const {
    return cache().value(QStringLiteral("screen_recording/enable_system_audio")).toBool();
}

bool RecordingSettings::setSystemAudioEnabled(bool enabled) const {
    return cache().setValue(QStringLiteral("screen_recording/enable_system_audio"), enabled);
}

QString RecordingSettings::screenRecordingClarity() const {
    return cache().value(QStringLiteral("screen_recording/clarity")).toString();
}

bool RecordingSettings::setScreenRecordingClarity(const QString& clarity) const {
    return cache().setValue(QStringLiteral("screen_recording/clarity"), clarity);
}

int RecordingSettings::frameRate() const {
    return cache().value(QStringLiteral("screen_recording/frame_rate")).toInt();
}

bool RecordingSettings::setFrameRate(int frameRate) const {
    return cache().setValue(QStringLiteral("screen_recording/frame_rate"), frameRate);
}

QString RecordingSettings::animatedImageClarity() const {
    return cache().value(QStringLiteral("screen_recording/animated_image_clarity")).toString();
}

bool RecordingSettings::setAnimatedImageClarity(const QString& clarity) const {
    return cache().setValue(QStringLiteral("screen_recording/animated_image_clarity"), clarity);
}

int RecordingSettings::animatedImageFrameRate() const {
    return cache().value(QStringLiteral("screen_recording/animated_image_frame_rate")).toInt();
}

bool RecordingSettings::setAnimatedImageFrameRate(int frameRate) const {
    return cache().setValue(QStringLiteral("screen_recording/animated_image_frame_rate"),
                            frameRate);
}

QString RecordingSettings::animatedImageFormat() const {
    return cache().value(QStringLiteral("screen_recording/animated_image_format")).toString();
}

bool RecordingSettings::setAnimatedImageFormat(const QString& format) const {
    return cache().setValue(QStringLiteral("screen_recording/animated_image_format"), format);
}

QString RecordingSettings::encoder() const {
    return cache().value(QStringLiteral("screen_recording/encoder")).toString();
}

bool RecordingSettings::setEncoder(const QString& encoder) const {
    return cache().setValue(QStringLiteral("screen_recording/encoder"), encoder);
}

QString RecordingSettings::encodingPreset() const {
    return cache().value(QStringLiteral("screen_recording/encoding_preset")).toString();
}

bool RecordingSettings::setEncodingPreset(const QString& preset) const {
    return cache().setValue(QStringLiteral("screen_recording/encoding_preset"), preset);
}

bool RecordingSettings::hideToolbarInRecording() const {
    return cache().value(QStringLiteral("screen_recording/hide_toolbar_in_recording")).toBool();
}

bool RecordingSettings::setHideToolbarInRecording(bool hide) const {
    return cache().setValue(QStringLiteral("screen_recording/hide_toolbar_in_recording"), hide);
}

QString RecordingSettings::videoSaveDirectory() const {
    return cache().value(QStringLiteral("screen_recording/video_save_directory")).toString();
}

bool RecordingSettings::setVideoSaveDirectory(const QString& directory) const {
    return cache().setValue(QStringLiteral("screen_recording/video_save_directory"), directory);
}

QString RecordingSettings::videoFilenameFormat() const {
    return cache().value(QStringLiteral("screen_recording/video_filename_format")).toString();
}

bool RecordingSettings::setVideoFilenameFormat(const QString& format) const {
    return cache().setValue(QStringLiteral("screen_recording/video_filename_format"), format);
}

QString ScreenshotToolbarSettings::arrowLineTool() const {
    return cache().value(QStringLiteral("screenshot_toolbar/arrow_line_tool")).toString();
}

bool ScreenshotToolbarSettings::setArrowLineTool(const QString& tool) const {
    return cache().setValue(QStringLiteral("screenshot_toolbar/arrow_line_tool"), tool);
}

QString ScreenshotToolbarSettings::highlightTool() const {
    return cache().value(QStringLiteral("screenshot_toolbar/highlight_tool")).toString();
}

bool ScreenshotToolbarSettings::setHighlightTool(const QString& tool) const {
    return cache().setValue(QStringLiteral("screenshot_toolbar/highlight_tool"), tool);
}

QString ScreenshotToolbarSettings::tableQrTool() const {
    return cache().value(QStringLiteral("screenshot_toolbar/table_qr_tool")).toString();
}

bool ScreenshotToolbarSettings::setTableQrTool(const QString& tool) const {
    return cache().setValue(QStringLiteral("screenshot_toolbar/table_qr_tool"), tool);
}

ScreenshotToolbarLayout ScreenshotToolbarSettings::layout() const {
    const QJsonObject object =
        cache().value(QStringLiteral("screenshot_toolbar/layout")).toObject();
    return {stringListArray(object.value(QStringLiteral("positions"))),
            stringList(object.value(QStringLiteral("hidden")))};
}

ScreenshotTranslationConfiguration ScreenshotTranslationSettings::configuration() const {
    return {cache().value(QStringLiteral("screenshot_translation/source_language")).toString(),
            cache().value(QStringLiteral("screenshot_translation/target_language")).toString(),
            cache().value(QStringLiteral("screenshot_translation/model")).toString()};
}

bool ScreenshotTranslationSettings::setConfiguration(
    const ScreenshotTranslationConfiguration& configuration) const {
    return cache().setValues({
        {QStringLiteral("screenshot_translation/source_language"), configuration.sourceLanguage},
        {QStringLiteral("screenshot_translation/target_language"), configuration.targetLanguage},
        {QStringLiteral("screenshot_translation/model"), configuration.modelId},
    });
}

bool ScreenshotToolbarSettings::setLayout(const ScreenshotToolbarLayout& layout) const {
    return cache().setValue(
        QStringLiteral("screenshot_toolbar/layout"),
        QJsonObject{{QStringLiteral("positions"), stringArrayArray(layout.positions)},
                    {QStringLiteral("hidden"), stringArray(layout.hidden)}});
}

QColor PinToScreenSettings::borderColor() const {
    return colorValue(QStringLiteral("pin_to_screen/border_color"));
}

bool PinToScreenSettings::setBorderColor(const QColor& color) const {
    return setColorValue(QStringLiteral("pin_to_screen/border_color"), color);
}

QString PinToScreenSettings::mouseWheelZoomMode() const {
    return cache().value(QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode")).toString();
}

bool PinToScreenSettings::setMouseWheelZoomMode(const QString& mode) const {
    return cache().setValue(QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode"), mode);
}

bool PinToScreenSettings::automaticTextRecognition() const {
    return cache().value(QStringLiteral("pin_to_screen/automatic_text_recognition")).toBool();
}

bool PinToScreenSettings::setAutomaticTextRecognition(bool enabled) const {
    return cache().setValue(QStringLiteral("pin_to_screen/automatic_text_recognition"), enabled);
}

bool PinToScreenSettings::autoResizeWindow() const {
    return cache().value(QStringLiteral("pin_to_screen/auto_resize_window")).toBool();
}

bool PinToScreenSettings::setAutoResizeWindow(bool enabled) const {
    return cache().setValue(QStringLiteral("pin_to_screen/auto_resize_window"), enabled);
}

bool TraySettings::enabled() const {
    return cache().value(QStringLiteral("tray/enabled")).toBool();
}

bool TraySettings::setEnabled(bool enabled) const {
    return cache().setValue(QStringLiteral("tray/enabled"), enabled);
}

QString TraySettings::icon() const {
    return cache().value(QStringLiteral("tray/icon")).toString();
}

bool TraySettings::setIcon(const QString& icon) const {
    return cache().setValue(QStringLiteral("tray/icon"), icon);
}

QString TraySettings::customIcon() const {
    return cache().value(QStringLiteral("tray/custom_icon")).toString();
}

bool TraySettings::setCustomIcon(const QString& path) const {
    return cache().setValue(QStringLiteral("tray/custom_icon"), path);
}

QString TraySettings::leftClickAction() const {
    return cache().value(QStringLiteral("tray/left_click_action")).toString();
}

bool TraySettings::setLeftClickAction(const QString& action) const {
    return cache().setValue(QStringLiteral("tray/left_click_action"), action);
}

QStringList TraySettings::menuOptions() const {
    return stringList(cache().value(QStringLiteral("tray/menu_options")));
}

bool TraySettings::setMenuOptions(const QStringList& options) const {
    return cache().setValue(QStringLiteral("tray/menu_options"), stringArray(options));
}

bool SystemSettings::autoStartAtBoot() const {
    return cache().value(QStringLiteral("system/auto_start_at_boot")).toBool();
}

bool SystemSettings::setAutoStartAtBoot(bool enabled) const {
    return cache().setValue(QStringLiteral("system/auto_start_at_boot"), enabled);
}

QString NetworkSettings::proxy() const {
    return cache().value(QStringLiteral("network/proxy")).toString();
}

bool NetworkSettings::setProxy(const QString& proxy) const {
    return cache().setValue(QStringLiteral("network/proxy"), proxy);
}

CaptureHistoryPolicy HistorySettings::policy() const {
    return captureHistoryPolicyFromConfiguration(cache());
}

std::shared_future<StorageResult>
HistorySettings::setPolicy(const CaptureHistoryPolicy& policy) const {
    auto& storage = ApplicationStorage::instance();
    if (!storage.isInitialized()) {
        static_cast<void>(storage.initialize());
    }
    return storage.requestCaptureHistoryPolicyAsync(policy);
}

std::shared_future<StorageResult> HistorySettings::setEnabled(bool enabled) const {
    CaptureHistoryPolicy next = policy();
    next.enabled = enabled;
    return setPolicy(next);
}

std::shared_future<StorageResult> HistorySettings::setRetentionDays(int days) const {
    CaptureHistoryPolicy next = policy();
    next.retentionDays = days;
    return setPolicy(next);
}

std::shared_future<StorageResult> HistorySettings::setMaxEntries(int entries) const {
    CaptureHistoryPolicy next = policy();
    next.maxEntries = entries;
    return setPolicy(next);
}

std::shared_future<StorageResult> HistorySettings::setMaxDiskMiB(int mebibytes) const {
    CaptureHistoryPolicy next = policy();
    next.maxDiskMiB = mebibytes;
    return setPolicy(next);
}
} // namespace snow_shot::storage
