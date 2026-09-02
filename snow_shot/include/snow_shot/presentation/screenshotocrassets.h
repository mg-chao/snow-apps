#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRASSETS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRASSETS_H

#include <QObject>
#include <QString>

#include <memory>
#include <functional>

enum class ScreenshotOcrAssetPhase {
    Unchecked,
    Verifying,
    Downloading,
    ReadyOffline,
    ReadyCached,
    Failed,
};

struct ScreenshotOcrAssetStatus {
    ScreenshotOcrAssetPhase phase = ScreenshotOcrAssetPhase::Unchecked;
    QString component;
    qint64 receivedBytes = 0;
    qint64 totalBytes = 0;
    QString error;
};

struct ScreenshotOcrResolvedAssets {
    QString runtimeVersion;
    QString runtimeDirectory;
    QString processPath;
    QString detectorModelPath;
    QString recognizerModelPath;
    QString dictionaryPath;
    QString stateDirectory;
    bool offline = false;

    [[nodiscard]] bool valid() const;
};

class ScreenshotOcrAssets final : public QObject {
    Q_OBJECT

  public:
    struct Options {
        // The trusted descriptor and optional offline payload live here.
        QString offlineRoot;
        // Downloaded, application-managed components live here.
        QString cacheRoot;
        QString proxyUrl;
        // Optional test hooks. Production uses the built-in HTTPS downloader
        // and minizip-ng extractor.
        std::function<bool(const QString& url, const QString& destination, QString* error)>
            downloadOverride;
        std::function<bool(const QString& archive, const QString& destination, QString* error)>
            extractOverride;
    };

    explicit ScreenshotOcrAssets(Options options, QObject* parent = nullptr);
    ~ScreenshotOcrAssets() override;

    void prepare();
    void setProxyUrl(const QString& proxyUrl);

  signals:
    void statusChanged(const ScreenshotOcrAssetStatus& status);
    void ready(const ScreenshotOcrResolvedAssets& assets);
    void failed(const QString& error);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

Q_DECLARE_METATYPE(ScreenshotOcrAssetStatus)
Q_DECLARE_METATYPE(ScreenshotOcrResolvedAssets)

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRASSETS_H
