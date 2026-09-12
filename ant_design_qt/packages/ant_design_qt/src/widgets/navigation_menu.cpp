#include "navigation_menu.h"

#include "detail/popup_geometry.h"

#include "detail/navigation_menu_popup_state.h"
#include "detail/navigation_menu_state.h"
#include "detail/navigation_menu_view_state.h"
#include "detail/popup_shadow.h"
#include "antd_icons.h"
#include "detail/timing_hub.h"
#include "menu_style.h"
#include "popup_placement.h"
#include "theme/theme.h"
#include "tooltip.h"

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QFrame>
#include <QHeaderView>
#include <QHash>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

namespace adqt::widgets {

namespace {

using detail::MenuStyleInput;
using detail::MenuVisualStyle;

constexpr int kAntdDropdownMinWidth = 160;
constexpr int kSubMenuArrowBoxWidth = 12;
constexpr int kSubMenuArrowBoxHeight = 14;
constexpr int kSubMenuArrowTextGap = 6;

QString trimmedOrFallback(const QString& value, const QString& fallback) {
  const QString trimmed = value.trimmed();
  return trimmed.isEmpty() ? fallback : trimmed;
}

bool isKeyboardFocusReason(Qt::FocusReason reason) {
  return reason != Qt::MouseFocusReason && reason != Qt::NoFocusReason;
}

QStringList uniqueStringList(const QStringList& values) {
  return detail::navigationMenuUniqueStringList(values);
}

QString stableIdForIndex(const QModelIndex& sourceIndex) {
  return detail::navigationMenuStableIdForIndex(sourceIndex);
}

QModelIndex findIndexByStableId(const QAbstractItemModel* model, const QString& stableId,
                                const QModelIndex& parent = QModelIndex()) {
  if (!model || stableId.trimmed().isEmpty()) {
    return QModelIndex();
  }
  const int rowCount = model->rowCount(parent);
  for (int row = 0; row < rowCount; ++row) {
    const QModelIndex index = model->index(row, 0, parent);
    if (!index.isValid()) {
      continue;
    }
    if (stableIdForIndex(index) == stableId) {
      return index;
    }
    if (const QModelIndex child = findIndexByStableId(model, stableId, index); child.isValid()) {
      return child;
    }
  }
  return QModelIndex();
}

QSize pixmapDeviceIndependentSize(const QPixmap& pixmap) {
  if (pixmap.isNull()) {
    return QSize();
  }
  const qreal dpr = pixmap.devicePixelRatio();
  if (dpr <= 0.0) {
    return pixmap.size();
  }
  return QSize(qRound(pixmap.width() / dpr), qRound(pixmap.height() / dpr));
}

QRect horizontalIndicatorContentRect(const QRect& contentRect, const QRect& iconRect, bool hasIcon,
                                     const QRect& textRect, int textWidth) {
  QRect occupiedRect;
  if (hasIcon && iconRect.isValid()) {
    occupiedRect = iconRect;
  }
  if (textWidth > 0 && textRect.isValid()) {
    const int clampedTextWidth = std::min(std::max(0, textWidth), textRect.width());
    if (clampedTextWidth > 0) {
      const QRect paintedTextRect(textRect.left(), textRect.top(), clampedTextWidth,
                                  textRect.height());
      occupiedRect =
          occupiedRect.isValid() ? occupiedRect.united(paintedTextRect) : paintedTextRect;
    }
  }
  if (!occupiedRect.isValid()) {
    return contentRect;
  }
  return occupiedRect.intersected(contentRect);
}

QRect horizontalActiveBarRect(const QRect& rowRect, const QRect& contentRect, const QRect& iconRect,
                              bool hasIcon, const QRect& textRect, int textWidth,
                              int activeBarHeight) {
  if (!rowRect.isValid() || !contentRect.isValid() || activeBarHeight <= 0) {
    return QRect();
  }

  const int barHeight = std::min(activeBarHeight, rowRect.height());
  if (barHeight <= 0) {
    return QRect();
  }

  const QRect occupiedRect =
      horizontalIndicatorContentRect(contentRect, iconRect, hasIcon, textRect, textWidth);
  if (!occupiedRect.isValid() || occupiedRect.isEmpty()) {
    return QRect(contentRect.left(), rowRect.bottom() - barHeight + 1,
                 std::max(0, contentRect.width()), barHeight);
  }

  return QRect(occupiedRect.left(), rowRect.bottom() - barHeight + 1,
               std::max(0, occupiedRect.width()), barHeight);
}

int collapsedInlineRootWidth(const MenuVisualStyle& style) {
  const int collapsedIconSize = std::max(10, style.metrics.collapsedIconSize);
  const int insetWidth = std::max(0, style.metrics.itemMarginInline) * 2;
  const int contentDrivenWidth =
      collapsedIconSize + std::max(0, style.metrics.itemPaddingInline) * 2 + insetWidth;
  const int rowDrivenWidth = std::max(0, style.metrics.itemHeight) * 2;
  return std::max(rowDrivenWidth, contentDrivenWidth);
}

void paintMenuIcon(QPainter& painter, adqt::icons::IconRef icon, const QRect& targetRect,
                   const QColor& color, bool disabled) {
  if (!adqt::icons::isValid(icon) || !targetRect.isValid()) {
    return;
  }

  const std::optional<QColor> primaryColor = icon.colors().primarySlot();
  if (!primaryColor.value_or(QColor()).isValid() && color.isValid()) {
    icon = icon.withColors(icon.colors().withPrimary(color));
  }
  const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
  const QIcon::Mode mode = disabled ? QIcon::Disabled : QIcon::Normal;
  adqt::icons::IconRenderRequest request;
  request.logicalSize = targetRect.size();
  request.devicePixelRatio = dpr;
  request.mode = mode;
  const QPixmap pixmap = adqt::icons::renderIconPixmap(icon, request);
  if (pixmap.isNull()) {
    return;
  }

  const QSize drawSize = pixmapDeviceIndependentSize(pixmap);
  const QPoint drawTopLeft(targetRect.x() + (targetRect.width() - drawSize.width()) / 2,
                           targetRect.y() + (targetRect.height() - drawSize.height()) / 2);
  painter.drawPixmap(drawTopLeft, pixmap);
}

QModelIndex mapToSourceIndex(const QModelIndex& index) {
  if (!index.isValid()) {
    return QModelIndex();
  }
  const auto* proxy = qobject_cast<const QAbstractProxyModel*>(index.model());
  return proxy ? proxy->mapToSource(index) : index;
}

QModelIndex mapFromSourceIndex(const QAbstractItemModel* model, const QModelIndex& sourceIndex) {
  if (!model || !sourceIndex.isValid()) {
    return QModelIndex();
  }
  const auto* proxy = qobject_cast<const QAbstractProxyModel*>(model);
  return proxy ? proxy->mapFromSource(sourceIndex) : sourceIndex;
}

int sourceDepth(const QModelIndex& index) {
  int depth = 0;
  QModelIndex current = index;
  while (current.isValid()) {
    current = current.parent();
    if (current.isValid()) {
      ++depth;
    }
  }
  return depth;
}

AdNavigationMenu::NodeKind nodeTypeForIndex(const QModelIndex& sourceIndex) {
  if (!sourceIndex.isValid()) {
    return AdNavigationMenu::NodeKind::Action;
  }
  const QVariant raw = sourceIndex.data(AdNavigationMenu::NodeKindRole);
  if (raw.isValid()) {
    return static_cast<AdNavigationMenu::NodeKind>(raw.toInt());
  }
  return AdNavigationMenu::NodeKind::Action;
}

bool isSeparatorIndex(const QModelIndex& sourceIndex) {
  return sourceIndex.isValid() &&
         nodeTypeForIndex(sourceIndex) == AdNavigationMenu::NodeKind::Separator;
}

bool isGroupIndex(const QModelIndex& sourceIndex) {
  return sourceIndex.isValid() &&
         nodeTypeForIndex(sourceIndex) == AdNavigationMenu::NodeKind::Group;
}

bool isSubmenuIndex(const QModelIndex& sourceIndex) {
  return sourceIndex.isValid() && !isSeparatorIndex(sourceIndex) && !isGroupIndex(sourceIndex) &&
         sourceIndex.model() && sourceIndex.model()->rowCount(sourceIndex) > 0;
}

bool isActionIndex(const QModelIndex& sourceIndex) {
  return sourceIndex.isValid() && !isSeparatorIndex(sourceIndex) && !isGroupIndex(sourceIndex) &&
         (!sourceIndex.model() || sourceIndex.model()->rowCount(sourceIndex) <= 0);
}

bool isInteractiveIndex(const QModelIndex& sourceIndex) {
  return sourceIndex.isValid() && !isSeparatorIndex(sourceIndex) &&
         (sourceIndex.flags() & Qt::ItemIsEnabled) &&
         (isActionIndex(sourceIndex) || isSubmenuIndex(sourceIndex));
}

bool isMenuRowIndex(const QModelIndex& sourceIndex) {
  return sourceIndex.isValid() && !isSeparatorIndex(sourceIndex) && !isGroupIndex(sourceIndex);
}

bool isDescendantOf(const QModelIndex& sourceIndex, const QModelIndex& ancestorIndex) {
  if (!sourceIndex.isValid() || !ancestorIndex.isValid()) {
    return false;
  }
  QModelIndex current = sourceIndex.parent();
  while (current.isValid()) {
    if (current == ancestorIndex) {
      return true;
    }
    current = current.parent();
  }
  return false;
}

int menuRowBaseSpacing(const MenuVisualStyle& style, const QModelIndex& sourceIndex) {
  return isMenuRowIndex(sourceIndex) ? std::max(0, style.metrics.itemMarginBlock) : 0;
}

int menuRowBoundarySpacing(const MenuVisualStyle& style, const QModelIndex& sourceIndex,
                           const QModelIndex& previousSourceIndex) {
  Q_UNUSED(style);
  Q_UNUSED(sourceIndex);
  Q_UNUSED(previousSourceIndex);
  return 0;
}

int menuRowLeadingSpacing(const MenuVisualStyle& style, const QModelIndex& sourceIndex,
                          const QModelIndex& previousSourceIndex) {
  return menuRowBaseSpacing(style, sourceIndex) +
         menuRowBoundarySpacing(style, sourceIndex, previousSourceIndex);
}

int menuRootLeadingSpacing(const MenuVisualStyle& style, AdNavigationMenu::Mode mode,
                           bool popupLevel, const QModelIndex& sourceIndex,
                           const QModelIndex& previousSourceIndex) {
  if (mode == AdNavigationMenu::Mode::Horizontal || popupLevel || sourceIndex.parent().isValid() ||
      previousSourceIndex.isValid()) {
    return 0;
  }
  return std::max(0, style.metrics.rootPaddingBlockStart);
}

bool isInlineSubMenuChildRow(AdNavigationMenu::Mode mode, bool collapsed, bool popupLevel,
                             const QModelIndex& sourceIndex) {
  return !popupLevel && mode == AdNavigationMenu::Mode::Inline && !collapsed &&
         isMenuRowIndex(sourceIndex) && sourceIndex.parent().isValid();
}

int inlineSubMenuLeadingSpacing(const MenuVisualStyle& style, AdNavigationMenu::Mode mode,
                                bool collapsed, bool popupLevel, const QModelIndex& sourceIndex,
                                const QModelIndex& previousSourceIndex) {
  if (!isInlineSubMenuChildRow(mode, collapsed, popupLevel, sourceIndex)) {
    return 0;
  }
  const QModelIndex parentIndex = sourceIndex.parent();
  return previousSourceIndex == parentIndex ? std::max(0, style.metrics.itemMarginBlock) : 0;
}

int inlineSubMenuTrailingSpacing(const MenuVisualStyle& style, AdNavigationMenu::Mode mode,
                                 bool collapsed, bool popupLevel, const QModelIndex& sourceIndex,
                                 const QModelIndex& nextSourceIndex) {
  if (!isInlineSubMenuChildRow(mode, collapsed, popupLevel, sourceIndex)) {
    return 0;
  }
  const QModelIndex parentIndex = sourceIndex.parent();
  if (!nextSourceIndex.isValid()) {
    return std::max(0, style.metrics.itemMarginBlock);
  }
  if (nextSourceIndex.parent() == parentIndex || isDescendantOf(nextSourceIndex, parentIndex)) {
    return 0;
  }
  return std::max(0, style.metrics.itemMarginBlock);
}

bool shouldShowSubMenuArrow(AdNavigationMenu::Mode mode, bool collapsed,
                            const QModelIndex& sourceIndex) {
  if (!isSubmenuIndex(sourceIndex)) {
    return false;
  }
  if (mode == AdNavigationMenu::Mode::Horizontal) {
    return false;
  }
  if (mode == AdNavigationMenu::Mode::Inline && collapsed) {
    return false;
  }
  return true;
}

AdTooltip::Placement collapsedMenuTooltipPlacement(const QWidget* widget) {
  if (widget && widget->layoutDirection() == Qt::RightToLeft) {
    return AdTooltip::Placement::Left;
  }
  return AdTooltip::Placement::Right;
}

QString displayTextForIndex(const QModelIndex& sourceIndex) {
  return trimmedOrFallback(sourceIndex.data(Qt::DisplayRole).toString(),
                           stableIdForIndex(sourceIndex));
}

QString tooltipTextForIndex(const QModelIndex& sourceIndex) {
  QString explicitText = sourceIndex.data(Qt::ToolTipRole).toString().trimmed();
  if (!explicitText.isEmpty()) {
    return explicitText;
  }
  return displayTextForIndex(sourceIndex);
}

adqt::icons::IconRef iconRefForIndex(const QModelIndex& sourceIndex) {
  const QVariant raw = sourceIndex.data(Qt::DecorationRole);
  return raw.canConvert<adqt::icons::IconRef>() ? raw.value<adqt::icons::IconRef>()
                                                : adqt::icons::IconRef();
}

AdNavigationMenu::ColorScheme popupColorSchemeForIndex(const QModelIndex& sourceIndex,
                                                       AdNavigationMenu::ColorScheme fallback) {
  const QVariant raw = sourceIndex.data(AdNavigationMenu::PopupColorSchemeRole);
  if (!raw.isValid()) {
    return fallback;
  }
  return static_cast<AdNavigationMenu::ColorScheme>(raw.toInt());
}

bool dashedSeparatorForIndex(const QModelIndex& sourceIndex) {
  return sourceIndex.isValid() && sourceIndex.data(AdNavigationMenu::DashedRole).toBool();
}

void clearLayoutItems(QLayout* layout) {
  if (!layout) {
    return;
  }
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (QWidget* widget = item->widget()) {
      widget->hide();
    }
    delete item;
  }
}

AdNavigationMenu::ColorScheme resolvedColorScheme(AdNavigationMenu::ColorScheme colorScheme,
                                                  adqt::theme::ThemeScheme themeScheme) {
  if (colorScheme != AdNavigationMenu::ColorScheme::Inherit) {
    return colorScheme;
  }
  return themeScheme == adqt::theme::ThemeScheme::Dark ? AdNavigationMenu::ColorScheme::Dark
                                                       : AdNavigationMenu::ColorScheme::Light;
}

int rootBorderWidthForStyle(AdNavigationMenu::Mode mode, AdNavigationMenu::ColorScheme colorScheme,
                            adqt::theme::ThemeScheme themeScheme, const MenuVisualStyle& style) {
  if (mode == AdNavigationMenu::Mode::Horizontal &&
      resolvedColorScheme(colorScheme, themeScheme) == AdNavigationMenu::ColorScheme::Dark) {
    return 0;
  }
  return std::max(0, style.metrics.borderWidth);
}

enum MenuRootBorderEdge : std::uint8_t {
  kMenuRootBorderNone = 0,
  kMenuRootBorderRight = 1,
  kMenuRootBorderBottom = 2,
};

QColor menuViewPropertyColor(const QWidget* widget, const char* name) {
  if (!widget) {
    return QColor();
  }
  const QVariant value = widget->property(name);
  return value.isValid() && value.canConvert<QColor>() ? value.value<QColor>() : QColor();
}

int menuViewPropertyInt(const QWidget* widget, const char* name) {
  return widget ? widget->property(name).toInt() : 0;
}

void paintMenuRootBackground(const QWidget* view, QWidget* surface, const QRect& dirtyRect) {
  if (!view || !surface || view->property("AdNavigationMenu.popupLevel").toBool()) {
    return;
  }
  const QColor backgroundColor = menuViewPropertyColor(view, "AdNavigationMenu.backgroundColor");
  if (!backgroundColor.isValid() || backgroundColor.alpha() <= 0) {
    return;
  }
  QPainter painter(surface);
  painter.setClipRect(dirtyRect.intersected(surface->rect()));
  painter.fillRect(surface->rect(), backgroundColor);
}

void paintMenuRootBorder(const QWidget* view, QWidget* surface, const QRect& dirtyRect) {
  if (!view || !surface || view->property("AdNavigationMenu.popupLevel").toBool()) {
    return;
  }
  const QColor borderColor = menuViewPropertyColor(view, "AdNavigationMenu.borderColor");
  const int borderWidth = std::max(0, menuViewPropertyInt(view, "AdNavigationMenu.borderWidth"));
  const int borderEdge = menuViewPropertyInt(view, "AdNavigationMenu.borderEdge");
  if (!borderColor.isValid() || borderColor.alpha() <= 0 || borderWidth <= 0 ||
      borderEdge == kMenuRootBorderNone) {
    return;
  }

  const QRect clipRect = dirtyRect.intersected(surface->rect());
  if (!clipRect.isValid() || clipRect.isEmpty()) {
    return;
  }

  QPainter painter(surface);
  painter.setClipRect(clipRect);
  if (borderEdge == kMenuRootBorderBottom) {
    painter.fillRect(
        QRect(0, std::max(0, surface->height() - borderWidth), surface->width(), borderWidth),
        borderColor);
  } else if (borderEdge == kMenuRootBorderRight) {
    painter.fillRect(
        QRect(std::max(0, surface->width() - borderWidth), 0, borderWidth, surface->height()),
        borderColor);
  }
}

QItemSelectionModel::SelectionFlags menuSelectionCommandForIndex(
    const QModelIndex& index, QItemSelectionModel::SelectionFlags fallback) {
  const QModelIndex sourceIndex = mapToSourceIndex(index);
  if (sourceIndex.isValid() && !isActionIndex(sourceIndex)) {
    return QItemSelectionModel::NoUpdate;
  }
  return fallback;
}

}  // namespace

namespace detail {

class AdMenuTreeView final : public QTreeView {
 public:
  explicit AdMenuTreeView(AdNavigationMenu* owner, QWidget* parent = nullptr)
      : QTreeView(parent), owner_(owner) {
    suppressChrome();
  }

  std::function<bool(QKeyEvent*)> keyHandler;
  std::function<void()> enterHandler;
  std::function<void()> leaveHandler;
  std::function<void(Qt::FocusReason)> focusInHandler;
  std::function<void()> focusOutHandler;
  std::function<void()> mousePressHandler;

  void syncColumnWidth() {
    if (!model()) {
      return;
    }
    if (model()->columnCount(rootIndex()) <= 0) {
      return;
    }
    const int targetWidth = viewport() ? viewport()->width() : width();
    if (targetWidth <= 0) {
      return;
    }
    setColumnWidth(0, targetWidth);
  }

  void refreshLayout() {
    suppressChrome();
    scheduleDelayedItemsLayout();
    doItemsLayout();
    syncColumnWidth();
    updateGeometries();
    suppressChrome();
    if (viewport()) {
      viewport()->update();
    }
  }

  void resizeEvent(QResizeEvent* event) override {
    QTreeView::resizeEvent(event);
    suppressChrome();
    syncColumnWidth();
  }

  void suppressChrome() {
    setHeaderHidden(true);
    setViewportMargins(0, 0, 0, 0);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    if (QHeaderView* treeHeader = header()) {
      treeHeader->hide();
      treeHeader->setMinimumHeight(0);
      treeHeader->setMaximumHeight(0);
      treeHeader->resize(treeHeader->width(), 0);
    }
    if (QScrollBar* verticalBar = verticalScrollBar()) {
      verticalBar->hide();
      verticalBar->setFixedWidth(0);
    }
    if (QScrollBar* horizontalBar = horizontalScrollBar()) {
      horizontalBar->hide();
      horizontalBar->setFixedHeight(0);
    }
  }

