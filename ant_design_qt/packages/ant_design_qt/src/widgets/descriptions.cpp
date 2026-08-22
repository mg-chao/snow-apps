#include "descriptions.h"

#include "descriptions_style.h"
#include "theme/theme.h"

#include <QEvent>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace adqt::widgets {

namespace {

using Appearance = detail::DescriptionsAppearance;
using SlotStyle = AdDescriptions::SemanticSlotStyle;

QColor mergeColor(const std::optional<QColor>& value, const QColor& fallback) {
  return value && value->isValid() ? *value : fallback;
}

QFont mergeFont(const std::optional<QFont>& value, const QFont& fallback) {
  return value ? *value : fallback;
}

AdDescriptions::ComponentTokens mergeTokens(AdDescriptions::ComponentTokens base,
                                            const AdDescriptions::ComponentTokens& overlay) {
#define ADQT_MERGE_COLOR(name) \
  if (overlay.colors.name) base.colors.name = overlay.colors.name;
  ADQT_MERGE_COLOR(labelBackground)
  ADQT_MERGE_COLOR(labelColor)
  ADQT_MERGE_COLOR(titleColor)
  ADQT_MERGE_COLOR(contentColor)
  ADQT_MERGE_COLOR(extraColor)
  ADQT_MERGE_COLOR(borderColor)
#undef ADQT_MERGE_COLOR
#define ADQT_MERGE_METRIC(name) \
  if (overlay.metrics.name) base.metrics.name = overlay.metrics.name;
  ADQT_MERGE_METRIC(titleMarginBottom)
  ADQT_MERGE_METRIC(itemPaddingBottom)
  ADQT_MERGE_METRIC(itemPaddingEnd)
  ADQT_MERGE_METRIC(colonMarginLeft)
  ADQT_MERGE_METRIC(colonMarginRight)
  ADQT_MERGE_METRIC(borderedPaddingBlock)
  ADQT_MERGE_METRIC(borderedPaddingInline)
  ADQT_MERGE_METRIC(borderWidth)
  ADQT_MERGE_METRIC(borderRadius)
#undef ADQT_MERGE_METRIC
  return base;
}

AdDescriptions::SemanticSlotStyle mergeSlot(AdDescriptions::SemanticSlotStyle base,
                                            const AdDescriptions::SemanticSlotStyle& overlay) {
  if (overlay.textColor) base.textColor = overlay.textColor;
  if (overlay.backgroundColor) base.backgroundColor = overlay.backgroundColor;
  if (overlay.borderColor) base.borderColor = overlay.borderColor;
  if (overlay.font) base.font = overlay.font;
  return base;
}

AdDescriptions::SemanticSlotStyle inheritTypography(const AdDescriptions::SemanticSlotStyle& parent,
                                                    AdDescriptions::SemanticSlotStyle child) {
  if (!child.textColor) child.textColor = parent.textColor;
  if (!child.font) child.font = parent.font;
  return child;
}

AdDescriptions::SemanticStyles mergeSemantics(AdDescriptions::SemanticStyles base,
                                              const AdDescriptions::SemanticStyles& overlay) {
  base.root = mergeSlot(base.root, overlay.root);
  base.header = mergeSlot(base.header, overlay.header);
  base.title = mergeSlot(base.title, overlay.title);
  base.extra = mergeSlot(base.extra, overlay.extra);
  base.label = mergeSlot(base.label, overlay.label);
  base.content = mergeSlot(base.content, overlay.content);
  return base;
}

AdDescriptions::SemanticStyles resolveSemanticInheritance(AdDescriptions::SemanticStyles styles) {
  styles.header = inheritTypography(styles.root, styles.header);
  styles.title = inheritTypography(styles.header, styles.title);
  styles.extra = inheritTypography(styles.header, styles.extra);
  styles.label = inheritTypography(styles.root, styles.label);
  styles.content = inheritTypography(styles.root, styles.content);
  return styles;
}

class DescriptionLabel final : public QLabel {
 public:
  DescriptionLabel(const QString& text, const QColor& color, const QFont& font,
                   const QString& accessibleRole, QWidget* parent = nullptr)
      : QLabel(text, parent) {
    setWordWrap(true);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setAlignment(Qt::AlignLeading | Qt::AlignTop);
    setFont(font);
    QPalette p = palette();
    p.setColor(QPalette::WindowText, color);
    setPalette(p);
    setAccessibleName(text);
    setAccessibleDescription(accessibleRole);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
  }
};

class LayoutWidget : public QWidget {
 public:
  explicit LayoutWidget(QWidget* parent = nullptr) : QWidget(parent) {
    QSizePolicy policy = sizePolicy();
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
  }

  bool hasHeightForWidth() const override { return layout() && layout()->hasHeightForWidth(); }

  int heightForWidth(int width) const override {
    if (!layout()) return QWidget::heightForWidth(width);
    const int height = layout()->heightForWidth(width);
    return height >= 0 ? height : layout()->sizeHint().height();
  }
};

class Surface final : public LayoutWidget {
 public:
  explicit Surface(QWidget* parent = nullptr) : LayoutWidget(parent) {}

  void setAppearance(const Appearance& value, bool drawsBorder) {
    appearance_ = value;
    drawsBorder_ = drawsBorder;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor background = appearance_.rootBackground;
    const bool paintBorder = drawsBorder_ && appearance_.metrics.borderWidth > 0;
    const qreal inset = paintBorder ? appearance_.metrics.borderWidth / 2.0 : 0.0;
    const QRectF rect = QRectF(this->rect()).adjusted(inset, inset, -inset, -inset);
    painter.setBrush(background);
    painter.setPen(paintBorder ? QPen(appearance_.borderColor, appearance_.metrics.borderWidth)
                               : Qt::NoPen);
    painter.drawRoundedRect(rect, appearance_.metrics.borderRadius,
                            appearance_.metrics.borderRadius);
  }

