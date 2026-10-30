#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include <QKeyEvent>
#include <QInputMethodEvent>
#include "widgets/button.h"
#include "widgets/input_line_edit.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/slider.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QCursor>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScopeGuard>
#include <QTimeZone>
#include <QToolButton>
#include <limits>
#include <memory>
#include <cstdlib>
#include <iostream>

void runScreenshotSelectedFilterStyleTests();

namespace {
using namespace snow_shot::presentation;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}
ScreenshotToolPalette::Options options() {
    ScreenshotToolPalette::Options result;
    result.showTextTool = true;
    result.showSerialNumberTool = true;
    result.showFilterTool = true;
    result.showWatermarkTool = true;
    result.showSpotlightTool = true;
    result.styleDefaults = screenshotCanvasToolStyleDefaults();
    return result;
}
class StyleBindingCanvas final : public SnowCanvasWidget {
  public:
    int angleAdjustmentConnectionCount() const {
        return receivers(SIGNAL(angleAdjustmentTargetChanged()));
    }
};
void styleBindingsReleaseConnectionsForPooledEditors() {
    StyleBindingCanvas canvas;
    ScreenshotToolPalette palette(options());
    const int originalConnections = canvas.angleAdjustmentConnectionCount();
    for (int capture = 0; capture < 160; ++capture) {
        {
            ScreenshotStyleBinding binding(palette, canvas, nullptr);
            require(canvas.angleAdjustmentConnectionCount() == originalConnections + 1,
                    "each capture binding owns exactly one angle adjustment connection");
        }
        require(canvas.angleAdjustmentConnectionCount() == originalConnections,
                "ending capture releases angle adjustment connections while editors stay pooled");
    }
}
void styleBindingsTolerateEditorDestruction() {
    {
        SnowCanvasWidget canvas;
        auto palette = std::make_unique<ScreenshotToolPalette>(options());
        ScreenshotStyleBinding binding(*palette, canvas, nullptr);
        palette.reset();
        canvas.angleAdjustmentTargetChanged();
    }
    {
        ScreenshotToolPalette palette(options());
        auto canvas = std::make_unique<SnowCanvasWidget>();
        ScreenshotStyleBinding binding(palette, *canvas, nullptr);
        canvas.reset();
        palette.angleValueAdjustmentRequested(1, false);
    }
}
struct Editor {
    SnowCanvasWidget canvas;
    ScreenshotToolPalette palette{options()};
    ScreenshotStyleBinding binding{palette, canvas, &palette};
    Editor() {
        applyScreenshotCanvasToolStyles(canvas, palette.creationStyleDefaults());
        canvas.resize(400, 300);
        canvas.setInteractionEnabled(true);
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette, [this] {
            palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
        });
    }
    void text() {
        require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text tool");
        palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    }
};
void wheel(SnowCanvasWidget& canvas, int delta) {
    const QPointF point(100, 100);
    QWheelEvent event(point, canvas.mapToGlobal(point.toPoint()), QPoint(), QPoint(0, delta),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &event);
}
void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}
QJsonArray documentSlots(const SnowCanvasRuntime& runtime) {
    return QJsonDocument::fromJson(runtime.serializeDocumentSession())
        .object()
        .value(QStringLiteral("document"))
        .toObject()
        .value(QStringLiteral("slots"))
        .toArray();
}
void arrowCreationDefaultsRestoreShaftAndRatio() {
    SnowCanvasWidget canvas;
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.arrow.arrowRatio = 2.7;
    defaults.arrow.arrowOutlineWidth = 8.0;
    defaults.arrow.arrowOutlineColor = QColor(4, 100, 20, 120);
    defaults.arrow.startArrowhead = SnowCanvasArrowhead::Circle;
    defaults.arrow.endArrowhead = SnowCanvasArrowhead::IndentedTriangle;
    for (const auto arrowType :
         {SnowCanvasArrowType::Straight, SnowCanvasArrowType::Curve, SnowCanvasArrowType::Elbow}) {
        defaults.arrow.arrowType = arrowType;
        for (const auto shaftType :
             {SnowCanvasArrowShaftType::Tapered, SnowCanvasArrowShaftType::Plain}) {
            defaults.arrow.arrowShaftType = shaftType;
            applyScreenshotCanvasToolStyles(canvas, defaults);
            require(canvas.setCanvasTool(SnowCanvasTool::Arrow), "inspect restored arrow defaults");
            const auto style = canvas.canvasStyleToolbarState().shapeStyle;
            require(style.arrowShaftType == shaftType &&
                        style.arrowRatio == defaults.arrow.arrowRatio &&
                        style.arrowOutlineWidth == defaults.arrow.arrowOutlineWidth &&
                        style.arrowOutlineColor == defaults.arrow.arrowOutlineColor &&
                        style.arrowType == arrowType &&
                        style.startArrowhead == defaults.arrow.startArrowhead &&
                        style.endArrowhead == defaults.arrow.endArrowhead,
                    "arrow creation defaults must restore shaft and ratio independently of type "
                    "and arrowhead compatibility");
        }
    }
}
void creationDefaultsDoNotActivateFilterTools() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "prepare screenshot selection tool");
    const QCursor cursor = canvas.cursor();
    const auto viewport = canvas.viewportId();
    const auto revision = runtime.documentRevision();
    int activeToolChanges = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::activeToolChanged, &canvas,
                     [&] { ++activeToolChanges; });
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.rectangleFilter = {SnowCanvasFilterType::GaussianBlur, 0.37, 0.61, 3};
    defaults.penFilter = {SnowCanvasFilterType::Brightness, 0.37, 0.83, 21};
    for (int capture = 0; capture < 8; ++capture) {
        applyScreenshotCanvasToolStyles(canvas, defaults);
        require(activeToolChanges == 0 && canvas.canvasTool() == SnowCanvasTool::Select &&
                    canvas.cursor() == cursor,
                "repeated screenshot defaults must not activate tools or allocate brush cursors");
        require(canvas.viewportId() == viewport && runtime.documentRevision() == revision &&
                    !runtime.canUndo(),
                "creation defaults must preserve the viewport and empty document history");
    }
    require(canvas.setCanvasTool(SnowCanvasTool::RectangleFilter), "inspect rectangle defaults");
    require(canvas.canvasStyleToolbarState().filterStyle == defaults.rectangleFilter,
            "rectangle filter creation style must apply without activating it during setup");
    require(canvas.setCanvasTool(SnowCanvasTool::PenFilter), "inspect pen defaults");
    require(canvas.canvasStyleToolbarState().filterStyle == defaults.penFilter,
            "pen filter creation style must apply independently of rectangle defaults");
}
void creationDefaultsPreserveDocumentAndEditingCleanup() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(320, 240);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(160, 120, 1), "set style restoration fixture camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "draw style restoration fixture");
    mouse(canvas, QEvent::MouseButtonPress, QPointF(40, 40));
    mouse(canvas, QEvent::MouseMove, QPointF(140, 100));
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(140, 100));
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select style restoration fixture");
    mouse(canvas, QEvent::MouseButtonPress, QPointF(40, 60));
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(40, 60));
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedRectangle,
            "fixture rectangle must be selected before defaults refresh");
    const auto originalSlots = documentSlots(runtime);
    const auto history = runtime.serializeDocumentHistory();
    const auto revision = runtime.documentRevision();
    require(!originalSlots.isEmpty(), "style restoration fixture must contain a document element");
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.rectangle.stroke = QColor(19, 47, 89);
    defaults.rectangle.strokeWidth = 12;
    applyScreenshotCanvasToolStyles(canvas, defaults);
    require(documentSlots(runtime) == originalSlots &&
                runtime.serializeDocumentHistory() == history &&
                runtime.documentRevision() == revision,
            "refreshing creation styles must never rewrite selected elements or their history");
    require(canvas.canvasStyleToolbarState().source !=
                SnowCanvasStyleToolbarSource::SelectedRectangle,
            "creation style restoration retains its existing selection cleanup");
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "start pending text before style refresh");
    mouse(canvas, QEvent::MouseButtonPress, QPointF(180, 150));
    QKeyEvent type(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("pending"));
    QApplication::sendEvent(&canvas, &type);
    require(canvas.hasActiveTextEditing(), "style restoration fixture must have pending text");
    QList<SnowCanvasTool> observedTools;
    QObject::connect(&canvas, &SnowCanvasWidget::activeToolChanged, &canvas,
                     [&] { observedTools.append(canvas.canvasTool()); });
    applyScreenshotCanvasToolStyles(canvas, defaults);
    require(!canvas.hasActiveTextEditing() && canvas.canvasTool() == SnowCanvasTool::Text &&
                canvas.cursor().shape() == Qt::IBeamCursor &&
                documentSlots(runtime) != originalSlots,
            "creation style restoration must commit pending text and restore its active tool");
    require(!observedTools.contains(SnowCanvasTool::RectangleFilter) &&
                !observedTools.contains(SnowCanvasTool::PenFilter),
            "restoring a non-filter tool must not transiently activate either filter");
}
void serialNumberAppearanceDefaultsPreserveSessionSequences() {
    const SnowCanvasSerialNumberNumericType types[] = {
        SnowCanvasSerialNumberNumericType::Arabic,
        SnowCanvasSerialNumberNumericType::Roman,
        SnowCanvasSerialNumberNumericType::LowercaseLetters,
        SnowCanvasSerialNumberNumericType::UppercaseLetters,
        SnowCanvasSerialNumberNumericType::Chinese,
    };
    for (const bool explicitStart : {false, true}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(800, 600);
        canvas.show();
        auto defaults = screenshotCanvasToolStyleDefaults();
        defaults.serialNumber.type = SnowCanvasSerialNumberType::OutlinedCircle;
        defaults.serialNumber.numericType = SnowCanvasSerialNumberNumericType::Arabic;
        defaults.serialNumber.number = 1;
        defaults.serialNumber.fontSize = 24;
        applyScreenshotCanvasToolStyles(canvas, defaults);
        require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial creation");
        const auto create = [&](QPointF point) {
            mouse(canvas, QEvent::MouseButtonPress, point);
            mouse(canvas, QEvent::MouseButtonRelease, point);
            return documentSlots(runtime)
                .last()
                .toObject()
                .value(QStringLiteral("data"))
                .toObject()
                .value(QStringLiteral("SerialNumber"))
                .toObject()
                .value(QStringLiteral("number"))
                .toVariant()
                .toLongLong();
        };
        for (int index = 0; index < 5; ++index) {
            auto style = canvas.canvasStyleToolbarState().serialNumberStyle;
            style.numericType = types[index];
            style.number = 10 + index;
            require(canvas.applyStyleEdit(SnowCanvasSerialNumberEdit{
                        style, SnowCanvasSerialNumberStyleMixedNumericType |
                                   (explicitStart ? SnowCanvasSerialNumberStyleMixedNumber : 0)}),
                    "prepare independent automatic or explicit sequences");
            const qint64 start = explicitStart ? 10 + index : 1;
            require(create({80.0 + index * 140.0, 100}) == start &&
                        create({80.0 + index * 140.0, 250}) == start + 1,
                    "each numeric type creates two sequential badges");
        }
        for (int index = 0; index < 5; ++index) {
            const auto annotationsBeforeRefresh = documentSlots(runtime);
            const auto history = runtime.serializeDocumentHistory();
            const auto revision = runtime.documentRevision();
            defaults.serialNumber.numericType = types[index];
            defaults.serialNumber.color = QColor(19, 47, 89);
            defaults.serialNumber.fontSize = 32;
            applyScreenshotCanvasToolStyles(canvas, defaults);
            auto expected = defaults.serialNumber;
            expected.number = explicitStart ? 12 + index : 3;
            require(canvas.canvasTool() == SnowCanvasTool::SerialNumber &&
                        canvas.canvasStyleToolbarState().serialNumberStyle == expected,
                    "reopening creation appearance restores the format's session counter");
            require(documentSlots(runtime) == annotationsBeforeRefresh &&
                        runtime.serializeDocumentHistory() == history &&
                        runtime.documentRevision() == revision,
                    "appearance refresh preserves all existing annotations and history");
            require(create({80.0 + index * 140.0, 400}) == expected.number,
                    "creation continues after appearance refresh without duplicate numbers");
            require(runtime.undo(), "undo creation after reopening appearance");
            require(canvas.canvasStyleToolbarState().serialNumberStyle.number ==
                        expected.number + (explicitStart ? 1 : 0),
                    "appearance refresh preserves automatic and explicit history behavior");
            require(runtime.redo() && canvas.canvasStyleToolbarState().serialNumberStyle.number ==
                                          expected.number + 1,
                    "redo restores the next number without changing sequence ownership");
        }
    }
}
void selectedSerialNumberToolbarEditsContinueCreationSequence() {
    for (int editPath = 0; editPath < 3; ++editPath) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        ScreenshotToolPalette palette(options());
        ScreenshotStyleBinding binding(palette, canvas, &palette, {},
                                       [](const SnowCanvasStyleEdit&) { return true; });
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                         [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
        auto defaults = screenshotCanvasToolStyleDefaults();
        defaults.serialNumber = SnowCanvasSerialNumberStyle{};
        applyScreenshotCanvasToolStyles(canvas, defaults);
        canvas.resize(800, 600);
        canvas.show();
        require(canvas.setViewportCamera(400, 300, 1) &&
                    canvas.setCanvasTool(SnowCanvasTool::SerialNumber),
                "prepare sequence toolbar fixture");
        const auto creationStyle = canvas.canvasStyleToolbarState().serialNumberStyle;
        palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
        palette.show();
        QApplication::processEvents();
        for (const QPointF point : {QPointF(80, 80), QPointF(250, 80), QPointF(420, 80)}) {
            mouse(canvas, QEvent::MouseButtonPress, point);
            mouse(canvas, QEvent::MouseButtonRelease, point);
        }
        const QPointF point(80, 80);
        QMouseEvent press(QEvent::MouseButtonPress, point, canvas.mapToGlobal(point.toPoint()),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, canvas.mapToGlobal(point.toPoint()),
                            Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &press);
        QApplication::sendEvent(&canvas, &release);
        require(canvas.canvasStyleToolbarState().source ==
                    SnowCanvasStyleToolbarSource::SelectedSerialNumber,
                "select the first sequence number for editing");
        qint64 editedNumber = 0;
        if (editPath < 2) {
            const QString name = editPath == 0
                                     ? QStringLiteral("snowSerialNumberToolbarIncreaseButton")
                                     : QStringLiteral("snowSerialNumberToolbarDecreaseButton");
            auto* button = canvas.findChild<QToolButton*>(name);
            require(button != nullptr && button->isVisible() && button->isEnabled(),
                    "floating sequence toolbar exposes an enabled adjustment button");
            button->click();
            editedNumber = editPath == 0 ? 2 : 0;
        } else {
            adqt::widgets::AdLineEdit* input = nullptr;
            for (auto* candidate : palette.findChildren<adqt::widgets::AdLineEdit*>()) {
                if (candidate->toolTip() == QStringLiteral("Sequence number (scroll to adjust)")) {
                    input = candidate;
                    break;
                }
            }
            require(input != nullptr, "sequence style toolbar exposes its value editor");
            input->setText(QStringLiteral("55"));
            require(QMetaObject::invokeMethod(input, "editingFinished", Qt::DirectConnection),
                    "commit the selected sequence value from its input");
            editedNumber = 55;
        }
        require(canvas.canvasStyleToolbarState().serialNumberStyle.number == editedNumber,
                "toolbar edit updates the selected annotation");
        require(canvas.resetEditingState() && canvas.setCanvasTool(SnowCanvasTool::SerialNumber),
                "return to sequence creation after editing");
        auto expected = creationStyle;
        expected.number = editedNumber + 1;
        require(canvas.canvasStyleToolbarState().serialNumberStyle == expected &&
                    palette.creationStyleDefaults().serialNumber == expected,
                "canvas and style toolbar continue from the edited number");
        mouse(canvas, QEvent::MouseButtonPress, {600, 300});
        mouse(canvas, QEvent::MouseButtonRelease, {600, 300});
        require(documentSlots(runtime)
                        .last()
                        .toObject()
                        .value(QStringLiteral("data"))
                        .toObject()
                        .value(QStringLiteral("SerialNumber"))
                        .toObject()
                        .value(QStringLiteral("number"))
                        .toVariant()
                        .toLongLong() == editedNumber + 1,
                "the next created annotation uses the successor of the toolbar value");
    }
}

