#include "widgets/input_number.h"

#include "antd_icons.h"

#include <QApplication>
#include <QEnterEvent>
#include <QLabel>
#include <QLineEdit>

#include <cstdlib>
#include <iostream>

namespace {

using adqt::widgets::AdInputNumber;

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void setHovered(QWidget& widget, bool hovered) {
  widget.setAttribute(Qt::WA_UnderMouse, hovered);
  if (hovered) {
    const QPoint local = widget.rect().center();
    QEnterEvent event(local, widget.mapTo(widget.window(), local), widget.mapToGlobal(local));
    QApplication::sendEvent(&widget, &event);
  } else {
    QEvent event(QEvent::Leave);
    QApplication::sendEvent(&widget, &event);
  }
}

struct Fixture {
  QWidget window;
  AdInputNumber number{&window};
  QLineEdit* editor = nullptr;
  QWidget* actions = nullptr;

  Fixture() {
    window.resize(320, 100);
    number.setValue(50);
    number.setSuffixText(QStringLiteral("%"));
    number.setGeometry(10, 10, 160, number.sizeHint().height());
    window.show();
    QApplication::processEvents();
    number.blurInput();
    setHovered(number, false);
    editor = number.findChild<QLineEdit*>();
    actions = number.findChild<QWidget*>(QStringLiteral("ad-input-number-actions"));
    require(editor && actions, "input number must expose its editor and actions as children");
  }

  QList<QLabel*> suffixes() const {
    QList<QLabel*> result;
    for (QLabel* label : number.findChildren<QLabel*>()) {
      if (label->isVisible() && label->text() != QStringLiteral("$")) {
        result.append(label);
      }
    }
    return result;
  }

  void requireSeparated() const {
    require(actions->isVisible() && actions->width() > 0, "step actions must be visible");
    const auto labels = suffixes();
    require(!labels.isEmpty(), "suffix text or icon must be visible");
    for (const QLabel* label : labels) {
      require(label->geometry().right() < actions->geometry().left(),
              "suffix must remain to the left of the step buttons");
      require(editor->geometry().right() < label->geometry().left(),
              "editor must leave room for the suffix");
      require(number.rect().contains(label->geometry()), "suffix must remain inside the input");
    }
    if (labels.size() == 2) {
      require(!labels[0]->geometry().intersects(labels[1]->geometry()),
              "suffix text and icon must not overlap");
    }
  }
};

void hoverReflowsSuffix() {
  for (const auto variant :
       {AdInputNumber::Variant::Outlined, AdInputNumber::Variant::Filled,
        AdInputNumber::Variant::Borderless, AdInputNumber::Variant::Underlined}) {
    for (const auto size : {AdInputNumber::ControlSize::Small, AdInputNumber::ControlSize::Medium,
                            AdInputNumber::ControlSize::Large}) {
      for (int affix = 0; affix < 3; ++affix) {
        Fixture f;
        f.number.setVariant(variant);
        f.number.setControlSize(size);
        f.number.resize(160, f.number.sizeHint().height());
        f.number.setPrefixText(QStringLiteral("$"));
        if (affix > 0) {
          f.number.setSuffixIconRef(adqt::icons::antd::outlined::InfoCircle());
        }
        if (affix == 1) {
          f.number.setSuffixText(QString());
        }
        const QRect idleEditor = f.editor->geometry();
        const auto suffixes = f.suffixes();
        QList<QRect> idleSuffixes;
        for (const QLabel* suffix : suffixes) {
          idleSuffixes.append(suffix->geometry());
        }
        require(!f.actions->isVisible(), "idle input must hide its step buttons");

        setHovered(f.number, true);
        f.requireSeparated();
        require(f.editor->x() == idleEditor.x(), "hover must preserve the prefix and editor start");
        require(f.editor->width() == idleEditor.width() - f.actions->width(),
                "editor must give the suffix room to move by the visible action width");
        for (qsizetype i = 0; i < suffixes.size(); ++i) {
          require(suffixes[i]->geometry() == idleSuffixes[i].translated(-f.actions->width(), 0),
                  "each suffix must shift by the visible action width");
        }

        setHovered(f.number, false);
        require(!f.actions->isVisible(), "leaving the input must hide its step buttons");
        require(f.editor->geometry() == idleEditor, "leaving must restore the editor width");
        for (qsizetype i = 0; i < suffixes.size(); ++i) {
          require(suffixes[i]->geometry() == idleSuffixes[i],
                  "leaving must restore the suffix position");
        }
      }
    }
  }
}

void unavailableControlsReleaseSuffixSpace() {
  for (int mode = 0; mode < 3; ++mode) {
    Fixture f;
    const QRect idleSuffix = f.suffixes().first()->geometry();
    const QRect idleEditor = f.editor->geometry();
    setHovered(f.number, true);
    f.requireSeparated();
    if (mode == 0) f.number.setStepButtonsVisible(false);
    if (mode == 1) f.number.setReadOnly(true);
    if (mode == 2) f.number.setEnabled(false);
    require(!f.actions->isVisible(), "unavailable step buttons must be hidden");
    require(f.suffixes().first()->geometry() == idleSuffix,
            "hidden controls must not reserve suffix space");
    require(f.editor->geometry() == idleEditor, "hidden controls must restore editor space");
    if (mode == 0) f.number.setStepButtonsVisible(true);
    if (mode == 1) f.number.setReadOnly(false);
    if (mode == 2) f.number.setEnabled(true);
    setHovered(f.number, true);
    f.requireSeparated();
  }
}

void customWidthsAndDynamicSuffixes() {
  Fixture f;
  const QRect idleSuffix = f.suffixes().first()->geometry();
  AdInputNumber::AppearanceOverrides overrides;
  overrides.metrics.handleWidth = 34;
  f.number.setAppearanceOverrides(overrides);
  setHovered(f.number, true);
  f.requireSeparated();
  require(f.suffixes().first()->x() == idleSuffix.x() - 34,
          "suffix must respect a custom handle width");

  setHovered(f.number, false);
  overrides.metrics.handleVisibleWidth = 28;
  f.number.setAppearanceOverrides(overrides);
  f.requireSeparated();
  require(f.suffixes().first()->x() == idleSuffix.x() - 28,
          "always visible handles must reserve their actual visible width");
  f.number.focusEditor();
  f.requireSeparated();

  f.number.setSuffixText(QString());
  const QRect plainEditor = f.editor->geometry();
  f.number.setStepButtonsVisible(false);
  require(f.editor->geometry() == plainEditor,
          "inputs without a suffix must retain their existing editor layout");
  f.number.setStepButtonsVisible(true);
  f.number.setSuffixText(QStringLiteral("RMB"));
  f.requireSeparated();
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  hoverReflowsSuffix();
  unavailableControlsReleaseSuffixSpace();
  customWidthsAndDynamicSuffixes();
  std::cout << "Input number layout tests passed\n";
  return 0;
}
