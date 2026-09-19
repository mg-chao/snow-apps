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

struct SnowUiSelectorServiceImpl {
    SnowUiSelectorEventCallback event;
    SnowUiSelectorRefreshCallback refresh;
    void* userdata;
};

namespace {
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

bool automaticReply = true;
struct Submission {
    SnowUiSelectorService* service;
    SnowUiSelectorQuery query;
};
QVector<Submission> submissions;
QVector<Submission> refinements;
int invalidations = 0;
void deliver(const Submission& submission, SnowUiSelectorPhase phase = SNOW_UI_SELECTOR_INITIAL,
             SnowUiSelectorStopReason reason = SNOW_UI_SELECTOR_BUDGET_EXHAUSTED) {
    SnowUiSelectorRect rects[]{{0, 0, 10, 10}, {0, 0, 100, 100}};
    SnowUiSelectorEvent event{};
    event.query = submission.query;
    event.phase = phase;
    event.reason = reason;
    event.ok = 1;
    event.rects = reason == SNOW_UI_SELECTOR_PERMISSION_REQUIRED ? rects + 1 : rects;
    event.count = reason == SNOW_UI_SELECTOR_PERMISSION_REQUIRED ? 1 : 2;
    submission.service->event(&event, submission.service->userdata);
    // Prove callbacks copy borrowed rectangles before queued delivery.
    rects[0].right = 1;
}
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

void settingsPersistAndResetToUia(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto binding = settings::SettingsSelectBinding::WindowElementApi;
    constexpr auto cursorBinding = settings::SettingsSwitchBinding::ScreenshotCaptureCursor;
    require(backend.selectValue(binding) == QStringLiteral("uia"),
            "Window Element API must initially select UIA");
    require(!backend.switchValue(cursorBinding) && backend.applySwitchValue(cursorBinding, true) &&
                backend.switchValue(cursorBinding),
            "the settings backend must apply and read cursor capture");
    require(backend.applySelectValue(binding, QStringLiteral("msaa")) &&
                backend.selectValue(binding) == QStringLiteral("msaa"),
            "the settings backend must apply and read MSAA");
    require(!backend.applySelectValue(binding, QStringLiteral("unknown")) &&
                backend.selectValue(binding) == QStringLiteral("msaa"),
            "invalid API choices must preserve the accepted value");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "window element API must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(reloaded.value(QStringLiteral("screenshot/window_element_api")) ==
                QStringLiteral("msaa"),
            "window element API must survive a configuration reload");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotCapture) &&
                backend.selectValue(binding) == QStringLiteral("uia") &&
                !backend.switchValue(cursorBinding),
            "resetting system Screenshot settings must restore UIA and disable cursor capture");
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
                QStringLiteral("uia"),
            "invalid stored API values must fall back to UIA");
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

void shortcutExitConfirmationSettingsPersistAndReset(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto binding =
        settings::SettingsSwitchBinding::ScreenshotConfirmBeforeExitingViaShortcut;
    require(!backend.switchValue(binding), "shortcut exit confirmation must default to disabled");
    require(backend.applySwitchValue(binding, true) && backend.switchValue(binding) &&
                storage::ScreenshotSettings().confirmBeforeExitingViaShortcut(),
            "shortcut exit confirmation must be enabled through the settings backend");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "shortcut exit confirmation preference must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(
        reloaded.value(QStringLiteral("screenshot/confirm_before_exiting_via_shortcut")).toBool(),
        "enabled shortcut exit confirmation must survive a configuration reload");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotCapture) &&
                backend.switchValue(binding),
            "system screenshot reset must preserve shortcut exit confirmation");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotSettings) &&
                !backend.switchValue(binding),
            "function screenshot reset must disable shortcut exit confirmation");
    const auto invalid = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot/confirm_before_exiting_via_shortcut"),
        QStringLiteral("enabled"));
    require(!invalid.valid, "shortcut exit confirmation preference must reject nonboolean values");
}