void selectedSerialNumberSizeEditsUpdateCreationAppearance() {
    const auto original = screenshotCanvasToolStyleDefaults();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(persistScreenshotCanvasToolStyles(original)); });
    for (int editPath = 0; editPath < 3; ++editPath) {
        auto defaults = original;
        defaults.serialNumber.fontSize = 24;
        require(persistScreenshotCanvasToolStyles(defaults), "save sequence size fixture defaults");
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        auto settings = options();
        settings.styleDefaults = defaults;
        ScreenshotToolPalette palette(settings);
        int saves = 0;
        int replications = 0;
        ScreenshotStyleBinding binding(
            palette, canvas, &palette, [&](const SnowCanvasStyleEdit&) { ++replications; },
            [&](const SnowCanvasStyleEdit& edit) {
                ++saves;
                return persistScreenshotCanvasStyleEdit(edit);
            });
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                         [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
        auto olderAppearance = defaults;
        olderAppearance.serialNumber.color = QColor(17, 29, 43);
        olderAppearance.serialNumber.fill = QColor(50, 60, 70, 128);
        olderAppearance.serialNumber.strokeWidth = 7;
        olderAppearance.serialNumber.fontFamily = QStringLiteral("Older badge font");
        applyScreenshotCanvasToolStyles(canvas, olderAppearance);
        canvas.resize(400, 300);
        canvas.show();
        require(canvas.setViewportCamera(200, 150, 1) &&
                    canvas.setCanvasTool(SnowCanvasTool::SerialNumber),
                "prepare selected sequence size fixture");
        palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
        mouse(canvas, QEvent::MouseButtonPress, {80, 80});
        mouse(canvas, QEvent::MouseButtonRelease, {80, 80});
        applyScreenshotCanvasToolStyles(canvas, defaults);
        palette.setCreationStyleDefaults(defaults);
        palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
        palette.show();
        QApplication::processEvents();
        const QPointF point(80, 80);
        QMouseEvent press(QEvent::MouseButtonPress, point, canvas.mapToGlobal(point.toPoint()),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, canvas.mapToGlobal(point.toPoint()),
                            Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &press);
        QApplication::sendEvent(&canvas, &release);
        require(canvas.canvasStyleToolbarState().source ==
                    SnowCanvasStyleToolbarSource::SelectedSerialNumber,
                "select an older badge whose untouched appearance differs from creation defaults");
        double expectedSize = 25;
        if (editPath == 0) {
            wheel(canvas, 120);
        } else if (editPath == 1) {
            adqt::widgets::AdButton* preset = nullptr;
            for (auto* button : palette.findChildren<adqt::widgets::AdButton*>()) {
                if (button->toolTip() == QStringLiteral("Sequence number font size 42px"))
                    preset = button;
            }
            require(preset != nullptr && preset->isEnabled(), "sequence size preset exists");
            preset->click();
            expectedSize = 42;
        } else {
            auto selected = canvas.canvasStyleToolbarState().serialNumberStyle;
            selected.fontSize = 37;
            require(canvas.commitStyleEdit(SnowCanvasSerialNumberEdit{
                        selected, SnowCanvasSerialNumberStyleMixedFontSize}),
                    "commit a direct selected sequence size edit");
            expectedSize = 37;
        }
        auto expectedSelected = olderAppearance.serialNumber;
        expectedSelected.number = 1;
        expectedSelected.fontSize = expectedSize;
        require(canvas.canvasStyleToolbarState().serialNumberStyle == expectedSelected &&
                    saves == 1 && replications == 1,
                "selected size paths apply and save exactly one property edit");
        auto expectedSaved = defaults;
        expectedSaved.serialNumber.fontSize = expectedSize;
        require(screenshotCanvasToolStyleDefaults() == expectedSaved,
                "selected size edits persist only font size without the sequence counter");
        auto expectedCreation = expectedSaved;
        expectedCreation.serialNumber.number = 2;
        require(palette.creationStyleDefaults() == expectedCreation && canvas.resetEditingState() &&
                    canvas.setCanvasTool(SnowCanvasTool::SerialNumber) &&
                    canvas.canvasStyleToolbarState().serialNumberStyle ==
                        expectedCreation.serialNumber,
                "selected size edits preserve every untouched creation property and sequence");
        mouse(canvas, QEvent::MouseButtonPress, {250, 180});
        mouse(canvas, QEvent::MouseButtonRelease, {250, 180});
        const auto created = documentSlots(runtime)
                                 .last()
                                 .toObject()
                                 .value(QStringLiteral("data"))
                                 .toObject()
                                 .value(QStringLiteral("SerialNumber"))
                                 .toObject();
        require(created.value(QStringLiteral("font_size")).toDouble() == expectedSize &&
                    created.value(QStringLiteral("number")).toInt() == 2,
                "the subsequent badge uses the selected size edit and the next sequence number");
    }
}

