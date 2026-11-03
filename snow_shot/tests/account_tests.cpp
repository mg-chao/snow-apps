#include "snow_shot/app/accountsessionservice.h"
#include "snow_shot/app/distribution.h"
#include "translation_test_support.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/components/accountpagewidget.h"
#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "widgets/scroll_area.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/accountsecretstore.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/alert.h"
#include "widgets/input_line_edit.h"
#include "widgets/form.h"
#include "widgets/message.h"
#include "widgets/tag.h"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QPainter>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QLabel>
#include <QKeyEvent>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTranslator>
#include <QTextDocument>
#include <QScrollBar>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <utility>

using snow_shot::app::AccountSessionService;
namespace settings = snow_shot::presentation::settings;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void await(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "account operation timed out");
}

class Api {
  public:
    QTcpServer server;
    QString state = QStringLiteral("awaiting_payment");
    QString username = QStringLiteral("TestUser");
    QString completionKind;
    int polls = 0;
    int claims = 0;
    int refreshes = 0;
    int registrations = 0;
    int purchases = 0;
    int cancellations = 0;
    int captchas = 0;
    int logins = 0;
    bool manualQuery = false;
    bool proofSeen = false;
    bool officialAuthSeen = false;
    bool rejectPolls = false;
    QString failurePath;
    QString failureCode;
    int failureStatus = 400;
    bool abortRequest = false;
    bool emptyTransactions = false;
    bool holdPurchasePolls = false;
    QList<std::function<void()>> heldPurchasePolls;
    QString requestId = QStringLiteral("request1");
    QByteArray registrationBody;
    QString balance = QStringLiteral("9900000000");
    QString debt = QStringLiteral("0");
    QString cumulativeRecharge = QStringLiteral("188000000000");
    int expirySeconds = 600;
    QString codeData;
    QString captchaData;

