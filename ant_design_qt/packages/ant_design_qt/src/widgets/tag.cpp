#include "tag.h"
#include "detail/pointer_region.h"

#include "antd_icons.h"
#include "tag_style.h"
#include "theme/theme.h"

#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyle>

#include <algorithm>
#include <tuple>
#include <utility>

namespace adqt::widgets {

namespace {

bool colorTokensEqual(const AdTag::ColorTokens& lhs, const AdTag::ColorTokens& rhs) {
  return std::tie(lhs.defaultBg, lhs.defaultColor, lhs.defaultBorderColor, lhs.solidTextColor,
                  lhs.closeColor, lhs.closeHoverColor, lhs.closeHoverBackground,
                  lhs.colorTextDisabled, lhs.colorBgContainerDisabled, lhs.colorBorderDisabled,
                  lhs.checkableHoverColor, lhs.checkableHoverBg, lhs.checkableCheckedColor,
                  lhs.checkableCheckedBg, lhs.checkableCheckedHoverBg, lhs.checkableActiveBg,
                  lhs.focusOutlineColor, lhs.waveColor) ==
         std::tie(rhs.defaultBg, rhs.defaultColor, rhs.defaultBorderColor, rhs.solidTextColor,
                  rhs.closeColor, rhs.closeHoverColor, rhs.closeHoverBackground,
                  rhs.colorTextDisabled, rhs.colorBgContainerDisabled, rhs.colorBorderDisabled,
                  rhs.checkableHoverColor, rhs.checkableHoverBg, rhs.checkableCheckedColor,
                  rhs.checkableCheckedBg, rhs.checkableCheckedHoverBg, rhs.checkableActiveBg,
                  rhs.focusOutlineColor, rhs.waveColor);
}

bool metricTokensEqual(const AdTag::MetricTokens& lhs, const AdTag::MetricTokens& rhs) {
  return std::tie(lhs.height, lhs.borderRadius, lhs.borderWidth, lhs.paddingHorizontal,
                  lhs.iconSize, lhs.contentGap, lhs.closeGap, lhs.focusOutlineWidth,
                  lhs.focusOutlineOffset) ==
         std::tie(rhs.height, rhs.borderRadius, rhs.borderWidth, rhs.paddingHorizontal,
                  rhs.iconSize, rhs.contentGap, rhs.closeGap, rhs.focusOutlineWidth,
                  rhs.focusOutlineOffset);
}

bool componentTokensEqual(const AdTag::ComponentTokens& lhs, const AdTag::ComponentTokens& rhs) {
  return colorTokensEqual(lhs.colors, rhs.colors) && metricTokensEqual(lhs.metrics, rhs.metrics);
}

bool semanticSlotStyleEqual(const AdTag::SemanticSlotStyle& lhs,
                            const AdTag::SemanticSlotStyle& rhs) {
  return std::tie(lhs.textColor, lhs.backgroundColor, lhs.borderColor) ==
         std::tie(rhs.textColor, rhs.backgroundColor, rhs.borderColor);
}

bool semanticStylesEqual(const AdTag::SemanticStyles& lhs, const AdTag::SemanticStyles& rhs) {
  return semanticSlotStyleEqual(lhs.root, rhs.root) && semanticSlotStyleEqual(lhs.icon, rhs.icon) &&
         semanticSlotStyleEqual(lhs.content, rhs.content) &&
         semanticSlotStyleEqual(lhs.closeIcon, rhs.closeIcon);
}

template <typename T>
void mergeOptional(std::optional<T>* target, const std::optional<T>& source) {
  if (target && source.has_value()) {
    *target = source;
  }
}

void mergeColorTokens(AdTag::ColorTokens* target, const AdTag::ColorTokens& source) {
  if (!target) {
    return;
  }

  mergeOptional(&target->defaultBg, source.defaultBg);
  mergeOptional(&target->defaultColor, source.defaultColor);
  mergeOptional(&target->defaultBorderColor, source.defaultBorderColor);
  mergeOptional(&target->solidTextColor, source.solidTextColor);
  mergeOptional(&target->closeColor, source.closeColor);
  mergeOptional(&target->closeHoverColor, source.closeHoverColor);
  mergeOptional(&target->closeHoverBackground, source.closeHoverBackground);
  mergeOptional(&target->colorTextDisabled, source.colorTextDisabled);
  mergeOptional(&target->colorBgContainerDisabled, source.colorBgContainerDisabled);
  mergeOptional(&target->colorBorderDisabled, source.colorBorderDisabled);
  mergeOptional(&target->checkableHoverColor, source.checkableHoverColor);
  mergeOptional(&target->checkableHoverBg, source.checkableHoverBg);
  mergeOptional(&target->checkableCheckedColor, source.checkableCheckedColor);
  mergeOptional(&target->checkableCheckedBg, source.checkableCheckedBg);
  mergeOptional(&target->checkableCheckedHoverBg, source.checkableCheckedHoverBg);
  mergeOptional(&target->checkableActiveBg, source.checkableActiveBg);
  mergeOptional(&target->focusOutlineColor, source.focusOutlineColor);
  mergeOptional(&target->waveColor, source.waveColor);
}

void mergeMetricTokens(AdTag::MetricTokens* target, const AdTag::MetricTokens& source) {
  if (!target) {
    return;
  }

  mergeOptional(&target->height, source.height);
  mergeOptional(&target->borderRadius, source.borderRadius);
  mergeOptional(&target->borderWidth, source.borderWidth);
  mergeOptional(&target->paddingHorizontal, source.paddingHorizontal);
  mergeOptional(&target->iconSize, source.iconSize);
  mergeOptional(&target->contentGap, source.contentGap);
  mergeOptional(&target->closeGap, source.closeGap);
  mergeOptional(&target->focusOutlineWidth, source.focusOutlineWidth);
  mergeOptional(&target->focusOutlineOffset, source.focusOutlineOffset);
}

void mergeComponentTokens(AdTag::ComponentTokens* target, const AdTag::ComponentTokens& source) {
  if (!target) {
    return;
  }
  mergeColorTokens(&target->colors, source.colors);
  mergeMetricTokens(&target->metrics, source.metrics);
}

void mergeSemanticSlotStyle(AdTag::SemanticSlotStyle* target,
                            const AdTag::SemanticSlotStyle& source) {
  if (!target) {
    return;
  }
  mergeOptional(&target->textColor, source.textColor);
  mergeOptional(&target->backgroundColor, source.backgroundColor);
  mergeOptional(&target->borderColor, source.borderColor);
}

void mergeSemanticStyles(AdTag::SemanticStyles* target, const AdTag::SemanticStyles& source) {
  if (!target) {
    return;
  }
  mergeSemanticSlotStyle(&target->root, source.root);
  mergeSemanticSlotStyle(&target->icon, source.icon);
  mergeSemanticSlotStyle(&target->content, source.content);
  mergeSemanticSlotStyle(&target->closeIcon, source.closeIcon);
}

bool isKeyboardFocusReason(Qt::FocusReason reason) {
  return reason != Qt::MouseFocusReason && reason != Qt::NoFocusReason;
}

bool isInteractiveKey(int key) {
  return key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter;
}

QPoint mouseEventPos(const QMouseEvent* event) {
  if (!event) {
    return QPoint();
  }
  return event->position().toPoint();
}

int closeButtonBoxSize(const detail::TagVisualStyle& style) {
  return std::max(style.metrics.iconSize + 4, 14);
}

QRect textBounds(const QFontMetrics& metrics, const QString& text) {
  if (text.isEmpty()) {
    return QRect();
  }
  return metrics.boundingRect(QRect(0, 0, 8192, metrics.height() * 2),
                              Qt::TextSingleLine | Qt::TextShowMnemonic, text);
}

QPainterPath roundedRectPath(const QRectF& rect, qreal radius) {
  QPainterPath path;
  path.addRoundedRect(rect, radius, radius);
  return path;
}

}  // namespace

AdTag::AdTag(QWidget* parent) : QAbstractButton(parent) {
  setFocusPolicy(Qt::StrongFocus);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setAttribute(Qt::WA_Hover, true);
  setMouseTracking(true);
  setCursor(Qt::PointingHandCursor);

  connect(this, &QAbstractButton::toggled, this, [this](bool) { refreshAfterStateChange(false); });
}

AdTag::AdTag(const QString& text, QWidget* parent) : AdTag(parent) { setText(text); }

AdTag::~AdTag() = default;

void AdTag::setCheckable(bool value) {
  if (isCheckable() == value) {
    return;
  }
  QAbstractButton::setCheckable(value);
  refreshAfterStateChange();
}

AdTag::Variant AdTag::variant() const { return variant_; }

void AdTag::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  variant_ = value;
  refreshAfterStateChange();
  emit variantChanged(variant_);
}

