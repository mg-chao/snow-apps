#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFrame>
#include <QImage>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include "antd_icons.h"
#include "widgets/context_menu.h"

using adqt::widgets::AdContextMenu;
namespace outlined_icons = adqt::icons::antd::outlined;

class ContextMenuTests final : public QObject {
  Q_OBJECT

 private slots:
  void actionMetadataAndNativeStateCoexist();
  void triggerWidgetOpensOnContextMenuEvent();
  void rebindingStopsHandlingTheOldWidget();
  void keyboardActivationUsesNativeMenuBehavior();
  void menuUsesCompactAntMetrics();
  void metricTokensRelayoutExistingActions();
  void longLabelsRespectTrailingColumnsInConstrainedMenus();
};

void ContextMenuTests::actionMetadataAndNativeStateCoexist() {
  AdContextMenu menu;
  QVERIFY(menu.graphicsEffect() == nullptr);
  const auto editIcon = outlined_icons::Edit();
  QAction* rename = menu.addItem(QStringLiteral("Rename"), editIcon, QKeySequence(Qt::Key_F2));
  QVERIFY(rename);
  QCOMPARE(rename->shortcut(), QKeySequence(Qt::Key_F2));
  QCOMPARE(menu.actionIcon(rename), editIcon);
  QVERIFY(!menu.actionDanger(rename));

  menu.setActionDanger(rename);
  QVERIFY(menu.actionDanger(rename));
  rename->setCheckable(true);
  rename->setChecked(true);
  QVERIFY(rename->isChecked());

  AdContextMenu* submenu = menu.addSubMenu(QStringLiteral("Move to"), outlined_icons::Folder());
  QVERIFY(submenu);
  QCOMPARE(submenu->parentWidget(), &menu);
  QVERIFY(submenu->menuAction());
  QCOMPARE(menu.actionIcon(submenu->menuAction()), outlined_icons::Folder());

  AdContextMenu::ComponentTokens tokens;
  tokens.itemHeight = 40;
  menu.setColorScheme(AdContextMenu::ColorScheme::Dark);
  menu.setComponentTokens(tokens);
  QCOMPARE(submenu->colorScheme(), AdContextMenu::ColorScheme::Dark);
  QVERIFY(submenu->componentTokens().itemHeight.has_value());
  QCOMPARE(submenu->componentTokens().itemHeight.value(), 40);
}

void ContextMenuTests::triggerWidgetOpensOnContextMenuEvent() {
  QWidget window;
  window.resize(360, 240);
  QVBoxLayout layout(&window);
  auto* target = new QFrame(&window);
  target->setMinimumSize(200, 100);
  layout.addWidget(target);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  AdContextMenu menu(&window);
  menu.addItem(QStringLiteral("Rename"));
  menu.setTriggerWidget(target);
  QCOMPARE(menu.triggerWidget(), target);

  const QPoint localPosition = target->rect().center();
  QContextMenuEvent event(QContextMenuEvent::Mouse, localPosition,
                          target->mapToGlobal(localPosition));
  QCoreApplication::sendEvent(target, &event);
  QTRY_VERIFY(menu.isVisible());
  QVERIFY(event.isAccepted());
  menu.hide();
}

void ContextMenuTests::rebindingStopsHandlingTheOldWidget() {
  QWidget window;
  window.resize(420, 240);
  QVBoxLayout layout(&window);
  auto* first = new QWidget(&window);
  auto* second = new QWidget(&window);
  first->setMinimumHeight(80);
  second->setMinimumHeight(80);
  layout.addWidget(first);
  layout.addWidget(second);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  AdContextMenu menu(&window);
  menu.addItem(QStringLiteral("Open"));
  menu.setTriggerWidget(first);
  menu.setTriggerWidget(second);

  const QPoint firstLocal = first->rect().center();
  QContextMenuEvent firstEvent(QContextMenuEvent::Mouse, firstLocal,
                               first->mapToGlobal(firstLocal));
  QCoreApplication::sendEvent(first, &firstEvent);
  QVERIFY(!menu.isVisible());

  const QPoint secondLocal = second->rect().center();
  QContextMenuEvent secondEvent(QContextMenuEvent::Mouse, secondLocal,
                                second->mapToGlobal(secondLocal));
  QCoreApplication::sendEvent(second, &secondEvent);
  QTRY_VERIFY(menu.isVisible());
  menu.hide();
}

void ContextMenuTests::keyboardActivationUsesNativeMenuBehavior() {
  QWidget window;
  window.resize(320, 200);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  AdContextMenu menu(&window);
  QAction* action = menu.addItem(QStringLiteral("Duplicate"), outlined_icons::Copy());
  QSignalSpy triggered(action, &QAction::triggered);

  menu.popupAt(window.mapToGlobal(QPoint(30, 30)));
  QTRY_VERIFY(menu.isVisible());
  menu.setActiveAction(action);
  QTest::keyClick(&menu, Qt::Key_Return);
  QTRY_COMPARE(triggered.count(), 1);
  QVERIFY(!menu.isVisible());
}

