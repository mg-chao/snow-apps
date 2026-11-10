#include "snow_shot/presentation/components/accountpagewidget.h"

#include "snow_shot/app/accountsessionservice.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/presentation/components/emptystateicon.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "antd_icons.h"
#include "icon_renderer.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/form.h"
#include "widgets/divider.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_password_edit.h"
#include "widgets/message.h"
#include "widgets/scroll_area.h"
#include "widgets/tag.h"

#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QTimer>
#include <QtMath>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

using namespace adqt::widgets;
using snow_shot::app::AccountSessionService;
namespace styles = snow_shot::presentation::styles;

namespace {
QLabel* copyLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}
QVBoxLayout* column(QWidget* parent, int spacing = 12) {
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
    return layout;
}

void transactionColumns(QGridLayout* layout) {
    layout->setColumnStretch(0, 4);
    layout->setColumnStretch(1, 4);
    layout->setColumnStretch(2, 3);
    layout->setColumnStretch(3, 4);
}

class AccountTransactionRow final : public QWidget {
  public:
    AccountTransactionRow(const QJsonObject& transaction, const QString& type,
                          const QString& status, QWidget* parent)
        : QWidget(parent) {
        setObjectName(QStringLiteral("accountTransaction-") +
                      transaction.value(QStringLiteral("id")).toString());
        setMinimumWidth(0);
        m_layout = new QGridLayout(this);
        transactionColumns(m_layout);
        m_type = copyLabel(this);
        m_type->setObjectName(QStringLiteral("accountTransactionType"));
        m_type->setText(type);
        m_date = copyLabel(this);
        m_date->setObjectName(QStringLiteral("accountTransactionDate"));
        const auto date = QDateTime::fromString(
            transaction.value(QStringLiteral("occurred_at")).toString(), Qt::ISODateWithMs);
        m_date->setText(date.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        m_amount = copyLabel(this);
        m_amount->setObjectName(QStringLiteral("accountTransactionAmount"));
        m_amount->setText(AccountSessionService::formatCredits(
            transaction.value(QStringLiteral("amount_nano_yuan")).toString()));
        m_amount->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        m_status = new AdTag(status, this);
        m_status->setObjectName(QStringLiteral("accountTransactionStatus"));
        m_status->setFocusPolicy(Qt::NoFocus);
        const QString state = transaction.value(QStringLiteral("status")).toString();
        m_status->setColorScheme(state == u"completed" ? AdTag::ColorScheme::Success
                                 : state == u"failed"  ? AdTag::ColorScheme::Error
                                                       : AdTag::ColorScheme::Processing);
        m_order = copyLabel(this);
        m_order->setObjectName(QStringLiteral("accountTransactionOrder"));
        const QString orderId = transaction.value(QStringLiteral("order_id")).toString();
        m_order->setText(orderId.isEmpty() ? QString()
                                           : AccountPageWidget::tr("Order ID: %1").arg(orderId));
        m_order->setVisible(!orderId.isEmpty());
        m_order->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, this,
                [this] { applyTheme(); });
        applyTheme();
        arrange();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        arrange();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto& scheme = styles::ThemeManager::instance().themeColorScheme();
        painter.setPen(QPen(scheme.map.colorSplit, scheme.metricAlias.lineWidth));
        painter.drawLine(0, 0, width(), 0);
    }

  private:
    void applyTheme() {
        const auto& scheme = styles::ThemeManager::instance().themeColorScheme();
        QFont body = font();
        body.setPixelSize(scheme.metricAlias.fontSize);
        body.setWeight(QFont::Normal);
        m_type->setFont(body);
        body.setWeight(QFont::DemiBold);
        m_amount->setFont(body);
        body.setWeight(QFont::Normal);
        body.setPixelSize(scheme.metricAlias.fontSizeSM);
        QPalette muted = palette();
        muted.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
        for (auto* label : {m_date, m_order}) {
            label->setFont(body);
            label->setPalette(muted);
        }
        m_layout->setContentsMargins(0, scheme.metricAlias.padding, 0, scheme.metricAlias.padding);
        m_layout->setHorizontalSpacing(scheme.metricAlias.padding);
        m_layout->setVerticalSpacing(scheme.metricAlias.paddingXS);
        m_arranged = false;
        arrange();
        update();
    }

    void arrange() {
        const auto& metric = styles::ThemeManager::instance().themeColorScheme().metricAlias;
        const bool compact = width() < metric.fontSize * 42;
        if (m_arranged && compact == m_compact)
            return;
        m_arranged = true;
        m_compact = compact;
        for (QWidget* widget : {static_cast<QWidget*>(m_type), static_cast<QWidget*>(m_date),
                                static_cast<QWidget*>(m_amount), static_cast<QWidget*>(m_order),
                                static_cast<QWidget*>(m_status)})
            m_layout->removeWidget(widget);
        m_layout->addWidget(m_type, 0, 0, 1, compact ? 2 : 1);
        m_layout->addWidget(m_date, compact ? 2 : 0, compact ? 0 : 1, 1, compact ? 4 : 1);
        m_layout->addWidget(m_status, 0, 2, 1, compact ? 2 : 1,
                            compact ? Qt::AlignRight : Qt::AlignLeft);
        m_layout->addWidget(m_amount, compact ? 1 : 0, compact ? 0 : 3, 1, compact ? 4 : 1);
        m_amount->setAlignment(compact ? Qt::AlignLeft : Qt::AlignRight);
        m_layout->addWidget(m_order, compact ? 3 : 1, 0, 1, 4);
    }

    QGridLayout* m_layout = nullptr;
    QLabel* m_type = nullptr;
    QLabel* m_date = nullptr;
    QLabel* m_amount = nullptr;
    QLabel* m_order = nullptr;
    AdTag* m_status = nullptr;
    bool m_arranged = false;
    bool m_compact = false;
};

