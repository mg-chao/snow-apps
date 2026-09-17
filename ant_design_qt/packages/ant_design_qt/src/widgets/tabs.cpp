#include "tabs.h"

#include "detail/text_metrics.h"
#include "tabs_style.h"
#include "theme/theme.h"

#include "antd_icons.h"

#include <QAbstractButton>
#include <QAction>
#include <QBoxLayout>
#include <QEvent>
#include <QFocusEvent>
#include <QHash>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QVariantAnimation>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>

namespace adqt::widgets {

namespace {

using detail::TabsAppearance;

template <typename T>
void overlayOptional(std::optional<T>& target, const std::optional<T>& source) {
  if (source) {
    target = source;
  }
}

AdTabs::ComponentTokens mergeTokens(AdTabs::ComponentTokens base,
                                    const AdTabs::ComponentTokens& overlay) {
#define ADQT_OVERLAY_COLOR(name) overlayOptional(base.colors.name, overlay.colors.name)
  ADQT_OVERLAY_COLOR(itemColor);
  ADQT_OVERLAY_COLOR(itemSelectedColor);
  ADQT_OVERLAY_COLOR(itemHoverColor);
  ADQT_OVERLAY_COLOR(itemActiveColor);
  ADQT_OVERLAY_COLOR(itemDisabledColor);
  ADQT_OVERLAY_COLOR(inkBarColor);
  ADQT_OVERLAY_COLOR(cardBackground);
  ADQT_OVERLAY_COLOR(cardActiveBackground);
  ADQT_OVERLAY_COLOR(borderColor);
  ADQT_OVERLAY_COLOR(focusOutline);
#undef ADQT_OVERLAY_COLOR
#define ADQT_OVERLAY_METRIC(name) overlayOptional(base.metrics.name, overlay.metrics.name)
  ADQT_OVERLAY_METRIC(horizontalItemGutter);
  ADQT_OVERLAY_METRIC(horizontalItemPadding);
  ADQT_OVERLAY_METRIC(verticalItemPadding);
  ADQT_OVERLAY_METRIC(cardHeight);
  ADQT_OVERLAY_METRIC(indicatorThickness);
  ADQT_OVERLAY_METRIC(borderRadius);
  ADQT_OVERLAY_METRIC(iconSize);
  ADQT_OVERLAY_METRIC(iconGap);
#undef ADQT_OVERLAY_METRIC
  return base;
}

bool isHorizontal(AdTabs::Placement placement) {
  return placement == AdTabs::Placement::Top || placement == AdTabs::Placement::Bottom;
}

bool keyboardFocusReason(Qt::FocusReason reason) {
  return reason != Qt::MouseFocusReason && reason != Qt::NoFocusReason;
}

QVector<int> shrinkExtentsToFit(const QVector<int>& naturalExtents,
                                const QVector<int>& minimumExtents, int available) {
  QVector<int> result = naturalExtents;
  if (result.isEmpty()) {
    return result;
  }

  const int naturalTotal = std::accumulate(result.cbegin(), result.cend(), 0);
  if (naturalTotal <= available) {
    return result;
  }

  const int minimumTotal = std::accumulate(minimumExtents.cbegin(), minimumExtents.cend(), 0);
  if (minimumTotal >= available) {
    return minimumExtents;
  }

  int low = 0;
  int high = *std::max_element(naturalExtents.cbegin(), naturalExtents.cend());
  int cap = 0;
  while (low <= high) {
    const int candidate = low + (high - low) / 2;
    int total = 0;
    for (int index = 0; index < naturalExtents.size(); ++index) {
      total += std::clamp(candidate, minimumExtents.at(index), naturalExtents.at(index));
    }
    if (total <= available) {
      cap = candidate;
      low = candidate + 1;
    } else {
      high = candidate - 1;
    }
  }

  int used = 0;
  for (int index = 0; index < result.size(); ++index) {
    result[index] = std::clamp(cap, minimumExtents.at(index), naturalExtents.at(index));
    used += result.at(index);
  }
  for (int index = 0; used < available && index < result.size(); ++index) {
    if (result.at(index) < naturalExtents.at(index)) {
      ++result[index];
      ++used;
    }
  }
  return result;
}

class TabButton final : public QAbstractButton {
 public:
  explicit TabButton(QWidget* parent = nullptr) : QAbstractButton(parent) {
    setObjectName(QStringLiteral("ad-tabs-item"));
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }

  void configure(int index, const AdTabs::TabItem& item, bool selected, bool focusable,
                 AdTabs::Type type, AdTabs::Placement placement,
                 AdTabs::IndicatorAlignment indicatorAlignment, int indicatorSize,
                 const TabsAppearance& appearance) {
    index_ = index;
    key_ = item.key;
    setProperty("tabKey", item.key);
    setText(item.label);
    icon_ = item.icon;
    selected_ = selected;
    setChecked(selected);
    setProperty("selected", selected);
    closable_ = type == AdTabs::Type::EditableCard && item.closable;
    type_ = type;
    placement_ = placement;
    indicatorAlignment_ = indicatorAlignment;
    indicatorSize_ = indicatorSize;
    appearance_ = appearance;
    setEnabled(item.enabled);
    setFocusPolicy(item.enabled && focusable ? Qt::TabFocus : Qt::NoFocus);
    setAccessibleName(item.label.isEmpty() ? item.key : item.label);
    setAccessibleDescription(selected ? tr("Selected tab") : tr("Tab"));
    setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    updateElisionTooltip();
    updateGeometry();
    update();
  }

  int index() const { return index_; }
  QString key() const { return key_; }
  bool selected() const { return selected_; }
  bool closable() const { return closable_; }

  std::function<void(int)> activate;
  std::function<void(int)> close;
  std::function<void(int, int)> navigate;

  QSize sizeHint() const override {
    int content = detail::singleLineTextWidth(appearance_.metrics.font, text());
    if (adqt::icons::isValid(icon_)) {
      content += appearance_.metrics.iconSize;
      if (!text().isEmpty()) {
        content += appearance_.metrics.iconGap;
      }
    }
    if (closable_) {
      content += appearance_.metrics.iconGap + closeButtonExtent();
    }

    const int width = std::max(24, content + appearance_.metrics.horizontalPadding * 2);
    return QSize(width, appearance_.metrics.itemHeight);
  }

