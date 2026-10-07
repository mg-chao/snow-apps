#include "snow_shot/presentation/languagemanager.h"
#include "image_orientation_fixture.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "../src/presentation/services/systemtraymenuposition.h"
#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/shortcuts/shortcutdisplayservice.h"

#include "widgets/context_menu.h"
#include "widgets/modal.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QLineEdit>
#include <QMenu>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QString>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <functional>

#ifdef Q_OS_MACOS
#include "systemnotificationservice.h"
int runNativeSystemTrayMenuTests(snow_shot::presentation::SystemTrayController& controller);
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void clickNotification(snow_shot::presentation::SystemTrayController& controller,
                       QSystemTrayIcon* trayIcon,
                       snow_shot::platform::SystemNotificationAction action,
                       const QString& path = {}) {
#ifdef Q_OS_MACOS
    Q_UNUSED(trayIcon);
    auto* notifications =
        controller.findChild<snow_shot::platform::macos::SystemNotificationService*>();
    require(notifications != nullptr, "macOS must own its native notification service");
    notifications->activated(action, path);
#else
    Q_UNUSED(controller);
    Q_UNUSED(action);
    Q_UNUSED(path);
    trayIcon->messageClicked();
#endif
}

void requireActionText(const QAction* action, const QString& expected, const char* message) {
    require(action != nullptr && action->text() == expected, message);
}

void requireGroupAction(const adqt::widgets::AdContextMenu* menu, const QAction* action,
                        const QString& label, const QString& counts, const char* message) {
    require(action != nullptr && action->text() == label && menu->actionBadge(action) == counts &&
                action->shortcut().isEmpty(),
            message);
}

QString balloonTitle(const QSystemTrayIcon* trayIcon) {
    return trayIcon->property("lastBalloonTitle").toString();
}

QString balloonMessage(const QSystemTrayIcon* trayIcon) {
    return trayIcon->property("lastBalloonMessage").toString();
}

QSystemTrayIcon::MessageIcon balloonIcon(const QSystemTrayIcon* trayIcon) {
    return static_cast<QSystemTrayIcon::MessageIcon>(trayIcon->property("lastBalloonIcon").toInt());
}

void requireBalloon(const QSystemTrayIcon* trayIcon, const QString& title, const QString& message,
                    QSystemTrayIcon::MessageIcon icon, const char* reason) {
    require(balloonTitle(trayIcon) == title && balloonMessage(trayIcon) == message &&
                balloonIcon(trayIcon) == icon,
            reason);
}

void waitFor(const std::function<bool()>& predicate, const char* message) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

QAction* menuActionNamed(QMenu* menu, const QString& name) {
    for (auto* action : menu->actions())
        if (action->objectName() == name)
            return action;
    return nullptr;
}

void drainMenus() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void verifyTrayMenuPosition() {
    using snow_shot::presentation::systemTrayMenuPosition;
    const QRect icon(1800, 1040, 24, 24);
    require(systemTrayMenuPosition(icon, QPoint(100, 100)) == icon.center(),
            "keyboard context requests must stay at the tray icon when the pointer is elsewhere");
    require(systemTrayMenuPosition(icon, QPoint(-1500, 500)) == icon.center(),
            "a pointer on another monitor must not move a keyboard tray menu to that monitor");
    const QRect secondaryIcon(-120, -40, 24, 24);
    require(systemTrayMenuPosition(secondaryIcon, QPoint(100, 100)) == secondaryIcon.center(),
            "tray menu anchors must preserve negative coordinates on secondary monitors");
    const QPoint mouseClick = icon.topLeft() + QPoint(3, 5);
    require(systemTrayMenuPosition(icon, mouseClick) == mouseClick,
            "mouse context requests inside the icon must retain their pointer position");
    require(systemTrayMenuPosition(QRect(), mouseClick) == mouseClick,
            "platforms without tray geometry must keep a usable pointer-based placement");
}

void verifyTraySkins(snow_shot::presentation::SystemTrayController& controller,
                     const QTemporaryDir& directory) {
    using snow_shot::presentation::MainWindowSkinController;
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    QPointer<adqt::widgets::AdContextMenu> menu = controller.createContextMenu();
    if (menu->nativeMenuEnabled()) {
        auto* previous = MainWindowSkinController::existingInstance();
        QStyle* previousStyle = menu->style();
        require(configuration.setValue(QStringLiteral("interface/tray_menu_skin_path"),
                                       directory.filePath(QStringLiteral("native-skin.png"))),
                "the native skin fixture accepts a configured path");
        require(menu->style() == previousStyle && menu->backgroundFrame().image.isNull() &&
                    MainWindowSkinController::existingInstance() == previous,
                "native tray menus ignore skins without allocating a renderer");
        require(configuration.setValue(QStringLiteral("interface/tray_menu_skin_path"), QString()),
                "restore the empty native skin path");
    }
    menu->setNativeMenuEnabled(false);
    auto* previousController = MainWindowSkinController::existingInstance();
    menu->popupAt(QPoint(20, 20));
    require(menu->backgroundFrame().image.isNull() &&
                MainWindowSkinController::existingInstance() == previousController,
            "an unskinned popup allocates no skin service");
    menu->dismissPopup();
    drainMenus();
    require(!menu, "an unskinned tray popup retires completely after hiding");

    const QString skinPath = directory.filePath(QStringLiteral("tray-skin.png"));
    QImage skin(64, 64, QImage::Format_ARGB32_Premultiplied);
    skin.fill(QColor(213, 43, 79));
    require(skin.save(skinPath), "the tray skin fixture is writable");
    require(configuration.setValues({{QStringLiteral("interface/skin_opacity"), 0},
                                     {QStringLiteral("interface/tray_menu_skin_path"), skinPath}}),
            "configure a skin without opacity");
    for (int depth = 0; depth < 3; ++depth) {
        menu = controller.createContextMenu();
        menu->setNativeMenuEnabled(false);
        menu->popupAt(QPoint(20, 20));
        auto* popup = menu.data();
        if (depth >= 1) {
            popup = qobject_cast<adqt::widgets::AdContextMenu*>(
                menuActionNamed(popup, QStringLiteral("systemTrayWindowGroupAction"))->menu());
            popup->popupAt(QPoint(330, 20));
        }
        if (depth >= 2) {
            popup = qobject_cast<adqt::widgets::AdContextMenu*>(
                menuActionNamed(popup, QStringLiteral("systemTrayDeleteSpecifiedGroupAction"))
                    ->menu());
            popup->popupAt(QPoint(640, 20));
        }
        require(popup->backgroundFrame().image.isNull() &&
                    MainWindowSkinController::existingInstance() == previousController,
                "zero-opacity menus at every depth allocate no renderer");
        menu->dismissPopup();
        drainMenus();
        require(!menu, "each zero-opacity popup tree retires after hiding");
    }
    require(
        configuration.setValues(
            {{QStringLiteral("interface/skin_display_mode"), QStringLiteral("contain")},
             {QStringLiteral("interface/skin_blur_level"), 0},
             {QStringLiteral("interface/skin_mask_opacity"), 20},
             {QStringLiteral("interface/skin_opacity"), 73},
             {QStringLiteral("interface/tray_menu_skin_position"), QStringLiteral("top_left")}}),
        "configure shared skin effects and independent tray placement");
    menu = controller.createContextMenu();
    menu->setNativeMenuEnabled(false);
    menu->popupAt(QPoint(20, 20));
    QPointer<adqt::widgets::AdContextMenu> group = qobject_cast<adqt::widgets::AdContextMenu*>(
        menuActionNamed(menu, QStringLiteral("systemTrayWindowGroupAction"))->menu());
    group->popupAt(QPoint(330, 20));
    QPointer<adqt::widgets::AdContextMenu> deletion = qobject_cast<adqt::widgets::AdContextMenu*>(
        menuActionNamed(group, QStringLiteral("systemTrayDeleteSpecifiedGroupAction"))->menu());
    deletion->popupAt(QPoint(640, 20));
    waitFor(
        [&] {
            return !menu->backgroundFrame().image.isNull() &&
                   !group->backgroundFrame().image.isNull() &&
                   !deletion->backgroundFrame().image.isNull();
        },
        "each visible popup prepares its own frame");
    auto* skinController = MainWindowSkinController::existingInstance();
    require(skinController && skinController->diagnostics().decodeJobs == 1,
            "one popup tree shares a single decoded skin source");
    for (auto* popup : {menu.data(), group.data(), deletion.data()}) {
        const auto frame = popup->backgroundFrame();
        require(qAbs(frame.normalizedPlacement.x()) < 0.001 &&
                    qAbs(frame.normalizedPlacement.y()) < 0.001 &&
                    qAbs(frame.imageOpacity - 0.73) < 0.001 &&
                    qAbs(frame.maskOpacity - 0.20) < 0.001,
                "every menu uses independent placement with common effects");
    }
    require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 0),
            "disable loaded skins");
    waitFor([&] { return !skinController->diagnostics().busy; },
            "retire invisible render resources");
    require(skinController->diagnostics().retainedBytes == 0 &&
                skinController->diagnostics().executorCount == 0 &&
                skinController->diagnostics().scratchRetainedBytes == 0,
            "zero opacity releases decoded images, workers and scratch buffers");
    require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 73),
            "reactivate visible skins");
    waitFor([&] { return !deletion->backgroundFrame().image.isNull(); },
            "reattach visible submenu frames");
    const auto decoded = skinController->diagnostics().decodeJobs;
    require(configuration.setValue(QStringLiteral("interface/tray_menu_skin_position"),
                                   QStringLiteral("bottom_right")),
            "change tray placement live");
    waitFor(
        [&] {
            const auto frame = deletion->backgroundFrame();
            return !frame.image.isNull() &&
                   qAbs(frame.normalizedPlacement.x() + frame.normalizedPlacement.width() - 1.0) <
                       0.001 &&
                   qAbs(frame.normalizedPlacement.y() + frame.normalizedPlacement.height() - 1.0) <
                       0.001;
        },
        "a visible nested submenu follows live placement changes");
    require(skinController->diagnostics().decodeJobs == decoded,
            "placement changes reuse decoded source");
    deletion->dismissPopup();
    drainMenus();
    require(!deletion && group && menu, "a hidden nested submenu releases its frame and object");
    group->dismissPopup();
    drainMenus();
    require(!group && menu, "a hidden group submenu releases its frame and object");
    menu->resize(menu->width() + 37, menu->height() + 11);
    const int extent = qRound(std::min(menu->width(), menu->height()) * menu->devicePixelRatioF());
    waitFor([&] { return menu->backgroundFrame().image.size() == QSize(extent, extent); },
            "viewport changes prepare a new frame without decoding the source");
    require(skinController->diagnostics().decodeJobs == decoded,
            "resizing reuses the shared source");
    require(configuration.setValue(QStringLiteral("interface/tray_menu_skin_path"),
                                   directory.filePath(QStringLiteral("missing-tray-skin.png"))),
            "replace the skin with a failing source");
    waitFor(
        [&] {
            return skinController->hasError(snow_shot::presentation::SkinSurface::TrayMenu) &&
                   menu->backgroundFrame().image.isNull();
        },
        "a failed replacement releases stale frames and restores the themed menu");
    require(configuration.setValue(QStringLiteral("interface/tray_menu_skin_path"), QString()),
            "clear the failed skin");
    QPointer<MainWindowSkinController> retiring = skinController;
    waitFor([] { return MainWindowSkinController::existingInstance() == nullptr; },
            "retire the last skin controller");
    require(configuration.setValue(QStringLiteral("interface/tray_menu_skin_path"), skinPath),
            "reenable while the previous controller is retiring");
    auto* reenabled = MainWindowSkinController::existingInstance();
    require(reenabled && reenabled != retiring,
            "a visible popup attaches to the new active controller");
    drainMenus();
    require(!retiring, "release the retired skin controller");
    waitFor([&] { return !menu->backgroundFrame().image.isNull(); },
            "restore the visible popup skin");
    menu->dismissPopup();
    drainMenus();
    require(!menu, "hiding a skinned root releases its entire popup session");
    menu = controller.createContextMenu();
    menu->setNativeMenuEnabled(false);
    menu->popupAt(QPoint(20, 20));
    waitFor([&] { return !menu->backgroundFrame().image.isNull(); },
            "a fresh popup restores its configured skin");
    menu->dismissPopup();
    drainMenus();
    require(configuration.setValues(
                {{QStringLiteral("interface/tray_menu_skin_path"), QString()},
                 {QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                 {QStringLiteral("interface/skin_mask_opacity"), 80},
                 {QStringLiteral("interface/skin_opacity"), 100},
                 {QStringLiteral("interface/tray_menu_skin_position"), QStringLiteral("center")}}),
            "restore skin settings");
}

