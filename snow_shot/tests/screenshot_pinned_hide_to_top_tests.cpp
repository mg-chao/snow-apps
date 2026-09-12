#include "../src/presentation/pinned/screenshotpinnedhidetotopcontroller.h"
#include "../src/presentation/pinned/screenshotpinnedpointerpresence.h"
#include "theme/theme_color_utils.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWidget>

#include <algorithm>
#include <initializer_list>

#include <cstdlib>
#include <stdexcept>

class ScreenshotPinnedHideToTopControllerTestAccess {
  public:
    static QTimer& timer(ScreenshotPinnedHideToTopController& controller) {
        return controller.m_pointerPresence->timer();
    }
};

namespace {
namespace geometry = screenshot_pinned_hide_to_top;
using Controller = ScreenshotPinnedHideToTopController;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Fixture {
    QWidget owner;
    QRect nativeGeometry{100, 300, 320, 240};
    int opacity = 80;
    int allocations = 0;
    int activations = 0;
    int changes = 0;
    int menus = 0;
    int doubles = 0;
    int middles = 0;
    bool rejectGeometry = false;
    std::optional<QPoint> cursor;
    Controller controller;

    Fixture()
        : controller(&owner, Controller::Hooks{
                                 [this] { return nativeGeometry; },
                                 [this] { return nativeGeometry.adjusted(-4, -4, 4, 4); },
                                 [this](const QRect& rect) {
                                     if (rejectGeometry) {
                                         return false;
                                     }
                                     nativeGeometry = rect;
                                     return true;
                                 },
                                 [this] { return opacity; }, [this] { return allocations++ % 13; },
                                 [this] { ++activations; }, [this] { ++changes; },
                                 [this] { return cursor; },
                                 [this](const QPoint&) {
                                     ++menus;
                                     controller.exit();
                                 },
                                 [this](bool doubleClick) {
                                     if (doubleClick) {
                                         ++doubles;
                                     } else {
                                         ++middles;
                                     }
                                 }}) {
        owner.show();
    }

    static geometry::Screen screen() {
        return {nullptr, QRect(0, 40, 1000, 700), 1.0};
    }
    QTimer& timer() {
        return ScreenshotPinnedHideToTopControllerTestAccess::timer(controller);
    }
    void expire() {
        timer().stop();
        require(QMetaObject::invokeMethod(&timer(), "timeout"), "deliver handle presence timeout");
    }
    void pointer(const QPoint& position) {
        controller.updatePointer(position);
        if (timer().isActive()) {
            expire();
        }
    }
    void enter() {
        require(controller.enter(screen()), "entry must succeed");
        controller.animation().pause();
    }
    void finish() {
        controller.animation().resume();
        controller.animation().setCurrentTime(250);
    }
};

void placement() {
    const QRect work(-1200, -780, 1200, 740);
    require(geometry::placeHandle(work, -900, 1.0, {}) == QRect(-900, -780, 30, 6),
            "placement must retain negative monitor coordinates and work-area top");
    require(geometry::placeHandle(work, -900, 1.0, {QRect(-900, -780, 30, 6)}).left() == -866,
            "collision must shift right with a four-pixel gap");
    require(geometry::placeHandle(work, -30, 1.0, {QRect(-30, -780, 30, 6)}).left() == -64,
            "right-edge collision must search left");
    require(geometry::placeHandle(QRect(0, 0, 50, 20), 40, 1.0, {QRect(0, 0, 30, 6)}) ==
                QRect(20, 0, 30, 6),
            "containment must win when every slot overlaps");
    require(geometry::placeHandle(QRect(0, 0, 10, 3), -200, 2.0, {}) == QRect(0, 0, 10, 3),
            "a tiny work area must still contain the complete handle");
    require(geometry::placeHandle(work, 5000, 1.25, {}).size() == QSize(38, 8),
            "fractional DPI must scale handle dimensions");
    const QRect h = geometry::placeHandle(work, -1200, 1.5, {});
    require(geometry::hitGeometry(h, work, 1.5) == QRect(-1200, -780, 48, 12),
            "hover margin must scale and be clipped at monitor edges");
    require(geometry::placeHandle({}, 0, 1, {}).isEmpty(),
            "invalid monitors cannot reserve handles");
}

void animationAndHover() {
    Fixture f;
    const QRect original = f.nativeGeometry;
    f.enter();
    require(f.controller.animation().duration() == 250 &&
                f.controller.animation().easingCurve().type() == QEasingCurve::InOutCubic,
            "entry must use the specified synchronized animation");
    require(!f.controller.handleWidget()->isVisible(), "handle must stay hidden during entry");
    f.pointer(f.controller.handleGeometry().center());
    require(f.activations == 0, "entry ignores handle hover");
    f.controller.animation().setCurrentTime(125);
    require(f.nativeGeometry.top() == 170 && qAbs(f.owner.windowOpacity() - 0.4) < 0.01,
            "halfway entry must synchronize position and configured opacity");
    require(f.controller.shownGeometry() == QRect(100, 46, 320, 240),
            "entry snapshots must describe the eventual shown rectangle");
    f.opacity = 60;
    f.controller.refreshOpacity();
    require(qAbs(f.owner.windowOpacity() - 0.3) < 0.01,
            "configured opacity changes must retain animation progress");
    f.controller.exit(true);
    require(
        !f.controller.active() && f.nativeGeometry == original && f.owner.isVisible() &&
            qAbs(f.owner.windowOpacity() - 0.6) < 0.01,
        "toggle cancellation must restore the original position and current configured opacity");
    f.controller.animation().setCurrentTime(250);
    require(f.owner.isVisible() && !f.controller.handleWidget()->isVisible(),
            "stale animation updates must not resurrect a canceled handle");
    f.enter();
    f.finish();
    require(f.controller.state() == Controller::State::Hidden && !f.owner.isVisible() &&
                f.controller.handleWidget()->isVisible() && !f.owner.hasFocus(),
            "finished entry must leave only the unfocused handle");
    f.pointer(f.controller.handleGeometry().center());
    require(f.controller.state() == Controller::State::Revealed && f.activations == 1 &&
                f.owner.isVisible() && f.nativeGeometry.topLeft() == QPoint(100, 46),
            "hover must reveal and activate directly below the handle");
    f.pointer(QPoint(150, 100));
    require(f.owner.isVisible(), "crossing into the window must not hide it");
    f.pointer(QPoint(97, 100));
    require(f.owner.isVisible(), "non-client border belongs to the hover union");
    f.pointer(QPoint(800, 600));
    require(!f.owner.isVisible() && f.controller.state() == Controller::State::Hidden,
            "leaving both regions must hide after the delay");
    f.controller.exit();
    require(f.owner.isVisible() && f.nativeGeometry.topLeft() == QPoint(100, 46) &&
                f.allocations == 1,
            "hidden exit must use the shown position and retain its original accent");
}

void debouncedHandlePresence() {
    Fixture f;
    f.enter();
    f.finish();
    const QPoint handle = f.controller.handleGeometry().center();
    const QPoint outside(800, 600);
    require(f.timer().interval() == 100 && f.timer().timerType() == Qt::PreciseTimer,
            "handle must share the 100 ms presence policy");
    f.controller.updatePointer(handle);
    require(!f.owner.isVisible() && f.timer().isActive(), "handle entry must wait");
    const auto timerId = f.timer().id();
    f.controller.updatePointer(handle);
    require(f.timer().id() == timerId, "handle motion must not restart the delay");
    f.controller.updatePointer(outside);
    require(!f.timer().isActive() && !f.owner.isVisible(), "brief handle entry must cancel");
    f.pointer(handle);
    f.controller.updatePointer(outside);
    require(f.owner.isVisible() && f.timer().isActive(), "handle union exit must wait");
    f.controller.updatePointer(f.nativeGeometry.center());
    require(f.owner.isVisible() && !f.timer().isActive(),
            "returning to the image must cancel pending hiding");
    f.controller.updatePointer(handle);
    require(!f.timer().isActive(), "moving from image to handle must keep the same presence");
    f.controller.updatePointer(outside);
    f.cursor = handle;
    f.expire();
    require(f.owner.isVisible(), "timeout must reject a stale exit using live cursor");
    f.cursor.reset();
    f.pointer(outside);
    require(!f.owner.isVisible(), "stable exit must hide after timeout");
    f.controller.updatePointer(handle);
    f.cursor = outside;
    f.expire();
    require(!f.owner.isVisible(), "timeout must reject a stale handle entry using live cursor");
    f.cursor.reset();
    f.controller.updatePointer(handle);
    f.controller.setSuppressed(true);
    require(!f.timer().isActive(), "suppression must cancel pending entry");
    f.controller.setSuppressed(false);
    f.controller.updatePointer(handle);
    f.controller.exit();
    require(!f.timer().isActive(), "exiting hide-to-top must cancel pending entry");
    f.enter();
    f.finish();
    f.controller.updatePointer(handle);
    f.controller.shutdown();
    require(!f.timer().isActive(), "shutdown must cancel pending entry");
}

void reservationsAndRestore() {
    Fixture first;
    Fixture second;
    first.enter();
    second.enter();
    require(second.controller.handleGeometry().left() == 134,
            "entering windows must reserve their positions before handles appear");
    second.controller.exit(true);
    require(second.controller.enter({nullptr, QRect(0, -700, 1000, 600), 1.0}),
            "another monitor must accept an independent reservation");
    require(second.controller.handleGeometry().left() == 100,
            "handles on other monitors must not participate in avoidance");
    second.controller.exit(true);
    second.controller.setAccentIndex(9);
    require(second.controller.prepareRestore(Fixture::screen(), QRect(9999, -5, 30, 6)),
            "saved handles must restore without entry animation");
    require(second.controller.handleGeometry() == QRect(970, 40, 30, 6),
            "restore must clamp a saved handle into the work area");
    require(second.owner.testAttribute(Qt::WA_ShowWithoutActivating) &&
                second.owner.windowOpacity() == 0.0 &&
                !second.controller.handleWidget()->isVisible(),
            "restore initialization must not expose or activate the shell");
    second.controller.finishRestore();
    require(!second.owner.isVisible() && second.controller.handleWidget()->isVisible(),
            "restored content readiness must publish only the handle");
    second.controller.setSuppressed(true);
    require(second.controller.active() && !second.controller.handleWidget()->isVisible(),
            "bulk suppression must preserve the persisted mode");
    second.controller.setSuppressed(false);
    require(second.controller.handleWidget()->isVisible() && second.controller.accentIndex() == 9,
            "show-all must restore the same assigned accent");
    second.controller.reconcileScreen({nullptr, QRect(0, 60, 600, 500), 1.5});
    require(second.controller.handleGeometry() == QRect(555, 60, 45, 9),
            "work-area and DPI changes must regenerate and clamp the handle");
    second.controller.recoverToScreen({nullptr, QRect(-1000, 0, 1000, 700), 1.0});
    require(!second.controller.active() && second.owner.isVisible() &&
                QRect(-1000, 0, 1000, 700).contains(second.nativeGeometry),
            "monitor loss must recover a reachable visible window");
    first.controller.shutdown();
    require(first.controller.active() && !first.controller.handleWidget()->isVisible(),
            "teardown must retain mode state for close snapshots");
}

void themeAndActions() {
    auto& manager = adqt::theme::ThemeManager::instance();
    const auto original = manager.config();
    Fixture f;
    for (const auto scheme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        manager.setColorScheme(scheme);
        const auto resolved = manager.resolve(&f.owner);
        const auto& a = resolved.theme.accents;
        const QColor colors[] = {a.blue,     a.purple, a.cyan,   a.green,  a.magenta,
                                 a.pink,     a.red,    a.orange, a.yellow, a.volcano,
                                 a.geekblue, a.gold,   a.lime};
        for (int i = 0; i < 13; ++i) {
            f.controller.setAccentIndex(i);
            const auto palette = adqt::theme::generateMappedPalette(
                colors[i], scheme == adqt::theme::ThemeScheme::Dark,
                resolved.values.colorBgContainer);
            require(f.controller.handleColor() == palette.at(4),
                    "every handle accent must use shade five of the current theme");
        }
    }
    manager.setConfig(original);
    f.enter();
    f.finish();
    QWidget* handle = f.controller.handleWidget();
    QMouseEvent middle(QEvent::MouseButtonPress, QPointF(3, 3), QPointF(103, 43), Qt::MiddleButton,
                       Qt::MiddleButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &middle);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(3, 3), QPointF(103, 43),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &doubleClick);
    require(f.middles == 1 && f.doubles == 1, "handle mouse actions must route to their owner");
    QContextMenuEvent context(QContextMenuEvent::Mouse, QPoint(3, 3), QPoint(103, 43));
    QCoreApplication::sendEvent(handle, &context);
    require(f.menus == 1 && !f.controller.active() && f.owner.isVisible(),
            "handle context menu must immediately exit the mode");
    f.enter();
    f.finish();
    f.rejectGeometry = true;
    f.pointer(f.controller.handleGeometry().center());
    require(!f.controller.active() && f.owner.isVisible(),
            "failed reveal geometry must recover a visible normal window");
}

