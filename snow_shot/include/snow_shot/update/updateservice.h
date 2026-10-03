#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <chrono>
#include <memory>

class QEvent;

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
    QUrl downloadUrl = {};
};

// Windows delegates installation to the updater helper. macOS checks releases over HTTPS
// and leaves package downloads and installation to the selected release host.
class UpdateService final : public QObject {
    Q_OBJECT
  public:
    struct Options {
        QString applicationDirectory;
        QString root;
        QString cacheDirectory;
        bool allowLocalHttp = false;
        std::chrono::milliseconds startupCheckDelay = std::chrono::seconds(30);
        std::chrono::milliseconds automaticCheckInterval = std::chrono::hours(24);
        // macOS check transport; overrides also support deterministic local-server tests.
        QString installedVersion;
        // Production discovery endpoint; loopback overrides require allowLocalHttp.
        QUrl githubApiUrl =
            QUrl(QStringLiteral("https://api.github.com/repos/mg-chao/snow-apps/releases"));
        QUrl giteeApiUrl =
            QUrl(QStringLiteral("https://gitee.com/api/v5/repos/mg-chao/snow-apps/releases"));
        std::chrono::milliseconds requestTimeout = std::chrono::seconds(30);
    };

    explicit UpdateService(Options options, QObject* parent = nullptr);
    ~UpdateService() override;
    const UpdateStatus& status() const;
    bool busy() const;
    bool handoffPending() const;
    void start();
    void setMode(const QString& mode);
    void setSystemProxy(bool enabled);
    void setProgressAppearance(const QJsonObject& appearance);
    void check(bool manual = true);
    void download();
    void cancel();
    void requestRestart();
    void beginApply();
    // Applies only an update that was already verified and ready before this launch.
    // Does not start network checks, downloads, or the background schedule.
    void applyAtStartup();
    void setRelaunchArguments(const QStringList& arguments);
    void reportBlocked(const QString& reason);

  signals:
    void statusChanged();
    void operationFinished(const QString& operation, const QString& outcome);
    void updateReady();
    void automaticUpdateAvailable(const QString& version);
    void restartRequested();
    void handoffReady();
    void handoffPendingChanged(bool pending);
    void handoffCommitted();

  protected:
    bool event(QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::update
