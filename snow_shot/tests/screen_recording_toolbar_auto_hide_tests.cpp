// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../src/presentation/recording/recordingaudiogainpopover.h"
#include "../src/presentation/recording/screenrecordingtoolbarautohide.h"
#include "widgets/popover.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QAbstractButton>
#include <QEnterEvent>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QImage>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScopeGuard>
#include <QScreen>
#include <QTimer>
#include <QTranslator>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

class ScreenRecordingToolbarWindowTestAccess {
  public:
    static void cursor(ScreenRecordingToolbarWindow& toolbar, const QPoint& position) {
        toolbar.m_autoHideCursorPositionOverride = position;
        toolbar.refreshAutoHideControl();
    }
};

#ifdef Q_OS_WIN
class ScreenshotFloatingToolPaletteWindowTestAccess {
  public:
    static void setCurrentDpr(ScreenshotFloatingToolPaletteWindow& toolbar, qreal dpr) {
        toolbar.m_testWindowDevicePixelRatio = dpr;
        QEvent changed(QEvent::DevicePixelRatioChange);
        QApplication::sendEvent(&toolbar, &changed);
    }

    static qreal referenceDpr(const ScreenshotFloatingToolPaletteWindow& toolbar) {
        return toolbar.m_referenceDevicePixelRatio;
    }

    static bool hasPlacementScreen(const ScreenshotFloatingToolPaletteWindow& toolbar) {
        return !toolbar.m_placementScreen.isNull();
    }
};
#endif

void runScreenRecordingToolbarAutoHideTests();
void renderScreenRecordingToolbarAutoHidePreviews();
void setRecordingToolbarAutoHideTestCursor(ScreenRecordingToolbarWindow& toolbar,
                                           const QPoint& position);

void setRecordingToolbarAutoHideTestCursor(ScreenRecordingToolbarWindow& toolbar,
                                           const QPoint& position) {
    ScreenRecordingToolbarWindowTestAccess::cursor(toolbar, position);
}

