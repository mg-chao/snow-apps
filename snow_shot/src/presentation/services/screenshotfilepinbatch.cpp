#include "snow_shot/presentation/screenshotfilepinbatch.h"

#include "snow_shot/platform/selectedfiles.h"
#include "snow_shot/image/screenshotdisplayimage.h"

#include <QPointer>
#include <QTimer>

namespace {
// The export coordinator's pool runs two worker threads, so two concurrent
// decodes use it fully while bounding decoded-but-unpresented images to this
// count (the image being presented is the only additional one).
constexpr qsizetype kDecodeDepth = 2;

std::optional<ScreenshotClipboardContent>
decodePinContent(ScreenshotClipboardContentSnapshot snapshot,
                 const ScreenshotClipboardContentReader::CancellationCheck& cancelled) {
    auto content = ScreenshotClipboardContentReader::decode(std::move(snapshot), cancelled);
    if (!content || (cancelled && cancelled()))
        return std::nullopt;
    if (content->preparedDisplayImage.isNull())
        content->preparedDisplayImage =
            snow_shot::image::normalizeScreenshotDisplayImage(content->image);
    if (content->preparedDisplayImage.isNull() || (cancelled && cancelled()))
        return std::nullopt;
    return content;
}
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
    m_completion = {};
    m_succeeded = false;
    m_duplicateFilter = {};
    m_skippedDuplicates.clear();
    m_nextSubmit = 0;
    m_nextPresent = 0;
    m_active = false;
    m_redispatch = false;
}

void ScreenshotFilePinBatch::finish(bool succeeded) {
    auto completion = std::move(m_completion);
    cancel();
    if (completion) {
        const quint64 generation = m_generation;
        QTimer::singleShot(0, this,
                           [this, generation, succeeded, completion = std::move(completion)] {
                               if (generation == m_generation)
                                   completion(succeeded);
                           });
    }
}

ScreenshotFilePinQueue::ScreenshotFilePinQueue(QObject* parent) : QObject(parent) {}

void ScreenshotFilePinQueue::enqueue(QStringList paths, Prepare prepare, Failure failure) {
    for (QString& path : paths)
        m_requests.enqueue({std::move(path), prepare, failure});
    startNext();
}

void ScreenshotFilePinQueue::cancel() {
    m_batch.cancel();
    m_requests.clear();
    m_running = false;
}

void ScreenshotFilePinQueue::startNext() {
    if (m_running || m_requests.isEmpty())
        return;
    m_running = true;
    auto request = m_requests.dequeue();
    auto options = request.prepare ? request.prepare() : Options{};
    const QString path = request.path;
    m_batch.start({path}, std::move(options.present), std::move(options.duplicateFilter),
                  [this, request = std::move(request)](bool succeeded) {
                      m_running = false;
                      const QPointer<ScreenshotFilePinQueue> guard(this);
                      if (!succeeded && request.failure)
                          request.failure(request.path);
                      if (guard)
                          startNext();
                  });
}

void ScreenshotFilePinBatch::start(QStringList paths, Present present, DuplicateFilter filter,
                                   Completion completion) {
    startSource(
        [paths = std::move(paths)](const ScreenshotExportCancellation&) {
            return snow_shot::platform::SelectedFileResult{paths};
        },
        std::move(present), {}, std::move(filter), std::move(completion));
}

void ScreenshotFilePinBatch::startSelection(
    std::shared_ptr<snow_shot::platform::SelectedFileBackend> backend,
    snow_shot::platform::SelectedFileTarget target, Present present, Failure failure,
    DuplicateFilter filter) {
    startSource(
        [backend = std::move(backend), target](const ScreenshotExportCancellation& token) {
            return backend->selectedFiles(target,
                                          [&token]() { return token.isCancellationRequested(); });
        },
        std::move(present), std::move(failure), std::move(filter));
}

