#include <QApplication>
#include <QCoreApplication>
#include <QHelpEvent>
#include <QListView>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QWidget>

#include "widgets/date_picker.h"
#include "widgets/detail/overlay_popup_surface.h"
#include "widgets/detail/qt_tooltip_bridge.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/tooltip.h"

using adqt::widgets::AdDatePicker;
using adqt::widgets::AdDateRangePicker;
using adqt::widgets::AdPopover;
using adqt::widgets::AdSelect;
using adqt::widgets::AdTooltip;

namespace adqt::widgets::detail {

class OverlayPopupSurfaceTestAccess final {
 public:
  static bool hasPathCache(const OverlayPopupSurface& surface) {
    return surface.pathCache_ != nullptr;
  }

  static bool hasShadowCache(const OverlayPopupSurface& surface) {
    return surface.shadowCache_ != nullptr;
  }

  static bool pathCacheValid(const OverlayPopupSurface& surface) {
    return surface.pathCache_ && surface.pathCache_->valid;
  }

  static bool shadowCacheValid(const OverlayPopupSurface& surface) {
    return surface.shadowCache_ && surface.shadowCache_->valid;
  }
};

}  // namespace adqt::widgets::detail

namespace {

AdSelect::Option makeOption(const QString& value, const QString& label) {
  AdSelect::Option option;
  option.value = value;
  option.label = label;
  return option;
}

QWidget* selectPopupSurface(AdSelect& select) {
  QWidget* candidate = select.view();
  while (candidate && candidate->objectName() != QStringLiteral("adselect-popup")) {
    candidate = candidate->parentWidget();
  }
  return candidate;
}

QWidget* findSurface(const QString& objectName, bool visibleOnly = false) {
  for (QWidget* candidate : QApplication::allWidgets()) {
    if (candidate && candidate->objectName() == objectName &&
        (!visibleOnly || candidate->isVisible())) {
      return candidate;
    }
  }
  return nullptr;
}

void verifyNativeResourcesReleased(QWidget* surface) {
  QVERIFY(surface);
  QTRY_VERIFY(!surface->isVisible());
  QTRY_VERIFY(surface->windowHandle() == nullptr);
  QCOMPARE(surface->backingStore(), nullptr);
}

}  // namespace

class QtToolPopupTest final : public QObject {
  Q_OBJECT

 private slots:
  void siblingPopoversReopenAfterOverdueHoverTasks();
  void popoverReleasesAndRecreatesNativeResources();
  void popupTriggerTooltipsRequireOptIn();
  void popupTriggerTooltipsAvoidPopoverAtScreenEdges();
  void popupTriggerTooltipsRequireSpaceBelow();
  void warmTooltipSurvivesGroupPopoverOpening();
  void pendingTooltipSurvivesGroupRouteRegistration();
  void selectReleasesAndRecreatesNativeResources();
  void selectSurvivesPopupContainerDestructionOrder();
  void tooltipReleasesAndRecreatesNativeResources();
  void datePickerReleasesAndRecreatesNativeResources();
  void dateRangePickerReleasesAndRecreatesNativeResources();
  void recreateLifetimeStillDestroysPopupSurface();
  void retainedPopupCachesFollowVisibilityAndStayComponentLocal();
};

void QtToolPopupTest::popupTriggerTooltipsRequireOptIn() {
  AdTooltip::installApplicationTooltips();
  QWidget host;
  host.resize(640, 360);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(24, 24, 100, 32);
  trigger->setToolTip(QStringLiteral("Draw (2)"));
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setText(QStringLiteral("Drawing options"));
  popover.setPlacement(AdPopover::Placement::Top);
  host.show();
  QCoreApplication::processEvents();
  popover.show();
  QCoreApplication::processEvents();
  QVERIFY(popover.isVisible());

  for (bool enabled : {false, true, false}) {
    trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, enabled);
    const QPoint center = trigger->rect().center();
    QHelpEvent help(QEvent::ToolTip, center, trigger->mapToGlobal(center));
    QApplication::sendEvent(trigger, &help);
    QCOMPARE(help.isAccepted(), enabled);
    bool visible = false;
    for (auto* tooltip : qApp->findChildren<AdTooltip*>()) {
      visible |= tooltip->isVisible() && tooltip->targetWidget() == trigger &&
                 tooltip->text() == trigger->toolTip();
    }
    QCOMPARE(visible, enabled);
  }
}

