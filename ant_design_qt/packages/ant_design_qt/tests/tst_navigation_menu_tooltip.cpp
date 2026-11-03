#include <QAbstractItemView>
#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEvent>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>

#include "theme/theme_types.h"
#include "widgets/color_picker.h"
#include "widgets/menu_style.h"
#include "widgets/modal.h"
#include "widgets/navigation_menu.h"
#include "widgets/tooltip.h"

using adqt::widgets::AdColorPicker;
using adqt::widgets::AdModal;
using adqt::widgets::AdModalService;
using adqt::widgets::AdNavigationMenu;
using adqt::widgets::AdTooltip;

namespace {

constexpr auto kTooltipObjectName = "ad-menu-inline-collapsed-tooltip";
constexpr auto kTooltipSurfaceObjectName = "adtooltip-surface";
constexpr auto kPopoverSurfaceObjectName = "adpopover-surface";

QWidget* findVisiblePopoverSurface() {
  for (QWidget* candidate : QApplication::allWidgets()) {
    if (candidate && candidate->isVisible() &&
        candidate->objectName() == QString::fromLatin1(kPopoverSurfaceObjectName)) {
      return candidate;
    }
  }
  return nullptr;
}

int delayFromEnvironment(const char* name, int fallbackMs) {
  bool ok = false;
  const int value = qEnvironmentVariableIntValue(name, &ok);
  return ok && value >= 0 ? value : fallbackMs;
}

bool moveSystemCursor(QWidget* widget, const QPoint& localPosition) {
  if (!widget) {
    return false;
  }

  const QPoint globalPosition = widget->mapToGlobal(localPosition);
  QCursor::setPos(globalPosition);
  QTest::qWait(delayFromEnvironment("ADQT_TOOLTIP_MOVE_DELAY_MS", 80));
  return QCursor::pos() == globalPosition;
}

bool moveSystemCursorAlongLine(const QPoint& fromGlobal, const QPoint& toGlobal, int steps,
                               int stepDelayMs = 16) {
  const int normalizedSteps = qMax(1, steps);
  for (int step = 1; step <= normalizedSteps; ++step) {
    const QPoint position(
        fromGlobal.x() + (toGlobal.x() - fromGlobal.x()) * step / normalizedSteps,
        fromGlobal.y() + (toGlobal.y() - fromGlobal.y()) * step / normalizedSteps);
    QCursor::setPos(position);
    QTest::qWait(qMax(0, stepDelayMs));
  }
  return QCursor::pos() == toGlobal;
}

class ViewLeaveSpy final : public QObject {
 public:
  explicit ViewLeaveSpy(QObject* watched) : QObject(watched) {
    if (watched) {
      watched->installEventFilter(this);
    }
  }

  int leaveCount = 0;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event && event->type() == QEvent::Leave) {
      ++leaveCount;
    }
    return QObject::eventFilter(watched, event);
  }
};

class PopupSurfaceShowProbe final : public QObject {
 public:
  PopupSurfaceShowProbe() { qApp->installEventFilter(this); }

  ~PopupSurfaceShowProbe() override { qApp->removeEventFilter(this); }

  int polishCount = 0;
  int showCount = 0;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && event && widget->objectName() == QString::fromLatin1(kPopoverSurfaceObjectName)) {
      if (event->type() == QEvent::Polish) {
        ++polishCount;
      } else if (event->type() == QEvent::Show) {
        ++showCount;
      }
    }
    return QObject::eventFilter(watched, event);
  }
};

class MouseMoveProbe final : public QObject {
 public:
  explicit MouseMoveProbe(QWidget* watched) : QObject(watched) {
    if (watched) {
      watched->setMouseTracking(true);
      watched->installEventFilter(this);
    }
  }

  int mouseMoveCount = 0;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event && event->type() == QEvent::MouseMove) {
      ++mouseMoveCount;
    }
    return QObject::eventFilter(watched, event);
  }
};

class TooltipLifecycleProbe final : public QObject {
 public:
  TooltipLifecycleProbe() { qApp->installEventFilter(this); }

  ~TooltipLifecycleProbe() override { qApp->removeEventFilter(this); }

