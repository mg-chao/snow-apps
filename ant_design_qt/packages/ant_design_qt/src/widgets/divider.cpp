#include "divider.h"

#include "detail/button_rendering.h"
#include "divider_style.h"
#include "theme/theme.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QSizePolicy>

#include <algorithm>
#include <utility>

namespace adqt::widgets {

namespace {

class AdDividerAccessible final : public QAccessibleWidget {
 public:
  explicit AdDividerAccessible(AdDivider* divider)
      : QAccessibleWidget(divider, QAccessible::Separator) {}
};

QAccessibleInterface* dividerAccessibleFactory(const QString& className, QObject* object) {
  Q_UNUSED(className)
  if (auto* divider = qobject_cast<AdDivider*>(object)) {
    return new AdDividerAccessible(divider);
  }
  return nullptr;
}

void ensureDividerAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(dividerAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

template <typename T>
bool optionalEqual(const std::optional<T>& lhs, const std::optional<T>& rhs) {
  return lhs == rhs;
}

bool colorTokensEqual(const AdDivider::ColorTokens& lhs, const AdDivider::ColorTokens& rhs) {
  return optionalEqual(lhs.splitColor, rhs.splitColor) &&
         optionalEqual(lhs.textColor, rhs.textColor) &&
         optionalEqual(lhs.headingTextColor, rhs.headingTextColor);
}

bool metricTokensEqual(const AdDivider::MetricTokens& lhs, const AdDivider::MetricTokens& rhs) {
  return optionalEqual(lhs.lineWidth, rhs.lineWidth) &&
         optionalEqual(lhs.textPaddingInline, rhs.textPaddingInline) &&
         optionalEqual(lhs.orientationMargin, rhs.orientationMargin) &&
         optionalEqual(lhs.verticalMarginInline, rhs.verticalMarginInline) &&
         optionalEqual(lhs.horizontalMarginSmall, rhs.horizontalMarginSmall) &&
         optionalEqual(lhs.horizontalMarginMiddle, rhs.horizontalMarginMiddle) &&
         optionalEqual(lhs.horizontalMarginLarge, rhs.horizontalMarginLarge) &&
         optionalEqual(lhs.horizontalMarginWithText, rhs.horizontalMarginWithText) &&
         optionalEqual(lhs.verticalHeightFactor, rhs.verticalHeightFactor);
}

bool componentTokensEqual(const AdDivider::ComponentTokens& lhs,
                          const AdDivider::ComponentTokens& rhs) {
  return colorTokensEqual(lhs.colors, rhs.colors) && metricTokensEqual(lhs.metrics, rhs.metrics);
}

bool semanticSlotEqual(const AdDivider::SemanticSlotStyle& lhs,
                       const AdDivider::SemanticSlotStyle& rhs) {
  return optionalEqual(lhs.textColor, rhs.textColor) &&
         optionalEqual(lhs.backgroundColor, rhs.backgroundColor) &&
         optionalEqual(lhs.borderColor, rhs.borderColor) &&
         optionalEqual(lhs.borderWidth, rhs.borderWidth) && optionalEqual(lhs.font, rhs.font) &&
         optionalEqual(lhs.marginStart, rhs.marginStart) &&
         optionalEqual(lhs.marginEnd, rhs.marginEnd);
}

bool semanticStylesEqual(const AdDivider::SemanticStyles& lhs,
                         const AdDivider::SemanticStyles& rhs) {
  return semanticSlotEqual(lhs.root, rhs.root) && semanticSlotEqual(lhs.rail, rhs.rail) &&
         semanticSlotEqual(lhs.content, rhs.content);
}

void mergeColorTokens(AdDivider::ColorTokens* target, const AdDivider::ColorTokens& source) {
  if (!target) {
    return;
  }
  if (source.splitColor.has_value()) target->splitColor = source.splitColor;
  if (source.textColor.has_value()) target->textColor = source.textColor;
  if (source.headingTextColor.has_value()) target->headingTextColor = source.headingTextColor;
}

void mergeMetricTokens(AdDivider::MetricTokens* target, const AdDivider::MetricTokens& source) {
  if (!target) {
    return;
  }
  if (source.lineWidth.has_value()) target->lineWidth = source.lineWidth;
  if (source.textPaddingInline.has_value()) {
    target->textPaddingInline = source.textPaddingInline;
  }
  if (source.orientationMargin.has_value()) target->orientationMargin = source.orientationMargin;
  if (source.verticalMarginInline.has_value()) {
    target->verticalMarginInline = source.verticalMarginInline;
  }
  if (source.horizontalMarginSmall.has_value()) {
    target->horizontalMarginSmall = source.horizontalMarginSmall;
  }
  if (source.horizontalMarginMiddle.has_value()) {
    target->horizontalMarginMiddle = source.horizontalMarginMiddle;
  }
  if (source.horizontalMarginLarge.has_value()) {
    target->horizontalMarginLarge = source.horizontalMarginLarge;
  }
  if (source.horizontalMarginWithText.has_value()) {
    target->horizontalMarginWithText = source.horizontalMarginWithText;
  }
  if (source.verticalHeightFactor.has_value()) {
    target->verticalHeightFactor = source.verticalHeightFactor;
  }
}

void mergeComponentTokens(AdDivider::ComponentTokens* target,
                          const AdDivider::ComponentTokens& source) {
  if (!target) {
    return;
  }
  mergeColorTokens(&target->colors, source.colors);
  mergeMetricTokens(&target->metrics, source.metrics);
}

void mergeSemanticSlot(AdDivider::SemanticSlotStyle* target,
                       const AdDivider::SemanticSlotStyle& source) {
  if (!target) {
    return;
  }
  if (source.textColor.has_value()) target->textColor = source.textColor;
  if (source.backgroundColor.has_value()) target->backgroundColor = source.backgroundColor;
  if (source.borderColor.has_value()) target->borderColor = source.borderColor;
  if (source.borderWidth.has_value()) target->borderWidth = source.borderWidth;
  if (source.font.has_value()) target->font = source.font;
  if (source.marginStart.has_value()) target->marginStart = source.marginStart;
  if (source.marginEnd.has_value()) target->marginEnd = source.marginEnd;
}

void mergeSemanticStyles(AdDivider::SemanticStyles* target,
                         const AdDivider::SemanticStyles& source) {
  if (!target) {
    return;
  }
  mergeSemanticSlot(&target->root, source.root);
  mergeSemanticSlot(&target->rail, source.rail);
  mergeSemanticSlot(&target->content, source.content);
}

bool usesCustomPalette(const QWidget* widget) {
  if (!widget) {
    return false;
  }
  const auto& themeManager = adqt::theme::ThemeManager::instance();
  for (const QWidget* current = widget; current; current = current->parentWidget()) {
    if (current->testAttribute(Qt::WA_SetPalette) &&
        current->palette() != themeManager.resolve(current).palette) {
      return true;
    }
  }
  return false;
}

detail::DividerStyleInput styleInputFor(const AdDivider* divider) {
  detail::DividerStyleInput input;
  if (!divider) {
    return input;
  }
  input.orientation = divider->orientation();
  input.size = divider->dividerSize();
  input.titlePlacement = divider->titlePlacement();
  input.variant = divider->variant();
  input.plain = divider->plain();
  input.hasContent = divider->contentWidget() || !divider->text().isEmpty();
  input.enabled = divider->isEnabled();
  input.baseFont = divider->font();
  input.componentTokens = divider->componentTokens();
  input.semanticStyles = divider->semanticStyles();
  return input;
}

int horizontalMargin(const detail::DividerAppearance& appearance, AdDivider::Size size,
                     bool hasContent) {
  if (hasContent) {
    if (size == AdDivider::Size::Small) {
      return appearance.metrics.horizontalMarginSmall;
    }
    return appearance.metrics.horizontalMarginWithText;
  }
  switch (size) {
    case AdDivider::Size::Small:
      return appearance.metrics.horizontalMarginSmall;
    case AdDivider::Size::Middle:
      return appearance.metrics.horizontalMarginMiddle;
    case AdDivider::Size::Large:
      return appearance.metrics.horizontalMarginLarge;
  }
  return appearance.metrics.horizontalMarginLarge;
}

struct ContentLayout {
  QRect outerRect;
  QRect contentRect;
  QRect innerRect;
};

ContentLayout calculateContentLayout(const AdDivider* divider,
                                     const detail::DividerAppearance& appearance) {
  ContentLayout result;
  if (!divider || divider->orientation() != AdDivider::Orientation::Horizontal ||
      (!divider->contentWidget() && divider->text().isEmpty()) || divider->width() <= 0 ||
      divider->height() <= 0) {
    return result;
  }

  const bool customContent = divider->contentWidget() != nullptr;
  const QFontMetrics fontMetrics(appearance.metrics.contentFont);
  const QSize desiredInner = customContent ? divider->contentWidget()->sizeHint().expandedTo(
                                                 divider->contentWidget()->minimumSizeHint())
                                           : QSize(fontMetrics.horizontalAdvance(divider->text()),
                                                   appearance.metrics.textLineHeight);
  const int padding = appearance.metrics.textPaddingInline;
  const int contentMarginStart = appearance.metrics.contentMarginStart;
  const int contentMarginEnd = appearance.metrics.contentMarginEnd;
  const int desiredContentWidth = std::max(0, desiredInner.width()) + padding * 2;
  const int desiredOuterWidth = desiredContentWidth + contentMarginStart + contentMarginEnd;
  const int outerWidth = std::min(divider->width(), desiredOuterWidth);
  const int edgeRail = std::clamp(qRound(divider->width() * appearance.metrics.orientationMargin),
                                  0, std::max(0, divider->width() - outerWidth));

  int outerX = 0;
  switch (divider->titlePlacement()) {
    case AdDivider::TitlePlacement::Center:
      outerX = (divider->width() - outerWidth) / 2;
      break;
    case AdDivider::TitlePlacement::Start:
      outerX = divider->layoutDirection() == Qt::LeftToRight
                   ? edgeRail
                   : divider->width() - edgeRail - outerWidth;
      break;
    case AdDivider::TitlePlacement::End:
      outerX = divider->layoutDirection() == Qt::LeftToRight
                   ? divider->width() - edgeRail - outerWidth
                   : edgeRail;
      break;
    case AdDivider::TitlePlacement::Left:
      outerX = edgeRail;
      break;
    case AdDivider::TitlePlacement::Right:
      outerX = divider->width() - edgeRail - outerWidth;
      break;
  }

  const int desiredContentHeight =
      customContent ? std::max(0, desiredInner.height()) : appearance.metrics.textLineHeight;
  const int contentHeight = std::min(divider->height(), desiredContentHeight);
  const int contentY = (divider->height() - contentHeight) / 2;
  result.outerRect = QRect(outerX, contentY, outerWidth, contentHeight);

  const bool rtl = divider->layoutDirection() == Qt::RightToLeft;
  const int physicalLeftMargin = rtl ? contentMarginEnd : contentMarginStart;
  const int physicalRightMargin = rtl ? contentMarginStart : contentMarginEnd;
  result.contentRect = result.outerRect.adjusted(std::min(physicalLeftMargin, outerWidth), 0,
                                                 -std::min(physicalRightMargin, outerWidth), 0);
  if (result.contentRect.width() < 0) {
    result.contentRect.setWidth(0);
  }
  const int effectivePadding = std::min(padding, result.contentRect.width() / 2);
  result.innerRect = result.contentRect.adjusted(effectivePadding, 0, -effectivePadding, 0);
  return result;
}

Qt::PenStyle penStyleFor(AdDivider::Variant variant) {
  switch (variant) {
    case AdDivider::Variant::Dashed:
      return Qt::DashLine;
    case AdDivider::Variant::Dotted:
      return Qt::DotLine;
    case AdDivider::Variant::Solid:
      return Qt::SolidLine;
  }
  return Qt::SolidLine;
}

}  // namespace

AdDivider::AdDivider(QWidget* parent) : QFrame(parent) {
  ensureDividerAccessibleFactoryInstalled();
  setFocusPolicy(Qt::NoFocus);
  setAttribute(Qt::WA_OpaquePaintEvent, false);
  updateFrameAndSizePolicy();
  updateAccessibleText();

  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { refreshAfterStateChange(); });
}

AdDivider::AdDivider(const QString& text, QWidget* parent) : AdDivider(parent) { setText(text); }

AdDivider::~AdDivider() = default;

AdDivider::Orientation AdDivider::orientation() const { return orientation_; }

void AdDivider::setOrientation(Orientation value) {
  if (orientation_ == value) {
    return;
  }
  orientation_ = value;
  updateFrameAndSizePolicy();
  refreshAfterStateChange();
  emit orientationChanged(orientation_);
  emit typeChanged(orientation_);
  emit verticalChanged(vertical());
}

AdDivider::Orientation AdDivider::type() const { return orientation_; }

void AdDivider::setType(Orientation value) { setOrientation(value); }

bool AdDivider::vertical() const { return orientation_ == Orientation::Vertical; }

void AdDivider::setVertical(bool value) {
  setOrientation(value ? Orientation::Vertical : Orientation::Horizontal);
}

qreal AdDivider::orientationMargin() const {
  const qreal value = componentTokens_.metrics.orientationMargin.value_or(0.05);
  return qIsFinite(value) ? std::clamp<qreal>(value, 0.0, 1.0) : 0.05;
}

void AdDivider::setOrientationMargin(qreal value) {
  const qreal next = qIsFinite(value) ? std::clamp<qreal>(value, 0.0, 1.0) : 0.05;
  if (qFuzzyCompare(orientationMargin() + 1.0, next + 1.0)) {
    return;
  }
  componentTokens_.metrics.orientationMargin = next;
  refreshAfterStateChange();
  emit orientationMarginChanged(next);
  emit componentTokensChanged();
}

AdDivider::Size AdDivider::dividerSize() const { return size_; }

void AdDivider::setDividerSize(Size value) {
  if (size_ == value) {
    return;
  }
  size_ = value;
  refreshAfterStateChange();
  emit sizeChanged(size_);
}

void AdDivider::setSize(Size value) { setDividerSize(value); }

AdDivider::TitlePlacement AdDivider::titlePlacement() const { return titlePlacement_; }

void AdDivider::setTitlePlacement(TitlePlacement value) {
  if (titlePlacement_ == value) {
    return;
  }
  titlePlacement_ = value;
  refreshAfterStateChange(false);
  emit titlePlacementChanged(titlePlacement_);
}

AdDivider::Variant AdDivider::variant() const { return variant_; }

void AdDivider::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  const bool wasDashed = dashed();
  variant_ = value;
  refreshAfterStateChange(false);
  emit variantChanged(variant_);
  if (wasDashed != dashed()) {
    emit dashedChanged(dashed());
  }
}

