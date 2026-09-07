#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/screenshotselectorcoordinator.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_ui_selector.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

struct SnowUiSelectorServiceImpl {};

namespace {
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

int created = 0;
int destroyed = 0;
int refreshed = 0;
SnowUiSelectorBackend currentBackend = SNOW_UI_SELECTOR_BACKEND_MSAA;
SnowUiSelectorHitTestMode currentMode = SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW;
QVector<std::uintptr_t> currentExclusions;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void setApi(const QString& api) {
    require(storage::ScreenshotSettings().setWindowElementApi(api),
            "failed to change the window element API");
}

void settingsPersistAndResetToMsaa(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto binding = settings::SettingsSelectBinding::WindowElementApi;
    constexpr auto cursorBinding = settings::SettingsSwitchBinding::ScreenshotCaptureCursor;
    require(backend.selectValue(binding) == QStringLiteral("msaa"),
            "Window Element API must initially select MSAA");
    require(!backend.switchValue(cursorBinding) && backend.applySwitchValue(cursorBinding, true) &&
                backend.switchValue(cursorBinding),
            "the settings backend must apply and read cursor capture");
    require(backend.applySelectValue(binding, QStringLiteral("uia")) &&
                backend.selectValue(binding) == QStringLiteral("uia"),
            "the settings backend must apply and read UIA");
    require(!backend.applySelectValue(binding, QStringLiteral("unknown")) &&
                backend.selectValue(binding) == QStringLiteral("uia"),
            "invalid API choices must preserve the accepted value");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "window element API must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(reloaded.value(QStringLiteral("screenshot/window_element_api")) ==
                QStringLiteral("uia"),
            "window element API must survive a configuration reload");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotCapture) &&
                backend.selectValue(binding) == QStringLiteral("msaa") &&
                !backend.switchValue(cursorBinding),
            "resetting system Screenshot settings must restore MSAA and disable cursor capture");
    const auto invalid = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot/window_element_api"), QStringLiteral("unknown"));
    require(!invalid.valid, "the schema must reject unsupported window element APIs");
    const QString invalidPath = configurationPath + QStringLiteral(".invalid");
    QFile invalidFile(invalidPath);
    require(invalidFile.open(QIODevice::WriteOnly), "failed to create invalid configuration");
    const QByteArray invalidDocument =
        QJsonDocument(QJsonObject{{QStringLiteral("screenshot"),
                                   QJsonObject{{QStringLiteral("window_element_api"),
                                                QStringLiteral("unknown")}}}})
            .toJson();
    require(invalidFile.write(invalidDocument) == invalidDocument.size(),
            "failed to write invalid configuration");
    invalidFile.close();
    storage::ConfigurationStore repaired(invalidPath, true, true, 60000);
    require(repaired.value(QStringLiteral("screenshot/window_element_api")) ==
                QStringLiteral("msaa"),
            "invalid stored API values must fall back to MSAA");
}

void shutterSoundSettingsPersistAndReset(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto binding = settings::SettingsSwitchBinding::ScreenshotShutterSoundNotification;
    require(backend.switchValue(binding), "shutter notification must default to enabled");
    require(backend.applySwitchValue(binding, false) && !backend.switchValue(binding) &&
                !storage::ScreenshotSettings().shutterSoundNotification(),
            "shutter notification must be disabled through the settings backend");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "shutter preference must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(!reloaded.value(QStringLiteral("screenshot/shutter_sound_notification")).toBool(),
            "disabled shutter preference must survive a configuration reload");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotCapture) &&
                !backend.switchValue(binding),
            "system screenshot reset must preserve the function shutter preference");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotSettings) &&
                backend.switchValue(binding),
            "function screenshot reset must restore shutter notification to enabled");
    const auto invalid = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot/shutter_sound_notification"), QStringLiteral("enabled"));
    require(!invalid.valid, "shutter preference must reject nonboolean values");
}

