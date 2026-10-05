#include "../../test-support/canvas_quick_selection_test_support.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QContextMenuEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>

using canvas_quick_selection_test::mouse;
using canvas_quick_selection_test::require;

namespace {
void strokeSelection() {
    SnowCanvasRuntime runtime;
    require(runtime.setQuickSelectionDisabledTools({SnowCanvasTool::FreeDraw}),
            "disable left pen selection");
    SnowCanvasWidget canvas(runtime);
    canvas.resize(400, 300);
    canvas.show();
    QApplication::processEvents();
    canvas_quick_selection_test::drawStroke(canvas);
    require(!canvas.hasQuickSelectionTargetAt({120, 100}, Qt::LeftButton),
            "left query respects setting");
    canvas_quick_selection_test::selectAndDragStroke(canvas);

    QContextMenuEvent menu(QContextMenuEvent::Mouse, {120, 135},
                           canvas.mapToGlobal(QPoint(120, 135)));
    menu.setAccepted(false);
    QApplication::sendEvent(&canvas, &menu);
    require(menu.isAccepted(), "consumed right gesture suppresses its mouse context menu");
    QContextMenuEvent keyboardMenu(QContextMenuEvent::Keyboard, {120, 135},
                                   canvas.mapToGlobal(QPoint(120, 135)));
    QApplication::sendEvent(&canvas, &keyboardMenu);
    require(!keyboardMenu.isAccepted(), "keyboard context menu remains unhandled by canvas");
    require(
        mouse(canvas, QEvent::MouseButtonDblClick, {120, 135}, Qt::RightButton, Qt::RightButton),
        "right double-click selects the stroke");
    mouse(canvas, QEvent::MouseButtonRelease, {120, 135}, Qt::RightButton, Qt::NoButton);
    QContextMenuEvent doubleClickMenu(QContextMenuEvent::Mouse, {120, 135},
                                      canvas.mapToGlobal(QPoint(120, 135)));
    doubleClickMenu.setAccepted(false);
    QApplication::sendEvent(&canvas, &doubleClickMenu);
    require(doubleClickMenu.isAccepted(), "right double-click consumes its mouse context menu");
    mouse(canvas, QEvent::MouseButtonPress, {25, 40}, Qt::RightButton, Qt::RightButton);
    mouse(canvas, QEvent::MouseButtonRelease, {25, 40}, Qt::RightButton, Qt::NoButton);
    QContextMenuEvent missMenu(QContextMenuEvent::Mouse, {25, 40},
                               canvas.mapToGlobal(QPoint(25, 40)));
    QApplication::sendEvent(&canvas, &missMenu);
    require(!missMenu.isAccepted(), "miss leaves context menu available to host");
    require(runtime.selectedElementIds().size() == 1, "miss retains element selection");
}

void textCommitAndMove() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(500, 300);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text");
    mouse(canvas, QEvent::MouseButtonPress, {200, 150}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {200, 150}, Qt::LeftButton, Qt::NoButton);
    QKeyEvent type(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("initial text"));
    QApplication::sendEvent(&canvas, &type);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "commit initial text");
    const QJsonArray records = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                                   .object()
                                   .value(QStringLiteral("document"))
                                   .toObject()
                                   .value(QStringLiteral("slots"))
                                   .toArray();
    const auto center = records.first()
                            .toObject()
                            .value(QStringLiteral("data"))
                            .toObject()
                            .value(QStringLiteral("Text"))
                            .toObject()
                            .value(QStringLiteral("center"))
                            .toObject();
    const QPointF point =
        canvas.canvasToViewTransform().map(QPointF(center.value(QStringLiteral("x")).toDouble(),
                                                   center.value(QStringLiteral("y")).toDouble()));
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text tool for draft selection");
    mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    require(canvas.hasActiveTextEditing(), "open existing text draft");
    QKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(&canvas, &selectAll);
    QKeyEvent replacement(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
                          QStringLiteral("updated text"));
    QApplication::sendEvent(&canvas, &replacement);
    require(runtime.setQuickSelectionDisabledTools({SnowCanvasTool::Text}),
            "disable left text selection");
    mouse(canvas, QEvent::MouseButtonPress, point, Qt::RightButton, Qt::RightButton);
    require(!canvas.hasActiveTextEditing(), "right selection commits draft on same press");
    mouse(canvas, QEvent::MouseMove, point + QPointF(0, 40), Qt::NoButton, Qt::RightButton);
    mouse(canvas, QEvent::MouseButtonRelease, point + QPointF(0, 40), Qt::RightButton,
          Qt::NoButton);
    require(!canvas.hasActiveTextEditing() &&
                runtime.serializeDocumentSession().contains("updated text") &&
                runtime.selectedElementIds().size() == 1,
            "right movement preserves committed text and selection without opening editor");
}