void imageFromData(QLabel* label, const QString& data, const QSize& bounds) {
    const auto comma = data.indexOf(u',');
    QPixmap pixmap;
    if (data.startsWith(QStringLiteral("data:image/")) && comma > 0 && data.size() < 4000000)
        pixmap.loadFromData(QByteArray::fromBase64(data.mid(comma + 1).toLatin1()));
    if (!pixmap.isNull()) {
        const qreal ratio = label->devicePixelRatioF();
        const QSize pixels(std::max(1, qFloor(bounds.width() * ratio)),
                           std::max(1, qFloor(bounds.height() * ratio)));
        pixmap = pixmap.scaled(pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(ratio);
    }
    label->setPixmap(pixmap);
}
} // namespace

AccountPageWidget::AccountPageWidget(AccountSessionService* service, QWidget* parent)
    : QWidget(parent), m_service(service) {
    setObjectName(QStringLiteral("accountPage"));
    auto* root = column(this, 0);
    m_container = new PageContainerWidget(
        styles::ThemeManager::instance().themeColorScheme().metricAlias, this);
    root->addWidget(m_container);
    auto* content = m_container->contentWidget();
    auto* layout = m_container->contentLayout();
    layout->setSpacing(0);
    auto* heading = new QWidget(content);
    auto* headingLayout = column(heading, 0);
    m_pageTitle = copyLabel(heading);
    m_pageTitle->setObjectName(QStringLiteral("accountPageHeading"));
    m_pageDescription = copyLabel(heading);
    headingLayout->addWidget(m_pageTitle);
    headingLayout->addWidget(m_pageDescription);
    layout->addWidget(heading);
    m_notice = new AdAlert(content);
    m_notice->setSeverity(AdAlert::Severity::Info);
    m_notice->setAnimated(false);
    m_notice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(m_notice);

    m_auth = new QWidget(content);
    auto* authSection = column(m_auth);
    m_authTitle = copyLabel(m_auth);
    m_authTitle->setObjectName(QStringLiteral("accountAuthHeading"));
    authSection->addWidget(m_authTitle);
    auto* authContent = new QWidget(m_auth);
    authContent->setMinimumWidth(0);
    authContent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    authContent->setObjectName(QStringLiteral("accountAuthCard"));
    authContent->setMaximumWidth(440);
    auto* auth = column(authContent);
    m_form = new AdForm(authContent);
    m_form->setObjectName(QStringLiteral("accountForm"));
    m_form->setFormLayout(AdForm::FormLayout::Vertical);
    m_form->setRequiredMark(AdForm::RequiredMark::Hidden);
    m_form->setColon(false);
    auth->addWidget(m_form);
    const auto field = [this](const QString& id, const QString& key, AdLineEdit* editor) {
        editor->setObjectName(id);
        editor->setMinimumWidth(0);
        editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* item = m_form->addField({}, editor, key);
        item->setValidateOnChange(false);
        connect(editor, &QLineEdit::textChanged, this, [this, key] {
            if (m_service)
                m_service->clearFieldError(key);
        });
        return item;
    };
    m_username = new AdLineEdit(authContent);
    m_username->setMaxLength(32);
    m_usernameField =
        field(QStringLiteral("accountUsername"), QStringLiteral("username"), m_username);
    m_password = new AdPasswordEdit(authContent);
    m_password->setMaxLength(16);
    m_passwordField =
        field(QStringLiteral("accountPassword"), QStringLiteral("password"), m_password);
    m_confirmation = new AdPasswordEdit(authContent);
    m_confirmation->setMaxLength(16);
    m_confirmationField = field(QStringLiteral("accountPasswordConfirmation"),
                                QStringLiteral("password_confirmation"), m_confirmation);
    m_captchaAnswer = new AdLineEdit(authContent);
    m_captchaAnswer->setMaxLength(4);
    m_captchaField = field(QStringLiteral("accountCaptchaAnswer"), QStringLiteral("captcha_answer"),
                           m_captchaAnswer);
    m_captchaRow = new QWidget(authContent);
    auto* captchaLayout = new QHBoxLayout(m_captchaRow);
    captchaLayout->setContentsMargins(0, 0, 0, 0);
    m_captchaImage = new QLabel(m_captchaRow);
    m_captchaImage->setObjectName(QStringLiteral("accountCaptchaImage"));
    m_captchaImage->setFixedSize(160, 64);
    m_captchaImage->setAlignment(Qt::AlignCenter);
    m_captchaRefresh = new AdButton(m_captchaRow);
    m_captchaRefresh->setButtonStyle(AdButton::ButtonStyle::Text);
    m_captchaRefresh->setObjectName(QStringLiteral("accountCaptchaRefresh"));
    captchaLayout->addWidget(m_captchaImage, 0);
    captchaLayout->addWidget(m_captchaRefresh, 0);
    captchaLayout->addStretch();
    auth->addWidget(m_captchaRow);
    m_agreementRow = new QWidget(authContent);
    m_agreementRow->setObjectName(QStringLiteral("accountAgreementRow"));
    auto* agreementLayout = new QHBoxLayout(m_agreementRow);
    agreementLayout->setContentsMargins(0, 0, 0, 0);
    m_agreement = new AdCheckbox(m_agreementRow);
    m_agreement->setObjectName(QStringLiteral("accountAgreement"));
    m_agreement->setFocusPolicy(Qt::StrongFocus);
    m_agreementText = copyLabel(m_agreementRow);
    m_agreementText->setObjectName(QStringLiteral("accountAgreementText"));
    m_agreementText->setTextFormat(Qt::RichText);
    m_agreementText->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                             Qt::LinksAccessibleByKeyboard);
    m_agreementText->setOpenExternalLinks(true);
    agreementLayout->addWidget(m_agreement, 0, Qt::AlignTop);
    agreementLayout->addWidget(m_agreementText, 1);
    auth->addWidget(m_agreementRow);
    m_submit = new AdButton(authContent);
    m_submit->setObjectName(QStringLiteral("accountSubmit"));
    m_submit->setButtonStyle(AdButton::ButtonStyle::Solid);
    m_submit->setAccentRole(AdButton::AccentRole::Primary);
    auth->addWidget(m_submit);
    m_switch = new AdButton(authContent);
    m_switch->setObjectName(QStringLiteral("accountSwitchForm"));
    m_switch->setButtonStyle(AdButton::ButtonStyle::Link);
    auth->addWidget(m_switch);
    auto* authRow = new QHBoxLayout;
    authRow->addWidget(authContent);
    authSection->addLayout(authRow);
    layout->addWidget(m_auth);

    m_profile = new QWidget(content);
    m_profile->setObjectName(QStringLiteral("accountOverviewCard"));
    auto* profile = column(m_profile, 16);
    m_identityLayout = new QBoxLayout(QBoxLayout::LeftToRight);
    m_identityLayout->setSpacing(12);
    auto* identity = new QWidget(m_profile);
    auto* identityText = column(identity, 4);
    m_accountName = copyLabel(m_profile);
    m_accountName->setObjectName(QStringLiteral("accountDisplayUsername"));
    m_signOut = new AdButton(m_profile);
    m_signOut->setObjectName(QStringLiteral("accountSignOut"));
    m_signOut->setButtonStyle(AdButton::ButtonStyle::Text);
    m_signOut->setIconRef(adqt::icons::antd::outlined::Logout());
    identityText->addWidget(m_accountName);
    m_cumulativeRecharge = copyLabel(m_profile);
    m_cumulativeRecharge->setObjectName(QStringLiteral("accountCumulativeRecharge"));
    identityText->addWidget(m_cumulativeRecharge);
    m_identityLayout->addWidget(identity, 1);
    m_identityLayout->addWidget(m_signOut, 0, Qt::AlignLeft);
    profile->addLayout(m_identityLayout);
    auto* profileDivider = new AdDivider(m_profile);
    AdDivider::ComponentTokens dividerTokens;
    dividerTokens.metrics.horizontalMarginSmall = 0;
    profileDivider->setComponentTokens(dividerTokens);
    profileDivider->setDividerSize(AdDivider::Size::Small);
    profile->addWidget(profileDivider);
    m_balanceLayout = new QBoxLayout(QBoxLayout::LeftToRight);
    m_balanceLayout->setSpacing(24);
    auto* balance = new QWidget(m_profile);
    auto* balanceText = new QHBoxLayout(balance);
    balanceText->setContentsMargins(0, 0, 0, 0);
    m_creditIcon = new QLabel(balance);
    m_creditIcon->setObjectName(QStringLiteral("accountCreditIcon"));
    m_credit = copyLabel(balance);
    m_credit->setObjectName(QStringLiteral("accountAvailableCredit"));
    balanceText->addWidget(m_creditIcon, 0, Qt::AlignVCenter);
    balanceText->addWidget(m_credit, 1);
    m_topUp = new AdButton(m_profile);
    m_topUp->setObjectName(QStringLiteral("accountTopUp"));
    m_topUp->setButtonStyle(AdButton::ButtonStyle::Solid);
    m_topUp->setAccentRole(AdButton::AccentRole::Primary);
    m_topUp->setIconRef(adqt::icons::antd::outlined::Plus());
    m_balanceLayout->addWidget(balance, 1);
    m_balanceLayout->addWidget(m_topUp, 0, Qt::AlignLeft);
    profile->addLayout(m_balanceLayout);
    m_debt = new AdAlert(m_profile);
    m_debt->setObjectName(QStringLiteral("accountOutstandingBalance"));
    m_debt->setSeverity(AdAlert::Severity::Warning);
    m_debt->setAnimated(false);
    m_debt->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    profile->addWidget(m_debt);
    layout->addWidget(m_profile);

    m_payment = new QWidget(content);
    m_payment->setObjectName(QStringLiteral("accountPaymentCard"));
    m_payment->setMinimumWidth(0);
    m_payment->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* payment = column(m_payment, 16);
    m_paymentTitle = copyLabel(m_payment);
    m_paymentTitle->setObjectName(QStringLiteral("accountPurchaseHeading"));
    m_paymentTitle->setAlignment(Qt::AlignHCenter);
    m_paymentStatus = copyLabel(m_payment);
    m_paymentStatus->setObjectName(QStringLiteral("accountPurchaseStatus"));
    m_code = new QLabel(m_payment);
    m_code->setObjectName(QStringLiteral("accountMiniProgramCode"));
    m_code->setAlignment(Qt::AlignCenter);
    m_code->setFixedSize(208, 208);
    m_code->setAutoFillBackground(true);
    QPalette white = m_code->palette();
    white.setColor(QPalette::Window, Qt::white);
    m_code->setPalette(white);
    m_expiry = copyLabel(m_payment);
    m_expiry->setObjectName(QStringLiteral("accountPurchaseExpiry"));
    m_paymentHelp = copyLabel(m_payment);
    m_paymentNotice = copyLabel(m_payment);
    for (auto* label : {m_paymentStatus, m_expiry, m_paymentHelp, m_paymentNotice})
        label->setAlignment(Qt::AlignHCenter);
    m_paid = new AdButton(m_payment);
    m_paid->setObjectName(QStringLiteral("accountRefreshPayment"));
    m_paid->setButtonStyle(AdButton::ButtonStyle::Solid);
    m_paid->setAccentRole(AdButton::AccentRole::Primary);
    m_dismiss = new AdButton(m_payment);
    m_dismiss->setObjectName(QStringLiteral("accountDismissPurchase"));
    m_signInInstead = new AdButton(m_payment);
    m_signInInstead->setButtonStyle(AdButton::ButtonStyle::Outline);
    payment->addWidget(m_paymentTitle);
    payment->addWidget(m_paymentStatus);
    payment->addWidget(m_code, 0, Qt::AlignHCenter);
    payment->addWidget(m_expiry);
    payment->addWidget(m_paymentHelp);
    payment->addWidget(m_paymentNotice);
    auto* paymentActions = new QHBoxLayout;
    paymentActions->setSpacing(
        styles::ThemeManager::instance().themeColorScheme().metricAlias.paddingXS);
    for (auto* button : {m_paid, m_dismiss, m_signInInstead})
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    paymentActions->addWidget(m_paid, 1);
    paymentActions->addWidget(m_dismiss, 1);
    paymentActions->addWidget(m_signInInstead, 1);
    payment->addLayout(paymentActions);
    auto* paymentRow = new QHBoxLayout;
    paymentRow->setContentsMargins(0, 0, 0, 0);
    paymentRow->addStretch();
    paymentRow->addWidget(m_payment);
    paymentRow->addStretch();
    layout->addLayout(paymentRow);

    m_history = new QWidget(content);
    m_history->setObjectName(QStringLiteral("accountHistoryCard"));
    auto* history = column(m_history, 0);
    auto* historyHeading = new QHBoxLayout;
    m_historyTitle = copyLabel(m_history);
    m_refresh = new AdButton(m_history);
    m_refresh->setObjectName(QStringLiteral("accountRefreshTransactions"));
    m_refresh->setButtonStyle(AdButton::ButtonStyle::Text);
    m_refresh->setIconRef(adqt::icons::antd::outlined::Reload());
    historyHeading->addWidget(m_historyTitle, 1);
    historyHeading->addWidget(m_refresh);
    history->addLayout(historyHeading);
    m_historyDescription = copyLabel(m_history);
    history->addWidget(m_historyDescription);
    m_transactionHeader = new QWidget(m_history);
    m_transactionHeader->setObjectName(QStringLiteral("accountTransactionHeader"));
    auto* transactionHeading = new QGridLayout(m_transactionHeader);
    transactionHeading->setContentsMargins(0, 16, 0, 8);
    transactionHeading->setHorizontalSpacing(16);
    transactionColumns(transactionHeading);
    for (int index = 0; index < 4; ++index) {
        auto* label = copyLabel(m_transactionHeader);
        if (index == 3)
            label->setAlignment(Qt::AlignRight);
        transactionHeading->addWidget(label, 0, index);
    }
    history->addWidget(m_transactionHeader);
    m_records = new QWidget(m_history);
    m_records->setObjectName(QStringLiteral("accountTransactions"));
    column(m_records, 0);
    history->addWidget(m_records);
    m_more = new AdButton(m_history);
    history->addWidget(m_more, 0, Qt::AlignHCenter);
    layout->addWidget(m_history);
    layout->addStretch();

    connect(m_submit, &AdButton::clicked, this, &AccountPageWidget::submit);
    connect(m_agreement, &AdCheckbox::toggled, this, &AccountPageWidget::updateState);
    connect(m_username, &QLineEdit::returnPressed, this, &AccountPageWidget::submit);
    connect(m_password, &QLineEdit::returnPressed, this, &AccountPageWidget::submit);
    connect(m_confirmation, &QLineEdit::returnPressed, this, &AccountPageWidget::submit);
    connect(m_captchaAnswer, &QLineEdit::returnPressed, this, &AccountPageWidget::submit);
    connect(m_switch, &AdButton::clicked, this, [this] { showRegistration(!m_registration); });
    connect(m_password, &QLineEdit::textChanged, this, [this] {
        if (m_service)
            m_service->clearFieldError(QStringLiteral("password_confirmation"));
    });
    connect(m_captchaRefresh, &AdButton::clicked, this, [this] {
        if (m_service) {
            m_captchaAnswer->clear();
            m_service->loadCaptcha();
        }
    });
    connect(m_signInInstead, &AdButton::clicked, this, [this] {
        m_showAuthWithPurchase = true;
        showRegistration(false);
    });
    if (m_service) {
        connect(m_service, &AccountSessionService::changed, this, &AccountPageWidget::updateState);
        connect(m_service, &AccountSessionService::requestFailed, this,
                &AccountPageWidget::showRequestError);
        connect(m_service, &AccountSessionService::signedInChanged, this, [this](bool signedIn) {
            m_agreement->setChecked(false);
            m_password->clear();
            m_confirmation->clear();
            m_captchaAnswer->clear();
            if (!signedIn) {
                m_registration = false;
                m_showAuthWithPurchase = false;
                retranslateUi();
            }
        });
        connect(m_topUp, &AdButton::clicked, m_service, &AccountSessionService::beginTopUp);
        connect(m_signOut, &AdButton::clicked, m_service, &AccountSessionService::signOut);
        connect(m_paid, &AdButton::clicked, this, [this] { m_service->refreshPurchase(true); });
        connect(m_dismiss, &AdButton::clicked, this, [this] {
            const auto& purchase = m_service->purchase();
            if (purchase.pending()) {
                m_service->cancelPurchase();
                return;
            }
            const bool retryRegistration =
                purchase.registration() && !m_service->signedIn() &&
                purchase.request().value(QStringLiteral("state")).toString() != u"fulfilled";
            m_showAuthWithPurchase = false;
            if (retryRegistration) {
                m_registration = true;
                m_username->setText(
                    purchase.request().value(QStringLiteral("username")).toString());
                m_captchaAnswer->clear();
                retranslateUi();
            }
            m_service->dismissPurchase();
            if (retryRegistration) {
                m_service->loadCaptcha();
                m_username->setFocus();
            }
        });
        connect(m_more, &AdButton::clicked, this, [this] { m_service->loadTransactions(true); });
        connect(m_refresh, &AdButton::clicked, this, [this] {
            m_service->refreshAccount();
            m_service->loadTransactions();
        });
        m_service->refreshAccount();
        m_service->loadTransactions();
    }
    connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, this,
            [this] { applyTheme(); });
    auto* countdown = new QTimer(this);
    countdown->setInterval(1000);
    connect(countdown, &QTimer::timeout, this, &AccountPageWidget::updateState);
    countdown->start();
    retranslateUi();
    applyTheme();
    updateState();
}

