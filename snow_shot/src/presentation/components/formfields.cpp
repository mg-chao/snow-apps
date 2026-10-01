#include "snow_shot/presentation/components/formfields.h"

#include "snow_shot/presentation/components/settingspageutils.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/radio.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScopedValueRollback>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace snow_shot::presentation::components::form_fields {
namespace {
using namespace adqt::widgets;

QString choiceText(const Choice& choice) {
    return choice.label.isValid() ? choice.label.translated() : choice.text;
}

QVector<AdSelect::Option> nativeChoices(const QVector<Choice>& choices) {
    QVector<AdSelect::Option> result;
    result.reserve(choices.size());
    for (const auto& choice : choices) {
        result.append({choice.value, choiceText(choice), !choice.enabled});
    }
    return result;
}

bool sameChoices(const QVector<AdSelect::Option>& first, const QVector<AdSelect::Option>& second) {
    if (first.size() != second.size()) {
        return false;
    }
    for (int index = 0; index < first.size(); ++index) {
        const auto& left = first.at(index);
        const auto& right = second.at(index);
        // Native options include synthetic model-role metadata which Choice does not own.
        if (left.value != right.value || left.label != right.label ||
            left.disabled != right.disabled || left.group != right.group) {
            return false;
        }
    }
    return true;
}

void configureLineEdit(AdLineEdit* editor, const Options& options) {
    editor->setControlSize(AdLineEdit::ControlSize::Medium);
    editor->setVariant(AdLineEdit::Variant::Outlined);
    editor->setAllowClear(options.allowClear);
    editor->setReadOnly(options.readOnly);
    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

auto commitOnFinish(FormField* field, CommitPolicy policy) {
    return [field, policy] {
        if (policy == CommitPolicy::OnFinish) {
            field->notifyCommitted();
        }
    };
}

template <typename Editor>
Handle<Editor> makeLineField(Editor* editor, const Metadata& metadata, const Options& options) {
    configureLineEdit(editor, options);
    auto* field =
        new FormField(metadata, options,
                      {editor,
                       editor,
                       [editor] { return QVariant(editor->text()); },
                       [editor](const QVariant& value) { editor->setText(value.toString()); },
                       {}},
                      false);
    QObject::connect(editor, &QLineEdit::textChanged, field, [field] { field->notifyEdited(); });
    const auto finish = commitOnFinish(field, options.commitPolicy);
    QObject::connect(editor, &QLineEdit::editingFinished, field, finish);
    QObject::connect(editor, &AdLineEdit::cleared, field, finish);
    return {field, editor};
}

template <typename Editor>
Handle<Editor> makePathField(Editor* editor, const Metadata& metadata, const Options& options) {
    editor->setControlSize(AdLineEdit::ControlSize::Medium);
    editor->setVariant(AdLineEdit::Variant::Outlined);
    editor->setAllowClear(options.allowClear);
    editor->setReadOnly(options.readOnly);
    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* field =
        new FormField(metadata, options,
                      {editor,
                       editor->lineEdit(),
                       [editor] { return QVariant(editor->text()); },
                       [editor](const QVariant& value) { editor->setText(value.toString()); },
                       {}},
                      true);
    QObject::connect(editor, &DirectoryPathInput::textChanged, field,
                     [field] { field->notifyEdited(); });
    const auto finish = commitOnFinish(field, options.commitPolicy);
    QObject::connect(editor, &DirectoryPathInput::editingFinished, field, finish);
    QObject::connect(editor, &DirectoryPathInput::cleared, field, finish);
    return {field, editor};
}

QWidget* compactHost(QWidget* editor, Presentation presentation) {
    auto* host = new QWidget;
    host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (presentation == Presentation::SettingsRow) {
        layout->addStretch();
        layout->addWidget(editor, 0, Qt::AlignRight | Qt::AlignVCenter);
    } else {
        layout->addWidget(editor, 0, Qt::AlignLeft | Qt::AlignVCenter);
        layout->addStretch();
    }
    host->setFocusProxy(editor);
    return host;
}
} // namespace

struct FormField::Impl {
    Metadata metadata;
    Options options;
    CustomBinding binding;
    QPointer<QWidget> view;
    QPointer<QWidget> control;
    QPointer<QWidget> focus;
    QPointer<adqt::widgets::AdFormItem> item;
    QPointer<QLabel> title;
    QPointer<QLabel> description;
    QPointer<QLabel> feedback;
    QPointer<QWidget> additional;
    QVector<Choice> choices;
    std::function<void(const QVector<Choice>&)> choiceUpdater;
    bool managesChoices = false;
    bool bridgeControlChanges = true;
    bool awaitingOwner = false;
    bool clearingDescriptionOverride = false;
    bool synchronizing = false;
    bool pendingCommit = false;
    int reservedFeedbackHeight = 0;
    int unreservedItemMinimumHeight = 0;
    QStringList errors;
    QStringList warnings;
    QString descriptionOverride;
    bool descriptionError = false;