 private:
  Appearance appearance_;
  bool drawsBorder_ = false;
};

class SemanticFrame final : public LayoutWidget {
 public:
  SemanticFrame(const SlotStyle& style, int borderWidth, QWidget* parent = nullptr)
      : LayoutWidget(parent), style_(style), borderWidth_(std::max(0, borderWidth)) {
    setAutoFillBackground(false);
  }

  int frameWidth() const {
    return style_.borderColor && style_.borderColor->isValid() ? borderWidth_ : 0;
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    if (style_.backgroundColor && style_.backgroundColor->isValid()) {
      painter.fillRect(rect(), *style_.backgroundColor);
    }
    if (frameWidth() <= 0) return;
    const qreal inset = frameWidth() / 2.0;
    painter.setPen(QPen(*style_.borderColor, frameWidth()));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset));
  }

 private:
  SlotStyle style_;
  int borderWidth_ = 0;
};

class Cell final : public LayoutWidget {
 public:
  Cell(const Appearance& appearance, const QColor& background, bool drawBlockEnd,
       bool drawInlineEnd, QWidget* parent = nullptr)
      : LayoutWidget(parent),
        appearance_(appearance),
        background_(background),
        drawBlockEnd_(drawBlockEnd),
        drawInlineEnd_(drawInlineEnd) {
    setAutoFillBackground(false);
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), background_);
    if (appearance_.metrics.borderWidth <= 0) return;
    QPen pen(appearance_.borderColor, appearance_.metrics.borderWidth);
    painter.setPen(pen);
    const qreal offset = appearance_.metrics.borderWidth / 2.0;
    if (drawBlockEnd_) {
      painter.drawLine(QPointF(0, height() - offset), QPointF(width(), height() - offset));
    }
    if (drawInlineEnd_) {
      const qreal x = layoutDirection() == Qt::RightToLeft ? offset : width() - offset;
      painter.drawLine(QPointF(x, 0), QPointF(x, height()));
    }
  }

 private:
  Appearance appearance_;
  QColor background_;
  bool drawBlockEnd_ = false;
  bool drawInlineEnd_ = false;
};

void detachWidget(QWidget* widget, QWidget* owner) {
  if (widget && widget->parentWidget() != owner) {
    widget->hide();
    widget->setParent(owner);
  }
}

void deleteLayoutContents(QLayout* layout) {
  while (QLayoutItem* item = layout->takeAt(0)) {
    delete item->widget();
    if (QLayout* child = item->layout()) {
      deleteLayoutContents(child);
      delete child;
    }
    delete item;
  }
}

struct RowEntry {
  int itemIndex = -1;
  int span = 1;
};

int responsiveValue(const QMap<int, int>& values, int width, int fallback) {
  int result = std::max(1, fallback);
  for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
    if (width >= it.key()) result = std::max(1, it.value());
  }
  return result;
}

QVector<QVector<RowEntry>> calculateRows(const QVector<AdDescriptions::Item>& items, int column,
                                         int width) {
  QVector<QVector<RowEntry>> rows;
  QVector<RowEntry> current;
  int used = 0;
  const int availableColumns = std::max(1, column);
  for (int index = 0; index < items.size(); ++index) {
    const AdDescriptions::Item& item = items.at(index);
    const int remaining = std::max(1, availableColumns - used);
    const int responsiveSpan = responsiveValue(item.responsiveSpans, width, item.span);
    int span = item.fillRemaining ? remaining : responsiveSpan;
    span = std::min(span, remaining);
    current.append({index, span});
    used += span;
    if (item.fillRemaining || used >= availableColumns) {
      rows.append(current);
      current.clear();
      used = 0;
    }
  }
  if (!current.isEmpty()) {
    current.last().span += availableColumns - used;
    rows.append(current);
  }
  return rows;
}

bool isAncestorOrSelf(const QWidget* owner, const QWidget* candidate) {
  if (!owner || !candidate) return false;
  for (const QWidget* cursor = owner; cursor; cursor = cursor->parentWidget()) {
    if (cursor == candidate) return true;
  }
  return false;
}

}  // namespace

struct AdDescriptions::Private {
  enum class OwnedWidgetRole : std::uint8_t {
    Title,
    Extra,
    ItemLabel,
    ItemContent,
  };

  explicit Private(AdDescriptions* owner) : q(owner) {
    root = new QVBoxLayout(q);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
  }

  ComponentTokenContext context() const { return {bordered, size, layoutMode, q->isEnabled()}; }

  ComponentTokens resolvedTokens() const {
    ComponentTokens result = componentTokens;
    if (tokenResolver) result = mergeTokens(result, tokenResolver(context()));
    return result;
  }

  SemanticStyles resolvedSemantics() const {
    SemanticStyles result = semanticStyles;
    if (semanticResolver) result = mergeSemantics(result, semanticResolver(context()));
    return resolveSemanticInheritance(result);
  }

  int calculateEffectiveColumn(int width) const {
    if (responsiveColumns.isEmpty()) return std::max(1, column);
    int result = std::max(1, responsiveColumns.constBegin().value());
    for (auto it = responsiveColumns.constBegin(); it != responsiveColumns.constEnd(); ++it) {
      if (width >= it.key()) result = std::max(1, it.value());
    }
    return result;
  }

  bool widgetInUse(const QWidget* widget, int ignoredIndex = -1) const {
    if (!widget) return false;
    if (widget == titleWidget || widget == extraWidget) return true;
    for (int i = 0; i < items.size(); ++i) {
      if (i == ignoredIndex) continue;
      if (items.at(i).labelWidget == widget || items.at(i).contentWidget == widget) return true;
    }
    return false;
  }

