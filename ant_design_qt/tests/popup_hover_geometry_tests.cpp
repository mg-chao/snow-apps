#include "widgets/popover.h"
#include "widgets/date_picker.h"
#include "widgets/navigation_menu.h"
#include "widgets/select.h"
#include "widgets/detail/overlay_popup_surface.h"
#include "widgets/tooltip.h"
#include "widgets/detail/popup_geometry.h"
#include "widgets/popup_interaction_host.h"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QStandardItemModel>
#include <QListView>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

using namespace adqt::widgets::detail;

class TestPopupOwner final : public PopupInteractionOwner {
 public:
  QWidget* anchor;
  QWidget* scope;
  QWidget* surface;
  bool open = true;
  int closes = 0;
  TestPopupOwner(QWidget* anchor, QWidget* scope, QWidget* surface)
      : anchor(anchor), scope(scope), surface(surface) {}
  ~TestPopupOwner() override { setPopupInteractionHostOpen(this, false); }
  QObject* popupOwnerObject() const override { return anchor; }
  QWidget* popupAnchorWidget() const override { return anchor; }
  QWidget* popupScopeWindow() const override { return scope; }
  QWidget* popupSurfaceWidget() const override { return surface; }
  bool popupIsVisible() const override { return open; }
  bool popupContainsGlobalPos(const QPoint& point) const override {
    return widgetContainsGlobalPos(anchor, point) || widgetContainsGlobalPos(surface, point);
  }
  void popupCloseFromHost(PopupCloseReason) override {
    open = false;
    ++closes;
  }
  void popupRelayoutFromHost() override {}
};

