// SPDX-License-Identifier: GPL-3.0-or-later
// Included after the pinned-window fixtures.
#include "../src/presentation/pinned/screenshotpinnedcropgeometry.h"
#include "../src/presentation/pinned/screenshotpinnedgeometrymapping.h"
#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include <QThreadPool>

namespace pinned_crop_tests {
using Access = ScreenshotPinnedWindowTestAccess;
using LiveAccess = ScreenshotLivePinnedWindowTestAccess;
using Handle = screenshot_pinned_resize_geometry::DragHandle;

ScreenshotPinnedWindow::Config config() {
    QImage image(100, 80, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(x * 2, y * 3, 100));
    ScreenshotPinnedWindow::Config result;
    result.screen = QGuiApplication::primaryScreen();
    result.nativeGeometry = QRect(220, 200, 200, 160);
    result.initialWindowSize = image.size();
    result.canvasSourceRect = QRectF(100, 200, 100, 80);
    result.imageSource = ScreenshotImageSource::fromImage(image, result.canvasSourceRect);
    result.automaticTextRecognition = false;
    result.initialBorderVisible = false;
    result.initialShadowVisible = true;
    return result;
}

QImage exportImage(ScreenshotPinnedWindow& owner,
                   std::shared_ptr<ScreenshotExportArtifact> artifact) {
    require(artifact != nullptr, "crop export artifact exists");
    bool done = false;
    ScreenshotExportImageResult output;
    require(artifact->requestImage(&owner,
                                   [&](ScreenshotExportImageResult result) {
                                       output = std::move(result);
                                       done = true;
                                   }),
            "crop export starts");
    translation_tests::waitUntil([&] { return done; }, "crop export finishes");
    require(output.succeeded(), "crop export succeeds");
    return output.image;
}

void geometry() {
    using namespace screenshot_pinned_crop_geometry;
    const QRectF bounds(-100, -200, 100, 80);
    const QRectF start(-80, -180, 30, 20);
    for (int i = 0; i < 8; ++i) {
        const auto handle = Handle(i);
        for (const QPointF delta : {QPointF(-150, -150), QPointF(150, 150), QPointF(0, 0),
                                    QPointF(25.5, -19.5), QPointF(-25, 60)}) {
            const auto adjusted = adjust(start, bounds, delta, handle, handle, QSizeF(10, 10));
            require(bounds.contains(adjusted.rect) && adjusted.rect.width() >= 10 &&
                        adjusted.rect.height() >= 10,
                    "every handle obeys source bounds and independent minimum extents");
        }
    }
    const auto crossed = adjust(start, bounds, {45, 0}, Handle::Left, Handle::Left, {10, 10});
    require(crossed.handle == Handle::Right && crossed.rect == QRectF(-50, -180, 15, 20),
            "crossing exchanges the moving edge without reflecting source coordinates");
    const auto tiny = adjust(QRectF(1, 1, 2, 2), QRectF(0, 0, 5, 7), {}, Handle::Right,
                             Handle::Right, QSizeF(10, 10));
    require(tiny.rect == QRectF(0, 0, 5, 7), "tiny source uses its complete available extent");
    for (int turns = 0; turns < 4; ++turns) {
        QTransform transform;
        transform.rotate(turns * 90);
        transform.scale(-1, 1);
        const QSize size(100, 80);
        const QRectF canvas(300, -200, turns % 2 ? 40 : 50, turns % 2 ? 50 : 40);
        const auto mapping = sourceToCanvas(size, transform, canvas);
        const QRectF crop(10, 20, 30, 40);
        require(mapping.inverted().mapRect(mapping.mapRect(crop)) == crop,
                "rotation, reflection and backing-scale crop mappings round-trip");
    }
}

void cornerOutlines() {
    ScreenshotPinnedWindow window;
    Access::prepareReplacement(window, config());
    window.show();
    require(Access::crop(window, {110, 210, 80, 60}), "corner outline crop fixture");
    auto* border = Access::decoration(window);
    const QColor base(210, 220, 230);
    const QColor active(30, 120, 240);
    border->setProperty("borderColor", base);
    border->setProperty("cropMarkerColor", active);
    const qreal dpr = border->devicePixelRatioF();
    const auto render = [&] {
        QImage image(qRound(border->width() * dpr), qRound(border->height() * dpr),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            border->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
        }
        image.setDevicePixelRatio(1);
        return image;
    };
    const auto contentPixels = [&] {
        QTransform toDevice;
        toDevice.scale(dpr, dpr);
        return ScreenshotPinnedGeometryMapping::roundedDeviceRect(
            toDevice.mapRect(Access::contentViewport(window)));
    };
    const auto mirroredPoint = [](const QRect& rect, int x, int y, bool right, bool bottom) {
        return QPoint(right ? rect.right() - x : rect.left() + x,
                      bottom ? rect.bottom() - y : rect.top() + y);
    };
    const int extent = qCeil(30 * dpr);
    const int width = qCeil(dpr);
    QRect content = contentPixels();
    const QImage square = render();
    for (bool right : {false, true}) {
        for (bool bottom : {false, true}) {
            for (int distance = 0; distance < extent; ++distance) {
                requireColorNear(
                    square.pixelColor(mirroredPoint(content, distance, 0, right, bottom)), active,
                    0, "every crop corner follows the horizontal rim for thirty logical pixels");
                requireColorNear(
                    square.pixelColor(mirroredPoint(content, 0, distance, right, bottom)), active,
                    0, "every crop corner follows the vertical rim for thirty logical pixels");
            }
            requireColorNear(
                square.pixelColor(mirroredPoint(content, extent, 0, right, bottom)), base, 0,
                "corner highlights stop after their minimum extent on a large square pin");
            require(square.pixelColor(
                        mirroredPoint(content, width + 1, width + 1, right, bottom)) != active,
                    "crop corners leave image pixels inside the rim unobscured");
        }
    }
    border->setProperty("cropMarkerColor", QColor());
    requireColorNear(render().pixelColor(content.topLeft()), QColor(128, 128, 128), 0,
                     "inactive crop corners keep their neutral outline color");
    border->setProperty("cropMarkerColor", active);

    // Exercise the decoration's rounded-outline contract independently of the rectangular
    // crop viewport. An elliptical radius also covers transformed, nonuniformly scaled rims.
    const QRectF logicalOutline = Access::contentViewport(window);
    border->setProperty("borderOutline", logicalOutline);
    for (const QSizeF radii : {QSizeF(12, 12), QSizeF(40, 20)}) {
        border->setProperty("cornerRadii", radii);
        border->setProperty("cropped", false);
        const QImage plain = render();
        border->setProperty("cropped", true);
        const QImage rounded = render();
        for (bool right : {false, true}) {
            for (bool bottom : {false, true}) {
                const QPoint corner = mirroredPoint(content, 0, 0, right, bottom);
                require(rounded.pixelColor(corner) == plain.pixelColor(corner),
                        "rounded crop corners leave the bounding-box corner clear");
                requireColorNear(rounded.pixelColor(mirroredPoint(
                                     content, qCeil(radii.width() * dpr), 0, right, bottom)),
                                 active, 1,
                                 "crop outlines cover the complete rounded horizontal corner");
                requireColorNear(rounded.pixelColor(mirroredPoint(
                                     content, 0, qCeil(radii.height() * dpr), right, bottom)),
                                 active, 1,
                                 "crop outlines cover the complete rounded vertical corner");
                bool curvedHighlight = false;
                for (int y = width; y < qFloor(radii.height() * dpr); ++y)
                    for (int x = width; x < qFloor(radii.width() * dpr); ++x) {
                        const QPoint point = mirroredPoint(content, x, y, right, bottom);
                        const QColor highlighted = rounded.pixelColor(point);
                        curvedHighlight |= highlighted != plain.pixelColor(point) &&
                                           highlighted.blue() - highlighted.red() > 80;
                    }
                require(curvedHighlight, "crop highlights follow the arc between straight edges");
            }
        }
    }

    LiveAccess::scale(window, 10);
    border->setProperty("borderColor", base);
    const QColor translucent(30, 120, 240, 128);
    border->setProperty("cropMarkerColor", translucent);
    content = contentPixels();
    const QImage tiny = render();
    QImage expected(1, 1, QImage::Format_ARGB32_Premultiplied);
    expected.fill(base);
    {
        QPainter painter(&expected);
        painter.fillRect(expected.rect(), translucent);
    }
    for (const QPoint point :
         {content.topLeft(), content.topRight(), content.bottomLeft(), content.bottomRight()})
        requireColorNear(tiny.pixelColor(point), expected.pixelColor(0, 0), 0,
                         "tiny crop outlines fit the rim and composite each corner only once");
    window.hide();
}

void cropSelectionProjectsNativeBounds() {
    ScreenshotPinnedWindow window;
    auto setup = config();
    setup.nativeGeometry = QRect(-537, -293, 201, 163);
    Access::prepareReplacement(window, setup);
    window.show();
    auto* canvas = window.findChild<SnowCanvasWidget*>();
    require(canvas != nullptr, "native crop projection has a canvas");
    const QPointF nativeOrigin = Access::authority(window).topLeft();
    const QPointF contentOrigin = LiveAccess::contentGeometry(window).topLeft();
    const qreal scale = snow_shot::presentation::pinnedGeometryScale(window.devicePixelRatioF());
    for (const QRectF& rect : {QRectF(10, 12, 61, 43), QRectF(10.25, 12.75, 61.5, 43.25),
                               QRectF(10.75, 12.25, 1.25, 43.5)}) {
        for (bool reverse : {false, true}) {
            const QPointF start = contentOrigin + (reverse ? rect.bottomRight() : rect.topLeft());
            const QPointF end = contentOrigin + (reverse ? rect.topLeft() : rect.bottomRight());
            require(Access::beginCrop(window, start), "begin native crop projection fixture");
            Access::moveCrop(window, end);
            for (QWidget* target : std::array<QWidget*, 2>{&window, canvas}) {
                const QRectF expected(target->mapFrom(&window, QPointF()) +
                                          (contentOrigin + rect.topLeft() - nativeOrigin) / scale,
                                      rect.size() / scale);
                const QRectF actual = Access::cropSelectionView(window, target);
                require(qAbs(actual.left() - expected.left()) < 1e-10 &&
                            qAbs(actual.top() - expected.top()) < 1e-10 &&
                            qAbs(actual.right() - expected.right()) < 1e-10 &&
                            qAbs(actual.bottom() - expected.bottom()) < 1e-10,
                        "crop projection preserves native bounds independently of widget rounding");
            }
            Access::endCrop(window, end, true);
            require(Access::cropSelectionView(window, canvas).isEmpty(),
                    "cancelling a crop clears its projected bounds");
        }
    }
    window.hide();
}

void cropSelectionMatchesCanvas() {
    ScreenshotPinnedWindow window;
    auto setup = config();
    setup.nativeGeometry.setSize(QSize(300, 240));
    Access::prepareReplacement(window, setup);
    window.show();
    auto* pinnedCanvas = window.findChild<SnowCanvasWidget*>();
    require(pinnedCanvas != nullptr, "crop selection paints on the pinned canvas");

    SnowCanvasWidget canvas;
    canvas.resize(pinnedCanvas->size());
    canvas.setClearBackgroundEnabled(false);
    canvas.setAttribute(Qt::WA_ShowWithoutActivating);
    canvas.show();
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "activate reference canvas selection");

