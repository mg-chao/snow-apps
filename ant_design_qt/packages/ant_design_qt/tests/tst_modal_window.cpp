#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QEventLoop>
#include <QLayout>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QLabel>
#include <QToolButton>
#include <QWidget>
#include <QWindow>
#include <QtTest>
#include <memory>

#ifdef Q_OS_MACOS
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#endif

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "widgets/modal.h"

using adqt::widgets::AdModal;

namespace {

constexpr auto kOverlayObjectName = "ad-modal-overlay";

#ifdef Q_OS_MACOS
struct NativeOrderRecord {
  QString title;
  NSWindowOrderingMode mode;
  NSInteger relative;
  NSWindowAnimationBehavior animation;
};
QList<NativeOrderRecord> nativeOrders;
NSWindow* ownerActivatingDuringOrderOut = nil;

void observeNativeOrdering() {
  static const bool installed = [] {
    const SEL selector = @selector(orderWindow:relativeTo:);
    const Method method = class_getInstanceMethod(NSWindow.class, selector);
    const IMP original = method_getImplementation(method);
    method_setImplementation(
        method, imp_implementationWithBlock(^(NSWindow* window, NSWindowOrderingMode mode,
                                              NSInteger relative) {
          nativeOrders.append(
              {QString::fromNSString(window.title), mode, relative, window.animationBehavior});
          if (mode == NSWindowOut && ownerActivatingDuringOrderOut) {
            // AppKit can return focus to the owner synchronously inside orderOut,
            // before QWidget clears its visible state. Reproduce that ordering.
            NSWindow* owner = ownerActivatingDuringOrderOut;
            ownerActivatingDuringOrderOut = nil;
            [NSNotificationCenter.defaultCenter
                postNotificationName:NSWindowDidBecomeKeyNotification
                              object:owner];
          }
          reinterpret_cast<void (*)(id, SEL, NSWindowOrderingMode, NSInteger)>(original)(
              window, selector, mode, relative);
        }));
    return true;
  }();
  Q_UNUSED(installed)
  nativeOrders.clear();
}

bool orderedAbove(NSWindow* window, NSWindow* owner) {
  NSArray<NSNumber*>* windows = [NSWindow windowNumbersWithOptions:0];
  const NSUInteger windowIndex = [windows indexOfObject:@(window.windowNumber)];
  const NSUInteger ownerIndex = [windows indexOfObject:@(owner.windowNumber)];
  return windowIndex != NSNotFound && ownerIndex != NSNotFound && windowIndex < ownerIndex;
}
#endif

class SurfaceLifecycleObserver : public QObject {
 public:
  int disruptions = 0;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Hide || event->type() == QEvent::Show ||
        event->type() == QEvent::WinIdChange) {
      ++disruptions;
    }
    return QObject::eventFilter(watched, event);
  }
};

class ModalityObserver : public QObject {
 public:
  bool blocked = false;

 protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == QEvent::WindowBlocked) {
      blocked = true;
    } else if (event->type() == QEvent::WindowUnblocked) {
      blocked = false;
    }
    return false;
  }
};

// Returns the modal dialog surface the way an external observer would find
// it: as a top-level widget. A window-mode overlay with no owner window is a
// parentless top-level dialog, so findChild() cannot be used here.
QWidget* visibleOverlaySurface(const QString& title = {}) {
  const QList<QWidget*> topLevels = QApplication::topLevelWidgets();
  for (QWidget* widget : topLevels) {
    if (widget && widget->isVisible() &&
        widget->objectName() == QString::fromLatin1(kOverlayObjectName) &&
        (title.isEmpty() || widget->windowTitle() == title)) {
      return widget;
    }
  }
  return nullptr;
}

QWidget* modalSection(QWidget* surface, const char* objectName) {
  if (surface == nullptr) {
    return nullptr;
  }
  return surface->findChild<QWidget*>(QString::fromLatin1(objectName));
}

QMargins sectionMargins(QWidget* surface, const char* objectName) {
  QWidget* section = modalSection(surface, objectName);
  if (section == nullptr || section->layout() == nullptr) {
    return {};
  }
  return section->layout()->contentsMargins();
}

// A window-mode modal must show a dialog surface even when no owner window
// can be resolved: tray-menu actions and background notifications open
// dialogs while the application has no active or visible window.
void requireOpenProducesVisibleWindow(AdModal& modal, const QString& title) {
  modal.setWindowTitle(title);
  modal.open();

  QVERIFY(modal.isOpen());
  QWidget* surface = visibleOverlaySurface(title);
  QVERIFY2(surface, "window-mode modal is open but no visible dialog surface exists");
  QVERIFY(!surface->geometry().isEmpty());

  modal.close();
  QVERIFY(!modal.isOpen());
  QVERIFY(!visibleOverlaySurface(title));
}

class TstModalWindow : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase() {
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
      observeNativeOrdering();
    }
#endif
  }
  // Guard the precondition shared by the tests below: no ambient window
  // state that resolveOwnerWindow() could pick up.
  void init() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
      if (widget && widget->objectName() != QString::fromLatin1(kOverlayObjectName)) {
        widget->hide();
        widget->deleteLater();
      }
    }
    qApp->processEvents();
    QVERIFY(QApplication::activeWindow() == nullptr);
  }

  void windowModeWithoutOwnerShowsDialog() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    requireOpenProducesVisibleWindow(modal, QStringLiteral("Ownerless window"));
  }

  void windowModeDetachedWithoutOwnerShowsDialog() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    requireOpenProducesVisibleWindow(modal, QStringLiteral("Ownerless detached"));
  }

