#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/presentation/components/historypagecommon.h"
#include "snow_shot/presentation/components/thumbnailcache.h"
#include "snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/historypinplacement.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "../src/presentation/pinned/screenshotclipboardplacementgeometry.h"
#include <QClipboard>
#include <QMimeData>
#include "snowimageqtcodec.h"

#include "widgets/date_picker.h"
#include "theme/theme_manager.h"
#include "widgets/select.h"
#include "widgets/image.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/pagination.h"
#include "widgets/context_menu.h"
#include "widgets/popconfirm.h"
#include "widgets/message.h"
#include "widgets/scroll_area.h"
#include "history_sticky_selection_test_support.h"
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
#include <QColorSpace>
#include <QPainter>
#include <QPointer>
#include <QRegion>
#include <QScopeGuard>
#include <QStringList>
#include <QCryptographicHash>
#include <QUuid>
#include <QLabel>
#include <QLayout>
#include <QBoxLayout>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QTranslator>
#include <QTimer>
#include <QHoverEvent>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

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

void activateMenuAction(QMenu* menu, QAction* action) {
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
}

void recordFiltersPreserveLocalDateBoundaries() {
    using snow_shot::presentation::components::history_page::RecordFilter;
    const QDate day(2026, 10, 4);
    const QDateTime timestamp(day, QTime(0, 30));
    const QVariant source = QStringLiteral("copy");
    const RecordFilter unrestricted;
    require(unrestricted.matches(source, timestamp.toUTC()),
            "unfiltered history includes every source and date");
    const RecordFilter exact{{source}, day, day};
    require(exact.matches(source, timestamp.toUTC()),
            "date boundaries include the selected local day even for UTC records");
    require(exact.matches(source, QDateTime(day, QTime(23, 30)).toUTC()),
            "both ends of the local day are included regardless of their UTC date");
    require(!exact.matches(QStringLiteral("save"), timestamp),
            "source and date restrictions are combined");
    require(!RecordFilter{{}, day.addDays(1), {}}.matches(source, timestamp),
            "an open-ended start date excludes earlier records");
    require(!RecordFilter{{}, {}, day.addDays(-1)}.matches(source, timestamp),
            "an open-ended end date excludes later records");
}

class MutableHistoryDataSource : public ScreenshotHistoryPageDataSource {
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
    QStringList revealedIds;
    bool revealSucceeds = true;
    bool revealRecord(const QString& id) override {
        revealedIds.append(id);
        return revealSucceeds;
    }
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

class SkinBackdrop final : public QWidget {
  public:
    QColor color = QColor(35, 90, 145);

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), color);
    }
};

void selectionPanelSticksWhileScrolling() {
    MutableHistoryDataSource source;
    source.setRecords(historyRecords(55));
    storage::InterfaceSettings settings;
    const int originalPageSize = settings.screenshotHistoryPageSize();
    const auto restorePageSize =
        qScopeGuard([&] { settings.setScreenshotHistoryPageSize(originalPageSize); });
    for (const int pageSize : {10, 20, 50}) {
        require(settings.setScreenshotHistoryPageSize(pageSize), "save history fixture page size");
        ScreenshotHistoryPageWidget page(&source, nullptr);
        require(page.findChild<adqt::widgets::AdPagination*>()->pageSize() == pageSize,
                "sticky history fixture restores the saved page size");
        page.resize(980, 640);
        page.show();
        history_sticky_test::settleLayout();
        auto* row = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-0"));
        require(row != nullptr, "sticky history fixture has an entry");
        history_sticky_test::verifySelection(page, QStringLiteral("screenshotHistory"),
                                             row->findChild<adqt::widgets::AdCheckbox*>(), require);
    }
}

class StyleChangeRecorder final : public QObject {
  public:
    QStringList styles;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::StyleChange) {
            styles.push_back(static_cast<QWidget*>(watched)->styleSheet());
        }
        return false;
    }
};

void entryDescriptionsUseSmallThemeText() {
    MutableHistoryDataSource source;
    source.setRecords(historyRecords(2));
    QWidget host;
    QFont inheritedFont = host.font();
    inheritedFont.setPixelSize(14);
    inheritedFont.setHintingPreference(QFont::PreferNoHinting);
    host.setFont(inheritedFont);
    ScreenshotHistoryPageWidget page(&source, &host);
    host.resize(980, 640);
    page.resize(host.size());
    host.show();
    page.show();
    page.setActive(true);
    flushEvents();

    for (const int fontSize : {14, 20, 14}) {
        snow_shot::presentation::styles::ThemeStyleConfig config;
        config.fontSize = fontSize;
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme(config);
        page.applyTheme(scheme);
        const auto requireDescriptionFonts = [&] {
            for (const auto* name :
                 {"screenshotHistorySourceBadge", "screenshotHistoryEntrySummary",
                  "screenshotHistoryEntryMetadata"}) {
                const auto labels = page.findChildren<QLabel*>(QString::fromLatin1(name));
                require(labels.size() == source.records().size(),
                        "every screenshot entry exposes all three description labels");
                for (const auto* label : labels) {
                    require(label->font().pixelSize() == scheme.metricAlias.fontSizeSM &&
                                label->font().weight() == QFont::Normal &&
                                label->font().hintingPreference() == QFont::PreferNoHinting,
                            "screenshot descriptions use small theme text and inherited hinting");
                }
            }
            const auto timestamps = page.findChildren<adqt::widgets::AdCheckbox*>(
                QStringLiteral("screenshotHistoryEntrySelection"));
            for (const auto* timestamp : timestamps) {
                require(timestamp->font().pixelSize() == scheme.metricAlias.fontSizeLG &&
                            timestamp->font().weight() == QFont::DemiBold,
                        "screenshot timestamps retain their heading text style");
            }
        };
        flushEvents();
        requireDescriptionFonts();
        inheritedFont.setPointSizeF(24);
        inheritedFont.setWeight(QFont::Bold);
        host.setFont(inheritedFont);
        flushEvents();
        requireDescriptionFonts();
        source.setRecords(historyRecords(static_cast<int>(source.records().size()) + 1));
        page.refresh();
        flushEvents();
        requireDescriptionFonts();
    }
}

void historyThemeChangesDoNotRestyleTwice() {
    auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Light);
    MutableHistoryDataSource source;
    source.setRecords(historyRecords(2));
    ScreenshotHistoryPageWidget page(&source, nullptr);
    page.resize(980, 640);
    page.show();
    flushEvents();
    auto* badge = page.findChild<QLabel*>(QStringLiteral("screenshotHistorySourceBadge"));
    require(badge != nullptr, "the history theme regression needs a seeded source badge");
    StyleChangeRecorder recorder;
    badge->installEventFilter(&recorder);
    for (const auto appearance : {snow_shot::presentation::styles::ThemeAppearance::Dark,
                                  snow_shot::presentation::styles::ThemeAppearance::Light}) {
        const QString previous = badge->styleSheet();
        recorder.styles.clear();
        themeManager.setThemeAppearance(appearance);
        require(recorder.styles.size() == 1 && recorder.styles.first() != previous,
                "an unskinned history theme change must apply each source badge exactly once");
    }
    QWidget unrelated;
    adqt::theme::ThemeOverride overrideValue;
    overrideValue.backgroundOpacity = 0.4;
    auto& controlTheme = adqt::theme::ThemeManager::instance();
    recorder.styles.clear();
    controlTheme.setScopeOverride(&unrelated, overrideValue);
    controlTheme.clearScopeOverride(&unrelated);
    require(recorder.styles.isEmpty(),
            "an unrelated skin must not restyle unskinned history source badges");
}

