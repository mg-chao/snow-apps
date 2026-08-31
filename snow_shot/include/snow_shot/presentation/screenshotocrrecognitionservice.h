#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRRECOGNITIONSERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRRECOGNITIONSERVICE_H

#include <QImage>
#include <QObject>
#include <QRectF>
#include <QSize>
#include <QString>

#include <functional>
#include <memory>

inline constexpr qint64 kScreenshotOcrMaximumPixels = 3840LL * 2160LL;

[[nodiscard]] inline bool screenshotOcrImageWithinPixelLimit(const QSize& size) {
    if (size.width() < 1 || size.height() < 1) {
        return false;
    }
    return static_cast<qint64>(size.width()) * static_cast<qint64>(size.height()) <=
           kScreenshotOcrMaximumPixels;
}

class ScreenshotOcrPresentation;
struct ScreenshotOcrRecognitionResult {
    std::shared_ptr<ScreenshotOcrPresentation> presentation;
    QString error;
};

enum class ScreenshotOcrRequestPriority { Interactive, Prefetch };

enum class ScreenshotOcrBackendPreference { Cpu, DirectMl };

struct ScreenshotOcrRequest {
    QImage image;
    QRectF canvasRect;
    ScreenshotOcrRequestPriority priority = ScreenshotOcrRequestPriority::Interactive;
};

class ScreenshotOcrRecognitionPort : public QObject {
  public:
    explicit ScreenshotOcrRecognitionPort(QObject* parent = nullptr) : QObject(parent) {}
    using RequestToken = quint64;
    using Completion = std::function<void(ScreenshotOcrRecognitionResult)>;

    virtual ~ScreenshotOcrRecognitionPort() = default;
    virtual RequestToken recognize(ScreenshotOcrRequest request, QObject* receiver,
                                   Completion completion) = 0;
    virtual void cancel(RequestToken token) = 0;
    virtual bool reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) = 0;
    // Implementations that manage disk-backed models report whether the
    // required files are already available before a request is queued.
    virtual bool modelFilesReady() const { return true; }
};

class ScreenshotOcrRecognitionService final : public ScreenshotOcrRecognitionPort {
    Q_OBJECT

  public:
    struct Options {
        // Maximum concurrent workers; the pool grows only while queued demand requires it.
        int workerCount = 2;
        QString modelStoreDirectory;
    };

    explicit ScreenshotOcrRecognitionService(QObject* parent = nullptr);
    explicit ScreenshotOcrRecognitionService(ScreenshotOcrBackendPreference backendPreference,
                                             QObject* parent = nullptr);
    explicit ScreenshotOcrRecognitionService(const Options& options,
                                             ScreenshotOcrBackendPreference backendPreference =
                                                 ScreenshotOcrBackendPreference::Cpu,
                                             QObject* parent = nullptr);
    ~ScreenshotOcrRecognitionService() override;

    RequestToken recognize(ScreenshotOcrRequest request, QObject* receiver,
                           Completion completion) override;
    void cancel(RequestToken token) override;
    bool reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) override;
    [[nodiscard]] bool modelFilesReady() const override;
    void setBackendPreference(ScreenshotOcrBackendPreference preference);
    [[nodiscard]] int liveWorkerCount() const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    RequestToken m_nextToken = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRRECOGNITIONSERVICE_H
