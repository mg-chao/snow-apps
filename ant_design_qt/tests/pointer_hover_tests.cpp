#include "widgets/detail/overlay_popup_controller.h"
#include "widgets/detail/overlay_popup_surface.h"
#include "widgets/detail/pointer_region.h"
#include "widgets/detail/timing_hub.h"
#include "widgets/checkbox.h"
#include "widgets/radio.h"
#include "widgets/switch.h"
#include "widgets/tag.h"
#include "widgets/select.h"
#include "widgets/navigation_menu.h"
#include <QAbstractItemView>
#include <QImage>
#include <QStandardItemModel>

#include <QApplication>
#include <QFocusEvent>
#include <QTest>
#include <QWheelEvent>

#include <cstdlib>
#include <iostream>

using namespace adqt::widgets::detail;

namespace {
void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct Fixture final : OverlayPopupControllerDelegate {
  QWidget window;
  QWidget trigger{&window};
  QWidget child{&trigger};
  QWidget outside{&window};
  QWidget popup{&window};
  std::unique_ptr<OverlayPopupSurface> shadowSurface;
  QWidget* surface = &popup;
  adqt::widgets::AdPopupLayerMode layerMode = adqt::widgets::AdPopupLayerMode::InWindow;
  QPoint cursor;
  QWidget* target = &trigger;
  int cursorReads = 0;
  QPointer<QWidget> releasedContent;
  OverlayPopupController controller{this, nullptr,
                                    [this] {
                                      ++cursorReads;
                                      return cursor;
                                    },
                                    [this](const QPoint&) { return target; }};

  Fixture() {
    window.resize(500, 300);
    trigger.setGeometry(10, 10, 100, 40);
    child.setGeometry(10, 10, 20, 20);
    outside.setGeometry(300, 200, 50, 50);
    popup.setFixedSize(120, 60);
    popup.hide();
    window.show();
    QCoreApplication::processEvents();
    cursor = trigger.mapToGlobal(QPoint(5, 5));
    controller.anchorWidgetChanged();
    controller.setMouseEnterDelayMs(0);
    controller.setMouseLeaveDelayMs(0);
  }
  QObject* popupOwnerObject() const override { return const_cast<QWidget*>(&trigger); }
  QWidget* popupAnchorWidget() const override { return const_cast<QWidget*>(&trigger); }
  QWidget* popupScopeWindow() const override { return const_cast<QWidget*>(&window); }
  QWidget* popupSurfaceWidget() const override { return surface; }
  QWidget* popupEnsureSurface() override { return surface; }
  void popupPrepareToShow() override {}
  bool popupReleaseOnHide() const override { return !releasedContent.isNull(); }
  void popupReleaseSurface() override {
    QWidget* released = releasedContent.data();
    if (released == surface) surface = nullptr;
    delete released;
  }
  bool popupHasContent() const override { return true; }
  OverlayPopupPlacement popupPlacement() const override { return OverlayPopupPlacement::Bottom; }
  adqt::widgets::AdPopupLayerMode popupLayerMode() const override { return layerMode; }
  bool popupAutoAdjustOverflow() const override { return false; }
  bool popupArrowVisible() const override { return false; }
  bool popupArrowPointAtCenter() const override { return false; }
  int popupOffset() const override { return 4; }
  int popupArrowOffsetHorizontal() const override { return 0; }
  int popupArrowOffsetVertical() const override { return 0; }
  void popupApplyResolvedPlacement(OverlayPopupPlacement, qreal) override {}

  void enter(QWidget& receiver) {
    const QPoint local(5, 5);
    QEnterEvent event(local, receiver.mapTo(&window, local), receiver.mapToGlobal(local));
    QApplication::sendEvent(&receiver, &event);
  }
  void move(QWidget& receiver) {
    const QPoint local(5, 5);
    QMouseEvent event(QEvent::MouseMove, local, receiver.mapToGlobal(local), Qt::NoButton,
                      Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&receiver, &event);
  }
};

struct WheelTarget final : QWidget {
  using QWidget::QWidget;

  int wheelCount = 0;
  QPoint lastAngleDelta;