  void beginLeaveObservation() {
    observingLeave_ = true;
    emptyTooltipSeen_ = false;
    timeline_.clear();
    lastStateByWidget_.clear();
    sampleVisibleTooltips(QStringLiteral("before leave"));
  }

  void observeFor(int durationMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < durationMs) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
      sampleVisibleTooltips(QStringLiteral("poll"));
      QTest::qWait(5);
    }
  }

  bool emptyTooltipSeen() const { return emptyTooltipSeen_; }

  QString timeline() const { return timeline_.join(QLatin1Char('\n')); }

  void printCurrentState(const char* phase) {
    const QList<QWidget*> widgets = visibleTooltipWidgets();
    if (widgets.isEmpty()) {
      qInfo().noquote().nospace() << phase << " visible tooltip widgets: none";
      return;
    }
    for (QWidget* widget : widgets) {
      qInfo().noquote().nospace() << phase << " visible tooltip widget: class="
                                  << widget->metaObject()->className()
                                  << ", objectName=" << widget->objectName() << ", text=\""
                                  << tooltipText(widget) << "\""
                                  << ", geometry=" << widget->geometry();
    }
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (observingLeave_ && widget && isTooltipWidget(widget) && event) {
      switch (event->type()) {
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::Close:
        case QEvent::Paint:
        case QEvent::LayoutRequest:
          sampleWidget(widget,
                       QString::fromLatin1(event->type() == QEvent::Show    ? "Show"
                                           : event->type() == QEvent::Hide  ? "Hide"
                                           : event->type() == QEvent::Close ? "Close"
                                           : event->type() == QEvent::Paint ? "Paint"
                                                                            : "LayoutRequest"));
          break;
        default:
          break;
      }
    }
    return QObject::eventFilter(watched, event);
  }

 private:
  static bool isTooltipWidget(const QWidget* widget) {
    if (!widget) {
      return false;
    }
    return widget->objectName() == QString::fromLatin1(kTooltipSurfaceObjectName) ||
           widget->inherits("QTipLabel") || widget->windowType() == Qt::ToolTip;
  }

  static QString tooltipText(QWidget* widget) {
    if (!widget) {
      return {};
    }
    if (auto* label = qobject_cast<QLabel*>(widget)) {
      return label->text();
    }
    QStringList texts;
    const auto labels = widget->findChildren<QLabel*>();
    for (const QLabel* label : labels) {
      if (label && !label->text().isEmpty()) {
        texts.append(label->text());
      }
    }
    return texts.join(QStringLiteral(" | "));
  }

  static QList<QWidget*> visibleTooltipWidgets() {
    QList<QWidget*> result;
    const auto widgets = QApplication::allWidgets();
    for (QWidget* widget : widgets) {
      if (isTooltipWidget(widget) && widget->isVisible()) {
        result.append(widget);
      }
    }
    return result;
  }

  void sampleVisibleTooltips(const QString& cause) {
    const auto widgets = visibleTooltipWidgets();
    for (QWidget* widget : widgets) {
      sampleWidget(widget, cause);
    }
  }

  void sampleWidget(QWidget* widget, const QString& cause) {
    if (!widget) {
      return;
    }
    const QString text = tooltipText(widget).trimmed();
    const bool visible = widget->isVisible();
    const QString fingerprint =
        QStringLiteral("visible=%1 text=\"%2\"")
            .arg(visible ? QStringLiteral("true") : QStringLiteral("false"), text);
    const quintptr widgetKey = reinterpret_cast<quintptr>(widget);
    if (lastStateByWidget_.value(widgetKey) == fingerprint) {
      return;
    }
    lastStateByWidget_.insert(widgetKey, fingerprint);
    const QString state =
        QStringLiteral("%1: class=%2 objectName=%3 visible=%4 text=\"%5\"")
            .arg(cause, QString::fromLatin1(widget->metaObject()->className()),
                 widget->objectName(), visible ? QStringLiteral("true") : QStringLiteral("false"),
                 text);
    if (timeline_.isEmpty() || timeline_.constLast() != state) {
      timeline_.append(state);
    }
    if (visible && text.isEmpty()) {
      emptyTooltipSeen_ = true;
    }
  }

  bool observingLeave_ = false;
  bool emptyTooltipSeen_ = false;
  QStringList timeline_;
  QHash<quintptr, QString> lastStateByWidget_;
};

