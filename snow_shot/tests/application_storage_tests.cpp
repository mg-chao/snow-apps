#include "snow_shot/presentation/globalmousetypes.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/persistedselectioncodec.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/storageusagetracker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfoList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <system_error>
#include <thread>

namespace storage = snow_shot::storage;
namespace shortcuts = snow_shot::shortcuts;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QJsonObject shortcutObject(const QString& portable, std::optional<quint32> macKey = std::nullopt) {
    shortcuts::ShortcutBinding binding{portable};
    if (macKey.has_value()) {
        binding.physicalKeys.insert(shortcuts::ShortcutPlatform::MacOS, *macKey);
    }
    return shortcuts::shortcutBindingToJson(binding);
}

QJsonArray structuredShortcuts(const QJsonArray& portableShortcuts) {
    QJsonArray result;
    for (const QJsonValue& value : portableShortcuts) {
        result.push_back(shortcutObject(value.toString()));
    }
    return result;
}

QStringList portable(const shortcuts::ShortcutBindingList& bindings) {
    return shortcuts::portableTextList(bindings);
}

QString systemSaveDirectory(QStandardPaths::StandardLocation location) {
    const QString directory = QStandardPaths::writableLocation(location);
    return directory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                               : directory;
}

void writeBytes(const QString& path, const QByteArray& bytes) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "failed to create test directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "failed to open test file");
    require(file.write(bytes) == bytes.size(), "failed to write test file");
}

void updateSettingsPersistAndValidate() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "update settings require isolated storage");
    const QString path = temporary.filePath(QStringLiteral("config.json"));
    const QString key = QStringLiteral("updates/mode");
#ifdef Q_OS_MACOS
    const QString defaultMode = QStringLiteral("check");
    const QString nextLaunchMode = QStringLiteral("check");
#else
    const QString defaultMode = QStringLiteral("download");
    const QString nextLaunchMode = QStringLiteral("next_launch");
#endif
    {
        storage::ConfigurationStore store(path, true, true, 60000);
        require(store.value(key) == defaultMode, "existing update default must be preserved");
        require(store.setValue(key, QStringLiteral("next_launch")) &&
                    store.value(key) == nextLaunchMode,
                "next-launch update policy must normalize for the current platform");
        require(!store.setValue(key, QStringLiteral("invalid")) && !store.setValue(key, true) &&
                    store.value(key) == nextLaunchMode,
                "invalid policy writes must preserve the accepted update policy");
        require(store.flushNow().success, "update policy must flush to disk");
    }
    {
        storage::ConfigurationStore reloaded(path, true, true, 60000);
        require(reloaded.value(key) == nextLaunchMode,
                "next-launch update policy must survive configuration reload");
        require(reloaded.setValue(key, storage::ConfigurationSchema::defaultValue(key)) &&
                    reloaded.value(key) == defaultMode,
                "update policy reset must restore its original default");
    }
}

void scrollingIntervalSettingsPersistAndValidate() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "interval settings require an isolated directory");
    auto& appStorage = storage::ApplicationStorage::instance();
    const storage::StorageInitializationOptions options{temporary.filePath(QStringLiteral("bin")),
                                                        temporary.filePath(QStringLiteral("data")),
                                                        60000};
    static_cast<void>(appStorage.initialize(options));
    const storage::ScreenshotSettings settings;
    const QString key = QStringLiteral("screenshot/scrolling_auto_scroll_interval_ms");
    require(settings.scrollingAutoScrollIntervalMs() == 200, "interval must default to 200 ms");
    for (const int value : {128, 1000, 350}) {
        require(settings.setScrollingAutoScrollIntervalMs(value) &&
                    settings.scrollingAutoScrollIntervalMs() == value,
                "valid intervals must round trip through the settings adapter");
    }
    for (const int value : {127, 1001}) {
        require(!settings.setScrollingAutoScrollIntervalMs(value) &&
                    settings.scrollingAutoScrollIntervalMs() == 350,
                "invalid writes must preserve the accepted setting");
    }
    require(appStorage.flushNow().success, "interval must be persisted to disk");
    appStorage.shutdown();
    static_cast<void>(appStorage.initialize(options));
    require(settings.scrollingAutoScrollIntervalMs() == 350,
            "interval must survive application storage restart");
    appStorage.shutdown();
    for (const QJsonValue& value : {QJsonValue(127), QJsonValue(1001), QJsonValue(200.5),
                                    QJsonValue(QStringLiteral("invalid"))}) {
        const QString path = temporary.filePath(QStringLiteral("invalid.json"));
        writeBytes(
            path, QJsonDocument(
                      QJsonObject{{QStringLiteral("screenshot"),
                                   QJsonObject{{QStringLiteral("scrolling_auto_scroll_interval_ms"),
                                                value}}}})
                      .toJson());
        storage::ConfigurationStore store(path, true, true, 60000);
        require(store.value(key).toInt() == 200,
                "invalid stored intervals must fall back to 200 ms");
    }
}

void selectionAspectRatioSettingsValidateAndPreserveLegacyLock() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "aspect ratio settings require an isolated directory");
    const QString ratioKey = QStringLiteral("screenshot_selection/aspect_ratio");
    const QString lockKey = QStringLiteral("screenshot_selection/lock_aspect_ratio");
    const QStringList ratios{QStringLiteral("free"), QStringLiteral("1:1"),  QStringLiteral("3:2"),
                             QStringLiteral("4:3"),  QStringLiteral("16:9"), QStringLiteral("2:3"),
                             QStringLiteral("3:4"),  QStringLiteral("9:16")};
    const auto* entry = storage::ConfigurationSchema::entry(ratioKey);
    require(entry != nullptr && entry->defaultValue == QStringLiteral("free") &&
                entry->allowedStringValues == ratios,
            "aspect ratio schema must preserve the ordered toolbar presets and Free default");
    const QString path = temporary.filePath(QStringLiteral("aspect-ratio.json"));
    storage::ConfigurationStore store(path, true, true, 60000);
    require(store.value(ratioKey) == QStringLiteral("free") && !store.value(lockKey).toBool(),
            "new configurations must default to an unlocked Free aspect ratio");
    for (const QString& ratio : ratios) {
        require(store.setValues({{ratioKey, ratio}, {lockKey, ratio != QStringLiteral("free")}}) &&
                    store.value(ratioKey) == ratio,
                "every toolbar aspect ratio must be accepted by storage");
    }
    const auto revision = store.revision();
    int changes = 0;
    QObject::connect(&store, &storage::ConfigurationStore::valueChanged,
                     [&changes](const QString&, const QJsonValue&) { ++changes; });
    require(store.setValues({{ratioKey, QStringLiteral("9:16")}, {lockKey, true}}) &&
                store.revision() == revision && changes == 0,
            "unchanged aspect ratio preferences must not announce changes or advance revision");
    for (const QJsonValue& invalid : {QJsonValue(QStringLiteral("16:10")), QJsonValue(1),
                                      QJsonValue(true), QJsonValue(QJsonValue::Null)}) {
        require(!store.setValue(ratioKey, invalid) &&
                    store.value(ratioKey) == QStringLiteral("9:16") && store.revision() == revision,
                "invalid aspect ratio writes must preserve the accepted preference");
    }
    require(!store.setValues(
                {{ratioKey, QStringLiteral("1:1")}, {lockKey, QStringLiteral("invalid")}}) &&
                store.value(ratioKey) == QStringLiteral("9:16") && store.value(lockKey).toBool(),
            "a rejected preference transaction must not partially change the aspect ratio");
    require(store.flushNow().success, "aspect ratio preferences must flush to disk");
    storage::ConfigurationStore reloaded(path, true, true, 60000);
    require(reloaded.value(ratioKey) == QStringLiteral("9:16") && reloaded.value(lockKey).toBool(),
            "aspect ratio preferences must survive storage restart");

    for (const QJsonValue& storedRatio :
         {QJsonValue(QJsonValue::Undefined), QJsonValue(QStringLiteral("16:10")), QJsonValue(1)}) {
        QJsonObject selection{{QStringLiteral("lock_aspect_ratio"), true}};
        if (!storedRatio.isUndefined()) {
            selection.insert(QStringLiteral("aspect_ratio"), storedRatio);
        }
        const QString legacyPath = temporary.filePath(QStringLiteral("legacy-aspect-ratio.json"));
        writeBytes(
            legacyPath,
            QJsonDocument(QJsonObject{
                              {QStringLiteral("storage"),
                               QJsonObject{{QStringLiteral("schema_version"),
                                            storage::ConfigurationStore::currentSchemaVersion()}}},
                              {QStringLiteral("screenshot_selection"), selection},
                          })
                .toJson());
        storage::ConfigurationStore legacy(legacyPath, true, true, 60000);
        require(legacy.value(ratioKey) == QStringLiteral("free") && legacy.value(lockKey).toBool(),
                "missing or invalid presets must preserve the legacy custom lock preference");
    }
}