    Api() {
        QImage code(192, 192, QImage::Format_RGB32);
        code.fill(Qt::white);
        QPainter painter(&code);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        for (int y = 2; y < 30; ++y)
            for (int x = 2; x < 30; ++x)
                if ((x * 17 + y * 11 + x * y) % 7 < 3)
                    painter.drawRect(x * 6, y * 6, 5, 5);
        painter.end();
        QByteArray png;
        QBuffer encoded(&png);
        encoded.open(QIODevice::WriteOnly);
        code.save(&encoded, "PNG");
        codeData = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
        QImage captcha(152, 64, QImage::Format_RGB32);
        captcha.fill(QColor(QStringLiteral("#edf3fa")));
        QPainter letters(&captcha);
        QFont font;
        font.setPixelSize(25);
        font.setBold(true);
        letters.setFont(font);
        letters.setPen(QColor(QStringLiteral("#234f85")));
        letters.drawText(captcha.rect(), Qt::AlignCenter, QStringLiteral("A7K9"));
        letters.end();
        png.clear();
        encoded.close();
        encoded.open(QIODevice::WriteOnly);
        captcha.save(&encoded, "PNG");
        captchaData =
            QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
        require(server.listen(QHostAddress::LocalHost), "fake account API listens");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                auto bytes = std::make_shared<QByteArray>();
                QObject::connect(socket, &QIODevice::readyRead, socket, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    const auto headerEnd = bytes->indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    int length = 0;
                    for (const auto& line : bytes->left(headerEnd).split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    if (bytes->size() < headerEnd + 4 + length ||
                        socket->property("handled").toBool())
                        return;
                    socket->setProperty("handled", true);
                    const auto first = bytes->left(bytes->indexOf("\r\n")).split(' ');
                    const QByteArray path = first.value(1);
                    const bool post = first.value(0) == "POST";
                    const auto body = QJsonDocument::fromJson(bytes->mid(headerEnd + 4)).object();
                    const auto headers = bytes->left(headerEnd).toLower();
                    QJsonObject data;
                    int responseStatus = 200;
                    if (path == "/api/v2/registration/captcha") {
                        ++captchas;
                        data = {{QStringLiteral("captcha_id"), QStringLiteral("challenge")},
                                {QStringLiteral("image_data_url"), captchaData}};
                    } else if (path == "/api/v2/registration/requests" && post) {
                        ++registrations;
                        registrationBody = bytes->mid(headerEnd + 4);
                        username = body.value(QStringLiteral("username")).toString();
                        data = purchase();
                        data.insert(QStringLiteral("registration_proof"),
                                    QStringLiteral("private-proof"));
                    } else if (path.endsWith("/claim")) {
                        ++claims;
                        proofSeen = headers.contains("x-registration-proof: private-proof");
                        data = session();
                    } else if (path.startsWith("/api/v2/registration/requests/")) {
                        ++polls;
                        if (rejectPolls)
                            responseStatus = 503;
                        proofSeen = headers.contains("x-registration-proof: private-proof");
                        manualQuery = manualQuery || (post && path.endsWith("/refresh"));
                        data = purchase();
                        data.insert(QStringLiteral("mini_program_code_data_url"), QJsonValue());
                    } else if (path == "/api/v2/purchases/requests" && post) {
                        requestId = QStringLiteral("purchase%1").arg(++purchases);
                        data = purchase();
                    } else if (path.startsWith("/api/v2/purchases/requests/") &&
                               path.endsWith("/cancel") && post) {
                        ++cancellations;
                        officialAuthSeen = headers.contains("authorization: bearer desktop-access");
                        data = purchase();
                        if (state == u"awaiting_payment")
                            data.insert(QStringLiteral("state"), QStringLiteral("failed"));
                    } else if (path.startsWith("/api/v2/purchases/requests/")) {
                        ++polls;
                        if (rejectPolls)
                            responseStatus = 503;
                        officialAuthSeen = headers.contains("authorization: bearer desktop-access");
                        manualQuery = manualQuery || (post && path.endsWith("/refresh"));
                        data = purchase();
                        data.insert(QStringLiteral("mini_program_code_data_url"), QJsonValue());
                    } else if (path == "/api/v2/auth/login") {
                        ++logins;
                        data = session();
                    } else if (path == "/api/v2/auth/refresh") {
                        ++refreshes;
                        data = session();
                    } else if (path == "/api/v2/account") {
                        officialAuthSeen = headers.contains("authorization: bearer desktop-access");
                        data = {
                            {QStringLiteral("username"), username},
                            {QStringLiteral("linked"), true},
                            {QStringLiteral("membership_status"), QStringLiteral("active")},
                            {QStringLiteral("available_credit_nano_yuan"), balance},
                            {QStringLiteral("outstanding_balance_nano_yuan"), debt},
                            {QStringLiteral("cumulative_recharge_nano_yuan"), cumulativeRecharge},
                            {QStringLiteral("payments_available"), true}};
                    } else if (path.startsWith("/api/v2/account/transactions")) {
                        data = {{QStringLiteral("items"),
                                 QJsonArray{QJsonObject{
                                     {QStringLiteral("id"), QStringLiteral("refund1")},
                                     {QStringLiteral("type"), QStringLiteral("refund")},
                                     {QStringLiteral("status"), QStringLiteral("pending")},
                                     {QStringLiteral("amount_nano_yuan"),
                                      QStringLiteral("-9900000000")},
                                     {QStringLiteral("order_id"), QStringLiteral("order1")},
                                     {QStringLiteral("occurred_at"),
                                      QStringLiteral("2026-11-02T00:00:00Z")}}}},
                                {QStringLiteral("next_cursor"), QJsonValue()}};
                        auto items = data.value(QStringLiteral("items")).toArray();
                        items.append(QJsonObject{
                            {QStringLiteral("id"), QStringLiteral("usage1")},
                            {QStringLiteral("type"), QStringLiteral("usage")},
                            {QStringLiteral("status"), QStringLiteral("completed")},
                            {QStringLiteral("amount_nano_yuan"), QStringLiteral("-12500000")},
                            {QStringLiteral("occurred_at"),
                             QStringLiteral("2026-11-01T12:30:00Z")}});
                        items.append(QJsonObject{
                            {QStringLiteral("id"), QStringLiteral("topup1")},
                            {QStringLiteral("type"), QStringLiteral("top_up")},
                            {QStringLiteral("status"), QStringLiteral("completed")},
                            {QStringLiteral("amount_nano_yuan"), QStringLiteral("29900000000")},
                            {QStringLiteral("order_id"), QStringLiteral("SS202611010001")},
                            {QStringLiteral("occurred_at"),
                             QStringLiteral("2026-11-01T10:00:00Z")}});
                        data.insert(QStringLiteral("items"),
                                    emptyTransactions ? QJsonArray() : items);
                    }
                    const bool rejected = path == failurePath.toUtf8();
                    if (rejected && abortRequest) {
                        socket->abort();
                        return;
                    }
                    if (rejected)
                        responseStatus = failureStatus;
                    const auto response =
                        QJsonDocument(
                            QJsonObject{{QStringLiteral("code"),
                                         rejected ? QJsonValue(failureCode) : QJsonValue(0)},
                                        {QStringLiteral("data"), data}})
                            .toJson();
                    const auto respond = [socket = QPointer<QTcpSocket>(socket), response,
                                          responseStatus] {
                        if (!socket)
                            return;
                        socket->write("HTTP/1.1 " + QByteArray::number(responseStatus) +
                                      " OK\r\nContent-Type: "
                                      "application/json\r\nConnection: close\r\nContent-Length: " +
                                      QByteArray::number(response.size()) + "\r\n\r\n" + response);
                        socket->disconnectFromHost();
                    };
                    if (holdPurchasePolls && path.startsWith("/api/v2/purchases/requests/") &&
                        !path.endsWith("/cancel"))
                        heldPurchasePolls.append(respond);
                    else
                        respond();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QString origin() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }
    QJsonObject session() const {
        return {{QStringLiteral("access_token"), QStringLiteral("desktop-access")},
                {QStringLiteral("refresh_token"), QStringLiteral("desktop-refresh")}};
    }
    QJsonObject purchase() const {
        return {
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("username"), username},
            {QStringLiteral("state"), state},
            {QStringLiteral("completion_kind"), completionKind},
            {QStringLiteral("order_id"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("expires_at"),
             QDateTime::currentDateTimeUtc().addSecs(expirySeconds).toString(Qt::ISODate)},
            {QStringLiteral("server_time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {QStringLiteral("mini_program_code_data_url"), codeData}};
    }
};

AccountSessionService::Options options() {
    AccountSessionService::Options result;
    result.persistSecrets = false;
    result.pollIntervalMs = 10;
    return result;
}

void accountNavigationIsStandaloneAndFollowsGeneral() {
    const auto catalog = settings::buildBuiltInSettingsCatalog();
    const auto* page = catalog.page(QStringLiteral("account"));
    require(page && page->kind == settings::SettingsPageKind::Account &&
                page->route == u"/settings/account",
            "account is a standalone settings route");
    bool ordered = false;
    for (const auto& node : catalog.navigation()) {
        if (const auto* group = std::get_if<settings::SettingsNavigationGroupDefinition>(&node);
            group && group->id == u"nav.settings")
            ordered = group->pages.at(0).pageId == u"general" &&
                      group->pages.at(1).pageId == u"account" &&
                      group->pages.at(2).pageId == u"general-appearance";
    }
    require(ordered, "Account navigation immediately follows General");
}

void registrationEnforcesPasswordLength() {
    const QStringList passwords{
        QString(),         QString(7, u'x'),   QString(8, u'x'),          QString(16, u'x'),
        QString(17, u'x'), QString(128, u'x'), QStringLiteral("  secret")};
    for (const auto& password : passwords) {
        Api api;
        SnowShotApiClient client(api.origin());
        AccountSessionService service(client, options());
        service.registerAccount(QStringLiteral("TestUser"), password, password,
                                QStringLiteral("abcd"));
        if (password.size() >= 8 && password.size() <= 16) {
            await([&] { return api.registrations == 1 && !service.busy(); });
            require(service.error().isEmpty(), "passwords of 8 through 16 characters are accepted");
            const auto submitted = QJsonDocument::fromJson(api.registrationBody).object();
            require(submitted.value(QStringLiteral("password")).toString() == password &&
                        submitted.value(QStringLiteral("password_confirmation")).toString() ==
                            password,
                    "accepted passwords and confirmation are submitted without trimming");
        } else {
            require(!service.busy() &&
                        service.fieldError(QStringLiteral("password")) ==
                            u"Use a password between 8 and 16 characters." &&
                        service.error().isEmpty(),
                    "invalid password lengths fail locally with the current limits");
            QApplication::processEvents();
            require(api.registrations == 0, "invalid password lengths never submit registration");
        }
    }
}

void passwordInputsEnforceMaximumLength() {
    Api api;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    auto* password = page.findChild<QLineEdit*>(QStringLiteral("accountPassword"));
    auto* confirmation = page.findChild<QLineEdit*>(QStringLiteral("accountPasswordConfirmation"));
    require(password && confirmation && password->maxLength() == 16 &&
                confirmation->maxLength() == 16,
            "password and confirmation inputs accept at most 16 characters");
    require(password->placeholderText() == u"8–16 characters" &&
                password->accessibleDescription() == password->placeholderText(),
            "password hint and accessible description explain the current limits");
    for (auto* input : {password, confirmation}) {
        input->setText(QString(16, u'x'));
        require(input->text().size() == 16, "password inputs preserve the 16-character boundary");
        input->insert(QStringLiteral("x"));
        require(input->text().size() == 16, "password inputs reject a seventeenth character");
    }
}

void registrationReconcilesBeyondExpiryAndClaims() {
    Api api;
    api.expirySeconds = -120;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    int activations = 0;
    QObject::connect(&service, &AccountSessionService::registrationActivated, &service,
                     [&] { ++activations; });
    service.loadCaptcha();
    await([&] { return !service.captcha().isEmpty(); });
    service.registerAccount(QStringLiteral("TestUser"), QStringLiteral("long-password"),
                            QStringLiteral("mismatch"), QStringLiteral("abcd"));
    require(api.registrations == 0 &&
                !service.fieldError(QStringLiteral("password_confirmation")).isEmpty(),
            "mismatched passwords never submit");
    service.registerAccount(QStringLiteral("TestUser"), QStringLiteral("long-password"),
                            QStringLiteral("long-password"), QStringLiteral("abcd"));
    await([&] { return !service.purchase().request().isEmpty(); });
    api.state = QStringLiteral("payment_pending");
    await([&] { return api.polls >= 2; });
    require(service.purchase().pending() && service.purchase().remainingSeconds() == 0,
            "payment pending continues beyond original expiry");
    require(api.proofSeen, "private registration polling requires initiating proof");
    await([&] { return !service.purchase().polling(); });
    service.refreshPurchase(true);
    await([&] { return api.manualQuery; });
    require(!service.signedIn(), "manual refresh cannot locally grant activation");
    api.state = QStringLiteral("fulfilled");
    await([&] { return service.signedIn() && !service.account().isEmpty(); });
    require(api.claims == 1 && api.officialAuthSeen && activations == 1,
            "verified fulfillment exchanges proof for session once");
    require(service.purchase().request().isEmpty(), "activation retires registration credentials");
    const auto submitted = QJsonDocument::fromJson(api.registrationBody).object();
    require(submitted.value(QStringLiteral("password_confirmation")).toString() ==
                    u"long-password" &&
                submitted.value(QStringLiteral("captcha_id")).toString() == u"challenge",
            "registration sends contract confirmation and challenge fields");
}

void temporaryAccountLinkClaimsWithoutPayment() {
    Api api;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    service.registerAccount(QStringLiteral("DesktopUser"), QStringLiteral("long-password"),
                            QStringLiteral("long-password"), QStringLiteral("abcd"));
    await([&] { return !service.purchase().request().isEmpty(); });
    api.completionKind = QStringLiteral("account_link");
    api.state = QStringLiteral("fulfilled");
    require(api.purchase().value(QStringLiteral("order_id")).isNull(),
            "account linking has no payment order");
    await([&] { return service.signedIn() && !service.account().isEmpty(); });
    require(api.claims == 1 && api.proofSeen && api.logins == 0 && !api.manualQuery,
            "orderless linking automatically claims the desktop proof without a payment query");
    require(service.account().value(QStringLiteral("username")).toString() == u"DesktopUser" &&
                service.account().value(QStringLiteral("available_credit_nano_yuan")).toString() ==
                    api.balance,
            "linked account uses desktop credentials and retains its existing balance");
}

void explicitIdentityChoiceSuppressesAutomaticClaim() {
    Api api;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    service.registerAccount(QStringLiteral("AccountA"), QStringLiteral("long-password"),
                            QStringLiteral("long-password"), QStringLiteral("abcd"));
    await([&] { return !service.purchase().request().isEmpty(); });
    service.signIn(QStringLiteral("AccountB"), QStringLiteral("another-password"));
    await([&] { return service.signedIn(); });
    api.state = QStringLiteral("fulfilled");
    await([&] {
        return service.purchase().request().value(QStringLiteral("state")).toString() ==
               u"fulfilled";
    });
    require(api.claims == 0 && api.logins == 1,
            "a pending registration cannot replace explicit sign-in");
    service.signOut();
    require(!service.signedIn() && service.account().isEmpty(),
            "sign-out clears account immediately");
}

void uncertainPollingBacksOffAndManualRefreshRecovers() {
    Api api;
    api.rejectPolls = true;
    SnowShotApiClient client(api.origin());
    auto config = options();
    config.pollIntervalMs = 100;
    AccountSessionService service(client, config);
    AccountPageWidget page(&service);
    page.resize(640, 640);
    page.show();
    service.registerAccount(QStringLiteral("TestUser"), QStringLiteral("long-password"),
                            QStringLiteral("long-password"), QStringLiteral("abcd"));
    await([&] { return !service.error().isEmpty(); });
    require(service.purchase().pending() && service.purchase().nextPollDelayMs() == 200,
            "unknown results retain the request and back off polling");
    require(adqt::widgets::AdMessageService::instance(&page)->count() == 1 &&
                service.purchase().notice().isEmpty(),
            "polling failures use a message while preserving the pending purchase");
    api.rejectPolls = false;
    service.refreshPurchase(true);
    await([&] { return api.manualQuery && !service.purchase().polling(); });
    require(service.purchase().nextPollDelayMs() == 100 && service.purchase().notice().isEmpty(),
            "manual refresh bypasses backoff and successful queries reset it");
}

void paymentActionsStayInOneRowAndDetailsAreCentered() {
    using namespace adqt::widgets;
    Api api;
    auto config = options();
    config.pollIntervalMs = 60000;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, config);
    AccountPageWidget page(&service);
    page.resize(900, 640);
    page.show();
    service.registerAccount(QStringLiteral("TestUser"), QStringLiteral("long-password"),
                            QStringLiteral("long-password"), QStringLiteral("abcd"));
    await([&] { return !service.purchase().request().isEmpty(); });
    auto* paid = page.findChild<AdButton*>(QStringLiteral("accountRefreshPayment"));
    require(paid->buttonStyle() == AdButton::ButtonStyle::Solid &&
                paid->accentRole() == AdButton::AccentRole::Primary,
            "payment confirmation remains the primary action");
    auto* payment = paid->parentWidget();
    AdButton* signIn = nullptr;
    for (auto* button : payment->findChildren<AdButton*>())
        if (button != paid && !button->isHidden())
            signIn = button;
    require(signIn && signIn->buttonStyle() == AdButton::ButtonStyle::Outline &&
                signIn->accentRole() == AdButton::AccentRole::Neutral,
            "existing-account sign-in uses a regular neutral outlined button");
    const auto verifyLayout = [&] {
        QApplication::sendPostedEvents();
        QApplication::processEvents();
        if (qAbs(paid->width() - signIn->width()) > 1)
            std::cerr << "Unequal payment actions at page width " << page.width() << ": "
                      << paid->width() << " / " << signIn->width() << '\n';
        require(paid->geometry().top() == signIn->geometry().top() &&
                    paid->geometry().right() < signIn->geometry().left() &&
                    qAbs(paid->width() - signIn->width()) <= 1,
                "payment and sign-in actions occupy two equal columns in one row");
        require(payment->rect().contains(paid->geometry()) &&
                    payment->rect().contains(signIn->geometry()),
                "both payment actions fit inside the payment content");
        auto* code = page.findChild<QLabel*>(QStringLiteral("accountMiniProgramCode"));
        require(qAbs(code->geometry().center().x() - payment->rect().center().x()) <= 1,
                "the Mini Program code is horizontally centered in the payment content");
        for (auto* label : payment->findChildren<QLabel*>()) {
            if (label->objectName() != u"accountPurchaseHeading")
                require(label->alignment().testFlag(Qt::AlignHCenter),
                        "payment status, expiry, and instructions are centered");
        }
    };
    for (const int width : {900, 640, 400, 292, 260}) {
        page.resize(width, 640);
        verifyLayout();
    }
    for (const auto* locale : {"zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "load localized payment actions");
        QApplication::installTranslator(&translator);
        for (const int width : {900, 400, 260}) {
            page.resize(width, 640);
            verifyLayout();
        }
        QApplication::removeTranslator(&translator);
    }
    signIn->click();
    require(page.findChild<QLineEdit*>(QStringLiteral("accountUsername"))->isVisibleTo(&page) &&
                !service.purchase().request().isEmpty(),
            "the regular sign-in action opens the existing-account form during payment");
}

void automaticPaymentPollingDoesNotShowButtonLoading() {
    using namespace adqt::widgets;
    Api api;
    auto config = options();
    config.pollIntervalMs = 100;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, config);
    AccountPageWidget page(&service);
    page.resize(640, 640);
    page.show();
    auto* paid = page.findChild<AdButton*>(QStringLiteral("accountRefreshPayment"));
    int automaticPolls = 0;
    bool manualQueryStarted = false;
    QObject::connect(&service, &AccountSessionService::changed, &page, [&] {
        if (service.purchase().polling() && !manualQueryStarted) {
            ++automaticPolls;
            require(!paid->busy(), "automatic payment polling never shows button loading");
            require(paid->isEnabled(), "automatic polling keeps the payment action enabled");
            require(paid->buttonStyle() == AdButton::ButtonStyle::Solid &&
                        paid->accentRole() == AdButton::AccentRole::Primary,
                    "automatic polling preserves the primary payment action");
        }
    });
    service.registerAccount(QStringLiteral("TestUser"), QStringLiteral("long-password"),
                            QStringLiteral("long-password"), QStringLiteral("abcd"));
    await([&] { return automaticPolls > 0 && !service.purchase().polling(); });
    require(!paid->busy() && paid->isEnabled(),
            "the payment action is available after an automatic poll");
    manualQueryStarted = true;
    paid->click();
    require(paid->busy() && !paid->isEnabled(),
            "an explicit payment query shows loading and prevents duplicate clicks");
    await([&] { return api.manualQuery && !service.purchase().polling(); });
    require(!paid->busy() && paid->isEnabled(),
            "manual payment query completion clears button loading");
    const int previousPolls = api.polls;
    api.manualQuery = false;
    service.refreshPurchase();
    require(!paid->busy() && paid->isEnabled(),
            "starting another background query leaves the payment action unchanged");
    paid->click();
    require(paid->busy() && !paid->isEnabled(),
            "clicking during a background query starts manual-query feedback");
    paid->click();
    await([&] { return api.manualQuery && !service.purchase().polling(); });
    require(
        api.polls == previousPolls + 2 && !paid->busy() && paid->isEnabled(),
        "a background query serializes one requested manual query and ignores duplicate clicks");
    api.rejectPolls = true;
    paid->click();
    require(paid->busy(), "a new manual payment query shows loading");
    await([&] { return !service.purchase().polling(); });
    require(!paid->busy() && paid->isEnabled() && service.purchase().pending(),
            "a failed manual query clears loading while retaining the pending payment");
}

void topUpCanBeCancelledAndRestarted() {
    using namespace adqt::widgets;
    for (const bool failedPoll : {false, true}) {
        Api api;
        QTemporaryDir secrets;
        auto config = options();
        config.pollIntervalMs = 60000;
        config.persistSecrets = true;
        config.secretDirectory = secrets.path();
        config.profileIdentity = secrets.path();
        SnowShotApiClient client(api.origin());
        AccountSessionService service(client, config);
        AccountPageWidget page(&service);
        page.resize(640, 640);
        page.show();
        service.signIn(QStringLiteral("TestUser"), QStringLiteral("long-password"));
        await([&] { return !service.account().isEmpty(); });
        auto* topUp = page.findChild<AdButton*>(QStringLiteral("accountTopUp"));
        auto* cancel = page.findChild<AdButton*>(QStringLiteral("accountDismissPurchase"));
        auto* paid = page.findChild<AdButton*>(QStringLiteral("accountRefreshPayment"));
        auto* payment = page.findChild<QWidget*>(QStringLiteral("accountPaymentCard"));
        topUp->click();
        await([&] { return !service.purchase().request().isEmpty(); });
        require(cancel->isVisibleTo(&page) && cancel->text() == u"Cancel payment",
                "pending top-ups provide a cancel-payment action");
        for (const int width : {640, 400, 260}) {
            page.resize(width, 640);
            QApplication::processEvents();
            require(paid->geometry().top() == cancel->geometry().top() &&
                        paid->geometry().right() < cancel->geometry().left() &&
                        payment->rect().contains(cancel->geometry()),
                    "payment and cancellation actions fit in one row at narrow widths");
        }
        api.holdPurchasePolls = true;
        api.state = QStringLiteral("fulfilled");
        api.rejectPolls = failedPoll;
        service.refreshPurchase();
        await([&] { return api.heldPurchasePolls.size() == 1; });
        QPointer<QNetworkReply> oldPoll;
        for (auto* reply : service.findChildren<QNetworkReply*>())
            if (reply->url().path().endsWith(u"/purchase1"))
                oldPoll = reply;
        require(oldPoll, "hold the previous purchase's in-flight status response");
        bool oldPollFinished = false;
        QObject::connect(oldPoll.data(), &QNetworkReply::finished, &page,
                         [&] { oldPollFinished = true; });
        paid->click();
        api.state = QStringLiteral("awaiting_payment");
        api.rejectPolls = false;
        cancel->click();
        cancel->click();
        service.cancelPurchase();
        require(service.purchase().cancelling() && cancel->busy() && !cancel->isEnabled() &&
                    !paid->isEnabled(),
                "cancellation shows progress and prevents duplicate payment actions");
        await([&] { return !service.purchase().cancelling(); });
        require(api.cancellations == 1 && api.officialAuthSeen &&
                    service.purchase().request().isEmpty() && !service.purchase().polling() &&
                    !service.purchase().queryingPlatform() && payment->isHidden() &&
                    topUp->isEnabled() && service.signedIn(),
                "confirmed cancellation stops polling and returns to the signed-in account");
        {
            SnowShotApiClient restoredClient(api.origin());
            AccountSessionService restored(restoredClient, config);
            await([&] { return restored.signedIn(); });
            require(restored.purchase().request().isEmpty(),
                    "a canceled top-up is not restored after restarting");
        }
        topUp->click();
        await([&] {
            return service.purchase().request().value(QStringLiteral("request_id")).toString() ==
                   u"purchase2";
        });
        service.refreshPurchase();
        await([&] { return api.heldPurchasePolls.size() == 2; });
        api.heldPurchasePolls.takeFirst()();
        await([&] { return oldPollFinished; });
        require(service.purchase().polling() && service.error().isEmpty() &&
                    service.purchase().request().value(QStringLiteral("request_id")).toString() ==
                        u"purchase2" &&
                    service.purchase().request().value(QStringLiteral("state")).toString() ==
                        u"awaiting_payment",
                "late canceled-purchase responses cannot change the new purchase or its polling");
        api.heldPurchasePolls.takeFirst()();
        await([&] { return !service.purchase().polling(); });
        cancel->click();
        await([&] { return service.purchase().request().isEmpty(); });
        require(api.cancellations == 2, "a replacement top-up can also be canceled");
    }
}

void topUpCancellationPreservesUnconfirmedOrCompletedPayments() {
    using namespace adqt::widgets;
    for (const auto* result :
         {"payment_pending", "reconciliation_required", "fulfilled", "request_failed"}) {
        Api api;
        auto config = options();
        config.pollIntervalMs = 60000;
        SnowShotApiClient client(api.origin());
        AccountSessionService service(client, config);
        AccountPageWidget page(&service);
        page.resize(640, 640);
        page.show();
        service.signIn(QStringLiteral("TestUser"), QStringLiteral("long-password"));
        await([&] { return !service.account().isEmpty() && !service.transactions().isEmpty(); });
        service.beginTopUp();
        await([&] { return !service.purchase().request().isEmpty(); });
        const bool failed = QByteArray(result) == "request_failed";
        if (failed) {
            api.failurePath = QStringLiteral("/api/v2/purchases/requests/purchase1/cancel");
            api.failureCode = QStringLiteral("request_failed");
            api.failureStatus = 503;
        } else {
            api.state = QString::fromLatin1(result);
        }
        api.balance = QStringLiteral("188000000000");
        auto* cancel = page.findChild<AdButton*>(QStringLiteral("accountDismissPurchase"));
        cancel->click();
        await([&] { return !service.purchase().cancelling(); });
        require(!service.purchase().request().isEmpty() && !cancel->busy(),
                "failed cancellation or an issued payment retains its purchase and clears loading");
        if (failed) {
            require(!service.error().isEmpty() && service.purchase().pending(),
                    "a cancellation error keeps the code available for retry");
            api.failurePath.clear();
            cancel->click();
            await([&] { return service.purchase().request().isEmpty(); });
            continue;
        }
        if (api.state != u"fulfilled") {
            require(service.purchase().pending() && !service.purchase().notice().isEmpty(),
                    "issued uncertain payments retain polling and explain why cancellation failed");
            service.beginTopUp();
            require(api.purchases == 1, "uncertain payments cannot be replaced by another top-up");
            api.state = QStringLiteral("fulfilled");
            service.refreshPurchase(true);
        }
        await([&] {
            return !service.purchase().pending() &&
                   service.account().value(QStringLiteral("available_credit_nano_yuan")) ==
                       api.balance;
        });
        require(cancel->text() == u"Continue",
                "a completed payment remains visible until dismissed");
        cancel->click();
        require(service.purchase().request().isEmpty(), "completed purchases can be dismissed");
    }
}

void registrationRequiresExplicitAgreement() {
    using namespace adqt::widgets;
    Api api;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    page.resize(400, 640);
    page.show();
    page.activateWindow();
    auto* agreement = page.findChild<AdCheckbox*>(QStringLiteral("accountAgreement"));
    auto* text = page.findChild<QLabel*>(QStringLiteral("accountAgreementText"));
    auto* submit = page.findChild<AdButton*>(QStringLiteral("accountSubmit"));
    auto* toggle = page.findChild<AdButton*>(QStringLiteral("accountSwitchForm"));
    auto* username = page.findChild<QLineEdit*>(QStringLiteral("accountUsername"));
    auto* password = page.findChild<QLineEdit*>(QStringLiteral("accountPassword"));
    auto* confirmation = page.findChild<QLineEdit*>(QStringLiteral("accountPasswordConfirmation"));
    auto* captcha = page.findChild<QLineEdit*>(QStringLiteral("accountCaptchaAnswer"));
    require(agreement && text && !agreement->isVisibleTo(&page) && submit->isEnabled(),
            "sign-in does not require registration consent");
    toggle->click();
    await([&] { return !service.captcha().isEmpty(); });
    require(agreement->isVisibleTo(&page) && !agreement->isChecked() && !submit->isEnabled(),
            "registration requires consent without preselecting the checkbox");
    username->setText(QStringLiteral("TestUser"));
    password->setText(QStringLiteral("long-password"));
    confirmation->setText(QStringLiteral("long-password"));
    captcha->setText(QStringLiteral("abcd"));
    submit->click();
    for (auto* editor : {username, password, confirmation, captcha}) {
        require(QMetaObject::invokeMethod(editor, "returnPressed", Qt::DirectConnection),
                "exercise every editor's keyboard submission path");
        QApplication::processEvents();
        require(!service.busy() && service.purchase().request().isEmpty() &&
                    api.registrations == 0 && agreement->hasFocus(),
                "Enter cannot bypass consent and focuses the agreement checkbox");
    }
    require(text->openExternalLinks() &&
                text->textInteractionFlags().testFlag(Qt::LinksAccessibleByKeyboard) &&
                text->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse),
            "both agreement documents can be opened with the mouse or keyboard");
    for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "load translated registration consent");
        QApplication::installTranslator(&translator);
        QApplication::processEvents();
        const QString expected = AccountPageWidget::tr("I have read and agree to the %1 and %2.")
                                     .arg(AccountPageWidget::tr("User agreement"),
                                          AccountPageWidget::tr("Privacy policy"));
        QTextDocument document;
        document.setHtml(text->text());
        require(
            document.toPlainText() == expected && agreement->accessibleName() == expected &&
                text->text().contains(QStringLiteral("href=\"https://snowshot.top/terms.html\"")) &&
                text->text().contains(QStringLiteral("href=\"%1\"")
                                          .arg(snow_shot::app::distribution::privacyPolicyUrl())),
            "consent copy retranslates while both document destinations stay stable");
        for (const int width : {400, 260}) {
            page.resize(width, 640);
            for (int index = 0; index < 5; ++index)
                QApplication::processEvents();
            require(text->height() >= text->heightForWidth(text->width()) &&
                        page.findChild<PageContainerWidget*>()
                                ->scrollArea()
                                ->horizontalScrollBar()
                                ->maximum() == 0,
                    "translated consent wraps without clipping or horizontal scrolling");
        }
        QApplication::removeTranslator(&translator);
    }
    agreement->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(agreement, &press);
    QCoreApplication::sendEvent(agreement, &release);
    require(agreement->isChecked() && submit->isEnabled(),
            "keyboard consent enables registration immediately");
    agreement->setChecked(false);
    require(!submit->isEnabled(), "withdrawing consent disables registration immediately");
    agreement->setChecked(true);
    toggle->click();
    require(!agreement->isVisibleTo(&page) && submit->isEnabled(),
            "switching to sign-in removes the registration consent requirement");
    toggle->click();
    require(!agreement->isChecked() && !submit->isEnabled(),
            "a new registration form requires fresh consent");
    password->setText(QStringLiteral("long-password"));
    confirmation->setText(QStringLiteral("long-password"));
    captcha->setText(QStringLiteral("abcd"));
    agreement->setChecked(true);
    submit->click();
    require(!agreement->isEnabled() && !submit->isEnabled(),
            "in-flight registration freezes consent and prevents duplicate submissions");
    await([&] { return !service.purchase().request().isEmpty(); });
    require(api.registrations == 1, "explicit consent permits exactly one registration request");
}

void registrationDraftSurvivesExpiredPurchase() {
    using namespace adqt::widgets;
    Api api;
    api.expirySeconds = -120;
    auto config = options();
    config.pollIntervalMs = 60000;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, config);
    AccountPageWidget page(&service);
    page.resize(640, 640);
    page.show();
    auto* username = page.findChild<QLineEdit*>(QStringLiteral("accountUsername"));
    auto* password = page.findChild<QLineEdit*>(QStringLiteral("accountPassword"));
    auto* confirmation = page.findChild<QLineEdit*>(QStringLiteral("accountPasswordConfirmation"));
    auto* captcha = page.findChild<QLineEdit*>(QStringLiteral("accountCaptchaAnswer"));
    auto* submit = page.findChild<AdButton*>(QStringLiteral("accountSubmit"));
    auto* dismiss = page.findChild<AdButton*>(QStringLiteral("accountDismissPurchase"));
    auto* code = page.findChild<QLabel*>(QStringLiteral("accountMiniProgramCode"));
    page.findChild<AdButton*>(QStringLiteral("accountSwitchForm"))->click();
    await([&] { return !service.captcha().isEmpty(); });
    username->setText(QStringLiteral("DraftUser"));
    auto* agreement = page.findChild<AdCheckbox*>(QStringLiteral("accountAgreement"));
    agreement->setChecked(true);
    password->setText(QStringLiteral("long-password"));
    confirmation->setText(QStringLiteral("long-password"));
    captcha->setText(QStringLiteral("abcd"));
    submit->click();
    await([&] { return !service.purchase().request().isEmpty(); });
    require(!username->isVisibleTo(&page) && code->isVisibleTo(&page),
            "the payment code temporarily replaces the registration form");
    require(username->text() == u"DraftUser" && password->text() == u"long-password" &&
                confirmation->text() == u"long-password",
            "showing the payment code preserves the registration credentials");
    require(service.purchase().remainingSeconds() == 0 && dismiss->isHidden(),
            "the local countdown alone cannot discard a pending registration");
    api.state = QStringLiteral("payment_pending");
    service.refreshPurchase();
    await([&] { return !service.purchase().polling(); });
    require(!username->isVisibleTo(&page) && dismiss->isHidden(),
            "payment confirmation keeps the registration draft hidden beyond code expiry");
    api.state = QStringLiteral("expired");
    service.refreshPurchase();
    await([&] { return !service.purchase().pending(); });
    require(dismiss->isVisibleTo(&page) && !code->isVisibleTo(&page),
            "confirmed expiry offers a return action and hides the expired code");
    const int previousCaptchas = api.captchas;
    dismiss->click();
    await([&] { return api.captchas > previousCaptchas && !service.captcha().isEmpty(); });
    require(service.purchase().request().isEmpty() && confirmation->isVisibleTo(&page) &&
                captcha->isVisibleTo(&page) && submit->text() == u"Register and continue",
            "returning from an expired code restores registration instead of sign-in");
    require(username->text() == u"DraftUser" && password->text() == u"long-password" &&
                confirmation->text() == u"long-password" && captcha->text().isEmpty(),
            "registration retry retains credentials and requires only a fresh image code");
    require(agreement->isChecked(), "retrying the same registration draft retains its consent");
    captcha->setText(QStringLiteral("efgh"));
    api.state = QStringLiteral("awaiting_payment");
    submit->click();
    await([&] { return api.registrations == 2 && !service.busy(); });
    const auto submitted = QJsonDocument::fromJson(api.registrationBody).object();
    require(submitted.value(QStringLiteral("username")).toString() == u"DraftUser" &&
                submitted.value(QStringLiteral("password")).toString() == u"long-password" &&
                submitted.value(QStringLiteral("password_confirmation")).toString() ==
                    u"long-password" &&
                submitted.value(QStringLiteral("captcha_answer")).toString() == u"efgh",
            "registration can generate another code using the retained form values");
    api.state = QStringLiteral("fulfilled");
    service.refreshPurchase();
    await([&] { return service.signedIn(); });
    require(password->text().isEmpty() && confirmation->text().isEmpty() &&
                captcha->text().isEmpty(),
            "successful activation clears the registration secrets");
    require(!agreement->isChecked(), "successful activation clears registration consent");
}

void reopenedRegistrationPurchaseReturnsToRegistration() {
    using namespace adqt::widgets;
    for (const bool alreadyExpired : {false, true}) {
        Api api;
        auto config = options();
        config.pollIntervalMs = 60000;
        SnowShotApiClient client(api.origin());
        AccountSessionService service(client, config);
        service.registerAccount(QStringLiteral("RecoveredUser"), QStringLiteral("long-password"),
                                QStringLiteral("long-password"), QStringLiteral("abcd"));
        await([&] { return !service.purchase().request().isEmpty(); });
        if (alreadyExpired) {
            api.state = QStringLiteral("expired");
            service.refreshPurchase();
            await([&] { return !service.purchase().pending(); });
        }
        AccountPageWidget page(&service);
        page.resize(640, 640);
        page.show();
        if (!alreadyExpired) {
            api.state = QStringLiteral("expired");
            service.refreshPurchase();
            await([&] { return !service.purchase().pending(); });
        }
        page.findChild<AdButton*>(QStringLiteral("accountDismissPurchase"))->click();
        await([&] { return !service.captcha().isEmpty(); });
        auto* username = page.findChild<QLineEdit*>(QStringLiteral("accountUsername"));
        require(username->text() == u"RecoveredUser" &&
                    page.findChild<QLineEdit*>(QStringLiteral("accountPasswordConfirmation"))
                        ->isVisibleTo(&page) &&
                    page.findChild<AdButton*>(QStringLiteral("accountSubmit"))->text() ==
                        u"Register and continue",
                "a reopened registration purchase returns to registration with its username");
    }
}

