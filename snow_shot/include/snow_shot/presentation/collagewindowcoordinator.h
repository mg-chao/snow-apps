// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGEWINDOWCOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_COLLAGEWINDOWCOORDINATOR_H

#include "snow_shot/presentation/collagewindow.h"

#include <QObject>
#include <QPointer>

namespace snow_shot::presentation {

class CollageWindowCoordinator final : public QObject {
    Q_OBJECT
  public:
    explicit CollageWindowCoordinator(QObject* parent = nullptr,
                                      CollageWindowServices services = {});
    ~CollageWindowCoordinator() override;

    void present(const QVector<storage::CaptureHistoryRecord>& records = {});
    void presentPinnedRecords(const QStringList& recordIds);
    void presentPinnedWindows(const QVector<ScreenshotPinnedImageSnapshot>& snapshots);
    [[nodiscard]] bool presentSplitImagesAsPdf(ImageSplitSnapshot snapshot);
    void presentSelectedFiles(std::shared_ptr<platform::SelectedFileBackend> backend,
                              platform::SelectedFileTarget target);
    void shutdown();
    [[nodiscard]] CollageWindow* window() const;

  private:
    CollageWindow* ensureWindow();

    CollageWindowServices m_services;
    QPointer<CollageWindow> m_window;
    bool m_shutdown = false;
};

} // namespace snow_shot::presentation
#endif
