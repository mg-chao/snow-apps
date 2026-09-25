#include <QApplication>
#include <QCoreApplication>
#include <QHelpEvent>
#include <QListView>
#include <QMouseEvent>
#include <QImage>
#include <QPainter>
#include <QProxyStyle>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QStyleOption>
#include <QTest>
#include <QWidget>

#include <algorithm>

#include "widgets/date_picker.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/detail/overlay_popup_surface.h"
#include "widgets/detail/qt_tooltip_bridge.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/tooltip.h"

#if defined(Q_OS_MACOS)
#include "macos_native_input.h"
#endif

using adqt::widgets::AdColorPicker;
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
  void customButtonHitAreaIgnoresNativeBevel();
  void popupHoverShapeTracksPaintedSurface();
  void popupSurfacePreservesAntialiasedEdges();
#if defined(Q_OS_MACOS)
  void popupShadowDoesNotCoverUnderlyingWindow();
#endif
  void siblingPopoversReopenAfterOverdueHoverTasks();
  void siblingPopoversReopenAfterScopeRecreation();
  void popoverReleasesAndRecreatesNativeResources();
  void popupTriggerTooltipsRequireOptIn();
  void popupOptionTooltipsRemainVisible();
  void unrelatedLeaveDoesNotDismissTooltip();
  void popupTriggerTooltipsAvoidPopoverAtScreenEdges();
  void popupTriggerTooltipsRequireSpaceBelow();
  void warmTooltipSurvivesGroupPopoverOpening();
  void hoveringFromTooltipToAdjacentPopoverOpensPopover();
  void pendingTooltipSurvivesGroupRouteRegistration();
  void selectReleasesAndRecreatesNativeResources();
  void selectSurvivesPopupContainerDestructionOrder();
  void tooltipReleasesAndRecreatesNativeResources();
  void datePickerReleasesAndRecreatesNativeResources();
  void dateRangePickerReleasesAndRecreatesNativeResources();
  void recreateLifetimeStillDestroysPopupSurface();
  void retainedPopupCachesFollowVisibilityAndStayComponentLocal();
  void retainedFactoryContentIsLazyAndOwnerBound();
  void recreateFactoryContentIsReleasedAfterHide();
  void directContentRemainsCompatibleWithFactoryApi();
  void colorPickerPrewarmCanBeDisabled();
};

