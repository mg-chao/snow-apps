#include <QAbstractButton>
#include <QDir>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPainter>
#include <QSet>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtTest>

#include "antd_icons.h"
#include "theme/theme.h"
#include "widgets/tabs.h"
#include "widgets/popover.h"

#include <algorithm>

using adqt::widgets::AdTabs;

namespace {

QAbstractButton* tabButton(AdTabs* tabs, const QString& name) {
  const QList<QAbstractButton*> buttons = tabs->findChildren<QAbstractButton*>();
  for (QAbstractButton* button : buttons) {
    if (button->accessibleName() == name &&
        button->accessibleDescription().contains("tab", Qt::CaseInsensitive)) {
      return button;
    }
  }
  return nullptr;
}

QAbstractButton* operationButton(AdTabs* tabs, const QString& name) {
  const QList<QAbstractButton*> buttons = tabs->findChildren<QAbstractButton*>();
  for (QAbstractButton* button : buttons) {
    if (button->accessibleName() == name) {
      return button;
    }
  }
  return nullptr;
}

}  // namespace

class TabsTest final : public QObject {
  Q_OBJECT

 private slots:
  void firstEnabledTabBecomesCurrent();
  void keysStayStableAcrossInsertionAndRemoval();
  void disabledTabsCannotBecomeCurrent();
  void takeTabTransfersOwnership();
  void externallyDestroyedPageIsRemoved();
  void invalidPageHierarchiesAreRejected();
  void editableCardEmitsAddAndCloseRequests();
  void arrowKeysSelectTheNextEnabledTab();
  void selectionUsesStandardButtonState();
  void propertiesRoundTrip();
  void sizeHintsFollowContentAndTokens();
  void extraContentTracksQObjectLifetime();
  void horizontalTabsKeepNaturalWidths_data();
  void horizontalTabsKeepNaturalWidths();
  void overflowScrollsTabs_data();
  void overflowScrollsTabs();
  void overflowMenuTracksScrolledTabs();
  void oversizedTabsKeepExtraContentFixed();
  void overflowPopupMatchesAntDesign_data();
  void overflowPopupMatchesAntDesign();
  void overflowPopupHoverAndLiveUpdates();
  void overflowEdgesTrackScrollPosition();
  void startPlacementFollowsLayoutDirection();
  void rendersRepresentativeStates();
};

void TabsTest::firstEnabledTabBecomesCurrent() {
  AdTabs tabs;
  QSignalSpy indexChanged(&tabs, &AdTabs::currentIndexChanged);
  QSignalSpy keyChanged(&tabs, &AdTabs::currentKeyChanged);

  AdTabs::TabItem disabled;
  disabled.key = QStringLiteral("disabled");
  disabled.label = QStringLiteral("Disabled");
  disabled.enabled = false;
  QCOMPARE(tabs.addTab(disabled), 0);
  QCOMPARE(tabs.currentIndex(), -1);

  QCOMPARE(tabs.addTab(QStringLiteral("profile"), QStringLiteral("Profile")), 1);
  QCOMPARE(tabs.currentIndex(), 1);
  QCOMPARE(tabs.currentKey(), QStringLiteral("profile"));
  QCOMPARE(indexChanged.count(), 1);
  QCOMPARE(keyChanged.count(), 1);
  QCOMPARE(tabs.addTab(QStringLiteral("profile"), QStringLiteral("Duplicate")), -1);
  QCOMPARE(tabs.count(), 2);
}

void TabsTest::keysStayStableAcrossInsertionAndRemoval() {
  AdTabs tabs;
  tabs.addTab(QStringLiteral("a"), QStringLiteral("A"));
  tabs.addTab(QStringLiteral("b"), QStringLiteral("B"));
  tabs.setCurrentKey(QStringLiteral("b"));

  AdTabs::TabItem inserted;
  inserted.key = QStringLiteral("zero");
  inserted.label = QStringLiteral("Zero");
  QCOMPARE(tabs.insertTab(0, inserted), 0);
  QCOMPARE(tabs.currentIndex(), 2);
  QCOMPARE(tabs.currentKey(), QStringLiteral("b"));

  tabs.removeTab(QStringLiteral("a"));
  QCOMPARE(tabs.currentIndex(), 1);
  QCOMPARE(tabs.currentKey(), QStringLiteral("b"));
  QCOMPARE(tabs.tabKey(0), QStringLiteral("zero"));
}

void TabsTest::disabledTabsCannotBecomeCurrent() {
  AdTabs tabs;
  tabs.addTab(QStringLiteral("a"), QStringLiteral("A"));
  tabs.addTab(QStringLiteral("b"), QStringLiteral("B"));
  tabs.addTab(QStringLiteral("c"), QStringLiteral("C"));
  tabs.setTabEnabled(1, false);

  tabs.setCurrentIndex(1);
  QCOMPARE(tabs.currentKey(), QStringLiteral("a"));
  tabs.setTabEnabled(0, false);
  QCOMPARE(tabs.currentKey(), QStringLiteral("c"));
  tabs.setTabEnabled(2, false);
  QCOMPARE(tabs.currentIndex(), -1);
  tabs.setTabEnabled(1, true);
  QCOMPARE(tabs.currentKey(), QStringLiteral("b"));
}