bool AdDivider::dashed() const { return variant_ == Variant::Dashed; }

void AdDivider::setDashed(bool value) {
  if (value) {
    setVariant(Variant::Dashed);
  } else if (dashed()) {
    setVariant(Variant::Solid);
  }
}

bool AdDivider::plain() const { return plain_; }

void AdDivider::setPlain(bool value) {
  if (plain_ == value) {
    return;
  }
  plain_ = value;
  refreshAfterStateChange();
  emit plainChanged(plain_);
}

QString AdDivider::text() const { return text_; }

void AdDivider::setText(const QString& value) {
  if (text_ == value) {
    return;
  }
  text_ = value;
  updateAccessibleText();
  refreshAfterStateChange();
  emit textChanged(text_);
}

QWidget* AdDivider::contentWidget() const { return contentWidget_.data(); }

void AdDivider::setContentWidget(QWidget* widget) {
  if (contentWidget_ == widget) {
    return;
  }

  // A divider cannot host itself or one of its ancestors without creating a
  // QWidget parent cycle. Treat those inputs as invalid and leave the current
  // content untouched.
  if (widget == this || (widget && widget->isAncestorOf(this))) {
    return;
  }

  clearContentWidget(true);
  attachContentWidget(widget);
  updateAccessibleText();
  refreshAfterStateChange();
  emit contentWidgetChanged(contentWidget_.data());
}

