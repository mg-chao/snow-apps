#include "widgets/popover.h"
#include "widgets/popup_surface_guard.h"
#include "widgets/color_picker.h"
#include "widgets/detail/top_level_popup_window.h"
#include "widgets/detail/overlay_popup_surface.h"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTest>
#include <QWindow>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
using adqt::widgets::AdPopover;

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void flush() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents();
}

class SurfaceMoveCounter final : public QObject {
 public:
  int moves = 0;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Move) {
      ++moves;
    }
    return QObject::eventFilter(watched, event);
  }
};

void globalPlacementOverrideHasOneGeometryAuthority() {
  QWidget host;
  QPushButton trigger(&host);
  host.setGeometry(10, 10, 200, 140);
  trigger.setGeometry(20, 20, 80, 25);
  host.show();
  flush();
  AdPopover popup;
  popup.setSourceWidget(&trigger);
  popup.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popup.setTriggers({});
  popup.setVisibilityPolicy(AdPopover::VisibilityPolicy::Manual);
  popup.setPlacement(AdPopover::Placement::BottomLeft);
  popup.setAutoAdjustOverflow(false);
  popup.setArrowVisible(false);
  popup.setPopupOffset(0);
  auto* content = new QWidget;
  content->setFixedSize(80, 25);
  popup.setContentWidget(content);
  const QRect available = host.screen()->availableGeometry();
  const QRect placement(available.center(), QSize(16, 8));
  popup.setAnchorGlobalRect(placement);
  popup.show();
  flush();
  auto* surface = qobject_cast<adqt::widgets::detail::OverlayPopupSurface*>(popup.surfaceWidget());
  require(surface && surface->isVisible(),
          "a visible source can place a tool popup beyond its own window");
  const auto bodyTopLeft = [surface]() { return surface->bodyWidget()->mapToGlobal(QPoint()); };
  const QPoint expected(placement.left(), placement.bottom() + 1);
  require(popup.anchorGlobalRect() == placement && bodyTopLeft() == expected,
          "global placement overrides geometry without changing the source anchor");
  SurfaceMoveCounter counter;
  surface->installEventFilter(&counter);
  AdPopover::resetSyncPopupGeometryCountersForTesting();
  for (int index = 0; index < 100; ++index) {
    popup.setAnchorGlobalRect(placement);
  }
  require(AdPopover::syncPopupGeometryCallCountForTesting() == 0 && counter.moves == 0,
          "repeated identical global anchors do not prepare or move the popup");
  for (int index = 0; index < 8; ++index) {
    popup.refreshPopupLayout();
    flush();
    require(bodyTopLeft() == expected, "framework refresh preserves the supplied global placement");
  }
  require(counter.moves == 0, "settled placement refreshes never oscillate the native surface");
  QRect desktop;
  for (QScreen* screen : QApplication::screens()) {
    desktop = desktop.united(screen->geometry());
  }
  const QRect outsideDesktop(desktop.bottomRight() + QPoint(4096, 4096), QSize(16, 8));
  require(!QApplication::screenAt(outsideDesktop.center()),
          "fallback fixture lies outside every screen");
  popup.setAnchorGlobalRect(outsideDesktop);
  flush();
  const QRect bodyBounds(bodyTopLeft(), surface->bodyWidget()->size());
  require(available.contains(bodyBounds),
          "off-screen overrides fall back to the actual anchor screen and remain screen-clamped");
  popup.setAnchorGlobalRect(std::nullopt);
  flush();
  const QRect widgetAnchor(trigger.mapToGlobal(QPoint()), trigger.size());
  require(!popup.anchorGlobalRect() &&
              bodyTopLeft() == QPoint(widgetAnchor.left(), widgetAnchor.bottom() + 1),
          "clearing the global override restores ordinary widget placement");
  AdPopover embedded;
  embedded.setSourceWidget(&trigger);
  embedded.setTriggers({});
  embedded.setPlacement(AdPopover::Placement::BottomLeft);
  embedded.setAutoAdjustOverflow(false);
  embedded.setArrowVisible(false);
  embedded.setPopupOffset(0);
  auto* embeddedContent = new QWidget;
  embeddedContent->setFixedSize(60, 20);
  embedded.setContentWidget(embeddedContent);
  embedded.setAnchorGlobalRect(placement);
  embedded.show();
  flush();
  auto* embeddedSurface =
      qobject_cast<adqt::widgets::detail::OverlayPopupSurface*>(embedded.surfaceWidget());
  require(embeddedSurface && !embeddedSurface->isWindow() &&
              embeddedSurface->bodyWidget()->mapToGlobal(QPoint()) ==
                  QPoint(widgetAnchor.left(), widgetAnchor.bottom() + 1),
          "in-window popovers retain widget placement while a global override is configured");
  embedded.hide();
  popup.setAnchorGlobalRect(placement);
  trigger.hide();
  flush();
  popup.refreshPopupLayout();
  require(!surface->isVisible(),
          "global placement never bypasses the actual source widget visibility");
}