  bool validItem(const Item& item, int ignoredIndex = -1) const {
    if (!item.key.isEmpty()) {
      for (int i = 0; i < items.size(); ++i) {
        if (i != ignoredIndex && items.at(i).key == item.key) return false;
      }
    }
    if (item.labelWidget &&
        (isAncestorOrSelf(q, item.labelWidget) || widgetInUse(item.labelWidget, ignoredIndex))) {
      return false;
    }
    if (item.contentWidget &&
        (item.contentWidget == item.labelWidget || isAncestorOrSelf(q, item.contentWidget) ||
         widgetInUse(item.contentWidget, ignoredIndex))) {
      return false;
    }
    return true;
  }

  int indexForId(quint64 id) const {
    for (int index = 0; index < itemIds.size(); ++index) {
      if (itemIds.at(index) == id) return index;
    }
    return -1;
  }

  void unbindOwnedWidget(QWidget* widget) {
    if (!widget) return;
    const auto it = ownedWidgetConnections.find(widget);
    if (it == ownedWidgetConnections.end()) return;
    QObject::disconnect(it.value());
    ownedWidgetConnections.erase(it);
  }

  void disconnectOwnedWidgets() {
    for (const QMetaObject::Connection& connection : std::as_const(ownedWidgetConnections)) {
      QObject::disconnect(connection);
    }
    ownedWidgetConnections.clear();
  }

  void bindOwnedWidget(QWidget* widget, OwnedWidgetRole role, quint64 itemId = 0) {
    if (!widget) return;
    unbindOwnedWidget(widget);
    ownedWidgetConnections.insert(
        widget,
        QObject::connect(widget, &QObject::destroyed, q, [this, widget, role, itemId](QObject*) {
          ownedWidgetConnections.remove(widget);
          QMetaObject::invokeMethod(
              q,
              [this, role, itemId] {
                switch (role) {
                  case OwnedWidgetRole::Title:
                    if (titleWidget) return;
                    rebuild();
                    emit q->titleWidgetChanged(nullptr);
                    break;
                  case OwnedWidgetRole::Extra:
                    if (extraWidget) return;
                    rebuild();
                    emit q->extraWidgetChanged(nullptr);
                    break;
                  case OwnedWidgetRole::ItemLabel:
                  case OwnedWidgetRole::ItemContent: {
                    const int index = indexForId(itemId);
                    if (index < 0) return;
                    const bool stillPresent =
                        role == OwnedWidgetRole::ItemLabel
                            ? static_cast<bool>(items.at(index).labelWidget)
                            : static_cast<bool>(items.at(index).contentWidget);
                    if (stillPresent) return;
                    rebuild();
                    emit q->itemChanged(index);
                    break;
                  }
                }
              },
              Qt::QueuedConnection);
        }));
  }

  void detachCustomWidgets() {
    detachWidget(titleWidget, q);
    detachWidget(extraWidget, q);
    for (const Item& item : std::as_const(items)) {
      detachWidget(item.labelWidget, q);
      detachWidget(item.contentWidget, q);
    }
  }

  QWidget* textOrWidget(QWidget* custom, const QString& text, const QColor& color,
                        const QFont& font, const QString& role, QWidget* parent) {
    if (custom) {
      custom->setParent(parent);
      custom->show();
      return custom;
    }
    return new DescriptionLabel(text, color, font, role, parent);
  }

  QWidget* frameValue(QWidget* value, const SlotStyle& style, const Appearance& appearance,
                      QWidget* parent) {
    const bool hasBackground = style.backgroundColor && style.backgroundColor->isValid();
    const bool hasBorder =
        style.borderColor && style.borderColor->isValid() && appearance.metrics.borderWidth > 0;
    if (!hasBackground && !hasBorder) return value;

    auto* frame = new SemanticFrame(style, appearance.metrics.borderWidth, parent);
    auto* layout = new QVBoxLayout(frame);
    const int frameWidth = frame->frameWidth();
    layout->setContentsMargins(frameWidth, frameWidth, frameWidth, frameWidth);
    layout->setSpacing(0);
    value->setParent(frame);
    layout->addWidget(value);
    return frame;
  }

  QWidget* makeValue(const Item& item, bool label, const Appearance& appearance,
                     const SemanticStyles& semantics, QWidget* parent) {
    const SlotStyle merged = mergeSlot(label ? semantics.label : semantics.content,
                                       label ? item.labelStyle : item.contentStyle);
    const QColor color =
        mergeColor(merged.textColor, label ? appearance.labelColor : appearance.contentColor);
    const QFont font = mergeFont(merged.font, appearance.metrics.textFont);
    return textOrWidget(
        label ? item.labelWidget.data() : item.contentWidget.data(),
        label ? item.label : item.content, color, font,
        label ? AdDescriptions::tr("Description label")
              : AdDescriptions::tr("Description content"),
        parent);
  }