void printTooltipState(const char* phase, const AdTooltip* tooltip, const QWidget* surface) {
  const bool visible = tooltip && tooltip->isVisible();
  const QString text = tooltip ? tooltip->text() : QStringLiteral("<not-created>");
  const bool hasAnchorRect = tooltip && tooltip->hasAnchorRect();
  const bool surfaceVisible = surface && surface->isVisible();

  qInfo().noquote().nospace() << phase << " tooltip state: visible=" << (visible ? "true" : "false")
                              << ", text=\"" << text << "\""
                              << ", hasAnchorRect=" << (hasAnchorRect ? "true" : "false")
                              << ", surfaceVisible=" << (surfaceVisible ? "true" : "false");
}

}  // namespace

class NavigationMenuTooltipTest final : public QObject {
  Q_OBJECT

 private slots:
  void colorPickerCreationDoesNotShowPopup_data();
  void colorPickerCreationDoesNotShowPopup();
  void colorPickerQtToolKeepsParentInteractive();
  void colorPickerQtToolChangesColorFromPanelClick();
  void colorPickerQtToolSurvivesOwnerDestructionWhileOpen();
  void menuIconsUseLargeThemeMetric();
  void collapsedInlineLeafDoesNotReopenAsEmptyTooltipOnMouseLeave();
  void modalCanRenderDirectlyIntoContainer();
  void windowModeConfirmRespectsPanelCloseButtonSetting();
};

void NavigationMenuTooltipTest::colorPickerCreationDoesNotShowPopup_data() {
  QTest::addColumn<int>("popupLayerMode");
  QTest::newRow("in-window") << static_cast<int>(AdColorPicker::PopupLayerMode::InWindow);
  QTest::newRow("qt-tool") << static_cast<int>(AdColorPicker::PopupLayerMode::QtTool);
}

void NavigationMenuTooltipTest::colorPickerCreationDoesNotShowPopup() {
  QFETCH(int, popupLayerMode);

  PopupSurfaceShowProbe popupProbe;
  QWidget window;
  auto* layout = new QVBoxLayout(&window);
  auto* picker = new AdColorPicker(&window);
  picker->setPopupLayerMode(static_cast<AdColorPicker::PopupLayerMode>(popupLayerMode));
  layout->addWidget(picker);

  QSignalSpy popupVisibleSpy(picker, &AdColorPicker::popupVisibleChanged);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  // AdColorPicker schedules editor prewarming after its first show event.
  QTRY_VERIFY(popupProbe.polishCount > 0);
  QVERIFY(!picker->popupVisible());
  QCOMPARE(popupVisibleSpy.count(), 0);
  QCOMPARE(popupProbe.showCount, 0);

  picker->setPopupVisible(true);
  QTRY_VERIFY(picker->popupVisible());
  QTRY_VERIFY(popupProbe.showCount > 0);
  picker->setPopupVisible(false);
}

