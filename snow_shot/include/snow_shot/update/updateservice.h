#pragma once

#include "snow_shot/update/updatecontract.h"
#include <QDateTime>
#include <QFile>
#include <QJsonObject>
#include <QLocalServer>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <optional>

namespace snow_shot::update {
enum class UpdateState {
    Unavailable,
    Idle,
    Checking,
    Available,
    Downloading,
    Verifying,
    Ready,
    Applying,
    Failed
};
struct UpdateStatus {
    UpdateState state = UpdateState::Unavailable;
    QString version;
    QString error;
    qint64 received = 0;
    qint64 total = 0;
};

class UpdateService final : public QObject {
    Q_OBJECT
  public:
    struct Options {
        QString root;
        QString cacheDirectory;
        QUrl baseUrl;
        QByteArray trustedKeys;
        bool allowLocalHttp = false;
        std::function<QDateTime()> now = [] { return QDateTime::currentDateTimeUtc(); };
    };
    explicit UpdateService(Options options, QObject* parent = nullptr);
    ~UpdateService() override;
    const UpdateStatus& status() const;
    void start();
    void setMode(const QString& mode);
    void setSystemProxy(bool enabled);
    void check(bool manual = true);
    void download();
    void cancel();
    void requestRestart();
    void beginApply();
    void reportBlocked(const QString& reason);

  signals:
    void statusChanged();
    void updateReady();
    void restartRequested();
    void handoffReady();

  private:
    void setState(UpdateState state, const QString& error = {});
    void fail(const QString& message);
    void saveState();
    void fetchMetadata();
    void fetchPackage();
    void acceptMetadata(const QByteArray& bytes);
    QNetworkReply* get(const QUrl& url);
    QString cachePath(const QString& name) const;

    Options m_options;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_schedule;
    QTimer m_deadline;
    QTimer m_retry;
    QTimer m_handoffTimeout;
    QLocalServer m_server;
    UpdateStatus m_status;
    QJsonObject m_persisted;
    std::optional<UpdateRelease> m_release;
    QString m_variant;
    QString m_installedVersion;
    QString m_mode = QStringLiteral("download");
    QFile m_partial;
    bool m_manual = false;
    bool m_autoDownload = false;
    int m_attempt = 0;
    quint64 m_generation = 0;
};
} // namespace snow_shot::update
