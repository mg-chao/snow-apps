#include "form.h"

#include "antd_icons.h"
#include "button.h"
#include "form_style.h"
#include "input_line_edit.h"
#include "input_number.h"
#include "input_text_edit.h"
#include "theme/theme.h"
#include "widgets/detail/flow_layout.h"
#include "widgets/detail/form_value_adapter.h"
#include "widgets/detail/timing_hub.h"

#include <QAccessible>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaProperty>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace adqt::widgets {

namespace {

void setLabelTextColor(QLabel* label, const QColor& color) {
  if (!label) {
    return;
  }
  QPalette palette = label->palette();
  palette.setColor(QPalette::WindowText, color);
  palette.setColor(QPalette::Text, color);
  palette.setColor(QPalette::Disabled, QPalette::WindowText, color);
  palette.setColor(QPalette::Disabled, QPalette::Text, color);
  label->setPalette(palette);
}

constexpr char kFallbackFeedbackSpinnerFrameKey[] = "AdFormItem.FallbackFeedbackSpinnerFrame";

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

QPixmap renderFeedbackIconPixmap(const adqt::icons::IconRef& icon, int iconSize, qreal dpr,
                                 int rotationDegrees = 0) {
  if (!adqt::icons::isValid(icon)) {
    return {};
  }

  if (rotationDegrees == 0) {
    return adqt::icons::renderIconPixmap(icon, {QSize(iconSize, iconSize), dpr});
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

QString normalizedLabelForColon(QString value) {
  value = value.trimmed();
  while (!value.isEmpty()) {
    const QChar last = value.at(value.size() - 1);
    if (last == QLatin1Char(':') || last == QLatin1Char('|') || last == QChar(0xff1a) ||
        last.isSpace()) {
      value.chop(1);
      value = value.trimmed();
      continue;
    }
    break;
  }
  return value;
}

bool hasPaletteOverride(const QWidget* widget) {
  return widget && widget->testAttribute(Qt::WA_SetPalette);
}

detail::FormVisualStyle resolveStyleFor(const QWidget* widget, AdForm::ControlSize controlSize) {
  detail::FormStyleInput input;
  input.controlSize = controlSize;
  input.enabled = !widget || widget->isEnabled();
  input.hasPaletteOverride = hasPaletteOverride(widget);
  input.paletteGroup = widget ? widget->palette().currentColorGroup() : QPalette::Active;
  input.baseFont = widget ? widget->font() : QFont();
  input.palette = widget ? widget->palette() : QPalette();

  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::ThemeManager::instance().resolve(widget, widget);
  return detail::resolveFormVisualStyle(input, resolved);
}

int effectiveLabelColumnWidth(const AdForm* form) { return form ? form->labelColumnWidth() : 0; }

bool writeEnumProperty(QWidget* widget, const char* name, int value) {
  if (!widget) {
    return false;
  }
  const QMetaObject* meta = widget->metaObject();
  const int propertyIndex = meta ? meta->indexOfProperty(name) : -1;
  if (propertyIndex < 0) {
    return false;
  }
  QMetaProperty property = meta->property(propertyIndex);
  return property.isWritable() && property.write(widget, value);
}

bool writeIconRefProperty(QWidget* widget, const char* name, const adqt::icons::IconRef& value) {
  if (!widget) {
    return false;
  }
  const QMetaObject* meta = widget->metaObject();
  const int propertyIndex = meta ? meta->indexOfProperty(name) : -1;
  if (propertyIndex < 0) {
    return false;
  }
  QMetaProperty property = meta->property(propertyIndex);
  return property.isWritable() && property.write(widget, QVariant::fromValue(value));
}

bool canHostFeedbackIcon(QWidget* widget) {
  if (!widget) {
    return false;
  }
  if (qobject_cast<AdInputNumber*>(widget)) {
    return true;
  }
  const QMetaObject* meta = widget->metaObject();
  const int propertyIndex = meta ? meta->indexOfProperty("feedbackIconRef") : -1;
  if (propertyIndex < 0) {
    return false;
  }
  const QMetaProperty property = meta->property(propertyIndex);
  return property.isWritable();
}

int statusPropertyValue(AdFormItem::ValidateStatus status) {
  switch (status) {
    case AdFormItem::ValidateStatus::Error:
      return 1;
    case AdFormItem::ValidateStatus::Warning:
      return 2;
    case AdFormItem::ValidateStatus::None:
    case AdFormItem::ValidateStatus::Success:
    case AdFormItem::ValidateStatus::Validating:
    default:
      return 0;
  }
}

int controlSizePropertyValue(AdForm::ControlSize size) {
  switch (size) {
    case AdForm::ControlSize::Large:
      return 0;
    case AdForm::ControlSize::Small:
      return 2;
    case AdForm::ControlSize::Medium:
    default:
      return 1;
  }
}

int variantPropertyValue(AdForm::Variant variant) {
  switch (variant) {
    case AdForm::Variant::Outlined:
      return 0;
    case AdForm::Variant::Filled:
      return 1;
    case AdForm::Variant::Borderless:
      return 2;
    case AdForm::Variant::Underlined:
      return 3;
  }
  return 0;
}

bool isEmptyValue(const QVariant& value) {
  if (!value.isValid() || value.isNull()) {
    return true;
  }

  const int type = value.userType();
  if (type == QMetaType::QString) {
    return value.toString().trimmed().isEmpty();
  }
  if (type == QMetaType::QStringList) {
    return value.toStringList().isEmpty();
  }
  if (type == QMetaType::QVariantList) {
    return value.toList().isEmpty();
  }
  if (type == QMetaType::QVariantMap) {
    return value.toMap().isEmpty();
  }
  if (type == QMetaType::Bool) {
    return !value.toBool();
  }
  return false;
}

QColor messageColorForStatus(const detail::FormVisualStyle& style,
                             AdFormItem::ValidateStatus status, bool warningFallback) {
  switch (status) {
    case AdFormItem::ValidateStatus::Error:
      return style.errorColor;
    case AdFormItem::ValidateStatus::Warning:
      return style.warningColor;
    case AdFormItem::ValidateStatus::Success:
      return style.successColor;
    case AdFormItem::ValidateStatus::Validating:
      return style.validatingColor;
    case AdFormItem::ValidateStatus::None:
    default:
      return warningFallback ? style.warningColor : style.messageColor;
  }
}

adqt::icons::IconRef feedbackIconForStatus(AdFormItem::ValidateStatus status) {
  switch (status) {
    case AdFormItem::ValidateStatus::Success:
      return adqt::icons::antd::filled::CheckCircle();
    case AdFormItem::ValidateStatus::Warning:
      return adqt::icons::antd::filled::ExclamationCircle();
    case AdFormItem::ValidateStatus::Error:
      return adqt::icons::antd::filled::CloseCircle();
    case AdFormItem::ValidateStatus::Validating:
      return adqt::icons::antd::outlined::Loading();
    case AdFormItem::ValidateStatus::None:
    default:
      return {};
  }
}

QColor feedbackColorForStatus(const detail::FormVisualStyle& style,
                              AdFormItem::ValidateStatus status) {
  switch (status) {
    case AdFormItem::ValidateStatus::Success:
      return style.successColor;
    case AdFormItem::ValidateStatus::Warning:
      return style.warningColor;
    case AdFormItem::ValidateStatus::Error:
      return style.errorColor;
    case AdFormItem::ValidateStatus::Validating:
      return style.validatingColor;
    case AdFormItem::ValidateStatus::None:
    default:
      return QColor();
  }
}

int validationStatusPriority(AdFormItem::ValidateStatus status) {
  switch (status) {
    case AdFormItem::ValidateStatus::Error:
      return 4;
    case AdFormItem::ValidateStatus::Warning:
      return 3;
    case AdFormItem::ValidateStatus::Validating:
      return 2;
    case AdFormItem::ValidateStatus::Success:
      return 1;
    case AdFormItem::ValidateStatus::None:
    default:
      return 0;
  }
}

AdFormItem::ValidationResult normalizedValidationResult(AdFormItem::ValidationResult result) {
  if (!result.errors.isEmpty()) {
    result.status = AdFormItem::ValidateStatus::Error;
  } else if (!result.warnings.isEmpty() && result.status != AdFormItem::ValidateStatus::Error) {
    result.status = AdFormItem::ValidateStatus::Warning;
  } else if (result.status == AdFormItem::ValidateStatus::None) {
    result.status = AdFormItem::ValidateStatus::Success;
  }
  return result;
}

void mergeValidationResult(AdFormItem::ValidationResult& target,
                           AdFormItem::ValidationResult next) {
  next = normalizedValidationResult(std::move(next));
  target.errors.append(next.errors);
  target.warnings.append(next.warnings);
  if (!next.helpText.isNull()) {
    target.helpText = next.helpText;
  }
  if (validationStatusPriority(next.status) > validationStatusPriority(target.status)) {
    target.status = next.status;
  }
  if (!target.errors.isEmpty()) {
    target.status = AdFormItem::ValidateStatus::Error;
  } else if (!target.warnings.isEmpty() && target.status != AdFormItem::ValidateStatus::Error) {
    target.status = AdFormItem::ValidateStatus::Warning;
  }
}

QString formatRequiredMessage(QString message, const QString& labelText) {
  if (message.isEmpty()) {
    return message;
  }
  if (message.contains(QStringLiteral("%1"))) {
    return message.arg(labelText);
  }
  message.replace(QStringLiteral("${label}"), labelText);
  message.replace(QStringLiteral("{label}"), labelText);
  return message;
}

void notifyAccessibleNameChanged(QWidget* widget) {
  if (!widget) {
    return;
  }
  QAccessibleEvent event(widget, QAccessible::NameChanged);
  QAccessible::updateAccessibility(&event);
}

QScrollArea* enclosingScrollArea(QWidget* widget) {
  for (QWidget* current = widget ? widget->parentWidget() : nullptr; current;
       current = current->parentWidget()) {
    if (auto* scrollArea = qobject_cast<QScrollArea*>(current)) {
      return scrollArea;
    }
  }
  return nullptr;
}

void focusControlWidget(QWidget* widget) {
  if (!widget) {
    return;
  }
  if (auto* lineEdit = qobject_cast<AdLineEdit*>(widget)) {
    lineEdit->focusEditor(AdLineEdit::FocusSelection::Preserve, true);
    return;
  }
  if (auto* textEdit = qobject_cast<AdTextEdit*>(widget)) {
    textEdit->focusEditor(AdLineEdit::FocusSelection::Preserve, true);
    return;
  }
  if (auto* inputNumber = qobject_cast<AdInputNumber*>(widget)) {
    inputNumber->focusEditor(AdInputNumber::FocusSelection::Preserve);
    return;
  }
  widget->setFocus(Qt::OtherFocusReason);
}

bool itemMatchesFieldName(const AdFormItem* item, const QString& fieldName) {
  if (!item) {
    return false;
  }
  return item->fieldName() == fieldName ||
         detail::sameFieldPath(item->namePath(), detail::parseFieldPath(fieldName));
}

}  // namespace

AdForm::AdForm(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("ad-form"));
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  rebuildRootLayout();
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { refreshItems(); });
}