void ContextMenuTests::menuUsesCompactAntMetrics() {
  QWidget window;
  window.resize(360, 240);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  AdContextMenu menu(&window);
  QAction* first = menu.addItem(QStringLiteral("Rename"), outlined_icons::Edit());
  QAction* second = menu.addItem(QStringLiteral("Duplicate"), outlined_icons::Copy(),
                                 QKeySequence(Qt::CTRL | Qt::Key_D));
  QAction* checked = menu.addItem(QStringLiteral("Keep available offline"));
  checked->setCheckable(true);
  checked->setChecked(true);
  QAction* disabled = menu.addItem(QStringLiteral("Share"), outlined_icons::ShareAlt());
  disabled->setEnabled(false);
  AdContextMenu* moveMenu =
      menu.addSubMenu(QStringLiteral("Move to"), outlined_icons::FolderOpen());
  moveMenu->addItem(QStringLiteral("Design"), outlined_icons::Folder());
  menu.addSeparator();
  QAction* danger = menu.addItem(QStringLiteral("Move to trash"), outlined_icons::IconDelete());
  menu.setActionDanger(danger);

  menu.popupAt(window.mapToGlobal(QPoint(40, 40)));
  QTRY_VERIFY(menu.isVisible());
  QTRY_VERIFY(menu.actionGeometry(first).isValid());

  QVERIFY(menu.width() >= 160);
  QVERIFY(menu.actionGeometry(first).height() >= 24);
  QCOMPARE(menu.actionGeometry(first).height(), menu.actionGeometry(second).height());
  QCOMPARE(menu.actionGeometry(first).height(), menu.actionGeometry(checked).height());
  QCOMPARE(menu.actionGeometry(first).height(), menu.actionGeometry(disabled).height());
  QVERIFY(menu.actionGeometry(danger).height() >= 24);

  menu.setActiveAction(danger);
  QCoreApplication::processEvents();
  const QImage image = menu.grab().toImage().convertToFormat(QImage::Format_ARGB32);
  QVERIFY(!image.isNull());
  QVERIFY(image.width() >= 160);
  QVERIFY(image.height() > menu.actionGeometry(first).height() * 3);
  QCOMPARE(image.pixelColor(image.rect().bottomRight()).alpha(), 0);
  const QString snapshotDirectory = qEnvironmentVariable("ADQT_CONTEXT_MENU_SNAPSHOT_DIR");
  if (!snapshotDirectory.isEmpty()) {
    QVERIFY(QDir().mkpath(snapshotDirectory));
    QVERIFY(image.save(QDir(snapshotDirectory).filePath(QStringLiteral("context-menu-light.png"))));
  }
  menu.hide();

  menu.setColorScheme(AdContextMenu::ColorScheme::Dark);
  menu.popupAt(window.mapToGlobal(QPoint(40, 40)));
  QTRY_VERIFY(menu.isVisible());
  menu.setActiveAction(danger);
  QCoreApplication::processEvents();
  const QImage darkImage = menu.grab().toImage().convertToFormat(QImage::Format_ARGB32);
  QVERIFY(!darkImage.isNull());
  QCOMPARE(darkImage.size(), image.size());
  QCOMPARE(darkImage.pixelColor(darkImage.rect().bottomRight()).alpha(), 0);
  if (!snapshotDirectory.isEmpty()) {
    QVERIFY(
        darkImage.save(QDir(snapshotDirectory).filePath(QStringLiteral("context-menu-dark.png"))));
  }
  menu.hide();
}

void ContextMenuTests::metricTokensRelayoutExistingActions() {
  QWidget window;
  window.resize(360, 240);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  AdContextMenu menu(&window);
  QAction* action = menu.addItem(QStringLiteral("Existing action"));
  menu.popupAt(window.mapToGlobal(QPoint(30, 30)));
  QTRY_VERIFY(menu.isVisible());
  const int defaultHeight = menu.actionGeometry(action).height();
  menu.hide();

  AdContextMenu::ComponentTokens tokens;
  tokens.itemHeight = defaultHeight + 12;
  tokens.minimumWidth = 260;
  menu.setComponentTokens(tokens);
  menu.popupAt(window.mapToGlobal(QPoint(30, 30)));
  QTRY_VERIFY(menu.isVisible());
  QCOMPARE(menu.actionGeometry(action).height(), defaultHeight + 12);
  QVERIFY(menu.width() >= 260);
  menu.hide();
}

void ContextMenuTests::longLabelsRespectTrailingColumnsInConstrainedMenus() {
  QWidget window;
  window.resize(360, 240);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  AdContextMenu menu(&window);
  menu.setFixedWidth(300);
  QAction* longAction = menu.addItem(
      QStringLiteral("A very long action label that must be elided before the trailing columns"),
      outlined_icons::Copy(), QKeySequence(Qt::CTRL | Qt::Key_C));
  AdContextMenu* submenu = menu.addSubMenu(QStringLiteral("Submenu"), outlined_icons::Folder());
  submenu->addItem(QStringLiteral("Child"));

  menu.popupAt(window.mapToGlobal(QPoint(30, 30)));
  QTRY_VERIFY(menu.isVisible());
  QTRY_VERIFY(menu.actionGeometry(longAction).isValid());

  QCOMPARE(menu.width(), 300);
  const QRect actionRect = menu.actionGeometry(longAction);
  QVERIFY(actionRect.width() < menu.fontMetrics().horizontalAdvance(longAction->text()));
  QVERIFY(menu.actionGeometry(submenu->menuAction()).right() < menu.width());
  menu.hide();
}

QTEST_MAIN(ContextMenuTests)

#include "context_menu_tests.moc"
