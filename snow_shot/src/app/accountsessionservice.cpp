#include "snow_shot/app/accountsessionservice.h"

#include "snow_shot/app/edition.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/storage/accountsecretstore.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace snow_shot::app {
namespace {
class AccountSystemProxyFactory final : public QNetworkProxyFactory {
  public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return systemProxyForQuery(query);
    }
};

QString translatedAccountMessage(const QString& source) {
    if (source.isEmpty())
        return {};
    const auto bytes = source.toUtf8();
    const QString translated =
        QCoreApplication::translate("AccountSessionService", bytes.constData());
    return translated == source ? QCoreApplication::translate(
                                      "snow_shot::app::AccountSessionService", bytes.constData())
                                : translated;
}
QString accountError(const QString& code) {
    if (code == u"invalid_credentials")
        return QString::fromUtf8(
            QT_TRANSLATE_NOOP("AccountSessionService", "The username or password is incorrect."));
    if (code == u"account_inactive")
        return QString::fromUtf8(QT_TRANSLATE_NOOP(
            "AccountSessionService", "Complete your first payment to activate this account."));
    if (code == u"username_unavailable")
        return QString::fromUtf8(QT_TRANSLATE_NOOP(
            "AccountSessionService", "This username is already taken or temporarily reserved."));
    if (code == u"invalid_captcha")
        return QString::fromUtf8(
            QT_TRANSLATE_NOOP("AccountSessionService",
                              "The image code is incorrect or expired. Refresh it and try again."));
    if (code == u"password_mismatch")
        return QString::fromUtf8(
            QT_TRANSLATE_NOOP("AccountSessionService", "The passwords do not match."));
    if (code == u"payments_unavailable")
        return QString::fromUtf8(
            QT_TRANSLATE_NOOP("AccountSessionService",
                              "Purchases are currently unavailable. Please try again later."));
    if (code == u"wechat_link_conflict")
        return QString::fromUtf8(QT_TRANSLATE_NOOP(
            "AccountSessionService",
            "This purchase must use the WeChat account linked to your Snow Shot account."));
    if (code == u"request_expired")
        return QString::fromUtf8(
            QT_TRANSLATE_NOOP("AccountSessionService",
                              "This purchase code has expired. Generate a new code to continue."));
    if (code == u"payment_pending" || code == u"payment_not_received")
        return QString::fromUtf8(QT_TRANSLATE_NOOP(
            "AccountSessionService", "Payment has not been confirmed. We will keep checking."));
    if (code == u"reconciliation_required")
        return QString::fromUtf8(QT_TRANSLATE_NOOP(
            "AccountSessionService",
            "The payment result needs confirmation. Your request remains reserved."));
    if (code == u"invalid_session")
        return QString::fromUtf8(
            QT_TRANSLATE_NOOP("AccountSessionService", "Your session has expired. Sign in again."));
    if (code == u"rate_limited")
        return QString::fromUtf8(QT_TRANSLATE_NOOP(
            "AccountSessionService", "Too many requests. Wait a moment and try again."));
    return QString::fromUtf8(
        QT_TRANSLATE_NOOP("AccountSessionService",
                          "Unable to complete the request. Check your connection and try again."));
}
bool continuingPurchase(const QString& state) {
    return state == u"awaiting_payment" || state == u"payment_pending" ||
           state == u"reconciliation_required";
}
} // namespace

AccountPurchaseCoordinator::AccountPurchaseCoordinator(QObject* parent) : QObject(parent) {
    m_poll.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, &AccountPurchaseCoordinator::refreshRequested);
}
bool AccountPurchaseCoordinator::pending() const {
    return continuingPurchase(m_request.value(QStringLiteral("state")).toString());
}
qint64 AccountPurchaseCoordinator::remainingSeconds() const {
    const auto expiry = QDateTime::fromString(
        m_request.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs);
    return std::max<qint64>(
        0, QDateTime::currentDateTimeUtc().addMSecs(m_serverOffsetMs).secsTo(expiry));
}