  QSize minimumSizeHint() const override {
    int content = text().isEmpty() ? 0
                                   : detail::singleLineTextWidth(appearance_.metrics.font,
                                                                 QString(QChar(0x2026)));
    if (adqt::icons::isValid(icon_)) {
      content += appearance_.metrics.iconSize;
      if (!text().isEmpty()) {
        content += appearance_.metrics.iconGap;
      }
    }
    if (closable_) {
      content += appearance_.metrics.iconGap + closeButtonExtent();
    }
    return QSize(std::max(24, content + appearance_.metrics.horizontalPadding * 2),
                 appearance_.metrics.itemHeight);
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(appearance_.metrics.font);

    const bool hovered = underMouse() && isEnabled();
    const bool pressed = isDown() && isEnabled();
    QColor foreground = appearance_.item;
    if (!isEnabled()) {
      foreground = appearance_.disabled;
    } else if (selected_) {
      foreground = appearance_.selected;
    } else if (pressed) {
      foreground = appearance_.active;
    } else if (hovered) {
      foreground = appearance_.hover;
    }

    const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (type_ != AdTabs::Type::Line) {
      const QColor background =
          selected_ ? appearance_.cardActiveBackground : appearance_.cardBackground;
      painter.setPen(QPen(appearance_.border, appearance_.metrics.borderWidth));
      painter.setBrush(background);
      const qreal radius = appearance_.metrics.borderRadius;
      QPainterPath path;
      if (placement_ == AdTabs::Placement::Top) {
        path.moveTo(bounds.left(), bounds.bottom());
        path.lineTo(bounds.left(), bounds.top() + radius);
        path.quadTo(bounds.left(), bounds.top(), bounds.left() + radius, bounds.top());
        path.lineTo(bounds.right() - radius, bounds.top());
        path.quadTo(bounds.right(), bounds.top(), bounds.right(), bounds.top() + radius);
        path.lineTo(bounds.right(), bounds.bottom());
      } else if (placement_ == AdTabs::Placement::Bottom) {
        path.moveTo(bounds.left(), bounds.top());
        path.lineTo(bounds.left(), bounds.bottom() - radius);
        path.quadTo(bounds.left(), bounds.bottom(), bounds.left() + radius, bounds.bottom());
        path.lineTo(bounds.right() - radius, bounds.bottom());
        path.quadTo(bounds.right(), bounds.bottom(), bounds.right(), bounds.bottom() - radius);
        path.lineTo(bounds.right(), bounds.top());
      } else if ((placement_ == AdTabs::Placement::Start) !=
                 (layoutDirection() == Qt::RightToLeft)) {
        path.moveTo(bounds.right(), bounds.top());
        path.lineTo(bounds.left() + radius, bounds.top());
        path.quadTo(bounds.left(), bounds.top(), bounds.left(), bounds.top() + radius);
        path.lineTo(bounds.left(), bounds.bottom() - radius);
        path.quadTo(bounds.left(), bounds.bottom(), bounds.left() + radius, bounds.bottom());
        path.lineTo(bounds.right(), bounds.bottom());
      } else {
        path.moveTo(bounds.left(), bounds.top());
        path.lineTo(bounds.right() - radius, bounds.top());
        path.quadTo(bounds.right(), bounds.top(), bounds.right(), bounds.top() + radius);
        path.lineTo(bounds.right(), bounds.bottom() - radius);
        path.quadTo(bounds.right(), bounds.bottom(), bounds.right() - radius, bounds.bottom());
        path.lineTo(bounds.left(), bounds.bottom());
      }
      path.closeSubpath();
      painter.drawPath(path);
    }

    QRect contentRect = rect().adjusted(appearance_.metrics.horizontalPadding, 0,
                                        -appearance_.metrics.horizontalPadding, 0);
    QRect closeRect;
    if (closable_) {
      closeRect = closeButtonRect();
      if (layoutDirection() == Qt::RightToLeft) {
        contentRect.setLeft(closeRect.right() + appearance_.metrics.iconGap);
      } else {
        contentRect.setRight(closeRect.left() - appearance_.metrics.iconGap);
      }
    }

    const int iconWidth = adqt::icons::isValid(icon_) ? appearance_.metrics.iconSize : 0;
    const int iconGap = iconWidth > 0 && !text().isEmpty() ? appearance_.metrics.iconGap : 0;
    const int availableTextWidth = std::max(0, contentRect.width() - iconWidth - iconGap);
    const QString displayText =
        detail::elidedSingleLineText(appearance_.metrics.font, text(), availableTextWidth);
    const int textWidth = detail::singleLineTextAdvanceWidth(appearance_.metrics.font, displayText);
    const int groupWidth = iconWidth + iconGap + textWidth;
    const int groupLeft = contentRect.x() + std::max(0, (contentRect.width() - groupWidth) / 2);
    const bool rightToLeft = layoutDirection() == Qt::RightToLeft;
    const int iconLeft = rightToLeft ? groupLeft + textWidth + iconGap : groupLeft;
    const int textLeft = rightToLeft ? groupLeft : groupLeft + iconWidth + iconGap;

    if (adqt::icons::isValid(icon_)) {
      adqt::icons::IconRef colored = icon_;
      colored = colored.withColors(colored.colors().withPrimary(foreground));
      const QRect iconRect(iconLeft, (height() - appearance_.metrics.iconSize) / 2,
                           appearance_.metrics.iconSize, appearance_.metrics.iconSize);
      adqt::icons::paintIcon(&painter, colored, iconRect);
    }

    painter.setPen(foreground);
    painter.drawText(
        QRect(textLeft, 0, textWidth, height()),
        static_cast<int>(Qt::AlignVCenter | (rightToLeft ? Qt::AlignRight : Qt::AlignLeft) |
                         Qt::TextSingleLine),
        displayText);

    if (closable_) {
      adqt::icons::IconRef closeIcon = adqt::icons::antd::outlined::Close();
      closeIcon = closeIcon.withColors(
          closeIcon.colors().withPrimary(closeHovered_ ? appearance_.item : appearance_.disabled));
      adqt::icons::paintIcon(&painter, closeIcon, closeRect.adjusted(2, 2, -2, -2));
    }

    if (hasFocus() && focusVisible_) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(appearance_.focusOutline, appearance_.metrics.focusOutlineWidth,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      const qreal inset =
          appearance_.metrics.focusOutlineOffset + appearance_.metrics.focusOutlineWidth / 2.0;
      painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset),
                              appearance_.metrics.borderRadius, appearance_.metrics.borderRadius);
    }
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    const bool wasCloseHovered = closeHovered_;
    closeHovered_ = closable_ && closeButtonRect().contains(event->position().toPoint());
    if (wasCloseHovered != closeHovered_) {
      update();
    }
    QAbstractButton::mouseMoveEvent(event);
  }

