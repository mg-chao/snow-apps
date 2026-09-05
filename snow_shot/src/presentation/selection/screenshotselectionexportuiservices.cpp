#include "snow_shot/presentation/screenshotselectionexportuiservices.h"

#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "../pinned/screenshotpinnedrestoregeometry.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QDataStream>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <optional>

namespace {
ScreenshotRecognitionResults
recognitionResultsForPinnedImage(const ScreenshotRecognitionResults& sourceResults,
                                 const QRect& sourceSelection,
                                 const ScreenshotResultStyle& resultStyle, const QSize& imageSize) {
    ScreenshotRecognitionResults results = sourceResults;
    if (!results.text.has_value() || !results.text->error.isEmpty() ||
        results.text->presentation == nullptr || sourceSelection.isEmpty() || imageSize.isEmpty()) {
        return results;
    }

    const ScreenshotResultLayout layout =
        ScreenshotResultCompositor::layoutForContent(sourceSelection.size(), resultStyle);
    if (!layout.isValid() || layout.outputRect.size() != imageSize) {
        return results;
    }

    // Cached OCR quads are in the original screenshot canvas. The pinned image is a
    // separately rendered result whose origin is local to the new canvas and may include
    // effect padding for the result shadow.
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->lines = results.text->presentation->lines;
    const QPointF sourceOrigin = sourceSelection.topLeft();
    const QPointF destinationOrigin(layout.contentRect.topLeft());
    presentation->selection = layout.contentRect;
    for (ScreenshotOcrLine& line : presentation->lines) {
        for (QPointF& point : line.quad) {
            point = point - sourceOrigin + destinationOrigin;
        }
    }
    presentation->prepareForRendering();
    results.text->presentation = std::move(presentation);
    return results;
}

void applyPinRuntimeSettings(ScreenshotPinnedWindow::Config* config) {
    if (config == nullptr) {
        return;
    }
    const snow_shot::storage::PinToScreenSettings settings;
    config->mouseWheelZoomMode = settings.mouseWheelZoomMode();
    config->automaticTextRecognition =
        config->formattedTextDocument == nullptr && settings.automaticTextRecognition();
}

void applyPersistence(ScreenshotPinnedWindow::Config* config, const QString& id = {},
                      bool sourceManaged = false) {
    if (config == nullptr) {
        return;
    }
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.isInitialized()) {
        return;
    }
    config->persistenceId = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
    config->persistenceWriter =
        [sourceManaged](const snow_shot::storage::PinnedWindowRecord& record) {
            auto& storage = snow_shot::storage::ApplicationStorage::instance();
            if (!storage.configurationDirectory().isEmpty()) {
                static_cast<void>(sourceManaged ? storage.pinnedWindows().updateState(record)
                                                : storage.pinnedWindows().upsert(record));
            }
        };
    config->persistenceRemover = [](const QString& recordId) {
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (!storage.configurationDirectory().isEmpty()) {
            static_cast<void>(storage.pinnedWindows().remove(recordId));
        }
    };
}

ScreenshotResultStyle decodeResultStyle(const QByteArray& bytes) {
    ScreenshotResultStyle style;
    if (!bytes.isEmpty()) {
        QDataStream stream(bytes);
        stream >> style.cornerRadius >> style.shadowWidth >> style.shadowColor;
    }
    return style;
}

QRect availablePhysicalRect(QScreen* screen) {
    if (screen == nullptr) {
        return {};
    }
    const QRect logicalBounds = screen->geometry();
    const QRect logicalAvailable = screen->availableGeometry();
    const QRect physicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const qreal scale = screen->devicePixelRatio() > 0.0 ? screen->devicePixelRatio() : 1.0;
    return QRect(
        physicalBounds.left() + qRound((logicalAvailable.left() - logicalBounds.left()) * scale),
        physicalBounds.top() + qRound((logicalAvailable.top() - logicalBounds.top()) * scale),
        std::max(1, qRound(logicalAvailable.width() * scale)),
        std::max(1, qRound(logicalAvailable.height() * scale)));
}

screenshot_pinned_restore_geometry::ScreenGeometry restoreScreenGeometry(QScreen* screen) {
    screenshot_pinned_restore_geometry::ScreenGeometry geometry;
    geometry.physicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    geometry.availableBounds = availablePhysicalRect(screen);
    return geometry;
}

// Re-bases the persisted physical geometries of a record onto `target`
// (never null) in one step. The saved physical pixel sizes — and with them
// the scale value — are left untouched; monitor DPI plays no role.
screenshot_pinned_restore_geometry::RestoredState
reconcileRestoreState(const snow_shot::storage::PinnedWindowRecord& record, QScreen& target) {
    screenshot_pinned_restore_geometry::SavedState saved;
    saved.nativeGeometry = record.nativeGeometry;
    saved.preThumbnailNativeGeometry = record.preThumbnailNativeGeometry;
    saved.screenPhysicalBounds = record.screenPhysicalGeometry;
    QList<screenshot_pinned_restore_geometry::ScreenGeometry> screens;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != nullptr) {
            screens.push_back(restoreScreenGeometry(screen));
        }
    }
    return screenshot_pinned_restore_geometry::reconcileSavedState(
        saved, restoreScreenGeometry(&target), screens);
}