void setLastModified(const QString& path, const QDateTime& when) {
    namespace fs = std::filesystem;
    const auto systemMoment =
        std::chrono::system_clock::time_point{std::chrono::milliseconds(when.toMSecsSinceEpoch())};
    const auto moment =
        fs::file_time_type::clock::now() + std::chrono::duration_cast<fs::file_time_type::duration>(
                                               systemMoment - std::chrono::system_clock::now());
    std::error_code error;
    fs::last_write_time(fs::path(path.toStdWString()), moment, error);
    require(!error, "failed to set test file timestamp");
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
    const QJsonObject tray = root.value(QStringLiteral("tray")).toObject();
    require(root.value(QStringLiteral("storage"))
                        .toObject()
                        .value(QStringLiteral("schema_version"))
                        .toInt() == 3 &&
                root.value(QStringLiteral("screenshot_selection"))
                    .toObject()
                    .value(QStringLiteral("smart_selection"))
                    .toBool() &&
                history.value(QStringLiteral("enabled")).toBool() &&
                history.value(QStringLiteral("retention_days")).toInt() == 7 &&
                history.value(QStringLiteral("max_entries")).toInt() == 100 &&
                history.value(QStringLiteral("max_disk_mib")).toInt() == 1024 &&
                history.value(QStringLiteral("compression_level")).toString() ==
                    QStringLiteral("medium") &&
                screenshotUi.value(QStringLiteral("toolbar_size")).toString() ==
                    QStringLiteral("normal") &&
                screenshotUi.value(QStringLiteral("selection_transition_animation")).toBool() &&
                screenshotUi.value(QStringLiteral("selection_border_color")).toString() ==
                    QStringLiteral("#4096FFFF") &&
                screenshotUi.value(QStringLiteral("selection_mask_color")).toString() ==
                    QStringLiteral("#00000080") &&
                screenshotUi.value(QStringLiteral("shortcut_hint_opacity")).toInt() == 100 &&
                toolbarLayout.size() == 2 &&
                toolbarLayout.value(QStringLiteral("hidden")).toArray().isEmpty() &&
                tray.value(QStringLiteral("enabled")).toBool() &&
                tray.value(QStringLiteral("icon")).toString() == QStringLiteral("default") &&
                tray.value(QStringLiteral("custom_icon")).toString().isEmpty() &&
                !history.contains(QStringLiteral("records")),
            "schema defaults are incomplete");
    require(readBytes(config).endsWith('\n'), "configuration has no final newline");

    require(
        store.setValues({
            {QStringLiteral("interface/theme_mode"), QStringLiteral("DARK")},
            {QStringLiteral("interface/language"), QStringLiteral("zh-CN")},
            {QStringLiteral("capture_history/retention_days"), 30},
            {QStringLiteral("capture_history/max_entries"), 250},
            {QStringLiteral("capture_history/max_disk_mib"), 2048},
            {QStringLiteral("capture_history/compression_level"), QStringLiteral("high")},
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
            reloaded.value(QStringLiteral("capture_history/compression_level")).toString() ==
                QStringLiteral("high") &&
            reloaded.value(QStringLiteral("screenshot/compression_level")).toString() ==
                QStringLiteral("medium") &&
            !reloaded.value(QStringLiteral("screenshot_selection/smart_selection")).toBool() &&
            reloaded.value(QStringLiteral("screenshot_ui/selection_mask_color")).toString() ==
                QStringLiteral("#12AB34CD") &&
            reloaded.value(QStringLiteral("screenshot_ui/shortcut_hint_opacity")).toInt() == 42 &&
            reloaded.value(QStringLiteral("tray/icon")).toString() == QStringLiteral("snow-dark"),
        "typed values did not normalize and round-trip");
}

void settingsSchemaDefaultsAndValidationAreComplete() {
    const auto defaultValue = [](const char* key) {
        return storage::ConfigurationSchema::defaultValue(QString::fromLatin1(key));
    };
    require(
        defaultValue("system/auto_start_at_boot").toBool() &&
            defaultValue("network/proxy").toString() == QStringLiteral("none") &&
            defaultValue("text_recognition/model_type").toString() == QStringLiteral("small") &&
            defaultValue("text_recognition/detector_resize_policy").toString() ==
                QStringLiteral("max") &&
            !defaultValue("text_recognition/resident_process").toBool() &&
            !defaultValue("text_recognition/model_hot_start").toBool() &&
            !defaultValue("global_shortcuts/disable_on_focused_fullscreen_window").toBool() &&
            !defaultValue("extended_features/jump_to_translation_page").toBool() &&
#ifdef Q_OS_MACOS
            defaultValue("global_shortcuts/screenshot").toArray() ==
                QJsonArray{shortcutObject(QStringLiteral("Meta+1"), 18)} &&
            defaultValue("global_shortcuts/screenshot_copy").toArray() ==
                QJsonArray{shortcutObject(QStringLiteral("Meta+2"), 19)} &&
            defaultValue("global_shortcuts/pin_clipboard_content").toArray() ==
                QJsonArray{shortcutObject(QStringLiteral("Meta+3"), 20)} &&
#else
            defaultValue("global_shortcuts/screenshot").toArray() ==
                structuredShortcuts(QJsonArray{QStringLiteral("F1")}) &&
            defaultValue("global_shortcuts/screenshot_copy").toArray() ==
                structuredShortcuts(QJsonArray{QStringLiteral("Ctrl+F1")}) &&
            defaultValue("global_shortcuts/pin_clipboard_content").toArray() ==
                structuredShortcuts(QJsonArray{QStringLiteral("F3")}) &&
#endif
            defaultValue("global_shortcuts/pin_selected_files").toArray().isEmpty() &&
            defaultValue("global_shortcuts/hide_show_all_windows").toArray().isEmpty() &&
            defaultValue("global_shortcuts/open_screen_recording_folder").toArray().isEmpty() &&
            defaultValue("screenshot/auto_execute_after_text_recognition").toString() ==
                QStringLiteral("no_action") &&
            defaultValue("screenshot/double_click_action").toString() == QStringLiteral("copy") &&
            defaultValue("screenshot/middle_mouse_button_action").toString() ==
                QStringLiteral("pin") &&
            defaultValue("screenshot/selection_resize_mode").toString() ==
                QStringLiteral("follow_mouse_movement") &&
            defaultValue("screenshot_shortcuts/quick_save").toArray() ==
                structuredShortcuts(QJsonArray{QStringLiteral("Ctrl+Shift+S")}) &&
            defaultValue("screenshot_shortcuts/save_as_file").toArray() ==
                structuredShortcuts(QJsonArray{QStringLiteral("Ctrl+S")}) &&
            !defaultValue("screenshot/auto_save_after_copy").toBool() &&
            !defaultValue("screenshot/copy_image_file_to_clipboard").toBool() &&
            defaultValue("screenshot/image_save_directory").toString() ==
                systemSaveDirectory(QStandardPaths::PicturesLocation) &&
            defaultValue("screenshot/last_manual_save_directory").toString().isEmpty() &&
            defaultValue("screenshot/image_format").toString() == QStringLiteral("png") &&
            defaultValue("screenshot/compression_level").toString() == QStringLiteral("medium") &&
            defaultValue("screenshot/image_quality").toInt() == 100 &&
            defaultValue("screenshot_ui/area_type_hint_enabled").toBool() &&
            defaultValue("screenshot/manual_save_format_options").toObject().isEmpty() &&
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
            defaultValue("screen_recording/clarity").toString() == QStringLiteral("1080p") &&
            defaultValue("screen_recording/frame_rate").toInt() == 30 &&
            defaultValue("screen_recording/animated_image_clarity").toString() ==
                QStringLiteral("720p") &&
            defaultValue("screen_recording/animated_image_frame_rate").toInt() == 10 &&
            defaultValue("screen_recording/output_format").toString() == QStringLiteral("mp4") &&
            defaultValue("screen_recording/mouse_trail_color").toString() ==
                QStringLiteral("#00000000") &&
            defaultValue("screen_recording/mouse_click_color").toString() ==
                QStringLiteral("#00000000") &&
            defaultValue("screen_recording/show_cursor").toBool() &&
            !defaultValue("screen_recording/show_keyboard").toBool() &&
            storage::ConfigurationSchema::entry(
                QStringLiteral("screen_recording/animated_image_format")) == nullptr &&
            defaultValue("screen_recording/encoder").toString() == QStringLiteral("h264_hw") &&
            defaultValue("screen_recording/video_quality").toInt() == 80 &&
            defaultValue("screen_recording/encoding_preset").toString() ==
                QStringLiteral("veryfast") &&
            defaultValue("screen_recording/capture_toolbar_in_recording").toBool() &&
            storage::ConfigurationSchema::entry(
                QStringLiteral("screen_recording/hide_toolbar_in_recording")) == nullptr &&
            defaultValue("screenshot/capture_ui_in_scrolling_screenshot").toBool() &&
            defaultValue("screen_recording/video_save_directory").toString() ==
                systemSaveDirectory(QStandardPaths::MoviesLocation) &&
            defaultValue("screen_recording/video_filename_format").toString() ==
                QStringLiteral("SnowShot_Video_{YYYY-MM-DD_HH-mm-ss}") &&
            defaultValue("tray/left_click_action").toString() == QStringLiteral("screenshot") &&
            defaultValue("tray/middle_click_action").toString() ==
                QStringLiteral("screenshot_fixed") &&
            defaultValue("tray/menu_options").toArray() ==
                QJsonArray{
                    QStringLiteral("quick.screenshot"), QStringLiteral("quick.screenshot-delay"),
                    QStringLiteral("quick.screenshot-fixed"),
                    QStringLiteral("quick.screenshot-ocr"), QStringLiteral("quick.screenshot-copy"),
                    QStringLiteral("quick.pin-clipboard-content"),
                    QStringLiteral("quick.restore-last-closed-windows"),
                    QStringLiteral("quick.screen-record"),
                    QStringLiteral("quick.toggle-global-hotkeys"),
                    QStringLiteral("tray.window-grouping"), QStringLiteral("tray.show-main-window"),
                    QStringLiteral("tray.exit")},
        "new settings defaults do not match the requested contract");

    const QMap<QString, QJsonArray> drawingShortcutDefaults{
        {QStringLiteral("line"), QJsonArray()},
        {QStringLiteral("spotlight"), QJsonArray()},
        {QStringLiteral("select"), QJsonArray{QStringLiteral("V")}},
        {QStringLiteral("shape"), QJsonArray{QStringLiteral("1")}},
        {QStringLiteral("arrow"), QJsonArray{QStringLiteral("2")}},
        {QStringLiteral("brush"), QJsonArray{QStringLiteral("3"), QStringLiteral("P")}},
        {QStringLiteral("highlight"), QJsonArray{QStringLiteral("4"), QStringLiteral("H")}},
        {QStringLiteral("text"), QJsonArray{QStringLiteral("5"), QStringLiteral("T")}},
        {QStringLiteral("serial_number"), QJsonArray{QStringLiteral("6"), QStringLiteral("N")}},
        {QStringLiteral("filter"), QJsonArray{QStringLiteral("7"), QStringLiteral("F")}},
        {QStringLiteral("eraser"), QJsonArray{QStringLiteral("8"), QStringLiteral("E")}},
        {QStringLiteral("watermark"), QJsonArray{QStringLiteral("9")}},
    };
    for (auto it = drawingShortcutDefaults.cbegin(); it != drawingShortcutDefaults.cend(); ++it) {
        const QString key = QStringLiteral("drawing_shortcuts/") + it.key();
        const auto* entry = storage::ConfigurationSchema::entry(key);
        require(entry != nullptr &&
                    entry->defaultValue.toArray() == structuredShortcuts(it.value()) &&
                    entry->maximumListItems == 2,
                "drawing shortcut defaults and list limits must remain stable");
    }

    const QMap<QString, QJsonArray> screenshotShortcutDefaults{
        {QStringLiteral("move_tool"), QJsonArray{QStringLiteral("M"), QStringLiteral("Ctrl+E")}},
        {QStringLiteral("move_cursor_up"), QJsonArray{QStringLiteral("W"), QStringLiteral("Up")}},
        {QStringLiteral("move_cursor_down"),
         QJsonArray{QStringLiteral("S"), QStringLiteral("Down")}},
        {QStringLiteral("move_cursor_left"),
         QJsonArray{QStringLiteral("A"), QStringLiteral("Left")}},
        {QStringLiteral("move_cursor_right"),
         QJsonArray{QStringLiteral("D"), QStringLiteral("Right")}},
        {QStringLiteral("move_entire_selection"), QJsonArray{QStringLiteral("Space")}},
        {QStringLiteral("keep_selection_width_and_height_consistent"),
         QJsonArray{QStringLiteral("Shift")}},
        {QStringLiteral("selection_aspect_ratio_snap"), QJsonArray{QStringLiteral("Q")}},
        {QStringLiteral("switch_selection_between_window_and_window_sub_element"),
         QJsonArray{QStringLiteral("Tab")}},
        {QStringLiteral("previous_screenshot_history"), QJsonArray{QStringLiteral(",")}},
        {QStringLiteral("next_screenshot_history"), QJsonArray{QStringLiteral(".")}},
        {QStringLiteral("select_previously_selected_area"), QJsonArray{QStringLiteral("R")}},
        {QStringLiteral("recapture"), QJsonArray{QStringLiteral("Alt+R")}},
        {QStringLiteral("toggle_cursor_visibility"), QJsonArray{QStringLiteral("`")}},
        {QStringLiteral("copy_color"), QJsonArray{QStringLiteral("C")}},
        {QStringLiteral("toggle_coordinate_mode"), QJsonArray{QStringLiteral("Shift+P")}},
        {QStringLiteral("toggle_guides"), QJsonArray{QStringLiteral("Alt")}},
        {QStringLiteral("table_recognition"), QJsonArray{QStringLiteral("Ctrl+X")}},
        {QStringLiteral("qr_code_recognition"), QJsonArray{QStringLiteral("Ctrl+Q")}},
        {QStringLiteral("video_recording"), QJsonArray{QStringLiteral("Ctrl+R")}},
        {QStringLiteral("text_recognition"), QJsonArray{QStringLiteral("Ctrl+D")}},
        {QStringLiteral("text_translation"), QJsonArray{QStringLiteral("Ctrl+T")}},
        {QStringLiteral("scrolling_screenshot"), QJsonArray{QStringLiteral("L")}},
        {QStringLiteral("print"), QJsonArray{QStringLiteral("Ctrl+P")}},
        {QStringLiteral("save_as_file"), QJsonArray{QStringLiteral("Ctrl+S")}},
        {QStringLiteral("pin_to_screen"), QJsonArray{QStringLiteral("Ctrl+F")}},
        {QStringLiteral("cancel_screenshot"), QJsonArray{QStringLiteral("Esc")}},
        {QStringLiteral("copy_to_clipboard"), QJsonArray{QStringLiteral("Ctrl+C")}},
        {QStringLiteral("undo"), QJsonArray{QStringLiteral("Ctrl+Z")}},
        {QStringLiteral("redo"), QJsonArray{QStringLiteral("Ctrl+Y")}},
    };
    for (auto it = screenshotShortcutDefaults.cbegin(); it != screenshotShortcutDefaults.cend();
         ++it) {
        const QString key = QStringLiteral("screenshot_shortcuts/") + it.key();
        const auto* entry = storage::ConfigurationSchema::entry(key);
        require(entry != nullptr &&
                    entry->defaultValue.toArray() == structuredShortcuts(it.value()) &&
                    entry->maximumListItems == 2,
                "screenshot shortcut defaults and list limits must remain stable");
    }
    require(storage::ConfigurationSchema::defaultValue(
                QStringLiteral("screenshot_ui/show_guides_by_default")) == QJsonValue(false) &&
                storage::ConfigurationSchema::defaultValue(
                    QStringLiteral("screenshot_ui/cursor_guide_line_color")) ==
                    QJsonValue(QStringLiteral("#000000FF")) &&
                storage::ConfigurationSchema::defaultValue(
                    QStringLiteral("screenshot_ui/selection_center_guide_line_color")) ==
                    QJsonValue(QStringLiteral("#4096FFFF")) &&
                storage::ConfigurationSchema::defaultValue(
                    QStringLiteral("screenshot_ui/monitor_center_guide_line_color")) ==
                    QJsonValue(QStringLiteral("#FF0000FF")),
            "screenshot guide defaults must start hidden with opaque configured colors");

    const QMap<QString, QJsonArray> pinToScreenShortcutDefaults{
        {QStringLiteral("copy_to_clipboard"), QJsonArray{QStringLiteral("Ctrl+C")}},
        {QStringLiteral("copy_original_content"), QJsonArray{QStringLiteral("Ctrl+Shift+C")}},
        {QStringLiteral("print"), QJsonArray{QStringLiteral("Ctrl+P")}},
        {QStringLiteral("save_as_file"), QJsonArray{QStringLiteral("Ctrl+S")}},
        {QStringLiteral("show_text_recognition_results"), QJsonArray{QStringLiteral("Ctrl+D")}},
        {QStringLiteral("drawing_mode"), QJsonArray{QStringLiteral("Space")}},
        {QStringLiteral("resize_window"), QJsonArray{QStringLiteral("M")}},
        {QStringLiteral("thumbnail_mode"), QJsonArray{QStringLiteral("R")}},
        {QStringLiteral("hide_to_top"), QJsonArray{QStringLiteral("H")}},
        {QStringLiteral("toggle_click_through"), QJsonArray{QStringLiteral("Ctrl+M")}},
        {QStringLiteral("always_on_top"), QJsonArray{QStringLiteral("T")}},
        {QStringLiteral("show_border"), QJsonArray{QStringLiteral("B")}},
        {QStringLiteral("show_shadow"), QJsonArray{QStringLiteral("Y")}},
        {QStringLiteral("close_window"), QJsonArray{QStringLiteral("Esc")}},
        {QStringLiteral("destroy_window"), QJsonArray{QStringLiteral("Shift+Esc")}},
        {QStringLiteral("move_cursor_up"), QJsonArray{QStringLiteral("W"), QStringLiteral("Up")}},
        {QStringLiteral("move_cursor_down"),
         QJsonArray{QStringLiteral("S"), QStringLiteral("Down")}},
        {QStringLiteral("move_cursor_left"),
         QJsonArray{QStringLiteral("A"), QStringLiteral("Left")}},
        {QStringLiteral("move_cursor_right"),
         QJsonArray{QStringLiteral("D"), QStringLiteral("Right")}},
    };
    for (auto it = pinToScreenShortcutDefaults.cbegin(); it != pinToScreenShortcutDefaults.cend();
         ++it) {
        const QString key = QStringLiteral("pin_to_screen_shortcuts/") + it.key();
        const auto* entry = storage::ConfigurationSchema::entry(key);
        require(entry != nullptr &&
                    entry->defaultValue.toArray() == structuredShortcuts(it.value()) &&
                    entry->maximumListItems == 2,
                "pinned-window shortcut defaults and list limits must remain stable");
    }

    for (const QString& malformed : {QStringLiteral("Ctrl+K, Ctrl+C"), QStringLiteral("Ctrl"),
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
    require(
        !storage::ConfigurationSchema::normalize(
             QStringLiteral("drawing/quick_selection_disabled_tools"), QStringLiteral("free-draw"))
             .valid,
        "drawing-tool lists must reject non-array values");

    const auto migratedTrayOptions = storage::ConfigurationSchema::normalize(
        QStringLiteral("tray/menu_options"),
        QJsonArray{QStringLiteral("quick.screenshot"),
                   QStringLiteral("tray.disable-shortcut-functions"),
                   QStringLiteral("quick.toggle-global-hotkeys"), QStringLiteral("tray.exit")});
    require(
        migratedTrayOptions.valid && migratedTrayOptions.changed &&
            migratedTrayOptions.value.toArray() ==
                QJsonArray{QStringLiteral("quick.screenshot"),
                           QStringLiteral("quick.toggle-global-hotkeys"),
                           QStringLiteral("tray.exit")},
        "legacy tray hotkey commands must rename in place without duplicating the quick action");
    const auto stableTrayOptions = storage::ConfigurationSchema::normalize(
        QStringLiteral("tray/menu_options"),
        storage::ConfigurationSchema::defaultValue(QStringLiteral("tray/menu_options")).toArray());
    require(stableTrayOptions.valid && !stableTrayOptions.changed,
            "current tray menu defaults must normalize without changes");
    const auto restartTrayOption = storage::ConfigurationSchema::normalize(
        QStringLiteral("tray/menu_options"),
        QJsonArray{QStringLiteral("tray.show-main-window"), QStringLiteral("tray.restart-app"),
                   QStringLiteral("tray.exit")});
    require(restartTrayOption.valid && !restartTrayOption.changed &&
                restartTrayOption.value.toArray().contains(QStringLiteral("tray.restart-app")),
            "Restart App must be accepted only when explicitly selected");
    require(!storage::ConfigurationSchema::normalize(QStringLiteral("tray/menu_options"),
                                                     QStringLiteral("quick.screenshot"))
                 .valid,
            "tray menu options must reject non-array values");

    for (const int frameRate : {5, 10, 15, 24, 30, 60, 120, 83}) {
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
    for (const int frameRate : {5, 10, 15, 24}) {
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
        {QStringLiteral("pin_to_screen/middle_mouse_button_action"),
         {QStringLiteral("none"), QStringLiteral("reset_zoom"), QStringLiteral("thumbnail_mode"),
          QStringLiteral("hide_to_top"), QStringLiteral("close")}},
        {QStringLiteral("pin_to_screen/double_click_action"),
         {QStringLiteral("none"), QStringLiteral("thumbnail_mode"), QStringLiteral("hide_to_top"),
          QStringLiteral("close")}},
        {QStringLiteral("text_recognition/fill_style"),
         {QStringLiteral("blur"), QStringLiteral("background_fill")}},
        {QStringLiteral("text_recognition/model_type"),
         {QStringLiteral("extra_small"), QStringLiteral("small"), QStringLiteral("medium"),
          QStringLiteral("small_v5"), QStringLiteral("medium_v5"), QStringLiteral("small_v4"),
          QStringLiteral("medium_v4")}},
        {QStringLiteral("text_recognition/detector_resize_policy"),
         {QStringLiteral("max"), QStringLiteral("min")}},
        {QStringLiteral("screenshot/auto_execute_after_text_recognition"),
         {QStringLiteral("no_action"), QStringLiteral("copy_text"),
          QStringLiteral("copy_text_and_end_screenshot"), QStringLiteral("quick_copy_text"),
          QStringLiteral("quick_copy_text_and_end_screenshot"),
          QStringLiteral("enable_edit_mode")}},
        {QStringLiteral("screenshot/double_click_action"),
         {QStringLiteral("copy"), QStringLiteral("save"), QStringLiteral("quick_save"),
          QStringLiteral("pin"), QStringLiteral("none")}},
        {QStringLiteral("screenshot/middle_mouse_button_action"),
         {QStringLiteral("copy"), QStringLiteral("save"), QStringLiteral("quick_save"),
          QStringLiteral("pin"), QStringLiteral("none")}},
        {QStringLiteral("screenshot/image_format"),
         {QStringLiteral("png"), QStringLiteral("jpeg"), QStringLiteral("bmp"),
          QStringLiteral("webp"), QStringLiteral("jxl"), QStringLiteral("avif"),
          QStringLiteral("pdf")}},
        {QStringLiteral("screenshot/compression_level"),
         {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}},
        {QStringLiteral("capture_history/compression_level"),
         {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}},
        {QStringLiteral("pin_to_screen/mouse_wheel_zoom_mode"),
         {QStringLiteral("mouse_position"), QStringLiteral("top_left"), QStringLiteral("top_right"),
          QStringLiteral("bottom_left"), QStringLiteral("bottom_right"), QStringLiteral("center")}},
        {QStringLiteral("screen_recording/clarity"),
         {QStringLiteral("4k"), QStringLiteral("2k"), QStringLiteral("1080p"),
          QStringLiteral("720p"), QStringLiteral("480p")}},
        {QStringLiteral("screen_recording/animated_image_clarity"),
         {QStringLiteral("1080p"), QStringLiteral("720p"), QStringLiteral("480p")}},
        {QStringLiteral("screen_recording/output_format"),
         {QStringLiteral("mp4"), QStringLiteral("gif"), QStringLiteral("apng"),
          QStringLiteral("webp")}},
        {QStringLiteral("screen_recording/encoder"),
         {QStringLiteral("h264_hw"), QStringLiteral("h264"), QStringLiteral("h265")}},
        {QStringLiteral("screen_recording/encoding_preset"),
         {QStringLiteral("ultrafast"), QStringLiteral("veryfast"), QStringLiteral("medium"),
          QStringLiteral("veryslow"), QStringLiteral("placebo")}},
        {QStringLiteral("tray/left_click_action"),
         {QStringLiteral("screenshot"), QStringLiteral("show_main_window"),
          QStringLiteral("screenshot_copy"), QStringLiteral("screenshot_fixed"),
          QStringLiteral("open_function_settings")}},
        {QStringLiteral("tray/middle_click_action"),
         {QStringLiteral("screenshot"), QStringLiteral("show_main_window"),
          QStringLiteral("screenshot_copy"), QStringLiteral("screenshot_fixed"),
          QStringLiteral("open_function_settings")}},
    };
    for (auto it = allowedStringValues.cbegin(); it != allowedStringValues.cend(); ++it) {
        const auto* entry = storage::ConfigurationSchema::entry(it.key());
        require(entry != nullptr && entry->allowedStringValues == it.value(),
                "select schema options must exactly match the UI contract");
        for (const QString& value : it.value()) {
            require(storage::ConfigurationSchema::normalize(it.key(), value).valid,
                    "every advertised select value must be accepted");
        }
        require(
            !storage::ConfigurationSchema::normalize(it.key(), QStringLiteral("unsupported-value"))
                 .valid,
            "select settings must reject unsupported values");
    }

    const auto repairedManualOptions = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot/manual_save_format_options"),
        QJsonObject{
            {QStringLiteral("png"),
             QJsonObject{{QStringLiteral("quality"), 41},
                         {QStringLiteral("compression_level"), QStringLiteral("medium")},
                         {QStringLiteral("unknown"), true}}},
            {QStringLiteral("jpeg"),
             QJsonObject{{QStringLiteral("quality"), -4},
                         {QStringLiteral("compression_level"), QStringLiteral("high")}}},
            {QStringLiteral("webp"),
             QJsonObject{{QStringLiteral("quality"), 140},
                         {QStringLiteral("compression_level"), QStringLiteral("invalid")}}},
            {QStringLiteral("jxl"), QStringLiteral("malformed")},
            {QStringLiteral("avif"),
             QJsonObject{{QStringLiteral("quality"), 55.5},
                         {QStringLiteral("compression_level"), QStringLiteral("high")}}},
            {QStringLiteral("pdf"), QJsonObject{{QStringLiteral("quality"), 0}}},
            {QStringLiteral("unsupported"), QJsonObject{{QStringLiteral("quality"), 75}}},
        });
    const QJsonObject expectedManualOptions{
        {QStringLiteral("png"),
         QJsonObject{{QStringLiteral("compression_level"), QStringLiteral("medium")}}},
        {QStringLiteral("jpeg"), QJsonObject{{QStringLiteral("quality"), 0}}},
        {QStringLiteral("webp"), QJsonObject{{QStringLiteral("quality"), 100}}},
        {QStringLiteral("avif"),
         QJsonObject{{QStringLiteral("compression_level"), QStringLiteral("high")}}},
        {QStringLiteral("pdf"), QJsonObject{{QStringLiteral("quality"), 0}}},
    };
    require(repairedManualOptions.valid && repairedManualOptions.changed &&
                repairedManualOptions.value.toObject() == expectedManualOptions &&
                !storage::ConfigurationSchema::normalize(
                     QStringLiteral("screenshot/manual_save_format_options"), QJsonArray{})
                     .valid,
            "manual-save format options must clamp supported values and remove malformed fields");
    for (const int quality : {0, 1, 99, 100}) {
        require(storage::ConfigurationSchema::normalize(QStringLiteral("screenshot/image_quality"),
                                                        quality)
                    .valid,
                "image quality boundaries must be accepted");
    }
    for (const int quality : {-1, 101}) {
        require(!storage::ConfigurationSchema::normalize(QStringLiteral("screenshot/image_quality"),
                                                         quality)
                     .valid,
                "out-of-range image quality must be rejected");
    }
}

void globalMouseCombinationSchemaIsStrictAndPersistent() {
    const QStringList keys{
        QStringLiteral("global_mouse/screenshot_copy"),
        QStringLiteral("global_mouse/screenshot_fixed"),
        QStringLiteral("global_mouse/screenshot_ocr"),
        QStringLiteral("global_mouse/screenshot_translation"),
        QStringLiteral("global_mouse/screenshot_save"),
        QStringLiteral("global_mouse/screenshot_quick_save"),
        QStringLiteral("global_mouse/screen_recording"),
    };
    const QStringList activationKeys{snow_shot::presentation::globalMouseActivationKeys().at(0),
                                     snow_shot::presentation::globalMouseActivationKeys().at(1),
                                     snow_shot::presentation::globalMouseActivationKeys().at(2),
                                     QStringLiteral("shift")};
    const QStringList mouseButtons{
        QStringLiteral("left_drag"),          QStringLiteral("right_drag"),
        QStringLiteral("wheel_drag"),         QStringLiteral("side_button_1_drag"),
        QStringLiteral("side_button_2_drag"),
    };
    for (const QString& key : keys) {
        const auto* entry = storage::ConfigurationSchema::entry(key);
        require(entry != nullptr && entry->valueKind == storage::ConfigurationValueKind::Structured,
                "global mouse fields must be structured values");
        const auto normalizedDefault =
            storage::ConfigurationSchema::normalize(key, entry->defaultValue);
        require(normalizedDefault.valid && !normalizedDefault.changed &&
                    normalizedDefault.value == entry->defaultValue,
                "global mouse defaults must already use their canonical persisted representation");
#ifdef Q_OS_MACOS
        require(entry->defaultValue == QJsonObject{},
                "macOS global mouse bindings must be unset by default");
#else
        const QString button =
            key.endsWith(QStringLiteral("screenshot_copy"))    ? QStringLiteral("left_drag")
            : key.endsWith(QStringLiteral("screenshot_fixed")) ? QStringLiteral("wheel_drag")
            : key.endsWith(QStringLiteral("screenshot_ocr"))   ? QStringLiteral("right_drag")
                                                               : QString();
        const QJsonObject expected =
            button.isEmpty()
                ? QJsonObject{}
                : QJsonObject{{QStringLiteral("activation_key"),
                               snow_shot::presentation::globalMouseActivationKeys().at(0)},
                              {QStringLiteral("mouse_button"), button}};
        require(entry->defaultValue == expected,
                "copy, pin, and OCR must default to Windows plus left, middle, and right drag");
#endif
        const auto unset = storage::ConfigurationSchema::normalize(key, QJsonObject());
        require(unset.valid && !unset.changed && unset.value == QJsonObject(),
                "an empty global mouse object must normalize as Unset");
        for (const QString& activationKey : activationKeys) {
            for (const QString& mouseButton : mouseButtons) {
                const QJsonObject combination{
                    {QStringLiteral("activation_key"), activationKey},
                    {QStringLiteral("mouse_button"), mouseButton},
                };
                const auto normalized = storage::ConfigurationSchema::normalize(key, combination);
                require(normalized.valid && !normalized.changed && normalized.value == combination,
                        "every declared global mouse combination must be accepted unchanged");
            }
        }
    }

    const QString key = keys.constFirst();
#ifdef Q_OS_MACOS
    for (const auto* oldName : {"windows", "ctrl", "alt"}) {
        require(
            !storage::ConfigurationSchema::normalize(
                 key, QJsonObject{{QStringLiteral("activation_key"), QString::fromLatin1(oldName)},
                                  {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}})
                 .valid,
            "macOS must reject Windows-oriented modifier names instead of silently remapping");
    }
#endif
    const QJsonObject multi{
        {QStringLiteral("activation_key"),
         QJsonArray{QStringLiteral("shift"),
                    snow_shot::presentation::globalMouseActivationKeys().at(1),
                    snow_shot::presentation::globalMouseActivationKeys().at(1)}},
        {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}};
    const auto normalizedMulti = storage::ConfigurationSchema::normalize(key, multi);
    require(normalizedMulti.valid && normalizedMulti.changed &&
                normalizedMulti.value.toObject().value(QStringLiteral("activation_key")) ==
                    QJsonArray{snow_shot::presentation::globalMouseActivationKeys().at(1),
                               QStringLiteral("shift")},
            "multiple activation keys must normalize as a sorted unique set");
    const QVector<QJsonValue> malformed{
        QStringLiteral("windows+left_drag"),
        QJsonArray{snow_shot::presentation::globalMouseActivationKeys().at(0),
                   QStringLiteral("left_drag")},
        QJsonObject{{QStringLiteral("activation_key"),
                     snow_shot::presentation::globalMouseActivationKeys().at(0)}},
        QJsonObject{{QStringLiteral("mouse_button"), QStringLiteral("left_drag")}},
        QJsonObject{{QStringLiteral("activation_key"), QStringLiteral("meta")},
                    {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}},
        QJsonObject{{QStringLiteral("activation_key"),
                     snow_shot::presentation::globalMouseActivationKeys().at(0)},
                    {QStringLiteral("mouse_button"), QStringLiteral("middle_drag")}},
        QJsonObject{{QStringLiteral("activation_key"),
                     snow_shot::presentation::globalMouseActivationKeys().at(0)},
                    {QStringLiteral("mouse_button"), QStringLiteral("left_drag")},
                    {QStringLiteral("extra"), true}},
        QJsonObject{{QStringLiteral("activation_key"), 1},
                    {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}},
        QJsonObject{{QStringLiteral("activation_key"), QJsonArray{}},
                    {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}},
        QJsonObject{{QStringLiteral("activation_key"),
                     QJsonArray{snow_shot::presentation::globalMouseActivationKeys().at(1), 1}},
                    {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}},
        QJsonObject{{QStringLiteral("activation_key"),
                     QJsonArray{snow_shot::presentation::globalMouseActivationKeys().at(1),
                                QStringLiteral("bad")}},
                    {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}},
    };
    for (const QJsonValue& value : malformed) {
        require(!storage::ConfigurationSchema::normalize(key, value).valid,
                "malformed global mouse values must be rejected");
    }

    QTemporaryDir roundTripDirectory;
    require(roundTripDirectory.isValid(), "failed to create global mouse round-trip directory");
    const QString roundTripPath =
        QDir(roundTripDirectory.path()).filePath(QStringLiteral("config.json"));
    const QJsonObject savedCombination{
        {QStringLiteral("activation_key"),
         QJsonArray{snow_shot::presentation::globalMouseActivationKeys().at(1),
                    QStringLiteral("shift")}},
        {QStringLiteral("mouse_button"), QStringLiteral("side_button_2_drag")},
    };
    {
        storage::ConfigurationStore store(roundTripPath, true, true, 60000);
        require(store.flushNow().success, "persist initial global mouse defaults");
        storage::ConfigurationStore defaultsReloaded(roundTripPath, true, true, 60000);
        require(defaultsReloaded.snapshot() == store.snapshot(),
                "materialized defaults must retain the same values after their first reload");
        require(store.value(key) == storage::ConfigurationSchema::defaultValue(key) &&
                    store.setValue(key, savedCombination) &&
                    !store.setValue(key, malformed.constFirst()) &&
                    store.value(key) == savedCombination && store.flushNow().success,
                "global mouse persistence must retain valid values after a rejected mutation");
    }
    storage::ConfigurationStore reloaded(roundTripPath, true, true, 60000);
    require(reloaded.value(key) == savedCombination,
            "a valid global mouse combination must round-trip through storage");
    require(reloaded.setValue(key, QJsonObject{}) && reloaded.flushNow().success,
            "a default binding must be explicitly clearable");
    storage::ConfigurationStore cleared(roundTripPath, true, true, 60000);
    require(cleared.value(key) == QJsonObject{},
            "an explicitly unset binding must remain unset after reload");

    QTemporaryDir repairDirectory;
    require(repairDirectory.isValid(), "failed to create global mouse repair directory");
    const QString repairPath = QDir(repairDirectory.path()).filePath(QStringLiteral("config.json"));
    QJsonObject malformedDocument = storage::ConfigurationSchema::completeDefaultDocument();
    QJsonObject globalMouse = malformedDocument.value(QStringLiteral("global_mouse")).toObject();
    globalMouse.insert(QStringLiteral("screenshot_copy"), malformed.constFirst());
    malformedDocument.insert(QStringLiteral("global_mouse"), globalMouse);
    writeBytes(repairPath, QJsonDocument(malformedDocument).toJson(QJsonDocument::Indented));
    storage::ConfigurationStore repaired(repairPath, true, true, 60000);
    require(repaired.value(key) == storage::ConfigurationSchema::defaultValue(key) &&
                repaired.isDirty(),
            "malformed persisted global mouse values must be restored to their default");
}

void screenshotUiSchemaRepairsStructuredValues() {
    require(!storage::ConfigurationSchema::normalize(
                 QStringLiteral("screenshot_toolbar/last_drawing_tool"), QStringLiteral("undo"))
                 .valid,
            "history actions must not become remembered drawing tools");
    const QJsonObject defaultActionLayout =
        storage::ConfigurationSchema::defaultValue(
            QStringLiteral("screenshot_toolbar/action_tools_layout"))
            .toObject();
    require(
        defaultActionLayout ==
            QJsonObject{
                {QStringLiteral("positions"),
                 QJsonArray{
                     QJsonArray{
                         QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown"),
                         QStringLiteral("latex-recognition"), QStringLiteral("barcode-recognition"),
                         QStringLiteral("table-recognition")},
                     QJsonArray{QStringLiteral("record-screen")},
                     QJsonArray{QStringLiteral("pin-to-screen")},
                     QJsonArray{QStringLiteral("text-recognition")},
                     QJsonArray{QStringLiteral("text-translation")},
                     QJsonArray{QStringLiteral("scrolling-screenshot")},
                     QJsonArray{QStringLiteral("upload-to-cloud"), QStringLiteral("print"),
                                QStringLiteral("quick-save"), QStringLiteral("save-as-file")},
                     QJsonArray{QStringLiteral("separator")},
                     QJsonArray{QStringLiteral("cancel")},
                     QJsonArray{QStringLiteral("copy")},
                 }},
                {QStringLiteral("hidden"), QJsonArray{}},
            },
        "default action toolbar groups conversions with barcode and table recognition");

    const auto validColor = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_ui/cursor_guide_line_color"), QStringLiteral("#abcdef80"));
    require(validColor.valid && validColor.changed &&
                validColor.value.toString() == QStringLiteral("#ABCDEF80"),
            "RGBA colors were not normalized canonically");
    const auto savedTransparentColor = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_ui/cursor_guide_line_color"), QStringLiteral("#00000000"));
    require(savedTransparentColor.valid && !savedTransparentColor.changed &&
                savedTransparentColor.value.toString() == QStringLiteral("#00000000"),
            "existing transparent guide colors must remain valid after default changes");
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
             QJsonArray{QStringLiteral("unknown-highlight"), QStringLiteral("unknown-pen")},
         }},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("free-draw"),
                    QStringLiteral("unknown-highlight"), QStringLiteral("arrow")}},
    };
    const auto normalized = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_toolbar/layout"), malformedLayout);
    const QJsonObject layout = normalized.value.toObject();
    const QJsonArray positions = layout.value(QStringLiteral("positions")).toArray();
    require(normalized.valid && normalized.changed && layout.size() == 2 &&
                positions ==
                    QJsonArray{
                        QJsonArray{QStringLiteral("select")},
                        QJsonArray{QStringLiteral("select-separator")},
                        QJsonArray{QStringLiteral("watermark"), QStringLiteral("shape")},
                        QJsonArray{QStringLiteral("line")},
                        QJsonArray{QStringLiteral("spotlight"), QStringLiteral("highlighter")},
                        QJsonArray{QStringLiteral("text")},
                        QJsonArray{QStringLiteral("serial-number")},
                        QJsonArray{QStringLiteral("filter")},
                        QJsonArray{QStringLiteral("eraser")},
                        QJsonArray{QStringLiteral("separator")},
                        QJsonArray{QStringLiteral("undo")},
                        QJsonArray{QStringLiteral("redo")},
                    } &&
                layout.value(QStringLiteral("hidden")).toArray() ==
                    QJsonArray{QStringLiteral("arrow"), QStringLiteral("free-draw")},
            "toolbar layout normalization did not preserve hidden nested membership");

    const QJsonObject invalidSeparatorLayout{
        {QStringLiteral("positions"),
         QJsonArray{QJsonArray{QStringLiteral("shape"), QStringLiteral("separator"),
                               QStringLiteral("undo"), QStringLiteral("redo")},
                    QJsonArray{}}},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("select"), QStringLiteral("select-separator")}}};
    const auto separated = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_toolbar/layout"), invalidSeparatorLayout);
    const QJsonArray separatedPositions =
        separated.value.toObject().value(QStringLiteral("positions")).toArray();
    require(separated.valid && separated.changed && separatedPositions.size() >= 3 &&
                separatedPositions.at(0).toArray() == QJsonArray{QStringLiteral("shape")} &&
                separatedPositions.at(1).toArray() == QJsonArray{QStringLiteral("separator")} &&
                separatedPositions.at(2).toArray() ==
                    QJsonArray{QStringLiteral("undo"), QStringLiteral("redo")},
            "separator must normalize into its own drawing toolbar position");
    QJsonObject selectSeparatorLayout = invalidSeparatorLayout;
    selectSeparatorLayout.insert(
        QStringLiteral("positions"),
        QJsonArray{QJsonArray{QStringLiteral("shape"), QStringLiteral("select-separator"),
                              QStringLiteral("select"), QStringLiteral("separator"),
                              QStringLiteral("undo")},
                   QJsonArray{}});
    selectSeparatorLayout.insert(QStringLiteral("hidden"), QJsonArray{});
    const auto selectSeparated = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_toolbar/layout"), selectSeparatorLayout);
    const auto selectPositions =
        selectSeparated.value.toObject().value(QStringLiteral("positions")).toArray();
    require(selectSeparated.valid && selectPositions.at(0) == QJsonArray{QStringLiteral("shape")} &&
                selectPositions.at(1) == QJsonArray{QStringLiteral("select-separator")} &&
                selectPositions.at(2) == QJsonArray{QStringLiteral("select")} &&
                selectPositions.at(3) == QJsonArray{QStringLiteral("separator")} &&
                selectPositions.at(4) == QJsonArray{QStringLiteral("undo")} &&
                !storage::ConfigurationSchema::normalize(
                     QStringLiteral("screenshot_toolbar/layout"), selectSeparated.value)
                     .changed,
            "both separators must split stacks without moving Select and normalize idempotently");
    const QJsonObject hiddenSeparatorLayout{
        {QStringLiteral("positions"),
         QJsonArray{QJsonArray{QStringLiteral("shape")}, QJsonArray{}}},
        {QStringLiteral("hidden"), QJsonArray{QStringLiteral("separator")}}};
    const auto hiddenSeparator = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_toolbar/layout"), hiddenSeparatorLayout);
    require(hiddenSeparator.valid &&
                hiddenSeparator.value.toObject().value(QStringLiteral("hidden")).toArray() ==
                    QJsonArray{QStringLiteral("separator")} &&
                !hiddenSeparator.value.toObject()
                     .value(QStringLiteral("positions"))
                     .toArray()
                     .contains(QJsonArray{QStringLiteral("separator")}),
            "a hidden separator must stay hidden during legacy layout upgrade");

    const QJsonObject malformedActionLayout{
        {QStringLiteral("positions"),
         QJsonArray{
             QJsonArray{QStringLiteral("save-as-file"), QStringLiteral("table-recognition"),
                        QStringLiteral("save-as-file"), QStringLiteral("unknown")},
             QStringLiteral("not-a-position"),
             QJsonArray{QStringLiteral("record-screen"), QStringLiteral("table-recognition")},
         }},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
                    QStringLiteral("text-recognition"), QStringLiteral("barcode-recognition"),
                    QStringLiteral("unknown")}},
    };
    const auto normalizedActions = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_toolbar/action_tools_layout"), malformedActionLayout);
    const QJsonObject actionLayout = normalizedActions.value.toObject();
    require(
        normalizedActions.valid && normalizedActions.changed && actionLayout.size() == 2 &&
            actionLayout.value(QStringLiteral("positions")).toArray() ==
                QJsonArray{
                    QJsonArray{QStringLiteral("upload-to-cloud"), QStringLiteral("quick-save"),
                               QStringLiteral("save-as-file"), QStringLiteral("table-recognition"),
                               QStringLiteral("convert-to-markdown"),
                               QStringLiteral("latex-recognition"),
                               QStringLiteral("convert-to-html")},
                    QJsonArray{QStringLiteral("record-screen")},
                    QJsonArray{QStringLiteral("pin-to-screen")},
                    QJsonArray{QStringLiteral("text-translation")},
                    QJsonArray{QStringLiteral("scrolling-screenshot")},
                    QJsonArray{QStringLiteral("print")},
                    QJsonArray{QStringLiteral("separator")},
                    QJsonArray{QStringLiteral("cancel")},
                    QJsonArray{QStringLiteral("copy")},
                } &&
            actionLayout.value(QStringLiteral("hidden")).toArray() ==
                QJsonArray{QStringLiteral("barcode-recognition"),
                           QStringLiteral("text-recognition")},
        "action toolbar normalization must drop invalid entries, prefer visible membership, and "
        "append missing tools in default positions");

    const QJsonObject allHiddenActionLayout{
        {QStringLiteral("positions"), QJsonArray{}},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition"),
                    QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html"),
                    QStringLiteral("latex-recognition"), QStringLiteral("record-screen"),
                    QStringLiteral("pin-to-screen"), QStringLiteral("text-recognition"),
                    QStringLiteral("text-translation"), QStringLiteral("scrolling-screenshot"),
                    QStringLiteral("upload-to-cloud"), QStringLiteral("print"),
                    QStringLiteral("quick-save"), QStringLiteral("save-as-file"),
                    QStringLiteral("separator"), QStringLiteral("cancel"), QStringLiteral("copy")}},
    };
    const auto normalizedAllHidden = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot_toolbar/action_tools_layout"), allHiddenActionLayout);
    require(normalizedAllHidden.valid && !normalizedAllHidden.changed &&
                normalizedAllHidden.value.toObject() == allHiddenActionLayout,
            "an all-hidden action toolbar layout must remain valid without restoring tools");

    const QString actionKey = QStringLiteral("screenshot_toolbar/action_tools_layout");
    const auto defaultActions = storage::ConfigurationSchema::defaultValue(actionKey).toObject();
    auto legacyPositions = defaultActions.value(QStringLiteral("positions")).toArray();
    for (int index = 0; index < 3; ++index) {
        legacyPositions.removeLast();
    }
    QJsonObject legacyActions{{QStringLiteral("positions"), legacyPositions},
                              {QStringLiteral("hidden"), QJsonArray{}}};
    require(storage::ConfigurationSchema::normalize(actionKey, legacyActions).value ==
                defaultActions,
            "legacy screenshot layouts must append the formerly fixed result controls");
    legacyPositions[0] =
        QJsonArray{QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
                   QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")};
    legacyActions.insert(QStringLiteral("positions"), legacyPositions);
    const auto upgraded = storage::ConfigurationSchema::normalize(actionKey, legacyActions);
    require(upgraded.value == defaultActions &&
                !storage::ConfigurationSchema::normalize(actionKey, upgraded.value).changed,
            "historical recognition defaults must migrate without duplicating result controls");

    QJsonArray hiddenResultPeers;
    for (const QJsonValue& id : allHiddenActionLayout.value(QStringLiteral("hidden")).toArray()) {
        if (id != QJsonValue(QStringLiteral("separator")) &&
            id != QJsonValue(QStringLiteral("cancel")) &&
            id != QJsonValue(QStringLiteral("copy"))) {
            hiddenResultPeers.append(id);
        }
    }
    const QJsonObject stackedResults{
        {QStringLiteral("positions"),
         QJsonArray{QJsonValue(QJsonArray{QStringLiteral("copy"), QStringLiteral("separator"),
                                          QStringLiteral("cancel")})}},
        {QStringLiteral("hidden"), hiddenResultPeers}};
    const auto separatedResults =
        storage::ConfigurationSchema::normalize(actionKey, stackedResults);
    require(separatedResults.valid &&
                separatedResults.value.toObject().value(QStringLiteral("positions")).toArray() ==
                    QJsonArray{QJsonArray{QStringLiteral("copy")},
                               QJsonArray{QStringLiteral("separator")},
                               QJsonArray{QStringLiteral("cancel")}} &&
                !storage::ConfigurationSchema::normalize(actionKey, separatedResults.value).changed,
            "result controls must persist custom order with separators in their own positions");
}