  void leaveEvent(QEvent* event) override {
    closeHovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && closable_ &&
        closeButtonRect().contains(event->position().toPoint())) {
      closePressed_ = true;
      event->accept();
      update();
      return;
    }
    QAbstractButton::mousePressEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (closePressed_) {
      const bool requestClose = event->button() == Qt::LeftButton && closable_ &&
                                closeButtonRect().contains(event->position().toPoint());
      closePressed_ = false;
      event->accept();
      update();
      if (requestClose && close) {
        close(index_);
      }
      return;
    }
    QAbstractButton::mouseReleaseEvent(event);
  }

  void keyPressEvent(QKeyEvent* event) override {
    int delta = 0;
    if (isHorizontal(placement_)) {
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
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && closable_) {
      if (close) {
        close(index_);
      }
      event->accept();
      return;
    }
    QAbstractButton::keyPressEvent(event);
  }

  void focusInEvent(QFocusEvent* event) override {
    focusVisible_ = keyboardFocusReason(event->reason());
    update();
    QAbstractButton::focusInEvent(event);
  }

  void focusOutEvent(QFocusEvent* event) override {
    focusVisible_ = false;
    update();
    QAbstractButton::focusOutEvent(event);
  }

  void resizeEvent(QResizeEvent* event) override {
    QAbstractButton::resizeEvent(event);
    updateElisionTooltip();
  }

  void nextCheckState() override {
    if (activate) {
      activate(index_);
    }
  }

 private:
  int closeButtonExtent() const { return appearance_.metrics.iconSize + 4; }

  void updateElisionTooltip() {
    const int iconWidth = adqt::icons::isValid(icon_) ? appearance_.metrics.iconSize : 0;
    const int iconGap = iconWidth > 0 && !text().isEmpty() ? appearance_.metrics.iconGap : 0;
    QRect contentRect = rect().adjusted(appearance_.metrics.horizontalPadding, 0,
                                        -appearance_.metrics.horizontalPadding, 0);
    if (closable_) {
      const QRect closeRect = closeButtonRect();
      if (layoutDirection() == Qt::RightToLeft) {
        contentRect.setLeft(closeRect.right() + appearance_.metrics.iconGap);
      } else {
        contentRect.setRight(closeRect.left() - appearance_.metrics.iconGap);
      }
    }
    const int availableTextWidth = std::max(0, contentRect.width() - iconWidth - iconGap);
    setToolTip(detail::singleLineTextAdvance(appearance_.metrics.font, text()) > availableTextWidth
                   ? text()
                   : QString());
  }

  QRect closeButtonRect() const {
    const int side = closeButtonExtent();
    const int y = (height() - side) / 2;
    if (layoutDirection() == Qt::RightToLeft) {
      return QRect(appearance_.metrics.horizontalPadding, y, side, side);
    }
    return QRect(width() - appearance_.metrics.horizontalPadding - side, y, side, side);
  }

  int index_ = -1;
  QString key_;
  adqt::icons::IconRef icon_;
  bool selected_ = false;
  bool closable_ = false;
  bool closeHovered_ = false;
  bool closePressed_ = false;
  bool focusVisible_ = false;
  AdTabs::Type type_ = AdTabs::Type::Line;
  AdTabs::Placement placement_ = AdTabs::Placement::Top;
  AdTabs::IndicatorAlignment indicatorAlignment_ = AdTabs::IndicatorAlignment::Fill;
  int indicatorSize_ = -1;
  TabsAppearance appearance_;
};

class OperationButton final : public QAbstractButton {
 public:
  enum class Kind : std::uint8_t { More, Add };

  explicit OperationButton(Kind kind, QWidget* parent = nullptr)
      : QAbstractButton(parent), kind_(kind) {
    setObjectName(kind == Kind::More ? QStringLiteral("ad-tabs-more")
                                     : QStringLiteral("ad-tabs-add"));
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
    setToolTip(kind == Kind::More ? tr("More tabs") : tr("Add tab"));
    setAccessibleName(toolTip());
  }

  void setAppearance(const TabsAppearance& value) {
    appearance_ = value;
    setFixedSize(appearance_.metrics.operationExtent, appearance_.metrics.operationExtent);
    setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor color = isEnabled() ? appearance_.item : appearance_.disabled;
    if (isEnabled() && underMouse()) {
      color = appearance_.hover;
    }
    adqt::icons::IconRef icon = kind_ == Kind::More ? adqt::icons::antd::outlined::Ellipsis()
                                                    : adqt::icons::antd::outlined::Plus();
    icon = icon.withColors(icon.colors().withPrimary(color));
    const int side = appearance_.metrics.iconSize;
    adqt::icons::paintIcon(&painter, icon,
                           QRect((width() - side) / 2, (height() - side) / 2, side, side));
    if (hasFocus()) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(appearance_.focusOutline, appearance_.metrics.focusOutlineWidth));
      painter.drawRoundedRect(QRectF(rect()).adjusted(3, 3, -3, -3),
                              appearance_.metrics.borderRadius, appearance_.metrics.borderRadius);
    }
  }

 private:
  Kind kind_;
  TabsAppearance appearance_;
};

class IndicatorWidget final : public QWidget {
 public:
  explicit IndicatorWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    hide();
  }

  void setColor(const QColor& color) {
    color_ = color;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), color_);
  }

 private:
  QColor color_;
};

