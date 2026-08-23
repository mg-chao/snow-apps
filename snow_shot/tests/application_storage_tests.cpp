#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/persistedselectioncodec.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "failed to open test file");
    require(file.write(bytes) == bytes.size(), "failed to write test file");
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "failed to read test file");
    return file.readAll();
}

QJsonObject readObject(const QString& path) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(readBytes(path), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            "stored configuration is not valid JSON");
    return document.object();
}

storage::ApplicationStorage& initialize(const QString& executableDirectory,
                                        const QString& appDataDirectory,
                                        int debounceMilliseconds = 60000) {
    auto& applicationStorage = storage::ApplicationStorage::instance();
    static_cast<void>(applicationStorage.initialize(
        {executableDirectory, appDataDirectory, debounceMilliseconds}));
    return applicationStorage;
}

void markerResolutionAndStatus() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create marker test directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    const QString fallback = QDir(temporary.path()).filePath(QStringLiteral("fallback"));
    require(QDir().mkpath(executable), "failed to create executable directory");

    auto& missing = initialize(executable, fallback);
    require(missing.configurationDirectory() == QDir::cleanPath(fallback) &&
                missing.status().effectiveMode == storage::StorageMode::ApplicationData,
            "missing marker did not select application data storage");

    const QString marker = QDir(executable).filePath(QStringLiteral("__data_directory"));
    writeBytes(marker, QByteArrayLiteral("portable"));
    auto& portable = initialize(executable, fallback);
    require(portable.configurationDirectory() ==
                    QDir(executable).filePath(QStringLiteral("portable")) &&
                portable.status().effectiveMode == storage::StorageMode::Portable,
            "relative marker did not select portable storage");

    const QString blocking = QDir(temporary.path()).filePath(QStringLiteral("file-target"));
    writeBytes(blocking, QByteArrayLiteral("file"));
    writeBytes(marker, blocking.toUtf8());
    auto& fallbackStorage = initialize(executable, fallback);
    require(fallbackStorage.configurationDirectory() == QDir::cleanPath(fallback) &&
                !fallbackStorage.status().fallbackReason.isEmpty(),
            "unwritable portable target did not report fallback");
}

void defaultsAndTypedRoundTrip() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create defaults directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    storage::ConfigurationStore store(config, true, true, 60000);
    require(store.isDirty() && store.flushNow().success,
            "default configuration was not materialized");
    const QJsonObject root = readObject(config);
    require(root == storage::ConfigurationSchema::completeDefaultDocument(),
            "materialized defaults must exactly match the complete schema document");
    const QJsonObject history = root.value(QStringLiteral("capture_history")).toObject();
    const QJsonObject screenshotUi = root.value(QStringLiteral("screenshot_ui")).toObject();
    const QJsonObject toolbarLayout = root.value(QStringLiteral("screenshot_toolbar"))
                                          .toObject()
                                          .value(QStringLiteral("layout"))
                                          .toObject();
    const QJsonArray toolbarPositions = toolbarLayout.value(QStringLiteral("positions")).toArray();
    const QJsonObject tray = root.value(QStringLiteral("tray")).toObject();
    require(
        root.value(QStringLiteral("storage"))
                    .toObject()
                    .value(QStringLiteral("schema_version"))
                    .toInt() == 1 &&
            root.value(QStringLiteral("screenshot_selection"))
                .toObject()
                .value(QStringLiteral("smart_selection"))
                .toBool() &&
            history.value(QStringLiteral("enabled")).toBool() &&
            history.value(QStringLiteral("retention_days")).toInt() == 7 &&
            history.value(QStringLiteral("max_entries")).toInt() == 100 &&
            history.value(QStringLiteral("max_disk_mib")).toInt() == 1024 &&
            screenshotUi.value(QStringLiteral("toolbar_size")).toString() ==
                QStringLiteral("normal") &&
            screenshotUi.value(QStringLiteral("selection_transition_animation")).toBool() &&
            screenshotUi.value(QStringLiteral("selection_mask_color")).toString() ==
                QStringLiteral("#00000080") &&
            screenshotUi.value(QStringLiteral("shortcut_hint_opacity")).toInt() == 100 &&
            toolbarLayout.size() == 2 &&
            toolbarLayout.value(QStringLiteral("hidden")).toArray().isEmpty() &&
            toolbarPositions ==
                QJsonArray{
                    QJsonArray{QStringLiteral("shape")},
                    QJsonArray{QStringLiteral("line"), QStringLiteral("arrow")},
                    QJsonArray{QStringLiteral("free-draw")},
                    QJsonArray{QStringLiteral("spotlight"),
                               QStringLiteral("highlighter")},
                    QJsonArray{QStringLiteral("text")},
                    QJsonArray{QStringLiteral("serial-number")},
                    QJsonArray{QStringLiteral("filter")},
                    QJsonArray{QStringLiteral("eraser")},
                    QJsonArray{QStringLiteral("watermark")},
                } &&
            tray.value(QStringLiteral("enabled")).toBool() &&
            tray.value(QStringLiteral("icon")).toString() == QStringLiteral("default") &&
            tray.value(QStringLiteral("custom_icon")).toString().isEmpty() &&
            !history.contains(QStringLiteral("records")),
        "schema-v1 defaults are incomplete");
    require(readBytes(config).endsWith('\n'), "configuration has no final newline");

    require(
        store.setValues({
            {QStringLiteral("interface/theme_mode"), QStringLiteral("DARK")},
            {QStringLiteral("interface/language"), QStringLiteral("zh-CN")},
            {QStringLiteral("capture_history/retention_days"), 30},
            {QStringLiteral("capture_history/max_entries"), 250},
            {QStringLiteral("capture_history/max_disk_mib"), 2048},
            {QStringLiteral("screenshot_selection/smart_selection"), false},
            {QStringLiteral("screenshot_ui/selection_mask_color"), QStringLiteral(" #12ab34cd ")},
            {QStringLiteral("screenshot_ui/shortcut_hint_opacity"), 42},
            {QStringLiteral("tray/icon"), QStringLiteral("snow-dark")},
        }) &&
            store.flushNow().success,
        "typed configuration mutation failed");
    storage::ConfigurationStore reloaded(config, true, true, 60000);
    require(
        reloaded.value(QStringLiteral("interface/theme_mode")).toString() ==
                QStringLiteral("dark") &&
            reloaded.value(QStringLiteral("interface/language")).toString() ==
                QStringLiteral("zh_CN") &&
            reloaded.value(QStringLiteral("capture_history/retention_days")).toInt() == 30 &&
            !reloaded.value(QStringLiteral("screenshot_selection/smart_selection")).toBool() &&
            reloaded.value(QStringLiteral("screenshot_ui/selection_mask_color")).toString() ==
                QStringLiteral("#12AB34CD") &&
            reloaded.value(QStringLiteral("screenshot_ui/shortcut_hint_opacity")).toInt() == 42 &&
            reloaded.value(QStringLiteral("tray/icon")).toString() == QStringLiteral("snow-dark"),
        "typed values did not normalize and round-trip");
}

