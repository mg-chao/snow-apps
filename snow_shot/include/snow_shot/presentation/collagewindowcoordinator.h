// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGEWINDOWCOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_COLLAGEWINDOWCOORDINATOR_H

#include "snow_shot/presentation/collagewindow.h"

#include <QObject>
#include <QPointer>

namespace snow_shot::presentation {

class ImageStitchingWindowCoordinator final : public QObject {
    Q_OBJECT
  public:
    explicit ImageStitchingWindowCoordinator(QObject* parent = nullptr,
                                             ImageStitchingWindowServices services = {});
    ~ImageStitchingWindowCoordinator() override;

    void present(const QVector<storage::CaptureHistoryRecord>& records = {});
    void presentPinnedRecords(const QStringList& recordIds);
    void presentPinnedWindows(const QVector<ScreenshotPinnedImageSnapshot>& snapshots);
    [[nodiscard]] bool presentSplitImagesAsPdf(ImageSplitSnapshot snapshot);
    void presentSelectedFiles(std::shared_ptr<platform::SelectedFileBackend> backend,
                              platform::SelectedFileTarget target);
    void shutdown();
    [[nodiscard]] ImageStitchingWindow* window() const;

  private:
    ImageStitchingWindow* ensureWindow();

    ImageStitchingWindowServices m_services;
    QPointer<ImageStitchingWindow> m_window;
    bool m_shutdown = false;
};

} // namespace snow_shot::presentation
#endif