AdForm::~AdForm() {
  if (auto* flow = dynamic_cast<detail::FlowLayout*>(rootLayout_)) {
    flow->setItemEndSpacingProvider({});
  }
  for (const QPointer<AdFormItem>& item : std::as_const(items_)) {
    if (item) {
      QObject::disconnect(item, nullptr, this, nullptr);
      item->form_.clear();
    }
  }
}

AdForm::FormLayout AdForm::formLayout() const { return formLayout_; }

void AdForm::setFormLayout(FormLayout value) {
  if (formLayout_ == value) {
    return;
  }
  formLayout_ = value;
  rebuildRootLayout();
  emit formLayoutChanged(formLayout_);
  refreshItems();
}

AdForm::LabelAlign AdForm::labelAlign() const { return labelAlign_; }

void AdForm::setLabelAlign(LabelAlign value) {
  if (labelAlign_ == value) {
    return;
  }
  labelAlign_ = value;
  emit labelAlignChanged(labelAlign_);
  refreshItems();
}

AdForm::RequiredMark AdForm::requiredMark() const { return requiredMark_; }

void AdForm::setRequiredMark(RequiredMark value) {
  if (requiredMark_ == value) {
    return;
  }
  requiredMark_ = value;
  emit requiredMarkChanged(requiredMark_);
  refreshItems();
}

AdForm::ControlSize AdForm::controlSize() const { return controlSize_; }

void AdForm::setControlSize(ControlSize value) {
  if (controlSize_ == value) {
    return;
  }
  controlSize_ = value;
  emit controlSizeChanged(controlSize_);
  refreshItems();
}

AdForm::Variant AdForm::variant() const { return variant_; }

void AdForm::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  variant_ = value;
  emit variantChanged(variant_);
  refreshItems();
}

bool AdForm::colon() const { return colon_; }

void AdForm::setColon(bool value) {
  if (colon_ == value) {
    return;
  }
  colon_ = value;
  emit colonChanged(colon_);
  refreshItems();
}

bool AdForm::labelWrap() const { return labelWrap_; }

void AdForm::setLabelWrap(bool value) {
  if (labelWrap_ == value) {
    return;
  }
  labelWrap_ = value;
  emit labelWrapChanged(labelWrap_);
  refreshItems();
}

bool AdForm::disabled() const { return disabled_; }

void AdForm::setDisabled(bool value) {
  if (disabled_ == value) {
    return;
  }
  if (value) {
    for (const QPointer<AdFormItem>& item : items_) {
      if (!item || item->formDisabledApplied_) {
        continue;
      }
      QWidget* control = item->controlWidget();
      if (!control) {
        continue;
      }
      item->controlEnabledBeforeFormDisable_ = control->isEnabled();
      item->formDisabledApplied_ = true;
    }
  }
  disabled_ = value;
  QWidget::setDisabled(value);
  emit disabledChanged(disabled_);
  refreshItems();
}

bool AdForm::scrollToFirstError() const { return scrollToFirstError_; }

void AdForm::setScrollToFirstError(bool value) {
  if (scrollToFirstError_ == value) {
    return;
  }
  scrollToFirstError_ = value;
  emit scrollToFirstErrorChanged(scrollToFirstError_);
}

int AdForm::labelColumnWidth() const { return labelColumnWidth_; }

void AdForm::setLabelColumnWidth(int value) {
  value = std::max(0, value);
  if (labelColumnWidth_ == value) {
    return;
  }
  labelColumnWidth_ = value;
  emit labelColumnWidthChanged(labelColumnWidth_);
  refreshItems();
}

QString AdForm::name() const { return name_; }

void AdForm::setName(const QString& value) {
  if (name_ == value) {
    return;
  }
  name_ = value;
  setObjectName(name_.isEmpty() ? QStringLiteral("ad-form") : name_);
  emit nameChanged(name_);
}

QVariantMap AdForm::initialValues() const { return initialValues_; }

void AdForm::setInitialValues(const QVariantMap& values) {
  if (initialValues_ == values) {
    return;
  }
  initialValues_ = values;
  emit initialValuesChanged(initialValues_);

  for (const QPointer<AdFormItem>& item : items_) {
    if (!item || item->isTouched()) {
      continue;
    }
    QVariant initialValue;
    if (initialValueForItem(item, &initialValue)) {
      item->setValue(initialValue);
      item->resetValidation();
      item->setMetaState(false, false);
    }
  }
}

QString AdForm::requiredMessageTemplate() const { return requiredMessageTemplate_; }

void AdForm::setRequiredMessageTemplate(const QString& value) {
  if (requiredMessageTemplate_ == value) {
    return;
  }
  requiredMessageTemplate_ = value;
  emit requiredMessageTemplateChanged(requiredMessageTemplate_);
}

AdFormItem* AdForm::addItem(const QString& label, QWidget* controlWidget,
                            const QString& fieldName) {
  auto* item = new AdFormItem(label, controlWidget, fieldName, this);
  addItem(item);
  return item;
}

AdFormItem* AdForm::addField(const QString& label, QWidget* editor, const QString& fieldKey) {
  return addItem(label, editor, fieldKey);
}

void AdForm::addItem(AdFormItem* item) { insertItem(static_cast<int>(items_.size()), item); }

void AdForm::insertItem(int index, AdFormItem* item) {
  if (!item) {
    return;
  }
  if (items_.contains(item)) {
    return;
  }
  index = std::clamp(index, 0, static_cast<int>(items_.size()));
  items_.insert(index, item);
  attachItem(item);
  rebuildRootLayout();
  emit itemAdded(item);
}

void AdForm::removeItem(AdFormItem* item) {
  if (!item) {
    return;
  }
  const int index = static_cast<int>(items_.indexOf(item));
  if (index < 0) {
    return;
  }
  items_.removeAt(index);
  if (rootLayout_) {
    rootLayout_->removeWidget(item);
  }
  detachItem(item);
  emit itemRemoved(item);
}

QVector<AdFormItem*> AdForm::items() const {
  QVector<AdFormItem*> result;
  result.reserve(items_.size());
  for (const QPointer<AdFormItem>& item : items_) {
    if (item) {
      result.append(item);
    }
  }
  return result;
}

AdFormItem* AdForm::itemForName(const QString& fieldName) const {
  for (const QPointer<AdFormItem>& item : items_) {
    if (item && item->fieldName() == fieldName) {
      return item;
    }
  }
  return nullptr;
}

AdFormItem* AdForm::itemForNamePath(const QStringList& namePath) const {
  for (const QPointer<AdFormItem>& item : items_) {
    if (item && detail::sameFieldPath(item->namePath(), namePath)) {
      return item;
    }
  }
  return nullptr;
}

AdFormItem* AdForm::field(const QString& fieldKey) const { return itemForName(fieldKey); }

AdFormItem* AdForm::fieldAtPath(const QStringList& fieldPath) const {
  return itemForNamePath(fieldPath);
}

QStringList AdForm::fieldKeys() const {
  QStringList result;
  for (const QPointer<AdFormItem>& item : items_) {
    if (item && !item->fieldKey().isEmpty()) {
      result.append(item->fieldKey());
    }
  }
  return result;
}

QVariantMap AdForm::values() const {
  QVariant root = QVariantMap();
  for (const QPointer<AdFormItem>& item : items_) {
    if (!item || item->fieldName().isEmpty()) {
      continue;
    }
    const QStringList path = item->namePath();
    if (path.isEmpty()) {
      continue;
    }
    root = detail::setValueAtFieldPath(root, path, 0, item->value());
  }
  return root.toMap();
}

QVariantMap AdForm::fieldValues() const { return values(); }

QVariantMap AdForm::flatValues() const {
  QVariantMap result;
  for (const QPointer<AdFormItem>& item : items_) {
    if (!item || item->fieldName().isEmpty()) {
      continue;
    }
    result.insert(item->fieldName(), item->value());
  }
  return result;
}

void AdForm::setValues(const QVariantMap& values) {
  const QVariant root(values);
  const QVariantMap& flat = values;
  for (const QPointer<AdFormItem>& item : items_) {
    if (!item || item->fieldName().isEmpty()) {
      continue;
    }

    bool found = false;
    QVariant next = detail::valueAtFieldPath(root, item->namePath(), &found);
    if (!found && flat.contains(item->fieldName())) {
      next = flat.value(item->fieldName());
      found = true;
    }
    if (found) {
      item->setValue(next);
    }
  }
}

void AdForm::setFieldValues(const QVariantMap& values) { setValues(values); }

QVariant AdForm::value(const QString& fieldName) const {
  if (AdFormItem* item = itemForName(fieldName)) {
    return item->value();
  }
  bool found = false;
  const QVariant next =
      detail::valueAtFieldPath(QVariant(values()), detail::parseFieldPath(fieldName), &found);
  return found ? next : QVariant();
}

QVariant AdForm::value(const QStringList& namePath) const {
  if (AdFormItem* item = itemForNamePath(namePath)) {
    return item->value();
  }
  bool found = false;
  const QVariant next = detail::valueAtFieldPath(QVariant(values()), namePath, &found);
  return found ? next : QVariant();
}

QVariant AdForm::fieldValue(const QString& fieldKey) const { return value(fieldKey); }

QVariant AdForm::fieldValue(const QStringList& fieldPath) const { return value(fieldPath); }

void AdForm::setValue(const QString& fieldName, const QVariant& value) {
  if (AdFormItem* item = itemForName(fieldName)) {
    item->setValue(value);
    return;
  }
  setValue(detail::parseFieldPath(fieldName), value);
}

void AdForm::setValue(const QStringList& namePath, const QVariant& value) {
  if (AdFormItem* item = itemForNamePath(namePath)) {
    item->setValue(value);
  }
}

void AdForm::setFieldValue(const QString& fieldKey, const QVariant& value) {
  setValue(fieldKey, value);
}

void AdForm::setFieldValue(const QStringList& fieldPath, const QVariant& value) {
  setValue(fieldPath, value);
}

bool AdForm::validate() { return validateFields().isEmpty(); }

QVector<AdFormItem*> AdForm::validateFields(const QStringList& fieldNames) {
  QVector<AdFormItem*> invalid;
  for (const QPointer<AdFormItem>& item : items_) {
    if (!item) {
      continue;
    }

    bool include = fieldNames.isEmpty();
    for (const QString& fieldName : fieldNames) {
      if (itemMatchesFieldName(item, fieldName)) {
        include = true;
        break;
      }
    }
    if (!include) {
      continue;
    }

    if (!item->validate()) {
      invalid.append(item);
    }
  }
  emit validationFinished(invalid.isEmpty());
  return invalid;
}

bool AdForm::validateField(const QString& fieldName) {
  if (AdFormItem* item = itemForName(fieldName)) {
    const bool valid = item->validate();
    emit validationFinished(valid);
    return valid;
  }
  return validateField(detail::parseFieldPath(fieldName));
}

bool AdForm::validateField(const QStringList& namePath) {
  AdFormItem* item = itemForNamePath(namePath);
  const bool valid = !item || item->validate();
  emit validationFinished(valid);
  return valid;
}

QVector<AdFormItem*> AdForm::invalidItems() const {
  QVector<AdFormItem*> invalid;
  for (const QPointer<AdFormItem>& item : items_) {
    if (item && item->validateStatus() == AdFormItem::ValidateStatus::Error) {
      invalid.append(item);
    }
  }
  return invalid;
}

void AdForm::resetValidation() {
  for (const QPointer<AdFormItem>& item : items_) {
    if (item) {
      item->resetValidation();
    }
  }
}

void AdForm::resetFields() { resetFields(QStringList()); }

void AdForm::resetFields(const QStringList& fieldNames) {
  for (const QPointer<AdFormItem>& item : items_) {
    if (!item) {
      continue;
    }

    bool include = fieldNames.isEmpty();
    for (const QString& fieldName : fieldNames) {
      if (itemMatchesFieldName(item, fieldName)) {
        include = true;
        break;
      }
    }
    if (include) {
      resetItemToInitial(item);
    }
  }
}

void AdForm::resetField(const QString& fieldName) {
  if (AdFormItem* item = itemForName(fieldName)) {
    resetItemToInitial(item);
    return;
  }
  resetField(detail::parseFieldPath(fieldName));
}

void AdForm::resetField(const QStringList& namePath) {
  if (AdFormItem* item = itemForNamePath(namePath)) {
    resetItemToInitial(item);
  }
}

bool AdForm::isFieldTouched(const QString& fieldName) const {
  if (AdFormItem* item = itemForName(fieldName)) {
    return item->isTouched();
  }
  return isFieldTouched(detail::parseFieldPath(fieldName));
}

bool AdForm::isFieldTouched(const QStringList& namePath) const {
  AdFormItem* item = itemForNamePath(namePath);
  return item && item->isTouched();
}

bool AdForm::isFieldsTouched(bool allTouched) const {
  bool sawField = false;
  for (const QPointer<AdFormItem>& item : items_) {
    if (!item || item->fieldName().isEmpty()) {
      continue;
    }
    sawField = true;
    if (allTouched && !item->isTouched()) {
      return false;
    }
    if (!allTouched && item->isTouched()) {
      return true;
    }
  }
  return allTouched ? sawField : false;
}

bool AdForm::isFieldDirty(const QString& fieldName) const {
  if (AdFormItem* item = itemForName(fieldName)) {
    return item->isDirty();
  }
  return isFieldDirty(detail::parseFieldPath(fieldName));
}

bool AdForm::isFieldDirty(const QStringList& namePath) const {
  AdFormItem* item = itemForNamePath(namePath);
  return item && item->isDirty();
}

bool AdForm::submit() {
  QVector<AdFormItem*> invalidItems = validateFields();

  const bool valid = invalidItems.isEmpty();
  if (valid) {
    emit submitSucceeded(values());
  } else {
    if (scrollToFirstError_ && !invalidItems.isEmpty()) {
      scrollToField(invalidItems.first()->namePath(), true);
    }
    emit submitFailed(invalidItems);
  }
  return valid;
}

bool AdForm::scrollToField(const QString& fieldName, bool focusControl) {
  if (AdFormItem* item = itemForName(fieldName)) {
    return scrollToField(item->namePath(), focusControl);
  }
  return scrollToField(detail::parseFieldPath(fieldName), focusControl);
}

bool AdForm::scrollToField(const QStringList& namePath, bool focusControl) {
  AdFormItem* item = itemForNamePath(namePath);
  if (!item) {
    return false;
  }
  if (QScrollArea* scrollArea = enclosingScrollArea(item)) {
    scrollArea->ensureWidgetVisible(item, 24, 24);
  }
  if (focusControl) {
    focusControlWidget(item->controlWidget());
  }
  return true;
}

void AdForm::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }
  switch (event->type()) {
    case QEvent::EnabledChange:
      if (disabled_ != !isEnabled()) {
        disabled_ = !isEnabled();
        emit disabledChanged(disabled_);
      }
      refreshItems();
      break;
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::LanguageChange:
      refreshItems();
      break;
    default:
      break;
  }
}

void AdForm::rebuildRootLayout() {
  if (rootLayout_) {
    while (QLayoutItem* layoutItem = rootLayout_->takeAt(0)) {
      delete layoutItem;
    }
    delete rootLayout_;
    rootLayout_ = nullptr;
  }

  if (formLayout_ == FormLayout::Inline) {
    const int inlineItemGap = resolveStyleFor(this, controlSize_).metrics.inlineItemGap;
    auto* flow = new detail::FlowLayout(this, 0, 0, 0);
    flow->setItemEndSpacingProvider([inlineItemGap](const QLayoutItem* layoutItem) {
      const auto* item =
          layoutItem ? qobject_cast<const AdFormItem*>(layoutItem->widget()) : nullptr;
      if (!item || item->itemLayout() != AdFormItem::ItemLayout::Inherit) {
        return 0;
      }
      return inlineItemGap;
    });
    rootLayout_ = flow;
  } else {
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(0);
    rootLayout_ = box;
  }

  for (const QPointer<AdFormItem>& item : items_) {
    if (item) {
      rootLayout_->addWidget(item);
    }
  }
  refreshItems();
}

void AdForm::refreshItems() {
  if (auto* flow = dynamic_cast<detail::FlowLayout*>(rootLayout_)) {
    const int inlineItemGap = resolveStyleFor(this, controlSize_).metrics.inlineItemGap;
    flow->setItemEndSpacingProvider([inlineItemGap](const QLayoutItem* layoutItem) {
      const auto* item =
          layoutItem ? qobject_cast<const AdFormItem*>(layoutItem->widget()) : nullptr;
      return item && item->itemLayout() == AdFormItem::ItemLayout::Inherit ? inlineItemGap : 0;
    });
  }

  for (const QPointer<AdFormItem>& item : items_) {
    if (item) {
      item->refresh();
    }
  }
  updateGeometry();
}

void AdForm::attachItem(AdFormItem* item) {
  if (!item) {
    return;
  }
  if (item->parentWidget() != this) {
    item->setParent(this);
  }
  item->attachForm(this);
  QVariant initialValue;
  if (initialValueForItem(item, &initialValue)) {
    item->setValue(initialValue);
    item->resetValidation();
    item->setMetaState(false, false);
  }
  connect(item, &AdFormItem::valueChanged, this,
          [this, item](const QString&, const QVariant&) { itemValueChanged(item); });
  connect(item, &QObject::destroyed, this, [this, item]() { notifyItemDestroyed(item); });
  item->refresh();
}

void AdForm::detachItem(AdFormItem* item) {
  if (!item) {
    return;
  }
  QObject::disconnect(item, nullptr, this, nullptr);
  item->attachForm(nullptr);
  item->setParent(nullptr);
}

void AdForm::itemValueChanged(AdFormItem* item) {
  if (!item) {
    return;
  }
  QVariant initialValue;
  const bool hasInitialValue = initialValueForItem(item, &initialValue);
  item->setMetaState(true, !hasInitialValue || item->value() != initialValue);
  emit fieldValueChanged(item->fieldName(), item->value());
  emit fieldChanged(item->fieldKey(), item->value());
  QVariant changedRoot = QVariantMap();
  const QStringList path = item->namePath();
  if (!path.isEmpty()) {
    changedRoot = detail::setValueAtFieldPath(changedRoot, path, 0, item->value());
  }
  emit fieldPathValueChanged(path, item->value());
  emit valuesChanged(changedRoot.toMap(), values());
  revalidateDependents(item);
}

void AdForm::revalidateDependents(AdFormItem* sourceItem) {
  if (!sourceItem) {
    return;
  }
  const QStringList sourcePath = sourceItem->namePath();
  if (sourcePath.isEmpty()) {
    return;
  }
  for (const QPointer<AdFormItem>& item : items_) {
    if (!item || item == sourceItem) {
      continue;
    }
    if (item->hasDependencyOn(sourcePath)) {
      item->scheduleValidation();
    }
  }
}

bool AdForm::initialValueForItem(AdFormItem* item, QVariant* valueOut) const {
  if (!item || item->fieldName().isEmpty()) {
    return false;
  }
  const QVariant root(initialValues_);
  bool found = false;
  QVariant value = detail::valueAtFieldPath(root, item->namePath(), &found);
  if (!found && initialValues_.contains(item->fieldName())) {
    value = initialValues_.value(item->fieldName());
    found = true;
  }
  if (!found && item->hasInitialValue()) {
    value = item->initialValue();
    found = true;
  }
  if (found && valueOut) {
    *valueOut = value;
  }
  return found;
}

void AdForm::resetItemToInitial(AdFormItem* item) {
  if (!item) {
    return;
  }
  QVariant initialValue;
  item->setValue(initialValueForItem(item, &initialValue) ? initialValue : QVariant());
  item->resetValidation();
  item->setMetaState(false, false);
}

void AdForm::notifyItemDestroyed(AdFormItem* item) {
  for (qsizetype i = items_.size() - 1; i >= 0; --i) {
    if (!items_.at(i) || items_.at(i).data() == item) {
      items_.removeAt(i);
    }
  }
}

AdFormItem::AdFormItem(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("ad-form-item"));
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  ensureUi();
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { refresh(); });
}

AdFormItem::AdFormItem(const QString& label, QWidget* controlWidget, const QString& fieldName,
                       QWidget* parent)
    : AdFormItem(parent) {
  label_ = label;
  fieldName_ = fieldName;
  setControlWidget(controlWidget);
}

AdFormItem::~AdFormItem() { clearControlValueSignals(); }

QString AdFormItem::label() const { return label_; }

void AdFormItem::setLabel(const QString& value) {
  if (label_ == value && label_.isNull() == value.isNull()) {
    return;
  }
  label_ = value;
  emit labelChanged(label_);
  refresh();
}

QString AdFormItem::fieldKey() const { return fieldName_; }

void AdFormItem::setFieldKey(const QString& value) { setFieldName(value); }

QString AdFormItem::fieldName() const { return fieldName_; }

void AdFormItem::setFieldName(const QString& value) {
  if (fieldName_ == value) {
    return;
  }
  const QStringList previousPath = namePath();
  fieldName_ = value;
  emit fieldKeyChanged(fieldName_);
  emit fieldNameChanged(fieldName_);
  if (!detail::sameFieldPath(previousPath, namePath())) {
    emit fieldPathChanged(namePath());
    emit namePathChanged(namePath());
  }
  refreshAccessibility();
}

QStringList AdFormItem::fieldPath() const { return namePath(); }

void AdFormItem::setFieldPath(const QStringList& value) { setNamePath(value); }

QStringList AdFormItem::namePath() const { return detail::parseFieldPath(fieldName_); }

void AdFormItem::setNamePath(const QStringList& value) {
  const QString normalized = detail::joinFieldPath(value);
  if (fieldName_ == normalized) {
    return;
  }
  const QStringList previousPath = namePath();
  fieldName_ = normalized;
  emit fieldKeyChanged(fieldName_);
  emit fieldNameChanged(fieldName_);
  if (!detail::sameFieldPath(previousPath, namePath())) {
    emit fieldPathChanged(namePath());
    emit namePathChanged(namePath());
  }
  refreshAccessibility();
}

QStringList AdFormItem::dependencies() const { return dependencies_; }

void AdFormItem::setDependencies(const QStringList& value) {
  QStringList normalized;
  normalized.reserve(value.size());
  for (const QString& dependency : value) {
    const QString normalizedDependency = detail::joinFieldPath(detail::parseFieldPath(dependency));
    if (!normalizedDependency.isEmpty() && !normalized.contains(normalizedDependency)) {
      normalized.append(normalizedDependency);
    }
  }
  if (dependencies_ == normalized) {
    return;
  }
  dependencies_ = normalized;
  emit dependenciesChanged(dependencies_);
}

