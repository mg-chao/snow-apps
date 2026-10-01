#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFrame>
#include <QImage>
#include <QPalette>
#include <QPainter>
#include <QSignalSpy>
#include <QStyleOption>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include "antd_icons.h"
#include "widgets/context_menu.h"

using adqt::widgets::AdContextMenu;
namespace outlined_icons = adqt::icons::antd::outlined;
#ifdef Q_OS_MACOS
namespace twotone_icons = adqt::icons::antd::twotone;

namespace {
bool containsOpaqueColor(const QImage& image, const QColor& color) {
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (pixel.alpha() == 255 && pixel.rgb() == color.rgb()) {
        return true;
      }
    }
  }
  return false;
}
}  // namespace
#endif

class ContextMenuTests final : public QObject {
  Q_OBJECT

 private slots:
  void actionMetadataAndNativeStateCoexist();
  void iconsCanBeReplacedAndCleared();
  void popupDismissalPreservesActions();
  void macMenuUsesPlatformDefaults();
  void macMultitoneIconsUseMenuForeground();
  void macIconUpdatesLeaveOtherActionsUnchanged();
  void triggerWidgetOpensOnContextMenuEvent();
  void rebindingStopsHandlingTheOldWidget();
  void keyboardActivationUsesNativeMenuBehavior();
  void menuUsesCompactAntMetrics();
  void metricTokensRelayoutExistingActions();
  void longLabelsRespectTrailingColumnsInConstrainedMenus();
  void widgetMenuHonorsComponentTokens();
  void widgetSurfaceFollowsExistingSubmenus();
  void preparedBackgroundClipsAndComposites();
  void preparedBackgroundPreservesItemStates();
  void preparedBackgroundDoesNotSelectNativeSurface();
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

void ContextMenuTests::iconsCanBeReplacedAndCleared() {
  const bool previous = QCoreApplication::testAttribute(Qt::AA_DontShowIconsInMenus);
  QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, true);
  AdContextMenu menu;
  QAction* action = menu.addItem(QStringLiteral("Edit"), outlined_icons::Edit());
  QVERIFY(action->isIconVisibleInMenu());
  QVERIFY(!action->icon().pixmap(16, 16).isNull());
#ifdef Q_OS_MACOS
  QVERIFY(action->icon().isMask());
#endif
  const auto colored =
      outlined_icons::Copy().withColors(adqt::icons::IconColors::primary(QColor(Qt::red)));
  menu.setActionIcon(action, colored);
  QCOMPARE(menu.actionIcon(action), colored);
  QVERIFY(!action->icon().isMask());
  const auto* submenu = menu.addSubMenu(QStringLiteral("Folder"), outlined_icons::Folder());
  QVERIFY(submenu->menuAction()->isIconVisibleInMenu());
  QVERIFY(!submenu->menuAction()->icon().isNull());
  menu.setActionIcon(action, {});
  QVERIFY(!menu.actionIcon(action).isValid());
  QVERIFY(action->icon().isNull());
  QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, previous);
}

void ContextMenuTests::popupDismissalPreservesActions() {
  AdContextMenu menu;
  QAction* action = menu.addItem(QStringLiteral("Keep"));
  menu.popupAt(QPoint(20, 20));
  QVERIFY(menu.isPopupVisible());
  menu.dismissPopup();
  QCoreApplication::processEvents();
  QVERIFY(!menu.isPopupVisible());
  QCOMPARE(menu.actions(), QList<QAction*>{action});
  menu.popupAt(QPoint(20, 20));
  QVERIFY(menu.isPopupVisible());
  menu.dismissPopup();
  auto* transient = new AdContextMenu;
  transient->addItem(QStringLiteral("Temporary"));
  transient->popupAt(QPoint(20, 20));
  delete transient;
  QCoreApplication::processEvents();
}

void ContextMenuTests::macMenuUsesPlatformDefaults() {
#ifdef Q_OS_MACOS
  AdContextMenu menu;
  QMenu baseline;
  QVERIFY(!menu.testAttribute(Qt::WA_TranslucentBackground));
  QCOMPARE(menu.windowFlags(), baseline.windowFlags());
  QCOMPARE(menu.style(), baseline.style());
  const auto* action = menu.addItem(QStringLiteral("Native size"));
  baseline.addAction(action->text());
  const QSize original = menu.sizeHint();
  AdContextMenu::ComponentTokens tokens;
  tokens.itemHeight = 100;
  tokens.minimumWidth = 600;
  menu.setComponentTokens(tokens);
  menu.setColorScheme(AdContextMenu::ColorScheme::Dark);
  menu.setActionDanger(menu.actions().first());
  QCOMPARE(menu.sizeHint(), original);
  QCOMPARE(menu.sizeHint(), baseline.sizeHint());
#endif
}