void newSettingsSchemaDefaultsAndValidationAreComplete() {
    const auto defaultValue = [](const char* key) {
        return storage::ConfigurationSchema::defaultValue(QString::fromLatin1(key));
    };
    require(defaultValue("system/auto_start_at_boot").toBool() &&
                defaultValue("network/proxy").toString() == QStringLiteral("none") &&
                !defaultValue("global_shortcuts/disable_on_focused_fullscreen_window").toBool() &&
                defaultValue("screenshot/auto_execute_after_text_recognition").toString() ==
                    QStringLiteral("no_action") &&
                defaultValue("screenshot/double_click_action").toString() ==
                    QStringLiteral("copy") &&
                defaultValue("screenshot/middle_mouse_button_action").toString() ==
                    QStringLiteral("pin") &&
                !defaultValue("screenshot/auto_save_after_copy").toBool() &&
                !defaultValue("screenshot/copy_image_file_to_clipboard").toBool() &&
                QDir::fromNativeSeparators(
                    defaultValue("screenshot/image_save_directory").toString())
                    .endsWith(QStringLiteral("/SnowShot")) &&
                defaultValue("screenshot/last_manual_save_directory").toString().isEmpty() &&
                defaultValue("screenshot/image_format").toString() == QStringLiteral("png") &&
                defaultValue("screenshot/manual_save_filename_format").toString() ==
                    QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}") &&
                defaultValue("screenshot/auto_save_filename_format").toString() ==
                    QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}") &&
                defaultValue("drawing/quick_selection_disabled_tools").toArray() ==
                    QJsonArray{QStringLiteral("free-draw"), QStringLiteral("pen-filter")} &&
                defaultValue("pin_to_screen/mouse_wheel_zoom_mode").toString() ==
                    QStringLiteral("mouse_position") &&
                defaultValue("pin_to_screen/automatic_text_recognition").toBool() &&
                defaultValue("pin_to_screen/auto_resize_window").toBool() &&
                defaultValue("screen_recording/clarity").toString() ==
                    QStringLiteral("1080p") &&
                defaultValue("screen_recording/frame_rate").toInt() == 30 &&
                defaultValue("screen_recording/animated_image_clarity").toString() ==
                    QStringLiteral("1080p") &&
                defaultValue("screen_recording/animated_image_frame_rate").toInt() == 10 &&
                defaultValue("screen_recording/animated_image_format").toString() ==
                    QStringLiteral("gif") &&
                defaultValue("screen_recording/encoder").toString() ==
                    QStringLiteral("h264") &&
                defaultValue("screen_recording/encoding_preset").toString() ==
                    QStringLiteral("veryfast") &&
                defaultValue("screen_recording/hide_toolbar_in_recording").toBool() &&
                QDir::fromNativeSeparators(
                    defaultValue("screen_recording/video_save_directory").toString())
                    .endsWith(QStringLiteral("/SnowShot")) &&
                defaultValue("screen_recording/video_filename_format").toString() ==
                    QStringLiteral("SnowShot_Video_{YYYY-MM-DD_HH-mm-ss}") &&
                defaultValue("tray/left_click_action").toString() ==
                    QStringLiteral("screenshot") &&
                defaultValue("tray/menu_options").toArray() ==
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
            "new settings defaults do not match the requested contract");

    const QMap<QString, QJsonArray> drawingShortcutDefaults{
        {QStringLiteral("select"), QJsonArray{QStringLiteral("V")}},
        {QStringLiteral("shape"),
         QJsonArray{QStringLiteral("1")}},
        {QStringLiteral("arrow"),
         QJsonArray{QStringLiteral("2")}},
        {QStringLiteral("brush"),
         QJsonArray{QStringLiteral("3"), QStringLiteral("P")}},
        {QStringLiteral("highlight"),
         QJsonArray{QStringLiteral("4"), QStringLiteral("H")}},
        {QStringLiteral("text"),
         QJsonArray{QStringLiteral("5"), QStringLiteral("T")}},
        {QStringLiteral("serial_number"),
         QJsonArray{QStringLiteral("6"), QStringLiteral("N")}},
        {QStringLiteral("filter"),
         QJsonArray{QStringLiteral("7"), QStringLiteral("F")}},
        {QStringLiteral("eraser"),
         QJsonArray{QStringLiteral("8"), QStringLiteral("E")}},
        {QStringLiteral("watermark"),
         QJsonArray{QStringLiteral("9")}},
    };
    for (auto it = drawingShortcutDefaults.cbegin(); it != drawingShortcutDefaults.cend(); ++it) {
        const QString key = QStringLiteral("drawing_shortcuts/") + it.key();
        const auto* entry = storage::ConfigurationSchema::entry(key);
        require(entry != nullptr && entry->defaultValue.toArray() == it.value() &&
                    entry->maximumListItems == 2,
                "drawing shortcut defaults and list limits must remain stable");
    }

    const QMap<QString, QJsonArray> screenshotShortcutDefaults{
        {QStringLiteral("move_tool"), QJsonArray{QStringLiteral("M")}},
        {QStringLiteral("move_cursor_up"),
         QJsonArray{QStringLiteral("W"), QStringLiteral("Up")}},
        {QStringLiteral("move_cursor_down"),
         QJsonArray{QStringLiteral("S"), QStringLiteral("Down")}},
        {QStringLiteral("move_cursor_left"),
         QJsonArray{QStringLiteral("A"), QStringLiteral("Left")}},
        {QStringLiteral("move_cursor_right"),
          QJsonArray{QStringLiteral("D"), QStringLiteral("Right")}},
        {QStringLiteral("move_entire_selection"), QJsonArray{QStringLiteral("Space")}},
        {QStringLiteral("keep_selection_width_and_height_consistent"),
         QJsonArray{QStringLiteral("Shift")}},
        {QStringLiteral("switch_selection_between_window_and_window_sub_element"),
         QJsonArray{QStringLiteral("Tab")}},
        {QStringLiteral("previous_screenshot_history"), QJsonArray{QStringLiteral(",")}},
        {QStringLiteral("next_screenshot_history"), QJsonArray{QStringLiteral(".")}},
        {QStringLiteral("select_previously_selected_area"), QJsonArray{QStringLiteral("R")}},
        {QStringLiteral("copy_color"), QJsonArray{QStringLiteral("C")}},
        {QStringLiteral("table_recognition"), QJsonArray{QStringLiteral("Ctrl+X")}},
        {QStringLiteral("qr_code_recognition"), QJsonArray{QStringLiteral("Ctrl+Q")}},
        {QStringLiteral("video_recording"), QJsonArray{QStringLiteral("Ctrl+R")}},
        {QStringLiteral("text_recognition"), QJsonArray{QStringLiteral("Ctrl+D")}},
        {QStringLiteral("text_translation"), QJsonArray{QStringLiteral("Ctrl+T")}},
        {QStringLiteral("scrolling_screenshot"), QJsonArray{QStringLiteral("L")}},
        {QStringLiteral("save_as_file"), QJsonArray{QStringLiteral("Ctrl+S")}},
        {QStringLiteral("pin_to_screen"), QJsonArray{QStringLiteral("Ctrl+F")}},
        {QStringLiteral("cancel_screenshot"), QJsonArray{QStringLiteral("Esc")}},
        {QStringLiteral("copy_to_clipboard"), QJsonArray{QStringLiteral("Ctrl+C")}},
        {QStringLiteral("undo"), QJsonArray{QStringLiteral("Ctrl+Z")}},
        {QStringLiteral("redo"), QJsonArray{QStringLiteral("Ctrl+Y")}},
    };
    for (auto it = screenshotShortcutDefaults.cbegin();
         it != screenshotShortcutDefaults.cend(); ++it) {
        const QString key = QStringLiteral("screenshot_shortcuts/") + it.key();
        const auto* entry = storage::ConfigurationSchema::entry(key);
        require(entry != nullptr && entry->defaultValue.toArray() == it.value() &&
                    entry->maximumListItems == 2,
                "screenshot shortcut defaults and list limits must remain stable");
    }

    const QMap<QString, QJsonArray> pinToScreenShortcutDefaults{
        {QStringLiteral("copy_to_clipboard"), QJsonArray{QStringLiteral("Ctrl+C")}},
        {QStringLiteral("copy_original_content"),
         QJsonArray{QStringLiteral("Ctrl+Shift+C")}},
        {QStringLiteral("show_text_recognition_results"),
         QJsonArray{QStringLiteral("Ctrl+D")}},
        {QStringLiteral("drawing_mode"), QJsonArray{QStringLiteral("Ctrl+E")}},
        {QStringLiteral("thumbnail_mode"), QJsonArray{QStringLiteral("R")}},
        {QStringLiteral("close_window"), QJsonArray{QStringLiteral("Esc")}},
        {QStringLiteral("move_cursor_up"),
         QJsonArray{QStringLiteral("W"), QStringLiteral("Up")}},
        {QStringLiteral("move_cursor_down"),
         QJsonArray{QStringLiteral("S"), QStringLiteral("Down")}},
        {QStringLiteral("move_cursor_left"),
         QJsonArray{QStringLiteral("A"), QStringLiteral("Left")}},
        {QStringLiteral("move_cursor_right"),
         QJsonArray{QStringLiteral("D"), QStringLiteral("Right")}},
    };
    for (auto it = pinToScreenShortcutDefaults.cbegin();
         it != pinToScreenShortcutDefaults.cend(); ++it) {
        const QString key = QStringLiteral("pin_to_screen_shortcuts/") + it.key();
        const auto* entry = storage::ConfigurationSchema::entry(key);
        require(entry != nullptr && entry->defaultValue.toArray() == it.value() &&
                    entry->maximumListItems == 2,
                "pinned-window shortcut defaults and list limits must remain stable");
    }

    for (const QString& malformed : {QStringLiteral("Ctrl+K, Ctrl+C"),
                                     QStringLiteral("Ctrl"),
                                     QStringLiteral("NotARealKey")}) {
        const auto normalized = storage::ConfigurationSchema::normalize(
            QStringLiteral("drawing_shortcuts/shape"), QJsonArray{malformed});
        require(normalized.valid && normalized.changed && normalized.value.toArray().isEmpty(),
                "malformed drawing shortcuts must be removed during normalization");
    }

    const auto normalizedDrawingTools = storage::ConfigurationSchema::normalize(
        QStringLiteral("drawing/quick_selection_disabled_tools"),
        QJsonArray{QStringLiteral(" FREE-DRAW "), QStringLiteral("pen-filter"),
                   QStringLiteral("PEN-FILTER"), QStringLiteral("unknown"), 42});
    require(normalizedDrawingTools.valid && normalizedDrawingTools.changed &&
                normalizedDrawingTools.value.toArray() ==
                    QJsonArray{QStringLiteral("free-draw"), QStringLiteral("pen-filter")},
            "drawing-tool lists must trim, canonicalize, deduplicate, and drop invalid entries");
    require(!storage::ConfigurationSchema::normalize(
                 QStringLiteral("drawing/quick_selection_disabled_tools"),
                 QStringLiteral("free-draw"))
                 .valid,
            "drawing-tool lists must reject non-array values");

    for (const int frameRate : {10, 15, 24, 30, 60, 120, 83}) {
        require(storage::ConfigurationSchema::normalize(
                    QStringLiteral("screen_recording/frame_rate"), frameRate)
                    .valid,
                "every advertised video frame rate must be accepted");
    }
    for (const int frameRate : {0, 25, 29, 84, 121}) {
        require(!storage::ConfigurationSchema::normalize(
                     QStringLiteral("screen_recording/frame_rate"), frameRate)
                     .valid,
                "unadvertised video frame rates must be rejected");
    }
    for (const int frameRate : {10, 15, 24}) {
        require(storage::ConfigurationSchema::normalize(
                    QStringLiteral("screen_recording/animated_image_frame_rate"), frameRate)
                    .valid,
                "every advertised animated-image frame rate must be accepted");
    }
    require(!storage::ConfigurationSchema::normalize(
                 QStringLiteral("screen_recording/animated_image_frame_rate"), 30)
                 .valid &&
                !storage::ConfigurationSchema::normalize(
                     QStringLiteral("screen_recording/frame_rate"), 30.5)
                     .valid,
            "frame-rate settings must reject unsupported and non-integral values");

    const QMap<QString, QStringList> allowedStringValues{
        {QStringLiteral("screenshot/auto_execute_after_text_recognition"),
         {QStringLiteral("no_action"), QStringLiteral("copy_text"),
          QStringLiteral("copy_text_and_end_screenshot"), QStringLiteral("quick_copy_text"),
          QStringLiteral("quick_copy_text_and_end_screenshot"),
          QStringLiteral("enable_edit_mode")}},
        {QStringLiteral("screenshot/double_click_action"),
         {QStringLiteral("copy"), QStringLiteral("save"), QStringLiteral("pin"),
          QStringLiteral("none")}},
        {QStringLiteral("screenshot/middle_mouse_button_action"),
         {QStringLiteral("copy"), QStringLiteral("save"), QStringLiteral("pin"),
          QStringLiteral("none")}},
        {QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode"),
         {QStringLiteral("mouse_position"), QStringLiteral("top_left"),
          QStringLiteral("top_right"), QStringLiteral("bottom_left"),
          QStringLiteral("bottom_right"), QStringLiteral("center")}},
        {QStringLiteral("screen_recording/clarity"),
         {QStringLiteral("4k"), QStringLiteral("2k"), QStringLiteral("1080p"),
          QStringLiteral("720p"), QStringLiteral("480p")}},
        {QStringLiteral("screen_recording/animated_image_clarity"),
         {QStringLiteral("1080p"), QStringLiteral("720p"), QStringLiteral("480p")}},
        {QStringLiteral("screen_recording/animated_image_format"),
         {QStringLiteral("gif"), QStringLiteral("apng"), QStringLiteral("webp")}},
        {QStringLiteral("screen_recording/encoder"),
         {QStringLiteral("h264"), QStringLiteral("h265")}},
        {QStringLiteral("screen_recording/encoding_preset"),
         {QStringLiteral("ultrafast"), QStringLiteral("veryfast"),
          QStringLiteral("medium"), QStringLiteral("veryslow"), QStringLiteral("placebo")}},
        {QStringLiteral("tray/left_click_action"),
         {QStringLiteral("screenshot"), QStringLiteral("show_main_window")}},
    };
    for (auto it = allowedStringValues.cbegin(); it != allowedStringValues.cend(); ++it) {
        const auto* entry = storage::ConfigurationSchema::entry(it.key());
        require(entry != nullptr && entry->allowedStringValues == it.value(),
                "select schema options must exactly match the UI contract");
        for (const QString& value : it.value()) {
            require(storage::ConfigurationSchema::normalize(it.key(), value).valid,
                    "every advertised select value must be accepted");
        }
        require(!storage::ConfigurationSchema::normalize(
                     it.key(), QStringLiteral("unsupported-value"))
                     .valid,
                "select settings must reject unsupported values");
    }
}

