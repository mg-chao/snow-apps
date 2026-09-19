#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/components/standalonetranslationwindow.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/windowgeometrymemory.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/persistedwindowgeometry.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QJsonObject>
#include <QPointer>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QTemporaryDir>
#include <QWidget>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>

namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

QRect primaryAvailableGeometry() {
    return QGuiApplication::primaryScreen()->availableGeometry();
}

// The offscreen QPA emulates a 2px window frame and translates top-level
// placements by it, so positions are compared with a small tolerance; sizes are
// unaffected and stay exact.
constexpr int OFFSCREEN_FRAME_TOLERANCE = 4;

bool positionNear(const QPoint& actual, const QPoint& expected) {
    return std::abs(actual.x() - expected.x()) <= OFFSCREEN_FRAME_TOLERANCE &&
           std::abs(actual.y() - expected.y()) <= OFFSCREEN_FRAME_TOLERANCE;
}

struct MainWindowHarness {
    const settings::SettingsRegistry& registry;
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend;
    settings::SettingsRuntimeSession session;

    MainWindowHarness()
        : registry(settings::builtInSettingsRegistry()), backend(shortcuts),
          session(registry, backend) {}
};

// Creates a shown window, hands it to interact, then closes it and waits for the
// WA_DeleteOnClose destruction that persists the geometry. The window must live on
// the heap: WA_DeleteOnClose deletes it via deleteLater.
void runMainWindowSession(MainWindowHarness& harness,
                          const std::function<void(MainWindow&)>& interact) {
    QPointer<MainWindow> window(new MainWindow(harness.registry, harness.session));
    window->show();
    flushEvents();
    interact(*window);
    window->close();
    flushEvents();
    require(window.isNull(), "closing the main window deleted it");
}

void closeWithoutDeleting(MainWindow& window) {
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.close();
    flushEvents();
}

void geometryCodecParsingAndClamping() {
    const QRect saved(120, 80, 640, 480);
    const std::optional<storage::PersistedWindowGeometry> roundTripped =
        storage::parseWindowGeometry(storage::windowGeometryToJson(saved, true));
    require(roundTripped.has_value() && roundTripped->normalGeometry == saved &&
                roundTripped->maximized,
            "geometry codec round trip");

    require(!storage::parseWindowGeometry(QJsonObject{}).has_value(),
            "empty geometry object is rejected");
    require(!storage::parseWindowGeometry(
                 QJsonObject{{"x", 1}, {"y", 2}, {"width", 0}, {"height", 100}})
                 .has_value(),
            "non-positive dimensions are rejected");
    require(!storage::parseWindowGeometry(
                 QJsonObject{{"x", QStringLiteral("1")}, {"y", 2}, {"width", 3}, {"height", 4}})
                 .has_value(),
            "non-integer coordinates are rejected");
    require(!storage::parseWindowGeometry(
                 QJsonObject{{"x", 1}, {"y", 2}, {"width", 3}, {"height", 4}, {"maximized", 1}})
                 .has_value(),
            "non-boolean maximized flag is rejected");
    const std::optional<storage::PersistedWindowGeometry> withoutFlag =
        storage::parseWindowGeometry(QJsonObject{{"x", 1}, {"y", 2}, {"width", 3}, {"height", 4}});
    require(withoutFlag.has_value() && !withoutFlag->maximized, "maximized flag defaults to false");

    const std::optional<QSize> sizeRoundTrip =
        storage::parseWindowSize(storage::windowSizeToJson(QSize(680, 460)));
    require(sizeRoundTrip.has_value() && *sizeRoundTrip == QSize(680, 460),
            "size codec round trip");
    require(!storage::parseWindowSize(QJsonObject{{"width", -5}, {"height", 100}}).has_value(),
            "negative size is rejected");
    require(!storage::parseWindowSize(QJsonObject{{"width", 100}}).has_value(),
            "partial size is rejected");

    const QList<QRect> screens{QRect(0, 0, 800, 600), QRect(-1920, 0, 1920, 1080)};
    require(storage::clampWindowGeometryToScreens(QRect(-1900, 100, 640, 480), screens) ==
                QRect(-1900, 100, 640, 480),
            "geometry intersecting any screen keeps its position");
    require(storage::clampWindowGeometryToScreens(QRect(5000, 5000, 640, 480), screens) ==
                QRect(160, 120, 640, 480),
            "off-screen geometry is clamped inside the first screen");
    require(storage::clampWindowGeometryToScreens(QRect(5000, 5000, 1000, 700), screens) ==
                QRect(0, 0, 800, 600),
            "geometry larger than the fallback screen is clamped to its size");
    require(storage::clampWindowGeometryToScreens(QRect(10, 10, 640, 480), {}) ==
                QRect(10, 10, 640, 480),
            "missing screen list keeps the geometry unchanged");

    require(storage::clampWindowSize(QSize(100, 100), QSize(512, 316), QSize(800, 600)) ==
                QSize(512, 316),
            "sizes below the minimum are raised");
    require(storage::clampWindowSize(QSize(2000, 2000), QSize(512, 316), QSize(800, 600)) ==
                QSize(800, 600),
            "sizes above the maximum are capped");
    require(storage::clampWindowSize(QSize(700, 500), QSize(512, 316), QSize(800, 600)) ==
                QSize(700, 500),
            "in-range sizes are kept");

    const storage::PersistedWindowGeometry offScreen{QRect(5000, 5000, 640, 480), true};
    const storage::PersistedWindowGeometry fittedOffScreen =
        storage::fitPersistedWindowGeometry(offScreen, QSize(100, 80), screens);
    require(fittedOffScreen.maximized &&
                fittedOffScreen.normalGeometry == QRect(160, 120, 640, 480),
            "fit clamps off-screen geometry and keeps maximized");
    const storage::PersistedWindowGeometry tooSmall{QRect(10, 10, 50, 40), false};
    const storage::PersistedWindowGeometry fittedTooSmall =
        storage::fitPersistedWindowGeometry(tooSmall, QSize(100, 80), screens);
    require(!fittedTooSmall.maximized && fittedTooSmall.normalGeometry.size() == QSize(100, 80) &&
                fittedTooSmall.normalGeometry.topLeft() == QPoint(10, 10),
            "fit raises below-minimum sizes without moving an on-screen origin");
    const storage::PersistedWindowGeometry oversized{QRect(10, 10, 2000, 2000), false};
    require(storage::fitPersistedWindowGeometry(oversized, QSize(100, 80), screens)
                    .normalGeometry.size() == QSize(1920, 1080),
            "fit caps size to the largest available screen");
    require(storage::fitPersistedWindowGeometry(tooSmall, QSize(100, 80), {}).normalGeometry ==
                QRect(10, 10, 100, 80),
            "fit with no screens only enforces the minimum size");
}