    void refreshDescriptionStyle(const styles::ThemeColorScheme& scheme) const {
        if (description) {
            QPalette palette = description->palette();
            palette.setColor(QPalette::WindowText, descriptionError
                                                       ? scheme.map.colorErrorText
                                                       : scheme.map.colorTextSecondary);
            description->setPalette(palette);
        }
    }

    [[nodiscard]] QString descriptionText() const {
        return !descriptionOverride.isEmpty() ? descriptionOverride
                                              : metadata.description.translated();
    }

    void refreshAccessibility() const {
        QStringList descriptions;
        if (metadata.description.isValid() || !descriptionOverride.isEmpty()) {
            descriptions.append(descriptionText());
        }
        if (item->required()) {
            // refresh() owns the translated required marker and its accompanying messages.
            item->refresh();
            descriptions.append(control->accessibleDescription());
        } else {
            if (!item->helpText().isEmpty()) {
                descriptions.append(item->helpText());
            }
            descriptions.append(item->errorMessages());
            descriptions.append(item->warningMessages());
            if (!item->extraText().isEmpty()) {
                descriptions.append(item->extraText());
            }
        }
        control->setAccessibleDescription(descriptions.join(QLatin1Char(' ')).trimmed());
        if (focus && focus != control) {
            focus->setAccessibleDescription(control->accessibleDescription());
        }
    }

    void restoreFeedbackSpace() const {
        if (additional && reservedFeedbackHeight > 0) {
            if (item && item->layout()) {
                auto margins = item->layout()->contentsMargins();
                margins.setBottom(0);
                item->layout()->setContentsMargins(margins);
            }
            additional->setMinimumHeight(reservedFeedbackHeight);
            additional->show();
            if (item && item->layout()) {
                // Native feedback refreshes can temporarily hide/reset the additional host.
                // Anchor its reserved space outside that refresh transaction.
                item->setMinimumHeight(
                    std::max(unreservedItemMinimumHeight, item->layout()->minimumSize().height()));
            }
        }
    }
};

FormField::FormField(const Metadata& metadata, const Options& options, CustomBinding binding,
                     bool bridgeControlChanges)
    : m_impl(std::make_unique<Impl>()) {
    Q_ASSERT(binding.control != nullptr);
    auto& state = *m_impl;
    state.metadata = metadata;
    state.options = options;
    state.binding = std::move(binding);
    state.bridgeControlChanges = bridgeControlChanges;
    state.control = state.binding.control;
    state.focus = state.binding.focusWidget ? state.binding.focusWidget : state.control.data();
    state.control->setObjectName(metadata.id);
    QWidget* itemOwner = options.form ? static_cast<QWidget*>(options.form) : options.parent;
    if (options.presentation == Presentation::SettingsRow) {
        state.view = new QWidget(options.parent);
        itemOwner = state.view;
    }
    // AdFormItem refreshes show its contents. Keep standalone fields under a hidden owner
    // throughout initialization so those refreshes cannot show a temporary top-level window.
    std::unique_ptr<QWidget> stagingOwner;
    if (!itemOwner) {
        stagingOwner = std::make_unique<QWidget>();
        itemOwner = stagingOwner.get();
    }
    state.item = new AdFormItem(itemOwner);
    state.item->setObjectName(QStringLiteral("form-field-%1").arg(metadata.id));
    state.item->setFieldKey(metadata.id);
    state.item->setItemLayout(AdFormItem::ItemLayout::Vertical);
    state.item->setRequired(options.required);
    state.item->setHasFeedback(options.hasFeedback);
    state.item->setValidateOnChange(false);
    if (state.binding.readValue) {
        state.item->setValueReader(
            [reader = state.binding.readValue](QWidget*) { return reader(); });
    }
    if (state.binding.writeValue) {
        state.item->setValueWriter(
            [this, writer = state.binding.writeValue](QWidget*, const QVariant& value) {
                synchronize([&writer, &value] { writer(value); });
                m_impl->pendingCommit = false;
            });
    }
    state.item->setControlWidget(state.control);
    if (options.presentation == Presentation::SettingsRow) {
        auto* row = state.view.data();
        row->setObjectName(QStringLiteral("settings-item-%1").arg(metadata.id));
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(24);
        auto* copy = new QWidget(row);
        auto* copyLayout = new QVBoxLayout(copy);
        copyLayout->setContentsMargins(0, 0, 0, 0);
        copyLayout->setSpacing(4);
        state.title = new QLabel(copy);
        state.description = new QLabel(copy);
        state.title->setWordWrap(true);
        state.description->setWordWrap(true);
        copyLayout->addWidget(state.title);
        copyLayout->addWidget(state.description);
        layout->addWidget(copy, 1, Qt::AlignVCenter);
        auto* form = new AdForm(row);
        configureForm(form);
        form->setFormLayout(AdForm::FormLayout::Inline);
        const int width = std::max(1, options.controlWidth);
        form->setFixedWidth(width);
        state.item->setFixedWidth(width);
        form->addItem(state.item);
        layout->addWidget(form, 0, Qt::AlignVCenter);
        state.view = row;
    } else {
        state.view = state.item;
        if (options.form) {
            options.form->addItem(state.item);
        }
    }
    setParent(state.view);
    connect(state.view, &QObject::destroyed, this, [this] {
        auto& current = *m_impl;
        current.synchronizing = true;
        current.pendingCommit = false;
        if (!current.control) {
            return;
        }
        // QWidget emits destroyed before deleting its children. Close owned popups while
        // composite wrappers can still receive their native selectors' forwarded signals.
        auto selectors = current.control->findChildren<AdSelect*>();
        if (auto* selector = qobject_cast<AdSelect*>(current.control)) {
            selectors.prepend(selector);
        }
        for (auto* selector : selectors) {
            selector->hidePopup();
        }
        auto colorPickers = current.control->findChildren<AdColorPicker*>();
        if (auto* picker = qobject_cast<AdColorPicker*>(current.control)) {
            colorPickers.prepend(picker);
        }
        for (auto* picker : colorPickers) {
            picker->setPopupVisible(false);
        }
    });
    state.view->setFocusProxy(state.focus);
    state.feedback = state.item->findChild<QLabel*>(QStringLiteral("ad-form-item-explain"));
    state.additional = state.item->findChild<QWidget*>(QStringLiteral("ad-form-item-additional"));
    state.view->installEventFilter(this);
    if (state.view != state.item) {
        state.item->installEventFilter(this);
    }
    if (state.focus) {
        state.focus->installEventFilter(this);
    }
    connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, this,
            &FormField::applyTheme);
    retranslateUi();
    applyTheme(styles::ThemeManager::instance().themeColorScheme());
    if (stagingOwner) {
        state.awaitingOwner = true;
        state.item->setItemHidden(true);
        state.item->setParent(nullptr);
    }
}