  QRect visualRect(const QModelIndex& index) const override {
    const QRect rawRect = QTreeView::visualRect(index);
    if (!rawRect.isValid() || !owner_) {
      return rawRect;
    }

    const bool popupLevel = property("AdNavigationMenu.popupLevel").toBool();
    const AdNavigationMenu::Mode mode =
        static_cast<AdNavigationMenu::Mode>(property("AdNavigationMenu.mode").toInt());
    const AdNavigationMenu::ColorScheme popupColorScheme =
        static_cast<AdNavigationMenu::ColorScheme>(
            property("AdNavigationMenu.popupColorScheme").toInt());
    const MenuVisualStyle style = owner_->resolvedVisualStyle(
        popupLevel ? AdNavigationMenu::Mode::Vertical : mode, popupColorScheme, false);
    const QModelIndex sourceIndex = mapToSourceIndex(index);

    const QModelIndex previousIndex = indexAbove(index);
    const QModelIndex previousSourceIndex =
        previousIndex.isValid() ? mapToSourceIndex(previousIndex) : QModelIndex();
    const int topSpacing =
        (popupLevel ? menuRowLeadingSpacing(style, sourceIndex, previousSourceIndex) : 0) +
        menuRootLeadingSpacing(style, mode, popupLevel, sourceIndex, previousSourceIndex);
    return rawRect.adjusted(0, topSpacing, 0, 0);
  }

  void paintEvent(QPaintEvent* event) override {
    bool popupLevel = false;
    qreal popupRadius = 0.0;
    qreal popupBorderWidth = 0.0;
    MenuVisualStyle popupStyle;

    popupLevel = property("AdNavigationMenu.popupLevel").toBool();
    if (!popupLevel && viewport()) {
      paintMenuRootBackground(this, viewport(), event->rect());
    }

    if (owner_) {
      popupLevel = property("AdNavigationMenu.popupLevel").toBool();
      const AdNavigationMenu::ColorScheme popupColorScheme =
          static_cast<AdNavigationMenu::ColorScheme>(
              property("AdNavigationMenu.popupColorScheme").toInt());
      if (popupLevel && viewport()) {
        popupStyle =
            owner_->resolvedVisualStyle(AdNavigationMenu::Mode::Vertical, popupColorScheme, false);
        popupRadius =
            std::max<qreal>(0.0, static_cast<qreal>(popupStyle.metrics.popupBorderRadius));
        popupBorderWidth = std::max<qreal>(0.0, static_cast<qreal>(popupStyle.metrics.borderWidth));

        const qreal inset = popupBorderWidth > 0.0 ? popupBorderWidth * 0.5 : 0.5;
        const QRectF popupRect = QRectF(viewport()->rect()).adjusted(inset, inset, -inset, -inset);
        QPainterPath popupPath;
        popupPath.addRoundedRect(popupRect, popupRadius, popupRadius);
        viewport()->setMask(QRegion(popupPath.toFillPolygon().toPolygon()));

        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setClipRect(event->rect().intersected(viewport()->rect()));
        painter.fillPath(popupPath, popupStyle.popupBackground);
      } else {
        if (viewport()) {
          viewport()->clearMask();
        }
      }
    }

    QTreeView::paintEvent(event);

    if (!popupLevel && viewport()) {
      paintMenuRootBorder(this, viewport(), event->rect());
    }

    if (popupLevel && viewport() && popupStyle.metrics.borderWidth > 0 &&
        popupStyle.popupBorderColor.alpha() > 0) {
      const QRect repaintRect = event->rect().intersected(viewport()->rect());
      const int borderInset =
          std::max(0, static_cast<int>(std::ceil(std::max<qreal>(popupRadius, popupBorderWidth))));
      const bool touchesBorder =
          !repaintRect.isValid() || borderInset <= 0 || repaintRect.top() < borderInset ||
          repaintRect.bottom() > viewport()->rect().height() - borderInset - 1 ||
          repaintRect.left() < borderInset ||
          repaintRect.right() > viewport()->rect().width() - borderInset - 1;
      if (touchesBorder) {
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(popupStyle.popupBorderColor, popupStyle.metrics.borderWidth));
        painter.setBrush(Qt::NoBrush);
        const qreal inset = popupBorderWidth * 0.5;
        const QRectF borderRect = QRectF(viewport()->rect()).adjusted(inset, inset, -inset, -inset);
        painter.drawRoundedRect(borderRect, popupRadius, popupRadius);
      }
    }
  }
  void enterEvent(QEnterEvent* event) override {
    QTreeView::enterEvent(event);
    if (enterHandler) {
      enterHandler();
    }
  }

  void leaveEvent(QEvent* event) override {
    QTreeView::leaveEvent(event);
    if (leaveHandler) {
      leaveHandler();
    }
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (keyHandler && keyHandler(event)) {
      return;
    }
    QTreeView::keyPressEvent(event);
  }

  void focusInEvent(QFocusEvent* event) override {
    QTreeView::focusInEvent(event);
    if (focusInHandler) {
      focusInHandler(event ? event->reason() : Qt::OtherFocusReason);
    }
  }

  void focusOutEvent(QFocusEvent* event) override {
    QTreeView::focusOutEvent(event);
    if (focusOutHandler) {
      focusOutHandler();
    }
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (mousePressHandler) {
      mousePressHandler();
    }
    QTreeView::mousePressEvent(event);
  }

  QItemSelectionModel::SelectionFlags selectionCommand(
      const QModelIndex& index, const QEvent* event = nullptr) const override {
    return menuSelectionCommandForIndex(index, QTreeView::selectionCommand(index, event));
  }

 private:
  QPointer<AdNavigationMenu> owner_;
};

class AdMenuBarProxyModel final : public QAbstractProxyModel {
 public:
  explicit AdMenuBarProxyModel(QObject* parent = nullptr) : QAbstractProxyModel(parent) {}

  void setSourceModel(QAbstractItemModel* sourceModel) override {
    beginResetModel();
    for (const QMetaObject::Connection& connection : connections_) {
      QObject::disconnect(connection);
    }
    connections_.clear();

    QAbstractProxyModel::setSourceModel(sourceModel);
    if (sourceModel) {
      const auto reset = [this]() {
        beginResetModel();
        endResetModel();
      };
      connections_.append(
          QObject::connect(sourceModel, &QAbstractItemModel::modelReset, this, reset));
      connections_.append(
          QObject::connect(sourceModel, &QAbstractItemModel::layoutChanged, this, reset));
      connections_.append(
          QObject::connect(sourceModel, &QAbstractItemModel::rowsInserted, this, reset));
      connections_.append(
          QObject::connect(sourceModel, &QAbstractItemModel::rowsRemoved, this, reset));
      connections_.append(
          QObject::connect(sourceModel, &QAbstractItemModel::rowsMoved, this, reset));
      connections_.append(
          QObject::connect(sourceModel, &QAbstractItemModel::dataChanged, this,
                           [this](const QModelIndex& topLeft, const QModelIndex& bottomRight,
                                  const QList<int>& roles) {
                             if (!topLeft.isValid() || !bottomRight.isValid()) {
                               beginResetModel();
                               endResetModel();
                               return;
                             }
                             if (topLeft.parent().isValid() || bottomRight.parent().isValid()) {
                               return;
                             }
                             emit dataChanged(index(topLeft.row(), 0, QModelIndex()),
                                              index(bottomRight.row(), 0, QModelIndex()), roles);
                           }));
    }
    endResetModel();
  }

  QModelIndex mapToSource(const QModelIndex& proxyIndex) const override {
    if (!proxyIndex.isValid() || !sourceModel()) {
      return QModelIndex();
    }
    return sourceModel()->index(proxyIndex.row(), 0, QModelIndex());
  }

  QModelIndex mapFromSource(const QModelIndex& sourceIndex) const override {
    if (!sourceModel() || !sourceIndex.isValid() || sourceIndex.parent().isValid() ||
        sourceIndex.column() != 0) {
      return QModelIndex();
    }
    return index(sourceIndex.row(), 0, QModelIndex());
  }

  QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override {
    if (parent.isValid() || !sourceModel() || column != 0 || row < 0 ||
        row >= sourceModel()->rowCount()) {
      return QModelIndex();
    }
    return createIndex(row, column);
  }

  QModelIndex parent(const QModelIndex&) const override { return QModelIndex(); }

  int rowCount(const QModelIndex& parent = QModelIndex()) const override {
    return parent.isValid() || !sourceModel() ? 0 : sourceModel()->rowCount(QModelIndex());
  }

  int columnCount(const QModelIndex& parent = QModelIndex()) const override {
    Q_UNUSED(parent)
    return 1;
  }

  QVariant data(const QModelIndex& proxyIndex, int role = Qt::DisplayRole) const override {
    return mapToSource(proxyIndex).data(role);
  }

  Qt::ItemFlags flags(const QModelIndex& proxyIndex) const override {
    const QModelIndex sourceIndex = mapToSource(proxyIndex);
    return sourceIndex.isValid() ? sourceIndex.flags() : Qt::NoItemFlags;
  }

 private:
  QVector<QMetaObject::Connection> connections_;
};

class AdMenuBarView final : public QListView {
 public:
  explicit AdMenuBarView(QWidget* parent = nullptr) : QListView(parent) { suppressScrollBars(); }

  std::function<bool(QKeyEvent*)> keyHandler;
  std::function<void()> enterHandler;
  std::function<void()> leaveHandler;
  std::function<void(Qt::FocusReason)> focusInHandler;
  std::function<void()> focusOutHandler;
  std::function<void()> mousePressHandler;

  void refreshLayout() {
    suppressScrollBars();
    scheduleDelayedItemsLayout();
    doItemsLayout();
    updateGeometries();
    suppressScrollBars();
    if (viewport()) {
      viewport()->update();
    }
  }

  void suppressScrollBars() {
    setViewportMargins(0, 0, 0, 0);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    if (QScrollBar* verticalBar = verticalScrollBar()) {
      verticalBar->hide();
      verticalBar->setFixedWidth(0);
    }
    if (QScrollBar* horizontalBar = horizontalScrollBar()) {
      horizontalBar->hide();
      horizontalBar->setFixedHeight(0);
    }
  }

 protected:
  void enterEvent(QEnterEvent* event) override {
    QListView::enterEvent(event);
    if (enterHandler) {
      enterHandler();
    }
  }

  void leaveEvent(QEvent* event) override {
    QListView::leaveEvent(event);
    if (leaveHandler) {
      leaveHandler();
    }
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (keyHandler && keyHandler(event)) {
      return;
    }
    QListView::keyPressEvent(event);
  }

  void focusInEvent(QFocusEvent* event) override {
    QListView::focusInEvent(event);
    if (focusInHandler) {
      focusInHandler(event ? event->reason() : Qt::OtherFocusReason);
    }
  }

  void focusOutEvent(QFocusEvent* event) override {
    QListView::focusOutEvent(event);
    if (focusOutHandler) {
      focusOutHandler();
    }
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (mousePressHandler) {
      mousePressHandler();
    }
    QListView::mousePressEvent(event);
  }

  QItemSelectionModel::SelectionFlags selectionCommand(
      const QModelIndex& index, const QEvent* event = nullptr) const override {
    return menuSelectionCommandForIndex(index, QListView::selectionCommand(index, event));
  }

  void paintEvent(QPaintEvent* event) override {
    if (viewport()) {
      paintMenuRootBackground(this, viewport(), event->rect());
      // Match Ant Design stacking: horizontal item indicators should paint over the root bottom
      // border.
      paintMenuRootBorder(this, viewport(), event->rect());
    }
    QListView::paintEvent(event);
  }
};

class AdMenuPopupShell final : public QWidget {
 public:
  explicit AdMenuPopupShell(AdNavigationMenu* owner, QWidget* parent = nullptr)
      : QWidget(parent), owner_(owner) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    setProperty("adqt.interaction.surface", true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
  }

  void setPopupColorScheme(AdNavigationMenu::ColorScheme colorScheme) {
    popupColorScheme_ = colorScheme;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    QWidget::paintEvent(event);
    if (!owner_) {
      return;
    }
    const MenuVisualStyle style =
        owner_ ? owner_->resolvedVisualStyle(AdNavigationMenu::Mode::Vertical, popupColorScheme_,
                                             false)
               : detail::resolveMenuVisualStyle({});
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal radius = std::max<qreal>(0.0, static_cast<qreal>(style.metrics.popupBorderRadius));
    const qreal borderWidth = std::max<qreal>(0.0, static_cast<qreal>(style.metrics.borderWidth));
    const qreal inset = borderWidth > 0.0 ? borderWidth * 0.5 : 0.5;
    QRect chromeRect = rect();
    if (layout()) {
      const QRect contentsRect = layout()->contentsRect();
      if (contentsRect.isValid() && !contentsRect.isEmpty()) {
        chromeRect = contentsRect;
      }
    }
    const QRectF popupRect = QRectF(chromeRect).adjusted(inset, inset, -inset, -inset);
    QPainterPath path;
    path.addRoundedRect(popupRect, radius, radius);
    detail::paintAntPopupBoxShadowSecondary(painter, path);
    painter.fillPath(path, style.popupBackground);
    if (style.metrics.borderWidth > 0 && style.popupBorderColor.alpha() > 0) {
      painter.setPen(QPen(style.popupBorderColor, style.metrics.borderWidth));
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(path);
    }
  }

 private:
  QPointer<AdNavigationMenu> owner_;
  AdNavigationMenu::ColorScheme popupColorScheme_ = AdNavigationMenu::ColorScheme::Inherit;
};

}  // namespace detail

AdNavigationMenuPopupFactory::AdNavigationMenuPopupFactory(QObject* parent) : QObject(parent) {}

AdNavigationMenuPopupFactory::~AdNavigationMenuPopupFactory() = default;

QWidget* AdNavigationMenuPopupFactory::createPopup(const AdNavigationMenuPopupContext& context,
                                                   QWidget* defaultPopup, QWidget* parent) {
  Q_UNUSED(context)
  if (!defaultPopup) {
    return nullptr;
  }
  if (parent && defaultPopup->parentWidget() != parent) {
    defaultPopup->setParent(parent);
  }
  return defaultPopup;
}

AdNavigationMenuItemDelegate::AdNavigationMenuItemDelegate(AdNavigationMenu* owner, QObject* parent)
    : QStyledItemDelegate(parent ? parent : owner), owner_(owner) {}

AdNavigationMenuItemDelegate::~AdNavigationMenuItemDelegate() = default;

AdNavigationMenu* AdNavigationMenuItemDelegate::owner() const { return owner_.data(); }

class AdNavigationMenu::Private {
 public:
  explicit Private(AdNavigationMenu* qq);
  ~Private();

  using PopupLevel = detail::NavigationMenuPopupLevel;

  AdNavigationMenu* q = nullptr;
  Mode mode = Mode::Vertical;
  ColorScheme colorScheme = ColorScheme::Inherit;
  bool collapsed = false;
  int indentationOverride = -1;
  SelectionMode selectionMode = SelectionMode::SingleSelection;
  TriggerSubMenuAction submenuTrigger = TriggerSubMenuAction::Hover;
  int submenuOpenDelayMs = -1;
  int submenuCloseDelayMs = -1;
  bool tooltipEnabled = true;
  QPointer<AdTooltip> tooltipHost;
  ComponentTokens componentTokens;
  SemanticStyles semanticStyles;
  SemanticStyleResolver semanticStyleResolver;
  QPointer<AdNavigationMenuPopupFactory> popupFactory;
  adqt::icons::IconRef expandIcon;
  QPoint popupOffset;
  QPointer<QStandardItemModel> ownedItemModel;

  detail::NavigationMenuState state;
  QPointer<QAbstractItemModel>& model = state.model;
  QPointer<QItemSelectionModel>& selectionModel = state.selectionModel;
  bool& ownsSelectionModel = state.ownsSelectionModel;
  bool& focusVisible = state.focusVisible;
  QStringList& inlineExpandedCacheStableIds = state.inlineExpandedCacheStableIds;
  QHash<QString, QPersistentModelIndex>& stableIdIndexCache = state.stableIdIndexCache;
  QSet<QString>& duplicateStableIds = state.duplicateStableIds;
  QStringList& expandedStableIds = state.expandedStableIds;
  QStringList& collapsedPopupStableIds = state.collapsedPopupStableIds;
  QPersistentModelIndex& pendingHoverIndex = state.pendingHoverIndex;
  QPersistentModelIndex& hoveredIndex = state.hoveredIndex;

  QPointer<QItemSelectionModel> barSelectionModel;
  bool updatingCurrentForViews = false;

  detail::NavigationMenuViewState views;
  QStackedLayout*& rootLayout = views.rootLayout;
  detail::AdMenuTreeView*& inlineView = views.inlineView;
  detail::AdMenuTreeView*& verticalView = views.verticalView;
  detail::AdMenuBarProxyModel*& barProxyModel = views.barProxyModel;
  detail::AdMenuBarView*& barView = views.barView;

  AdNavigationMenuItemDelegate* delegate = nullptr;
  QPointer<QAbstractItemDelegate> customDelegate;

  detail::NavigationMenuPopupState popupState;
  std::vector<std::unique_ptr<PopupLevel>>& popupLevels = popupState.levels;
  QTimer& hoverOpenTimer = popupState.hoverOpenTimer;
  QTimer& hoverCloseTimer = popupState.hoverCloseTimer;

  QVector<QMetaObject::Connection> modelConnections;
  QVector<QMetaObject::Connection> selectionConnections;

  void prepareForDestruction();
  QWidget* activeViewWidget() const;
  detail::AdMenuTreeView* activeTreeView() const;
  bool usingOwnedModel() const;
  bool popupLikeMode() const;
  QItemSelectionModel* ensureSelectionModel();
  QItemSelectionModel* ensureBarSelectionModel();
  QAbstractItemView::SelectionMode qtSelectionMode() const;
  void adoptSelectionModel(QItemSelectionModel* value, bool owned);
  void rebuildStableIdIndexCache();
  QModelIndex indexForStableId(const QString& key) const;
  QString stableIdForIndexNormalized(const QModelIndex& index) const;
  QStringList filterKnownStableIds(const QStringList& keys, bool submenuOnly,
                                   const char* context) const;
  void syncModelConnections();
  void syncSelectionConnections();
  void rebuildRootViews();
  void rebuildModeView();
  QAbstractItemDelegate* effectiveDelegate() const;
  void syncDelegates();
  void updateViewProperties(QWidget* view, Mode viewMode, bool collapsedValue,
                            ColorScheme popupColorScheme, int rootDepth, bool popupLevel) const;
  void syncTreeView(detail::AdMenuTreeView* view, const QModelIndex& rootIndex, bool inlineMode);
  void syncAllViews();
  void syncViewSelectionModels();
  void syncBarSelectionState();
  void updateAllViewports();
  void normalizeExpandedStableIds(QStringList& keys) const;
  QStringList& visibleExpandedStableIds();
  const QStringList& visibleExpandedStableIds() const;
  bool containsExpanded(const QModelIndex& index) const;
  void setExpandedInternal(const QModelIndex& index, bool expanded, bool emitSignal);
  void clearPopupExpandedSubmenus();
  QVector<QModelIndex> popupChain() const;
  void ensurePopupLevel(int levelIndex);
  void syncPopupVisibility();
  void syncPopupLevel(PopupLevel& level, const QModelIndex& submenuIndex);
  void hidePopupLevelsFrom(int levelIndex);
  QSize popupContentSize(const QModelIndex& submenuIndex, ColorScheme popupColorScheme,
                         const QWidget* measuringWidget = nullptr) const;
  QSize rootContentSize() const;
  void appendVisibleRows(const QModelIndex& parentIndex, bool inlineMode,
                         QVector<QModelIndex>& out) const;
  bool hasSelectedDescendant(const QModelIndex& sourceIndex) const;
  bool isSourceIndexSelected(const QModelIndex& sourceIndex) const;
  bool hasSelectedDescendantForStyle(const QModelIndex& sourceIndex) const;
  bool shouldUseSelectedState(const QModelIndex& sourceIndex) const;
  bool hasFocusWithin() const;
  bool isSourceIndexCurrent(const QModelIndex& sourceIndex) const;
  bool shouldPaintCurrentAsActive(const QModelIndex& sourceIndex) const;
  void setFocusVisible(bool visible);
  void handleViewFocusIn(Qt::FocusReason reason);
  void handleViewFocusOut();
  bool applySelectionTrigger(const QModelIndex& sourceIndex);
  QStringList selectedStableIdsForState() const;
  void syncModeTransitionState(Mode previousMode, bool previousCollapsed);
  void setCurrentIndexInternal(const QModelIndex& sourceIndex, bool emitSignal);
  void restoreCurrentFromSelection();
  void updateCurrentForViews();
  void activateSourceIndex(const QModelIndex& sourceIndex, bool fromKeyboard,
                           bool selectionAlreadyHandled = false);
  void applyPendingHoverOpen();
  void openFromHover(const QModelIndex& sourceIndex);
  void scheduleHoverClose();
  void cancelHoverClose();
  void closeDanglingPopups();
  void handleHoveredIndex(const QModelIndex& sourceIndex);
  void handleLeave();
  void updateCursorForView(QWidget* target, const QModelIndex& sourceIndex);
  void clearCursorForView(QWidget* target);
  bool handleTreeKey(detail::AdMenuTreeView* view, QKeyEvent* event);
  bool handleBarKey(QKeyEvent* event);
  QModelIndex firstVisibleIndex(detail::AdMenuTreeView* view) const;
  QModelIndex lastVisibleIndex(detail::AdMenuTreeView* view) const;
  QModelIndex nextNavigableIndex(detail::AdMenuTreeView* view, const QModelIndex& start,
                                 int delta) const;
  QModelIndex firstNavigableIndex(detail::AdMenuTreeView* view) const;
  QModelIndex lastNavigableIndex(detail::AdMenuTreeView* view) const;
  QModelIndex barAdjacentIndex(const QModelIndex& start, int delta) const;
  QModelIndex topLevelAncestor(const QModelIndex& sourceIndex) const;
  QModelIndex popupParentForView(detail::AdMenuTreeView* view) const;
  void focusPopupForIndex(const QModelIndex& sourceIndex);
  void focusPopupForIndexDeferred(const QModelIndex& sourceIndex);
  void ensureTooltipHost();
  void hideTooltip();
  void syncCollapsedTooltip(const QModelIndex& sourceIndex);
  static void applyExpandedStateChange(QStringList& targetExpanded, const QModelIndex& index,
                                       bool expanded, bool popupLikeMode, const Private* self);
  void applySelectionStateFromStableIds(const QStringList& keys);
};