void windowMemorySettingsPersistAcrossReload(const QString& dataDirectory) {
    const storage::WindowMemorySettings settings;
    require(settings.setMainWindowGeometry(QRect(12, 34, 567, 489), true),
            "write remembered main window geometry");
    require(settings.setTranslationWindowSize(QSize(680, 460)),
            "write remembered translation window size");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.configuration().flushNow().success, "flush window memory");

    const QString configPath = QDir(dataDirectory).filePath(QStringLiteral("config.json"));
    storage::ConfigurationStore reloaded(configPath, true, true, 60000);
    const std::optional<storage::PersistedWindowGeometry> geometry = storage::parseWindowGeometry(
        reloaded.value(QStringLiteral("interface/main_window_geometry")).toObject());
    require(geometry.has_value() && geometry->normalGeometry == QRect(12, 34, 567, 489) &&
                geometry->maximized,
            "remembered geometry survives a store reload");
    const std::optional<QSize> size = storage::parseWindowSize(
        reloaded.value(QStringLiteral("interface/translation_window_size")).toObject());
    require(size.has_value() && *size == QSize(680, 460),
            "remembered size survives a store reload");
}

void persistableCaptureUsesNormalGeometryWhenMaximized() {
    QWidget window;
    window.resize(320, 240);
    window.show();
    flushEvents();
    require(snow_shot::presentation::persistableWindowSize(window) == QSize(320, 240),
            "visible windows persist their current size");
    window.setWindowState(window.windowState() | Qt::WindowMaximized);
    flushEvents();
    require(window.isMaximized(), "offscreen QPA reports maximized");
    require(snow_shot::presentation::persistableWindowSize(window) ==
                window.normalGeometry().size(),
            "maximized windows persist the restore size");
    window.close();
}

void geometryMemoryAppliesRestoreOnlyAfterShow() {
    const storage::WindowMemorySettings settings;
    const QRect available = primaryAvailableGeometry();
    const QSize rememberedSize(qMin(600, available.width()), qMin(400, available.height()));
    const QPoint rememberedPosition(available.left() + 40, available.top() + 30);
    require(settings.setMainWindowGeometry(QRect(rememberedPosition, rememberedSize), false),
            "seed geometry for delayed restore");

    QWidget window;
    window.resize(200, 150);
    snow_shot::presentation::WindowGeometryMemory memory(&window);
    memory.restoreMainWindow(QSize(100, 100));
    require(window.size() == QSize(200, 150), "restore does not apply while hidden");
    window.show();
    flushEvents();
    require(window.size() == rememberedSize && positionNear(window.pos(), rememberedPosition),
            "first show applies the fitted geometry");
    window.close();
    flushEvents();
}

void translationSizeHelpersClampRememberedValues() {
    require(storage::WindowMemorySettings().setTranslationWindowSize(QSize(40, 40)),
            "seed tiny translation size");
    const std::optional<QSize> raised =
        snow_shot::presentation::restoredTranslationWindowSize(QSize(650, 400), QSize(800, 600));
    require(raised.has_value() && *raised == QSize(650, 400),
            "restored translation size is raised to the minimum");

    QWidget window;
    window.resize(680, 460);
    window.show();
    flushEvents();
    snow_shot::presentation::rememberTranslationWindowSize(window);
    const std::optional<QSize> remembered = storage::WindowMemorySettings().translationWindowSize();
    require(remembered.has_value() && *remembered == QSize(680, 460),
            "translation size helper persists the visible size");
    window.close();
}