FormField::~FormField() = default;

QWidget* FormField::viewWidget() const {
    return m_impl->view;
}
QWidget* FormField::controlWidget() const {
    return m_impl->control;
}
QWidget* FormField::focusWidget() const {
    QWidget* result = m_impl->focus;
    while (result && result->focusProxy() && result->focusProxy() != result) {
        result = result->focusProxy();
    }
    return result;
}
adqt::widgets::AdFormItem* FormField::item() const {
    return m_impl->item;
}
QVariant FormField::value() const {
    return m_impl->binding.readValue ? m_impl->binding.readValue()
                                     : (m_impl->item ? m_impl->item->value() : QVariant());
}
const Metadata& FormField::metadata() const {
    return m_impl->metadata;
}
bool FormField::isSynchronizing() const {
    return m_impl->synchronizing;
}

void FormField::synchronize(const std::function<void()>& update) {
    const QScopedValueRollback<bool> guard(m_impl->synchronizing, true);
    if (update) {
        update();
    }
    m_impl->restoreFeedbackSpace();
}

void FormField::syncValue(const QVariant& next) {
    synchronize([this, &next] {
        if (m_impl->binding.writeValue) {
            m_impl->binding.writeValue(next);
        }
        m_impl->pendingCommit = false;
    });
}

void FormField::setMetadata(const Metadata& metadata) {
    m_impl->metadata = metadata;
    retranslateUi();
}

void FormField::setDescriptionOverride(const QString& description, bool error) {
    m_impl->clearingDescriptionOverride =
        !m_impl->descriptionOverride.isEmpty() && description.isEmpty();
    m_impl->descriptionOverride = description;
    m_impl->descriptionError = error;
    retranslateUi();
}

void FormField::setChoices(const QVector<Choice>& choices) {
    m_impl->choices = choices;
    m_impl->managesChoices = true;
    synchronize([this] {
        if (m_impl->choiceUpdater) {
            m_impl->choiceUpdater(m_impl->choices);
        }
    });
}

void FormField::setChoiceUpdater(std::function<void(const QVector<Choice>&)> updater) {
    m_impl->choiceUpdater = std::move(updater);
}

void FormField::setFeedback(const QStringList& errors, const QStringList& warnings, bool busy) {
    auto& state = *m_impl;
    state.errors = errors;
    state.warnings = warnings;
    state.item->setErrorMessages(errors);
    state.item->setWarningMessages(warnings);
    state.item->setValidateStatus(!errors.isEmpty()     ? AdFormItem::ValidateStatus::Error
                                  : !warnings.isEmpty() ? AdFormItem::ValidateStatus::Warning
                                  : busy                ? AdFormItem::ValidateStatus::Validating
                                                        : AdFormItem::ValidateStatus::None);
    state.refreshAccessibility();
    state.restoreFeedbackSpace();
}

