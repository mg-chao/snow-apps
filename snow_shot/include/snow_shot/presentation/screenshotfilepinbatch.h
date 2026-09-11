#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTFILEPINBATCH_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTFILEPINBATCH_H

#include "snow_shot/platform/windows/selectedfiles.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include <QObject>

class ScreenshotFilePinBatch final : public QObject {
  public:
    using Present = std::function<bool(ScreenshotClipboardContent)>;
    using Source = std::function<QStringList(const ScreenshotExportCancellation&)>;

    explicit ScreenshotFilePinBatch(QObject* parent = nullptr);
    ~ScreenshotFilePinBatch() override;
    void start(QStringList paths, Present present);
    void startSelection(std::shared_ptr<snow_shot::platform::windows::SelectedFileBackend> backend,
                        snow_shot::platform::windows::SelectedFileTarget target, Present present);
    void cancel();
    [[nodiscard]] bool active() const {
        return m_active;
    }

  private:
    void startSource(Source source, Present present);
    void next(quint64 generation);
    ScreenshotExportJobHandle m_job;
    quint64 m_generation = 0;
    QList<ScreenshotClipboardLocalImage> m_files;
    qsizetype m_next = 0;
    Present m_present;
    bool m_active = false;
};
#endif