void selectedSerialNumberEditsUpdateCreationAppearance() {
    constexpr quint32 properties =
        SnowCanvasSerialNumberStyleMixedNumber | SnowCanvasSerialNumberStyleMixedColor |
        SnowCanvasSerialNumberStyleMixedFill | SnowCanvasSerialNumberStyleMixedFillStyle |
        SnowCanvasSerialNumberStyleMixedFontSize | SnowCanvasSerialNumberStyleMixedFontFamily |
        SnowCanvasSerialNumberStyleMixedStrokeWidth | SnowCanvasSerialNumberStyleMixedStrokeStyle |
        SnowCanvasSerialNumberStyleMixedOpacity | SnowCanvasSerialNumberStyleMixedType |
        SnowCanvasSerialNumberStyleMixedNumericType;
    const auto originalDefaults = screenshotCanvasToolStyleDefaults();
    const auto restore = qScopeGuard(
        [&] { static_cast<void>(persistScreenshotCanvasToolStyles(originalDefaults)); });
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    ScreenshotToolPalette palette(options());
    int saves = 0;
    int replications = 0;
    ScreenshotStyleBinding binding(
        palette, canvas, &palette, [&](const SnowCanvasStyleEdit&) { ++replications; },
        [&](const SnowCanvasStyleEdit& edit) {
            ++saves;
            return persistScreenshotCanvasStyleEdit(edit);
        });
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    auto defaults = originalDefaults;
    defaults.serialNumber = SnowCanvasSerialNumberStyle{};
    require(persistScreenshotCanvasToolStyles(defaults), "save selected sequence style defaults");
    palette.setCreationStyleDefaults(defaults);
    applyScreenshotCanvasToolStyles(canvas, defaults);
    canvas.resize(400, 300);
    canvas.show();
    require(canvas.setViewportCamera(200, 150, 1) &&
                canvas.setCanvasTool(SnowCanvasTool::SerialNumber),
            "prepare selected serial number style fixture");
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    palette.show();
    QApplication::processEvents();
    mouse(canvas, QEvent::MouseButtonPress, {80, 80});
    mouse(canvas, QEvent::MouseButtonRelease, {80, 80});
    const auto creationDefaults = palette.creationStyleDefaults();
    const auto configuration =
        snow_shot::storage::ApplicationStorage::instance().configuration().snapshot();
    const QPointF point(80, 80);
    QMouseEvent press(QEvent::MouseButtonPress, point, canvas.mapToGlobal(point.toPoint()),
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, point, canvas.mapToGlobal(point.toPoint()),
                        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QApplication::sendEvent(&canvas, &release);
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedSerialNumber,
            "fixture sequence number is selected");
    adqt::widgets::AdLineEdit* input = nullptr;
    for (auto* candidate : palette.findChildren<adqt::widgets::AdLineEdit*>()) {
        if (candidate->toolTip() == QStringLiteral("Sequence number (scroll to adjust)")) {
            input = candidate;
            break;
        }
    }
    require(input != nullptr, "selected sequence number exposes its number editor");
    input->setText(QStringLiteral("55"));
    require(QMetaObject::invokeMethod(input, "editingFinished", Qt::DirectConnection) &&
                canvas.canvasStyleToolbarState().serialNumberStyle.number == 55,
            "number editor updates the selected sequence number");
    require(saves == 0 && replications == 0 &&
                snow_shot::storage::ApplicationStorage::instance().configuration().snapshot() ==
                    configuration,
            "selected number edits continue the session without persisting a counter");
    auto selected = canvas.canvasStyleToolbarState().serialNumberStyle;
    selected.type = SnowCanvasSerialNumberType::OutlinedSquare;
    selected.numericType = SnowCanvasSerialNumberNumericType::Roman;
    selected.color = QColor(17, 29, 43);
    selected.fill = QColor(50, 60, 70, 128);
    selected.fillStyle = SnowCanvasFillStyle::CrossLine;
    selected.fontSize = 40;
    selected.fontFamily = QStringLiteral("Selected font");
    selected.strokeWidth = 3;
    selected.strokeStyle = SnowCanvasStrokeStyle::Dashed;
    selected.opacity = 0.5;
    require(canvas.commitStyleEdit(SnowCanvasSerialNumberEdit{selected, properties}) &&
                canvas.canvasStyleToolbarState().serialNumberStyle == selected,
            "selected sequence number accepts appearance and format edits");
    auto expectedDefaults = creationDefaults;
    snowCanvasMergeStyleEdit(expectedDefaults, SnowCanvasSerialNumberEdit{selected, properties});
    auto expectedSaved = expectedDefaults;
    expectedSaved.serialNumber.number = 1;
    require(saves == 1 && replications == 1 &&
                palette.creationStyleDefaults() == expectedDefaults &&
                screenshotCanvasToolStyleDefaults() == expectedSaved,
            "selected sequence appearance edits update and persist creation preferences");
    const auto appearanceConfiguration =
        snow_shot::storage::ApplicationStorage::instance().configuration().snapshot();
    require(persistScreenshotCanvasStyleEdit(
                SnowCanvasSerialNumberEdit{selected, SnowCanvasSerialNumberStyleMixedNumber}) &&
                snow_shot::storage::ApplicationStorage::instance().configuration().snapshot() ==
                    appearanceConfiguration,
            "direct number-only persistence preserves stored appearance and omits the counter");
    require(canvas.resetEditingState(), "leave sequence number selection");
    for (int repeat = 0; repeat < 2; ++repeat) {
        require(canvas.undo(), "undo selected sequence number styling");
    }
    for (int repeat = 0; repeat < 2; ++repeat) {
        require(canvas.redo(), "redo selected sequence number styling");
    }
    auto expectedCreation = expectedDefaults.serialNumber;
    expectedCreation.number = 1;
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber) &&
                canvas.canvasStyleToolbarState().serialNumberStyle == expectedCreation,
            "selected appearance carries forward while each format retains its own sequence");
    mouse(canvas, QEvent::MouseButtonPress, {250, 180});
    mouse(canvas, QEvent::MouseButtonRelease, {250, 180});
    require(documentSlots(runtime)
                    .last()
                    .toObject()
                    .value(QStringLiteral("data"))
                    .toObject()
                    .value(QStringLiteral("SerialNumber"))
                    .toObject()
                    .value(QStringLiteral("number"))
                    .toInt() == expectedCreation.number,
            "new Roman sequence number uses its independent creation counter");
    auto creation = canvas.canvasStyleToolbarState().serialNumberStyle;
    auto switched = creation;
    switched.numericType = SnowCanvasSerialNumberNumericType::Arabic;
    require(canvas.applyStyleEdit(SnowCanvasSerialNumberEdit{
                switched, SnowCanvasSerialNumberStyleMixedNumericType}) &&
                canvas.canvasStyleToolbarState().serialNumberStyle.number == 56,
            "the original Arabic format continues after its selected value edit");
    switched.numericType = SnowCanvasSerialNumberNumericType::Roman;
    require(canvas.applyStyleEdit(SnowCanvasSerialNumberEdit{
                switched, SnowCanvasSerialNumberStyleMixedNumericType}) &&
                canvas.canvasStyleToolbarState().serialNumberStyle == creation &&
                creation.number == 2 && saves == 1 && replications == 1,
            "restoring Roman retains its next counter without persisting programmatic switches");
    creation.fontSize = 32;
    require(canvas.commitStyleEdit(
                SnowCanvasSerialNumberEdit{creation, SnowCanvasSerialNumberStyleMixedFontSize}) &&
                saves == 2 && replications == 2 &&
                palette.creationStyleDefaults().serialNumber.fontSize == 32 &&
                screenshotCanvasToolStyleDefaults().serialNumber.fontSize == 32,
            "editing creation styles still remembers and saves future preferences");
}
void newScreenshotDocumentRestartsSerialNumberSequences() {
    const SnowCanvasSerialNumberNumericType types[] = {
        SnowCanvasSerialNumberNumericType::Arabic,
        SnowCanvasSerialNumberNumericType::Roman,
        SnowCanvasSerialNumberNumericType::LowercaseLetters,
        SnowCanvasSerialNumberNumericType::UppercaseLetters,
        SnowCanvasSerialNumberNumericType::Chinese,
    };
    for (const bool explicitStart : {false, true}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        ScreenshotToolPalette palette(options());
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                         [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
        canvas.resize(800, 600);
        canvas.show();
        auto defaults = screenshotCanvasToolStyleDefaults();
        defaults.serialNumber.type = SnowCanvasSerialNumberType::OutlinedCircle;
        defaults.serialNumber.fontSize = 32;
        applyScreenshotCanvasToolStyles(canvas, defaults);
        require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber),
                "activate screenshot numbering");
        palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
        const auto viewport = canvas.viewportId();
        for (int capture = 0; capture < 3; ++capture) {
            if (capture > 0) {
                // Capture teardown reuses the runtime and its attached canvas.
                require(runtime.clearDocumentPreservingViewports(), "clear completed capture");
                applyScreenshotCanvasToolStyles(canvas, defaults);
                require(canvas.viewportId() == viewport && documentSlots(runtime).isEmpty() &&
                            !runtime.canUndo() && !runtime.canRedo(),
                        "new capture reuses its viewport with an empty document and history");
            }
            require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber),
                    "reactivate numbering in the new capture");
            for (int index = 0; index < 5; ++index) {
                auto style = canvas.canvasStyleToolbarState().serialNumberStyle;
                style.numericType = types[index];
                style.number = 10 + index;
                require(canvas.applyStyleEdit(SnowCanvasSerialNumberEdit{
                            style, SnowCanvasSerialNumberStyleMixedNumericType |
                                       (capture == 0 && explicitStart
                                            ? SnowCanvasSerialNumberStyleMixedNumber
                                            : 0)}),
                        "switch screenshot numbering format");
                auto expected = defaults.serialNumber;
                expected.numericType = types[index];
                expected.number = capture == 0 && explicitStart ? style.number : 1;
                require(
                    canvas.canvasStyleToolbarState().serialNumberStyle == expected &&
                        palette.creationStyleDefaults().serialNumber == expected,
                    "new capture resets every format and its toolbar while retaining appearance");
                const QPointF point(80.0 + index * 140.0, 100);
                mouse(canvas, QEvent::MouseButtonPress, point);
                mouse(canvas, QEvent::MouseButtonRelease, point);
                const qint64 created = documentSlots(runtime)
                                           .last()
                                           .toObject()
                                           .value(QStringLiteral("data"))
                                           .toObject()
                                           .value(QStringLiteral("SerialNumber"))
                                           .toObject()
                                           .value(QStringLiteral("number"))
                                           .toVariant()
                                           .toLongLong();
                require(created == expected.number,
                        "new capture creates a badge at its reset value");
                if (capture > 0) {
                    require(runtime.undo() &&
                                canvas.canvasStyleToolbarState().serialNumberStyle.number == 1,
                            "new capture discards the old manual start override");
                    require(runtime.redo() &&
                                canvas.canvasStyleToolbarState().serialNumberStyle.number == 2,
                            "new capture resumes automatic numbering after redo");
                }
            }
        }
    }
}
void textEmphasisButtonsPersistDefaultsWithoutEndingDrafts() {
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.text.bold = false;
    defaults.text.italic = false;
    require(persistScreenshotCanvasToolStyles(defaults), "reset emphasis defaults");
    Editor editor;
    editor.text();
    const auto button = [&](const char* name) {
        auto* result =
            editor.palette.findChild<adqt::widgets::AdButton*>(QString::fromLatin1(name));
        require(result != nullptr, "materialize text emphasis button");
        return result;
    };
    button("screenshotTextBoldButton")->click();
    require(screenshotCanvasToolStyleDefaults().text.bold &&
                !screenshotCanvasToolStyleDefaults().text.italic &&
                editor.canvas.canvasStyleToolbarState().textStyle.bold,
            "bold button persists and updates canvas defaults");
    mouse(editor.canvas, QEvent::MouseButtonPress, {100, 100});
    mouse(editor.canvas, QEvent::MouseButtonRelease, {100, 100});
    QInputMethodEvent input;
    input.setCommitString(QStringLiteral("Draft emphasis"));
    QApplication::sendEvent(&editor.canvas, &input);
    require(editor.canvas.hasActiveTextEditing(), "begin a styled text draft");
    button("screenshotTextItalicButton")->click();
    const auto style = editor.canvas.canvasStyleToolbarState().textStyle;
    require(editor.canvas.hasActiveTextEditing() && style.bold && style.italic &&
                screenshotCanvasToolStyleDefaults().text.bold &&
                screenshotCanvasToolStyleDefaults().text.italic,
            "italic updates the draft and remembered defaults without ending editing");
    require(editor.canvas.cancelActiveTextEditing(), "cancel the styled draft");
    Editor fresh;
    fresh.text();
    require(fresh.canvas.canvasStyleToolbarState().textStyle.bold &&
                fresh.canvas.canvasStyleToolbarState().textStyle.italic,
            "new editors restore emphasis choices even after draft cancellation");
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    const auto key = QStringLiteral("drawing/text_style");
    auto remembered = configuration.value(key).toObject();
    require(remembered.value(QStringLiteral("bold")).isBool() &&
                remembered.value(QStringLiteral("bold")).toBool() &&
                remembered.value(QStringLiteral("italic")).isBool() &&
                remembered.value(QStringLiteral("italic")).toBool(),
            "remembered emphasis is stored as independent booleans");
    remembered.remove(QStringLiteral("bold"));
    remembered.remove(QStringLiteral("italic"));
    require(configuration.setValue(key, remembered), "load legacy text settings");
    const auto legacy = screenshotCanvasToolStyleDefaults().text;
    require(!legacy.bold && !legacy.italic && legacy.fontSize == style.fontSize &&
                legacy.fontFamily == style.fontFamily,
            "legacy text settings default emphasis to false and preserve existing typography");
}

