#include "snow_shot/storage/configurationschema.h"

#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/storage/persistedselectioncodec.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QDir>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace snow_shot::storage {
namespace {
const QStringList kDrawingToolbarItemIds = {
    QStringLiteral("shape"),     QStringLiteral("arrow"),
    QStringLiteral("line"),      QStringLiteral("free-draw"),
    QStringLiteral("highlighter"), QStringLiteral("spotlight"),
    QStringLiteral("text"),      QStringLiteral("serial-number"),
    QStringLiteral("filter"),    QStringLiteral("eraser"),
    QStringLiteral("watermark"),
};

const QStringList kLegacyScreenshotToolbarItemIds = {
    QStringLiteral("move"),
    QStringLiteral("select"),
    QStringLiteral("shape"),
    QStringLiteral("arrow-line"),
    QStringLiteral("free-draw"),
    QStringLiteral("highlight"),
    QStringLiteral("text"),
    QStringLiteral("serial-number"),
    QStringLiteral("filter"),
    QStringLiteral("eraser"),
    QStringLiteral("watermark"),
    QStringLiteral("history"),
    QStringLiteral("table-qr"),
    QStringLiteral("video-record"),
    QStringLiteral("screen-record"),
    QStringLiteral("pin"),
    QStringLiteral("ocr"),
    QStringLiteral("scrolling-screenshot"),
};

QJsonArray jsonArray(const QStringList& values) {
    QJsonArray result;
    for (const QString& value : values) {
        result.push_back(value);
    }
    return result;
}

QJsonArray jsonArray(const QVector<QStringList>& values) {
    QJsonArray result;
    for (const QStringList& value : values) {
        result.push_back(jsonArray(value));
    }
    return result;
}

QVector<QStringList> defaultDrawingToolbarPositions() {
    return {
        {QStringLiteral("shape")},
        {QStringLiteral("line"), QStringLiteral("arrow")},
        {QStringLiteral("free-draw")},
        {QStringLiteral("spotlight"), QStringLiteral("highlighter")},
        {QStringLiteral("text")},
        {QStringLiteral("serial-number")},
        {QStringLiteral("filter")},
        {QStringLiteral("eraser")},
        {QStringLiteral("watermark")},
    };
}

QJsonObject defaultScreenshotToolbarLayout() {
    return {{QStringLiteral("positions"), jsonArray(defaultDrawingToolbarPositions())},
            {QStringLiteral("hidden"), QJsonArray()}};
}

QString defaultOutputDirectory(QStandardPaths::StandardLocation primary) {
    QString root = QStandardPaths::writableLocation(primary);
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    if (root.isEmpty()) {
        return {};
    }
    return QDir(root).filePath(QStringLiteral("SnowShot"));
}

const QVector<ConfigurationSchemaEntry> kEntries = {
    {QStringLiteral("storage/schema_version"), 1, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{1, 1, 1}},
    {QStringLiteral("interface/theme_mode"),
     QStringLiteral("system"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark")}},
    {QStringLiteral("interface/language"), QStringLiteral("system"),
     ConfigurationValueKind::String},
    {QStringLiteral("system/application_priority"),
     QStringLiteral("above_normal"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("normal"), QStringLiteral("above_normal"), QStringLiteral("high"),
      QStringLiteral("real_time")}},
    {QStringLiteral("system/auto_start_at_boot"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("network/proxy"),
     QStringLiteral("none"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("none"), QStringLiteral("system")}},
    {QStringLiteral("text_recognition/direct_ml_acceleration"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot_translation/source_language"), QStringLiteral("auto"),
     ConfigurationValueKind::String, std::nullopt,
     {QStringLiteral("auto"), QStringLiteral("ar"), QStringLiteral("de"),
      QStringLiteral("en"), QStringLiteral("es"), QStringLiteral("fr"),
      QStringLiteral("it"), QStringLiteral("ja"), QStringLiteral("pt"),
      QStringLiteral("ru"), QStringLiteral("tr"), QStringLiteral("zh-Hans"),
      QStringLiteral("zh-Hant")}},
    {QStringLiteral("screenshot_translation/target_language"), QString(),
     ConfigurationValueKind::String, std::nullopt,
     {QStringLiteral("ar"), QStringLiteral("de"), QStringLiteral("en"),
      QStringLiteral("es"), QStringLiteral("fr"), QStringLiteral("it"),
      QStringLiteral("ja"), QStringLiteral("pt"), QStringLiteral("ru"),
      QStringLiteral("tr"), QStringLiteral("zh-Hans"), QStringLiteral("zh-Hant")}},
    {QStringLiteral("screenshot_translation/model"), QString(), ConfigurationValueKind::String},
    {QStringLiteral("interface/sidebar_collapsed"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("global_shortcuts/screenshot"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/screenshot_delay"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/screenshot_fixed"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/screenshot_ocr"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/screenshot_translation"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/screenshot_copy"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/screenshot_full_screen"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/screenshot_focused_window"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/screen_record"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/screen_record_copy"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/open_capture_history"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/open_settings"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/pin_clipboard_content"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/enable_microphone"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/enable_system_audio"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/clarity"),
     QStringLiteral("1080p"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("4k"), QStringLiteral("2k"), QStringLiteral("1080p"),
      QStringLiteral("720p"), QStringLiteral("480p")}},
    {QStringLiteral("screen_recording/frame_rate"), 30, ConfigurationValueKind::Integer},
    {QStringLiteral("screen_recording/animated_image_clarity"),
     QStringLiteral("1080p"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("1080p"), QStringLiteral("720p"), QStringLiteral("480p")}},
    {QStringLiteral("screen_recording/animated_image_frame_rate"), 10,
     ConfigurationValueKind::Integer},
    {QStringLiteral("screen_recording/animated_image_format"),
     QStringLiteral("gif"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("gif"), QStringLiteral("apng"), QStringLiteral("webp")}},
    {QStringLiteral("screen_recording/encoder"),
     QStringLiteral("h264"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("h264"), QStringLiteral("h265")}},
    {QStringLiteral("screen_recording/encoding_preset"),
     QStringLiteral("veryfast"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("ultrafast"), QStringLiteral("veryfast"), QStringLiteral("medium"),
      QStringLiteral("veryslow"), QStringLiteral("placebo")}},
    {QStringLiteral("screen_recording/hide_toolbar_in_recording"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/video_save_directory"),
     defaultOutputDirectory(QStandardPaths::MoviesLocation), ConfigurationValueKind::String},
    {QStringLiteral("screen_recording/video_filename_format"),
     QStringLiteral("SnowShot_Video_{YYYY-MM-DD_HH-mm-ss}"), ConfigurationValueKind::String},
    {QStringLiteral("drawing/quick_selection_disabled_tools"),
     QJsonArray{QStringLiteral("free-draw"), QStringLiteral("pen-filter")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("line"),
      QStringLiteral("free-draw"), QStringLiteral("rectangle-highlight"),
      QStringLiteral("pen-highlight"), QStringLiteral("spotlight"),
      QStringLiteral("rectangle-filter"), QStringLiteral("pen-filter"),
      QStringLiteral("text"), QStringLiteral("serial-number"), QStringLiteral("eraser"),
      QStringLiteral("watermark")}},
    {QStringLiteral("drawing_shortcuts/select"), QJsonArray{QStringLiteral("V")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("drawing_shortcuts/shape"), QJsonArray{QStringLiteral("1")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("drawing_shortcuts/arrow"), QJsonArray{QStringLiteral("2")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("drawing_shortcuts/brush"),
     QJsonArray{QStringLiteral("3"), QStringLiteral("P")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("drawing_shortcuts/highlight"),
     QJsonArray{QStringLiteral("4"), QStringLiteral("H")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("drawing_shortcuts/text"),
     QJsonArray{QStringLiteral("5"), QStringLiteral("T")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("drawing_shortcuts/serial_number"),
     QJsonArray{QStringLiteral("6"), QStringLiteral("N")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("drawing_shortcuts/filter"),
     QJsonArray{QStringLiteral("7"), QStringLiteral("F")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("drawing_shortcuts/eraser"),
     QJsonArray{QStringLiteral("8"), QStringLiteral("E")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("drawing_shortcuts/watermark"), QJsonArray{QStringLiteral("9")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/move_tool"), QJsonArray{QStringLiteral("M")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/move_cursor_up"),
     QJsonArray{QStringLiteral("W"), QStringLiteral("Up")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/move_cursor_down"),
     QJsonArray{QStringLiteral("S"), QStringLiteral("Down")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/move_cursor_left"),
     QJsonArray{QStringLiteral("A"), QStringLiteral("Left")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/move_cursor_right"),
     QJsonArray{QStringLiteral("D"), QStringLiteral("Right")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/move_entire_selection"),
     QJsonArray{QStringLiteral("Space")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/keep_selection_width_and_height_consistent"),
     QJsonArray{QStringLiteral("Shift")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/switch_selection_between_window_and_window_sub_element"),
     QJsonArray{QStringLiteral("Tab")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/previous_screenshot_history"),
     QJsonArray{QStringLiteral(",")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/next_screenshot_history"),
     QJsonArray{QStringLiteral(".")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/select_previously_selected_area"),
     QJsonArray{QStringLiteral("R")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/copy_color"), QJsonArray{QStringLiteral("C")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/table_recognition"), QJsonArray{QStringLiteral("Ctrl+X")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/qr_code_recognition"), QJsonArray{QStringLiteral("Ctrl+Q")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/video_recording"), QJsonArray{QStringLiteral("Ctrl+R")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/text_recognition"), QJsonArray{QStringLiteral("Ctrl+D")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/text_translation"), QJsonArray{QStringLiteral("Ctrl+T")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/scrolling_screenshot"), QJsonArray{QStringLiteral("L")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/save_as_file"), QJsonArray{QStringLiteral("Ctrl+S")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/pin_to_screen"), QJsonArray{QStringLiteral("Ctrl+F")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/cancel_screenshot"), QJsonArray{QStringLiteral("Esc")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/copy_to_clipboard"),
     QJsonArray{QStringLiteral("Ctrl+C")}, ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/undo"), QJsonArray{QStringLiteral("Ctrl+Z")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("screenshot_shortcuts/redo"), QJsonArray{QStringLiteral("Ctrl+Y")},
     ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/copy_to_clipboard"),
     QJsonArray{QStringLiteral("Ctrl+C")}, ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/copy_original_content"),
     QJsonArray{QStringLiteral("Ctrl+Shift+C")}, ConfigurationValueKind::StringList, std::nullopt,
     {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/show_text_recognition_results"),
     QJsonArray{QStringLiteral("Ctrl+D")}, ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/drawing_mode"),
     QJsonArray{QStringLiteral("Ctrl+E")}, ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/thumbnail_mode"),
     QJsonArray{QStringLiteral("R")}, ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/close_window"),
     QJsonArray{QStringLiteral("Esc")}, ConfigurationValueKind::StringList, std::nullopt, {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/move_cursor_up"),
     QJsonArray{QStringLiteral("W"), QStringLiteral("Up")}, ConfigurationValueKind::StringList,
     std::nullopt, {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/move_cursor_down"),
     QJsonArray{QStringLiteral("S"), QStringLiteral("Down")}, ConfigurationValueKind::StringList,
     std::nullopt, {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/move_cursor_left"),
     QJsonArray{QStringLiteral("A"), QStringLiteral("Left")}, ConfigurationValueKind::StringList,
     std::nullopt, {}, 2},
    {QStringLiteral("pin_to_screen_shortcuts/move_cursor_right"),
     QJsonArray{QStringLiteral("D"), QStringLiteral("Right")}, ConfigurationValueKind::StringList,
     std::nullopt, {}, 2},
    {QStringLiteral("screenshot_toolbar/arrow_line_tool"),
     QStringLiteral("arrow"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("arrow"), QStringLiteral("line")}},
    {QStringLiteral("screenshot_toolbar/highlight_tool"),
     QStringLiteral("pen_highlight"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("pen_highlight"), QStringLiteral("highlight"), QStringLiteral("spotlight")}},
    {QStringLiteral("screenshot_toolbar/table_qr_tool"),
     QStringLiteral("table"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("table"), QStringLiteral("qr")}},
    {QStringLiteral("screenshot_toolbar/layout"), defaultScreenshotToolbarLayout(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("screenshot_ui/toolbar_size"),
     QStringLiteral("normal"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("small"), QStringLiteral("normal")}},
    {QStringLiteral("screenshot_ui/selection_transition_animation"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot_ui/color_picker_display_mode"),
     QStringLiteral("hide_outside_selection"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("hide_outside_selection"), QStringLiteral("always_show"),
      QStringLiteral("always_hide")}},
    {QStringLiteral("screenshot_ui/selection_mask_color"), QStringLiteral("#00000080"),
     ConfigurationValueKind::String},
    {QStringLiteral("screenshot_ui/shortcut_hint_opacity"), 100, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{0, 100, 1}},
    {QStringLiteral("screenshot_ui/cursor_guide_line_color"), QStringLiteral("#00000000"),
     ConfigurationValueKind::String},
    {QStringLiteral("screenshot_ui/monitor_center_guide_line_color"), QStringLiteral("#00000000"),
     ConfigurationValueKind::String},
    {QStringLiteral("screenshot_ui/color_picker_center_guide_line_color"),
     QStringLiteral("#00000000"), ConfigurationValueKind::String},
    {QStringLiteral("pin_to_screen/border_color"), QStringLiteral("#DBDBDBFF"),
     ConfigurationValueKind::String},
    {QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode"),
     QStringLiteral("mouse_position"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("mouse_position"), QStringLiteral("top_left"),
      QStringLiteral("top_right"), QStringLiteral("bottom_left"),
      QStringLiteral("bottom_right"), QStringLiteral("center")}},
    {QStringLiteral("pin_to_screen/automatic_text_recognition"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("pin_to_screen/auto_resize_window"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("tray/enabled"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("tray/icon"),
     QStringLiteral("default"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("default"), QStringLiteral("light"), QStringLiteral("dark"),
      QStringLiteral("snow-default"), QStringLiteral("snow-light"), QStringLiteral("snow-dark")}},
    {QStringLiteral("tray/custom_icon"), QString(), ConfigurationValueKind::String},
    {QStringLiteral("tray/left_click_action"),
     QStringLiteral("screenshot"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("screenshot"), QStringLiteral("show_main_window")}},
    {QStringLiteral("tray/menu_options"),
     QJsonArray{QStringLiteral("quick.screenshot"),
                QStringLiteral("quick.screenshot-delay"),
                QStringLiteral("quick.screenshot-fixed"),
                QStringLiteral("quick.screenshot-ocr"),
                QStringLiteral("quick.screenshot-copy"),
                QStringLiteral("quick.screen-record"),
                QStringLiteral("quick.pin-clipboard-content"),
                QStringLiteral("tray.disable-shortcut-functions"),
                QStringLiteral("tray.show-main-window"),
                QStringLiteral("tray.exit")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {QStringLiteral("quick.screenshot"),
      QStringLiteral("quick.screenshot-delay"),
      QStringLiteral("quick.screenshot-fixed"),
      QStringLiteral("quick.screenshot-ocr"),
      QStringLiteral("quick.screenshot-translation"),
      QStringLiteral("quick.screenshot-copy"),
      QStringLiteral("quick.screenshot-full-screen"),
      QStringLiteral("quick.screenshot-focused-window"),
      QStringLiteral("quick.screen-record"),
      QStringLiteral("quick.screen-record-copy"),
      QStringLiteral("quick.open-capture-history"),
      QStringLiteral("quick.pin-clipboard-content"),
      QStringLiteral("tray.disable-shortcut-functions"),
      QStringLiteral("tray.show-main-window"),
      QStringLiteral("tray.exit")},
     15},
    {QStringLiteral("screenshot_selection/previous_selection"), QJsonValue::Null,
     ConfigurationValueKind::Structured},
    {QStringLiteral("screenshot_selection/smart_selection"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/delay_seconds"), 3, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{1, 10, 1}},
    {QStringLiteral("screenshot/auto_execute_after_text_recognition"),
     QStringLiteral("no_action"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("no_action"), QStringLiteral("copy_text"),
      QStringLiteral("copy_text_and_end_screenshot"), QStringLiteral("quick_copy_text"),
      QStringLiteral("quick_copy_text_and_end_screenshot"),
      QStringLiteral("enable_edit_mode")}},
    {QStringLiteral("screenshot/double_click_action"),
     QStringLiteral("copy"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("copy"), QStringLiteral("save"), QStringLiteral("pin"),
      QStringLiteral("none")}},
    {QStringLiteral("screenshot/middle_mouse_button_action"),
     QStringLiteral("pin"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("copy"), QStringLiteral("save"), QStringLiteral("pin"),
      QStringLiteral("none")}},
    {QStringLiteral("screenshot/auto_save_after_copy"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/copy_image_file_to_clipboard"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/image_save_directory"),
     defaultOutputDirectory(QStandardPaths::PicturesLocation), ConfigurationValueKind::String},
    {QStringLiteral("screenshot/last_manual_save_directory"), QString(),
     ConfigurationValueKind::String},
    {QStringLiteral("screenshot/image_format"),
     QStringLiteral("png"), ConfigurationValueKind::String, std::nullopt,
     {QStringLiteral("png"), QStringLiteral("jpeg"), QStringLiteral("webp"),
      QStringLiteral("jxl"), QStringLiteral("avif")}},
    {QStringLiteral("screenshot/manual_save_filename_format"),
     QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"), ConfigurationValueKind::String},
    {QStringLiteral("screenshot/auto_save_filename_format"),
     QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"), ConfigurationValueKind::String},
    {QStringLiteral("screenshot_selection/selection_rect_presets"), QJsonArray(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("capture_history/enabled"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("capture_history/retention_days"), 7, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{CaptureHistoryPolicy::MinimumRetentionDays,
                               CaptureHistoryPolicy::MaximumRetentionDays, 1}},
    {QStringLiteral("capture_history/max_entries"), 100, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{CaptureHistoryPolicy::MinimumEntries,
                               CaptureHistoryPolicy::MaximumEntries, 1}},
    {QStringLiteral("capture_history/max_disk_mib"), 1024, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{CaptureHistoryPolicy::MinimumDiskMiB,
                               CaptureHistoryPolicy::MaximumDiskMiB, 1}},
};

bool isInteger(const QJsonValue& value, int* result = nullptr) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble()) {
        return false;
    }
    if (value.toDouble() < static_cast<double>(std::numeric_limits<int>::min()) ||
        value.toDouble() > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (result != nullptr) {
        *result = value.toInt();
    }
    return true;
}

ConfigurationNormalization exactType(const QJsonValue& value, QJsonValue::Type type) {
    return {value, value.type() == type, false};
}

ConfigurationNormalization normalizeIntegerRange(const QJsonValue& value, int minimum,
                                                 int maximum) {
    int integer = 0;
    if (!isInteger(value, &integer) || integer < minimum || integer > maximum) {
        return {};
    }
    return {integer, true, false};
}

ConfigurationNormalization normalizeTheme(const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    const QString normalized = value.toString().trimmed().toLower();
    if (normalized != QStringLiteral("system") && normalized != QStringLiteral("light") &&
        normalized != QStringLiteral("dark")) {
        return {};
    }
    return {normalized, true, normalized != value.toString()};
}

ConfigurationNormalization normalizeLanguage(const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    QString normalized = value.toString().trimmed();
    if (normalized.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0) {
        normalized = QStringLiteral("system");
    } else {
        normalized.replace(u'-', u'_');
        if (normalized.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0) {
            normalized = QStringLiteral("en_US");
        } else {
            static const QRegularExpression localePattern(
                QStringLiteral("^[A-Za-z]{2,3}(?:_[A-Za-z0-9]{2,8})*$"));
            if (!localePattern.match(normalized).hasMatch()) {
                return {};
            }
            const QLocale locale(normalized);
            if (locale.language() == QLocale::AnyLanguage || locale.name() == QStringLiteral("C")) {
                return {};
            }
            normalized = locale.name();
        }
    }
    return {normalized, true, normalized != value.toString()};
}

QString canonicalShortcut(const QString& input, bool allowModifierOnlyShift) {
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    // Shift is intentionally supported as a modifier-only local shortcut. Qt
    // reports its press as Key_Shift with ShiftModifier, which is distinct
    // from an ordinary Shift-modified key combination.
    if (allowModifierOnlyShift &&
        trimmed.compare(QStringLiteral("Shift"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Shift");
    }
    QKeySequence sequence = QKeySequence::fromString(trimmed, QKeySequence::PortableText);
    if (sequence.isEmpty()) {
        sequence = QKeySequence::fromString(trimmed, QKeySequence::NativeText);
    }
    if (sequence.count() != 1) {
        return {};
    }
    const QKeyCombination combination = sequence[0];
    const Qt::Key key = combination.key();
    if (allowModifierOnlyShift && key == Qt::Key_Shift &&
        combination.keyboardModifiers() == Qt::ShiftModifier) {
        return QStringLiteral("Shift");
    }
    if (key == Qt::Key_unknown || key == Qt::Key_Control || key == Qt::Key_Alt ||
        key == Qt::Key_Shift || key == Qt::Key_Meta || key == Qt::Key_AltGr ||
        key == Qt::Key_Super_L || key == Qt::Key_Super_R) {
        return {};
    }
    const QString portable = sequence.toString(QKeySequence::PortableText).trimmed();
    return portable;
}

ConfigurationNormalization normalizeShortcuts(const QJsonValue& value, int maximumItems,
                                              bool allowModifierOnlyShift) {
    if (!value.isArray()) {
        return {};
    }
    QJsonArray normalized;
    QSet<QString> seen;
    bool changed = false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString()) {
            changed = true;
            continue;
        }
        const QString shortcut = canonicalShortcut(item.toString(), allowModifierOnlyShift);
        if (shortcut.isEmpty() || seen.contains(shortcut) ||
            (maximumItems >= 0 && normalized.size() >= maximumItems)) {
            changed = true;
            continue;
        }
        seen.insert(shortcut);
        normalized.push_back(shortcut);
        changed = changed || shortcut != item.toString();
    }
    return {normalized, true, changed};
}

ConfigurationNormalization normalizeAllowedStringList(
    const ConfigurationSchemaEntry& schemaEntry, const QJsonValue& value) {
    if (!value.isArray()) {
        return {};
    }
    QJsonArray normalized;
    QSet<QString> seen;
    bool changed = false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString()) {
            changed = true;
            continue;
        }
        const QString original = item.toString();
        const QString candidate = original.trimmed();
        const auto canonical = std::find_if(
            schemaEntry.allowedStringValues.cbegin(), schemaEntry.allowedStringValues.cend(),
            [&candidate](const QString& allowed) {
                return allowed.compare(candidate, Qt::CaseInsensitive) == 0;
            });
        if (canonical == schemaEntry.allowedStringValues.cend() || seen.contains(*canonical) ||
            (schemaEntry.maximumListItems >= 0 &&
             normalized.size() >= schemaEntry.maximumListItems)) {
            changed = true;
            continue;
        }
        seen.insert(*canonical);
        normalized.push_back(*canonical);
        changed = changed || *canonical != original;
    }
    return {normalized, true, changed};
}

ConfigurationNormalization normalizeAllowedInteger(const QJsonValue& value,
                                                   std::initializer_list<int> allowed) {
    int integer = 0;
    if (!isInteger(value, &integer) ||
        std::find(allowed.begin(), allowed.end(), integer) == allowed.end()) {
        return {};
    }
    return {integer, true, false};
}

ConfigurationNormalization normalizeSelection(const QJsonValue& value) {
    if (value.isNull()) {
        return {QJsonValue::Null, true, false};
    }
    const PersistedSelectionNormalization normalized = normalizePersistedSelection(value);
    if (!normalized.valid) {
        return {};
    }
    return {persistedSelectionToJson(normalized.value), true, normalized.changed};
}

ConfigurationNormalization normalizePresets(const QJsonValue& value) {
    if (!value.isArray()) {
        return {};
    }
    QJsonArray result;
    bool changed = false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isObject()) {
            changed = true;
            continue;
        }
        const QJsonObject itemObject = item.toObject();
        const QString name = itemObject.value(QStringLiteral("name")).toString().trimmed();
        const PersistedSelectionNormalization normalized = normalizePersistedSelection(item);
        if (name.isEmpty() || !normalized.valid) {
            changed = true;
            continue;
        }
        QJsonObject normalizedSelection = persistedSelectionToJson(normalized.value);
        normalizedSelection.insert(QStringLiteral("name"), name);
        result.push_back(normalizedSelection);
        changed = changed || normalized.changed || normalizedSelection != itemObject;
    }
    return {result, true, changed};
}

bool isRgbaColorKey(const QString& key) {
    return key == QStringLiteral("screenshot_ui/selection_mask_color") ||
           key == QStringLiteral("screenshot_ui/cursor_guide_line_color") ||
           key == QStringLiteral("screenshot_ui/monitor_center_guide_line_color") ||
           key == QStringLiteral("screenshot_ui/color_picker_center_guide_line_color") ||
           key == QStringLiteral("pin_to_screen/border_color");
}

ConfigurationNormalization normalizeRgbaColor(const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    const QString original = value.toString();
    const QString normalized = original.trimmed().toUpper();
    static const QRegularExpression pattern(QStringLiteral("^#[0-9A-F]{8}$"));
    if (!pattern.match(normalized).hasMatch()) {
        return {};
    }
    return {normalized, true, normalized != original};
}

bool isFilenameFormatKey(const QString& key) {
    return key == QStringLiteral("screenshot/manual_save_filename_format") ||
           key == QStringLiteral("screenshot/auto_save_filename_format") ||
           key == QStringLiteral("screen_recording/video_filename_format");
}

ConfigurationNormalization normalizeFilenameFormat(const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    const QString original = value.toString();
    const QString normalized = original.trimmed();
    static const QRegularExpression invalidCharacters(QStringLiteral("[\\\\/:*?\"<>|]"));
    if (normalized.isEmpty() || invalidCharacters.match(normalized).hasMatch()) {
        return {};
    }
    return {normalized, true, normalized != original};
}

ConfigurationNormalization normalizeTranslationLanguage(
    const ConfigurationSchemaEntry& schemaEntry, const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    const QString original = value.toString();
    const QString trimmed = original.trimmed();
    const auto canonical = std::find_if(
        schemaEntry.allowedStringValues.cbegin(), schemaEntry.allowedStringValues.cend(),
        [&trimmed](const QString& allowed) {
            return allowed.compare(trimmed, Qt::CaseInsensitive) == 0;
        });
    if (canonical == schemaEntry.allowedStringValues.cend()) {
        return {};
    }
    return {*canonical, true, *canonical != original};
}

ConfigurationNormalization normalizeToolbarLayout(const QJsonValue& value) {
    if (!value.isObject()) {
        return {};
    }
    const QJsonObject object = value.toObject();
    const QSet<QString> known(kDrawingToolbarItemIds.cbegin(), kDrawingToolbarItemIds.cend());
    QVector<QStringList> positions;
    QSet<QString> positioned;
    QStringList hidden;
    QSet<QString> hiddenSet;
    const auto canonicalItemId = [](const QString& id) {
        return id == QStringLiteral("rectangle-highlight") || id == QStringLiteral("pen-highlight")
                   ? QStringLiteral("highlighter")
                   : id;
    };
    const auto appendPosition = [&positions, &positioned, &hiddenSet,
                                 &known, &canonicalItemId](const QStringList& ids) {
        QStringList position;
        for (const QString& storedId : ids) {
            const QString id = canonicalItemId(storedId);
            if (known.contains(id) && !positioned.contains(id) && !hiddenSet.contains(id)) {
                position.push_back(id);
                positioned.insert(id);
            }
        }
        if (!position.isEmpty()) {
            positions.push_back(position);
        }
    };
    const auto appendHidden = [&hidden, &hiddenSet, &positioned,
                               &known, &canonicalItemId](const QStringList& ids) {
        for (const QString& storedId : ids) {
            const QString id = canonicalItemId(storedId);
            if (known.contains(id) && !positioned.contains(id) && !hiddenSet.contains(id)) {
                hidden.push_back(id);
                hiddenSet.insert(id);
            }
        }
    };

    if (object.value(QStringLiteral("positions")).isArray()) {
        for (const QJsonValue& positionValue :
             object.value(QStringLiteral("positions")).toArray()) {
            if (!positionValue.isArray()) {
                continue;
            }
            QStringList ids;
            for (const QJsonValue& item : positionValue.toArray()) {
                if (item.isString()) {
                    ids.push_back(item.toString());
                }
            }
            appendPosition(ids);
        }
        if (object.value(QStringLiteral("hidden")).isArray()) {
            QStringList hiddenIds;
            for (const QJsonValue& item : object.value(QStringLiteral("hidden")).toArray()) {
                if (item.isString()) {
                    hiddenIds.push_back(item.toString());
                }
            }
            appendHidden(hiddenIds);
        }
    } else if (object.value(QStringLiteral("order")).isArray()) {
        const QSet<QString> legacyKnown(kLegacyScreenshotToolbarItemIds.cbegin(),
                                        kLegacyScreenshotToolbarItemIds.cend());
        QSet<QString> legacyHidden;
        if (object.value(QStringLiteral("hidden")).isArray()) {
            for (const QJsonValue& item : object.value(QStringLiteral("hidden")).toArray()) {
                if (item.isString() && legacyKnown.contains(item.toString())) {
                    legacyHidden.insert(item.toString());
                }
            }
        }
        const auto mappedLegacyIds = [](const QString& id) {
            if (id == QStringLiteral("arrow-line")) {
                return QStringList{QStringLiteral("line"), QStringLiteral("arrow")};
            }
            if (id == QStringLiteral("highlight")) {
                return QStringList{QStringLiteral("spotlight"),
                                   QStringLiteral("highlighter")};
            }
            return QStringList{id};
        };
        for (const QJsonValue& item : object.value(QStringLiteral("hidden")).toArray()) {
            if (item.isString() && legacyHidden.contains(item.toString())) {
                appendHidden(mappedLegacyIds(item.toString()));
            }
        }
        QSet<QString> migratedGroups;
        for (const QJsonValue& item : object.value(QStringLiteral("order")).toArray()) {
            if (!item.isString()) {
                continue;
            }
            const QString id = item.toString();
            if (!legacyKnown.contains(id) || migratedGroups.contains(id)) {
                continue;
            }
            migratedGroups.insert(id);
            if (!legacyHidden.contains(id)) {
                appendPosition(mappedLegacyIds(id));
            }
        }
    } else {
        return {};
    }

    for (const QStringList& defaultPosition : defaultDrawingToolbarPositions()) {
        QStringList missing;
        for (const QString& id : defaultPosition) {
            if (!positioned.contains(id) && !hiddenSet.contains(id)) {
                missing.push_back(id);
            }
        }
        appendPosition(missing);
    }

    const QJsonObject normalized{
        {QStringLiteral("positions"), jsonArray(positions)},
        {QStringLiteral("hidden"), jsonArray(hidden)},
    };
    return {normalized, true, normalized != object};
}

void insertPath(QJsonObject* root, const QString& path, const QJsonValue& value) {
    const QStringList parts = path.split(u'/');
    if (root == nullptr || parts.size() != 2) {
        return;
    }
    QJsonObject group = root->value(parts[0]).toObject();
    group.insert(parts[1], value);
    root->insert(parts[0], group);
}
} // namespace

const QVector<ConfigurationSchemaEntry>& ConfigurationSchema::entries() {
    return kEntries;
}

const ConfigurationSchemaEntry* ConfigurationSchema::entry(const QString& key) {
    const auto found = std::find_if(kEntries.cbegin(), kEntries.cend(),
                                    [&key](const auto& item) { return item.key == key; });
    return found == kEntries.cend() ? nullptr : &*found;
}

bool ConfigurationSchema::contains(const QString& key) {
    return entry(key) != nullptr;
}

QJsonValue ConfigurationSchema::defaultValue(const QString& key) {
    const ConfigurationSchemaEntry* found = entry(key);
    return found == nullptr ? QJsonValue() : found->defaultValue;
}

ConfigurationNormalization ConfigurationSchema::normalize(const QString& key,
                                                          const QJsonValue& value) {
    const ConfigurationSchemaEntry* schemaEntry = entry(key);
    if (schemaEntry == nullptr) {
        return {};
    }
    if (key == QStringLiteral("interface/theme_mode")) {
        return normalizeTheme(value);
    }
    if (key == QStringLiteral("interface/language")) {
        return normalizeLanguage(value);
    }
    if (key == QStringLiteral("screenshot_selection/previous_selection")) {
        return normalizeSelection(value);
    }
    if (key == QStringLiteral("screenshot_selection/selection_rect_presets")) {
        return normalizePresets(value);
    }
    if (key == QStringLiteral("screenshot_toolbar/layout")) {
        return normalizeToolbarLayout(value);
    }
    if (isRgbaColorKey(key)) {
        return normalizeRgbaColor(value);
    }
    if (isFilenameFormatKey(key)) {
        return normalizeFilenameFormat(value);
    }
    if (key == QStringLiteral("screenshot_translation/source_language") ||
        key == QStringLiteral("screenshot_translation/target_language")) {
        return normalizeTranslationLanguage(*schemaEntry, value);
    }
    if (key == QStringLiteral("drawing/quick_selection_disabled_tools")) {
        return normalizeAllowedStringList(*schemaEntry, value);
    }
    if (key == QStringLiteral("tray/menu_options")) {
        return normalizeAllowedStringList(*schemaEntry, value);
    }
    if (key == QStringLiteral("screen_recording/frame_rate")) {
        return normalizeAllowedInteger(value, {10, 15, 24, 30, 60, 120, 83});
    }
    if (key == QStringLiteral("screen_recording/animated_image_frame_rate")) {
        return normalizeAllowedInteger(value, {10, 15, 24});
    }
    switch (schemaEntry->valueKind) {
    case ConfigurationValueKind::Boolean:
        return exactType(value, QJsonValue::Bool);
    case ConfigurationValueKind::Integer:
        if (schemaEntry->integerRange.has_value()) {
            return normalizeIntegerRange(value, schemaEntry->integerRange->minimum,
                                         schemaEntry->integerRange->maximum);
        }
        return isInteger(value) ? ConfigurationNormalization{value, true, false}
                                : ConfigurationNormalization{};
    case ConfigurationValueKind::String: {
        if (!value.isString()) {
            return {};
        }
        const QString normalizedValue = value.toString().trimmed();
        if (!schemaEntry->allowedStringValues.isEmpty() &&
            !schemaEntry->allowedStringValues.contains(normalizedValue)) {
            return {};
        }
        return {normalizedValue, true, normalizedValue != value.toString()};
    }
    case ConfigurationValueKind::StringList:
        return normalizeShortcuts(value, schemaEntry->maximumListItems,
                                  key.startsWith(QStringLiteral("screenshot_shortcuts/")));
    case ConfigurationValueKind::Structured:
        break;
    }
    return {};
}

QJsonObject ConfigurationSchema::completeDefaultDocument() {
    QJsonObject root;
    for (const ConfigurationSchemaEntry& entry : kEntries) {
        insertPath(&root, entry.key, entry.defaultValue);
    }
    return root;
}
} // namespace snow_shot::storage
