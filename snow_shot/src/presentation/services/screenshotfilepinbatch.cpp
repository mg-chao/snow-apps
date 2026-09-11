#include "snow_shot/presentation/screenshotfilepinbatch.h"

#include <QPointer>

ScreenshotFilePinBatch::ScreenshotFilePinBatch(QObject* parent) : QObject(parent) {}

ScreenshotFilePinBatch::~ScreenshotFilePinBatch() {
    cancel();
}

void ScreenshotFilePinBatch::cancel() {
    ++m_generation;
    m_job.cancel();
    m_job = {};
    m_files.clear();
    m_present = {};
    m_next = 0;
    m_active = false;
}

void ScreenshotFilePinBatch::start(QStringList paths, Present present) {
    startSource([paths = std::move(paths)](const ScreenshotExportCancellation&) { return paths; },
                std::move(present));
}

void ScreenshotFilePinBatch::startSelection(
    std::shared_ptr<snow_shot::platform::windows::SelectedFileBackend> backend,
    snow_shot::platform::windows::SelectedFileTarget target, Present present) {
    startSource(
        [backend = std::move(backend), target](const ScreenshotExportCancellation& token) {
            return backend->selectedFiles(target,
                                          [&token]() { return token.isCancellationRequested(); });
        },
        std::move(present));
}

void ScreenshotFilePinBatch::startSource(Source source, Present present) {
    cancel();
    m_active = true;
    m_present = std::move(present);
    const quint64 generation = m_generation;
    auto files = std::make_shared<QList<ScreenshotClipboardLocalImage>>();
    m_job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [source = std::move(source), files](const ScreenshotExportCancellation& token) {
            const QStringList paths = source(token);
            *files = ScreenshotClipboardContentReader::snapshotLocalFiles(
                paths, [&token]() { return token.isCancellationRequested(); });
            return ScreenshotExportTaskResult{};
        },
        [this, generation, files](ScreenshotExportTaskResult result) {
            if (generation != m_generation) {
                return;
            }
            if (!result.succeeded()) {
                cancel();
                return;
            }
            m_files = std::move(*files);
            next(generation);
        });
    if (!m_job.isValid()) {
        cancel();
    }
}

void ScreenshotFilePinBatch::next(quint64 generation) {
    if (generation != m_generation) {
        return;
    }
    if (m_next >= m_files.size()) {
        cancel();
        return;
    }
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.localImage = m_files.at(m_next++);
    auto content = std::make_shared<std::optional<ScreenshotClipboardContent>>();
    m_job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [snapshot = std::move(snapshot),
         content](const ScreenshotExportCancellation& token) mutable {
            *content = ScreenshotClipboardContentReader::decode(
                std::move(snapshot), [&token]() { return token.isCancellationRequested(); });
            return ScreenshotExportTaskResult{};
        },
        [this, generation, content](ScreenshotExportTaskResult result) {
            if (generation != m_generation) {
                return;
            }
            if (result.succeeded() && content->has_value()) {
                const QPointer<ScreenshotFilePinBatch> guard(this);
                const auto present = m_present;
                const bool proceed = present && present(std::move(content->value()));
                if (!guard || generation != m_generation) {
                    return;
                }
                if (!proceed) {
                    cancel();
                    return;
                }
            }
            next(generation);
        });
    if (!m_job.isValid()) {
        cancel();
    }
}
