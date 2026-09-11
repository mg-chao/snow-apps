#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTFILEPINBATCH_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTFILEPINBATCH_H

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include <QHash>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

namespace snow_shot::platform::windows {
class SelectedFileBackend;
struct SelectedFileTarget;
} // namespace snow_shot::platform::windows

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
    void submitDecode(quint64 generation, qsizetype index);
    void submitPendingDecodes(quint64 generation);
    void dispatch(quint64 generation);

    ScreenshotExportJobHandle m_snapshotJob;
    QHash<qsizetype, ScreenshotExportJobHandle> m_decodeJobs;
    quint64 m_generation = 0;
    QList<ScreenshotClipboardLocalImage> m_files;
    // Completed decodes keyed by file index; out-of-order completions wait in
    // their slot so presentation always follows source order.
    QHash<qsizetype, std::optional<ScreenshotClipboardContent>> m_ready;
    qsizetype m_nextSubmit = 0;
    qsizetype m_nextPresent = 0;
    Present m_present;
    bool m_active = false;
    bool m_dispatching = false;
    bool m_redispatch = false;
};
#endif