void fontWheelRemembersDefaultsAndDraftChoice() {
    Editor editor;
    editor.text();
    editor.canvas.show();
    const double initial = editor.canvas.canvasStyleToolbarState().textStyle.fontSize;
    wheel(editor.canvas, 120);
    require(screenshotCanvasToolStyleDefaults().text.fontSize == initial + 1,
            "canvas text wheel persists without an active draft");
    require(editor.palette.creationStyleDefaults().text.fontSize == initial + 1,
            "canvas text wheel updates palette creation defaults");
    const QPointF point(100, 100);
    QMouseEvent press(QEvent::MouseButtonPress, point, editor.canvas.mapToGlobal(point.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&editor.canvas, &press);
    require(editor.canvas.hasActiveTextEditing(), "start a text draft");
    wheel(editor.canvas, 120);
    require(editor.canvas.hasActiveTextEditing(), "style commit preserves the active draft");
    require(screenshotCanvasToolStyleDefaults().text.fontSize == initial + 2,
            "active draft wheel persists");
    require(editor.canvas.cancelActiveTextEditing(), "cancel draft");
    require(screenshotCanvasToolStyleDefaults().text.fontSize == initial + 2,
            "canceling a draft retains the explicit preference");
    require(editor.canvas.canvasStyleToolbarState().textStyle.fontSize == initial + 2,
            "canceling a draft preserves this canvas's creation font size");
    Editor fresh;
    require(fresh.palette.creationStyleDefaults().text.fontSize == initial + 2,
            "new editor loads the remembered font size");
}
void textAlignmentPopupPreservesEditing(bool editExisting) {
    const auto original = screenshotCanvasToolStyleDefaults();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(persistScreenshotCanvasToolStyles(original)); });
    auto defaults = original;
    defaults.text.horizontalAlign = SnowCanvasTextHorizontalAlign::Left;
    require(persistScreenshotCanvasToolStyles(defaults), "reset text alignment defaults");
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    ScreenshotToolPalette palette(options());
    ScreenshotStyleBinding binding(palette, canvas, &palette);
    applyScreenshotCanvasToolStyles(canvas, palette.creationStyleDefaults());
    canvas.resize(400, 300);
    canvas.setInteractionEnabled(true);
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    QObject::connect(&palette, &ScreenshotToolPalette::textStylePopupInteractionBegan, &canvas,
                     [&] { canvas.beginTextStylePopupInteraction(); });
    QObject::connect(&palette, &ScreenshotToolPalette::textStylePopupInteractionEnded, &canvas,
                     [&] { canvas.endTextStylePopupInteraction(&palette); });
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text alignment fixture");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    canvas.show();
    palette.show();
    QApplication::processEvents();
    mouse(canvas, QEvent::MouseButtonPress, {100, 100});
    mouse(canvas, QEvent::MouseButtonRelease, {100, 100});
    QInputMethodEvent input;
    input.setCommitString(QStringLiteral("Alignment draft\nShort"));
    QApplication::sendEvent(&canvas, &input);
    require(canvas.hasActiveTextEditing() && documentSlots(runtime).isEmpty() &&
                canvas.canvasStyleToolbarState().source ==
                    SnowCanvasStyleToolbarSource::DefaultText,
            "new text must be an unselected draft outside the document");
    if (editExisting) {
        QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
        QApplication::sendEvent(&canvas, &focusOut);
        const auto center = documentSlots(runtime)
                                .first()
                                .toObject()
                                .value(QStringLiteral("data"))
                                .toObject()
                                .value(QStringLiteral("Text"))
                                .toObject()
                                .value(QStringLiteral("center"))
                                .toObject();
        const auto point = canvas.canvasToViewTransform().map(
            QPointF(center.value(QStringLiteral("x")).toDouble(),
                    center.value(QStringLiteral("y")).toDouble()));
        mouse(canvas, QEvent::MouseButtonPress, point);
        mouse(canvas, QEvent::MouseButtonRelease, point);
        require(canvas.hasActiveTextEditing() && canvas.canvasStyleToolbarState().source ==
                                                     SnowCanvasStyleToolbarSource::SelectedText,
                "reopening committed text must edit the existing selected element");
    }
    const auto before = documentSlots(runtime);

    adqt::widgets::AdPopover* popover = nullptr;
    for (QWidget* control : palette.findChildren<QWidget*>()) {
        if (control->accessibleName() == QStringLiteral("Text alignment")) {
            popover = control->findChild<adqt::widgets::AdPopover*>();
            if (popover != nullptr)
                break;
        }
    }
    require(popover != nullptr, "text alignment must own an option popover");
    popover->setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::InWindow);
    for (const auto alignment :
         {SnowCanvasTextHorizontalAlign::Center, SnowCanvasTextHorizontalAlign::Right}) {
        popover->show();
        // Deliver the native popup's focus transfer explicitly so this also reproduces offscreen.
        QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
        QApplication::sendEvent(&canvas, &focusOut);
        require(canvas.hasActiveTextEditing() && documentSlots(runtime) == before,
                "opening alignment options must retain text editing before focus loss");
        auto* content = popover->contentWidget();
        require(content != nullptr, "alignment options must materialize on each open");
        const auto tooltip = alignment == SnowCanvasTextHorizontalAlign::Center
                                 ? QStringLiteral("Align text center")
                                 : QStringLiteral("Align text right");
        adqt::widgets::AdButton* option = nullptr;
        for (auto* button : content->findChildren<adqt::widgets::AdButton*>()) {
            if (button->toolTip() == tooltip)
                option = button;
        }
        require(option != nullptr, "requested alignment option must be present");
        option->click();
        QApplication::processEvents();
        require(!popover->isVisible() && canvas.hasActiveTextEditing() &&
                    documentSlots(runtime) == before &&
                    canvas.canvasStyleToolbarState().textStyle.horizontalAlign == alignment &&
                    screenshotCanvasToolStyleDefaults().text.horizontalAlign == alignment,
                "alignment clicks must update the active draft and defaults without committing it");
    }
    QFocusEvent unrelatedFocusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QApplication::sendEvent(&canvas, &unrelatedFocusOut);
    require(!canvas.hasActiveTextEditing() && documentSlots(runtime).size() == 1,
            "ordinary focus loss must still commit after the alignment popup closes");
    const auto text = documentSlots(runtime)
                          .first()
                          .toObject()
                          .value(QStringLiteral("data"))
                          .toObject()
                          .value(QStringLiteral("Text"))
                          .toObject();
    require(
        text.value(QStringLiteral("text")).toString() == QStringLiteral("Alignment draft\nShort") &&
            text.value(QStringLiteral("horizontal_align")).toString() == QStringLiteral("Right"),
        "committed text must preserve the content and alignment chosen during editing");
}
void propertyPatchesPreserveOtherEditorsAndProgrammaticState() {
    Editor first;
    Editor second;
    first.text();
    second.text();
    const auto initial = screenshotCanvasToolStyleDefaults();
    auto style = initial.text;
    style.color = QColor(17, 31, 53);
    require(first.canvas.commitStyleEdit(SnowCanvasTextEdit{style, SnowCanvasTextStyleMixedColor}),
            "commit first editor color");
    style = initial.text;
    style.fontSize = 47;
    require(
        second.canvas.commitStyleEdit(SnowCanvasTextEdit{style, SnowCanvasTextStyleMixedFontSize}),
        "commit second editor size");
    const auto saved = screenshotCanvasToolStyleDefaults();
    require(saved.text.fontSize == 47 && saved.text.color == QColor(17, 31, 53),
            "stale editor must not overwrite another editor's changed property");
    require(second.binding.lastSaveSucceeded() == true, "binding exposes persistence success");
    style.color = Qt::blue;
    require(second.canvas.setCanvasTextStyle(style), "programmatic style update");
    second.palette.setStyleToolbarState(second.canvas.canvasStyleToolbarState());
    second.canvas.previewCanvasWatermarkConfig(initial.watermark);
    second.canvas.previewCanvasSpotlightConfig(initial.spotlight);
    static_cast<void>(second.canvas.undo());
    static_cast<void>(second.canvas.redo());
    require(screenshotCanvasToolStyleDefaults() == saved,
            "refresh, previews, programmatic setters and history do not persist preferences");
    style.fontSize = std::numeric_limits<double>::quiet_NaN();
    require(
        !second.canvas.commitStyleEdit(SnowCanvasTextEdit{style, SnowCanvasTextStyleMixedFontSize}),
        "invalid style edit fails");
    second.canvas.setInteractionEnabled(false);
    wheel(second.canvas, 120);
    require(!second.canvas.commitStyleEdit(
                SnowCanvasTextEdit{saved.text, SnowCanvasTextStyleMixedColor}),
            "disabled interaction rejects user edits");
    require(screenshotCanvasToolStyleDefaults() == saved, "failed edits never persist");
}
void allStyleFamiliesPersistOnlyTheirPatch() {
    Editor editor;
    auto expected = screenshotCanvasToolStyleDefaults();
    auto verify = [&](SnowCanvasTool tool, const SnowCanvasStyleEdit& edit) {
        require(editor.canvas.setCanvasTool(tool), "activate family");
        require(editor.canvas.commitStyleEdit(edit), "apply family patch");
        snowCanvasMergeStyleEdit(expected, edit);
        require(screenshotCanvasToolStyleDefaults() == expected,
                "each style family saves only explicitly edited properties");
    };
    for (const auto kind :
         {SnowCanvasShapeKind::Rectangle, SnowCanvasShapeKind::Arrow, SnowCanvasShapeKind::Line,
          SnowCanvasShapeKind::FreeDraw, SnowCanvasShapeKind::RectangleHighlight,
          SnowCanvasShapeKind::PenHighlight}) {
        auto style = expected.rectangle;
        style.strokeWidth = 11;
        verify(SnowCanvasTool::Shape,
               SnowCanvasShapeEdit{style, SnowCanvasShapeStylePropertyStrokeWidth, kind});
    }
    auto serial = expected.serialNumber;
    serial.fontSize = 73;
    serial.number = 999;
    serial.color = Qt::cyan;
    verify(SnowCanvasTool::SerialNumber,
           SnowCanvasSerialNumberEdit{serial, SnowCanvasSerialNumberStyleMixedFontSize});
    serial.numericType = SnowCanvasSerialNumberNumericType::Roman;
    verify(SnowCanvasTool::SerialNumber,
           SnowCanvasSerialNumberEdit{serial, SnowCanvasSerialNumberStyleMixedNumericType});
    auto filter = expected.rectangleFilter;
    filter.strength = 0.37;
    verify(SnowCanvasTool::RectangleFilter,
           SnowCanvasFilterEdit{filter, SnowCanvasFilterStylePropertyStrength, false});
    filter.strokeWidth = 13;
    verify(SnowCanvasTool::PenFilter,
           SnowCanvasFilterEdit{filter, SnowCanvasFilterStylePropertyStrokeWidth, true});
    auto watermark = expected.watermark;
    watermark.fontSize = 41;
    watermark.color = Qt::blue;
    verify(SnowCanvasTool::Watermark,
           SnowCanvasWatermarkEdit{watermark, SnowCanvasWatermarkFontSize});
    auto spotlight = expected.spotlight;
    spotlight.opacity = 0.37;
    verify(SnowCanvasTool::Spotlight,
           SnowCanvasSpotlightEdit{spotlight, SnowCanvasSpotlightOpacity});
    auto spotlightStyle = expected.rectangle;
    spotlightStyle.shape = SnowCanvasRectangleShape::Diamond;
    verify(SnowCanvasTool::Spotlight,
           SnowCanvasShapeEdit{spotlightStyle, SnowCanvasShapeStylePropertyShape,
                               SnowCanvasShapeKind::Spotlight});
}
void sharedScreenshotRuntimeKeepsDraftEditsTransient() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    SnowCanvasWidget peer(runtime);
    ScreenshotToolPalette palette(options());
    ScreenshotStyleBinding binding(palette, canvas, &palette, [&](const SnowCanvasStyleEdit& edit) {
        replicateScreenshotStyleEdit(peer, edit);
    });
    canvas.resize(400, 300);
    canvas.show();
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate shared-runtime text");
    const QPointF point(100, 100);
    QMouseEvent press(QEvent::MouseButtonPress, point, canvas.mapToGlobal(point.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QKeyEvent type(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("alpha"));
    QApplication::sendEvent(&canvas, &type);
    require(canvas.hasActiveTextEditing(), "shared-runtime draft stays active");
    const auto history = canvas.canvasHistoryState();
    wheel(canvas, 120);
    require(canvas.hasActiveTextEditing() &&
                canvas.canvasHistoryState().canUndo == history.canUndo &&
                canvas.canvasHistoryState().canRedo == history.canRedo,
            "replication must not commit a draft or add a document history entry");
    const double size = canvas.canvasStyleToolbarState().textStyle.fontSize;
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "commit the draft by leaving Text");
    const QPointF start(1, 1), end(399, 299);
    QMouseEvent selectPress(QEvent::MouseButtonPress, start, canvas.mapToGlobal(start.toPoint()),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent selectMove(QEvent::MouseMove, end, canvas.mapToGlobal(end.toPoint()), Qt::NoButton,
                           Qt::LeftButton, Qt::NoModifier);
    QMouseEvent selectRelease(QEvent::MouseButtonRelease, end, canvas.mapToGlobal(end.toPoint()),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &selectPress);
    QApplication::sendEvent(&canvas, &selectMove);
    QApplication::sendEvent(&canvas, &selectRelease);
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedText,
            "select the committed text for a property edit");
    const auto saved = screenshotCanvasToolStyleDefaults();
    require(canvas.stepFontSize(1), "selected text font step");
    require(screenshotCanvasToolStyleDefaults().text.fontSize == size + 1 &&
                screenshotCanvasToolStyleDefaults().text.color == saved.text.color,
            "selected text step remembers only its explicit property");
    require(canvas.undo(), "undo selected text style");
    require(screenshotCanvasToolStyleDefaults().text.fontSize == size + 1,
            "undo leaves the preference unchanged");
    require(canvas.redo(), "redo selected text style");
    require(peer.canvasStyleToolbarState().textStyle.fontSize == size + 1,
            "shared screenshot viewports synchronize without a second edit");
}

