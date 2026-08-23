#include "radio.h"

#include "interaction_overlay_manager.h"
#include "radio_button_group.h"
#include "radio_style.h"
#include "theme/theme.h"

#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>

#include <algorithm>
#include <tuple>
#include <utility>

namespace adqt::widgets {

namespace {

constexpr qreal kDefaultRadioWaveStrokeScale = 0.6;
constexpr int kMinimumContentGap = 4;

bool isArrowKey(int key) {
  return key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down;
}

bool colorTokensEqual(const AdRadio::ColorTokens& lhs, const AdRadio::ColorTokens& rhs) {
  return std::tie(lhs.textColor, lhs.indicatorBorderColor, lhs.indicatorFillColor,
                  lhs.indicatorDotColor, lhs.buttonTextColor, lhs.buttonFillColor,
                  lhs.buttonBorderColor, lhs.focusRingColor, lhs.waveColor) ==
         std::tie(rhs.textColor, rhs.indicatorBorderColor, rhs.indicatorFillColor,
                  rhs.indicatorDotColor, rhs.buttonTextColor, rhs.buttonFillColor,
                  rhs.buttonBorderColor, rhs.focusRingColor, rhs.waveColor);
}

bool metricTokensEqual(const AdRadio::MetricTokens& lhs, const AdRadio::MetricTokens& rhs) {
  return std::tie(lhs.radioSize, lhs.dotSize, lhs.borderWidth, lhs.labelPaddingInlineStart,
                  lhs.labelPaddingInlineEnd, lhs.textLineHeight, lhs.wrapperMarginInlineEnd,
                  lhs.buttonHeight, lhs.buttonPaddingInline, lhs.buttonBorderRadius,
                  lhs.focusOutlineWidth, lhs.focusOutlineOffset) ==
         std::tie(rhs.radioSize, rhs.dotSize, rhs.borderWidth, rhs.labelPaddingInlineStart,
                  rhs.labelPaddingInlineEnd, rhs.textLineHeight, rhs.wrapperMarginInlineEnd,
                  rhs.buttonHeight, rhs.buttonPaddingInline, rhs.buttonBorderRadius,
                  rhs.focusOutlineWidth, rhs.focusOutlineOffset);
}

bool componentTokensEqual(const AdRadio::ComponentTokens& lhs,
                          const AdRadio::ComponentTokens& rhs) {
  return colorTokensEqual(lhs.colors, rhs.colors) && metricTokensEqual(lhs.metrics, rhs.metrics);
}

bool radioStyleInputsEqual(const detail::RadioStyleInput& lhs, const detail::RadioStyleInput& rhs) {
  return lhs.controlSize == rhs.controlSize && lhs.checked == rhs.checked &&
         lhs.hovered == rhs.hovered && lhs.pressed == rhs.pressed && lhs.focused == rhs.focused &&
         lhs.baseFont == rhs.baseFont &&
         componentTokensEqual(lhs.componentTokens, rhs.componentTokens);
}

bool radioButtonStyleInputsEqual(const detail::RadioButtonStyleInput& lhs,
                                 const detail::RadioButtonStyleInput& rhs) {
  return lhs.controlSize == rhs.controlSize && lhs.buttonStyle == rhs.buttonStyle &&
         lhs.checked == rhs.checked && lhs.hovered == rhs.hovered && lhs.pressed == rhs.pressed &&
         lhs.focused == rhs.focused && lhs.baseFont == rhs.baseFont &&
         componentTokensEqual(lhs.componentTokens, rhs.componentTokens);
}

template <typename T>
void mergeOptional(std::optional<T>* target, const std::optional<T>& source) {
  if (target && source.has_value()) {
    *target = source;
  }
}

void mergeColorTokens(AdRadio::ColorTokens* target, const AdRadio::ColorTokens& source) {
  if (!target) {
    return;
  }
  mergeOptional(&target->textColor, source.textColor);
  mergeOptional(&target->indicatorBorderColor, source.indicatorBorderColor);
  mergeOptional(&target->indicatorFillColor, source.indicatorFillColor);
  mergeOptional(&target->indicatorDotColor, source.indicatorDotColor);
  mergeOptional(&target->buttonTextColor, source.buttonTextColor);
  mergeOptional(&target->buttonFillColor, source.buttonFillColor);
  mergeOptional(&target->buttonBorderColor, source.buttonBorderColor);
  mergeOptional(&target->focusRingColor, source.focusRingColor);
  mergeOptional(&target->waveColor, source.waveColor);
}

void mergeMetricTokens(AdRadio::MetricTokens* target, const AdRadio::MetricTokens& source) {
  if (!target) {
    return;
  }
  mergeOptional(&target->radioSize, source.radioSize);
  mergeOptional(&target->dotSize, source.dotSize);
  mergeOptional(&target->borderWidth, source.borderWidth);
  mergeOptional(&target->labelPaddingInlineStart, source.labelPaddingInlineStart);
  mergeOptional(&target->labelPaddingInlineEnd, source.labelPaddingInlineEnd);
  mergeOptional(&target->textLineHeight, source.textLineHeight);
  mergeOptional(&target->wrapperMarginInlineEnd, source.wrapperMarginInlineEnd);
  mergeOptional(&target->buttonHeight, source.buttonHeight);
  mergeOptional(&target->buttonPaddingInline, source.buttonPaddingInline);
  mergeOptional(&target->buttonBorderRadius, source.buttonBorderRadius);
  mergeOptional(&target->focusOutlineWidth, source.focusOutlineWidth);
  mergeOptional(&target->focusOutlineOffset, source.focusOutlineOffset);
}

void mergeComponentTokens(AdRadio::ComponentTokens* target,
                          const AdRadio::ComponentTokens& source) {
  if (!target) {
    return;
  }
  mergeColorTokens(&target->colors, source.colors);
  mergeMetricTokens(&target->metrics, source.metrics);
}

bool isKeyboardFocusReason(Qt::FocusReason reason) {
  return reason != Qt::MouseFocusReason && reason != Qt::NoFocusReason;
}

QPoint mouseEventPos(const QMouseEvent* event) {
  if (!event) {
    return QPoint();
  }
  return event->position().toPoint();
}

qreal snapToDevicePixelSize(qreal value, qreal dpr) {
  if (dpr <= 0.0) {
    return value;
  }
  const qreal snapped = qRound(value * dpr) / dpr;
  return std::max(snapped, 1.0 / dpr);
}

qreal centeredStrokeInset(qreal strokeWidth) {
  return std::max<qreal>(0.0, strokeWidth / 2.0) + 0.5;
}

int collapsedStrokeOverlapPixels(qreal strokeWidth) {
  return std::max(1, qRound(centeredStrokeInset(strokeWidth) * 2.0));
}

QPointF widgetPaintOrigin(const QWidget* widget) {
  if (!widget) {
    return QPointF();
  }

  const QWidget* const hostWindow = widget->window();
  if (!hostWindow) {
    return QPointF();
  }

  return QPointF(widget->mapTo(hostWindow, QPoint(0, 0)));
}

qreal snapToDevicePixelCoord(qreal value, qreal dpr, qreal origin) {
  if (dpr <= 0.0) {
    return value;
  }
  return qRound((value + origin) * dpr) / dpr - origin;
}

QRectF snapRectToDevicePixels(const QRectF& rect, qreal dpr, const QPointF& origin = QPointF()) {
  if (dpr <= 0.0) {
    return rect;
  }

  const qreal left = snapToDevicePixelCoord(rect.left(), dpr, origin.x());
  const qreal top = snapToDevicePixelCoord(rect.top(), dpr, origin.y());
  const qreal right = snapToDevicePixelCoord(rect.left() + rect.width(), dpr, origin.x());
  const qreal bottom = snapToDevicePixelCoord(rect.top() + rect.height(), dpr, origin.y());
  const qreal minSize = 1.0 / dpr;

  return QRectF(left, top, std::max(minSize, right - left), std::max(minSize, bottom - top));
}

QRectF centeredSquare(const QPointF& center, qreal size) {
  const qreal halfSize = size / 2.0;
  return QRectF(center.x() - halfSize, center.y() - halfSize, size, size);
}

QRect textBoundsForButton(const QFontMetrics& metrics, const QString& text) {
  if (text.isEmpty()) {
    return QRect();
  }
  return metrics.boundingRect(QRect(0, 0, 8192, metrics.height() * 2),
                              Qt::TextShowMnemonic | Qt::TextSingleLine, text);
}

QSize effectiveButtonIconSize(const AdRadio* radio, const QFontMetrics& metrics) {
  if (!radio || radio->icon().isNull()) {
    return QSize();
  }

  const QSize requested = radio->iconSize();
  if (requested.isValid() && !requested.isEmpty()) {
    return requested;
  }

  const int side = std::max(12, metrics.height());
  return QSize(side, side);
}

int contentGapForMetrics(const detail::RadioMetrics& metrics) {
  return std::max(kMinimumContentGap, metrics.labelPaddingInlineStart / 2);
}

struct RadioLabelContent {
  QSize iconSize;
  QRect textBounds;
  int gap = 0;
  int width = 0;