 protected:
  void wheelEvent(QWheelEvent* event) override {
    ++wheelCount;
    lastAngleDelta = event->angleDelta();
    event->accept();
  }
};

void eventPositionOwnsImmediateTransitions() {
  Fixture f;
  f.controller.setVisibilityMode(OverlayPopupController::VisibilityMode::External);
  int opens = 0;
  QObject::connect(&f.controller, &OverlayPopupController::popupVisibilityRequested,
                   [&opens](bool visible) {
                     if (visible) ++opens;
                   });
  f.cursor = f.outside.mapToGlobal(QPoint(5, 5));
  f.target = &f.outside;
  f.enter(f.trigger);
  require(opens == 1, "immediate enter must use event coordinates, not the live cursor");
  require(f.cursorReads == 0, "an immediate enter must not sample the cursor");
  f.enter(f.child);
  require(opens == 1, "entering a descendant must not restart the session");
  const QPoint childPosition = f.child.mapToGlobal(QPoint(5, 5));
  QMouseEvent propagated(QEvent::MouseMove, f.window.mapFromGlobal(childPosition), childPosition,
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&f.window, &propagated);
  f.enter(f.child);
  require(opens == 1, "an event propagated to an ancestor must preserve the child hover session");
  f.move(f.outside);
  f.enter(f.trigger);
  require(opens == 2, "an outside event must end the previous session");
}

void deadlinesValidateTargetAndStopWhenIdle() {
  Fixture f;
  f.controller.setVisibilityMode(OverlayPopupController::VisibilityMode::External);
  f.controller.setMouseEnterDelayMs(10);
  int opens = 0;
  QObject::connect(&f.controller, &OverlayPopupController::popupVisibilityRequested,
                   [&opens](bool visible) {
                     if (visible) ++opens;
                   });
  f.enter(f.trigger);
  f.target = &f.outside;  // Occlusion: same coordinate, different input target.
  QTest::qWait(40);
  require(opens == 0, "covered triggers must not open when the deadline fires");
  f.target = &f.trigger;
  f.enter(f.trigger);
  QTest::qWait(40);
  require(opens == 1, "a new eligible hover must recover after rejection");
  const int reads = f.cursorReads;
  QTest::qWait(90);
  require(f.cursorReads == reads, "an idle hover session must not poll the cursor");
  f.trigger.hide();
  f.trigger.show();
  f.enter(f.trigger);
  f.controller.setDisabled(true);
  QTest::qWait(40);
  require(opens == 1, "disabling must cancel pending transitions");
}

void tooltipWindowCannotStealAnEstablishedHoverTarget() {
  const auto opensAfterDeliveredHover = [](bool tooltipTarget, bool moveCursor) {
    Fixture f;
    f.controller.setVisibilityMode(OverlayPopupController::VisibilityMode::External);
    f.controller.setMouseEnterDelayMs(10);
    int opens = 0;
    QObject::connect(&f.controller, &OverlayPopupController::popupVisibilityRequested,
                     [&opens](bool visible) {
                       if (visible) ++opens;
                     });
    QWidget tooltip(nullptr, Qt::ToolTip | Qt::WindowTransparentForInput);
    tooltip.setGeometry(QRect(f.trigger.mapToGlobal(QPoint()), f.trigger.size()));
    tooltip.show();
    f.target = tooltipTarget ? &tooltip : nullptr;
    if (moveCursor) f.cursor = f.outside.mapToGlobal(QPoint(5, 5));
    f.enter(f.trigger);
    QTest::qWait(40);
    return opens;
  };
  require(opensAfterDeliveredHover(true, false) == 1,
          "an input-transparent tooltip must not veto a delivered trigger hover");
  require(opensAfterDeliveredHover(false, false) == 1,
          "a missing widgetAt result must not discard unchanged delivered input");
  require(opensAfterDeliveredHover(false, true) == 0,
          "a moved cursor must invalidate the delivered hover target");

  Fixture covered;
  covered.controller.setVisibilityMode(OverlayPopupController::VisibilityMode::External);
  covered.controller.setMouseEnterDelayMs(10);
  int coveredOpens = 0;
  QObject::connect(&covered.controller, &OverlayPopupController::popupVisibilityRequested,
                   [&coveredOpens](bool visible) {
                     if (visible) ++coveredOpens;
                   });
  QWidget blocker(&covered.window);
  blocker.setGeometry(covered.trigger.geometry());
  blocker.show();
  blocker.raise();
  covered.target = nullptr;
  covered.enter(covered.trigger);
  QTest::qWait(40);
  require(coveredOpens == 0,
          "a missing widgetAt result must not bypass an opaque sibling over the trigger");
}

void independentReasonsAndPopupTravel() {
  Fixture f;
  f.enter(f.trigger);
  require(f.controller.popupVisible(), "hover must open the popup");
  f.controller.popupSurfaceChanged();
  f.enter(f.popup);
  require(f.controller.popupVisible(), "entering popup content must preserve the session");
  f.controller.setPopupVisible(true);
  f.move(f.outside);
  require(f.controller.popupVisible(), "hover exit must preserve a programmatic open reason");
  f.controller.setPopupVisible(false);
  require(!f.controller.popupVisible(), "explicit dismissal must close the popup");
}

void popupShadowOverTriggerPreservesHover() {
  for (const auto layer :
       {adqt::widgets::AdPopupLayerMode::QtTool, adqt::widgets::AdPopupLayerMode::InWindow}) {
    Fixture f;
    f.shadowSurface = std::make_unique<OverlayPopupSurface>(
        layer == adqt::widgets::AdPopupLayerMode::InWindow ? &f.window : nullptr);
    auto& surface = *f.shadowSurface;
    f.layerMode = layer;
    if (layer == adqt::widgets::AdPopupLayerMode::QtTool)
      surface.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    surface.setArrowVisible(false);
    surface.setFixedSize(160, 100);
    f.surface = &surface;
    f.controller.popupSurfaceChanged();
    f.cursor = f.trigger.mapToGlobal(QPoint(50, 39));
    f.enter(f.trigger);
    require(f.controller.popupVisible(), "trigger hover must open the shadowed popup");
    require(surface.isWindow() == (layer == adqt::widgets::AdPopupLayerMode::QtTool),
            "fixture surface must match its popup layer mode");
    require(surface.rect().contains(surface.mapFromGlobal(f.cursor)) &&
                !surface.containsInteractiveGlobalPos(f.cursor),
            "popup shadow must overlap the trigger edge without interactive content");

    // widgetAt sees the rectangular Qt window, although Windows native hit testing
    // returns HTTRANSPARENT for these shadow pixels.
    f.target = &surface;
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&f.trigger, &leave);
    QTest::qWait(40);
    require(f.controller.popupVisible(),
            "reconciliation must not close a popup whose own shadow covers its trigger");

    const auto moveAt = [&](const QPoint& global) {
      QMouseEvent move(QEvent::MouseMove, surface.mapFromGlobal(global), global, Qt::NoButton,
                       Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(&surface, &move);
    };
    moveAt(surface.mapToGlobal(surface.rect().center()));
    require(f.controller.popupVisible(), "popup body must retain hover");
    moveAt(f.cursor);
    require(f.controller.popupVisible(),
            "returning through the shadow to the trigger must stay open");

    QWidget cover(&f.window);
    cover.setGeometry(f.trigger.geometry());
    cover.show();
    cover.raise();
    moveAt(f.cursor);
    require(!f.controller.popupVisible(),
            "shadow hit testing must still respect trigger occlusion");
    cover.hide();
    f.enter(f.trigger);
    require(f.controller.popupVisible(), "uncovered trigger must reopen");
    const QPoint outsideShadow = surface.mapToGlobal(surface.rect().bottomRight() - QPoint(1, 1));
    require(!surface.containsInteractiveGlobalPos(outsideShadow), "fixture must hit only shadow");
    moveAt(outsideShadow);
    require(!f.controller.popupVisible(), "shadow outside the trigger must still close hover");
  }
}

void embeddedPopupShadowForwardsWheelAfterClosing() {
  Fixture f;
  WheelTarget underlying(&f.window);
  underlying.setGeometry(f.window.rect());
  underlying.show();
  underlying.lower();

  auto* surface = new OverlayPopupSurface(&f.window);
  surface->setArrowVisible(false);
  surface->setFixedSize(160, 100);
  f.surface = surface;
  f.releasedContent = surface;
  f.controller.popupSurfaceChanged();
  f.enter(f.trigger);
  require(f.controller.popupVisible() && surface->isVisible() && !surface->isWindow(),
          "fixture must open an embedded popup that releases its surface on close");

  const QPoint body = surface->mapToGlobal(surface->rect().center());
  require(surface->containsInteractiveGlobalPos(body), "fixture must identify popup body");
  QWheelEvent bodyWheel(surface->mapFromGlobal(body), body, QPoint(), QPoint(0, 120), Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(surface, &bodyWheel);
  require(underlying.wheelCount == 0 && f.controller.popupVisible(),
          "wheel input on popup content must stay with the popup");

  const QPoint shadow = surface->mapToGlobal(surface->rect().bottomRight() - QPoint(1, 1));
  require(!surface->containsInteractiveGlobalPos(shadow) &&
              underlying.rect().contains(underlying.mapFromGlobal(shadow)),
          "fixture must place an underlying wheel target behind popup shadow");
  QWheelEvent shadowWheel(surface->mapFromGlobal(shadow), shadow, QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(surface, &shadowWheel);
  require(underlying.wheelCount == 1 && underlying.lastAngleDelta == QPoint(0, -120),
          "wheel input on embedded popup shadow must reach the underlying target once");
  require(!f.controller.popupVisible() && f.releasedContent.isNull(),
          "forwarding must remain safe when closing releases the event receiver");
}

void gapTravelAndRapidReentryPreserveSession() {
  Fixture f;
  f.controller.setMouseLeaveDelayMs(50);
  f.enter(f.trigger);
  require(f.controller.popupVisible(), "hover must open before gap traversal");
  f.controller.popupSurfaceChanged();
  f.cursor = f.outside.mapToGlobal(QPoint(5, 5));
  f.target = &f.outside;
  f.move(f.outside);
  require(f.controller.popupVisible(), "crossing a gap must honor the close delay");
  f.cursor = f.popup.mapToGlobal(QPoint(5, 5));
  f.target = &f.popup;
  f.enter(f.popup);
  QTest::qWait(80);
  require(f.controller.popupVisible(), "entering content must cancel the obsolete close deadline");
  f.cursor = f.outside.mapToGlobal(QPoint(5, 5));
  f.target = &f.outside;
  f.move(f.outside);
  f.cursor = f.trigger.mapToGlobal(QPoint(5, 5));
  f.target = &f.trigger;
  f.enter(f.trigger);
  QTest::qWait(80);
  require(f.controller.popupVisible(), "rapid trigger reentry must cancel closure");
  f.cursor = f.outside.mapToGlobal(QPoint(5, 5));
  f.target = &f.outside;
  f.move(f.outside);
  QTest::qWait(80);
  require(!f.controller.popupVisible(), "an eligible outside deadline must close hover content");
}

void focusAndClickSurviveHoverExit() {
  for (const auto reason :
       {OverlayPopupController::Trigger::Focus, OverlayPopupController::Trigger::Click}) {
    Fixture f;
    f.controller.setTriggerModes(OverlayPopupController::Trigger::Hover | reason);
    f.enter(f.trigger);
    if (reason == OverlayPopupController::Trigger::Focus) {
      QFocusEvent focus(QEvent::FocusIn, Qt::TabFocusReason);
      QApplication::sendEvent(&f.trigger, &focus);
    } else {
      const QPoint local(5, 5);
      const QPoint global = f.trigger.mapToGlobal(local);
      QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                        Qt::NoModifier);
      QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                          Qt::NoModifier);
      QApplication::sendEvent(&f.trigger, &press);
      QApplication::sendEvent(&f.trigger, &release);
    }
    f.move(f.outside);
    require(f.controller.popupVisible(), "hover exit must preserve focus and click open reasons");
    f.controller.setPopupVisible(false);
    require(!f.controller.popupVisible(), "explicit dismissal must clear independent reasons");
  }
}

void externalAcknowledgementPreservesEventSession() {
  Fixture f;
  f.controller.setVisibilityMode(OverlayPopupController::VisibilityMode::External);
  QObject::connect(&f.controller, &OverlayPopupController::popupVisibilityRequested, &f.controller,
                   &OverlayPopupController::setPopupVisible);
  // The owner acknowledges the event synchronously, after live input moved on.
  f.cursor = f.outside.mapToGlobal(QPoint(5, 5));
  f.target = &f.outside;
  f.enter(f.trigger);
  require(f.controller.popupVisible(), "manual owner must acknowledge the hover open request");
  f.move(f.outside);
  require(!f.controller.popupVisible(),
          "acknowledging visibility must preserve the session that will request hover closure");
}

void geometryBurstsDoNotPostponeReconciliation() {
  Fixture f;
  f.enter(f.trigger);
  f.cursor = f.outside.mapToGlobal(QPoint(5, 5));
  f.target = &f.outside;
  int geometryChanges = 0;
  std::function<void()> changeGeometry;
  changeGeometry = [&] {
    if (++geometryChanges == 5) {
      require(!f.controller.popupVisible(),
              "geometry activity must not continually replace an already queued hover check");
      return;
    }
    // Native popup relayout can run before hover reconciliation in the same
    // timing-hub batch, raising the surface and invalidating its geometry again.
    deferTimingTask(&f.window, QStringLiteral("geometry-burst"), changeGeometry);
    QEvent changed(QEvent::ZOrderChange);
    QApplication::sendEvent(&f.popup, &changed);
  };
  deferTimingTask(&f.window, QStringLiteral("geometry-burst"), changeGeometry);
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&f.trigger, &leave);
  QTest::qWait(40);
  require(geometryChanges == 5, "all queued geometry changes must be dispatched");
}