void screenshotUiAdaptersRoundTripTypedValues() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create screenshot UI adapter directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create screenshot UI executable directory");
    static_cast<void>(initialize(executable, temporary.path()));

    const storage::ScreenshotUiSettings screenshot;
    require(screenshot.screenshotAreaTypeHintEnabled() &&
                screenshot.setScreenshotAreaTypeHintEnabled(false) &&
                !screenshot.screenshotAreaTypeHintEnabled(),
            "screenshot area type hint defaults on and its adapter accepts the switch value");
    require(!screenshot.showGuidesByDefault() &&
                screenshot.cursorGuideLineColor() == QColor(0, 0, 0) &&
                screenshot.selectionCenterGuideLineColor() == QColor(0x40, 0x96, 0xff) &&
                screenshot.monitorCenterGuideLineColor() == QColor(255, 0, 0) &&
                screenshot.setShowGuidesByDefault(true) && screenshot.showGuidesByDefault() &&
                screenshot.setShowGuidesByDefault(false) && !screenshot.showGuidesByDefault(),
            "guide visibility and opaque color defaults must round-trip through screenshot UI "
            "settings");
    require(screenshot.setSelectionCenterGuideLineColor(QColor(14, 25, 36, 47)) &&
                screenshot.selectionCenterGuideLineColor() == QColor(14, 25, 36, 47),
            "selection center guide color must preserve its RGBA value");
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
    layout.hidden = {QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("free-draw"),
                     QStringLiteral("pen-highlight"), QStringLiteral("arrow")};
    const storage::ScreenshotToolbarSettings toolbar;
    const QVector<QStringList> expectedPositions{
        {QStringLiteral("select")},
        {QStringLiteral("select-separator")},
        {QStringLiteral("watermark"), QStringLiteral("shape")},
        {QStringLiteral("line")},
        {QStringLiteral("spotlight"), QStringLiteral("highlighter")},
        {QStringLiteral("text")},
        {QStringLiteral("serial-number")},
        {QStringLiteral("filter")},
        {QStringLiteral("eraser")},
        {QStringLiteral("separator")},
        {QStringLiteral("undo")},
        {QStringLiteral("redo")},
    };
    const storage::ScreenshotToolbarLayout expectedLayout{
        expectedPositions,
        {QStringLiteral("arrow"), QStringLiteral("free-draw")},
    };
    require(toolbar.setLayout(storage::ScreenshotToolbarLayoutKind::DrawingTools, layout) &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::DrawingTools) ==
                    expectedLayout,
            "typed toolbar layout did not preserve normalized visible and hidden entries");

    const storage::ScreenshotToolbarLayout actionLayout{
        {{QStringLiteral("upload-to-cloud"), QStringLiteral("quick-save"),
          QStringLiteral("save-as-file"), QStringLiteral("record-screen")},
         {QStringLiteral("table-recognition")}},
        {QStringLiteral("barcode-recognition"), QStringLiteral("pin-to-screen"),
         QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html"),
         QStringLiteral("latex-recognition"), QStringLiteral("text-recognition"),
         QStringLiteral("text-translation"), QStringLiteral("scrolling-screenshot"),
         QStringLiteral("print"), QStringLiteral("separator"), QStringLiteral("cancel"),
         QStringLiteral("copy")},
    };
    require(toolbar.setLayout(storage::ScreenshotToolbarLayoutKind::ActionTools, actionLayout) &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::ActionTools) == actionLayout &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::DrawingTools) ==
                    expectedLayout,
            "drawing and action toolbar layouts must round-trip independently");
    const auto pinnedKind = storage::ScreenshotToolbarLayoutKind::PinnedActionTools;
    const auto pinnedDefault = toolbar.layout(pinnedKind);
    require(pinnedDefault.positions.size() == 8 && pinnedDefault.hidden.isEmpty() &&
                pinnedDefault.positions.first().last() == QStringLiteral("table-recognition") &&
                pinnedDefault.positions.last() == QStringList{QStringLiteral("confirm")},
            "pinned defaults must expose configurable section dividers and Confirm Edit");
    // This valid layout resembles a historical screenshot default; pinned layouts must not migrate.
    const storage::ScreenshotToolbarLayout pinnedLayout{
        {{QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")},
         {QStringLiteral("convert-to-markdown"), QStringLiteral("latex-recognition")},
         {QStringLiteral("convert-to-html")},
         {QStringLiteral("text-recognition")},
         {QStringLiteral("text-translation")}},
        {}};
    auto upgradedPinned = pinnedLayout;
    upgradedPinned.positions.append({QStringLiteral("separator")});
    upgradedPinned.positions.append({QStringLiteral("upload-to-cloud"), QStringLiteral("print"),
                                     QStringLiteral("quick-save"), QStringLiteral("save-as-file")});
    upgradedPinned.positions.append({QStringLiteral("copy")});
    upgradedPinned.positions.append({QStringLiteral("confirm-separator")});
    upgradedPinned.positions.append({QStringLiteral("confirm")});
    require(toolbar.setLayout(pinnedKind, pinnedLayout) &&
                toolbar.layout(pinnedKind) == upgradedPinned,
            "pinned layouts must not inherit screenshot conversion migrations");
    auto formerBoundaryLayout = upgradedPinned;
    formerBoundaryLayout.positions.prepend({QStringLiteral("drawing-separator")});
    require(toolbar.setLayout(pinnedKind, formerBoundaryLayout) &&
                toolbar.layout(pinnedKind) == upgradedPinned,
            "saved operation layouts must discard the former leading section boundary");
    formerBoundaryLayout = upgradedPinned;
    formerBoundaryLayout.hidden.append(QStringLiteral("drawing-separator"));
    require(toolbar.setLayout(pinnedKind, formerBoundaryLayout) &&
                toolbar.layout(pinnedKind) == upgradedPinned,
            "the removed section boundary must not remain among hidden operation tools");
    auto malformedPinned = pinnedLayout;
    malformedPinned.positions.prepend({QStringLiteral("record-screen"), QStringLiteral("unknown")});
    malformedPinned.positions.last().append(QStringLiteral("table-recognition"));
    malformedPinned.hidden = {QStringLiteral("text-recognition"), QStringLiteral("unknown")};
    require(
        toolbar.setLayout(pinnedKind, malformedPinned) &&
            toolbar.layout(pinnedKind) == upgradedPinned,
        "pinned normalization must remove unknown IDs and duplicates, preferring visible tools");
    storage::ScreenshotToolbarLayout hiddenPinned;
    for (const auto& position : upgradedPinned.positions)
        hiddenPinned.hidden.append(position);
    require(toolbar.setLayout(pinnedKind, hiddenPinned) &&
                toolbar.layout(pinnedKind) == hiddenPinned &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::ActionTools) == actionLayout,
            "all-hidden pinned layouts must remain empty and independent of screenshot layouts");
    require(toolbar.setLayout(pinnedKind, {}) && toolbar.layout(pinnedKind) == pinnedDefault,
            "restoring pinned defaults must recover the original arrangement");
}