AdTag::BorderStyle AdTag::borderStyle() const { return borderStyle_; }

void AdTag::setBorderStyle(BorderStyle value) {
  if (borderStyle_ == value) {
    return;
  }
  borderStyle_ = value;
  refreshAfterStateChange();
  emit borderStyleChanged(borderStyle_);
}

AdTag::ColorScheme AdTag::colorScheme() const { return colorScheme_; }

void AdTag::setColorScheme(ColorScheme value) {
  if (colorScheme_ == value) {
    return;
  }
  colorScheme_ = value;
  refreshAfterStateChange();
  emit colorSchemeChanged(colorScheme_);
}

QColor AdTag::customColor() const { return customColor_; }

void AdTag::setCustomColor(const QColor& value) {
  if (customColor_ == value) {
    return;
  }
  customColor_ = value;
  refreshAfterStateChange();
  emit customColorChanged(customColor_);
}

bool AdTag::closable() const { return closable_; }

void AdTag::setClosable(bool value) {
  if (closable_ == value) {
    return;
  }
  closable_ = value;
  refreshAfterStateChange();
  emit closableChanged(closable_);
}

bool AdTag::autoHideOnClose() const { return autoHideOnClose_; }

void AdTag::setAutoHideOnClose(bool value) {
  if (autoHideOnClose_ == value) {
    return;
  }
  autoHideOnClose_ = value;
  update();
  emit autoHideOnCloseChanged(autoHideOnClose_);
}