void registrationProofSurvivesRestartAndDoesNotOverrideStoredSession() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    Api api;
    SnowShotApiClient client(api.origin());
    QTemporaryDir directory;
    auto config = options();
    config.persistSecrets = true;
    config.secretDirectory = directory.path();
    config.profileIdentity = directory.path();
    {
        AccountSessionService original(client, config);
        original.registerAccount(QStringLiteral("TestUser"), QStringLiteral("long-password"),
                                 QStringLiteral("long-password"), QStringLiteral("abcd"));
        await([&] { return !original.purchase().request().isEmpty(); });
    }
    api.state = QStringLiteral("fulfilled");
    {
        AccountSessionService recovered(client, config);
        await([&] { return recovered.signedIn(); });
        require(api.claims == 1 && api.proofSeen,
                "restart recovers the original private activation proof");
        recovered.signOut();
    }
    // A previous explicit session choice always wins over stale persisted auto-claim intent.
    const QString identity =
        QCoreApplication::applicationName() + u'|' + config.profileIdentity + u'|' + api.origin();
    snow_shot::storage::AccountSecretStore secrets(identity, directory.path());
    auto stored = api.purchase();
    require(secrets.write(QJsonDocument(QJsonObject{{QStringLiteral("refresh_token"),
                                                     QStringLiteral("chosen-session")},
                                                    {QStringLiteral("purchase"), stored},
                                                    {QStringLiteral("registration_proof"),
                                                     QStringLiteral("private-proof")},
                                                    {QStringLiteral("registration"), true},
                                                    {QStringLiteral("auto_claim"), true}})
                              .toJson()),
            "seed stale competing account intents");
    {
        AccountSessionService recovered(client, config);
        await([&] { return recovered.signedIn(); });
        require(api.claims == 1 && api.refreshes == 1,
                "restoring an existing session never races an old activation claim");
        recovered.signOut();
    }
    require(secrets.remove(), "remove test account secrets");
#endif
}

void tokenRefreshPreservesQueuedOfficialCalls() {
    translation_tests::Server server;
    server.streamRejection = [](int index) {
        if (index != 0)
            return QByteArray();
        const QByteArray body = R"({"code":"rate_limited","retry_after_seconds":1})";
        return QByteArray("HTTP/1.1 429 Too Many Requests\r\nContent-Type: "
                          "application/problem+json\r\nRetry-After: 1\r\nConnection: "
                          "close\r\nContent-Length: ") +
               QByteArray::number(body.size()) + "\r\n\r\n" + body;
    };
    SnowShotApiClient client(server.url());
    client.setAccountAccessToken("original-session");
    int completions = 0;
    const auto token = client.streamTranslation(
        {QStringLiteral("general"), QStringLiteral("en"), QStringLiteral("de"),
         QStringLiteral("Hello")},
        &client, [](const QString&) {},
        [&](const auto& result) {
            require(result.succeeded(), "same-account token refresh preserves queued work");
            ++completions;
        });
    await([&] { return server.streams.size() == 1 && client.isQueued(token); });
    client.setAccountAccessToken("rotated-session", true);
    await([&] { return server.streams.size() == 2; });
    const auto requestId = [](const QByteArray& headers) {
        for (const auto& line : headers.split('\n'))
            if (line.toLower().startsWith("x-request-id:"))
                return line.trimmed();
        return QByteArray();
    };
    require(
        server.streams.at(1).headers.contains("Authorization: Bearer rotated-session") &&
            requestId(server.streams.at(0).headers) == requestId(server.streams.at(1).headers),
        "token rotation preserves billing identity and refreshes only queued official credentials");
    server.delta(1, QStringLiteral("Hallo"));
    server.finish(1);
    await([&] { return completions == 1; });
}