void historyRowsRespectSkinMask() {
    MutableHistoryDataSource source;
    source.setRecords(historyRecords(2));
    SkinBackdrop backdrop;
    backdrop.resize(980, 640);
    ScreenshotHistoryPageWidget page(&source, &backdrop);
    page.resize(backdrop.size());
    backdrop.show();
    page.show();
    flushEvents();
    auto* row = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-0"));
    require(row != nullptr && row->width() > 100, "seeded history mask fixture has a visible row");
    const QColor fill = snow_shot::presentation::styles::ThemeManager::instance()
                            .themeColorScheme()
                            .map.colorBgContainer;
    auto& manager = adqt::theme::ThemeManager::instance();
    for (const qreal opacity : {0.4, 0.0, 1.0}) {
        adqt::theme::ThemeOverride overrideValue;
        overrideValue.backgroundOpacity = opacity;
        manager.setScopeOverride(&backdrop, overrideValue);
        flushEvents();
        QImage rendered(backdrop.size(), QImage::Format_ARGB32_Premultiplied);
        rendered.fill(Qt::transparent);
        backdrop.render(&rendered);
        const QPoint sample = row->mapTo(&backdrop, QPoint(row->width() / 2, 5));
        const QColor actual = rendered.pixelColor(sample);
        const qreal alpha = static_cast<qreal>(fill.alphaF()) * opacity;
        require(std::abs(actual.red() -
                         qRound(fill.red() * alpha + backdrop.color.red() * (1 - alpha))) <= 1 &&
                    std::abs(actual.green() - qRound(fill.green() * alpha +
                                                     backdrop.color.green() * (1 - alpha))) <= 1 &&
                    std::abs(actual.blue() - qRound(fill.blue() * alpha +
                                                    backdrop.color.blue() * (1 - alpha))) <= 1,
                "history row background must paint one mask and expose the backdrop at zero");
        const QString reviewDirectory = qEnvironmentVariable("SNOW_SKIN_PAGE_REVIEW_DIR");
        if (!reviewDirectory.isEmpty()) {
            require(QDir().mkpath(reviewDirectory), "create seeded history review directory");
            require(rendered.save(QDir(reviewDirectory)
                                      .filePath(QStringLiteral("seeded-history-mask-%1.png")
                                                    .arg(qRound(opacity * 100)))),
                    "save seeded history skin review");
        }
    }
    manager.clearScopeOverride(&backdrop);
}

class PreviewHistoryDataSource final : public MutableHistoryDataSource {
  public:
    QHash<QString, storage::CaptureHistoryAssetSet> assets;
    QVector<QString> requested;
    bool supportsAsyncDisplayAssets() const override {
        return true;
    }
    void requestDisplayAssets(const QVector<storage::CaptureHistoryRecord>& records,
                              quint64 generation) override {
        QVector<ScreenshotHistoryAssetResolution> resolutions;
        for (const auto& record : records) {
            requested.push_back(record.id);
            resolutions.push_back({record.id, assets.value(record.id)});
        }
        QTimer::singleShot(0, this, [this, generation, resolutions] {
            emit displayAssetsReady(generation, resolutions);
        });
    }
};