void TabsTest::takeTabTransfersOwnership() {
  AdTabs tabs;
  auto* page = new QLabel(QStringLiteral("Page"));
  QPointer<QWidget> guard(page);
  tabs.addTab(QStringLiteral("page"), QStringLiteral("Page"), page);

  QWidget* taken = tabs.takeTab(0);
  QCOMPARE(taken, page);
  QVERIFY(taken->parentWidget() == nullptr);
  QCOMPARE(tabs.count(), 0);
  QVERIFY(!guard.isNull());

  auto* replacement = new QLabel(QStringLiteral("Replacement"));
  tabs.addTab(QStringLiteral("page"), QStringLiteral("Replacement"), replacement);
  delete taken;
  QVERIFY(guard.isNull());
  QCoreApplication::processEvents();
  QCOMPARE(tabs.count(), 1);
  QCOMPARE(tabs.widget(0), replacement);
}

void TabsTest::externallyDestroyedPageIsRemoved() {
  AdTabs tabs;
  auto* first = new QWidget;
  tabs.addTab(QStringLiteral("first"), QStringLiteral("First"), first);
  tabs.addTab(QStringLiteral("second"), QStringLiteral("Second"));
  QSignalSpy indexChanged(&tabs, &AdTabs::currentIndexChanged);
  QSignalSpy keyChanged(&tabs, &AdTabs::currentKeyChanged);

  delete first;
  QTRY_COMPARE(tabs.count(), 1);

  QCOMPARE(tabs.currentIndex(), 0);
  QCOMPARE(tabs.currentKey(), QStringLiteral("second"));
  QCOMPARE(indexChanged.count(), 0);
  QCOMPARE(keyChanged.count(), 1);
}

void TabsTest::invalidPageHierarchiesAreRejected() {
  AdTabs tabs;
  auto* page = new QWidget;
  QCOMPARE(tabs.addTab(QStringLiteral("first"), QStringLiteral("First"), page), 0);
  QCOMPARE(tabs.addTab(QStringLiteral("duplicate-page"), QStringLiteral("Duplicate"), page), -1);
  QCOMPARE(tabs.addTab(QStringLiteral("self"), QStringLiteral("Self"), &tabs), -1);
  QCOMPARE(tabs.count(), 1);

  tabs.setTabBarExtraContentStart(page);
  QVERIFY(tabs.tabBarExtraContentStart() == nullptr);
  QCOMPARE(tabs.widget(0), page);
  QStackedWidget* stack = tabs.findChild<QStackedWidget*>();
  QVERIFY(stack);
  tabs.setTabBarExtraContentStart(stack);
  QVERIFY(tabs.tabBarExtraContentStart() == nullptr);

  delete tabs.takeTab(0);
}

void TabsTest::editableCardEmitsAddAndCloseRequests() {
  AdTabs tabs;
  tabs.setType(AdTabs::Type::EditableCard);
  tabs.addTab(QStringLiteral("one"), QStringLiteral("One"));
  tabs.resize(420, 180);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));

  QSignalSpy addRequested(&tabs, &AdTabs::addRequested);
  QSignalSpy closeRequested(&tabs, &AdTabs::tabCloseRequested);
  QAbstractButton* add = operationButton(&tabs, QStringLiteral("Add tab"));
  QAbstractButton* tab = tabButton(&tabs, QStringLiteral("One"));
  QVERIFY(add);
  QVERIFY(add->isVisible());
  QVERIFY(tab);

  QTest::mouseClick(add, Qt::LeftButton);
  QCOMPARE(addRequested.count(), 1);
  tab->setFocus(Qt::TabFocusReason);
  QTest::keyClick(tab, Qt::Key_Delete);
  QCOMPARE(closeRequested.count(), 1);
  QCOMPARE(closeRequested.takeFirst().at(0).toString(), QStringLiteral("one"));

  tabs.setTabClosable(0, false);
  QTest::keyClick(tab, Qt::Key_Delete);
  QCOMPARE(closeRequested.count(), 0);
}

void TabsTest::arrowKeysSelectTheNextEnabledTab() {
  AdTabs tabs;
  tabs.addTab(QStringLiteral("a"), QStringLiteral("A"));
  tabs.addTab(QStringLiteral("b"), QStringLiteral("B"));
  tabs.addTab(QStringLiteral("c"), QStringLiteral("C"));
  tabs.setTabEnabled(1, false);
  tabs.resize(520, 180);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));

  QAbstractButton* first = tabButton(&tabs, QStringLiteral("A"));
  QVERIFY(first);
  first->setFocus(Qt::TabFocusReason);
  QTest::keyClick(first, Qt::Key_Right);
  QCOMPARE(tabs.currentKey(), QStringLiteral("c"));
  QAbstractButton* last = tabButton(&tabs, QStringLiteral("C"));
  QVERIFY(last);
  QVERIFY(last->hasFocus());
}