void QtToolPopupTest::popupTriggerTooltipsAvoidPopoverAtScreenEdges() {
  AdTooltip::installApplicationTooltips();
  const QRect screen = QApplication::primaryScreen()->availableGeometry();
  for (int position : {0, 1, 2}) {
    QWidget host(nullptr, Qt::FramelessWindowHint);
    const int y = position == 0   ? screen.top() + 2
                  : position == 1 ? screen.center().y()
                                  : screen.bottom() - 41;
    host.setGeometry(screen.center().x() - 160, y, 320, 40);
    auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
    trigger->setGeometry(100, 8, 100, 32);
    trigger->setToolTip(QStringLiteral("Draw (2)"));
    trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
    AdPopover popover;
    popover.setSourceWidget(trigger);
    popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    popover.setPlacement(AdPopover::Placement::Top);
    auto* content = new QWidget;
    content->setFixedSize(260, 90);
    popover.setContentWidget(content);
    host.show();
    QCoreApplication::processEvents();
    const auto requestTooltip = [trigger]() {
      const QPoint center = trigger->rect().center();
      QHelpEvent help(QEvent::ToolTip, center, trigger->mapToGlobal(center));
      QApplication::sendEvent(trigger, &help);
      for (auto* tip : qApp->findChildren<AdTooltip*>()) {
        if (tip->isVisible() && tip->targetWidget() == trigger) {
          return tip;
        }
      }
      return static_cast<AdTooltip*>(nullptr);
    };
    // Bottom-only placement applies before the group menu opens as well.
    QCOMPARE(requestTooltip() != nullptr, position != 2);
    popover.show();
    QCoreApplication::processEvents();
    QVERIFY(popover.isVisible());
    auto* popup = static_cast<adqt::widgets::detail::OverlayPopupSurface*>(content->window());
    const QRect popupBody =
        QRect(popup->mapToGlobal(QPoint()), popup->size()).marginsRemoved(popup->shadowMargins());
    const QRect triggerRect(trigger->mapToGlobal(QPoint()), trigger->size());
    QCOMPARE(popupBody.center().y() > triggerRect.center().y(), position == 0);
    AdTooltip* tip = requestTooltip();
    QCOMPARE(tip != nullptr, position == 1);
    if (tip) {
      QCOMPARE(tip->anchorWidget(), trigger);
      QCOMPARE(tip->anchorRect(), trigger->rect());
      QCOMPARE(tip->placement(), AdTooltip::Placement::Bottom);
      auto* surface = static_cast<adqt::widgets::detail::OverlayPopupSurface*>(
          findSurface(QStringLiteral("adtooltip-surface"), true));
      QVERIFY(surface && surface->isVisible());
      const QRect body = QRect(surface->mapToGlobal(QPoint()), surface->size())
                             .marginsRemoved(surface->shadowMargins());
      QVERIFY(body.top() > triggerRect.bottom());
      QVERIFY(!body.intersects(popupBody));
      QVERIFY(screen.contains(body));
    }
    popover.hide();
    QCoreApplication::processEvents();
    QCOMPARE(requestTooltip() != nullptr, position != 2);
  }
}

void QtToolPopupTest::popupTriggerTooltipsRequireSpaceBelow() {
  AdTooltip::installApplicationTooltips();
  const QRect screen = QApplication::primaryScreen()->availableGeometry();
  QWidget host(nullptr, Qt::FramelessWindowHint);
  host.setGeometry(screen.center().x() - 100, screen.center().y(), 200, 32);
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(50, 0, 100, 32);
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  host.show();
  QCoreApplication::processEvents();
  AdTooltip::showText(trigger, QStringLiteral("Draw (2)"));
  auto* surface = static_cast<adqt::widgets::detail::OverlayPopupSurface*>(
      findSurface(QStringLiteral("adtooltip-surface"), true));
  QVERIFY(surface && surface->isVisible());
  const QRect body = QRect(surface->mapToGlobal(QPoint()), surface->size())
                         .marginsRemoved(surface->shadowMargins());
  const int requiredSpace = body.bottom() - trigger->mapToGlobal(trigger->rect().bottomLeft()).y();
  for (int extraSpace : {0, -1, 1}) {
    for (auto* tip : qApp->findChildren<AdTooltip*>()) {
      tip->hide();
    }
    const int triggerBottom = screen.bottom() - requiredSpace - extraSpace;
    host.move(host.x(), triggerBottom - trigger->height() + 1);
    QCoreApplication::processEvents();
    AdTooltip::showText(trigger, QStringLiteral("Draw (2)"));
    bool visible = false;
    for (auto* tip : qApp->findChildren<AdTooltip*>()) {
      visible |= tip->isVisible() && tip->targetWidget() == trigger;
    }
    QCOMPARE(visible, extraSpace >= 0);
  }
}