int colorDistance(const QColor& lhs, const QColor& rhs) {
    return std::max({std::abs(lhs.red() - rhs.red()), std::abs(lhs.green() - rhs.green()),
                     std::abs(lhs.blue() - rhs.blue())});
}

QColor compositeLayers(std::initializer_list<QColor> layers) {
    QImage surface(1, 1, QImage::Format_ARGB32_Premultiplied);
    surface.fill(Qt::transparent);
    QPainter painter(&surface);
    for (const QColor& layer : layers) {
        painter.fillRect(surface.rect(), layer);
    }
    painter.end();
    return surface.convertToFormat(QImage::Format_ARGB32).pixelColor(0, 0);
}

void handleBorderPainting() {
    constexpr int borderWidth = 2;
    Fixture f;
    f.enter();
    f.finish();
    const geometry::Screen screen = Fixture::screen();
    const QRect hit =
        geometry::hitGeometry(f.controller.handleGeometry(), screen.workArea, screen.dpi);
    const QRect visual = f.controller.handleGeometry().translated(-hit.topLeft());
    const QImage image =
        f.controller.handleWidget()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    require(image.rect().contains(visual),
            "the grabbed handle must cover its visual rectangle in physical pixels");
    const QColor underlay(0, 0, 0, 1);
    const QColor fill = f.controller.handleColor();
    const QColor border = adqt::theme::ThemeManager::instance().resolveTheme(&f.owner).colorText;
    require(colorDistance(border, fill) > 16,
            "the theme foreground must differ from the handle fill so the border is observable");
    // SourceOver composition is associative, so the expected color of a pixel
    // follows from how many fill and border layers cover it, regardless of the
    // order the paint event fills them.
    const QColor expectedFill = compositeLayers({underlay, fill});
    const QColor expectedBorder = compositeLayers({expectedFill, border});
    const QColor expectedCorner = compositeLayers({expectedBorder, border});
    const auto matches = [&](int x, int y, const QColor& expected) {
        return colorDistance(image.pixelColor(x, y), expected) <= 2;
    };
    for (int y = visual.top(); y <= visual.bottom(); ++y) {
        const bool inBottomBorder = y > visual.bottom() - borderWidth;
        for (const int x : {visual.left(), visual.left() + 1, visual.right() - 1, visual.right()}) {
            require(matches(x, y, inBottomBorder ? expectedCorner : expectedBorder),
                    "the left and right borders must span the full height in the theme "
                    "foreground");
        }
        const QColor besideSide = inBottomBorder ? expectedBorder : expectedFill;
        require(matches(visual.left() + borderWidth, y, besideSide) &&
                    matches(visual.right() - borderWidth, y, besideSide),
                "the left and right borders must be exactly two physical pixels wide");
    }
    for (int x = visual.left(); x <= visual.right(); ++x) {
        const bool overSide = x < visual.left() + borderWidth || x > visual.right() - borderWidth;
        require(matches(x, visual.bottom(), overSide ? expectedCorner : expectedBorder),
                "the bottom border must span the full width in the theme foreground");
    }
    const int middleX = visual.center().x();
    for (const int y : {visual.top(), visual.top() + 1}) {
        require(matches(middleX, y, expectedFill), "the handle must not draw a top border");
    }
    require(matches(middleX, visual.center().y(), expectedFill),
            "the handle interior must keep its accent fill outside the border");
}
} // namespace

void runPinnedHideToTopControllerTests() {
    placement();
    animationAndHover();
    debouncedHandlePresence();
    reservationsAndRestore();
    themeAndActions();
    handleBorderPainting();
}