void closingDuringDispatchCanDestroyTheReceiver() {
  Fixture f;
  f.enter(f.trigger);
  f.releasedContent = new QWidget(&f.popup);
  f.releasedContent->setGeometry(0, 0, 20, 20);
  f.releasedContent->show();
  QWidget* receiver = f.releasedContent;
  const QPoint global = f.outside.mapToGlobal(QPoint(5, 5));
  QMouseEvent move(QEvent::MouseMove, receiver->mapFromGlobal(global), global, Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(receiver, &move);
  require(f.releasedContent.isNull() && !f.controller.popupVisible(),
          "closing while dispatching must safely release transient content");
}

void destructionCancelsPendingWork() {
  int opens = 0;
  {
    Fixture f;
    f.controller.setVisibilityMode(OverlayPopupController::VisibilityMode::External);
    f.controller.setMouseEnterDelayMs(30);
    QObject::connect(&f.controller, &OverlayPopupController::popupVisibilityRequested,
                     [&opens](bool visible) {
                       if (visible) ++opens;
                     });
    f.enter(f.trigger);
  }
  QTest::qWait(70);
  require(opens == 0, "destroyed controllers must not run their pending hover callbacks");
}

void cancelledDeadlinesCannotReopen() {
  Fixture f;
  f.controller.setVisibilityMode(OverlayPopupController::VisibilityMode::External);
  f.controller.setMouseEnterDelayMs(20);
  int opens = 0;
  QObject::connect(&f.controller, &OverlayPopupController::popupVisibilityRequested,
                   [&opens](bool visible) {
                     if (visible) ++opens;
                   });
  f.enter(f.trigger);
  f.controller.anchorWidgetChanged();
  QTest::qWait(50);
  require(opens == 0, "replacing the anchor must invalidate its pending deadline");
  f.enter(f.trigger);
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&f.trigger, &escape);
  QTest::qWait(50);
  require(opens == 0, "Escape must cancel a pending hover before the surface opens");
  f.enter(f.trigger);
  f.target = &f.outside;
  f.trigger.move(200, 100);
  QTest::qWait(50);
  require(opens == 0, "moving away from a stationary pointer must cancel pending hover");
}
template <typename Control>
void ordinaryControlUsesQtState() {
  QWidget secondHost;
  QWidget host;
  host.resize(240, 100);
  Control control(&host);
  control.setGeometry(20, 20, 180, 40);
  bool observed = false;
  control.setComponentTokenResolver([&](const typename Control::ComponentTokenContext& state) {
    observed = state.hovered;
    return typename Control::ComponentTokens{};
  });
  host.show();
  QCoreApplication::processEvents();
  const auto paint = [&] {
    QImage image(control.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    control.render(&image);
  };
  const QPoint local(5, 5);
  QEnterEvent enter(local, control.mapTo(&host, local), control.mapToGlobal(local));
  QApplication::sendEvent(&control, &enter);
  paint();
  require(observed, "control style must read Qt hover state");
  control.hide();
  control.show();
  paint();
  require(!observed, "retained controls must not carry hover into a new visible session");
  QApplication::sendEvent(&control, &enter);
  control.setEnabled(false);
  paint();
  require(!observed, "disabled control styles must not retain hover");
  control.setEnabled(true);
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&control, &leave);
  paint();
  require(!observed, "leaving must invalidate cached hover presentation");
  QApplication::sendEvent(&control, &enter);
  secondHost.show();
  control.setParent(&secondHost);
  control.show();
  paint();
  require(!observed, "reparenting must not carry hover into another visual owner");
}