AdNavigationMenu::Private::Private(AdNavigationMenu* qq) : q(qq) {
  hoverOpenTimer.setSingleShot(true);
  hoverCloseTimer.setSingleShot(true);
  QObject::connect(&hoverOpenTimer, &QTimer::timeout, q, [this]() { applyPendingHoverOpen(); });
  QObject::connect(&hoverCloseTimer, &QTimer::timeout, q, [this]() { closeDanglingPopups(); });
}

AdNavigationMenu::Private::~Private() {
  hoverOpenTimer.stop();
  hoverCloseTimer.stop();
  pendingHoverIndex = QModelIndex();
  hoveredIndex = QModelIndex();
  hideTooltip();
  hidePopupLevelsFrom(0);
}

void AdNavigationMenu::Private::prepareForDestruction() {
  hoverOpenTimer.stop();
  hoverCloseTimer.stop();
  pendingHoverIndex = QModelIndex();
  hoveredIndex = QModelIndex();

  auto clearTreeViewHandlers = [](detail::AdMenuTreeView* view) {
    if (!view) {
      return;
    }
    view->keyHandler = {};
    view->enterHandler = {};
    view->leaveHandler = {};
    view->focusInHandler = {};
    view->focusOutHandler = {};
    view->mousePressHandler = {};
  };
  auto clearBarViewHandlers = [](detail::AdMenuBarView* view) {
    if (!view) {
      return;
    }
    view->keyHandler = {};
    view->enterHandler = {};
    view->leaveHandler = {};
    view->focusInHandler = {};
    view->focusOutHandler = {};
    view->mousePressHandler = {};
  };

  clearTreeViewHandlers(inlineView);
  clearTreeViewHandlers(verticalView);
  clearBarViewHandlers(barView);

  for (const auto& level : popupLevels) {
    if (!level) {
      continue;
    }
    clearTreeViewHandlers(level->view);
    if (level->shell) {
      level->shell->removeEventFilter(q);
      QObject::disconnect(level->shell, nullptr, q, nullptr);
    }
    if (level->renderedRoot && level->renderedRoot != level->view) {
      level->renderedRoot->removeEventFilter(q);
      QObject::disconnect(level->renderedRoot, nullptr, q, nullptr);
    }
    if (level->view) {
      QObject::disconnect(level->view, nullptr, q, nullptr);
    }
  }

  if (inlineView) {
    QObject::disconnect(inlineView, nullptr, q, nullptr);
  }
  if (verticalView) {
    QObject::disconnect(verticalView, nullptr, q, nullptr);
  }
  if (barView) {
    QObject::disconnect(barView, nullptr, q, nullptr);
  }
  if (model) {
    QObject::disconnect(model, nullptr, q, nullptr);
  }
  if (selectionModel) {
    QObject::disconnect(selectionModel, nullptr, q, nullptr);
  }
  if (barSelectionModel) {
    QObject::disconnect(barSelectionModel, nullptr, q, nullptr);
  }

  for (const QMetaObject::Connection& connection : modelConnections) {
    QObject::disconnect(connection);
  }
  modelConnections.clear();
  for (const QMetaObject::Connection& connection : selectionConnections) {
    QObject::disconnect(connection);
  }
  selectionConnections.clear();
}

QWidget* AdNavigationMenu::Private::activeViewWidget() const {
  if (!rootLayout) {
    return nullptr;
  }
  return rootLayout->currentWidget();
}

detail::AdMenuTreeView* AdNavigationMenu::Private::activeTreeView() const {
  QWidget* view = activeViewWidget();
  return dynamic_cast<detail::AdMenuTreeView*>(view);
}

bool AdNavigationMenu::Private::usingOwnedModel() const {
  return model && ownedItemModel && model == ownedItemModel;
}

bool AdNavigationMenu::Private::popupLikeMode() const { return mode != Mode::Inline || collapsed; }

QItemSelectionModel* AdNavigationMenu::Private::ensureSelectionModel() {
  if (selectionModel && (!model || selectionModel->model() == model.data())) {
    return selectionModel.data();
  }
  if (!model) {
    adoptSelectionModel(nullptr, false);
    return nullptr;
  }
  auto* owned = new QItemSelectionModel(model, q);
  adoptSelectionModel(owned, true);
  return owned;
}

QItemSelectionModel* AdNavigationMenu::Private::ensureBarSelectionModel() {
  if (!barProxyModel) {
    return nullptr;
  }
  if (barSelectionModel && barSelectionModel->model() == barProxyModel) {
    return barSelectionModel;
  }
  barSelectionModel = new QItemSelectionModel(barProxyModel, q);
  if (barView) {
    barView->setSelectionModel(barSelectionModel);
  }
  return barSelectionModel;
}

QAbstractItemView::SelectionMode AdNavigationMenu::Private::qtSelectionMode() const {
  switch (selectionMode) {
    case SelectionMode::NoSelection:
      return QAbstractItemView::NoSelection;
    case SelectionMode::MultiSelection:
      return QAbstractItemView::ExtendedSelection;
    case SelectionMode::SingleSelection:
    default:
      return QAbstractItemView::SingleSelection;
  }
}

void AdNavigationMenu::Private::adoptSelectionModel(QItemSelectionModel* value, bool owned) {
  if (selectionModel == value && ownsSelectionModel == owned) {
    return;
  }
  for (const QMetaObject::Connection& connection : selectionConnections) {
    QObject::disconnect(connection);
  }
  selectionConnections.clear();
  if (ownsSelectionModel && selectionModel) {
    selectionModel->deleteLater();
  }
  selectionModel = value;
  ownsSelectionModel = owned;
  syncSelectionConnections();
  emit q->selectionModelChanged(selectionModel);
}

void AdNavigationMenu::Private::rebuildStableIdIndexCache() { state.rebuildStableIdIndexCache(); }

QModelIndex AdNavigationMenu::Private::indexForStableId(const QString& key) const {
  return state.indexForStableId(key);
}

QString AdNavigationMenu::Private::stableIdForIndexNormalized(const QModelIndex& index) const {
  return state.stableIdForIndexNormalized(index);
}

QStringList AdNavigationMenu::Private::filterKnownStableIds(const QStringList& keys,
                                                            bool submenuOnly,
                                                            const char* context) const {
  return state.filterKnownStableIds(
      keys,
      [submenuOnly](const QModelIndex& index) {
        return submenuOnly ? isSubmenuIndex(index) : isActionIndex(index);
      },
      context);
}

void AdNavigationMenu::Private::syncModelConnections() {
  for (const QMetaObject::Connection& connection : modelConnections) {
    QObject::disconnect(connection);
  }
  modelConnections.clear();
  if (!model) {
    stableIdIndexCache.clear();
    duplicateStableIds.clear();
    return;
  }
  const auto refreshStructure = [this]() {
    rebuildStableIdIndexCache();
    normalizeExpandedStableIds(expandedStableIds);
    normalizeExpandedStableIds(collapsedPopupStableIds);
    syncAllViews();
    syncPopupVisibility();
    q->updateGeometry();
    q->update();
  };
  const auto refreshData = [this, refreshStructure](const QModelIndex&, const QModelIndex&,
                                                    const QList<int>& roles) {
    const bool unknownRoles = roles.isEmpty();
    const bool requiresStructureRefresh = unknownRoles ||
                                          roles.contains(AdNavigationMenu::StableIdRole) ||
                                          roles.contains(AdNavigationMenu::NodeKindRole);
    if (requiresStructureRefresh) {
      refreshStructure();
      return;
    }

    const bool affectsGeometry =
        roles.contains(Qt::DisplayRole) || roles.contains(Qt::DecorationRole) ||
        roles.contains(AdNavigationMenu::ExtraTextRole) || roles.contains(Qt::FontRole);
    const bool affectsPopup = roles.contains(AdNavigationMenu::PopupColorSchemeRole) ||
                              roles.contains(AdNavigationMenu::PopupOffsetRole);

    if (affectsGeometry) {
      if (inlineView) {
        inlineView->refreshLayout();
      }
      if (verticalView) {
        verticalView->refreshLayout();
      }
      if (barView) {
        barView->refreshLayout();
      }
      for (const auto& level : popupLevels) {
        if (level && level->view) {
          level->view->refreshLayout();
        }
      }
      q->updateGeometry();
    }

    updateCurrentForViews();
    const QModelIndex tooltipIndex =
        hoveredIndex.isValid() ? static_cast<QModelIndex>(hoveredIndex)
                               : (selectionModel ? selectionModel->currentIndex() : QModelIndex());
    syncCollapsedTooltip(tooltipIndex);
    if (affectsPopup) {
      syncPopupVisibility();
    }
    updateAllViewports();
    q->update();
  };
  modelConnections.append(
      QObject::connect(model, &QAbstractItemModel::modelReset, q, refreshStructure));
  modelConnections.append(
      QObject::connect(model, &QAbstractItemModel::layoutChanged, q, refreshStructure));
  modelConnections.append(
      QObject::connect(model, &QAbstractItemModel::rowsInserted, q,
                       [refreshStructure](const QModelIndex&, int, int) { refreshStructure(); }));
  modelConnections.append(
      QObject::connect(model, &QAbstractItemModel::rowsRemoved, q,
                       [refreshStructure](const QModelIndex&, int, int) { refreshStructure(); }));
  modelConnections.append(
      QObject::connect(model, &QAbstractItemModel::rowsMoved, q,
                       [refreshStructure](const QModelIndex&, int, int, const QModelIndex&, int) {
                         refreshStructure();
                       }));
  modelConnections.append(
      QObject::connect(model, &QAbstractItemModel::dataChanged, q, refreshData));
}

void AdNavigationMenu::Private::syncSelectionConnections() {
  if (!selectionModel) {
    return;
  }
  selectionConnections.append(
      QObject::connect(selectionModel, &QItemSelectionModel::selectionChanged, q,
                       [this](const QItemSelection&, const QItemSelection&) {
                         syncBarSelectionState();
                         updateAllViewports();
                       }));
  selectionConnections.append(
      QObject::connect(selectionModel, &QItemSelectionModel::currentChanged, q,
                       [this](const QModelIndex& current, const QModelIndex&) {
                         updateCurrentForViews();
                         syncCollapsedTooltip(current);
                         emit q->currentIndexChanged(current);
                       }));
}

QAbstractItemDelegate* AdNavigationMenu::Private::effectiveDelegate() const {
  return customDelegate ? customDelegate.data() : static_cast<QAbstractItemDelegate*>(delegate);
}

void AdNavigationMenu::Private::syncDelegates() {
  QAbstractItemDelegate* activeDelegate = effectiveDelegate();
  if (inlineView && activeDelegate) {
    inlineView->setItemDelegate(activeDelegate);
  }
  if (verticalView && activeDelegate) {
    verticalView->setItemDelegate(activeDelegate);
  }
  if (barView && activeDelegate) {
    barView->setItemDelegate(activeDelegate);
  }
  for (const auto& level : popupLevels) {
    if (level && level->view && activeDelegate) {
      level->view->setItemDelegate(activeDelegate);
    }
  }
}

void AdNavigationMenu::Private::rebuildRootViews() {
  if (!rootLayout) {
    rootLayout = new QStackedLayout(q);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
  }
  if (!delegate) {
    delegate = new AdNavigationMenuItemDelegate(q);
  }
  if (!inlineView) {
    inlineView = new detail::AdMenuTreeView(q, q);
    inlineView->setObjectName(QStringLiteral("AdNavigationMenu-inline-view"));
    inlineView->setMouseTracking(true);
    inlineView->setHeaderHidden(true);
    inlineView->setRootIsDecorated(false);
    inlineView->setItemsExpandable(false);
    inlineView->setIndentation(0);
    inlineView->setFrameShape(QFrame::NoFrame);
    inlineView->setSelectionBehavior(QAbstractItemView::SelectRows);
    inlineView->setSelectionMode(qtSelectionMode());
    inlineView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    inlineView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inlineView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inlineView->setAllColumnsShowFocus(false);
    inlineView->setItemDelegate(effectiveDelegate());
    inlineView->keyHandler = [this](QKeyEvent* event) { return handleTreeKey(inlineView, event); };
    inlineView->enterHandler = [this]() { cancelHoverClose(); };
    inlineView->focusInHandler = [this](Qt::FocusReason reason) { handleViewFocusIn(reason); };
    inlineView->focusOutHandler = [this]() { handleViewFocusOut(); };
    inlineView->mousePressHandler = [this]() { setFocusVisible(false); };
    inlineView->leaveHandler = [this]() {
      clearCursorForView(inlineView);
      handleLeave();
    };
    QObject::connect(inlineView, &QTreeView::clicked, q,
                     [this](const QModelIndex& index) { activateSourceIndex(index, false, true); });
    QObject::connect(inlineView, &QTreeView::doubleClicked, q, [this](const QModelIndex& index) {
      if (isSubmenuIndex(index)) {
        activateSourceIndex(index, false, true);
      }
    });
    QObject::connect(inlineView, &QTreeView::entered, q, [this](const QModelIndex& index) {
      updateCursorForView(inlineView, index);
      handleHoveredIndex(index);
    });
    rootLayout->addWidget(inlineView);
  }
  if (!verticalView) {
    verticalView = new detail::AdMenuTreeView(q, q);
    verticalView->setObjectName(QStringLiteral("AdNavigationMenu-vertical-view"));
    verticalView->setMouseTracking(true);
    verticalView->setHeaderHidden(true);
    verticalView->setRootIsDecorated(false);
    verticalView->setItemsExpandable(false);
    verticalView->setIndentation(0);
    verticalView->setFrameShape(QFrame::NoFrame);
    verticalView->setSelectionBehavior(QAbstractItemView::SelectRows);
    verticalView->setSelectionMode(qtSelectionMode());
    verticalView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    verticalView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    verticalView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    verticalView->setAllColumnsShowFocus(false);
    verticalView->setItemDelegate(effectiveDelegate());
    verticalView->keyHandler = [this](QKeyEvent* event) {
      return handleTreeKey(verticalView, event);
    };
    verticalView->enterHandler = [this]() { cancelHoverClose(); };
    verticalView->focusInHandler = [this](Qt::FocusReason reason) { handleViewFocusIn(reason); };
    verticalView->focusOutHandler = [this]() { handleViewFocusOut(); };
    verticalView->mousePressHandler = [this]() { setFocusVisible(false); };
    verticalView->leaveHandler = [this]() {
      clearCursorForView(verticalView);
      handleLeave();
    };
    QObject::connect(verticalView, &QTreeView::clicked, q,
                     [this](const QModelIndex& index) { activateSourceIndex(index, false, true); });
    QObject::connect(verticalView, &QTreeView::doubleClicked, q, [this](const QModelIndex& index) {
      if (isSubmenuIndex(index)) {
        activateSourceIndex(index, false, true);
      }
    });
    QObject::connect(verticalView, &QTreeView::entered, q, [this](const QModelIndex& index) {
      updateCursorForView(verticalView, index);
      handleHoveredIndex(index);
    });
    rootLayout->addWidget(verticalView);
  }
  if (!barProxyModel) {
    barProxyModel = new detail::AdMenuBarProxyModel(q);
  }
  if (!barView) {
    barView = new detail::AdMenuBarView(q);
    barView->setObjectName(QStringLiteral("AdNavigationMenu-bar-view"));
    barView->setMouseTracking(true);
    barView->setFrameShape(QFrame::NoFrame);
    barView->setSelectionBehavior(QAbstractItemView::SelectRows);
    barView->setSelectionMode(qtSelectionMode());
    barView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    barView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    barView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    barView->setFlow(QListView::LeftToRight);
    barView->setWrapping(false);
    barView->setResizeMode(QListView::Adjust);
    barView->setMovement(QListView::Static);
    barView->setSpacing(0);
    barView->setUniformItemSizes(false);
    barView->setViewMode(QListView::ListMode);
    barView->setModel(barProxyModel);
    barView->setSelectionModel(ensureBarSelectionModel());
    barView->setItemDelegate(effectiveDelegate());
    barView->keyHandler = [this](QKeyEvent* event) { return handleBarKey(event); };
    barView->enterHandler = [this]() { cancelHoverClose(); };
    barView->focusInHandler = [this](Qt::FocusReason reason) { handleViewFocusIn(reason); };
    barView->focusOutHandler = [this]() { handleViewFocusOut(); };
    barView->mousePressHandler = [this]() { setFocusVisible(false); };
    barView->leaveHandler = [this]() {
      clearCursorForView(barView);
      handleLeave();
    };
    QObject::connect(barView, &QListView::clicked, q, [this](const QModelIndex& index) {
      activateSourceIndex(mapToSourceIndex(index), false);
    });
    QObject::connect(barView, &QListView::doubleClicked, q, [this](const QModelIndex& index) {
      const QModelIndex sourceIndex = mapToSourceIndex(index);
      if (isSubmenuIndex(sourceIndex)) {
        activateSourceIndex(sourceIndex, false);
      }
    });
    QObject::connect(barView, &QListView::entered, q, [this](const QModelIndex& index) {
      updateCursorForView(barView, mapToSourceIndex(index));
      handleHoveredIndex(mapToSourceIndex(index));
    });
    rootLayout->addWidget(barView);
  }
  syncDelegates();
  rebuildModeView();
}

void AdNavigationMenu::Private::rebuildModeView() {
  if (!rootLayout) {
    return;
  }
  if (mode == Mode::Horizontal) {
    rootLayout->setCurrentWidget(barView);
    q->setFocusProxy(barView);
  } else if (mode == Mode::Inline && !collapsed) {
    rootLayout->setCurrentWidget(inlineView);
    q->setFocusProxy(inlineView);
  } else {
    rootLayout->setCurrentWidget(verticalView);
    q->setFocusProxy(verticalView);
  }
  updateViewProperties(inlineView, Mode::Inline, false, colorScheme, 0, false);
  const Mode verticalRootMode = (mode == Mode::Inline && collapsed) ? Mode::Inline : Mode::Vertical;
  updateViewProperties(verticalView, verticalRootMode, collapsed, colorScheme, 0, false);
  updateViewProperties(barView, Mode::Horizontal, false, colorScheme, 0, false);
  updateCurrentForViews();
  updateAllViewports();
  q->update();
}