    const auto render = [](QWidget& widget, qreal dpr) {
        QImage image(qCeil(widget.width() * dpr), qCeil(widget.height() * dpr),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        widget.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
        return image;
    };
    const auto pointer = [](QWidget& widget, QEvent::Type type, const QPointF& point,
                            Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, widget.mapToGlobal(point), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&widget, &event);
    };
    // Compare the same logical selection bounds. The native crop input must use the
    // paint scale, rather than the rounded QWidget extent used by hit testing.
    const QPointF nativeOrigin = Access::authority(window).topLeft();
    const QPointF contentOrigin = LiveAccess::contentGeometry(window).topLeft();
    const QPointF canvasOrigin = pinnedCanvas->mapFrom(&window, QPointF());
    const qreal geometryScale =
        snow_shot::presentation::pinnedGeometryScale(window.devicePixelRatioF());
    const QPointF contentInset = (contentOrigin - nativeOrigin) / geometryScale;
    // Keep the reference bounds exactly representable through the canvas camera;
    // otherwise a round-trip can straddle a rasterizer subpixel boundary.
    const QPointF offset = canvasOrigin + QPointF(qCeil(contentInset.x()), qCeil(contentInset.y()));
    const auto nativePoint = [&](const QPointF& view) {
        return Access::cropNativePoint(window, pinnedCanvas->mapTo(&window, view));
    };
    const std::array<qreal, 6> scales{1.0, 1.25, 1.5, 1.75, 2.0, 3.0};
    for (const QRectF& rect : {QRectF(10, 12, 61, 43), QRectF(10.25, 12.75, 61.5, 43.25),
                               QRectF(10.75, 12.25, 1.25, 43.5)}) {
        for (bool reverse : {false, true}) {
            const QPointF start = offset + (reverse ? rect.bottomRight() : rect.topLeft());
            const QPointF end = offset + (reverse ? rect.topLeft() : rect.bottomRight());
            const QPointF nativeStart = nativePoint(start);
            const QPointF nativeEnd = nativePoint(end);
            pointer(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            pointer(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
            // Canvas moves publish on the next event-loop frame.
            QCoreApplication::processEvents();
            std::array<QImage, 6> background;
            std::array<QImage, 6> expected;
            const QImage snapshot = pinnedCanvas->renderContentToImage(pinnedCanvas->size());
            for (std::size_t i = 0; i < scales.size(); ++i) {
                background[i] = render(*pinnedCanvas, scales[i]);
                expected[i] = background[i];
                QPainter painter(&expected[i]);
                canvas.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
            }
            require(Access::beginCrop(window, nativeStart), "begin canvas crop rendering fixture");
            Access::moveCrop(window, nativeEnd);
            require(pinnedCanvas->renderContentToImage(pinnedCanvas->size()) == snapshot,
                    "crop preview is excluded from canvas content snapshots");
            for (std::size_t i = 0; i < scales.size(); ++i) {
                const QImage actual = render(*pinnedCanvas, scales[i]);
                if (actual != expected[i]) {
                    std::cerr << "Display scale " << geometryScale << ", render scale " << scales[i]
                              << ", logical rect " << rect.x() << ',' << rect.y() << ','
                              << rect.width() << ',' << rect.height() << ", reverse " << reverse
                              << '\n';
                }
                require(actual == expected[i],
                        "crop selection matches the canvas select tool at every pixel and DPI");
            }
            Access::endCrop(window, nativeEnd, true);
            for (std::size_t i = 0; i < scales.size(); ++i)
                require(render(*pinnedCanvas, scales[i]) == background[i],
                        "cancelling crop removes its canvas preview without changing content");
            pointer(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        }
    }
    window.hide();
}

void staticWindow() {
    ScreenshotPinnedWindow window;
    const auto setup = config();
    Access::prepareReplacement(window, setup);
    window.show();
    require(Access::cropAllowed(window), "ordinary pin permits crop selection");
    const auto original = Access::originalImage(window);
    const auto key = original.cacheKey();
    require(Access::beginCrop(window, QPointF(240, 220)), "right crop starts in content");
    Access::endCrop(window, QPointF(300, 260));
    require(window.persistenceSnapshot().cropRect == QRectF(10, 10, 30, 20) &&
                LiveAccess::contentGeometry(window) == QRect(240, 220, 60, 40) &&
                Access::originalImage(window).cacheKey() == key && Access::scale(window) == 200,
            "selection shrinks geometry, preserves source pixels and display scale");
    auto* border = Access::decoration(window);
    require(border->property("showBorder").toBool() && !border->property("showShadow").toBool() &&
                !window.persistenceSnapshot().showBorder && window.persistenceSnapshot().showShadow,
            "crop decorations override rendering without rewriting preferences");
    auto* menu = Access::contextMenu(window);
    auto* apply = window.findChild<QAction*>(QStringLiteral("screenshotPinnedApplyCropAction"));
    auto* process = window.findChild<QAction*>(QStringLiteral("screenshotPinnedProcessImageMenu"));
    require(apply && apply->isEnabled(), "Apply Crop is available for a cropped pin");
    require(process && menu->actions().indexOf(apply) == menu->actions().indexOf(process) + 1,
            "Apply Crop follows Process image");
    const auto saved = exportImage(window, Access::cropSave(window));
    require(saved.size() == QSize(30, 20) && saved.pixelColor(5, 5) == original.pixelColor(15, 15),
            "saving uses original-resolution crop pixels");
    for (const auto& snapshot : {window.splittingSnapshot(), window.stitchingSnapshot()}) {
        const auto output =
            exportImage(window, std::make_shared<ScreenshotExportArtifact>(snapshot.source));
        require(snapshot.outputSize == QSize(30, 20) && output == saved,
                "splitting and stitching use the same visible source region");
    }
    const auto viewport = exportImage(window, Access::viewportExport(window));
    require(viewport.size() == QSize(60, 40),
            "current view retains its existing zoom resolution policy");
    QImage filter;
    window.requestAutoFilterSource([&](QImage image) { filter = std::move(image); });
    translation_tests::waitUntil([&] { return !filter.isNull(); }, "crop filter source ready");
    require(filter == saved && window.autoFilterSourceBounds() == QRectF(110, 210, 30, 20),
            "automatic filters receive visible pixels with the correct canvas origin");
    ScreenshotCanvasColorSampler sampler;
    require(sampler.ensureContentSnapshot(*window.findChild<SnowCanvasWidget*>(),
                                          window.currentNativeGeometry()),
            "crop color sampling snapshot");
    const QPoint samplePoint = Access::cropScreenPoint(window, {125, 220}).toPoint();
    const QImage sampled = sampler.previewAtPhysicalPoint(samplePoint);
    const QColor actual = sampled.pixelColor(3, 3);
    const QColor expected = original.pixelColor(25, 20);
    require(qAbs(actual.red() - expected.red()) <= 2 &&
                qAbs(actual.green() - expected.green()) <= 2,
            "color sampler maps screen coordinates through the cropped camera");
    const auto editor = window.imageEditingSnapshot();
    require(editor.image.size() == QSize(30, 20) && editor.canvasRect == QRectF(110, 210, 30, 20),
            "editor receives cropped base and unchanged document coordinate origin");
    LiveAccess::scale(window, 10);
    require(LiveAccess::contentGeometry(window).size() == QSize(3, 2) &&
                window.persistenceSnapshot().cropRect == QRectF(10, 10, 30, 20),
            "zoom can display less than ten units without changing source selection");
    LiveAccess::scale(window, 200);
    require(Access::beginCropResize(window, QPointF(240, 230), int(Handle::Left)),
            "cropped edge starts controlled resize");
    Access::moveCropResize(window, QPointF(320, 230));
    Access::endCropResize(window);
    require(window.persistenceSnapshot().cropRect == QRectF(40, 10, 10, 20) &&
                LiveAccess::contentGeometry(window) == QRect(300, 220, 20, 40) &&
                Access::scale(window) == 200,
            "edge crosses the opposite edge at fixed source-to-screen scale");
    require(Access::crop(window, QRectF(110, 210, 30, 20)), "restore crop fixture");
    require(!Access::applyAnnotationTransaction(
                 window,
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[115,215,12,12]}]})")
                 .isEmpty(),
            "crop fixture has editable annotations");
    const auto history = Access::drawingHistory(window);
    const auto before = window.persistenceSnapshot();
    Access::applyCrop(window);
    require(
        !window.persistenceSnapshot().cropRect &&
            Access::originalImage(window) == original.copy(QRect(10, 10, 30, 20)) &&
            Access::drawingHistory(window) == history &&
            LiveAccess::contentGeometry(window) == before.nativeGeometry &&
            Access::scale(window) == 200,
        "applying changes only original-resolution base pixels and preserves annotation history");
    require(!border->property("showBorder").toBool() && border->property("showShadow").toBool(),
            "applying restores configured decoration flags");
    window.hide();
}