void screenshotUiSchemaRepairsStructuredValues() {
    const auto validColor = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_ui/cursor_guide_line_color"), QStringLiteral("#abcdef80"));
    require(validColor.valid && validColor.changed &&
                validColor.value.toString() == QStringLiteral("#ABCDEF80"),
            "RGBA colors were not normalized canonically");
    require(!storage::ConfigurationSchema::normalize(
                 QStringLiteral("screenshot_ui/cursor_guide_line_color"), QStringLiteral("#ABCDEF"))
                 .valid,
            "RGBA color schema accepted an incomplete value");

    const QJsonObject malformedLayout{
        {QStringLiteral("positions"),
         QJsonArray{
             QJsonArray{QStringLiteral("watermark"), QStringLiteral("shape"),
                        QStringLiteral("unknown"), QStringLiteral("watermark")},
             QJsonArray{QStringLiteral("line"), QStringLiteral("shape")},
             QStringLiteral("not-a-position"),
             QJsonArray{QStringLiteral("rectangle-highlight"),
                        QStringLiteral("pen-highlight")},
         }},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("shape"), QStringLiteral("arrow"),
                    QStringLiteral("free-draw"), QStringLiteral("pen-highlight"),
                    QStringLiteral("arrow")}},
    };
    const auto normalized = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_toolbar/layout"), malformedLayout);
    const QJsonObject layout = normalized.value.toObject();
    const QJsonArray positions = layout.value(QStringLiteral("positions")).toArray();
    require(
        normalized.valid && normalized.changed && layout.size() == 2 &&
            positions ==
                QJsonArray{
                    QJsonArray{QStringLiteral("watermark"), QStringLiteral("shape")},
                    QJsonArray{QStringLiteral("line")},
                    QJsonArray{QStringLiteral("highlighter")},
                    QJsonArray{QStringLiteral("spotlight")},
                    QJsonArray{QStringLiteral("text")},
                    QJsonArray{QStringLiteral("serial-number")},
                    QJsonArray{QStringLiteral("filter")},
                    QJsonArray{QStringLiteral("eraser")},
                } &&
            layout.value(QStringLiteral("hidden")).toArray() ==
                QJsonArray{QStringLiteral("arrow"), QStringLiteral("free-draw")},
        "toolbar layout normalization did not preserve hidden nested membership");

    const QJsonObject legacyLayout{
        {QStringLiteral("order"),
         QJsonArray{QStringLiteral("move"), QStringLiteral("highlight"), QStringLiteral("shape"),
                    QStringLiteral("arrow-line"), QStringLiteral("watermark"),
                    QStringLiteral("highlight")}},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("highlight"), QStringLiteral("arrow-line"),
                    QStringLiteral("watermark"), QStringLiteral("free-draw")}},
    };
    const auto migrated = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_toolbar/layout"), legacyLayout);
    require(migrated.valid && migrated.changed &&
                migrated.value.toObject() ==
                    QJsonObject{
                        {QStringLiteral("positions"),
                         QJsonArray{
                             QJsonArray{QStringLiteral("shape")},
                             QJsonArray{QStringLiteral("text")},
                             QJsonArray{QStringLiteral("serial-number")},
                             QJsonArray{QStringLiteral("filter")},
                             QJsonArray{QStringLiteral("eraser")},
                         }},
                        {QStringLiteral("hidden"),
                         QJsonArray{QStringLiteral("spotlight"),
                                    QStringLiteral("highlighter"),
                                    QStringLiteral("line"), QStringLiteral("arrow"),
                                    QStringLiteral("watermark"),
                                    QStringLiteral("free-draw")}}},
            "legacy hidden toolbar groups were not migrated into hidden drawing entries");
}