void TabsTest::selectionUsesStandardButtonState() {
  AdTabs tabs;
  tabs.addTab(QStringLiteral("a"), QStringLiteral("A"));
  tabs.addTab(QStringLiteral("b"), QStringLiteral("B"));
  tabs.resize(420, 180);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));

  QAbstractButton* first = tabButton(&tabs, QStringLiteral("A"));
  QAbstractButton* second = tabButton(&tabs, QStringLiteral("B"));
  QVERIFY(first);
  QVERIFY(second);
  QVERIFY(first->isCheckable());
  QVERIFY(first->isChecked());
  QCOMPARE(first->focusPolicy(), Qt::TabFocus);
  QVERIFY(!second->isChecked());
  QCOMPARE(second->focusPolicy(), Qt::NoFocus);

  QTest::mouseClick(second, Qt::LeftButton);
  QVERIFY(!first->isChecked());
  QCOMPARE(first->focusPolicy(), Qt::NoFocus);
  QVERIFY(second->isChecked());
  QCOMPARE(second->focusPolicy(), Qt::TabFocus);

  tabs.setEnabled(false);
  QCOMPARE(second->cursor().shape(), Qt::ArrowCursor);

  AdTabs iconOnly;
  iconOnly.addTab(QStringLiteral("files"), QString());
  QAbstractButton* iconOnlyButton = tabButton(&iconOnly, QStringLiteral("files"));
  QVERIFY(iconOnlyButton);
}

void TabsTest::propertiesRoundTrip() {
  AdTabs tabs;
  tabs.setType(AdTabs::Type::Card);
  tabs.setControlSize(AdTabs::ControlSize::Large);
  tabs.setTabPlacement(AdTabs::Placement::Start);
  tabs.setCentered(true);
  tabs.setAnimated(false);
  tabs.setHideAdd(true);
  tabs.setTabBarGutter(12);
  tabs.setIndicatorSize(24);
  tabs.setIndicatorAlignment(AdTabs::IndicatorAlignment::Center);

  QCOMPARE(tabs.type(), AdTabs::Type::Card);
  QCOMPARE(tabs.controlSize(), AdTabs::ControlSize::Large);
  QCOMPARE(tabs.tabPlacement(), AdTabs::Placement::Start);
  QVERIFY(tabs.centered());
  QVERIFY(!tabs.animated());
  QVERIFY(tabs.hideAdd());
  QCOMPARE(tabs.tabBarGutter(), 12);
  QCOMPARE(tabs.indicatorSize(), 24);
  QCOMPARE(tabs.indicatorAlignment(), AdTabs::IndicatorAlignment::Center);
}

void TabsTest::sizeHintsFollowContentAndTokens() {
  AdTabs tabs;
  tabs.addTab(QStringLiteral("short"), QStringLiteral("Short"));
  const QSize shortHint = tabs.sizeHint();

  tabs.setTabText(0, QStringLiteral("A substantially longer tab label"));
  const QSize longHint = tabs.sizeHint();
  QVERIFY(longHint.width() > shortHint.width());

  AdTabs::ComponentTokens tokens;
  tokens.metrics.verticalItemPadding = 30;
  tabs.setComponentTokens(tokens);
  QVERIFY(tabs.sizeHint().height() > longHint.height());
}

void TabsTest::extraContentTracksQObjectLifetime() {
  AdTabs tabs;
  tabs.addTab(QStringLiteral("tab"), QStringLiteral("Tab"));
  auto* extra = new QLabel(QStringLiteral("Extra"));
  extra->setFixedWidth(100);
  tabs.setTabBarExtraContentStart(extra);
  QCOMPARE(tabs.tabBarExtraContentStart(), extra);
  tabs.setTabBarExtraContentEnd(extra);
  QVERIFY(tabs.tabBarExtraContentStart() == nullptr);
  QCOMPARE(tabs.tabBarExtraContentEnd(), extra);
  tabs.setTabBarExtraContentStart(extra);
  QCOMPARE(tabs.tabBarExtraContentStart(), extra);
  QVERIFY(tabs.tabBarExtraContentEnd() == nullptr);

  tabs.resize(360, 160);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));
  QAbstractButton* button = tabButton(&tabs, QStringLiteral("Tab"));
  QVERIFY(button);
  const int withExtraX = button->mapTo(&tabs, QPoint()).x();
  QVERIFY(withExtraX >= extra->width());

  delete extra;
  QVERIFY(tabs.tabBarExtraContentStart() == nullptr);
  QTRY_VERIFY(button->mapTo(&tabs, QPoint()).x() < withExtraX);
}

void TabsTest::horizontalTabsKeepNaturalWidths_data() {
  QTest::addColumn<AdTabs::Type>("type");
  QTest::addColumn<AdTabs::Placement>("placement");
  QTest::addColumn<Qt::LayoutDirection>("direction");
  for (auto type : {AdTabs::Type::Line, AdTabs::Type::Card, AdTabs::Type::EditableCard}) {
    for (auto placement : {AdTabs::Placement::Top, AdTabs::Placement::Bottom}) {
      for (auto direction : {Qt::LeftToRight, Qt::RightToLeft}) {
        const QByteArray name = QByteArray::number(static_cast<int>(type)) + '-' +
                                QByteArray::number(static_cast<int>(placement)) + '-' +
                                QByteArray::number(static_cast<int>(direction));
        QTest::newRow(name.constData()) << type << placement << direction;
      }
    }
  }
}