void FormField::setFieldEnabled(bool enabled) {
    m_impl->view->setEnabled(enabled);
}
void FormField::setFieldVisible(bool visible) {
    m_impl->awaitingOwner = false;
    m_impl->view->setVisible(visible);
    if (m_impl->view == m_impl->item) {
        m_impl->item->setItemHidden(!visible);
    }
}
QLabel* FormField::feedbackLabel() const {
    return m_impl->feedback;
}
void FormField::reserveFeedbackHeight(int pixels) {
    auto& state = *m_impl;
    const int previous = state.reservedFeedbackHeight;
    state.reservedFeedbackHeight = std::max(0, pixels);
    if (previous == 0 && state.reservedFeedbackHeight > 0) {
        state.unreservedItemMinimumHeight = state.item->minimumHeight();
    } else if (previous > 0 && state.reservedFeedbackHeight == 0) {
        state.item->setMinimumHeight(state.unreservedItemMinimumHeight);
    }
    m_impl->item->refresh();
    m_impl->restoreFeedbackSpace();
}

void FormField::retranslateUi() {
    synchronize([this] {
        auto& state = *m_impl;
        const auto& copy = state.metadata;
        if (copy.label.isValid()) {
            const QString label = copy.label.translated();
            if (state.title) {
                state.title->setText(label);
            } else {
                state.item->setLabel(label);
            }
            state.control->setAccessibleName(label);
            if (state.focus && state.focus != state.control) {
                state.focus->setAccessibleName(label);
            }
        }
        if (copy.description.isValid() || !state.descriptionOverride.isEmpty() ||
            state.clearingDescriptionOverride) {
            const QString description = state.descriptionText();
            if (state.description) {
                state.description->setTextFormat(Qt::PlainText);
                state.description->setText(description);
                state.description->setVisible(!description.isEmpty());
                state.refreshDescriptionStyle(styles::ThemeManager::instance().themeColorScheme());
            }
            state.item->setTooltipText(description);
            state.control->setToolTip(description);
            if (state.focus && state.focus != state.control) {
                state.focus->setToolTip(description);
            }
            state.refreshAccessibility();
            state.clearingDescriptionOverride = false;
        } else if (state.description) {
            state.description->hide();
        }
        if (copy.placeholder.isValid()) {
            const QString placeholder = copy.placeholder.translated();
            if (auto* lineEditor = qobject_cast<AdLineEdit*>(state.control)) {
                lineEditor->setPlaceholderText(placeholder);
            } else if (auto* textEditor = qobject_cast<AdTextEdit*>(state.control)) {
                textEditor->setPlaceholderText(placeholder);
            } else if (auto* numberEditor = qobject_cast<AdInputNumber*>(state.control)) {
                numberEditor->setPlaceholderText(placeholder);
            } else if (auto* selectEditor = qobject_cast<AdSelect*>(state.control)) {
                selectEditor->setPlaceholder(placeholder);
            } else if (auto* abstractSelector =
                           qobject_cast<AdAbstractSelectWidget*>(state.control)) {
                abstractSelector->setPlaceholder(placeholder);
            } else if (auto* pathEditor = qobject_cast<DirectoryPathInput*>(state.control)) {
                pathEditor->setPlaceholderText(placeholder);
            }
        }
        if (copy.suffix.isValid()) {
            const QString suffix = copy.suffix.translated();
            state.control->setProperty("formFieldSuffix", suffix);
            if (auto* editor = qobject_cast<AdInputNumber*>(state.control)) {
                editor->setSuffixText(suffix);
            }
        }
        if (state.managesChoices && state.choiceUpdater) {
            state.choiceUpdater(state.choices);
        }
        if (state.binding.retranslate) {
            state.binding.retranslate();
        }
    });
}

void FormField::applyTheme(const styles::ThemeColorScheme& scheme) {
    if (m_impl->title && m_impl->description) {
        components::applySettingItemTheme(m_impl->title, m_impl->description, scheme);
        m_impl->refreshDescriptionStyle(scheme);
        if (auto* layout = qobject_cast<QHBoxLayout*>(m_impl->view->layout())) {
            layout->setSpacing(scheme.metricAlias.marginLG);
        }
        if (auto* copy = m_impl->title->parentWidget(); copy && copy->layout()) {
            copy->layout()->setSpacing(scheme.metricAlias.marginXXS);
        }
    }
    m_impl->item->refresh();
    if (m_impl->metadata.description.isValid() || !m_impl->descriptionOverride.isEmpty() ||
        m_impl->item->required()) {
        m_impl->refreshAccessibility();
    }
    m_impl->restoreFeedbackSpace();
}

void FormField::notifyEdited() {
    auto& state = *m_impl;
    if (state.synchronizing) {
        return;
    }
    if (state.bridgeControlChanges) {
        synchronize([this] { m_impl->item->setValue(value()); });
    }
    state.pendingCommit = true;
    const QVariant next = value();
    emit valueEdited(next);
    if (state.options.commitPolicy == CommitPolicy::Immediate) {
        state.pendingCommit = false;
        emit valueCommitted(next);
    }
}