adqt::icons::IconRef AdTag::iconRef() const { return iconRef_; }

void AdTag::setIconRef(const adqt::icons::IconRef& value) {
  if (iconRef_ == value) {
    return;
  }
  iconRef_ = value;
  refreshAfterStateChange();
  emit iconRefChanged(iconRef_);
}

adqt::icons::IconRef AdTag::closeIconRef() const { return closeIconRef_; }

void AdTag::setCloseIconRef(const adqt::icons::IconRef& value) {
  if (closeIconRef_ == value) {
    return;
  }
  closeIconRef_ = value;
  refreshAfterStateChange();
  emit closeIconRefChanged(closeIconRef_);
}

AdTag::ComponentTokens AdTag::componentTokens() const { return componentTokens_; }

void AdTag::setComponentTokens(const ComponentTokens& value) {
  if (componentTokensEqual(componentTokens_, value)) {
    return;
  }
  componentTokens_ = value;
  refreshAfterStateChange();
  emit componentTokensChanged();
}

void AdTag::resetComponentTokens() { setComponentTokens(ComponentTokens{}); }

void AdTag::setComponentTokenResolver(ComponentTokenResolver resolver) {
  const bool hadResolver = static_cast<bool>(componentTokenResolver_);
  const bool hasResolver = static_cast<bool>(resolver);
  if (!hadResolver && !hasResolver) {
    return;
  }
  componentTokenResolver_ = std::move(resolver);
  refreshAfterStateChange();
  emit componentTokensChanged();
}

void AdTag::resetComponentTokenResolver() { setComponentTokenResolver({}); }

AdTag::SemanticStyles AdTag::semanticStyles() const { return semanticStyles_; }

void AdTag::setSemanticStyles(const SemanticStyles& styles) {
  if (semanticStylesEqual(semanticStyles_, styles)) {
    return;
  }
  semanticStyles_ = styles;
  refreshAfterStateChange(false);
  emit semanticStylesChanged();
}

void AdTag::resetSemanticStyles() { setSemanticStyles(SemanticStyles{}); }

void AdTag::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  const bool hadResolver = static_cast<bool>(semanticStyleResolver_);
  const bool hasResolver = static_cast<bool>(resolver);
  if (!hadResolver && !hasResolver) {
    return;
  }
  semanticStyleResolver_ = std::move(resolver);
  refreshAfterStateChange(false);
  emit semanticStylesChanged();
}