void TabsTest::horizontalTabsKeepNaturalWidths() {
  QFETCH(AdTabs::Type, type);
  QFETCH(AdTabs::Placement, placement);
  QFETCH(Qt::LayoutDirection, direction);
  AdTabs tabs;
  tabs.setType(type);
  tabs.setTabPlacement(placement);
  tabs.setLayoutDirection(direction);
  tabs.setAnimated(false);
  tabs.setTabBarGutter(0);
  tabs.addTab(QStringLiteral("history"), QStringLiteral("Recent History"));
  tabs.addTab(QStringLiteral("storage-status"), QStringLiteral("Storage Status"));
  tabs.setTabIcon(1, adqt::icons::antd::outlined::FolderOpen());
  tabs.setCurrentKey(QStringLiteral("storage-status"));
  tabs.resize(480, 160);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));

  QAbstractButton* history = tabButton(&tabs, QStringLiteral("Recent History"));
  QAbstractButton* storageStatus = tabButton(&tabs, QStringLiteral("Storage Status"));
  QAbstractButton* more = operationButton(&tabs, QStringLiteral("More tabs"));
  QVERIFY(history);
  QVERIFY(storageStatus);
  QVERIFY(more);

  const int historyNaturalWidth = history->sizeHint().width();
  const int storageNaturalWidth = storageStatus->sizeHint().width();
  QVERIFY(storageNaturalWidth > historyNaturalWidth);
  const int reduction = std::max(1, (storageNaturalWidth - historyNaturalWidth) / 2);
  QAbstractButton* add = operationButton(&tabs, QStringLiteral("Add tab"));
  const int addWidth = type == AdTabs::Type::EditableCard ? add->width() : 0;
  tabs.resize(historyNaturalWidth + storageNaturalWidth + addWidth - reduction, tabs.height());
  QCoreApplication::processEvents();

  QVERIFY(history->isVisible());
  QVERIFY(storageStatus->isVisible());
  QVERIFY(more->isVisible());
  QCOMPARE(history->width(), historyNaturalWidth);
  QCOMPARE(storageStatus->width(), storageNaturalWidth);
  QCOMPARE(storageStatus->height(), storageStatus->sizeHint().height());
  QVERIFY(history->toolTip().isEmpty());
  QVERIFY(storageStatus->toolTip().isEmpty());
  auto* scroll = tabs.findChild<QScrollArea*>();
  QVERIFY(scroll);
  const auto visibleRect = [scroll](QWidget* button) {
    return QRect(button->mapTo(scroll->viewport(), QPoint()), button->size());
  };
  QVERIFY(scroll->viewport()->rect().contains(visibleRect(storageStatus)));
  QVERIFY(!scroll->viewport()->rect().contains(visibleRect(history)));

  tabs.resize(640, tabs.height());
  QCoreApplication::processEvents();
  QVERIFY(more->isHidden());
  QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
  QVERIFY(scroll->viewport()->rect().contains(visibleRect(history)));
  QVERIFY(scroll->viewport()->rect().contains(visibleRect(storageStatus)));
}

void TabsTest::overflowScrollsTabs_data() {
  QTest::addColumn<AdTabs::Placement>("placement");
  QTest::addColumn<Qt::LayoutDirection>("direction");
  for (auto placement : {AdTabs::Placement::Top, AdTabs::Placement::Bottom,
                         AdTabs::Placement::Start, AdTabs::Placement::End}) {
    for (auto direction : {Qt::LeftToRight, Qt::RightToLeft}) {
      const QByteArray name = QByteArray::number(static_cast<int>(placement)) + '-' +
                              QByteArray::number(static_cast<int>(direction));
      QTest::newRow(name.constData()) << placement << direction;
    }
  }
}