void hiddenPreparationAndNativeRetention() {
  QWidget host;
  QPushButton trigger(&host);
  host.resize(400, 260);
  trigger.setGeometry(80, 60, 80, 30);
  host.show();
  flush();
  AdPopover popup;
  popup.setSourceWidget(&trigger);
  popup.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popup.setTriggers({});
  int contents = 0;
  int visibilityChanges = 0;
  popup.setContentFactory([&contents]() {
    ++contents;
    return new QLabel(QStringLiteral("Meter"));
  });
  QObject::connect(&popup, &AdPopover::visibleChanged,
                   [&visibilityChanges](bool) { ++visibilityChanges; });
  require(!popup.surfaceWidget() && contents == 0 && !popup.retainNativeSurfaceOnHide(),
          "surface accessor is lazy and retention defaults off");
  popup.setRetainNativeSurfaceOnHide(true);
  popup.preparePopup();
  QWidget* surface = popup.surfaceWidget();
  require(surface && !surface->isVisible() && !popup.isVisible() && visibilityChanges == 0,
          "native preparation never flashes or enters visible lifecycle");
  const WId identity = surface->internalWinId();
  require(identity && contents == 1, "opt-in preparation creates hidden native identity once");
  popup.show();
  require(surface->isVisible(), "prepared retained popup opens");
  popup.hide();
  flush();
  require(surface->internalWinId() == identity && contents == 1,
          "retained native window survives hide without recreating content");
  // A release posted before enabling retention must recheck at execution time.
  popup.setRetainNativeSurfaceOnHide(false);
  popup.setRetainNativeSurfaceOnHide(true);
  flush();
  require(surface->internalWinId() == identity, "queued release respects newly enabled retention");
  popup.setRetainNativeSurfaceOnHide(false);
  flush();
  require(!surface->internalWinId(), "ending retention releases an already hidden native surface");
}

void showGuardGatesInitialAndVisibleNativeTransitions() {
  QWidget host;
  QPushButton trigger(&host);
  host.resize(400, 260);
  trigger.setGeometry(80, 60, 80, 30);
  host.show();
  flush();
  AdPopover popup;
  popup.setSourceWidget(&trigger);
  popup.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popup.setTriggers({});
  popup.setText(QStringLiteral("Meter"));
  popup.setRetainNativeSurfaceOnHide(true);
  bool allow = false;
  int guards = 0;
  popup.setSurfaceShowGuard([&](QWidget* surface) {
    require(surface && surface->internalWinId(), "guard receives prepared native identity");
    ++guards;
    return allow;
  });
  popup.show();
  QWidget* surface = popup.surfaceWidget();
  require(popup.isVisible() && surface && !surface->isVisible() && guards > 0,
          "unacknowledged native identity remains hidden while request stays open");
  allow = true;
  popup.refreshPopupLayout();
  require(surface->isVisible(), "acknowledged request can retry without reopening");
  allow = false;
  QEvent nativeChange(QEvent::WinIdChange);
  QApplication::sendEvent(surface, &nativeChange);
  flush();
  require(popup.isVisible() && !surface->isVisible(),
          "visible identity changes hide surface without losing requested visibility");
  allow = true;
  popup.refreshPopupLayout();
  require(surface->isVisible(), "changed identity opens only after renewed acknowledgment");
  popup.hide();
  allow = true;
  popup.refreshPopupLayout();
  require(!popup.isVisible() && !surface->isVisible(),
          "stale completion cannot reopen cancelled request");
  popup.setSurfaceShowGuard({});
  popup.show();
  require(surface->isVisible(), "default guard preserves existing popup behavior");
}