QScreen* restoreScreen(const snow_shot::storage::PinnedWindowRecord& record) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* screen : screens) {
        if (screen != nullptr && !record.screenSerial.isEmpty() &&
            screen->serialNumber() == record.screenSerial) {
            return screen;
        }
    }
    for (QScreen* screen : screens) {
        if (screen != nullptr && !record.screenName.isEmpty() &&
            screen->name() == record.screenName) {
            return screen;
        }
    }
    if (screens.isEmpty()) {
        return nullptr;
    }
    const QPoint savedCenter = record.nativeGeometry.center();
    QScreen* nearest = screens.front();
    qint64 nearestDistance = std::numeric_limits<qint64>::max();
    for (QScreen* screen : screens) {
        if (screen == nullptr) {
            continue;
        }
        const QRect bounds = availablePhysicalRect(screen);
        const QPoint clamped(qBound(bounds.left(), savedCenter.x(), bounds.right()),
                             qBound(bounds.top(), savedCenter.y(), bounds.bottom()));
        const qint64 dx = static_cast<qint64>(savedCenter.x()) - clamped.x();
        const qint64 dy = static_cast<qint64>(savedCenter.y()) - clamped.y();
        const qint64 distance = dx * dx + dy * dy;
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = screen;
        }
    }
    return nearest;
}
} // namespace

class ScreenshotPinnedWindowPool final : public QObject {
  public:
    explicit ScreenshotPinnedWindowPool(QObject* parent = nullptr) : QObject(parent) {}

    ~ScreenshotPinnedWindowPool() override {
        if (m_spare != nullptr) {
            delete m_spare;
            m_spare = nullptr;
        }
    }

    ScreenshotPinnedWindow* acquire(ScreenshotPinnedWindow::RuntimeMode mode, QScreen* screen) {
        if (screen != nullptr) {
            m_targetScreen = screen;
        }

        ScreenshotPinnedWindow* window = m_spare;
        bool usedSpare = false;
        if (window != nullptr) {
            m_spare = nullptr;
            if (window->prewarm(resolvedTargetScreen())) {
                usedSpare = true;
            } else {
                window->deleteLater();
                window = nullptr;
            }
        }
        if (window == nullptr) {
            const QElapsedTimer timer = [&]() {
                QElapsedTimer value;
                value.start();
                return value;
            }();
            window = new ScreenshotPinnedWindow(mode);
            if (window != nullptr && window->prewarm(resolvedTargetScreen())) {
                SNOW_SHOT_PIN_PERF_COUNTER("shell.construction_ns", timer.nsecsElapsed());
            } else {
                if (window != nullptr) {
                    window->deleteLater();
                    window = nullptr;
                }
            }
        }

        SNOW_SHOT_PIN_PERF_COUNTER(usedSpare ? "shell.hit" : "shell.miss", 1);
        if (window == nullptr) {
            schedulePrewarm(screen);
        }
        return window;
    }

    void prewarm(QScreen* screen) {
        if (screen != nullptr) {
            m_targetScreen = screen;
        }
        QScreen* targetScreen = resolvedTargetScreen();
        if (targetScreen == nullptr) {
            return;
        }
        if (m_spare != nullptr) {
            if (m_spare->prewarm(targetScreen)) {
                return;
            }
            m_spare->deleteLater();
            m_spare = nullptr;
        }

        auto* spare = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
        if (spare == nullptr || !spare->prewarm(targetScreen)) {
            if (spare != nullptr) {
                spare->deleteLater();
            }
            return;
        }
        m_spare = spare;
    }

    void schedulePrewarm(QScreen* screen) {
        if (m_spare != nullptr) {
            return;
        }
        if (screen != nullptr) {
            m_targetScreen = screen;
        }
        if (m_prewarmScheduled) {
            return;
        }
        m_prewarmScheduled = true;
        QTimer::singleShot(0, this, [this]() {
            m_prewarmScheduled = false;
            if (m_spare == nullptr) {
                prewarm(m_targetScreen.data());
            }
        });
    }

  private:
    QScreen* resolvedTargetScreen() const {
        if (m_targetScreen != nullptr) {
            return m_targetScreen.data();
        }
        if (QScreen* cursorScreen = QGuiApplication::screenAt(QCursor::pos())) {
            return cursorScreen;
        }
        return QGuiApplication::primaryScreen();
    }

    QPointer<ScreenshotPinnedWindow> m_spare;
    QPointer<QScreen> m_targetScreen;
    bool m_prewarmScheduled = false;
};

class ScreenshotPendingPinCoordinator final : public QObject {
  public:
    explicit ScreenshotPendingPinCoordinator(QObject* parent = nullptr) : QObject(parent) {}
    ~ScreenshotPendingPinCoordinator() override {
        const QStringList persistenceIds = m_transactions.keys();
        for (const QString& persistenceId : persistenceIds) {
            finish(persistenceId);
        }
    }

