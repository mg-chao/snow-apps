#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_ACCOUNTPAGEWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_ACCOUNTPAGEWIDGET_H

#include <QJsonArray>
#include <QPointer>
#include <QWidget>

class QLabel;
class QBoxLayout;
class QVBoxLayout;
class PageContainerWidget;
namespace adqt::widgets {
class AdAlert;
class AdButton;
class AdCheckbox;
class AdForm;
class AdFormItem;
class AdLineEdit;
class AdMessageHandle;
class AdPasswordEdit;
} // namespace adqt::widgets
namespace snow_shot::app {
class AccountSessionService;
}

class AccountPageWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit AccountPageWidget(snow_shot::app::AccountSessionService* service,
                               QWidget* parent = nullptr);

  protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void updateState();
    void retranslateUi();
    void applyTheme();
    void updateCompactGeometry();
    void submit();
    void showRegistration(bool value);
    void showRequestError();
    void rebuildTransactions();

    QPointer<snow_shot::app::AccountSessionService> m_service;
    PageContainerWidget* m_container = nullptr;
    QLabel* m_pageTitle = nullptr;
    QLabel* m_pageDescription = nullptr;
    QPointer<adqt::widgets::AdMessageHandle> m_errorMessage;
    adqt::widgets::AdAlert* m_notice = nullptr;
    adqt::widgets::AdAlert* m_debt = nullptr;
    QWidget* m_auth = nullptr;
    QLabel* m_authTitle = nullptr;
    adqt::widgets::AdForm* m_form = nullptr;
    adqt::widgets::AdFormItem* m_usernameField = nullptr;
    adqt::widgets::AdFormItem* m_passwordField = nullptr;
    adqt::widgets::AdFormItem* m_confirmationField = nullptr;
    adqt::widgets::AdFormItem* m_captchaField = nullptr;
    adqt::widgets::AdLineEdit* m_username = nullptr;
    adqt::widgets::AdPasswordEdit* m_password = nullptr;
    adqt::widgets::AdPasswordEdit* m_confirmation = nullptr;
    adqt::widgets::AdLineEdit* m_captchaAnswer = nullptr;
    QLabel* m_captchaImage = nullptr;
    QWidget* m_captchaRow = nullptr;
    adqt::widgets::AdButton* m_captchaRefresh = nullptr;
    QWidget* m_agreementRow = nullptr;
    adqt::widgets::AdCheckbox* m_agreement = nullptr;
    QLabel* m_agreementText = nullptr;
    adqt::widgets::AdButton* m_submit = nullptr;
    adqt::widgets::AdButton* m_switch = nullptr;
    QWidget* m_profile = nullptr;
    QBoxLayout* m_identityLayout = nullptr;
    QBoxLayout* m_balanceLayout = nullptr;
    QLabel* m_accountName = nullptr;
    QLabel* m_cumulativeRecharge = nullptr;
    QLabel* m_creditIcon = nullptr;
    QLabel* m_credit = nullptr;
    adqt::widgets::AdButton* m_topUp = nullptr;
    adqt::widgets::AdButton* m_signOut = nullptr;
    QWidget* m_payment = nullptr;
    QLabel* m_paymentTitle = nullptr;
    QLabel* m_code = nullptr;
    QLabel* m_paymentStatus = nullptr;
    QLabel* m_expiry = nullptr;
    QLabel* m_paymentHelp = nullptr;
    QLabel* m_paymentNotice = nullptr;
    adqt::widgets::AdButton* m_paid = nullptr;
    adqt::widgets::AdButton* m_dismiss = nullptr;
    adqt::widgets::AdButton* m_signInInstead = nullptr;
    QWidget* m_history = nullptr;
    QLabel* m_historyTitle = nullptr;
    QLabel* m_historyDescription = nullptr;
    QWidget* m_transactionHeader = nullptr;
    QWidget* m_records = nullptr;
    QPointer<QLabel> m_emptyIcon;
    QPointer<QLabel> m_emptyTitle;
    QPointer<QLabel> m_emptyDescription;
    adqt::widgets::AdButton* m_refresh = nullptr;
    adqt::widgets::AdButton* m_more = nullptr;
    QJsonArray m_renderedTransactions;
    QString m_captchaData;
    QString m_codeData;
    QString m_displayedRequest;
    bool m_registration = false;
    bool m_showAuthWithPurchase = false;
};

#endif