void screenshotUiAdaptersRoundTripTypedValues() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create screenshot UI adapter directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create screenshot UI executable directory");
    static_cast<void>(initialize(executable, temporary.path()));

    const storage::ScreenshotUiSettings screenshot;
    require(screenshot.setSelectionMaskColor(QColor(18, 52, 86, 120)) &&
                screenshot.selectionMaskColor() == QColor(18, 52, 86, 120) &&
                storage::colorToRgbaString(screenshot.selectionMaskColor()) ==
                    QStringLiteral("#12345678") &&
                storage::colorFromRgbaString(QStringLiteral("#ABCDEF01")) ==
                    QColor(171, 205, 239, 1),
            "typed RGBA settings did not round-trip");

    storage::ScreenshotToolbarLayout layout;
    layout.positions = {
        {QStringLiteral("watermark"), QStringLiteral("shape"), QStringLiteral("watermark"),
         QStringLiteral("unknown")},
        {QStringLiteral("line")},
        {QStringLiteral("rectangle-highlight")},
        {QStringLiteral("shape")},
    };
    layout.hidden = {QStringLiteral("shape"), QStringLiteral("arrow"),
                     QStringLiteral("free-draw"), QStringLiteral("pen-highlight"),
                     QStringLiteral("arrow")};
    const storage::ScreenshotToolbarSettings toolbar;
    const QVector<QStringList> expectedPositions{
        {QStringLiteral("watermark"), QStringLiteral("shape")},
        {QStringLiteral("line")},
        {QStringLiteral("highlighter")},
        {QStringLiteral("spotlight")},
        {QStringLiteral("text")},
        {QStringLiteral("serial-number")},
        {QStringLiteral("filter")},
        {QStringLiteral("eraser")},
    };
    const storage::ScreenshotToolbarLayout expectedLayout{
        expectedPositions,
        {QStringLiteral("arrow"), QStringLiteral("free-draw")},
    };
    require(toolbar.setLayout(layout) && toolbar.layout() == expectedLayout,
            "typed toolbar layout did not preserve normalized visible and hidden entries");
}

void screenshotTranslationSettingsRoundTripSupportedValues() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create translation settings directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create translation settings executable directory");
    static_cast<void>(initialize(executable, temporary.path()));

    const storage::ScreenshotTranslationSettings translation;
    require(translation.configuration() ==
                storage::ScreenshotTranslationConfiguration{QStringLiteral("auto"), {}, {}},
            "translation settings should default to Auto source and runtime-derived target/model");
    const storage::ScreenshotTranslationConfiguration selected{
        QStringLiteral("ja"), QStringLiteral("zh-Hant"), QStringLiteral("model-a")};
    require(translation.setConfiguration(selected) && translation.configuration() == selected,
            "translation language and model selections should persist together");

    const auto unsupportedTarget = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_translation/target_language"), QStringLiteral("auto"));
    require(!unsupportedTarget.valid,
            "Auto Detect should be accepted only for the source translation language");
    const auto normalizedSource = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_translation/source_language"), QStringLiteral(" ZH-HANS "));
    require(normalizedSource.valid && normalizedSource.changed &&
                normalizedSource.value.toString() == QStringLiteral("zh-Hans"),
            "translation language codes should normalize to their canonical persisted form");
}

void verifyPinToScreenShortcutSettings() {
    const storage::PinToScreenShortcutSettings shortcuts;
    const QMap<QString, QStringList> defaults = shortcuts.allShortcuts();
    require(defaults.size() == 10 &&
                defaults.value(QStringLiteral("copy_to_clipboard")) ==
                    QStringList{QStringLiteral("Ctrl+C")} &&
                defaults.value(QStringLiteral("copy_original_content")) ==
                    QStringList{QStringLiteral("Ctrl+Shift+C")} &&
                defaults.value(QStringLiteral("show_text_recognition_results")) ==
                    QStringList{QStringLiteral("Ctrl+D")} &&
                defaults.value(QStringLiteral("drawing_mode")) ==
                    QStringList{QStringLiteral("Ctrl+E")} &&
                defaults.value(QStringLiteral("thumbnail_mode")) ==
                    QStringList{QStringLiteral("R")} &&
                defaults.value(QStringLiteral("close_window")) ==
                    QStringList{QStringLiteral("Esc")} &&
                defaults.value(QStringLiteral("move_cursor_up")) ==
                    QStringList{QStringLiteral("W"), QStringLiteral("Up")} &&
                defaults.value(QStringLiteral("move_cursor_right")) ==
                    QStringList{QStringLiteral("D"), QStringLiteral("Right")} &&
                shortcuts.shortcuts(QStringLiteral("unsupported")).isEmpty() &&
                !shortcuts.setShortcuts(QStringLiteral("unsupported"),
                                        {QStringLiteral("Q")}),
            "pinned-window shortcut adapter must expose ten stable actions and defaults");
    require(shortcuts.setShortcuts(QStringLiteral("drawing_mode"),
                                   {QStringLiteral("Alt+E")}) &&
                shortcuts.shortcuts(QStringLiteral("drawing_mode")) ==
                    QStringList{QStringLiteral("Alt+E")},
            "pinned-window shortcuts must round-trip through the typed adapter");
    QMap<QString, QStringList> duplicates = shortcuts.allShortcuts();
    duplicates.insert(QStringLiteral("thumbnail_mode"), {QStringLiteral("Ctrl+C")});
    require(!shortcuts.setAllShortcutsAtomic(duplicates),
            "pinned-window shortcuts must reject duplicate bindings atomically");
}

void pinToScreenShortcutSettingsRoundTrip() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create pinned-shortcut adapter directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable),
            "failed to create pinned-shortcut executable directory");
    static_cast<void>(initialize(executable, temporary.path()));
    verifyPinToScreenShortcutSettings();
    storage::ApplicationStorage::instance().shutdown();
}

