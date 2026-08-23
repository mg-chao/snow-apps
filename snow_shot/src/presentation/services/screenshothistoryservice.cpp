#include "snow_shot/presentation/screenshothistoryservice.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/storage/applicationstorage.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QUuid>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace {
bool restoreCanvasPayload(SnowCanvasRuntime& runtime, const QByteArray& payload) {
    // Earlier releases wrote full document sessions. Current entries contain
    // only elements and undo/redo history because creation styles are shared.
    return runtime.restoreDocumentSession(payload) || runtime.restoreDocumentHistory(payload);
}

} // namespace

void initializeHistoryValidationQueue(
    std::unique_ptr<ScreenshotHistoryValidationQueue>* target,
    snow_shot::storage::CaptureHistoryRepository& repository);

ScreenshotHistoryService::ScreenshotHistoryService(ScreenshotHistoryServiceContext context,
                                                   QString storageRoot, Clock clock)
    : m_context(std::move(context)),
      m_clock(clock ? std::move(clock) : []() { return QDateTime::currentDateTimeUtc(); }) {
    if (storageRoot.isEmpty()) {
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (!storage.isInitialized()) {
            static_cast<void>(storage.initialize());
        }
        m_repository = &storage.captureHistory();
        connect(&storage, &snow_shot::storage::ApplicationStorage::captureHistoryChanged, this,
                &ScreenshotHistoryService::refreshMetadata);
    } else {
        snow_shot::storage::CaptureHistoryRepositoryOptions options;
        options.writeAvailable = true;
        options.clock = m_clock;
        m_ownedRepository = snow_shot::storage::makeCaptureHistoryRepository(
            std::move(storageRoot), std::move(options));
        m_repository = m_ownedRepository.get();
    }
    m_entries = m_repository->records();
    initializeHistoryValidationQueue(&m_validationQueue, *m_repository);
}

namespace {
bool validateCanvasPayload(const QByteArray& payload) {
    if (payload.isEmpty() || payload.size() > 16 * 1024 * 1024) {
        return false;
    }
    SnowCanvasRuntime runtime(
        SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
    return restoreCanvasPayload(runtime, payload);
}

bool structurallyValidCanvasPayload(const QByteArray& payload) {
    return !payload.isEmpty() && payload.size() <= 16 * 1024 * 1024;
}

snow_shot::storage::PersistedSelection
persistedSelection(const ScreenshotSelectionParams& selection) {
    return {
        selection.selection,
        selection.radius,
        selection.shadowWidth,
        selection.shadowColor,
        selection.lockAspectRatio,
        selection.lockDragAspectRatio,
    };
}

ScreenshotSelectionParams
presentationSelection(const snow_shot::storage::PersistedSelection& selection) {
    ScreenshotSelectionParams result;
    result.selection = selection.rectangle;
    result.radius = selection.cornerRadius;
    result.shadowWidth = selection.shadowWidth;
    result.shadowColor = selection.shadowColor;
    result.lockAspectRatio = selection.lockAspectRatio;
    result.lockDragAspectRatio = selection.lockDragAspectRatio;
    return result;
}

snow_shot::storage::CaptureHistoryDraft storageDraft(const ScreenshotHistoryEntry& entry) {
    snow_shot::storage::CaptureHistoryDraft draft;
    draft.id = entry.id;
    draft.createdUtc = entry.createdUtc.toUTC();
    draft.canvasBounds = entry.recordedCanvasBounds;
    draft.selection = persistedSelection(entry.selection);
    draft.canvasHistory = entry.canvasHistory;
    draft.source = entry.source;
    for (const ScreenshotHistoryDisplay& display : entry.displays) {
        draft.displays.push_back({display.stableId, display.name, display.image});
    }
    draft.resultImage = entry.resultImage;
    return draft;
}

snow_shot::storage::CaptureHistoryRecord
placeholderRecord(const snow_shot::storage::CaptureHistoryDraft& draft) {
    snow_shot::storage::CaptureHistoryRecord record;
    record.id = draft.id;
    record.createdUtc = draft.createdUtc;
    record.canvasBounds = draft.canvasBounds;
    record.selection = draft.selection;
    record.source = draft.source;
    record.canvasBytes = draft.canvasHistory.size();
    if (draft.resultImage.has_value()) {
        record.result = snow_shot::storage::CaptureHistoryResultRecord{
            draft.resultImage->size(), 0};
    }
    for (const snow_shot::storage::CaptureHistoryDisplayDraft& display : draft.displays) {
        record.displays.push_back({display.stableId, display.name, display.image.size(), 0});
    }
    return record;
}

std::optional<ScreenshotHistoryEntry>
presentationEntry(const snow_shot::storage::CaptureHistoryRecord& record,
                  const snow_shot::storage::CaptureHistoryPayload& payload) {
    if (payload.displayImages.size() != record.displays.size() ||
        !validateCanvasPayload(payload.canvasHistory)) {
        return std::nullopt;
    }
    ScreenshotHistoryEntry entry;
    entry.id = record.id;
    entry.createdUtc = record.createdUtc;
    entry.recordedCanvasBounds = record.canvasBounds;
    entry.selection = presentationSelection(record.selection);
    entry.canvasHistory = payload.canvasHistory;
    entry.source = record.source;
    entry.persistent = true;
    for (qsizetype index = 0; index < record.displays.size(); ++index) {
        entry.displays.push_back({record.displays[index].stableId,
                                  record.displays[index].name,
                                  payload.displayImages[index]});
    }
    return entry;
}
} // namespace

class ScreenshotHistoryValidationQueue final {
  public:
    explicit ScreenshotHistoryValidationQueue(
        snow_shot::storage::CaptureHistoryRepository& repository)
        : m_repository(repository), m_thread([this]() { run(); }) {}