void TabsTest::overflowScrollsTabs() {
  QFETCH(AdTabs::Placement, placement);
  QFETCH(Qt::LayoutDirection, direction);
  AdTabs tabs;
  tabs.setAnimated(false);
  tabs.setTabPlacement(placement);
  tabs.setLayoutDirection(direction);
  for (int index = 0; index < 12; ++index) {
    tabs.addTab(QString::number(index), QStringLiteral("Long tab %1").arg(index));
  }
  tabs.resize(360, 180);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));

  QAbstractButton* more = operationButton(&tabs, QStringLiteral("More tabs"));
  QVERIFY(more);
  QVERIFY(more->isVisible());
  const QList<QAbstractButton*> buttons =
      tabs.findChildren<QAbstractButton*>(QStringLiteral("ad-tabs-item"));
  for (QAbstractButton* button : buttons) {
    QVERIFY(button->isVisible());
    QVERIFY(button->width() >= button->sizeHint().width());
  }
  auto* scroll = tabs.findChild<QScrollArea*>();
  QVERIFY(scroll);
  const bool horizontal =
      placement == AdTabs::Placement::Top || placement == AdTabs::Placement::Bottom;
  QScrollBar* bar = horizontal ? scroll->horizontalScrollBar() : scroll->verticalScrollBar();
  QVERIFY(bar->maximum() > 0);
  const QPoint position = scroll->viewport()->rect().center();
  QWheelEvent wheel(position, scroll->viewport()->mapToGlobal(position), QPoint(), QPoint(0, -120),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(scroll->viewport(), &wheel);
  QVERIFY(bar->value() > 0);
  QCOMPARE(tabs.currentIndex(), 0);

  bar->setValue(0);
  const QPoint pixelDelta =
      horizontal ? QPoint(direction == Qt::RightToLeft ? 17 : -17, 0) : QPoint(0, -17);
  QWheelEvent trackpad(position, scroll->viewport()->mapToGlobal(position), pixelDelta, QPoint(),
                       Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
  QCoreApplication::sendEvent(scroll->viewport(), &trackpad);
  QCOMPARE(bar->value(), 17);
  QVERIFY(trackpad.isAccepted());
  const QPoint angleDelta =
      horizontal ? QPoint(direction == Qt::RightToLeft ? 120 : -120, 0) : QPoint(0, -120);
  QWheelEvent nativeWheel(position, scroll->viewport()->mapToGlobal(position), QPoint(), angleDelta,
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(scroll->viewport(), &nativeWheel);
  QVERIFY(bar->value() > 17);
  bar->setValue(bar->maximum());
  QCoreApplication::sendEvent(scroll->viewport(), &trackpad);
  QVERIFY(!trackpad.isAccepted());

  tabs.setCurrentIndex(11);
  QAbstractButton* last = tabButton(&tabs, QStringLiteral("Long tab 11"));
  QVERIFY(last);
  const auto visibleRect = [scroll](QWidget* button) {
    return QRect(button->mapTo(scroll->viewport(), QPoint()), button->size());
  };
  QVERIFY(scroll->viewport()->rect().contains(visibleRect(last)));
  last->setFocus(Qt::TabFocusReason);
  QTest::keyClick(last, Qt::Key_Home);
  QCOMPARE(tabs.currentIndex(), 0);
  QAbstractButton* first = tabButton(&tabs, QStringLiteral("Long tab 0"));
  QVERIFY(first->hasFocus());
  QVERIFY(scroll->viewport()->rect().contains(visibleRect(first)));

  QWidget* indicator = tabs.findChild<QWidget*>(QStringLiteral("ad-tabs-indicator"));
  QVERIFY(indicator);
  QVERIFY(scroll->viewport()->rect().contains(visibleRect(indicator)));
}

void TabsTest::overflowMenuTracksScrolledTabs() {
  AdTabs tabs;
  tabs.setAnimated(false);
  for (int index = 0; index < 8; ++index) {
    tabs.addTab(QString::number(index), QStringLiteral("Workspace %1").arg(index));
  }
  tabs.setTabEnabled(2, false);
  tabs.resize(320, 180);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));
  auto* scroll = tabs.findChild<QScrollArea*>();
  QVERIFY(scroll);
  auto* more = operationButton(&tabs, QStringLiteral("More tabs"));
  QVERIFY(more);
  scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->maximum());

  QStringList expected;
  for (int index = 0; index < tabs.count(); ++index) {
    QAbstractButton* button = tabButton(&tabs, tabs.tabText(index));
    const QRect bounds(button->mapTo(scroll->viewport(), QPoint()), button->size());
    if (!scroll->viewport()->rect().contains(bounds)) {
      expected.append(tabs.tabText(index));
    }
  }
  QVERIFY(expected.contains(tabs.tabText(0)));
  QVERIFY(!expected.contains(tabs.tabText(7)));
  auto* popup = tabs.findChild<adqt::widgets::AdPopover*>(QStringLiteral("ad-tabs-overflow-popup"));
  QVERIFY(popup);
  QTest::mouseClick(more, Qt::LeftButton);
  QTRY_VERIFY(popup->isVisible());
  auto* list = qobject_cast<QListWidget*>(popup->contentWidget());
  QVERIFY(list);
  QStringList actual;
  for (int row = 0; row < list->count(); ++row) {
    actual.append(list->item(row)->text());
    QVERIFY(!list->item(row)->flags().testFlag(Qt::ItemIsUserCheckable));
  }
  QCOMPARE(actual, expected);
  QCOMPARE(list->currentRow(), -1);
  QVERIFY(!list->item(actual.indexOf(tabs.tabText(2)))->flags().testFlag(Qt::ItemIsEnabled));
  QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                    list->visualItemRect(list->item(0)).center());
  QTRY_VERIFY(!popup->isVisible());
  QCOMPARE(tabs.currentIndex(), 0);
  QCOMPARE(scroll->horizontalScrollBar()->value(), 0);
}

