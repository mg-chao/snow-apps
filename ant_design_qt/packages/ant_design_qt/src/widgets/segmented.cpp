#include "segmented.h"

#include "detail/text_metrics.h"

#include <QApplication>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadioButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVariantAnimation>
#include <algorithm>
#include <utility>

#include "segmented_style.h"
#include "theme/theme.h"

namespace adqt::widgets {

namespace {

template <typename T>
void overlayOptional(std::optional<T>* target, const std::optional<T>& source) {
  if (target && source) {
    *target = source;
  }
}

AdSegmented::ComponentTokens mergeTokens(AdSegmented::ComponentTokens base,
                                         const AdSegmented::ComponentTokens& overlay) {
#define ADQT_SEGMENTED_OVERLAY_COLOR(name) overlayOptional(&base.colors.name, overlay.colors.name)
  ADQT_SEGMENTED_OVERLAY_COLOR(itemColor);
  ADQT_SEGMENTED_OVERLAY_COLOR(itemHoverColor);
  ADQT_SEGMENTED_OVERLAY_COLOR(itemHoverBackground);
  ADQT_SEGMENTED_OVERLAY_COLOR(itemActiveBackground);
  ADQT_SEGMENTED_OVERLAY_COLOR(itemSelectedBackground);
  ADQT_SEGMENTED_OVERLAY_COLOR(itemSelectedColor);
  ADQT_SEGMENTED_OVERLAY_COLOR(itemDisabledColor);
  ADQT_SEGMENTED_OVERLAY_COLOR(trackBackground);
  ADQT_SEGMENTED_OVERLAY_COLOR(focusOutline);
  ADQT_SEGMENTED_OVERLAY_COLOR(thumbShadow);
#undef ADQT_SEGMENTED_OVERLAY_COLOR
#define ADQT_SEGMENTED_OVERLAY_METRIC(name) \
  overlayOptional(&base.metrics.name, overlay.metrics.name)
  ADQT_SEGMENTED_OVERLAY_METRIC(trackPadding);
  ADQT_SEGMENTED_OVERLAY_METRIC(horizontalPadding);
  ADQT_SEGMENTED_OVERLAY_METRIC(smallHorizontalPadding);
  ADQT_SEGMENTED_OVERLAY_METRIC(borderRadius);
  ADQT_SEGMENTED_OVERLAY_METRIC(iconSize);
  ADQT_SEGMENTED_OVERLAY_METRIC(iconGap);
  ADQT_SEGMENTED_OVERLAY_METRIC(focusOutlineWidth);
  ADQT_SEGMENTED_OVERLAY_METRIC(focusOutlineOffset);
  ADQT_SEGMENTED_OVERLAY_METRIC(thumbShadowOffsetY);
#undef ADQT_SEGMENTED_OVERLAY_METRIC
  return base;
}

void mergeSemanticSlot(AdSegmented::SemanticSlotStyle* target,
                       const AdSegmented::SemanticSlotStyle& overlay) {
  if (!target) {
    return;
  }
  overlayOptional(&target->textColor, overlay.textColor);
  overlayOptional(&target->backgroundColor, overlay.backgroundColor);
  overlayOptional(&target->font, overlay.font);
}

AdSegmented::SemanticStyles mergeSemanticStyles(AdSegmented::SemanticStyles base,
                                                const AdSegmented::SemanticStyles& overlay) {
  mergeSemanticSlot(&base.root, overlay.root);
  mergeSemanticSlot(&base.item, overlay.item);
  mergeSemanticSlot(&base.label, overlay.label);
  mergeSemanticSlot(&base.icon, overlay.icon);
  return base;
}

bool sameVariant(const QVariant& lhs, const QVariant& rhs) {
  if (!lhs.isValid() || !rhs.isValid()) {
    return !lhs.isValid() && !rhs.isValid();
  }
  return lhs == rhs;
}

bool sameOption(const AdSegmented::Option& lhs, const AdSegmented::Option& rhs) {
  return sameVariant(lhs.value, rhs.value) && lhs.label == rhs.label && lhs.icon == rhs.icon &&
         lhs.tooltip == rhs.tooltip && lhs.enabled == rhs.enabled &&
         sameVariant(lhs.data, rhs.data);
}

bool sameOptions(const QList<AdSegmented::Option>& lhs, const QList<AdSegmented::Option>& rhs) {
  return lhs.size() == rhs.size() && std::equal(lhs.cbegin(), lhs.cend(), rhs.cbegin(), sameOption);
}

bool sameComponentTokens(const AdSegmented::ComponentTokens& lhs,
                         const AdSegmented::ComponentTokens& rhs) {
#define ADQT_SEGMENTED_SAME_COLOR(name) lhs.colors.name == rhs.colors.name
  const bool colorsEqual =
      ADQT_SEGMENTED_SAME_COLOR(itemColor) && ADQT_SEGMENTED_SAME_COLOR(itemHoverColor) &&
      ADQT_SEGMENTED_SAME_COLOR(itemHoverBackground) &&
      ADQT_SEGMENTED_SAME_COLOR(itemActiveBackground) &&
      ADQT_SEGMENTED_SAME_COLOR(itemSelectedBackground) &&
      ADQT_SEGMENTED_SAME_COLOR(itemSelectedColor) &&
      ADQT_SEGMENTED_SAME_COLOR(itemDisabledColor) && ADQT_SEGMENTED_SAME_COLOR(trackBackground) &&
      ADQT_SEGMENTED_SAME_COLOR(focusOutline) && ADQT_SEGMENTED_SAME_COLOR(thumbShadow);
#undef ADQT_SEGMENTED_SAME_COLOR
#define ADQT_SEGMENTED_SAME_METRIC(name) lhs.metrics.name == rhs.metrics.name
  const bool metricsEqual =
      ADQT_SEGMENTED_SAME_METRIC(trackPadding) && ADQT_SEGMENTED_SAME_METRIC(horizontalPadding) &&
      ADQT_SEGMENTED_SAME_METRIC(smallHorizontalPadding) &&
      ADQT_SEGMENTED_SAME_METRIC(borderRadius) && ADQT_SEGMENTED_SAME_METRIC(iconSize) &&
      ADQT_SEGMENTED_SAME_METRIC(iconGap) && ADQT_SEGMENTED_SAME_METRIC(focusOutlineWidth) &&
      ADQT_SEGMENTED_SAME_METRIC(focusOutlineOffset) &&
      ADQT_SEGMENTED_SAME_METRIC(thumbShadowOffsetY);
#undef ADQT_SEGMENTED_SAME_METRIC
  return colorsEqual && metricsEqual;
}

bool sameSemanticSlot(const AdSegmented::SemanticSlotStyle& lhs,
                      const AdSegmented::SemanticSlotStyle& rhs) {
  return lhs.textColor == rhs.textColor && lhs.backgroundColor == rhs.backgroundColor &&
         lhs.font == rhs.font;
}

bool sameSemanticStyles(const AdSegmented::SemanticStyles& lhs,
                        const AdSegmented::SemanticStyles& rhs) {
  return sameSemanticSlot(lhs.root, rhs.root) && sameSemanticSlot(lhs.item, rhs.item) &&
         sameSemanticSlot(lhs.label, rhs.label) && sameSemanticSlot(lhs.icon, rhs.icon);
}

AdSegmented::Option normalizedOption(AdSegmented::Option option) {
  if (!option.value.isValid()) {
    option.value = option.label;
  }
  return option;
}

bool keyboardFocusReason(Qt::FocusReason reason) {
  return reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason ||
         reason == Qt::ShortcutFocusReason;
}

class SegmentButton final : public QRadioButton {
 public:
  explicit SegmentButton(QWidget* parent = nullptr) : QRadioButton(parent) {
    setObjectName(QStringLiteral("ad-segmented-item"));
    setAutoExclusive(true);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }

  void configure(int index, const AdSegmented::Option& option, bool selected, bool ownerEnabled,
                 AdSegmented::ControlSize controlSize, Qt::Orientation orientation,
                 const detail::SegmentedAppearance& appearance,
                 const AdSegmented::SemanticStyles& semanticStyles,
                 const AdSegmented::ItemPaintCallback& paintCallback,
                 const AdSegmented::ItemSizeHintCallback& sizeHintCallback) {
    index_ = index;
    option_ = option;
    controlSize_ = controlSize;
    orientation_ = orientation;
    appearance_ = appearance;
    semanticStyles_ = semanticStyles;
    paintCallback_ = paintCallback;
    sizeHintCallback_ = sizeHintCallback;
    setText(option.label);
    setToolTip(option.tooltip);
    setEnabled(ownerEnabled && option.enabled);
    setChecked(selected);
    setProperty("optionIndex", index);
    setProperty("optionValue", option.value);
    setAccessibleName(option.label.isEmpty() ? option.value.toString() : option.label);
    setAccessibleDescription(selected ? tr("Selected option") : tr("Option"));
    setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    updateGeometry();
    update();
  }

  int index() const { return index_; }
  bool hovered() const { return hovered_; }
  bool pressed() const { return pressed_; }
  bool focusVisible() const { return focusVisible_; }

  std::function<void(int, int)> navigate;
  std::function<void()> stateChanged;

  QSize sizeHint() const override {
    QFont resolvedFont = semanticStyles_.label.font.value_or(semanticStyles_.item.font.value_or(
        semanticStyles_.root.font.value_or(appearance_.metrics.font)));
    if (sizeHintCallback_) {
      const QSize custom = sizeHintCallback_(option_, controlSize_, resolvedFont);
      if (custom.isValid() && !custom.isEmpty()) {
        return QSize(std::max(1, custom.width()), std::max(itemHeight(), custom.height()));
      }
    }

    const int textWidth = detail::singleLineTextWidth(resolvedFont, option_.label);
    const int iconWidth = adqt::icons::isValid(option_.icon) ? appearance_.metrics.iconSize : 0;
    const int gap = iconWidth > 0 && textWidth > 0 ? appearance_.metrics.iconGap : 0;
    const int width = std::max(
        itemHeight(), textWidth + iconWidth + gap + appearance_.metrics.horizontalPadding * 2);
    return QSize(width, itemHeight());
  }

  QSize minimumSizeHint() const override {
    const int contentFloor = adqt::icons::isValid(option_.icon) ? appearance_.metrics.iconSize : 12;
    return QSize(contentFloor + appearance_.metrics.horizontalPadding * 2, itemHeight());
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const bool effectiveEnabled = isEnabled();
    const bool selected = isChecked();
    QColor foreground = appearance_.itemColor;
    if (!effectiveEnabled) {
      foreground = appearance_.itemDisabledColor;
    } else if (selected) {
      foreground = appearance_.itemSelectedColor;
    } else if (hovered_ || pressed_) {
      foreground = appearance_.itemHoverColor;
    }
    foreground = semanticStyles_.label.textColor.value_or(semanticStyles_.item.textColor.value_or(
        semanticStyles_.root.textColor.value_or(foreground)));

    QColor background = semanticStyles_.item.backgroundColor.value_or(Qt::transparent);
    if (effectiveEnabled && !selected) {
      if (pressed_) {
        background = appearance_.itemActiveBackground;
      } else if (hovered_) {
        background = appearance_.itemHoverBackground;
      }
    }
    if (background.alpha() > 0) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(background);
      painter.drawRoundedRect(QRectF(rect()), appearance_.metrics.itemBorderRadius,
                              appearance_.metrics.itemBorderRadius);
    }

