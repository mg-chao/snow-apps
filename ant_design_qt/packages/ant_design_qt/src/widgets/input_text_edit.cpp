#include "input_text_edit.h"

#include "detail/themed_scrollbar.h"
#include "detail/timing_hub.h"
#include "antd_icons.h"
#include "input_internal.h"
#include "input_style.h"
#include "interaction_overlay_manager.h"
#include "theme/theme_manager.h"

#include <QAbstractTextDocumentLayout>
#include <QDropEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QMimeData>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBlockFormat>
#include <QTextFrame>
#include <QShowEvent>

#include <algorithm>

namespace adqt::widgets {

namespace {

namespace filled_icons = adqt::icons::antd::filled;

using detail::InputVisualStyle;
using detail::input_internal::InputFramePaintStyle;
using detail::input_internal::InputIconButton;

constexpr char kFeedbackSpinnerFrameKey[] = "AdTextEdit.FeedbackSpinnerFrame";

bool isLoadingIcon(const adqt::icons::IconRef& icon) {
  const auto metadata = adqt::icons::describeIcon(icon);
  return metadata.key.pack == QStringLiteral("antd") &&
         metadata.key.variant == QStringLiteral("outlined") &&
         metadata.key.name == QStringLiteral("loading");
}

int sharedSpinnerAngle() {
  const int cycleMs = detail::spinnerCycleDurationMs();
  if (cycleMs <= 0) {
    return 0;
  }
  qint64 phaseMs = detail::timingNowMs() % cycleMs;
  if (phaseMs < 0) {
    phaseMs += cycleMs;
  }
  return static_cast<int>((phaseMs * 360) / cycleMs);
}

QPixmap renderIconPixmap(const adqt::icons::IconRef& icon, int iconSide, qreal dpr,
                         int rotationDegrees = 0) {
  if (!adqt::icons::isValid(icon)) {
    return {};
  }

  if (rotationDegrees == 0) {
    return adqt::icons::renderIconPixmap(icon, {QSize(iconSide, iconSide), dpr});
  }

  QPixmap pixmap(QSize(qCeil(iconSide * dpr), qCeil(iconSide * dpr)));
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  const QRectF rect(0, 0, iconSide, iconSide);
  painter.translate(rect.center());
  painter.rotate(rotationDegrees);
  adqt::icons::paintIcon(&painter, icon,
                         QRectF(-iconSide / 2.0, -iconSide / 2.0, iconSide, iconSide));
  return pixmap;
}

void setLabelTextColor(QLabel* label, const QColor& color) {
  if (!label) {
    return;
  }
  QPalette palette = label->palette();
  palette.setColor(QPalette::Text, color);
  palette.setColor(QPalette::WindowText, color);
  palette.setColor(QPalette::Disabled, QPalette::Text, color);
  palette.setColor(QPalette::Disabled, QPalette::WindowText, color);
  label->setPalette(palette);
}

QColor resolvedBackgroundColor(const InputVisualStyle& style, bool focused, bool hovered) {
  if (focused) {
    return style.selectorActiveBg;
  }
  if (hovered) {
    return style.selectorHoverBg;
  }
  return style.selectorBg;
}

QColor resolvedBorderColor(const InputVisualStyle& style, bool focused, bool hovered) {
  if (focused) {
    return style.selectorActiveBorderColor;
  }
  if (hovered) {
    return style.selectorHoverBorderColor;
  }
  return style.selectorBorderColor;
}

int clearButtonGap(const InputVisualStyle& style) {
  return std::max(0, style.metrics.affixPadding);
}

int horizontalTextWidth(const QFontMetrics& fm, const QString& text) {
  if (text.isEmpty()) {
    return 0;
  }

  const QStringList lines = text.split(QChar('\n'));
  int maxWidth = 0;
  for (const QString& line : lines) {
    maxWidth = std::max(maxWidth, fm.horizontalAdvance(line));
  }
  return maxWidth;
}

int actualTextLineHeight(const InputVisualStyle& style) {
  return detail::input_internal::lineHeightForFont(style.metrics.font);
}

int visualTextLineHeight(const InputVisualStyle& style) {
  return std::max(actualTextLineHeight(style), style.metrics.textLineHeight);
}

int textAreaLineHeightDelta(const InputVisualStyle& style) {
  return std::max(0, visualTextLineHeight(style) - actualTextLineHeight(style));
}

qreal textAreaLeadingTopInset(const InputVisualStyle& style) {
  return static_cast<qreal>(textAreaLineHeightDelta(style)) / 2.0;
}

qreal textAreaLeadingBottomInset(const InputVisualStyle& style) {
  return static_cast<qreal>(textAreaLineHeightDelta(style)) - textAreaLeadingTopInset(style);
}

QRect textAreaFrameRect(const QRect& bounds, const InputVisualStyle& style, bool countVisible) {
  const int reservedBottom =
      countVisible ? style.metrics.countTopMargin + style.metrics.countHeight : 0;
  return QRect(bounds.left(), bounds.top(), bounds.width(),
               std::max(0, bounds.height() - reservedBottom));
}

class TextAreaFrameLayer final : public QWidget {
 public:
  explicit TextAreaFrameLayer(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAutoFillBackground(false);
  }