void FormField::notifyCommitted() {
    auto& state = *m_impl;
    if (!state.synchronizing && state.pendingCommit &&
        state.options.commitPolicy != CommitPolicy::Immediate) {
        state.pendingCommit = false;
        emit valueCommitted(value());
    }
}

bool FormField::eventFilter(QObject* watched, QEvent* event) {
    if (m_impl->awaitingOwner && watched == m_impl->item &&
        ((event->type() == QEvent::ParentChange && m_impl->item->parentWidget()) ||
         event->type() == QEvent::Show)) {
        m_impl->awaitingOwner = false;
        m_impl->item->setItemHidden(false);
    }
    if (event->type() == QEvent::LanguageChange && watched == m_impl->view) {
        retranslateUi();
    } else if (event->type() == QEvent::LayoutRequest && watched == m_impl->item) {
        m_impl->restoreFeedbackSpace();
    } else if (event->type() == QEvent::FocusOut && watched == m_impl->focus &&
               m_impl->options.commitPolicy == CommitPolicy::OnFinish &&
               qobject_cast<AdTextEdit*>(m_impl->control)) {
        notifyCommitted();
    }
    return QObject::eventFilter(watched, event);
}

void configureForm(AdForm* form) {
    if (!form) {
        return;
    }
    form->setFormLayout(AdForm::FormLayout::Vertical);
    form->setLabelAlign(AdForm::LabelAlign::Left);
    form->setLabelWrap(true);
    form->setRequiredMark(AdForm::RequiredMark::Hidden);
    form->setColon(false);
    form->setControlSize(AdForm::ControlSize::Medium);
    form->setVariant(AdForm::Variant::Outlined);
}

void configureTwoColumnGrid(QGridLayout* grid) {
    if (!grid) {
        return;
    }
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(
        styles::ThemeManager::instance().themeColorScheme().metricAlias.marginLG);
    grid->setVerticalSpacing(0);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
}

Handle<AdLineEdit> text(const Metadata& metadata, const Options& options) {
    return makeLineField(new AdLineEdit, metadata, options);
}
Handle<AdPasswordEdit> password(const Metadata& metadata, const Options& options) {
    return makeLineField(new AdPasswordEdit, metadata, options);
}
Handle<AdTextEdit> textArea(const Metadata& metadata, const Options& options) {
    auto* editor = new AdTextEdit;
    editor->setControlSize(AdTextEdit::ControlSize::Medium);
    editor->setVariant(AdTextEdit::Variant::Outlined);
    editor->setAllowClear(options.allowClear);
    editor->setReadOnly(options.readOnly);
    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* field =
        new FormField(metadata, options,
                      {editor,
                       editor,
                       [editor] { return QVariant(editor->plainText()); },
                       [editor](const QVariant& value) { editor->setPlainText(value.toString()); },
                       {}},
                      false);
    QObject::connect(editor, &AdTextEdit::plainTextChanged, field,
                     [field] { field->notifyEdited(); });
    return {field, editor};
}

Handle<AdSelect> select(const Metadata& metadata, const QVector<Choice>& choices,
                        const Options& options) {
    auto* editor = new AdSelect;
    editor->setControlSize(AdSelect::ControlSize::Middle);
    editor->setVariant(AdSelect::Variant::Outlined);
    editor->setSearchEnabled(options.searchEnabled);
    editor->setSearchFilterFields({QStringLiteral("label")});
    editor->setAllowClear(options.allowClear);
    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (options.popupInModal) {
        editor->setPopupLayerMode(AdSelect::PopupLayerMode::QtTool);
    }
    auto* field = new FormField(metadata, options,
                                {editor,
                                 editor,
                                 [editor] {
                                     return editor->mode() == AdSelect::Mode::Single
                                                ? editor->currentValue()
                                                : QVariant(editor->currentValues());
                                 },
                                 [editor](const QVariant& value) {
                                     if (editor->mode() == AdSelect::Mode::Single) {
                                         editor->setCurrentValue(value);
                                     } else {
                                         editor->setCurrentValues(value.toList());
                                     }
                                 },
                                 {}},
                                false);
    field->setChoiceUpdater([editor](const QVector<Choice>& next) {
        const auto nativeOptions = nativeChoices(next);
        if (sameChoices(editor->options(), nativeOptions)) {
            return;
        }
        const QVariantList values = editor->currentValues();
        const QVariant value = editor->currentValue();
        editor->setOptions(nativeOptions);
        if (editor->mode() == AdSelect::Mode::Single) {
            editor->setCurrentValue(value);
        } else {
            editor->setCurrentValues(values);
        }
    });
    if (!choices.isEmpty()) {
        field->setChoices(choices);
    }
    const auto finish = commitOnFinish(field, options.commitPolicy);
    QObject::connect(editor, &AdSelect::currentValueChanged, field, [field, editor, finish] {
        if (editor->mode() == AdSelect::Mode::Single) {
            field->notifyEdited();
            finish();
        }
    });
    QObject::connect(editor, &AdSelect::currentValuesChanged, field, [field, editor, finish] {
        if (editor->mode() != AdSelect::Mode::Single) {
            field->notifyEdited();
            finish();
        }
    });
    return {field, editor};
}

