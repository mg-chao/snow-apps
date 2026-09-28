#include "select_option_utils.h"

#include <QAbstractItemModel>
#include <QDataStream>
#include <QFont>
#include <QIODevice>
#include <QMap>

namespace adqt::widgets::detail {

QString syntheticRoleFieldName(int role) { return QStringLiteral("__role_%1").arg(role); }

QVariant normalizeSelectValue(const QVariant& value) {
  if (!value.isValid() || value.isNull()) {
    return QVariant();
  }

  if (value.typeId() == QMetaType::QString) {
    const QString text = value.toString().trimmed();
    // Equal empty strings must share a key: QDataStream distinguishes null from empty.
    return text.isEmpty() ? QStringLiteral("") : text;
  }

  return value;
}

QString selectValueKey(const QVariant& value) {
  const QVariant normalized = normalizeSelectValue(value);
  if (!normalized.isValid() || normalized.isNull()) {
    return QString();
  }

  QByteArray payload;
  QDataStream stream(&payload, QIODevice::WriteOnly);
  stream << normalized;
  return QStringLiteral("%1:%2")
      .arg(normalized.typeId())
      .arg(QString::fromLatin1(payload.toBase64()));
}

AdSelectTypes::Option materializeSelectOption(const QModelIndex& index,
                                              const SelectRoleConfig& roles) {
  AdSelectTypes::Option option;
  if (!index.isValid() || !index.model()) {
    return option;
  }

  QAbstractItemModel* model = const_cast<QAbstractItemModel*>(index.model());
  const QVariant rawValue = model->data(index, roles.valueRole);
  const QString labelFromRole = model->data(index, roles.labelRole).toString().trimmed();
  const QString displayText = model->data(index, Qt::DisplayRole).toString().trimmed();

  option.value = normalizeSelectValue(rawValue);
  option.label = !labelFromRole.isEmpty() ? labelFromRole : displayText;
  if ((!option.value.isValid() || option.value.isNull()) && !option.label.isEmpty()) {
    option.value = option.label;
  }
  if (option.label.isEmpty()) {
    option.label = option.value.toString().trimmed();
  }

  const Qt::ItemFlags flags = model->flags(index);
  option.disabled = !(flags & Qt::ItemIsEnabled) || !(flags & Qt::ItemIsSelectable);
  option.group = model->data(index, roles.groupRole).toString().trimmed();

  const QVariant metadata = model->data(index, AdSelectTypes::DefaultMetadataRole);
  if (metadata.isValid()) {
    option.metadata = metadata.toMap();
  }

  const QString tagText = model->data(index, roles.tagTextRole).toString().trimmed();
  if (!tagText.isEmpty()) {
    option.metadata.insert(QString::fromLatin1(kSelectTagTextMetadataKey), tagText);
  }

  const QString selectedText = model->data(index, roles.selectedTextRole).toString().trimmed();
  if (!selectedText.isEmpty()) {
    option.metadata.insert(QString::fromLatin1(kSelectSelectedTextMetadataKey), selectedText);
  }

  const QVariant font = model->data(index, Qt::FontRole);
  if (font.canConvert<QFont>()) {
    option.metadata.insert(QString::fromLatin1(kSelectFontMetadataKey), font);
  }

  const QMap<int, QVariant> roleDataMap = model->itemData(index);
  for (auto it = roleDataMap.cbegin(); it != roleDataMap.cend(); ++it) {
    const int role = it.key();
    if (role < Qt::UserRole + 1) {
      continue;
    }
    if (!it.value().isValid() || it.value().isNull()) {
      continue;
    }
    option.metadata.insert(syntheticRoleFieldName(role), it.value());
  }

  return option;
}

QString optionTagTextFromMetadata(const AdSelectTypes::Option& option) {
  return option.metadata.value(QString::fromLatin1(kSelectTagTextMetadataKey)).toString().trimmed();
}

QString optionSelectedTextFromMetadata(const AdSelectTypes::Option& option) {
  return option.metadata.value(QString::fromLatin1(kSelectSelectedTextMetadataKey))
      .toString()
      .trimmed();
}

}  // namespace adqt::widgets::detail