  QWidget* makeSingleCell(const Item& item, bool label, const Appearance& appearance,
                          const SemanticStyles& semantics, bool blockEnd, bool inlineEnd,
                          bool showColon) {
    const SlotStyle merged = mergeSlot(label ? semantics.label : semantics.content,
                                       label ? item.labelStyle : item.contentStyle);
    const QColor background =
        mergeColor(merged.backgroundColor,
                   label && bordered ? appearance.labelBackground : appearance.rootBackground);
    Appearance cellAppearance = appearance;
    cellAppearance.borderColor = mergeColor(merged.borderColor, appearance.borderColor);
    auto* cell = new Cell(cellAppearance, background, bordered && blockEnd, bordered && inlineEnd);
    auto* layout = new QHBoxLayout(cell);
    const int blockPadding = bordered ? appearance.metrics.borderedPaddingBlock : 0;
    const int inlinePadding = bordered ? appearance.metrics.borderedPaddingInline : 0;
    const int itemEnd = !bordered && inlineEnd ? appearance.metrics.itemPaddingEnd : 0;
    const int left =
        q->layoutDirection() == Qt::RightToLeft ? inlinePadding + itemEnd : inlinePadding;
    const int right =
        q->layoutDirection() == Qt::RightToLeft ? inlinePadding : inlinePadding + itemEnd;
    layout->setContentsMargins(
        left, blockPadding, right,
        blockPadding + (!bordered && blockEnd ? appearance.metrics.itemPaddingBottom : 0));
    layout->setSpacing(0);
    QWidget* value = makeValue(item, label, appearance, semantics, cell);
    layout->addWidget(value, label ? 0 : 1);
    if (label && showColon) {
      auto* colonLabel = new DescriptionLabel(QStringLiteral(":"),
                                              mergeColor(merged.textColor, appearance.labelColor),
                                              mergeFont(merged.font, appearance.metrics.textFont),
                                              AdDescriptions::tr("Label separator"), cell);
      colonLabel->setTextInteractionFlags(Qt::NoTextInteraction);
      layout->addSpacing(appearance.metrics.colonMarginLeft);
      layout->addWidget(colonLabel);
      layout->addSpacing(appearance.metrics.colonMarginRight);
    }
    return cell;
  }

  QWidget* makeCombinedCell(const Item& item, const Appearance& appearance,
                            const SemanticStyles& semantics, bool lastRow, bool lastInRow) {
    auto* cell = new Cell(appearance, appearance.rootBackground, false, false);
    auto* layout = new QHBoxLayout(cell);
    const int itemEnd = lastInRow ? 0 : appearance.metrics.itemPaddingEnd;
    layout->setContentsMargins(q->layoutDirection() == Qt::RightToLeft ? itemEnd : 0, 0,
                               q->layoutDirection() == Qt::RightToLeft ? 0 : itemEnd,
                               lastRow ? 0 : appearance.metrics.itemPaddingBottom);
    layout->setSpacing(0);
    layout->setAlignment(Qt::AlignTop);
    const SlotStyle labelStyle = mergeSlot(semantics.label, item.labelStyle);
    QWidget* labelValue = makeValue(item, true, appearance, semantics, cell);
    layout->addWidget(frameValue(labelValue, labelStyle, appearance, cell));
    if (colon) {
      const SlotStyle merged = mergeSlot(semantics.label, item.labelStyle);
      auto* colonLabel = new DescriptionLabel(QStringLiteral(":"),
                                              mergeColor(merged.textColor, appearance.labelColor),
                                              mergeFont(merged.font, appearance.metrics.textFont),
                                              AdDescriptions::tr("Label separator"), cell);
      colonLabel->setTextInteractionFlags(Qt::NoTextInteraction);
      layout->addSpacing(appearance.metrics.colonMarginLeft);
      layout->addWidget(colonLabel);
      layout->addSpacing(appearance.metrics.colonMarginRight);
    }
    const SlotStyle contentStyle = mergeSlot(semantics.content, item.contentStyle);
    QWidget* contentValue = makeValue(item, false, appearance, semantics, cell);
    layout->addWidget(frameValue(contentValue, contentStyle, appearance, cell), 1);
    return cell;
  }

  bool responsiveItemLayoutChanges(int oldWidth, int newWidth) const {
    return std::any_of(items.cbegin(), items.cend(), [oldWidth, newWidth](const Item& item) {
      return !item.responsiveSpans.isEmpty() &&
             responsiveValue(item.responsiveSpans, oldWidth, item.span) !=
                 responsiveValue(item.responsiveSpans, newWidth, item.span);
    });
  }

  void rebuild() {
    if (rebuilding) {
      rebuildPending = true;
      return;
    }

    QScopedValueRollback<bool> guard(rebuilding, true);
    do {
      rebuildPending = false;
      rebuildOnce();
    } while (rebuildPending);
  }

