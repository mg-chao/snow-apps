#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h"
#include "snow_shot/storage/applicationstorage.h"

#include "widgets/date_picker.h"
#include "widgets/select.h"
#include "widgets/image.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/pagination.h"
#include "widgets/context_menu.h"
#include "widgets/popconfirm.h"
#include "widgets/scroll_area.h"
#include "snow_shot/storage/storageusagetracker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QFrame>
#include <QImage>
#include <QPainter>
#include <QRegion>
#include <QScopeGuard>
#include <QCryptographicHash>
#include <QUuid>
#include <QLabel>
#include <QLayout>
#include <QTemporaryDir>
#include <QTranslator>

#include <algorithm>
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
        ++displayAssetRequests;
        return std::nullopt;
    }

    void remove(const QString& id) override {
        removedIds.push_back(id);
    }
    QVector<QString> removedIds;
    bool requestRemoveMany(const QVector<QString>& ids) override {
        removedBatches.push_back(ids);
        return acceptRemoval;
    }
    void reportReadFailure(const storage::CaptureHistoryRecord&, const QString&) override {
        ++readFailures;
    }
    int readFailures = 0;
    mutable int displayAssetRequests = 0;
    QVector<QVector<QString>> removedBatches;
    bool acceptRemoval = true;
    bool requestClear() override {
        return true;
    }

    void setRecords(QVector<storage::CaptureHistoryRecord> records) {
        m_records = std::move(records);
    }

  private:
    QVector<storage::CaptureHistoryRecord> m_records;
};

QVector<storage::CaptureHistoryRecord> historyRecords(int count) {
    QVector<storage::CaptureHistoryRecord> records;
    records.reserve(count);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int index = 0; index < count; ++index) {
        storage::CaptureHistoryRecord record;
        record.id = QStringLiteral("record-%1").arg(index);
        record.createdUtc = now.addSecs(-index);
        record.source = index % 2 == 0 ? storage::CaptureHistorySource::CopiedToClipboard
                                       : storage::CaptureHistorySource::PinnedToScreen;
        record.selection.rectangle = QRect(10 + index, 20 + index, 320, 180);
        record.totalBytes = 1024 + index;
        records.push_back(record);
    }
    return records;
}

QList<adqt::widgets::AdCheckbox*> entryCheckboxes(ScreenshotHistoryPageWidget& page) {
    return page.findChildren<adqt::widgets::AdCheckbox*>(
        QStringLiteral("screenshotHistoryEntrySelection"));
}

QString entryId(adqt::widgets::AdCheckbox* checkbox) {
    QWidget* entry = checkbox != nullptr && checkbox->parentWidget() != nullptr
                         ? checkbox->parentWidget()->parentWidget()
                         : nullptr;
    const QString prefix = QStringLiteral("screenshotHistoryEntry-");
    return entry != nullptr && entry->objectName().startsWith(prefix)
               ? entry->objectName().mid(prefix.size())
               : QString();
}

int verticalLayoutGap(QWidget& root, QWidget* upper, QWidget* lower) {
    const int upperBottom = upper->mapTo(&root, QPoint(0, upper->height())).y();
    const int lowerTop = lower->mapTo(&root, QPoint()).y();
    return lowerTop - upperBottom;
}

int colorDistance(const QColor& first, const QColor& second) {
    return std::max({std::abs(first.red() - second.red()), std::abs(first.green() - second.green()),
                     std::abs(first.blue() - second.blue()),
                     std::abs(first.alpha() - second.alpha())});
}

