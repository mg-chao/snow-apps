// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/collagepdftemplatewidget.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/components/collagepdfpagegrid.h"
#include "snow_shot/presentation/components/imagedropsurface.h"
#include "snow_shot/presentation/components/overlaycontrolbutton.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_draw_engine_qt/snow_canvas_overlay_style.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/image.h"
#include "widgets/scroll_area.h"

#include <QApplication>
#include <QColorSpace>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScrollBar>
#include <QThread>
#include <QTranslator>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace snow_shot::presentation;
using namespace snow_shot::presentation::components;
using adqt::widgets::AdButton;

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
    while (!condition() && elapsed.elapsed() < 4000) {
        pump();
        QThread::msleep(1);
    }
    require(condition(), "background template preview preparation must complete");
}
QImage image(QSize size, QColor color) {
    QImage value(size, QImage::Format_RGBA8888);
    value.fill(color);
    value.setColorSpace(QColorSpace::SRgb);
    return value;
}
void configure(ImageStitchingDocument& document,
               ImageStitchingPdfTemplateType type = ImageStitchingPdfTemplateType::TwoImages) {
    auto settings = document.pdfSettings();
    settings.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
    settings.templateType = type;
    document.setPdfSettings(settings);
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
}
void show(QWidget& widget, QSize size = {800, 720}) {
    widget.resize(size);
    widget.show();
    widget.activateWindow();
    pump();
}
AdButton* button(QWidget& widget, const char* name) {
    auto* result = widget.findChild<AdButton*>(QString::fromLatin1(name));
    require(result != nullptr, "template action must exist");
    return result;
}
void mouse(QWidget* widget, QEvent::Type type, QPointF position, Qt::MouseButton changed,
           Qt::MouseButtons held) {
    QMouseEvent event(type, position, widget->mapToGlobal(position), changed, held, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
void press(QWidget* widget, QPointF position) {
    mouse(widget, QEvent::MouseButtonPress, position, Qt::LeftButton, Qt::LeftButton);
}
void move(QWidget* widget, QPointF position) {
    mouse(widget, QEvent::MouseMove, position, Qt::NoButton, Qt::LeftButton);
}
void release(QWidget* widget, QPointF position) {
    mouse(widget, QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton);
    pump();
}
void drag(QWidget* widget, QPointF start, QPointF finish) {
    press(widget, start);
    move(widget, finish);
    release(widget, finish);
}
void key(QWidget* widget, int value) {
    QKeyEvent event(QEvent::KeyPress, value, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    pump();
}
bool close(qreal first, qreal second, qreal epsilon = .001) {
    return std::abs(first - second) < epsilon;
}
void setManualCoverPlacement(ImageStitchingDocument& document, quint64 id, qreal scaleFactor = 1) {
    const auto settings = document.lastValidPdfSettings();
    const int index = document.indexOf(id);
    require(index >= 0, "manual placement fixture must identify an input image");
    const auto page = calculateImageStitchingPdfComposedPage(
        document.items(), settings,
        index / imageStitchingPdfTemplateCapacity(settings.templateType));
    for (const auto& cell : page.cells) {
        if (cell.itemId != id)
            continue;
        const auto pixels = transformedSize(document.items()[index]);
        ImageStitchingPdfPlacement placement;
        placement.millimetersPerPixel =
            scaleFactor * std::max(cell.clipMillimeters.width() / pixels.width(),
                                   cell.clipMillimeters.height() / pixels.height());
        require(document.setPdfPlacement(id, placement),
                "manual cover scale must be admitted independently of automatic contained fit");
        return;
    }
    require(false, "manual placement fixture must find its assigned cell");
}
void requireVisibleImagesContained(const ImageStitchingDocument& document,
                                   const ImageStitchingPdfTemplateCanvas& canvas) {
    const auto settings = document.lastValidPdfSettings();
    const auto page =
        calculateImageStitchingPdfComposedPage(document.items(), settings, canvas.pageIndex());
    require(page.isValid(), "automatic layout fixture must compose a valid page");
    for (const auto& cell : page.cells) {
        if (!cell.itemId)
            continue;
        const auto imageBounds = canvas.imageViewportRect(cell.itemId);
        const auto cellBounds = canvas.cellViewportRect(cell.itemId);
        const auto pixels = transformedSize(document.items()[cell.itemIndex]);
        require(!imageBounds.isEmpty() &&
                    cellBounds.adjusted(-.001, -.001, .001, .001).contains(imageBounds),
                "automatic template layout must show the entire image inside its cell");
        require((imageBounds.center() - cellBounds.center()).manhattanLength() < .001 &&
                    close(imageBounds.width() / imageBounds.height(),
                          qreal(pixels.width()) / pixels.height()),
                "automatic fit must center every image and preserve its aspect ratio");
        require(close(imageBounds.width(), cellBounds.width()) ||
                    close(imageBounds.height(), cellBounds.height()),
                "automatic fit must use the largest scale that preserves the entire image");
        require(!document.items()[cell.itemIndex].pdfPlacement.millimetersPerPixel &&
                    document.items()[cell.itemIndex].pdfPlacement.centerOffsetMillimeters.isNull(),
                "automatic layout must clear earlier manual scale and position adjustments");
    }
}
void saveFixture(QWidget& widget, const char* name) {
    const auto directory = qEnvironmentVariable("SNOW_TEST_TEMPLATE_PREVIEW_DIR");
    if (directory.isEmpty())
        return;
    require(QDir().mkpath(directory), "preview fixture directory must be available");
    require(widget.grab().save(QDir(directory).filePath(QString::fromLatin1(name))),
            "template preview fixture must save");
}

void sharedSelectionMetricsAndDpi() {
    const QRectF imageBounds(40.25, 35.75, 100.5, 70.25);
    const auto frame = snowCanvasImageSelectionFrame(imageBounds);
    require(frame == imageBounds.adjusted(-4, -4, 4, 4),
            "selection frame must preserve fractional coordinates and engine four-pixel padding");
    const auto corners = snowCanvasImageSelectionCorners(imageBounds);
    require(snowCanvasImageSelectionHandleAt(imageBounds, corners[0] + QPointF(-5.9, -5.9)) ==
                SnowCanvasSelectionHandle::TopLeft,
            "corner hit squares must include six logical pixels on each side");
    require(snowCanvasImageSelectionHandleAt(imageBounds, corners[0] + QPointF(-6.1, -6.1)) ==
                SnowCanvasSelectionHandle::None,
            "corner hit squares must not expand with device pixel ratio or canvas zoom");
    require(snowCanvasImageSelectionHandleAt(imageBounds,
                                             QPointF(frame.center().x(), frame.top() + 5.9)) ==
                SnowCanvasSelectionHandle::Top,
            "engine selection edge bands must remain interactive without extra visible handles");
    require(snowCanvasImageSelectionHandleAt(imageBounds,
                                             QPointF(frame.center().x(), frame.top() - 20)) ==
                SnowCanvasSelectionHandle::None,
            "template selection must expose no free-rotation handle");
    for (const qreal dpr : std::array<qreal, 4>{1, 1.25, 1.5, 2}) {
        QImage surface(QSize(qCeil(220 * dpr), qCeil(180 * dpr)),
                       QImage::Format_ARGB32_Premultiplied);
        surface.setDevicePixelRatio(dpr);
        surface.fill(QColor(230, 230, 230));
        {
            QPainter painter(&surface);
            snowCanvasPaintImageSelection(painter, imageBounds);
        }
        const QPoint center(qRound(corners[0].x() * dpr), qRound(corners[0].y() * dpr));
        require(surface.pixelColor(center).red() > 245 && surface.pixelColor(center).green() > 245,
                "corner handle fill must remain white at fractional and integral DPI");
        const QPoint outside(qRound((corners[0].x() - 6) * dpr), qRound(corners[0].y() * dpr));
        require(surface.pixelColor(outside) == QColor(230, 230, 230),
                "eight-pixel visual handles must not grow to their twelve-pixel hit areas");
    }
}

void requireSolidCellBorder(const QImage& surface, const QRectF& bounds, const QColor& stroke) {
    for (int edge = 0; edge < 4; ++edge) {
        const bool vertical = edge < 2;
        const int across = qRound(vertical ? (edge == 0 ? bounds.left() : bounds.right())
                                           : (edge == 2 ? bounds.top() : bounds.bottom()));
        const int start = qCeil(vertical ? bounds.top() : bounds.left()) + 8;
        const int end = qFloor(vertical ? bounds.bottom() : bounds.right()) - 8;
        const auto thicknessAt = [&](int along) {
            int count = 0;
            for (int pixel = across - 4; pixel <= across + 4; ++pixel) {
                const QPoint sample = vertical ? QPoint(pixel, along) : QPoint(along, pixel);
                if (surface.pixelColor(sample) == stroke)
                    ++count;
            }
            return count;
        };
        for (int along = start; along <= end; ++along)
            require(thicknessAt(along) > 0,
                    "every occupied and empty cell edge must be a continuous #F0F0F0 line");
        const int thickness = thicknessAt(qRound(start + (end - start) * .37));
        require(thickness == qRound(surface.devicePixelRatio()),
                qPrintable(QStringLiteral("Cell edge %1 at DPR %2 has thickness %3; expected %4 "
                                          "(bounds %5,%6 %7x%8)")
                               .arg(edge)
                               .arg(surface.devicePixelRatio())
                               .arg(thickness)
                               .arg(qRound(surface.devicePixelRatio()))
                               .arg(bounds.x())
                               .arg(bounds.y())
                               .arg(bounds.width())
                               .arg(bounds.height())));
    }
}

void solidCellBordersAtEveryDpi() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalConfig = themes.config();
    ImageStitchingDocument document;
    configure(document);
    ImageStitchingPdfTemplateCanvas canvas(&document);
    // Distinguish border coverage from the surrounding canvas in both themes.
    auto palette = canvas.palette();
    palette.setColor(QPalette::Window, QColor(32, 48, 64));
    canvas.setPalette(palette);
    canvas.setAutoFillBackground(true);
    show(canvas, {803, 727});
    canvas.setExportBusy(true);
    for (const auto scheme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(scheme);
        auto seed = themes.seed();
        seed.primary =
            scheme == adqt::theme::ThemeScheme::Light ? QColor("#c41d7f") : QColor("#13c2c2");
        themes.setSeed(seed);
        const QColor stroke(QStringLiteral("#F0F0F0"));
        for (const auto type :
             {ImageStitchingPdfTemplateType::TwoImages, ImageStitchingPdfTemplateType::IdCard,
              ImageStitchingPdfTemplateType::ThreeImages, ImageStitchingPdfTemplateType::FourImages,
              ImageStitchingPdfTemplateType::FiveImages, ImageStitchingPdfTemplateType::SixImages,
              ImageStitchingPdfTemplateType::SevenImages,
              ImageStitchingPdfTemplateType::EightImages,
              ImageStitchingPdfTemplateType::NineImages}) {
            document.clear();
            configure(document, type);
            // Leave the final cell empty; occupied cells cover their clips right up to the guides.
            for (int index = 1; index < imageStitchingPdfTemplateCapacity(type); ++index) {
                const auto id =
                    document.append(image({64, 48}, QColor(240, 220, 180)), QString::number(index));
                setManualCoverPlacement(document, id);
            }
            document.select(0);
            for (const qreal spacing : {qreal{0}, qreal{5}}) {
                auto settings = document.pdfSettings();
                settings.imageSpacingMm = spacing;
                settings.marginLeftMm = settings.marginTopMm = spacing == 0 ? 0 : 10;
                settings.marginRightMm = settings.marginBottomMm = spacing == 0 ? 0 : 10;
                document.setPdfSettings(settings);
                for (const auto& item : document.items())
                    setManualCoverPlacement(document, item.id);
                const auto page =
                    calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
                const auto paper = canvas.pageViewportRect();
                const qreal viewScale = paper.width() / page.pageMillimeters.width();
                for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0, 3.0}) {
                    for (const QPoint offset : {QPoint{}, QPoint{3, 5}}) {
                        QImage surface(QSize(qCeil((canvas.width() + 6) * dpr),
                                             qCeil((canvas.height() + 10) * dpr)),
                                       QImage::Format_ARGB32_Premultiplied);
                        surface.setDevicePixelRatio(dpr);
                        surface.fill(Qt::transparent);
                        {
                            QPainter painter(&surface);
                            canvas.render(&painter, offset, {}, QWidget::DrawWindowBackground);
                        }
                        for (const auto& cell : page.cells) {
                            const QRectF bounds((paper.topLeft() +
                                                 cell.clipMillimeters.topLeft() * viewScale +
                                                 offset) *
                                                    dpr,
                                                cell.clipMillimeters.size() * viewScale * dpr);
                            requireSolidCellBorder(surface, bounds, stroke);
                        }
                    }
                }
            }
        }
    }
    themes.setConfig(originalConfig);
}