  void rebuildOnce() {
    effectiveColumn = calculateEffectiveColumn(q->width());
    detachCustomWidgets();
    deleteLayoutContents(root);

    ComponentTokens tokens = resolvedTokens();
    SemanticStyles semantics = resolvedSemantics();
    Appearance appearance = detail::resolveDescriptionsAppearance(q, tokens, semantics);

    QPalette rootPalette = q->palette();
    rootPalette.setColor(QPalette::Window, appearance.rootBackground);
    if (q->palette() != rootPalette) q->setPalette(rootPalette);
    const bool autoFill = semantics.root.backgroundColor.has_value();
    if (q->autoFillBackground() != autoFill) q->setAutoFillBackground(autoFill);

    const bool hasTitle = titleWidget || !title.isEmpty();
    const bool hasExtra = extraWidget || !extra.isEmpty();
    if (hasTitle || hasExtra) {
      auto* header = new SemanticFrame(semantics.header, appearance.metrics.borderWidth, q);
      header->setObjectName(QStringLiteral("adDescriptionsHeader"));
      auto* headerLayout = new QHBoxLayout(header);
      const int headerFrame = header->frameWidth();
      headerLayout->setContentsMargins(headerFrame, headerFrame, headerFrame,
                                       headerFrame + appearance.metrics.titleMarginBottom);
      headerLayout->setSpacing(8);
      if (hasTitle) {
        QWidget* widget = textOrWidget(
            titleWidget, title, mergeColor(semantics.title.textColor, appearance.titleColor),
            mergeFont(semantics.title.font, appearance.metrics.titleFont),
            AdDescriptions::tr("Descriptions title"), header);
        if (!titleWidget) widget->setObjectName(QStringLiteral("adDescriptionsTitle"));
        headerLayout->addWidget(frameValue(widget, semantics.title, appearance, header), 1);
      } else {
        headerLayout->addStretch(1);
      }
      if (hasExtra) {
        QWidget* widget = textOrWidget(extraWidget, extra,
                                       mergeColor(semantics.extra.textColor, appearance.extraColor),
                                       mergeFont(semantics.extra.font, appearance.metrics.textFont),
                                       AdDescriptions::tr("Descriptions extra"), header);
        if (!extraWidget) widget->setObjectName(QStringLiteral("adDescriptionsExtra"));
        headerLayout->addWidget(frameValue(widget, semantics.extra, appearance, header), 0,
                                Qt::AlignTop | Qt::AlignTrailing);
      }
      root->addWidget(header);
    }

    auto* surface = new Surface(q);
    surface->setObjectName(QStringLiteral("adDescriptionsView"));
    surface->setAppearance(appearance, bordered);
    auto* grid = new QGridLayout(surface);
    grid->setContentsMargins(bordered ? appearance.metrics.borderWidth : 0,
                             bordered ? appearance.metrics.borderWidth : 0,
                             bordered ? appearance.metrics.borderWidth : 0,
                             bordered ? appearance.metrics.borderWidth : 0);
    grid->setHorizontalSpacing(0);
    grid->setVerticalSpacing(0);

    const QVector<QVector<RowEntry>> rows = calculateRows(items, effectiveColumn, q->width());
    const int trackCount = effectiveColumn * 2;
    for (int track = 0; track < trackCount; ++track) {
      const bool naturalLabelTrack =
          bordered && layoutMode == LayoutMode::Horizontal && track % 2 == 0;
      grid->setColumnStretch(track, naturalLabelTrack ? 0 : 1);
    }

    int gridRow = 0;
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
      const QVector<RowEntry>& row = rows.at(rowIndex);
      const bool lastLogicalRow = rowIndex == rows.size() - 1;
      int start = 0;
      if (layoutMode == LayoutMode::Horizontal && !bordered) {
        for (int entryIndex = 0; entryIndex < row.size(); ++entryIndex) {
          const RowEntry& entry = row.at(entryIndex);
          grid->addWidget(makeCombinedCell(items.at(entry.itemIndex), appearance, semantics,
                                           lastLogicalRow, entryIndex == row.size() - 1),
                          gridRow, start * 2, 1, entry.span * 2);
          start += entry.span;
        }
        ++gridRow;
        continue;
      }

      if (layoutMode == LayoutMode::Horizontal) {
        for (int entryIndex = 0; entryIndex < row.size(); ++entryIndex) {
          const RowEntry& entry = row.at(entryIndex);
          const bool lastEntry = entryIndex == row.size() - 1;
          grid->addWidget(makeSingleCell(items.at(entry.itemIndex), true, appearance, semantics,
                                         !lastLogicalRow, true, false),
                          gridRow, start * 2, 1, 1);
          grid->addWidget(makeSingleCell(items.at(entry.itemIndex), false, appearance, semantics,
                                         !lastLogicalRow, !lastEntry, false),
                          gridRow, start * 2 + 1, 1, entry.span * 2 - 1);
          start += entry.span;
        }
        ++gridRow;
        continue;
      }

      start = 0;
      for (int entryIndex = 0; entryIndex < row.size(); ++entryIndex) {
        const RowEntry& entry = row.at(entryIndex);
        const bool lastEntry = entryIndex == row.size() - 1;
        grid->addWidget(makeSingleCell(items.at(entry.itemIndex), true, appearance, semantics, true,
                                       !lastEntry, !bordered && colon),
                        gridRow, start * 2, 1, entry.span * 2);
        start += entry.span;
      }
      ++gridRow;
      start = 0;
      for (int entryIndex = 0; entryIndex < row.size(); ++entryIndex) {
        const RowEntry& entry = row.at(entryIndex);
        const bool lastEntry = entryIndex == row.size() - 1;
        grid->addWidget(makeSingleCell(items.at(entry.itemIndex), false, appearance, semantics,
                                       !lastLogicalRow, !lastEntry, false),
                        gridRow, start * 2, 1, entry.span * 2);
        start += entry.span;
      }
      ++gridRow;
    }
    root->addWidget(surface);
    surface->setVisible(!items.isEmpty());
    const QString nextAccessibleName =
        title.trimmed().isEmpty() ? AdDescriptions::tr("Descriptions") : title.trimmed();
    QStringList accessibilityParts;
    for (const Item& item : std::as_const(items)) {
      const auto accessibleText = [](const QPointer<QWidget>& widget, const QString& fallback) {
        if (!widget) return fallback.trimmed();
        if (!widget->accessibleName().trimmed().isEmpty()) {
          return widget->accessibleName().trimmed();
        }
        if (const auto* label = qobject_cast<const QLabel*>(widget.data())) {
          return label->text().trimmed();
        }
        return fallback.trimmed();
      };
      const QString labelText = accessibleText(item.labelWidget, item.label);
      const QString contentText = accessibleText(item.contentWidget, item.content);
      if (!labelText.isEmpty() && !contentText.isEmpty()) {
        accessibilityParts.append(QStringLiteral("%1: %2").arg(labelText, contentText));
      } else if (!labelText.isEmpty()) {
        accessibilityParts.append(labelText);
      } else if (!contentText.isEmpty()) {
        accessibilityParts.append(contentText);
      }
    }
    const QString nextAccessibleDescription = accessibilityParts.join(QStringLiteral("; "));
    if (q->accessibleName().isEmpty() || q->accessibleName() == generatedAccessibleName) {
      q->setAccessibleName(nextAccessibleName);
      generatedAccessibleName = nextAccessibleName;
    }
    if (q->accessibleDescription().isEmpty() ||
        q->accessibleDescription() == generatedAccessibleDescription) {
      q->setAccessibleDescription(nextAccessibleDescription);
      generatedAccessibleDescription = nextAccessibleDescription;
    }
    q->updateGeometry();
    q->update();
  }

  AdDescriptions* q = nullptr;
  QVBoxLayout* root = nullptr;
  QVector<Item> items;
  QVector<quint64> itemIds;
  quint64 nextItemId = 1;
  QHash<QWidget*, QMetaObject::Connection> ownedWidgetConnections;
  bool bordered = false;
  Size size = Size::Default;
  LayoutMode layoutMode = LayoutMode::Horizontal;
  int column = 3;
  int effectiveColumn = 3;
  QMap<int, int> responsiveColumns{{0, 1}, {576, 2}, {768, 3}};
  bool colon = true;
  QString title;
  QString extra;
  QPointer<QWidget> titleWidget;
  QPointer<QWidget> extraWidget;
  ComponentTokens componentTokens;
  ComponentTokenResolver tokenResolver;
  SemanticStyles semanticStyles;
  SemanticStyleResolver semanticResolver;
  bool rebuilding = false;
  bool rebuildPending = false;
  QString generatedAccessibleName;
  QString generatedAccessibleDescription;
};

