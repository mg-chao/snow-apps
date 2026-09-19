#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTQRRECOGNITIONSERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTQRRECOGNITIONSERVICE_H

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

struct ScreenshotQrRecognitionResult {
    QStringList contents;
    QString error;
};

class ScreenshotQrRecognitionPort : public QObject {
  public:
    explicit ScreenshotQrRecognitionPort(QObject* parent = nullptr) : QObject(parent) {}
    using RequestToken = quint64;
    using Completion = std::function<void(ScreenshotQrRecognitionResult)>;

    ~ScreenshotQrRecognitionPort() override = default;
    virtual RequestToken recognize(QImage image, QObject* receiver, Completion completion) = 0;
    virtual void cancel(RequestToken token) = 0;
};

class ScreenshotQrRecognitionService final : public ScreenshotQrRecognitionPort {
    Q_OBJECT

  public:
    // Recognition owns a short-lived worker; the service itself remains lightweight while idle.
    // An empty modelsDirectory resolves to <application dir>/assets/qrcode at runtime; tests
    // inject a custom directory to exercise non-ASCII installation paths.
    explicit ScreenshotQrRecognitionService(QObject* parent = nullptr,
                                            const QString& modelsDirectory = QString());
    ~ScreenshotQrRecognitionService() override;

    RequestToken recognize(QImage image, QObject* receiver, Completion completion) override;
    void cancel(RequestToken token) override;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    RequestToken m_nextToken = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTQRRECOGNITIONSERVICE_H
