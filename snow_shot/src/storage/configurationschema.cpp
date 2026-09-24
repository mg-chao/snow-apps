#include "snow_shot/presentation/globalmousetypes.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/customaimodelconfiguration.h"

#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/storage/persistedselectioncodec.h"
#include "snow_shot/shortcuts/shortcutbinding.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QHash>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <limits>

namespace snow_shot::storage {
namespace {
const QStringList kDrawingToolbarItemIds = {
    QStringLiteral("shape"),     QStringLiteral("arrow"),         QStringLiteral("line"),
    QStringLiteral("free-draw"), QStringLiteral("highlighter"),   QStringLiteral("spotlight"),
    QStringLiteral("text"),      QStringLiteral("serial-number"), QStringLiteral("filter"),
    QStringLiteral("eraser"),    QStringLiteral("watermark"),
};
const QStringList kLastDrawingToolIds = QStringList{QStringLiteral("")} + kDrawingToolbarItemIds;

const QStringList kActionToolbarItemIds = {
    QStringLiteral("barcode-recognition"),  QStringLiteral("table-recognition"),
    QStringLiteral("convert-to-markdown"),  QStringLiteral("convert-to-html"),
    QStringLiteral("record-screen"),        QStringLiteral("pin-to-screen"),
    QStringLiteral("text-recognition"),     QStringLiteral("text-translation"),
    QStringLiteral("scrolling-screenshot"), QStringLiteral("quick-save"),
    QStringLiteral("save-as-file"),
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
        {QStringLiteral("shape")},     {QStringLiteral("line"), QStringLiteral("arrow")},
        {QStringLiteral("free-draw")}, {QStringLiteral("spotlight"), QStringLiteral("highlighter")},
        {QStringLiteral("text")},      {QStringLiteral("serial-number")},
        {QStringLiteral("filter")},    {QStringLiteral("eraser")},
        {QStringLiteral("watermark")},
    };
}

QVector<QStringList> defaultActionToolbarPositions() {
    return {
        {QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown"),
         QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")},
        {QStringLiteral("record-screen")},
        {QStringLiteral("pin-to-screen")},
        {QStringLiteral("text-recognition")},
        {QStringLiteral("text-translation")},
        {QStringLiteral("scrolling-screenshot")},
        {QStringLiteral("quick-save"), QStringLiteral("save-as-file")},
    };
}

const QStringList kPinnedActionToolbarItemIds = {
    QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition"),
    QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html"),
    QStringLiteral("text-recognition"),    QStringLiteral("text-translation")};

QVector<QStringList> defaultPinnedActionToolbarPositions() {
    return {
        {QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown"),
         QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")},
        {QStringLiteral("text-recognition")},
        {QStringLiteral("text-translation")},
    };
}

QJsonObject defaultToolbarLayout(const QVector<QStringList>& positions) {
    return {{QStringLiteral("positions"), jsonArray(positions)},
            {QStringLiteral("hidden"), QJsonArray()}};
}

QString defaultOutputDirectory(QStandardPaths::StandardLocation primary) {
    QString root = QStandardPaths::writableLocation(primary);
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    return root;
}

const QVector<ConfigurationSchemaEntry> kRawEntries = {
    {QStringLiteral("api_configuration/custom_models"), QJsonArray(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("storage/schema_version"), 3, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{3, 3, 1}},
    {QStringLiteral("interface/theme_mode"),
     QStringLiteral("system"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark")}},
    {QStringLiteral("interface/theme_primary_color"), QStringLiteral("#1677FFFF"),
     ConfigurationValueKind::String},
    {QStringLiteral("interface/language"), QStringLiteral("system"),
     ConfigurationValueKind::String},
    {QStringLiteral("system/application_priority"),
     QStringLiteral("above_normal"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("normal"), QStringLiteral("above_normal"), QStringLiteral("high"),
      QStringLiteral("real_time")}},
    {QStringLiteral("system/auto_start_at_boot"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("system/launch_as_administrator"), false, ConfigurationValueKind::Boolean},
#ifdef Q_OS_MACOS
    {QStringLiteral("updates/mode"),
     QStringLiteral("check"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("manual"), QStringLiteral("check")}},
#else
    {QStringLiteral("updates/mode"),
     QStringLiteral("download"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("manual"), QStringLiteral("check"), QStringLiteral("download")}},
#endif
    {QStringLiteral("network/proxy"),
     QStringLiteral("none"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("none"), QStringLiteral("system")}},
    {QStringLiteral("text_recognition/direct_ml_acceleration"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("text_recognition/resident_process"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("text_recognition/model_hot_start"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("text_recognition/model_type"),
     QStringLiteral("small"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("extra_small"), QStringLiteral("small"), QStringLiteral("medium"),
      QStringLiteral("small_v5"), QStringLiteral("medium_v5"), QStringLiteral("small_v4"),
      QStringLiteral("medium_v4")}},
    {QStringLiteral("screenshot_translation/source_language"),
     QStringLiteral("auto"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("auto"), QStringLiteral("ar"), QStringLiteral("de"), QStringLiteral("en"),
      QStringLiteral("es"), QStringLiteral("fr"), QStringLiteral("it"), QStringLiteral("ja"),
      QStringLiteral("pt"), QStringLiteral("ru"), QStringLiteral("tr"), QStringLiteral("zh-Hans"),
      QStringLiteral("zh-Hant")}},
    {QStringLiteral("screenshot_translation/target_language"),
     QString(),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("ar"), QStringLiteral("de"), QStringLiteral("en"), QStringLiteral("es"),
      QStringLiteral("fr"), QStringLiteral("it"), QStringLiteral("ja"), QStringLiteral("pt"),
      QStringLiteral("ru"), QStringLiteral("tr"), QStringLiteral("zh-Hans"),
      QStringLiteral("zh-Hant")}},
    {QStringLiteral("screenshot_translation/model"), QString(), ConfigurationValueKind::String},
    {QStringLiteral("screenshot_conversion/vision_model"), QString(),
     ConfigurationValueKind::String},
    {QStringLiteral("extended_features/standalone_translation_window"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("extended_features/jump_to_translation_page"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("extended_features/translation_page_enabled"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot_translation/original_image_translation"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot_translation/layout_processing"),
     QStringLiteral("smart_merge"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("smart_merge"), QStringLiteral("original")}},
    {QStringLiteral("interface/sidebar_collapsed"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("interface/main_window_geometry"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("interface/translation_window_size"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("text_recognition/save_recognition_result_as_image"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("text_recognition/fill_style"),
     QStringLiteral("background_fill"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("blur"), QStringLiteral("background_fill")}},
    {QStringLiteral("global_shortcuts/screenshot"),
     QJsonArray{QStringLiteral("F1")},
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
     QJsonArray{QStringLiteral("Ctrl+F1")},
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
    {QStringLiteral("global_shortcuts/open_screen_recording_folder"),
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
    {QStringLiteral("global_shortcuts/open_pin_to_screen_management"),
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
    {QStringLiteral("global_shortcuts/translate_selected_text"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/pin_clipboard_content"),
     QJsonArray{QStringLiteral("F3")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/pin_selected_files"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/restore_last_closed_windows"),
     QJsonArray{QStringLiteral("Ctrl+F3")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/toggle_global_hotkeys"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/toggle_disable_on_focused_fullscreen_window"),
     QJsonArray(),
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("global_mouse/screenshot_copy"),
#ifdef Q_OS_MACOS
     QJsonObject{{QStringLiteral("activation_key"), QJsonArray{QStringLiteral("command")}},
#else
     QJsonObject{{QStringLiteral("activation_key"), QJsonArray{QStringLiteral("windows")}},
#endif
                 {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}},
     ConfigurationValueKind::Structured},
    {QStringLiteral("global_mouse/screenshot_fixed"),
#ifdef Q_OS_MACOS
     QJsonObject{{QStringLiteral("activation_key"), QJsonArray{QStringLiteral("command")}},
#else
     QJsonObject{{QStringLiteral("activation_key"), QJsonArray{QStringLiteral("windows")}},
#endif
                 {QStringLiteral("mouse_button"), QStringLiteral("wheel_drag")}},
     ConfigurationValueKind::Structured},
    {QStringLiteral("global_mouse/screenshot_ocr"),
#ifdef Q_OS_MACOS
     QJsonObject{{QStringLiteral("activation_key"), QJsonArray{QStringLiteral("command")}},
#else
     QJsonObject{{QStringLiteral("activation_key"), QJsonArray{QStringLiteral("windows")}},
#endif
                 {QStringLiteral("mouse_button"), QStringLiteral("right_drag")}},
     ConfigurationValueKind::Structured},
    {QStringLiteral("global_mouse/screenshot_translation"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("global_mouse/screenshot_quick_save"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("global_mouse/screenshot_save"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("global_mouse/screen_recording"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("screen_recording/enable_microphone"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/enable_system_audio"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/clarity"),
     QStringLiteral("1080p"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("4k"), QStringLiteral("2k"), QStringLiteral("1080p"), QStringLiteral("720p"),
      QStringLiteral("480p")}},
    {QStringLiteral("screen_recording/frame_rate"), 30, ConfigurationValueKind::Integer},
    {QStringLiteral("screen_recording/animated_image_clarity"),
     QStringLiteral("720p"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("1080p"), QStringLiteral("720p"), QStringLiteral("480p")}},
    {QStringLiteral("screen_recording/animated_image_frame_rate"), 10,
     ConfigurationValueKind::Integer},
    {QStringLiteral("screen_recording/loop_animated_images"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/output_format"),
     QStringLiteral("mp4"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("mp4"), QStringLiteral("gif"), QStringLiteral("apng"),
      QStringLiteral("webp")}},
    {QStringLiteral("screen_recording/mouse_trail_color"), QStringLiteral("#00000000"),
     ConfigurationValueKind::String},
    {QStringLiteral("screen_recording/mouse_click_color"), QStringLiteral("#00000000"),
     ConfigurationValueKind::String},
    {QStringLiteral("screen_recording/mouse_trail_duration_ms"), 500,
     ConfigurationValueKind::Integer, ConfigurationIntegerRange{100, 2000, 100}},
    {QStringLiteral("screen_recording/keyboard_size"), 64, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{32, 128, 8}},
    {QStringLiteral("screen_recording/keyboard_background_color"), QStringLiteral("#000000CC"),
     ConfigurationValueKind::String},
    {QStringLiteral("screen_recording/keyboard_foreground_color"), QStringLiteral("#FFFFFFFF"),
     ConfigurationValueKind::String},
    {QStringLiteral("screen_recording/show_keyboard"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/show_cursor"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/mouse_highlight_enabled"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/record_mouse_clicks"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/mouse_highlight_color"), QStringLiteral("#FFFF0080"),
     ConfigurationValueKind::String},
    {QStringLiteral("screen_recording/encoder"),
     QStringLiteral("h264_hw"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("h264_hw"), QStringLiteral("h264"), QStringLiteral("h265")}},
    {QStringLiteral("screen_recording/encoding_preset"),
     QStringLiteral("veryfast"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("ultrafast"), QStringLiteral("veryfast"), QStringLiteral("medium"),
      QStringLiteral("veryslow"), QStringLiteral("placebo")}},
    {QStringLiteral("screen_recording/capture_toolbar_in_recording"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screen_recording/start_delay_seconds"), 0, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{0, 10, 1}},
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
      QStringLiteral("rectangle-filter"), QStringLiteral("pen-filter"), QStringLiteral("text"),
      QStringLiteral("serial-number"), QStringLiteral("eraser"), QStringLiteral("watermark")}},
    {QStringLiteral("drawing/remember_last_used_tool"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("drawing/shape_style"), QJsonObject(), ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/arrow_style"), QJsonObject(), ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/line_style"), QJsonObject(), ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/free_draw_style"), QJsonObject(), ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/rectangle_highlight_style"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/pen_highlight_style"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/rectangle_filter_style"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/pen_filter_style"), QJsonObject(), ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/text_style"), QJsonObject(), ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/serial_number_style"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/watermark_style"), QJsonObject(), ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/spotlight_style"), QJsonObject(), ConfigurationValueKind::Structured},
    {QStringLiteral("drawing/watermark_templates"), QJsonArray(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("drawing_shortcuts/select"),
     QJsonArray{QStringLiteral("V")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("drawing_shortcuts/shape"),
     QJsonArray{QStringLiteral("1")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("drawing_shortcuts/arrow"),
     QJsonArray{QStringLiteral("2")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("drawing_shortcuts/brush"),
     QJsonArray{QStringLiteral("3"), QStringLiteral("P")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("drawing_shortcuts/highlight"),
     QJsonArray{QStringLiteral("4"), QStringLiteral("H")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("drawing_shortcuts/text"),
     QJsonArray{QStringLiteral("5"), QStringLiteral("T")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("drawing_shortcuts/serial_number"),
     QJsonArray{QStringLiteral("6"), QStringLiteral("N")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("drawing_shortcuts/filter"),
     QJsonArray{QStringLiteral("7"), QStringLiteral("F")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("drawing_shortcuts/eraser"),
     QJsonArray{QStringLiteral("8"), QStringLiteral("E")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("drawing_shortcuts/watermark"),
     QJsonArray{QStringLiteral("9")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/move_tool"),
     QJsonArray{QStringLiteral("M"), QStringLiteral("Ctrl+E")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/move_cursor_up"),
     QJsonArray{QStringLiteral("W"), QStringLiteral("Up")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/move_cursor_down"),
     QJsonArray{QStringLiteral("S"), QStringLiteral("Down")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/move_cursor_left"),
     QJsonArray{QStringLiteral("A"), QStringLiteral("Left")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/move_cursor_right"),
     QJsonArray{QStringLiteral("D"), QStringLiteral("Right")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/move_entire_selection"),
     QJsonArray{QStringLiteral("Space")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/keep_selection_width_and_height_consistent"),
     QJsonArray{QStringLiteral("Shift")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/switch_selection_between_window_and_window_sub_element"),
     QJsonArray{QStringLiteral("Tab")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/previous_screenshot_history"),
     QJsonArray{QStringLiteral(",")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/next_screenshot_history"),
     QJsonArray{QStringLiteral(".")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/select_previously_selected_area"),
     QJsonArray{QStringLiteral("R")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/recapture"),
     QJsonArray{QStringLiteral("Alt+R")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/copy_color"),
     QJsonArray{QStringLiteral("C")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/table_recognition"),
     QJsonArray{QStringLiteral("Ctrl+X")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/qr_code_recognition"),
     QJsonArray{QStringLiteral("Ctrl+Q")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/video_recording"),
     QJsonArray{QStringLiteral("Ctrl+R")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/text_recognition"),
     QJsonArray{QStringLiteral("Ctrl+D")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/text_translation"),
     QJsonArray{QStringLiteral("Ctrl+T")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/scrolling_screenshot"),
     QJsonArray{QStringLiteral("L")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/quick_save"),
     QJsonArray{QStringLiteral("Ctrl+Shift+S")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/save_as_file"),
     QJsonArray{QStringLiteral("Ctrl+S")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/pin_to_screen"),
     QJsonArray{QStringLiteral("Ctrl+F")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/cancel_screenshot"),
     QJsonArray{QStringLiteral("Esc")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/copy_to_clipboard"),
     QJsonArray{QStringLiteral("Ctrl+C")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/undo"),
     QJsonArray{QStringLiteral("Ctrl+Z")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screenshot_shortcuts/redo"),
     QJsonArray{QStringLiteral("Ctrl+Y")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screen_recording_shortcuts/export"),
     QJsonArray{QStringLiteral("Ctrl+E")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screen_recording_shortcuts/toggle_recording"),
     QJsonArray{QStringLiteral("Ctrl+S")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screen_recording_shortcuts/copy_to_clipboard"),
     QJsonArray{QStringLiteral("Ctrl+C")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("screen_recording_shortcuts/end_recording"),
     QJsonArray{QStringLiteral("Esc")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/copy_to_clipboard"),
     QJsonArray{QStringLiteral("Ctrl+C")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/copy_original_content"),
     QJsonArray{QStringLiteral("Ctrl+Shift+C")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/save_as_file"),
     QJsonArray{QStringLiteral("Ctrl+S")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/show_text_recognition_results"),
     QJsonArray{QStringLiteral("Ctrl+D")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/drawing_mode"),
     QJsonArray{QStringLiteral("Space")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/resize_window"),
     QJsonArray{QStringLiteral("M")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/hide_to_top"),
     QJsonArray{QStringLiteral("H")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/toggle_click_through"),
     QJsonArray{QStringLiteral("Ctrl+M")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/thumbnail_mode"),
     QJsonArray{QStringLiteral("R")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/close_window"),
     QJsonArray{QStringLiteral("Esc")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/destroy_window"),
     QJsonArray{QStringLiteral("Shift+Esc")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/move_cursor_up"),
     QJsonArray{QStringLiteral("W"), QStringLiteral("Up")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/move_cursor_down"),
     QJsonArray{QStringLiteral("S"), QStringLiteral("Down")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/move_cursor_left"),
     QJsonArray{QStringLiteral("A"), QStringLiteral("Left")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
    {QStringLiteral("pin_to_screen_shortcuts/move_cursor_right"),
     QJsonArray{QStringLiteral("D"), QStringLiteral("Right")},
     ConfigurationValueKind::StringList,
     std::nullopt,
     {},
     2},
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
    {QStringLiteral("screenshot_toolbar/last_filter_tool"),
     QStringLiteral("pen-filter"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("rectangle-filter"), QStringLiteral("pen-filter"),
      QStringLiteral("auto-filter")}},
    {QStringLiteral("screenshot_toolbar/last_highlight_tool"),
     QStringLiteral("pen-highlight"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("rectangle-highlight"), QStringLiteral("pen-highlight")}},
    {QStringLiteral("screenshot_toolbar/last_drawing_tool"), QString(),
     ConfigurationValueKind::String, std::nullopt, kLastDrawingToolIds},
    {QStringLiteral("screenshot_toolbar/layout"),
     defaultToolbarLayout(defaultDrawingToolbarPositions()), ConfigurationValueKind::Structured},
    {QStringLiteral("pin_to_screen/action_tools_layout"),
     defaultToolbarLayout(defaultPinnedActionToolbarPositions()),
     ConfigurationValueKind::Structured},
    {QStringLiteral("screenshot_toolbar/action_tools_layout"),
     defaultToolbarLayout(defaultActionToolbarPositions()), ConfigurationValueKind::Structured},
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
    {QStringLiteral("screenshot_ui/color_picker_format"),
     QStringLiteral("hex"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("hex"), QStringLiteral("hex_without_hash"), QStringLiteral("rgb"),
      QStringLiteral("hsl")}},
    {QStringLiteral("screenshot_ui/selection_border_color"), QStringLiteral("#4096FFFF"),
     ConfigurationValueKind::String},
    {QStringLiteral("screenshot_ui/selection_mask_color"), QStringLiteral("#00000080"),
     ConfigurationValueKind::String},
    {QStringLiteral("screenshot_ui/shortcut_hint_opacity"), 100, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{0, 100, 1}},
    {QStringLiteral("screenshot_ui/area_type_hint_enabled"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot_ui/cursor_guide_line_color"), QStringLiteral("#00000000"),
     ConfigurationValueKind::String},
    {QStringLiteral("screenshot_ui/monitor_center_guide_line_color"), QStringLiteral("#00000000"),
     ConfigurationValueKind::String},
    {QStringLiteral("screenshot_ui/color_picker_center_guide_line_color"),
     QStringLiteral("#00000000"), ConfigurationValueKind::String},
    {QStringLiteral("pin_to_screen/border_color"), QStringLiteral("#DBDBDBFF"),
     ConfigurationValueKind::String},
    {QStringLiteral("pin_to_screen/border_active_color"), QStringLiteral("#69B1FFFF"),
     ConfigurationValueKind::String},
    {QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode"),
     QStringLiteral("mouse_position"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("mouse_position"), QStringLiteral("top_left"), QStringLiteral("top_right"),
      QStringLiteral("bottom_left"), QStringLiteral("bottom_right"), QStringLiteral("center")}},
    {QStringLiteral("pin_to_screen/double_click_action"),
     QStringLiteral("thumbnail_mode"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("none"), QStringLiteral("thumbnail_mode"), QStringLiteral("hide_to_top"),
      QStringLiteral("close")}},
    {QStringLiteral("pin_to_screen/middle_mouse_button_action"),
     QStringLiteral("reset_zoom"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("none"), QStringLiteral("reset_zoom"), QStringLiteral("thumbnail_mode"),
      QStringLiteral("hide_to_top"), QStringLiteral("close")}},
    {QStringLiteral("pin_to_screen/automatic_text_recognition"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("pin_to_screen/text_selection_on_recognition_results"),
     QStringLiteral("only_when_displayed"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("only_when_displayed"), QStringLiteral("always")}},
    {QStringLiteral("pin_to_screen/auto_resize_window"), true, ConfigurationValueKind::Boolean},
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
     {QStringLiteral("screenshot"), QStringLiteral("show_main_window"),
      QStringLiteral("screenshot_copy"), QStringLiteral("screenshot_fixed"),
      QStringLiteral("open_function_settings")}},
    {QStringLiteral("tray/middle_click_action"),
     QStringLiteral("screenshot_fixed"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("screenshot"), QStringLiteral("show_main_window"),
      QStringLiteral("screenshot_copy"), QStringLiteral("screenshot_fixed"),
      QStringLiteral("open_function_settings")}},
    {QStringLiteral("tray/menu_options"),
     QJsonArray{
         QStringLiteral("quick.screenshot"), QStringLiteral("quick.screenshot-delay"),
         QStringLiteral("quick.screenshot-fixed"), QStringLiteral("quick.screenshot-ocr"),
         QStringLiteral("quick.screenshot-copy"), QStringLiteral("quick.pin-clipboard-content"),
         QStringLiteral("quick.restore-last-closed-windows"), QStringLiteral("quick.screen-record"),
         QStringLiteral("quick.toggle-global-hotkeys"), QStringLiteral("tray.window-grouping"),
         QStringLiteral("tray.show-main-window"), QStringLiteral("tray.exit")},
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
      QStringLiteral("quick.pin-clipboard-content"),
      QStringLiteral("quick.pin-selected-files"),
      QStringLiteral("quick.restore-last-closed-windows"),
      QStringLiteral("quick.screen-record"),
      QStringLiteral("quick.screen-record-copy"),
      QStringLiteral("quick.open-screen-recording-folder"),
      QStringLiteral("quick.open-capture-history"),
      QStringLiteral("quick.translate-selected-text"),
      QStringLiteral("quick.toggle-global-hotkeys"),
      QStringLiteral("quick.toggle-disable-on-focused-fullscreen-window"),
      QStringLiteral("tray.window-grouping"),
      QStringLiteral("tray.show-main-window"),
      QStringLiteral("tray.restart-app"),
      QStringLiteral("tray.exit")},
     22},
    {QStringLiteral("screenshot_selection/previous_selection"), QJsonValue::Null,
     ConfigurationValueKind::Structured},
    {QStringLiteral("screenshot_selection/region_type"),
     QStringLiteral("rectangle"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("rectangle"), QStringLiteral("polyline"), QStringLiteral("curve"),
      QStringLiteral("freehand")}},
    {QStringLiteral("screenshot_selection/smart_selection"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot_selection/selection_target"),
     QStringLiteral("window_sub_element"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("window"), QStringLiteral("window_sub_element")}},
    {QStringLiteral("screenshot_selection/corner_radius"), 0, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{0, 256, 1}},
    {QStringLiteral("screenshot_selection/shadow_width"), 0, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{0, 64, 1}},
    {QStringLiteral("screenshot_selection/lock_aspect_ratio"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/capture_cursor"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/capture_ui_in_scrolling_screenshot"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/shutter_sound_notification"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/confirm_before_exiting_via_shortcut"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/restore_original_screen_colors"), true,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/delay_seconds"), 3, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{1, 10, 1}},
    {QStringLiteral("screenshot/auto_execute_after_text_recognition"),
     QStringLiteral("no_action"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("no_action"), QStringLiteral("copy_text"),
      QStringLiteral("copy_text_and_end_screenshot"), QStringLiteral("quick_copy_text"),
      QStringLiteral("quick_copy_text_and_end_screenshot"), QStringLiteral("enable_edit_mode")}},
    {QStringLiteral("screenshot/double_click_action"),
     QStringLiteral("copy"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("copy"), QStringLiteral("save"), QStringLiteral("quick_save"),
      QStringLiteral("pin"), QStringLiteral("none")}},
    {QStringLiteral("screenshot/middle_mouse_button_action"),
     QStringLiteral("pin"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("copy"), QStringLiteral("save"), QStringLiteral("quick_save"),
      QStringLiteral("pin"), QStringLiteral("none")}},
    {QStringLiteral("screenshot/selection_resize_mode"),
     QStringLiteral("follow_mouse_movement"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("follow_mouse_movement"), QStringLiteral("follow_mouse_position")}},
    {QStringLiteral("screenshot/auto_save_after_copy"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/copy_image_file_to_clipboard"), false,
     ConfigurationValueKind::Boolean},
    {QStringLiteral("screenshot/image_save_directory"),
     defaultOutputDirectory(QStandardPaths::PicturesLocation), ConfigurationValueKind::String},
    {QStringLiteral("screenshot/last_manual_save_directory"), QString(),
     ConfigurationValueKind::String},
    {QStringLiteral("screenshot/last_manual_save_format"),
     QStringLiteral("png"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("png"), QStringLiteral("jpeg"), QStringLiteral("bmp"), QStringLiteral("webp"),
      QStringLiteral("jxl"), QStringLiteral("avif"), QStringLiteral("pdf")}},
    {QStringLiteral("screenshot/save_as_file_dialog"),
     QStringLiteral("system"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("system"), QStringLiteral("snow_shot")}},
    {QStringLiteral("screenshot/save_path_shortcuts"), QJsonArray(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("screenshot/manual_save_format_options"), QJsonObject(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("screenshot/image_format"),
     QStringLiteral("png"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("png"), QStringLiteral("jpeg"), QStringLiteral("bmp"), QStringLiteral("webp"),
      QStringLiteral("jxl"), QStringLiteral("avif"), QStringLiteral("pdf")}},
    {QStringLiteral("screenshot/compression_level"),
     QStringLiteral("medium"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}},
    {QStringLiteral("screenshot/image_quality"), 100, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{0, 100, 1}},
    {QStringLiteral("screenshot/pdf_page_size"),
     QStringLiteral("a4_portrait"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("image_size"), QStringLiteral("a4_portrait"), QStringLiteral("a4_landscape")}},
    {QStringLiteral("screenshot/api_mode"),
     QStringLiteral("auto"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("auto"), QStringLiteral("dxgi"), QStringLiteral("wgc"),
      QStringLiteral("gdi")}},
    {QStringLiteral("screenshot/window_element_api"),
     QStringLiteral("uia"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("msaa"), QStringLiteral("uia")}},
    {QStringLiteral("screenshot/manual_save_filename_format"),
     QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"), ConfigurationValueKind::String},
    {QStringLiteral("screenshot/auto_save_filename_format"),
     QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"), ConfigurationValueKind::String},
    {QStringLiteral("screenshot_selection/selection_rect_presets"), QJsonArray(),
     ConfigurationValueKind::Structured},
    {QStringLiteral("capture_history/enabled"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("capture_history/keep_permanently"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("capture_history/compression_level"),
     QStringLiteral("medium"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}},
    {QStringLiteral("capture_history/retention_days"), 7, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{CaptureHistoryPolicy::MinimumRetentionDays,
                               CaptureHistoryPolicy::MaximumRetentionDays, 1}},
    {QStringLiteral("capture_history/max_entries"), 100, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{CaptureHistoryPolicy::MinimumEntries,
                               CaptureHistoryPolicy::MaximumEntries, 1}},
    {QStringLiteral("capture_history/max_disk_mib"), 1024, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{CaptureHistoryPolicy::MinimumDiskMiB,
                               CaptureHistoryPolicy::MaximumDiskMiB, 1}},
    {QStringLiteral("pinned_history/enabled"), true, ConfigurationValueKind::Boolean},
    {QStringLiteral("pinned_history/keep_permanently"), false, ConfigurationValueKind::Boolean},
    {QStringLiteral("pinned_history/compression_level"),
     QStringLiteral("medium"),
     ConfigurationValueKind::String,
     std::nullopt,
     {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}},
    {QStringLiteral("pinned_history/retention_days"), 7, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{CaptureHistoryPolicy::MinimumRetentionDays,
                               CaptureHistoryPolicy::MaximumRetentionDays, 1}},
    {QStringLiteral("pinned_history/max_entries"), 100, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{CaptureHistoryPolicy::MinimumEntries,
                               CaptureHistoryPolicy::MaximumEntries, 1}},
    {QStringLiteral("pinned_history/max_disk_mib"), 1024, ConfigurationValueKind::Integer,
     ConfigurationIntegerRange{CaptureHistoryPolicy::MinimumDiskMiB,
                               CaptureHistoryPolicy::MaximumDiskMiB, 1}},
};

bool shortcutConfigurationKey(const QString& key) {
    return key.startsWith(QStringLiteral("global_shortcuts/")) ||
           key.startsWith(QStringLiteral("drawing_shortcuts/")) ||
           key.startsWith(QStringLiteral("screenshot_shortcuts/")) ||
           key.startsWith(QStringLiteral("screen_recording_shortcuts/")) ||
           key.startsWith(QStringLiteral("pin_to_screen_shortcuts/"));
}

QJsonArray shortcutDefaults(const QJsonValue& value) {
    return snow_shot::shortcuts::shortcutBindingsToJson(
        snow_shot::shortcuts::shortcutBindingsFromJson(value, true));
}

QJsonArray macGlobalShortcutDefault(const QString& portableText, quint32 virtualKey) {
    snow_shot::shortcuts::ShortcutBinding binding{portableText};
    binding.physicalKeys.insert(snow_shot::shortcuts::ShortcutPlatform::MacOS, virtualKey);
    return snow_shot::shortcuts::shortcutBindingsToJson({binding});
}

QVector<ConfigurationSchemaEntry> buildEntries() {
    QVector<ConfigurationSchemaEntry> result = kRawEntries;
    for (ConfigurationSchemaEntry& entry : result) {
        if (!shortcutConfigurationKey(entry.key) ||
            entry.valueKind != ConfigurationValueKind::StringList) {
            continue;
        }
        entry.valueKind = ConfigurationValueKind::ShortcutList;
        entry.defaultValue = shortcutDefaults(entry.defaultValue);
    }
#ifdef Q_OS_MACOS
    const auto replaceDefault = [&result](const QString& key, const QJsonArray& value) {
        const auto found = std::find_if(result.begin(), result.end(),
                                        [&key](const auto& entry) { return entry.key == key; });
        if (found != result.end()) {
            found->defaultValue = value;
        }
    };
    // Portable Meta maps to the physical Control key on Apple platforms.
    replaceDefault(QStringLiteral("global_shortcuts/screenshot"),
                   macGlobalShortcutDefault(QStringLiteral("Meta+1"), 18));
    replaceDefault(QStringLiteral("global_shortcuts/screenshot_copy"),
                   macGlobalShortcutDefault(QStringLiteral("Meta+2"), 19));
    replaceDefault(QStringLiteral("global_shortcuts/restore_last_closed_windows"),
                   macGlobalShortcutDefault(QStringLiteral("Meta+Shift+3"), 20));
    replaceDefault(QStringLiteral("global_shortcuts/pin_clipboard_content"),
                   macGlobalShortcutDefault(QStringLiteral("Meta+3"), 20));
#endif
    return result;
}

const QVector<ConfigurationSchemaEntry> kEntries = buildEntries();

const QHash<QString, int>& entryIndex() {
    static const QHash<QString, int> index = [] {
        QHash<QString, int> result;
        result.reserve(kEntries.size());
        for (int i = 0; i < kEntries.size(); ++i) {
            result.insert(kEntries.at(i).key, i);
        }
        return result;
    }();
    return index;
}

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

ConfigurationNormalization normalizeShortcuts(const QJsonValue& value, int maximumItems,
                                              bool allowModifierOnlyShift) {
    bool changed = false;
    bool valid = false;
    const auto bindings = snow_shot::shortcuts::shortcutBindingsFromJson(
        value, allowModifierOnlyShift, maximumItems, &valid, &changed);
    return {snow_shot::shortcuts::shortcutBindingsToJson(bindings), valid, changed};
}

ConfigurationNormalization normalizeAllowedStringList(const ConfigurationSchemaEntry& schemaEntry,
                                                      const QJsonValue& value) {
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

ConfigurationNormalization normalizeTrayMenuOptions(const ConfigurationSchemaEntry& schemaEntry,
                                                    const QJsonValue& value) {
    if (!value.isArray()) {
        return {};
    }
    QJsonArray migrated;
    bool renamed = false;
    for (const QJsonValue& item : value.toArray()) {
        // Earlier builds exposed the hotkey switch through the dedicated tray
        // command "tray.disable-shortcut-functions"; rename stored copies to the
        // quick action so upgraders keep the entry instead of silently losing it.
        if (item.isString() &&
            item.toString().trimmed() == QStringLiteral("tray.disable-shortcut-functions")) {
            migrated.push_back(QStringLiteral("quick.toggle-global-hotkeys"));
            renamed = true;
            continue;
        }
        migrated.push_back(item);
    }
    ConfigurationNormalization normalized = normalizeAllowedStringList(schemaEntry, migrated);
    // The rename itself is a change even when the mapped list is otherwise
    // canonical, so the store rewrites the persisted document with the new id.
    normalized.changed = normalized.changed || renamed;
    return normalized;
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

ConfigurationNormalization normalizeWatermarkTemplates(const QJsonValue& value) {
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
        const QJsonObject object = item.toObject();
        const QJsonValue nameValue = object.value(QStringLiteral("name"));
        const QJsonValue templateValue = object.value(QStringLiteral("value"));
        if (!nameValue.isString() || !templateValue.isString()) {
            changed = true;
            continue;
        }
        const QString name = nameValue.toString().trimmed();
        const QString templateText = templateValue.toString();
        if (name.isEmpty() || templateText.trimmed().isEmpty()) {
            changed = true;
            continue;
        }
        const QJsonObject normalized{{QStringLiteral("name"), name},
                                     {QStringLiteral("value"), templateText}};
        result.push_back(normalized);
        changed = changed || normalized != object;
    }
    return {result, true, changed};
}

bool isRgbaColorKey(const QString& key) {
    return key == QStringLiteral("interface/theme_primary_color") ||
           key == QStringLiteral("screenshot_ui/selection_border_color") ||
           key == QStringLiteral("screenshot_ui/selection_mask_color") ||
           key == QStringLiteral("screenshot_ui/cursor_guide_line_color") ||
           key == QStringLiteral("screenshot_ui/monitor_center_guide_line_color") ||
           key == QStringLiteral("screenshot_ui/color_picker_center_guide_line_color") ||
           key == QStringLiteral("pin_to_screen/border_color") ||
           key == QStringLiteral("pin_to_screen/border_active_color") ||
           key == QStringLiteral("screen_recording/mouse_trail_color") ||
           key == QStringLiteral("screen_recording/mouse_click_color") ||
           key == QStringLiteral("screen_recording/mouse_highlight_color") ||
           key == QStringLiteral("screen_recording/keyboard_background_color") ||
           key == QStringLiteral("screen_recording/keyboard_foreground_color");
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

ConfigurationNormalization normalizeTranslationLanguage(const ConfigurationSchemaEntry& schemaEntry,
                                                        const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    const QString original = value.toString();
    const QString trimmed = original.trimmed();
    const auto canonical =
        std::find_if(schemaEntry.allowedStringValues.cbegin(),
                     schemaEntry.allowedStringValues.cend(), [&trimmed](const QString& allowed) {
                         return allowed.compare(trimmed, Qt::CaseInsensitive) == 0;
                     });
    if (canonical == schemaEntry.allowedStringValues.cend()) {
        return {};
    }
    return {*canonical, true, *canonical != original};
}

ConfigurationNormalization normalizeToolbarLayout(const QJsonValue& value,
                                                  const QStringList& itemIds,
                                                  const QVector<QStringList>& defaultPositions,
                                                  bool migrateScreenshotLayout = false) {
    if (!value.isObject()) {
        return {};
    }
    const QJsonObject object = value.toObject();
    const QSet<QString> known(itemIds.cbegin(), itemIds.cend());
    QVector<QStringList> positions;
    QSet<QString> positioned;
    QStringList hidden;
    QSet<QString> hiddenSet;
    const auto appendPosition = [&positions, &positioned, &hiddenSet,
                                 &known](const QStringList& ids) {
        QStringList position;
        for (const QString& id : ids) {
            if (known.contains(id) && !positioned.contains(id) && !hiddenSet.contains(id)) {
                position.push_back(id);
                positioned.insert(id);
            }
        }
        if (!position.isEmpty()) {
            positions.push_back(position);
        }
    };
    const auto appendHidden = [&hidden, &hiddenSet, &positioned, &known](const QStringList& ids) {
        for (const QString& id : ids) {
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
    } else {
        return {};
    }

    if (known.contains(QStringLiteral("quick-save")) &&
        !positioned.contains(QStringLiteral("quick-save")) &&
        !hiddenSet.contains(QStringLiteral("quick-save"))) {
        for (QStringList& position : positions) {
            const qsizetype saveIndex = position.indexOf(QStringLiteral("save-as-file"));
            if (saveIndex >= 0) {
                position.insert(saveIndex, QStringLiteral("quick-save"));
                positioned.insert(QStringLiteral("quick-save"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("quick-save")) &&
            hiddenSet.contains(QStringLiteral("save-as-file"))) {
            hidden.push_back(QStringLiteral("quick-save"));
            hiddenSet.insert(QStringLiteral("quick-save"));
        }
    }
    if (migrateScreenshotLayout && !positions.isEmpty() &&
        known.contains(QStringLiteral("convert-to-markdown"))) {
        // Upgrade earlier defaults without changing custom placements.
        auto previousDefault = defaultPositions;
        previousDefault[0] = {QStringLiteral("barcode-recognition"),
                              QStringLiteral("table-recognition")};
        previousDefault.insert(1, QStringList{QStringLiteral("convert-to-markdown")});
        previousDefault.insert(2, QStringList{QStringLiteral("convert-to-html")});
        auto previousGroupedDefault = defaultPositions;
        previousGroupedDefault[0] = {
            QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
            QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")};
        if (hidden.isEmpty() &&
            (positions == previousDefault || positions == previousGroupedDefault)) {
            positions = defaultPositions;
        }
        qsizetype recognitionPosition = -1;
        for (const QString& anchor :
             {QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")}) {
            for (qsizetype index = 0; index < positions.size(); ++index) {
                if (positions.at(index).contains(anchor)) {
                    recognitionPosition = index;
                    break;
                }
            }
            if (recognitionPosition >= 0) {
                break;
            }
        }
        for (const QString& id :
             {QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")}) {
            if (recognitionPosition >= 0 && !positioned.contains(id) && !hiddenSet.contains(id)) {
                positions[recognitionPosition].push_back(id);
                positioned.insert(id);
            }
        }
    }
    for (const QStringList& defaultPosition : defaultPositions) {
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

bool isGlobalMouseKey(const QString& key) {
    return key.startsWith(QStringLiteral("global_mouse/"));
}

ConfigurationNormalization normalizeGlobalMouseCombination(const QJsonValue& value) {
    if (!value.isObject()) {
        return {};
    }
    const QJsonObject object = value.toObject();
    if (object.isEmpty()) {
        return {QJsonObject(), true, false};
    }
    const QJsonValue activationValue = object.value(QStringLiteral("activation_key"));
    if (object.size() != 2 || (!activationValue.isString() && !activationValue.isArray()) ||
        !object.value(QStringLiteral("mouse_button")).isString()) {
        return {};
    }

    QStringList keys;
    if (activationValue.isString()) {
        keys.push_back(activationValue.toString().trimmed());
    } else {
        for (const QJsonValue& item : activationValue.toArray()) {
            if (!item.isString()) {
                return {};
            }
            keys.push_back(item.toString().trimmed());
        }
    }
    const QString mouseButton = object.value(QStringLiteral("mouse_button")).toString().trimmed();
    static const QStringList activationKeys = presentation::globalMouseActivationKeys();
    static const QSet<QString> mouseButtons{
        QStringLiteral("left_drag"), QStringLiteral("right_drag"), QStringLiteral("wheel_drag"),
        QStringLiteral("side_button_1_drag"), QStringLiteral("side_button_2_drag")};
    if (keys.isEmpty() || !mouseButtons.contains(mouseButton)) {
        return {};
    }
    for (const QString& key : keys) {
        if (!activationKeys.contains(key)) {
            return {};
        }
    }
    keys.removeDuplicates();
    keys.sort();

    const QJsonObject normalized{{QStringLiteral("activation_key"),
                                  keys.size() == 1 ? QJsonValue(keys.front())
                                                   : QJsonValue(QJsonArray::fromStringList(keys))},
                                 {QStringLiteral("mouse_button"), mouseButton}};
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
    const auto found = entryIndex().constFind(key);
    return found == entryIndex().cend() ? nullptr : &kEntries.at(found.value());
}

bool ConfigurationSchema::contains(const QString& key) {
    return entry(key) != nullptr;
}

QJsonValue ConfigurationSchema::defaultValue(const QString& key) {
    const ConfigurationSchemaEntry* found = entry(key);
    return found == nullptr ? QJsonValue() : found->defaultValue;
}

namespace {
ConfigurationNormalization normalizeManualSaveFormatOptions(const QJsonValue& value) {
    if (!value.isObject())
        return {};
    const QJsonObject source = value.toObject();
    QJsonObject result;
    const QSet<QString> qualityFormats{
        QStringLiteral("jpeg"), QStringLiteral("webp"), QStringLiteral("jxl"),
        QStringLiteral("avif"), QStringLiteral("pdf"),
    };
    const QSet<QString> compressionFormats{
        QStringLiteral("png"),
        QStringLiteral("webp"),
        QStringLiteral("jxl"),
        QStringLiteral("avif"),
    };
    const QSet<QString> compressionValues{
        QStringLiteral("low"),
        QStringLiteral("medium"),
        QStringLiteral("high"),
    };
    for (auto format = source.constBegin(); format != source.constEnd(); ++format) {
        if ((!qualityFormats.contains(format.key()) &&
             !compressionFormats.contains(format.key())) ||
            !format.value().isObject()) {
            continue;
        }
        const QJsonObject options = format.value().toObject();
        QJsonObject normalized;
        if (qualityFormats.contains(format.key())) {
            const QJsonValue quality = options.value(QStringLiteral("quality"));
            if (isInteger(quality))
                normalized.insert(QStringLiteral("quality"), qBound(0, quality.toInt(), 100));
        }
        if (compressionFormats.contains(format.key())) {
            const QString compression =
                options.value(QStringLiteral("compression_level")).toString().trimmed();
            if (compressionValues.contains(compression))
                normalized.insert(QStringLiteral("compression_level"), compression);
        }
        if (!normalized.isEmpty())
            result.insert(format.key(), normalized);
    }
    return {result, true, result != source};
}
} // namespace

ConfigurationNormalization ConfigurationSchema::normalize(const QString& key,
                                                          const QJsonValue& value) {
    const ConfigurationSchemaEntry* schemaEntry = entry(key);
    if (schemaEntry == nullptr) {
        return {};
    }
#ifdef Q_OS_MACOS
    if (key == u"updates/mode" && value.toString() == u"download") {
        return {QStringLiteral("check"), true, true};
    }
#endif
    if (key == QStringLiteral("api_configuration/custom_models")) {
        bool valid = false;
        const auto models = customAiModelsFromJson(value, &valid);
        const auto normalized = customAiModelsToJson(models);
        return {normalized, valid, normalized != value};
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
    if (key == QStringLiteral("drawing/watermark_templates")) {
        return normalizeWatermarkTemplates(value);
    }
    if (key == QStringLiteral("screenshot/save_path_shortcuts")) {
        if (!value.isArray()) {
            return {};
        }
        QJsonArray result;
        QSet<QString> names;
        for (const auto& item : value.toArray()) {
            const auto object = item.toObject();
            const QString name = object.value(QStringLiteral("name")).toString().trimmed();
            const QString path = object.value(QStringLiteral("path")).toString().trimmed();
            if (name.isEmpty() || path.isEmpty() || names.contains(name.toCaseFolded())) {
                continue;
            }
            names.insert(name.toCaseFolded());
            result.append(
                QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("path"), path}});
        }
        return {result, true, result != value.toArray()};
    }
    if (key == QStringLiteral("screenshot/manual_save_format_options"))
        return normalizeManualSaveFormatOptions(value);
    if (key == QStringLiteral("screenshot_toolbar/layout")) {
        return normalizeToolbarLayout(value, kDrawingToolbarItemIds,
                                      defaultDrawingToolbarPositions());
    }
    if (key == QStringLiteral("pin_to_screen/action_tools_layout")) {
        return normalizeToolbarLayout(value, kPinnedActionToolbarItemIds,
                                      defaultPinnedActionToolbarPositions());
    }
    if (key == QStringLiteral("screenshot_toolbar/action_tools_layout")) {
        return normalizeToolbarLayout(value, kActionToolbarItemIds, defaultActionToolbarPositions(),
                                      true);
    }
    if (isGlobalMouseKey(key)) {
        return normalizeGlobalMouseCombination(value);
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
        return normalizeTrayMenuOptions(*schemaEntry, value);
    }
    if (key == QStringLiteral("screen_recording/frame_rate")) {
        return normalizeAllowedInteger(value, {5, 10, 15, 24, 30, 60, 120, 83});
    }
    if (key == QStringLiteral("screen_recording/animated_image_frame_rate")) {
        return normalizeAllowedInteger(value, {5, 10, 15, 24});
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
        return normalizeAllowedStringList(*schemaEntry, value);
    case ConfigurationValueKind::ShortcutList:
        return normalizeShortcuts(value, schemaEntry->maximumListItems,
                                  key.startsWith(QStringLiteral("screenshot_shortcuts/")));
    case ConfigurationValueKind::Structured:
        return exactType(value, QJsonValue::Object);
    }
    return {};
}

bool ConfigurationSchema::parseIntegerVersion(const QJsonValue& value, int* version) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble() || value.toDouble() < 1.0 ||
        value.toDouble() > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (version != nullptr) {
        *version = value.toInt();
    }
    return true;
}

int ConfigurationSchema::currentVersion() {
    return defaultValue(QStringLiteral("storage/schema_version")).toInt();
}

QJsonObject ConfigurationSchema::completeDefaultDocument() {
    QJsonObject root;
    for (const ConfigurationSchemaEntry& entry : kEntries) {
        insertPath(&root, entry.key, entry.defaultValue);
    }
    return root;
}
} // namespace snow_shot::storage
