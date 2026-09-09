#include "snow_shot/presentation/components/globalmouserow.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/components/shortcutconfigurationbutton.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"

#include "widgets/form.h"
#include "widgets/modal.h"
#include "widgets/select.h"

#include <QAbstractButton>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QPointer>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace {
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;
namespace settings = snow_shot::presentation::settings;

constexpr int CONFIGURATION_MODAL_WIDTH = 520;
constexpr int COMBINATION_TEXT_MAX_WIDTH = 260;

adqt::icons::IconRef actionIcon(settings::SettingsGlobalMouseAction action) {
    switch (action) {
    case settings::SettingsGlobalMouseAction::ScreenshotCopy:
        return custom_outlined_icons::ScreenshotCopy();
    case settings::SettingsGlobalMouseAction::ScreenshotFixed:
        return custom_outlined_icons::PinToScreen();
    case settings::SettingsGlobalMouseAction::ScreenshotOcr:
        return custom_outlined_icons::ToolRecognizeText();
    case settings::SettingsGlobalMouseAction::ScreenshotTranslation:
        return custom_outlined_icons::OcrTranslate();
    case settings::SettingsGlobalMouseAction::ScreenshotSave:
        return custom_outlined_icons::Save();
    case settings::SettingsGlobalMouseAction::ScreenshotQuickSave:
        return custom_outlined_icons::QuickSave();
    case settings::SettingsGlobalMouseAction::ScreenRecording:
        return custom_outlined_icons::RecordScreen();
    }
    return {};
}

adqt::widgets::AdSelect::Option option(const QString& value, const QString& label) {
    adqt::widgets::AdSelect::Option result;
    result.value = value;
    result.label = label;
    return result;
}
} // namespace

GlobalMouseRow::GlobalMouseRow(const QString& title, settings::SettingsGlobalMouseAction action,
                               settings::SettingsRuntimeSession& runtimeSession,
                               const snow_shot::presentation::styles::ThemeColorScheme& colorScheme,
                               QWidget* parent)
    : ActionRow({title, actionIcon(action), {}, true}, colorScheme.metricAlias,
                snow_shot::presentation::styles::buildMainWindowComponentMetricToken(colorScheme),
                colorScheme, parent),
      m_title(title), m_action(action), m_runtimeSession(runtimeSession) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const auto& metric = m_colorScheme.metricAlias;
    m_titleLabel->setObjectName(QStringLiteral("globalMouseActionLabel"));
    if (auto* icon = findChild<QLabel*>(QStringLiteral("actionRowTitleIcon"))) {
        icon->setObjectName(QStringLiteral("globalMouseActionIcon"));
    }

    m_button = new ShortcutConfigurationButton(metric, COMBINATION_TEXT_MAX_WIDTH,
                                               custom_outlined_icons::WheelMouse(), this);
    m_button->setObjectName(QStringLiteral("globalMouseConfigurationButton"));
    m_button->setFixedHeight(metric.controlHeight);
    m_button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_button->setRegistrationStatusTooltipVisible(false);
    setConfigurationButton(m_button);
    connect(m_button, &QAbstractButton::clicked, this, [this]() { openConfigurationDialog(); });

    setCombination(m_runtimeSession.globalMouseCombination(m_action));
    retranslateUi();
    applyTheme(m_colorScheme);
}

void GlobalMouseRow::setTitle(const QString& title) {
    m_title = title;
    setAccessibleName(m_title);
    if (m_titleLabel != nullptr) {
        m_titleLabel->setText(m_title);
    }
    retranslateUi();
}

void GlobalMouseRow::setCombination(const settings::SettingsGlobalMouseCombination& combination) {
    m_combination = combination;
    syncButton();
}

ShortcutConfigurationButton* GlobalMouseRow::configurationButton() const {
    return m_button;
}

void GlobalMouseRow::applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    ActionRow::applyTheme(scheme);
    if (m_validationLabel != nullptr) {
        QPalette palette = m_validationLabel->palette();
        palette.setColor(QPalette::WindowText, scheme.map.colorError);
        m_validationLabel->setPalette(palette);
    }
    if (m_button != nullptr) {
        m_button->setTheme(scheme);
    }
    update();
}