Handle<AdComboBox> comboBox(const Metadata& metadata, const QVector<Choice>& choices,
                            const Options& options) {
    auto* editor = new AdComboBox;
    editor->setControlSize(AdComboBox::ControlSize::Middle);
    editor->setVariant(AdComboBox::Variant::Outlined);
    editor->setEditable(options.editable);
    editor->lineEdit()->setReadOnly(options.readOnly);
    editor->setAllowClear(options.allowClear);
    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (options.popupInModal) {
        editor->setPopupLayerMode(AdComboBox::PopupLayerMode::QtTool);
    }
    auto* field =
        new FormField(metadata, options,
                      {editor,
                       editor,
                       [editor] { return editor->currentValue(); },
                       [editor](const QVariant& value) { editor->setCurrentValue(value); },
                       {}},
                      true);
    field->setChoiceUpdater([editor](const QVector<Choice>& next) {
        const auto nativeOptions = nativeChoices(next);
        if (sameChoices(editor->options(), nativeOptions)) {
            return;
        }
        const QVariant value = editor->currentValue();
        editor->setOptions(nativeOptions);
        editor->setCurrentValue(value);
    });
    if (!choices.isEmpty()) {
        field->setChoices(choices);
    }
    auto typing = std::make_shared<bool>(false);
    const auto finish = commitOnFinish(field, options.commitPolicy);
    QObject::connect(editor, &AdComboBox::currentValueChanged, field, [field, typing, finish] {
        field->notifyEdited();
        if (!*typing) {
            finish();
        }
    });
    QObject::connect(editor->lineEdit(), &QLineEdit::textEdited, field,
                     [editor, typing](const QString& value) {
                         if (editor->editable()) {
                             const QScopedValueRollback<bool> guard(*typing, true);
                             editor->setCurrentValue(value);
                         }
                     });
    QObject::connect(editor->lineEdit(), &QLineEdit::editingFinished, field, finish);
    return {field, editor};
}

Handle<AdMultiSelect> multiSelect(const Metadata& metadata, const QVector<Choice>& choices,
                                  const Options& options) {
    auto* editor = new AdMultiSelect;
    editor->setControlSize(AdMultiSelect::ControlSize::Middle);
    editor->setVariant(AdMultiSelect::Variant::Outlined);
    editor->setSearchEnabled(options.searchEnabled);
    editor->setSearchFilterFields({QStringLiteral("label")});
    editor->setResponsiveMaxTagCount(true);
    editor->setAllowClear(options.allowClear);
    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (options.popupInModal) {
        editor->setPopupLayerMode(AdMultiSelect::PopupLayerMode::QtTool);
    }
    auto* field = new FormField(
        metadata, options,
        {editor,
         editor,
         [editor] { return QVariant(editor->selectedValues()); },
         [editor](const QVariant& value) { editor->setSelectedValues(value.toList()); },
         {}},
        true);
    field->setChoiceUpdater([editor](const QVector<Choice>& next) {
        const auto nativeOptions = nativeChoices(next);
        if (sameChoices(editor->options(), nativeOptions)) {
            return;
        }
        const QVariantList values = editor->selectedValues();
        editor->setOptions(nativeOptions);
        editor->setSelectedValues(values);
    });
    if (!choices.isEmpty()) {
        field->setChoices(choices);
    }
    QObject::connect(editor, &AdMultiSelect::selectedValuesChanged, field,
                     [field, finish = commitOnFinish(field, options.commitPolicy)] {
                         field->notifyEdited();
                         finish();
                     });
    return {field, editor};
}

Handle<AdSwitch> switchField(const Metadata& metadata, const Options& options) {
    auto* editor = new AdSwitch;
    editor->setObjectName(metadata.id);
    editor->setControlSize(AdSwitch::ControlSize::Medium);
    auto* host = compactHost(editor, options.presentation);
    auto* field =
        new FormField(metadata, options,
                      {host,
                       editor,
                       [editor] { return QVariant(editor->isChecked()); },
                       [editor](const QVariant& value) { editor->setChecked(value.toBool()); },
                       {}},
                      true);
    QObject::connect(editor, &QAbstractButton::toggled, field,
                     [field, finish = commitOnFinish(field, options.commitPolicy)] {
                         field->notifyEdited();
                         finish();
                     });
    return {field, editor};
}