AdDescriptions::AdDescriptions(QWidget* parent)
    : QWidget(parent), d_(std::make_unique<Private>(this)) {
  QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Minimum);
  policy.setHeightForWidth(true);
  setSizePolicy(policy);
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this] { d_->rebuild(); });
  d_->rebuild();
}

AdDescriptions::~AdDescriptions() { d_->disconnectOwnedWidgets(); }

int AdDescriptions::count() const { return static_cast<int>(d_->items.size()); }

AdDescriptions::Item AdDescriptions::itemAt(int index) const {
  return index >= 0 && index < count() ? d_->items.at(index) : Item{};
}

int AdDescriptions::indexOf(const QString& key) const {
  if (key.isEmpty()) return -1;
  for (int i = 0; i < count(); ++i) {
    if (d_->items.at(i).key == key) return i;
  }
  return -1;
}

int AdDescriptions::indexOf(const QWidget* widget) const {
  if (!widget) return -1;
  for (int i = 0; i < count(); ++i) {
    if (d_->items.at(i).labelWidget == widget || d_->items.at(i).contentWidget == widget) return i;
  }
  return -1;
}

int AdDescriptions::addItem(const QString& label, const QString& content, int span) {
  Item item;
  item.label = label;
  item.content = content;
  item.span = span;
  return addItem(item);
}

int AdDescriptions::addItem(const Item& item) { return insertItem(count(), item); }

int AdDescriptions::insertItem(int index, const Item& item) {
  if (index < 0 || index > count() || !d_->validItem(item)) return -1;
  Item normalized = item;
  normalized.span = std::max(1, normalized.span);
  if (normalized.labelWidget) normalized.labelWidget->setParent(this);
  if (normalized.contentWidget) normalized.contentWidget->setParent(this);
  const quint64 itemId = d_->nextItemId++;
  d_->items.insert(index, normalized);
  d_->itemIds.insert(index, itemId);
  d_->bindOwnedWidget(normalized.labelWidget, Private::OwnedWidgetRole::ItemLabel, itemId);
  d_->bindOwnedWidget(normalized.contentWidget, Private::OwnedWidgetRole::ItemContent, itemId);
  d_->rebuild();
  emit itemAdded(index);
  emit countChanged(count());
  return index;
}

bool AdDescriptions::updateItem(int index, const Item& item) {
  if (index < 0 || index >= count() || !d_->validItem(item, index)) return false;
  const Item previous = d_->items.at(index);
  Item normalized = item;
  normalized.span = std::max(1, normalized.span);
  d_->unbindOwnedWidget(previous.labelWidget);
  d_->unbindOwnedWidget(previous.contentWidget);
  if (normalized.labelWidget) normalized.labelWidget->setParent(this);
  if (normalized.contentWidget) normalized.contentWidget->setParent(this);
  d_->items[index] = normalized;
  if (previous.labelWidget && previous.labelWidget != normalized.labelWidget &&
      previous.labelWidget != normalized.contentWidget) {
    previous.labelWidget->deleteLater();
  }
  if (previous.contentWidget && previous.contentWidget != normalized.labelWidget &&
      previous.contentWidget != normalized.contentWidget) {
    previous.contentWidget->deleteLater();
  }
  const quint64 itemId = d_->itemIds.at(index);
  d_->bindOwnedWidget(normalized.labelWidget, Private::OwnedWidgetRole::ItemLabel, itemId);
  d_->bindOwnedWidget(normalized.contentWidget, Private::OwnedWidgetRole::ItemContent, itemId);
  d_->rebuild();
  emit itemChanged(index);
  return true;
}

void AdDescriptions::removeItem(int index) {
  if (index < 0 || index >= count()) return;
  Item item = takeItem(index);
  if (item.labelWidget) item.labelWidget->deleteLater();
  if (item.contentWidget) item.contentWidget->deleteLater();
}

void AdDescriptions::removeItem(const QString& key) { removeItem(indexOf(key)); }

AdDescriptions::Item AdDescriptions::takeItem(int index) {
  if (index < 0 || index >= count()) return {};
  d_->detachCustomWidgets();
  Item item = d_->items.takeAt(index);
  d_->itemIds.removeAt(index);
  d_->unbindOwnedWidget(item.labelWidget);
  d_->unbindOwnedWidget(item.contentWidget);
  if (item.labelWidget) item.labelWidget->setParent(nullptr);
  if (item.contentWidget) item.contentWidget->setParent(nullptr);
  d_->rebuild();
  emit itemRemoved(index);
  emit countChanged(count());
  return item;
}

