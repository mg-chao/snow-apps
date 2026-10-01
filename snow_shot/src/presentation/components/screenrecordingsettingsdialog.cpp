#include "snow_shot/presentation/components/screenrecordingsettingsdialog.h"
#include "snow_shot/presentation/components/screenrecordingmodal.h"

#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/configurationschema.h"
#include "widgets/form.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/slider.h"
#include "widgets/switch.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <utility>

namespace snow_shot::presentation {
namespace {
namespace settings = snow_shot::presentation::settings;
using namespace adqt::widgets;

settings::SettingsRegistry recordingRegistry() {
    QVector<settings::SettingsPageDefinition> pages;
    for (const auto& page : settings::builtInSettingsRegistry().pages()) {
        auto recordingPage = page;
        recordingPage.sections.clear();
        for (const auto& section : page.sections) {
            if (section.reset == settings::SettingsSectionReset::ScreenRecording ||
                section.reset == settings::SettingsSectionReset::ScreenRecordingCapture) {
                recordingPage.sections.append(section);
            }
        }
        if (!recordingPage.sections.isEmpty())
            pages.append(std::move(recordingPage));
    }
    const settings::SettingsLocation initial{
        pages.first().id, pages.first().sections.first().id, {}};
    return settings::SettingsRegistry(settings::SettingsCatalog(std::move(pages), {}, initial));
}

class ScreenRecordingSettingsBody final : public QWidget {
  public:
    explicit ScreenRecordingSettingsBody(AdModal& modal)
        : m_modal(modal), m_registry(recordingRegistry()), m_backend(m_shortcuts),
          m_session(m_registry, m_backend) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        m_form = new AdForm(this);
        m_form->setObjectName(QStringLiteral("screenRecordingSettingsForm"));
        m_form->setFormLayout(AdForm::FormLayout::Vertical);
        m_form->setLabelAlign(AdForm::LabelAlign::Left);
        m_form->setLabelWrap(true);
        m_form->setRequiredMark(AdForm::RequiredMark::Hidden);
        m_form->setColon(false);
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(24);
        grid->setVerticalSpacing(0);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);

        for (const auto& descriptor : m_registry.fields()) {
            Field field;
            field.definition = descriptor.definition;
            const auto& definition = *field.definition;
            QWidget* editor = nullptr;
            if (std::holds_alternative<settings::SettingsSelectDefinition>(definition.payload)) {
                field.select = new AdSelect(m_form);
                field.select->setPopupLayerMode(AdSelect::PopupLayerMode::QtTool);
                editor = field.select;
                connect(field.select, &AdSelect::currentValueChanged, this,
                        [this, id = definition.id](const QVariant& value) {
                            m_session.submitDraft(id, value);
                        });
            } else if (std::holds_alternative<settings::SettingsSwitchDefinition>(
                           definition.payload)) {
                auto* row = new QWidget(m_form);
                auto* rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                field.toggle = new AdSwitch(row);
                rowLayout->addWidget(field.toggle);
                rowLayout->addStretch();
                editor = row;
                connect(
                    field.toggle, &AdSwitch::toggled, this,
                    [this, id = definition.id](bool value) { m_session.submitDraft(id, value); });
            } else if (std::holds_alternative<settings::SettingsSliderDefinition>(
                           definition.payload)) {
                auto* row = new QWidget(m_form);
                auto* rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                field.slider = new AdSlider(row);
                const auto* schema =
                    storage::ConfigurationSchema::entry(definition.configurationKey);
                Q_ASSERT(schema != nullptr && schema->integerRange.has_value());
                field.slider->setRange(schema->integerRange->minimum,
                                       schema->integerRange->maximum);
                field.slider->setSingleStep(schema->integerRange->step);
                field.slider->setTooltipEnabled(true);
                field.valueLabel = new QLabel(row);
                field.valueLabel->setMinimumWidth(42);
                field.valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                rowLayout->addWidget(field.slider, 1);
                rowLayout->addWidget(field.valueLabel);
                editor = row;
                connect(field.slider, &AdSlider::valueChanged, this,
                        [this, id = definition.id](double value) {
                            m_session.submitDraft(id, qRound(value));
                        });
            }
            Q_ASSERT(editor != nullptr);
            if (editor == nullptr)
                continue;
            field.control = field.select   ? static_cast<QWidget*>(field.select)
                            : field.toggle ? static_cast<QWidget*>(field.toggle)
                                           : static_cast<QWidget*>(field.slider);
            field.control->setObjectName(definition.id);
            field.item = m_form->addField({}, editor, definition.id);
            m_fields.append(field);
        }
        // Adding a field rebuilds AdForm's default layout. Arrange the complete
        // set only after registration so those rebuilds cannot undo the grid.
        for (int index = 0; index < m_fields.size(); ++index) {
            auto* item = m_fields[index].item;
            m_form->layout()->removeWidget(item);
            grid->addWidget(item, index / 2, index % 2, Qt::AlignTop);
        }
        static_cast<QVBoxLayout*>(m_form->layout())->addLayout(grid);
        m_form->setAutoFillBackground(false);
        layout->addWidget(m_form);
        connect(&m_session, &settings::SettingsRuntimeSession::fieldChanged, this,
                [this](const QString& id, const settings::SettingsFieldState&) {
                    for (auto& field : m_fields) {
                        if (field.definition->id == id)
                            syncField(field);
                    }
                });
        retranslate();
        modal.setInitialFocusWidget(m_fields.first().control);
    }