  void setPaintStyle(const InputFramePaintStyle& style) {
    paintStyle_ = style;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    QPainter painter(this);
    detail::input_internal::paintInputFrame(&painter, rect(), paintStyle_);
  }

 private:
  InputFramePaintStyle paintStyle_;
};

}  // namespace

AdTextEdit::AdTextEdit(QWidget* parent) : QTextEdit(parent) {
  setAttribute(Qt::WA_Hover, true);
  setMouseTracking(true);
  setSizePolicy(QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed));
  setFocusPolicy(Qt::StrongFocus);
  setFrameStyle(QFrame::NoFrame);
  setAutoFillBackground(false);
  setAcceptRichText(false);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  viewport()->setAutoFillBackground(false);
  if (document()) {
    document()->setDocumentMargin(0.0);
  }

  viewport()->installEventFilter(this);
  if (viewport()) {
    overlayVerticalScrollBar_ = new QScrollBar(Qt::Vertical, viewport());
    overlayVerticalScrollBar_->setObjectName(QStringLiteral("adtextarea-overlay-vbar"));
    overlayVerticalScrollBar_->setFocusPolicy(Qt::NoFocus);
    overlayVerticalScrollBar_->setAttribute(Qt::WA_Hover, true);
    overlayVerticalScrollBar_->installEventFilter(this);
    overlayVerticalScrollBar_->hide();
    overlayVerticalScrollBar_->raise();
  }
  if (verticalScrollBar()) {
    verticalScrollBar()->hide();
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int) { syncOverlayScrollBar(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { syncOverlayScrollBar(); });
  }

  frameLayer_ = new TextAreaFrameLayer(this);
  frameLayer_->lower();

  clearButton_ = new InputIconButton(this);
  clearButton_->setFocusPolicy(Qt::NoFocus);
  clearButton_->setAutoRaise(true);
  clearButton_->hide();
  clearButton_->installEventFilter(this);

  feedbackIconLabel_ = new QLabel(this);
  feedbackIconLabel_->setObjectName(QStringLiteral("adtextarea-feedback-icon"));
  feedbackIconLabel_->setAlignment(Qt::AlignCenter);
  feedbackIconLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  feedbackIconLabel_->hide();

  countLabel_ = new QLabel(this);
  countLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  countLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  countLabel_->hide();

  if (overlayVerticalScrollBar_) {
    connect(overlayVerticalScrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
      QScrollBar* source = verticalScrollBar();
      if (!source || source->value() == value) {
        return;
      }
      source->setValue(value);
    });
  }

  connect(this, &QTextEdit::textChanged, this, [this]() {
    if (internalTextUpdate_ || internalDocumentLayoutUpdate_) {
      return;
    }

    QString currentText = toPlainText();
    if (maxLength_ > 0 && currentText.size() > maxLength_) {
      currentText = currentText.left(maxLength_);
    }
    currentText = normalizedText(currentText);

    if (currentText != toPlainText()) {
      QTextCursor cursor = textCursor();
      const int position = std::min(cursor.position(), static_cast<int>(currentText.size()));
      internalTextUpdate_ = true;
      QTextEdit::setPlainText(currentText);
      cursor = textCursor();
      cursor.setPosition(position);
      setTextCursor(cursor);
      internalTextUpdate_ = false;
    }

    emit plainTextChanged(toPlainText());
    if (userEditInProgress_) {
      emit textEdited(toPlainText());
    }
    updateCountLabel();
    updateClearButton();
    updateHeightConstraints();
    refreshVisualState(false);
  });

  connect(clearButton_, &QToolButton::clicked, this, [this]() {
    if (toPlainText().isEmpty()) {
      return;
    }
    internalTextUpdate_ = true;
    QTextEdit::clear();
    internalTextUpdate_ = false;
    emit cleared();
    emit plainTextChanged(QString());
    focusEditor(FocusSelection::Start, false);
  });

  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { refreshVisualState(true); });

  refreshVisualState(true);
}

AdTextEdit::~AdTextEdit() { stopInteractionFocusForOwner(this); }

AdTextEdit::ControlSize AdTextEdit::controlSize() const { return controlSize_; }

void AdTextEdit::setControlSize(ControlSize value) {
  if (controlSize_ == value) {
    return;
  }
  controlSize_ = value;
  refreshVisualState(true);
  emit controlSizeChanged(controlSize_);
}

AdTextEdit::Variant AdTextEdit::variant() const { return variant_; }

void AdTextEdit::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  variant_ = value;
  refreshVisualState(true);
  emit variantChanged(variant_);
}

AdTextEdit::Status AdTextEdit::status() const { return status_; }

void AdTextEdit::setStatus(Status value) {
  if (status_ == value) {
    return;
  }
  status_ = value;
  refreshVisualState(false);
  emit statusChanged(status_);
}

bool AdTextEdit::allowClear() const { return allowClear_; }