QWidget* AdDivider::takeContentWidget() {
  QWidget* widget = contentWidget_.data();
  if (!widget) {
    return nullptr;
  }

  clearContentWidget(false);
  updateAccessibleText();
  refreshAfterStateChange();
  emit contentWidgetChanged(nullptr);
  return widget;
}

AdDivider::ComponentTokens AdDivider::componentTokens() const { return componentTokens_; }

void AdDivider::setComponentTokens(const ComponentTokens& value) {
  if (componentTokensEqual(componentTokens_, value)) {
    return;
  }
  const qreal previousOrientationMargin = orientationMargin();
  componentTokens_ = value;
  refreshAfterStateChange();
  emit componentTokensChanged();
  if (!qFuzzyCompare(previousOrientationMargin + 1.0, orientationMargin() + 1.0)) {
    emit orientationMarginChanged(orientationMargin());
  }
}

void AdDivider::resetComponentTokens() { setComponentTokens(ComponentTokens{}); }

void AdDivider::setComponentTokenResolver(ComponentTokenResolver resolver) {
  const bool hadResolver = static_cast<bool>(componentTokenResolver_);
  const bool hasResolver = static_cast<bool>(resolver);
  if (!hadResolver && !hasResolver) {
    return;
  }
  componentTokenResolver_ = std::move(resolver);
  refreshAfterStateChange();
  emit componentTokensChanged();
}