void requireUniformEntryBorder(QWidget* entry) {
    constexpr qreal devicePixelRatio = 2.0;
    const QSize pixelSize(qRound(entry->width() * devicePixelRatio),
                          qRound(entry->height() * devicePixelRatio));
    QImage rendered(pixelSize, QImage::Format_ARGB32_Premultiplied);
    rendered.setDevicePixelRatio(devicePixelRatio);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    entry->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    painter.end();

    const int centerX = rendered.width() / 2;
    const int centerY = rendered.height() / 2;
    constexpr int profileDepth = 8;
    for (int offset = 0; offset < profileDepth; ++offset) {
        const QColor top = rendered.pixelColor(centerX, offset);
        const QColor bottom = rendered.pixelColor(centerX, rendered.height() - 1 - offset);
        const QColor left = rendered.pixelColor(offset, centerY);
        const QColor right = rendered.pixelColor(rendered.width() - 1 - offset, centerY);
        require(colorDistance(top, bottom) <= 2 && colorDistance(top, left) <= 2 &&
                    colorDistance(top, right) <= 2,
                "history entry borders must have uniform device-pixel stroke profiles");
    }

    const QColor stroke = rendered.pixelColor(centerX, qRound(devicePixelRatio));
    const QColor fill = rendered.pixelColor(centerX, qRound(8.0 * devicePixelRatio));
    require(colorDistance(stroke, fill) > 2,
            "history entries must visibly paint a border around the container fill");
}