void gesturesAndAlignment() {
    ScreenshotPinnedWindow window;
    Access::prepareReplacement(window, config());
    window.show();
    Access::editSelectionOffscreen(window, true);
    require(!Access::cropAllowed(window), "annotation mode excludes crop selection");
    Access::editSelectionOffscreen(window, false);
    Access::setRecognitionInteraction(window, true);
    require(!Access::cropAllowed(window), "text mode excludes crop selection");
    Access::setRecognitionInteraction(window, false);
    Access::thumbnailForHideTest(window, true);
    require(!Access::cropAllowed(window), "thumbnail mode excludes crop selection");
    Access::thumbnailForHideTest(window, false);
    require(Access::beginCrop(window, {240, 220}), "begin interrupted crop");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&window, &escape);
    require(!Access::cropSelecting(window) && !window.persistenceSnapshot().cropRect,
            "Escape cancels an unfinished selection");
    require(Access::beginCrop(window, {240, 220}), "begin hidden crop");
    window.hide();
    require(!Access::cropSelecting(window), "hiding cancels an unfinished selection");
    window.show();
    require(Access::beginCrop(window, {240, 220}), "begin small selection");
    Access::endCrop(window, {249, 230});
    auto* menu = Access::contextMenu(window);
    require(!window.persistenceSnapshot().cropRect && menu->isPopupVisible(),
            "subminimum selection opens the existing menu");
    int menus = 0;
    QObject::connect(menu, &QMenu::aboutToShow, &window, [&] { ++menus; });
    QContextMenuEvent duplicate(QContextMenuEvent::Mouse, {20, 20}, {240, 220});
    QApplication::sendEvent(&window, &duplicate);
    require(menus == 0, "the generated mouse context event cannot duplicate the menu");
    menu->hide();
    QContextMenuEvent keyboard(QContextMenuEvent::Keyboard, {20, 20}, {240, 220});
    QApplication::sendEvent(&window, &keyboard);
    menu = Access::contextMenu(window);
    require(menu->isPopupVisible(), "keyboard menu remains available after a crop gesture");
    menu->hide();
    snow_shot::presentation::PinnedWindowSelectionController selection;
    Access::attachSelection(window, selection);
    selection.toggleSelection(&window);
    require(selection.isSelected(&window), "select the target pin");
    require(Access::beginCrop(window, {300, 260}), "reverse crop starts");
    Access::endCrop(window, {240, 220});
    require(window.persistenceSnapshot().cropRect == QRectF(10, 10, 30, 20) &&
                !selection.isSelected(&window),
            "reverse selection crops and deselects the target");
    Access::detachSelection(window);
    const auto original = window.persistenceSnapshot();
    require(Access::beginCropResize(window, {300, 240}, int(Handle::Right)),
            "begin canceled border drag");
    Access::moveCropResize(window, {350, 240});
    Access::endCropResize(window, true);
    require(window.persistenceSnapshot().cropRect == original.cropRect &&
                LiveAccess::contentGeometry(window) == original.nativeGeometry,
            "canceling a border drag restores its crop and geometry");
    LiveAccess::scale(window, 10);
    const QRect tiny = LiveAccess::contentGeometry(window);
    require(Access::beginCropResize(window, tiny.bottomRight(), int(Handle::Right)),
            "resize a crop zoomed below the minimum");
    Access::moveCropResize(window, tiny.bottomRight());
    Access::endCropResize(window);
    require(LiveAccess::contentGeometry(window).size() == QSize(10, 8),
            "adjustment repairs both axes using the full displayed source limit");
    LiveAccess::scale(window, 137);
    const QPointF landmark(150, 240);
    const auto before = Access::cropScreenPoint(window, landmark);
    const auto zoom = Access::viewportZoom(window);
    require(Access::crop(window, {120.25, 210.75, 45.5, 30.25}), "fractional crop");
    const auto after = Access::cropScreenPoint(window, landmark);
    require(qAbs(before.x() - after.x()) <= .51 && qAbs(before.y() - after.y()) <= .51 &&
                qAbs(Access::viewportZoom(window) - zoom) < 1e-9,
            "fractional crop preserves scale and screen landmarks within window rounding");
    const auto cropped = window.persistenceSnapshot().cropRect;
    Access::thumbnailForHideTest(window, true);
    Access::thumbnailForHideTest(window, false);
    require(window.persistenceSnapshot().cropRect == cropped, "thumbnail transitions retain crop");
    Access::cropActive(window, true);
    auto* border = Access::decoration(window);
    const QColor inactive = border->property("borderColor").value<QColor>();
    const auto activeMarker = border->property("cropMarkerColor").value<QColor>();
    require(activeMarker.isValid(), "active crop markers receive the active color");
    Access::cropActive(window, false);
    require(border->property("borderColor").value<QColor>() == inactive &&
                !border->property("cropMarkerColor").value<QColor>().isValid(),
            "deactivation changes markers without changing the crop border");
    LiveAccess::lock(window, true);
    require(border->property("cropMarkerColor").value<QColor>().isValid(),
            "locked markers use the configured lock color");
    window.hide();
}