void toolbarAndCanvasShareFontCommit() {
    Editor editor;
    editor.text();
    editor.palette.show();
    QApplication::processEvents();
    adqt::widgets::AdButton* preset = nullptr;
    for (auto* button : editor.palette.findChildren<adqt::widgets::AdButton*>()) {
        if (button->toolTip() == QStringLiteral("Text font size L (42px)"))
            preset = button;
    }
    require(preset != nullptr, "text size preset exists");
    preset->click();
    require(screenshotCanvasToolStyleDefaults().text.fontSize == 42 &&
                editor.canvas.canvasStyleToolbarState().textStyle.fontSize == 42,
            "toolbar preset applies and persists through the shared binding");
    wheel(editor.canvas, -120);
    require(screenshotCanvasToolStyleDefaults().text.fontSize == 41,
            "canvas wheel follows toolbar preset state");
    require(editor.canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number");
    auto serial = editor.canvas.canvasStyleToolbarState().serialNumberStyle;
    serial.fontSize = 511;
    require(editor.canvas.commitStyleEdit(
                SnowCanvasSerialNumberEdit{serial, SnowCanvasSerialNumberStyleMixedFontSize}),
            "set serial font near limit");
    wheel(editor.canvas, 120);
    wheel(editor.canvas, 120);
    wheel(editor.canvas, 0);
    require(screenshotCanvasToolStyleDefaults().serialNumber.fontSize == 512,
            "serial wheel uses the same upper limit as direct input");
}
ScreenshotToolPalette::Options watermarkOptions(std::function<QDateTime()> clock) {
    auto result = options();
    result.watermarkTemplateClock = clock ? std::move(clock) : [] {
        return QDateTime(QDate(2026, 10, 9), QTime(12, 34, 56), QTimeZone::LocalTime);
    };
    return result;
}

struct WatermarkEditor {
    SnowCanvasRuntime runtime{SnowCanvasRuntimeConfig{screenshotCanvasToolStyleDefaults()}};
    SnowCanvasWidget canvas{runtime};
    ScreenshotToolPalette palette;
    ScreenshotStyleBinding binding;

    explicit WatermarkEditor(std::function<QDateTime()> clock = {})
        : palette(watermarkOptions(std::move(clock))), binding(palette, canvas, &palette) {
        canvas.resize(400, 300);
        canvas.setInteractionEnabled(true);
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette, [this] {
            palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
            palette.setWatermarkConfig(canvas.canvasWatermarkConfig());
        });
        QObject::connect(&palette, &ScreenshotToolPalette::watermarkRequested, &canvas, [this] {
            require(canvas.setCanvasTool(SnowCanvasTool::Watermark), "activate watermark canvas");
        });
        QObject::connect(&palette, &ScreenshotToolPalette::selectRequested, &canvas, [this] {
            require(canvas.setCanvasTool(SnowCanvasTool::Select), "activate selection canvas");
        });
    }

    void activate() {
        require(palette.activateToolShortcut(ScreenshotToolPalette::Tool::Watermark),
                "watermark toolbar activation succeeds");
    }

    void reopen() {
        require(palette.activateToolShortcut(ScreenshotToolPalette::Tool::Select),
                "switch away from watermark");
        activate();
    }

    QLineEdit* text() {
        auto* editor = palette.findChild<QLineEdit*>(QStringLiteral("screenshotWatermarkTextEdit"));
        require(editor != nullptr, "watermark text editor exists");
        return editor;
    }

    QLineEdit* templateText() {
        auto* select = palette.findChild<adqt::widgets::AdSelect*>(
            QStringLiteral("screenshotWatermarkTemplateSelect"));
        require(select != nullptr && select->lineEdit() != nullptr,
                "watermark template editor exists");
        return select->lineEdit();
    }

    void editTemplate(const QString& value) {
        auto* editor = templateText();
        editor->setText(value);
        emit editor->textEdited(value);
    }
};

