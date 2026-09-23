#include "checkbox.h"
#include "detail/pointer_region.h"

#include "checkbox_group.h"
#include "checkbox_style.h"
#include "interaction_overlay_manager.h"
#include "theme/theme.h"

#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>

#include <algorithm>
#include <utility>

namespace adqt::widgets {

namespace {

constexpr qreal kWaveStrokeScale = 0.6;

template <typename T>
void mergeOptional(std::optional<T>* target, const std::optional<T>& source) {
  if (target && source.has_value()) {
    *target = source;
  }
}

void mergeTokens(AdCheckbox::ComponentTokens* target, const AdCheckbox::ComponentTokens& source) {
  if (!target) {
    return;
  }
  mergeOptional(&target->colors.textColor, source.colors.textColor);
  mergeOptional(&target->colors.indicatorBorderColor, source.colors.indicatorBorderColor);
  mergeOptional(&target->colors.indicatorFillColor, source.colors.indicatorFillColor);
  mergeOptional(&target->colors.indicatorMarkColor, source.colors.indicatorMarkColor);
  mergeOptional(&target->colors.focusRingColor, source.colors.focusRingColor);
  mergeOptional(&target->colors.waveColor, source.colors.waveColor);
  mergeOptional(&target->metrics.checkboxSize, source.metrics.checkboxSize);
  mergeOptional(&target->metrics.borderWidth, source.metrics.borderWidth);
  mergeOptional(&target->metrics.borderRadius, source.metrics.borderRadius);
  mergeOptional(&target->metrics.markWidth, source.metrics.markWidth);
  mergeOptional(&target->metrics.labelPaddingInlineStart, source.metrics.labelPaddingInlineStart);
  mergeOptional(&target->metrics.labelPaddingInlineEnd, source.metrics.labelPaddingInlineEnd);
  mergeOptional(&target->metrics.textLineHeight, source.metrics.textLineHeight);
  mergeOptional(&target->metrics.wrapperMarginInlineEnd, source.metrics.wrapperMarginInlineEnd);
  mergeOptional(&target->metrics.focusOutlineWidth, source.metrics.focusOutlineWidth);
  mergeOptional(&target->metrics.focusOutlineOffset, source.metrics.focusOutlineOffset);
}

bool keyboardFocusReason(Qt::FocusReason reason) {
  return reason != Qt::MouseFocusReason && reason != Qt::NoFocusReason;
}

QRect textBounds(const QFontMetrics& metrics, const QString& text) {
  if (text.isEmpty()) {
    return QRect();
  }
  return metrics.boundingRect(QRect(0, 0, 8192, metrics.height() * 2),
                              Qt::TextShowMnemonic | Qt::TextSingleLine, text);
}

QSize effectiveIconSize(const AdCheckbox* checkbox, const QFontMetrics& metrics) {
  if (!checkbox || checkbox->icon().isNull()) {
    return QSize();
  }
  const QSize requested = checkbox->iconSize();
  if (requested.isValid() && !requested.isEmpty()) {
    return requested;
  }
  const int side = std::max(12, metrics.height());
  return QSize(side, side);
}

struct LabelMetrics {
  QSize iconSize;
  QRect textRect;
  int gap = 4;
  int width = 0;
  int height = 0;
};

LabelMetrics measureLabel(const AdCheckbox* checkbox, const detail::CheckboxMetrics& style) {
  LabelMetrics result;
  if (!checkbox) {
    return result;
  }
  const QFontMetrics metrics(style.font);
  result.iconSize = effectiveIconSize(checkbox, metrics);
  result.textRect = textBounds(metrics, checkbox->text());
  if (!result.iconSize.isEmpty()) {
    result.width += result.iconSize.width();
    result.height = std::max(result.height, result.iconSize.height());
  }
  if (!result.textRect.isEmpty()) {
    if (result.width > 0) {
      result.width += result.gap;
    }
    result.width += result.textRect.width();
    result.height = std::max(result.height, result.textRect.height());
  }
  return result;
}

detail::CheckboxStateStyle stateStyle(const detail::CheckboxVisualStyle& style, bool enabled,
                                      bool checked, bool indeterminate, bool hovered,
                                      bool pressed) {
  if (!enabled) {
    if (indeterminate) {
      return style.indeterminateDisabled;
    }
    return checked ? style.checkedDisabled : style.disabled;
  }
  if (indeterminate) {
    return (hovered || pressed) ? style.indeterminateHover : style.indeterminate;
  }
  if (checked) {
    return (hovered || pressed) ? style.checkedHover : style.checked;
  }
  return (hovered || pressed) ? style.hover : style.normal;
}

QRectF translatedToWindow(const QWidget* widget, const QRectF& rect) {
  if (!widget || !widget->window()) {
    return QRectF();
  }
  const QPoint origin = widget->mapTo(widget->window(), QPoint(0, 0));
  return rect.translated(origin.x(), origin.y());
}

}  // namespace

AdCheckbox::AdCheckbox(QWidget* parent) : QCheckBox(parent) {
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_Hover, true);
  explicitCursorOverride_ = testAttribute(Qt::WA_SetCursor);
  refreshAutomaticCursor();
  connect(this, &QAbstractButton::toggled, this, [this]() {
    updateInteractionFocusOverlay();
    update();
  });
  connect(this, &QCheckBox::checkStateChanged, this, [this]() {
    const bool effective = isIndeterminate();
    if (reportedIndeterminate_ != effective) {
      reportedIndeterminate_ = effective;
      emit indeterminateChanged(effective);
    }
  });
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { refreshAfterPropertyChange(); });
}