namespace {
namespace storage = snow_shot::storage;
namespace layout = snow_shot::presentation::toolbar_layout;
namespace auto_hide = snow_shot::presentation::recording::toolbar_auto_hide;
using Side = auto_hide::Side;
using Session = ScreenshotToolPalette::RecordingSessionStatus;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void pump() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

QTimer* timer(ScreenRecordingToolbarWindow& toolbar, const char* name) {
    auto* result = toolbar.findChild<QTimer*>(QString::fromLatin1(name));
    require(result != nullptr, "recording toolbar must expose its deterministic auto-hide timer");
    return result;
}

void expire(QTimer* value) {
    require(value->isActive(), "the expected auto-hide transition must be scheduled");
    value->stop();
    require(QMetaObject::invokeMethod(value, "timeout", Qt::DirectConnection),
            "the auto-hide timer must support deterministic expiration");
    pump();
}

storage::ScreenshotToolbarLayout compactLayout(storage::ScreenshotToolbarLayoutKind kind,
                                               const QVector<QStringList>& positions) {
    auto hidden = layout::defaultOrder(kind);
    for (const auto& position : positions)
        for (const auto& id : position)
            hidden.removeAll(id);
    return {positions, hidden};
}

void geometryCases() {
    const QRect monitor(-1920, -400, 1920, 1080);
    const QRect leftRow(monitor.left(), 100, 340, 40);
    const QRect rightRow(monitor.right() + 1 - 340, 100, 340, 40);
    require(auto_hide::sideForRow(leftRow, monitor) == Side::Left &&
                auto_hide::sideForRow(rightRow, monitor) == Side::Right,
            "auto-hide must dock to the corresponding physical monitor edge at negative origins");
    require(auto_hide::sideForRow(leftRow.translated(auto_hide::edgeThreshold, 0), monitor) ==
                    Side::Left &&
                auto_hide::sideForRow(leftRow.translated(auto_hide::edgeThreshold + 1, 0),
                                      monitor) == Side::None &&
                auto_hide::sideForRow(rightRow.translated(-auto_hide::edgeThreshold, 0), monitor) ==
                    Side::Right &&
                auto_hide::sideForRow(rightRow.translated(-auto_hide::edgeThreshold - 1, 0),
                                      monitor) == Side::None,
            "docking must honor the inclusive horizontal edge threshold on both sides");
    for (const int overflow : {auto_hide::edgeThreshold + 1, 100}) {
        require(auto_hide::sideForRow(leftRow.translated(-overflow, 0), monitor) == Side::Left &&
                    auto_hide::sideForRow(rightRow.translated(overflow, 0), monitor) == Side::Right,
                "partially offscreen toolbar rows must dock beyond the inward edge threshold");
    }
    require(
        auto_hide::sideForRow(QRect(-1800, monitor.top(), 340, 40), monitor) == Side::None &&
            auto_hide::sideForRow(QRect(-1800, monitor.bottom() - 39, 340, 40), monitor) ==
                Side::None &&
            auto_hide::sideForRow(leftRow.translated(0, monitor.height() * 2), monitor) ==
                Side::None &&
            auto_hide::sideForRow(leftRow.translated(-monitor.width(), 0), monitor) == Side::None &&
            auto_hide::sideForRow(rightRow.translated(monitor.width(), 0), monitor) == Side::None &&
            auto_hide::sideForRow({}, monitor) == Side::None &&
            auto_hide::sideForRow(leftRow, {}) == Side::None,
        "top, bottom, disjoint and empty geometries must not become horizontal docking edges");
    const QRect spanning(monitor.left(), 100, monitor.width(), 40);
    require(auto_hide::sideForRow(spanning, monitor) == Side::Right &&
                auto_hide::sideForRow(spanning, monitor, Side::Left) == Side::Left &&
                auto_hide::sideForRow(spanning, monitor, Side::Right) == Side::Right,
            "equal-distance docking must retain the previous side with a deterministic default");
    const QRect oversizedLeft(monitor.left(), 100, monitor.width() + 200, 40);
    const QRect oversizedRight(monitor.left() - 200, 100, monitor.width() + 200, 40);
    const QRect oversizedBoth(monitor.left() - 100, 100, monitor.width() + 200, 40);
    require(
        auto_hide::sideForRow(oversizedLeft, monitor) == Side::Right &&
            auto_hide::sideForRow(oversizedRight, monitor) == Side::Left &&
            auto_hide::sideForRow(oversizedBoth, monitor) == Side::Right &&
            auto_hide::sideForRow(oversizedBoth, monitor, Side::Left) == Side::Left &&
            auto_hide::sideForRow(oversizedBoth, monitor, Side::Right) == Side::Right &&
            monitor.contains(auto_hide::tabGeometry(oversizedLeft, monitor, Side::Right, 1)) &&
            monitor.contains(auto_hide::tabGeometry(oversizedRight, monitor, Side::Left, 1)),
        "oversized rows must prefer their overflowing edge and retain the previous side on ties");
    const QRect moreLeftOverflow(monitor.left() - 200, 100, monitor.width() + 300, 40);
    const QRect moreRightOverflow(monitor.left() - 100, 100, monitor.width() + 300, 40);
    const QRect leftOverflowRightInset(monitor.left() - 100, 100,
                                       monitor.width() + 100 - auto_hide::edgeThreshold, 40);
    const QRect rightOverflowLeftInset(monitor.left() + auto_hide::edgeThreshold, 100,
                                       monitor.width() + 100 - auto_hide::edgeThreshold, 40);
    require(auto_hide::sideForRow(moreLeftOverflow, monitor, Side::Right) == Side::Left &&
                auto_hide::sideForRow(moreRightOverflow, monitor, Side::Left) == Side::Right &&
                auto_hide::sideForRow(leftOverflowRightInset, monitor) == Side::Left &&
                auto_hide::sideForRow(rightOverflowLeftInset, monitor) == Side::Right,
            "overflow must outrank an inset opposite edge and choose the greater overflow");
    for (const qreal scale : {0.75, 1.0, 1.25, 1.5, 2.0}) {
        QRect scaledLeftRow = leftRow;
        QRect scaledRightRow = rightRow;
        scaledLeftRow.setHeight(qRound(screenshot_action_toolbar::PanelHeight * scale));
        scaledRightRow.setHeight(scaledLeftRow.height());
        const auto leftTab = auto_hide::tabGeometry(scaledLeftRow, monitor, Side::Left, scale);
        const auto rightTab = auto_hide::tabGeometry(scaledRightRow, monitor, Side::Right, scale);
        require(leftTab.left() == monitor.left() && rightTab.right() == monitor.right() &&
                    monitor.contains(leftTab) && monitor.contains(rightTab) &&
                    leftTab.size() == QSize(qRound(20 * scale), scaledLeftRow.height()) &&
                    leftTab.top() == scaledLeftRow.top() &&
                    rightTab.top() == scaledRightRow.top() && leftTab.size() == rightTab.size(),
                "edge controls must match toolbar row sizing and alignment at either monitor edge");
        for (const int y : {monitor.top(), monitor.bottom()}) {
            const QRect row(leftRow.x(), y, leftRow.width(), leftRow.height());
            require(monitor.contains(auto_hide::tabGeometry(row, monitor, Side::Left, scale)),
                    "edge controls must clamp to the monitor at the top and bottom corners");
        }
    }
    require(auto_hide::tabGeometry(leftRow, monitor, Side::None, 1).isEmpty() &&
                auto_hide::tabGeometry({}, monitor, Side::Left, 1).isEmpty() &&
                auto_hide::tabGeometry(leftRow, {}, Side::Left, 1).isEmpty() &&
                auto_hide::tabGeometry(leftRow, monitor, Side::Left, 0).size() == QSize(20, 40) &&
                auto_hide::tabGeometry(leftRow, monitor, Side::Left,
                                       std::numeric_limits<qreal>::quiet_NaN())
                        .size() == QSize(20, 40),
            "invalid docking and scale inputs must not produce invalid edge-control geometry");
    const QRect tinyMonitor(0, 0, 8, 12);
    require(auto_hide::tabGeometry(QRect(0, 0, 8, 40), tinyMonitor, Side::Right, 2) == tinyMonitor,
            "edge controls must fit even monitors smaller than their preferred dimensions");
    const QRect tallRow(monitor.left(), 100, 340, 64);
    require(auto_hide::tabGeometry(tallRow, monitor, Side::Left, 1).height() == tallRow.height(),
            "edge controls must follow the actual row height when toolbar content is taller");
    const QRect insetLeft = leftRow.translated(auto_hide::edgeThreshold, 0);
    const QRect narrowLeftTab(monitor.left(), 99, 8, 44);
    const QRect insetRight = rightRow.translated(-auto_hide::edgeThreshold, 0);
    const QRect narrowRightTab(monitor.right() - 7, 99, 8, 44);
    require(auto_hide::transferRegion(insetLeft, narrowLeftTab, Side::Left)
                    .contains(QPoint(monitor.left() + 12, 110)) &&
                auto_hide::transferRegion(insetRight, narrowRightTab, Side::Right)
                    .contains(QPoint(monitor.right() - 12, 110)) &&
                auto_hide::transferRegion(insetLeft, narrowLeftTab, Side::None).isEmpty(),
            "hover transfer corridors must bridge the tab and toolbar without a dead strip");
}

struct ToolbarFixture {
    ScreenRecordingToolbarWindow toolbar;
    QScreen* screen = QGuiApplication::primaryScreen();
    QRect bounds;
    QRect recordingRegion;

    explicit ToolbarFixture(Session status = Session::recording()) {
        require(screen != nullptr, "toolbar auto-hide requires a screen");
        bounds = screen->geometry();
        const QRect logicalRegion(bounds.left() + 100, bounds.top() + 80, 320, 240);
#ifdef Q_OS_MACOS
        recordingRegion = logicalRegion;
#else
        recordingRegion = ScreenshotGeometryMapper::nativeRectForLogicalRect(
            logicalRegion, bounds, ScreenshotGeometryMapper::physicalRectForScreen(*screen));
#endif
        toolbar.palette()->setRecordingSession(status);
        toolbar.placeForRecordingRegion(recordingRegion);
        toolbar.showWithoutActivating();
        pump();
        require(row().width() + auto_hide::edgeThreshold * 2 < bounds.width(),
                "the compact recording fixture must fit the offscreen monitor");
        place(Side::Left);
        outside();
    }

