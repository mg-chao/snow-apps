#include "select.h"

#include "detail/select_models.h"
#include "detail/select_option_utils.h"
#include "detail/select_selection_controller.h"
#include "detail/popup_shadow.h"
#include "detail/qt_tooltip_bridge.h"
#include "detail/timing_hub.h"
#include "detail/top_level_popup_window.h"
#include "detail/overlay_accessibility.h"
#include "antd_icons.h"
#include "interaction_overlay_manager.h"
#include "icons/widget_icons.h"
#include "popup_placement.h"
#include "scroll_area.h"
#include "select_style.h"
#include "theme/theme.h"

#include <QAbstractItemDelegate>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPalette>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSet>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace adqt::widgets {

namespace {

namespace outlined_icons = adqt::icons::antd::outlined;
namespace filled_icons = adqt::icons::antd::filled;
constexpr char kSuffixSpinnerFrameKey[] = "AdSelect.SuffixSpinnerFrame";
constexpr char kShowLayoutRefreshKey[] = "AdSelect.ShowLayoutRefresh";
constexpr char kTagTextMetadataKey[] = "__tagText";
constexpr char kSelectedTextMetadataKey[] = "__selectedText";
constexpr char kValueVariantMetadataKey[] = "__valueVariant";

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

QPixmap renderIconPixmap(const adqt::icons::IconRef& icon, int iconSize, qreal dpr,
                         int rotationDegrees = 0) {
  if (!adqt::icons::isValid(icon)) {
    return {};
  }

  if (rotationDegrees == 0) {
    adqt::icons::IconRenderRequest request;
    request.logicalSize = QSize(iconSize, iconSize);
    request.devicePixelRatio = dpr;
    return adqt::icons::renderIconPixmap(icon, request);
  }

  QPixmap pixmap(QSize(qCeil(iconSize * dpr), qCeil(iconSize * dpr)));
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  const QRectF rect(0, 0, iconSize, iconSize);
  painter.translate(rect.center());
  painter.rotate(rotationDegrees);
  adqt::icons::paintIcon(&painter, icon,
                         QRectF(-iconSize / 2.0, -iconSize / 2.0, iconSize, iconSize));
  return pixmap;
}

QString syntheticRoleFieldName(int role) { return QStringLiteral("__role_%1").arg(role); }

QRect widgetGlobalRect(const QWidget* widget) {
  if (!widget) {
    return QRect();
  }
  return QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size());
}

bool widgetContainsGlobalPos(const QWidget* widget, const QPoint& globalPos) {
  const QRect globalRect = widgetGlobalRect(widget);
  return globalRect.isValid() && globalRect.contains(globalPos);
}

QPixmap appendFeedbackIconPixmap(const QPixmap& leadingPixmap,
                                 const adqt::icons::IconRef& feedbackIcon, int iconSize, int gap,
                                 qreal dpr, int feedbackRotationDegrees = 0) {
  if (leadingPixmap.isNull() || !adqt::icons::isValid(feedbackIcon)) {
    return leadingPixmap;
  }

  const int logicalWidth = iconSize + gap + iconSize;
  QPixmap combined(QSize(qCeil(logicalWidth * dpr), qCeil(iconSize * dpr)));
  combined.setDevicePixelRatio(dpr);
  combined.fill(Qt::transparent);

  const QPixmap feedbackPixmap =
      renderIconPixmap(feedbackIcon, iconSize, dpr, feedbackRotationDegrees);

  QPainter painter(&combined);
  painter.drawPixmap(QPointF(0, 0), leadingPixmap);
  if (!feedbackPixmap.isNull()) {
    painter.drawPixmap(QPointF(iconSize + gap, 0), feedbackPixmap);
  }
  return combined;
}

QStringList uniqueStringList(const QStringList& values) {
  QStringList out;
  out.reserve(values.size());
  QSet<QString> seen;
  for (const QString& value : values) {
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty() || seen.contains(trimmed)) {
      continue;
    }
    seen.insert(trimmed);
    out.append(trimmed);
  }
  return out;
}

QString escapedForRegex(const QString& text) { return QRegularExpression::escape(text); }

bool iconRefsEqual(const adqt::icons::IconRef& lhs, const adqt::icons::IconRef& rhs) {
  return lhs == rhs;
}

bool selectionItemsEqual(const QVector<AdSelect::SelectionItem>& lhs,
                         const QVector<AdSelect::SelectionItem>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (int i = 0; i < lhs.size(); ++i) {
    if (lhs.at(i).value != rhs.at(i).value || lhs.at(i).label != rhs.at(i).label) {
      return false;
    }
  }
  return true;
}

QString defaultSelectAccessibleName(const AdSelect* select) {
  if (!select) {
    return AdSelect::tr("Select");
  }

  QString explicitName = select->accessibleName().trimmed();
  if (!explicitName.isEmpty()) {
    return explicitName;
  }

  switch (select->mode()) {
    case AdSelect::Mode::Single:
      return AdSelect::tr("Select");
    case AdSelect::Mode::Multiple:
      return AdSelect::tr("Multi-select");
    case AdSelect::Mode::Tags:
      return AdSelect::tr("Tag select");
  }
  return AdSelect::tr("Select");
}

QString defaultSelectAccessibleDescription(const AdSelect* select) {
  if (!select) {
    return QString();
  }

  QString explicitDescription = select->accessibleDescription().trimmed();
  if (!explicitDescription.isEmpty()) {
    return explicitDescription;
  }

  QString placeholder = select->placeholder().trimmed();
  if (!placeholder.isEmpty()) {
    return placeholder;
  }

  if (select->mode() == AdSelect::Mode::Single) {
    return AdSelect::tr("Use arrow keys to browse options.");
  }
  return AdSelect::tr("Use arrow keys to browse options and Space to toggle selection.");
}

QString selectAccessibleValueText(const AdSelect* select) {
  if (!select) {
    return QString();
  }

  if (select->mode() == AdSelect::Mode::Single) {
    return select->currentText();
  }

  QStringList labels;
  const QVector<AdSelect::SelectionItem> items = select->currentItems();
  labels.reserve(items.size());
  for (const AdSelect::SelectionItem& item : items) {
    const QString label = item.label.trimmed();
    if (!label.isEmpty()) {
      labels.append(label);
    }
  }
  return labels.join(QStringLiteral(", "));
}

class AdSelectAccessible final : public QAccessibleWidget {
 public:
  explicit AdSelectAccessible(AdSelect* select)
      : QAccessibleWidget(select, select && select->mode() == AdSelect::Mode::Single
                                      ? QAccessible::ComboBox
                                      : QAccessible::List) {}

  QString text(QAccessible::Text t) const override {
    const auto* select = qobject_cast<AdSelect*>(object());
    if (!select) {
      return QAccessibleWidget::text(t);
    }

    switch (t) {
      case QAccessible::Name:
        return defaultSelectAccessibleName(select);
      case QAccessible::Description:
        return defaultSelectAccessibleDescription(select);
      case QAccessible::Value:
        return selectAccessibleValueText(select);
      default:
        return QAccessibleWidget::text(t);
    }
  }

  QAccessible::Role role() const override {
    const auto* select = qobject_cast<AdSelect*>(object());
    if (!select) {
      return QAccessibleWidget::role();
    }
    return select->mode() == AdSelect::Mode::Single ? QAccessible::ComboBox : QAccessible::List;
  }

  QAccessible::State state() const override {
    QAccessible::State st = QAccessibleWidget::state();
    const auto* select = qobject_cast<AdSelect*>(object());
    if (!select) {
      return st;
    }

    st.focusable = true;
    st.expandable = true;
    st.hasPopup = true;
    st.expanded = select->popupVisible();
    st.collapsed = !select->popupVisible();
    st.readOnly = !select->editable();
    st.editable = select->editable();
    st.selectable = true;
    if (select->mode() != AdSelect::Mode::Single) {
      st.multiSelectable = true;
      st.extSelectable = true;
    }
    if (select->disabled()) {
      st.disabled = true;
    }
    return st;
  }
};

QAccessibleInterface* selectAccessibleFactory(const QString& className, QObject* object) {
  Q_UNUSED(className)
  if (auto* select = qobject_cast<AdSelect*>(object)) {
    return new AdSelectAccessible(select);
  }
  return nullptr;
}

void ensureSelectAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(selectAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

QVariantList normalizedSelectValues(const QVariantList& values, AdSelect::Mode mode, int maxCount) {
  detail::SelectSelectionController controller;
  controller.setMode(mode);
  controller.setMaxCount(maxCount);
  controller.setCurrentValues(values);
  return controller.currentValues();
}

void disposeOwnedSelectionModel(QPointer<QItemSelectionModel>& selectionModel) {
  if (selectionModel) {
    selectionModel->deleteLater();
  }
  selectionModel.clear();
}

QPixmap makeSpinnerPixmap(const QSize& logicalSize, qreal devicePixelRatio, const QColor& color,
                          int angleDegrees) {
  if (!logicalSize.isValid() || logicalSize.isEmpty()) {
    return QPixmap();
  }

  const qreal dpr = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
  const int logicalW = qMax(1, logicalSize.width());
  const int logicalH = qMax(1, logicalSize.height());
  const int pixelW = qMax(1, qRound(static_cast<qreal>(logicalW) * dpr));
  const int pixelH = qMax(1, qRound(static_cast<qreal>(logicalH) * dpr));

  QPixmap spinner(pixelW, pixelH);
  spinner.setDevicePixelRatio(dpr);
  spinner.fill(Qt::transparent);

  const int normalizedAngle = ((angleDegrees % 360) + 360) % 360;
  const qreal side = std::max<qreal>(8.0, std::min(logicalW, logicalH) - 2.0);
  const QPointF center(static_cast<qreal>(logicalW) / 2.0, static_cast<qreal>(logicalH) / 2.0);
  const QRectF spinnerRect(center.x() - side / 2.0, center.y() - side / 2.0, side, side);
  const qreal strokeWidth =
      std::clamp(static_cast<qreal>(std::min(logicalW, logicalH)) * 0.08, 1.0, 2.0);

  QPainter painter(&spinner);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.translate(center);
  painter.rotate(static_cast<qreal>(normalizedAngle));
  painter.translate(-center);
  QPen pen(color, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawArc(spinnerRect, 90 * 16, -270 * 16);
  return spinner;
}

bool setWidgetFontIfChanged(QWidget* widget, const QFont& font) {
  if (!widget || widget->font() == font) {
    return false;
  }
  widget->setFont(font);
  return true;
}

bool setWidgetPaletteIfChanged(QWidget* widget, const QPalette& palette) {
  if (!widget || widget->palette() == palette) {
    return false;
  }
  widget->setPalette(palette);
  return true;
}

bool setWidgetAutoFillBackgroundIfChanged(QWidget* widget, bool enabled) {
  if (!widget || widget->autoFillBackground() == enabled) {
    return false;
  }
  widget->setAutoFillBackground(enabled);
  return true;
}

bool setWidgetContentsMarginsIfChanged(QWidget* widget, const QMargins& margins) {
  if (!widget || widget->contentsMargins() == margins) {
    return false;
  }
  widget->setContentsMargins(margins);
  return true;
}

bool setWidgetFixedHeightIfChanged(QWidget* widget, int height) {
  if (!widget) {
    return false;
  }
  const int normalized = std::max(0, height);
  bool changed = false;
  if (widget->minimumHeight() != normalized) {
    widget->setMinimumHeight(normalized);
    changed = true;
  }
  if (widget->maximumHeight() != normalized) {
    widget->setMaximumHeight(normalized);
    changed = true;
  }
  if (widget->height() != normalized) {
    widget->resize(widget->width(), normalized);
    changed = true;
  }
  return changed;
}

bool resetWidgetHeightConstraintsIfChanged(QWidget* widget) {
  if (!widget) {
    return false;
  }

  bool changed = false;
  if (widget->minimumHeight() != 0) {
    widget->setMinimumHeight(0);
    changed = true;
  }
  if (widget->maximumHeight() != QWIDGETSIZE_MAX) {
    widget->setMaximumHeight(QWIDGETSIZE_MAX);
    changed = true;
  }
  return changed;
}

bool setWidgetFixedWidthIfChanged(QWidget* widget, int width) {
  if (!widget) {
    return false;
  }
  const int normalized = std::max(0, width);
  bool changed = false;
  if (widget->minimumWidth() != normalized) {
    widget->setMinimumWidth(normalized);
    changed = true;
  }
  if (widget->maximumWidth() != normalized) {
    widget->setMaximumWidth(normalized);
    changed = true;
  }
  return changed;
}

bool setWidgetCursorIfChanged(QWidget* widget, Qt::CursorShape cursorShape) {
  if (!widget) {
    return false;
  }
  if (widget->cursor().shape() == cursorShape) {
    return false;
  }
  widget->setCursor(cursorShape);
  return true;
}

QRectF joinedSelectorRect(const QRect& bounds, qreal borderWidth, bool joinedLeft,
                          bool joinedRight) {
  const qreal half = std::max<qreal>(0.0, borderWidth / 2.0);
  qreal leftInset = half + 0.5;
  qreal rightInset = half + 0.5;
  if (joinedLeft) {
    leftInset = half;
  }
  if (joinedRight) {
    rightInset = half;
  }
  return QRectF(bounds).adjusted(leftInset, half + 0.5, -rightInset, -half - 0.5);
}

bool setLayoutContentsMarginsIfChanged(QLayout* layout, const QMargins& margins) {
  if (!layout || layout->contentsMargins() == margins) {
    return false;
  }
  layout->setContentsMargins(margins);
  return true;
}

bool setLayoutSpacingIfChanged(QLayout* layout, int spacing) {
  if (!layout || layout->spacing() == spacing) {
    return false;
  }
  layout->setSpacing(spacing);
  return true;
}

int boundedWidgetHeightHint(const QWidget* widget, int availableWidth) {
  if (!widget) {
    return 0;
  }

  int hintHeight = widget->sizeHint().height();
  if (availableWidth > 0 && widget->sizePolicy().hasHeightForWidth()) {
    hintHeight = widget->heightForWidth(availableWidth);
  }
  hintHeight = std::max(hintHeight, widget->minimumSizeHint().height());

  const int minHeight = std::max(0, widget->minimumHeight());
  const int maxHeight = std::max(minHeight, widget->maximumHeight());
  return std::clamp(hintHeight, minHeight, maxHeight);
}

detail::PopupPlacement toPopupPlacement(AdSelect::Placement placement) {
  switch (placement) {
    case AdSelect::Placement::BottomLeft:
      return detail::PopupPlacement::BottomLeft;
    case AdSelect::Placement::BottomRight:
      return detail::PopupPlacement::BottomRight;
    case AdSelect::Placement::TopLeft:
      return detail::PopupPlacement::TopLeft;
    case AdSelect::Placement::TopRight:
      return detail::PopupPlacement::TopRight;
    case AdSelect::Placement::BottomCenter:
      return detail::PopupPlacement::BottomCenter;
    case AdSelect::Placement::TopCenter:
      return detail::PopupPlacement::TopCenter;
  }
  return detail::PopupPlacement::BottomLeft;
}

QPainterPath roundedRectPath(const QRectF& rect, qreal radius) {
  const qreal clampedRadius = std::clamp(radius, 0.0, std::min(rect.width(), rect.height()) / 2.0);
  QPainterPath path;
  path.addRoundedRect(rect, clampedRadius, clampedRadius);
  return path;
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

class WrappingTagsLayout final : public QLayout {
 public:
  explicit WrappingTagsLayout(QWidget* parent = nullptr) : QLayout(parent) {
    setContentsMargins(0, 0, 0, 0);
    setSpacing(0);
  }

  ~WrappingTagsLayout() override {
    while (QLayoutItem* item = takeAt(0)) {
      delete item;
    }
  }

  void addItem(QLayoutItem* item) override {
    if (!item) {
      return;
    }
    items_.append(item);
  }

  int count() const override { return static_cast<int>(items_.size()); }

  QLayoutItem* itemAt(int index) const override {
    if (index < 0 || index >= items_.size()) {
      return nullptr;
    }
    return items_.at(index);
  }

  QLayoutItem* takeAt(int index) override {
    if (index < 0 || index >= items_.size()) {
      return nullptr;
    }
    return items_.takeAt(index);
  }

  Qt::Orientations expandingDirections() const override { return {}; }

  bool hasHeightForWidth() const override { return true; }

  int heightForWidth(int width) const override {
    return doLayout(QRect(0, 0, std::max(0, width), 0), true);
  }

  int layoutHeightForWidth(int width) const { return heightForWidth(width); }

  QSize sizeHint() const override { return minimumSize(); }

  QSize minimumSize() const override {
    QSize size;
    for (QLayoutItem* item : items_) {
      if (!item) {
        continue;
      }
      size = size.expandedTo(item->minimumSize());
    }
    const QMargins margins = contentsMargins();
    size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    return size;
  }

  void setGeometry(const QRect& rect) override {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
  }

 private:
  int doLayout(const QRect& rect, bool testOnly) const {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);
    QRect effectiveRect = rect.adjusted(left, top, -right, -bottom);
    if (effectiveRect.width() < 0) {
      effectiveRect.setWidth(0);
    }

    const int horizontalSpacing = std::max(0, spacing());
    // Match Ant Design multiple selector layout:
    // item vertical rhythm is driven by each tag's marginBlock, so wrapped
    // lines should not add extra row spacing on top.
    const int verticalSpacing = 0;
    int x = effectiveRect.x();
    int y = effectiveRect.y();
    int lineHeight = 0;

    auto remainingWidth = [&effectiveRect, &x]() {
      return std::max(0, effectiveRect.right() - x + 1);
    };

    for (QLayoutItem* item : items_) {
      if (!item) {
        continue;
      }

      QSize itemSize = item->sizeHint();
      if (!itemSize.isValid()) {
        itemSize = item->minimumSize();
      }
      const QSize minimum = item->minimumSize();
      int minWidth = std::max(0, minimum.width());
      int itemHeight = std::max(itemSize.height(), minimum.height());

      QWidget* widget = item->widget();
      bool expanding = false;
      if (widget) {
        minWidth = std::max(minWidth, std::max(0, widget->minimumWidth()));
        itemHeight = std::max(itemHeight, std::max(0, widget->minimumHeight()));
        if (itemSize.width() <= 0) {
          const QSize hint =
              widget->sizeHint().isValid() ? widget->sizeHint() : widget->minimumSizeHint();
          minWidth = std::max(minWidth, std::max(0, hint.width()));
        }
        if (itemHeight <= 0) {
          const QSize hint =
              widget->sizeHint().isValid() ? widget->sizeHint() : widget->minimumSizeHint();
          itemHeight = std::max(itemHeight, std::max(0, hint.height()));
        }
        const QSizePolicy::Policy horizontalPolicy = widget->sizePolicy().horizontalPolicy();
        expanding = horizontalPolicy == QSizePolicy::Expanding ||
                    horizontalPolicy == QSizePolicy::MinimumExpanding;
      }

      int itemWidth = std::max(itemSize.width(), minWidth);
      if (expanding) {
        itemWidth = std::max(minWidth, remainingWidth());
      }
      const int maxItemWidth = std::max(0, effectiveRect.width());
      if (maxItemWidth > 0) {
        itemWidth = std::min(itemWidth, maxItemWidth);
      }

      if (x > effectiveRect.x() && x + itemWidth > effectiveRect.right() + 1) {
        x = effectiveRect.x();
        y += lineHeight + verticalSpacing;
        lineHeight = 0;
        if (expanding) {
          itemWidth = std::max(minWidth, remainingWidth());
          if (maxItemWidth > 0) {
            itemWidth = std::min(itemWidth, maxItemWidth);
          }
        }
      }

      if (!testOnly) {
        item->setGeometry(QRect(QPoint(x, y), QSize(itemWidth, itemHeight)));
      }

      x += itemWidth + horizontalSpacing;
      lineHeight = std::max(lineHeight, itemHeight);
    }

    const int contentBottom = lineHeight > 0 ? (y + lineHeight) : effectiveRect.y();
    return contentBottom - rect.y() + bottom;
  }

  QVector<QLayoutItem*> items_;
};

constexpr int kSelectRowHeaderRole = Qt::UserRole + 97;
constexpr int kSelectRowEmptyRole = Qt::UserRole + 98;

}  // namespace

class FlatIconToolButton final : public QToolButton {
 public:
  explicit FlatIconToolButton(QWidget* parent = nullptr) : QToolButton(parent) {
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover, true);
    setAutoFillBackground(false);
  }

  void setBackgroundDecoration(const QColor& background, int radius) {
    const int normalizedRadius = std::max(0, radius);
    if (background_ == background && radius_ == normalizedRadius) {
      return;
    }

    background_ = background;
    radius_ = normalizedRadius;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (background_.isValid() && background_.alpha() > 0) {
      const QRectF fillRect = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
      if (fillRect.isValid()) {
        painter.fillPath(roundedRectPath(fillRect, radius_), background_);
      }
    }

    const QSize targetIconSize = iconSize().isValid() ? iconSize() : QSize(width(), height());
    const QRect iconRect((width() - targetIconSize.width()) / 2,
                         (height() - targetIconSize.height()) / 2, targetIconSize.width(),
                         targetIconSize.height());

    const QIcon::Mode iconMode =
        !isEnabled()
            ? QIcon::Disabled
            : (isDown() ? QIcon::Selected : (underMouse() ? QIcon::Active : QIcon::Normal));
    const QIcon::State iconState = isChecked() ? QIcon::On : QIcon::Off;
    if (!icon().isNull() && iconRect.isValid()) {
      const QPixmap iconPixmap = icon().pixmap(targetIconSize, iconMode, iconState);
      if (!iconPixmap.isNull()) {
        painter.drawPixmap(iconRect.topLeft(), iconPixmap);
        return;
      }
    }

    if (!text().isEmpty()) {
      painter.setPen(palette().color(QPalette::ButtonText));
      painter.setFont(font());
      painter.drawText(rect(), Qt::AlignCenter, text());
    }
  }

 private:
  QColor background_;
  int radius_ = 0;
};