class TabsStrip final : public QWidget {
 public:
  explicit TabsStrip(QWidget* parent = nullptr)
      : QWidget(parent),
        moreButton_(new OperationButton(OperationButton::Kind::More, this)),
        addButton_(new OperationButton(OperationButton::Kind::Add, this)),
        indicator_(new IndicatorWidget(this)),
        indicatorAnimation_(new QVariantAnimation(this)) {
    setObjectName(QStringLiteral("ad-tabs-strip"));
    indicator_->setObjectName(QStringLiteral("ad-tabs-indicator"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    moreButton_->hide();
    addButton_->hide();
    connect(moreButton_, &QAbstractButton::clicked, this, [this] { showOverflowMenu(); });
    connect(addButton_, &QAbstractButton::clicked, this, [this] {
      if (addRequested) {
        addRequested();
      }
    });
    indicatorAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(indicatorAnimation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
              indicatorRect_ = value.toRectF();
              applyIndicatorGeometry();
            });
  }

  void sync(const QList<AdTabs::TabItem>& items, int currentIndex, AdTabs::Type type,
            AdTabs::Placement placement, bool centered, bool animated, bool hideAdd,
            AdTabs::IndicatorAlignment indicatorAlignment, int indicatorSize,
            const TabsAppearance& appearance) {
    items_ = items;
    const bool selectionChanged = currentIndex_ != currentIndex;
    const QRectF previousIndicatorRect = indicatorRect_;
    currentIndex_ = currentIndex;
    type_ = type;
    placement_ = placement;
    centered_ = centered;
    animated_ = animated;
    hideAdd_ = hideAdd;
    indicatorAlignment_ = indicatorAlignment;
    indicatorSize_ = indicatorSize;
    appearance_ = appearance;
    indicator_->setColor(appearance.inkBar);

    while (buttons_.size() < items.size()) {
      auto* button = new TabButton(this);
      button->activate = [this](int index) {
        if (activate) {
          activate(index);
        }
      };
      button->close = [this](int index) {
        if (closeRequested) {
          closeRequested(index);
        }
      };
      button->navigate = [this](int index, int delta) { navigateFrom(index, delta); };
      buttons_.append(button);
    }
    while (buttons_.size() > items.size()) {
      delete buttons_.takeLast();
    }
    bool assignedFallbackFocus = false;
    for (int index = 0; index < buttons_.size(); ++index) {
      const bool focusable =
          index == currentIndex ||
          (currentIndex < 0 && items.at(index).enabled && !assignedFallbackFocus);
      assignedFallbackFocus = assignedFallbackFocus || focusable;
      buttons_.at(index)->configure(index, items.at(index), index == currentIndex, focusable, type,
                                    placement, indicatorAlignment, indicatorSize, appearance);
    }
    moreButton_->setAppearance(appearance);
    addButton_->setAppearance(appearance);
    const bool horizontal = isHorizontal(placement_);
    setSizePolicy(horizontal ? QSizePolicy::Expanding : QSizePolicy::Fixed,
                  horizontal ? QSizePolicy::Fixed : QSizePolicy::Expanding);
    updateGeometry();
    layoutChildren();
    if (selectionChanged && previousIndicatorRect.isValid()) {
      indicatorRect_ = previousIndicatorRect;
    }
    updateIndicator(selectionChanged);
    update();
  }

  void setExtraStart(QWidget* widget) {
    if (extraStart_ == widget) {
      return;
    }
    if (widget && (widget == this || (isAncestorOf(widget) && widget != extraEnd_))) {
      return;
    }
    QObject::disconnect(extraStartDestroyed_);
    if (extraStart_) {
      extraStart_->hide();
      extraStart_->setParent(nullptr);
    }
    if (widget == extraEnd_) {
      QObject::disconnect(extraEndDestroyed_);
      extraEnd_.clear();
    }
    extraStart_ = widget;
    if (extraStart_) {
      extraStart_->setParent(this);
      extraStart_->show();
      extraStartDestroyed_ = connect(
          extraStart_, &QObject::destroyed, this,
          [this] {
            extraStartDestroyed_ = {};
            updateGeometry();
            layoutChildren();
          },
          Qt::QueuedConnection);
    }
    updateGeometry();
    layoutChildren();
  }

  void setExtraEnd(QWidget* widget) {
    if (extraEnd_ == widget) {
      return;
    }
    if (widget && (widget == this || (isAncestorOf(widget) && widget != extraStart_))) {
      return;
    }
    QObject::disconnect(extraEndDestroyed_);
    if (extraEnd_) {
      extraEnd_->hide();
      extraEnd_->setParent(nullptr);
    }
    if (widget == extraStart_) {
      QObject::disconnect(extraStartDestroyed_);
      extraStart_.clear();
    }
    extraEnd_ = widget;
    if (extraEnd_) {
      extraEnd_->setParent(this);
      extraEnd_->show();
      extraEndDestroyed_ = connect(
          extraEnd_, &QObject::destroyed, this,
          [this] {
            extraEndDestroyed_ = {};
            updateGeometry();
            layoutChildren();
          },
          Qt::QueuedConnection);
    }
    updateGeometry();
    layoutChildren();
  }

  QWidget* extraStart() const { return extraStart_; }
  QWidget* extraEnd() const { return extraEnd_; }

  QSize sizeHint() const override {
    const bool horizontal = isHorizontal(placement_);
    const int gutter = type_ == AdTabs::Type::Line ? appearance_.metrics.itemGutter
                                                   : appearance_.metrics.cardGutter;
    int primary = 0;
    int cross = appearance_.metrics.itemHeight;
    for (int index = 0; index < buttons_.size(); ++index) {
      const QSize hint = buttons_.at(index)->sizeHint();
      primary += horizontal ? hint.width() : hint.height();
      cross = std::max(cross, horizontal ? hint.height() : hint.width());
      if (index > 0) {
        primary += gutter;
      }
    }
    addWidgetHint(extraStart_, horizontal, &primary, &cross);
    addWidgetHint(extraEnd_, horizontal, &primary, &cross);
    if (type_ == AdTabs::Type::EditableCard && !hideAdd_) {
      primary += appearance_.metrics.operationExtent;
      cross = std::max(cross, appearance_.metrics.operationExtent);
    }
    return horizontal ? QSize(primary, cross) : QSize(cross, primary);
  }

  QSize minimumSizeHint() const override {
    const bool horizontal = isHorizontal(placement_);
    int primary = 0;
    int cross = appearance_.metrics.itemHeight;
    TabButton* representative = currentIndex_ >= 0 && currentIndex_ < buttons_.size()
                                    ? buttons_.at(currentIndex_)
                                    : (buttons_.isEmpty() ? nullptr : buttons_.first());
    if (representative) {
      const QSize hint = representative->minimumSizeHint().expandedTo(
          QSize(appearance_.metrics.operationExtent, appearance_.metrics.itemHeight));
      primary += horizontal ? hint.width() : hint.height();
      cross = std::max(cross, horizontal ? hint.height() : hint.width());
    }
    if (buttons_.size() > 1) {
      primary += appearance_.metrics.operationExtent;
    }
    if (type_ == AdTabs::Type::EditableCard && !hideAdd_) {
      primary += appearance_.metrics.operationExtent;
    }
    addWidgetHint(extraStart_, horizontal, &primary, &cross, true);
    addWidgetHint(extraEnd_, horizontal, &primary, &cross, true);
    return horizontal ? QSize(primary, cross) : QSize(cross, primary);
  }

  std::function<void(int)> activate;
  std::function<void(int)> closeRequested;
  std::function<void()> addRequested;

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    layoutChildren();
  }

  void paintEvent(QPaintEvent* event) override {
    QWidget::paintEvent(event);
    QPainter painter(this);
    const int border = appearance_.metrics.borderWidth;
    painter.setPen(QPen(appearance_.border, border));
    if (isHorizontal(placement_)) {
      const qreal y = placement_ == AdTabs::Placement::Top ? height() - border / 2.0 : border / 2.0;
      painter.drawLine(QPointF(0, y), QPointF(width(), y));
    } else {
      const bool onRight =
          (placement_ == AdTabs::Placement::Start) != (layoutDirection() == Qt::RightToLeft);
      const qreal x = onRight ? width() - border / 2.0 : border / 2.0;
      painter.drawLine(QPointF(x, 0), QPointF(x, height()));
    }
  }

 private:
  static void addWidgetHint(const QWidget* widget, bool horizontal, int* primary, int* cross,
                            bool minimum = false) {
    if (!widget) {
      return;
    }
    QSize hint = minimum ? widget->minimumSizeHint() : widget->sizeHint();
    if (!hint.isValid()) {
      hint = widget->sizeHint();
    }
    *primary += horizontal ? hint.width() : hint.height();
    *cross = std::max(*cross, horizontal ? hint.height() : hint.width());
  }

  int itemExtent(TabButton* button) const {
    return isHorizontal(placement_) ? button->sizeHint().width() : button->sizeHint().height();
  }

  int widgetExtent(QWidget* widget) const {
    if (!widget) {
      return 0;
    }
    return isHorizontal(placement_) ? widget->sizeHint().width() : widget->sizeHint().height();
  }

  QRect extentRect(int start, int extent, int crossExtent) const {
    QRect result = isHorizontal(placement_) ? QRect(start, 0, extent, crossExtent)
                                            : QRect(0, start, crossExtent, extent);
    if (isHorizontal(placement_) && layoutDirection() == Qt::RightToLeft) {
      result.moveLeft(width() - result.right() - 1);
    }
    return result;
  }