void NavigationMenuTooltipTest::colorPickerQtToolKeepsParentInteractive() {
  QWidget window;
  window.setWindowTitle(QStringLiteral("Color picker tool-layer interaction test"));
  window.resize(720, 480);

  auto* picker = new AdColorPicker(&window);
  picker->setPopupLayerMode(AdColorPicker::PopupLayerMode::QtTool);
  picker->move(24, 24);
  picker->resize(picker->sizeHint());

  auto* parentButton = new QPushButton(QStringLiteral("Parent action"), &window);
  parentButton->setGeometry(550, 410, 140, 36);
  MouseMoveProbe mouseMoveProbe(parentButton);
  QSignalSpy clickedSpy(parentButton, &QPushButton::clicked);

  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));
  window.raise();
  window.activateWindow();
  QTRY_VERIFY(window.isActiveWindow());

  picker->setPopupVisible(true);
  QTRY_VERIFY(picker->popupVisible());
  QWidget* surface = findVisiblePopoverSurface();
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QCOMPARE(surface->windowType(), Qt::Tool);
  QVERIFY(surface->isWindow());
  QVERIFY(!surface->parentWidget());
  QVERIFY(surface->testAttribute(Qt::WA_ShowWithoutActivating));
  QVERIFY(window.isActiveWindow());
  QVERIFY(surface->windowHandle());
  QCOMPARE(surface->windowHandle()->transientParent(), window.windowHandle());
  QVERIFY(QApplication::activePopupWidget() != surface);
  QVERIFY(QWidget::mouseGrabber() != surface);

  const QPoint parentButtonCenter = parentButton->rect().center();
  const QPoint parentButtonGlobalCenter = parentButton->mapToGlobal(parentButtonCenter);
  const QPoint parentButtonWindowCenter = window.mapFromGlobal(parentButtonGlobalCenter);
  QMouseEvent parentMoveEvent(QEvent::MouseMove, parentButtonCenter, parentButtonWindowCenter,
                              parentButtonGlobalCenter, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(parentButton, &parentMoveEvent);
  QCOMPARE(mouseMoveProbe.mouseMoveCount, 1);
  QVERIFY(picker->popupVisible());

  QLineEdit* editor = surface->findChild<QLineEdit*>(QStringLiteral("ad-color-picker-hex-input"));
  QVERIFY2(editor, "The color picker tool surface should contain its hex editor");
  QVERIFY(editor->isVisibleTo(surface));
  surface->activateWindow();
  QTRY_VERIFY_WITH_TIMEOUT(surface->isActiveWindow(), 10000);
  QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, editor->rect().center());
  QTRY_VERIFY(editor->hasFocus());
  QVERIFY(picker->popupVisible());

  QTest::mouseClick(parentButton, Qt::LeftButton, Qt::NoModifier, parentButton->rect().center());
  QTRY_COMPARE(clickedSpy.count(), 1);
  QTRY_VERIFY(!picker->popupVisible());
  QVERIFY(!surface->isVisible());
}

void NavigationMenuTooltipTest::colorPickerQtToolChangesColorFromPanelClick() {
  QWidget window;
  window.resize(720, 480);

  auto* picker = new AdColorPicker(&window);
  picker->setPopupLayerMode(AdColorPicker::PopupLayerMode::QtTool);
  picker->move(24, 24);
  picker->resize(picker->sizeHint());
  const QString initialCssText = picker->cssText();
  QSignalSpy valueChangedSpy(picker, &AdColorPicker::valueChanged);

  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  picker->setPopupVisible(true);
  QTRY_VERIFY(picker->popupVisible());
  QWidget* surface = findVisiblePopoverSurface();
  QVERIFY(surface);

  QWidget* saturationPanel =
      surface->findChild<QWidget*>(QStringLiteral("ad-color-picker-saturation-panel"));
  QVERIFY(saturationPanel);
  QVERIFY(saturationPanel->isVisibleTo(surface));

  const QPoint selectionPoint(saturationPanel->width() / 4, saturationPanel->height() * 3 / 4);
  QTest::mouseClick(saturationPanel, Qt::LeftButton, Qt::NoModifier, selectionPoint);

  QTRY_VERIFY(valueChangedSpy.count() > 0);
  QTRY_VERIFY(picker->cssText() != initialCssText);
  QVERIFY(picker->popupVisible());
}

void NavigationMenuTooltipTest::colorPickerQtToolSurvivesOwnerDestructionWhileOpen() {
  auto* window = new QWidget;
  window->resize(720, 480);

  auto* picker = new AdColorPicker(window);
  picker->setPopupLayerMode(AdColorPicker::PopupLayerMode::QtTool);
  picker->move(24, 24);
  picker->resize(picker->sizeHint());

  window->show();
  QVERIFY(QTest::qWaitForWindowExposed(window));
  window->raise();
  window->activateWindow();
  QTRY_VERIFY(window->isActiveWindow());

  picker->setPopupVisible(true);
  QTRY_VERIFY(picker->popupVisible());
  QPointer<QWidget> surface = findVisiblePopoverSurface();
  QVERIFY(surface);

  QLineEdit* editor = surface->findChild<QLineEdit*>(QStringLiteral("ad-color-picker-hex-input"));
  QVERIFY(editor);
  surface->activateWindow();
  QTRY_VERIFY_WITH_TIMEOUT(surface->isActiveWindow(), 10000);
  QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, editor->rect().center());
  QTRY_VERIFY(editor->hasFocus());

  delete window;
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QTRY_VERIFY(surface.isNull());
}

