#include "input_otp_edit.h"

#include "input_internal.h"
#include "input_style.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>

#include <algorithm>

namespace adqt::widgets {

namespace {

using detail::InputVisualStyle;
using detail::input_internal::InputFramePaintStyle;

QColor resolvedBackgroundColor(const InputVisualStyle& style, bool enabled, bool focused,
                               bool hovered) {
  if (!enabled) {
    return style.disabledBg;
  }
  if (focused) {
    return style.selectorActiveBg;
  }
  if (hovered) {
    return style.selectorHoverBg;
  }
  return style.selectorBg;
}

QColor resolvedBorderColor(const InputVisualStyle& style, bool enabled, bool focused,
                           bool hovered) {
  if (!enabled) {
    return style.disabledBorderColor;
  }
  if (focused) {
    return style.selectorActiveBorderColor;
  }
  if (hovered) {
    return style.selectorHoverBorderColor;
  }
  return style.selectorBorderColor;
}

class OtpCellLineEdit final : public QLineEdit {
 public:
  explicit OtpCellLineEdit(QWidget* parent = nullptr) : QLineEdit(parent) {
    setFrame(false);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
  }

  void setMasked(bool value) {
    if (masked_ == value) {
      return;
    }
    masked_ = value;
    QLineEdit::setEchoMode(masked_ ? QLineEdit::NoEcho : QLineEdit::Normal);
    update();
  }

  void setMaskCharacter(const QString& value) {
    if (maskCharacter_ == value) {
      return;
    }
    maskCharacter_ = value;
    if (masked_) {
      update();
    }
  }

  void setPaintStyle(const InputFramePaintStyle& value) {
    paintStyle_ = value;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    QPainter painter(this);
    detail::input_internal::paintInputFrame(&painter, rect(), paintStyle_);
    QLineEdit::paintEvent(event);

    if (!masked_ || text().isEmpty()) {
      return;
    }

    QPainter textPainter(this);
    textPainter.setRenderHint(QPainter::TextAntialiasing, true);
    const QPalette::ColorGroup group = isEnabled() ? QPalette::Active : QPalette::Disabled;
    textPainter.setPen(palette().color(group, QPalette::Text));
    textPainter.setFont(font());
    textPainter.drawText(rect(), static_cast<int>(alignment() | Qt::AlignVCenter),
                         maskCharacter_.isEmpty() ? QString(QChar(0x2022)) : maskCharacter_);
  }

 private:
  InputFramePaintStyle paintStyle_;
  bool masked_ = false;
  QString maskCharacter_;
};

}  // namespace

AdOtpEdit::AdOtpEdit(QWidget* parent) : QWidget(parent) {
  setFocusPolicy(Qt::StrongFocus);

  rootLayout_ = new QHBoxLayout(this);
  rootLayout_->setContentsMargins(0, 0, 0, 0);
  rootLayout_->setSpacing(8);

  rebuildCells();
}

AdOtpEdit::~AdOtpEdit() = default;

QString AdOtpEdit::code() const { return code_; }

void AdOtpEdit::setCode(const QString& value) {
  const QString next = formattedCode(value);

  if (code_ == next) {
    return;
  }

  code_ = next;
  applyValueToCells(code_);
  emit codeChanged(code_);
  emitInputState();
}

int AdOtpEdit::cellCount() const { return cellCount_; }

void AdOtpEdit::setCellCount(int value) {
  const int normalized = std::max(1, value);
  if (cellCount_ == normalized) {
    return;
  }
  cellCount_ = normalized;
  if (code_.size() > cellCount_) {
    code_ = code_.left(cellCount_);
    emit codeChanged(code_);
  }
  rebuildCells();
  emit cellCountChanged(cellCount_);
}

AdOtpEdit::ControlSize AdOtpEdit::controlSize() const { return controlSize_; }

void AdOtpEdit::setControlSize(ControlSize value) {
  if (controlSize_ == value) {
    return;
  }
  controlSize_ = value;
  applyVisualStyle();
  emit controlSizeChanged(controlSize_);
}

AdOtpEdit::Variant AdOtpEdit::variant() const { return variant_; }

void AdOtpEdit::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  variant_ = value;
  applyVisualStyle();
  emit variantChanged(variant_);
}

AdOtpEdit::Status AdOtpEdit::status() const { return status_; }

void AdOtpEdit::setStatus(Status value) {
  if (status_ == value) {
    return;
  }
  status_ = value;
  applyVisualStyle();
  emit statusChanged(status_);
}

