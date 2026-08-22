#include "snow_shot/platform/windows/windowchrome.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QPoint>
#include <QWidget>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::processEvents();
}

void overlayObscuresTitleBarDragArea() {
    QWidget window;
    window.resize(420, 300);

    QWidget titleBar(&window);
    titleBar.setGeometry(0, 0, window.width(), 48);

    QWidget titleBarButton(&titleBar);
    titleBarButton.setGeometry(360, 4, 40, 40);

    QWidget overlay(&window);
    overlay.setGeometry(window.rect());
    overlay.hide();

    QWidget overlayCloseButton(&overlay);
    overlayCloseButton.setGeometry(366, 8, 42, 42);

    window.show();
    flushEvents();

    using snow_shot::platform::windows::isTitleBarDragArea;
    const QPoint exposedTitleBarPoint = titleBar.mapToGlobal(QPoint(120, 24));
    const QPoint titleBarButtonPoint = titleBarButton.mapToGlobal(titleBarButton.rect().center());

    require(isTitleBarDragArea(&titleBar, exposedTitleBarPoint),
            "an exposed title-bar background should remain draggable");
    require(!isTitleBarDragArea(&titleBar, titleBarButtonPoint),
            "a title-bar control should not become draggable");

    overlay.show();
    overlay.raise();
    flushEvents();

    const QPoint overlayBackgroundPoint = overlay.mapToGlobal(QPoint(120, 24));
    const QPoint overlayButtonPoint =
        overlayCloseButton.mapToGlobal(overlayCloseButton.rect().center());
    require(!isTitleBarDragArea(&titleBar, overlayBackgroundPoint),
            "an overlay covering the title bar should keep client-area input");
    require(!isTitleBarDragArea(&titleBar, overlayButtonPoint),
            "an overlay control covering the title bar should remain clickable");

    overlay.hide();
    flushEvents();
    require(isTitleBarDragArea(&titleBar, exposedTitleBarPoint),
            "hiding the overlay should restore title-bar dragging");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    overlayObscuresTitleBarDragArea();
    return 0;
}