void emptyPreviewMatchesPageBasedStitching() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalConfig = themes.config();
    ImageStitchingDocument document;
    configure(document);
    ImageStitchingPdfTemplateWidget widget(&document);
    ImageStitchingPdfPageGrid grid(&document);
    show(widget);
    show(grid);
    auto* empty = dynamic_cast<ImageDropSurface*>(
        widget.findChild<QWidget*>(QStringLiteral("collagePdfTemplateEmptyState")));
    auto* pageEmpty = dynamic_cast<ImageDropSurface*>(
        grid.findChild<QWidget*>(QStringLiteral("collagePdfEmptyState")));
    auto* add = button(widget, "collagePdfTemplateEmptyAddImages");
    auto* pageAdd = button(grid, "collagePdfEmptyAddImages");
    auto* strip = widget.findChild<QWidget*>(QStringLiteral("collagePdfTemplateThumbnailStrip"));
    auto* pageNumber = widget.findChild<QLabel*>(QStringLiteral("collagePdfTemplatePageNumber"));
    auto* toolbar = widget.findChild<QWidget*>(QStringLiteral("collagePdfTemplateCanvasToolbar"));
    require(empty && pageEmpty && strip && pageNumber && toolbar,
            "empty template fixture must have its import surface and populated controls");
    for (const auto scheme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(scheme);
        for (const QSize size : {QSize(803, 727), QSize(480, 420)}) {
            widget.resize(size);
            grid.resize(size);
            pump();
            add->clearFocus();
            pageAdd->clearFocus();
            require(empty->isVisible() && !strip->isVisible() && !pageNumber->isVisible() &&
                        !toolbar->isVisible() && empty->size() == pageEmpty->size() &&
                        empty->size() == widget.size(),
                    "empty templates must fill the same preview area as empty page stitching "
                    "without pagination, thumbnails, or floating actions");
            require(empty->title->text() == pageEmpty->title->text() &&
                        empty->description->text() == pageEmpty->description->text() &&
                        add->text() == pageAdd->text() && add->isVisible() &&
                        add->accessibleName() == add->text(),
                    "empty templates must share page stitching's import copy and accessible add "
                    "action");
            saveFixture(widget, "empty-template.png");
            saveFixture(grid, "empty-pages.png");
            require(empty->grab().toImage() == pageEmpty->grab().toImage(),
                    "empty template and page stitching must render the same import surface "
                    "in both themes and at different viewport sizes");
        }
    }
    int additions = 0;
    QObject::connect(&widget, &ImageStitchingPdfTemplateWidget::addImagesRequested, &widget,
                     [&] { ++additions; });
    add->click();
    require(additions == 1, "empty template add action must request image import");
    widget.setLoading(true);
    require(!add->isEnabled() && add->busy(),
            "empty template import must disable and busy its add action");
    widget.setLoading(false);
    widget.setExportBusy(true);
    require(!add->isEnabled() && !add->busy(), "export must disable the empty template add action");
    widget.setExportBusy(false);
    require(add->isEnabled(), "empty template add action must recover after busy work");
    document.append(image({80, 60}, Qt::red), QStringLiteral("First"));
    pump();
    require(!empty->isVisible() && strip->isVisible() && pageNumber->isVisible() &&
                toolbar->isVisible() && !widget.canvas()->pageViewportRect().isEmpty(),
            "adding an image must restore the template preview, navigation, and actions");
    document.clear();
    pump();
    require(empty->isVisible() && !strip->isVisible() && !pageNumber->isVisible() &&
                !toolbar->isVisible() && add->isEnabled(),
            "clearing template images must restore the full empty import surface");
    saveFixture(widget, "empty-template.png");
    saveFixture(grid, "empty-pages.png");
    themes.setConfig(originalConfig);
}

