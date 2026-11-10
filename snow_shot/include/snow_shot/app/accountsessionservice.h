#ifndef SNOW_SHOT_APP_ACCOUNTSESSIONSERVICE_H
#define SNOW_SHOT_APP_ACCOUNTSESSIONSERVICE_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class SnowShotApiClient;

namespace snow_shot::storage {
class AccountSecretStore;
}
namespace snow_shot::app {
class AccountSessionService;

class AccountPurchaseCoordinator final : public QObject {
    Q_OBJECT
  public:
    explicit AccountPurchaseCoordinator(QObject* parent = nullptr);
    [[nodiscard]] const QJsonObject& request() const {
        return m_request;
    }
    [[nodiscard]] bool pending() const;
    [[nodiscard]] bool polling() const {
        return m_pollBusy;
    }
    [[nodiscard]] bool queryingPlatform() const {
        return m_pollBusy && m_queryPlatform;
    }
    [[nodiscard]] bool cancelling() const {
        return m_cancelBusy;
    }
    [[nodiscard]] bool registration() const {
        return m_registration;
    }
    [[nodiscard]] qint64 remainingSeconds() const;
    [[nodiscard]] int nextPollDelayMs() const {
        return m_poll.interval();
    }
    [[nodiscard]] QString notice() const;

  signals:
    void changed();
    void refreshRequested();

  private:
    friend class AccountSessionService;
    QJsonObject m_request;
    QString m_proof;
    QString m_notice;
    QTimer m_poll;
    quint64 m_generation = 0;
    bool m_registration = true;
    bool m_pollBusy = false;
    bool m_queryPlatform = false;
    bool m_cancelBusy = false;
    bool m_autoClaim = true;
    bool m_claimBusy = false;
    qint64 m_serverOffsetMs = 0;
    int m_failures = 0;
};

class AccountSessionService final : public QObject {
    Q_OBJECT
  public:
    struct Options {
        bool persistSecrets = true;
        QString secretDirectory;
        QString profileIdentity;
        int pollIntervalMs = 3000;
    };
    explicit AccountSessionService(SnowShotApiClient& client, QObject* parent = nullptr);
    AccountSessionService(SnowShotApiClient& client, Options options, QObject* parent = nullptr);
    ~AccountSessionService() override;
    static AccountSessionService* forClient(SnowShotApiClient* client);

    [[nodiscard]] bool signedIn() const {
        return !m_accessToken.isEmpty();
    }
    [[nodiscard]] bool busy() const {
        return m_busy;
    }
    [[nodiscard]] QString error() const;
    [[nodiscard]] QString fieldError(const QString& field) const;
    [[nodiscard]] QString storageNotice() const;
    [[nodiscard]] const QJsonObject& account() const {
        return m_account;
    }
    [[nodiscard]] const QJsonObject& captcha() const {
        return m_captcha;
    }
    [[nodiscard]] const QJsonArray& transactions() const {
        return m_transactions;
    }
    [[nodiscard]] bool hasMoreTransactions() const {
        return !m_nextCursor.isEmpty();
    }
    [[nodiscard]] AccountPurchaseCoordinator& purchase() {
        return m_purchase;
    }
    [[nodiscard]] static QString formatCredits(const QString& nanos);
    [[nodiscard]] static QString formatCreditAmount(const QString& nanos);

    void signIn(const QString& username, const QString& password);
    void signOut();
    void loadCaptcha();
    void registerAccount(const QString& username, const QString& password,
                         const QString& confirmation, const QString& answer);
    void beginTopUp();
    void refreshPurchase(bool queryPlatform = false);
    void cancelPurchase();
    void dismissPurchase();
    void refreshAccount();
    void loadTransactions(bool append = false);
    void clearError();
    void clearFieldError(const QString& field);

  signals:
    void changed();
    void requestFailed();
    void signedInChanged(bool signedIn);
    void registrationActivated();

  private:
    struct RequestFailure {
        QString code;
        int status = 0;
        [[nodiscard]] bool isEmpty() const {
            return code.isEmpty();
        }
    };
    using Completion = std::function<void(const QJsonObject&, const RequestFailure&)>;
    void send(const QByteArray& method, const QString& path, const QJsonObject& body,
              Completion completion, bool authenticated = false, const QString& proof = {},
              const QString& operation = {}, bool retried = false);
    void refreshSession(std::function<void(bool)> completion);
    void acceptSession(const QJsonObject& session, bool sameAccountRefresh = false);
    void acceptPurchase(const QJsonObject& request, bool registration);
    void clearPurchase();
    void claimRegistration();
    void resetOrigin();
    void persist();
    void setBusy(bool value);
    void fail(const QString& message);
    void fail(const RequestFailure& failure);
    void failField(const QString& field, const QString& message);
    void suppressAutomaticSignIn();

    QPointer<SnowShotApiClient> m_client;
    QNetworkAccessManager* m_network = nullptr;
    std::unique_ptr<storage::AccountSecretStore> m_secrets;
    Options m_options;
    AccountPurchaseCoordinator m_purchase;
    QTimer m_accountRefresh;
    QTimer m_tokenRefresh;
    QSet<QNetworkReply*> m_replies;
    QString m_origin;
    QString m_accessToken;
    QString m_refreshToken;
    QString m_error;
    QMap<QString, QString> m_fieldErrors;
    QString m_storageNotice;
    QString m_nextCursor;
    QJsonObject m_account;
    QJsonObject m_captcha;
    QJsonArray m_transactions;
    quint64 m_generation = 0;
    bool m_busy = false;
    bool m_accountBusy = false;
    bool m_transactionsBusy = false;
    bool m_refreshing = false;
    QList<std::function<void(bool)>> m_refreshWaiters;
};
} // namespace snow_shot::app

#endif