void entriesUseBordersAndSupportCrossPageSelection() {
    MutableHistoryDataSource dataSource;
    const QVector<storage::CaptureHistoryRecord> records = historyRecords(12);
    dataSource.setRecords(records);
    ScreenshotHistoryPageWidget page(&dataSource, nullptr);
    page.resize(900, 720);
    page.show();
    page.setActive(true);
    flushEvents();

    auto checkboxes = entryCheckboxes(page);
    require(checkboxes.size() == 10, "the first history page must expose ten title checkboxes");
    require(page.findChildren<QWidget*>(QStringLiteral("screenshotHistoryEntryDivider")).isEmpty(),
            "history entries must not retain divider widgets after restoring record borders");
    for (adqt::widgets::AdCheckbox* checkbox : checkboxes) {
        QWidget* entry = checkbox->parentWidget()->parentWidget();
        auto* frame = qobject_cast<QFrame*>(entry);
        require(frame != nullptr && frame->frameShape() == QFrame::NoFrame,
                "history entry borders must be custom-painted instead of using QFrame strokes");
        require(!checkbox->text().isEmpty() && !entryId(checkbox).isEmpty(),
                "history title checkboxes must retain the timestamp and record identity");
    }
    requireUniformEntryBorder(checkboxes.first()->parentWidget()->parentWidget());

    auto* filters = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryFilters"));
    auto* selectionBar = page.findChild<QWidget*>(QStringLiteral("screenshotHistorySelectionBar"));
    auto* selectionPanel =
        page.findChild<QWidget*>(QStringLiteral("screenshotHistorySelectionPanel"));
    auto* selectionActions =
        page.findChild<QWidget*>(QStringLiteral("screenshotHistorySelectionActions"));
    auto* summary = page.findChild<QLabel*>(QStringLiteral("screenshotHistorySelectionSummary"));
    auto* deleteSelected =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryDeleteSelected"));
    auto* selectAll =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistorySelectAll"));
    auto* deselectAll =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryDeselectAll"));
    auto* pagination =
        page.findChild<adqt::widgets::AdPagination*>(QStringLiteral("screenshotHistoryPagination"));
    auto* sourceFilter =
        page.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenshotHistorySourceFilter"));
    auto* dateFilter = page.findChild<adqt::widgets::AdDateRangePicker*>(
        QStringLiteral("screenshotHistoryDateRangeFilter"));
    require(filters != nullptr && selectionBar != nullptr && selectionPanel != nullptr &&
                selectionActions != nullptr && summary != nullptr && deleteSelected != nullptr &&
                selectAll != nullptr && deselectAll != nullptr && pagination != nullptr &&
                sourceFilter != nullptr && dateFilter != nullptr,
            "history selection controls must be discoverable");
    require(!selectionBar->isVisible(), "selection bar must be hidden before selection");
    QWidget* firstEntry = checkboxes.first()->parentWidget()->parentWidget();
    require(verticalLayoutGap(page, filters, firstEntry) > 0,
            "history entries must retain a gap below the filters when selection is empty");
    require(selectionActions->layout() != nullptr && selectionActions->layout()->spacing() == 0,
            "selection actions must not add redundant spacing between link buttons");
    require(deleteSelected->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Link &&
                deleteSelected->accentRole() == adqt::widgets::AdButton::AccentRole::Danger &&
                selectAll->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Link &&
                deselectAll->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Link,
            "selection actions must use link styling with a danger Delete action");

    adqt::widgets::AdCheckbox* firstCheckbox = checkboxes.first();
    const int assetRequestsBeforeSelection = dataSource.displayAssetRequests;
    auto* scrollArea =
        page.findChild<adqt::widgets::AdScrollArea*>(QStringLiteral("screenshotHistoryScrollArea"));
    require(scrollArea != nullptr && scrollArea->verticalScrollBar()->maximum() > 0,
            "selection performance test requires a scrollable history page");
    scrollArea->verticalScrollBar()->setValue(
        std::min(100, scrollArea->verticalScrollBar()->maximum()));
    const int scrollPositionBeforeSelection = scrollArea->verticalScrollBar()->value();
    firstCheckbox->click();
    flushEvents();
    require(selectionBar->isVisible() && summary->text() == QStringLiteral("Selected 1 item"),
            "selecting one entry must show the singular selection summary");
    auto* countLabel = page.findChild<QLabel*>(QStringLiteral("screenshotHistoryCountLabel"));
    require(countLabel != nullptr &&
                countLabel->font().pixelSize() == summary->font().pixelSize() &&
                countLabel->font().weight() == summary->font().weight(),
            "selected history records keep the same text style as the page subtitle");
    require(verticalLayoutGap(page, filters, selectionPanel) > 0 &&
                verticalLayoutGap(page, selectionPanel, firstEntry) > 0,
            "the selection bar must preserve gaps below the filters and above the first entry");
    require(entryCheckboxes(page).first() == firstCheckbox &&
                dataSource.displayAssetRequests == assetRequestsBeforeSelection &&
                scrollArea->verticalScrollBar()->value() == scrollPositionBeforeSelection,
            "selection must not recreate entries, request assets, or reset scrolling");

    QTranslator chineseTranslator;
    require(chineseTranslator.load(QStringLiteral(":/i18n/snow_shot_zh_CN.qm")),
            "load the Simplified Chinese history translations");
    QCoreApplication::installTranslator(&chineseTranslator);
    flushEvents();
    QString translatedSummary = chineseTranslator.translate("ScreenshotHistoryPageWidget",
                                                            "Selected %n item(s)", nullptr, 1);
    translatedSummary.replace(QStringLiteral("%n"), QStringLiteral("1"));
    require(summary->text() == translatedSummary && firstCheckbox->isChecked(),
            "language changes must retranslate the summary without changing selection");
    QCoreApplication::removeTranslator(&chineseTranslator);
    flushEvents();
    require(summary->text() == QStringLiteral("Selected 1 item") && firstCheckbox->isChecked(),
            "removing a translator must restore English without changing selection");

    selectAll->click();
    flushEvents();
    require(summary->text() == QStringLiteral("Selected 10 items") && !selectAll->isEnabled(),
            "Select all must add only the current page and disable when that page is complete");

    pagination->setCurrentPage(2);
    flushEvents();
    checkboxes = entryCheckboxes(page);
    require(checkboxes.size() == 2 && summary->text() == QStringLiteral("Selected 10 items") &&
                selectionBar->isVisible(),
            "page navigation must carry selection while rendering only the new page");
    require(!checkboxes.first()->isChecked() && !checkboxes.last()->isChecked(),
            "Select all on page one must not select page two");
    checkboxes.first()->click();
    flushEvents();
    require(summary->text() == QStringLiteral("Selected 11 items"),
            "cross-page selection count must include the newly selected entry");

    pagination->setCurrentPage(1);
    flushEvents();
    checkboxes = entryCheckboxes(page);
    require(std::all_of(checkboxes.cbegin(), checkboxes.cend(),
                        [](const auto* checkbox) { return checkbox->isChecked(); }),
            "returning to a page must restore its selected checkboxes");
    pagination->setPageSize(20);
    flushEvents();
    require(summary->text() == QStringLiteral("Selected 11 items"),
            "page-size changes must preserve cross-page selection");

    deselectAll->click();
    flushEvents();
    checkboxes = entryCheckboxes(page);
    require(!selectionBar->isVisible() &&
                std::none_of(checkboxes.cbegin(), checkboxes.cend(),
                             [](const auto* checkbox) { return checkbox->isChecked(); }),
            "Deselect all must clear the complete cross-page selection");

    entryCheckboxes(page).first()->click();
    sourceFilter->setCurrentValues({QStringLiteral("clipboard")});
    flushEvents();
    require(!selectionBar->isVisible(), "changing the source filter must clear selection");
    sourceFilter->setCurrentValues({});
    flushEvents();
    entryCheckboxes(page).first()->click();
    dateFilter->setRange(QDate::currentDate(), QDate::currentDate());
    flushEvents();
    require(!selectionBar->isVisible(), "changing the date filter must clear selection");

    dateFilter->clear();
    flushEvents();
    pagination->setPageSize(10);
    pagination->setCurrentPage(1);
    flushEvents();
    checkboxes = entryCheckboxes(page);
    const QString removedSelectionId = entryId(checkboxes.first());
    checkboxes.first()->click();
    pagination->setCurrentPage(2);
    flushEvents();
    entryCheckboxes(page).first()->click();
    require(summary->text() == QStringLiteral("Selected 2 items"),
            "refresh-pruning fixture must begin with two selected records");
    QVector<storage::CaptureHistoryRecord> reducedRecords = records;
    reducedRecords.removeIf([&](const auto& record) { return record.id == removedSelectionId; });
    dataSource.setRecords(reducedRecords);
    page.refresh();
    flushEvents();
    require(summary->text() == QStringLiteral("Selected 1 item"),
            "refresh must prune selected IDs that no longer exist");

    deselectAll->click();
    dataSource.setRecords(records);
    page.refresh();
    pagination->setPageSize(10);
    pagination->setCurrentPage(1);
    flushEvents();
    selectAll->click();
    pagination->setCurrentPage(2);
    flushEvents();
    entryCheckboxes(page).first()->click();
    auto* confirmation = page.findChild<adqt::widgets::AdPopconfirm*>(
        QStringLiteral("screenshotHistoryDeleteSelectedConfirm"));
    require(confirmation != nullptr &&
                confirmation->text() == QStringLiteral("Delete 11 selected items?"),
            "bulk deletion confirmation must describe the complete cross-page selection");
    QMetaObject::invokeMethod(confirmation, "rejected", Qt::DirectConnection);
    require(dataSource.removedBatches.isEmpty(), "canceling bulk deletion must submit no batch");
    dataSource.acceptRemoval = false;
    QMetaObject::invokeMethod(confirmation, "accepted", Qt::DirectConnection);
    flushEvents();
    require(dataSource.removedBatches.size() == 1 && selectionBar->isVisible() &&
                summary->text() == QStringLiteral("Selected 11 items"),
            "a synchronously rejected batch must preserve the complete selection");
    dataSource.removedBatches.clear();
    dataSource.acceptRemoval = true;
    QMetaObject::invokeMethod(confirmation, "accepted", Qt::DirectConnection);
    flushEvents();
    require(dataSource.removedBatches.size() == 1 && dataSource.removedBatches.first().size() == 11,
            "confirming bulk deletion must submit exactly one complete batch");
    for (qsizetype index = 0; index < dataSource.removedBatches.first().size(); ++index) {
        require(dataSource.removedBatches.first()[index] == records[index].id,
                "bulk deletion IDs must follow deterministic repository order");
    }
    require(!selectionBar->isVisible(),
            "an accepted bulk deletion request must clear the selection bar");
}

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