class TagChipWidget final : public QWidget {
 public:
  explicit TagChipWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setAutoFillBackground(false);
  }

  void setVisualStyle(const QColor& background, const QColor& borderColor, int borderRadius,
                      int borderWidth, int blockInset) {
    const int normalizedRadius = std::max(0, borderRadius);
    const int normalizedBorderWidth = std::max(0, borderWidth);
    const int normalizedBlockInset = std::max(0, blockInset);
    if (background_ == background && borderColor_ == borderColor &&
        borderRadius_ == normalizedRadius && borderWidth_ == normalizedBorderWidth &&
        blockInset_ == normalizedBlockInset) {
      return;
    }

    background_ = background;
    borderColor_ = borderColor;
    borderRadius_ = normalizedRadius;
    borderWidth_ = normalizedBorderWidth;
    blockInset_ = normalizedBlockInset;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    const int safeBlockInset = std::clamp(blockInset_, 0, std::max(0, height() / 2));
    const qreal borderWidth =
        (borderColor_.isValid() && borderColor_.alpha() > 0 && borderWidth_ > 0)
            ? static_cast<qreal>(borderWidth_)
            : 0.0;
    const qreal inset = borderWidth > 0.0 ? borderWidth * 0.5 : 0.0;
    const QRectF chipRect =
        QRectF(rect()).adjusted(inset, inset + static_cast<qreal>(safeBlockInset), -inset,
                                -inset - static_cast<qreal>(safeBlockInset));
    if (!chipRect.isValid()) {
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPainterPath chipPath = roundedRectPath(chipRect, borderRadius_);
    if (background_.isValid() && background_.alpha() > 0) {
      painter.fillPath(chipPath, background_);
    }

    if (borderWidth > 0.0) {
      QPen borderPen(borderColor_, borderWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
      painter.setPen(borderPen);
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(chipPath);
    }
  }

 private:
  QColor background_;
  QColor borderColor_;
  int borderRadius_ = 0;
  int borderWidth_ = 0;
  int blockInset_ = 0;
};

class AdSelect::OptionListModel final : public QAbstractListModel {
 public:
  explicit OptionListModel(AdSelect* owner) : QAbstractListModel(owner), owner_(owner) {}

  void setRows(const QVector<ModelRow>& rows) {
    beginResetModel();
    rows_ = rows;
    endResetModel();
  }

  int rowCount(const QModelIndex& parent) const override {
    if (parent.isValid()) {
      return 0;
    }
    return static_cast<int>(rows_.size());
  }

  QVariant data(const QModelIndex& index, int role) const override {
    if (!owner_ || !index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
      return QVariant();
    }

    const ModelRow& row = rows_.at(index.row());
    const detail::SelectVisualStyle& style = *owner_->visualStyle_;

    if (role == kSelectRowHeaderRole) {
      return row.header;
    }

    if (role == kSelectRowEmptyRole) {
      return row.empty;
    }

    if (role == Qt::SizeHintRole) {
      if (row.empty) {
        return QSize(0, style.metrics.emptyStateHeight);
      }
      return QSize(0, style.metrics.optionHeight);
    }

    if (row.empty) {
      if (role == Qt::DisplayRole) {
        return row.headerText;
      }
      if (role == Qt::FontRole) {
        QFont font = style.metrics.optionFont;
        font.setBold(false);
        return font;
      }
      if (role == Qt::ForegroundRole) {
        return style.emptyTextColor;
      }
      if (role == Qt::TextAlignmentRole) {
        return static_cast<int>(Qt::AlignHCenter | Qt::AlignTop);
      }
      return QVariant();
    }

    if (row.header) {
      if (role == Qt::DisplayRole) {
        return row.headerText;
      }
      if (role == Qt::FontRole) {
        QFont font = style.metrics.optionFont;
        font.setBold(true);
        return font;
      }
      if (role == Qt::ForegroundRole) {
        return style.groupTitleColor;
      }
      if (role == Qt::TextAlignmentRole) {
        return static_cast<int>(Qt::AlignVCenter | Qt::AlignLeft);
      }
      return QVariant();
    }

    if (row.optionIndex < 0 || row.optionIndex >= owner_->options_.size()) {
      return QVariant();
    }

    const Option& option = owner_->options_.at(row.optionIndex);
    if (role == Qt::DisplayRole) {
      return owner_->formattedOptionText(option);
    }
    if (role == Qt::ForegroundRole) {
      if (option.disabled) {
        return style.disabledTextColor;
      }
      if (owner_->isValueSelected(option.value)) {
        return style.optionSelectedColor;
      }
      return style.optionTextColor;
    }
    if (role == Qt::FontRole) {
      QFont font = style.metrics.optionFont;
      const QVariant previewFont =
          option.metadata.value(QString::fromLatin1(detail::kSelectFontMetadataKey));
      if (previewFont.canConvert<QFont>()) {
        font = qvariant_cast<QFont>(previewFont).resolve(font);
      }
      if (owner_->isValueSelected(option.value)) {
        font.setWeight(QFont::DemiBold);
      }
      return font;
    }
    if (role == Qt::TextAlignmentRole) {
      return static_cast<int>(Qt::AlignVCenter | Qt::AlignLeft);
    }
    if (role == Qt::UserRole) {
      return option.value;
    }
    if (role == owner_->valueRole_) {
      const QVariant rawValue =
          option.metadata.value(QString::fromLatin1(kValueVariantMetadataKey));
      return rawValue.isValid() ? rawValue : QVariant(option.value);
    }
    if (role == owner_->labelRole_) {
      return option.label;
    }
    if (role == owner_->tagTextRole_) {
      return option.metadata.value(QString::fromLatin1(kTagTextMetadataKey), option.label);
    }
    if (role == owner_->selectedTextRole_) {
      return option.metadata.value(QString::fromLatin1(kSelectedTextMetadataKey), option.label);
    }
    if (role == owner_->groupRole_) {
      return option.group;
    }
    if (role >= Qt::UserRole + 1) {
      const QVariant metadataRoleValue = option.metadata.value(syntheticRoleFieldName(role));
      if (metadataRoleValue.isValid()) {
        return metadataRoleValue;
      }
    }
    return QVariant();
  }

  Qt::ItemFlags flags(const QModelIndex& index) const override {
    if (!owner_ || !index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
      return Qt::NoItemFlags;
    }

    const ModelRow& row = rows_.at(index.row());
    if (row.empty) {
      return Qt::NoItemFlags;
    }
    if (row.header || row.optionIndex < 0 || row.optionIndex >= owner_->options_.size()) {
      return Qt::ItemIsEnabled;
    }

    const Option& option = owner_->options_.at(row.optionIndex);
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (option.disabled) {
      flags &= ~Qt::ItemIsEnabled;
      flags &= ~Qt::ItemIsSelectable;
    }
    return flags;
  }

 private:
  QPointer<AdSelect> owner_;
  QVector<ModelRow> rows_;
};

class AdSelect::PopupFrame final : public QFrame, public detail::TopLevelToolResourceReleaser {
 public:
  explicit PopupFrame(QWidget* parent = nullptr) : QFrame(parent) {
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_Hover, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
  }

  void releaseTopLevelToolResources() override { destroy(); }

  void setVisualStyle(const QColor& background, const QColor& borderColor, int borderRadius) {
    const int normalizedRadius = std::max(0, borderRadius);
    if (background_ == background && borderColor_ == borderColor &&
        borderRadius_ == normalizedRadius) {
      return;
    }

    background_ = background;
    borderColor_ = borderColor;
    borderRadius_ = normalizedRadius;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    const qreal popupRadius = std::max<qreal>(0.0, static_cast<qreal>(borderRadius_));
    const qreal popupBorderWidth = (borderColor_.isValid() && borderColor_.alpha() > 0) ? 1.0 : 0.0;
    const qreal inset = popupBorderWidth > 0.0 ? popupBorderWidth * 0.5 : 0.5;
    const QRectF visualRect = detail::antPopupShadowVisualRect(rect());
    const QRectF frameRect = visualRect.adjusted(inset, inset, -inset, -inset);
    if (!frameRect.isValid()) {
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath framePath;
    framePath.addRoundedRect(frameRect, popupRadius, popupRadius);
    detail::paintAntPopupBoxShadowSecondary(painter, framePath);
    if (background_.isValid() && background_.alpha() > 0) {
      painter.fillPath(framePath, background_);
    }

    if (popupBorderWidth > 0.0) {
      QPen borderPen(borderColor_, popupBorderWidth);
      painter.setPen(borderPen);
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(frameRect, popupRadius, popupRadius);
    }
  }

 private:
  QColor background_;
  QColor borderColor_;
  int borderRadius_ = 0;
};

class AdSelect::OptionListDelegate final : public QStyledItemDelegate {
 public:
  explicit OptionListDelegate(AdSelect* owner) : QStyledItemDelegate(owner), owner_(owner) {}

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    const QVariant candidate = index.data(Qt::SizeHintRole);
    if (candidate.isValid()) {
      const QSize size = candidate.toSize();
      if (size.isValid()) {
        return size;
      }
    }
    return QStyledItemDelegate::sizeHint(option, index);
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    if (!painter || !index.isValid() || !owner_ || !owner_->visualStyle_) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }

    QStyleOptionViewItem itemOption(option);
    initStyleOption(&itemOption, index);

    const detail::SelectVisualStyle& style = *owner_->visualStyle_;
    const bool isHeader = index.data(kSelectRowHeaderRole).toBool();
    const bool isEmpty = index.data(kSelectRowEmptyRole).toBool();
    const bool isSelected = (itemOption.state & QStyle::State_Selected) != 0;
    const bool isHovered = (itemOption.state & QStyle::State_MouseOver) != 0;
    const bool isEnabled = (itemOption.state & QStyle::State_Enabled) != 0;
    bool optionSelected = false;
    bool optionDisabled = false;
    if (!isHeader && !isEmpty && index.row() >= 0 && index.row() < owner_->rows_.size()) {
      const AdSelect::ModelRow& modelRow = owner_->rows_.at(index.row());
      if (modelRow.optionIndex >= 0 && modelRow.optionIndex < owner_->options_.size()) {
        const Option& modelOption = owner_->options_.at(modelRow.optionIndex);
        optionDisabled = modelOption.disabled;
        optionSelected = owner_->isValueSelected(modelOption.value);
      }
    }

    QColor background(Qt::transparent);
    if (!isHeader && !isEmpty) {
      if (optionSelected) {
        background = optionDisabled ? style.disabledBg : style.optionSelectedBg;
      } else if (!optionDisabled && (isHovered || isSelected)) {
        background = style.optionHoverBg;
      }
    }
    const bool hasBackground = background.isValid() && background.alpha() > 0;
    const int optionRadius = std::max(0, style.metrics.optionBorderRadius);
    const bool drawRoundedBackground = hasBackground && optionRadius > 0;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, drawRoundedBackground);

    painter->fillRect(itemOption.rect, style.popupBg);

    if (isEmpty) {
      const int optionHInset = std::max(0, style.metrics.optionPaddingHorizontal);
      const int optionVInset = std::max(0, style.metrics.optionPaddingVertical);
      QRect contentRect =
          itemOption.rect.adjusted(optionHInset, optionVInset, -optionHInset, -optionVInset);
      const int marginInline = std::max(0, style.metrics.emptyStateMarginInline);
      contentRect.adjust(marginInline, 0, -marginInline, 0);

      const int marginBlock = std::max(0, style.metrics.emptyStateMarginBlock);
      const int imageBottomMargin = std::max(0, style.metrics.emptyStateImageMarginBottom);
      const int iconWidth = std::max(30, style.metrics.emptyStateIconWidth);
      const int iconHeight = std::max(20, style.metrics.emptyStateIconHeight);
      const int textHeight = std::max(12, style.metrics.emptyDescriptionLineHeight);
      const int top = contentRect.top() + marginBlock;

      const QRectF iconRect(contentRect.left() + (contentRect.width() - iconWidth) / 2.0, top,
                            iconWidth, iconHeight);
      const auto emptyIconColors = adqt::icons::IconColors::threeTone(
          style.emptyBorderColor, style.emptyContentColor, style.emptyShadowColor);
      const adqt::icons::IconRef emptyIcon = icons::twotone::EmptySimple(emptyIconColors);
      const qreal dpr = owner_->devicePixelRatioF();
      adqt::icons::IconRenderRequest request;
      request.logicalSize = QSize(iconWidth, iconHeight);
      request.devicePixelRatio = dpr;
      const QPixmap iconPixmap = adqt::icons::renderIconPixmap(emptyIcon, request);
      if (!iconPixmap.isNull()) {
        painter->drawPixmap(QPointF(iconRect.left(), iconRect.top()), iconPixmap);
      }

      QFont textFont = style.metrics.optionFont;
      textFont.setWeight(QFont::Normal);
      textFont.setPixelSize(std::max(12, style.metrics.emptyDescriptionFontSize));
      painter->setFont(textFont);
      painter->setPen(style.emptyTextColor);
      const QFontMetrics metrics(textFont);
      const QString text =
          metrics.elidedText(itemOption.text, Qt::ElideRight, std::max(0, contentRect.width()));
      const QRect textRect(contentRect.left(), top + iconHeight + imageBottomMargin,
                           contentRect.width(), textHeight);
      painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, text);

      painter->restore();
      return;
    }

    if (hasBackground) {
      QRectF backgroundRect(itemOption.rect);
      if (backgroundRect.isValid()) {
        if (drawRoundedBackground) {
          qreal topLeftRadius = static_cast<qreal>(optionRadius);
          qreal topRightRadius = static_cast<qreal>(optionRadius);
          qreal bottomRightRadius = static_cast<qreal>(optionRadius);
          qreal bottomLeftRadius = static_cast<qreal>(optionRadius);
          if (optionSelected) {
            const bool hasSelectedPrev = rowHasSelectedOption(index.row() - 1);
            const bool hasSelectedNext = rowHasSelectedOption(index.row() + 1);
            if (hasSelectedPrev) {
              topLeftRadius = 0.0;
              topRightRadius = 0.0;
              // Overlap by half pixel to avoid anti-aliased seams between joined rows.
              backgroundRect.adjust(0.0, -0.5, 0.0, 0.0);
            }
            if (hasSelectedNext) {
              bottomRightRadius = 0.0;
              bottomLeftRadius = 0.0;
              // Overlap by half pixel to avoid anti-aliased seams between joined rows.
              backgroundRect.adjust(0.0, 0.0, 0.0, 0.5);
            }
          }
          painter->fillPath(roundedRectPath(backgroundRect, topLeftRadius, topRightRadius,
                                            bottomRightRadius, bottomLeftRadius),
                            background);
        } else {
          painter->fillRect(backgroundRect.toAlignedRect(), background);
        }
      }
    }

    QFont textFont = itemOption.font;
    const QVariant fontRole = index.data(Qt::FontRole);
    if (fontRole.canConvert<QFont>()) {
      textFont = qvariant_cast<QFont>(fontRole);
    }
    painter->setFont(textFont);

    QColor textColor = style.optionTextColor;
    const QVariant foregroundRole = index.data(Qt::ForegroundRole);
    if (foregroundRole.canConvert<QColor>()) {
      textColor = qvariant_cast<QColor>(foregroundRole);
    } else if (!isEnabled) {
      textColor = style.disabledTextColor;
    }
    painter->setPen(textColor);

    Qt::Alignment textAlignment = Qt::AlignVCenter | Qt::AlignLeft;
    const QVariant alignmentRole = index.data(Qt::TextAlignmentRole);
    if (alignmentRole.isValid()) {
      textAlignment = Qt::Alignment(alignmentRole.toInt());
    }

    const int horizontalPadding = std::max(0, style.metrics.optionPaddingHorizontal);
    const int verticalPadding = std::max(0, style.metrics.optionPaddingVertical);
    const bool showSelectedIcon = optionSelected && owner_->mode_ != AdSelect::Mode::Single;
    const int selectedIconSize = std::max(10, style.metrics.iconSize);
    const int selectedStateGap = std::max(2, style.metrics.optionStateGap);

    QRect textRect = itemOption.rect.adjusted(horizontalPadding, verticalPadding,
                                              -horizontalPadding, -verticalPadding);
    if (showSelectedIcon) {
      textRect.adjust(0, 0, -(selectedIconSize + selectedStateGap), 0);
    }
    const QFontMetrics metrics(textFont);
    const QString text =
        metrics.elidedText(itemOption.text, Qt::ElideRight, std::max(0, textRect.width()));
    painter->drawText(textRect, textAlignment, text);

    if (showSelectedIcon) {
      const QRect stateRect(
          itemOption.rect.right() - horizontalPadding - selectedIconSize + 1,
          itemOption.rect.top() + (itemOption.rect.height() - selectedIconSize) / 2,
          selectedIconSize, selectedIconSize);
      adqt::icons::IconRef checkIcon = outlined_icons::Check();
      if (adqt::icons::isValid(checkIcon)) {
        checkIcon = checkIcon.withColors(adqt::icons::IconColors::primary(
            isEnabled ? style.selectorActiveBorderColor : style.disabledTextColor));
        const qreal dpr = owner_->devicePixelRatioF();
        adqt::icons::IconRenderRequest request;
        request.logicalSize = QSize(selectedIconSize, selectedIconSize);
        request.devicePixelRatio = dpr;
        const QPixmap iconPixmap = adqt::icons::renderIconPixmap(checkIcon, request);
        if (!iconPixmap.isNull()) {
          painter->drawPixmap(stateRect.topLeft(), iconPixmap);
        }
      }
    }

    painter->restore();
  }

 private:
  bool rowHasSelectedOption(int row) const {
    if (!owner_ || row < 0 || row >= owner_->rows_.size()) {
      return false;
    }
    const AdSelect::ModelRow& modelRow = owner_->rows_.at(row);
    if (modelRow.header || modelRow.empty || modelRow.optionIndex < 0 ||
        modelRow.optionIndex >= owner_->options_.size()) {
      return false;
    }
    const Option& modelOption = owner_->options_.at(modelRow.optionIndex);
    return owner_->isValueSelected(modelOption.value);
  }

  QPointer<AdSelect> owner_;
};

AdSelect::AdSelect(QWidget* parent) : QWidget(parent) {
  ensureSelectAccessibleFactoryInstalled();
  setObjectName(QStringLiteral("adselect-root"));
  setAttribute(Qt::WA_Hover, true);
  setAttribute(Qt::WA_StyledBackground, true);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);

  qRegisterMetaType<AdSelect::SelectionItem>("adqt::widgets::AdSelect::SelectionItem");
  qRegisterMetaType<QVector<AdSelect::SelectionItem>>(
      "QVector<adqt::widgets::AdSelect::SelectionItem>");

  visualStyle_ = new detail::SelectVisualStyle();
  compositeModel_ = std::make_unique<detail::SelectCompositeModel>(this);
  compositeModel_->setPrimaryColumn(modelColumn_);
  filterProxyModel_ = std::make_unique<detail::SelectFilterProxyModel>(this);
  filterProxyModel_->setSourceModel(compositeModel_.get());
  searchFilterFields_ = {QStringLiteral("label"), QStringLiteral("value")};
  searchRoles_ = {DefaultLabelRole, DefaultValueRole};
  tokenSeparators_ = {QStringLiteral(",")};
  updateRoleConfig();

  rootLayout_ = new QHBoxLayout(this);
  rootLayout_->setContentsMargins(10, 4, 10, 4);
  rootLayout_->setSpacing(6);

  prefixLabel_ = new QLabel(this);
  prefixLabel_->setObjectName(QStringLiteral("adselect-prefix"));
  prefixLabel_->setVisible(false);
  rootLayout_->addWidget(prefixLabel_);

  contentHost_ = new QWidget(this);
  contentHost_->setAutoFillBackground(false);
  contentHost_->setAttribute(Qt::WA_Hover, true);
  contentHost_->setMouseTracking(true);
  contentLayout_ = new QHBoxLayout(contentHost_);
  contentLayout_->setContentsMargins(0, 0, 0, 0);
  contentLayout_->setSpacing(6);

  tagsContainer_ = new QWidget(contentHost_);
  tagsContainer_->setObjectName(QStringLiteral("adselect-tags"));
  tagsContainer_->setAutoFillBackground(false);
  tagsContainer_->setAttribute(Qt::WA_Hover, true);
  tagsContainer_->setMouseTracking(true);
  tagsContainer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  tagsContainer_->setVisible(false);
  tagsLayout_ = new WrappingTagsLayout(tagsContainer_);
  tagsLayout_->setContentsMargins(0, 0, 0, 0);
  tagsLayout_->setSpacing(4);
  contentLayout_->addWidget(tagsContainer_);

  placeholderLabel_ = new QLabel(tagsContainer_);
  placeholderLabel_->setObjectName(QStringLiteral("adselect-placeholder"));
  placeholderLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  placeholderLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  placeholderLabel_->setVisible(false);

  lineEdit_ = new QLineEdit(contentHost_);
  lineEdit_->setObjectName(QStringLiteral("adselect-input"));
  lineEdit_->setAttribute(Qt::WA_Hover, true);
  lineEdit_->setMouseTracking(true);
  lineEdit_->setFrame(false);
  lineEdit_->setAutoFillBackground(false);
  lineEdit_->setMinimumWidth(4);
  lineEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  QPalette inputBasePalette = lineEdit_->palette();
  inputBasePalette.setColor(QPalette::Base, QColor(0, 0, 0, 0));
  inputBasePalette.setColor(QPalette::Disabled, QPalette::Base, QColor(0, 0, 0, 0));
  lineEdit_->setPalette(inputBasePalette);
  lineEdit_->setPlaceholderText(placeholder_);
  lineEdit_->installEventFilter(this);
  contentLayout_->addWidget(lineEdit_, 1);
  rootLayout_->addWidget(contentHost_, 1);

  clearButton_ = new FlatIconToolButton(this);
  clearButton_->setObjectName(QStringLiteral("adselect-clear"));
  clearButton_->setAttribute(Qt::WA_Hover, true);
  clearButton_->setMouseTracking(true);
  clearButton_->setCursor(Qt::PointingHandCursor);
  clearButton_->installEventFilter(this);
  clearButton_->setVisible(false);

  suffixButton_ = new FlatIconToolButton(this);
  suffixButton_->setObjectName(QStringLiteral("adselect-suffix"));
  suffixButton_->setAttribute(Qt::WA_Hover, true);
  suffixButton_->setMouseTracking(true);
  suffixButton_->setCursor(Qt::PointingHandCursor);
  rootLayout_->addWidget(suffixButton_);

  listModel_ = new OptionListModel(this);

  connect(lineEdit_, &QLineEdit::textEdited, this, [this](const QString& text) {
    if (suppressLineEditChange_) {
      return;
    }

    QString effectiveText = text;
    if (mode_ == Mode::Tags && !tokenSeparators_.isEmpty()) {
      consumeTokenizedInput(text);
      effectiveText = lineEdit_ ? lineEdit_->text() : searchText_;
    }

    if (!isSearchEnabledForCurrentMode()) {
      return;
    }
    if (searchText_ == effectiveText) {
      return;
    }
    searchText_ = effectiveText;
    if (filterProxyModel_) {
      filterProxyModel_->setSearchText(searchText_);
    }
    emit searchTextChanged(searchText_);
    if (mode_ != Mode::Single) {
      updateDisplay();
    }
    if (!open_) {
      openPopup();
    } else {
      refreshRows();
    }
  });

  connect(clearButton_, &QToolButton::clicked, this, [this]() { clearSelectionInternal(true); });
  connect(suffixButton_, &QToolButton::clicked, this, [this]() {
    if (!suffixButtonTriggersPopup()) {
      return;
    }
    if (open_) {
      closePopup();
    } else {
      openPopup();
    }
  });

  applyVisualStyle();
  updateInputMode();
  updateDisplay();
  updateClearButton();
  updatePrefixVisual();
  updateSuffixVisual();
  updateAccessibility();
}

AdSelect::~AdSelect() {
  detail::syncTopLevelPopupTooltipRoute(this, nullptr, nullptr, false);
  if (sourceModel_) {
    disconnect(sourceModel_, nullptr, this, nullptr);
    sourceModel_.clear();
  }
  if (selectionModel_) {
    disconnect(selectionModel_, nullptr, this, nullptr);
    selectionModel_.clear();
  }
  ownedSelectionModel_.clear();
  if (itemDelegateOverride_) {
    disconnect(itemDelegateOverride_, nullptr, this, nullptr);
    itemDelegateOverride_.clear();
  }
  if (popupFooterWidget_) {
    disconnect(popupFooterWidget_, nullptr, this, nullptr);
    popupFooterWidget_ = nullptr;
  }

  stopInteractionFocusForOwner(this);
  detail::clearFrameSubscription(this, QString::fromLatin1(kSuffixSpinnerFrameKey));
  suffixSpinnerSubscribed_ = false;
  detail::setPopupInteractionHostOpen(this, false);
  if (popup_) {
    popup_->hide();
    popup_->deleteLater();
    popup_ = nullptr;
  }
  delete visualStyle_;
  visualStyle_ = nullptr;
}

AdSelect::SelectionSnapshot AdSelect::captureSelectionSnapshot() const {
  SelectionSnapshot snapshot;
  snapshot.currentValue = currentValue();
  snapshot.currentValues = currentValues();
  snapshot.items = currentItems();
  snapshot.currentModelIndex = currentModelIndexCache_;
  return snapshot;
}

void AdSelect::emitSelectionSignalsFromSnapshot(const SelectionSnapshot& previous) {
  const QVariant nextCurrentValue = currentValue();
  const QVariantList nextCurrentValues = currentValues();
  const QVector<SelectionItem> nextItems = currentItems();
  const QModelIndex nextCurrentModelIndex = currentModelIndex();

  if (previous.currentValue != nextCurrentValue) {
    emit currentValueChanged(nextCurrentValue);
  }
  if (previous.currentValues != nextCurrentValues) {
    emit currentValuesChanged(nextCurrentValues);
  }
  if (!selectionItemsEqual(previous.items, nextItems)) {
    emit currentItemsChanged(nextItems);
    emit selectionChanged(nextItems);
  }
  if (previous.currentModelIndex != nextCurrentModelIndex) {
    emit currentModelIndexChanged(nextCurrentModelIndex);
  }
  currentModelIndexCache_ = nextCurrentModelIndex;
}

QVariantList AdSelect::normalizedSelectionValues(const QVariantList& values) const {
  return normalizedSelectValues(values, mode_, maxCount_);
}