void compositeBoundaryOwnsClearVisibility() {
  QWidget host;
  host.resize(300, 140);
  adqt::widgets::AdSelect select(&host);
  select.setGeometry(20, 20, 200, 40);
  select.setAllowClear(true);
  adqt::widgets::AdSelect::Option option;
  option.value = QStringLiteral("one");
  option.label = QStringLiteral("One");
  select.setOptions({option});
  select.setCurrentValue(QStringLiteral("one"));
  host.show();
  QCoreApplication::processEvents();
  QWidget* clear = select.findChild<QWidget*>(QStringLiteral("adselect-clear"));
  require(clear, "select must expose its clear accessory");
  const QPoint local(5, 5);
  QEnterEvent enter(local, select.mapTo(&host, local), select.mapToGlobal(local));
  QApplication::sendEvent(&select, &enter);
  require(clear->isVisible(), "shell entry must show the clear accessory");
  select.hide();
  select.show();
  require(!clear->isVisible(), "reopening a select must clear the previous accessory hover");
  QApplication::sendEvent(&select, &enter);
  QEvent leave(QEvent::Leave);
  for (QWidget* child : select.findChildren<QWidget*>()) {
    if (child == clear || child->isWindow()) continue;
    QApplication::sendEvent(child, &leave);
  }
  require(clear->isVisible(), "child leave events must not terminate shell hover");
  QApplication::sendEvent(&select, &leave);
  require(!clear->isVisible(), "shell exit must hide the clear accessory");
}