void pageTextAndEmptyStateMatchPinnedWindowManagement() {
    MutableHistoryDataSource dataSource;
    ScreenshotHistoryPageWidget historyPage(&dataSource, nullptr);
    PinnedWindowManagementPageWidget pinnedPage;
    auto* historyIcon =
        historyPage.findChild<QLabel*>(QStringLiteral("screenshotHistoryEmptyIcon"));
    auto* pinnedIcon = pinnedPage.findChild<QLabel*>(QStringLiteral("pinnedManagementEmptyIcon"));
    require(historyIcon != nullptr && pinnedIcon != nullptr,
            "both management pages must expose their empty-state icons");
    auto* historyCount =
        historyPage.findChild<QLabel*>(QStringLiteral("screenshotHistoryCountLabel"));
    auto* pinnedCount = pinnedPage.findChild<QLabel*>(QStringLiteral("pinnedManagementCountLabel"));
    auto* historySelection =
        historyPage.findChild<QLabel*>(QStringLiteral("screenshotHistorySelectionSummary"));
    auto* pinnedSelection =
        pinnedPage.findChild<QLabel*>(QStringLiteral("pinnedManagementSelectionSummary"));
    require(historyCount != nullptr && pinnedCount != nullptr && historySelection != nullptr &&
                pinnedSelection != nullptr,
            "both management pages must expose their count and selection labels");

    for (const auto appearance : {snow_shot::presentation::styles::ThemeAppearance::Light,
                                  snow_shot::presentation::styles::ThemeAppearance::Dark}) {
        snow_shot::presentation::styles::ThemeStyleConfig config;
        config.appearance = appearance;
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme(config);
        historyPage.applyTheme(scheme);
        pinnedPage.applyTheme(scheme);
        require(!historyIcon->pixmap().isNull() &&
                    historyIcon->pixmap().toImage() == pinnedIcon->pixmap().toImage(),
                "the empty-state icon must use identical colors on both pages in each theme");
        for (QLabel* label : {historyCount, pinnedCount, historySelection, pinnedSelection}) {
            require(
                label->font().pixelSize() == scheme.metricAlias.fontSize &&
                    label->font().weight() == QFont::Normal &&
                    label->palette().color(QPalette::WindowText) == scheme.map.colorTextSecondary,
                "both page subtitles and selection summaries must use the same theme text style");
        }
    }
}