    const QRect contentRect = rect().adjusted(appearance_.metrics.horizontalPadding, 0,
                                              -appearance_.metrics.horizontalPadding, 0);
    if (semanticStyles_.label.backgroundColor &&
        semanticStyles_.label.backgroundColor->alpha() > 0) {
      painter.fillRect(contentRect, *semanticStyles_.label.backgroundColor);
    }
    QFont resolvedFont = semanticStyles_.label.font.value_or(semanticStyles_.item.font.value_or(
        semanticStyles_.root.font.value_or(appearance_.metrics.font)));
    painter.setFont(resolvedFont);

    if (paintCallback_) {
      AdSegmented::ItemPaintInfo info;
      info.index = index_;
      info.option = option_;
      info.itemRect = rect();
      info.contentRect = contentRect;
      info.foreground = foreground;
      info.font = resolvedFont;
      info.iconSize = appearance_.metrics.iconSize;
      info.iconGap = appearance_.metrics.iconGap;
      info.selected = selected;
      info.hovered = hovered_;
      info.pressed = pressed_;
      info.focused = hasFocus();
      info.enabled = effectiveEnabled;
      paintCallback_(painter, info);
    } else {
      paintDefaultContent(&painter, contentRect, resolvedFont, foreground);
    }

    if (hasFocus() && focusVisible_) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(appearance_.focusOutline, appearance_.metrics.focusOutlineWidth,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      const qreal inset =
          appearance_.metrics.focusOutlineOffset + appearance_.metrics.focusOutlineWidth / 2.0;
      painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset),
                              appearance_.metrics.itemBorderRadius,
                              appearance_.metrics.itemBorderRadius);
    }
  }

  void enterEvent(QEnterEvent* event) override {
    hovered_ = true;
    update();
    if (stateChanged) {
      stateChanged();
    }
    QRadioButton::enterEvent(event);
  }

  void leaveEvent(QEvent* event) override {
    hovered_ = false;
    pressed_ = false;
    update();
    if (stateChanged) {
      stateChanged();
    }
    QRadioButton::leaveEvent(event);
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && isEnabled()) {
      pressed_ = true;
      update();
      if (stateChanged) {
        stateChanged();
      }
    }
    QRadioButton::mousePressEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    const bool wasPressed = pressed_;
    pressed_ = false;
    if (wasPressed) {
      update();
      if (stateChanged) {
        stateChanged();
      }
    }
    QRadioButton::mouseReleaseEvent(event);
  }

  void keyPressEvent(QKeyEvent* event) override {
    int delta = 0;
    if (orientation_ == Qt::Horizontal) {
      if (event->key() == Qt::Key_Left) {
        delta = layoutDirection() == Qt::RightToLeft ? 1 : -1;
      } else if (event->key() == Qt::Key_Right) {
        delta = layoutDirection() == Qt::RightToLeft ? -1 : 1;
      }
    } else if (event->key() == Qt::Key_Up) {
      delta = -1;
    } else if (event->key() == Qt::Key_Down) {
      delta = 1;
    }

    if (delta != 0 && navigate) {
      navigate(index_, delta);
      event->accept();
      return;
    }
    if ((event->key() == Qt::Key_Home || event->key() == Qt::Key_End) && navigate) {
      navigate(index_, event->key() == Qt::Key_Home ? -1000000 : 1000000);
      event->accept();
      return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && isEnabled()) {
      click();
      event->accept();
      return;
    }
    QRadioButton::keyPressEvent(event);
  }

  void focusInEvent(QFocusEvent* event) override {
    focusVisible_ = keyboardFocusReason(event->reason());
    update();
    QRadioButton::focusInEvent(event);
  }

  void focusOutEvent(QFocusEvent* event) override {
    focusVisible_ = false;
    update();
    QRadioButton::focusOutEvent(event);
  }

 private:
  int itemHeight() const {
    return std::max(1, appearance_.metrics.controlHeight - appearance_.metrics.trackPadding * 2);
  }

  void paintDefaultContent(QPainter* painter, const QRect& contentRect, const QFont& font,
                           const QColor& foreground) const {
    if (!painter) {
      return;
    }
    const int iconWidth = adqt::icons::isValid(option_.icon) ? appearance_.metrics.iconSize : 0;
    const int iconGap = iconWidth > 0 && !option_.label.isEmpty() ? appearance_.metrics.iconGap : 0;
    const int availableTextWidth = std::max(0, contentRect.width() - iconWidth - iconGap);
    const QString displayText =
        detail::elidedSingleLineText(font, option_.label, availableTextWidth);
    const int textWidth = detail::singleLineTextAdvanceWidth(font, displayText);
    const int groupWidth = iconWidth + iconGap + textWidth;
    const int groupLeft = contentRect.left() + std::max(0, (contentRect.width() - groupWidth) / 2);
    const bool rtl = layoutDirection() == Qt::RightToLeft;
    const int iconLeft = rtl ? groupLeft + textWidth + iconGap : groupLeft;
    const int textLeft = rtl ? groupLeft : groupLeft + iconWidth + iconGap;

    if (iconWidth > 0) {
      adqt::icons::IconRef icon = option_.icon;
      icon = icon.withColors(
          icon.colors().withPrimary(semanticStyles_.icon.textColor.value_or(foreground)));
      const QRect iconRect(iconLeft, (height() - iconWidth) / 2, iconWidth, iconWidth);
      if (semanticStyles_.icon.backgroundColor &&
          semanticStyles_.icon.backgroundColor->alpha() > 0) {
        painter->fillRect(iconRect, *semanticStyles_.icon.backgroundColor);
      }
      adqt::icons::paintIcon(painter, icon, iconRect);
    }

    if (!displayText.isEmpty()) {
      painter->setPen(foreground);
      painter->drawText(QRect(textLeft, 0, textWidth, height()),
                        static_cast<int>(Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft) |
                                         Qt::TextSingleLine),
                        displayText);
    }
  }

  int index_ = -1;
  AdSegmented::Option option_;
  AdSegmented::ControlSize controlSize_ = AdSegmented::ControlSize::Medium;
  Qt::Orientation orientation_ = Qt::Horizontal;
  detail::SegmentedAppearance appearance_;
  AdSegmented::SemanticStyles semanticStyles_;
  AdSegmented::ItemPaintCallback paintCallback_;
  AdSegmented::ItemSizeHintCallback sizeHintCallback_;
  bool hovered_ = false;
  bool pressed_ = false;
  bool focusVisible_ = false;
};

}  // namespace

