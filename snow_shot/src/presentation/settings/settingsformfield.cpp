#include "snow_shot/presentation/settings/settingsformfield.h"

#include "snow_shot/app/edition.h"
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
#include "snow_shot/network/snowshotapiclient.h"
#endif
#include "snow_shot/storage/configurationschema.h"
#include "widgets/radio.h"

#include <QEvent>
#include <QFileDialog>
#include <QIcon>
#include <QLabel>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyle>

#include <algorithm>
#include <type_traits>

namespace snow_shot::presentation::settings {
namespace fields = components::form_fields;

struct SettingsFormField::Impl {
    Impl(SettingsFormField& owner, const SettingsFieldDescriptor& fieldDescriptor,
         SettingsRuntimeSession& runtime, fields::Options options)
        : q(owner), descriptor(fieldDescriptor), session(runtime) {
        const auto& definition = *descriptor.definition;
        fields::Metadata metadata{descriptor.id, definition.title, definition.description};
        options.allowClear = descriptor.kind == SettingsFieldKind::FilePath ||
                             descriptor.kind == SettingsFieldKind::DirectoryPath;
        std::visit(
            [&](const auto& payload) {
                using Payload = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, SettingsSelectDefinition>) {
                    const auto handle = fields::select(metadata, {}, options);
                    field = handle.field;
                    editor = handle.editor;
                    if (payload.binding == SettingsSelectBinding::AppFont) {
                        field->setChoiceUpdater(
                            [this](const auto& choices) { updateFontModel(choices); });
                        QObject::connect(handle.editor, &adqt::widgets::AdSelect::popupOpening, &q,
                                         [this] {
                                             if (fontOptionsLoaded)
                                                 return;
                                             fontOptionsLoaded = true;
                                             session.requestFontOptions();
                                             syncChoices();
                                         });
                    }
                } else if constexpr (std::is_same_v<Payload, SettingsMultiSelectDefinition>) {
                    const auto handle = fields::multiSelect(metadata, {}, options);
                    field = handle.field;
                    editor = handle.editor;
                } else if constexpr (std::is_same_v<Payload, SettingsSwitchDefinition>) {
                    const auto handle = fields::switchField(metadata, options);
                    field = handle.field;
                    editor = handle.editor;
                } else if constexpr (std::is_same_v<Payload, SettingsIntegerDefinition> ||
                                     std::is_same_v<Payload, SettingsSliderDefinition>) {
                    metadata.suffix = payload.suffix;
                    fields::NumberOptions numberOptions;
                    const auto* schema =
                        storage::ConfigurationSchema::entry(descriptor.configurationKey);
                    Q_ASSERT(schema != nullptr && schema->integerRange.has_value());
                    if (schema != nullptr && schema->integerRange.has_value()) {
                        numberOptions.minimum = schema->integerRange->minimum;
                        numberOptions.maximum = schema->integerRange->maximum;
                        numberOptions.step = schema->integerRange->step;
                    }
                    if constexpr (std::is_same_v<Payload, SettingsIntegerDefinition>) {
                        const auto handle = fields::number(metadata, numberOptions, options);
                        field = handle.field;
                        editor = handle.editor;
                    } else {
                        // Keep drag previews local and apply only the completed adjustment.
                        options.commitPolicy = fields::CommitPolicy::OnFinish;
                        const auto handle = fields::slider(metadata, numberOptions, options);
                        field = handle.field;
                        editor = handle.editor;
                        handle.editor->setPageStep(std::max(numberOptions.step, 10.0));
                        handle.editor->setTooltipEnabled(true);
                    }
                } else if constexpr (std::is_same_v<Payload, SettingsColorDefinition>) {
                    options.commitPolicy = fields::CommitPolicy::OnFinish;
                    const auto handle = fields::color(metadata, options);
                    field = handle.field;
                    editor = handle.editor;
                    handle.editor->setAlphaChannelEnabled(payload.alphaChannelEnabled);
                    handle.editor->setAllowClear(false);
                    handle.editor->setTriggerTextVisible(true);
                } else if constexpr (std::is_same_v<Payload, SettingsRadioDefinition>) {
                    QVector<fields::Choice> choices;
                    for (const auto& option : payload.options)
                        choices.push_back({option.value, option.label});
                    const auto handle = fields::radio(metadata, choices, options);
                    field = handle.field;
                    editor = field->controlWidget();
                    radioGroup = handle.editor;
                    radioGroup->setControlSize(adqt::widgets::AdRadio::ControlSize::Small);
                    for (int index = 0; index < payload.options.size(); ++index) {
                        if (auto* radio =
                                qobject_cast<adqt::widgets::AdRadio*>(radioGroup->button(index))) {
                            radio->setIcon(QIcon(payload.options.at(index).iconResource));
                            radio->setIconSize(QSize(24, 24));
                        }
                    }
                } else if constexpr (std::is_same_v<Payload, SettingsFilePathDefinition>) {
                    options.commitPolicy = fields::CommitPolicy::OnFinish;
                    const auto handle = fields::filePath(metadata, options);
                    field = handle.field;
                    editor = handle.editor;
                    QObject::connect(handle.editor, &DirectoryPathInput::browseRequested, &q,
                                     [this, payload](const QString& current) {
                                         const QString path = QFileDialog::getOpenFileName(
                                             field->viewWidget()->window(),
                                             payload.dialogTitle.translated(), current,
                                             payload.fileFilter.translated());
                                         commitBrowsedPath(path);
                                     });
                } else if constexpr (std::is_same_v<Payload, SettingsDirectoryPathDefinition>) {
                    options.commitPolicy = fields::CommitPolicy::OnFinish;
                    const auto handle = fields::directoryPath(metadata, options);
                    field = handle.field;
                    editor = handle.editor;
                    QObject::connect(handle.editor, &DirectoryPathInput::browseRequested, &q,
                                     [this, payload](const QString& current) {
                                         const QString path = QFileDialog::getExistingDirectory(
                                             field->viewWidget()->window(),
                                             payload.dialogTitle.translated(), current);
                                         commitBrowsedPath(path);
                                     });
                } else if constexpr (std::is_same_v<Payload, SettingsTextDefinition>) {
                    options.commitPolicy = fields::CommitPolicy::OnFinish;
                    options.allowClear = payload.binding == SettingsTextBinding::ServerUrl;
                    const auto handle = fields::text(metadata, options);
                    field = handle.field;
                    editor = handle.editor;
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
                    if (payload.binding == SettingsTextBinding::ServerUrl)
                        handle.editor->setPlaceholderText(SnowShotApiClient::configuredBaseUrl());
#endif
                }
            },
            definition.payload);
        Q_ASSERT(field != nullptr && editor != nullptr);
        q.setParent(field->viewWidget());
        field->viewWidget()->setObjectName(
            generatedObjectName(QStringLiteral("settings-item"), descriptor.id));
        editor->setObjectName(
            generatedObjectName(QStringLiteral("settings-control"), descriptor.id));
        if (radioGroup != nullptr && field->focusWidget() != editor) {
            editor->setObjectName(descriptor.id);
            field->focusWidget()->setObjectName(
                generatedObjectName(QStringLiteral("settings-control"), descriptor.id));
        }
        QObject::connect(
            field, &fields::FormField::valueCommitted, &q, [this](const QVariant& value) {
                const QVariant draft = descriptor.kind == SettingsFieldKind::Integer ||
                                               descriptor.kind == SettingsFieldKind::Slider
                                           ? QVariant(value.toInt())
                                           : value;
                if (const auto* file =
                        std::get_if<SettingsFilePathDefinition>(&descriptor.definition->payload)) {
                    static_cast<void>(session.applyFilePathValue(file->binding, draft.toString()));
                } else {
                    static_cast<void>(session.submitDraft(descriptor.id, draft));
                }
                sync();
            });
        QObject::connect(&session, &SettingsRuntimeSession::fieldChanged, &q,
                         [this](const QString& id, const SettingsFieldState& state) {
                             if (id == descriptor.id)
                                 sync(&state);
                         });
        QObject::connect(&session, &SettingsRuntimeSession::filePathStatusChanged, &q,
                         [this](SettingsFilePathBinding binding) {
                             const auto* file = std::get_if<SettingsFilePathDefinition>(
                                 &descriptor.definition->payload);
                             if (file != nullptr && file->binding == binding)
                                 sync();
                         });
        QObject::connect(&session, &SettingsRuntimeSession::optionsChanged, &q,
                         [this](const QString& id, const SettingsOptions& optionsState) {
                             if (id != descriptor.id)
                                 return;
                             optionsError = optionsState.error;
                             optionsLoading = optionsState.loading;
                             syncChoices();
                             sync();
                         });
        retranslateUi();
    }

