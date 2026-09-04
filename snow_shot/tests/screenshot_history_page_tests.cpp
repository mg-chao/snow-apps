#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/storage/applicationstorage.h"

#include "widgets/date_picker.h"
#include "widgets/select.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
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
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