QString AccountPurchaseCoordinator::notice() const {
    return translatedAccountMessage(m_notice);
}
QString AccountSessionService::error() const {
    return translatedAccountMessage(m_error);
}
QString AccountSessionService::fieldError(const QString& field) const {
    return translatedAccountMessage(m_fieldErrors.value(field));
}
QString AccountSessionService::storageNotice() const {
    return translatedAccountMessage(m_storageNotice);
}

AccountSessionService::AccountSessionService(SnowShotApiClient& client, QObject* parent)
    : AccountSessionService(client, Options{}, parent) {}
AccountSessionService::AccountSessionService(SnowShotApiClient& client, Options options,
                                             QObject* parent)
    : QObject(parent), m_client(&client), m_network(new QNetworkAccessManager(this)),
      m_options(std::move(options)), m_purchase(this) {
    setObjectName(QStringLiteral("accountSessionService"));
    connect(&client, &SnowShotApiClient::baseUrlChanged, this, &AccountSessionService::resetOrigin);
    connect(&client, &SnowShotApiClient::officialCallSettled, this, [this] {
        refreshAccount();
        loadTransactions();
    });
    connect(&m_purchase, &AccountPurchaseCoordinator::refreshRequested, this,
            [this] { refreshPurchase(); });
    connect(&m_purchase, &AccountPurchaseCoordinator::changed, this,
            &AccountSessionService::changed);
    m_accountRefresh.setInterval(30000);
    connect(&m_accountRefresh, &QTimer::timeout, this, [this] {
        refreshAccount();
        loadTransactions();
    });
    m_tokenRefresh.setSingleShot(true);
    connect(&m_tokenRefresh, &QTimer::timeout, this, [this] {
        if (!m_refreshToken.isEmpty())
            refreshSession([](bool) {});
    });
    resetOrigin();
}
AccountSessionService::~AccountSessionService() = default;

AccountSessionService* AccountSessionService::forClient(SnowShotApiClient* client) {
    if (!client || edition::isMini)
        return nullptr;
    for (auto* service :
         client->findChildren<AccountSessionService*>(QString(), Qt::FindDirectChildrenOnly))
        return service;
    Options options;
    const auto& appStorage = storage::ApplicationStorage::instance();
    if (appStorage.isInitialized())
        options.profileIdentity = appStorage.configurationDirectory();
    return new AccountSessionService(*client, std::move(options), client);
}

QString AccountSessionService::formatMoney(const QString& nanos) {
    bool valid = false;
    const qint64 value = nanos.toLongLong(&valid);
    if (!valid)
        return QStringLiteral("CNY 0.00");
    const auto magnitude = value < 0 ? quint64(-(value + 1)) + 1 : quint64(value);
    QString fraction = QString::number(magnitude % 1000000000).rightJustified(9, u'0');
    while (fraction.size() > 2 && fraction.endsWith(u'0'))
        fraction.chop(1);
    return QStringLiteral("%1CNY %2.%3")
        .arg(value < 0 ? QStringLiteral("−") : QString(), QString::number(magnitude / 1000000000),
             fraction);
}

