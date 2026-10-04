#pragma once

#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

#include <functional>

#include "icon_core.h"

class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLayout;
class QPushButton;
class QScrollArea;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace adqt::widgets {

namespace detail {
struct FormVisualStyle;
}

class AdFormItem;
class AdFormList;

class AdForm final : public QWidget {
  Q_OBJECT

  Q_PROPERTY(FormLayout formLayout READ formLayout WRITE setFormLayout NOTIFY formLayoutChanged)
  Q_PROPERTY(LabelAlign labelAlign READ labelAlign WRITE setLabelAlign NOTIFY labelAlignChanged)
  Q_PROPERTY(
      RequiredMark requiredMark READ requiredMark WRITE setRequiredMark NOTIFY requiredMarkChanged)
  Q_PROPERTY(
      ControlSize controlSize READ controlSize WRITE setControlSize NOTIFY controlSizeChanged)
  Q_PROPERTY(Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(bool colon READ colon WRITE setColon NOTIFY colonChanged)
  Q_PROPERTY(bool labelWrap READ labelWrap WRITE setLabelWrap NOTIFY labelWrapChanged)
  Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)
  Q_PROPERTY(bool scrollToFirstError READ scrollToFirstError WRITE setScrollToFirstError NOTIFY
                 scrollToFirstErrorChanged)
  Q_PROPERTY(int labelColumnWidth READ labelColumnWidth WRITE setLabelColumnWidth NOTIFY
                 labelColumnWidthChanged)
  Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
  Q_PROPERTY(QVariantMap initialValues READ initialValues WRITE setInitialValues NOTIFY
                 initialValuesChanged)
  Q_PROPERTY(QString requiredMessageTemplate READ requiredMessageTemplate WRITE
                 setRequiredMessageTemplate NOTIFY requiredMessageTemplateChanged)

 public:
  enum class FormLayout {
    Horizontal,
    Vertical,
    Inline,
  };
  Q_ENUM(FormLayout)

  enum class LabelAlign {
    Right,
    Left,
  };
  Q_ENUM(LabelAlign)

  enum class RequiredMark {
    Visible,
    Hidden,
    Optional,
  };
  Q_ENUM(RequiredMark)

  enum class ControlSize {
    Large,
    Medium,
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

  explicit AdForm(QWidget* parent = nullptr);
  ~AdForm() override;

  FormLayout formLayout() const;
  void setFormLayout(FormLayout value);

  LabelAlign labelAlign() const;
  void setLabelAlign(LabelAlign value);

  RequiredMark requiredMark() const;
  void setRequiredMark(RequiredMark value);

  ControlSize controlSize() const;
  void setControlSize(ControlSize value);

  Variant variant() const;
  void setVariant(Variant value);

  bool colon() const;
  void setColon(bool value);

  bool labelWrap() const;
  void setLabelWrap(bool value);

  bool disabled() const;
  void setDisabled(bool value);

  bool scrollToFirstError() const;
  void setScrollToFirstError(bool value);

  int labelColumnWidth() const;
  void setLabelColumnWidth(int value);

  QString name() const;
  void setName(const QString& value);

  QVariantMap initialValues() const;
  void setInitialValues(const QVariantMap& values);

  QString requiredMessageTemplate() const;
  void setRequiredMessageTemplate(const QString& value);

  AdFormItem* addItem(const QString& label, QWidget* controlWidget,
                      const QString& fieldName = QString());
  AdFormItem* addField(const QString& label, QWidget* editor, const QString& fieldKey = QString());
  void addItem(AdFormItem* item);
  void insertItem(int index, AdFormItem* item);
  void removeItem(AdFormItem* item);
  QVector<AdFormItem*> items() const;
  AdFormItem* itemForName(const QString& fieldName) const;
  AdFormItem* itemForNamePath(const QStringList& namePath) const;
  AdFormItem* field(const QString& fieldKey) const;
  AdFormItem* fieldAtPath(const QStringList& fieldPath) const;
  QStringList fieldKeys() const;

