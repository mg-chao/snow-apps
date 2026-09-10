#include "snow_shot/update/updateservice.h"
#include "snow_shot/update/updatetransaction.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QUuid>
#include <memory>

namespace snow_shot::update {
UpdateService::UpdateService(Options options, QObject* parent)
    : QObject(parent), m_options(std::move(options)) {
    setObjectName(QStringLiteral("snowShotUpdateService"));
    try {
        const bool localHttp =
            m_options.allowLocalHttp && m_options.baseUrl.scheme() == u"http" &&
            (m_options.baseUrl.host() == u"127.0.0.1" || m_options.baseUrl.host() == u"localhost");
        requireUpdate(m_options.baseUrl.isValid() && !m_options.baseUrl.host().isEmpty() &&
                          m_options.baseUrl.userInfo().isEmpty() &&
                          (m_options.baseUrl.scheme() == u"https" || localHttp),
                      "The update server must use HTTPS");
        const auto record = installationRecord(m_options.root);
        m_variant = record.value(QStringLiteral("variant")).toString();
        m_installedVersion = record.value(QStringLiteral("version")).toString();
        requireUpdate(QDir().mkpath(m_options.cacheDirectory), "Could not create update cache");
        if (QFileInfo::exists(cachePath(QStringLiteral("state.json")))) {
            try {
                m_persisted =
                    QJsonDocument::fromJson(readLimited(cachePath(QStringLiteral("state.json"))))
                        .object();
                const QString observed =
                    m_persisted.value(QStringLiteral("observedVersion")).toString();
                if (!observed.isEmpty()) {
                    compareVersions(observed, observed);
                }
            } catch (...) {
                m_persisted = {};
            }
        }
        setState(UpdateState::Idle);
        if (QFileInfo::exists(cachePath(QStringLiteral("release.json")))) {
            try {
                auto release = verifyRelease(readLimited(cachePath(QStringLiteral("release.json"))),
                                             m_options.trustedKeys);
                const QString observed =
                    m_persisted.value(QStringLiteral("observedVersion")).toString();
                if (!observed.isEmpty()) {
                    requireUpdate(compareVersions(release.version, observed) >= 0,
                                  "The server offered older release metadata");
                    if (compareVersions(release.version, observed) == 0) {
                        requireUpdate(
                            release.updatePackage(m_variant).sha256 ==
                                m_persisted.value(QStringLiteral("observedHash")).toString(),
                            "The server changed an already published release");
                    }
                }
                if (compareVersions(release.version, m_installedVersion) > 0) {
                    m_status.version = release.version;
                    const auto& package = release.updatePackage(m_variant);
                    const QString complete = cachePath(package.sha256 + QStringLiteral(".zip"));
                    const QString failed =
                        QDir(m_options.root)
                            .filePath(QStringLiteral(".snow-shot-update/failed-version.txt"));
                    const bool suppressed =
                        QFileInfo::exists(failed) &&
                        QString::fromUtf8(readLimited(failed, 256)).trimmed() == release.version;
                    setState(UpdateState::Available);
                    if (!suppressed && QFileInfo::exists(complete)) {
                        verifyFile(complete, package.size, package.sha256);
                        setState(UpdateState::Ready);
                    }
                    m_release = std::move(release);
                }
            } catch (...) {
                setState(UpdateState::Idle);
            }
        }
        const QString result = cachePath(QStringLiteral("result.txt"));
        if (QFileInfo::exists(result)) {
            const QString text = QString::fromUtf8(readLimited(result, 8192));
            QFile::remove(result);
            if (text.startsWith(u"failed:")) {
                fail(text.mid(7));
            }
        }
    } catch (const std::exception& error) {
        setState(UpdateState::Unavailable, QString::fromUtf8(error.what()));
    }
    m_schedule.setInterval(24 * 60 * 60 * 1000);
    connect(&m_schedule, &QTimer::timeout, this, [this] { check(false); });
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_reply) {
            m_reply->abort();
        }
    });
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &UpdateService::fetchMetadata);
    m_handoffTimeout.setSingleShot(true);
    connect(&m_handoffTimeout, &QTimer::timeout, this, [this] {
        m_server.close();
        fail(tr("The update helper did not respond. Please retry."));
    });
}

UpdateService::~UpdateService() {
    cancel();
}

