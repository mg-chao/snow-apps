#include "snow_shot/presentation/mousereleaseactioncontroller.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
using snow_shot::presentation::MouseReleaseActionController;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void release(QWidget& receiver, Qt::MouseButton button, QPointF point = {10, 10}) {
    QMouseEvent event(QEvent::MouseButtonRelease, point, point, button, Qt::NoButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &event);
}

void completesAfterReleaseDispatch() {
    QWidget window;
    window.resize(100, 100);
    window.show();
    QApplication::processEvents();
    MouseReleaseActionController controller;
    int closes = 0;
    require(controller.arm(&window, Qt::RightButton, [&] { ++closes; }), "arm right close");
    require(closes == 0 && window.isVisible(), "arming must keep the window visible");
    require(!controller.arm(&window, Qt::MiddleButton, [&] { closes += 100; }),
            "another action cannot replace an owned gesture");
    release(window, Qt::LeftButton);
    require(closes == 0 && controller.pending(), "unrelated release must not complete");
    release(window, Qt::RightButton, {-100, 200});
    require(closes == 0, "action must wait until release dispatch returns");
    QContextMenuEvent context(QContextMenuEvent::Mouse, {10, 10}, {10, 10});
    context.ignore();
    QCoreApplication::sendEvent(&window, &context);
    require(context.isAccepted(), "dismissing right-click must not open a context menu");
    QApplication::processEvents();
    require(closes == 1 && !controller.pending() && !QWidget::mouseGrabber(),
            "outside release must complete once and release capture");
    release(window, Qt::RightButton);
    QApplication::processEvents();
    require(closes == 1, "duplicate release must not execute again");
    require(controller.arm(&window, Qt::MiddleButton, [&] { ++closes; }), "arm next gesture");
    release(window, Qt::MiddleButton);
    QApplication::processEvents();
    require(closes == 2, "next independent gesture must work");
}

void interruptionsDiscardPendingAndQueuedActions() {
    for (int scenario = 0; scenario != 5; ++scenario) {
        QWidget window;
        window.show();
        QApplication::processEvents();
        MouseReleaseActionController controller;
        int closes = 0;
        require(controller.arm(&window, Qt::RightButton, [&] { ++closes; }), "arm interruption");
        switch (scenario) {
        case 0:
            window.hide();
            break;
        case 1: {
            QEvent deactivate(QEvent::ApplicationDeactivate);
            QCoreApplication::sendEvent(qApp, &deactivate);
            break;
        }
        case 2: {
            QEvent ungrab(QEvent::UngrabMouse);
            QCoreApplication::sendEvent(&window, &ungrab);
            break;
        }
        case 3:
            release(window, Qt::RightButton);
            controller.cancel();
            break;
        case 4:
            controller.cancel();
            break;
        }
        release(window, Qt::RightButton);
        QApplication::processEvents();
        require(closes == 0 && !controller.pending(), "interrupted gesture must never close later");
    }
}

void captureAndCallbackLifetimesAreRespected() {
    QWidget window;
    QWidget other;
    window.show();
    other.show();
    QApplication::processEvents();
    MouseReleaseActionController lifetime;
    int closes = 0;
    auto retiring = std::make_unique<QWidget>();
    retiring->show();
    QApplication::processEvents();
    require(lifetime.arm(retiring.get(), Qt::RightButton, [&] { ++closes; }), "arm retiring scope");
    release(*retiring, Qt::RightButton);
    retiring.reset();
    QApplication::processEvents();
    require(closes == 0 && !lifetime.pending(), "scope destruction must cancel queued closing");
    auto controller = std::make_unique<MouseReleaseActionController>();
    other.grabMouse();
    require(!controller->arm(&window, Qt::LeftButton, [] {}),
            "must not steal another window's capture");
    require(QWidget::mouseGrabber() == &other, "unrelated capture must remain owned");
    other.releaseMouse();
    require(controller->arm(&window, Qt::LeftButton, [&] { controller.reset(); }),
            "arm destructive callback");
    release(window, Qt::LeftButton);
    QApplication::processEvents();
    require(!controller && !QWidget::mouseGrabber(), "callback may destroy its controller");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    completesAfterReleaseDispatch();
    interruptionsDiscardPendingAndQueuedActions();
    captureAndCallbackLifetimesAreRespected();
    return 0;
}
