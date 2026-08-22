#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "theme/theme_manager.h"
#include "widgets/context_menu.h"
#include "widgets/detail/button_rendering.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

std::array<QPoint, 4> cornerPixels(const QSize& size) {
    return {QPoint(0, 0), QPoint(size.width() - 1, 0), QPoint(0, size.height() - 1),
            QPoint(size.width() - 1, size.height() - 1)};
}

void populateProductionSizedMenu(adqt::widgets::AdContextMenu& menu) {
    adqt::widgets::AdContextMenu::ComponentTokens tokens;
    tokens.background = QColor(QStringLiteral("#ffffff"));
    tokens.border = QColor(QStringLiteral("#1677ff"));
    tokens.borderRadius = 8;
    menu.setComponentTokens(tokens);
    menu.setFixedWidth(300);
    menu.addItem(QStringLiteral("Copy to Clipboard"));
    QAction* disabled = menu.addItem(QStringLiteral("Recognizing text"));
    disabled->setEnabled(false);
    QAction* checkable = menu.addItem(QStringLiteral("Drawing Mode"));
    checkable->setCheckable(true);
    menu.addSeparator();
    adqt::widgets::AdContextMenu* processMenu = menu.addSubMenu(QStringLiteral("Process Image"));
    processMenu->addItem(QStringLiteral("Rotate Clockwise"));
    processMenu->addItem(QStringLiteral("Flip Horizontally"));
    menu.addSubMenu(QStringLiteral("Opacity"))->addItem(QStringLiteral("100%"));
    menu.addSeparator();
    menu.addItem(QStringLiteral("Close"));
}

QPixmap captureCompositedWindow(const QWidget& widget) {
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(widget.winId()); // NOLINT(performance-no-int-to-ptr)
    RECT windowRect{};
    require(hwnd != nullptr && GetWindowRect(hwnd, &windowRect) != FALSE,
            "native context menu geometry was unavailable");

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    require(monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo) != FALSE,
            "native context menu monitor geometry was unavailable");

    QScreen* screen = widget.screen();
    require(screen != nullptr, "native context menu has no screen");
    const QImage monitorCapture = screen->grabWindow(0).toImage();
    require(!monitorCapture.isNull(), "native monitor capture failed");

    const QRect physicalWindowRect(
        windowRect.left - monitorInfo.rcMonitor.left, windowRect.top - monitorInfo.rcMonitor.top,
        windowRect.right - windowRect.left, windowRect.bottom - windowRect.top);
    require(QRect(QPoint(0, 0), monitorCapture.size()).contains(physicalWindowRect),
            "native context menu lay outside its monitor capture");

    QImage windowCapture = monitorCapture.copy(physicalWindowRect);
    windowCapture.setDevicePixelRatio(widget.devicePixelRatioF());
    return QPixmap::fromImage(windowCapture);
#else
    QScreen* screen = widget.screen();
    require(screen != nullptr, "native context menu has no screen");
    const QPoint globalTopLeft = widget.mapToGlobal(QPoint(0, 0));
    return screen->grabWindow(0, globalTopLeft.x(), globalTopLeft.y(), widget.width(),
                              widget.height());
#endif
}

void dirtyRenderTargetCornersAreCleared() {
    adqt::widgets::AdContextMenu menu;
    require(menu.windowFlags().testFlag(Qt::Popup),
            "context menu must remain a native popup for tray and keyboard behavior");
    require(menu.windowFlags().testFlag(Qt::FramelessWindowHint),
            "translucent context menu must use a frameless top-level window");
    require(menu.windowFlags().testFlag(Qt::NoDropShadowWindowHint),
            "context menu must own its shadow contract instead of using a platform shadow");
    require(menu.testAttribute(Qt::WA_TranslucentBackground),
            "context menu surface must expose an alpha channel");
    populateProductionSizedMenu(menu);
    menu.ensurePolished();
    menu.adjustSize();
    require(menu.width() > 0 && menu.height() > 0, "context menu did not acquire a render size");

    QImage image(menu.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(0, 0, 0, 255));
    {
        QPainter painter(&image);
        menu.render(&painter);
    }

    for (const QPoint& corner : cornerPixels(image.size())) {
        require(image.pixelColor(corner).alpha() == 0,
                "context menu did not clear a dirty rounded-corner pixel to transparent");
    }
}

