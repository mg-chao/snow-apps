#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPRINTSERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPRINTSERVICE_H

#include <QImage>
#include <QObject>
#include <QRectF>

#include <functional>
#include <memory>

class QWidget;

class ScreenshotPrintService final : public QObject {
  public:
    // HandedOff means a system-owned dialog closed without reporting a spooler outcome.
    // Keep the capture available because it may have been cancelled.
    enum class Status { Submitted, Cancelled, Failed, Unavailable, HandedOff };
    struct Result {
        Status status = Status::Failed;
        QString error;
    };
    using Completion = std::function<void(Result)>;
    using Backend = std::function<void(QWidget*, QImage, Completion)>;

    explicit ScreenshotPrintService(Backend primary, Backend legacy = {},
                                    QObject* parent = nullptr);
    [[nodiscard]] static ScreenshotPrintService& shared();
    // Accepted requests finish asynchronously on the GUI thread. Unavailable requests
    // legacy recovery internally; callers receive Failed if recovery is unavailable.
    [[nodiscard]] bool printImage(QObject* receiver, QWidget* owner, QImage snapshot,
                                  Completion completion);
    [[nodiscard]] bool busy() const;
    [[nodiscard]] static QImage opaqueImage(const QImage& image);
    [[nodiscard]] static QRectF fittedRect(QSize imageSize, const QRectF& printableRect);

  private:
    struct Request;
    void startBackend(const std::shared_ptr<Request>& request, bool legacy);
    Backend m_primary;
    Backend m_legacy;
    std::shared_ptr<Request> m_request;
};

#endif