  void layoutChildren() {
    if (width() <= 0 || height() <= 0) {
      return;
    }
    const bool horizontal = isHorizontal(placement_);
    const int totalExtent = horizontal ? width() : height();
    const int crossExtent = horizontal ? height() : width();
    const int gutter = type_ == AdTabs::Type::Line ? appearance_.metrics.itemGutter
                                                   : appearance_.metrics.cardGutter;

    int start = 0;
    if (extraStart_) {
      const int extent = std::min(widgetExtent(extraStart_), totalExtent);
      extraStart_->setGeometry(extentRect(start, extent, crossExtent));
      extraStart_->show();
      start += extent;
    }

    int end = totalExtent;
    if (extraEnd_) {
      const int extent = std::min(widgetExtent(extraEnd_), std::max(0, end - start));
      end -= extent;
      extraEnd_->setGeometry(extentRect(end, extent, crossExtent));
      extraEnd_->show();
    }

    const bool showAdd = type_ == AdTabs::Type::EditableCard && !hideAdd_;
    addButton_->setVisible(showAdd);
    if (showAdd) {
      const int extent = std::min(appearance_.metrics.operationExtent, std::max(0, end - start));
      end -= extent;
      addButton_->setGeometry(extentRect(end, extent, crossExtent));
    }

    int naturalExtent = 0;
    for (int index = 0; index < buttons_.size(); ++index) {
      naturalExtent += itemExtent(buttons_.at(index));
      if (index > 0) {
        naturalExtent += gutter;
      }
    }
    int available = std::max(0, end - start);

    for (TabButton* button : buttons_) {
      button->hide();
    }
    hiddenIndexes_.clear();
    if (buttons_.isEmpty() || available <= 0) {
      moreButton_->hide();
      return;
    }

    if (horizontal) {
      QVector<int> naturalExtents;
      QVector<int> minimumExtents;
      naturalExtents.reserve(buttons_.size());
      minimumExtents.reserve(buttons_.size());
      for (TabButton* button : buttons_) {
        naturalExtents.append(button->sizeHint().width());
        minimumExtents.append(button->minimumSizeHint().width());
      }

      const int minimumExtent = std::accumulate(minimumExtents.cbegin(), minimumExtents.cend(), 0) +
                                gutter * std::max(0, static_cast<int>(buttons_.size()) - 1);
      QVector<int> visibleIndexes;
      bool overflow = minimumExtent > available;
      if (!overflow) {
        visibleIndexes.reserve(buttons_.size());
        for (int index = 0; index < buttons_.size(); ++index) {
          visibleIndexes.append(index);
        }
      } else {
        const int operationExtent = std::min(appearance_.metrics.operationExtent, available);
        end -= operationExtent;
        available = std::max(0, end - start);
        moreButton_->setGeometry(extentRect(end, operationExtent, crossExtent));

        QVector<int> candidates;
        candidates.reserve(buttons_.size());
        for (int index = 0; index < buttons_.size(); ++index) {
          if (index != currentIndex_) {
            candidates.append(index);
          }
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [&naturalExtents](int first, int second) {
                           return naturalExtents.at(first) < naturalExtents.at(second);
                         });

        int usedMinimum = 0;
        const auto appendIfFits = [&](int index, bool required) {
          if (index < 0 || index >= minimumExtents.size()) {
            return;
          }
          const int needed = minimumExtents.at(index) + (visibleIndexes.isEmpty() ? 0 : gutter);
          if (required || usedMinimum + needed <= available) {
            visibleIndexes.append(index);
            usedMinimum += needed;
          }
        };
        appendIfFits(currentIndex_, true);
        for (int index : std::as_const(candidates)) {
          appendIfFits(index, visibleIndexes.isEmpty());
        }
        std::sort(visibleIndexes.begin(), visibleIndexes.end());
      }

      moreButton_->setVisible(overflow);
      QVector<bool> visible(buttons_.size(), false);
      QVector<int> visibleNaturalExtents;
      QVector<int> visibleMinimumExtents;
      visibleNaturalExtents.reserve(visibleIndexes.size());
      visibleMinimumExtents.reserve(visibleIndexes.size());
      for (int index : std::as_const(visibleIndexes)) {
        visible[index] = true;
        visibleNaturalExtents.append(naturalExtents.at(index));
        visibleMinimumExtents.append(minimumExtents.at(index));
      }
      for (int index = 0; index < buttons_.size(); ++index) {
        if (!visible.at(index)) {
          hiddenIndexes_.append(index);
        }
      }

      const int gutterExtent = gutter * std::max(0, static_cast<int>(visibleIndexes.size()) - 1);
      const QVector<int> allocatedExtents = shrinkExtentsToFit(
          visibleNaturalExtents, visibleMinimumExtents, std::max(0, available - gutterExtent));
      const int usedExtent =
          std::accumulate(allocatedExtents.cbegin(), allocatedExtents.cend(), gutterExtent);
      int cursor = start;
      if (centered_ && !overflow) {
        cursor += std::max(0, (available - usedExtent) / 2);
      }
      for (int visiblePosition = 0; visiblePosition < visibleIndexes.size(); ++visiblePosition) {
        if (visiblePosition > 0) {
          cursor += gutter;
        }
        TabButton* button = buttons_.at(visibleIndexes.at(visiblePosition));
        const int extent =
            std::min(allocatedExtents.at(visiblePosition), std::max(0, end - cursor));
        button->setGeometry(extentRect(cursor, extent, crossExtent));
        button->show();
        cursor += extent;
      }
      if (indicatorAnimation_->state() != QAbstractAnimation::Running) {
        indicatorRect_ = targetIndicatorRect();
        applyIndicatorGeometry();
      }
      return;
    }

    const bool overflow = naturalExtent > available;
    moreButton_->setVisible(overflow);
    if (overflow) {
      const int extent = std::min(appearance_.metrics.operationExtent, available);
      end -= extent;
      available = std::max(0, end - start);
      moreButton_->setGeometry(extentRect(end, extent, crossExtent));
    }

    int first = 0;
    int last = static_cast<int>(buttons_.size()) - 1;
    if (overflow) {
      const int buttonCount = static_cast<int>(buttons_.size());
      first = std::clamp(scrollStart_, 0, std::max(0, buttonCount - 1));
      if (currentIndex_ >= 0) {
        first = std::min(first, currentIndex_);
        int required = 0;
        for (int i = first; i <= currentIndex_; ++i) {
          required += itemExtent(buttons_.at(i));
          if (i > first) {
            required += gutter;
          }
        }
        while (first < currentIndex_ && required > available) {
          required -= itemExtent(buttons_.at(first)) + gutter;
          ++first;
        }
      }
      last = first - 1;
      int used = 0;
      for (int i = first; i < buttons_.size(); ++i) {
        const int needed = itemExtent(buttons_.at(i)) + (i > first ? gutter : 0);
        if (used + needed > available && last >= first) {
          break;
        }
        if (needed > available && last < first) {
          last = i;
          break;
        }
        used += needed;
        last = i;
      }
      scrollStart_ = first;
    }

    int usedExtent = 0;
    for (int i = first; i <= last; ++i) {
      usedExtent += itemExtent(buttons_.at(i));
      if (i > first) {
        usedExtent += gutter;
      }
    }
    int cursor = start;
    if (centered_ && !overflow) {
      cursor += std::max(0, (available - usedExtent) / 2);
    }
    for (int i = 0; i < buttons_.size(); ++i) {
      if (i < first || i > last) {
        hiddenIndexes_.append(i);
        continue;
      }
      if (i > first) {
        cursor += gutter;
      }
      TabButton* button = buttons_.at(i);
      const int extent = std::min(itemExtent(button), std::max(0, end - cursor));
      button->setGeometry(extentRect(cursor, extent, crossExtent));
      button->show();
      cursor += extent;
    }
    if (indicatorAnimation_->state() != QAbstractAnimation::Running) {
      indicatorRect_ = targetIndicatorRect();
      applyIndicatorGeometry();
    }
  }

  void applyIndicatorGeometry() {
    const bool visible = type_ == AdTabs::Type::Line && indicatorRect_.isValid() &&
                         indicatorRect_.width() > 0.0 && indicatorRect_.height() > 0.0;
    indicator_->setVisible(visible);
    if (!visible) {
      return;
    }
    indicator_->setGeometry(indicatorRect_.toAlignedRect());
    indicator_->raise();
  }