    ~ScreenshotHistoryValidationQueue() {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_condition.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    std::shared_future<snow_shot::storage::CaptureHistoryPublishResult>
    submit(snow_shot::storage::CaptureHistoryDraft draft) {
        auto promise =
            std::make_shared<std::promise<snow_shot::storage::CaptureHistoryPublishResult>>();
        std::shared_future<snow_shot::storage::CaptureHistoryPublishResult> result =
            promise->get_future().share();
        {
            const std::lock_guard lock(m_mutex);
            if (m_stopping) {
                promise->set_value({snow_shot::storage::StorageResult::failure(
                                        QStringLiteral("History validation is shutting down")),
                                    {}});
                return result;
            }
            if (m_jobs.size() >= kMaximumPendingJobs) {
                promise->set_value({snow_shot::storage::StorageResult::failure(
                                        QStringLiteral("History validation queue is full")),
                                    {}});
                return result;
            }
            m_jobs.push_back(Job{std::move(draft), std::move(promise)});
        }
        m_condition.notify_one();
        return result;
    }

  private:
    static constexpr std::size_t kMaximumPendingJobs = 2;

    struct Job {
        snow_shot::storage::CaptureHistoryDraft draft;
        std::shared_ptr<std::promise<snow_shot::storage::CaptureHistoryPublishResult>> promise;
    };

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait(lock, [this]() { return m_stopping || !m_jobs.empty(); });
                if (m_jobs.empty()) {
                    if (m_stopping) {
                        return;
                    }
                    continue;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }

            try {
                if (!validateCanvasPayload(job.draft.canvasHistory)) {
                    job.promise->set_value({snow_shot::storage::StorageResult::failure(
                                                QStringLiteral("Capture history is invalid")),
                                            {}});
                    continue;
                }
                std::shared_future<snow_shot::storage::CaptureHistoryPublishResult> publication =
                    m_repository.publish(std::move(job.draft));
                job.promise->set_value(
                    publication.valid()
                        ? publication.get()
                        : snow_shot::storage::CaptureHistoryPublishResult{
                              snow_shot::storage::StorageResult::failure(
                                  QStringLiteral("History publication returned no result")),
                              {}});
            } catch (const std::exception& error) {
                job.promise->set_value({snow_shot::storage::StorageResult::failure(
                                            QString::fromUtf8(error.what())),
                                        {}});
            } catch (...) {
                job.promise->set_value({snow_shot::storage::StorageResult::failure(
                                            QStringLiteral("History validation failed")),
                                        {}});
            }
        }
    }