    ~ToolbarFixture() {
        toolbar.dismissAutoHide();
        toolbar.hide();
    }

    QRect row() const {
        return toolbar.paletteHost()->mainToolbarContentRect().translated(
            toolbar.contentPosition());
    }

    QWidget* control() {
        return toolbar.autoHideControlWindow();
    }

    QTimer* retreat() {
        return timer(toolbar, "recordingToolbarRetreatTimer");
    }

    QTimer* reveal() {
        return timer(toolbar, "recordingToolbarRevealTimer");
    }

    void cursor(const QPoint& position) {
        ScreenRecordingToolbarWindowTestAccess::cursor(toolbar, position);
    }

    void outside() {
        cursor(bounds.bottomRight() + QPoint(100, 100));
    }

    void place(Side side, int overflow = 0) {
        const QRect relative = toolbar.paletteHost()->mainToolbarContentRect();
        const int x = side == Side::Left    ? bounds.left() - overflow
                      : side == Side::Right ? bounds.right() + 1 - relative.width() + overflow
                                            : bounds.center().x() - relative.width() / 2;
        const QPoint position(x, bounds.top() + bounds.height() / 3);
        toolbar.paletteHost()->dragStarted(row().center());
        toolbar.moveContentTo(position - relative.topLeft());
        toolbar.paletteHost()->dragFinished(position);
        pump();
        toolbar.refreshAutoHideControl();
    }

    void collapse() {
        outside();
        expire(retreat());
        require(!toolbar.isVisible() && control()->isVisible() && toolbar.recordingUiPresented(),
                "retreat must replace the toolbar with its control while retaining the UI session");
    }

    void hoverReveal() {
        const QPoint point = control()->geometry().center();
        cursor(point);
        QEnterEvent event(control()->mapFromGlobal(point), control()->mapFromGlobal(point), point);
        QCoreApplication::sendEvent(control(), &event);
        require(!toolbar.isVisible() && reveal()->isActive() && reveal()->interval() == 100,
                "entering the edge control must wait for the reveal dwell interval");
        expire(reveal());
        require(toolbar.isVisible() && !control()->isVisible() && toolbar.recordingUiPresented(),
                "hover dwell must restore the toolbar and retire the edge control");
    }
};

void onlyActiveRecordingAutoHides() {
    require(storage::RecordingSettings().setAutoHideToolbar(true),
            "enable recording state fixture");
    ToolbarFixture fixture(Session::idle());
    const auto requireExpanded = [&] {
        require(fixture.toolbar.isVisible() && fixture.toolbar.recordingUiPresented() &&
                    !fixture.control()->isVisible() && !fixture.retreat()->isActive() &&
                    !fixture.reveal()->isActive(),
                "only active recording may collapse the toolbar or schedule auto-hide timers");
    };
    requireExpanded();
    for (const auto status : {Session::idle(), Session::starting(), Session::countingDown(),
                              Session::paused(), Session::stopping(), Session::pausedStopping(),
                              Session::copying(), Session::pausedCopying()}) {
        fixture.toolbar.palette()->setRecordingSession(Session::recording());
        pump();
        require(fixture.retreat()->isActive(), "starting or resuming must enable auto-hide");
        fixture.toolbar.palette()->setRecordingSession(status);
        pump();
        requireExpanded();
        require(QMetaObject::invokeMethod(fixture.retreat(), "timeout", Qt::DirectConnection),
                "dispatch a stale retreat after leaving active recording");
        pump();
        requireExpanded();

        fixture.toolbar.palette()->setRecordingSession(Session::recording());
        pump();
        fixture.collapse();
        const QPoint content = fixture.toolbar.contentPosition();
        fixture.cursor(fixture.control()->geometry().center());
        require(fixture.reveal()->isActive(), "collapsed recording schedules hover reveal");
        fixture.toolbar.palette()->setRecordingSession(status);
        pump();
        requireExpanded();
        require(fixture.toolbar.contentPosition() == content,
                "leaving active recording must restore the expanded toolbar position");
        fixture.outside();
    }

    fixture.toolbar.palette()->setRecordingSession(Session::recording());
    pump();
    fixture.toolbar.setAutoHideControlShowGuard([&](QWidget*) {
        fixture.toolbar.palette()->setRecordingSession(Session::paused());
        return true;
    });
    expire(fixture.retreat());
    requireExpanded();
}

void appearanceMatchesToolbar() {
    namespace styles = snow_shot::presentation::styles;
    auto& themes = styles::ThemeManager::instance();
    const auto savedTheme = themes.themeMode();
    const storage::ScreenshotUiSettings ui;
    const QString savedSize = ui.toolbarSize();
    const auto restore = qScopeGuard([&] {
        themes.setThemeMode(savedTheme);
        require(ui.setToolbarSize(savedSize), "restore toolbar appearance fixture size");
    });
    require(storage::RecordingSettings().setAutoHideToolbar(true), "enable appearance fixtures");
    ToolbarFixture fixture;
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        themes.setThemeAppearance(appearance);
        for (const QString& size : {QStringLiteral("normal"), QStringLiteral("small")}) {
            require(ui.setToolbarSize(size), "apply toolbar size to an existing edge control");
            for (const Side side : {Side::Left, Side::Right}) {
                fixture.toolbar.showWithoutActivating();
                fixture.place(side);
                fixture.collapse();
                auto* control = qobject_cast<QAbstractButton*>(fixture.control());
                require(control != nullptr && control->height() == fixture.row().height() &&
                            control->geometry().top() == fixture.row().top(),
                        "an existing edge control must track the toolbar row's size and alignment");
                const qreal scale = fixture.toolbar.paletteHost()->physicalScale();
                require(control->width() == qRound(20 * scale),
                        "matching toolbar height must preserve the original narrow trigger width");
                for (const int dpr : {1, 2}) {
                    const auto render = [dpr](QWidget* widget) {
                        QImage image(widget->size() * dpr, QImage::Format_ARGB32_Premultiplied);
                        image.setDevicePixelRatio(dpr);
                        image.fill(Qt::transparent);
                        widget->render(&image);
                        return image;
                    };
                    for (int state = 0; state < 3; ++state) {
                        control->setAttribute(Qt::WA_UnderMouse, state != 0);
                        control->setDown(state == 2);
                        const QImage image = render(control);
                        const int edgeX = side == Side::Left ? 0 : image.width() - 1;
                        const int inwardX = side == Side::Left ? image.width() - 1 : 0;
                        for (const int y : {0, image.height() - 1}) {
                            require(image.pixelColor(inwardX, y).alpha() == 0,
                                    "only inward-facing corners must use the toolbar radius");
                            require(
                                image.pixelColor(edgeX, y).alpha() > 0,
                                "screen-facing corners must retain their original square shape");
                        }
                        const auto colors =
                            adqt::theme::ThemeManager::instance().resolveTheme(control);
                        QImage expected(1, 1, QImage::Format_ARGB32_Premultiplied);
                        expected.fill(state == 2   ? colors.colorFillSecondary
                                      : state == 1 ? colors.colorPrimaryBg
                                                   : colors.colorBgContainer);
                        require(image.pixelColor(image.width() / 2, image.height() / 4) ==
                                    expected.pixelColor(0, 0),
                                "height and corner changes must preserve the original state fills");
                    }
                }
                control->setDown(false);
                control->setAttribute(Qt::WA_UnderMouse, false);
            }
        }
    }
}

