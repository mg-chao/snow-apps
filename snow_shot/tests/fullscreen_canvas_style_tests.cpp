#include "snow_shot/presentation/fullscreencanvasstylepanel.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "../src/presentation/tools/screenshottoolpalettebuttons.h"

#include "widgets/color_picker.h"
#include "widgets/input_number.h"
#include "widgets/popover.h"
#include "widgets/radio_button_group.h"
#include "widgets/select.h"
#include "widgets/slider.h"

#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTranslator>

#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

adqt::widgets::AdColorPicker* pickerNamed(QWidget& root, const char* name) {
    for (auto* picker : root.findChildren<adqt::widgets::AdColorPicker*>()) {
        if (picker->accessibleName() == QString::fromLatin1(name))
            return picker;
    }
    return nullptr;
}

class PanelTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) == "FullscreenCanvasStylePanel" && QByteArray(source) == "Stroke") {
            return QStringLiteral("Translated stroke");
        }
        return {};
    }
};

void mixedSelectionStyleEdits(SnowCanvasWidget& canvas,
                              snow_shot::presentation::FullscreenCanvasStylePanel& panel) {
    canvas.clearDocument();
    const auto pointer = [&canvas](QEvent::Type type, const QPointF& position,
                                   Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, canvas.mapToGlobal(position.toPoint()), button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    for (int index = 0; index < 2; ++index) {
        canvas.resetEditingStatePreservingTool();
        canvas.setCanvasTool(SnowCanvasTool::Shape);
        auto style = canvas.canvasStyleToolbarState().shapeStyle;
        style.stroke = index == 0 ? QColor(Qt::red) : QColor(Qt::blue);
        canvas.setCanvasShapeStylePatch(style, SnowCanvasShapeStylePropertyStrokeColor,
                                        SnowCanvasShapeKind::Rectangle);
        const QPointF start(40 + index * 180, 40);
        const QPointF end = start + QPointF(100, 80);
        pointer(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        pointer(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        pointer(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    }
    canvas.setCanvasTool(SnowCanvasTool::Select);
    pointer(QEvent::MouseButtonPress, QPointF(20, 20), Qt::LeftButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, QPointF(340, 140), Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseButtonRelease, QPointF(340, 140), Qt::LeftButton, Qt::NoButton);
    flushEvents();
    const auto mixed = canvas.canvasStyleToolbarState();
    require(mixed.selectedElementCount == 2 &&
                (mixed.shapeStyleMixed & SnowCanvasShapeStylePropertyStrokeColor) != 0,
            "different selected rectangle strokes must expose a mixed color");
    auto* picker = pickerNamed(panel, "Stroke color");
    require(picker != nullptr, "mixed rectangle selection must display the stroke editor");
    QWidget* editor = picker;
    while (editor && !editor->property("screenshotStyleEditorRoot").toBool())
        editor = editor->parentWidget();
    require(editor != nullptr, "stroke picker must belong to a shared editor");
    QVector<ColorSwatchButton*> swatches;
    for (auto* button : editor->findChildren<adqt::widgets::AdButton*>()) {
        auto* swatch = dynamic_cast<ColorSwatchButton*>(button);
        if (!swatch || dynamic_cast<ColorPickerTrigger*>(swatch))
            continue;
        require(swatch->buttonStyle() != adqt::widgets::AdButton::ButtonStyle::Tonal ||
                    swatch->accentRole() != adqt::widgets::AdButton::AccentRole::Primary,
                "mixed selection must not display any stroke preset as selected");
        swatches.push_back(swatch);
    }
    require(!swatches.isEmpty(), "mixed stroke editor must expose inline color presets");
    const QColor chosen(Qt::black);
    require(swatches.last()->toolTip().endsWith(chosen.name()),
            "the shared stroke palette must end with its black preset");
    swatches.last()->click();
    const auto uniform = canvas.canvasStyleToolbarState();
    require(uniform.selectedElementCount == 2 && uniform.shapeStyle.stroke == chosen &&
                (uniform.shapeStyleMixed & SnowCanvasShapeStylePropertyStrokeColor) == 0,
            "one inline swatch must apply the same stroke color to every selected rectangle");
}
} // namespace

// Called by the focused fullscreen-canvas test entrypoint.
void runFullscreenCanvasStylePanelTests() {
    const auto defaults = snow_shot::presentation::screenshotCanvasStyleDefaults();
    SnowCanvasRuntime runtime(SnowCanvasRuntimeConfig{defaults});
    require(runtime.isValid(), "fullscreen style tests require a valid canvas runtime");
    SnowCanvasWidget canvas(runtime);
    canvas.resize(640, 480);
    canvas.setCanvasTool(SnowCanvasTool::Shape);
    snow_shot::presentation::FullscreenCanvasStylePanel panel(canvas, defaults);
    panel.resize(260, 520);
    panel.show();
    flushEvents();

    auto* stroke = pickerNamed(panel, "Stroke color");
    require(stroke != nullptr && stroke->popupContent() == nullptr,
            "inline stroke pattern choices must not require a picker popup");
    QVector<StrokeStylePreviewButton*> patterns;
    for (auto* button : panel.findChildren<adqt::widgets::AdButton*>()) {
        if (auto* pattern = dynamic_cast<StrokeStylePreviewButton*>(button))
            patterns.push_back(pattern);
    }
    require(patterns.size() == 3, "inline shape styles expose all three stroke patterns eagerly");
    int changes = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &panel,
                     [&changes]() { ++changes; });
    patterns.last()->click();
    require(canvas.canvasStyleToolbarState().shapeStyle.strokeStyle ==
                SnowCanvasStrokeStyle::Dotted,
            "inline stroke pattern must update the engine");
    require(changes == 1, "one inline pattern click must apply one style change");
    require(panel.findChild<QWidget*>(QStringLiteral("screenshotFillColorPresets")) != nullptr,
            "fill color presets must be available without opening a popup");

    for (const auto tool :
         {SnowCanvasTool::Arrow, SnowCanvasTool::Line, SnowCanvasTool::FreeDraw,
          SnowCanvasTool::Text, SnowCanvasTool::RectangleHighlight, SnowCanvasTool::PenHighlight,
          SnowCanvasTool::SerialNumber, SnowCanvasTool::Watermark, SnowCanvasTool::Spotlight,
          SnowCanvasTool::Shape}) {
        canvas.setCanvasTool(tool);
        panel.setActiveTool(tool);
        flushEvents();
        require(panel.width() == 260, "style families must retain the sidebar width");
        for (auto* widget : panel.findChildren<QWidget*>()) {
            if (widget->property("screenshotStyleEditorRoot").toBool()) {
                // Fixed-width line edits and selects intentionally constrain their platform
                // size hints. Verify the effective layout size and the actual allocated size.
                const QSize effectiveHint = widget->sizeHint().boundedTo(widget->maximumSize());
                if (effectiveHint.width() > 228 || widget->width() > 228) {
                    throw std::runtime_error(
                        QStringLiteral("inline style editor must fit sidebar: tool=%1 role=%2 "
                                       "hint=%3x%4 actual=%5x%6")
                            .arg(static_cast<int>(tool))
                            .arg(widget->property("screenshotStyleEditorRole").toString())
                            .arg(widget->sizeHint().width())
                            .arg(widget->sizeHint().height())
                            .arg(widget->width())
                            .arg(widget->height())
                            .toStdString());
                }
                for (auto* button : widget->findChildren<adqt::widgets::AdButton*>()) {
                    if (!button->isVisibleTo(widget))
                        continue;
                    for (QWidget* child = button; child != widget; child = child->parentWidget()) {
                        auto* container = child->parentWidget();
                        require(container != nullptr &&
                                    container->rect().contains(child->geometry()),
                                "inline controls must fit each structural row without clipping");
                    }
                }
            }
        }
        if (tool == SnowCanvasTool::Arrow || tool == SnowCanvasTool::Text) {
            const QByteArray role =
                tool == SnowCanvasTool::Arrow ? "start-arrowhead" : "text-alignment";
            QWidget* options = nullptr;
            for (auto* widget : panel.findChildren<QWidget*>()) {
                if (widget->property("screenshotStyleEditorRole").toByteArray() == role) {
                    options = widget;
                    break;
                }
            }
            require(options != nullptr &&
                        options->findChildren<adqt::widgets::AdPopover*>().isEmpty(),
                    "arrowhead and text-alignment editors must not own option popovers");
            const auto buttons = options->findChildren<adqt::widgets::AdButton*>();
            require(buttons.size() == (tool == SnowCanvasTool::Arrow ? 14 : 3),
                    "all arrowhead and text-alignment choices must be built inline eagerly");
            buttons.last()->click();
            require(tool == SnowCanvasTool::Arrow
                        ? canvas.canvasStyleToolbarState().shapeStyle.startArrowhead ==
                              SnowCanvasArrowhead::CrowfootOneOrMany
                        : canvas.canvasStyleToolbarState().textStyle.horizontalAlign ==
                              SnowCanvasTextHorizontalAlign::Right,
                    "inline icon options must update the corresponding engine style");
        }
    }
    require(canvas.canvasStyleToolbarState().shapeStyle.strokeStyle ==
                SnowCanvasStrokeStyle::Dotted,
            "switching families must preserve edited creation styles");

    canvas.setCanvasTool(SnowCanvasTool::Text);
    panel.setActiveTool(SnowCanvasTool::Text);
    panel.resize(260, 80);
    flushEvents();
    auto* scroll = panel.findChild<QScrollArea*>(QStringLiteral("fullscreenCanvasStyleScroll"));
    require(panel.height() == 80 && scroll && scroll->verticalScrollBar()->maximum() > 0,
            "a short sidebar must scroll its natural-height controls within the available height");
    panel.resize(260, 520);

    canvas.setCanvasTool(SnowCanvasTool::Spotlight);
    panel.setActiveTool(SnowCanvasTool::Spotlight);
    auto* opacity = panel.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotSpotlightOpacitySlider"));
    require(opacity != nullptr, "spotlight must expose the shared mask-opacity slider");
    opacity->setValue(37);
    require(qFuzzyCompare(canvas.canvasSpotlightConfig().opacity, 0.37),
            "spotlight opacity must update the engine configuration");

    panel.setLaserActive(true);
    panel.setLaserStyle(QColor(Qt::green), 7.0, 1800);
    int laserChanges = 0;
    QObject::connect(&panel,
                     &snow_shot::presentation::FullscreenCanvasStylePanel::laserStyleChanged,
                     &panel, [&laserChanges](const QColor&, qreal, int) { ++laserChanges; });
    auto* width =
        panel.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("fullscreenLaserWidth"));
    auto* duration =
        panel.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("fullscreenLaserDuration"));
    require(width && duration && width->value() == 7.0 && duration->value() == 1800,
            "laser controls must reflect incoming session style");
    require(width->minimum() == 1 && width->maximum() == 20 && duration->minimum() == 100 &&
                duration->maximum() == 5000,
            "laser inputs must enforce their documented ranges");
    const auto history = canvas.canvasHistoryState();
    width->setValue(8.0);
    require(laserChanges == 1, "one laser style edit must emit one signal");
    require(canvas.canvasHistoryState().canUndo == history.canUndo &&
                canvas.canvasHistoryState().canRedo == history.canRedo,
            "laser style edits must not touch drawing history");
    panel.dismissPopups();
    panel.setLaserActive(false);
    panel.synchronize();

    const auto variantConnection = QObject::connect(
        &panel, &snow_shot::presentation::FullscreenCanvasStylePanel::toolVariantRequested, &canvas,
        [&canvas](SnowCanvasTool tool) { canvas.setCanvasTool(tool); });
    canvas.setCanvasTool(SnowCanvasTool::RectangleHighlight);
    for (const auto destination : {ScreenshotToolPalette::Tool::PenHighlight,
                                   ScreenshotToolPalette::Tool::RectangleHighlight}) {
        auto* selector =
            panel.findChild<QWidget*>(QStringLiteral("screenshotHighlightModeSelector"));
        auto* group =
            selector ? selector->findChild<adqt::widgets::AdRadioButtonGroup*>() : nullptr;
        const auto current = destination == ScreenshotToolPalette::Tool::PenHighlight
                                 ? ScreenshotToolPalette::Tool::RectangleHighlight
                                 : ScreenshotToolPalette::Tool::PenHighlight;
        require(group && group->checkedId() == static_cast<int>(current),
                "highlight variant selector must reflect the active family");
        auto* button = group->button(static_cast<int>(destination));
        require(button != nullptr, "highlight variant must expose the destination button");
        button->click();
        flushEvents();
        require(canvas.canvasTool() == (destination == ScreenshotToolPalette::Tool::PenHighlight
                                            ? SnowCanvasTool::PenHighlight
                                            : SnowCanvasTool::RectangleHighlight),
                "queued variant changes must safely rebuild the active highlight family");
    }
    QObject::disconnect(variantConnection);

    canvas.show();
    canvas.setCanvasTool(SnowCanvasTool::Text);
    panel.setActiveTool(SnowCanvasTool::Text);
    const QPointF position(200, 150);
    QMouseEvent press(QEvent::MouseButtonPress, position, position, position, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, position, position, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QApplication::sendEvent(&canvas, &release);
    QKeyEvent type(QEvent::KeyPress, Qt::Key_L, Qt::NoModifier, QStringLiteral("Label"));
    QApplication::sendEvent(&canvas, &type);
    require(canvas.hasActiveTextEditing(), "text sidebar fixture must begin an active draft");
    auto* textColor = pickerNamed(panel, "Text color");
    require(textColor != nullptr, "text sidebar must expose a custom color picker");
    textColor->setPopupVisible(true);
    QFocusEvent focusOut(QEvent::FocusOut, Qt::PopupFocusReason);
    QApplication::sendEvent(&canvas, &focusOut);
    require(canvas.hasActiveTextEditing(), "opening a text color picker must preserve the draft");
    textColor->setPopupVisible(false);
    auto* font = panel.findChild<adqt::widgets::AdSelect*>();
    require(font != nullptr, "text sidebar must expose the shared font selector");
    font->showPopup();
    QApplication::sendEvent(&canvas, &focusOut);
    require(canvas.hasActiveTextEditing(), "opening the font selector must preserve the draft");
    font->hidePopup();
    QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
    QApplication::sendEvent(&canvas, &commit);
    require(!canvas.hasActiveTextEditing() && canvas.canvasHistoryState().canUndo,
            "text draft must commit to drawing history after the style popups close");
    canvas.setCanvasTool(SnowCanvasTool::Select);
    panel.setActiveTool(SnowCanvasTool::Select);
    // Changing tools clears selection, so select the committed label explicitly.
    QMouseEvent textSelectionPress(QEvent::MouseButtonPress, QPointF(80, 80), QPointF(80, 80),
                                   QPointF(80, 80), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent textSelectionMove(QEvent::MouseMove, QPointF(400, 250), QPointF(400, 250),
                                  QPointF(400, 250), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent textSelectionRelease(QEvent::MouseButtonRelease, QPointF(400, 250),
                                     QPointF(400, 250), QPointF(400, 250), Qt::LeftButton,
                                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &textSelectionPress);
    QApplication::sendEvent(&canvas, &textSelectionMove);
    QApplication::sendEvent(&canvas, &textSelectionRelease);
    require(canvas.canvasStyleToolbarState().selectedElementCount == 1 &&
                panel.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls")) != nullptr,
            "selection mode must follow the selected text's style family");
    auto* selectedOpacity = panel.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("fullscreenSelectionOpacitySlider"));
    require(selectedOpacity && selectedOpacity->isEnabled(),
            "selection opacity must be available for selected text");
    selectedOpacity->setValue(42);
    require(qFuzzyCompare(canvas.canvasStyleToolbarState().textStyle.opacity, 0.42),
            "selection opacity must update the selected element");
    mixedSelectionStyleEdits(canvas, panel);
    canvas.clearDocument();
    canvas.setCanvasTool(SnowCanvasTool::Shape);
    panel.setActiveTool(SnowCanvasTool::Shape);
    PanelTranslator translator;
    QApplication::installTranslator(&translator);
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(&panel, &languageChange);
    bool translated = false;
    for (auto* label : panel.findChildren<QLabel*>()) {
        translated = translated || label->text() == QStringLiteral("Translated stroke");
    }
    require(translated, "sidebar section labels must update on LanguageChange");
    QApplication::removeTranslator(&translator);
    QApplication::sendEvent(&panel, &languageChange);

    ScreenshotToolPalette compact(ScreenshotToolPalette::Options{});
    compact.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    auto* compactStroke = pickerNamed(compact, "Stroke color");
    require(compactStroke != nullptr && compactStroke->popupContent() == nullptr,
            "compact screenshot stroke options must stay lazy");
    for (auto* button : compact.findChildren<adqt::widgets::AdButton*>()) {
        require(dynamic_cast<StrokeStylePreviewButton*>(button) == nullptr,
                "compact screenshot presentation must not expose inline stroke options");
    }
}
