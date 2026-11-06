// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/collagepdfpagegrid.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/button.h"
#include "widgets/divider.h"
#include "widgets/image.h"
#include "widgets/scroll_area.h"

#include <QApplication>
#include <QColorSpace>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QThread>
#include <QTemporaryDir>
#include <QTranslator>
#include <QTimer>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace snow_shot::presentation;
using namespace snow_shot::presentation::components;
using adqt::widgets::AdButton;
using adqt::widgets::AdImage;
using adqt::widgets::AdImageViewer;
using adqt::widgets::AdScrollArea;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void pump() {
    for (int index = 0; index < 3; ++index) {
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

void waitUntil(const std::function<bool()>& condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 1500) {
        pump();
        QThread::msleep(1);
    }
    require(condition(), "asynchronous grid operation must finish");
}

QImage image(QSize size, QColor color = Qt::red) {
    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    result.fill(color);
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}

AdButton* button(QWidget* parent, const char* name) {
    auto* result = parent->findChild<AdButton*>(QString::fromLatin1(name));
    require(result != nullptr, "expected PDF grid action must exist");
    return result;
}

QWidget* card(ImageStitchingPdfPageGrid& grid, quint64 id) {
    auto* result = grid.findChild<QWidget*>(QStringLiteral("collagePdfPage_%1").arg(id));
    require(result != nullptr, "visible page card must exist");
    return result;
}

AdScrollArea* scroll(ImageStitchingPdfPageGrid& grid) {
    auto* result = grid.findChild<AdScrollArea*>(QStringLiteral("collagePdfPagesScroll"));
    require(result != nullptr, "PDF pages scroll area must exist");
    return result;
}

void show(ImageStitchingPdfPageGrid& grid, QSize size = {760, 640}) {
    grid.resize(size);
    grid.show();
    grid.activateWindow();
    pump();
}

void key(QWidget* target, int code) {
    QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
    pump();
}

void setDragMime(QMimeData& mime, const ImageStitchingDocument& document, quint64 id) {
    mime.setData(QStringLiteral("application/x-snow-shot-collage-pdf-page"),
                 QByteArray::number(reinterpret_cast<quintptr>(&document)) + ':' +
                     QByteArray::number(id));
}

bool enter(QWidget* target, QMimeData* mime, QPoint position,
           Qt::DropActions actions = Qt::MoveAction) {
    QDragEnterEvent event(QPointF(position), actions, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
    return event.isAccepted();
}

void pageLayoutAndResponsiveColumns() {
    ImageStitchingDocument document;
    QImage source = image({80, 120}, QColor(0, 0, 0, 0));
    source.setPixelColor(30, 40, QColor(Qt::red));
    const auto first = document.append(
        source,
        QStringLiteral("A very long image name that exceeds the width of this page card.png"));
    document.append(image({100, 10000}), QStringLiteral("/tmp/Long image.png"));
    ImageStitchingPdfPageGrid grid(&document);
    show(grid);
    require(grid.property("columnCount").toInt() == 3, "wide grid must use three columns");
    auto* firstCard = card(grid, first);
    const auto paper = firstCard->property("paperRect").toRectF();
    const auto content = firstCard->property("contentRect").toRectF();
    const auto placed = firstCard->property("imageRect").toRectF();
    require(std::abs(paper.width() / paper.height() - 210.0 / 297.0) < 0.001,
            "page card must show actual paper proportions");
    require(std::abs((content.left() - paper.left()) / paper.width() - 10.0 / 210.0) < 0.001,
            "grid must show the page's ten millimeter margin");
    require(content.contains(placed) &&
                (placed.center() - content.center()).manhattanLength() < 0.001 &&
                std::abs(placed.width() / placed.height() - 80.0 / 120.0) < 0.001,
            "image must fit entirely and center without distortion");
    require(firstCard->toolTip() == document.items().front().label &&
                firstCard->accessibleName().contains(document.items().front().label),
            "truncated image names must remain available in tooltips and accessibility");
    auto* number = firstCard->findChild<QLabel*>(QStringLiteral("collagePdfPageNumber"));
    require(number && number->isVisible() && number->text() == QStringLiteral("1"),
            "page number must remain visible above the image");
    const QSize numberSize(
        std::max(24, firstCard->fontMetrics().horizontalAdvance(number->text()) + 12), 22);
    const QPoint numberOrigin(qRound(paper.left()) + 4, qRound(paper.top()) + 4);
    require(number->geometry() == QRect(numberOrigin, numberSize) &&
                number->styleSheet() ==
                    QStringLiteral("color: white; background-color: rgba(0, 0, 0, 130); "
                                   "border-radius: 4px;"),
            "the shared ordinal style must preserve the PDF page badge appearance");
    auto* pageImage = firstCard->findChild<AdImage*>();
    waitUntil([&] { return !pageImage->loading(); });
    const auto rendered = firstCard->grab().toImage();
    require(rendered.pixelColor(placed.center().toPoint()) == QColor(Qt::white),
            "transparent pixels must display on white paper");
    const auto longPlaced = card(grid, document.items().back().id)->property("imageRect").toRectF();
    require(longPlaced.width() < 4 && longPlaced.height() > 100 &&
                std::abs(longPlaced.width() / longPlaced.height() - 0.01) < 0.001,
            "long image must stay in one proportionally fitted page");
    auto* longCard = card(grid, document.items().back().id);
    require(longCard->toolTip() == QStringLiteral("Long image.png"),
            "page labels and tooltips must show the image name instead of its source directory");
    auto* longImage = longCard->findChild<AdImage*>();
    auto* pageReply = longImage->imageLoader()->load(longImage->source(),
                                                     {.targetPixelSize = QSize(400, 600)}, &grid);
    waitUntil([&] { return pageReply->isFinished(); });
    const auto longRaster = pageReply->image();
    require(
        !longRaster.isNull() && longRaster.pixelColor(longRaster.width() / 2, 24).green() < 50 &&
            longRaster.pixelColor(longRaster.width() / 2, longRaster.height() - 24).green() < 50,
        "long-image thumbnail must fill the full content height despite source-preview aspect "
        "rounding");
    delete pageReply;
    grid.resize(500, 640);
    pump();
    require(grid.property("columnCount").toInt() == 2, "narrower grid must reflow to two columns");
    grid.resize(300, 640);
    pump();
    require(grid.property("columnCount").toInt() == 1, "small viewport must retain one column");
    auto settings = document.pdfSettings();
    settings.orientation = ImageStitchingPdfOrientation::Landscape;
    settings.marginLeftMm = 20;
    settings.marginRightMm = 30;
    document.setPdfSettings(settings);
    pump();
    firstCard = card(grid, first);
    const auto landscape = firstCard->property("paperRect").toRectF();
    require(landscape.width() > landscape.height(),
            "orientation edits must immediately update paper proportions");
    settings.customWidthMm = 10;
    settings.customHeightMm = 400;
    settings.paperSize = ImageStitchingPdfPaperSize::Custom;
    settings.orientation = ImageStitchingPdfOrientation::Portrait;
    settings.marginLeftMm = settings.marginRightMm = 0;
    settings.marginTopMm = settings.marginBottomMm = 0;
    document.setPdfSettings(settings);
    pump();
    const auto custom = card(grid, first)->property("paperRect").toRectF();
    require(std::abs(custom.width() / custom.height() - 10.0 / 400.0) < 0.001,
            "extreme custom paper must show its actual proportions");
    settings.marginLeftMm = 20;
    document.setPdfSettings(settings);
    pump();
    require(grid.findChild<QLabel*>(QStringLiteral("collagePdfValidationNotice"))->isVisible() &&
                card(grid, first)->property("paperRect").toRectF() == custom,
            "invalid edits must explain and retain the last valid page preview");
}

void imageSizedPagePreview() {
    ImageStitchingDocument document;
    const auto first = document.append(image({80, 40}), QStringLiteral("Wide"));
    const auto second = document.append(image({40, 120}), QStringLiteral("Tall"));
    ImageStitchingPdfPageGrid grid(&document);
    show(grid);
    auto settings = document.pdfSettings();
    settings.paperSize = ImageStitchingPdfPaperSize::ImageSize;
    settings.orientation = ImageStitchingPdfOrientation::Landscape;
    document.setPdfSettings(settings);
    pump();
    const auto check = [&](quint64 id, qreal ratio) {
        auto* page = card(grid, id);
        const auto paper = page->property("paperRect").toRectF();
        const auto content = page->property("contentRect").toRectF();
        const auto placed = page->property("imageRect").toRectF();
        require(std::abs(paper.width() / paper.height() - ratio) < 0.001 &&
                    (content.topLeft() - paper.topLeft()).manhattanLength() < 0.001 &&
                    (content.bottomRight() - paper.bottomRight()).manhattanLength() < 0.001 &&
                    (placed.topLeft() - paper.topLeft()).manhattanLength() < 0.001 &&
                    (placed.bottomRight() - paper.bottomRight()).manhattanLength() < 0.001,
                "each image-sized preview has its own aspect ratio and fills its borderless page");
        auto* raster = page->findChild<AdImage*>();
        waitUntil([&] { return !raster->loading(); });
    };
    check(first, 2);
    check(second, 1.0 / 3.0);
    require(document.rotate(first, true), "rotate the image-sized page preview");
    pump();
    check(first, 0.5);
    check(second, 1.0 / 3.0);
    settings.paperSize = ImageStitchingPdfPaperSize::A4;
    document.setPdfSettings(settings);
    pump();
    const auto paper = card(grid, first)->property("paperRect").toRectF();
    require(std::abs(paper.width() / paper.height() - 297.0 / 210.0) < 0.001,
            "switching back to paper sizing restores its orientation in the preview");
}

void keyboardActionsAndBusyStates() {
    ImageStitchingDocument document;
    const auto first = document.append(image({80, 40}), QStringLiteral("First"));
    const auto second = document.append(image({40, 80}), QStringLiteral("Second"));
    ImageStitchingPdfPageGrid grid(&document);
    show(grid);
    quint64 replacement = 0;
    int clears = 0;
    int adds = 0;
    QObject::connect(&grid, &ImageStitchingPdfPageGrid::replaceImageRequested, &grid,
                     [&](quint64 id) { replacement = id; });
    QObject::connect(&grid, &ImageStitchingPdfPageGrid::clearRequested, &grid, [&] { ++clears; });
    QObject::connect(&grid, &ImageStitchingPdfPageGrid::addImagesRequested, &grid, [&] { ++adds; });
    auto* firstCard = card(grid, first);
    firstCard->setFocus();
    pump();
    auto* toolbar = grid.findChild<QWidget*>(QStringLiteral("collagePdfImageToolbar"));
    auto* replace = button(toolbar, "collagePdfReplaceImage");
    auto* remove = button(toolbar, "collagePdfDeleteSelected");
    auto* preview = button(toolbar, "collagePdfPreviewImage");
    require(replace->isVisible() && remove->isVisible() && preview->isVisible(),
            "keyboard selection must expose every page action in the floating toolbar");
    for (auto* page : {firstCard, card(grid, second)})
        require(page->findChildren<AdButton*>().isEmpty(),
                "page thumbnails must leave corner actions in the selection toolbar");
    require(!replace->geometry().intersects(remove->geometry()) &&
                !remove->geometry().intersects(preview->geometry()) &&
                !replace->accessibleName().isEmpty(),
            "page actions need separate targets and accessible labels");
    replace->setFocus();
    pump();
    require(preview->isVisible() && document.selectedId() == first,
            "focus on a toolbar action must preserve selection and keep every action visible");
    button(&grid, "collagePdfAddImages")->setFocus();
    document.select(0);
    pump();
    QEnterEvent hover(QPointF(20, 20), QPointF(20, 20), firstCard->mapToGlobal(QPoint(20, 20)));
    QApplication::sendEvent(firstCard, &hover);
    require(!toolbar->isVisible() && document.selectedId() == 0 &&
                firstCard->findChildren<AdButton*>().isEmpty(),
            "hovering an unselected page must keep its corners and selection toolbar clear");
    firstCard->setFocus();
    pump();
    replace->setFocus();
    replace->click();
    require(replacement == first && document.items().front().id == first,
            "replace must identify its page and preserve it until loading succeeds");
    firstCard->setFocus();
    key(firstCard, Qt::Key_Right);
    require(document.selectedId() == second && card(grid, second)->hasFocus(),
            "arrow navigation must move focus and selection in page order");
    replace->click();
    require(replacement == second, "the shared replacement action follows the selected page");
    auto* viewer = grid.findChild<AdImageViewer*>(QStringLiteral("collagePdfOriginalViewer"));
    preview->click();
    require(viewer && viewer->isVisible() && viewer->currentRow() == 1,
            "the floating preview action opens the selected page's original image");
    viewer->close();
    card(grid, second)->setFocus();
    key(card(grid, second), Qt::Key_Home);
    require(document.selectedId() == first, "Home must select the first page");
    grid.setLoading(true);
    replacement = 0;
    replace->click();
    require(!replace->isEnabled() && remove->isEnabled() &&
                !button(&grid, "collagePdfAddImages")->isEnabled() && replacement == 0,
            "loading must prevent duplicate import and replacement");
    grid.setLoading(false);
    grid.setExportBusy(true);
    require(!replace->isEnabled() && !remove->isEnabled() &&
                !button(&grid, "collagePdfClear")->isEnabled() && preview->isEnabled() &&
                toolbar->isVisible(),
            "export must prevent content changes while preserving original preview");
    preview->click();
    require(viewer->isVisible() && viewer->currentRow() == 0,
            "the floating toolbar keeps original preview available during export");
    viewer->close();
    firstCard->setFocus();
    key(firstCard, Qt::Key_Delete);
    require(document.items().size() == 2, "export must block keyboard deletion");
    grid.setExportBusy(false);
    button(&grid, "collagePdfClear")->click();
    button(&grid, "collagePdfAddImages")->click();
    require(
        clears == 1 && adds == 1 && document.items().size() == 2,
        "toolbar must request clearing confirmation and import without directly mutating images");
    remove->setFocus();
    remove->click();
    pump();
    require(document.items().size() == 1 && document.items().front().id == second,
            "delete action must remove only its own page");
    require(card(grid, second)->hasFocus(),
            "deleting a focused page must restore keyboard focus to the selected surviving page");
}

void bottomToolbarAndReversePageOrder() {
    ImageStitchingDocument document;
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
    ImageStitchingPdfPageGrid grid(&document);
    show(grid);
    auto* reverse = button(&grid, "collagePdfReversePageOrder");
    auto* add = button(&grid, "collagePdfAddImages");
    auto* clear = button(&grid, "collagePdfClear");
    auto* toolbar = grid.findChild<QWidget*>(QStringLiteral("collagePdfCanvasToolbar"));
    auto* layout = toolbar->layout();
    require(layout->count() == 4 && layout->itemAt(0)->widget() == reverse &&
                layout->itemAt(1)->widget() == add &&
                qobject_cast<adqt::widgets::AdDivider*>(layout->itemAt(2)->widget()) &&
                layout->itemAt(3)->widget() == clear,
            "bottom toolbar order must be Reverse page order, Add images, separator, Clear");
    require(reverse->toolTip() == QStringLiteral("Reverse page order") &&
                reverse->accessibleName() == reverse->toolTip() && !reverse->isEnabled(),
            "reverse action must have an accessible label and be disabled for an empty document");
    document.append(image({20, 30}), QStringLiteral("First"));
    pump();
    require(!reverse->isEnabled(), "a single page cannot be reversed");
    for (int index = 1; index < 21; ++index)
        document.append(image({20, 30}), QString::number(index));
    document.moveItem(document.items().front().id, 3);
    pump();
    require(reverse->isEnabled() && toolbar->isVisible() && reverse->x() < add->x() &&
                add->x() < clear->x() &&
                grid.property("materializedPageCount").toInt() < document.items().size(),
            "ordered bottom actions must support a document with pages outside the viewport");
    const auto original = document.items();
    document.select(original.front().id);
    reverse->click();
    pump();
    auto* viewer = grid.findChild<AdImageViewer*>(QStringLiteral("collagePdfOriginalViewer"));
    for (int index = 0; index < original.size(); ++index) {
        const auto expected = original[original.size() - index - 1].id;
        require(document.items()[index].id == expected &&
                    viewer->itemAt(index).source.path().toULongLong() == expected,
                "reverse action must reverse every current page and original-preview entry");
    }
    require(card(grid, original.back().id)->property("pageNumber").toInt() == 1 &&
                document.selectedId() == original.front().id,
            "reversal updates visible page numbers and preserves the selected page");
    const auto revision = document.contentRevision();
    grid.setLoading(true);
    reverse->click();
    require(!reverse->isEnabled() && document.contentRevision() == revision,
            "loading must disable reversal until every pending page has been imported");
    grid.setLoading(false);
    grid.setExportBusy(true);
    reverse->click();
    require(!reverse->isEnabled() && document.contentRevision() == revision,
            "export must block page reversal");
    grid.setExportBusy(false);
    require(reverse->isEnabled(), "reverse action must recover after export");
    reverse->click();
    pump();
    for (int index = 0; index < original.size(); ++index)
        require(document.items()[index].id == original[index].id,
                "a second click must restore the document's prior order");
    document.clear();
    pump();
    require(!reverse->isEnabled(), "clearing all pages must disable reversal");
}

void selectedPageFloatingToolbar() {
    ImageStitchingDocument document;
    QVector<quint64> ids;
    for (int index = 0; index < 12; ++index)
        ids.append(document.append(image({80, 40}), QString::number(index)));
    const auto originalKey = document.items().front().image.cacheKey();
    const auto settings = document.pdfSettings();
    ImageStitchingPdfPageGrid grid(&document);
    show(grid);
    auto* toolbar = grid.findChild<QWidget*>(QStringLiteral("collagePdfImageToolbar"));
    require(toolbar && toolbar->isVisible() && grid.rect().contains(toolbar->geometry()),
            "a selected visible page exposes a floating toolbar inside the grid");
    document.select(0);
    require(!toolbar->isVisible(), "clearing selection hides its floating toolbar");
    auto* first = card(grid, ids.front());
    const QPointF position(12, 100);
    QMouseEvent press(QEvent::MouseButtonPress, position, first->mapToGlobal(position.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(first, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, position,
                        first->mapToGlobal(position.toPoint()), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(first, &release);
    require(document.selectedId() == ids.front() && toolbar->isVisible(),
            "clicking a page selects it and reveals the floating toolbar immediately");
    button(toolbar, "collagePdfRotateRight")->click();
    require(transformedSize(document.items().front()) == QSize(40, 80),
            "floating clockwise rotation edits the selected page");
    button(toolbar, "collagePdfRotateLeft")->click();
    require(document.items().front().transform.isIdentity(),
            "floating counterclockwise rotation reverses clockwise rotation");
    button(toolbar, "collagePdfFlipHorizontal")->click();
    require(document.items().front().transform == QTransform(-1, 0, 0, 1, 0, 0),
            "floating horizontal flip edits the selected page");
    button(toolbar, "collagePdfFlipVertical")->click();
    require(document.items().front().transform == QTransform(-1, 0, 0, -1, 0, 0),
            "floating vertical flip edits the selected page");
    button(toolbar, "collagePdfResetTransform")->click();
    pump();
    require(document.items().front().transform.isIdentity() &&
                document.items().front().image.cacheKey() == originalKey &&
                document.pdfSettings() == settings && document.items()[1].revision == 1,
            "reset restores the selected transform while preserving originals, settings and peers");
    key(first, Qt::Key_Down);
    require(document.selectedId() == ids[3] && toolbar->isVisible(),
            "keyboard navigation moves the floating toolbar to the newly selected page");
    const int selectedCenter =
        card(grid, ids[3])->mapTo(&grid, card(grid, ids[3])->rect().center()).x();
    require(std::abs(toolbar->geometry().center().x() - selectedCenter) <= 1,
            "the floating toolbar is centered above its selected page");
    auto* bar = scroll(grid)->verticalScrollBar();
    const int previousScroll = bar->value();
    const int previousToolbarY = toolbar->y();
    bar->setValue(previousScroll + 40);
    pump();
    require(toolbar->isVisible() && toolbar->y() == previousToolbarY - 40 &&
                grid.rect().contains(toolbar->geometry()),
            "scrolling repositions the toolbar with its visible selected page");
    document.select(ids.front());
    bar->setValue(0);
    grid.resize(240, 640);
    pump();
    require(toolbar->isVisible() && grid.rect().contains(toolbar->geometry()),
            "column changes keep the selected-page toolbar within the resized viewport");
    bar->setValue(bar->maximum());
    pump();
    require(!toolbar->isVisible(), "scrolling the selected page out of view hides its toolbar");
    bar->setValue(0);
    pump();
    require(toolbar->isVisible(), "scrolling back restores the selected-page toolbar");
    grid.setLoading(true);
    require(toolbar->isVisible() && button(toolbar, "collagePdfRotateRight")->isEnabled(),
            "importing more images keeps transformations available for the selected page");
    grid.setLoading(false);
    grid.setExportBusy(true);
    const auto revision = document.items().front().revision;
    auto* preview = button(toolbar, "collagePdfPreviewImage");
    for (auto* action : toolbar->findChildren<AdButton*>()) {
        if (action == preview)
            continue;
        require(!action->isEnabled(), "export disables every content-changing selection action");
        action->click();
    }
    require(toolbar->isVisible() && preview->isEnabled() &&
                document.items().front().revision == revision && document.items().size() == 12,
            "export preserves preview while blocking transformations, replacement and deletion");
    grid.setExportBusy(false);
    auto* remove = button(toolbar, "collagePdfDeleteSelected");
    remove->setFocus();
    pump();
    remove->click();
    pump();
    require(document.items().size() == 11 && document.items().front().id == ids[1] &&
                card(grid, ids[1])->hasFocus() && toolbar->isVisible(),
            "floating deletion removes only the selected page and focuses its surviving neighbor");
    document.clear();
    pump();
    require(!toolbar->isVisible(), "clearing every page hides the floating selection toolbar");
}

void insertionReorderAndAutoscroll() {
    ImageStitchingDocument document;
    QVector<quint64> ids;
    for (int index = 0; index < 40; ++index)
        ids.push_back(document.append(image({5, 3}), QString::number(index)));
    document.select(ids.front());
    ImageStitchingPdfPageGrid grid(&document);
    show(grid, {760, 500});
    auto* viewport = scroll(grid)->viewport();
    QMimeData mime;
    setDragMime(mime, document, ids.front());
    auto* third = card(grid, ids[2]);
    const QPoint afterThird = third->mapTo(viewport, QPoint(third->width() - 2, 70));
    require(enter(viewport, &mime, afterThird), "internal page drag must be accepted");
    require(grid.findChild<QWidget*>(QStringLiteral("collagePdfInsertionMarker"))->isVisible() &&
                grid.property("insertionIndex").toInt() == 3,
            "drag must show the insertion slot after the pointed page");
    QDropEvent drop(afterThird, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &drop);
    pump();
    require(drop.isAccepted() && document.items()[0].id == ids[1] &&
                document.items()[1].id == ids[2] && document.items()[2].id == ids[0] &&
                document.selectedId() == ids[0],
            "moving a page must insert and shift intermediates while preserving selection");
    auto* first = card(grid, ids[1]);
    const QPoint beforeFirst = first->mapTo(viewport, QPoint(2, 70));
    require(enter(viewport, &mime, beforeFirst), "backward insertion must be accepted");
    QDropEvent back(beforeFirst, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &back);
    pump();
    require(document.items().front().id == ids[0] && document.items()[1].id == ids[1],
            "backward insertion must restore original order without swapping");
    ImageStitchingDocument foreign;
    QMimeData foreignMime;
    setDragMime(foreignMime, foreign, ids.front());
    require(!enter(viewport, &foreignMime, beforeFirst),
            "drag from another document must be rejected");
    const QPoint edge(viewport->width() / 2, viewport->height() - 2);
    require(enter(viewport, &mime, edge), "edge drag must start insertion feedback");
    auto* bar = scroll(grid)->verticalScrollBar();
    waitUntil([&] { return bar->value() > 0; });
    const int position = bar->value();
    grid.setExportBusy(true);
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 60)
        pump();
    require(bar->value() == position && grid.property("insertionIndex").toInt() == -1 &&
                grid.findChild<QWidget*>(QStringLiteral("collagePdfInsertionMarker"))->isHidden(),
            "export must stop autoscroll and remove pending insertion feedback");
    require(!enter(viewport, &mime, edge), "export must reject racing internal drags");
    grid.setExportBusy(false);
    require(enter(viewport, &mime, edge), "dragging must recover after export");
    QDragLeaveEvent leave;
    QApplication::sendEvent(viewport, &leave);
    require(grid.property("insertionIndex").toInt() == -1,
            "leaving the grid must clear insertion and autoscroll state");
}

void originalPreviewIsolationAndOrder() {
    ImageStitchingDocument document;
    const auto first = document.append(image({100, 40}), QStringLiteral("First"));
    const auto second = document.append(image({50, 120}, Qt::green), QStringLiteral("Second"));
    const auto third = document.append(image({200, 20}, Qt::blue), QStringLiteral("Third"));
    document.rotate(second, true);
    document.flip(second, true);
    ImageStitchingPdfPageGrid grid(&document);
    show(grid);
    auto* viewer = grid.findChild<AdImageViewer*>(QStringLiteral("collagePdfOriginalViewer"));
    require(viewer && viewer->rowCount() == 3,
            "grid must use Ant Design's shared multi-image viewer");
    card(grid, second)->setFocus();
    key(card(grid, second), Qt::Key_Return);
    require(viewer->isVisible() && viewer->currentRow() == 1,
            "keyboard preview must open at the selected page");
    pump();
    const auto transform = document.items()[1].transform;
    const auto pixels = document.items()[1].image.constBits();
    const auto settings = document.pdfSettings();
    const auto layout =
        calculateImageStitchingPdfPageLayout(transformedSize(document.items()[1]), settings);
    auto* reply = viewer->imageLoader()->load(viewer->itemAt(1).source, {}, &grid);
    waitUntil([&] { return reply->isFinished(); });
    require(reply->isSuccessful() && reply->image().size() == QSize(50, 120) &&
                reply->image().constBits() == pixels,
            "original preview must share full original pixels without document transforms");
    delete reply;
    viewer->zoomIn();
    viewer->rotateLeft();
    viewer->flipX();
    viewer->flipY();
    viewer->zoomOut();
    pump();
    const auto after = calculateImageStitchingPdfPageLayout(transformedSize(document.items()[1]),
                                                            document.pdfSettings());
    require(document.items()[1].transform == transform &&
                document.items()[1].image.constBits() == pixels &&
                document.pdfSettings() == settings &&
                layout.imageMillimeters == after.imageMillimeters,
            "preview viewing adjustments must leave source, settings, transforms and export "
            "geometry unchanged");
    viewer->activate(1);
    require(viewer->currentRow() == 2 && viewer->itemAt(2).source.path().toULongLong() == third,
            "viewer navigation must follow current page order");
    document.moveItem(third, 0);
    pump();
    require(
        viewer->isVisible() && viewer->currentRow() == 0 &&
            viewer->itemAt(0).source.path().toULongLong() == third &&
            viewer->itemAt(1).source.path().toULongLong() == first &&
            viewer->itemAt(2).source.path().toULongLong() == second,
        "reordering must synchronize preview order and preserve the currently viewed stable image");
    viewer->close();
    document.replace(second, image({80, 80}, Qt::yellow), QStringLiteral("Replacement"));
    pump();
    require(card(grid, second)->toolTip() == QStringLiteral("Replacement") &&
                viewer->itemAt(2).altText == QStringLiteral("Replacement"),
            "replacement must refresh page name and original-preview metadata");
}

void emptyImportAndVirtualization() {
    ImageStitchingDocument document;
    ImageStitchingPdfPageGrid grid(&document);
    show(grid, {760, 500});
    require(grid.findChild<QWidget*>(QStringLiteral("collagePdfEmptyState"))->isVisible() &&
                !button(&grid, "collagePdfClear")->isEnabled(),
            "empty grid must show an add/drop prompt and disable Clear");
    QStringList files;
    QObject::connect(&grid, &ImageStitchingPdfPageGrid::imageFilesDropped, &grid,
                     [&](const QStringList& paths) { files = paths; });
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/first.png")),
                  QUrl::fromLocalFile(QStringLiteral("/tmp/second.jpg")),
                  QUrl::fromLocalFile(QStringLiteral("/tmp/first.png")),
                  QUrl::fromLocalFile(QStringLiteral("/tmp/ignored.txt"))});
    require(enter(&grid, &mime, grid.rect().center(), Qt::CopyAction),
            "empty grid must accept external image files");
    QDropEvent drop(grid.rect().center(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&grid, &drop);
    require(drop.isAccepted() && files == QStringList({QStringLiteral("/tmp/first.png"),
                                                       QStringLiteral("/tmp/second.jpg"),
                                                       QStringLiteral("/tmp/first.png")}),
            "external import must preserve file order and duplicates");
    grid.setExportBusy(true);
    require(!enter(&grid, &mime, grid.rect().center(), Qt::CopyAction) &&
                !button(&grid, "collagePdfEmptyAddImages")->isEnabled(),
            "empty grid must block imports during export");
    grid.setExportBusy(false);
    QVector<quint64> ids;
    for (int index = 0; index < 1000; ++index)
        ids.append(document.append(image({1, 1}), QString::number(index)));
    pump();
    require(grid.property("materializedPageCount").toInt() < 30 &&
                grid.findChildren<AdImage*>().size() < 30,
            "large documents must materialize only visible and buffered page cards");
    require(grid.property("pageThumbnailBytes").toLongLong() > 0 &&
                grid.property("pageThumbnailBytes").toLongLong() <= qint64{64} * 1024 * 1024,
            "full-page thumbnail rasters and pixmaps must stay inside the visible-page budget");
    auto* area = scroll(grid);
    auto* bar = area->verticalScrollBar();
    bar->setValue(bar->maximum());
    pump();
    auto* last = card(grid, ids.back());
    auto* toolbar = grid.findChild<QWidget*>(QStringLiteral("collagePdfCanvasToolbar"));
    const int lastBottom = last->mapTo(&grid, QPoint(0, last->height())).y();
    require(lastBottom < toolbar->y(),
            "bottom padding must let the last row clear the fixed toolbar");
    require(grid.property("materializedPageCount").toInt() < 30,
            "scrolling must retire offscreen cards and keep memory bounded");
    bar->setValue(0);
    pump();
    auto* first = card(grid, ids.front());
    first->setFocus();
    key(first, Qt::Key_End);
    require(document.selectedId() == ids.back() && card(grid, ids.back())->hasFocus(),
            "keyboard navigation must materialize and reveal a distant page");
    auto settings = document.pdfSettings();
    settings.marginTopMm = 25;
    document.setPdfSettings(settings);
    document.clear();
    pump();
    require(document.pdfSettings() == settings &&
                grid.findChild<QWidget*>(QStringLiteral("collagePdfEmptyState"))->isVisible() &&
                grid.property("materializedPageCount").toInt() == 0,
            "clear must retain PDF settings and retire every page card");
}

class GridTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* sourceText, const char*,
                      int) const override {
        if (QString::fromLatin1(context) == QStringLiteral("ImageStitchingPdfPageGrid")) {
            if (QString::fromUtf8(sourceText) == QStringLiteral("Clear"))
                return QStringLiteral("Translated clear");
            if (QString::fromUtf8(sourceText) == QStringLiteral("Reverse page order"))
                return QStringLiteral("Translated reverse");
            if (QString::fromUtf8(sourceText) == QStringLiteral("Rotate clockwise by 90°"))
                return QStringLiteral("Translated rotation");
            if (QString::fromUtf8(sourceText) == QStringLiteral("Replace image"))
                return QStringLiteral("Translated replacement");
            if (QString::fromUtf8(sourceText) == QStringLiteral("Preview original image"))
                return QStringLiteral("Translated preview");
        }
        return {};
    }
};

void languageChangeAndHiddenWork() {
    ImageStitchingDocument document;
    const auto first = document.append(image({20, 20}), QStringLiteral("Name"));
    ImageStitchingPdfPageGrid grid(&document);
    show(grid);
    GridTranslator translator;
    QCoreApplication::installTranslator(&translator);
    QEvent event(QEvent::LanguageChange);
    QApplication::sendEvent(&grid, &event);
    require(button(&grid, "collagePdfClear")->toolTip() == QStringLiteral("Translated clear") &&
                button(&grid, "collagePdfClear")->accessibleName() ==
                    QStringLiteral("Translated clear"),
            "language changes must update cached toolbar and accessibility copy");
    require(button(&grid, "collagePdfReversePageOrder")->toolTip() ==
                    QStringLiteral("Translated reverse") &&
                button(&grid, "collagePdfReversePageOrder")->accessibleName() ==
                    QStringLiteral("Translated reverse"),
            "language changes update the reverse action tooltip and accessible label");
    require(button(&grid, "collagePdfRotateRight")->toolTip() ==
                    QStringLiteral("Translated rotation") &&
                button(&grid, "collagePdfRotateRight")->accessibleName() ==
                    QStringLiteral("Translated rotation"),
            "language changes update floating selection toolbar tooltips and accessibility");
    for (const auto* name : {"collagePdfReplaceImage", "collagePdfPreviewImage"}) {
        const auto expected = QString::fromLatin1(name).contains(QStringLiteral("Replace"))
                                  ? QStringLiteral("Translated replacement")
                                  : QStringLiteral("Translated preview");
        auto* action = button(&grid, name);
        require(action->toolTip() == expected && action->accessibleName() == expected,
                "moved page actions must retranslate their tooltips and accessible labels");
    }
    QCoreApplication::removeTranslator(&translator);
    pump();
    const auto thumbnailBytes = grid.property("pageThumbnailBytes").toLongLong();
    const auto beforeHide = imageStitchingResourceUsage().reservedBytes;
    grid.hide();
    for (auto* pageImage : grid.findChildren<AdImage*>())
        require(
            pageImage->source().isEmpty(),
            "hiding must synchronously release displayed pixels before deferred widget deletion");
    require(grid.property("pageThumbnailBytes").toLongLong() == 0 &&
                imageStitchingResourceUsage().reservedBytes == beforeHide - thumbnailBytes,
            "thumbnail reservations must retire only after displayed pixmaps release their pixels");
    pump();
    require(grid.findChildren<AdImage*>().isEmpty(),
            "hidden PDF mode must release page thumbnail widgets");
    document.rotate(first, true);
    document.append(image({20, 30}), QStringLiteral("Later"));
    pump();
    require(grid.findChildren<AdImage*>().isEmpty(),
            "static-mode edits must not materialize hidden PDF cards");
    grid.show();
    pump();
    require(grid.property("materializedPageCount").toInt() == 2 &&
                card(grid, first)->property("pageNumber").toInt() == 1,
            "returning to PDF mode must rebuild current shared editing content");
}

void hidingDuringNativeDragKeepsContent() {
    ImageStitchingDocument document;
    const auto first = document.append(image({20, 30}), QStringLiteral("First"));
    document.append(image({30, 20}), QStringLiteral("Second"));
    ImageStitchingPdfPageGrid grid(&document);
    show(grid);
    auto* source = card(grid, first);
    const QPointF pressed(12, 100);
    QMouseEvent press(QEvent::MouseButtonPress, pressed, source->mapToGlobal(pressed.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(source, &press);
    bool hidden = false;
    QTimer::singleShot(0, &grid, [&] {
        hidden = true;
        grid.hide();
        QDrag::cancel();
    });
    const QPointF moved = pressed + QPointF(QApplication::startDragDistance() + 8, 0);
    QMouseEvent move(QEvent::MouseMove, moved, source->mapToGlobal(moved.toPoint()), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(source, &move);
    pump();
    require(hidden && grid.isHidden() && document.items().size() == 2 &&
                document.items().front().id == first &&
                grid.property("materializedPageCount").toInt() == 0 &&
                grid.property("pageThumbnailBytes").toLongLong() == 0,
            "hiding during a native drag must retire its source safely without editing content");
    grid.show();
    pump();
    require(card(grid, first)->findChild<AdImage*>()->source().isValid(),
            "reopening after drag cancellation must restore the page thumbnail");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    try {
        require(directory.isValid() &&
                    storage.initialize({directory.path(), directory.path(), 60000}).success,
                "grid tests must isolate application storage in a temporary directory");
        pageLayoutAndResponsiveColumns();
        imageSizedPagePreview();
        keyboardActionsAndBusyStates();
        bottomToolbarAndReversePageOrder();
        selectedPageFloatingToolbar();
        insertionReorderAndAutoscroll();
        originalPreviewIsolationAndOrder();
        emptyImportAndVirtualization();
        languageChangeAndHiddenWork();
        hidingDuringNativeDragKeepsContent();
        storage.shutdown();
        std::cout << "Collage PDF page grid tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        storage.shutdown();
        std::cerr << "Collage PDF page grid tests failed: " << error.what() << '\n';
        return 1;
    }
}
