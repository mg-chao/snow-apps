#pragma once

#include <QColor>
#include <QItemSelectionModel>
#include <QModelIndex>
#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QStyledItemDelegate>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

#include "icon_core.h"
#include "popup_interaction_host.h"

class QAbstractItemDelegate;
class QAbstractItemModel;
class QHideEvent;
class QItemSelectionModel;
class QPainter;
class QShowEvent;
class QStyleOptionViewItem;

namespace adqt::widgets {

class AdNavigationMenu;

struct AdNavigationMenuPopupContext {
  QModelIndex submenuIndex;
};

class AdNavigationMenuPopupFactory : public QObject {
  Q_OBJECT

 public:
  explicit AdNavigationMenuPopupFactory(QObject* parent = nullptr);
  ~AdNavigationMenuPopupFactory() override;

  virtual QWidget* createPopup(const AdNavigationMenuPopupContext& context, QWidget* defaultPopup,
                               QWidget* parent);
};

class AdNavigationMenuItemDelegate : public QStyledItemDelegate {
  Q_OBJECT

 public:
  explicit AdNavigationMenuItemDelegate(AdNavigationMenu* owner, QObject* parent = nullptr);
  ~AdNavigationMenuItemDelegate() override;

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override;

 protected:
  AdNavigationMenu* owner() const;