const UpdateStatus& UpdateService::status() const {
    return m_status;
}
QString UpdateService::cachePath(const QString& name) const {
    return QDir(m_options.cacheDirectory).filePath(name);
}
void UpdateService::setState(UpdateState state, const QString& error) {
    m_status.state = state;
    m_status.error = error;
    emit statusChanged();
}
void UpdateService::fail(const QString& message) {
    setState(UpdateState::Failed,
             QCoreApplication::translate("UpdateErrors", message.toUtf8().constData()));
}
void UpdateService::saveState() {
    writeAtomic(cachePath(QStringLiteral("state.json")),
                QJsonDocument(m_persisted).toJson(QJsonDocument::Compact));
}

void UpdateService::start() {
    if (m_status.state == UpdateState::Unavailable) {
        return;
    }
    m_schedule.start();
    if (m_status.state == UpdateState::Ready) {
        QTimer::singleShot(0, this, &UpdateService::updateReady);
    }
    QTimer::singleShot(30000, this, [this] {
        if (m_status.state == UpdateState::Available && m_mode == u"download") {
            m_autoDownload = true;
            m_manual = false;
            fetchMetadata();
        } else {
            check(false);
        }
    });
}

void UpdateService::setMode(const QString& mode) {
    if (mode == u"manual" || mode == u"check" || mode == u"download") {
        m_mode = mode;
        if (m_mode != u"download" && !m_manual) {
            m_autoDownload = false;
            if (m_status.state == UpdateState::Downloading ||
                (m_mode == u"manual" && (m_reply || m_retry.isActive()))) {
                cancel();
            }
        }
    }
}

void UpdateService::setSystemProxy(bool enabled) {
    m_network.setProxy(enabled ? QNetworkProxy(QNetworkProxy::DefaultProxy)
                               : QNetworkProxy(QNetworkProxy::NoProxy));
    if (enabled) {
        const auto proxies =
            QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(m_options.baseUrl));
        if (!proxies.isEmpty()) {
            m_network.setProxy(proxies.first());
        }
    }
}

void UpdateService::check(bool manual) {
    if (m_status.state == UpdateState::Unavailable || m_reply || m_retry.isActive() ||
        m_status.state == UpdateState::Applying) {
        return;
    }
    if (!manual) {
        const auto last = QDateTime::fromString(
            m_persisted.value(QStringLiteral("checkedAt")).toString(), Qt::ISODate);
        if (m_mode == u"manual" ||
            (last.isValid() && last <= m_options.now() && last.secsTo(m_options.now()) < 86400)) {
            return;
        }
    }
    m_manual = manual;
    m_autoDownload = m_mode == u"download";
    m_attempt = 0;
    fetchMetadata();
}

QNetworkReply* UpdateService::get(const QUrl& url) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SnowShot/") + m_installedVersion);
    request.setTransferTimeout(30000);
    return m_network.get(request);
}

void UpdateService::fetchMetadata() {
    setState(UpdateState::Checking);
    const quint64 generation = ++m_generation;
    auto* reply = get(m_options.baseUrl.resolved(QUrl(QStringLiteral("/latest-version.json"))));
    m_reply = reply;
    m_deadline.start(30000);
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QIODevice::readyRead, this, [reply, bytes] {
        bytes->append(reply->readAll());
        if (bytes->size() > 8 * 1024 * 1024) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, generation] {
        reply->deleteLater();
        if (generation != m_generation) {
            return;
        }
        m_reply = nullptr;
        m_deadline.stop();
        bytes->append(reply->readAll());
        try {
            requireUpdate(reply->error() == QNetworkReply::NoError &&
                              reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() ==
                                  200,
                          "Could not download signed update metadata");
            acceptMetadata(*bytes);
        } catch (const std::exception& error) {
            fail(QString::fromUtf8(error.what()));
        }
    });
}