  bool hasContent() const { return width > 0; }
};

RadioLabelContent measureRadioLabelContent(const AdRadio* radio,
                                           const detail::RadioMetrics& metrics) {
  RadioLabelContent content;
  if (!radio) {
    return content;
  }

  const QFontMetrics fontMetrics(metrics.font);
  content.iconSize = effectiveButtonIconSize(radio, fontMetrics);
  content.textBounds = textBoundsForButton(fontMetrics, radio->text());
  content.gap = contentGapForMetrics(metrics);

  if (content.iconSize.isValid() && !content.iconSize.isEmpty()) {
    content.width += content.iconSize.width();
  }

  if (!content.textBounds.isNull() && !content.textBounds.isEmpty()) {
    if (content.width > 0) {
      content.width += content.gap;
    }
    content.width += content.textBounds.width();
  }

  return content;
}

struct InlineContentLayout {
  QRectF iconRect;
  QRectF textRect;
  qreal contentWidth = 0.0;
  qreal contentHeight = 0.0;
};

InlineContentLayout layoutInlineContent(const QRectF& rect, const QSize& iconSize,
                                        const QRect& textBounds, int gap,
                                        Qt::LayoutDirection direction, bool centered) {
  InlineContentLayout layout;

  const bool hasIcon = iconSize.isValid() && !iconSize.isEmpty();
  const bool hasText = !textBounds.isNull() && !textBounds.isEmpty();
  const qreal iconWidth = hasIcon ? static_cast<qreal>(iconSize.width()) : 0.0;
  const qreal iconHeight = hasIcon ? static_cast<qreal>(iconSize.height()) : 0.0;
  const qreal textWidth = hasText ? static_cast<qreal>(textBounds.width()) : 0.0;
  const qreal textHeight = hasText ? static_cast<qreal>(textBounds.height()) : 0.0;

  layout.contentWidth = iconWidth + textWidth;
  if (hasIcon && hasText) {
    layout.contentWidth += static_cast<qreal>(gap);
  }
  layout.contentHeight = std::max(iconHeight, textHeight);

  const qreal startX =
      centered
          ? rect.x() + std::max<qreal>(0.0, (rect.width() - layout.contentWidth) / 2.0)
          : (direction == Qt::RightToLeft ? rect.right() - layout.contentWidth + 1.0 : rect.x());
  const qreal startY =
      rect.y() + std::max<qreal>(0.0, (rect.height() - layout.contentHeight) / 2.0);

  if (direction == Qt::RightToLeft) {
    qreal cursor = startX + layout.contentWidth;
    if (hasIcon) {
      cursor -= iconWidth;
      layout.iconRect =
          QRectF(cursor, startY + std::max<qreal>(0.0, (layout.contentHeight - iconHeight) / 2.0),
                 iconWidth, iconHeight);
    }
    if (hasText) {
      if (hasIcon) {
        cursor -= gap;
      }
      cursor -= textWidth;
      layout.textRect =
          QRectF(cursor, startY + std::max<qreal>(0.0, (layout.contentHeight - textHeight) / 2.0),
                 textWidth, textHeight);
    }
  } else {
    qreal cursor = startX;
    if (hasIcon) {
      layout.iconRect =
          QRectF(cursor, startY + std::max<qreal>(0.0, (layout.contentHeight - iconHeight) / 2.0),
                 iconWidth, iconHeight);
      cursor += iconWidth;
    }
    if (hasText) {
      if (hasIcon) {
        cursor += gap;
      }
      layout.textRect =
          QRectF(cursor, startY + std::max<qreal>(0.0, (layout.contentHeight - textHeight) / 2.0),
                 textWidth, textHeight);
    }
  }

  return layout;
}

QPainterPath roundedRectPath(const QRectF& rect, qreal topLeft, qreal topRight, qreal bottomRight,
                             qreal bottomLeft) {
  const qreal w = std::max(rect.width(), 0.0);
  const qreal h = std::max(rect.height(), 0.0);
  const qreal maxRadius = std::min(w, h) / 2.0;

  topLeft = std::clamp(topLeft, 0.0, maxRadius);
  topRight = std::clamp(topRight, 0.0, maxRadius);
  bottomRight = std::clamp(bottomRight, 0.0, maxRadius);
  bottomLeft = std::clamp(bottomLeft, 0.0, maxRadius);

  const qreal left = rect.left();
  const qreal top = rect.top();
  const qreal right = left + rect.width();
  const qreal bottom = top + rect.height();

  QPainterPath path;
  path.moveTo(left + topLeft, top);
  path.lineTo(right - topRight, top);
  if (topRight > 0.0) {
    path.arcTo(QRectF(right - 2.0 * topRight, top, 2.0 * topRight, 2.0 * topRight), 90.0, -90.0);
  }
  path.lineTo(right, bottom - bottomRight);
  if (bottomRight > 0.0) {
    path.arcTo(QRectF(right - 2.0 * bottomRight, bottom - 2.0 * bottomRight, 2.0 * bottomRight,
                      2.0 * bottomRight),
               0.0, -90.0);
  }
  path.lineTo(left + bottomLeft, bottom);
  if (bottomLeft > 0.0) {
    path.arcTo(QRectF(left, bottom - 2.0 * bottomLeft, 2.0 * bottomLeft, 2.0 * bottomLeft), 270.0,
               -90.0);
  }
  path.lineTo(left, top + topLeft);
  if (topLeft > 0.0) {
    path.arcTo(QRectF(left, top, 2.0 * topLeft, 2.0 * topLeft), 180.0, -90.0);
  }
  path.closeSubpath();
  return path;
}

QRectF defaultRadioIndicatorRect(const QRectF& contentRect, const detail::RadioVisualStyle& style,
                                 qreal labelContentWidth, bool hasLabelContent,
                                 Qt::LayoutDirection direction, bool block) {
  qreal totalWidth = static_cast<qreal>(style.metrics.radioSize);
  if (hasLabelContent) {
    totalWidth += static_cast<qreal>(style.metrics.labelPaddingInlineStart) + labelContentWidth +
                  static_cast<qreal>(style.metrics.labelPaddingInlineEnd);
  }

  const qreal startX =
      block ? contentRect.x() + std::max<qreal>(0.0, (contentRect.width() - totalWidth) / 2.0)
            : (direction == Qt::RightToLeft ? contentRect.right() - totalWidth + 1.0
                                            : contentRect.x());
  const qreal startY =
      contentRect.y() +
      std::max<qreal>(0.0,
                      (contentRect.height() - static_cast<qreal>(style.metrics.radioSize)) / 2.0);

  return QRectF(startX, startY, static_cast<qreal>(style.metrics.radioSize),
                static_cast<qreal>(style.metrics.radioSize));
}

QRectF defaultRadioIndicatorRectForRadio(const AdRadio* radio,
                                         const detail::RadioVisualStyle& style,
                                         const RadioLabelContent& labelContent, bool fill) {
  if (!radio) {
    return QRectF();
  }

  return defaultRadioIndicatorRect(QRectF(radio->rect()), style,
                                   static_cast<qreal>(labelContent.width),
                                   labelContent.hasContent(), radio->layoutDirection(), fill);
}

QRectF defaultRadioLabelRect(const QRectF& contentRect, const QRectF& indicatorRect,
                             const detail::RadioVisualStyle& style, Qt::LayoutDirection direction) {
  if (direction == Qt::RightToLeft) {
    const qreal textLeft =
        contentRect.x() + static_cast<qreal>(style.metrics.labelPaddingInlineEnd);
    const qreal textRight =
        indicatorRect.left() - static_cast<qreal>(style.metrics.labelPaddingInlineStart);
    return QRectF(textLeft, contentRect.y(), std::max<qreal>(0.0, textRight - textLeft),
                  contentRect.height());
  }

  const qreal textX =
      indicatorRect.right() + static_cast<qreal>(style.metrics.labelPaddingInlineStart);
  const qreal textRight =
      contentRect.right() + 1.0 - static_cast<qreal>(style.metrics.labelPaddingInlineEnd);
  return QRectF(textX, contentRect.y(), std::max<qreal>(0.0, textRight - textX),
                contentRect.height());
}

void drawStyledText(const QWidget* widget, QPainter* painter, const QRectF& rect,
                    const QString& text, const QColor& color, Qt::Alignment alignment,
                    QPalette::ColorRole role) {
  if (!widget || !painter || rect.isEmpty() || text.isEmpty()) {
    return;
  }

  QPalette palette = widget->palette();
  palette.setColor(role, color);
  widget->style()->drawItemText(painter, rect.toAlignedRect(),
                                alignment | Qt::TextShowMnemonic | Qt::TextSingleLine, palette,
                                widget->isEnabled(), text, role);
}

void drawButtonIcon(const AdRadio* radio, QPainter* painter, const QRectF& rect,
                    const QSize& iconSize) {
  if (!radio || !painter || rect.isEmpty() || iconSize.isEmpty() || radio->icon().isNull()) {
    return;
  }

  const QIcon::Mode mode = !radio->isEnabled()   ? QIcon::Disabled
                           : radio->isDown()     ? QIcon::Selected
                           : radio->underMouse() ? QIcon::Active
                                                 : QIcon::Normal;
  const QIcon::State state = radio->isChecked() ? QIcon::On : QIcon::Off;
  radio->icon().paint(painter, rect.toAlignedRect(), Qt::AlignCenter, mode, state);
}

detail::RadioButtonStateStyle resolveButtonStateStyle(const detail::RadioButtonVisualStyle& style,
                                                      bool enabled, bool checked, bool hovered,
                                                      bool pressed) {
  if (!enabled) {
    return checked ? style.checkedDisabled : style.disabled;
  }
  if (checked) {
    if (pressed) {
      return style.checkedActive;
    }
    if (hovered) {
      return style.checkedHover;
    }
    return style.checked;
  }
  if (pressed) {
    return style.active;
  }
  if (hovered) {
    return style.hover;
  }
  return style.normal;
}

detail::RadioDotStateStyle resolveDotStateStyle(const detail::RadioVisualStyle& style, bool enabled,
                                                bool checked, bool hovered, bool pressed) {
  if (!enabled) {
    return checked ? style.checkedDisabled : style.disabled;
  }
  if (checked) {
    return (hovered || pressed) ? style.checkedHover : style.checked;
  }
  if (pressed) {
    return style.active;
  }
  if (hovered) {
    return style.hover;
  }
  return style.normal;
}

QRectF widgetRectInWindow(const QWidget* widget, const QWidget* hostWindow) {
  if (!widget || !hostWindow) {
    return QRectF();
  }
  return QRectF(QPointF(widget->mapTo(hostWindow, QPoint(0, 0))), QSizeF(widget->size()));
}

QRectF rectInWindow(const QWidget* widget, const QWidget* hostWindow, const QRectF& rect) {
  if (!widget || !hostWindow) {
    return QRectF();
  }

  const QPoint widgetOriginInWindow = widget->mapTo(hostWindow, QPoint(0, 0));
  return rect.translated(widgetOriginInWindow.x(), widgetOriginInWindow.y());
}

}  // namespace

struct AdRadio::StyleCache {
  std::optional<detail::RadioVisualStyle> radioStyle;
  detail::RadioStyleInput radioInput;
  quint64 radioThemeRevision = 0;
  quint64 radioPaletteKey = 0;