bool AdOtpEdit::maskInput() const { return maskInput_; }

void AdOtpEdit::setMaskInput(bool value) {
  if (maskInput_ == value) {
    return;
  }
  maskInput_ = value;
  updateEchoModes();
  emit maskInputChanged(maskInput_);
}

QString AdOtpEdit::maskCharacter() const { return maskCharacter_; }

void AdOtpEdit::setMaskCharacter(const QString& value) {
  if (maskCharacter_ == value) {
    return;
  }
  maskCharacter_ = value;
  updateEchoModes();
  emit maskCharacterChanged(maskCharacter_);
}

AdOtpCodeFormatter* AdOtpEdit::codeFormatter() const { return codeFormatter_; }

void AdOtpEdit::setCodeFormatter(AdOtpCodeFormatter* value) {
  if (codeFormatter_ == value) {
    return;
  }
  codeFormatter_ = value;
  emit codeFormatterChanged(codeFormatter_);
  setCode(code_);
}

QString AdOtpEdit::separatorText() const { return separatorText_; }

void AdOtpEdit::setSeparatorText(const QString& value) {
  if (separatorText_ == value) {
    return;
  }
  separatorText_ = value;
  rebuildCells();
}

AdOtpSeparatorFactory* AdOtpEdit::separatorFactory() const { return separatorFactory_; }

void AdOtpEdit::setSeparatorFactory(AdOtpSeparatorFactory* value) {
  if (separatorFactory_ == value) {
    return;
  }
  separatorFactory_ = value;
  emit separatorFactoryChanged(separatorFactory_);
  rebuildCells();
}

void AdOtpEdit::focusEditor(int index) {
  if (index < 0 || index >= cells_.size()) {
    return;
  }

  QLineEdit* cell = cells_.at(index);
  if (!cell || !cell->isEnabled()) {
    return;
  }

  cell->setFocus(Qt::OtherFocusReason);
  cell->selectAll();
}

QLineEdit* AdOtpEdit::cellAt(int index) const {
  return (index >= 0 && index < cells_.size()) ? cells_.at(index) : nullptr;
}

bool AdOtpEdit::eventFilter(QObject* watched, QEvent* event) {
  if (!event) {
    return QWidget::eventFilter(watched, event);
  }

  const int index = static_cast<int>(cells_.indexOf(qobject_cast<QLineEdit*>(watched)));
  if (index < 0) {
    return QWidget::eventFilter(watched, event);
  }

  if (event->type() == QEvent::FocusIn) {
    if (QLineEdit* cell = cells_.at(index)) {
      cell->selectAll();
    }
    applyVisualStyle();
  } else if (event->type() == QEvent::FocusOut) {
    applyVisualStyle();
  } else if (event->type() == QEvent::KeyPress) {
    auto* keyEvent = static_cast<QKeyEvent*>(event);
    if (keyEvent->matches(QKeySequence::Paste) ||
        (keyEvent->modifiers().testFlag(Qt::ShiftModifier) && keyEvent->key() == Qt::Key_Insert)) {
      distributeTextFrom(index, QApplication::clipboard()->text());
      return true;
    }
    if (keyEvent->key() == Qt::Key_Left) {
      focusEditor(index - 1);
      return true;
    }
    if (keyEvent->key() == Qt::Key_Right) {
      focusEditor(index + 1);
      return true;
    }
    if (keyEvent->key() == Qt::Key_Backspace && cells_.at(index)->text().isEmpty()) {
      const int previousIndex = index - 1;
      if (QLineEdit* previous = cellAt(previousIndex)) {
        previous->clear();
        focusEditor(previousIndex);
      }
      return true;
    }
  }

  return QWidget::eventFilter(watched, event);
}

void AdOtpEdit::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }

  if (event->type() == QEvent::EnabledChange) {
    for (QLineEdit* cell : cells_) {
      if (cell) {
        cell->setEnabled(isEnabled());
      }
    }
    syncAccessibleState();
  }

  if (event->type() == QEvent::LanguageChange) {
    syncAccessibleState();
  }

  if (event->type() == QEvent::LanguageChange || event->type() == QEvent::EnabledChange ||
      event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::StyleChange) {
    applyVisualStyle();
  }
}

void AdOtpEdit::focusInEvent(QFocusEvent* event) {
  QWidget::focusInEvent(event);
  int target = 0;
  for (int i = 0; i < cells_.size(); ++i) {
    if (cells_.at(i) && cells_.at(i)->text().isEmpty()) {
      target = i;
      break;
    }
  }
  focusEditor(target);
}