void UpdateService::acceptMetadata(const QByteArray& bytes) {
    auto release = verifyRelease(bytes, m_options.trustedKeys);
    const QString observed = m_persisted.value(QStringLiteral("observedVersion")).toString();
    if (!observed.isEmpty()) {
        requireUpdate(compareVersions(release.version, observed) >= 0,
                      "The server offered older release metadata");
        if (compareVersions(release.version, observed) == 0) {
            const QString hash = release.updatePackage(m_variant).sha256;
            requireUpdate(hash == m_persisted.value(QStringLiteral("observedHash")).toString(),
                          "The server changed an already published release");
        }
    }
    m_persisted.insert(QStringLiteral("observedVersion"), release.version);
    m_persisted.insert(QStringLiteral("observedHash"), release.updatePackage(m_variant).sha256);
    m_persisted.insert(QStringLiteral("checkedAt"), m_options.now().toString(Qt::ISODate));
    saveState();
    m_status.version = release.version;
    m_release = std::move(release);
    if (compareVersions(m_release->version, m_installedVersion) <= 0) {
        setState(UpdateState::Idle);
        return;
    }
    writeAtomic(cachePath(QStringLiteral("release.json")), bytes);
    const auto& package = m_release->updatePackage(m_variant);
    // Retain only this release's payload. Never sweep arbitrary files in the cache.
    static const QRegularExpression payloadName(QStringLiteral("^[a-f0-9]{64}\\.(zip|part)$"));
    for (const auto& entry : QDir(m_options.cacheDirectory).entryInfoList(QDir::Files)) {
        if (payloadName.match(entry.fileName()).hasMatch() &&
            !entry.fileName().startsWith(package.sha256 + u'.') && !entry.isSymLink()) {
            QFile::remove(entry.absoluteFilePath());
        }
    }
    const QString failed =
        QDir(m_options.root).filePath(QStringLiteral(".snow-shot-update/failed-version.txt"));
    const bool suppressed =
        QFileInfo::exists(failed) &&
        QString::fromUtf8(readLimited(failed, 256)).trimmed() == m_release->version;
    if (suppressed && !m_manual) {
        setState(UpdateState::Available);
        return;
    }
    const QString complete = cachePath(package.sha256 + QStringLiteral(".zip"));
    if (QFileInfo::exists(complete)) {
        try {
            verifyFile(complete, package.size, package.sha256);
            setState(UpdateState::Ready);
            emit updateReady();
            return;
        } catch (...) {
            QFile::remove(complete);
        }
    }
    setState(UpdateState::Available);
    if (m_autoDownload && (m_manual || !suppressed)) {
        fetchPackage();
    }
}

void UpdateService::download() {
    if (!m_release || m_reply || m_status.state == UpdateState::Applying) {
        return;
    }
    m_manual = true;
    m_autoDownload = true;
    m_attempt = 0;
    fetchPackage();
}

void UpdateService::fetchPackage() {
    try {
        const auto package = m_release->updatePackage(m_variant);
        const QStorageInfo disk(m_options.cacheDirectory);
        requireUpdate(disk.bytesAvailable() > package.size + 64 * 1024 * 1024,
                      "Not enough free space to download the update");
        m_partial.setFileName(cachePath(package.sha256 + QStringLiteral(".part")));
        const QString savedHash = m_persisted.value(QStringLiteral("partialHash")).toString();
        QByteArray validator = m_persisted.value(QStringLiteral("validator")).toString().toLatin1();
        if (savedHash != package.sha256 || validator.isEmpty() || validator.startsWith("W/") ||
            m_partial.size() > package.size) {
            m_partial.remove();
        }
        requireUpdate(m_partial.open(QIODevice::ReadWrite),
                      "Could not open the update download file");
        const qint64 offset = m_partial.size();
        requireUpdate(offset <= package.size, "The partial update download is invalid");
        m_partial.seek(offset);
        QNetworkRequest request(m_options.baseUrl.resolved(QUrl(u'/' + package.path)));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::SameOriginRedirectPolicy);
        request.setRawHeader("Accept-Encoding", "identity");
        request.setRawHeader("Cache-Control", "no-cache");
        request.setTransferTimeout(30000);
        if (offset > 0) {
            request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + '-');
            request.setRawHeader("If-Range", validator);
        }
        auto* reply = m_network.get(request);
        m_reply = reply;
        const quint64 generation = ++m_generation;
        auto accepted = std::make_shared<bool>(false);
        auto error = std::make_shared<QString>();
        m_status.received = offset;
        m_status.total = package.size;
        setState(UpdateState::Downloading);
        m_deadline.start(30 * 60 * 1000);
        const auto consume = [this, reply, package, offset, accepted, error, generation] {
            if (generation != m_generation || !error->isEmpty()) {
                return;
            }
            try {
                if (!*accepted) {
                    const int status =
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    requireUpdate(status == 200 || status == 206,
                                  "The update package could not be downloaded");
                    if (status == 206) {
                        const QByteArray expected = "bytes " + QByteArray::number(offset) + '-' +
                                                    QByteArray::number(package.size - 1) + '/' +
                                                    QByteArray::number(package.size);
                        requireUpdate(reply->rawHeader("Content-Range") == expected,
                                      "The server returned an invalid download range");
                    } else if (offset > 0) {
                        requireUpdate(m_partial.resize(0) && m_partial.seek(0),
                                      "Could not restart the update download");
                    }
                    m_persisted.insert(QStringLiteral("partialHash"), package.sha256);
                    m_persisted.insert(QStringLiteral("validator"),
                                       QString::fromLatin1(reply->rawHeader("ETag")));
                    saveState();
                    *accepted = true;
                }
                while (reply->bytesAvailable() > 0) {
                    const QByteArray bytes = reply->read(65536);
                    requireUpdate(m_partial.pos() + bytes.size() <= package.size &&
                                      m_partial.write(bytes) == bytes.size(),
                                  "The download exceeded its signed size or could not be saved");
                }
                m_status.received = m_partial.pos();
                emit statusChanged();
            } catch (const std::exception& exception) {
                *error = QString::fromUtf8(exception.what());
                reply->abort();
            }
        };
        connect(reply, &QIODevice::readyRead, this, consume);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, package, error, consume, generation] {
                    reply->deleteLater();
                    if (generation != m_generation) {
                        return;
                    }
                    if (reply->bytesAvailable() > 0) {
                        consume();
                    }
                    m_reply = nullptr;
                    m_deadline.stop();
                    m_partial.close();
                    try {
                        requireUpdate(error->isEmpty() && reply->error() == QNetworkReply::NoError,
                                      "The update download was interrupted");
                        setState(UpdateState::Verifying);
                        verifyFile(m_partial.fileName(), package.size, package.sha256);
                        requireUpdate(
                            QFile::rename(m_partial.fileName(),
                                          cachePath(package.sha256 + QStringLiteral(".zip"))),
                            "Could not save the verified update");
                        setState(UpdateState::Ready);
                        emit updateReady();
                    } catch (const std::exception& exception) {
                        if (reply->error() == QNetworkReply::NoError || !error->isEmpty()) {
                            m_partial.remove();
                        }
                        if (++m_attempt <= 2) {
                            setState(UpdateState::Checking);
                            m_retry.start(m_attempt * 2000);
                        } else {
                            fail(error->isEmpty() ? QString::fromUtf8(exception.what()) : *error);
                        }
                    }
                });
    } catch (const std::exception& error) {
        m_partial.close();
        fail(QString::fromUtf8(error.what()));
    }
}

