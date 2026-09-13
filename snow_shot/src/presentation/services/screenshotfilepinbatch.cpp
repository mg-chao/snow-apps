#include "snow_shot/presentation/screenshotfilepinbatch.h"

#include "snow_shot/platform/windows/selectedfiles.h"

#include <QPointer>

namespace {
// The export coordinator's pool runs two worker threads, so two concurrent
// decodes use it fully while bounding decoded-but-unpresented images to this
// count (the image being presented is the only additional one).
constexpr qsizetype kDecodeDepth = 2;
} // namespace

ScreenshotFilePinBatch::ScreenshotFilePinBatch(QObject* parent) : QObject(parent) {}

ScreenshotFilePinBatch::~ScreenshotFilePinBatch() {
    cancel();
}

void ScreenshotFilePinBatch::cancel() {
    ++m_generation;
    m_snapshotJob.cancel();
    m_snapshotJob = {};
    for (const ScreenshotExportJobHandle& job : m_decodeJobs) {
        job.cancel();
    }
    m_decodeJobs.clear();
    m_files.clear();
    m_ready.clear();
    m_present = {};
    m_nextSubmit = 0;
    m_nextPresent = 0;
    m_active = false;
    m_redispatch = false;
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
    auto first = std::make_shared<std::optional<ScreenshotClipboardContent>>();
    m_snapshotJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [source = std::move(source), files, first](const ScreenshotExportCancellation& token) {
            const QStringList paths = source(token);
            *files = ScreenshotClipboardContentReader::snapshotLocalFiles(
                paths, [&token]() { return token.isCancellationRequested(); });
            if (!files->isEmpty()) {
                // Decoding the first file on the worker keeps the first pin
                // from paying a queue round trip before its decode starts.
                ScreenshotClipboardContentSnapshot snapshot;
                snapshot.localImage = files->constFirst();
                *first = ScreenshotClipboardContentReader::decode(
                    std::move(snapshot), [&token]() { return token.isCancellationRequested(); });
            }
            return ScreenshotExportTaskResult{};
        },
        [this, generation, files, first](ScreenshotExportTaskResult result) {
            if (generation != m_generation) {
                return;
            }
            if (!result.succeeded()) {
                cancel();
                return;
            }
            m_snapshotJob = {};
            m_files = std::move(*files);
            if (!m_files.isEmpty()) {
                m_nextSubmit = 1;
                m_ready.insert(0, std::move(*first));
            }
            dispatch(generation);
        });
    if (!m_snapshotJob.isValid()) {
        cancel();
    }
}

void ScreenshotFilePinBatch::submitDecode(quint64 generation, qsizetype index) {
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.localImage = m_files.at(index);
    auto content = std::make_shared<std::optional<ScreenshotClipboardContent>>();
    const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [snapshot = std::move(snapshot),
         content](const ScreenshotExportCancellation& token) mutable {
            *content = ScreenshotClipboardContentReader::decode(
                std::move(snapshot), [&token]() { return token.isCancellationRequested(); });
            return ScreenshotExportTaskResult{};
        },
        [this, generation, index, content](ScreenshotExportTaskResult result) {
            if (generation != m_generation) {
                return;
            }
            m_decodeJobs.remove(index);
            if (!result.succeeded()) {
                cancel();
                return;
            }
            m_ready.insert(index, std::move(*content));
            dispatch(generation);
        });
    if (!job.isValid()) {
        cancel();
        return;
    }
    m_decodeJobs.insert(index, job);
}

void ScreenshotFilePinBatch::submitPendingDecodes(quint64 generation) {
    while (m_decodeJobs.size() < kDecodeDepth && m_nextSubmit < m_files.size()) {
        submitDecode(generation, m_nextSubmit++);
    }
}

void ScreenshotFilePinBatch::dispatch(quint64 generation) {
    if (generation != m_generation) {
        return;
    }
    if (m_dispatching) {
        // A queued completion ran inside present(); remember it so the loop
        // unwinding here picks its slot up instead of dropping it.
        m_redispatch = true;
        return;
    }
    m_dispatching = true;
    while (true) {
        if (m_nextPresent >= m_files.size()) {
            cancel();
            break;
        }
        const auto ready = m_ready.find(m_nextPresent);
        if (ready == m_ready.end()) {
            break;
        }
        std::optional<ScreenshotClipboardContent> content = std::move(*ready);
        m_ready.erase(ready);
        ++m_nextPresent;
        // Refill before presenting so the next decode overlaps this call.
        submitPendingDecodes(generation);
        if (generation != m_generation) {
            break;
        }
        if (!content.has_value()) {
            continue;
        }
        const QPointer<ScreenshotFilePinBatch> guard(this);
        const Present present = m_present;
        const bool proceed = present && present(std::move(*content));
        if (!guard || generation != m_generation) {
            break;
        }
        if (!proceed) {
            cancel();
            break;
        }
    }
    m_dispatching = false;
    if (m_redispatch) {
        m_redispatch = false;
        dispatch(m_generation);
    }
}