void GlobalMouseRow::retranslateUi() {
    if (m_titleLabel != nullptr) {
        m_titleLabel->setText(m_title);
    }
    if (m_button != nullptr) {
        const QString accessibleName = tr("Configure mouse combination for %1").arg(m_title);
        m_button->setAccessibleName(accessibleName);
        m_button->setToolTip(accessibleName);
    }
    syncButton();
    syncModalText();
}

void GlobalMouseRow::changeEvent(QEvent* event) {
    ActionRow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void GlobalMouseRow::mousePressEvent(QMouseEvent* event) {
    ActionRow::mousePressEvent(event);
    if (event->button() == Qt::LeftButton && isEnabled() && m_modal == nullptr &&
        rect().contains(event->position().toPoint())) {
        emit dragRequested(m_action);
    }
}

void GlobalMouseRow::openConfigurationDialog() {
    if (m_modal != nullptr) {
        return;
    }

    auto* modal = new adqt::widgets::AdModal(this);
    m_modal = modal;
    modal->setObjectName(QStringLiteral("globalMouseConfigurationModal"));
    modal->setOwnerWindow(window());
    modal->setCentered(true);
    modal->setPreferredWidth(CONFIGURATION_MODAL_WIDTH);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(adqt::widgets::AdModal::ClosePolicy::Manual);
    modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                              adqt::widgets::AdModal::StandardButton::Cancel);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("globalMouseConfigurationContent"));
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(m_colorScheme.metricAlias.margin);

    auto* form = new adqt::widgets::AdForm(content);
    form->setObjectName(QStringLiteral("globalMouseConfigurationForm"));
    form->setFormLayout(adqt::widgets::AdForm::FormLayout::Vertical);
    contentLayout->addWidget(form);
    const auto addSelect = [form](const QString& objectName) {
        auto* select = new adqt::widgets::AdSelect(form);
        select->setObjectName(objectName);
        select->setMode(adqt::widgets::AdSelect::Mode::Single);
        select->setControlSize(adqt::widgets::AdSelect::ControlSize::Middle);
        return select;
    };

    m_activationSelect = addSelect(QStringLiteral("globalMouseActivationKeySelect"));
    m_activationSelect->setMode(adqt::widgets::AdSelect::Mode::Multiple);
    m_activationField =
        form->addField(QString(), m_activationSelect, QStringLiteral("activationKeys"));
    m_mouseButtonSelect = addSelect(QStringLiteral("globalMouseButtonSelect"));
    // Keep validation inside the field, before AdForm's trailing item margin.
    auto* mouseButtonEditor = new QWidget(form);
    auto* mouseButtonLayout = new QVBoxLayout(mouseButtonEditor);
    mouseButtonLayout->setContentsMargins(0, 0, 0, 0);
    mouseButtonLayout->setSpacing(m_colorScheme.metricAlias.marginXS);
    mouseButtonLayout->addWidget(m_mouseButtonSelect);

    m_validationLabel = new QLabel(mouseButtonEditor);
    m_validationLabel->setObjectName(QStringLiteral("globalMouseValidationMessage"));
    m_validationLabel->setWordWrap(true);
    m_validationLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    QSizePolicy validationPolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    validationPolicy.setRetainSizeWhenHidden(true);
    m_validationLabel->setSizePolicy(validationPolicy);
    m_validationLabel->hide();
    mouseButtonLayout->addWidget(m_validationLabel);
    m_mouseButtonField =
        form->addField(QString(), mouseButtonEditor, QStringLiteral("mouseButton"));

    const settings::SettingsGlobalMouseCombination initial =
        m_combination.isUnset()
            ? settings::SettingsGlobalMouseCombination{{QStringLiteral("windows")}, {}}
            : m_combination;
    modal->setContentWidget(content);
    syncModalText();
    QVariantList initialKeys;
    for (const QString& key : initial.activationKeys) {
        initialKeys.push_back(key);
    }
    m_activationSelect->setCurrentValues(initialKeys);
    m_mouseButtonSelect->setCurrentValue(initial.mouseButton.isEmpty() ? QVariant()
                                                                       : initial.mouseButton);

    connect(m_activationSelect, &adqt::widgets::AdSelect::currentValuesChanged, this,
            [this](const QVariantList&) { syncModalValidation(); });
    connect(m_mouseButtonSelect, &adqt::widgets::AdSelect::currentValueChanged, this,
            [this](const QVariant&) { syncModalValidation(); });
    connect(
        modal, &adqt::widgets::AdModal::closeRequested, modal,
        [this, modal](adqt::widgets::AdModal::CloseReason reason) {
            if (reason == adqt::widgets::AdModal::CloseReason::OkAction &&
                (m_mouseButtonSelect->currentValue() == QStringLiteral("none") ||
                 !m_activationSelect->currentValues().isEmpty()) &&
                m_mouseButtonSelect->currentValue().isValid() &&
                m_runtimeSession.globalMouseCombinationAvailable(m_action, modalCombination())) {
                modal->accept();
            } else if (reason != adqt::widgets::AdModal::CloseReason::OkAction) {
                modal->reject();
            }
        });
    connect(modal, &adqt::widgets::AdModal::accepted, this, [this]() {
        static_cast<void>(
            m_runtimeSession.applyGlobalMouseCombination(m_action, modalCombination()));
    });
    connect(modal, &adqt::widgets::AdModal::finished, this,
            [this, modal](adqt::widgets::AdModal::DialogCode) {
                modal->deleteLater();
                m_modal = nullptr;
                m_activationField = nullptr;
                m_mouseButtonField = nullptr;
                m_validationLabel = nullptr;
                m_activationSelect = nullptr;
                m_mouseButtonSelect = nullptr;
            });
    syncModalValidation();
    modal->open();
    syncModalValidation();
}