void AdOtpEdit::rebuildCells() {
  if (!rootLayout_) {
    return;
  }

  while (QLayoutItem* item = rootLayout_->takeAt(0)) {
    if (item->widget()) {
      item->widget()->deleteLater();
    }
    delete item;
  }

  cells_.clear();
  separators_.clear();

  for (int i = 0; i < cellCount_; ++i) {
    auto* cell = new OtpCellLineEdit(this);
    cell->setObjectName(QStringLiteral("ad-otp-cell-%1").arg(i));
    cell->setAlignment(Qt::AlignCenter);
    cell->setMaxLength(1);
    cell->setEnabled(isEnabled());
    cell->setAccessibleName(tr("Code digit %1 of %2").arg(i + 1).arg(cellCount_));
    cell->installEventFilter(this);

    connect(cell, &QLineEdit::textEdited, this,
            [this, i](const QString& text) { handleCellEdited(i, text); });

    cells_.append(cell);
    rootLayout_->addWidget(cell);

    if (i == cellCount_ - 1) {
      continue;
    }

    QWidget* separator = nullptr;
    if (separatorFactory_) {
      separator = separatorFactory_->createSeparator(i, this);
    }
    if (!separator && !separatorText_.isEmpty()) {
      auto* label = new QLabel(separatorText_, this);
      label->setAlignment(Qt::AlignCenter);
      separator = label;
    }
    if (separator) {
      separators_.append(separator);
      rootLayout_->addWidget(separator);
    }
  }

  setFocusProxy(cells_.isEmpty() ? nullptr : cells_.first());
  applyValueToCells(code_);
  updateEchoModes();
  applyVisualStyle();
  syncAccessibleState();
}

void AdOtpEdit::applyVisualStyle() {
  detail::InputStyleInput input;
  input.controlSize = controlSize_;
  input.variant = variant_;
  input.status = status_;
  input.disabled = !isEnabled();
  input.baseFont = font();
  const InputVisualStyle style = adqt::widgets::detail::resolveInputVisualStyle(
      input, adqt::theme::ThemeManager::instance().resolve(this));

  const int cellSide = std::max(22, style.metrics.height);

  for (QLineEdit* cell : cells_) {
    if (!cell) {
      continue;
    }

    InputFramePaintStyle paintStyle;
    paintStyle.background =
        resolvedBackgroundColor(style, isEnabled(), cell->hasFocus(), cell->underMouse());
    paintStyle.border =
        resolvedBorderColor(style, isEnabled(), cell->hasFocus(), cell->underMouse());
    paintStyle.borderWidth = std::max<qreal>(0.0, style.metrics.borderWidth);
    paintStyle.underlined = style.underlined;
    const qreal radius = style.underlined ? 0.0 : std::max<qreal>(0.0, style.metrics.borderRadius);
    paintStyle.topLeftRadius = radius;
    paintStyle.topRightRadius = radius;
    paintStyle.bottomRightRadius = radius;
    paintStyle.bottomLeftRadius = radius;

    auto* otpCell = static_cast<OtpCellLineEdit*>(cell);
    otpCell->setPaintStyle(paintStyle);
    otpCell->setFont(style.metrics.font);
    otpCell->setFixedSize(cellSide, cellSide);

    QPalette palette = cell->palette();
    palette.setColor(QPalette::Base, QColor(Qt::transparent));
    palette.setColor(QPalette::Window, QColor(Qt::transparent));
    palette.setColor(QPalette::Text,
                     isEnabled() ? style.selectorTextColor : style.disabledTextColor);
    palette.setColor(QPalette::Disabled, QPalette::Text, style.disabledTextColor);
    cell->setPalette(palette);
  }

  for (QWidget* separator : separators_) {
    if (auto* label = qobject_cast<QLabel*>(separator)) {
      QPalette palette = label->palette();
      palette.setColor(QPalette::WindowText,
                       isEnabled() ? style.suffixColor : style.disabledTextColor);
      label->setPalette(palette);
      label->setFont(style.metrics.font);
    }
  }

  rootLayout_->setSpacing(std::max(4, style.metrics.horizontalPadding / 2));
  updateGeometry();
}

void AdOtpEdit::applyValueToCells(const QString& value) {
  internalUpdate_ = true;
  for (int i = 0; i < cells_.size(); ++i) {
    if (QLineEdit* cell = cells_.at(i)) {
      cell->setText(i < value.size() ? QString(value.at(i)) : QString());
    }
  }
  internalUpdate_ = false;
  syncAccessibleState();
}