struct AdSegmented::Private {
  explicit Private(AdSegmented* owner)
      : q(owner), group(new QButtonGroup(owner)), animation(new QVariantAnimation(owner)) {
    group->setExclusive(true);
    layout = new QBoxLayout(QBoxLayout::LeftToRight, owner);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    animation->setEasingCurve(QEasingCurve::InOutCubic);
    QObject::connect(animation, &QVariantAnimation::valueChanged, owner,
                     [this](const QVariant& value) {
                       thumbRect = value.toRectF();
                       q->update();
                     });
    QObject::connect(animation, &QVariantAnimation::finished, owner, [this] {
      thumbRect = targetThumbRect();
      q->update();
    });
    QObject::connect(group, &QButtonGroup::idClicked, owner, [this](int index) {
      if (index < 0 || index >= options.size() || !options.at(index).enabled || !q->isEnabled()) {
        refreshButtons(false);
        return;
      }
      q->setCurrentIndex(index);
      emit q->activated(index, options.at(index).value);
    });
  }

  ComponentTokenContext tokenContext() const {
    ComponentTokenContext context;
    context.controlSize = controlSize;
    context.orientation = orientation;
    context.shape = shape;
    context.currentIndex = currentIndex;
    context.enabled = q->isEnabled();
    context.distribution = distribution;
    return context;
  }

  StyleContext styleContext() const {
    StyleContext context;
    context.controlSize = controlSize;
    context.orientation = orientation;
    context.shape = shape;
    context.currentIndex = currentIndex;
    context.hoveredIndex = hoveredIndex;
    context.pressedIndex = pressedIndex;
    context.enabled = q->isEnabled();
    context.distribution = distribution;
    return context;
  }

  ComponentTokens effectiveTokens() const {
    return tokenResolver ? mergeTokens(componentTokens, tokenResolver(tokenContext()))
                         : componentTokens;
  }

  SemanticStyles effectiveSemanticStyles() const {
    return semanticStyleResolver
               ? mergeSemanticStyles(semanticStyles, semanticStyleResolver(styleContext()))
               : semanticStyles;
  }

  detail::SegmentedAppearance appearance() const {
    return detail::resolveSegmentedAppearance(q, effectiveTokens());
  }

  QVariant currentValue() const {
    return currentIndex >= 0 && currentIndex < options.size() ? options.at(currentIndex).value
                                                              : QVariant();
  }

  QRectF targetThumbRect() const {
    return currentIndex >= 0 && currentIndex < buttons.size() && buttons.at(currentIndex)
               ? QRectF(buttons.at(currentIndex)->geometry())
               : QRectF();
  }

  void rebuildButtons() {
    bool restoreFocus = false;
    bool restoreFocusVisible = false;
    for (SegmentButton* button : std::as_const(buttons)) {
      if (button && button->hasFocus()) {
        restoreFocus = true;
        restoreFocusVisible = button->focusVisible();
        break;
      }
    }
    while (QLayoutItem* item = layout->takeAt(0)) {
      delete item;
    }
    for (SegmentButton* button : std::as_const(buttons)) {
      group->removeButton(button);
      delete button;
    }
    buttons.clear();
    hoveredIndex = -1;
    pressedIndex = -1;

    for (int index = 0; index < options.size(); ++index) {
      auto* button = new SegmentButton(q);
      buttons.append(button);
      group->addButton(button, index);
      layout->addWidget(button);
      button->navigate = [this](int from, int delta) { navigate(from, delta); };
      button->stateChanged = [this] { refreshInteractiveState(); };
    }
    refreshButtons(true);
    if (restoreFocus) {
      focusCurrentButton(restoreFocusVisible);
      QTimer::singleShot(0, q, [this, restoreFocusVisible] {
        QWidget* focused = QApplication::focusWidget();
        if (!focused || focused == q || q->isAncestorOf(focused)) {
          focusCurrentButton(restoreFocusVisible);
        }
      });
    }
  }

  void refreshInteractiveState() {
    int nextHovered = -1;
    int nextPressed = -1;
    for (SegmentButton* button : std::as_const(buttons)) {
      if (!button) {
        continue;
      }
      if (button->hovered()) {
        nextHovered = button->index();
      }
      if (button->pressed()) {
        nextPressed = button->index();
      }
    }
    const bool changed = hoveredIndex != nextHovered || pressedIndex != nextPressed;
    hoveredIndex = nextHovered;
    pressedIndex = nextPressed;
    if (changed && semanticStyleResolver) {
      refreshButtons(false);
    }
  }