  protected:
    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            retranslate();
    }

  private:
    struct Field {
        const settings::SettingsItemDefinition* definition = nullptr;
        AdFormItem* item = nullptr;
        QWidget* control = nullptr;
        AdSelect* select = nullptr;
        AdSwitch* toggle = nullptr;
        AdSlider* slider = nullptr;
        QLabel* valueLabel = nullptr;
    };

    void syncField(Field& field) {
        const auto state = m_session.state(field.definition->id);
        const QSignalBlocker guard(field.control);
        if (field.select)
            field.select->setCurrentValue(state.draftValue);
        if (field.toggle)
            field.toggle->setChecked(state.draftValue.toBool());
        if (field.slider) {
            field.slider->setValue(state.draftValue.toInt());
            const auto& slider =
                std::get<settings::SettingsSliderDefinition>(field.definition->payload);
            field.valueLabel->setText(QStringLiteral("%1%2")
                                          .arg(state.draftValue.toInt())
                                          .arg(slider.suffix.translated()));
        }
        field.control->setEnabled(state.enabled);
        field.item->setHelpText(state.error);
        field.item->setValidateStatus(state.error.isEmpty() ? AdFormItem::ValidateStatus::None
                                                            : AdFormItem::ValidateStatus::Error);
    }

    void retranslate() {
        m_modal.setWindowTitle(
            QCoreApplication::translate("ScreenRecordingSettingsDialog", "Recording settings"));
        m_modal.setAcceptText(QCoreApplication::translate("ScreenRecordingSettingsDialog", "Done"));
        for (auto& field : m_fields) {
            const auto& definition = *field.definition;
            field.item->setLabel(definition.title.translated());
            field.control->setToolTip(definition.description.translated());
            field.control->setAccessibleName(definition.title.translated());
            field.control->setAccessibleDescription(definition.description.translated());
            if (field.select) {
                const QSignalBlocker guard(field.select);
                QVector<AdSelect::Option> options;
                const auto& select =
                    std::get<settings::SettingsSelectDefinition>(definition.payload);
                for (const auto& option : select.options)
                    options.append({option.value, option.label.translated()});
                field.select->setOptions(options);
            }
            syncField(field);
        }
    }

    AdModal& m_modal;
    settings::SettingsRegistry m_registry;
    // Recording fields do not register shortcuts. The built-in backend shares the
    // same validation, storage notifications and write state as the settings page.
    GlobalShortcutManager m_shortcuts;
    settings::BuiltInSettingsBackend m_backend;
    settings::SettingsRuntimeSession m_session;
    AdForm* m_form = nullptr;
    QVector<Field> m_fields;
};
} // namespace

adqt::widgets::AdModal* createScreenRecordingSettingsDialog(QWidget* owner, QObject* parent) {
    using namespace adqt::widgets;
    auto* modal = new AdModal(parent);
    modal->setObjectName(QStringLiteral("screenRecordingSettingsModal"));
    configureScreenRecordingModal(*modal, owner);
    modal->setPreferredWidth(660);
    modal->setStandardButtons(AdModal::StandardButton::Ok);
    modal->setContentWidget(new ScreenRecordingSettingsBody(*modal));
    QObject::connect(modal, &AdModal::finished, modal, &QObject::deleteLater);
    return modal;
}
} // namespace snow_shot::presentation