#ifdef Q_OS_MACOS
  void macOwnerModalPreservesDefaultPresentation_data() {
    QTest::addColumn<bool>("confirm");
    QTest::addColumn<bool>("elevated");
    QTest::addColumn<bool>("nonmodal");
    QTest::newRow("regular-modal") << false << false << false;
    QTest::newRow("destroy-confirm") << true << false << false;
    QTest::newRow("elevated-modal") << false << true << false;
    QTest::newRow("elevated-confirm") << true << true << false;
    QTest::newRow("elevated-nonmodal") << false << true << true;
  }

  void macOwnerModalPreservesDefaultPresentation() {
    QFETCH(bool, confirm);
    QFETCH(bool, elevated);
    QFETCH(bool, nonmodal);
    QWidget owner;
    if (elevated) {
      owner.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    }
    owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    AdModal modal(&owner);
    modal.setMode(AdModal::Mode::Window);
    modal.setPreset(confirm ? AdModal::Preset::Confirm : AdModal::Preset::Plain);
    if (nonmodal) {
      modal.setWindowModality(Qt::NonModal);
    }
    modal.setWindowTitle(QStringLiteral("Default modal presentation"));
    modal.setText(QStringLiteral("Destroy this pinned window?"));
    const auto verifyPresentation = [&] {
      QWidget* surface = visibleOverlaySurface(modal.windowTitle());
      QVERIFY(surface);
      QVERIFY(QTest::qWaitForWindowExposed(surface));
      if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        NSWindow* window = reinterpret_cast<NSView*>(surface->winId()).window;
        NSWindow* nativeOwner = reinterpret_cast<NSView*>(owner.winId()).window;
        // AppKit starts a normal order-in animation for this independent window.
        // addChildWindow after show cancels it; attaching before show suppresses it.
        QVERIFY(window.parentWindow == nil);
        QCOMPARE(window.animationBehavior, NSWindowAnimationBehaviorDefault);
        QVERIFY(orderedAbove(window, nativeOwner));
        bool presented = false;
        for (const auto& order : nativeOrders) {
          if (order.title == modal.windowTitle() && order.mode == NSWindowAbove) {
            presented = true;
            QCOMPARE(order.animation, NSWindowAnimationBehaviorDocumentWindow);
          }
        }
        QVERIFY(presented);
        if (!nonmodal) {
          [nativeOwner orderFront:nil];
          QVERIFY(orderedAbove(window, nativeOwner));
        }
      }
      nativeOrders.clear();
    };
    nativeOrders.clear();
    modal.open();
    verifyPresentation();
    modal.close();
    modal.open();
    verifyPresentation();
    modal.setWindowTaskbarVisible(true);
    verifyPresentation();
    modal.setWindowTaskbarVisible(false);
    verifyPresentation();
    modal.close();
  }

  void macClosingDoesNotReactivateSurface_data() {
    QTest::addColumn<int>("action");
    QTest::newRow("cancel") << 0;
    QTest::newRow("destroy") << 1;
    QTest::newRow("window-close") << 2;
  }

  void macClosingDoesNotReactivateSurface() {
    QFETCH(int, action);
    QWidget owner;
    owner.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    AdModal modal(&owner);
    modal.setMode(AdModal::Mode::Window);
    modal.setPreset(AdModal::Preset::Confirm);
    modal.setWindowTitle(QStringLiteral("Closing confirmation"));
    for (int presentation = 0; presentation < 2; ++presentation) {
      modal.open();
      QWidget* surface = modal.acceptButton()->window();
      QVERIFY(QTest::qWaitForWindowExposed(surface));
      QVERIFY(QGuiApplication::modalWindow());
      const bool cocoa = QGuiApplication::platformName() == QStringLiteral("cocoa");
      if (cocoa) {
        ownerActivatingDuringOrderOut = reinterpret_cast<NSView*>(owner.winId()).window;
      }
      nativeOrders.clear();
      if (action == 0) {
        modal.reject();
      } else if (action == 1) {
        modal.accept();
      } else {
        surface->close();
      }
      QVERIFY(!modal.isOpen());
      QVERIFY(!surface->isVisible());
      QVERIFY(!QGuiApplication::modalWindow());
      if (cocoa) {
        QVERIFY(!ownerActivatingDuringOrderOut);
        bool hidden = false;
        for (const auto& order : nativeOrders) {
          if (order.title != modal.windowTitle()) {
            continue;
          }
          // Dismissal may order out more than once, but must never order in
          // again or restart the native presentation animation.
          QCOMPARE(order.mode, NSWindowOut);
          hidden = true;
        }
        QVERIFY(hidden);
      }
    }
  }

  void macPresentationRespectsExplicitAnimationAndRestoresDefault() {
    QWidget owner;
    owner.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    AdModal modal(&owner);
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowTitle(QStringLiteral("Explicit presentation"));
    modal.open();
    QWidget* surface = modal.acceptButton()->window();
    QVERIFY(QTest::qWaitForWindowExposed(surface));
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
      return;
    }
    NSWindow* window = reinterpret_cast<NSView*>(surface->winId()).window;
    for (auto behavior : {NSWindowAnimationBehaviorNone, NSWindowAnimationBehaviorUtilityWindow,
                          NSWindowAnimationBehaviorDefault}) {
      window.animationBehavior = behavior;
      nativeOrders.clear();
      modal.close();
      modal.open();
      QVERIFY(QTest::qWaitForWindowExposed(surface));
      bool shown = false;
      bool hidden = false;
      for (const auto& order : nativeOrders) {
        if (order.title != modal.windowTitle()) {
          continue;
        }
        shown |= order.mode == NSWindowAbove;
        hidden |= order.mode == NSWindowOut;
        QCOMPARE(order.animation, behavior == NSWindowAnimationBehaviorDefault
                                      ? NSWindowAnimationBehaviorDocumentWindow
                                      : behavior);
      }
      QVERIFY(shown);
      QVERIFY(hidden);
      QCOMPARE(window.animationBehavior, behavior);
    }
    modal.close();
    QCOMPARE(window.animationBehavior, NSWindowAnimationBehaviorDefault);
  }

  void macApplicationModalPreservesNativeAnimationPolicy() {
    QWidget owner;
    owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    AdModal modal(&owner);
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModality(Qt::ApplicationModal);
    modal.setWindowTitle(QStringLiteral("Application modal presentation"));
    nativeOrders.clear();
    modal.open();
    QVERIFY(QTest::qWaitForWindowExposed(modal.acceptButton()->window()));
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
      // Cocoa begins application modality at the event-loop boundary and can
      // present through its own modal-session API rather than orderWindow.
      QEventLoop loop;
      connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, &loop,
              &QEventLoop::quit, Qt::QueuedConnection);
      loop.exec();
      NSWindow* window = reinterpret_cast<NSView*>(modal.acceptButton()->window()->winId()).window;
      QCOMPARE(NSApp.modalWindow, window);
      QCOMPARE(window.animationBehavior, NSWindowAnimationBehaviorDefault);
      [window orderFront:nil];
      bool presented = false;
      for (const auto& order : nativeOrders) {
        if (order.title == modal.windowTitle() && order.mode == NSWindowAbove) {
          presented = true;
          QCOMPARE(order.animation, NSWindowAnimationBehaviorDefault);
        }
      }
      QVERIFY(presented);
    }
  }

  void macOwnerOrderingDoesNotPromoteModalAboveUnrelatedWindows() {
    QWidget owner;
    owner.setWindowTitle(QStringLiteral("Ordering owner"));
    QWidget unrelated;
    owner.show();
    unrelated.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    QVERIFY(QTest::qWaitForWindowExposed(&unrelated));
    AdModal modal(&owner);
    modal.setMode(AdModal::Mode::Window);
    modal.open();
    QWidget* surface = modal.acceptButton()->window();
    QVERIFY(QTest::qWaitForWindowExposed(surface));
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
      NSWindow* window = reinterpret_cast<NSView*>(surface->winId()).window;
      NSWindow* nativeOwner = reinterpret_cast<NSView*>(owner.winId()).window;
      NSWindow* nativeUnrelated = reinterpret_cast<NSView*>(unrelated.winId()).window;
      QCOMPARE(window.level, nativeOwner.level);
      [nativeUnrelated makeKeyAndOrderFront:nil];
      QVERIFY(orderedAbove(nativeUnrelated, window));
      nativeOrders.clear();
      [nativeOwner orderFront:nil];
      // Verify the ordering passed to AppKit, before any update notification can
      // repair an inverted order. The owner must never cover its modal for a frame.
      bool orderedOwner = false;
      for (const auto& order : nativeOrders) {
        if (order.title == owner.windowTitle()) {
          orderedOwner = true;
          QCOMPARE(order.mode, NSWindowBelow);
          QCOMPARE(order.relative, window.windowNumber);
        }
      }
      QVERIFY(orderedOwner);
      QVERIFY(orderedAbove(window, nativeOwner));
      QVERIFY(orderedAbove(nativeUnrelated, window));
      [nativeOwner makeKeyAndOrderFront:nil];
      QVERIFY(NSApp.keyWindow != nativeOwner);
      QVERIFY(orderedAbove(window, nativeOwner));
      [nativeUnrelated makeKeyAndOrderFront:nil];
      QVERIFY(orderedAbove(nativeUnrelated, window));
      [window orderBack:nil];
      QVERIFY(orderedAbove(window, nativeOwner));
      QVERIFY(orderedAbove(nativeUnrelated, window));
      modal.close();
      [nativeOwner makeKeyAndOrderFront:nil];
      QVERIFY(orderedAbove(nativeOwner, nativeUnrelated));
    }
  }

  void macOwnerModalMovesIndependentlyAndBlocksOnlyOwner_data() {
    QTest::addColumn<bool>("expandedOwner");
    QTest::newRow("ordinary-owner") << false;
    QTest::newRow("main-window-owner") << true;
  }

  void macOwnerModalMovesIndependentlyAndBlocksOnlyOwner() {
    QFETCH(bool, expandedOwner);
    QWidget owner;
    if (expandedOwner) {
      owner.setWindowFlags(owner.windowFlags() | Qt::ExpandedClientAreaHint |
                           Qt::NoTitleBarBackgroundHint);
    }
    QWidget unrelated;
    QPushButton ownerButton(QStringLiteral("Owner"), &owner);
    QPushButton unrelatedButton(QStringLiteral("Unrelated"), &unrelated);
    QSignalSpy ownerClicks(&ownerButton, &QPushButton::clicked);
    QSignalSpy unrelatedClicks(&unrelatedButton, &QPushButton::clicked);
    const QRect available = qApp->primaryScreen()->availableGeometry();
    owner.setGeometry(available.right() - 100, available.bottom() - 80, 100, 80);
    unrelated.setGeometry(available.left() + 50, available.top() + 100, 100, 80);
    owner.show();
    unrelated.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    QVERIFY(QTest::qWaitForWindowExposed(&unrelated));
    ModalityObserver ownerState;
    ModalityObserver unrelatedState;
    owner.windowHandle()->installEventFilter(&ownerState);
    unrelated.windowHandle()->installEventFilter(&unrelatedState);

    AdModal modal(&owner);
    modal.setMode(AdModal::Mode::Window);
    modal.setPreset(AdModal::Preset::Confirm);
    modal.setWindowTitle(QStringLiteral("Destroy pinned window"));
    modal.setText(QStringLiteral("Destroy this pinned window?"));
    modal.open();
    QWidget* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface);
    QVERIFY(QTest::qWaitForWindowExposed(surface));
    QVERIFY(ownerState.blocked);
    QVERIFY(!unrelatedState.blocked);
    QVERIFY(surface->isEnabled());
    QCOMPARE(modal.windowModality(), Qt::WindowModal);
    QCOMPARE(surface->windowModality(), Qt::NonModal);
    QVERIFY(available.contains(surface->frameGeometry()));
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
      NSWindow* window = reinterpret_cast<NSView*>(surface->winId()).window;
      NSWindow* nativeOwner = reinterpret_cast<NSView*>(owner.winId()).window;
      QVERIFY(!window.isSheet);
      QVERIFY(window.movable);
      QVERIFY(orderedAbove(window, nativeOwner));
      [nativeOwner makeKeyAndOrderFront:nil];
      QVERIFY(NSApp.keyWindow != nativeOwner);
      QVERIFY(orderedAbove(window, nativeOwner));
    }
    QTest::mouseClick(owner.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      ownerButton.geometry().center());
    QTest::mouseClick(unrelated.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      unrelatedButton.geometry().center());
    QCOMPARE(ownerClicks.count(), 0);
    QCOMPARE(unrelatedClicks.count(), 1);

    QWidget* panel = modalSection(surface, "ad-modal-panel");
    QVERIFY(panel);
    const QPoint before = surface->pos();
    const QPoint local(8, 8);
    const QPoint press = panel->mapToGlobal(local);
    const QPoint delta(-60, -40);
    QMouseEvent down(QEvent::MouseButtonPress, local, press, Qt::LeftButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(panel, &down);
    QMouseEvent move(QEvent::MouseMove, surface->mapFromGlobal(press + delta), press + delta,
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(surface, &move);
    QMouseEvent up(QEvent::MouseButtonRelease, surface->mapFromGlobal(press + delta), press + delta,
                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(surface, &up);
    qApp->processEvents();
    QCOMPARE(surface->pos(), before + delta);
    QVERIFY(ownerState.blocked);
    QTest::mouseClick(surface->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      modal.rejectButton()->mapTo(surface, modal.rejectButton()->rect().center()));
    QVERIFY(!modal.isOpen());
    QVERIFY(!ownerState.blocked);
    QVERIFY(QGuiApplication::modalWindow() == nullptr);
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
      QVERIFY(reinterpret_cast<NSView*>(surface->winId()).window.parentWindow == nil);
    }
    modal.open();
    QVERIFY(ownerState.blocked);
    QVERIFY(available.contains(surface->frameGeometry()));
    QTest::mouseClick(surface->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      modal.acceptButton()->mapTo(surface, modal.acceptButton()->rect().center()));
    QVERIFY(!modal.isOpen());
    QVERIFY(!ownerState.blocked);
    QVERIFY(!unrelatedState.blocked);
    QTest::mouseClick(owner.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      ownerButton.geometry().center());
    QCOMPARE(ownerClicks.count(), 1);
  }

  void macOwnerModalReleasesBlockingOnChangesAndDestruction() {
    QWidget owner;
    owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    ModalityObserver ownerState;
    owner.windowHandle()->installEventFilter(&ownerState);
    auto modal = std::make_unique<AdModal>(&owner);
    modal->setMode(AdModal::Mode::Window);
    modal->open();
    QVERIFY(ownerState.blocked);
    modal->setWindowModality(Qt::NonModal);
    QVERIFY(!ownerState.blocked);
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
      QWidget* surface = modal->acceptButton()->window();
      QVERIFY(reinterpret_cast<NSView*>(surface->winId()).window.parentWindow == nil);
    }
    modal->setWindowModality(Qt::WindowModal);
    QVERIFY(ownerState.blocked);
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
      QWidget* surface = modal->acceptButton()->window();
      QVERIFY(orderedAbove(reinterpret_cast<NSView*>(surface->winId()).window,
                           reinterpret_cast<NSView*>(owner.winId()).window));
    }
    modal.reset();
    QVERIFY(!ownerState.blocked);
    QVERIFY(QGuiApplication::modalWindow() == nullptr);
  }

  void macNestedOwnerModalsCloseThroughActiveModalWidget() {
    QWidget owner;
    owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    ModalityObserver ownerState;
    owner.windowHandle()->installEventFilter(&ownerState);
    AdModal outer(&owner);
    outer.setMode(AdModal::Mode::Window);
    outer.setWindowTitle(QStringLiteral("Outer modal"));
    outer.open();
    QWidget* outerSurface = visibleOverlaySurface(outer.windowTitle());
    QVERIFY(outerSurface);
    ModalityObserver outerState;
    outerSurface->windowHandle()->installEventFilter(&outerState);
    AdModal inner(outerSurface);
    inner.setMode(AdModal::Mode::Window);
    inner.setWindowTitle(QStringLiteral("Inner modal"));
    inner.open();
    QVERIFY(ownerState.blocked);
    QVERIFY(outerState.blocked);
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
      NSWindow* nativeOuter = reinterpret_cast<NSView*>(outerSurface->winId()).window;
      NSWindow* nativeInner =
          reinterpret_cast<NSView*>(inner.acceptButton()->window()->winId()).window;
      QVERIFY(orderedAbove(nativeInner, nativeOuter));
      NSWindow* nativeOwner = reinterpret_cast<NSView*>(owner.winId()).window;
      [nativeOwner orderFront:nil];
      QVERIFY(orderedAbove(nativeOuter, nativeOwner));
      QVERIFY(orderedAbove(nativeInner, nativeOuter));
      [nativeOwner orderWindow:NSWindowAbove relativeTo:nativeInner.windowNumber];
      [nativeInner orderBack:nil];
      QVERIFY(orderedAbove(nativeOuter, nativeOwner));
      QVERIFY(orderedAbove(nativeInner, nativeOuter));
      [nativeOuter makeKeyAndOrderFront:nil];
      QVERIFY(NSApp.keyWindow != nativeOuter);
    }
    QVERIFY(QApplication::activeModalWidget());
    QApplication::activeModalWidget()->close();
    QVERIFY(!inner.isOpen());
    QVERIFY(outer.isOpen());
    QVERIFY(ownerState.blocked);
    QVERIFY(!outerState.blocked);
    QVERIFY(QApplication::activeModalWidget());
    QApplication::activeModalWidget()->close();
    QVERIFY(!outer.isOpen());
    QVERIFY(!ownerState.blocked);
    QVERIFY(QApplication::activeModalWidget() == nullptr);
  }

  void macWindowChromeSurvivesSurfaceChanges() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowTitle(QStringLiteral("Native modal chrome"));
    const auto verifyChrome = [&] {
      QWidget* surface = visibleOverlaySurface(modal.windowTitle());
      QVERIFY(surface);
      QVERIFY(!surface->windowFlags().testFlag(Qt::FramelessWindowHint));
      QVERIFY(surface->windowFlags().testFlag(Qt::WindowTitleHint));
      QVERIFY(surface->windowFlags().testFlag(Qt::ExpandedClientAreaHint));
      QVERIFY(surface->windowFlags().testFlag(Qt::NoTitleBarBackgroundHint));
      QVERIFY(!surface->testAttribute(Qt::WA_TranslucentBackground));
      surface->layout()->activate();
      QWidget* panel = modalSection(surface, "ad-modal-panel");
      QVERIFY(panel);
      QCOMPARE(panel->mapTo(surface, QPoint()).y(), 0);
      if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        NSWindow* window = reinterpret_cast<NSView*>(surface->winId()).window;
        QVERIFY(window);
        QVERIFY(window.styleMask & NSWindowStyleMaskTitled);
        QVERIFY(window.styleMask & NSWindowStyleMaskFullSizeContentView);
        QVERIFY(window.hasShadow);
        QVERIFY(window.titlebarAppearsTransparent);
        QCOMPARE(window.titleVisibility, NSWindowTitleHidden);
        QVERIFY([window standardWindowButton:NSWindowCloseButton].hidden);
        QVERIFY([window standardWindowButton:NSWindowMiniaturizeButton].hidden);
        QVERIFY([window standardWindowButton:NSWindowZoomButton].hidden);
      }
    };
    modal.open();
    verifyChrome();
    modal.setWindowAlwaysOnTop(true);
    verifyChrome();
    modal.setWindowTaskbarVisible(true);
    verifyChrome();
    modal.setWindowTitle(QStringLiteral("Updated native title"));
    verifyChrome();
    modal.setWindowAlwaysOnTop(false);
    modal.setWindowTaskbarVisible(false);
    verifyChrome();
    modal.setWindowResizable(true);
    verifyChrome();
    modal.close();
    modal.open();
    verifyChrome();
    modal.close();
  }