void partiallyOffscreenRows() {
    require(storage::RecordingSettings().setAutoHideToolbar(true), "enable overflow fixtures");
    for (const Side side : {Side::Left, Side::Right}) {
        for (const int overflow : {auto_hide::edgeThreshold + 1, 100}) {
            ToolbarFixture fixture;
            fixture.place(side, overflow);
            const QRect overhungRow = fixture.row();
            const QPoint overhungPosition = fixture.toolbar.contentPosition();
            require(
                (side == Side::Left && overhungRow.left() == fixture.bounds.left() - overflow) ||
                    (side == Side::Right &&
                     overhungRow.right() == fixture.bounds.right() + overflow),
                "native drag completion must retain the partially offscreen toolbar row");
            fixture.outside();
            require(fixture.toolbar.isVisible() && fixture.retreat()->isActive(),
                    "an overflowing toolbar must schedule retreat after the pointer leaves");
            fixture.collapse();
            const QRect tab = fixture.control()->geometry();
            require(fixture.bounds.contains(tab) &&
                        ((side == Side::Left && tab.left() == fixture.bounds.left()) ||
                         (side == Side::Right && tab.right() == fixture.bounds.right())),
                    "an overflowing toolbar must collapse to its corresponding visible edge");
            fixture.hoverReveal();
            require(fixture.toolbar.contentPosition() == overhungPosition &&
                        fixture.row() == overhungRow && !fixture.retreat()->isActive(),
                    "hover reveal must preserve the overhanging placement and protect tab travel");
            fixture.collapse();
            fixture.hoverReveal();
            require(fixture.toolbar.contentPosition() == overhungPosition &&
                        fixture.row() == overhungRow,
                    "repeated overflow reveal must not progressively shift the toolbar anchor");
        }
    }
}

void defaultOffAndLifecycle() {
    require(storage::RecordingSettings().setAutoHideToolbar(false), "disable auto-hide fixture");
    ToolbarFixture fixture;
    require(fixture.toolbar.isVisible() && fixture.toolbar.recordingUiPresented() &&
                !fixture.control()->isVisible() && !fixture.retreat()->isActive(),
            "default-off must preserve the visible recording toolbar beside a monitor edge");
    require(storage::RecordingSettings().setAutoHideToolbar(true), "enable auto-hide live");
    fixture.outside();
    require(fixture.retreat()->isActive() && fixture.retreat()->interval() == 200,
            "live enabling must schedule the polished retreat delay without hiding immediately");
    const QPoint content = fixture.toolbar.contentPosition();
    fixture.collapse();
    require(fixture.control()->geometry().left() == fixture.bounds.left() &&
                fixture.control()->focusPolicy() == Qt::NoFocus &&
                fixture.control()->testAttribute(Qt::WA_ShowWithoutActivating),
            "the collapsed control must sit at its edge and avoid taking keyboard focus");
    const QString originalName = fixture.control()->accessibleName();
    const QString originalTooltip = fixture.control()->toolTip();
    class EdgeControlTranslator final : public QTranslator {
      public:
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            if (QByteArray(context) == "ScreenRecordingToolbarWindow")
                return QStringLiteral("Translated: ") + QString::fromUtf8(source);
            return {};
        }
    } translator;
    require(QCoreApplication::installTranslator(&translator),
            "install recording edge-control translations");
    pump();
    require(fixture.control()->accessibleName() ==
                    QStringLiteral("Translated: Show recording toolbar") &&
                fixture.control()->toolTip() ==
                    QStringLiteral("Translated: Show recording toolbar"),
            "an existing edge control must retranslate its accessible name and tooltip");
    QCoreApplication::removeTranslator(&translator);
    pump();
    require(fixture.control()->accessibleName() == originalName &&
                fixture.control()->toolTip() == originalTooltip,
            "removing edge-control translations must restore the current application language");
    require(storage::RecordingSettings().setAutoHideToolbar(false), "disable a collapsed toolbar");
    require(fixture.toolbar.isVisible() && !fixture.control()->isVisible() &&
                fixture.toolbar.contentPosition() == content,
            "disabling auto-hide while collapsed must restore the saved expanded position");
    require(storage::RecordingSettings().setAutoHideToolbar(true), "enable the lifecycle fixture");
    fixture.collapse();
    fixture.toolbar.beginRegionInteraction();
    require(!fixture.toolbar.isVisible() && !fixture.control()->isVisible() &&
                !fixture.toolbar.recordingUiPresented() && !fixture.retreat()->isActive() &&
                !fixture.reveal()->isActive(),
            "region interaction must suppress both representations and cancel pending transitions");
    fixture.toolbar.endRegionInteraction(fixture.recordingRegion);
    fixture.place(Side::Right);
    fixture.collapse();
    require(fixture.control()->geometry().right() == fixture.bounds.right(),
            "moving to the opposite edge must relocate the reveal control");
    fixture.hoverReveal();
    fixture.place(Side::None);
    fixture.outside();
    require(fixture.toolbar.isVisible() && !fixture.control()->isVisible() &&
                !fixture.retreat()->isActive(),
            "moving away from horizontal monitor edges must leave the toolbar expanded");
    fixture.place(Side::Left);
    fixture.collapse();
    fixture.toolbar.dismissAutoHide();
    require(!fixture.control()->isVisible() && !fixture.toolbar.recordingUiPresented() &&
                !fixture.retreat()->isActive() && !fixture.reveal()->isActive(),
            "dismissing a recording must retire its edge control and pending timers");
    fixture.toolbar.showWithoutActivating();
    fixture.outside();
    fixture.collapse();
    fixture.toolbar.hide();
    require(!fixture.control()->isVisible() && !fixture.toolbar.recordingUiPresented(),
            "explicitly hiding a collapsed recording must also dismiss its edge control");
}