void TabsTest::oversizedTabsKeepExtraContentFixed() {
  AdTabs tabs;
  tabs.setAnimated(false);
  tabs.setType(AdTabs::Type::EditableCard);
  tabs.setCentered(true);
  auto* start = new QWidget;
  start->setFixedWidth(40);
  auto* end = new QWidget;
  end->setFixedWidth(40);
  tabs.setTabBarExtraContentStart(start);
  tabs.setTabBarExtraContentEnd(end);
  tabs.addTab(QStringLiteral("long"),
              QStringLiteral("An unusually long workspace name ").repeated(3));
  tabs.resize(320, 180);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));
  auto* scroll = tabs.findChild<QScrollArea*>();
  auto* button = tabButton(&tabs, tabs.tabText(0));
  auto* more = operationButton(&tabs, QStringLiteral("More tabs"));
  auto* add = operationButton(&tabs, QStringLiteral("Add tab"));
  QVERIFY(scroll);
  QVERIFY(button);
  QVERIFY(more);
  QVERIFY(add);
  QCOMPARE(button->width(), button->sizeHint().width());
  QVERIFY(button->width() > scroll->viewport()->width());
  QVERIFY(more->isVisible());
  QVERIFY(button->toolTip().isEmpty());
  const QRect startGeometry = start->geometry();
  const QRect endGeometry = end->geometry();
  const QRect addGeometry = add->geometry();
  QVERIFY(!scroll->geometry().intersects(startGeometry));
  QVERIFY(!scroll->geometry().intersects(endGeometry));
  QVERIFY(!scroll->geometry().intersects(addGeometry));
  scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->maximum());
  QCOMPARE(start->geometry(), startGeometry);
  QCOMPARE(end->geometry(), endGeometry);
  QCOMPARE(add->geometry(), addGeometry);
  QCOMPARE(button->mapTo(scroll->viewport(), button->rect().topRight()).x(),
           scroll->viewport()->rect().right());

  tabs.setTabText(0, QStringLiteral("Short"));
  QCoreApplication::processEvents();
  QVERIFY(more->isHidden());
  QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
  const QRect centeredRect(button->mapTo(scroll->viewport(), QPoint()), button->size());
  QVERIFY(std::abs(centeredRect.center().x() - scroll->viewport()->rect().center().x()) <= 1);
  tabs.clear();
  QCoreApplication::processEvents();
  QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
  QVERIFY(more->isHidden());
  QVERIFY(add->isVisible());
  QVERIFY(tabs.findChildren<QAbstractButton*>(QStringLiteral("ad-tabs-item")).isEmpty());
}

void TabsTest::overflowPopupMatchesAntDesign_data() {
  QTest::addColumn<bool>("dark");
  QTest::addColumn<Qt::LayoutDirection>("direction");
  QTest::newRow("light-ltr") << false << Qt::LeftToRight;
  QTest::newRow("dark-ltr") << true << Qt::LeftToRight;
  QTest::newRow("light-rtl") << false << Qt::RightToLeft;
  QTest::newRow("dark-rtl") << true << Qt::RightToLeft;
}

void TabsTest::overflowPopupMatchesAntDesign() {
  QFETCH(bool, dark);
  QFETCH(Qt::LayoutDirection, direction);
  auto& manager = adqt::theme::ThemeManager::instance();
  const auto original = manager.config();
  struct RestoreTheme {
    adqt::theme::ThemeConfig config;
    ~RestoreTheme() { adqt::theme::ThemeManager::instance().setConfig(config); }
  } restore{original};
  manager.setPreset(dark ? adqt::theme::ThemeScheme::Dark : adqt::theme::ThemeScheme::Light,
                    adqt::theme::ThemeDensity::Comfortable);
  AdTabs tabs;
  tabs.setAnimated(false);
  tabs.setLayoutDirection(direction);
  tabs.setType(AdTabs::Type::EditableCard);
  for (int i = 0; i < 12; ++i) {
    tabs.addTab(QString::number(i), i == 0 ? QStringLiteral("Configuration")
                                           : QStringLiteral("Data storage %1").arg(i));
  }
  tabs.setTabEnabled(2, false);
  tabs.resize(240, 200);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));
  auto* more = operationButton(&tabs, QStringLiteral("More tabs"));
  QVERIFY(more);
  more->setFocus(Qt::TabFocusReason);
  QTest::keyClick(more, Qt::Key_Down);
  auto* popup = tabs.findChild<adqt::widgets::AdPopover*>(QStringLiteral("ad-tabs-overflow-popup"));
  QVERIFY(popup);
  QTRY_VERIFY(popup->isVisible());
  auto* list = qobject_cast<QListWidget*>(popup->contentWidget());
  QVERIFY(list);
  const auto theme = manager.resolveTheme(&tabs);
  QVERIFY(!popup->arrowVisible());
  QCOMPARE(popup->borderWidth(), 0);
  QCOMPARE(popup->cornerRadius(), qRound(theme.borderRadiusLG));
  QCOMPARE(popup->contentMargins(), QMargins(0, qRound(theme.sizeXXS), 0, qRound(theme.sizeXXS)));
  QCOMPARE(list->visualItemRect(list->item(0)).height(),
           qRound(theme.fontHeight + theme.sizeXXS * 2));
  QVERIFY(list->height() + popup->contentMargins().top() * 2 <= 200);
  QVERIFY(list->verticalScrollBar()->maximum() > 0);
  QCOMPARE(list->currentRow(), -1);
  const int originalIndex = tabs.currentIndex();
  QTest::keyClick(more, Qt::Key_Down);
  QVERIFY(list->currentRow() >= 0);
  QVERIFY(list->currentItem()->flags().testFlag(Qt::ItemIsEnabled));
  QCOMPARE(tabs.currentIndex(), originalIndex);

  const QString snapshotDirectory = qEnvironmentVariable("ADQT_TABS_SNAPSHOT_DIR");
  if (!snapshotDirectory.isEmpty()) {
    QDir().mkpath(snapshotDirectory);
    QCoreApplication::processEvents();
    QVERIFY(popup->surfaceWidget()->grab().save(
        QDir(snapshotDirectory)
            .filePath(QStringLiteral("tabs-overflow-%1-%2.png")
                          .arg(dark ? "dark" : "light")
                          .arg(direction == Qt::RightToLeft ? "rtl" : "ltr"))));
  }
  QSignalSpy closeRequested(&tabs, &AdTabs::tabCloseRequested);
  const QString closeKey = list->currentItem()->data(Qt::UserRole).toString();
  QTest::keyClick(more, Qt::Key_Delete);
  QCOMPARE(closeRequested.count(), 1);
  QCOMPARE(closeRequested.at(0).at(0).toString(), closeKey);
  QCOMPARE(tabs.currentIndex(), originalIndex);
  QTest::keyClick(more, Qt::Key_Return);
  QTRY_VERIFY(!popup->isVisible());
  QCOMPARE(tabs.currentKey(), closeKey);
  QTest::keyClick(more, Qt::Key_Down);
  QTRY_VERIFY(popup->isVisible());
  QTest::keyClick(more, Qt::Key_Escape);
  QTRY_VERIFY(!popup->isVisible());
}

