#include "snow_shot/presentation/screenshotstylebinding.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include <QDebug>
#include <QPointer>
#include <numbers>

namespace snow_shot::presentation {
ScreenshotStyleBinding::ScreenshotStyleBinding(ScreenshotToolPalette& palette,
                                               SnowCanvasWidget& canvas, QObject* parent,
                                               Replicate replicate, Save save)
    : QObject(parent) {
    connect(&canvas, &SnowCanvasWidget::angleAdjustmentTargetChanged, &palette,
            &ScreenshotToolPalette::resetAngleWheelInput);
    connect(&palette, &ScreenshotToolPalette::angleValueAdjustmentRequested, this,
            [target = QPointer<SnowCanvasWidget>(&canvas)](int steps, bool fine) {
                if (target != nullptr)
                    static_cast<void>(target->adjustAngleValue(steps * (fine ? 0.1 : 1.0) *
                                                               std::numbers::pi / 180.0));
            });
    palette.setStyleEditHandler(
        [guard = QPointer<QObject>(this), target = QPointer<SnowCanvasWidget>(&canvas),
         source = QPointer<ScreenshotToolPalette>(&palette)](const SnowCanvasStyleEdit& edit) {
            if (guard == nullptr || target == nullptr)
                return false;
            if (target->commitStyleEdit(edit))
                return true;
            if (source != nullptr) {
                source->setStyleToolbarState(target->canvasStyleToolbarState());
                source->setWatermarkConfig(target->canvasWatermarkConfig());
                source->setSpotlightConfig(target->canvasSpotlightConfig());
            }
            qWarning() << "Unable to apply canvas style edit";
            return false;
        });
    connect(&canvas, &SnowCanvasWidget::styleEditCommitted, this,
            [this, source = QPointer<ScreenshotToolPalette>(&palette), replicate,
             save](const SnowCanvasStyleEdit& edit) {
                if (source == nullptr)
                    return;
                if (const auto* angle = std::get_if<SnowCanvasAngleStyleEdit>(&edit);
                    angle != nullptr && !angle->creationDefaults)
                    return;
                source->rememberStyleEdit(edit);
                if (replicate)
                    replicate(edit);
                m_lastSaveSucceeded = save ? save(edit) : persistScreenshotCanvasStyleEdit(edit);
                if (!*m_lastSaveSucceeded) {
                    qWarning() << "Unable to persist canvas style edit";
                }
            });
}

void replicateScreenshotStyleEdit(SnowCanvasWidget& peer, const SnowCanvasStyleEdit& edit) {
    // Document and creation styles are already shared by the screenshot runtime. Applying
    // a text patch again through an idle viewport would bypass an active draft's ownership.
    if (std::holds_alternative<SnowCanvasWatermarkEdit>(edit) ||
        std::holds_alternative<SnowCanvasSpotlightEdit>(edit)) {
        static_cast<void>(peer.applyStyleEdit(edit));
    }
}

bool stepScreenshotStyle(ScreenshotToolPalette& palette, SnowCanvasWidget& canvas, int direction,
                         bool fine) {
    if (direction == 0 || !canvas.interactionEnabled() || canvas.hasActiveTextEditing())
        return false;
    if (canvas.canvasTool() == SnowCanvasTool::Angle ||
        canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedAngle) {
        return canvas.adjustAngleValue(direction * (fine ? 0.1 : 1.0) * std::numbers::pi / 180.0);
    }
    switch (canvas.canvasTool()) {
    case SnowCanvasTool::Shape:
    case SnowCanvasTool::Distance:
    case SnowCanvasTool::Arrow:
    case SnowCanvasTool::Line:
    case SnowCanvasTool::FreeDraw:
    case SnowCanvasTool::RectangleHighlight:
    case SnowCanvasTool::PenHighlight:
        return palette.stepStrokeWidth(direction);
    case SnowCanvasTool::Select:
        return palette.stepSelectionOpacity(direction);
    case SnowCanvasTool::Spotlight:
        return palette.stepSpotlightOpacity(direction);
    case SnowCanvasTool::RectangleFilter:
    case SnowCanvasTool::AutoFilter:
        return palette.stepFilterIntensity(direction);
    case SnowCanvasTool::PenFilter:
        return palette.stepPenFilterStrokeWidth(direction);
    case SnowCanvasTool::BrushEraser:
        return palette.stepBrushEraserStrokeWidth(direction);
    case SnowCanvasTool::Watermark:
        return palette.stepWatermarkFontSize(direction);
    default:
        return false;
    }
}

bool handleScreenshotStyleWheel(ScreenshotToolPalette& palette, SnowCanvasWidget& canvas,
                                const QWheelEvent& event, WheelStepAccumulator& accumulator) {
    if (angleWheelTarget(canvas)) {
        if ((event.modifiers() & ~Qt::ShiftModifier) != Qt::NoModifier) {
            accumulator.reset();
            return false;
        }
        const int steps = accumulator.consume(event);
        if (steps != 0)
            static_cast<void>(stepScreenshotStyle(palette, canvas, steps,
                                                  event.modifiers().testFlag(Qt::ShiftModifier)));
        return true;
    }
    accumulator.reset();
    if (event.modifiers() != Qt::NoModifier)
        return false;
    const int delta =
        usesPreciseWheelDelta(event) ? event.pixelDelta().y() : event.angleDelta().y();
    return delta != 0 && stepScreenshotStyle(palette, canvas, delta > 0 ? 1 : -1);
}
} // namespace snow_shot::presentation
