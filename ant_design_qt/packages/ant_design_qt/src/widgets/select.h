#pragma once

#include <QColor>
#include <QHash>
#include <QModelIndex>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

#include "popup_interaction_host.h"
#include "icon_core.h"
#include "popup_types.h"
#include "select_types.h"
#include "control_scale.h"

class QAbstractItemDelegate;
class QAbstractItemModel;
class QFrame;
class QHBoxLayout;
class QItemSelectionModel;
class QLabel;
class QLayout;
class QLineEdit;
class QListView;
class QModelIndex;
class QMoveEvent;
class QPaintEvent;
class QEnterEvent;
class QPainter;
class QToolButton;
class QVBoxLayout;

namespace adqt::widgets {

class AdScrollArea;

namespace detail {
class SelectCompositeModel;
class SelectFilterProxyModel;
struct SelectVisualStyle;
}  // namespace detail

class AdSelect final : public QWidget,
                       public AdControlScaleParticipant,
                       private detail::PopupInteractionOwner {
  Q_OBJECT

  Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)
  Q_PROPERTY(
      ControlSize controlSize READ controlSize WRITE setControlSize NOTIFY controlSizeChanged)
  Q_PROPERTY(Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(Status status READ status WRITE setStatus NOTIFY statusChanged)
  Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear NOTIFY allowClearChanged)
  Q_PROPERTY(bool loading READ loading WRITE setLoading NOTIFY loadingChanged)
  Q_PROPERTY(bool popupVisible READ popupVisible WRITE setPopupVisible NOTIFY popupVisibleChanged)
  Q_PROPERTY(
      bool searchEnabled READ searchEnabled WRITE setSearchEnabled NOTIFY searchEnabledChanged)
  Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
  Q_PROPERTY(int maxCount READ maxCount WRITE setMaxCount NOTIFY maxCountChanged)
  Q_PROPERTY(int maxTagCount READ maxTagCount WRITE setMaxTagCount NOTIFY maxTagCountChanged)
  Q_PROPERTY(bool responsiveMaxTagCount READ responsiveMaxTagCount WRITE setResponsiveMaxTagCount
                 NOTIFY responsiveMaxTagCountChanged)
  Q_PROPERTY(bool autoClearSearchValue READ autoClearSearchValue WRITE setAutoClearSearchValue
                 NOTIFY autoClearSearchValueChanged)
  Q_PROPERTY(Placement placement READ placement WRITE setPlacement NOTIFY placementChanged)
  Q_PROPERTY(adqt::widgets::AdPopupLayerMode popupLayerMode READ popupLayerMode WRITE
                 setPopupLayerMode NOTIFY popupLayerModeChanged)
  Q_PROPERTY(bool popupMatchSelectWidth READ popupMatchSelectWidth WRITE setPopupMatchSelectWidth
                 NOTIFY popupMatchSelectWidthChanged)
  Q_PROPERTY(int popupWidth READ popupWidth WRITE setPopupWidth NOTIFY popupWidthChanged)
  Q_PROPERTY(int modelColumn READ modelColumn WRITE setModelColumn NOTIFY modelColumnChanged)
  Q_PROPERTY(QString placeholder READ placeholder WRITE setPlaceholder NOTIFY placeholderChanged)
  Q_PROPERTY(bool joinedLeft READ joinedLeft WRITE setJoinedLeft)
  Q_PROPERTY(bool joinedRight READ joinedRight WRITE setJoinedRight)
  Q_PROPERTY(QString prefixText READ prefixText WRITE setPrefixText NOTIFY prefixTextChanged)
  Q_PROPERTY(adqt::icons::IconRef feedbackIconRef READ feedbackIconRef WRITE setFeedbackIconRef
                 NOTIFY feedbackIconRefChanged)
  Q_PROPERTY(
      QVariant currentValue READ currentValue WRITE setCurrentValue NOTIFY currentValueChanged)
  Q_PROPERTY(QVariantList currentValues READ currentValues WRITE setCurrentValues NOTIFY
                 currentValuesChanged)