void AccountPageWidget::showRegistration(bool value) {
    m_registration = value;
    m_agreement->setChecked(false);
    if (m_service) {
        m_service->clearError();
        if (value && m_service->captcha().isEmpty())
            m_service->loadCaptcha();
    }
    m_password->clear();
    m_confirmation->clear();
    m_captchaAnswer->clear();
    retranslateUi();
    updateState();
    m_username->setFocus();
}
void AccountPageWidget::submit() {
    if (!m_service || m_service->busy())
        return;
    if (m_registration) {
        if (!m_agreement->isChecked()) {
            m_container->scrollArea()->ensureWidgetVisible(m_agreementRow);
            m_agreement->setFocus();
            return;
        }
        m_service->registerAccount(m_username->text(), m_password->text(), m_confirmation->text(),
                                   m_captchaAnswer->text());
    } else {
        m_service->signIn(m_username->text(), m_password->text());
    }
    for (auto* item : m_form->items()) {
        if (!item->itemHidden() && item->validateStatus() == AdFormItem::ValidateStatus::Error) {
            m_container->scrollArea()->ensureWidgetVisible(item);
            item->controlWidget()->setFocus();
            break;
        }
    }
}

void AccountPageWidget::showRequestError() {
    if (!m_service || m_service->error().isEmpty())
        return;
    AdMessage::Request request;
    request.key = QStringLiteral("accountRequestError");
    request.content = m_service->error();
    m_errorMessage = AdMessageService::error(std::move(request), window());
}

