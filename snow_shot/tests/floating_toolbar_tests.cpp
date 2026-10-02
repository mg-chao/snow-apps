#include "snow_shot/presentation/floatingtoolbarcontroller.h"
#include "snow_shot/presentation/floatingtoolbarplacement.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/screenshotimagedrop.h"
#include "snow_shot/platform/focusedfullscreenwindow.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/floatingtoolbarsettings.h"
#include "widgets/button.h"
#include "widgets/popover.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QEnterEvent>
#include <QJsonDocument>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QDir>
#include <QFile>
#include <QUrl>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#endif

using namespace snow_shot::presentation;
namespace storage = snow_shot::storage;
namespace geometry = snow_shot::presentation::floating_toolbar;
namespace {
void require(bool result, const char* message) {
    if (!result) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void pump() {
    for (int iteration = 0; iteration < 5; ++iteration)
        QCoreApplication::processEvents();
}
QWidget* window(const char* name) {
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QLatin1String(name))
            return widget;
    return nullptr;
}
void timer(FloatingToolbarController& controller, const char* name) {
    auto* found = controller.findChild<QTimer*>(QString::fromLatin1(name));
    require(found, "named interaction timer exists");
    found->stop();
    QMetaObject::invokeMethod(found, "timeout", Qt::DirectConnection);
    pump();
}
void mouse(QWidget* target, QEvent::Type type, const QPoint& global, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(target->mapFromGlobal(global)), QPointF(global), button,
                      buttons, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}
void click(QWidget* target) {
    const QPoint global = target->mapToGlobal(target->rect().center());
    mouse(target, QEvent::MouseButtonPress, global, Qt::LeftButton, Qt::LeftButton);
    mouse(target, QEvent::MouseButtonRelease, global, Qt::LeftButton, Qt::NoButton);
    pump();
}
QMenu* context(QWidget* target) {
    QContextMenuEvent event(QContextMenuEvent::Mouse, target->rect().center(),
                            target->mapToGlobal(target->rect().center()));
    QApplication::sendEvent(target, &event);
    pump();
    return qobject_cast<QMenu*>(window("floatingToolbarContextMenu"));
}
void verifySettings() {
    const storage::FloatingToolbarSettings stored;
    require(!stored.enabled() && !stored.toolbarMode() && stored.hideInFullscreen() &&
                stored.hideDuringCapture(),
            "initial floating toolbar preferences");
    const auto defaults = storage::defaultFloatingToolbarLayout();
    require(storage::ScreenshotToolbarSettings().layout(
                storage::ScreenshotToolbarLayoutKind::FloatingTools) == defaults,
            "stored default layout equals editor default");
    require(defaults.positions.size() == 4 &&
                defaults.positions.back().back() == QStringLiteral("text-recognition"),
            "default row contains four positions with OCR as group entry");
    require(defaults.hidden.contains(QStringLiteral("save-as-file")), "save-as-file starts hidden");
    auto hidden = toolbar_layout::moveItemToHidden(
        defaults, storage::ScreenshotToolbarLayoutKind::FloatingTools, QStringLiteral("screenshot"),
        0);
    auto grouped = toolbar_layout::stackItemInPosition(
        hidden, storage::ScreenshotToolbarLayoutKind::FloatingTools, QStringLiteral("save-as-file"),
        0, 0);
    require(grouped.positions[0].contains(QStringLiteral("save-as-file")) &&
                grouped.hidden.contains(QStringLiteral("screenshot")),
            "floating editor supports hiding and regrouping");
    require(storage::ScreenshotToolbarSettings().setLayout(
                storage::ScreenshotToolbarLayoutKind::FloatingTools, grouped),
            "custom layout commits");
    require(storage::ScreenshotToolbarSettings().layout(
                storage::ScreenshotToolbarLayoutKind::FloatingTools) == grouped,
            "layout persists without changing group order");
    GlobalShortcutManager manager;
    settings::BuiltInSettingsBackend backend(manager);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    require(
        session.applySwitchValue(settings::SettingsSwitchBinding::FloatingToolbarEnabled, true) &&
            stored.enabled(),
        "settings runtime writes visibility");
    require(backend.resetSection(settings::SettingsSectionReset::FloatingToolbarBehavior) &&
                !stored.enabled(),
            "behavior reset turns toolbar off");
    require(backend.resetSection(settings::SettingsSectionReset::FloatingToolbarLayout),
            "layout reset succeeds");
    require(storage::ScreenshotToolbarSettings().layout(
                storage::ScreenshotToolbarLayoutKind::FloatingTools) == defaults,
            "reset restores hidden tools and grouping");
    const auto& catalog = settings::builtInSettingsRegistry().catalog();
    require(catalog.validationErrors().isEmpty(), "settings catalog validates");
    const auto* function = catalog.page(QStringLiteral("function-settings"));
    const auto* interfacePage = catalog.page(QStringLiteral("interface-settings"));
    require(function && interfacePage, "both settings pages exist");
    auto after = [](const auto& sections, const QString& before, const QString& next) {
        for (qsizetype index = 1; index < sections.size(); ++index)
            if (sections[index - 1].id == before && sections[index].id == next)
                return true;
        return false;
    };
    require(after(function->sections, QStringLiteral("tray-settings"),
                  QStringLiteral("floating-toolbar-settings")),
            "function category follows Tray");
    require(after(interfacePage->sections, QStringLiteral("pin-to-screen"),
                  QStringLiteral("floating-toolbar")),
            "interface category follows Pin to screen");
}
void verifyPlacement() {
    for (const QRect bounds :
         {QRect(-1920, -300, 1920, 1080), QRect(0, 0, 800, 600), QRect(1920, 200, 2560, 1400)}) {
        for (const int size : {38, 48, 72}) {
            const QSize extent(size, size);
            for (const QPointF fraction : {QPointF(0, 0), QPointF(1, 1), QPointF(0.37, 0.81)}) {
                const auto restored = geometry::restore(fraction, extent, bounds);
                require(bounds.contains(QRect(restored, extent)),
                        "restored icon is inside monitor work area");
                require(geometry::restore(geometry::remember(restored, extent, bounds), extent,
                                          bounds) == restored,
                        "placement round trips at differing scales and negative origins");
            }
            auto left = geometry::dock(bounds.topLeft() + QPoint(12, 80), extent, bounds);
            require(left.x() == bounds.left() &&
                        geometry::tucked(left, extent, bounds).x() == bounds.left() - size / 2,
                    "left edge hides exactly half");
            auto right = geometry::dock(bounds.topRight() + QPoint(-size - 10, 80), extent, bounds);
            require(geometry::rightSide(right, extent, bounds) &&
                        geometry::tucked(right, extent, bounds).x() == right.x() + size / 2,
                    "right edge hides exactly half");
            auto middle = geometry::restore({0.5, 0}, extent, bounds);
            require(geometry::tucked(middle, extent, bounds) == middle, "top edge does not tuck");
        }
    }
    require(geometry::constrain(QPoint(-300, 900), QSize(500, 90), QRect(0, 0, 120, 80)) ==
                QPoint(),
            "oversized content clamps deterministically on a constrained work area");
}
void verifyDropIdentity() {
    ScreenshotClipboardContentSnapshot first;
    first.detachedImage = QImage(5, 7, QImage::Format_RGB32);
    first.detachedImage.fill(Qt::red);
    auto decoded = decodeScreenshotImageDrop(first);
    require(decoded && decoded->sourceIdentity.isValid(),
            "image drops have duplicate-pin identities");
    auto second = first;
    second.detachedImage = first.detachedImage.convertToFormat(QImage::Format_RGBA8888);
    auto equivalent = decodeScreenshotImageDrop(second);
    require(equivalent && equivalent->sourceIdentity == decoded->sourceIdentity,
            "identical pixels across formats use the same duplicate-pin identity");
    second.detachedImage.setPixelColor(1, 1, Qt::blue);
    auto different = decodeScreenshotImageDrop(second);
    require(different && different->sourceIdentity != decoded->sourceIdentity,
            "different images do not collide in duplicate-pin preferences");
    require(!decodeScreenshotImageDrop(first, [] { return true; }),
            "cancelled image decoding is discarded");
}
void verifyCustomizedActions() {
    const storage::FloatingToolbarSettings stored;
    stored.setToolbarMode(true);
    stored.setHideInFullscreen(false);
    const auto ids = storage::floatingToolbarItemIds();
    const storage::ScreenshotToolbarLayout allTools{{ids}, {}};
    require(storage::ScreenshotToolbarSettings().setLayout(
                storage::ScreenshotToolbarLayoutKind::FloatingTools, allTools),
            "all floating tools can be combined into one position");
    FloatingToolbarController controller;
    QString selected;
    QObject::connect(&controller, &FloatingToolbarController::actionRequested, &controller,
                     [&](const QString& id) { selected = id; });
    stored.setEnabled(true);
    pump();
    auto* toolbar = window("floatingToolbarWindow");
    const auto popovers = toolbar->findChildren<adqt::widgets::AdPopover*>();
    require(popovers.size() == 1, "custom group uses one shared hover popover");
    for (const auto& id : ids) {
        popovers.front()->show();
        pump();
        auto* content = popovers.front()->contentWidget();
        require(content, "group options materialize on open");
        auto* item = content->findChild<adqt::widgets::AdButton*>(id);
        require(item, "every configured capture tool is reachable in its group");
        item->click();
        require(selected == id, "group dispatches its selected capture workflow");
        bool repeats = false;
        for (auto* button : toolbar->findChildren<adqt::widgets::AdButton*>())
            repeats |= button->property("floatingToolbarAction").toString() == id;
        require(repeats, "each selected tool becomes the repeated main action");
    }
    const auto snapshot = storage::ApplicationStorage::instance().configuration().snapshot();
    QTemporaryDir persisted;
    const auto path = persisted.filePath(QStringLiteral("configuration.json"));
    {
        storage::ConfigurationStore exported(path, false, true, 0);
        require(exported.applySnapshot(snapshot) && exported.flushNow().success,
                "floating preferences participate in configuration export");
    }
    storage::ConfigurationStore imported(path, true, true);
    require(imported.value(QStringLiteral("floating_toolbar/layout")) ==
                    snapshot.value(QStringLiteral("floating_toolbar/layout")) &&
                imported.value(QStringLiteral("floating_toolbar/mode")) ==
                    QStringLiteral("toolbar"),
            "mode and layout survive disk reload and configuration import");
    stored.setEnabled(false);
    pump();
    storage::ScreenshotToolbarSettings().setLayout(
        storage::ScreenshotToolbarLayoutKind::FloatingTools,
        storage::defaultFloatingToolbarLayout());
}
void nativeProbe() {
    auto* screen = QGuiApplication::primaryScreen();
    require(screen && QGuiApplication::platformName() != QStringLiteral("offscreen"),
            "native probe requires a desktop platform");
    // Keep the short-lived fixture above a test launcher's own desktop panel.
    QWidget backdrop(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    backdrop.setStyleSheet(QStringLiteral("background: #d41c9a;"));
    backdrop.setGeometry(screen->geometry());
    backdrop.showFullScreen();
    backdrop.raise();
    backdrop.activateWindow();
    pump();
#ifdef Q_OS_WIN
    SetForegroundWindow(reinterpret_cast<HWND>(backdrop.winId()));
#endif
    // Native activation and fullscreen presentation are asynchronous. Finish
    // the fixture's presentation before testing the synchronous hide boundary.
    QEventLoop presentation;
    QTimer::singleShot(250, &presentation, &QEventLoop::quit);
    presentation.exec();
#ifdef Q_OS_WIN
    const auto backdropHandle = reinterpret_cast<HWND>(backdrop.winId());
    if (GetForegroundWindow() != backdropHandle) {
        // Background test launchers cannot request foreground activation.
        // Click only our verified, empty fixture, and restore the pointer.
        RECT frame{};
        GetWindowRect(backdropHandle, &frame);
        POINT point{};
        bool found = false;
        for (const int x : {75, 25, 50, 90, 10}) {
            for (const int y : {75, 25, 50, 90, 10}) {
                const POINT candidate{frame.left + (frame.right - frame.left) * x / 100,
                                      frame.top + (frame.bottom - frame.top) * y / 100};
                if (WindowFromPoint(candidate) == backdropHandle) {
                    point = candidate;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
        require(found, "native focus fixture owns the click target");
        POINT previous{};
        GetCursorPos(&previous);
        SetCursorPos(point.x, point.y);
        INPUT input[2]{};
        input[0].type = INPUT_MOUSE;
        input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        input[1].type = INPUT_MOUSE;
        input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        require(SendInput(2, input, sizeof(INPUT)) == 2,
                "native fixture receives its activation click");
        pump();
        SetCursorPos(previous.x, previous.y);
    }
    require(GetForegroundWindow() == backdropHandle,
            "native fixture is foreground before testing focus preservation");
#endif
    const storage::FloatingToolbarSettings stored;
    stored.setToolbarMode(false);
    stored.setHideInFullscreen(false);
    stored.setHideDuringCapture(true);
    stored.setPlacement({{QStringLiteral("screen"), screen->name()},
                         {QStringLiteral("x"), 0.75},
                         {QStringLiteral("y"), 0.75}});
    FloatingToolbarController controller;
    stored.setEnabled(true);
    pump();
    auto* iconWindow = window("floatingToolbarIconWindow");
    require(iconWindow && iconWindow->isVisible(), "native icon is visible over ordinary windows");
    require(QApplication::activeWindow() == &backdrop,
            "showing floating surfaces does not steal activation");
    const QRect region = iconWindow->geometry();
    const auto capture = [&] {
        const auto local = region.topLeft() - screen->geometry().topLeft();
        return screen->grabWindow(0, local.x(), local.y(), region.width(), region.height())
            .toImage();
    };
    const auto before = capture();
    controller.setCaptureActive(QStringLiteral("native-probe"), true);
    const auto hidden = capture();
    require(!before.isNull() && !hidden.isNull() && before != hidden,
            "native capture sees the icon disappear before acquisition");
    const auto center = hidden.pixelColor(hidden.width() / 2, hidden.height() / 2);
    require(qAbs(center.red() - 212) < 4 && qAbs(center.green() - 28) < 4 &&
                qAbs(center.blue() - 154) < 4,
            "capture output contains the unobstructed backdrop instead of toolbar pixels");
    controller.setCaptureActive(QStringLiteral("native-probe"), false);
    pump();
#ifdef Q_OS_WIN
    require(GetForegroundWindow() == backdropHandle,
            "restoring the native toolbar does not steal focus");
    const auto style = GetWindowLongPtrW(reinterpret_cast<HWND>(iconWindow->winId()), GWL_EXSTYLE);
    require((style & WS_EX_TOOLWINDOW) && (style & WS_EX_TOPMOST) && !(style & WS_EX_APPWINDOW),
            "native toolbar stays above ordinary windows without a taskbar entry");
#endif
    if (!snow_shot::platform::focusedFullscreenWindowOnScreen(screen)) {
#ifdef Q_OS_WIN
        const auto foreground = GetForegroundWindow();
        RECT rect{};
        DwmGetWindowAttribute(foreground, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST),
                        reinterpret_cast<MONITORINFO*>(&monitor));
        std::cerr << "Fullscreen probe: foreground backdrop="
                  << (foreground == reinterpret_cast<HWND>(backdrop.winId()))
                  << " frame=" << rect.left << ',' << rect.top << ',' << rect.right << ','
                  << rect.bottom << " monitor=" << monitor.rcMonitor.left << ','
                  << monitor.rcMonitor.top << ',' << monitor.rcMonitor.right << ','
                  << monitor.rcMonitor.bottom << " qt=" << screen->name().toStdString()
                  << " win=" << QString::fromWCharArray(monitor.szDevice).toStdString() << '\n';
#endif
    }
    require(snow_shot::platform::focusedFullscreenWindowOnScreen(screen),
            "native detector recognizes fullscreen on the toolbar monitor");
    for (auto* other : QGuiApplication::screens())
        if (other != screen)
            require(!snow_shot::platform::focusedFullscreenWindowOnScreen(other),
                    "native fullscreen does not suppress another monitor");
    stored.setHideInFullscreen(true);
    pump();
    require(!iconWindow->isVisible(), "fullscreen preference hides the native surface");
    std::cout << "Native floating toolbar probe passed on " << QGuiApplication::screens().size()
              << " monitor(s).\n";
    stored.setEnabled(false);
    pump();
}
class FloatingTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (qstrcmp(context, "FloatingToolbar") == 0 && qstrcmp(source, "Screenshot") == 0)
            return QStringLiteral("Translated screenshot");
        return {};
    }
    bool isEmpty() const override {
        return false;
    }
};
void reveal(FloatingToolbarController& controller) {
    auto* icon = window("floatingToolbarIconWindow");
    require(icon && icon->isVisible(), "icon visible before hover");
    QEnterEvent event(QPointF(20, 20), QPointF(20, 20), QPointF(icon->mapToGlobal(QPoint(20, 20))));
    QApplication::sendEvent(icon, &event);
    timer(controller, "floatingToolbarRevealTimer");
}
void verifyWindows(const QString& visualDirectory) {
    bool fullscreen = false;
    int polls = 0;
    const storage::FloatingToolbarSettings stored;
    FloatingToolbarController controller(nullptr, [&](QScreen*) {
        ++polls;
        return fullscreen;
    });
    pump();
    require(!window("floatingToolbarIconWindow") && !window("floatingToolbarWindow"),
            "disabled toolbar creates no desktop surfaces");
    require(polls == 0, "disabled toolbar never polls fullscreen");
    stored.setEnabled(true);
    pump();
    auto* iconWindow = window("floatingToolbarIconWindow");
    require(iconWindow && iconWindow->isVisible() && !window("floatingToolbarWindow"),
            "first show creates only the icon");
    auto* icon =
        iconWindow->findChild<QAbstractButton*>(QStringLiteral("floatingToolbarSnowflake"));
    require(icon && icon->width() == 48, "normal icon has a 48-unit hit target");
    QStringList actions;
    QObject::connect(&controller, &FloatingToolbarController::actionRequested, &controller,
                     [&](const QString& action) { actions << action; });
    click(icon);
    require(actions == QStringList{QStringLiteral("screenshot")},
            "icon click starts standard screenshot");
    reveal(controller);
    auto* toolbar = window("floatingToolbarWindow");
    require(toolbar && toolbar->isVisible(), "hover materializes the toolbar");
    require(toolbar->geometry().center().x() < iconWindow->geometry().center().x(),
            "right-side icon expands left");
    auto* ocr = toolbar->findChild<adqt::widgets::AdButton*>();
    for (auto* candidate : toolbar->findChildren<adqt::widgets::AdButton*>())
        if (candidate->property("floatingToolbarAction").toString() ==
            QStringLiteral("text-recognition"))
            ocr = candidate;
    require(ocr && ocr->property("floatingToolbarAction").toString() ==
                       QStringLiteral("text-recognition"),
            "default main recognition action is OCR");
    auto* popover = ocr->findChild<adqt::widgets::AdPopover*>();
    require(popover && popover->triggers() == adqt::widgets::AdPopover::Trigger::Hover,
            "OCR uses the drawing toolbar hover group implementation");
    require(toolbar->findChildren<adqt::widgets::AdButton*>().size() == 5,
            "four groups and overflow use one button each without special arrow buttons");
    popover->show();
    pump();
    require(popover->contentWidget(), "recognition options materialize lazily");
    timer(controller, "floatingToolbarRetreatTimer");
    require(toolbar->isVisible() && popover->isVisible(),
            "open group keeps the floating toolbar expanded");
    auto* scrolling = popover->contentWidget()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("scrolling-screenshot"));
    require(scrolling, "scrolling screenshot is available in the group");
    scrolling->click();
    require(actions.back() == QStringLiteral("scrolling-screenshot") &&
                ocr->property("floatingToolbarAction").toString() ==
                    QStringLiteral("scrolling-screenshot"),
            "group selection becomes the repeated action");
    ocr->click();
    require(actions.back() == QStringLiteral("scrolling-screenshot"),
            "split button repeats its selection");

    reveal(controller);
    QPoint from = icon->mapToGlobal(icon->rect().center());
    mouse(icon, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    mouse(icon, QEvent::MouseMove, from - QPoint(180, 40), Qt::NoButton, Qt::LeftButton);
    require(!toolbar->isVisible(), "drag immediately hides the popup");
    timer(controller, "floatingToolbarRevealTimer");
    require(!toolbar->isVisible(), "hover timer cannot reveal while dragging");
    const auto count = actions.size();
    mouse(icon, QEvent::MouseButtonRelease, from - QPoint(180, 40), Qt::LeftButton, Qt::NoButton);
    require(actions.size() == count && !stored.placement().isEmpty(),
            "drag saves placement and never captures");

    controller.setCaptureActive(QStringLiteral("selection"), true);
    require(!iconWindow->isVisible(), "capture hides before acquisition returns");
    controller.setCaptureActive(QStringLiteral("direct"), true);
    controller.setCaptureActive(QStringLiteral("selection"), false);
    pump();
    require(!iconWindow->isVisible(), "one finished producer cannot restore another capture");
    controller.setCaptureActive(QStringLiteral("direct"), false);
    controller.setCaptureActive(QStringLiteral("recording"), true);
    pump();
    require(!iconWindow->isVisible(), "capture-to-recording transition remains hidden");
    controller.setCaptureActive(QStringLiteral("recording"), false);
    pump();
    require(iconWindow->isVisible(), "last completion restores icon");
    fullscreen = true;
    timer(controller, "floatingToolbarFullscreenTimer");
    require(!iconWindow->isVisible(), "fullscreen on target monitor hides icon");
    fullscreen = false;
    timer(controller, "floatingToolbarFullscreenTimer");
    require(iconWindow->isVisible(), "leaving fullscreen restores icon");
    stored.setHideDuringCapture(false);
    pump();
    controller.setCaptureActive(QStringLiteral("selection"), true);
    require(iconWindow->isVisible(), "unchecked capture hiding is honored");
    controller.setCaptureActive(QStringLiteral("selection"), false);
    stored.setHideDuringCapture(true);
    pump();

    QMimeData mime;
    QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    mime.setImageData(image);
    int drops = 0;
    const auto dropConnection = QObject::connect(
        &controller, &FloatingToolbarController::imagesDropped, &controller,
        [&](const ScreenshotClipboardContentSnapshot& snapshot, const QStringList& paths) {
            require(snapshot.detachedImage.size() == QSize(32, 24) && paths.isEmpty(),
                    "drop snapshots image without clipboard substitution");
            ++drops;
        });
    QApplication::clipboard()->setText(QStringLiteral("preserve clipboard"));
    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(iconWindow, &enter);
    require(enter.isAccepted(), "image drag is accepted");
    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(iconWindow, &drop);
    require(drops == 1 && QApplication::clipboard()->text() == QStringLiteral("preserve clipboard"),
            "drop dispatch preserves clipboard");
    QObject::disconnect(dropConnection);
    QStringList droppedPaths;
    QObject::connect(&controller, &FloatingToolbarController::imagesDropped, &controller,
                     [&](const ScreenshotClipboardContentSnapshot&, const QStringList& paths) {
                         droppedPaths = paths;
                     });
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(QStringLiteral("/one.png")),
                   QUrl::fromLocalFile(QStringLiteral("/two.jpg")),
                   QUrl::fromLocalFile(QStringLiteral("/ignore.txt")),
                   QUrl(QStringLiteral("https://example.com/remote.png"))});
    QDragEnterEvent filesEnter(QPoint(20, 20), Qt::CopyAction, &files, Qt::LeftButton,
                               Qt::NoModifier);
    QApplication::sendEvent(iconWindow, &filesEnter);
    QDropEvent filesDrop(QPointF(20, 20), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(iconWindow, &filesDrop);
    require(
        filesDrop.isAccepted() && droppedPaths.size() == 2,
        "multiple local image files dispatch together while remote and nonimage files are ignored");

    FloatingTranslator translator;
    QApplication::installTranslator(&translator);
    pump();
    require(icon->accessibleName() == QStringLiteral("Translated screenshot"),
            "visible icon retranslates immediately");
    QApplication::removeTranslator(&translator);
    pump();

    stored.setPlacement({{QStringLiteral("screen"), QStringLiteral("removed-monitor")},
                         {QStringLiteral("x"), 0.0},
                         {QStringLiteral("y"), 0.5}});
    pump();
    reveal(controller);
    require(toolbar->geometry().center().x() > iconWindow->geometry().center().x(),
            "missing monitor falls back to primary and left-side icon expands right");
    const QPoint preservedAnchor = icon->mapToGlobal(QPoint());

    auto* menu = context(icon);
    require(menu && menu->actions().size() == 6 && menu->actions()[2]->isSeparator(),
            "context menu has exact order and separator");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(iconWindow, &leave);
    timer(controller, "floatingToolbarRetreatTimer");
    require(toolbar->isVisible() && menu->isVisible(),
            "an owned menu keeps the hover toolbar open across the interaction gap");
    int customizations = 0;
    QObject::connect(&controller, &FloatingToolbarController::customizeRequested, &controller,
                     [&] { ++customizations; });
    menu->actions()[1]->trigger();
    require(customizations == 1, "customization navigation is dispatched");
    menu->actions()[0]->trigger();
    menu->close();
    pump();
    toolbar = window("floatingToolbarWindow");
    require(stored.toolbarMode() && toolbar->isVisible() && !iconWindow->isVisible(),
            "toolbar mode has one visible desktop window");
    auto* toolbarIcon =
        toolbar->findChild<QAbstractButton*>(QStringLiteral("floatingToolbarSnowflake"));
    require(toolbarIcon && toolbarIcon->isVisible() && toolbarIcon->width() > 0,
            "toolbar mode contains reachable snowflake");
    stored.setToolbarMode(false);
    pump();
    reveal(controller);
    require(icon->mapToGlobal(QPoint()) == preservedAnchor,
            "switching modes preserves the icon anchor even when toolbar bounds clamp");
    stored.setToolbarMode(true);
    pump();
    click(toolbarIcon);
    menu = qobject_cast<QMenu*>(window("floatingToolbarContextMenu"));
    require(menu && menu->isVisible() &&
                menu->actions()[0]->text() == QStringLiteral("Toolbar mode"),
            "toolbar icon opens menu with current mode label");
    menu->close();
    if (!visualDirectory.isEmpty()) {
        QDir().mkpath(visualDirectory);
        for (const auto mode : {styles::ThemeMode::Light, styles::ThemeMode::Dark}) {
            styles::ThemeManager::instance().setThemeMode(mode);
            pump();
            const auto name =
                mode == styles::ThemeMode::Light ? QStringLiteral("light") : QStringLiteral("dark");
            toolbar->grab().save(visualDirectory + QStringLiteral("/toolbar-") + name +
                                 QStringLiteral(".png"));
            stored.setToolbarMode(false);
            pump();
            reveal(controller);
            iconWindow->grab().save(visualDirectory + QStringLiteral("/icon-") + name +
                                    QStringLiteral(".png"));
            toolbar->grab().save(visualDirectory + QStringLiteral("/hover-") + name +
                                 QStringLiteral(".png"));
            stored.setToolbarMode(true);
            pump();
        }
        storage::ApplicationStorage::instance().configuration().setValue(
            QStringLiteral("screenshot_ui/toolbar_size"), QStringLiteral("small"));
        pump();
        toolbar->grab().save(visualDirectory + QStringLiteral("/toolbar-small.png"));
#ifdef SNOW_FLOATING_TEST_TRANSLATIONS_DIR
        for (const auto& language :
             {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
            QTranslator catalog;
            require(catalog.load(QDir(QStringLiteral(SNOW_FLOATING_TEST_TRANSLATIONS_DIR))
                                     .filePath(QStringLiteral("snow_shot_%1.qm").arg(language))),
                    "compiled toolbar language catalog loads");
            QApplication::installTranslator(&catalog);
            pump();
            auto* translatedMenu = context(toolbarIcon);
            translatedMenu->grab().save(visualDirectory + QStringLiteral("/menu-") + language +
                                        QStringLiteral(".png"));
            translatedMenu->close();
            QApplication::removeTranslator(&catalog);
            pump();
        }
#endif
    }
    menu = context(toolbarIcon);
    menu->actions().last()->trigger();
    menu->close();
    pump();
    require(!stored.enabled() && !window("floatingToolbarIconWindow") &&
                !window("floatingToolbarWindow"),
            "Close persists off and releases desktop surfaces");
    auto* poll = controller.findChild<QTimer*>(QStringLiteral("floatingToolbarFullscreenTimer"));
    require(poll && !poll->isActive(), "disabled fullscreen polling stops");
    const auto placement = stored.placement();
    stored.setEnabled(true);
    pump();
    require(stored.toolbarMode() && stored.placement() == placement &&
                window("floatingToolbarWindow")->isVisible(),
            "re-enable restores mode and placement");
    stored.setToolbarMode(false);
    pump();
    reveal(controller);
    toolbar = window("floatingToolbarWindow");
    bool resetToDefault = false;
    for (auto* button : toolbar->findChildren<adqt::widgets::AdButton*>())
        resetToDefault |= button->property("floatingToolbarAction").toString() ==
                          QStringLiteral("scrolling-screenshot");
    require(resetToDefault, "disabling and reenabling in the same session preserves split choices");
    stored.setEnabled(false);
    pump();
}
void benchmark() {
    const storage::FloatingToolbarSettings stored;
    stored.setToolbarMode(false);
    stored.setHideInFullscreen(false);
    FloatingToolbarController controller(nullptr, [](QScreen*) { return false; });
    QElapsedTimer clock;
    clock.start();
    stored.setEnabled(true);
    pump();
    reveal(controller);
    const double first = clock.nsecsElapsed() / 1e6;
    QVector<double> samples;
    for (int iteration = 0; iteration < 200; ++iteration) {
        controller.setCaptureActive(QStringLiteral("benchmark"), true);
        clock.restart();
        controller.setCaptureActive(QStringLiteral("benchmark"), false);
        pump();
        reveal(controller);
        samples.append(clock.nsecsElapsed() / 1e6);
    }
    std::sort(samples.begin(), samples.end());
    const auto baseline = QApplication::allWidgets().size();
    auto* icon = window("floatingToolbarIconWindow")
                     ->findChild<QAbstractButton*>(QStringLiteral("floatingToolbarSnowflake"));
    const QPoint start = icon->mapToGlobal(icon->rect().center());
    mouse(icon, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    clock.restart();
    for (int iteration = 0; iteration < 200; ++iteration)
        mouse(icon, QEvent::MouseMove, start - QPoint(40 + iteration % 80, 40), Qt::NoButton,
              Qt::LeftButton);
    const double drag = clock.nsecsElapsed() / 1e6 / 200;
    mouse(icon, QEvent::MouseButtonRelease, start - QPoint(80, 40), Qt::LeftButton, Qt::NoButton);
    pump();
    clock.restart();
    for (int iteration = 0; iteration < 100; ++iteration) {
        stored.setToolbarMode(iteration % 2 == 0);
        pump();
    }
    const double modeChange = clock.nsecsElapsed() / 1e6 / 100;
    require(QApplication::allWidgets().size() <= baseline + 1,
            "repeated mode changes do not accumulate widgets");
    class PaintCounter final : public QObject {
      public:
        int count = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Paint)
                ++count;
            return false;
        }
    } paints;
    const auto waitForEvents = [](int milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };
    waitForEvents(500);
    for (auto* widget : QApplication::allWidgets())
        widget->installEventFilter(&paints);
    waitForEvents(600);
    require(paints.count == 0, "stationary toolbar performs no continuous repainting");
    std::cout << "{\"first_reveal_ms\":" << first << ",\"warm_median_ms\":" << samples[100]
              << ",\"warm_p95_ms\":" << samples[190] << ",\"drag_move_ms\":" << drag
              << ",\"mode_change_ms\":" << modeChange << ",\"idle_paints\":" << paints.count
              << ",\"widgets\":" << baseline << "}\n";
    stored.setEnabled(false);
    pump();
    require(!window("floatingToolbarIconWindow") && !window("floatingToolbarWindow") &&
                !controller.findChild<QTimer*>(QStringLiteral("floatingToolbarFullscreenTimer"))
                     ->isActive(),
            "disabled benchmark state retains no native surfaces or polling");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir directory;
    auto& storage = storage::ApplicationStorage::instance();
    require(
        storage.initialize({directory.filePath(QStringLiteral("bin")), directory.path()}).success,
        "temporary storage initializes");
    styles::ThemeManager::instance().initialize(app);
    if (app.arguments().contains(QStringLiteral("--benchmark"))) {
        benchmark();
    } else if (app.arguments().contains(QStringLiteral("--native-probe"))) {
        nativeProbe();
    } else {
        verifySettings();
        verifyPlacement();
        verifyDropIdentity();
        const int visual = app.arguments().indexOf(QStringLiteral("--visual-output"));
        verifyWindows(visual >= 0 ? app.arguments().value(visual + 1) : QString());
        verifyCustomizedActions();
    }
    storage.shutdown();
    return 0;
}