void AdNavigationMenu::Private::updateViewProperties(QWidget* view, Mode viewMode,
                                                     bool collapsedValue,
                                                     ColorScheme popupColorScheme, int rootDepth,
                                                     bool popupLevel) const {
  if (!view) {
    return;
  }
  view->setProperty("AdNavigationMenu.mode", static_cast<int>(viewMode));
  view->setProperty("AdNavigationMenu.collapsed", collapsedValue);
  view->setProperty("AdNavigationMenu.popupColorScheme", static_cast<int>(popupColorScheme));
  view->setProperty("AdNavigationMenu.rootDepth", rootDepth);
  view->setProperty("AdNavigationMenu.popupLevel", popupLevel);
  const MenuVisualStyle style = q->resolvedVisualStyle(popupLevel ? Mode::Vertical : viewMode,
                                                       popupColorScheme, collapsedValue);
  const adqt::theme::ThemeScheme themeScheme =
      adqt::theme::ThemeManager::instance().resolve(q).theme.scheme;
  const QColor backgroundColor = popupLevel ? QColor(Qt::transparent) : style.menuBackground;
  const QColor borderColor = style.borderColor;
  const int borderWidth =
      popupLevel ? 0 : rootBorderWidthForStyle(viewMode, popupColorScheme, themeScheme, style);
  int borderEdge = kMenuRootBorderNone;
  if (borderWidth > 0) {
    borderEdge = viewMode == Mode::Horizontal ? kMenuRootBorderBottom : kMenuRootBorderRight;
  }

  view->setProperty("AdNavigationMenu.backgroundColor", backgroundColor);
  view->setProperty("AdNavigationMenu.borderColor", borderColor);
  view->setProperty("AdNavigationMenu.borderWidth", borderWidth);
  view->setProperty("AdNavigationMenu.borderEdge", borderEdge);

  QPalette palette = view->palette();
  palette.setColor(QPalette::Base, backgroundColor);
  palette.setColor(QPalette::Window, backgroundColor);
  view->setPalette(palette);
  if (auto* itemView = qobject_cast<QAbstractItemView*>(view)) {
    if (QWidget* viewport = itemView->viewport()) {
      viewport->setPalette(palette);
      viewport->update();
    }
  }

  view->update();
}

void AdNavigationMenu::Private::syncTreeView(detail::AdMenuTreeView* view,
                                             const QModelIndex& rootIndex, bool inlineMode) {
  if (!view) {
    return;
  }
  if (view->model() != model) {
    view->setModel(model);
  }
  if (view->selectionModel() != ensureSelectionModel()) {
    view->setSelectionModel(ensureSelectionModel());
  }
  view->setSelectionBehavior(QAbstractItemView::SelectRows);
  view->setSelectionMode(qtSelectionMode());
  if (view->rootIndex() != rootIndex) {
    view->setRootIndex(rootIndex);
  }
  const bool popupLevel = rootIndex.isValid();
  const Mode rootViewMode =
      inlineMode
          ? Mode::Inline
          : ((mode == Mode::Inline && collapsed && !popupLevel) ? Mode::Inline : Mode::Vertical);
  updateViewProperties(view, rootViewMode, inlineMode ? false : collapsed, colorScheme,
                       popupLevel ? sourceDepth(rootIndex) + 1 : 0, popupLevel);

  if (!model) {
    return;
  }

  std::function<void(const QModelIndex&)> syncChildren;
  syncChildren = [this, view, inlineMode, &syncChildren](const QModelIndex& parentIndex) {
    const int rowCount = model->rowCount(parentIndex);
    for (int row = 0; row < rowCount; ++row) {
      const QModelIndex child = model->index(row, 0, parentIndex);
      if (!child.isValid()) {
        continue;
      }
      if (isGroupIndex(child)) {
        view->setExpanded(child, true);
        syncChildren(child);
      } else if (isSubmenuIndex(child)) {
        const bool expanded = inlineMode ? containsExpanded(child) : false;
        view->setExpanded(child, expanded);
        if (expanded) {
          syncChildren(child);
        }
      }
    }
  };
  syncChildren(rootIndex);
  view->refreshLayout();
}

void AdNavigationMenu::Private::syncAllViews() {
  hideTooltip();
  rebuildRootViews();
  barProxyModel->setSourceModel(model);
  syncTreeView(inlineView, QModelIndex(), true);
  syncTreeView(verticalView, QModelIndex(), false);
  syncViewSelectionModels();
  syncBarSelectionState();
  updateCurrentForViews();
  updateAllViewports();
  const QModelIndex tooltipIndex =
      hoveredIndex.isValid() ? static_cast<QModelIndex>(hoveredIndex)
                             : (selectionModel ? selectionModel->currentIndex() : QModelIndex());
  syncCollapsedTooltip(tooltipIndex);
}

void AdNavigationMenu::Private::syncViewSelectionModels() {
  QItemSelectionModel* sourceSelection = ensureSelectionModel();
  if (inlineView && inlineView->model() == model &&
      inlineView->selectionModel() != sourceSelection) {
    inlineView->setSelectionModel(sourceSelection);
  }
  if (verticalView && verticalView->model() == model &&
      verticalView->selectionModel() != sourceSelection) {
    verticalView->setSelectionModel(sourceSelection);
  }
  if (inlineView) {
    inlineView->setSelectionBehavior(QAbstractItemView::SelectRows);
    inlineView->setSelectionMode(qtSelectionMode());
  }
  if (verticalView) {
    verticalView->setSelectionBehavior(QAbstractItemView::SelectRows);
    verticalView->setSelectionMode(qtSelectionMode());
  }
  if (barView) {
    barView->setSelectionBehavior(QAbstractItemView::SelectRows);
    barView->setSelectionMode(qtSelectionMode());
    if (barView->selectionModel() != ensureBarSelectionModel()) {
      barView->setSelectionModel(ensureBarSelectionModel());
    }
  }
  for (const auto& level : popupLevels) {
    if (!level || !level->view) {
      continue;
    }
    if (level->view->model() == model && level->view->selectionModel() != sourceSelection) {
      level->view->setSelectionModel(sourceSelection);
    }
    level->view->setSelectionBehavior(QAbstractItemView::SelectRows);
    level->view->setSelectionMode(qtSelectionMode());
  }
}

void AdNavigationMenu::Private::syncBarSelectionState() {
  QItemSelectionModel* barSm = ensureBarSelectionModel();
  if (!barSm || !barProxyModel) {
    return;
  }

  if (!model || !selectionModel) {
    QSignalBlocker blocker(barSm);
    barSm->clearSelection();
    barSm->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
    return;
  }

  QSignalBlocker blocker(barSm);
  barSm->clearSelection();
  barSm->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);

  QSet<int> selectedRows;
  std::function<void(const QModelIndex&)> walk;
  walk = [this, &selectedRows, &walk](const QModelIndex& parent) {
    if (!model || !selectionModel) {
      return;
    }
    const int rowCount = model->rowCount(parent);
    for (int row = 0; row < rowCount; ++row) {
      const QModelIndex index = model->index(row, 0, parent);
      if (!index.isValid()) {
        continue;
      }
      if (selectionModel->isSelected(index)) {
        const QModelIndex top = topLevelAncestor(index);
        if (top.isValid()) {
          selectedRows.insert(top.row());
        }
      }
      if (model->rowCount(index) > 0) {
        walk(index);
      }
    }
  };
  walk(QModelIndex());

  if (selectionMode != SelectionMode::NoSelection) {
    bool first = true;
    const QList<int> rows = selectedRows.values();
    for (int row : rows) {
      const QModelIndex proxyIndex = barProxyModel->index(row, 0, QModelIndex());
      if (!proxyIndex.isValid()) {
        continue;
      }
      const auto flags =
          (first ? QItemSelectionModel::ClearAndSelect : QItemSelectionModel::Select) |
          QItemSelectionModel::Rows;
      barSm->select(proxyIndex, flags);
      first = false;
      if (selectionMode == SelectionMode::SingleSelection) {
        break;
      }
    }
  }

  const QModelIndex currentSource = selectionModel->currentIndex();
  const QModelIndex currentProxy =
      currentSource.isValid() ? mapFromSourceIndex(barProxyModel, topLevelAncestor(currentSource))
                              : QModelIndex();
  barSm->setCurrentIndex(currentProxy, QItemSelectionModel::NoUpdate);
}

void AdNavigationMenu::Private::updateAllViewports() {
  if (inlineView && inlineView->viewport()) {
    inlineView->viewport()->update();
  }
  if (verticalView && verticalView->viewport()) {
    verticalView->viewport()->update();
  }
  if (barView && barView->viewport()) {
    barView->viewport()->update();
  }
  for (const auto& level : popupLevels) {
    if (level && level->view && level->view->viewport()) {
      level->view->viewport()->update();
      if (level->shell) {
        level->shell->update();
      }
    }
  }
}

void AdNavigationMenu::Private::normalizeExpandedStableIds(QStringList& keys) const {
  state.normalizeExpandedStableIds(keys,
                                   [](const QModelIndex& index) { return isSubmenuIndex(index); });
}

QStringList& AdNavigationMenu::Private::visibleExpandedStableIds() {
  return state.visibleExpandedStableIds(mode, collapsed);
}

const QStringList& AdNavigationMenu::Private::visibleExpandedStableIds() const {
  return state.visibleExpandedStableIds(mode, collapsed);
}

void AdNavigationMenu::Private::applyExpandedStateChange(QStringList& targetExpanded,
                                                         const QModelIndex& index, bool expanded,
                                                         bool popupLikeMode, const Private* self) {
  if (!isSubmenuIndex(index)) {
    return;
  }
  const QString key = self ? self->stableIdForIndexNormalized(index) : QString();
  if (key.isEmpty()) {
    return;
  }

  auto removeIndex = [&targetExpanded](const QString& targetKey) {
    for (qsizetype i = targetExpanded.size() - 1; i >= 0; --i) {
      if (targetExpanded.at(i) == targetKey) {
        targetExpanded.removeAt(i);
      }
    }
  };

  auto removeWithDescendants = [&targetExpanded, self](const QModelIndex& parentIndex) {
    for (qsizetype i = targetExpanded.size() - 1; i >= 0; --i) {
      const QModelIndex candidate =
          self ? self->indexForStableId(targetExpanded.at(i)) : QModelIndex();
      QModelIndex current = candidate;
      while (current.isValid()) {
        if (current == parentIndex) {
          targetExpanded.removeAt(i);
          break;
        }
        current = current.parent();
      }
    }
  };

  if (expanded) {
    if (targetExpanded.contains(key)) {
      return;
    }
    if (popupLikeMode) {
      for (qsizetype i = targetExpanded.size() - 1; i >= 0; --i) {
        const QModelIndex candidate =
            self ? self->indexForStableId(targetExpanded.at(i)) : QModelIndex();
        bool keep = false;
        QModelIndex current = index.parent();
        while (current.isValid()) {
          if (current == candidate) {
            keep = true;
            break;
          }
          current = current.parent();
        }
        current = candidate.parent();
        while (!keep && current.isValid()) {
          if (current == index) {
            keep = true;
            break;
          }
          current = current.parent();
        }
        if (!keep) {
          targetExpanded.removeAt(i);
        }
      }
    }
    targetExpanded.append(key);
    targetExpanded = uniqueStringList(targetExpanded);
    return;
  }

  if (popupLikeMode) {
    removeWithDescendants(index);
  } else {
    removeIndex(key);
  }
}

void AdNavigationMenu::Private::applySelectionStateFromStableIds(const QStringList& keys) {
  QItemSelectionModel* sm = ensureSelectionModel();
  if (!sm) {
    return;
  }

  const QStringList normalized = filterKnownStableIds(keys, false, nullptr);
  QSignalBlocker blocker(sm);
  const QModelIndex previousCurrent = sm->currentIndex();
  sm->clearSelection();
  sm->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);

  QModelIndex current;
  bool first = true;
  for (const QString& key : normalized) {
    const QModelIndex index = indexForStableId(key);
    if (!index.isValid() || selectionMode == SelectionMode::NoSelection) {
      continue;
    }
    const auto flags = (first ? QItemSelectionModel::ClearAndSelect : QItemSelectionModel::Select) |
                       QItemSelectionModel::Rows;
    sm->select(index, flags);
    if (!current.isValid()) {
      current = index;
    }
    first = false;
    if (selectionMode != SelectionMode::MultiSelection) {
      break;
    }
  }
  if (current.isValid()) {
    sm->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
  }
  updateCurrentForViews();
  syncCollapsedTooltip(sm->currentIndex());
  updateAllViewports();
  if (previousCurrent != sm->currentIndex()) {
    emit q->currentIndexChanged(sm->currentIndex());
  }
}

bool AdNavigationMenu::Private::containsExpanded(const QModelIndex& index) const {
  return state.containsExpanded(index, mode, collapsed);
}

void AdNavigationMenu::Private::setExpandedInternal(const QModelIndex& index, bool expanded,
                                                    bool emitSignal) {
  if (!isSubmenuIndex(index)) {
    return;
  }

  QStringList& targetExpanded = visibleExpandedStableIds();

  if (expanded) {
    if (containsExpanded(index)) {
      syncPopupVisibility();
      updateAllViewports();
      return;
    }
    applyExpandedStateChange(targetExpanded, index, true, popupLikeMode(), this);
    if (emitSignal) {
      emit q->expanded(index);
    }
  } else {
    const bool hadIndex = containsExpanded(index);
    applyExpandedStateChange(targetExpanded, index, false, popupLikeMode(), this);
    if (emitSignal && hadIndex) {
      emit q->collapsed(index);
    }
  }
  normalizeExpandedStableIds(targetExpanded);

  syncAllViews();
  syncPopupVisibility();
  q->updateGeometry();
  if (mode == Mode::Inline && !collapsed) {
    q->adjustSize();
    if (QWidget* parent = q->parentWidget()) {
      parent->updateGeometry();
      if (QLayout* layout = parent->layout()) {
        layout->activate();
      }
    }
  }
}

void AdNavigationMenu::Private::clearPopupExpandedSubmenus() {
  if (!popupLikeMode()) {
    return;
  }
  QStringList& targetExpanded = visibleExpandedStableIds();
  if (targetExpanded.isEmpty()) {
    return;
  }
  targetExpanded.clear();
}

QVector<QModelIndex> AdNavigationMenu::Private::popupChain() const {
  QVector<QModelIndex> chain;
  if (!model || !popupLikeMode()) {
    return chain;
  }
  QModelIndex parentIndex;
  while (true) {
    QModelIndex found;
    const int rowCount = model->rowCount(parentIndex);
    for (int row = 0; row < rowCount; ++row) {
      const QModelIndex child = model->index(row, 0, parentIndex);
      if (isSubmenuIndex(child) && containsExpanded(child)) {
        found = child;
        break;
      }
    }
    if (!found.isValid()) {
      break;
    }
    chain.append(found);
    parentIndex = found;
  }
  return chain;
}

void AdNavigationMenu::Private::ensurePopupLevel(int levelIndex) {
  while (static_cast<int>(popupLevels.size()) <= levelIndex) {
    auto popup = std::make_unique<PopupLevel>();
    popup->shell = new detail::AdMenuPopupShell(q, detail::resolvePopupScopeWindow(q));
    popup->shell->setObjectName(
        levelIndex == 0 ? QStringLiteral("AdNavigationMenu-popup-window")
                        : QStringLiteral("AdNavigationMenu-popup-window-%1").arg(levelIndex));
    popup->shell->installEventFilter(q);
    popup->view = new detail::AdMenuTreeView(q, popup->shell);
    popup->view->setMouseTracking(true);
    popup->view->setHeaderHidden(true);
    popup->view->setRootIsDecorated(false);
    popup->view->setItemsExpandable(false);
    popup->view->setIndentation(0);
    popup->view->setFrameShape(QFrame::NoFrame);
    popup->view->setSelectionBehavior(QAbstractItemView::SelectRows);
    popup->view->setSelectionMode(qtSelectionMode());
    popup->view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    popup->view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    popup->view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    popup->view->setAllColumnsShowFocus(false);
    popup->view->setItemDelegate(effectiveDelegate());
    popup->view->keyHandler = [this, view = popup->view.data()](QKeyEvent* event) {
      return handleTreeKey(view, event);
    };
    popup->view->enterHandler = [this]() { cancelHoverClose(); };
    popup->view->focusInHandler = [this](Qt::FocusReason reason) { handleViewFocusIn(reason); };
    popup->view->focusOutHandler = [this]() { handleViewFocusOut(); };
    popup->view->mousePressHandler = [this]() { setFocusVisible(false); };
    popup->view->leaveHandler = [this, popupView = popup->view.data()]() {
      clearCursorForView(popupView);
      handleLeave();
    };
    QObject::connect(popup->view, &QTreeView::clicked, q,
                     [this](const QModelIndex& index) { activateSourceIndex(index, false, true); });
    QObject::connect(popup->view, &QTreeView::doubleClicked, q, [this](const QModelIndex& index) {
      if (isSubmenuIndex(index)) {
        activateSourceIndex(index, false, true);
      }
    });
    QObject::connect(popup->view, &QTreeView::entered, q,
                     [this, popupView = popup->view.data()](const QModelIndex& index) {
                       updateCursorForView(popupView, index);
                       handleHoveredIndex(index);
                     });
    popup->shell->layout()->addWidget(popup->view);
    popup->renderedRoot = popup->view;
    popupLevels.push_back(std::move(popup));
  }
}

void AdNavigationMenu::Private::syncPopupLevel(PopupLevel& level, const QModelIndex& submenuIndex) {
  level.submenuIndex = submenuIndex;
  level.popupColorScheme = popupColorSchemeForIndex(submenuIndex, colorScheme);
  if (level.shell) {
    level.shell->setPopupColorScheme(level.popupColorScheme);
  }
  updateViewProperties(level.view, Mode::Vertical, false, level.popupColorScheme,
                       sourceDepth(submenuIndex) + 1, true);
  syncTreeView(level.view, submenuIndex, false);

  const bool wantsCustomPopup = !popupFactory.isNull();
  const bool needsRebuild = !level.renderedRoot || level.renderedForIndex != submenuIndex ||
                            level.renderedWithCustomPopup != wantsCustomPopup;
  if (!needsRebuild || !level.shell) {
    return;
  }

  if (level.view && level.view->parentWidget() && level.view->parentWidget() != level.shell) {
    level.view->setParent(level.shell);
  }
  if (level.renderedRoot && level.renderedRoot != level.view) {
    level.renderedRoot->removeEventFilter(q);
    level.renderedRoot->hide();
    level.renderedRoot->deleteLater();
  }

  QWidget* renderedPopup = level.view;
  if (wantsCustomPopup) {
    auto* defaultPopup = new QWidget(level.shell);
    auto* defaultLayout = new QVBoxLayout(defaultPopup);
    defaultLayout->setContentsMargins(0, 0, 0, 0);
    defaultLayout->setSpacing(0);
    defaultLayout->addWidget(level.view);

    AdNavigationMenuPopupContext ctx;
    ctx.submenuIndex = submenuIndex;
    QWidget* custom =
        popupFactory ? popupFactory->createPopup(ctx, defaultPopup, level.shell) : nullptr;
    if (custom) {
      renderedPopup = custom;
    } else {
      renderedPopup = defaultPopup;
    }
    if (renderedPopup != defaultPopup && defaultPopup->parentWidget() == level.shell) {
      defaultPopup->deleteLater();
    }
  }

  if (renderedPopup->parentWidget() != level.shell) {
    renderedPopup->setParent(level.shell);
  }
  if (renderedPopup != level.view) {
    renderedPopup->removeEventFilter(q);
    renderedPopup->installEventFilter(q);
  }
  clearLayoutItems(level.shell->layout());
  level.shell->layout()->addWidget(renderedPopup);
  if (level.view) {
    level.view->show();
  }
  renderedPopup->show();
  level.shell->layout()->activate();
  level.renderedRoot = renderedPopup;
  level.renderedForIndex = submenuIndex;
  level.renderedWithCustomPopup = wantsCustomPopup;
}

QSize AdNavigationMenu::Private::popupContentSize(const QModelIndex& submenuIndex,
                                                  ColorScheme popupColorScheme,
                                                  const QWidget* measuringWidget) const {
  const MenuVisualStyle style = q->resolvedVisualStyle(Mode::Vertical, popupColorScheme, false);
  QVector<QModelIndex> rows;
  appendVisibleRows(submenuIndex, false, rows);
  int width = kAntdDropdownMinWidth;
  int height = 0;
  QModelIndex lastRow;
  QStyleOptionViewItem option;
  option.widget = measuringWidget;
  QAbstractItemDelegate* activeDelegate = effectiveDelegate();
  if (!activeDelegate) {
    return QSize(width, style.metrics.itemHeight);
  }
  for (const QModelIndex& row : rows) {
    const QSize hint = activeDelegate->sizeHint(option, row);
    width = std::max(width, hint.width());
    height += hint.height();
    lastRow = row;
  }
  if (isMenuRowIndex(lastRow)) {
    height += style.metrics.itemMarginBlock;
  }
  height += std::max(0, style.metrics.borderWidth);
  // A single-row submenu should fit its row instead of reserving a second empty row.
  return QSize(width + std::max(0, style.metrics.borderWidth) * 2,
               std::max(height, style.metrics.itemHeight));
}