void AccountPageWidget::retranslateUi() {
    setAccessibleName(tr("Account"));
    m_pageTitle->setText(tr("Account"));
    m_pageDescription->setText(tr("Manage and view account data"));
    m_authTitle->setText(m_registration ? tr("Create your account") : tr("Sign in to Snow Shot"));
    m_usernameField->setLabel(tr("Username"));
    m_passwordField->setLabel(tr("Password"));
    m_confirmationField->setLabel(tr("Confirm password"));
    m_captchaField->setLabel(tr("Image code"));
    m_username->setAccessibleName(tr("Username"));
    m_password->setAccessibleName(tr("Password"));
    m_confirmation->setAccessibleName(tr("Confirm password"));
    m_captchaAnswer->setAccessibleName(tr("Image code"));
    m_username->setPlaceholderText(tr("3–32 letters, numbers, _, . or -"));
    m_username->setAccessibleDescription(m_username->placeholderText());
    m_password->setPlaceholderText(tr("8–16 characters"));
    m_password->setAccessibleDescription(m_password->placeholderText());
    if (m_errorMessage && m_errorMessage->isOpen())
        showRequestError();
    m_captchaImage->setAccessibleName(tr("Registration image code"));
    m_captchaRefresh->setText(tr("Refresh code"));
    const QString agreement = tr("User agreement");
    const QString privacy = tr("Privacy policy");
    const QString consent = tr("I have read and agree to the %1 and %2");
    const auto link = [](const QString& url, const QString& title) {
        return QStringLiteral("<a href=\"%1\">%2</a>")
            .arg(url.toHtmlEscaped(), title.toHtmlEscaped());
    };
    m_agreement->setAccessibleName(consent.arg(agreement, privacy));
    m_agreementText->setText(consent.toHtmlEscaped().arg(
        link(snow_shot::app::distribution::userAgreementUrl(), agreement),
        link(snow_shot::app::distribution::privacyPolicyUrl(), privacy)));
    m_submit->setText(m_registration ? tr("Register and continue") : tr("Sign in"));
    m_switch->setText(m_registration ? tr("Already have an account? Sign in")
                                     : tr("Create an account"));
    m_credit->parentWidget()->setToolTip(tr("Available credits"));
    m_topUp->setText(tr("Purchase credits"));
    m_signOut->setText(tr("Sign out"));
    m_paid->setText(tr("I've paid"));
    m_dismiss->setText(tr("Continue"));
    m_signInInstead->setText(tr("Sign in to an existing account"));
    m_historyTitle->setText(tr("Account transactions"));
    m_historyDescription->setText(tr("Top-ups, usage charges, and refunds"));
    const QStringList headings = {tr("Transaction"), tr("Date"), tr("Status"), tr("Credits")};
    const auto headerLabels = m_transactionHeader->findChildren<QLabel*>();
    for (qsizetype index = 0; index < headerLabels.size(); ++index)
        headerLabels.at(index)->setText(headings.at(index));
    m_refresh->setText(tr("Refresh"));
    m_more->setText(tr("Load more"));
    m_code->setAccessibleName(tr("Scan this Mini Program code with WeChat"));
    m_renderedTransactions = {};
    rebuildTransactions();
}