void AdTextEdit::setAllowClear(bool value) {
  if (allowClear_ == value) {
    return;
  }
  allowClear_ = value;
  refreshVisualState(true);
  emit allowClearChanged(allowClear_);
}

void AdTextEdit::setPlaceholderText(const QString& value) {
  if (placeholderText() == value) {
    return;
  }
  QTextEdit::setPlaceholderText(value);
  refreshVisualState(true);
  emit placeholderTextChanged(value);
}

QString AdTextEdit::plainText() const { return toPlainText(); }

void AdTextEdit::setPlainText(const QString& value) {
  QString next = value;
  if (maxLength_ > 0 && next.size() > maxLength_) {
    next = next.left(maxLength_);
  }
  next = normalizedText(next);

  if (toPlainText() == next) {
    return;
  }

  internalTextUpdate_ = true;
  QTextEdit::setPlainText(next);
  internalTextUpdate_ = false;
  emit plainTextChanged(next);
  refreshVisualState(true);
}

void AdTextEdit::setReadOnly(bool value) {
  if (isReadOnly() == value) {
    return;
  }
  QTextEdit::setReadOnly(value);
  refreshVisualState(false);
  emit readOnlyChanged(value);
}

int AdTextEdit::maxLength() const { return maxLength_; }

void AdTextEdit::setMaxLength(int value) {
  const int normalized = value < 0 ? -1 : value;
  if (maxLength_ == normalized) {
    return;
  }
  maxLength_ = normalized;
  if (maxLength_ > 0 && toPlainText().size() > maxLength_) {
    setPlainText(toPlainText().left(maxLength_));
  }
  refreshVisualState(true);
  emit maxLengthChanged(maxLength_);
}

bool AdTextEdit::countVisible() const { return countVisible_; }

void AdTextEdit::setCountVisible(bool value) {
  if (countVisible_ == value) {
    return;
  }
  countVisible_ = value;
  refreshVisualState(true);
  emit countVisibleChanged(countVisible_);
}

int AdTextEdit::maximumCharacterCount() const { return maximumCharacterCount_; }

void AdTextEdit::setMaximumCharacterCount(int value) {
  const int normalized = value < 0 ? -1 : value;
  if (maximumCharacterCount_ == normalized) {
    return;
  }
  maximumCharacterCount_ = normalized;
  refreshVisualState(true);
  emit maximumCharacterCountChanged(maximumCharacterCount_);
}

AdTextEdit::HeightMode AdTextEdit::heightMode() const { return heightMode_; }

void AdTextEdit::setHeightMode(HeightMode value) {
  if (heightMode_ == value) {
    return;
  }
  heightMode_ = value;
  updateHeightConstraints();
  emit heightModeChanged(heightMode_);
}

int AdTextEdit::minimumVisibleRows() const { return minimumVisibleRows_; }

void AdTextEdit::setMinimumVisibleRows(int value) {
  const int normalized = std::max(1, value);
  if (minimumVisibleRows_ == normalized) {
    return;
  }
  minimumVisibleRows_ = normalized;
  if (maximumVisibleRows_ < minimumVisibleRows_) {
    maximumVisibleRows_ = minimumVisibleRows_;
    emit maximumVisibleRowsChanged(maximumVisibleRows_);
  }
  updateHeightConstraints();
  emit minimumVisibleRowsChanged(minimumVisibleRows_);
}

int AdTextEdit::maximumVisibleRows() const { return maximumVisibleRows_; }

void AdTextEdit::setMaximumVisibleRows(int value) {
  const int normalized = std::max(minimumVisibleRows_, value);
  if (maximumVisibleRows_ == normalized) {
    return;
  }
  maximumVisibleRows_ = normalized;
  updateHeightConstraints();
  emit maximumVisibleRowsChanged(maximumVisibleRows_);
}

adqt::icons::IconRef AdTextEdit::feedbackIconRef() const { return feedbackIconRef_; }

void AdTextEdit::setFeedbackIconRef(const adqt::icons::IconRef& value) {
  if (detail::input_internal::iconRefsEqual(feedbackIconRef_, value)) {
    return;
  }
  feedbackIconRef_ = value;
  updateFeedbackSpinnerState();
  refreshVisualState(true);
  emit feedbackIconRefChanged(feedbackIconRef_);
}

AdInputTextPolicy* AdTextEdit::textPolicy() const { return textPolicy_; }

void AdTextEdit::setTextPolicy(AdInputTextPolicy* value) {
  if (textPolicy_ == value) {
    return;
  }

  textPolicy_ = value;
  emit textPolicyChanged(textPolicy_);

  const QString current = toPlainText();
  if (normalizedText(current) != current) {
    setPlainText(current);
    return;
  }
  refreshVisualState(true);
}