void AdDescriptions::clear() {
  while (!d_->items.isEmpty()) removeItem(static_cast<int>(d_->items.size()) - 1);
}

void AdDescriptions::setItemLabel(int index, const QString& label) {
  if (index < 0 || index >= count() || d_->items[index].label == label) return;
  d_->items[index].label = label;
  d_->rebuild();
  emit itemChanged(index);
}

void AdDescriptions::setItemContent(int index, const QString& content) {
  if (index < 0 || index >= count() || d_->items[index].content == content) return;
  d_->items[index].content = content;
  d_->rebuild();
  emit itemChanged(index);
}

void AdDescriptions::setItemSpan(int index, int span) {
  if (index < 0 || index >= count()) return;
  span = std::max(1, span);
  if (d_->items[index].span == span) return;
  d_->items[index].span = span;
  d_->rebuild();
  emit itemChanged(index);
}

void AdDescriptions::setItemResponsiveSpans(int index, const QMap<int, int>& spans) {
  if (index < 0 || index >= count()) return;
  QMap<int, int> normalized;
  for (auto it = spans.constBegin(); it != spans.constEnd(); ++it) {
    normalized.insert(std::max(0, it.key()), std::max(1, it.value()));
  }
  if (d_->items[index].responsiveSpans == normalized) return;
  d_->items[index].responsiveSpans = normalized;
  d_->rebuild();
  emit itemChanged(index);
}

void AdDescriptions::setItemFillRemaining(int index, bool fill) {
  if (index < 0 || index >= count() || d_->items[index].fillRemaining == fill) return;
  d_->items[index].fillRemaining = fill;
  d_->rebuild();
  emit itemChanged(index);
}

void AdDescriptions::setItemLabelWidget(int index, QWidget* widget) {
  if (index < 0 || index >= count() || d_->items[index].labelWidget == widget) return;
  Item item = d_->items.at(index);
  item.labelWidget = widget;
  updateItem(index, item);
}

void AdDescriptions::setItemContentWidget(int index, QWidget* widget) {
  if (index < 0 || index >= count() || d_->items[index].contentWidget == widget) return;
  Item item = d_->items.at(index);
  item.contentWidget = widget;
  updateItem(index, item);
}

QWidget* AdDescriptions::takeItemLabelWidget(int index) {
  if (index < 0 || index >= count() || !d_->items[index].labelWidget) return nullptr;
  d_->detachCustomWidgets();
  QWidget* widget = d_->items[index].labelWidget;
  d_->items[index].labelWidget = nullptr;
  widget->setParent(nullptr);
  d_->rebuild();
  emit itemChanged(index);
  return widget;
}

QWidget* AdDescriptions::takeItemContentWidget(int index) {
  if (index < 0 || index >= count() || !d_->items[index].contentWidget) return nullptr;
  d_->detachCustomWidgets();
  QWidget* widget = d_->items[index].contentWidget;
  d_->items[index].contentWidget = nullptr;
  widget->setParent(nullptr);
  d_->rebuild();
  emit itemChanged(index);
  return widget;
}

bool AdDescriptions::bordered() const { return d_->bordered; }
void AdDescriptions::setBordered(bool value) {
  if (d_->bordered == value) return;
  d_->bordered = value;
  d_->rebuild();
  emit borderedChanged(value);
}

AdDescriptions::Size AdDescriptions::descriptionSize() const { return d_->size; }
void AdDescriptions::setDescriptionSize(Size value) {
  if (d_->size == value) return;
  d_->size = value;
  d_->rebuild();
  emit sizeChanged(value);
}
void AdDescriptions::setSize(Size value) { setDescriptionSize(value); }

AdDescriptions::LayoutMode AdDescriptions::layoutMode() const { return d_->layoutMode; }
void AdDescriptions::setLayoutMode(LayoutMode value) {
  if (d_->layoutMode == value) return;
  d_->layoutMode = value;
  d_->rebuild();
  emit layoutModeChanged(value);
}

int AdDescriptions::column() const { return d_->column; }
void AdDescriptions::setColumn(int value) {
  value = std::max(1, value);
  const bool columnValueChanged = d_->column != value;
  const bool mapChanged = !d_->responsiveColumns.isEmpty();
  if (!columnValueChanged && !mapChanged) return;
  d_->column = value;
  d_->responsiveColumns.clear();
  const int previousEffective = d_->effectiveColumn;
  d_->effectiveColumn = value;
  d_->rebuild();
  if (columnValueChanged) emit columnChanged(value);
  if (mapChanged) emit responsiveColumnsChanged();
  if (previousEffective != value) emit effectiveColumnChanged(value);
}

int AdDescriptions::effectiveColumn() const { return d_->calculateEffectiveColumn(width()); }
QMap<int, int> AdDescriptions::responsiveColumns() const { return d_->responsiveColumns; }
void AdDescriptions::setResponsiveColumns(const QMap<int, int>& columns) {
  QMap<int, int> normalized;
  for (auto it = columns.constBegin(); it != columns.constEnd(); ++it) {
    normalized.insert(std::max(0, it.key()), std::max(1, it.value()));
  }
  if (normalized == d_->responsiveColumns) return;
  d_->responsiveColumns = normalized;
  const int previous = d_->effectiveColumn;
  d_->effectiveColumn = d_->calculateEffectiveColumn(width());
  d_->rebuild();
  emit responsiveColumnsChanged();
  if (previous != d_->effectiveColumn) emit effectiveColumnChanged(d_->effectiveColumn);
}

void AdDescriptions::resetResponsiveColumns() {
  setResponsiveColumns({{0, 1}, {576, 2}, {768, 3}});
}