void hoverAndInteractionProtection() {
    require(storage::RecordingSettings().setAutoHideToolbar(true), "enable hover fixture");
    ToolbarFixture fixture;
    fixture.cursor(fixture.row().center());
    require(!fixture.retreat()->isActive(), "hovering toolbar content must prevent retreat");
    fixture.collapse();
    auto* revealButton = qobject_cast<QAbstractButton*>(fixture.control());
    require(revealButton != nullptr, "the collapsed edge control must support click-to-reveal");
    revealButton->click();
    require(fixture.toolbar.isVisible() && !fixture.control()->isVisible() &&
                !fixture.reveal()->isActive(),
            "clicking the edge control must reveal immediately without a hover dwell");
    fixture.collapse();
    fixture.cursor(fixture.control()->geometry().center());
    require(fixture.reveal()->isActive(), "hovering the hidden control must arm its reveal timer");
    fixture.outside();
    require(!fixture.reveal()->isActive() && !fixture.toolbar.isVisible(),
            "crossing the edge control briefly must cancel reveal when the pointer leaves");
    fixture.hoverReveal();
    require(!fixture.retreat()->isActive(),
            "the pointer transfer corridor must protect the freshly revealed toolbar");
    fixture.cursor(fixture.row().center());
    fixture.outside();
    fixture.toolbar.paletteHost()->dragStarted(fixture.row().center());
    fixture.toolbar.refreshAutoHideControl();
    require(fixture.toolbar.isVisible() && !fixture.retreat()->isActive(),
            "dragging the toolbar must cancel retreat until drag completion");
    fixture.toolbar.paletteHost()->dragFinished(fixture.row().center());
    fixture.outside();
    require(fixture.retreat()->isActive(), "drag completion must resume eligible auto-hide");

    {
        QLineEdit textEditor(&fixture.toolbar);
        textEditor.setGeometry(5, 5, 160, 24);
        textEditor.show();
        fixture.toolbar.activateWindow();
        textEditor.setFocus(Qt::OtherFocusReason);
        pump();
        require(QApplication::focusWidget() == &textEditor &&
                    textEditor.testAttribute(Qt::WA_InputMethodEnabled),
                "the text-protection fixture must establish an active toolbar text editor");
        fixture.outside();
        require(fixture.toolbar.isVisible() && !fixture.retreat()->isActive(),
                "focused toolbar text editing must protect the toolbar after the pointer leaves");
        textEditor.clearFocus();
        textEditor.hide();
    }
    pump();
    fixture.outside();
    require(fixture.retreat()->isActive(), "ending text input must resume eligible auto-hide");

    QObject first;
    QObject second;
    fixture.toolbar.setAutoHideBlocked(&first, true);
    fixture.toolbar.setAutoHideBlocked(&second, true);
    fixture.outside();
    require(!fixture.retreat()->isActive(), "owned modal blockers must suspend retreat");
    fixture.toolbar.setAutoHideBlocked(&first, false);
    fixture.outside();
    require(!fixture.retreat()->isActive(), "one modal must not release another modal's blocker");
    fixture.toolbar.setAutoHideBlocked(&second, false);
    fixture.outside();
    require(fixture.retreat()->isActive(), "releasing the last blocker must resume retreat");
    auto owner = std::make_unique<QObject>();
    fixture.toolbar.setAutoHideBlocked(owner.get(), true);
    owner.reset();
    fixture.outside();
    require(fixture.retreat()->isActive(),
            "destroying a blocker owner must release its protection");

    fixture.toolbar.palette()->setRecordingMicrophoneEnabled(true);
    auto* gain = fixture.toolbar.palette()->recordingAudioGainPopover(true);
    require(gain != nullptr, "compact recording fixture must retain the real microphone popover");
    gain->openAndFocus();
    pump();
    require(gain->popover()->isVisible(), "the microphone gain popover must open");
    fixture.outside();
    require(fixture.toolbar.isVisible() && !fixture.retreat()->isActive(),
            "an open owned gain popover must protect the expanded toolbar");
    gain->close();
    pump();
    fixture.outside();
    require(fixture.retreat()->isActive(),
            "closing an owned popup must restore auto-hide eligibility");
    fixture.collapse();
    QObject collapsedBlocker;
    fixture.toolbar.setAutoHideBlocked(&collapsedBlocker, true);
    require(fixture.toolbar.isVisible() && !fixture.control()->isVisible() &&
                !fixture.retreat()->isActive(),
            "starting a protected interaction while collapsed must reveal the toolbar immediately");
    fixture.toolbar.setAutoHideBlocked(&collapsedBlocker, false);
    fixture.collapse();
}