  std::optional<detail::RadioButtonVisualStyle> buttonStyle;
  detail::RadioButtonStyleInput buttonInput;
  quint64 buttonThemeRevision = 0;
  quint64 buttonPaletteKey = 0;
};

AdRadio::AdRadio(QWidget* parent) : QRadioButton(parent) {
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_Hover, true);
  explicitCursorOverride_ = testAttribute(Qt::WA_SetCursor);
  syncManagedSizePolicy();
  refreshAutomaticCursor();

  connect(this, &QRadioButton::toggled, this, [this](bool checked) {
    Q_UNUSED(checked)
    bumpGroupZOrder();
    if (group_) {
      group_->updateButtonStackingOrder();
    }
    updateInteractionFocusOverlay();
    refreshAutomaticCursor();
    update();
  });
}

AdRadio::AdRadio(const QString& text, QWidget* parent) : AdRadio(parent) { setText(text); }

AdRadio::~AdRadio() {
  stopInteractionWaveForOwner(this);
  stopInteractionFocusForOwner(this);
}

AdRadio::ControlSize AdRadio::controlSize() const { return effectiveControlSize(); }

void AdRadio::setControlSize(ControlSize value) {
  if (controlSizeOverride_.has_value() && controlSizeOverride_.value() == value) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  controlSizeOverride_ = value;
  applyEffectiveStateChange(before, true);
}

