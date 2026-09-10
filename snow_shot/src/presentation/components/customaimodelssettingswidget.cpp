#include "snow_shot/presentation/components/customaimodelssettingswidget.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_password_edit.h"
#include "widgets/modal.h"
#include "widgets/switch.h"
#include "widgets/tag.h"
#include "widgets/combo_box.h"
#include "widgets/spin.h"
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <memory>
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

namespace {
class ModelRow final : public QWidget {
  public:
    ModelRow(const presentation::styles::ThemeColorScheme& scheme, QWidget* parent)
        : QWidget(parent), m_scheme(scheme) {}

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal borderWidth = m_scheme.metricAlias.lineWidth;
        const qreal inset = borderWidth / 2.0;
        painter.setBrush(m_scheme.map.colorBgContainer);
        painter.setPen(borderWidth > 0 ? QPen(m_scheme.map.colorBorderSecondary, borderWidth)
                                       : Qt::NoPen);
        painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset),
                                m_scheme.metricAlias.borderRadius,
                                m_scheme.metricAlias.borderRadius);
    }

  private:
    presentation::styles::ThemeColorScheme m_scheme;
};

class ModelNameLabel final : public QLabel {
  public:
    explicit ModelNameLabel(const QString& name, QWidget* parent) : QLabel(name, parent) {
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

CustomAiModelsSettingsWidget::CustomAiModelsSettingsWidget(
    settings::SettingsRuntimeSession& session, QWidget* parent)
    : SettingsCustomWidget(parent), m_session(session) {
    setObjectName(QStringLiteral("customAiModelsSettings"));
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
    m_add->setObjectName(QStringLiteral("customAiModelAdd"));
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
                if (id == QStringLiteral("api.custom-models")) {
                    QTimer::singleShot(0, this, [this]() { rebuild(); });
                }
            });
    retranslateUi();
}

void CustomAiModelsSettingsWidget::applyTheme(
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
    rebuild();
    update();
}

