#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QEvent>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <optional>
#include <algorithm>

namespace snow_shot::update {
namespace {
constexpr qint64 kMaximumVersionBytes = 4096;

struct Version {
    QStringList core;
    QStringList prerelease;
};

std::optional<Version> parseVersion(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral("\\A(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
                       "(?:-([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?"
                       "(?:\\+([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?\\z"));
    const auto match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    Version version{{match.captured(1), match.captured(2), match.captured(3)}, {}};
    if (!match.captured(4).isEmpty()) {
        version.prerelease = match.captured(4).split(u'.');
        for (const auto& identifier : version.prerelease) {
            if (identifier.size() > 1 && identifier.front() == u'0' &&
                std::all_of(identifier.begin(), identifier.end(),
                            [](QChar c) { return c >= u'0' && c <= u'9'; }))
                return std::nullopt;
        }
    }
    return version;
}

bool numeric(const QString& text) {
    return std::all_of(text.begin(), text.end(), [](QChar c) { return c >= u'0' && c <= u'9'; });
}

int compareIdentifier(const QString& left, const QString& right, bool number) {
    if (number && left.size() != right.size())
        return left.size() < right.size() ? -1 : 1;
    return QString::compare(left, right, Qt::CaseSensitive);
}

int compareVersions(const Version& left, const Version& right) {
    for (qsizetype i = 0; i < 3; ++i) {
        const int order = compareIdentifier(left.core[i], right.core[i], true);
        if (order != 0)
            return order;
    }
    if (left.prerelease.isEmpty() != right.prerelease.isEmpty())
        return left.prerelease.isEmpty() ? 1 : -1;
    for (qsizetype i = 0; i < std::min(left.prerelease.size(), right.prerelease.size()); ++i) {
        const bool leftNumeric = numeric(left.prerelease[i]);
        const bool rightNumeric = numeric(right.prerelease[i]);
        if (leftNumeric != rightNumeric)
            return leftNumeric ? -1 : 1;
        const int order = compareIdentifier(left.prerelease[i], right.prerelease[i], leftNumeric);
        if (order != 0)
            return order;
    }
    return left.prerelease.size() == right.prerelease.size()
               ? 0
               : (left.prerelease.size() < right.prerelease.size() ? -1 : 1);
}

class SystemProxyFactory final : public QNetworkProxyFactory {
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return systemProxyForQuery(query);
    }
};
} // namespace

struct UpdateService::Impl {
    Impl(UpdateService& owner, Options value)
        : q(owner), options(std::move(value)), network(&owner), schedule(&owner), deadline(&owner) {
        status.state = UpdateState::Idle;
        if (options.installedVersion.isEmpty())
            options.installedVersion = QStringLiteral(SNOW_SHOT_VERSION);
        schedule.setSingleShot(true);
        deadline.setSingleShot(true);
        QObject::connect(&schedule, &QTimer::timeout, &q, [this] { check(false); });
        QObject::connect(&deadline, &QTimer::timeout, &q, [this] {
            finishFailure(QT_TRANSLATE_NOOP("UpdateService",
                                            "The update check timed out. Please try again."));
        });
        network.setProxy(QNetworkProxy::NoProxy);
    }

    void arm(std::chrono::milliseconds delay) {
        if (started && mode == u"check")
            schedule.start(delay);
    }

    void stopReply() {
        deadline.stop();
        if (reply) {
            auto* finished = reply.data();
            reply.clear();
            finished->disconnect(&q);
            finished->abort();
            finished->deleteLater();
        }
    }

    void finishFailure(const char* source) {
        stopReply();
        status = previous;
        errorSource = previousErrorSource;
        if (manual) {
            errorSource = source;
            status.state = UpdateState::Failed;
            status.error = QCoreApplication::translate("UpdateService", source);
        }
        arm(options.automaticCheckInterval);
        emit q.statusChanged();
    }

    void read() {
        if (!reply)
            return;
        bytes += reply->read(kMaximumVersionBytes + 1 - bytes.size());
        if (bytes.size() > kMaximumVersionBytes)
            finishFailure(QT_TRANSLATE_NOOP("UpdateService",
                                            "The update server returned an invalid version."));
    }