bool AdFormItem::required() const { return required_; }

void AdFormItem::setRequired(bool value) {
  if (required_ == value) {
    return;
  }
  required_ = value;
  emit requiredChanged(required_);
  refresh();
}

QString AdFormItem::requiredMessage() const { return requiredMessage_; }

void AdFormItem::setRequiredMessage(const QString& value) {
  if (requiredMessage_ == value) {
    return;
  }
  requiredMessage_ = value;
  emit requiredMessageChanged(requiredMessage_);
}

bool AdFormItem::hasFeedback() const { return hasFeedback_; }

void AdFormItem::setHasFeedback(bool value) {
  const bool valueChanged = hasFeedback_ != value;
  const bool explicitChanged = !hasFeedbackExplicit_;
  if (!valueChanged && !explicitChanged) {
    return;
  }
  hasFeedback_ = value;
  hasFeedbackExplicit_ = true;
  if (valueChanged) {
    emit hasFeedbackChanged(hasFeedback_);
  }
  refresh();
}

bool AdFormItem::noStyle() const { return noStyle_; }

void AdFormItem::setNoStyle(bool value) {
  if (noStyle_ == value) {
    return;
  }
  noStyle_ = value;
  emit noStyleChanged(noStyle_);
  refresh();
}

bool AdFormItem::itemHidden() const { return itemHidden_; }

void AdFormItem::setItemHidden(bool value) {
  if (itemHidden_ == value) {
    return;
  }
  itemHidden_ = value;
  QWidget::setVisible(!itemHidden_);
  emit itemHiddenChanged(itemHidden_);
  refresh();
}

bool AdFormItem::validateOnChange() const { return validateOnChange_; }

void AdFormItem::setValidateOnChange(bool value) {
  if (validateOnChange_ == value) {
    return;
  }
  validateOnChange_ = value;
  emit validateOnChangeChanged(validateOnChange_);
}

int AdFormItem::validateDebounceMs() const { return validateDebounceMs_; }

void AdFormItem::setValidateDebounceMs(int value) {
  value = std::max(0, value);
  if (validateDebounceMs_ == value) {
    return;
  }
  validateDebounceMs_ = value;
  if (validateDebounceMs_ == 0 && validationTimer_) {
    validationTimer_->stop();
  }
  emit validateDebounceMsChanged(validateDebounceMs_);
}

QVariant AdFormItem::initialValue() const { return initialValue_; }

void AdFormItem::setInitialValue(const QVariant& value) {
  if (hasInitialValue_ && initialValue_ == value) {
    return;
  }
  initialValue_ = value;
  hasInitialValue_ = true;
  emit initialValueChanged(initialValue_);

  if (form_ && !isTouched()) {
    QVariant formInitialValue;
    if (!form_->initialValueForItem(this, &formInitialValue) || formInitialValue == initialValue_) {
      setValue(initialValue_);
      resetValidation();
      setMetaState(false, false);
    }
  }
}

bool AdFormItem::hasInitialValue() const { return hasInitialValue_; }

void AdFormItem::clearInitialValue() {
  if (!hasInitialValue_) {
    return;
  }
  hasInitialValue_ = false;
  initialValue_ = QVariant();
  emit initialValueChanged(initialValue_);
}

QString AdFormItem::valuePropertyName() const { return valuePropertyName_; }

void AdFormItem::setValuePropertyName(const QString& value) {
  const QString normalized = value.trimmed().isEmpty() ? QStringLiteral("value") : value.trimmed();
  if (valuePropertyName_ == normalized) {
    return;
  }
  valuePropertyName_ = normalized;
  emit valuePropertyNameChanged(valuePropertyName_);
  emit controlValuePropertyChanged(valuePropertyName_);
}

QString AdFormItem::controlValueProperty() const { return valuePropertyName_; }

void AdFormItem::setControlValueProperty(const QString& value) { setValuePropertyName(value); }

bool AdFormItem::isTouched() const { return touched_; }

bool AdFormItem::isDirty() const { return dirty_; }

AdFormItem::ValidateStatus AdFormItem::validateStatus() const { return validateStatus_; }

void AdFormItem::setValidateStatus(ValidateStatus value) { applyValidateStatus(value, true); }

AdFormItem::ItemLayout AdFormItem::itemLayout() const { return itemLayout_; }

void AdFormItem::setItemLayout(ItemLayout value) {
  if (itemLayout_ == value) {
    return;
  }
  itemLayout_ = value;
  rebuildItemLayout();
  emit itemLayoutChanged(itemLayout_);
  refresh();
}

QString AdFormItem::helpText() const { return helpText_; }

void AdFormItem::setHelpText(const QString& value) {
  if (helpText_ == value) {
    return;
  }
  helpText_ = value;
  emit helpTextChanged(helpText_);
  refresh();
}

void AdFormItem::clearHelpText() {
  if (helpText_.isNull()) {
    return;
  }
  helpText_ = QString();
  helpText_.clear();
  emit helpTextChanged(helpText_);
  refresh();
}

QString AdFormItem::extraText() const { return extraText_; }

void AdFormItem::setExtraText(const QString& value) {
  if (extraText_ == value) {
    return;
  }
  extraText_ = value;
  emit extraTextChanged(extraText_);
  refresh();
}

QString AdFormItem::tooltipText() const { return tooltipText_; }

void AdFormItem::setTooltipText(const QString& value) {
  if (tooltipText_ == value) {
    return;
  }
  tooltipText_ = value;
  emit tooltipTextChanged(tooltipText_);
  refresh();
}

QStringList AdFormItem::errorMessages() const { return errorMessages_; }

void AdFormItem::setErrorMessages(const QStringList& messages) {
  if (errorMessages_ == messages) {
    return;
  }
  errorMessages_ = messages;
  if (!errorMessages_.isEmpty()) {
    applyValidateStatus(ValidateStatus::Error, false);
  } else if (validateStatus_ == ValidateStatus::Error) {
    applyValidateStatus(warningMessages_.isEmpty() ? ValidateStatus::None : ValidateStatus::Warning,
                        false);
  } else {
    refresh();
  }
  emit errorMessagesChanged(errorMessages_);
  emit validationStateChanged(validateStatus_, errorMessages_, warningMessages_);
}

QStringList AdFormItem::warningMessages() const { return warningMessages_; }

void AdFormItem::setWarningMessages(const QStringList& messages) {
  if (warningMessages_ == messages) {
    return;
  }
  warningMessages_ = messages;
  if (errorMessages_.isEmpty() && !warningMessages_.isEmpty()) {
    applyValidateStatus(ValidateStatus::Warning, false);
  } else if (warningMessages_.isEmpty() && validateStatus_ == ValidateStatus::Warning) {
    applyValidateStatus(errorMessages_.isEmpty() ? ValidateStatus::None : ValidateStatus::Error,
                        false);
  } else {
    refresh();
  }
  emit warningMessagesChanged(warningMessages_);
  emit validationStateChanged(validateStatus_, errorMessages_, warningMessages_);
}

QWidget* AdFormItem::controlWidget() const { return controlWidget_.data(); }

void AdFormItem::setControlWidget(QWidget* widget) {
  if (controlWidget_ == widget) {
    return;
  }

  clearControlFeedbackIcon();
  clearControlValueSignals();
  QWidget* previous = controlWidget_.data();
  if (previous && controlLayout_) {
    controlLayout_->removeWidget(previous);
    previous->hide();
    previous->setParent(nullptr);
  }

  controlWidget_ = widget;
  formDisabledApplied_ = false;
  controlEnabledBeforeFormDisable_ = true;

  if (widget) {
    widget->hide();
    if (widget->parentWidget() != controlHost_) {
      widget->setParent(controlHost_);
    }
    if (controlLayout_ && controlLayout_->indexOf(widget) < 0) {
      controlLayout_->insertWidget(0, widget);
    }
    bindControlValueSignals(widget);
    widget->show();
  }

  emit controlWidgetChanged(controlWidget_.data());
  refresh();
}

QWidget* AdFormItem::takeControlWidget() {
  QWidget* widget = controlWidget_.data();
  if (!widget) {
    return nullptr;
  }
  clearControlFeedbackIcon();
  clearControlValueSignals();
  if (controlLayout_) {
    controlLayout_->removeWidget(widget);
  }
  controlWidget_.clear();
  formDisabledApplied_ = false;
  widget->hide();
  widget->setParent(nullptr);
  emit controlWidgetChanged(nullptr);
  refresh();
  return widget;
}

AdForm* AdFormItem::formWidget() const { return form_.data(); }

AdFormItem::Validator AdFormItem::validator() const { return validator_; }

void AdFormItem::setValidator(Validator validator) { validator_ = std::move(validator); }

AdFormItem::FormValidator AdFormItem::formValidator() const { return formValidator_; }

void AdFormItem::setFormValidator(FormValidator validator) {
  formValidator_ = std::move(validator);
}

AdFormItem::ValueReader AdFormItem::valueReader() const { return valueReader_; }

void AdFormItem::setValueReader(ValueReader reader) { valueReader_ = std::move(reader); }

void AdFormItem::resetValueReader() { valueReader_ = {}; }

AdFormItem::ValueWriter AdFormItem::valueWriter() const { return valueWriter_; }

void AdFormItem::setValueWriter(ValueWriter writer) { valueWriter_ = std::move(writer); }

void AdFormItem::resetValueWriter() { valueWriter_ = {}; }

AdFormItem::ValueNormalizer AdFormItem::valueNormalizer() const { return valueNormalizer_; }

void AdFormItem::setValueNormalizer(ValueNormalizer normalizer) {
  valueNormalizer_ = std::move(normalizer);
}

void AdFormItem::resetValueNormalizer() { valueNormalizer_ = {}; }

AdFormItem::FeedbackIconProvider AdFormItem::feedbackIconProvider() const {
  return feedbackIconProvider_;
}

void AdFormItem::setFeedbackIconProvider(FeedbackIconProvider provider) {
  feedbackIconProvider_ = std::move(provider);
  emit feedbackIconProviderChanged();
  refreshFeedbackIcon();
}

void AdFormItem::resetFeedbackIconProvider() {
  if (!feedbackIconProvider_) {
    return;
  }
  feedbackIconProvider_ = {};
  emit feedbackIconProviderChanged();
  refreshFeedbackIcon();
}

QVariant AdFormItem::value() const {
  QVariant currentValue = readControlValue();
  if (valueNormalizer_) {
    currentValue = valueNormalizer_(currentValue, previousValue_, const_cast<AdFormItem*>(this));
  }
  return currentValue;
}

void AdFormItem::setValue(const QVariant& value) {
  QWidget* widget = controlWidget_.data();
  if (!widget) {
    return;
  }
  const QSignalBlocker blocker(widget);
  writeControlValue(value);
  Q_UNUSED(blocker)
  emitCurrentValueChanged();
}

bool AdFormItem::validate() {
  if (validationTimer_) {
    validationTimer_->stop();
  }
  if (itemHidden_) {
    resetValidation();
    return true;
  }

  const QVariant currentValue = value();
  ValidationResult result;
  result.status = ValidateStatus::Success;

  if (required_ && isEmptyValue(currentValue)) {
    result.status = ValidateStatus::Error;
    const QString labelText = label_.trimmed().isEmpty() ? fieldName_ : label_;
    const QString displayName = labelText.isEmpty() ? tr("This field") : labelText;
    QString message = requiredMessage_;
    if (message.isEmpty() && form_ && !form_->requiredMessageTemplate().isEmpty()) {
      message = formatRequiredMessage(form_->requiredMessageTemplate(), displayName);
    }
    result.errors.append(message.isEmpty() ? tr("%1 is required.").arg(displayName) : message);
  } else {
    if (validator_) {
      mergeValidationResult(result, validator_(currentValue, controlWidget_.data()));
    }
    if (formValidator_) {
      mergeValidationResult(result, formValidator_(currentValue, this));
    }
  }

  result = normalizedValidationResult(std::move(result));

  const bool errorsChanged = errorMessages_ != result.errors;
  const bool warningsChanged = warningMessages_ != result.warnings;
  const bool helpChanged = !result.helpText.isNull() && helpText_ != result.helpText;

  errorMessages_ = result.errors;
  warningMessages_ = result.warnings;
  if (!result.helpText.isNull()) {
    helpText_ = result.helpText;
  }

  if (validateStatus_ != result.status || validateStatusExplicit_) {
    const bool statusChanged = validateStatus_ != result.status;
    validateStatus_ = result.status;
    validateStatusExplicit_ = false;
    if (statusChanged) {
      emit validateStatusChanged(validateStatus_);
    }
  }
  if (errorsChanged) {
    emit errorMessagesChanged(errorMessages_);
  }
  if (warningsChanged) {
    emit warningMessagesChanged(warningMessages_);
  }
  if (helpChanged) {
    emit helpTextChanged(helpText_);
  }

  emit validationStateChanged(validateStatus_, errorMessages_, warningMessages_);
  refresh();
  return validateStatus_ != ValidateStatus::Error;
}

void AdFormItem::scheduleValidation() {
  if (validationTimer_) {
    validationTimer_->stop();
  }

  if (validateDebounceMs_ <= 0) {
    validate();
    return;
  }

  if (!validationTimer_) {
    validationTimer_ = new QTimer(this);
    validationTimer_->setSingleShot(true);
    connect(validationTimer_, &QTimer::timeout, this, [this]() { validate(); });
  }
  validationTimer_->start(validateDebounceMs_);
}

void AdFormItem::resetValidation() {
  if (validationTimer_) {
    validationTimer_->stop();
  }
  bool changed = false;
  if (validateStatus_ != ValidateStatus::None || validateStatusExplicit_) {
    const bool statusChanged = validateStatus_ != ValidateStatus::None;
    validateStatus_ = ValidateStatus::None;
    validateStatusExplicit_ = false;
    if (statusChanged) {
      emit validateStatusChanged(validateStatus_);
    }
    changed = true;
  }
  if (!errorMessages_.isEmpty()) {
    errorMessages_.clear();
    emit errorMessagesChanged(errorMessages_);
    changed = true;
  }
  if (!warningMessages_.isEmpty()) {
    warningMessages_.clear();
    emit warningMessagesChanged(warningMessages_);
    changed = true;
  }
  if (changed) {
    emit validationStateChanged(validateStatus_, errorMessages_, warningMessages_);
  }
  refresh();
}

void AdFormItem::refresh() {
  ensureUi();
  QWidget::setVisible(!itemHidden_);
  rebuildItemLayout();
  refreshLabel();
  refreshMessages();
  refreshFeedbackIcon();
  refreshControlStyle();
  refreshNoStyleDescendantStatus();
  refreshAccessibility();
  updateGeometry();
  update();
  notifyStyledParentOfNoStyleMetaChange();
}

void AdFormItem::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }
  switch (event->type()) {
    case QEvent::EnabledChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::LanguageChange:
      refresh();
      break;
    default:
      break;
  }
}

bool AdFormItem::event(QEvent* event) {
  const bool handled = QWidget::event(event);
  if (event && event->type() == QEvent::ParentChange) {
    refresh();
  }
  return handled;
}

void AdFormItem::attachForm(AdForm* form) {
  if (form_ == form) {
    return;
  }
  form_ = form;
  refresh();
}

void AdFormItem::ensureUi() {
  if (labelWidget_) {
    return;
  }

  labelHost_ = new QWidget(this);
  labelHost_->setObjectName(QStringLiteral("ad-form-item-label-host"));
  labelHost_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

  labelLayout_ = new QHBoxLayout(labelHost_);
  labelLayout_->setContentsMargins(0, 0, 0, 0);
  labelLayout_->setSpacing(0);

  requiredMarkWidget_ = new QLabel(labelHost_);
  requiredMarkWidget_->setObjectName(QStringLiteral("ad-form-item-required-mark"));
  requiredMarkWidget_->setTextFormat(Qt::PlainText);
  requiredMarkWidget_->setTextInteractionFlags(Qt::NoTextInteraction);
  requiredMarkWidget_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  requiredMarkWidget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  requiredMarkWidget_->hide();

  labelWidget_ = new QLabel(labelHost_);
  labelWidget_->setObjectName(QStringLiteral("ad-form-item-label"));
  labelWidget_->setTextFormat(Qt::PlainText);
  labelWidget_->setTextInteractionFlags(Qt::NoTextInteraction);
  labelWidget_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

  optionalWidget_ = new QLabel(labelHost_);
  optionalWidget_->setObjectName(QStringLiteral("ad-form-item-optional"));
  optionalWidget_->setTextFormat(Qt::PlainText);
  optionalWidget_->setTextInteractionFlags(Qt::NoTextInteraction);
  optionalWidget_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  optionalWidget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  optionalWidget_->hide();

  tooltipWidget_ = new QLabel(labelHost_);
  tooltipWidget_->setObjectName(QStringLiteral("ad-form-item-label-tooltip"));
  tooltipWidget_->setAlignment(Qt::AlignCenter);
  tooltipWidget_->setCursor(Qt::WhatsThisCursor);
  tooltipWidget_->setVisible(false);
  tooltipWidget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  colonWidget_ = new QLabel(labelHost_);
  colonWidget_->setObjectName(QStringLiteral("ad-form-item-colon"));
  colonWidget_->setTextFormat(Qt::PlainText);
  colonWidget_->setTextInteractionFlags(Qt::NoTextInteraction);
  colonWidget_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  colonWidget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  colonWidget_->hide();

  labelLayout_->addWidget(requiredMarkWidget_, 0, Qt::AlignVCenter);
  labelLayout_->addWidget(labelWidget_, 0, Qt::AlignVCenter);
  labelLayout_->addWidget(tooltipWidget_, 0, Qt::AlignVCenter);
  labelLayout_->addWidget(optionalWidget_, 0, Qt::AlignVCenter);
  labelLayout_->addWidget(colonWidget_, 0, Qt::AlignVCenter);

  controlHost_ = new QWidget(this);
  controlHost_->setObjectName(QStringLiteral("ad-form-item-control"));
  controlHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  controlLayout_ = new QHBoxLayout(controlHost_);
  controlLayout_->setContentsMargins(0, 0, 0, 0);
  controlLayout_->setSpacing(0);
  controlLayout_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

  feedbackWidget_ = new QLabel(controlHost_);
  feedbackWidget_->setObjectName(QStringLiteral("ad-form-item-feedback-icon"));
  feedbackWidget_->setAlignment(Qt::AlignCenter);
  feedbackWidget_->setVisible(false);
  feedbackWidget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  controlLayout_->addWidget(feedbackWidget_);

  additionalHost_ = new QWidget(this);
  additionalHost_->setObjectName(QStringLiteral("ad-form-item-additional"));
  additionalHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  additionalLayout_ = new QVBoxLayout(additionalHost_);
  additionalLayout_->setContentsMargins(0, 0, 0, 0);
  additionalLayout_->setSpacing(0);

  helpWidget_ = new QLabel(additionalHost_);
  helpWidget_->setObjectName(QStringLiteral("ad-form-item-explain"));
  helpWidget_->setWordWrap(true);
  helpWidget_->setTextFormat(Qt::PlainText);
  helpWidget_->setTextInteractionFlags(Qt::NoTextInteraction);
  helpWidget_->setVisible(false);

  extraWidget_ = new QLabel(additionalHost_);
  extraWidget_->setObjectName(QStringLiteral("ad-form-item-extra"));
  extraWidget_->setWordWrap(true);
  extraWidget_->setTextFormat(Qt::PlainText);
  extraWidget_->setTextInteractionFlags(Qt::NoTextInteraction);
  extraWidget_->setVisible(false);

  additionalLayout_->addWidget(helpWidget_);
  additionalLayout_->addWidget(extraWidget_);

  rebuildItemLayout();
}