void AdTag::resetSemanticStyleResolver() { setSemanticStyleResolver({}); }

QSize AdTag::sizeHint() const {
  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this);
  detail::TagStyleInput input;
  input.variant = variant_;
  input.borderStyle = borderStyle_;
  input.colorScheme = colorScheme_;
  input.customColor = customColor_;
  input.checkable = isCheckable();
  input.checked = isChecked();
  input.closable = closeButtonVisible();
  input.disabled = !isEnabled();
  input.hovered = detail::widgetHovered(this);
  input.pressed = pressed_;
  input.closeHovered = closeHovered_;
  input.baseFont = font();
  input.componentTokens = resolvedComponentTokens();
  input.semanticStyles = resolvedSemanticStyles();

  const detail::TagVisualStyle style = detail::resolveTagVisualStyle(input, resolvedTheme);
  const QFontMetrics metrics(style.metrics.font);
  const QRect bounds = textBounds(metrics, text());
  const int textWidthValue = std::max(0, bounds.width());
  const int textHeight = std::max(metrics.height(), bounds.height());

  int width = style.metrics.paddingHorizontal * 2 + style.metrics.borderWidth * 2;
  int contentHeight = 0;
  if (adqt::icons::isValid(iconRef_)) {
    width += style.metrics.iconSize;
    contentHeight = std::max(contentHeight, style.metrics.iconSize);
  }
  if (textWidthValue > 0) {
    if (adqt::icons::isValid(iconRef_)) {
      width += style.metrics.contentGap;
    }
    width += textWidthValue;
    contentHeight = std::max(contentHeight, textHeight);
  }
  if (closeButtonVisible()) {
    if (textWidthValue > 0 || adqt::icons::isValid(iconRef_)) {
      width += style.metrics.closeGap;
    }
    width += closeButtonBoxSize(style);
    contentHeight = std::max(contentHeight, closeButtonBoxSize(style));
  }

  const int height =
      std::max(style.metrics.height, contentHeight + style.metrics.borderWidth * 2 + 4);
  return QSize(std::max(width, height / 2), height);
}

QSize AdTag::minimumSizeHint() const { return sizeHint(); }

