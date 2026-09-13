#include "snow_shot/presentation/screenshotautofiltercontroller.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMouseEvent>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
struct Fixture {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas{runtime};
    QRectF bounds{0, 0, 200, 200};
    qint64 now = 1000;
    int runs = 0;
    ScreenshotAutoFilterController::Completion completion;
    ScreenshotAutoFilterController controller{[this]() { return bounds; },
                                              [](auto done) {
                                                  QImage image(100, 100, QImage::Format_RGB32);
                                                  image.fill(Qt::white);
                                                  done(image);
                                              },
                                              nullptr,
                                              [this](QImage image, auto done) {
                                                  require(image.size() == QSize(100, 100),
                                                          "original image input");
                                                  ++runs;
                                                  completion = std::move(done);
                                              },
                                              [this]() { return now; }};
    Fixture() {
        canvas.resize(200, 200);
        controller.attachCanvas(&canvas);
    }
    void activate() {
        canvas.setCanvasTool(SnowCanvasTool::AutoFilter);
        controller.validate();
    }
    void complete(bool empty = false) {
        auto done = std::move(completion);
        done(empty ? QList<SnowCanvasAutoFilterRegion>{}
                   : QList<SnowCanvasAutoFilterRegion>{{1, QRectF(0, 0, 50, 50),
                                                        QStringLiteral("image")},
                                                       {2, QRectF(10, 10, 10, 10),
                                                        QStringLiteral("text")}},
             {});
    }
    int fills() const {
        const QJsonArray elements = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                                        .object()
                                        .value(QStringLiteral("document"))
                                        .toObject()
                                        .value(QStringLiteral("slots"))
                                        .toArray();
        int result = 0;
        for (const auto& slot : elements) {
            if (slot.toObject()
                    .value(QStringLiteral("data"))
                    .toObject()
                    .value(QStringLiteral("Filter"))
                    .toObject()
                    .contains(QStringLiteral("auto_region_id"))) {
                ++result;
            }
        }
        return result;
    }
};
void lifecycle() {
    Fixture f;
    f.activate();
    f.controller.validate();
    require(f.runs == 1 && f.controller.detecting(), "single flight");
    f.complete();
    require(f.controller.available() && f.controller.flashing(), "accepted detection flashes");
    require(f.canvas.autoFilterRegions()->regions[1].bounds == QRectF(20, 20, 20, 20),
            "pixel regions map to canvas");
    f.now += 299;
    require(f.controller.flashing(), "flash lasts 299ms");
    ++f.now;
    require(!f.controller.flashing(), "flash ends at 300ms");
    f.controller.fillCategory(QStringLiteral("text"));
    require(f.fills() == 1, "category fills");
    f.canvas.setCanvasTool(SnowCanvasTool::Shape);
    f.bounds.translate(300, 0);
    require(f.fills() == 1 && !f.controller.available(),
            "selection change preserves fills and makes dropdown stale");
    f.activate();
    require(f.runs == 2 && f.fills() == 0, "stale activation atomically resets");
    f.complete();
    require(f.canvas.undo() && !f.canvas.autoFilterRegions(),
            "undo identification returns unidentified");
    require(f.canvas.undo() && f.fills() == 1, "undo reset restores fills");
    require(f.canvas.autoFilterRegions()->sourceBounds.x() == 0,
            "undo restores original selection record");
    f.controller.validate();
    require(f.runs == 3 && f.fills() == 0, "next validation enforces revived mismatch");
}
void races() {
    Fixture f;
    f.activate();
    f.canvas.setCanvasTool(SnowCanvasTool::Shape);
    f.complete();
    require(f.canvas.autoFilterRegions().has_value() && !f.controller.flashing(),
            "tool switch keeps result without visuals");
    f.activate();
    require(f.runs == 1 && !f.controller.flashing(), "identified activation reuses without flash");
    f.canvas.undo();
    f.controller.validate();
    require(f.runs == 2, "undo identification permits next run");
    const quint64 generation = f.canvas.autoFilterGeneration();
    f.canvas.redo();
    f.canvas.undo();
    require(f.canvas.autoFilterGeneration() != generation, "record generation never rewinds");
    f.complete();
    require(!f.canvas.autoFilterRegions() && !f.controller.flashing(),
            "discard old unidentified run after history roundtrip");
    require(f.runs == 2, "discard never auto retries");
    f.controller.validate();
    f.bounds.translate(200, 0);
    f.complete();
    require(f.canvas.autoFilterRegions()->sourceBounds.x() == 0 && !f.controller.available(),
            "selection change accepts original result as stale");
}
void emptyFailureAndDisposal() {
    Fixture f;
    f.activate();
    f.complete(true);
    require(f.canvas.autoFilterRegions().has_value() && !f.controller.available(),
            "zero result is identified but disabled");
    f.canvas.undo();
    require(!f.canvas.canvasHistoryState().canUndo, "zero result is one history entry");
    f.controller.validate();
    auto done = std::move(f.completion);
    done({}, QStringLiteral("failure"));
    require(!f.controller.detecting() && !f.canvas.autoFilterRegions() &&
                !f.canvas.canvasHistoryState().canUndo,
            "failure preserves unidentified without history");
    f.controller.validate();
    f.controller.resetSession();
    f.complete();
    require(!f.canvas.autoFilterRegions(), "disposed session rejects completion");
}
void renderingAndExport() {
    Fixture f;
    f.activate();
    f.complete();
    QImage source(200, 200, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 200; ++x) {
            source.setPixelColor(
                x, y,
                QColor((x * 17 + y * 3) % 256, (x * 5 + y * 19) % 256, (x * 11 + y * 7) % 256));
        }
    }
    const auto render = [&]() {
        return f.runtime.renderToImage(f.bounds, source.size(), {{source, f.bounds}});
    };
    require(render() == source, "identification flash is excluded from export");
    for (const auto type : {SnowCanvasFilterType::Mosaic, SnowCanvasFilterType::GaussianBlur,
                            SnowCanvasFilterType::Grayscale, SnowCanvasFilterType::Inversion}) {
        auto style = f.canvas.canvasStyleToolbarState().filterStyle;
        style.type = type;
        style.strength = 0.6;
        f.canvas.setCanvasFilterStyle(style, SnowCanvasFilterStylePropertyType |
                                                 SnowCanvasFilterStylePropertyStrength);
        f.controller.fillCategory(QStringLiteral("image"));
        const QImage filtered = render();
        require(!filtered.isNull() && filtered.pixel(150, 150) == source.pixel(150, 150),
                "filter export preserves outside pixels");
        bool changed = false;
        for (int y = 5; y < 95; ++y) {
            for (int x = 5; x < 95; ++x) {
                changed |= filtered.pixel(x, y) != source.pixel(x, y);
            }
        }
        require(changed, "every auto filter type changes pixels within the region");
        style.strength = 0.1;
        f.canvas.setCanvasFilterStyle(style, SnowCanvasFilterStylePropertyStrength);
        require(render() == filtered, "later toolbar strength never rewrites a fill");
    }
    SnowCanvasRuntime restored;
    require(restored.restoreDocumentSession(f.runtime.serializeDocumentSession()),
            "pinned session restores");
    SnowCanvasWidget restoredCanvas(restored);
    require(restoredCanvas.autoFilterRegions()->regions.size() == 2 &&
                restored.renderToImage(f.bounds, source.size(), {{source, f.bounds}}) == render(),
            "restored regions and filtered export match original session");
    f.canvas.setViewportCamera(100, 100, 2);
    require(f.controller.available(), "viewport zoom does not invalidate image bounds");
}
void pointer(Fixture& f, QEvent::Type type, QPointF position) {
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&f.canvas, &event);
}
void fallbackGesturesAndUnrelatedEdits() {
    Fixture f;
    f.canvas.show();
    f.canvas.setViewportCamera(100, 100, 1);
    f.activate();
    const auto generation = f.canvas.autoFilterGeneration();
    f.canvas.setCanvasTool(SnowCanvasTool::Shape);
    pointer(f, QEvent::MouseButtonPress, {120, 120});
    pointer(f, QEvent::MouseMove, {170, 170});
    pointer(f, QEvent::MouseButtonRelease, {170, 170});
    require(f.canvas.canvasHistoryState().canUndo && f.canvas.autoFilterGeneration() == generation,
            "unrelated drawing edits preserve request generation");
    f.complete();
    require(f.canvas.autoFilterRegions().has_value(),
            "unrelated edit allows detection to complete");
    f.activate();
    f.canvas.undo();
    require(!f.canvas.autoFilterRegions(), "undo identification before fallback gesture");
    pointer(f, QEvent::MouseButtonPress, {30, 30});
    require(f.runs == 2, "pointer hook starts fallback detection");
    f.complete();
    pointer(f, QEvent::MouseButtonRelease, {30, 30});
    require(f.fills() == 0, "fallback gesture cannot fill newly arriving regions");
    pointer(f, QEvent::MouseButtonPress, {30, 30});
    pointer(f, QEvent::MouseButtonRelease, {30, 30});
    require(f.fills() == 1, "subsequent click fills the child region");
    pointer(f, QEvent::MouseButtonPress, {25, 25});
    pointer(f, QEvent::MouseMove, {45, 45});
    pointer(f, QEvent::MouseButtonRelease, {45, 45});
    require(f.fills() == 1, "marquee toggles both intersecting parent and child");
    require(f.canvas.undo() && f.fills() == 1, "one undo restores the whole marquee");
}
void destroyedReceiverIgnoresCompletion() {
    auto f = std::make_unique<Fixture>();
    f->activate();
    auto done = std::move(f->completion);
    f.reset();
    done({}, {});
}