#endif

  void serviceShowInfoWithoutOwnerShowsDialog() {
    QPointer<AdModal> modal;
    {
      AdModal* opened = adqt::widgets::AdModalService::showInfo(
          {.mode = AdModal::Mode::Window, .text = QStringLiteral("Tray info")}, nullptr);
      QVERIFY(opened != nullptr);
      modal = opened;
    }
    qApp->processEvents();
    QVERIFY(modal != nullptr);
    QVERIFY(modal->isOpen());
    QVERIFY2(visibleOverlaySurface(), "service modal without owner has no visible surface");
    modal->close();
    QVERIFY(!modal->isOpen());
  }

  void resizableWindowRetainsGeometryAndReopensAtDefault() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowScreen(qApp->primaryScreen());
    modal.setWindowPreferredSize(QSize(960, 640));
    modal.setWindowMinimumSize(QSize(640, 480));
    modal.setWindowResizable(true);
    modal.setCentered(true);
    modal.setFooterVisible(false);
    modal.setWindowTitle(QStringLiteral("Resizable translation"));
    auto* content = new QLabel(QStringLiteral("Content"));
    modal.setContentWidget(content);
    modal.present();
    auto* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface);
    const QRect available = qApp->primaryScreen()->availableGeometry().adjusted(16, 16, -16, -16);
    const QSize initial = QSize(960, 640).boundedTo(available.size());
    QCOMPARE(surface->size(), initial);
    QCOMPARE(surface->minimumSize(), QSize(640, 480).boundedTo(available.size()));
    QVERIFY((surface->geometry().center() - available.center()).manhattanLength() <= 2);
    QCOMPARE(content->window(), surface);
    surface->resize(initial - QSize(40, 40));
    surface->move(surface->pos() + QPoint(11, 7));
    const QRect changed = surface->geometry();
    modal.setWindowTitle(QStringLiteral("Updated title"));
    content->setText(QStringLiteral("More content that must not reset window geometry"));
    AdModal::ComponentTokens tokens;
    tokens.contentBg = QColor(Qt::darkGray);
    modal.setComponentTokens(tokens);
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(surface, &languageChange);
    qApp->processEvents();
    modal.present();
    QCOMPARE(surface->geometry(), changed);
    surface->showMinimized();
    modal.present();
    QVERIFY(!surface->isMinimized());
    QCOMPARE(surface->geometry(), changed);
    modal.close();
    modal.present();
    QCOMPARE(content->window(), surface);
    QCOMPARE(surface->size(), initial);
    QVERIFY((surface->geometry().center() - available.center()).manhattanLength() <= 2);
    modal.close();
  }

  // Component tokens must pad only the modal's content area: the content
  // widget tracks the token, while the header section stays exactly where the
  // theme insets place it.
  void contentPaddingTokensPadOnlyTheContentArea() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowScreen(qApp->primaryScreen());
    modal.setWindowPreferredSize(QSize(480, 360));
    modal.setWindowResizable(true);
    modal.setFooterVisible(false);
    modal.setWindowTitle(QStringLiteral("Content padding"));
    auto* content = new QLabel(QStringLiteral("Content"));
    modal.setContentWidget(content);
    modal.open();
    QWidget* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface != nullptr);
    auto* header = surface->findChild<QWidget*>(QStringLiteral("ad-modal-header"));
    QVERIFY(header != nullptr);

    AdModal::ComponentTokens tokens;
    tokens.bodyPaddingHorizontal = 0;
    tokens.bodyPaddingVertical = 0;
    tokens.contentPaddingHorizontal = 30;
    tokens.contentPaddingVertical = 26;
    modal.setComponentTokens(tokens);
    qApp->processEvents();
    const QPoint headerWithLargePadding = header->mapTo(surface, QPoint(0, 0));
    const QPoint contentWithLargePadding = content->mapTo(surface, QPoint(0, 0));

    tokens.contentPaddingHorizontal = 7;
    tokens.contentPaddingVertical = 5;
    modal.setComponentTokens(tokens);
    qApp->processEvents();
    const QPoint contentWithSmallPadding = content->mapTo(surface, QPoint(0, 0));

    QCOMPARE(contentWithLargePadding.x() - contentWithSmallPadding.x(), 23);
    QCOMPARE(contentWithLargePadding.y() - contentWithSmallPadding.y(), 21);
    QCOMPARE(contentWithSmallPadding.x(), 7);
    // The header is the first panel item, so its position follows only the
    // theme insets and must not move when the content area is re-padded.
    QCOMPARE(header->mapTo(surface, QPoint(0, 0)), headerWithLargePadding);
    modal.close();
  }

  // The header bottom margin token gaps the header and the content area:
  // the content moves by the token while the header itself stays put. A
  // non-resizable surface keeps sections at their hint sizes, so the margin
  // is not absorbed by extra-space redistribution.
  void headerMarginBottomTokenGapsHeaderAndContent() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowScreen(qApp->primaryScreen());
    modal.setWindowPreferredSize(QSize(480, 360));
    modal.setFooterVisible(false);
    modal.setWindowTitle(QStringLiteral("Header margin"));
    auto* content = new QLabel(QStringLiteral("Content"));
    modal.setContentWidget(content);
    modal.open();
    QWidget* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface != nullptr);
    auto* header = surface->findChild<QWidget*>(QStringLiteral("ad-modal-header"));
    QVERIFY(header != nullptr);

    AdModal::ComponentTokens tokens;
    tokens.bodyPaddingHorizontal = 0;
    tokens.bodyPaddingVertical = 0;
    tokens.contentPaddingHorizontal = 7;
    tokens.contentPaddingVertical = 5;
    tokens.headerPaddingVertical = 0;
    tokens.headerMarginBottom = 12;
    modal.setComponentTokens(tokens);
    qApp->processEvents();
    const QPoint contentWithGap = content->mapTo(surface, QPoint(0, 0));
    const QMargins headerWithGapMargins = header->layout()->contentsMargins();

    tokens.headerMarginBottom = 0;
    modal.setComponentTokens(tokens);
    qApp->processEvents();
    const QPoint contentWithoutGap = content->mapTo(surface, QPoint(0, 0));
    const QMargins headerMarginsWithoutGap = header->layout()->contentsMargins();

    // The margin lands in the header layout's bottom edge; distribution of
    // leftover space may absorb geometry shifts, so assert the applied
    // margins rather than absolute content geometry.
    QCOMPARE(headerWithGapMargins.bottom() - headerMarginsWithoutGap.bottom(), 12);
    QCOMPARE(headerMarginsWithoutGap.bottom(), 0);
    QCOMPARE(headerMarginsWithoutGap.top(), headerWithGapMargins.top());
    QVERIFY(contentWithoutGap.y() <= contentWithGap.y());
    QCOMPARE(contentWithoutGap.x(), contentWithGap.x());
    modal.close();
  }

  // The panel's outer vertical inset lives on the header top and footer
  // bottom. Confirm dialogs hide that header (window-mode Confirm, or any
  // confirm-like preset with the close button off), so the body must inherit
  // the same inset instead of sitting flush against the panel edge.
  void hiddenEdgeSectionsInheritOuterVerticalInset() {
    AdModal reference;
    reference.setMode(AdModal::Mode::Window);
    reference.setWindowTitle(QStringLiteral("Reference"));
    reference.setText(QStringLiteral("Body"));
    reference.open();
    QWidget* referenceSurface = visibleOverlaySurface(QStringLiteral("Reference"));
    QVERIFY(referenceSurface != nullptr);
    auto* referenceHeader = modalSection(referenceSurface, "ad-modal-header");
    auto* referenceBody = modalSection(referenceSurface, "ad-modal-body");
    auto* referenceFooter = modalSection(referenceSurface, "ad-modal-footer");
    QVERIFY(referenceHeader != nullptr && !referenceHeader->isHidden());
    QVERIFY(referenceBody != nullptr);
    QVERIFY(referenceFooter != nullptr && !referenceFooter->isHidden());
    const int outerTop = sectionMargins(referenceSurface, "ad-modal-header").top();
    const int outerBottom = sectionMargins(referenceSurface, "ad-modal-footer").bottom();
    QVERIFY(outerTop > 0);
    QVERIFY(outerBottom > 0);
    QCOMPARE(sectionMargins(referenceSurface, "ad-modal-body").top(), 0);
    QCOMPARE(sectionMargins(referenceSurface, "ad-modal-body").bottom(), 0);
    reference.close();

    AdModal confirm;
    confirm.setMode(AdModal::Mode::Window);
    confirm.setPreset(AdModal::Preset::Confirm);
    confirm.setWindowTitle(QStringLiteral("Delete preset"));
    confirm.setText(QStringLiteral("Delete preset \"1049 x 700\"? This action cannot be undone"));
    confirm.open();
    QWidget* confirmSurface = visibleOverlaySurface(QStringLiteral("Delete preset"));
    QVERIFY(confirmSurface != nullptr);
    auto* confirmHeader = modalSection(confirmSurface, "ad-modal-header");
    auto* confirmBody = modalSection(confirmSurface, "ad-modal-body");
    auto* confirmFooter = modalSection(confirmSurface, "ad-modal-footer");
    auto* confirmPanel = modalSection(confirmSurface, "ad-modal-panel");
    auto* confirmIcon = modalSection(confirmSurface, "ad-modal-title-icon");
    QVERIFY(confirmHeader != nullptr && confirmHeader->isHidden());
    QVERIFY(confirmBody != nullptr);
    QVERIFY(confirmFooter != nullptr && !confirmFooter->isHidden());
    QVERIFY(confirmPanel != nullptr);
    QVERIFY(confirmIcon != nullptr);
    QCOMPARE(sectionMargins(confirmSurface, "ad-modal-body").top(), outerTop);
    QCOMPARE(sectionMargins(confirmSurface, "ad-modal-body").bottom(), 0);
    QCOMPARE(confirmIcon->mapTo(confirmPanel, QPoint(0, 0)).y(), outerTop);

    confirm.setFooterVisible(false);
    QVERIFY(confirmFooter->isHidden());
    QCOMPARE(sectionMargins(confirmSurface, "ad-modal-body").top(), outerTop);
    QCOMPARE(sectionMargins(confirmSurface, "ad-modal-body").bottom(), outerBottom);
    confirm.close();

    QWidget owner;
    owner.resize(800, 600);
    owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));

    AdModal overlayConfirm(&owner);
    overlayConfirm.setOwnerWindow(&owner);
    overlayConfirm.setPreset(AdModal::Preset::Confirm);
    overlayConfirm.setCloseButtonVisible(false);
    overlayConfirm.setWindowTitle(QStringLiteral("Overlay confirm"));
    overlayConfirm.setText(QStringLiteral("This action cannot be undone"));
    overlayConfirm.open();
    QWidget* overlaySurface = owner.findChild<QWidget*>(QString::fromLatin1(kOverlayObjectName));
    QVERIFY(overlaySurface != nullptr);
    auto* overlayHeader = modalSection(overlaySurface, "ad-modal-header");
    QVERIFY(overlayHeader != nullptr && overlayHeader->isHidden());
    QCOMPARE(sectionMargins(overlaySurface, "ad-modal-body").top(), outerTop);
    QCOMPARE(sectionMargins(overlaySurface, "ad-modal-body").bottom(), 0);
    overlayConfirm.close();
  }

  // Taskbar visibility swaps the detached Qt::Tool surface for a plain
  // Qt::Window, and the extra chrome buttons drive minimize and always-on-top
  // without disturbing the window geometry.
  void windowChromeButtonsAndTaskbarSurface() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowScreen(qApp->primaryScreen());
    modal.setWindowPreferredSize(QSize(480, 360));
    modal.setWindowResizable(true);
    modal.setWindowTaskbarVisible(true);
    modal.setWindowMinimizeButtonVisible(true);
    modal.setWindowAlwaysOnTopButtonVisible(true);
    modal.setWindowTitle(QStringLiteral("Chrome"));
    auto* content = new QLabel(QStringLiteral("Content"));
    modal.setContentWidget(content);
    modal.open();
    QWidget* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(surface));

    QCOMPARE(surface->windowFlags() & Qt::WindowType_Mask, Qt::Window);
    auto* minimize = surface->findChild<QToolButton*>(QStringLiteral("ad-modal-minimize"));
    auto* pin = surface->findChild<QToolButton*>(QStringLiteral("ad-modal-always-on-top"));
    QVERIFY(minimize != nullptr);
    QVERIFY(pin != nullptr);
    QVERIFY(minimize->isVisible());
    QVERIFY(pin->isVisible());
    QVERIFY(!modal.windowAlwaysOnTop());
    QVERIFY(!pin->isChecked());

    const QRect geometry = surface->geometry();
    pin->click();
    QVERIFY(modal.windowAlwaysOnTop());
    QVERIFY(pin->isChecked());
    QVERIFY(surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QCOMPARE(surface->geometry(), geometry);

    minimize->click();
    QTRY_VERIFY(surface->isMinimized());
    modal.present();
    QTRY_VERIFY(!surface->isMinimized());
    QCOMPARE(surface->geometry(), geometry);
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
      const auto hwnd = reinterpret_cast<HWND>(surface->winId());
      QVERIFY((GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    }
#endif

    pin->click();
    QVERIFY(!modal.windowAlwaysOnTop());
    QVERIFY(!pin->isChecked());
    QVERIFY(!surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QCOMPARE(surface->geometry(), geometry);

    // Toggling after the surface exists must keep the taskbar surface type.
    modal.setWindowTaskbarVisible(false);
    QCOMPARE(surface->windowFlags() & Qt::WindowType_Mask, Qt::Tool);
    modal.setWindowTaskbarVisible(true);
    QCOMPARE(surface->windowFlags() & Qt::WindowType_Mask, Qt::Window);
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
      const auto hwnd = reinterpret_cast<HWND>(surface->winId());
      const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
      QCOMPARE(style & (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME),
               LONG_PTR(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME));
      QCOMPARE(GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED, LONG_PTR(0));
    }
#endif
    modal.close();
  }

  void alwaysOnTopPreservesSurfaceAndNativeChrome() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowTaskbarVisible(true);
    modal.setWindowTitle(QStringLiteral("Stable stacking"));
    modal.open();
    QWidget* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface);
    QVERIFY(QTest::qWaitForWindowExposed(surface));
    const WId id = surface->winId();
    const QRect geometry = surface->geometry();
    SurfaceLifecycleObserver observer;
    surface->installEventFilter(&observer);
    for (bool pinned : {true, false, true, false}) {
      modal.setWindowAlwaysOnTop(pinned);
      QCoreApplication::processEvents();
      QCOMPARE(observer.disruptions, 0);
      QCOMPARE(surface->winId(), id);
      QCOMPARE(surface->geometry(), geometry);
      QVERIFY(surface->isVisible());
      QCOMPARE(surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint), pinned);
