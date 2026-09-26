#ifndef SNOW_SHOT_PRESENTATION_DIRECTCAPTURECONTROLLER_H
#define SNOW_SHOT_PRESENTATION_DIRECTCAPTURECONTROLLER_H

#include <QObject>
#include <QImage>
#include <QJsonObject>
#include <functional>
#include <memory>

namespace snow_shot::presentation {
class DirectCaptureController final : public QObject {
    Q_OBJECT
  public:
    explicit DirectCaptureController(QObject* parent = nullptr);
    ~DirectCaptureController() override;
    void captureFocusedWindow();
    void captureCurrentMonitor();
    void shutdown();
    void cancelMcpCapture();
    [[nodiscard]] bool mcpCapture(const QJsonObject& options,
                                  std::function<void(QImage, QJsonObject, QString)> completion);
    [[nodiscard]] bool blocksApplicationUpdate() const;

  signals:
    void operationFailed(const QString& message, bool warning);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
#endif