void AdFormItem::rebuildItemLayout() {
  ensureUi();
  const AdForm::ControlSize controlSize =
      form_ ? form_->controlSize() : AdForm::ControlSize::Medium;
  const detail::FormVisualStyle style = resolveStyleFor(this, controlSize);
  const bool vertical = effectiveVerticalLayout();
  const bool inlineLayout = effectiveInlineLayout();
  const bool hasLabel = !label_.trimmed().isEmpty();
  const bool hasNullLabelOffset = !hasLabel && label_.isNull() && !vertical && !inlineLayout &&
                                  form_ && form_->labelColumnWidth() > 0;

  if (itemLayoutGrid_) {
    while (QLayoutItem* layoutItem = itemLayoutGrid_->takeAt(0)) {
      delete layoutItem;
    }
    delete itemLayoutGrid_;
    itemLayoutGrid_ = nullptr;
  }

  itemLayoutGrid_ = new QGridLayout(this);
  itemLayoutGrid_->setContentsMargins(
      0, 0, 0,
      noStyle_
          ? 0
          : (inlineLayout ? style.metrics.inlineItemMarginBottom : style.metrics.itemMarginBottom));
  itemLayoutGrid_->setHorizontalSpacing(0);
  itemLayoutGrid_->setVerticalSpacing(0);

  if (noStyle_) {
    itemLayoutGrid_->addWidget(controlHost_, 0, 0);
    labelHost_->hide();
    additionalHost_->hide();
  } else if (hasNullLabelOffset) {
    itemLayoutGrid_->addWidget(controlHost_, 0, 1);
    itemLayoutGrid_->addWidget(additionalHost_, 1, 1);
    itemLayoutGrid_->setColumnMinimumWidth(0, effectiveLabelColumnWidth(form_));
    itemLayoutGrid_->setColumnStretch(0, 0);
    itemLayoutGrid_->setColumnStretch(1, 1);
    labelHost_->hide();
  } else if (!hasLabel) {
    itemLayoutGrid_->addWidget(controlHost_, 0, 0);
    itemLayoutGrid_->addWidget(additionalHost_, 1, 0);
    itemLayoutGrid_->setColumnStretch(0, 1);
    labelHost_->hide();
  } else if (vertical) {
    itemLayoutGrid_->addWidget(labelHost_, 0, 0, Qt::AlignTop);
    itemLayoutGrid_->addWidget(controlHost_, 1, 0);
    itemLayoutGrid_->addWidget(additionalHost_, 2, 0);
    itemLayoutGrid_->setColumnStretch(0, 1);
  } else {
    itemLayoutGrid_->addWidget(labelHost_, 0, 0, Qt::AlignTop);
    itemLayoutGrid_->addWidget(controlHost_, 0, 1);
    itemLayoutGrid_->addWidget(additionalHost_, 1, 1);
    itemLayoutGrid_->setColumnStretch(0, 0);
    itemLayoutGrid_->setColumnStretch(1, 1);
  }

  if (labelHost_) {
    const int labelColumnWidth = effectiveLabelColumnWidth(form_);
    if (!noStyle_ && hasLabel && !vertical && !inlineLayout && labelColumnWidth > 0) {
      labelHost_->setFixedWidth(labelColumnWidth);
    } else {
      labelHost_->setMinimumWidth(0);
      labelHost_->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    const int verticalLabelHeight =
        style.metrics.messageLineHeight + std::max(0, style.metrics.verticalLabelPaddingBottom);
    labelHost_->setMinimumHeight(vertical ? verticalLabelHeight : style.metrics.labelHeight);
  }

  if (labelLayout_) {
    labelLayout_->setContentsMargins(
        0, 0, 0, !noStyle_ && vertical ? std::max(0, style.metrics.verticalLabelPaddingBottom) : 0);
  }

  if (controlHost_) {
    controlHost_->setMinimumHeight(noStyle_ ? 0 : style.metrics.controlMinHeight);
  }
  if (controlLayout_) {
    controlLayout_->setSpacing(style.metrics.feedbackIconGap);
  }
  updateAdditionalSpacing();
}

void AdFormItem::refreshLabel() {
  ensureUi();
  const AdForm::ControlSize controlSize =
      form_ ? form_->controlSize() : AdForm::ControlSize::Medium;
  const detail::FormVisualStyle style = resolveStyleFor(this, controlSize);
  const bool vertical = effectiveVerticalLayout();
  const bool hasLabel = !label_.trimmed().isEmpty();

  labelHost_->setVisible(!noStyle_ && hasLabel);
  if (!hasLabel) {
    requiredMarkWidget_->clear();
    requiredMarkWidget_->hide();
    labelWidget_->clear();
    optionalWidget_->clear();
    optionalWidget_->hide();
    tooltipWidget_->clear();
    tooltipWidget_->hide();
    colonWidget_->clear();
    colonWidget_->hide();
    return;
  }

  QFont requiredMarkFont = style.metrics.labelFont;
  requiredMarkFont.setFamily(QStringLiteral("SimSun"));
  requiredMarkWidget_->setFont(requiredMarkFont);
  labelWidget_->setFont(style.metrics.labelFont);
  optionalWidget_->setFont(style.metrics.labelFont);
  colonWidget_->setFont(style.metrics.labelFont);
  labelWidget_->setWordWrap(vertical || (form_ ? form_->labelWrap() : false));
  const Qt::Alignment horizontal =
      form_ && form_->labelAlign() == AdForm::LabelAlign::Left ? Qt::AlignLeft : Qt::AlignRight;
  const Qt::Alignment labelAlignment =
      (vertical ? Qt::AlignLeft : horizontal) | (vertical ? Qt::AlignTop : Qt::AlignVCenter);
  labelWidget_->setAlignment(labelAlignment);
  labelLayout_->setAlignment(labelAlignment);
  labelLayout_->setSpacing(0);

  const AdForm::RequiredMark requiredMark =
      form_ ? form_->requiredMark() : AdForm::RequiredMark::Visible;

  requiredMarkWidget_->setText(QStringLiteral("*"));
  requiredMarkWidget_->setVisible(!noStyle_ && required_ &&
                                  requiredMark == AdForm::RequiredMark::Visible);
  requiredMarkWidget_->setContentsMargins(0, 0, std::max(0, style.metrics.requiredMarkGap), 0);
  requiredMarkWidget_->setAlignment(vertical ? (Qt::AlignTop | Qt::AlignLeft)
                                             : (Qt::AlignVCenter | Qt::AlignLeft));
  setLabelTextColor(requiredMarkWidget_, style.requiredMarkColor);

  const QString displayLabel =
      form_ && form_->colon() && !vertical ? normalizedLabelForColon(label_) : label_.trimmed();
  labelWidget_->setText(displayLabel);
  setLabelTextColor(labelWidget_, style.labelColor);

  optionalWidget_->setText(tr("(optional)"));
  optionalWidget_->setVisible(!noStyle_ && !required_ &&
                              requiredMark == AdForm::RequiredMark::Optional);
  optionalWidget_->setContentsMargins(std::max(0, style.metrics.optionalMarkGap), 0, 0, 0);
  optionalWidget_->setAlignment(vertical ? (Qt::AlignTop | Qt::AlignLeft)
                                         : (Qt::AlignVCenter | Qt::AlignLeft));
  setLabelTextColor(optionalWidget_, style.optionalColor);

  const bool showColonSlot = !noStyle_ && form_ && !vertical;
  colonWidget_->setText(form_ && form_->colon() ? QStringLiteral(":") : QStringLiteral(" "));
  colonWidget_->setVisible(showColonSlot);
  colonWidget_->setContentsMargins(std::max(0, style.metrics.colonMarginInlineStart), 0,
                                   std::max(0, style.metrics.colonMarginInlineEnd), 0);
  colonWidget_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  setLabelTextColor(colonWidget_, style.labelColor);

  labelWidget_->setToolTip(label_);
  tooltipWidget_->setToolTip(tooltipText_);
  const bool hasTooltip = !tooltipText_.trimmed().isEmpty();
  tooltipWidget_->setVisible(!noStyle_ && hasTooltip);
  if (hasTooltip) {
    adqt::icons::IconRef icon = adqt::icons::antd::outlined::QuestionCircle();
    icon = icon.withColors(icon.colors().withPrimary(style.optionalColor));
    const int iconSize = style.metrics.feedbackIconSize;
    const int tooltipGap = std::max(0, style.metrics.optionalMarkGap);
    tooltipWidget_->setFixedSize(iconSize + tooltipGap, iconSize);
    tooltipWidget_->setContentsMargins(tooltipGap, 0, 0, 0);
    tooltipWidget_->setPixmap(adqt::icons::renderIconPixmap(
        icon, {QSize(iconSize, iconSize), std::max(1.0, devicePixelRatioF())}));
  } else {
    tooltipWidget_->clear();
    tooltipWidget_->setContentsMargins(0, 0, 0, 0);
  }
  if (controlWidget_) {
    labelWidget_->setBuddy(controlWidget_);
  }
}

void AdFormItem::refreshMessages() {
  ensureUi();
  const AdForm::ControlSize controlSize =
      form_ ? form_->controlSize() : AdForm::ControlSize::Medium;
  const detail::FormVisualStyle style = resolveStyleFor(this, controlSize);

  helpWidget_->setFont(style.metrics.messageFont);
  extraWidget_->setFont(style.metrics.messageFont);
  helpWidget_->setMinimumHeight(hasVisibleHelpMessage() ? style.metrics.messageLineHeight : 0);
  extraWidget_->setMinimumHeight(style.metrics.messageMinHeight);

  const QStringList collectedErrors = collectedErrorMessages();
  const QStringList collectedWarnings = collectedWarningMessages();
  QString helpText;
  QColor helpColor = style.messageColor;
  if (hasExplicitHelpText()) {
    helpText = helpText_;
    helpColor = messageColorForStatus(style, effectiveVisualStatus(), false);
  } else if (!collectedErrors.isEmpty()) {
    helpText = collectedErrors.join(QLatin1Char('\n'));
    helpColor = style.errorColor;
  } else if (!collectedWarnings.isEmpty()) {
    helpText = collectedWarnings.join(QLatin1Char('\n'));
    helpColor = style.warningColor;
  }

  helpWidget_->setText(helpText);
  extraWidget_->setText(extraText_);

  QPalette helpPalette = helpWidget_->palette();
  helpPalette.setColor(QPalette::WindowText, helpColor);
  helpWidget_->setPalette(helpPalette);

  QPalette extraPalette = extraWidget_->palette();
  extraPalette.setColor(QPalette::WindowText, style.messageColor);
  extraWidget_->setPalette(extraPalette);

  helpWidget_->setVisible(!noStyle_ && !helpText.isEmpty());
  extraWidget_->setVisible(!noStyle_ && !extraText_.trimmed().isEmpty());
  additionalHost_->setVisible(!noStyle_ && hasVisibleAdditionalText());
  updateAdditionalSpacing();
}

void AdFormItem::updateAdditionalSpacing() {
  if (!itemLayoutGrid_) {
    return;
  }

  const AdForm::ControlSize controlSize =
      form_ ? form_->controlSize() : AdForm::ControlSize::Medium;
  const detail::FormVisualStyle style = resolveStyleFor(this, controlSize);
  const bool inlineLayout = effectiveInlineLayout();
  const bool helpVisible = hasVisibleHelpMessage();
  const int bottomMargin =
      noStyle_ ? 0
               : (inlineLayout ? style.metrics.inlineItemMarginBottom
                               : (helpVisible ? 0 : style.metrics.itemMarginBottom));
  itemLayoutGrid_->setContentsMargins(0, 0, 0, bottomMargin);
  if (additionalHost_) {
    additionalHost_->setMinimumHeight(!noStyle_ && helpVisible ? style.metrics.itemMarginBottom
                                                               : 0);
  }
  updateGeometry();
}

void AdFormItem::refreshFeedbackIcon() {
  ensureUi();
  const AdForm::ControlSize controlSize =
      form_ ? form_->controlSize() : AdForm::ControlSize::Medium;
  const detail::FormVisualStyle style = resolveStyleFor(this, controlSize);
  const ValidateStatus visualStatus = effectiveVisualStatus();
  const bool delegateFeedbackToNoStyleChildren =
      !noStyle_ && hasNoStyleDescendantItems() && !canHostFeedbackIcon(controlWidget_.data());
  const bool visible = effectiveHasFeedback() && visualStatus != ValidateStatus::None &&
                       !delegateFeedbackToNoStyleChildren;
  feedbackWidget_->setFixedSize(style.metrics.feedbackIconSize, style.metrics.feedbackIconSize);
  if (!visible) {
    setFallbackFeedbackSpinnerActive(false);
    clearControlFeedbackIcon();
    feedbackWidget_->setVisible(false);
    feedbackWidget_->clear();
    return;
  }

  adqt::icons::IconRef icon =
      effectiveFeedbackIconProvider()
          ? effectiveFeedbackIconProvider()(visualStatus, errorMessages_, warningMessages_)
          : feedbackIconForStatus(visualStatus);
  if (!adqt::icons::isValid(icon)) {
    setFallbackFeedbackSpinnerActive(false);
    clearControlFeedbackIcon();
    feedbackWidget_->setVisible(false);
    feedbackWidget_->clear();
    return;
  }
  if (!icon.colors().primarySlot()) {
    icon = icon.withColors(icon.colors().withPrimary(feedbackColorForStatus(style, visualStatus)));
  }

  if (writeIconRefProperty(controlWidget_.data(), "feedbackIconRef", icon)) {
    setFallbackFeedbackSpinnerActive(false);
    controlFeedbackIconPropertyApplied_ = true;
    feedbackWidget_->setVisible(false);
    feedbackWidget_->clear();
    return;
  }

  if (auto* inputNumber = qobject_cast<AdInputNumber*>(controlWidget_.data())) {
    setFallbackFeedbackSpinnerActive(false);
    if (!inputNumberFeedbackIconApplied_) {
      inputNumberSuffixIconBeforeFeedback_ = inputNumber->suffixIconRef();
      inputNumberSuffixTextBeforeFeedback_ = inputNumber->suffixText();
      inputNumberFeedbackIconApplied_ = true;
    }
    inputNumber->setSuffixText(QString());
    inputNumber->setSuffixIconRef(icon);
    feedbackWidget_->setVisible(false);
    feedbackWidget_->clear();
    return;
  }

  clearControlFeedbackIcon();
  setFallbackFeedbackSpinnerActive(isLoadingIcon(icon));
  feedbackWidget_->setVisible(true);
  feedbackWidget_->setPixmap(renderFeedbackIconPixmap(
      icon, style.metrics.feedbackIconSize, std::max(1.0, devicePixelRatioF()),
      isLoadingIcon(icon) ? sharedSpinnerAngle() : 0));
}

void AdFormItem::clearControlFeedbackIcon() {
  if (inputNumberFeedbackIconApplied_) {
    if (auto* inputNumber = qobject_cast<AdInputNumber*>(controlWidget_.data())) {
      inputNumber->setSuffixText(inputNumberSuffixTextBeforeFeedback_);
      inputNumber->setSuffixIconRef(inputNumberSuffixIconBeforeFeedback_);
    }
    inputNumberSuffixTextBeforeFeedback_.clear();
    inputNumberSuffixIconBeforeFeedback_ = adqt::icons::IconRef();
    inputNumberFeedbackIconApplied_ = false;
  }

  if (controlFeedbackIconPropertyApplied_) {
    writeIconRefProperty(controlWidget_.data(), "feedbackIconRef", adqt::icons::IconRef());
    controlFeedbackIconPropertyApplied_ = false;
  }
}

void AdFormItem::setFallbackFeedbackSpinnerActive(bool active) {
  active = active && isVisible();
  if (active && !fallbackFeedbackSpinnerSubscribed_) {
    detail::setFrameSubscription(this, QString::fromLatin1(kFallbackFeedbackSpinnerFrameKey), true,
                                 [this](qint64, qint64) {
                                   if (fallbackFeedbackSpinnerSubscribed_) {
                                     refreshFeedbackIcon();
                                   }
                                 });
    fallbackFeedbackSpinnerSubscribed_ = true;
  } else if (!active && fallbackFeedbackSpinnerSubscribed_) {
    detail::clearFrameSubscription(this, QString::fromLatin1(kFallbackFeedbackSpinnerFrameKey));
    fallbackFeedbackSpinnerSubscribed_ = false;
  }
}

void AdFormItem::refreshNoStyleDescendantStatus() {
  const QList<AdFormItem*> descendants =
      findChildren<AdFormItem*>(QString(), Qt::FindChildrenRecursively);
  for (AdFormItem* item : descendants) {
    if (!item || item == this || !item->noStyle() || item->parentFormItem() != this) {
      continue;
    }
    item->refreshFeedbackIcon();
    item->refreshControlStyle();
    item->refreshNoStyleDescendantStatus();
  }
}

void AdFormItem::notifyStyledParentOfNoStyleMetaChange() {
  if (!noStyle_) {
    return;
  }
  AdFormItem* parentItem = nearestStyledParentFormItem();
  if (!parentItem) {
    return;
  }
  parentItem->refreshMessages();
  parentItem->refreshFeedbackIcon();
  parentItem->refreshControlStyle();
  parentItem->updateGeometry();
  parentItem->update();
}

void AdFormItem::refreshControlStyle() {
  QWidget* widget = controlWidget_.data();
  if (!widget) {
    return;
  }

  if (form_) {
    writeEnumProperty(widget, "controlSize", controlSizePropertyValue(form_->controlSize()));
    writeEnumProperty(widget, "variant", variantPropertyValue(form_->variant()));
    if (form_->disabled()) {
      if (!formDisabledApplied_) {
        controlEnabledBeforeFormDisable_ = widget->isEnabled();
        formDisabledApplied_ = true;
      }
      widget->setEnabled(false);
    } else if (formDisabledApplied_) {
      widget->setEnabled(controlEnabledBeforeFormDisable_);
      formDisabledApplied_ = false;
    }
  }

  writeEnumProperty(widget, "status", statusPropertyValue(effectiveVisualStatus()));
}

void AdFormItem::refreshAccessibility() {
  QWidget* widget = controlWidget_.data();
  if (!widget) {
    return;
  }

  const QString accessibleName = label_.trimmed().isEmpty() ? fieldName_ : label_;
  if (!accessibleName.isEmpty() && widget->accessibleName().isEmpty()) {
    widget->setAccessibleName(accessibleName);
    notifyAccessibleNameChanged(widget);
  }

  QStringList descriptionParts;
  if (required_) {
    descriptionParts.append(tr("Required"));
  }
  if (hasExplicitHelpText()) {
    descriptionParts.append(helpText_);
  }
  descriptionParts.append(errorMessages_);
  descriptionParts.append(warningMessages_);
  if (!extraText_.trimmed().isEmpty()) {
    descriptionParts.append(extraText_);
  }
  const QString description = descriptionParts.join(QLatin1Char(' ')).trimmed();
  if (!description.isEmpty()) {
    widget->setAccessibleDescription(description);
  }
}

void AdFormItem::bindControlValueSignals(QWidget* widget) {
  clearControlValueSignals();
  if (!widget) {
    return;
  }
  controlConnections_ =
      detail::connectWidgetValueSignals(widget, this, [this]() { emitCurrentValueChanged(); });
}

void AdFormItem::clearControlValueSignals() {
  for (const QMetaObject::Connection& connection : std::as_const(controlConnections_)) {
    QObject::disconnect(connection);
  }
  controlConnections_.clear();
}

QVariant AdFormItem::readControlValue() const {
  QWidget* widget = controlWidget_.data();
  if (!widget) {
    return {};
  }
  if (valueReader_) {
    return valueReader_(widget);
  }
  return detail::readWidgetValue(widget, valuePropertyName_);
}

void AdFormItem::writeControlValue(const QVariant& value) {
  QWidget* widget = controlWidget_.data();
  if (!widget) {
    return;
  }
  if (valueWriter_) {
    valueWriter_(widget, value);
    return;
  }
  detail::writeWidgetValue(widget, value, valuePropertyName_);
}

void AdFormItem::emitCurrentValueChanged() {
  const QVariant nextValue = value();
  if (valueNormalizer_ && nextValue != readControlValue()) {
    const QSignalBlocker blocker(controlWidget_.data());
    writeControlValue(nextValue);
    Q_UNUSED(blocker)
  }
  setMetaState(true, true);
  previousValue_ = nextValue;
  emit valueChanged(fieldName_, nextValue);
  if (validateOnChange_) {
    scheduleValidation();
  }
}

void AdFormItem::applyValidateStatus(ValidateStatus value, bool explicitStatus) {
  const bool valueChanged = validateStatus_ != value;
  const bool explicitChanged = validateStatusExplicit_ != explicitStatus;
  if (!valueChanged && !explicitChanged) {
    return;
  }
  validateStatus_ = value;
  validateStatusExplicit_ = explicitStatus;
  if (valueChanged) {
    emit validateStatusChanged(validateStatus_);
  }
  emit validationStateChanged(validateStatus_, errorMessages_, warningMessages_);
  refresh();
}

bool AdFormItem::hasDependencyOn(const QStringList& sourceNamePath) const {
  if (sourceNamePath.isEmpty()) {
    return false;
  }
  return std::any_of(
      dependencies_.cbegin(), dependencies_.cend(), [&sourceNamePath](const QString& dependency) {
        return detail::sameFieldPath(detail::parseFieldPath(dependency), sourceNamePath);
      });
}

void AdFormItem::setMetaState(bool touched, bool dirty) {
  if (touched_ != touched) {
    touched_ = touched;
    emit touchedChanged(touched_);
  }
  if (dirty_ != dirty) {
    dirty_ = dirty;
    emit dirtyChanged(dirty_);
  }
}

bool AdFormItem::hasExplicitHelpText() const { return !helpText_.isNull(); }

bool AdFormItem::hasVisibleHelpMessage() const {
  if (noStyle_) {
    return false;
  }
  if (hasExplicitHelpText()) {
    return true;
  }
  return !collectedErrorMessages().isEmpty() || !collectedWarningMessages().isEmpty();
}

bool AdFormItem::hasVisibleAdditionalText() const {
  if (noStyle_) {
    return false;
  }
  return hasExplicitHelpText() || !collectedErrorMessages().isEmpty() ||
         !collectedWarningMessages().isEmpty() || !extraText_.trimmed().isEmpty();
}

AdFormItem* AdFormItem::parentFormItem() const {
  for (QWidget* current = parentWidget(); current; current = current->parentWidget()) {
    if (auto* item = qobject_cast<AdFormItem*>(current)) {
      return item == this ? nullptr : item;
    }
  }
  return nullptr;
}

AdFormItem* AdFormItem::nearestStyledParentFormItem() const {
  for (AdFormItem* item = parentFormItem(); item; item = item->parentFormItem()) {
    if (!item->noStyle()) {
      return item;
    }
  }
  return nullptr;
}

bool AdFormItem::hasNoStyleDescendantItems() const {
  const QList<AdFormItem*> descendants =
      findChildren<AdFormItem*>(QString(), Qt::FindChildrenRecursively);
  return std::any_of(descendants.cbegin(), descendants.cend(), [this](const AdFormItem* item) {
    return item && item != this && item->noStyle() && item->nearestStyledParentFormItem() == this;
  });
}

QStringList AdFormItem::collectedErrorMessages() const {
  QStringList messages = errorMessages_;
  if (noStyle_) {
    return messages;
  }

  const QList<AdFormItem*> descendants =
      findChildren<AdFormItem*>(QString(), Qt::FindChildrenRecursively);
  for (const AdFormItem* item : descendants) {
    if (!item || item == this || !item->noStyle() || item->nearestStyledParentFormItem() != this) {
      continue;
    }
    messages.append(item->errorMessages_);
  }
  return messages;
}

QStringList AdFormItem::collectedWarningMessages() const {
  QStringList messages = warningMessages_;
  if (noStyle_) {
    return messages;
  }

  const QList<AdFormItem*> descendants =
      findChildren<AdFormItem*>(QString(), Qt::FindChildrenRecursively);
  for (const AdFormItem* item : descendants) {
    if (!item || item == this || !item->noStyle() || item->nearestStyledParentFormItem() != this) {
      continue;
    }
    messages.append(item->warningMessages_);
  }
  return messages;
}

AdFormItem::ValidateStatus AdFormItem::ownMessageStatus() const {
  if (!errorMessages_.isEmpty()) {
    return ValidateStatus::Error;
  }
  if (!warningMessages_.isEmpty()) {
    return ValidateStatus::Warning;
  }
  return ValidateStatus::None;
}

AdFormItem::ValidateStatus AdFormItem::effectiveVisualStatus() const {
  if (validateStatusExplicit_ || validateStatus_ != ValidateStatus::None) {
    return validateStatus_;
  }
  const ValidateStatus ownStatus = ownMessageStatus();
  if (ownStatus != ValidateStatus::None) {
    return ownStatus;
  }
  if (noStyle_) {
    if (AdFormItem* parentItem = parentFormItem()) {
      return parentItem->effectiveVisualStatus();
    }
  }
  return ValidateStatus::None;
}

bool AdFormItem::effectiveHasFeedback() const {
  if (noStyle_ && !hasFeedbackExplicit_) {
    if (AdFormItem* parentItem = parentFormItem()) {
      return parentItem->effectiveHasFeedback();
    }
  }
  return hasFeedback_;
}

AdFormItem::FeedbackIconProvider AdFormItem::effectiveFeedbackIconProvider() const {
  if (feedbackIconProvider_) {
    return feedbackIconProvider_;
  }
  if (noStyle_ && !hasFeedbackExplicit_) {
    if (AdFormItem* parentItem = parentFormItem()) {
      return parentItem->effectiveFeedbackIconProvider();
    }
  }
  return {};
}

AdForm::FormLayout AdFormItem::effectiveFormLayout() const {
  if (itemLayout_ == ItemLayout::Horizontal) {
    return AdForm::FormLayout::Horizontal;
  }
  if (itemLayout_ == ItemLayout::Vertical) {
    return AdForm::FormLayout::Vertical;
  }
  return form_ ? form_->formLayout() : AdForm::FormLayout::Horizontal;
}

bool AdFormItem::effectiveVerticalLayout() const {
  return effectiveFormLayout() == AdForm::FormLayout::Vertical;
}

bool AdFormItem::effectiveInlineLayout() const {
  return effectiveFormLayout() == AdForm::FormLayout::Inline;
}

bool AdFormItem::effectiveDisabled() const { return form_ && form_->disabled(); }

AdFormList::AdFormList(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("ad-form-list"));
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  addButtonText_ = tr("Add row");
  ensureUi();
}