void screenshotTranslationSettingsRoundTripSupportedValues() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create translation settings directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable),
            "failed to create translation settings executable directory");
    static_cast<void>(initialize(executable, temporary.path()));

    const storage::ScreenshotTranslationSettings translation;
    const storage::ScreenshotImageConversionSettings conversion;
    require(conversion.visionModel().isEmpty(), "vision model defaults to catalog selection");
    require(conversion.setVisionModel(QStringLiteral("vision-model")),
            "save the shared vision model");
    require(translation.layoutProcessing() == QStringLiteral("smart_merge"),
            "layout processing should default to Smart Merge for existing configurations");
    require(translation.originalImageTranslationEnabled(),
            "original image translation should default to enabled for existing configurations");
    require(translation.configuration() ==
                storage::ScreenshotTranslationConfiguration{QStringLiteral("auto"), {}, {}},
            "translation settings should default to Auto source and runtime-derived target/model");
    const storage::ScreenshotTranslationConfiguration selected{
        QStringLiteral("ja"), QStringLiteral("zh-Hant"), QStringLiteral("model-a"),
        QStringLiteral("original")};
    require(translation.setConfiguration(selected) && translation.configuration() == selected,
            "translation language and model selections should persist together");
    require(translation.setOriginalImageTranslationEnabled(false) &&
                !translation.originalImageTranslationEnabled() &&
                translation.configuration() == selected,
            "the display toggle must persist independently of language and model settings");
    require(storage::ApplicationStorage::instance().flushNow().success,
            "flush translation configuration before restarting storage");
    static_cast<void>(initialize(executable, temporary.path()));
    require(!translation.originalImageTranslationEnabled() &&
                translation.configuration() == selected,
            "an explicitly disabled display toggle must survive restart");
    require(conversion.visionModel() == QStringLiteral("vision-model"),
            "vision model survives storage restart independently of translation settings");

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

void recordingActionToolbarLayoutRepairsAndPersistsIndependently() {
    const QString key = QStringLiteral("screen_recording/action_tools_layout");
    const QStringList ids{
        QStringLiteral("start-stop"),  QStringLiteral("pause-resume"), QStringLiteral("duration"),
        QStringLiteral("microphone"),  QStringLiteral("system-audio"), QStringLiteral("separator"),
        QStringLiteral("open-folder"), QStringLiteral("close"),        QStringLiteral("trim"),
        QStringLiteral("save"),        QStringLiteral("copy"),
    };
    QJsonArray defaultPositions;
    storage::ScreenshotToolbarLayout defaultLayout;
    for (const QString& id : ids) {
        defaultPositions.append(QJsonArray{id});
        defaultLayout.positions.append({id});
    }
    const QJsonObject defaults{{QStringLiteral("positions"), defaultPositions},
                               {QStringLiteral("hidden"), QJsonArray{}}};
    require(storage::ConfigurationSchema::defaultValue(key) == defaults &&
                !storage::ConfigurationSchema::normalize(key, defaults).changed &&
                !storage::ConfigurationSchema::normalize(key, QJsonArray{}).valid &&
                !storage::ConfigurationSchema::normalize(key, QJsonObject{}).valid,
            "recording toolbar defaults must retain every current action in its original order");

    const QJsonObject malformed{
        {QStringLiteral("positions"),
         QJsonArray{QJsonArray{QStringLiteral("copy"), QStringLiteral("duration"),
                               QStringLiteral("pause-resume"), QStringLiteral("separator"),
                               QStringLiteral("start-stop"), QStringLiteral("copy"),
                               QStringLiteral("unknown")},
                    QStringLiteral("invalid-position"), QJsonArray{}}},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("copy"), QStringLiteral("microphone"),
                    QStringLiteral("open-folder"), QStringLiteral("microphone"),
                    QStringLiteral("unknown")}},
    };
    const QJsonObject repaired{
        {QStringLiteral("positions"),
         QJsonArray{QJsonArray{QStringLiteral("copy")}, QJsonArray{QStringLiteral("duration")},
                    QJsonArray{QStringLiteral("pause-resume")},
                    QJsonArray{QStringLiteral("separator")},
                    QJsonArray{QStringLiteral("start-stop")},
                    QJsonArray{QStringLiteral("system-audio")}, QJsonArray{QStringLiteral("close")},
                    QJsonArray{QStringLiteral("trim")}, QJsonArray{QStringLiteral("save")}}},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("microphone"), QStringLiteral("open-folder")}},
    };
    const auto normalized = storage::ConfigurationSchema::normalize(key, malformed);
    require(normalized.valid && normalized.changed && normalized.value == repaired &&
                !storage::ConfigurationSchema::normalize(key, normalized.value).changed,
            "recording normalization must keep duration and separator standalone, remove unknown "
            "and duplicate tools, and preserve hidden membership without screenshot migrations");

    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create recording toolbar persistence directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create recording toolbar executable directory");
    auto& applicationStorage = initialize(executable, temporary.path());
    const storage::ScreenshotToolbarSettings toolbar;
    const auto kind = storage::ScreenshotToolbarLayoutKind::RecordingActionTools;
    const auto drawing = toolbar.layout(storage::ScreenshotToolbarLayoutKind::DrawingTools);
    const auto screenshot = toolbar.layout(storage::ScreenshotToolbarLayoutKind::ActionTools);
    const auto pinned = toolbar.layout(storage::ScreenshotToolbarLayoutKind::PinnedActionTools);
    require(toolbar.layout(kind) == defaultLayout,
            "recording adapter must expose the schema's complete default arrangement");

    const storage::ScreenshotToolbarLayout custom{
        {{QStringLiteral("start-stop"), QStringLiteral("save")},
         {QStringLiteral("duration")},
         {QStringLiteral("pause-resume"), QStringLiteral("system-audio")},
         {QStringLiteral("separator")},
         {QStringLiteral("trim"), QStringLiteral("copy")}},
        {QStringLiteral("microphone"), QStringLiteral("open-folder"), QStringLiteral("close")},
    };
    int notifications = 0;
    const auto connection = QObject::connect(
        &applicationStorage.configuration(), &storage::ConfigurationStore::valueChanged,
        [&key, &notifications](const QString& changedKey, const QJsonValue&) {
            if (changedKey == key) {
                ++notifications;
            }
        });
    require(toolbar.setLayout(kind, custom) && toolbar.layout(kind) == custom &&
                notifications == 1 &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::DrawingTools) == drawing &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::ActionTools) == screenshot &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::PinnedActionTools) == pinned,
            "recording toolbar writes must notify live consumers and preserve other layouts");
    QObject::disconnect(connection);
    require(applicationStorage.flushNow().success,
            "flush custom recording toolbar before restarting storage");
    static_cast<void>(initialize(executable, temporary.path()));
    require(toolbar.layout(kind) == custom &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::DrawingTools) == drawing &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::ActionTools) == screenshot &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::PinnedActionTools) == pinned,
            "recording stacks and hidden tools must survive a storage restart independently");

    const storage::ScreenshotToolbarLayout allHidden{{}, ids};
    require(toolbar.setLayout(kind, allHidden) && toolbar.layout(kind) == allHidden &&
                applicationStorage.flushNow().success,
            "an all-hidden recording toolbar must not restore any visible tools");
    static_cast<void>(initialize(executable, temporary.path()));
    require(toolbar.layout(kind) == allHidden && toolbar.setLayout(kind, {}) &&
                toolbar.layout(kind) == defaultLayout &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::DrawingTools) == drawing &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::ActionTools) == screenshot &&
                toolbar.layout(storage::ScreenshotToolbarLayoutKind::PinnedActionTools) == pinned,
            "all-hidden recording layouts must survive restart and restore defaults independently");
}