#ifdef Q_OS_MACOS
bool containsOpaqueColor(const QImage& image, const QColor& color) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() == 255 && pixel.rgb() == color.rgb()) {
                return true;
            }
        }
    }
    return false;
}
#endif

bool hasTrayMenu() {
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QStringLiteral("systemTrayMenu"))
            return true;
    return false;
}

void verifyLazyGroupMenuRefresh() {
    snow_shot::presentation::PinnedWindowGroupManager groups;
    snow_shot::presentation::SystemTrayController controller(
        snow_shot::presentation::settings::builtInTrayCommandManifest(), &groups);
    require(!hasTrayMenu(), "constructing a tray allocates no menus");
    for (int index = 0; index < 20; ++index)
        groups.registerPendingPin(QStringLiteral("lazy-tray-%1").arg(index),
                                  QStringLiteral("default"));
    require(!hasTrayMenu(), "background state changes allocate no hidden menus");
    controller.setMenuOptions(snow_shot::storage::TraySettings().menuOptions());
    QPointer<adqt::widgets::AdContextMenu> menu = controller.createContextMenu();
    menu->setNativeMenuEnabled(false);
    menu->popupAt(QPoint(20, 20));
    auto* header = menuActionNamed(menu, QStringLiteral("systemTrayWindowGroupAction"));
    require(header && header->menu()->actions().isEmpty(),
            "root opening leaves group content unallocated");
    QPointer<adqt::widgets::AdContextMenu> group =
        qobject_cast<adqt::widgets::AdContextMenu*>(header->menu());
    group->popupAt(QPoint(330, 20));
    requireGroupAction(group,
                       menuActionNamed(group, QStringLiteral("systemTrayGroupAction-default")),
                       QStringLiteral("Default"), QStringLiteral("20/20"),
                       "submenu opening snapshots the latest pending counts");
    groups.completePendingPin(QStringLiteral("lazy-tray-0"));
    QCoreApplication::processEvents();
    requireGroupAction(
        group, menuActionNamed(group, QStringLiteral("systemTrayGroupAction-default")),
        QStringLiteral("Default"), QStringLiteral("19/19"), "visible groups follow owner updates");
    QPointer<QAction> previous =
        menuActionNamed(group, QStringLiteral("systemTrayGroupAction-default"));
    group->dismissPopup();
    drainMenus();
    require(!group && !previous && menu && header->menu()->actions().isEmpty(),
            "hiding groups retires their actions and leaves an empty shell");
    groups.completePendingPin(QStringLiteral("lazy-tray-1"));
    group = qobject_cast<adqt::widgets::AdContextMenu*>(header->menu());
    group->popupAt(QPoint(330, 20));
    requireGroupAction(group,
                       menuActionNamed(group, QStringLiteral("systemTrayGroupAction-default")),
                       QStringLiteral("Default"), QStringLiteral("18/18"),
                       "reopening groups creates a fresh snapshot");
    menu->dismissPopup();
    drainMenus();
    require(!menu && !group && !hasTrayMenu(), "hiding the tray root releases every menu resource");
    for (int index = 2; index < 20; ++index)
        groups.completePendingPin(QStringLiteral("lazy-tray-%1").arg(index));
}

} // namespace