QSize AdNavigationMenu::Private::rootContentSize() const {
  const MenuVisualStyle style = q->resolvedVisualStyle(mode, colorScheme, collapsed);
  const adqt::theme::ThemeScheme themeScheme =
      adqt::theme::ThemeManager::instance().resolve(q).theme.scheme;
  const int collapsedWidth = collapsedInlineRootWidth(style);
  if (!model) {
    if (mode == Mode::Horizontal) {
      return QSize(160, std::max(1, style.metrics.horizontalLineHeight) +
                            rootBorderWidthForStyle(mode, colorScheme, themeScheme, style));
    }
    return QSize(collapsed ? collapsedWidth : 256,
                 style.metrics.itemHeight + style.metrics.itemMarginBlock * 2);
  }
  QVector<QModelIndex> rows;
  appendVisibleRows(QModelIndex(), mode == Mode::Inline && !collapsed, rows);
  int width = mode == Mode::Horizontal ? 0 : (collapsed ? collapsedWidth : 180);
  int height = 0;
  int visibleCount = 0;
  QModelIndex lastRow;
  QStyleOptionViewItem option;
  option.widget = mode == Mode::Horizontal ? static_cast<QWidget*>(barView)
                                           : static_cast<QWidget*>(activeTreeView());
  QAbstractItemDelegate* activeDelegate = effectiveDelegate();
  if (!activeDelegate) {
    return QSize(mode == Mode::Horizontal ? 160 : (collapsed ? collapsedWidth : 180),
                 std::max(style.metrics.itemHeight, style.metrics.horizontalLineHeight));
  }
  for (const QModelIndex& row : rows) {
    const QSize hint = activeDelegate->sizeHint(option, row);
    if (mode == Mode::Horizontal) {
      if (visibleCount > 0) {
        width += style.metrics.horizontalSpacing;
      }
      width += hint.width();
      height = std::max(
          height, hint.height() + rootBorderWidthForStyle(mode, colorScheme, themeScheme, style));
    } else {
      width = std::max(width, hint.width());
      height += hint.height();
      lastRow = row;
    }
    ++visibleCount;
  }
  if (mode == Mode::Horizontal) {
    return QSize(
        std::max(160, width),
        std::max(height, std::max(1, style.metrics.horizontalLineHeight) +
                             rootBorderWidthForStyle(mode, colorScheme, themeScheme, style)));
  }
  if (isMenuRowIndex(lastRow)) {
    height += style.metrics.itemMarginBlock;
  }
  return QSize(width,
               std::max(height, style.metrics.itemHeight + style.metrics.itemMarginBlock * 2));
}

void AdNavigationMenu::Private::appendVisibleRows(const QModelIndex& parentIndex, bool inlineMode,
                                                  QVector<QModelIndex>& out) const {
  if (!model) {
    return;
  }
  const int rowCount = model->rowCount(parentIndex);
  for (int row = 0; row < rowCount; ++row) {
    const QModelIndex child = model->index(row, 0, parentIndex);
    if (!child.isValid()) {
      continue;
    }
    out.append(child);
    if (isGroupIndex(child) || (inlineMode && isSubmenuIndex(child) && containsExpanded(child))) {
      appendVisibleRows(child, inlineMode, out);
    }
  }
}

bool AdNavigationMenu::Private::hasSelectedDescendant(const QModelIndex& sourceIndex) const {
  if (!selectionModel || !sourceIndex.isValid()) {
    return false;
  }
  const int rowCount = model ? model->rowCount(sourceIndex) : 0;
  for (int row = 0; row < rowCount; ++row) {
    const QModelIndex child = model->index(row, 0, sourceIndex);
    if (!child.isValid()) {
      continue;
    }
    if (selectionModel->isSelected(child) || hasSelectedDescendant(child)) {
      return true;
    }
  }
  return false;
}

bool AdNavigationMenu::Private::isSourceIndexSelected(const QModelIndex& sourceIndex) const {
  if (!sourceIndex.isValid() || isSeparatorIndex(sourceIndex) || isGroupIndex(sourceIndex) ||
      !selectionModel) {
    return false;
  }
  return selectionModel->isSelected(sourceIndex) ||
         (isSubmenuIndex(sourceIndex) && hasSelectedDescendant(sourceIndex));
}

bool AdNavigationMenu::Private::hasSelectedDescendantForStyle(
    const QModelIndex& sourceIndex) const {
  return sourceIndex.isValid() && isSubmenuIndex(sourceIndex) && hasSelectedDescendant(sourceIndex);
}

bool AdNavigationMenu::Private::shouldUseSelectedState(const QModelIndex& sourceIndex) const {
  return isSourceIndexSelected(sourceIndex);
}

bool AdNavigationMenu::Private::hasFocusWithin() const {
  QWidget* focusWidget = QApplication::focusWidget();
  if (!focusWidget) {
    return false;
  }
  if (focusWidget == q || q->isAncestorOf(focusWidget)) {
    return true;
  }
  return std::any_of(popupLevels.cbegin(), popupLevels.cend(), [focusWidget](const auto& level) {
    return level && level->shell &&
           (focusWidget == level->shell || level->shell->isAncestorOf(focusWidget));
  });
}

bool AdNavigationMenu::Private::isSourceIndexCurrent(const QModelIndex& sourceIndex) const {
  return selectionModel && selectionModel->currentIndex() == sourceIndex;
}

bool AdNavigationMenu::Private::shouldPaintCurrentAsActive(const QModelIndex& sourceIndex) const {
  return focusVisible && hasFocusWithin() && isSourceIndexCurrent(sourceIndex);
}

void AdNavigationMenu::Private::setFocusVisible(bool visible) {
  if (focusVisible == visible) {
    return;
  }
  focusVisible = visible;
  updateAllViewports();
}

void AdNavigationMenu::Private::handleViewFocusIn(Qt::FocusReason reason) {
  setFocusVisible(isKeyboardFocusReason(reason));
}

void AdNavigationMenu::Private::handleViewFocusOut() {
  QPointer<AdNavigationMenu> guard = q;
  QTimer::singleShot(0, q, [this, guard]() {
    if (!guard) {
      return;
    }
    if (!hasFocusWithin()) {
      setFocusVisible(false);
    } else {
      updateAllViewports();
    }
  });
}

