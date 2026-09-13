#include "input_search_edit.h"

#include "button.h"
#include "detail/button_grouping.h"
#include "antd_icons.h"
#include "input_internal.h"
#include "theme/theme_manager.h"

#include <algorithm>
#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QResizeEvent>
#include <QSpacerItem>
#include <QTimer>

namespace adqt::widgets {

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;

class SearchSeparatorOverlay final : public QWidget {
 public:
  explicit SearchSeparatorOverlay(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    hide();
  }

  void setSeparator(const QColor& color, int width, int padding) {
    const int normalizedWidth = std::max(1, width);
    const int normalizedPadding = std::max(0, padding);
    if (color_ == color && width_ == normalizedWidth && padding_ == normalizedPadding) {
      return;
    }
    color_ = color;
    width_ = normalizedWidth;
    padding_ = normalizedPadding;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    if (!color_.isValid() || color_.alpha() <= 0 || width_ <= 0) {
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const qreal dpr =
        painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
    const qreal penWidth = std::max<qreal>(1.0, width_);
    QPen pen(color_, penWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const qreal x = detail::input_internal::snapToDevicePixelCoord(
        static_cast<qreal>(padding_) + penWidth / 2.0, dpr);
    painter.drawLine(QPointF(x, 0.0), QPointF(x, static_cast<qreal>(height())));
  }

 private:
  QColor color_;
  int width_ = 1;
  int padding_ = 0;
};

QColor resolveSearchSeparatorColor(const adqt::theme::ThemeMapToken& map, AdLineEdit::Status status,
                                   bool enabled, bool inputActiveBorder) {
  if (!enabled) {
    return map.colorBorderDisabled.isValid() ? map.colorBorderDisabled : QColor("#d9d9d9");
  }

  switch (status) {
    case AdLineEdit::Status::Error:
      if (map.colorError.isValid()) {
        return map.colorError;
      }
      if (map.colorErrorBorder.isValid()) {
        return map.colorErrorBorder;
      }
      break;
    case AdLineEdit::Status::Warning:
      if (map.colorWarning.isValid()) {
        return map.colorWarning;
      }
      if (map.colorWarningBorder.isValid()) {
        return map.colorWarningBorder;
      }
      break;
    case AdLineEdit::Status::None:
    default:
      break;
  }

  if (inputActiveBorder) {
    if (map.colorPrimary.isValid()) {
      return map.colorPrimary;
    }
    if (map.colorPrimaryHover.isValid()) {
      return map.colorPrimaryHover;
    }
  }

  if (map.colorBorder.isValid()) {
    return map.colorBorder;
  }
  if (map.colorBorderSecondary.isValid()) {
    return map.colorBorderSecondary;
  }
  return QColor("#d9d9d9");
}

}  // namespace

AdSearchEdit::AdSearchEdit(QWidget* parent) : QWidget(parent) {
  setFocusPolicy(Qt::StrongFocus);
  setSizePolicy(QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed));

  rootLayout_ = new QHBoxLayout(this);
  rootLayout_->setContentsMargins(0, 0, 0, 0);
  rootLayout_->setSpacing(0);

  input_ = new AdLineEdit(this);
  button_ = new AdButton(this);
  separatorOverlay_ = new SearchSeparatorOverlay(this);

  joinOverlapSpacer_ = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
  rootLayout_->addWidget(input_, 1);
  rootLayout_->addItem(joinOverlapSpacer_);
  rootLayout_->addWidget(button_, 0);

  setFocusProxy(input_);

  input_->setAllowClear(true);
  input_->setJoinedRight(true);
  input_->installEventFilter(this);

  button_->setAutoDefault(false);
  button_->setDefault(false);
  detail::setButtonSegmentPosition(button_, detail::SegmentPosition::Trailing);
  button_->installEventFilter(this);

