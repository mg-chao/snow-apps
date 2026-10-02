#pragma once

#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/radio_button_group.h"
#include <QAbstractButton>
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>

inline void verifyEraserToolbarHost(ScreenshotToolPalette& palette, SnowCanvasWidget& canvas,
                                    void (*require)(bool, const char*)) {
    using Tool = ScreenshotToolPalette::Tool;
    const snow_shot::storage::ScreenshotToolbarSettings settings;
    require(palette.activateDrawingShortcut(QStringLiteral("eraser")) &&
                canvas.canvasTool() == SnowCanvasTool::Eraser,
            "first host eraser activation preserves the whole-element default");
    const auto choose = [&](Tool tool) {
        auto* selector =
            palette.findChild<QWidget*>(QStringLiteral("screenshotEraserModeSelector"));
        auto* group =
            selector ? selector->findChild<adqt::widgets::AdRadioButtonGroup*>() : nullptr;
        require(group && group->button(static_cast<int>(tool)),
                "host eraser modes expose the secondary selector");
        group->button(static_cast<int>(tool))->click();
    };
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas.mapToGlobal(point), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    const auto gesture = [&](QPointF first, QPointF last) {
        mouse(QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, (first + last) / 2, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, last, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, last, Qt::LeftButton, Qt::NoButton);
    };
    const auto pixel = [&](const QImage& image, QPointF point) {
        return image.pixelColor((point * canvas.devicePixelRatioF()).toPoint());
    };
    for (const auto tool : {Tool::RectangleEraser, Tool::BrushEraser}) {
        canvas.clearDocument();
        require(palette.activateDrawingShortcut(QStringLiteral("brush")),
                "host drawing shortcut activates a real annotation tool");
        gesture({40, 80}, {180, 80});
        const QImage before = canvas.grab().toImage();
        require(pixel(before, {100, 80}).alpha() > 200, "eraser fixture paints an opaque stroke");
        require(palette.activateDrawingShortcut(QStringLiteral("eraser")),
                "host generic eraser entry activates its remembered mode");
        choose(tool);
        const auto expected = tool == Tool::RectangleEraser ? SnowCanvasTool::RectangleEraser
                                                            : SnowCanvasTool::BrushEraser;
        require(canvas.canvasTool() == expected && palette.activeTool() == tool,
                "secondary eraser activation reaches the host canvas");
        gesture(tool == Tool::RectangleEraser ? QPointF(90, 60) : QPointF(90, 80),
                tool == Tool::RectangleEraser ? QPointF(120, 100) : QPointF(120, 80));
        const QImage erased = canvas.grab().toImage();
        require(pixel(erased, {100, 80}).alpha() <= 2 && pixel(erased, {50, 80}).alpha() > 200,
                "host erasers restore the transparent original only inside their geometry");
        require(canvas.undo() &&
                    pixel(canvas.grab().toImage(), {100, 80}) == pixel(before, {100, 80}),
                "host eraser undo restores the original annotation pixels");
        require(canvas.redo() && pixel(canvas.grab().toImage(), {100, 80}).alpha() <= 2,
                "host eraser redo restores the transparent patch");
    }
    const double width = canvas.canvasStyleToolbarState().brushEraserStyle.strokeWidth;
    const QPointF point(100, 100);
    QWheelEvent wheel(point, canvas.mapToGlobal(point.toPoint()), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &wheel);
    require(
        canvas.canvasStyleToolbarState().brushEraserStyle.strokeWidth == width + 1 &&
            snow_shot::presentation::screenshotCanvasToolStyleDefaults().brushEraser.strokeWidth ==
                width + 1,
        "host canvas wheel applies and persists the independent brush eraser width");
    require(palette.activateDrawingShortcut(QStringLiteral("eraser")) &&
                canvas.canvasTool() == SnowCanvasTool::Select &&
                settings.lastEraserTool() == QStringLiteral("brush-eraser"),
            "repeated eraser activation selects without losing the host mode");
    require(palette.activateDrawingShortcut(QStringLiteral("eraser")) &&
                canvas.canvasTool() == SnowCanvasTool::BrushEraser,
            "host eraser entry restores its last selected variant");
}