int main(int argc, char* argv[]) {
    const QString applicationName =
        QStringLiteral("system-tray-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(applicationName);

    QApplication application(argc, argv);
    verifyTrayMenuPosition();
    if (application.arguments().contains(QStringLiteral("--menu-position-only"))) {
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--skin-only"))) {
        snow_shot::presentation::SystemTrayController beforeStorage;
        require(!snow_shot::storage::ApplicationStorage::instance().isInitialized() &&
                    snow_shot::presentation::MainWindowSkinController::existingInstance() ==
                        nullptr,
                "constructing a tray before storage initialization must remain safe and lazy");
    }
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "temporary storage directory should be available");
    static_cast<void>(snow_shot::storage::ApplicationStorage::instance().initialize(
        {storageDirectory.path(), storageDirectory.path(), 8000}));
    auto& languageManager = snow_shot::presentation::LanguageManager::instance();
    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English should be available from the English catalog");

    if (application.arguments().contains(QStringLiteral("--group-refresh-only"))) {
        verifyLazyGroupMenuRefresh();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    snow_shot::presentation::SystemTrayController controller;
#ifdef Q_OS_MACOS
    if (application.arguments().contains(QStringLiteral("--native-menu"))) {
        int result = 1;
        QTimer::singleShot(0, &application, [&]() {
            result = runNativeSystemTrayMenuTests(controller);
            application.exit(result);
        });
        application.exec();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return result;
    }
#endif
    if (application.arguments().contains(QStringLiteral("--skin-only"))) {
        controller.setMenuOptions(snow_shot::storage::TraySettings().menuOptions());
        verifyTraySkins(controller, storageDirectory);
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    auto* trayIcon =
        controller.findChild<QSystemTrayIcon*>(QStringLiteral("snowShotSystemTrayIcon"));
    require(trayIcon != nullptr, "the controller should own a system tray icon");
    if (application.arguments().contains(QStringLiteral("--recording-export-only"))) {
        using snow_shot::presentation::SystemTrayController;
        using Action = snow_shot::platform::SystemNotificationAction;
#ifdef Q_OS_MACOS
        using snow_shot::platform::macos::SystemNotificationService;
        auto* notifications = controller.findChild<SystemNotificationService*>();
        require(notifications != nullptr,
                "macOS export notifications must use the authorized UserNotifications backend");
        int notificationAttempts = 0;
        QVector<snow_shot::platform::SystemNotificationRequest> attemptedRequests;
        QObject::connect(
            &controller, &SystemTrayController::notificationDeliveryFinished, &controller,
            [&](const snow_shot::platform::SystemNotificationRequest& request,
                const snow_shot::platform::SystemNotificationResult& result) {
                require(
                    result.status ==
                        snow_shot::platform::SystemNotificationResult::Status::Unavailable,
                    "an unbundled fixture must report unavailable delivery without native calls");
                ++notificationAttempts;
                attemptedRequests.append(request);
            });
#endif
        QStringList openedPaths;
        int aboutRequests = 0;
        QObject::connect(&controller, &SystemTrayController::openRecordingFileRequested,
                         &controller, [&](const QString& path) { openedPaths.append(path); });
        QObject::connect(&controller, &SystemTrayController::openAboutRequested, &controller,
                         [&] { ++aboutRequests; });
        const QString first =
            storageDirectory.filePath(QStringLiteral("Video exports/first clip.mp4"));
        const QString second =
            storageDirectory.filePath(QStringLiteral("Video exports/second clip.mp4"));
        controller.showRecordingExportMessage(first);
        QCoreApplication::processEvents();
#ifdef Q_OS_MACOS
        require(notificationAttempts == 1,
                "recording completion must reach the native notification delivery service");
#endif
        requireBalloon(trayIcon, QStringLiteral("Video export completed"),
                       QDir::toNativeSeparators(first), QSystemTrayIcon::Information,
                       "recording notification displays the exported file path");
        clickNotification(controller, trayIcon, Action::OpenRecording, first);
        require(openedPaths == QStringList{first} && aboutRequests == 0,
                "clicking recording notification requests its file without opening About");
        controller.showRecordingExportMessage(second);
        clickNotification(controller, trayIcon, Action::OpenRecording, second);
        require(openedPaths == QStringList{first, second},
                "recording notification clicks must use their own exported file");
#ifdef Q_OS_MACOS
        notifications->activated(SystemNotificationService::Action::OpenRecording, first);
        require(openedPaths == QStringList{first, second, first},
                "clicking an older macOS notification must open that notification's export");
        openedPaths.removeLast();
#endif
        controller.showWarningMessage(QStringLiteral("Warning"),
                                      QStringLiteral("Capture unavailable"));
        clickNotification(controller, trayIcon, Action::None);
        require(openedPaths.size() == 2,
                "warning notifications clear the previous recording action");
        controller.showUpdateMessage(QStringLiteral("Update ready"));
        clickNotification(controller, trayIcon, Action::OpenAbout);
        require(openedPaths.size() == 2 && aboutRequests == 1,
                "update clicks keep their existing About action");
#ifdef Q_OS_MACOS
        QCoreApplication::processEvents();
        require(notificationAttempts == 4,
                "recording, warning and update messages must share the native delivery service");
        require(attemptedRequests.at(0).filePath == first &&
                    attemptedRequests.at(1).filePath == second &&
                    attemptedRequests.at(2).severity ==
                        snow_shot::platform::SystemNotificationSeverity::Warning &&
                    attemptedRequests.at(3).action == Action::OpenAbout,
                "unavailable results must preserve each notification's content and action");
#endif
        controller.showRecordingExportMessage(second);
        controller.setEnabled(false);
        controller.showRecordingExportMessage(first);
        trayIcon->messageClicked();
#ifdef Q_OS_MACOS
        notifications->activated(SystemNotificationService::Action::OpenRecording, first);
        notifications->activated(SystemNotificationService::Action::OpenRecording, second);
        QCoreApplication::processEvents();
        require(notificationAttempts == 6,
                "macOS notifications must remain independent of tray visibility");
        require(openedPaths == QStringList{first, second, first, second},
                "hidden tray icons must not disable new or previously delivered native actions");
        controller.showCaptureMessage(QStringLiteral("Capture failed"), false);
        QCoreApplication::processEvents();
        require(notificationAttempts == 7 &&
                    attemptedRequests.last().severity ==
                        snow_shot::platform::SystemNotificationSeverity::Error,
                "capture failure feedback must also reach delivery with the tray hidden");
        controller.showRecordingExportMessage({});
        QCoreApplication::processEvents();
        require(notificationAttempts == 7, "empty export paths must not create notifications");
#else
        require(openedPaths.size() == 2, "disabled legacy tray notifications cannot open files");
#endif
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    require(!trayIcon->icon().isNull(), "the bundled tray icon should load");
#ifdef Q_OS_MACOS
    require(!trayIcon->icon().isMask(),
            "bundled macOS tray icons must preserve their original colors");
#endif
    require(trayIcon->toolTip() == QStringLiteral("SnowShot"),
            "the tray tooltip should be SnowShot");
    controller.show();
    require(trayIcon->isVisible(), "show should make the tray icon visible");
    controller.setEnabled(false);
    require(!controller.isEnabled() && !trayIcon->isVisible(),
            "disabling the tray should hide it immediately");
    controller.show();
    require(!trayIcon->isVisible(), "show should not bypass a disabled tray");
    controller.setEnabled(true);
    require(controller.isEnabled() && trayIcon->isVisible(),
            "enabling the tray should show it immediately");
    controller.hide();
    require(!trayIcon->isVisible(), "hide should make the tray icon invisible");

    controller.showUpdateMessage(QStringLiteral("An update is ready."));
    requireBalloon(trayIcon, QStringLiteral("Update"), QStringLiteral("An update is ready."),
                   QSystemTrayIcon::Information,
                   "an update-ready balloon must use the Update title and an informational icon");
    controller.showCaptureMessage(QStringLiteral("Capture failed"), false);
    requireBalloon(trayIcon, QStringLiteral("Capture"), QStringLiteral("Capture failed"),
                   QSystemTrayIcon::Critical,
                   "a capture failure balloon must stay titled Capture with a critical icon");
    controller.showCaptureMessage(QStringLiteral("Capture delayed"), true);
    requireBalloon(trayIcon, QStringLiteral("Capture"), QStringLiteral("Capture delayed"),
                   QSystemTrayIcon::Warning,
                   "a capture warning balloon must stay titled Capture with a warning icon");
    controller.showCaptureMessage(QStringLiteral("Capture timed out"), true);
    requireBalloon(trayIcon, QStringLiteral("Capture"), QStringLiteral("Capture timed out"),
                   QSystemTrayIcon::Warning,
                   "a capture warning balloon must stay titled Capture with a warning icon");
    controller.showWarningMessage(QStringLiteral("Feature unavailable"),
                                  QStringLiteral("Screenshot is unavailable"));
    requireBalloon(trayIcon, QStringLiteral("Feature unavailable"),
                   QStringLiteral("Screenshot is unavailable"), QSystemTrayIcon::Warning,
                   "a general tray warning must preserve its title and warning severity");
    controller.setEnabled(false);
    controller.showUpdateMessage(QStringLiteral("Update with tray hidden"));
#ifdef Q_OS_MACOS
    requireBalloon(trayIcon, QStringLiteral("Update"), QStringLiteral("Update with tray hidden"),
                   QSystemTrayIcon::Information,
                   "native notification delivery must remain enabled with the tray hidden");
#else
    requireBalloon(trayIcon, QStringLiteral("Feature unavailable"),
                   QStringLiteral("Screenshot is unavailable"), QSystemTrayIcon::Warning,
                   "a disabled tray must not replace the last balloon with an update notice");
#endif
    controller.setEnabled(true);

    const QStringList bundledSelections{
        QStringLiteral("default"),      QStringLiteral("light"),      QStringLiteral("dark"),
        QStringLiteral("snow-default"), QStringLiteral("snow-light"), QStringLiteral("snow-dark"),
    };
    for (const QString& selection : bundledSelections) {
        controller.setIconSelection(selection);
        require(controller.iconSelection() == selection,
                "each supported tray icon selection should be retained");
        require(trayIcon->property("resolvedIconSource").toString() ==
                    QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-%1.png").arg(selection),
                "each tray icon selection should resolve to its bundled asset");
#ifdef Q_OS_MACOS
        require(!trayIcon->icon().isMask(),
                "switching bundled macOS tray icons must preserve their original colors");
#endif
    }
    const auto verifyDisabledBadge = [&]() {
        using snow_shot::presentation::GlobalShortcutAction;
        controller.setQuickActionChecked(GlobalShortcutAction::ToggleGlobalHotkeys, false);
        const QIcon original = trayIcon->icon();
        controller.setQuickActionChecked(GlobalShortcutAction::ToggleGlobalHotkeys, true);
        require(!trayIcon->icon().isMask(), "the disabled badge must retain its red color");
        for (const int size : {16, 22, 32, 44, 64}) {
            QImage normal(size, size, QImage::Format_ARGB32_Premultiplied);
            normal.fill(Qt::transparent);
            QPainter painter(&normal);
            original.paint(&painter, QRect(0, 0, size, size));
            painter.end();
            const QImage disabled = trayIcon->icon().pixmap(QSize(size, size), 1.0).toImage();
            require(disabled.size() == QSize(size, size),
                    "badged tray icons should provide standard and high-DPI sizes");
            const QColor red = disabled.pixelColor(size * 6 / 16, size * 6 / 16);
            require(red.red() > 180 && red.green() < 100 && red.blue() < 100,
                    "disabled shortcuts should display a red badge at the center");
            const int slashX = size / 2 - 1;
            const int slashY = size / 2;
            const QColor slash = disabled.pixelColor(slashX, slashY);
            require(slash.red() > 220 && slash.green() > 220 && slash.blue() > 220,
                    "the disabled badge should have a white diagonal slash");
            const int margin = size * 3 / 16;
            require(normal.copy(0, 0, size, margin) == disabled.copy(0, 0, size, margin) &&
                        normal.copy(0, size - margin, size, margin) ==
                            disabled.copy(0, size - margin, size, margin) &&
                        normal.copy(0, 0, margin, size) == disabled.copy(0, 0, margin, size) &&
                        normal.copy(size - margin, 0, margin, size) ==
                            disabled.copy(size - margin, 0, margin, size),
                    "the centered badge must preserve artwork along every edge");
        }
        controller.show();
        require(trayIcon->icon().pixmap(QSize(32, 32), 1.0).toImage() !=
                    original.pixmap(QSize(32, 32), 1.0).toImage(),
                "refreshing the tray must preserve the disabled badge");
        controller.setQuickActionChecked(GlobalShortcutAction::ToggleGlobalHotkeys, false);
        require(trayIcon->icon().pixmap(QSize(32, 32), 1.0).toImage() ==
                    original.pixmap(QSize(32, 32), 1.0).toImage(),
                "re-enabling shortcuts must restore the original tray artwork");
    };
    for (const QString& selection : bundledSelections) {
        controller.setQuickActionChecked(
            snow_shot::presentation::GlobalShortcutAction::ToggleGlobalHotkeys, true);
        controller.setIconSelection(selection);
        verifyDisabledBadge();
    }
    controller.setIconSelection(QStringLiteral("unsupported"));
    require(controller.iconSelection() == QStringLiteral("default") &&
                trayIcon->property("resolvedIconSource").toString() ==
                    QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-default.png"),
            "an unsupported tray icon selection should use the bundled default");

    const QString customIconPath = storageDirectory.filePath(QStringLiteral("custom-icon.png"));
    QImage customImage(64, 64, QImage::Format_ARGB32_Premultiplied);
    customImage.fill(QColor(242, 17, 137));
    require(customImage.save(customIconPath), "the custom tray icon fixture should be writable");
    controller.setCustomIconPath(customIconPath);
#ifdef Q_OS_MACOS
    require(!trayIcon->icon().isMask(),
            "custom macOS tray icons must preserve their original colors");
#endif
    require(controller.customIconPath() == customIconPath &&
                trayIcon->icon().pixmap(QSize(64, 64)).toImage().pixelColor(32, 32) ==
                    QColor(242, 17, 137),
            "a valid custom image should replace the bundled tray icon");
    const qulonglong firstDecodeCount = trayIcon->property("customIconDecodeCount").toULongLong();
    const qulonglong firstHitCount = trayIcon->property("customIconCacheHits").toULongLong();
    controller.show();
    controller.show();
    require(trayIcon->property("customIconDecodeCount").toULongLong() == firstDecodeCount &&
                trayIcon->property("customIconCacheHits").toULongLong() >= firstHitCount + 2,
            "repeated show calls should reuse the fingerprinted custom image without decoding");

    QImage replacementImage(80, 40, QImage::Format_ARGB32_Premultiplied);
    replacementImage.fill(QColor(17, 113, 229));
    require(replacementImage.save(customIconPath),
            "the custom tray icon fixture should support same-path replacement");
    QFile replacementFile(customIconPath);
    require(replacementFile.open(QIODevice::ReadWrite) &&
                replacementFile.setFileTime(QDateTime::currentDateTime().addSecs(5),
                                            QFileDevice::FileModificationTime),
            "the replacement fixture should receive a distinct source fingerprint");
    replacementFile.close();
    controller.show();
    require(trayIcon->property("customIconDecodeCount").toULongLong() == firstDecodeCount + 1 &&
                trayIcon->property("customIconSourcePixelSize").toSize() == QSize(80, 40) &&
                trayIcon->icon().pixmap(QSize(80, 40)).toImage().pixelColor(40, 20) ==
                    QColor(17, 113, 229),
            "a changed source fingerprint should replace the retained custom raster");

    verifyDisabledBadge();

    const QString largeIconPath = storageDirectory.filePath(QStringLiteral("large-icon.png"));
    QImage largeImage(1024, 512, QImage::Format_ARGB32_Premultiplied);
    largeImage.fill(QColor(31, 173, 91));
    require(largeImage.save(largeIconPath), "the large tray icon fixture should be writable");
    controller.setCustomIconPath(largeIconPath);
    require(trayIcon->property("customIconSourcePixelSize").toSize() == QSize(1024, 512) &&
                trayIcon->property("customIconDecodedPixelSize").toSize() == QSize(256, 128),
            "a large custom image should retain no raster larger than 256 by 256");

    for (const quint8 orientation : {quint8(6), quint8(8)}) {
        // The supported suffix uses QImageReader's content detection. JPEG provides the EXIF
        // metadata needed to exercise both quarter-turns through that existing decode path.
        const QString orientedPath =
            storageDirectory.filePath(QStringLiteral("oriented-icon-%1.png").arg(orientation));
        const QByteArray encoded =
            image_orientation_fixture::jpegWithExifOrientation(largeImage, orientation);
        QFile oriented(orientedPath);
        require(!encoded.isEmpty() && oriented.open(QIODevice::WriteOnly) &&
                    oriented.write(encoded) == encoded.size(),
                "the oriented tray image fixture should be writable");
        oriented.close();
        controller.setCustomIconPath(orientedPath);
        require(
            trayIcon->property("customIconSourcePixelSize").toSize() == QSize(512, 1024) &&
                trayIcon->property("customIconDecodedPixelSize").toSize() == QSize(128, 256) &&
                trayIcon->icon().pixmap(QSize(128, 256), 1.0).size() == QSize(128, 256),
            "tray metadata must match orientation while scaling remains bounded before rotation");
    }

    const QString icoPath = QFileInfo(QString::fromUtf8(__FILE__))
                                .dir()
                                .absoluteFilePath(QStringLiteral("../resources/app-icon.ico"));
    const QStringList pluginPaths = QCoreApplication::libraryPaths();
    QCoreApplication::setLibraryPaths({});
    require(!QImageReader::supportedImageFormats().contains(QByteArrayLiteral("ico")),
            "the regression must run without a Qt ICO decoder");
    controller.setCustomIconPath(icoPath);
    require(trayIcon->property("customIconSourcePixelSize").toSize() == QSize(256, 256) &&
                trayIcon->property("customIconDecodedPixelSize").toSize() == QSize(256, 256),
            "ICO loading should select the available frame nearest 256 by 256 without Qt plugins");
    require(trayIcon->property("resolvedIconSource").toString() == icoPath,
            "ICO loading without Qt plugins must use the custom icon");
    // Build the directory explicitly: fixture creation must not need Qt's ICO plugin either.
    const auto writeIcon = [&](const QString& name, const QList<QSize>& sizes,
                               const QList<QByteArray>& payloads) {
        const QString path = storageDirectory.filePath(name);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "the ICO fixture should be writable");
        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << quint16(0) << quint16(1) << quint16(sizes.size());
        quint32 offset = 6 + 16 * static_cast<quint32>(sizes.size());
        for (qsizetype index = 0; index < sizes.size(); ++index) {
            stream << quint8(sizes[index].width()) << quint8(sizes[index].height()) << quint8(0)
                   << quint8(0) << quint16(1) << quint16(32) << quint32(payloads[index].size())
                   << offset;
            offset += static_cast<quint32>(payloads[index].size());
        }
        for (const auto& payload : payloads) {
            require(file.write(payload) == payload.size(), "the ICO payload should be written");
        }
        return path;
    };
    const auto pngPayload = [](int extent, const QColor& color) {
        QImage image(extent, extent, QImage::Format_RGBA8888);
        image.fill(color);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
                "the embedded PNG fixture should be writable");
        return bytes;
    };
    const QColor selectedColor(64, 128, 255, 128);
    const QString multiIconPath = writeIcon(
        QStringLiteral("multi-icon.ico"), {QSize(16, 16), QSize(64, 64), QSize(32, 32)},
        {pngPayload(16, Qt::red), pngPayload(64, selectedColor), pngPayload(32, Qt::green)});
    controller.setCustomIconPath(multiIconPath);
    require(trayIcon->property("customIconSourcePixelSize").toSize() == QSize(64, 64) &&
                trayIcon->icon().pixmap(QSize(64, 64)).toImage().pixelColor(32, 32).rgba() ==
                    selectedColor.rgba(),
            "ICO selection must decode the nearest frame and preserve its alpha and color");
    const auto icoDecodeCount = trayIcon->property("customIconDecodeCount").toULongLong();
    controller.show();
    require(trayIcon->property("customIconDecodeCount").toULongLong() == icoDecodeCount,
            "unchanged ICO files should reuse the decoded icon");

    QByteArray dib;
    QDataStream dibStream(&dib, QIODevice::WriteOnly);
    dibStream.setByteOrder(QDataStream::LittleEndian);
    dibStream << quint32(40) << qint32(16) << qint32(32) << quint16(1) << quint16(32) << quint32(0)
              << quint32(16 * 16 * 4) << qint32(0) << qint32(0) << quint32(0) << quint32(0);
    for (int pixel = 0; pixel < 16 * 16; ++pixel) {
        dibStream << quint32(0xff4080c0);
    }
    dib.append(QByteArray(16 * 4, '\0')); // DWORD-aligned AND mask rows.
    controller.setCustomIconPath(writeIcon(QStringLiteral("dib-icon.ico"), {QSize(16, 16)}, {dib}));
    require(trayIcon->icon().pixmap(QSize(16, 16)).toImage().pixelColor(8, 8) ==
                QColor(64, 128, 192),
            "DIB-backed ICO images must decode without Qt plugins");
    controller.setCustomIconPath(
        writeIcon(QStringLiteral("broken-icon.ico"), {QSize(16, 16)}, {QByteArray("broken")}));
    require(trayIcon->property("resolvedIconSource").toString().startsWith(QStringLiteral(":/")),
            "a corrupt ICO payload must fall back to the bundled icon");
#ifdef Q_OS_MACOS
    require(!trayIcon->icon().isMask(),
            "a bundled fallback must preserve its colors after a custom icon fails to load");
#endif
    QCoreApplication::setLibraryPaths(pluginPaths);

    controller.setIconSelection(QStringLiteral("light"));
    const QString oversizedIconPath =
        storageDirectory.filePath(QStringLiteral("oversized-icon.png"));
    QImage oversizedImage(20000, 1, QImage::Format_ARGB32_Premultiplied);
    oversizedImage.fill(QColor(213, 71, 56));
    require(oversizedImage.save(oversizedIconPath),
            "the oversized tray icon fixture should be writable");
    controller.setCustomIconPath(oversizedIconPath);
    const qulonglong oversizedHitCount = trayIcon->property("customIconCacheHits").toULongLong();
    const qulonglong oversizedMissCount = trayIcon->property("customIconCacheMisses").toULongLong();
    const qulonglong oversizedDecodeCount =
        trayIcon->property("customIconDecodeCount").toULongLong();
    require(trayIcon->property("resolvedIconSource").toString() ==
                    QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-light.png") &&
                trayIcon->property("customIconSourcePixelSize").toSize() == QSize(20000, 1) &&
                trayIcon->property("customIconDecodedPixelSize").toSize().isEmpty(),
            "pathological source dimensions should be rejected before pixel allocation");
    controller.show();
    controller.show();
    require(trayIcon->property("customIconCacheHits").toULongLong() >= oversizedHitCount + 2 &&
                trayIcon->property("customIconCacheMisses").toULongLong() == oversizedMissCount &&
                trayIcon->property("customIconDecodeCount").toULongLong() == oversizedDecodeCount,
            "a rejected source fingerprint should remain cached across fallback rendering");

    controller.setCustomIconPath(storageDirectory.filePath(QStringLiteral("missing.png")));
    require(trayIcon->property("resolvedIconSource").toString() ==
                QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-light.png"),
            "an invalid custom image should fall back to the selected bundled tray icon");
    verifyDisabledBadge();
    const QString malformedIconPath =
        storageDirectory.filePath(QStringLiteral("malformed-icon.png"));
    QFile malformedIcon(malformedIconPath);
    require(malformedIcon.open(QIODevice::WriteOnly),
            "the malformed custom icon fixture should be writable");
    malformedIcon.write("not an image");
    malformedIcon.close();
    controller.setCustomIconPath(malformedIconPath);
    require(trayIcon->property("resolvedIconSource").toString() ==
                QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-light.png"),
            "a malformed custom image should fall back to the selected bundled tray icon");
    const QString unsupportedIconPath =
        storageDirectory.filePath(QStringLiteral("unsupported-icon.bmp"));
    require(customImage.save(unsupportedIconPath, "BMP"),
            "the unsupported custom icon fixture should be writable");
    controller.setCustomIconPath(unsupportedIconPath);
    require(trayIcon->property("resolvedIconSource").toString() ==
                QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-light.png"),
            "a readable custom image outside PNG and ICO should use the bundled fallback");

    require(trayIcon->contextMenu() == nullptr, "the tray must not retain an attached popup");
    auto* menu = controller.createContextMenu();
    require(menu != nullptr, "the tray should use the Ant Design context menu");
    require(menu->minimumWidth() == 300,
            "tray context menu should retain its 300-pixel minimum width");
    const auto actionForId = [menu](const QString& id) {
        for (QAction* action : menu->actions()) {
            if (action != nullptr && action->data().toString() == id) {
                return action;
            }
        }
        return static_cast<QAction*>(nullptr);
    };
    const auto actionForObjectName = [menu](const QString& objectName) {
        for (QAction* action : menu->actions()) {
            if (action != nullptr && action->objectName() == objectName) {
                return action;
            }
        }
        return static_cast<QAction*>(nullptr);
    };
    const auto visibleActions = [menu]() {
        QList<QAction*> result;
        for (QAction* action : menu->actions()) {
            if (action != nullptr && action->isVisible()) {
                result.push_back(action);
            }
        }
        return result;
    };
    const QString selectedTextId = QStringLiteral("quick.translate-selected-text");
    const QStringList translationMenu{selectedTextId, QStringLiteral("tray.exit")};
    controller.setMenuOptions(translationMenu);
    require(!actionForId(selectedTextId)->isVisible(),
            "disabled translation feature hides requested tray action");
    require(snow_shot::storage::ExtendedFeaturesSettings().setTranslationPageEnabled(true),
            "enable tray translation");
    controller.setMenuOptions(translationMenu);
    require(actionForId(selectedTextId)->isVisible(),
            "enabled translation feature exposes requested tray action");
    int selectedTextDispatches = 0;
    const auto translationConnection = QObject::connect(
        &controller, &snow_shot::presentation::SystemTrayController::quickActionRequested,
        &controller, [&](snow_shot::presentation::GlobalShortcutAction action) {
            require(action == snow_shot::presentation::GlobalShortcutAction::TranslateSelectedText,
                    "translation tray action uses the shared quick-action dispatch");
            ++selectedTextDispatches;
        });
    for (const bool standalone : {true, false}) {
        require(snow_shot::storage::ExtendedFeaturesSettings().setStandaloneTranslationWindow(
                    standalone),
                "toggle standalone mode for tray dispatch");
        controller.setMenuOptions(translationMenu);
        require(actionForId(selectedTextId)->isVisible(),
                "standalone setting does not gate actual tray action");
        actionForId(selectedTextId)->trigger();
    }
    require(selectedTextDispatches == 2, "both standalone modes dispatch the same tray action");
    QObject::disconnect(translationConnection);
    require(snow_shot::storage::ExtendedFeaturesSettings().setTranslationPageEnabled(false),
            "disable tray translation");
    controller.setMenuOptions(translationMenu);
    require(!actionForId(selectedTextId)->isVisible(),
            "disabling translation hides tray action again");
    const QStringList defaultMenuOptions = snow_shot::storage::TraySettings().menuOptions();
    controller.setMenuOptions(defaultMenuOptions);
    const QList<QAction*> defaultVisibleActions = visibleActions();
    auto* screenshotMenuAction = actionForId(QStringLiteral("quick.screenshot"));
    auto* delayedScreenshotMenuAction = actionForId(QStringLiteral("quick.screenshot-delay"));
    auto* recordingToggleMenuAction = actionForId(QStringLiteral("quick.screen-record-copy"));
    auto* hotkeyToggleMenuAction = actionForId(QStringLiteral("quick.toggle-global-hotkeys"));
    auto* showMainWindowMenuAction = actionForId(QStringLiteral("tray.show-main-window"));
    auto* restartMenuAction = actionForId(QStringLiteral("tray.restart-app"));
    auto* exitMenuAction = actionForId(QStringLiteral("tray.exit"));
    auto* windowGroupMenuAction =
        actionForObjectName(QStringLiteral("systemTrayWindowGroupAction"));
    auto* restoreClosedAction = actionForId(QStringLiteral("quick.restore-last-closed-windows"));
    require(restoreClosedAction && restoreClosedAction->isVisible(),
            "restore closed pins must appear in the default tray menu");
    const QStringList normalizedDefaultMenuOptions = controller.menuOptions();
    require(
        QSet<QString>(normalizedDefaultMenuOptions.cbegin(), normalizedDefaultMenuOptions.cend()) ==
                QSet<QString>(defaultMenuOptions.cbegin(), defaultMenuOptions.cend()) &&
            defaultVisibleActions.size() == 16 && screenshotMenuAction != nullptr &&
            screenshotMenuAction->isVisible() && delayedScreenshotMenuAction != nullptr &&
            delayedScreenshotMenuAction->isVisible() && recordingToggleMenuAction != nullptr &&
            !recordingToggleMenuAction->isVisible() && !screenshotMenuAction->icon().isNull() &&
            hotkeyToggleMenuAction != nullptr && hotkeyToggleMenuAction->isVisible() &&
            hotkeyToggleMenuAction->isCheckable() && !hotkeyToggleMenuAction->isChecked() &&
            showMainWindowMenuAction != nullptr && showMainWindowMenuAction->isVisible() &&
            !showMainWindowMenuAction->icon().isNull() && restartMenuAction != nullptr &&
            !restartMenuAction->isVisible() && !restartMenuAction->isCheckable() &&
            !restartMenuAction->icon().isNull() && exitMenuAction != nullptr &&
            exitMenuAction->isVisible() && !exitMenuAction->icon().isNull() &&
            windowGroupMenuAction != nullptr && windowGroupMenuAction->isVisible() &&
            actionForId(QStringLiteral("tray.window-grouping")) == windowGroupMenuAction &&
            defaultVisibleActions.contains(hotkeyToggleMenuAction) &&
            defaultVisibleActions.contains(showMainWindowMenuAction) &&
            defaultVisibleActions.indexOf(windowGroupMenuAction) ==
                defaultVisibleActions.indexOf(showMainWindowMenuAction) - 1,
        "the tray menu should expose the twelve default options in five catalog groups");
    requireActionText(screenshotMenuAction, QStringLiteral("Screenshot"),
                      "Screenshot should use its catalog label");
#ifdef Q_OS_MACOS
    const QImage screenshotMenuIcon =
        screenshotMenuAction->icon().pixmap(QSize(32, 32), QIcon::Normal).toImage();
    require(containsOpaqueColor(screenshotMenuIcon,
                                menu->palette().color(QPalette::Active, QPalette::Text)) &&
                containsOpaqueColor(screenshotMenuIcon, QColor(QStringLiteral("#9254de"))) &&
                !containsOpaqueColor(screenshotMenuIcon, QColor(QStringLiteral("#1677ff"))),
            "the native screenshot menu icon must use menu foreground with only its fixed purple "
            "accent");
#endif
    requireActionText(delayedScreenshotMenuAction, QStringLiteral("Delay 3s to execute"),
                      "Delayed screenshot should use the canonical shortcut title");
    requireActionText(recordingToggleMenuAction, QStringLiteral("Record/Copy Video"),
                      "Recording toggle should use the canonical shortcut title");
    requireActionText(hotkeyToggleMenuAction, QStringLiteral("Disable global hotkeys"),
                      "the hotkey toggle should keep the historical tray label");
    const QStringList displayCases{
        QStringLiteral("+"),     QStringLiteral("Shift++"),        QStringLiteral("Num+1"),
        QStringLiteral("Num++"), QStringLiteral("Shift+Shift"),    QStringLiteral("Period"),
        QStringLiteral("Comma"), QStringLiteral("  Shift + F2  "),
    };
    for (const QString& portable : displayCases) {
        controller.setGlobalShortcuts(snow_shot::presentation::GlobalShortcutAction::Screenshot,
                                      {QString(), portable, QStringLiteral("F3")});
        const auto configured =
            snow_shot::shortcuts::bindingsFromPortableText({portable, QStringLiteral("F3")}, true);
        requireActionText(screenshotMenuAction,
                          QStringLiteral("Screenshot\t") +
                              snow_shot::shortcuts::formatShortcutListDisplayText(configured),
                          "tray key names and alternatives must use the settings display format");
    }
    const QString screenshotShortcut = QStringLiteral("Ctrl+Alt+1");
    const QString alternateScreenshotShortcut = QStringLiteral("Meta+Shift+S");
    const QString screenshotShortcutHint = snow_shot::shortcuts::formatShortcutListDisplayText(
        snow_shot::shortcuts::bindingsFromPortableText(
            {screenshotShortcut, alternateScreenshotShortcut}));
    controller.setGlobalShortcuts(snow_shot::presentation::GlobalShortcutAction::Screenshot,
                                  {screenshotShortcut, alternateScreenshotShortcut});
    requireActionText(screenshotMenuAction, QStringLiteral("Screenshot\t") + screenshotShortcutHint,
                      "quick tray actions should display all configured global shortcuts");
    snow_shot::shortcuts::ShortcutDisplayService::instance().refresh();
    requireActionText(screenshotMenuAction, QStringLiteral("Screenshot\t") + screenshotShortcutHint,
                      "tray shortcut labels must refresh after an input-layout change");
    require(screenshotMenuAction->shortcut().isEmpty(),
            "displayed global shortcuts must not become menu-local shortcuts");
    controller.setScreenshotDelaySeconds(7);
    require(controller.screenshotDelaySeconds() == 7,
            "the tray should retain a normalized screenshot delay value");
    requireActionText(delayedScreenshotMenuAction, QStringLiteral("Delay 7s to execute"),
                      "the tray should refresh the canonical delayed screenshot title");
    controller.setScreenshotDelaySeconds(3);
    requireActionText(showMainWindowMenuAction, QStringLiteral("Show main interface"),
                      "Show main interface should follow Disable global hotkeys");
    requireActionText(restartMenuAction, QStringLiteral("Restart App"),
                      "Restart App should use its catalog label");
    requireActionText(exitMenuAction, QStringLiteral("Exit"), "Exit should be last");

    QStringList menuWithRestart = defaultMenuOptions;
    menuWithRestart.push_back(QStringLiteral("tray.restart-app"));
    controller.setMenuOptions(menuWithRestart);
    const QList<QAction*> visibleWithRestart = visibleActions();
    require(restartMenuAction->isVisible() &&
                visibleWithRestart.indexOf(restartMenuAction) ==
                    visibleWithRestart.indexOf(showMainWindowMenuAction) + 1 &&
                visibleWithRestart.indexOf(exitMenuAction) ==
                    visibleWithRestart.indexOf(restartMenuAction) + 1,
            "Restart App should be opt-in directly below Show main interface and above Exit");
    controller.setMenuOptions(defaultMenuOptions);

    snow_shot::presentation::PinnedWindowGroupManager groupManager;
    controller.setGroupManager(&groupManager);
    auto* windowGroupMenu =
        qobject_cast<adqt::widgets::AdContextMenu*>(windowGroupMenuAction->menu());
    QMetaObject::invokeMethod(windowGroupMenu, "aboutToShow", Qt::DirectConnection);
    require(windowGroupMenu != nullptr, "the tray menu should own a window group submenu");
    const auto groupActionNamed = [windowGroupMenu](const QString& name) {
        // Materialize the lazy submenu as an actual opening would.
        QMetaObject::invokeMethod(windowGroupMenu, "aboutToShow", Qt::DirectConnection);
        for (QAction* action : windowGroupMenu->actions()) {
            if (action != nullptr && action->objectName() == name) {
                return action;
            }
        }
        return static_cast<QAction*>(nullptr);
    };
    const auto deletionModalNamed = [&groupManager](const QString& name) {
        return groupManager.findChild<adqt::widgets::AdModal*>(name);
    };
    require(!windowGroupMenuAction->icon().isNull(),
            "the window group submenu header should carry an icon");
    requireGroupAction(windowGroupMenu,
                       groupActionNamed(QStringLiteral("systemTrayGroupAction-default")),
                       QStringLiteral("Default"), QStringLiteral("0/0"),
                       "the tray group row should show non-ignored and total counts");
    QAction* trayNewGroup = groupActionNamed(QStringLiteral("systemTrayNewGroupAction"));
    require(trayNewGroup != nullptr && !trayNewGroup->icon().isNull() && trayNewGroup->isEnabled(),
            "tray New Group should expose an icon and stay actionable");
    // An unrelated top-level window exercises the same implicit owner selection as a pin.
    // Keep it away from the screen center so this also catches regressions offscreen.
    const QPoint originalCursorPosition = QCursor::pos();
    for (QScreen* screen : QApplication::screens()) {
        QCursor::setPos(screen->availableGeometry().center());
        for (const bool hasVisibleWindow : {false, true}) {
            QWidget unrelatedWindow(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
            unrelatedWindow.setGeometry(
                QRect(screen->availableGeometry().topLeft() + QPoint(20, 20), QSize(160, 100)));
            if (hasVisibleWindow) {
                unrelatedWindow.show();
                unrelatedWindow.activateWindow();
                QApplication::processEvents();
            }
            trayNewGroup->trigger();
            QApplication::processEvents();
            QWidget* dialog = nullptr;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (widget->isVisible() &&
                    widget->objectName() == QStringLiteral("ad-modal-overlay") &&
                    widget->findChild<QWidget*>(QStringLiteral("pinnedWindowGroupCreateForm"))) {
                    dialog = widget;
                    break;
                }
            }
            require(dialog != nullptr, "tray New Group should show its dialog without an owner");
            const QPoint centerOffset =
                dialog->geometry().center() - screen->availableGeometry().center();
            require(qAbs(centerOffset.x()) <= 1 && qAbs(centerOffset.y()) <= 1,
                    "tray New Group must center on the cursor screen regardless of visible pins");
            require(dialog->parentWidget() == nullptr,
                    "tray New Group must not adopt an unrelated window as its owner");
            dialog->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
    QCursor::setPos(originalCursorPosition);
    QAction* trayDeleteEmpty =
        groupActionNamed(QStringLiteral("systemTrayDeleteEmptyGroupsAction"));
    require(trayDeleteEmpty != nullptr && !trayDeleteEmpty->icon().isNull(),
            "tray Delete Empty Groups should expose an icon");
    require(!trayDeleteEmpty->isEnabled(),
            "tray Delete Empty Groups should start disabled while only the built-in group "
            "exists");

    QPointer<adqt::widgets::AdContextMenu> trayDeleteSpecifiedMenu =
        qobject_cast<adqt::widgets::AdContextMenu*>(
            windowGroupMenu
                ->findChild<QAction*>(QStringLiteral("systemTrayDeleteSpecifiedGroupAction"))
                ->menu());
    QMetaObject::invokeMethod(trayDeleteSpecifiedMenu, "aboutToShow", Qt::DirectConnection);
    require(trayDeleteSpecifiedMenu != nullptr &&
                !trayDeleteSpecifiedMenu->menuAction()->icon().isNull(),
            "tray Delete Specified Group should expose the supplied icon");
    const auto deleteSpecifiedActionNamed = [windowGroupMenu,
                                             &trayDeleteSpecifiedMenu](const QString& name) {
        auto* header = windowGroupMenu->findChild<QAction*>(
            QStringLiteral("systemTrayDeleteSpecifiedGroupAction"));
        trayDeleteSpecifiedMenu = qobject_cast<adqt::widgets::AdContextMenu*>(header->menu());
        QMetaObject::invokeMethod(trayDeleteSpecifiedMenu, "aboutToShow", Qt::DirectConnection);
        for (QAction* action : trayDeleteSpecifiedMenu->actions())
            if (action->objectName() == name)
                return action;
        return static_cast<QAction*>(nullptr);
    };
    const auto requireTrayModalCentered = [](adqt::widgets::AdModal* modal, const char* message) {
        QScreen* screen = QApplication::screenAt(QCursor::pos());
        if (screen == nullptr) {
            screen = QApplication::primaryScreen();
        }
        QWidget* surface = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->isVisible() && widget->objectName() == QStringLiteral("ad-modal-overlay")) {
                surface = widget;
                break;
            }
        }
        require(screen != nullptr && modal != nullptr && modal->centered() && surface != nullptr &&
                    surface->isVisible() &&
                    (surface->geometry().center() - screen->availableGeometry().center())
                            .manhattanLength() <= 2,
                message);
    };
    const QList<QAction*> initialGroupActions = windowGroupMenu->actions();
    require(initialGroupActions.indexOf(trayDeleteEmpty) + 1 ==
                initialGroupActions.indexOf(windowGroupMenu->findChild<QAction*>(
                    QStringLiteral("systemTrayDeleteSpecifiedGroupAction"))),
            "tray Delete Specified Group should sit directly below Delete Empty Groups");
    requireGroupAction(
        trayDeleteSpecifiedMenu,
        deleteSpecifiedActionNamed(QStringLiteral("systemTrayDeleteSpecifiedGroupAction-default")),
        QStringLiteral("Default"), QStringLiteral("0/0"),
        "tray Delete Specified Group should initially list only the empty Default group");

    const auto traySpecifiedId = groupManager.createGroup(QStringLiteral("Tray specified"));
    require(traySpecifiedId.has_value(),
            "a custom group should be created for tray specified deletion");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QAction* trayDeleteSpecified = deleteSpecifiedActionNamed(
        QStringLiteral("systemTrayDeleteSpecifiedGroupAction-%1").arg(*traySpecifiedId));
    require(trayDeleteSpecified != nullptr &&
                trayDeleteSpecified->data().toString() == *traySpecifiedId &&
                trayDeleteSpecified->text() == QStringLiteral("Tray specified") &&
                trayDeleteSpecifiedMenu->actionBadge(trayDeleteSpecified) == QStringLiteral("0/0"),
            "tray specified deletion should list every custom group with its count and id");
    trayDeleteSpecified->trigger();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    auto* specifiedModal =
        deletionModalNamed(QStringLiteral("pinnedWindowGroupDeleteSpecifiedModal"));
    require(specifiedModal != nullptr && specifiedModal->ownerWindow() == nullptr &&
                specifiedModal->windowModeDetached() &&
                specifiedModal->windowModality() == Qt::ApplicationModal &&
                specifiedModal->acceptAccentRole() == adqt::widgets::AdButton::AccentRole::Danger &&
                specifiedModal->text().contains(QStringLiteral("Tray specified")) &&
                specifiedModal->text().contains(QStringLiteral("including closed windows")) &&
                groupManager.contains(*traySpecifiedId),
            "tray specified deletion should open a detached application-modal confirmation");
    requireTrayModalCentered(specifiedModal,
                             "tray specified deletion should center on the cursor screen");
    specifiedModal->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(groupManager.contains(*traySpecifiedId),
            "canceling tray specified deletion should preserve the group");
    trayDeleteSpecified->trigger();
    specifiedModal = deletionModalNamed(QStringLiteral("pinnedWindowGroupDeleteSpecifiedModal"));
    require(specifiedModal != nullptr, "tray specified confirmation should reopen");
    specifiedModal->accept();
    require(!groupManager.contains(*traySpecifiedId),
            "accepting the tray specified-group item should delete its custom group");

    const auto trayCleanupId = groupManager.createGroup(QStringLiteral("Tray cleanup"));
    require(trayCleanupId.has_value(),
            "an empty custom group should be created for the tray cleanup state");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    trayDeleteEmpty = groupActionNamed(QStringLiteral("systemTrayDeleteEmptyGroupsAction"));
    require(trayDeleteEmpty != nullptr && trayDeleteEmpty->isEnabled(),
            "tray Delete Empty Groups should enable once an empty custom group exists");

    trayDeleteEmpty->trigger();
    auto* emptyModal = deletionModalNamed(QStringLiteral("pinnedWindowGroupDeleteEmptyModal"));
    require(emptyModal != nullptr &&
                emptyModal->acceptAccentRole() == adqt::widgets::AdButton::AccentRole::Danger &&
                emptyModal->text().contains(
                    QStringLiteral("no pinned windows other than closed ones")) &&
                emptyModal->text().contains(QStringLiteral("Closed pinned windows saved")) &&
                groupManager.contains(*trayCleanupId),
            "tray empty-group deletion should await confirmation");
    requireTrayModalCentered(emptyModal,
                             "tray empty-group deletion should center on the cursor screen");
    emptyModal->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(groupManager.contains(*trayCleanupId),
            "canceling tray empty-group deletion should preserve the group");
    trayDeleteEmpty->trigger();
    emptyModal = deletionModalNamed(QStringLiteral("pinnedWindowGroupDeleteEmptyModal"));
    require(emptyModal != nullptr, "tray empty-group confirmation should reopen");
    emptyModal->accept();
    require(!groupManager.contains(*trayCleanupId),
            "confirming tray empty-group deletion should remove the group");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    trayDeleteEmpty = groupActionNamed(QStringLiteral("systemTrayDeleteEmptyGroupsAction"));
    require(trayDeleteEmpty != nullptr && !trayDeleteEmpty->isEnabled(),
            "tray Delete Empty Groups should disable again after the cleanup");

    // The tray New Group action runs while the application has no active or
    // visible window; its dialog must still open and complete the creation.
    require(QApplication::activeWindow() == nullptr,
            "the tray-only state should have no active window");
    // Re-resolve the action: the group mutations above rebuild the group menu,
    // so actions captured earlier may have been destroyed.
    trayNewGroup = groupActionNamed(QStringLiteral("systemTrayNewGroupAction"));
    require(trayNewGroup != nullptr, "tray New Group should survive the menu rebuild");
    const auto visibleModalSurface = []() {
        QWidget* surface = nullptr;
        for (QWidget* widget : QApplication::allWidgets()) {
            if (widget != nullptr && widget->isVisible() &&
                widget->objectName() == QStringLiteral("ad-modal-overlay")) {
                surface = widget;
            }
        }
        return surface;
    };
    trayNewGroup->trigger();
    QApplication::processEvents();
    QWidget* createGroupSurface = visibleModalSurface();
    require(createGroupSurface != nullptr,
            "tray New Group must show its dialog when no application window is active");
    auto* groupNameInput =
        createGroupSurface->findChild<QLineEdit*>(QStringLiteral("pinnedWindowGroupNameInput"));
    require(groupNameInput != nullptr,
            "the create-group dialog should contain the group name input");
    groupNameInput->setText(QStringLiteral("TrayCreated"));
    const QList<QPushButton*> dialogButtons = createGroupSurface->findChildren<QPushButton*>();
    require(dialogButtons.size() == 2, "the create-group dialog should show two footer buttons");
    // The footer adds the reject button before the accept button, so the last
    // child button is the accept action.
    dialogButtons.last()->click();
    QApplication::processEvents();
    require(visibleModalSurface() == nullptr,
            "the create-group dialog should close after accepting");
    bool trayGroupCreated = false;
    for (const auto& trayGroup : groupManager.groups()) {
        trayGroupCreated = trayGroupCreated || trayGroup.name == QStringLiteral("TrayCreated");
    }
    require(trayGroupCreated, "accepting the tray New Group dialog should create the group");

#ifdef Q_OS_MACOS
    // Real status-item presentation and placement are covered by the Cocoa fixture.
#endif

    int screenshotRequests = 0;
    int showMainWindowRequests = 0;
    int restartRequests = 0;
    int exitRequests = 0;
    QVector<snow_shot::presentation::GlobalShortcutAction> quickActions;
    QObject::connect(&controller,
                     &snow_shot::presentation::SystemTrayController::screenshotRequested,
                     [&screenshotRequests]() { ++screenshotRequests; });
    QObject::connect(&controller,
                     &snow_shot::presentation::SystemTrayController::showMainWindowRequested,
                     [&showMainWindowRequests]() { ++showMainWindowRequests; });
    QObject::connect(&controller, &snow_shot::presentation::SystemTrayController::restartRequested,
                     [&restartRequests]() { ++restartRequests; });
    QObject::connect(&controller, &snow_shot::presentation::SystemTrayController::exitRequested,
                     [&exitRequests]() { ++exitRequests; });
    QObject::connect(&controller,
                     &snow_shot::presentation::SystemTrayController::quickActionRequested,
                     [&quickActions](snow_shot::presentation::GlobalShortcutAction action) {
                         quickActions.push_back(action);
                     });

    int functionSettingsRequests = 0;
    QObject::connect(&controller,
                     &snow_shot::presentation::SystemTrayController::openFunctionSettingsRequested,
                     [&functionSettingsRequests]() { ++functionSettingsRequests; });
    int aboutRequests = 0;
    QObject::connect(&controller,
                     &snow_shot::presentation::SystemTrayController::openAboutRequested,
                     [&aboutRequests]() { ++aboutRequests; });
    require(controller.middleClickAction() == QStringLiteral("screenshot_fixed"),
            "middle click must default to capture and pin");
    const QStringList clickActions{QStringLiteral("screenshot"), QStringLiteral("show_main_window"),
                                   QStringLiteral("screenshot_copy"),
                                   QStringLiteral("screenshot_fixed"),
                                   QStringLiteral("open_function_settings")};
    for (const auto reason : {QSystemTrayIcon::Trigger, QSystemTrayIcon::MiddleClick}) {
        for (const auto& action : clickActions) {
            if (reason == QSystemTrayIcon::Trigger) {
                controller.setLeftClickAction(action);
            } else {
                controller.setMiddleClickAction(action);
            }
            screenshotRequests = showMainWindowRequests = functionSettingsRequests = 0;
            quickActions.clear();
            trayIcon->activated(reason);
            require(!menu->isPopupVisible(), "tray click actions must not open the context menu");
            require(screenshotRequests == (action == QStringLiteral("screenshot") ? 1 : 0) &&
                        showMainWindowRequests ==
                            (action == QStringLiteral("show_main_window") ? 1 : 0) &&
                        functionSettingsRequests ==
                            (action == QStringLiteral("open_function_settings") ? 1 : 0),
                    "each tray button must dispatch exactly the selected dedicated request");
            QVector<snow_shot::presentation::GlobalShortcutAction> expected;
            if (action == QStringLiteral("screenshot_copy")) {
                expected.push_back(snow_shot::presentation::GlobalShortcutAction::ScreenshotCopy);
            } else if (action == QStringLiteral("screenshot_fixed")) {
                expected.push_back(snow_shot::presentation::GlobalShortcutAction::ScreenshotFixed);
            }
            require(quickActions == expected,
                    "copy and pin must dispatch their existing capture commands");
        }
    }
    screenshotRequests = showMainWindowRequests = functionSettingsRequests = 0;
    quickActions.clear();
    controller.setLeftClickAction(QStringLiteral("unsupported"));
    controller.setMiddleClickAction(QStringLiteral("unsupported"));
    require(controller.middleClickAction() == QStringLiteral("screenshot_fixed"),
            "invalid middle click must fall back to capture and pin");
    trayIcon->activated(QSystemTrayIcon::Trigger);
    trayIcon->activated(QSystemTrayIcon::DoubleClick);
    trayIcon->activated(QSystemTrayIcon::MiddleClick);
    trayIcon->activated(QSystemTrayIcon::Unknown);
    require(screenshotRequests == 1, "only a left-click trigger should request a screenshot");
    require(quickActions ==
                    QVector<snow_shot::presentation::GlobalShortcutAction>{
                        snow_shot::presentation::GlobalShortcutAction::ScreenshotFixed} &&
                showMainWindowRequests == 0 && functionSettingsRequests == 0,
            "middle click must pin once and unrelated activation reasons must do nothing");
    quickActions.clear();

    controller.setLeftClickAction(QStringLiteral("show_main_window"));
    require(controller.leftClickAction() == QStringLiteral("show_main_window"),
            "the configured show-window left-click action should be retained");
    trayIcon->activated(QSystemTrayIcon::Trigger);
    require(screenshotRequests == 1 && showMainWindowRequests == 1,
            "the configured tray left-click should request the main window");
    controller.setLeftClickAction(QStringLiteral("unsupported"));
    require(controller.leftClickAction() == QStringLiteral("screenshot"),
            "an unsupported tray left-click action should fall back to Screenshot");

    const int screenshotRequestsBeforeMessageClicks = screenshotRequests;
    const int showMainWindowRequestsBeforeMessageClicks = showMainWindowRequests;
    const int functionSettingsRequestsBeforeMessageClicks = functionSettingsRequests;
    controller.showUpdateMessage(QStringLiteral("Snow Shot 2.0.0 is available."));
    clickNotification(controller, trayIcon,
                      snow_shot::platform::SystemNotificationAction::OpenAbout);
    require(aboutRequests == 1 && screenshotRequests == screenshotRequestsBeforeMessageClicks &&
                showMainWindowRequests == showMainWindowRequestsBeforeMessageClicks &&
                functionSettingsRequests == functionSettingsRequestsBeforeMessageClicks,
            "clicking a new-version system notification must request only the About page");
    controller.showCaptureMessage(QStringLiteral("Capture failed"), false);
    controller.showWarningMessage(QStringLiteral("Feature unavailable"),
                                  QStringLiteral("Screenshot is unavailable"));
    clickNotification(controller, trayIcon, snow_shot::platform::SystemNotificationAction::None);
    require(aboutRequests == 1,
            "capture and warning balloon clicks must not request the About page");
    controller.setEnabled(false);
    controller.showUpdateMessage(QStringLiteral("Update with tray hidden"));
    clickNotification(controller, trayIcon,
                      snow_shot::platform::SystemNotificationAction::OpenAbout);
    controller.setEnabled(true);
#ifdef Q_OS_MACOS
    require(aboutRequests == 2, "native notification actions must remain valid with a hidden tray");
#else
    require(aboutRequests == 1,
            "a balloon suppressed while the tray is disabled must stay unclickable");
#endif

    screenshotMenuAction->trigger();
    showMainWindowMenuAction->trigger();
    restartMenuAction->trigger();
    exitMenuAction->trigger();
    require(quickActions ==
                QVector<snow_shot::presentation::GlobalShortcutAction>{
                    snow_shot::presentation::GlobalShortcutAction::Screenshot},
            "generated tray actions should emit their catalog shortcut commands");
    require(screenshotRequests == 1 && showMainWindowRequests == 2,
            "Show main interface should emit the dedicated tray request");
    require(restartRequests == 1, "Restart App should emit its dedicated tray request once");
    require(exitRequests == 1, "the Exit action should emit its request");

    hotkeyToggleMenuAction->trigger();
    require(quickActions ==
                    QVector<snow_shot::presentation::GlobalShortcutAction>{
                        snow_shot::presentation::GlobalShortcutAction::Screenshot,
                        snow_shot::presentation::GlobalShortcutAction::ToggleGlobalHotkeys} &&
                hotkeyToggleMenuAction->isChecked(),
            "the hotkey toggle tray entry should dispatch its command and check its state");
    controller.setQuickActionChecked(
        snow_shot::presentation::GlobalShortcutAction::ToggleGlobalHotkeys, false);
    controller.setQuickActionChecked(
        snow_shot::presentation::GlobalShortcutAction::ToggleGlobalHotkeys, true);
    require(hotkeyToggleMenuAction->isChecked() && quickActions.size() == 2,
            "the manager-driven check sync must never redispatch the command");
    controller.setQuickActionChecked(
        snow_shot::presentation::GlobalShortcutAction::ToggleGlobalHotkeys, false);
    require(!hotkeyToggleMenuAction->isChecked() && quickActions.size() == 2,
            "the manager-driven check sync must mirror the enabled state");
    controller.setQuickActionChecked(
        snow_shot::presentation::GlobalShortcutAction::ToggleGlobalHotkeys, true);
    controller.setMenuOptions({QStringLiteral("quick.screenshot"), QStringLiteral("tray.exit")});
    const QList<QAction*> compactVisibleActions = visibleActions();
    require(compactVisibleActions.size() == 3 && compactVisibleActions.at(1)->isSeparator() &&
                !windowGroupMenuAction->isVisible() && !hotkeyToggleMenuAction->isVisible() &&
                !hotkeyToggleMenuAction->isChecked() && quickActions.size() == 3 &&
                quickActions.last() ==
                    snow_shot::presentation::GlobalShortcutAction::ToggleGlobalHotkeys,
            "hiding the checked toggle should re-enable hotkeys and collapse empty groups");
    controller.setMenuOptions(defaultMenuOptions);
    require(windowGroupMenuAction->isVisible() && hotkeyToggleMenuAction->isVisible() &&
                !hotkeyToggleMenuAction->isChecked(),
            "restoring the defaults should bring the window group submenu back");

    const QString fullscreenToggleId =
        QStringLiteral("quick.toggle-disable-on-focused-fullscreen-window");
    controller.setMenuOptions({fullscreenToggleId, QStringLiteral("tray.exit")});
    QAction* fullscreenToggleMenuAction = actionForId(fullscreenToggleId);
    require(fullscreenToggleMenuAction != nullptr && fullscreenToggleMenuAction->isCheckable() &&
                !fullscreenToggleMenuAction->isChecked(),
            "the fullscreen suppression tray entry should be a checkable view of the setting");
    controller.setQuickActionChecked(
        snow_shot::presentation::GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow,
        true);
    require(fullscreenToggleMenuAction->isChecked() && quickActions.size() == 3,
            "the store-driven check sync must mirror suppression without redispatching");
    fullscreenToggleMenuAction->trigger();
    require(quickActions.size() == 4 && quickActions.last() ==
                                            snow_shot::presentation::GlobalShortcutAction::
                                                ToggleDisableOnFocusedFullscreenWindow,
            "clicking the fullscreen suppression entry should dispatch its quick action");
    controller.setQuickActionChecked(
        snow_shot::presentation::GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow,
        false);
    require(!fullscreenToggleMenuAction->isChecked(),
            "clearing fullscreen suppression should uncheck the tray entry");
    controller.setMenuOptions(defaultMenuOptions);
    require(!fullscreenToggleMenuAction->isVisible() && quickActions.size() == 4,
            "hiding the fullscreen suppression entry must not redispatch its quick action");
    for (const auto& entry :
         {std::pair{QStringLiteral("quick.open-pin-to-screen-management"),
                    snow_shot::presentation::GlobalShortcutAction::OpenPinToScreenManagement},
          std::pair{QStringLiteral("quick.global-canvas"),
                    snow_shot::presentation::GlobalShortcutAction::GlobalCanvas}}) {
        auto* optionalAction = actionForId(entry.first);
        require(optionalAction != nullptr && !optionalAction->isVisible(),
                "management and canvas tray actions must start hidden");
        controller.setMenuOptions({entry.first, QStringLiteral("tray.exit")});
        require(optionalAction->isVisible(), "selected optional tray action must be visible");
        const auto previousCount = quickActions.size();
        optionalAction->trigger();
        require(quickActions.size() == previousCount + 1 && quickActions.last() == entry.second,
                "optional tray action must dispatch the corresponding command");
        controller.setMenuOptions(defaultMenuOptions);
        require(!optionalAction->isVisible() && quickActions.size() == previousCount + 1,
                "restoring defaults must hide optional actions without dispatching");
    }
    require(groupManager.setActiveGroup(QStringLiteral("default")),
            "the default group should be activatable for the localized title check");

    require(languageManager.setLanguage(QStringLiteral("zh_CN")),
            "the Simplified Chinese translation should load");
    controller.showUpdateMessage(QStringLiteral("An update is ready."));
    requireBalloon(
        trayIcon, QStringLiteral("\u66f4\u65b0"), QStringLiteral("An update is ready."),
        QSystemTrayIcon::Information,
        "an update-ready balloon should keep an informational icon in Simplified Chinese");
    requireActionText(screenshotMenuAction,
                      QStringLiteral("\u622a\u56fe\t") + screenshotShortcutHint,
                      "Screenshot should translate to Simplified Chinese");
    requireActionText(
        delayedScreenshotMenuAction,
        snow_shot::presentation::settings::builtInSettingsRegistry().catalog().shortcutActionTitle(
            snow_shot::presentation::GlobalShortcutAction::ScreenshotDelay, 3),
        "Simplified Chinese tray text should equal the canonical shortcut title");
    requireActionText(
        recordingToggleMenuAction,
        snow_shot::presentation::settings::builtInSettingsRegistry().catalog().shortcutActionTitle(
            snow_shot::presentation::GlobalShortcutAction::ScreenRecordCopy),
        "Simplified Chinese recording text should equal the canonical shortcut title");
    requireActionText(showMainWindowMenuAction, QStringLiteral("\u663e\u793a\u4e3b\u754c\u9762"),
                      "Show main interface should translate to Simplified Chinese");
    requireActionText(hotkeyToggleMenuAction,
                      QStringLiteral("\u7981\u7528\u5168\u5c40\u5feb\u6377\u952e"),
                      "Disable global hotkeys should translate to Simplified Chinese");
    requireActionText(exitMenuAction, QStringLiteral("\u9000\u51fa"),
                      "Exit should translate to Simplified Chinese");
    // The window group submenu title resolves through the SystemTrayController
    // catalog while the built-in group name comes from the namespaced
    // PinnedWindowGroupManager tr() context; both must follow the language.
    requireActionText(windowGroupMenuAction,
                      QStringLiteral("\u7a97\u53e3\u5206\u7ec4\uff1a\u9ed8\u8ba4"),
                      "the window group submenu title should translate to Simplified Chinese");
    requireGroupAction(windowGroupMenu,
                       groupActionNamed(QStringLiteral("systemTrayGroupAction-default")),
                       QStringLiteral("\u9ed8\u8ba4"), QStringLiteral("0/0"),
                       "the default group entry should translate to Simplified Chinese");
    requireActionText(groupActionNamed(QStringLiteral("systemTrayNewGroupAction")),
                      QStringLiteral("\u65b0\u5efa\u5206\u7ec4"),
                      "tray New Group should translate to Simplified Chinese");
    requireActionText(groupActionNamed(QStringLiteral("systemTrayDeleteEmptyGroupsAction")),
                      QStringLiteral("\u5220\u9664\u7a7a\u5206\u7ec4"),
                      "tray Delete Empty Groups should translate to Simplified Chinese");
    requireActionText(windowGroupMenu->findChild<QAction*>(
                          QStringLiteral("systemTrayDeleteSpecifiedGroupAction")),
                      QStringLiteral("\u5220\u9664\u6307\u5b9a\u5206\u7ec4"),
                      "tray Delete Specified Group should translate to Simplified Chinese");
    auto* simplifiedDefaultDeletion =
        deleteSpecifiedActionNamed(QStringLiteral("systemTrayDeleteSpecifiedGroupAction-default"));
    requireGroupAction(
        trayDeleteSpecifiedMenu, simplifiedDefaultDeletion, QStringLiteral("\u9ed8\u8ba4"),
        QStringLiteral("0/0"),
        "tray specified deletion should translate its Default entry to Simplified Chinese");
    require(QString::fromLatin1(groupManager.metaObject()->className()) ==
                    QStringLiteral("snow_shot::presentation::PinnedWindowGroupManager") &&
                QCoreApplication::translate("snow_shot::presentation::PinnedWindowGroupManager",
                                            "Group name") ==
                    QStringLiteral("\u5206\u7ec4\u540d\u79f0"),
            "the group manager translation context should resolve its catalog entries");

    QAction* translatedDefaultDeletion =
        deleteSpecifiedActionNamed(QStringLiteral("systemTrayDeleteSpecifiedGroupAction-default"));
    require(translatedDefaultDeletion != nullptr,
            "the translated Default group should remain available for confirmation");
    translatedDefaultDeletion->trigger();
    auto* translatedModal =
        deletionModalNamed(QStringLiteral("pinnedWindowGroupDeleteSpecifiedModal"));
    require(translatedModal != nullptr &&
                translatedModal->windowTitle() ==
                    QStringLiteral("\u6e05\u7a7a\u9ed8\u8ba4\u5206\u7ec4"),
            "the Default deletion modal should open in Simplified Chinese");
    require(languageManager.setLanguage(QStringLiteral("zh_TW")),
            "the Traditional Chinese translation should load");
    require(translatedModal->windowTitle() ==
                QStringLiteral("\u6e05\u7a7a\u9810\u8a2d\u7fa4\u7d44"),
            "an open group deletion modal should retranslate to Traditional Chinese");
    translatedModal->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    controller.showUpdateMessage(QStringLiteral("An update is ready."));
    requireBalloon(
        trayIcon, QStringLiteral("\u66f4\u65b0"), QStringLiteral("An update is ready."),
        QSystemTrayIcon::Information,
        "an update-ready balloon should keep an informational icon in Traditional Chinese");
    requireActionText(screenshotMenuAction,
                      QStringLiteral("\u622a\u5716\t") + screenshotShortcutHint,
                      "Screenshot should translate to Traditional Chinese");
    requireActionText(
        delayedScreenshotMenuAction,
        snow_shot::presentation::settings::builtInSettingsRegistry().catalog().shortcutActionTitle(
            snow_shot::presentation::GlobalShortcutAction::ScreenshotDelay, 3),
        "Traditional Chinese tray text should equal the canonical shortcut title");
    requireActionText(
        recordingToggleMenuAction,
        snow_shot::presentation::settings::builtInSettingsRegistry().catalog().shortcutActionTitle(
            snow_shot::presentation::GlobalShortcutAction::ScreenRecordCopy),
        "Traditional Chinese recording text should equal the canonical shortcut title");
    requireActionText(showMainWindowMenuAction, QStringLiteral("\u986f\u793a\u4e3b\u4ecb\u9762"),
                      "Show main interface should translate to Traditional Chinese");
    requireActionText(hotkeyToggleMenuAction,
                      QStringLiteral("\u505c\u7528\u5168\u57df\u5feb\u901f\u9375"),
                      "Disable global hotkeys should translate to Traditional Chinese");
    requireActionText(exitMenuAction, QStringLiteral("\u7d50\u675f"),
                      "Exit should translate to Traditional Chinese");
    requireActionText(windowGroupMenuAction,
                      QStringLiteral("\u8996\u7a97\u7fa4\u7d44\uff1a\u9810\u8a2d"),
                      "the window group submenu title should translate to Traditional Chinese");
    requireGroupAction(windowGroupMenu,
                       groupActionNamed(QStringLiteral("systemTrayGroupAction-default")),
                       QStringLiteral("\u9810\u8a2d"), QStringLiteral("0/0"),
                       "the default group entry should translate to Traditional Chinese");
    requireActionText(groupActionNamed(QStringLiteral("systemTrayNewGroupAction")),
                      QStringLiteral("\u65b0\u589e\u7fa4\u7d44"),
                      "tray New Group should translate to Traditional Chinese");
    requireActionText(groupActionNamed(QStringLiteral("systemTrayDeleteEmptyGroupsAction")),
                      QStringLiteral("\u522a\u9664\u7a7a\u7fa4\u7d44"),
                      "tray Delete Empty Groups should translate to Traditional Chinese");
    requireActionText(windowGroupMenu->findChild<QAction*>(
                          QStringLiteral("systemTrayDeleteSpecifiedGroupAction")),
                      QStringLiteral("\u522a\u9664\u6307\u5b9a\u7fa4\u7d44"),
                      "tray Delete Specified Group should translate to Traditional Chinese");
    auto* traditionalDefaultDeletion =
        deleteSpecifiedActionNamed(QStringLiteral("systemTrayDeleteSpecifiedGroupAction-default"));
    requireGroupAction(
        trayDeleteSpecifiedMenu, traditionalDefaultDeletion, QStringLiteral("\u9810\u8a2d"),
        QStringLiteral("0/0"),
        "tray specified deletion should translate its Default entry to Traditional Chinese");
    controller.setGlobalShortcuts(snow_shot::presentation::GlobalShortcutAction::Screenshot, {});
    requireActionText(screenshotMenuAction, QStringLiteral("\u622a\u5716"),
                      "clearing a global shortcut should remove its tray menu hint");
    menu->dismissPopup();
    drainMenus();
    const auto beforeContext = quickActions.size();
    trayIcon->activated(QSystemTrayIcon::Context);
#ifndef Q_OS_MACOS
    QPointer<adqt::widgets::AdContextMenu> requested = controller.createContextMenu();
    require(requested && requested->isPopupVisible(), "a tray context request creates a popup");
    requested->dismissPopup();
    drainMenus();
    require(!requested, "a tray context session releases its menu after dismissal");
#else
    drainMenus();
    require(!hasTrayMenu(), "a synthetic Context cancels its unpresented native session");
#endif
    require(quickActions.size() == beforeContext, "right-click never dispatches a click action");
    verifyTraySkins(controller, storageDirectory);
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
