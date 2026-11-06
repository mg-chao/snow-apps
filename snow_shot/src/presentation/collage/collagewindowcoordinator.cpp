// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagewindowcoordinator.h"
#include "snow_shot/presentation/imagesplitdocument.h"

#include <utility>

namespace snow_shot::presentation {

ImageStitchingWindowCoordinator::ImageStitchingWindowCoordinator(
    QObject* parent, ImageStitchingWindowServices services)
    : QObject(parent), m_services(std::move(services)) {}

ImageStitchingWindowCoordinator::~ImageStitchingWindowCoordinator() {
    shutdown();
}

ImageStitchingWindow* ImageStitchingWindowCoordinator::ensureWindow() {
    if (m_shutdown || (m_window && m_window->busy()))
        return nullptr;
    if (!m_window) {
        auto* window = new ImageStitchingWindow(this, m_services);
        m_window = window;
        connect(window, &ImageStitchingWindow::closed, this, [this, window] {
            if (m_window == window)
                m_window.clear();
            window->deleteLater();
        });
    }
    return m_window;
}

void ImageStitchingWindowCoordinator::present(
    const QVector<storage::CaptureHistoryRecord>& records) {
    if (auto* window = ensureWindow())
        window->present(records);
}

void ImageStitchingWindowCoordinator::presentPinnedRecords(const QStringList& recordIds) {
    if (auto* window = ensureWindow())
        window->presentPinnedRecords(recordIds);
}

void ImageStitchingWindowCoordinator::presentPinnedWindows(
    const QVector<ScreenshotPinnedImageSnapshot>& snapshots) {
    if (auto* window = ensureWindow())
        window->presentPinnedWindows(snapshots);
}

void ImageStitchingWindowCoordinator::shutdown() {
    m_shutdown = true;
    if (m_window)
        m_window->shutdown();
}

bool ImageStitchingWindowCoordinator::presentSplitImagesAsPdf(ImageSplitSnapshot snapshot) {
    if (auto* window = ensureWindow())
        return window->presentSplitImagesAsPdf(std::move(snapshot));
    return false;
}

void ImageStitchingWindowCoordinator::presentSelectedFiles(
    std::shared_ptr<platform::SelectedFileBackend> backend, platform::SelectedFileTarget target) {
    if (auto* window = ensureWindow()) {
        window->present();
        window->addSelectedFiles(std::move(backend), target);
    }
}

ImageStitchingWindow* ImageStitchingWindowCoordinator::window() const {
    return m_window;
}

} // namespace snow_shot::presentation
