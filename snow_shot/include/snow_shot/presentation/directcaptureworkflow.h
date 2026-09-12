#ifndef SNOW_SHOT_PRESENTATION_DIRECTCAPTUREWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_DIRECTCAPTUREWORKFLOW_H

#include "snow_shot/presentation/screenshotpdfexport.h"
#include <QDateTime>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QStringList>
#include <QVector>

#include <deque>
#include <functional>

namespace snow_shot::presentation {
enum class DirectCaptureTarget { FocusedWindow, CurrentMonitor };

struct DirectCaptureDisplay {
    QImage image;
    QRect physicalBounds;
    QString stableId;
    QString name;
};

struct DirectCaptureRequest {
    DirectCaptureTarget target = DirectCaptureTarget::CurrentMonitor;
    quintptr window = 0;
    QString monitorName;
    QDateTime requestedAt;
    bool autoSave = false;
    bool copyFile = false;
    bool historyEnabled = false;
    QStringList directories;
    QString imageFormat;
    ScreenshotPdfOptions pdf;
    QString filenameFormat;
    bool restoreOriginalScreenColors = false;
    bool shutterSoundNotification = true;
};

struct DirectCaptureFrame {
    QImage image;
    QRect physicalBounds;
    QString identity;
    quint8 backend = 0;
    QString error;
    QVector<DirectCaptureDisplay> displays;
    QByteArray canonicalPng;

    [[nodiscard]] bool isValid() const {
        return error.isEmpty() && !image.isNull() && physicalBounds.size() == image.size();
    }
};

struct DirectCapturePorts {
    using Completion = std::function<void(QString)>;
    std::function<bool(const DirectCaptureRequest&, std::function<void(DirectCaptureFrame)>)>
        acquire;
    std::function<bool(const DirectCaptureRequest&, const DirectCaptureFrame&,
                       std::function<void(QString, QString)>)>
        save;
    std::function<bool(const DirectCaptureRequest&, const DirectCaptureFrame&, const QString&,
                       Completion)>
        copy;
    std::function<bool(const DirectCaptureRequest&, const DirectCaptureFrame&, Completion)> history;
    std::function<void(const QString&, bool)> report;
    std::function<void()> captureRequested;
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
    void saveOrCopy();
    void copy(const QString& path = {});
    void publishHistory();
    void finish();
    void report(const QString& error, bool warning = false);

    DirectCapturePorts m_ports;
    std::deque<DirectCaptureRequest> m_queue;
    DirectCaptureFrame m_frame;
    Phase m_phase = Phase::Idle;
    quint64 m_generation = 0;
};
} // namespace snow_shot::presentation

#endif
