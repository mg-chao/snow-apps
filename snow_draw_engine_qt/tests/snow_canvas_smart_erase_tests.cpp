#include "snow_canvas_smart_erase.h"
#include "snow_canvas_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_mat_allocator.h"
#include "../../test-support/virtualmemory.h"
#include <opencv2/imgproc.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QMouseEvent>
#include <QThread>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
template <class Predicate> void until(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), "asynchronous operation did not complete");
}
SnowCanvasSceneItem rectangle() {
    SnowSceneDisplayItem item{};
    item.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    item.element_id = {1, 1};
    item.center_x = 32;
    item.center_y = 32;
    item.width = 12;
    item.height = 12;
    item.opacity = 1;
    item.filter = snow_filter_render_spec_resolve(5, 0.9);
    return SnowCanvasSceneItem(item);
}

void reconstructionBuffersReturnMappedPages() {
    using namespace snow_canvas_smart_erase;
    using snow::test_support::virtualMemoryMapped;
    auto source = pixelMatrix<cv::Vec3f>(cv::Size(1024, 1024), cv::Vec3f(0.2F, 0.4F, 0.6F));
    const auto* originalMiddle = source.data + source.total() * source.elemSize() / 2;
    auto converted = pixelMatrix<cv::Vec3f>();
    cv::cvtColor(source, converted, cv::COLOR_RGB2Lab);
    require(converted.u != nullptr && converted.u->currAllocator == &pixelMatAllocator(),
            "OpenCV output creation must use the local pixel allocator");
    auto copied = pixelMatrixCopy(converted);
    require(copied(0, 0) == converted(0, 0) && copied.data != converted.data,
            "matrix copies must preserve pixels with independent ownership");
    const auto* convertedMiddle = converted.data + converted.total() * converted.elemSize() / 2;
    const auto* copiedMiddle = copied.data + copied.total() * copied.elemSize() / 2;
    auto roi = source(cv::Rect(1, 1, 100, 100));
    source.release();
    require(virtualMemoryMapped(originalMiddle), "matrix ROI must retain its original allocation");
    roi.release();
    require(!virtualMemoryMapped(originalMiddle), "final matrix ROI release must unmap its pixels");
    converted.release();
    require(!virtualMemoryMapped(convertedMiddle),
            "conversion matrix release must unmap its pixels");
    copied.release();
    require(!virtualMemoryMapped(copiedMiddle), "matrix clone release must unmap its pixels");

    QImage background(QSize(1024, 1024), QImage::Format_ARGB32);
    background.fill(Qt::green);
    {
        QPainter painter(&background);
        painter.fillRect(QRect(162, 162, 700, 700), Qt::blue);
    }
    auto item = rectangle();
    item.center_x = item.center_y = 512;
    item.width = item.height = 700;
    item.rebuildLocalRectangleGeometry();
    std::atomic_bool cancelled{false};
    auto result = reconstruct(item, {{background, QRectF(0, 0, 1024, 1024), {}}}, cancelled);
    require(result.success && result.original.size() == QSize(700, 700) &&
                result.filled.size() == result.original.size(),
            "reconstruction must publish only the affected before and after crops");
    require(result.original.pixelColor(350, 350) == QColor(Qt::blue) &&
                result.filled.pixelColor(350, 350) == QColor(Qt::green),
            "mapped reconstruction must preserve original pixels and restore the surface");
    const auto* beforeMiddle = result.original.constBits() + result.original.sizeInBytes() / 2;
    const auto* afterMiddle = result.filled.constBits() + result.filled.sizeInBytes() / 2;
    QImage retained = result.filled;
    result = {};
    require(!virtualMemoryMapped(beforeMiddle), "released original crop must unmap immediately");
    require(virtualMemoryMapped(afterMiddle), "export sharing must retain the reconstructed crop");
    retained = {};
    require(!virtualMemoryMapped(afterMiddle), "final reconstructed crop owner must unmap pixels");
}
QImage render(const SnowCanvasSceneItem& item, const SnowCanvasSmartEraseSnapshot& snapshot = {},
              qreal dpr = 1) {
    QImage image(QSize(qRound(64 * dpr), qRound(64 * dpr)), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::white);
    QPainter painter(&image);
    SceneDisplayInfo info{};
    info.surface_width = 64;
    info.surface_height = 64;
    info.camera_center_x = 32;
    info.camera_center_y = 32;
    info.camera_zoom = 1;
    snow_canvas_smart_erase::paint(painter, info, item, snapshot);
    return image;
}
snow_canvas_smart_erase::Result fakeResult(const SnowCanvasSceneItem& item) {
    QImage base(12, 12, QImage::Format_ARGB32);
    base.fill(Qt::white);
    QImage filled = base;
    filled.fill(Qt::blue);
    return {base, filled, snow_canvas_smart_erase::path(item).boundingRect(), true};
}
QImage trackedResultImage(std::atomic_int& released, QRgb color) {
    struct Pixels {
        explicit Pixels(std::atomic_int& counter) : released(counter) {}
        ~Pixels() {
            ++released;
        }
        std::array<QRgb, 12 * 12> values;
        std::atomic_int& released;
    };
    auto pixels = std::make_unique<Pixels>(released);
    pixels->values.fill(color);
    auto* data = reinterpret_cast<uchar*>(pixels->values.data());
    return QImage(
        data, 12, 12, 12 * static_cast<int>(sizeof(QRgb)), QImage::Format_ARGB32,
        [](void* context) { delete static_cast<Pixels*>(context); }, pixels.release());
}
void placeholders() {
    auto rect = rectangle();
    auto image = render(rect);
    require(image.pixelColor(32, 32) == QColor(255, 219, 220),
            "rectangle placeholder must use twenty percent red");
    require(image.pixelColor(5, 5) == Qt::white, "placeholder must preserve outside pixels");
    rect.opacity = 0;
    require(render(rect) == image, "placeholder must remain visible at zero element opacity");
    rect.rotation = 0.6;
    require(render(rect) != image, "rectangle placeholder must rotate");
    const auto doubleResolution = render(rect, {}, 2);
    require(doubleResolution.pixelColor(64, 64) == QColor(255, 219, 220),
            "DPI must preserve placeholder fill opacity");
    auto pen = rectangle();
    pen.is_free_draw = 1;
    pen.stroke_width = 8;
    const SnowArrowPoint points[]{{20, 32}, {44, 32}};
    pen.setArrowPoints(points, 2);
    image = render(pen);
    require(image.pixelColor(32, 32) == QColor(255, 219, 220),
            "pen interior must match rectangle fill");
    require(image.pixelColor(18, 32) == QColor(255, 219, 220), "pen must have round end caps");
    require(image.pixelColor(32, 25) == Qt::white, "pen fill must follow brush width");
    const SnowArrowPoint dot[]{{32, 32}, {32, 32}};
    pen.setArrowPoints(dot, 2);
    require(render(pen).pixelColor(32, 32) == QColor(255, 219, 220),
            "single-point stroke must render a round mark");
}
void asynchronousLifecycle() {
    using namespace snow_canvas_smart_erase;
    std::atomic_int calls{0};
    std::atomic_int finished{0};
    std::atomic_bool released{false};
    std::atomic_bool fail{false};
    int repaints = 0;
    Coordinator coordinator([&] { ++repaints; },
                            [&](const auto& item, const auto&, const std::atomic_bool& cancelled) {
                                ++calls;
                                while (!released.load() && !cancelled.load())
                                    QThread::msleep(1);
                                auto result =
                                    cancelled.load() || fail.load() ? Result{} : fakeResult(item);
                                ++finished;
                                return result;
                            });
    QImage base(64, 64, QImage::Format_ARGB32);
    base.fill(Qt::white);
    coordinator.setSources(&coordinator, {{base, QRectF(0, 0, 64, 64), {}}});
    repaints = 0;
    auto item = rectangle();
    item.filter.render_phase = 1;
    coordinator.syncItems({item});
    require(calls == 0, "creation must not schedule computation");
    item.filter.render_phase = 0;
    coordinator.syncItems({item});
    until([&] { return calls == 1; });
    const auto pending = coordinator.snapshot();
    require(render(item, pending).pixelColor(32, 32) == QColor(255, 219, 220),
            "running job must display placeholder");
    for (int i = 0; i < 20; ++i)
        coordinator.syncItems({item});
    require(calls == 1, "stable sync must coalesce an existing job");
    released = true;
    until([&] { return repaints == 1; });
    const auto ready = coordinator.snapshot();
    require(render(item, ready).pixelColor(32, 32) == Qt::blue, "completed result must render");
    require(render(item, pending).pixelColor(32, 32) == QColor(255, 219, 220),
            "captured pending export must remain immutable");
    item.opacity = 0.5;
    coordinator.syncItems({item});
    require(calls == 1, "opacity and selection-only sync must reuse computation");
    const QColor blended = render(item, coordinator.snapshot()).pixelColor(32, 32);
    require(blended.blue() == 255 && std::abs(blended.red() - 128) <= 1,
            "opacity must blend reconstruction with base pixels");
    item.filter.render_phase = 2;
    item.center_x += 2;
    coordinator.syncItems({item});
    require(render(item, coordinator.snapshot()).pixelColor(32, 32) == QColor(255, 219, 220),
            "active transform must hide cached result");
    require(calls == 1, "transform previews must not schedule jobs");
    item.filter.render_phase = 0;
    coordinator.syncItems({item});
    until([&] { return calls == 2 && repaints == 2; });
    item.center_x -= 2;
    coordinator.syncItems({item});
    require(calls == 2, "undo to cached geometry must reuse result");
    fail = true;
    base.fill(Qt::green);
    coordinator.setSources(&coordinator, {{base, QRectF(0, 0, 64, 64), {}}});
    coordinator.syncItems({item});
    until([&] { return calls == 3 && repaints == 4; });
    coordinator.syncItems({item});
    require(calls == 3, "failure must not retry on repaint or selection");
    item.filter.render_phase = 2;
    coordinator.syncItems({item});
    item.filter.render_phase = 0;
    fail = false;
    coordinator.syncItems({item});
    until([&] { return calls == 4 && repaints == 5; });
    released = false;
    item.center_x += 3;
    coordinator.syncItems({item});
    until([&] { return calls == 5; });
    coordinator.syncItems({});
    released = true;
    until([&] { return finished == 5; });
    std::vector<SnowCanvasSceneItem> exported;
    applySnapshot(exported, coordinator.snapshot());
    require(exported.empty(), "deleted elements must not survive in export snapshots");
    Coordinator worker([] {});
    worker.restoreSnapshot(ready);
    applySnapshot(exported, worker.snapshot());
    require(exported.size() == 1 &&
                render(exported.front(), worker.snapshot()).pixelColor(32, 32) == Qt::blue,
            "worker restoration must preserve ready appearance without computation");
}
void historicalCacheCleanup() {
    using namespace snow_canvas_smart_erase;
    std::array<std::atomic_int, 3> originalReleased{};
    std::array<std::atomic_int, 3> filledReleased{};
    const std::array<QRgb, 3> colors{qRgb(0, 0, 255), qRgb(0, 255, 0), qRgb(255, 0, 0)};
    std::atomic_int calls{0};
    int repaints = 0;
    Coordinator coordinator(
        [&] { ++repaints; },
        [&](const auto& item, const auto&, const std::atomic_bool& cancelled) {
            require(!cancelled.load(), "cache cleanup must not cancel current reconstruction");
            const auto index = static_cast<std::size_t>(calls.fetch_add(1));
            require(index < colors.size(), "cache cleanup must only reconstruct evicted geometry");
            return Result{trackedResultImage(originalReleased[index], qRgb(255, 255, 255)),
                          trackedResultImage(filledReleased[index], colors[index]),
                          path(item).boundingRect(), true};
        });
    QImage base(64, 64, QImage::Format_ARGB32);
    base.fill(Qt::white);
    coordinator.setSources(&coordinator, {{base, QRectF(0, 0, 64, 64), {}}});
    repaints = 0;
    auto original = rectangle();
    coordinator.syncItems({original});
    until([&] { return calls == 1 && repaints == 1; });
    require(render(original, coordinator.snapshot()).pixelColor(32, 32) == Qt::blue,
            "first reconstruction must render before becoming historical");
    auto current = original;
    current.center_x += 4;
    coordinator.syncItems({current});
    until([&] { return calls == 2 && repaints == 2; });
    require(originalReleased[0] == 0 && filledReleased[0] == 0,
            "historical cache must retain both result images before cleanup");
    auto currentSnapshot = coordinator.snapshot();
    coordinator.clearCache();
    require(originalReleased[0] == 1 && filledReleased[0] == 1,
            "cache cleanup must release both obsolete result images immediately");
    require(originalReleased[1] == 0 && filledReleased[1] == 0,
            "cache cleanup must preserve current reconstruction images");
    require(render(current, coordinator.snapshot()).pixelColor(36, 32) == Qt::green &&
                render(current, currentSnapshot).pixelColor(36, 32) == Qt::green,
            "current drawing and captured export must survive cache cleanup");
    coordinator.syncItems({current});
    require(calls == 2, "current reconstruction must remain reusable after cache cleanup");
    coordinator.syncItems({original});
    until([&] { return calls == 3 && repaints == 3; });
    require(render(original, coordinator.snapshot()).pixelColor(32, 32) == Qt::red,
            "returning to evicted geometry must render its newly computed result");
    require(render(current, currentSnapshot).pixelColor(36, 32) == Qt::green,
            "captured current result must remain immutable after recomputation");
    currentSnapshot = {};
    require(originalReleased[1] == 1 && filledReleased[1] == 1,
            "replaced current result must release when its final export snapshot is dropped");
    coordinator.clearCache();
    require(originalReleased[2] == 0 && filledReleased[2] == 0,
            "recomputed current result must survive subsequent cache cleanup");
    coordinator.reset();
    require(originalReleased[2] == 1 && filledReleased[2] == 1,
            "reset must release the final current result after cache cleanup");
}
void reconstruction() {
    using namespace snow_canvas_smart_erase;
    QImage base(64, 64, QImage::Format_ARGB32);
    base.fill(QColor(90, 160, 210));
    {
        QPainter painter(&base);
        painter.fillRect(QRect(26, 26, 12, 12), Qt::black);
    }
    std::atomic_bool cancelled{false};
    const auto item = rectangle();
    const QList<SnowCanvasBaseImageSource> sources{{base, QRectF(0, 0, 64, 64), {}}};
    const auto result = reconstruct(item, sources, cancelled);
    require(result.success && !result.filled.isNull(),
            "flat-background reconstruction must succeed");
    const auto center =
        result.filled.pixelColor(result.filled.width() / 2, result.filled.height() / 2);
    require(std::abs(center.red() - 90) < 3 && std::abs(center.green() - 160) < 3 &&
                std::abs(center.blue() - 210) < 3,
            "removed black object must be reconstructed from surrounding color");
    const auto repeated = reconstruct(item, sources, cancelled);
    require(result.filled == repeated.filled,
            "fixed reconstruction input must produce deterministic pixels");
    // Equivalent pixels supplied by adjacent displays must give the same reconstruction.
    const QList<SnowCanvasBaseImageSource> split{
        {base.copy(0, 0, 32, 64), QRectF(0, 0, 32, 64), {}},
        {base.copy(32, 0, 32, 64), QRectF(32, 0, 32, 64), {}}};
    require(reconstruct(item, split, cancelled).filled == result.filled,
            "display boundaries must not change reconstruction at equal pixel scale");
    auto negative = item;
    negative.center_x -= 100;
    negative.center_y -= 200;
    const auto shifted = reconstruct(negative, {{base, QRectF(-100, -200, 64, 64), {}}}, cancelled);
    require(shifted.success && shifted.filled == result.filled,
            "negative canvas origins must preserve source pixels and mask alignment");
    auto rotated = item;
    rotated.rotation = 0.6;
    const auto rotatedResult = reconstruct(rotated, sources, cancelled);
    require(rotatedResult.success, "rotated source mask must reconstruct successfully");
    auto outside = item;
    outside.center_x = -1000;
    const auto empty = reconstruct(outside, sources, cancelled);
    require(empty.success && empty.filled.isNull(),
            "out-of-image geometry must be an empty result");
    auto all = item;
    all.width = 64;
    all.height = 64;
    require(!reconstruct(all, sources, cancelled).success,
            "fully masked source must fail without inventing donor pixels");
    QImage textured(64, 64, QImage::Format_ARGB32);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x)
            textured.setPixel(x, y, qRgb(80 + x, 100 + y, 120 + (x + y) / 2));
    }
    auto pen = item;
    pen.is_free_draw = 1;
    pen.stroke_width = 8;
    const SnowArrowPoint points[]{{20, 20}, {44, 44}};
    pen.setArrowPoints(points, 2);
    const auto maskPath = path(pen);
    {
        QPainter painter(&textured);
        painter.fillPath(maskPath, Qt::black);
    }
    const QList<SnowCanvasBaseImageSource> textureSources{{textured, QRectF(0, 0, 64, 64), {}}};
    const auto textureResult = reconstruct(pen, textureSources, cancelled);
    require(textureResult.success && !textureResult.filled.isNull(),
            "nonrepeating background must reconstruct through patch search");
    require(textureResult.filled == reconstruct(pen, textureSources, cancelled).filled,
            "general patch search must be deterministic");
    require(textureResult.filled
                    .pixelColor(textureResult.filled.width() / 2, textureResult.filled.height() / 2)
                    .red() > 50,
            "masked dark pixels must not contaminate multiscale donor patches");
    require(textureResult.filled.pixel(0, textureResult.filled.height() - 1) ==
                textureResult.original.pixel(0, textureResult.original.height() - 1),
            "pixels outside the pen mask must remain byte-for-byte unchanged");
    cancelled = true;
    require(!reconstruct(item, sources, cancelled).success,
            "cancelled work must not produce a result");
}