void AdTag::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this);
  detail::TagStyleInput input;
  input.variant = variant_;
  input.borderStyle = borderStyle_;
  input.colorScheme = colorScheme_;
  input.customColor = customColor_;
  input.checkable = isCheckable();
  input.checked = isChecked();
  input.closable = closeButtonVisible();
  input.disabled = !isEnabled();
  input.hovered = detail::widgetHovered(this);
  input.pressed = pressed_;
  input.closeHovered = closeHovered_;
  input.baseFont = font();
  input.componentTokens = resolvedComponentTokens();
  input.semanticStyles = resolvedSemanticStyles();

  const detail::TagVisualStyle visualStyle = detail::resolveTagVisualStyle(input, resolvedTheme);
  painter.setFont(visualStyle.metrics.font);

  const qreal focusInset =
      (hasFocus() && focusVisible_ && visualStyle.focusOutlineColor.alpha() > 0 &&
       visualStyle.metrics.focusOutlineWidth > 0.0)
          ? visualStyle.metrics.focusOutlineWidth / 2.0 + visualStyle.metrics.focusOutlineOffset
          : 0.0;
  QRectF outerRect = QRectF(rect()).adjusted(focusInset, focusInset, -focusInset, -focusInset);
  if (!outerRect.isValid() || outerRect.width() <= 0.0 || outerRect.height() <= 0.0) {
    outerRect = rect();
  }

  if (hasFocus() && focusVisible_ && visualStyle.focusOutlineColor.alpha() > 0 &&
      visualStyle.metrics.focusOutlineWidth > 0.0) {
    painter.setPen(QPen(visualStyle.focusOutlineColor, visualStyle.metrics.focusOutlineWidth,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(roundedRectPath(outerRect, visualStyle.metrics.borderRadius));
  }

  const qreal bodyInset =
      focusInset +
      (visualStyle.metrics.borderWidth > 0 ? visualStyle.metrics.borderWidth / 2.0 : 0.0) + 0.5;
  const QRectF bodyRect = QRectF(rect()).adjusted(bodyInset, bodyInset, -bodyInset, -bodyInset);
  const QPainterPath bodyPath = roundedRectPath(bodyRect, visualStyle.metrics.borderRadius);

  painter.setPen(Qt::NoPen);
  painter.setBrush(visualStyle.backgroundColor);
  painter.drawPath(bodyPath);

  if (visualStyle.borderColor.alpha() > 0 && visualStyle.metrics.borderWidth > 0) {
    painter.setPen(QPen(visualStyle.borderColor, visualStyle.metrics.borderWidth,
                        visualStyle.borderPenStyle, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(bodyPath);
  }

  const bool rtl = layoutDirection() == Qt::RightToLeft;
  const int padding = visualStyle.metrics.paddingHorizontal;
  const int iconSize = visualStyle.metrics.iconSize;
  const int closeSize = closeButtonVisible() ? closeButtonBoxSize(visualStyle) : 0;
  const QFontMetrics metrics(visualStyle.metrics.font);
  const QRect bounds = textBounds(metrics, text());
  const int textWidthValue = std::max(0, bounds.width());
  const int iconGap =
      adqt::icons::isValid(iconRef_) && textWidthValue > 0 ? visualStyle.metrics.contentGap : 0;
  const int closeGap =
      closeButtonVisible() && (textWidthValue > 0 || adqt::icons::isValid(iconRef_))
          ? visualStyle.metrics.closeGap
          : 0;

  QRectF contentRect = bodyRect.adjusted(padding, 0.0, -padding, 0.0);
  if (closeButtonVisible()) {
    if (rtl) {
      contentRect.adjust(closeSize + closeGap, 0.0, 0.0, 0.0);
    } else {
      contentRect.adjust(0.0, 0.0, -(closeSize + closeGap), 0.0);
    }
  }

  if (closeButtonVisible()) {
    const QRect closeRect = closeButtonRect();
    const bool highlightClose = closeHovered_ || closePressed_;
    if (highlightClose && visualStyle.closeHoverBackground.alpha() > 0) {
      QColor closeBackground = visualStyle.closeHoverBackground;
      if (closePressed_) {
        closeBackground = closeBackground.darker(110);
      }
      painter.setPen(Qt::NoPen);
      painter.setBrush(closeBackground);
      painter.drawRoundedRect(closeRect.adjusted(1, 1, -1, -1), 4.0, 4.0);
    }

    const adqt::icons::IconRef closeIcon =
        adqt::icons::isValid(closeIconRef_) ? closeIconRef_ : adqt::icons::antd::outlined::Close();
    if (adqt::icons::isValid(closeIcon)) {
      const adqt::icons::IconRef coloredClose =
          closeIcon.withColors(adqt::icons::IconColors::primary(closeHovered_ || closePressed_
                                                                    ? visualStyle.closeHoverColor
                                                                    : visualStyle.closeColor));
      adqt::icons::paintIcon(&painter, coloredClose, closeRect.adjusted(2, 2, -2, -2));
    }
  }

  qreal cursor = rtl ? contentRect.right() : contentRect.left();
  QRectF iconRect;
  if (adqt::icons::isValid(iconRef_)) {
    const qreal iconY =
        contentRect.y() + std::max<qreal>(0.0, (contentRect.height() - iconSize) / 2.0);
    if (rtl) {
      iconRect = QRectF(cursor - iconSize + 1.0, iconY, iconSize, iconSize);
      cursor = iconRect.left() - iconGap;
    } else {
      iconRect = QRectF(cursor, iconY, iconSize, iconSize);
      cursor = iconRect.right() + 1.0 + iconGap;
    }

    const adqt::icons::IconRef coloredIcon =
        iconRef_.withColors(adqt::icons::IconColors::primary(visualStyle.iconColor));
    adqt::icons::paintIcon(&painter, coloredIcon, iconRect);
  }

  if (!text().isEmpty()) {
    QRectF textRect;
    const qreal textHeight = metrics.height();
    const qreal textY =
        contentRect.y() + std::max<qreal>(0.0, (contentRect.height() - textHeight) / 2.0);
    if (rtl) {
      textRect = QRectF(contentRect.left(), textY,
                        std::max<qreal>(0.0, cursor - contentRect.left()), textHeight);
    } else {
      textRect = QRectF(cursor, textY, std::max<qreal>(0.0, contentRect.right() - cursor + 1.0),
                        textHeight);
    }

    QPalette palette = this->palette();
    palette.setColor(QPalette::ButtonText, visualStyle.contentColor);
    QWidget::style()->drawItemText(
        &painter, textRect.toAlignedRect(),
        static_cast<int>(Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft) |
                         Qt::TextSingleLine | Qt::TextShowMnemonic),
        palette, isEnabled(), text(), QPalette::ButtonText);
  }
}

bool AdTag::event(QEvent* event) {
  detail::resetWidgetHoverOnLifecycle(this, event);
  return QAbstractButton::event(event);
}

void AdTag::enterEvent(QEnterEvent* event) {
  update();
  QAbstractButton::enterEvent(event);
}

void AdTag::leaveEvent(QEvent* event) {
  pressed_ = false;
  closeHovered_ = false;
  closePressed_ = false;
  update();
  QAbstractButton::leaveEvent(event);
}

void AdTag::mouseMoveEvent(QMouseEvent* event) {
  updateHoverState(mouseEventPos(event));
  QAbstractButton::mouseMoveEvent(event);
}

void AdTag::mousePressEvent(QMouseEvent* event) {
  if (!event || !isEnabled()) {
    if (event) {
      event->ignore();
    }
    return;
  }

  if (event->button() == Qt::LeftButton && closeButtonHit(mouseEventPos(event))) {
    closePressed_ = true;
    closeHovered_ = true;
    pressed_ = false;
    setDown(false);
    update();
    event->accept();
    return;
  }

  if (event->button() == Qt::LeftButton) {
    focusVisible_ = false;
    pressed_ = true;
    updateHoverState(mouseEventPos(event));
    update();
  }
  QAbstractButton::mousePressEvent(event);
}

void AdTag::mouseReleaseEvent(QMouseEvent* event) {
  if (!event || !isEnabled()) {
    if (event) {
      event->ignore();
    }
    return;
  }

  if (closePressed_) {
    const bool triggerClose =
        event->button() == Qt::LeftButton && closeButtonHit(mouseEventPos(event));
    closePressed_ = false;
    updateHoverState(mouseEventPos(event));
    update();
    if (triggerClose) {
      emit closeRequested();
      if (autoHideOnClose_) {
        pendingClosedEmission_ = true;
        hide();
      }
    }
    event->accept();
    return;
  }

  const bool shouldUpdate = pressed_;
  pressed_ = false;
  QAbstractButton::mouseReleaseEvent(event);
  updateHoverState(mouseEventPos(event));
  if (shouldUpdate) {
    update();
  }
}

void AdTag::focusInEvent(QFocusEvent* event) {
  focusVisible_ = event && isKeyboardFocusReason(event->reason());
  QAbstractButton::focusInEvent(event);
  update();
}

void AdTag::focusOutEvent(QFocusEvent* event) {
  focusVisible_ = false;
  pressed_ = false;
  closePressed_ = false;
  QAbstractButton::focusOutEvent(event);
  update();
}

void AdTag::keyPressEvent(QKeyEvent* event) {
  if (event && isEnabled() && isInteractiveKey(event->key())) {
    pressed_ = true;
    update();
  }
  QAbstractButton::keyPressEvent(event);
}

void AdTag::keyReleaseEvent(QKeyEvent* event) {
  const bool hadPressed = pressed_;
  pressed_ = false;
  QAbstractButton::keyReleaseEvent(event);
  if (hadPressed) {
    update();
  }
}

void AdTag::hideEvent(QHideEvent* event) {
  const bool emitClosedSignal = pendingClosedEmission_;
  pendingClosedEmission_ = false;
  closePressed_ = false;
  closeHovered_ = false;
  pressed_ = false;

  QAbstractButton::hideEvent(event);
  if (emitClosedSignal) {
    emit closed();
  }
}

void AdTag::changeEvent(QEvent* event) {
  QAbstractButton::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::EnabledChange:
      if (!isEnabled()) {
        pressed_ = false;
        closePressed_ = false;
        closeHovered_ = false;
      }
      if (isEnabled()) {
        setCursor(Qt::PointingHandCursor);
      } else {
        unsetCursor();
      }
      update();
      break;
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::StyleChange:
      refreshAfterStateChange();
      break;
    default:
      break;
  }
}

bool AdTag::hitButton(const QPoint& pos) const {
  return rect().contains(pos) && !closeButtonHit(pos);
}

AdTag::ComponentTokens AdTag::resolvedComponentTokens() const {
  ComponentTokens resolved;
  mergeComponentTokens(&resolved, componentTokens_);
  if (componentTokenResolver_) {
    mergeComponentTokens(&resolved, componentTokenResolver_(currentComponentTokenContext()));
  }
  return resolved;
}

AdTag::SemanticStyles AdTag::resolvedSemanticStyles() const {
  SemanticStyles resolved;
  mergeSemanticStyles(&resolved, semanticStyles_);
  if (semanticStyleResolver_) {
    mergeSemanticStyles(&resolved, semanticStyleResolver_(currentStyleContext()));
  }
  return resolved;
}

AdTag::ComponentTokenContext AdTag::currentComponentTokenContext() const {
  ComponentTokenContext context;
  context.variant = variant_;
  context.borderStyle = borderStyle_;
  context.colorScheme = colorScheme_;
  context.customColor = customColor_;
  context.checkable = isCheckable();
  context.checked = isChecked();
  context.closable = closable_;
  context.disabled = !isEnabled();
  context.hovered = detail::widgetHovered(this);
  context.pressed = pressed_;
  context.closeHovered = closeHovered_;
  return context;
}

AdTag::StyleContext AdTag::currentStyleContext() const {
  StyleContext context;
  context.variant = variant_;
  context.borderStyle = borderStyle_;
  context.colorScheme = colorScheme_;
  context.customColor = customColor_;
  context.checkable = isCheckable();
  context.checked = isChecked();
  context.closable = closable_;
  context.disabled = !isEnabled();
  context.hovered = detail::widgetHovered(this);
  context.pressed = pressed_;
  context.closeHovered = closeHovered_;
  return context;
}

QRect AdTag::closeButtonRect() const {
  if (!closeButtonVisible()) {
    return QRect();
  }

  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this);
  detail::TagStyleInput input;
  input.variant = variant_;
  input.borderStyle = borderStyle_;
  input.colorScheme = colorScheme_;
  input.customColor = customColor_;
  input.checkable = isCheckable();
  input.checked = isChecked();
  input.closable = true;
  input.disabled = !isEnabled();
  input.hovered = detail::widgetHovered(this);
  input.pressed = pressed_;
  input.closeHovered = closeHovered_;
  input.baseFont = font();
  input.componentTokens = resolvedComponentTokens();
  input.semanticStyles = resolvedSemanticStyles();

  const detail::TagVisualStyle style = detail::resolveTagVisualStyle(input, resolvedTheme);
  const int side = closeButtonBoxSize(style);
  const qreal borderInset = style.metrics.borderWidth + 1.0;
  const QRectF bodyRect =
      QRectF(rect()).adjusted(borderInset, borderInset, -borderInset, -borderInset);
  const int x = layoutDirection() == Qt::RightToLeft
                    ? qRound(bodyRect.left()) + style.metrics.paddingHorizontal
                    : qRound(bodyRect.right()) - style.metrics.paddingHorizontal - side + 1;
  const int y = qRound(bodyRect.y() + std::max<qreal>(0.0, (bodyRect.height() - side) / 2.0));
  return QRect(x, y, side, side);
}

bool AdTag::closeButtonVisible() const { return closable_ && !isCheckable(); }

bool AdTag::closeButtonHit(const QPoint& pos) const {
  return closeButtonVisible() && closeButtonRect().contains(pos);
}

void AdTag::refreshAfterStateChange(bool updateGeometry) {
  if (updateGeometry) {
    QWidget::updateGeometry();
  }
  update();
}

void AdTag::updateHoverState(const QPoint& pos) {
  const bool nextCloseHovered = closeButtonHit(pos);
  if (closeHovered_ == nextCloseHovered) {
    return;
  }
  closeHovered_ = nextCloseHovered;
  update();
}

}  // namespace adqt::widgets
