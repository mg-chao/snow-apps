#include "tabs.h"

#include "detail/text_metrics.h"
#include "popover.h"
#include "scroll_area.h"
#include "tabs_style.h"
#include "theme/theme.h"

#include "antd_icons.h"

#include <QAbstractButton>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QHash>
#include <QKeyEvent>
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QStackedWidget>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cstdint>
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

// A mouse-transparent overlay stays above the moving tab content, like Ant's
// inset overflow shadows. It indicates only the edges that can still be scrolled.
class TabScrollEdges final : public QWidget {
 public:
  explicit TabScrollEdges(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ad-tabs-scroll-edges"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
  }

  void configure(bool horizontal, bool before, bool after) {
    horizontal_ = horizontal;
    before_ = before;
    after_ = after;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    const int extent = std::min(10, (horizontal_ ? width() : height()) / 2);
    for (const bool before : {true, false}) {
      if (!(before ? before_ : after_)) {
        continue;
      }
      const QPointF edge =
          horizontal_ ? QPointF(before ? 0 : width(), 0) : QPointF(0, before ? 0 : height());
      const QPointF inside = edge + (horizontal_ ? QPointF(before ? extent : -extent, 0)
                                                 : QPointF(0, before ? extent : -extent));
      QLinearGradient gradient(edge, inside);
      gradient.setColorAt(0, QColor(0, 0, 0, 20));
      gradient.setColorAt(1, Qt::transparent);
      const QRect area = horizontal_ ? QRect(before ? 0 : width() - extent, 0, extent, height())
                                     : QRect(0, before ? 0 : height() - extent, width(), extent);
      painter.fillRect(area, gradient);
    }
  }

 private:
  bool horizontal_ = true;
  bool before_ = false;
  bool after_ = false;
};

// QScrollArea owns clipping, scroll ranges and RTL positioning. Tab visibility uses
// logical coordinates so selection and wheel input share the same scroll direction.
class TabScrollArea final : public QScrollArea {
 public:
  explicit TabScrollArea(QWidget* parent)
      : QScrollArea(parent), edges_(new TabScrollEdges(viewport())) {
    setObjectName(QStringLiteral("ad-tabs-scroll-area"));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::NoFocus);
    setAutoFillBackground(false);
    viewport()->setAutoFillBackground(false);
    QScroller::grabGesture(viewport(), QScroller::TouchGesture);
    for (QScrollBar* bar : {horizontalScrollBar(), verticalScrollBar()}) {
      connect(bar, &QScrollBar::rangeChanged, this, [this] { updateEdges(); });
    }
  }

  void setHorizontal(bool horizontal) {
    horizontal_ = horizontal;
    updateEdges();
  }

  void revealTab(QWidget* tab) {
    const QRect bounds(tab->mapTo(widget(), QPoint()), tab->size());
    const int start =
        horizontal_ ? (layoutDirection() == Qt::RightToLeft ? widget()->width() - bounds.right() - 1
                                                            : bounds.left())
                    : bounds.top();
    const int extent = horizontal_ ? bounds.width() : bounds.height();
    const int available = horizontal_ ? viewport()->width() : viewport()->height();
    QScrollBar* bar = horizontal_ ? horizontalScrollBar() : verticalScrollBar();
    if (start < bar->value() || extent > available) {
      bar->setValue(start);
    } else if (start + extent > bar->value() + available) {
      bar->setValue(start + extent - available);
    }
  }

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QScrollArea::resizeEvent(event);
    updateEdges();
  }

  void scrollContentsBy(int dx, int dy) override {
    QScrollArea::scrollContentsBy(dx, dy);
    updateEdges();
  }

  bool focusNextPrevChild(bool next) override {
    if (!QWidget::focusNextPrevChild(next)) {
      return false;
    }
    if (QWidget* focused = focusWidget(); focused && widget()->isAncestorOf(focused)) {
      revealTab(focused);
    }
    return true;
  }

  void wheelEvent(QWheelEvent* event) override {
    QScrollBar* bar = horizontal_ ? horizontalScrollBar() : verticalScrollBar();
    const bool rightToLeft = horizontal_ && layoutDirection() == Qt::RightToLeft;
    if (!event->pixelDelta().isNull()) {
      // Pixel deltas already include the platform's natural scrolling preference.
      const QPoint delta = event->pixelDelta();
      const int distance =
          horizontal_ && delta.x() != 0 ? delta.x() * (rightToLeft ? -1 : 1) : delta.y();
      const int previous = bar->value();
      bar->setValue(previous - distance);
      event->setAccepted(bar->value() != previous);
      return;
    }

    QPoint delta = event->angleDelta();
    if (horizontal_ && delta.x() != 0) {
      delta = QPoint(delta.x() * (rightToLeft ? -1 : 1), 0);
    }
    // Let QScrollBar apply native wheel steps and accumulate high-resolution deltas.
    QWheelEvent forwarded(event->position(), event->globalPosition(), QPoint(), delta,
                          event->buttons(), event->modifiers(), event->phase(), event->inverted(),
                          event->source());
    QCoreApplication::sendEvent(bar, &forwarded);
    event->setAccepted(forwarded.isAccepted());
  }

 private:
  void updateEdges() {
    QScrollBar* bar = horizontal_ ? horizontalScrollBar() : verticalScrollBar();
    bool before = bar->value() > bar->minimum();
    bool after = bar->value() < bar->maximum();
    if (horizontal_ && layoutDirection() == Qt::RightToLeft) {
      std::swap(before, after);
    }
    edges_->setGeometry(viewport()->rect());
    edges_->configure(horizontal_, before, after);
    edges_->raise();
  }

  bool horizontal_ = true;
  TabScrollEdges* edges_;
};