AdCheckbox::AdCheckbox(const QString& text, QWidget* parent) : AdCheckbox(parent) { setText(text); }

AdCheckbox::~AdCheckbox() {
  stopInteractionWaveForOwner(this);
  stopInteractionFocusForOwner(this);
}

void AdCheckbox::setCursor(const QCursor& cursor) {
  explicitCursorOverride_ = true;
  autoCursorManaged_ = false;
  applyingAutoCursor_ = true;
  QCheckBox::setCursor(cursor);
  applyingAutoCursor_ = false;
}

void AdCheckbox::unsetCursor() {
  explicitCursorOverride_ = false;
  autoCursorManaged_ = false;
  applyingAutoCursor_ = true;
  QCheckBox::unsetCursor();
  applyingAutoCursor_ = false;
  refreshAutomaticCursor();
}

bool AdCheckbox::isIndeterminate() const { return checkState() == Qt::PartiallyChecked; }

void AdCheckbox::setIndeterminate(bool value) {
  if (isIndeterminate() == value) {
    return;
  }
  setCheckState(value ? Qt::PartiallyChecked : Qt::Unchecked);
}

QVariant AdCheckbox::value() const { return value_; }

void AdCheckbox::setValue(const QVariant& value) {
  if (value_ == value) {
    return;
  }
  value_ = value;
  emit valueChanged(value_);
}

AdCheckbox::ComponentTokens AdCheckbox::componentTokens() const { return componentTokens_; }

void AdCheckbox::setComponentTokens(const ComponentTokens& value) {
  if (componentTokensEqual(componentTokens_, value)) {
    return;
  }
  componentTokens_ = value;
  refreshAfterPropertyChange();
  emit componentTokensChanged();
}

void AdCheckbox::resetComponentTokens() { setComponentTokens({}); }

void AdCheckbox::setComponentTokenResolver(ComponentTokenResolver resolver) {
  if (!componentTokenResolver_ && !resolver) {
    return;
  }
  componentTokenResolver_ = std::move(resolver);
  refreshAfterPropertyChange();
  emit componentTokensChanged();
}

void AdCheckbox::resetComponentTokenResolver() { setComponentTokenResolver({}); }

AdCheckbox::ComponentTokenContext AdCheckbox::currentComponentTokenContext() const {
  ComponentTokenContext context;
  context.checked = checkState() == Qt::Checked;
  context.indeterminate = isIndeterminate();
  context.disabled = !effectiveEnabled();
  context.hovered = detail::widgetHovered(this);
  context.pressed = pressed_;
  context.focused = hasFocus() && focusVisible_;
  return context;
}