    void reserve(ScreenshotPinnedWindow* window, const QString& persistenceId,
                 const QString& groupId,
                 snow_shot::presentation::PinnedWindowGroupManager* groupManager,
                 std::shared_ptr<ScreenshotExportArtifact> artifact = {}) {
        if (window == nullptr || persistenceId.isEmpty()) {
            return;
        }
        if (m_transactions.contains(persistenceId)) {
            finish(persistenceId);
        }
        Transaction transaction;
        transaction.window = window;
        transaction.snapshot.id = persistenceId;
        transaction.snapshot.groupId = groupId;
        transaction.groupManager = groupManager;
        transaction.artifact = std::move(artifact);
        m_transactions.insert(persistenceId, transaction);
        if (groupManager != nullptr) {
            groupManager->registerPendingPin(persistenceId, groupId);
        }
        QObject::connect(
            window, &ScreenshotPinnedWindow::closingForPersistence, this,
            [this, persistenceId](const snow_shot::storage::PinnedWindowRecord& snapshot,
                                  bool removalRequested) {
                auto transaction = m_transactions.find(persistenceId);
                if (transaction == m_transactions.end()) {
                    return;
                }
                transaction->snapshot = snapshot;
                if (removalRequested) {
                    transaction->removed = true;
                    removePersistedRecord(persistenceId);
                    finish(persistenceId);
                }
            });
        QObject::connect(window, &QObject::destroyed, this, [this, persistenceId]() {
            auto transaction = m_transactions.find(persistenceId);
            if (transaction != m_transactions.end()) {
                transaction->window = nullptr;
            }
        });
    }

    void updateSnapshot(const QString& persistenceId,
                        const snow_shot::storage::PinnedWindowRecord& snapshot) {
        auto transaction = m_transactions.find(persistenceId);
        if (transaction != m_transactions.end()) {
            transaction->snapshot = snapshot;
        }
    }

    void cancel(const QString& persistenceId) {
        finish(persistenceId);
    }

    ScreenshotImageLoader wrapLoader(const QString& persistenceId, ScreenshotImageLoader loader) {
        const QPointer<ScreenshotPendingPinCoordinator> receiver(this);
        return [receiver, persistenceId, loader = std::move(loader)](
                   QObject*, ScreenshotImageLoadCallback callback) mutable {
            if (receiver.isNull() || !loader) {
                callback({});
                return;
            }
            loader(receiver,
                   [receiver, persistenceId, callback = std::move(callback)](QImage image) mutable {
                       if (receiver.isNull()) {
                           callback({});
                           return;
                       }
                       receiver->completeLoadedImage(persistenceId, image);
                       callback(std::move(image));
                   });
        };
    }

    void completeSelection(const QString& persistenceId, bool success, QImage image) {
        auto transaction = m_transactions.find(persistenceId);
        if (transaction == m_transactions.end()) {
            return;
        }
        if (transaction->removed) {
            finish(persistenceId);
            return;
        }
        const QPointer<ScreenshotPinnedWindow> window = transaction->window;
        if (success && !image.isNull()) {
            transaction->image = image;
            if (transaction->artifact == nullptr) {
                transaction->artifact = std::make_shared<ScreenshotExportArtifact>(
                    ScreenshotExportSource::fromImage(image));
            }
            if (!window.isNull()) {
                static_cast<void>(window->publishMaterializedImage(std::move(image)));
            }
        } else if (!window.isNull()) {
            static_cast<void>(window->publishMaterializedImage({}));
        }
    }

    void completeFirstFrame(const QString& persistenceId, bool success) {
        auto transaction = m_transactions.find(persistenceId);
        if (transaction == m_transactions.end()) {
            return;
        }
        if (!success || transaction->removed || transaction->window.isNull() ||
            transaction->artifact == nullptr) {
            finish(persistenceId);
            return;
        }
        transaction->snapshot = transaction->window->persistenceSnapshot();
        if (transaction->snapshot.sourceKind !=
            snow_shot::storage::PinnedWindowSourceKind::ImageData) {
            persistNonImageSource(*transaction);
            finish(persistenceId);
            return;
        }
        const QPointer<ScreenshotPendingPinCoordinator> receiver(this);
        const std::shared_ptr<ScreenshotExportArtifact> artifact = transaction->artifact;
        if (!artifact->requestCanonicalPng(
                this, [receiver, persistenceId](ScreenshotExportEncodingResult result) mutable {
                    if (!receiver.isNull()) {
                        receiver->completePreparedSource(persistenceId, std::move(result));
                    }
                })) {
            qWarning("Pinned source PNG encoding could not be started");
            finish(persistenceId);
        }
    }