    snow_shot::storage::CaptureHistoryRepository& m_repository;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<Job> m_jobs;
    bool m_stopping = false;
    std::thread m_thread;
};

void initializeHistoryValidationQueue(
    std::unique_ptr<ScreenshotHistoryValidationQueue>* target,
    snow_shot::storage::CaptureHistoryRepository& repository) {
    if (target != nullptr) {
        *target = std::make_unique<ScreenshotHistoryValidationQueue>(repository);
    }
}

ScreenshotHistoryService::ScreenshotHistoryService(
    ScreenshotHistoryServiceContext context,
    snow_shot::storage::CaptureHistoryRepository& repository, Clock clock)
    : m_context(std::move(context)), m_repository(&repository),
      m_clock(clock ? std::move(clock) : []() { return QDateTime::currentDateTimeUtc(); }) {
    m_entries = m_repository->records();
    initializeHistoryValidationQueue(&m_validationQueue, *m_repository);
}

ScreenshotHistoryService::~ScreenshotHistoryService() {
    resetCaptureNavigation();
    drainPendingLoads();
    drainPendingWrites();
}

QRect ScreenshotHistoryService::currentCanvasBounds() const {
    QRect bounds;
    m_context.displays.forEachActiveDisplay(
        [&bounds](qsizetype, const CapturedDisplayModel& display) {
            bounds =
                bounds.united(ScreenshotGeometryMapper::displayCanvasRect(display).toAlignedRect());
        });
    return bounds;
}

std::optional<ScreenshotHistoryEntry>
ScreenshotHistoryService::snapshotCurrent(bool persistent) const {
    if (persistent && (m_repository == nullptr || !m_repository->policy().enabled)) {
        return std::nullopt;
    }
    const QRect bounds = currentCanvasBounds();
    if (bounds.isEmpty()) {
        return std::nullopt;
    }
    ScreenshotHistoryEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.createdUtc = m_clock().toUTC();
    entry.recordedCanvasBounds = bounds;
    entry.selection = m_context.selection.params(bounds);
    entry.canvasHistory = m_context.runtime.serializeDocumentHistory();
    entry.intelligentSelectionMode = m_context.interaction.intelligentSelecting();
    entry.persistent = persistent;
    if (!persistent) {
        entry.liveIntelligentSelection = m_context.intelligentSelection;
    }
    if (!structurallyValidCanvasPayload(entry.canvasHistory) ||
        entry.selection.selection.isEmpty()) {
        return std::nullopt;
    }
    m_context.displays.forEachActiveDisplay(
        [&entry](qsizetype, const CapturedDisplayModel& display) {
            if (display.image.isNull()) {
                return;
            }
            entry.displays.push_back(
                ScreenshotHistoryDisplay{display.stableId, display.name, display.image});
        });
    if (entry.displays.isEmpty()) {
        return std::nullopt;
    }
    return entry;
}

void ScreenshotHistoryService::commit(ScreenshotHistoryEntry entry) {
    entry.persistent = true;
    scheduleWrite(std::move(entry));
}

bool ScreenshotHistoryService::navigateToRecord(const QString& recordId) {
    if (recordId.isEmpty() || m_navigationInProgress) {
        return false;
    }
    reapCompletedWrites();
    const auto target = std::find_if(
        m_entries.cbegin(), m_entries.cend(),
        [&recordId](const snow_shot::storage::CaptureHistoryRecord& record) {
            return record.id == recordId;
        });
    if (target == m_entries.cend() || !ensureLiveEndpoint()) {
        return false;
    }
    const qsizetype recordIndex = std::distance(m_entries.cbegin(), target);
    return navigateTo(static_cast<int>(recordIndex) + 1);
}