  void refreshButtons(bool geometryChanged) {
    bool restoreFocus = false;
    bool restoreFocusVisible = false;
    for (SegmentButton* button : std::as_const(buttons)) {
      if (button && button->hasFocus()) {
        restoreFocus = true;
        restoreFocusVisible = button->focusVisible();
        break;
      }
    }
    const detail::SegmentedAppearance resolved = appearance();
    const SemanticStyles resolvedSemantic = effectiveSemanticStyles();
    const int selected = currentIndex;
    const int fallbackFocus = firstEnabledIndex();

    layout->setDirection(orientation == Qt::Horizontal ? QBoxLayout::LeftToRight
                                                       : QBoxLayout::TopToBottom);
    layout->setContentsMargins(resolved.metrics.trackPadding, resolved.metrics.trackPadding,
                               resolved.metrics.trackPadding, resolved.metrics.trackPadding);
    group->setExclusive(false);
    for (SegmentButton* button : std::as_const(buttons)) {
      if (button) {
        button->setAutoExclusive(false);
      }
    }
    for (int index = 0; index < buttons.size(); ++index) {
      SegmentButton* button = buttons.at(index);
      const bool focusable = index == selected || (selected < 0 && index == fallbackFocus);
      button->configure(index, options.at(index), index == selected, q->isEnabled(), controlSize,
                        orientation, resolved, resolvedSemantic, itemPaintCallback,
                        itemSizeHintCallback);
      button->setFocusPolicy(focusable && button->isEnabled() ? Qt::TabFocus : Qt::NoFocus);
      if (orientation == Qt::Horizontal) {
        button->setSizePolicy(
            distribution == Distribution::Fill ? QSizePolicy::Expanding : QSizePolicy::Fixed,
            QSizePolicy::Fixed);
      } else {
        button->setSizePolicy(QSizePolicy::Expanding, distribution == Distribution::Fill
                                                          ? QSizePolicy::Expanding
                                                          : QSizePolicy::Fixed);
      }
    }
    for (SegmentButton* button : std::as_const(buttons)) {
      if (button) {
        button->setAutoExclusive(true);
      }
    }
    group->setExclusive(true);
    if (restoreFocus) {
      focusCurrentButton(restoreFocusVisible);
    }

    if (orientation == Qt::Horizontal) {
      q->setSizePolicy(
          distribution == Distribution::Fill ? QSizePolicy::Expanding : QSizePolicy::Preferred,
          QSizePolicy::Fixed);
    } else {
      q->setSizePolicy(
          distribution == Distribution::Fill ? QSizePolicy::Expanding : QSizePolicy::Preferred,
          distribution == Distribution::Fill ? QSizePolicy::Expanding : QSizePolicy::Preferred);
    }
    if (geometryChanged) {
      q->updateGeometry();
      layout->invalidate();
      layout->activate();
      syncThumb(false);
    }
    q->update();
  }

  void syncThumb(bool animate) {
    layout->activate();
    const QRectF target = targetThumbRect();
    const detail::SegmentedAppearance resolved = appearance();
    const bool canAnimate = animate && q->isVisible() && animated && !thumbRect.isEmpty() &&
                            !target.isEmpty() && resolved.metrics.animationDurationMs > 0;
    animation->stop();
    if (!canAnimate) {
      thumbRect = target;
      q->update();
      return;
    }
    animation->setDuration(resolved.metrics.animationDurationMs);
    animation->setStartValue(thumbRect);
    animation->setEndValue(target);
    animation->start();
  }

  void navigate(int from, int delta) {
    if (options.isEmpty() || !q->isEnabled()) {
      return;
    }
    int next = -1;
    if (delta <= -1000000) {
      next = firstEnabledIndex();
    } else if (delta >= 1000000) {
      for (qsizetype index = options.size() - 1; index >= 0; --index) {
        if (options.at(index).enabled) {
          next = static_cast<int>(index);
          break;
        }
      }
    } else {
      const int count = static_cast<int>(options.size());
      for (int offset = 1; offset <= count; ++offset) {
        const int candidate = (from + delta * offset + count * 2) % count;
        if (options.at(candidate).enabled) {
          next = candidate;
          break;
        }
      }
    }
    if (next < 0 || next >= buttons.size() || next == from) {
      return;
    }
    q->setCurrentIndex(next);
    buttons.at(next)->setFocus(Qt::TabFocusReason);
    emit q->activated(next, options.at(next).value);
  }

  int firstEnabledIndex() const {
    for (int index = 0; index < options.size(); ++index) {
      if (options.at(index).enabled) {
        return index;
      }
    }
    return -1;
  }

  void focusCurrentButton(bool visible) {
    const int focusIndex = currentIndex >= 0 ? currentIndex : firstEnabledIndex();
    if (focusIndex >= 0 && focusIndex < buttons.size() && buttons.at(focusIndex)->isEnabled()) {
      buttons.at(focusIndex)->setFocus(visible ? Qt::TabFocusReason : Qt::OtherFocusReason);
    }
  }

  int firstEnabledIndex(int from, int direction) const {
    if (options.isEmpty() || direction == 0) {
      return -1;
    }
    const int optionCount = static_cast<int>(options.size());
    for (int index = std::clamp(from, 0, optionCount - 1); index >= 0 && index < optionCount;
         index += direction) {
      if (options.at(index).enabled) {
        return index;
      }
    }
    return -1;
  }

  void emitSelectionChanges(int previousIndex, const QVariant& previousValue) {
    const int nextIndex = currentIndex;
    const QVariant nextValue = currentValue();
    const bool indexChanged = previousIndex != nextIndex;
    const bool valueChanged = !sameVariant(previousValue, nextValue);
    if (indexChanged) {
      emit q->currentIndexChanged(nextIndex);
    }
    if (valueChanged) {
      emit q->currentValueChanged(nextValue);
    }
    if (indexChanged || valueChanged) {
      emit q->currentChanged(nextIndex, nextValue);
    }
  }