    void check(bool user) {
        if (!user && mode == u"manual")
            return;
        if (reply) {
            // A user check joins the current request and gets visible error feedback.
            manual = manual || user;
            return;
        }
        schedule.stop();
        manual = user;
        previous = status;
        previousErrorSource = errorSource;
        errorSource.clear();
        const auto url = options.baseUrl.resolved(QUrl(QStringLiteral("/latest-version.txt")));
        const bool local =
            options.allowLocalHttp && url.scheme() == u"http" &&
            (url.host() == u"127.0.0.1" || url.host() == u"localhost" || url.host() == u"::1");
        if (!url.isValid() || url.host().isEmpty() || (url.scheme() != u"https" && !local) ||
            !url.userInfo().isEmpty()) {
            finishFailure(QT_TRANSLATE_NOOP("UpdateService",
                                            "Could not check for updates. Please try again."));
            return;
        }
        if (!parseVersion(options.installedVersion)) {
            finishFailure(QT_TRANSLATE_NOOP("UpdateService",
                                            "The update server returned an invalid version."));
            return;
        }
        status = {UpdateState::Checking, {}, {}, 0, 0};
        bytes.clear();
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::SameOriginRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        request.setRawHeader("Accept", "text/plain");
        request.setRawHeader("Cache-Control", "no-cache");
        reply = network.get(request);
        reply->setReadBufferSize(kMaximumVersionBytes + 1);
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &q, [this] {
            if (reply && reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() >
                             kMaximumVersionBytes)
                finishFailure(QT_TRANSLATE_NOOP("UpdateService",
                                                "The update server returned an invalid version."));
        });
        QObject::connect(reply, &QNetworkReply::readyRead, &q, [this] { read(); });
        QObject::connect(reply, &QNetworkReply::finished, &q, [this] {
            read();
            if (!reply)
                return;
            if (reply->error() != QNetworkReply::NoError ||
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
                finishFailure(QT_TRANSLATE_NOOP("UpdateService",
                                                "Could not check for updates. Please try again."));
                return;
            }
            const QString text = QString::fromUtf8(bytes).trimmed();
            const auto version = parseVersion(text);
            if (!version) {
                finishFailure(QT_TRANSLATE_NOOP("UpdateService",
                                                "The update server returned an invalid version."));
                return;
            }
            const bool available =
                compareVersions(*version, *parseVersion(options.installedVersion)) > 0;
            stopReply();
            status = {available ? UpdateState::Available : UpdateState::Idle, text, {}, 0, 0};
            const bool notify =
                available && !manual && mode == u"check" && !notified.contains(text);
            if (notify)
                notified.insert(text);
            arm(options.automaticCheckInterval);
            emit q.statusChanged();
            if (notify)
                emit q.automaticUpdateAvailable(text);
        });
        deadline.start(options.requestTimeout);
        emit q.statusChanged();
    }

    UpdateService& q;
    Options options;
    QNetworkAccessManager network;
    QTimer schedule;
    QTimer deadline;
    QPointer<QNetworkReply> reply;
    QByteArray bytes;
    QByteArray errorSource;
    QByteArray previousErrorSource;
    UpdateStatus status;
    UpdateStatus previous;
    QSet<QString> notified;
    QString mode = QStringLiteral("check");
    bool started = false;
    bool manual = false;
};

UpdateService::UpdateService(Options options, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(options))) {
    setObjectName(QStringLiteral("snowShotUpdateService"));
}
UpdateService::~UpdateService() {
    m_impl->stopReply();
}
const UpdateStatus& UpdateService::status() const {
    return m_impl->status;
}
void UpdateService::start() {
    if (m_impl->started)
        return;
    m_impl->started = true;
    m_impl->arm(m_impl->options.startupCheckDelay);
}
void UpdateService::setMode(const QString& value) {
    const QString mode = value == u"download" ? QStringLiteral("check") : value;
    if ((mode != u"manual" && mode != u"check") || mode == m_impl->mode)
        return;
    m_impl->mode = mode;
    if (mode == u"manual") {
        m_impl->schedule.stop();
        if (m_impl->reply && !m_impl->manual)
            cancel();
    } else {
        m_impl->arm(m_impl->options.startupCheckDelay);
    }
}
void UpdateService::setSystemProxy(bool enabled) {
    if (enabled)
        m_impl->network.setProxyFactory(new SystemProxyFactory);
    else
        m_impl->network.setProxy(QNetworkProxy::NoProxy);
}
void UpdateService::check(bool manual) {
    m_impl->check(manual);
}
void UpdateService::cancel() {
    if (!m_impl->reply)
        return;
    m_impl->stopReply();
    m_impl->status = m_impl->previous;
    m_impl->errorSource = m_impl->previousErrorSource;
    m_impl->arm(m_impl->options.automaticCheckInterval);
    emit statusChanged();
}
// These Windows installation operations intentionally have no macOS implementation.
void UpdateService::download() {}
void UpdateService::requestRestart() {}
void UpdateService::beginApply() {}
void UpdateService::reportBlocked(const QString&) {}
bool UpdateService::event(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && !m_impl->errorSource.isEmpty()) {
        m_impl->status.error =
            QCoreApplication::translate("UpdateService", m_impl->errorSource.constData());
        emit statusChanged();
    }
    return QObject::event(event);
}
} // namespace snow_shot::update