void moreMenuOffersPinAndDelete() {
    MutableHistoryDataSource dataSource;
    QVector<storage::CaptureHistoryRecord> records = historyRecords(2);
    storage::CaptureHistoryResultRecord result;
    result.imageSize = QSize(32, 18);
    result.encodedBytes = 256;
    records[0].result = result;
    dataSource.setRecords(records);

    ScreenshotHistoryPageWidget page(&dataSource, nullptr);
    QString pinnedId;
    QObject::connect(&page, &ScreenshotHistoryPageWidget::pinRequested, &page,
                     [&pinnedId](const QString& recordId) { pinnedId = recordId; });
    page.resize(900, 720);
    page.show();
    page.setActive(true);
    flushEvents();
    const QPoint previousCursor = QCursor::pos();
    const auto restoreCursor = qScopeGuard([previousCursor] { QCursor::setPos(previousCursor); });

    auto* pinnableEntry =
        page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-0"));
    auto* plainEntry = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-1"));
    require(pinnableEntry != nullptr && plainEntry != nullptr,
            "history entries must keep their record identity");

    auto* more = pinnableEntry->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotHistoryEntryMore"));
    auto* confirmation = pinnableEntry->findChild<adqt::widgets::AdPopconfirm*>(
        QStringLiteral("screenshotHistoryEntryDeleteConfirm"));
    require(more != nullptr && more->text() == QStringLiteral("More") && more->isVisible() &&
                confirmation != nullptr && confirmation->sourceWidget() == more,
            "each history entry must replace Delete with a More button");
    require(pinnableEntry->findChild<adqt::widgets::AdContextMenu*>(
                QStringLiteral("screenshotHistoryEntryMoreMenu")) == nullptr,
            "the More menu must stay closed until it is opened");
    require(
        more->parentWidget()
            ->findChildren<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryEntryDelete"))
            .isEmpty(),
        "the entry action row must not contain Delete");

    auto visibleMenu = [](QWidget* entry) -> adqt::widgets::AdContextMenu* {
        const auto menus = entry->findChildren<adqt::widgets::AdContextMenu*>(
            QStringLiteral("screenshotHistoryEntryMoreMenu"));
        for (adqt::widgets::AdContextMenu* candidate : menus) {
            if (candidate->isPopupVisible()) {
                return candidate;
            }
        }
        return nullptr;
    };
    auto hover = [](QWidget* widget) {
        const QPoint local = widget->rect().center();
        const QPoint global = widget->mapToGlobal(local);
        QCursor::setPos(global);
        QEnterEvent event(local, local, global);
        QApplication::sendEvent(widget, &event);
    };
    auto activate = [](QMenu* menu, QAction* action) {
        const QRect geometry = menu->actionGeometry(action);
        const QPoint local = geometry.center();
        const QPoint global = menu->mapToGlobal(local);
        QCursor::setPos(global);
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(menu, &press);
        QApplication::sendEvent(menu, &release);
    };
    hover(more);
    flushEvents();
    auto* menu = visibleMenu(pinnableEntry);
    require(menu != nullptr && menu->actions().size() == 2 && !menu->nativeMenuEnabled(),
            "hovering More must show the shared widget action menu");
    QAction* pin = menu->actions().at(0);
    QAction* remove = menu->actions().at(1);
    require(pin->text() == QStringLiteral("Pin to screen") && pin->isEnabled() &&
                remove->text() == QStringLiteral("Delete") && menu->actionDanger(remove) &&
                menu->actionGeometry(pin).isValid(),
            "the More menu must offer Pin to screen and a danger Delete action");
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(pinnableEntry, &languageChange);
    flushEvents();
    require(pin->text() == QStringLiteral("Pin to screen") &&
                pin->toolTip() == QStringLiteral("Pin this screenshot to the screen") &&
                remove->text() == QStringLiteral("Delete"),
            "an open More menu must keep its own action labels when the language changes");
    activate(menu, pin);
    flushEvents();
    require(pinnedId == QStringLiteral("record-0") && !menu->isPopupVisible(),
            "Pin to screen must request that history entry and close the menu");

    auto* plainMore = plainEntry->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotHistoryEntryMore"));
    require(plainMore != nullptr, "every history entry must expose More");
    hover(plainMore);
    flushEvents();
    auto* plainMenu = visibleMenu(plainEntry);
    require(plainMenu != nullptr && !plainMenu->actions().at(0)->isEnabled() &&
                plainMenu->actions().at(0)->toolTip() ==
                    QStringLiteral("This screenshot cannot be pinned"),
            "Pin to screen must stay unavailable when the history entry has no image");
    plainMenu->dismissPopup();
    flushEvents();

    hover(more);
    flushEvents();
    menu = visibleMenu(pinnableEntry);
    require(menu != nullptr, "More must reopen its action menu");
    activate(menu, menu->actions().at(1));
    flushEvents();
    adqt::widgets::AdButton* confirmDelete =
        confirmation->button(adqt::widgets::AdPopconfirm::StandardButton::Ok);
    require(confirmation->isVisible() && confirmDelete != nullptr && !menu->isPopupVisible(),
            "Delete must close the action menu and ask for confirmation");
    confirmDelete->click();
    flushEvents();
    require(dataSource.removedIds.size() == 1 &&
                dataSource.removedIds.front() == QStringLiteral("record-0"),
            "confirming Delete must remove that history entry");
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
    QTranslator englishTranslator;
    require(englishTranslator.load(QStringLiteral(":/i18n/snow_shot_en_US.qm")),
            "load the English application translations");
    QCoreApplication::installTranslator(&englishTranslator);
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary storage directory must be available");
    require(storage::ApplicationStorage::instance()
                .initialize({temporary.path(), temporary.path(), 8000})
                .success,
            "isolated application storage must initialize");
    emptyStateRemainsVisibleAfterFilteringEmptyHistory();
    pageTextAndEmptyStateMatchPinnedWindowManagement();
    moreMenuOffersPinAndDelete();
    entriesUseBordersAndSupportCrossPageSelection();
    imageFailuresRespectCacheFallbackAndCancellation();
    shutdownDrainsBacklogThenRejectsNewWork();
    storage::ApplicationStorage::instance().shutdown();
    QCoreApplication::removeTranslator(&englishTranslator);
    return 0;
}