void NavigationMenuTooltipTest::menuIconsUseLargeThemeMetric() {
  for (const double fontSize : {14.0, 18.0}) {
    adqt::theme::ThemeConfig config;
    config.fontSize = fontSize;
    const auto resolvedTheme = adqt::theme::makeResolvedTheme(config);
    const adqt::widgets::detail::MenuVisualStyle style =
        adqt::widgets::detail::resolveMenuVisualStyle({}, resolvedTheme);

    const int expectedIconSize = std::max(12, qRound(resolvedTheme.values.fontSizeLG));
    QCOMPARE(style.metrics.iconSize, expectedIconSize);
    QCOMPARE(style.metrics.collapsedIconSize, expectedIconSize);
  }
}

void NavigationMenuTooltipTest::collapsedInlineLeafDoesNotReopenAsEmptyTooltipOnMouseLeave() {
  TooltipLifecycleProbe tooltipProbe;
  QWidget window;
  window.setWindowTitle(QStringLiteral("Navigation menu tooltip test"));
  window.resize(480, 280);

  auto* layout = new QVBoxLayout(&window);
  auto* menu = new AdNavigationMenu(&window);
  menu->setMode(AdNavigationMenu::Mode::Inline);
  menu->setCollapsed(true);
  menu->setTooltipEnabled(true);
  menu->setFixedWidth(menu->sizeHint().width());

  auto* model = new QStandardItemModel(menu);
  auto* leafItem = new QStandardItem(QStringLiteral("Dashboard"));
  leafItem->setData(QStringLiteral("dashboard"), AdNavigationMenu::StableIdRole);
  leafItem->setData(static_cast<int>(AdNavigationMenu::NodeKind::Action),
                    AdNavigationMenu::NodeKindRole);
  leafItem->setData(QStringLiteral("Dashboard tooltip"), Qt::ToolTipRole);
  model->appendRow(leafItem);
  menu->setModel(model);

  layout->addWidget(menu, 0, Qt::AlignLeft);
  layout->addStretch();

  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));
  QTest::qWait(delayFromEnvironment("ADQT_TOOLTIP_START_DELAY_MS", 0));

  const QModelIndex leafIndex = model->index(0, 0);
  QVERIFY(leafIndex.isValid());
  QCOMPARE(model->rowCount(leafIndex), 0);

  auto* view = menu->findChild<QTreeView*>(QStringLiteral("AdNavigationMenu-vertical-view"));
  QVERIFY(view);
  QVERIFY(view->viewport());
  QTRY_VERIFY(view->visualRect(leafIndex).isValid());
  const QRect leafRect = view->visualRect(leafIndex);

  QSignalSpy enteredSpy(view, &QAbstractItemView::entered);
  ViewLeaveSpy leaveSpy(view->viewport());

  const QPoint outsideMenu(window.width() - 12, window.height() - 12);
  QVERIFY2(moveSystemCursor(&window, outsideMenu),
           "The native cursor could not be moved outside the menu.");

  QVERIFY2(moveSystemCursor(view->viewport(), leafRect.center()),
           "The native cursor could not be moved onto the first-level leaf item.");
  QTRY_VERIFY_WITH_TIMEOUT(enteredSpy.count() > 0, 1000);
  QCOMPARE(qvariant_cast<QModelIndex>(enteredSpy.at(0).at(0)), leafIndex);
  QCOMPARE(leaveSpy.leaveCount, 0);

  AdTooltip* tooltip = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT(
      (tooltip = menu->findChild<AdTooltip*>(QString::fromLatin1(kTooltipObjectName))) != nullptr,
      1000);
  QTRY_VERIFY_WITH_TIMEOUT(tooltip->isVisible(), 1000);

  QWidget* surface = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((surface = window.findChild<QWidget*>(
                                QString::fromLatin1(kTooltipSurfaceObjectName))) != nullptr,
                           1000);
  QTRY_VERIFY_WITH_TIMEOUT(surface->isVisible(), 1000);

  printTooltipState("after mouse enter", tooltip, surface);
  QTest::qWait(delayFromEnvironment("ADQT_TOOLTIP_DWELL_MS", 100));
  tooltipProbe.printCurrentState("before mouse leave");
  QCOMPARE(tooltip->text(), QStringLiteral("Dashboard tooltip"));
  QCOMPARE(tooltip->anchorWidget(), view->viewport());
  QCOMPARE(tooltip->anchorRect(), leafRect.intersected(view->viewport()->rect()));

  tooltipProbe.beginLeaveObservation();
  // Follow the natural rightward exit path through the popup instead of
  // teleporting directly to a distant point outside both widgets.
  const QPoint cursorOnItem = view->viewport()->mapToGlobal(leafRect.center());
  const QPoint cursorAcrossTooltip = surface->mapToGlobal(surface->rect().center());
  QVERIFY2(moveSystemCursorAlongLine(cursorOnItem, cursorAcrossTooltip, 12),
           "The native cursor could not be moved through the tooltip area.");
  QVERIFY2(moveSystemCursor(&window, outsideMenu),
           "The native cursor could not be moved from the tooltip area to the window.");
  QTRY_VERIFY_WITH_TIMEOUT(leaveSpy.leaveCount > 0, 1000);
  tooltipProbe.observeFor(delayFromEnvironment("ADQT_TOOLTIP_OBSERVE_MS", 500));
  printTooltipState("after mouse leave", tooltip, surface);
  qInfo().noquote() << "tooltip lifecycle after mouse leave:\n" + tooltipProbe.timeline();
  QVERIFY2(!tooltipProbe.emptyTooltipSeen(),
           qPrintable(QStringLiteral("Captured a visible empty tooltip after leaving the menu.\n%1")
                          .arg(tooltipProbe.timeline())));
  QVERIFY(!tooltip->isVisible());
  QCOMPARE(tooltip->text(), QString());
  QVERIFY(!tooltip->hasAnchorRect());
  QVERIFY(!surface->isVisible());
}

