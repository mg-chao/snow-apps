#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "widgets/context_menu.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QImage>
#include <QKeySequence>
#include <QMenu>
#include <QString>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <cstdlib>
#include <functional>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void requireActionText(const QAction* action, const QString& expected, const char* message) {
    require(action != nullptr && action->text() == expected, message);
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}

} // namespace

int main(int argc, char* argv[]) {
    const QString applicationName =
        QStringLiteral("system-tray-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(applicationName);

    QApplication application(argc, argv);
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "temporary storage directory should be available");
    static_cast<void>(snow_shot::storage::ApplicationStorage::instance().initialize(
        {storageDirectory.path(), storageDirectory.path(), 8000}));
    auto& languageManager = snow_shot::presentation::LanguageManager::instance();
    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English should be available from the English catalog");

    snow_shot::presentation::SystemTrayController controller;
    auto* trayIcon =
        controller.findChild<QSystemTrayIcon*>(QStringLiteral("snowShotSystemTrayIcon"));
    require(trayIcon != nullptr, "the controller should own a system tray icon");
    require(!trayIcon->icon().isNull(), "the bundled tray icon should load");
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

    const QStringList bundledSelections{
        QStringLiteral("default"),      QStringLiteral("light"),
        QStringLiteral("dark"),         QStringLiteral("snow-default"),
        QStringLiteral("snow-light"),   QStringLiteral("snow-dark"),
    };
    for (const QString& selection : bundledSelections) {
        controller.setIconSelection(selection);
        require(controller.iconSelection() == selection,
                "each supported tray icon selection should be retained");
        require(trayIcon->property("resolvedIconSource").toString() ==
                    QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-%1.png")
                        .arg(selection),
                "each tray icon selection should resolve to its bundled asset");
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
    require(controller.customIconPath() == customIconPath &&
                trayIcon->icon().pixmap(QSize(64, 64)).toImage().pixelColor(32, 32) ==
                    QColor(242, 17, 137),
            "a valid custom image should replace the bundled tray icon");
    const qulonglong firstDecodeCount =
        trayIcon->property("customIconDecodeCount").toULongLong();
    const qulonglong firstHitCount =
        trayIcon->property("customIconCacheHits").toULongLong();
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
    require(trayIcon->property("customIconDecodeCount").toULongLong() ==
                    firstDecodeCount + 1 &&
                trayIcon->property("customIconSourcePixelSize").toSize() == QSize(80, 40) &&
                trayIcon->icon().pixmap(QSize(80, 40)).toImage().pixelColor(40, 20) ==
                    QColor(17, 113, 229),
            "a changed source fingerprint should replace the retained custom raster");

    const QString largeIconPath = storageDirectory.filePath(QStringLiteral("large-icon.png"));
    QImage largeImage(1024, 512, QImage::Format_ARGB32_Premultiplied);
    largeImage.fill(QColor(31, 173, 91));
    require(largeImage.save(largeIconPath), "the large tray icon fixture should be writable");
    controller.setCustomIconPath(largeIconPath);
    require(trayIcon->property("customIconSourcePixelSize").toSize() == QSize(1024, 512) &&
                trayIcon->property("customIconDecodedPixelSize").toSize() == QSize(256, 128),
            "a large custom image should retain no raster larger than 256 by 256");

    const QString icoPath =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../resources/app-icon.ico"));
    controller.setCustomIconPath(icoPath);
    require(trayIcon->property("customIconSourcePixelSize").toSize() == QSize(256, 256) &&
                trayIcon->property("customIconDecodedPixelSize").toSize() == QSize(256, 256),
            "ICO loading should select the available frame nearest 256 by 256");

    controller.setIconSelection(QStringLiteral("light"));
    const QString oversizedIconPath =
        storageDirectory.filePath(QStringLiteral("oversized-icon.png"));
    QImage oversizedImage(20000, 1, QImage::Format_ARGB32_Premultiplied);
    oversizedImage.fill(QColor(213, 71, 56));
    require(oversizedImage.save(oversizedIconPath),
            "the oversized tray icon fixture should be writable");
    controller.setCustomIconPath(oversizedIconPath);
    const qulonglong oversizedHitCount =
        trayIcon->property("customIconCacheHits").toULongLong();
    const qulonglong oversizedMissCount =
        trayIcon->property("customIconCacheMisses").toULongLong();
    const qulonglong oversizedDecodeCount =
        trayIcon->property("customIconDecodeCount").toULongLong();
    require(trayIcon->property("resolvedIconSource").toString() ==
                    QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-light.png") &&
                trayIcon->property("customIconSourcePixelSize").toSize() == QSize(20000, 1) &&
                trayIcon->property("customIconDecodedPixelSize").toSize().isEmpty(),
            "pathological source dimensions should be rejected before pixel allocation");
    controller.show();
    controller.show();
    require(trayIcon->property("customIconCacheHits").toULongLong() >=
                    oversizedHitCount + 2 &&
                trayIcon->property("customIconCacheMisses").toULongLong() ==
                    oversizedMissCount &&
                trayIcon->property("customIconDecodeCount").toULongLong() ==
                    oversizedDecodeCount,
            "a rejected source fingerprint should remain cached across fallback rendering");

    controller.setCustomIconPath(storageDirectory.filePath(QStringLiteral("missing.png")));
    require(trayIcon->property("resolvedIconSource").toString() ==
                QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-light.png"),
            "an invalid custom image should fall back to the selected bundled tray icon");
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

    auto* menu = dynamic_cast<adqt::widgets::AdContextMenu*>(trayIcon->contextMenu());
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
    const auto visibleActions = [menu]() {
        QList<QAction*> result;
        for (QAction* action : menu->actions()) {
            if (action != nullptr && action->isVisible()) {
                result.push_back(action);
            }
        }
        return result;
    };
    const QStringList defaultMenuOptions = snow_shot::storage::TraySettings().menuOptions();
    controller.setMenuOptions(defaultMenuOptions);
    const QList<QAction*> defaultVisibleActions = visibleActions();
    int transientUiHiddenCount = 0;
    QObject::connect(&controller, &snow_shot::presentation::SystemTrayController::transientUiHidden,
                     [&transientUiHiddenCount]() { ++transientUiHiddenCount; });
    menu->popupAt(QPoint(100, 100));
    require(waitUntil([menu]() { return menu->isVisible(); }),
            "the tray context menu did not become visible for its hide lifecycle test");
    menu->hide();
    require(transientUiHiddenCount == 0,
            "tray transient cleanup was emitted synchronously from aboutToHide");
    require(waitUntil([&transientUiHiddenCount]() { return transientUiHiddenCount == 1; }),
            "hiding the tray context menu did not publish deferred transient cleanup");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(transientUiHiddenCount == 1,
            "one tray context-menu hide published transient cleanup more than once");
    auto* screenshotMenuAction = actionForId(QStringLiteral("quick.screenshot"));
    auto* delayedScreenshotMenuAction =
        actionForId(QStringLiteral("quick.screenshot-delay"));
    auto* recordingToggleMenuAction =
        actionForId(QStringLiteral("quick.screen-record-copy"));
    auto* disableMenuAction =
        actionForId(QStringLiteral("tray.disable-shortcut-functions"));
    auto* showMainWindowMenuAction =
        actionForId(QStringLiteral("tray.show-main-window"));
    auto* exitMenuAction = actionForId(QStringLiteral("tray.exit"));
    require(controller.menuOptions() == defaultMenuOptions && defaultVisibleActions.size() == 13 &&
                screenshotMenuAction != nullptr && screenshotMenuAction->isVisible() &&
                delayedScreenshotMenuAction != nullptr &&
                delayedScreenshotMenuAction->isVisible() &&
                recordingToggleMenuAction != nullptr &&
                !recordingToggleMenuAction->isVisible() &&
                !screenshotMenuAction->icon().isNull() && disableMenuAction != nullptr &&
                disableMenuAction->isVisible() && disableMenuAction->isCheckable() &&
                !disableMenuAction->isChecked() && showMainWindowMenuAction != nullptr &&
                showMainWindowMenuAction->isVisible() &&
                !showMainWindowMenuAction->icon().isNull() && exitMenuAction != nullptr &&
                exitMenuAction->isVisible() && !exitMenuAction->icon().isNull() &&
                defaultVisibleActions.at(5)->isSeparator() &&
                defaultVisibleActions.at(7)->isSeparator() &&
                defaultVisibleActions.at(9)->isSeparator() &&
                defaultVisibleActions.at(10) == disableMenuAction &&
                defaultVisibleActions.at(11) == showMainWindowMenuAction,
            "the tray menu should expose the ten default options in four catalog groups");
    requireActionText(screenshotMenuAction, QStringLiteral("Screenshot"),
                      "Screenshot should use its catalog label");
    requireActionText(delayedScreenshotMenuAction, QStringLiteral("Delay 3s to Execute"),
                      "Delayed screenshot should use the canonical shortcut title");
    requireActionText(recordingToggleMenuAction,
                      QStringLiteral("Start Screen Recording / Stop and Copy Video"),
                      "Recording toggle should use the canonical shortcut title");
    const QString screenshotShortcut = QStringLiteral("Ctrl+Alt+1");
    const QString alternateScreenshotShortcut = QStringLiteral("Meta+Shift+S");
    const auto nativeShortcut = [](const QString& portableShortcut) {
        return QKeySequence::fromString(portableShortcut, QKeySequence::PortableText)
            .toString(QKeySequence::NativeText);
    };
    const QString screenshotShortcutHint = nativeShortcut(screenshotShortcut) +
                                           QStringLiteral(" / ") +
                                           nativeShortcut(alternateScreenshotShortcut);
    controller.setGlobalShortcuts(snow_shot::presentation::GlobalShortcutAction::Screenshot,
                                  {screenshotShortcut, alternateScreenshotShortcut});
    requireActionText(screenshotMenuAction, QStringLiteral("Screenshot\t") + screenshotShortcutHint,
                      "quick tray actions should display all configured global shortcuts");
    require(screenshotMenuAction->shortcut().isEmpty(),
            "displayed global shortcuts must not become menu-local shortcuts");
    controller.setScreenshotDelaySeconds(7);
    require(controller.screenshotDelaySeconds() == 7,
            "the tray should retain a normalized screenshot delay value");
    requireActionText(delayedScreenshotMenuAction, QStringLiteral("Delay 7s to Execute"),
                      "the tray should refresh the canonical delayed screenshot title");
    controller.setScreenshotDelaySeconds(3);
    requireActionText(showMainWindowMenuAction, QStringLiteral("Show Main Interface"),
                      "Show Main Interface should follow Disable Shortcut Functions");
    requireActionText(exitMenuAction, QStringLiteral("Exit"), "Exit should be last");

    int screenshotRequests = 0;
    int showMainWindowRequests = 0;
    int exitRequests = 0;
    int disableChanges = 0;
    bool shortcutsDisabled = false;
    QVector<snow_shot::presentation::GlobalShortcutAction> quickActions;
    QObject::connect(&controller,
                     &snow_shot::presentation::SystemTrayController::screenshotRequested,
                     [&screenshotRequests]() { ++screenshotRequests; });
    QObject::connect(&controller,
                     &snow_shot::presentation::SystemTrayController::showApplicationInterfaceRequested,
                     [&showMainWindowRequests]() { ++showMainWindowRequests; });
    QObject::connect(&controller, &snow_shot::presentation::SystemTrayController::exitRequested,
                     [&exitRequests]() { ++exitRequests; });
    QObject::connect(
        &controller, &snow_shot::presentation::SystemTrayController::quickActionRequested,
        [&quickActions](snow_shot::presentation::GlobalShortcutAction action) {
            quickActions.push_back(action);
        });
    QObject::connect(
        &controller,
        &snow_shot::presentation::SystemTrayController::shortcutFunctionsDisabledChanged,
        [&disableChanges, &shortcutsDisabled](bool disabled) {
            ++disableChanges;
            shortcutsDisabled = disabled;
        });

    trayIcon->activated(QSystemTrayIcon::Trigger);
    trayIcon->activated(QSystemTrayIcon::Context);
    trayIcon->activated(QSystemTrayIcon::DoubleClick);
    trayIcon->activated(QSystemTrayIcon::MiddleClick);
    trayIcon->activated(QSystemTrayIcon::Unknown);
    require(screenshotRequests == 1, "only a left-click trigger should request a screenshot");

    controller.setLeftClickAction(QStringLiteral("show_main_window"));
    require(controller.leftClickAction() == QStringLiteral("show_main_window"),
            "the configured show-window left-click action should be retained");
    trayIcon->activated(QSystemTrayIcon::Trigger);
    require(screenshotRequests == 1 && showMainWindowRequests == 1,
            "the configured tray left-click should request the main window");
    controller.setLeftClickAction(QStringLiteral("unsupported"));
    require(controller.leftClickAction() == QStringLiteral("screenshot"),
            "an unsupported tray left-click action should fall back to Screenshot");

    screenshotMenuAction->trigger();
    showMainWindowMenuAction->trigger();
    exitMenuAction->trigger();
    require(quickActions ==
                QVector<snow_shot::presentation::GlobalShortcutAction>{
                    snow_shot::presentation::GlobalShortcutAction::Screenshot},
            "generated tray actions should emit their catalog shortcut commands");
    require(screenshotRequests == 1 && showMainWindowRequests == 2,
            "Show Main Interface should emit the dedicated tray request");
    require(exitRequests == 1, "the Exit action should emit its request");

    disableMenuAction->trigger();
    require(controller.shortcutFunctionsDisabled() && disableChanges == 1 && shortcutsDisabled,
            "the disable command should expose its checked session state");
    controller.setMenuOptions({QStringLiteral("quick.screenshot"), QStringLiteral("tray.exit")});
    const QList<QAction*> compactVisibleActions = visibleActions();
    require(!controller.shortcutFunctionsDisabled() && disableChanges == 2 &&
                !shortcutsDisabled && compactVisibleActions.size() == 3 &&
                compactVisibleActions.at(1)->isSeparator(),
            "hiding the disable command should re-enable shortcuts and collapse empty groups");
    controller.setMenuOptions(defaultMenuOptions);

    require(languageManager.setLanguage(QStringLiteral("zh_CN")),
            "the Simplified Chinese translation should load");
    requireActionText(screenshotMenuAction,
                      QStringLiteral("\u622a\u56fe\t") + screenshotShortcutHint,
                       "Screenshot should translate to Simplified Chinese");
    requireActionText(
        delayedScreenshotMenuAction,
        snow_shot::presentation::settings::builtInSettingsCatalog().shortcutActionTitle(
            snow_shot::presentation::GlobalShortcutAction::ScreenshotDelay, 3),
        "Simplified Chinese tray text should equal the canonical shortcut title");
    requireActionText(
        recordingToggleMenuAction,
        snow_shot::presentation::settings::builtInSettingsCatalog().shortcutActionTitle(
            snow_shot::presentation::GlobalShortcutAction::ScreenRecordCopy),
        "Simplified Chinese recording text should equal the canonical shortcut title");
    requireActionText(showMainWindowMenuAction, QStringLiteral("\u663e\u793a\u4e3b\u754c\u9762"),
                       "Show Main Interface should translate to Simplified Chinese");
    requireActionText(disableMenuAction, QStringLiteral("\u7981\u7528\u5feb\u6377\u529f\u80fd"),
                      "Disable Shortcut Functions should translate to Simplified Chinese");
    requireActionText(exitMenuAction, QStringLiteral("\u9000\u51fa"),
                       "Exit should translate to Simplified Chinese");

    require(languageManager.setLanguage(QStringLiteral("zh_TW")),
            "the Traditional Chinese translation should load");
    requireActionText(screenshotMenuAction,
                      QStringLiteral("\u622a\u5716\t") + screenshotShortcutHint,
                       "Screenshot should translate to Traditional Chinese");
    requireActionText(
        delayedScreenshotMenuAction,
        snow_shot::presentation::settings::builtInSettingsCatalog().shortcutActionTitle(
            snow_shot::presentation::GlobalShortcutAction::ScreenshotDelay, 3),
        "Traditional Chinese tray text should equal the canonical shortcut title");
    requireActionText(
        recordingToggleMenuAction,
        snow_shot::presentation::settings::builtInSettingsCatalog().shortcutActionTitle(
            snow_shot::presentation::GlobalShortcutAction::ScreenRecordCopy),
        "Traditional Chinese recording text should equal the canonical shortcut title");
    requireActionText(showMainWindowMenuAction, QStringLiteral("\u986f\u793a\u4e3b\u4ecb\u9762"),
                       "Show Main Interface should translate to Traditional Chinese");
    requireActionText(disableMenuAction, QStringLiteral("\u505c\u7528\u5feb\u6377\u529f\u80fd"),
                      "Disable Shortcut Functions should translate to Traditional Chinese");
    requireActionText(exitMenuAction, QStringLiteral("\u7d50\u675f"),
                       "Exit should translate to Traditional Chinese");
    controller.setGlobalShortcuts(
        snow_shot::presentation::GlobalShortcutAction::Screenshot, {});
    requireActionText(screenshotMenuAction, QStringLiteral("\u622a\u5716"),
                      "clearing a global shortcut should remove its tray menu hint");
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