void continuousHistoryPreview() {
    QTemporaryDir directory;
    PreviewHistoryDataSource source;
    auto records = historyRecords(15);
    for (auto& record : records) {
        const QString path = directory.filePath(record.id + QStringLiteral(".png"));
        QImage image(32, 24, QImage::Format_RGB32);
        image.fill(Qt::red);
        require(image.save(path), "save preview fixture");
        record.result = storage::CaptureHistoryResultRecord{image.size(), 100};
        storage::CaptureHistoryAssetSet assets;
        assets.recordId = record.id;
        assets.result =
            storage::CaptureHistoryResultAsset{record.id, image.size(), QUrl::fromLocalFile(path)};
        source.assets.insert(record.id, assets);
    }
    storage::CaptureHistoryDisplayRecord display;
    display.stableId = QStringLiteral("monitor");
    display.name = QStringLiteral("Monitor");
    records.front().displays.push_back(display);
    auto& firstAssets = source.assets[records.front().id];
    firstAssets.displays.push_back({records.front().id, display.stableId, display.name,
                                    QSize(32, 24), firstAssets.result->localFileUrl});
    source.setRecords(records);
    ScreenshotHistoryPageWidget page(&source, nullptr);
    page.resize(980, 640);
    page.setActive(true);
    page.show();
    for (int index = 0; index < 5; ++index)
        flushEvents();
    auto* viewer = page.findChild<adqt::widgets::AdImageViewer*>();
    require(viewer && viewer->rowCount() == 16,
            "history preview includes all filtered records and their display images");
    const auto images =
        page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-0"))
            ->findChildren<adqt::widgets::AdImage*>(QStringLiteral("screenshotHistoryImage"));
    require(images.size() == 2 &&
                std::all_of(images.cbegin(), images.cend(),
                            [viewer](auto* image) { return image->viewer() == viewer; }) &&
                std::any_of(images.cbegin(), images.cend(),
                            [](auto* image) { return image->previewRow() == 0; }) &&
                std::any_of(images.cbegin(), images.cend(),
                            [](auto* image) { return image->previewRow() == 1; }),
            "history thumbnails share the continuous viewer and start at the clicked image");
    viewer->openAt(0);
    viewer->activate(1);
    require(viewer->currentRow() == 1 &&
                viewer->itemAt(1).source.fragment() == QStringLiteral("display:monitor"),
            "history next preserves display previews within a record");
    viewer->activate(1);
    require(viewer->currentRow() == 2 && viewer->itemAt(2).source.path() == records[1].id,
            "history next continues into the next record");
    viewer->activate(-1);
    require(viewer->currentRow() == 1, "history previous returns to the preceding image");
    viewer->openAt(10);
    viewer->activate(1);
    flushEvents();
    require(viewer->currentRow() == 11 && viewer->itemAt(11).source.path() == records[10].id,
            "history next crosses the pagination boundary");
    auto* reply = viewer->imageLoader()->load(viewer->itemAt(11).source, {}, &page);
    QElapsedTimer timer;
    timer.start();
    while (!reply->isFinished() && timer.elapsed() < 5000)
        flushEvents();
    require(reply->isSuccessful() && reply->image().size() == QSize(32, 24) &&
                source.requested.contains(records[10].id),
            "off-page history previews resolve asynchronously and load full images");
    delete reply;
    auto* canceled = viewer->imageLoader()->load(viewer->itemAt(12).source, {}, &page);
    canceled->abort();
    flushEvents();
    require(canceled->isFinished() && !canceled->isSuccessful(),
            "canceled history preview cannot become a successful late reply");
    delete canceled;
    viewer->close();
    auto* pagination = page.findChild<adqt::widgets::AdPagination*>();
    pagination->setCurrentPage(2);
    for (int index = 0; index < 5; ++index)
        flushEvents();
    auto* secondPageImage =
        page.findChild<adqt::widgets::AdImage*>(QStringLiteral("screenshotHistoryImage"));
    require(secondPageImage && secondPageImage->previewRow() == 11,
            "second-page history thumbnail opens its absolute image row");
    auto* filter = page.findChild<adqt::widgets::AdSelect*>();
    filter->setCurrentValues({QStringLiteral("clipboard")});
    flushEvents();
    require(viewer->rowCount() == 9 && viewer->itemAt(2).source.path() == records[2].id,
            "continuous history respects active source filters");
    viewer->openAt(0);
    records.removeFirst();
    source.setRecords(records);
    page.refresh();
    require(!viewer->isVisible() && viewer->rowCount() == 7,
            "history refresh closes stale preview and removes deleted images");
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

void triggerBatchStitch(adqt::widgets::AdButton* trigger) {
    trigger->click();
    QCoreApplication::processEvents();
    auto* menu = trigger->window()->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotHistoryBatchActionsMenu"));
    require(menu && menu->isPopupVisible(), "Batch Actions opens its menu");
    menu->setActiveAction(menu->actions().first());
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(menu, &enter);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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
    auto* batchActions =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryBatchActions"));
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
                selectAll != nullptr && batchActions != nullptr && deselectAll != nullptr &&
                pagination != nullptr && sourceFilter != nullptr && dateFilter != nullptr,
            "history selection controls must be discoverable");
    require(!selectionBar->isVisible(), "selection bar must be hidden before selection");
    require(!batchActions->isVisible(),
            "Batch Actions must be hidden when no history entry is selected");
    QWidget* firstEntry = checkboxes.first()->parentWidget()->parentWidget();
    require(verticalLayoutGap(page, filters, firstEntry) > 0,
            "history entries must retain a gap below the filters when selection is empty");
    require(selectionActions->layout() != nullptr && selectionActions->layout()->spacing() == 0,
            "selection actions must not add redundant spacing between link buttons");
    require(selectionActions->layout()->indexOf(batchActions) + 1 ==
                selectionActions->layout()->indexOf(selectAll),
            "Batch Actions must appear immediately to the left of Select all");
    require(deleteSelected->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Link &&
                deleteSelected->accentRole() == adqt::widgets::AdButton::AccentRole::Danger &&
                selectAll->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Link &&
                batchActions->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Link &&
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
    require(batchActions->isVisible() && batchActions->text() == QStringLiteral("Batch Actions") &&
                !batchActions->toolTip().isEmpty() && !batchActions->accessibleName().isEmpty(),
            "one selected screenshot exposes an accessible Batch Actions menu");
    QVector<QVector<storage::CaptureHistoryRecord>> collageRequests;
    QObject::connect(
        &page, &ScreenshotHistoryPageWidget::collageRequested, &page,
        [&collageRequests](const auto& selected) { collageRequests.append(selected); });
    triggerBatchStitch(batchActions);
    require(collageRequests.size() == 1 && collageRequests.first() == QVector{records.first()},
            "Collage emits the selected complete history record");
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
    triggerBatchStitch(batchActions);
    triggerBatchStitch(batchActions);
    require(collageRequests.size() == 3 && collageRequests.at(1).size() == 11 &&
                collageRequests.at(2) == collageRequests.at(1),
            "each Stitch Image activation independently emits all selections across pages");
    for (qsizetype index = 0; index < collageRequests.at(1).size(); ++index)
        require(collageRequests.at(1).at(index) == records.at(index),
                "collage inputs must preserve history list order and the complete record metadata");
    require(summary->text() == QStringLiteral("Selected 11 items") &&
                checkboxes.first()->isChecked(),
            "opening collages must leave the original history selection unchanged");

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
    require(!batchActions->isVisible(), "clearing history selection hides Batch Actions");

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

void batchPinUsesEligibleSelectionAtActivation() {
    MutableHistoryDataSource source;
    auto records = historyRecords(12);
    for (auto& record : records)
        record.result = storage::CaptureHistoryResultRecord{};
    records[1].result.reset();
    source.setRecords(records);
    ScreenshotHistoryPageWidget page(&source, nullptr);
    page.resize(900, 720);
    page.show();
    page.setActive(true);
    flushEvents();
    auto* trigger =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryBatchActions"));
    auto* pagination =
        page.findChild<adqt::widgets::AdPagination*>(QStringLiteral("screenshotHistoryPagination"));
    auto boxes = entryCheckboxes(page);
    boxes[1]->setChecked(true);
    QStringList pinned;
    bool refreshOnPin = false;
    QObject::connect(&page, &ScreenshotHistoryPageWidget::pinRequested, &page,
                     [&](const QString& id) {
                         pinned.append(id);
                         if (refreshOnPin) {
                             source.setRecords(records);
                             page.refresh();
                         }
                     });
    int stitched = 0;
    QObject::connect(&page, &ScreenshotHistoryPageWidget::collageRequested, &page,
                     [&](const auto&) { ++stitched; });
    QKeyEvent open(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(trigger, &open);
    flushEvents();
    QPointer<adqt::widgets::AdContextMenu> menu = page.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotHistoryBatchActionsMenu"));
    require(menu && menu->isPopupVisible() && !menu->nativeMenuEnabled() &&
                menu->actions().size() == 2 && pinned.isEmpty() && stitched == 0,
            "keyboard opens Batch Actions without dispatching either action");
    auto* stitch = menu->actions()[0];
    auto* pin = menu->actions()[1];
    require(stitch->text() == QStringLiteral("Stitch Image") &&
                pin->text() == QStringLiteral("Pin to screen") && !stitch->icon().isNull() &&
                !pin->icon().isNull() && !pin->isEnabled(),
            "batch menu has ordered themed actions and disables pin for unavailable records");
    boxes[0]->setChecked(true);
    require(pin->isEnabled(), "adding an eligible record enables the open menu pin action");
    boxes[0]->setChecked(false);
    require(!pin->isEnabled(), "removing the last eligible record disables the open pin action");
    boxes[0]->setChecked(true);
    for (const QString& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        QTranslator translator;
        require(translator.load(QStringLiteral(":/i18n/snow_shot_%1.qm").arg(locale)),
                "load batch action translations");
        QCoreApplication::installTranslator(&translator);
        QEvent language(QEvent::LanguageChange);
        QApplication::sendEvent(&page, &language);
        require(trigger->text() ==
                        translator.translate("ScreenshotHistoryPageWidget", "Batch Actions") &&
                    trigger->accessibleName() == trigger->text() &&
                    stitch->text() ==
                        translator.translate("ScreenshotHistoryPageWidget", "Stitch Image") &&
                    pin->text() ==
                        translator.translate("ScreenshotHistoryPageWidget", "Pin to screen") &&
                    pin->toolTip() == pin->text(),
                "language changes translate the trigger and both actions in the open menu");
        QCoreApplication::removeTranslator(&translator);
    }
    QEvent language(QEvent::LanguageChange);
    QApplication::sendEvent(&page, &language);
    pagination->setCurrentPage(2);
    flushEvents();
    entryCheckboxes(page).first()->setChecked(true);
    menu->setActiveAction(pin);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(menu, &enter);
    flushEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(pinned == QStringList{records[0].id, records[10].id},
            "batch pin uses the current cross-page selection and skips unavailable records");
    require(!menu, "batch pin closes and retires its menu");
    require(entryCheckboxes(page).first()->isChecked(), "batch pin retains selection");
    pagination->setCurrentPage(1);
    flushEvents();
    entryCheckboxes(page)[1]->setChecked(false);
    pinned.clear();
    refreshOnPin = true;
    trigger->click();
    flushEvents();
    menu = page.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotHistoryBatchActionsMenu"));
    require(menu && menu->actions()[1]->isEnabled(), "all eligible selections can be pinned again");
    menu->setActiveAction(menu->actions()[1]);
    QApplication::sendEvent(menu, &enter);
    flushEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(pinned == QStringList{records[0].id, records[10].id},
            "synchronous history refresh cannot invalidate the batch pin snapshot");
    page.resize(500, 720);
    flushEvents();
    auto* panel = page.findChild<QWidget*>(QStringLiteral("screenshotHistorySelectionPanel"));
    const QPoint origin = trigger->parentWidget()->mapTo(panel, QPoint());
    require(origin.x() >= 0 && origin.x() + trigger->parentWidget()->width() <= panel->width(),
            "Batch Actions fits the narrow history selection toolbar");
}

class PinnedLayoutDataSource final : public PinnedWindowManagementDataSource {
  public:
    storage::PinnedWindowSummary record;

    QVector<storage::PinnedWindowSummary> records() const override {
        return {record};
    }
    QVector<storage::PinnedWindowGroup> groups() const override {
        return {};
    }
    std::optional<quint64> previewRevision(const QString&) const override {
        return 1;
    }
    void requestPreview(const QString& id, quint64 request, const QSize&) override {
        QImage image(120, 60, QImage::Format_RGB32);
        image.fill(Qt::green);
        emit previewReady(id, request, image, image.size());
    }
    void requestFullImage(const QString&, quint64) override {}
    void showRecord(const QString&) override {}
    void showRecords(const QStringList&) override {}
    void removeRecords(const QVector<QString>&) override {}
};

void pageSizesRememberedIndependently(const QString& storageDirectory) {
    MutableHistoryDataSource historySource;
    historySource.setRecords(historyRecords(55));
    PinnedLayoutDataSource pinnedSource;
    pinnedSource.record.id = QStringLiteral("page-size-pin");
    pinnedSource.record.createdUtc = historySource.records().first().createdUtc;
    int previousHistorySize = 10;
    int previousPinnedSize = 10;

    for (const auto sizes : {std::pair{20, 50}, std::pair{50, 20}, std::pair{10, 10}}) {
        {
            ScreenshotHistoryPageWidget historyPage(&historySource, nullptr);
            PinnedWindowManagementPageWidget pinnedPage(&pinnedSource, nullptr);
            auto* historyPagination = historyPage.findChild<adqt::widgets::AdPagination*>();
            auto* pinnedPagination = pinnedPage.findChild<adqt::widgets::AdPagination*>();
            require(historyPagination->pageSize() == previousHistorySize &&
                        pinnedPagination->pageSize() == previousPinnedSize,
                    "each management page restores its own previous page size");
            historyPagination->setPageSize(sizes.first);
            require(pinnedPagination->pageSize() == previousPinnedSize,
                    "changing history page size leaves pinned management unchanged");
            pinnedPagination->setPageSize(sizes.second);
            historyPage.setActive(true);
            historyPage.refresh();
            pinnedPage.refresh();
            require(historyPagination->pageSize() == sizes.first &&
                        pinnedPagination->pageSize() == sizes.second,
                    "refresh preserves both selected page sizes");
            require(historyPage
                            .findChildren<adqt::widgets::AdCheckbox*>(
                                QStringLiteral("screenshotHistoryEntrySelection"))
                            .size() == sizes.first,
                    "history renders the selected number of entries");
        }

        storage::ApplicationStorage::instance().shutdown();
        require(storage::ApplicationStorage::instance()
                    .initialize({storageDirectory, storageDirectory, 8000})
                    .success,
                "reload persisted management page preferences");
        {
            ScreenshotHistoryPageWidget historyPage(&historySource, nullptr);
            PinnedWindowManagementPageWidget pinnedPage(&pinnedSource, nullptr);
            require(historyPage.findChild<adqt::widgets::AdPagination*>()->pageSize() ==
                            sizes.first &&
                        pinnedPage.findChild<adqt::widgets::AdPagination*>()->pageSize() ==
                            sizes.second,
                    "both management page sizes survive storage shutdown and reload");
        }
        previousHistorySize = sizes.first;
        previousPinnedSize = sizes.second;
    }

    auto& configuration = storage::ApplicationStorage::instance().configuration();
    for (const auto& key : {QStringLiteral("interface/screenshot_history_page_size"),
                            QStringLiteral("interface/pinned_window_management_page_size")}) {
        for (const auto& value : {QJsonValue(0), QJsonValue(15), QJsonValue(100), QJsonValue(20.5),
                                  QJsonValue(QStringLiteral("20")), QJsonValue(QJsonValue::Null)}) {
            require(!configuration.setValue(key, value) && configuration.value(key).toInt() == 10,
                    "unsupported page sizes and invalid types preserve the saved preference");
        }
    }
}

void entryLayoutsMatchAcrossManagementPages() {
    auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    const auto originalThemeMode = themeManager.themeMode();
    const auto restoreTheme = qScopeGuard([&] { themeManager.setThemeMode(originalThemeMode); });
    MutableHistoryDataSource historySource;
    auto records = historyRecords(1);
    records[0].createdUtc =
        QDateTime::fromString(QStringLiteral("2026-10-22T10:00:00Z"), Qt::ISODate);
    historySource.setRecords(records);
    PinnedLayoutDataSource pinnedSource;
    pinnedSource.record.id = QStringLiteral("layout-pin");
    pinnedSource.record.createdUtc = historySource.records().first().createdUtc;
    ScreenshotHistoryPageWidget historyPage(&historySource, nullptr);
    PinnedWindowManagementPageWidget pinnedPage(&pinnedSource, nullptr);
    historyPage.show();
    pinnedPage.show();
    historyPage.setActive(true);

    for (const auto appearance : {snow_shot::presentation::styles::ThemeAppearance::Light,
                                  snow_shot::presentation::styles::ThemeAppearance::Dark}) {
        themeManager.setThemeAppearance(appearance);
        snow_shot::presentation::styles::ThemeStyleConfig config;
        config.appearance = appearance;
        config.lineWidth =
            appearance == snow_shot::presentation::styles::ThemeAppearance::Dark ? 3 : 1;
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme(config);
        historyPage.applyTheme(scheme);
        pinnedPage.applyTheme(scheme);
        for (const int width : {980, 608, 607, 500, 980}) {
            historyPage.resize(width, 720);
            pinnedPage.resize(width, 720);
            flushEvents();
            flushEvents();
            auto* history =
                historyPage.findChild<QFrame*>(QStringLiteral("screenshotHistoryEntry-record-0"));
            auto* pinned = pinnedPage.findChild<QFrame*>(QStringLiteral("pinnedManagementRecord"));
            require(history && pinned, "both management pages expose a seeded entry");
            auto* historyLayout = dynamic_cast<QBoxLayout*>(history->layout());
            auto* pinnedLayout = dynamic_cast<QBoxLayout*>(pinned->layout());
            require(historyLayout && pinnedLayout &&
                        historyLayout->contentsMargins() == QMargins(18, 16, 18, 16) &&
                        pinnedLayout->contentsMargins() == historyLayout->contentsMargins() &&
                        pinnedLayout->spacing() == historyLayout->spacing(),
                    "both entry layouts use Screenshot History padding and spacing");
            require(pinned->contentsRect() == history->contentsRect() &&
                        pinnedLayout->direction() == historyLayout->direction(),
                    "entry borders preserve equal content insets and responsive geometry");
            const auto relativeRect = [](QWidget* child, QWidget* row) {
                return QRect(child->mapTo(row, QPoint()), child->size());
            };
            auto* historyDate = history->findChild<adqt::widgets::AdCheckbox*>();
            auto* pinnedDate = pinned->findChild<adqt::widgets::AdCheckbox*>();
            require(relativeRect(pinnedDate, pinned) == relativeRect(historyDate, history),
                    "timestamps use identical alignment and leave no stretched checkbox area");
            for (const auto& names :
                 {std::pair{"screenshotHistorySourceBadge", "pinnedManagementSourceBadge"},
                  std::pair{"screenshotHistoryEntrySummary", "pinnedManagementGroup"},
                  std::pair{"screenshotHistoryEntryMetadata", "pinnedManagementStatus"},
                  std::pair{"screenshotHistoryEntryEdit", "pinnedManagementEntryShow"}}) {
                auto* historyControl =
                    history->findChild<QWidget*>(QString::fromLatin1(names.first));
                auto* pinnedControl =
                    pinned->findChild<QWidget*>(QString::fromLatin1(names.second));
                require(
                    historyControl && pinnedControl &&
                        relativeRect(pinnedControl, pinned).topLeft() ==
                            relativeRect(historyControl, history).topLeft() &&
                        pinnedControl->height() == historyControl->height(),
                    "badges, descriptions, and actions share entry padding and vertical spacing");
            }
            auto* historyPreview =
                history->findChild<QWidget*>(QStringLiteral("screenshotHistoryImageCarousel"));
            auto* pinnedPreview =
                pinned->findChild<QWidget*>(QStringLiteral("pinnedManagementPreview"));
            require(historyPreview && pinnedPreview &&
                        relativeRect(pinnedPreview, pinned) ==
                            relativeRect(historyPreview, history),
                    "previews share size and placement when narrowing and widening the pages");
            const QString reviewDirectory = qEnvironmentVariable("SNOW_HISTORY_LAYOUT_REVIEW_DIR");
            if (!reviewDirectory.isEmpty()) {
                require(QDir().mkpath(reviewDirectory), "create history layout review directory");
                const QString suffix =
                    QStringLiteral("%1-%2.png").arg(static_cast<int>(appearance)).arg(width);
                require(history->grab().save(
                            QDir(reviewDirectory).filePath(QStringLiteral("history-") + suffix)) &&
                            pinned->grab().save(
                                QDir(reviewDirectory).filePath(QStringLiteral("pinned-") + suffix)),
                        "save both entry layouts for visual review");
            }
        }
    }
}

void entryStitchActionUsesOnlyItsRecord() {
    MutableHistoryDataSource dataSource;
    auto records = historyRecords(3);
    storage::CaptureHistoryResultRecord result;
    result.imageSize = QSize(32, 18);
    records[0].result = result;
    records[2].result = result;
    dataSource.setRecords(records);
    ScreenshotHistoryPageWidget page(&dataSource, nullptr);
    page.resize(900, 720);
    page.show();
    page.setActive(true);
    flushEvents();

    QVector<storage::CaptureHistoryRecord> requested;
    int requests = 0;
    QObject::connect(&page, &ScreenshotHistoryPageWidget::collageRequested, &page,
                     [&](const auto& selected) {
                         requested = selected;
                         ++requests;
                     });
    auto* entry = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-0"));
    auto* other = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-2"));
    require(entry && other, "stitching fixtures retain their record identities");
    auto* selection = other->findChild<adqt::widgets::AdCheckbox*>();
    selection->setChecked(true);
    auto* more =
        entry->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryEntryMore"));
    require(more && !entry->findChild<adqt::widgets::AdButton*>(
                        QStringLiteral("screenshotHistoryEntryStitch")),
            "history keeps Stitch Image inside More instead of the entry action row");
    const QPoint previousCursor = QCursor::pos();
    const auto restoreCursor = qScopeGuard([previousCursor] { QCursor::setPos(previousCursor); });
    more->click();
    flushEvents();
    QPointer<adqt::widgets::AdContextMenu> menu = entry->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotHistoryEntryMoreMenu"));
    require(menu && menu->isPopupVisible(), "More opens the history stitching menu");
    auto* stitch = menu->findChild<QAction*>(QStringLiteral("screenshotHistoryEntryStitch"));
    require(stitch && stitch->isEnabled() && stitch->text() == QStringLiteral("Stitch Image") &&
                !stitch->icon().isNull() && menu->actionGeometry(stitch).isValid(),
            "More exposes the labeled stitching action with its icon");
    activateMenuAction(menu, stitch);
    flushEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(requests == 1 && requested == QVector<storage::CaptureHistoryRecord>{records[0]} &&
                selection->isChecked() && dataSource.removedIds.isEmpty() && !menu,
            "entry stitching imports only that record, preserves selection, and closes More");
    auto* unavailableEntry =
        page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-1"));
    unavailableEntry
        ->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryEntryMore"))
        ->click();
    flushEvents();
    auto* unavailableMenu = unavailableEntry->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotHistoryEntryMoreMenu"));
    require(unavailableMenu && unavailableMenu->isPopupVisible(),
            "entries without a final image still expose More");
    auto* unavailable =
        unavailableMenu->findChild<QAction*>(QStringLiteral("screenshotHistoryEntryStitch"));
    require(unavailable && !unavailable->isEnabled(),
            "a history record without a final image cannot start stitching");
    activateMenuAction(unavailableMenu, unavailable);
    flushEvents();
    require(requests == 1, "disabled stitching must not dispatch an import");
    unavailableMenu->dismissPopup();
    flushEvents();

    more->click();
    flushEvents();
    menu = entry->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotHistoryEntryMoreMenu"));
    require(menu && menu->isPopupVisible(), "More reopens the history stitching menu");
    stitch = menu->findChild<QAction*>(QStringLiteral("screenshotHistoryEntryStitch"));
    require(stitch != nullptr, "reopening More recreates the stitching action");

    QTranslator translator;
    require(translator.load(QStringLiteral(":/i18n/snow_shot_zh_CN.qm")),
            "load translations for the history stitching action");
    QCoreApplication::installTranslator(&translator);
    QEvent language(QEvent::LanguageChange);
    QApplication::sendEvent(entry, &language);
    const QString translated = translator.translate("HistoryEntryWidget", "Stitch Image");
    require(!translated.isEmpty() && translated != QStringLiteral("Stitch Image") &&
                stitch->text() == translated && stitch->toolTip() == translated,
            "language changes translate the stitching action in an open More menu");
    QCoreApplication::removeTranslator(&translator);
}

void entrySplitActionUsesOnlyItsRecord() {
    MutableHistoryDataSource dataSource;
    auto records = historyRecords(3);
    storage::CaptureHistoryResultRecord result;
    result.imageSize = QSize(32, 18);
    records[0].result = result;
    records[2].result = result;
    dataSource.setRecords(records);
    ScreenshotHistoryPageWidget page(&dataSource, nullptr);
    page.resize(900, 720);
    page.show();
    page.setActive(true);
    flushEvents();
    int requests = 0;
    storage::CaptureHistoryRecord requested;
    QObject::connect(&page, &ScreenshotHistoryPageWidget::splitRequested, &page,
                     [&](const auto& record) {
                         requested = record;
                         ++requests;
                     });
    auto* entry = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-0"));
    auto* other = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-2"));
    require(entry && other, "split fixtures retain their record identities");
    auto* selection = other->findChild<adqt::widgets::AdCheckbox*>();
    selection->setChecked(true);
    auto* more =
        entry->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryEntryMore"));
    more->click();
    flushEvents();
    QPointer<adqt::widgets::AdContextMenu> menu = entry->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotHistoryEntryMoreMenu"));
    require(menu && menu->isPopupVisible(), "More opens the history split menu");
    auto* split = menu->findChild<QAction*>(QStringLiteral("screenshotHistoryEntrySplit"));
    auto* stitch = menu->findChild<QAction*>(QStringLiteral("screenshotHistoryEntryStitch"));
    require(split && stitch && split->isEnabled() &&
                split->text() == QStringLiteral("Image Splitting") && !split->icon().isNull() &&
                menu->actions().indexOf(split) == menu->actions().indexOf(stitch) + 1,
            "Image Splitting follows Stitch Image with a themed icon");
    class SplitTranslator final : public QTranslator {
      public:
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            return QByteArray(context) == "ImageSplitWindow" &&
                           QByteArray(source) == "Image Splitting"
                       ? QStringLiteral("Translated Image Splitting")
                       : QString();
        }
    } translator;
    QCoreApplication::installTranslator(&translator);
    const auto removeTranslator =
        qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
    QEvent language(QEvent::LanguageChange);
    QApplication::sendEvent(entry, &language);
    require(split->text() == QStringLiteral("Translated Image Splitting") &&
                split->toolTip() == split->text(),
            "open history Image Splitting action uses the shared translation context");
    activateMenuAction(menu, split);
    flushEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(requests == 1 && requested == records[0] && selection->isChecked() &&
                dataSource.removedIds.isEmpty() && !menu,
            "splitting dispatches only its source record and preserves history selection");
    auto* unavailableEntry =
        page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-1"));
    unavailableEntry
        ->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryEntryMore"))
        ->click();
    flushEvents();
    auto* unavailableMenu = unavailableEntry->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotHistoryEntryMoreMenu"));
    require(unavailableMenu, "record without a result retains its More menu");
    auto* unavailable =
        unavailableMenu->findChild<QAction*>(QStringLiteral("screenshotHistoryEntrySplit"));
    require(unavailable && !unavailable->isEnabled(), "missing history result disables splitting");
    activateMenuAction(unavailableMenu, unavailable);
    require(requests == 1, "disabled split entry cannot dispatch");
}

void moreMenuOffersPinStitchAndDelete() {
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
        const QPoint local(widget->width() / 2, 0);
        const QPoint global = widget->mapToGlobal(local);
        QCursor::setPos(global);
        QEnterEvent event(local, local, global);
        QApplication::sendEvent(widget, &event);
    };
    hover(more);
    flushEvents();
    QPointer<adqt::widgets::AdContextMenu> menu = visibleMenu(pinnableEntry);
    require(menu != nullptr && menu->actions().size() == 6 && !menu->nativeMenuEnabled(),
            "hovering More must show the shared widget action menu");
    QAction* pin = menu->actions().at(0);
    QAction* stitch = menu->actions().at(1);
    QAction* split = menu->actions().at(2);
    QAction* reveal = menu->actions().at(4);
    QAction* remove = menu->actions().at(5);
    auto* imageEditing =
        menu->findChild<QAction*>(QStringLiteral("screenshotHistoryEntryImageEditing"));
    require(imageEditing && menu->actions().at(3) == imageEditing && imageEditing->isEnabled() &&
                imageEditing->text() == QStringLiteral("Image Editing") &&
                !imageEditing->icon().isNull(),
            "independent image editing follows Image Splitting in the record menu");
    require(reveal->objectName() == QStringLiteral("screenshotHistoryEntryReveal") &&
                reveal->text() ==
                    snow_shot::presentation::components::history_page::revealActionText() &&
                reveal->toolTip() == reveal->text() && reveal->isEnabled() &&
                !reveal->icon().isNull(),
            "the platform reveal action sits between image editing and Delete with its icon");
    require(pin->text() == QStringLiteral("Pin to screen") && pin->isEnabled() &&
                stitch->text() == QStringLiteral("Stitch Image") && stitch->isEnabled() &&
                split->text() == QStringLiteral("Image Splitting") && split->isEnabled() &&
                remove->text() == QStringLiteral("Delete") && menu->actionDanger(remove) &&
                menu->actionGeometry(pin).isValid(),
            "the More menu preserves its existing image actions and danger Delete");
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(pinnableEntry, &languageChange);
    flushEvents();
    require(pin->text() == QStringLiteral("Pin to screen") &&
                pin->toolTip() == QStringLiteral("Pin this screenshot to the screen") &&
                stitch->text() == QStringLiteral("Stitch Image") &&
                remove->text() == QStringLiteral("Delete"),
            "an open More menu must keep its own action labels when the language changes");
    activateMenuAction(menu, pin);
    flushEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(pinnedId == QStringLiteral("record-0") && !menu,
            "Pin to screen must request that history entry and close the menu");

    storage::CaptureHistoryRecord editorRecord;
    QObject::connect(&page, &ScreenshotHistoryPageWidget::imageEditingRequested, &page,
                     [&](const auto& record) { editorRecord = record; });
    more->click();
    flushEvents();
    auto* editorMenu = visibleMenu(pinnableEntry);
    require(editorMenu, "reopen More for independent editing");
    activateMenuAction(editorMenu, editorMenu->findChild<QAction*>(
                                       QStringLiteral("screenshotHistoryEntryImageEditing")));
    flushEvents();
    require(editorRecord == records[0] && dataSource.removedIds.isEmpty(),
            "Image Editing dispatches the source record without altering history");

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
    require(plainMenu->actions().at(4)->isEnabled(),
            "resultless history still supports revealing its stored source or directory");
    activateMenuAction(plainMenu, plainMenu->actions().at(4));
    flushEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(dataSource.revealedIds == QStringList{QStringLiteral("record-1")},
            "resultless reveal uses its own record");

    auto* selected = plainEntry->findChild<adqt::widgets::AdCheckbox*>();
    selected->setChecked(true);
    more->click();
    flushEvents();
    menu = visibleMenu(pinnableEntry);
    require(menu, "reopen the menu for reveal translation");
    for (const QString& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        QTranslator translator;
        require(translator.load(QStringLiteral(":/i18n/snow_shot_%1.qm").arg(locale)),
                "load reveal action translations");
        QCoreApplication::installTranslator(&translator);
        QApplication::sendEvent(pinnableEntry, &languageChange);
        const QString label = snow_shot::presentation::components::history_page::revealActionText();
        require(menu && menu->actions().at(4)->text() == label &&
                    menu->actions().at(4)->toolTip() == label &&
                    !translator.translate("HistoryFileLocation", "Open in Explorer").isEmpty() &&
                    !translator.translate("HistoryFileLocation", "Reveal in Finder").isEmpty(),
                "open history menus translate their reveal label and tooltip");
        QCoreApplication::removeTranslator(&translator);
    }
    activateMenuAction(menu, menu->actions().at(4));
    flushEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!menu && selected->isChecked() &&
                dataSource.revealedIds.last() == QStringLiteral("record-0") &&
                dataSource.removedIds.isEmpty() && dataSource.readFailures == 0,
            "reveal preserves selection, targets only its row, and retires the popup");

    dataSource.revealSucceeds = false;
    QString failureMessage;
    auto* messages = adqt::widgets::AdMessageService::instance(&page);
    QObject::connect(
        messages, &adqt::widgets::AdMessage::messageOpened, &page,
        [&](adqt::widgets::AdMessageHandle* handle) { failureMessage = handle->content(); });
    more->click();
    flushEvents();
    menu = visibleMenu(pinnableEntry);
    require(menu, "reopen reveal failure fixture menu");
    // Exercise keyboard dispatch as well as the successful mouse activations above.
    menu->setActiveAction(menu->actions().at(4));
    QKeyEvent revealKey(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(menu, &revealKey);
    flushEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!menu && dataSource.removedIds.isEmpty() && dataSource.readFailures == 0 &&
                dataSource.records().size() == 2 && selected->isChecked(),
            "failed reveal preserves history and selection without invoking read-failure cleanup");
    require(dataSource.revealedIds.size() == 3 &&
                failureMessage == QStringLiteral("Unable to open this entry's file location.") &&
                QApplication::activeModalWidget() == nullptr,
            "failed reveal displays a non-modal error");
    adqt::widgets::AdMessageService::destroyAll(&page);

    hover(more);
    flushEvents();
    menu = visibleMenu(pinnableEntry);
    require(menu != nullptr, "More must reopen its action menu");
    activateMenuAction(menu, menu->actions().last());
    flushEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    adqt::widgets::AdButton* confirmDelete =
        confirmation->button(adqt::widgets::AdPopconfirm::StandardButton::Ok);
    require(confirmation->isVisible() && confirmDelete != nullptr && !menu,
            "Delete must close the action menu and ask for confirmation");
    confirmDelete->click();
    flushEvents();
    require(dataSource.removedIds.size() == 1 &&
                dataSource.removedIds.front() == QStringLiteral("record-0"),
            "confirming Delete must remove that history entry");
}