void TabsTest::overflowPopupHoverAndLiveUpdates() {
  AdTabs tabs;
  tabs.setAnimated(false);
  tabs.addTab(QStringLiteral("config"), QStringLiteral("Configuration"));
  tabs.addTab(QStringLiteral("storage"), QStringLiteral("Data storage"));
  tabs.resize(96, 200);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));
  auto* more = operationButton(&tabs, QStringLiteral("More tabs"));
  auto* popup = tabs.findChild<adqt::widgets::AdPopover*>(QStringLiteral("ad-tabs-overflow-popup"));
  QVERIFY(more);
  QVERIFY(popup);
  QTest::mouseMove(&tabs, QPoint(5, tabs.height() - 5));
  QTest::mouseMove(more, more->rect().center());
  QTRY_VERIFY(popup->isVisible());
  auto* list = qobject_cast<QListWidget*>(popup->contentWidget());
  QVERIFY(list);
  QCOMPARE(list->count(), 2);
  QCOMPARE(list->currentRow(), -1);
  QTest::mouseMove(list->viewport(), list->visualItemRect(list->item(0)).center());
  const QString directory = qEnvironmentVariable("ADQT_TABS_SNAPSHOT_DIR");
  if (!directory.isEmpty()) {
    QDir().mkpath(directory);
    QVERIFY(
        popup->surfaceWidget()->grab().save(QDir(directory).filePath("tabs-overflow-simple.png")));
  }
  QTest::mouseMove(&tabs, QPoint(5, tabs.height() - 5));
  QTRY_VERIFY(!popup->isVisible());
  QTest::mouseClick(more, Qt::LeftButton);
  QTRY_VERIFY(popup->isVisible());
  tabs.setTabText(1, QStringLiteral("Updated data storage"));
  QTRY_COMPARE(list->item(1)->text(), QStringLiteral("Updated data storage"));
  tabs.setTabEnabled(1, false);
  QVERIFY(!list->item(1)->flags().testFlag(Qt::ItemIsEnabled));
  tabs.resize(640, tabs.height());
  QTRY_VERIFY(!popup->isVisible());
  QVERIFY(more->isHidden());
  QCOMPARE(list->count(), 0);
}

void TabsTest::overflowEdgesTrackScrollPosition() {
  AdTabs tabs;
  tabs.setAnimated(false);
  for (int i = 0; i < 8; ++i) {
    tabs.addTab(QString::number(i), QStringLiteral("Workspace %1").arg(i));
  }
  tabs.resize(320, 180);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));
  auto* scroll = tabs.findChild<QScrollArea*>();
  QVERIFY(scroll);
  auto* edges = scroll->viewport()->findChild<QWidget*>(QStringLiteral("ad-tabs-scroll-edges"));
  QVERIFY(edges);
  QVERIFY(edges->testAttribute(Qt::WA_TransparentForMouseEvents));
  const auto edgeAlpha = [edges](bool left) {
    QImage image(edges->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    edges->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    return image.pixelColor(left ? 1 : image.width() - 2, image.height() / 2).alpha();
  };
  QCOMPARE(edgeAlpha(true), 0);
  QVERIFY(edgeAlpha(false) > 0);
  scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->maximum());
  QVERIFY(edgeAlpha(true) > 0);
  QCOMPARE(edgeAlpha(false), 0);
  tabs.setLayoutDirection(Qt::RightToLeft);
  QCoreApplication::processEvents();
  scroll->horizontalScrollBar()->setValue(0);
  QVERIFY(edgeAlpha(true) > 0);
  QCOMPARE(edgeAlpha(false), 0);
  tabs.resize(tabs.sizeHint().width(), tabs.height());
  QCoreApplication::processEvents();
  QCOMPARE(edgeAlpha(true), 0);
  QCOMPARE(edgeAlpha(false), 0);
}

void TabsTest::startPlacementFollowsLayoutDirection() {
  AdTabs tabs;
  tabs.setTabPlacement(AdTabs::Placement::Start);
  tabs.addTab(QStringLiteral("a"), QStringLiteral("A"));
  tabs.resize(480, 220);
  tabs.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tabs));
  QWidget* strip = tabs.findChild<QWidget*>(QStringLiteral("ad-tabs-strip"));
  QStackedWidget* stack = tabs.findChild<QStackedWidget*>();
  QVERIFY(strip);
  QVERIFY(stack);
  QVERIFY(strip->geometry().right() < stack->geometry().left());

  tabs.setLayoutDirection(Qt::RightToLeft);
  QCoreApplication::processEvents();
  QVERIFY(strip->geometry().left() > stack->geometry().right());
}