void inlineAnnotations() {
    ScreenshotPinnedWindow window;
    Access::prepareReplacement(window, config());
    window.show();
    require(Access::crop(window, {110, 210, 60, 50}), "inline annotation crop fixture");
    Access::editSelectionOffscreen(window, true);
    auto* canvas = window.findChild<SnowCanvasWidget*>();
    auto* controller = window.findChild<ScreenshotPinnedEditController*>();
    require(controller && controller->toolbarWindow()->palette()->activateDrawingShortcut(
                              QStringLiteral("text")),
            "activate cropped text annotation");
    waitForUi(30);
    const auto bounds = canvas->textEditingBounds();
    require(bounds && *bounds == Access::visibleCrop(window),
            "inline text wrapping uses the visible crop in document coordinates");
    SnowCanvasTextStyle style;
    style.fontSize = 10;
    require(canvas->setCanvasTextStyle(style), "crop text style");
    const QPointF position = canvas->canvasToViewTransform().map(bounds->topLeft() + QPointF(5, 5));
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonPress, position, Qt::LeftButton,
                                       Qt::LeftButton);
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonRelease, position,
                                       Qt::LeftButton, Qt::NoButton);
    require(canvas->hasActiveTextEditing(), "begin editing text inside the crop");
    QInputMethodEvent input;
    input.setCommitString(QStringLiteral("Crop text"));
    QApplication::sendEvent(canvas, &input);
    QCoreApplication::processEvents();
    require(canvas->hasActiveTextEditing(), "inline editing remains active after event delivery");
    const auto history = Access::drawingHistory(window);
    Access::applyCrop(window);
    require(canvas->hasActiveTextEditing(), "applying crop preserves the active inline editor");
    require(Access::drawingHistory(window) == history,
            "applying crop preserves editable annotation history");
    require(canvas->resetEditingStatePreservingTool(), "commit crop text annotation");
    require(!window.imageEditingSnapshot().canvasSession.isEmpty(),
            "editor import retains the separate annotation document");
    window.hide();
}

void textAndRecognition() {
    ScreenshotPinnedWindow text;
    auto setup = config();
    setup.originalClipboardContent.text = QStringLiteral("Original text payload");
    setup.originalClipboardContent.html = QStringLiteral("<b>Original text payload</b>");
    setup.formattedTextDocument = std::make_shared<QTextDocument>();
    setup.formattedTextDocument->setHtml(setup.originalClipboardContent.html);
    setup.formattedPlainText = setup.originalClipboardContent.text;
    text.setAttribute(Qt::WA_DeleteOnClose, false);
    require(text.present(setup), "present text crop fixture");
    translation_tests::waitUntil([&] { return Access::shakeContentReady(text); }, "text ready");
    require(Access::crop(text, {110, 210, 30, 20}), "crop a text raster");
    require(text.automationOriginalContent().text == setup.originalClipboardContent.text,
            "temporary crop retains original text and HTML");
    const QImage original = Access::originalImage(text);
    Access::applyCrop(text);
    require(text.automationOriginalContent().isEmpty() &&
                text.persistenceSnapshot().sourceKind ==
                    snow_shot::storage::PinnedWindowSourceKind::ImageData &&
                Access::originalImage(text) == original.copy(QRect(10, 10, 30, 20)),
            "applying text crop produces an ordinary original-resolution image");
    require(Access::crop(text, {115, 215, 10, 10}), "repeat crop on new base");
    Access::applyCrop(text);
    require(Access::originalImage(text) == original.copy(QRect(15, 15, 10, 10)),
            "repeated applied crop composes pixel offsets");

    ScreenshotPinnedWindow recognized;
    auto recognitionConfig = cachedOcrPinConfig(nullptr);
    auto* session = Access::hiddenSelectionOffscreen(recognized, recognitionConfig);
    const auto full = recognized.recognitionSnapshot();
    require(Access::crop(recognized, {700, 420, 100, 70}), "crop recognized content");
    require(recognized.recognitionSnapshot().text->presentation->lines.size() ==
                full.text->presentation->lines.size(),
            "temporary crop retains complete recognition results");
    session->activate(ScreenshotRecognitionSessionController::Mode::Text);
    Access::applyCrop(recognized);
    const auto applied = recognized.recognitionSnapshot();
    require(applied.text &&
                applied.text->presentation->lines.size() == full.text->presentation->lines.size() &&
                applied.text->presentation->lines.first().quad ==
                    full.text->presentation->lines.first().quad &&
                Access::recognition(recognized)->active(),
            "applied crop keeps full recognition geometry and active text presentation");
    recognized.hide();
}

