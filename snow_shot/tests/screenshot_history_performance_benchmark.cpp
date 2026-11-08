#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/settingsadapters.h"

#include "widgets/image.h"
#include "legacy_thumbnail_test_support.h"

#include <QApplication>
#include <QEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <functional>
#include <iostream>

namespace {
using snow_shot::storage::CaptureHistoryDisplayDraft;
using snow_shot::storage::CaptureHistoryDraft;

constexpr int kRecordCount = 10;
constexpr int kDisplayCount = 2;
constexpr QSize kImageSize(3840, 2160);
constexpr int kRefreshCycles = 30;
constexpr qint64 kActivationLimitMs = 75;
constexpr qint64 kFirstThumbnailLimitMs = 120;
constexpr qint64 kCompleteThumbnailLimitMs = 450;
constexpr qint64 kSteadyPrivateBytesLimit = 25 * 1024 * 1024;
constexpr qint64 kRefreshDriftLimitBytes = 1 * 1024 * 1024;

#ifndef SNOW_SHOT_HISTORY_BENCHMARK_BASELINE_PATH
#define SNOW_SHOT_HISTORY_BENCHMARK_BASELINE_PATH                                                  \
    "tests/baselines/screenshot_history_performance.json"
#endif

struct ProcessMetrics final {
    qint64 privateWorkingSetBytes = -1;
    qint64 privateCommitBytes = -1;
    int threadCount = -1;
};

ProcessMetrics processMetrics() {
    ProcessMetrics metrics;
#if defined(NTDDI_WIN10_CU)
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        metrics.privateWorkingSetBytes = static_cast<qint64>(counters.PrivateWorkingSetSize);
        metrics.privateCommitBytes = static_cast<qint64>(counters.PrivateUsage);
    }
#else
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        metrics.privateCommitBytes = static_cast<qint64>(counters.PrivateUsage);
    }
#endif

    const DWORD processId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return metrics;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    int threadCount = 0;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == processId) {
                ++threadCount;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    metrics.threadCount = threadCount;
    return metrics;
}

CaptureHistoryDraft benchmarkDraft(int index) {
    CaptureHistoryDraft draft;
    draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    draft.createdUtc = QDateTime::currentDateTimeUtc().addSecs(-index);
    draft.canvasBounds = QRect(QPoint(0, 0), kImageSize);
    draft.selection.rectangle = QRect(100, 100, 1920, 1080);
    draft.selection.shadowColor = QColor(0, 0, 0, 96);
    draft.canvasHistory = QByteArrayLiteral("{\"schemaVersion\":1,\"document\":{},\"history\":{}}");
    for (int display = 0; display < kDisplayCount; ++display) {
        QImage image(kImageSize, QImage::Format_RGB32);
        image.fill(QColor::fromHsv((index * 31 + display * 127) % 360, 180, 210));
        draft.displays.push_back(CaptureHistoryDisplayDraft{
            QStringLiteral("display-%1").arg(display),
            QStringLiteral("Display %1").arg(display + 1), std::move(image)});
    }
    return draft;
}

void processUntil(const std::function<bool()>& complete, int timeoutMs,
                  qint64* peakPrivateCommitBytes = nullptr) {
    QElapsedTimer timer;
    timer.start();
    while (!complete() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        if (peakPrivateCommitBytes != nullptr) {
            *peakPrivateCommitBytes =
                std::max(*peakPrivateCommitBytes, processMetrics().privateCommitBytes);
        }
        QThread::msleep(1);
    }
}

QJsonObject readBaseline() {
    QFile file(QString::fromUtf8(SNOW_SHOT_HISTORY_BENCHMARK_BASELINE_PATH));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object()
                                                                          : QJsonObject{};
}