AdFormList::~AdFormList() {
  for (Row& row : rows_) {
    clearRowSignals(row);
  }
}

int AdFormList::count() const { return static_cast<int>(rows_.size()); }

int AdFormList::minRows() const { return minRows_; }

void AdFormList::setMinRows(int value) {
  value = std::max(0, value);
  if (minRows_ == value) {
    return;
  }
  minRows_ = value;
  if (maxRows_ >= 0 && maxRows_ < minRows_) {
    maxRows_ = minRows_;
    emit maxRowsChanged(maxRows_);
  }
  while (rows_.size() < minRows_) {
    addRow();
  }
  updateControls();
  emit minRowsChanged(minRows_);
}

int AdFormList::maxRows() const { return maxRows_; }

void AdFormList::setMaxRows(int value) {
  value = value < 0 ? -1 : std::max(value, minRows_);
  if (maxRows_ == value) {
    return;
  }
  maxRows_ = value;
  while (maxRows_ >= 0 && rows_.size() > maxRows_) {
    removeRow(static_cast<int>(rows_.size()) - 1);
  }
  updateControls();
  emit maxRowsChanged(maxRows_);
}

QString AdFormList::addButtonText() const { return addButtonText_; }

void AdFormList::setAddButtonText(const QString& value) {
  if (addButtonText_ == value) {
    return;
  }
  addButtonText_ = value;
  if (addButton_) {
    addButton_->setText(addButtonText_);
  }
  emit addButtonTextChanged(addButtonText_);
}