    void commitBrowsedPath(const QString& path) {
        if (path.isEmpty())
            return;
        field->synchronize(
            [this, &path] { qobject_cast<DirectoryPathInput*>(editor)->setText(path); });
        field->notifyEdited();
        field->notifyCommitted();
    }

    QVector<fields::Choice> selectChoices(const SettingsSelectDefinition& definition) const {
        QVector<fields::Choice> choices;
        for (const auto& option : definition.options)
            choices.push_back({option.value, option.label});
        const bool font = definition.binding == SettingsSelectBinding::AppFont;
        if (!font || fontOptionsLoaded) {
            for (const auto& option : session.dynamicSelectOptions(definition.binding))
                choices.push_back({option.value, {}, option.label});
        }
        if (font) {
            const QString current = session.state(descriptor.id).draftValue.toString();
            if (!current.isEmpty() &&
                std::none_of(choices.cbegin(), choices.cend(), [&current](const auto& choice) {
                    return choice.value.toString() == current;
                }))
                choices.push_back({current, {}, current});
            std::sort(choices.begin(), choices.end(), [](const auto& first, const auto& second) {
                if (first.value.toString().isEmpty() != second.value.toString().isEmpty())
                    return first.value.toString().isEmpty();
                const QString a = first.label.isValid() ? first.label.translated() : first.text;
                const QString b = second.label.isValid() ? second.label.translated() : second.text;
                return QString::compare(a, b, Qt::CaseInsensitive) < 0;
            });
        }
        return choices;
    }

