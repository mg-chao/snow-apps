#include "snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h"
#include "snow_shot/presentation/components/historypagecommon.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../../test-support/virtualmemory.h"
#include "snow_shot/presentation/components/thumbnailcache.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "theme/theme_manager.h"
#include "widgets/select.h"
#include "widgets/date_picker.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/popconfirm.h"
#include "widgets/pagination.h"
#include "widgets/image.h"
#include "widgets/context_menu.h"
#include "widgets/message.h"
#include "history_sticky_selection_test_support.h"
#include <QElapsedTimer>
#include <QTimeZone>
#include <QThread>
#include <QUuid>
#include <QFile>
#include <QDir>
#include <QApplication>
#include <QTemporaryDir>
#include <QLabel>
#include <QFrame>
#include <QBoxLayout>
#include <QEvent>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QPainter>
#include <QSemaphore>
#include <QStackedWidget>
#include <QTranslator>
#include <QStringList>
#include <QScopeGuard>
#include <algorithm>
#include <functional>
#include <cstdlib>
#include <iostream>
using namespace snow_shot;
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
class Fixture final : public PinnedWindowManagementDataSource {
  public:
    QVector<storage::PinnedWindowSummary> items;
    QString shown;
    QStringList shownIds;
    QVector<QStringList> shownBatches;
    std::function<void()> onShow;
    QStringList revealed;
    bool revealSucceeds = true;
    bool revealRecord(const QString& id) override {
        revealed.append(id);
        return revealSucceeds;
    }
    QVector<QString> removed;
    QVector<QSize> previewSizes;
    int fullImageRequests = 0;
    bool previewFailure = false;
    mutable int recordRequests = 0;
    QHash<QString, quint64> previewRevisions;
    QString lastFullImageId;
    QVector<storage::PinnedWindowSummary> records() const override {
        ++recordRequests;
        return items;
    }
    QVector<storage::PinnedWindowGroup> groups() const override {
        return {{QStringLiteral("default"), QStringLiteral("Default"), true},
                {QStringLiteral("work"), QStringLiteral("Work"), false}};
    }
    std::optional<quint64> previewRevision(const QString& id) const override {
        return previewRevisions.value(id, 1);
    }
    void requestPreview(const QString& id, quint64 requestId, const QSize& targetSize) override {
        previewSizes.push_back(targetSize);
        if (previewFailure) {
            emit previewReady(id, requestId, {}, {});
            return;
        }
        QImage image(120, 60, QImage::Format_RGB32);
        image.fill(Qt::green);
        emit previewReady(id, requestId, image, image.size());
    }
    void requestFullImage(const QString& id, quint64 requestId) override {
        ++fullImageRequests;
        lastFullImageId = id;
        QImage image(120, 60, QImage::Format_RGB32);
        image.fill(Qt::green);
        emit fullImageReady(id, requestId, image);
    }
    void showRecord(const QString& id) override {
        shown = id;
        shownIds.append(id);
        if (onShow)
            onShow();
    }
    void showRecords(const QStringList& ids) override {
        shownBatches.append(ids);
        for (const auto& id : ids)
            showRecord(id);
    }
    void removeRecords(const QVector<QString>& ids) override {
        removed = ids;
    }
};

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
    Fixture fixture;
    for (int index = 0; index < 55; ++index) {
        storage::PinnedWindowSummary item;
        item.id = QStringLiteral("sticky-%1").arg(index);
        item.createdUtc =
            QDateTime(QDate(2026, 9, 24), QTime(0, 30), QTimeZone::UTC).addSecs(-index);
        fixture.items.push_back(item);
    }
    storage::InterfaceSettings settings;
    const int originalPageSize = settings.pinnedWindowManagementPageSize();
    const auto restorePageSize =
        qScopeGuard([&] { settings.setPinnedWindowManagementPageSize(originalPageSize); });
    for (const int pageSize : {10, 20, 50}) {
        require(settings.setPinnedWindowManagementPageSize(pageSize),
                "save pinned fixture page size");
        PinnedWindowManagementPageWidget page(&fixture, nullptr);
        require(page.findChild<adqt::widgets::AdPagination*>()->pageSize() == pageSize,
                "sticky pinned fixture restores the saved page size");
        page.resize(980, 640);
        page.show();
        history_sticky_test::settleLayout();
        auto* row = page.findChild<QFrame*>(QStringLiteral("pinnedManagementRecord"));
        require(row != nullptr, "sticky pinned fixture has an entry");
        history_sticky_test::verifySelection(page, QStringLiteral("pinnedManagement"),
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

void pinnedDescriptionsUseSmallThemeText(const QVector<storage::PinnedWindowSummary>& records) {
    Fixture fixture;
    fixture.items = records;
    QWidget host;
    QFont inheritedFont = host.font();
    inheritedFont.setPixelSize(14);
    inheritedFont.setHintingPreference(QFont::PreferNoHinting);
    host.setFont(inheritedFont);
    PinnedWindowManagementPageWidget page(&fixture, &host);
    host.resize(980, 640);
    page.resize(host.size());
    host.show();
    page.show();
    QCoreApplication::processEvents();

    const QStringList descriptionNames{QStringLiteral("pinnedManagementSourceBadge"),
                                       QStringLiteral("pinnedManagementGroup"),
                                       QStringLiteral("pinnedManagementStatus")};
    QHash<QString, QFont::HintingPreference> originalHinting;
    for (const auto& name : descriptionNames) {
        const auto* label = page.findChild<QLabel*>(name);
        require(label != nullptr, "pinned description labels exist before changing their theme");
        originalHinting.insert(name, label->font().hintingPreference());
    }

    for (const int fontSize : {14, 20, 14}) {
        presentation::styles::ThemeStyleConfig config;
        config.fontSize = fontSize;
        const auto scheme = presentation::styles::generateThemeColorScheme(config);
        page.applyTheme(scheme);
        const auto requireDescriptionFonts = [&] {
            for (const auto& name : descriptionNames) {
                const auto labels = page.findChildren<QLabel*>(name);
                require(labels.size() == fixture.items.size(),
                        "every pinned entry exposes all three description labels");
                for (const auto* label : labels) {
                    require(label->font().pixelSize() == scheme.metricAlias.fontSizeSM &&
                                label->font().weight() == QFont::Normal &&
                                label->font().hintingPreference() == originalHinting.value(name),
                            "pinned descriptions use small theme text and preserve their hinting");
                }
            }
            const auto timestamps = page.findChildren<adqt::widgets::AdCheckbox*>(
                QStringLiteral("pinnedManagementEntrySelection"));
            for (const auto* timestamp : timestamps) {
                require(timestamp->font().pixelSize() == scheme.metricAlias.fontSizeLG &&
                            timestamp->font().weight() == QFont::DemiBold,
                        "pinned timestamps retain their heading text style");
            }
        };
        QCoreApplication::processEvents();
        requireDescriptionFonts();
        inheritedFont.setPointSizeF(24);
        inheritedFont.setWeight(QFont::Bold);
        host.setFont(inheritedFont);
        QCoreApplication::processEvents();
        requireDescriptionFonts();
        auto record = records.first();
        record.id = QStringLiteral("added-%1").arg(fixture.items.size());
        fixture.items.push_back(record);
        page.refresh();
        QCoreApplication::processEvents();
        requireDescriptionFonts();
    }
}

void pinnedThemeChangesDoNotRestyleTwice(const QVector<storage::PinnedWindowSummary>& records) {
    auto& themeManager = presentation::styles::ThemeManager::instance();
    themeManager.setThemeAppearance(presentation::styles::ThemeAppearance::Light);
    Fixture fixture;
    fixture.items = records;
    PinnedWindowManagementPageWidget page(&fixture, nullptr);
    page.resize(980, 640);
    page.show();
    QCoreApplication::processEvents();
    auto* badge = page.findChild<QLabel*>(QStringLiteral("pinnedManagementSourceBadge"));
    require(badge != nullptr, "the pinned theme regression needs a seeded source badge");
    StyleChangeRecorder recorder;
    badge->installEventFilter(&recorder);
    for (const auto appearance : {presentation::styles::ThemeAppearance::Dark,
                                  presentation::styles::ThemeAppearance::Light}) {
        const QString previous = badge->styleSheet();
        recorder.styles.clear();
        themeManager.setThemeAppearance(appearance);
        require(recorder.styles.size() == 1 && recorder.styles.first() != previous,
                "an unskinned pinned theme change must apply each source badge exactly once");
    }
    QWidget unrelated;
    adqt::theme::ThemeOverride overrideValue;
    overrideValue.backgroundOpacity = 0.4;
    auto& controlTheme = adqt::theme::ThemeManager::instance();
    recorder.styles.clear();
    controlTheme.setScopeOverride(&unrelated, overrideValue);
    controlTheme.clearScopeOverride(&unrelated);
    require(recorder.styles.isEmpty(),
            "an unrelated skin must not restyle unskinned pinned source badges");
}

void pinnedRowsRespectSkinMask(const QVector<storage::PinnedWindowSummary>& records) {
    Fixture fixture;
    fixture.items = records;
    SkinBackdrop backdrop;
    backdrop.resize(980, 640);
    PinnedWindowManagementPageWidget page(&fixture, &backdrop);
    page.resize(backdrop.size());
    backdrop.show();
    page.show();
    QCoreApplication::processEvents();
    auto* row = page.findChild<QFrame*>(QStringLiteral("pinnedManagementRecord"));
    require(row != nullptr && row->width() > 100, "seeded pinned mask fixture has a visible row");
    const QColor fill =
        presentation::styles::ThemeManager::instance().themeColorScheme().map.colorBgContainer;
    auto& manager = adqt::theme::ThemeManager::instance();
    for (const qreal opacity : {0.4, 0.0, 1.0}) {
        adqt::theme::ThemeOverride overrideValue;
        overrideValue.backgroundOpacity = opacity;
        manager.setScopeOverride(&backdrop, overrideValue);
        QCoreApplication::processEvents();
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
                "pinned row background must paint one mask and expose the backdrop at zero");
        const QString reviewDirectory = qEnvironmentVariable("SNOW_SKIN_PAGE_REVIEW_DIR");
        if (!reviewDirectory.isEmpty()) {
            require(QDir().mkpath(reviewDirectory), "create seeded pinned review directory");
            require(rendered.save(QDir(reviewDirectory)
                                      .filePath(QStringLiteral("seeded-pinned-mask-%1.png")
                                                    .arg(qRound(opacity * 100)))),
                    "save seeded pinned skin review");
        }
    }
    manager.clearScopeOverride(&backdrop);
}

void pinnedChangesRefreshOnlyVisiblePages(const QVector<storage::PinnedWindowSummary>& records) {
    Fixture source;
    source.items = records;
    QWidget owner;
    owner.resize(980, 640);
    auto* layout = new QVBoxLayout(&owner);
    auto* page = new PinnedWindowManagementPageWidget(&source, &owner);
    layout->addWidget(page);
    auto* viewer = page->findChild<adqt::widgets::AdImageViewer*>();
    auto* model = page->findChild<adqt::widgets::AdImageListModel*>();
    require(source.recordRequests == 1 && viewer != nullptr && model != nullptr,
            "constructing a hidden page prepares its initial records");
    for (int index = 0; index < 20; ++index)
        emit source.changed();
    QApplication::processEvents();
    require(source.recordRequests == 1, "a hidden page must defer a burst of repository snapshots");
    owner.show();
    QApplication::processEvents();
    require(source.recordRequests == 2,
            "showing a dirty page takes one current repository snapshot");

    viewer->openAt(0);
    QApplication::processEvents();
    const QString previewId = viewer->itemAt(0).source.path();
    int modelResets = 0;
    QObject::connect(model, &QAbstractItemModel::modelReset, page,
                     [&modelResets]() { ++modelResets; });
    for (auto& record : source.items)
        if (record.id == previewId)
            record.ignored = !record.ignored;
    emit source.changed();
    require(source.recordRequests == 3 && viewer->isVisible() && modelResets == 0,
            "visible metadata changes refresh immediately and preserve the current preview");
    source.previewRevisions.insert(previewId, 2);
    emit source.changed();
    require(!viewer->isVisible() && modelResets == 0,
            "changed image pixels invalidate the preview without resetting an unchanged list");

    viewer->openAt(0);
    owner.hide();
    require(!viewer->isVisible(), "hiding the page closes its independent image viewer");
    const int requestsBeforeHiddenChanges = source.recordRequests;
    auto newest = records.front();
    newest.id = QStringLiteral("latest-visible-refresh");
    source.items = {newest};
    for (int index = 0; index < 20; ++index)
        emit source.changed();
    QApplication::processEvents();
    require(source.recordRequests == requestsBeforeHiddenChanges && viewer->rowCount() == 2,
            "parent-window hiding suppresses catalog and preview-list rebuilding");
    owner.show();
    QApplication::processEvents();
    require(source.recordRequests == requestsBeforeHiddenChanges + 1 && viewer->rowCount() == 1 &&
                viewer->itemAt(0).source.path() == newest.id,
            "showing the page reconciles all hidden changes with one fresh snapshot");
}

void pinnedHiddenRecordsUseShowAction(const storage::PinnedWindowSummary& record) {
    Fixture source;
    source.items = {record};
    source.items.first().ignored = false;
    source.items.first().hidden = true;
    PinnedWindowManagementPageWidget page(&source, nullptr);
    page.resize(980, 640);
    page.show();
    QCoreApplication::processEvents();
    QPointer<QFrame> row = page.findChild<QFrame*>(QStringLiteral("pinnedManagementRecord"));
    require(!row.isNull(), "hidden persisted record remains available in management");
    auto* status = row->findChild<QLabel*>(QStringLiteral("pinnedManagementStatus"));
    auto* show =
        row->findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementEntryShow"));
    require(status && show && status->text() == QStringLiteral("Hidden") &&
                show->text() == QStringLiteral("Show"),
            "hidden records display Hidden and use the Show action");
    show->click();
    require(source.shown == record.id, "showing a hidden record dispatches its persisted ID");
    source.items.first().hidden = false;
    emit source.changed();
    require(!row.isNull() && status->text() == QStringLiteral("Not Closed") &&
                show->text() == QStringLiteral("Show"),
            "showing a hidden record refreshes its status in the existing row");
    source.items.first().hidden = true;
    source.items.first().ignored = true;
    emit source.changed();
    require(!row.isNull() && status->text() == QStringLiteral("Closed") &&
                show->text() == QStringLiteral("Restore"),
            "closed status and Restore take precedence over a hidden flag");
}

void pinnedFailedPreviewsRetryOnRefresh(const QVector<storage::PinnedWindowSummary>& records) {
    Fixture source;
    source.items = records;
    source.previewFailure = true;
    PinnedWindowManagementPageWidget page(&source, nullptr);
    page.resize(980, 640);
    page.show();
    QCoreApplication::processEvents();
    QPointer<adqt::widgets::AdImage> failedPreview =
        page.findChild<adqt::widgets::AdImage*>(QStringLiteral("pinnedManagementPreview"));
    QElapsedTimer timer;
    timer.start();
    while (failedPreview && !failedPreview->loadFailed() && timer.elapsed() < 5000) {
        failedPreview->grab();
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(failedPreview && failedPreview->loadFailed(), "seed a transient preview failure");
    source.previewFailure = false;
    emit source.changed();
    QCoreApplication::processEvents();
    auto* recovered =
        page.findChild<adqt::widgets::AdImage*>(QStringLiteral("pinnedManagementPreview"));
    timer.restart();
    while (recovered && (recovered->loading() || source.previewSizes.size() < 3) &&
           timer.elapsed() < 5000) {
        recovered->grab();
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(failedPreview.isNull() && recovered && !recovered->loading() &&
                !recovered->loadFailed(),
            "refresh retries a failed preview with unchanged record IDs and payload revisions");
    const QPointer<adqt::widgets::AdImage> successfulPreview = recovered;
    emit source.changed();
    require(!successfulPreview.isNull(), "refresh preserves a successfully recovered preview");
}

void pinnedPreviewRetriesAcrossStorageMigration(const QString& destination) {
    auto& applicationStorage = storage::ApplicationStorage::instance();
    auto& repository = applicationStorage.pinnedWindows();
    storage::PinnedWindowRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.image = QImage(1600, 900, QImage::Format_RGB32);
    record.image.fill(Qt::yellow);
    record.nativeGeometry = QRect(QPoint(0, 0), record.image.size());
    record.canvasSourceRect = QRectF(record.nativeGeometry);
    record.contentCanvasRect = record.canvasSourceRect;
    record.surfaceCanvasRect = record.canvasSourceRect;
    record.initialWindowSize = record.image.size();
    require(repository.upsert(record).success && repository.flush().success,
            "seed a persisted preview for migration recovery");
    const auto payloadRevision = repository.previewSourceRevision(record.id);
    PinnedWindowManagementPageWidget page;
    auto* source = page.findChild<PinnedWindowManagementDataSource*>();
    int replies = 0;
    bool lastSucceeded = false;
    QImage recoveredPreview;
    QSize recoveredNaturalSize;
    QObject::connect(
        source, &PinnedWindowManagementDataSource::previewReady, &page,
        [&](const QString&, quint64 requestId, const QImage& image, const QSize& naturalSize) {
            ++replies;
            lastSucceeded = !image.isNull();
            if (requestId == 700000) {
                recoveredPreview = image;
                recoveredNaturalSize = naturalSize;
            }
        });

    repository.suspendWrites(true);
    source->requestPreview(record.id, 700000, QSize(800, 600));
    require(applicationStorage.pinnedPreviewPool().waitForDone(5000),
            "queue a rejected preview before delivering the completion generation");
    repository.suspendWrites(false);
    // Deliver the migration completion before the already queued worker reply.
    emit applicationStorage.directoryChangeFinished({true, {}, {}});
    QElapsedTimer timer;
    timer.start();
    while (replies == 0 && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(replies == 1 && lastSucceeded,
            "a failed reply delivered after migration finishes retries the current generation");
    require(recoveredPreview.size() == QSize(800, 450) &&
                recoveredNaturalSize == record.image.size() &&
                recoveredPreview.pixelColor(400, 225) == QColor(Qt::yellow),
            "a migration retry scales pixels while preserving aspect ratio and natural size");
    repository.suspendWrites(true);
    source->requestPreview(record.id, 700003, QSize(37, 19));
    require(applicationStorage.pinnedPreviewPool().waitForDone(5000),
            "queue a preview that remains unavailable after completion");
    emit applicationStorage.directoryChangeFinished({false, {}, {}});
    timer.restart();
    while (replies < 2 && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(replies == 2 && !lastSucceeded,
            "an unavailable preview reports failure after one migration retry");
    repository.suspendWrites(false);

#ifdef Q_OS_WIN
    PinnedWindowManagementPageWidget cancelledPage;
    auto* cancelledSource = cancelledPage.findChild<PinnedWindowManagementDataSource*>();
    int cancelledReplies = 0;
    QObject::connect(cancelledSource, &PinnedWindowManagementDataSource::previewReady,
                     &cancelledPage,
                     [&](const auto&, auto, const auto&, const auto&) { ++cancelledReplies; });
    repository.suspendWrites(true);
    source->requestPreview(record.id, 700001, QSize(33, 15));
    cancelledSource->requestPreview(record.id, 700002, QSize(35, 17));
    require(applicationStorage.pinnedPreviewPool().waitForDone(5000),
            "queue rejected previews before the migration drain");
    QSemaphore migrationDrain;
    applicationStorage.setDirectoryChangeHooks([]() { return storage::StorageResult::ok(); }, {},
                                               [&]() { migrationDrain.acquire(); });
    bool finished = false;
    bool succeeded = false;
    const auto connection =
        QObject::connect(&applicationStorage, &storage::ApplicationStorage::directoryChangeFinished,
                         &page, [&](const auto& result) {
                             finished = true;
                             succeeded = result.success;
                         });
    require(applicationStorage.requestDirectoryChange(destination, true).success,
            "start a migration held at its drain boundary");
    QCoreApplication::processEvents();
    require(applicationStorage.directoryChanging() && replies == 2 && cancelledReplies == 0,
            "rejected previews wait while migration is active");
    cancelledSource->cancelPreviews();
    migrationDrain.release();
    timer.restart();
    while ((!finished || replies < 3) && timer.elapsed() < 15000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    applicationStorage.setDirectoryChangeHooks({}, {}, {});
    QObject::disconnect(connection);
    require(finished && succeeded && replies == 3 && lastSucceeded && cancelledReplies == 0,
            "migration retries a waiting preview once and respects cancellation");
    require(repository.previewSourceRevision(record.id) == payloadRevision,
            "migration recovery does not depend on a changed payload revision");
#else
    Q_UNUSED(destination);
    Q_UNUSED(payloadRevision);
#endif
    require(repository.remove(record.id).success && repository.flush().success,
            "remove the migration recovery fixture");
    static_cast<void>(source->records());
    const auto* recoveredPixels = recoveredPreview.constBits();
    require(snowCanvasDetachImage(recoveredPreview) &&
                recoveredPreview.constBits() == recoveredPixels,
            "a recovered large preview retains writable page-backed storage without copying");
    const auto* middle = recoveredPixels + recoveredPreview.sizeInBytes() / 2;
    recoveredPreview = {};
    require(!snow::test_support::virtualMemoryMapped(middle),
            "releasing a recovered preview and its cache returns large pixel pages to the OS");
}

void triggerBatchStitch(adqt::widgets::AdButton* trigger) {
    trigger->click();
    QCoreApplication::processEvents();
    auto* menu = trigger->window()->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("pinnedManagementBatchActionsMenu"));
    require(menu && menu->isPopupVisible(), "Batch Actions opens its menu");
    menu->setActiveAction(menu->actions().first());
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(menu, &enter);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void pinnedCollageSelectionKeepsRecordOrder() {
    Fixture fixture;
    for (int index = 0; index < 15; ++index) {
        storage::PinnedWindowSummary item;
        item.id = QStringLiteral("collage-%1").arg(index);
        item.activitySequence = static_cast<quint64>(index) + 1;
        item.createdUtc = QDateTime::currentDateTimeUtc();
        fixture.items.append(item);
    }
    PinnedWindowManagementPageWidget page(&fixture, nullptr);
    page.resize(980, 900);
    page.show();
    QCoreApplication::processEvents();
    auto* batchActions =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementBatchActions"));
    auto* selectAll =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementSelectAll"));
    auto* deselect =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementDeselectAll"));
    auto* pagination =
        page.findChild<adqt::widgets::AdPagination*>(QStringLiteral("pinnedManagementPagination"));
    require(batchActions && selectAll && deselect && pagination &&
                batchActions->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Link &&
                batchActions->parentWidget()->layout()->indexOf(batchActions) + 1 ==
                    batchActions->parentWidget()->layout()->indexOf(selectAll),
            "pinned Collage is a link action immediately before Select all");
    require(!batchActions->isVisible() && !batchActions->isEnabled(),
            "pinned Collage is unavailable before selection");
    QStringList requested;
    int requests = 0;
    QObject::connect(&page, &PinnedWindowManagementPageWidget::collageRequested, &page,
                     [&](const QStringList& ids) {
                         requested = ids;
                         ++requests;
                     });
    const auto select = [&](int index) {
        for (auto* row : page.findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"))) {
            if (row->property("recordId").toString() == QStringLiteral("collage-%1").arg(index)) {
                row->findChild<adqt::widgets::AdCheckbox*>()->setChecked(true);
                return;
            }
        }
        require(false, "requested collage record is on the current page");
    };
    select(14);
    require(batchActions->isVisible() && batchActions->isEnabled(),
            "one selected pin enables Collage");
    triggerBatchStitch(batchActions);
    require(requested == QStringList{QStringLiteral("collage-14")},
            "a single selected pin can start a collage");
    pagination->setCurrentPage(2);
    select(0);
    pagination->setCurrentPage(1);
    select(5);
    triggerBatchStitch(batchActions);
    require(requested == QStringList{QStringLiteral("collage-14"), QStringLiteral("collage-5"),
                                     QStringLiteral("collage-0")},
            "collage includes selected pins across pages in displayed record order");
    QEvent language(QEvent::LanguageChange);
    QApplication::sendEvent(&page, &language);
    require(batchActions->text() == QStringLiteral("Batch Actions") &&
                batchActions->accessibleName() == QStringLiteral("Batch Actions"),
            "language changes retranslate the pinned collage action");
    fixture.items.removeLast();
    emit fixture.changed();
    triggerBatchStitch(batchActions);
    require(requested == QStringList{QStringLiteral("collage-5"), QStringLiteral("collage-0")},
            "removed records are excluded from the retained collage selection");
    deselect->click();
    const int previousRequests = requests;
    batchActions->click();
    require(!batchActions->isVisible() && !batchActions->isEnabled() &&
                requests == previousRequests,
            "deselecting every pin disables collage dispatch");
    selectAll->click();
    triggerBatchStitch(batchActions);
    require(requested.size() == 10 && requested.first() == QStringLiteral("collage-13") &&
                requested.last() == QStringLiteral("collage-4"),
            "Select all imports only the current page in its display order");
    page.resize(500, 900);
    QCoreApplication::processEvents();
    const auto* panel = page.findChild<QWidget*>(QStringLiteral("pinnedManagementSelectionPanel"));
    const QPoint actionOrigin = batchActions->parentWidget()->mapTo(panel, QPoint());
    require(actionOrigin.x() >= 0 &&
                actionOrigin.x() + batchActions->parentWidget()->width() <= panel->width(),
            "pinned selection actions fit within a narrow management page");
    page.findChild<adqt::widgets::AdSelect*>()->setCurrentValues(
        {static_cast<int>(storage::PinnedWindowCreationSource::Clipboard)});
    require(!batchActions->isVisible() && !batchActions->isEnabled(),
            "changing the source filter clears the collage selection");
    require(fixture.fullImageRequests == 0 && fixture.removed.isEmpty() && fixture.shown.isEmpty(),
            "selection dispatch neither decodes images nor mutates or opens saved pins");
}

void activatePinnedMenuAction(QMenu* menu, QAction* action) {
    menu->setActiveAction(action);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(menu, &enter);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void batchPinRequiresSingleGroup() {
    Fixture fixture;
    for (int index = 0; index < 2; ++index) {
        storage::PinnedWindowSummary item;
        item.id = QStringLiteral("group-batch-%1").arg(index);
        item.groupId = index == 0 ? QStringLiteral("default") : QStringLiteral("work");
        item.activitySequence = static_cast<quint64>(index);
        fixture.items.append(item);
    }
    PinnedWindowManagementPageWidget page(&fixture, nullptr);
    page.resize(980, 900);
    page.show();
    QCoreApplication::processEvents();
    auto* trigger =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementBatchActions"));
    page.findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementSelectAll"))->click();
    trigger->click();
    QCoreApplication::processEvents();
    auto* menu = page.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("pinnedManagementBatchActionsMenu"));
    require(menu && menu->actions()[0]->isEnabled() && !menu->actions()[1]->isEnabled(),
            "mixed-group selections allow stitching but cannot switch groups during batch pin");
    auto* pin = menu->actions()[1];
    require(pin->toolTip().contains(QStringLiteral("same group")),
            "disabled batch pin explains the group restriction");
    const auto boxes = page.findChildren<adqt::widgets::AdCheckbox*>();
    boxes.first()->setChecked(false);
    require(pin->isEnabled(), "a single remaining group enables the open menu pin action");
    boxes.first()->setChecked(true);
    require(!pin->isEnabled(), "adding another group disables the open menu pin action");
    for (const QString& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        QTranslator translator;
        require(translator.load(QStringLiteral(":/i18n/snow_shot_%1.qm").arg(locale)),
                "load group restriction translations");
        QCoreApplication::installTranslator(&translator);
        QEvent language(QEvent::LanguageChange);
        QApplication::sendEvent(&page, &language);
        require(pin->toolTip() ==
                    translator.translate(
                        "PinnedWindowManagementPageWidget",
                        "Select pinned windows from the same group to pin them to the screen."),
                "the open menu retranslates its mixed-group explanation");
        QCoreApplication::removeTranslator(&translator);
    }
    pin->setEnabled(true);
    pin->trigger();
    require(fixture.shownIds.isEmpty() && fixture.shownBatches.isEmpty(),
            "a stale enabled action revalidates groups before dispatch");
}

void batchPinUsesExistingShowForEveryState() {
    Fixture fixture;
    for (int index = 0; index < 12; ++index) {
        storage::PinnedWindowSummary item;
        item.id = QStringLiteral("batch-%1").arg(index);
        item.activitySequence = static_cast<quint64>(index) + 1;
        item.createdUtc = QDateTime::currentDateTimeUtc();
        item.ignored = index % 3 == 0;
        item.hidden = index % 3 == 1;
        fixture.items.append(item);
    }
    PinnedWindowManagementPageWidget page(&fixture, nullptr);
    page.resize(980, 900);
    page.show();
    QCoreApplication::processEvents();
    auto* trigger =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementBatchActions"));
    auto* selectAll =
        page.findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementSelectAll"));
    auto* pagination =
        page.findChild<adqt::widgets::AdPagination*>(QStringLiteral("pinnedManagementPagination"));
    selectAll->click();
    pagination->setCurrentPage(2);
    selectAll->click();
    int stitched = 0;
    QObject::connect(&page, &PinnedWindowManagementPageWidget::collageRequested, &page,
                     [&](const auto&) { ++stitched; });
    QKeyEvent open(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(trigger, &open);
    QCoreApplication::processEvents();
    QPointer<adqt::widgets::AdContextMenu> menu = page.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("pinnedManagementBatchActionsMenu"));
    require(menu && menu->isPopupVisible() && !menu->nativeMenuEnabled() &&
                menu->actions().size() == 2 && stitched == 0 && fixture.shownIds.isEmpty(),
            "keyboard opens pinned Batch Actions without dispatch");
    auto* stitch = menu->actions()[0];
    auto* pin = menu->actions()[1];
    require(stitch->text() == QStringLiteral("Stitch Image") &&
                pin->text() == QStringLiteral("Pin to screen") && pin->isEnabled() &&
                !stitch->icon().isNull() && !pin->icon().isNull(),
            "pinned batch menu offers themed Stitch Image then Pin to screen");
    for (const QString& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        QTranslator translator;
        require(translator.load(QStringLiteral(":/i18n/snow_shot_%1.qm").arg(locale)),
                "load pinned batch translations");
        QCoreApplication::installTranslator(&translator);
        QEvent language(QEvent::LanguageChange);
        QApplication::sendEvent(&page, &language);
        require(trigger->text() ==
                        translator.translate("PinnedWindowManagementPageWidget", "Batch Actions") &&
                    trigger->accessibleName() == trigger->text() &&
                    stitch->text() ==
                        translator.translate("PinnedWindowManagementPageWidget", "Stitch Image") &&
                    pin->text() ==
                        translator.translate("PinnedWindowManagementPageWidget", "Pin to screen") &&
                    pin->toolTip() == pin->text(),
                "pinned open menu and trigger retranslate together");
        QCoreApplication::removeTranslator(&translator);
    }
    for (auto* row : page.findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"))) {
        if (row->property("recordId").toString() == QStringLiteral("batch-0"))
            row->findChild<adqt::widgets::AdCheckbox*>()->setChecked(false);
    }
    QStringList expected;
    for (int index = 11; index > 0; --index)
        expected.append(QStringLiteral("batch-%1").arg(index));
    activatePinnedMenuAction(menu, pin);
    require(!menu && fixture.shownIds == expected &&
                fixture.shownBatches == QVector<QStringList>{expected},
            "batch pin dispatches every selected closed, hidden, and visible record in order "
            "across pages");
    auto* summary = page.findChild<QLabel*>(QStringLiteral("pinnedManagementSelectionSummary"));
    require(summary && summary->text().contains(QStringLiteral("11")),
            "pinning retains the selection");
    fixture.shownIds.clear();
    fixture.onShow = [&] {
        fixture.items.detach();
        page.refresh();
    };
    trigger->click();
    QCoreApplication::processEvents();
    menu = page.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("pinnedManagementBatchActionsMenu"));
    require(menu, "reopen pinned batch menu");
    activatePinnedMenuAction(menu, menu->actions()[1]);
    require(fixture.shownIds == expected && fixture.removed.isEmpty(),
            "synchronous source refresh cannot invalidate the batch show snapshot");
}

void pinnedEntryMoreActionsUseOnlyTheirRecord() {
    Fixture fixture;
    for (int index = 0; index < 3; ++index) {
        storage::PinnedWindowSummary item;
        item.id = QStringLiteral("stitch-%1").arg(index);
        item.createdUtc = QDateTime::currentDateTimeUtc();
        item.ignored = index == 0;
        fixture.items.append(item);
    }
    PinnedWindowManagementPageWidget page(&fixture, nullptr);
    page.resize(980, 900);
    page.show();
    QCoreApplication::processEvents();
    QStringList requested;
    int requests = 0;
    QString splitId;
    int splitRequests = 0;
    QObject::connect(&page, &PinnedWindowManagementPageWidget::collageRequested, &page,
                     [&](const QStringList& ids) {
                         requested = ids;
                         ++requests;
                     });
    QObject::connect(&page, &PinnedWindowManagementPageWidget::splitRequested, &page,
                     [&](const QString& id) {
                         splitId = id;
                         ++splitRequests;
                     });
    const QPoint previousCursor = QCursor::pos();
    const auto restoreCursor = qScopeGuard([previousCursor] { QCursor::setPos(previousCursor); });
    QString editorId;
    QObject::connect(&page, &PinnedWindowManagementPageWidget::imageEditingRequested, &page,
                     [&](const QString& id) { editorId = id; });
    const auto rows = page.findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"));
    require(rows.size() == 3, "stitching fixtures include closed and open pins");
    auto* selection = rows.back()->findChild<adqt::widgets::AdCheckbox*>();
    selection->setChecked(true);
    for (auto* row : rows) {
        auto* show =
            row->findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementEntryShow"));
        auto* more =
            row->findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementEntryMore"));
        auto* remove =
            row->findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementEntryDelete"));
        require(show && more && remove && more->isVisible() && more->isEnabled() &&
                    more->text() == QStringLiteral("More") && more->iconRef().isValid() &&
                    more->toolTip() == QStringLiteral("More actions") &&
                    more->accessibleName() == QStringLiteral("More actions") &&
                    !row->findChild<adqt::widgets::AdButton*>(
                        QStringLiteral("pinnedManagementEntryStitch")),
                "closed and open pins replace the Stitch Image button with More");
        bool adjacent = false;
        for (auto* layout : show->parentWidget()->findChildren<QBoxLayout*>()) {
            adjacent |= layout->indexOf(show) >= 0 &&
                        layout->indexOf(remove) == layout->indexOf(show) + 1 &&
                        layout->indexOf(more) == layout->indexOf(remove) + 1;
        }
        require(adjacent && more->geometry().left() > remove->geometry().right(),
                "More sits immediately to the right of Delete");
        require(!row->findChild<adqt::widgets::AdContextMenu*>(
                    QStringLiteral("pinnedManagementEntryMoreMenu")),
                "More creates its menu only when opened");
        const QPoint local = more->rect().center();
        const QPoint global = more->mapToGlobal(local);
        QCursor::setPos(global);
        QEnterEvent hover(local, local, global);
        QApplication::sendEvent(more, &hover);
        QCoreApplication::processEvents();
        QPointer<adqt::widgets::AdContextMenu> menu = row->findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("pinnedManagementEntryMoreMenu"));
        require(menu && menu->isPopupVisible() && !menu->nativeMenuEnabled() &&
                    menu->actions().size() == 4,
                "hovering More opens the shared widget menu");
        auto* stitch = menu->actions().at(0);
        auto* split = menu->actions().at(1);
        auto* reveal = menu->actions().at(3);
        auto* imageEditing =
            menu->findChild<QAction*>(QStringLiteral("pinnedManagementEntryImageEditing"));
        require(imageEditing && menu->actions().at(2) == imageEditing &&
                    imageEditing->isEnabled() &&
                    imageEditing->text() == QStringLiteral("Image Editing") &&
                    !imageEditing->icon().isNull(),
                "independent editor follows Image Splitting for open and closed pins");
        require(reveal->objectName() == QStringLiteral("pinnedManagementEntryReveal") &&
                    reveal->text() == presentation::components::history_page::revealActionText() &&
                    reveal->toolTip() == reveal->text() && reveal->isEnabled() &&
                    !reveal->icon().isNull(),
                "More offers a themed, platform-specific reveal action after image editing");
        require(stitch->text() == QStringLiteral("Stitch Image") && stitch->isEnabled() &&
                    !stitch->icon().isNull() &&
                    split->text() == QStringLiteral("Image Splitting") && split->isEnabled() &&
                    !split->icon().isNull(),
                "More offers Stitch Image followed by Image Splitting with their icons");
        activatePinnedMenuAction(menu, stitch);
        require(requested == QStringList{row->property("recordId").toString()},
                "entry stitching imports exactly that saved pin");
        require(!menu, "stitching closes and retires its menu");
        more->click();
        QCoreApplication::processEvents();
        menu = row->findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("pinnedManagementEntryMoreMenu"));
        require(menu && menu->isPopupVisible(), "clicking More reopens the menu");
        activatePinnedMenuAction(menu, menu->actions().at(1));
        require(splitId == row->property("recordId").toString() && !menu,
                "splitting dispatches only its saved pin and retires the menu");
        more->click();
        QCoreApplication::processEvents();
        menu = row->findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("pinnedManagementEntryMoreMenu"));
        require(menu, "reopen More for reveal");
        activatePinnedMenuAction(menu, menu->actions().at(3));
        require(!menu && fixture.revealed.last() == row->property("recordId").toString(),
                "reveal dispatches only its row and retires the menu");
        more->click();
        QCoreApplication::processEvents();
        menu = row->findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("pinnedManagementEntryMoreMenu"));
        require(menu, "reopen More for independent editing");
        activatePinnedMenuAction(
            menu, menu->findChild<QAction*>(QStringLiteral("pinnedManagementEntryImageEditing")));
        require(editorId == row->property("recordId").toString() && !menu,
                "Image Editing dispatches exactly its pin and retires the menu");
    }
    require(requests == 3 && splitRequests == 3 && fixture.revealed.size() == 3 &&
                selection->isChecked() && fixture.fullImageRequests == 0 &&
                fixture.shown.isEmpty() && fixture.removed.isEmpty(),
            "image action dispatch preserves selection and pins without restoring or decoding");

    auto* more = rows.front()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("pinnedManagementEntryMore"));
    more->click();
    QCoreApplication::processEvents();
    QPointer<adqt::widgets::AdContextMenu> menu =
        rows.front()->findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("pinnedManagementEntryMoreMenu"));
    require(menu && menu->isPopupVisible(), "translation fixture keeps a menu open");
    for (const QString& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        QTranslator translator;
        require(translator.load(QStringLiteral(":/i18n/snow_shot_%1.qm").arg(locale)),
                "load translations for the pinned More menu");
        QCoreApplication::installTranslator(&translator);
        QEvent language(QEvent::LanguageChange);
        QApplication::sendEvent(&page, &language);
        const QString translatedMore =
            translator.translate("PinnedWindowManagementPageWidget", "More");
        const QString translatedActions =
            translator.translate("PinnedWindowManagementPageWidget", "More actions");
        require(!translatedMore.isEmpty() && !translatedActions.isEmpty() &&
                    more->text() == translatedMore && more->toolTip() == translatedActions &&
                    more->accessibleName() == translatedActions,
                "language changes translate More and its accessible name");
        require(
            menu && menu->isPopupVisible() &&
                menu->actions().at(0)->text() ==
                    translator.translate("PinnedWindowManagementPageWidget", "Stitch Image") &&
                menu->actions().at(0)->toolTip() == menu->actions().at(0)->text() &&
                menu->actions().at(1)->text() ==
                    translator.translate("PinnedWindowManagementPageWidget", "Image Splitting") &&
                menu->actions().at(1)->toolTip() == menu->actions().at(1)->text(),
            "language changes retranslate both actions in the open menu");
        const QString revealLabel = presentation::components::history_page::revealActionText();
        require(menu->actions().at(3)->text() == revealLabel &&
                    menu->actions().at(3)->toolTip() == revealLabel &&
                    !translator.translate("HistoryFileLocation", "Open in Explorer").isEmpty() &&
                    !translator.translate("HistoryFileLocation", "Reveal in Finder").isEmpty(),
                "language changes retranslate the platform reveal action in the open menu");
        QCoreApplication::removeTranslator(&translator);
    }
    menu->dismissPopup();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    require(!menu && more->isVisible(), "dismissal retires the menu and leaves More usable");
    QString failureMessage;
    auto* messages = adqt::widgets::AdMessageService::instance(&page);
    QObject::connect(
        messages, &adqt::widgets::AdMessage::messageOpened, &page,
        [&](adqt::widgets::AdMessageHandle* handle) { failureMessage = handle->content(); });
    fixture.revealSucceeds = false;
    more->click();
    QCoreApplication::processEvents();
    menu = rows.front()->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("pinnedManagementEntryMoreMenu"));
    require(menu, "reopen More for failure fixture");
    activatePinnedMenuAction(menu, menu->actions().at(3));
    require(!menu &&
                failureMessage == QStringLiteral("Unable to open this entry's file location.") &&
                QApplication::activeModalWidget() == nullptr && fixture.removed.isEmpty() &&
                fixture.shown.isEmpty() && selection->isChecked(),
            "failed pin reveal reports a non-modal error and preserves pins and selection");
    adqt::widgets::AdMessageService::destroyAll(&page);
    more->click();
    QCoreApplication::processEvents();
    menu = rows.front()->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("pinnedManagementEntryMoreMenu"));
    for (auto& item : fixture.items) {
        if (item.id == rows.front()->property("recordId").toString())
            item.pending = true;
    }
    page.refresh();
    require(menu && !menu->actions().at(3)->isEnabled(),
            "pending pins retain a visible disabled reveal action");
    fixture.items.clear();
    page.refresh();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!menu && page.findChildren<adqt::widgets::AdContextMenu*>().isEmpty(),
            "removing a pinned row releases its open More menu");
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir directory;
    QTemporaryDir migratedDirectory;
    require(storage::ApplicationStorage::instance()
                .initialize({directory.path(), directory.path(), 30000})
                .success,
            "isolated storage");
    if (application.arguments().contains(QStringLiteral("--sticky-only"))) {
        selectionPanelSticksWhileScrolling();
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    pinnedEntryMoreActionsUseOnlyTheirRecord();
    pinnedCollageSelectionKeepsRecordOrder();
    batchPinRequiresSingleGroup();
    batchPinUsesExistingShowForEveryState();
    if (QApplication::arguments().contains(QStringLiteral("--collage-only"))) {
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    Fixture fixture;
    const auto today = QDateTime(QDate(2026, 9, 24), QTime(0, 30), QTimeZone::systemTimeZone());
    storage::PinnedWindowSummary first;
    first.id = QStringLiteral("first");
    first.createdUtc = today.addDays(-2).toUTC();
    first.creationSource = storage::PinnedWindowCreationSource::Clipboard;
    first.groupId = QStringLiteral("work");
    storage::PinnedWindowSummary second;
    second.id = QStringLiteral("second");
    second.createdUtc = today.addDays(-5).toUTC();
    second.lastClosedUtc = today.toUTC();
    second.ignored = true;
    second.creationSource = storage::PinnedWindowCreationSource::Screenshot;
    fixture.items = {first, second};
    pinnedDescriptionsUseSmallThemeText(fixture.items);
    pinnedThemeChangesDoNotRestyleTwice(fixture.items);
    pinnedRowsRespectSkinMask(fixture.items);
    selectionPanelSticksWhileScrolling();
    pinnedChangesRefreshOnlyVisiblePages(fixture.items);
    pinnedHiddenRecordsUseShowAction(first);
    pinnedFailedPreviewsRetryOnRefresh(fixture.items);
    pinnedPreviewRetriesAcrossStorageMigration(migratedDirectory.path());
    {
        Fixture many;
        for (int index = 0; index < 15; ++index) {
            auto record = first;
            record.id = QStringLiteral("many-%1").arg(index);
            record.createdUtc = today.addSecs(-index);
            record.creationSource = index % 2 == 0
                                        ? storage::PinnedWindowCreationSource::Clipboard
                                        : storage::PinnedWindowCreationSource::Screenshot;
            many.items.push_back(record);
        }
        PinnedWindowManagementPageWidget page(&many, nullptr);
        auto* viewer = page.findChild<adqt::widgets::AdImageViewer*>();
        auto* pagination = page.findChild<adqt::widgets::AdPagination*>();
        require(viewer->rowCount() == 15, "pinned viewer includes records beyond the first page");
        viewer->openAt(9);
        viewer->activate(1);
        application.processEvents();
        require(viewer->currentRow() == 10 && many.lastFullImageId == QStringLiteral("many-10"),
                "pinned next control crosses page boundaries");
        viewer->close();
        pagination->setCurrentPage(2);
        application.processEvents();
        auto* preview =
            page.findChild<adqt::widgets::AdImage*>(QStringLiteral("pinnedManagementPreview"));
        require(preview && preview->previewRow() == 10,
                "second-page thumbnails use absolute preview rows");
        auto* filter = page.findChild<adqt::widgets::AdSelect*>();
        filter->setCurrentValues(
            {static_cast<int>(storage::PinnedWindowCreationSource::Clipboard)});
        application.processEvents();
        require(viewer->rowCount() == 8 &&
                    viewer->itemAt(1).source.path() == QStringLiteral("many-2"),
                "pinned continuous preview follows active filters");
        viewer->openAt(0);
        many.items.removeFirst();
        page.refresh();
        require(!viewer->isVisible() && viewer->rowCount() == 7,
                "refresh closes stale pinned preview and removes deleted records");
    }
    {
        QStackedWidget pages;
        pages.resize(980, 640);
        pages.addWidget(new QWidget);
        pages.show();
        application.processEvents();
        auto* pinnedPage = new PinnedWindowManagementPageWidget(&fixture, &pages);
        pages.addWidget(pinnedPage);
        pages.setCurrentWidget(pinnedPage);
        application.processEvents();
        auto* entries = pinnedPage->findChild<QWidget*>(QStringLiteral("pinnedManagementEntries"));
        const auto rows =
            pinnedPage->findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"));
        require(entries != nullptr && entries->width() >= 560 && rows.size() == 2,
                "switching to pinned management lays out wide records");
        for (auto* row : rows) {
            auto* rowLayout = dynamic_cast<QBoxLayout*>(row->layout());
            auto* group = row->findChild<QLabel*>(QStringLiteral("pinnedManagementGroup"));
            auto* preview = row->findChild<QWidget*>(QStringLiteral("pinnedManagementPreview"));
            require(rowLayout != nullptr && rowLayout->direction() == QBoxLayout::LeftToRight,
                    "pinned record preview stays beside its description after navigation");
            require(group != nullptr && preview != nullptr &&
                        preview->x() > group->mapTo(row, QPoint(group->width(), 0)).x(),
                    "pinned record image is positioned to the right of its description");
        }
        pages.resize(500, 640);
        application.processEvents();
        require(entries->width() < 560, "narrow navigation test reaches the responsive breakpoint");
        for (auto* row : rows) {
            auto* rowLayout = dynamic_cast<QBoxLayout*>(row->layout());
            auto* group = row->findChild<QLabel*>(QStringLiteral("pinnedManagementGroup"));
            auto* preview = row->findChild<QWidget*>(QStringLiteral("pinnedManagementPreview"));
            require(rowLayout->direction() == QBoxLayout::TopToBottom,
                    "pinned records stack on narrow pages");
            require(preview->y() > group->mapTo(row, QPoint(0, group->height())).y(),
                    "pinned record image follows its description on narrow pages");
        }
        pages.resize(980, 640);
        application.processEvents();
        for (auto* row : rows) {
            auto* rowLayout = dynamic_cast<QBoxLayout*>(row->layout());
            require(rowLayout->direction() == QBoxLayout::LeftToRight,
                    "pinned records return to one row when the page widens");
        }
    }
    {
        fixture.previewSizes.clear();
        PinnedWindowManagementPageWidget page(&fixture, nullptr);
        page.resize(980, 640);
        page.show();
        application.processEvents();
        const auto rows = [&]() {
            return page.findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"));
        };
        require(rows().size() == 2 &&
                    rows().front()->property("recordId") == QStringLiteral("second"),
                "closed activity sorts first");
        auto* sharedImage = rows().front()->findChild<adqt::widgets::AdImage*>(
            QStringLiteral("pinnedManagementPreview"));
        QElapsedTimer thumbnailTimer;
        thumbnailTimer.start();
        while (sharedImage != nullptr && fixture.previewSizes.isEmpty() &&
               thumbnailTimer.elapsed() < 5000) {
            sharedImage->grab();
            application.processEvents();
            QThread::msleep(1);
        }
        require(sharedImage != nullptr && !sharedImage->loading() && !sharedImage->loadFailed() &&
                    !fixture.previewSizes.isEmpty() && fixture.previewSizes.front().isValid() &&
                    fixture.fullImageRequests == 0,
                "pinned thumbnails use AdImage and request bounded previews without full images");
        auto* viewer = sharedImage->viewer();
        require(viewer != nullptr && viewer->rowCount() == 2 && sharedImage->previewRow() == 0,
                "pinned preview contains every record in activity order");
        viewer->openAt(sharedImage->previewRow());
        viewer->activate(1);
        application.processEvents();
        require(viewer->currentRow() == 1 && fixture.lastFullImageId == QStringLiteral("first"),
                "next preview loads the next record, not the originally clicked record");
        viewer->activate(-1);
        application.processEvents();
        require(viewer->currentRow() == 0 && fixture.lastFullImageId == QStringLiteral("second"),
                "previous preview returns to the original record");
        viewer->close();
        require(page.findChild<QWidget*>(QStringLiteral("pinnedManagementPageContainer")) &&
                    page.findChild<QWidget*>(QStringLiteral("pinnedManagementSelectionBar")) &&
                    page.findChild<adqt::widgets::AdPagination*>(
                        QStringLiteral("pinnedManagementPagination")),
                "paste image management uses the history page's container, selection, and "
                "pagination components");
        bool hasGroup = false;
        for (auto* label : page.findChildren<QLabel*>())
            hasGroup |= label->text() == QStringLiteral("Group: Work");
        require(hasGroup, "each row displays its group");
        for (auto* button : rows().front()->findChildren<adqt::widgets::AdButton*>())
            if (button->text() == QStringLiteral("Restore"))
                button->click();
        require(fixture.shown == QStringLiteral("second"), "restore dispatches existing record ID");
        QPointer<QFrame> restoredRow = rows().front();
        fixture.items[1].ignored = false;
        emit fixture.changed();
        require(!restoredRow.isNull() && restoredRow
                                                 ->findChild<adqt::widgets::AdButton*>(
                                                     QStringLiteral("pinnedManagementEntryShow"))
                                                 ->text() == QStringLiteral("Show"),
                "restoring a record updates its existing row");
        fixture.items[1].ignored = true;
        emit fixture.changed();
        require(!restoredRow.isNull() && restoredRow
                                                 ->findChild<adqt::widgets::AdButton*>(
                                                     QStringLiteral("pinnedManagementEntryShow"))
                                                 ->text() == QStringLiteral("Restore"),
                "closing a record reuses its preview row");
        auto* dates = page.findChild<adqt::widgets::AdDateRangePicker*>();
        dates->setStartDate(today.date());
        dates->setEndDate(today.date());
        require(rows().size() == 1 &&
                    rows().front()->property("recordId") == QStringLiteral("second"),
                "date filter uses local close date");
        auto* source = page.findChild<adqt::widgets::AdSelect*>();
        source->setCurrentValues(
            {static_cast<int>(storage::PinnedWindowCreationSource::Clipboard)});
        require(rows().isEmpty(), "source and date filters compose");
        require(page.findChild<QLabel*>(QStringLiteral("pinnedManagementEmptyTitle"))->text() ==
                    QStringLiteral("No matching pinned windows"),
                "empty filtered results show the history-style empty state");
        source->setCurrentValues({});
        dates->setStartDate({});
        dates->setEndDate({});
        require(rows().size() == 2, "clearing filters restores all records");
        QEvent language(QEvent::LanguageChange);
        QApplication::sendEvent(&page, &language);
        require(rows().size() == 2, "language change preserves records");
        application.processEvents();
        require(rows().front()->isVisible() && rows().front()->height() > 100,
                "rebuilt records must remain visible after language changes");
        auto* singleDelete = page.findChild<adqt::widgets::AdPopconfirm*>(
            QStringLiteral("pinnedManagementEntryDeleteConfirm"));
        require(page.findChildren<adqt::widgets::AdPopconfirm*>(
                        QStringLiteral("pinnedManagementEntryDeleteConfirm"))
                        .size() == 1,
                "one delete confirmation serves every visible row");
        rows()
            .front()
            ->findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementEntryDelete"))
            ->click();
        require(QMetaObject::invokeMethod(singleDelete, "accepted", Qt::DirectConnection),
                "accept single deletion");
        require(fixture.removed == QVector<QString>{QStringLiteral("second")},
                "single deletion dispatches only its record");
        rows().front()->findChild<adqt::widgets::AdCheckbox*>()->setChecked(true);
        require(
            page.findChild<QWidget*>(QStringLiteral("pinnedManagementSelectionBar"))->isVisible(),
            "selection actions appear when an entry is selected");
        auto* countLabel = page.findChild<QLabel*>(QStringLiteral("pinnedManagementCountLabel"));
        auto* selectionLabel =
            page.findChild<QLabel*>(QStringLiteral("pinnedManagementSelectionSummary"));
        require(countLabel != nullptr && selectionLabel != nullptr &&
                    countLabel->font().pixelSize() == selectionLabel->font().pixelSize() &&
                    countLabel->font().weight() == selectionLabel->font().weight(),
                "selected pinned records keep the same text style as the page subtitle");
        auto* selectedDelete = page.findChild<adqt::widgets::AdPopconfirm*>(
            QStringLiteral("pinnedManagementDeleteSelectedConfirm"));
        QMetaObject::invokeMethod(selectedDelete, "accepted", Qt::DirectConnection);
        require(fixture.removed == QVector<QString>{QStringLiteral("second")},
                "bulk selection deletes selected IDs");
        auto* allDelete = page.findChild<adqt::widgets::AdPopconfirm*>(
            QStringLiteral("pinnedManagementDeleteAllConfirm"));
        QMetaObject::invokeMethod(allDelete, "accepted", Qt::DirectConnection);
        require(fixture.removed.size() == 2, "delete all covers all groups");
        if (qEnvironmentVariableIsSet("SNOW_PIN_MANAGEMENT_RENDER"))
            page.grab().save(qEnvironmentVariable("SNOW_PIN_MANAGEMENT_RENDER"));
        fixture.items.removeFirst();
        emit fixture.changed();
        require(rows().size() == 1, "repository changes update page");
    }
    {
        Fixture many;
        for (int index = 0; index < 50; ++index) {
            storage::PinnedWindowSummary item;
            item.id = QStringLiteral("many-%1").arg(index);
            item.createdUtc = today.toUTC();
            many.items.push_back(item);
        }
        PinnedWindowManagementPageWidget page(&many, nullptr);
        page.resize(980, 640);
        page.show();
        auto* pagination = page.findChild<adqt::widgets::AdPagination*>(
            QStringLiteral("pinnedManagementPagination"));
        pagination->setPageSize(50);
        const auto restorePageSize =
            qScopeGuard([] { storage::InterfaceSettings().setPinnedWindowManagementPageSize(10); });
        application.processEvents();
        QHash<QString, QPointer<QFrame>> originalRows;
        for (auto* row : page.findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord")))
            originalRows.insert(row->property("recordId").toString(), row);
        require(originalRows.size() == 50 &&
                    page.findChildren<adqt::widgets::AdPopconfirm*>(
                            QStringLiteral("pinnedManagementEntryDeleteConfirm"))
                            .size() == 1,
                "a full page keeps one delete confirmation");
        many.items[25].ignored = true;
        many.items[25].lastClosedUtc = today.toUTC();
        emit many.changed();
        application.processEvents();
        for (auto* row : page.findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"))) {
            const QString id = row->property("recordId").toString();
            require(originalRows.value(id) == row,
                    "a close event reorders a full page without recreating preview rows");
        }
    }
    // Exercise the production asynchronous preview path with all persisted content kinds.
    auto& repository = storage::ApplicationStorage::instance().pinnedWindows();
    QImage base(640, 384, QImage::Format_RGB32);
    base.fill(Qt::cyan);
    const auto sourcePath = directory.filePath(QStringLiteral("original.png"));
    require(base.save(sourcePath), "write preview file source");
    QString imageId;
    for (const auto kind : {storage::PinnedWindowSourceKind::ImageData,
                            storage::PinnedWindowSourceKind::ClipboardImageFile,
                            storage::PinnedWindowSourceKind::ClipboardText}) {
        storage::PinnedWindowRecord record;
        record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        record.sourceKind = kind;
        record.image = base;
        record.nativeGeometry = QRect(QPoint(0, 0), base.size());
        record.canvasSourceRect = QRectF(record.nativeGeometry);
        record.contentCanvasRect = record.canvasSourceRect;
        record.surfaceCanvasRect = record.canvasSourceRect;
        record.initialWindowSize = base.size();
        // A preview must not parse or render pinned drawing/recognition payloads.
        record.canvasSession = QByteArrayLiteral("drawing payload is intentionally not a canvas");
        record.recognitionResults = QByteArrayLiteral("recognition overlay");
        if (kind == storage::PinnedWindowSourceKind::ClipboardImageFile) {
            record.originalFilePath = sourcePath;
            record.originalFileName = QStringLiteral("original.png");
        } else if (kind == storage::PinnedWindowSourceKind::ClipboardText) {
            record.originalText = QStringLiteral("Retained text pin");
            record.image = {};
        }
        require(repository.upsert(record).success, "create production preview record");
        if (kind == storage::PinnedWindowSourceKind::ImageData)
            imageId = record.id;
    }
    require(repository.flush().success && QFile::remove(sourcePath),
            "previews must use persisted payloads after the original file is removed");
    {
        PinnedWindowManagementPageWidget page;
        int completedPreviews = 0;
        QSize imageThumbnailSize;
        QSize imageNaturalSize;
        auto* pageSource = page.findChild<PinnedWindowManagementDataSource*>();
        require(pageSource != nullptr, "production page owns its preview source");
        QObject::connect(pageSource, &PinnedWindowManagementDataSource::previewReady, &page,
                         [&completedPreviews, &imageId, &imageThumbnailSize,
                          &imageNaturalSize](const QString& id, quint64, const QImage& image,
                                             const QSize& naturalSize) {
                             ++completedPreviews;
                             if (id == imageId) {
                                 imageThumbnailSize = image.size();
                                 imageNaturalSize = naturalSize;
                             }
                         });
        QObject::connect(&storage::ApplicationStorage::instance(),
                         &storage::ApplicationStorage::pinnedWindowDeleteRequested, &page,
                         [&repository](const QVector<QString>& ids) {
                             for (const auto& id : ids)
                                 require(repository.remove(id).success, "remove requested pin");
                         });
        page.resize(980, 900);
        page.show();
        const auto ready = [&]() {
            const auto previews = page.findChildren<adqt::widgets::AdImage*>(
                QStringLiteral("pinnedManagementPreview"));
            if (previews.size() != 3 || completedPreviews < 3)
                return false;
            for (auto* preview : previews)
                if (preview->loading() || preview->loadFailed())
                    return false;
            return true;
        };
        QElapsedTimer timer;
        timer.start();
        while (!ready() && timer.elapsed() < 5000) {
            application.processEvents();
            QThread::msleep(1);
        }
        require(ready(), "all content kinds produce asynchronous previews");
        require(imageNaturalSize == base.size() && imageThumbnailSize.width() <= 320 &&
                    imageThumbnailSize.height() <= 192,
                "large pinned images keep natural dimensions while thumbnails stay bounded");
        bool explicitPreviewReady = false;
        constexpr quint64 kCacheProbeRequestId = 100000;
        QObject::connect(pageSource, &PinnedWindowManagementDataSource::previewReady, &page,
                         [&explicitPreviewReady, &imageId](const QString& id, quint64 requestId,
                                                           const QImage& image, const QSize&) {
                             if (id == imageId && requestId == kCacheProbeRequestId)
                                 explicitPreviewReady = !image.isNull();
                         });
        pageSource->requestPreview(imageId, kCacheProbeRequestId, QSize(260, 156));
        timer.restart();
        while (!explicitPreviewReady && timer.elapsed() < 5000) {
            application.processEvents();
            QThread::msleep(1);
        }
        require(explicitPreviewReady, "explicit pinned thumbnail request completes");
        const QString cacheKey = QStringLiteral("pinned|") + imageId + u':' +
                                 QString::number(*repository.previewSourceRevision(imageId)) +
                                 QStringLiteral(":260x156");
        const auto cached = snow_shot::presentation::components::thumbnail_cache::load(
            snow_shot::presentation::components::thumbnail_cache::pathForKey(cacheKey));
        require(!cached.image.isNull() && cached.image.size() == imageThumbnailSize &&
                    cached.naturalSize == base.size(),
                "pinned thumbnails share the persistent bounded cache and retain natural size");
        int baseImages = 0;
        for (auto* preview : page.findChildren<adqt::widgets::AdImage*>(
                 QStringLiteral("pinnedManagementPreview"))) {
            const auto image = preview->grab().toImage();
            if (image.pixelColor(image.width() / 2, image.height() / 2) == QColor(Qt::cyan))
                ++baseImages;
        }
        require(baseImages == 2, "image previews show base pixels without drawings or overlays");
        QFrame* imageRow = nullptr;
        for (auto* candidate :
             page.findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"))) {
            if (candidate->property("recordId").toString() == imageId) {
                imageRow = candidate;
                break;
            }
        }
        require(imageRow != nullptr, "saved image has a management row");
        auto* imagePreview =
            imageRow->findChild<adqt::widgets::AdImage*>(QStringLiteral("pinnedManagementPreview"));
        auto* viewer = imagePreview->viewer();
        require(imagePreview != nullptr && viewer != nullptr && viewer->rowCount() == 3,
                "saved image has a preview viewer");
        const QColor idleCorner = imagePreview->grab().toImage().pixelColor(20, 20);
        const QPointF hoverPoint = imagePreview->rect().center();
        QEnterEvent hover(hoverPoint, hoverPoint, imagePreview->mapToGlobal(hoverPoint.toPoint()));
        QApplication::sendEvent(imagePreview, &hover);
        require(imagePreview->grab().toImage().pixelColor(20, 20) != idleCorner,
                "hovering a pinned image draws the shared Preview overlay");
        QSize previewSize;
        QObject::connect(viewer, &adqt::widgets::AdImageViewer::currentItemChanged, &page,
                         [&previewSize](int, int, const adqt::widgets::AdImageItem&,
                                        const QSize& size) { previewSize = size; });
        const QPointF previewLocal = imagePreview->rect().center();
        const QPointF previewGlobal = imagePreview->mapToGlobal(previewLocal.toPoint());
        QMouseEvent previewPress(QEvent::MouseButtonPress, previewLocal, previewGlobal,
                                 Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent previewRelease(QEvent::MouseButtonRelease, previewLocal, previewGlobal,
                                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(imagePreview, &previewPress);
        QApplication::sendEvent(imagePreview, &previewRelease);
        timer.restart();
        while (previewSize.isEmpty() && timer.elapsed() < 5000) {
            application.processEvents();
            QThread::msleep(1);
        }
        require(viewer->isVisible() && previewSize == base.size(),
                "clicking the thumbnail opens the full-resolution saved image");
        viewer->close();
        imagePreview->setFocus();
        QKeyEvent previewKey(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(imagePreview, &previewKey);
        require(viewer->isVisible(), "keyboard activation opens the image preview");
        viewer->close();
        auto* row = page.findChild<QFrame*>(QStringLiteral("pinnedManagementRecord"));
        require(row != nullptr, "production page shows a deletable record");
        const QString removedId = row->property("recordId").toString();
        auto* remove =
            row->findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementEntryDelete"));
        auto* confirmation = page.findChild<adqt::widgets::AdPopconfirm*>(
            QStringLiteral("pinnedManagementEntryDeleteConfirm"));
        require(remove != nullptr && confirmation != nullptr,
                "record has a Delete action and confirmation");
        const QPointF local = remove->rect().center();
        const QPointF global = remove->mapToGlobal(local.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(remove, &press);
        QApplication::sendEvent(remove, &release);
        application.processEvents();
        require(confirmation->isVisible(), "Delete opens its confirmation");
        auto* accept = confirmation->button(adqt::widgets::AdPopconfirm::StandardButton::Ok);
        require(accept != nullptr, "confirmation exposes its Delete button");
        accept->click();
        application.processEvents();
        require(!repository.loadRecord(removedId), "confirmed Delete removes the record");
        const auto remainingRows =
            page.findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"));
        require(!confirmation->isVisible() && remainingRows.size() == 2 &&
                    std::none_of(remainingRows.cbegin(), remainingRows.cend(),
                                 [&removedId](const QFrame* candidate) {
                                     return candidate->property("recordId").toString() == removedId;
                                 }),
                "Delete closes its popup and refreshes the remaining rows");
        const QString selectedId = remainingRows.front()->property("recordId").toString();
        remainingRows.front()->findChild<adqt::widgets::AdCheckbox*>()->setChecked(true);
        auto* selectedConfirmation = page.findChild<adqt::widgets::AdPopconfirm*>(
            QStringLiteral("pinnedManagementDeleteSelectedConfirm"));
        require(selectedConfirmation != nullptr, "selected Delete has a confirmation");
        selectedConfirmation->show();
        application.processEvents();
        require(selectedConfirmation->isVisible(), "selected Delete opens its confirmation");
        selectedConfirmation->button(adqt::widgets::AdPopconfirm::StandardButton::Ok)->click();
        application.processEvents();
        require(!repository.loadRecord(selectedId) &&
                    page.findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord")).size() ==
                        1,
                "selected Delete refreshes the page after removing its record");
    }
    {
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.text = QStringLiteral("#7DD35380");
        snapshot.devicePixelRatio = 1.25;
        snapshot.colorSwatchStyle = capturePinnedColorSwatchStyle();
        const auto content = ScreenshotClipboardContentReader::decode(snapshot);
        require(content.has_value(), "prepare swatch preview source");
        storage::PinnedWindowRecord record;
        record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        record.sourceKind = storage::PinnedWindowSourceKind::ClipboardText;
        record.originalText = snapshot.text;
        record.textStyle = content->originalContent.textStyle;
        record.firstCreationTextDpi = snapshot.devicePixelRatio;
        record.nativeGeometry = QRect(0, 0, 342, 210);
        record.canvasSourceRect = QRectF(record.nativeGeometry);
        record.contentCanvasRect = record.canvasSourceRect;
        record.surfaceCanvasRect = record.canvasSourceRect;
        record.initialWindowSize = record.nativeGeometry.size();
        require(repository.upsert(record).success && repository.flush().success,
                "persist swatch preview source");
        PinnedWindowManagementPageWidget page;
        auto* source = page.findChild<PinnedWindowManagementDataSource*>();
        QImage thumbnail;
        QImage fullImage;
        QSize naturalSize;
        QObject::connect(source, &PinnedWindowManagementDataSource::previewReady, &page,
                         [&](const QString& id, quint64, const QImage& image, const QSize& size) {
                             if (id == record.id) {
                                 thumbnail = image;
                                 naturalSize = size;
                             }
                         });
        QObject::connect(source, &PinnedWindowManagementDataSource::fullImageReady, &page,
                         [&](const QString& id, quint64, const QImage& image) {
                             if (id == record.id)
                                 fullImage = image;
                         });
        const auto waitFor = [&](const auto& ready) {
            QElapsedTimer timer;
            timer.start();
            while (!ready() && timer.elapsed() < 5000) {
                application.processEvents();
                QThread::msleep(1);
            }
            return ready();
        };
        page.resize(980, 900);
        page.show();
        require(waitFor([&] { return !thumbnail.isNull(); }) &&
                    naturalSize == content->image.size() &&
                    thumbnail.pixelColor(0, 0) == content->image.pixelColor(0, 0),
                "swatch thumbnails use the shared renderer and preserve fractional DPI");
        source->requestFullImage(record.id, 1);
        require(waitFor([&] { return !fullImage.isNull(); }) && fullImage == content->image,
                "full swatch previews match the pinned raster");
        auto& theme = adqt::theme::ThemeManager::instance();
        const auto oldTheme = theme.config();
        const auto restoreTheme = qScopeGuard([&] { theme.setConfig(oldTheme); });
        const auto revision = source->previewRevision(record.id);
        const QColor background = thumbnail.pixelColor(0, 0);
        theme.setColorScheme(oldTheme.scheme == adqt::theme::ThemeScheme::Dark
                                 ? adqt::theme::ThemeScheme::Light
                                 : adqt::theme::ThemeScheme::Dark);
        require(source->previewRevision(record.id) != revision &&
                    waitFor([&] { return thumbnail.pixelColor(0, 0) != background; }),
                "theme changes invalidate cached swatches and refresh visible previews");
        class SwatchTranslator final : public QTranslator {
          public:
            QString translate(const char* context, const char* text, const char*,
                              int) const override {
                return QByteArray(context) == "PinnedColorSwatch" && QByteArray(text) == "Color:"
                           ? QStringLiteral("Colour:")
                           : QString();
            }
        } translator;
        const QImage beforeLanguage = thumbnail;
        const auto beforeLanguageRevision = source->previewRevision(record.id);
        QCoreApplication::installTranslator(&translator);
        const auto removeTranslator =
            qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
        QEvent language(QEvent::LanguageChange);
        QApplication::sendEvent(&page, &language);
        require(source->previewRevision(record.id) != beforeLanguageRevision &&
                    waitFor([&] { return thumbnail != beforeLanguage; }),
                "language changes refresh swatch previews without reusing old cached labels");
    }
    storage::ApplicationStorage::instance().shutdown();
    QTemporaryDir burstDirectory;
    require(storage::ApplicationStorage::instance()
                .initialize({burstDirectory.path(), burstDirectory.path(), 30000})
                .success,
            "isolated storage for notification coalescing");
    application.processEvents();
    int changeSignals = 0;
    QObject observer;
    QObject::connect(&storage::ApplicationStorage::instance(),
                     &storage::ApplicationStorage::pinnedWindowsChanged, &observer,
                     [&changeSignals]() { ++changeSignals; });
    for (int index = 0; index < 20; ++index) {
        storage::PinnedWindowRecord record;
        record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        record.image = base;
        record.nativeGeometry = QRect(QPoint(0, 0), base.size());
        record.canvasSourceRect = QRectF(record.nativeGeometry);
        record.contentCanvasRect = record.canvasSourceRect;
        record.surfaceCanvasRect = record.canvasSourceRect;
        record.initialWindowSize = base.size();
        require(storage::ApplicationStorage::instance().pinnedWindows().upsert(record).success,
                "create notification burst record");
    }
    application.processEvents();
    require(changeSignals == 1, "a mutation burst produces one page change signal");
    require(storage::ApplicationStorage::instance().pinnedWindows().flush().success,
            "commit notification burst");
    application.processEvents();
    require(changeSignals == 1, "disk commit does not repeat the page change signal");
    {
        PinnedWindowManagementPageWidget fleetingPage;
    }
    application.processEvents();
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
