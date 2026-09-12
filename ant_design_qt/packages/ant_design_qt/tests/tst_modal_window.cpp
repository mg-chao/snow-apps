#include <QApplication>
#include <QLayout>
#include <QPointer>
#include <QScreen>
#include <QLabel>
#include <QToolButton>
#include <QWidget>
#include <QtTest>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "widgets/modal.h"

using adqt::widgets::AdModal;

namespace {

constexpr auto kOverlayObjectName = "ad-modal-overlay";

class SurfaceLifecycleObserver : public QObject {
 public:
  int disruptions = 0;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Hide || event->type() == QEvent::Show ||
        event->type() == QEvent::WinIdChange) {
      ++disruptions;
    }
    return QObject::eventFilter(watched, event);
  }
};

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

  // Component tokens must pad only the modal's content area: the content
  // widget tracks the token, while the header section stays exactly where the
  // theme insets place it.
  void contentPaddingTokensPadOnlyTheContentArea() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowScreen(qApp->primaryScreen());
    modal.setWindowPreferredSize(QSize(480, 360));
    modal.setWindowResizable(true);
    modal.setFooterVisible(false);
    modal.setWindowTitle(QStringLiteral("Content padding"));
    auto* content = new QLabel(QStringLiteral("Content"));
    modal.setContentWidget(content);
    modal.open();
    QWidget* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface != nullptr);
    auto* header = surface->findChild<QWidget*>(QStringLiteral("ad-modal-header"));
    QVERIFY(header != nullptr);

    AdModal::ComponentTokens tokens;
    tokens.bodyPaddingHorizontal = 0;
    tokens.bodyPaddingVertical = 0;
    tokens.contentPaddingHorizontal = 30;
    tokens.contentPaddingVertical = 26;
    modal.setComponentTokens(tokens);
    qApp->processEvents();
    const QPoint headerWithLargePadding = header->mapTo(surface, QPoint(0, 0));
    const QPoint contentWithLargePadding = content->mapTo(surface, QPoint(0, 0));

    tokens.contentPaddingHorizontal = 7;
    tokens.contentPaddingVertical = 5;
    modal.setComponentTokens(tokens);
    qApp->processEvents();
    const QPoint contentWithSmallPadding = content->mapTo(surface, QPoint(0, 0));

    QCOMPARE(contentWithLargePadding.x() - contentWithSmallPadding.x(), 23);
    QCOMPARE(contentWithLargePadding.y() - contentWithSmallPadding.y(), 21);
    QCOMPARE(contentWithSmallPadding.x(), 7);
    // The header is the first panel item, so its position follows only the
    // theme insets and must not move when the content area is re-padded.
    QCOMPARE(header->mapTo(surface, QPoint(0, 0)), headerWithLargePadding);
    modal.close();
  }

  // The header bottom margin token gaps the header and the content area:
  // the content moves by the token while the header itself stays put. A
  // non-resizable surface keeps sections at their hint sizes, so the margin
  // is not absorbed by extra-space redistribution.
  void headerMarginBottomTokenGapsHeaderAndContent() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowScreen(qApp->primaryScreen());
    modal.setWindowPreferredSize(QSize(480, 360));
    modal.setFooterVisible(false);
    modal.setWindowTitle(QStringLiteral("Header margin"));
    auto* content = new QLabel(QStringLiteral("Content"));
    modal.setContentWidget(content);
    modal.open();
    QWidget* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface != nullptr);
    auto* header = surface->findChild<QWidget*>(QStringLiteral("ad-modal-header"));
    QVERIFY(header != nullptr);

    AdModal::ComponentTokens tokens;
    tokens.bodyPaddingHorizontal = 0;
    tokens.bodyPaddingVertical = 0;
    tokens.contentPaddingHorizontal = 7;
    tokens.contentPaddingVertical = 5;
    tokens.headerPaddingVertical = 0;
    tokens.headerMarginBottom = 12;
    modal.setComponentTokens(tokens);
    qApp->processEvents();
    const QPoint contentWithGap = content->mapTo(surface, QPoint(0, 0));
    const QMargins headerWithGapMargins = header->layout()->contentsMargins();

    tokens.headerMarginBottom = 0;
    modal.setComponentTokens(tokens);
    qApp->processEvents();
    const QPoint contentWithoutGap = content->mapTo(surface, QPoint(0, 0));
    const QMargins headerMarginsWithoutGap = header->layout()->contentsMargins();

    // The margin lands in the header layout's bottom edge; distribution of
    // leftover space may absorb geometry shifts, so assert the applied
    // margins rather than absolute content geometry.
    QCOMPARE(headerWithGapMargins.bottom() - headerMarginsWithoutGap.bottom(), 12);
    QCOMPARE(headerMarginsWithoutGap.bottom(), 0);
    QCOMPARE(headerMarginsWithoutGap.top(), headerWithGapMargins.top());
    QVERIFY(contentWithoutGap.y() <= contentWithGap.y());
    QCOMPARE(contentWithoutGap.x(), contentWithGap.x());
    modal.close();
  }

  // Taskbar visibility swaps the detached Qt::Tool surface for a plain
  // Qt::Window, and the extra chrome buttons drive minimize and always-on-top
  // without disturbing the window geometry.
  void windowChromeButtonsAndTaskbarSurface() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowScreen(qApp->primaryScreen());
    modal.setWindowPreferredSize(QSize(480, 360));
    modal.setWindowResizable(true);
    modal.setWindowTaskbarVisible(true);
    modal.setWindowMinimizeButtonVisible(true);
    modal.setWindowAlwaysOnTopButtonVisible(true);
    modal.setWindowTitle(QStringLiteral("Chrome"));
    auto* content = new QLabel(QStringLiteral("Content"));
    modal.setContentWidget(content);
    modal.open();
    QWidget* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(surface));

    QCOMPARE(surface->windowFlags() & Qt::WindowType_Mask, Qt::Window);
    auto* minimize = surface->findChild<QToolButton*>(QStringLiteral("ad-modal-minimize"));
    auto* pin = surface->findChild<QToolButton*>(QStringLiteral("ad-modal-always-on-top"));
    QVERIFY(minimize != nullptr);
    QVERIFY(pin != nullptr);
    QVERIFY(minimize->isVisible());
    QVERIFY(pin->isVisible());
    QVERIFY(!modal.windowAlwaysOnTop());
    QVERIFY(!pin->isChecked());

    const QRect geometry = surface->geometry();
    pin->click();
    QVERIFY(modal.windowAlwaysOnTop());
    QVERIFY(pin->isChecked());
    QVERIFY(surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QCOMPARE(surface->geometry(), geometry);

    minimize->click();
    QTRY_VERIFY(surface->isMinimized());
    modal.present();
    QTRY_VERIFY(!surface->isMinimized());
    QCOMPARE(surface->geometry(), geometry);
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
      const auto hwnd = reinterpret_cast<HWND>(surface->winId());
      QVERIFY((GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    }
#endif

    pin->click();
    QVERIFY(!modal.windowAlwaysOnTop());
    QVERIFY(!pin->isChecked());
    QVERIFY(!surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QCOMPARE(surface->geometry(), geometry);

    // Toggling after the surface exists must keep the taskbar surface type.
    modal.setWindowTaskbarVisible(false);
    QCOMPARE(surface->windowFlags() & Qt::WindowType_Mask, Qt::Tool);
    modal.setWindowTaskbarVisible(true);
    QCOMPARE(surface->windowFlags() & Qt::WindowType_Mask, Qt::Window);
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
      const auto hwnd = reinterpret_cast<HWND>(surface->winId());
      const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
      QCOMPARE(style & (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME),
               LONG_PTR(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME));
      QCOMPARE(GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED, LONG_PTR(0));
    }
#endif
    modal.close();
  }

  void alwaysOnTopPreservesSurfaceAndNativeChrome() {
    AdModal modal;
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModeDetached(true);
    modal.setWindowModality(Qt::NonModal);
    modal.setWindowTaskbarVisible(true);
    modal.setWindowTitle(QStringLiteral("Stable stacking"));
    modal.open();
    QWidget* surface = visibleOverlaySurface(modal.windowTitle());
    QVERIFY(surface);
    QVERIFY(QTest::qWaitForWindowExposed(surface));
    const WId id = surface->winId();
    const QRect geometry = surface->geometry();
    SurfaceLifecycleObserver observer;
    surface->installEventFilter(&observer);
    for (bool pinned : {true, false, true, false}) {
      modal.setWindowAlwaysOnTop(pinned);
      QCoreApplication::processEvents();
      QCOMPARE(observer.disruptions, 0);
      QCOMPARE(surface->winId(), id);
      QCOMPARE(surface->geometry(), geometry);
      QVERIFY(surface->isVisible());
      QCOMPARE(surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint), pinned);
#ifdef Q_OS_WIN
      if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const auto hwnd = reinterpret_cast<HWND>(id);
        const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        QCOMPARE((exStyle & WS_EX_TOPMOST) != 0, pinned);
        QCOMPARE(style & (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME),
                 LONG_PTR(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME));
        QCOMPARE(exStyle & WS_EX_LAYERED, LONG_PTR(0));
      }
#endif
    }
    surface->removeEventFilter(&observer);
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