void GlobalMouseRow::syncButton() {
    if (m_button == nullptr) {
        return;
    }
    const bool unset = m_combination.isUnset();
    QStringList keys;
    for (const QString& key : m_combination.sortedActivationKeys()) {
        keys.push_back(activationKeyLabel(key));
    }
    m_button->setText(unset ? tr("Unset")
                            : tr("%1 + %2").arg(keys.join(QStringLiteral(" + ")),
                                                mouseButtonLabel(m_combination.mouseButton)));
    m_button->setRegistrationStatus(
        unset ? snow_shot::presentation::GlobalShortcutStatus::Unset
              : snow_shot::presentation::GlobalShortcutStatus::Registered);
}

void GlobalMouseRow::syncModalText() {
    if (m_modal == nullptr || m_activationSelect == nullptr || m_mouseButtonSelect == nullptr) {
        return;
    }
    m_modal->setWindowTitle(tr("Mouse configuration for \"%1\"").arg(m_title));
    m_modal->setAcceptText(tr("OK"));
    m_modal->setRejectText(tr("Cancel"));
    if (m_activationField != nullptr) {
        m_activationField->setLabel(tr("Activation keys"));
        m_activationSelect->setAccessibleName(m_activationField->label());
    }
    if (m_mouseButtonField != nullptr) {
        m_mouseButtonField->setLabel(tr("Mouse button"));
        m_mouseButtonSelect->setAccessibleName(m_mouseButtonField->label());
    }
    const QVariantList activation = m_activationSelect->currentValues();
    const QVariant mouseButton = m_mouseButtonSelect->currentValue();
    m_activationSelect->setOptions({option(QStringLiteral("windows"), tr("Windows")),
                                    option(QStringLiteral("ctrl"), tr("Ctrl")),
                                    option(QStringLiteral("alt"), tr("Alt")),
                                    option(QStringLiteral("shift"), tr("Shift"))});
    m_mouseButtonSelect->setOptions(
        {option(QStringLiteral("left_drag"), tr("Left-button drag")),
         option(QStringLiteral("right_drag"), tr("Right-button drag")),
         option(QStringLiteral("wheel_drag"), tr("Wheel drag")),
         option(QStringLiteral("side_button_1_drag"), tr("Side button 1 (Back) drag")),
         option(QStringLiteral("side_button_2_drag"), tr("Side button 2 (Forward) drag")),
         option(QStringLiteral("none"), tr("None"))});
    m_activationSelect->setCurrentValues(activation);
    if (mouseButton.isValid()) {
        m_mouseButtonSelect->setCurrentValue(mouseButton);
    }
    // Reserve the longest translated validation message before the modal is shown.
    const QFontMetrics metrics(m_validationLabel->font());
    const int messageWidth = CONFIGURATION_MODAL_WIDTH - 2 * m_colorScheme.metricAlias.paddingLG;
    int messageHeight = 0;
    for (const QString& message :
         {tr("Select at least one activation key."), tr("Select a mouse button."),
          tr("This mouse combination is already assigned to another action. Choose a "
             "different combination.")}) {
        messageHeight = qMax(
            messageHeight,
            metrics.boundingRect(QRect(0, 0, messageWidth, 0), Qt::TextWordWrap, message).height());
    }
    m_validationLabel->setFixedHeight(messageHeight);
    syncModalValidation();
}

