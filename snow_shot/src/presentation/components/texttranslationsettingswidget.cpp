#include "snow_shot/presentation/components/texttranslationsettingswidget.h"
#include "snow_shot/presentation/components/formfields.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "theme/theme_manager.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_password_edit.h"
#include "widgets/modal.h"
#include "widgets/input_number.h"
#include "widgets/tag.h"
#include "widgets/combo_box.h"
#include <QSignalBlocker>
#include <QEvent>
#include <QApplication>
#include "antd_icons.h"
#include <QLabel>
#include <QGridLayout>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

using namespace adqt::widgets;
using namespace snow_shot;
namespace settings = snow_shot::presentation::settings;
namespace fields = snow_shot::presentation::components::form_fields;

namespace {
class ConfigurationRow final : public QWidget {
  public:
    ConfigurationRow(const presentation::styles::ThemeColorScheme& scheme, QWidget* parent)
        : QWidget(parent), m_scheme(scheme) {
        connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                this, [this] { update(); });
    }

    void applyTheme(const presentation::styles::ThemeColorScheme& scheme) {
        m_scheme = scheme;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal borderWidth = m_scheme.metricAlias.lineWidth;
        const qreal inset = borderWidth / 2.0;
        painter.setBrush(
            presentation::styles::mainWindowBackgroundColor(this, m_scheme.map.colorBgContainer));
        painter.setPen(borderWidth > 0 ? QPen(m_scheme.map.colorBorderSecondary, borderWidth)
                                       : Qt::NoPen);
        painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset),
                                m_scheme.metricAlias.borderRadius,
                                m_scheme.metricAlias.borderRadius);
    }

  private:
    presentation::styles::ThemeColorScheme m_scheme;
};

class ConfigurationNameLabel final : public QLabel {
  public:
    explicit ConfigurationNameLabel(const QString& name, QWidget* parent) : QLabel(name, parent) {
        setTextFormat(Qt::PlainText);
        setToolTip(name);
        setAccessibleName(name);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(), Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};
} // namespace