 public:
  enum class Mode {
    Single,
    Multiple,
    Tags,
  };
  Q_ENUM(Mode)

  enum class ControlSize {
    Large,
    Middle,
    Small,
  };
  Q_ENUM(ControlSize)

  enum class Variant {
    Outlined,
    Filled,
    Borderless,
    Underlined,
  };
  Q_ENUM(Variant)

  enum class Status {
    None,
    Error,
    Warning,
  };
  Q_ENUM(Status)

  enum class Placement {
    BottomLeft,
    BottomRight,
    TopLeft,
    TopRight,
    BottomCenter,
    TopCenter,
  };
  Q_ENUM(Placement)

  using ItemDataRole = AdSelectTypes::ItemDataRole;
  static constexpr int DefaultValueRole = AdSelectTypes::DefaultValueRole;
  static constexpr int DefaultLabelRole = AdSelectTypes::DefaultLabelRole;
  static constexpr int DefaultTagTextRole = AdSelectTypes::DefaultTagTextRole;
  static constexpr int DefaultSelectedTextRole = AdSelectTypes::DefaultSelectedTextRole;
  static constexpr int DefaultGroupRole = AdSelectTypes::DefaultGroupRole;
  static constexpr int DefaultMetadataRole = AdSelectTypes::DefaultMetadataRole;

  using Option = AdSelectTypes::Option;
  using Item = AdSelectTypes::Item;
  using SelectionValue = AdSelectTypes::SelectionValue;
  using SelectionItem = AdSelectTypes::SelectionItem;
  using RoleConfig = AdSelectTypes::RoleConfig;
  using MetricTokens = AdSelectTypes::MetricTokens;
  using ColorTokens = AdSelectTypes::ColorTokens;
  using ComponentTokens = AdSelectTypes::ComponentTokens;
  using SemanticSlotStyle = AdSelectTypes::SemanticSlotStyle;
  using SemanticStyles = AdSelectTypes::SemanticStyles;

  struct StyleContext {
    Mode mode = Mode::Single;
    ControlSize controlSize = ControlSize::Middle;
    Variant variant = Variant::Outlined;
    Status status = Status::None;
    bool disabled = false;
    bool popupVisible = false;
    QString searchText;
    QVariantList currentValues;
    QStringList currentValueKeys;
  };

  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;
  using FilterPredicate = AdSelectTypes::FilterPredicate;
  using SortComparator = AdSelectTypes::SortComparator;
  using OptionTextFormatter = AdSelectTypes::OptionTextFormatter;
  using TagTextFormatter = AdSelectTypes::TagTextFormatter;
  using LabelFormatter = AdSelectTypes::LabelFormatter;
  using PopupExtraContentFactory = AdSelectTypes::PopupExtraContentFactory;
  using SearchPolicy = adqt::widgets::select::SearchPolicy;
  using PopupLayerMode = AdPopupLayerMode;

  explicit AdSelect(QWidget* parent = nullptr);
  ~AdSelect() override;

  Mode mode() const;
  void setMode(Mode value);

  ControlSize controlSize() const;
  void setControlSize(ControlSize value);

  Variant variant() const;
  void setVariant(Variant value);

  Status status() const;
  void setStatus(Status value);

  bool allowClear() const;
  void setAllowClear(bool value);

  bool loading() const;
  void setLoading(bool value);

  bool popupVisible() const;
  void setPopupVisible(bool value);

  bool disabled() const;
  void setDisabled(bool value);

  bool searchEnabled() const;
  void setSearchEnabled(bool value);

  QString searchText() const;
  void setSearchText(const QString& value);

  SearchPolicy searchPolicy() const;
  void setSearchPolicy(SearchPolicy value);

  int maxCount() const;
  void setMaxCount(int value);