QSize AdTextEdit::sizeHint() const {
  const InputVisualStyle style = resolvedStyle();
  QFontMetrics fm(style.metrics.font);
  const QMargins contentInsets = detail::input_internal::textControlContentMargins(style);
  const int lineHeight = visualTextLineHeight(style);
  const int rows =
      std::clamp(minimumVisibleRows_, 1, std::max(minimumVisibleRows_, maximumVisibleRows_));
  const int contentHeight =
      rows * lineHeight + detail::input_internal::textAreaRowPaddingExtra(style);
  const int countHeight =
      countVisible_ ? (style.metrics.countTopMargin + style.metrics.countHeight) : 0;
  const int textWidth =
      std::max(horizontalTextWidth(fm, placeholderText()), horizontalTextWidth(fm, toPlainText()));
  const int widthHint = std::max(240, contentInsets.left() + contentInsets.right() + textWidth +
                                          widthAccessoryHint(style) + style.metrics.height * 2);
  return QSize(widthHint, contentHeight + countHeight + style.metrics.borderWidth * 2);
}

QSize AdTextEdit::minimumSizeHint() const {
  const QSize hint = sizeHint();
  if (property("ad-flex-min-width-zero").toBool()) {
    return QSize(0, hint.height());
  }
  return QSize(std::min(120, hint.width()), hint.height());
}

void AdTextEdit::focusEditor(FocusSelection selection, bool preventScroll) {
  const QVector<detail::input_internal::PreservedScrollPosition> preserved =
      preventScroll ? detail::input_internal::captureAncestorScrollPositions(this)
                    : QVector<detail::input_internal::PreservedScrollPosition>();

  setFocus(Qt::OtherFocusReason);

  QTextCursor cursor = textCursor();
  if (selection == FocusSelection::Start) {
    cursor.movePosition(QTextCursor::Start);
  } else if (selection == FocusSelection::End) {
    cursor.movePosition(QTextCursor::End);
  } else if (selection == FocusSelection::SelectAll) {
    selectAll();
  }

  if (selection != FocusSelection::SelectAll) {
    setTextCursor(cursor);
  }

  if (!preserved.isEmpty()) {
    detail::input_internal::restoreAncestorScrollPositions(preserved);
    detail::input_internal::restoreAncestorScrollPositionsDeferred(this, preserved);
  }

  refreshVisualState(false);
}

void AdTextEdit::blurInput() { clearFocus(); }

bool AdTextEdit::eventFilter(QObject* watched, QEvent* event) {
  if (!event) {
    return QTextEdit::eventFilter(watched, event);
  }

  if (watched == clearButton_) {
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
        event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
      refreshVisualState(false);
    }
  } else if (watched == viewport()) {
    if (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
        event->type() == QEvent::LayoutRequest) {
      updateLayoutMetrics();
      syncOverlayScrollBar();
    }
  } else if (watched == overlayVerticalScrollBar_) {
    if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
      if (!verticalScrollBarHovered_) {
        verticalScrollBarHovered_ = true;
        applyScrollBarStyle();
      }
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave ||
               event->type() == QEvent::Hide) {
      if (verticalScrollBarHovered_) {
        verticalScrollBarHovered_ = false;
        applyScrollBarStyle();
      }
    }
  }

  return QTextEdit::eventFilter(watched, event);
}

bool AdTextEdit::event(QEvent* event) { return QTextEdit::event(event); }

void AdTextEdit::paintEvent(QPaintEvent* event) { QTextEdit::paintEvent(event); }

void AdTextEdit::keyPressEvent(QKeyEvent* event) {
  userEditInProgress_ = true;
  QTextEdit::keyPressEvent(event);
  userEditInProgress_ = false;
}

void AdTextEdit::inputMethodEvent(QInputMethodEvent* event) {
  userEditInProgress_ = true;
  QTextEdit::inputMethodEvent(event);
  userEditInProgress_ = false;
}

void AdTextEdit::insertFromMimeData(const QMimeData* source) {
  userEditInProgress_ = true;
  QTextEdit::insertFromMimeData(source);
  userEditInProgress_ = false;
}

void AdTextEdit::dropEvent(QDropEvent* event) {
  userEditInProgress_ = true;
  QTextEdit::dropEvent(event);
  userEditInProgress_ = false;
}

void AdTextEdit::resizeEvent(QResizeEvent* event) {
  QTextEdit::resizeEvent(event);
  updateLayoutMetrics();
  if (heightMode_ == HeightMode::AutoGrow && event->size().width() != event->oldSize().width()) {
    updateHeightConstraints();
  }
  syncOverlayScrollBar();
  updateInteractionFocusOverlay();
}

void AdTextEdit::changeEvent(QEvent* event) {
  QTextEdit::changeEvent(event);
  if (!event) {
    return;
  }

  if (event->type() == QEvent::LanguageChange || event->type() == QEvent::EnabledChange ||
      event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::StyleChange) {
    refreshVisualState(true);
  }
}

void AdTextEdit::moveEvent(QMoveEvent* event) {
  QTextEdit::moveEvent(event);
  updateInteractionFocusOverlay();
}

void AdTextEdit::showEvent(QShowEvent* event) {
  QTextEdit::showEvent(event);
  // Rebuild clear icon pixmaps after the widget lands on its final screen so
  // the DPR-sensitive affix rendering matches the single-line input widgets.
  updateFeedbackSpinnerState();
  refreshVisualState(false);
}