void accountRequestsFollowSystemProxyPreference() {
    Api api;
    SnowShotApiClient client(api.origin());
    client.setUseSystemProxy(true);
    AccountSessionService service(client, options());
    service.loadCaptcha();
    auto* network = service.findChild<QNetworkAccessManager*>();
    require(network && network->proxyFactory(), "account requests honor system proxy resolution");
    // Do not depend on the machine's configured proxy reaching the local fixture.
    service.signOut();
    client.setUseSystemProxy(false);
    service.loadCaptcha();
    require(network->proxyFactory() == nullptr && network->proxy().type() == QNetworkProxy::NoProxy,
            "disabling system proxy returns account requests to direct connections");
    await([&] { return !service.captcha().isEmpty(); });
}

void accountUiAndCredits() {
    const auto creditsError =
        SnowShotApiClient::formatFailure(402, QStringLiteral("insufficient_credits"), {});
    const auto debtError =
        SnowShotApiClient::formatFailure(402, QStringLiteral("outstanding_balance"), {});
    const auto reservedError =
        SnowShotApiClient::formatFailure(429, QStringLiteral("funds_temporarily_reserved"), {});
    require(creditsError.contains(QStringLiteral("cannot cover this request")) &&
                debtError.contains(QStringLiteral("outstanding balance")) &&
                reservedError.contains(QStringLiteral("reserved by other calls")),
            "billing failures explain distinct insufficient funds, debt, and concurrent holds");
    require(AccountSessionService::formatCredits(QStringLiteral("9900000000")) == u"990 credits",
            "credits use exactly 100 credits per yuan");
    require(AccountSessionService::formatCredits(QStringLiteral("-1234567")) == u"0 credits",
            "sub-credit debits display zero without a negative-zero sign");
    require(AccountSessionService::formatCredits(QStringLiteral("1")) == u"0 credits",
            "sub-credit amounts omit decimals");
    require(AccountSessionService::formatCredits(QStringLiteral("-12500000")) == u"−1 credits" &&
                AccountSessionService::formatCredits(QStringLiteral("19999999")) == u"1 credits" &&
                AccountSessionService::formatCredits(QStringLiteral("-19999999")) == u"−1 credits",
            "credit amounts discard fractions toward zero");
    require(AccountSessionService::formatCredits(QStringLiteral("9223372036854775807")) ==
                    u"922337203685 credits" &&
                AccountSessionService::formatCredits(QStringLiteral("-9223372036854775808")) ==
                    u"−922337203685 credits",
            "credit conversion preserves both signed integer limits without overflow");
    require(AccountSessionService::formatCredits(QString()) == u"0 credits" &&
                AccountSessionService::formatCredits(QStringLiteral("invalid")) == u"0 credits",
            "missing and invalid credit values display zero");
    Api api;
    api.debt = QStringLiteral("1000000000");
    api.balance = QStringLiteral("0");
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    page.resize(400, 400);
    page.show();
    auto* username = page.findChild<adqt::widgets::AdLineEdit*>(QStringLiteral("accountUsername"));
    auto* submit = page.findChild<adqt::widgets::AdButton*>(QStringLiteral("accountSubmit"));
    require(username && submit && !username->accessibleName().isEmpty(),
            "account inputs have stable names and accessible labels");
    service.signIn(QStringLiteral("TestUser"), QStringLiteral("long-password"));
    await([&] { return !service.account().isEmpty() && !service.transactions().isEmpty(); });
    QApplication::processEvents();
    auto* balance = page.findChild<QLabel*>(QStringLiteral("accountAvailableCredit"));
    require(balance && balance->text() == u"0" &&
                balance->accessibleName() == u"Available credits" &&
                balance->accessibleDescription() == u"0 credits" &&
                balance->textInteractionFlags().testFlag(Qt::TextSelectableByMouse),
            "balance displays selectable digits with an accessible credit unit");
    const auto* coin = page.findChild<QLabel*>(QStringLiteral("accountCreditIcon"));
    require(coin && coin->isVisible() && !coin->pixmap().isNull() &&
                coin->parentWidget() == balance->parentWidget() &&
                coin->geometry().right() < balance->geometry().left(),
            "a rendered coin sits to the left of the available credit amount");
    auto* cumulative = page.findChild<QLabel*>(QStringLiteral("accountCumulativeRecharge"));
    require(
        cumulative && cumulative->text() == u"Total top-ups: 18800 credits",
        "cumulative recharge uses the account total independently of balance and transaction page");
    auto* debt =
        page.findChild<adqt::widgets::AdAlert*>(QStringLiteral("accountOutstandingBalance"));
    require(debt && !debt->isHidden() && debt->text() == u"Outstanding balance: 100 credits",
            "outstanding balances show a paid-call warning");
    for (const auto* label : page.findChildren<QLabel*>()) {
        require(!label->text().contains(QStringLiteral("No expiration")) &&
                    !label->text().contains(QStringLiteral("never expire")),
                "account overview omits permanent membership and credit descriptions");
    }
    struct LocalizedCredits {
        const char* locale;
        const char16_t* balance;
        const char16_t* cumulative;
        const char16_t* debt;
        const char16_t* refund;
    };
    for (const auto& expected : {
             LocalizedCredits{"en_US", u"0 credits", u"Total top-ups: 18800 credits",
                              u"Outstanding balance: 100 credits", u"−990 credits"},
             LocalizedCredits{"zh_CN", u"0 积分", u"累计充值：18800 积分", u"待偿还欠款：100 积分",
                              u"−990 积分"},
             LocalizedCredits{"zh_TW", u"0 積分", u"累計儲值：18800 積分", u"待償還欠款：100 積分",
                              u"−990 積分"},
         }) {
        QTranslator translator;
        require(
            translator.load(
                QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(expected.locale))),
            "load credit display translations");
        QApplication::installTranslator(&translator);
        QApplication::processEvents();
        require(balance->text() == u"0" &&
                    balance->accessibleDescription() == QStringView(expected.balance) &&
                    balance->parentWidget()->toolTip() ==
                        AccountPageWidget::tr("Available credits") &&
                    cumulative->text() == QStringView(expected.cumulative) &&
                    debt->text() == QStringView(expected.debt),
                "language changes retranslate all account credit summaries");
        const auto* refund = page.findChild<QWidget*>(QStringLiteral("accountTransaction-refund1"))
                                 ->findChild<QLabel*>(QStringLiteral("accountTransactionAmount"));
        require(refund && refund->text() == QStringView(expected.refund),
                "transaction amounts use localized credit units");
        QApplication::removeTranslator(&translator);
        QApplication::processEvents();
    }
    bool processing = false;
    for (const auto* tag : page.findChildren<adqt::widgets::AdTag*>())
        processing =
            processing || (tag->text() == u"Processing" &&
                           tag->colorScheme() == adqt::widgets::AdTag::ColorScheme::Processing);
    require(processing, "accepted refund is displayed as processing, never completed");
    api.emptyTransactions = true;
    service.loadTransactions();
    await([&] { return service.transactions().isEmpty(); });
    require(cumulative->text() == u"Total top-ups: 18800 credits",
            "empty transaction history does not reset cumulative recharge");
    api.cumulativeRecharge = QStringLiteral("88800000000");
    service.refreshAccount();
    await([&] { return cumulative->text() == u"Total top-ups: 8880 credits"; });
    for (const auto& amount : {
             std::pair{QStringLiteral("1"), QStringLiteral("0")},
             std::pair{QStringLiteral("12500000"), QStringLiteral("1")},
             std::pair{QStringLiteral("19999999"), QStringLiteral("1")},
             std::pair{QStringLiteral("9223372036854775807"), QStringLiteral("922337203685")},
         }) {
        api.balance = amount.first;
        service.refreshAccount();
        await([&] {
            return !service.busy() &&
                   service.account().value(QStringLiteral("available_credit_nano_yuan")) ==
                       amount.first &&
                   balance->text() == amount.second;
        });
        require(balance->accessibleDescription() == amount.second + QStringLiteral(" credits"),
                "balance refresh uses integer credits while retaining the exact account value");
    }
    page.resize(256, 220);
    QApplication::processEvents();
    require(page.minimumSizeHint().width() <= 256, "account page supports narrow desktop content");
}