void verifyGeometryAndOwnership(QScreen* screen) {
  QWidget owner;
  owner.setGeometry(400, 400, 100, 100);
  owner.show();
  QWidget window(&owner, Qt::Tool | Qt::FramelessWindowHint);
  window.setScreen(screen);
  window.setGeometry(-80, 200, 700, 100);
  QWidget panel(&window);
  panel.setGeometry(0, 0, 600, 100);
  QPushButton trigger(&panel);
  trigger.setGeometry(20, 10, 140, 40);
  window.show();
  // Qt selects a screen while creating the platform window. Set the source
  // screen afterward to exercise both DPR directions at the same seam.
  window.setScreen(screen);
  QCoreApplication::processEvents();
  require(qFuzzyCompare(window.devicePixelRatioF(), screen->devicePixelRatio()),
          "fixture must retain the requested source window scale");
  const auto visible = PopupWidgetRect::whole(&trigger).visible();
  require(visible.rect == trigger.rect(), "a tool window must not be clipped by its QObject owner");
  for (const QPoint point : {QPoint(8, 8), QPoint(120, 8), QPoint(120, 30)}) {
    require(visible.containsGlobalPos(trigger.mapToGlobal(point)),
            "both sides of a control spanning different display scales must be interactive");
    require(widgetContainsGlobalPos(&window, trigger.mapToGlobal(point)),
            "popup host containment must use its own local coordinates");
  }
  require(!visible.containsGlobalPos(trigger.mapToGlobal(QPoint(150, 20))),
          "a point outside the trigger must stay outside");
  const auto placement = visible.onScreen();
  require(placement.screen, "placement must retain its selected screen");
  // A real window on the selected screen must see the same physical anchor size.
  QWidget destination(nullptr, Qt::Tool | Qt::FramelessWindowHint);
  destination.setScreen(placement.screen);
  destination.setGeometry(
      QRect(placement.screen->geometry().topLeft() + QPoint(200, 100), QSize(300, 100)));
  destination.show();
  QCoreApplication::processEvents();
  const QRect mapped = visible.mappedTo(&destination);
  require(qAbs(mapped.width() * destination.devicePixelRatioF() -
               trigger.width() * trigger.devicePixelRatioF()) <= 2,
          "cross-window mapping must preserve physical anchor width");
  require(qAbs(placement.rect.width() * placement.screen->devicePixelRatio() -
               trigger.width() * trigger.devicePixelRatioF()) <= 2,
          "screen placement must preserve physical anchor width");

  // Nested owners stay open, and an in-scope press outside both closes the chain.
  QWidget surface(&window, Qt::Tool | Qt::FramelessWindowHint);
  surface.setScreen(screen);
  surface.setGeometry(-80, 350, 400, 100);
  QWidget nestedAnchor(&surface);
  nestedAnchor.setGeometry(20, 10, 140, 40);
  surface.show();
  surface.setScreen(screen);
  QCoreApplication::processEvents();
  TestPopupOwner parent(&trigger, &window, &surface);
  TestPopupOwner child(&nestedAnchor, &window, nullptr);
  setPopupInteractionHostOpen(&parent, true);
  setPopupInteractionHostOpen(&child, true);
  require(parent.open && child.open, "opening a nested popup must preserve its parent owner");
  const QPoint pressLocal(500, 50);
  QMouseEvent press(QEvent::MouseButtonPress, pressLocal, window.mapToGlobal(pressLocal),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&window, &press);
  require(!parent.open && !child.open && parent.closes == 1 && child.closes == 1,
          "a visible in-scope outside press must dismiss the entire nested popup chain");

  // Exercise small overlay anchors on either side of the boundary (the same
  // projection used by the Windows-only isolated busy indicator).
  for (const QRect iconRect : {QRect(40, 10, 16, 16), QRect(120, 10, 16, 16)}) {
    const auto icon = PopupWidgetRect{&panel, iconRect}.onScreen();
    QWidget overlay(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    overlay.setScreen(icon.screen);
    overlay.setGeometry(icon.rect);
    overlay.show();
    QCoreApplication::processEvents();
    const QPoint iconCenter = panel.mapFromGlobal(overlay.mapToGlobal(overlay.rect().center()));
    require((iconCenter - iconRect.center()).manhattanLength() <= 3,
            "small overlay must stay physically centered over its local anchor");
  }

  adqt::widgets::AdDateRangePicker picker(&panel);
  picker.setGeometry(0, 10, 260, 32);
  picker.setPopupLayerMode(adqt::widgets::AdDateRangePicker::PopupLayerMode::QtTool);
  picker.show();
  picker.showPopup();
  QCoreApplication::processEvents();
  OverlayPopupSurface* rangeSurface = nullptr;
  for (QWidget* candidate : QApplication::allWidgets()) {
    if (candidate->isVisible() &&
        candidate->objectName() == QStringLiteral("addaterangepicker-popup")) {
      rangeSurface = dynamic_cast<OverlayPopupSurface*>(candidate);
    }
  }
  require(rangeSurface && rangeSurface->arrowVisible(), "range picker must expose its popup arrow");
  const QPoint arrowInPopup(
      qRound(rangeSurface->arrowCenter()) + rangeSurface->shadowMargins().left(),
      rangeSurface->shadowMargins().top());
  const int arrowInPicker = picker.mapFromGlobal(rangeSurface->mapToGlobal(arrowInPopup)).x();
  require(
      arrowInPicker > 10 && arrowInPicker < picker.width() / 2,
      "date range popup arrow must point to the active start field in the picker's coordinates");
  picker.hidePopup();

  adqt::widgets::AdSelect select(&panel);
  select.setGeometry(20, 10, 140, 40);
  select.setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::QtTool);
  adqt::widgets::AdSelect::Option option;
  option.value = QStringLiteral("item");
  option.label = QStringLiteral("Item");
  select.setOptions({option});
  select.show();
  select.showPopup();
  QCoreApplication::processEvents();
  const QPoint selectLocal = select.mapTo(&window, QPoint(8, 8));
  QMouseEvent selectPress(QEvent::MouseButtonPress, selectLocal, window.mapToGlobal(selectLocal),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&window, &selectPress);
  require(select.view()->isVisible(),
          "select ownership must retain a press within its visible trigger");
  select.hidePopup();

  adqt::widgets::AdNavigationMenu menu(&panel);
  QStandardItemModel model;
  auto* group = new QStandardItem(QStringLiteral("Group"));
  group->appendRow(new QStandardItem(QStringLiteral("Child")));
  model.appendRow(group);
  menu.setModel(&model);
  menu.setGeometry(20, 10, 140, 80);
  menu.show();
  const QModelIndex groupIndex = model.index(0, 0);
  menu.setExpanded(groupIndex, true);
  QCoreApplication::processEvents();
  require(menu.isExpanded(groupIndex), "navigation submenu must open");
  const QPoint menuLocal = menu.mapTo(&window, QPoint(8, 8));
  QMouseEvent menuPress(QEvent::MouseButtonPress, menuLocal, window.mapToGlobal(menuLocal),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&window, &menuPress);
  require(menu.isExpanded(groupIndex), "navigation ownership must retain a press inside its menu");
  menu.collapseAll();

  panel.resize(80, 100);
  const auto clipped = PopupWidgetRect::whole(&trigger).visible();
  require(clipped.rect.width() == 60, "ancestor clipping must be computed in local coordinates");
  require(!clipped.containsGlobalPos(trigger.mapToGlobal(QPoint(100, 20))),
          "clipped trigger pixels must not accept hover");
  panel.hide();
  require(!PopupWidgetRect::whole(&trigger).visible().rect.isValid(),
          "hidden ancestors must reject popup anchors");
}

}  // namespace