  private:
    struct Transaction final {
        QPointer<ScreenshotPinnedWindow> window;
        snow_shot::storage::PinnedWindowRecord snapshot;
        QPointer<snow_shot::presentation::PinnedWindowGroupManager> groupManager;
        std::shared_ptr<ScreenshotExportArtifact> artifact;
        QImage image;
        bool removed = false;
    };

    static void removePersistedRecord(const QString& persistenceId) {
        if (persistenceId.isEmpty()) {
            return;
        }
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            static_cast<void>(storage.pinnedWindows().remove(persistenceId));
        }
    }

    static void persistNonImageSource(Transaction& transaction) {
        if (!transaction.window.isNull()) {
            transaction.snapshot = transaction.window->persistenceSnapshot();
        }
        if (transaction.snapshot.id.isEmpty()) {
            return;
        }
        transaction.snapshot.updatedUtc = QDateTime::currentDateTimeUtc();
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            const auto persisted = storage.pinnedWindows().create(transaction.snapshot);
            if (!persisted.success) {
                qWarning("Pinned source persistence failed: %s", qPrintable(persisted.error));
            }
        }
    }

    void completePreparedSource(const QString& persistenceId,
                                ScreenshotExportEncodingResult result) {
        auto transaction = m_transactions.find(persistenceId);
        if (transaction == m_transactions.end()) {
            return;
        }
        if (!transaction->removed && result.succeeded() && !transaction->window.isNull()) {
            transaction->snapshot = transaction->window->persistenceSnapshot();
            if (transaction->snapshot.image.isNull() && !transaction->image.isNull()) {
                transaction->snapshot.image = transaction->image;
            }
            auto& storage = snow_shot::storage::ApplicationStorage::instance();
            if (storage.isInitialized()) {
                const auto persisted =
                    storage.pinnedWindows().create(transaction->snapshot, std::move(result.image));
                if (!persisted.success) {
                    qWarning("Pinned source persistence failed: %s", qPrintable(persisted.error));
                }
            }
        } else if (!result.succeeded()) {
            qWarning("Pinned source PNG encoding failed: %s", qPrintable(result.error));
        }
        finish(persistenceId);
    }

    void completeLoadedImage(const QString& persistenceId, const QImage& image) {
        auto transaction = m_transactions.find(persistenceId);
        if (transaction == m_transactions.end()) {
            return;
        }
        if (!transaction->removed && !image.isNull()) {
            transaction->image = image;
        }
    }

    void finish(const QString& persistenceId) {
        auto transaction = m_transactions.find(persistenceId);
        if (transaction == m_transactions.end()) {
            return;
        }
        if (!transaction->groupManager.isNull()) {
            transaction->groupManager->completePendingPin(persistenceId);
        }
        m_transactions.erase(transaction);
    }

    QHash<QString, Transaction> m_transactions;
};

namespace {
bool presentPinnedWindowAndSynchronize(ScreenshotPinnedWindowPool* pool,
                                       ScreenshotPinnedWindow* window,
                                       const ScreenshotPinnedWindow::Config& config,
                                       const std::function<void()>& showMainWindowRequested,
                                       std::function<void(bool, QImage)> completion = {},
                                       bool pending = false) {
    if (window == nullptr) {
        return false;
    }
    QObject::disconnect(window, &ScreenshotPinnedWindow::showMainWindowRequested, window, nullptr);
    if (showMainWindowRequested) {
        QObject::connect(window, &ScreenshotPinnedWindow::showMainWindowRequested, window,
                         showMainWindowRequested);
    }
    SNOW_SHOT_PIN_PERF_MILESTONE("ui.pinned_window_constructed");
    const QPointer<ScreenshotPinnedWindowPool> guardedPool(pool);
    const QPointer<QScreen> guardedScreen(config.screen);
    auto synchronizedCompletion = [guardedPool, guardedScreen, completion = std::move(completion)](
                                      bool succeeded, QImage image) mutable {
        if (completion) {
            completion(succeeded, std::move(image));
        }
        if (guardedPool != nullptr) {
            guardedPool->schedulePrewarm(guardedScreen.data());
        }
    };
    const bool presented = pending
                               ? window->presentPending(config, std::move(synchronizedCompletion))
                               : window->present(config, std::move(synchronizedCompletion));
    if (!presented) {
        window->deleteLater();
        if (guardedPool != nullptr) {
            guardedPool->schedulePrewarm(guardedScreen.data());
        }
        return false;
    }
    SNOW_SHOT_PIN_PERF_COUNTER("window.visible", window->isVisible() ? 1 : 0);
    SNOW_SHOT_PIN_PERF_COUNTER("window.geometry_valid",
                               window->currentNativeGeometry() == config.nativeGeometry ? 1 : 0);
    SNOW_SHOT_PIN_PERF_MILESTONE("window.present_returned");
    return true;
}
} // namespace