void accountTransactionsRemainReadableWhenResized() {
    using namespace adqt::widgets;
    namespace styles = snow_shot::presentation::styles;
    Api api;
    api.debt = QStringLiteral("1200000000");
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    page.resize(900, 640);
    page.show();
    service.signIn(QStringLiteral("TestUser"), QStringLiteral("long-password"));
    await([&] { return service.transactions().size() == 3; });
    const auto verify = [&] {
        for (int index = 0; index < 5; ++index)
            QApplication::processEvents();
        auto* scroll = page.findChild<PageContainerWidget*>()->scrollArea();
        require(scroll->horizontalScrollBar()->maximum() == 0,
                "account details never require horizontal scrolling");
        const auto* coin = page.findChild<QLabel*>(QStringLiteral("accountCreditIcon"));
        const auto* balance = page.findChild<QLabel*>(QStringLiteral("accountAvailableCredit"));
        require(coin && balance && !coin->pixmap().isNull() &&
                    coin->pixmap().devicePixelRatio() == coin->devicePixelRatioF() &&
                    coin->pixmap().size() ==
                        QSize(qRound(coin->width() * coin->devicePixelRatioF()),
                              qRound(coin->height() * coin->devicePixelRatioF())) &&
                    coin->parentWidget()->rect().contains(coin->geometry()) &&
                    balance->parentWidget()->rect().contains(balance->geometry()) &&
                    coin->geometry().right() < balance->geometry().left(),
                "the credit coin and amount stay sharp and separate across widths and font sizes");
        const auto* debt = page.findChild<AdAlert*>(QStringLiteral("accountOutstandingBalance"));
        for (const auto* label : debt->findChildren<QLabel*>()) {
            if (!label->isHidden() && !label->text().isEmpty()) {
                if (label->height() < label->heightForWidth(label->width()))
                    std::cerr << "Warning text clipped at page " << page.width() << ", font "
                              << label->font().pixelSize() << ", "
                              << label->objectName().toStdString() << ": width=" << label->width()
                              << " height=" << label->height()
                              << " required=" << label->heightForWidth(label->width())
                              << " alertHeight=" << debt->height()
                              << " alertHfw=" << debt->heightForWidth(debt->width())
                              << " hostHeight=" << label->parentWidget()->height() << " hostHfw="
                              << label->parentWidget()->heightForWidth(label->width()) << '\n';
                require(label->height() >= label->heightForWidth(label->width()),
                        "outstanding balance warnings show their complete wrapped text");
            }
        }
        auto* row = page.findChild<QWidget*>(QStringLiteral("accountTransaction-refund1"));
        const auto widgets = row->findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
        for (const auto* child : widgets) {
            if (child->isHidden())
                continue;
            require(row->rect().contains(child->geometry()),
                    "transaction fields fit inside their row");
            for (const auto* other : widgets)
                if (child != other && !other->isHidden())
                    require(!child->geometry().intersects(other->geometry()),
                            "transaction details do not overlap at any content width");
        }
        const auto* amount = row->findChild<QLabel*>(QStringLiteral("accountTransactionAmount"));
        require(amount &&
                    amount->text() ==
                        AccountSessionService::formatCredits(QStringLiteral("-9900000000")) &&
                    amount->textInteractionFlags().testFlag(Qt::TextSelectableByMouse),
                "responsive transactions retain exact, selectable amounts");
        const auto* status = row->findChild<AdTag*>(QStringLiteral("accountTransactionStatus"));
        require(status && status->colorScheme() == AdTag::ColorScheme::Processing &&
                    status->focusPolicy() == Qt::NoFocus,
                "transaction states remain informative without adding a keyboard stop");
        require(row->findChild<QLabel*>(QStringLiteral("accountTransactionOrder"))
                    ->text()
                    .contains(QStringLiteral("order1")),
                "transaction order identifiers remain available in compact layouts");
    };
    for (const int width : {900, 640, 400, 292, 260}) {
        page.resize(width, 640);
        verify();
    }
    for (const auto* locale : {"zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "load localized account transaction details");
        QApplication::installTranslator(&translator);
        for (const int width : {900, 260}) {
            page.resize(width, 640);
            verify();
        }
        QApplication::removeTranslator(&translator);
    }
    styles::ThemeStyleConfig normal;
    normal.appFont = QApplication::font();
    auto large = normal;
    large.fontSize = 21;
    large.appFont.setPixelSize(21);
    styles::ThemeManager::instance().setThemeStyleConfig(large);
    for (const int width : {900, 400, 260}) {
        page.resize(width, 640);
        verify();
    }
    styles::ThemeManager::instance().setThemeStyleConfig(normal);
    api.emptyTransactions = true;
    service.loadTransactions();
    await([&] { return service.transactions().isEmpty(); });
    QApplication::processEvents();
    require(
        page.findChild<QLabel*>(QStringLiteral("accountTransactionsEmpty"))->isVisibleTo(&page) &&
            page.findChild<QWidget*>(QStringLiteral("accountTransactionHeader"))->isHidden(),
        "empty account history provides guidance without unused column headings");
}

void emptyAccountTransactionsMatchScreenshotHistory() {
    namespace styles = snow_shot::presentation::styles;
    Api api;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    ScreenshotHistoryPageWidget historyPage;
    page.resize(900, 800);
    page.show();
    service.signIn(QStringLiteral("TestUser"), QStringLiteral("long-password"));
    await([&] { return service.transactions().size() == 3; });
    api.emptyTransactions = true;
    service.loadTransactions();
    await([&] { return service.transactions().isEmpty(); });
    const auto verify = [&] {
        for (int index = 0; index < 5; ++index)
            QApplication::processEvents();
        auto* icon = page.findChild<QLabel*>(QStringLiteral("accountTransactionsEmptyIcon"));
        auto* title = page.findChild<QLabel*>(QStringLiteral("accountTransactionsEmpty"));
        auto* description =
            page.findChild<QLabel*>(QStringLiteral("accountTransactionsEmptyDescription"));
        require(icon && title && description && icon->isVisibleTo(&page) &&
                    title->isVisibleTo(&page) && description->isVisibleTo(&page),
                "empty account transactions show an icon, title, and description");
        const auto* historyIcon =
            historyPage.findChild<QLabel*>(QStringLiteral("screenshotHistoryEmptyIcon"));
        const auto* historyTitle =
            historyPage.findChild<QLabel*>(QStringLiteral("screenshotHistoryEmptyTitle"));
        const auto* historyDescription =
            historyPage.findChild<QLabel*>(QStringLiteral("screenshotHistoryEmptyDescription"));
        require(!icon->pixmap().isNull() && icon->size() == historyIcon->size() &&
                    icon->pixmap().toImage() == historyIcon->pixmap().toImage() &&
                    title->font() == historyTitle->font() &&
                    title->palette().color(QPalette::WindowText) ==
                        historyTitle->palette().color(QPalette::WindowText) &&
                    description->palette().color(QPalette::WindowText) ==
                        historyDescription->palette().color(QPalette::WindowText),
                "account empty states match screenshot history icons, titles, and colors");
        require(icon->geometry().bottom() < title->geometry().top() &&
                    title->geometry().bottom() < description->geometry().top(),
                "empty account state content remains vertically separated");
        for (const auto* label : {title, description})
            require(label->alignment() == Qt::AlignCenter &&
                        label->height() >= label->heightForWidth(label->width()),
                    "empty account state text stays centered and wraps without clipping");
        auto* scroll = page.findChild<PageContainerWidget*>()->scrollArea();
        require(scroll->horizontalScrollBar()->maximum() == 0,
                "empty account transactions fit narrow content without horizontal scrolling");
        require(page.findChild<QWidget*>(QStringLiteral("accountTransactionHeader"))->isHidden(),
                "empty account transactions hide unused column headings");
    };
    styles::ThemeStyleConfig normal;
    normal.appFont = QApplication::font();
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        auto theme = normal;
        theme.appearance = appearance;
        styles::ThemeManager::instance().setThemeStyleConfig(theme);
        historyPage.applyTheme(styles::ThemeManager::instance().themeColorScheme());
        for (const int width : {900, 260}) {
            page.resize(width, 800);
            verify();
        }
    }
    for (const auto* locale : {"zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "load localized empty account transaction guidance");
        QApplication::installTranslator(&translator);
        verify();
        require(page.findChild<QLabel*>(QStringLiteral("accountTransactionsEmpty"))->text() !=
                        u"No transactions yet" &&
                    page.findChild<QLabel*>(QStringLiteral("accountTransactionsEmptyDescription"))
                            ->text() != u"Top-ups, usage charges, and refunds appear here.",
                "language changes retranslate both empty account state labels");
        QApplication::removeTranslator(&translator);
    }
    styles::ThemeManager::instance().setThemeStyleConfig(normal);
    api.emptyTransactions = false;
    service.loadTransactions();
    await([&] { return service.transactions().size() == 3; });
    require(!page.findChild<QLabel*>(QStringLiteral("accountTransactionsEmptyIcon")) &&
                page.findChild<QWidget*>(QStringLiteral("accountTransactions"))->minimumHeight() ==
                    0,
            "loaded transactions replace the complete empty state and release its space");
}

void languageChangeRetranslatesCachedErrors() {
    Api api;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    page.show();
    page.findChild<adqt::widgets::AdButton*>(QStringLiteral("accountSwitchForm"))->click();
    service.registerAccount(QStringLiteral("TestUser"), QStringLiteral("long-password"),
                            QStringLiteral("mismatch"), QStringLiteral("abcd"));
    const auto english = service.fieldError(QStringLiteral("password_confirmation"));
    QTranslator translator;
    require(translator.load(QStringLiteral(":/i18n/snow_shot_zh_CN.qm")),
            "load account Chinese translations");
    QApplication::installTranslator(&translator);
    QApplication::processEvents();
    const auto translated = service.fieldError(QStringLiteral("password_confirmation"));
    require(translated != english && translated.contains(QStringLiteral("密码")),
            "stored service errors translate when language changes");
    auto* form = page.findChild<adqt::widgets::AdForm*>(QStringLiteral("accountForm"));
    require(form->field(QStringLiteral("password_confirmation"))->helpText() == translated,
            "visible failure text is retranslated without repeating the request");
    const QString longPassword(17, u'x');
    service.registerAccount(QStringLiteral("TestUser"), longPassword, longPassword,
                            QStringLiteral("abcd"));
    require(service.fieldError(QStringLiteral("password")) == u"密码长度须为 8–16 个字符。" &&
                page.findChild<QLineEdit*>(QStringLiteral("accountPassword"))->placeholderText() ==
                    u"8–16 个字符",
            "translated password errors and input hints use the current limits");
    QApplication::removeTranslator(&translator);
}

void accountFormsAdvanceOneEditorPerTab() {
    using namespace adqt::widgets;
    Api api;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    page.resize(400, 640);
    page.show();
    page.activateWindow();
    QApplication::processEvents();
    auto* username = page.findChild<QLineEdit*>(QStringLiteral("accountUsername"));
    auto* password = page.findChild<QLineEdit*>(QStringLiteral("accountPassword"));
    auto* confirmation = page.findChild<QLineEdit*>(QStringLiteral("accountPasswordConfirmation"));
    auto* captcha = page.findChild<QLineEdit*>(QStringLiteral("accountCaptchaAnswer"));
    auto* submit = page.findChild<AdButton*>(QStringLiteral("accountSubmit"));
    auto* toggle = page.findChild<AdButton*>(QStringLiteral("accountSwitchForm"));
    const auto tabTo = [](QWidget* expected, bool reverse = false) {
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, reverse ? Qt::ShiftModifier : Qt::NoModifier);
        QCoreApplication::sendEvent(QApplication::focusWidget(), &tab);
        require(QApplication::focusWidget() == expected,
                "a single Tab or Shift+Tab must activate the adjacent account field");
    };
    require(username && password && confirmation && captcha && submit && toggle,
            "account focus fixture exposes all fields");
    for (const bool registration : {false, true}) {
        if (registration) {
            toggle->click();
            await([&] { return !service.captcha().isEmpty(); });
        }
        for (auto* editor : {username, password, confirmation, captcha}) {
            editor->setText(QStringLiteral("value"));
        }
        username->setFocus();
        tabTo(password);
        if (registration) {
            tabTo(confirmation);
            tabTo(captcha);
            tabTo(confirmation, true);
        } else {
            tabTo(submit);
        }
        tabTo(password, true);
        tabTo(username, true);
    }
}

void accountFormValidationStaysWithFields() {
    using namespace adqt::widgets;
    Api api;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    page.resize(400, 640);
    page.show();
    QApplication::processEvents();
    auto* form = page.findChild<AdForm*>(QStringLiteral("accountForm"));
    auto* username = page.findChild<AdLineEdit*>(QStringLiteral("accountUsername"));
    auto* password = page.findChild<QLineEdit*>(QStringLiteral("accountPassword"));
    auto* confirmation = page.findChild<QLineEdit*>(QStringLiteral("accountPasswordConfirmation"));
    auto* captcha = page.findChild<AdLineEdit*>(QStringLiteral("accountCaptchaAnswer"));
    auto* submit = page.findChild<AdButton*>(QStringLiteral("accountSubmit"));
    auto* toggle = page.findChild<AdButton*>(QStringLiteral("accountSwitchForm"));
    auto* messages = AdMessageService::instance(&page);
    require(form && form->items().size() == 4, "account editors belong to form fields");
    submit->click();
    for (const auto& key : {QStringLiteral("username"), QStringLiteral("password")}) {
        auto* item = form->field(key);
        require(item->validateStatus() == AdFormItem::ValidateStatus::Error &&
                    !item->helpText().isEmpty() &&
                    item->controlWidget()->accessibleDescription() == item->helpText(),
                "empty sign-in fields display accessible validation hints");
    }
    require(username->hasFocus() && api.logins == 0 && messages->count() == 0 &&
                service.error().isEmpty(),
            "invalid sign-in focuses its first field without a request or message");
    username->setText(QStringLiteral("TestUser"));
    require(form->field(QStringLiteral("username"))->helpText().isEmpty() &&
                !form->field(QStringLiteral("password"))->helpText().isEmpty(),
            "editing clears only the affected field error");
    toggle->click();
    await([&] { return !service.captcha().isEmpty(); });
    for (auto* item : form->items())
        require(item->helpText().isEmpty(), "switching forms clears old validation hints");
    page.findChild<AdCheckbox*>(QStringLiteral("accountAgreement"))->setChecked(true);
    submit->click();
    require(!form->field(QStringLiteral("password_confirmation"))->helpText().isEmpty(),
            "registration requires password confirmation even when both password fields are empty");
    username->setText(QStringLiteral("!"));
    password->setText(QStringLiteral("short"));
    confirmation->setText(QStringLiteral("different"));
    submit->click();
    for (auto* item : form->items()) {
        auto* editor = qobject_cast<AdLineEdit*>(item->controlWidget());
        require(item->validateStatus() == AdFormItem::ValidateStatus::Error &&
                    !item->helpText().isEmpty() && editor->status() == AdLineEdit::Status::Error,
                "registration reports every invalid field and marks its editor");
    }
    require(api.registrations == 0 && messages->count() == 0,
            "invalid registration never sends a request or opens a message");
    page.resize(256, 360);
    QApplication::processEvents();
    require(page.minimumSizeHint().width() <= 256,
            "validation hints preserve the narrow account layout");
    username->setText(QStringLiteral("TestUser"));
    password->setText(QStringLiteral("long-password"));
    confirmation->setText(QStringLiteral("long-password"));
    captcha->setText(QStringLiteral("abcd"));
    submit->click();
    await([&] { return !service.purchase().request().isEmpty(); });
    require(api.registrations == 1 && messages->count() == 0,
            "corrected registration submits once without stale validation feedback");
}