void ownUiCapturePreferencesPersistAndReset() {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto scrolling =
        settings::SettingsSwitchBinding::ScreenshotCaptureUiInScrollingScreenshot;
    constexpr auto recording = settings::SettingsSwitchBinding::ScreenRecordingCaptureToolbar;
    require(backend.switchValue(scrolling) && backend.switchValue(recording),
            "both own-UI capture preferences must default to enabled");
    require(backend.applySwitchValue(scrolling, false) && !backend.switchValue(scrolling) &&
                !storage::ScreenshotSettings().captureUiInScrollingScreenshot(),
            "scrolling screenshot UI capture must be disabled through the settings backend");
    require(backend.applySwitchValue(recording, false) && !backend.switchValue(recording) &&
                !storage::RecordingSettings().captureToolbarInRecording(),
            "recording toolbar capture must be disabled through the settings backend");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                !backend.switchValue(recording) && !backend.switchValue(scrolling),
            "the Function screen recording reset must not own either capture preference");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenRecordingCapture) &&
                backend.switchValue(recording) && !backend.switchValue(scrolling),
            "the system Screen recording reset must restore only toolbar capture");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotCapture) &&
                backend.switchValue(scrolling),
            "the system Screenshot reset must restore scrolling screenshot UI capture");
}

void toolbarLayoutSectionResetsRemainIndependent() {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto defaultDrawingLayout =
        backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::DrawingTools);
    const auto defaultActionLayout =
        backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools);
    const storage::ScreenshotToolbarLayout expectedDefaultActionLayout{
        {{QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown"),
          QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")},
         {QStringLiteral("record-screen")},
         {QStringLiteral("pin-to-screen")},
         {QStringLiteral("text-recognition")},
         {QStringLiteral("text-translation")},
         {QStringLiteral("scrolling-screenshot")},
         {QStringLiteral("quick-save"), QStringLiteral("save-as-file")}},
        {}};
    require(defaultActionLayout == expectedDefaultActionLayout,
            "the default action layout must include conversions and quick-save");
    const storage::ScreenshotToolbarLayout drawingLayout{
        {{QStringLiteral("watermark")}},
        {QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("line"),
         QStringLiteral("free-draw"), QStringLiteral("highlighter"), QStringLiteral("spotlight"),
         QStringLiteral("text"), QStringLiteral("serial-number"), QStringLiteral("filter"),
         QStringLiteral("eraser")},
    };
    // The custom arrangement still lists every default action (including the
    // conversions and quick-save) so normalization cannot append anything and
    // the persisted layout compares equal to what was applied.
    const storage::ScreenshotToolbarLayout actionLayout{
        {{QStringLiteral("quick-save"), QStringLiteral("save-as-file")}},
        {QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown"),
         QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition"),
         QStringLiteral("record-screen"), QStringLiteral("pin-to-screen"),
         QStringLiteral("text-recognition"), QStringLiteral("text-translation"),
         QStringLiteral("scrolling-screenshot")},
    };
    require(backend.applyToolbarLayout(storage::ScreenshotToolbarLayoutKind::DrawingTools,
                                       drawingLayout) &&
                backend.applyToolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools,
                                           actionLayout),
            "toolbar reset fixture must persist independent layouts");
    const auto savedActionLayout =
        backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools);

    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotInterfaceSettings) &&
                backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::DrawingTools) ==
                    drawingLayout &&
                backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools) ==
                    defaultActionLayout,
            "Screenshot Interface reset must restore only the screenshot action layout");

    require(backend.applyToolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools,
                                       actionLayout) &&
                backend.resetSection(settings::SettingsSectionReset::DrawingToolbar) &&
                backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools) ==
                    savedActionLayout &&
                backend.toolbarLayout(storage::ScreenshotToolbarLayoutKind::DrawingTools) ==
                    defaultDrawingLayout,
            "Drawing reset must restore only the drawing toolbar layout");
}

#ifndef Q_OS_MACOS
void changedApiRefreshesServiceAndRejectsOldResults() {
    ScreenshotSelectorCoordinator coordinator;
    const QVector<std::uintptr_t> exclusions{123, 456};
    int hitResults = 0;
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::initialResultReady, &coordinator,
                     [&hitResults](bool, const QVector<QRectF>&) { ++hitResults; });
    require(coordinator.startRefresh(exclusions), "initial selector refresh failed");
    QCoreApplication::sendPostedEvents();
    require(coordinator.ready() && currentBackend == SNOW_UI_SELECTOR_BACKEND_UIA,
            "default smart selection must create a UIA service");

    for (const auto& api : {QStringLiteral("msaa"), QStringLiteral("uia")}) {
        require(coordinator.requestHitTest(QPoint(10, 20),
                                           ScreenshotSelectorHitTestMode::WindowSubElement),
                "element hit test was not dispatched");
        require(currentMode == SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT,
                "smart selection must request child elements");
        const int previousCreated = created;
        const int previousDestroyed = destroyed;
        setApi(api);
        require(
            created == previousCreated && destroyed == previousDestroyed &&
                currentBackend == (api == QStringLiteral("uia") ? SNOW_UI_SELECTOR_BACKEND_UIA
                                                                : SNOW_UI_SELECTOR_BACKEND_MSAA) &&
                currentExclusions == exclusions && coordinator.refreshInFlight() &&
                !coordinator.ready() && !coordinator.hitTestInFlight(),
            "API changes must reuse and refresh the worker service with the same excluded windows");
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
    setApi(QStringLiteral("msaa"));
    QCoreApplication::sendPostedEvents();
    require(refreshResults == 1 && coordinator.ready() &&
                currentBackend == SNOW_UI_SELECTOR_BACKEND_MSAA,
            "an API change during refresh must discard the previous refresh result");

    coordinator.releaseCache();
    const int previousCreated = created;
    const int previousRefreshed = refreshed;
    setApi(QStringLiteral("uia"));
    require(created == previousCreated && refreshed == previousRefreshed && !coordinator.ready(),
            "changing the API while idle must defer service creation until the next capture");
    require(coordinator.startRefresh({789}) && currentBackend == SNOW_UI_SELECTOR_BACKEND_UIA,
            "the next capture must use the API selected while idle");
    QCoreApplication::sendPostedEvents();
}

