// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagewindowcoordinator.h"
#include "snow_shot/presentation/imagesplitdocument.h"

#include <utility>

namespace snow_shot::presentation {

CollageWindowCoordinator::CollageWindowCoordinator(QObject* parent, CollageWindowServices services)
    : QObject(parent), m_services(std::move(services)) {}

CollageWindowCoordinator::~CollageWindowCoordinator() {
    shutdown();
}

CollageWindow* CollageWindowCoordinator::ensureWindow() {
    if (m_shutdown || (m_window && m_window->busy()))
        return nullptr;
    if (!m_window) {
        auto* window = new CollageWindow(this, m_services);
        m_window = window;
        connect(window, &CollageWindow::closed, this, [this, window] {
            if (m_window == window)
                m_window.clear();
            window->deleteLater();
        });
    }
    return m_window;
}

void CollageWindowCoordinator::present(const QVector<storage::CaptureHistoryRecord>& records) {
    if (auto* window = ensureWindow())
        window->present(records);
}

void CollageWindowCoordinator::presentPinnedRecords(const QStringList& recordIds) {
    if (auto* window = ensureWindow())
        window->presentPinnedRecords(recordIds);
}

void CollageWindowCoordinator::presentPinnedWindows(
    const QVector<ScreenshotPinnedImageSnapshot>& snapshots) {
    if (auto* window = ensureWindow())
        window->presentPinnedWindows(snapshots);
}

void CollageWindowCoordinator::shutdown() {
    m_shutdown = true;
    if (m_window)
        m_window->shutdown();
}

bool CollageWindowCoordinator::presentSplitImagesAsPdf(ImageSplitSnapshot snapshot) {
    if (auto* window = ensureWindow())
        return window->presentSplitImagesAsPdf(std::move(snapshot));
    return false;
}

void CollageWindowCoordinator::presentSelectedFiles(
    std::shared_ptr<platform::SelectedFileBackend> backend, platform::SelectedFileTarget target) {
    if (auto* window = ensureWindow()) {
        window->present();
        window->addSelectedFiles(std::move(backend), target);
    }
}

CollageWindow* CollageWindowCoordinator::window() const {
    return m_window;
}

} // namespace snow_shot::presentation
