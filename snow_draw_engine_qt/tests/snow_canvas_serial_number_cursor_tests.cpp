#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_canvas_cursor_controller.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_input_adapter.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_viewport.h"

#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct Fixture {
    ScopedRuntimeHandle runtime;
    SnowViewport viewport = nullptr;

    Fixture() {
        require(snow_runtime_create(runtime.outParam()) == SNOW_OK, "create cursor runtime");
        const auto config = snow_canvas_viewport::defaultEngineConfig();
        require(snow_viewport_create(runtime.get(), &config, &viewport) == SNOW_OK,
                "create cursor viewport");
        require(snow_viewport_set_surface_size(runtime.get(), viewport, 800, 600) == SNOW_OK,
                "size cursor viewport");
        ScopedChangedViewportList changed;
        require(snow_viewport_set_active_tool_ex(runtime.get(), viewport,
                                                 SNOW_ACTIVE_TOOL_SERIAL_NUMBER,
                                                 changed.outParam()) == SNOW_OK,
                "activate cursor tool");
    }
    ~Fixture() {
        snow_viewport_destroy(runtime.get(), viewport);
    }

    SnowCanvasSceneItem preview(QPointF position = {400, 300}) const {
        SnowCanvasSceneItem item;
        require(snow_viewport_visit_serial_number_cursor_preview(
                    runtime.get(), viewport, position.x(), position.y(), {},
                    [](void* context, const SnowSceneDisplayItem* borrowed) {
                        *static_cast<SnowCanvasSceneItem*>(context) = *borrowed;
                    },
                    &item) == SNOW_OK,
                "query owned cursor preview");
        return item;
    }
    void setStyle(const SnowSerialNumberStyle& style) {
        ScopedChangedViewportList changed;
        require(snow_viewport_set_serial_number_style_ex(runtime.get(), viewport, &style,
                                                         changed.outParam()) == SNOW_OK,
                "set cursor creation style");
    }
    SnowCanvasSceneItem place() {
        SnowInteractionOutput output{};
        ScopedChangedViewportList changed;
        auto input = snow_canvas_input::makePointerInput({400, 300}, Qt::LeftButton, Qt::LeftButton,
                                                         Qt::NoModifier, SNOW_POINTER_EVENT_DOWN);
        require(snow_viewport_process_input_ex(runtime.get(), viewport, &input, &output,
                                               changed.outParam()) == SNOW_OK,
                "place previewed badge");
        ScopedPatchHandle patch;
        require(snow_viewport_acquire_patch(runtime.get(), viewport, nullptr, patch.outParam()) ==
                    SNOW_OK,
                "read placed badge patch");
        const SnowSceneDisplayItem* items = nullptr;
        uint32_t count = 0;
        require(snow_patch_get_scene_items(patch.get(), &items, &count) == SNOW_OK,
                "read placed badge");
        for (uint32_t index = 0; index < count; ++index) {
            if (items[index].kind == SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER) {
                return SnowCanvasSceneItem(items[index]);
            }
        }
        require(false, "placed badge must be in the scene");
        return {};
    }
};