void TabsTest::rendersRepresentativeStates() {
  adqt::icons::antd::ensureRegistered();
  auto& themeManager = adqt::theme::ThemeManager::instance();
  const adqt::theme::ThemeConfig originalConfig = themeManager.config();
  themeManager.applyTo(*qApp);

  QWidget showcase;
  showcase.setObjectName(QStringLiteral("tabsSnapshot"));
  showcase.setFocusPolicy(Qt::StrongFocus);
  showcase.resize(960, 640);
  auto* grid = new QGridLayout(&showcase);
  grid->setContentsMargins(24, 24, 24, 24);
  grid->setHorizontalSpacing(24);
  grid->setVerticalSpacing(24);

  auto makePage = [](const QString& text) {
    auto* page = new QLabel(text);
    page->setAlignment(Qt::AlignCenter);
    return page;
  };
  auto populate = [&makePage](AdTabs* tabs, int count) {
    for (int index = 0; index < count; ++index) {
      AdTabs::TabItem item;
      item.key = QString::number(index);
      item.label = QStringLiteral("Tab %1").arg(index + 1);
      item.page = makePage(QStringLiteral("Content %1").arg(index + 1));
      item.enabled = index != 2;
      item.closable = index != 0;
      if (index == 0) {
        item.icon = adqt::icons::antd::outlined::FolderOpen();
      }
      tabs->addTab(item);
    }
  };

  auto* line = new AdTabs;
  populate(line, 4);
  line->setCentered(true);
  line->setIndicatorSize(30);
  line->setIndicatorAlignment(AdTabs::IndicatorAlignment::Center);
  auto* card = new AdTabs;
  card->setType(AdTabs::Type::Card);
  populate(card, 4);
  auto* editable = new AdTabs;
  editable->setType(AdTabs::Type::EditableCard);
  populate(editable, 4);
  auto* vertical = new AdTabs;
  vertical->setTabPlacement(AdTabs::Placement::Start);
  populate(vertical, 7);
  auto* overflow = new AdTabs;
  populate(overflow, 14);
  auto* extra = new QLabel(QStringLiteral("Workspace"));
  extra->setContentsMargins(0, 0, 12, 0);
  overflow->setTabBarExtraContentStart(extra);

  grid->addWidget(line, 0, 0);
  grid->addWidget(card, 0, 1);
  grid->addWidget(editable, 1, 0);
  grid->addWidget(vertical, 1, 1);
  grid->addWidget(overflow, 2, 0, 1, 2);
  grid->setRowStretch(0, 1);
  grid->setRowStretch(1, 1);
  grid->setRowStretch(2, 1);

  const QString snapshotDirectory = qEnvironmentVariable("ADQT_TABS_SNAPSHOT_DIR");
  auto renderScheme = [&](adqt::theme::ThemeScheme scheme, const QString& fileName) {
    themeManager.setPreset(scheme, adqt::theme::ThemeDensity::Comfortable);
    const adqt::theme::ThemeMapToken colors = themeManager.resolveTheme(&showcase);
    QPalette palette = showcase.palette();
    palette.setColor(QPalette::Window, colors.colorBgContainer);
    palette.setColor(QPalette::Base, colors.colorBgContainer);
    palette.setColor(QPalette::WindowText, colors.colorText);
    palette.setColor(QPalette::Text, colors.colorText);
    showcase.setPalette(palette);
    showcase.setAutoFillBackground(true);
    showcase.show();
    showcase.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    QTest::qWait(250);
    if (QWidget* focus = qApp->focusWidget()) {
      focus->clearFocus();
    }
    QCoreApplication::processEvents();
    QWidget* lineIndicator = line->findChild<QWidget*>(QStringLiteral("ad-tabs-indicator"));
    QVERIFY(lineIndicator);
    QVERIFY(lineIndicator->isVisible());
    QVERIFY(lineIndicator->width() >= 28);
    QVERIFY(lineIndicator->height() >= 2);
    QVERIFY(lineIndicator->height() <= 4);
    const QImage image = showcase.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), showcase.size());
    QSet<QRgb> sampledColors;
    for (int y = 0; y < image.height(); y += 8) {
      for (int x = 0; x < image.width(); x += 8) {
        sampledColors.insert(image.pixel(x, y));
      }
    }
    QVERIFY2(sampledColors.size() > 8,
             "Tabs snapshot should contain text, "
             "borders, surfaces, and state colors");
    if (!snapshotDirectory.isEmpty()) {
      QDir().mkpath(snapshotDirectory);
      QVERIFY(image.save(QDir(snapshotDirectory).filePath(fileName)));
    }
  };

  renderScheme(adqt::theme::ThemeScheme::Light, QStringLiteral("tabs-light.png"));
  renderScheme(adqt::theme::ThemeScheme::Dark, QStringLiteral("tabs-dark.png"));
  themeManager.setConfig(originalConfig);
}

QTEST_MAIN(TabsTest)

#include "tst_tabs.moc"