void AdTextEdit::hideEvent(QHideEvent* event) {
  QTextEdit::hideEvent(event);
  updateFeedbackSpinnerState();
  updateInteractionFocusOverlay();
}

void AdTextEdit::enterEvent(QEnterEvent* event) {
  hovered_ = true;
  refreshVisualState(false);
  QTextEdit::enterEvent(event);
}

void AdTextEdit::leaveEvent(QEvent* event) {
  hovered_ = false;
  refreshVisualState(false);
  QTextEdit::leaveEvent(event);
}

void AdTextEdit::focusInEvent(QFocusEvent* event) {
  focused_ = true;
  refreshVisualState(false);
  QTextEdit::focusInEvent(event);
}

void AdTextEdit::focusOutEvent(QFocusEvent* event) {
  focused_ = false;
  refreshVisualState(false);
  QTextEdit::focusOutEvent(event);
}

void AdTextEdit::updateCountLabel() {
  if (!countLabel_) {
    return;
  }

  const int count = effectiveCount(toPlainText());
  const int maximum = effectiveCountMax();
  countLabel_->setText(countLabelText(toPlainText(), count, maximum));
  countLabel_->setVisible(countVisible_);
}

void AdTextEdit::updateClearButton() {
  if (!clearButton_) {
    return;
  }

  const InputVisualStyle style = resolvedStyle();
  const int iconSide = std::max(10, style.metrics.clearIconSize);
  const QPixmap normal = detail::input_internal::renderTintedIcon(
      filled_icons::CloseCircle(), style.clearColor, iconSide, devicePixelRatioF());
  const QPixmap hover = detail::input_internal::renderTintedIcon(
      filled_icons::CloseCircle(), style.clearHoverColor, iconSide, devicePixelRatioF());
  const QPixmap pressed = detail::input_internal::renderTintedIcon(
      filled_icons::CloseCircle(), style.clearActiveColor, iconSide, devicePixelRatioF());

  QIcon icon;
  icon.addPixmap(normal, QIcon::Normal, QIcon::Off);
  icon.addPixmap(hover, QIcon::Active, QIcon::Off);
  icon.addPixmap(pressed.isNull() ? hover : pressed, QIcon::Selected, QIcon::Off);
  icon.addPixmap(normal, QIcon::Disabled, QIcon::Off);
  clearButton_->setIcon(icon);
  clearButton_->setIconSize(QSize(iconSide, iconSide));
  clearButton_->setFixedSize(iconSide, iconSide);

  const bool visible = allowClear_ && !isReadOnly() && isEnabled() && !toPlainText().isEmpty();
  clearButton_->setEnabled(visible);
  clearButton_->setCursor(visible ? Qt::PointingHandCursor : Qt::ArrowCursor);
  clearButton_->setVisible(visible);
}

void AdTextEdit::updateFeedbackIcon() {
  if (!feedbackIconLabel_) {
    return;
  }

  const InputVisualStyle style = resolvedStyle();
  const int iconSide = std::max(10, style.metrics.affixIconSize);
  feedbackIconLabel_->setFixedSize(iconSide, iconSide);
  if (!adqt::icons::isValid(feedbackIconRef_)) {
    feedbackIconLabel_->clear();
    feedbackIconLabel_->hide();
    return;
  }

  const QPixmap pixmap =
      renderIconPixmap(feedbackIconRef_, iconSide, devicePixelRatioF(),
                       isLoadingIcon(feedbackIconRef_) ? sharedSpinnerAngle() : 0);
  feedbackIconLabel_->setPixmap(pixmap);
  feedbackIconLabel_->setVisible(!pixmap.isNull());
}

void AdTextEdit::updateFeedbackSpinnerState() {
  const bool spinning =
      isVisible() && adqt::icons::isValid(feedbackIconRef_) && isLoadingIcon(feedbackIconRef_);
  if (spinning && !feedbackSpinnerSubscribed_) {
    detail::setFrameSubscription(this, QString::fromLatin1(kFeedbackSpinnerFrameKey), true,
                                 [this](qint64, qint64) {
                                   if (isVisible() && isLoadingIcon(feedbackIconRef_)) {
                                     updateFeedbackIcon();
                                   }
                                 });
    feedbackSpinnerSubscribed_ = true;
  } else if (!spinning && feedbackSpinnerSubscribed_) {
    detail::clearFrameSubscription(this, QString::fromLatin1(kFeedbackSpinnerFrameKey));
    feedbackSpinnerSubscribed_ = false;
  }
}