  AdSegmented* q = nullptr;
  QBoxLayout* layout = nullptr;
  QButtonGroup* group = nullptr;
  QVariantAnimation* animation = nullptr;
  QList<Option> options;
  QList<SegmentButton*> buttons;
  int currentIndex = -1;
  ControlSize controlSize = ControlSize::Medium;
  Qt::Orientation orientation = Qt::Horizontal;
  Shape shape = Shape::Default;
  Distribution distribution = Distribution::Content;
  bool animated = true;
  int hoveredIndex = -1;
  int pressedIndex = -1;
  QRectF thumbRect;
  ComponentTokens componentTokens;
  ComponentTokenResolver tokenResolver;
  SemanticStyles semanticStyles;
  SemanticStyleResolver semanticStyleResolver;
  ItemPaintCallback itemPaintCallback;
  ItemSizeHintCallback itemSizeHintCallback;
};

AdSegmented::AdSegmented(QWidget* parent) : QWidget(parent), d_(std::make_unique<Private>(this)) {
  setObjectName(QStringLiteral("ad-segmented"));
  setAccessibleName(tr("Segmented control"));
  setAttribute(Qt::WA_OpaquePaintEvent, false);
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this] { d_->refreshButtons(true); });
}

AdSegmented::~AdSegmented() = default;

int AdSegmented::count() const { return static_cast<int>(d_->options.size()); }
int AdSegmented::currentIndex() const { return d_->currentIndex; }
QVariant AdSegmented::currentValue() const { return d_->currentValue(); }

int AdSegmented::addOption(const QString& label, const QVariant& value) {
  Option option;
  option.label = label;
  option.value = value;
  return addOption(option);
}

int AdSegmented::addOption(const Option& option) { return insertOption(count(), option); }

int AdSegmented::insertOption(int index, const Option& rawOption) {
  if (index < 0 || index > d_->options.size()) {
    return -1;
  }
  const Option option = normalizedOption(rawOption);
  if (indexOfValue(option.value) >= 0) {
    return -1;
  }
  const int previousIndex = currentIndex();
  const QVariant previousValue = currentValue();
  d_->options.insert(index, option);
  if (d_->currentIndex < 0 && option.enabled) {
    d_->currentIndex = index;
  } else if (index <= d_->currentIndex) {
    ++d_->currentIndex;
  }
  d_->rebuildButtons();
  d_->emitSelectionChanges(previousIndex, previousValue);
  emit optionsChanged();
  return index;
}

void AdSegmented::removeOption(int index) {
  if (index < 0 || index >= d_->options.size()) {
    return;
  }
  const int previousIndex = currentIndex();
  const QVariant previousValue = currentValue();
  d_->options.removeAt(index);
  if (d_->options.isEmpty()) {
    d_->currentIndex = -1;
  } else if (index < d_->currentIndex) {
    --d_->currentIndex;
  } else if (index == d_->currentIndex) {
    const int lastIndex = static_cast<int>(d_->options.size()) - 1;
    d_->currentIndex = d_->firstEnabledIndex(std::min(index, lastIndex), 1);
    if (d_->currentIndex < 0) {
      d_->currentIndex = d_->firstEnabledIndex(std::min(index - 1, lastIndex), -1);
    }
  }
  d_->rebuildButtons();
  d_->emitSelectionChanges(previousIndex, previousValue);
  emit optionsChanged();
}

void AdSegmented::clear() {
  if (d_->options.isEmpty()) {
    return;
  }
  const int previousIndex = currentIndex();
  const QVariant previousValue = currentValue();
  d_->options.clear();
  d_->currentIndex = -1;
  d_->rebuildButtons();
  d_->emitSelectionChanges(previousIndex, previousValue);
  emit optionsChanged();
}

AdSegmented::Option AdSegmented::option(int index) const {
  return index >= 0 && index < d_->options.size() ? d_->options.at(index) : Option{};
}

QList<AdSegmented::Option> AdSegmented::options() const { return d_->options; }

void AdSegmented::setOptions(const QList<Option>& options) {
  QList<Option> normalized;
  normalized.reserve(options.size());
  for (const Option& raw : options) {
    const Option candidate = normalizedOption(raw);
    const bool duplicate =
        std::any_of(normalized.cbegin(), normalized.cend(), [&candidate](const Option& existing) {
          return sameVariant(existing.value, candidate.value);
        });
    if (!duplicate) {
      normalized.append(candidate);
    }
  }

  if (sameOptions(d_->options, normalized)) {
    return;
  }

  const int previousIndex = currentIndex();
  const QVariant previousValue = currentValue();
  const bool hadOptions = !d_->options.isEmpty();
  d_->options = normalized;
  d_->currentIndex = previousIndex >= 0 ? indexOfValue(previousValue) : -1;
  if (d_->currentIndex >= 0 && !d_->options.at(d_->currentIndex).enabled) {
    d_->currentIndex = -1;
  }
  if (d_->currentIndex < 0 && (previousIndex >= 0 || !hadOptions)) {
    d_->currentIndex = d_->firstEnabledIndex();
  }
  d_->rebuildButtons();
  d_->emitSelectionChanges(previousIndex, previousValue);
  emit optionsChanged();
}

int AdSegmented::indexOfValue(const QVariant& value) const {
  for (int index = 0; index < d_->options.size(); ++index) {
    if (sameVariant(d_->options.at(index).value, value)) {
      return index;
    }
  }
  return -1;
}

QRect AdSegmented::optionRect(int index) const {
  return index >= 0 && index < d_->buttons.size() && d_->buttons.at(index)
             ? d_->buttons.at(index)->geometry()
             : QRect();
}