void AdDivider::resetComponentTokenResolver() { setComponentTokenResolver({}); }

AdDivider::SemanticStyles AdDivider::semanticStyles() const { return semanticStyles_; }

void AdDivider::setSemanticStyles(const SemanticStyles& value) {
  if (semanticStylesEqual(semanticStyles_, value)) {
    return;
  }
  semanticStyles_ = value;
  refreshAfterStateChange();
  emit semanticStylesChanged();
}

void AdDivider::resetSemanticStyles() { setSemanticStyles(SemanticStyles{}); }

void AdDivider::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  const bool hadResolver = static_cast<bool>(semanticStyleResolver_);
  const bool hasResolver = static_cast<bool>(resolver);
  if (!hadResolver && !hasResolver) {
    return;
  }
  semanticStyleResolver_ = std::move(resolver);
  refreshAfterStateChange();
  emit semanticStylesChanged();
}

void AdDivider::resetSemanticStyleResolver() { setSemanticStyleResolver({}); }

QSize AdDivider::sizeHint() const {
  const detail::DividerAppearance appearance = resolvedAppearance();

  if (orientation_ == Orientation::Vertical) {
    const int width = appearance.metrics.verticalMarginInline * 2 +
                      std::max(1, qCeil(appearance.metrics.lineWidth));
    return QSize(width, appearance.metrics.verticalHeight);
  }

  int contentHeight = std::max(1, qCeil(appearance.metrics.lineWidth));
  int contentWidth = 0;
  if (hasContent()) {
    if (contentWidget_) {
      const QSize childSize =
          contentWidget_->sizeHint().expandedTo(contentWidget_->minimumSizeHint());
      contentWidth = std::max(0, childSize.width());
      contentHeight = std::max(contentHeight, std::max(0, childSize.height()));
    } else {
      const QFontMetrics metrics(appearance.metrics.contentFont);
      contentWidth = metrics.horizontalAdvance(text_);
      contentHeight = std::max(contentHeight, appearance.metrics.textLineHeight);
    }
    contentWidth += appearance.metrics.textPaddingInline * 2 +
                    appearance.metrics.contentMarginStart + appearance.metrics.contentMarginEnd;
  }
  const int margin = horizontalMargin(appearance, size_, hasContent());
  const int width =
      std::max(240, contentWidth + qRound(appearance.metrics.orientationMargin * 480.0));
  return QSize(width, contentHeight + margin * 2);
}