ScreenshotSelectionExportUiServices::ScreenshotSelectionExportUiServices(
    ScreenshotOcrRecognitionPort* recognition, ScreenshotQrRecognitionPort* qrRecognition,
    SnowShotApiClient* tableRecognition, std::function<void()> showMainWindowRequested,
    std::function<ScreenshotPinnedRecognitionProviders()> recognitionProvider,
    snow_shot::presentation::PinnedWindowGroupManager* groupManager)
    : m_recognition(recognition), m_qrRecognition(qrRecognition),
      m_tableRecognition(tableRecognition),
      m_showMainWindowRequested(std::move(showMainWindowRequested)),
      m_recognitionProvider(std::move(recognitionProvider)), m_groupManager(groupManager),
      m_windowPool(std::make_unique<ScreenshotPinnedWindowPool>()),
      m_pendingPinCoordinator(std::make_unique<ScreenshotPendingPinCoordinator>()) {}

ScreenshotSelectionExportUiServices::~ScreenshotSelectionExportUiServices() {
    cancelClipboardPublication();
}

bool ScreenshotSelectionExportUiServices::publishClipboard(QObject* receiver,
                                                           ScreenshotClipboardPayload payload,
                                                           ClipboardCompletion completion) {
    return publishClipboard(receiver, std::move(payload), std::move(completion),
                            ScreenshotClipboardService::reservePublication());
}

bool ScreenshotSelectionExportUiServices::publishClipboard(QObject* receiver,
                                                           ScreenshotClipboardPayload payload,
                                                           ClipboardCompletion completion,
                                                           quint64 publicationId) {
    if (publicationId == 0) {
        publicationId = ScreenshotClipboardService::reservePublication();
    }
    auto completionEnabled = std::make_shared<std::atomic_bool>(true);
    m_clipboardCompletionEnabled.push_back(completionEnabled);
    auto commit = ScreenshotClipboardService::commit(
        QApplication::clipboard(), receiver, std::move(payload), publicationId,
        [completionEnabled,
         completion = std::move(completion)](ScreenshotClipboardCommitResult result) mutable {
            if (completionEnabled->exchange(false, std::memory_order_acq_rel)) {
                completion(result.succeeded());
            }
        });
    if (commit.isValid()) {
        m_clipboardCommits.push_back(commit);
    }
    return commit.isValid();
}

void ScreenshotSelectionExportUiServices::cancelClipboardPublication() {
    for (const auto& completionEnabled : m_clipboardCompletionEnabled) {
        if (completionEnabled != nullptr) {
            completionEnabled->store(false, std::memory_order_release);
        }
    }
    for (const auto& commit : m_clipboardCommits) {
        commit.cancel();
    }
    m_clipboardCompletionEnabled.clear();
    m_clipboardCommits.clear();
}

void ScreenshotSelectionExportUiServices::prewarmPinnedWindow(QScreen* screen) {
    if (m_windowPool != nullptr) {
        m_windowPool->prewarm(screen);
    }
}

bool ScreenshotSelectionExportUiServices::presentPinnedSelection(
    const ScreenshotPinnedSelectionRequest& request, ScreenshotPinnedSelectionResultHandle result,
    PinnedCompletion completion) {
    SNOW_SHOT_PIN_PERF_SCOPE("ui.present_pinned_selection");
    if (!request.isPrepared() || !result.isValid()) {
        return false;
    }

    const ScreenshotPinnedSelectionResultHandle cancellation = result;
    auto artifact =
        std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImageLoader(
            [result = std::move(result)](QObject* receiver,
                                         std::function<void(QImage)> callback) mutable {
                return result.subscribe(
                    receiver, [callback = std::move(callback)](bool success, QImage image) mutable {
                        callback(success ? std::move(image) : QImage{});
                    });
            }));
    const bool presented =
        presentPinnedArtifact(request, std::move(artifact), std::move(completion));
    if (!presented) {
        cancellation.cancel();
    }
    return presented;
}

