#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
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
            sourceFailure(QT_TRANSLATE_NOOP("UpdateService",
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
        emit q.operationFinished(QStringLiteral("check"), QStringLiteral("failed"));
    }

    void sourceFailure(const char* source) {
        stopReply();
        if (!github) {
            github = true;
            page = 1;
            bestRelease = {};
            request();
        } else {
            finishFailure(source);
        }
    }

    qint64 byteLimit() const {
        return github ? 8 * 1024 * 1024 : kMaximumVersionBytes;
    }

    void read() {
        if (!reply)
            return;
        bytes += reply->read(byteLimit() + 1 - bytes.size());
        if (bytes.size() > byteLimit())
            sourceFailure(QT_TRANSLATE_NOOP("UpdateService",
                                            "The update server returned an invalid version."));
    }

    bool validUrl(const QUrl& url) const {
        const bool local =
            options.allowLocalHttp && url.scheme() == u"http" &&
            (url.host() == u"127.0.0.1" || url.host() == u"localhost" || url.host() == u"::1");
        return url.isValid() && !url.host().isEmpty() && (url.scheme() == u"https" || local) &&
               url.userInfo().isEmpty();
    }

    void complete(const QString& text, const QUrl& downloadUrl = {}) {
        const auto version = parseVersion(text);
        if (!version) {
            sourceFailure(QT_TRANSLATE_NOOP("UpdateService",
                                            "The update server returned an invalid version."));
            return;
        }
        const bool available =
            compareVersions(*version, *parseVersion(options.installedVersion)) > 0;
        stopReply();
        status = {
            available ? UpdateState::Available : UpdateState::Idle, text, {}, 0, 0, downloadUrl};
        const bool notify = available && !manual && mode == u"check" && !notified.contains(text);
        if (notify)
            notified.insert(text);
        arm(options.automaticCheckInterval);
        emit q.statusChanged();
        emit q.operationFinished(QStringLiteral("check"), QStringLiteral("success"));
        if (notify)
            emit q.automaticUpdateAvailable(text);
    }

    void githubResponse() {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isArray() ||
            document.array().size() > 100) {
            sourceFailure(QT_TRANSLATE_NOOP("UpdateService",
                                            "The update server returned an invalid version."));
            return;
        }
        for (const auto& value : document.array()) {
            const auto release = value.toObject();
            if (!release.value(QStringLiteral("draft")).isBool() ||
                release.value(QStringLiteral("draft")).toBool() ||
                !release.value(QStringLiteral("prerelease")).isBool() ||
                release.value(QStringLiteral("prerelease")).toBool())
                continue;
            const QString tag = release.value(QStringLiteral("tag_name")).toString();
            if (!tag.startsWith(u'v') || !tag.endsWith(u"_snow-shot"))
                continue;
            const QString text = tag.mid(1, tag.size() - 11);
            const auto version = parseVersion(text);
            if (!version || !version->prerelease.isEmpty())
                continue;
            if (bestVersion.isEmpty() ||
                compareVersions(*version, *parseVersion(bestVersion)) > 0) {
                bestVersion = text;
                bestRelease = release;
            }
        }
        if (document.array().size() == 100) {
            if (page == 10) {
                sourceFailure(QT_TRANSLATE_NOOP("UpdateService",
                                                "The update server returned an invalid version."));
                return;
            }
            ++page;
            request();
            return;
        }
#if defined(Q_PROCESSOR_ARM_64)
        const QString arch = QStringLiteral("arm64");
#else
        const QString arch = QStringLiteral("x64");
#endif
        const QString name = QStringLiteral("snow-shot-%1-macos-%2.dmg").arg(bestVersion, arch);
        const QString root = QStringLiteral("https://github.com/mg-chao/snow-apps/releases/");
        const QString tag = bestRelease.value(QStringLiteral("tag_name")).toString();
        for (const QString& assetName : {name, name + QStringLiteral(".sha256")}) {
            int matches = 0;
            for (const auto& value : bestRelease.value(QStringLiteral("assets")).toArray()) {
                const auto asset = value.toObject();
                if (asset.value(QStringLiteral("name")).toString() == assetName) {
                    ++matches;
                    if (QUrl(asset.value(QStringLiteral("browser_download_url")).toString()) !=
                        QUrl(root + QStringLiteral("download/") + tag + u'/' + assetName)) {
                        sourceFailure(QT_TRANSLATE_NOOP(
                            "UpdateService", "The update server returned an invalid version."));
                        return;
                    }
                }
            }
            if (matches != 1) {
                sourceFailure(QT_TRANSLATE_NOOP("UpdateService",
                                                "The update server returned an invalid version."));
                return;
            }
        }
        complete(bestVersion, QUrl(root + QStringLiteral("tag/") + tag));
    }

    void request() {
        stopReply();
        QUrl url = github ? options.githubApiUrl
                          : options.baseUrl.resolved(QUrl(QStringLiteral("/latest-version.txt")));
        if (!validUrl(url)) {
            finishFailure(QT_TRANSLATE_NOOP("UpdateService",
                                            "Could not check for updates. Please try again."));
            return;
        }
        if (github) {
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("per_page"), QStringLiteral("100"));
            query.addQueryItem(QStringLiteral("page"), QString::number(page));
            url.setQuery(query);
        }
        bytes.clear();
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::SameOriginRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        request.setRawHeader("Accept", github ? "application/vnd.github+json" : "text/plain");
        request.setRawHeader("User-Agent", "SnowShot/" + options.installedVersion.toUtf8());
        request.setRawHeader("Cache-Control", "no-cache");
        reply = network.get(request);
        reply->setReadBufferSize(byteLimit() + 1);
        const QPointer<QNetworkReply> current = reply;
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &q, [this, current] {
            if (reply == current && reply &&
                reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > byteLimit())
                sourceFailure(QT_TRANSLATE_NOOP("UpdateService",
                                                "The update server returned an invalid version."));
        });
        QObject::connect(reply, &QNetworkReply::readyRead, &q, [this, current] {
            if (reply == current)
                read();
        });
        QObject::connect(reply, &QNetworkReply::finished, &q, [this, current] {
            if (reply != current)
                return;
            read();
            if (!reply || reply != current)
                return;
            if (reply->error() != QNetworkReply::NoError ||
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
                sourceFailure(QT_TRANSLATE_NOOP("UpdateService",
                                                "Could not check for updates. Please try again."));
                return;
            }
            if (github)
                githubResponse();
            else
                complete(QString::fromUtf8(bytes).trimmed());
        });
        deadline.start(options.requestTimeout);
    }

    void check(bool user) {
        if (!user && mode == u"manual")
            return;
        if (reply) {
            manual = manual || user;
            return;
        }
        schedule.stop();
        manual = user;
        previous = status;
        previousErrorSource = errorSource;
        errorSource.clear();
        if (!parseVersion(options.installedVersion) || !validUrl(options.baseUrl)) {
            finishFailure(QT_TRANSLATE_NOOP("UpdateService",
                                            "Could not check for updates. Please try again."));
            return;
        }
        github = false;
        bestVersion.clear();
        status = {UpdateState::Checking, {}, {}, 0, 0};
        request();
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
    bool github = false;
    int page = 1;
    QString bestVersion;
    QJsonObject bestRelease;
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
bool UpdateService::busy() const {
    return !m_impl->reply.isNull();
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
    emit operationFinished(QStringLiteral("check"), QStringLiteral("cancelled"));
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