    void syncChoices() {
        const auto& payload = descriptor.definition->payload;
        if (const auto* select = std::get_if<SettingsSelectDefinition>(&payload)) {
            field->setChoices(selectChoices(*select));
            if (auto* control = qobject_cast<adqt::widgets::AdSelect*>(editor))
                control->setLoading(optionsLoading);
        } else if (const auto* multi = std::get_if<SettingsMultiSelectDefinition>(&payload)) {
            QVector<fields::Choice> choices;
            for (const auto& option : multi->options)
                choices.push_back({option.value, option.label});
            field->setChoices(choices);
        }
    }

    void updateFontModel(const QVector<fields::Choice>& choices) {
        QVector<adqt::widgets::AdSelect::Option> options;
        for (const auto& choice : choices) {
            adqt::widgets::AdSelect::Option option;
            option.value = choice.value;
            option.label = choice.label.isValid() ? choice.label.translated() : choice.text;
            options.push_back(option);
        }
        const auto same = [](const auto& first, const auto& second) {
            return first.value == second.value && first.label == second.label;
        };
        if (fontModelInitialized && options.size() == presentedFontOptions.size() &&
            std::equal(options.cbegin(), options.cend(), presentedFontOptions.cbegin(), same))
            return;
        fontModelInitialized = true;
        presentedFontOptions = options;
        auto* control = qobject_cast<adqt::widgets::AdSelect*>(editor);
        const QSignalBlocker blocker(control);
        auto* model = new QStandardItemModel(control);
        for (const auto& option : options) {
            auto* row = new QStandardItem(option.label);
            row->setData(option.label, adqt::widgets::AdSelect::DefaultLabelRole);
            row->setData(option.value, adqt::widgets::AdSelect::DefaultValueRole);
            if (!option.value.toString().isEmpty())
                row->setData(QFont(option.value.toString()), Qt::FontRole);
            model->appendRow(row);
        }
        auto* previousModel = control->model();
        control->setModel(model);
        control->setCurrentValue(session.state(descriptor.id).draftValue);
        if (previousModel != nullptr)
            previousModel->deleteLater();
    }

    void sync(const SettingsFieldState* providedState = nullptr) {
        const auto state = providedState != nullptr ? *providedState : session.state(descriptor.id);
        // Repeated refreshes and translation preserve a text draft that has not yet committed.
        if (!hasState || lastDraft != state.draftValue) {
            if (const auto* select =
                    std::get_if<SettingsSelectDefinition>(&descriptor.definition->payload);
                select != nullptr && select->binding == SettingsSelectBinding::AppFont)
                syncChoices();
            field->syncValue(state.draftValue);
            lastDraft = state.draftValue;
            hasState = true;
        }
        bool enabled = state.enabled;
        if (const auto* toggle =
                std::get_if<SettingsSwitchDefinition>(&descriptor.definition->payload)) {
            enabled = enabled && session.switchEnabled(toggle->binding);
            const QString hint = session.switchHint(toggle->binding);
            field->setDescriptionOverride(hint);
            editor->setToolTip(hint);
        }
        field->setFieldEnabled(enabled);
        field->setFieldVisible(state.visible);
        const QString error = !state.error.isEmpty() ? state.error : optionsError;
        field->setFeedback(error.isEmpty() ? QStringList() : QStringList{error}, {}, state.busy);
        if (const auto* file =
                std::get_if<SettingsFilePathDefinition>(&descriptor.definition->payload)) {
            const QString status = session.filePathStatus(file->binding);
            const bool statusError = session.filePathStatusError(file->binding);
            QString description;
            if (!status.isEmpty()) {
                description = descriptor.definition->description.translated();
                if (!description.isEmpty())
                    description += QLatin1Char('\n');
                description += status;
            }
            field->setDescriptionOverride(description, statusError);
            if (statusError)
                field->item()->setValidateStatus(adqt::widgets::AdFormItem::ValidateStatus::Error);
            qobject_cast<FilePathInput*>(editor)->lineEdit()->setStatus(
                statusError || !error.isEmpty() ? adqt::widgets::AdLineEdit::Status::Error
                                                : adqt::widgets::AdLineEdit::Status::None);
        }
        const auto applyProperties = [&state](QWidget* target) {
            bool changed = false;
            const auto setProperty = [target, &changed](const char* name, const QVariant& value) {
                if (target->property(name) != value) {
                    target->setProperty(name, value);
                    changed = true;
                }
            };
            setProperty("settingsDirty", state.dirty);
            setProperty("settingsPending", state.busy);
            setProperty("settingsConflicted", state.conflicted);
            setProperty("settingsError", state.error);
            if (changed && target->testAttribute(Qt::WA_WState_Polished)) {
                target->style()->unpolish(target);
                target->style()->polish(target);
                target->update();
            }
        };
        applyProperties(editor);
        if (field->focusWidget() != editor)
            applyProperties(field->focusWidget());
    }