void AdSelect::applySelectedValues(const QVariantList& values, bool ensureTagOptions) {
  const QVariantList normalizedValues = normalizedSelectionValues(values);

  if (ensureTagOptions && mode_ == Mode::Tags) {
    for (const QVariant& value : normalizedValues) {
      if (!sourceIndexForValue(value).isValid()) {
        ensureTagOptionExists(value);
      }
    }
  }

  syncSelectionModelFromState(normalizedValues);
  setCustomTagValues(normalizedValues);
  syncSelectionKeysFromState();
}

QVariantList AdSelect::selectedModelValues() const {
  QVariantList values;
  if (!selectionModel_) {
    return values;
  }

  QModelIndexList selectedRows = selectionModel_->selectedRows(modelColumn_);
  std::sort(selectedRows.begin(), selectedRows.end(),
            [](const QModelIndex& lhs, const QModelIndex& rhs) {
              if (lhs.row() == rhs.row()) {
                return lhs.column() < rhs.column();
              }
              return lhs.row() < rhs.row();
            });

  detail::SelectRoleConfig roles;
  roles.valueRole = valueRole_;
  roles.labelRole = labelRole_;
  roles.tagTextRole = tagTextRole_;
  roles.selectedTextRole = selectedTextRole_;
  roles.groupRole = groupRole_;

  QStringList seen;
  for (const QModelIndex& index : selectedRows) {
    if (!index.isValid()) {
      continue;
    }
    const QVariant value = detail::materializeSelectOption(index, roles).value;
    const QString key = detail::selectValueKey(value);
    if (!value.isValid() || value.isNull() || key.isEmpty() || seen.contains(key)) {
      continue;
    }
    seen.append(key);
    values.append(value);
  }

  return values;
}

QVariantList AdSelect::normalizedCustomTagValues(const QVariantList& values) const {
  QVariantList out;
  QStringList seen;
  out.reserve(values.size());
  for (const QVariant& value : values) {
    const QVariant normalized = detail::normalizeSelectValue(value);
    const QString key = detail::selectValueKey(normalized);
    if (!normalized.isValid() || normalized.isNull() || key.isEmpty() || seen.contains(key)) {
      continue;
    }
    if (sourceIndexForValue(normalized).isValid()) {
      continue;
    }
    seen.append(key);
    out.append(normalized);
  }

  return out;
}

void AdSelect::setCustomTagValues(const QVariantList& values) {
  customTagValues_ = normalizedCustomTagValues(values);
  if (maxCount_ > 0) {
    const int selectedCount = static_cast<int>(selectedModelValues().size());
    const int allowedCustomCount = std::max(0, maxCount_ - selectedCount);
    if (customTagValues_.size() > allowedCustomCount) {
      customTagValues_ = customTagValues_.mid(0, allowedCustomCount);
    }
  }
}

QVariantList AdSelect::effectiveSelectedValues() const {
  QVariantList values = selectedModelValues();

  QStringList seen;
  seen.reserve(values.size() + customTagValues_.size());
  for (const QVariant& value : values) {
    const QString key = detail::selectValueKey(value);
    if (!key.isEmpty()) {
      seen.append(key);
    }
  }
  for (const QVariant& value : customTagValues_) {
    const QString key = detail::selectValueKey(value);
    if (!key.isEmpty() && !seen.contains(key)) {
      seen.append(key);
      values.append(value);
    }
  }

  if (mode_ == Mode::Single && values.size() > 1) {
    values = {values.constFirst()};
  }
  if (maxCount_ > 0 && values.size() > maxCount_) {
    values = values.mid(0, maxCount_);
  }

  return values;
}

void AdSelect::syncSelectionKeysFromState() {
  detail::SelectSelectionController controller;
  controller.setMode(mode_);
  controller.setMaxCount(maxCount_);
  controller.setCurrentValues(effectiveSelectedValues());

  currentValueKeys_ = controller.currentKeys();
  currentValueKey_ = mode_ == Mode::Single && !currentValueKeys_.isEmpty()
                         ? currentValueKeys_.constFirst()
                         : QString();
}

QModelIndex AdSelect::sourceIndexForValue(const QVariant& value) const {
  if (!sourceModel_) {
    return QModelIndex();
  }

  const QString expectedKey = detail::selectValueKey(value);
  if (expectedKey.isEmpty()) {
    return QModelIndex();
  }

  for (int row = 0; row < sourceModel_->rowCount(); ++row) {
    const QModelIndex index = sourceModel_->index(row, modelColumn_);
    if (!index.isValid()) {
      continue;
    }
    detail::SelectRoleConfig roles;
    roles.valueRole = valueRole_;
    roles.labelRole = labelRole_;
    roles.tagTextRole = tagTextRole_;
    roles.selectedTextRole = selectedTextRole_;
    roles.groupRole = groupRole_;
    const QVariant candidate = detail::materializeSelectOption(index, roles).value;
    if (detail::selectValueKey(candidate) == expectedKey) {
      return index;
    }
  }
  return QModelIndex();
}

QModelIndex AdSelect::compositeIndexForValue(const QVariant& value) const {
  if (!compositeModel_) {
    return QModelIndex();
  }

  const QModelIndex sourceIndex = sourceIndexForValue(value);
  if (sourceIndex.isValid()) {
    return compositeModel_->index(sourceIndex.row(), 0);
  }

  if (!overlayModel_) {
    return QModelIndex();
  }

  const QString expectedKey = detail::selectValueKey(value);
  if (expectedKey.isEmpty()) {
    return QModelIndex();
  }

  const int primaryRows = sourceModel_ ? sourceModel_->rowCount() : 0;
  for (int row = 0; row < overlayModel_->rowCount(); ++row) {
    const QModelIndex overlayIndex = overlayModel_->index(row, 0);
    if (!overlayIndex.isValid()) {
      continue;
    }
    const QVariant candidate = overlayModel_->data(overlayIndex, valueRole_);
    if (detail::selectValueKey(candidate) == expectedKey) {
      return compositeModel_->index(primaryRows + row, 0);
    }
  }

  return QModelIndex();
}

void AdSelect::syncSelectionStateFromSelectionModel() {
  if (!selectionModel_ || syncingSelectionModel_) {
    return;
  }

  const SelectionSnapshot previous = captureSelectionSnapshot();
  const QVariantList modelValues = selectedModelValues();
  const QVariantList normalizedModelValues = normalizedSelectionValues(modelValues);
  if (modelValues != normalizedModelValues) {
    syncSelectionModelFromState(normalizedModelValues);
  }
  setCustomTagValues(customTagValues_);
  syncSelectionKeysFromState();
  updateSelectionCaches();
  refreshRows();
  updateDisplay();
  updateClearButton();
  emitSelectionSignalsFromSnapshot(previous);
}

void AdSelect::syncSelectionModelFromState(const QVariantList& selectedValues) {
  if (!selectionModel_ || syncingSelectionModel_) {
    return;
  }
  if (sourceModel_ && selectionModel_->model() != sourceModel_) {
    return;
  }

  QModelIndexList indexes;
  QStringList seen;
  for (const QVariant& value : selectedValues) {
    const QString key = detail::selectValueKey(value);
    if (key.isEmpty() || seen.contains(key)) {
      continue;
    }
    seen.append(key);
    const QModelIndex index = sourceIndexForValue(value);
    if (index.isValid()) {
      indexes.append(index);
    }
  }

  QScopedValueRollback<bool> syncGuard(syncingSelectionModel_, true);
  QSignalBlocker blocker(selectionModel_);
  selectionModel_->clearSelection();
  selectionModel_->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
  for (const QModelIndex& index : indexes) {
    selectionModel_->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  }
  if (!indexes.isEmpty()) {
    selectionModel_->setCurrentIndex(indexes.constFirst(), QItemSelectionModel::NoUpdate);
  }
}

void AdSelect::updateRoleConfig() {
  if (!filterProxyModel_) {
    return;
  }

  detail::SelectRoleConfig roles;
  roles.valueRole = valueRole_;
  roles.labelRole = labelRole_;
  roles.tagTextRole = tagTextRole_;
  roles.selectedTextRole = selectedTextRole_;
  roles.groupRole = groupRole_;
  filterProxyModel_->setRoleConfig(roles);
  filterProxyModel_->setSearchEnabled(searchEnabled_);
  filterProxyModel_->setSearchText(searchText_);
  filterProxyModel_->setSearchPolicy(searchPolicy_);
  filterProxyModel_->setSearchRoles(searchRoles_);
  filterProxyModel_->setSearchFilterFields(effectiveSearchFilterFields());
  filterProxyModel_->setFilterPredicate(filterPredicate_);
  filterProxyModel_->setSortComparator(sortComparator_);
}

QStringList AdSelect::effectiveSearchFilterFields() const {
  if (searchFilterFieldsExplicit_ && !searchFilterFields_.isEmpty()) {
    return searchFilterFields_;
  }

  QStringList fields;
  fields.reserve(searchRoles_.size());
  for (int role : searchRoles_) {
    if (role == DefaultLabelRole) {
      fields.append(QStringLiteral("label"));
    } else if (role == DefaultValueRole) {
      fields.append(QStringLiteral("value"));
    } else {
      fields.append(syntheticRoleFieldName(role));
    }
  }
  const QStringList normalized = uniqueStringList(fields);
  return normalized.isEmpty() ? QStringList{QStringLiteral("label"), QStringLiteral("value")}
                              : normalized;
}

void AdSelect::ensureOverlayModel() {
  if (overlayModel_) {
    return;
  }

  auto* model = new QStandardItemModel(this);
  overlayModel_ = model;
  if (compositeModel_) {
    compositeModel_->setOverlayModel(model);
  }
}

void AdSelect::clearOverlayModel() {
  if (auto* model = qobject_cast<QStandardItemModel*>(overlayModel_.data())) {
    model->clear();
  }
}

AdSelect::Mode AdSelect::mode() const { return mode_; }

void AdSelect::setMode(Mode value) {
  if (mode_ == value) {
    return;
  }
  const SelectionSnapshot previous = captureSelectionSnapshot();
  const bool previousSearchEnabled = searchEnabled_;
  const QVariantList preservedValues = currentValues();
  mode_ = value;
  if (!searchEnabledExplicit_) {
    searchEnabled_ = (mode_ != Mode::Single);
  }
  if (!searchEnabled_ && !searchText_.isEmpty()) {
    searchText_.clear();
    emit searchTextChanged(searchText_);
  }
  if (mode_ != Mode::Tags) {
    clearOverlayModel();
  }

  syncModelBackedOptions(false);
  applySelectedValues(preservedValues);
  updateRoleConfig();
  updateSelectionCaches();
  updateInputMode();
  applyVisualStyle();
  refreshRows();
  updateDisplay();
  updateClearButton();
  updateSuffixVisual();
  updateAccessibility();
  emit modeChanged(mode_);
  if (previousSearchEnabled != searchEnabled_) {
    emit searchEnabledChanged(searchEnabled_);
  }
  emitSelectionSignalsFromSnapshot(previous);
}

AdSelect::ControlSize AdSelect::controlSize() const { return controlSize_; }

void AdSelect::setControlSize(ControlSize value) {
  if (controlSize_ == value) {
    return;
  }
  controlSize_ = value;
  applyVisualStyle();
  emit controlSizeChanged(controlSize_);
}

AdSelect::Variant AdSelect::variant() const { return variant_; }

void AdSelect::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  variant_ = value;
  applyVisualStyle();
  emit variantChanged(variant_);
}

AdSelect::Status AdSelect::status() const { return status_; }

void AdSelect::setStatus(Status value) {
  if (status_ == value) {
    return;
  }
  status_ = value;
  applyVisualStyle();
  emit statusChanged(status_);
}

bool AdSelect::allowClear() const { return allowClear_; }

void AdSelect::setAllowClear(bool value) {
  if (allowClear_ == value) {
    return;
  }
  allowClear_ = value;
  updateClearButton();
  emit allowClearChanged(allowClear_);
}

bool AdSelect::loading() const { return loading_; }

void AdSelect::setLoading(bool value) {
  if (loading_ == value) {
    return;
  }
  loading_ = value;
  updateLoadingSpinnerState();
  updateSuffixVisual();
  emit loadingChanged(loading_);
}

bool AdSelect::popupVisible() const { return open_; }

void AdSelect::setPopupVisible(bool value) {
  if (value) {
    openPopup();
  } else {
    closePopup();
  }
}

bool AdSelect::disabled() const { return !isEnabled(); }

void AdSelect::setDisabled(bool value) {
  if (disabled() == value) {
    return;
  }
  QWidget::setDisabled(value);
  if (value) {
    clearHovered_ = false;
    closePopup();
  }
  updateInputMode();
  applyVisualStyle();
  updateDisplay();
  updateClearButton();
  updateSuffixVisual();
  emit disabledChanged(value);
}

bool AdSelect::searchEnabled() const { return searchEnabled_; }

void AdSelect::setSearchEnabled(bool value) {
  if (searchEnabled_ == value && searchEnabledExplicit_) {
    return;
  }
  searchEnabledExplicit_ = true;
  if (searchEnabled_ == value) {
    updateRoleConfig();
    updateInputMode();
    updateDisplay();
    return;
  }
  searchEnabled_ = value;
  if (!searchEnabled_ && !searchText_.isEmpty()) {
    searchText_.clear();
    emit searchTextChanged(searchText_);
  }
  if (filterProxyModel_) {
    filterProxyModel_->setSearchEnabled(searchEnabled_);
    filterProxyModel_->setSearchText(searchText_);
  }
  updateInputMode();
  refreshRows();
  updateDisplay();
  updateSuffixVisual();
  updateAccessibility();
  emit searchEnabledChanged(searchEnabled_);
}

QString AdSelect::searchText() const { return searchText_; }

void AdSelect::setSearchText(const QString& value) {
  if (searchText_ == value) {
    return;
  }
  if (!inputMethodPreeditText_.isEmpty()) {
    inputMethodPreeditText_.clear();
  }
  searchText_ = value;
  emit searchTextChanged(searchText_);
  if (filterProxyModel_) {
    filterProxyModel_->setSearchText(searchText_);
  }
  if (isSearchEnabledForCurrentMode()) {
    if (lineEdit_ && (open_ || mode_ != Mode::Single)) {
      suppressLineEditChange_ = true;
      lineEdit_->setText(searchText_);
      suppressLineEditChange_ = false;
    }
    refreshRows();
  }
  if (mode_ != Mode::Single) {
    updateDisplay();
  }
}

AdSelect::SearchPolicy AdSelect::searchPolicy() const { return searchPolicy_; }

void AdSelect::setSearchPolicy(SearchPolicy value) {
  if (searchPolicy_ == value) {
    return;
  }
  searchPolicy_ = value;
  if (filterProxyModel_) {
    filterProxyModel_->setSearchPolicy(searchPolicy_);
  }
  refreshRows();
}

int AdSelect::maxCount() const { return maxCount_; }

void AdSelect::setMaxCount(int value) {
  if (maxCount_ == value) {
    return;
  }
  const SelectionSnapshot previous = captureSelectionSnapshot();
  const QVariantList preservedValues = currentValues();
  maxCount_ = value;
  applySelectedValues(preservedValues, false);
  updateSelectionCaches();
  refreshRows();
  updateDisplay();
  updateClearButton();
  emit maxCountChanged(maxCount_);
  emitSelectionSignalsFromSnapshot(previous);
}

int AdSelect::maxTagCount() const { return maxTagCount_; }

void AdSelect::setMaxTagCount(int value) {
  if (maxTagCount_ == value) {
    return;
  }
  maxTagCount_ = value;
  updateDisplay();
  emit maxTagCountChanged(maxTagCount_);
}

bool AdSelect::responsiveMaxTagCount() const { return responsiveMaxTagCount_; }

void AdSelect::setResponsiveMaxTagCount(bool value) {
  if (responsiveMaxTagCount_ == value) {
    return;
  }
  responsiveMaxTagCount_ = value;
  updateDisplay();
  emit responsiveMaxTagCountChanged(responsiveMaxTagCount_);
}

bool AdSelect::autoClearSearchValue() const { return autoClearSearchValue_; }

void AdSelect::setAutoClearSearchValue(bool value) {
  if (autoClearSearchValue_ == value) {
    return;
  }
  autoClearSearchValue_ = value;
  emit autoClearSearchValueChanged(autoClearSearchValue_);
}

AdSelect::Placement AdSelect::placement() const { return placement_; }

void AdSelect::setPlacement(Placement value) {
  if (placement_ == value) {
    return;
  }
  placement_ = value;
  if (open_) {
    syncPopupGeometry();
  }
  emit placementChanged(placement_);
}

AdSelect::PopupLayerMode AdSelect::popupLayerMode() const { return popupLayerMode_; }

void AdSelect::setPopupLayerMode(PopupLayerMode value) {
  if (popupLayerMode_ == value) {
    return;
  }

  const bool wasOpen = open_;
  if (wasOpen) {
    detail::setPopupInteractionHostOpen(this, false);
  }

  popupLayerMode_ = value;
  if (popup_) {
    QScopedValueRollback<bool> hideGuard(suppressPopupHideClose_, true);
    popup_->hide();
    applyPopupLayerMode();
  }

  emit popupLayerModeChanged(popupLayerMode_);

  if (wasOpen) {
    syncPopupGeometry();
    if (popup_) {
      popup_->show();
      popup_->raise();
    }
    detail::setPopupInteractionHostOpen(this, true);
  }
  syncTopLevelPopupTooltipRoute();
}

bool AdSelect::popupMatchSelectWidth() const { return popupMatchSelectWidth_; }

void AdSelect::setPopupMatchSelectWidth(bool value) {
  if (popupMatchSelectWidth_ == value) {
    return;
  }
  popupMatchSelectWidth_ = value;
  if (open_) {
    syncPopupGeometry();
  }
  emit popupMatchSelectWidthChanged(popupMatchSelectWidth_);
}

int AdSelect::popupWidth() const { return popupWidth_; }

void AdSelect::setPopupWidth(int value) {
  if (popupWidth_ == value) {
    return;
  }
  popupWidth_ = std::max(0, value);
  if (open_) {
    syncPopupGeometry();
  }
  emit popupWidthChanged(popupWidth_);
}

int AdSelect::modelColumn() const { return modelColumn_; }

void AdSelect::setModelColumn(int value) {
  const int normalized = std::max(0, value);
  if (modelColumn_ == normalized) {
    return;
  }

  const SelectionSnapshot previous = captureSelectionSnapshot();
  modelColumn_ = normalized;
  if (compositeModel_) {
    compositeModel_->setPrimaryColumn(modelColumn_);
  }
  syncModelBackedOptions(false);
  updateAccessibility();
  emit modelColumnChanged(modelColumn_);
  emitSelectionSignalsFromSnapshot(previous);
}

QString AdSelect::placeholder() const { return placeholder_; }

void AdSelect::setPlaceholder(const QString& value) {
  if (placeholder_ == value) {
    return;
  }
  placeholder_ = value;
  updateDisplay();
  updateAccessibility();
  emit placeholderChanged(placeholder_);
}

bool AdSelect::joinedLeft() const { return joinedLeft_; }

void AdSelect::setJoinedLeft(bool value) {
  if (joinedLeft_ == value) {
    return;
  }
  joinedLeft_ = value;
  bumpJoinedZOrder();
  updateInteractionFocusOverlay();
  update();
}

bool AdSelect::joinedRight() const { return joinedRight_; }

void AdSelect::setJoinedRight(bool value) {
  if (joinedRight_ == value) {
    return;
  }
  joinedRight_ = value;
  bumpJoinedZOrder();
  updateInteractionFocusOverlay();
  update();
}

QString AdSelect::prefixText() const { return prefixText_; }

void AdSelect::setPrefixText(const QString& value) {
  if (prefixText_ == value) {
    return;
  }
  prefixText_ = value;
  updatePrefixVisual();
  emit prefixTextChanged(prefixText_);
}

adqt::icons::IconRef AdSelect::prefixIconRef() const { return prefixIconRef_; }

void AdSelect::setPrefixIconRef(const adqt::icons::IconRef& token) {
  if (iconRefsEqual(prefixIconRef_, token)) {
    return;
  }
  prefixIconRef_ = token;
  updatePrefixVisual();
  emit prefixIconRefChanged(prefixIconRef_);
}

adqt::icons::IconRef AdSelect::suffixIconRef() const { return suffixIconRef_; }

void AdSelect::setSuffixIconRef(const adqt::icons::IconRef& token) {
  if (iconRefsEqual(suffixIconRef_, token)) {
    return;
  }
  suffixIconRef_ = token;
  updateInputMode();
  updateSuffixVisual();
  emit suffixIconRefChanged(suffixIconRef_);
}

adqt::icons::IconRef AdSelect::feedbackIconRef() const { return feedbackIconRef_; }

void AdSelect::setFeedbackIconRef(const adqt::icons::IconRef& token) {
  if (iconRefsEqual(feedbackIconRef_, token)) {
    return;
  }
  feedbackIconRef_ = token;
  updateAccessoryGeometry();
  updateLoadingSpinnerState();
  updateSuffixVisual();
  emit feedbackIconRefChanged(feedbackIconRef_);
}

QVariant AdSelect::currentValue() const { return currentValueCache_; }

void AdSelect::setCurrentValue(const QVariant& value) {
  setCurrentValues(value.isValid() && !value.isNull() ? QVariantList{value} : QVariantList{});
}

QVariantList AdSelect::currentValues() const { return currentValuesCache_; }

void AdSelect::setCurrentValues(const QVariantList& values) {
  const SelectionSnapshot previous = captureSelectionSnapshot();
  const QVariantList normalizedValues = normalizedSelectionValues(values);
  if (currentValues() == normalizedValues) {
    updateSelectionCaches();
    updateDisplay();
    return;
  }
  applySelectedValues(normalizedValues);
  updateSelectionCaches();
  refreshRows();
  updateDisplay();
  updateClearButton();
  emitSelectionSignalsFromSnapshot(previous);
}

QVector<AdSelect::SelectionItem> AdSelect::currentItems() const {
  QVector<SelectionItem> items;
  items.reserve(currentValueKeys_.size());
  for (const QString& current : currentValueKeys_) {
    SelectionItem item;
    item.value = rawValueForSelectionKey(current);
    item.label = fallbackSelectedLabel(item.value);
    items.append(item);
  }
  return items;
}

QModelIndex AdSelect::currentModelIndex() const {
  if (!selectionModel_) {
    return QModelIndex();
  }

  QModelIndex currentIndex = selectionModel_->currentIndex();
  if (!currentIndex.isValid()) {
    const QModelIndexList selectedIndexes = selectedModelIndexes();
    return selectedIndexes.isEmpty() ? QModelIndex() : selectedIndexes.constFirst();
  }
  if (currentIndex.model() != sourceModel_) {
    return QModelIndex();
  }

  const QModelIndex modelColumnIndex = currentIndex.sibling(currentIndex.row(), modelColumn_);
  return modelColumnIndex.isValid() ? modelColumnIndex : currentIndex;
}

QModelIndexList AdSelect::selectedModelIndexes() const {
  if (!selectionModel_ || (sourceModel_ && selectionModel_->model() != sourceModel_)) {
    return {};
  }

  QModelIndexList selectedRows = selectionModel_->selectedRows(modelColumn_);
  std::sort(selectedRows.begin(), selectedRows.end(),
            [](const QModelIndex& lhs, const QModelIndex& rhs) {
              if (lhs.row() == rhs.row()) {
                return lhs.column() < rhs.column();
              }
              return lhs.row() < rhs.row();
            });
  return selectedRows;
}

int AdSelect::currentIndex() const {
  const QModelIndex index = currentModelIndex();
  return index.isValid() ? index.row() : -1;
}

void AdSelect::setCurrentIndex(int index) {
  if (!sourceModel_ || index < 0 || index >= sourceModel_->rowCount()) {
    setCurrentValues({});
    return;
  }

  const QModelIndex modelIndex = sourceModel_->index(index, modelColumn_);
  if (!modelIndex.isValid()) {
    return;
  }

  const QVariant value = modelIndex.data(valueRole_);
  if (mode_ == Mode::Single) {
    setCurrentValue(value);
  } else {
    QVariantList nextValues = currentValues();
    if (!nextValues.contains(value)) {
      nextValues.append(value);
      setCurrentValues(nextValues);
    }
  }
}

QString AdSelect::currentText() const {
  const QModelIndex index = currentModelIndex();
  if (index.isValid()) {
    QString label = index.data(labelRole_).toString().trimmed();
    if (!label.isEmpty()) {
      return label;
    }
    return index.data(Qt::DisplayRole).toString().trimmed();
  }

  if (!currentValue().isValid() || currentValue().isNull()) {
    return QString();
  }
  return fallbackSelectedLabel(currentValue());
}