  QVariantMap values() const;
  QVariantMap fieldValues() const;
  QVariantMap flatValues() const;
  void setValues(const QVariantMap& values);
  void setFieldValues(const QVariantMap& values);
  QVariant value(const QString& fieldName) const;
  QVariant value(const QStringList& namePath) const;
  QVariant fieldValue(const QString& fieldKey) const;
  QVariant fieldValue(const QStringList& fieldPath) const;
  void setValue(const QString& fieldName, const QVariant& value);
  void setValue(const QStringList& namePath, const QVariant& value);
  void setFieldValue(const QString& fieldKey, const QVariant& value);
  void setFieldValue(const QStringList& fieldPath, const QVariant& value);

  bool validate();
  QVector<AdFormItem*> validateFields(const QStringList& fieldNames = QStringList());
  bool validateField(const QString& fieldName);
  bool validateField(const QStringList& namePath);
  QVector<AdFormItem*> invalidItems() const;
  void resetValidation();
  void resetFields();
  void resetFields(const QStringList& fieldNames);
  void resetField(const QString& fieldName);
  void resetField(const QStringList& namePath);
  bool isFieldTouched(const QString& fieldName) const;
  bool isFieldTouched(const QStringList& namePath) const;
  bool isFieldsTouched(bool allTouched = false) const;
  bool isFieldDirty(const QString& fieldName) const;
  bool isFieldDirty(const QStringList& namePath) const;
  bool submit();
  bool scrollToField(const QString& fieldName, bool focusControl = true);
  bool scrollToField(const QStringList& namePath, bool focusControl = true);

 signals:
  void formLayoutChanged(FormLayout value);
  void labelAlignChanged(LabelAlign value);
  void requiredMarkChanged(RequiredMark value);
  void controlSizeChanged(ControlSize value);
  void variantChanged(Variant value);
  void colonChanged(bool value);
  void labelWrapChanged(bool value);
  void disabledChanged(bool value);
  void scrollToFirstErrorChanged(bool value);
  void labelColumnWidthChanged(int value);
  void nameChanged(const QString& value);
  void initialValuesChanged(const QVariantMap& values);
  void requiredMessageTemplateChanged(const QString& value);
  void itemAdded(AdFormItem* item);
  void itemRemoved(AdFormItem* item);
  void fieldValueChanged(const QString& fieldName, const QVariant& value);
  void fieldChanged(const QString& fieldKey, const QVariant& value);
  void fieldPathValueChanged(const QStringList& fieldPath, const QVariant& value);
  void valuesChanged(const QVariantMap& changedValues, const QVariantMap& allValues);
  void validationFinished(bool valid);
  void submitSucceeded(const QVariantMap& values);
  void submitFailed(const QVector<adqt::widgets::AdFormItem*>& invalidItems);

 protected:
  void changeEvent(QEvent* event) override;

 private:
  friend class AdFormItem;

  void rebuildRootLayout();
  void refreshItems();
  void attachItem(AdFormItem* item);
  void detachItem(AdFormItem* item);
  void itemValueChanged(AdFormItem* item);
  void revalidateDependents(AdFormItem* sourceItem);
  bool initialValueForItem(AdFormItem* item, QVariant* valueOut = nullptr) const;
  void resetItemToInitial(AdFormItem* item);
  void notifyItemDestroyed(AdFormItem* item);

  FormLayout formLayout_ = FormLayout::Horizontal;
  LabelAlign labelAlign_ = LabelAlign::Right;
  RequiredMark requiredMark_ = RequiredMark::Visible;
  ControlSize controlSize_ = ControlSize::Medium;
  Variant variant_ = Variant::Outlined;
  bool colon_ = true;
  bool labelWrap_ = false;
  bool disabled_ = false;
  bool scrollToFirstError_ = false;
  int labelColumnWidth_ = 0;
  QString name_;
  QVariantMap initialValues_;
  QString requiredMessageTemplate_;
  QVector<QPointer<AdFormItem>> items_;
  QLayout* rootLayout_ = nullptr;
};

class AdFormItem final : public QWidget {
  Q_OBJECT