void NavigationMenuTooltipTest::windowModeConfirmRespectsPanelCloseButtonSetting() {
  QWidget owner;
  owner.resize(640, 480);
  owner.show();
  QVERIFY(QTest::qWaitForWindowExposed(&owner));

  AdModalService::Request request;
  request.mode = AdModal::Mode::Window;
  AdModal* modal = AdModalService::showConfirm(request, &owner);
  QVERIFY(modal);
  QVERIFY(modal->closeButton());
  QTRY_VERIFY_WITH_TIMEOUT(!modal->closeButton()->isVisible(), 1000);

  request.closeButtonVisible = true;
  AdModal* closableModal = AdModalService::showConfirm(request, &owner);
  QVERIFY(closableModal);
  QVERIFY(closableModal->closeButton());
  QTRY_VERIFY_WITH_TIMEOUT(closableModal->closeButton()->isVisible(), 1000);
  closableModal->close();
  modal->close();
  QCoreApplication::processEvents();
  AdModalService::closeAll();
}

void NavigationMenuTooltipTest::modalCanRenderDirectlyIntoContainer() {
  QWidget owner;
  owner.resize(640, 480);
  QWidget container(&owner);
  container.setGeometry(20, 30, 360, 300);
  owner.show();
  QVERIFY(QTest::qWaitForWindowExposed(&owner));

  AdModal modal(&owner);
  modal.setRenderContainer(&container);
  modal.setMode(AdModal::Mode::Window);
  modal.setMaskVisible(false);
  modal.setContentWidget(new QLabel(QStringLiteral("Container content")));
  modal.open();

  QCOMPARE(modal.renderContainer(), &container);
  QWidget* surface =
      container.findChild<QWidget*>(QStringLiteral("ad-modal-overlay"), Qt::FindDirectChildrenOnly);
  QVERIFY(surface);
  QCOMPARE(surface->parentWidget(), &container);
  QVERIFY(!surface->isWindow());
  QCOMPARE(surface->geometry(), container.rect());

  container.resize(420, 340);
  QTRY_COMPARE(surface->geometry(), container.rect());
  modal.close();
}

QTEST_MAIN(NavigationMenuTooltipTest)

#include "tst_navigation_menu_tooltip.moc"