void toolbarLayoutSectionResetsRemainIndependent() {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const storage::ScreenshotToolbarLayout drawingLayout{
        {{QStringLiteral("watermark")}},
        {QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("line"),
         QStringLiteral("free-draw"), QStringLiteral("highlighter"), QStringLiteral("spotlight"),
         QStringLiteral("text"), QStringLiteral("serial-number"), QStringLiteral("filter"),
         QStringLiteral("eraser")},
    };
    const storage::ScreenshotToolbarLayout actionLayout{
        {{QStringLiteral("save-as-file")}},
        {QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition"),
         QStringLiteral("record-screen"), QStringLiteral("pin-to-screen"),
         QStringLiteral("text-recognition"), QStringLiteral("text-translation"),
         QStringLiteral("scrolling-screenshot")},
    };
    require(backend.applyToolbarLayout(storage::ScreenshotToolbarLayoutKind::DrawingTools,
                                       drawingLayout) &&
                backend.applyToolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools,
                                           actionLayout),
            "toolbar reset fixture must persist independent layouts");

    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotInterfaceSettings) &&
                backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::DrawingTools) ==
                    drawingLayout &&
                backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools) ==
                    storage::ScreenshotToolbarLayout{{{QStringLiteral("barcode-recognition"),
                                                       QStringLiteral("table-recognition")},
                                                      {QStringLiteral("record-screen")},
                                                      {QStringLiteral("pin-to-screen")},
                                                      {QStringLiteral("text-recognition")},
                                                      {QStringLiteral("text-translation")},
                                                      {QStringLiteral("scrolling-screenshot")},
                                                      {QStringLiteral("save-as-file")}},
                                                     {}},
            "Screenshot Interface reset must restore only the screenshot action layout");

    require(backend.applyToolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools,
                                       actionLayout) &&
                backend.resetSection(settings::SettingsSectionReset::DrawingToolbar) &&
                backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools) ==
                    actionLayout &&
                backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::DrawingTools) ==
                    storage::ScreenshotToolbarLayout{
                        {{QStringLiteral("shape")},
                         {QStringLiteral("line"), QStringLiteral("arrow")},
                         {QStringLiteral("free-draw")},
                         {QStringLiteral("spotlight"), QStringLiteral("highlighter")},
                         {QStringLiteral("text")},
                         {QStringLiteral("serial-number")},
                         {QStringLiteral("filter")},
                         {QStringLiteral("eraser")},
                         {QStringLiteral("watermark")}},
                        {}},
            "Drawing reset must restore only the drawing toolbar layout");
}

void changedApiRefreshesServiceAndRejectsOldResults() {
    ScreenshotSelectorCoordinator coordinator;
    const QVector<std::uintptr_t> exclusions{123, 456};
    int hitResults = 0;
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::hitTestFinished, &coordinator,
                     [&hitResults](bool, const QVector<QRectF>&) { ++hitResults; });
    require(coordinator.startRefresh(exclusions), "initial selector refresh failed");
    QCoreApplication::sendPostedEvents();
    require(coordinator.ready() && currentBackend == SNOW_UI_SELECTOR_BACKEND_MSAA,
            "default smart selection must create an MSAA service");

    for (const auto& api : {QStringLiteral("uia"), QStringLiteral("msaa")}) {
        require(coordinator.requestHitTest(QPoint(10, 20),
                                           ScreenshotSelectorHitTestMode::WindowSubElement),
                "element hit test was not dispatched");
        require(currentMode == SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT,
                "smart selection must request child elements");
        const int previousCreated = created;
        const int previousDestroyed = destroyed;
        setApi(api);
        require(created == previousCreated + 1 && destroyed == previousDestroyed + 1 &&
                    currentBackend == (api == QStringLiteral("uia")
                                           ? SNOW_UI_SELECTOR_BACKEND_UIA
                                           : SNOW_UI_SELECTOR_BACKEND_MSAA) &&
                    currentExclusions == exclusions && coordinator.refreshInFlight() &&
                    !coordinator.ready() && !coordinator.hitTestInFlight(),
                "API changes must replace and refresh the service with the same excluded windows");
        QCoreApplication::sendPostedEvents();
        require(coordinator.ready() && hitResults == 0,
                "results queued by the previous API must be discarded");
    }
    require(
        coordinator.requestHitTest(QPoint(10, 20), ScreenshotSelectorHitTestMode::WindowSubElement),
        "new API must accept a hit test after refreshing");
    QCoreApplication::sendPostedEvents();
    require(hitResults == 1, "new API results must reach the coordinator");

    require(storage::ApplicationStorage::instance().requestSmartSelection(false),
            "failed to disable smart selection");
    QCoreApplication::sendPostedEvents();
    require(coordinator.requestHitTest(QPoint(10, 20),
                                       ScreenshotSelectorHitTestMode::WindowSubElement) &&
                currentMode == SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
            "disabled smart selection must still use window-only lookup");
    QCoreApplication::sendPostedEvents();
    require(storage::ApplicationStorage::instance().requestSmartSelection(true),
            "failed to restore smart selection");
}