void mainWindowRemembersGeometryAcrossRecreation() {
    MainWindowHarness harness;
    const storage::WindowMemorySettings settings;
    const QRect available = primaryAvailableGeometry();
    const QSize rememberedSize(qMin(600, available.width()), qMin(400, available.height()));
    const QPoint rememberedPosition(available.left() + 40, available.top() + 30);

    runMainWindowSession(harness, [&](MainWindow& window) {
        window.resize(rememberedSize);
        window.move(rememberedPosition);
        flushEvents();
    });

    const std::optional<storage::PersistedWindowGeometry> saved = settings.mainWindowGeometry();
    require(saved.has_value() && saved->normalGeometry.size() == rememberedSize &&
                positionNear(saved->normalGeometry.topLeft(), rememberedPosition) &&
                !saved->maximized,
            "closing the main window persists its geometry");

    MainWindow restored(harness.registry, harness.session);
    restored.show();
    flushEvents();
    require(restored.size() == rememberedSize && positionNear(restored.pos(), rememberedPosition),
            "recreated main window restores size and position");
    closeWithoutDeleting(restored);
}

void mainWindowRemembersMaximizedState() {
    MainWindowHarness harness;
    const storage::WindowMemorySettings settings;
    require(settings.setMainWindowGeometry(QRect(40, 30, 600, 400), false),
            "seed unmaximized geometry");

    runMainWindowSession(harness, [](MainWindow& window) {
        window.setWindowState(window.windowState() | Qt::WindowMaximized);
        flushEvents();
        require(window.isMaximized(), "window reports the maximized state");
    });

    const std::optional<storage::PersistedWindowGeometry> saved = settings.mainWindowGeometry();
    require(saved.has_value() && saved->maximized,
            "closing a maximized main window persists the maximized flag");

    MainWindow restored(harness.registry, harness.session);
    restored.show();
    flushEvents();
    require(restored.windowState().testFlag(Qt::WindowMaximized),
            "recreated main window restores the maximized state");
    closeWithoutDeleting(restored);
}

void mainWindowClampsUnreachableRememberedGeometry() {
    MainWindowHarness harness;
    const storage::WindowMemorySettings settings;
    const QRect available = primaryAvailableGeometry();

    require(settings.setMainWindowGeometry(QRect(50000, 40000, 600, 400), false),
            "seed off-screen geometry");
    {
        MainWindow window(harness.registry, harness.session);
        window.show();
        flushEvents();
        require(available.contains(window.geometry()),
                "off-screen remembered geometry is restored inside the primary screen");
        closeWithoutDeleting(window);
    }

    require(settings.setMainWindowGeometry(
                QRect(0, 0, available.width() * 4, available.height() * 4), false),
            "seed oversized geometry");
    {
        MainWindow window(harness.registry, harness.session);
        window.show();
        flushEvents();
        const QRect visibleArea =
            available.adjusted(-OFFSCREEN_FRAME_TOLERANCE, -OFFSCREEN_FRAME_TOLERANCE,
                               OFFSCREEN_FRAME_TOLERANCE, OFFSCREEN_FRAME_TOLERANCE);
        require(window.width() <= available.width() && window.height() <= available.height() &&
                    visibleArea.contains(window.geometry()),
                "oversized remembered geometry is clamped to the largest screen");
        closeWithoutDeleting(window);
    }

    require(settings.setMainWindowGeometry(QRect(40, 30, 200, 150), false),
            "seed below-minimum geometry");
    {
        MainWindow window(harness.registry, harness.session);
        window.show();
        flushEvents();
        require(window.size() == QSize(512, 316),
                "remembered geometry below the window minimum is raised to the minimum");
        closeWithoutDeleting(window);
    }
}

void mainWindowOpensWithDefaultsWhenNothingIsRemembered() {
    MainWindowHarness harness;
    require(storage::ApplicationStorage::instance().configuration().setValue(
                QStringLiteral("interface/main_window_geometry"), QJsonObject()),
            "clear remembered geometry");
    MainWindow window(harness.registry, harness.session);
    window.show();
    flushEvents();
    require(window.size() == QSize(900, 556),
            "a fresh configuration opens the main window at the default size");
    closeWithoutDeleting(window);
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("window_geometry_memory_tests"));
    QTemporaryDir directory;
    require(directory.isValid(), "isolated window geometry test storage");
    auto& storage = storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 60000}).success,
            "initialize isolated window geometry test storage");
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);

    geometryCodecParsingAndClamping();
    windowMemorySettingsPersistAcrossReload(directory.path());
    persistableCaptureUsesNormalGeometryWhenMaximized();
    geometryMemoryAppliesRestoreOnlyAfterShow();
    translationSizeHelpersClampRememberedValues();
    mainWindowRemembersGeometryAcrossRecreation();
    mainWindowRemembersMaximizedState();
    mainWindowClampsUnreachableRememberedGeometry();
    mainWindowOpensWithDefaultsWhenNothingIsRemembered();

    storage.shutdown();
    return 0;
}