  int maxTagCount() const;
  void setMaxTagCount(int value);

  bool responsiveMaxTagCount() const;
  void setResponsiveMaxTagCount(bool value);

  bool autoClearSearchValue() const;
  void setAutoClearSearchValue(bool value);

  Placement placement() const;
  void setPlacement(Placement value);

  PopupLayerMode popupLayerMode() const;
  void setPopupLayerMode(PopupLayerMode value);

  bool popupMatchSelectWidth() const;
  void setPopupMatchSelectWidth(bool value);

  int popupWidth() const;
  void setPopupWidth(int value);

  int modelColumn() const;
  void setModelColumn(int value);

  QString placeholder() const;
  void setPlaceholder(const QString& value);

  bool joinedLeft() const;
  void setJoinedLeft(bool value);

  bool joinedRight() const;
  void setJoinedRight(bool value);

  QString prefixText() const;
  void setPrefixText(const QString& value);

  adqt::icons::IconRef prefixIconRef() const;
  void setPrefixIconRef(const adqt::icons::IconRef& token);

  adqt::icons::IconRef suffixIconRef() const;
  void setSuffixIconRef(const adqt::icons::IconRef& token);

  adqt::icons::IconRef feedbackIconRef() const;
  void setFeedbackIconRef(const adqt::icons::IconRef& token);

  QVariant currentValue() const;
  void setCurrentValue(const QVariant& value);

  QVariantList currentValues() const;
  void setCurrentValues(const QVariantList& values);

  QVector<SelectionItem> currentItems() const;
  QModelIndex currentModelIndex() const;
  QModelIndexList selectedModelIndexes() const;
  int currentIndex() const;
  void setCurrentIndex(int index);
  QString currentText() const;
  QVariant currentData(int role = DefaultValueRole) const;
  void setCurrentData(const QVariant& value, int role = DefaultValueRole);
  bool editable() const;
  void setEditable(bool value);
  QLineEdit* lineEdit() const;
  QListView* view() const;
  void showPopup();
  void hidePopup();

  QVector<Option> options() const;
  void setOptions(const QVector<Option>& options);
  void appendOption(const Option& option);
  void clearOptions();

  QAbstractItemModel* model() const;
  void setModel(QAbstractItemModel* model);
  QItemSelectionModel* selectionModel() const;
  void setSelectionModel(QItemSelectionModel* model);

  QList<Item> items() const;
  void setItems(const QList<Item>& items);

  int valueRole() const;
  void setValueRole(int role);

  int labelRole() const;
  void setLabelRole(int role);

  int tagTextRole() const;
  void setTagTextRole(int role);

  int selectedTextRole() const;
  void setSelectedTextRole(int role);

  int groupRole() const;
  void setGroupRole(int role);

  RoleConfig roleConfig() const;
  void setRoleConfig(const RoleConfig& config);

  QList<int> searchRoles() const;
  void setSearchRoles(const QList<int>& roles);

  QStringList searchFilterFields() const;
  void setSearchFilterFields(const QStringList& fields);

  QAbstractItemDelegate* itemDelegate() const;
  void setItemDelegate(QAbstractItemDelegate* delegate);

  QWidget* popupFooterWidget() const;
  void setPopupFooterWidget(QWidget* widget);

  void setFilterPredicate(FilterPredicate predicate);
  FilterPredicate filterPredicate() const;

  void setSortComparator(SortComparator comparator);
  SortComparator sortComparator() const;

  void setTokenSeparators(const QStringList& separators);
  QStringList tokenSeparators() const;

  void setOptionTextFormatter(OptionTextFormatter formatter);
  OptionTextFormatter optionTextFormatter() const;

  void setTagTextFormatter(TagTextFormatter formatter);
  TagTextFormatter tagTextFormatter() const;

  void setLabelFormatter(LabelFormatter formatter);
  LabelFormatter labelFormatter() const;