void AdTextEdit::applyDocumentLayoutMetrics(const InputVisualStyle& style) {
  QTextDocument* doc = document();
  if (!doc) {
    return;
  }

  const int lineHeightDelta = textAreaLineHeightDelta(style);
  const qreal topInset = textAreaLeadingTopInset(style);
  const qreal bottomInset = textAreaLeadingBottomInset(style);

  QSignalBlocker blocker(doc);
  internalDocumentLayoutUpdate_ = true;

  if (QTextFrame* rootFrame = doc->rootFrame()) {
    QTextFrameFormat frameFormat = rootFrame->frameFormat();
    bool frameChanged = false;
    auto updateMargin = [&frameChanged](qreal* target, qreal value) {
      if (!qFuzzyCompare(*target + 1.0, value + 1.0)) {
        *target = value;
        frameChanged = true;
      }
    };

    qreal topMargin = frameFormat.topMargin();
    qreal bottomMargin = frameFormat.bottomMargin();
    qreal leftMargin = frameFormat.leftMargin();
    qreal rightMargin = frameFormat.rightMargin();
    updateMargin(&topMargin, topInset);
    updateMargin(&bottomMargin, bottomInset);
    updateMargin(&leftMargin,
                 static_cast<qreal>(std::max(0, style.metrics.multilineInlineStartCompensation)));
    updateMargin(&rightMargin, 0.0);

    if (frameChanged) {
      frameFormat.setTopMargin(topMargin);
      frameFormat.setBottomMargin(bottomMargin);
      frameFormat.setLeftMargin(leftMargin);
      frameFormat.setRightMargin(rightMargin);
      rootFrame->setFrameFormat(frameFormat);
    }
  }

  QTextBlockFormat blockFormat;
  if (lineHeightDelta > 0) {
    blockFormat.setLineHeight(lineHeightDelta, QTextBlockFormat::LineDistanceHeight);
  } else {
    blockFormat.setLineHeight(100.0, QTextBlockFormat::ProportionalHeight);
  }

  QTextCursor documentCursor(doc);
  documentCursor.select(QTextCursor::Document);
  documentCursor.mergeBlockFormat(blockFormat);

  QTextCursor currentCursor = textCursor();
  currentCursor.mergeBlockFormat(blockFormat);
  setTextCursor(currentCursor);

  internalDocumentLayoutUpdate_ = false;
}

void AdTextEdit::applyScrollBarStyle() {
  const int baseThickness = detail::input_internal::kTextAreaScrollBarBaseThickness;
  const int hoverThickness = detail::input_internal::textAreaScrollBarHoverThickness();
  const int collapsedInset = std::max(
      0, baseThickness - detail::input_internal::kTextAreaScrollBarCollapsedVisualThickness);

  QScrollBar* styledVerticalBar =
      overlayVerticalScrollBar_ ? overlayVerticalScrollBar_ : verticalScrollBar();
  if (styledVerticalBar) {
    const bool hovered = verticalScrollBarHovered_ || styledVerticalBar->underMouse();
    const int extent = hovered ? hoverThickness : baseThickness;
    const int inset = hovered ? 0 : collapsedInset;
    const int visualWidth = std::max(1, extent - inset);
    const int radius = std::max(1, (visualWidth + 1) / 2);
    detail::applyThemedScrollBar(styledVerticalBar, extent, radius, inset);
  }

  if (QScrollBar* hBar = horizontalScrollBar()) {
    detail::applyThemedScrollBar(hBar, baseThickness, std::max(1, baseThickness / 2), 0);
  }

  updateOverlayScrollBarGeometry();
}

void AdTextEdit::syncOverlayScrollBar() {
  if (!overlayVerticalScrollBar_) {
    return;
  }

  QScrollBar* source = verticalScrollBar();
  if (!source) {
    const bool wasHovered = verticalScrollBarHovered_;
    verticalScrollBarHovered_ = false;
    if (wasHovered) {
      applyScrollBarStyle();
    }
    overlayVerticalScrollBar_->hide();
    return;
  }

  {
    QSignalBlocker blocker(overlayVerticalScrollBar_);
    overlayVerticalScrollBar_->setRange(source->minimum(), source->maximum());
    overlayVerticalScrollBar_->setPageStep(source->pageStep());
    overlayVerticalScrollBar_->setSingleStep(source->singleStep());
    overlayVerticalScrollBar_->setValue(source->value());
  }

  const bool visible = source->maximum() > source->minimum();
  if (!visible && verticalScrollBarHovered_) {
    verticalScrollBarHovered_ = false;
    applyScrollBarStyle();
  }
  overlayVerticalScrollBar_->setVisible(visible);
  updateOverlayScrollBarGeometry();
  if (visible) {
    overlayVerticalScrollBar_->raise();
  }
}

void AdTextEdit::updateOverlayScrollBarGeometry() {
  if (!overlayVerticalScrollBar_ || !viewport()) {
    return;
  }

  const int margin = 2;
  const int overlayWidth = verticalScrollBarHovered_
                               ? detail::input_internal::textAreaScrollBarHoverThickness()
                               : detail::input_internal::kTextAreaScrollBarBaseThickness;
  const int height = std::max(0, viewport()->height() - margin * 2);
  const int x = std::max(0, viewport()->width() - overlayWidth - margin);
  overlayVerticalScrollBar_->setGeometry(x, margin, overlayWidth, height);
  overlayVerticalScrollBar_->raise();
}