Handle<AdInputNumber> number(const Metadata& metadata, const NumberOptions& numberOptions,
                             const Options& options) {
    auto* editor = new AdInputNumber;
    editor->setControlSize(AdInputNumber::ControlSize::Medium);
    editor->setVariant(AdInputNumber::Variant::Outlined);
    editor->setStepButtonLayout(AdInputNumber::StepButtonLayout::Compact);
    editor->setRange(numberOptions.minimum, numberOptions.maximum);
    editor->setSingleStep(numberOptions.step);
    editor->setDecimals(numberOptions.decimals);
    editor->setWheelStepEnabled(numberOptions.wheelStepEnabled);
    editor->setReadOnly(options.readOnly);
    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* field = new FormField(
        metadata, options,
        {editor,
         editor,
         [editor] {
             if (!editor->hasValue()) {
                 return QVariant();
             }
             return editor->valueMode() == AdInputNumber::ValueMode::ExactDecimal
                        ? QVariant(editor->exactValue())
                        : QVariant(editor->value());
         },
         [editor](const QVariant& value) {
             if (!value.isValid() || value.isNull() || value.toString().isEmpty()) {
                 editor->clear();
             } else if (editor->valueMode() == AdInputNumber::ValueMode::ExactDecimal) {
                 editor->setExactValue(value.toString());
             } else {
                 editor->setValue(value.toDouble());
             }
         },
         {}},
        false);
    QObject::connect(editor, &AdInputNumber::valueChanged, field, [field, editor] {
        if (editor->valueMode() == AdInputNumber::ValueMode::Number) {
            field->notifyEdited();
        }
    });
    QObject::connect(editor, &AdInputNumber::exactValueChanged, field, [field, editor] {
        if (editor->valueMode() == AdInputNumber::ValueMode::ExactDecimal) {
            field->notifyEdited();
        }
    });
    QObject::connect(editor, &QAbstractSpinBox::editingFinished, field,
                     commitOnFinish(field, options.commitPolicy));
    return {field, editor};
}

Handle<AdSlider> slider(const Metadata& metadata, const NumberOptions& numberOptions,
                        const Options& options) {
    auto* host = new QWidget;
    host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* editor = new AdSlider(host);
    editor->setObjectName(metadata.id);
    editor->setRange(numberOptions.minimum, numberOptions.maximum);
    editor->setSingleStep(numberOptions.step);
    editor->setPageStep(std::max(numberOptions.step, 10.0));
    editor->setTooltipEnabled(true);
    editor->setTooltipFormatter([host, decimals = numberOptions.decimals](double value) {
        return QStringLiteral("%1%2").arg(QString::number(value, 'f', std::max(0, decimals)),
                                          host->property("formFieldSuffix").toString());
    });
    auto* label = new QLabel(host);
    label->setObjectName(QStringLiteral("form-field-value"));
    label->setMinimumWidth(42);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(editor, 1);
    layout->addWidget(label);
    host->setFocusProxy(editor);
    const auto refreshValue = [editor, label, host, decimals = numberOptions.decimals] {
        label->setText(
            QStringLiteral("%1%2").arg(QString::number(editor->value(), 'f', std::max(0, decimals)),
                                       host->property("formFieldSuffix").toString()));
    };
    auto* field = new FormField(metadata, options,
                                {host, editor, [editor] { return QVariant(editor->value()); },
                                 [editor, refreshValue](const QVariant& value) {
                                     editor->setValue(value.toDouble());
                                     refreshValue();
                                 },
                                 refreshValue},
                                true);
    QObject::connect(editor, &AdSlider::valueChanged, field, [field, refreshValue] {
        refreshValue();
        field->notifyEdited();
    });
    QObject::connect(editor, &AdSlider::editingFinished, field,
                     commitOnFinish(field, options.commitPolicy));
    return {field, editor};
}

Handle<AdColorPicker> color(const Metadata& metadata, const Options& options) {
    auto* editor = new AdColorPicker;
    editor->setPopupPrewarmEnabled(false);
    editor->setSize(AdColorPicker::Size::Middle);
    editor->setModeOptions({AdColorPicker::Mode::Solid});
    editor->setMode(AdColorPicker::Mode::Solid);
    editor->setFormat(AdColorPicker::Format::Hex);
    editor->setAllowClear(options.allowClear);
    editor->setTriggerTextVisible(true);
    if (options.popupInModal) {
        editor->setPopupLayerMode(AdColorPicker::PopupLayerMode::QtTool);
    }
    if (auto* layout = qobject_cast<QHBoxLayout*>(editor->layout());
        layout && layout->count() > 0 && layout->itemAt(0)->widget()) {
        layout->setAlignment(layout->itemAt(0)->widget(),
                             options.presentation == Presentation::SettingsRow
                                 ? Qt::AlignRight | Qt::AlignVCenter
                                 : Qt::AlignLeft | Qt::AlignVCenter);
    }
    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    Options colorOptions = options;
    if (colorOptions.commitPolicy == CommitPolicy::Immediate) {
        colorOptions.commitPolicy = CommitPolicy::OnFinish;
    }
    auto* field = new FormField(metadata, colorOptions,
                                {editor,
                                 editor,
                                 [editor] {
                                     return editor->value().isNone()
                                                ? QVariant()
                                                : QVariant::fromValue(editor->value().solidColor);
                                 },
                                 [editor](const QVariant& value) {
                                     if (!value.isValid() || value.isNull()) {
                                         editor->setValue(AdColorValue::none());
                                     } else if (value.canConvert<AdColorValue>()) {
                                         editor->setValue(value.value<AdColorValue>());
                                     } else {
                                         const QColor colorValue = value.canConvert<QColor>()
                                                                       ? value.value<QColor>()
                                                                       : QColor(value.toString());
                                         editor->setValue(AdColorValue::solid(colorValue));
                                     }
                                 },
                                 {}},
                                true);
    QObject::connect(editor, &AdColorPicker::valueChanged, field,
                     [field] { field->notifyEdited(); });
    QObject::connect(editor, &AdColorPicker::editingFinished, field,
                     commitOnFinish(field, colorOptions.commitPolicy));
    return {field, editor};
}