void watermarkContentEditsPersistOnlyCommittedFields() {
    const snow_shot::storage::WatermarkContentSettings settings;
    require(settings.setContent({}), "start watermark edits without saved content");
    WatermarkEditor editor;
    editor.activate();
    editor.text()->setText(QStringLiteral("  版权所有 © 雪  "));
    const QString text = QStringLiteral("版权所有 © 雪");
    const QString templateValue = QStringLiteral("  {text} / {YYYY-MM-DD_HH-mm-ss}  ");
    editor.editTemplate(templateValue);
    require(editor.binding.lastSaveSucceeded() == true &&
                settings.content() == snow_shot::storage::WatermarkContent{text, templateValue} &&
                editor.canvas.canvasWatermarkConfig().text == text &&
                editor.canvas.canvasWatermarkConfig().templateValue == templateValue,
            "successful editor commits save trimmed text and exact manual template whitespace");
    const auto stored = snow_shot::storage::ApplicationStorage::instance()
                            .configuration()
                            .value(QStringLiteral("drawing/watermark_content"))
                            .toObject();
    require(stored.size() == 2 && !stored.contains(QStringLiteral("template_application_time")),
            "content preferences never save application timestamps");
    auto stale = editor.canvas.canvasWatermarkConfig();
    stale.text = QStringLiteral("stale text");
    stale.templateValue = QStringLiteral("stale template");
    stale.fontSize = 37;
    require(editor.canvas.commitStyleEdit(
                SnowCanvasWatermarkEdit{stale, SnowCanvasWatermarkFontSize}) &&
                settings.content() == snow_shot::storage::WatermarkContent{text, templateValue},
            "appearance-only commits preserve saved text and template");
    stale.text = QStringLiteral("Updated");
    require(
        editor.canvas.commitStyleEdit(SnowCanvasWatermarkEdit{stale, SnowCanvasWatermarkText}) &&
            settings.content() == snow_shot::storage::WatermarkContent{stale.text, templateValue},
        "text-only patches preserve the latest template rather than a stale editor value");
    editor.editTemplate(QString());
    require(settings.content() == snow_shot::storage::WatermarkContent{stale.text, QString()},
            "clearing the template preserves saved text");
    WatermarkEditor plain;
    plain.activate();
    require(plain.canvas.canvasWatermarkConfig().text == stale.text &&
                plain.canvas.canvasWatermarkConfig().templateValue.isEmpty() &&
                !plain.canvas.canvasWatermarkConfig().templateApplicationTime.has_value(),
            "plain text restores without a template or application time");
    plain.text()->clear();
    require(settings.content() == snow_shot::storage::WatermarkContent{},
            "clearing text persists empty content");
    plain.reopen();
    require(plain.canvas.canvasWatermarkConfig().text.isEmpty(),
            "reopening a cleared watermark does not resurrect previous text");
    WatermarkEditor empty;
    empty.activate();
    require(!empty.runtime.hasDocumentContent() && !empty.runtime.canUndo(),
            "empty saved content creates no watermark or history entry");
    empty.editTemplate(QStringLiteral("{YYYY}"));
    WatermarkEditor templateOnly;
    templateOnly.activate();
    require(templateOnly.canvas.canvasWatermarkConfig().text.isEmpty() &&
                templateOnly.canvas.canvasWatermarkConfig().templateValue ==
                    QStringLiteral("{YYYY}") &&
                templateOnly.canvas.canvasWatermarkConfig().templateApplicationTime ==
                    std::optional<SnowCanvasWatermarkTemplateApplicationTime>{
                        {2026, 10, 9, 12, 34, 56}},
            "a timestamp-only template restores even with empty watermark text");
}

void rememberedWatermarkAppliesOncePerCaptureWithFreshTime() {
    const snow_shot::storage::WatermarkContentSettings settings;
    const snow_shot::storage::WatermarkContent saved{
        QStringLiteral("CONFIDENTIAL"), QStringLiteral("  {text} {YYYY-MM-DD_HH-mm-ss}  ")};
    require(settings.setContent(saved), "prepare remembered watermark content");
    int clockCalls = 0;
    WatermarkEditor editor([&clockCalls] {
        return QDateTime(QDate(2026, 10, 9), QTime(12, 34, 10 + clockCalls++),
                         QTimeZone::LocalTime);
    });
    const QRectF bounds(0, 0, 400, 300);
    QImage base(400, 300, QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::white);
    const QList<CanvasExportSource> sources{{base, bounds}};
    const QImage before = editor.runtime.renderToImage(bounds, base.size(), sources);
    require(!before.isNull() && !editor.runtime.hasDocumentContent() && !editor.runtime.canUndo() &&
                editor.canvas.canvasWatermarkConfig().text.isEmpty() && clockCalls == 0,
            "remembered content does not apply during canvas construction");
    editor.activate();
    const auto first = editor.canvas.canvasWatermarkConfig();
    require(
        first.text == saved.text && first.templateValue == saved.templateValue &&
            first.templateApplicationTime ==
                std::optional<SnowCanvasWatermarkTemplateApplicationTime>{
                    {2026, 10, 9, 12, 34, 10}} &&
            editor.text()->text() == saved.text &&
            editor.templateText()->text() == saved.templateValue && clockCalls == 1,
        "activation populates both editors and applies remembered content with the current time");
    const QImage after = editor.runtime.renderToImage(bounds, base.size(), sources);
    require(!after.isNull() && after != before && editor.runtime.hasDocumentContent(),
            "the remembered watermark is rendered on the capture after activation");
    editor.reopen();
    require(editor.canvas.canvasWatermarkConfig() == first && clockCalls == 1 &&
                editor.templateText()->text() == saved.templateValue,
            "tool switching preserves watermark content and its application time");
    require(editor.canvas.undo() && !editor.runtime.hasDocumentContent() &&
                !editor.runtime.canUndo(),
            "restored text and template undo together as one edit");
    editor.reopen();
    require(!editor.runtime.hasDocumentContent() && editor.runtime.canRedo() && clockCalls == 1 &&
                settings.content() == saved,
            "undo and reopening do not reapply saved content or overwrite preferences");
    require(editor.canvas.redo() && editor.canvas.canvasWatermarkConfig() == first,
            "redo restores the original application timestamp");
    require(editor.canvas.clearDocument(), "start a new capture using the same canvas");
    editor.palette.resetStyleState();
    editor.palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    editor.activate();
    require(clockCalls == 2 && editor.canvas.canvasWatermarkConfig().text == saved.text &&
                editor.canvas.canvasWatermarkConfig().templateApplicationTime ==
                    std::optional<SnowCanvasWatermarkTemplateApplicationTime>{
                        {2026, 10, 9, 12, 34, 11}},
            "a new capture rearms restoration and takes a fresh timestamp");
    WatermarkEditor existing([&clockCalls] {
        ++clockCalls;
        return QDateTime(QDate(2026, 10, 9), QTime(12, 34, 56), QTimeZone::LocalTime);
    });
    auto document = first;
    document.text = QStringLiteral("Existing document");
    document.templateValue = QStringLiteral("{text} {YYYY}");
    document.templateApplicationTime =
        SnowCanvasWatermarkTemplateApplicationTime{2024, 2, 29, 1, 2, 3};
    require(existing.canvas.setCanvasWatermarkConfig(document),
            "load an existing document watermark");
    existing.activate();
    require(
        existing.canvas.canvasWatermarkConfig() == document &&
            existing.text()->text() == document.text &&
            existing.templateText()->text() == document.templateValue && clockCalls == 2 &&
            settings.content() == saved,
        "existing document content and timestamps take precedence without changing preferences");
    require(existing.canvas.undo(), "undo the loaded document watermark");
    existing.reopen();
    require(!existing.runtime.hasDocumentContent() && clockCalls == 2,
            "an existing document's empty undo state does not trigger preference restoration");
}

void rejectedWatermarkRestorationCanRetryWithoutSaving() {
    const snow_shot::storage::WatermarkContentSettings settings;
    const snow_shot::storage::WatermarkContent saved{QStringLiteral("Saved"),
                                                     QStringLiteral("{text} {YYYY}")};
    require(settings.setContent(saved), "prepare content for rejected restoration");
    WatermarkEditor editor;
    editor.palette.setStyleEditHandler([](const SnowCanvasStyleEdit&) { return false; });
    editor.activate();
    require(!editor.runtime.hasDocumentContent() &&
                !editor.binding.lastSaveSucceeded().has_value() && settings.content() == saved,
            "a rejected restoration neither changes the document nor persists content");
    editor.palette.setStyleEditHandler(
        [&editor](const SnowCanvasStyleEdit& edit) { return editor.canvas.commitStyleEdit(edit); });
    editor.reopen();
    require(editor.canvas.canvasWatermarkConfig().text == saved.text &&
                editor.canvas.canvasWatermarkConfig().templateValue == saved.templateValue &&
                editor.binding.lastSaveSucceeded() == true,
            "rejected restoration remains pending and succeeds on the next activation");
    const auto applied = editor.canvas.canvasWatermarkConfig();
    editor.palette.setStyleEditHandler([&editor](const SnowCanvasStyleEdit&) {
        editor.palette.setWatermarkConfig(editor.canvas.canvasWatermarkConfig());
        return false;
    });
    editor.text()->setText(QStringLiteral("Rejected"));
    require(editor.canvas.canvasWatermarkConfig() == applied && settings.content() == saved &&
                editor.text()->text() == applied.text,
            "rejected text edits preserve the document, saved content, and synchronized editor");
}
} // namespace

void runScreenshotTextAlignmentTests() {
    textAlignmentPopupPreservesEditing(false);
    textAlignmentPopupPreservesEditing(true);
}

void runWatermarkPersistenceTests() {
    const snow_shot::storage::WatermarkContentSettings settings;
    const auto originalContent = settings.content();
    const auto originalStyles = screenshotCanvasToolStyleDefaults();
    const auto restore = qScopeGuard([&] {
        static_cast<void>(settings.setContent(originalContent));
        static_cast<void>(persistScreenshotCanvasToolStyles(originalStyles));
    });
    watermarkContentEditsPersistOnlyCommittedFields();
    rememberedWatermarkAppliesOncePerCaptureWithFreshTime();
    rejectedWatermarkRestorationCanRetryWithoutSaving();
}