void AccountPageWidget::updateState() {
    const bool signedIn = m_service && m_service->signedIn();
    const bool busy = m_service && m_service->busy();
    const bool hasRequest = m_service && !m_service->purchase().request().isEmpty();
    m_pageTitle->parentWidget()->setVisible(signedIn || hasRequest);
    const QString requestId =
        hasRequest ? m_service->purchase().request().value(QStringLiteral("request_id")).toString()
                   : QString();
    const bool newRegistration =
        hasRequest && m_service->purchase().registration() && requestId != m_displayedRequest;
    m_displayedRequest = requestId;
    if (newRegistration && !signedIn) {
        m_registration = true;
        m_showAuthWithPurchase = false;
        m_username->setText(
            m_service->purchase().request().value(QStringLiteral("username")).toString());
        m_captchaAnswer->clear();
        retranslateUi();
    }
    m_auth->setVisible(!signedIn && (!hasRequest || m_showAuthWithPurchase));
    m_profile->setVisible(signedIn);
    m_history->setVisible(signedIn);
    m_payment->setVisible(hasRequest);
    m_confirmationField->setItemHidden(!m_registration);
    m_captchaField->setItemHidden(!m_registration);
    m_captchaRow->setVisible(m_registration);
    m_agreementRow->setVisible(m_registration);
    m_agreement->setEnabled(!busy);
    m_submit->setEnabled(m_service && !busy && (!m_registration || m_agreement->isChecked()));
    m_submit->setBusy(busy);
    m_switch->setEnabled(!busy && (!m_service || !m_service->purchase().pending()));
    m_username->setEnabled(!busy);
    m_password->setEnabled(!busy);
    m_confirmation->setEnabled(!busy);
    m_captchaAnswer->setEnabled(!busy);
    m_captchaRefresh->setEnabled(!busy);
    if (m_errorMessage && (!m_service || m_service->error().isEmpty()))
        m_errorMessage->close();
    for (auto* item : m_form->items()) {
        const QString error = m_service ? m_service->fieldError(item->fieldKey()) : QString();
        item->setValidateStatus(error.isEmpty() ? AdFormItem::ValidateStatus::None
                                                : AdFormItem::ValidateStatus::Error);
        item->setHelpText(error);
        auto* editor = qobject_cast<AdLineEdit*>(item->controlWidget());
        editor->setStatus(error.isEmpty() ? AdLineEdit::Status::None : AdLineEdit::Status::Error);
        editor->setAccessibleDescription(error.isEmpty() ? editor->placeholderText() : error);
    }
    if (!m_service) {
        m_notice->setText(tr("The account service is unavailable"));
        m_notice->show();
        return;
    }
    const auto account = m_service->account();
    const bool paymentsAvailable = account.value(QStringLiteral("payments_available")).toBool(true);
    const QString notice = !m_service->storageNotice().isEmpty() ? m_service->storageNotice()
                           : signedIn && !paymentsAvailable
                               ? tr("Purchases are currently unavailable. Your account and "
                                    "transaction records are still available")
                               : QString();
    m_notice->setText(notice);
    m_notice->setVisible(!notice.isEmpty());
    m_accountName->setText(account.value(QStringLiteral("username")).toString());
    m_cumulativeRecharge->setText(
        tr("Total top-ups: %1")
            .arg(AccountSessionService::formatCredits(
                account.value(QStringLiteral("cumulative_recharge_nano_yuan")).toString())));
    const QString credit = account.value(QStringLiteral("available_credit_nano_yuan")).toString();
    m_credit->setText(AccountSessionService::formatCreditAmount(credit));
    m_credit->setAccessibleName(tr("Available credits"));
    m_credit->setAccessibleDescription(AccountSessionService::formatCredits(credit));
    const QString debt = account.value(QStringLiteral("outstanding_balance_nano_yuan")).toString();
    m_debt->setVisible(debt.toLongLong() > 0);
    m_debt->setText(tr("Outstanding balance: %1").arg(AccountSessionService::formatCredits(debt)));
    m_debt->setInformativeText(tr("Paid calls are paused. Your free allowance remains available. "
                                  "New purchases pay this balance first"));
    m_topUp->setEnabled(!busy && paymentsAvailable && !m_service->purchase().pending());
    m_topUp->setBusy(busy);
    const QString captcha = m_service->captcha().value(QStringLiteral("image_data_url")).toString();
    if (captcha != m_captchaData) {
        m_captchaData = captcha;
        imageFromData(m_captchaImage, captcha, m_captchaImage->contentsRect().size());
    }
    if (hasRequest) {
        auto& purchase = m_service->purchase();
        const auto request = purchase.request();
        const QString state = request.value(QStringLiteral("state")).toString();
        const QString username = request.value(QStringLiteral("username")).toString();
        m_paymentTitle->setText(purchase.registration()
                                    ? tr("Activate %1").arg(username)
                                    : tr("Purchase credits for %1").arg(username));
        QString status;
        if (state == u"awaiting_payment")
            status = tr("Waiting for payment");
        else if (state == u"payment_pending")
            status = tr("Confirming payment");
        else if (state == u"fulfilled")
            status = request.value(QStringLiteral("completion_kind")).toString() == u"account_link"
                         ? tr("Account linked")
                         : tr("Payment received");
        else if (state == u"expired")
            status = tr("This purchase code has expired");
        else if (state == u"failed")
            status = tr("Payment failed or the order was closed");
        else
            status = tr("The payment result is still being checked");
        m_paymentStatus->setText(status);
        const QString code = request.value(QStringLiteral("mini_program_code_data_url")).toString();
        if (code != m_codeData) {
            m_codeData = code;
            imageFromData(m_code, code, QSize(192, 192));
        }
        m_code->setVisible(state == u"awaiting_payment" && !code.isEmpty());
        if (state == u"awaiting_payment") {
            const auto seconds = purchase.remainingSeconds();
            m_expiry->setText(tr("Code valid for %1:%2 · Valid until %3")
                                  .arg(seconds / 60)
                                  .arg(seconds % 60, 2, 10, QLatin1Char('0'))
                                  .arg(QDateTime::fromString(
                                           request.value(QStringLiteral("expires_at")).toString(),
                                           Qt::ISODateWithMs)
                                           .toLocalTime()
                                           .toString(QStringLiteral("HH:mm:ss"))));
        } else if (purchase.pending()) {
            m_expiry->setText(
                tr("Payment confirmation continues after the code's original expiry"));
        } else {
            m_expiry->clear();
        }
        m_paymentHelp->setText(
            state == u"expired"
                ? tr("Return to registration and generate a new code. Your previous "
                     "order records are retained")
                : tr("Scan with your own WeChat account. An existing temporary account will be "
                     "linked and signed in automatically using these desktop credentials. "
                     "Otherwise, select a package and confirm your order. The WeChat link cannot "
                     "be changed"));
        m_paymentNotice->setText(purchase.notice());
        m_paymentNotice->setVisible(!purchase.notice().isEmpty());
        m_paid->setVisible(purchase.pending());
        m_paid->setEnabled((purchase.registration() || signedIn) && !purchase.queryingPlatform() &&
                           !purchase.cancelling());
        m_paid->setBusy(purchase.queryingPlatform());
        m_dismiss->setText(purchase.pending() ? tr("Cancel payment") : tr("Continue"));
        m_dismiss->setVisible(!purchase.pending() || !purchase.registration());
        m_dismiss->setEnabled((!purchase.pending() || purchase.registration() || signedIn) &&
                              !purchase.cancelling());
        m_dismiss->setBusy(purchase.cancelling());
        m_signInInstead->setVisible(!signedIn && !m_showAuthWithPurchase);
    }
    m_more->setVisible(m_service->hasMoreTransactions());
    if (m_renderedTransactions != m_service->transactions())
        rebuildTransactions();
    updateCompactGeometry();
}

