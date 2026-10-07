#ifndef SNOW_SHOT_RECORDINGTRIMSESSION_H
#define SNOW_SHOT_RECORDINGTRIMSESSION_H

#include "snow_recording.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <future>
#include <functional>
#include <atomic>
#include <memory>

class ScreenRecordingAreaWindow;
class ScreenRecordingToolbarWindow;
class RecordingTrimToolbar;
class RecordingRenderDialog;

// The media lifetime is independent of the two recording windows. Native work never joins on UI.
class RecordingTrimSession final : public QObject {
  public:
    RecordingTrimSession(ScreenRecordingAreaWindow* area, ScreenRecordingToolbarWindow* toolbar,
                         QObject* parent);
    ~RecordingTrimSession() override;
    void open(const QString& sourcePath, const QString& outputPath,
              const SnowRecordingClipOptions& options, bool deferred, bool saveWhenReady);
    void exportClip(bool save);
    void detach();
    [[nodiscard]] bool ready() const;
    [[nodiscard]] QString phase() const;
    std::function<void(const QString&)> reportError;
    std::function<void()> exported;

  private:
    enum class Phase { Preparing, Trimming, Exporting };
    Phase m_phase = Phase::Preparing;
    struct OpenResult {
        SnowRecordingClip* clip = nullptr;
        QString error;
    };
    struct PublishResult {
        bool ok = false;
        QString error;
    };
    struct Publication {
        // Cancellation and final replacement have a single, nonblocking winner.
        enum State { Copying, Canceled, Committing };
        std::atomic<int> state{Copying};
        std::atomic<int> percent{0};
    };
    std::shared_ptr<Publication> m_publication;
    void poll();
    void seek(int frame, bool play);
    void setBusy(bool busy);
    void finishExport(bool ok, const QString& error);
    void publish();
    void releaseMedia();
    void showProgress();
    QPointer<ScreenRecordingAreaWindow> m_area;
    QPointer<ScreenRecordingToolbarWindow> m_toolbar;
    QPointer<RecordingTrimToolbar> m_panel;
    QTimer m_timer;
    std::future<OpenResult> m_openFuture;
    std::future<PublishResult> m_publishFuture;
    std::future<void> m_cancelFuture;
    SnowRecordingClip* m_clip = nullptr;
    SnowRecordingClipExport* m_export = nullptr;
    SnowRecordingClipInfo m_info{};
    quint64 m_revision = 0;
    quint64 m_first = 0;
    quint64 m_end = 0;
    quint64 m_cachedFirst = 0;
    quint64 m_cachedEnd = 0;
    QString m_sourcePath;
    QString m_outputPath;
    QString m_cachePath;
    QString m_pendingPath;
    QString m_destination;
    bool m_copy = false;
    bool m_deferred = false;
    bool m_busy = true;
    bool m_detached = false;
    bool m_saveWhenReady = false;
    bool m_previewFailed = false;
    QPointer<RecordingRenderDialog> m_dialog;
    double m_percent = 0;
};

#endif