Handle<AdRadioButtonGroup> radio(const Metadata& metadata, const QVector<Choice>& choices,
                                 const Options& options) {
    auto* host = new QWidget;
    host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* editor = new AdRadioButtonGroup(host);
    editor->setObjectName(metadata.id);
    editor->setControlSize(AdRadio::ControlSize::Medium);
    editor->setManagedLayout(layout);
    auto values = std::make_shared<QVector<QVariant>>();
    const auto read = [editor, values] {
        const int index = editor->checkedId();
        return index >= 0 && index < values->size() ? values->at(index) : QVariant();
    };
    const auto write = [editor, values](const QVariant& value) {
        const auto found = std::find(values->cbegin(), values->cend(), value);
        if (found != values->cend()) {
            editor->setCheckedId(static_cast<int>(std::distance(values->cbegin(), found)));
        } else {
            editor->setCheckedId(-1);
        }
    };
    const auto refreshFocus = [editor, host, values] {
        const auto enabled = [host](QWidget* widget) {
            return widget && widget->isEnabledTo(host);
        };
        QWidget* target = host->focusProxy();
        if (!enabled(target)) {
            target = editor->checkedButton();
        }
        if (!enabled(target)) {
            target = nullptr;
            for (int index = 0; index < values->size(); ++index) {
                if (auto* button = editor->button(index); enabled(button)) {
                    target = button;
                    break;
                }
            }
        }
        host->setFocusProxy(target);
    };
    auto* field = new FormField(metadata, options, {host, host, read, write, {}}, true);
    field->setChoiceUpdater([editor, host, layout, values, read, write, refreshFocus,
                             presentation = options.presentation](const QVector<Choice>& next) {
        bool sameValues = values->size() == next.size();
        for (int index = 0; sameValues && index < next.size(); ++index) {
            sameValues = values->at(index) == next.at(index).value;
        }
        if (sameValues) {
            for (int index = 0; index < next.size(); ++index) {
                if (auto* button = editor->button(index)) {
                    button->setText(choiceText(next.at(index)));
                    button->setEnabled(next.at(index).enabled);
                }
            }
            refreshFocus();
            return;
        }
        const QVariant previous = read();
        while (QLayoutItem* layoutItem = layout->takeAt(0)) {
            QWidget* widget = layoutItem->widget();
            if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                editor->removeButton(button);
            }
            delete widget;
            delete layoutItem;
        }
        values->clear();
        for (int index = 0; index < next.size(); ++index) {
            const auto& choice = next.at(index);
            auto* button = new AdRadio(host);
            button->setText(choiceText(choice));
            button->setEnabled(choice.enabled);
            editor->addButton(button, index);
            layout->addWidget(button, 0,
                              presentation == Presentation::SettingsRow ? Qt::AlignRight
                                                                        : Qt::AlignLeft);
            values->append(choice.value);
        }
        write(previous);
        refreshFocus();
    });
    if (!choices.isEmpty()) {
        field->setChoices(choices);
    }
    QObject::connect(editor, &AdRadioButtonGroup::checkedIdChanged, field,
                     [field, finish = commitOnFinish(field, options.commitPolicy)] {
                         field->notifyEdited();
                         finish();
                     });
    return {field, editor};
}

Handle<FilePathInput> filePath(const Metadata& metadata, const Options& options) {
    return makePathField(new FilePathInput, metadata, options);
}
Handle<DirectoryPathInput> directoryPath(const Metadata& metadata, const Options& options) {
    return makePathField(new DirectoryPathInput, metadata, options);
}
Handle<QWidget> custom(const Metadata& metadata, CustomBinding binding, const Options& options) {
    QWidget* editor = binding.control;
    return {new FormField(metadata, options, std::move(binding), true), editor};
}

} // namespace snow_shot::presentation::components::form_fields