int benchmarkPageSizes() {
    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    for (int index = 0; index < 55; ++index) {
        if (!applicationStorage.captureHistory()
                 .publish(benchmarkDraft(index))
                 .get()
                 .storage.success)
            return 1;
        snow_shot::storage::PinnedWindowRecord pin;
        pin.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        pin.image = QImage(kImageSize, QImage::Format_RGB32);
        pin.image.fill(QColor::fromHsv(index * 31 % 360, 180, 210));
        pin.nativeGeometry = QRect(QPoint(), pin.image.size());
        pin.canvasSourceRect = QRectF(pin.nativeGeometry);
        pin.contentCanvasRect = pin.canvasSourceRect;
        pin.surfaceCanvasRect = pin.canvasSourceRect;
        pin.initialWindowSize = pin.image.size();
        pin.createdUtc = QDateTime::currentDateTimeUtc().addSecs(-index);
        if (index % 3 == 0) {
            pin.sourceKind = snow_shot::storage::PinnedWindowSourceKind::ClipboardText;
            pin.originalText =
                QStringLiteral("Pinned text preview, measured at its original layout.\n")
                    .repeated(100);
            pin.firstCreationTextDpi = 2.0;
        }
        if (!applicationStorage.pinnedWindows().upsert(pin).success ||
            !applicationStorage.pinnedWindows().flush().success)
            return 1;
    }
    QJsonArray samples;
    bool passed = true;
    for (const int pageSize : {10, 20, 50}) {
        snow_shot::storage::InterfaceSettings settings;
        settings.setScreenshotHistoryPageSize(pageSize);
        settings.setPinnedWindowManagementPageSize(pageSize);
        for (int run = 0; run < 3; ++run) {
            for (bool history : {true, false}) {
                QElapsedTimer timer;
                timer.start();
                std::unique_ptr<QWidget> page;
                if (history) {
                    auto* widget = new ScreenshotHistoryPageWidget;
                    page.reset(widget);
                    widget->setActive(true);
                } else {
                    page = std::make_unique<PinnedWindowManagementPageWidget>();
                }
                const qint64 constructionMs = timer.elapsed();
                page->resize(980, 640);
                page->show();
                qint64 firstReadyMs = -1;
                const auto visibleReady = [&] {
                    int visible = 0;
                    int ready = 0;
                    for (auto* image : page->findChildren<adqt::widgets::AdImage*>()) {
                        if (image->visibleRegion().isEmpty())
                            continue;
                        // WhenVisible starts on paint; force it before treating idle as loaded.
                        static_cast<void>(image->grab());
                        ++visible;
                        if (!image->source().isEmpty() && !image->loading() && !image->loadFailed())
                            ++ready;
                    }
                    if (ready && firstReadyMs < 0)
                        firstReadyMs = timer.elapsed();
                    return visible > 0 && visible == ready;
                };
                QApplication::processEvents();
                processUntil(visibleReady, 10000);
                const qint64 visibleReadyMs = timer.elapsed();
                int cards = 0;
                for (auto* widget : page->findChildren<QWidget*>())
                    if (widget->objectName() == QStringLiteral("pinnedManagementRecord") ||
                        widget->objectName().startsWith(QStringLiteral("screenshotHistoryEntry-")))
                        ++cards;
                passed &= visibleReady() && cards > 0 && cards <= 8;
                auto* area = page->findChild<QScrollArea*>();
                timer.restart();
                area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
                // Deliver the viewport's queued row update before checking the newly visible rows.
                for (int turn = 0; turn < 3; ++turn)
                    QApplication::processEvents();
                processUntil(visibleReady, 10000);
                passed &= visibleReady();
                samples.push_back(
                    QJsonObject{{QStringLiteral("page"),
                                 history ? QStringLiteral("history") : QStringLiteral("pins")},
                                {QStringLiteral("page_size"), pageSize},
                                {QStringLiteral("run"), run + 1},
                                {QStringLiteral("construction_ms"), constructionMs},
                                {QStringLiteral("first_visible_ready_ms"), firstReadyMs},
                                {QStringLiteral("all_visible_ready_ms"), visibleReadyMs},
                                {QStringLiteral("scroll_to_bottom_ready_ms"), timer.elapsed()},
                                {QStringLiteral("materialized_cards"), cards}});
                page.reset();
                applicationStorage.pinnedPreviewPool().waitForDone();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QApplication::processEvents();
            }
        }
    }
    std::cout << QJsonDocument(QJsonObject{{QStringLiteral("samples"), samples},
                                           {QStringLiteral("passed"), passed},
                                           {QStringLiteral("fixture_records_per_page_type"), 55}})
                     .toJson(QJsonDocument::Indented)
                     .constData();
    return passed ? 0 : 2;
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    thumbnail_tests::CacheEnvironment cacheEnvironment;
    if (!cacheEnvironment.isValid()) {
        std::cerr << "Unable to isolate benchmark thumbnail cache\n";
        return 1;
    }
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);

    QTemporaryDir storageDirectory;
    if (!storageDirectory.isValid() ||
        !snow_shot::storage::ApplicationStorage::instance()
             .initialize({storageDirectory.path(), storageDirectory.path(), 60000})
             .success) {
        std::cerr << "Unable to initialize benchmark storage\n";
        return 1;
    }

    if (application.arguments().contains(QStringLiteral("--page-sizes"))) {
        const int result = benchmarkPageSizes();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return result;
    }

    auto& repository = snow_shot::storage::ApplicationStorage::instance().captureHistory();
    for (int index = 0; index < kRecordCount; ++index) {
        const auto result = repository.publish(benchmarkDraft(index)).get();
        if (!result.storage.success) {
            std::cerr << "Unable to publish benchmark record\n";
            return 1;
        }
    }

    snow_shot::presentation::GlobalShortcutManager shortcutManager;
    const auto& registry = snow_shot::presentation::settings::builtInSettingsRegistry();
    snow_shot::presentation::settings::BuiltInSettingsBackend backend(shortcutManager);
    snow_shot::presentation::settings::SettingsRuntimeSession session(registry, backend);
    QElapsedTimer constructionTimer;
    constructionTimer.start();
    ContentCardWidget content(registry, session);
    const qint64 constructionMs = constructionTimer.elapsed();
    content.resize(900, 556);
    content.show();
    QApplication::processEvents();

    QElapsedTimer activationTimer;
    activationTimer.start();
    content.setCurrentRoute(QStringLiteral("/history"));
    const qint64 activationMs = activationTimer.elapsed();

    auto* page =
        content.findChild<ScreenshotHistoryPageWidget*>(QStringLiteral("screenshotHistoryPage"));
    if (page == nullptr) {
        std::cerr << "History route was not created\n";
        return 1;
    }
    const ProcessMetrics postNavigationMetrics = processMetrics();
    const int historyPageCountPostNavigation =
        content.findChildren<ScreenshotHistoryPageWidget*>().size();
    const int adImageCountPostNavigation = page->findChildren<adqt::widgets::AdImage*>().size();

    QElapsedTimer assetTimer;
    assetTimer.start();
    qint64 peakBytes = processMetrics().privateCommitBytes;
    processUntil([&]() { return !page->findChildren<adqt::widgets::AdImage*>().isEmpty(); }, 30000,
                 &peakBytes);
    const qint64 assetPreparationMs = assetTimer.elapsed();

    const QList<adqt::widgets::AdImage*> images = page->findChildren<adqt::widgets::AdImage*>();
    qint64 firstThumbnailMs = -1;
    QElapsedTimer thumbnailTimer;
    thumbnailTimer.start();
    // Poll current state so completions during asset preparation are included.
    // QWidget visibility alone also includes rows clipped below the scroll viewport.
    processUntil(
        [&]() {
            int visibleCount = 0;
            int completedCount = 0;
            for (adqt::widgets::AdImage* image : images) {
                if (image->visibleRegion().isEmpty())
                    continue;
                ++visibleCount;
                if (!image->source().isEmpty() && !image->loading() && !image->loadFailed())
                    ++completedCount;
            }
            if (completedCount > 0 && firstThumbnailMs < 0)
                firstThumbnailMs = thumbnailTimer.elapsed();
            return visibleCount > 0 && completedCount == visibleCount;
        },
        30000, &peakBytes);
    const qint64 completeThumbnailMs = thumbnailTimer.elapsed();
    const ProcessMetrics immediateMetrics = processMetrics();
    const int historyPageCountImmediate =
        content.findChildren<ScreenshotHistoryPageWidget*>().size();
    const int adImageCountImmediate = page->findChildren<adqt::widgets::AdImage*>().size();

    const qint64 cycleStartBytes = immediateMetrics.privateCommitBytes;
    for (int cycle = 0; cycle < kRefreshCycles; ++cycle) {
        content.setCurrentRoute(QStringLiteral("/"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (content.findChildren<ScreenshotHistoryPageWidget*>().size() != 0) {
            std::cerr << "History route retained a page after navigation away\n";
            return 1;
        }
        content.setCurrentRoute(QStringLiteral("/history"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        if (content.findChildren<ScreenshotHistoryPageWidget*>().size() != 1) {
            std::cerr << "History route created duplicate page instances during cycle\n";
            return 1;
        }
        page = content.findChild<ScreenshotHistoryPageWidget*>(
            QStringLiteral("screenshotHistoryPage"));
        if (page == nullptr) {
            std::cerr << "History route was not recreated during benchmark cycle\n";
            return 1;
        }
        peakBytes = std::max(peakBytes, processMetrics().privateCommitBytes);
    }
    const qint64 postCyclePrivateCommitBytes = processMetrics().privateCommitBytes;
    const int pendingHistoryJobsBeforeDrain = screenshotHistoryPendingJobCount();
    // Drain asset reads and image decodes after rapid navigation. The image
    // loader has its own workers, independent of the history executor.
    for (int drainPass = 0; drainPass < 3; ++drainPass) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        processUntil(
            [page]() {
                const auto previews = page->findChildren<adqt::widgets::AdImage*>();
                return screenshotHistoryPendingJobCount() == 0 &&
                       std::none_of(previews.cbegin(), previews.cend(),
                                    [](const auto* preview) { return preview->loading(); });
            },
            30000, &peakBytes);
    }
    const ProcessMetrics drainedMetrics = processMetrics();
    QThread::msleep(100);
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    const ProcessMetrics idleMetrics = processMetrics();
    const bool executorContractPassed = screenshotHistoryWorkerCount() == 2 &&
                                        screenshotHistoryWorkerExpiryTimeout() == 0 &&
                                        screenshotHistoryPendingJobCount() == 0;
    const bool timingAndMemoryThresholdsPassed =
        activationMs <= kActivationLimitMs && firstThumbnailMs >= 0 &&
        firstThumbnailMs <= kFirstThumbnailLimitMs &&
        completeThumbnailMs <= kCompleteThumbnailLimitMs &&
        drainedMetrics.privateCommitBytes <= kSteadyPrivateBytesLimit &&
        (drainedMetrics.privateCommitBytes - cycleStartBytes) <= kRefreshDriftLimitBytes &&
        executorContractPassed;
    const qint64 refreshDriftBytes = drainedMetrics.privateCommitBytes - cycleStartBytes;

    const QJsonObject expectedFixture{
        {QStringLiteral("records"), kRecordCount},
        {QStringLiteral("displays_per_record"), kDisplayCount},
        {QStringLiteral("image_width"), kImageSize.width()},
        {QStringLiteral("image_height"), kImageSize.height()},
    };
    const QJsonObject expectedBehavior{
        {QStringLiteral("decode_worker_limit"), 2},
        {QStringLiteral("decoded_cache_limit_bytes"), 64 * 1024 * 1024},
        {QStringLiteral("gui_thread_payload_io"), false},
        {QStringLiteral("persistent_thumbnail_cache"), false},
        {QStringLiteral("history_worker_limit"), 2},
        {QStringLiteral("history_worker_expiry_timeout_ms"), 0},
    };
    const QJsonObject baseline = readBaseline();
    const bool baselineFixtureMatches =
        baseline.value(QStringLiteral("fixture")).toObject() == expectedFixture;
    const bool baselineBehaviorMatches =
        baseline.value(QStringLiteral("behavioral_limits")).toObject() == expectedBehavior;

    QJsonObject result{
        {QStringLiteral("records"), kRecordCount},
        {QStringLiteral("displays_per_record"), kDisplayCount},
        {QStringLiteral("image_width"), kImageSize.width()},
        {QStringLiteral("image_height"), kImageSize.height()},
        {QStringLiteral("main_content_construction_ms"), constructionMs},
        {QStringLiteral("history_activation_sync_ms"), activationMs},
        {QStringLiteral("asset_preparation_ms"), assetPreparationMs},
        {QStringLiteral("first_visible_thumbnail_ms"), firstThumbnailMs},
        {QStringLiteral("complete_visible_thumbnails_ms"), completeThumbnailMs},
        {QStringLiteral("peak_private_bytes"), peakBytes},
        {QStringLiteral("post_cycle_private_commit_bytes"), postCyclePrivateCommitBytes},
        {QStringLiteral("immediate_private_working_set_bytes"),
         immediateMetrics.privateWorkingSetBytes},
        {QStringLiteral("immediate_private_commit_bytes"), immediateMetrics.privateCommitBytes},
        {QStringLiteral("immediate_process_thread_count"), immediateMetrics.threadCount},
        {QStringLiteral("drained_private_working_set_bytes"),
         drainedMetrics.privateWorkingSetBytes},
        {QStringLiteral("drained_private_commit_bytes"), drainedMetrics.privateCommitBytes},
        {QStringLiteral("steady_private_bytes"), drainedMetrics.privateCommitBytes},
        {QStringLiteral("drained_process_thread_count"), drainedMetrics.threadCount},
        {QStringLiteral("idle_private_working_set_bytes"), idleMetrics.privateWorkingSetBytes},
        {QStringLiteral("idle_private_commit_bytes"), idleMetrics.privateCommitBytes},
        {QStringLiteral("idle_process_thread_count"), idleMetrics.threadCount},
        {QStringLiteral("history_page_count_immediate"), historyPageCountImmediate},
        {QStringLiteral("ad_image_count_immediate"), adImageCountImmediate},
        {QStringLiteral("history_page_count_post_navigation"), historyPageCountPostNavigation},
        {QStringLiteral("ad_image_count_post_navigation"), adImageCountPostNavigation},
        {QStringLiteral("post_navigation_private_working_set_bytes"),
         postNavigationMetrics.privateWorkingSetBytes},
        {QStringLiteral("post_navigation_private_commit_bytes"),
         postNavigationMetrics.privateCommitBytes},
        {QStringLiteral("post_navigation_process_thread_count"), postNavigationMetrics.threadCount},
        {QStringLiteral("history_page_count_drained"),
         content.findChildren<ScreenshotHistoryPageWidget*>().size()},
        {QStringLiteral("ad_image_count_drained"),
         page->findChildren<adqt::widgets::AdImage*>().size()},
        {QStringLiteral("history_page_count_idle"),
         content.findChildren<ScreenshotHistoryPageWidget*>().size()},
        {QStringLiteral("ad_image_count_idle"),
         page->findChildren<adqt::widgets::AdImage*>().size()},
        {QStringLiteral("pending_history_jobs_before_drain"), pendingHistoryJobsBeforeDrain},
        {QStringLiteral("pending_history_jobs_after_drain"), screenshotHistoryPendingJobCount()},
        {QStringLiteral("executor_contract_passed"), executorContractPassed},
        {QStringLiteral("decode_worker_limit"), 2},
        {QStringLiteral("decoded_cache_limit_bytes"), 64 * 1024 * 1024},
        {QStringLiteral("refresh_cycles"), kRefreshCycles},
        {QStringLiteral("refresh_cycle_memory_drift_bytes"), refreshDriftBytes},
        {QStringLiteral("activation_limit_ms"), kActivationLimitMs},
        {QStringLiteral("first_thumbnail_limit_ms"), kFirstThumbnailLimitMs},
        {QStringLiteral("complete_thumbnail_limit_ms"), kCompleteThumbnailLimitMs},
        {QStringLiteral("steady_private_bytes_limit"), kSteadyPrivateBytesLimit},
        {QStringLiteral("refresh_drift_limit_bytes"), kRefreshDriftLimitBytes},
        {QStringLiteral("baseline"), QString::fromUtf8(SNOW_SHOT_HISTORY_BENCHMARK_BASELINE_PATH)},
        {QStringLiteral("baseline_policy"), baseline.value(QStringLiteral("policy")).toString()},
        {QStringLiteral("baseline_fixture_matches"), baselineFixtureMatches},
        {QStringLiteral("baseline_behavioral_limits_match"), baselineBehaviorMatches},
        {QStringLiteral("timing_and_memory_thresholds_enforced"), true},
        {QStringLiteral("timing_and_memory_thresholds_passed"), timingAndMemoryThresholdsPassed},
    };
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Indented).constData();
    content.setCurrentRoute(QStringLiteral("/"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return timingAndMemoryThresholdsPassed && baselineFixtureMatches && baselineBehaviorMatches ? 0
                                                                                                : 2;
}