QVariant AdSelect::currentData(int role) const {
  const QModelIndex index = currentModelIndex();
  if (!index.isValid()) {
    return QVariant();
  }

  return index.data(role);
}

void AdSelect::setCurrentData(const QVariant& value, int role) {
  if (!sourceModel_) {
    return;
  }

  for (int row = 0; row < sourceModel_->rowCount(); ++row) {
    const QModelIndex index = sourceModel_->index(row, modelColumn_);
    if (!index.isValid()) {
      continue;
    }
    if (index.data(role) == value) {
      setCurrentIndex(row);
      return;
    }
  }
}

bool AdSelect::editable() const { return searchEnabled_; }

void AdSelect::setEditable(bool value) { setSearchEnabled(value); }

QLineEdit* AdSelect::lineEdit() const { return lineEdit_; }

QListView* AdSelect::view() const {
  auto* self = const_cast<AdSelect*>(this);
  self->ensurePopup();
  return listView_;
}

void AdSelect::showPopup() { openPopup(); }

void AdSelect::hidePopup() { closePopup(); }

QVector<AdSelect::Option> AdSelect::options() const { return options_; }

void AdSelect::setOptions(const QVector<Option>& options) {
  auto* model = new QStandardItemModel(this);
  for (const Option& option : options) {
    auto* item = new QStandardItem(option.label);
    item->setData(option.label, Qt::DisplayRole);
    item->setData(option.value, valueRole_);
    item->setData(option.label, labelRole_);
    item->setData(option.group, groupRole_);
    item->setData(option.metadata, DefaultMetadataRole);

    Qt::ItemFlags flags = item->flags();
    if (option.disabled) {
      flags &= ~Qt::ItemIsEnabled;
      flags &= ~Qt::ItemIsSelectable;
    }
    item->setFlags(flags);
    model->appendRow(item);
  }
  if (ownedModel_ && ownedModel_ != model && ownedModel_ != sourceModel_) {
    ownedModel_->deleteLater();
  }
  ownedModel_ = model;
  setModel(model);
}

void AdSelect::appendOption(const Option& option) {
  QVector<Option> next = options_;
  next.append(option);
  setOptions(next);
}

void AdSelect::clearOptions() {
  if (!sourceModel_ && options_.isEmpty()) {
    return;
  }
  const SelectionSnapshot previous = captureSelectionSnapshot();
  if (ownedModel_) {
    if (ownedModel_ != sourceModel_) {
      ownedModel_->deleteLater();
    }
    ownedModel_.clear();
  }
  clearOverlayModel();
  setModel(nullptr);
  options_.clear();
  updateSelectionCaches();
  refreshRows();
  updateDisplay();
  updateClearButton();
  emit optionsChanged();
  emitSelectionSignalsFromSnapshot(previous);
}

QAbstractItemModel* AdSelect::model() const { return sourceModel_.data(); }

void AdSelect::setModel(QAbstractItemModel* model) {
  if (sourceModel_ == model) {
    return;
  }
  const SelectionSnapshot previous = captureSelectionSnapshot();

  if (ownedModel_ && ownedModel_ == sourceModel_ && ownedModel_ != model) {
    ownedModel_->deleteLater();
    ownedModel_.clear();
  }

  if (sourceModel_) {
    disconnect(sourceModel_, nullptr, this, nullptr);
  }

  sourceModel_ = model;
  if (compositeModel_) {
    compositeModel_->setPrimaryModel(sourceModel_);
    compositeModel_->setPrimaryColumn(modelColumn_);
  }

  if (sourceModel_) {
    connect(sourceModel_, &QObject::destroyed, this, [this]() {
      const SelectionSnapshot previousSnapshot = captureSelectionSnapshot();
      sourceModel_.clear();
      if (compositeModel_) {
        compositeModel_->setPrimaryModel(nullptr);
      }
      if (selectionModel_ && (!sourceModel_ || selectionModel_->model() != sourceModel_)) {
        disconnect(selectionModel_, nullptr, this, nullptr);
        selectionModel_.clear();
        disposeOwnedSelectionModel(ownedSelectionModel_);
        emit selectionModelChanged(nullptr);
      }
      syncModelBackedOptions();
      updateSelectionCaches();
      refreshRows();
      updateDisplay();
      updateClearButton();
      emitSelectionSignalsFromSnapshot(previousSnapshot);
      emit modelChanged(nullptr);
    });
    connect(sourceModel_, &QAbstractItemModel::modelReset, this,
            [this]() { syncModelBackedOptions(); });
    connect(sourceModel_, &QAbstractItemModel::layoutChanged, this,
            [this](const QList<QPersistentModelIndex>&, QAbstractItemModel::LayoutChangeHint) {
              syncModelBackedOptions();
            });
    connect(sourceModel_, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int, int) { syncModelBackedOptions(); });
    connect(sourceModel_, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex&, int, int) { syncModelBackedOptions(); });
    connect(sourceModel_, &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex&, int, int, const QModelIndex&, int) {
              syncModelBackedOptions();
            });
    connect(sourceModel_,
            qOverload<const QModelIndex&, const QModelIndex&, const QList<int>&>(
                &QAbstractItemModel::dataChanged),
            this, [this](const QModelIndex&, const QModelIndex&, const QList<int>&) {
              syncModelBackedOptions();
            });
  }

  if (selectionModel_ && selectionModel_->model() != sourceModel_) {
    disconnect(selectionModel_, nullptr, this, nullptr);
    selectionModel_.clear();
    disposeOwnedSelectionModel(ownedSelectionModel_);
    emit selectionModelChanged(nullptr);
  }

  if (!selectionModel_ && sourceModel_) {
    auto* ownedSelection = new QItemSelectionModel(sourceModel_, this);
    ownedSelectionModel_ = ownedSelection;
    setSelectionModel(ownedSelection);
  } else if (selectionModel_) {
    syncSelectionStateFromSelectionModel();
  }

  updateRoleConfig();
  syncModelBackedOptions();
  updateSelectionCaches();
  refreshRows();
  updateDisplay();
  updateClearButton();
  updateAccessibility();
  emit modelChanged(sourceModel_.data());
  emitSelectionSignalsFromSnapshot(previous);
}

QItemSelectionModel* AdSelect::selectionModel() const { return selectionModel_.data(); }

void AdSelect::setSelectionModel(QItemSelectionModel* model) {
  if (selectionModel_ == model) {
    return;
  }

  const SelectionSnapshot previous = captureSelectionSnapshot();
  const QVariantList preservedValues = currentValues();

  if (model && sourceModel_ && model->model() != sourceModel_) {
    return;
  }

  if (selectionModel_) {
    disconnect(selectionModel_, nullptr, this, nullptr);
  }

  selectionModel_ = model;
  if (!selectionModel_ || selectionModel_ != ownedSelectionModel_) {
    disposeOwnedSelectionModel(ownedSelectionModel_);
  }

  bool selectionSignalsHandled = false;
  if (selectionModel_) {
    connect(selectionModel_, &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) {
              syncSelectionStateFromSelectionModel();
            });
    connect(
        selectionModel_, &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex&, const QModelIndex&) { syncSelectionStateFromSelectionModel(); });
    if (!selectionModel_->selectedRows(modelColumn_).isEmpty()) {
      syncSelectionStateFromSelectionModel();
      selectionSignalsHandled = true;
    } else {
      applySelectedValues(preservedValues, false);
      updateSelectionCaches();
      refreshRows();
      updateDisplay();
      updateClearButton();
    }
  } else {
    setCustomTagValues(preservedValues);
    syncSelectionKeysFromState();
    updateSelectionCaches();
    refreshRows();
    updateDisplay();
    updateClearButton();
  }

  updateAccessibility();
  emit selectionModelChanged(selectionModel_.data());
  if (!selectionSignalsHandled) {
    emitSelectionSignalsFromSnapshot(previous);
  }
}

QList<AdSelect::Item> AdSelect::items() const { return options_.toList(); }

void AdSelect::setItems(const QList<Item>& items) {
  QVector<Option> options;
  options.reserve(items.size());
  for (const Item& item : items) {
    options.append(item);
  }
  setOptions(options);
}

int AdSelect::valueRole() const { return valueRole_; }

void AdSelect::setValueRole(int role) {
  if (valueRole_ == role) {
    return;
  }
  valueRole_ = role;
  updateRoleConfig();
  syncModelBackedOptions(false);
}

int AdSelect::labelRole() const { return labelRole_; }

void AdSelect::setLabelRole(int role) {
  if (labelRole_ == role) {
    return;
  }
  labelRole_ = role;
  updateRoleConfig();
  syncModelBackedOptions(false);
}

int AdSelect::tagTextRole() const { return tagTextRole_; }

void AdSelect::setTagTextRole(int role) {
  if (tagTextRole_ == role) {
    return;
  }
  tagTextRole_ = role;
  updateRoleConfig();
  syncModelBackedOptions(false);
}

int AdSelect::selectedTextRole() const { return selectedTextRole_; }

void AdSelect::setSelectedTextRole(int role) {
  if (selectedTextRole_ == role) {
    return;
  }
  selectedTextRole_ = role;
  updateRoleConfig();
  syncModelBackedOptions(false);
}

int AdSelect::groupRole() const { return groupRole_; }

void AdSelect::setGroupRole(int role) {
  if (groupRole_ == role) {
    return;
  }
  groupRole_ = role;
  updateRoleConfig();
  syncModelBackedOptions(false);
}

AdSelect::RoleConfig AdSelect::roleConfig() const {
  RoleConfig config;
  config.valueRole = valueRole_;
  config.labelRole = labelRole_;
  config.tagTextRole = tagTextRole_;
  config.selectedTextRole = selectedTextRole_;
  config.groupRole = groupRole_;
  config.searchRoles = searchRoles_;
  return config;
}

void AdSelect::setRoleConfig(const RoleConfig& config) {
  const int nextValueRole = config.valueRole;
  const int nextLabelRole = config.labelRole;
  const int nextTagTextRole = config.tagTextRole;
  const int nextSelectedTextRole = config.selectedTextRole;
  const int nextGroupRole = config.groupRole;
  const QList<int> nextSearchRoles =
      config.searchRoles.isEmpty() ? QList<int>{nextLabelRole, nextValueRole} : config.searchRoles;

  if (valueRole_ == nextValueRole && labelRole_ == nextLabelRole &&
      tagTextRole_ == nextTagTextRole && selectedTextRole_ == nextSelectedTextRole &&
      groupRole_ == nextGroupRole && searchRoles_ == nextSearchRoles) {
    return;
  }

  valueRole_ = nextValueRole;
  labelRole_ = nextLabelRole;
  tagTextRole_ = nextTagTextRole;
  selectedTextRole_ = nextSelectedTextRole;
  groupRole_ = nextGroupRole;
  searchRoles_ = nextSearchRoles;
  if (!searchFilterFieldsExplicit_) {
    searchFilterFields_ = effectiveSearchFilterFields();
  }

  updateRoleConfig();
  syncModelBackedOptions(false);
}

QList<int> AdSelect::searchRoles() const { return searchRoles_; }

void AdSelect::setSearchRoles(const QList<int>& roles) {
  const QList<int> normalized = roles.isEmpty() ? QList<int>{labelRole_, valueRole_} : roles;
  if (searchRoles_ == normalized) {
    return;
  }
  searchRoles_ = normalized;
  if (!searchFilterFieldsExplicit_) {
    searchFilterFields_ = effectiveSearchFilterFields();
  }
  updateRoleConfig();
  syncModelBackedOptions();
  refreshRows();
}

QAbstractItemDelegate* AdSelect::itemDelegate() const { return itemDelegateOverride_.data(); }

void AdSelect::setItemDelegate(QAbstractItemDelegate* delegate) {
  if (itemDelegateOverride_ == delegate) {
    return;
  }
  itemDelegateOverride_ = delegate;
  if (listView_) {
    listView_->setItemDelegate(itemDelegateOverride_ ? itemDelegateOverride_.data()
                                                     : new OptionListDelegate(this));
  }
}

QWidget* AdSelect::popupFooterWidget() const { return popupFooterWidget_; }

void AdSelect::setPopupFooterWidget(QWidget* widget) {
  if (popupFooterWidget_ == widget) {
    return;
  }

  if (popupFooterWidget_) {
    if (popupLayout_) {
      popupLayout_->removeWidget(popupFooterWidget_);
    }
    if (popupExtraContent_ == popupFooterWidget_) {
      popupExtraContent_ = nullptr;
    }
    if (popupFooterWidget_->parentWidget() == popup_ ||
        popupFooterWidget_->parentWidget() == this) {
      popupFooterWidget_->setParent(this);
    }
    popupFooterWidget_->hide();
  }

  popupFooterWidget_ = widget;
  if (popupFooterWidget_) {
    // Keep custom popup content out of the selector area before the popup exists.
    popupFooterWidget_->setParent(this);
    popupFooterWidget_->hide();
  }

  if (popup_) {
    syncPopupExtraContentWidget();
    if (open_) {
      syncPopupGeometry();
    }
  }
}

void AdSelect::setSearchFilterFields(const QStringList& fields) {
  const QStringList normalized = uniqueStringList(fields);
  const bool explicitOverride = !normalized.isEmpty();
  if (searchFilterFields_ == normalized && searchFilterFieldsExplicit_ == explicitOverride) {
    return;
  }
  searchFilterFields_ = normalized;
  searchFilterFieldsExplicit_ = explicitOverride;
  updateRoleConfig();
  refreshRows();
}

QStringList AdSelect::searchFilterFields() const { return searchFilterFields_; }

void AdSelect::setFilterPredicate(FilterPredicate predicate) {
  filterPredicate_ = std::move(predicate);
  if (filterProxyModel_) {
    filterProxyModel_->setFilterPredicate(filterPredicate_);
  }
  refreshRows();
}

AdSelect::FilterPredicate AdSelect::filterPredicate() const { return filterPredicate_; }

void AdSelect::setSortComparator(SortComparator comparator) {
  sortComparator_ = std::move(comparator);
  if (filterProxyModel_) {
    filterProxyModel_->setSortComparator(sortComparator_);
  }
  refreshRows();
}

AdSelect::SortComparator AdSelect::sortComparator() const { return sortComparator_; }

void AdSelect::setTokenSeparators(const QStringList& separators) {
  tokenSeparators_ = uniqueStringList(separators);
}

QStringList AdSelect::tokenSeparators() const { return tokenSeparators_; }

void AdSelect::setOptionTextFormatter(OptionTextFormatter formatter) {
  optionTextFormatter_ = std::move(formatter);
  refreshRows();
  updateDisplay();
}

AdSelect::OptionTextFormatter AdSelect::optionTextFormatter() const { return optionTextFormatter_; }

void AdSelect::setTagTextFormatter(TagTextFormatter formatter) {
  tagTextFormatter_ = std::move(formatter);
  updateDisplay();
}

AdSelect::TagTextFormatter AdSelect::tagTextFormatter() const { return tagTextFormatter_; }

void AdSelect::setLabelFormatter(LabelFormatter formatter) {
  labelFormatter_ = std::move(formatter);
  updateDisplay();
}

AdSelect::LabelFormatter AdSelect::labelFormatter() const { return labelFormatter_; }

void AdSelect::setPopupExtraContentFactory(PopupExtraContentFactory factory) {
  popupExtraContentFactory_ = std::move(factory);
  if (popup_) {
    rebuildPopupExtraContent();
    if (popupIsVisible()) {
      syncPopupGeometry();
    }
  }
}

AdSelect::PopupExtraContentFactory AdSelect::popupExtraContentFactory() const {
  return popupExtraContentFactory_;
}

AdSelect::ComponentTokens AdSelect::componentTokens() const { return componentTokens_; }

void AdSelect::setComponentTokens(const ComponentTokens& tokens) {
  componentTokens_ = tokens;
  applyVisualStyle();
  emit componentTokensChanged();
}

void AdSelect::resetComponentTokens() {
  componentTokens_ = ComponentTokens();
  applyVisualStyle();
  emit componentTokensChanged();
}

AdSelect::SemanticStyles AdSelect::semanticStyles() const { return semanticStyles_; }

void AdSelect::setSemanticStyles(const SemanticStyles& styles) {
  semanticStyles_ = styles;
  applyVisualStyle();
  emit semanticStylesChanged();
}

void AdSelect::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  semanticStyleResolver_ = std::move(resolver);
  applyVisualStyle();
  emit semanticStylesChanged();
}

QSize AdSelect::sizeHint() const {
  int height = visualStyle_ ? visualStyle_->metrics.height : 32;
  if (mode_ != Mode::Single) {
    height = std::max(height, this->height());
  }
  return QSize(qMax(1, qRound(240 * controlScale_.logicalScale)),
               qMax(1, qRound(height * controlScale_.logicalScale)));
}

QSize AdSelect::minimumSizeHint() const {
  int height = visualStyle_ ? visualStyle_->metrics.height : 32;
  if (mode_ != Mode::Single) {
    height = std::max(height, this->height());
  }
  return QSize(qMax(1, qRound(120 * controlScale_.logicalScale)),
               qMax(1, qRound(height * controlScale_.logicalScale)));
}

void AdSelect::prepareControlScale(const AdControlScaleContext& context) { Q_UNUSED(context) }

void AdSelect::commitControlScale(const AdControlScaleContext& context) {
  if (!referenceFontCaptured_) {
    referenceFont_ = font();
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
  applyVisualStyle();
  updateDisplay();
  if (popupIsVisible()) {
    syncPopupGeometry();
  }
}

bool AdSelect::eventFilter(QObject* watched, QEvent* event) {
  if (!watched || !event) {
    return QWidget::eventFilter(watched, event);
  }

  if (watched == lineEdit_) {
    if (event->type() == QEvent::MouseButtonPress) {
      if (lineEdit_->isReadOnly()) {
        if (!disabled()) {
          if (open_) {
            closePopup();
          } else {
            openPopup();
          }
        }
        lineEdit_->deselect();
        return true;
      }
      if (!disabled()) {
        // Single-select should behave like a pure toggle target even when
        // search is enabled: clicking the embedded editor must not bubble up
        // and trigger a second toggle on the selector shell.
        if (mode_ == Mode::Single) {
          if (open_) {
            closePopup();
          } else {
            openPopup();
          }
          return true;
        }
        if (!open_) {
          openPopup();
        }
      }
    } else if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
      if (!disabled()) {
        hovered_ = true;
        updateClearButton();
        update();
      }
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
      if (!underMouse() && !(clearButton_ && clearButton_->underMouse())) {
        hovered_ = false;
      }
      if (!clearButton_ || !clearButton_->underMouse()) {
        clearHovered_ = false;
      }
      updateClearButton();
      update();
    } else if (event->type() == QEvent::MouseMove) {
      if (!disabled() && !hovered_) {
        hovered_ = true;
        updateClearButton();
        update();
      }
      if (lineEdit_->isReadOnly()) {
        return true;
      }
    } else if (event->type() == QEvent::MouseButtonRelease ||
               event->type() == QEvent::ContextMenu) {
      if (lineEdit_->isReadOnly()) {
        return true;
      }
    } else if (event->type() == QEvent::MouseButtonDblClick) {
      if (lineEdit_->isReadOnly()) {
        lineEdit_->deselect();
        return true;
      }
    } else if (event->type() == QEvent::FocusIn) {
      updateFocusState();
    } else if (event->type() == QEvent::FocusOut) {
      if (!open_) {
        updateFocusState();
      }
      if (mode_ != Mode::Single && !inputMethodPreeditText_.isEmpty()) {
        inputMethodPreeditText_.clear();
        updateDisplay();
      }
    } else if (event->type() == QEvent::InputMethod) {
      if (mode_ != Mode::Single) {
        const auto* inputMethodEvent = static_cast<const QInputMethodEvent*>(event);
        const QString preeditText =
            inputMethodEvent ? inputMethodEvent->preeditString() : QString();
        if (inputMethodPreeditText_ != preeditText) {
          inputMethodPreeditText_ = preeditText;
          updateDisplay();
        }
      }
    } else if (event->type() == QEvent::KeyPress) {
      auto* keyEvent = static_cast<QKeyEvent*>(event);
      if (lineEdit_->isReadOnly() && keyEvent->matches(QKeySequence::SelectAll)) {
        return true;
      }
      if (keyEvent->key() == Qt::Key_F4) {
        if (open_) {
          closePopup();
        } else {
          openPopup();
        }
        return true;
      }
      if (keyEvent->modifiers().testFlag(Qt::AltModifier) && keyEvent->key() == Qt::Key_Down) {
        if (!open_) {
          openPopup();
        }
        return true;
      } else if (keyEvent->modifiers().testFlag(Qt::AltModifier) && keyEvent->key() == Qt::Key_Up) {
        if (open_) {
          closePopup();
        }
        return true;
      } else if (keyEvent->key() == Qt::Key_Down) {
        if (!open_) {
          openPopup();
          return true;
        }
        moveCurrentListRow(1);
        return true;
      } else if (keyEvent->key() == Qt::Key_Up) {
        if (open_) {
          moveCurrentListRow(-1);
          return true;
        }
      } else if (keyEvent->key() == Qt::Key_PageDown) {
        if (open_) {
          moveCurrentListRow(1, true);
          return true;
        }
      } else if (keyEvent->key() == Qt::Key_PageUp) {
        if (open_) {
          moveCurrentListRow(-1, true);
          return true;
        }
      } else if (keyEvent->key() == Qt::Key_Home) {
        if (open_) {
          moveCurrentListRowToBoundary(false);
          return true;
        }
      } else if (keyEvent->key() == Qt::Key_End) {
        if (open_) {
          moveCurrentListRowToBoundary(true);
          return true;
        }
      } else if (keyEvent->key() == Qt::Key_Escape) {
        if (open_) {
          closePopup();
          return true;
        }
      } else if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
        if (mode_ == Mode::Tags) {
          const QString token = lineEdit_->text().trimmed();
          if (!token.isEmpty() && addTagValue(token)) {
            if (autoClearSearchValue_) {
              setSearchText(QString());
            }
            suppressLineEditChange_ = true;
            lineEdit_->clear();
            suppressLineEditChange_ = false;
            refreshRows();
            updateDisplay();
            return true;
          }
        }
        if (open_) {
          activateCurrentListRow();
          return true;
        }
      } else if (keyEvent->key() == Qt::Key_Backspace &&
                 (mode_ == Mode::Multiple || mode_ == Mode::Tags)) {
        if (lineEdit_->text().isEmpty() && !currentValueKeys_.isEmpty()) {
          const SelectionSnapshot previous = captureSelectionSnapshot();
          QVariantList nextValues = currentValues();
          if (nextValues.isEmpty()) {
            return true;
          }
          const QVariant removedRaw = nextValues.takeLast();
          applySelectedValues(nextValues, false);
          emit deselected(removedRaw, fallbackSelectedLabel(removedRaw));
          updateSelectionCaches();
          emitSelectionSignalsFromSnapshot(previous);
          refreshRows();
          updateDisplay();
          updateClearButton();
          return true;
        }
      }
    }
  } else if (watched == clearButton_) {
    if (event->type() == QEvent::Enter) {
      clearHovered_ = true;
      updateClearButton();
    } else if (event->type() == QEvent::Leave) {
      clearHovered_ = false;
      updateClearButton();
    }
  } else if (watched == popup_) {
    if (event->type() == QEvent::Hide) {
      if (open_ && !suppressPopupHideClose_) {
        setOpenInternal(false, true);
      }
    }
  } else if (watched == listView_) {
    if (event->type() == QEvent::KeyPress) {
      auto* keyEvent = static_cast<QKeyEvent*>(event);
      if (keyEvent->key() == Qt::Key_F4) {
        if (open_) {
          closePopup();
        } else {
          openPopup();
        }
        return true;
      }
      if (keyEvent->modifiers().testFlag(Qt::AltModifier) && keyEvent->key() == Qt::Key_Up) {
        closePopup();
        return true;
      }
      if (keyEvent->key() == Qt::Key_Escape) {
        closePopup();
        return true;
      }
      if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
        activateCurrentListRow();
        return true;
      }
      if (keyEvent->key() == Qt::Key_Space && mode_ != Mode::Single) {
        activateCurrentListRow();
        return true;
      }
    }
  } else if (listView_ && watched == listView_->viewport()) {
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease) {
      const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
      syncPopupOptionCursor(mouseEvent->pos());
    } else if (event->type() == QEvent::Enter) {
      syncPopupOptionCursor(listView_->viewport()->mapFromGlobal(QCursor::pos()));
    } else if (event->type() == QEvent::Leave) {
      syncPopupOptionCursor(QPoint(-1, -1));
    }
  }
  return QWidget::eventFilter(watched, event);
}