void linearCreationConsumesContextMenu() {
    for (const auto tool : {SnowCanvasTool::Arrow, SnowCanvasTool::Line}) {
        for (const auto type : {SnowCanvasArrowType::Straight, SnowCanvasArrowType::Curve}) {
            for (const bool contextBeforeRelease : {false, true}) {
                SnowCanvasRuntime runtime;
                SnowCanvasWidget canvas(runtime);
                canvas.resize(400, 300);
                canvas.show();
                QApplication::processEvents();
                canvas_quick_selection_test::beginLinearCreation(canvas, tool, type);
                require(mouse(canvas, QEvent::MouseButtonPress, {25, 180}, Qt::RightButton,
                              Qt::RightButton),
                        "right click finishes linear creation");
                const auto contextMenu = [&]() {
                    QContextMenuEvent keyboard(QContextMenuEvent::Keyboard, {25, 180},
                                               canvas.mapToGlobal(QPoint(25, 180)));
                    QApplication::sendEvent(&canvas, &keyboard);
                    require(!keyboard.isAccepted(), "keyboard menu remains available to host");
                    QContextMenuEvent menu(QContextMenuEvent::Mouse, {25, 180},
                                           canvas.mapToGlobal(QPoint(25, 180)));
                    menu.setAccepted(false);
                    QApplication::sendEvent(&canvas, &menu);
                    require(menu.isAccepted(), "finishing creation consumes its mouse menu");
                };
                if (contextBeforeRelease)
                    contextMenu();
                mouse(canvas, QEvent::MouseButtonRelease, {25, 180}, Qt::RightButton, Qt::NoButton);
                if (!contextBeforeRelease)
                    contextMenu();
                require(canvas.canvasHistoryState().canUndo && canvas.undo() &&
                            !canvas.canvasHistoryState().canUndo &&
                            canvas.canvasHistoryState().canRedo,
                        "finishing linear creation commits one undoable operation");
                mouse(canvas, QEvent::MouseButtonPress, {25, 180}, Qt::RightButton,
                      Qt::RightButton);
                mouse(canvas, QEvent::MouseButtonRelease, {25, 180}, Qt::RightButton, Qt::NoButton);
                QContextMenuEvent miss(QContextMenuEvent::Mouse, {25, 180},
                                       canvas.mapToGlobal(QPoint(25, 180)));
                QApplication::sendEvent(&canvas, &miss);
                require(!miss.isAccepted(), "the next idle right click remains available to host");
            }
        }
    }
}

void textCommitConsumesContextMenu() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(400, 300);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text draft fixture");
    mouse(canvas, QEvent::MouseButtonPress, {200, 150}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {200, 150}, Qt::LeftButton, Qt::NoButton);
    QKeyEvent type(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("completed draft"));
    QApplication::sendEvent(&canvas, &type);
    require(canvas.hasActiveTextEditing() &&
                !canvas.hasQuickSelectionTargetAt({25, 40}, Qt::RightButton),
            "right click outside the editor is not a selection hit");
    require(mouse(canvas, QEvent::MouseButtonPress, {25, 40}, Qt::RightButton, Qt::RightButton) &&
                !canvas.hasActiveTextEditing() &&
                runtime.serializeDocumentSession().contains("completed draft"),
            "canvas right click commits the text draft");
    mouse(canvas, QEvent::MouseButtonRelease, {25, 40}, Qt::RightButton, Qt::NoButton);
    QContextMenuEvent menu(QContextMenuEvent::Mouse, {25, 40}, canvas.mapToGlobal(QPoint(25, 40)));
    menu.setAccepted(false);
    QApplication::sendEvent(&canvas, &menu);
    require(menu.isAccepted(), "text commit consumes its mouse context menu");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    strokeSelection();
    textCommitAndMove();
    linearCreationConsumesContextMenu();
    textCommitConsumesContextMenu();
    return 0;
}