void AdRadio::resetControlSize() {
  if (!controlSizeOverride_.has_value()) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  controlSizeOverride_.reset();
  applyEffectiveStateChange(before, true);
}

bool AdRadio::hasControlSizeOverride() const { return controlSizeOverride_.has_value(); }

AdRadio::Variant AdRadio::variant() const { return effectiveVariant(); }

void AdRadio::setVariant(Variant value) {
  if (variantOverride_.has_value() && variantOverride_.value() == value) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  variantOverride_ = value;
  applyEffectiveStateChange(before, true);
}

void AdRadio::resetVariant() {
  if (!variantOverride_.has_value()) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  variantOverride_.reset();
  applyEffectiveStateChange(before, true);
}

bool AdRadio::hasVariantOverride() const { return variantOverride_.has_value(); }

AdRadio::ButtonStyle AdRadio::buttonStyle() const { return effectiveButtonStyle(); }

void AdRadio::setButtonStyle(ButtonStyle value) {
  if (buttonStyleOverride_.has_value() && buttonStyleOverride_.value() == value) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  buttonStyleOverride_ = value;
  applyEffectiveStateChange(before, true, false);
}

void AdRadio::resetButtonStyle() {
  if (!buttonStyleOverride_.has_value()) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  buttonStyleOverride_.reset();
  applyEffectiveStateChange(before, true, false);
}

bool AdRadio::hasButtonStyleOverride() const { return buttonStyleOverride_.has_value(); }

AdRadio::ComponentTokens AdRadio::componentTokens() const { return componentTokens_; }

void AdRadio::setComponentTokens(const ComponentTokens& value) {
  if (componentTokensOverride_ && componentTokensEqual(componentTokens_, value)) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  componentTokens_ = value;
  componentTokensOverride_ = true;
  applyEffectiveStateChange(before, true, false);
}

void AdRadio::resetComponentTokens() {
  if (!componentTokensOverride_ && componentTokensEqual(componentTokens_, ComponentTokens{})) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  componentTokens_ = {};
  componentTokensOverride_ = false;
  applyEffectiveStateChange(before, true, false);
}

bool AdRadio::hasComponentTokensOverride() const { return componentTokensOverride_; }

void AdRadio::setComponentTokenResolver(ComponentTokenResolver resolver) {
  const bool hadOverride = componentTokenResolverOverride_;
  const bool hasResolver = static_cast<bool>(resolver);
  if (!hadOverride && !hasResolver) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  componentTokenResolver_ = std::move(resolver);
  componentTokenResolverOverride_ = hasResolver;
  applyEffectiveStateChange(before, true, false);
}

void AdRadio::resetComponentTokenResolver() {
  if (!componentTokenResolverOverride_) {
    return;
  }
  const EffectiveStateSnapshot before = captureEffectiveState(true);
  componentTokenResolver_ = {};
  componentTokenResolverOverride_ = false;
  applyEffectiveStateChange(before, true, false);
}

bool AdRadio::hasComponentTokenResolverOverride() const { return componentTokenResolverOverride_; }

AdRadio::ControlSize AdRadio::effectiveControlSize() const {
  if (controlSizeOverride_.has_value()) {
    return controlSizeOverride_.value();
  }
  return group_ ? group_->controlSize_ : ControlSize::Medium;
}

AdRadio::Variant AdRadio::effectiveVariant() const {
  if (variantOverride_.has_value()) {
    return variantOverride_.value();
  }
  return group_ ? group_->variant_ : Variant::Default;
}

AdRadio::ButtonStyle AdRadio::effectiveButtonStyle() const {
  if (buttonStyleOverride_.has_value()) {
    return buttonStyleOverride_.value();
  }
  return group_ ? group_->buttonStyle_ : ButtonStyle::Outline;
}

bool AdRadio::effectiveFill() const {
  return group_ && group_->distribution_ == AdRadioButtonGroup::Distribution::Fill;
}

AdRadio::ComponentTokenContext AdRadio::currentComponentTokenContext() const {
  ComponentTokenContext state;
  state.controlSize = effectiveControlSize();
  state.variant = effectiveVariant();
  state.buttonStyle = effectiveButtonStyle();
  state.checked = isChecked();
  state.disabled = !isEnabled();
  state.hovered = hovered_;
  state.pressed = pressed_;
  state.focused = hasFocus() && focusVisible_;
  state.block = effectiveFill();
  return state;
}

AdRadio::ComponentTokens AdRadio::resolvedComponentTokens() const {
  ComponentTokens resolved;
  const ComponentTokenContext state = currentComponentTokenContext();
  if (group_) {
    mergeComponentTokens(&resolved, group_->componentTokens_);
    if (group_->componentTokenResolver_) {
      mergeComponentTokens(&resolved, group_->componentTokenResolver_(state));
    }
  }
  mergeComponentTokens(&resolved, componentTokens_);
  if (componentTokenResolver_) {
    mergeComponentTokens(&resolved, componentTokenResolver_(state));
  }
  return resolved;
}

AdRadio::EffectiveStateSnapshot AdRadio::captureEffectiveState(bool includeTokens) const {
  EffectiveStateSnapshot snapshot;
  snapshot.controlSize = controlSize();
  snapshot.variant = variant();
  snapshot.buttonStyle = buttonStyle();
  if (includeTokens) {
    snapshot.resolvedTokens = resolvedComponentTokens();
  }
  return snapshot;
}

void AdRadio::applyEffectiveStateChange(const EffectiveStateSnapshot& before, bool tokensMayChange,
                                        bool updateGeometry) {
  syncManagedSizePolicy();
  if (group_) {
    group_->refreshManagedLayoutState();
  }
  if (effectiveVariant() == Variant::Button && isChecked()) {
    bumpGroupZOrder();
  }
  refreshAfterPropertyChange(updateGeometry);

  const ControlSize nextControlSize = controlSize();
  const Variant nextVariant = variant();
  const ButtonStyle nextButtonStyle = buttonStyle();
  if (before.controlSize != nextControlSize) {
    emit controlSizeChanged(nextControlSize);
  }
  if (before.variant != nextVariant) {
    emit variantChanged(nextVariant);
  }
  if (before.buttonStyle != nextButtonStyle) {
    emit buttonStyleChanged(nextButtonStyle);
  }
  if (tokensMayChange) {
    const ComponentTokens resolved = resolvedComponentTokens();
    if (!componentTokensEqual(before.resolvedTokens, resolved)) {
      emit componentTokensChanged();
    }
  }
}