void verifyPinToScreenShortcutSettings() {
    const storage::PinToScreenShortcutSettings shortcutSettings;
    const shortcuts::ShortcutBindingMap defaults = shortcutSettings.allShortcuts();
    require(
        defaults.size() == 30 &&
            portable(defaults.value(QStringLiteral("copy_to_clipboard"))) ==
                QStringList{QStringLiteral("Ctrl+C")} &&
            portable(defaults.value(QStringLiteral("copy_original_content"))) ==
                QStringList{QStringLiteral("Ctrl+Shift+C")} &&
            portable(defaults.value(QStringLiteral("print"))) ==
                QStringList{QStringLiteral("Ctrl+P")} &&
            portable(defaults.value(QStringLiteral("save_as_file"))) ==
                QStringList{QStringLiteral("Ctrl+S")} &&
            portable(defaults.value(QStringLiteral("upload_to_cloud"))) ==
                QStringList{QStringLiteral("Ctrl+U")} &&
            portable(defaults.value(QStringLiteral("show_text_recognition_results"))) ==
                QStringList{QStringLiteral("Ctrl+D")} &&
            portable(defaults.value(QStringLiteral("drawing_mode"))) ==
                QStringList{QStringLiteral("Space")} &&
            portable(defaults.value(QStringLiteral("resize_window"))) ==
                QStringList{QStringLiteral("M")} &&
            portable(defaults.value(QStringLiteral("thumbnail_mode"))) ==
                QStringList{QStringLiteral("R")} &&
            portable(defaults.value(QStringLiteral("hide_to_top"))) ==
                QStringList{QStringLiteral("H")} &&
            portable(defaults.value(QStringLiteral("toggle_click_through"))) ==
                QStringList{QStringLiteral("Ctrl+M")} &&
            portable(defaults.value(QStringLiteral("toggle_lock"))) ==
                QStringList{QStringLiteral("L")} &&
            portable(defaults.value(QStringLiteral("always_on_top"))) ==
                QStringList{QStringLiteral("T")} &&
            portable(defaults.value(QStringLiteral("show_border"))) ==
                QStringList{QStringLiteral("B")} &&
            portable(defaults.value(QStringLiteral("show_shadow"))) ==
                QStringList{QStringLiteral("Y")} &&
            portable(defaults.value(QStringLiteral("close_window"))) ==
                QStringList{QStringLiteral("Esc")} &&
            portable(defaults.value(QStringLiteral("destroy_window"))) ==
                QStringList{QStringLiteral("Shift+Esc")} &&
            portable(defaults.value(QStringLiteral("move_cursor_up"))) ==
                QStringList{QStringLiteral("W"), QStringLiteral("Up")} &&
            portable(defaults.value(QStringLiteral("move_cursor_right"))) ==
                QStringList{QStringLiteral("D"), QStringLiteral("Right")} &&
            shortcutSettings.shortcuts(QStringLiteral("click_through")).isEmpty() &&
            !shortcutSettings.setShortcuts(QStringLiteral("click_through"),
                                           {QStringLiteral("M")}) &&
            shortcutSettings.shortcuts(QStringLiteral("unsupported")).isEmpty() &&
            !shortcutSettings.setShortcuts(QStringLiteral("unsupported"), {QStringLiteral("Q")}),
        "pinned-window shortcut adapter must expose thirty stable actions and defaults");
    require(portable(defaults.value(QStringLiteral("increase_opacity"))) ==
                QStringList{QStringLiteral("]")},
            "increase_opacity must have its default binding");
    require(portable(defaults.value(QStringLiteral("decrease_opacity"))) ==
                QStringList{QStringLiteral("[")},
            "decrease_opacity must have its default binding");
    require(portable(defaults.value(QStringLiteral("increase_scale"))) ==
                QStringList{QStringLiteral(".")},
            "increase_scale must have its default binding");
    require(portable(defaults.value(QStringLiteral("decrease_scale"))) ==
                QStringList{QStringLiteral(",")},
            "decrease_scale must have its default binding");
    require(portable(defaults.value(QStringLiteral("rotate_clockwise"))) ==
                QStringList{QStringLiteral("1")},
            "rotate_clockwise must have its default binding");
    require(portable(defaults.value(QStringLiteral("rotate_counterclockwise"))) ==
                QStringList{QStringLiteral("2")},
            "rotate_counterclockwise must have its default binding");
    require(portable(defaults.value(QStringLiteral("flip_horizontal"))) ==
                QStringList{QStringLiteral("3")},
            "flip_horizontal must have its default binding");
    require(portable(defaults.value(QStringLiteral("flip_vertical"))) ==
                QStringList{QStringLiteral("4")},
            "flip_vertical must have its default binding");
    require(portable(defaults.value(QStringLiteral("reset_transform"))) ==
                QStringList{QStringLiteral("0")},
            "reset_transform must have its default binding");
    require(
        shortcutSettings.setShortcuts(QStringLiteral("drawing_mode"), {QStringLiteral("Alt+E")}) &&
            portable(shortcutSettings.shortcuts(QStringLiteral("drawing_mode"))) ==
                QStringList{QStringLiteral("Alt+E")},
        "pinned-window shortcuts must round-trip through the typed adapter");
    shortcuts::ShortcutBindingMap duplicates = shortcutSettings.allShortcuts();
    duplicates.insert(QStringLiteral("thumbnail_mode"), {QStringLiteral("Ctrl+C")});
    require(!shortcutSettings.setAllShortcutsAtomic(duplicates),
            "pinned-window shortcuts must reject duplicate bindings atomically");
    require(shortcutSettings.setAllShortcutsAtomic(defaults) &&
                shortcutSettings.allShortcuts() == defaults,
            "resetting the complete pinned shortcut map must restore all image commands");
}

void pinToScreenShortcutSettingsRoundTrip() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create pinned-shortcut adapter directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create pinned-shortcut executable directory");
    static_cast<void>(initialize(executable, temporary.path()));
    verifyPinToScreenShortcutSettings();
    storage::ApplicationStorage::instance().shutdown();
}

void pinnedDestroyShortcutMigratesPreviousDefault() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create pinned Destroy shortcut migration directory");
    const QString key = QStringLiteral("pin_to_screen_shortcuts/destroy_window");
    const auto write = [&](const QString& name, int version, const QJsonArray& shortcut) {
        const QString path = temporary.filePath(name);
        writeBytes(path,
                   QJsonDocument(QJsonObject{
                                     {QStringLiteral("storage"),
                                      QJsonObject{{QStringLiteral("schema_version"), version}}},
                                     {QStringLiteral("pin_to_screen_shortcuts"),
                                      QJsonObject{{QStringLiteral("destroy_window"), shortcut}}},
                                 })
                       .toJson());
        return path;
    };
    const auto migratedShortcut = structuredShortcuts(QJsonArray{QStringLiteral("Shift+Esc")});
    const auto oldShortcut = structuredShortcuts(QJsonArray{QStringLiteral("Ctrl+Esc")});
    for (int index = 0; index < 2; ++index) {
        const QJsonArray oldDefault =
            index == 0 ? QJsonArray{QStringLiteral("Ctrl+Esc")} : oldShortcut;
        const QString path = write(QStringLiteral("legacy-%1.json").arg(index), 2, oldDefault);
        {
            storage::ConfigurationStore store(path, true, true, 60000);
            require(store.value(key).toArray() == migratedShortcut && store.isDirty() &&
                        store.value(QStringLiteral("storage/schema_version")).toInt() == 3 &&
                        store.flushNow().success,
                    "v2 pinned Destroy default must migrate to Shift+Esc");
        }
        storage::ConfigurationStore reloaded(path, true, true, 60000);
        require(reloaded.value(key).toArray() == migratedShortcut &&
                    reloaded.value(QStringLiteral("storage/schema_version")).toInt() == 3,
                "migrated pinned Destroy shortcut must persist after reload");
    }
    const QString customizedPath =
        write(QStringLiteral("custom.json"), 2, QJsonArray{QStringLiteral("Alt+X")});
    storage::ConfigurationStore customized(customizedPath, true, true, 60000);
    require(customized.value(key).toArray() ==
                structuredShortcuts(QJsonArray{QStringLiteral("Alt+X")}),
            "a customized pinned Destroy shortcut must survive migration");
    const QString currentPath = write(QStringLiteral("current.json"), 3, oldShortcut);
    storage::ConfigurationStore current(currentPath, true, true, 60000);
    require(current.value(key).toArray() == oldShortcut,
            "an explicitly configured Ctrl+Esc on v3 must remain unchanged");
}

void obsoleteClickThroughShortcutIsIgnored() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create obsolete pinned-shortcut directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    writeBytes(config, QByteArrayLiteral("{\n"
                                         "  \"storage\": {\"schema_version\": 2},\n"
                                         "  \"pin_to_screen_shortcuts\": {\n"
                                         "    \"click_through\": [\"Alt+M\"]\n"
                                         "  }\n"
                                         "}\n"));
    storage::ConfigurationStore store(config, true, true, 60000);
    require(
        storage::ConfigurationSchema::entry(
            QStringLiteral("pin_to_screen_shortcuts/click_through")) == nullptr &&
            store.value(QStringLiteral("pin_to_screen_shortcuts/click_through")).isNull() &&
            store.value(QStringLiteral("pin_to_screen_shortcuts/toggle_click_through")).toArray() ==
                structuredShortcuts(QJsonArray{QStringLiteral("Ctrl+M")}) &&
            store.flushNow().success &&
            readObject(config)
                    .value(QStringLiteral("pin_to_screen_shortcuts"))
                    .toObject()
                    .value(QStringLiteral("click_through"))
                    .toArray() == QJsonArray{QStringLiteral("Alt+M")},
        "the obsolete click-through shortcut must be ignored but preserved as unknown data");
}

void shortcutSchemaMigrationAndPhysicalMetadataRoundTrip() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create shortcut migration directory");
    const QString config = temporary.filePath(QStringLiteral("config.json"));
    writeBytes(
        config,
        QByteArrayLiteral(
            R"({"storage":{"schema_version":1},"global_shortcuts":{"screenshot":["Ctrl+Alt+K"]},"screenshot_shortcuts":{"copy_color":["Alt+C"]}})"));
    {
        storage::ConfigurationStore store(config, true, true, 60000);
        require(store.value(QStringLiteral("storage/schema_version")).toInt() == 3 &&
                    store.value(QStringLiteral("global_shortcuts/screenshot")).toArray() ==
                        QJsonArray{shortcutObject(QStringLiteral("Ctrl+Alt+K"))} &&
                    store.value(QStringLiteral("screenshot_shortcuts/copy_color")).toArray() ==
                        QJsonArray{shortcutObject(QStringLiteral("Alt+C"))} &&
                    !store.value(QStringLiteral("global_shortcuts/screenshot_copy"))
                         .toArray()
                         .isEmpty() &&
                    store.isDirty() && store.flushNow().success,
                "v1 shortcut migration must preserve explicit bindings and fill only missing keys");
    }

    const QJsonObject physicalWithUnknown{
        {QStringLiteral("portable"), QStringLiteral("Ctrl+C")},
        {QStringLiteral("physical_keys"),
         QJsonObject{{QStringLiteral("macos"), 8}, {QStringLiteral("future"), 99}}},
    };
    const QJsonObject invalidPhysical{
        {QStringLiteral("portable"), QStringLiteral("Alt+X")},
        {QStringLiteral("physical_keys"), QJsonObject{{QStringLiteral("macos"), 128}}},
    };
    QJsonObject document = storage::ConfigurationSchema::completeDefaultDocument();
    QJsonObject screenshot = document.value(QStringLiteral("screenshot_shortcuts")).toObject();
    screenshot.insert(
        QStringLiteral("copy_color"),
        QJsonArray{physicalWithUnknown, invalidPhysical,
                   QJsonObject{{QStringLiteral("portable"), QStringLiteral("Ctrl+K, Ctrl+C")}}});
    document.insert(QStringLiteral("screenshot_shortcuts"), screenshot);
    writeBytes(config, QJsonDocument(document).toJson(QJsonDocument::Indented));

    storage::ConfigurationStore store(config, true, true, 60000);
    const QJsonArray repaired =
        store.value(QStringLiteral("screenshot_shortcuts/copy_color")).toArray();
    require(repaired == QJsonArray{shortcutObject(QStringLiteral("Ctrl+C"), 8),
                                   shortcutObject(QStringLiteral("Alt+X"))} &&
                store.isDirty() && store.flushNow().success,
            "v2 repair must retain portable fallbacks while dropping malformed metadata/items");
    storage::ConfigurationStore reloaded(config, true, true, 60000);
    require(reloaded.value(QStringLiteral("screenshot_shortcuts/copy_color")).toArray() == repaired,
            "structured shortcut bindings must round-trip without losing physical metadata");
}

void recordingPostProcessingPreferencesPersistAndValidate() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated post-processing settings storage");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create post-processing settings executable directory");
    auto& applicationStorage = initialize(executable, temporary.path());
    const storage::RecordingSettings recording;
    require(!recording.postProcessingEnabled() &&
                recording.postProcessingEffect() == QStringLiteral("progress_bar") &&
                recording.progressBarColor() == QColor(22, 119, 255) &&
                recording.setPostProcessingEnabled(true) &&
                recording.setPostProcessingEffect(QStringLiteral("playback_time")) &&
                recording.setProgressBarColor(QColor(12, 34, 56, 78)) &&
                storage::RecordingSettings().postProcessingEnabled() &&
                storage::RecordingSettings().postProcessingEffect() ==
                    QStringLiteral("playback_time") &&
                storage::RecordingSettings().progressBarColor() == QColor(12, 34, 56, 78) &&
                !recording.setPostProcessingEffect(QStringLiteral("unsupported")) &&
                !recording.setProgressBarColor(QColor()) &&
                recording.postProcessingEffect() == QStringLiteral("playback_time") &&
                recording.progressBarColor() == QColor(12, 34, 56, 78),
            "post-processing preferences validate and persist including alpha");
    require(applicationStorage.flushNow().success, "flush post-processing settings");
    applicationStorage.shutdown();
    static_cast<void>(initialize(executable, temporary.path()));
    require(recording.postProcessingEnabled() &&
                recording.postProcessingEffect() == QStringLiteral("playback_time") &&
                recording.progressBarColor() == QColor(12, 34, 56, 78),
            "post-processing preferences survive storage restart");
    applicationStorage.shutdown();
}