int AdSegmented::optionAt(const QPoint& position) const {
  for (int index = 0; index < d_->buttons.size(); ++index) {
    if (d_->buttons.at(index) && d_->buttons.at(index)->geometry().contains(position)) {
      return index;
    }
  }
  return -1;
}

QString AdSegmented::optionLabel(int index) const {
  return index >= 0 && index < d_->options.size() ? d_->options.at(index).label : QString();
}

void AdSegmented::setOptionLabel(int index, const QString& label) {
  if (index < 0 || index >= d_->options.size() || d_->options.at(index).label == label) {
    return;
  }
  d_->options[index].label = label;
  d_->refreshButtons(true);
  emit optionsChanged();
}

adqt::icons::IconRef AdSegmented::optionIcon(int index) const {
  return index >= 0 && index < d_->options.size() ? d_->options.at(index).icon
                                                  : adqt::icons::IconRef{};
}

void AdSegmented::setOptionIcon(int index, const adqt::icons::IconRef& icon) {
  if (index < 0 || index >= d_->options.size() || d_->options.at(index).icon == icon) {
    return;
  }
  d_->options[index].icon = icon;
  d_->refreshButtons(true);
  emit optionsChanged();
}

QString AdSegmented::optionTooltip(int index) const {
  return index >= 0 && index < d_->options.size() ? d_->options.at(index).tooltip : QString();
}

void AdSegmented::setOptionTooltip(int index, const QString& tooltip) {
  if (index < 0 || index >= d_->options.size() || d_->options.at(index).tooltip == tooltip) {
    return;
  }
  d_->options[index].tooltip = tooltip;
  d_->refreshButtons(false);
  emit optionsChanged();
}

bool AdSegmented::isOptionEnabled(int index) const {
  return index >= 0 && index < d_->options.size() && d_->options.at(index).enabled;
}

void AdSegmented::setOptionEnabled(int index, bool enabled) {
  if (index < 0 || index >= d_->options.size() || d_->options.at(index).enabled == enabled) {
    return;
  }
  const int previousIndex = currentIndex();
  const QVariant previousValue = currentValue();
  d_->options[index].enabled = enabled;
  if (!enabled && index == d_->currentIndex) {
    d_->currentIndex = d_->firstEnabledIndex(index + 1, 1);
    if (d_->currentIndex < 0) {
      d_->currentIndex = d_->firstEnabledIndex(index - 1, -1);
    }
  } else if (enabled && d_->currentIndex < 0) {
    d_->currentIndex = index;
  }
  d_->refreshButtons(false);
  d_->syncThumb(true);
  d_->emitSelectionChanges(previousIndex, previousValue);
  emit optionsChanged();
}

QVariant AdSegmented::optionData(int index) const {
  return index >= 0 && index < d_->options.size() ? d_->options.at(index).data : QVariant();
}

void AdSegmented::setOptionData(int index, const QVariant& value) {
  if (index < 0 || index >= d_->options.size() || sameVariant(d_->options.at(index).data, value)) {
    return;
  }
  d_->options[index].data = value;
  emit optionsChanged();
}

AdSegmented::ControlSize AdSegmented::controlSize() const { return d_->controlSize; }

void AdSegmented::setControlSize(ControlSize value) {
  if (d_->controlSize == value) {
    return;
  }
  d_->controlSize = value;
  d_->refreshButtons(true);
  emit controlSizeChanged(value);
}

Qt::Orientation AdSegmented::orientation() const { return d_->orientation; }

void AdSegmented::setOrientation(Qt::Orientation value) {
  if (d_->orientation == value) {
    return;
  }
  d_->orientation = value;
  d_->animation->stop();
  d_->refreshButtons(true);
  emit orientationChanged(value);
}

AdSegmented::Distribution AdSegmented::distribution() const { return d_->distribution; }

void AdSegmented::setDistribution(Distribution value) {
  if (d_->distribution == value) {
    return;
  }
  d_->distribution = value;
  d_->refreshButtons(true);
  emit distributionChanged(value);
}

AdSegmented::Shape AdSegmented::shape() const { return d_->shape; }

void AdSegmented::setShape(Shape value) {
  if (d_->shape == value) {
    return;
  }
  d_->shape = value;
  d_->refreshButtons(false);
  emit shapeChanged(value);
}

bool AdSegmented::animated() const { return d_->animated; }

void AdSegmented::setAnimated(bool value) {
  if (d_->animated == value) {
    return;
  }
  d_->animated = value;
  if (!value) {
    d_->syncThumb(false);
  }
  emit animatedChanged(value);
}

AdSegmented::ComponentTokens AdSegmented::componentTokens() const { return d_->componentTokens; }

void AdSegmented::setComponentTokens(const ComponentTokens& tokens) {
  if (sameComponentTokens(d_->componentTokens, tokens)) {
    return;
  }
  d_->componentTokens = tokens;
  d_->refreshButtons(true);
  emit componentTokensChanged();
}

void AdSegmented::resetComponentTokens() { setComponentTokens(ComponentTokens{}); }

void AdSegmented::setComponentTokenResolver(ComponentTokenResolver resolver) {
  d_->tokenResolver = std::move(resolver);
  d_->refreshButtons(true);
  emit componentTokensChanged();
}

void AdSegmented::resetComponentTokenResolver() {
  if (!d_->tokenResolver) {
    return;
  }
  d_->tokenResolver = {};
  d_->refreshButtons(true);
  emit componentTokensChanged();
}

AdSegmented::SemanticStyles AdSegmented::semanticStyles() const { return d_->semanticStyles; }