void accountServerValidationStaysWithFields() {
    using namespace adqt::widgets;
    struct Rejection {
        QString path;
        QString code;
        QString field;
    };
    const Rejection rejections[] = {
        {QStringLiteral("/api/v2/auth/login"), QStringLiteral("invalid_credentials"),
         QStringLiteral("password")},
        {QStringLiteral("/api/v2/registration/requests"), QStringLiteral("username_unavailable"),
         QStringLiteral("username")},
        {QStringLiteral("/api/v2/registration/requests"), QStringLiteral("invalid_captcha"),
         QStringLiteral("captcha_answer")},
        {QStringLiteral("/api/v2/registration/requests"), QStringLiteral("password_mismatch"),
         QStringLiteral("password_confirmation")}};
    for (const auto& rejection : rejections) {
        Api api;
        api.failurePath = rejection.path;
        api.failureCode = rejection.code;
        SnowShotApiClient client(api.origin());
        AccountSessionService service(client, options());
        AccountPageWidget page(&service);
        page.resize(400, 640);
        page.show();
        auto* form = page.findChild<AdForm*>(QStringLiteral("accountForm"));
        if (rejection.path.contains(QStringLiteral("registration"))) {
            page.findChild<AdButton*>(QStringLiteral("accountSwitchForm"))->click();
            await([&] { return !service.captcha().isEmpty(); });
            page.findChild<AdCheckbox*>(QStringLiteral("accountAgreement"))->setChecked(true);
        }
        page.findChild<QLineEdit*>(QStringLiteral("accountUsername"))
            ->setText(QStringLiteral("TestUser"));
        page.findChild<QLineEdit*>(QStringLiteral("accountPassword"))
            ->setText(QStringLiteral("long-password"));
        page.findChild<QLineEdit*>(QStringLiteral("accountPasswordConfirmation"))
            ->setText(QStringLiteral("long-password"));
        page.findChild<QLineEdit*>(QStringLiteral("accountCaptchaAnswer"))
            ->setText(QStringLiteral("abcd"));
        page.findChild<AdButton*>(QStringLiteral("accountSubmit"))->click();
        auto* item = form->field(rejection.field);
        await([&] { return !item->helpText().isEmpty(); });
        require(item->validateStatus() == AdFormItem::ValidateStatus::Error &&
                    service.error().isEmpty() && AdMessageService::instance(&page)->count() == 0,
                "server validation is attached to its field without a message");
        qobject_cast<QLineEdit*>(item->controlWidget())->clear();
        require(item->helpText().isEmpty(), "editing removes a stale server field error");
    }
}

void accountNetworkFailuresUseMessages() {
    using namespace adqt::widgets;
    for (const bool abortRequest : {false, true}) {
        Api api;
        api.failurePath = QStringLiteral("/api/v2/auth/login");
        api.failureStatus = 503;
        api.abortRequest = abortRequest;
        SnowShotApiClient client(api.origin());
        AccountSessionService service(client, options());
        AccountPageWidget page(&service);
        page.resize(640, 640);
        page.show();
        auto* messages = AdMessageService::instance(&page);
        QPointer<AdMessageHandle> errorMessage;
        int opened = 0;
        QObject::connect(messages, &AdMessage::messageOpened, &page, [&](AdMessageHandle* handle) {
            ++opened;
            errorMessage = handle;
        });
        page.findChild<QLineEdit*>(QStringLiteral("accountUsername"))
            ->setText(QStringLiteral("TestUser"));
        page.findChild<QLineEdit*>(QStringLiteral("accountPassword"))
            ->setText(QStringLiteral("long-password"));
        auto* submit = page.findChild<AdButton*>(QStringLiteral("accountSubmit"));
        submit->click();
        await([&] { return !service.busy() && errorMessage; });
        require(errorMessage->type() == AdMessage::Type::Error &&
                    errorMessage->content() == service.error() && opened == 1,
                "HTTP and transport failures open an error message");
        for (auto* item : page.findChild<AdForm*>()->items())
            require(item->helpText().isEmpty(), "network errors never mark valid fields");
        require(!page.findChild<QWidget*>(QStringLiteral("accountError")),
                "request errors have no page-level alert");
        QTranslator translator;
        require(translator.load(QStringLiteral(":/i18n/snow_shot_zh_CN.qm")),
                "load message translations");
        QApplication::installTranslator(&translator);
        QApplication::processEvents();
        require(errorMessage->content() == service.error() &&
                    errorMessage->content().contains(QStringLiteral("连接")) && opened == 1,
                "open network messages retranslate without duplicating the notification");
        QApplication::removeTranslator(&translator);
        QApplication::processEvents();
        errorMessage->close();
        service.loadCaptcha();
        await([&] { return !service.captcha().isEmpty(); });
        require(opened == 1, "later state changes do not reopen a dismissed error message");
        submit->click();
        await([&] { return !service.busy() && opened == 2; });
        require(errorMessage && errorMessage->content() == service.error(),
                "a repeated network failure produces a new notification");
    }
}

QString captchaImageData(const QSize& size) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.fillRect(0, 0, size.width(), 6, QColor(255, 0, 255));
    painter.fillRect(0, size.height() - 6, size.width(), 6, QColor(0, 255, 255));
    painter.fillRect(0, 6, 6, size.height() - 12, QColor(255, 0, 0));
    painter.fillRect(size.width() - 6, 6, 6, size.height() - 12, QColor(0, 180, 0));
    QFont font;
    font.setPixelSize(28);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::black);
    painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("A7K9"));
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

void captchaImagesFitAllocatedRectangles() {
    Api api;
    api.captchaData = captchaImageData(QSize(228, 64));
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    page.resize(640, 640);
    page.show();
    QApplication::processEvents();
    auto* image = page.findChild<QLabel*>(QStringLiteral("accountCaptchaImage"));
    require(image != nullptr, "registration exposes an accessible CAPTCHA image");
    require(page.findChild<adqt::widgets::AdLineEdit*>(QStringLiteral("accountCaptchaAnswer"))
                    ->maxLength() == 4,
            "registration accepts a four-character CAPTCHA answer");
    bool fittedOnPublication = true;
    int publications = 0;
    QObject::connect(&service, &AccountSessionService::changed, &page, [&] {
        if (service.captcha().value(QStringLiteral("image_data_url")).toString() != api.captchaData)
            return;
        ++publications;
        const auto painted = image->pixmap().deviceIndependentSize();
        const auto viewport = image->contentsRect().size();
        const bool fits =
            painted.width() <= viewport.width() && painted.height() <= viewport.height();
        if (!fits)
            std::cerr << "CAPTCHA overflow at publication: dpr=" << image->devicePixelRatioF()
                      << " pixmap=" << painted.width() << 'x' << painted.height()
                      << " contents=" << viewport.width() << 'x' << viewport.height() << '\n';
        fittedOnPublication = fittedOnPublication && fits;
    });
    page.findChild<adqt::widgets::AdButton*>(QStringLiteral("accountSwitchForm"))->click();
    await([&] { return publications > 0; });
    for (const int width : {640, 292, 260}) {
        page.resize(width, 640);
        QApplication::processEvents();
        for (const QSize source : {QSize(228, 64), QSize(152, 64), QSize(228, 120)}) {
            api.captchaData = captchaImageData(source);
            publications = 0;
            service.loadCaptcha();
            await([&] { return publications > 0; });
            QApplication::processEvents();
            const auto pixmap = image->pixmap().deviceIndependentSize();
            require(pixmap.width() <= image->contentsRect().width() &&
                        pixmap.height() <= image->contentsRect().height(),
                    "the complete CAPTCHA fits its settled image viewport");
            require(image->parentWidget()->rect().contains(image->geometry()),
                    "the CAPTCHA image fits its parent layout row");
            const QImage rendered = image->grab().toImage().convertToFormat(QImage::Format_RGB32);
            for (const QColor color :
                 {QColor(255, 0, 255), QColor(0, 255, 255), QColor(255, 0, 0), QColor(0, 180, 0)}) {
                bool found = false;
                for (int y = 0; y < rendered.height() && !found; ++y)
                    for (int x = 0; x < rendered.width() && !found; ++x)
                        found = rendered.pixelColor(x, y) == color;
                require(found, "all four CAPTCHA image edges remain visible after refresh");
            }
        }
    }
    require(fittedOnPublication,
            "a newly loaded or refreshed CAPTCHA must fit before layout catches up");
}

void captchaImageReview(const QString& input, const QString& output) {
    QFile imageFile(input);
    require(imageFile.open(QIODevice::ReadOnly), "open the real CAPTCHA review image");
    Api api;
    api.captchaData = QStringLiteral("data:image/png;base64,") +
                      QString::fromLatin1(imageFile.readAll().toBase64());
    require(QDir().mkpath(output), "create CAPTCHA screenshot directory");
    QTranslator translator;
    require(translator.load(QStringLiteral(":/i18n/snow_shot_zh_CN.qm")),
            "load CAPTCHA review translations");
    QApplication::installTranslator(&translator);
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, options());
    AccountPageWidget page(&service);
    page.resize(640, 640);
    page.show();
    page.findChild<adqt::widgets::AdButton*>(QStringLiteral("accountSwitchForm"))->click();
    await([&] { return !service.captcha().isEmpty(); });
    auto* image = page.findChild<QLabel*>(QStringLiteral("accountCaptchaImage"));
    auto* scroll = page.findChild<PageContainerWidget*>()->scrollArea();
    for (const int width : {640, 292, 260}) {
        page.resize(width, 640);
        for (int index = 0; index < 5; ++index) {
            QApplication::sendPostedEvents();
            QApplication::processEvents();
        }
        scroll->ensureWidgetVisible(image, 0, 12);
        QApplication::processEvents();
        const auto pixmap = image->pixmap().deviceIndependentSize();
        require(pixmap.width() <= image->contentsRect().width() &&
                    pixmap.height() <= image->contentsRect().height(),
                "a real CAPTCHA remains complete inside its review viewport");
        const auto destination = [&](const QString& name) {
            return QDir(output).filePath(name + QStringLiteral("-%1.png").arg(width));
        };
        require(page.grab().save(destination(QStringLiteral("registration"))),
                "save real CAPTCHA page preview");
        require(image->parentWidget()->grab().save(destination(QStringLiteral("row"))),
                "save real CAPTCHA row preview");
        require(image->grab().save(destination(QStringLiteral("image"))),
                "save real CAPTCHA image preview");
    }
    QApplication::removeTranslator(&translator);
}