AdFormList::RowFactory AdFormList::rowFactory() const { return rowFactory_; }

void AdFormList::setRowFactory(RowFactory factory) { rowFactory_ = std::move(factory); }

QVariantList AdFormList::values() const {
  QVariantList result;
  result.reserve(rows_.size());
  for (const Row& row : rows_) {
    result.append(detail::readWidgetValue(row.editor.data()));
  }
  return result;
}

void AdFormList::setValues(const QVariantList& values) {
  ensureUi();
  suppressValueSignal_ = true;

  int targetCount = static_cast<int>(values.size());
  targetCount = std::max(targetCount, minRows_);
  if (maxRows_ >= 0) {
    targetCount = std::min(targetCount, maxRows_);
  }

  while (rows_.size() > targetCount) {
    removeRow(static_cast<int>(rows_.size()) - 1);
  }
  while (rows_.size() < targetCount) {
    addRow();
  }

  for (int i = 0; i < rows_.size(); ++i) {
    const QVariant value = i < values.size() ? values.at(i) : QVariant();
    if (QWidget* editor = rows_.at(i).editor.data()) {
      const QSignalBlocker blocker(editor);
      detail::writeWidgetValue(editor, value);
      Q_UNUSED(blocker)
    }
  }

  suppressValueSignal_ = false;
  emitValuesChanged();
}

QWidget* AdFormList::rowWidget(int index) const {
  if (index < 0 || index >= rows_.size()) {
    return nullptr;
  }
  return rows_.at(index).editor.data();
}

QVector<QWidget*> AdFormList::rowWidgets() const {
  QVector<QWidget*> result;
  result.reserve(rows_.size());
  for (const Row& row : rows_) {
    if (row.editor) {
      result.append(row.editor.data());
    }
  }
  return result;
}

void AdFormList::addRow(const QVariant& value, int index) {
  ensureUi();
  if (maxRows_ >= 0 && rows_.size() >= maxRows_) {
    return;
  }

  const int rowCount = static_cast<int>(rows_.size());
  index = index < 0 ? rowCount : std::clamp(index, 0, rowCount);

  Row row;
  row.host = new QWidget(this);
  row.host->setObjectName(QStringLiteral("ad-form-list-row"));
  row.host->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

  auto* rowLayout = new QHBoxLayout(row.host);
  rowLayout->setContentsMargins(0, 0, 0, 0);
  rowLayout->setSpacing(8);

  row.editor = createRowEditor(index, row.host);
  if (row.editor && row.editor->parentWidget() != row.host) {
    row.editor->setParent(row.host);
  }
  if (row.editor) {
    row.editor->setSizePolicy(QSizePolicy::Expanding, row.editor->sizePolicy().verticalPolicy());
    rowLayout->addWidget(row.editor, 1);
  }

  row.removeButton = new QToolButton(row.host);
  row.removeButton->setObjectName(QStringLiteral("ad-form-list-remove"));
  row.removeButton->setText(tr("Remove"));
  row.removeButton->setToolTip(tr("Remove row"));
  row.removeButton->setAutoRaise(true);
  row.removeButton->setCursor(Qt::PointingHandCursor);
  rowLayout->addWidget(row.removeButton, 0, Qt::AlignTop);

  connect(row.removeButton, &QToolButton::clicked, this,
          [this, host = row.host.data()]() { removeRow(indexOfHost(host)); });

  rowsLayout_->insertWidget(index, row.host);
  rows_.insert(index, row);
  if (row.editor) {
    detail::writeWidgetValue(row.editor, value);
  }
  bindRowSignals(rows_[index]);
  rebuildRowIndexes();
  updateControls();
  emit countChanged(static_cast<int>(rows_.size()));
  emit rowAdded(index);
  emitValuesChanged();
}

void AdFormList::removeRow(int index) {
  ensureUi();
  if (index < 0 || index >= rows_.size() || rows_.size() <= minRows_) {
    return;
  }

  Row row = rows_.takeAt(index);
  clearRowSignals(row);
  if (row.host) {
    rowsLayout_->removeWidget(row.host);
    delete row.host.data();
  }
  rebuildRowIndexes();
  updateControls();
  emit countChanged(static_cast<int>(rows_.size()));
  emit rowRemoved(index);
  emitValuesChanged();
}

void AdFormList::moveRow(int from, int to) {
  ensureUi();
  if (from < 0 || from >= rows_.size() || to < 0 || to >= rows_.size() || from == to) {
    return;
  }

  QWidget* host = rows_.at(from).host.data();
  if (host) {
    rowsLayout_->removeWidget(host);
  }
  rows_.move(from, to);
  if (host) {
    rowsLayout_->insertWidget(to, host);
  }
  rebuildRowIndexes();
  updateControls();
  emit rowMoved(from, to);
  emitValuesChanged();
}

void AdFormList::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event && event->type() == QEvent::LanguageChange) {
    if (addButtonText_.isEmpty()) {
      setAddButtonText(tr("Add row"));
    }
    for (Row& row : rows_) {
      if (row.removeButton) {
        row.removeButton->setText(tr("Remove"));
        row.removeButton->setToolTip(tr("Remove row"));
      }
    }
  }
}

void AdFormList::ensureUi() {
  if (rootLayout_) {
    return;
  }

  rootLayout_ = new QVBoxLayout(this);
  rootLayout_->setContentsMargins(0, 0, 0, 0);
  rootLayout_->setSpacing(8);

  rowsLayout_ = new QVBoxLayout();
  rowsLayout_->setContentsMargins(0, 0, 0, 0);
  rowsLayout_->setSpacing(8);
  rootLayout_->addLayout(rowsLayout_);

  auto* addButton = new AdButton(addButtonText_, this);
  addButton->setButtonStyle(AdButton::ButtonStyle::Dashed);
  addButton->setSizeClass(AdButton::SizeClass::Small);
  addButton_ = addButton;
  connect(addButton_, &QPushButton::clicked, this, [this]() { addRow(); });
  rootLayout_->addWidget(addButton_, 0, Qt::AlignLeft);
  updateControls();
}

QWidget* AdFormList::createRowEditor(int index, QWidget* parent) {
  if (rowFactory_) {
    if (QWidget* widget = rowFactory_(index, parent)) {
      return widget;
    }
  }

  auto* editor = new AdLineEdit(parent);
  editor->setPlaceholderText(tr("Value"));
  editor->setMinimumWidth(220);
  return editor;
}

void AdFormList::bindRowSignals(Row& row) {
  clearRowSignals(row);
  row.connections =
      detail::connectWidgetValueSignals(row.editor.data(), this, [this]() { emitValuesChanged(); });
}

void AdFormList::clearRowSignals(Row& row) {
  for (const QMetaObject::Connection& connection : std::as_const(row.connections)) {
    QObject::disconnect(connection);
  }
  row.connections.clear();
}

int AdFormList::indexOfHost(QWidget* host) const {
  if (!host) {
    return -1;
  }
  for (int i = 0; i < rows_.size(); ++i) {
    if (rows_.at(i).host == host) {
      return i;
    }
  }
  return -1;
}

void AdFormList::rebuildRowIndexes() {
  for (int i = 0; i < rows_.size(); ++i) {
    Row& row = rows_[i];
    if (row.host) {
      row.host->setProperty("formListIndex", i);
    }
    if (row.editor) {
      row.editor->setProperty("formListIndex", i);
    }
    if (row.removeButton) {
      row.removeButton->setProperty("formListIndex", i);
      row.removeButton->setEnabled(rows_.size() > minRows_);
      row.removeButton->setAccessibleName(tr("Remove row %1").arg(i + 1));
    }
  }
}

void AdFormList::updateControls() {
  if (addButton_) {
    const bool canAdd = maxRows_ < 0 || rows_.size() < maxRows_;
    addButton_->setEnabled(canAdd);
    addButton_->setVisible(canAdd);
    addButton_->setText(addButtonText_);
  }
  for (Row& row : rows_) {
    if (row.removeButton) {
      row.removeButton->setEnabled(rows_.size() > minRows_);
    }
  }
}

void AdFormList::emitValuesChanged() {
  if (suppressValueSignal_) {
    return;
  }
  emit valuesChanged(values());
}

}  // namespace adqt::widgets
