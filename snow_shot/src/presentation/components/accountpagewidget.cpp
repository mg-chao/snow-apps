#include "snow_shot/presentation/components/accountpagewidget.h"

#include "snow_shot/app/accountsessionservice.h"
#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_password_edit.h"
#include "widgets/message.h"
#include "widgets/scroll_area.h"

#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
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
    layout->setSpacing(24);
    m_notice = new AdAlert(content);
    m_notice->setSeverity(AdAlert::Severity::Info);
    m_notice->setAnimated(false);
    layout->addWidget(m_notice);

    m_auth = new QWidget(content);
    m_auth->setMaximumWidth(400);
    auto* auth = column(m_auth);
    m_authTitle = copyLabel(m_auth);
    m_authTitle->setObjectName(QStringLiteral("accountAuthHeading"));
    auth->addWidget(m_authTitle);
    m_form = new AdForm(m_auth);
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
    m_username = new AdLineEdit(m_auth);
    m_username->setMaxLength(32);
    m_usernameField =
        field(QStringLiteral("accountUsername"), QStringLiteral("username"), m_username);
    m_password = new AdPasswordEdit(m_auth);
    m_password->setMaxLength(16);
    m_passwordField =
        field(QStringLiteral("accountPassword"), QStringLiteral("password"), m_password);
    m_confirmation = new AdPasswordEdit(m_auth);
    m_confirmation->setMaxLength(16);
    m_confirmationField = field(QStringLiteral("accountPasswordConfirmation"),
                                QStringLiteral("password_confirmation"), m_confirmation);
    m_captchaAnswer = new AdLineEdit(m_auth);
    m_captchaAnswer->setMaxLength(4);
    m_captchaField = field(QStringLiteral("accountCaptchaAnswer"), QStringLiteral("captcha_answer"),
                           m_captchaAnswer);
    m_captchaRow = new QWidget(m_auth);
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
    m_submit = new AdButton(m_auth);
    m_submit->setObjectName(QStringLiteral("accountSubmit"));
    m_submit->setButtonStyle(AdButton::ButtonStyle::Solid);
    m_submit->setAccentRole(AdButton::AccentRole::Primary);
    auth->addWidget(m_submit);
    m_switch = new AdButton(m_auth);
    m_switch->setObjectName(QStringLiteral("accountSwitchForm"));
    m_switch->setButtonStyle(AdButton::ButtonStyle::Outline);
    auth->addWidget(m_switch);
    auto* authRow = new QHBoxLayout;
    authRow->addWidget(m_auth);
    layout->addLayout(authRow);

    m_profile = new QWidget(content);
    auto* profile = column(m_profile);
    auto* identity = new QHBoxLayout;
    m_accountName = copyLabel(m_profile);
    m_accountName->setObjectName(QStringLiteral("accountDisplayUsername"));
    m_signOut = new AdButton(m_profile);
    m_signOut->setButtonStyle(AdButton::ButtonStyle::Text);
    identity->addWidget(m_accountName, 1);
    identity->addWidget(m_signOut);
    profile->addLayout(identity);
    m_membership = copyLabel(m_profile);
    profile->addWidget(m_membership);
    m_creditLabel = copyLabel(m_profile);
    m_credit = copyLabel(m_profile);
    m_credit->setObjectName(QStringLiteral("accountAvailableCredit"));
    profile->addWidget(m_creditLabel);
    profile->addWidget(m_credit);
    m_permanent = copyLabel(m_profile);
    profile->addWidget(m_permanent);
    m_debt = new AdAlert(m_profile);
    m_debt->setObjectName(QStringLiteral("accountOutstandingBalance"));
    m_debt->setSeverity(AdAlert::Severity::Warning);
    m_debt->setAnimated(false);
    profile->addWidget(m_debt);
    m_topUp = new AdButton(m_profile);
    m_topUp->setObjectName(QStringLiteral("accountTopUp"));
    m_topUp->setButtonStyle(AdButton::ButtonStyle::Solid);
    m_topUp->setAccentRole(AdButton::AccentRole::Primary);
    profile->addWidget(m_topUp, 0, Qt::AlignLeft);
    layout->addWidget(m_profile);

    m_payment = new QWidget(content);
    auto* payment = column(m_payment);
    m_paymentTitle = copyLabel(m_payment);
    m_paymentTitle->setObjectName(QStringLiteral("accountPurchaseHeading"));
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
    m_paid = new AdButton(m_payment);
    m_paid->setObjectName(QStringLiteral("accountRefreshPayment"));
    m_dismiss = new AdButton(m_payment);
    m_dismiss->setObjectName(QStringLiteral("accountDismissPurchase"));
    m_signInInstead = new AdButton(m_payment);
    m_signInInstead->setButtonStyle(AdButton::ButtonStyle::Link);
    payment->addWidget(m_paymentTitle);
    payment->addWidget(m_paymentStatus);
    payment->addWidget(m_code, 0, Qt::AlignLeft);
    payment->addWidget(m_expiry);
    payment->addWidget(m_paymentHelp);
    payment->addWidget(m_paymentNotice);
    payment->addWidget(m_paid, 0, Qt::AlignLeft);
    payment->addWidget(m_dismiss, 0, Qt::AlignLeft);
    payment->addWidget(m_signInInstead, 0, Qt::AlignLeft);
    layout->addWidget(m_payment);

    m_history = new QWidget(content);
    auto* history = column(m_history);
    auto* historyHeading = new QHBoxLayout;
    m_historyTitle = copyLabel(m_history);
    m_refresh = new AdButton(m_history);
    m_refresh->setButtonStyle(AdButton::ButtonStyle::Text);
    historyHeading->addWidget(m_historyTitle, 1);
    historyHeading->addWidget(m_refresh);
    history->addLayout(historyHeading);
    m_records = new QWidget(m_history);
    m_records->setObjectName(QStringLiteral("accountTransactions"));
    column(m_records, 16);
    history->addWidget(m_records);
    m_more = new AdButton(m_history);
    history->addWidget(m_more, 0, Qt::AlignLeft);
    layout->addWidget(m_history);
    layout->addStretch();

    connect(m_submit, &AdButton::clicked, this, &AccountPageWidget::submit);
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
            m_service->dismissPurchase();
            if (m_registration && !m_service->signedIn())
                m_service->loadCaptcha();
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
    m_submit->setText(m_registration ? tr("Register and continue") : tr("Sign in"));
    m_switch->setText(m_registration ? tr("Already have an account? Sign in")
                                     : tr("Create an account"));
    m_creditLabel->setText(tr("Available credits"));
    m_permanent->setText(tr("Membership and purchased credits never expire."));
    m_topUp->setText(tr("Purchase credits"));
    m_signOut->setText(tr("Sign out"));
    m_paid->setText(tr("I've Paid"));
    m_dismiss->setText(tr("Continue"));
    m_signInInstead->setText(tr("Sign in to an existing account"));
    m_historyTitle->setText(tr("Account transactions"));
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
    m_auth->setVisible(!signedIn && (!hasRequest || m_showAuthWithPurchase));
    m_profile->setVisible(signedIn);
    m_history->setVisible(signedIn);
    m_payment->setVisible(hasRequest);
    m_confirmationField->setItemHidden(!m_registration);
    m_captchaField->setItemHidden(!m_registration);
    m_captchaRow->setVisible(m_registration);
    m_submit->setEnabled(m_service && !busy);
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
        m_notice->setText(tr("The account service is unavailable."));
        m_notice->show();
        return;
    }
    const auto account = m_service->account();
    const bool paymentsAvailable = account.value(QStringLiteral("payments_available")).toBool(true);
    const QString notice = !m_service->storageNotice().isEmpty() ? m_service->storageNotice()
                           : signedIn && !paymentsAvailable
                               ? tr("Purchases are currently unavailable. Your account and "
                                    "transaction records are still available.")
                               : QString();
    m_notice->setText(notice);
    m_notice->setVisible(!notice.isEmpty());
    m_accountName->setText(account.value(QStringLiteral("username")).toString());
    m_membership->setText(tr("Active membership · No expiration"));
    m_credit->setText(AccountSessionService::formatMoney(
        account.value(QStringLiteral("available_credit_nano_yuan")).toString()));
    const QString debt = account.value(QStringLiteral("outstanding_balance_nano_yuan")).toString();
    m_debt->setVisible(debt.toLongLong() > 0);
    m_debt->setText(tr("Outstanding balance: %1").arg(AccountSessionService::formatMoney(debt)));
    m_debt->setInformativeText(tr("Paid calls are paused. Your free allowance remains available. "
                                  "New purchases pay this balance first."));
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
                tr("Payment confirmation continues after the code's original expiry."));
        } else {
            m_expiry->clear();
        }
        m_paymentHelp->setText(
            state == u"expired"
                ? tr("Return to registration and generate a new code. Your previous "
                     "order records are retained.")
                : tr("Scan with your own WeChat account. An existing temporary account will be "
                     "linked and signed in automatically using these desktop credentials. "
                     "Otherwise, select a package and confirm your order. The WeChat link cannot "
                     "be changed."));
        m_paymentNotice->setText(purchase.notice());
        m_paymentNotice->setVisible(!purchase.notice().isEmpty());
        m_paid->setVisible(purchase.pending());
        m_paid->setEnabled(!purchase.polling());
        m_paid->setBusy(purchase.polling());
        m_dismiss->setVisible(!purchase.pending());
        m_signInInstead->setVisible(!signedIn && !m_showAuthWithPurchase);
        const QString requestId = request.value(QStringLiteral("request_id")).toString();
        if (purchase.registration() && requestId != m_displayedRequest) {
            m_password->clear();
            m_confirmation->clear();
            m_captchaAnswer->clear();
        }
        m_displayedRequest = requestId;
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
    if (m_renderedTransactions.isEmpty()) {
        auto* empty = copyLabel(m_records);
        empty->setText(tr("No transactions yet. Top-ups, usage charges, and refunds appear here."));
        layout->addWidget(empty);
    }
    for (const auto& value : m_renderedTransactions) {
        const auto transaction = value.toObject();
        auto* row = new QWidget(m_records);
        row->setObjectName(QStringLiteral("accountTransaction-") +
                           transaction.value(QStringLiteral("id")).toString());
        auto* contents = column(row, 4);
        auto* heading = new QHBoxLayout;
        auto* type = copyLabel(row);
        const QString kind = transaction.value(QStringLiteral("type")).toString();
        type->setText(kind == u"top_up"   ? tr("Top-up")
                      : kind == u"refund" ? tr("Refund")
                                          : tr("Usage"));
        auto* amount =
            new QLabel(AccountSessionService::formatMoney(
                           transaction.value(QStringLiteral("amount_nano_yuan")).toString()),
                       row);
        amount->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        heading->addWidget(type, 1);
        heading->addWidget(amount);
        contents->addLayout(heading);
        auto* details = copyLabel(row);
        const QString state = transaction.value(QStringLiteral("status")).toString();
        const QString status = state == u"completed" ? tr("Completed")
                               : state == u"failed"  ? tr("Failed")
                               : kind == u"refund"   ? tr("Processing")
                                                     : tr("Pending");
        const auto date = QDateTime::fromString(
            transaction.value(QStringLiteral("occurred_at")).toString(), Qt::ISODateWithMs);
        details->setText(date.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) +
                         QStringLiteral(" · ") + status);
        contents->addWidget(details);
        const QString orderId = transaction.value(QStringLiteral("order_id")).toString();
        if (!orderId.isEmpty()) {
            auto* order = copyLabel(row);
            order->setText(tr("Order ID: %1").arg(orderId));
            order->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                           Qt::TextSelectableByKeyboard);
            contents->addWidget(order);
        }
        layout->addWidget(row);
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
    for (auto* label :
         {m_membership, m_creditLabel, m_permanent, m_expiry, m_paymentHelp, m_paymentNotice})
        label->setPalette(muted);
    QFont heading = font();
    heading.setPixelSize(scheme.metricAlias.fontSizeHeading4);
    heading.setWeight(QFont::DemiBold);
    for (auto* label : {m_authTitle, m_accountName, m_paymentTitle, m_historyTitle})
        label->setFont(heading);
    QFont balance = heading;
    balance.setPixelSize(scheme.metricAlias.fontSizeHeading2);
    m_credit->setFont(balance);
    const auto& metric = scheme.metricAlias;
    m_container->contentLayout()->setContentsMargins(metric.paddingLG, metric.paddingLG,
                                                     metric.paddingLG, metric.paddingLG);
    m_container->contentLayout()->setSpacing(metric.paddingLG);
    updateCompactGeometry();
}
void AccountPageWidget::updateCompactGeometry() {
    const auto& metric = styles::ThemeManager::instance().themeColorScheme().metricAlias;
    const int available = std::max(144, width() - 2 * metric.paddingLG - 12);
    m_signInInstead->setText(available < 280 ? tr("Sign in")
                                             : tr("Sign in to an existing account"));
    if (m_registration)
        m_switch->setText(available < 280 ? tr("Sign in") : tr("Already have an account? Sign in"));
    const int codeSize = std::min(208, available);
    m_code->setFixedSize(codeSize, codeSize);
    if (!m_codeData.isEmpty())
        imageFromData(m_code, m_codeData, QSize(codeSize - 16, codeSize - 16));
    const int captchaWidth =
        std::clamp(available - m_captchaRefresh->sizeHint().width() - 12, 80, 160);
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
        updateCompactGeometry();
    }
}