  connect(input_, &AdLineEdit::controlSizeChanged, this, [this](ControlSize value) {
    updateButtonVisual();
    emit controlSizeChanged(value);
  });
  connect(input_, &AdLineEdit::variantChanged, this, [this](Variant value) {
    updateButtonVisual();
    emit variantChanged(value);
  });
  connect(input_, &AdLineEdit::statusChanged, this, [this](Status value) {
    updateButtonVisual();
    emit statusChanged(value);
  });
  connect(input_, &AdLineEdit::allowClearChanged, this, &AdSearchEdit::allowClearChanged);
  connect(input_, &AdLineEdit::placeholderTextChanged, this, &AdSearchEdit::placeholderTextChanged);
  connect(input_, &QLineEdit::textChanged, this, &AdSearchEdit::textChanged);
  connect(input_, &QLineEdit::textEdited, this, &AdSearchEdit::textEdited);
  connect(input_, &AdLineEdit::cleared, this, [this]() {
    emit cleared();
    emit searchRequested(text(), SearchReason::ClearAction);
  });
  connect(input_, &AdLineEdit::prefixIconRefChanged, this, &AdSearchEdit::prefixIconRefChanged);
  connect(input_, &AdLineEdit::suffixIconRefChanged, this, &AdSearchEdit::suffixIconRefChanged);
  connect(input_, &AdLineEdit::feedbackIconRefChanged, this, &AdSearchEdit::feedbackIconRefChanged);
  connect(input_, &QLineEdit::editingFinished, this, &AdSearchEdit::editingFinished);
  connect(input_, &QLineEdit::returnPressed, this,
          [this]() { emit searchRequested(text(), SearchReason::ReturnKey); });
  connect(button_, &QAbstractButton::clicked, this, [this]() {
    if (!isEnabled()) {
      return;
    }
    emit searchRequested(text(), SearchReason::ButtonClick);
  });

  updateButtonVisual();
  syncAccessibleState();
}

AdSearchEdit::~AdSearchEdit() = default;

AdSearchEdit::ControlSize AdSearchEdit::controlSize() const {
  return input_ ? input_->controlSize() : ControlSize::Medium;
}

void AdSearchEdit::setControlSize(ControlSize value) {
  if (input_) {
    input_->setControlSize(value);
  }
}

AdSearchEdit::Variant AdSearchEdit::variant() const {
  return input_ ? input_->variant() : Variant::Outlined;
}

void AdSearchEdit::setVariant(Variant value) {
  if (input_) {
    input_->setVariant(value);
  }
}

AdSearchEdit::Status AdSearchEdit::status() const {
  return input_ ? input_->status() : Status::None;
}

void AdSearchEdit::setStatus(Status value) {
  if (input_) {
    input_->setStatus(value);
  }
}

bool AdSearchEdit::allowClear() const { return input_ ? input_->allowClear() : false; }

void AdSearchEdit::setAllowClear(bool value) {
  if (input_) {
    input_->setAllowClear(value);
  }
}

QString AdSearchEdit::placeholderText() const {
  return input_ ? input_->placeholderText() : QString();
}

void AdSearchEdit::setPlaceholderText(const QString& value) {
  if (input_) {
    input_->setPlaceholderText(value);
  }
}

QString AdSearchEdit::text() const { return input_ ? input_->text() : QString(); }

void AdSearchEdit::setText(const QString& value) {
  if (input_) {
    input_->setText(value);
  }
}

void AdSearchEdit::clear() {
  if (input_) {
    input_->clear();
  }
}

bool AdSearchEdit::busy() const { return busy_; }

void AdSearchEdit::setBusy(bool value) {
  if (busy_ == value) {
    return;
  }
  busy_ = value;
  updateButtonVisual();
  emit busyChanged(busy_);
}

QString AdSearchEdit::searchButtonText() const { return searchButtonText_; }

void AdSearchEdit::setSearchButtonText(const QString& value) {
  const QString trimmed = value.trimmed();
  if (searchButtonText_ == trimmed) {
    return;
  }
  searchButtonText_ = trimmed;
  searchButtonVisible_ = !searchButtonText_.isEmpty();
  updateButtonVisual();
  emit searchButtonTextChanged(searchButtonText_);
}

adqt::icons::IconRef AdSearchEdit::prefixIconRef() const {
  return input_ ? input_->prefixIconRef() : adqt::icons::IconRef();
}

void AdSearchEdit::setPrefixIconRef(const adqt::icons::IconRef& value) {
  if (input_) {
    input_->setPrefixIconRef(value);
  }
}

adqt::icons::IconRef AdSearchEdit::suffixIconRef() const {
  return input_ ? input_->suffixIconRef() : adqt::icons::IconRef();
}

void AdSearchEdit::setSuffixIconRef(const adqt::icons::IconRef& value) {
  if (input_) {
    input_->setSuffixIconRef(value);
  }
}

adqt::icons::IconRef AdSearchEdit::feedbackIconRef() const {
  return input_ ? input_->feedbackIconRef() : adqt::icons::IconRef();
}

void AdSearchEdit::setFeedbackIconRef(const adqt::icons::IconRef& value) {
  if (input_) {
    input_->setFeedbackIconRef(value);
  }
}

bool AdSearchEdit::joinedLeft() const { return input_ ? input_->joinedLeft() : false; }

void AdSearchEdit::setJoinedLeft(bool value) {
  if (input_) {
    input_->setJoinedLeft(value);
  }
}

bool AdSearchEdit::joinedRight() const {
  if (!button_) {
    return false;
  }
  const detail::SegmentPosition position = detail::buttonSegmentPosition(button_);
  return position == detail::SegmentPosition::Leading ||
         position == detail::SegmentPosition::Middle;
}

void AdSearchEdit::setJoinedRight(bool value) {
  if (!button_) {
    return;
  }
  const detail::SegmentPosition nextPosition =
      value ? detail::SegmentPosition::Middle : detail::SegmentPosition::Trailing;
  if (detail::buttonSegmentPosition(button_) == nextPosition) {
    return;
  }
  detail::setButtonSegmentPosition(button_, nextPosition);
  updateButtonVisual();
}

void AdSearchEdit::focusEditor(FocusSelection selection, bool preventScroll) {
  if (input_) {
    input_->focusEditor(selection, preventScroll);
  }
}

AdLineEdit* AdSearchEdit::lineEdit() const { return input_; }

void AdSearchEdit::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }

  if (event->type() == QEvent::EnabledChange) {
    if (input_) {
      input_->setEnabled(isEnabled());
    }
    if (button_) {
      button_->setEnabled(isEnabled());
    }
  }

  if (event->type() == QEvent::LanguageChange) {
    syncAccessibleState();
  }

  if (event->type() == QEvent::LanguageChange || event->type() == QEvent::EnabledChange ||
      event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::StyleChange) {
    updateButtonVisual();
  }
}