 private:
  QPointer<AdNavigationMenu> owner_;
};

namespace detail {
class AdMenuBarProxyModel;
class AdMenuBarView;
struct MenuVisualStyle;
class AdMenuPopupShell;
class AdMenuTreeView;
}  // namespace detail

class AdNavigationMenu final : public QWidget, private detail::PopupInteractionOwner {
  Q_OBJECT

  Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)
  Q_PROPERTY(
      ColorScheme colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
  Q_PROPERTY(bool collapsed READ collapsed WRITE setCollapsed NOTIFY collapsedChanged)
  Q_PROPERTY(int indentation READ indentation WRITE setIndentation NOTIFY indentationChanged)
  Q_PROPERTY(SelectionMode selectionMode READ selectionMode WRITE setSelectionMode NOTIFY
                 selectionModeChanged)
  Q_PROPERTY(TriggerSubMenuAction submenuTrigger READ submenuTrigger WRITE setSubmenuTrigger NOTIFY
                 submenuTriggerChanged)
  Q_PROPERTY(int submenuOpenDelayMs READ submenuOpenDelayMs WRITE setSubmenuOpenDelayMs NOTIFY
                 submenuOpenDelayMsChanged)
  Q_PROPERTY(int submenuCloseDelayMs READ submenuCloseDelayMs WRITE setSubmenuCloseDelayMs NOTIFY
                 submenuCloseDelayMsChanged)
  Q_PROPERTY(
      bool tooltipEnabled READ tooltipEnabled WRITE setTooltipEnabled NOTIFY tooltipEnabledChanged)
  Q_PROPERTY(QAbstractItemDelegate* itemDelegate READ itemDelegate WRITE setItemDelegate NOTIFY
                 itemDelegateChanged)
  Q_PROPERTY(adqt::widgets::AdNavigationMenuPopupFactory* popupFactory READ popupFactory WRITE
                 setPopupFactory NOTIFY popupFactoryChanged)
  Q_PROPERTY(QAbstractItemModel* model READ model WRITE setModel NOTIFY modelChanged)
  Q_PROPERTY(QItemSelectionModel* selectionModel READ selectionModel WRITE setSelectionModel NOTIFY
                 selectionModelChanged)
  Q_PROPERTY(
      QModelIndex currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)

 public:
  enum class Mode {
    Vertical,
    Horizontal,
    Inline,
  };
  Q_ENUM(Mode)

  enum class ColorScheme {
    Inherit,
    Light,
    Dark,
  };
  Q_ENUM(ColorScheme)

  enum class SelectionMode {
    NoSelection,
    SingleSelection,
    MultiSelection,
  };
  Q_ENUM(SelectionMode)

  enum class TriggerSubMenuAction {
    Hover,
    Click,
  };
  Q_ENUM(TriggerSubMenuAction)

  enum class NodeKind {
    Action,
    Group,
    Separator,
  };
  Q_ENUM(NodeKind)

  enum ItemDataRole {
    NodeKindRole = Qt::UserRole + 1,
    ExtraTextRole,
    DangerRole,
    PopupColorSchemeRole,
    StableIdRole,
    DashedRole,
    PopupOffsetRole,
  };
  Q_ENUM(ItemDataRole)

  struct MetricTokens {
    std::optional<int> itemHeight;
    std::optional<int> itemPaddingInline;
    std::optional<int> itemMarginInline;
    std::optional<int> itemMarginBlock;
    std::optional<int> rootPaddingBlockStart;
    std::optional<int> itemBorderRadius;
    std::optional<int> horizontalItemBorderRadius;
    std::optional<int> subMenuItemBorderRadius;
    std::optional<int> indentation;
    std::optional<int> inlineIndent;
    std::optional<int> iconSize;
    std::optional<int> iconMarginInlineEnd;
    std::optional<int> activeBarWidth;
    std::optional<int> borderWidth;
    std::optional<int> groupTitleFontSize;
    std::optional<int> groupTitleLineHeight;
  };

  struct SharedColorTokens {
    std::optional<QColor> popupBackground;
    std::optional<QColor> itemText;
    std::optional<QColor> itemBackground;
    std::optional<QColor> subMenuItemBackground;
    std::optional<QColor> groupTitleText;
    std::optional<QColor> itemHoverBackground;
    std::optional<QColor> itemHoverText;
    std::optional<QColor> itemDisabledText;
    std::optional<QColor> subMenuItemSelectedText;
    std::optional<QColor> horizontalItemHoverBackground;
    std::optional<QColor> horizontalItemHoverText;
    std::optional<QColor> horizontalItemSelectedBackground;
    std::optional<QColor> horizontalItemSelectedText;
    std::optional<QColor> dangerItemText;
    std::optional<QColor> dangerItemHoverText;
    std::optional<QColor> dangerItemSelectedText;
    std::optional<QColor> dangerItemActiveBackground;
    std::optional<QColor> dangerItemSelectedBackground;
    std::optional<QColor> itemActiveBackground;
    std::optional<QColor> itemSelectedBackground;
    std::optional<QColor> itemSelectedText;
  };

  struct SchemeColorTokens {
    std::optional<QColor> popupBackground;
    std::optional<QColor> itemText;
    std::optional<QColor> itemBackground;
    std::optional<QColor> subMenuItemBackground;
    std::optional<QColor> groupTitleText;
    std::optional<QColor> itemHoverBackground;
    std::optional<QColor> itemHoverText;
    std::optional<QColor> itemDisabledText;
    std::optional<QColor> subMenuItemSelectedText;
    std::optional<QColor> horizontalItemHoverBackground;
    std::optional<QColor> horizontalItemHoverText;
    std::optional<QColor> horizontalItemSelectedBackground;
    std::optional<QColor> horizontalItemSelectedText;
    std::optional<QColor> dangerItemText;
    std::optional<QColor> dangerItemHoverText;
    std::optional<QColor> dangerItemSelectedText;
    std::optional<QColor> dangerItemActiveBackground;
    std::optional<QColor> dangerItemSelectedBackground;
    std::optional<QColor> itemActiveBackground;
    std::optional<QColor> itemSelectedBackground;
    std::optional<QColor> itemSelectedText;
  };

  struct ColorTokens {
    SharedColorTokens shared;
    SchemeColorTokens light;
    SchemeColorTokens dark;
  };

  struct ComponentTokens {
    MetricTokens metrics;
    ColorTokens colors;
  };

  struct ResolvedColorTokens {
    QColor itemBackground;
    QColor subMenuItemBackground;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle item;
    SemanticSlotStyle itemTitle;
    SemanticSlotStyle list;
    SemanticSlotStyle itemIcon;
    SemanticSlotStyle itemContent;
    SemanticSlotStyle popup;
    SemanticSlotStyle subMenuItem;
    SemanticSlotStyle subMenuItemTitle;
    SemanticSlotStyle subMenuList;
    SemanticSlotStyle subMenuItemIcon;
    SemanticSlotStyle subMenuItemContent;
  };

  struct StyleContext {
    const QAbstractItemModel* model = nullptr;
    const QItemSelectionModel* selectionModel = nullptr;
    Mode mode = Mode::Vertical;
    ColorScheme colorScheme = ColorScheme::Light;
    bool collapsed = false;
    bool popupVisible = false;
  };

  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;

  explicit AdNavigationMenu(QWidget* parent = nullptr);
  ~AdNavigationMenu() override;

  Mode mode() const;
  void setMode(Mode value);

  ColorScheme colorScheme() const;
  void setColorScheme(ColorScheme value);

  bool collapsed() const;
  void setCollapsed(bool value);

  int indentation() const;
  void setIndentation(int value);

  int indent() const;
  void setIndent(int value);

  SelectionMode selectionMode() const;
  void setSelectionMode(SelectionMode value);

  TriggerSubMenuAction submenuTrigger() const;
  void setSubmenuTrigger(TriggerSubMenuAction value);

  int submenuOpenDelayMs() const;
  void setSubmenuOpenDelayMs(int value);
  int subMenuOpenDelayMs() const;
  void setSubMenuOpenDelayMs(int value);

  int submenuCloseDelayMs() const;
  void setSubmenuCloseDelayMs(int value);
  int subMenuCloseDelayMs() const;
  void setSubMenuCloseDelayMs(int value);

  bool tooltipEnabled() const;
  void setTooltipEnabled(bool value);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  ResolvedColorTokens resolvedColorTokens() const;
  static ResolvedColorTokens resolveColorTokens(const QWidget* context = nullptr,
                                                ColorScheme colorScheme = ColorScheme::Inherit);

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);

  adqt::icons::IconRef expandIcon() const;
  void setExpandIcon(const adqt::icons::IconRef& icon);

  QPoint popupOffset() const;
  void setPopupOffset(const QPoint& value);

  QAbstractItemDelegate* itemDelegate() const;
  void setItemDelegate(QAbstractItemDelegate* delegate);

  AdNavigationMenuPopupFactory* popupFactory() const;
  void setPopupFactory(AdNavigationMenuPopupFactory* factory);

  QAbstractItemModel* model() const;
  void setModel(QAbstractItemModel* model);

  QItemSelectionModel* selectionModel() const;
  void setSelectionModel(QItemSelectionModel* model);

  QModelIndex currentIndex() const;
  void setCurrentIndex(const QModelIndex& index);

  bool isExpanded(const QModelIndex& index) const;
  void setExpanded(const QModelIndex& index, bool expanded);
  void expand(const QModelIndex& index);
  void collapse(const QModelIndex& index);
  void expandAll();
  void collapseAll();

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void modeChanged(Mode value);
  void colorSchemeChanged(ColorScheme value);
  void collapsedChanged(bool value);
  void indentationChanged(int value);
  void indentChanged(int value);
  void selectionModeChanged(SelectionMode value);
  void submenuTriggerChanged(TriggerSubMenuAction value);
  void submenuOpenDelayMsChanged(int value);
  void submenuCloseDelayMsChanged(int value);
  void subMenuOpenDelayMsChanged(int value);
  void subMenuCloseDelayMsChanged(int value);
  void tooltipEnabledChanged(bool value);
  void componentTokensChanged();
  void semanticStylesChanged();
  void expandIconChanged(const adqt::icons::IconRef& icon);
  void popupOffsetChanged(const QPoint& value);
  void itemDelegateChanged(QAbstractItemDelegate* delegate);
  void popupFactoryChanged(adqt::widgets::AdNavigationMenuPopupFactory* factory);
  void modelChanged(QAbstractItemModel* model);
  void selectionModelChanged(QItemSelectionModel* model);
  void currentIndexChanged(const QModelIndex& index);
  void expanded(const QModelIndex& index);
  void collapsed(const QModelIndex& index);
  void activated(const QModelIndex& index);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  friend class AdNavigationMenuItemDelegate;
  friend class detail::AdMenuBarProxyModel;
  friend class detail::AdMenuBarView;
  friend class detail::AdMenuPopupShell;
  friend class detail::AdMenuTreeView;

  QObject* popupOwnerObject() const override;
  QWidget* popupAnchorWidget() const override;
  QWidget* popupScopeWindow() const override;
  bool popupIsVisible() const override;
  bool popupContainsGlobalPos(const QPoint& globalPos) const override;
  void popupCloseFromHost(detail::PopupCloseReason reason) override;
  void popupRelayoutFromHost() override;

  int effectiveIndentation(const detail::MenuVisualStyle& style) const;
  SemanticStyles resolvedSemanticStyles(Mode mode, ColorScheme colorScheme, bool collapsed) const;
  detail::MenuVisualStyle resolvedVisualStyle(Mode mode, ColorScheme colorScheme,
                                              bool collapsed) const;

  class Private;
  std::unique_ptr<Private> d_;
};

}  // namespace adqt::widgets