void surfaceGuardAndLifetime() {
    require(storage::RecordingSettings().setAutoHideToolbar(true), "enable show-guard fixture");
    ToolbarFixture fixture;
    bool allowed = false;
    int requests = 0;
    fixture.toolbar.setAutoHideControlShowGuard([&](QWidget* surface) {
        require(surface == fixture.control(),
                "capture guard must receive the retained edge surface");
        ++requests;
        return allowed;
    });
    fixture.outside();
    expire(fixture.retreat());
    require(requests > 0 && fixture.toolbar.isVisible() && !fixture.control()->isVisible(),
            "a failed or pending capture-exclusion guard must keep the expanded toolbar visible");
    const int pendingRequests = requests;
    QTimer* watch = nullptr;
    for (auto* candidate :
         fixture.reveal()->parent()->findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!candidate->isSingleShot() && candidate->interval() == 50)
            watch = candidate;
    }
    require(watch != nullptr && watch->isActive(),
            "a pending edge surface must retain the expanded toolbar's pointer watcher");
    for (int i = 0; i < 3; ++i) {
        require(QMetaObject::invokeMethod(watch, "timeout", Qt::DirectConnection),
                "pending auto-hide must support deterministic watcher evaluation");
        QMouseEvent move(QEvent::MouseMove, QPointF(10, 10), QPointF(10, 10),
                         QPointF(fixture.bounds.bottomRight() + QPoint(100, 100)), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&fixture.toolbar, &move);
        pump();
    }
    require(
        requests == pendingRequests && fixture.toolbar.isVisible() &&
            !fixture.control()->isVisible(),
        "watcher and queued pointer events must not repeatedly request pending capture approval");
    allowed = true;
    fixture.outside();
    require(
        !fixture.toolbar.isVisible() && fixture.control()->isVisible() &&
            fixture.toolbar.recordingUiPresented() && requests == pendingRequests + 1,
        "explicit refresh must retry pending capture approval and collapse as soon as approved");
    allowed = false;
    QEvent nativeSurfaceChanged(QEvent::WinIdChange);
    QCoreApplication::sendEvent(fixture.control(), &nativeSurfaceChanged);
    pump();
    require(fixture.toolbar.isVisible() && !fixture.control()->isVisible() &&
                fixture.toolbar.recordingUiPresented(),
            "a replaced native edge surface must retire its approval and reveal the toolbar");
    fixture.outside();
    expire(fixture.retreat());
    require(requests == pendingRequests + 2 && fixture.toolbar.isVisible() &&
                !fixture.control()->isVisible(),
            "collapsing after native surface replacement must request capture approval again");
    allowed = true;
    fixture.outside();
    require(!fixture.toolbar.isVisible() && fixture.control()->isVisible(),
            "a replaced native edge surface may reappear only after fresh capture approval");
    QPointer<QWidget> retiredControl;
    {
        auto replacement = std::make_unique<ScreenRecordingToolbarWindow>();
        replacement->showWithoutActivating();
        retiredControl = replacement->autoHideControlWindow();
        require(retiredControl != nullptr,
                "a replacement session can create its retained edge control");
    }
    pump();
    require(retiredControl.isNull(), "destroying a toolbar must destroy its retained edge surface");
}

void nativeDpiPlacementPreservesPhysicalExtent() {
#ifdef Q_OS_WIN
    require(storage::RecordingSettings().setAutoHideToolbar(false),
            "disable auto-hide for the native DPI baseline fixture");
    ScreenRecordingToolbarWindow toolbar;
    QScreen* primary = QGuiApplication::primaryScreen();
    require(primary != nullptr &&
                !ScreenshotFloatingToolPaletteWindowTestAccess::hasPlacementScreen(toolbar),
            "the native DPI fixture must start before recording-region placement");
    const qreal originalDpr = primary->devicePixelRatio();
    const qreal migratedDpr = originalDpr * 2;
    ScreenshotFloatingToolPaletteWindowTestAccess::setCurrentDpr(toolbar, migratedDpr);
    toolbar.resetPhysicalSizeInvariant();
    toolbar.prepareForDisplay();
    const QSize physicalExtent(qRound(toolbar.width() * migratedDpr),
                               qRound(toolbar.height() * migratedDpr));
    require(qFuzzyCompare(ScreenshotFloatingToolPaletteWindowTestAccess::referenceDpr(toolbar),
                          migratedDpr),
            "a fresh native toolbar must establish its physical extent at the current display DPR");
    toolbar.setPlacementContext(primary, primary->geometry(),
                                ScreenshotGeometryMapper::physicalRectForScreen(*primary), true);
    require(qFuzzyCompare(ScreenshotFloatingToolPaletteWindowTestAccess::referenceDpr(toolbar),
                          migratedDpr) &&
                QSize(qRound(toolbar.width() * migratedDpr),
                      qRound(toolbar.height() * migratedDpr)) == physicalExtent,
            "adopting a monitor after native dragging must preserve the toolbar's physical extent");
    toolbar.setPlacementContext(nullptr, {}, {});
    toolbar.setPlacementContext(primary, primary->geometry(),
                                ScreenshotGeometryMapper::physicalRectForScreen(*primary));
    require(
        qFuzzyCompare(ScreenshotFloatingToolPaletteWindowTestAccess::referenceDpr(toolbar),
                      originalDpr) &&
            QSize(qRound(toolbar.width() * migratedDpr), qRound(toolbar.height() * migratedDpr)) !=
                physicalExtent,
        "ordinary recording-region placement must still establish the selected monitor baseline");
#endif
}