TextTranslationSettingsWidget::TextTranslationSettingsWidget(
    settings::SettingsRuntimeSession& session, QWidget* parent)
    : SettingsCustomWidget(parent), m_session(session) {
    setObjectName(QStringLiteral("textTranslationConfigurationsSettings"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_title = new QLabel(this);
    layout->addWidget(m_title);
    m_error = new AdAlert(this);
    m_error->setSeverity(AdAlert::Severity::Error);
    m_error->hide();
    layout->addWidget(m_error);
    m_rows = new QVBoxLayout;
    m_rows->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(m_rows);
    m_add = new AdButton(this);
    m_add->setObjectName(QStringLiteral("textTranslationAdd"));
    m_add->setIconRef(adqt::icons::antd::outlined::Plus());
    m_add->setButtonStyle(AdButton::ButtonStyle::Dashed);
    m_add->setShape(AdButton::Shape::Rounded);
    m_add->setCursor(Qt::PointingHandCursor);
    m_add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_add->setAccentRole(AdButton::AccentRole::Primary);
    layout->addWidget(m_add);
    setFocusProxy(m_add);
    setFocusPolicy(Qt::StrongFocus);
    connect(m_add, &AdButton::clicked, this, [this]() { openEditor(); });
    connect(&session, &settings::SettingsRuntimeSession::fieldChanged, this,
            [this](const QString& id, const settings::SettingsFieldState&) {
                if (id == QStringLiteral("api.text-translation")) {
                    QTimer::singleShot(0, this, [this]() { rebuild(); });
                }
            });
    retranslateUi();
}

void TextTranslationSettingsWidget::applyTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    m_scheme = scheme;
    layout()->setSpacing(scheme.metricAlias.margin);
    m_rows->setSpacing(scheme.metricAlias.marginXS);
    m_add->setFixedHeight(scheme.metricAlias.controlHeight);
    QFont contentFont = font();
    contentFont.setPixelSize(scheme.metricAlias.fontSize);
    setFont(contentFont);
    QFont titleFont = contentFont;
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    QPalette colors = palette();
    colors.setColor(QPalette::WindowText, scheme.map.colorText);
    setPalette(colors);
    for (int index = 0; index < m_rows->count(); ++index) {
        if (auto* row = dynamic_cast<ConfigurationRow*>(m_rows->itemAt(index)->widget())) {
            row->applyTheme(scheme);
        }
    }
    update();
}

void TextTranslationSettingsWidget::rebuild() {
    const QString focusedName =
        QApplication::focusWidget() != nullptr && isAncestorOf(QApplication::focusWidget())
            ? QApplication::focusWidget()->objectName()
            : QString();
    while (auto* item = m_rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto models = m_session.textTranslationConfigurations();
    if (models.isEmpty()) {
        auto* empty = new QLabel(tr("No translation configurations added"), this);
        empty->setObjectName(QStringLiteral("textTranslationConfigurationsEmpty"));
        empty->setMargin(12);
        m_rows->addWidget(empty);
    }
    for (const auto& model : models) {
        auto* row = new ConfigurationRow(m_scheme, this);
        row->setObjectName(QStringLiteral("textTranslationRow:") + model.id);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(4);
        layout->addWidget(new ConfigurationNameLabel(model.name, row));
        auto* tag = new AdTag(model.provider == QStringLiteral("deepl")   ? tr("DeepL")
                              : model.provider == QStringLiteral("baidu") ? tr("Baidu")
                                                                          : tr("Youdao"),
                              row);
        layout->addWidget(tag);
        layout->addStretch(1);
        const QStringList labels{tr("Edit"), tr("Delete"), tr("Copy")};
        const QStringList actions{QStringLiteral("edit"), QStringLiteral("delete"),
                                  QStringLiteral("copy")};
        const std::array icons{adqt::icons::antd::outlined::Edit(),
                               adqt::icons::antd::outlined::IconDelete(),
                               adqt::icons::antd::outlined::Copy()};
        for (int i = 0; i < 3; ++i) {
            auto* button = new AdButton(row);
            button->setIconRef(icons[static_cast<size_t>(i)]);
            button->setSizeClass(AdButton::SizeClass::Small);
            button->setToolTip(labels[i]);
            button->setObjectName(actions[i] + u':' + model.id);
            button->setAccessibleName(tr("%1 configuration %2").arg(labels[i], model.name));
            button->setButtonStyle(AdButton::ButtonStyle::Text);
            button->setAccentRole(i == 1 ? AdButton::AccentRole::Danger
                                         : AdButton::AccentRole::Primary);
            layout->addWidget(button);
            connect(button, &AdButton::clicked, this, [this, id = model.id, i]() {
                if (i == 0) {
                    openEditor(id);
                } else if (i == 1) {
                    deleteModel(id);
                } else {
                    copyModel(id);
                }
            });
        }
        m_rows->addWidget(row);
    }
    if (!focusedName.isEmpty()) {
        if (auto* button = findChild<AdButton*>(focusedName)) {
            button->setFocus();
        }
    }
}

bool TextTranslationSettingsWidget::save(const TextTranslationConfigurations& models) {
    const bool success = m_session.applyTextTranslationConfigurations(models);
    m_error->setVisible(!success);
    if (!success) {
        m_error->setText(tr("Unable to save configurations. Check that configuration storage is "
                            "writable and try again."));
    }
    return success;
}

void TextTranslationSettingsWidget::copyModel(const QString& id) {
    auto models = m_session.textTranslationConfigurations();
    const auto it = std::find_if(models.cbegin(), models.cend(),
                                 [&id](const auto& model) { return model.id == id; });
    if (it == models.cend()) {
        return;
    }
    auto copy = *it;
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    int number = 1;
    do {
        copy.name = number == 1 ? tr("%1 (Copy)").arg(it->name)
                                : tr("%1 (Copy %2)").arg(it->name).arg(number);
        ++number;
    } while (std::any_of(models.cbegin(), models.cend(), [&copy](const auto& model) {
        return model.name.compare(copy.name, Qt::CaseInsensitive) == 0;
    }));
    models.push_back(copy);
    save(models);
}

void TextTranslationSettingsWidget::deleteModel(const QString& id) {
    if (m_deleteModal != nullptr) {
        return;
    }
    m_deleteId = id;
    auto* modal = new AdModal(this);
    m_deleteModal = modal;
    modal->setObjectName(QStringLiteral("textTranslationDeleteModal"));
    modal->setAcceptAccentRole(AdButton::AccentRole::Danger);
    modal->setOwnerWindow(window());
    modal->setCentered(true);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    modal->setStandardButtons(AdModal::StandardButtons(AdModal::StandardButton::Ok) |
                              AdModal::StandardButton::Cancel);
    translateModal();
    connect(modal, &AdModal::closeRequested, this, [this, modal, id](AdModal::CloseReason reason) {
        if (reason != AdModal::CloseReason::OkAction) {
            modal->reject();
            return;
        }
        auto models = m_session.textTranslationConfigurations();
        models.removeIf([&id](const auto& model) { return model.id == id; });
        if (save(models)) {
            modal->accept();
        } else {
            modal->setText(m_error->text());
        }
    });
    connect(modal, &AdModal::finished, this, [this, modal](AdModal::DialogCode) {
        m_deleteModal = nullptr;
        modal->deleteLater();
        m_add->setFocus();
    });
    modal->open();
}

void TextTranslationSettingsWidget::openEditor(const QString& id) {
    if (m_modal != nullptr) {
        return;
    }
    TextTranslationConfiguration value;
    m_editing = !id.isEmpty();
    if (m_editing) {
        const auto models = m_session.textTranslationConfigurations();
        const auto it = std::find_if(models.cbegin(), models.cend(),
                                     [&id](const auto& model) { return model.id == id; });
        if (it == models.cend()) {
            return;
        }
        value = *it;
    } else {
        value.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    m_editId = value.id;
    auto* modal = new AdModal(this);
    m_modal = modal;
    modal->setObjectName(QStringLiteral("textTranslationEditor"));
    modal->setOwnerWindow(window());
    modal->setCentered(true);
    modal->setPreferredWidth(760);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    modal->setStandardButtons(AdModal::StandardButtons(AdModal::StandardButton::Ok) |
                              AdModal::StandardButton::Cancel);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(m_scheme.metricAlias.marginSM);
    auto* form = new AdForm(body);
    fields::configureForm(form);
    auto* grid = new QGridLayout;
    fields::configureTwoColumnGrid(grid);
    connect(&presentation::styles::ThemeManager::instance(),
            &presentation::styles::ThemeManager::themeChanged, body,
            [layout, grid](const presentation::styles::ThemeColorScheme& scheme) {
                layout->setSpacing(scheme.metricAlias.marginSM);
                grid->setHorizontalSpacing(scheme.metricAlias.marginLG);
            });
    const QStringList names{QStringLiteral("modelName"), QStringLiteral("apiUrl"),
                            QStringLiteral("apiKey"), QStringLiteral("applicationId")};
    const QStringList values{value.name, value.endpoint, value.apiKey, value.applicationId};
    for (size_t i = 0; i < m_inputs.size(); ++i) {
        fields::Options options;
        options.parent = form;
        options.form = form;
        options.commitPolicy = fields::CommitPolicy::Explicit;
        options.required = i < 2;
        const fields::Metadata metadata{names[static_cast<qsizetype>(i)]};
        if (i == 2) {
            const auto field = fields::password(metadata, options);
            m_inputs[i] = field.editor;
            m_formFields[i] = field.field;
        } else {
            const auto field = fields::text(metadata, options);
            m_inputs[i] = field.editor;
            m_formFields[i] = field.field;
        }
        m_fields[i] = m_formFields[i]->item();
        m_formFields[i]->syncValue(values[static_cast<qsizetype>(i)]);
    }
    fields::Options options;
    options.parent = form;
    options.form = form;
    options.commitPolicy = fields::CommitPolicy::Explicit;
    options.popupInModal = true;
    const auto providerField =
        fields::comboBox({QStringLiteral("translationProvider")}, {}, options);
    m_provider = providerField.editor;
    m_formFields[4] = providerField.field;
    m_fields[4] = providerField.item();
    const auto concurrencyField =
        fields::number({QStringLiteral("translationConcurrency")}, {1, 16, 1, 0}, options);
    m_concurrency = concurrencyField.editor;
    m_formFields[5] = concurrencyField.field;
    m_fields[5] = concurrencyField.item();
    concurrencyField.field->syncValue(value.concurrency);
    for (size_t i = 0; i < m_formFields.size(); ++i) {
        auto* view = m_formFields[i]->viewWidget();
        form->layout()->removeWidget(view);
        grid->addWidget(view, static_cast<int>(i / 2), static_cast<int>(i % 2), Qt::AlignTop);
    }
    static_cast<QVBoxLayout*>(form->layout())->addLayout(grid);
    translateModal();
    providerField.field->syncValue(value.provider);
    connect(m_provider, &AdComboBox::currentValueChanged, this, [this]() { translateModal(); });
    layout->addWidget(form);
    m_modalError = new AdAlert(body);
    m_modalError->setSeverity(AdAlert::Severity::Error);
    m_modalError->hide();
    layout->addWidget(m_modalError);
    modal->setContentWidget(body);
    translateModal();
    form->setInitialValues(form->values());
    form->resetFields();
    connect(modal, &AdModal::closeRequested, this, [this, modal](AdModal::CloseReason reason) {
        if (reason == AdModal::CloseReason::OkAction) {
            submitEditor();
        } else {
            modal->reject();
        }
    });
    connect(modal, &AdModal::finished, this, [this, modal](AdModal::DialogCode) {
        m_modal = nullptr;
        m_modalError = nullptr;
        modal->deleteLater();
        QTimer::singleShot(0, this, [this]() {
            auto* button =
                m_editing ? findChild<AdButton*>(QStringLiteral("edit:") + m_editId) : m_add;
            (button != nullptr ? button : m_add)->setFocus();
        });
    });
    modal->setInitialFocusWidget(m_inputs[0]);
    // Resolve nested form size hints before the centered modal gets its first paint.
    body->ensurePolished();
    const auto children = body->findChildren<QWidget*>();
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        (*it)->ensurePolished();
        if ((*it)->layout() != nullptr) {
            (*it)->layout()->activate();
        }
    }
    body->layout()->activate();
    modal->open();
}

void TextTranslationSettingsWidget::submitEditor(bool saveChanges) {
    TextTranslationConfiguration value{m_editId,
                                       m_inputs[0]->text().trimmed(),
                                       m_provider->currentValue().toString(),
                                       m_inputs[1]->text().trimmed(),
                                       m_inputs[2]->text().trimmed(),
                                       m_inputs[3]->text().trimmed(),
                                       static_cast<int>(m_concurrency->value())};
    auto models = m_session.textTranslationConfigurations();
    std::array<QString, 4> errors;
    if (value.name.isEmpty()) {
        errors[0] = tr("Enter a configuration name.");
    } else if (std::any_of(models.cbegin(), models.cend(), [&value](const auto& model) {
                   return model.id != value.id &&
                          model.name.compare(value.name, Qt::CaseInsensitive) == 0;
               })) {
        errors[0] = tr("A configuration with this name already exists.");
    }
    if (!validTranslationEndpoint(value.endpoint)) {
        errors[1] =
            tr("Enter a full HTTP or HTTPS endpoint without embedded credentials or a fragment.");
    }
    if (value.apiKey.contains(u'\r') || value.apiKey.contains(u'\n')) {
        errors[2] = tr("The API key must not contain line breaks.");
    }
    QWidget* firstInvalid = nullptr;
    for (size_t i = 0; i < errors.size(); ++i) {
        m_formFields[i]->setFeedback(errors[i].isEmpty() ? QStringList{} : QStringList{errors[i]});
        if (!errors[i].isEmpty() && firstInvalid == nullptr) {
            firstInvalid = m_inputs[i];
        }
    }
    if (firstInvalid != nullptr) {
        firstInvalid->setFocus();
        return;
    }
    if (!saveChanges) {
        return;
    }
    const auto it = std::find_if(models.begin(), models.end(),
                                 [&value](const auto& model) { return model.id == value.id; });
    if (m_editing && it == models.end()) {
        m_modalError->setProperty("deletedModel", true);
        m_modalError->setText(
            tr("This configuration was deleted. Close this form and create a new configuration."));
        m_modalError->show();
        return;
    }
    if (it != models.end()) {
        *it = value;
    } else {
        models.push_back(value);
    }
    if (save(models)) {
        for (auto* field : m_formFields)
            field->notifyCommitted();
        m_modal->accept();
    } else {
        m_modalError->setProperty("deletedModel", false);
        m_modalError->setText(m_error->text());
        m_modalError->show();
    }
}

void TextTranslationSettingsWidget::translateModal() {
    if (m_modal != nullptr) {
        m_modal->setWindowTitle(m_editing ? tr("Edit Configuration") : tr("Add Configuration"));
        m_modal->setAcceptText(tr("Save"));
        m_modal->setRejectText(tr("Cancel"));
        if (m_modalError != nullptr && !m_modalError->isHidden()) {
            m_modalError->setText(m_modalError->property("deletedModel").toBool()
                                      ? tr("This configuration was deleted. Close this form and "
                                           "create a new configuration.")
                                      : tr("Unable to save configurations. Check that "
                                           "configuration storage is writable and "
                                           "try again."));
        }
        const QString provider = m_provider->currentValue().toString();
        const bool deepL = provider.isEmpty() || provider == QStringLiteral("deepl");
        const char* labels[] = {
            QT_TRANSLATE_NOOP("TextTranslationSettingsWidget", "Configuration Name"),
            QT_TRANSLATE_NOOP("TextTranslationSettingsWidget", "API URL"),
            deepL ? QT_TRANSLATE_NOOP("TextTranslationSettingsWidget", "API Key")
                  : QT_TRANSLATE_NOOP("TextTranslationSettingsWidget", "Application Secret"),
            QT_TRANSLATE_NOOP("TextTranslationSettingsWidget", "Application ID"),
            QT_TRANSLATE_NOOP("TextTranslationSettingsWidget", "Service Format"),
            QT_TRANSLATE_NOOP("TextTranslationSettingsWidget", "Concurrency")};
        const char* descriptions[] = {
            nullptr,
            QT_TRANSLATE_NOOP("TextTranslationSettingsWidget",
                              "The full translation endpoint. Its path and query are used as "
                              "entered."),
            QT_TRANSLATE_NOOP("TextTranslationSettingsWidget",
                              "Optional for servers that do not require authentication."),
            nullptr,
            nullptr,
            QT_TRANSLATE_NOOP("TextTranslationSettingsWidget",
                              "Maximum simultaneous requests for this configuration across "
                              "translation jobs (1-16).")};
        for (size_t i = 0; i < m_fields.size(); ++i) {
            auto metadata = m_formFields[i]->metadata();
            metadata.label = {"TextTranslationSettingsWidget", labels[i]};
            metadata.description = {"TextTranslationSettingsWidget", descriptions[i]};
            m_formFields[i]->setMetadata(metadata);
        }
        m_formFields[3]->setFieldVisible(!deepL);
        const QSignalBlocker blocker(m_provider);
        QVector<AdComboBox::Option> options;
        const QStringList ids{QStringLiteral("deepl"), QStringLiteral("baidu"),
                              QStringLiteral("youdao")};
        const QStringList names{tr("DeepL"), tr("Baidu"), tr("Youdao")};
        for (int i = 0; i < ids.size(); ++i) {
            AdComboBox::Option option;
            option.value = ids[i];
            option.label = names[i];
            options.append(option);
        }
        m_formFields[4]->synchronize([&] {
            m_provider->setOptions(options);
            m_provider->setCurrentValue(deepL ? QStringLiteral("deepl") : provider);
        });
    }
    if (m_deleteModal != nullptr) {
        m_deleteModal->setWindowTitle(tr("Delete Configuration"));
        m_deleteModal->setAcceptText(tr("Delete"));
        m_deleteModal->setRejectText(tr("Cancel"));
        for (const auto& model : m_session.textTranslationConfigurations()) {
            if (model.id == m_deleteId) {
                m_deleteModal->setText(tr("Delete configuration \"%1\"? If selected, another "
                                          "available service will be used.")
                                           .arg(model.name));
            }
        }
    }
}

void TextTranslationSettingsWidget::retranslateUi() {
    m_error->setText(tr("Unable to save configurations. Check that configuration storage is "
                        "writable and try again."));
    m_add->setText(tr("Add Configuration"));
    m_title->setText(QCoreApplication::translate("SettingsCatalog", "Translation Configurations"));
    rebuild();
    translateModal();
    if (m_modal != nullptr &&
        std::any_of(m_fields.cbegin(), m_fields.cend(),
                    [](const auto* field) { return !field->errorMessages().isEmpty(); })) {
        submitEditor(false);
    }
}

void TextTranslationSettingsWidget::changeEvent(QEvent* event) {
    SettingsCustomWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}