void AdOtpEdit::emitInputState() {
  QStringList pieces;
  pieces.reserve(cells_.size());

  bool allFilled = !cells_.isEmpty();
  QString joined;
  joined.reserve(cells_.size());

  for (QLineEdit* cell : cells_) {
    const QString piece =
        (cell && !cell->text().isEmpty()) ? QString(cell->text().at(0)) : QString();
    pieces.push_back(piece);
    joined += piece;
    if (piece.isEmpty()) {
      allFilled = false;
    }
  }

  emit cellsChanged(pieces);
  if (code_ != joined) {
    code_ = joined;
    emit codeChanged(code_);
  }
  if (allFilled && pieces.size() == cellCount_) {
    emit codeCompleted(code_);
  }
}

void AdOtpEdit::handleCellEdited(int index, const QString& text) {
  if (internalUpdate_ || index < 0 || index >= cells_.size()) {
    return;
  }

  if (text.size() > 1) {
    distributeTextFrom(index, text);
    return;
  }

  QString joined;
  joined.reserve(cells_.size());
  for (int i = 0; i < cells_.size(); ++i) {
    if (i == index) {
      joined += text.left(1);
      continue;
    }
    joined += cells_.at(i) ? cells_.at(i)->text().left(1) : QString();
  }

  code_ = formattedCode(joined);
  applyValueToCells(code_);
  if (!text.isEmpty()) {
    focusEditor(std::min(index + 1, static_cast<int>(cells_.size() - 1)));
  }
  emit codeChanged(code_);
  emitInputState();
}

void AdOtpEdit::distributeTextFrom(int index, const QString& text) {
  if (index < 0 || index >= cells_.size()) {
    return;
  }

  QString filtered;
  filtered.reserve(text.size());
  for (const QChar ch : text) {
    if (!ch.isSpace()) {
      filtered.append(ch);
    }
  }
  if (filtered.isEmpty()) {
    return;
  }

  QStringList parts;
  parts.reserve(cells_.size());
  for (QLineEdit* cell : cells_) {
    parts.push_back(cell ? cell->text().left(1) : QString());
  }

  int cursor = index;
  for (const QChar ch : filtered) {
    if (cursor >= parts.size()) {
      break;
    }
    parts[cursor] = QString(ch);
    ++cursor;
  }

  QString joined;
  joined.reserve(parts.size());
  for (const QString& part : parts) {
    joined += part;
  }

  code_ = formattedCode(joined);
  applyValueToCells(code_);
  focusEditor(std::min(cursor, static_cast<int>(cells_.size() - 1)));
  emit codeChanged(code_);
  emitInputState();
}

void AdOtpEdit::updateEchoModes() {
  for (QLineEdit* cell : cells_) {
    if (!cell) {
      continue;
    }
    auto* otpCell = static_cast<OtpCellLineEdit*>(cell);
    otpCell->setMaskCharacter(maskCharacter_);
    otpCell->setMasked(maskInput_);
  }
  syncAccessibleState();
}

QString AdOtpEdit::formattedCode(const QString& value) const {
  QString next = value.left(cellCount_);
  if (codeFormatter_) {
    next = codeFormatter_->formatCode(next).left(cellCount_);
  }
  return next;
}

void AdOtpEdit::syncAccessibleState() {
  setAccessibleName(tr("One-time code input"));
  setAccessibleDescription(tr("Enter the %1-character verification code").arg(cells_.size()));

  for (int i = 0; i < cells_.size(); ++i) {
    QLineEdit* cell = cells_.at(i);
    if (!cell) {
      continue;
    }

    cell->setAccessibleName(tr("Code digit %1 of %2").arg(i + 1).arg(cells_.size()));
    if (cell->text().isEmpty()) {
      cell->setAccessibleDescription(tr("Verification digit %1 is empty").arg(i + 1));
    } else if (maskInput_) {
      cell->setAccessibleDescription(tr("Verification digit %1 has been entered").arg(i + 1));
    } else {
      cell->setAccessibleDescription(
          tr("Verification digit %1, current value %2").arg(i + 1).arg(cell->text()));
    }
  }

  for (QWidget* separator : separators_) {
    if (!separator) {
      continue;
    }
    separator->setAccessibleName(QString());
    separator->setAccessibleDescription(QString());
  }
}

}  // namespace adqt::widgets