void newSettingsAdaptersRoundTripAndRejectInvalidValues() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create new-settings adapter directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create new-settings executable directory");
    auto& applicationStorage = initialize(executable, temporary.path());

    const storage::ScreenshotSettings screenshot;
    require(screenshot.autoExecuteAfterTextRecognition() == QStringLiteral("no_action") &&
                screenshot.doubleClickAction() == QStringLiteral("copy") &&
                screenshot.middleMouseButtonAction() == QStringLiteral("pin") &&
                !screenshot.autoSaveAfterCopy() && !screenshot.copyImageFileToClipboard() &&
                screenshot.imageFormat() == QStringLiteral("png") &&
                screenshot.manualSaveFilenameFormat() ==
                    QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}") &&
                screenshot.autoSaveFilenameFormat() ==
                    QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}") &&
                screenshot.lastManualSaveDirectory().isEmpty() &&
                QDir::fromNativeSeparators(screenshot.imageSaveDirectory())
                    .endsWith(QStringLiteral("/SnowShot")),
            "screenshot adapters must expose requested defaults");
    require(screenshot.setAutoExecuteAfterTextRecognition(
                QStringLiteral("quick_copy_text_and_end_screenshot")) &&
                screenshot.setDoubleClickAction(QStringLiteral("save")) &&
                screenshot.setMiddleMouseButtonAction(QStringLiteral("none")) &&
                screenshot.setAutoSaveAfterCopy(true) &&
                screenshot.setCopyImageFileToClipboard(true) &&
                screenshot.setImageSaveDirectory(QStringLiteral("D:/Captures")) &&
                screenshot.setLastManualSaveDirectory(QStringLiteral("D:/Exports")) &&
                screenshot.setImageFormat(QStringLiteral("webp")) &&
                screenshot.setManualSaveFilenameFormat(QStringLiteral("Manual_{yyyyMMdd}")) &&
                screenshot.setAutoSaveFilenameFormat(QStringLiteral("Auto_{HHmmss}")) &&
                screenshot.autoExecuteAfterTextRecognition() ==
                    QStringLiteral("quick_copy_text_and_end_screenshot") &&
                screenshot.doubleClickAction() == QStringLiteral("save") &&
                screenshot.middleMouseButtonAction() == QStringLiteral("none") &&
                screenshot.autoSaveAfterCopy() && screenshot.copyImageFileToClipboard() &&
                screenshot.imageSaveDirectory() == QStringLiteral("D:/Captures") &&
                screenshot.lastManualSaveDirectory() == QStringLiteral("D:/Exports") &&
                screenshot.imageFormat() == QStringLiteral("webp") &&
                screenshot.manualSaveFilenameFormat() == QStringLiteral("Manual_{yyyyMMdd}") &&
                screenshot.autoSaveFilenameFormat() == QStringLiteral("Auto_{HHmmss}"),
            "screenshot adapters must persist every new value type");
    require(!screenshot.setDoubleClickAction(QStringLiteral("unsupported")) &&
                !screenshot.setImageFormat(QStringLiteral("bmp")) &&
                !screenshot.setAutoSaveFilenameFormat(QStringLiteral("invalid/name")) &&
                screenshot.doubleClickAction() == QStringLiteral("save"),
            "invalid screenshot actions must be rejected without changing the stored value");

    const storage::DrawingSettings drawing;
    require(drawing.quickSelectionDisabledTools() ==
                QStringList{QStringLiteral("free-draw"), QStringLiteral("pen-filter")} &&
                drawing.setQuickSelectionDisabledTools(
                    {QStringLiteral(" PEN-FILTER "), QStringLiteral("shape"),
                     QStringLiteral("pen-filter"), QStringLiteral("unsupported")}) &&
                drawing.quickSelectionDisabledTools() ==
                    QStringList{QStringLiteral("pen-filter"), QStringLiteral("shape")},
            "drawing exclusion adapter must use schema canonicalization");

    const storage::PinToScreenSettings pin;
    require(pin.mouseWheelZoomMode() == QStringLiteral("mouse_position") &&
                pin.automaticTextRecognition() && pin.autoResizeWindow() &&
                pin.setMouseWheelZoomMode(QStringLiteral("bottom_right")) &&
                pin.setAutomaticTextRecognition(false) && pin.setAutoResizeWindow(false) &&
                pin.mouseWheelZoomMode() == QStringLiteral("bottom_right") &&
                !pin.automaticTextRecognition() && !pin.autoResizeWindow(),
            "pin-to-screen adapters must round-trip requested settings");
    require(!pin.setMouseWheelZoomMode(QStringLiteral("unsupported")) &&
                pin.mouseWheelZoomMode() == QStringLiteral("bottom_right"),
            "invalid pin zoom modes must not replace the stored mode");

    const storage::RecordingSettings recording;
    require(recording.screenRecordingClarity() == QStringLiteral("1080p") &&
                recording.frameRate() == 30 &&
                recording.animatedImageClarity() == QStringLiteral("1080p") &&
                recording.animatedImageFrameRate() == 10 &&
                recording.animatedImageFormat() == QStringLiteral("gif") &&
                recording.encoder() == QStringLiteral("h264") &&
                recording.encodingPreset() == QStringLiteral("veryfast") &&
                recording.hideToolbarInRecording() &&
                QDir::fromNativeSeparators(recording.videoSaveDirectory())
                    .endsWith(QStringLiteral("/SnowShot")) &&
                recording.videoFilenameFormat() ==
                    QStringLiteral("SnowShot_Video_{YYYY-MM-DD_HH-mm-ss}"),
            "recording adapters must expose requested defaults");
    require(recording.setScreenRecordingClarity(QStringLiteral("2k")) && recording.setFrameRate(83) &&
                recording.setAnimatedImageClarity(QStringLiteral("720p")) &&
                recording.setAnimatedImageFrameRate(24) &&
                recording.setAnimatedImageFormat(QStringLiteral("webp")) &&
                recording.setEncoder(QStringLiteral("h265")) &&
                recording.setEncodingPreset(QStringLiteral("placebo")) &&
                recording.setHideToolbarInRecording(false) &&
                recording.setVideoSaveDirectory(QStringLiteral("D:/Recordings")) &&
                recording.setVideoFilenameFormat(QStringLiteral("Recording_{yyyyMMdd}")) &&
                recording.screenRecordingClarity() == QStringLiteral("2k") &&
                recording.frameRate() == 83 &&
                recording.animatedImageClarity() == QStringLiteral("720p") &&
                recording.animatedImageFrameRate() == 24 &&
                recording.animatedImageFormat() == QStringLiteral("webp") &&
                recording.encoder() == QStringLiteral("h265") &&
                recording.encodingPreset() == QStringLiteral("placebo") &&
                !recording.hideToolbarInRecording() &&
                recording.videoSaveDirectory() == QStringLiteral("D:/Recordings") &&
                recording.videoFilenameFormat() == QStringLiteral("Recording_{yyyyMMdd}"),
            "recording adapters must round-trip every requested option");
    require(!recording.setFrameRate(25) && !recording.setAnimatedImageFrameRate(30) &&
                !recording.setScreenRecordingClarity(QStringLiteral("8k")) &&
                !recording.setVideoFilenameFormat(QStringLiteral("invalid/name")) &&
                recording.frameRate() == 83 && recording.animatedImageFrameRate() == 24 &&
                recording.screenRecordingClarity() == QStringLiteral("2k"),
            "recording adapters must reject unadvertised values atomically");

    const storage::TraySettings tray;
    const storage::NetworkSettings network;
    const storage::GlobalShortcutSettings globalShortcuts;
    require(network.proxy() == QStringLiteral("none") &&
                network.setProxy(QStringLiteral("system")) &&
                network.proxy() == QStringLiteral("system") &&
                !network.setProxy(QStringLiteral("unsupported")) &&
                network.proxy() == QStringLiteral("system") &&
                tray.leftClickAction() == QStringLiteral("screenshot") &&
                tray.setLeftClickAction(QStringLiteral("show_main_window")) &&
                tray.leftClickAction() == QStringLiteral("show_main_window") &&
                !tray.setLeftClickAction(QStringLiteral("unsupported")) &&
                !globalShortcuts.disableOnFocusedFullscreenWindow() &&
                globalShortcuts.setDisableOnFocusedFullscreenWindow(true) &&
                globalShortcuts.disableOnFocusedFullscreenWindow() &&
                tray.menuOptions().size() == 10 &&
                tray.menuOptions().contains(QStringLiteral("tray.show-main-window")) &&
                tray.setMenuOptions({QStringLiteral("tray.exit"),
                                     QStringLiteral("quick.screenshot"),
                                     QStringLiteral("quick.screenshot"),
                                     QStringLiteral("unknown")}) &&
                tray.menuOptions() ==
                    QStringList{QStringLiteral("tray.exit"), QStringLiteral("quick.screenshot")},
            "network, tray, and global-hotkey adapters must round-trip and validate settings");

    const storage::DrawingShortcutSettings drawingShortcuts;
    const storage::ScreenshotShortcutSettings screenshotShortcuts;
    const QMap<QString, QStringList> screenshotDefaults = screenshotShortcuts.allShortcuts();
    require(screenshotDefaults.size() == 24 &&
                screenshotShortcuts.moveTool() == QStringList{QStringLiteral("M")} &&
                screenshotShortcuts.moveCursorUp() ==
                    QStringList{QStringLiteral("W"), QStringLiteral("Up")} &&
                screenshotShortcuts.moveCursorDown() ==
                    QStringList{QStringLiteral("S"), QStringLiteral("Down")} &&
                screenshotShortcuts.moveCursorLeft() ==
                    QStringList{QStringLiteral("A"), QStringLiteral("Left")} &&
                 screenshotShortcuts.moveCursorRight() ==
                     QStringList{QStringLiteral("D"), QStringLiteral("Right")} &&
                 screenshotShortcuts.moveEntireSelection() ==
                     QStringList{QStringLiteral("Space")} &&
                 screenshotShortcuts.keepSelectionWidthAndHeightConsistent() ==
                     QStringList{QStringLiteral("Shift")} &&
                 screenshotShortcuts.switchSelectionBetweenWindowAndWindowSubElement() ==
                     QStringList{QStringLiteral("Tab")} &&
                 screenshotShortcuts.previousScreenshotHistory() ==
                     QStringList{QStringLiteral(",")} &&
                 screenshotShortcuts.nextScreenshotHistory() ==
                     QStringList{QStringLiteral(".")} &&
                 screenshotShortcuts.selectPreviouslySelectedArea() ==
                     QStringList{QStringLiteral("R")} &&
                 screenshotShortcuts.copyColor() == QStringList{QStringLiteral("C")} &&
                 screenshotDefaults.value(QStringLiteral("pin_to_screen")) ==
                     QStringList{QStringLiteral("Ctrl+F")} &&
                 screenshotDefaults.value(QStringLiteral("cancel_screenshot")) ==
                     QStringList{QStringLiteral("Esc")} &&
                 screenshotDefaults.value(QStringLiteral("copy_to_clipboard")) ==
                     QStringList{QStringLiteral("Ctrl+C")} &&
                 screenshotDefaults.value(QStringLiteral("undo")) ==
                     QStringList{QStringLiteral("Ctrl+Z")} &&
                 screenshotDefaults.value(QStringLiteral("redo")) ==
                     QStringList{QStringLiteral("Ctrl+Y")} &&
                 screenshotShortcuts.shortcuts(QStringLiteral("unsupported")).isEmpty() &&
                !screenshotShortcuts.setShortcuts(QStringLiteral("unsupported"),
                                                  {QStringLiteral("Q")}),
            "screenshot shortcut adapter must expose all stable actions and defaults");
    require(screenshotShortcuts.setShortcuts(QStringLiteral("cancel_screenshot"),
                                             {QStringLiteral("Esc")}) &&
                screenshotShortcuts.setShortcuts(QStringLiteral("copy_to_clipboard"),
                                                 {QStringLiteral("Ctrl+C")}) &&
                screenshotShortcuts.setShortcuts(QStringLiteral("undo"),
                                                 {QStringLiteral("Ctrl+Z")}),
            "reserved screenshot commands must accept their own configurable defaults");
    require(screenshotShortcuts.setMoveTool({QStringLiteral("Alt+M")}) &&
                screenshotShortcuts.moveTool() == QStringList{QStringLiteral("Alt+M")} &&
                screenshotShortcuts.setMoveCursorUp({QStringLiteral("Ctrl+Alt+Up")}) &&
                screenshotShortcuts.moveCursorUp() ==
                    QStringList{QStringLiteral("Ctrl+Alt+Up")},
            "screenshot shortcuts must round-trip through the typed adapter");
    require(screenshotShortcuts.setMoveCursorRight({QStringLiteral("1")}) &&
                screenshotShortcuts.moveCursorRight() == QStringList{QStringLiteral("1")},
            "screenshot shortcuts must allow a key assigned in the drawing category");
    QMap<QString, QStringList> swappedHistoryShortcuts = screenshotShortcuts.allShortcuts();
    swappedHistoryShortcuts.insert(QStringLiteral("previous_screenshot_history"),
                                   {QStringLiteral(".")});
    swappedHistoryShortcuts.insert(QStringLiteral("next_screenshot_history"),
                                   {QStringLiteral(",")});
    require(screenshotShortcuts.setAllShortcutsAtomic(swappedHistoryShortcuts) &&
                screenshotShortcuts.previousScreenshotHistory() ==
                    QStringList{QStringLiteral(".")} &&
                screenshotShortcuts.nextScreenshotHistory() ==
                    QStringList{QStringLiteral(",")},
            "history shortcuts must allow comma and period to be swapped atomically");

    verifyPinToScreenShortcutSettings();

    const QMap<QString, QStringList> defaults = drawingShortcuts.allShortcuts();
    require(defaults.size() == 10 &&
                defaults.value(QStringLiteral("select")) ==
                    QStringList{QStringLiteral("V")} &&
                defaults.value(QStringLiteral("shape")) ==
                    QStringList{QStringLiteral("1")} &&
                defaults.value(QStringLiteral("arrow")) ==
                    QStringList{QStringLiteral("2")} &&
                defaults.value(QStringLiteral("watermark")) ==
                    QStringList{QStringLiteral("9")} &&
                drawingShortcuts.shortcuts(QStringLiteral("unsupported")).isEmpty() &&
                !drawingShortcuts.setShortcuts(QStringLiteral("unsupported"),
                                               {QStringLiteral("Q")}),
            "drawing shortcut adapter must expose ten stable tools only");

    require(drawingShortcuts.setSelect({QStringLiteral("Ctrl+Shift+V")}) &&
                drawingShortcuts.select() == QStringList{QStringLiteral("Ctrl+Shift+V")},
            "Select drawing shortcuts must round-trip through the typed adapter");

    require(drawingShortcuts.setShape({QStringLiteral("Ctrl+Shift+K"),
                                       QStringLiteral("Alt+1")}) &&
                drawingShortcuts.shape() ==
                    QStringList{QStringLiteral("Ctrl+Shift+K"), QStringLiteral("Alt+1")},
            "drawing shortcut adapter must persist normalized tool shortcuts");
    require(drawingShortcuts.setWatermark({QStringLiteral("Alt+M")}) &&
                drawingShortcuts.watermark() == QStringList{QStringLiteral("Alt+M")},
            "drawing shortcuts must allow a key assigned in the screenshot category");
    const QMap<QString, QStringList> beforeCollision = drawingShortcuts.allShortcuts();
    require(!drawingShortcuts.setArrow({QStringLiteral("ctrl+shift+k")}) &&
                drawingShortcuts.allShortcuts() == beforeCollision,
            "case-insensitive cross-tool collisions must reject the complete atomic update");

    for (const QString& reserved : {QStringLiteral("Escape"), QStringLiteral("Delete"),
                                    QStringLiteral("Ctrl+C"), QStringLiteral("Ctrl+Shift+Z"),
                                    QStringLiteral(","), QStringLiteral("F4")}) {
        require(!drawingShortcuts.setArrow({reserved}) &&
                    drawingShortcuts.allShortcuts() == beforeCollision,
                "canvas and screenshot commands must remain reserved from drawing shortcuts");
    }

    QMap<QString, QStringList> incomplete = beforeCollision;
    incomplete.remove(QStringLiteral("watermark"));
    require(!drawingShortcuts.setAllShortcutsAtomic(incomplete) &&
                drawingShortcuts.allShortcuts() == beforeCollision,
            "atomic drawing shortcut updates must require all ten tools");

    QMap<QString, QStringList> emptyAssignment = beforeCollision;
    emptyAssignment.insert(QStringLiteral("shape"), {});
    require(drawingShortcuts.setAllShortcutsAtomic(emptyAssignment) &&
                drawingShortcuts.shape().isEmpty(),
            "drawing shortcuts must allow a tool to be deliberately left unassigned");

    require(applicationStorage.configuration().flushNow().success,
            "new settings adapter mutations must be flushable");
}