bool AdNavigationMenu::Private::applySelectionTrigger(const QModelIndex& sourceIndex) {
  QItemSelectionModel* sm = ensureSelectionModel();
  if (!sm || !sourceIndex.isValid()) {
    return false;
  }
  sm->setCurrentIndex(sourceIndex, QItemSelectionModel::NoUpdate);
  if (selectionMode == SelectionMode::NoSelection) {
    return false;
  }
  if (selectionMode == SelectionMode::SingleSelection) {
    sm->select(sourceIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    return true;
  }
  const bool alreadySelected = sm->isSelected(sourceIndex);
  sm->select(sourceIndex,
             (alreadySelected ? QItemSelectionModel::Deselect : QItemSelectionModel::Select) |
                 QItemSelectionModel::Rows);
  return true;
}

QStringList AdNavigationMenu::Private::selectedStableIdsForState() const {
  QStringList keys;
  if (!model || !selectionModel) {
    return keys;
  }

  std::function<void(const QModelIndex&)> walk;
  walk = [this, &keys, &walk](const QModelIndex& parent) {
    if (!model) {
      return;
    }
    const int rowCount = model->rowCount(parent);
    for (int row = 0; row < rowCount; ++row) {
      const QModelIndex index = model->index(row, 0, parent);
      if (!index.isValid()) {
        continue;
      }
      if (selectionModel->isSelected(index) && isActionIndex(index)) {
        const QString key = stableIdForIndex(index).trimmed();
        if (!key.isEmpty()) {
          keys.append(key);
        }
      }
      if (model->rowCount(index) > 0) {
        walk(index);
      }
    }
  };
  walk(QModelIndex());

  keys = uniqueStringList(keys);
  if (selectionMode != SelectionMode::MultiSelection && keys.size() > 1) {
    keys = QStringList{keys.constFirst()};
  }
  return keys;
}

void AdNavigationMenu::Private::syncModeTransitionState(Mode previousMode, bool previousCollapsed) {
  const bool wasInlineExpanded = previousMode == Mode::Inline && !previousCollapsed;
  const bool isInlineExpanded = mode == Mode::Inline && !collapsed;
  const bool isInlineCollapsed = mode == Mode::Inline && collapsed;

  if (wasInlineExpanded) {
    inlineExpandedCacheStableIds = expandedStableIds;
    collapsedPopupStableIds.clear();
    if (!isInlineExpanded && mode != Mode::Inline) {
      expandedStableIds.clear();
    }
  }

  if (!wasInlineExpanded && isInlineExpanded) {
    collapsedPopupStableIds.clear();
    if (expandedStableIds.isEmpty() && !inlineExpandedCacheStableIds.isEmpty()) {
      expandedStableIds = filterKnownStableIds(inlineExpandedCacheStableIds, true, nullptr);
    }
  } else if (!isInlineCollapsed) {
    collapsedPopupStableIds.clear();
  }
}

void AdNavigationMenu::Private::setCurrentIndexInternal(const QModelIndex& sourceIndex,
                                                        bool emitSignal) {
  QItemSelectionModel* sm = ensureSelectionModel();
  if (!sm) {
    return;
  }
  const QModelIndex previous = sm->currentIndex();
  if (previous == sourceIndex) {
    return;
  }
  sm->setCurrentIndex(sourceIndex, QItemSelectionModel::NoUpdate);
  if (emitSignal) {
    emit q->currentIndexChanged(sourceIndex);
  }
}

void AdNavigationMenu::Private::restoreCurrentFromSelection() {
  if (!model || !selectionModel) {
    return;
  }
  QModelIndex selectedAction;
  std::function<void(const QModelIndex&)> walk;
  walk = [this, &selectedAction, &walk](const QModelIndex& parent) {
    if (selectedAction.isValid() || !model) {
      return;
    }
    const int rowCount = model->rowCount(parent);
    for (int row = 0; row < rowCount; ++row) {
      const QModelIndex index = model->index(row, 0, parent);
      if (!index.isValid()) {
        continue;
      }
      if (selectionModel->isSelected(index) && isActionIndex(index)) {
        selectedAction = index;
        return;
      }
      if (model->rowCount(index) > 0) {
        walk(index);
      }
    }
  };
  walk(QModelIndex());
  if (selectedAction.isValid() && selectionModel->currentIndex() != selectedAction) {
    setCurrentIndexInternal(selectedAction, false);
  }
}

QModelIndex AdNavigationMenu::Private::topLevelAncestor(const QModelIndex& sourceIndex) const {
  QModelIndex current = sourceIndex;
  QModelIndex previous = sourceIndex;
  while (current.isValid()) {
    previous = current;
    current = current.parent();
  }
  return previous;
}

void AdNavigationMenu::Private::updateCurrentForViews() {
  if (updatingCurrentForViews) {
    return;
  }
  updatingCurrentForViews = true;
  syncBarSelectionState();
  updateAllViewports();
  updatingCurrentForViews = false;
}

void AdNavigationMenu::Private::activateSourceIndex(const QModelIndex& sourceIndex,
                                                    bool fromKeyboard,
                                                    bool selectionAlreadyHandled) {
  if (!isInteractiveIndex(sourceIndex)) {
    return;
  }
  if (isSubmenuIndex(sourceIndex)) {
    if (!fromKeyboard) {
      restoreCurrentFromSelection();
    }
    if (mode == Mode::Inline && !collapsed) {
      const bool nextExpanded = !containsExpanded(sourceIndex);
      setExpandedInternal(sourceIndex, nextExpanded, true);
      if (fromKeyboard && nextExpanded && popupLikeMode()) {
        focusPopupForIndexDeferred(sourceIndex);
      }
      return;
    }
    if (submenuTrigger == TriggerSubMenuAction::Click || fromKeyboard) {
      const bool nextExpanded = !containsExpanded(sourceIndex);
      setExpandedInternal(sourceIndex, nextExpanded, true);
      if (fromKeyboard && nextExpanded) {
        focusPopupForIndexDeferred(sourceIndex);
      }
      return;
    }
    return;
  }
  if (fromKeyboard || !selectionAlreadyHandled) {
    applySelectionTrigger(sourceIndex);
  }
  emit q->activated(sourceIndex);
  if (popupLikeMode()) {
    clearPopupExpandedSubmenus();
    syncAllViews();
    syncPopupVisibility();
  }
}

void AdNavigationMenu::Private::applyPendingHoverOpen() {
  if (!pendingHoverIndex.isValid() || !isSubmenuIndex(pendingHoverIndex)) {
    return;
  }
  setExpandedInternal(pendingHoverIndex, true, true);
}

void AdNavigationMenu::Private::openFromHover(const QModelIndex& sourceIndex) {
  if (!popupLikeMode() || submenuTrigger != TriggerSubMenuAction::Hover) {
    return;
  }
  pendingHoverIndex = sourceIndex;
  hoverCloseTimer.stop();
  hoverOpenTimer.stop();
  if (!sourceIndex.isValid() || !isSubmenuIndex(sourceIndex)) {
    return;
  }
  const int delayMs = std::max(0, detail::resolveMenuOpenDelayMs(submenuOpenDelayMs));
  if (delayMs <= 0) {
    applyPendingHoverOpen();
    return;
  }
  hoverOpenTimer.start(delayMs);
}

void AdNavigationMenu::Private::scheduleHoverClose() {
  if (!popupLikeMode() || submenuTrigger != TriggerSubMenuAction::Hover) {
    return;
  }
  bool hasVisiblePopup = false;
  for (const auto& level : popupLevels) {
    if (level && level->shell && level->shell->isVisible()) {
      hasVisiblePopup = true;
      break;
    }
  }
  if (!hasVisiblePopup) {
    hoverCloseTimer.stop();
    return;
  }
  hoverCloseTimer.stop();
  const int delayMs = std::max(0, detail::resolveMenuCloseDelayMs(submenuCloseDelayMs));
  if (delayMs <= 0) {
    closeDanglingPopups();
    return;
  }
  hoverCloseTimer.start(delayMs);
}

void AdNavigationMenu::Private::cancelHoverClose() { hoverCloseTimer.stop(); }

void AdNavigationMenu::Private::closeDanglingPopups() {
  const QPoint cursorPos = QCursor::pos();
  if (detail::widgetContainsGlobalPos(q, cursorPos)) {
    return;
  }
  for (const auto& level : popupLevels) {
    if (level && level->shell && level->shell->isVisible() &&
        detail::widgetContainsGlobalPos(level->shell, cursorPos)) {
      return;
    }
  }
  clearPopupExpandedSubmenus();
  syncAllViews();
  syncPopupVisibility();
}

void AdNavigationMenu::Private::handleHoveredIndex(const QModelIndex& sourceIndex) {
  hoveredIndex = sourceIndex;
  const bool hoveredCollapsedInlineLeaf =
      mode == Mode::Inline && collapsed && sourceIndex.isValid() &&
      !sourceIndex.parent().isValid() && !isSubmenuIndex(sourceIndex);
  if (hoveredCollapsedInlineLeaf && popupLikeMode() &&
      submenuTrigger == TriggerSubMenuAction::Hover) {
    pendingHoverIndex = QModelIndex();
    hoverOpenTimer.stop();
    hideTooltip();

    QPointer<AdNavigationMenu> guard = q;
    const QPersistentModelIndex persistent(sourceIndex);
    QTimer::singleShot(0, q, [this, guard, persistent]() {
      if (!guard || !persistent.isValid() || hoveredIndex != persistent) {
        return;
      }
      if (!visibleExpandedStableIds().isEmpty()) {
        clearPopupExpandedSubmenus();
        syncAllViews();
        syncPopupVisibility();
      }
      syncCollapsedTooltip(persistent);
    });
    scheduleHoverClose();
    return;
  }
  syncCollapsedTooltip(sourceIndex);
  if (isSubmenuIndex(sourceIndex)) {
    openFromHover(sourceIndex);
  } else {
    pendingHoverIndex = QModelIndex();
    hoverOpenTimer.stop();
    scheduleHoverClose();
  }
}

void AdNavigationMenu::Private::handleLeave() {
  hoveredIndex = QModelIndex();
  hideTooltip();
  scheduleHoverClose();
}

void AdNavigationMenu::Private::updateCursorForView(QWidget* target,
                                                    const QModelIndex& sourceIndex) {
  QWidget* cursorTarget = target;
  if (auto* itemView = qobject_cast<QAbstractItemView*>(target)) {
    cursorTarget = itemView->viewport();
  }
  if (!cursorTarget) {
    return;
  }
  const bool cursorEligible =
      sourceIndex.isValid() && (isActionIndex(sourceIndex) || isSubmenuIndex(sourceIndex));
  if (!cursorEligible) {
    clearCursorForView(target);
    return;
  }
  const bool enabled = sourceIndex.flags().testFlag(Qt::ItemIsEnabled);
  cursorTarget->setCursor(enabled ? Qt::PointingHandCursor : Qt::ForbiddenCursor);
}

void AdNavigationMenu::Private::clearCursorForView(QWidget* target) {
  QWidget* cursorTarget = target;
  if (auto* itemView = qobject_cast<QAbstractItemView*>(target)) {
    cursorTarget = itemView->viewport();
  }
  if (!cursorTarget) {
    return;
  }
  if (cursorTarget->testAttribute(Qt::WA_SetCursor)) {
    cursorTarget->unsetCursor();
  }
}

QModelIndex AdNavigationMenu::Private::firstVisibleIndex(detail::AdMenuTreeView* view) const {
  if (!view || !view->model() || view->model()->rowCount(view->rootIndex()) <= 0) {
    return QModelIndex();
  }
  return view->model()->index(0, 0, view->rootIndex());
}

QModelIndex AdNavigationMenu::Private::lastVisibleIndex(detail::AdMenuTreeView* view) const {
  QModelIndex current = firstVisibleIndex(view);
  if (!current.isValid()) {
    return QModelIndex();
  }
  while (true) {
    const QModelIndex next = view->indexBelow(current);
    if (!next.isValid()) {
      return current;
    }
    current = next;
  }
}

QModelIndex AdNavigationMenu::Private::nextNavigableIndex(detail::AdMenuTreeView* view,
                                                          const QModelIndex& start,
                                                          int delta) const {
  QModelIndex current = start;
  if (!current.isValid()) {
    current = delta >= 0 ? firstVisibleIndex(view) : lastVisibleIndex(view);
  } else {
    current = delta >= 0 ? view->indexBelow(current) : view->indexAbove(current);
  }
  while (current.isValid()) {
    if (isInteractiveIndex(current)) {
      return current;
    }
    current = delta >= 0 ? view->indexBelow(current) : view->indexAbove(current);
  }
  return QModelIndex();
}

QModelIndex AdNavigationMenu::Private::firstNavigableIndex(detail::AdMenuTreeView* view) const {
  return nextNavigableIndex(view, QModelIndex(), +1);
}

QModelIndex AdNavigationMenu::Private::lastNavigableIndex(detail::AdMenuTreeView* view) const {
  return nextNavigableIndex(view, QModelIndex(), -1);
}

QModelIndex AdNavigationMenu::Private::barAdjacentIndex(const QModelIndex& start, int delta) const {
  if (!barView || !barView->model()) {
    return QModelIndex();
  }
  int row =
      start.isValid() ? start.row() + delta : (delta >= 0 ? 0 : barView->model()->rowCount() - 1);
  while (row >= 0 && row < barView->model()->rowCount()) {
    const QModelIndex candidate = barView->model()->index(row, 0);
    if (isInteractiveIndex(mapToSourceIndex(candidate))) {
      return candidate;
    }
    row += delta;
  }
  return QModelIndex();
}

QModelIndex AdNavigationMenu::Private::popupParentForView(detail::AdMenuTreeView* view) const {
  for (const auto& level : popupLevels) {
    if (level && level->view == view) {
      return level->submenuIndex;
    }
  }
  return QModelIndex();
}

void AdNavigationMenu::Private::focusPopupForIndex(const QModelIndex& sourceIndex) {
  for (const auto& level : popupLevels) {
    if (!level || !level->view || level->submenuIndex != sourceIndex) {
      continue;
    }
    level->view->setFocus();
    const QModelIndex first = firstNavigableIndex(level->view);
    if (first.isValid()) {
      setCurrentIndexInternal(first, false);
      level->view->scrollTo(first);
    }
    return;
  }
}

void AdNavigationMenu::Private::focusPopupForIndexDeferred(const QModelIndex& sourceIndex) {
  if (!sourceIndex.isValid()) {
    return;
  }
  QPointer<AdNavigationMenu> guard = q;
  const QPersistentModelIndex persistent(sourceIndex);
  QTimer::singleShot(0, q, [this, guard, persistent]() {
    if (!guard || !persistent.isValid()) {
      return;
    }
    focusPopupForIndex(persistent);
  });
}

void AdNavigationMenu::Private::ensureTooltipHost() {
  if (tooltipHost) {
    return;
  }

  auto* tooltip = new AdTooltip(q);
  tooltip->setObjectName(QStringLiteral("ad-menu-inline-collapsed-tooltip"));
  tooltip->setActivationMode(AdTooltip::ActivationMode::Manual);
  tooltip->setArrowPointAtCenter(true);
  tooltip->setHoverOpenDelayMs(0);
  tooltip->setHoverCloseDelayMs(0);
  tooltip->setPlacement(collapsedMenuTooltipPlacement(q));
  tooltip->setTargetWidget(verticalView ? verticalView->viewport() : q);
  tooltip->setAnchorWidget(verticalView ? verticalView->viewport() : q);
  tooltipHost = tooltip;
}

void AdNavigationMenu::Private::hideTooltip() {
  if (!tooltipHost) {
    return;
  }
  tooltipHost->setVisible(false);
  tooltipHost->setText(QString());
  tooltipHost->clearAnchorRect();
}

void AdNavigationMenu::Private::syncCollapsedTooltip(const QModelIndex& sourceIndex) {
  if (!tooltipEnabled) {
    hideTooltip();
    return;
  }
  if (q->popupIsVisible()) {
    hideTooltip();
    return;
  }
  if (!(mode == Mode::Inline && collapsed) || activeViewWidget() != verticalView) {
    hideTooltip();
    return;
  }
  if (!hoveredIndex.isValid() || hoveredIndex != sourceIndex) {
    hideTooltip();
    return;
  }
  if (!sourceIndex.isValid() || sourceIndex.parent().isValid() || !isActionIndex(sourceIndex)) {
    hideTooltip();
    return;
  }
  const QString text = tooltipTextForIndex(sourceIndex);
  if (text.isEmpty()) {
    hideTooltip();
    return;
  }
  ensureTooltipHost();
  if (!tooltipHost || !verticalView || !verticalView->viewport()) {
    return;
  }
  const QRect rect = verticalView->visualRect(sourceIndex);
  if (!rect.isValid()) {
    hideTooltip();
    return;
  }
  QRect anchorRect = rect.intersected(verticalView->viewport()->rect());
  if (anchorRect.isEmpty()) {
    anchorRect = QRect(0, 0, 1, 1);
  }
  tooltipHost->setPlacement(collapsedMenuTooltipPlacement(q));
  tooltipHost->setEnabled(q->isEnabled());
  tooltipHost->setTargetWidget(verticalView->viewport());
  tooltipHost->setAnchorWidget(verticalView->viewport());
  tooltipHost->setAnchorRect(anchorRect);
  tooltipHost->setText(text);
  tooltipHost->setVisible(true);
}

bool AdNavigationMenu::Private::handleTreeKey(detail::AdMenuTreeView* view, QKeyEvent* event) {
  if (!event || !view) {
    return false;
  }
  const QModelIndex current = selectionModel ? selectionModel->currentIndex() : QModelIndex();
  const QModelIndex rootIndex = popupParentForView(view);
  const bool inlineMode = (view == inlineView);
  const bool rtl = view->layoutDirection() == Qt::RightToLeft;
  const int forwardKey = rtl ? Qt::Key_Left : Qt::Key_Right;
  const int backKey = rtl ? Qt::Key_Right : Qt::Key_Left;
  const auto acceptWithKeyboardFocus = [this, event]() {
    setFocusVisible(true);
    event->accept();
    return true;
  };

  auto setCurrentTo = [this, view](const QModelIndex& next) {
    if (!next.isValid()) {
      return false;
    }
    setCurrentIndexInternal(next, false);
    view->scrollTo(next);
    return true;
  };

  switch (event->key()) {
    case Qt::Key_Home: {
      if (setCurrentTo(firstNavigableIndex(view))) {
        return acceptWithKeyboardFocus();
      }
      break;
    }
    case Qt::Key_End: {
      if (setCurrentTo(lastNavigableIndex(view))) {
        return acceptWithKeyboardFocus();
      }
      break;
    }
    case Qt::Key_Down: {
      if (setCurrentTo(nextNavigableIndex(view, current, +1))) {
        return acceptWithKeyboardFocus();
      }
      break;
    }
    case Qt::Key_Up: {
      if (setCurrentTo(nextNavigableIndex(view, current, -1))) {
        return acceptWithKeyboardFocus();
      }
      break;
    }
    case Qt::Key_Right:
    case Qt::Key_Left: {
      if (event->key() == forwardKey) {
        if (current.isValid() && isSubmenuIndex(current)) {
          setExpandedInternal(current, true, true);
          if (!inlineMode) {
            focusPopupForIndex(current);
          }
          return acceptWithKeyboardFocus();
        }
      } else if (event->key() == backKey) {
        if (inlineMode && current.isValid()) {
          QModelIndex parent = current.parent();
          while (parent.isValid() && isGroupIndex(parent)) {
            parent = parent.parent();
          }
          if (parent.isValid() && isSubmenuIndex(parent)) {
            if (containsExpanded(parent)) {
              setExpandedInternal(parent, false, true);
            }
            setCurrentIndexInternal(parent, false);
            view->scrollTo(parent);
            return acceptWithKeyboardFocus();
          }
        } else if (rootIndex.isValid()) {
          setExpandedInternal(rootIndex, false, true);
          setCurrentIndexInternal(rootIndex, false);
          return acceptWithKeyboardFocus();
        }
      }
      break;
    }
    case Qt::Key_Escape: {
      clearPopupExpandedSubmenus();
      syncAllViews();
      syncPopupVisibility();
      return acceptWithKeyboardFocus();
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space: {
      if (current.isValid()) {
        activateSourceIndex(current, true);
        return acceptWithKeyboardFocus();
      }
      break;
    }
    default:
      break;
  }
  return false;
}

bool AdNavigationMenu::Private::handleBarKey(QKeyEvent* event) {
  if (!event || !barView || !barView->model()) {
    return false;
  }
  const QModelIndex currentBar = barView->currentIndex();
  const bool rtl = barView->layoutDirection() == Qt::RightToLeft;
  const auto acceptWithKeyboardFocus = [this, event]() {
    setFocusVisible(true);
    event->accept();
    return true;
  };
  switch (event->key()) {
    case Qt::Key_Home: {
      const QModelIndex next = barAdjacentIndex(QModelIndex(), +1);
      if (next.isValid()) {
        barView->setCurrentIndex(next);
        setCurrentIndexInternal(mapToSourceIndex(next), false);
        return acceptWithKeyboardFocus();
      }
      break;
    }
    case Qt::Key_End: {
      const QModelIndex next = barAdjacentIndex(QModelIndex(), -1);
      if (next.isValid()) {
        barView->setCurrentIndex(next);
        setCurrentIndexInternal(mapToSourceIndex(next), false);
        return acceptWithKeyboardFocus();
      }
      break;
    }
    case Qt::Key_Right: {
      const QModelIndex next = barAdjacentIndex(currentBar, rtl ? -1 : +1);
      if (next.isValid()) {
        barView->setCurrentIndex(next);
        setCurrentIndexInternal(mapToSourceIndex(next), false);
        return acceptWithKeyboardFocus();
      }
      break;
    }
    case Qt::Key_Left: {
      const QModelIndex next = barAdjacentIndex(currentBar, rtl ? +1 : -1);
      if (next.isValid()) {
        barView->setCurrentIndex(next);
        setCurrentIndexInternal(mapToSourceIndex(next), false);
        return acceptWithKeyboardFocus();
      }
      break;
    }
    case Qt::Key_Down:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space: {
      const QModelIndex sourceIndex = mapToSourceIndex(currentBar);
      if (sourceIndex.isValid()) {
        activateSourceIndex(sourceIndex, true);
        if (isSubmenuIndex(sourceIndex)) {
          focusPopupForIndex(sourceIndex);
        }
        return acceptWithKeyboardFocus();
      }
      break;
    }
    case Qt::Key_Escape: {
      clearPopupExpandedSubmenus();
      syncAllViews();
      syncPopupVisibility();
      return acceptWithKeyboardFocus();
    }
    default:
      break;
  }
  return false;
}

void AdNavigationMenu::Private::hidePopupLevelsFrom(int levelIndex) {
  for (int i = levelIndex; i < static_cast<int>(popupLevels.size()); ++i) {
    if (popupLevels[i] && popupLevels[i]->shell) {
      popupLevels[i]->shell->hide();
      popupLevels[i]->submenuIndex = QModelIndex();
    }
  }
}

void AdNavigationMenu::Private::syncPopupVisibility() {
  if (!popupLikeMode()) {
    hidePopupLevelsFrom(0);
    detail::setPopupInteractionHostOpen(q, false);
    return;
  }
  QWidget* scopeWindow = detail::resolvePopupScopeWindow(q);
  const bool canPresentPopupShells = q && q->isVisible() && scopeWindow && scopeWindow->isVisible();
  if (!canPresentPopupShells) {
    hidePopupLevelsFrom(0);
    detail::setPopupInteractionHostOpen(q, false);
    return;
  }
  const QVector<QModelIndex> chain = popupChain();
  if (chain.isEmpty()) {
    hidePopupLevelsFrom(0);
    detail::setPopupInteractionHostOpen(q, false);
    return;
  }
  if (mode == Mode::Inline && collapsed) {
    const auto popupChainAnchoredByInteraction = [this, &chain]() {
      const QModelIndex currentSource =
          selectionModel ? selectionModel->currentIndex() : QModelIndex();
      if (currentSource.isValid() && topLevelAncestor(currentSource) == chain.constFirst()) {
        return true;
      }

      if (hoveredIndex.isValid() && topLevelAncestor(hoveredIndex) == chain.constFirst()) {
        return true;
      }

      auto* currentAnchorView = qobject_cast<QAbstractItemView*>(activeViewWidget());
      if (currentAnchorView && currentAnchorView->viewport()) {
        const QPoint localCursor = currentAnchorView->viewport()->mapFromGlobal(QCursor::pos());
        const QModelIndex viewHoveredIndex = currentAnchorView->indexAt(localCursor);
        const QModelIndex hoveredSource =
            currentAnchorView == barView ? mapToSourceIndex(viewHoveredIndex) : viewHoveredIndex;
        if (hoveredSource.isValid() && topLevelAncestor(hoveredSource) == chain.constFirst()) {
          return true;
        }
      }

      return std::any_of(popupLevels.cbegin(), popupLevels.cend(), [](const auto& level) {
        if (!level || !level->shell || !level->shell->isVisible()) {
          return false;
        }
        const QPoint popupCursor = level->shell->mapFromGlobal(QCursor::pos());
        return level->shell->rect().contains(popupCursor);
      });
    };
    if (!popupChainAnchoredByInteraction()) {
      hidePopupLevelsFrom(0);
      detail::setPopupInteractionHostOpen(q, false);
      return;
    }
  }

  auto* anchorView = qobject_cast<QAbstractItemView*>(activeViewWidget());
  for (int i = 0; i < chain.size(); ++i) {
    ensurePopupLevel(i);
    PopupLevel& level = *popupLevels[i];
    if (level.shell && level.shell->parentWidget() != scopeWindow) {
      level.shell->hide();
      level.shell->setParent(scopeWindow);
    }
    syncPopupLevel(level, chain.at(i));
    QSize popupContent = popupContentSize(chain.at(i), level.popupColorScheme, level.view);
    if (level.renderedRoot && level.renderedRoot != level.view) {
      popupContent = popupContent.expandedTo(level.renderedRoot->sizeHint());
    }

    QAbstractItemView* currentAnchorView =
        (i == 0) ? anchorView : qobject_cast<QAbstractItemView*>(popupLevels[i - 1]->view.data());
    QModelIndex anchorIndex = chain.at(i);
    if (currentAnchorView == barView) {
      anchorIndex = mapFromSourceIndex(barView->model(), anchorIndex);
    }
    if (!currentAnchorView) {
      continue;
    }
    const QRect anchorRect = currentAnchorView->visualRect(anchorIndex);
    if (!anchorRect.isValid()) {
      continue;
    }

    const bool horizontalRootPopup = mode == Mode::Horizontal && i == 0;
    const ColorScheme placementColorScheme =
        horizontalRootPopup ? colorScheme : level.popupColorScheme;
    const MenuVisualStyle placementStyle = q->resolvedVisualStyle(
        horizontalRootPopup ? Mode::Horizontal : Mode::Vertical, placementColorScheme, false);

    QLayout* popupLayout = level.shell ? level.shell->layout() : nullptr;
    const QMargins shadowMargins = detail::antPopupShadowSecondaryMargins();
    if (popupLayout) {
      popupLayout->setContentsMargins(shadowMargins);
    }

    const int horizontalPopupAlignOffset =
        horizontalRootPopup ? std::max(0, placementStyle.metrics.itemPaddingInline) : 0;
    const int horizontalPopupGap =
        horizontalRootPopup ? std::max(0, placementStyle.metrics.popupPlacementGap) : 0;
    const int sidePopupGap =
        horizontalRootPopup ? 0
                            : std::max(0, placementStyle.metrics.popupPlacementGap -
                                              std::max(0, placementStyle.metrics.itemMarginInline));
    const int horizontalStretchWidth =
        horizontalRootPopup
            ? std::max(0, anchorRect.width() - placementStyle.metrics.itemPaddingInline * 2)
            : 0;

    const auto measurePopupShell = [&]() {
      QSize measured = popupContent;
      if (horizontalRootPopup) {
        measured.setWidth(std::max(measured.width(), horizontalStretchWidth));
      }
      if (popupLayout) {
        const QMargins margins = popupLayout->contentsMargins();
        measured.rwidth() += margins.left() + margins.right();
        measured.rheight() += margins.top() + margins.bottom();
      }
      if (level.shell) {
        level.shell->resize(measured);
      }
      return measured;
    };

    QSize popupFrameSize = measurePopupShell();
    QSize popupSize = detail::removeAntPopupShadowMargins(popupFrameSize);

    detail::PopupPlacementInput placementInput;
    placementInput.anchorTopLeft =
        currentAnchorView->viewport()->mapTo(scopeWindow, anchorRect.topLeft());
    if (horizontalRootPopup) {
      placementInput.anchorTopLeft.rx() += horizontalPopupAlignOffset;
    }
    placementInput.anchorSize = anchorRect.size();
    placementInput.popupSize = popupSize;
    placementInput.bounds = scopeWindow ? scopeWindow->rect() : QRect(QPoint(0, 0), popupSize);
    placementInput.preferredPlacement =
        horizontalRootPopup ? detail::PopupPlacement::BottomLeft : detail::PopupPlacement::RightTop;
    const QPoint baseOffset =
        q->popupOffset() + chain.at(i).data(AdNavigationMenu::PopupOffsetRole).toPoint();
    placementInput.offset = baseOffset;

    detail::PopupPlacementOutput placementOutput = detail::resolvePopupPlacement(placementInput);
    if (popupLayout) {
      if (!horizontalRootPopup && sidePopupGap > 0) {
        const bool useLeftPlacement = placementOutput.placement == detail::PopupPlacement::LeftTop;
        const int leftGap = useLeftPlacement ? 0 : sidePopupGap;
        const int rightGap = useLeftPlacement ? sidePopupGap : 0;
        popupLayout->setContentsMargins(shadowMargins.left() + leftGap, shadowMargins.top(),
                                        shadowMargins.right() + rightGap, shadowMargins.bottom());
        popupFrameSize = measurePopupShell();
        popupSize = detail::removeAntPopupShadowMargins(popupFrameSize);
        placementInput.popupSize = popupSize;
        placementOutput = detail::resolvePopupPlacement(placementInput);
      } else {
        popupLayout->setContentsMargins(shadowMargins);
        popupFrameSize = measurePopupShell();
        popupSize = detail::removeAntPopupShadowMargins(popupFrameSize);
        placementInput.popupSize = popupSize;
        placementOutput = detail::resolvePopupPlacement(placementInput);
      }
    }
    if (horizontalRootPopup && horizontalPopupGap > 0) {
      switch (placementOutput.placement) {
        case detail::PopupPlacement::BottomLeft:
        case detail::PopupPlacement::BottomRight:
        case detail::PopupPlacement::BottomCenter:
          placementOutput.topLeft.ry() += horizontalPopupGap;
          break;
        case detail::PopupPlacement::TopLeft:
        case detail::PopupPlacement::TopRight:
        case detail::PopupPlacement::TopCenter:
          placementOutput.topLeft.ry() -= horizontalPopupGap;
          break;
        case detail::PopupPlacement::RightTop:
        case detail::PopupPlacement::LeftTop:
          break;
      }
      placementOutput.topLeft =
          detail::clampPopupTopLeft(placementOutput.topLeft, popupSize, placementInput.bounds);
    }

    if (level.shell) {
      level.shell->move(
          detail::antPopupShadowFrameTopLeftForVisualTopLeft(placementOutput.topLeft));
      level.shell->show();
      level.shell->raise();
    }
  }
  hidePopupLevelsFrom(static_cast<int>(chain.size()));
  detail::setPopupInteractionHostOpen(q, true);
}

int measureMenuItemWidth(const QModelIndex& sourceIndex, const MenuVisualStyle& style,
                         AdNavigationMenu::Mode mode, bool collapsed, int indentStep,
                         int relativeDepth, bool popupLevel) {
  if (!sourceIndex.isValid()) {
    return 0;
  }
  const QFontMetricsF fm(style.metrics.font);
  if (isSeparatorIndex(sourceIndex)) {
    return style.metrics.itemPaddingInline * 2 + 40;
  }
  if (isGroupIndex(sourceIndex)) {
    QFont groupFont = style.metrics.font;
    groupFont.setBold(false);
    groupFont.setPixelSize(std::max(10, style.metrics.groupTitleFontSize));
    const QFontMetricsF groupFm(groupFont);
    return style.metrics.groupTitleHorizontalPadding * 2 +
           static_cast<int>(std::ceil(groupFm.horizontalAdvance(displayTextForIndex(sourceIndex))));
  }
  if (mode == AdNavigationMenu::Mode::Inline && collapsed && !popupLevel && relativeDepth == 0) {
    return collapsedInlineRootWidth(style);
  }
  const bool horizontal = mode == AdNavigationMenu::Mode::Horizontal && !popupLevel;
  int width = style.metrics.itemPaddingInline * 2;
  if (!horizontal) {
    width += style.metrics.itemMarginInline * 2;
  }
  if (mode == AdNavigationMenu::Mode::Inline && !collapsed) {
    width += std::max(0, relativeDepth * indentStep);
  } else if (!horizontal) {
    width += std::max(
        0, relativeDepth * (style.metrics.itemPaddingInline - style.metrics.itemMarginInline));
  }
  const adqt::icons::IconRef icon = iconRefForIndex(sourceIndex);
  if (adqt::icons::isValid(icon)) {
    width += std::max(10, style.metrics.iconSize) + style.metrics.iconMarginInlineEnd + 1;
  }
  QString label = displayTextForIndex(sourceIndex);
  if (mode == AdNavigationMenu::Mode::Inline && collapsed) {
    if (adqt::icons::isValid(icon)) {
      label.clear();
    } else if (!label.isEmpty()) {
      label = label.left(1);
    }
  }
  width += static_cast<int>(std::ceil(fm.horizontalAdvance(label)));
  const QString extra = sourceIndex.data(AdNavigationMenu::ExtraTextRole).toString();
  if (!extra.isEmpty() && !horizontal && !(mode == AdNavigationMenu::Mode::Inline && collapsed)) {
    width += static_cast<int>(std::ceil(fm.horizontalAdvance(extra))) + 12;
  }
  if (shouldShowSubMenuArrow(mode, collapsed, sourceIndex) ||
      (popupLevel && isSubmenuIndex(sourceIndex))) {
    width += kSubMenuArrowBoxWidth + kSubMenuArrowTextGap;
  }
  return width;
}

int menuItemBaseHeight(const QModelIndex& sourceIndex, const MenuVisualStyle& style,
                       AdNavigationMenu::Mode mode, bool popupLevel) {
  if (isSeparatorIndex(sourceIndex)) {
    return std::max(4, style.metrics.borderWidth + style.metrics.dividerMarginBlock * 2 + 2);
  }
  if (isGroupIndex(sourceIndex)) {
    return std::max(style.metrics.groupTitleFontSize, style.metrics.groupTitleLineHeight) +
           style.metrics.groupTitleVerticalPadding * 2;
  }
  if (mode == AdNavigationMenu::Mode::Horizontal && !popupLevel) {
    return std::max(1, style.metrics.horizontalLineHeight);
  }
  return style.metrics.itemHeight;
}

int menuItemLeadingSpacing(const QWidget* widget, const QModelIndex& index,
                           const QModelIndex& sourceIndex, const MenuVisualStyle& style,
                           AdNavigationMenu::Mode mode, bool collapsed, bool popupLevel) {
  if (!widget || mode == AdNavigationMenu::Mode::Horizontal) {
    return 0;
  }
  QModelIndex previousSourceIndex;
  if (const auto* treeView = qobject_cast<const QTreeView*>(widget)) {
    const QModelIndex previousIndex = treeView->indexAbove(index);
    if (previousIndex.isValid()) {
      previousSourceIndex = mapToSourceIndex(previousIndex);
    }
  }
  return menuRowLeadingSpacing(style, sourceIndex, previousSourceIndex) +
         menuRootLeadingSpacing(style, mode, popupLevel, sourceIndex, previousSourceIndex) +
         inlineSubMenuLeadingSpacing(style, mode, collapsed, popupLevel, sourceIndex,
                                     previousSourceIndex);
}

int menuItemTrailingSpacing(const QWidget* widget, const QModelIndex& index,
                            const QModelIndex& sourceIndex, const MenuVisualStyle& style,
                            AdNavigationMenu::Mode mode, bool collapsed, bool popupLevel) {
  if (!widget || mode == AdNavigationMenu::Mode::Horizontal) {
    return 0;
  }
  QModelIndex nextSourceIndex;
  if (const auto* treeView = qobject_cast<const QTreeView*>(widget)) {
    const QModelIndex nextIndex = treeView->indexBelow(index);
    if (nextIndex.isValid()) {
      nextSourceIndex = mapToSourceIndex(nextIndex);
    }
  }
  return inlineSubMenuTrailingSpacing(style, mode, collapsed, popupLevel, sourceIndex,
                                      nextSourceIndex);
}

QSize AdNavigationMenuItemDelegate::sizeHint(const QStyleOptionViewItem& option,
                                             const QModelIndex& index) const {
  if (!owner_) {
    return QStyledItemDelegate::sizeHint(option, index);
  }
  const QModelIndex sourceIndex = mapToSourceIndex(index);
  const QWidget* widget = option.widget;
  const AdNavigationMenu::Mode mode =
      widget
          ? static_cast<AdNavigationMenu::Mode>(widget->property("AdNavigationMenu.mode").toInt())
          : owner_->mode();
  const bool collapsed =
      widget ? widget->property("AdNavigationMenu.collapsed").toBool() : owner_->collapsed();
  const bool popupLevel = widget ? widget->property("AdNavigationMenu.popupLevel").toBool() : false;
  const int rootDepth = widget ? widget->property("AdNavigationMenu.rootDepth").toInt() : 0;
  const AdNavigationMenu::ColorScheme colorScheme =
      widget ? static_cast<AdNavigationMenu::ColorScheme>(
                   widget->property("AdNavigationMenu.popupColorScheme").toInt())
             : owner_->colorScheme();
  const MenuVisualStyle style = owner_->resolvedVisualStyle(
      popupLevel ? AdNavigationMenu::Mode::Vertical : mode, colorScheme, collapsed);
  const int relativeDepth = std::max(0, sourceDepth(sourceIndex) - rootDepth);
  const int height =
      menuItemBaseHeight(sourceIndex, style, mode, popupLevel) +
      menuItemLeadingSpacing(widget, index, sourceIndex, style, mode, collapsed, popupLevel) +
      menuItemTrailingSpacing(widget, index, sourceIndex, style, mode, collapsed, popupLevel);
  return QSize(measureMenuItemWidth(sourceIndex, style, mode, collapsed,
                                    owner_->effectiveIndentation(style), relativeDepth, popupLevel),
               height);
}

void AdNavigationMenuItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                         const QModelIndex& index) const {
  if (!owner_ || !painter) {
    return;
  }
  const QModelIndex sourceIndex = mapToSourceIndex(index);
  if (!sourceIndex.isValid()) {
    return;
  }
  const QWidget* widget = option.widget;
  const AdNavigationMenu::Mode mode =
      widget
          ? static_cast<AdNavigationMenu::Mode>(widget->property("AdNavigationMenu.mode").toInt())
          : owner_->mode();
  const bool collapsed =
      widget ? widget->property("AdNavigationMenu.collapsed").toBool() : owner_->collapsed();
  const bool popupLevel = widget ? widget->property("AdNavigationMenu.popupLevel").toBool() : false;
  const int rootDepth = widget ? widget->property("AdNavigationMenu.rootDepth").toInt() : 0;
  const AdNavigationMenu::ColorScheme colorScheme =
      widget ? static_cast<AdNavigationMenu::ColorScheme>(
                   widget->property("AdNavigationMenu.popupColorScheme").toInt())
             : owner_->colorScheme();
  const MenuVisualStyle style = owner_->resolvedVisualStyle(
      popupLevel ? AdNavigationMenu::Mode::Vertical : mode, colorScheme, collapsed);
  const Qt::LayoutDirection direction = option.direction;
  const QRect rowRect = option.rect;
  const bool horizontal = mode == AdNavigationMenu::Mode::Horizontal && !popupLevel;
  const bool disabled = !(sourceIndex.flags() & Qt::ItemIsEnabled);
  const bool danger = sourceIndex.data(AdNavigationMenu::DangerRole).toBool();
  const bool hovered = option.state & QStyle::State_MouseOver;
  const bool pressed = option.state & QStyle::State_Sunken;
  const bool active = owner_->d_->shouldPaintCurrentAsActive(sourceIndex);
  const int relativeDepth = std::max(0, sourceDepth(sourceIndex) - rootDepth);
  const int topSpacing =
      menuItemLeadingSpacing(widget, index, sourceIndex, style, mode, collapsed, popupLevel);
  const int bottomSpacing =
      menuItemTrailingSpacing(widget, index, sourceIndex, style, mode, collapsed, popupLevel);
  const QRect contentRowRect = rowRect.adjusted(0, topSpacing, 0, -bottomSpacing);
  const bool inlineSubMenuChild = !popupLevel && mode == AdNavigationMenu::Mode::Inline &&
                                  !collapsed && sourceIndex.parent().isValid();

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, false);
  painter->setFont(style.metrics.font);

  if (inlineSubMenuChild && style.subMenuBackground.alpha() > 0) {
    QModelIndex previousSourceIndex;
    if (const auto* treeView = qobject_cast<const QTreeView*>(widget)) {
      const QModelIndex previousIndex = treeView->indexAbove(index);
      if (previousIndex.isValid()) {
        previousSourceIndex = mapToSourceIndex(previousIndex);
      }
    }
    const int surfaceWidth = widget ? widget->width() : rowRect.width();
    const int listTop =
        rowRect.top() + inlineSubMenuLeadingSpacing(style, mode, collapsed, popupLevel, sourceIndex,
                                                    previousSourceIndex);
    const QRect surfaceRect(0, listTop, surfaceWidth, rowRect.bottom() - listTop + 1);
    painter->fillRect(surfaceRect.intersected(painter->viewport()), style.subMenuBackground);
  }

  if (isSeparatorIndex(sourceIndex)) {
    const int midY = contentRowRect.center().y();
    QPen pen(style.dividerColor, std::max(1, style.metrics.borderWidth));
    if (dashedSeparatorForIndex(sourceIndex)) {
      pen.setStyle(Qt::DashLine);
    }
    painter->setPen(pen);
    painter->drawLine(contentRowRect.left() + style.metrics.itemPaddingInline, midY,
                      contentRowRect.right() - style.metrics.itemPaddingInline, midY);
    painter->restore();
    return;
  }

  if (isGroupIndex(sourceIndex)) {
    QFont groupFont = style.metrics.font;
    groupFont.setBold(false);
    groupFont.setPixelSize(std::max(10, style.metrics.groupTitleFontSize));
    painter->setFont(groupFont);
    painter->setPen(style.groupTitleColor);
    painter->drawText(contentRowRect.adjusted(style.metrics.groupTitleHorizontalPadding,
                                              style.metrics.groupTitleVerticalPadding,
                                              -style.metrics.groupTitleHorizontalPadding,
                                              -style.metrics.groupTitleVerticalPadding),
                      QStyle::visualAlignment(direction, Qt::AlignVCenter | Qt::AlignLeft),
                      displayTextForIndex(sourceIndex));
    painter->restore();
    return;
  }

  const bool itemSelected =
      option.state.testFlag(QStyle::State_Selected) && isActionIndex(sourceIndex);
  const bool submenuSelected = owner_->d_->hasSelectedDescendantForStyle(sourceIndex);
  const bool selected = itemSelected || submenuSelected;
  const bool opened = owner_->d_->containsExpanded(sourceIndex);

  detail::MenuStateStyle state = style.normal;
  if (disabled) {
    state = style.disabled;
  } else if (horizontal) {
    state = style.horizontalNormal;
    if (isSubmenuIndex(sourceIndex) && submenuSelected) {
      state = style.horizontalSelected;
      if (style.subMenuItemSelectedColor.isValid()) {
        state.text = style.subMenuItemSelectedColor;
      } else if (style.selected.text.isValid()) {
        state.text = style.selected.text;
      }
    } else if (selected) {
      state = style.horizontalSelected;
    } else if (danger) {
      if (pressed || active) {
        state = style.dangerActive;
      } else if (hovered) {
        state = style.dangerHover;
      } else {
        state = style.danger;
      }
    } else if (pressed || active) {
      state = style.horizontalActive;
    } else if (hovered) {
      state = style.horizontalHover;
    }
  } else if (isSubmenuIndex(sourceIndex) && submenuSelected) {
    if (danger) {
      if (pressed || active) {
        state = style.dangerActive;
      } else if (hovered) {
        state = style.dangerHover;
      } else {
        state = style.danger;
      }
      if (style.dangerSelected.text.isValid()) {
        state.text = style.dangerSelected.text;
      }
    } else {
      if (pressed || active) {
        state = style.active;
      } else if (hovered) {
        state = style.hover;
      } else {
        state = style.normal;
        state.background = QColor(Qt::transparent);
      }
      if (style.subMenuItemSelectedColor.isValid()) {
        state.text = style.subMenuItemSelectedColor;
      } else if (style.selected.text.isValid()) {
        state.text = style.selected.text;
      }
    }
  } else if (danger) {
    if (itemSelected) {
      state = style.dangerSelected;
    } else if (pressed || active) {
      state = style.dangerActive;
    } else if (hovered) {
      state = style.dangerHover;
    } else {
      state = style.danger;
    }
  } else if (itemSelected) {
    state = style.selected;
  } else if (pressed || active) {
    state = style.active;
  } else if (hovered) {
    state = style.hover;
  }

  if (inlineSubMenuChild && !itemSelected && !pressed && !active && !hovered) {
    state.background = QColor(Qt::transparent);
  }

  QRect fillRect = contentRowRect.adjusted(style.metrics.itemMarginInline, 0,
                                           -style.metrics.itemMarginInline, 0);
  qreal radius =
      horizontal
          ? style.metrics.horizontalItemBorderRadius
          : (popupLevel ? style.metrics.subMenuItemBorderRadius
                        : (isSubmenuIndex(sourceIndex) ? style.metrics.subMenuItemBorderRadius
                                                       : style.metrics.itemBorderRadius));
  if (horizontal) {
    fillRect = rowRect;
    if (!disabled && !selected && (hovered || pressed || opened)) {
      state.background = style.horizontalHover.background;
    }
  }
  const QColor textColor = state.text;
  const QColor backgroundColor = state.background;
  if (backgroundColor.alpha() > 0) {
    painter->setPen(Qt::NoPen);
    painter->setBrush(backgroundColor);
    painter->drawRoundedRect(fillRect, radius, radius);
  }

  if (!horizontal && style.metrics.activeBarWidth > 0 && itemSelected && !collapsed) {
    painter->setPen(Qt::NoPen);
    painter->setBrush(textColor);
    const bool rtl = direction == Qt::RightToLeft;
    const int activeBarLeft =
        rtl ? fillRect.left() : fillRect.right() - style.metrics.activeBarWidth + 1;
    painter->drawRoundedRect(
        QRect(activeBarLeft, fillRect.top(), style.metrics.activeBarWidth, fillRect.height()),
        style.metrics.activeBarWidth / 2.0, style.metrics.activeBarWidth / 2.0);
  }

  int indent = 0;
  if (mode == AdNavigationMenu::Mode::Inline && !collapsed) {
    indent = relativeDepth * std::max(0, owner_->effectiveIndentation(style));
  } else if (!horizontal) {
    indent = relativeDepth *
             std::max(0, style.metrics.itemPaddingInline - style.metrics.itemMarginInline);
  }
  QRect logicalContentRect = fillRect.adjusted(style.metrics.itemPaddingInline + indent, 0,
                                               -style.metrics.itemPaddingInline, 0);
  if (horizontal) {
    logicalContentRect =
        fillRect.adjusted(style.metrics.itemPaddingInline, 0, -style.metrics.itemPaddingInline, 0);
  }
  const bool collapsedInlineRoot = !horizontal && mode == AdNavigationMenu::Mode::Inline &&
                                   collapsed && !popupLevel && relativeDepth == 0;
  if (collapsedInlineRoot) {
    logicalContentRect = fillRect;
  }
  const QRect contentRect = QStyle::visualRect(direction, fillRect, logicalContentRect);

  const int iconSide = collapsedInlineRoot ? std::max(10, style.metrics.collapsedIconSize)
                                           : std::max(10, style.metrics.iconSize);
  const adqt::icons::IconRef icon = iconRefForIndex(sourceIndex);
  const bool hasIcon = adqt::icons::isValid(icon);
  QRect logicalIconRect(logicalContentRect.left(), logicalContentRect.center().y() - iconSide / 2,
                        iconSide, iconSide);
  if (collapsedInlineRoot) {
    logicalIconRect.moveLeft(logicalContentRect.center().x() - iconSide / 2);
  }
  const QRect iconRect = QStyle::visualRect(direction, fillRect, logicalIconRect);

  int textLeft = logicalContentRect.left();
  if (hasIcon) {
    paintMenuIcon(*painter, icon, iconRect, textColor, disabled);
    textLeft = logicalIconRect.right() + 1 + style.metrics.iconMarginInlineEnd;
  }

  const bool showArrow = shouldShowSubMenuArrow(mode, collapsed, sourceIndex) ||
                         (popupLevel && isSubmenuIndex(sourceIndex));
  QRect logicalArrowRect(logicalContentRect.right() - kSubMenuArrowBoxWidth,
                         logicalContentRect.top(), kSubMenuArrowBoxWidth, kSubMenuArrowBoxHeight);
  logicalArrowRect.moveCenter(
      QPoint(logicalArrowRect.center().x(), logicalContentRect.center().y()));
  const QRect arrowRect = QStyle::visualRect(direction, fillRect, logicalArrowRect);
  int textRight =
      showArrow ? logicalArrowRect.left() - kSubMenuArrowTextGap : logicalContentRect.right();

  const QString extra = sourceIndex.data(AdNavigationMenu::ExtraTextRole).toString();
  if (!extra.isEmpty() && !horizontal && !(mode == AdNavigationMenu::Mode::Inline && collapsed)) {
    painter->setPen(textColor);
    const int extraWidth = painter->fontMetrics().horizontalAdvance(extra) + 8;
    QRect logicalExtraRect(std::max(textLeft, textRight - extraWidth), logicalContentRect.top(),
                           extraWidth, logicalContentRect.height());
    const QRect extraRect = QStyle::visualRect(direction, fillRect, logicalExtraRect);
    painter->drawText(extraRect,
                      QStyle::visualAlignment(direction, Qt::AlignVCenter | Qt::AlignRight), extra);
    textRight = logicalExtraRect.left() - 4;
  }

  QString label = displayTextForIndex(sourceIndex);
  if (mode == AdNavigationMenu::Mode::Inline && collapsed) {
    if (hasIcon) {
      label.clear();
    } else if (!label.isEmpty()) {
      label = label.left(1);
    }
  }
  QFont textFont = style.metrics.font;
  if (collapsedInlineRoot && !hasIcon) {
    textFont.setPixelSize(std::max(textFont.pixelSize(), style.metrics.collapsedIconSize));
  }
  const QRect logicalTextRect(textLeft, logicalContentRect.top(), std::max(0, textRight - textLeft),
                              logicalContentRect.height());
  const QRect textRect = QStyle::visualRect(direction, fillRect, logicalTextRect);
  const int textFlags = collapsedInlineRoot && !hasIcon
                            ? Qt::AlignCenter
                            : QStyle::visualAlignment(direction, Qt::AlignVCenter | Qt::AlignLeft);
  const QFontMetrics textMetrics(textFont);
  const int paintedTextWidth = label.isEmpty() ? 0
                                               : std::max(textMetrics.horizontalAdvance(label),
                                                          textMetrics.boundingRect(label).width());

  if (horizontal && style.metrics.activeBarHeight > 0 &&
      (selected || hovered || pressed || opened) && !disabled) {
    const QRect indicatorRect =
        horizontalActiveBarRect(rowRect, contentRect, iconRect, hasIcon, textRect, paintedTextWidth,
                                style.metrics.activeBarHeight);
    if (indicatorRect.isValid() && !indicatorRect.isEmpty()) {
      painter->setPen(Qt::NoPen);
      painter->setBrush(style.horizontalSelected.text.isValid() ? style.horizontalSelected.text
                                                                : textColor);
      painter->drawRect(indicatorRect);
    }
  }

  painter->setFont(textFont);
  painter->setPen(textColor);
  painter->drawText(textRect, textFlags, label);

  if (showArrow) {
    if (adqt::icons::isValid(owner_->expandIcon())) {
      paintMenuIcon(*painter, owner_->expandIcon(), arrowRect, textColor, disabled);
    } else {
      painter->save();
      painter->setRenderHint(QPainter::Antialiasing, true);
      painter->setPen(QPen(textColor, 1.5));
      const int midX = arrowRect.center().x();
      const int midY = arrowRect.center().y();
      QPainterPath path;
      if (mode == AdNavigationMenu::Mode::Inline && !collapsed) {
        if (owner_->isExpanded(sourceIndex)) {
          path.moveTo(midX - 3, midY + 1);
          path.lineTo(midX, midY - 2);
          path.lineTo(midX + 3, midY + 1);
        } else {
          path.moveTo(midX - 3, midY - 1);
          path.lineTo(midX, midY + 2);
          path.lineTo(midX + 3, midY - 1);
        }
      } else {
        if (direction == Qt::RightToLeft) {
          path.moveTo(midX + 2, midY - 4);
          path.lineTo(midX - 2, midY);
          path.lineTo(midX + 2, midY + 4);
        } else {
          path.moveTo(midX - 2, midY - 4);
          path.lineTo(midX + 2, midY);
          path.lineTo(midX - 2, midY + 4);
        }
      }
      painter->drawPath(path);
      painter->restore();
    }
  }

  painter->restore();
}