void runScreenshotStyleBindingTests() {
    const auto original = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
    const auto restore = qScopeGuard([&] {
        static_cast<void>(snow_shot::presentation::persistScreenshotCanvasToolStyles(original));
    });
    styleBindingsReleaseConnectionsForPooledEditors();
    styleBindingsTolerateEditorDestruction();
    arrowCreationDefaultsRestoreShaftAndRatio();
    creationDefaultsDoNotActivateFilterTools();
    creationDefaultsPreserveDocumentAndEditingCleanup();
    fontWheelRemembersDefaultsAndDraftChoice();
    textEmphasisButtonsPersistDefaultsWithoutEndingDrafts();
    propertyPatchesPreserveOtherEditorsAndProgrammaticState();
    allStyleFamiliesPersistOnlyTheirPatch();
    toolbarAndCanvasShareFontCommit();
    sharedScreenshotRuntimeKeepsDraftEditsTransient();
    runScreenshotSelectedFilterStyleTests();
    runWatermarkPersistenceTests();
}

void runScreenshotSerialNumberRestartTests() {
    selectedSerialNumberToolbarEditsContinueCreationSequence();
    selectedSerialNumberSizeEditsUpdateCreationAppearance();
    selectedSerialNumberEditsUpdateCreationAppearance();
    serialNumberAppearanceDefaultsPreserveSessionSequences();
    newScreenshotDocumentRestartsSerialNumberSequences();
    const auto original = screenshotCanvasToolStyleDefaults();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(persistScreenshotCanvasToolStyles(original)); });
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    ScreenshotToolPalette palette(options());
    ScreenshotStyleBinding binding(palette, canvas, &palette);
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    canvas.resize(400, 300);
    canvas.show();
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number tool");
    auto style = canvas.canvasStyleToolbarState().serialNumberStyle;
    style.type = SnowCanvasSerialNumberType::OutlinedCircle;
    style.numericType = SnowCanvasSerialNumberNumericType::Arabic;
    style.fontSize = 24;
    style.number = 50;
    require(canvas.setCanvasSerialNumberStyle(style), "start with a high canvas number");
    const auto create = [&](QPointF point) {
        mouse(canvas, QEvent::MouseButtonPress, point);
        mouse(canvas, QEvent::MouseButtonRelease, point);
        return documentSlots(runtime)
            .last()
            .toObject()
            .value(QStringLiteral("data"))
            .toObject()
            .value(QStringLiteral("SerialNumber"))
            .toObject()
            .value(QStringLiteral("number"))
            .toVariant()
            .toLongLong();
    };
    require(create({40, 150}) == 50, "create the existing high sequence number");
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
    palette.show();
    QApplication::processEvents();
    adqt::widgets::AdLineEdit* input = nullptr;
    for (auto* candidate : palette.findChildren<adqt::widgets::AdLineEdit*>()) {
        if (candidate->toolTip() == QStringLiteral("Sequence number (scroll to adjust)")) {
            input = candidate;
            break;
        }
    }
    require(input != nullptr && input->text() == QStringLiteral("51"),
            "style editor displays the next number after the high element");
    for (const int start : {2, 0}) {
        input->setText(QString::number(start));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(input, &enter);
        require(canvas.canvasStyleToolbarState().serialNumberStyle.number == start,
                "style editor commits a value below the maximum canvas number");
        for (int offset = 0; offset < 3; ++offset) {
            require(create({120.0 + offset * 100.0, 80.0 + start * 50.0}) == start + offset,
                    "canvas creates the entered smaller number and increments from it");
            require(input->text() == QString::number(start + offset + 1),
                    "style editor follows the restarted sequence");
        }
    }
    require(documentSlots(runtime)
                    .first()
                    .toObject()
                    .value(QStringLiteral("data"))
                    .toObject()
                    .value(QStringLiteral("SerialNumber"))
                    .toObject()
                    .value(QStringLiteral("number"))
                    .toInt() == 50,
            "restarting numbering preserves the existing high element");

    const SnowCanvasSerialNumberNumericType types[] = {
        SnowCanvasSerialNumberNumericType::Arabic,
        SnowCanvasSerialNumberNumericType::Roman,
        SnowCanvasSerialNumberNumericType::Chinese,
        SnowCanvasSerialNumberNumericType::LowercaseLetters,
        SnowCanvasSerialNumberNumericType::UppercaseLetters,
    };
    qint64 nextNumbers[] = {11, 12, 13, 14, 15};
    const auto numericEditorText = [&] {
        for (auto* candidate : palette.findChildren<adqt::widgets::AdLineEdit*>()) {
            if (candidate->toolTip() == QStringLiteral("Sequence number (scroll to adjust)")) {
                return candidate->text();
            }
        }
        require(false, "the active serial number editor exists after a canvas state change");
        return QString();
    };
    const auto activateNumericType = [&](int index, bool setNumber) {
        auto nextStyle = canvas.canvasStyleToolbarState().serialNumberStyle;
        nextStyle.numericType = types[index];
        nextStyle.number = nextNumbers[index];
        require(canvas.commitStyleEdit(SnowCanvasSerialNumberEdit{
                    nextStyle, SnowCanvasSerialNumberStyleMixedNumericType |
                                   (setNumber ? SnowCanvasSerialNumberStyleMixedNumber : 0)}),
                "activate the numeric type's independent creation sequence");
    };
    const auto rightClick = [&](QPointF point) {
        QMouseEvent press(QEvent::MouseButtonPress, point, canvas.mapToGlobal(point.toPoint()),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, canvas.mapToGlobal(point.toPoint()),
                            Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &press);
        QApplication::sendEvent(&canvas, &release);
    };
    for (int index = 0; index < 5; ++index) {
        activateNumericType(index, true);
    }
    for (int index = 0; index < 5; ++index) {
        activateNumericType(index, false);
        auto expectedStyle = canvas.canvasStyleToolbarState().serialNumberStyle;
        expectedStyle.number = 1;
        const auto elementsBeforeReset = documentSlots(runtime);
        const auto history = runtime.serializeDocumentHistory();
        const auto revision = runtime.documentRevision();
        if (index == 0) {
            rightClick({40, 150});
            require(canvas.canvasStyleToolbarState().source ==
                            SnowCanvasStyleToolbarSource::SelectedSerialNumber &&
                        numericEditorText() == QStringLiteral("50"),
                    "right clicking an existing serial number selects it without resetting it");
        }
        const QPointF blank(390, 290);
        rightClick(blank);
        require(canvas.canvasStyleToolbarState().source ==
                        SnowCanvasStyleToolbarSource::DefaultSerialNumber &&
                    canvas.canvasStyleToolbarState().serialNumberStyle == expectedStyle &&
                    palette.creationStyleDefaults().serialNumber == expectedStyle &&
                    numericEditorText() == QStringLiteral("1"),
                "blank canvas right click resets the active numeric editor and creation value");
        require(documentSlots(runtime) == elementsBeforeReset &&
                    runtime.serializeDocumentHistory() == history &&
                    runtime.documentRevision() == revision,
                "right click reset leaves existing annotations and undo history unchanged");
        QContextMenuEvent menu(QContextMenuEvent::Mouse, blank.toPoint(),
                               canvas.mapToGlobal(blank.toPoint()));
        menu.setAccepted(false);
        QApplication::sendEvent(&canvas, &menu);
        require(menu.isAccepted(), "serial number reset consumes the mouse context menu");
        rightClick(blank);
        require(numericEditorText() == QStringLiteral("1"),
                "repeated blank right click stays at one");
        require(create({40.0 + index * 75.0, 260}) == 1 &&
                    numericEditorText() == QStringLiteral("2"),
                "the next annotation starts at one and advances the numeric editor");
        nextNumbers[index] = 2;
        for (int other = 0; other < 5; ++other) {
            activateNumericType(other, false);
            require(numericEditorText() == QString::number(nextNumbers[other]),
                    "right click reset preserves the other numeric types' next values");
        }
    }
    activateNumericType(0, true);
    style = canvas.canvasStyleToolbarState().serialNumberStyle;
    style.number = 9;
    require(canvas.commitStyleEdit(
                SnowCanvasSerialNumberEdit{style, SnowCanvasSerialNumberStyleMixedNumber}),
            "prepare a serial number before disabling interaction");
    canvas.setInteractionEnabled(false);
    rightClick({390, 290});
    require(numericEditorText() == QStringLiteral("9"),
            "disabled canvas interaction does not reset the numeric editor");
    canvas.setInteractionEnabled(true);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "leave the serial number tool");
    rightClick({390, 290});
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "return to the serial number tool");
    require(numericEditorText() == QStringLiteral("9"),
            "blank right click with another active tool preserves the serial number");
    rightClick({40, 260});
    require(canvas.createSerialNumberText() && canvas.hasActiveTextEditing(),
            "edit a serial number's attached text before resetting its sequence");
    QKeyEvent label(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("Kept label"));
    QApplication::sendEvent(&canvas, &label);
    rightClick({390, 290});
    require(!canvas.hasActiveTextEditing() && numericEditorText() == QStringLiteral("1") &&
                runtime.serializeDocumentSession().contains("Kept label"),
            "blank right click commits an active serial number label and resets the same press");
}

void runScreenshotStylePersistenceFailureTest() {
    SnowCanvasWidget canvas;
    ScreenshotToolPalette palette(options());
    ScreenshotStyleBinding binding(palette, canvas, &palette, {},
                                   [](const SnowCanvasStyleEdit&) { return false; });
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text for failed save");
    auto style = canvas.canvasStyleToolbarState().textStyle;
    style.fontSize = 37;
    require(canvas.commitStyleEdit(SnowCanvasTextEdit{style, SnowCanvasTextStyleMixedFontSize}),
            "a storage failure does not invalidate a successful canvas edit");
    require(binding.lastSaveSucceeded() == false,
            "binding exposes the persistence dependency's failed save");
}