void smartSelectionAccessorAndSignal() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create smart-selection directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create smart-selection executable directory");
    auto& applicationStorage = initialize(executable, temporary.path());
    bool changed = false;
    QObject::connect(&applicationStorage, &storage::ApplicationStorage::smartSelectionChanged,
                     [&changed](bool enabled) { changed = !enabled; });
    require(applicationStorage.smartSelectionEnabled() &&
                applicationStorage.requestSmartSelection(false) &&
                !applicationStorage.smartSelectionEnabled() && changed,
            "smart-selection accessor did not persist or signal changes");
    require(applicationStorage.requestSmartSelection(true) &&
                applicationStorage.smartSelectionEnabled(),
            "smart-selection setting did not restore its enabled default");
}

void unknownFieldsArePreserved() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create unknown-field directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    writeBytes(config,
               QByteArrayLiteral("{\n"
                                 "  \"storage\": {\"schema_version\": 1, \"future_flag\": true},\n"
                                 "  \"interface\": {\"theme_mode\": \"light\"},\n"
                                 "  \"extension\": {\"nested\": [1, 2, 3]}\n"
                                 "}\n"));
    storage::ConfigurationStore store(config, true, true, 60000);
    require(store.setValue(QStringLiteral("interface/sidebar_collapsed"), true) &&
                store.flushNow().success,
            "failed to update document containing unknown fields");
    const QJsonObject root = readObject(config);
    require(root.value(QStringLiteral("storage"))
                    .toObject()
                    .value(QStringLiteral("future_flag"))
                    .toBool() &&
                root.value(QStringLiteral("extension"))
                        .toObject()
                        .value(QStringLiteral("nested"))
                        .toArray()
                        .size() == 3,
            "schema-v1 unknown fields were erased");
}

