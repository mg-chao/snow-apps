#include "abstract_select_widget.h"

#include "select.h"

#include <QAbstractItemDelegate>
#include <QAbstractItemModel>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>

#include <utility>

namespace adqt::widgets {

namespace {

AdSelect* embeddedSelect(const AdAbstractSelectWidget* widget) {
  return widget ? widget->findChild<AdSelect*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

class AdAbstractSelectWidgetAccessible final : public QAccessibleWidget {
 public:
  explicit AdAbstractSelectWidgetAccessible(AdAbstractSelectWidget* widget)
      : QAccessibleWidget(widget) {}

  QString text(QAccessible::Text t) const override {
    const auto* widget = qobject_cast<AdAbstractSelectWidget*>(object());
    if (!widget) {
      return QAccessibleWidget::text(t);
    }

    if (t == QAccessible::Name) {
      const QString explicitName = widget->accessibleName().trimmed();
      if (!explicitName.isEmpty()) {
        return explicitName;
      }
    } else if (t == QAccessible::Description) {
      const QString explicitDescription = widget->accessibleDescription().trimmed();
      if (!explicitDescription.isEmpty()) {
        return explicitDescription;
      }
    }

    if (QAccessibleInterface* iface =
            QAccessible::queryAccessibleInterface(embeddedSelect(widget))) {
      return iface->text(t);
    }
    return QAccessibleWidget::text(t);
  }

  QAccessible::Role role() const override {
    const auto* widget = qobject_cast<AdAbstractSelectWidget*>(object());
    if (QAccessibleInterface* iface =
            widget ? QAccessible::queryAccessibleInterface(embeddedSelect(widget)) : nullptr) {
      return iface->role();
    }
    return QAccessibleWidget::role();
  }

  QAccessible::State state() const override {
    const auto* widget = qobject_cast<AdAbstractSelectWidget*>(object());
    if (QAccessibleInterface* iface =
            widget ? QAccessible::queryAccessibleInterface(embeddedSelect(widget)) : nullptr) {
      return iface->state();
    }
    return QAccessibleWidget::state();
  }
};

QAccessibleInterface* abstractSelectWidgetAccessibleFactory(const QString& className,
                                                            QObject* object) {
  Q_UNUSED(className)
  if (auto* widget = qobject_cast<AdAbstractSelectWidget*>(object)) {
    return new AdAbstractSelectWidgetAccessible(widget);
  }
  return nullptr;
}

void ensureAbstractSelectWidgetAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(abstractSelectWidgetAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

AdSelect::Mode toInternalMode(AdAbstractSelectWidget::Mode mode) {
  switch (mode) {
    case AdAbstractSelectWidget::Mode::Single:
      return AdSelect::Mode::Single;
    case AdAbstractSelectWidget::Mode::Multiple:
      return AdSelect::Mode::Multiple;
    case AdAbstractSelectWidget::Mode::Tags:
      return AdSelect::Mode::Tags;
  }
  return AdSelect::Mode::Single;
}

AdAbstractSelectWidget::ControlSize fromInternalControlSize(AdSelect::ControlSize value) {
  switch (value) {
    case AdSelect::ControlSize::Large:
      return AdAbstractSelectWidget::ControlSize::Large;
    case AdSelect::ControlSize::Middle:
      return AdAbstractSelectWidget::ControlSize::Middle;
    case AdSelect::ControlSize::Small:
      return AdAbstractSelectWidget::ControlSize::Small;
  }
  return AdAbstractSelectWidget::ControlSize::Middle;
}

AdSelect::ControlSize toInternalControlSize(AdAbstractSelectWidget::ControlSize value) {
  switch (value) {
    case AdAbstractSelectWidget::ControlSize::Large:
      return AdSelect::ControlSize::Large;
    case AdAbstractSelectWidget::ControlSize::Middle:
      return AdSelect::ControlSize::Middle;
    case AdAbstractSelectWidget::ControlSize::Small:
      return AdSelect::ControlSize::Small;
  }
  return AdSelect::ControlSize::Middle;
}

AdAbstractSelectWidget::Variant fromInternalVariant(AdSelect::Variant value) {
  switch (value) {
    case AdSelect::Variant::Outlined:
      return AdAbstractSelectWidget::Variant::Outlined;
    case AdSelect::Variant::Filled:
      return AdAbstractSelectWidget::Variant::Filled;
    case AdSelect::Variant::Borderless:
      return AdAbstractSelectWidget::Variant::Borderless;
    case AdSelect::Variant::Underlined:
      return AdAbstractSelectWidget::Variant::Underlined;
  }
  return AdAbstractSelectWidget::Variant::Outlined;
}

AdSelect::Variant toInternalVariant(AdAbstractSelectWidget::Variant value) {
  switch (value) {
    case AdAbstractSelectWidget::Variant::Outlined:
      return AdSelect::Variant::Outlined;
    case AdAbstractSelectWidget::Variant::Filled:
      return AdSelect::Variant::Filled;
    case AdAbstractSelectWidget::Variant::Borderless:
      return AdSelect::Variant::Borderless;
    case AdAbstractSelectWidget::Variant::Underlined:
      return AdSelect::Variant::Underlined;
  }
  return AdSelect::Variant::Outlined;
}

AdAbstractSelectWidget::Status fromInternalStatus(AdSelect::Status value) {
  switch (value) {
    case AdSelect::Status::None:
      return AdAbstractSelectWidget::Status::None;
    case AdSelect::Status::Error:
      return AdAbstractSelectWidget::Status::Error;
    case AdSelect::Status::Warning:
      return AdAbstractSelectWidget::Status::Warning;
  }
  return AdAbstractSelectWidget::Status::None;
}

AdSelect::Status toInternalStatus(AdAbstractSelectWidget::Status value) {
  switch (value) {
    case AdAbstractSelectWidget::Status::None:
      return AdSelect::Status::None;
    case AdAbstractSelectWidget::Status::Error:
      return AdSelect::Status::Error;
    case AdAbstractSelectWidget::Status::Warning:
      return AdSelect::Status::Warning;
  }
  return AdSelect::Status::None;
}

AdAbstractSelectWidget::Placement fromInternalPlacement(AdSelect::Placement value) {
  switch (value) {
    case AdSelect::Placement::BottomLeft:
      return AdAbstractSelectWidget::Placement::BottomLeft;
    case AdSelect::Placement::BottomRight:
      return AdAbstractSelectWidget::Placement::BottomRight;
    case AdSelect::Placement::TopLeft:
      return AdAbstractSelectWidget::Placement::TopLeft;
    case AdSelect::Placement::TopRight:
      return AdAbstractSelectWidget::Placement::TopRight;
    case AdSelect::Placement::BottomCenter:
      return AdAbstractSelectWidget::Placement::BottomCenter;
    case AdSelect::Placement::TopCenter:
      return AdAbstractSelectWidget::Placement::TopCenter;
  }
  return AdAbstractSelectWidget::Placement::BottomLeft;
}

AdSelect::Placement toInternalPlacement(AdAbstractSelectWidget::Placement value) {
  switch (value) {
    case AdAbstractSelectWidget::Placement::BottomLeft:
      return AdSelect::Placement::BottomLeft;
    case AdAbstractSelectWidget::Placement::BottomRight:
      return AdSelect::Placement::BottomRight;
    case AdAbstractSelectWidget::Placement::TopLeft:
      return AdSelect::Placement::TopLeft;
    case AdAbstractSelectWidget::Placement::TopRight:
      return AdSelect::Placement::TopRight;
    case AdAbstractSelectWidget::Placement::BottomCenter:
      return AdSelect::Placement::BottomCenter;
    case AdAbstractSelectWidget::Placement::TopCenter:
      return AdSelect::Placement::TopCenter;
  }
  return AdSelect::Placement::BottomLeft;
}

}  // namespace

AdAbstractSelectWidget::AdAbstractSelectWidget(QWidget* parent) : QWidget(parent) {
  ensureAbstractSelectWidgetAccessibleFactoryInstalled();
  qRegisterMetaType<SelectionItem>("adqt::widgets::select::SelectionItem");
  qRegisterMetaType<QVector<SelectionItem>>("QVector<adqt::widgets::select::SelectionItem>");

  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(0);

  control_ = new AdSelect(this);
  layout_->addWidget(control_);

  setFocusProxy(control_);
  setSizePolicy(control_->sizePolicy());
  applyPopupWidthPolicy();

  connect(control_, &AdSelect::modelChanged, this, &AdAbstractSelectWidget::modelChanged);
  connect(control_, &AdSelect::selectionModelChanged, this,
          &AdAbstractSelectWidget::selectionModelChanged);
  connect(control_, &AdSelect::modelColumnChanged, this,
          &AdAbstractSelectWidget::modelColumnChanged);
  connect(control_, &AdSelect::loadingChanged, this, &AdAbstractSelectWidget::loadingChanged);
  connect(control_, &AdSelect::popupVisibleChanged, this,
          &AdAbstractSelectWidget::popupVisibleChanged);
  connect(control_, &AdSelect::searchEnabledChanged, this,
          &AdAbstractSelectWidget::searchEnabledChanged);
  connect(control_, &AdSelect::searchTextChanged, this, &AdAbstractSelectWidget::searchTextChanged);
  connect(control_, &AdSelect::placementChanged, this, [this](AdSelect::Placement value) {
    emit placementChanged(fromInternalPlacement(value));
  });
  connect(control_, &AdSelect::popupLayerModeChanged, this,
          &AdAbstractSelectWidget::popupLayerModeChanged);
  connect(control_, &AdSelect::placeholderChanged, this,
          &AdAbstractSelectWidget::placeholderChanged);
  connect(control_, &AdSelect::allowClearChanged, this, &AdAbstractSelectWidget::allowClearChanged);
  connect(control_, &AdSelect::controlSizeChanged, this, [this](AdSelect::ControlSize value) {
    emit controlSizeChanged(fromInternalControlSize(value));
  });
  connect(control_, &AdSelect::variantChanged, this,
          [this](AdSelect::Variant value) { emit variantChanged(fromInternalVariant(value)); });
  connect(control_, &AdSelect::statusChanged, this,
          [this](AdSelect::Status value) { emit statusChanged(fromInternalStatus(value)); });
  connect(control_, &AdSelect::prefixTextChanged, this, &AdAbstractSelectWidget::prefixTextChanged);
  connect(control_, &AdSelect::prefixIconRefChanged, this,
          &AdAbstractSelectWidget::prefixIconRefChanged);
  connect(control_, &AdSelect::suffixIconRefChanged, this,
          &AdAbstractSelectWidget::suffixIconRefChanged);
  connect(control_, &AdSelect::feedbackIconRefChanged, this,
          &AdAbstractSelectWidget::feedbackIconRefChanged);
  connect(control_, &AdSelect::optionsChanged, this, &AdAbstractSelectWidget::optionsChanged);
  connect(control_, &AdSelect::componentTokensChanged, this,
          &AdAbstractSelectWidget::componentTokensChanged);
  connect(control_, &AdSelect::semanticStylesChanged, this,
          &AdAbstractSelectWidget::semanticStylesChanged);
}

AdAbstractSelectWidget::~AdAbstractSelectWidget() = default;

QAbstractItemModel* AdAbstractSelectWidget::model() const { return control_->model(); }

void AdAbstractSelectWidget::setModel(QAbstractItemModel* model) { control_->setModel(model); }

QItemSelectionModel* AdAbstractSelectWidget::selectionModel() const {
  return control_->selectionModel();
}

void AdAbstractSelectWidget::setSelectionModel(QItemSelectionModel* model) {
  control_->setSelectionModel(model);
}

int AdAbstractSelectWidget::modelColumn() const { return control_->modelColumn(); }

void AdAbstractSelectWidget::setModelColumn(int value) { control_->setModelColumn(value); }

bool AdAbstractSelectWidget::loading() const { return control_->loading(); }

void AdAbstractSelectWidget::setLoading(bool value) { control_->setLoading(value); }

bool AdAbstractSelectWidget::popupVisible() const { return control_->popupVisible(); }

void AdAbstractSelectWidget::setPopupVisible(bool value) { control_->setPopupVisible(value); }

bool AdAbstractSelectWidget::searchEnabled() const { return control_->searchEnabled(); }

void AdAbstractSelectWidget::setSearchEnabled(bool value) { control_->setSearchEnabled(value); }

QString AdAbstractSelectWidget::searchText() const { return control_->searchText(); }

void AdAbstractSelectWidget::setSearchText(const QString& value) { control_->setSearchText(value); }

AdAbstractSelectWidget::SearchPolicy AdAbstractSelectWidget::searchPolicy() const {
  return control_->searchPolicy();
}

void AdAbstractSelectWidget::setSearchPolicy(SearchPolicy value) {
  if (control_->searchPolicy() == value) {
    return;
  }
  control_->setSearchPolicy(value);
  emit searchPolicyChanged(value);
}

AdAbstractSelectWidget::Placement AdAbstractSelectWidget::placement() const {
  return fromInternalPlacement(control_->placement());
}

void AdAbstractSelectWidget::setPlacement(Placement value) {
  control_->setPlacement(toInternalPlacement(value));
}

AdAbstractSelectWidget::PopupLayerMode AdAbstractSelectWidget::popupLayerMode() const {
  return control_->popupLayerMode();
}

void AdAbstractSelectWidget::setPopupLayerMode(PopupLayerMode value) {
  control_->setPopupLayerMode(value);
}

AdAbstractSelectWidget::PopupWidthMode AdAbstractSelectWidget::popupWidthMode() const {
  return popupWidthMode_;
}

void AdAbstractSelectWidget::setPopupWidthMode(PopupWidthMode value) {
  if (popupWidthMode_ == value) {
    return;
  }
  popupWidthMode_ = value;
  applyPopupWidthPolicy();
  emit popupWidthModeChanged(popupWidthMode_);
}

int AdAbstractSelectWidget::popupWidth() const { return popupWidth_; }

void AdAbstractSelectWidget::setPopupWidth(int value) {
  const int normalized = value > 0 ? value : 0;
  if (popupWidth_ == normalized) {
    return;
  }
  popupWidth_ = normalized;
  if (popupWidthMode_ == PopupWidthMode::FixedWidth) {
    applyPopupWidthPolicy();
  }
  emit popupWidthChanged(popupWidth_);
}

QString AdAbstractSelectWidget::placeholder() const { return control_->placeholder(); }

void AdAbstractSelectWidget::setPlaceholder(const QString& value) {
  control_->setPlaceholder(value);
}

bool AdAbstractSelectWidget::allowClear() const { return control_->allowClear(); }

void AdAbstractSelectWidget::setAllowClear(bool value) { control_->setAllowClear(value); }

AdAbstractSelectWidget::ControlSize AdAbstractSelectWidget::controlSize() const {
  return fromInternalControlSize(control_->controlSize());
}

void AdAbstractSelectWidget::setControlSize(ControlSize value) {
  control_->setControlSize(toInternalControlSize(value));
}

AdAbstractSelectWidget::Variant AdAbstractSelectWidget::variant() const {
  return fromInternalVariant(control_->variant());
}

void AdAbstractSelectWidget::setVariant(Variant value) {
  control_->setVariant(toInternalVariant(value));
}

AdAbstractSelectWidget::Status AdAbstractSelectWidget::status() const {
  return fromInternalStatus(control_->status());
}

void AdAbstractSelectWidget::setStatus(Status value) {
  control_->setStatus(toInternalStatus(value));
}

bool AdAbstractSelectWidget::joinedLeft() const { return control_->joinedLeft(); }

void AdAbstractSelectWidget::setJoinedLeft(bool value) {
  if (control_->joinedLeft() == value) {
    return;
  }
  control_->setJoinedLeft(value);
  emit joinedLeftChanged(value);
}

bool AdAbstractSelectWidget::joinedRight() const { return control_->joinedRight(); }

void AdAbstractSelectWidget::setJoinedRight(bool value) {
  if (control_->joinedRight() == value) {
    return;
  }
  control_->setJoinedRight(value);
  emit joinedRightChanged(value);
}

QString AdAbstractSelectWidget::prefixText() const { return control_->prefixText(); }

void AdAbstractSelectWidget::setPrefixText(const QString& value) { control_->setPrefixText(value); }

adqt::icons::IconRef AdAbstractSelectWidget::prefixIconRef() const {
  return control_->prefixIconRef();
}

void AdAbstractSelectWidget::setPrefixIconRef(const adqt::icons::IconRef& token) {
  control_->setPrefixIconRef(token);
}

adqt::icons::IconRef AdAbstractSelectWidget::suffixIconRef() const {
  return control_->suffixIconRef();
}

void AdAbstractSelectWidget::setSuffixIconRef(const adqt::icons::IconRef& token) {
  control_->setSuffixIconRef(token);
}

adqt::icons::IconRef AdAbstractSelectWidget::feedbackIconRef() const {
  return control_->feedbackIconRef();
}

void AdAbstractSelectWidget::setFeedbackIconRef(const adqt::icons::IconRef& token) {
  control_->setFeedbackIconRef(token);
}

QVector<AdAbstractSelectWidget::Option> AdAbstractSelectWidget::options() const {
  return control_->options();
}

void AdAbstractSelectWidget::setOptions(const QVector<Option>& options) {
  control_->setOptions(options);
}

void AdAbstractSelectWidget::appendOption(const Option& option) { control_->appendOption(option); }

void AdAbstractSelectWidget::clearOptions() { control_->clearOptions(); }

int AdAbstractSelectWidget::valueRole() const { return control_->valueRole(); }

void AdAbstractSelectWidget::setValueRole(int role) { control_->setValueRole(role); }

int AdAbstractSelectWidget::labelRole() const { return control_->labelRole(); }

void AdAbstractSelectWidget::setLabelRole(int role) { control_->setLabelRole(role); }

int AdAbstractSelectWidget::tagTextRole() const { return control_->tagTextRole(); }

void AdAbstractSelectWidget::setTagTextRole(int role) { control_->setTagTextRole(role); }

int AdAbstractSelectWidget::selectedTextRole() const { return control_->selectedTextRole(); }

void AdAbstractSelectWidget::setSelectedTextRole(int role) { control_->setSelectedTextRole(role); }

int AdAbstractSelectWidget::groupRole() const { return control_->groupRole(); }

void AdAbstractSelectWidget::setGroupRole(int role) { control_->setGroupRole(role); }

AdAbstractSelectWidget::RoleConfig AdAbstractSelectWidget::roleConfig() const {
  return control_->roleConfig();
}

void AdAbstractSelectWidget::setRoleConfig(const RoleConfig& config) {
  control_->setRoleConfig(config);
}

QList<int> AdAbstractSelectWidget::searchRoles() const { return control_->searchRoles(); }

void AdAbstractSelectWidget::setSearchRoles(const QList<int>& roles) {
  control_->setSearchRoles(roles);
}

QStringList AdAbstractSelectWidget::searchFilterFields() const {
  return control_->searchFilterFields();
}

void AdAbstractSelectWidget::setSearchFilterFields(const QStringList& fields) {
  control_->setSearchFilterFields(fields);
}

QAbstractItemDelegate* AdAbstractSelectWidget::itemDelegate() const {
  return control_->itemDelegate();
}

void AdAbstractSelectWidget::setItemDelegate(QAbstractItemDelegate* delegate) {
  control_->setItemDelegate(delegate);
}

QWidget* AdAbstractSelectWidget::notFoundContentWidget() const {
  return control_->notFoundContentWidget();
}

void AdAbstractSelectWidget::setNotFoundContentWidget(QWidget* widget) {
  control_->setNotFoundContentWidget(widget);
}

QWidget* AdAbstractSelectWidget::popupFooterWidget() const { return control_->popupFooterWidget(); }

void AdAbstractSelectWidget::setPopupFooterWidget(QWidget* widget) {
  control_->setPopupFooterWidget(widget);
}

AdAbstractSelectWidget::ComponentTokens AdAbstractSelectWidget::componentTokens() const {
  return control_->componentTokens();
}

void AdAbstractSelectWidget::setComponentTokens(const ComponentTokens& tokens) {
  control_->setComponentTokens(tokens);
}

void AdAbstractSelectWidget::resetComponentTokens() { control_->resetComponentTokens(); }

AdAbstractSelectWidget::SemanticStyles AdAbstractSelectWidget::semanticStyles() const {
  return control_->semanticStyles();
}

void AdAbstractSelectWidget::setSemanticStyles(const SemanticStyles& styles) {
  control_->setSemanticStyles(styles);
}

void AdAbstractSelectWidget::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  if (!resolver) {
    control_->setSemanticStyleResolver({});
    return;
  }

  control_->setSemanticStyleResolver(
      [resolver = std::move(resolver)](const AdSelect::StyleContext& ctx) {
        StyleContext sharedCtx;
        switch (ctx.mode) {
          case AdSelect::Mode::Single:
            sharedCtx.mode = Mode::Single;
            break;
          case AdSelect::Mode::Multiple:
            sharedCtx.mode = Mode::Multiple;
            break;
          case AdSelect::Mode::Tags:
            sharedCtx.mode = Mode::Tags;
            break;
        }
        sharedCtx.controlSize = fromInternalControlSize(ctx.controlSize);
        sharedCtx.variant = fromInternalVariant(ctx.variant);
        sharedCtx.status = fromInternalStatus(ctx.status);
        sharedCtx.disabled = ctx.disabled;
        sharedCtx.popupVisible = ctx.popupVisible;
        sharedCtx.searchText = ctx.searchText;
        sharedCtx.currentValues = ctx.currentValues;
        sharedCtx.currentValueKeys = ctx.currentValueKeys;
        return resolver(sharedCtx);
      });
}

QLineEdit* AdAbstractSelectWidget::lineEdit() const { return control_->lineEdit(); }

QListView* AdAbstractSelectWidget::view() const { return control_->view(); }

void AdAbstractSelectWidget::showPopup() { control_->showPopup(); }

void AdAbstractSelectWidget::hidePopup() { control_->hidePopup(); }

QSize AdAbstractSelectWidget::sizeHint() const { return control_->sizeHint(); }

QSize AdAbstractSelectWidget::minimumSizeHint() const { return control_->minimumSizeHint(); }

void AdAbstractSelectWidget::setInternalMode(Mode mode) { control_->setMode(toInternalMode(mode)); }

AdSelect* AdAbstractSelectWidget::internalSelect() const { return control_; }

void AdAbstractSelectWidget::applyPopupWidthPolicy() {
  switch (popupWidthMode_) {
    case PopupWidthMode::MatchControlWidth:
      control_->setPopupWidth(0);
      control_->setPopupMatchSelectWidth(true);
      break;
    case PopupWidthMode::ContentWidth:
      control_->setPopupMatchSelectWidth(false);
      control_->setPopupWidth(0);
      break;
    case PopupWidthMode::FixedWidth:
      control_->setPopupMatchSelectWidth(false);
      control_->setPopupWidth(popupWidth_ > 0 ? popupWidth_ : 1);
      break;
  }
}

}  // namespace adqt::widgets