bool ScreenshotSelectionExportUiServices::presentPinnedArtifact(
    const ScreenshotPinnedSelectionRequest& request,
    std::shared_ptr<ScreenshotExportArtifact> artifact, PinnedCompletion completion) {
    SNOW_SHOT_PIN_PERF_SCOPE("ui.present_pinned_artifact");
    if (!request.isPrepared() || artifact == nullptr || !artifact->isValid()) {
        return false;
    }

    auto* pinnedWindow =
        m_windowPool != nullptr
            ? m_windowPool->acquire(ScreenshotPinnedWindow::RuntimeMode::NoDocument, request.screen)
            : nullptr;
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = request.geometry.nativeGeometry;
    config.canvasSourceRect = request.surfaceCanvasRect;
    config.contentCanvasRect = request.surfaceCanvasRect;
    config.surfaceCanvasRect = request.surfaceCanvasRect;
    // The worker loader returns a fully composited result image. Keep the live
    // renderer neutral so the result style is not applied twice after loading.
    config.resultStyle = ScreenshotResultStyle{};
    config.fullResolutionScaleBasis = request.fullResolutionScaleBasis;
    config.screen = request.screen;
    config.enableEditing = true;
    config.recognition = m_recognition;
    config.qrRecognition = m_qrRecognition;
    config.tableRecognition = m_tableRecognition;
    config.recognitionProvider = m_recognitionProvider;
    config.recognitionResults =
        recognitionResultsForPinnedImage(request.recognitionResults, request.selection,
                                         request.resultStyle, request.fullResolutionScaleBasis);
    config.formattedTextDocument.reset();
    config.formattedPlainText.clear();
    applyPinRuntimeSettings(&config);
    config.groupManager = m_groupManager;
    config.groupId =
        m_groupManager != nullptr ? m_groupManager->activeGroupId() : QStringLiteral("default");
    applyPersistence(&config, {}, true);
    m_pendingPinCoordinator->reserve(pinnedWindow, config.persistenceId, config.groupId,
                                     m_groupManager, artifact);
    ScreenshotImageLoader loader = [artifact](QObject* receiver,
                                              ScreenshotImageLoadCallback callback) mutable {
        auto sharedCallback = std::make_shared<ScreenshotImageLoadCallback>(std::move(callback));
        if (!artifact->requestImage(
                receiver, [sharedCallback](ScreenshotExportImageResult result) mutable {
                    if (*sharedCallback) {
                        auto completion = std::move(*sharedCallback);
                        completion(result.succeeded() ? std::move(result.image) : QImage{});
                    }
                })) {
            if (*sharedCallback) {
                auto completion = std::move(*sharedCallback);
                completion({});
            }
        }
    };
    config.imageLoader =
        m_pendingPinCoordinator->wrapLoader(config.persistenceId, std::move(loader));
    const QPointer<ScreenshotPendingPinCoordinator> coordinator(m_pendingPinCoordinator.get());
    const QString persistenceId = config.persistenceId;
    auto synchronizedCompletion = [coordinator, persistenceId, completion = std::move(completion)](
                                      bool success, QImage image) mutable {
        if (completion) {
            completion(success, image);
        }
        if (!coordinator.isNull()) {
            coordinator->completeFirstFrame(persistenceId, success);
        }
    };
    const bool presented = presentPinnedWindowAndSynchronize(m_windowPool.get(), pinnedWindow,
                                                             config, m_showMainWindowRequested,
                                                             std::move(synchronizedCompletion));
    if (!presented) {
        artifact->cancel();
        m_pendingPinCoordinator->cancel(config.persistenceId);
        return false;
    }
    m_pendingPinCoordinator->updateSnapshot(config.persistenceId,
                                            pinnedWindow->persistenceSnapshot());
    return true;
}

bool ScreenshotSelectionExportUiServices::presentPinnedImageArtifact(
    std::shared_ptr<ScreenshotExportArtifact> artifact, QScreen* screen,
    const QRect& nativeGeometry, const QSize& fullResolutionScaleBasis,
    PinnedCompletion completion) {
    SNOW_SHOT_PIN_PERF_SCOPE("ui.present_pinned_image_artifact");
    if (artifact == nullptr || !artifact->isValid() || screen == nullptr ||
        nativeGeometry.isEmpty() || fullResolutionScaleBasis.isEmpty()) {
        return false;
    }

    auto* pinnedWindow =
        m_windowPool != nullptr
            ? m_windowPool->acquire(ScreenshotPinnedWindow::RuntimeMode::NoDocument, screen)
            : nullptr;
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = nativeGeometry;
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(fullResolutionScaleBasis));
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect = config.canvasSourceRect;
    config.fullResolutionScaleBasis = fullResolutionScaleBasis;
    config.screen = screen;
    config.enableEditing = true;
    config.recognition = m_recognition;
    config.qrRecognition = m_qrRecognition;
    config.tableRecognition = m_tableRecognition;
    config.recognitionProvider = m_recognitionProvider;
    applyPinRuntimeSettings(&config);
    config.groupManager = m_groupManager;
    config.groupId =
        m_groupManager != nullptr ? m_groupManager->activeGroupId() : QStringLiteral("default");
    applyPersistence(&config, {}, true);
    m_pendingPinCoordinator->reserve(pinnedWindow, config.persistenceId, config.groupId,
                                     m_groupManager, artifact);
    ScreenshotImageLoader loader = [artifact](QObject* receiver,
                                              ScreenshotImageLoadCallback callback) mutable {
        auto sharedCallback = std::make_shared<ScreenshotImageLoadCallback>(std::move(callback));
        if (!artifact->requestImage(receiver,
                                    [sharedCallback](ScreenshotExportImageResult result) mutable {
                                        if (*sharedCallback) {
                                            auto completion = std::move(*sharedCallback);
                                            completion(result.succeeded() ? std::move(result.image)
                                                                          : QImage{});
                                        }
                                    }) &&
            *sharedCallback) {
            auto completion = std::move(*sharedCallback);
            completion({});
        }
    };
    config.imageLoader =
        m_pendingPinCoordinator->wrapLoader(config.persistenceId, std::move(loader));
    const QPointer<ScreenshotPendingPinCoordinator> coordinator(m_pendingPinCoordinator.get());
    const QString persistenceId = config.persistenceId;
    auto synchronizedCompletion = [coordinator, persistenceId, completion = std::move(completion)](
                                      bool success, QImage completedImage) mutable {
        if (completion) {
            completion(success, completedImage);
        }
        if (!coordinator.isNull()) {
            coordinator->completeFirstFrame(persistenceId, success);
        }
    };
    const bool presented = presentPinnedWindowAndSynchronize(m_windowPool.get(), pinnedWindow,
                                                             config, m_showMainWindowRequested,
                                                             std::move(synchronizedCompletion));
    if (!presented) {
        artifact->cancel();
        m_pendingPinCoordinator->cancel(config.persistenceId);
        return false;
    }
    m_pendingPinCoordinator->updateSnapshot(config.persistenceId,
                                            pinnedWindow->persistenceSnapshot());
    return true;
}