void runAngleStyleBindingTests() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.angle.decimalPlaces = 1;
    auto settings = options();
    settings.showAngleTool = true;
    settings.styleDefaults = defaults;
    ScreenshotToolPalette palette(settings);
    int saves = 0;
    ScreenshotStyleBinding binding(palette, canvas, &palette, {},
                                   [&saves](const SnowCanvasStyleEdit&) {
                                       ++saves;
                                       return true;
                                   });
    applyScreenshotCanvasToolStyles(canvas, defaults);
    canvas.resize(400, 300);
    canvas.setInteractionEnabled(true);
    canvas.show();
    QApplication::processEvents();
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    require(canvas.setCanvasTool(SnowCanvasTool::Angle), "activate angle style binding fixture");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Angle);
    const auto click = [&](QPointF point) {
        mouse(canvas, QEvent::MouseButtonPress, point);
        mouse(canvas, QEvent::MouseButtonRelease, point);
    };
    const auto label = [&] {
        for (const auto& slot : documentSlots(runtime)) {
            const auto text = slot.toObject()
                                  .value(QStringLiteral("data"))
                                  .toObject()
                                  .value(QStringLiteral("Text"))
                                  .toObject();
            if (!text.isEmpty())
                return text.value(QStringLiteral("text")).toString();
        }
        return QString();
    };
    const auto sourceWidth = [&] {
        for (const auto& slot : documentSlots(runtime)) {
            const auto arrow = slot.toObject()
                                   .value(QStringLiteral("data"))
                                   .toObject()
                                   .value(QStringLiteral("Arrow"))
                                   .toObject();
            if (!arrow.isEmpty())
                return arrow.value(QStringLiteral("stroke_width")).toDouble();
        }
        return 0.0;
    };
    click({300, 150});
    click({180, 150});
    click({180, 40});
    require(label() == QStringLiteral("90.0°"), "angle fixture has a generated measured label");
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select angle through selection tool");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    click({260, 150});
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedAngle,
            "selected angle exposes its dedicated style editor");
    WheelStepAccumulator accumulator;
    const auto hostWheel = [&](int delta, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                               Qt::ScrollPhase phase = Qt::NoScrollPhase) {
        QWheelEvent event({260, 150}, {260, 150}, {}, {0, delta}, Qt::NoButton, modifiers, phase,
                          false);
        event.setTimestamp(100);
        return handleScreenshotStyleWheel(palette, canvas, event, accumulator);
    };
    require(hostWheel(240) && label() == QStringLiteral("92.0°") && sourceWidth() == 2,
            "host wheel applies all coalesced angle steps ahead of selection opacity");
    require(hostWheel(120, Qt::ShiftModifier) && label() == QStringLiteral("92.1°"),
            "shift wheel uses a tenth-degree increment");
    const auto beforeControl = runtime.serializeDocumentSession();
    require(!hostWheel(120, Qt::ControlModifier) &&
                runtime.serializeDocumentSession() == beforeControl,
            "control wheel retains host navigation without editing the angle");
    require(hostWheel(120, Qt::NoModifier, Qt::ScrollMomentum) &&
                runtime.serializeDocumentSession() == beforeControl,
            "momentum consumes without changing discrete angle values");
    palette.angleValueAdjustmentRequested(-1, true);
    require(label() == QStringLiteral("92.0°"), "palette angle wheel signal reaches the canvas");
    auto selected = canvas.canvasAngleStyle();
    selected.strokeWidth = 7;
    selected.unit = SnowCanvasAngleUnit::Radians;
    const auto properties = static_cast<quint32>(SnowCanvasAngleStyleProperty::StrokeWidth) |
                            static_cast<quint32>(SnowCanvasAngleStyleProperty::Unit);
    selected.decimalPlaces = 3;
    require(canvas.commitStyleEdit(SnowCanvasAngleStyleEdit{
                selected, properties, SnowCanvasStyleEditTarget::Selection}) &&
                sourceWidth() == 7 && saves == 1,
            "selected angle styling changes the document and saves its property patch once");
    auto expectedDefaults = defaults;
    expectedDefaults.angle.strokeWidth = 7;
    expectedDefaults.angle.unit = SnowCanvasAngleUnit::Radians;
    require(palette.creationStyleDefaults() == expectedDefaults,
            "selected angle styling preserves untouched properties and other tool defaults");
    require(canvas.resetEditingState() && canvas.setCanvasTool(SnowCanvasTool::Angle) &&
                canvas.canvasAngleStyle() == expectedDefaults.angle,
            "selected styling updates future angle creation defaults property by property");
    selected = defaults.angle;
    selected.strokeWidth = 4;
    require(canvas.commitStyleEdit(SnowCanvasAngleStyleEdit{
                selected, static_cast<quint32>(SnowCanvasAngleStyleProperty::StrokeWidth)}) &&
                saves == 2 && palette.creationStyleDefaults().angle.strokeWidth == 4 &&
                palette.creationStyleDefaults().angle.unit == SnowCanvasAngleUnit::Radians,
            "creation style commits are remembered and saved once");

    click({150, 270});
    click({30, 270});
    click({30, 180});
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select among multiple angles");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    click({260, 150});
    const auto palettePixelWheel = [&] {
        QWheelEvent event({100, 30}, {100, 30}, {0, 1}, {}, Qt::NoButton, Qt::NoModifier,
                          Qt::ScrollUpdate, false);
        QApplication::sendEvent(&palette, &event);
    };
    const auto firstSelection = runtime.serializeDocumentHistory();
    palettePixelWheel();
    const auto firstStep = runtime.serializeDocumentHistory();
    require(firstStep != firstSelection,
            "selected angle responds to the first precise wheel delta");
    palettePixelWheel();
    require(runtime.serializeDocumentHistory() == firstStep,
            "subsequent precise deltas accumulate without extra angle steps");
    click({90, 270});
    const auto secondSelection = runtime.serializeDocumentHistory();
    palettePixelWheel();
    require(runtime.serializeDocumentHistory() != secondSelection,
            "changing selected angle resets the palette's fractional wheel remainder");
}

void runBracketStyleBindingTests() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(400, 300);
    canvas.setWheelZoomEnabled(false);
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.bracket.strokeWidth = 5;
    auto settings = options();
    settings.styleDefaults = defaults;
    ScreenshotToolPalette palette(settings);
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&]() { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    QObject::connect(&palette, &ScreenshotToolPalette::selectionOpacityChanged, &canvas,
                     [&](qreal opacity) {
                         require(canvas.setSelectedOpacity(opacity), "apply bracket opacity");
                     });
    int saves = 0;
    ScreenshotStyleBinding binding(palette, canvas, &palette, {}, [&](const SnowCanvasStyleEdit&) {
        ++saves;
        return true;
    });
    applyScreenshotCanvasToolStyles(canvas, defaults);
    require(canvas.setCanvasTool(SnowCanvasTool::Bracket), "activate shared bracket style binding");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Bracket);
    auto changed = defaults.bracket;
    changed.strokeWidth = 7;
    require(canvas.commitStyleEdit(SnowCanvasBracketStyleEdit{changed, 2}) && saves == 1 &&
                palette.creationStyleDefaults().bracket == changed &&
                canvas.canvasBracketStyle() == changed,
            "shared host binding applies and persists bracket style patches once");
    require(palette.creationStyleDefaults() != defaults,
            "style default equality detects independent bracket appearance changes");

    WheelStepAccumulator accumulator;
    const auto hostWheel = [&](int pixels, int angle,
                               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QWheelEvent event({200, 150}, {200, 150}, {0, pixels}, {0, angle}, Qt::NoButton, modifiers,
                          Qt::ScrollUpdate, false);
        return handleScreenshotStyleWheel(palette, canvas, event, accumulator);
    };
    const auto camera = canvas.canvasToViewTransform();
    const auto beforeWidth = canvas.canvasStyleToolbarState();
    require(hostWheel(0, 120) && canvas.canvasBracketStyle().strokeWidth == 8 && saves == 2,
            "canvas wheel routes bracket width through the shared style binding once");
    require(canvas.canvasStyleToolbarState() != beforeWidth,
            "toolbar state equality detects bracket appearance changes");
    auto mixedWidth = beforeWidth;
    mixedWidth.bracketStyleMixed =
        static_cast<quint32>(SnowCanvasBracketStyleProperty::StrokeWidth);
    require(mixedWidth != beforeWidth, "toolbar state equality detects mixed bracket properties");
    require(hostWheel(-1, 120) && canvas.canvasBracketStyle().strokeWidth == 7 && saves == 3,
            "precise bracket wheel direction takes precedence over angle delta");
    for (const auto modifiers : {Qt::ControlModifier, Qt::ShiftModifier, Qt::AltModifier})
        require(!hostWheel(0, 120, modifiers) && canvas.canvasBracketStyle().strokeWidth == 7 &&
                    saves == 3,
                "modified host wheels leave bracket style unchanged");
    require(canvas.canvasToViewTransform() == camera,
            "bracket style wheels leave the camera unchanged");

    require(runtime.setAutomaticallySelectNewlyCreatedElements(true), "select new bracket");
    mouse(canvas, QEvent::MouseButtonPress, {100, 50});
    mouse(canvas, QEvent::MouseMove, {180, 250});
    require(hostWheel(0, 120), "bracket width wheel updates the active creation preview");
    mouse(canvas, QEvent::MouseButtonRelease, {180, 250});
    require(canvas.canvasStyleToolbarState().source ==
                    SnowCanvasStyleToolbarSource::SelectedBracket &&
                canvas.canvasBracketStyle().strokeWidth == 8,
            "bracket commits with the preview's updated width");
    require(hostWheel(0, -120) && canvas.canvasBracketStyle().strokeWidth == 7,
            "shared host wheel edits the selected bracket");
    require(canvas.undo() && canvas.canvasBracketStyle().strokeWidth == 8 && canvas.redo() &&
                canvas.canvasBracketStyle().strokeWidth == 7,
            "selected bracket wheel styling undoes and redoes atomically");
    for (const double limit : {1.0, 72.0}) {
        auto style = canvas.canvasBracketStyle();
        style.strokeWidth = limit;
        require(canvas.commitStyleEdit(
                    SnowCanvasBracketStyleEdit{style, 2, SnowCanvasStyleEditTarget::Selection}),
                "prepare selected bracket width boundary");
        const int before = saves;
        require(hostWheel(0, limit == 1 ? -120 : 120) && saves == before &&
                    canvas.canvasBracketStyle().strokeWidth == limit,
                "bracket boundary wheel is consumed without duplicate edits or saves");
    }

    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select bracket opacity");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    require(canvas.setSelectedOpacity(0.25), "change bracket opacity independently of stroke");
    auto* opacity = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotSelectionOpacitySlider"));
    require(opacity != nullptr && opacity->value() == 25,
            "selected bracket opacity is published to the selection toolbar");
    require(hostWheel(0, -120) && opacity->value() == 20 &&
                canvas.canvasStyleToolbarState().shapeStyle.opacity == 0.2,
            "Select-tool wheels adjust bracket opacity rather than stroke width");
    require(canvas.undo() && opacity->value() == 25 && canvas.redo() && opacity->value() == 20,
            "bracket opacity undo and redo update the selection toolbar");
}