QImage pixels(const QCursor& cursor) {
    return cursor.pixmap().toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage renderPlaced(SnowCanvasSceneItem item, const QCursor& cursor, const QFont& font,
                    double zoom) {
    item.center_x = item.center_y = 0;
    QImage image(cursor.pixmap().size(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(cursor.pixmap().devicePixelRatio());
    image.fill(Qt::transparent);
    SceneDisplayInfo info;
    info.surface_width = cursor.hotSpot().x() * 2;
    info.surface_height = cursor.hotSpot().y() * 2;
    info.camera_zoom = zoom;
    QPainter painter(&image);
    painter.setFont(font);
    painter.setRenderHint(QPainter::Antialiasing);
    snow_canvas_renderer::SceneRenderRequest request;
    request.painter = &painter;
    request.displayInfo = &info;
    request.sceneItems = &item;
    request.sceneItemCount = 1;
    request.exposedRegion =
        QRegion(0, 0, cursor.hotSpot().x() * 2 + 1, cursor.hotSpot().y() * 2 + 1);
    snow_canvas_renderer::renderSceneItems(request);
    painter.end();
    return image;
}

void cursorMatchesPlacedPixels() {
    SnowStyleDefaults defaults{};
    require(snow_runtime_style_defaults_default(&defaults) == SNOW_OK, "read style defaults");
    const auto families = QFontDatabase::families();
    for (int shape = 0; shape < 5; ++shape) {
        for (int numeric = 0; numeric < 5; ++numeric) {
            for (int variant = 0; variant < 3; ++variant) {
                Fixture fixture;
                auto style = defaults.serial_number;
                style.serial_number_type = static_cast<SnowSerialNumberType>(shape);
                style.numeric_type = static_cast<SnowSerialNumberNumericType>(numeric);
                style.number = variant == 2 ? std::numeric_limits<int64_t>::max() : 27;
                style.font_size = 28 + variant * 7;
                style.color = {30, 100, 210, 180};
                style.fill = {230, 180, 90, 120};
                style.opacity = 0.75;
                style.fill_style = static_cast<SnowFillStyle>(variant);
                style.stroke_rendering_style = static_cast<SnowStrokeRenderingStyle>(variant);
                fixture.setStyle(style);
                const auto preview = fixture.preview();
                const auto placed = fixture.place();
                QFont font = QApplication::font();
                if (variant != 0 && !families.isEmpty()) {
                    font.setFamily(families.at(variant % families.size()));
                }
                require(snow_canvas_input::serialNumberCursorKey(preview, font, 1.0) ==
                            snow_canvas_input::serialNumberCursorKey(placed, font, 1.0),
                        "preview must use the committed creation values and seed");
                for (qreal dpr : {1.0, 1.5, 2.0}) {
                    for (double zoom : {0.75, 2.0}) {
                        const auto cursor =
                            snow_canvas_input::serialNumberCursor(preview, font, zoom, dpr);
                        require(cursor.shape() == Qt::BitmapCursor,
                                "badge must be a native bitmap cursor");
                        require(cursor.pixmap().devicePixelRatio() == dpr, "cursor preserves DPI");
                        const int size = cursor.hotSpot().x() * 2 + 1;
                        require(cursor.hotSpot().x() == cursor.hotSpot().y() &&
                                    cursor.pixmap().width() ==
                                        static_cast<int>(std::ceil(size * dpr)),
                                "badge hotspot must remain centered in logical pixels");
                        const QImage actual = pixels(cursor);
                        require(actual == renderPlaced(placed, cursor, font, zoom),
                                "cursor pixels must exactly match the placed badge renderer");
                        require(actual.pixelColor(0, 0).alpha() == 0 &&
                                    actual.pixelColor(actual.width() - 1, actual.height() - 1)
                                            .alpha() == 0,
                                "cursor padding must stay transparent");
                    }
                }
            }
        }
    }
}

class CountingCanvas : public SnowCanvasWidget {
  public:
    using SnowCanvasWidget::SnowCanvasWidget;
    int cursorChanges = 0;

  protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::CursorChange)
            ++cursorChanges;
        return SnowCanvasWidget::event(event);
    }
};

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF position,
           Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton) {
    QMouseEvent event(type, position, position, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}

void widgetRefreshAndCacheLifecycle() {
    SnowCanvasRuntime runtime;
    CountingCanvas canvas(runtime);
    canvas.resize(800, 600);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate native sequence cursor");
    require(canvas.cursor().shape() == Qt::BitmapCursor,
            "activation previews before pointer motion");
    const QImage first = pixels(canvas.cursor());
    mouse(canvas, QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {100, 100}, Qt::LeftButton);
    require(canvas.cursor().shape() == Qt::BitmapCursor && pixels(canvas.cursor()) != first,
            "release immediately previews the next number");
    mouse(canvas, QEvent::MouseMove, {600, 450});
    const QCursor cached = canvas.cursor();
    const int changes = canvas.cursorChanges;
    for (int index = 0; index < 10; ++index)
        mouse(canvas, QEvent::MouseMove, {650.0 + index, 450});
    require(canvas.cursor() == cached && canvas.cursorChanges == changes,
            "ordinary motion must reuse the native cursor without sprite changes");
    require(canvas.undo(), "undo previewed placement");
    require(pixels(canvas.cursor()) == first, "undo refreshes the next creation number");
    require(canvas.redo(), "redo previewed placement");
    require(pixels(canvas.cursor()) == pixels(cached), "redo refreshes the next creation number");
    const auto originalSize = canvas.cursor().pixmap().size();
    canvas.setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::WaitCursor));
    require(canvas.setViewportCamera(0, 0, 2), "zoom sequence cursor");
    require(canvas.cursor().shape() == Qt::WaitCursor, "host override retains priority");
    canvas.clearCursorForLayer(SnowCanvasCursorLayer::Host);
    require(canvas.cursor().pixmap().width() > originalSize.width(), "restore latest zoomed badge");
    const QImage beforeStyle = pixels(canvas.cursor());
    auto style = canvas.canvasStyleToolbarState().serialNumberStyle;
    style.color = QColor(20, 200, 80);
    style.fontSize = 40;
    require(canvas.setCanvasSerialNumberStyle(style), "update creation appearance");
    require(pixels(canvas.cursor()) != beforeStyle, "style updates immediately without motion");
    const auto beforeFont = canvas.cursor();
    QFont font = canvas.font();
    font.setFamily(QStringLiteral("monospace"));
    canvas.setFont(font);
    require(canvas.cursor() != beforeFont, "font change refreshes the native cursor");
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "switch to text");
    require(canvas.cursor().shape() == Qt::IBeamCursor, "text cursor is preserved");
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "resume sequence cursor");
    mouse(canvas, QEvent::MouseButtonPress, {600, 450}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {700, 500}, Qt::NoButton, Qt::LeftButton);
    require(canvas.cursor().shape() != Qt::BitmapCursor,
            "attached-text drag keeps interaction cursor");
    mouse(canvas, QEvent::MouseButtonRelease, {700, 500}, Qt::LeftButton);
    require(canvas.hasActiveTextEditing(), "sequence drag still starts attached-text editing");
    require(canvas.resetEditingState() && canvas.clearDocument(), "reset cursor runtime");
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "restore preview after reset");
    require(canvas.cursor().shape() == Qt::BitmapCursor, "reset must rebuild a native preview");
}