void AccountPageWidget::rebuildTransactions() {
    auto* layout = static_cast<QVBoxLayout*>(m_records->layout());
    while (auto* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_renderedTransactions = m_service ? m_service->transactions() : QJsonArray();
    m_records->setMinimumHeight(m_renderedTransactions.isEmpty() ? 260 : 0);
    if (m_renderedTransactions.isEmpty()) {
        m_emptyIcon = new QLabel(m_records);
        m_emptyIcon->setObjectName(QStringLiteral("accountTransactionsEmptyIcon"));
        m_emptyIcon->setAlignment(Qt::AlignCenter);
        m_emptyIcon->setFixedSize(96, 62);
        m_emptyTitle = copyLabel(m_records);
        m_emptyTitle->setObjectName(QStringLiteral("accountTransactionsEmpty"));
        m_emptyTitle->setText(tr("No transactions yet"));
        m_emptyTitle->setAlignment(Qt::AlignCenter);
        m_emptyDescription = copyLabel(m_records);
        m_emptyDescription->setObjectName(QStringLiteral("accountTransactionsEmptyDescription"));
        m_emptyDescription->setText(tr("Top-ups, usage charges, and refunds appear here"));
        m_emptyDescription->setAlignment(Qt::AlignCenter);
        layout->addSpacing(48);
        layout->addStretch(1);
        layout->addWidget(m_emptyIcon, 0, Qt::AlignHCenter);
        layout->addSpacing(18);
        layout->addWidget(m_emptyTitle);
        layout->addSpacing(8);
        layout->addWidget(m_emptyDescription);
        layout->addStretch(1);
        layout->addSpacing(48);
    }
    for (const auto& value : m_renderedTransactions) {
        const auto transaction = value.toObject();
        const QString kind = transaction.value(QStringLiteral("type")).toString();
        const QString type = kind == u"top_up"   ? tr("Top-up")
                             : kind == u"refund" ? tr("Refund")
                                                 : tr("Usage");
        const QString state = transaction.value(QStringLiteral("status")).toString();
        const QString status = state == u"completed" ? tr("Completed")
                               : state == u"failed"  ? tr("Failed")
                               : kind == u"refund"   ? tr("Processing")
                                                     : tr("Pending");
        layout->addWidget(new AccountTransactionRow(transaction, type, status, m_records));
    }
    applyTheme();
}

void AccountPageWidget::applyTheme() {
    const auto scheme = styles::ThemeManager::instance().themeColorScheme();
    QPalette primary = palette();
    primary.setColor(QPalette::WindowText, scheme.map.colorText);
    setPalette(primary);
    QPalette muted = primary;
    muted.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
    QPalette agreementPalette = primary;
    agreementPalette.setColor(QPalette::Link, scheme.map.colorLink);
    m_agreementText->setPalette(agreementPalette);
    for (auto* label : {m_pageDescription, m_cumulativeRecharge, m_expiry, m_paymentHelp,
                        m_paymentNotice, m_historyDescription})
        label->setPalette(muted);
    QFont heading = font();
    heading.setPixelSize(scheme.metricAlias.fontSizeHeading4);
    heading.setWeight(QFont::DemiBold);
    for (auto* label : {m_pageTitle, m_authTitle, m_accountName, m_paymentTitle})
        label->setFont(heading);
    QFont description = font();
    description.setPixelSize(scheme.metricAlias.fontSize);
    description.setWeight(QFont::Normal);
    m_pageDescription->setFont(description);
    m_agreementText->setFont(description);
    m_agreementRow->layout()->setSpacing(scheme.metricAlias.paddingXS);
    heading.setPixelSize(scheme.metricAlias.fontSizeHeading5);
    m_historyTitle->setFont(heading);
    QFont balance = heading;
    balance.setPixelSize(scheme.metricAlias.fontSizeHeading1);
    m_credit->setFont(balance);
    const int coinSize = scheme.metricAlias.fontSizeHeading1;
    m_creditIcon->setFixedSize(coinSize, coinSize);
    adqt::icons::IconRenderRequest coinRequest;
    coinRequest.logicalSize = m_creditIcon->size();
    coinRequest.devicePixelRatio = m_creditIcon->devicePixelRatioF();
    m_creditIcon->setPixmap(adqt::icons::renderIconPixmap(
        snow_shot::presentation::icons::custom::outlined::CreditsCoin(
            adqt::icons::IconColors::primary(scheme.map.colorPrimaryText)),
        coinRequest));
    m_credit->parentWidget()->layout()->setSpacing(scheme.metricAlias.marginXS);
    m_credit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_accountName->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                           Qt::TextSelectableByKeyboard);
    QPalette creditPalette = primary;
    creditPalette.setColor(QPalette::WindowText, scheme.map.colorPrimaryText);
    m_credit->setPalette(creditPalette);
    QFont detail = font();
    detail.setPixelSize(scheme.metricAlias.fontSizeSM);
    for (auto* label : {m_expiry, m_paymentHelp, m_paymentNotice, m_historyDescription})
        label->setFont(detail);
    for (auto* label : m_transactionHeader->findChildren<QLabel*>()) {
        label->setPalette(muted);
        label->setFont(detail);
    }
    m_records->layout()->setSpacing(m_renderedTransactions.isEmpty() ? scheme.metricAlias.marginSM
                                                                     : 0);
    if (m_emptyIcon) {
        m_emptyIcon->setPixmap(snow_shot::presentation::components::renderEmptyStateIcon(
            scheme, m_emptyIcon->size(), m_emptyIcon->devicePixelRatioF()));
        heading.setPixelSize(scheme.metricAlias.fontSizeLG);
        m_emptyTitle->setFont(heading);
        m_emptyTitle->setPalette(primary);
        m_emptyDescription->setFont(description);
        m_emptyDescription->setPalette(muted);
    }
    const auto& metric = scheme.metricAlias;
    m_history->layout()->setContentsMargins(0, metric.marginLG, 0, 0);
    auto* headingLayout = m_pageTitle->parentWidget()->layout();
    headingLayout->setContentsMargins(0, metric.marginMD, 0, metric.marginSM);
    headingLayout->setSpacing(metric.marginXXS);
    updateCompactGeometry();
}
void AccountPageWidget::updateCompactGeometry() {
    const auto& metric = styles::ThemeManager::instance().themeColorScheme().metricAlias;
    const int cardPadding = width() < 480 ? metric.padding : metric.paddingLG;
    const int topPadding =
        m_pageTitle->parentWidget()->isHidden() ? metric.paddingLG : metric.paddingXXS;
    m_container->contentLayout()->setContentsMargins(metric.paddingLG, topPadding, metric.paddingLG,
                                                     metric.paddingLG);
    const auto margins = m_container->contentLayout()->contentsMargins();
    const int contentWidth = std::max(144, width() - margins.left() - margins.right() -
                                               metric.scrollbarThickness - metric.scrollbarMargin);
    auto* authCard = m_auth->findChild<QWidget*>(QStringLiteral("accountAuthCard"));
    authCard->setFixedWidth(std::min(440, contentWidth));
    m_payment->setFixedWidth(std::min(560, contentWidth));
    for (auto* card : {authCard, m_payment})
        card->layout()->setContentsMargins(cardPadding, cardPadding, cardPadding, cardPadding);
    // Preserve the alerts' natural wrapped height before the scroll container
    // distributes space between sections, including after font-size changes.
    const int pageContentWidth = std::max(144, width() - 2 * metric.paddingLG);
    m_notice->setMinimumHeight(std::max(0, m_notice->heightForWidth(pageContentWidth)));
    m_debt->setMinimumHeight(std::max(0, m_debt->heightForWidth(pageContentWidth)));
    const bool compact = contentWidth < metric.fontSize * 38;
    m_identityLayout->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_balanceLayout->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_transactionHeader->setVisible(contentWidth >= metric.fontSize * 42 &&
                                    !m_renderedTransactions.isEmpty());
    const int available = std::max(112, m_payment->width() - 2 * cardPadding);
    if (m_service && m_service->purchase().pending()) {
        m_dismiss->setText(tr("Cancel payment"));
        if (m_dismiss->sizeHint().width() > (available - metric.paddingXS) / 2)
            m_dismiss->setText(tr("Cancel"));
    }
    m_signInInstead->setText(tr("Sign in to an existing account"));
    if (m_signInInstead->sizeHint().width() > (available - 12) / 2)
        m_signInInstead->setText(tr("Sign in"));
    if (m_registration)
        m_switch->setText(available < 280 ? tr("Sign in") : tr("Already have an account? Sign in"));
    const int codeSize = std::min(208, available);
    m_code->setFixedSize(codeSize, codeSize);
    if (!m_codeData.isEmpty())
        imageFromData(m_code, m_codeData, QSize(codeSize - 16, codeSize - 16));
    const int authWidth = std::max(112, authCard->width() - 2 * cardPadding);
    const int captchaWidth =
        std::clamp(authWidth - m_captchaRefresh->sizeHint().width() - 12, 64, 160);
    // The viewport owns its geometry; asynchronous images and fractional DPR
    // must never make its pixmap taller than the row allocated by the layout.
    m_captchaImage->setFixedSize(captchaWidth, 64);
    if (!m_captchaData.isEmpty())
        imageFromData(m_captchaImage, m_captchaData, m_captchaImage->contentsRect().size());
}
void AccountPageWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateCompactGeometry();
}
void AccountPageWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        updateState();
    } else if (event->type() == QEvent::DevicePixelRatioChange) {
        applyTheme();
    }
}