void historyHoverMovementDoesNotRequireSelectionOrEnter() {
    MutableHistoryDataSource dataSource;
    dataSource.setRecords(historyRecords(2));
    ScreenshotHistoryPageWidget page(&dataSource, nullptr);
    page.resize(900, 720);
    page.show();
    page.setActive(true);
    flushEvents();
    const QPoint previousCursor = QCursor::pos();
    const auto restoreCursor = qScopeGuard([previousCursor] { QCursor::setPos(previousCursor); });
    QCursor::setPos(page.mapToGlobal(QPoint(10, 10)));
    auto* first = page.findChild<QWidget*>(QStringLiteral("screenshotHistoryEntry-record-0"));
    auto* selection = first->findChild<adqt::widgets::AdCheckbox*>();
    for (const bool selected : {false, true, false}) {
        selection->setChecked(selected);
        flushEvents();
        for (int index = 0; index < 2; ++index) {
            auto* row = page.findChild<QWidget*>(
                QStringLiteral("screenshotHistoryEntry-record-%1").arg(index));
            auto* more = row->findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenshotHistoryEntryMore"));
            // The failing native stream contains HoverMove alone, including on
            // repeated visits to the first row before its selection bar opens.
            for (int visit = 0; visit < 2; ++visit) {
                const QPoint local(more->width() / 2, 0);
                QHoverEvent move(QEvent::HoverMove, local, more->mapToGlobal(local),
                                 QPointF(local.x(), -1), Qt::NoModifier);
                QApplication::sendEvent(more, &move);
                auto* menu = row->findChild<adqt::widgets::AdContextMenu*>(
                    QStringLiteral("screenshotHistoryEntryMoreMenu"));
                require(menu && menu->isPopupVisible(), "each row must open More immediately on "
                                                        "hover movement with or without selection");
                menu->dismissPopup();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
        }
    }
    require(dataSource.removedIds.isEmpty(), "hovering More must preserve every history entry");
}

void moreButtonClicksDoNotRequestDeletion() {
    MutableHistoryDataSource dataSource;
    dataSource.setRecords(historyRecords(1));
    int confirmationOpenings = 0;
    ScreenshotHistoryPageWidget page(&dataSource, nullptr);
    page.resize(900, 720);
    page.show();
    page.setActive(true);
    flushEvents();
    auto* more =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryEntryMore"));
    auto* confirmation = page.findChild<adqt::widgets::AdPopconfirm*>(
        QStringLiteral("screenshotHistoryEntryDeleteConfirm"));
    require(more && confirmation, "the click regression needs More and its delete confirmation");
    QObject::connect(confirmation, &adqt::widgets::AdPopconfirm::visibleChanged, &page,
                     [&](bool visible) {
                         if (visible)
                             ++confirmationOpenings;
                     });
    const QPoint previousCursor = QCursor::pos();
    const auto restoreCursor = qScopeGuard([previousCursor] { QCursor::setPos(previousCursor); });
    const QPoint local = more->rect().center();
    const QPoint global = more->mapToGlobal(local);
    QCursor::setPos(global);
    for (int cycle = 0; cycle < 3; ++cycle) {
        // QAbstractButton::click() bypasses the popconfirm's input event filter.
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(more, &press);
        QApplication::sendEvent(more, &release);
        flushEvents();
        require(!confirmation->isVisible() && confirmationOpenings == 0,
                "clicking More must never open Delete confirmation");
        QPointer<adqt::widgets::AdContextMenu> menu = page.findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("screenshotHistoryEntryMoreMenu"));
        require(menu && menu->isPopupVisible(), "clicking More must open the action menu");
        menu->dismissPopup();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        flushEvents();
    }
    for (const Qt::Key key : {Qt::Key_Return, Qt::Key_Space}) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(more, &press);
        QApplication::sendEvent(more, &release);
        flushEvents();
        require(!confirmation->isVisible() && confirmationOpenings == 0,
                "keyboard activation of More must never open Delete confirmation");
        auto* menu = page.findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("screenshotHistoryEntryMoreMenu"));
        require(menu && menu->isPopupVisible() && menu->activeAction(),
                "keyboard activation must open More with an active action");
        menu->dismissPopup();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        flushEvents();
    }
    require(dataSource.removedIds.isEmpty(), "opening More must preserve its history entry");
}