  void setPopupExtraContentFactory(PopupExtraContentFactory factory);
  PopupExtraContentFactory popupExtraContentFactory() const;

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  void prepareControlScale(const AdControlScaleContext& context) override;
  void commitControlScale(const AdControlScaleContext& context) override;

 signals:
  void modeChanged(Mode value);
  void controlSizeChanged(ControlSize value);
  void variantChanged(Variant value);
  void statusChanged(Status value);
  void allowClearChanged(bool value);
  void loadingChanged(bool value);
  // Emitted before opening transfers keyboard focus into the popup.
  void popupOpening();
  void popupVisibleChanged(bool value);
  void disabledChanged(bool value);
  void searchEnabledChanged(bool value);
  void searchTextChanged(const QString& value);
  void maxCountChanged(int value);
  void maxTagCountChanged(int value);
  void responsiveMaxTagCountChanged(bool value);
  void autoClearSearchValueChanged(bool value);
  void placementChanged(Placement value);
  void popupLayerModeChanged(PopupLayerMode value);
  void popupMatchSelectWidthChanged(bool value);
  void popupWidthChanged(int value);
  void modelColumnChanged(int value);
  void placeholderChanged(const QString& value);
  void prefixTextChanged(const QString& value);
  void prefixIconRefChanged(const adqt::icons::IconRef& token);
  void suffixIconRefChanged(const adqt::icons::IconRef& token);
  void feedbackIconRefChanged(const adqt::icons::IconRef& token);
  void currentValueChanged(const QVariant& value);
  void currentValuesChanged(const QVariantList& values);
  void currentItemsChanged(const QVector<SelectionItem>& items);
  void currentModelIndexChanged(const QModelIndex& index);
  void modelChanged(QAbstractItemModel* model);
  void selectionModelChanged(QItemSelectionModel* model);
  void optionsChanged();
  void componentTokensChanged();
  void semanticStylesChanged();
  void selected(const QVariant& value, const QString& label);
  void deselected(const QVariant& value, const QString& label);
  void cleared();
  void selectionChanged(const QVector<SelectionItem>& items);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  struct ModelRow {
    bool header = false;
    bool empty = false;
    int optionIndex = -1;
    QString headerText;
  };

  class PopupFrame;
  class OptionListModel;
  class OptionListDelegate;

  struct SelectionSnapshot {
    QVariant currentValue;
    QVariantList currentValues;
    QVector<SelectionItem> items;
    QModelIndex currentModelIndex;
  };