void settingsAdaptersRoundTripAndRejectInvalidValues() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create settings adapter directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create settings executable directory");
    auto& applicationStorage = initialize(executable, temporary.path());

    const storage::SystemSettings system;
    require(system.autoStartAtBoot() && !system.launchAsAdministrator(),
            "elevated startup must default off");
    require(system.setLaunchAsAdministrator(true) && system.launchAsAdministrator(),
            "elevated startup preference must persist");
    require(applicationStorage.flushNow().success, "startup preferences must flush");
    applicationStorage.shutdown();
    static_cast<void>(initialize(executable, temporary.path()));
    require(system.launchAsAdministrator(),
            "elevated startup preference must survive storage restart");
    const storage::ExtendedFeaturesSettings extendedFeatures;
    require(!extendedFeatures.translationPageEnabled() &&
                !extendedFeatures.jumpToTranslationPage() &&
                extendedFeatures.setTranslationPageEnabled(true) &&
                extendedFeatures.setJumpToTranslationPage(true) &&
                applicationStorage.flushNow().success,
            "extended translation settings must default off and persist through typed adapters");
    applicationStorage.shutdown();
    static_cast<void>(initialize(executable, temporary.path()));
    require(extendedFeatures.translationPageEnabled() && extendedFeatures.jumpToTranslationPage(),
            "extended translation settings must survive storage restart");
    require(extendedFeatures.setJumpToTranslationPage(false) &&
                extendedFeatures.setTranslationPageEnabled(false),
            "extended translation settings must restore both default values");
    require(system.setAutoStartAtBoot(false) && !system.launchAsAdministrator() &&
                !system.setLaunchAsAdministrator(true),
            "disabling auto-start must reset and gate administrator launch");
    require(system.setAutoStartAtBoot(true) && !system.launchAsAdministrator(),
            "re-enabling auto-start must not restore elevation implicitly");
    const storage::ScreenshotSettings screenshot;
    require(!screenshot.showCursor(), "cursor capture must default off");
    require(screenshot.setShowCursor(true) && storage::ScreenshotSettings().showCursor(),
            "cursor capture must persist when enabled");
    require(applicationStorage.configuration().flushNow().success,
            "enabled cursor capture must be flushable");
    applicationStorage.shutdown();
    static_cast<void>(initialize(executable, temporary.path()));
    require(storage::ScreenshotSettings().showCursor(),
            "enabled cursor capture must survive storage restart");
    require(screenshot.setShowCursor(false) && !storage::ScreenshotSettings().showCursor(),
            "cursor capture must support disabling");
    require(applicationStorage.configuration().flushNow().success,
            "disabled cursor capture must be flushable");
    applicationStorage.shutdown();
    static_cast<void>(initialize(executable, temporary.path()));
    require(!storage::ScreenshotSettings().showCursor(),
            "disabled cursor capture must survive storage restart");
    require(screenshot.restoreOriginalScreenColors(), "screen color restoration must default on");
    require(screenshot.setRestoreOriginalScreenColors(false) &&
                !storage::ScreenshotSettings().restoreOriginalScreenColors(),
            "screen color restoration must persist when disabled");
    require(screenshot.setRestoreOriginalScreenColors(true) &&
                storage::ScreenshotSettings().restoreOriginalScreenColors(),
            "screen color restoration must support re-enabling");
    require(screenshot.captureUiInScrollingScreenshot(),
            "scrolling screenshot UI capture must default on");
    require(screenshot.setCaptureUiInScrollingScreenshot(false) &&
                !storage::ScreenshotSettings().captureUiInScrollingScreenshot(),
            "scrolling screenshot UI capture must persist when disabled");
    require(screenshot.setCaptureUiInScrollingScreenshot(true) &&
                storage::ScreenshotSettings().captureUiInScrollingScreenshot(),
            "scrolling screenshot UI capture must support re-enabling");
    require(screenshot.autoExecuteAfterTextRecognition() == QStringLiteral("no_action") &&
                screenshot.doubleClickAction() == QStringLiteral("copy") &&
                screenshot.middleMouseButtonAction() == QStringLiteral("pin") &&
                !screenshot.showCursor() && !screenshot.autoSaveAfterCopy() &&
                !screenshot.copyImageFileToClipboard() &&
                screenshot.imageFormat() == QStringLiteral("png") &&
                screenshot.compressionLevel() == QStringLiteral("medium") &&
                screenshot.imageQuality() == 100 &&
                screenshot.manualSaveFormatOptions().isEmpty() &&
                screenshot.manualSaveFilenameFormat() ==
                    QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}") &&
                screenshot.autoSaveFilenameFormat() ==
                    QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}") &&
                screenshot.lastManualSaveDirectory().isEmpty() &&
                screenshot.imageSaveDirectory() ==
                    systemSaveDirectory(QStandardPaths::PicturesLocation),
            "screenshot adapters must expose requested defaults");
    require(screenshot.setAutoExecuteAfterTextRecognition(
                QStringLiteral("quick_copy_text_and_end_screenshot")) &&
                screenshot.setDoubleClickAction(QStringLiteral("save")) &&
                screenshot.setMiddleMouseButtonAction(QStringLiteral("none")) &&
                screenshot.setAutoSaveAfterCopy(true) &&
                screenshot.setCopyImageFileToClipboard(true) &&
                screenshot.setImageSaveDirectory(QStringLiteral("D:/Captures")) &&
                screenshot.setLastManualSaveDirectory(QStringLiteral("D:/Exports")) &&
                screenshot.setImageFormat(QStringLiteral("bmp")) &&
                screenshot.setCompressionLevel(QStringLiteral("high")) &&
                screenshot.setImageQuality(0) &&
                screenshot.setManualSaveFormatOptions(QJsonObject{
                    {QStringLiteral("png"),
                     QJsonObject{{QStringLiteral("compression_level"), QStringLiteral("medium")}}},
                    {QStringLiteral("jpeg"), QJsonObject{{QStringLiteral("quality"), 83}}}}) &&
                screenshot.setManualSaveFilenameFormat(QStringLiteral("Manual_{yyyyMMdd}")) &&
                screenshot.setAutoSaveFilenameFormat(QStringLiteral("Auto_{HHmmss}")) &&
                screenshot.autoExecuteAfterTextRecognition() ==
                    QStringLiteral("quick_copy_text_and_end_screenshot") &&
                screenshot.doubleClickAction() == QStringLiteral("save") &&
                screenshot.middleMouseButtonAction() == QStringLiteral("none") &&
                screenshot.autoSaveAfterCopy() && screenshot.copyImageFileToClipboard() &&
                screenshot.imageSaveDirectory() == QStringLiteral("D:/Captures") &&
                screenshot.lastManualSaveDirectory() == QStringLiteral("D:/Exports") &&
                screenshot.imageFormat() == QStringLiteral("bmp") &&
                screenshot.compressionLevel() == QStringLiteral("high") &&
                screenshot.imageQuality() == 0 &&
                screenshot.manualSaveFormatOptions()
                        .value(QStringLiteral("jpeg"))
                        .toObject()
                        .value(QStringLiteral("quality")) == 83 &&
                screenshot.manualSaveFilenameFormat() == QStringLiteral("Manual_{yyyyMMdd}") &&
                screenshot.autoSaveFilenameFormat() == QStringLiteral("Auto_{HHmmss}"),
            "screenshot adapters must persist every new value type");
    require(!screenshot.setDoubleClickAction(QStringLiteral("unsupported")) &&
                !screenshot.setImageFormat(QStringLiteral("unsupported")) &&
                !screenshot.setCompressionLevel(QStringLiteral("maximum")) &&
                !screenshot.setImageQuality(101) &&
                !screenshot.setAutoSaveFilenameFormat(QStringLiteral("invalid/name")) &&
                screenshot.doubleClickAction() == QStringLiteral("save") &&
                screenshot.compressionLevel() == QStringLiteral("high") &&
                screenshot.imageQuality() == 0,
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
    require(pin.doubleClickAction() == QStringLiteral("thumbnail_mode"),
            "pinned double-click must default to thumbnail mode");
    for (const QString& action : {QStringLiteral("none"), QStringLiteral("thumbnail_mode"),
                                  QStringLiteral("hide_to_top"), QStringLiteral("close")}) {
        require(pin.setDoubleClickAction(action) && pin.doubleClickAction() == action,
                "pinned double-click actions must round-trip through storage");
    }
    require(!pin.setDoubleClickAction(QStringLiteral("unsupported")) &&
                pin.doubleClickAction() == QStringLiteral("close"),
            "invalid pinned double-click actions must preserve the saved choice");
    require(pin.middleMouseButtonAction() == QStringLiteral("reset_zoom"),
            "pinned middle-click must default to reset zoom");
    for (const QString& action :
         {QStringLiteral("none"), QStringLiteral("reset_zoom"), QStringLiteral("thumbnail_mode"),
          QStringLiteral("hide_to_top"), QStringLiteral("close")}) {
        require(pin.setMiddleMouseButtonAction(action) && pin.middleMouseButtonAction() == action,
                "pinned middle-click actions must round-trip through storage");
    }
    require(!pin.setMiddleMouseButtonAction(QStringLiteral("unsupported")) &&
                pin.middleMouseButtonAction() == QStringLiteral("close"),
            "invalid pinned middle-click actions must preserve the saved choice");
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
                recording.animatedImageClarity() == QStringLiteral("720p") &&
                recording.animatedImageFrameRate() == 10 && recording.loopAnimatedImages() &&
                recording.outputFormat() == QStringLiteral("mp4") &&
                recording.mouseTrailColor() == QColor(0, 0, 0, 0) &&
                recording.mouseClickColor() == QColor(0, 0, 0, 0) && recording.showCursor() &&
                !recording.showKeyboard() && recording.encoder() == QStringLiteral("h264_hw") &&
                recording.videoQuality() == 80 &&
                recording.encodingPreset() == QStringLiteral("veryfast") &&
                recording.captureToolbarInRecording() &&
                recording.videoSaveDirectory() ==
                    systemSaveDirectory(QStandardPaths::MoviesLocation) &&
                recording.videoFilenameFormat() ==
                    QStringLiteral("SnowShot_Video_{YYYY-MM-DD_HH-mm-ss}"),
            "recording adapters must expose requested defaults");
    require(
        recording.setScreenRecordingClarity(QStringLiteral("2k")) && recording.setFrameRate(83) &&
            recording.setAnimatedImageClarity(QStringLiteral("480p")) &&
            recording.setAnimatedImageFrameRate(24) && recording.setLoopAnimatedImages(false) &&
            !storage::RecordingSettings().loopAnimatedImages() &&
            recording.setOutputFormat(QStringLiteral("webp")) &&
            recording.setMouseTrailColor(QColor(1, 2, 3, 4)) &&
            recording.setMouseClickColor(QColor(5, 6, 7, 128)) && recording.setShowCursor(false) &&
            recording.setShowKeyboard(true) && storage::RecordingSettings().showKeyboard() &&
            recording.setEncoder(QStringLiteral("h265")) && recording.setVideoQuality(37) &&
            recording.setEncodingPreset(QStringLiteral("placebo")) &&
            recording.setCaptureToolbarInRecording(false) &&
            recording.setVideoSaveDirectory(QStringLiteral("D:/Recordings")) &&
            recording.setVideoFilenameFormat(QStringLiteral("Recording_{yyyyMMdd}")) &&
            recording.screenRecordingClarity() == QStringLiteral("2k") &&
            recording.frameRate() == 83 &&
            recording.animatedImageClarity() == QStringLiteral("480p") &&
            recording.animatedImageFrameRate() == 24 &&
            recording.outputFormat() == QStringLiteral("webp") &&
            recording.mouseTrailColor() == QColor(1, 2, 3, 4) &&
            recording.mouseClickColor() == QColor(5, 6, 7, 128) && !recording.showCursor() &&
            recording.encoder() == QStringLiteral("h265") && recording.videoQuality() == 37 &&
            recording.encodingPreset() == QStringLiteral("placebo") &&
            !recording.captureToolbarInRecording() &&
            recording.videoSaveDirectory() == QStringLiteral("D:/Recordings") &&
            recording.videoFilenameFormat() == QStringLiteral("Recording_{yyyyMMdd}"),
        "recording adapters must round-trip every requested option");
    require(recording.setLoopAnimatedImages(true) &&
                storage::RecordingSettings().loopAnimatedImages(),
            "recording loop preference must round-trip enabled");
    require(recording.setEncoder(QStringLiteral("h264_hw")) &&
                recording.encoder() == QStringLiteral("h264_hw") &&
                recording.setEncoder(QStringLiteral("h264")) &&
                recording.encoder() == QStringLiteral("h264"),
            "recording adapters must round-trip every advertised encoder");
    require(!recording.setFrameRate(25) && !recording.setAnimatedImageFrameRate(30) &&
                !recording.setScreenRecordingClarity(QStringLiteral("8k")) &&
                !recording.setOutputFormat(QStringLiteral("avi")) &&
                !recording.setMouseTrailColor(QColor()) &&
                !recording.setMouseClickColor(QColor()) &&
                !recording.setEncoder(QStringLiteral("vp9")) && !recording.setVideoQuality(101) &&
                !recording.setVideoFilenameFormat(QStringLiteral("invalid/name")) &&
                recording.frameRate() == 83 && recording.animatedImageFrameRate() == 24 &&
                recording.screenRecordingClarity() == QStringLiteral("2k") &&
                recording.outputFormat() == QStringLiteral("webp") &&
                recording.mouseTrailColor() == QColor(1, 2, 3, 4) &&
                recording.mouseClickColor() == QColor(5, 6, 7, 128) &&
                recording.encoder() == QStringLiteral("h264") && recording.videoQuality() == 37,
            "recording adapters must reject unadvertised values atomically");

    require(!recording.mouseHighlightEnabled() && !recording.recordMouseClicks() &&
                recording.mouseHighlightColor() == QColor(255, 255, 0, 128),
            "new mouse recording settings default off with soft yellow");
    require(recording.setMouseHighlightEnabled(true) && recording.setRecordMouseClicks(true) &&
                recording.setMouseHighlightColor(QColor(12, 34, 56, 78)),
            "mouse recording settings save");
    const storage::RecordingSettings reloadedRecording;
    require(reloadedRecording.mouseHighlightEnabled() && reloadedRecording.recordMouseClicks() &&
                reloadedRecording.mouseHighlightColor() == QColor(12, 34, 56, 78),
            "mouse recording settings persist across adapter instances");
    require(!recording.setMouseHighlightColor(QColor()) &&
                recording.mouseHighlightColor() == QColor(12, 34, 56, 78),
            "invalid highlight color is rejected atomically");
    const storage::TraySettings tray;
    const storage::NetworkSettings network;
    const storage::GlobalShortcutSettings globalShortcuts;
    require(
        network.proxy() == QStringLiteral("none") && network.setProxy(QStringLiteral("system")) &&
            network.proxy() == QStringLiteral("system") &&
            !network.setProxy(QStringLiteral("unsupported")) &&
            network.proxy() == QStringLiteral("system") &&
            tray.middleClickAction() == QStringLiteral("screenshot_fixed") &&
            tray.setMiddleClickAction(QStringLiteral("screenshot_copy")) &&
            tray.middleClickAction() == QStringLiteral("screenshot_copy") &&
            !tray.setMiddleClickAction(QStringLiteral("unsupported")) &&
            tray.leftClickAction() == QStringLiteral("screenshot") &&
            tray.setLeftClickAction(QStringLiteral("show_main_window")) &&
            tray.leftClickAction() == QStringLiteral("show_main_window") &&
            !tray.setLeftClickAction(QStringLiteral("unsupported")) &&
            !globalShortcuts.disableOnFocusedFullscreenWindow() &&
            globalShortcuts.setDisableOnFocusedFullscreenWindow(true) &&
            globalShortcuts.disableOnFocusedFullscreenWindow() && tray.menuOptions().size() == 12 &&
            tray.menuOptions().contains(QStringLiteral("tray.show-main-window")) &&
            tray.menuOptions().contains(QStringLiteral("tray.window-grouping")) &&
            tray.setMenuOptions({QStringLiteral("tray.exit"), QStringLiteral("quick.screenshot"),
                                 QStringLiteral("quick.screenshot"), QStringLiteral("unknown")}) &&
            tray.menuOptions() ==
                QStringList{QStringLiteral("tray.exit"), QStringLiteral("quick.screenshot")},
        "network, tray, and global-hotkey adapters must round-trip and validate settings");

    const storage::DrawingShortcutSettings drawingShortcuts;
    const storage::ScreenshotShortcutSettings screenshotShortcuts;
    const shortcuts::ShortcutBindingMap screenshotDefaults = screenshotShortcuts.allShortcuts();
    require(
        screenshotDefaults.size() == 32 &&
            portable(screenshotShortcuts.moveTool()) ==
                QStringList{QStringLiteral("M"), QStringLiteral("Ctrl+E")} &&
            portable(screenshotShortcuts.moveCursorUp()) ==
                QStringList{QStringLiteral("W"), QStringLiteral("Up")} &&
            portable(screenshotShortcuts.moveCursorDown()) ==
                QStringList{QStringLiteral("S"), QStringLiteral("Down")} &&
            portable(screenshotShortcuts.moveCursorLeft()) ==
                QStringList{QStringLiteral("A"), QStringLiteral("Left")} &&
            portable(screenshotShortcuts.moveCursorRight()) ==
                QStringList{QStringLiteral("D"), QStringLiteral("Right")} &&
            portable(screenshotShortcuts.moveEntireSelection()) ==
                QStringList{QStringLiteral("Space")} &&
            portable(screenshotShortcuts.keepSelectionWidthAndHeightConsistent()) ==
                QStringList{QStringLiteral("Shift")} &&
            portable(screenshotShortcuts.switchSelectionBetweenWindowAndWindowSubElement()) ==
                QStringList{QStringLiteral("Tab")} &&
            portable(screenshotShortcuts.previousScreenshotHistory()) ==
                QStringList{QStringLiteral(",")} &&
            portable(screenshotShortcuts.nextScreenshotHistory()) ==
                QStringList{QStringLiteral(".")} &&
            portable(screenshotShortcuts.selectPreviouslySelectedArea()) ==
                QStringList{QStringLiteral("R")} &&
            portable(screenshotShortcuts.recapture()) == QStringList{QStringLiteral("Alt+R")} &&
            portable(screenshotShortcuts.shortcuts(QStringLiteral("toggle_cursor_visibility"))) ==
                QStringList{QStringLiteral("`")} &&
            portable(screenshotShortcuts.copyColor()) == QStringList{QStringLiteral("C")} &&
            portable(screenshotShortcuts.toggleCoordinateMode()) ==
                QStringList{QStringLiteral("Shift+P")} &&
            portable(screenshotDefaults.value(QStringLiteral("print"))) ==
                QStringList{QStringLiteral("Ctrl+P")} &&
            portable(screenshotShortcuts.toggleGuides()) == QStringList{QStringLiteral("Alt")} &&
            portable(screenshotDefaults.value(QStringLiteral("pin_to_screen"))) ==
                QStringList{QStringLiteral("Ctrl+F")} &&
            portable(screenshotDefaults.value(QStringLiteral("quick_save"))) ==
                QStringList{QStringLiteral("Ctrl+Shift+S")} &&
            portable(screenshotDefaults.value(QStringLiteral("save_as_file"))) ==
                QStringList{QStringLiteral("Ctrl+S")} &&
            portable(screenshotDefaults.value(QStringLiteral("upload_to_cloud"))) ==
                QStringList{QStringLiteral("Ctrl+U")} &&
            portable(screenshotDefaults.value(QStringLiteral("cancel_screenshot"))) ==
                QStringList{QStringLiteral("Esc")} &&
            portable(screenshotDefaults.value(QStringLiteral("copy_to_clipboard"))) ==
                QStringList{QStringLiteral("Ctrl+C")} &&
            portable(screenshotDefaults.value(QStringLiteral("undo"))) ==
                QStringList{QStringLiteral("Ctrl+Z")} &&
            portable(screenshotDefaults.value(QStringLiteral("redo"))) ==
                QStringList{QStringLiteral("Ctrl+Y")} &&
            screenshotShortcuts.shortcuts(QStringLiteral("unsupported")).isEmpty() &&
            !screenshotShortcuts.setShortcuts(QStringLiteral("unsupported"), {QStringLiteral("Q")}),
        "screenshot shortcut adapter must expose all stable actions and defaults");
    require(portable(screenshotDefaults.value(QStringLiteral("selection_aspect_ratio_snap"))) ==
                QStringList{QStringLiteral("Q")},
            "snap must expose Q as its configurable default");
    require(screenshotShortcuts.setShortcuts(QStringLiteral("toggle_guides"),
                                             {QStringLiteral("Ctrl+G")}) &&
                portable(screenshotShortcuts.toggleGuides()) ==
                    QStringList{QStringLiteral("Ctrl+G")} &&
                screenshotShortcuts.setShortcuts(QStringLiteral("toggle_guides"),
                                                 {QStringLiteral("Alt")}) &&
                portable(screenshotShortcuts.toggleGuides()) == QStringList{QStringLiteral("Alt")},
            "guide toggle shortcut must round-trip through the typed adapter");
    require(
        screenshotShortcuts.setShortcuts(QStringLiteral("cancel_screenshot"),
                                         {QStringLiteral("Esc")}) &&
            screenshotShortcuts.setShortcuts(QStringLiteral("copy_to_clipboard"),
                                             {QStringLiteral("Ctrl+C")}) &&
            screenshotShortcuts.setShortcuts(QStringLiteral("undo"), {QStringLiteral("Ctrl+Z")}),
        "reserved screenshot commands must accept their own configurable defaults");
    require(screenshotShortcuts.setMoveTool({QStringLiteral("Alt+M")}) &&
                portable(screenshotShortcuts.moveTool()) == QStringList{QStringLiteral("Alt+M")} &&
                screenshotShortcuts.setMoveCursorUp({QStringLiteral("Ctrl+Alt+Up")}) &&
                portable(screenshotShortcuts.moveCursorUp()) ==
                    QStringList{QStringLiteral("Ctrl+Alt+Up")} &&
                screenshotShortcuts.setShortcuts(QStringLiteral("recapture"),
                                                 {QStringLiteral("Ctrl+Alt+R")}) &&
                portable(screenshotShortcuts.recapture()) ==
                    QStringList{QStringLiteral("Ctrl+Alt+R")},
            "screenshot shortcuts must round-trip through the typed adapter");
    require(screenshotShortcuts.setShortcuts(QStringLiteral("toggle_cursor_visibility"),
                                             {QStringLiteral("Alt+`")}) &&
                portable(screenshotShortcuts.shortcuts(QStringLiteral(
                    "toggle_cursor_visibility"))) == QStringList{QStringLiteral("Alt+`")},
            "cursor visibility shortcut must be configurable through the typed adapter");
    require(screenshotShortcuts.setMoveCursorRight({QStringLiteral("1")}) &&
                portable(screenshotShortcuts.moveCursorRight()) == QStringList{QStringLiteral("1")},
            "screenshot shortcuts must allow a key assigned in the drawing category");
    shortcuts::ShortcutBindingMap swappedHistoryShortcuts = screenshotShortcuts.allShortcuts();
    swappedHistoryShortcuts.insert(QStringLiteral("previous_screenshot_history"),
                                   {QStringLiteral(".")});
    swappedHistoryShortcuts.insert(QStringLiteral("next_screenshot_history"),
                                   {QStringLiteral(",")});
    require(screenshotShortcuts.setAllShortcutsAtomic(swappedHistoryShortcuts) &&
                portable(screenshotShortcuts.previousScreenshotHistory()) ==
                    QStringList{QStringLiteral(".")} &&
                portable(screenshotShortcuts.nextScreenshotHistory()) ==
                    QStringList{QStringLiteral(",")},
            "history shortcuts must allow comma and period to be swapped atomically");

    verifyPinToScreenShortcutSettings();

    const shortcuts::ShortcutBindingMap defaults = drawingShortcuts.allShortcuts();
    require(
        defaults.size() == 12 && defaults.contains(QStringLiteral("line")) &&
            defaults.value(QStringLiteral("line")).isEmpty() &&
            defaults.contains(QStringLiteral("spotlight")) &&
            defaults.value(QStringLiteral("spotlight")).isEmpty() &&
            portable(defaults.value(QStringLiteral("select"))) ==
                QStringList{QStringLiteral("V")} &&
            portable(defaults.value(QStringLiteral("shape"))) == QStringList{QStringLiteral("1")} &&
            portable(defaults.value(QStringLiteral("arrow"))) == QStringList{QStringLiteral("2")} &&
            portable(defaults.value(QStringLiteral("watermark"))) ==
                QStringList{QStringLiteral("9")} &&
            drawingShortcuts.shortcuts(QStringLiteral("unsupported")).isEmpty() &&
            !drawingShortcuts.setShortcuts(QStringLiteral("unsupported"), {QStringLiteral("Q")}),
        "drawing shortcut adapter must expose all twelve tools with empty new defaults");

    require(drawingShortcuts.setSelect({QStringLiteral("Ctrl+Shift+V")}) &&
                portable(drawingShortcuts.select()) == QStringList{QStringLiteral("Ctrl+Shift+V")},
            "Select drawing shortcuts must round-trip through the typed adapter");

    require(drawingShortcuts.setShape({QStringLiteral("Ctrl+Shift+K"), QStringLiteral("Alt+1")}) &&
                portable(drawingShortcuts.shape()) ==
                    QStringList{QStringLiteral("Ctrl+Shift+K"), QStringLiteral("Alt+1")},
            "drawing shortcut adapter must persist normalized tool shortcuts");
    require(drawingShortcuts.setWatermark({QStringLiteral("Alt+M")}) &&
                portable(drawingShortcuts.watermark()) == QStringList{QStringLiteral("Alt+M")},
            "drawing shortcuts must allow a key assigned in the screenshot category");
    const shortcuts::ShortcutBindingMap beforeCollision = drawingShortcuts.allShortcuts();
    require(!drawingShortcuts.setArrow({QStringLiteral("ctrl+shift+k")}) &&
                drawingShortcuts.allShortcuts() == beforeCollision,
            "case-insensitive cross-tool collisions must reject the complete atomic update");

    for (const QString& reserved :
         {QStringLiteral("Escape"), QStringLiteral("Delete"), QStringLiteral("Ctrl+C"),
          QStringLiteral("Ctrl+Shift+Z"), QStringLiteral(","), QStringLiteral("F4")}) {
        require(!drawingShortcuts.setArrow({reserved}) &&
                    drawingShortcuts.allShortcuts() == beforeCollision,
                "canvas and screenshot commands must remain reserved from drawing shortcuts");
    }

    shortcuts::ShortcutBindingMap incomplete = beforeCollision;
    incomplete.remove(QStringLiteral("watermark"));
    require(!drawingShortcuts.setAllShortcutsAtomic(incomplete) &&
                drawingShortcuts.allShortcuts() == beforeCollision,
            "atomic drawing shortcut updates must require all twelve tools");

    shortcuts::ShortcutBindingMap emptyAssignment = beforeCollision;
    emptyAssignment.insert(QStringLiteral("shape"), {});
    require(drawingShortcuts.setAllShortcutsAtomic(emptyAssignment) &&
                drawingShortcuts.shape().isEmpty(),
            "drawing shortcuts must allow a tool to be deliberately left unassigned");

    require(applicationStorage.configuration().flushNow().success,
            "new settings adapter mutations must be flushable");
    applicationStorage.shutdown();
    static_cast<void>(initialize(executable, temporary.path()));
    require(screenshot.imageSaveDirectory() == QStringLiteral("D:/Captures") &&
                screenshot.compressionLevel() == QStringLiteral("high") &&
                screenshot.imageQuality() == 0 &&
                screenshot.manualSaveFormatOptions()
                        .value(QStringLiteral("png"))
                        .toObject()
                        .value(QStringLiteral("compression_level")) == QStringLiteral("medium") &&
                recording.videoSaveDirectory() == QStringLiteral("D:/Recordings"),
            "custom save directories must survive reload without being replaced by defaults");
}