  QRectF targetIndicatorRect() const {
    if (type_ != AdTabs::Type::Line || currentIndex_ < 0 || currentIndex_ >= buttons_.size() ||
        buttons_.at(currentIndex_)->isHidden()) {
      return QRectF();
    }
    const QRect buttonRect = buttons_.at(currentIndex_)->geometry();
    const int thickness = std::max(1, appearance_.metrics.indicatorThickness);
    const int naturalLength = isHorizontal(placement_) ? buttonRect.width() : buttonRect.height();
    int length = indicatorSize_ > 0 ? std::min(indicatorSize_, naturalLength) : naturalLength;
    if (indicatorAlignment_ == AdTabs::IndicatorAlignment::Fill) {
      length = naturalLength;
    }
    int offset = 0;
    if (indicatorAlignment_ == AdTabs::IndicatorAlignment::Center) {
      offset = (naturalLength - length) / 2;
    } else if (indicatorAlignment_ == AdTabs::IndicatorAlignment::End) {
      offset = naturalLength - length;
    }
    if (isHorizontal(placement_) && layoutDirection() == Qt::RightToLeft &&
        indicatorAlignment_ != AdTabs::IndicatorAlignment::Center &&
        indicatorAlignment_ != AdTabs::IndicatorAlignment::Fill) {
      offset = naturalLength - length - offset;
    }
    if (isHorizontal(placement_)) {
      return QRectF(buttonRect.x() + offset,
                    placement_ == AdTabs::Placement::Top ? height() - thickness : 0, length,
                    thickness);
    }
    const bool onRight =
        (placement_ == AdTabs::Placement::Start) != (layoutDirection() == Qt::RightToLeft);
    return QRectF(onRight ? width() - thickness : 0, buttonRect.y() + offset, thickness, length);
  }

  void updateIndicator(bool selectionChanged) {
    const QRectF target = targetIndicatorRect();
    indicatorAnimation_->stop();
    if (!animated_ || !selectionChanged || !indicatorRect_.isValid() || !target.isValid() ||
        appearance_.motionDuration <= 0) {
      indicatorRect_ = target;
      applyIndicatorGeometry();
      return;
    }
    indicatorAnimation_->setDuration(appearance_.motionDuration);
    indicatorAnimation_->setStartValue(indicatorRect_);
    indicatorAnimation_->setEndValue(target);
    indicatorAnimation_->start();
  }

  void navigateFrom(int index, int delta) {
    if (buttons_.isEmpty()) {
      return;
    }
    const int buttonCount = static_cast<int>(buttons_.size());
    int target = index;
    if (delta <= -1000000) {
      target = -1;
      delta = 1;
    } else if (delta >= 1000000) {
      target = buttonCount;
      delta = -1;
    }
    for (int attempts = 0; attempts < buttonCount; ++attempts) {
      target = (target + delta + buttonCount) % buttonCount;
      if (buttons_.at(target)->isEnabled()) {
        scrollStart_ = std::min(scrollStart_, target);
        if (activate) {
          activate(target);
        }
        buttons_.at(target)->setFocus(Qt::TabFocusReason);
        return;
      }
    }
  }

  void showOverflowMenu() {
    QMenu menu(this);
    for (int index : std::as_const(hiddenIndexes_)) {
      if (index < 0 || index >= items_.size()) {
        continue;
      }
      const AdTabs::TabItem& item = items_.at(index);
      QAction* action = menu.addAction(item.label);
      action->setEnabled(item.enabled);
      action->setCheckable(true);
      action->setChecked(index == currentIndex_);
      connect(action, &QAction::triggered, &menu, [this, index] {
        scrollStart_ = index;
        if (activate) {
          activate(index);
        }
      });
    }
    if (menu.actions().isEmpty()) {
      return;
    }
    QPoint popupPoint;
    if (placement_ == AdTabs::Placement::Bottom) {
      popupPoint = moreButton_->mapToGlobal(QPoint(0, -menu.sizeHint().height()));
    } else if (!isHorizontal(placement_)) {
      const bool stripOnLeft =
          (placement_ == AdTabs::Placement::Start) != (layoutDirection() == Qt::RightToLeft);
      popupPoint = moreButton_->mapToGlobal(
          QPoint(stripOnLeft ? moreButton_->width() : -menu.sizeHint().width(), 0));
    } else {
      popupPoint = moreButton_->mapToGlobal(QPoint(0, moreButton_->height()));
    }
    menu.exec(popupPoint);
  }

  QList<AdTabs::TabItem> items_;
  QList<TabButton*> buttons_;
  QList<int> hiddenIndexes_;
  int currentIndex_ = -1;
  int scrollStart_ = 0;
  AdTabs::Type type_ = AdTabs::Type::Line;
  AdTabs::Placement placement_ = AdTabs::Placement::Top;
  bool centered_ = false;
  bool animated_ = true;
  bool hideAdd_ = false;
  AdTabs::IndicatorAlignment indicatorAlignment_ = AdTabs::IndicatorAlignment::Fill;
  int indicatorSize_ = -1;
  TabsAppearance appearance_;
  OperationButton* moreButton_ = nullptr;
  OperationButton* addButton_ = nullptr;
  IndicatorWidget* indicator_ = nullptr;
  QVariantAnimation* indicatorAnimation_ = nullptr;
  QRectF indicatorRect_;
  QPointer<QWidget> extraStart_;
  QPointer<QWidget> extraEnd_;
  QMetaObject::Connection extraStartDestroyed_;
  QMetaObject::Connection extraEndDestroyed_;
};

}  // namespace

struct AdTabs::Private {
  explicit Private(AdTabs* owner) : q(owner) {}

  ComponentTokens resolvedTokens() const {
    ComponentTokens result = componentTokens;
    if (tokenResolver) {
      ComponentTokenContext context;
      context.type = type;
      context.controlSize = controlSize;
      context.placement = placement;
      context.enabled = q->isEnabled();
      result = mergeTokens(result, tokenResolver(context));
    }
    return result;
  }

  void refreshStrip() {
    appearance = detail::resolveTabsAppearance(q, resolvedTokens());
    strip->sync(items, currentIndex, type, placement, centered, animated, hideAdd,
                indicatorAlignment, indicatorSize, appearance);
    q->updateGeometry();
  }

  void rebuildLayout() {
    layout->removeWidget(strip);
    layout->removeWidget(stack);
    if (placement == Placement::Top) {
      layout->setDirection(QBoxLayout::TopToBottom);
      layout->addWidget(strip);
      layout->addWidget(stack, 1);
    } else if (placement == Placement::Bottom) {
      layout->setDirection(QBoxLayout::TopToBottom);
      layout->addWidget(stack, 1);
      layout->addWidget(strip);
    } else {
      layout->setDirection(QBoxLayout::LeftToRight);
      if (placement == Placement::Start) {
        layout->addWidget(strip);
        layout->addWidget(stack, 1);
      } else {
        layout->addWidget(stack, 1);
        layout->addWidget(strip);
      }
    }
    layout->setSpacing(type == Type::Line ? 16 : 0);
    refreshStrip();
  }

