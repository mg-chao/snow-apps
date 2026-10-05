#include "snow_shot/presentation/components/clouduploadsettingswidget.h"
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

CloudUploadSettingsWidget::CloudUploadSettingsWidget(settings::SettingsRuntimeSession& session,
                                                     QWidget* parent)
    : SettingsCustomWidget(parent), m_session(session) {
    setObjectName(QStringLiteral("cloudUploadConfigurationsSettings"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    fields::Options options;
    options.parent = this;
    options.presentation = fields::Presentation::SettingsRow;
    auto defaultField =
        fields::comboBox({QStringLiteral("cloudUploadDefault"),
                          {"CloudUploadSettingsWidget",
                           QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Default destination")},
                          {"CloudUploadSettingsWidget",
                           QT_TRANSLATE_NOOP("CloudUploadSettingsWidget",
                                             "Choose the configuration used for cloud uploads")}},
                         {}, options);
    m_default = defaultField.field;
    layout->addWidget(m_default->viewWidget());
    connect(defaultField.editor, &AdComboBox::currentValueChanged, this,
            [this](const QVariant& value) {
                if (m_default->isSynchronizing())
                    return;
                auto values = m_session.cloudUploadSettings();
                values.defaultId = value.toString();
                save(values);
            });
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("cloudUploadConfigurationsTitle"));
    layout->addWidget(m_title);
    m_error = new AdAlert(this);
    m_error->setSeverity(AdAlert::Severity::Error);
    m_error->hide();
    layout->addWidget(m_error);
    m_rows = new QVBoxLayout;
    m_rows->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(m_rows);
    m_add = new AdButton(this);
    m_add->setObjectName(QStringLiteral("cloudUploadAdd"));
    m_add->setIconRef(adqt::icons::antd::outlined::Plus());
    m_add->setButtonStyle(AdButton::ButtonStyle::Dashed);
    m_add->setShape(AdButton::Shape::Rounded);
    m_add->setAccentRole(AdButton::AccentRole::Primary);
    m_add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(m_add);
    setFocusProxy(m_add);
    setFocusPolicy(Qt::StrongFocus);
    connect(m_add, &AdButton::clicked, this, [this] { openEditor(); });
    connect(&session, &settings::SettingsRuntimeSession::fieldChanged, this,
            [this](const QString& id, const settings::SettingsFieldState&) {
                if (id == QStringLiteral("screenshot-output.cloud-upload"))
                    QTimer::singleShot(0, this, [this] { rebuild(); });
            });
    retranslateUi();
}
void CloudUploadSettingsWidget::applyTheme(const presentation::styles::ThemeColorScheme& scheme) {
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
    m_default->applyTheme(scheme);
    for (int i = 0; i < m_rows->count(); ++i)
        if (auto* row = dynamic_cast<ConfigurationRow*>(m_rows->itemAt(i)->widget()))
            row->applyTheme(scheme);
}
void CloudUploadSettingsWidget::rebuild() {
    const QString focused = QApplication::focusWidget() && isAncestorOf(QApplication::focusWidget())
                                ? QApplication::focusWidget()->objectName()
                                : QString();
    while (auto* item = m_rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto values = m_session.cloudUploadSettings();
    QVector<fields::Choice> choices{{QString(), {}, tr("None")}};
    for (const auto& value : values.configurations)
        choices.push_back({value.id, {}, value.name});
    m_default->synchronize([&] { m_default->setChoices(choices); });
    m_default->syncValue(values.defaultId);
    if (values.configurations.isEmpty()) {
        auto* label = new QLabel(tr("No cloud upload configurations added"), this);
        label->setObjectName(QStringLiteral("cloudUploadConfigurationsEmpty"));
        label->setMargin(12);
        m_rows->addWidget(label);
    }
    for (const auto& value : values.configurations) {
        auto* row = new ConfigurationRow(m_scheme, this);
        row->setObjectName(QStringLiteral("cloudUploadRow:") + value.id);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 8, 12, 8);
        rowLayout->addWidget(new ConfigurationNameLabel(value.name, row));
        rowLayout->addWidget(new AdTag(tr("S3"), row));
        rowLayout->addStretch();
        const QStringList labels{tr("Edit"), tr("Delete"), tr("Copy")};
        const QStringList actions{QStringLiteral("edit"), QStringLiteral("delete"),
                                  QStringLiteral("copy")};
        const std::array icons{adqt::icons::antd::outlined::Edit(),
                               adqt::icons::antd::outlined::IconDelete(),
                               adqt::icons::antd::outlined::Copy()};
        for (int i = 0; i < 3; ++i) {
            auto* button = new AdButton(row);
            button->setObjectName(actions[i] + u':' + value.id);
            button->setIconRef(icons[static_cast<size_t>(i)]);
            button->setSizeClass(AdButton::SizeClass::Small);
            button->setButtonStyle(AdButton::ButtonStyle::Text);
            button->setAccentRole(i == 1 ? AdButton::AccentRole::Danger
                                         : AdButton::AccentRole::Primary);
            button->setToolTip(labels[i]);
            button->setAccessibleName(tr("%1 configuration %2").arg(labels[i], value.name));
            rowLayout->addWidget(button);
            connect(button, &AdButton::clicked, this, [this, id = value.id, i] {
                if (i == 0)
                    openEditor(id);
                else if (i == 1)
                    deleteConfiguration(id);
                else
                    copyConfiguration(id);
            });
        }
        m_rows->addWidget(row);
    }
    if (!focused.isEmpty())
        if (auto* button = findChild<AdButton*>(focused))
            button->setFocus();
}
bool CloudUploadSettingsWidget::save(const CloudUploadSettings& values) {
    const bool success = m_session.applyCloudUploadSettings(values);
    m_error->setVisible(!success);
    if (!success)
        m_error->setText(tr("Unable to save configurations. Check that configuration storage is "
                            "writable and try again."));
    return success;
}
void CloudUploadSettingsWidget::copyConfiguration(const QString& id) {
    auto values = m_session.cloudUploadSettings();
    for (const auto& value : values.configurations) {
        if (value.id != id)
            continue;
        auto copy = value;
        copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        int number = 1;
        do {
            copy.name = number == 1 ? tr("%1 (Copy)").arg(value.name)
                                    : tr("%1 (Copy %2)").arg(value.name).arg(number);
            ++number;
        } while (std::any_of(values.configurations.cbegin(), values.configurations.cend(),
                             [&copy](const auto& other) {
                                 return other.name.compare(copy.name, Qt::CaseInsensitive) == 0;
                             }));
        values.configurations.push_back(copy);
        save(values);
        return;
    }
}
void CloudUploadSettingsWidget::deleteConfiguration(const QString& id) {
    if (m_deleteModal)
        return;
    auto* modal = new AdModal(this);
    m_deleteModal = modal;
    modal->setObjectName(QStringLiteral("cloudUploadDeleteModal"));
    modal->setOwnerWindow(window());
    modal->setCentered(true);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    modal->setAcceptAccentRole(AdButton::AccentRole::Danger);
    modal->setStandardButtons(AdModal::StandardButtons(AdModal::StandardButton::Ok) |
                              AdModal::StandardButton::Cancel);
    retranslateUi();
    connect(modal, &AdModal::closeRequested, this, [this, modal, id](AdModal::CloseReason reason) {
        if (reason != AdModal::CloseReason::OkAction) {
            modal->reject();
            return;
        }
        auto values = m_session.cloudUploadSettings();
        values.configurations.removeIf([&id](const auto& value) { return value.id == id; });
        if (values.defaultId == id)
            values.defaultId.clear();
        if (save(values))
            modal->accept();
        else
            modal->setText(m_error->text());
    });
    connect(modal, &AdModal::finished, this, [this, modal](AdModal::DialogCode) {
        m_deleteModal = nullptr;
        modal->deleteLater();
        m_add->setFocus();
    });
    modal->open();
}
void CloudUploadSettingsWidget::openEditor(const QString& id) {
    m_editorValidationAttempted = false;
    if (m_modal)
        return;
    CloudUploadConfiguration value;
    m_editing = !id.isEmpty();
    if (m_editing) {
        const auto values = m_session.cloudUploadSettings();
        const auto it = std::find_if(values.configurations.cbegin(), values.configurations.cend(),
                                     [&id](const auto& other) { return other.id == id; });
        if (it == values.configurations.cend())
            return;
        value = *it;
    } else
        value.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_editId = value.id;
    auto* modal = new AdModal(this);
    m_modal = modal;
    modal->setObjectName(QStringLiteral("cloudUploadEditor"));
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
    auto* form = new AdForm(body);
    fields::configureForm(form);
    form->setRequiredMark(AdForm::RequiredMark::Visible);
    auto* grid = new QGridLayout;
    fields::configureTwoColumnGrid(grid);
    const QStringList names{QStringLiteral("configurationName"),
                            QStringLiteral("endpoint"),
                            QStringLiteral("region"),
                            QStringLiteral("bucket"),
                            QStringLiteral("accessKeyId"),
                            QStringLiteral("secretAccessKey"),
                            QStringLiteral("sessionToken"),
                            QStringLiteral("keyPrefix"),
                            QStringLiteral("publicBaseUrl")};
    const QStringList contents{value.name,         value.endpoint,    value.region,
                               value.bucket,       value.accessKeyId, value.secretAccessKey,
                               value.sessionToken, value.keyPrefix,   value.publicBaseUrl};
    fields::Options options;
    options.parent = form;
    options.form = form;
    options.commitPolicy = fields::CommitPolicy::Explicit;
    for (size_t i = 0; i < m_fields.size(); ++i) {
        options.required = i < 6;
        const fields::Metadata metadata{names[static_cast<qsizetype>(i)]};
        m_fields[i] = i >= 4 && i <= 6 ? fields::password(metadata, options).field
                                       : fields::text(metadata, options).field;
        m_fields[i]->syncValue(contents[static_cast<qsizetype>(i)]);
    }
    options.required = true;
    options.popupInModal = true;
    m_addressing = fields::comboBox({QStringLiteral("addressingStyle")}, {}, options).field;
    m_protocol = fields::comboBox({QStringLiteral("uploadProtocol")}, {}, options).field;
    m_protocol->setFieldEnabled(false);
    // Register every item before moving them: AdForm rebuilds its layout when adding an item.
    for (size_t i = 0; i < m_fields.size(); ++i) {
        form->layout()->removeWidget(m_fields[i]->viewWidget());
        grid->addWidget(m_fields[i]->viewWidget(), static_cast<int>(i / 2), static_cast<int>(i % 2),
                        Qt::AlignTop);
    }
    form->layout()->removeWidget(m_addressing->viewWidget());
    form->layout()->removeWidget(m_protocol->viewWidget());
    grid->addWidget(m_addressing->viewWidget(), 4, 1, Qt::AlignTop);
    grid->addWidget(m_protocol->viewWidget(), 5, 0, Qt::AlignTop);
    static_cast<QVBoxLayout*>(form->layout())->addLayout(grid);
    layout->addWidget(form);
    m_modalError = new AdAlert(body);
    m_modalError->setSeverity(AdAlert::Severity::Error);
    m_modalError->hide();
    layout->addWidget(m_modalError);
    modal->setContentWidget(body);
    translateEditor();
    m_addressing->syncValue(value.addressingStyle);
    m_protocol->syncValue(value.protocol);
    form->setInitialValues(form->values());
    form->resetFields();
    connect(modal, &AdModal::closeRequested, this, [this, modal](AdModal::CloseReason reason) {
        if (reason == AdModal::CloseReason::OkAction)
            submitEditor();
        else
            modal->reject();
    });
    connect(modal, &AdModal::finished, this, [this, modal](AdModal::DialogCode) {
        m_modal = nullptr;
        m_modalError = nullptr;
        modal->deleteLater();
        QTimer::singleShot(0, this, [this] {
            auto* button =
                m_editing ? findChild<AdButton*>(QStringLiteral("edit:") + m_editId) : m_add;
            (button ? button : m_add)->setFocus();
        });
    });
    modal->setInitialFocusWidget(m_fields[0]->focusWidget());
    body->ensurePolished();
    const auto children = body->findChildren<QWidget*>();
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        (*it)->ensurePolished();
        if ((*it)->layout())
            (*it)->layout()->activate();
    }
    body->layout()->activate();
    modal->open();
}
void CloudUploadSettingsWidget::submitEditor(bool validateOnly) {
    if (!validateOnly)
        m_editorValidationAttempted = true;
    m_modalError->hide();
    auto values = m_session.cloudUploadSettings();
    const auto text = [this](size_t i) { return m_fields[i]->value().toString(); };
    CloudUploadConfiguration value{m_editId,
                                   text(0).trimmed(),
                                   QStringLiteral("s3"),
                                   text(1).trimmed(),
                                   text(2).trimmed(),
                                   text(3).trimmed(),
                                   text(4).trimmed(),
                                   text(5),
                                   text(6),
                                   text(7),
                                   m_addressing->value().toString(),
                                   text(8).trimmed()};
    std::array<QString, 9> errors;
    for (size_t i = 0; i < 6; ++i)
        if (text(i).trimmed().isEmpty())
            errors[i] = tr("This field is required.");
    if (std::any_of(values.configurations.cbegin(), values.configurations.cend(),
                    [&value](const auto& other) {
                        return other.id != value.id &&
                               other.name.compare(value.name, Qt::CaseInsensitive) == 0;
                    }))
        errors[0] = tr("A configuration with this name already exists.");
    if (!validCloudUploadUrl(value.endpoint))
        errors[1] =
            tr("Enter a full HTTP or HTTPS URL without credentials, a query, or a fragment.");
    if (!value.publicBaseUrl.isEmpty() && !validCloudUploadUrl(value.publicBaseUrl))
        errors[8] =
            errors[1].isEmpty()
                ? tr("Enter a full HTTP or HTTPS URL without credentials, a query, or a fragment.")
                : errors[1];
    QWidget* invalid = nullptr;
    for (size_t i = 0; i < errors.size(); ++i) {
        m_fields[i]->setFeedback(errors[i].isEmpty() ? QStringList{} : QStringList{errors[i]});
        if (!errors[i].isEmpty() && !invalid)
            invalid = m_fields[i]->focusWidget();
    }
    if (invalid) {
        if (!validateOnly)
            invalid->setFocus();
        return;
    }
    if (!cloudUploadConfigurationUsable(value)) {
        m_modalError->setText(tr("Check the region, bucket, addressing style, and credentials. "
                                 "Fields must not contain line breaks."));
        m_modalError->show();
        return;
    }
    if (validateOnly)
        return;
    auto it = std::find_if(values.configurations.begin(), values.configurations.end(),
                           [&value](const auto& other) { return other.id == value.id; });
    if (it == values.configurations.end()) {
        const bool first =
            !std::any_of(values.configurations.cbegin(), values.configurations.cend(),
                         cloudUploadConfigurationUsable);
        values.configurations.push_back(value);
        if (first)
            values.defaultId = value.id;
    } else
        *it = value;
    if (save(values))
        m_modal->accept();
    else {
        m_modalError->setText(m_error->text());
        m_modalError->show();
    }
}
void CloudUploadSettingsWidget::translateEditor() {
    if (!m_modal)
        return;
    m_modal->setWindowTitle(m_editing ? tr("Edit cloud upload configuration")
                                      : tr("Add cloud upload configuration"));
    m_modal->setAcceptText(tr("Save"));
    m_modal->setRejectText(tr("Cancel"));
    const char* labels[]{QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Configuration Name"),
                         QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Service Endpoint"),
                         QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Signing Region"),
                         QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Bucket"),
                         QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Access Key ID"),
                         QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Secret Access Key"),
                         QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Session Token"),
                         QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Object Key Prefix"),
                         QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Public / CDN Base URL")};
    for (size_t i = 0; i < m_fields.size(); ++i) {
        auto metadata = m_fields[i]->metadata();
        metadata.label = {"CloudUploadSettingsWidget", labels[i]};
        if (i == 8)
            metadata.description = {
                "CloudUploadSettingsWidget",
                QT_TRANSLATE_NOOP(
                    "CloudUploadSettingsWidget",
                    "Optional bucket-root URL. Link access follows your bucket or CDN policy.")};
        m_fields[i]->setMetadata(metadata);
    }
    auto metadata = m_addressing->metadata();
    metadata.label = {"CloudUploadSettingsWidget",
                      QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Addressing Style")};
    m_addressing->setMetadata(metadata);
    m_addressing->setChoices({{QStringLiteral("path"), {}, tr("Path style")},
                              {QStringLiteral("virtual"), {}, tr("Virtual hosted")}});
    metadata = m_protocol->metadata();
    metadata.label = {"CloudUploadSettingsWidget",
                      QT_TRANSLATE_NOOP("CloudUploadSettingsWidget", "Protocol")};
    m_protocol->setMetadata(metadata);
    m_protocol->setChoices({{QStringLiteral("s3"), {}, tr("S3")}});
}
void CloudUploadSettingsWidget::retranslateUi() {
    const bool storageError =
        m_modalError && !m_modalError->isHidden() && m_modalError->text() == m_error->text();
    m_error->setText(tr("Unable to save configurations. Check that configuration storage is "
                        "writable and try again."));
    m_add->setText(tr("Add configuration"));
    m_title->setText(tr("Upload Configurations"));
    m_default->retranslateUi();
    rebuild();
    translateEditor();
    if (m_modal && m_editorValidationAttempted)
        submitEditor(true);
    if (storageError) {
        m_modalError->setText(m_error->text());
        m_modalError->show();
    }
    if (m_deleteModal) {
        m_deleteModal->setWindowTitle(tr("Delete configuration"));
        m_deleteModal->setText(tr("Delete this cloud upload configuration?"));
        m_deleteModal->setAcceptText(tr("Delete"));
        m_deleteModal->setRejectText(tr("Cancel"));
    }
}
void CloudUploadSettingsWidget::changeEvent(QEvent* event) {
    SettingsCustomWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
}