void moreMenuRetirementKeepsIdleHistoryUsable() {
    MutableHistoryDataSource dataSource;
    dataSource.setRecords(historyRecords(1));
    ScreenshotHistoryPageWidget page(&dataSource, nullptr);
    page.resize(900, 720);
    page.show();
    page.setActive(true);
    flushEvents();
    auto* more =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHistoryEntryMore"));
    require(more != nullptr, "the idle history fixture must expose More");
    auto currentMenu = [&]() -> adqt::widgets::AdContextMenu* {
        for (auto* menu : page.findChildren<adqt::widgets::AdContextMenu*>(
                 QStringLiteral("screenshotHistoryEntryMoreMenu"))) {
            if (menu->isPopupVisible()) {
                return menu;
            }
        }
        return nullptr;
    };
    for (int cycle = 0; cycle < 4; ++cycle) {
        more->click();
        QPointer<adqt::widgets::AdContextMenu> menu = currentMenu();
        require(menu != nullptr, "More must open a fresh idle history menu");
        QPointer<QAction> action = menu->actions().last();
        menu->dismissPopup();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        flushEvents();
        require(!menu && !action && more->isVisible(),
                "menu teardown must release the popup and leave the idle history row usable");
    }
    more->click();
    QPointer<adqt::widgets::AdContextMenu> menu = currentMenu();
    require(menu != nullptr, "the row removal fixture must have an open menu");
    dataSource.setRecords({});
    page.refresh();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    flushEvents();
    require(!menu && page.findChildren<adqt::widgets::AdContextMenu*>().isEmpty(),
            "removing a history row must release its open action menu safely");
}