void AdTextEdit::updateLayoutMetrics() {
  const InputVisualStyle style = resolvedStyle();
  const QRect frameRect = textAreaFrameRect(rect(), style, countVisible_);
  const QMargins contentInsets = detail::input_internal::textControlContentMargins(style);
  const int clearGap = clearButtonGap(style);
  const int clearWidth =
      (clearButton_ && clearButton_->isVisible()) ? clearButton_->width() + clearGap : 0;
  const int feedbackWidth = (feedbackIconLabel_ && feedbackIconLabel_->isVisible())
                                ? feedbackIconLabel_->width() + clearGap
                                : 0;
  const int countHeight =
      countVisible_ ? style.metrics.countTopMargin + style.metrics.countHeight : 0;

  setViewportMargins(contentInsets.left(), contentInsets.top(),
                     contentInsets.right() + std::max(clearWidth, feedbackWidth),
                     contentInsets.bottom() + countHeight);

  if (frameLayer_) {
    frameLayer_->setGeometry(frameRect);
    frameLayer_->lower();
  }

  viewport()->raise();

  if (clearButton_ && clearButton_->isVisible()) {
    const int x = width() - std::max(0, contentInsets.right()) - clearButton_->width();
    const int y = std::max(0, style.metrics.borderWidth + style.metrics.multilineAffixTopInset);
    clearButton_->move(std::max(0, x), y);
    clearButton_->raise();
  }

  if (feedbackIconLabel_ && feedbackIconLabel_->isVisible()) {
    const int x = width() - std::max(0, contentInsets.right()) - feedbackIconLabel_->width();
    const int y =
        frameRect.top() + std::max(0, (frameRect.height() - feedbackIconLabel_->height()) / 2);
    feedbackIconLabel_->move(std::max(0, x), std::max(0, y));
    feedbackIconLabel_->raise();
  }

  if (countLabel_ && countVisible_) {
    countLabel_->adjustSize();
    const int countAreaTop = frameRect.bottom() + 1 + style.metrics.countTopMargin;
    const int x = width() - std::max(0, contentInsets.right()) - countLabel_->width();
    const int y =
        countAreaTop + std::max(0, (style.metrics.countHeight - countLabel_->height()) / 2);
    countLabel_->move(std::max(0, x), std::max(0, y));
    countLabel_->raise();
  }

  updateOverlayScrollBarGeometry();
}

void AdTextEdit::updateHeightConstraints() {
  const InputVisualStyle style = resolvedStyle();
  const int lineHeight = visualTextLineHeight(style);
  const int documentLineHeight = lineHeight;
  const int minRows = std::max(1, minimumVisibleRows_);
  const int maxRows = std::max(minRows, maximumVisibleRows_);
  const int rowPadding = detail::input_internal::textAreaRowPaddingExtra(style);
  const int countHeight =
      countVisible_ ? (style.metrics.countTopMargin + style.metrics.countHeight) : 0;

  if (heightMode_ == HeightMode::FixedGeometry) {
    setMinimumHeight(0);
    setMaximumHeight(QWIDGETSIZE_MAX);
    updateLayoutMetrics();
    applyScrollBarStyle();
    syncOverlayScrollBar();
    updateGeometry();
    return;
  }

  if (heightMode_ == HeightMode::FixedRows) {
    const int editorHeight = minRows * lineHeight + rowPadding;
    const int totalHeight = editorHeight + countHeight + style.metrics.borderWidth * 2;
    setMinimumHeight(totalHeight);
    setMaximumHeight(totalHeight);
    updateLayoutMetrics();
    applyScrollBarStyle();
    syncOverlayScrollBar();
    updateGeometry();
    return;
  }

  const qreal documentHeight = document()->documentLayout()
                                   ? document()->documentLayout()->documentSize().height()
                                   : document()->size().height();
  const int contentHeight = std::max(documentLineHeight, qCeil(documentHeight));
  const int desiredRows = std::clamp(
      (contentHeight + documentLineHeight - 1) / std::max(1, documentLineHeight), minRows, maxRows);
  const int editorHeight = desiredRows * lineHeight + rowPadding;
  const int totalHeight = editorHeight + countHeight + style.metrics.borderWidth * 2;

  setMinimumHeight(totalHeight);
  setMaximumHeight(totalHeight);
  updateLayoutMetrics();
  applyScrollBarStyle();
  syncOverlayScrollBar();
  updateGeometry();
}

void AdTextEdit::applyEditorPalette() {
  const InputVisualStyle style = resolvedStyle();
  const QColor transparent(0, 0, 0, 0);
  const QColor textColor = isEnabled() ? style.selectorTextColor : style.disabledTextColor;
  const QColor placeholderColor = isEnabled() ? style.placeholderColor : style.disabledTextColor;

  QPalette palette = this->palette();
  palette.setColor(QPalette::Base, transparent);
  palette.setColor(QPalette::Window, transparent);
  palette.setColor(QPalette::Text, textColor);
  palette.setColor(QPalette::Disabled, QPalette::Base, transparent);
  palette.setColor(QPalette::Disabled, QPalette::Window, transparent);
  palette.setColor(QPalette::Disabled, QPalette::Text, textColor);
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
  palette.setColor(QPalette::PlaceholderText, placeholderColor);
  palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, placeholderColor);