void thumbnailReplacementAndDeletion() {
    ImageStitchingDocument document;
    configure(document);
    const auto first = document.append(image({80, 60}, Qt::red), QStringLiteral("first"));
    const auto second = document.append(image({60, 80}, Qt::blue), QStringLiteral("second"));
    ImageStitchingPdfTemplateWidget widget(&document);
    show(widget);
    auto* thumbnail =
        widget.findChild<QWidget*>(QStringLiteral("collagePdfTemplateThumbnail_%1").arg(second));
    require(thumbnail != nullptr, "PDF input thumbnail must exist");
    thumbnail->clearFocus();
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(thumbnail, &leave);
    pump();
    auto* replace = button(*thumbnail, "collagePdfTemplateReplaceThumbnailImage");
    auto* remove = button(*thumbnail, "collagePdfTemplateDeleteThumbnailImage");
    require(!replace->isVisible() && !remove->isVisible(),
            "unfocused PDF thumbnails hide their corner controls");
    thumbnail->setFocus();
    pump();
    require(dynamic_cast<OverlayControlButton*>(replace) &&
                dynamic_cast<OverlayControlButton*>(remove) && replace->isVisible() &&
                remove->isVisible() && replace->size() == QSize(28, 28) &&
                remove->pos() == QPoint(thumbnail->width() - 32, thumbnail->height() - 32) &&
                replace->pos() == remove->pos() - QPoint(32, 0),
            "PDF inputs use the same focused bottom-right overlays as static thumbnails");
    saveFixture(widget, "template-thumbnail-actions.png");
    replace->setFocus();
    pump();
    require(replace->isVisible() && remove->isVisible(),
            "corner controls remain visible when focus moves into an action");
    document.select(first);
    quint64 replacement = 0;
    QObject::connect(&widget, &ImageStitchingPdfTemplateWidget::replaceImageRequested, &widget,
                     [&](quint64 id) { replacement = id; });
    replace->click();
    require(replacement == second && document.selectedId() == first && document.items().size() == 2,
            "PDF thumbnail replacement targets its own input independently of selection");
    widget.setLoading(true);
    require(!replace->isEnabled() && remove->isEnabled(),
            "loading blocks PDF thumbnail replacement while retaining deletion like static mode");
    widget.setLoading(false);
    widget.setExportBusy(true);
    replace->click();
    remove->click();
    require(!replace->isEnabled() && !remove->isEnabled() && document.items().size() == 2,
            "export disables both PDF thumbnail edits");
    widget.setExportBusy(false);
    QTranslator translator;
    require(translator.load(QStringLiteral(":/i18n/snow_shot_zh_CN.qm")),
            "load PDF thumbnail translations");
    QApplication::installTranslator(&translator);
    QEvent language(QEvent::LanguageChange);
    QApplication::sendEvent(&widget, &language);
    require(replace->toolTip() == QCoreApplication::translate("ImageStitchingPdfTemplateWidget",
                                                              "Replace image") &&
                remove->accessibleName() ==
                    QCoreApplication::translate("ImageStitchingPdfTemplateWidget", "Delete image"),
            "PDF corner actions retranslate their accessible labels and tooltips");
    QApplication::removeTranslator(&translator);
    remove->click();
    pump();
    require(document.items().size() == 1 && document.items().front().id == first,
            "PDF thumbnail deletion removes only its own input");
}