void QtToolPopupTest::warmTooltipSurvivesGroupPopoverOpening() {
  AdTooltip::installApplicationTooltips();
  QWidget host(nullptr, Qt::FramelessWindowHint);
  host.resize(400, 100);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* previous = new QPushButton(QStringLiteral("Previous"), &host);
  previous->setGeometry(20, 20, 100, 32);
  previous->setToolTip(QStringLiteral("Previous tool"));
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(140, 20, 100, 32);
  trigger->setToolTip(QStringLiteral("Draw (2)"));
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setPlacement(AdPopover::Placement::Top);
  popover.setTriggers(AdPopover::Trigger::Hover);
  auto* content = new QWidget;
  content->setFixedSize(240, 60);
  popover.setContentWidget(content);
  host.show();
  QCoreApplication::processEvents();

  const auto requestTooltip = [](QWidget* target) {
    const QPoint center = target->rect().center();
    QHelpEvent help(QEvent::ToolTip, center, target->mapToGlobal(center));
    QApplication::sendEvent(target, &help);
  };
  requestTooltip(previous);
  AdTooltip* tip = nullptr;
  for (auto* candidate : qApp->findChildren<AdTooltip*>()) {
    if (candidate->isVisible() && candidate->targetWidget() == previous) {
      tip = candidate;
      break;
    }
  }
  QVERIFY(tip);
  QSignalSpy visibility(tip, &AdTooltip::visibleChanged);
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(previous, &leave);
  requestTooltip(trigger);
  QVERIFY(tip->isVisible());
  QCOMPARE(tip->targetWidget(), trigger);
  QCOMPARE(tip->text(), trigger->toolTip());
  for (int cycle = 0; cycle < 3; ++cycle) {
    popover.show();
    QVERIFY(popover.isVisible());
    const auto* surface =
        static_cast<const adqt::widgets::detail::OverlayPopupSurface*>(content->window());
    const QRect body = surface->rect().marginsRemoved(surface->shadowMargins());
    QVERIFY(surface->mapToGlobal(body.bottomLeft()).y() < trigger->mapToGlobal(QPoint()).y());
    QVERIFY2(tip->isVisible(), "opening the upper group popover must preserve its visible tip");
    QCOMPARE(tip->targetWidget(), trigger);
    QCOMPARE(tip->text(), trigger->toolTip());
    popover.hide();
    QVERIFY(tip->isVisible());
  }
  QCOMPARE(visibility.count(), 0);
  tip->hide();
}

void QtToolPopupTest::pendingTooltipSurvivesGroupRouteRegistration() {
  class TooltipDelayStyle final : public QProxyStyle {
   public:
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* data = nullptr) const override {
      return hint == QStyle::SH_ToolTip_WakeUpDelay
                 ? 100
                 : QProxyStyle::styleHint(hint, option, widget, data);
    }
  };
  AdTooltip::installApplicationTooltips();
  QWidget host(nullptr, Qt::FramelessWindowHint);
  host.resize(400, 100);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(140, 20, 100, 32);
  trigger->setToolTip(QStringLiteral("Draw (2)"));
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  auto* style = new TooltipDelayStyle;
  style->setParent(trigger);
  trigger->setStyle(style);
  // Exercise route registration separately from platform focus changes when
  // opening native windows. The route's surface is already laid out above the button.
  QWidget popup(nullptr, Qt::Tool | Qt::FramelessWindowHint);
  popup.setGeometry(trigger->mapToGlobal(QPoint(0, -80)).x(),
                    trigger->mapToGlobal(QPoint(0, -80)).y(), 180, 60);
  host.show();
  popup.show();
  QCoreApplication::processEvents();
  const QPoint center = trigger->rect().center();
  const QPoint global = trigger->mapToGlobal(center);
  QMouseEvent move(QEvent::MouseMove, center, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(trigger, &move);
  QHelpEvent help(QEvent::ToolTip, center, global);
  QApplication::sendEvent(trigger, &help);
  QVERIFY(help.isAccepted());
  const auto visibleTip = [trigger]() -> AdTooltip* {
    for (auto* tip : qApp->findChildren<AdTooltip*>()) {
      if (tip->isVisible() && tip->targetWidget() == trigger) {
        return tip;
      }
    }
    return nullptr;
  };
  QVERIFY(!visibleTip());
  QObject owner;
  adqt::widgets::detail::syncTopLevelPopupTooltipRoute(&owner, trigger, &popup, true);
  QTRY_VERIFY_WITH_TIMEOUT(visibleTip(), 1500);
  AdTooltip* tip = visibleTip();
  QSignalSpy visibility(tip, &AdTooltip::visibleChanged);
  adqt::widgets::detail::syncTopLevelPopupTooltipRoute(&owner, trigger, &popup, false);
  QVERIFY(tip->isVisible());
  QCOMPARE(visibility.count(), 0);
  tip->hide();
}

