#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/storage/applicationstorage.h"

#include "widgets/date_picker.h"
#include "widgets/select.h"
#include "widgets/image.h"
#include "widgets/button.h"
#include "snow_shot/storage/storageusagetracker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QUuid>
#include <QLabel>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

class MutableHistoryDataSource final : public ScreenshotHistoryPageDataSource {
  public:
    using ScreenshotHistoryPageDataSource::ScreenshotHistoryPageDataSource;

    QVector<storage::CaptureHistoryRecord> records() const override {
        return m_records;
    }

    std::optional<storage::CaptureHistoryAssetSet>
    displayAssets(const storage::CaptureHistoryRecord&) const override {
        return std::nullopt;
    }

    void remove(const QString&) override {}
    void reportReadFailure(const storage::CaptureHistoryRecord&, const QString&) override {
        ++readFailures;
    }
    int readFailures = 0;
    bool requestClear() override {
        return true;
    }

    void setRecords(QVector<storage::CaptureHistoryRecord> records) {
        m_records = std::move(records);
    }

  private:
    QVector<storage::CaptureHistoryRecord> m_records;
};

void emptyStateRemainsVisibleAfterFilteringEmptyHistory() {
    MutableHistoryDataSource dataSource;
    ScreenshotHistoryPageWidget page(&dataSource, nullptr);
    page.resize(720, 600);
    page.show();
    page.setActive(true);
    flushEvents();

    auto* title = page.findChild<QLabel*>(QStringLiteral("screenshotHistoryEmptyTitle"));
    auto* description =
        page.findChild<QLabel*>(QStringLiteral("screenshotHistoryEmptyDescription"));
    auto* icon = page.findChild<QLabel*>(QStringLiteral("screenshotHistoryEmptyIcon"));
    auto* sourceFilter =
        page.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenshotHistorySourceFilter"));
    auto* dateFilter = page.findChild<adqt::widgets::AdDateRangePicker*>(
        QStringLiteral("screenshotHistoryDateRangeFilter"));
    require(title != nullptr && description != nullptr && icon != nullptr &&
                sourceFilter != nullptr && dateFilter != nullptr,
            "screenshot history page must expose its empty state and filters");
    require(page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryDeleteAll"))
                ->isEnabled(),
            "empty history must allow explicit cleanup of unmanaged files or a broken index");
    const auto requireCompleteEmptyState = [title, description, icon](const char* message) {
        require(title->isVisible() && description->isVisible() && icon->isVisible(), message);
        require(title->text() == QStringLiteral("No screenshot history") &&
                    description->text() ==
                        QStringLiteral("Copied and pinned screenshots will appear here"),
                "an empty repository must retain its unfiltered empty-state wording");
    };
    requireCompleteEmptyState(
        "an empty screenshot history must display the complete empty-state prompt");

    sourceFilter->setCurrentValues({QStringLiteral("clipboard")});
    flushEvents();
    requireCompleteEmptyState(
        "source filtering an empty screenshot history must preserve the empty-state prompt");

    const QDate today = QDate::currentDate();
    dateFilter->setRange(today, today);
    flushEvents();
    requireCompleteEmptyState(
        "date filtering an empty screenshot history must preserve the empty-state prompt");

    page.refresh();
    flushEvents();
    requireCompleteEmptyState(
        "refreshing filtered empty screenshot history must preserve the empty-state prompt");

    storage::CaptureHistoryRecord nonMatchingRecord;
    nonMatchingRecord.id = QStringLiteral("pinned-record");
    nonMatchingRecord.createdUtc = QDateTime::currentDateTimeUtc();
    nonMatchingRecord.source = storage::CaptureHistorySource::PinnedToScreen;
    dataSource.setRecords({nonMatchingRecord});
    page.refresh();
    flushEvents();
    require(title->isVisible() && description->isVisible() && icon->isVisible(),
            "a non-empty history with no filtered matches must display the empty-state prompt");
    require(title->text() == QStringLiteral("No matching screenshots") &&
                description->text() ==
                    QStringLiteral("Change the source or date range to see more history"),
            "a reused empty layout must update its prompt when repository state changes");

    dataSource.setRecords({});
    page.refresh();
    flushEvents();
    requireCompleteEmptyState(
        "an emptied repository must restore the unfiltered empty-state prompt");
}