void paginationThumbnailsAndSwaps() {
    ImageStitchingDocument document;
    configure(document);
    for (int index = 0; index < 1000; ++index)
        document.append(image({32, 24}, QColor::fromHsv(index % 360, 180, 220)),
                        QString::number(index));
    const auto selected = document.items().front().id;
    document.select(selected);
    ImageStitchingPdfTemplateWidget widget(&document);
    show(widget);
    require(widget.canvas()->pageIndex() == 0 &&
                widget.property("materializedThumbnailCount").toInt() > 0 &&
                widget.property("materializedThumbnailCount").toInt() < 20,
            "all-input thumbnail strip must materialize only its viewport and a small overscan");
    const auto last = document.items().back().id;
    document.select(last);
    pump();
    require(widget.canvas()->pageIndex() == 499,
            "selecting an input must navigate to its composed page");
    auto* lastThumbnail =
        widget.findChild<QWidget*>(QStringLiteral("collagePdfTemplateThumbnail_%1").arg(last));
    require(
        lastThumbnail && lastThumbnail->isVisible() &&
            widget.property("materializedThumbnailCount").toInt() < 20,
        "far-page selection must reveal its thumbnail without materializing intermediate inputs");
    require(widget.findChild<QLabel*>(QStringLiteral("collagePdfTemplatePageNumber"))->text() ==
                QStringLiteral("Page 500 of 500"),
            "navigation must report final composed page count");
    button(widget, "collagePdfTemplateReverseImages")->click();
    pump();
    require(document.items().front().id == last && document.selectedId() == last &&
                widget.canvas()->pageIndex() == 0,
            "reversing all inputs must repaginate and follow the selected image identity");
    const auto other = document.items()[3].id;
    auto* target =
        widget.findChild<QWidget*>(QStringLiteral("collagePdfTemplateThumbnail_%1").arg(other));
    require(target != nullptr, "nearby input on a different page must have a thumbnail");
    QMimeData mime;
    mime.setData(QStringLiteral("application/x-snow-shot-collage-item"),
                 QByteArray::number(reinterpret_cast<quintptr>(&document)) + ':' +
                     QByteArray::number(last));
    QDragEnterEvent enter(QPointF(target->rect().center()), Qt::MoveAction, &mime, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    require(enter.isAccepted(), "thumbnail must accept document-local image swaps");
    QDropEvent drop(QPointF(target->rect().center()), Qt::MoveAction, &mime, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
    pump();
    require(drop.isAccepted() && document.items()[3].id == last &&
                document.items().front().id == other && widget.canvas()->pageIndex() == 1,
            "thumbnail drop must swap across composed pages and follow the dragged image");
    key(widget.canvas(), Qt::Key_Delete);
    require(document.selectedId() != last && widget.canvas()->pageIndex() == 1,
            "deleting a selected input must select its next ordinal and keep a valid page");
    document.select(document.items().front().id);
    pump();
    auto* focused = widget.findChild<QWidget*>(
        QStringLiteral("collagePdfTemplateThumbnail_%1").arg(document.selectedId()));
    require(focused != nullptr, "selected thumbnail must be materialized for keyboard navigation");
    focused->setFocus(Qt::OtherFocusReason);
    for (int index = 1; index <= 10; ++index) {
        key(QApplication::focusWidget(), Qt::Key_Right);
        require(document.indexOf(document.selectedId()) == index &&
                    widget.canvas()->pageIndex() == index / 2,
                "repeated thumbnail arrows must advance selection across pages and virtualization "
                "boundaries");
    }
}

void centeredResizeMoveResetAndCancellation() {
    ImageStitchingDocument document;
    configure(document);
    const auto first = document.append(image({800, 500}, Qt::red), QStringLiteral("First"));
    const auto second = document.append(image({400, 600}, Qt::blue), QStringLiteral("Second"));
    document.select(first);
    ImageStitchingPdfTemplateWidget widget(&document);
    show(widget);
    auto* canvas = widget.canvas();
    requireVisibleImagesContained(document, *canvas);
    const auto fitted = canvas->imageViewportRect(first);
    const auto center = fitted.center();
    const auto handle = snowCanvasImageSelectionCorners(fitted)[2];
    int placements = 0;
    QObject::connect(&document, &ImageStitchingDocument::pdfPlacementChanged, &widget,
                     [&](quint64) { ++placements; });
    const auto originalRevision = document.contentRevision();
    press(canvas, handle);
    move(canvas, handle - (fitted.bottomRight() - center) * .5);
    require(!document.items()[document.indexOf(first)].pdfPlacement.millimetersPerPixel &&
                placements == 0,
            "gesture adjustments must remain transient until release");
    release(canvas, handle - (fitted.bottomRight() - center) * .5);
    const auto smaller = canvas->imageViewportRect(first);
    require(close(smaller.width(), fitted.width() * .5) &&
                close(smaller.height(), fitted.height() * .5) &&
                (smaller.center() - center).manhattanLength() < .001 && placements == 1 &&
                document.contentRevision() == originalRevision,
            "corner resize must preserve aspect and center without invalidating immutable source "
            "revisions");
    drag(canvas, smaller.center(), smaller.center() + QPointF(15, 12));
    const auto moved = canvas->imageViewportRect(first);
    require((moved.center() - smaller.center() - QPointF(15, 12)).manhattanLength() < .001,
            "drag within the assigned cell must adjust position and permit whitespace");
    saveFixture(widget, "selected-shrink.png");
    const auto committed = document.items()[document.indexOf(first)].pdfPlacement;
    press(canvas, moved.center());
    move(canvas, moved.center() + QPointF(20, 10));
    key(canvas, Qt::Key_Escape);
    release(canvas, moved.center() + QPointF(20, 10));
    require(document.items()[document.indexOf(first)].pdfPlacement == committed &&
                canvas->imageViewportRect(first) == moved,
            "Escape must cancel the gesture without committing or swapping");
    button(widget, "collagePdfTemplateRotateRight")->click();
    const auto rotated = canvas->imageViewportRect(first);
    require((rotated.center() - moved.center()).manhattanLength() < .001 &&
                close(rotated.width(), moved.height()) && close(rotated.height(), moved.width()),
            "rotation must preserve center and displayed source scale");
    button(widget, "collagePdfTemplateRotateLeft")->click();
    button(widget, "collagePdfTemplateFlipHorizontal")->click();
    button(widget, "collagePdfTemplateFlipHorizontal")->click();
    require(canvas->imageViewportRect(first) == moved,
            "paired rotations and flips must preserve manual placement");
    button(widget, "collagePdfTemplateResetImage")->click();
    require(
        canvas->imageViewportRect(first) == fitted &&
            !document.items()[document.indexOf(first)].pdfPlacement.millimetersPerPixel,
        "reset must restore complete-image centered fit even when orientation is already identity");
    requireVisibleImagesContained(document, *canvas);
    drag(canvas, canvas->cellViewportRect(first).center(),
         canvas->cellViewportRect(second).center());
    require(document.items().front().id == second && document.items().back().id == first &&
                document.selectedId() == first,
            "dragging over another occupied cell must swap complete input images");
    const auto releaseStart = canvas->cellViewportRect(first).center();
    press(canvas, releaseStart);
    move(canvas, releaseStart + QPointF(12, 10));
    release(canvas, canvas->cellViewportRect(second).center());
    require(document.items().front().id == first,
            "release over another cell must determine the swap even when the last move stayed in "
            "the source");
    const auto outsideStart = canvas->cellViewportRect(first).center();
    press(canvas, outsideStart);
    move(canvas, canvas->cellViewportRect(second).center());
    release(canvas, QPointF(canvas->width() - 2, canvas->height() / 2));
    require(document.items().front().id == first,
            "release outside every cell must not use a stale swap highlight from the last move");
    button(widget, "collagePdfTemplateResetImage")->click();
    const auto cell = canvas->cellViewportRect(first);
    const auto selection = canvas->imageViewportRect(first);
    const auto corner = snowCanvasImageSelectionCorners(selection)[0];
    press(canvas, corner);
    move(canvas, canvas->cellViewportRect(second).center());
    require(canvas->property("swapTargetId").toULongLong() == 0,
            "resizing across another cell must never become an image-swap gesture");
    widget.setExportBusy(true);
    release(canvas, cell.center());
    require(!document.items()[document.indexOf(first)].pdfPlacement.millimetersPerPixel,
            "starting export must cancel transient placement before snapshotting output");
    widget.setExportBusy(false);
    saveFixture(widget, "centered-fit-after-reset.png");
}

void automaticContainmentAcrossTemplatesAndLayoutChanges() {
    ImageStitchingDocument document;
    configure(document);
    for (int index = 0; index < 9; ++index) {
        const QSize size = index % 3 == 0   ? QSize(960, 120)
                           : index % 3 == 1 ? QSize(120, 960)
                                            : QSize(500, 500);
        document.append(image(size, QColor::fromHsv(index * 35, 160, 220)), QString::number(index));
    }
    ImageStitchingPdfTemplateWidget widget(&document);
    show(widget);
    const auto checkEveryPage = [&] {
        const auto count =
            imageStitchingPdfPageCount(document.items().size(), document.lastValidPdfSettings());
        for (qsizetype page = 0; page < count; ++page) {
            widget.canvas()->setPageIndex(static_cast<int>(page));
            pump();
            requireVisibleImagesContained(document, *widget.canvas());
        }
    };
    constexpr std::array types{
        ImageStitchingPdfTemplateType::TwoImages,   ImageStitchingPdfTemplateType::IdCard,
        ImageStitchingPdfTemplateType::ThreeImages, ImageStitchingPdfTemplateType::FourImages,
        ImageStitchingPdfTemplateType::FiveImages,  ImageStitchingPdfTemplateType::SixImages,
        ImageStitchingPdfTemplateType::SevenImages, ImageStitchingPdfTemplateType::EightImages,
        ImageStitchingPdfTemplateType::NineImages};
    for (const auto type : types) {
        for (const auto orientation :
             {ImageStitchingPdfOrientation::Portrait, ImageStitchingPdfOrientation::Landscape}) {
            auto settings = document.pdfSettings();
            settings.templateType = type;
            settings.orientation = orientation;
            document.setPdfSettings(settings);
            checkEveryPage();
        }
    }
    document.select(document.items().front().id);
    const auto first = document.selectedId();
    const std::array<std::function<void(ImageStitchingPdfSettings&)>, 8> changes{
        [](auto& settings) { settings.templateType = ImageStitchingPdfTemplateType::FourImages; },
        [](auto& settings) { settings.paperSize = ImageStitchingPdfPaperSize::A5; },
        [](auto& settings) { settings.orientation = ImageStitchingPdfOrientation::Portrait; },
        [](auto& settings) { settings.marginLeftMm += 3; },
        [](auto& settings) { settings.imageSpacingMm += 2; },
        [](auto& settings) { settings.paperSize = ImageStitchingPdfPaperSize::ImageSize; },
        [](auto& settings) {
            settings.paperSize = ImageStitchingPdfPaperSize::Custom;
            settings.customWidthMm = 240;
            settings.customHeightMm = 320;
        },
        [](auto& settings) { settings.customWidthMm += 20; },
    };
    for (const auto& change : changes) {
        document.select(first);
        setManualCoverPlacement(document, first, 1.4);
        auto placement = document.items()[document.indexOf(first)].pdfPlacement;
        placement.centerOffsetMillimeters = {11, -9};
        document.setPdfPlacement(first, placement);
        require(!widget.canvas()->cellViewportRect(first).contains(
                    widget.canvas()->imageViewportRect(first)),
                "layout reset fixture must begin with an explicitly enlarged and moved image");
        auto settings = document.pdfSettings();
        change(settings);
        document.setPdfSettings(settings);
        checkEveryPage();
    }
}

void clippingBlankCellsAndDetails() {
    ImageStitchingDocument document;
    configure(document);
    const auto first = document.append(image({1200, 400}, Qt::red), QStringLiteral("Red"));
    document.append(image({600, 600}, Qt::blue), QStringLiteral("Blue"));
    setManualCoverPlacement(document, first);
    document.select(first);
    ImageStitchingPdfTemplateWidget widget(&document);
    show(widget, {850, 850});
    auto* canvas = widget.canvas();
    const auto cell = canvas->cellViewportRect(first);
    const auto paper = canvas->pageViewportRect();
    const QPoint sample(qRound(std::min(cell.right() + 5, paper.right() - 3)),
                        qRound(cell.center().y()));
    const auto ghost = canvas->grab().toImage().pixelColor(sample);
    require(ghost.red() > ghost.green() + 20 && ghost.green() > 100,
            "selected overflow must be visible semi-transparently outside its cell");
    saveFixture(widget, "selected-overflow.png");
    document.select(0);
    pump();
    const auto clipped = canvas->grab().toImage().pixelColor(sample);
    require(clipped == QColor(Qt::white), "unselected overflow must be strictly clipped");
    document.select(first);
    waitUntil([&] { return widget.property("detailBytes").toLongLong() > 0; });
    require(widget.property("detailBytes").toLongLong() <= qint64{32} * 1024 * 1024,
            "active-page detailed rasters must remain bounded");
    const auto bytes = widget.property("detailBytes").toLongLong();
    auto placement = document.items().front().pdfPlacement;
    placement.centerOffsetMillimeters = {4, 3};
    document.setPdfPlacement(first, placement);
    pump();
    require(widget.property("detailBytes").toLongLong() == bytes,
            "placement-only edits must reuse detailed source rasters");
    document.append(image({400, 300}, Qt::green), QStringLiteral("Third"));
    canvas->setPageIndex(1);
    pump();
    require(canvas->pageIndex() == 1 && widget.property("detailBytes").toLongLong() == 0,
            "page navigation must retire previous-page detailed rasters");
    const auto third = document.items().back().id;
    const auto before = document.items();
    const auto occupied = canvas->cellViewportRect(third);
    const QPointF blankCenter(occupied.center().x(),
                              paper.bottom() - (occupied.center().y() - paper.top()));
    drag(canvas, occupied.center(), blankCenter);
    require(document.items().front().id == before.front().id && document.items().back().id == third,
            "unused final-page cells must remain blank and cannot swap input order");
    const auto blankGhost = canvas->grab().toImage().pixelColor(blankCenter.toPoint());
    require(blankGhost.green() > blankGhost.red() + 20 && blankGhost.red() > 100,
            "selected overflow must remain translucent over an unused cell's whitespace");
}

void longThinImagesPrepareVisibleSourceDetail() {
    ImageStitchingDocument document;
    configure(document);
    QImage stripes({6400, 64}, QImage::Format_RGB32);
    stripes.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < stripes.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(stripes.scanLine(y));
        for (int x = 0; x < stripes.width(); ++x)
            row[x] = x % 2 == 0 ? qRgb(255, 0, 0) : qRgb(0, 0, 255);
    }
    const auto id = document.append(stripes, QStringLiteral("Long thin stripes"));
    setManualCoverPlacement(document, id);
    document.select(0);
    ImageStitchingPdfTemplateWidget widget(&document);
    show(widget, {850, 850});
    auto* canvas = widget.canvas();
    const auto cell = canvas->cellViewportRect(id);
    const auto placed = canvas->imageViewportRect(id);
    const qreal sourceCenter =
        (cell.center().x() - placed.left()) / placed.width() * stripes.width();
    const int firstSourcePixel = qFloor(sourceCenter);
    const auto sample = [&](int sourcePixel) {
        const qreal x = placed.left() + (sourcePixel + .5) * placed.width() / stripes.width();
        return QPoint(qRound(x - .5), qRound(cell.center().y() - .5));
    };
    const auto clearStripe = [](QColor color, int sourcePixel) {
        return sourcePixel % 2 == 0 ? color.red() - color.blue() > 140
                                    : color.blue() - color.red() > 140;
    };
    waitUntil([&] {
        const auto rendered = canvas->grab().toImage();
        return clearStripe(rendered.pixelColor(sample(firstSourcePixel)), firstSourcePixel) &&
               clearStripe(rendered.pixelColor(sample(firstSourcePixel + 1)), firstSourcePixel + 1);
    });
    require(
        widget.property("detailBytes").toLongLong() > 0 &&
            widget.property("detailBytes").toLongLong() <= qint64{32} * 1024 * 1024,
        "manually enlarged long thin images must prepare the visible source crop at bounded screen "
        "resolution");
    saveFixture(widget, "long-thin-detail.png");
}

void detailedAlphaDoesNotBlendThePreviewTwice() {
    ImageStitchingDocument document;
    configure(document);
    const auto id = document.append(image({1200, 400}, QColor(255, 0, 0, 128)),
                                    QStringLiteral("Transparent red"));
    document.select(0);
    ImageStitchingPdfTemplateWidget widget(&document);
    show(widget, {850, 850});
    auto* canvas = widget.canvas();
    const auto sample = canvas->cellViewportRect(id).center().toPoint();
    const auto preview = canvas->grab().toImage().pixelColor(sample);
    waitUntil([&] {
        return widget.property("detailBytes").toLongLong() > 0 &&
               ScreenshotExportCoordinator::shared().pendingJobCount() == 0;
    });
    pump();
    const auto detailed = canvas->grab().toImage().pixelColor(sample);
    require(std::abs(preview.red() - detailed.red()) <= 2 &&
                std::abs(preview.green() - detailed.green()) <= 2 &&
                std::abs(preview.blue() - detailed.blue()) <= 2 && detailed.green() > 120,
            "detailed alpha must replace its preview pixels without a second source-over blend");
}

void actionsLanguageAndVisualFixtures() {
    ImageStitchingDocument document;
    configure(document, ImageStitchingPdfTemplateType::FiveImages);
    for (int index = 0; index < 7; ++index)
        document.append(image({700 + index * 30, 450}, QColor::fromHsv(index * 45, 140, 210)),
                        QStringLiteral("Image %1").arg(index + 1));
    ImageStitchingPdfTemplateWidget widget(&document);
    show(widget);
    saveFixture(widget, "portrait-five.png");
    auto settings = document.pdfSettings();
    settings.templateType = ImageStitchingPdfTemplateType::IdCard;
    settings.orientation = ImageStitchingPdfOrientation::Landscape;
    document.setPdfSettings(settings);
    pump();
    saveFixture(widget, "landscape-id.png");
    int add = 0;
    int clear = 0;
    quint64 replace = 0;
    QObject::connect(&widget, &ImageStitchingPdfTemplateWidget::addImagesRequested, &widget,
                     [&] { ++add; });
    QObject::connect(&widget, &ImageStitchingPdfTemplateWidget::clearRequested, &widget,
                     [&] { ++clear; });
    QObject::connect(&widget, &ImageStitchingPdfTemplateWidget::replaceImageRequested, &widget,
                     [&](quint64 id) { replace = id; });
    button(widget, "collagePdfTemplateAddImages")->click();
    button(widget, "collagePdfTemplateClear")->click();
    button(widget, "collagePdfTemplateReplaceImage")->click();
    require(add == 1 && clear == 1 && replace == document.selectedId() &&
                document.items().size() == 7,
            "add/clear/replace toolbar actions must request their existing workflows");
    button(widget, "collagePdfTemplatePreviewImage")->click();
    pump();
    auto* viewer = widget.findChild<adqt::widgets::AdImageViewer*>();
    require(viewer && viewer->isVisible() &&
                viewer->currentRow() == document.indexOf(document.selectedId()),
            "original-image preview must open the selected source");
    const auto viewed = document.items()[1].id;
    viewer->openAt(1);
    document.swapItems(viewed, document.items()[4].id);
    pump();
    require(viewer->isVisible() && viewer->currentRow() == document.indexOf(viewed),
            "an open original viewer must follow the viewed input identity through reordering");
    document.replace(viewed, image({480, 320}, Qt::cyan), QStringLiteral("Replacement"));
    pump();
    auto* originalModel = widget.findChild<adqt::widgets::AdImageListModel*>();
    require(originalModel && viewer->isVisible() &&
                originalModel->items()[viewer->currentRow()].source.query() ==
                    QString::number(document.items()[document.indexOf(viewed)].image.cacheKey()),
            "replacement must refresh an open original viewer with the new immutable source");
    document.remove(viewed);
    pump();
    require(viewer->isVisible() && viewer->currentRow() == document.indexOf(document.selectedId()),
            "removing the viewed input must fall back to the current selected image");
    document.clear();
    pump();
    require(!viewer->isVisible(), "clearing all inputs must close and retire the original viewer");
    class Translator final : public QTranslator {
      public:
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            if (QByteArray(context) != "ImageStitchingPdfTemplateWidget")
                return {};
            if (QByteArray(source) == "Reverse image order")
                return QStringLiteral("Translated reverse");
            if (QByteArray(source) == "Add images to create a PDF document")
                return QStringLiteral("Translated empty title");
            if (QByteArray(source) == "Drag image files here, or choose add images")
                return QStringLiteral("Translated empty description");
            if (QByteArray(source) == "Add images")
                return QStringLiteral("Translated add");
            return {};
        }
    } translator;
    QApplication::installTranslator(&translator);
    QEvent event(QEvent::LanguageChange);
    QApplication::sendEvent(&widget, &event);
    require(button(widget, "collagePdfTemplateReverseImages")->accessibleName() ==
                QStringLiteral("Translated reverse"),
            "language changes must update floating action accessibility and tooltips");
    auto* empty = dynamic_cast<ImageDropSurface*>(
        widget.findChild<QWidget*>(QStringLiteral("collagePdfTemplateEmptyState")));
    auto* emptyAdd = button(widget, "collagePdfTemplateEmptyAddImages");
    require(empty && empty->title->text() == QStringLiteral("Translated empty title") &&
                empty->description->text() == QStringLiteral("Translated empty description") &&
                emptyAdd->text() == QStringLiteral("Translated add") &&
                emptyAdd->accessibleName() == emptyAdd->text(),
            "language changes must refresh the empty template guide and its accessible add action");
    QApplication::removeTranslator(&translator);
    document.clear();
    pump();
    require(button(widget, "collagePdfTemplateEmptyAddImages")->isVisible() &&
                !button(widget, "collagePdfTemplateAddImages")->isVisible() &&
                !button(widget, "collagePdfTemplateClear")->isEnabled() &&
                !button(widget, "collagePdfTemplateReverseImages")->isEnabled(),
            "empty template state must expose its import guide instead of floating actions");
}