void styledAndTransformedApply() {
    for (const bool styled : {false, true}) {
        ScreenshotPinnedWindow window;
        auto setup = config();
        if (styled) {
            setup.resultStyle.cornerRadius = 12;
            setup.resultStyle.shadowWidth = 6;
            setup.contentCanvasRect = setup.canvasSourceRect;
            setup.surfaceCanvasRect = setup.canvasSourceRect.adjusted(-6, -6, 6, 6);
            setup.initialWindowSize = setup.surfaceCanvasRect.size().toSize();
            setup.nativeGeometry.setSize(setup.initialWindowSize * 2);
        }
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        require(window.present(setup), "present styled crop fixture");
        translation_tests::waitUntil([&] { return Access::shakeContentReady(window); },
                                     "styled crop ready");
        LiveAccess::command(window, QStringLiteral("rotate_clockwise"));
        const auto full = window.persistenceSnapshot().contentCanvasRect;
        const QRectF crop(full.x(), full.y(), 30, 40);
        require(Access::crop(window, crop), "crop transformed styled image");
        const auto expected = exportImage(window, Access::cropSave(window));
        const auto editor = window.imageEditingSnapshot();
        require(!editor.resultStyle && editor.image == expected && editor.canvasRect == crop,
                "editor crop bakes only base styling and preserves the document coordinate frame");
        const auto landmark = Access::cropScreenPoint(window, crop.center());
        const auto geometry = LiveAccess::contentGeometry(window);
        Access::applyCrop(window);
        require(
            exportImage(window, Access::cropSave(window)) == expected &&
                LiveAccess::contentGeometry(window) == geometry &&
                QLineF(landmark, Access::cropScreenPoint(window, crop.center())).length() < .01,
            "applying rotation and styling preserves visible pixels and source-to-screen mapping");
        window.hide();
    }
}

void staleApplyAndRestore() {
    ScreenshotPinnedWindow window;
    Access::prepareReplacement(window, config());
    require(Access::crop(window, {110, 210, 30, 20}), "stale crop fixture");
    const auto original = Access::originalImage(window);
    Access::startApplyCrop(window);
    Access::changeBatchIdentity(window);
    translation_tests::waitUntil([&] { return !Access::cropApplying(window); },
                                 "stale crop finishes");
    require(window.persistenceSnapshot().cropRect && Access::originalImage(window) == original,
            "stale preparation leaves both source and viewport unchanged");
    LiveAccess::scale(window, 137);
    require(Access::crop(window, {111.2, 210.75, 24.3, 19.25}), "fractional restore fixture");
    const auto record = window.persistenceSnapshot();
    auto restoredConfig = config();
    restoredConfig.restorePersistentState = true;
    restoredConfig.persistedCropRect = record.cropRect;
    restoredConfig.persistedScalePercent = record.scalePercent;
    restoredConfig.nativeGeometry = record.nativeGeometry;
    ScreenshotPinnedWindow restored;
    restored.setAttribute(Qt::WA_DeleteOnClose, false);
    require(restored.present(restoredConfig), "present restored crop");
    translation_tests::waitUntil([&] { return Access::shakeContentReady(restored); },
                                 "restored crop ready");
    require(restored.persistenceSnapshot().cropRect == record.cropRect &&
                Access::scale(restored) == 137 &&
                Access::visibleCrop(restored) == Access::visibleCrop(window),
            "restore installs crop before deriving the initial camera and scale");
    for (const QRectF invalid : {QRectF(-1, 0, 10, 10), QRectF(0, 0, 200, 100), QRectF()}) {
        restoredConfig.persistedCropRect = invalid;
        ScreenshotPinnedWindow legacy;
        Access::prepareReplacement(legacy, restoredConfig);
        require(!legacy.persistenceSnapshot().cropRect, "invalid persisted crop falls back safely");
    }
}

void liveWindow() {
    auto source = std::make_shared<live_pinned_tests::FakeSource>();
    ScreenshotPinnedWindow window;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    require(window.present(live_pinned_tests::config(source)), "present live crop fixture");
    translation_tests::waitUntil([&] { return window.liveFrameAvailable(); },
                                 "live crop frame ready");
    require(Access::crop(window, QRectF(20, 10, 60, 40)), "crop live frame");
    translation_tests::waitUntil(
        [&] {
            return !source->presentations.isEmpty() &&
                   source->presentations.last().sourceViewport == QRectF(.125, .125, .375, .5);
        },
        "native presentation receives normalized crop bounds");
    source->setState(live_pinned_tests::State::Minimized);
    require(!window.liveFrameAvailable() && !Access::cropAllowed(window),
            "unavailable live content prevents starting a new crop");
    source->setState(live_pinned_tests::State::Live);
    source->publish(Qt::green);
    translation_tests::waitUntil(
        [&] { return LiveAccess::original(window).pixelColor(0, 0) == QColor(Qt::green); },
        "cropped live frame continues refreshing");
    require(LiveAccess::original(window).size() == QSize(80, 40),
            "temporary live crop retains full source frame");
    const auto visible = Access::visibleCrop(window);
    source->frame = QImage(160, 80, QImage::Format_ARGB32_Premultiplied);
    source->publish(Qt::green);
    require(Access::visibleCrop(window) == visible,
            "live backing-size changes preserve the visible source region and display scale");
    source->frame = QImage(80, 40, QImage::Format_ARGB32_Premultiplied);
    source->publish(Qt::green);
    Access::applyCrop(window);
    source->publish(Qt::blue);
    translation_tests::waitUntil(
        [&] { return LiveAccess::original(window).pixelColor(0, 0) == QColor(Qt::blue); },
        "applied live crop continues refreshing");
    require(!window.persistenceSnapshot().cropRect &&
                LiveAccess::original(window).size() == QSize(30, 20) && window.isLiveWindow(),
            "applying live crop restricts the refreshed base without converting the source");
    const auto requests = source->requests;
    source->deferFrames = true;
    LiveAccess::command(window, QStringLiteral("rotate_clockwise"));
    require(LiveAccess::original(window).size() == QSize(30, 20) &&
                Access::renderer(window).imageSourceSnapshot().materializedImage.cacheKey() ==
                    LiveAccess::original(window).cacheKey() &&
                LiveAccess::transformed(window).isNull() && source->requests == requests,
            "applied live crop rotates immediately without cropping twice or recapturing");
    window.closeLiveWindow();
}

std::shared_ptr<live_pinned_tests::FakeSource> patternedLiveSource() {
    auto source = std::make_shared<live_pinned_tests::FakeSource>();
    for (int y = 0; y < source->frame.height(); ++y)
        for (int x = 0; x < source->frame.width(); ++x)
            source->frame.setPixelColor(x, y, QColor(x * 3, y * 5, 80));
    return source;
}