#endif
  setPalette(palette);
  viewport()->setPalette(palette);
}

void AdTextEdit::refreshVisualState(bool geometryChanged) {
  const InputVisualStyle style = resolvedStyle();
  if (font() != style.metrics.font) {
    QTextEdit::setFont(style.metrics.font);
  }
  if (document()) {
    document()->setDocumentMargin(0.0);
  }
  applyDocumentLayoutMetrics(style);

  updateCountLabel();
  updateClearButton();
  updateFeedbackIcon();
  applyEditorPalette();
  if (frameLayer_) {
    InputFramePaintStyle frameStyle;
    frameStyle.background = resolvedBackgroundColor(style, focused_, hovered_);
    frameStyle.border = resolvedBorderColor(style, focused_, hovered_);
    frameStyle.borderWidth = std::max<qreal>(0.0, style.metrics.borderWidth);
    frameStyle.underlined = style.underlined;
    const qreal radius = style.underlined ? 0.0 : std::max<qreal>(0.0, style.metrics.borderRadius);
    frameStyle.topLeftRadius = radius;
    frameStyle.topRightRadius = radius;
    frameStyle.bottomRightRadius = radius;
    frameStyle.bottomLeftRadius = radius;
    static_cast<TextAreaFrameLayer*>(frameLayer_)->setPaintStyle(frameStyle);
  }
  if (countLabel_) {
    countLabel_->setFont(style.metrics.font);
    setLabelTextColor(countLabel_, isEnabled() ? style.countColor : style.disabledTextColor);
  }
  updateHeightConstraints();
  syncAccessibleState();
  updateInteractionFocusOverlay();
  viewport()->update();
  update();
  if (geometryChanged) {
    updateGeometry();
  }
}

void AdTextEdit::updateInteractionFocusOverlay() {
  if (!focused_) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const InputVisualStyle style = resolvedStyle();
  detail::input_internal::updateInputFocusOverlay(
      this, textAreaFrameRect(rect(), style, countVisible_), style, false, false);
}

AdTextEdit::Status AdTextEdit::effectiveStatus() const {
  if (status_ != Status::None) {
    return status_;
  }
  const int maximum = effectiveCountMax();
  if (maximum > 0 && effectiveCount(toPlainText()) > maximum) {
    return Status::Warning;
  }
  return Status::None;
}

InputVisualStyle AdTextEdit::resolvedStyle() const {
  detail::InputStyleInput input;
  input.controlSize = controlSize_;
  input.variant = variant_;
  input.status = effectiveStatus();
  input.disabled = !isEnabled();
  input.focused = focused_;
  input.hovered = hovered_;
  input.multiline = true;
  input.baseFont = font();
  return adqt::widgets::detail::resolveTextControlVisualStyle(
      input, adqt::theme::ThemeManager::instance().resolve(this));
}

int AdTextEdit::effectiveCount(const QString& text) const {
  if (textPolicy_) {
    return std::max(0, textPolicy_->characterCount(text));
  }
  return static_cast<int>(text.size());
}

int AdTextEdit::effectiveCountMax() const {
  if (maximumCharacterCount_ > 0) {
    return maximumCharacterCount_;
  }
  if (maxLength_ > 0) {
    return maxLength_;
  }
  return -1;
}

QString AdTextEdit::normalizedText(const QString& text) const {
  if (!textPolicy_) {
    return text;
  }
  return textPolicy_->normalizeText(text, effectiveCountMax());
}

QString AdTextEdit::countLabelText(const QString& text, int count, int maximum) const {
  if (textPolicy_) {
    return textPolicy_->formatCountLabel(text, count, maximum);
  }
  if (maximum > 0) {
    return QStringLiteral("%1 / %2").arg(count).arg(maximum);
  }
  return QString::number(count);
}

int AdTextEdit::widthAccessoryHint(const InputVisualStyle& style) const {
  const QFontMetrics fm(style.metrics.font);
  int widthHint = 0;
  if (allowClear_ && !isReadOnly()) {
    widthHint += std::max(10, style.metrics.clearIconSize) + clearButtonGap(style);
  }
  if (adqt::icons::isValid(feedbackIconRef_)) {
    widthHint =
        std::max(widthHint, std::max(10, style.metrics.affixIconSize) + clearButtonGap(style));
  }
  if (countVisible_) {
    widthHint += horizontalTextWidth(
        fm, countLabelText(toPlainText(), effectiveCount(toPlainText()), effectiveCountMax()));
  }
  return widthHint;
}

void AdTextEdit::syncAccessibleState() {
  setAccessibleName(tr("Multiline input"));
  setAccessibleDescription(tr("Enter multi-line text"));
  if (clearButton_) {
    clearButton_->setAccessibleName(tr("Clear input"));
    clearButton_->setAccessibleDescription(tr("Clear the current text"));
  }
}

}  // namespace adqt::widgets