void QtToolPopupTest::siblingPopoversReopenAfterOverdueHoverTasks() {
  QWidget host;
  host.resize(640, 360);
  QPushButton first(QStringLiteral("First"), &host), second(QStringLiteral("Second"), &host);
  first.setGeometry(50, 150, 100, 32);
  second.setGeometry(220, 150, 100, 32);
  AdPopover firstPopup, secondPopup;
  firstPopup.setSourceWidget(&first);
  secondPopup.setSourceWidget(&second);
  for (auto* popup : {&firstPopup, &secondPopup}) {
    popup->setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    popup->setTriggers(AdPopover::Trigger::Hover);
    popup->setHoverOpenDelayMs(10);
    popup->setHoverCloseDelayMs(10);
    auto* content = new QWidget;
    content->setFixedSize(100, 30);
    popup->setContentWidget(content);
  }
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  host.raise();
  for (int cycle = 0; cycle < 3; ++cycle) {
    QTest::mouseMove(&host, QPoint(400, 280));
    QTRY_VERIFY(!firstPopup.isVisible() && !secondPopup.isVisible());
    QTest::mouseMove(&first, first.rect().center());
    // Let hover deadlines become overdue without dispatching GUI events.
    QTest::qSleep(40);
    QTRY_VERIFY(firstPopup.isVisible());
    QTRY_VERIFY(firstPopup.contentWidget()->window()->isVisible());
    QTest::mouseMove(&second, second.rect().center());
    QTest::qSleep(40);
    QTRY_VERIFY(secondPopup.isVisible());
    QTRY_VERIFY(secondPopup.contentWidget()->window()->isVisible());
    QTRY_VERIFY(!firstPopup.isVisible());
  }
}