#endif

void phasedSchedulingPreservesLatestPendingAndInitialCadence() {
    setApi(QStringLiteral("uia"));
    automaticReply = false;
    submissions.clear();
    refinements.clear();
    qint64 now = 0;
    ScreenshotSelectorCoordinator coordinator(nullptr, [&now]() { return now; });
    int initialCount = 0, refinementCount = 0;
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::initialResultReady, &coordinator,
                     [&](bool ok, const QVector<QRectF>& rects) {
                         require(ok && rects.first().width() == 10,
                                 "initial delivery must copy borrowed data");
                         ++initialCount;
                     });
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::refinementReady, &coordinator,
                     [&](const QVector<QRectF>& rects) {
                         require(rects.first().width() == 10, "refinement data must be copied");
                         ++refinementCount;
                     });
    require(coordinator.startRefresh({}), "refresh failed");
    QCoreApplication::sendPostedEvents();
    const auto request = [&](int x) {
        require(coordinator.requestHitTest(QPoint(x, 5),
                                           ScreenshotSelectorHitTestMode::WindowSubElement),
                "query failed");
    };
    request(1);
    const Submission a = submissions.last();
    now = 10;
    request(2);
    now = 20;
    request(3);
    require(submissions.size() == 1, "movement must coalesce while initial query runs");
    now = 170;
    deliver(a);
    QCoreApplication::sendPostedEvents();
    require(initialCount == 1 && submissions.size() == 2 && submissions.last().query.x == 3 &&
                refinements.isEmpty(),
            "A must display before C starts; B and A refinement must be skipped");
    const Submission c = submissions.last();
    deliver(c);
    QCoreApplication::sendPostedEvents();
    require(initialCount == 2 && refinements.size() == 1,
            "slow initial response must not add another stability delay");
    deliver(c, SNOW_UI_SELECTOR_REFINEMENT);
    QCoreApplication::sendPostedEvents();
    require(refinementCount == 1 && initialCount == 2 && !coordinator.hitTestInFlight(),
            "refinement must not act as initial completion");
    request(3);
    require(submissions.size() == 2 && refinements.size() == 1,
            "duplicate target must not restart either phase");
    now = 180;
    request(4);
    const Submission d = submissions.last();
    request(3);
    deliver(c, SNOW_UI_SELECTOR_FINISHED);
    QCoreApplication::sendPostedEvents();
    require(coordinator.hitTestInFlight() && refinementCount == 1,
            "old C terminal must not clear D or match a new C generation");
    deliver(d);
    QCoreApplication::sendPostedEvents();
    const Submission newC = submissions.last();
    require(newC.query.generation != c.query.generation && initialCount == 3,
            "returning to a position needs a fresh generation");
    now = 190;
    deliver(newC);
    QCoreApplication::sendPostedEvents();
    require(refinements.size() == 1, "fast initial must wait for target stability");
    now = 259;
    require(QMetaObject::invokeMethod(&coordinator, "scheduleRefinement", Qt::DirectConnection),
            "admission slot missing");
    require(refinements.size() == 1, "refinement must not start before 80 ms");
    now = 260;
    QMetaObject::invokeMethod(&coordinator, "scheduleRefinement", Qt::DirectConnection);
    require(refinements.size() == 2, "refinement must start at target stability deadline");
    deliver(c, SNOW_UI_SELECTOR_REFINEMENT);
    QCoreApplication::sendPostedEvents();
    require(refinementCount == 1, "A-B-A freshness must reject an old matching coordinate");
    require(coordinator.requestHitTest(QPoint(3, 5), ScreenshotSelectorHitTestMode::Window),
            "mode change failed");
    const Submission window = submissions.last();
    deliver(newC, SNOW_UI_SELECTOR_FINISHED);
    deliver(window, SNOW_UI_SELECTOR_INITIAL, SNOW_UI_SELECTOR_COMPLETE);
    QCoreApplication::sendPostedEvents();
    require(refinementCount == 1 && refinements.size() == 2,
            "window mode must invalidate refinement and stay single phase");
    coordinator.resetHitTestState();
    deliver(window);
    QCoreApplication::sendPostedEvents();
    require(initialCount == 5, "reset must reject old initial results");
    now = 300;
    request(7);
    const Submission oldEpoch = submissions.last();
    require(coordinator.startRefresh({}), "second refresh failed");
    deliver(oldEpoch);
    QCoreApplication::sendPostedEvents();
    require(initialCount == 5 && coordinator.ready(), "refresh must reject prior capture events");
    // Continuous switching must keep displaying the active initial results.
    const int invalidationsBeforeMovement = invalidations;
    request(10);
    for (int i = 11; i < 31; ++i) {
        const Submission active = submissions.last();
        request(i);
        deliver(active);
        QCoreApplication::sendPostedEvents();
    }
    require(initialCount == 25 && coordinator.hitTestInFlight(),
            "continuous movement must not starve foreground display");
    require(
        invalidations == invalidationsBeforeMovement,
        "foreground movement with no submitted refinement must not access the refinement queue");
    coordinator.releaseCache();
    automaticReply = true;
}