bool ScreenshotHistoryService::ensureLiveEndpoint() {
    if (m_liveEndpoint.has_value() && m_navigationIndex != 0) {
        return true;
    }
    std::optional<ScreenshotHistoryEntry> liveEndpoint = snapshotCurrent(false);
    if (!liveEndpoint.has_value()) {
        return false;
    }
    if (m_context.interaction.manualSelecting()) {
        m_context.interaction.confirmSelection();
        liveEndpoint->intelligentSelectionMode = false;
    }
    m_liveEndpoint = std::move(liveEndpoint);
    m_navigationIndex = 0;
    return true;
}

bool ScreenshotHistoryService::navigatePrevious() {
    if (m_navigationInProgress) {
        return false;
    }
    reapCompletedWrites();
    if (m_entries.isEmpty()) {
        return false;
    }
    if (!ensureLiveEndpoint()) {
        return false;
    }
    return navigateTo(m_navigationIndex + 1);
}

bool ScreenshotHistoryService::navigateNext() {
    if (m_navigationInProgress) {
        return false;
    }
    reapCompletedWrites();
    if (!ensureLiveEndpoint() || m_navigationIndex <= 0) {
        return false;
    }
    return navigateTo(m_navigationIndex - 1);
}

bool ScreenshotHistoryService::returnToCurrentScreenshot() {
    if (m_navigationInProgress || m_navigationIndex <= 0 || !m_liveEndpoint.has_value()) {
        return false;
    }
    reapCompletedWrites();
    return navigateTo(0);
}

bool ScreenshotHistoryService::navigateTo(int index) {
    if (index < 0 || index > m_entries.size()) {
        return false;
    }
    if (index == 0) {
        if (!applyEntry(*m_liveEndpoint)) {
            return false;
        }
        m_navigationIndex = 0;
        return true;
    }

    snow_shot::storage::CaptureHistoryRecord metadata = m_entries[index - 1];
    const QString entryId = metadata.id;
    const std::shared_future<snow_shot::storage::CaptureHistoryPublishResult> pendingWrite =
        pendingWriteFor(entryId);
    const quint64 generation = ++m_navigationGeneration;
    reapCompletedLoads();
    m_navigationInProgress = true;
    m_context.loadingStateChanged(true);
    try {
        m_pendingLoads.push_back(std::async(std::launch::async, [this, generation, index, entryId,
                                                                 metadata = std::move(metadata),
                                                                 pendingWrite]() mutable {
            std::optional<ScreenshotHistoryEntry> loadedEntry;
            try {
                if (pendingWrite.valid()) {
                    const auto published = pendingWrite.get();
                    if (published.storage.success) {
                        metadata = published.record;
                    } else {
                        metadata.id.clear();
                    }
                }
                if (!metadata.id.isEmpty()) {
                    const auto payload = m_repository->load(metadata);
                    if (payload.has_value()) {
                        loadedEntry = presentationEntry(metadata, *payload);
                    }
                }
            } catch (...) {
                loadedEntry.reset();
            }

            QMetaObject::invokeMethod(
                this,
                [this, generation, index, entryId, loadedEntry = std::move(loadedEntry)]() mutable {
                    finishPersistentNavigation(generation, index, entryId, std::move(loadedEntry));
                },
                Qt::QueuedConnection);
        }));
    } catch (...) {
        m_navigationInProgress = false;
        m_context.loadingStateChanged(false);
        return false;
    }
    return true;
}

void ScreenshotHistoryService::finishPersistentNavigation(
    quint64 generation, int index, const QString& entryId,
    std::optional<ScreenshotHistoryEntry> entry) {
    if (generation != m_navigationGeneration || !m_navigationInProgress) {
        reapCompletedLoads();
        return;
    }

    m_navigationInProgress = false;
    const bool targetStillExists =
        index > 0 && index <= m_entries.size() && m_entries[index - 1].id == entryId;
    if (!entry.has_value()) {
        if (targetStillExists) {
            static_cast<void>(m_repository->remove(m_entries[index - 1].id));
            m_entries.removeAt(index - 1);
            if (index < m_navigationIndex) {
                --m_navigationIndex;
            }
        }
    } else if (targetStillExists && applyEntry(*entry)) {
        m_navigationIndex = index;
    }

    m_context.loadingStateChanged(false);
    reapCompletedLoads();
}