void ContextMenuTests::macMultitoneIconsUseMenuForeground() {
#ifdef Q_OS_MACOS
  AdContextMenu menu;
  QAction* action = menu.addItem(QStringLiteral("Camera"), twotone_icons::Camera());
  const QColor foreground = menu.palette().color(QPalette::Active, QPalette::Text);
  const QColor defaultAccent(QStringLiteral("#1677ff"));
  const QImage image = action->icon().pixmap(QSize(32, 32), QIcon::Normal).toImage();
  QVERIFY(containsOpaqueColor(image, foreground));
  QVERIFY(!containsOpaqueColor(image, defaultAccent) || foreground.rgb() == defaultAccent.rgb());
  QPalette palette = menu.palette();
  const QColor updatedForeground(Qt::magenta);
  palette.setColor(QPalette::Active, QPalette::Text, updatedForeground);
  menu.setPalette(palette);
  QVERIFY(containsOpaqueColor(action->icon().pixmap(QSize(32, 32)).toImage(), updatedForeground));
#endif
}

void ContextMenuTests::macIconUpdatesLeaveOtherActionsUnchanged() {
#ifdef Q_OS_MACOS
  AdContextMenu menu;
  QAction* first = menu.addItem(QStringLiteral("First"), outlined_icons::Edit());
  QSignalSpy changed(first, &QAction::changed);
  QAction* second = menu.addItem(QStringLiteral("Second"), outlined_icons::Copy());
  menu.setActionIcon(second, outlined_icons::Folder());
  menu.setActionIcon(second, {});
  QVERIFY(changed.isEmpty());
  QVERIFY(first->icon().isMask());
  QVERIFY(!first->icon().pixmap(16, 16).isNull());
#endif
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
#ifdef Q_OS_MACOS
  QSKIP("macOS controls menu metrics and painting");
#endif
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
#ifdef Q_OS_MACOS
  QSKIP("macOS controls menu metrics and painting");
#endif
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
#ifdef Q_OS_MACOS
  QSKIP("macOS controls menu metrics and painting");
#endif
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

void ContextMenuTests::widgetMenuHonorsComponentTokens() {
#ifdef Q_OS_MACOS
  AdContextMenu nativeMenu;
  QVERIFY(nativeMenu.nativeMenuEnabled());
  QVERIFY(!nativeMenu.testAttribute(Qt::WA_TranslucentBackground));
#endif
  AdContextMenu menu;
  menu.setNativeMenuEnabled(false);
  QVERIFY(!menu.nativeMenuEnabled());
  menu.setNativeMenuEnabled(true);
#ifdef Q_OS_MACOS
  QVERIFY(menu.nativeMenuEnabled());
  menu.setNativeMenuEnabled(false);
#else
  QVERIFY(!menu.nativeMenuEnabled());
#endif
  QVERIFY(menu.testAttribute(Qt::WA_TranslucentBackground));
  menu.addItem(QStringLiteral("Shared metrics"));
  AdContextMenu* submenu = menu.addSubMenu(QStringLiteral("More"));
  QVERIFY(submenu);
  QVERIFY(!submenu->nativeMenuEnabled());
  const QSize original = menu.sizeHint();
  AdContextMenu::ComponentTokens tokens;
  tokens.itemHeight = 80;
  tokens.minimumWidth = 420;
  menu.setComponentTokens(tokens);
  QVERIFY(menu.sizeHint().height() >= 80);
  QVERIFY(menu.sizeHint().width() >= 420);
  QVERIFY(menu.sizeHint() != original);
}

void ContextMenuTests::widgetSurfaceFollowsExistingSubmenus() {
  AdContextMenu menu;
#ifdef Q_OS_MACOS
  QVERIFY(menu.nativeMenuEnabled());
#endif
  const auto pinIcon = outlined_icons::Edit();
  QAction* action = menu.addItem(QStringLiteral("Pin"), pinIcon);
  AdContextMenu* submenu = menu.addSubMenu(QStringLiteral("More"), outlined_icons::Folder());
  QVERIFY(submenu != nullptr);
  QAction* child = submenu->addItem(QStringLiteral("Child"), outlined_icons::Edit());
#ifdef Q_OS_MACOS
  QVERIFY(submenu->nativeMenuEnabled());
  QVERIFY(action->icon().isMask());
#endif
  menu.setNativeMenuEnabled(false);
  QVERIFY(!menu.nativeMenuEnabled());
  QVERIFY(!submenu->nativeMenuEnabled());
  QVERIFY(menu.testAttribute(Qt::WA_TranslucentBackground));
  QVERIFY(submenu->testAttribute(Qt::WA_TranslucentBackground));
  QCOMPARE(menu.actionIcon(action), pinIcon);
  QCOMPARE(menu.actionIcon(submenu->menuAction()), outlined_icons::Folder());
  QCOMPARE(submenu->actionIcon(child), outlined_icons::Edit());
  QVERIFY(!action->icon().pixmap(16, 16).isNull());
  QVERIFY(!submenu->menuAction()->icon().pixmap(16, 16).isNull());
  QVERIFY(!child->icon().pixmap(16, 16).isNull());
#ifdef Q_OS_MACOS
  QVERIFY(!action->icon().isMask());
#endif
}

void ContextMenuTests::preparedBackgroundClipsAndComposites() {
  AdContextMenu menu;
  menu.setNativeMenuEnabled(false);
  menu.resize(200, 120);
  AdContextMenu::ComponentTokens tokens;
  tokens.background = Qt::white;
  tokens.border = Qt::blue;
  tokens.borderRadius = 12;
  menu.setComponentTokens(tokens);
  QPixmap skin(20, 20);
  skin.fill(Qt::red);
  const auto renderPanel = [&]() {
    QImage result(menu.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::black);
    QPainter painter(&result);
    QStyleOption option;
    option.initFrom(&menu);
    option.rect = menu.rect();
    menu.style()->drawPrimitive(QStyle::PE_PanelMenu, &option, &painter, &menu);
    return result;
  };

  menu.setBackgroundFrame({skin, QRectF(0.25, 0.25, 0.5, 0.5), 1.0, 0.0});
  QImage rendered = renderPanel();
  QCOMPARE(rendered.pixelColor(100, 60), QColor(Qt::red));
  QCOMPARE(rendered.pixelColor(20, 60), QColor(Qt::white));
  QCOMPARE(rendered.pixelColor(0, 0).alpha(), 0);
  QCOMPARE(rendered.pixelColor(199, 119).alpha(), 0);

  menu.setBackgroundFrame({skin, QRectF(0.0, 0.0, 1.0, 1.0), 0.5, 0.5});
  rendered = renderPanel();
  const QColor blended = rendered.pixelColor(100, 60);
  QCOMPARE(blended.red(), 255);
  QVERIFY(qAbs(blended.green() - 191) <= 1);
  QVERIFY(qAbs(blended.blue() - 191) <= 1);
  QCOMPARE(blended.alpha(), 255);
  QCOMPARE(rendered.pixelColor(0, 0).alpha(), 0);

  menu.resetBackgroundFrame();
  QVERIFY(menu.backgroundFrame().image.isNull());
  QCOMPARE(renderPanel().pixelColor(100, 60), QColor(Qt::white));
  menu.setBackgroundFrame({skin, QRectF(), 1.0, 0.0});
  QVERIFY(menu.backgroundFrame().image.isNull());
}

void ContextMenuTests::preparedBackgroundPreservesItemStates() {
  AdContextMenu menu;
  menu.setNativeMenuEnabled(false);
  AdContextMenu::ComponentTokens tokens;
  tokens.background = Qt::white;
  tokens.hoverBackground = Qt::green;
  tokens.dangerHoverBackground = Qt::yellow;
  tokens.itemBorderRadius = 0;
  menu.setComponentTokens(tokens);
  QAction* normal = menu.addItem(QStringLiteral("Normal"));
  QAction* danger = menu.addItem(QStringLiteral("Danger"));
  menu.setActionDanger(danger);
  QPixmap skin(20, 20);
  skin.fill(Qt::red);
  menu.setBackgroundFrame({skin, QRectF(0.0, 0.0, 1.0, 1.0), 1.0, 0.0});
  menu.ensurePolished();
  menu.adjustSize();
  const auto renderItem = [&](QAction* action) {
    QImage result(menu.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    QStyleOptionMenuItem option;
    option.initFrom(&menu);
    option.rect = menu.actionGeometry(action);
    option.state |= QStyle::State_Selected | QStyle::State_Enabled;
    option.menuItemType = QStyleOptionMenuItem::Normal;
    option.text = action->text();
    menu.style()->drawControl(QStyle::CE_MenuItem, &option, &painter, &menu);
    return result.pixelColor(option.rect.left() + 2, option.rect.center().y());
  };
  QCOMPARE(renderItem(normal), QColor(Qt::green));
  QCOMPARE(renderItem(danger), QColor(Qt::yellow));
  QVERIFY(!normal->isCheckable());
  QVERIFY(menu.actionDanger(danger));
}

void ContextMenuTests::preparedBackgroundDoesNotSelectNativeSurface() {
  AdContextMenu menu;
  const bool native = menu.nativeMenuEnabled();
  QStyle* originalStyle = menu.style();
  QPixmap skin(10, 10);
  skin.fill(Qt::red);
  menu.setBackgroundFrame({skin, QRectF(0.0, 0.0, 1.0, 1.0), 1.0, 0.0});
  QCOMPARE(menu.nativeMenuEnabled(), native);
  QCOMPARE(menu.style(), originalStyle);
  menu.resetBackgroundFrame();
  QCOMPARE(menu.nativeMenuEnabled(), native);
}

QTEST_MAIN(ContextMenuTests)

#include "context_menu_tests.moc"