void waitUntil(const std::function<bool()>& complete, const char* message) {
    QElapsedTimer deadline;
    deadline.start();
    while (!complete() && deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    require(complete(), message);
}

// Production publishes native Windows formats even under the offscreen plugin.
// Read that boundary explicitly rather than inspecting the unrelated offscreen clipboard.
std::optional<ScreenshotClipboardContentSnapshot> historyClipboardSnapshot() {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        if (!OpenClipboard(nullptr))
            return std::nullopt;
        const auto close = qScopeGuard([] { CloseClipboard(); });
        const auto bytes = [](const wchar_t* name) {
            const auto handle =
                static_cast<HGLOBAL>(GetClipboardData(RegisterClipboardFormatW(name)));
            const auto size = handle ? GlobalSize(handle) : 0;
            const void* memory = handle ? GlobalLock(handle) : nullptr;
            QByteArray result;
            if (memory && size)
                result = QByteArray(static_cast<const char*>(memory), qsizetype(size));
            if (memory)
                GlobalUnlock(handle);
            return result;
        };
        ScreenshotClipboardContentSnapshot snapshot;
        const auto png = bytes(L"PNG");
        if (png.isEmpty())
            return std::nullopt;
        snapshot.encodedImages.append({png, QStringLiteral("image/png")});
        snapshot.placement =
            decodeScreenshotClipboardPlacement(bytes(L"SnowShotScreenshotPlacement"));
        snapshot.appearance =
            decodeScreenshotClipboardAppearance(bytes(L"SnowShotScreenshotAppearance"));
        return snapshot;
    }
#endif
    return ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), 1);
}

