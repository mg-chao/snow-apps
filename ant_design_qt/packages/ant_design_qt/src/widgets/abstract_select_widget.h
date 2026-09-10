#pragma once

#include "icon_core.h"
#include "popup_types.h"
#include "select_types.h"

#include <QAbstractItemModel>
#include <QItemSelectionModel>
#include <QWidget>

class QAbstractItemDelegate;
class QHBoxLayout;
class QLineEdit;
class QListView;

namespace adqt::widgets {

class AdSelect;

class AdAbstractSelectWidget : public QWidget {
  Q_OBJECT

  Q_PROPERTY(QAbstractItemModel* model READ model WRITE setModel NOTIFY modelChanged)
  Q_PROPERTY(QItemSelectionModel* selectionModel READ selectionModel WRITE setSelectionModel NOTIFY
                 selectionModelChanged)
  Q_PROPERTY(int modelColumn READ modelColumn WRITE setModelColumn NOTIFY modelColumnChanged)
  Q_PROPERTY(bool loading READ loading WRITE setLoading NOTIFY loadingChanged)
  Q_PROPERTY(bool popupVisible READ popupVisible WRITE setPopupVisible NOTIFY popupVisibleChanged)
  Q_PROPERTY(
      bool searchEnabled READ searchEnabled WRITE setSearchEnabled NOTIFY searchEnabledChanged)
  Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
  Q_PROPERTY(adqt::widgets::select::SearchPolicy searchPolicy READ searchPolicy WRITE
                 setSearchPolicy NOTIFY searchPolicyChanged)
  Q_PROPERTY(adqt::widgets::select::Placement placement READ placement WRITE setPlacement NOTIFY
                 placementChanged)
  Q_PROPERTY(adqt::widgets::AdPopupLayerMode popupLayerMode READ popupLayerMode WRITE
                 setPopupLayerMode NOTIFY popupLayerModeChanged)
  Q_PROPERTY(adqt::widgets::select::PopupWidthMode popupWidthMode READ popupWidthMode WRITE
                 setPopupWidthMode NOTIFY popupWidthModeChanged)
  Q_PROPERTY(int popupWidth READ popupWidth WRITE setPopupWidth NOTIFY popupWidthChanged)
  Q_PROPERTY(QString placeholder READ placeholder WRITE setPlaceholder NOTIFY placeholderChanged)
  Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear NOTIFY allowClearChanged)
  Q_PROPERTY(adqt::widgets::select::ControlSize controlSize READ controlSize WRITE setControlSize
                 NOTIFY controlSizeChanged)
  Q_PROPERTY(
      adqt::widgets::select::Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(adqt::widgets::select::Status status READ status WRITE setStatus NOTIFY statusChanged)
  Q_PROPERTY(bool joinedLeft READ joinedLeft WRITE setJoinedLeft NOTIFY joinedLeftChanged)
  Q_PROPERTY(bool joinedRight READ joinedRight WRITE setJoinedRight NOTIFY joinedRightChanged)
  Q_PROPERTY(QString prefixText READ prefixText WRITE setPrefixText NOTIFY prefixTextChanged)
  Q_PROPERTY(adqt::icons::IconRef prefixIconRef READ prefixIconRef WRITE setPrefixIconRef NOTIFY
                 prefixIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef suffixIconRef READ suffixIconRef WRITE setSuffixIconRef NOTIFY
                 suffixIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef feedbackIconRef READ feedbackIconRef WRITE setFeedbackIconRef
                 NOTIFY feedbackIconRefChanged)

 public:
  using Mode = select::Mode;
  using ControlSize = select::ControlSize;
  using Variant = select::Variant;
  using Status = select::Status;
  using Placement = select::Placement;
  using PopupLayerMode = AdPopupLayerMode;
  using SearchPolicy = select::SearchPolicy;
  using PopupWidthMode = select::PopupWidthMode;
  using Option = select::Option;
  using Item = select::Item;
  using SelectionItem = select::SelectionItem;
  using SelectionValue = select::SelectionValue;
  using RoleConfig = select::RoleConfig;
  using MetricTokens = select::MetricTokens;
  using ColorTokens = select::ColorTokens;
  using ComponentTokens = select::ComponentTokens;
  using SemanticSlotStyle = select::SemanticSlotStyle;
  using SemanticStyles = select::SemanticStyles;
  using StyleContext = select::StyleContext;
  using SemanticStyleResolver = select::SemanticStyleResolver;

  enum ItemDataRole {
    DefaultValueRole = AdSelectTypes::DefaultValueRole,
    DefaultLabelRole = AdSelectTypes::DefaultLabelRole,
    DefaultTagTextRole = AdSelectTypes::DefaultTagTextRole,
    DefaultSelectedTextRole = AdSelectTypes::DefaultSelectedTextRole,
    DefaultGroupRole = AdSelectTypes::DefaultGroupRole,
    DefaultMetadataRole = AdSelectTypes::DefaultMetadataRole,
  };

  explicit AdAbstractSelectWidget(QWidget* parent = nullptr);
  ~AdAbstractSelectWidget() override;

  QAbstractItemModel* model() const;
  void setModel(QAbstractItemModel* model);

  QItemSelectionModel* selectionModel() const;
  void setSelectionModel(QItemSelectionModel* model);

  int modelColumn() const;
  void setModelColumn(int value);

  bool loading() const;
  void setLoading(bool value);

  bool popupVisible() const;
  void setPopupVisible(bool value);

  bool searchEnabled() const;
  void setSearchEnabled(bool value);

  QString searchText() const;
  void setSearchText(const QString& value);

  SearchPolicy searchPolicy() const;
  void setSearchPolicy(SearchPolicy value);

  Placement placement() const;
  void setPlacement(Placement value);

  PopupLayerMode popupLayerMode() const;
  void setPopupLayerMode(PopupLayerMode value);

  PopupWidthMode popupWidthMode() const;
  void setPopupWidthMode(PopupWidthMode value);

  int popupWidth() const;
  void setPopupWidth(int value);

  QString placeholder() const;
  void setPlaceholder(const QString& value);

  bool allowClear() const;
  void setAllowClear(bool value);

  ControlSize controlSize() const;
  void setControlSize(ControlSize value);

  Variant variant() const;
  void setVariant(Variant value);

  Status status() const;
  void setStatus(Status value);

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

  QVector<Option> options() const;
  void setOptions(const QVector<Option>& options);
  void appendOption(const Option& option);
  void clearOptions();

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

  // Replaces the default empty-result row; retained when options become available.
  QWidget* notFoundContentWidget() const;
  void setNotFoundContentWidget(QWidget* widget);

  QWidget* popupFooterWidget() const;
  void setPopupFooterWidget(QWidget* widget);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);