detail::RadioStyleInput AdRadio::buildStyleInput() const {
  const ComponentTokenContext state = currentComponentTokenContext();
  detail::RadioStyleInput input;
  input.controlSize = state.controlSize;
  input.checked = state.checked;
  input.hovered = state.hovered;
  input.pressed = state.pressed;
  input.focused = state.focused;
  input.baseFont = font();
  input.componentTokens = resolvedComponentTokens();
  return input;
}

detail::RadioButtonStyleInput AdRadio::buildButtonStyleInput() const {
  const ComponentTokenContext state = currentComponentTokenContext();
  detail::RadioButtonStyleInput input;
  input.controlSize = state.controlSize;
  input.buttonStyle = state.buttonStyle;
  input.checked = state.checked;
  input.hovered = state.hovered;
  input.pressed = state.pressed;
  input.focused = state.focused;
  input.baseFont = font();
  input.componentTokens = resolvedComponentTokens();
  return input;
}

const detail::RadioVisualStyle& AdRadio::resolvedRadioStyle() const {
  if (!styleCache_) {
    styleCache_ = std::make_unique<StyleCache>();
  }
  const detail::RadioStyleInput input = buildStyleInput();
  const auto& themeManager = adqt::theme::ThemeManager::instance();
  const quint64 themeRevision = themeManager.themeRevision();
  const quint64 paletteKey = palette().cacheKey();
  if (!styleCache_->radioStyle.has_value() || styleCache_->radioThemeRevision != themeRevision ||
      styleCache_->radioPaletteKey != paletteKey ||
      !radioStyleInputsEqual(styleCache_->radioInput, input)) {
    styleCache_->radioStyle = detail::resolveRadioVisualStyle(input, themeManager.resolve(this));
    styleCache_->radioInput = input;
    styleCache_->radioThemeRevision = themeRevision;
    styleCache_->radioPaletteKey = paletteKey;
  }
  return styleCache_->radioStyle.value();
}

const detail::RadioButtonVisualStyle& AdRadio::resolvedRadioButtonStyle() const {
  if (!styleCache_) {
    styleCache_ = std::make_unique<StyleCache>();
  }
  const detail::RadioButtonStyleInput input = buildButtonStyleInput();
  const auto& themeManager = adqt::theme::ThemeManager::instance();
  const quint64 themeRevision = themeManager.themeRevision();
  const quint64 paletteKey = palette().cacheKey();
  if (!styleCache_->buttonStyle.has_value() || styleCache_->buttonThemeRevision != themeRevision ||
      styleCache_->buttonPaletteKey != paletteKey ||
      !radioButtonStyleInputsEqual(styleCache_->buttonInput, input)) {
    styleCache_->buttonStyle =
        detail::resolveRadioButtonVisualStyle(input, themeManager.resolve(this));
    styleCache_->buttonInput = input;
    styleCache_->buttonThemeRevision = themeRevision;
    styleCache_->buttonPaletteKey = paletteKey;
  }
  return styleCache_->buttonStyle.value();
}

QSize AdRadio::sizeHint() const {
  const auto scaled = [this](const QSize& size) {
    return QSize(qMax(1, qRound(size.width() * controlScale_.logicalScale)),
                 qMax(1, qRound(size.height() * controlScale_.logicalScale)));
  };
  if (effectiveVariant() == Variant::Button) {
    const detail::RadioButtonVisualStyle& style = resolvedRadioButtonStyle();
    const QFontMetrics metrics(style.metrics.font);
    const QSize iconLogicalSize = effectiveButtonIconSize(this, metrics);
    const int contentGap = contentGapForMetrics(style.metrics);
    int width = textWidth(metrics);
    if (iconLogicalSize.isValid() && !iconLogicalSize.isEmpty()) {
      width += iconLogicalSize.width();
      if (!text().isEmpty()) {
        width += contentGap;
      }
    }
    width += style.metrics.buttonPaddingInline * 2 + style.metrics.borderWidth * 2;
    width = std::max(width, 0);
    const int height = std::max(style.metrics.buttonHeight,
                                iconLogicalSize.height() + style.metrics.borderWidth * 2 + 8);
    return scaled(QSize(width, height));
  }

  const detail::RadioVisualStyle& style = resolvedRadioStyle();
  const RadioLabelContent labelContent = measureRadioLabelContent(this, style.metrics);
  int width = style.metrics.radioSize;
  if (labelContent.hasContent()) {
    width += style.metrics.labelPaddingInlineStart + labelContent.width +
             style.metrics.labelPaddingInlineEnd;
  }
  const int height = std::max(
      {style.metrics.radioSize, style.metrics.textLineHeight, labelContent.iconSize.height()});
  return scaled(QSize(width, height));
}

QSize AdRadio::minimumSizeHint() const { return sizeHint(); }

void AdRadio::prepareControlScale(const AdControlScaleContext& context) {
  Q_UNUSED(context)
  styleCache_.reset();
}

void AdRadio::commitControlScale(const AdControlScaleContext& context) {
  if (!referenceFontCaptured_) {
    referenceFont_ = font();
    referenceIconSize_ = iconSize();
    referenceFontCaptured_ = true;
  }
  controlScale_ = context;
  QFont scaledFont = referenceFont_;
  if (scaledFont.pixelSize() > 0) {
    scaledFont.setPixelSize(qMax(1, qRound(scaledFont.pixelSize() * context.logicalScale)));
  } else if (scaledFont.pointSizeF() > 0.0) {
    scaledFont.setPointSizeF(scaledFont.pointSizeF() * context.logicalScale);
  }
  setFont(scaledFont);
  if (referenceIconSize_.isValid()) {
    setIconSize(QSize(qMax(1, qRound(referenceIconSize_.width() * context.logicalScale)),
                      qMax(1, qRound(referenceIconSize_.height() * context.logicalScale))));
  }
  styleCache_.reset();
}

bool AdRadio::event(QEvent* event) {
  const bool handled = QRadioButton::event(event);
  if (!event) {
    return handled;
  }

  if (event->type() == QEvent::Polish || event->type() == QEvent::PolishRequest) {
    const bool explicitCursorOverride = testAttribute(Qt::WA_SetCursor);
    if (explicitCursorOverride_ != explicitCursorOverride) {
      autoCursorManaged_ = false;
      explicitCursorOverride_ = explicitCursorOverride;
      refreshAutomaticCursor();
    }
  }

  if (event->type() == QEvent::CursorChange && !applyingAutoCursor_) {
    autoCursorManaged_ = false;
    explicitCursorOverride_ = testAttribute(Qt::WA_SetCursor);
    refreshAutomaticCursor();
  }
  return handled;
}

void AdRadio::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  if (effectiveVariant() == Variant::Button) {
    paintButtonVariant(&painter);
    return;
  }

  paintDefaultVariant(&painter);
}