void synchronousGuardCallbacksRetirePresentation() {
    require(storage::RecordingSettings().setAutoHideToolbar(true),
            "enable synchronous capture-guard callback fixtures");
    {
        ToolbarFixture fixture;
        fixture.collapse();
        fixture.toolbar.setAutoHideControlShowGuard([&](QWidget*) {
            fixture.toolbar.dismissAutoHide();
            return true;
        });
        fixture.outside();
        pump();
        require(
            !fixture.toolbar.recordingUiPresented() && !fixture.toolbar.isVisible() &&
                !fixture.control()->isVisible() && !fixture.retreat()->isActive() &&
                !fixture.reveal()->isActive(),
            "synchronous capture-guard dismissal must retire the tab and every pending transition");
    }

    for (const bool alreadyCollapsed : {false, true}) {
        auto ownedToolbar = std::make_unique<ScreenRecordingToolbarWindow>();
        ownedToolbar->palette()->setRecordingSession(Session::recording());
        const QPointer<ScreenRecordingToolbarWindow> lifetime(ownedToolbar.get());
        QScreen* screen = QGuiApplication::primaryScreen();
        require(screen != nullptr, "synchronous capture-guard lifetime fixture requires a screen");
        const QRect bounds = screen->geometry();
        ownedToolbar->setPlacementContext(screen, bounds,
                                          ScreenshotGeometryMapper::physicalRectForScreen(*screen));
        ownedToolbar->moveContentTo(
            QPoint(bounds.left(), bounds.top() + bounds.height() / 3) -
            ownedToolbar->paletteHost()->mainToolbarContentRect().topLeft());
        ownedToolbar->showWithoutActivating();
        pump();
        ScreenRecordingToolbarWindowTestAccess::cursor(*ownedToolbar,
                                                       bounds.bottomRight() + QPoint(100, 100));
        QPointer<QWidget> retiredSurface(ownedToolbar->autoHideControlWindow());
        if (alreadyCollapsed) {
            expire(timer(*ownedToolbar, "recordingToolbarRetreatTimer"));
            require(retiredSurface->isVisible() && !ownedToolbar->isVisible(),
                    "the revalidation deletion fixture must start with an approved collapsed tab");
        }
        ownedToolbar->setAutoHideControlShowGuard([&](QWidget*) {
            ownedToolbar.reset();
            return true;
        });
        if (alreadyCollapsed) {
            auto* borrowed = ownedToolbar.get();
            // Revalidation deletes the receiver inside its evaluation scope.
            // The raw borrow is used only to dispatch this single call.
            borrowed->refreshAutoHideControl();
        } else {
            auto* retreat = timer(*ownedToolbar, "recordingToolbarRetreatTimer");
            require(retreat->isActive(),
                    "the synchronous lifetime fixture must schedule an initial retreat");
            retreat->stop();
            // Expiring this timer deletes the sender through the show callback.
            // Do not read the timer or toolbar after dispatch.
            require(QMetaObject::invokeMethod(retreat, "timeout", Qt::DirectConnection),
                    "dispatch synchronous deletion from the initial collapse callback");
        }
        require(!ownedToolbar && lifetime.isNull() && retiredSurface.isNull(),
                "initial collapse and revalidation callbacks must stop after toolbar deletion");
        pump();
    }
}
} // namespace

void runScreenRecordingToolbarAutoHideTests() {
    const storage::RecordingSettings recording;
    const storage::ScreenshotToolbarSettings toolbar;
    const bool savedAutoHide = recording.autoHideToolbar();
    constexpr auto drawingKind = storage::ScreenshotToolbarLayoutKind::DrawingTools;
    constexpr auto actionKind = storage::ScreenshotToolbarLayoutKind::RecordingActionTools;
    const auto savedDrawing = toolbar.layout(drawingKind);
    const auto savedActions = toolbar.layout(actionKind);
    const auto restore = qScopeGuard([&] {
        require(recording.setAutoHideToolbar(savedAutoHide) &&
                    toolbar.setLayout(drawingKind, savedDrawing) &&
                    toolbar.setLayout(actionKind, savedActions),
                "restore recording toolbar fixture preferences");
    });
    require(toolbar.setLayout(drawingKind, compactLayout(drawingKind, {})) &&
                toolbar.setLayout(actionKind,
                                  compactLayout(actionKind, {{QStringLiteral("start-stop")},
                                                             {QStringLiteral("duration")},
                                                             {QStringLiteral("microphone")}})),
            "prepare a compact recording toolbar for deterministic offscreen edge tests");
    geometryCases();
    onlyActiveRecordingAutoHides();
    appearanceMatchesToolbar();
    partiallyOffscreenRows();
    defaultOffAndLifecycle();
    hoverAndInteractionProtection();
    surfaceGuardAndLifetime();
    nativeDpiPlacementPreservesPhysicalExtent();
    synchronousGuardCallbacksRetirePresentation();
}