void liveCropPaintsTransforms() {
    for (const int cropState : {0, 1, 2}) {
        for (int turns = 0; turns < 4; ++turns) {
            for (const bool reflected : {false, true}) {
                auto source = patternedLiveSource();
                ScreenshotPinnedWindow window;
                live_pinned_tests::present(window, live_pinned_tests::config(source));
                require(Access::crop(window, {20, 10, 60, 40}), "live rendering crop fixture");
                if (cropState != 0)
                    Access::applyCrop(window);
                if (cropState == 2)
                    require(Access::crop(window, {30, 20, 30, 20}),
                            "live rendering crop inside an applied crop");
                for (int turn = 0; turn < turns; ++turn)
                    LiveAccess::command(window, QStringLiteral("rotate_clockwise"));
                if (reflected)
                    LiveAccess::command(window, QStringLiteral("flip_horizontal"));

                const auto verify = [&] {
                    const auto& original = Access::originalImage(window);
                    require(Access::renderer(window)
                                    .imageSourceSnapshot()
                                    .materializedImage.cacheKey() == original.cacheKey(),
                            "cropped live rendering samples the installed source pixels");
                    require(LiveAccess::transform(window).isIdentity() ||
                                LiveAccess::transformed(window).isNull(),
                            "cropping preserves direct live transform rendering");
                    const QImage expected =
                        exportImage(window, LiveAccess::artifact(window, false));
                    // Decorations use physical pixels. Capture at the widget's backing
                    // scale so their native clip agrees with the sampled content rectangle.
                    const qreal dpr = window.devicePixelRatioF();
                    const QSize raster(qCeil(window.width() * dpr), qCeil(window.height() * dpr));
                    QImage actual(raster, QImage::Format_ARGB32_Premultiplied);
                    actual.setDevicePixelRatio(dpr);
                    actual.fill(Qt::transparent);
                    window.render(&actual);
                    const auto content = LiveAccess::contentRect(window);
                    QImage reference(actual.size(), actual.format());
                    reference.setDevicePixelRatio(dpr);
                    reference.fill(Qt::transparent);
                    {
                        QPainter painter(&reference);
                        painter.setRenderHint(QPainter::SmoothPixmapTransform);
                        painter.drawImage(content, expected);
                    }
                    for (const qreal x : {.25, .75}) {
                        for (const qreal y : {.25, .75}) {
                            const QPoint sample(qFloor((content.x() + x * content.width()) * dpr),
                                                qFloor((content.y() + y * content.height()) * dpr));
                            const auto message =
                                QStringLiteral(
                                    "cropped live display matches the oriented viewport "
                                    "export (crop=%1, turns=%2, reflected=%3, "
                                    "source=%4x%5, sample=%6,%7, actual=%8, expected=%9)")
                                    .arg(cropState)
                                    .arg(turns)
                                    .arg(reflected)
                                    .arg(original.width())
                                    .arg(original.height())
                                    .arg(sample.x())
                                    .arg(sample.y())
                                    .arg(actual.pixelColor(sample).name(QColor::HexArgb))
                                    .arg(reference.pixelColor(sample).name(QColor::HexArgb))
                                    .toStdString();
                            requireColorNear(actual.pixelColor(sample),
                                             reference.pixelColor(sample), 6, message.c_str());
                        }
                    }
                };
                verify();
                if (cropState != 1) {
                    Access::applyCrop(window);
                    verify();
                }
                source->frame = source->frame.scaled(source->frame.size() * 2);
                emit source->frameChanged(source->frame);
                verify();
                window.closeLiveWindow();
            }
        }
    }
}

void liveConversion() {
    for (const int cropState : {0, 1, 2}) {
        for (const bool transformed : {false, true}) {
            auto source = patternedLiveSource();
            std::optional<snow_shot::storage::PinnedWindowRecord> persisted;
            ScreenshotPinnedWindow window;
            auto setup = live_pinned_tests::config(source);
            setup.initialShadowVisible = true;
            setup.persistenceWriter = [&](const auto& record) { persisted = record; };
            live_pinned_tests::present(window, setup);
            require(Access::crop(window, {20, 10, 60, 40}), "live conversion crop fixture");
            if (cropState != 0)
                Access::applyCrop(window);
            if (cropState == 2)
                require(Access::crop(window, {30, 20, 30, 20}),
                        "temporary crop inside a committed live crop");
            if (transformed) {
                LiveAccess::command(window, QStringLiteral("rotate_clockwise"));
                LiveAccess::command(window, QStringLiteral("flip_horizontal"));
            }
            LiveAccess::scale(window, 137);
            const QRect geometry = LiveAccess::contentGeometry(window);
            const QImage expected = exportImage(window, LiveAccess::artifact(window, false));
            require(expected.pixelColor(1, 1) !=
                        expected.pixelColor(expected.width() - 2, expected.height() - 2),
                    "conversion fixture distinguishes source positions and orientation");
            require(!persisted, "live crop changes do not persist a static image");
            LiveAccess::convert(window);
            translation_tests::waitUntil([&] { return !window.isLiveWindow(); },
                                         "cropped live window converts");

            const auto record = window.persistenceSnapshot();
            const QRectF canvas(QPointF(), QSizeF(geometry.size()));
            require(!record.cropRect && record.imageTransform.isIdentity() &&
                        record.quarterTurns == 0 && Access::originalImage(window) == expected &&
                        exportImage(window, Access::cropSave(window)) == expected,
                    "conversion bakes crop and orientation exactly once into the new source");
            require(LiveAccess::contentGeometry(window) == geometry &&
                        record.canvasSourceRect == canvas && record.contentCanvasRect == canvas &&
                        record.surfaceCanvasRect == canvas &&
                        Access::visibleCrop(window) == canvas && record.scalePercent == 100,
                    "converted snapshot owns a new canvas and a one-hundred-percent baseline");
            const auto editor = window.imageEditingSnapshot();
            require(editor.image == expected && editor.canvasRect == canvas,
                    "editing receives the complete converted snapshot");
            auto* border = Access::decoration(window);
            require(!border->property("cropped").toBool() &&
                        !border->property("showBorder").toBool() &&
                        border->property("showShadow").toBool(),
                    "conversion clears crop decorations and restores configured appearance");
            require(persisted && !persisted->cropRect && persisted->image == expected &&
                        persisted->canvasSourceRect == canvas &&
                        persisted->initialWindowSize == geometry.size() &&
                        persisted->scalePercent == 100,
                    "the first persisted image contains the normalized conversion state");
            LiveAccess::scale(window, 100);
            require(LiveAccess::contentGeometry(window) == geometry,
                    "one-hundred-percent zoom preserves converted geometry");
            LiveAccess::scale(window, 200);
            require(LiveAccess::contentGeometry(window).size() == geometry.size() * 2,
                    "zoom uses the converted extent instead of an applied crop's old baseline");
            LiveAccess::scale(window, 100);
            source->publish(Qt::magenta);
            QCoreApplication::processEvents();
            require(source->shutdowns == 1 && Access::originalImage(window) == expected &&
                        exportImage(window, Access::cropSave(window)) == expected,
                    "source updates and zoom cannot alter the converted snapshot");
            window.hide();
        }
    }
}