void controllerDpiAndSeedCaching() {
    Fixture fixture;
    auto item = fixture.preview();
    QWidget widget;
    SnowCanvasCursorController controller(widget);
    controller.configureSerialNumberCursor(&item, 1);
    controller.setEngineCursor(SNOW_CURSOR_STYLE_SERIAL_NUMBER);
    const auto first = widget.cursor();
    item.center_x += 100;
    ++item.stroke_rendering_seed;
    controller.configureSerialNumberCursor(&item, 1);
    require(widget.cursor() == first, "architect motion ignores position-only seed changes");
    for (qreal dpr : {1.0, 1.5, 2.0}) {
        controller.refreshDevicePixelRatio(dpr);
        require(widget.cursor().pixmap().devicePixelRatio() == dpr,
                "DPI change rebuilds native cursor");
        const auto cached = widget.cursor();
        controller.refreshDevicePixelRatio(dpr);
        require(widget.cursor() == cached, "unchanged DPI reuses native cursor");
    }
    item.stroke_rendering_style = SNOW_STROKE_RENDERING_STYLE_ARTIST;
    controller.configureSerialNumberCursor(&item, 1);
    const auto rough = widget.cursor();
    ++item.stroke_rendering_seed;
    controller.configureSerialNumberCursor(&item, 1);
    require(widget.cursor() != rough, "rough stroke seed changes refresh the preview");
    controller.setEngineCursor(SNOW_CURSOR_STYLE_MOVE);
    controller.configureSerialNumberCursor(&item, 2);
    require(widget.cursor().shape() == Qt::SizeAllCursor,
            "preview updates preserve selection cursors");
    const auto huge = snow_canvas_input::serialNumberCursor(item, widget.font(), 1e8, 2);
    require(huge.pixmap().width() <= 2060, "extreme zoom has bounded native allocation");

    SnowCanvasSceneItem detached;
    {
        Fixture temporary;
        detached = temporary.preview();
    }
    require(!snow_canvas_input::serialNumberCursor(detached, widget.font(), 1).pixmap().isNull(),
            "copied preview remains valid after its runtime is destroyed");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "cursor tests require a system font");
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) >= 0,
            "cursor tests require a CJK font");
#endif
    cursorMatchesPlacedPixels();
    widgetRefreshAndCacheLifecycle();
    controllerDpiAndSeedCaching();
    return 0;
}