void AdSegmented::setSemanticStyles(const SemanticStyles& styles) {
  if (sameSemanticStyles(d_->semanticStyles, styles)) {
    return;
  }
  d_->semanticStyles = styles;
  d_->refreshButtons(true);
  emit semanticStylesChanged();
}

void AdSegmented::resetSemanticStyles() { setSemanticStyles(SemanticStyles{}); }

void AdSegmented::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  d_->semanticStyleResolver = std::move(resolver);
  d_->refreshButtons(true);
  emit semanticStylesChanged();
}

void AdSegmented::resetSemanticStyleResolver() {
  if (!d_->semanticStyleResolver) {
    return;
  }
  d_->semanticStyleResolver = {};
  d_->refreshButtons(true);
  emit semanticStylesChanged();
}

void AdSegmented::setItemPaintCallback(ItemPaintCallback callback) {
  d_->itemPaintCallback = std::move(callback);
  d_->refreshButtons(false);
}

void AdSegmented::resetItemPaintCallback() {
  d_->itemPaintCallback = {};
  d_->refreshButtons(false);
}

void AdSegmented::setItemSizeHintCallback(ItemSizeHintCallback callback) {
  d_->itemSizeHintCallback = std::move(callback);
  d_->refreshButtons(true);
}

void AdSegmented::resetItemSizeHintCallback() {
  d_->itemSizeHintCallback = {};
  d_->refreshButtons(true);
}

QSize AdSegmented::sizeHint() const {
  const detail::SegmentedAppearance appearance = d_->appearance();
  if (d_->buttons.isEmpty()) {
    return QSize(0, appearance.metrics.controlHeight);
  }
  int width = appearance.metrics.trackPadding * 2;
  int height = appearance.metrics.trackPadding * 2;
  if (d_->orientation == Qt::Horizontal) {
    int maximumHeight = 0;
    for (SegmentButton* button : std::as_const(d_->buttons)) {
      width += button->sizeHint().width();
      maximumHeight = std::max(maximumHeight, button->sizeHint().height());
    }
    height += maximumHeight;
  } else {
    int maximumWidth = 0;
    for (SegmentButton* button : std::as_const(d_->buttons)) {
      height += button->sizeHint().height();
      maximumWidth = std::max(maximumWidth, button->sizeHint().width());
    }
    width += maximumWidth;
  }
  return QSize(width, height);
}

QSize AdSegmented::minimumSizeHint() const {
  const detail::SegmentedAppearance appearance = d_->appearance();
  if (d_->buttons.isEmpty()) {
    return QSize(0, appearance.metrics.controlHeight);
  }
  int width = appearance.metrics.trackPadding * 2;
  int height = appearance.metrics.trackPadding * 2;
  if (d_->orientation == Qt::Horizontal) {
    int maximumHeight = 0;
    for (SegmentButton* button : std::as_const(d_->buttons)) {
      width += button->minimumSizeHint().width();
      maximumHeight = std::max(maximumHeight, button->minimumSizeHint().height());
    }
    height += maximumHeight;
  } else {
    int maximumWidth = 0;
    for (SegmentButton* button : std::as_const(d_->buttons)) {
      height += button->minimumSizeHint().height();
      maximumWidth = std::max(maximumWidth, button->minimumSizeHint().width());
    }
    width += maximumWidth;
  }
  return QSize(width, height);
}

void AdSegmented::setCurrentValue(const QVariant& value) {
  if (!value.isValid()) {
    setCurrentIndex(-1);
    return;
  }
  const int index = indexOfValue(value);
  if (index >= 0) {
    setCurrentIndex(index);
  }
}

void AdSegmented::setCurrentIndex(int index) {
  if (index < -1 || index >= d_->options.size() || (index >= 0 && !d_->options.at(index).enabled) ||
      d_->currentIndex == index) {
    return;
  }
  const int previousIndex = currentIndex();
  const QVariant previousValue = currentValue();
  const QRectF previousThumb = d_->targetThumbRect();
  d_->animation->stop();
  d_->thumbRect = previousThumb;
  d_->currentIndex = index;
  d_->refreshButtons(false);
  d_->syncThumb(true);
  d_->emitSelectionChanges(previousIndex, previousValue);
}

void AdSegmented::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)
  const detail::SegmentedAppearance appearance = d_->appearance();
  const SemanticStyles semantic = d_->effectiveSemanticStyles();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const qreal rootRadius = d_->shape == Shape::Round ? std::min(width(), height()) / 2.0
                                                     : appearance.metrics.borderRadius;
  painter.setPen(Qt::NoPen);
  painter.setBrush(semantic.root.backgroundColor.value_or(appearance.trackBackground));
  painter.drawRoundedRect(QRectF(rect()), rootRadius, rootRadius);

  if (!d_->thumbRect.isEmpty()) {
    const qreal itemRadius = d_->shape == Shape::Round
                                 ? std::min(d_->thumbRect.width(), d_->thumbRect.height()) / 2.0
                                 : appearance.metrics.itemBorderRadius;
    if (appearance.thumbShadow.alpha() > 0) {
      painter.setBrush(appearance.thumbShadow);
      painter.drawRoundedRect(d_->thumbRect.translated(0, appearance.metrics.thumbShadowOffsetY),
                              itemRadius, itemRadius);
    }
    painter.setBrush(appearance.itemSelectedBackground);
    painter.drawRoundedRect(d_->thumbRect, itemRadius, itemRadius);
  }
}

void AdSegmented::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  d_->syncThumb(false);
}

void AdSegmented::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }
  switch (event->type()) {
    case QEvent::EnabledChange:
    case QEvent::FontChange:
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::LayoutDirectionChange:
      d_->refreshButtons(true);
      break;
    default:
      break;
  }
}

}  // namespace adqt::widgets