void renderScreenRecordingToolbarAutoHidePreviews() {
    namespace styles = snow_shot::presentation::styles;
    const storage::RecordingSettings recording;
    const storage::ScreenshotToolbarSettings toolbar;
    const storage::ScreenshotUiSettings ui;
    auto& applicationThemes = styles::ThemeManager::instance();
    auto& componentThemes = adqt::theme::ThemeManager::instance();
    const auto savedThemeMode = applicationThemes.themeMode();
    const auto savedThemeConfig = componentThemes.config();
    const bool savedAutoHide = recording.autoHideToolbar();
    const auto savedSize = ui.toolbarSize();
    constexpr auto drawingKind = storage::ScreenshotToolbarLayoutKind::DrawingTools;
    constexpr auto actionKind = storage::ScreenshotToolbarLayoutKind::RecordingActionTools;
    const auto savedDrawing = toolbar.layout(drawingKind);
    const auto savedActions = toolbar.layout(actionKind);
    const auto restore = qScopeGuard([&] {
        applicationThemes.setThemeMode(savedThemeMode);
        componentThemes.setConfig(savedThemeConfig);
        require(recording.setAutoHideToolbar(savedAutoHide) && ui.setToolbarSize(savedSize) &&
                    toolbar.setLayout(drawingKind, savedDrawing) &&
                    toolbar.setLayout(actionKind, savedActions),
                "restore recording toolbar visual-review preferences");
    });
    require(recording.setAutoHideToolbar(true) &&
                toolbar.setLayout(drawingKind, compactLayout(drawingKind, {})) &&
                toolbar.setLayout(actionKind,
                                  compactLayout(actionKind, {{QStringLiteral("start-stop")},
                                                             {QStringLiteral("duration")},
                                                             {QStringLiteral("microphone")}})),
            "prepare compact recording toolbar visual-review fixtures");
    const QString outputDirectory = QDir(QString::fromUtf8(SNOW_SHOT_TEST_TRANSLATIONS_DIR))
                                        .filePath(QStringLiteral("toolbar-auto-hide-artifacts"));
    require(QDir().mkpath(outputDirectory), "create recording toolbar visual-review directory");
    const QDir directory(outputDirectory);
    constexpr int groupWidth = 156;
    constexpr int cellWidth = 176;
    constexpr int rowHeight = 220;
    constexpr int headerHeight = 82;
    constexpr int rowCount = 8;
    QImage contactSheet(groupWidth + cellWidth * 6, headerHeight + rowHeight * rowCount,
                        QImage::Format_ARGB32_Premultiplied);
    contactSheet.fill(QColor(QStringLiteral("#e8edf4")));
    int previewFontId = -1;
    const auto unloadPreviewFont = qScopeGuard([&] {
        if (previewFontId >= 0)
            QFontDatabase::removeApplicationFont(previewFontId);
    });
    QFont headingFont;
    const QString systemFontPath = QStringLiteral("/System/Library/Fonts/Supplemental/Arial.ttf");
    if (QFileInfo::exists(systemFontPath)) {
        previewFontId = QFontDatabase::addApplicationFont(systemFontPath);
        const QStringList families = QFontDatabase::applicationFontFamilies(previewFontId);
        if (!families.isEmpty())
            headingFont.setFamily(families.first());
    }
    QPainter sheet(&contactSheet);
    headingFont.setPixelSize(20);
    headingFont.setBold(true);
    sheet.setFont(headingFont);
    sheet.setPen(QColor(QStringLiteral("#172236")));
    sheet.drawText(QRect(20, 10, contactSheet.width() - 40, 30), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Recording toolbar edge controls"));
    QFont captionFont = headingFont;
    captionFont.setPixelSize(13);
    captionFont.setBold(false);
    sheet.setFont(captionFont);
    sheet.drawText(QRect(20, 41, groupWidth - 30, 35), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Actual paint, 4× view"));
    const QStringList stateLabels{QStringLiteral("Idle"), QStringLiteral("Hover"),
                                  QStringLiteral("Pressed")};
    for (int column = 0; column < 6; ++column) {
        sheet.drawText(QRect(groupWidth + column * cellWidth, 41, cellWidth, 35), Qt::AlignCenter,
                       (column < 3 ? QStringLiteral("Left · ") : QStringLiteral("Right · ")) +
                           stateLabels.at(column % 3));
    }

    int rowIndex = 0;
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        applicationThemes.setThemeAppearance(appearance);
        const bool dark = appearance == styles::ThemeAppearance::Dark;
        const QString themeName = dark ? QStringLiteral("dark") : QStringLiteral("light");
        for (const QString& size : {QStringLiteral("normal"), QStringLiteral("small")}) {
            require(ui.setToolbarSize(size), "configure visual-review toolbar size");
            for (const int dpr : {1, 2}) {
                const int rowTop = headerHeight + rowIndex * rowHeight;
                sheet.fillRect(
                    QRect(0, rowTop, contactSheet.width(), rowHeight),
                    QColor(dark ? QStringLiteral("#141820") : QStringLiteral("#f3f6fb")));
                sheet.setPen(QColor(dark ? QStringLiteral("#e4eaf5") : QStringLiteral("#172236")));
                sheet.drawText(
                    QRect(18, rowTop + 25, groupWidth - 28, 90), Qt::AlignLeft | Qt::AlignTop,
                    QStringLiteral("%1\n%2 toolbar\n%3× raster DPR").arg(themeName, size).arg(dpr));
                ToolbarFixture fixture;
                for (const Side side : {Side::Left, Side::Right}) {
                    fixture.toolbar.showWithoutActivating();
                    fixture.place(side);
                    fixture.collapse();
                    auto* button = qobject_cast<QAbstractButton*>(fixture.control());
                    require(button != nullptr, "render the actual recording edge-control widget");
                    const QString sideName =
                        side == Side::Left ? QStringLiteral("left") : QStringLiteral("right");
                    for (int state = 0; state < stateLabels.size(); ++state) {
                        button->setAttribute(Qt::WA_UnderMouse, state != 0);
                        button->setDown(state == 2);
                        QImage capture(button->width() * dpr, button->height() * dpr,
                                       QImage::Format_ARGB32_Premultiplied);
                        capture.setDevicePixelRatio(dpr);
                        capture.fill(Qt::transparent);
                        QPainter paint(&capture);
                        button->render(&paint);
                        paint.end();
                        const QString filename =
                            QStringLiteral("%1-%2-%3-%4-%5x.png")
                                .arg(themeName, size, sideName, stateLabels.at(state).toLower())
                                .arg(dpr);
                        require(capture.save(directory.filePath(filename)),
                                "save actual edge-control raster preview");
                        const int column = (side == Side::Left ? 0 : 3) + state;
                        const QSize viewSize(button->width() * 4, button->height() * 4);
                        const QRect view(groupWidth + column * cellWidth +
                                             (cellWidth - viewSize.width()) / 2,
                                         rowTop + (rowHeight - viewSize.height()) / 2,
                                         viewSize.width(), viewSize.height());
                        const int edgeX = side == Side::Left ? view.left() : view.right();
                        sheet.setPen(QPen(
                            QColor(dark ? QStringLiteral("#64738a") : QStringLiteral("#99a8bd")),
                            1));
                        sheet.drawLine(edgeX, rowTop + 12, edgeX, rowTop + rowHeight - 12);
                        sheet.setRenderHint(QPainter::SmoothPixmapTransform, false);
                        sheet.drawImage(QRectF(view), capture, QRectF(capture.rect()));
                    }
                    button->setDown(false);
                    button->setAttribute(Qt::WA_UnderMouse, false);
                }
                ++rowIndex;
            }
        }
    }
    sheet.end();
    require(contactSheet.save(directory.filePath(QStringLiteral("contact-sheet.png"))),
            "save the recording edge-control visual-review contact sheet");
    std::cout << outputDirectory.toStdString() << '\n';
}
