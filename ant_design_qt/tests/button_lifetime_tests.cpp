#include "widgets/button.h"
#include "widgets/button_style.h"
#include "theme/theme_types.h"
#include "widgets/popover.h"

#include <QApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QEventLoop>
#include <QTimer>
#include <QPointer>
#include <QWidget>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void cursorOverlayMayBeDestroyedBeforeButton() {
  auto window = std::make_unique<QWidget>();
  window->resize(320, 200);
  auto* button = new adqt::widgets::AdButton(window.get());
  button->setGeometry(10, 10, 80, 32);
  window->show();
  button->setDisabled(true);
  QApplication::processEvents();
  QPointer<QWidget> overlay =
      window->findChild<QWidget*>(QStringLiteral("ad-button-disabled-cursor-overlay"));
  require(overlay != nullptr, "disabled button must create its cursor overlay");
  button->setEnabled(true);
  button->raise();
  require(window->children().indexOf(overlay) < window->children().indexOf(button),
          "raising the button must place its sibling overlay earlier in destruction order");
  window.reset();
  require(overlay.isNull(), "destroying the window must release the cursor overlay");
  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void activationMayDestroyButton() {
  using adqt::widgets::AdButton;
  for (const bool nestedLoop : {false, true}) {
    for (const int key : {0, int(Qt::Key_Space), int(Qt::Key_Return), int(Qt::Key_Enter)}) {
      QPointer<AdButton> button = new AdButton;
      button->resize(100, 32);
      button->show();
      QApplication::processEvents();
      QObject::connect(button, &AdButton::clicked, qApp, [button, nestedLoop] {
        if (nestedLoop) {
          QEventLoop loop;
          QTimer::singleShot(0, &loop, [&] {
            delete button.data();
            loop.quit();
          });
          loop.exec();
        } else {
          delete button.data();
        }
      });
      if (key == 0) {
        const QPointF local = button->rect().center();
        const QPointF global = button->mapToGlobal(local.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(button, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(button, &release);
      } else {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(button, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(button, &release);
      }
      require(!button, "activation must tolerate deletion before the event handler returns");
    }
  }
  QPointer<AdButton> button = new AdButton;
  button->resize(100, 32);
  QObject::connect(button, &AdButton::pressed, qApp, [button] { delete button.data(); });
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10), Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(button, &press);
  require(!button, "pressed callbacks may also delete the button");
}

void retainedPopoverButtonsForgetHover() {
  using adqt::widgets::AdButton;
  using adqt::widgets::AdPopover;
  QWidget window;
  window.setGeometry(100, 100, 320, 200);
  AdButton trigger(&window);
  trigger.setGeometry(20, 20, 80, 32);
  AdPopover popover(&window);
  popover.setSourceWidget(&trigger);
  popover.setTriggers(AdPopover::Trigger::Click);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  auto* content = new QWidget;
  auto* layout = new QHBoxLayout(content);
  auto* first = new AdButton(content);
  auto* second = new AdButton(content);
  for (auto* button : {first, second}) {
    button->setFixedSize(32, 32);
    button->setFocusPolicy(Qt::NoFocus);
    button->setCheckable(true);
    button->setButtonStyle(AdButton::ButtonStyle::Text);
    button->setAccentRole(AdButton::AccentRole::Neutral);
    layout->addWidget(button);
    QObject::connect(button, &AdButton::clicked, &popover, [&, button] {
      first->setChecked(button == first);
      second->setChecked(button == second);
      for (auto* option : {first, second}) {
        option->setButtonStyle(option == button ? AdButton::ButtonStyle::Tonal
                                                : AdButton::ButtonStyle::Text);
        option->setAccentRole(option == button ? AdButton::AccentRole::Primary
                                               : AdButton::AccentRole::Neutral);
      }
      popover.hide();
    });
  }
  popover.setContentWidget(content);
  QCursor::setPos(0, 0);
  window.show();
  QApplication::processEvents();
  popover.show();
  QApplication::processEvents();
  require(first->isVisible(), "popover options must actually be visible");
  const QImage idle = first->grab().toImage();
  auto enter = [](AdButton* button) {
    const QPointF local = button->rect().center();
    QEnterEvent event(local, local, button->mapToGlobal(local.toPoint()));
    QApplication::sendEvent(button, &event);
  };
  // Deliver Enter without Leave: hiding a popup during activation does not
  // guarantee a matching Leave for its retained child widgets.
  enter(first);
  require(first->grab().toImage() != idle, "fixture must render a distinct hover state");
  first->click();
  require(!first->isVisible(), "selecting an option must hide the popup");
  popover.show();
  QApplication::processEvents();
  enter(second);
  second->click();
  popover.show();
  QApplication::processEvents();
  require(!first->isChecked() && second->isChecked(), "selection must move to option B");
  require(first->grab().toImage() == idle,
          "option A must not retain hover after selecting B and reopening");
  const QImage selected = second->grab().toImage();
  popover.hide();
  popover.show();
  QApplication::processEvents();
  require(second->isChecked() && second->grab().toImage() == selected,
          "dismissal must preserve the selected option's appearance");
  enter(first);
  require(first->grab().toImage() != idle, "hover must still work after reopening");
  // Also cover direct child hiding, independently of the popup controller.
  first->hide();
  first->show();
  require(first->grab().toImage() == idle, "direct hiding must also end hover");
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  try {
    adqt::widgets::detail::ButtonStyleInput successInput;
    successInput.accentRole = adqt::widgets::AdButton::AccentRole::Success;
    adqt::theme::ResolvedTheme successTheme;
    successTheme.values.colorSuccess = QColor("#123456");
    successTheme.values.colorSuccessHover = QColor("#234567");
    successTheme.values.colorSuccessActive = QColor("#345678");
    const auto successStyle =
        adqt::widgets::detail::resolveButtonVisualStyle(successInput, successTheme);
    require(successStyle.normal.text == QColor("#123456") &&
                successStyle.hover.text == QColor("#234567") &&
                successStyle.active.text == QColor("#345678"),
            "success buttons must use semantic success tokens");
    activationMayDestroyButton();
    cursorOverlayMayBeDestroyedBeforeButton();
    retainedPopoverButtonsForgetHover();
    std::cout << "Button lifetime tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