  Q_PROPERTY(QString label READ label WRITE setLabel NOTIFY labelChanged)
  Q_PROPERTY(QString fieldKey READ fieldKey WRITE setFieldKey NOTIFY fieldKeyChanged)
  Q_PROPERTY(QStringList fieldPath READ fieldPath WRITE setFieldPath NOTIFY fieldPathChanged)
  Q_PROPERTY(QString fieldName READ fieldName WRITE setFieldName NOTIFY fieldNameChanged)
  Q_PROPERTY(QStringList namePath READ namePath WRITE setNamePath NOTIFY namePathChanged)
  Q_PROPERTY(
      QStringList dependencies READ dependencies WRITE setDependencies NOTIFY dependenciesChanged)
  Q_PROPERTY(bool required READ required WRITE setRequired NOTIFY requiredChanged)
  Q_PROPERTY(QString requiredMessage READ requiredMessage WRITE setRequiredMessage NOTIFY
                 requiredMessageChanged)
  Q_PROPERTY(bool hasFeedback READ hasFeedback WRITE setHasFeedback NOTIFY hasFeedbackChanged)
  Q_PROPERTY(bool noStyle READ noStyle WRITE setNoStyle NOTIFY noStyleChanged)
  Q_PROPERTY(bool itemHidden READ itemHidden WRITE setItemHidden NOTIFY itemHiddenChanged)
  Q_PROPERTY(bool validateOnChange READ validateOnChange WRITE setValidateOnChange NOTIFY
                 validateOnChangeChanged)
  Q_PROPERTY(int validateDebounceMs READ validateDebounceMs WRITE setValidateDebounceMs NOTIFY
                 validateDebounceMsChanged)
  Q_PROPERTY(
      QVariant initialValue READ initialValue WRITE setInitialValue NOTIFY initialValueChanged)
  Q_PROPERTY(QString valuePropertyName READ valuePropertyName WRITE setValuePropertyName NOTIFY
                 valuePropertyNameChanged)
  Q_PROPERTY(QString controlValueProperty READ controlValueProperty WRITE setControlValueProperty
                 NOTIFY controlValuePropertyChanged)
  Q_PROPERTY(bool touched READ isTouched NOTIFY touchedChanged)
  Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)
  Q_PROPERTY(ValidateStatus validateStatus READ validateStatus WRITE setValidateStatus NOTIFY
                 validateStatusChanged)
  Q_PROPERTY(ItemLayout itemLayout READ itemLayout WRITE setItemLayout NOTIFY itemLayoutChanged)
  Q_PROPERTY(QString helpText READ helpText WRITE setHelpText NOTIFY helpTextChanged)
  Q_PROPERTY(QString extraText READ extraText WRITE setExtraText NOTIFY extraTextChanged)
  Q_PROPERTY(QString tooltipText READ tooltipText WRITE setTooltipText NOTIFY tooltipTextChanged)
  Q_PROPERTY(
      QWidget* controlWidget READ controlWidget WRITE setControlWidget NOTIFY controlWidgetChanged)

 public:
  enum class ValidateStatus {
    None,
    Success,
    Warning,
    Error,
    Validating,
  };
  Q_ENUM(ValidateStatus)

  enum class ItemLayout {
    Inherit,
    Horizontal,
    Vertical,
  };
  Q_ENUM(ItemLayout)

  struct ValidationResult {
    ValidateStatus status = ValidateStatus::Success;
    QStringList errors;
    QStringList warnings;
    QString helpText;
  };

  using Validator = std::function<ValidationResult(const QVariant& value, QWidget* controlWidget)>;
  using FormValidator = std::function<ValidationResult(const QVariant& value, AdFormItem* item)>;
  using ValueReader = std::function<QVariant(QWidget* controlWidget)>;
  using ValueWriter = std::function<void(QWidget* controlWidget, const QVariant& value)>;
  using ValueNormalizer = std::function<QVariant(const QVariant& value,
                                                 const QVariant& previousValue, AdFormItem* item)>;
  using FeedbackIconProvider = std::function<adqt::icons::IconRef(
      ValidateStatus status, const QStringList& errors, const QStringList& warnings)>;