void invalidCaptureCursorConfigurationFallsBackToDisabled() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create invalid cursor setting directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    writeBytes(config, QByteArrayLiteral("{\n"
                                         "  \"storage\": {\"schema_version\": 1},\n"
                                         "  \"screenshot\": {\"capture_cursor\": \"enabled\"}\n"
                                         "}\n"));
    storage::ConfigurationStore store(config, true, true, 60000);
    require(!store.value(QStringLiteral("screenshot/capture_cursor")).toBool() && store.isDirty() &&
                store.flushNow().success &&
                !readObject(config)
                     .value(QStringLiteral("screenshot"))
                     .toObject()
                     .value(QStringLiteral("capture_cursor"))
                     .toBool(),
            "invalid stored cursor capture values must be replaced with disabled");
}

void invalidOcrModelConfigurationFallsBackToSmallWithoutAMigration() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create invalid OCR model setting directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    writeBytes(config, QByteArrayLiteral("{\n"
                                         "  \"storage\": {\"schema_version\": 1},\n"
                                         "  \"text_recognition\": {\"model_type\": \"large\"}\n"
                                         "}\n"));
    storage::ConfigurationStore store(config, true, true, 60000);
    require(store.value(QStringLiteral("text_recognition/model_type")).toString() ==
                    QStringLiteral("small") &&
                store.value(QStringLiteral("storage/schema_version")).toInt() == 3 &&
                store.isDirty() && store.flushNow().success,
            "invalid OCR model types must normalize while migrating the schema version");
}

void missingOcrModelConfigurationDefaultsToSmallWithoutAMigration() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create missing OCR model setting directory");
    const QString config = QDir(temporary.path()).filePath(QStringLiteral("config.json"));
    writeBytes(config,
               QByteArrayLiteral("{\n"
                                 "  \"storage\": {\"schema_version\": 1},\n"
                                 "  \"text_recognition\": {\"direct_ml_acceleration\": false}\n"
                                 "}\n"));
    storage::ConfigurationStore store(config, true, true, 60000);
    require(store.value(QStringLiteral("text_recognition/model_type")).toString() ==
                    QStringLiteral("small") &&
                !store.value(QStringLiteral("text_recognition/direct_ml_acceleration")).toBool() &&
                store.value(QStringLiteral("storage/schema_version")).toInt() == 3 &&
                store.isDirty() && store.flushNow().success,
            "missing OCR model types must insert Small while preserving peer settings");
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
            "unknown schema fields were erased");
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
                                                .toInt() == 3,
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
                                         "  \"storage\": {\"schema_version\": 4},\n"
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
    require(!policy.keepPermanently, "permanent history must default to off");
    policy.maxEntries = 2;
    policy.keepPermanently = true;
    const auto policyResult = applicationStorage.requestCaptureHistoryPolicyAsync(policy);
    require(policyResult.valid() && policyResult.get().success,
            "asynchronous policy mutation did not complete successfully");
    QCoreApplication::processEvents();
    require(!applicationStorage.status().historyPolicyUpdating,
            "policy mutation remained busy after completion");
    require(applicationStorage.captureHistoryPolicy() == policy &&
                applicationStorage.configuration()
                    .value(QStringLiteral("capture_history/keep_permanently"))
                    .toBool(),
            "permanent history policy must update the repository and configuration");
    require(applicationStorage.flushNow().success, "flush permanent history configuration");
    applicationStorage.shutdown();
    initialize(executable, temporary.path(), 60000);
    require(applicationStorage.captureHistoryPolicy() == policy,
            "permanent history and existing limits must survive restart");

    const auto clearResult = applicationStorage.requestCaptureHistoryClearAsync();
    require(clearResult.valid() && clearResult.get().success,
            "asynchronous history clear did not complete successfully");
    QCoreApplication::processEvents();
    require(!applicationStorage.status().historyClearing,
            "history clear remained busy after completion");
}

void pendingClosedPinsReceiveBackgroundRetentionCleanup() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create pin retention directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "failed to create pin retention executable directory");
    auto& applicationStorage = initialize(executable, temporary.path(), 60000);
    auto& repository = applicationStorage.pinnedWindows();
    auto policy = repository.policy();
    policy.maxEntries = 1;
    require(repository.setPolicy(policy).success, "set closed-pin retention limit");

    const auto makeRecord = [](const QString& id) {
        storage::PinnedWindowRecord record;
        record.id = id;
        record.sourceKind = storage::PinnedWindowSourceKind::ClipboardText;
        record.originalText = QStringLiteral("Pinned text");
        record.nativeGeometry = QRect(0, 0, 2, 2);
        record.canvasSourceRect = QRectF(record.nativeGeometry);
        record.contentCanvasRect = record.canvasSourceRect;
        record.surfaceCanvasRect = record.canvasSourceRect;
        record.initialWindowSize = record.nativeGeometry.size();
        return record;
    };
    const QString firstId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString secondId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    repository.reserveCreation(firstId);
    repository.reserveCreation(secondId);
    require(repository.markClosedDeferred(firstId).success &&
                repository.markClosedDeferred(secondId).success,
            "record closes before their sources are saved");
    require(repository.createReserved(makeRecord(firstId)).success &&
                repository.createReserved(makeRecord(secondId)).success &&
                repository.summaries().size() == 2,
            "pending closed sources are retained until cleanup runs");

    applicationStorage.requestPinnedWindowRetentionCleanup();
    QElapsedTimer timer;
    timer.start();
    while (repository.summaries().size() != 1 && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto records = repository.summaries();
    require(records.size() == 1 && records.front().id == secondId && records.front().ignored,
            "background cleanup prunes the oldest pending closed pin");

    const QString thirdId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    require(repository.upsert(makeRecord(thirdId)).success &&
                repository.markClosedDeferred(thirdId).success &&
                repository.loadRecord(thirdId)->ignored,
            "ordinary close state is visible before background cleanup");
    applicationStorage.requestPinnedWindowRetentionCleanup();
    applicationStorage.shutdown();
    auto& reopened = initialize(executable, temporary.path(), 60000);
    const auto persisted = reopened.pinnedWindows().summaries();
    require(persisted.size() == 1 && persisted.front().id == thirdId,
            "shutdown drains pending pin retention cleanup before flushing");
    reopened.shutdown();
}

void appUsageScanAndCacheCleanup() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create app usage directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("root"));
    auto& applicationStorage = initialize(executable, root, 60000);
    require(applicationStorage.flushNow().success, "initial flush must succeed");
    require(!applicationStorage.status().appUsage.scanning,
            "a flushed storage must not report scanning");

    // The cache locations come from QStandardPaths; the test process uses a
    // dedicated organization and application name, so they belong to this run.
    const QString thumbnailCache = storage::StorageUsageTracker::defaultThumbnailCacheDirectory();
    const QString recordingTemp = storage::StorageUsageTracker::defaultRecordingTempDirectory();
    QDir(thumbnailCache).removeRecursively();
    QDir(recordingTemp).removeRecursively();
    const QString thumbnail = QDir(thumbnailCache).filePath(QStringLiteral("probe.png"));
    const QString staleRecording = QDir(recordingTemp).filePath(QStringLiteral("stale.pcm"));
    const QString activeRecording = QDir(recordingTemp).filePath(QStringLiteral("active.pcm"));
    writeBytes(thumbnail, QByteArray(64, 'x'));
    writeBytes(staleRecording, QByteArray(32, 'x'));
    writeBytes(activeRecording, QByteArray(48, 'x'));
    setLastModified(staleRecording, QDateTime::currentDateTime().addSecs(-7200));
    setLastModified(activeRecording, QDateTime::currentDateTime().addSecs(3600));
    writeBytes(QDir(root).filePath(QStringLiteral("capture_history/records/dummy/display.png")),
               QByteArray(200, 'x'));
    writeBytes(QDir(root).filePath(QStringLiteral("pinned_windows_v2/index.json")),
               QByteArray(30, 'x'));
    writeBytes(QDir(root).filePath(QStringLiteral("assets/ocr/model.bin")), QByteArray(150, 'x'));

    applicationStorage.requestStorageUsageRefresh();
    require(applicationStorage.flushNow().success, "post-scan flush must succeed");
    const storage::StorageStatus scanned = applicationStorage.status();
    require(!scanned.appUsage.scanning, "a refreshed storage must not report scanning");
    // History bytes must come from the repository's incrementally maintained
    // usage, so the dummy record materialized behind its back stays uncounted.
    require(scanned.appUsage.historyBytes == scanned.historyUsage.totalBytes,
            "history bytes must mirror the capture-history repository usage");
    require(scanned.appUsage.historyBytes == 0,
            "records written behind the repository must not be counted until reconcile");
    require(scanned.appUsage.pinnedWindowBytes == 30,
            "pinned window bytes must match the test payload");
    require(scanned.appUsage.ocrAssetBytes == 150, "ocr asset bytes must match the test payload");
    require(scanned.appUsage.thumbnailCacheBytes == 64,
            "thumbnail cache bytes must match the test payload");
    require(scanned.appUsage.recordingTempBytes == 80,
            "recording temp bytes must match the test payloads");
    require(scanned.appUsage.otherBytes > 0,
            "other bytes must cover the materialized configuration");
    require(scanned.appUsage.totalBytes() ==
                scanned.appUsage.historyBytes + 30 + 150 + 64 + 80 + scanned.appUsage.otherBytes,
            "total app usage must be the sum of all categories");

    const auto thumbnailClear = applicationStorage.requestThumbnailCacheClearAsync();
    require(thumbnailClear.valid() && thumbnailClear.get().success,
            "thumbnail cache clear did not complete successfully");
    require(applicationStorage.flushNow().success, "post-clear flush must succeed");
    QCoreApplication::processEvents();
    require(!applicationStorage.status().cacheClearing,
            "cache clear remained busy after completion");
    require(!QFile::exists(thumbnail), "thumbnail cache clear left files behind");
    require(applicationStorage.status().appUsage.thumbnailCacheBytes == 0,
            "thumbnail cache bytes must be zero after the clear");

    const auto recordingClear = applicationStorage.requestRecordingTempClearAsync();
    require(recordingClear.valid() && recordingClear.get().success,
            "recording temp clear did not complete successfully");
    require(applicationStorage.flushNow().success, "post-recording-clear flush must succeed");
    QCoreApplication::processEvents();
    require(!QFile::exists(staleRecording), "recording temp clear left stale files behind");
    require(QFile::exists(activeRecording), "recording temp clear must keep active-session files");
    require(applicationStorage.status().appUsage.recordingTempBytes == 48,
            "recording temp bytes must only cover the active session after the clear");
}
} // namespace

class LifetimeObservedApplication final : public QCoreApplication {
  public:
    using QCoreApplication::QCoreApplication;

    int quitObserverCount() const {
        return receivers(SIGNAL(aboutToQuit()));
    }
};

void applicationQuitPreservesStorageForConsumerDestruction(
    LifetimeObservedApplication& application) {
    QTemporaryDir temporary;
    require(temporary.isValid(), "quit-lifetime storage directory unavailable");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    const storage::StorageInitializationOptions options{temporary.filePath(QStringLiteral("bin")),
                                                        temporary.path(), 60000};
    applicationStorage.shutdown();
    int unrelatedQuitNotifications = 0;
    QObject unrelatedObserver;
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &unrelatedObserver,
                     [&unrelatedQuitNotifications] { ++unrelatedQuitNotifications; });
    const int originalObservers = application.quitObserverCount();
    for (int iteration = 0; iteration < 8; ++iteration) {
        require(applicationStorage.initialize(options).success,
                "repeated quit-lifetime initialization must succeed");
        require(application.quitObserverCount() == originalObservers + 1,
                "initialized storage must own exactly one application quit observer");
        require(applicationStorage.initialize(options).success,
                "quit-lifetime storage must support reinitialization");
        require(application.quitObserverCount() == originalObservers + 1,
                "reinitializing storage must replace its application quit observer");
        applicationStorage.shutdown();
        require(application.quitObserverCount() == originalObservers,
                "storage shutdown must release only its application quit observer");
        applicationStorage.shutdown();
        require(application.quitObserverCount() == originalObservers,
                "repeated storage shutdown must preserve unrelated quit observers");
    }
    require(applicationStorage.initialize(options).success,
            "quit-lifetime storage must initialize");
    auto* history = &applicationStorage.captureHistory();
    auto* pins = &applicationStorage.pinnedWindows();
    auto* configuration = &applicationStorage.configuration();
    require(storage::ScreenshotSettings().setShowCursor(true), "pending settings must be accepted");
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
    QCoreApplication::exec();
    require(unrelatedQuitNotifications == 1,
            "storage lifecycle changes must preserve unrelated application quit callbacks");
    require(applicationStorage.isInitialized(),
            "aboutToQuit must preserve initialized storage until consumers are destroyed");
    require(storage::ScreenshotSettings().showCursor(), "destructors must still read settings");
    require(&applicationStorage.captureHistory() == history &&
                &applicationStorage.pinnedWindows() == pins &&
                &applicationStorage.configuration() == configuration,
            "settings reads after quit must not replace repositories held by consumers");
    const auto saved = readObject(temporary.filePath(QStringLiteral("config.json")));
    require(saved.value(QStringLiteral("screenshot"))
                .toObject()
                .value(QStringLiteral("capture_cursor"))
                .toBool(),
            "aboutToQuit must flush pending settings before the event loop exits");
    applicationStorage.shutdown();
    require(application.quitObserverCount() == originalObservers,
            "shutdown after quit must release its application quit observer");
}

void invalidTrayClickSettingsUseIndependentDefaults() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary invalid tray settings directory");
    const QString config = temporary.filePath(QStringLiteral("config.json"));
    writeBytes(
        config,
        QByteArrayLiteral(
            R"({"storage":{"schema_version":1},"tray":{"left_click_action":"invalid","middle_click_action":"invalid"}})"));
    storage::ConfigurationStore store(config, true, true, 60000);
    require(store.value(QStringLiteral("tray/left_click_action")).toString() ==
                    QStringLiteral("screenshot") &&
                store.value(QStringLiteral("tray/middle_click_action")).toString() ==
                    QStringLiteral("screenshot_fixed") &&
                store.isDirty() && store.flushNow().success,
            "invalid persisted click actions must be repaired to their independent defaults");
}

void legacyTrayHotkeyCommandMigratesToQuickAction() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create tray menu migration directory");
    const QString config = temporary.filePath(QStringLiteral("config.json"));
    writeBytes(
        config,
        QByteArrayLiteral(
            R"({"storage":{"schema_version":2},"tray":{"menu_options":["quick.screenshot","tray.window-grouping","tray.disable-shortcut-functions","tray.exit"]}})"));
    const QJsonArray migratedOptions{
        QStringLiteral("quick.screenshot"), QStringLiteral("tray.window-grouping"),
        QStringLiteral("quick.toggle-global-hotkeys"), QStringLiteral("tray.exit")};
    {
        storage::ConfigurationStore store(config, true, true, 60000);
        require(store.value(QStringLiteral("tray/menu_options")).toArray() == migratedOptions &&
                    store.isDirty() && store.flushNow().success,
                "the legacy tray hotkey command must migrate in place to the toggle quick action");
    }
    QFile persisted(config);
    require(persisted.open(QIODevice::ReadOnly), "the migrated config must be readable");
    const QJsonObject persistedRoot = QJsonDocument::fromJson(persisted.readAll()).object();
    persisted.close();
    require(persistedRoot.value(QStringLiteral("tray"))
                    .toObject()
                    .value(QStringLiteral("menu_options"))
                    .toArray() == migratedOptions,
            "the rename must be written back so it is not re-derived on every load");
    storage::ConfigurationStore reloaded(config, true, true, 60000);
    require(reloaded.value(QStringLiteral("tray/menu_options")).toArray() == migratedOptions,
            "the migrated tray menu must survive a storage reload");
}

