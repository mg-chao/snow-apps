#include "../src/platform/macos/windowcursorcoordinator_p.h"

#include <QApplication>
#include <QEventLoop>
#include <QMouseEvent>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
using snow_shot::platform::macos::detail::WindowCursorCoordinator;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void drain() {
    for (int turn = 0; turn != 3; ++turn) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

void notify(QObject* receiver, QEvent::Type type) {
    QEvent event(type);
    QCoreApplication::sendEvent(receiver, &event);
}

void mouse(QWidget& widget, QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(10, 10), QPointF(widget.mapToGlobal(QPoint(10, 10))), button,
                      buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

class Surface final : public QWidget {
  public:
    void releaseSurface() {
        destroy();
    }
};

struct Fixture {
    bool active = true;
    bool suspended = false;
    bool inputEnabled = true;
    QPointer<QWidget> hovered;
    QPointer<QWidget> grabber;
    QPointer<QWidget> applied;
    bool grabbed = false;
    int applications = 0;
    int hitTests = 0;
    WindowCursorCoordinator coordinator{{[this] { return active; },
                                         [this] { return suspended; },
                                         [this]() -> QWidget* {
                                             ++hitTests;
                                             return hovered;
                                         },
                                         [this]() -> QWidget* { return grabber; },
                                         [this](QWidget*) { return inputEnabled; },
                                         [this](QWidget* window, bool duringGrab) {
                                             applied = window;
                                             grabbed = duringGrab;
                                             ++applications;
                                         },
                                         {}}};

    void reset() {
        drain();
        applied.clear();
        applications = 0;
        hitTests = 0;
    }
    void refresh() {
        coordinator.invalidate();
        drain();
    }
};

void registrationAndPooling() {
    Fixture f;
    QWidget root, other, popup;
    f.coordinator.addWindow(&root);
    f.coordinator.addWindow(&root);
    drain();
    require(root.internalWinId() == 0, "registration must not materialize prewarmed windows");
    root.show();
    other.show();
    popup.show();
    f.hovered = &root;
    f.reset();
    for (int i = 0; i != 30; ++i)
        root.setCursor(i % 2 ? Qt::CrossCursor : Qt::IBeamCursor);
    drain();
    require(f.applications == 1 && f.hitTests == 1 && f.applied == &root,
            "cursor changes and repeated registration must coalesce to one native update");
    f.reset();
    drain();
    require(f.hitTests == 0, "idle event processing must not poll native hit testing");

    popup.windowHandle()->setTransientParent(root.windowHandle());
    f.hovered = &popup;
    f.refresh();
    require(f.applied == &popup, "transient descendants inherit cursor management");
    f.reset();
    popup.windowHandle()->setTransientParent(other.windowHandle());
    drain();
    require(f.applications == 0, "pooled controls must stop inheriting a previous owner");
    f.coordinator.addWindow(&other);
    drain();
    require(f.applied == &popup, "a new registered owner must manage the pooled control");

    QWidget nested(&root, Qt::Tool);
    nested.show();
    f.hovered = &nested;
    f.refresh();
    require(f.applied == &nested, "QWidget window ownership also defines descendants");
    QWidget unrelated;
    unrelated.show();
    f.reset();
    nested.windowHandle()->setTransientParent(unrelated.windowHandle());
    drain();
    require(f.applications == 0, "live transient ownership must supersede a stale QObject parent");
    f.hovered = &unrelated;
    f.reset();
    f.refresh();
    require(f.applications == 0, "unrelated foreground windows must not be overwritten");
}

void inputAndLifecycle() {
    Fixture f;
    Surface root;
    root.show();
    f.coordinator.addWindow(&root);
    f.hovered = &root;
    f.reset();
    root.setAttribute(Qt::WA_TransparentForMouseEvents);
    f.refresh();
    require(f.applications == 0, "Qt mouse-transparent surfaces cannot own the cursor");
    root.setAttribute(Qt::WA_TransparentForMouseEvents, false);
    root.windowHandle()->setFlag(Qt::WindowTransparentForInput);
    f.refresh();
    require(f.applications == 0, "recapture transparency must surrender cursor ownership");
    root.windowHandle()->setFlag(Qt::WindowTransparentForInput, false);
    f.inputEnabled = false;
    f.refresh();
    require(f.applications == 0, "native click-through and recording pass-through must be honored");
    f.inputEnabled = true;
    notify(root.windowHandle(), QEvent::WindowBlocked);
    f.refresh();
    require(f.applications == 0, "modal-blocked windows cannot update the cursor");
    notify(root.windowHandle(), QEvent::WindowUnblocked);
    drain();
    require(f.applied == &root, "modal completion reconciles the stationary pointer");

    f.reset();
    f.suspended = true;
    f.refresh();
    require(f.applications == 0, "native menu and file-panel tracking must retain the cursor");
    f.suspended = false;
    root.hide();
    f.refresh();
    require(f.applications == 0, "hidden windows cannot apply their cursor");
    root.releaseSurface();
    f.refresh();
    require(f.applications == 0 && root.internalWinId() == 0,
            "pending work cannot revive a destroyed native surface");
    root.show();
    drain();
    require(f.applied == &root, "registration survives native surface recreation");

    auto temporary = std::make_unique<QWidget>();
    temporary->show();
    f.coordinator.addWindow(temporary.get());
    f.hovered = temporary.get();
    f.reset();
    temporary->setCursor(Qt::CrossCursor);
    temporary.reset();
    drain();
    require(f.applications == 0, "destroyed targets cancel pending native updates");
}

void dragAndActivation() {
    Fixture f;
    Surface owner;
    QWidget other, foreign;
    owner.show();
    other.show();
    foreign.show();
    f.coordinator.addWindow(&owner);
    f.coordinator.addWindow(&other);
    f.hovered = &other;
    f.reset();
    mouse(owner, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    drain();
    require(f.applied == &owner && f.grabbed, "implicit press owner wins over pointer hover");
    mouse(owner, QEvent::MouseButtonPress, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
    mouse(owner, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::RightButton);
    drain();
    require(f.applied == &owner && f.grabbed, "retain the owner until the last button releases");
    mouse(owner, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton);
    drain();
    require(f.applied == &other && !f.grabbed, "release resumes hover ownership");

    f.grabber = &foreign;
    f.reset();
    f.refresh();
    require(f.applications == 0, "an unrelated explicit grab must suppress managed updates");
    f.grabber = &owner;
    f.refresh();
    require(f.applied == &owner && f.grabbed, "explicit managed grabs preserve their cursor");
    f.grabber.clear();
    mouse(foreign, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    f.reset();
    f.refresh();
    require(f.applications == 0, "unrelated implicit drags must also retain cursor ownership");

    f.active = false;
    notify(qApp, QEvent::ApplicationDeactivate);
    drain();
    f.active = true;
    notify(qApp, QEvent::ApplicationActivate);
    drain();
    require(f.applied == &other && !f.grabbed, "reactivation discards stale press ownership");
    f.reset();
    f.active = false;
    f.refresh();
    require(f.applications == 0 && f.hitTests == 0,
            "inactive apps must not hit-test or set cursors");
    f.active = true;
    mouse(owner, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    f.reset();
    owner.hide();
    owner.releaseSurface();
    owner.show();
    drain();
    require(f.applications == 0, "a recreated surface must not inherit an unfinished drag");
    mouse(owner, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    drain();
    require(f.applied == &other, "release resumes hover after the original drag surface is gone");
}

void overrideInvalidation() {
    Fixture f;
    QWidget root;
    root.show();
    f.coordinator.addWindow(&root);
    f.hovered = &root;
    f.reset();
    const auto changeOverride = [&](bool push) {
        QEventLoop loop;
        QTimer::singleShot(0, &loop, [push] {
            if (push)
                QApplication::setOverrideCursor(Qt::CrossCursor);
            else
                QApplication::restoreOverrideCursor();
        });
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    };
    changeOverride(true);
    require(f.applied == &root && QApplication::overrideCursor()->shape() == Qt::CrossCursor,
            "override changes must refresh without altering Qt's cursor stack");
    f.reset();
    changeOverride(false);
    require(f.applied == &root && !QApplication::overrideCursor(),
            "override restoration must refresh a stationary pointer");
}

void nativeReleaseReconciliation() {
    QWidget owner, hovered;
    owner.show();
    hovered.show();
    bool pressed = false;
    QPointer<QWidget> applied;
    WindowCursorCoordinator coordinator(
        {[] { return true; }, [] { return false; }, [&] { return &hovered; },
         []() -> QWidget* { return nullptr; }, [](QWidget*) { return true; },
         [&](QWidget* target, bool) { applied = target; }, [&] { return pressed; }});
    coordinator.addWindow(&owner);
    coordinator.addWindow(&hovered);
    drain();
    pressed = true;
    applied.clear();
    hovered.setCursor(Qt::CrossCursor);
    drain();
    require(!applied, "a native press without a Qt owner must not become a hover update");
    mouse(owner, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    drain();
    require(applied == &owner, "a Qt press establishes the native gesture's owner");
    // A menu consumes the release, so no Qt MouseButtonRelease is delivered.
    pressed = false;
    hovered.setCursor(Qt::IBeamCursor);
    drain();
    require(applied == &hovered, "a native release must clear stale implicit ownership");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    registrationAndPooling();
    inputAndLifecycle();
    dragAndActivation();
    overrideInvalidation();
    nativeReleaseReconciliation();
    std::cout << "macOS cursor ownership tests passed\n";
}
