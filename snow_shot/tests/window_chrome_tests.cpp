#include "snow_shot/platform/windows/windowchrome.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QToolButton>
#include <QWidget>

#include <cstdlib>
#include <iostream>

#include <qt_windows.h>

namespace {
HWND toNativeHwnd(WId windowId) {
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

qintptr hitTestAt(QWidget& window, QWidget& titleBar, const QPoint& globalPosition) {
    QCursor::setPos(globalPosition);
    flushEvents();

    POINT nativePosition{};
    require(GetCursorPos(&nativePosition) != 0, "the native cursor position should be available");

    MSG message{};
    message.hwnd = toNativeHwnd(window.winId());
    message.message = WM_NCHITTEST;
    message.lParam =
        MAKELPARAM(static_cast<WORD>(nativePosition.x), static_cast<WORD>(nativePosition.y));

    qintptr result = HTERROR;
    require(snow_shot::platform::windows::handleNativeWindowEvent(&titleBar, &message, &result),
            "the window chrome should handle WM_NCHITTEST");
    return result;
}

void raisedOverlayPreventsTitleBarDragging() {
    const QPoint originalCursorPosition = QCursor::pos();

    QWidget window(nullptr, Qt::Window | Qt::FramelessWindowHint);
    window.resize(420, 280);

    QWidget titleBar(&window);
    titleBar.setGeometry(0, 0, window.width(), 48);

    QToolButton windowControl(&titleBar);
    windowControl.setGeometry(376, 8, 32, 32);

    window.show();
    flushEvents();

    const QPoint dragPosition = titleBar.mapToGlobal(QPoint(180, 24));
    require(hitTestAt(window, titleBar, dragPosition) == HTCAPTION,
            "an uncovered blank title-bar point should remain draggable");

    const QPoint windowControlPosition = windowControl.mapToGlobal(windowControl.rect().center());
    require(hitTestAt(window, titleBar, windowControlPosition) == HTCLIENT,
            "a title-bar control should remain in the client area");

    QWidget previewOverlay(&window);
    previewOverlay.setGeometry(window.rect());
    QToolButton previewClose(&previewOverlay);
    previewClose.setGeometry(320, 8, 40, 40);
    previewOverlay.show();
    previewOverlay.raise();
    flushEvents();

    const QPoint previewClosePosition = previewClose.mapToGlobal(previewClose.rect().center());
    require(hitTestAt(window, titleBar, previewClosePosition) == HTCLIENT,
            "a raised preview control over the title bar must not start a window drag");
    require(hitTestAt(window, titleBar, dragPosition) == HTCLIENT,
            "a raised preview surface must occlude the title-bar drag region");

    previewOverlay.hide();
    flushEvents();
    require(hitTestAt(window, titleBar, dragPosition) == HTCAPTION,
            "hiding the preview should restore the title-bar drag region");

    window.hide();
    QCursor::setPos(originalCursorPosition);
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    raisedOverlayPreventsTitleBarDragging();
    return 0;
}