void captchaImageDprProcesses() {
    for (const auto* scale : {"1", "1.5", "2"}) {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_SCALE_FACTOR"), QString::fromLatin1(scale));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("-platform"), QStringLiteral("offscreen"),
                     QStringLiteral("--captcha-images-only")});
        require(child.waitForFinished(15000), "CAPTCHA DPR regression child completes");
        if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0)
            std::cerr << child.readAllStandardError().constData();
        require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                "CAPTCHA remains complete at 1.0, 1.5, and 2.0 device pixel ratios");
    }
}

void screenshots(const QString& directory) {
    namespace styles = snow_shot::presentation::styles;
    require(QDir().mkpath(directory), "create account UI preview directory");
    Api api;
    auto config = options();
    config.pollIntervalMs = 60000;
    SnowShotApiClient client(api.origin());
    AccountSessionService service(client, config);
    AccountPageWidget page(&service);
    page.resize(900, 640);
    page.show();
    const auto capture = [&](const QString& name) {
        for (int index = 0; index < 5; ++index) {
            QApplication::sendPostedEvents();
            QApplication::processEvents();
        }
        require(page.grab().save(QDir(directory).filePath(name + QStringLiteral(".png"))),
                "save account UI preview");
    };
    const auto narrow = [&](const QString& state) {
        auto* scroll = page.findChild<PageContainerWidget*>()->scrollArea()->verticalScrollBar();
        for (const int width : {292, 260}) {
            page.resize(width, 316);
            scroll->setValue(0);
            capture(state + QStringLiteral("-%1-top").arg(width));
            scroll->setValue(scroll->maximum());
            capture(state + QStringLiteral("-%1-bottom").arg(width));
        }
        for (const auto* locale : {"zh_CN", "zh_TW"}) {
            QTranslator translator;
            require(translator.load(
                        QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                    "load localized account preview catalog");
            QApplication::installTranslator(&translator);
            page.resize(260, 640);
            scroll->setValue(0);
            capture(state + u'-' + QString::fromLatin1(locale) + QStringLiteral("-260"));
            QApplication::removeTranslator(&translator);
        }
        page.resize(900, 640);
        scroll->setValue(0);
    };
    auto theme = styles::ThemeStyleConfig{};
    theme.appFont = QApplication::font();
    theme.appearance = styles::ThemeAppearance::Light;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    capture(QStringLiteral("sign-in-light"));
    page.findChild<adqt::widgets::AdButton*>(QStringLiteral("accountSubmit"))->click();
    capture(QStringLiteral("sign-in-validation-light"));
    service.clearError();
    theme.appearance = styles::ThemeAppearance::Dark;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    capture(QStringLiteral("sign-in-dark"));
    page.findChild<adqt::widgets::AdButton*>(QStringLiteral("accountSwitchForm"))->click();
    await([&] { return !service.captcha().isEmpty(); });
    capture(QStringLiteral("registration-dark"));
    theme.appearance = styles::ThemeAppearance::Light;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    capture(QStringLiteral("registration-light"));
    narrow(QStringLiteral("registration"));
    page.findChild<adqt::widgets::AdCheckbox*>(QStringLiteral("accountAgreement"))
        ->setChecked(true);
    page.findChild<adqt::widgets::AdButton*>(QStringLiteral("accountSubmit"))->click();
    capture(QStringLiteral("registration-validation-light"));
    narrow(QStringLiteral("registration-validation"));
    service.clearError();
    service.registerAccount(QStringLiteral("SnowShotUser"), QStringLiteral("long-password"),
                            QStringLiteral("long-password"), QStringLiteral("A7K9"));
    await([&] { return !service.purchase().request().isEmpty(); });
    capture(QStringLiteral("payment-light"));
    narrow(QStringLiteral("payment"));
    theme.appearance = styles::ThemeAppearance::Dark;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    capture(QStringLiteral("payment-dark"));
    api.state = QStringLiteral("fulfilled");
    service.refreshPurchase();
    await([&] { return service.signedIn() && !service.transactions().isEmpty(); });
    capture(QStringLiteral("profile-dark"));
    theme.appearance = styles::ThemeAppearance::Light;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    capture(QStringLiteral("profile-light"));
    api.state = QStringLiteral("awaiting_payment");
    service.beginTopUp();
    await([&] { return !service.purchase().request().isEmpty(); });
    page.resize(900, 960);
    capture(QStringLiteral("top-up-light"));
    narrow(QStringLiteral("top-up"));
    theme.appearance = styles::ThemeAppearance::Dark;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    page.resize(900, 960);
    capture(QStringLiteral("top-up-dark"));
    service.cancelPurchase();
    await([&] { return service.purchase().request().isEmpty(); });
    capture(QStringLiteral("top-up-canceled-dark"));
    theme.appearance = styles::ThemeAppearance::Light;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    page.resize(900, 800);
    capture(QStringLiteral("profile-overview-light"));
    QTranslator overviewTranslator;
    require(overviewTranslator.load(QStringLiteral(":/i18n/snow_shot_zh_CN.qm")),
            "load Chinese account overview preview");
    QApplication::installTranslator(&overviewTranslator);
    capture(QStringLiteral("profile-overview-zh_CN"));
    QApplication::removeTranslator(&overviewTranslator);
    theme.appearance = styles::ThemeAppearance::Dark;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    capture(QStringLiteral("profile-overview-dark"));
    theme.appearance = styles::ThemeAppearance::Light;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    api.emptyTransactions = true;
    service.loadTransactions();
    await([&] { return service.transactions().isEmpty(); });
    capture(QStringLiteral("profile-empty"));
    theme.appearance = styles::ThemeAppearance::Dark;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    capture(QStringLiteral("profile-empty-dark"));
    theme.appearance = styles::ThemeAppearance::Light;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    QApplication::installTranslator(&overviewTranslator);
    capture(QStringLiteral("profile-empty-zh_CN"));
    page.resize(260, 800);
    capture(QStringLiteral("profile-empty-zh_CN-narrow"));
    QApplication::removeTranslator(&overviewTranslator);
    api.emptyTransactions = false;
    service.loadTransactions();
    await([&] { return service.transactions().size() == 3; });
    page.resize(900, 640);
    api.balance = QStringLiteral("0");
    api.debt = QStringLiteral("1200000000");
    service.refreshAccount();
    await([&] {
        return service.account()
                   .value(QStringLiteral("outstanding_balance_nano_yuan"))
                   .toString() == api.debt;
    });
    capture(QStringLiteral("profile-debt-light"));
    narrow(QStringLiteral("profile-debt"));
    theme.appearance = styles::ThemeAppearance::Dark;
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    capture(QStringLiteral("profile-debt-dark"));
    page.resize(512, 316);
    capture(QStringLiteral("profile-debt-small"));
    theme.fontSize = 21;
    theme.appFont.setPixelSize(21);
    styles::ThemeManager::instance().setThemeStyleConfig(theme);
    page.resize(900, 640);
    capture(QStringLiteral("profile-debt-large-type"));
    page.resize(260, 640);
    capture(QStringLiteral("profile-debt-large-type-narrow"));
}

void secureStore() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    QTemporaryDir temporary;
    snow_shot::storage::AccountSecretStore store(
        QStringLiteral("account-unit-test-") + temporary.path(), temporary.path());
    const QByteArray secret("refresh-token-and-private-proof");
    require(store.write(secret), "native secret store saves account data");
    require(store.read() == secret, "native secret store decrypts its own account data");
    require(store.remove() && store.read().isEmpty(), "native secret store removes account data");
#endif
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("snow-shot-account-tests"));
    QTemporaryDir storageDirectory;
    snow_shot::storage::StorageInitializationOptions storageOptions;
    storageOptions.executableDirectory = storageDirectory.path() + QStringLiteral("/app");
    storageOptions.appDataDirectory = storageDirectory.path() + QStringLiteral("/data");
    require(QDir().mkpath(storageOptions.executableDirectory),
            "create isolated test app directory");
    require(snow_shot::storage::ApplicationStorage::instance().initialize(storageOptions).success,
            "initialize isolated account test storage");
#ifdef Q_OS_WIN
    const int font =
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (font >= 0)
        application.setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(), 10));
#endif
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
    const auto arguments = application.arguments();
    const int captchaReview = arguments.indexOf(QStringLiteral("--captcha-review"));
    if (captchaReview >= 0) {
        require(arguments.size() > captchaReview + 2,
                "CAPTCHA review needs input and output paths");
        captchaImageReview(arguments.at(captchaReview + 1), arguments.at(captchaReview + 2));
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--captcha-images-only"))) {
        captchaImagesFitAllocatedRectangles();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--screenshots"))) {
        screenshots(application.arguments().last());
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (arguments.contains(QStringLiteral("--top-up-cancellation-only"))) {
        topUpCanBeCancelledAndRestarted();
        topUpCancellationPreservesUnconfirmedOrCompletedPayments();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (arguments.contains(QStringLiteral("--payment-ui-only"))) {
        automaticPaymentPollingDoesNotShowButtonLoading();
        paymentActionsStayInOneRowAndDetailsAreCentered();
        uncertainPollingBacksOffAndManualRefreshRecovers();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (arguments.contains(QStringLiteral("--account-layout-only"))) {
        accountTransactionsRemainReadableWhenResized();
        emptyAccountTransactionsMatchScreenshotHistory();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    registrationRequiresExplicitAgreement();
    registrationDraftSurvivesExpiredPurchase();
    reopenedRegistrationPurchaseReturnsToRegistration();
    accountFormsAdvanceOneEditorPerTab();
    captchaImageDprProcesses();
    accountNavigationIsStandaloneAndFollowsGeneral();
    registrationEnforcesPasswordLength();
    passwordInputsEnforceMaximumLength();
    registrationReconcilesBeyondExpiryAndClaims();
    temporaryAccountLinkClaimsWithoutPayment();
    explicitIdentityChoiceSuppressesAutomaticClaim();
    automaticPaymentPollingDoesNotShowButtonLoading();
    paymentActionsStayInOneRowAndDetailsAreCentered();
    uncertainPollingBacksOffAndManualRefreshRecovers();
    registrationProofSurvivesRestartAndDoesNotOverrideStoredSession();
    tokenRefreshPreservesQueuedOfficialCalls();
    accountRequestsFollowSystemProxyPreference();
    accountUiAndCredits();
    accountTransactionsRemainReadableWhenResized();
    emptyAccountTransactionsMatchScreenshotHistory();
    languageChangeRetranslatesCachedErrors();
    accountFormValidationStaysWithFields();
    accountServerValidationStaysWithFields();
    accountNetworkFailuresUseMessages();
    secureStore();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