void fullEditorVisualFixtures() {
    if (qEnvironmentVariableIsEmpty("SNOW_TEST_TEMPLATE_PREVIEW_DIR"))
        return;
    ImageStitchingDocument document;
    configure(document, ImageStitchingPdfTemplateType::FiveImages);
    auto settings = document.pdfSettings();
    settings.paperSize = ImageStitchingPdfPaperSize::ImageSize;
    document.setPdfSettings(settings);
    for (int index = 0; index < 7; ++index)
        document.append(image({700 + index * 30, 450}, QColor::fromHsv(index * 45, 140, 210)),
                        QStringLiteral("Image %1").arg(index + 1));
    ImageStitchingEditorWidget editor(&document);
    show(editor, {1180, 800});
    saveFixture(editor, "editor-template-five-image-size.png");
    settings.stitchingMethod = ImageStitchingPdfStitchingMethod::PageBased;
    document.setPdfSettings(settings);
    pump();
    saveFixture(editor, "editor-page-based.png");
    document.setOutputType(ImageStitchingOutputType::StaticImage);
    pump();
    saveFixture(editor, "editor-static-toolbar.png");
}

void invalidDraftKeepsTheLastValidPreviewMethod() {
    ImageStitchingDocument document;
    for (int index = 0; index < 5; ++index)
        document.append(image({80, 60}, QColor::fromHsv(index * 60, 150, 220)),
                        QString::number(index));
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
    document.select(document.items()[3].id);
    ImageStitchingEditorWidget editor(&document);
    show(editor, {1100, 760});
    require(editor.pdfGrid()->isVisible() && !editor.pdfTemplateWidget()->isVisible(),
            "valid page-based settings must show the existing page grid");
    auto draft = document.pdfSettings();
    draft.paperSize = ImageStitchingPdfPaperSize::Custom;
    draft.customWidthMm = 0;
    document.setPdfSettings(draft);
    draft.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
    document.setPdfSettings(draft);
    pump();
    require(!validateImageStitchingPdfSettings(document.pdfSettings()).isEmpty() &&
                editor.pdfGrid()->isVisible() && !editor.pdfTemplateWidget()->isVisible(),
            "an invalid raw template draft must keep the last valid page-based preview visible");
    draft.customWidthMm = 210;
    document.setPdfSettings(draft);
    pump();
    require(
        !editor.pdfGrid()->isVisible() && editor.pdfTemplateWidget()->isVisible() &&
            editor.pdfTemplateWidget()->canvas()->pageIndex() == 1,
        "repairing the draft must switch to template composition and follow the selected input");
    draft.marginTopMm = 999;
    document.setPdfSettings(draft);
    draft.stitchingMethod = ImageStitchingPdfStitchingMethod::PageBased;
    document.setPdfSettings(draft);
    pump();
    require(!validateImageStitchingPdfSettings(document.pdfSettings()).isEmpty() &&
                !editor.pdfGrid()->isVisible() && editor.pdfTemplateWidget()->isVisible() &&
                editor.pdfTemplateWidget()->canvas()->pageIndex() == 1,
            "an invalid raw page-based draft must preserve the last valid template preview and "
            "pagination");
    draft.marginTopMm = 10;
    document.setPdfSettings(draft);
    pump();
    require(editor.pdfGrid()->isVisible() && !editor.pdfTemplateWidget()->isVisible(),
            "repairing page margins must apply the newly valid page-based method");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        sharedSelectionMetricsAndDpi();
        solidCellBordersAtEveryDpi();
        emptyPreviewMatchesPageBasedStitching();
        paginationThumbnailsAndSwaps();
        thumbnailReplacementAndDeletion();
        centeredResizeMoveResetAndCancellation();
        automaticContainmentAcrossTemplatesAndLayoutChanges();
        clippingBlankCellsAndDetails();
        longThinImagesPrepareVisibleSourceDetail();
        detailedAlphaDoesNotBlendThePreviewTwice();
        actionsLanguageAndVisualFixtures();
        fullEditorVisualFixtures();
        invalidDraftKeepsTheLastValidPreviewMethod();
        std::cout << "collage PDF template tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