void AccountSessionService::send(const QByteArray& method, const QString& path,
                                 const QJsonObject& body, Completion completion, bool authenticated,
                                 const QString& proof, const QString& operation, bool retried) {
    const quint64 generation = m_generation;
    const QString requestId =
        operation.isEmpty() ? QUuid::createUuid().toString(QUuid::Id128) : operation;
    QNetworkRequest request(QUrl(m_origin + QStringLiteral("/api/v2") + path));
    if (m_client && m_client->usesSystemProxy()) {
        if (!m_network->proxyFactory())
            m_network->setProxyFactory(new AccountSystemProxyFactory);
    } else {
        m_network->setProxy(QNetworkProxy::NoProxy);
    }
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(20000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Content-Type", "application/json");
    request.setRawHeader("X-Request-ID", requestId.toUtf8());
    if (authenticated && !m_accessToken.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    if (!proof.isEmpty())
        request.setRawHeader("X-Registration-Proof", proof.toUtf8());
    auto* reply = method == "GET"
                      ? m_network->get(request)
                      : m_network->sendCustomRequest(
                            request, method, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_replies.insert(reply);
    // Codes are bounded separately from streaming translation responses.
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QIODevice::readyRead, this, [reply, bytes] {
        bytes->append(reply->readAll());
        if (bytes->size() > 4 * 1024 * 1024)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, bytes, generation, method, path, body, completion, authenticated, proof,
             requestId, retried] {
                m_replies.remove(reply);
                bytes->append(reply->readAll());
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const auto object = QJsonDocument::fromJson(*bytes).object();
                const bool success = status >= 200 && status < 300 &&
                                     reply->error() == QNetworkReply::NoError &&
                                     object.value(QStringLiteral("code")).toInt(-1) == 0 &&
                                     object.value(QStringLiteral("data")).isObject();
                reply->deleteLater();
                if (generation != m_generation)
                    return;
                if (status == 401 && authenticated && !retried && !m_refreshToken.isEmpty()) {
                    refreshSession(
                        [this, method, path, body, completion, proof, requestId](bool refreshed) {
                            if (refreshed)
                                send(method, path, body, completion, true, proof, requestId, true);
                            else
                                completion({}, {QStringLiteral("invalid_session"), 401});
                        });
                    return;
                }
                QString code = object.value(QStringLiteral("code")).toString();
                if (!success && code.isEmpty())
                    code = QStringLiteral("request_failed");
                completion(object.value(QStringLiteral("data")).toObject(),
                           {success ? QString() : code, status});
            });
}

void AccountSessionService::resetOrigin() {
    ++m_generation;
    const auto replies = m_replies;
    for (auto* reply : replies)
        reply->abort();
    m_refreshWaiters.clear();
    m_refreshing = m_busy = m_accountBusy = m_transactionsBusy = false;
    m_purchase.m_poll.stop();
    m_purchase.m_request = {};
    m_purchase.m_proof.clear();
    m_purchase.m_notice.clear();
    m_purchase.m_pollBusy = m_purchase.m_claimBusy = false;
    m_origin = m_client ? m_client->baseUrl() : QString();
    m_accessToken.clear();
    m_refreshToken.clear();
    m_account = {};
    m_captcha = {};
    m_transactions = {};
    m_nextCursor.clear();
    m_error.clear();
    m_fieldErrors.clear();
    m_accountRefresh.stop();
    m_tokenRefresh.stop();
    m_secrets = std::make_unique<storage::AccountSecretStore>(
        QCoreApplication::applicationName() + u'|' + m_options.profileIdentity + u'|' + m_origin,
        m_options.secretDirectory);
    if (m_options.persistSecrets) {
        const auto stored = QJsonDocument::fromJson(m_secrets->read()).object();
        m_refreshToken = stored.value(QStringLiteral("refresh_token")).toString();
        m_purchase.m_request = stored.value(QStringLiteral("purchase")).toObject();
        m_purchase.m_proof = stored.value(QStringLiteral("registration_proof")).toString();
        m_purchase.m_registration = stored.value(QStringLiteral("registration")).toBool(true);
        m_purchase.m_autoClaim = stored.value(QStringLiteral("auto_claim")).toBool(false);
    }
    if (!m_refreshToken.isEmpty()) {
        // A stored session expresses an existing account choice. Never race its
        // restoration against a registration's bootstrap-session claim.
        m_purchase.m_autoClaim = false;
        persist();
        refreshSession([this](bool success) {
            if (success) {
                refreshAccount();
                loadTransactions();
            }
        });
    }
    if (m_purchase.pending())
        m_purchase.m_poll.start(0);
    else if (m_purchase.m_autoClaim && m_purchase.m_registration &&
             m_purchase.m_request.value(QStringLiteral("state")).toString() == u"fulfilled")
        QTimer::singleShot(0, this, &AccountSessionService::claimRegistration);
    emit changed();
}

void AccountSessionService::persist() {
    if (!m_options.persistSecrets || !m_secrets)
        return;
    const QJsonObject state{{QStringLiteral("refresh_token"), m_refreshToken},
                            {QStringLiteral("purchase"), m_purchase.m_request},
                            {QStringLiteral("registration_proof"), m_purchase.m_proof},
                            {QStringLiteral("registration"), m_purchase.m_registration},
                            {QStringLiteral("auto_claim"), m_purchase.m_autoClaim}};
    if (!m_secrets->write(QJsonDocument(state).toJson(QJsonDocument::Compact)))
        m_storageNotice = QString::fromUtf8(QT_TRANSLATE_NOOP(
            "snow_shot::app::AccountSessionService",
            "Your session could not be saved securely. Keep this app open until "
            "payment is confirmed; you may need to sign in again after restarting."));
}

void AccountSessionService::refreshSession(std::function<void(bool)> completion) {
    m_refreshWaiters.append(std::move(completion));
    if (m_refreshing)
        return;
    m_refreshing = true;
    send("POST", QStringLiteral("/auth/refresh"),
         {{QStringLiteral("refresh_token"), m_refreshToken}},
         [this](const QJsonObject& data, const RequestFailure& error) {
             m_refreshing = false;
             const bool success = error.isEmpty() &&
                                  !data.value(QStringLiteral("access_token")).toString().isEmpty();
             if (success)
                 acceptSession(data, true);
             else if (error.status == 401) {
                 m_accessToken.clear();
                 m_refreshToken.clear();
                 if (m_client)
                     m_client->setAccountAccessToken({});
                 m_account = {};
                 m_transactions = {};
                 persist();
                 emit signedInChanged(false);
             }
             const auto waiters = std::exchange(m_refreshWaiters, {});
             for (const auto& waiter : waiters)
                 waiter(success);
             if (!success)
                 fail(error);
         });
}
void AccountSessionService::acceptSession(const QJsonObject& session, bool sameAccountRefresh) {
    if (session.value(QStringLiteral("access_token")).toString().isEmpty() ||
        session.value(QStringLiteral("refresh_token")).toString().isEmpty()) {
        fail(accountError({}));
        return;
    }
    m_accessToken = session.value(QStringLiteral("access_token")).toString();
    m_refreshToken = session.value(QStringLiteral("refresh_token")).toString();
    if (m_client)
        m_client->setAccountAccessToken(m_accessToken.toUtf8(), sameAccountRefresh);
    m_accountRefresh.start();
    const auto expires = QDateTime::fromString(
        session.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs);
    if (expires.isValid())
        m_tokenRefresh.start(static_cast<int>(std::clamp<qint64>(
            QDateTime::currentDateTimeUtc().msecsTo(expires) - 60000, 1000, 3600000)));
    persist();
    emit signedInChanged(signedIn());
    emit changed();
}
void AccountSessionService::setBusy(bool value) {
    m_busy = value;
    emit changed();
}
void AccountSessionService::fail(const QString& message) {
    m_error = message;
    emit changed();
    emit requestFailed();
}
void AccountSessionService::fail(const RequestFailure& failure) {
    fail(accountError(failure.code));
}
void AccountSessionService::failField(const QString& field, const QString& message) {
    m_fieldErrors.insert(field, message);
    emit changed();
}
void AccountSessionService::clearError() {
    m_error.clear();
    m_fieldErrors.clear();
    emit changed();
}
void AccountSessionService::clearFieldError(const QString& field) {
    if (m_fieldErrors.remove(field) > 0)
        emit changed();
}
void AccountSessionService::suppressAutomaticSignIn() {
    m_purchase.m_autoClaim = false;
    persist();
}

void AccountSessionService::signIn(const QString& username, const QString& password) {
    if (m_busy)
        return;
    clearError();
    if (username.trimmed().isEmpty())
        m_fieldErrors.insert(QStringLiteral("username"),
                             QString::fromUtf8(QT_TRANSLATE_NOOP(
                                 "snow_shot::app::AccountSessionService", "Enter your username.")));
    if (password.isEmpty())
        m_fieldErrors.insert(QStringLiteral("password"),
                             QString::fromUtf8(QT_TRANSLATE_NOOP(
                                 "snow_shot::app::AccountSessionService", "Enter your password.")));
    if (!m_fieldErrors.isEmpty()) {
        emit changed();
        return;
    }
    suppressAutomaticSignIn();
    ++m_generation;
    const auto replies = m_replies;
    for (auto* reply : replies)
        reply->abort();
    m_refreshWaiters.clear();
    m_refreshing = m_accountBusy = m_transactionsBusy = false;
    m_purchase.m_pollBusy = m_purchase.m_claimBusy = false;
    if (m_purchase.pending())
        m_purchase.m_poll.start(m_options.pollIntervalMs);
    m_error.clear();
    setBusy(true);
    send("POST", QStringLiteral("/auth/login"),
         {{QStringLiteral("username"), username}, {QStringLiteral("password"), password}},
         [this](const QJsonObject& data, const RequestFailure& error) {
             setBusy(false);
             if (!error.isEmpty()) {
                 if (error.code == u"invalid_credentials")
                     failField(QStringLiteral("password"), accountError(error.code));
                 else
                     fail(error);
                 return;
             }
             acceptSession(data);
             refreshAccount();
             loadTransactions();
         });
}
void AccountSessionService::signOut() {
    const auto refresh = m_refreshToken;
    ++m_generation;
    const auto replies = m_replies;
    for (auto* reply : replies)
        reply->abort();
    m_refreshWaiters.clear();
    m_refreshing = m_busy = m_accountBusy = m_transactionsBusy = false;
    m_purchase.m_pollBusy = m_purchase.m_claimBusy = false;
    suppressAutomaticSignIn();
    m_accountRefresh.stop();
    m_tokenRefresh.stop();
    m_accessToken.clear();
    m_refreshToken.clear();
    m_account = {};
    m_transactions = {};
    m_nextCursor.clear();
    m_error.clear();
    m_fieldErrors.clear();
    if (m_client)
        m_client->setAccountAccessToken({});
    persist();
    if (!refresh.isEmpty())
        send("POST", QStringLiteral("/auth/logout"), {{QStringLiteral("refresh_token"), refresh}},
             [](const auto&, const auto&) {});
    emit signedInChanged(false);
    emit changed();
    if (m_purchase.pending())
        m_purchase.m_poll.start(m_options.pollIntervalMs);
}

void AccountSessionService::loadCaptcha() {
    send("GET", QStringLiteral("/registration/captcha"), {},
         [this](const QJsonObject& data, const RequestFailure& error) {
             if (!error.isEmpty()) {
                 fail(error);
                 return;
             }
             m_captcha = data;
             emit changed();
         });
}
void AccountSessionService::registerAccount(const QString& username, const QString& password,
                                            const QString& confirmation, const QString& answer) {
    if (m_busy || m_purchase.pending())
        return;
    clearError();
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]{3,32}$")).match(username).hasMatch()) {
        m_fieldErrors.insert(
            QStringLiteral("username"),
            QString::fromUtf8(QT_TRANSLATE_NOOP(
                "snow_shot::app::AccountSessionService",
                "Use 3–32 letters, numbers, underscores, dots, or hyphens for your username.")));
    }
    if (password.size() < 8 || password.size() > 16) {
        m_fieldErrors.insert(
            QStringLiteral("password"),
            QString::fromUtf8(QT_TRANSLATE_NOOP("snow_shot::app::AccountSessionService",
                                                "Use a password between 8 and 16 characters.")));
    }
    if (confirmation.isEmpty()) {
        m_fieldErrors.insert(
            QStringLiteral("password_confirmation"),
            QString::fromUtf8(QT_TRANSLATE_NOOP("snow_shot::app::AccountSessionService",
                                                "Confirm your password.")));
    } else if (password != confirmation) {
        m_fieldErrors.insert(QStringLiteral("password_confirmation"),
                             accountError(QStringLiteral("password_mismatch")));
    }
    if (answer.trimmed().isEmpty()) {
        m_fieldErrors.insert(
            QStringLiteral("captcha_answer"),
            QString::fromUtf8(QT_TRANSLATE_NOOP("snow_shot::app::AccountSessionService",
                                                "Enter the image code.")));
    }
    if (!m_fieldErrors.isEmpty()) {
        emit changed();
        return;
    }
    m_error.clear();
    setBusy(true);
    send("POST", QStringLiteral("/registration/requests"),
         {{QStringLiteral("username"), username},
          {QStringLiteral("password"), password},
          {QStringLiteral("password_confirmation"), confirmation},
          {QStringLiteral("captcha_id"), m_captcha.value(QStringLiteral("captcha_id"))},
          {QStringLiteral("captcha_answer"), answer}},
         [this](const QJsonObject& data, const RequestFailure& error) {
             setBusy(false);
             m_captcha = {};
             if (!error.isEmpty()) {
                 if (error.code == u"username_unavailable")
                     failField(QStringLiteral("username"), accountError(error.code));
                 else if (error.code == u"invalid_captcha")
                     failField(QStringLiteral("captcha_answer"), accountError(error.code));
                 else if (error.code == u"password_mismatch")
                     failField(QStringLiteral("password_confirmation"), accountError(error.code));
                 else
                     fail(error);
                 loadCaptcha();
                 return;
             }
             acceptPurchase(data, true);
         });
}
void AccountSessionService::beginTopUp() {
    if (!signedIn() || m_busy || m_purchase.pending())
        return;
    m_error.clear();
    setBusy(true);
    send(
        "POST", QStringLiteral("/purchases/requests"), {},
        [this](const QJsonObject& data, const RequestFailure& error) {
            setBusy(false);
            if (!error.isEmpty())
                fail(error);
            else
                acceptPurchase(data, false);
        },
        true);
}
void AccountSessionService::acceptPurchase(const QJsonObject& request, bool registration) {
    m_purchase.m_poll.stop();
    m_purchase.m_request = request;
    m_purchase.m_proof = request.value(QStringLiteral("registration_proof")).toString();
    m_purchase.m_request.remove(QStringLiteral("registration_proof"));
    m_purchase.m_registration = registration;
    m_purchase.m_autoClaim = registration;
    m_purchase.m_notice.clear();
    m_purchase.m_failures = 0;
    const auto serverTime = QDateTime::fromString(
        request.value(QStringLiteral("server_time")).toString(), Qt::ISODateWithMs);
    if (serverTime.isValid())
        m_purchase.m_serverOffsetMs = QDateTime::currentDateTimeUtc().msecsTo(serverTime);
    persist();
    m_purchase.m_poll.start(m_options.pollIntervalMs);
    emit changed();
}