void permissionFallbackIsAppliedAndWarningIsThrottled() {
    automaticReply = false;
    submissions.clear();
    ScreenshotSelectorCoordinator coordinator;
    int warnings = 0, applied = 0;
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::accessibilityPermissionRequired,
                     &coordinator, [&] { ++warnings; });
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::initialResultReady, &coordinator,
                     [&](bool ok, const QVector<QRectF>& rects) {
                         require(ok && rects == QVector<QRectF>{QRectF(0, 0, 100, 100)},
                                 "permission fallback must contain only the selected window");
                         ++applied;
                     });
    require(coordinator.startRefresh({}), "permission fixture refresh failed");
    QCoreApplication::sendPostedEvents();
    const qsizetype before = refinements.size();
    for (int i = 0; i < 3; ++i) {
        require(coordinator.requestHitTest(QPoint(i + 1, 20),
                                           ScreenshotSelectorHitTestMode::WindowSubElement),
                "permission fixture query failed");
        deliver(submissions.last(), SNOW_UI_SELECTOR_INITIAL, SNOW_UI_SELECTOR_PERMISSION_REQUIRED);
        QCoreApplication::sendPostedEvents();
    }
    require(applied == 3 && warnings == 1 && refinements.size() == before,
            "permission fallback must apply on every hover, warn once, and never refine");
    coordinator.releaseCache();
    require(coordinator.startRefresh({}), "next capture refresh failed");
    QCoreApplication::sendPostedEvents();
    require(
        coordinator.requestHitTest(QPoint(5, 20), ScreenshotSelectorHitTestMode::WindowSubElement),
        "next capture query failed");
    deliver(submissions.last(), SNOW_UI_SELECTOR_INITIAL, SNOW_UI_SELECTOR_PERMISSION_REQUIRED);
    QCoreApplication::sendPostedEvents();
    require(warnings == 2, "a new capture may show the permission warning again");
    automaticReply = true;
}

void permissionRevocationDuringRefinementAppliesFallback() {
    automaticReply = false;
    qint64 now = 0;
    ScreenshotSelectorCoordinator coordinator(nullptr, [&] { return now; });
    int warnings = 0, fallbacks = 0;
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::accessibilityPermissionRequired,
                     &coordinator, [&] { ++warnings; });
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::refinementReady, &coordinator,
                     [&](const QVector<QRectF>& rects, quint32 displayId, bool permissionRequired) {
                         require(permissionRequired &&
                                     rects == QVector<QRectF>{QRectF(0, 0, 100, 100)} &&
                                     displayId == submissions.last().query.display_id,
                                 "refinement must carry permission fallback and display identity");
                         ++fallbacks;
                     });
    require(coordinator.startRefresh({}), "revocation fixture refresh failed");
    QCoreApplication::sendPostedEvents();
    require(
        coordinator.requestHitTest(QPoint(10, 20), ScreenshotSelectorHitTestMode::WindowSubElement),
        "revocation fixture query failed");
    now = 100;
    deliver(submissions.last());
    QCoreApplication::sendPostedEvents();
    deliver(submissions.last(), SNOW_UI_SELECTOR_FINISHED, SNOW_UI_SELECTOR_PERMISSION_REQUIRED);
    QCoreApplication::sendPostedEvents();
    require(warnings == 1 && fallbacks == 1,
            "revoked permission must warn and apply window fallback");
    automaticReply = true;
}