void liveConversionRejectsChangedCrop() {
    auto source = patternedLiveSource();
    ScreenshotPinnedWindow window;
    live_pinned_tests::present(window, live_pinned_tests::config(source));
    require(Access::crop(window, {20, 10, 60, 40}), "deferred conversion crop fixture");
    const QRect initialGeometry = LiveAccess::contentGeometry(window);
    source->deferFrames = true;
    LiveAccess::convert(window);
    require(LiveAccess::snapshotPending(window) && source->pending.size() == 1,
            "conversion waits for its captured frame");
    const QPointF press = initialGeometry.topLeft() + QPoint(0, 20);
    require(Access::beginCropResize(window, press, int(Handle::Left)),
            "resize a crop while its conversion is pending");
    Access::moveCropResize(window, press + QPointF(120, 0));
    Access::endCropResize(window);
    require(LiveAccess::contentGeometry(window).size() == initialGeometry.size() &&
                Access::visibleCrop(window) == QRectF(80, 10, 60, 40),
            "crossing the opposite edge changes the source viewport without changing its size");
    source->deliver();
    translation_tests::waitUntil([&] { return !LiveAccess::snapshotPending(window); },
                                 "obsolete crop conversion finishes");
    require(window.isLiveWindow() && source->shutdowns == 0 &&
                Access::visibleCrop(window) == QRectF(80, 10, 60, 40),
            "conversion cannot install pixels from a superseded source viewport");
    source->deferFrames = false;
    const QImage expected = exportImage(window, LiveAccess::artifact(window, false));
    LiveAccess::convert(window);
    translation_tests::waitUntil([&] { return !window.isLiveWindow(); },
                                 "conversion can retry using the current crop");
    require(exportImage(window, Access::cropSave(window)) == expected,
            "conversion retry installs the current source region exactly once");
    window.hide();
}

void liveConversionEndsCropInteraction() {
    for (const bool resizing : {false, true}) {
        auto source = patternedLiveSource();
        ScreenshotPinnedWindow window;
        live_pinned_tests::present(window, live_pinned_tests::config(source));
        require(Access::crop(window, {20, 10, 60, 40}), "conversion interaction crop fixture");
        const QImage expected = exportImage(window, LiveAccess::artifact(window, false));
        source->deferFrames = true;
        LiveAccess::convert(window);
        const QRect geometry = LiveAccess::contentGeometry(window);
        if (resizing)
            require(Access::beginCropResize(window, geometry.topLeft(), int(Handle::Left)),
                    "begin crop edge interaction before conversion completes");
        else
            require(Access::beginCrop(window, geometry.center()),
                    "begin crop selection before conversion completes");
        source->deliver();
        translation_tests::waitUntil([&] { return !window.isLiveWindow(); },
                                     "conversion ends the obsolete crop interaction");
        require(!Access::cropSelecting(window) && !Access::interactionActive(window) &&
                    !window.persistenceSnapshot().cropRect &&
                    LiveAccess::contentGeometry(window) == geometry &&
                    exportImage(window, Access::cropSave(window)) == expected,
                "conversion releases crop interaction ownership without changing the snapshot");
        window.hide();
    }
}

void liveConversionDiscardsPendingCrop() {
    auto source = patternedLiveSource();
    ScreenshotPinnedWindow window;
    live_pinned_tests::present(window, live_pinned_tests::config(source));
    require(Access::crop(window, {20, 10, 60, 40}), "pending apply conversion fixture");
    const QImage expected = exportImage(window, LiveAccess::artifact(window, false));
    auto* pool = QThreadPool::globalInstance();
    const int workers = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    const auto started = std::make_shared<QSemaphore>();
    const auto release = std::make_shared<QSemaphore>();
    const auto restoreWorkers = qScopeGuard([&] {
        release->release();
        pool->setMaxThreadCount(workers);
    });
    pool->start([started, release] {
        started->release();
        release->acquire();
    });
    require(started->tryAcquire(1, 5000), "hold crop preparation until after conversion");
    Access::startApplyCrop(window);
    require(Access::cropApplying(window), "crop preparation remains pending");
    LiveAccess::convert(window);
    translation_tests::waitUntil([&] { return !window.isLiveWindow(); },
                                 "conversion installs its independently prepared snapshot");
    release->release();
    translation_tests::waitUntil([&] { return !Access::cropApplying(window); },
                                 "obsolete crop preparation retires");
    require(!window.persistenceSnapshot().cropRect && Access::originalImage(window) == expected &&
                exportImage(window, Access::cropSave(window)) == expected,
            "pending crop work cannot overwrite the converted source");
    window.hide();
}

void restorationAndTransforms() {
    QTemporaryDir directory;
    require(directory.isValid(), "crop repository fixture directory");
    snow_shot::storage::PinnedWindowRepository repository(directory.path(), true, 0);
    ScreenshotPinnedWindow window;
    auto setup = config();
    Access::prepareReplacement(window, setup);
    require(Access::crop(window, QRectF(110, 210, 30, 20)), "persisted crop fixture");
    const auto sourceRegion = window.persistenceSnapshot().cropRect;
    for (const auto& operation :
         {QStringLiteral("rotate_clockwise"), QStringLiteral("flip_horizontal"),
          QStringLiteral("flip_vertical")}) {
        LiveAccess::command(window, operation);
        require(window.persistenceSnapshot().cropRect == sourceRegion &&
                    Access::scale(window) == 200,
                "image operations preserve the canonical source crop and scale");
    }
    require(LiveAccess::contentGeometry(window).size() == QSize(40, 60),
            "quarter turn transposes the cropped window");
    const auto record = window.persistenceSnapshot();
    require(repository.upsert(record).success && repository.flush().success,
            "save cropped pin payload and metadata");
    const auto restored = repository.loadRecord(record.id);
    require(restored.has_value(), "cropped repository record reloads");
    require(restored->cropRect == record.cropRect, "repository retains crop rectangle");
    require(restored->image.size() == record.image.size(),
            "repository retains full source dimensions");
    for (int y = 0; y < record.image.height(); ++y)
        for (int x = 0; x < record.image.width(); ++x)
            require(restored->image.pixelColor(x, y) == record.image.pixelColor(x, y),
                    "repository retains full source pixels");
    require(restored->showBorder == record.showBorder && restored->showShadow == record.showShadow,
            "repository retains independent appearance");
    LiveAccess::command(window, QStringLiteral("reset_transform"));
    require(Access::visibleCrop(window) == QRectF(110, 210, 30, 20),
            "reset transform restores crop orientation");
    LiveAccess::lock(window, true);
    require(!Access::cropAllowed(window), "lock forbids crop selection");
    const auto locked = window.persistenceSnapshot();
    Access::applyCrop(window);
    require(window.persistenceSnapshot().cropRect == locked.cropRect,
            "lock forbids committing crop");
    LiveAccess::lock(window, false);
    Access::applyCrop(window);
    require(repository.upsert(window.persistenceSnapshot()).success && repository.flush().success,
            "save applied base replacement");
    const auto applied = repository.loadRecord(record.id);
    require(applied && !applied->cropRect && applied->image.size() == QSize(30, 20) &&
                applied->exactInitialWindowSize == QSizeF(30, 20),
            "applied crop persists the new original and exact scale baseline");
}

