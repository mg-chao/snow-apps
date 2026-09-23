#include "snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/select.h"
#include "widgets/date_picker.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/popconfirm.h"
#include "widgets/pagination.h"
#include "widgets/image.h"
#include <QElapsedTimer>
#include <QTimeZone>
#include <QThread>
#include <QUuid>
#include <QFile>
#include <QApplication>
#include <QTemporaryDir>
#include <QLabel>
#include <QFrame>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <algorithm>
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
    QVector<QString> removed;
    QVector<storage::PinnedWindowSummary> records() const override {
        return items;
    }
    QVector<storage::PinnedWindowGroup> groups() const override {
        return {{QStringLiteral("default"), QStringLiteral("Default"), true},
                {QStringLiteral("work"), QStringLiteral("Work"), false}};
    }
    void requestPreview(const QString& id, quint64 generation) override {
        QImage image(120, 60, QImage::Format_RGB32);
        image.fill(Qt::green);
        emit previewReady(id, generation, image);
    }
    void requestFullImage(const QString& id, quint64 requestId) override {
        QImage image(120, 60, QImage::Format_RGB32);
        image.fill(Qt::green);
        emit fullImageReady(id, requestId, image);
    }
    void showRecord(const QString& id) override {
        shown = id;
    }
    void removeRecords(const QVector<QString>& ids) override {
        removed = ids;
    }
};
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir directory;
    require(storage::ApplicationStorage::instance()
                .initialize({directory.path(), directory.path(), 30000})
                .success,
            "isolated storage");
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
    {
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
        auto* singleDelete = rows().front()->findChild<adqt::widgets::AdPopconfirm*>();
        require(QMetaObject::invokeMethod(singleDelete, "accepted", Qt::DirectConnection),
                "accept single deletion");
        require(fixture.removed == QVector<QString>{QStringLiteral("second")},
                "single deletion dispatches only its record");
        rows().front()->findChild<adqt::widgets::AdCheckbox*>()->setChecked(true);
        require(
            page.findChild<QWidget*>(QStringLiteral("pinnedManagementSelectionBar"))->isVisible(),
            "selection actions appear when an entry is selected");
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
    // Exercise the production asynchronous preview path with all persisted content kinds.
    auto& repository = storage::ApplicationStorage::instance().pinnedWindows();
    QImage base(32, 16, QImage::Format_RGB32);
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
        record.nativeGeometry = QRect(0, 0, 32, 16);
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
        QObject::connect(&storage::ApplicationStorage::instance(),
                         &storage::ApplicationStorage::pinnedWindowDeleteRequested, &page,
                         [&repository](const QVector<QString>& ids) {
                             for (const auto& id : ids)
                                 require(repository.remove(id).success, "remove requested pin");
                         });
        page.resize(980, 900);
        page.show();
        const auto ready = [&]() {
            const auto previews =
                page.findChildren<QLabel*>(QStringLiteral("pinnedManagementPreview"));
            if (previews.size() != 3)
                return false;
            for (auto* preview : previews)
                if (preview->pixmap().isNull())
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
        int baseImages = 0;
        for (auto* preview :
             page.findChildren<QLabel*>(QStringLiteral("pinnedManagementPreview"))) {
            const auto image = preview->pixmap().toImage();
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
            imageRow->findChild<QLabel*>(QStringLiteral("pinnedManagementPreview"));
        auto* viewer = imageRow->findChild<adqt::widgets::AdImageViewer*>();
        require(imagePreview != nullptr && viewer != nullptr && viewer->rowCount() == 1,
                "saved image has a preview viewer");
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
        auto* confirmation = row->findChild<adqt::widgets::AdPopconfirm*>(
            QStringLiteral("pinnedManagementEntryDeleteConfirm"));
        require(remove != nullptr && confirmation != nullptr,
                "record has a Delete action and confirmation");
        QPointer<adqt::widgets::AdPopconfirm> confirmationGuard(confirmation);
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
        require(confirmationGuard.isNull() && remainingRows.size() == 2 &&
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
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