void AdSelect::paintEvent(QPaintEvent* event) {
  QWidget::paintEvent(event);
  QPainter painter(this);
  paintSelectorShell(painter);
}

QRectF AdSelect::selectorPaintRect() const {
  if (!visualStyle_) {
    return rect();
  }
  return joinedSelectorRect(rect(), visualStyle_->metrics.borderWidth, joinedLeft_, joinedRight_);
}

QColor AdSelect::resolveSelectorBgColor() const {
  if (!visualStyle_) {
    return QColor();
  }

  if (disabled()) {
    return visualStyle_->selectorBg;
  }
  if (hasFocusWithin_ || open_) {
    return visualStyle_->selectorActiveBg;
  }
  if (hovered_) {
    return visualStyle_->selectorHoverBg;
  }
  return visualStyle_->selectorBg;
}

QColor AdSelect::resolveSelectorBorderColor() const {
  if (!visualStyle_) {
    return QColor();
  }

  if (disabled()) {
    return visualStyle_->selectorBorderColor;
  }
  if (hasFocusWithin_ || open_) {
    return visualStyle_->selectorActiveBorderColor;
  }
  if (hovered_) {
    return visualStyle_->selectorHoverBorderColor;
  }
  return visualStyle_->selectorBorderColor;
}

qreal AdSelect::resolveSelectorRadius() const {
  if (!visualStyle_) {
    return 0.0;
  }
  if (variant_ == Variant::Underlined) {
    return 0.0;
  }
  return std::max<qreal>(0.0, visualStyle_->metrics.borderRadius);
}

void AdSelect::paintSelectorShell(QPainter& painter) const {
  if (!visualStyle_) {
    return;
  }

  const QRectF shellRect = selectorPaintRect();
  if (!shellRect.isValid() || shellRect.width() <= 0.0 || shellRect.height() <= 0.0) {
    return;
  }

  const QColor background = resolveSelectorBgColor();
  const QColor border = resolveSelectorBorderColor();
  const qreal borderWidth = std::max<qreal>(0.0, visualStyle_->metrics.borderWidth);
  const qreal radius = resolveSelectorRadius();
  const qreal topLeftRadius = joinedLeft_ ? 0.0 : radius;
  const qreal topRightRadius = joinedRight_ ? 0.0 : radius;
  const qreal bottomRightRadius = joinedRight_ ? 0.0 : radius;
  const qreal bottomLeftRadius = joinedLeft_ ? 0.0 : radius;

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);

  const QPainterPath shellPath = roundedRectPath(shellRect, topLeftRadius, topRightRadius,
                                                 bottomRightRadius, bottomLeftRadius);
  if (background.alpha() > 0) {
    painter.fillPath(shellPath, background);
  }

  if (variant_ == Variant::Underlined) {
    if (borderWidth > 0.0 && border.alpha() > 0) {
      QPen underlinePen(border, borderWidth, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
      painter.setPen(underlinePen);
      painter.setBrush(Qt::NoBrush);
      const qreal y = shellRect.bottom();
      painter.drawLine(QPointF(shellRect.left(), y), QPointF(shellRect.right(), y));
    }
  } else if (borderWidth > 0.0 && border.alpha() > 0) {
    QPen borderPen(border, borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    painter.setPen(borderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shellPath);
  }

  painter.restore();
}

void AdSelect::enterEvent(QEnterEvent* event) {
  QWidget::enterEvent(event);
  hovered_ = true;
  bumpJoinedZOrder();
  updateClearButton();
  update();
}

void AdSelect::leaveEvent(QEvent* event) {
  QWidget::leaveEvent(event);
  hovered_ = false;
  if (!clearButton_ || !clearButton_->underMouse()) {
    clearHovered_ = false;
  }
  updateClearButton();
  update();
}

void AdSelect::mousePressEvent(QMouseEvent* event) {
  if (!disabled() && event && event->button() == Qt::LeftButton) {
    if (clearButton_ && clearButton_->geometry().contains(event->pos())) {
      QWidget::mousePressEvent(event);
      return;
    }
    const bool readOnlyDisplay = lineEdit_ && lineEdit_->isReadOnly();
    if (readOnlyDisplay) {
      if (open_) {
        closePopup();
      } else {
        openPopup();
      }
    } else {
      // Keep multiple/tags editable interaction unchanged, but in single mode
      // clicking the selector while open should close the popup.
      if (mode_ == Mode::Single && open_) {
        closePopup();
      } else if (!open_) {
        openPopup();
      }
    }
  }
  QWidget::mousePressEvent(event);
}

void AdSelect::keyPressEvent(QKeyEvent* event) {
  if (!event || disabled()) {
    QWidget::keyPressEvent(event);
    return;
  }

  if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Return ||
      event->key() == Qt::Key_Enter) {
    if (!open_) {
      openPopup();
      return;
    }
  } else if (event->key() == Qt::Key_Escape) {
    if (open_) {
      closePopup();
      return;
    }
  }

  QWidget::keyPressEvent(event);
}

void AdSelect::moveEvent(QMoveEvent* event) {
  QWidget::moveEvent(event);
  updateInteractionFocusOverlay();
}

void AdSelect::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (responsiveMaxTagCount_ || mode_ != Mode::Single) {
    updateDisplay();
  }
  if (open_) {
    syncPopupGeometry();
  }
  updateAccessoryGeometry();
  updateInteractionFocusOverlay();
}

void AdSelect::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }
  if (event->type() == QEvent::Hide) {
    detail::cancelTimingTask(this, QString::fromLatin1(kShowLayoutRefreshKey));
    hovered_ = false;
    clearHovered_ = false;
    updateClearButton();
    stopInteractionFocusForOwner(this);
    return;
  }
  if (event->type() == QEvent::Show) {
    updateInteractionFocusOverlay();
    if (mode_ != Mode::Single || responsiveMaxTagCount_) {
      detail::deferTimingTask(this, QString::fromLatin1(kShowLayoutRefreshKey), [this]() {
        if (!isVisible()) {
          return;
        }
        updateDisplay();
        updateClearButton();
        updateAccessoryGeometry();
      });
    }
    return;
  }
  if (event->type() == QEvent::LanguageChange) {
    updateInputMode();
    refreshRows();
    updateDisplay();
    updateClearButton();
    updateSuffixVisual();
    update();
  } else if (event->type() == QEvent::EnabledChange || event->type() == QEvent::PaletteChange ||
      event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::FontChange ||
      event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::StyleChange) {
    if (event->type() == QEvent::EnabledChange && disabled()) {
      hovered_ = false;
    }
    updateInputMode();
    applyVisualStyle();
    updateDisplay();
    updateClearButton();
    updateSuffixVisual();
    update();
  }
}

void AdSelect::syncModelBackedOptions(bool preserveCurrentValues) {
  const SelectionSnapshot previous = captureSelectionSnapshot();
  const QVariantList preservedValues = currentValues();
  const QVariantList preservedCustomValues = customTagValues_;
  if (!searchFilterFieldsExplicit_) {
    searchFilterFields_ = effectiveSearchFilterFields();
  }
  updateRoleConfig();

  QVector<Option> nextOptions;
  QAbstractItemModel* effectiveModel = compositeModel_
                                           ? static_cast<QAbstractItemModel*>(compositeModel_.get())
                                           : sourceModel_.data();
  const int rowCount = effectiveModel ? effectiveModel->rowCount() : 0;
  nextOptions.reserve(rowCount);

  detail::SelectRoleConfig roles;
  roles.valueRole = valueRole_;
  roles.labelRole = labelRole_;
  roles.tagTextRole = tagTextRole_;
  roles.selectedTextRole = selectedTextRole_;
  roles.groupRole = groupRole_;

  for (int row = 0; row < rowCount; ++row) {
    const QModelIndex index = effectiveModel->index(row, 0);
    if (!index.isValid()) {
      continue;
    }
    nextOptions.append(detail::materializeSelectOption(index, roles));
  }

  options_ = nextOptions;
  if (preserveCurrentValues) {
    applySelectedValues(preservedValues, false);
  } else {
    customTagValues_ = preservedCustomValues;
    setCustomTagValues(customTagValues_);
    syncSelectionKeysFromState();
  }
  updateSelectionCaches();
  refreshRows();
  updateDisplay();
  updateClearButton();
  emit optionsChanged();
  emitSelectionSignalsFromSnapshot(previous);
}

AdSelect::Option AdSelect::optionFromRow(int row) const {
  if (row < 0 || row >= rows_.size()) {
    return {};
  }
  const ModelRow& modelRow = rows_.at(row);
  if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
    return {};
  }
  return options_.at(modelRow.optionIndex);
}

QString AdSelect::modelLabelForValue(const QVariant& value) const {
  const QString key = detail::selectValueKey(value);
  if (selectedLabelCache_.contains(key)) {
    return selectedLabelCache_.value(key);
  }
  const Option* option = findOption(value);
  return option ? optionLabelOrFallback(*option) : value.toString().trimmed();
}

void AdSelect::updateSelectionCaches() {
  const auto previousLabels = selectedLabelCache_;
  const auto previousTagTexts = selectedTagTextCache_;
  const auto previousDisplayTexts = selectedDisplayTextCache_;
  const auto previousCurrentValues = currentValuesCache_;
  const QVariantList selectedValues = effectiveSelectedValues();

  selectedLabelCache_.clear();
  selectedTagTextCache_.clear();
  selectedDisplayTextCache_.clear();

  currentValuesCache_.clear();
  currentValuesCache_.reserve(selectedValues.size());

  QHash<QString, QVariant> previousValueMap;
  previousValueMap.reserve(previousCurrentValues.size());
  for (const QVariant& previousValue : previousCurrentValues) {
    const QString key = detail::selectValueKey(previousValue);
    if (!key.isEmpty()) {
      previousValueMap.insert(key, previousValue);
    }
  }

  for (const QVariant& selectedValue : selectedValues) {
    const QString current = detail::selectValueKey(selectedValue);
    if (current.isEmpty()) {
      continue;
    }

    QVariant rawValue = previousValueMap.value(current, selectedValue);
    QString label = previousLabels.value(current, rawValue.toString().trimmed());
    QString tagText = previousTagTexts.value(current, label);
    QString displayText = previousDisplayTexts.value(current, label);

    if (const Option* option = findOption(rawValue)) {
      label = optionLabelOrFallback(*option);

      const QVariant candidateRaw =
          option->metadata.value(QString::fromLatin1(kValueVariantMetadataKey));
      if (candidateRaw.isValid()) {
        rawValue = candidateRaw;
      } else {
        rawValue = option->value;
      }

      const QString candidateTag =
          option->metadata.value(QString::fromLatin1(kTagTextMetadataKey)).toString().trimmed();
      tagText = candidateTag.isEmpty() ? (tagTextFormatter_ ? tagTextFormatter_(*option) : label)
                                       : candidateTag;

      const QString candidateDisplay =
          option->metadata.value(QString::fromLatin1(kSelectedTextMetadataKey))
              .toString()
              .trimmed();
      displayText = candidateDisplay.isEmpty()
                        ? (labelFormatter_ ? labelFormatter_(*option) : label)
                        : candidateDisplay;
    }

    selectedLabelCache_.insert(current, label);
    selectedTagTextCache_.insert(current, tagText.isEmpty() ? label : tagText);
    selectedDisplayTextCache_.insert(current, displayText.isEmpty() ? label : displayText);
    currentValuesCache_.append(rawValue);
  }

  currentValueCache_ =
      currentValuesCache_.isEmpty() ? QVariant() : currentValuesCache_.constFirst();
}

void AdSelect::syncPopupExtraContentWidget() {
  if (!popup_ || !popupLayout_) {
    return;
  }

  QWidget* nextContent = popupFooterWidget_;
  if (!nextContent && popupExtraContentFactory_) {
    nextContent = popupExtraContentFactory_(popup_);
  }

  if (popupExtraContent_ == nextContent) {
    if (popupExtraContent_) {
      if (popupExtraContent_->parentWidget() != popup_) {
        popupExtraContent_->setParent(popup_);
      }
      if (popupLayout_->indexOf(popupExtraContent_) < 0) {
        popupLayout_->addWidget(popupExtraContent_);
      }
      popupExtraContent_->show();
    }
    return;
  }

  QWidget* previous = popupExtraContent_;
  popupExtraContent_ = nullptr;

  if (previous) {
    popupLayout_->removeWidget(previous);
    if (previous == popupFooterWidget_) {
      previous->hide();
      previous->setParent(nullptr);
    } else {
      previous->deleteLater();
    }
  }

  if (!nextContent) {
    return;
  }

  popupExtraContent_ = nextContent;
  popupExtraContent_->setParent(popup_);
  popupLayout_->addWidget(popupExtraContent_);
  popupExtraContent_->show();
}

bool AdSelect::isSearchEnabledForCurrentMode() const { return searchEnabled_; }

bool AdSelect::isValueSelected(const QVariant& value) const {
  const QString key = detail::selectValueKey(value);
  if (mode_ == Mode::Single) {
    return !currentValueKey_.isEmpty() && currentValueKey_ == key;
  }
  return currentValueKeys_.contains(key);
}

int AdSelect::indexOfValue(const QVariant& value) const {
  const QString key = detail::selectValueKey(value);
  for (int i = 0; i < options_.size(); ++i) {
    if (detail::selectValueKey(options_.at(i).value) == key) {
      return i;
    }
  }
  return -1;
}

const AdSelect::Option* AdSelect::findOption(const QVariant& value) const {
  const int index = indexOfValue(value);
  if (index < 0 || index >= options_.size()) {
    return nullptr;
  }
  return &options_.at(index);
}

AdSelect::Option* AdSelect::findOption(const QVariant& value) {
  const int index = indexOfValue(value);
  if (index < 0 || index >= options_.size()) {
    return nullptr;
  }
  return &options_[index];
}

QString AdSelect::optionLabelOrFallback(const Option& option) const {
  const QString trimmed = option.label.trimmed();
  return trimmed.isEmpty() ? option.value.toString().trimmed() : trimmed;
}

QString AdSelect::formattedOptionText(const Option& option) const {
  if (optionTextFormatter_) {
    return optionTextFormatter_(option);
  }
  return optionLabelOrFallback(option);
}

QString AdSelect::formattedTagText(const Option& option) const {
  if (tagTextFormatter_) {
    return tagTextFormatter_(option);
  }
  QString tagText =
      option.metadata.value(QString::fromLatin1(kTagTextMetadataKey)).toString().trimmed();
  if (!tagText.isEmpty()) {
    return tagText;
  }
  return optionLabelOrFallback(option);
}

QString AdSelect::formattedSelectedLabel(const Option& option) const {
  if (labelFormatter_) {
    return labelFormatter_(option);
  }
  QString selectedText =
      option.metadata.value(QString::fromLatin1(kSelectedTextMetadataKey)).toString().trimmed();
  if (!selectedText.isEmpty()) {
    return selectedText;
  }
  return optionLabelOrFallback(option);
}

QString AdSelect::fallbackSelectedLabel(const QVariant& value) const {
  const QString key = detail::selectValueKey(value);
  const Option* option = findOption(value);
  if (!option) {
    if (mode_ == Mode::Multiple || mode_ == Mode::Tags) {
      return selectedTagTextCache_.value(
          key, selectedLabelCache_.value(key, value.toString().trimmed()));
    }
    return selectedDisplayTextCache_.value(
        key, selectedLabelCache_.value(key, value.toString().trimmed()));
  }
  if (mode_ == Mode::Multiple || mode_ == Mode::Tags) {
    return formattedTagText(*option);
  }
  return formattedSelectedLabel(*option);
}

QVariant AdSelect::rawValueForSelectionKey(const QString& value) const {
  const QVariantList selectedValues = effectiveSelectedValues();
  for (const QVariant& candidate : selectedValues) {
    if (detail::selectValueKey(candidate) == value) {
      return candidate;
    }
  }

  const int selectedIndex = static_cast<int>(currentValueKeys_.indexOf(value));
  if (selectedIndex >= 0 && selectedIndex < currentValuesCache_.size()) {
    const QVariant cached = currentValuesCache_.at(selectedIndex);
    if (cached.isValid()) {
      return cached;
    }
  }

  for (const Option& option : options_) {
    if (detail::selectValueKey(option.value) != value) {
      continue;
    }
    QVariant candidateRaw = option.metadata.value(QString::fromLatin1(kValueVariantMetadataKey));
    if (candidateRaw.isValid()) {
      return candidateRaw;
    }
    return option.value;
  }

  return value;
}

int AdSelect::responsiveVisibleTagCount(const QStringList& labels, int availableWidth) const {
  if (labels.isEmpty() || availableWidth <= 0) {
    return 0;
  }

  const QFontMetrics fm(tagsContainer_ ? tagsContainer_->font() : font());
  const int labelCount = static_cast<int>(labels.size());
  const int tagPaddingStart =
      std::max(4, visualStyle_ ? visualStyle_->metrics.tagPaddingInlineStart : 8);
  const int tagPaddingEnd =
      std::max(2, visualStyle_ ? visualStyle_->metrics.tagPaddingInlineEnd : 4);
  const int tagInnerGap = std::max(2, visualStyle_ ? visualStyle_->metrics.tagContentGap : 4);
  const int removeIconWidth = std::max(8, visualStyle_ ? (visualStyle_->metrics.iconSize - 2) : 10);
  const int interTagGap = std::max(2, visualStyle_ ? visualStyle_->metrics.tagItemGap : 4);

  const auto selectedTagWidth = [&](const QString& text) {
    return fm.horizontalAdvance(text) + tagPaddingStart + tagPaddingEnd + removeIconWidth +
           tagInnerGap;
  };
  const auto restTagWidth = [&](int hiddenCount) {
    if (hiddenCount <= 0) {
      return 0;
    }
    const QString restText = QStringLiteral("+%1...").arg(hiddenCount);
    return fm.horizontalAdvance(restText) + tagPaddingStart + tagPaddingEnd;
  };

  QVector<int> prefixWidths(labels.size() + 1, 0);
  for (int i = 0; i < labels.size(); ++i) {
    const int gap = i > 0 ? interTagGap : 0;
    prefixWidths[i + 1] = prefixWidths[i] + gap + selectedTagWidth(labels.at(i));
  }

  int bestVisibleCount = 0;
  for (int candidate = 0; candidate <= labelCount; ++candidate) {
    const int hiddenCount = labelCount - candidate;
    int totalWidth = prefixWidths[candidate];
    if (hiddenCount > 0) {
      if (candidate > 0) {
        totalWidth += interTagGap;
      }
      totalWidth += restTagWidth(hiddenCount);
    }
    if (totalWidth <= availableWidth) {
      bestVisibleCount = candidate;
    }
  }

  return bestVisibleCount;
}

void AdSelect::clearTagWidgets() {
  if (!tagsLayout_) {
    return;
  }

  while (QLayoutItem* item = tagsLayout_->takeAt(0)) {
    if (QWidget* widget = item->widget()) {
      if (widget == lineEdit_) {
        delete item;
        continue;
      }
      delete widget;
    }
    delete item;
  }
}