void CustomAiModelsSettingsWidget::rebuild() {
    const QString focusedName =
        QApplication::focusWidget() != nullptr && isAncestorOf(QApplication::focusWidget())
            ? QApplication::focusWidget()->objectName()
            : QString();
    while (auto* item = m_rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto models = m_session.customAiModels();
    if (models.isEmpty()) {
        auto* empty = new QLabel(tr("No custom models configured"), this);
        empty->setObjectName(QStringLiteral("customAiModelsEmpty"));
        empty->setMargin(12);
        m_rows->addWidget(empty);
    }
    for (const auto& model : models) {
        auto* row = new ModelRow(m_scheme, this);
        row->setObjectName(QStringLiteral("customAiModelRow:") + model.id);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(4);
        layout->addWidget(new ModelNameLabel(model.name, row));
        if (model.supportsVision) {
            auto* tag = new AdTag(tr("Vision"), row);
            tag->setObjectName(QStringLiteral("customAiModelVision:") + model.id);
            tag->setColorScheme(AdTag::ColorScheme::Blue);
            layout->addWidget(tag);
        }
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
            button->setAccessibleName(tr("%1 model %2").arg(labels[i], model.name));
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

bool CustomAiModelsSettingsWidget::save(const CustomAiModels& models) {
    const bool success = m_session.applyCustomAiModels(models);
    m_error->setVisible(!success);
    if (!success) {
        m_error->setText(tr(
            "Unable to save models. Check that configuration storage is writable and try again."));
    }
    return success;
}

void CustomAiModelsSettingsWidget::copyModel(const QString& id) {
    auto models = m_session.customAiModels();
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

void CustomAiModelsSettingsWidget::deleteModel(const QString& id) {
    if (m_deleteModal != nullptr) {
        return;
    }
    m_deleteId = id;
    auto* modal = new AdModal(this);
    m_deleteModal = modal;
    modal->setObjectName(QStringLiteral("customAiModelDeleteModal"));
    modal->setAcceptAccentRole(AdButton::AccentRole::Danger);
    modal->setOwnerWindow(window());
    modal->setCentered(true);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    modal->setStandardButtons(AdModal::StandardButton::Ok | AdModal::StandardButton::Cancel);
    translateModal();
    connect(modal, &AdModal::closeRequested, this, [this, modal, id](AdModal::CloseReason reason) {
        if (reason != AdModal::CloseReason::OkAction) {
            modal->reject();
            return;
        }
        auto models = m_session.customAiModels();
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

void CustomAiModelsSettingsWidget::openEditor(const QString& id) {
    if (m_modal != nullptr) {
        return;
    }
    CustomAiModelConfiguration value;
    m_editing = !id.isEmpty();
    if (m_editing) {
        const auto models = m_session.customAiModels();
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
    modal->setObjectName(QStringLiteral("customAiModelEditor"));
    modal->setOwnerWindow(window());
    modal->setCentered(true);
    modal->setPreferredWidth(760);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    modal->setStandardButtons(AdModal::StandardButton::Ok | AdModal::StandardButton::Cancel);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QWidget(body);
    auto* grid = new QGridLayout(form);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(m_scheme.metricAlias.marginLG);
    grid->setVerticalSpacing(0);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    const QStringList names{QStringLiteral("modelName"), QStringLiteral("apiUrl"),
                            QStringLiteral("apiKey"), QStringLiteral("apiModel")};
    const QStringList values{value.name, value.baseUrl, value.apiKey, value.model};
    for (size_t i = 0; i < m_inputs.size(); ++i) {
        m_inputs[i] = i == 2 ? new AdPasswordEdit(form) : new AdLineEdit(form);
        m_inputs[i]->setObjectName(names[static_cast<qsizetype>(i)]);
        m_inputs[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_inputs[i]->setText(values[static_cast<qsizetype>(i)]);
        m_fields[i] =
            new AdFormItem(QString(), m_inputs[i], names[static_cast<qsizetype>(i)], form);
        m_fields[i]->setItemLayout(AdFormItem::ItemLayout::Vertical);
        grid->addWidget(m_fields[i], static_cast<int>(i / 2), static_cast<int>(i % 2),
                        Qt::AlignTop);
        m_fields[i]->setRequired(i != 2);
        m_fields[i]->setValidateOnChange(false);
    }
    m_modelSelect = new AdComboBox(form);
    m_modelSelect->setObjectName(QStringLiteral("apiModel"));
    m_modelSelect->setEditable(true);
    m_modelSelect->setPopupLayerMode(AdComboBox::PopupLayerMode::QtTool);
    m_modelSelect->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_modelSelect->setCurrentValue(value.model);
    // Commit typed IDs independently of the select's transient search text.
    connect(m_modelSelect->lineEdit(), &QLineEdit::textEdited, m_modelSelect,
            [select = m_modelSelect](const QString& text) { select->setCurrentValue(text); });
    m_fields[3] = new AdFormItem(QString(), m_modelSelect, QStringLiteral("apiModel"), form);
    m_fields[3]->setItemLayout(AdFormItem::ItemLayout::Vertical);
    m_fields[3]->setRequired(true);
    m_fields[3]->setValidateOnChange(false);
    grid->addWidget(m_fields[3], 1, 1, Qt::AlignTop);
    m_modelFetchStatus = new QLabel(m_modelSelect);
    m_modelFetchStatus->setObjectName(QStringLiteral("modelFetchStatus"));
    m_modelFetchStatus->setWordWrap(true);
    m_modelFetchStatus->setMargin(8);
    m_modelFetchStatus->hide();
    auto* fetchContent = new QWidget(m_modelSelect);
    auto* fetchLayout = new QHBoxLayout(fetchContent);
    fetchLayout->setContentsMargins(m_scheme.metricAlias.paddingSM, 0,
                                    m_scheme.metricAlias.paddingSM, 0);
    auto* fetchSpin = new AdSpin(fetchContent);
    fetchLayout->addWidget(fetchSpin, 0, Qt::AlignLeft | Qt::AlignVCenter);
    fetchLayout->addStretch();
    fetchSpin->setObjectName(QStringLiteral("modelFetchSpin"));
    fetchSpin->setSizeClass(AdSpin::SizeClass::Small);
    fetchSpin->setSpinning(false);
    fetchContent->hide();
    connect(m_modelSelect, &AdComboBox::loadingChanged, m_modelSelect,
            [select = m_modelSelect, fetchSpin, fetchContent](bool loading) {
                fetchSpin->setSpinning(loading);
                select->setNotFoundContentWidget(loading ? fetchContent : nullptr);
            });
    auto* network = new QNetworkAccessManager(body);
    auto pending = std::make_shared<QPointer<QNetworkReply>>();
    // Successful results belong to this editor and its current connection, even when empty.
    auto fetched = std::make_shared<bool>(false);
    const auto invalidate = [select = m_modelSelect, pending, fetched,
                             status = m_modelFetchStatus]() {
        *fetched = false;
        if (*pending) {
            auto* reply = pending->data();
            *pending = nullptr;
            reply->abort();
        }
        select->setLoading(false);
        select->clearOptions();
        status->setProperty("fetchFailed", false);
        status->clear();
        select->setPopupFooterWidget(nullptr);
    };
    connect(m_inputs[1], &AdLineEdit::textChanged, body, invalidate);
    connect(m_inputs[2], &AdLineEdit::textChanged, body, invalidate);
    connect(modal, &AdModal::finished, body, invalidate);
    connect(m_modelSelect, &AdComboBox::popupVisibleChanged, body,
            [this, network, pending, fetched, body](bool visible) {
                if (!visible || *pending || *fetched) {
                    return;
                }
                const auto connection = normalizeCustomAiModel(
                    {{}, {}, m_inputs[1]->text(), m_inputs[2]->text(), {}, false});
                if (customAiModelUrlError(connection.baseUrl) != CustomAiModelUrlError::None ||
                    connection.apiKey.contains(u'\r') || connection.apiKey.contains(u'\n')) {
                    return;
                }
                QNetworkRequest request(QUrl(connection.baseUrl + QStringLiteral("/models")));
                request.setTransferTimeout(15000);
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                     QNetworkRequest::SameOriginRedirectPolicy);
                request.setRawHeader("Accept", "application/json");
                if (!connection.apiKey.isEmpty()) {
                    request.setRawHeader("Authorization", "Bearer " + connection.apiKey.toUtf8());
                }
                m_modelFetchStatus->setProperty("fetchFailed", false);
                m_modelFetchStatus->clear();
                m_modelSelect->setPopupFooterWidget(nullptr);
                m_modelSelect->clearOptions();
                m_modelSelect->setLoading(true);
                auto* reply = network->get(request);
                *pending = reply;
                connect(reply, &QNetworkReply::finished, body, [this, reply, pending, fetched]() {
                    reply->deleteLater();
                    if (pending->data() != reply) {
                        return;
                    }
                    *pending = nullptr;
                    const auto document = QJsonDocument::fromJson(reply->readAll());
                    const auto data = document.object().value(QStringLiteral("data"));
                    const bool failed = reply->error() != QNetworkReply::NoError || !data.isArray();
                    *fetched = !failed;
                    QVector<AdComboBox::Option> options;
                    QSet<QString> seen;
                    if (!failed) {
                        for (const auto& entry : data.toArray()) {
                            const auto id =
                                entry.toObject().value(QStringLiteral("id")).toString().trimmed();
                            if (id.isEmpty() || seen.contains(id)) {
                                continue;
                            }
                            seen.insert(id);
                            AdComboBox::Option option;
                            option.value = id;
                            option.label = id;
                            options.append(option);
                        }
                    }
                    m_modelSelect->setOptions(options);
                    m_modelSelect->setLoading(false);
                    m_modelFetchStatus->setProperty("fetchFailed", failed);
                    translateModal();
                });
            });
    m_inputs[1]->setPlaceholderText(QStringLiteral("https://api.openai.com/v1"));
    m_vision = new AdSwitch(form);
    m_vision->setObjectName(QStringLiteral("visionSupport"));
    m_vision->setChecked(value.supportsVision);
    m_fields[4] = new AdFormItem(QString(), m_vision, QStringLiteral("visionSupport"), form);
    m_fields[4]->setItemLayout(AdFormItem::ItemLayout::Vertical);
    grid->addWidget(m_fields[4], 2, 0, 1, 2);
    layout->addWidget(form);
    m_modalError = new AdAlert(body);
    m_modalError->setSeverity(AdAlert::Severity::Error);
    m_modalError->hide();
    layout->addWidget(m_modalError);
    modal->setContentWidget(body);
    translateModal();
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

void CustomAiModelsSettingsWidget::submitEditor(bool saveChanges) {
    auto value = normalizeCustomAiModel(
        {m_editId, m_inputs[0]->text(), m_inputs[1]->text(), m_inputs[2]->text(),
         m_modelSelect->currentValue().toString(), m_vision->isChecked()});
    auto models = m_session.customAiModels();
    std::array<QString, 4> errors;
    if (value.name.isEmpty()) {
        errors[0] = tr("Enter a model name.");
    } else if (std::any_of(models.cbegin(), models.cend(), [&value](const auto& model) {
                   return model.id != value.id &&
                          model.name.compare(value.name, Qt::CaseInsensitive) == 0;
               })) {
        errors[0] = tr("A model with this name already exists.");
    }
    const auto urlError = customAiModelUrlError(value.baseUrl);
    if (urlError == CustomAiModelUrlError::FullEndpoint) {
        errors[1] = tr("Enter the base URL without /chat/completions.");
    } else if (urlError != CustomAiModelUrlError::None) {
        errors[1] =
            tr("Enter an HTTP or HTTPS base URL without credentials, a query, or a fragment.");
    }
    if (value.apiKey.contains(u'\r') || value.apiKey.contains(u'\n')) {
        errors[2] = tr("The API key must not contain line breaks.");
    }
    if (value.model.isEmpty()) {
        errors[3] = tr("Enter the API model ID.");
    }
    QWidget* firstInvalid = nullptr;
    for (size_t i = 0; i < errors.size(); ++i) {
        m_fields[i]->setErrorMessages(errors[i].isEmpty() ? QStringList{} : QStringList{errors[i]});
        m_fields[i]->setValidateStatus(errors[i].isEmpty() ? AdFormItem::ValidateStatus::None
                                                           : AdFormItem::ValidateStatus::Error);
        if (!errors[i].isEmpty() && firstInvalid == nullptr) {
            firstInvalid = i == 3 ? static_cast<QWidget*>(m_modelSelect) : m_inputs[i];
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
            tr("This model was deleted. Close this form and create a new model."));
        m_modalError->show();
        return;
    }
    if (it != models.end()) {
        *it = value;
    } else {
        models.push_back(value);
    }
    if (save(models)) {
        m_modal->accept();
    } else {
        m_modalError->setProperty("deletedModel", false);
        m_modalError->setText(m_error->text());
        m_modalError->show();
    }
}

void CustomAiModelsSettingsWidget::translateModal() {
    if (m_modal != nullptr) {
        m_modal->setWindowTitle(m_editing ? tr("Edit Model") : tr("Add Model"));
        m_modal->setAcceptText(tr("Save"));
        m_modal->setRejectText(tr("Cancel"));
        if (m_modalError != nullptr && !m_modalError->isHidden()) {
            m_modalError->setText(
                m_modalError->property("deletedModel").toBool()
                    ? tr("This model was deleted. Close this form and create a new model.")
                    : tr("Unable to save models. Check that configuration storage is writable and "
                         "try again."));
        }
        const QStringList labels{tr("Model Name"), tr("API URL"), tr("API Key"), tr("API Model"),
                                 tr("Vision Support")};
        for (size_t i = 0; i < m_fields.size(); ++i) {
            m_fields[i]->setLabel(labels[static_cast<qsizetype>(i)]);
            if (i < m_inputs.size()) {
                m_inputs[i]->setAccessibleName(labels[static_cast<qsizetype>(i)]);
            }
        }
        m_modelSelect->setAccessibleName(tr("API Model"));
        m_modelSelect->setPlaceholder(tr("Enter or select a model ID"));
        m_modelFetchStatus->setText(
            m_modelFetchStatus->property("fetchFailed").toBool()
                ? tr("Unable to fetch models. Enter a model ID or reopen the list to retry.")
                : QString());
        // An attached footer contributes its size even when its label is empty.
        m_modelSelect->setPopupFooterWidget(
            m_modelFetchStatus->property("fetchFailed").toBool() ? m_modelFetchStatus : nullptr);
        m_vision->setAccessibleName(tr("Vision Support"));
        m_fields[0]->setTooltipText(tr("The model name displayed in Snow Shot."));
        m_fields[1]->setTooltipText(tr(
            "OpenAI-compatible Chat Completions. /chat/completions is appended to this base URL."));
        m_fields[2]->setTooltipText(tr("Optional for servers that do not require authentication."));
        m_fields[3]->setTooltipText(
            tr("Enter a custom model ID or open the list to fetch models from the API URL."));
        m_fields[4]->setTooltipText(tr("Allow this model to convert images to Markdown and HTML."));
    }
    if (m_deleteModal != nullptr) {
        m_deleteModal->setWindowTitle(tr("Delete Model"));
        m_deleteModal->setAcceptText(tr("Delete"));
        m_deleteModal->setRejectText(tr("Cancel"));
        for (const auto& model : m_session.customAiModels()) {
            if (model.id == m_deleteId) {
                m_deleteModal->setText(
                    tr("Delete model \"%1\"? If selected, another available model will be used.")
                        .arg(model.name));
            }
        }
    }
}

void CustomAiModelsSettingsWidget::retranslateUi() {
    m_error->setText(
        tr("Unable to save models. Check that configuration storage is writable and try again."));
    m_add->setText(tr("Add Model"));
    m_title->setText(QCoreApplication::translate("SettingsCatalog", "Custom Models"));
    rebuild();
    translateModal();
    if (m_modal != nullptr &&
        std::any_of(m_fields.cbegin(), m_fields.cend(),
                    [](const auto* field) { return !field->errorMessages().isEmpty(); })) {
        submitEditor(false);
    }
}

void CustomAiModelsSettingsWidget::changeEvent(QEvent* event) {
    SettingsCustomWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}
