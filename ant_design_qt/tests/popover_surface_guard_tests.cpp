#include "widgets/popover.h"
#include "widgets/detail/top_level_popup_window.h"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <QWindow>

#include <cstdlib>
#include <iostream>

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
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  hiddenPreparationAndNativeRetention();
  showGuardGatesInitialAndVisibleNativeTransitions();
  return 0;
}