bool ScreenshotSelectionExportUiServices::presentPinnedImage(
    const QImage& image, QScreen* screen, const QRect& nativeGeometry,
    const QSize& fullResolutionScaleBasis, std::shared_ptr<QTextDocument> formattedTextDocument,
    const QString& formattedPlainText, qreal formattedTextDevicePixelRatio,
    ScreenshotClipboardOriginalContent originalContent, ScreenshotImageLoader imageLoader,
    PinnedCompletion completion) {
    SNOW_SHOT_PIN_PERF_SCOPE("ui.present_pinned_image");
    const QSize imageSize =
        !image.isNull() && !image.size().isEmpty() ? image.size() : fullResolutionScaleBasis;
    if (imageSize.isEmpty() || (!imageLoader && image.isNull()) || screen == nullptr ||
        nativeGeometry.isEmpty()) {
        return false;
    }

    if (!image.isNull()) {
        SNOW_SHOT_PIN_PERF_COUNTER("source.mode.materialized", 1);
        SNOW_SHOT_PIN_PERF_COUNTER("source.retained_bytes", image.sizeInBytes());
    }

    const bool pending = static_cast<bool>(imageLoader);
    std::shared_ptr<ScreenshotExportArtifact> artifact;
    if (pending) {
        artifact =
            std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImageLoader(
                [loader = std::move(imageLoader)](QObject* receiver,
                                                  std::function<void(QImage)> callback) mutable {
                    loader(receiver, std::move(callback));
                    return true;
                }));
    } else {
        artifact =
            std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(image));
    }

    auto* pinnedWindow =
        m_windowPool != nullptr
            ? m_windowPool->acquire(ScreenshotPinnedWindow::RuntimeMode::NoDocument, screen)
            : nullptr;
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = nativeGeometry;
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(imageSize));
    if (!image.isNull()) {
        config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    }
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect = config.canvasSourceRect;
    config.fullResolutionScaleBasis =
        fullResolutionScaleBasis.isEmpty() ? imageSize : fullResolutionScaleBasis;
    config.screen = screen;
    config.enableEditing = true;
    config.formattedTextDocument = std::move(formattedTextDocument);
    config.formattedPlainText = formattedPlainText;
    config.formattedTextDevicePixelRatio = formattedTextDevicePixelRatio;
    config.originalClipboardContent = std::move(originalContent);
    config.recognition = m_recognition;
    config.qrRecognition = m_qrRecognition;
    config.tableRecognition = m_tableRecognition;
    config.recognitionProvider = m_recognitionProvider;
    applyPinRuntimeSettings(&config);
    config.groupManager = m_groupManager;
    config.groupId =
        m_groupManager != nullptr ? m_groupManager->activeGroupId() : QStringLiteral("default");
    applyPersistence(&config, {}, true);
    m_pendingPinCoordinator->reserve(pinnedWindow, config.persistenceId, config.groupId,
                                     m_groupManager, artifact);
    if (pending) {
        ScreenshotImageLoader artifactLoader =
            [artifact](QObject* receiver, ScreenshotImageLoadCallback callback) mutable {
                auto sharedCallback =
                    std::make_shared<ScreenshotImageLoadCallback>(std::move(callback));
                if (!artifact->requestImage(
                        receiver,
                        [sharedCallback](ScreenshotExportImageResult result) mutable {
                            if (*sharedCallback) {
                                auto completion = std::move(*sharedCallback);
                                completion(result.succeeded() ? std::move(result.image) : QImage{});
                            }
                        }) &&
                    *sharedCallback) {
                    auto completion = std::move(*sharedCallback);
                    completion({});
                }
            };
        config.imageLoader =
            m_pendingPinCoordinator->wrapLoader(config.persistenceId, std::move(artifactLoader));
    }
    const QPointer<ScreenshotPendingPinCoordinator> coordinator(m_pendingPinCoordinator.get());
    const QString persistenceId = config.persistenceId;
    auto synchronizedCompletion = [coordinator, persistenceId, completion = std::move(completion)](
                                      bool success, QImage completedImage) mutable {
        if (completion) {
            completion(success, completedImage);
        }
        if (!coordinator.isNull()) {
            coordinator->completeFirstFrame(persistenceId, success);
        }
    };
    const bool presented = presentPinnedWindowAndSynchronize(m_windowPool.get(), pinnedWindow,
                                                             config, m_showMainWindowRequested,
                                                             std::move(synchronizedCompletion));
    if (!presented) {
        artifact->cancel();
        m_pendingPinCoordinator->cancel(config.persistenceId);
        return false;
    }
    m_pendingPinCoordinator->updateSnapshot(config.persistenceId,
                                            pinnedWindow->persistenceSnapshot());
    return true;
}