void leavingMenuCancelsPendingOpen() {
  adqt::widgets::AdNavigationMenu menu;
  QStandardItemModel model;
  auto* group = new QStandardItem(QStringLiteral("Group"));
  group->appendRow(new QStandardItem(QStringLiteral("Child")));
  model.appendRow(group);
  menu.setModel(&model);
  menu.setSubmenuOpenDelayMs(30);
  menu.resize(240, 200);
  menu.show();
  QCoreApplication::processEvents();
  QAbstractItemView* view = nullptr;
  for (auto* candidate : menu.findChildren<QAbstractItemView*>()) {
    if (candidate->isVisible()) {
      view = candidate;
      break;
    }
  }
  require(view, "navigation menu must have a visible item view");
  const QModelIndex index = view->model()->index(0, 0);
  const QPoint local = view->visualRect(index).center();
  QMouseEvent move(QEvent::MouseMove, local, view->viewport()->mapToGlobal(local), Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view->viewport(), &move);
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(view, &leave);
  QTest::qWait(70);
  require(!menu.isExpanded(model.index(0, 0)), "menu leave must cancel its pending hover open");
}
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  eventPositionOwnsImmediateTransitions();
  deadlinesValidateTargetAndStopWhenIdle();
  tooltipWindowCannotStealAnEstablishedHoverTarget();
  independentReasonsAndPopupTravel();
  popupShadowOverTriggerPreservesHover();
  embeddedPopupShadowForwardsWheelAfterClosing();
  gapTravelAndRapidReentryPreserveSession();
  focusAndClickSurviveHoverExit();
  destructionCancelsPendingWork();
  closingDuringDispatchCanDestroyTheReceiver();
  geometryBurstsDoNotPostponeReconciliation();
  externalAcknowledgementPreservesEventSession();
  cancelledDeadlinesCannotReopen();
  ordinaryControlUsesQtState<adqt::widgets::AdCheckbox>();
  ordinaryControlUsesQtState<adqt::widgets::AdRadio>();
  ordinaryControlUsesQtState<adqt::widgets::AdSwitch>();
  ordinaryControlUsesQtState<adqt::widgets::AdTag>();
  compositeBoundaryOwnsClearVisibility();
  leavingMenuCancelsPendingOpen();
  return 0;
}