QSize AdDivider::minimumSizeHint() const {
  const QSize preferred = sizeHint();
  return orientation_ == Orientation::Horizontal ? QSize(0, preferred.height()) : preferred;
}

void AdDivider::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)

  const detail::DividerAppearance appearance = resolvedAppearance();

  QPainter painter(this);
  if (appearance.rootBackground.alpha() > 0) {
    painter.fillRect(rect(), appearance.rootBackground);
  }
  const bool railVisible = appearance.metrics.lineWidth > 0.0 && appearance.lineColor.alpha() > 0;
  const qreal devicePixelRatio =
      painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
  const qreal railWidth =
      detail::deviceAlignedPenWidth(appearance.metrics.lineWidth, devicePixelRatio);
  const auto setRailPen = [&]() {
    QPen pen = detail::makeButtonBorderPen(appearance.lineColor, railWidth, penStyleFor(variant_));
    pen.setCapStyle(variant_ == Variant::Dotted ? Qt::RoundCap : Qt::FlatCap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(pen);
  };
  const auto alignedStrokeCenter = [&](qreal coordinate, bool vertical) {
    // Include backing-store and parent translations before snapping to the Qt raster phase.
    const QTransform deviceTransform = painter.deviceTransform();
    bool invertible = false;
    const QTransform inverseTransform = deviceTransform.inverted(&invertible);
    if (!invertible) {
      return detail::deviceAlignedStrokeCenter(coordinate, railWidth, devicePixelRatio);
    }

    QPointF devicePoint =
        deviceTransform.map(vertical ? QPointF(coordinate, 0.0) : QPointF(0.0, coordinate));
    const int physicalWidth = std::max(1, qRound(railWidth * devicePixelRatio));
    const qreal phase = (physicalWidth % 2 == 0) ? 0.5 : 0.0;
    if (vertical) {
      devicePoint.setX(qRound(devicePoint.x() - phase) + phase);
    } else {
      devicePoint.setY(qRound(devicePoint.y() - phase) + phase);
    }
    const QPointF alignedPoint = inverseTransform.map(devicePoint);
    return vertical ? alignedPoint.x() : alignedPoint.y();
  };

  if (orientation_ == Orientation::Vertical) {
    if (railVisible) {
      setRailPen();
      const qreal x = alignedStrokeCenter((width() - 1) / 2.0, true);
      painter.drawLine(QPointF(x, 0.0), QPointF(x, std::max(0, height() - 1)));
    }
    return;
  }

  const ContentLayout contentLayout = calculateContentLayout(this, appearance);
  if (!hasContent()) {
    if (railVisible) {
      setRailPen();
      const qreal y = alignedStrokeCenter((height() - 1) / 2.0, false);
      painter.drawLine(QPointF(0.0, y), QPointF(std::max(0, width() - 1), y));
    }
    return;
  }

  if (railVisible) {
    setRailPen();
    const qreal y = alignedStrokeCenter((height() - 1) / 2.0, false);
    if (contentLayout.outerRect.left() > 0) {
      painter.drawLine(QPointF(0.0, y), QPointF(contentLayout.outerRect.left(), y));
    }
    if (contentLayout.outerRect.right() < width() - 1) {
      painter.drawLine(QPointF(contentLayout.outerRect.right(), y),
                       QPointF(std::max(0, width() - 1), y));
    }
  }

  if (appearance.contentBackground.alpha() > 0 && contentLayout.contentRect.isValid()) {
    painter.fillRect(contentLayout.contentRect, appearance.contentBackground);
  }
  if (!contentWidget_ && !text_.isEmpty() && contentLayout.innerRect.isValid()) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(appearance.metrics.contentFont);
    painter.setPen(appearance.textColor);
    const QFontMetrics metrics(appearance.metrics.contentFont);
    const QString renderedText =
        metrics.elidedText(text_, Qt::ElideRight, std::max(0, contentLayout.innerRect.width()));
    painter.drawText(contentLayout.innerRect, Qt::AlignCenter | Qt::TextSingleLine, renderedText);
  }
}