void legacyRecordingKeysMigrateToScreenRecording() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create screen-recording migration directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    writeBytes(config,
               QByteArrayLiteral("{\n"
                                 "  \"storage\": {\"schema_version\": 1},\n"
                                 "  \"global_shortcuts\": {\n"
                                 "    \"video_record\": [\"Ctrl+Alt+8\"],\n"
                                 "    \"video_record_copy\": [\"Ctrl+Alt+9\"]\n"
                                 "  },\n"
                                 "  \"video_recording\": {\n"
                                 "    \"video_clarity\": \"2k\",\n"
                                 "    \"frame_rate\": 60,\n"
                                 "    \"enable_microphone\": true\n"
                                 "  }\n"
                                 "}\n"));

    storage::ConfigurationStore store(config, true, true, 60000);
    require(store.value(QStringLiteral("global_shortcuts/screen_record")).toArray() ==
                    QJsonArray{QStringLiteral("Ctrl+Alt+8")} &&
                store.value(QStringLiteral("global_shortcuts/screen_record_copy")).toArray() ==
                    QJsonArray{QStringLiteral("Ctrl+Alt+9")} &&
                store.value(QStringLiteral("screen_recording/clarity")).toString() ==
                    QStringLiteral("2k") &&
                store.value(QStringLiteral("screen_recording/frame_rate")).toInt() == 60 &&
                store.value(QStringLiteral("screen_recording/enable_microphone")).toBool(),
            "legacy recording settings were not exposed through screen-recording keys");
    require(store.flushNow().success, "failed to flush migrated screen-recording settings");

    const QJsonObject root = readObject(config);
    const QJsonObject shortcuts = root.value(QStringLiteral("global_shortcuts")).toObject();
    const QJsonObject screenRecording = root.value(QStringLiteral("screen_recording")).toObject();
    const QJsonObject legacyRecording = root.value(QStringLiteral("video_recording")).toObject();
    require(shortcuts.value(QStringLiteral("screen_record")).toArray() ==
                    QJsonArray{QStringLiteral("Ctrl+Alt+8")} &&
                shortcuts.value(QStringLiteral("screen_record_copy")).toArray() ==
                    QJsonArray{QStringLiteral("Ctrl+Alt+9")} &&
                !shortcuts.contains(QStringLiteral("video_record")) &&
                !shortcuts.contains(QStringLiteral("video_record_copy")) &&
                screenRecording.value(QStringLiteral("clarity")).toString() ==
                    QStringLiteral("2k") &&
                screenRecording.value(QStringLiteral("frame_rate")).toInt() == 60 &&
                screenRecording.value(QStringLiteral("enable_microphone")).toBool() &&
                legacyRecording.isEmpty(),
            "legacy recording keys were not rewritten canonically");
}

void legacyDrawingAliasesMigrateToScreenshotShortcuts() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create drawing-shortcut migration directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    writeBytes(config,
               QByteArrayLiteral("{\n"
                                 "  \"storage\": {\"schema_version\": 1},\n"
                                 "  \"drawing_shortcuts\": {\n"
                                 "    \"shape\": [\"Ctrl+K\", \"S\"],\n"
                                 "    \"arrow\": [\"2\", \"a\"],\n"
                                 "    \"brush\": [\"3\", \"P\"],\n"
                                 "    \"watermark\": [\"w\", \"Alt+9\"]\n"
                                 "  }\n"
                                 "}\n"));

    storage::ConfigurationStore store(config, true, true, 60000);
    require(store.value(QStringLiteral("drawing_shortcuts/shape")).toArray() ==
                    QJsonArray{QStringLiteral("Ctrl+K")} &&
                store.value(QStringLiteral("drawing_shortcuts/arrow")).toArray() ==
                    QJsonArray{QStringLiteral("2")} &&
                store.value(QStringLiteral("drawing_shortcuts/brush")).toArray() ==
                    QJsonArray{QStringLiteral("3"), QStringLiteral("P")} &&
                store.value(QStringLiteral("drawing_shortcuts/watermark")).toArray() ==
                    QJsonArray{QStringLiteral("Alt+9")} &&
                store.value(QStringLiteral("screenshot_shortcuts/move_cursor_up")).toArray() ==
                    QJsonArray{QStringLiteral("W"), QStringLiteral("Up")} &&
                store.value(QStringLiteral("screenshot_shortcuts/move_cursor_down")).toArray() ==
                    QJsonArray{QStringLiteral("S"), QStringLiteral("Down")} &&
                store.value(QStringLiteral("screenshot_shortcuts/move_cursor_left")).toArray() ==
                    QJsonArray{QStringLiteral("A"), QStringLiteral("Left")},
            "legacy drawing aliases were not moved to screenshot shortcuts");
    require(store.flushNow().success, "failed to flush migrated drawing shortcuts");

    const QJsonObject root = readObject(config);
    const QJsonObject drawing = root.value(QStringLiteral("drawing_shortcuts")).toObject();
    const QJsonObject screenshot = root.value(QStringLiteral("screenshot_shortcuts")).toObject();
    require(drawing.value(QStringLiteral("shape")).toArray() ==
                    QJsonArray{QStringLiteral("Ctrl+K")} &&
                drawing.value(QStringLiteral("arrow")).toArray() ==
                    QJsonArray{QStringLiteral("2")} &&
                drawing.value(QStringLiteral("watermark")).toArray() ==
                    QJsonArray{QStringLiteral("Alt+9")} &&
                screenshot.value(QStringLiteral("move_tool")).toArray() ==
                    QJsonArray{QStringLiteral("M")},
            "drawing shortcut migration was not persisted canonically");
}