bool AdSearchEdit::eventFilter(QObject* watched, QEvent* event) {
  if ((watched == input_ || watched == button_) && event) {
    switch (event->type()) {
      case QEvent::Enter:
      case QEvent::Leave:
      case QEvent::FocusIn:
      case QEvent::FocusOut:
      case QEvent::Move:
      case QEvent::Resize:
      case QEvent::Show:
      case QEvent::Hide:
      case QEvent::ZOrderChange:
        scheduleSeparatorRefresh();
        break;
      default:
        break;
    }
  }

  return QWidget::eventFilter(watched, event);
}

void AdSearchEdit::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updateSeparatorVisual();
}

void AdSearchEdit::updateButtonVisual() {
  if (!input_ || !button_) {
    return;
  }

  const adqt::theme::ThemeMapToken map = adqt::theme::ThemeManager::instance().resolveTheme(this);
  const int lineWidth = std::max(1, qRound(map.lineWidth));

  button_->setBusy(busy_);
  button_->setEnabled(isEnabled());
  detail::setButtonSegmentPosition(
      button_, joinedRight() ? detail::SegmentPosition::Middle : detail::SegmentPosition::Trailing);
  button_->setSizeClass(static_cast<AdButton::SizeClass>(static_cast<int>(controlSize())));

  const Variant currentVariant = variant();
  const bool plainVariant =
      currentVariant == Variant::Borderless || currentVariant == Variant::Underlined;

  if (searchButtonVisible_) {
    button_->setText(searchButtonText_);
    button_->setIconRef(adqt::icons::IconRef());
    button_->setAccentRole(AdButton::AccentRole::Primary);
    button_->setButtonStyle((currentVariant == Variant::Filled || plainVariant)
                                ? AdButton::ButtonStyle::Text
                                : AdButton::ButtonStyle::Solid);
  } else {
    button_->setText(QString());
    button_->setIconRef(outlined_icons::Search());
    button_->setAccentRole(AdButton::AccentRole::Neutral);
    if (currentVariant == Variant::Filled) {
      button_->setButtonStyle(AdButton::ButtonStyle::Tonal);
    } else if (plainVariant) {
      button_->setButtonStyle(AdButton::ButtonStyle::Text);
    } else {
      button_->setButtonStyle(AdButton::ButtonStyle::Outline);
    }
  }

  if (joinOverlapSpacer_) {
    joinOverlapSpacer_->changeSize(-lineWidth, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    rootLayout_->invalidate();
  }

  input_->setJoinedRight(true);
  const bool showFilledSearchSeparator = !searchButtonVisible_ && currentVariant == Variant::Filled;

  const int controlHeight = std::max(input_->sizeHint().height(), button_->minimumHeight());
  input_->setMinimumHeight(controlHeight);
  button_->setMinimumHeight(controlHeight);
  setMinimumHeight(controlHeight);
  setMaximumHeight(controlHeight);
  if (!showFilledSearchSeparator && separatorOverlay_) {
    separatorOverlay_->hide();
  }
  updateSeparatorVisual();
  syncAccessibleState();
  updateGeometry();
}

void AdSearchEdit::updateSeparatorVisual() {
  if (!button_ || !input_ || !separatorOverlay_) {
    return;
  }

  const Variant currentVariant = variant();
  const bool showFilledSearchSeparator = !searchButtonVisible_ && currentVariant == Variant::Filled;
  const QWidget* focusedWidget = QApplication::focusWidget();
  const bool inputActiveBorder =
      focusedWidget && (focusedWidget == input_ || input_->isAncestorOf(focusedWidget));
  if (!showFilledSearchSeparator || !button_->isVisible()) {
    separatorOverlay_->hide();
    return;
  }

  const adqt::theme::ThemeMapToken map = adqt::theme::ThemeManager::instance().resolveTheme(this);
  const int lineWidth = std::max(1, qRound(map.lineWidth));
  const int overlayPadding = 1;
  const int overlayWidth = lineWidth + overlayPadding * 2;
  const QRect buttonRect = button_->geometry();
  if (!buttonRect.isValid()) {
    separatorOverlay_->hide();
    return;
  }

  const int separatorX =
      std::clamp(buttonRect.left() - overlayPadding, 0, std::max(0, width() - overlayWidth));
  separatorOverlay_->setGeometry(separatorX, buttonRect.top(), overlayWidth, buttonRect.height());
  static_cast<SearchSeparatorOverlay*>(separatorOverlay_)
      ->setSeparator(resolveSearchSeparatorColor(map, status(), isEnabled(), inputActiveBorder),
                     lineWidth, overlayPadding);
  separatorOverlay_->show();
  if (inputActiveBorder) {
    input_->raise();
    separatorOverlay_->raise();
    separatorOverlay_->stackUnder(input_);
  } else {
    separatorOverlay_->raise();
  }
}

void AdSearchEdit::scheduleSeparatorRefresh() {
  if (separatorRefreshQueued_) {
    return;
  }

  separatorRefreshQueued_ = true;
  QTimer::singleShot(0, this, [this]() {
    separatorRefreshQueued_ = false;
    updateSeparatorVisual();
  });
}

void AdSearchEdit::syncAccessibleState() {
  setAccessibleName(tr("Search input"));
  setAccessibleDescription(tr("Enter search text and trigger search"));

  if (input_) {
    input_->setAccessibleName(tr("Search text"));
    input_->setAccessibleDescription(tr("Enter the text to search"));
  }

  if (button_) {
    button_->setAccessibleName(searchButtonVisible_ ? searchButtonText_ : tr("Search"));
    button_->setAccessibleDescription(busy_ ? tr("Search is in progress") : tr("Run search"));
  }
}

}  // namespace adqt::widgets