  explicit AdFormItem(QWidget* parent = nullptr);
  explicit AdFormItem(const QString& label, QWidget* controlWidget,
                      const QString& fieldName = QString(), QWidget* parent = nullptr);
  ~AdFormItem() override;

  QString label() const;
  void setLabel(const QString& value);

  QString fieldKey() const;
  void setFieldKey(const QString& value);

  QString fieldName() const;
  void setFieldName(const QString& value);

  QStringList fieldPath() const;
  void setFieldPath(const QStringList& value);

  QStringList namePath() const;
  void setNamePath(const QStringList& value);

  QStringList dependencies() const;
  void setDependencies(const QStringList& value);

  bool required() const;
  void setRequired(bool value);

  QString requiredMessage() const;
  void setRequiredMessage(const QString& value);

  bool hasFeedback() const;
  void setHasFeedback(bool value);

  bool noStyle() const;
  void setNoStyle(bool value);

  bool itemHidden() const;
  void setItemHidden(bool value);

  bool validateOnChange() const;
  void setValidateOnChange(bool value);

  int validateDebounceMs() const;
  void setValidateDebounceMs(int value);

  QVariant initialValue() const;
  void setInitialValue(const QVariant& value);
  bool hasInitialValue() const;
  void clearInitialValue();

  QString valuePropertyName() const;
  void setValuePropertyName(const QString& value);
  QString controlValueProperty() const;
  void setControlValueProperty(const QString& value);

  bool isTouched() const;
  bool isDirty() const;

  ValidateStatus validateStatus() const;
  void setValidateStatus(ValidateStatus value);

  ItemLayout itemLayout() const;
  void setItemLayout(ItemLayout value);

  QString helpText() const;
  void setHelpText(const QString& value);
  void clearHelpText();

  QString extraText() const;
  void setExtraText(const QString& value);

  QString tooltipText() const;
  void setTooltipText(const QString& value);

  QStringList errorMessages() const;
  void setErrorMessages(const QStringList& messages);

  QStringList warningMessages() const;
  void setWarningMessages(const QStringList& messages);

  QWidget* controlWidget() const;
  void setControlWidget(QWidget* widget);
  QWidget* takeControlWidget();

  AdForm* formWidget() const;

  Validator validator() const;
  void setValidator(Validator validator);
  FormValidator formValidator() const;
  void setFormValidator(FormValidator validator);
  ValueReader valueReader() const;
  void setValueReader(ValueReader reader);
  void resetValueReader();
  ValueWriter valueWriter() const;
  void setValueWriter(ValueWriter writer);
  void resetValueWriter();
  ValueNormalizer valueNormalizer() const;
  void setValueNormalizer(ValueNormalizer normalizer);
  void resetValueNormalizer();
  FeedbackIconProvider feedbackIconProvider() const;
  void setFeedbackIconProvider(FeedbackIconProvider provider);
  void resetFeedbackIconProvider();

  QVariant value() const;
  void setValue(const QVariant& value);

  bool validate();
  void scheduleValidation();
  void resetValidation();
  void refresh();

 signals:
  void labelChanged(const QString& value);
  void fieldKeyChanged(const QString& value);
  void fieldNameChanged(const QString& value);
  void fieldPathChanged(const QStringList& value);
  void namePathChanged(const QStringList& value);
  void dependenciesChanged(const QStringList& value);
  void requiredChanged(bool value);
  void requiredMessageChanged(const QString& value);
  void hasFeedbackChanged(bool value);
  void noStyleChanged(bool value);
  void itemHiddenChanged(bool value);
  void validateOnChangeChanged(bool value);
  void validateDebounceMsChanged(int value);
  void initialValueChanged(const QVariant& value);
  void valuePropertyNameChanged(const QString& value);
  void controlValuePropertyChanged(const QString& value);
  void touchedChanged(bool value);
  void dirtyChanged(bool value);
  void validateStatusChanged(ValidateStatus value);
  void helpTextChanged(const QString& value);
  void extraTextChanged(const QString& value);
  void tooltipTextChanged(const QString& value);
  void errorMessagesChanged(const QStringList& messages);
  void warningMessagesChanged(const QStringList& messages);
  void itemLayoutChanged(ItemLayout value);
  void controlWidgetChanged(QWidget* value);
  void feedbackIconProviderChanged();
  void valueChanged(const QString& fieldName, const QVariant& value);
  void validationStateChanged(ValidateStatus status, const QStringList& errors,
                              const QStringList& warnings);