void malformedConfigurationIsCopiedAndReplaced() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create malformed directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    writeBytes(config, QByteArrayLiteral("{broken"));
    storage::ConfigurationStore store(config, true, true, 60000);
    require(store.compatibility() == storage::ConfigurationCompatibility::RecoveredDefaults &&
                store.isDirty(),
            "malformed configuration did not load recoverable defaults");
    require(!QDir(temporary.path())
                 .entryInfoList({QStringLiteral("config.json.corrupt.*.json")}, QDir::Files)
                 .isEmpty(),
            "malformed configuration was not copied to a corrupt backup");
    require(store.flushNow().success && readObject(config)
                                                .value(QStringLiteral("storage"))
                                                .toObject()
                                                .value(QStringLiteral("schema_version"))
                                                .toInt() == 1,
            "malformed configuration was not replaced cleanly");

    const QString expiredBackup =
        QDir(temporary.path())
            .filePath(QStringLiteral("config.json.corrupt.20000101T000000000Z.json"));
    writeBytes(expiredBackup, QByteArrayLiteral("old"));
    QFile expiredFile(expiredBackup);
    require(expiredFile.open(QIODevice::ReadWrite), "failed to open aged corrupt backup");
    require(expiredFile.setFileTime(QDateTime::currentDateTimeUtc().addDays(-31),
                                    QFileDevice::FileModificationTime),
            "failed to age corrupt configuration backup");
    expiredFile.close();
    storage::ConfigurationStore cleanup(config, true, true, 60000);
    require(!QFileInfo::exists(expiredBackup) &&
                !QDir(temporary.path())
                     .entryInfoList({QStringLiteral("config.json.corrupt.*.json")}, QDir::Files)
                     .isEmpty(),
            "expired corrupt configuration backups were not cleaned up");

    writeBytes(config, QByteArrayLiteral("{\"storage\": {}}"));
    storage::ConfigurationStore missingVersion(config, true, true, 60000);
    require(missingVersion.compatibility() ==
                storage::ConfigurationCompatibility::RecoveredDefaults,
            "missing schema version was not treated as corrupt");
}

void futureVersionIsReadOnly() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create future-version directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    writeBytes(config, QByteArrayLiteral("{\n"
                                         "  \"storage\": {\"schema_version\": 2},\n"
                                         "  \"interface\": {\"theme_mode\": \"dark\"},\n"
                                         "  \"future\": {\"value\": 42}\n"
                                         "}\n"));
    const QByteArray original = readBytes(config);
    storage::ConfigurationStore store(config, true, true, 60000);
    bool rejected = false;
    QObject::connect(&store, &storage::ConfigurationStore::mutationRejected,
                     [&rejected](const QString&, const QString&) { rejected = true; });
    require(store.compatibility() == storage::ConfigurationCompatibility::FutureVersion &&
                !store.isWritable() &&
                store.value(QStringLiteral("interface/theme_mode")).toString() ==
                    QStringLiteral("dark") &&
                !store.setValue(QStringLiteral("interface/theme_mode"), QStringLiteral("light")) &&
                rejected && store.flushNow().success && readBytes(config) == original,
            "future configuration was not loaded conservatively in read-only mode");

    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create future-version executable directory");
    auto& applicationStorage = initialize(executable, temporary.path());
    const storage::StorageStatus status = applicationStorage.status();
    require(status.effectiveMode == storage::StorageMode::FutureVersionReadOnly &&
                !status.writeAvailable &&
                status.configurationCompatibility ==
                    storage::ConfigurationCompatibility::FutureVersion &&
                !applicationStorage.requestCaptureHistoryClear(),
            "application storage did not propagate future-version read-only mode");
}

void failedWriteCanBeRetried() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create retry directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    require(QDir().mkpath(config), "failed to create blocking config directory");
    storage::ConfigurationStore store(config, true, true, 60000);
    require(!store.flushNow().success && store.isDirty(),
            "failed configuration write did not remain dirty");
    require(QDir(config).removeRecursively(), "failed to remove blocking config directory");
    require(store.flushNow().success && !store.isDirty() && QFileInfo::exists(config),
            "configuration write did not recover on retry");
}

void concurrentFlushKeepsLatestRevision() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create concurrency directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    storage::ConfigurationStore store(config, true, true, 60000);
    require(store.flushNow().success, "failed to write concurrency defaults");

    std::atomic<bool> start{false};
    std::thread first([&store, &start]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (int index = 0; index < 50; ++index) {
            static_cast<void>(
                store.setValue(QStringLiteral("interface/sidebar_collapsed"), index % 2 == 0));
            static_cast<void>(store.flushNow());
        }
    });
    std::thread second([&store, &start]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (int index = 0; index < 50; ++index) {
            static_cast<void>(
                store.setValue(QStringLiteral("interface/theme_mode"),
                               index % 2 == 0 ? QStringLiteral("dark") : QStringLiteral("light")));
            static_cast<void>(store.flushNow());
        }
    });
    start = true;
    first.join();
    second.join();
    require(store.setValues({
                {QStringLiteral("interface/sidebar_collapsed"), true},
                {QStringLiteral("interface/theme_mode"), QStringLiteral("dark")},
            }) &&
                store.flushNow().success,
            "failed to flush final concurrent revision");
    const QJsonObject interface = readObject(config).value(QStringLiteral("interface")).toObject();
    require(interface.value(QStringLiteral("sidebar_collapsed")).toBool() &&
                interface.value(QStringLiteral("theme_mode")).toString() == QStringLiteral("dark"),
            "an older concurrent snapshot overwrote the latest revision");
}

void persistedSelectionCodecIsCanonicalAndStrict() {
    storage::PersistedSelection selection;
    selection.rectangle = QRect(4, 5, 120, 80);
    selection.cornerRadius = 8;
    selection.shadowWidth = 3;
    selection.shadowColor = QColor(10, 20, 30, 120);
    selection.lockAspectRatio = true;
    const QJsonObject encoded = storage::persistedSelectionToJson(selection);
    const auto decoded = storage::normalizePersistedSelection(encoded);
    require(decoded.valid && decoded.value == selection && !decoded.changed,
            "persisted selection codec did not round-trip canonically");

    QJsonObject malformed = encoded;
    malformed.insert(QStringLiteral("corner_radius"), 257);
    require(!storage::normalizePersistedSelection(malformed).valid,
            "persisted selection codec accepted an out-of-range radius");
}

void asynchronousMutationResultsAreObservable() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create async mutation directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create async mutation executable directory");
    auto& applicationStorage = initialize(executable, temporary.path(), 60000);

    storage::CaptureHistoryPolicy policy = applicationStorage.captureHistoryPolicy();
    policy.maxEntries = 2;
    const auto policyResult = applicationStorage.requestCaptureHistoryPolicyAsync(policy);
    require(policyResult.valid() && policyResult.get().success,
            "asynchronous policy mutation did not complete successfully");
    QCoreApplication::processEvents();
    require(!applicationStorage.status().historyPolicyUpdating,
            "policy mutation remained busy after completion");

    const auto clearResult = applicationStorage.requestCaptureHistoryClearAsync();
    require(clearResult.valid() && clearResult.get().success,
            "asynchronous history clear did not complete successfully");
    QCoreApplication::processEvents();
    require(!applicationStorage.status().historyClearing,
            "history clear remained busy after completion");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("storage-tests"));
    if (application.arguments().contains(QStringLiteral("--pin-shortcuts-only"))) {
        newSettingsSchemaDefaultsAndValidationAreComplete();
        pinToScreenShortcutSettingsRoundTrip();
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    markerResolutionAndStatus();
    defaultsAndTypedRoundTrip();
    newSettingsSchemaDefaultsAndValidationAreComplete();
    screenshotUiSchemaRepairsStructuredValues();
    screenshotUiAdaptersRoundTripTypedValues();
    screenshotTranslationSettingsRoundTripSupportedValues();
    newSettingsAdaptersRoundTripAndRejectInvalidValues();
    smartSelectionAccessorAndSignal();
    unknownFieldsArePreserved();
    legacyRecordingKeysMigrateToScreenRecording();
    legacyDrawingAliasesMigrateToScreenshotShortcuts();
    malformedConfigurationIsCopiedAndReplaced();
    futureVersionIsReadOnly();
    failedWriteCanBeRetried();
    concurrentFlushKeepsLatestRevision();
    persistedSelectionCodecIsCanonicalAndStrict();
    asynchronousMutationResultsAreObservable();
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
