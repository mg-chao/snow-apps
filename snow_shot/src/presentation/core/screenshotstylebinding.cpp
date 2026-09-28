#include "snow_shot/presentation/screenshotstylebinding.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include <QDebug>
#include <QPointer>

namespace snow_shot::presentation {
ScreenshotStyleBinding::ScreenshotStyleBinding(ScreenshotToolPalette& palette,
                                               SnowCanvasWidget& canvas, QObject* parent,
                                               Replicate replicate, Save save)
    : QObject(parent) {
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

bool stepScreenshotStyle(ScreenshotToolPalette& palette, SnowCanvasWidget& canvas, int direction) {
    if (direction == 0 || !canvas.interactionEnabled() || canvas.hasActiveTextEditing())
        return false;
    switch (canvas.canvasTool()) {
    case SnowCanvasTool::Shape:
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
    case SnowCanvasTool::Watermark:
        return palette.stepWatermarkFontSize(direction);
    default:
        return false;
    }
}
} // namespace snow_shot::presentation