 protected:
  void changeEvent(QEvent* event) override;
  bool event(QEvent* event) override;

 private:
  friend class AdForm;

  void attachForm(AdForm* form);
  void ensureUi();
  void rebuildItemLayout(const detail::FormVisualStyle* resolvedStyle = nullptr);
  void refreshLabel(const detail::FormVisualStyle* resolvedStyle = nullptr);
  void refreshMessages(const detail::FormVisualStyle* resolvedStyle = nullptr);
  void refreshFeedbackIcon(const detail::FormVisualStyle* resolvedStyle = nullptr);
  void refreshControlStyle();
  void refreshAccessibility();
  void updateAdditionalSpacing(const detail::FormVisualStyle* resolvedStyle = nullptr);
  void clearControlFeedbackIcon();
  void setFallbackFeedbackSpinnerActive(bool active);
  void refreshNoStyleDescendantStatus();
  void notifyStyledParentOfNoStyleMetaChange();
  void bindControlValueSignals(QWidget* widget);
  void clearControlValueSignals();
  QVariant readControlValue() const;
  void writeControlValue(const QVariant& value);
  void emitCurrentValueChanged();
  void applyValidateStatus(ValidateStatus value, bool explicitStatus);
  bool hasDependencyOn(const QStringList& sourceNamePath) const;
  void setMetaState(bool touched, bool dirty);
  bool hasExplicitHelpText() const;
  bool hasVisibleHelpMessage() const;
  bool hasVisibleAdditionalText() const;
  AdFormItem* parentFormItem() const;
  AdFormItem* nearestStyledParentFormItem() const;
  bool hasNoStyleDescendantItems() const;
  QStringList collectedErrorMessages() const;
  QStringList collectedWarningMessages() const;
  ValidateStatus ownMessageStatus() const;
  ValidateStatus effectiveVisualStatus() const;
  bool effectiveHasFeedback() const;
  FeedbackIconProvider effectiveFeedbackIconProvider() const;
  AdForm::FormLayout effectiveFormLayout() const;
  bool effectiveVerticalLayout() const;
  bool effectiveInlineLayout() const;
  bool effectiveDisabled() const;

  QString label_;
  QString fieldName_;
  QStringList dependencies_;
  bool required_ = false;
  QString requiredMessage_;
  bool hasFeedback_ = false;
  bool hasFeedbackExplicit_ = false;
  bool noStyle_ = false;
  bool itemHidden_ = false;
  bool validateOnChange_ = true;
  int validateDebounceMs_ = 0;
  QVariant initialValue_;
  bool hasInitialValue_ = false;
  QString valuePropertyName_ = QStringLiteral("value");
  bool touched_ = false;
  bool dirty_ = false;
  ValidateStatus validateStatus_ = ValidateStatus::None;
  bool validateStatusExplicit_ = false;
  ItemLayout itemLayout_ = ItemLayout::Inherit;
  QString helpText_;
  QString extraText_;
  QString tooltipText_;
  QStringList errorMessages_;
  QStringList warningMessages_;
  QPointer<QWidget> controlWidget_;
  QPointer<AdForm> form_;
  Validator validator_;
  FormValidator formValidator_;
  ValueReader valueReader_;
  ValueWriter valueWriter_;
  ValueNormalizer valueNormalizer_;
  FeedbackIconProvider feedbackIconProvider_;
  QVector<QMetaObject::Connection> controlConnections_;
  QTimer* validationTimer_ = nullptr;
  QVariant previousValue_;
  adqt::icons::IconRef inputNumberSuffixIconBeforeFeedback_;
  QString inputNumberSuffixTextBeforeFeedback_;
  bool inputNumberFeedbackIconApplied_ = false;
  bool controlFeedbackIconPropertyApplied_ = false;
  bool fallbackFeedbackSpinnerSubscribed_ = false;
  bool formDisabledApplied_ = false;
  bool controlEnabledBeforeFormDisable_ = true;

