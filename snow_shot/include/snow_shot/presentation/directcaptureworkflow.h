#ifndef SNOW_SHOT_PRESENTATION_DIRECTCAPTUREWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_DIRECTCAPTUREWORKFLOW_H

#include "snow_shot/presentation/screenshotpdfexport.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/presentation/screenshothidpiimagescaling.h"
#include <QDateTime>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <QVector>

#include <deque>
#include <functional>

namespace snow_shot::presentation {
enum class DirectCaptureTarget { FocusedWindow, CurrentMonitor };

[[nodiscard]] bool directCaptureAutoSaveEnabled(DirectCaptureTarget target,
                                                const QStringList& actions);

struct DirectCaptureDisplay {
    QImage image;
    QRect physicalBounds;
    QString stableId;
    QString name;
    QRect logicalBounds{};
    quint32 nativeDisplayId = 0;
};

struct DirectCaptureRequest {
    DirectCaptureTarget target = DirectCaptureTarget::CurrentMonitor;
    quintptr window = 0;
    QString monitorName;
    QDateTime requestedAt;
    bool autoSave = false;
    bool copyFile = false;
    bool historyEnabled = false;
    QString configuredSaveDirectory;
    QString imageFormat;
    ScreenshotImageEncodingOptions encoding;
    ScreenshotCompressionLevel historyDisplayCompressionLevel = ScreenshotCompressionLevel::Medium;
    ScreenshotPdfOptions pdf;
    QString filenameFormat;
    bool restoreOriginalScreenColors = false;
    bool shutterSoundNotification = true;
    bool captureCursor = false;
    quint8 captureBackend = 0;
    // Called on native capture threads with the fixed target's geometry before allocation.
    std::function<bool(QSize)> admitRaster;
    QString hiDpiImageScaling = QStringLiteral("disabled");
    QVector<ScreenshotHiDpiDisplay> scalingDisplays;
};

struct DirectCaptureFrame {
    QImage image;
    QRect physicalBounds;
    QString identity;
    quint8 backend = 0;
    QString error;
    QVector<DirectCaptureDisplay> displays{};
    QRect logicalBounds{};

    [[nodiscard]] bool isValid() const {
        return error.isEmpty() && !image.isNull() && physicalBounds.size() == image.size();
    }
};

[[nodiscard]] inline qreal directCaptureImageScale(const DirectCaptureRequest& request,
                                                   const DirectCaptureFrame& frame) {
    const QRect bounds = frame.logicalBounds.isEmpty() ? frame.physicalBounds : frame.logicalBounds;
    return screenshotHiDpiImageScale(request.hiDpiImageScaling, QRectF(bounds),
                                     request.scalingDisplays);
}

struct DirectCapturePorts {
    using Completion = std::function<void(QString)>;
    std::function<bool(const DirectCaptureRequest&, std::function<void(DirectCaptureFrame)>)>
        acquire;
    std::function<bool(const DirectCaptureRequest&, const DirectCaptureFrame&,
                       std::function<void(QString, QString)>)>
        save;
    std::function<bool(const DirectCaptureRequest&, const DirectCaptureFrame&, Completion)> copy;
    std::function<bool(const DirectCaptureRequest&, const DirectCaptureFrame&, Completion)> history;
    std::function<void(const QString&, bool)> report;
    std::function<void()> captureRequested;
    std::function<void()> finished{};
};

class DirectCaptureWorkflow final : public QObject {
  public:
    explicit DirectCaptureWorkflow(DirectCapturePorts ports, QObject* parent = nullptr);
    void enqueue(DirectCaptureRequest request);
    void shutdown();
    [[nodiscard]] qsizetype pendingCount() const;

  private:
    enum class Phase { Idle, Acquiring, Saving, Copying, History, Stopped };
    void startNext();
    void save();
    void copy();
    void publishHistory();
    void finish();
    void report(const QString& error, bool warning = false);

    DirectCapturePorts m_ports;
    std::deque<DirectCaptureRequest> m_queue;
    DirectCaptureFrame m_frame;
    bool m_copySucceeded = false;
    Phase m_phase = Phase::Idle;
    quint64 m_generation = 0;
};
} // namespace snow_shot::presentation

#endif