  SelectionSnapshot captureSelectionSnapshot() const;
  void emitSelectionSignalsFromSnapshot(const SelectionSnapshot& previous);
  QVariantList normalizedSelectionValues(const QVariantList& values) const;
  void applySelectedValues(const QVariantList& values, bool ensureTagOptions = true);
  void syncSelectionKeysFromState();
  void syncSelectionStateFromSelectionModel();
  void syncSelectionModelFromState(const QVariantList& selectedValues);
  QVariantList selectedModelValues() const;
  QVariantList effectiveSelectedValues() const;
  QVariantList normalizedCustomTagValues(const QVariantList& values) const;
  void setCustomTagValues(const QVariantList& values);
  QModelIndex sourceIndexForValue(const QVariant& value) const;
  QModelIndex compositeIndexForValue(const QVariant& value) const;
  void updateRoleConfig();
  QStringList effectiveSearchFilterFields() const;
  void ensureOverlayModel();
  void clearOverlayModel();
  void syncModelBackedOptions(bool preserveCurrentValues = true);
  Option optionFromRow(int row) const;
  QString modelLabelForValue(const QVariant& value) const;
  void updateSelectionCaches();
  void syncPopupExtraContentWidget();
  bool isSearchEnabledForCurrentMode() const;
  bool isValueSelected(const QVariant& value) const;
  int indexOfValue(const QVariant& value) const;
  const Option* findOption(const QVariant& value) const;
  Option* findOption(const QVariant& value);
  QString optionLabelOrFallback(const Option& option) const;
  QString formattedOptionText(const Option& option) const;
  QString formattedTagText(const Option& option) const;
  QString formattedSelectedLabel(const Option& option) const;
  QString fallbackSelectedLabel(const QVariant& value) const;
  QVariant rawValueForSelectionKey(const QString& value) const;
  int responsiveVisibleTagCount(const QStringList& labels, int availableWidth) const;
  void clearTagWidgets();
  void rebuildTagWidgets();
  void updateInputMode();
  void syncContentLayoutForMode();
  bool suffixButtonTriggersPopup() const;
  Qt::CursorShape selectorCursorShape() const;
  Qt::CursorShape optionCursorShapeAtRow(int row) const;
  int nextSelectableRow(int startRow, int step) const;
  void moveCurrentListRow(int step, bool pageStep = false);
  void moveCurrentListRowToBoundary(bool toEnd);
  void activateCurrentListRow();
  void syncPopupSelectionState();
  void syncPopupOptionCursor(const QPoint& viewportPos);
  void updateDisplay();
  void updateMultipleSelectorHeight();
  void updateClearButton();
  void updateClearVisual();
  void updateAccessoryGeometry();
  void updatePrefixVisual();
  void updateSuffixVisual();
  void updateLoadingSpinnerState();
  void updateAccessibility();
  void applyVisualStyle();
  void refreshRows();
  QVector<int> filteredOptionIndexes() const;
  void syncCurrentListRow(const QVariant& preferredValue = QVariant(),
                          bool preserveScrollPosition = false);
  bool addTagValue(const QString& value);
  void ensureTagOptionExists(const QVariant& value);
  void consumeTokenizedInput(const QString& text);
  void clearSelectionInternal(bool emitSignals);
  void emitSelectionChangedSignals();
  void toggleSelectionForOption(const Option& option);
  void ensurePopup();
  void applyPopupLayerMode();
  void syncTopLevelPopupTooltipRoute();
  void rebuildPopupExtraContent();
  int popupContentWidthHint() const;
  void syncPopupGeometry();
  void closePopup();
  void openPopup();
  void setOpenInternal(bool value, bool emitSignal);
  void updateFocusState();
  QRectF selectorPaintRect() const;
  QColor resolveSelectorBgColor() const;
  QColor resolveSelectorBorderColor() const;
  qreal resolveSelectorRadius() const;
  void paintSelectorShell(QPainter& painter) const;
  void updateInteractionFocusOverlay();
  void bumpJoinedZOrder();

  QObject* popupOwnerObject() const override;
  QWidget* popupAnchorWidget() const override;
  QWidget* popupScopeWindow() const override;
  QWidget* popupSurfaceWidget() const override;
  bool popupIsVisible() const override;
  bool popupWantsHostFrameRelayout() const override;
  bool popupContainsGlobalPos(const QPoint& globalPos) const override;
  void popupCloseFromHost(detail::PopupCloseReason reason) override;
  void popupRelayoutFromHost() override;

