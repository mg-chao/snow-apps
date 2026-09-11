#pragma once

#include <QColor>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

#include <functional>
#include <optional>

namespace adqt::widgets {

class AdSelectTypes {
  Q_GADGET

 public:
  enum ItemDataRole {
    DefaultValueRole = Qt::UserRole + 1,
    DefaultLabelRole,
    DefaultTagTextRole,
    DefaultSelectedTextRole,
    DefaultGroupRole,
    DefaultMetadataRole,
  };
  Q_ENUM(ItemDataRole)

  struct Option {
    QVariant value;
    QString label;
    bool disabled = false;
    QString group{};
    QVariantMap metadata{};
  };

  using Item = Option;
  using SelectionValue = QVariant;

  struct SelectionItem {
    QVariant value;
    QString label;
  };

  struct RoleConfig {
    int valueRole = DefaultValueRole;
    int labelRole = DefaultLabelRole;
    int tagTextRole = DefaultTagTextRole;
    int selectedTextRole = DefaultSelectedTextRole;
    int groupRole = DefaultGroupRole;
    QList<int> searchRoles = {DefaultLabelRole, DefaultValueRole};
  };

  struct MetricTokens {
    std::optional<int> controlHeight;
    std::optional<int> borderRadius;
    std::optional<int> borderWidth;
    std::optional<int> horizontalPadding;
    std::optional<int> popupMaxHeight;
    std::optional<int> optionHeight;
    std::optional<int> tagHeight;
    std::optional<int> iconSize;
    std::optional<int> selectorFontSize;
    std::optional<int> optionFontSize;
  };

  struct ColorTokens {
    std::optional<QColor> selectorBackground;
    std::optional<QColor> selectorBorder;
    std::optional<QColor> selectorHoverBorder;
    std::optional<QColor> selectorActiveBorder;
    std::optional<QColor> selectorText;
    std::optional<QColor> placeholderText;
    std::optional<QColor> popupBackground;
    std::optional<QColor> popupBorder;
    std::optional<QColor> optionText;
    std::optional<QColor> optionHoverBackground;
    std::optional<QColor> optionSelectedBackground;
    std::optional<QColor> optionSelectedText;
    std::optional<QColor> tagBackground;
    std::optional<QColor> tagText;
    std::optional<QColor> clear;
    std::optional<QColor> prefix;
    std::optional<QColor> suffix;
  };

  struct ComponentTokens {
    MetricTokens metrics;
    ColorTokens colors;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle selector;
    SemanticSlotStyle placeholder;
    SemanticSlotStyle tag;
    SemanticSlotStyle popup;
    SemanticSlotStyle option;
    SemanticSlotStyle optionHover;
    SemanticSlotStyle optionSelected;
    SemanticSlotStyle prefix;
    SemanticSlotStyle suffix;
  };

  using FilterPredicate = std::function<bool(const QString& searchText, const Option& option)>;
  using SortComparator = std::function<bool(const Option& lhs, const Option& rhs)>;
  using OptionTextFormatter = std::function<QString(const Option&)>;
  using TagTextFormatter = std::function<QString(const Option&)>;
  using LabelFormatter = std::function<QString(const Option&)>;
  using PopupExtraContentFactory = std::function<QWidget*(QWidget* parent)>;
};

namespace select {
Q_NAMESPACE

enum class Mode {
  Single,
  Multiple,
  Tags,
};
Q_ENUM_NS(Mode)

enum class ControlSize {
  Large,
  Middle,
  Small,
};
Q_ENUM_NS(ControlSize)

enum class Variant {
  Outlined,
  Filled,
  Borderless,
  Underlined,
};
Q_ENUM_NS(Variant)

enum class Status {
  None,
  Error,
  Warning,
};
Q_ENUM_NS(Status)

enum class Placement {
  BottomLeft,
  BottomRight,
  TopLeft,
  TopRight,
  BottomCenter,
  TopCenter,
};
Q_ENUM_NS(Placement)

enum class SearchPolicy {
  LocalFilter,
  External,
};
Q_ENUM_NS(SearchPolicy)

enum class PopupWidthMode {
  MatchControlWidth,
  ContentWidth,
  FixedWidth,
};
Q_ENUM_NS(PopupWidthMode)

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
using FilterPredicate = AdSelectTypes::FilterPredicate;
using SortComparator = AdSelectTypes::SortComparator;
using OptionTextFormatter = AdSelectTypes::OptionTextFormatter;
using TagTextFormatter = AdSelectTypes::TagTextFormatter;
using LabelFormatter = AdSelectTypes::LabelFormatter;
using PopupExtraContentFactory = AdSelectTypes::PopupExtraContentFactory;

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

}  // namespace select

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdSelectTypes::SelectionItem)
Q_DECLARE_METATYPE(QVector<adqt::widgets::AdSelectTypes::SelectionItem>)