void historyCopiesPreservePositionAndAppearance() {
    auto& repository = storage::ApplicationStorage::instance().captureHistory();
    for (int mode : {0, 1, 2}) {
        storage::CaptureHistoryDraft draft;
        draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        draft.createdUtc = QDateTime::currentDateTimeUtc();
        draft.canvasBounds = QRect(0, 0, 400, 300);
        draft.selection.rectangle = QRect(100, 80, 100, 50);
        draft.selection.cornerRadius = 12;
        draft.selection.shadowWidth = 8;
        draft.selection.shadowColor = Qt::black;
        draft.canvasHistory =
            QByteArrayLiteral("{\"schemaVersion\":1,\"document\":{},\"history\":{}}");
        draft.scrolling = mode == 2;
        if (mode != 1) {
#ifdef Q_OS_MACOS
            draft.desktopGeometry = storage::CaptureHistoryDesktopGeometry{QPoint(), true};
#else
            draft.desktopGeometry = storage::CaptureHistoryDesktopGeometry{QPoint(), false};
#endif
        }
        QImage display(draft.canvasBounds.size(), QImage::Format_RGB32);
        display.fill(Qt::blue);
        draft.displays.push_back(
            {QStringLiteral("display-id"), QStringLiteral("Display"), display});
        const ScreenshotResultStyle style{12, 8, Qt::black};
        draft.resultImage =
            ScreenshotResultCompositor::compose(display.copy(draft.selection.rectangle), style);
        const auto published = repository.publish(draft).get();
        require(published.storage.success, "publish clipboard history fixture");
        const auto stored = repository.loadResultPng(published.record);
        require(stored.has_value(), "history PNG fixture is unavailable");
        // Deliver the repository's publication notification before opening the page.
        // A refresh queued by that notification intentionally cancels pending copies.
        flushEvents();
        {
            ScreenshotHistoryPageWidget page;
            page.resize(900, 700);
            page.show();
            page.setActive(true);
            flushEvents();
            auto* entry = page.findChild<QWidget*>(
                QStringLiteral("screenshotHistoryEntry-%1").arg(published.record.id));
            require(entry != nullptr, "published history entry is unavailable");
            auto* copy = entry->findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenshotHistoryEntryCopy"));
            require(copy != nullptr && copy->isVisible(), "history copy action is unavailable");
            QApplication::clipboard()->setText(QStringLiteral("before history copy"));
#ifdef Q_OS_WIN
            require(OpenClipboard(nullptr), "open clipboard for history fixture");
            require(EmptyClipboard(), "clear native clipboard for history fixture");
            CloseClipboard();
#endif
            copy->click();
            std::optional<ScreenshotClipboardContentSnapshot> snapshot;
            waitUntil(
                [&] {
                    // Async asset resolution may replace the row while copying.
                    auto* currentEntry = page.findChild<QWidget*>(
                        QStringLiteral("screenshotHistoryEntry-%1").arg(published.record.id));
                    auto* currentCopy = currentEntry
                                            ? currentEntry->findChild<adqt::widgets::AdButton*>(
                                                  QStringLiteral("screenshotHistoryEntryCopy"))
                                            : nullptr;
                    snapshot = historyClipboardSnapshot();
                    return currentCopy && currentCopy->isEnabled() && snapshot &&
                           !snapshot->encodedImages.isEmpty();
                },
                "history image copy did not finish");
            require(std::any_of(snapshot->encodedImages.cbegin(), snapshot->encodedImages.cend(),
                                [&](const auto& image) { return image.bytes == stored->bytes(); }),
                    "history copy changes the stored PNG");
            auto content = ScreenshotClipboardContentReader::decode(std::move(*snapshot));
            require(content && content->appearance &&
                        content->appearance->borderAppearance ==
                            snow_shot::presentation::historySelectionBorderAppearance(
                                published.record) &&
                        !content->appearance->checkerboardEnabled,
                    "history copy loses its baked selection appearance");
            const auto direct =
                snow_shot::presentation::historySelectionPinPlacement(published.record);
            require(content->placement.has_value() == direct.isPrepared(),
                    "history copy loses recoverable position or invents missing position");
            if (direct.isPrepared())
                require(content->placement->windowRect == direct.geometry.nativeGeometry,
                        "history clipboard position differs from direct history pinning");
        }
        require(repository.remove(published.record.id).get().success,
                "remove clipboard history fixture");
    }
}