void UpdateService::cancel() {
    ++m_generation;
    m_retry.stop();
    m_deadline.stop();
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }
    m_partial.close();
    if (m_status.state != UpdateState::Unavailable && m_status.state != UpdateState::Applying) {
        setState(m_release ? UpdateState::Available : UpdateState::Idle);
    }
}

void UpdateService::requestRestart() {
    if (m_status.state == UpdateState::Ready) {
        emit restartRequested();
    }
}

void UpdateService::reportBlocked(const QString& reason) {
    setState(UpdateState::Ready, reason);
}

void UpdateService::beginApply() {
    if (m_status.state != UpdateState::Ready || !m_release) {
        return;
    }
    try {
        const auto& package = m_release->updatePackage(m_variant);
        const QString archive = cachePath(package.sha256 + QStringLiteral(".zip"));
        verifyFile(archive, package.size, package.sha256);
        writeAtomic(cachePath(QStringLiteral("release.json")), m_release->envelope);
        const QString pipe =
            QStringLiteral("snow-shot-update-") + QUuid::createUuid().toString(QUuid::Id128);
        m_server.close();
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        requireUpdate(m_server.listen(pipe), "Could not create the application update coordinator");
        disconnect(&m_server, nullptr, this, nullptr);
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (auto* socket = m_server.nextPendingConnection()) {
                const auto read = [this, socket] {
                    if (!socket->canReadLine()) {
                        return;
                    }
                    const QByteArray line = socket->readLine(4096).trimmed();
                    m_handoffTimeout.stop();
                    m_server.close();
                    if (line == "ready") {
                        emit handoffReady();
                        socket->write(m_status.state == UpdateState::Applying ? "go\n"
                                                                              : "cancel\n");
                        socket->flush();
                    } else {
                        fail(QString::fromUtf8(line).mid(7));
                    }
                };
                connect(socket, &QLocalSocket::readyRead, this, read);
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                read();
            }
        });
        const QString helper =
            QDir(m_options.root).filePath(QStringLiteral("bin/snow-shot-updater.exe"));
        const QStringList args{QStringLiteral("--launch"),
                               QStringLiteral("--target"),
                               m_options.root,
                               QStringLiteral("--pipe"),
                               pipe,
                               QStringLiteral("--parent"),
                               QString::number(QCoreApplication::applicationPid()),
                               QStringLiteral("--manifest"),
                               cachePath(QStringLiteral("release.json")),
                               QStringLiteral("--archive"),
                               archive,
                               QStringLiteral("--result"),
                               cachePath(QStringLiteral("result.txt"))};
        requireUpdate(QProcess::startDetached(helper, args),
                      "Could not launch the application update helper");
        setState(UpdateState::Applying);
        m_handoffTimeout.start(180000);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}
} // namespace snow_shot::update