void trayClickSettingsSurviveRestart() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary tray settings directory");
    const QString executable = temporary.filePath(QStringLiteral("app"));
    QDir().mkpath(executable);
    auto& applicationStorage = initialize(executable, temporary.path());
    const storage::TraySettings tray;
    require(tray.leftClickAction() == QStringLiteral("screenshot") &&
                tray.middleClickAction() == QStringLiteral("screenshot_fixed"),
            "missing tray settings must use independent defaults");
    require(tray.setLeftClickAction(QStringLiteral("screenshot_copy")) &&
                tray.setMiddleClickAction(QStringLiteral("open_function_settings")) &&
                tray.setMenuOptions({QStringLiteral("tray.show-main-window"),
                                     QStringLiteral("tray.restart-app"),
                                     QStringLiteral("tray.exit")}) &&
                applicationStorage.flushNow().success,
            "persist distinct tray click actions and opt-in menu commands");
    static_cast<void>(initialize(executable, temporary.path()));
    require(tray.leftClickAction() == QStringLiteral("screenshot_copy") &&
                tray.middleClickAction() == QStringLiteral("open_function_settings") &&
                tray.menuOptions() == QStringList{QStringLiteral("tray.show-main-window"),
                                                  QStringLiteral("tray.restart-app"),
                                                  QStringLiteral("tray.exit")},
            "tray click choices and Restart App must survive storage restart");
    applicationStorage.shutdown();
}

void watermarkContentSettingsRepairAndSurviveRestart() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary watermark-content settings directory");
    const QString executable = temporary.filePath(QStringLiteral("app"));
    require(QDir().mkpath(executable), "create watermark-content executable directory");
    auto& applicationStorage = initialize(executable, temporary.path());
    const storage::WatermarkContentSettings settings;
    require(settings.content() == storage::WatermarkContent{},
            "missing watermark content defaults to empty text and template");
    const storage::WatermarkContent expected{QStringLiteral("版权 © 雪"),
                                             QStringLiteral("  {text}\t{YYYY-MM-DD_HH-mm-ss}  ")};
    require(settings.setContent(expected) && applicationStorage.flushNow().success,
            "watermark content saves Unicode and exact template whitespace");
    static_cast<void>(initialize(executable, temporary.path()));
    require(settings.content() == expected, "watermark content survives application restart");
    const QString key = QStringLiteral("drawing/watermark_content");
    const auto normalized = storage::ConfigurationSchema::normalize(
        key, QJsonObject{{QStringLiteral("text"), 42},
                         {QStringLiteral("template_value"), expected.templateValue},
                         {QStringLiteral("template_application_time"), QJsonObject()}});
    require(normalized.valid && normalized.changed && normalized.value.toObject().size() == 2 &&
                normalized.value.toObject().value(QStringLiteral("text")).toString().isEmpty() &&
                normalized.value.toObject().value(QStringLiteral("template_value")).toString() ==
                    expected.templateValue &&
                !storage::ConfigurationSchema::normalize(key, QJsonArray()).valid,
            "invalid content fields repair independently and session timestamps are excluded");
    require(applicationStorage.configuration().setValue(key, normalized.value) &&
                settings.content() == storage::WatermarkContent{QString(), expected.templateValue},
            "typed content adapter reads repaired fields");
    require(settings.setContent({}) && applicationStorage.flushNow().success,
            "cleared watermark content saves empty values");
    static_cast<void>(initialize(executable, temporary.path()));
    require(settings.content() == storage::WatermarkContent{},
            "cleared watermark content remains empty after restart");
    applicationStorage.shutdown();
}

void watermarkTemplateSettingsRepairAndSurviveRestart() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary watermark-template settings directory");
    const QString executable = temporary.filePath(QStringLiteral("app"));
    require(QDir().mkpath(executable), "failed to create watermark-template executable directory");
    const QString config = temporary.filePath(QStringLiteral("config.json"));
    writeBytes(
        config,
        QByteArrayLiteral(
            R"({"storage":{"schema_version":1},"drawing":{"watermark_templates":[{"name":"  Release  ","value":"  {text} {YYYY}  ","extra":true},7,{"name":"   ","value":"x"},{"name":"Bad","value":42},{"name":"Whitespace","value":"   "},{"name":"Release","value":"  {text} {YYYY}  "},{"name":"Release","value":"{DD}"}]}})"));

    auto& applicationStorage = initialize(executable, temporary.path());
    const storage::WatermarkTemplateSettings settings;
    const QVector<storage::WatermarkTemplate> repaired = settings.templates();
    require(repaired ==
                QVector<storage::WatermarkTemplate>{
                    {QStringLiteral("Release"), QStringLiteral("  {text} {YYYY}  ")},
                    {QStringLiteral("Release"), QStringLiteral("  {text} {YYYY}  ")},
                    {QStringLiteral("Release"), QStringLiteral("{DD}")},
                },
            "watermark-template repair must preserve valid order, duplicates, and exact values");
    require(applicationStorage.flushNow().success,
            "repaired watermark-template settings must flush");

    const QJsonArray stored = readObject(config)
                                  .value(QStringLiteral("drawing"))
                                  .toObject()
                                  .value(QStringLiteral("watermark_templates"))
                                  .toArray();
    require(stored.size() == 3 && stored.at(0).toObject().size() == 2 &&
                stored.at(0).toObject().value(QStringLiteral("name")).toString() ==
                    QStringLiteral("Release") &&
                stored.at(0).toObject().value(QStringLiteral("value")).toString() ==
                    QStringLiteral("  {text} {YYYY}  "),
            "watermark-template repair must persist a canonical JSON array");

    require(settings.setTemplates({
                {QStringLiteral("  Duplicate  "), QStringLiteral(" {text} ")},
                {QStringLiteral("Duplicate"), QStringLiteral(" {text} ")},
                {QStringLiteral("Invalid"), QStringLiteral(" \t ")},
                {QStringLiteral("   "), QStringLiteral("{YYYY}")},
            }) &&
                applicationStorage.flushNow().success,
            "watermark-template settings must save valid entries");
    static_cast<void>(initialize(executable, temporary.path()));
    require(settings.templates() ==
                QVector<storage::WatermarkTemplate>{
                    {QStringLiteral("Duplicate"), QStringLiteral(" {text} ")},
                    {QStringLiteral("Duplicate"), QStringLiteral(" {text} ")},
                },
            "watermark-template settings must survive restart without deduplicating");
    applicationStorage.shutdown();
}

void drawTemplateSettingsRepairAndSurviveRestart() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary draw-template settings directory");
    const QString executable = temporary.filePath(QStringLiteral("app"));
    require(QDir().mkpath(executable), "failed to create draw-template executable directory");
    const QString config = temporary.filePath(QStringLiteral("config.json"));
    const QByteArray payload =
        QByteArrayLiteral(R"({"schemaVersion":1,"selectedIds":[1],"elements":[1]})");
    const QString encoded = QString::fromLatin1(payload.toBase64());
    writeBytes(
        config,
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("storage"), QJsonObject{{QStringLiteral("schema_version"), 1}}},
                {QStringLiteral("drawing"),
                 QJsonObject{
                     {QStringLiteral("draw_templates"),
                      QJsonArray{
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("  Mark  ")},
                                      {QStringLiteral("payload"), encoded},
                                      {QStringLiteral("extra"), 1}},
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("Mark")},
                                      {QStringLiteral("payload"), encoded}},
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("Bad")},
                                      {QStringLiteral("payload"), QStringLiteral("!invalid!")}},
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("  ")},
                                      {QStringLiteral("payload"), encoded}},
                      }}}},
            })
            .toJson(QJsonDocument::Compact));

    auto& applicationStorage = initialize(executable, temporary.path());
    const storage::DrawTemplateSettings settings;
    require(settings.templates() ==
                QVector<storage::DrawTemplate>{{QStringLiteral("Mark"), payload},
                                               {QStringLiteral("Mark"), payload}},
            "draw-template repair must retain order and duplicate names");
    require(applicationStorage.flushNow().success, "repaired draw-template settings must flush");
    const QJsonArray stored = readObject(config)
                                  .value(QStringLiteral("drawing"))
                                  .toObject()
                                  .value(QStringLiteral("draw_templates"))
                                  .toArray();
    require(stored.size() == 2 && stored.at(0).toObject().size() == 2,
            "draw-template repair must discard malformed entries and extra fields");
    require(!settings.setTemplates({{QStringLiteral("Invalid"), QByteArrayLiteral("no")}}),
            "draw-template settings must reject malformed payloads");
    require(settings.setTemplates(
                {{QStringLiteral("  First  "), payload}, {QStringLiteral("First"), payload}}) &&
                applicationStorage.flushNow().success,
            "draw-template settings must persist valid entries");
    static_cast<void>(initialize(executable, temporary.path()));
    require(settings.templates() ==
                QVector<storage::DrawTemplate>{{QStringLiteral("First"), payload},
                                               {QStringLiteral("First"), payload}},
            "draw templates must survive restart without deduplicating");
    applicationStorage.shutdown();
}

void pinnedManagementConfigurationAndTrayMigration() {
    QTemporaryDir directory;
    const auto defaults =
        storage::ConfigurationSchema::defaultValue(QStringLiteral("tray/menu_options")).toArray();
    require(defaults.contains(QStringLiteral("quick.restore-last-closed-windows")),
            "restore is visible in the default tray");
    auto previous = defaults;
    for (qsizetype i = previous.size(); i > 0; --i)
        if (previous.at(i - 1).toString() == QStringLiteral("quick.restore-last-closed-windows"))
            previous.removeAt(i - 1);
    const auto write = [&](const QString& name, const QJsonArray& menu) {
        QFile file(directory.filePath(name));
        require(file.open(QIODevice::WriteOnly), "create tray migration fixture");
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("storage"),
                                              QJsonObject{{QStringLiteral("schema_version"), 2}}},
                                             {QStringLiteral("tray"),
                                              QJsonObject{{QStringLiteral("menu_options"), menu}}}})
                       .toJson());
    };
    write(QStringLiteral("default.json"), previous);
    storage::ConfigurationStore migrated(directory.filePath(QStringLiteral("default.json")), true,
                                         true, 30000);
    require(migrated.value(QStringLiteral("tray/menu_options")).toArray() == defaults,
            "previous default tray receives restore action");
    auto customized = previous;
    customized.removeAt(0);
    write(QStringLiteral("custom.json"), customized);
    storage::ConfigurationStore retained(directory.filePath(QStringLiteral("custom.json")), true,
                                         true, 30000);
    require(retained.value(QStringLiteral("tray/menu_options")).toArray() == customized,
            "customized tray menu remains unchanged");
    require(migrated.value(QStringLiteral("pinned_history/enabled")).toBool() &&
                migrated.value(QStringLiteral("pinned_history/retention_days")).toInt() == 7 &&
                migrated.value(QStringLiteral("pinned_history/max_entries")).toInt() == 100 &&
                migrated.value(QStringLiteral("pinned_history/max_disk_mib")).toInt() == 1024,
            "pin history defaults match screenshot history limits");
    const auto shortcut =
        migrated.value(QStringLiteral("global_shortcuts/restore_last_closed_windows")).toArray();
#ifdef Q_OS_MACOS
    require(shortcut == QJsonArray{shortcutObject(QStringLiteral("Meta+Shift+3"), 20)},
            "macOS restores with physical Control Shift 3");
#else
    require(shortcut == QJsonArray{shortcutObject(QStringLiteral("Ctrl+F3"))},
            "Windows restore defaults to Ctrl F3");
#endif
}

void recordingGainSettingsPersistAndValidate() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "audio gain storage fixture exists");
    const QString executable = temporary.filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "audio gain executable directory exists");
    auto& appStorage = initialize(executable, temporary.filePath(QStringLiteral("settings")));
    const storage::RecordingSettings settings;
    require(settings.microphoneGainDb() == 0 && settings.systemAudioGainDb() == 0,
            "both new and legacy recording gains default to zero");
    require(settings.setMicrophoneGainDb(-24) && settings.setSystemAudioGainDb(24),
            "independent gains accept symmetric endpoints");
    require(!settings.setMicrophoneGainDb(-25) && !settings.setSystemAudioGainDb(25) &&
                settings.microphoneGainDb() == -24 && settings.systemAudioGainDb() == 24,
            "out of range gains reject without replacing values");
    require(appStorage.configuration().flushNow().success, "gain preferences flush");
    initialize(executable, temporary.filePath(QStringLiteral("settings")));
    require(settings.microphoneGainDb() == -24 && settings.systemAudioGainDb() == 24,
            "independent gain preferences survive restart");
    require(appStorage.configuration().setValues(
                {{QStringLiteral("screen_recording/microphone_gain_db"),
                  storage::ConfigurationSchema::defaultValue(
                      QStringLiteral("screen_recording/microphone_gain_db"))},
                 {QStringLiteral("screen_recording/system_audio_gain_db"),
                  storage::ConfigurationSchema::defaultValue(
                      QStringLiteral("screen_recording/system_audio_gain_db"))}}) &&
                settings.microphoneGainDb() == 0 && settings.systemAudioGainDb() == 0,
            "gain defaults restore unity independently");
    appStorage.shutdown();
}

int main(int argc, char** argv) {
#ifdef Q_OS_MACOS
    if (argc == 2 && QByteArray(argv[1]) == "--directory-resolution-only") {
        require(QCoreApplication::instance() == nullptr,
                "startup directory resolution runs before the Qt application exists");
        const QString expected = QFileInfo(QString::fromLocal8Bit(argv[0])).canonicalPath();
        QTemporaryDir temporary;
        require(temporary.isValid(), "startup directory resolution needs isolated storage");
        require(QDir::setCurrent(temporary.path()), "change to an unrelated launch directory");
        const storage::StorageInitializationOptions options{
            {}, temporary.filePath(QStringLiteral("data")), 60000};
        const auto early = storage::ApplicationStorage::resolveDirectory(options);
        require(early.executableDirectory == expected,
                "pre-application startup resolves the executable rather than the launch directory");
        require(early.effectiveDirectory == options.appDataDirectory,
                "pre-application resolution uses the requested isolated storage");
        LifetimeObservedApplication application(argc, argv);
        require(early.executableDirectory == QCoreApplication::applicationDirPath() &&
                    storage::ApplicationStorage::resolveDirectory(options).executableDirectory ==
                        early.executableDirectory,
                "executable directory stays stable across Qt application initialization");
        const QString explicitDirectory = temporary.filePath(QStringLiteral("bin"));
        require(storage::ApplicationStorage::resolveDirectory(
                    {explicitDirectory, options.appDataDirectory, 60000})
                        .executableDirectory == explicitDirectory,
                "explicit executable directories remain authoritative");
        return 0;
    }
#endif
    LifetimeObservedApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--update-settings-only"))) {
        updateSettingsPersistAndValidate();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--recording-audio-only"))) {
        recordingGainSettingsPersistAndValidate();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--recording-post-processing-only"))) {
        recordingPostProcessingPreferencesPersistAndValidate();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--scrolling-interval-only"))) {
        scrollingIntervalSettingsPersistAndValidate();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--selection-aspect-ratio-only"))) {
        selectionAspectRatioSettingsValidateAndPreserveLegacyLock();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--global-mouse-only"))) {
        globalMouseCombinationSchemaIsStrictAndPersistent();
        return 0;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("storage-tests"));
    if (application.arguments().contains(QStringLiteral("--shortcut-settings-only"))) {
        settingsSchemaDefaultsAndValidationAreComplete();
        settingsAdaptersRoundTripAndRejectInvalidValues();
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--quit-lifetime-only"))) {
        applicationQuitPreservesStorageForConsumerDestruction(application);
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--toolbar-layout-only"))) {
        screenshotUiSchemaRepairsStructuredValues();
        screenshotUiAdaptersRoundTripTypedValues();
        recordingActionToolbarLayoutRepairsAndPersistsIndependently();
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--image-conversion-only"))) {
        screenshotUiSchemaRepairsStructuredValues();
        screenshotUiAdaptersRoundTripTypedValues();
        screenshotTranslationSettingsRoundTripSupportedValues();
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--draw-template-only"))) {
        drawTemplateSettingsRepairAndSurviveRestart();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--watermark-content-only"))) {
        watermarkContentSettingsRepairAndSurviveRestart();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--pin-shortcuts-only"))) {
        settingsSchemaDefaultsAndValidationAreComplete();
        pinnedDestroyShortcutMigratesPreviousDefault();
        pinToScreenShortcutSettingsRoundTrip();
        obsoleteClickThroughShortcutIsIgnored();
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    pinnedManagementConfigurationAndTrayMigration();
    updateSettingsPersistAndValidate();
    markerResolutionAndStatus();
    defaultsAndTypedRoundTrip();
    selectionAspectRatioSettingsValidateAndPreserveLegacyLock();
    settingsSchemaDefaultsAndValidationAreComplete();
    pinnedDestroyShortcutMigratesPreviousDefault();
    obsoleteClickThroughShortcutIsIgnored();
    shortcutSchemaMigrationAndPhysicalMetadataRoundTrip();
    invalidTrayClickSettingsUseIndependentDefaults();
    legacyTrayHotkeyCommandMigratesToQuickAction();
    trayClickSettingsSurviveRestart();
    watermarkTemplateSettingsRepairAndSurviveRestart();
    watermarkContentSettingsRepairAndSurviveRestart();
    drawTemplateSettingsRepairAndSurviveRestart();
    globalMouseCombinationSchemaIsStrictAndPersistent();
    screenshotUiSchemaRepairsStructuredValues();
    screenshotUiAdaptersRoundTripTypedValues();
    recordingActionToolbarLayoutRepairsAndPersistsIndependently();
    screenshotTranslationSettingsRoundTripSupportedValues();
    settingsAdaptersRoundTripAndRejectInvalidValues();
    recordingPostProcessingPreferencesPersistAndValidate();
    invalidCaptureCursorConfigurationFallsBackToDisabled();
    invalidOcrModelConfigurationFallsBackToSmallWithoutAMigration();
    missingOcrModelConfigurationDefaultsToSmallWithoutAMigration();
    smartSelectionAccessorAndSignal();
    unknownFieldsArePreserved();
    malformedConfigurationIsCopiedAndReplaced();
    futureVersionIsReadOnly();
    failedWriteCanBeRetried();
    concurrentFlushKeepsLatestRevision();
    persistedSelectionCodecIsCanonicalAndStrict();
    asynchronousMutationResultsAreObservable();
    pendingClosedPinsReceiveBackgroundRetentionCleanup();
    appUsageScanAndCacheCleanup();
    applicationQuitPreservesStorageForConsumerDestruction(application);
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
