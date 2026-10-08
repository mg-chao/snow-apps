#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/modal.h"
#include "widgets/popconfirm.h"
#include "widgets/radio.h"
#include "widgets/switch.h"

#include <QApplication>
#include <QFocusEvent>
#include <QImage>
#include <QTest>
#include <QVBoxLayout>

#include <iostream>
#include <stdexcept>

namespace {

using namespace adqt::widgets;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void settle() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents();
}

template <typename Control>
void focusOutlineRequiresKeyboardInput() {
  QWidget owner;
  owner.resize(240, 140);
  Control control(&owner);
  control.setGeometry(40, 40, 120, 40);
  owner.show();
  owner.activateWindow();
  settle();
  const auto imageForFocus = [&](Qt::FocusReason reason) {
    control.clearFocus();
    control.setFocus(reason);
    settle();
    require(control.hasFocus(), "focus fixture must focus the control");
    return owner.grab().toImage();
  };
  const QImage mouse = imageForFocus(Qt::MouseFocusReason);
  for (const auto reason : {Qt::OtherFocusReason, Qt::PopupFocusReason, Qt::ActiveWindowFocusReason,
                            Qt::MenuBarFocusReason, Qt::NoFocusReason}) {
    require(imageForFocus(reason) == mouse,
            "programmatic, popup and window focus must not paint a keyboard outline");
  }
  for (const auto reason : {Qt::TabFocusReason, Qt::BacktabFocusReason, Qt::ShortcutFocusReason}) {
    require(imageForFocus(reason) != mouse,
            "Tab, Shift+Tab and shortcut focus must paint a keyboard outline");
  }
}

void modalDismissalPreservesCurrentFocus(AdModal::Mode mode) {
  QWidget owner;
  auto* layout = new QVBoxLayout(&owner);
  AdButton trigger(QStringLiteral("Open"));
  AdButton current(QStringLiteral("Current"));
  layout->addWidget(&trigger);
  layout->addWidget(&current);
  owner.show();
  owner.activateWindow();
  settle();
  trigger.setFocus(Qt::TabFocusReason);
  AdModal modal(&owner);
  modal.setMode(mode);
  modal.setWindowModality(Qt::NonModal);
  modal.open();
  settle();
  owner.activateWindow();
  current.setFocus(Qt::MouseFocusReason);
  settle();
  require(current.hasFocus(), "modal fixture must establish a new focus target");
  modal.reject();
  settle();
  require(current.hasFocus(), "closing a modal must not refocus its previous trigger");
}

void popconfirmDismissalPreservesCurrentFocus(bool action) {
  QWidget owner;
  auto* layout = new QVBoxLayout(&owner);
  AdButton trigger(QStringLiteral("Open"));
  AdButton current(QStringLiteral("Current"));
  layout->addWidget(&trigger);
  layout->addWidget(&current);
  owner.show();
  owner.activateWindow();
  settle();
  AdPopconfirm popup(&owner);
  popup.setSourceWidget(&trigger);
  popup.setPopupLayerMode(AdPopconfirm::PopupLayerMode::InWindow);
  popup.setText(QStringLiteral("Confirm"));
  popup.setDefaultButton(AdPopconfirm::StandardButton::Cancel);
  popup.show();
  settle();
  auto* cancel = popup.button(AdPopconfirm::StandardButton::Cancel);
  require(cancel && cancel->hasFocus(), "popconfirm fixture must establish popup focus");
  current.setFocus(Qt::MouseFocusReason);
  require(current.hasFocus(), "popconfirm fixture must establish a new focus target");
  if (action) {
    cancel->click();
  } else {
    popup.hide();
  }
  settle();
  require(!popup.isVisible(), "dismissal must close the popconfirm");
  require(current.hasFocus(), "closing a popconfirm must not refocus its source button");
}

void modalTabNavigationRemainsAvailable() {
  QWidget owner;
  owner.show();
  AdModal modal(&owner);
  modal.open();
  settle();
  auto* initial = QApplication::focusWidget();
  require(initial, "an open modal must establish initial focus");
  QTest::keyClick(initial, Qt::Key_Tab);
  require(QApplication::focusWidget() != initial,
          "Tab must still move keyboard focus inside a modal");
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab, Qt::ShiftModifier);
  require(QApplication::focusWidget() == initial,
          "Shift+Tab must still return to the initial modal control");
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  try {
    // Check dismissal first so the original focus handoff is reproduced independently
    // of the focus outline classification.
    modalDismissalPreservesCurrentFocus(AdModal::Mode::Overlay);
    modalDismissalPreservesCurrentFocus(AdModal::Mode::Window);
    popconfirmDismissalPreservesCurrentFocus(false);
    popconfirmDismissalPreservesCurrentFocus(true);
    focusOutlineRequiresKeyboardInput<AdButton>();
    focusOutlineRequiresKeyboardInput<AdCheckbox>();
    focusOutlineRequiresKeyboardInput<AdRadio>();
    focusOutlineRequiresKeyboardInput<AdSwitch>();
    modalTabNavigationRemainsAvailable();
    std::cout << "Focus interaction tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