void ScreenshotSelectionExportUiServices::restorePersistedWindows() {
    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    const QVector<snow_shot::storage::PinnedWindowSummary> summaries =
        applicationStorage.isInitialized() ? applicationStorage.pinnedWindows().summaries()
                                           : QVector<snow_shot::storage::PinnedWindowSummary>{};
    for (const auto& summary : summaries) {
        if (m_groupManager != nullptr && summary.groupId != m_groupManager->activeGroupId()) {
            continue;
        }
        if (m_groupManager != nullptr && m_groupManager->hasWindow(summary.id)) {
            continue;
        }
        const std::optional<snow_shot::storage::PinnedWindowRecord> loadedRecord =
            applicationStorage.pinnedWindows().loadRecord(summary.id);
        if (!loadedRecord.has_value()) {
            continue;
        }
        const snow_shot::storage::PinnedWindowRecord& record = *loadedRecord;
        QScreen* targetScreen = restoreScreen(record);
        if (targetScreen == nullptr || record.nativeGeometry.isEmpty()) {
            continue;
        }
        const screenshot_pinned_restore_geometry::RestoredState restored =
            reconcileRestoreState(record, *targetScreen);
        ScreenshotPinnedWindow::Config config;
        config.nativeGeometry = restored.nativeGeometry;
        config.canvasSourceRect = record.canvasSourceRect;
        // The persisted content/surface rects describe the post-transform
        // frame. Presentation starts from the immutable source canvas and
        // reapplies the transform below, so use the source rect for the
        // containment contract during setup.
        config.contentCanvasRect = record.canvasSourceRect;
        config.surfaceCanvasRect = record.canvasSourceRect;
        config.fullResolutionScaleBasis = record.initialPhysicalSize;
        config.screen = targetScreen;
        config.enableEditing = true;
        config.resultStyle = decodeResultStyle(record.resultStyle);
        config.persistenceId = record.id;
        config.restorePersistentState = true;
        config.persistedOpacityPercent = record.opacityPercent;
        config.persistedImageTransform = record.imageTransform;
        config.persistedQuarterTurns = record.quarterTurns;
        config.persistedThumbnailMode = record.thumbnailMode;
        config.persistedPreThumbnailNativeGeometry = restored.preThumbnailNativeGeometry;
        config.persistedFirstCreationTextDpi = record.firstCreationTextDpi;
        config.persistedCanvasSession = record.canvasSession;
        config.persistedRecognitionResults = record.recognitionResults;
        config.groupManager = m_groupManager;
        config.groupId = record.groupId;
        config.recognition = m_recognition;
        config.qrRecognition = m_qrRecognition;
        config.tableRecognition = m_tableRecognition;
        config.recognitionProvider = m_recognitionProvider;
        applyPersistence(&config, record.id);

        std::shared_ptr<QTextDocument> formattedDocument;
        if (record.sourceKind == snow_shot::storage::PinnedWindowSourceKind::ClipboardText) {
            ScreenshotClipboardOriginalContent original;
            original.html = record.originalHtml;
            original.text = record.originalText;
            const auto rendered = ScreenshotClipboardContentReader::renderOriginalText(
                original, record.firstCreationTextDpi);
            if (!rendered.has_value() || !rendered->isValid()) {
                continue;
            }
            config.imageSource =
                ScreenshotImageSource::fromImage(rendered->image, record.canvasSourceRect);
            config.formattedTextDocument = rendered->formattedDocument;
            config.formattedPlainText = rendered->plainText;
            config.formattedTextDevicePixelRatio = record.firstCreationTextDpi;
            config.originalClipboardContent = std::move(original);
        } else {
            if (record.image.isNull()) {
                continue;
            }
            config.imageSource =
                ScreenshotImageSource::fromImage(record.image, record.canvasSourceRect);
            if (record.sourceKind ==
                snow_shot::storage::PinnedWindowSourceKind::ClipboardImageFile) {
                config.originalClipboardContent.localFilePath = record.originalFilePath;
            }
        }
        auto* window = m_windowPool != nullptr
                           ? m_windowPool->acquire(ScreenshotPinnedWindow::RuntimeMode::NoDocument,
                                                   targetScreen)
                           : nullptr;
        if (window == nullptr ||
            !presentPinnedWindowAndSynchronize(m_windowPool.get(), window, config,
                                               m_showMainWindowRequested, {}, false)) {
            if (window != nullptr) {
                window->deleteLater();
            }
        }
    }
}
