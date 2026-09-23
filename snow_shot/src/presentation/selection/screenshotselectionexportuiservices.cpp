#include "../pinned/pinnedwindowplatform.h"
#include "snow_shot/presentation/screenshotselectionexportuiservices.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotselectionpin.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
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
void applyPinRuntimeSettings(ScreenshotPinnedWindow::Config* config) {
    if (config == nullptr) {
        return;
    }
    const snow_shot::storage::PinToScreenSettings settings;
    config->mouseWheelZoomMode = settings.mouseWheelZoomMode();
    config->automaticTextRecognition = settings.automaticTextRecognition();
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
    if (id.isEmpty())
        storage.pinnedWindows().reserveCreation(config->persistenceId);
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
    config->persistenceCloser = [](const snow_shot::storage::PinnedWindowRecord& record) {
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            static_cast<void>(storage.pinnedWindows().updateState(record));
            static_cast<void>(storage.pinnedWindows().markClosed(record.id));
        }
    };
    config->replacementPersistenceWriter =
        [](const snow_shot::storage::PinnedWindowRecord& record) {
            auto& storage = snow_shot::storage::ApplicationStorage::instance();
            if (!storage.configurationDirectory().isEmpty()) {
                static_cast<void>(storage.pinnedWindows().upsert(record));
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

// Restore display-local positions within the current usable area. Window
// extents retain their platform units independently of image density.
screenshot_pinned_restore_geometry::RestoredState
reconcileRestoreState(const snow_shot::storage::PinnedWindowRecord& record, QScreen& target) {
    using namespace snow_shot::presentation;
    const auto restore = [&target](const snow_shot::storage::PinnedWindowPlacement& placement) {
        return placement.isValid()
                   ? pinnedWindowRect(recoverPinnedPlacement(placement, target), target)
                   : QRect();
    };
    return {restore(record.placement), restore(record.preThumbnailPlacement),
            restore(record.hideToTopPlacement)};
}

QScreen* restoreScreen(const snow_shot::storage::PinnedWindowRecord& record) {
    return snow_shot::presentation::pinnedDisplay(record.placement);
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

    ScreenshotPinnedWindow* acquire(QScreen* screen) {
        if (screen != nullptr) {
            m_targetScreen = screen;
        }

        ScreenshotPinnedWindow* window = m_spare;
        [[maybe_unused]] bool usedSpare = false;
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
            window = new ScreenshotPinnedWindow();
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

        auto* spare = new ScreenshotPinnedWindow();
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
            QObject::connect(
                groupManager,
                &snow_shot::presentation::PinnedWindowGroupManager::groupDeletionRequested, this,
                [this, persistenceId](const QString& deletedGroup) {
                    auto transaction = m_transactions.find(persistenceId);
                    if (transaction == m_transactions.end())
                        return;
                    const QString group = transaction->window ? transaction->window->groupId()
                                                              : transaction->snapshot.groupId;
                    if (deletedGroup == group) {
                        removePersistedRecord(persistenceId);
                        finish(persistenceId);
                    }
                });
        }
        QObject::connect(
            window, &ScreenshotPinnedWindow::closingForPersistence, this,
            [this, persistenceId](const snow_shot::storage::PinnedWindowRecord& snapshot,
                                  snow_shot::storage::PinnedWindowCloseIntent intent) {
                auto transaction = m_transactions.find(persistenceId);
                if (transaction == m_transactions.end()) {
                    return;
                }
                transaction->snapshot = snapshot;
                transaction->intent = intent;
                if (intent == snow_shot::storage::PinnedWindowCloseIntent::Destroy) {
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

    void completeFirstFrame(const QString& persistenceId, bool success) {
        auto transaction = m_transactions.find(persistenceId);
        if (transaction == m_transactions.end()) {
            return;
        }
        if ((!success &&
             transaction->intent != snow_shot::storage::PinnedWindowCloseIntent::Close) ||
            transaction->removed || transaction->artifact == nullptr) {
            finish(persistenceId);
            return;
        }
        if (!transaction->window.isNull() &&
            transaction->intent == snow_shot::storage::PinnedWindowCloseIntent::Preserve)
            transaction->snapshot = transaction->window->persistenceSnapshot();
        if (transaction->snapshot.sourceKind !=
            snow_shot::storage::PinnedWindowSourceKind::ImageData) {
            persistNonImageSource(*transaction);
            finish(persistenceId);
            return;
        }
        const QPointer<ScreenshotPendingPinCoordinator> receiver(this);
        const std::shared_ptr<ScreenshotExportArtifact> artifact = transaction->artifact;
        const auto compression = snow_shot::storage::ApplicationStorage::instance()
                                     .configuration()
                                     .value(QStringLiteral("pinned_history/compression_level"))
                                     .toString();
        if (!artifact->requestPng(
                this, ScreenshotImageFileService::compressionLevelForKey(compression),
                [receiver, persistenceId](ScreenshotExportEncodingResult result) mutable {
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
        snow_shot::storage::PinnedWindowCloseIntent intent =
            snow_shot::storage::PinnedWindowCloseIntent::Preserve;
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
        if (!transaction.window.isNull() &&
            transaction.intent == snow_shot::storage::PinnedWindowCloseIntent::Preserve) {
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
        if (!transaction->removed && result.succeeded()) {
            if (!transaction->window.isNull() &&
                transaction->intent == snow_shot::storage::PinnedWindowCloseIntent::Preserve)
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
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (storage.isInitialized())
            static_cast<void>(storage.pinnedWindows().enforcePolicy());
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
                                       const ScreenshotPinnedWindow::Config& requestedConfig,
                                       const std::function<void()>& showMainWindowRequested,
                                       std::function<void(bool, QImage)> completion = {}) {
    ScreenshotPinnedWindow::Config config = requestedConfig;
    if (!config.placement.isValid() && config.screen)
        config.placement =
            snow_shot::presentation::pinnedPlacement(config.nativeGeometry, *config.screen);
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
    const bool presented = window->present(config, std::move(synchronizedCompletion));
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
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized())
        for (const auto& id : m_restoringIds)
            storage.pinnedWindows().cancelRestore(id);
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

    snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.export"),
                                     QStringLiteral("pin.started"),
                                     {{QStringLiteral("operation"), artifact->diagnosticId()}});

    auto* pinnedWindow = m_windowPool != nullptr ? m_windowPool->acquire(request.screen) : nullptr;
    ScreenshotPinnedWindow::Config config;
    config.creationSource = snow_shot::storage::PinnedWindowCreationSource::Screenshot;
    config.nativeGeometry = request.geometry.nativeGeometry;
    config.canvasSourceRect = request.surfaceCanvasRect;
    config.contentCanvasRect = request.surfaceCanvasRect;
    config.surfaceCanvasRect = request.surfaceCanvasRect;
    // The worker loader returns a fully composited result image. Keep the live
    // renderer neutral so the result style is not applied twice after loading.
    config.resultStyle = ScreenshotResultStyle{};
    config.borderAppearance =
        screenshotSelectionBorderAppearance(request.selection.size(), request.resultStyle);
    config.initialWindowSize = request.initialWindowSize;
    config.screen = request.screen;
    config.enableEditing = true;
    config.recognition = m_recognition;
    config.qrRecognition = m_qrRecognition;
    config.tableRecognition = m_tableRecognition;
    config.recognitionProvider = m_recognitionProvider;
    // The pinned image retains the source canvas coordinates, including shadow padding.
    // Cached OCR quads must stay in that same space to align with the image.
    config.recognitionResults = request.recognitionResults;
    config.recognitionVisible = request.recognitionVisible;
    config.translationVisible = request.translationVisible;
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
    auto synchronizedCompletion = [coordinator, persistenceId, operation = artifact->diagnosticId(),
                                   completion = std::move(completion)](bool success,
                                                                       QImage image) mutable {
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.export"), QStringLiteral("pin.finished"),
            {{QStringLiteral("operation"), operation},
             {QStringLiteral("stage"), QStringLiteral("first_frame")},
             {QStringLiteral("outcome"),
              success ? QStringLiteral("succeeded") : QStringLiteral("failed")}},
            success ? QtInfoMsg : QtWarningMsg);
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
        snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.export"),
                                         QStringLiteral("pin.rejected"),
                                         {{QStringLiteral("operation"), artifact->diagnosticId()},
                                          {QStringLiteral("stage"), QStringLiteral("presentation")},
                                          {QStringLiteral("outcome"), QStringLiteral("failed")}},
                                         QtWarningMsg);
        return false;
    }
    m_pendingPinCoordinator->updateSnapshot(config.persistenceId,
                                            pinnedWindow->persistenceSnapshot());
    return true;
}

bool ScreenshotSelectionExportUiServices::presentPinnedImageArtifact(
    std::shared_ptr<ScreenshotExportArtifact> artifact, QScreen* screen,
    const QRect& nativeGeometry, const QSize& initialWindowSize, PinnedCompletion completion) {
    SNOW_SHOT_PIN_PERF_SCOPE("ui.present_pinned_image_artifact");
    if (artifact == nullptr || !artifact->isValid() || screen == nullptr ||
        nativeGeometry.isEmpty() || initialWindowSize.isEmpty()) {
        return false;
    }

    snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.export"),
                                     QStringLiteral("pin.started"),
                                     {{QStringLiteral("operation"), artifact->diagnosticId()}});

    auto* pinnedWindow = m_windowPool != nullptr ? m_windowPool->acquire(screen) : nullptr;
    ScreenshotPinnedWindow::Config config;
    config.creationSource = snow_shot::storage::PinnedWindowCreationSource::Screenshot;
    config.nativeGeometry = nativeGeometry;
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(initialWindowSize));
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect = config.canvasSourceRect;
    config.initialWindowSize = initialWindowSize;
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
    auto synchronizedCompletion =
        [coordinator, persistenceId, operation = artifact->diagnosticId(),
         completion = std::move(completion)](bool success, QImage completedImage) mutable {
            snow_shot::diagnostics::logEvent(
                QStringLiteral("snow_shot.export"), QStringLiteral("pin.finished"),
                {{QStringLiteral("operation"), operation},
                 {QStringLiteral("stage"), QStringLiteral("first_frame")},
                 {QStringLiteral("outcome"),
                  success ? QStringLiteral("succeeded") : QStringLiteral("failed")}},
                success ? QtInfoMsg : QtWarningMsg);
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
        snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.export"),
                                         QStringLiteral("pin.rejected"),
                                         {{QStringLiteral("operation"), artifact->diagnosticId()},
                                          {QStringLiteral("stage"), QStringLiteral("presentation")},
                                          {QStringLiteral("outcome"), QStringLiteral("failed")}},
                                         QtWarningMsg);
        return false;
    }
    m_pendingPinCoordinator->updateSnapshot(config.persistenceId,
                                            pinnedWindow->persistenceSnapshot());
    return true;
}

bool ScreenshotSelectionExportUiServices::presentPinnedImage(
    const QImage& image, QScreen* screen, const QRect& nativeGeometry,
    const QSize& initialWindowSize, std::shared_ptr<QTextDocument> formattedTextDocument,
    const QString& formattedPlainText, qreal formattedTextDevicePixelRatio,
    ScreenshotClipboardOriginalContent originalContent, ScreenshotImageLoader imageLoader,
    PinnedCompletion completion,
    std::optional<snow_shot::storage::PinnedBorderAppearance> borderAppearance,
    snow_shot::storage::PinnedWindowCreationSource source) {
    const QSize imageSize =
        !image.isNull() && !image.size().isEmpty() ? image.size() : initialWindowSize;
    if (imageSize.isEmpty() || (!imageLoader && image.isNull()) || screen == nullptr ||
        nativeGeometry.isEmpty()) {
        return false;
    }
    return presentPinnedImageOnCanvas(
        image, screen, nativeGeometry, initialWindowSize,
        QRectF(QPointF(0.0, 0.0), QSizeF(imageSize)), std::move(formattedTextDocument),
        formattedPlainText, formattedTextDevicePixelRatio, std::move(originalContent),
        std::move(imageLoader), std::move(completion), std::move(borderAppearance), source);
}

bool ScreenshotSelectionExportUiServices::presentCompositedSelectionImage(
    const QImage& image, const ScreenshotPinnedSelectionRequest& request,
    PinnedCompletion completion) {
    if (image.isNull() || image.size().isEmpty() || !request.isPrepared() ||
        request.screen.isNull() ||
        request.surfaceCanvasRect.size() != QSizeF(request.initialWindowSize)) {
        return false;
    }
    return presentPinnedImageOnCanvas(
        image, request.screen.data(), request.geometry.nativeGeometry, request.initialWindowSize,
        request.surfaceCanvasRect, {}, {}, 1.0, {}, {}, std::move(completion),
        screenshotSelectionBorderAppearance(request.selection.size(), request.resultStyle),
        snow_shot::storage::PinnedWindowCreationSource::ScreenshotHistory);
}

bool ScreenshotSelectionExportUiServices::presentPinnedImageOnCanvas(
    const QImage& image, QScreen* screen, const QRect& nativeGeometry,
    const QSize& initialWindowSize, const QRectF& canvasRect,
    std::shared_ptr<QTextDocument> formattedTextDocument, const QString& formattedPlainText,
    qreal formattedTextDevicePixelRatio, ScreenshotClipboardOriginalContent originalContent,
    ScreenshotImageLoader imageLoader, PinnedCompletion completion,
    std::optional<snow_shot::storage::PinnedBorderAppearance> borderAppearance,
    snow_shot::storage::PinnedWindowCreationSource source) {
    SNOW_SHOT_PIN_PERF_SCOPE("ui.present_pinned_image");
    const QSize imageSize =
        !image.isNull() && !image.size().isEmpty() ? image.size() : initialWindowSize;
    if (imageSize.isEmpty() || (!imageLoader && image.isNull()) || screen == nullptr ||
        nativeGeometry.isEmpty() || !canvasRect.isValid() || canvasRect.isEmpty()) {
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

    auto* pinnedWindow = m_windowPool != nullptr ? m_windowPool->acquire(screen) : nullptr;
    ScreenshotPinnedWindow::Config config;
    config.creationSource = source;
    config.nativeGeometry = nativeGeometry;
    config.canvasSourceRect = canvasRect;
    config.borderAppearance = std::move(borderAppearance);
    if (!image.isNull()) {
        config.imageSource = ScreenshotImageSource::fromImage(image, canvasRect);
    }
    config.contentCanvasRect = canvasRect;
    config.surfaceCanvasRect = canvasRect;
    config.initialWindowSize = initialWindowSize.isEmpty() ? imageSize : initialWindowSize;
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
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.isInitialized())
        return;
    for (const auto& summary : storage.pinnedWindows().summaries()) {
        if (summary.ignored ||
            (m_groupManager && summary.groupId != m_groupManager->activeGroupId()) ||
            m_restoringIds.contains(summary.id) ||
            (m_groupManager && m_groupManager->hasWindow(summary.id)))
            continue;
        static_cast<void>(restoreRecord(summary.id, false));
    }
}

bool ScreenshotSelectionExportUiServices::restoreRecord(const QString& id, bool activateGroup) {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.isInitialized() || m_restoringIds.contains(id))
        return false;
    const auto loaded = storage.pinnedWindows().loadRecord(id);
    if (!loaded)
        return false;
    const auto& record = *loaded;
    if (m_groupManager) {
        if (activateGroup && !m_groupManager->setActiveGroup(record.groupId))
            return false;
        if (record.groupId != m_groupManager->activeGroupId())
            return false;
        if (m_groupManager->hasWindow(id))
            return m_groupManager->showWindow(id);
    }
    QScreen* targetScreen = restoreScreen(record);
    if (targetScreen == nullptr || record.nativeGeometry.isEmpty()) {
        return false;
    }
    const screenshot_pinned_restore_geometry::RestoredState restored =
        reconcileRestoreState(record, *targetScreen);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = restored.nativeGeometry;
    config.placement =
        snow_shot::presentation::recoverPinnedPlacement(record.placement, *targetScreen);
    config.canvasSourceRect = record.canvasSourceRect;
    // The persisted content/surface rects describe the post-transform
    // frame. Presentation starts from the immutable source canvas and
    // reapplies the transform below, so use the source rect for the
    // containment contract during setup.
    config.contentCanvasRect = record.canvasSourceRect;
    config.surfaceCanvasRect = record.canvasSourceRect;
    config.initialWindowSize = record.initialWindowSize;
    config.screen = targetScreen;
    config.enableEditing = true;
    config.resultStyle = decodeResultStyle(record.resultStyle);
    config.borderAppearance = record.borderAppearance;
    config.persistenceId = record.id;
    config.creationSource = record.creationSource;
    config.restorePersistentState = true;
    config.persistedOpacityPercent = record.opacityPercent;
    config.persistedClickThroughOpacityPercent = record.clickThroughOpacityPercent;
    config.persistedImageTransform = record.imageTransform;
    config.persistedQuarterTurns = record.quarterTurns;
    config.persistedHideToTopMode = record.hideToTopMode;
    config.persistedHideToTopHandleNativeGeometry = restored.hideToTopHandleNativeGeometry;
    config.persistedHideToTopAccentIndex = record.hideToTopAccentIndex;
    config.persistedThumbnailMode = record.thumbnailMode;
    config.persistedClickThroughMode = record.clickThroughMode;
    config.persistedAlwaysOnTop = record.alwaysOnTop;
    config.persistedShowBorder = record.showBorder;
    config.persistedPreThumbnailNativeGeometry = restored.preThumbnailNativeGeometry;
    if (record.preThumbnailPlacement.isValid()) {
        config.persistedPreThumbnailPlacement = snow_shot::presentation::recoverPinnedPlacement(
            record.preThumbnailPlacement, *targetScreen);
    }
    config.persistedFirstCreationTextDpi = record.firstCreationTextDpi;
    config.persistedCanvasSession = record.canvasSession;
    config.persistedRecognitionResults = record.recognitionResults;
    config.persistedRecognitionVisible = record.recognitionVisible;
    config.persistedTranslationVisible = record.translationVisible;
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
            return false;
        }
        config.imageSource =
            ScreenshotImageSource::fromImage(rendered->image, record.canvasSourceRect);
        config.formattedTextDocument = rendered->formattedDocument;
        config.formattedPlainText = rendered->plainText;
        config.formattedTextDevicePixelRatio = record.firstCreationTextDpi;
        config.originalClipboardContent = std::move(original);
    } else {
        if (record.image.isNull()) {
            return false;
        }
        config.imageSource =
            ScreenshotImageSource::fromImage(record.image, record.canvasSourceRect);
        if (record.sourceKind == snow_shot::storage::PinnedWindowSourceKind::ClipboardImageFile) {
            config.originalClipboardContent.localFilePath = record.originalFilePath;
        }
    }
    auto* window = m_windowPool != nullptr ? m_windowPool->acquire(targetScreen) : nullptr;
    if (!window)
        return false;
    if (!storage.pinnedWindows().beginRestore(record.id).success) {
        window->deleteLater();
        return false;
    }
    m_restoringIds.insert(record.id);
    const QPointer<ScreenshotPendingPinCoordinator> lifetime(m_pendingPinCoordinator.get());
    const QPointer<ScreenshotPinnedWindow> windowGuard(window);
    const bool presented = presentPinnedWindowAndSynchronize(
        m_windowPool.get(), window, config, m_showMainWindowRequested,
        [this, lifetime, id, windowGuard](bool success, QImage) {
            if (lifetime.isNull())
                return;
            m_restoringIds.remove(id);
            auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
            if (applicationStorage.isInitialized()) {
                if (success) {
                    if (!applicationStorage.pinnedWindows().markRestored(id).success && windowGuard)
                        windowGuard->requestDestroy();
                } else {
                    applicationStorage.pinnedWindows().cancelRestore(id);
                    if (m_restoreFailure)
                        m_restoreFailure();
                }
            }
        });
    if (!presented) {
        m_restoringIds.remove(id);
        storage.pinnedWindows().cancelRestore(id);
    }
    return presented;
}

void ScreenshotSelectionExportUiServices::restoreLastClosedWindow() {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.isInitialized())
        return;
    auto records = storage.pinnedWindows().summaries();
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        if (a.activitySequence != b.activitySequence)
            return a.activitySequence > b.activitySequence;
        return a.lastClosedUtc != b.lastClosedUtc ? a.lastClosedUtc > b.lastClosedUtc : a.id < b.id;
    });
    for (const auto& record : records) {
        if (!record.ignored || m_restoringIds.contains(record.id) ||
            (m_groupManager && record.groupId != m_groupManager->activeGroupId()))
            continue;
        if (!restoreRecord(record.id, false) && m_restoreFailure)
            m_restoreFailure();
        return;
    }
}

void ScreenshotSelectionExportUiServices::destroyRecords(const QVector<QString>& ids) {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.isInitialized())
        return;
    for (const auto& id : ids) {
        if (!storage.pinnedWindows().remove(id).success)
            continue;
        m_pendingPinCoordinator->cancel(id);
        m_restoringIds.remove(id);
        if (m_groupManager)
            m_groupManager->destroyWindow(id);
    }
}