void thumbnailsUseMediumCompression() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "thumbnail compression fixture must be available");
    QImage image(64, 48, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            image.setPixel(x, y, qRgb(x * 3, y * 5, (x + y) * 2));
    }
    const auto encoder = snow_shot::image_codec::encoderInfo(snow::image::Format::png);
    require(encoder.has_value(), "PNG encoder must be available for thumbnail persistence");
    const QByteArray expected =
        snow_shot::image_codec::encodePng(image, encoder->compression_level.default_value);
    require(!expected.isEmpty() && expected != snow_shot::image_codec::encodePng(image, 0),
            "thumbnail fixture must distinguish medium compression from level zero");
    const QString path = temporary.filePath(QStringLiteral("thumbnail.png"));
    const QSize naturalSize(900, 700);
    snow_shot::presentation::components::thumbnail_cache::persist(path, image, naturalSize);
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.readAll() == expected,
            "persisted thumbnails must use medium PNG compression");
    const auto cached = snow_shot::presentation::components::thumbnail_cache::load(path);
    const QImage converted = cached.image.convertToFormat(QImage::Format_RGB32);
    require(!converted.isNull() && converted.size() == image.size(),
            "thumbnail compression must preserve image dimensions");
    // Encoding explicitly declares sRGB even for an untagged input fixture.
    // Compare pixels independently of QImage's color-metadata-sensitive equality.
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            require(converted.pixel(x, y) == image.pixel(x, y),
                    "thumbnail compression must preserve every RGB pixel");
        }
    }
    require(cached.naturalSize == naturalSize,
            "thumbnail compression must preserve natural-size metadata");
    require(cached.image.colorSpace() == QColorSpace(QColorSpace::SRgb),
            "persisted thumbnails must retain their explicitly encoded sRGB color space");
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
    recordFiltersPreserveLocalDateBoundaries();
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
    if (application.arguments().contains(QStringLiteral("--sticky-only"))) {
        selectionPanelSticksWhileScrolling();
        storage::ApplicationStorage::instance().shutdown();
        QCoreApplication::removeTranslator(&englishTranslator);
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--page-size-only"))) {
        pageSizesRememberedIndependently(temporary.path());
        storage::ApplicationStorage::instance().shutdown();
        QCoreApplication::removeTranslator(&englishTranslator);
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--entry-layout-only"))) {
        entryLayoutsMatchAcrossManagementPages();
        storage::ApplicationStorage::instance().shutdown();
        QCoreApplication::removeTranslator(&englishTranslator);
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--clipboard-only"))) {
        historyCopiesPreservePositionAndAppearance();
        storage::ApplicationStorage::instance().shutdown();
        QCoreApplication::removeTranslator(&englishTranslator);
        return 0;
    }
    batchPinUsesEligibleSelectionAtActivation();
    entryStitchActionUsesOnlyItsRecord();
    entrySplitActionUsesOnlyItsRecord();
    if (application.arguments().contains(QStringLiteral("--collage-only"))) {
        entriesUseBordersAndSupportCrossPageSelection();
        storage::ApplicationStorage::instance().shutdown();
        QCoreApplication::removeTranslator(&englishTranslator);
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--context-menu-only"))) {
        historyHoverMovementDoesNotRequireSelectionOrEnter();
        moreButtonClicksDoNotRequestDeletion();
        moreMenuOffersPinStitchAndDelete();
        moreMenuRetirementKeepsIdleHistoryUsable();
        storage::ApplicationStorage::instance().shutdown();
        QCoreApplication::removeTranslator(&englishTranslator);
        return 0;
    }
    entryDescriptionsUseSmallThemeText();
    historyThemeChangesDoNotRestyleTwice();
    historyRowsRespectSkinMask();
    selectionPanelSticksWhileScrolling();
    continuousHistoryPreview();
    emptyStateRemainsVisibleAfterFilteringEmptyHistory();
    pageTextAndEmptyStateMatchPinnedWindowManagement();
    entryLayoutsMatchAcrossManagementPages();
    if (application.arguments().contains(QStringLiteral("--loading-only"))) {
        entriesUseBordersAndSupportCrossPageSelection();
        storage::ApplicationStorage::instance().shutdown();
        QCoreApplication::removeTranslator(&englishTranslator);
        return 0;
    }
    historyHoverMovementDoesNotRequireSelectionOrEnter();
    moreButtonClicksDoNotRequestDeletion();
    moreMenuOffersPinStitchAndDelete();
    moreMenuRetirementKeepsIdleHistoryUsable();
    entriesUseBordersAndSupportCrossPageSelection();
    historyCopiesPreservePositionAndAppearance();
    thumbnailsUseMediumCompression();
    imageFailuresRespectCacheFallbackAndCancellation();
    shutdownDrainsBacklogThenRejectsNewWork();
    storage::ApplicationStorage::instance().shutdown();
    QCoreApplication::removeTranslator(&englishTranslator);
    return 0;
}