AdNavigationMenu::AdNavigationMenu(QWidget* parent)
    : QWidget(parent), d_(std::make_unique<Private>(this)) {
  setFocusPolicy(Qt::StrongFocus);
  d_->rebuildRootViews();
}

AdNavigationMenu::~AdNavigationMenu() {
  d_->prepareForDestruction();
  detail::setPopupInteractionHostOpen(this, false);
}

AdNavigationMenu::Mode AdNavigationMenu::mode() const { return d_->mode; }

void AdNavigationMenu::setMode(Mode value) {
  if (d_->mode == value) {
    return;
  }
  const Mode previousMode = d_->mode;
  const bool previousCollapsed = d_->collapsed;
  d_->mode = value;
  d_->syncModeTransitionState(previousMode, previousCollapsed);
  d_->syncAllViews();
  d_->syncPopupVisibility();
  emit modeChanged(d_->mode);
  updateGeometry();
}

AdNavigationMenu::ColorScheme AdNavigationMenu::colorScheme() const { return d_->colorScheme; }

void AdNavigationMenu::setColorScheme(ColorScheme value) {
  if (d_->colorScheme == value) {
    return;
  }
  d_->colorScheme = value;
  d_->rebuildModeView();
  d_->syncPopupVisibility();
  emit colorSchemeChanged(d_->colorScheme);
  update();
}