void ScreenshotFilePinBatch::startSource(Source source, Present present, Failure failure,
                                         DuplicateFilter filter, Completion completion) {
    cancel();
    m_active = true;
    m_textStyle = ScreenshotClipboardContentReader::capturePinnedTextStyle();
    m_colorSwatchStyle = capturePinnedColorSwatchStyle();
    m_present = std::move(present);
    m_completion = std::move(completion);
    m_duplicateFilter = std::move(filter);
    const quint64 generation = m_generation;
    auto files = std::make_shared<QList<ScreenshotClipboardLocalImage>>();
    auto first = std::make_shared<std::optional<ScreenshotClipboardContent>>();
    auto selection = std::make_shared<snow_shot::platform::SelectedFileResult>();
    m_snapshotJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [source = std::move(source), files, first, selection, textStyle = m_textStyle,
         colorSwatchStyle = m_colorSwatchStyle,
         identities = m_duplicateFilter.identities](const ScreenshotExportCancellation& token) {
            *selection = source(token);
            if (selection->error != snow_shot::platform::SelectedFileError::None) {
                return ScreenshotExportTaskResult{};
            }
            *files = ScreenshotClipboardContentReader::snapshotLocalFiles(
                selection->paths, [&token]() { return token.isCancellationRequested(); });
            if (!files->isEmpty() && !identities.contains(files->constFirst().sourceIdentity.key)) {
                // Decoding the first file on the worker keeps the first pin
                // from paying a queue round trip before its decode starts.
                ScreenshotClipboardContentSnapshot snapshot;
                snapshot.localImage = files->constFirst();
                snapshot.textStyle = textStyle;
                snapshot.colorSwatchStyle = colorSwatchStyle;
                *first = decodePinContent(std::move(snapshot),
                                          [&token]() { return token.isCancellationRequested(); });
            }
            return ScreenshotExportTaskResult{};
        },
        [this, generation, files, first, selection,
         failure = std::move(failure)](ScreenshotExportTaskResult result) {
            if (generation != m_generation) {
                return;
            }
            if (!result.succeeded()) {
                finish(false);
                return;
            }
            if (selection->error != snow_shot::platform::SelectedFileError::None) {
                cancel();
                if (failure) {
                    failure(selection->error);
                }
                return;
            }
            m_snapshotJob = {};
            m_files = std::move(*files);
            if (!m_files.isEmpty()) {
                if (m_duplicateFilter.identities.contains(m_files.constFirst().sourceIdentity.key))
                    m_skippedDuplicates.insert(0);
                m_nextSubmit = 1;
                m_ready.insert(0, std::move(*first));
            }
            dispatch(generation);
        });
    if (!m_snapshotJob.isValid()) {
        finish(false);
    }
}

void ScreenshotFilePinBatch::submitDecode(quint64 generation, qsizetype index) {
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.localImage = m_files.at(index);
    snapshot.textStyle = m_textStyle;
    snapshot.colorSwatchStyle = m_colorSwatchStyle;
    auto content = std::make_shared<std::optional<ScreenshotClipboardContent>>();
    const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [snapshot = std::move(snapshot),
         content](const ScreenshotExportCancellation& token) mutable {
            *content = decodePinContent(std::move(snapshot),
                                        [&token]() { return token.isCancellationRequested(); });
            return ScreenshotExportTaskResult{};
        },
        [this, generation, index, content](ScreenshotExportTaskResult result) {
            if (generation != m_generation) {
                return;
            }
            m_decodeJobs.remove(index);
            if (!result.succeeded()) {
                finish(false);
                return;
            }
            m_ready.insert(index, std::move(*content));
            dispatch(generation);
        });
    if (!job.isValid()) {
        finish(false);
        return;
    }
    m_decodeJobs.insert(index, job);
}

void ScreenshotFilePinBatch::submitPendingDecodes(quint64 generation) {
    while (m_decodeJobs.size() < kDecodeDepth && m_nextSubmit < m_files.size()) {
        const qsizetype index = m_nextSubmit++;
        if (m_duplicateFilter.identities.contains(m_files.at(index).sourceIdentity.key)) {
            m_skippedDuplicates.insert(index);
            m_ready.insert(index, std::nullopt);
        } else {
            submitDecode(generation, index);
        }
        if (generation != m_generation)
            return;
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
            const QPointer<ScreenshotFilePinBatch> guard(this);
            finish(m_succeeded);
            if (!guard)
                return;
            break;
        }
        const auto ready = m_ready.find(m_nextPresent);
        if (ready == m_ready.end()) {
            break;
        }
        if (m_skippedDuplicates.remove(m_nextPresent)) {
            const auto consume = m_duplicateFilter.consume;
            const auto identity = m_files.at(m_nextPresent).sourceIdentity;
            const QPointer<ScreenshotFilePinBatch> guard(this);
            const bool consumed = consume && consume(identity);
            if (!guard)
                return;
            if (generation != m_generation)
                break;
            if (!consumed) {
                m_ready.remove(m_nextPresent);
                submitDecode(generation, m_nextPresent);
                break;
            }
            m_succeeded = true;
        }
        std::optional<ScreenshotClipboardContent> content = m_ready.take(m_nextPresent);
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
        if (!guard)
            return;
        if (generation != m_generation) {
            break;
        }
        if (!proceed) {
            finish(false);
            if (!guard)
                return;
            break;
        }
        m_succeeded = true;
    }
    m_dispatching = false;
    if (m_redispatch) {
        m_redispatch = false;
        dispatch(m_generation);
    }
}