void AdDivider::resizeEvent(QResizeEvent* event) {
  QFrame::resizeEvent(event);
  updateContentWidgetGeometry();
}

void AdDivider::changeEvent(QEvent* event) {
  QFrame::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::StyleChange:
    case QEvent::EnabledChange:
    case QEvent::ParentChange:
      refreshAfterStateChange();
      break;
    default:
      break;
  }
}

bool AdDivider::hasContent() const { return contentWidget_ || !text_.isEmpty(); }

AdDivider::StyleContext AdDivider::currentStyleContext() const {
  StyleContext context;
  context.orientation = orientation_;
  context.size = size_;
  context.titlePlacement = titlePlacement_;
  context.variant = variant_;
  context.plain = plain_;
  context.hasContent = hasContent();
  context.enabled = isEnabled();
  return context;
}

AdDivider::ComponentTokens AdDivider::resolvedComponentTokens() const {
  ComponentTokens resolved;
  mergeComponentTokens(&resolved, componentTokens_);
  if (componentTokenResolver_) {
    mergeComponentTokens(&resolved, componentTokenResolver_(currentStyleContext()));
  }
  return resolved;
}

AdDivider::SemanticStyles AdDivider::resolvedSemanticStyles() const {
  SemanticStyles resolved;
  mergeSemanticStyles(&resolved, semanticStyles_);
  if (semanticStyleResolver_) {
    mergeSemanticStyles(&resolved, semanticStyleResolver_(currentStyleContext()));
  }
  return resolved;
}