void styles() {
    Fixture f;
    f.activate();
    f.complete();
    auto style = f.canvas.canvasStyleToolbarState().filterStyle;
    style.strength = 0.2;
    style.type = SnowCanvasFilterType::GaussianBlur;
    f.canvas.setCanvasFilterStyle(style, SnowCanvasFilterStylePropertyStrength |
                                             SnowCanvasFilterStylePropertyType);
    f.controller.fillCategory(QStringLiteral("text"));
    f.canvas.setCanvasTool(SnowCanvasTool::PenFilter);
    require(f.canvas.canvasStyleToolbarState().filterStyle.strength == 0.2,
            "pen uses shared strength");
    style.strength = 0.8;
    f.canvas.setCanvasFilterStyle(style, SnowCanvasFilterStylePropertyStrength);
    f.canvas.setCanvasTool(SnowCanvasTool::RectangleFilter);
    require(f.canvas.canvasStyleToolbarState().filterStyle.strength == 0.8 &&
                f.canvas.canvasStyleToolbarState().filterStyle.type ==
                    SnowCanvasFilterType::GaussianBlur,
            "rectangle shares strength and auto type");
    require(f.fills() == 1, "default style changes preserve fills");
}
} // namespace
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    lifecycle();
    races();
    emptyFailureAndDisposal();
    styles();
    renderingAndExport();
    fallbackGesturesAndUnrelatedEdits();
    destroyedReceiverIgnoresCompletion();
    std::cout << "Auto Filter lifecycle, races, mapping, history, and styles passed\n";
}