void apiChangesDuringRefreshAndWhileIdle() {
    ScreenshotSelectorCoordinator coordinator;
    int refreshResults = 0;
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::refreshFinished, &coordinator,
                     [&refreshResults](bool) { ++refreshResults; });
    require(coordinator.startRefresh({789}), "selector refresh failed");
    setApi(QStringLiteral("uia"));
    QCoreApplication::sendPostedEvents();
    require(refreshResults == 1 && coordinator.ready() &&
                currentBackend == SNOW_UI_SELECTOR_BACKEND_UIA,
            "an API change during refresh must discard the previous refresh result");

    coordinator.releaseCache();
    const int previousCreated = created;
    const int previousRefreshed = refreshed;
    setApi(QStringLiteral("msaa"));
    require(created == previousCreated && refreshed == previousRefreshed && !coordinator.ready(),
            "changing the API while idle must defer service creation until the next capture");
    require(coordinator.startRefresh({789}) && currentBackend == SNOW_UI_SELECTOR_BACKEND_MSAA,
            "the next capture must use the API selected while idle");
    QCoreApplication::sendPostedEvents();
}

void diagnosticEnvironmentOverridesRemainAvailable() {
    setApi(QStringLiteral("uia"));
    qputenv("SNOW_SHOT_UI_SELECTOR_BACKEND", "msaa");
    {
        ScreenshotSelectorCoordinator coordinator;
        require(coordinator.startRefresh({}) && currentBackend == SNOW_UI_SELECTOR_BACKEND_MSAA,
                "legacy diagnostic override must take precedence over settings");
    }
    qputenv("SNOW_SHOT_SELECTOR_BACKEND", "uia");
    {
        ScreenshotSelectorCoordinator coordinator;
        require(coordinator.startRefresh({}) && currentBackend == SNOW_UI_SELECTOR_BACKEND_UIA,
                "primary diagnostic override must take precedence over the legacy override");
    }
    qunsetenv("SNOW_SHOT_SELECTOR_BACKEND");
    qunsetenv("SNOW_SHOT_UI_SELECTOR_BACKEND");
}
} // namespace

extern "C" {
SnowUiSelectorService* snow_ui_selector_service_create(SnowUiSelectorBackend backend) {
    ++created;
    currentBackend = backend;
    return new SnowUiSelectorService;
}

void snow_ui_selector_service_destroy(SnowUiSelectorService* service) {
    ++destroyed;
    delete service;
}

uint8_t snow_ui_selector_service_release_cache(SnowUiSelectorService*) {
    return 1;
}

uint8_t snow_ui_selector_service_refresh_async(SnowUiSelectorService*, uint64_t requestId,
                                               const uintptr_t* excludedHwnds, size_t excludedCount,
                                               SnowUiSelectorRefreshCallback callback,
                                               void* userdata) {
    ++refreshed;
    currentExclusions.clear();
    for (size_t index = 0; index < excludedCount; ++index) {
        currentExclusions.push_back(excludedHwnds[index]);
    }
    callback(requestId, 1, userdata);
    return 1;
}

uint8_t snow_ui_selector_service_hit_test_point_async(SnowUiSelectorService*, uint64_t requestId,
                                                      int32_t, int32_t,
                                                      SnowUiSelectorHitTestMode mode,
                                                      SnowUiSelectorHitTestPointCallback callback,
                                                      void* userdata) {
    currentMode = mode;
    callback(requestId, nullptr, 1, userdata);
    return 1;
}

size_t snow_ui_selector_hit_path_count(const SnowUiSelectorHitPath*) {
    return 0;
}

uint8_t snow_ui_selector_hit_path_rect(const SnowUiSelectorHitPath*, size_t, SnowUiSelectorRect*) {
    return 0;
}

void snow_ui_selector_hit_path_destroy(SnowUiSelectorHitPath*) {}
} // extern "C"

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    qunsetenv("SNOW_SHOT_SELECTOR_BACKEND");
    qunsetenv("SNOW_SHOT_UI_SELECTOR_BACKEND");
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create isolated settings directory");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    static_cast<void>(
        applicationStorage.initialize({temporary.filePath(QStringLiteral("bin")),
                                       temporary.filePath(QStringLiteral("data")), 60000}));
    settingsPersistAndResetToMsaa(temporary.filePath(QStringLiteral("data/config.json")));
    shutterSoundSettingsPersistAndReset(temporary.filePath(QStringLiteral("data/config.json")));
    toolbarLayoutSectionResetsRemainIndependent();
    changedApiRefreshesServiceAndRejectsOldResults();
    apiChangesDuringRefreshAndWhileIdle();
    diagnosticEnvironmentOverridesRemainAvailable();
    require(created == destroyed, "selector leaked a native service");
    applicationStorage.shutdown();
    return 0;
}