  Mode mode_ = Mode::Single;
  ControlSize controlSize_ = ControlSize::Middle;
  Variant variant_ = Variant::Outlined;
  Status status_ = Status::None;
  bool allowClear_ = false;
  bool loading_ = false;
  bool open_ = false;
  bool searchEnabled_ = false;
  bool searchEnabledExplicit_ = false;
  QString searchText_;
  SearchPolicy searchPolicy_ = SearchPolicy::LocalFilter;
  QString inputMethodPreeditText_;
  int maxCount_ = -1;
  int maxTagCount_ = -1;
  bool responsiveMaxTagCount_ = false;
  bool autoClearSearchValue_ = true;
  Placement placement_ = Placement::BottomLeft;
  PopupLayerMode popupLayerMode_ = PopupLayerMode::InWindow;
  bool popupMatchSelectWidth_ = true;
  int popupWidth_ = 0;
  int modelColumn_ = 0;
  QString placeholder_;
  bool joinedLeft_ = false;
  bool joinedRight_ = false;
  QString prefixText_;
  adqt::icons::IconRef prefixIconRef_;
  adqt::icons::IconRef suffixIconRef_;
  adqt::icons::IconRef feedbackIconRef_;
  QString currentValueKey_;
  QStringList currentValueKeys_;
  QVector<Option> options_;
  QPointer<QAbstractItemModel> overlayModel_;
  QPointer<QAbstractItemModel> sourceModel_;
  QPointer<QAbstractItemModel> ownedModel_;
  QPointer<QItemSelectionModel> selectionModel_;
  QPointer<QItemSelectionModel> ownedSelectionModel_;
  int valueRole_ = DefaultValueRole;
  int labelRole_ = DefaultLabelRole;
  int tagTextRole_ = DefaultTagTextRole;
  int selectedTextRole_ = DefaultSelectedTextRole;
  int groupRole_ = DefaultGroupRole;
  QList<int> searchRoles_;
  QPointer<QAbstractItemDelegate> itemDelegateOverride_;
  QWidget* popupFooterWidget_ = nullptr;
  QVariantList customTagValues_;
  QVariantList currentValuesCache_;
  QVariant currentValueCache_;
  QPersistentModelIndex currentModelIndexCache_;
  QHash<QString, QString> selectedLabelCache_;
  QHash<QString, QString> selectedTagTextCache_;
  QHash<QString, QString> selectedDisplayTextCache_;
  QStringList searchFilterFields_;
  bool searchFilterFieldsExplicit_ = false;
  FilterPredicate filterPredicate_;
  SortComparator sortComparator_;
  QStringList tokenSeparators_;
  OptionTextFormatter optionTextFormatter_;
  TagTextFormatter tagTextFormatter_;
  LabelFormatter labelFormatter_;
  PopupExtraContentFactory popupExtraContentFactory_;
  ComponentTokens componentTokens_;
  SemanticStyles semanticStyles_;
  SemanticStyleResolver semanticStyleResolver_;
  detail::SelectVisualStyle* visualStyle_ = nullptr;
  detail::SelectVisualStyle* popupVisualStyle_ = nullptr;
  std::unique_ptr<detail::SelectCompositeModel> compositeModel_;
  std::unique_ptr<detail::SelectFilterProxyModel> filterProxyModel_;

  QHBoxLayout* rootLayout_ = nullptr;
  QLabel* prefixLabel_ = nullptr;
  QWidget* contentHost_ = nullptr;
  QHBoxLayout* contentLayout_ = nullptr;
  QWidget* tagsContainer_ = nullptr;
  QLabel* placeholderLabel_ = nullptr;
  QLayout* tagsLayout_ = nullptr;
  QLineEdit* lineEdit_ = nullptr;
  QToolButton* clearButton_ = nullptr;
  QToolButton* suffixButton_ = nullptr;

  QFrame* popup_ = nullptr;
  QVBoxLayout* popupLayout_ = nullptr;
  AdScrollArea* popupScrollArea_ = nullptr;
  QListView* listView_ = nullptr;
  QWidget* popupExtraContent_ = nullptr;
  OptionListModel* listModel_ = nullptr;
  QVector<ModelRow> rows_;
  bool suffixSpinnerSubscribed_ = false;

  bool hovered_ = false;
  bool clearHovered_ = false;
  bool hasFocusWithin_ = false;
  bool suppressLineEditChange_ = false;
  bool suppressPopupHideClose_ = false;
  bool applyingVisualStyle_ = false;
  bool preservePopupScrollOnRefresh_ = false;
  bool syncingSelectionModel_ = false;
  AdControlScaleContext controlScale_;
  QFont referenceFont_;
  bool referenceFontCaptured_ = false;
};

}  // namespace adqt::widgets