  int firstEnabled(int from, int direction) const {
    if (items.isEmpty()) {
      return -1;
    }
    const int itemCount = static_cast<int>(items.size());
    int index = std::clamp(from, 0, itemCount - 1);
    for (int attempt = 0; attempt < items.size(); ++attempt) {
      if (items.at(index).enabled) {
        return index;
      }
      index += direction;
      if (index < 0 || index >= items.size()) {
        break;
      }
    }
    return -1;
  }

  bool canUseAsExtra(QWidget* widget) const {
    return !widget || (widget != q && widget != stack && widget != strip &&
                       !widget->isAncestorOf(q) && !stack->isAncestorOf(widget));
  }

  QWidget* detachTab(int index, bool pageDestroyed) {
    if (index < 0 || index >= items.size()) {
      return nullptr;
    }
    const int previousIndex = currentIndex;
    const QString previousKey = q->currentKey();
    QPointer<QWidget> page = items.at(index).page;
    const QString key = items.at(index).key;
    items.removeAt(index);
    const auto connection = pageDestroyedConnections.take(key);
    if (!pageDestroyed && connection) {
      QObject::disconnect(connection);
    }
    if (!pageDestroyed && page) {
      stack->removeWidget(page);
      page->setParent(nullptr);
    }

    if (items.isEmpty()) {
      currentIndex = -1;
    } else if (index < currentIndex) {
      --currentIndex;
    } else if (index == currentIndex) {
      const int remainingCount = static_cast<int>(items.size());
      int replacement = firstEnabled(std::min(index, remainingCount - 1), 1);
      if (replacement < 0) {
        replacement = firstEnabled(std::min(index - 1, remainingCount - 1), -1);
      }
      currentIndex = replacement;
    }
    stack->setCurrentIndex(currentIndex);
    refreshStrip();
    if (previousIndex != currentIndex) {
      emit q->currentIndexChanged(currentIndex);
    }
    if (previousKey != q->currentKey()) {
      emit q->currentKeyChanged(q->currentKey());
    }
    return page.data();
  }

  AdTabs* q = nullptr;
  QBoxLayout* layout = nullptr;
  TabsStrip* strip = nullptr;
  QStackedWidget* stack = nullptr;
  QList<TabItem> items;
  int currentIndex = -1;
  Type type = Type::Line;
  ControlSize controlSize = ControlSize::Medium;
  Placement placement = Placement::Top;
  bool centered = false;
  bool animated = true;
  bool hideAdd = false;
  int tabBarGutter = -1;
  int indicatorSize = -1;
  IndicatorAlignment indicatorAlignment = IndicatorAlignment::Fill;
  ComponentTokens componentTokens;
  ComponentTokenResolver tokenResolver;
  QHash<QString, QMetaObject::Connection> pageDestroyedConnections;
  TabsAppearance appearance;
};

AdTabs::AdTabs(QWidget* parent) : QWidget(parent), d_(std::make_unique<Private>(this)) {
  d_->layout = new QBoxLayout(QBoxLayout::TopToBottom, this);
  d_->layout->setContentsMargins(0, 0, 0, 0);
  d_->strip = new TabsStrip(this);
  d_->stack = new QStackedWidget(this);
  d_->stack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  d_->strip->activate = [this](int index) {
    if (index < 0 || index >= d_->items.size()) {
      return;
    }
    emit tabClicked(d_->items.at(index).key);
    setCurrentIndex(index);
  };
  d_->strip->closeRequested = [this](int index) {
    if (index >= 0 && index < d_->items.size() && d_->items.at(index).closable) {
      emit tabCloseRequested(d_->items.at(index).key);
    }
  };
  d_->strip->addRequested = [this] { emit addRequested(); };
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this] {
            d_->refreshStrip();
            updateGeometry();
          });
  d_->rebuildLayout();
}

AdTabs::~AdTabs() {
  for (const QMetaObject::Connection& connection : std::as_const(d_->pageDestroyedConnections)) {
    disconnect(connection);
  }
}

int AdTabs::count() const { return static_cast<int>(d_->items.size()); }
int AdTabs::currentIndex() const { return d_->currentIndex; }
QString AdTabs::currentKey() const {
  return d_->currentIndex >= 0 && d_->currentIndex < d_->items.size()
             ? d_->items.at(d_->currentIndex).key
             : QString();
}

int AdTabs::addTab(const QString& key, const QString& label, QWidget* page) {
  TabItem item;
  item.key = key;
  item.label = label;
  item.page = page;
  return addTab(item);
}

int AdTabs::addTab(const TabItem& item) { return insertTab(count(), item); }

int AdTabs::insertTab(int index, const TabItem& source) {
  TabItem item = source;
  if (item.key.isEmpty() || indexOf(item.key) >= 0) {
    return -1;
  }
  QWidget* suppliedPage = item.page.data();
  if (suppliedPage && (indexOf(suppliedPage) >= 0 || suppliedPage == this ||
                       suppliedPage == d_->stack || suppliedPage == d_->strip ||
                       suppliedPage->isAncestorOf(this) || d_->strip->isAncestorOf(suppliedPage))) {
    return -1;
  }
  index = std::clamp(index, 0, static_cast<int>(d_->items.size()));
  if (!item.page) {
    item.page = new QWidget;
  }
  item.page->setParent(d_->stack);
  d_->items.insert(index, item);
  d_->stack->insertWidget(index, item.page);
  const QString insertedKey = item.key;
  d_->pageDestroyedConnections.insert(
      insertedKey, connect(
                       item.page, &QObject::destroyed, this,
                       [this, insertedKey] {
                         const int destroyedIndex = indexOf(insertedKey);
                         if (destroyedIndex >= 0 && d_->items.at(destroyedIndex).page.isNull()) {
                           d_->detachTab(destroyedIndex, true);
                         }
                       },
                       Qt::QueuedConnection));

  const int previousIndex = d_->currentIndex;
  const QString previousKey =
      previousIndex >= 0 && previousIndex < d_->items.size()
          ? d_->items.at(previousIndex + (index <= previousIndex ? 1 : 0)).key
          : QString();
  if (d_->currentIndex < 0 && item.enabled) {
    d_->currentIndex = index;
  } else if (index <= d_->currentIndex) {
    ++d_->currentIndex;
  }
  if (d_->currentIndex >= 0) {
    d_->stack->setCurrentIndex(d_->currentIndex);
  }
  d_->refreshStrip();
  if (previousIndex != d_->currentIndex) {
    emit currentIndexChanged(d_->currentIndex);
  }
  if (previousKey != currentKey()) {
    emit currentKeyChanged(currentKey());
  }
  return index;
}

void AdTabs::removeTab(int index) {
  QWidget* page = takeTab(index);
  if (page) {
    page->deleteLater();
  }
}

void AdTabs::removeTab(const QString& key) { removeTab(indexOf(key)); }

QWidget* AdTabs::takeTab(int index) { return d_->detachTab(index, false); }

void AdTabs::clear() {
  while (!d_->items.isEmpty()) {
    removeTab(static_cast<int>(d_->items.size()) - 1);
  }
}

int AdTabs::indexOf(const QString& key) const {
  for (int index = 0; index < d_->items.size(); ++index) {
    if (d_->items.at(index).key == key) {
      return index;
    }
  }
  return -1;
}

int AdTabs::indexOf(const QWidget* page) const {
  for (int index = 0; index < d_->items.size(); ++index) {
    if (d_->items.at(index).page == page) {
      return index;
    }
  }
  return -1;
}

