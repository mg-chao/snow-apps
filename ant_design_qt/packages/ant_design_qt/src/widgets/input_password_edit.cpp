#include "input_password_edit.h"

#include "antd_icons.h"
#include "input_internal.h"

#include <QEvent>
#include <QToolButton>

namespace adqt::widgets {

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;
}

AdPasswordEdit::AdPasswordEdit(QWidget* parent) : AdLineEdit(parent) {
  visibleIconRef_ = outlined_icons::Eye();
  hiddenIconRef_ = outlined_icons::EyeInvisible();

  QLineEdit::setEchoMode(QLineEdit::Password);
  setTrailingActionLeading(true);
  if (QToolButton* actionButton = trailingActionButton()) {
    actionButton->setFocusPolicy(Qt::StrongFocus);
    connect(actionButton, &QToolButton::clicked, this, [this]() {
      if (!revealActionVisible_ || !isEnabled()) {
        return;
      }
      setTextVisible(!textVisible_);
    });
  }

  connect(this, &AdLineEdit::echoModeChanged, this, [this](QLineEdit::EchoMode mode) {
    const bool nextVisible = (mode == QLineEdit::Normal);
    if (textVisible_ == nextVisible) {
      return;
    }
    textVisible_ = nextVisible;
    updateToggleVisual();
    emit textVisibleChanged(textVisible_);
  });

  updateToggleVisual();
}

AdPasswordEdit::~AdPasswordEdit() = default;

bool AdPasswordEdit::revealActionVisible() const { return revealActionVisible_; }

void AdPasswordEdit::setRevealActionVisible(bool value) {
  if (revealActionVisible_ == value) {
    return;
  }
  revealActionVisible_ = value;
  updateToggleVisual();
  emit revealActionVisibleChanged(revealActionVisible_);
}

bool AdPasswordEdit::textVisible() const { return textVisible_; }

void AdPasswordEdit::setTextVisible(bool value) {
  if (textVisible_ == value) {
    return;
  }
  textVisible_ = value;
  AdLineEdit::setEchoMode(textVisible_ ? QLineEdit::Normal : QLineEdit::Password);
  updateToggleVisual();
  emit textVisibleChanged(textVisible_);
}

adqt::icons::IconRef AdPasswordEdit::visibleIconRef() const { return visibleIconRef_; }

void AdPasswordEdit::setVisibleIconRef(const adqt::icons::IconRef& value) {
  if (detail::input_internal::iconRefsEqual(visibleIconRef_, value)) {
    return;
  }
  visibleIconRef_ = value;
  updateToggleVisual();
}

adqt::icons::IconRef AdPasswordEdit::hiddenIconRef() const { return hiddenIconRef_; }

void AdPasswordEdit::setHiddenIconRef(const adqt::icons::IconRef& value) {
  if (detail::input_internal::iconRefsEqual(hiddenIconRef_, value)) {
    return;
  }
  hiddenIconRef_ = value;
  updateToggleVisual();
}

void AdPasswordEdit::changeEvent(QEvent* event) {
  AdLineEdit::changeEvent(event);
  if (!event) {
    return;
  }

  if (event->type() == QEvent::LanguageChange || event->type() == QEvent::EnabledChange ||
      event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::StyleChange) {
    updateToggleVisual();
  }
}

void AdPasswordEdit::updateToggleVisual() {
  setTrailingActionVisible(revealActionVisible_);
  if (QToolButton* actionButton = trailingActionButton()) {
    actionButton->setFocusPolicy(revealActionVisible_ ? Qt::StrongFocus : Qt::NoFocus);
  }
  if (!revealActionVisible_) {
    setTrailingActionAccessibleName(QString());
    return;
  }
  setTrailingActionIconRef(textVisible_ ? visibleIconRef_ : hiddenIconRef_);
  setTrailingActionAccessibleName(textVisible_ ? tr("Hide password") : tr("Show password"));
}

}  // namespace adqt::widgets