int main(int argc, char** argv) {
  QTemporaryDir directory;
  require(directory.isValid(), "create offscreen display configuration directory");
  const QString configPath = directory.filePath(QStringLiteral("screens.json"));
  QFile config(configPath);
  require(config.open(QIODevice::WriteOnly), "open offscreen display configuration");
  config.write(R"({"windowFrameMargins":false,"screens":[
    {"name":"primary","x":0,"y":0,"width":3840,"height":2160,"logicalDpi":144},
    {"name":"left","x":-1920,"y":0,"width":1920,"height":2160,"logicalDpi":96}
  ]})");
  config.close();
  // QPA separates platform arguments at colons, including Windows drive letters.
  const QString previousDirectory = QDir::currentPath();
  require(QDir::setCurrent(directory.path()), "select offscreen configuration directory");
  qputenv("QT_QPA_PLATFORM", "offscreen:configfile=screens.json");
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
      Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  QApplication app(argc, argv);
  require(QDir::setCurrent(previousDirectory), "restore test working directory");
  QScreen* primary = nullptr;
  for (auto* screen : app.screens()) {
    if (screen->name() == QStringLiteral("primary")) {
      primary = screen;
    }
  }
  require(primary && qFuzzyCompare(primary->devicePixelRatio(), 1.5),
          "simulated primary display must use 150 percent scaling");

  for (auto* screen : app.screens()) {
    verifyGeometryAndOwnership(screen);
  }

  QWidget window(nullptr, Qt::Tool | Qt::FramelessWindowHint);
  window.setScreen(primary);
  window.setGeometry(-80, 1053, 1242, 142);
  QWidget host(&window);
  host.setGeometry(0, 0, 1242, 142);
  QWidget palette(&host);
  palette.setGeometry(351, 0, 891, 138);
  QWidget mainPanel(&palette);
  mainPanel.setGeometry(24, 24, 843, 40);
  QWidget subPanel(&palette);
  subPanel.setGeometry(24, 70, 843, 40);
  QPushButton mainTrigger(QStringLiteral("Main"), &mainPanel);
  QPushButton subTrigger(QStringLiteral("Sub"), &subPanel);
  mainTrigger.setGeometry(565, 4, 32, 32);
  subTrigger.setGeometry(565, 4, 32, 32);
  adqt::widgets::AdPopover mainPopup, subPopup;
  mainPopup.setSourceWidget(&mainTrigger);
  subPopup.setSourceWidget(&subTrigger);
  for (auto* popup : {&mainPopup, &subPopup}) {
    popup->setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::QtTool);
    popup->setTriggers(adqt::widgets::AdPopover::Trigger::Hover);
    popup->setHoverOpenDelayMs(10);
    popup->setHoverCloseDelayMs(10);
    auto* content = new QWidget;
    content->setFixedSize(100, 30);
    popup->setContentWidget(content);
  }
  window.show();
  require(QTest::qWaitForWindowExposed(&window), "show simulated toolbar window");

  const QRect hostGlobal(host.mapToGlobal(QPoint()), host.size());
  const QRect triggerGlobal(mainTrigger.mapToGlobal(QPoint()), mainTrigger.size());
  require(!hostGlobal.intersects(triggerGlobal),
          "fixture must reproduce disjoint global rectangles across different display scales");

  for (auto* popup : {&mainPopup, &subPopup, &mainPopup}) {
    auto* trigger = popup->sourceWidget();
    const QPoint center = trigger->rect().center();
    const QPoint global = trigger->mapToGlobal(center);
    QCursor::setPos(global);
    QEnterEvent enter(center, trigger->mapTo(&window, center), global);
    QApplication::sendEvent(trigger, &enter);
    require(QTest::qWaitFor([&]() { return popup->isVisible(); }, 1000),
            "hover must open main and sub-tool popovers across a display scale boundary");
    require(popup->contentWidget()->window()->isVisible(),
            "popover surface must actually be visible");
  }
  mainPopup.hide();
  subPopup.hide();
  adqt::widgets::AdTooltip tooltip;
  tooltip.setTargetWidget(&mainTrigger);
  tooltip.setLayerMode(adqt::widgets::AdTooltip::LayerMode::TopLevelTransient);
  tooltip.setText(QStringLiteral("Mixed DPI sub-rectangle"));
  tooltip.setTriggerRect(QRect(4, 4, 12, 12));
  tooltip.setAnchorRect(QRect(4, 4, 12, 12));
  tooltip.setHoverOpenDelayMs(10);
  const QPoint tipPoint(8, 8);
  QCursor::setPos(mainTrigger.mapToGlobal(tipPoint));
  QEnterEvent tipEnter(tipPoint, mainTrigger.mapTo(&window, tipPoint),
                       mainTrigger.mapToGlobal(tipPoint));
  QApplication::sendEvent(&mainTrigger, &tipEnter);
  require(QTest::qWaitFor([&] { return tooltip.isVisible(); }, 1000),
          "tooltip sub-rectangles must open across mixed-DPI ancestor mappings");
  tooltip.hide();
  tooltip.setActivationMode(adqt::widgets::AdTooltip::ActivationMode::Manual);
  tooltip.show();
  QCoreApplication::processEvents();
  QWidget* tipSurface = nullptr;
  for (auto* candidate : QApplication::allWidgets()) {
    if (candidate->isVisible() && candidate->objectName() == QStringLiteral("adtooltip-surface")) {
      tipSurface = candidate;
    }
  }
  require(tipSurface, "manual transient tooltip must expose a visible surface");
  const QRect capturedGeometry = tipSurface->geometry();
  const QPoint oldTriggerPosition = mainTrigger.pos();
  mainTrigger.move(oldTriggerPosition + QPoint(20, 0));
  QTest::qWait(30);
  require(tipSurface->geometry() == capturedGeometry,
          "transient tooltip placement must retain its captured anchor until explicitly updated");
  tooltip.setAnchorRect(tooltip.anchorRect());
  QCoreApplication::processEvents();
  require(tipSurface->geometry().topLeft() != capturedGeometry.topLeft(),
          "explicit anchor refresh must recapture transient tooltip placement");
  tooltip.hide();
  mainTrigger.move(oldTriggerPosition);
  tooltip.setEnabled(false);
  mainPopup.hide();
  subPopup.hide();
  // Local clipping remains necessary: an off-panel trigger must not open a popup.
  mainTrigger.move(mainPanel.width() + 10, 4);
  const QPoint center = mainTrigger.rect().center();
  const QPoint global = mainTrigger.mapToGlobal(center);
  QCursor::setPos(global);
  QEnterEvent enter(center, mainTrigger.mapTo(&window, center), global);
  QApplication::sendEvent(&mainTrigger, &enter);
  QTest::qWait(40);
  require(!mainPopup.isVisible(), "a locally clipped trigger must remain closed");
  adqt::widgets::AdTooltip recoveringTooltip;
  recoveringTooltip.setTargetWidget(&mainTrigger);
  recoveringTooltip.setLayerMode(adqt::widgets::AdTooltip::LayerMode::TopLevelTransient);
  recoveringTooltip.setActivationMode(adqt::widgets::AdTooltip::ActivationMode::Manual);
  recoveringTooltip.setText(QStringLiteral("Anchor returns to view"));
  recoveringTooltip.show();
  QCoreApplication::processEvents();
  require(!recoveringTooltip.isVisible(),
          "fully clipped anchor must not display a tooltip surface");
  mainTrigger.move(oldTriggerPosition);
  require(QTest::qWaitFor([&] { return recoveringTooltip.isVisible(); }, 1000),
          "an initially clipped transient anchor must recover when it returns to view");
  return 0;
}
