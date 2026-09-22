#pragma once

#include <QObject>
#include <QString>
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
};

// Windows delegates installation to the updater helper. macOS checks versions over HTTPS
// and leaves package downloads and installation to the official website.
class UpdateService final : public QObject {
    Q_OBJECT
  public:
    struct Options {
        QString applicationDirectory;
        QString root;
        QString cacheDirectory;
        QUrl baseUrl;
        bool allowLocalHttp = false;
        std::chrono::milliseconds startupCheckDelay = std::chrono::seconds(30);
        std::chrono::milliseconds automaticCheckInterval = std::chrono::hours(24);
        // macOS check transport; overrides also support deterministic local-server tests.
        QString installedVersion;
        std::chrono::milliseconds requestTimeout = std::chrono::seconds(30);
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
    void automaticUpdateAvailable(const QString& version);
    void restartRequested();
    void handoffReady();

  protected:
    bool event(QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::update