void AdSelect::rebuildTagWidgets() {
  if (!tagsContainer_ || !tagsLayout_) {
    return;
  }

  clearTagWidgets();

  if (mode_ == Mode::Single) {
    tagsContainer_->setVisible(false);
    tagsContainer_->setToolTip(QString());
    return;
  }

  QStringList labels;
  labels.reserve(currentValueKeys_.size());
  for (const QString& current : currentValueKeys_) {
    labels.append(fallbackSelectedLabel(rawValueForSelectionKey(current)));
  }

  int visibleCount = static_cast<int>(labels.size());
  if (maxTagCount_ >= 0) {
    visibleCount = std::min(visibleCount, maxTagCount_);
  }
  if (responsiveMaxTagCount_) {
    int availableWidth = contentHost_ ? contentHost_->width() : width();
    const int reservedInputWidth =
        std::max(24, (visualStyle_ ? visualStyle_->metrics.tagHeight : 20) + 6);
    availableWidth = std::max(0, availableWidth - reservedInputWidth);
    visibleCount = std::min(visibleCount, responsiveVisibleTagCount(labels, availableWidth));
  }
  visibleCount = std::clamp(visibleCount, 0, static_cast<int>(labels.size()));
  const int hiddenCount = static_cast<int>(labels.size()) - visibleCount;
  const bool hasPrefix = prefixLabel_ && prefixLabel_->isVisible();
  int contentWidth = tagsContainer_->contentsRect().width();
  if (contentHost_) {
    contentWidth = std::max(contentWidth, contentHost_->contentsRect().width());
  }
  if (contentWidth <= 0) {
    contentWidth = contentsRect().width();
  }
  if (contentWidth <= 0) {
    contentWidth = std::max(1, width());
  }
  constexpr int kFixedInputMinWidth = 4;
  const int maxTagItemWidth = std::max(kFixedInputMinWidth, contentWidth - kFixedInputMinWidth);

  const int tagHeight = std::max(16, visualStyle_ ? visualStyle_->metrics.tagHeight : 20);
  const int tagRadius = std::max(0, visualStyle_ ? visualStyle_->metrics.tagBorderRadius : 4);
  const int tagBorderWidth = std::max(0, visualStyle_ ? visualStyle_->metrics.borderWidth : 1);
  const int tagItemMargin = std::max(0, visualStyle_ ? visualStyle_->metrics.tagItemMargin : 2);
  const int tagOuterHeight = std::max(1, tagHeight + tagItemMargin * 2);
  const int tagPaddingStart =
      std::max(4, visualStyle_ ? visualStyle_->metrics.tagPaddingInlineStart : 8);
  const int tagPaddingEnd =
      std::max(2, visualStyle_ ? visualStyle_->metrics.tagPaddingInlineEnd : 4);
  const int tagGap = std::max(2, visualStyle_ ? visualStyle_->metrics.tagContentGap : 4);
  const int removeIconSize = std::max(8, visualStyle_ ? (visualStyle_->metrics.iconSize - 2) : 10);
  const QColor tagBg = visualStyle_ ? visualStyle_->tagBg : QColor("#f5f5f5");
  const QColor tagBorderColor = visualStyle_ ? visualStyle_->tagBorderColor : QColor(0, 0, 0, 0);
  const QColor tagTextColor = visualStyle_ ? visualStyle_->tagTextColor : QColor("#141414");
  const QColor disabledTextColor =
      visualStyle_ ? visualStyle_->disabledTextColor : QColor("#bfbfbf");
  const QColor removeColor = visualStyle_ ? visualStyle_->clearColor : QColor("#8c8c8c");
  const QColor removeHoverColor = visualStyle_ ? visualStyle_->clearHoverColor : QColor("#595959");

  const auto buildTag = [this, tagHeight, tagOuterHeight, tagRadius, tagBorderWidth, tagItemMargin,
                         tagPaddingStart, tagPaddingEnd, tagGap, removeIconSize, maxTagItemWidth,
                         tagBg, tagBorderColor, tagTextColor, disabledTextColor, removeColor,
                         removeHoverColor](const QString& text, const QString& value,
                                           bool removable) {
    auto* chip = new TagChipWidget(tagsContainer_);
    chip->setObjectName(QStringLiteral("adselect-tag-item"));
    chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    setWidgetFixedHeightIfChanged(chip, tagOuterHeight);
    chip->setMaximumWidth(maxTagItemWidth);

    auto* chipLayout = new QHBoxLayout(chip);
    chipLayout->setContentsMargins(tagPaddingStart, tagItemMargin, tagPaddingEnd, tagItemMargin);
    chipLayout->setSpacing(tagGap);

    const bool showRemoveButton = removable && !disabled() && !value.isEmpty();
    const int removeSlotWidth = showRemoveButton ? (removeIconSize + tagGap) : 0;
    const int textAvailableWidth =
        std::max(8, maxTagItemWidth - tagPaddingStart - tagPaddingEnd - removeSlotWidth);
    auto* label = new QLabel(chip);
    label->setObjectName(QStringLiteral("adselect-tag-text"));
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    label->setMinimumWidth(1);
    label->setMaximumWidth(textAvailableWidth);
    label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    const QFontMetrics tagLabelMetrics(label->font());
    const QString displayText =
        tagLabelMetrics.elidedText(text, Qt::ElideRight, textAvailableWidth);
    label->setText(displayText);
    label->setToolTip(displayText != text ? text : QString());
    setWidgetFixedHeightIfChanged(label, std::max(1, tagHeight - tagBorderWidth * 2));
    chipLayout->addWidget(label);

    QPalette chipPalette = chip->palette();
    chipPalette.setColor(QPalette::Window, tagBg);
    chipPalette.setColor(QPalette::WindowText, tagTextColor);
    chipPalette.setColor(QPalette::Disabled, QPalette::WindowText, disabledTextColor);
    chip->setPalette(chipPalette);

    QPalette labelPalette = label->palette();
    labelPalette.setColor(QPalette::WindowText, tagTextColor);
    labelPalette.setColor(QPalette::Disabled, QPalette::WindowText, disabledTextColor);
    label->setPalette(labelPalette);

    chip->setVisualStyle(tagBg, tagBorderColor, tagRadius, tagBorderWidth, tagItemMargin);

    setWidgetCursorIfChanged(chip, selectorCursorShape());
    setWidgetCursorIfChanged(label, selectorCursorShape());

    if (showRemoveButton) {
      auto* removeButton = new FlatIconToolButton(chip);
      removeButton->setObjectName(QStringLiteral("adselect-tag-remove"));
      removeButton->setText(QString());
      removeButton->setFixedSize(removeIconSize, removeIconSize);
      removeButton->setIconSize(QSize(removeIconSize, removeIconSize));
      removeButton->setCursor(Qt::PointingHandCursor);

      adqt::icons::IconRef closeIcon = outlined_icons::Close();
      if (adqt::icons::isValid(closeIcon)) {
        closeIcon = closeIcon.withColors(adqt::icons::IconColors::primary(removeColor));
        const qreal dpr = devicePixelRatioF();
        QPixmap closePixmap = adqt::icons::renderIconPixmap(
            closeIcon, {QSize(removeIconSize, removeIconSize), dpr, QIcon::Normal, QIcon::Off});
        QIcon closeButtonIcon;
        if (!closePixmap.isNull()) {
          closeButtonIcon.addPixmap(closePixmap, QIcon::Normal, QIcon::Off);
        }
        adqt::icons::IconRef hoverIcon = closeIcon;
        hoverIcon = hoverIcon.withColors(adqt::icons::IconColors::primary(removeHoverColor));
        QPixmap closeHoverPixmap = adqt::icons::renderIconPixmap(
            hoverIcon, {QSize(removeIconSize, removeIconSize), dpr, QIcon::Active, QIcon::Off});
        if (!closeHoverPixmap.isNull()) {
          closeButtonIcon.addPixmap(closeHoverPixmap, QIcon::Active, QIcon::Off);
        }
        if (!closeButtonIcon.isNull()) {
          removeButton->setIcon(closeButtonIcon);
        }
      } else {
        removeButton->setText(QStringLiteral("x"));
      }

      connect(removeButton, &QToolButton::clicked, this, [this, value]() {
        if (value.isEmpty() || disabled()) {
          return;
        }
        const SelectionSnapshot previous = captureSelectionSnapshot();
        const bool shouldRestoreInputFocus = open_ && lineEdit_ && !lineEdit_->isReadOnly();
        const int index = static_cast<int>(currentValueKeys_.indexOf(value));
        if (index < 0) {
          return;
        }
        const QVariant removedRaw = rawValueForSelectionKey(value);
        QVariantList nextValues = currentValues();
        if (index < nextValues.size()) {
          nextValues.removeAt(index);
        } else {
          nextValues.removeAll(removedRaw);
        }
        applySelectedValues(nextValues, false);
        emit deselected(removedRaw, fallbackSelectedLabel(removedRaw));
        updateSelectionCaches();
        emitSelectionSignalsFromSnapshot(previous);
        refreshRows();
        updateDisplay();
        updateClearButton();
        if (shouldRestoreInputFocus && lineEdit_ && !lineEdit_->hasFocus()) {
          lineEdit_->setFocus(Qt::MouseFocusReason);
        }
      });
      chipLayout->addWidget(removeButton, 0, Qt::AlignVCenter);
    }

    tagsLayout_->addWidget(chip);
  };

  bool hasRenderedTag = false;
  for (int i = 0; i < visibleCount && i < labels.size() && i < currentValueKeys_.size(); ++i) {
    buildTag(labels.at(i), currentValueKeys_.at(i), true);
    hasRenderedTag = true;
  }
  if (hiddenCount > 0) {
    buildTag(QStringLiteral("+%1...").arg(hiddenCount), QString(), false);
    hasRenderedTag = true;
  }

  if (lineEdit_ && lineEdit_->parentWidget() == tagsContainer_) {
    if (!hasPrefix && !hasRenderedTag && visualStyle_) {
      auto* spacer = new QWidget(tagsContainer_);
      spacer->setObjectName(QStringLiteral("adselect-tag-leading-spacer"));
      spacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
      spacer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      const int leadingInset = std::max(0, visualStyle_->metrics.multipleItemPaddingHorizontal);
      const int itemGap = std::max(0, tagsLayout_->spacing());
      spacer->setFixedWidth(std::max(0, leadingInset - itemGap));
      spacer->setFixedHeight(std::max(1, tagOuterHeight));
      tagsLayout_->addWidget(spacer);
    }
    tagsLayout_->addWidget(lineEdit_);
  }

  tagsContainer_->setVisible(true);
  tagsContainer_->setToolTip(labels.join(QStringLiteral(", ")));
}

bool AdSelect::suffixButtonTriggersPopup() const {
  if (disabled()) {
    return false;
  }
  // Keep Ant Design behavior: custom suffix icons are decorative by default and
  // should not change popup visibility.
  return !adqt::icons::isValid(suffixIconRef_);
}

Qt::CursorShape AdSelect::selectorCursorShape() const {
  if (disabled()) {
    return Qt::ForbiddenCursor;
  }
  return isSearchEnabledForCurrentMode() ? Qt::IBeamCursor : Qt::PointingHandCursor;
}

Qt::CursorShape AdSelect::optionCursorShapeAtRow(int row) const {
  if (disabled()) {
    return Qt::ForbiddenCursor;
  }
  if (row < 0 || row >= rows_.size()) {
    return Qt::ArrowCursor;
  }

  const ModelRow& modelRow = rows_.at(row);
  if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
    return Qt::ArrowCursor;
  }
  return options_.at(modelRow.optionIndex).disabled ? Qt::ForbiddenCursor : Qt::PointingHandCursor;
}

int AdSelect::nextSelectableRow(int startRow, int step) const {
  if (rows_.isEmpty()) {
    return -1;
  }

  const int direction = step >= 0 ? 1 : -1;
  for (int row = startRow; row >= 0 && row < rows_.size(); row += direction) {
    const ModelRow& modelRow = rows_.at(row);
    if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
      continue;
    }
    if (!options_.at(modelRow.optionIndex).disabled) {
      return row;
    }
  }
  return -1;
}

void AdSelect::moveCurrentListRow(int step, bool pageStep) {
  if (!listView_ || rows_.isEmpty()) {
    return;
  }

  int moveStep = step;
  if (pageStep && visualStyle_ && visualStyle_->metrics.optionHeight > 0) {
    const int viewportHeight =
        popupScrollArea_
            ? popupScrollArea_->height()
            : (listView_->viewport() ? listView_->viewport()->height() : listView_->height());
    const int visibleRows =
        std::max(1, viewportHeight / std::max(1, visualStyle_->metrics.optionHeight));
    moveStep *= visibleRows;
  }

  const QModelIndex currentIndex = listView_->currentIndex();
  int targetRow = -1;
  if (currentIndex.isValid()) {
    targetRow = nextSelectableRow(currentIndex.row() + moveStep, moveStep);
  } else {
    targetRow = nextSelectableRow(moveStep > 0 ? 0 : static_cast<int>(rows_.size()) - 1, moveStep);
  }
  if (targetRow < 0) {
    return;
  }

  const QModelIndex targetIndex = listModel_ ? listModel_->index(targetRow, 0) : QModelIndex();
  if (!targetIndex.isValid()) {
    return;
  }

  listView_->setCurrentIndex(targetIndex);
  if (mode_ == Mode::Single && listView_->selectionModel()) {
    listView_->selectionModel()->select(targetIndex, QItemSelectionModel::ClearAndSelect);
  }
  listView_->scrollTo(targetIndex, QAbstractItemView::PositionAtCenter);
}

void AdSelect::moveCurrentListRowToBoundary(bool toEnd) {
  if (!listView_ || rows_.isEmpty()) {
    return;
  }

  const int targetRow =
      nextSelectableRow(toEnd ? static_cast<int>(rows_.size()) - 1 : 0, toEnd ? -1 : 1);
  if (targetRow < 0) {
    return;
  }

  const QModelIndex targetIndex = listModel_ ? listModel_->index(targetRow, 0) : QModelIndex();
  if (!targetIndex.isValid()) {
    return;
  }

  listView_->setCurrentIndex(targetIndex);
  if (mode_ == Mode::Single && listView_->selectionModel()) {
    listView_->selectionModel()->select(targetIndex, QItemSelectionModel::ClearAndSelect);
  }
  listView_->scrollTo(
      targetIndex, toEnd ? QAbstractItemView::PositionAtBottom : QAbstractItemView::PositionAtTop);
}

void AdSelect::activateCurrentListRow() {
  if (!listView_) {
    return;
  }
  const QModelIndex currentIndex = listView_->currentIndex();
  if (!currentIndex.isValid()) {
    return;
  }

  const int row = currentIndex.row();
  if (row < 0 || row >= rows_.size()) {
    return;
  }
  const ModelRow& modelRow = rows_.at(row);
  if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
    return;
  }
  toggleSelectionForOption(options_.at(modelRow.optionIndex));
}

void AdSelect::syncPopupSelectionState() {
  if (!listView_ || !listView_->selectionModel()) {
    return;
  }

  if (mode_ == Mode::Single) {
    return;
  }

  QItemSelectionModel* popupSelectionModel = listView_->selectionModel();
  QSignalBlocker blocker(popupSelectionModel);
  popupSelectionModel->clearSelection();
  for (int row = 0; row < rows_.size(); ++row) {
    const ModelRow& modelRow = rows_.at(row);
    if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
      continue;
    }
    const Option& option = options_.at(modelRow.optionIndex);
    if (!isValueSelected(option.value)) {
      continue;
    }
    const QModelIndex index = listModel_ ? listModel_->index(row, 0) : QModelIndex();
    if (index.isValid()) {
      popupSelectionModel->select(index, QItemSelectionModel::Select);
    }
  }
}

void AdSelect::syncPopupOptionCursor(const QPoint& viewportPos) {
  if (!listView_) {
    return;
  }
  QWidget* viewport = listView_->viewport();
  if (!viewport) {
    return;
  }

  Qt::CursorShape shape = disabled() ? Qt::ForbiddenCursor : Qt::ArrowCursor;
  if (!disabled()) {
    const QModelIndex hoveredIndex = listView_->indexAt(viewportPos);
    shape = optionCursorShapeAtRow(hoveredIndex.isValid() ? hoveredIndex.row() : -1);
  }
  setWidgetCursorIfChanged(viewport, shape);
}

void AdSelect::syncContentLayoutForMode() {
  if (!contentLayout_ || !contentHost_ || !tagsContainer_ || !tagsLayout_ || !lineEdit_) {
    return;
  }

  const bool multipleMode = mode_ != Mode::Single;
  if (multipleMode) {
    if (contentLayout_->indexOf(tagsContainer_) < 0) {
      contentLayout_->insertWidget(0, tagsContainer_, 1);
    }
    if (contentLayout_->indexOf(lineEdit_) >= 0) {
      contentLayout_->removeWidget(lineEdit_);
    }
    if (lineEdit_->parentWidget() != tagsContainer_) {
      lineEdit_->setParent(tagsContainer_);
      lineEdit_->show();
    }
    lineEdit_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    lineEdit_->setMinimumWidth(4);
    if (tagsLayout_->indexOf(lineEdit_) < 0) {
      tagsLayout_->addWidget(lineEdit_);
    }
    if (placeholderLabel_) {
      placeholderLabel_->setParent(tagsContainer_);
      placeholderLabel_->show();
    }
    tagsContainer_->setVisible(true);
    return;
  }

  if (tagsLayout_->indexOf(lineEdit_) >= 0) {
    tagsLayout_->removeWidget(lineEdit_);
  }
  if (lineEdit_->parentWidget() != contentHost_) {
    lineEdit_->setParent(contentHost_);
    lineEdit_->show();
  }
  lineEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  lineEdit_->setMinimumWidth(4);
  lineEdit_->setMaximumWidth(QWIDGETSIZE_MAX);
  if (contentLayout_->indexOf(tagsContainer_) < 0) {
    contentLayout_->insertWidget(0, tagsContainer_);
  }
  if (contentLayout_->indexOf(lineEdit_) < 0) {
    contentLayout_->addWidget(lineEdit_, 1);
  }
  if (placeholderLabel_) {
    placeholderLabel_->hide();
  }
  tagsContainer_->setVisible(false);
}

void AdSelect::updateInputMode() {
  syncContentLayoutForMode();

  const Qt::CursorShape selectorCursor = selectorCursorShape();
  setWidgetCursorIfChanged(this, selectorCursor);
  setWidgetCursorIfChanged(contentHost_, selectorCursor);
  setWidgetCursorIfChanged(prefixLabel_, selectorCursor);
  setWidgetCursorIfChanged(tagsContainer_, selectorCursor);
  setWidgetCursorIfChanged(placeholderLabel_, selectorCursor);
  if (tagsContainer_) {
    const auto tagWidgets = tagsContainer_->findChildren<QWidget*>(
        QStringLiteral("adselect-tag-item"), Qt::FindChildrenRecursively);
    for (QWidget* tagWidget : tagWidgets) {
      setWidgetCursorIfChanged(tagWidget, selectorCursor);
    }
    const auto removeButtons = tagsContainer_->findChildren<QToolButton*>(
        QStringLiteral("adselect-tag-remove"), Qt::FindChildrenRecursively);
    const Qt::CursorShape removeCursor = disabled() ? Qt::ForbiddenCursor : Qt::PointingHandCursor;
    for (QToolButton* removeButton : removeButtons) {
      setWidgetCursorIfChanged(removeButton, removeCursor);
    }
  }
  if (suffixButton_) {
    const Qt::CursorShape suffixCursor =
        suffixButtonTriggersPopup() ? Qt::PointingHandCursor : selectorCursor;
    setWidgetCursorIfChanged(suffixButton_, suffixCursor);
  }

  const bool searchable = isSearchEnabledForCurrentMode();
  if (lineEdit_) {
    const bool readOnly = !searchable;
    lineEdit_->setReadOnly(readOnly);
    setWidgetCursorIfChanged(lineEdit_, selectorCursor);
  }

  if (listView_) {
    listView_->setSelectionMode(mode_ == Mode::Single ? QAbstractItemView::SingleSelection
                                                      : QAbstractItemView::MultiSelection);
    setWidgetCursorIfChanged(listView_, disabled() ? Qt::ForbiddenCursor : Qt::ArrowCursor);
    if (QWidget* viewport = listView_->viewport()) {
      if (disabled()) {
        setWidgetCursorIfChanged(viewport, Qt::ForbiddenCursor);
      } else {
        syncPopupOptionCursor(viewport->mapFromGlobal(QCursor::pos()));
      }
    }
  }
}

void AdSelect::updateDisplay() {
  if (!lineEdit_ || !tagsContainer_) {
    return;
  }

  suppressLineEditChange_ = true;

  if (mode_ == Mode::Single) {
    if (!inputMethodPreeditText_.isEmpty()) {
      inputMethodPreeditText_.clear();
    }
    clearTagWidgets();
    tagsContainer_->setVisible(false);
    tagsContainer_->setToolTip(QString());
    const QString label = currentValueKey_.isEmpty()
                              ? QString()
                              : fallbackSelectedLabel(rawValueForSelectionKey(currentValueKey_));
    const QString targetText = (open_ && isSearchEnabledForCurrentMode()) ? searchText_ : label;
    if (lineEdit_->text() != targetText) {
      lineEdit_->setText(targetText);
    }
    lineEdit_->setPlaceholderText(placeholder_);
    lineEdit_->setToolTip(label);
    lineEdit_->setTextMargins(0, 0, 0, 0);
    resetWidgetHeightConstraintsIfChanged(lineEdit_);
    if (placeholderLabel_) {
      placeholderLabel_->setVisible(false);
    }
  } else {
    rebuildTagWidgets();

    if (open_) {
      if (lineEdit_->text() != searchText_) {
        lineEdit_->setText(searchText_);
      }
    } else {
      if (!inputMethodPreeditText_.isEmpty()) {
        inputMethodPreeditText_.clear();
      }
      if (!lineEdit_->text().isEmpty()) {
        lineEdit_->clear();
      }
    }
    lineEdit_->setPlaceholderText(QString());
    lineEdit_->setToolTip(QString());
    lineEdit_->setTextMargins(0, 0, 0, 0);
    const QFontMetrics inputMetrics(lineEdit_->font());
    const QString inputText = lineEdit_->text();
    QString effectiveInputText = inputText;
    if (!inputMethodPreeditText_.isEmpty()) {
      const qsizetype cursorPosition =
          std::clamp(static_cast<qsizetype>(lineEdit_->cursorPosition()), qsizetype(0),
                     effectiveInputText.size());
      effectiveInputText.insert(cursorPosition, inputMethodPreeditText_);
    }
    const int inputWidth =
        effectiveInputText.isEmpty()
            ? 4
            : std::max(
                  4, inputMetrics.horizontalAdvance(effectiveInputText + QStringLiteral(" ")) + 2);
    int availableInputWidth = tagsContainer_->contentsRect().width();
    if (contentHost_) {
      availableInputWidth = std::max(availableInputWidth, contentHost_->contentsRect().width());
    }
    if (availableInputWidth <= 0) {
      availableInputWidth = contentsRect().width();
    }
    if (availableInputWidth <= 0) {
      availableInputWidth = std::max(4, width());
    }
    const int cappedInputWidth = std::min(inputWidth, std::max(4, availableInputWidth));
    setWidgetFixedWidthIfChanged(lineEdit_, cappedInputWidth);
    const int tagHeight = std::max(16, visualStyle_ ? visualStyle_->metrics.tagHeight : 20);
    const int tagItemMargin = std::max(0, visualStyle_ ? visualStyle_->metrics.tagItemMargin : 2);
    setWidgetFixedHeightIfChanged(lineEdit_, std::max(1, tagHeight + tagItemMargin * 2));

    if (placeholderLabel_) {
      const bool showPlaceholder = currentValueKeys_.isEmpty() && effectiveInputText.isEmpty() &&
                                   !placeholder_.trimmed().isEmpty();
      placeholderLabel_->setText(placeholder_);
      placeholderLabel_->setVisible(showPlaceholder);
    }
  }

  suppressLineEditChange_ = false;
  updateMultipleSelectorHeight();
}

void AdSelect::updateMultipleSelectorHeight() {
  if (!visualStyle_ || mode_ == Mode::Single || !rootLayout_ || !tagsContainer_ || !tagsLayout_) {
    return;
  }

  int availableWidth = tagsContainer_->contentsRect().width();
  if (contentHost_) {
    availableWidth = std::max(availableWidth, contentHost_->contentsRect().width());
  }
  if (availableWidth <= 0) {
    availableWidth = contentsRect().width();
  }
  if (availableWidth <= 0) {
    availableWidth = std::max(1, width());
  }

  int tagsHeight = 0;
  if (auto* wrappingLayout = dynamic_cast<WrappingTagsLayout*>(tagsLayout_)) {
    tagsHeight = wrappingLayout->layoutHeightForWidth(availableWidth);
  } else {
    tagsHeight = tagsLayout_->sizeHint().height();
  }

  const int minTagsHeight = std::max(
      16, visualStyle_->metrics.tagHeight + std::max(0, visualStyle_->metrics.tagItemMargin) * 2);
  tagsHeight = std::max(minTagsHeight, tagsHeight);
  setWidgetFixedHeightIfChanged(tagsContainer_, tagsHeight);

  if (placeholderLabel_) {
    const bool hasPrefix = prefixLabel_ && prefixLabel_->isVisible();
    const int inset =
        hasPrefix ? 0 : std::max(0, visualStyle_->metrics.multipleItemPaddingHorizontal);
    const int labelWidth = std::max(0, availableWidth - inset);
    placeholderLabel_->setGeometry(inset, 0, labelWidth, tagsHeight);
    placeholderLabel_->raise();
  }

  const QMargins rootMargins = rootLayout_->contentsMargins();
  const bool wrappedToMultipleLines = tagsHeight > minTagsHeight;
  const int expandedHeight = tagsHeight + rootMargins.top() + rootMargins.bottom();
  const int targetHeight = wrappedToMultipleLines
                               ? std::max(visualStyle_->metrics.height, expandedHeight)
                               : visualStyle_->metrics.height;
  setWidgetFixedHeightIfChanged(this, targetHeight);
  updateGeometry();
}

void AdSelect::updateClearButton() {
  if (!clearButton_ || !visualStyle_) {
    return;
  }
  const bool hasValue =
      mode_ == Mode::Single ? !currentValueKey_.isEmpty() : !currentValueKeys_.isEmpty();
  const bool canShow = allowClear_ && hasValue && !disabled();
  if (!canShow) {
    clearHovered_ = false;
  }
  const bool hovered = hovered_ || clearHovered_ || underMouse() ||
                       (lineEdit_ && lineEdit_->underMouse()) ||
                       (clearButton_ && clearButton_->underMouse()) ||
                       (suffixButton_ && suffixButton_->underMouse());
  const bool shouldShow = canShow && hovered;
  if (clearButton_->isVisible() != shouldShow) {
    clearButton_->setVisible(shouldShow);
  }
  updateClearVisual();
  updateAccessoryGeometry();
}

void AdSelect::updateClearVisual() {
  if (!clearButton_ || !visualStyle_) {
    return;
  }

  const int iconSize = std::max(10, visualStyle_->metrics.iconSize);
  clearButton_->setText(QString());
  QColor iconColor = visualStyle_->clearColor;
  if (clearButton_->isVisible() && clearHovered_ && !disabled()) {
    iconColor = visualStyle_->clearHoverColor;
  } else if (disabled()) {
    iconColor = visualStyle_->disabledTextColor;
  }

  adqt::icons::IconRef icon = filled_icons::CloseCircle();
  if (adqt::icons::isValid(icon)) {
    icon = icon.withColors(adqt::icons::IconColors::primary(iconColor));
    const qreal dpr = devicePixelRatioF();
    const QPixmap pixmap = adqt::icons::renderIconPixmap(icon, {QSize(iconSize, iconSize), dpr});
    clearButton_->setIcon(QIcon(pixmap));
    clearButton_->setIconSize(QSize(iconSize, iconSize));
  } else {
    clearButton_->setIcon(QIcon());
    clearButton_->setText(QStringLiteral("x"));
  }

  const QColor clearBg = clearButton_->isVisible() ? visualStyle_->clearBg : QColor(0, 0, 0, 0);
  const int radius = std::max(0, iconSize / 2);
  if (auto* flatButton = dynamic_cast<FlatIconToolButton*>(clearButton_)) {
    flatButton->setBackgroundDecoration(clearBg, radius);
  }
}

void AdSelect::updateAccessoryGeometry() {
  if (!visualStyle_) {
    return;
  }

  const int iconSize = std::max(10, visualStyle_->metrics.iconSize);
  if (suffixButton_) {
    // Keep suffix footprint aligned with clear overlay, matching Ant Design:
    // clear is absolutely positioned over the suffix slot when it appears.
    const int feedbackGap = adqt::icons::isValid(feedbackIconRef_) ? std::max(2, iconSize / 4) : 0;
    const int feedbackWidth = adqt::icons::isValid(feedbackIconRef_) ? iconSize : 0;
    suffixButton_->setFixedSize(iconSize + feedbackGap + feedbackWidth, iconSize);
  }

  if (!clearButton_) {
    return;
  }
  clearButton_->setFixedSize(iconSize, iconSize);

  const int borderInset = std::max(0, visualStyle_->metrics.borderWidth);
  const int endInset = std::max(0, visualStyle_->metrics.horizontalPadding) + borderInset;
  const int x = std::max(0, width() - endInset - iconSize);
  const int y = std::max(0, (height() - iconSize) / 2);
  clearButton_->move(x, y);
  clearButton_->raise();
}