bool AdNavigationMenu::collapsed() const { return d_->collapsed; }

void AdNavigationMenu::setCollapsed(bool value) {
  if (d_->collapsed == value) {
    return;
  }
  const Mode previousMode = d_->mode;
  const bool previousCollapsed = d_->collapsed;
  d_->collapsed = value;
  d_->syncModeTransitionState(previousMode, previousCollapsed);
  d_->syncAllViews();
  d_->syncPopupVisibility();
  emit collapsedChanged(d_->collapsed);
  updateGeometry();
}

int AdNavigationMenu::indentation() const {
  const auto style = resolvedVisualStyle(d_->mode, d_->colorScheme, d_->collapsed);
  return effectiveIndentation(style);
}

void AdNavigationMenu::setIndentation(int value) {
  value = std::max(0, value);
  if (d_->indentationOverride == value) {
    return;
  }
  d_->indentationOverride = value;
  d_->syncAllViews();
  emit indentationChanged(d_->indentationOverride);
  emit indentChanged(d_->indentationOverride);
  updateGeometry();
}

int AdNavigationMenu::indent() const { return indentation(); }

void AdNavigationMenu::setIndent(int value) { setIndentation(value); }

AdNavigationMenu::SelectionMode AdNavigationMenu::selectionMode() const {
  return d_->selectionMode;
}

void AdNavigationMenu::setSelectionMode(SelectionMode value) {
  if (d_->selectionMode == value) {
    return;
  }
  const QModelIndex previousCurrent = currentIndex();
  const QString currentId = stableIdForIndex(previousCurrent);
  d_->selectionMode = value;
  emit selectionModeChanged(d_->selectionMode);
  d_->applySelectionStateFromStableIds(d_->selectedStableIdsForState());
  if (d_->selectionModel && !currentId.isEmpty()) {
    const QModelIndex mappedCurrent = findIndexByStableId(d_->model, currentId);
    if (mappedCurrent.isValid()) {
      QSignalBlocker blocker(d_->selectionModel);
      d_->selectionModel->setCurrentIndex(mappedCurrent, QItemSelectionModel::NoUpdate);
    }
  }
  d_->syncViewSelectionModels();
  d_->syncBarSelectionState();
  d_->updateAllViewports();
  const QModelIndex current = currentIndex();
  if (current != previousCurrent) {
    emit currentIndexChanged(current);
  }
}

AdNavigationMenu::TriggerSubMenuAction AdNavigationMenu::submenuTrigger() const {
  return d_->submenuTrigger;
}

void AdNavigationMenu::setSubmenuTrigger(TriggerSubMenuAction value) {
  if (d_->submenuTrigger == value) {
    return;
  }
  d_->submenuTrigger = value;
  emit submenuTriggerChanged(d_->submenuTrigger);
}

int AdNavigationMenu::submenuOpenDelayMs() const { return d_->submenuOpenDelayMs; }

void AdNavigationMenu::setSubmenuOpenDelayMs(int value) {
  value = value < 0 ? -1 : value;
  if (d_->submenuOpenDelayMs == value) {
    return;
  }
  d_->submenuOpenDelayMs = value;
  emit submenuOpenDelayMsChanged(d_->submenuOpenDelayMs);
  emit subMenuOpenDelayMsChanged(d_->submenuOpenDelayMs);
}

int AdNavigationMenu::subMenuOpenDelayMs() const { return submenuOpenDelayMs(); }

void AdNavigationMenu::setSubMenuOpenDelayMs(int value) { setSubmenuOpenDelayMs(value); }

int AdNavigationMenu::submenuCloseDelayMs() const { return d_->submenuCloseDelayMs; }

void AdNavigationMenu::setSubmenuCloseDelayMs(int value) {
  value = value < 0 ? -1 : value;
  if (d_->submenuCloseDelayMs == value) {
    return;
  }
  d_->submenuCloseDelayMs = value;
  emit submenuCloseDelayMsChanged(d_->submenuCloseDelayMs);
  emit subMenuCloseDelayMsChanged(d_->submenuCloseDelayMs);
}

int AdNavigationMenu::subMenuCloseDelayMs() const { return submenuCloseDelayMs(); }

void AdNavigationMenu::setSubMenuCloseDelayMs(int value) { setSubmenuCloseDelayMs(value); }

bool AdNavigationMenu::tooltipEnabled() const { return d_->tooltipEnabled; }

void AdNavigationMenu::setTooltipEnabled(bool value) {
  if (d_->tooltipEnabled == value) {
    return;
  }
  d_->tooltipEnabled = value;
  if (!d_->tooltipEnabled) {
    d_->hideTooltip();
  } else {
    d_->syncCollapsedTooltip(currentIndex());
  }
  emit tooltipEnabledChanged(d_->tooltipEnabled);
}

AdNavigationMenu::ComponentTokens AdNavigationMenu::componentTokens() const {
  return d_->componentTokens;
}

void AdNavigationMenu::setComponentTokens(const ComponentTokens& tokens) {
  const int previousIndentation = indentation();
  d_->componentTokens = tokens;
  d_->syncAllViews();
  d_->syncPopupVisibility();
  if (previousIndentation != indentation()) {
    emit indentationChanged(indentation());
    emit indentChanged(indentation());
  }
  emit componentTokensChanged();
  updateGeometry();
}

void AdNavigationMenu::resetComponentTokens() {
  const int previousIndentation = indentation();
  d_->componentTokens = {};
  d_->syncAllViews();
  d_->syncPopupVisibility();
  if (previousIndentation != indentation()) {
    emit indentationChanged(indentation());
    emit indentChanged(indentation());
  }
  emit componentTokensChanged();
  updateGeometry();
}

AdNavigationMenu::SemanticStyles AdNavigationMenu::semanticStyles() const {
  return d_->semanticStyles;
}

void AdNavigationMenu::setSemanticStyles(const SemanticStyles& styles) {
  d_->semanticStyles = styles;
  d_->syncAllViews();
  d_->syncPopupVisibility();
  emit semanticStylesChanged();
  update();
}

void AdNavigationMenu::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  d_->semanticStyleResolver = std::move(resolver);
  d_->syncAllViews();
  d_->syncPopupVisibility();
  emit semanticStylesChanged();
  update();
}

adqt::icons::IconRef AdNavigationMenu::expandIcon() const { return d_->expandIcon; }

void AdNavigationMenu::setExpandIcon(const adqt::icons::IconRef& icon) {
  d_->expandIcon = icon;
  d_->updateAllViewports();
  emit expandIconChanged(d_->expandIcon);
}

QPoint AdNavigationMenu::popupOffset() const { return d_->popupOffset; }

void AdNavigationMenu::setPopupOffset(const QPoint& value) {
  if (d_->popupOffset == value) {
    return;
  }
  d_->popupOffset = value;
  emit popupOffsetChanged(d_->popupOffset);
  d_->syncPopupVisibility();
}

QAbstractItemDelegate* AdNavigationMenu::itemDelegate() const { return d_->effectiveDelegate(); }

void AdNavigationMenu::setItemDelegate(QAbstractItemDelegate* delegate) {
  if (delegate == d_->delegate) {
    delegate = nullptr;
  }
  if (d_->customDelegate == delegate) {
    return;
  }
  d_->customDelegate = delegate;
  if (delegate && !delegate->parent()) {
    delegate->setParent(this);
  }
  if (delegate) {
    QObject::connect(delegate, &QObject::destroyed, this, [this, delegate]() {
      if (d_->customDelegate != delegate) {
        return;
      }
      d_->customDelegate = nullptr;
      d_->syncDelegates();
      d_->syncAllViews();
      d_->syncPopupVisibility();
      emit itemDelegateChanged(itemDelegate());
      updateGeometry();
    });
  }
  d_->syncDelegates();
  d_->syncAllViews();
  d_->syncPopupVisibility();
  emit itemDelegateChanged(itemDelegate());
  updateGeometry();
}

AdNavigationMenuPopupFactory* AdNavigationMenu::popupFactory() const { return d_->popupFactory; }

void AdNavigationMenu::setPopupFactory(AdNavigationMenuPopupFactory* factory) {
  if (d_->popupFactory == factory) {
    return;
  }
  d_->popupFactory = factory;
  if (factory && !factory->parent()) {
    factory->setParent(this);
  }
  if (factory) {
    QObject::connect(factory, &QObject::destroyed, this, [this, factory]() {
      if (d_->popupFactory != factory) {
        return;
      }
      d_->popupFactory = nullptr;
      d_->syncPopupVisibility();
      emit popupFactoryChanged(nullptr);
    });
  }
  d_->syncPopupVisibility();
  emit popupFactoryChanged(d_->popupFactory);
}

QAbstractItemModel* AdNavigationMenu::model() const { return d_->model; }

void AdNavigationMenu::setModel(QAbstractItemModel* model) {
  if (d_->model == model) {
    return;
  }

  const QModelIndex previousCurrent = currentIndex();
  const QStringList selectedIds = d_->selectedStableIdsForState();
  const QString currentId = stableIdForIndex(previousCurrent);

  d_->model = model;
  d_->syncModelConnections();
  d_->rebuildStableIdIndexCache();

  if (!d_->selectionModel || d_->selectionModel->model() != model) {
    if (model) {
      d_->adoptSelectionModel(new QItemSelectionModel(model, this), true);
    } else {
      d_->adoptSelectionModel(nullptr, false);
    }
  }

  d_->normalizeExpandedStableIds(d_->expandedStableIds);
  d_->normalizeExpandedStableIds(d_->collapsedPopupStableIds);

  if (d_->selectionModel) {
    d_->applySelectionStateFromStableIds(selectedIds);
    if (!currentId.isEmpty()) {
      const QModelIndex mappedCurrent = findIndexByStableId(model, currentId);
      if (mappedCurrent.isValid()) {
        QSignalBlocker blocker(d_->selectionModel);
        d_->selectionModel->setCurrentIndex(mappedCurrent, QItemSelectionModel::NoUpdate);
      }
    }
  }

  d_->syncAllViews();
  d_->syncPopupVisibility();

  const QModelIndex current = currentIndex();
  if (current != previousCurrent) {
    emit currentIndexChanged(current);
  }

  emit modelChanged(d_->model);
  updateGeometry();
}

QItemSelectionModel* AdNavigationMenu::selectionModel() const { return d_->selectionModel; }

void AdNavigationMenu::setSelectionModel(QItemSelectionModel* model) {
  const QModelIndex previousCurrent = currentIndex();

  if (model && d_->model != model->model()) {
    setModel(model->model());
  }

  if (!model && d_->model) {
    d_->adoptSelectionModel(new QItemSelectionModel(d_->model, this), true);
  } else {
    d_->adoptSelectionModel(model, false);
  }

  d_->syncViewSelectionModels();
  d_->syncBarSelectionState();
  d_->updateCurrentForViews();
  d_->syncCollapsedTooltip(currentIndex());
  d_->updateAllViewports();

  const QModelIndex current = currentIndex();
  if (current != previousCurrent) {
    emit currentIndexChanged(current);
  }
}

QModelIndex AdNavigationMenu::currentIndex() const {
  return d_->selectionModel ? d_->selectionModel->currentIndex() : QModelIndex();
}

void AdNavigationMenu::setCurrentIndex(const QModelIndex& index) {
  const QModelIndex sourceIndex = mapToSourceIndex(index);
  if (sourceIndex.isValid() && sourceIndex.model() != d_->model) {
    return;
  }
  d_->setCurrentIndexInternal(sourceIndex, false);
}

bool AdNavigationMenu::isExpanded(const QModelIndex& index) const {
  return d_->containsExpanded(mapToSourceIndex(index));
}

void AdNavigationMenu::setExpanded(const QModelIndex& index, bool expanded) {
  const QModelIndex sourceIndex = mapToSourceIndex(index);
  if (sourceIndex.isValid() && sourceIndex.model() != d_->model) {
    return;
  }
  d_->setExpandedInternal(sourceIndex, expanded, true);
}

void AdNavigationMenu::expand(const QModelIndex& index) { setExpanded(index, true); }

void AdNavigationMenu::collapse(const QModelIndex& index) { setExpanded(index, false); }

void AdNavigationMenu::expandAll() {
  if (!d_->model) {
    return;
  }

  QStringList expandedStableIds;
  std::function<void(const QModelIndex&)> collectSubmenus;
  collectSubmenus = [this, &expandedStableIds, &collectSubmenus](const QModelIndex& parent) {
    const int rowCount = d_->model->rowCount(parent);
    for (int row = 0; row < rowCount; ++row) {
      const QModelIndex index = d_->model->index(row, 0, parent);
      if (!index.isValid()) {
        continue;
      }
      if (isSubmenuIndex(index)) {
        const QString stableId = d_->stableIdForIndexNormalized(index);
        if (!stableId.isEmpty()) {
          expandedStableIds.append(stableId);
        }
      }
      if (d_->model->rowCount(index) > 0) {
        collectSubmenus(index);
      }
    }
  };
  collectSubmenus(QModelIndex());
  expandedStableIds = uniqueStringList(expandedStableIds);

  if (d_->expandedStableIds == expandedStableIds &&
      d_->inlineExpandedCacheStableIds == expandedStableIds) {
    return;
  }

  d_->expandedStableIds = expandedStableIds;
  d_->inlineExpandedCacheStableIds = expandedStableIds;
  d_->syncAllViews();
  d_->syncPopupVisibility();
  updateGeometry();
  if (d_->mode == Mode::Inline && !d_->collapsed) {
    adjustSize();
    if (QWidget* parent = parentWidget()) {
      parent->updateGeometry();
      if (QLayout* layout = parent->layout()) {
        layout->activate();
      }
    }
  }
}

void AdNavigationMenu::collapseAll() {
  if (d_->expandedStableIds.isEmpty() && d_->collapsedPopupStableIds.isEmpty()) {
    return;
  }
  d_->expandedStableIds.clear();
  d_->collapsedPopupStableIds.clear();
  d_->syncAllViews();
  d_->syncPopupVisibility();
  updateGeometry();
  if (d_->mode == Mode::Inline && !d_->collapsed) {
    adjustSize();
    if (QWidget* parent = parentWidget()) {
      parent->updateGeometry();
      if (QLayout* layout = parent->layout()) {
        layout->activate();
      }
    }
  }
}

QSize AdNavigationMenu::sizeHint() const {
  const QSize content = d_->rootContentSize();
  if (d_->mode == Mode::Horizontal) {
    return QSize(std::max(width(), content.width()), content.height());
  }
  if (d_->collapsed) {
    const auto style = resolvedVisualStyle(d_->mode, d_->colorScheme, d_->collapsed);
    return QSize(collapsedInlineRootWidth(style), content.height());
  }
  return QSize(256, content.height());
}

QSize AdNavigationMenu::minimumSizeHint() const {
  if (d_->mode == Mode::Horizontal) {
    return d_->rootContentSize();
  }
  const auto style = resolvedVisualStyle(d_->mode, d_->colorScheme, d_->collapsed);
  return QSize(d_->collapsed ? collapsedInlineRootWidth(style) : 120,
               style.metrics.itemHeight + style.metrics.itemMarginBlock * 2);
}

void AdNavigationMenu::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }
  if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange ||
      event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::LayoutDirectionChange) {
    d_->syncAllViews();
    d_->syncPopupVisibility();
    updateGeometry();
    update();
  }
}

void AdNavigationMenu::focusInEvent(QFocusEvent* event) {
  QWidget::focusInEvent(event);
  if (QWidget* active = d_->activeViewWidget()) {
    active->setFocus();
  }
}

void AdNavigationMenu::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  d_->hideTooltip();
  d_->syncPopupVisibility();
}

void AdNavigationMenu::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  d_->syncPopupVisibility();
}

bool AdNavigationMenu::eventFilter(QObject* watched, QEvent* event) {
  if (!watched || !event) {
    return QWidget::eventFilter(watched, event);
  }

  AdNavigationMenu::Private::PopupLevel* watchedLevel = nullptr;
  const bool fromPopupLevel = [&]() {
    for (const auto& level : d_->popupLevels) {
      if (!level) {
        continue;
      }
      if (watched == level->shell.data() || watched == level->renderedRoot.data()) {
        watchedLevel = level.get();
        return true;
      }
    }
    return false;
  }();

  if (fromPopupLevel) {
    if (event->type() == QEvent::Destroy && watchedLevel) {
      if (watched == watchedLevel->renderedRoot.data()) {
        watchedLevel->renderedRoot.clear();
      }
      if (watched == watchedLevel->shell.data()) {
        watchedLevel->shell.clear();
        watchedLevel->view.clear();
        watchedLevel->renderedRoot.clear();
        watchedLevel->submenuIndex = QModelIndex();
        watchedLevel->renderedForIndex = QModelIndex();
      }
    } else if (event->type() == QEvent::Enter) {
      d_->cancelHoverClose();
    } else if (event->type() == QEvent::Leave &&
               d_->submenuTrigger == TriggerSubMenuAction::Hover && d_->popupLikeMode()) {
      d_->scheduleHoverClose();
    }
  }

  return QWidget::eventFilter(watched, event);
}

QObject* AdNavigationMenu::popupOwnerObject() const { return const_cast<AdNavigationMenu*>(this); }

QWidget* AdNavigationMenu::popupAnchorWidget() const { return const_cast<AdNavigationMenu*>(this); }

QWidget* AdNavigationMenu::popupScopeWindow() const {
  return detail::resolvePopupScopeWindow(this);
}

bool AdNavigationMenu::popupIsVisible() const {
  return std::any_of(d_->popupLevels.cbegin(), d_->popupLevels.cend(), [](const auto& level) {
    return level && level->shell && level->shell->isVisible();
  });
}

bool AdNavigationMenu::popupContainsGlobalPos(const QPoint& globalPos) const {
  if (detail::widgetContainsGlobalPos(this, globalPos)) {
    return true;
  }
  return std::any_of(d_->popupLevels.cbegin(), d_->popupLevels.cend(),
                     [&globalPos](const auto& level) {
                       return level && level->shell && level->shell->isVisible() &&
                              detail::widgetContainsGlobalPos(level->shell, globalPos);
                     });
}

void AdNavigationMenu::popupCloseFromHost(detail::PopupCloseReason reason) {
  Q_UNUSED(reason)
  d_->pendingHoverIndex = QModelIndex();
  d_->hoverOpenTimer.stop();
  d_->hoverCloseTimer.stop();
  d_->clearPopupExpandedSubmenus();
  d_->syncAllViews();
  d_->syncPopupVisibility();
}

void AdNavigationMenu::popupRelayoutFromHost() { d_->syncPopupVisibility(); }

int AdNavigationMenu::effectiveIndentation(const detail::MenuVisualStyle& style) const {
  if (d_->indentationOverride >= 0) {
    return d_->indentationOverride;
  }
  return std::max(0, style.metrics.indentation);
}

AdNavigationMenu::SemanticStyles AdNavigationMenu::resolvedSemanticStyles(Mode mode,
                                                                          ColorScheme colorScheme,
                                                                          bool collapsed) const {
  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this);
  StyleContext ctx;
  ctx.model = model();
  ctx.selectionModel = selectionModel();
  ctx.mode = mode;
  ctx.colorScheme = resolvedColorScheme(colorScheme, resolvedTheme.theme.scheme);
  ctx.collapsed = collapsed;
  ctx.popupVisible = popupIsVisible();
  return d_->semanticStyleResolver ? d_->semanticStyleResolver(ctx) : d_->semanticStyles;
}

detail::MenuVisualStyle AdNavigationMenu::resolvedVisualStyle(Mode mode, ColorScheme colorScheme,
                                                              bool collapsed) const {
  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this);
  MenuStyleInput input;
  input.mode = mode;
  input.colorScheme = resolvedColorScheme(colorScheme, resolvedTheme.theme.scheme);
  input.collapsed = collapsed;
  input.baseFont = font();
  input.componentTokens = componentTokens();
  input.semanticStyles = resolvedSemanticStyles(mode, colorScheme, collapsed);
  return detail::resolveMenuVisualStyle(input, resolvedTheme);
}

}  // namespace adqt::widgets