void GlobalMouseRow::syncModalValidation() {
    if (m_modal == nullptr || m_validationLabel == nullptr || m_activationSelect == nullptr ||
        m_mouseButtonSelect == nullptr) {
        return;
    }
    const bool hasKeys = !m_activationSelect->currentValues().isEmpty();
    const bool hasMouseButton = m_mouseButtonSelect->currentValue().isValid();
    const bool none = m_mouseButtonSelect->currentValue() == QStringLiteral("none");
    m_activationSelect->setEnabled(!none);
    const bool available =
        (none || hasKeys) && hasMouseButton &&
        m_runtimeSession.globalMouseCombinationAvailable(m_action, modalCombination());
    m_validationLabel->setText(
        available  ? QString()
        : !hasKeys ? tr("Select at least one activation key.")
        : !hasMouseButton
            ? tr("Select a mouse button.")
            : tr("This mouse combination is already assigned to another action. Choose a "
                 "different combination."));
    m_validationLabel->setVisible(!available);
    if (m_modal->acceptButton() != nullptr) {
        m_modal->acceptButton()->setEnabled(available);
    }
    applyTheme(m_colorScheme);
}

settings::SettingsGlobalMouseCombination GlobalMouseRow::modalCombination() const {
    if (m_mouseButtonSelect != nullptr &&
        m_mouseButtonSelect->currentValue() == QStringLiteral("none")) {
        return {};
    }
    QStringList keys;
    if (m_activationSelect != nullptr) {
        for (const QVariant& key : m_activationSelect->currentValues()) {
            keys.push_back(key.toString());
        }
    }
    keys.sort();
    return {keys, m_mouseButtonSelect != nullptr ? m_mouseButtonSelect->currentValue().toString()
                                                 : QString()};
}

QString GlobalMouseRow::activationKeyLabel(const QString& value) const {
    if (value == QStringLiteral("windows")) {
        return tr("Windows");
    }
    if (value == QStringLiteral("ctrl")) {
        return tr("Ctrl");
    }
    if (value == QStringLiteral("alt")) {
        return tr("Alt");
    }
    return value == QStringLiteral("shift") ? tr("Shift") : value;
}

QString GlobalMouseRow::mouseButtonLabel(const QString& value) const {
    if (value == QStringLiteral("left_drag")) {
        return tr("Left-button drag");
    }
    if (value == QStringLiteral("right_drag")) {
        return tr("Right-button drag");
    }
    if (value == QStringLiteral("wheel_drag")) {
        return tr("Wheel drag");
    }
    if (value == QStringLiteral("side_button_1_drag")) {
        return tr("Side button 1 (Back) drag");
    }
    return value == QStringLiteral("side_button_2_drag") ? tr("Side button 2 (Forward) drag")
                                                         : value;
}