QString AdTabs::tabKey(int index) const {
  return index >= 0 && index < d_->items.size() ? d_->items.at(index).key : QString();
}
QString AdTabs::tabText(int index) const {
  return index >= 0 && index < d_->items.size() ? d_->items.at(index).label : QString();
}
void AdTabs::setTabText(int index, const QString& text) {
  if (index < 0 || index >= d_->items.size() || d_->items.at(index).label == text) {
    return;
  }
  d_->items[index].label = text;
  d_->refreshStrip();
}
adqt::icons::IconRef AdTabs::tabIcon(int index) const {
  return index >= 0 && index < d_->items.size() ? d_->items.at(index).icon : adqt::icons::IconRef{};
}
void AdTabs::setTabIcon(int index, const adqt::icons::IconRef& icon) {
  if (index < 0 || index >= d_->items.size() || d_->items.at(index).icon == icon) {
    return;
  }
  d_->items[index].icon = icon;
  d_->refreshStrip();
}
QWidget* AdTabs::widget(int index) const {
  return index >= 0 && index < d_->items.size() ? d_->items.at(index).page : nullptr;
}
QVariant AdTabs::tabData(int index) const {
  return index >= 0 && index < d_->items.size() ? d_->items.at(index).data : QVariant();
}
void AdTabs::setTabData(int index, const QVariant& value) {
  if (index >= 0 && index < d_->items.size()) {
    d_->items[index].data = value;
  }
}
bool AdTabs::isTabEnabled(int index) const {
  return index >= 0 && index < d_->items.size() && d_->items.at(index).enabled;
}
void AdTabs::setTabEnabled(int index, bool enabled) {
  if (index < 0 || index >= d_->items.size() || d_->items.at(index).enabled == enabled) {
    return;
  }
  d_->items[index].enabled = enabled;
  if (!enabled && index == d_->currentIndex) {
    int target = d_->firstEnabled(index + 1, 1);
    if (target < 0) {
      target = d_->firstEnabled(index - 1, -1);
    }
    setCurrentIndex(target);
  } else if (enabled && d_->currentIndex < 0) {
    setCurrentIndex(index);
  } else {
    d_->refreshStrip();
  }
}
bool AdTabs::isTabClosable(int index) const {
  return index >= 0 && index < d_->items.size() && d_->items.at(index).closable;
}
void AdTabs::setTabClosable(int index, bool closable) {
  if (index < 0 || index >= d_->items.size() || d_->items.at(index).closable == closable) {
    return;
  }
  d_->items[index].closable = closable;
  d_->refreshStrip();
}

AdTabs::Type AdTabs::type() const { return d_->type; }
void AdTabs::setType(Type value) {
  if (d_->type == value) return;
  d_->type = value;
  d_->rebuildLayout();
  emit typeChanged(value);
}
AdTabs::ControlSize AdTabs::controlSize() const { return d_->controlSize; }
void AdTabs::setControlSize(ControlSize value) {
  if (d_->controlSize == value) return;
  d_->controlSize = value;
  d_->refreshStrip();
  emit controlSizeChanged(value);
}
AdTabs::Placement AdTabs::tabPlacement() const { return d_->placement; }
void AdTabs::setTabPlacement(Placement value) {
  if (d_->placement == value) return;
  d_->placement = value;
  d_->rebuildLayout();
  emit tabPlacementChanged(value);
}
bool AdTabs::centered() const { return d_->centered; }
void AdTabs::setCentered(bool value) {
  if (d_->centered == value) return;
  d_->centered = value;
  d_->refreshStrip();
  emit centeredChanged(value);
}
bool AdTabs::animated() const { return d_->animated; }
void AdTabs::setAnimated(bool value) {
  if (d_->animated == value) return;
  d_->animated = value;
  d_->refreshStrip();
  emit animatedChanged(value);
}
bool AdTabs::hideAdd() const { return d_->hideAdd; }
void AdTabs::setHideAdd(bool value) {
  if (d_->hideAdd == value) return;
  d_->hideAdd = value;
  d_->refreshStrip();
  emit hideAddChanged(value);
}
int AdTabs::tabBarGutter() const { return d_->tabBarGutter; }
void AdTabs::setTabBarGutter(int value) {
  value = std::max(-1, value);
  if (d_->tabBarGutter == value) return;
  d_->tabBarGutter = value;
  d_->refreshStrip();
  emit tabBarGutterChanged(value);
}
int AdTabs::indicatorSize() const { return d_->indicatorSize; }
void AdTabs::setIndicatorSize(int value) {
  value = std::max(-1, value);
  if (d_->indicatorSize == value) return;
  d_->indicatorSize = value;
  d_->refreshStrip();
  emit indicatorChanged();
}
AdTabs::IndicatorAlignment AdTabs::indicatorAlignment() const { return d_->indicatorAlignment; }
void AdTabs::setIndicatorAlignment(IndicatorAlignment value) {
  if (d_->indicatorAlignment == value) return;
  d_->indicatorAlignment = value;
  d_->refreshStrip();
  emit indicatorChanged();
}

QWidget* AdTabs::tabBarExtraContentStart() const { return d_->strip->extraStart(); }
void AdTabs::setTabBarExtraContentStart(QWidget* widget) {
  if (d_->canUseAsExtra(widget)) {
    d_->strip->setExtraStart(widget);
  }
}
QWidget* AdTabs::tabBarExtraContentEnd() const { return d_->strip->extraEnd(); }
void AdTabs::setTabBarExtraContentEnd(QWidget* widget) {
  if (d_->canUseAsExtra(widget)) {
    d_->strip->setExtraEnd(widget);
  }
}

AdTabs::ComponentTokens AdTabs::componentTokens() const { return d_->componentTokens; }
void AdTabs::setComponentTokens(const ComponentTokens& value) {
  d_->componentTokens = value;
  d_->refreshStrip();
  emit componentTokensChanged();
}
void AdTabs::resetComponentTokens() { setComponentTokens({}); }
void AdTabs::setComponentTokenResolver(ComponentTokenResolver resolver) {
  d_->tokenResolver = std::move(resolver);
  d_->refreshStrip();
  emit componentTokensChanged();
}
void AdTabs::resetComponentTokenResolver() { setComponentTokenResolver({}); }

void AdTabs::setCurrentIndex(int index) {
  if (index < -1 || index >= d_->items.size()) {
    return;
  }
  if (index >= 0 && !d_->items.at(index).enabled) {
    return;
  }
  if (d_->currentIndex == index) {
    return;
  }
  const QString previousKey = currentKey();
  d_->currentIndex = index;
  d_->stack->setCurrentIndex(index);
  d_->refreshStrip();
  emit currentIndexChanged(index);
  if (previousKey != currentKey()) {
    emit currentKeyChanged(currentKey());
  }
}

void AdTabs::setCurrentKey(const QString& key) {
  const int index = indexOf(key);
  if (index >= 0) {
    setCurrentIndex(index);
  }
}

void AdTabs::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  switch (event->type()) {
    case QEvent::EnabledChange:
    case QEvent::FontChange:
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
      d_->refreshStrip();
      break;
    case QEvent::LayoutDirectionChange:
      d_->rebuildLayout();
      break;
    default:
      break;
  }
}

}  // namespace adqt::widgets