#ifdef Q_OS_WIN
      if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const auto hwnd = reinterpret_cast<HWND>(id);
        const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        QCOMPARE((exStyle & WS_EX_TOPMOST) != 0, pinned);
        QCOMPARE(style & (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME),
                 LONG_PTR(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME));
        QCOMPARE(exStyle & WS_EX_LAYERED, LONG_PTR(0));
      }
#endif
    }
    surface->removeEventFilter(&observer);
    modal.close();
  }

  void explicitScreenCentersOnEachAvailableDisplay() {
    for (QScreen* screen : qApp->screens()) {
      qInfo() << "Modal display:" << screen->name() << screen->availableGeometry()
              << "DPR:" << screen->devicePixelRatio();
      AdModal modal;
      modal.setMode(AdModal::Mode::Window);
      modal.setWindowModeDetached(true);
      modal.setWindowModality(Qt::NonModal);
      modal.setWindowScreen(screen);
      modal.setWindowPreferredSize(QSize(960, 640));
      modal.setWindowMinimumSize(QSize(640, 480));
      modal.setWindowResizable(true);
      modal.setCentered(true);
      modal.open();
      auto* surface = visibleOverlaySurface();
      QVERIFY(surface);
      QVERIFY(QTest::qWaitForWindowExposed(surface));
      QTRY_COMPARE(surface->screen(), screen);
      const QRect available = screen->availableGeometry().adjusted(16, 16, -16, -16);
      QCOMPARE(surface->size(), QSize(960, 640).boundedTo(available.size()));
      QVERIFY((surface->geometry().center() - available.center()).manhattanLength() <= 2);
      modal.close();
    }
  }

  void explicitGeometryClampsOversizedMinimumToScreen() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowScreen(qApp->primaryScreen());
    modal.setWindowPreferredSize(QSize(100000, 100000));
    modal.setWindowMinimumSize(QSize(90000, 90000));
    modal.setWindowResizable(true);
    modal.setCentered(true);
    modal.open();
    auto* surface = visibleOverlaySurface();
    QVERIFY(surface);
    const QRect available = qApp->primaryScreen()->availableGeometry().adjusted(16, 16, -16, -16);
    QCOMPARE(surface->size(), available.size());
    QCOMPARE(surface->minimumSize(), available.size());
    QVERIFY(available.contains(surface->geometry()));
    modal.close();
  }

  void detachedWindowDoesNotAcquireAmbientOwner() {
    QWidget owner;
    owner.show();
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.open();
    auto* surface = visibleOverlaySurface();
    QVERIFY(surface);
    QCOMPARE(modal.ownerWindow(), nullptr);
    QCOMPARE(surface->parentWidget(), nullptr);
    owner.hide();
    QVERIFY(modal.isOpen());
    modal.close();
  }

  // Control case: with a visible owner the dialog remains anchored to it.
  void windowModeWithOwnerStillCentersOnOwner() {
    QWidget owner;
    owner.setObjectName(QStringLiteral("tst-modal-owner"));
    owner.setGeometry(200, 200, 400, 300);
    owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));

    AdModal modal(&owner);
    modal.setMode(AdModal::Mode::Window);
    modal.setCentered(true);
    modal.setWindowTitle(QStringLiteral("With owner"));
    modal.open();
    QVERIFY(modal.isOpen());

    QWidget* surface = visibleOverlaySurface(QStringLiteral("With owner"));
    QVERIFY(surface != nullptr);
    QCOMPARE(surface->parentWidget(), &owner);

    modal.close();
    QVERIFY(!modal.isOpen());
  }
};

}  // namespace

QTEST_MAIN(TstModalWindow)
#include "tst_modal_window.moc"