void borderGeometryUsesWholeDevicePixels() {
    constexpr std::array<qreal, 4> devicePixelRatios = {1.0, 1.25, 1.5, 2.0};
    for (const qreal devicePixelRatio : devicePixelRatios) {
        const qreal logicalWidth =
            adqt::widgets::detail::deviceAlignedPenWidth(1.0, devicePixelRatio);
        const int physicalWidth = qRound(logicalWidth * devicePixelRatio);
        require(physicalWidth >= 1 && qFuzzyCompare(logicalWidth * devicePixelRatio,
                                                    static_cast<qreal>(physicalWidth)),
                "border width was not quantized to a whole device pixel");

        const qreal center =
            adqt::widgets::detail::deviceAlignedStrokeCenter(16.0, logicalWidth, devicePixelRatio);
        const qreal physicalCenter = center * devicePixelRatio;
        const qreal phase = (physicalWidth % 2 == 0) ? 0.0 : 0.5;
        require(std::abs(physicalCenter - (qRound(physicalCenter - phase) + phase)) < 0.001,
                "border center was not aligned to the device pixel phase");
    }
}

void constrainedPopupKeepsActionGeometryInsideSurface() {
    QWidget parent;
    parent.resize(640, 480);
    parent.show();
    require(waitUntil([&parent]() {
                return parent.windowHandle() != nullptr && parent.windowHandle()->isExposed();
            }),
            "context menu geometry parent was not exposed");

    adqt::widgets::AdContextMenu menu(&parent);
    adqt::widgets::AdContextMenu::ComponentTokens tokens;
    tokens.horizontalPadding = 12;
    tokens.secondaryText = QColor(QStringLiteral("#ff0000"));
    menu.setComponentTokens(tokens);
    menu.setFixedWidth(300);
    QAction* shortcutAction =
        menu.addItem(QStringLiteral("A deliberately long translated menu item\tCtrl+Shift+C"));
    adqt::widgets::AdContextMenu* submenu =
        menu.addSubMenu(QStringLiteral("A deliberately long submenu item"));
    submenu->addItem(QStringLiteral("Child"));

    menu.popupAt(parent.mapToGlobal(QPoint(40, 40)));
    require(waitUntil([&menu, shortcutAction]() {
                return menu.isVisible() && menu.actionGeometry(shortcutAction).isValid();
            }),
            "constrained context menu geometry was not available");
    require(menu.actionGeometry(shortcutAction).right() < menu.width() &&
                menu.actionGeometry(submenu->menuAction()).right() < menu.width(),
            "a constrained context menu exposed action geometry beyond its visible surface");

    const auto rightmostShortcutPixel = [](adqt::widgets::AdContextMenu& renderedMenu,
                                           QAction* action) {
        const QImage image = renderedMenu.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        const QRect actionGeometry = renderedMenu.actionGeometry(action);
        int rightmostPixel = -1;
        for (int y = actionGeometry.top(); y <= actionGeometry.bottom(); ++y) {
            for (int x = actionGeometry.left(); x <= actionGeometry.right(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.red() > 180 && pixel.green() < 100 && pixel.blue() < 100) {
                    rightmostPixel = std::max(rightmostPixel, x);
                }
            }
        }
        return rightmostPixel;
    };

    adqt::widgets::AdContextMenu referenceMenu(&parent);
    referenceMenu.setComponentTokens(tokens);
    referenceMenu.setFixedWidth(300);
    QAction* referenceShortcutAction = referenceMenu.addItem(
        QStringLiteral("A deliberately long translated menu item\tCtrl+Shift+C"));
    referenceMenu.popupAt(parent.mapToGlobal(QPoint(360, 40)));
    require(waitUntil([&referenceMenu, referenceShortcutAction]() {
                return referenceMenu.isVisible() &&
                       referenceMenu.actionGeometry(referenceShortcutAction).isValid();
            }),
            "reference context menu geometry was not available");
    require(rightmostShortcutPixel(menu, shortcutAction) ==
                rightmostShortcutPixel(referenceMenu, referenceShortcutAction),
            "a sibling submenu added trailing space after the shortcut text");
    referenceMenu.hide();

    menu.setActiveAction(submenu->menuAction());
    QKeyEvent openSubmenu(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCoreApplication::sendEvent(&menu, &openSubmenu);
    require(waitUntil([submenu]() { return submenu->isVisible(); }),
            "the constrained context submenu did not open");
    require(submenu->geometry().left() <= menu.geometry().right() &&
                submenu->geometry().left() >= menu.geometry().right() - 16,
            "the constrained context submenu opened beyond the visible parent edge");
    submenu->hide();
    menu.hide();
    parent.hide();
}

int colorDistance(const QColor& lhs, const QColor& rhs) {
    return std::max({std::abs(lhs.red() - rhs.red()), std::abs(lhs.green() - rhs.green()),
                     std::abs(lhs.blue() - rhs.blue())});
}

QColor logicalPixel(const QPixmap& pixmap, const QPoint& logicalPosition) {
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    const qreal dpr = pixmap.devicePixelRatio();
    const QPoint physicalPosition(
        std::clamp(qRound((logicalPosition.x() + 0.5) * dpr - 0.5), 0, image.width() - 1),
        std::clamp(qRound((logicalPosition.y() + 0.5) * dpr - 0.5), 0, image.height() - 1));
    return image.pixelColor(physicalPosition);
}

void nativePopupCornersCompositeOverTheirBackdrop() {
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        return;
    }

    const QColor backdropColor(QStringLiteral("#c12f76"));
    QWidget backdrop;
    backdrop.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    backdrop.setAutoFillBackground(true);
    QPalette palette = backdrop.palette();
    palette.setColor(QPalette::Window, backdropColor);
    backdrop.setPalette(palette);
    backdrop.resize(480, 320);
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        backdrop.move(screen->availableGeometry().center() - backdrop.rect().center());
    }
    backdrop.show();
    require(waitUntil([&backdrop]() {
                return backdrop.windowHandle() != nullptr && backdrop.windowHandle()->isExposed();
            }),
            "native backdrop window was not exposed");

    adqt::widgets::AdContextMenu menu(&backdrop);
    populateProductionSizedMenu(menu);
    menu.popupAt(backdrop.mapToGlobal(QPoint(120, 90)));
    require(waitUntil([&menu]() {
                return menu.isVisible() && menu.windowHandle() != nullptr &&
                       menu.windowHandle()->isExposed();
            }),
            "native context menu was not exposed");
    require(waitUntil([&menu]() { return menu.windowOpacity() >= 0.99; }),
            "native context menu fade-in did not settle");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    const QPixmap capture = captureCompositedWindow(menu);
    require(!capture.isNull(), "native context menu capture failed");

    const QColor expectedBorder(QStringLiteral("#1677ff"));
    const QColor expectedBackground(QStringLiteral("#ffffff"));
    for (const QPoint& corner : cornerPixels(menu.size())) {
        const int xDirection = corner.x() == 0 ? 1 : -1;
        const int yDirection = corner.y() == 0 ? 1 : -1;
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                const QPoint sample = corner + QPoint(x * xDirection, y * yDirection);
                require(colorDistance(logicalPixel(capture, sample), backdropColor) <= 12,
                        "native context menu outer corner did not composite over its backdrop");
            }
        }
    }

    const int cornerRadius = 8;
    for (int x = cornerRadius + 2; x < menu.width() - cornerRadius - 2; ++x) {
        require(colorDistance(logicalPixel(capture, QPoint(x, 0)), expectedBorder) <= 24,
                "native context menu top border contains a gap");
        require(colorDistance(logicalPixel(capture, QPoint(x, menu.height() - 1)),
                              expectedBorder) <= 24,
                "native context menu bottom border contains a gap");
    }
    for (int y = cornerRadius + 2; y < menu.height() - cornerRadius - 2; ++y) {
        require(colorDistance(logicalPixel(capture, QPoint(0, y)), expectedBorder) <= 24,
                "native context menu left border contains a gap");
        require(colorDistance(logicalPixel(capture, QPoint(menu.width() - 1, y)), expectedBorder) <=
                    24,
                "native context menu right border contains a gap");
    }

    require(colorDistance(logicalPixel(capture, QPoint(menu.width() / 2, menu.height() / 2)),
                          expectedBackground) <= 12,
            "native context menu panel fill is not opaque");

    for (int y = 0; y <= cornerRadius + 1; ++y) {
        for (int x = 0; x <= cornerRadius + 1; ++x) {
            const QColor topLeft = logicalPixel(capture, QPoint(x, y));
            const QColor topRight = logicalPixel(capture, QPoint(menu.width() - 1 - x, y));
            const QColor bottomLeft = logicalPixel(capture, QPoint(x, menu.height() - 1 - y));
            const QColor bottomRight =
                logicalPixel(capture, QPoint(menu.width() - 1 - x, menu.height() - 1 - y));
            require(colorDistance(topLeft, topRight) <= 24 &&
                        colorDistance(topLeft, bottomLeft) <= 24 &&
                        colorDistance(topLeft, bottomRight) <= 24,
                    "native context menu rounded corners are not symmetric");
        }
    }
    menu.hide();
    backdrop.hide();
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    adqt::theme::ThemeManager::instance().applyTo(application);
    try {
        borderGeometryUsesWholeDevicePixels();
        constrainedPopupKeepsActionGeometryInsideSurface();
        nativePopupCornersCompositeOverTheirBackdrop();
        dirtyRenderTargetCornersAreCleared();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