bool AdDescriptions::colon() const { return d_->colon; }
void AdDescriptions::setColon(bool value) {
  if (d_->colon == value) return;
  d_->colon = value;
  d_->rebuild();
  emit colonChanged(value);
}

QString AdDescriptions::title() const { return d_->title; }
void AdDescriptions::setTitle(const QString& value) {
  if (d_->title == value) return;
  d_->title = value;
  d_->rebuild();
  emit titleChanged(value);
}

QString AdDescriptions::extra() const { return d_->extra; }
void AdDescriptions::setExtra(const QString& value) {
  if (d_->extra == value) return;
  d_->extra = value;
  d_->rebuild();
  emit extraChanged(value);
}

QWidget* AdDescriptions::titleWidget() const { return d_->titleWidget; }
void AdDescriptions::setTitleWidget(QWidget* widget) {
  if (d_->titleWidget == widget ||
      (widget && (isAncestorOrSelf(this, widget) || d_->widgetInUse(widget))))
    return;
  if (d_->titleWidget) {
    d_->unbindOwnedWidget(d_->titleWidget);
    d_->titleWidget->deleteLater();
  }
  d_->titleWidget = widget;
  if (widget) {
    widget->setParent(this);
    d_->bindOwnedWidget(widget, Private::OwnedWidgetRole::Title);
  }
  d_->rebuild();
  emit titleWidgetChanged(widget);
}

QWidget* AdDescriptions::takeTitleWidget() {
  if (!d_->titleWidget) return nullptr;
  d_->detachCustomWidgets();
  QWidget* widget = d_->titleWidget;
  d_->titleWidget = nullptr;
  d_->unbindOwnedWidget(widget);
  widget->setParent(nullptr);
  d_->rebuild();
  emit titleWidgetChanged(nullptr);
  return widget;
}

QWidget* AdDescriptions::extraWidget() const { return d_->extraWidget; }
void AdDescriptions::setExtraWidget(QWidget* widget) {
  if (d_->extraWidget == widget ||
      (widget && (isAncestorOrSelf(this, widget) || d_->widgetInUse(widget))))
    return;
  if (d_->extraWidget) {
    d_->unbindOwnedWidget(d_->extraWidget);
    d_->extraWidget->deleteLater();
  }
  d_->extraWidget = widget;
  if (widget) {
    widget->setParent(this);
    d_->bindOwnedWidget(widget, Private::OwnedWidgetRole::Extra);
  }
  d_->rebuild();
  emit extraWidgetChanged(widget);
}

QWidget* AdDescriptions::takeExtraWidget() {
  if (!d_->extraWidget) return nullptr;
  d_->detachCustomWidgets();
  QWidget* widget = d_->extraWidget;
  d_->extraWidget = nullptr;
  d_->unbindOwnedWidget(widget);
  widget->setParent(nullptr);
  d_->rebuild();
  emit extraWidgetChanged(nullptr);
  return widget;
}

AdDescriptions::ComponentTokens AdDescriptions::componentTokens() const {
  return d_->componentTokens;
}
void AdDescriptions::setComponentTokens(const ComponentTokens& value) {
  d_->componentTokens = value;
  d_->rebuild();
  emit componentTokensChanged();
}
void AdDescriptions::resetComponentTokens() { setComponentTokens({}); }
void AdDescriptions::setComponentTokenResolver(ComponentTokenResolver resolver) {
  d_->tokenResolver = std::move(resolver);
  d_->rebuild();
  emit componentTokensChanged();
}
void AdDescriptions::resetComponentTokenResolver() { setComponentTokenResolver({}); }

AdDescriptions::SemanticStyles AdDescriptions::semanticStyles() const { return d_->semanticStyles; }
void AdDescriptions::setSemanticStyles(const SemanticStyles& value) {
  d_->semanticStyles = value;
  d_->rebuild();
  emit semanticStylesChanged();
}
void AdDescriptions::resetSemanticStyles() { setSemanticStyles({}); }
void AdDescriptions::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  d_->semanticResolver = std::move(resolver);
  d_->rebuild();
  emit semanticStylesChanged();
}
void AdDescriptions::resetSemanticStyleResolver() { setSemanticStyleResolver({}); }

QSize AdDescriptions::sizeHint() const {
  return d_->root ? d_->root->sizeHint() : QWidget::sizeHint();
}

QSize AdDescriptions::minimumSizeHint() const {
  return d_->root ? d_->root->minimumSize() : QWidget::minimumSizeHint();
}

bool AdDescriptions::hasHeightForWidth() const { return d_->root && d_->root->hasHeightForWidth(); }

int AdDescriptions::heightForWidth(int width) const {
  if (!d_->root) return QWidget::heightForWidth(width);
  const int height = d_->root->heightForWidth(width);
  return height >= 0 ? height : sizeHint().height();
}

void AdDescriptions::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event || !d_ || d_->rebuilding) return;
  switch (event->type()) {
    case QEvent::ApplicationPaletteChange:
    case QEvent::ApplicationFontChange:
    case QEvent::PaletteChange:
    case QEvent::FontChange:
    case QEvent::StyleChange:
    case QEvent::EnabledChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::LanguageChange:
      d_->rebuild();
      break;
    default:
      break;
  }
}

void AdDescriptions::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  const int next = d_->calculateEffectiveColumn(event->size().width());
  if (next != d_->effectiveColumn) {
    d_->effectiveColumn = next;
    d_->rebuild();
    emit effectiveColumnChanged(next);
    return;
  }
  if (d_->responsiveItemLayoutChanges(event->oldSize().width(), event->size().width())) {
    d_->rebuild();
  }
}

}  // namespace adqt::widgets