  QLineEdit* lineEdit() const;
  QListView* view() const;
  void showPopup();
  void hidePopup();

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void modelChanged(QAbstractItemModel* model);
  void selectionModelChanged(QItemSelectionModel* model);
  void modelColumnChanged(int value);
  void loadingChanged(bool value);
  void popupVisibleChanged(bool value);
  void searchEnabledChanged(bool value);
  void searchTextChanged(const QString& value);
  void searchPolicyChanged(adqt::widgets::select::SearchPolicy value);
  void placementChanged(adqt::widgets::select::Placement value);
  void popupLayerModeChanged(PopupLayerMode value);
  void popupWidthModeChanged(adqt::widgets::select::PopupWidthMode value);
  void popupWidthChanged(int value);
  void placeholderChanged(const QString& value);
  void allowClearChanged(bool value);
  void controlSizeChanged(adqt::widgets::select::ControlSize value);
  void variantChanged(adqt::widgets::select::Variant value);
  void statusChanged(adqt::widgets::select::Status value);
  void joinedLeftChanged(bool value);
  void joinedRightChanged(bool value);
  void prefixTextChanged(const QString& value);
  void prefixIconRefChanged(const adqt::icons::IconRef& token);
  void suffixIconRefChanged(const adqt::icons::IconRef& token);
  void feedbackIconRefChanged(const adqt::icons::IconRef& token);
  void optionsChanged();
  void componentTokensChanged();
  void semanticStylesChanged();

 protected:
  void setInternalMode(Mode mode);
  AdSelect* internalSelect() const;

 private:
  void applyPopupWidthPolicy();

  AdSelect* control_ = nullptr;
  QHBoxLayout* layout_ = nullptr;
  PopupWidthMode popupWidthMode_ = PopupWidthMode::MatchControlWidth;
  int popupWidth_ = 0;
};

}  // namespace adqt::widgets