bool AdCheckbox::componentTokensEqual(const ComponentTokens& lhs, const ComponentTokens& rhs) {
  return lhs.colors.textColor == rhs.colors.textColor &&
         lhs.colors.indicatorBorderColor == rhs.colors.indicatorBorderColor &&
         lhs.colors.indicatorFillColor == rhs.colors.indicatorFillColor &&
         lhs.colors.indicatorMarkColor == rhs.colors.indicatorMarkColor &&
         lhs.colors.focusRingColor == rhs.colors.focusRingColor &&
         lhs.colors.waveColor == rhs.colors.waveColor &&
         lhs.metrics.checkboxSize == rhs.metrics.checkboxSize &&
         lhs.metrics.borderWidth == rhs.metrics.borderWidth &&
         lhs.metrics.borderRadius == rhs.metrics.borderRadius &&
         lhs.metrics.markWidth == rhs.metrics.markWidth &&
         lhs.metrics.labelPaddingInlineStart == rhs.metrics.labelPaddingInlineStart &&
         lhs.metrics.labelPaddingInlineEnd == rhs.metrics.labelPaddingInlineEnd &&
         lhs.metrics.textLineHeight == rhs.metrics.textLineHeight &&
         lhs.metrics.wrapperMarginInlineEnd == rhs.metrics.wrapperMarginInlineEnd &&
         lhs.metrics.focusOutlineWidth == rhs.metrics.focusOutlineWidth &&
         lhs.metrics.focusOutlineOffset == rhs.metrics.focusOutlineOffset;
}

AdCheckbox::ComponentTokens AdCheckbox::resolvedComponentTokens() const {
  ComponentTokens result;
  const ComponentTokenContext context = currentComponentTokenContext();
  if (group_) {
    mergeTokens(&result, group_->componentTokens());
    if (group_->componentTokenResolver_) {
      mergeTokens(&result, group_->componentTokenResolver_(context));
    }
  }
  mergeTokens(&result, componentTokens_);
  if (componentTokenResolver_) {
    mergeTokens(&result, componentTokenResolver_(context));
  }
  return result;
}

detail::CheckboxStyleInput AdCheckbox::buildStyleInput() const {
  const ComponentTokenContext context = currentComponentTokenContext();
  detail::CheckboxStyleInput input;
  input.checked = context.checked;
  input.indeterminate = context.indeterminate;
  input.hovered = context.hovered;
  input.pressed = context.pressed;
  input.focused = context.focused;
  input.baseFont = font();
  input.componentTokens = resolvedComponentTokens();
  return input;
}

QSize AdCheckbox::sizeHint() const {
  const auto resolved = adqt::theme::ThemeManager::instance().resolve(this);
  const detail::CheckboxVisualStyle style =
      detail::resolveCheckboxVisualStyle(buildStyleInput(), resolved);
  const LabelMetrics label = measureLabel(this, style.metrics);
  int width = style.metrics.checkboxSize;
  if (label.width > 0) {
    width +=
        style.metrics.labelPaddingInlineStart + label.width + style.metrics.labelPaddingInlineEnd;
  }
  return QSize(width,
               std::max({style.metrics.checkboxSize, style.metrics.textLineHeight, label.height}));
}

QSize AdCheckbox::minimumSizeHint() const { return sizeHint(); }

QRectF AdCheckbox::indicatorRect(int checkboxSize) const {
  const qreal x =
      layoutDirection() == Qt::RightToLeft ? rect().right() - checkboxSize + 1.0 : rect().left();
  const qreal y = rect().top() + (rect().height() - checkboxSize) / 2.0;
  return QRectF(x, y, checkboxSize, checkboxSize);
}

