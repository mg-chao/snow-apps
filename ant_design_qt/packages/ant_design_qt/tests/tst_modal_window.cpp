#include <QApplication>
#include <QPointer>
#include <QScreen>
#include <QLabel>
#include <QWidget>
#include <QtTest>

#include "widgets/modal.h"

using adqt::widgets::AdModal;

namespace {

constexpr auto kOverlayObjectName = "ad-modal-overlay";

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