void animations() {
    for (const auto& fixture : pinned_animation_fixtures::images()) {
        const auto animation = snow_shot::image_codec::decodeAnimationSource(fixture.bytes);
        require(animation && animation->isAnimated(), "animated crop fixture decodes");
        ScreenshotPinnedWindow window;
        auto setup = config();
        setup.canvasSourceRect = QRectF(0, 0, 40, 40);
        setup.initialWindowSize = QSize(40, 40);
        setup.nativeGeometry = QRect(100, 100, 40, 40);
        setup.imageSource =
            ScreenshotImageSource::fromImage(animation->firstFrame, setup.canvasSourceRect);
        setup.animationSource = animation;
        setup.enableEditing = false;
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        require(window.present(setup), "present animated crop fixture");
        auto* canvas = window.findChild<SnowCanvasWidget*>();
        translation_tests::waitUntil(
            [&] {
                static_cast<void>(renderWidget(*canvas));
                return Access::shakeContentReady(window);
            },
            "animation crop initial frame paints");
        Access::playback(window).pause();
        const int frame = Access::playback(window).frameIndex();
        const int speed = Access::playback(window).speedPercent();
        require(Access::crop(window, QRectF(0, 0, 20, 40)), "crop animated frame viewport");
        Access::applyCrop(window);
        static_cast<void>(renderWidget(*canvas));
        const auto record = window.persistenceSnapshot();
        require(!record.cropRect && record.animationSource &&
                    record.animationSource->canvasSize == QSize(1, 2) &&
                    record.animationSource->encoded == animation->encoded &&
                    record.animationCropRect == QRect(0, 0, 1, 2) &&
                    Access::playback(window).frameIndex() == frame &&
                    Access::playback(window).speedPercent() == speed &&
                    !Access::playback(window).requestedPlaying(),
                "applied animation keeps encoded source, frame position, speed and pause state");
        Access::playback(window).nextFrame();
        translation_tests::waitUntil(
            [&] {
                static_cast<void>(renderWidget(*canvas));
                return Access::playback(window).frameIndex() != frame;
            },
            "cropped animation can advance");
        require(Access::originalImage(window).size() == QSize(1, 2),
                "new animation frames use committed source crop");
        QTemporaryDir directory;
        snow_shot::storage::PinnedWindowRepository repository(directory.path(), true, 0);
        auto persisted = window.persistenceSnapshot();
        persisted.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        require(repository.upsert(persisted).success && repository.flush().success,
                "persist applied animation crop");
        const auto loaded = repository.loadRecord(persisted.id);
        require(loaded && loaded->animationSource &&
                    loaded->animationSource->sourceCropRect == QRect(0, 0, 1, 2) &&
                    loaded->image.size() == QSize(1, 2),
                "animation persistence restores cropped preview and decoder region");
        auto decoder = snow_shot::image_codec::openAnimationDecoder(loaded->animationSource);
        QString error;
        require(decoder && decoder->advance(&error) &&
                    decoder->snapshot(&error).size() == QSize(1, 2),
                "restored animation decodes cropped frames at source resolution");
        require(Access::crop(window, {0, 20, 20, 20}), "repeat animated crop");
        Access::applyCrop(window);
        require(window.persistenceSnapshot().animationCropRect == QRect(0, 1, 1, 1) &&
                    Access::originalImage(window).size() == QSize(1, 1),
                "repeated animation crops compose descriptors without re-encoding");
        window.hide();
    }
}
#ifdef Q_OS_WIN
void nativeInput() {
    ScreenshotPinnedWindow window;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    require(window.present(config()), "native crop fixture");
    translation_tests::waitUntil([&] { return Access::shakeContentReady(window); },
                                 "native crop ready");
    const QPoint origin = LiveAccess::contentGeometry(window).topLeft();
    const QPoint first = origin + QPoint(20, 20);
    const QPoint last = first + QPoint(40, 30);
    require(Access::cropNativeInput(window, WM_NCRBUTTONDOWN, HTCAPTION,
                                    MAKELPARAM(first.x(), first.y())),
            "native crop press consumed");
    require(Access::cropSelecting(window), "native caption begins the same crop controller");
    require(
        Access::cropNativeInput(window, WM_NCRBUTTONUP, HTCAPTION, MAKELPARAM(last.x(), last.y())),
        "native crop release consumed");
    require(window.persistenceSnapshot().cropRect &&
                LiveAccess::contentGeometry(window) == QRect(first, QSize(40, 30)),
            "native selection uses physical pixels");
    const auto crop = window.persistenceSnapshot().cropRect;
    require(Access::cropNativeInput(window, WM_NCRBUTTONDOWN, HTCAPTION,
                                    MAKELPARAM(last.x() - 5, last.y() - 5)),
            "native interrupted press");
    Access::cropNativeInput(window, WM_CANCELMODE, 0, 0);
    require(!Access::cropSelecting(window) && window.persistenceSnapshot().cropRect == crop,
            "native cancellation preserves the preceding viewport");
    window.hide();
}
#elif defined(Q_OS_MACOS)
void nativeInput() {
    ScreenshotPinnedWindow window;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    require(window.present(config()), "native macOS crop fixture");
    translation_tests::waitUntil([&] { return Access::shakeContentReady(window); },
                                 "native crop ready");
    const QRect original = LiveAccess::contentGeometry(window);
    const auto landmark = Access::cropScreenPoint(window, {125, 220});
    auto* canvas = window.findChild<SnowCanvasWidget*>();
    const auto first = canvas->canvasToViewTransform().map(QPointF(110, 210));
    const auto last = canvas->canvasToViewTransform().map(QPointF(140, 230));
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonPress, first, Qt::RightButton,
                                       Qt::RightButton);
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseMove, last, Qt::NoButton,
                                       Qt::RightButton);
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonRelease, last, Qt::RightButton,
                                       Qt::NoButton);
    require(LiveAccess::contentGeometry(window) ==
                    QRect(original.topLeft() + QPoint(20, 20), QSize(60, 40)) &&
                QLineF(landmark, Access::cropScreenPoint(window, {125, 220})).length() < .01,
            "native macOS crop uses logical units and preserves exact screen alignment");
    Access::applyCrop(window);
    require(!window.persistenceSnapshot().cropRect &&
                Access::originalImage(window).size() == QSize(30, 20),
            "native macOS apply retains source resolution");
    window.hide();
}
#endif
} // namespace pinned_crop_tests

void livePinnedCropTests() {
    pinned_crop_tests::liveWindow();
    pinned_crop_tests::liveCropPaintsTransforms();
}

void pinnedCropTests() {
    pinned_crop_tests::geometry();
    pinned_crop_tests::cornerOutlines();
    pinned_crop_tests::cropSelectionProjectsNativeBounds();
    pinned_crop_tests::cropSelectionMatchesCanvas();
    pinned_crop_tests::staticWindow();
    pinned_crop_tests::restorationAndTransforms();
    pinned_crop_tests::gesturesAndAlignment();
    pinned_crop_tests::inlineAnnotations();
    pinned_crop_tests::textAndRecognition();
    pinned_crop_tests::staleApplyAndRestore();
    pinned_crop_tests::styledAndTransformedApply();
    pinned_crop_tests::animations();
    livePinnedCropTests();
    pinned_crop_tests::liveConversion();
    pinned_crop_tests::liveConversionRejectsChangedCrop();
    pinned_crop_tests::liveConversionEndsCropInteraction();
    pinned_crop_tests::liveConversionDiscardsPendingCrop();
}