void widgetAndWorkerExport() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(128, 128);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(64, 64, 1), "configure widget camera");
    QImage base(128, 128, QImage::Format_ARGB32);
    base.fill(Qt::white);
    {
        QPainter painter(&base);
        painter.fillRect(QRect(54, 54, 20, 20), Qt::black);
    }
    canvas.setBaseImageSources({{base, QRectF(0, 0, 128, 128), {}}});
    require(canvas.setCanvasTool(SnowCanvasTool::RectangleFilter), "activate rect filter");
    SnowCanvasFilterStyle style;
    style.type = SnowCanvasFilterType::SmartErase;
    require(canvas.setCanvasFilterStyle(style, SnowCanvasFilterStylePropertyType),
            "set Smart Erase type");
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    const QList<CanvasExportSource> sources{{base, QRectF(0, 0, 128, 128)}};
    mouse(QEvent::MouseButtonPress, QPointF(50, 50), Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, QPointF(78, 78), Qt::NoButton, Qt::LeftButton);
    const auto creating = runtime.smartEraseSnapshot();
    require(snow_canvas_smart_erase::hasItems(creating),
            "creating geometry must reach coordinator");
    QImage preview = runtime.renderToImage(QRectF(0, 0, 128, 128), base.size(), sources);
    require(preview.pixelColor(64, 64).red() > 30,
            "export during creation must include red preview");
    mouse(QEvent::MouseButtonRelease, QPointF(78, 78), Qt::LeftButton, Qt::NoButton);
    const auto pending = runtime.smartEraseSnapshot();
    const auto document = runtime.serializeDocumentSession();
    require(!document.isEmpty(), "Smart Erase document must serialize");
    until([&] {
        return runtime.renderToImage(QRectF(0, 0, 128, 128), base.size(), sources)
                   .pixelColor(64, 64)
                   .green() > 245;
    });
    const auto ready = runtime.smartEraseSnapshot();
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select ready Smart Erase");
    mouse(QEvent::MouseButtonPress, QPointF(64, 64), Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, QPointF(70, 70), Qt::NoButton, Qt::LeftButton);
    std::vector<SnowCanvasSceneItem> transformed;
    snow_canvas_smart_erase::applySnapshot(transformed, runtime.smartEraseSnapshot());
    require(transformed.size() == 1 && transformed.front().filter.render_phase == 2,
            "real widget transform must carry the transforming phase through FFI");
    mouse(QEvent::MouseButtonRelease, QPointF(70, 70), Qt::LeftButton, Qt::NoButton);
    transformed.clear();
    snow_canvas_smart_erase::applySnapshot(transformed, runtime.smartEraseSnapshot());
    require(transformed.size() == 1 && transformed.front().filter.render_phase == 0,
            "commit must be stable even while selection remains active");
    until([&] {
        return runtime.renderToImage(QRectF(0, 0, 128, 128), base.size(), sources)
                   .pixelColor(70, 70) != QColor(51, 15, 16);
    });
    require(canvas.undo(), "undo transformed geometry");
    require(runtime.renderToImage(QRectF(0, 0, 128, 128), base.size(), sources)
                    .pixelColor(64, 64)
                    .green() > 245,
            "undo must immediately reuse original geometry result");
    SnowCanvasRuntime worker;
    require(worker.restoreDocumentSession(document), "worker must restore Smart Erase document");
    worker.restoreSmartEraseSnapshot(ready);
    require(worker.renderToImage(QRectF(0, 0, 128, 128), base.size(), sources)
                    .pixelColor(64, 64)
                    .green() > 245,
            "ready export snapshot must survive worker restoration");
    worker.restoreSmartEraseSnapshot(pending);
    const auto pendingImage = worker.renderToImage(QRectF(0, 0, 128, 128), base.size(), sources);
    require(pendingImage.pixelColor(64, 64).green() < 40,
            "pending export must not adopt later completion");
    worker.restoreSmartEraseSnapshot(ready);
    // A normal filter above Smart Erase must sample its completed pixels.
    require(runtime.setQuickSelectionDisabledTools({SnowCanvasTool::RectangleFilter}),
            "disable quick selection to create an overlapping filter");
    require(canvas.resetEditingStatePreservingTool(),
            "clear selection before creating a second filter");
    require(canvas.setCanvasFilterStyle({SnowCanvasFilterType::Inversion, 0.5, 1, 30},
                                        SnowCanvasFilterStylePropertyType),
            "switch to inversion");
    require(canvas.setCanvasTool(SnowCanvasTool::RectangleFilter),
            "reactivate rectangle creation after first commit");
    mouse(QEvent::MouseButtonPress, QPointF(58, 58), Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, QPointF(70, 70), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, QPointF(70, 70), Qt::LeftButton, Qt::NoButton);
    const auto inverted = runtime.renderToImage(QRectF(0, 0, 128, 128), base.size(), sources);
    require(inverted.pixelColor(64, 64).green() < 10,
            "ordinary filter must render above completed Smart Erase");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--memory-ownership-only"))) {
        reconstructionBuffersReturnMappedPages();
        return 0;
    }
    reconstructionBuffersReturnMappedPages();
    placeholders();
    asynchronousLifecycle();
    historicalCacheCleanup();
    reconstruction();
    widgetAndWorkerExport();
    std::cout << "Smart Erase tests passed\n";
    return 0;
}