void AdRadio::paintButtonVariant(QPainter* painter) const {
  if (!painter) {
    return;
  }

  const detail::RadioButtonVisualStyle& style = resolvedRadioButtonStyle();
  const detail::RadioButtonStateStyle state =
      resolveButtonStateStyle(style, isEnabled(), isChecked(), hovered_, pressed_);

  painter->setFont(style.metrics.font);

  qreal topLeft = 0.0;
  qreal topRight = 0.0;
  qreal bottomRight = 0.0;
  qreal bottomLeft = 0.0;
  resolveButtonCornerRadii(&topLeft, &topRight, &bottomRight, &bottomLeft);

  const QRectF contentRect = QRectF(rect());
  const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
  const QPointF paintOrigin = widgetPaintOrigin(this);
  const qreal borderInset = centeredStrokeInset(style.metrics.borderWidth);

  const bool hasVisibleBorder = style.metrics.borderWidth > 0 && state.borderColor.alpha() > 0;
  const QRectF rawBorderRect =
      contentRect.adjusted(borderInset, borderInset, -borderInset, -borderInset);
  const QRectF borderRect = snapRectToDevicePixels(rawBorderRect, dpr, paintOrigin);
  const QRectF fillRect = hasVisibleBorder ? borderRect : contentRect;
  const QPainterPath fillPath =
      roundedRectPath(fillRect, topLeft, topRight, bottomRight, bottomLeft);
  painter->fillPath(fillPath, state.backgroundColor);

  if (hasVisibleBorder) {
    const QPainterPath borderPath =
        roundedRectPath(borderRect, topLeft, topRight, bottomRight, bottomLeft);
    QPen borderPen(state.borderColor, style.metrics.borderWidth, Qt::SolidLine, Qt::SquareCap,
                   Qt::MiterJoin);
    painter->setPen(borderPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(borderPath);
  }

  const QFontMetrics metrics(style.metrics.font);
  const QSize iconLogicalSize = effectiveButtonIconSize(this, metrics);
  const QRect textBounds = textBoundsForButton(metrics, text());
  const InlineContentLayout contentLayout = layoutInlineContent(
      contentRect.adjusted(style.metrics.buttonPaddingInline, 0, -style.metrics.buttonPaddingInline,
                           0),
      iconLogicalSize, textBounds, contentGapForMetrics(style.metrics), layoutDirection(), true);

  drawButtonIcon(this, painter, contentLayout.iconRect, iconLogicalSize);
  drawStyledText(this, painter, contentLayout.textRect, text(), state.textColor,
                 Qt::AlignVCenter | Qt::AlignLeft, QPalette::ButtonText);
}

void AdRadio::paintDefaultVariant(QPainter* painter) const {
  if (!painter) {
    return;
  }

  const detail::RadioVisualStyle& style = resolvedRadioStyle();
  const detail::RadioDotStateStyle dotState =
      resolveDotStateStyle(style, isEnabled(), isChecked(), hovered_, pressed_);

  painter->setFont(style.metrics.font);

  const QRectF contentRect = QRectF(rect());
  const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
  const QPointF paintOrigin = widgetPaintOrigin(this);
  const RadioLabelContent labelContent = measureRadioLabelContent(this, style.metrics);
  const QRectF indicatorRect =
      defaultRadioIndicatorRectForRadio(this, style, labelContent, effectiveFill());
  const QRectF indicatorBackgroundRect =
      snapRectToDevicePixels(indicatorRect.adjusted(0.5, 0.5, -0.5, -0.5), dpr, paintOrigin);
  const qreal iconStrokeInset = style.metrics.borderWidth / 2.0 + 0.5;
  const QRectF indicatorBorderRect = snapRectToDevicePixels(
      indicatorRect.adjusted(iconStrokeInset, iconStrokeInset, -iconStrokeInset, -iconStrokeInset),
      dpr, paintOrigin);

  painter->setPen(Qt::NoPen);
  painter->setBrush(dotState.backgroundColor);
  painter->drawEllipse(indicatorBackgroundRect);

  if (dotState.borderColor.alpha() > 0 && style.metrics.borderWidth > 0) {
    QPen borderPen(dotState.borderColor, style.metrics.borderWidth, Qt::SolidLine, Qt::RoundCap,
                   Qt::RoundJoin);
    painter->setPen(borderPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(indicatorBorderRect);
  }

  if (isChecked()) {
    const qreal dotSize =
        static_cast<qreal>(std::min(style.metrics.dotSize, style.metrics.radioSize - 4));
    const qreal alignedDotSize = snapToDevicePixelSize(dotSize, dpr);
    const QRectF dotRect = snapRectToDevicePixels(
        centeredSquare(indicatorBackgroundRect.center(), alignedDotSize), dpr, paintOrigin);
    painter->setPen(Qt::NoPen);
    painter->setBrush(dotState.dotColor);
    painter->drawEllipse(dotRect);
  }

  if (labelContent.hasContent()) {
    const QRectF labelRect =
        defaultRadioLabelRect(contentRect, indicatorRect, style, layoutDirection());
    const InlineContentLayout contentLayout =
        layoutInlineContent(labelRect, labelContent.iconSize, labelContent.textBounds,
                            labelContent.gap, layoutDirection(), false);
    drawButtonIcon(this, painter, contentLayout.iconRect, labelContent.iconSize);
    drawStyledText(this, painter, contentLayout.textRect, text(), dotState.labelColor,
                   Qt::AlignVCenter | Qt::AlignLeft, QPalette::WindowText);
  }
}

void AdRadio::enterEvent(QEnterEvent* event) {
  hovered_ = true;
  bumpGroupZOrder();
  update();
  QRadioButton::enterEvent(event);
}

void AdRadio::leaveEvent(QEvent* event) {
  hovered_ = false;
  pressed_ = false;
  update();
  QRadioButton::leaveEvent(event);
}

void AdRadio::mousePressEvent(QMouseEvent* event) {
  if (interactionBlocked()) {
    event->ignore();
    return;
  }
  if (event->button() == Qt::LeftButton) {
    if (AdRadio* target = seamNeighborAt(mouseEventPos(event))) {
      focusVisible_ = false;
      forwardedPressTarget_ = target;
      updateInteractionFocusOverlay();
      if (!target->interactionBlocked()) {
        target->pressed_ = true;
        target->update();
      }
      event->accept();
      return;
    }
    focusVisible_ = false;
    updateInteractionFocusOverlay();
    pressed_ = true;
    bumpGroupZOrder();
    update();
  }
  QRadioButton::mousePressEvent(event);
}

void AdRadio::mouseReleaseEvent(QMouseEvent* event) {
  if (forwardedPressTarget_) {
    AdRadio* const target = forwardedPressTarget_;
    forwardedPressTarget_ = nullptr;

    if (target) {
      const QPoint parentPos = mapToParent(mouseEventPos(event));
      const QPoint targetPos = target->mapFromParent(parentPos);
      const bool triggerTarget = event->button() == Qt::LeftButton &&
                                 target->rect().contains(targetPos) &&
                                 !target->interactionBlocked();
      target->pressed_ = false;
      target->update();
      if (triggerTarget) {
        target->setFocus(Qt::MouseFocusReason);
        target->click();
        target->triggerInteractionWaveOverlay();
      }
    }

    event->accept();
    update();
    return;
  }

  const bool triggerWave = event->button() == Qt::LeftButton && pressed_ &&
                           rect().contains(mouseEventPos(event)) && !interactionBlocked();
  pressed_ = false;
  if (triggerWave) {
    triggerInteractionWaveOverlay();
  }
  QRadioButton::mouseReleaseEvent(event);
  update();
}

void AdRadio::keyPressEvent(QKeyEvent* event) {
  if (event) {
    if (AdRadioButtonGroup* const group = group_;
        group && group->tryHandleNavigation(this, event->key())) {
      event->accept();
      return;
    }
    if (group_ && isArrowKey(event->key())) {
      event->accept();
      return;
    }
  }

  const bool interactiveKey =
      event && (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
                event->key() == Qt::Key_Enter);
  if (!interactionBlocked() && interactiveKey) {
    pressed_ = true;
    bumpGroupZOrder();
    update();
  }
  QRadioButton::keyPressEvent(event);
}

void AdRadio::keyReleaseEvent(QKeyEvent* event) {
  const bool interactiveKey =
      event && (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
                event->key() == Qt::Key_Enter);
  const bool triggerWave = !interactionBlocked() && interactiveKey;
  pressed_ = false;
  if (triggerWave) {
    triggerInteractionWaveOverlay();
  }
  QRadioButton::keyReleaseEvent(event);
  update();
}

bool AdRadio::hitButton(const QPoint& pos) const {
  if (effectiveVariant() == Variant::Button) {
    return rect().contains(pos);
  }
  return QRadioButton::hitButton(pos);
}

void AdRadio::focusInEvent(QFocusEvent* event) {
  focusVisible_ = isKeyboardFocusReason(event->reason());
  bumpGroupZOrder();
  QRadioButton::focusInEvent(event);
  updateInteractionFocusOverlay();
  update();
}

void AdRadio::focusOutEvent(QFocusEvent* event) {
  focusVisible_ = false;
  QRadioButton::focusOutEvent(event);
  stopInteractionFocusForOwner(this);
  update();
}

void AdRadio::moveEvent(QMoveEvent* event) {
  QRadioButton::moveEvent(event);
  updateInteractionFocusOverlay();
}

void AdRadio::resizeEvent(QResizeEvent* event) {
  QRadioButton::resizeEvent(event);
  updateInteractionFocusOverlay();
}

void AdRadio::showEvent(QShowEvent* event) {
  QRadioButton::showEvent(event);
  updateInteractionFocusOverlay();
  if (group_) {
    group_->refreshManagedLayoutState();
  }
}

void AdRadio::hideEvent(QHideEvent* event) {
  QRadioButton::hideEvent(event);
  stopInteractionFocusForOwner(this);
  stopInteractionWaveForOwner(this);
  if (group_) {
    group_->refreshManagedLayoutState();
  }
}

void AdRadio::changeEvent(QEvent* event) {
  QRadioButton::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::EnabledChange:
      if (interactionBlocked()) {
        pressed_ = false;
        stopInteractionFocusForOwner(this);
        stopInteractionWaveForOwner(this);
      }
      refreshAutomaticCursor();
      if (group_) {
        group_->refreshManagedLayoutState();
      }
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

bool AdRadio::interactionBlocked() const { return !isEnabled(); }

int AdRadio::textWidth(const QFontMetrics& metrics) const {
  if (text().isEmpty()) {
    return 0;
  }
  const QRect bounds = textBoundsForButton(metrics, text());
  return std::max(0, bounds.width());
}

int AdRadio::horizontalSpacingHint() const {
  if (effectiveVariant() == Variant::Button) {
    return std::max(0, resolvedRadioButtonStyle().metrics.wrapperMarginInlineEnd);
  }
  return std::max(0, resolvedRadioStyle().metrics.wrapperMarginInlineEnd);
}

int AdRadio::buttonGroupOverlapHint() const {
  const detail::RadioButtonVisualStyle& style = resolvedRadioButtonStyle();
  return collapsedStrokeOverlapPixels(style.metrics.borderWidth);
}

AdRadio* AdRadio::seamNeighborAt(const QPoint& pos) const {
  if (effectiveVariant() != Variant::Button || groupPosition_ == GroupPosition::None ||
      groupPosition_ == GroupPosition::Only) {
    return nullptr;
  }

  const AdRadioButtonGroup* const group = group_;
  if (!group || !group->usesButtonGroupingLayout()) {
    return nullptr;
  }

  const int overlap = group->buttonGroupOverlapPixels();
  if (overlap <= 1) {
    return nullptr;
  }

  AdRadio* candidate = nullptr;
  if (groupVertical_) {
    if (pos.y() < overlap) {
      candidate = group->visibleNeighbor(this, -1);
    } else if (pos.y() >= height() - overlap) {
      candidate = group->visibleNeighbor(this, 1);
    }
  } else {
    const bool rtl = layoutDirection() == Qt::RightToLeft;
    if (pos.x() < overlap) {
      candidate = group->visibleNeighbor(this, rtl ? 1 : -1);
    } else if (pos.x() >= width() - overlap) {
      candidate = group->visibleNeighbor(this, rtl ? -1 : 1);
    }
  }

  if (!candidate || candidate == this || candidate->isHidden() || !candidate->isEnabled()) {
    return nullptr;
  }
  return candidate;
}

void AdRadio::syncManagedSizePolicy() {
  const QSizePolicy::Policy horizontal =
      effectiveFill() ? QSizePolicy::Expanding : QSizePolicy::Preferred;
  if (sizePolicy().horizontalPolicy() == horizontal &&
      sizePolicy().verticalPolicy() == QSizePolicy::Fixed) {
    return;
  }
  setSizePolicy(horizontal, QSizePolicy::Fixed);
}

void AdRadio::refreshAfterPropertyChange(bool updateGeometry) {
  if (updateGeometry) {
    QWidget::updateGeometry();
  }
  refreshAutomaticCursor();
  updateInteractionFocusOverlay();
  update();
}

void AdRadio::refreshAutomaticCursor() {
  if (explicitCursorOverride_) {
    return;
  }
  applyAutomaticCursor(interactionBlocked()
                           ? std::optional<Qt::CursorShape>{}
                           : std::optional<Qt::CursorShape>{Qt::PointingHandCursor});
}

void AdRadio::applyAutomaticCursor(std::optional<Qt::CursorShape> cursorShape) {
  if (explicitCursorOverride_) {
    return;
  }

  if (cursorShape.has_value()) {
    if (!autoCursorManaged_ || !testAttribute(Qt::WA_SetCursor) ||
        cursor().shape() != cursorShape.value()) {
      applyingAutoCursor_ = true;
      QRadioButton::setCursor(QCursor(cursorShape.value()));
      applyingAutoCursor_ = false;
    }
    autoCursorManaged_ = true;
    return;
  }

  if (autoCursorManaged_ || testAttribute(Qt::WA_SetCursor)) {
    applyingAutoCursor_ = true;
    QRadioButton::unsetCursor();
    applyingAutoCursor_ = false;
  }
  autoCursorManaged_ = false;
}

void AdRadio::updateInteractionFocusOverlay() {
  if (!(hasFocus() && focusVisible_) || interactionBlocked() || !isVisible()) {
    stopInteractionFocusForOwner(this);
    return;
  }

  QWidget* hostWindow = window();
  if (!hostWindow) {
    return;
  }

  if (effectiveVariant() == Variant::Button) {
    const detail::RadioButtonVisualStyle& style = resolvedRadioButtonStyle();

    qreal topLeft = 0.0;
    qreal topRight = 0.0;
    qreal bottomRight = 0.0;
    qreal bottomLeft = 0.0;
    resolveButtonCornerRadii(&topLeft, &topRight, &bottomRight, &bottomLeft);

    InteractionFocusRequest request;
    request.owner = this;
    request.baseRectInWindow = widgetRectInWindow(this, hostWindow);
    request.topLeft = topLeft;
    request.topRight = topRight;
    request.bottomRight = bottomRight;
    request.bottomLeft = bottomLeft;
    request.color = style.metrics.focusOutlineColor;
    request.strokeWidth = style.metrics.focusOutlineWidth;
    request.offset = style.metrics.focusOutlineOffset;
    triggerInteractionFocus(request);
    return;
  }

  const detail::RadioVisualStyle& style = resolvedRadioStyle();
  const RadioLabelContent labelContent = measureRadioLabelContent(this, style.metrics);
  const QRectF indicatorRect =
      defaultRadioIndicatorRectForRadio(this, style, labelContent, effectiveFill());

  InteractionFocusRequest request;
  request.owner = this;
  request.baseRectInWindow = rectInWindow(this, hostWindow, indicatorRect);
  const qreal radius = indicatorRect.width() / 2.0;
  request.topLeft = radius;
  request.topRight = radius;
  request.bottomRight = radius;
  request.bottomLeft = radius;
  request.color = style.metrics.focusOutlineColor;
  request.strokeWidth = style.metrics.focusOutlineWidth;
  request.offset = style.metrics.focusOutlineOffset;
  triggerInteractionFocus(request);
}

void AdRadio::triggerInteractionWaveOverlay() {
  if (interactionBlocked() || !isVisible()) {
    return;
  }

  QWidget* hostWindow = window();
  if (!hostWindow) {
    return;
  }

  if (effectiveVariant() == Variant::Button) {
    const detail::RadioButtonVisualStyle& style = resolvedRadioButtonStyle();

    qreal topLeft = 0.0;
    qreal topRight = 0.0;
    qreal bottomRight = 0.0;
    qreal bottomLeft = 0.0;
    resolveButtonCornerRadii(&topLeft, &topRight, &bottomRight, &bottomLeft);

    InteractionWaveRequest request;
    request.owner = this;
    request.baseRectInWindow = widgetRectInWindow(this, hostWindow);
    request.topLeft = topLeft;
    request.topRight = topRight;
    request.bottomRight = bottomRight;
    request.bottomLeft = bottomLeft;
    request.color = style.metrics.waveColor;
    triggerInteractionWave(request);
    return;
  }

  const detail::RadioVisualStyle& style = resolvedRadioStyle();
  const RadioLabelContent labelContent = measureRadioLabelContent(this, style.metrics);
  const QRectF indicatorRect =
      defaultRadioIndicatorRectForRadio(this, style, labelContent, effectiveFill());

  InteractionWaveRequest request;
  request.owner = this;
  request.baseRectInWindow = rectInWindow(this, hostWindow, indicatorRect);
  const qreal radius = indicatorRect.width() / 2.0;
  request.topLeft = radius;
  request.topRight = radius;
  request.bottomRight = radius;
  request.bottomLeft = radius;
  request.color = style.metrics.waveColor;
  request.strokeWidthScale = kDefaultRadioWaveStrokeScale;
  triggerInteractionWave(request);
}

void AdRadio::bumpGroupZOrder() {
  if (effectiveVariant() != Variant::Button || groupPosition_ == GroupPosition::None ||
      !isVisible()) {
    return;
  }
  if (!isChecked() || !isEnabled()) {
    return;
  }
  raise();
}

void AdRadio::resolveButtonCornerRadii(qreal* topLeft, qreal* topRight, qreal* bottomRight,
                                       qreal* bottomLeft) const {
  const detail::RadioButtonVisualStyle& style = resolvedRadioButtonStyle();
  qreal tl = style.metrics.buttonBorderRadius;
  qreal tr = tl;
  qreal br = tl;
  qreal bl = tl;

  if (groupPosition_ != GroupPosition::None && groupPosition_ != GroupPosition::Only) {
    const bool rtl = layoutDirection() == Qt::RightToLeft;
    if (groupVertical_) {
      if (groupPosition_ == GroupPosition::First) {
        bl = 0.0;
        br = 0.0;
      } else if (groupPosition_ == GroupPosition::Middle) {
        tl = 0.0;
        tr = 0.0;
        bl = 0.0;
        br = 0.0;
      } else if (groupPosition_ == GroupPosition::Last) {
        tl = 0.0;
        tr = 0.0;
      }
    } else if (!rtl) {
      if (groupPosition_ == GroupPosition::First) {
        tr = 0.0;
        br = 0.0;
      } else if (groupPosition_ == GroupPosition::Middle) {
        tl = 0.0;
        tr = 0.0;
        bl = 0.0;
        br = 0.0;
      } else if (groupPosition_ == GroupPosition::Last) {
        tl = 0.0;
        bl = 0.0;
      }
    } else {
      if (groupPosition_ == GroupPosition::First) {
        tl = 0.0;
        bl = 0.0;
      } else if (groupPosition_ == GroupPosition::Middle) {
        tl = 0.0;
        tr = 0.0;
        bl = 0.0;
        br = 0.0;
      } else if (groupPosition_ == GroupPosition::Last) {
        tr = 0.0;
        br = 0.0;
      }
    }
  }

  if (topLeft) *topLeft = tl;
  if (topRight) *topRight = tr;
  if (bottomRight) *bottomRight = br;
  if (bottomLeft) *bottomLeft = bl;
}

void AdRadio::setGroupPosition(GroupPosition position) {
  if (groupPosition_ == position) {
    return;
  }
  groupPosition_ = position;
  bumpGroupZOrder();
  updateInteractionFocusOverlay();
  update();
}

void AdRadio::setGroupVertical(bool vertical) {
  if (groupVertical_ == vertical) {
    return;
  }
  groupVertical_ = vertical;
  updateInteractionFocusOverlay();
  update();
}

void AdRadio::setGroup(AdRadioButtonGroup* group) {
  if (group_ == group) {
    return;
  }

  const EffectiveStateSnapshot before = captureEffectiveState(true);
  group_ = group;
  groupPosition_ = GroupPosition::None;
  groupVertical_ = false;
  applyEffectiveStateChange(before, true);
}

}  // namespace adqt::widgets