detail::DividerAppearance AdDivider::resolvedAppearance() const {
  detail::DividerStyleInput input = styleInputFor(this);
  input.componentTokens = resolvedComponentTokens();
  input.semanticStyles = resolvedSemanticStyles();
  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this);
  input.hasPaletteOverride = usesCustomPalette(this);
  input.paletteGroup = isEnabled() ? palette().currentColorGroup() : QPalette::Disabled;
  input.palette = palette();
  return detail::resolveDividerAppearance(input, resolvedTheme);
}

void AdDivider::refreshAfterStateChange(bool geometryChanged) {
  if (geometryChanged) {
    updateGeometry();
  }
  updateContentWidgetGeometry();
  update();
}

void AdDivider::updateFrameAndSizePolicy() {
  if (orientation_ == Orientation::Horizontal) {
    setFrameShape(QFrame::HLine);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  } else {
    setFrameShape(QFrame::VLine);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }
  setFrameShadow(QFrame::Plain);
  setLineWidth(0);
}

void AdDivider::updateContentWidgetGeometry() {
  if (!contentWidget_) {
    return;
  }
  if (orientation_ == Orientation::Vertical) {
    contentWidget_->hide();
    return;
  }

  const detail::DividerAppearance appearance = resolvedAppearance();
  const ContentLayout contentLayout = calculateContentLayout(this, appearance);
  contentWidget_->setGeometry(contentLayout.innerRect);
  contentWidget_->setVisible(contentLayout.innerRect.isValid());
}

void AdDivider::clearContentWidget(bool deleteWidget) {
  QWidget* widget = contentWidget_.data();
  if (!widget) {
    if (contentDestroyedConnection_) {
      disconnect(contentDestroyedConnection_);
      contentDestroyedConnection_ = {};
    }
    contentWidget_.clear();
    return;
  }

  if (contentDestroyedConnection_) {
    disconnect(contentDestroyedConnection_);
    contentDestroyedConnection_ = {};
  }
  contentWidget_.clear();
  widget->hide();
  if (widget->parentWidget()) {
    widget->setParent(nullptr);
  }
  if (deleteWidget) {
    widget->deleteLater();
  }
}

void AdDivider::attachContentWidget(QWidget* widget) {
  if (!widget) {
    return;
  }

  widget->hide();
  widget->setParent(this);
  contentWidget_ = widget;
  QWidget* rawWidget = widget;
  contentDestroyedConnection_ = connect(widget, &QObject::destroyed, this, [this, rawWidget]() {
    if (contentWidget_ && contentWidget_.data() != rawWidget) {
      return;
    }
    contentWidget_.clear();
    contentDestroyedConnection_ = {};
    updateAccessibleText();
    refreshAfterStateChange();
    emit contentWidgetChanged(nullptr);
  });
}

void AdDivider::updateAccessibleText() {
  if (contentWidget_) {
    const QString contentName = contentWidget_->accessibleName().trimmed();
    if (!contentName.isEmpty()) {
      setAccessibleName(contentName);
      return;
    }
    const QString contentDescription = contentWidget_->accessibleDescription().trimmed();
    if (!contentDescription.isEmpty()) {
      setAccessibleName(contentDescription);
      return;
    }
  }
  if (!text_.isEmpty()) {
    setAccessibleName(text_);
    return;
  }
  setAccessibleName(QString());
}

}  // namespace adqt::widgets