void waitUntil(const std::function<bool()>& complete, const char* message) {
    QElapsedTimer deadline;
    deadline.start();
    while (!complete() && deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    require(complete(), message);
}

void imageFailuresRespectCacheFallbackAndCancellation() {
    QTemporaryDir temporary;
    MutableHistoryDataSource dataSource;
    storage::CaptureHistoryRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto* loader = createScreenshotHistoryImageLoader(record, &dataSource, &dataSource);
    const QString source = temporary.filePath(QStringLiteral("display_0.png"));
    QImage image(100, 80, QImage::Format_RGB32);
    image.fill(Qt::green);
    require(image.save(source), "failed to write image fixture");
    adqt::widgets::AdImageLoadOptions options;
    options.targetPixelSize = QSize(26, 16);
    auto* first = loader->load(QUrl::fromLocalFile(source), options, &dataSource);
    waitUntil([&]() { return first->isFinished() && screenshotHistoryPendingJobCount() == 0; },
              "thumbnail cache write did not finish");
    require(first->isSuccessful() && dataSource.readFailures == 0, "valid thumbnail failed");
    QString key = record.id + u'|' + QDir::fromNativeSeparators(source);
    key += QStringLiteral("|%1x%2|%3|%4")
               .arg(options.targetPixelSize.width())
               .arg(options.targetPixelSize.height())
               .arg(static_cast<int>(options.aspectRatioMode))
               .arg(options.allowUpscale ? 1 : 0);
    const QString cache =
        QDir(storage::StorageUsageTracker::defaultThumbnailCacheDirectory())
            .filePath(
                QString::fromLatin1(
                    QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) +
                QStringLiteral(".png"));
    QFile file(cache);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write("broken") == 6,
            "failed to corrupt thumbnail cache fixture");
    file.close();
    auto* retry = loader->load(QUrl::fromLocalFile(source), options, &dataSource);
    waitUntil([&]() { return retry->isFinished() && screenshotHistoryPendingJobCount() == 0; },
              "cache fallback did not finish");
    require(retry->isSuccessful() && dataSource.readFailures == 0,
            "cache failure invalidated history instead of loading the original");
    auto* cancelled = loader->load(
        QUrl::fromLocalFile(temporary.filePath(QStringLiteral("cancelled.png"))), {}, &dataSource);
    cancelled->abort();
    flushEvents();
    require(dataSource.readFailures == 0, "cancellation invalidated history");
    auto* missing = loader->load(
        QUrl::fromLocalFile(temporary.filePath(QStringLiteral("missing.png"))), {}, &dataSource);
    waitUntil([&]() { return missing->isFinished(); }, "missing original read did not finish");
    require(!missing->isSuccessful() && dataSource.readFailures == 1,
            "original read failure was not reported exactly once");
    QFile::remove(cache);
}

QString thumbnailCachePath(const storage::CaptureHistoryRecord& record, const QUrl& source,
                           const adqt::widgets::AdImageLoadOptions& options) {
    QString key = record.id + u'|' + QDir::fromNativeSeparators(source.toLocalFile());
    key += QStringLiteral("|%1x%2|%3|%4")
               .arg(options.targetPixelSize.width())
               .arg(options.targetPixelSize.height())
               .arg(static_cast<int>(options.aspectRatioMode))
               .arg(options.allowUpscale ? 1 : 0);
    return QDir(storage::StorageUsageTracker::defaultThumbnailCacheDirectory())
        .filePath(QString::fromLatin1(
                      QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) +
                  QStringLiteral(".png"));
}

// Must run last: shutdownScreenshotHistoryTasks() parks the process-wide
// history executor for the remainder of the test binary.
void shutdownDrainsBacklogThenRejectsNewWork() {
    QTemporaryDir temporary;
    storage::CaptureHistoryRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    MutableHistoryDataSource dataSource;
    auto* loader = createScreenshotHistoryImageLoader(record, &dataSource, &dataSource);
    adqt::widgets::AdImageLoadOptions options;
    options.targetPixelSize = QSize(260, 156);

    constexpr int kBacklogSources = 12;
    QVector<adqt::widgets::AdImageReply*> replies;
    for (int index = 0; index < kBacklogSources; ++index) {
        const QString source = temporary.filePath(QStringLiteral("drain_%1.png").arg(index));
        QImage image(512, 512, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::green);
        require(image.save(source), "failed to write drain fixture");
        replies.push_back(loader->load(QUrl::fromLocalFile(source), options, &dataSource));
    }
    waitUntil(
        [&]() {
            for (adqt::widgets::AdImageReply* reply : replies) {
                if (!reply->isFinished()) {
                    return false;
                }
            }
            return true;
        },
        "drain fixture loads must finish");

    shutdownScreenshotHistoryTasks();
    require(screenshotHistoryPendingJobCount() == 0,
            "shutdown must return only after queued and active history jobs finish");
    for (int index = 0; index < kBacklogSources; ++index) {
        const QString source = temporary.filePath(QStringLiteral("drain_%1.png").arg(index));
        require(QFile::exists(thumbnailCachePath(record, QUrl::fromLocalFile(source), options)),
                "shutdown must drain queued thumbnail persistence jobs, not drop them");
    }

    const QString rejectedSource = temporary.filePath(QStringLiteral("rejected.png"));
    QImage image(512, 512, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    require(image.save(rejectedSource), "failed to write rejection fixture");
    auto* rejected = loader->load(QUrl::fromLocalFile(rejectedSource), options, &dataSource);
    waitUntil([&]() { return rejected->isFinished(); }, "post-shutdown load must still finish");
    flushEvents();
    require(screenshotHistoryPendingJobCount() == 0,
            "submissions after shutdown must be rejected instead of queued");
    require(
        !QFile::exists(thumbnailCachePath(record, QUrl::fromLocalFile(rejectedSource), options)),
        "rejected submissions must not execute after shutdown");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary storage directory must be available");
    require(storage::ApplicationStorage::instance()
                .initialize({temporary.path(), temporary.path(), 8000})
                .success,
            "isolated application storage must initialize");
    emptyStateRemainsVisibleAfterFilteringEmptyHistory();
    imageFailuresRespectCacheFallbackAndCancellation();
    shutdownDrainsBacklogThenRejectsNewWork();
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