  QGridLayout* itemLayoutGrid_ = nullptr;
  QWidget* labelHost_ = nullptr;
  QHBoxLayout* labelLayout_ = nullptr;
  QLabel* requiredMarkWidget_ = nullptr;
  QLabel* labelWidget_ = nullptr;
  QLabel* optionalWidget_ = nullptr;
  QLabel* tooltipWidget_ = nullptr;
  QLabel* colonWidget_ = nullptr;
  QWidget* controlHost_ = nullptr;
  QHBoxLayout* controlLayout_ = nullptr;
  QLabel* feedbackWidget_ = nullptr;
  QWidget* additionalHost_ = nullptr;
  QVBoxLayout* additionalLayout_ = nullptr;
  QLabel* helpWidget_ = nullptr;
  QLabel* extraWidget_ = nullptr;
};

class AdFormList final : public QWidget {
  Q_OBJECT

  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(int minRows READ minRows WRITE setMinRows NOTIFY minRowsChanged)
  Q_PROPERTY(int maxRows READ maxRows WRITE setMaxRows NOTIFY maxRowsChanged)
  Q_PROPERTY(
      QString addButtonText READ addButtonText WRITE setAddButtonText NOTIFY addButtonTextChanged)

 public:
  using RowFactory = std::function<QWidget*(int index, QWidget* parent)>;

  explicit AdFormList(QWidget* parent = nullptr);
  ~AdFormList() override;

  int count() const;

  int minRows() const;
  void setMinRows(int value);

  int maxRows() const;
  void setMaxRows(int value);

  QString addButtonText() const;
  void setAddButtonText(const QString& value);

  RowFactory rowFactory() const;
  void setRowFactory(RowFactory factory);

  QVariantList values() const;
  void setValues(const QVariantList& values);

  QWidget* rowWidget(int index) const;
  QVector<QWidget*> rowWidgets() const;

  void addRow(const QVariant& value = QVariant(), int index = -1);
  void removeRow(int index);
  void moveRow(int from, int to);

 signals:
  void countChanged(int count);
  void minRowsChanged(int value);
  void maxRowsChanged(int value);
  void addButtonTextChanged(const QString& value);
  void valuesChanged(const QVariantList& values);
  void rowAdded(int index);
  void rowRemoved(int index);
  void rowMoved(int from, int to);

 protected:
  void changeEvent(QEvent* event) override;

 private:
  struct Row {
    QPointer<QWidget> host;
    QPointer<QWidget> editor;
    QPointer<QToolButton> removeButton;
    QVector<QMetaObject::Connection> connections;
  };

  void ensureUi();
  QWidget* createRowEditor(int index, QWidget* parent);
  void bindRowSignals(Row& row);
  void clearRowSignals(Row& row);
  int indexOfHost(QWidget* host) const;
  void rebuildRowIndexes();
  void updateControls();
  void emitValuesChanged();

  RowFactory rowFactory_;
  QVector<Row> rows_;
  int minRows_ = 0;
  int maxRows_ = -1;
  QString addButtonText_;
  bool suppressValueSignal_ = false;

  QVBoxLayout* rootLayout_ = nullptr;
  QVBoxLayout* rowsLayout_ = nullptr;
  QPushButton* addButton_ = nullptr;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdForm::FormLayout)
Q_DECLARE_METATYPE(adqt::widgets::AdForm::LabelAlign)
Q_DECLARE_METATYPE(adqt::widgets::AdForm::RequiredMark)
Q_DECLARE_METATYPE(adqt::widgets::AdForm::ControlSize)
Q_DECLARE_METATYPE(adqt::widgets::AdForm::Variant)
Q_DECLARE_METATYPE(adqt::widgets::AdFormItem::ValidateStatus)
Q_DECLARE_METATYPE(adqt::widgets::AdFormItem::ItemLayout)
Q_DECLARE_METATYPE(adqt::widgets::AdFormItem::ValidationResult)