    void retranslateUi() {
        syncChoices();
        field->retranslateUi();
        if (const auto* file =
                std::get_if<SettingsFilePathDefinition>(&descriptor.definition->payload)) {
            auto* control = qobject_cast<FilePathInput*>(editor);
            control->setBrowseButtonText(file->buttonText.translated());
            control->setPlaceholderText(
                file->fileFilter.translated().section(QStringLiteral(";;"), 0, 0));
        } else if (const auto* directory = std::get_if<SettingsDirectoryPathDefinition>(
                       &descriptor.definition->payload)) {
            qobject_cast<DirectoryPathInput*>(editor)->setBrowseButtonText(
                directory->buttonText.translated());
        }
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
        else if (const auto* text =
                     std::get_if<SettingsTextDefinition>(&descriptor.definition->payload);
                 text != nullptr && text->binding == SettingsTextBinding::ServerUrl) {
            qobject_cast<adqt::widgets::AdLineEdit*>(editor)->setPlaceholderText(
                SnowShotApiClient::configuredBaseUrl());
        }
#endif
        sync();
    }

    SettingsFormField& q;
    const SettingsFieldDescriptor& descriptor;
    SettingsRuntimeSession& session;
    fields::FormField* field = nullptr;
    QWidget* editor = nullptr;
    adqt::widgets::AdRadioButtonGroup* radioGroup = nullptr;
    QVariant lastDraft;
    bool hasState = false;
    bool fontOptionsLoaded = false;
    bool fontModelInitialized = false;
    QVector<adqt::widgets::AdSelect::Option> presentedFontOptions;
    QString optionsError;
    bool optionsLoading = false;
};

bool SettingsFormField::supports(const SettingsFieldDescriptor& descriptor) {
    return descriptor.definition != nullptr && descriptor.kind >= SettingsFieldKind::Select &&
           descriptor.kind <= SettingsFieldKind::Text;
}

SettingsFormField* SettingsFormField::create(const SettingsFieldDescriptor& descriptor,
                                             SettingsRuntimeSession& session,
                                             fields::Options options) {
    if (!supports(descriptor))
        return nullptr;
    return new SettingsFormField(descriptor, session, options);
}

SettingsFormField::SettingsFormField(const SettingsFieldDescriptor& descriptor,
                                     SettingsRuntimeSession& session,
                                     const fields::Options& options)
    : m_impl(std::make_unique<Impl>(*this, descriptor, session, options)) {
    viewWidget()->installEventFilter(this);
}

SettingsFormField::~SettingsFormField() = default;

const SettingsFieldDescriptor& SettingsFormField::descriptor() const {
    return m_impl->descriptor;
}

QWidget* SettingsFormField::viewWidget() const {
    return m_impl->field->viewWidget();
}

QWidget* SettingsFormField::editor() const {
    return m_impl->editor;
}

QWidget* SettingsFormField::focusTarget() const {
    return m_impl->field->focusWidget();
}

fields::FormField* SettingsFormField::controller() const {
    return m_impl->field;
}

void SettingsFormField::sync(const SettingsFieldState* state) {
    m_impl->sync(state);
}

void SettingsFormField::retranslateUi() {
    m_impl->retranslateUi();
}

bool SettingsFormField::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::LanguageChange && m_impl && watched == viewWidget())
        retranslateUi();
    return QObject::eventFilter(watched, event);
}

} // namespace snow_shot::presentation::settings