void QtToolPopupTest::customButtonHitAreaIgnoresNativeBevel() {
  class InsetButtonStyle final : public QProxyStyle {
   public:
    QRect subElementRect(SubElement element, const QStyleOption* option,
                         const QWidget* widget = nullptr) const override {
      if (element == SE_PushButtonBevel) {
        return option->rect.adjusted(6, 6, -6, -6);
      }
      return QProxyStyle::subElementRect(element, option, widget);
    }
  };
  adqt::widgets::AdButton button;
  auto* style = new InsetButtonStyle;
  style->setParent(&button);
  button.setStyle(style);
  button.setShape(adqt::widgets::AdButton::Shape::Rounded);
  button.resize(32, 32);
  button.show();
  QSignalSpy clicked(&button, &adqt::widgets::AdButton::clicked);
  int expected = 0;
  for (const QPoint point :
       {QPoint(16, 1), QPoint(16, 16), QPoint(16, 30), QPoint(1, 16), QPoint(30, 16)}) {
    QTest::mouseClick(&button, Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(clicked.count(), ++expected);
  }
  button.setShape(adqt::widgets::AdButton::Shape::Circle);
  QTest::mouseClick(&button, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
  QCOMPARE(clicked.count(), expected);
  QTest::mouseClick(&button, Qt::LeftButton, Qt::NoModifier, QPoint(16, 16));
  QCOMPARE(clicked.count(), ++expected);
  button.setDisabled(true);
  QTest::mouseClick(&button, Qt::LeftButton, Qt::NoModifier, QPoint(16, 16));
  QCOMPARE(clicked.count(), expected);
}

#if defined(Q_OS_MACOS)
void QtToolPopupTest::popupShadowDoesNotCoverUnderlyingWindow() {
  if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
    QSKIP("Requires the Cocoa window server");
  }
  QVERIFY2(macCanPostMouseEvents(),
           "Native click test requires Accessibility event-posting access");
  MacCursorRestore restoreCursor;
  using namespace adqt::widgets::detail;
  QWidget host;
  host.resize(500, 300);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* underlying = new QPushButton(QStringLiteral("Underlying"), &host);
  underlying->setGeometry(host.rect());
  QSignalSpy clicked(underlying, &QPushButton::clicked);
  host.show();
  host.raise();
  macRaiseTestWindow(&host, 1100);
  macActivateApplication();
  host.activateWindow();
  QTRY_VERIFY(host.isActiveWindow());
  OverlayPopupSurface surface;
  surface.setWindowFlags(overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.setAttribute(Qt::WA_TranslucentBackground);
  surface.setAttribute(Qt::WA_ShowWithoutActivating);
  OverlayPopupSurfaceStyle style;
  style.background = Qt::white;
  surface.setSurfaceStyle(style);
  surface.resize(240, 120);
  auto* option = new QPushButton(QStringLiteral("Popup option"), surface.bodyWidget());
  option->setGeometry(surface.bodyWidget()->rect().adjusted(16, 16, -16, -16));
  QSignalSpy optionClicked(option, &QPushButton::clicked);
  surface.move(host.mapToGlobal(QPoint(80, 60)));
  syncTopLevelToolTransientParent(&surface, &host);
  int expectedUnderlyingClicks = 0;
  int expectedOptionClicks = 0;
  for (int cycle = 0; cycle < 3; ++cycle) {
    surface.setArrowVisible(cycle < 2);
    surface.show();
    surface.raise();
    macRaiseTestWindow(&surface, 1101);
    for (const auto placement : {OverlayPopupPlacement::Top, OverlayPopupPlacement::Bottom,
                                 OverlayPopupPlacement::Left, OverlayPopupPlacement::Right}) {
      surface.setPlacement(placement);
      surface.setArrowCenter(80);
      option->setGeometry(surface.bodyWidget()->rect().adjusted(16, 16, -16, -16));
      const QPoint body = option->mapToGlobal(option->rect().center());
      QCOMPARE(surface.shadowMargins(), QMargins());
      QVERIFY(!surface.windowFlags().testFlag(Qt::NoDropShadowWindowHint));
      QTRY_VERIFY(macWindowReceivesPoint(&surface, body));
      QVERIFY(macWindowHasShadow(&surface));
      // Test real delivery outside the frame, in a rounded corner, and in the
      // transparent strip beside the arrow. No mouse events are replayed.
      const QPoint gutter =
          placement == OverlayPopupPlacement::Top      ? QPoint(20, surface.height() - 2)
          : placement == OverlayPopupPlacement::Bottom ? QPoint(20, 1)
          : placement == OverlayPopupPlacement::Left   ? QPoint(surface.width() - 2, 20)
                                                       : QPoint(1, 20);
      QVector<QPoint> transparentPoints{QPoint(surface.width() / 2, surface.height() + 4),
                                        QPoint(0, 0)};
      if (surface.arrowVisible()) {
        transparentPoints.push_back(gutter);
      }
      for (const QPoint local : transparentPoints) {
        const QPoint point = surface.mapToGlobal(local);
        macPostClick(point);
        ++expectedUnderlyingClicks;
        QTRY_VERIFY2(
            clicked.count() == expectedUnderlyingClicks,
            qPrintable(
                QStringLiteral(
                    "click did not reach underlying button: placement %1, local %2,%3, cycle %4")
                    .arg(static_cast<int>(placement))
                    .arg(local.x())
                    .arg(local.y())
                    .arg(cycle)));
      }
      macPostClick(body);
      ++expectedOptionClicks;
      QTRY_COMPARE(optionClicked.count(), expectedOptionClicks);
      QCOMPARE(clicked.count(), expectedUnderlyingClicks);
    }
    surface.hide();
    surface.releaseTopLevelToolResources();
    syncTopLevelToolTransientParent(&surface, &host);
  }
}
#endif

void QtToolPopupTest::popupSurfacePreservesAntialiasedEdges() {
#if defined(Q_OS_MACOS)
  using namespace adqt::widgets::detail;
  OverlayPopupSurface surface;
  surface.setWindowFlags(overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.setAttribute(Qt::WA_TranslucentBackground);
  OverlayPopupSurfaceStyle style;
  style.background = Qt::white;
  style.borderColor = Qt::black;
  style.metrics.borderRadius = 12;
  surface.setSurfaceStyle(style);
  surface.resize(240, 120);
  surface.show();
  const auto verifyRendering = [&surface]() {
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
      const auto render = [&surface, dpr](QWidget::RenderFlags flags) {
        QImage image(QSize(qRound(surface.width() * dpr), qRound(surface.height() * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        surface.render(&painter, QPoint(), QRegion(), flags);
        return image;
      };
      const QImage expected = render(QWidget::DrawChildren | QWidget::IgnoreMask);
      const QImage actual = render(QWidget::DrawChildren);
      int blendedPixels = 0;
      for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
          const int alpha = qAlpha(expected.pixel(x, y));
          blendedPixels += alpha > 0 && alpha < 255;
        }
      }
      if (surface.arrowVisible() || surface.surfaceStyle().metrics.borderRadius > 0) {
        QVERIFY(blendedPixels > 0);
      }
      QVERIFY2(
          actual == expected,
          qPrintable(
              QStringLiteral("native surface clipping loses painted edges at DPR %1").arg(dpr)));
    }
  };
  for (const auto placement : {OverlayPopupPlacement::Top, OverlayPopupPlacement::Bottom,
                               OverlayPopupPlacement::Left, OverlayPopupPlacement::Right}) {
    surface.setPlacement(placement);
    for (const qreal center : {42.0, 83.5}) {
      surface.setArrowCenter(center);
      for (const int borderWidth : {0, 1, 3}) {
        style.metrics.borderWidth = borderWidth;
        style.arrowBackground = QColor(230, 240, 255);
        style.arrowBorderColor = QColor(30, 60, 90);
        surface.setSurfaceStyle(style);
        verifyRendering();
      }
    }
    surface.resize(surface.width() + 10, surface.height() + 10);
    verifyRendering();
  }
  surface.setArrowVisible(false);
  verifyRendering();
  style.metrics.borderRadius = 0;
  surface.setSurfaceStyle(style);
  verifyRendering();
  surface.hide();
  surface.releaseTopLevelToolResources();
  surface.setArrowVisible(true);
  surface.show();
  verifyRendering();
  QWidget parent;
  parent.resize(500, 300);
  surface.hide();
  surface.setParent(&parent);
  parent.show();
  surface.show();
  verifyRendering();
  surface.hide();
  surface.setParent(nullptr, overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.show();
  verifyRendering();
#endif
}

void QtToolPopupTest::popupHoverShapeTracksPaintedSurface() {
#if defined(Q_OS_MACOS)
  using namespace adqt::widgets::detail;
  OverlayPopupSurface surface;
  surface.setWindowFlags(overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.setAttribute(Qt::WA_TranslucentBackground);
  OverlayPopupSurfaceStyle style;
  style.background = Qt::white;
  style.metrics.borderRadius = 12;
  surface.setSurfaceStyle(style);
  surface.resize(240, 120);
  const auto verifyShape = [&surface]() {
    QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    surface.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    painter.end();
    QVERIFY(surface.containsInteractiveLocalPos(surface.bodyWidget()->geometry().center()));
    QCOMPARE(surface.shadowMargins(), QMargins());
    // Sample away from rasterized edges: input must follow the bubble and arrow,
    // never the surrounding painted-shadow frame.
    for (int y = 2; y < surface.height(); y += 5) {
      for (int x = 2; x < surface.width(); x += 5) {
        const QPoint point(x, y);
        const bool inside = surface.containsInteractiveLocalPos(point);
        bool awayFromEdge = true;
        for (int dy = -2; dy <= 2; ++dy) {
          for (int dx = -2; dx <= 2; ++dx) {
            awayFromEdge &= inside == surface.containsInteractiveLocalPos(point + QPoint(dx, dy));
          }
        }
        if (awayFromEdge) {
          QVERIFY2((qAlpha(image.pixel(point)) > 0) == inside,
                   qPrintable(QStringLiteral("shape mismatch at %1,%2").arg(x).arg(y)));
        }
      }
    }
  };
  surface.show();
  QCoreApplication::processEvents();
  verifyShape();
  for (auto placement : {OverlayPopupPlacement::Bottom, OverlayPopupPlacement::Left,
                         OverlayPopupPlacement::Right, OverlayPopupPlacement::Top}) {
    surface.setPlacement(placement);
    surface.setArrowCenter(42);
    verifyShape();
    surface.resize(surface.width() + 20, surface.height() + 10);
    verifyShape();
  }
  surface.setArrowVisible(false);
  verifyShape();
  style.metrics.borderRadius = 0;
  surface.setSurfaceStyle(style);
  verifyShape();
  surface.hide();
  surface.releaseTopLevelToolResources();
  surface.setArrowVisible(true);
  surface.show();
  QCoreApplication::processEvents();
  verifyShape();
  QWidget parent;
  parent.resize(500, 300);
  surface.hide();
  surface.setParent(&parent);
  parent.show();
  surface.show();
  QCoreApplication::processEvents();
  QVERIFY(surface.mask().isEmpty());
  surface.setParent(nullptr, overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.show();
  QCoreApplication::processEvents();
  verifyShape();
#endif
}

void QtToolPopupTest::unrelatedLeaveDoesNotDismissTooltip() {
  AdTooltip::installApplicationTooltips();
  QWidget host;
  host.resize(400, 300);
  QPushButton target(QStringLiteral("Target"), &host);
  target.setGeometry(30, 30, 100, 30);
  target.setToolTip(QStringLiteral("Target help"));
  QWidget unrelated(&host);
  unrelated.setGeometry(250, 200, 50, 50);
  host.show();
  host.activateWindow();
  QCoreApplication::processEvents();
  const QPoint local = target.rect().center();
  QHelpEvent help(QEvent::ToolTip, local, target.mapToGlobal(local));
  QApplication::sendEvent(&target, &help);
  QTRY_VERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&unrelated, &leave);
  QTest::qWait(400);
  QVERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  target.hide();
  QTRY_VERIFY(!findSurface(QStringLiteral("adtooltip-surface"), true));
}

void QtToolPopupTest::popupOptionTooltipsRemainVisible() {
  AdTooltip::installApplicationTooltips();
  QWidget host;
  host.resize(640, 360);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(240, 160, 100, 32);
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  AdPopover popover(trigger);
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setPlacement(AdPopover::Placement::Top);
  auto* option = new QPushButton(QStringLiteral("Rectangle"));
  option->setToolTip(QStringLiteral("Draw rectangle"));
  popover.setContentWidget(option);
  host.show();
  QCoreApplication::processEvents();
  popover.show();
  QCoreApplication::processEvents();
  QVERIFY(option->isVisible());
  host.activateWindow();
  QCoreApplication::processEvents();
  QVERIFY(!option->window()->isActiveWindow());
  const QPoint center = option->rect().center();
  QTest::mouseMove(option->window()->windowHandle(), option->mapTo(option->window(), center));
  QTRY_VERIFY_WITH_TIMEOUT(findSurface(QStringLiteral("adtooltip-surface"), true), 2000);
  QVERIFY(popover.isVisible());
  QVERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  bool matched = false;
  for (auto* tip : qApp->findChildren<AdTooltip*>()) {
    matched |=
        tip->isVisible() && tip->targetWidget() == option && tip->text() == option->toolTip();
  }
  QVERIFY(matched);
}

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

void QtToolPopupTest::hoveringFromTooltipToAdjacentPopoverOpensPopover() {
  AdTooltip::installApplicationTooltips();
  QWidget host(nullptr, Qt::FramelessWindowHint);
  host.resize(400, 120);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* previous = new QPushButton(QStringLiteral("Scroll"), &host);
  previous->setGeometry(80, 55, 100, 32);
  previous->setToolTip(QStringLiteral("Scrolling screenshot"));
  auto* trigger = new QPushButton(QStringLiteral("Save"), &host);
  trigger->setGeometry(180, 55, 100, 32);
  trigger->setToolTip(QStringLiteral("Save as file"));
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setPlacement(AdPopover::Placement::Top);
  popover.setTriggers(AdPopover::Trigger::Hover);
  popover.setHoverOpenDelayMs(30);
  auto* content = new QWidget;
  content->setFixedSize(160, 40);
  popover.setContentWidget(content);
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  host.raise();

  QTest::mouseMove(previous, previous->rect().center());
  AdTooltip::showText(previous, previous->toolTip());
  QTRY_VERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  QTest::mouseMove(trigger, trigger->rect().center());
  AdTooltip::showText(trigger, trigger->toolTip());
  QTRY_VERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  QTRY_VERIFY(popover.isVisible());
  QTRY_VERIFY(content->window()->isVisible());
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

void QtToolPopupTest::siblingPopoversReopenAfterScopeRecreation() {
  class ReusableScope : public QWidget {
   public:
    void releaseNativeSurface() {
      hide();
      destroy(true, true);
    }
  } host;
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
    popup->setHoverOpenDelayMs(40);
    popup->setHoverCloseDelayMs(10);
    auto* content = new QWidget;
    content->setFixedSize(100, 30);
    popup->setContentWidget(content);
  }
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  host.raise();
  for (bool closeBeforeHoverDeadline : {true, false}) {
    QTest::mouseMove(&host, QPoint(400, 280));
    QTRY_VERIFY(!firstPopup.isVisible() && !secondPopup.isVisible());
    QTest::mouseMove(&first, first.rect().center());
    if (closeBeforeHoverDeadline) {
      QVERIFY(!firstPopup.isVisible());
    } else {
      QTRY_VERIFY(firstPopup.isVisible());
      QVERIFY(firstPopup.contentWidget()->window()->isVisible());
    }
    // Capture exit retains the QObject tree but releases the toolbar's native window.
    host.releaseNativeSurface();
    QTest::qWait(80);
    QVERIFY(!firstPopup.isVisible() && !secondPopup.isVisible());
    QVERIFY(!host.windowHandle());
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    host.raise();
    QTest::mouseMove(&host, QPoint(400, 280));
    for (auto* popup : {&secondPopup, &firstPopup}) {
      QWidget* trigger = popup->sourceWidget();
      QTest::mouseMove(trigger, trigger->rect().center());
      QTRY_VERIFY(popup->isVisible());
      QVERIFY(popup->contentWidget()->isVisible());
      QVERIFY(popup->contentWidget()->window()->isVisible());
    }
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

void QtToolPopupTest::retainedFactoryContentIsLazyAndOwnerBound() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);
  auto* popover = new AdPopover(trigger);
  popover->setSourceWidget(trigger);
  popover->setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);

  int creationCount = 0;
  popover->setContentFactory([&creationCount]() {
    ++creationCount;
    auto* content = new QWidget;
    content->setFixedSize(120, 48);
    return content;
  });
  QCOMPARE(popover->contentWidget(), nullptr);
  QCOMPARE(creationCount, 0);

  bool contentExistedWhenShown = false;
  connect(popover, &AdPopover::visibleChanged, popover,
          [popover, &contentExistedWhenShown](bool value) {
            if (value) {
              contentExistedWhenShown = popover->contentWidget() != nullptr;
            }
          });
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover->show();
  QTRY_VERIFY(popover->isVisible());
  QCOMPARE(creationCount, 1);
  QVERIFY(contentExistedWhenShown);
  QPointer<QWidget> retainedContent = popover->contentWidget();
  QVERIFY(retainedContent);

  popover->hide();
  QTRY_VERIFY(!popover->isVisible());
  QCOMPARE(popover->contentWidget(), retainedContent.data());
  QVERIFY(retainedContent);

  popover->show();
  QTRY_VERIFY(popover->isVisible());
  QCOMPARE(creationCount, 1);
  QCOMPARE(popover->contentWidget(), retainedContent.data());

  delete popover;
  QTRY_VERIFY(retainedContent.isNull());
}

void QtToolPopupTest::recreateFactoryContentIsReleasedAfterHide() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);

  int creationCount = 0;
  popover.setContentFactory(
      [&creationCount]() {
        auto* content = new QWidget;
        content->setFixedSize(120, 48);
        content->setProperty("factoryGeneration", ++creationCount);
        auto* button = new QPushButton(QStringLiteral("Option"), content);
        button->setGeometry(8, 8, 80, 28);
        return content;
      },
      AdPopover::FactoryContentLifetime::RecreateOnOpen);
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCOMPARE(creationCount, 1);
  QPointer<QWidget> firstContent = popover.contentWidget();
  QPointer<QPushButton> firstButton = firstContent->findChild<QPushButton*>();
  QVERIFY(firstContent && firstButton);

  popover.hide();
  QTRY_VERIFY(!popover.isVisible());
  QCOMPARE(popover.contentWidget(), nullptr);
  QTRY_VERIFY(firstContent.isNull());
  QTRY_VERIFY(firstButton.isNull());

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCOMPARE(creationCount, 2);
  QVERIFY(popover.contentWidget());
  QCOMPARE(popover.contentWidget()->property("factoryGeneration").toInt(), 2);
  QVERIFY(popover.contentWidget()->findChild<QPushButton*>());
}

void QtToolPopupTest::directContentRemainsCompatibleWithFactoryApi() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setContentFactory([]() {
    auto* content = new QWidget;
    content->setFixedSize(120, 48);
    return content;
  });
  popover.setContentWidget(nullptr);
  QVERIFY(!popover.contentFactory());
  popover.setContentFactory([]() {
    auto* content = new QWidget;
    content->setFixedSize(120, 48);
    return content;
  });

  auto* directContent = new QWidget;
  directContent->setFixedSize(140, 52);
  popover.setContentWidget(directContent);
  QVERIFY(!popover.contentFactory());
  QCOMPARE(popover.contentWidget(), directContent);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  for (int cycle = 0; cycle < 2; ++cycle) {
    popover.show();
    QTRY_VERIFY(popover.isVisible());
    QCOMPARE(popover.contentWidget(), directContent);
    popover.hide();
    QTRY_VERIFY(!popover.isVisible());
    QCOMPARE(popover.contentWidget(), directContent);
  }
}

void QtToolPopupTest::colorPickerPrewarmCanBeDisabled() {
  const auto pickerPanelCount = []() {
    const QWidgetList widgets = QApplication::allWidgets();
    return static_cast<int>(std::count_if(widgets.cbegin(), widgets.cend(), [](QWidget* widget) {
      return widget && widget->objectName() == QStringLiteral("ad-color-picker-picker-panel");
    }));
  };
  const int initialPickerPanelCount = pickerPanelCount();
  QWidget host;
  host.resize(640, 360);
  auto* defaultPicker = new AdColorPicker(&host);
  defaultPicker->setGeometry(24, 24, 120, 32);
  auto* explicitPicker = new AdColorPicker(&host);
  explicitPicker->setGeometry(24, 80, 120, 32);
  explicitPicker->setPopupPrewarmEnabled(false);
  auto* hoverPicker = new AdColorPicker(&host);
  hoverPicker->setGeometry(24, 136, 120, 32);
  hoverPicker->setPopupPrewarmEnabled(false);
  hoverPicker->setTrigger(AdColorPicker::Trigger::Hover);

  QCOMPARE(defaultPicker->popupPrewarmEnabled(), true);
  QCOMPARE(explicitPicker->popupPrewarmEnabled(), false);
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  QTRY_COMPARE(pickerPanelCount(), initialPickerPanelCount + 1);
  QTest::qWait(100);
  QCOMPARE(pickerPanelCount(), initialPickerPanelCount + 1);

  explicitPicker->setPopupVisible(true);
  QTRY_VERIFY(explicitPicker->popupVisible());
  QCOMPARE(pickerPanelCount(), initialPickerPanelCount + 2);
  explicitPicker->setPopupVisible(false);
  QTRY_VERIFY(!explicitPicker->popupVisible());

  QWidget* hoverTrigger =
      hoverPicker->findChild<QWidget*>(QStringLiteral("ad-color-picker-trigger-frame"));
  QVERIFY(hoverTrigger);
  QTest::mouseMove(&host, QPoint(400, 280));
  QTest::mouseMove(hoverTrigger, hoverTrigger->rect().center());
  QTRY_VERIFY(hoverPicker->popupVisible());
  QCOMPARE(pickerPanelCount(), initialPickerPanelCount + 3);
}

QTEST_MAIN(QtToolPopupTest)

#include "qt_tool_popup_tests.moc"