void AccountSessionService::refreshPurchase(bool queryPlatform) {
    if (m_purchase.m_pollBusy || m_purchase.m_request.isEmpty())
        return;
    m_purchase.m_poll.stop();
    if (queryPlatform)
        m_purchase.m_failures = 0;
    m_purchase.m_pollBusy = true;
    emit changed();
    const QString id = m_purchase.m_request.value(QStringLiteral("request_id")).toString();
    const QString path = (m_purchase.m_registration ? QStringLiteral("/registration/requests/")
                                                    : QStringLiteral("/purchases/requests/")) +
                         id + (queryPlatform ? QStringLiteral("/refresh") : QString());
    send(
        queryPlatform ? "POST" : "GET", path, {},
        [this, id](const QJsonObject& data, const RequestFailure& error) {
            m_purchase.m_pollBusy = false;
            if (m_purchase.m_request.value(QStringLiteral("request_id")).toString() != id)
                return;
            if (!error.isEmpty()) {
                fail(error);
                m_purchase.m_failures = std::min(m_purchase.m_failures + 1, 4);
            } else {
                m_purchase.m_notice.clear();
                m_purchase.m_failures = 0;
                const auto serverTime = QDateTime::fromString(
                    data.value(QStringLiteral("server_time")).toString(), Qt::ISODateWithMs);
                if (serverTime.isValid())
                    m_purchase.m_serverOffsetMs =
                        QDateTime::currentDateTimeUtc().msecsTo(serverTime);
                for (auto it = data.begin(); it != data.end(); ++it) {
                    if (it.key() != u"mini_program_code_data_url" || !it.value().isNull())
                        m_purchase.m_request.insert(it.key(), it.value());
                }
                persist();
                if (data.value(QStringLiteral("state")).toString() == u"fulfilled") {
                    if (m_purchase.m_registration && m_purchase.m_autoClaim && !signedIn())
                        claimRegistration();
                    else if (signedIn()) {
                        refreshAccount();
                        loadTransactions();
                    }
                }
            }
            if (m_purchase.pending())
                m_purchase.m_poll.start(
                    std::min(30000, m_options.pollIntervalMs * (1 << m_purchase.m_failures)));
            emit changed();
        },
        !m_purchase.m_registration, m_purchase.m_proof);
}
void AccountSessionService::claimRegistration() {
    if (m_purchase.m_claimBusy || !m_purchase.m_autoClaim || signedIn() || m_refreshing)
        return;
    m_purchase.m_claimBusy = true;
    const QString id = m_purchase.m_request.value(QStringLiteral("request_id")).toString();
    send(
        "POST", QStringLiteral("/registration/requests/") + id + QStringLiteral("/claim"), {},
        [this](const QJsonObject& data, const RequestFailure& error) {
            m_purchase.m_claimBusy = false;
            if (!m_purchase.m_autoClaim || signedIn())
                return;
            if (!error.isEmpty()) {
                fail(error);
                m_purchase.m_notice = QString::fromUtf8(
                    QT_TRANSLATE_NOOP("snow_shot::app::AccountSessionService",
                                      "Your account is activated. Sign in with your username "
                                      "and password if automatic sign-in does not complete."));
                if (error.status == 0 || error.status >= 500)
                    m_purchase.m_poll.start(m_options.pollIntervalMs);
            } else {
                m_purchase.m_autoClaim = false;
                m_purchase.m_proof.clear();
                m_purchase.m_request = {};
                acceptSession(data);
                refreshAccount();
                loadTransactions();
                if (signedIn())
                    emit registrationActivated();
            }
            persist();
            emit changed();
        },
        false, m_purchase.m_proof);
}
void AccountSessionService::dismissPurchase() {
    if (m_purchase.pending())
        return;
    m_purchase.m_poll.stop();
    m_purchase.m_request = {};
    m_purchase.m_proof.clear();
    m_purchase.m_notice.clear();
    persist();
    emit changed();
}
void AccountSessionService::refreshAccount() {
    if (!signedIn() || m_accountBusy)
        return;
    m_accountBusy = true;
    send(
        "GET", QStringLiteral("/account"), {},
        [this](const QJsonObject& data, const RequestFailure& error) {
            m_accountBusy = false;
            if (!error.isEmpty())
                fail(error);
            else {
                m_account = data;
                emit changed();
            }
        },
        true);
}
void AccountSessionService::loadTransactions(bool append) {
    if (!signedIn() || m_transactionsBusy)
        return;
    m_transactionsBusy = true;
    QString path = QStringLiteral("/account/transactions?limit=20");
    if (append && !m_nextCursor.isEmpty())
        path +=
            QStringLiteral("&cursor=") + QString::fromLatin1(QUrl::toPercentEncoding(m_nextCursor));
    send(
        "GET", path, {},
        [this, append](const QJsonObject& data, const RequestFailure& error) {
            m_transactionsBusy = false;
            if (!error.isEmpty()) {
                fail(error);
                return;
            }
            if (!append)
                m_transactions = {};
            for (const auto& item : data.value(QStringLiteral("items")).toArray())
                m_transactions.append(item);
            m_nextCursor = data.value(QStringLiteral("next_cursor")).toString();
            emit changed();
        },
        true);
}
} // namespace snow_shot::app