// A native item view supplies list accessibility, keyboard navigation and scrolling;
// the delegate paints Tabs' dropdown rows without native menu/checkmark decoration.
class TabOverflowList final : public QListWidget {
 public:
  enum Role { KeyRole = Qt::UserRole, ClosableRole };

  explicit TabOverflowList(QWidget* parent = nullptr) : QListWidget(parent) {
    setObjectName(QStringLiteral("ad-tabs-overflow-list"));
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setItemDelegate(new Delegate(this));
    setAutoFillBackground(false);
    viewport()->setAutoFillBackground(false);
    AdScrollArea::applyThemedScrollBar(verticalScrollBar());
    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
      if (item && item->flags().testFlag(Qt::ItemIsEnabled) && choose) {
        choose(item->data(KeyRole).toString());
      }
    });
  }

  void configure(const QList<AdTabs::TabItem>& items, bool editable,
                 const TabsAppearance& appearance) {
    const QString currentKey = currentItem() ? currentItem()->data(KeyRole).toString() : QString();
    appearance_ = appearance;
    setFont(appearance.popupFont);
    clear();
    widthHint_ = 120;
    for (const auto& tab : items) {
      auto* row = new QListWidgetItem(tab.label, this);
      row->setData(KeyRole, tab.key);
      row->setData(ClosableRole, editable && tab.closable && tab.enabled);
      row->setData(Qt::AccessibleTextRole, tab.label.isEmpty() ? tab.key : tab.label);
      row->setFlags(tab.enabled ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
      const int closeWidth = row->data(ClosableRole).toBool()
                                 ? appearance.popupCloseSize + appearance.popupHorizontalPadding
                                 : 0;
      widthHint_ =
          std::max(widthHint_, detail::singleLineTextWidth(appearance.popupFont, tab.label) +
                                   appearance.popupHorizontalPadding * 2 + closeWidth);
      if (tab.key == currentKey) {
        setCurrentItem(row);
      }
    }
    setFixedHeight(std::min(std::max(1, count()) * appearance.popupRowHeight,
                            200 - appearance.popupPadding * 2));
    updateGeometry();
    viewport()->update();
  }

  QSize sizeHint() const override {
    return QSize(widthHint_ + (count() * appearance_.popupRowHeight > height()
                                   ? verticalScrollBar()->sizeHint().width()
                                   : 0),
                 height());
  }
  QSize minimumSizeHint() const override { return QSize(120, height()); }

  void resetNavigation() {
    setCurrentRow(-1);
    clearSelection();
  }

  void navigate(int direction) {
    int row = currentRow();
    if (row < 0) {
      row = direction > 0 ? -1 : count();
    }
    for (int attempt = 0; attempt < count(); ++attempt) {
      row = (row + direction + count()) % count();
      if (item(row)->flags().testFlag(Qt::ItemIsEnabled)) {
        setCurrentRow(row);
        scrollToItem(item(row));
        return;
      }
    }
  }

  bool handleKey(QKeyEvent* event) {
    switch (event->key()) {
      case Qt::Key_Down:
        navigate(1);
        break;
      case Qt::Key_Up:
        navigate(-1);
        break;
      case Qt::Key_Escape:
        if (dismiss) dismiss();
        break;
      case Qt::Key_Return:
      case Qt::Key_Enter:
      case Qt::Key_Space:
        if (currentItem() && currentItem()->flags().testFlag(Qt::ItemIsEnabled) && choose) {
          choose(currentItem()->data(KeyRole).toString());
        }
        break;
      case Qt::Key_Delete:
      case Qt::Key_Backspace:
        if (currentItem() && currentItem()->data(ClosableRole).toBool() && close) {
          close(currentItem()->data(KeyRole).toString());
        }
        break;
      default:
        return false;
    }
    event->accept();
    return true;
  }

  std::function<void(const QString&)> choose;
  std::function<void(const QString&)> close;
  std::function<void()> dismiss;

 protected:
  void focusInEvent(QFocusEvent* event) override {
    const bool hadCurrentItem = currentItem() != nullptr;
    QListWidget::focusInEvent(event);
    // A popup acquiring focus must not choose a row before the user navigates.
    if (!hadCurrentItem) resetNavigation();
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (!handleKey(event)) QListWidget::keyPressEvent(event);
  }

  void mousePressEvent(QMouseEvent* event) override {
    const QModelIndex index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton &&
        closeRect(visualRect(index), index).contains(event->position().toPoint())) {
      closePressed_ = index.data(KeyRole).toString();
      event->accept();
      return;
    }
    QListWidget::mousePressEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (!closePressed_.isEmpty()) {
      const QString key = std::exchange(closePressed_, QString());
      const QModelIndex index = indexAt(event->position().toPoint());
      if (event->button() == Qt::LeftButton && key == index.data(KeyRole).toString() &&
          closeRect(visualRect(index), index).contains(event->position().toPoint()) && close) {
        close(key);
      }
      event->accept();
      return;
    }
    QListWidget::mouseReleaseEvent(event);
  }

 private:
  QRect closeRect(const QRect& row, const QModelIndex& index) const {
    if (!index.data(ClosableRole).toBool()) return {};
    const int side = appearance_.popupCloseSize;
    const QRect logical(row.right() + 1 - appearance_.popupHorizontalPadding - side,
                        row.y() + (row.height() - side) / 2, side, side);
    return QStyle::visualRect(layoutDirection(), row, logical);
  }

  class Delegate final : public QStyledItemDelegate {
   public:
    explicit Delegate(TabOverflowList* list) : QStyledItemDelegate(list), list_(list) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
      return QSize(list_->widthHint_, list_->appearance_.popupRowHeight);
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
      const auto& style = list_->appearance_;
      const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
      const bool active = enabled && (option.state.testFlag(QStyle::State_MouseOver) ||
                                      option.state.testFlag(QStyle::State_Selected));
      painter->save();
      if (active) painter->fillRect(option.rect, style.popupHoverBackground);
      const QColor foreground = enabled ? style.popupText : style.popupDisabledText;
      painter->setPen(foreground);
      painter->setFont(style.popupFont);
      QRect textRect =
          option.rect.adjusted(style.popupHorizontalPadding, 0, -style.popupHorizontalPadding, 0);
      const QRect closeBounds = list_->closeRect(option.rect, index);
      if (!closeBounds.isEmpty()) {
        if (option.direction == Qt::RightToLeft) {
          textRect.setLeft(closeBounds.right() + 1 + style.popupHorizontalPadding);
        } else {
          textRect.setRight(closeBounds.left() - 1 - style.popupHorizontalPadding);
        }
        auto icon = adqt::icons::antd::outlined::Close();
        icon = icon.withColors(icon.colors().withPrimary(style.popupClose));
        adqt::icons::paintIcon(painter, icon, closeBounds);
      }
      painter->drawText(
          textRect, Qt::AlignVCenter | Qt::AlignLeading | Qt::TextSingleLine,
          detail::elidedSingleLineText(style.popupFont, index.data().toString(), textRect.width()));
      painter->restore();
    }

   private:
    TabOverflowList* list_;
  };

  TabsAppearance appearance_;
  int widthHint_ = 120;
  QString closePressed_;
};

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

  std::function<bool(QKeyEvent*)> handleKey;

  void setAppearance(const TabsAppearance& value) {
    appearance_ = value;
    setFixedSize(appearance_.metrics.operationExtent, appearance_.metrics.operationExtent);
    setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
  }

 protected:
  void keyPressEvent(QKeyEvent* event) override {
    if (!handleKey || !handleKey(event)) QAbstractButton::keyPressEvent(event);
  }

  void focusInEvent(QFocusEvent* event) override {
    focusVisible_ = keyboardFocusReason(event->reason());
    QAbstractButton::focusInEvent(event);
    update();
  }

  void focusOutEvent(QFocusEvent* event) override {
    focusVisible_ = false;
    QAbstractButton::focusOutEvent(event);
    update();
  }

  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor color = isEnabled() ? appearance_.item : appearance_.disabled;
    if (kind_ == Kind::Add && isEnabled() && underMouse()) {
      color = appearance_.hover;
    }
    adqt::icons::IconRef icon = kind_ == Kind::More ? adqt::icons::antd::outlined::Ellipsis()
                                                    : adqt::icons::antd::outlined::Plus();
    icon = icon.withColors(icon.colors().withPrimary(color));
    const int side = appearance_.metrics.iconSize;
    adqt::icons::paintIcon(&painter, icon,
                           QRect((width() - side) / 2, (height() - side) / 2, side, side));
    if (hasFocus() && focusVisible_) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(appearance_.focusOutline, appearance_.metrics.focusOutlineWidth));
      painter.drawRoundedRect(QRectF(rect()).adjusted(3, 3, -3, -3),
                              appearance_.metrics.borderRadius, appearance_.metrics.borderRadius);
    }
  }

 private:
  Kind kind_;
  bool focusVisible_ = false;
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
        scrollArea_(new TabScrollArea(this)),
        tabContent_(new QWidget),
        moreButton_(new OperationButton(OperationButton::Kind::More, this)),
        addButton_(new OperationButton(OperationButton::Kind::Add, this)),
        indicator_(new IndicatorWidget(tabContent_)),
        indicatorAnimation_(new QVariantAnimation(this)) {
    setObjectName(QStringLiteral("ad-tabs-strip"));
    tabContent_->setObjectName(QStringLiteral("ad-tabs-content"));
    scrollArea_->setWidget(tabContent_);
    tabContent_->setAutoFillBackground(false);
    indicator_->setObjectName(QStringLiteral("ad-tabs-indicator"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    moreButton_->hide();
    addButton_->hide();
    overflowPopup_ = new AdPopover(this);
    overflowPopup_->setObjectName(QStringLiteral("ad-tabs-overflow-popup"));
    overflowPopup_->setSourceWidget(moreButton_);
    overflowPopup_->setTriggers(AdPopover::Trigger::Hover);
    overflowPopup_->setHoverOpenDelayMs(100);
    overflowPopup_->setHoverCloseDelayMs(100);
    overflowPopup_->setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    overflowPopup_->setPlacement(AdPopover::Placement::BottomLeft);
    overflowPopup_->setArrowVisible(false);
    overflowPopup_->setBorderWidth(0);
    overflowList_ = new TabOverflowList;
    overflowList_->setAccessibleName(moreButton_->accessibleName());
    overflowPopup_->setContentWidget(overflowList_);
    overflowList_->choose = [this](const QString& key) {
      overflowPopup_->hide();
      for (int index = 0; index < items_.size(); ++index) {
        if (items_.at(index).key == key) {
          if (activate) activate(index);
          break;
        }
      }
      if (currentIndex_ >= 0 && currentIndex_ < buttons_.size()) {
        scrollArea_->revealTab(buttons_.at(currentIndex_));
        buttons_.at(currentIndex_)->setFocus(Qt::OtherFocusReason);
      }
    };
    overflowList_->close = [this](const QString& key) {
      for (int index = 0; index < items_.size(); ++index) {
        if (items_.at(index).key == key) {
          if (closeRequested) closeRequested(index);
          break;
        }
      }
    };
    overflowList_->dismiss = [this] {
      overflowPopup_->hide();
      moreButton_->setFocus(Qt::TabFocusReason);
    };
    connect(overflowPopup_, &AdPopover::visibleChanged, this,
            [this] { overflowList_->resetNavigation(); });
    connect(moreButton_, &QAbstractButton::clicked, this, [this] {
      if (!overflowPopup_->isVisible()) overflowPopup_->show();
    });
    moreButton_->handleKey = [this](QKeyEvent* event) {
      if (!overflowPopup_->isVisible()) {
        if (event->key() != Qt::Key_Down && event->key() != Qt::Key_Return &&
            event->key() != Qt::Key_Enter && event->key() != Qt::Key_Space)
          return false;
        overflowPopup_->show();
        event->accept();
        return true;
      }
      return overflowList_->handleKey(event);
    };
    for (QScrollBar* bar : {scrollArea_->horizontalScrollBar(), scrollArea_->verticalScrollBar()}) {
      connect(bar, &QScrollBar::valueChanged, this, [this] {
        if (!layingOut_) refreshOverflowItems();
      });
    }
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

  ~TabsStrip() override { delete overflowPopup_; }

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
      auto* button = new TabButton(tabContent_);
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
    overflowList_->setLayoutDirection(layoutDirection());
    overflowPopup_->setBackgroundColor(appearance.surface);
    overflowPopup_->setCornerRadius(appearance.popupRadius);
    overflowPopup_->setContentMargins(
        QMargins(0, appearance.popupPadding, 0, appearance.popupPadding));
    overflowPopup_->setPopupOffset(appearance.popupPadding);
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
  static QSize constrainedWidgetHint(const QWidget* widget, bool minimum = false) {
    QSize hint = minimum ? widget->minimumSizeHint() : widget->sizeHint();
    if (!hint.isValid()) {
      hint = widget->sizeHint();
    }
    return hint.expandedTo(widget->minimumSizeHint())
        .expandedTo(widget->minimumSize())
        .boundedTo(widget->maximumSize());
  }

  static void addWidgetHint(const QWidget* widget, bool horizontal, int* primary, int* cross,
                            bool minimum = false) {
    if (!widget) {
      return;
    }
    const QSize hint = constrainedWidgetHint(widget, minimum);
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
    const QSize hint = constrainedWidgetHint(widget);
    return isHorizontal(placement_) ? hint.width() : hint.height();
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
    layingOut_ = true;
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

    const bool overflow = naturalExtent > available;
    moreButton_->setVisible(overflow);
    if (overflow) {
      const int extent = std::min(appearance_.metrics.operationExtent, available);
      end -= extent;
      available = std::max(0, end - start);
      moreButton_->setGeometry(extentRect(end, extent, crossExtent));
    }

    scrollArea_->setHorizontal(horizontal);
    scrollArea_->setGeometry(extentRect(start, available, crossExtent));
    const int contentExtent = std::max(naturalExtent, available);
    tabContent_->resize(horizontal ? QSize(contentExtent, crossExtent)
                                   : QSize(crossExtent, contentExtent));
    int cursor = centered_ && !overflow ? (available - naturalExtent) / 2 : 0;
    for (TabButton* button : buttons_) {
      // Keep each tab's natural extent. The viewport clips overflow, never the label layout.
      const int extent = itemExtent(button);
      QRect geometry = horizontal ? QRect(cursor, 0, extent, crossExtent)
                                  : QRect(0, cursor, crossExtent, extent);
      if (horizontal && layoutDirection() == Qt::RightToLeft) {
        geometry.moveLeft(contentExtent - cursor - extent);
      }
      button->setGeometry(geometry);
      button->show();
      cursor += extent + gutter;
    }
    if (currentIndex_ >= 0 && currentIndex_ < buttons_.size()) {
      scrollArea_->revealTab(buttons_.at(currentIndex_));
    }

    indicatorAnimation_->stop();
    indicatorRect_ = targetIndicatorRect();
    applyIndicatorGeometry();
    layingOut_ = false;
    refreshOverflowItems();
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
        if (activate) {
          activate(target);
        }
        buttons_.at(target)->setFocus(Qt::TabFocusReason);
        return;
      }
    }
  }

  void refreshOverflowItems() {
    QList<AdTabs::TabItem> overflowItems;
    for (int index = 0; index < buttons_.size(); ++index) {
      TabButton* button = buttons_.at(index);
      const QRect bounds(button->mapTo(scrollArea_->viewport(), QPoint()), button->size());
      if (!scrollArea_->viewport()->rect().contains(bounds)) overflowItems.append(items_.at(index));
    }
    overflowList_->configure(overflowItems, type_ == AdTabs::Type::EditableCard, appearance_);
    overflowPopup_->setEnabled(isEnabled() && !overflowItems.isEmpty() && !moreButton_->isHidden());
  }

  QList<AdTabs::TabItem> items_;
  QList<TabButton*> buttons_;
  int currentIndex_ = -1;
  AdTabs::Type type_ = AdTabs::Type::Line;
  AdTabs::Placement placement_ = AdTabs::Placement::Top;
  bool centered_ = false;
  bool animated_ = true;
  bool hideAdd_ = false;
  AdTabs::IndicatorAlignment indicatorAlignment_ = AdTabs::IndicatorAlignment::Fill;
  int indicatorSize_ = -1;
  TabsAppearance appearance_;
  TabScrollArea* scrollArea_ = nullptr;
  QWidget* tabContent_ = nullptr;
  OperationButton* moreButton_ = nullptr;
  OperationButton* addButton_ = nullptr;
  AdPopover* overflowPopup_ = nullptr;
  TabOverflowList* overflowList_ = nullptr;
  bool layingOut_ = false;
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