void AdCheckbox::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)
  const auto resolved = adqt::theme::ThemeManager::instance().resolve(this);
  const detail::CheckboxStyleInput input = buildStyleInput();
  const detail::CheckboxVisualStyle style = detail::resolveCheckboxVisualStyle(input, resolved);
  const detail::CheckboxStateStyle state =
      stateStyle(style, effectiveEnabled(), isChecked(), isIndeterminate(),
                 detail::widgetHovered(this), pressed_);
  const QRectF indicator = indicatorRect(style.metrics.checkboxSize);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setFont(style.metrics.font);

  const qreal inset = style.metrics.borderWidth > 0 ? style.metrics.borderWidth / 2.0 + 0.5 : 0.0;
  const QRectF box = indicator.adjusted(inset, inset, -inset, -inset);
  QPainterPath boxPath;
  boxPath.addRoundedRect(box, style.metrics.borderRadius, style.metrics.borderRadius);
  painter.fillPath(boxPath, state.backgroundColor);
  if (style.metrics.borderWidth > 0 && state.borderColor.alpha() > 0) {
    painter.setPen(QPen(state.borderColor, style.metrics.borderWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(boxPath);
  }

  if (isIndeterminate()) {
    const qreal markSize = std::max<qreal>(2.0, indicator.width() / 2.0);
    QRectF mark(indicator.center().x() - markSize / 2.0, indicator.center().y() - markSize / 2.0,
                markSize, markSize);
    painter.fillRect(mark, state.markColor);
  } else if (isChecked()) {
    QPainterPath mark;
    mark.moveTo(indicator.left() + indicator.width() * 0.27,
                indicator.top() + indicator.height() * 0.52);
    mark.lineTo(indicator.left() + indicator.width() * 0.44,
                indicator.top() + indicator.height() * 0.69);
    mark.lineTo(indicator.left() + indicator.width() * 0.75,
                indicator.top() + indicator.height() * 0.34);
    painter.setPen(QPen(state.markColor, style.metrics.markWidth, Qt::SolidLine, Qt::SquareCap,
                        Qt::MiterJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(mark);
  }

  const LabelMetrics label = measureLabel(this, style.metrics);
  if (label.width <= 0) {
    return;
  }
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  const qreal labelStart =
      rtl ? indicator.left() - style.metrics.labelPaddingInlineStart - label.width
          : indicator.right() + style.metrics.labelPaddingInlineStart;
  qreal cursor = labelStart;
  QRectF iconRect;
  QRectF textRect;
  if (rtl) {
    if (!label.textRect.isEmpty()) {
      textRect = QRectF(cursor, (height() - label.textRect.height()) / 2.0, label.textRect.width(),
                        label.textRect.height());
      cursor += label.textRect.width() + (label.iconSize.isEmpty() ? 0 : label.gap);
    }
    if (!label.iconSize.isEmpty()) {
      iconRect = QRectF(cursor, (height() - label.iconSize.height()) / 2.0, label.iconSize.width(),
                        label.iconSize.height());
    }
  } else {
    if (!label.iconSize.isEmpty()) {
      iconRect = QRectF(cursor, (height() - label.iconSize.height()) / 2.0, label.iconSize.width(),
                        label.iconSize.height());
      cursor += label.iconSize.width() + (label.textRect.isEmpty() ? 0 : label.gap);
    }
    if (!label.textRect.isEmpty()) {
      textRect = QRectF(cursor, (height() - label.textRect.height()) / 2.0, label.textRect.width(),
                        label.textRect.height());
    }
  }

  if (!iconRect.isEmpty()) {
    const QIcon::Mode mode = effectiveEnabled() ? QIcon::Normal : QIcon::Disabled;
    const QIcon::State iconState = checkState() == Qt::Checked ? QIcon::On : QIcon::Off;
    const QPixmap pixmap = icon().pixmap(label.iconSize, mode, iconState);
    painter.drawPixmap(iconRect.toAlignedRect(), pixmap);
  }
  if (!textRect.isEmpty()) {
    QPalette palette = this->palette();
    palette.setColor(QPalette::WindowText, state.labelColor);
    this->style()->drawItemText(
        &painter, textRect.toAlignedRect(),
        Qt::AlignVCenter | Qt::AlignLeft | Qt::TextShowMnemonic | Qt::TextSingleLine, palette,
        effectiveEnabled(), text(), QPalette::WindowText);
  }
}

bool AdCheckbox::event(QEvent* event) {
  detail::resetWidgetHoverOnLifecycle(this, event);
  const bool handled = QCheckBox::event(event);
  if (!event) {
    return handled;
  }

  if (event->type() == QEvent::Polish || event->type() == QEvent::PolishRequest) {
    const bool explicitCursorOverride = !autoCursorManaged_ && testAttribute(Qt::WA_SetCursor);
    if (explicitCursorOverride_ != explicitCursorOverride) {
      autoCursorManaged_ = false;
      explicitCursorOverride_ = explicitCursorOverride;
      refreshAutomaticCursor();
    }
  }

  if (event->type() == QEvent::CursorChange && !applyingAutoCursor_) {
    autoCursorManaged_ = false;
    explicitCursorOverride_ = testAttribute(Qt::WA_SetCursor);
    if (!cursorRefreshPending_) {
      cursorRefreshPending_ = true;
      QMetaObject::invokeMethod(
          this,
          [this]() {
            cursorRefreshPending_ = false;
            explicitCursorOverride_ = testAttribute(Qt::WA_SetCursor);
            refreshAutomaticCursor();
          },
          Qt::QueuedConnection);
    }
  }
  return handled;
}

void AdCheckbox::nextCheckState() {
  if (!interactionBlocked()) {
    QCheckBox::nextCheckState();
  }
}

bool AdCheckbox::hitButton(const QPoint& pos) const {
  return !interactionBlocked() && QCheckBox::hitButton(pos);
}

void AdCheckbox::enterEvent(QEnterEvent* event) {
  QCheckBox::enterEvent(event);
  update();
}

void AdCheckbox::leaveEvent(QEvent* event) {
  pressed_ = false;
  QCheckBox::leaveEvent(event);
  update();
}

void AdCheckbox::mousePressEvent(QMouseEvent* event) {
  if (!event) {
    return;
  }
  if (interactionBlocked()) {
    pressed_ = false;
    setDown(false);
    event->ignore();
    return;
  }
  if (event->button() == Qt::LeftButton) {
    focusVisible_ = false;
    pressed_ = true;
    updateInteractionFocusOverlay();
  }
  QCheckBox::mousePressEvent(event);
  update();
}

void AdCheckbox::mouseReleaseEvent(QMouseEvent* event) {
  if (!event) {
    return;
  }
  if (interactionBlocked()) {
    pressed_ = false;
    setDown(false);
    event->ignore();
    return;
  }
  const bool wave =
      event->button() == Qt::LeftButton && pressed_ && hitButton(event->position().toPoint());
  pressed_ = false;
  QCheckBox::mouseReleaseEvent(event);
  if (wave) {
    triggerInteractionWaveOverlay();
  }
  update();
}

void AdCheckbox::keyPressEvent(QKeyEvent* event) {
  if (!event) {
    return;
  }
  if (interactionBlocked()) {
    pressed_ = false;
    setDown(false);
    event->ignore();
    return;
  }
  if (event->key() == Qt::Key_Space) {
    pressed_ = true;
    update();
  }
  QCheckBox::keyPressEvent(event);
}

void AdCheckbox::keyReleaseEvent(QKeyEvent* event) {
  if (!event) {
    return;
  }
  const bool wave = !interactionBlocked() && event->key() == Qt::Key_Space;
  pressed_ = false;
  if (interactionBlocked()) {
    setDown(false);
    event->ignore();
    update();
    return;
  }
  QCheckBox::keyReleaseEvent(event);
  if (wave) {
    triggerInteractionWaveOverlay();
  }
  update();
}

void AdCheckbox::focusInEvent(QFocusEvent* event) {
  focusVisible_ = event && keyboardFocusReason(event->reason());
  QCheckBox::focusInEvent(event);
  updateInteractionFocusOverlay();
  update();
}

void AdCheckbox::focusOutEvent(QFocusEvent* event) {
  focusVisible_ = false;
  pressed_ = false;
  setDown(false);
  QCheckBox::focusOutEvent(event);
  stopInteractionFocusForOwner(this);
  update();
}

void AdCheckbox::moveEvent(QMoveEvent* event) {
  QCheckBox::moveEvent(event);
  updateInteractionFocusOverlay();
}

void AdCheckbox::resizeEvent(QResizeEvent* event) {
  QCheckBox::resizeEvent(event);
  updateInteractionFocusOverlay();
}

void AdCheckbox::showEvent(QShowEvent* event) {
  QCheckBox::showEvent(event);
  updateInteractionFocusOverlay();
}

void AdCheckbox::hideEvent(QHideEvent* event) {
  pressed_ = false;
  setDown(false);
  QCheckBox::hideEvent(event);
  stopInteractionFocusForOwner(this);
  stopInteractionWaveForOwner(this);
}

void AdCheckbox::changeEvent(QEvent* event) {
  QCheckBox::changeEvent(event);
  if (!event) {
    return;
  }
  switch (event->type()) {
    case QEvent::EnabledChange:
      if (interactionBlocked()) {
        pressed_ = false;
        setDown(false);
        stopInteractionFocusForOwner(this);
        stopInteractionWaveForOwner(this);
      }
      refreshAutomaticCursor();
      updateInteractionFocusOverlay();
      update();
      break;
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
      refreshAfterPropertyChange();
      break;
    default:
      break;
  }
}

bool AdCheckbox::effectiveEnabled() const {
  return isEnabled() && (!group_ || group_->isEnabled());
}

bool AdCheckbox::interactionBlocked() const { return !effectiveEnabled(); }

int AdCheckbox::horizontalSpacingHint() const {
  const auto resolved = adqt::theme::ThemeManager::instance().resolve(this);
  return detail::resolveCheckboxVisualStyle(buildStyleInput(), resolved)
      .metrics.wrapperMarginInlineEnd;
}

void AdCheckbox::refreshAfterPropertyChange(bool geometryChanged) {
  if (geometryChanged) {
    updateGeometry();
    if (group_) {
      group_->refreshLayoutSpacing();
    }
  }
  refreshAutomaticCursor();
  updateInteractionFocusOverlay();
  update();
}

void AdCheckbox::refreshAutomaticCursor() {
  if (explicitCursorOverride_) {
    return;
  }
  applyAutomaticCursor(interactionBlocked()
                           ? std::optional<Qt::CursorShape>{}
                           : std::optional<Qt::CursorShape>{Qt::PointingHandCursor});
}

void AdCheckbox::applyAutomaticCursor(std::optional<Qt::CursorShape> cursorShape) {
  if (explicitCursorOverride_) {
    return;
  }

  if (cursorShape.has_value()) {
    if (!autoCursorManaged_ || !testAttribute(Qt::WA_SetCursor) ||
        cursor().shape() != cursorShape.value()) {
      applyingAutoCursor_ = true;
      QCheckBox::setCursor(QCursor(cursorShape.value()));
      applyingAutoCursor_ = false;
    }
    autoCursorManaged_ = true;
    return;
  }

  if (autoCursorManaged_ || testAttribute(Qt::WA_SetCursor)) {
    applyingAutoCursor_ = true;
    QCheckBox::unsetCursor();
    applyingAutoCursor_ = false;
  }
  autoCursorManaged_ = false;
}

void AdCheckbox::updateInteractionFocusOverlay() {
  if (!(hasFocus() && focusVisible_) || interactionBlocked() || !isVisible()) {
    stopInteractionFocusForOwner(this);
    return;
  }
  const auto resolved = adqt::theme::ThemeManager::instance().resolve(this);
  const detail::CheckboxStyleInput input = buildStyleInput();
  const detail::CheckboxVisualStyle style = detail::resolveCheckboxVisualStyle(input, resolved);
  const QRectF box = indicatorRect(style.metrics.checkboxSize);
  InteractionFocusRequest request;
  request.owner = this;
  request.baseRectInWindow = translatedToWindow(this, box);
  request.topLeft = request.topRight = request.bottomRight = request.bottomLeft =
      style.metrics.borderRadius;
  request.color = style.metrics.focusOutlineColor;
  request.strokeWidth = style.metrics.focusOutlineWidth;
  request.offset = style.metrics.focusOutlineOffset;
  triggerInteractionFocus(request);
}

void AdCheckbox::triggerInteractionWaveOverlay() {
  if (interactionBlocked() || !isVisible()) {
    return;
  }
  const auto resolved = adqt::theme::ThemeManager::instance().resolve(this);
  const detail::CheckboxStyleInput input = buildStyleInput();
  const detail::CheckboxVisualStyle style = detail::resolveCheckboxVisualStyle(input, resolved);
  const QRectF box = indicatorRect(style.metrics.checkboxSize);
  InteractionWaveRequest request;
  request.owner = this;
  request.baseRectInWindow = translatedToWindow(this, box);
  request.topLeft = request.topRight = request.bottomRight = request.bottomLeft =
      style.metrics.borderRadius;
  request.color = style.metrics.waveColor;
  request.strokeWidthScale = kWaveStrokeScale;
  triggerInteractionWave(request);
}

void AdCheckbox::setGroup(AdCheckboxGroup* group) {
  if (group_ == group) {
    return;
  }
  group_ = group;
  refreshAfterPropertyChange();
}

}  // namespace adqt::widgets