bool ScreenshotHistoryService::applyEntry(const ScreenshotHistoryEntry& entry) {
    const QRect bounds = currentCanvasBounds();
    ScreenshotSelectionModel restoredSelection = m_context.selection;
    if (!restoredSelection.applyParams(entry.selection, bounds)) {
        return false;
    }

    SnowCanvasRuntime replacement(
        SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
    if (!restoreCanvasPayload(replacement, entry.canvasHistory)) {
        return false;
    }
    const QByteArray documentHistory = replacement.serializeDocumentHistory();
    if (documentHistory.isEmpty() ||
        !m_context.runtime.restoreDocumentHistoryPreservingEditorStyles(documentHistory)) {
        return false;
    }

    QVector<qsizetype> current;
    m_context.displays.forEachActiveDisplay(
        [&current](qsizetype index, const CapturedDisplayModel&) { current.push_back(index); });
    QVector<int> matchedSaved(current.size(), -1);
    QVector<bool> savedUsed(entry.displays.size(), false);
    auto matchPass = [&](auto matches) {
        for (qsizetype currentOrder = 0; currentOrder < current.size(); ++currentOrder) {
            if (matchedSaved[currentOrder] >= 0) {
                continue;
            }
            const CapturedDisplayModel& display =
                m_context.displays.displayAt(current[currentOrder]);
            for (qsizetype savedIndex = 0; savedIndex < entry.displays.size(); ++savedIndex) {
                if (!savedUsed[savedIndex] && matches(display, entry.displays[savedIndex])) {
                    matchedSaved[currentOrder] = static_cast<int>(savedIndex);
                    savedUsed[savedIndex] = true;
                    break;
                }
            }
        }
    };
    matchPass([](const CapturedDisplayModel& currentDisplay,
                 const ScreenshotHistoryDisplay& savedDisplay) {
        return !currentDisplay.stableId.isEmpty() &&
               currentDisplay.stableId == savedDisplay.stableId;
    });
    matchPass([](const CapturedDisplayModel& currentDisplay,
                 const ScreenshotHistoryDisplay& savedDisplay) {
        return !currentDisplay.name.isEmpty() && currentDisplay.name == savedDisplay.name;
    });
    matchPass([](const CapturedDisplayModel&, const ScreenshotHistoryDisplay&) { return true; });

    for (qsizetype currentOrder = 0; currentOrder < current.size(); ++currentOrder) {
        CapturedDisplayModel& display = m_context.displays.displayAt(current[currentOrder]);
        const int savedIndex = matchedSaved[currentOrder];
        if (savedIndex < 0) {
            display.image = QImage();
            display.imageSourceCanvasRect = QRect();
            continue;
        }
        const QImage& image = entry.displays[savedIndex].image;
        display.image = image;
        display.imageSourceCanvasRect = QRect(
            ScreenshotGeometryMapper::displayCanvasRect(display).topLeft().toPoint(), image.size());
    }
    m_context.selection = restoredSelection;
    m_context.interaction.cancelDrag();
    bool requestIntelligentSelection = false;
    if (entry.persistent) {
        m_context.intelligentSelection.clearTransientState();
        m_context.interaction.returnToSelectionMode(false);
    } else if (entry.intelligentSelectionMode) {
        if (entry.liveIntelligentSelection.has_value()) {
            m_context.intelligentSelection = *entry.liveIntelligentSelection;
        }
        m_context.interaction.returnToSelectionMode(true);
        requestIntelligentSelection = true;
    } else {
        m_context.intelligentSelection.clearTransientState();
        m_context.interaction.confirmSelection();
    }
    if (m_context.presentationChanged) {
        m_context.presentationChanged();
    }
    if (requestIntelligentSelection && m_context.intelligentSelectionRequested) {
        m_context.intelligentSelectionRequested();
    }
    return true;
}

void ScreenshotHistoryService::resetCaptureNavigation() {
    ++m_navigationGeneration;
    if (m_navigationInProgress) {
        m_navigationInProgress = false;
        m_context.loadingStateChanged(false);
    }
    m_liveEndpoint.reset();
    m_navigationIndex = -1;
}

bool ScreenshotHistoryService::navigationInProgress() const {
    return m_navigationInProgress;
}

void ScreenshotHistoryService::scheduleWrite(ScreenshotHistoryEntry entry) {
    reapCompletedWrites();
    if (m_validationQueue == nullptr ||
        !structurallyValidCanvasPayload(entry.canvasHistory)) {
        return;
    }
    snow_shot::storage::CaptureHistoryDraft draft = storageDraft(entry);
    const QString id = draft.id;
    m_entries.prepend(placeholderRecord(draft));
    m_pendingWrites.push_back(
        PendingWrite{id, m_validationQueue->submit(std::move(draft))});
}

void ScreenshotHistoryService::reapCompletedWrites() {
    const auto completed = std::remove_if(
        m_pendingWrites.begin(), m_pendingWrites.end(), [this](PendingWrite& write) {
            if (write.result.valid() &&
                write.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return false;
            }
            const auto result = write.result.valid()
                                    ? write.result.get()
                                    : snow_shot::storage::CaptureHistoryPublishResult{};
            const auto entry = std::find_if(m_entries.begin(), m_entries.end(),
                                            [&write](const auto& candidate) {
                                                return candidate.id == write.entryId;
                                            });
            if (entry != m_entries.end()) {
                if (result.storage.success) {
                    *entry = result.record;
                } else {
                    m_entries.erase(entry);
                }
            }
            return true;
        });
    m_pendingWrites.erase(completed, m_pendingWrites.end());
}

void ScreenshotHistoryService::drainPendingWrites() {
    for (PendingWrite& write : m_pendingWrites) {
        if (write.result.valid()) {
            write.result.wait();
        }
    }
    reapCompletedWrites();
    m_repository->drain();
    reapCompletedWrites();
    refreshMetadata();
}

void ScreenshotHistoryService::refreshMetadata() {
    reapCompletedWrites();
    QVector<snow_shot::storage::CaptureHistoryRecord> refreshed = m_repository->records();
    for (const PendingWrite& pending : m_pendingWrites) {
        const auto existing = std::find_if(
            m_entries.cbegin(), m_entries.cend(), [&pending](const auto& candidate) {
                return candidate.id == pending.entryId;
            });
        const auto alreadyPersisted = std::find_if(
            refreshed.cbegin(), refreshed.cend(), [&pending](const auto& candidate) {
                return candidate.id == pending.entryId;
            });
        if (existing != m_entries.cend() && alreadyPersisted == refreshed.cend()) {
            refreshed.push_back(*existing);
        }
    }
    std::sort(refreshed.begin(), refreshed.end(), [](const auto& first, const auto& second) {
        return first.createdUtc > second.createdUtc;
    });
    m_entries = std::move(refreshed);
    if (m_navigationIndex > m_entries.size()) {
        m_navigationIndex = static_cast<int>(m_entries.size()) + 1;
    }
}

std::shared_future<snow_shot::storage::CaptureHistoryPublishResult>
ScreenshotHistoryService::pendingWriteFor(const QString& entryId) const {
    for (const PendingWrite& write : m_pendingWrites) {
        if (write.entryId == entryId) {
            return write.result;
        }
    }
    return {};
}

void ScreenshotHistoryService::reapCompletedLoads() {
    const auto completed =
        std::remove_if(m_pendingLoads.begin(), m_pendingLoads.end(), [](std::future<void>& load) {
            return !load.valid() ||
                   load.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
    m_pendingLoads.erase(completed, m_pendingLoads.end());
}

void ScreenshotHistoryService::drainPendingLoads() {
    for (std::future<void>& load : m_pendingLoads) {
        if (load.valid()) {
            load.wait();
        }
    }
    m_pendingLoads.clear();
}