void AdSelect::updatePrefixVisual() {
  if (!prefixLabel_ || !visualStyle_) {
    return;
  }

  const bool hasPrefixText = !prefixText_.trimmed().isEmpty();
  const bool hasPrefixIcon = adqt::icons::isValid(prefixIconRef_);
  if (!hasPrefixText && !hasPrefixIcon) {
    prefixLabel_->clear();
    prefixLabel_->setVisible(false);
    return;
  }

  if (hasPrefixText) {
    prefixLabel_->setPixmap(QPixmap());
    prefixLabel_->setText(prefixText_);
    prefixLabel_->setVisible(true);
    return;
  }

  adqt::icons::IconRef icon = prefixIconRef_;
  icon = icon.withColors(adqt::icons::IconColors::primary(visualStyle_->prefixColor));
  const qreal dpr = devicePixelRatioF();
  const int iconSize = std::max(10, visualStyle_->metrics.iconSize);
  const QPixmap pixmap = adqt::icons::renderIconPixmap(icon, {QSize(iconSize, iconSize), dpr});
  prefixLabel_->setText(QString());
  prefixLabel_->setPixmap(pixmap);
  prefixLabel_->setVisible(!pixmap.isNull());
}

void AdSelect::updateLoadingSpinnerState() {
  const bool spinning = loading_ || isLoadingIcon(feedbackIconRef_);
  if (spinning) {
    if (!suffixSpinnerSubscribed_) {
      detail::setFrameSubscription(this, QString::fromLatin1(kSuffixSpinnerFrameKey), true,
                                   [this](qint64, qint64) {
                                     if (!loading_ && !isLoadingIcon(feedbackIconRef_)) {
                                       return;
                                     }
                                     updateSuffixVisual();
                                   });
      suffixSpinnerSubscribed_ = true;
    }
    return;
  }

  if (suffixSpinnerSubscribed_) {
    detail::clearFrameSubscription(this, QString::fromLatin1(kSuffixSpinnerFrameKey));
    suffixSpinnerSubscribed_ = false;
  }
}

void AdSelect::updateAccessibility() {
  const QString controlName =
      mode_ == Mode::Single ? tr("Select")
                            : (mode_ == Mode::Multiple ? tr("Multi-select") : tr("Tag select"));
  const QString placeholderText = placeholder_.trimmed();

  if (contentHost_) {
    contentHost_->setAccessibleName(controlName);
    contentHost_->setAccessibleDescription(placeholderText);
  }
  if (lineEdit_) {
    lineEdit_->setAccessibleName(isSearchEnabledForCurrentMode() ? tr("Filter options")
                                                                 : controlName);
    lineEdit_->setAccessibleDescription(placeholderText);
  }
  if (clearButton_) {
    clearButton_->setAccessibleName(tr("Clear selection"));
  }
  if (suffixButton_) {
    const QString suffixName = suffixButtonTriggersPopup()
                                   ? (open_ ? tr("Close options") : tr("Open options"))
                                   : tr("Selector icon");
    suffixButton_->setAccessibleName(suffixName);
  }
  if (popup_) {
    popup_->setAccessibleName(tr("Select popup"));
  }
  if (popupScrollArea_) {
    popupScrollArea_->setAccessibleName(tr("Options"));
  }
  if (listView_) {
    listView_->setAccessibleName(tr("Options"));
  }
}

void AdSelect::updateSuffixVisual() {
  if (!suffixButton_ || !visualStyle_) {
    return;
  }

  suffixButton_->setText(QString());
  suffixButton_->setIcon(QIcon());

  if (loading_) {
    const qreal dpr = devicePixelRatioF();
    const int iconSize = std::max(10, visualStyle_->metrics.iconSize);
    const int cycleMs = detail::spinnerCycleDurationMs();
    int angle = 0;
    if (cycleMs > 0) {
      qint64 phaseMs = detail::timingNowMs() % cycleMs;
      if (phaseMs < 0) {
        phaseMs += cycleMs;
      }
      angle = static_cast<int>((phaseMs * 360) / cycleMs);
    }
    QPixmap pixmap =
        makeSpinnerPixmap(QSize(iconSize, iconSize), dpr, visualStyle_->suffixColor, angle);
    if (!pixmap.isNull()) {
      const int feedbackGap =
          adqt::icons::isValid(feedbackIconRef_) ? std::max(2, iconSize / 4) : 0;
      const int logicalWidth =
          iconSize + feedbackGap + (adqt::icons::isValid(feedbackIconRef_) ? iconSize : 0);
      const QPixmap suffixPixmap =
          appendFeedbackIconPixmap(pixmap, feedbackIconRef_, iconSize, feedbackGap, dpr,
                                   isLoadingIcon(feedbackIconRef_) ? sharedSpinnerAngle() : 0);
      suffixButton_->setIcon(QIcon(suffixPixmap));
      suffixButton_->setIconSize(QSize(logicalWidth, iconSize));
      return;
    }
    suffixButton_->setText(QStringLiteral("..."));
    return;
  }

  adqt::icons::IconRef icon = suffixIconRef_;
  if (!adqt::icons::isValid(icon)) {
    const bool showSearchIcon = open_ && isSearchEnabledForCurrentMode();
    icon = showSearchIcon ? outlined_icons::Search() : outlined_icons::Down();
  }
  if (adqt::icons::isValid(icon)) {
    icon = icon.withColors(adqt::icons::IconColors::primary(visualStyle_->suffixColor));
    const qreal dpr = devicePixelRatioF();
    const int iconSize = std::max(10, visualStyle_->metrics.iconSize);
    const QPixmap pixmap = adqt::icons::renderIconPixmap(icon, {QSize(iconSize, iconSize), dpr});
    if (!pixmap.isNull()) {
      QPixmap suffixPixmap = pixmap;
      QSize suffixIconSize(iconSize, iconSize);
      if (adqt::icons::isValid(feedbackIconRef_)) {
        const int feedbackGap = std::max(2, iconSize / 4);
        const int logicalWidth = iconSize + feedbackGap + iconSize;
        suffixPixmap =
            appendFeedbackIconPixmap(pixmap, feedbackIconRef_, iconSize, feedbackGap, dpr,
                                     isLoadingIcon(feedbackIconRef_) ? sharedSpinnerAngle() : 0);
        suffixIconSize = QSize(logicalWidth, iconSize);
      }
      suffixButton_->setIcon(QIcon(suffixPixmap));
      suffixButton_->setIconSize(suffixIconSize);
      return;
    }
  }

  suffixButton_->setText(QStringLiteral("v"));
}

void AdSelect::applyVisualStyle() {
  if (!visualStyle_ || applyingVisualStyle_) {
    return;
  }
  QScopedValueRollback<bool> styleGuard(applyingVisualStyle_, true);

  StyleContext context;
  context.mode = mode_;
  context.controlSize = controlSize_;
  context.variant = variant_;
  context.status = status_;
  context.disabled = disabled();
  context.popupVisible = open_;
  context.searchText = searchText_;
  context.currentValues = currentValues();
  context.currentValueKeys = currentValueKeys_;

  SemanticStyles effectiveSemantic = semanticStyles_;
  if (semanticStyleResolver_) {
    effectiveSemantic = semanticStyleResolver_(context);
  }

  detail::SelectStyleInput input;
  input.mode = mode_;
  input.controlSize = controlSize_;
  input.variant = variant_;
  input.status = status_;
  input.disabled = disabled();
  input.baseFont = font();
  input.componentTokens = componentTokens_;
  input.semanticStyles = effectiveSemantic;
  const detail::SelectVisualStyle previousStyle = *visualStyle_;
  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this);
  *visualStyle_ = detail::resolveSelectVisualStyle(input, resolvedTheme);
  const bool prefixIconColorOverridesChanged =
      previousStyle.prefixColor != visualStyle_->prefixColor ||
      previousStyle.metrics.iconSize != visualStyle_->metrics.iconSize;
  const bool suffixIconColorOverridesChanged =
      previousStyle.suffixColor != visualStyle_->suffixColor ||
      previousStyle.metrics.iconSize != visualStyle_->metrics.iconSize;
  const bool listDelegateStyleChanged =
      previousStyle.popupBg != visualStyle_->popupBg ||
      previousStyle.optionTextColor != visualStyle_->optionTextColor ||
      previousStyle.optionHoverBg != visualStyle_->optionHoverBg ||
      previousStyle.optionSelectedBg != visualStyle_->optionSelectedBg ||
      previousStyle.optionSelectedColor != visualStyle_->optionSelectedColor ||
      previousStyle.selectorActiveBorderColor != visualStyle_->selectorActiveBorderColor ||
      previousStyle.disabledTextColor != visualStyle_->disabledTextColor ||
      previousStyle.disabledBg != visualStyle_->disabledBg ||
      previousStyle.metrics.optionBorderRadius != visualStyle_->metrics.optionBorderRadius ||
      previousStyle.metrics.optionPaddingHorizontal !=
          visualStyle_->metrics.optionPaddingHorizontal ||
      previousStyle.metrics.optionPaddingVertical != visualStyle_->metrics.optionPaddingVertical ||
      previousStyle.metrics.iconSize != visualStyle_->metrics.iconSize ||
      previousStyle.metrics.optionFont != visualStyle_->metrics.optionFont;

  bool widgetStyleChanged = false;

  widgetStyleChanged |= setWidgetFontIfChanged(this, visualStyle_->metrics.selectorFont);
  if (lineEdit_) {
    widgetStyleChanged |= setWidgetFontIfChanged(lineEdit_, visualStyle_->metrics.selectorFont);
  }
  if (tagsContainer_) {
    widgetStyleChanged |=
        setWidgetFontIfChanged(tagsContainer_, visualStyle_->metrics.selectorFont);
  }
  if (placeholderLabel_) {
    widgetStyleChanged |=
        setWidgetFontIfChanged(placeholderLabel_, visualStyle_->metrics.selectorFont);
  }
  if (prefixLabel_) {
    widgetStyleChanged |= setWidgetFontIfChanged(prefixLabel_, visualStyle_->metrics.selectorFont);
    const int prefixInset = mode_ != Mode::Single
                                ? std::max(0, visualStyle_->metrics.multipleItemPaddingHorizontal)
                                : 0;
    widgetStyleChanged |=
        setWidgetContentsMarginsIfChanged(prefixLabel_, QMargins(prefixInset, 0, 0, 0));
  }

  const bool multipleMode = mode_ != Mode::Single;
  // QLayout margins are measured from the widget outer rect, while CSS padding
  // is measured from the inner border edge (border-box). Add border inset to
  // match Ant Design's selector content positioning.
  const int borderInset = std::max(0, visualStyle_->metrics.borderWidth);
  const int baseStartPadding = multipleMode
                                   ? std::max(0, visualStyle_->metrics.multiplePaddingInlineStart)
                                   : std::max(0, visualStyle_->metrics.horizontalPadding);
  const int baseEndPadding = std::max(0, visualStyle_->metrics.horizontalPadding);
  const int baseVerticalPadding =
      multipleMode ? std::max(0, visualStyle_->metrics.multiplePaddingVertical) : 0;
  const int startPadding = baseStartPadding + borderInset;
  const int endPadding = baseEndPadding + borderInset;
  const int verticalPadding = baseVerticalPadding + borderInset;
  widgetStyleChanged |= setLayoutContentsMarginsIfChanged(
      rootLayout_, QMargins(startPadding, verticalPadding, endPadding, verticalPadding));
  widgetStyleChanged |= setLayoutSpacingIfChanged(rootLayout_, visualStyle_->metrics.spacing);
  widgetStyleChanged |=
      setLayoutSpacingIfChanged(contentLayout_, std::max(0, visualStyle_->metrics.tagItemGap));

  widgetStyleChanged |= setWidgetFixedHeightIfChanged(this, visualStyle_->metrics.height);

  const bool openSingleDisplay = mode_ == Mode::Single && open_ && !isSearchEnabledForCurrentMode();
  bool lineEditStyleChanged = false;

  if (lineEdit_) {
    QPalette inputPalette = lineEdit_->palette();
    const QColor inputColor =
        openSingleDisplay ? visualStyle_->placeholderColor : visualStyle_->selectorTextColor;
    inputPalette.setColor(QPalette::Text, inputColor);
    inputPalette.setColor(QPalette::Disabled, QPalette::Text, visualStyle_->disabledTextColor);
    inputPalette.setColor(QPalette::Highlight, visualStyle_->optionSelectedBg);
    inputPalette.setColor(QPalette::HighlightedText, visualStyle_->optionSelectedColor);
    inputPalette.setColor(QPalette::PlaceholderText, visualStyle_->placeholderColor);
    lineEditStyleChanged = setWidgetPaletteIfChanged(lineEdit_, inputPalette);
    if (lineEditStyleChanged) {
      lineEdit_->update();
    }
  }

  bool tagsStyleChanged = false;
  if (tagsLayout_) {
    tagsStyleChanged |=
        setLayoutSpacingIfChanged(tagsLayout_, std::max(0, visualStyle_->metrics.tagItemGap));
  }

  bool prefixPaletteChanged = false;
  if (prefixLabel_) {
    QPalette prefixPalette = prefixLabel_->palette();
    prefixPalette.setColor(QPalette::WindowText, visualStyle_->prefixColor);
    prefixPalette.setColor(QPalette::Disabled, QPalette::WindowText,
                           visualStyle_->disabledTextColor);
    prefixPaletteChanged = setWidgetPaletteIfChanged(prefixLabel_, prefixPalette);
    if (prefixPaletteChanged) {
      prefixLabel_->update();
    }
  }
  if (placeholderLabel_) {
    QPalette placeholderPalette = placeholderLabel_->palette();
    placeholderPalette.setColor(QPalette::WindowText, visualStyle_->placeholderColor);
    placeholderPalette.setColor(QPalette::Disabled, QPalette::WindowText,
                                visualStyle_->disabledTextColor);
    const bool placeholderPaletteChanged =
        setWidgetPaletteIfChanged(placeholderLabel_, placeholderPalette);
    if (placeholderPaletteChanged) {
      placeholderLabel_->update();
    }
    widgetStyleChanged |= placeholderPaletteChanged;
  }

  const auto applyToolButtonPalette = [this](QToolButton* button, const QColor& textColor) -> bool {
    if (!button || !visualStyle_) {
      return false;
    }
    QPalette palette = button->palette();
    palette.setColor(QPalette::ButtonText, textColor);
    palette.setColor(QPalette::WindowText, textColor);
    palette.setColor(QPalette::Text, textColor);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, visualStyle_->disabledTextColor);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, visualStyle_->disabledTextColor);
    const bool changed = setWidgetPaletteIfChanged(button, palette);
    if (changed) {
      button->update();
    }
    return changed;
  };
  const bool suffixPaletteChanged =
      applyToolButtonPalette(suffixButton_, visualStyle_->suffixColor);
  const QColor clearPaletteColor =
      clearHovered_ ? visualStyle_->clearHoverColor : visualStyle_->clearColor;
  const bool clearPaletteChanged = applyToolButtonPalette(clearButton_, clearPaletteColor);

  bool popupStyleChanged = false;
  if (popup_) {
    if (popupLayout_) {
      const int popupPadding = visualStyle_->metrics.popupPadding;
      const QMargins visualPadding(popupPadding, popupPadding, popupPadding, popupPadding);
      popupStyleChanged |= setLayoutContentsMarginsIfChanged(
          popupLayout_, detail::addAntPopupShadowMarginsToPadding(visualPadding));
      popupStyleChanged |= setLayoutSpacingIfChanged(popupLayout_, 0);
    }

    static_cast<PopupFrame*>(popup_)->setVisualStyle(visualStyle_->popupBg,
                                                     visualStyle_->popupBorderColor,
                                                     visualStyle_->metrics.popupBorderRadius);

    if (popupScrollArea_) {
      QPalette scrollPalette = popupScrollArea_->palette();
      scrollPalette.setColor(QPalette::Base, visualStyle_->popupBg);
      scrollPalette.setColor(QPalette::Window, visualStyle_->popupBg);
      popupStyleChanged |= setWidgetPaletteIfChanged(popupScrollArea_, scrollPalette);
      if (QWidget* viewport = popupScrollArea_->viewport()) {
        popupStyleChanged |= setWidgetPaletteIfChanged(viewport, scrollPalette);
        popupStyleChanged |= setWidgetAutoFillBackgroundIfChanged(viewport, true);
      }
    }

    if (listView_) {
      QPalette listPalette = listView_->palette();
      listPalette.setColor(QPalette::Base, visualStyle_->popupBg);
      listPalette.setColor(QPalette::Window, visualStyle_->popupBg);
      listPalette.setColor(QPalette::Text, visualStyle_->optionTextColor);
      listPalette.setColor(QPalette::Disabled, QPalette::Text, visualStyle_->disabledTextColor);
      listPalette.setColor(QPalette::Highlight, visualStyle_->optionSelectedBg);
      listPalette.setColor(QPalette::HighlightedText, visualStyle_->optionSelectedColor);
      popupStyleChanged |= setWidgetPaletteIfChanged(listView_, listPalette);
      if (QWidget* viewport = listView_->viewport()) {
        popupStyleChanged |= setWidgetAutoFillBackgroundIfChanged(viewport, true);
      }
    }
  }

  updateInteractionFocusOverlay();
  const bool hasPrefixIcon =
      prefixText_.trimmed().isEmpty() && adqt::icons::isValid(prefixIconRef_);
  const bool shouldRefreshPrefixIcon = hasPrefixIcon && prefixIconColorOverridesChanged;
  const bool shouldRefreshSuffixIcon = suffixIconColorOverridesChanged || loading_;
  if (shouldRefreshPrefixIcon) {
    updatePrefixVisual();
  }
  if (shouldRefreshSuffixIcon) {
    updateSuffixVisual();
  }
  updateDisplay();
  updateClearButton();
  updateAccessoryGeometry();
  if (widgetStyleChanged || lineEditStyleChanged || tagsStyleChanged || prefixPaletteChanged ||
      suffixPaletteChanged || clearPaletteChanged || shouldRefreshPrefixIcon ||
      shouldRefreshSuffixIcon) {
    update();
  }
  if (listView_ && popup_ && popup_->isVisible() && listView_->viewport() &&
      (popupStyleChanged || listDelegateStyleChanged)) {
    listView_->viewport()->update();
  }
}

void AdSelect::refreshRows() {
  const bool preserveScrollPosition =
      preservePopupScrollOnRefresh_ && open_ && mode_ != Mode::Single;
  preservePopupScrollOnRefresh_ = false;

  int preservedScrollValue = -1;
  QVariant preservedCurrentValue;
  if (preserveScrollPosition && listView_ && popupScrollArea_) {
    if (QScrollBar* scrollBar = popupScrollArea_->verticalScrollBar()) {
      preservedScrollValue = scrollBar->value();
    }
    const QModelIndex currentIndex = listView_->currentIndex();
    if (currentIndex.isValid()) {
      const int currentRow = currentIndex.row();
      if (currentRow >= 0 && currentRow < rows_.size()) {
        const ModelRow& modelRow = rows_.at(currentRow);
        if (modelRow.optionIndex >= 0 && modelRow.optionIndex < options_.size()) {
          preservedCurrentValue = options_.at(modelRow.optionIndex).value;
        }
      }
    }
  }

  QVector<ModelRow> nextRows;
  const QVector<int> filtered = filteredOptionIndexes();

  bool hasGroup = false;
  for (const Option& option : options_) {
    if (!option.group.trimmed().isEmpty()) {
      hasGroup = true;
      break;
    }
  }

  QString currentGroup;
  for (int index : filtered) {
    if (index < 0 || index >= options_.size()) {
      continue;
    }
    const Option& option = options_.at(index);
    const QString group = option.group.trimmed();
    if (hasGroup && !group.isEmpty() && group != currentGroup) {
      ModelRow groupRow;
      groupRow.header = true;
      groupRow.optionIndex = -1;
      groupRow.headerText = group;
      nextRows.append(groupRow);
      currentGroup = group;
    } else if (group.isEmpty()) {
      currentGroup.clear();
    }

    ModelRow row;
    row.optionIndex = index;
    nextRows.append(row);
  }

  if (nextRows.isEmpty()) {
    ModelRow emptyRow;
    emptyRow.empty = true;
    emptyRow.headerText = tr("No data");
    nextRows.append(emptyRow);
  }

  rows_ = nextRows;
  if (listModel_) {
    listModel_->setRows(rows_);
  }
  syncCurrentListRow(preservedCurrentValue, preserveScrollPosition);
  syncPopupSelectionState();
  if (listView_ && listView_->viewport()) {
    syncPopupOptionCursor(listView_->viewport()->mapFromGlobal(QCursor::pos()));
  }
  if (popupIsVisible()) {
    syncPopupGeometry();
  }
  if (preserveScrollPosition && preservedScrollValue >= 0 && popupScrollArea_) {
    if (QScrollBar* scrollBar = popupScrollArea_->verticalScrollBar()) {
      scrollBar->setValue(preservedScrollValue);
    }
  }
}

QVector<int> AdSelect::filteredOptionIndexes() const {
  QVector<int> indexes;
  if (!filterProxyModel_) {
    indexes.reserve(options_.size());
    for (int i = 0; i < options_.size(); ++i) {
      indexes.append(i);
    }
    return indexes;
  }

  indexes.reserve(filterProxyModel_->rowCount());
  for (int row = 0; row < filterProxyModel_->rowCount(); ++row) {
    const QModelIndex proxyIndex = filterProxyModel_->index(row, 0);
    if (!proxyIndex.isValid()) {
      continue;
    }
    const QModelIndex sourceIndex = filterProxyModel_->mapToSource(proxyIndex);
    if (!sourceIndex.isValid()) {
      continue;
    }
    indexes.append(sourceIndex.row());
  }
  return indexes;
}

void AdSelect::syncCurrentListRow(const QVariant& preferredValue, bool preserveScrollPosition) {
  if (!listView_ || rows_.isEmpty()) {
    return;
  }

  int targetRow = -1;
  if (preferredValue.isValid() && !preferredValue.isNull()) {
    for (int row = 0; row < rows_.size(); ++row) {
      const ModelRow& modelRow = rows_.at(row);
      if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
        continue;
      }
      if (options_.at(modelRow.optionIndex).value == preferredValue) {
        targetRow = row;
        break;
      }
    }
  }

  if (mode_ == Mode::Single && !currentValueKey_.isEmpty()) {
    for (int row = 0; row < rows_.size(); ++row) {
      const ModelRow& modelRow = rows_.at(row);
      if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
        continue;
      }
      const Option& option = options_.at(modelRow.optionIndex);
      if (detail::selectValueKey(option.value) == currentValueKey_) {
        targetRow = row;
        break;
      }
    }
  }

  if (targetRow < 0 && mode_ != Mode::Single && !currentValueKeys_.isEmpty()) {
    const QString selectedKey = currentValueKeys_.constFirst();
    for (int row = 0; row < rows_.size(); ++row) {
      const ModelRow& modelRow = rows_.at(row);
      if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
        continue;
      }
      const Option& option = options_.at(modelRow.optionIndex);
      if (detail::selectValueKey(option.value) == selectedKey) {
        targetRow = row;
        break;
      }
    }
  }

  if (targetRow < 0) {
    for (int row = 0; row < rows_.size(); ++row) {
      const ModelRow& modelRow = rows_.at(row);
      if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
        continue;
      }
      if (!options_.at(modelRow.optionIndex).disabled) {
        targetRow = row;
        break;
      }
    }
  }

  if (targetRow >= 0) {
    const QModelIndex targetIndex = listModel_->index(targetRow, 0);
    listView_->setCurrentIndex(targetIndex);
    if (mode_ == Mode::Single && listView_->selectionModel()) {
      listView_->selectionModel()->select(targetIndex, QItemSelectionModel::ClearAndSelect);
    }
    if (!preserveScrollPosition && popupScrollArea_) {
      const QRect targetRect = listView_->visualRect(targetIndex);
      if (targetRect.isValid()) {
        const int margin = visualStyle_ ? std::max(2, visualStyle_->metrics.optionHeight / 2) : 8;
        popupScrollArea_->ensureVisible(targetRect.center().x(), targetRect.center().y(), 0,
                                        margin);
      }
    }
  }
}

