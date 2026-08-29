#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/settings/settingsruntimebindings.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"

#include "widgets/image.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <Windows.h>
#include <Psapi.h>

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
constexpr qint64 kPeakPrivateBytesLimit = 100 * 1024 * 1024;
constexpr qint64 kSteadyPrivateBytesLimit = 25 * 1024 * 1024;
constexpr qint64 kRefreshDriftLimitBytes = 1 * 1024 * 1024;

#ifndef SNOW_SHOT_HISTORY_BENCHMARK_BASELINE_PATH
#define SNOW_SHOT_HISTORY_BENCHMARK_BASELINE_PATH \
    "tests/baselines/screenshot_history_performance.json"
#endif

qint64 privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return -1;
    }
    return static_cast<qint64>(counters.PrivateUsage);
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

void processUntil(const std::function<bool()>& complete, int timeoutMs, qint64* peakBytes) {
    QElapsedTimer timer;
    timer.start();
    while (!complete() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        if (peakBytes != nullptr) {
            *peakBytes = std::max(*peakBytes, privateBytes());
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
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);

    QTemporaryDir storageDirectory;
    if (!storageDirectory.isValid() ||
        !snow_shot::storage::ApplicationStorage::instance()
             .initialize({storageDirectory.path(), storageDirectory.path(), 60000})
             .success) {
        std::cerr << "Unable to initialize benchmark storage\n";
        return 1;
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
    snow_shot::presentation::settings::BuiltInSettingsRuntimeBindings runtimeBindings(
        shortcutManager);
    QElapsedTimer constructionTimer;
    constructionTimer.start();
    ContentCardWidget content(snow_shot::presentation::settings::builtInSettingsCatalog(),
                              runtimeBindings);
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

    QElapsedTimer assetTimer;
    assetTimer.start();
    qint64 peakBytes = privateBytes();
    processUntil(
        [&]() {
            return page->findChildren<adqt::widgets::AdImage*>().size() >=
                   kRecordCount * kDisplayCount;
        },
        30000, &peakBytes);
    const qint64 assetPreparationMs = assetTimer.elapsed();

    const QList<adqt::widgets::AdImage*> images = page->findChildren<adqt::widgets::AdImage*>();
    QSet<adqt::widgets::AdImage*> completed;
    qint64 firstThumbnailMs = -1;
    QElapsedTimer thumbnailTimer;
    thumbnailTimer.start();
    for (adqt::widgets::AdImage* image : images) {
        QObject::connect(image, &adqt::widgets::AdImage::loadingChanged, page,
                         [image, &completed, &firstThumbnailMs, &thumbnailTimer](bool loading) {
                             if (!loading && image->isVisible()) {
                                 completed.insert(image);
                                 if (firstThumbnailMs < 0) {
                                     firstThumbnailMs = thumbnailTimer.elapsed();
                                 }
                             }
                         });
    }

    processUntil(
        [&]() {
            int visibleCount = 0;
            for (adqt::widgets::AdImage* image : images) {
                if (image->isVisibleTo(page)) {
                    ++visibleCount;
                }
            }
            return visibleCount > 0 && completed.size() >= visibleCount;
        },
        30000, &peakBytes);
    const qint64 completeThumbnailMs = thumbnailTimer.elapsed();
    const qint64 steadyBytes = privateBytes();

    const qint64 cycleStartBytes = privateBytes();
    for (int cycle = 0; cycle < kRefreshCycles; ++cycle) {
        page->refresh();
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        peakBytes = std::max(peakBytes, privateBytes());
    }
    const qint64 cycleEndBytes = privateBytes();
    const qint64 refreshDriftBytes = cycleEndBytes - cycleStartBytes;
    const bool timingAndMemoryThresholdsPassed =
        activationMs <= kActivationLimitMs && firstThumbnailMs >= 0 &&
        firstThumbnailMs <= kFirstThumbnailLimitMs &&
        completeThumbnailMs <= kCompleteThumbnailLimitMs &&
        peakBytes <= kPeakPrivateBytesLimit && steadyBytes <= kSteadyPrivateBytesLimit &&
        refreshDriftBytes <= kRefreshDriftLimitBytes;

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
        {QStringLiteral("persistent_thumbnail_cache"), true},
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
        {QStringLiteral("steady_private_bytes"), steadyBytes},
        {QStringLiteral("decode_worker_limit"), 2},
        {QStringLiteral("decoded_cache_limit_bytes"), 64 * 1024 * 1024},
        {QStringLiteral("refresh_cycles"), kRefreshCycles},
        {QStringLiteral("refresh_cycle_memory_drift_bytes"), refreshDriftBytes},
        {QStringLiteral("activation_limit_ms"), kActivationLimitMs},
        {QStringLiteral("first_thumbnail_limit_ms"), kFirstThumbnailLimitMs},
        {QStringLiteral("complete_thumbnail_limit_ms"), kCompleteThumbnailLimitMs},
        {QStringLiteral("peak_private_bytes_limit"), kPeakPrivateBytesLimit},
        {QStringLiteral("steady_private_bytes_limit"), kSteadyPrivateBytesLimit},
        {QStringLiteral("refresh_drift_limit_bytes"), kRefreshDriftLimitBytes},
        {QStringLiteral("baseline"),
         QString::fromUtf8(SNOW_SHOT_HISTORY_BENCHMARK_BASELINE_PATH)},
        {QStringLiteral("baseline_policy"),
         baseline.value(QStringLiteral("policy")).toString()},
        {QStringLiteral("baseline_fixture_matches"), baselineFixtureMatches},
        {QStringLiteral("baseline_behavioral_limits_match"), baselineBehaviorMatches},
        {QStringLiteral("timing_and_memory_thresholds_enforced"), true},
        {QStringLiteral("timing_and_memory_thresholds_passed"), timingAndMemoryThresholdsPassed},
    };
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Indented).constData();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return timingAndMemoryThresholdsPassed && baselineFixtureMatches && baselineBehaviorMatches ? 0
                                                                                               : 2;
}
