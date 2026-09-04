#include <QApplication>
#include <QPointer>
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