void QtToolPopupTest::popoverReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);

  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setText(QStringLiteral("Popover content"));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QWidget* surface = findSurface(QStringLiteral("adpopover-surface"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  popover.hide();
  verifyNativeResourcesReleased(surface);

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCOMPARE(findSurface(QStringLiteral("adpopover-surface")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  // A close followed by an immediate reopen must cancel the deferred release.
  popover.hide();
  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCoreApplication::processEvents();
  QVERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::selectSurvivesPopupContainerDestructionOrder() {
  auto* host = new QWidget;
  auto* select = new AdSelect(host);
  select->setOptions({makeOption(QStringLiteral("hsb"), QStringLiteral("HSB"))});
  host->show();
  select->showPopup();
  QPointer<QWidget> surface = selectPopupSurface(*select);
  QVERIFY(surface);
  QCOMPARE(surface->parentWidget(), host);
  select->hidePopup();

  // A picker reparents its format selector into a later-created panel. The
  // original container then destroys the popup before the selector itself.
  auto* panel = new QWidget(host);
  select->setParent(panel);
  QPointer<AdSelect> guardedSelect = select;
  delete host;
  QVERIFY(surface.isNull());
  QVERIFY(guardedSelect.isNull());

  QWidget survivingHost;
  host = new QWidget;
  select = new AdSelect(host);
  select->setOptions({makeOption(QStringLiteral("rgb"), QStringLiteral("RGB"))});
  host->show();
  select->showPopup();
  surface = selectPopupSurface(*select);
  QVERIFY(surface);
  select->hidePopup();
  select->setParent(&survivingHost);
  delete host;
  QVERIFY(surface.isNull());
  survivingHost.show();
  select->show();
  select->showPopup();
  QVERIFY(selectPopupSurface(*select));
  QVERIFY(select->popupVisible());
}

void QtToolPopupTest::selectReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  AdSelect select(&host);
  select.setGeometry(24, 24, 180, 32);
  select.setPopupLayerMode(AdSelect::PopupLayerMode::QtTool);
  select.setOptions({makeOption(QStringLiteral("one"), QStringLiteral("One")),
                     makeOption(QStringLiteral("two"), QStringLiteral("Two"))});

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  select.showPopup();
  QTRY_VERIFY(select.popupVisible());
  QWidget* surface = selectPopupSurface(select);
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  select.hidePopup();
  verifyNativeResourcesReleased(surface);

  select.showPopup();
  QTRY_VERIFY(select.popupVisible());
  QCOMPARE(selectPopupSurface(select), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::tooltipReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  auto* target = new QPushButton(QStringLiteral("Target"), &host);
  target->setGeometry(24, 24, 100, 32);

  AdTooltip tooltip;
  tooltip.setTargetWidget(target);
  tooltip.setLayerMode(AdTooltip::LayerMode::TopLevelTransient);
  tooltip.setTriggers(AdTooltip::Trigger::Click);
  tooltip.setText(QStringLiteral("Tooltip content"));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  tooltip.show();
  QTRY_VERIFY(tooltip.isVisible());
  QWidget* surface = findSurface(QStringLiteral("adtooltip-surface"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  tooltip.hide();
  verifyNativeResourcesReleased(surface);

  tooltip.show();
  QTRY_VERIFY(tooltip.isVisible());
  QCOMPARE(findSurface(QStringLiteral("adtooltip-surface")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::datePickerReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  AdDatePicker picker(&host);
  picker.setGeometry(24, 24, 220, 32);
  picker.setPopupLayerMode(AdDatePicker::PopupLayerMode::QtTool);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QWidget* surface = findSurface(QStringLiteral("addatepicker-popup"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  picker.hidePopup();
  verifyNativeResourcesReleased(surface);

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QCOMPARE(findSurface(QStringLiteral("addatepicker-popup")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::dateRangePickerReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(760, 360);
  AdDateRangePicker picker(&host);
  picker.setGeometry(24, 24, 320, 32);
  picker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::QtTool);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QWidget* surface = findSurface(QStringLiteral("addaterangepicker-popup"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  picker.hidePopup();
  verifyNativeResourcesReleased(surface);

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QCOMPARE(findSurface(QStringLiteral("addaterangepicker-popup")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::recreateLifetimeStillDestroysPopupSurface() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);

  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setPopupLifetime(AdPopover::PopupLifetime::RecreateOnOpen);
  popover.setText(QStringLiteral("Popover content"));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QPointer<QWidget> firstSurface = findSurface(QStringLiteral("adpopover-surface"));
  QVERIFY(firstSurface);
  QTRY_VERIFY(firstSurface->isVisible());

  popover.hide();
  QTRY_VERIFY(!popover.isVisible());
  QTRY_VERIFY(firstSurface.isNull());

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QWidget* secondSurface = findSurface(QStringLiteral("adpopover-surface"));
  QVERIFY(secondSurface);
}

void QtToolPopupTest::retainedPopupCachesFollowVisibilityAndStayComponentLocal() {
  QWidget host;
  host.resize(640, 360);
  adqt::widgets::detail::OverlayPopupSurface first(&host);
  adqt::widgets::detail::OverlayPopupSurface second(&host);
  first.resize(180, 96);
  second.resize(180, 96);
  first.move(24, 80);
  second.move(240, 80);

  QVERIFY(!adqt::widgets::detail::OverlayPopupSurfaceTestAccess::hasPathCache(first));
  QVERIFY(!adqt::widgets::detail::OverlayPopupSurfaceTestAccess::hasShadowCache(first));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  first.show();
  second.show();
  QTRY_VERIFY(first.isVisible() && second.isVisible());
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::pathCacheValid(first));
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::shadowCacheValid(first));
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::pathCacheValid(second));
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::shadowCacheValid(second));

  first.hide();
  QTRY_VERIFY(!first.isVisible());
  QVERIFY(!first.containsInteractiveLocalPos(QPointF(40.0, 40.0)));
  QVERIFY(!adqt::widgets::detail::OverlayPopupSurfaceTestAccess::hasPathCache(first));
  QVERIFY(!adqt::widgets::detail::OverlayPopupSurfaceTestAccess::hasShadowCache(first));
  QVERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::pathCacheValid(second));
  QVERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::shadowCacheValid(second));

  first.show();
  QTRY_VERIFY(first.isVisible());
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::pathCacheValid(first));
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::shadowCacheValid(first));
}

QTEST_MAIN(QtToolPopupTest)

#include "qt_tool_popup_tests.moc"