void scopedGuardCoversLazyNestedAndReplacementSurfaces() {
  QWidget host;
  QPushButton trigger(&host);
  host.resize(400, 260);
  trigger.setGeometry(80, 60, 80, 30);
  host.show();
  flush();
  bool allow = false;
  int checks = 0;
  int requests = 0;
  auto guard = std::make_unique<adqt::widgets::AdPopupSurfaceGuard>(&host, [&](QWidget* surface) {
    require(surface && surface->internalWinId(), "scoped guard receives native surface");
    ++checks;
    return allow;
  });
  QObject::connect(guard.get(), &adqt::widgets::AdPopupSurfaceGuard::surfaceRequested,
                   [&](QWidget*) { ++requests; });
  AdPopover popup;
  popup.setSourceWidget(&trigger);
  popup.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popup.setTriggers({});
  auto* nestedTrigger = new QPushButton(QStringLiteral("Nested"));
  popup.setContentWidget(nestedTrigger);
  popup.show();
  auto* surface = popup.surfaceWidget();
  require(surface && popup.isVisible() && !surface->isVisible() && checks > 0 && requests == 1,
          "popups created after scope guard installation await acknowledgment");
  allow = true;
  guard->refresh();
  require(surface->isVisible(), "scope refresh releases acknowledged request");
  const WId identity = surface->internalWinId();
  popup.hide();
  flush();
  require(surface->internalWinId() == identity, "scope guard retains acknowledged native identity");
  popup.show();
  AdPopover nested;
  nested.setSourceWidget(nestedTrigger);
  nested.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  nested.setTriggers({});
  nested.setText(QStringLiteral("Nested popup"));
  allow = false;
  surface->windowHandle()->setTransientParent(nullptr);
  nested.show();
  require(nested.surfaceWidget() && !nested.surfaceWidget()->isVisible() && requests == 3,
          "detached nested popup inherits its logical owner's guard during native replacement");
  allow = true;
  guard->refresh();
  require(nested.surfaceWidget()->isVisible(), "scope refresh includes nested requested popups");
  allow = false;
  QEvent nativeChange(QEvent::WinIdChange);
  QApplication::sendEvent(nested.surfaceWidget(), &nativeChange);
  require(nested.isVisible() && !nested.surfaceWidget()->isVisible(),
          "native identity replacement reapplies the inherited guard");
  nested.hide();
  allow = true;
  guard->refresh();
  require(!nested.isVisible() && !nested.surfaceWidget()->isVisible(),
          "late acknowledgment never reopens a cancelled nested request");
  popup.hide();

  adqt::widgets::AdColorPicker picker(&host);
  picker.setGeometry(180, 60, 80, 30);
  picker.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  picker.show();
  allow = false;
  picker.setPopupVisible(true);
  auto* colorPopover = picker.findChild<AdPopover*>();
  require(
      colorPopover && colorPopover->surfaceWidget() && !colorPopover->surfaceWidget()->isVisible(),
      "color picker popovers use the same scoped native show policy");
  allow = true;
  guard->refresh();
  require(colorPopover->surfaceWidget()->isVisible(), "scope refresh releases color picker popup");
  picker.setPopupVisible(false);

  guard.reset();
  flush();
  require(!surface->internalWinId(), "removing scope guard releases hidden native resources");
  popup.show();
  require(surface->isVisible(), "removing scope guard restores ordinary popup visibility");
}
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  if (app.arguments().contains(QStringLiteral("--global-anchor-only"))) {
    globalPlacementOverrideHasOneGeometryAuthority();
    return 0;
  }
  globalPlacementOverrideHasOneGeometryAuthority();
  hiddenPreparationAndNativeRetention();
  showGuardGatesInitialAndVisibleNativeTransitions();
  scopedGuardCoversLazyNestedAndReplacementSurfaces();
  return 0;
}