bool AdSelect::addTagValue(const QString& value) {
  const QString normalized = value.trimmed();
  if (normalized.isEmpty()) {
    return false;
  }
  if (mode_ != Mode::Tags) {
    return false;
  }
  const QString normalizedKey = detail::selectValueKey(normalized);
  if (currentValueKeys_.contains(normalizedKey)) {
    return false;
  }
  if (maxCount_ > 0 && currentValueKeys_.size() >= maxCount_) {
    return false;
  }

  const SelectionSnapshot previous = captureSelectionSnapshot();
  QVariantList nextValues = currentValues();
  nextValues.append(normalized);
  if (normalizedSelectionValues(nextValues).size() == currentValues().size()) {
    return false;
  }
  applySelectedValues(nextValues);
  updateSelectionCaches();
  emit selected(normalized, fallbackSelectedLabel(normalized));
  emitSelectionSignalsFromSnapshot(previous);
  return true;
}

void AdSelect::ensureTagOptionExists(const QVariant& value) {
  const QVariant normalized = detail::normalizeSelectValue(value);
  if (sourceIndexForValue(normalized).isValid() || compositeIndexForValue(normalized).isValid()) {
    return;
  }
  ensureOverlayModel();
  auto* model = qobject_cast<QStandardItemModel*>(overlayModel_.data());
  if (!model) {
    return;
  }

  auto* item = new QStandardItem(normalized.toString().trimmed());
  item->setData(normalized.toString().trimmed(), Qt::DisplayRole);
  item->setData(normalized, valueRole_);
  item->setData(normalized.toString().trimmed(), labelRole_);
  model->appendRow(item);
  syncModelBackedOptions();
}

void AdSelect::consumeTokenizedInput(const QString& text) {
  if (tokenSeparators_.isEmpty()) {
    return;
  }

  QString pattern;
  for (const QString& separator : tokenSeparators_) {
    if (separator.isEmpty()) {
      continue;
    }
    if (!pattern.isEmpty()) {
      pattern.append(QStringLiteral("|"));
    }
    pattern.append(escapedForRegex(separator));
  }
  if (pattern.isEmpty()) {
    return;
  }

  QRegularExpression regex(pattern);
  if (!regex.isValid()) {
    return;
  }
  if (!text.contains(regex)) {
    return;
  }

  QStringList parts = text.split(regex, Qt::KeepEmptyParts);
  if (parts.isEmpty()) {
    return;
  }

  const QRegularExpression trailingRegex(QStringLiteral("(%1)$").arg(pattern));
  const bool trailingSeparator = trailingRegex.isValid() && trailingRegex.match(text).hasMatch();
  const int limit =
      trailingSeparator ? static_cast<int>(parts.size()) : static_cast<int>(parts.size()) - 1;
  bool changed = false;
  for (int i = 0; i < limit; ++i) {
    if (addTagValue(parts.at(i))) {
      changed = true;
    }
  }

  const QString remaining = trailingSeparator ? QString() : parts.constLast();
  if (searchText_ != remaining) {
    searchText_ = remaining;
    emit searchTextChanged(searchText_);
  }
  suppressLineEditChange_ = true;
  lineEdit_->setText(remaining);
  suppressLineEditChange_ = false;

  if (changed) {
    refreshRows();
    updateDisplay();
    updateClearButton();
  }
}

void AdSelect::clearSelectionInternal(bool emitSignals) {
  const SelectionSnapshot previous = captureSelectionSnapshot();
  if (currentValues().isEmpty()) {
    return;
  }

  applySelectedValues({}, false);
  updateSelectionCaches();

  if (emitSignals) {
    emit cleared();
  }
  refreshRows();
  updateDisplay();
  updateClearButton();
  emitSelectionSignalsFromSnapshot(previous);
}

void AdSelect::emitSelectionChangedSignals() { emit selectionChanged(currentItems()); }

void AdSelect::toggleSelectionForOption(const Option& option) {
  if (option.disabled) {
    return;
  }

  const SelectionSnapshot previous = captureSelectionSnapshot();
  const QString optionKey = detail::selectValueKey(option.value);
  const QVariant selectedRaw = rawValueForSelectionKey(optionKey);
  const QString selectedLabel = fallbackSelectedLabel(option.value);

  if (mode_ == Mode::Single) {
    const bool changed = currentValueKey_ != optionKey;
    applySelectedValues({option.value}, false);
    updateSelectionCaches();
    updateDisplay();
    updateClearButton();
    if (changed) {
      emit selected(rawValueForSelectionKey(optionKey), selectedLabel);
    }
    emitSelectionSignalsFromSnapshot(previous);
    closePopup();
    return;
  }

  const bool shouldRestoreInputFocus = open_ && lineEdit_ && !lineEdit_->isReadOnly();
  QVariantList nextValues = currentValues();

  const int index = static_cast<int>(currentValueKeys_.indexOf(optionKey));
  if (index >= 0) {
    if (index < nextValues.size()) {
      nextValues.removeAt(index);
    } else {
      nextValues.removeAll(selectedRaw);
    }
    emit deselected(selectedRaw, selectedLabel);
  } else {
    if (maxCount_ > 0 && currentValueKeys_.size() >= maxCount_) {
      return;
    }
    nextValues.append(option.value);
    emit selected(option.value, selectedLabel);
  }

  applySelectedValues(nextValues, false);
  updateSelectionCaches();
  emitSelectionSignalsFromSnapshot(previous);
  if (autoClearSearchValue_ && isSearchEnabledForCurrentMode()) {
    preservePopupScrollOnRefresh_ = true;
    setSearchText(QString());
    suppressLineEditChange_ = true;
    lineEdit_->clear();
    suppressLineEditChange_ = false;
  }
  preservePopupScrollOnRefresh_ = true;
  refreshRows();
  updateDisplay();
  updateClearButton();
  if (shouldRestoreInputFocus && lineEdit_ && !lineEdit_->hasFocus()) {
    lineEdit_->setFocus(Qt::MouseFocusReason);
  }
}

void AdSelect::ensurePopup() {
  if (popup_) {
    return;
  }

  QWidget* scopeWindow = detail::resolvePopupScopeWindow(this);
  popup_ = new PopupFrame(scopeWindow);
  popup_->setAttribute(Qt::WA_DeleteOnClose, false);
  popup_->setObjectName(QStringLiteral("adselect-popup"));
  popup_->setProperty("adqt.interaction.surface", true);
  popup_->installEventFilter(this);
  applyPopupLayerMode();

  popupLayout_ = new QVBoxLayout(popup_);
  popupLayout_->setContentsMargins(4, 4, 4, 4);
  popupLayout_->setSpacing(0);

  popupScrollArea_ = new AdScrollArea(popup_);
  popupScrollArea_->setObjectName(QStringLiteral("adselect-list-scroll"));
  popupScrollArea_->setFitToWidth(true);
  popupScrollArea_->setFocusPolicy(Qt::NoFocus);
  popupScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  listView_ = new QListView(popupScrollArea_);
  listView_->setObjectName(QStringLiteral("adselect-list"));
  listView_->setModel(listModel_);
  listView_->setItemDelegate(itemDelegateOverride_ ? itemDelegateOverride_.data()
                                                   : new OptionListDelegate(this));
  listView_->setFrameShape(QFrame::NoFrame);
  listView_->setSelectionMode(mode_ == Mode::Single ? QAbstractItemView::SingleSelection
                                                    : QAbstractItemView::MultiSelection);
  listView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  listView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  listView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  listView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  listView_->setSpacing(0);
  listView_->setUniformItemSizes(false);
  listView_->setMouseTracking(true);
  listView_->setFocusPolicy(Qt::StrongFocus);
  listView_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  if (listView_->viewport()) {
    listView_->viewport()->setMouseTracking(true);
    listView_->viewport()->installEventFilter(this);
  }
  listView_->installEventFilter(this);
  popupScrollArea_->setContentWidget(listView_);
  popupLayout_->addWidget(popupScrollArea_);

  connect(listView_, &QListView::clicked, this, [this](const QModelIndex& index) {
    if (!index.isValid()) {
      return;
    }
    const int row = index.row();
    if (row < 0 || row >= rows_.size()) {
      return;
    }
    const ModelRow& modelRow = rows_.at(row);
    if (modelRow.optionIndex < 0 || modelRow.optionIndex >= options_.size()) {
      return;
    }
    toggleSelectionForOption(options_.at(modelRow.optionIndex));
  });

  syncPopupExtraContentWidget();
  applyVisualStyle();
  updateInputMode();
  updateAccessibility();
}

void AdSelect::applyPopupLayerMode() {
  if (!popup_) {
    return;
  }

  QWidget* scopeWindow = detail::resolvePopupScopeWindow(this);
  QWidget* desiredParent = popupLayerMode_ == PopupLayerMode::QtTool ? nullptr : scopeWindow;
  const Qt::WindowFlags desiredFlags =
      popupLayerMode_ == PopupLayerMode::QtTool ? adQtToolWindowFlags() : Qt::Widget;
  const bool useToolWindow = popupLayerMode_ == PopupLayerMode::QtTool;
  popup_->setAttribute(Qt::WA_ShowWithoutActivating, useToolWindow);
  popup_->setAttribute(Qt::WA_TranslucentBackground, useToolWindow);
  popup_->setAttribute(Qt::WA_QuitOnClose, !useToolWindow);
  if (popup_->parentWidget() != desiredParent || popup_->windowFlags() != desiredFlags) {
    popup_->setParent(desiredParent, desiredFlags);
  }
}

void AdSelect::syncTopLevelPopupTooltipRoute() {
  const bool active =
      open_ && popupLayerMode_ == PopupLayerMode::QtTool && popup_ && popup_->isWindow();
  detail::syncTopLevelPopupTooltipRoute(this, this, popup_, active);
}

void AdSelect::rebuildPopupExtraContent() {
  if (!popup_ || !popupLayout_) {
    return;
  }
  syncPopupExtraContentWidget();
}

int AdSelect::popupContentWidthHint() const {
  if (!visualStyle_) {
    return 0;
  }

  const detail::SelectMetrics& metrics = visualStyle_->metrics;
  const int horizontalPadding = std::max(0, metrics.optionPaddingHorizontal);
  const int selectedIconSize = std::max(10, metrics.iconSize);
  const int selectedStateGap = std::max(2, metrics.optionStateGap);

  auto measureRowTextWidth = [horizontalPadding](const QString& text, const QFont& font) {
    const QFontMetrics fm(font);
    const int textWidth = std::max(fm.horizontalAdvance(text), fm.boundingRect(text).width());
    // Leave a tiny safety room to avoid boundary elide caused by font hinting/rounding.
    return textWidth + horizontalPadding * 2 + 2;
  };

  int maxRowWidth = 0;
  for (const ModelRow& row : rows_) {
    if (row.empty) {
      QFont emptyFont = metrics.optionFont;
      emptyFont.setWeight(QFont::Normal);
      emptyFont.setPixelSize(std::max(12, metrics.emptyDescriptionFontSize));

      const QFontMetrics emptyMetrics(emptyFont);
      const int emptyTextWidth = emptyMetrics.horizontalAdvance(row.headerText);
      const int emptyIconWidth = std::max(30, metrics.emptyStateIconWidth);
      const int emptyInlineMargin = std::max(0, metrics.emptyStateMarginInline);
      const int emptyContentWidth =
          std::max(emptyTextWidth, emptyIconWidth) + emptyInlineMargin * 2;
      maxRowWidth = std::max(maxRowWidth, emptyContentWidth + horizontalPadding * 2);
      continue;
    }

    if (row.header) {
      QFont headerFont = metrics.optionFont;
      headerFont.setBold(true);
      maxRowWidth = std::max(maxRowWidth, measureRowTextWidth(row.headerText, headerFont));
      continue;
    }

    if (row.optionIndex < 0 || row.optionIndex >= options_.size()) {
      continue;
    }

    const Option& option = options_.at(row.optionIndex);
    QFont optionFont = metrics.optionFont;
    const bool selected = isValueSelected(option.value);
    if (selected) {
      optionFont.setWeight(QFont::DemiBold);
    }

    int optionWidth = measureRowTextWidth(formattedOptionText(option), optionFont);
    if (selected && mode_ != Mode::Single) {
      optionWidth += selectedIconSize + selectedStateGap;
    }
    maxRowWidth = std::max(maxRowWidth, optionWidth);
  }

  if (maxRowWidth <= 0) {
    maxRowWidth = horizontalPadding * 2;
  }

  QMargins popupMargins =
      popupLayout_ ? popupLayout_->contentsMargins()
                   : QMargins(std::max(0, metrics.popupPadding), std::max(0, metrics.popupPadding),
                              std::max(0, metrics.popupPadding), std::max(0, metrics.popupPadding));
  if (popupLayout_) {
    popupMargins = detail::removeAntPopupShadowMarginsFromPadding(popupMargins);
  }
  return maxRowWidth + popupMargins.left() + popupMargins.right();
}

void AdSelect::syncPopupGeometry() {
  if (!popup_ || !visualStyle_) {
    return;
  }

  int contentHeight = 0;
  if (rows_.isEmpty()) {
    contentHeight = visualStyle_->metrics.optionHeight;
  } else {
    for (const ModelRow& row : rows_) {
      contentHeight +=
          row.empty ? visualStyle_->metrics.emptyStateHeight : visualStyle_->metrics.optionHeight;
    }
  }
  const int listHeight = std::min(visualStyle_->metrics.popupMaxHeight, contentHeight);
  int targetListHeight = std::max(visualStyle_->metrics.optionHeight, listHeight);
  const int targetListContentHeight = std::max(visualStyle_->metrics.optionHeight, contentHeight);
  if (listView_) {
    const int listViewHeight = popupScrollArea_ ? targetListContentHeight : targetListHeight;
    setWidgetFixedHeightIfChanged(listView_, listViewHeight);
    listView_->updateGeometry();
  }
  if (popupScrollArea_) {
    setWidgetFixedHeightIfChanged(popupScrollArea_, targetListHeight);
    popupScrollArea_->updateGeometry();
  } else if (!listView_) {
    targetListHeight = 0;
  }
  if (popupLayout_) {
    popupLayout_->invalidate();
    popupLayout_->activate();
  }

  const QSize popupSizeHint = popup_->sizeHint();
  int popupW = detail::removeAntPopupShadowMargins(popupSizeHint).width();
  if (popupMatchSelectWidth_) {
    popupW = std::max(width(), popupWidth_ > 0 ? popupWidth_ : width());
  } else if (popupWidth_ > 0) {
    popupW = popupWidth_;
  } else {
    popupW = std::max(popupW, popupContentWidthHint());
  }
  const int popupMinWidth = popupWidth_ > 0 ? 1 : 120;
  popupW = std::max(popupMinWidth, popupW);

  int popupH = detail::removeAntPopupShadowMargins(popupSizeHint).height();
  if (popupLayout_ && (popupScrollArea_ || listView_)) {
    const QMargins margins =
        detail::removeAntPopupShadowMarginsFromPadding(popupLayout_->contentsMargins());
    popupH = margins.top() + targetListHeight + margins.bottom();
    // The popup is hidden while its opening geometry is calculated, so an
    // extra widget in the active layout is not yet visible through its parent.
    if (popupExtraContent_) {
      popupH += std::max(0, popupLayout_->spacing());
      const int availableWidth = std::max(0, popupW - margins.left() - margins.right());
      popupH += boundedWidgetHeightHint(popupExtraContent_, availableWidth);
    }
  }
  const QSize visualPopupSize(popupW, std::max(1, popupH));
  popup_->resize(detail::addAntPopupShadowMargins(visualPopupSize));

  const bool useTopLevelToolLayer = popupLayerMode_ == PopupLayerMode::QtTool;
  QWidget* popupParent = popup_->parentWidget();
  QWidget* scopeWindow = detail::resolvePopupScopeWindow(this);
  QWidget* expectedPopupParent = useTopLevelToolLayer ? nullptr : scopeWindow;
  if (popupParent != expectedPopupParent && scopeWindow) {
    QScopedValueRollback<bool> hideGuard(suppressPopupHideClose_, true);
    const bool wasVisible = popup_->isVisible();
    popup_->setParent(expectedPopupParent,
                      useTopLevelToolLayer ? adQtToolWindowFlags() : Qt::Widget);
    popupParent = expectedPopupParent;
    if (wasVisible) {
      popup_->show();
      popup_->raise();
    }
  }
  if (!useTopLevelToolLayer && !popupParent) {
    return;
  }

  detail::PopupPlacementInput placementInput;
  placementInput.anchorTopLeft =
      useTopLevelToolLayer ? mapToGlobal(QPoint(0, 0)) : mapTo(popupParent, QPoint(0, 0));
  placementInput.anchorSize = QSize(width(), height());
  placementInput.popupSize = visualPopupSize;
  const QRect anchorRect(placementInput.anchorTopLeft, placementInput.anchorSize);
  QScreen* toolScreen = nullptr;
  if (useTopLevelToolLayer) {
    toolScreen = detail::popupScreenForGlobalRect(this, anchorRect);
    if (toolScreen && popup_->isWindow() && popup_->screen() != toolScreen) {
      popup_->setScreen(toolScreen);
    }
    detail::syncTopLevelToolTransientParent(popup_, scopeWindow);
  }
  const QRect popupBounds = useTopLevelToolLayer
                                ? (toolScreen ? toolScreen->availableGeometry()
                                              : detail::popupScreenBoundsInGlobal(anchorRect))
                                : QRect(QPoint(0, 0), popupParent->size());
  placementInput.bounds = popupBounds;
  placementInput.preferredPlacement = toPopupPlacement(placement_);

  const detail::PopupPlacementOutput placementOutput =
      detail::resolvePopupPlacement(placementInput);
  QPoint popupTopLeft = placementOutput.topLeft;
  const int popupOffset = std::max(0, visualStyle_->metrics.popupOffset);
  if (popupOffset > 0) {
    switch (placementOutput.placement) {
      case detail::PopupPlacement::BottomLeft:
      case detail::PopupPlacement::BottomRight:
      case detail::PopupPlacement::BottomCenter:
        popupTopLeft.ry() += popupOffset;
        break;
      case detail::PopupPlacement::TopLeft:
      case detail::PopupPlacement::TopRight:
      case detail::PopupPlacement::TopCenter:
        popupTopLeft.ry() -= popupOffset;
        break;
      case detail::PopupPlacement::RightTop:
        popupTopLeft.rx() += popupOffset;
        break;
      case detail::PopupPlacement::LeftTop:
        popupTopLeft.rx() -= popupOffset;
        break;
    }
    popupTopLeft = detail::clampPopupTopLeft(popupTopLeft, visualPopupSize, placementInput.bounds);
  }

  popup_->move(detail::antPopupShadowFrameTopLeftForVisualTopLeft(popupTopLeft));
}

void AdSelect::closePopup() { setOpenInternal(false, true); }

void AdSelect::openPopup() {
  if (disabled()) {
    return;
  }
  if (!open_) {
    emit popupOpening();
  }
  ensurePopup();
  refreshRows();
  rebuildPopupExtraContent();
  syncPopupGeometry();
  setOpenInternal(true, true);
}

void AdSelect::setOpenInternal(bool value, bool emitSignal) {
  if (open_ == value) {
    return;
  }

  open_ = value;

  if (open_) {
    if (popup_) {
      popup_->show();
      popup_->raise();
    }
    detail::setPopupInteractionHostOpen(this, true);
    hasFocusWithin_ = true;
    bumpJoinedZOrder();
    applyVisualStyle();
    if (isSearchEnabledForCurrentMode() && lineEdit_) {
      lineEdit_->setFocus();
      lineEdit_->selectAll();
    } else if (listView_) {
      listView_->setFocus(Qt::PopupFocusReason);
    }
  } else {
    detail::setPopupInteractionHostOpen(this, false);
    if (popup_) {
      popup_->hide();
    }
    if (popupLayerMode_ == PopupLayerMode::QtTool) {
      detail::releaseTopLevelToolResourcesOnHide(popup_);
    }
    if (autoClearSearchValue_ && isSearchEnabledForCurrentMode()) {
      setSearchText(QString());
      if (lineEdit_) {
        suppressLineEditChange_ = true;
        lineEdit_->clear();
        suppressLineEditChange_ = false;
      }
    }
    hasFocusWithin_ = false;
    applyVisualStyle();
  }

  // In filled/multiple mode most content surfaces are transparent. Make sure
  // they repaint when open state changes shell background/border colors.
  update();
  if (contentHost_) {
    contentHost_->update();
  }
  if (tagsContainer_ && tagsContainer_->isVisible()) {
    tagsContainer_->update();
  }
  if (lineEdit_) {
    lineEdit_->update();
  }
  if (placeholderLabel_ && placeholderLabel_->isVisible()) {
    placeholderLabel_->update();
  }

  updateSuffixVisual();
  updateAccessibility();

  if (emitSignal) {
    emit popupVisibleChanged(open_);
  }
  syncTopLevelPopupTooltipRoute();
}

QObject* AdSelect::popupOwnerObject() const { return const_cast<AdSelect*>(this); }

QWidget* AdSelect::popupAnchorWidget() const { return const_cast<AdSelect*>(this); }

QWidget* AdSelect::popupScopeWindow() const { return detail::resolvePopupScopeWindow(this); }

QWidget* AdSelect::popupSurfaceWidget() const { return popup_; }

bool AdSelect::popupIsVisible() const { return open_ && popup_ && popup_->isVisible(); }

bool AdSelect::popupWantsHostFrameRelayout() const {
  return popupLayerMode_ == PopupLayerMode::InWindow;
}

bool AdSelect::popupContainsGlobalPos(const QPoint& globalPos) const {
  return widgetContainsGlobalPos(this, globalPos) || widgetContainsGlobalPos(popup_, globalPos);
}

void AdSelect::popupCloseFromHost(detail::PopupCloseReason reason) {
  Q_UNUSED(reason)
  closePopup();
}

void AdSelect::popupRelayoutFromHost() {
  if (open_) {
    syncPopupGeometry();
  }
}

void AdSelect::updateInteractionFocusOverlay() {
  if (!visualStyle_ || disabled() || !(hasFocusWithin_ || open_)) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const QColor focusColor = visualStyle_->selectorFocusOutlineColor;
  if (focusColor.alpha() <= 0 || visualStyle_->metrics.focusOutlineWidth <= 0.0) {
    stopInteractionFocusForOwner(this);
    return;
  }

  QRectF focusBaseRectInWindow =
      joinedSelectorRect(rect(), visualStyle_->metrics.borderWidth, joinedLeft_, joinedRight_);

  QWidget* hostWindow = window();
  if (hostWindow) {
    const QPoint origin = mapTo(hostWindow, QPoint(0, 0));
    focusBaseRectInWindow.translate(origin.x(), origin.y());
  }

  const qreal radius = resolveSelectorRadius();
  InteractionFocusRequest request;
  request.owner = this;
  request.baseRectInWindow = focusBaseRectInWindow;
  request.topLeft = joinedLeft_ ? 0.0 : radius;
  request.topRight = joinedRight_ ? 0.0 : radius;
  request.bottomRight = joinedRight_ ? 0.0 : radius;
  request.bottomLeft = joinedLeft_ ? 0.0 : radius;
  request.color = focusColor;
  request.strokeWidth = std::max<qreal>(1.0, visualStyle_->metrics.focusOutlineWidth);
  request.offset = std::max<qreal>(0.0, visualStyle_->metrics.focusOutlineOffset);
  triggerInteractionFocus(request);
}

void AdSelect::bumpJoinedZOrder() {
  if (!(joinedLeft_ || joinedRight_)) {
    return;
  }
  if (!(hovered_ || hasFocusWithin_ || open_)) {
    return;
  }
  raise();
}

void AdSelect::updateFocusState() {
  const bool nextFocus = open_ || (lineEdit_ && lineEdit_->hasFocus());
  if (hasFocusWithin_ == nextFocus) {
    if (nextFocus) {
      bumpJoinedZOrder();
    }
    updateInteractionFocusOverlay();
    return;
  }
  hasFocusWithin_ = nextFocus;
  if (hasFocusWithin_) {
    bumpJoinedZOrder();
  }
  updateInteractionFocusOverlay();
  update();
}

}  // namespace adqt::widgets