void diagnosticEnvironmentOverridesRemainAvailable() {
#ifdef Q_OS_MACOS
    qputenv("SNOW_SHOT_SELECTOR_BACKEND", "msaa");
    qputenv("SNOW_SHOT_UI_SELECTOR_BACKEND", "uia");
    ScreenshotSelectorCoordinator coordinator;
    require(coordinator.startRefresh({}) &&
                currentBackend == SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY,
            "macOS must ignore Windows diagnostic overrides");
    QCoreApplication::sendPostedEvents();
    const int before = refreshed;
    setApi(QStringLiteral("msaa"));
    require(refreshed == before && coordinator.ready(),
            "legacy settings must not disturb macOS selection");
    qunsetenv("SNOW_SHOT_SELECTOR_BACKEND");
    qunsetenv("SNOW_SHOT_UI_SELECTOR_BACKEND");
#else
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
#endif
}
} // namespace

extern "C" {
SnowUiSelectorService* snow_ui_selector_service_create(SnowUiSelectorEventCallback event,
                                                       SnowUiSelectorRefreshCallback refresh,
                                                       void* userdata) {
    ++created;
    return new SnowUiSelectorService{event, refresh, userdata};
}
void snow_ui_selector_service_destroy(SnowUiSelectorService* service) {
    ++destroyed;
    delete service;
}
uint8_t snow_ui_selector_service_release_cache(SnowUiSelectorService*) {
    return 1;
}
void snow_ui_selector_service_invalidate_refinement(SnowUiSelectorService*) {
    ++invalidations;
}
uint8_t snow_ui_selector_service_refine(SnowUiSelectorService* service,
                                        const SnowUiSelectorQuery* query) {
    refinements.push_back({service, *query});
    return 1;
}
uint8_t snow_ui_selector_service_refresh(SnowUiSelectorService* service, uint64_t epoch,
                                         SnowUiSelectorBackend backend, const uintptr_t* excluded,
                                         size_t count) {
    ++refreshed;
    currentBackend = backend;
    currentExclusions.clear();
    for (size_t i = 0; i < count; ++i)
        currentExclusions.push_back(excluded[i]);
    service->refresh(epoch, 1, service->userdata);
    return 1;
}
uint8_t snow_ui_selector_service_query(SnowUiSelectorService* service,
                                       const SnowUiSelectorQuery* query) {
    currentMode = query->mode;
    submissions.push_back({service, *query});
    if (!automaticReply)
        return 1;
    SnowUiSelectorEvent event{};
    event.query = *query;
    event.ok = 1;
    service->event(&event, service->userdata);
    return 1;
}
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
    const bool selectorOnly = application.arguments().contains(QStringLiteral("--selector-only"));
    if (application.arguments().contains(
            QStringLiteral("--shortcut-exit-confirmation-settings-only"))) {
        shortcutExitConfirmationSettingsPersistAndReset(
            temporary.filePath(QStringLiteral("data/config.json")));
        applicationStorage.shutdown();
        return 0;
    }
    if (!selectorOnly) {
        settingsPersistAndResetToUia(temporary.filePath(QStringLiteral("data/config.json")));
        shutterSoundSettingsPersistAndReset(temporary.filePath(QStringLiteral("data/config.json")));
        shortcutExitConfirmationSettingsPersistAndReset(
            temporary.filePath(QStringLiteral("data/config.json")));
        ownUiCapturePreferencesPersistAndReset();
        toolbarLayoutSectionResetsRemainIndependent();
    }
#ifndef Q_OS_MACOS
    changedApiRefreshesServiceAndRejectsOldResults();
    apiChangesDuringRefreshAndWhileIdle();
#endif
    diagnosticEnvironmentOverridesRemainAvailable();
    phasedSchedulingPreservesLatestPendingAndInitialCadence();
    permissionFallbackIsAppliedAndWarningIsThrottled();
    permissionRevocationDuringRefinementAppliesFallback();
    require(created == destroyed, "selector leaked a native service");
    applicationStorage.shutdown();
    return 0;
}
