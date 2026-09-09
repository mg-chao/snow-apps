#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenrecordingshortcutcontroller.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "../src/presentation/recording/screenrecordingselection.h"
#include "widgets/button.h"

#include <QApplication>
#include <QDir>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLayout>
#include <QMouseEvent>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

class ScreenshotFloatingToolPaletteWindowTestAccess {
  public:
    static void edit(ScreenshotFloatingToolPaletteWindow& toolbar, QWidget& editor) {
        toolbar.beginKeyboardFocusInteraction(&editor);
        toolbar.endKeyboardFocusInteraction(&editor);
    }
};

class ScreenRecordingAreaWindowTestAccess {
  public:
    static QByteArray history(const ScreenRecordingAreaWindow& area) {
        return area.m_canvasRuntime->serializeDocumentHistory();
    }
};

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void press(QWidget& receiver, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
           bool autoRepeat = false) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), autoRepeat);
    QCoreApplication::sendEvent(&receiver, &event);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QCoreApplication::sendEvent(&receiver, &release);
}

void focus(QWidget& widget) {
    widget.window()->activateWindow();
    widget.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
}

void drawRectangle(SnowCanvasWidget& canvas) {
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "shape should activate");
    const auto mouse = [&canvas](QEvent::Type type, QPointF position, Qt::MouseButton button,
                                 Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &event);
    };
    mouse(QEvent::MouseButtonPress, {30, 30}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, {100, 80}, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {100, 80}, Qt::LeftButton, Qt::NoButton);
    require(canvas.canvasHistoryState().canUndo, "drawing should create undo history");
}

void requireFocusPolicy(QWidget& widget, bool acceptsFocus) {
    require(widget.focusPolicy() == (acceptsFocus ? Qt::StrongFocus : Qt::NoFocus) &&
                widget.testAttribute(Qt::WA_ShowWithoutActivating) != acceptsFocus,
            "window focus policy must survive preparation and text editing");
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const auto style = GetWindowLongPtrW(reinterpret_cast<HWND>(widget.winId()), GWL_EXSTYLE);
        require(((style & WS_EX_NOACTIVATE) == 0) == acceptsFocus,
                "native activation policy must match recording keyboard availability");
    }
#endif
}

void recordingToolbarTakesFocusWhenOpenedOrStarted() {
    ScreenRecordingToolbarWindow toolbar;
    QLineEdit otherWindow;
    otherWindow.show();
    for (const bool alreadyVisible : {false, true}) {
        if (alreadyVisible) {
            toolbar.show();
        }
        focus(otherWindow);
        require(QApplication::focusWidget() == &otherWindow,
                "another window must own focus before recording opens or starts");
        toolbar.showAndActivate();
        QCoreApplication::processEvents();
        require(toolbar.isActiveWindow() && QApplication::focusWidget() == &toolbar,
                "opening or starting recording must activate and focus the toolbar");
    }
}

void recordingToolbarKeepsFocusAfterEditingAndSurfaceRestoration() {
    ScreenRecordingToolbarWindow recording;
    ScreenshotFloatingToolPaletteWindow screenshot{ScreenshotToolPalette::Options{}};
    for (auto* toolbar :
         {static_cast<ScreenshotFloatingToolPaletteWindow*>(&recording), &screenshot}) {
        const bool acceptsFocus = toolbar == &recording;
        toolbar->prepareForDisplay();
        toolbar->show();
        QCoreApplication::processEvents();
        requireFocusPolicy(*toolbar, acceptsFocus);
        QLineEdit editor(toolbar);
        editor.show();
        ScreenshotFloatingToolPaletteWindowTestAccess::edit(*toolbar, editor);
        requireFocusPolicy(*toolbar, acceptsFocus);
        toolbar->releaseNativeSurface();
        toolbar->restoreNativeSurface();
        toolbar->show();
        QCoreApplication::processEvents();
        requireFocusPolicy(*toolbar, acceptsFocus);
    }
}

void recordingSelectionEditsAnnotationsAndPreservesPassThrough() {
    ScreenRecordingAreaWindow area;
    ScreenRecordingToolbarWindow toolbar;
    area.setPhysicalRegion(
        ScreenshotGeometryMapper::physicalRectForScreen(*QGuiApplication::primaryScreen())
            .adjusted(20, 20, -20, -20));
    auto& palette = *toolbar.palette();
    auto& canvas = *area.canvas();
    snow_shot::presentation::recording::connectScreenRecordingSelection(palette, area, area);
    area.show();
    toolbar.show();
    toolbar.prepareForDisplay();
    QCoreApplication::processEvents();

    const auto findButton = [&palette](const QString& source) {
        for (auto* button : palette.findChildren<adqt::widgets::AdButton*>()) {
            if (button->property("snowShotTranslationTooltipSource").toString() == source) {
                return button;
            }
        }
        return static_cast<adqt::widgets::AdButton*>(nullptr);
    };
    auto* select = findButton(QStringLiteral("Select elements"));
    auto* settings = findButton(QStringLiteral("Export Settings"));
    require(select != nullptr && settings != nullptr && select->isVisible(),
            "the recording toolbar must expose Select elements");
    QLayout* layout = palette.mainPanel()->layout();
    const int exportIndex = layout->indexOf(settings);
    const int selectIndex = layout->indexOf(select);
    require(exportIndex >= 0 && selectIndex > exportIndex,
            "Select elements must follow Export Settings in the main toolbar");
    for (int index = exportIndex + 1; index < selectIndex; ++index) {
        require(layout->itemAt(index)->widget() == nullptr,
                "Export Settings and Select elements must share a group without a separator");
    }
    QWidget* nextWidget = nullptr;
    for (int index = selectIndex + 1; index < layout->count() && nextWidget == nullptr; ++index) {
        nextWidget = layout->itemAt(index)->widget();
    }
    require(qobject_cast<QFrame*>(nextWidget) != nullptr &&
                nextWidget->x() > select->geometry().right(),
            "the recording group separator must appear immediately after Select elements");
    for (int index = exportIndex + 1; index < layout->count(); ++index) {
        auto* nextButton = qobject_cast<adqt::widgets::AdButton*>(layout->itemAt(index)->widget());
        if (nextButton != nullptr) {
            require(nextButton == select && select->x() > settings->geometry().right(),
                    "Select elements must be the first tool to the right of Export Settings");
            break;
        }
    }

    for (const auto state : {ScreenshotToolPalette::RecordingState::Idle,
                             ScreenshotToolPalette::RecordingState::Recording,
                             ScreenshotToolPalette::RecordingState::Paused}) {
        palette.setRecordingState(state);
        area.setRecordingState(state);
        if (!palette.recordingExportSettingsVisible()) {
            settings->click();
        }
        select->click();
        require(palette.activeTool() == ScreenshotToolPalette::Tool::Select &&
                    !palette.recordingExportSettingsVisible() &&
                    canvas.canvasTool() == SnowCanvasTool::Select && canvas.interactionEnabled() &&
                    area.inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing,
                "Select must replace Export Settings and enable annotation interaction");
        require(select->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
                "Select must retain its active appearance");
        area.setDrawingBlocked(true);
        require(!canvas.interactionEnabled(), "busy operations must block selection interaction");
        area.setDrawingBlocked(false);
        require(canvas.interactionEnabled(), "selection must resume after busy operations");
        select->click();
        require(!palette.activeTool().has_value() && !canvas.interactionEnabled() &&
                    area.inputMode() == ScreenRecordingAreaWindow::InputMode::RegionEditing &&
                    palette.recordingExportSettingsVisible(),
                "clicking active Select must return to Export Settings");
        select->click();
        settings->click();
        require(!palette.activeTool().has_value() && palette.recordingExportSettingsVisible() &&
                    area.inputMode() == ScreenRecordingAreaWindow::InputMode::RegionEditing,
                "Export Settings must leave selection and restore region editing mode");
        palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
        require(palette.activateToolShortcut(ScreenshotToolPalette::Tool::Select) &&
                    canvas.canvasTool() == SnowCanvasTool::Select && canvas.interactionEnabled(),
                "explicit selection activation must leave a drawing tool");
        palette.clearActiveTool();
        emit palette.selectRequested();
        require(area.inputMode() == ScreenRecordingAreaWindow::InputMode::PassThrough,
                "deactivating drawing must keep the existing pass-through behavior");
        require(palette.activateDrawingShortcut(QStringLiteral("shape")) &&
                    palette.activeTool() == ScreenshotToolPalette::Tool::Shape,
                "drawing shortcut must activate its tool");
        require(palette.activateDrawingShortcut(QStringLiteral("shape")) &&
                    !palette.activeTool().has_value() && palette.recordingExportSettingsVisible() &&
                    area.inputMode() == ScreenRecordingAreaWindow::InputMode::RegionEditing,
                "repeating a drawing shortcut must switch only to Export Settings");
        settings->click();
        require(palette.recordingExportSettingsVisible(),
                "repeated Export Settings must stay selected");
    }

    select->click();
    drawRectangle(canvas);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "selection tool should activate");
    const auto clickCanvas = [&canvas](QPointF position) {
        QMouseEvent down(QEvent::MouseButtonPress, position, position, position, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &down);
        QMouseEvent up(QEvent::MouseButtonRelease, position, position, position, Qt::LeftButton,
                       Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &up);
    };
    clickCanvas({30, 55});
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedRectangle,
            "Select must select an existing recording annotation");
    palette.prepareForDisplay();
    auto* duplicate = findButton(QStringLiteral("Copy selected elements"));
    auto* remove = findButton(QStringLiteral("Delete selected elements"));
    require(duplicate != nullptr && remove != nullptr && duplicate->isEnabled() &&
                remove->isEnabled(),
            "selection actions must become available for a selected annotation");
    const auto expectChange = [&area](const auto& action) {
        const QByteArray before = ScreenRecordingAreaWindowTestAccess::history(area);
        action();
        require(ScreenRecordingAreaWindowTestAccess::history(area) != before,
                "selection actions must change recording canvas history");
    };
    expectChange([&]() { emit palette.selectionOpacityChanged(0.5); });
    expectChange([&]() { duplicate->click(); });
    expectChange([&]() { emit palette.sendSelectionToBackRequested(); });
    expectChange([&]() { emit palette.bringSelectionToFrontRequested(); });
    expectChange([&]() { emit palette.sendSelectionBackwardRequested(); });
    expectChange([&]() { emit palette.bringSelectionForwardRequested(); });
    expectChange([&]() { remove->click(); });
    clickCanvas({30, 55});
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedRectangle,
            "deleting a duplicated annotation must preserve the original");
    remove->click();
    clickCanvas({30, 55});
    require(canvas.canvasStyleToolbarState().source !=
                SnowCanvasStyleToolbarSource::SelectedRectangle,
            "Delete must remove the selected recording annotation");
    auto* reset =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotResetCanvasButton"));
    require(reset != nullptr, "recording selection should expose canvas reset");
    for (const bool selected : {false, true}) {
        drawRectangle(canvas);
        require(canvas.setCanvasTool(SnowCanvasTool::Select), "Select should activate");
        clickCanvas({30, 55});
        require(canvas.duplicateSelected(), "reset fixture should contain multiple elements");
        if (!selected) {
            require(canvas.resetEditingState(), "reset fixture should clear selection");
        }
        require(reset->isEnabled(), "recording reset must work with or without selection");
        reset->click();
        require(!canvas.canvasHistoryState().canUndo && !canvas.canvasHistoryState().canRedo,
                "recording reset should clear canvas history");
        clickCanvas({30, 55});
        require(canvas.canvasStyleToolbarState().source !=
                    SnowCanvasStyleToolbarSource::SelectedRectangle,
                "recording reset must remove unselected annotations as well");
        reset->click();
        require(!canvas.canvasHistoryState().canUndo,
                "resetting an empty recording canvas should be harmless");
    }
}

void recordingControlShortcutsFollowButtonsAndSettings() {
    const snow_shot::storage::ScreenRecordingShortcutSettings settings;
    const auto defaults = settings.allShortcuts();
    require(defaults.size() == 4 &&
                defaults.value(QStringLiteral("export")) == QStringList{QStringLiteral("Ctrl+E")} &&
                defaults.value(QStringLiteral("toggle_recording")) ==
                    QStringList{QStringLiteral("Ctrl+S")} &&
                defaults.value(QStringLiteral("copy_to_clipboard")) ==
                    QStringList{QStringLiteral("Ctrl+C")} &&
                defaults.value(QStringLiteral("end_recording")) ==
                    QStringList{QStringLiteral("Esc")},
            "recording controls must have the requested defaults");
    ScreenRecordingAreaWindow area;
    ScreenRecordingToolbarWindow toolbar;
    area.setPhysicalRegion(
        ScreenshotGeometryMapper::physicalRectForScreen(*QGuiApplication::primaryScreen())
            .adjusted(20, 20, -20, -20));
    auto& palette = *toolbar.palette();
    ScreenRecordingShortcutController controller(area, toolbar);
    int starts = 0, exports = 0, pauses = 0, resumes = 0, copies = 0, ends = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingStartRequested, &area,
                     [&]() { ++starts; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingStopRequested, &area,
                     [&]() { ++exports; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingPauseRequested, &area,
                     [&]() { ++pauses; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingResumeRequested, &area,
                     [&]() { ++resumes; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingCopyRequested, &area,
                     [&]() { ++copies; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingCloseRequested, &area,
                     [&]() { ++ends; });
    const auto total = [&]() { return starts + exports + pauses + resumes + copies + ends; };
    const auto pressAll = [&](QWidget& receiver, bool repeat = false) {
        press(receiver, Qt::Key_E, Qt::ControlModifier, repeat);
        press(receiver, Qt::Key_S, Qt::ControlModifier, repeat);
        press(receiver, Qt::Key_C, Qt::ControlModifier, repeat);
        press(receiver, Qt::Key_Escape, Qt::NoModifier, repeat);
    };
    area.show();
    toolbar.show();
    focus(toolbar);
    pressAll(toolbar);
    require(starts == 1 && ends == 1 && exports == 0 && copies == 0 && pauses == 0 && resumes == 0,
            "idle shortcuts must only start or end the recording session");
    palette.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
    pressAll(area);
    require(starts == 1 && exports == 1 && pauses == 1 && resumes == 0 && copies == 1 && ends == 2,
            "recording shortcuts must export, pause, copy or end exactly once");
    palette.setRecordingState(ScreenshotToolPalette::RecordingState::Paused);
    pressAll(*area.canvas());
    require(exports == 2 && pauses == 1 && resumes == 1 && copies == 2 && ends == 3,
            "paused shortcuts must export, resume, copy or end exactly once");
    const int before = total();
    pressAll(toolbar, true);
    palette.setRecordingBusy(true);
    pressAll(toolbar);
    palette.setRecordingBusy(false);
    area.setDrawingBlocked(true);
    pressAll(area);
    area.setDrawingBlocked(false);
    require(total() == before,
            "busy operations and key repeat must not trigger recording commands");
    QLineEdit editor(&toolbar);
    editor.setText(QStringLiteral("recording shortcut text"));
    editor.show();
    focus(editor);
    pressAll(editor);
    editor.hide();
    focus(toolbar);
    require(total() == before, "editable controls must retain recording shortcut input");
    require(area.canvas()->setCanvasTool(SnowCanvasTool::Text), "text tool should activate");
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    const QPointF position(160, 100);
    QMouseEvent down(QEvent::MouseButtonPress, position, position, position, Qt::LeftButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, position, position, position, Qt::LeftButton,
                   Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(area.canvas(), &down);
    QCoreApplication::sendEvent(area.canvas(), &up);
    require(area.canvas()->hasActiveTextEditing(), "canvas text editing should start");
    pressAll(*area.canvas());
    require(total() == before, "canvas text editing must not dispatch recording commands");
    static_cast<void>(area.canvas()->cancelActiveTextEditing());
    focus(toolbar);
    QWidget unrelated;
    unrelated.show();
    pressAll(unrelated);
    toolbar.hide();
    pressAll(area);
    toolbar.show();
    area.hide();
    pressAll(toolbar);
    area.show();
    focus(toolbar);
    require(total() == before, "unrelated and hidden windows must not dispatch recording commands");
    require(!palette.activateRecordingShortcut(QStringLiteral("unsupported")),
            "unknown recording actions must not activate a button");
    const auto findButton = [&](const QString& name) {
        for (auto* button : palette.findChildren<adqt::widgets::AdButton*>()) {
            if (button->accessibleName() == name) {
                return button;
            }
        }
        return static_cast<adqt::widgets::AdButton*>(nullptr);
    };
    auto* exportButton = findButton(QStringLiteral("Stop recording"));
    require(exportButton != nullptr && exportButton->toolTip().contains(QStringLiteral("Ctrl+E")),
            "export tooltip must show its configured shortcut");
    require(settings.setShortcuts(QStringLiteral("export"),
                                  {QStringLiteral("F12"), QStringLiteral("Ctrl+F12")}),
            "recording actions must support two configurable keys");
    press(toolbar, Qt::Key_E, Qt::ControlModifier);
    require(total() == before, "reconfiguration must remove the old binding immediately");
    press(toolbar, Qt::Key_F12);
    palette.clearActiveTool();
    QWidget popup(&toolbar, Qt::Tool);
    emit palette.materializedScope(&popup);
    popup.show();
    press(popup, Qt::Key_F12, Qt::ControlModifier);
    popup.hide();
    require(exports == 4 && exportButton->toolTip().contains(QStringLiteral("F12")) &&
                !exportButton->toolTip().contains(QStringLiteral("Ctrl+E")),
            "both new keys and tooltip hints must update immediately, including popups");
    require(settings.setShortcuts(QStringLiteral("export"), {}),
            "recording shortcut may be cleared");
    focus(toolbar);
    press(toolbar, Qt::Key_F12);
    require(exports == 4 && exportButton->toolTip() == QStringLiteral("Stop recording"),
            "cleared shortcuts must remove bindings and hints");
    require(settings.setShortcuts(QStringLiteral("export"), {QStringLiteral("F11")}) &&
                snow_shot::storage::DrawingShortcutSettings().setArrow({QStringLiteral("F11")}),
            "recording and drawing shortcuts can share keys across scopes");
    int arrows = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::arrowRequested, &area, [&]() { ++arrows; });
    press(toolbar, Qt::Key_F11);
    require(exports == 5 && arrows == 0,
            "recording buttons must take priority over drawing actions");
    palette.setRecordingState(ScreenshotToolPalette::RecordingState::Idle);
    press(toolbar, Qt::Key_F11);
    require(exports == 5 && arrows == 1,
            "unavailable recording buttons must allow drawing fallback");
    require(settings.setAllShortcutsAtomic(defaults), "recording defaults must be restorable");
}

void recordingShortcutsFollowBothWindowsAndConfiguredKeys() {
    const snow_shot::storage::DrawingShortcutSettings drawing;
    const snow_shot::storage::ScreenshotShortcutSettings history;
    require(drawing.setShape({QStringLiteral("F6")}) && drawing.setArrow({QStringLiteral("F7")}) &&
                drawing.setShortcuts(QStringLiteral("highlight"), {QStringLiteral("F8")}) &&
                drawing.setShortcuts(QStringLiteral("filter"), {QStringLiteral("F9")}) &&
                history.setShortcuts(QStringLiteral("undo"), {QStringLiteral("Ctrl+Z")}) &&
                history.setShortcuts(QStringLiteral("redo"), {QStringLiteral("Ctrl+Y")}),
            "test shortcut settings should be accepted");

    ScreenRecordingAreaWindow area;
    ScreenRecordingToolbarWindow toolbar;
    area.setPhysicalRegion(
        ScreenshotGeometryMapper::physicalRectForScreen(*QGuiApplication::primaryScreen())
            .adjusted(20, 20, -20, -20));
    auto* palette = toolbar.palette();
    auto* canvas = area.canvas();
    int shapes = 0;
    int arrows = 0;
    int undos = 0;
    int redos = 0;
    snow_shot::presentation::recording::connectScreenRecordingSelection(*palette, area, area);
    QObject::connect(palette, &ScreenshotToolPalette::shapeRequested, &area, [&]() {
        ++shapes;
        static_cast<void>(canvas->setCanvasTool(SnowCanvasTool::Shape));
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    });
    QObject::connect(palette, &ScreenshotToolPalette::arrowRequested, &area, [&]() {
        ++arrows;
        static_cast<void>(canvas->setCanvasTool(SnowCanvasTool::Arrow));
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    });
    QObject::connect(palette, &ScreenshotToolPalette::undoRequested, &area, [&]() {
        ++undos;
        static_cast<void>(canvas->undo());
    });
    QObject::connect(palette, &ScreenshotToolPalette::redoRequested, &area, [&]() {
        ++redos;
        static_cast<void>(canvas->redo());
    });

    auto shortcuts = std::make_unique<ScreenRecordingShortcutController>(area, toolbar);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.show();
    toolbar.show();
    focus(toolbar);
    require(!toolbar.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus) &&
                !toolbar.testAttribute(Qt::WA_ShowWithoutActivating) &&
                toolbar.focusPolicy() == Qt::StrongFocus && toolbar.hasFocus(),
            "recording toolbar must accept focus before selecting any drawing tool");
    require(area.focusPolicy() == Qt::StrongFocus &&
                !area.testAttribute(Qt::WA_ShowWithoutActivating),
            "idle recording area must accept keyboard input for region editing");

    for (QWidget* receiver : {static_cast<QWidget*>(&toolbar), static_cast<QWidget*>(&area),
                              static_cast<QWidget*>(canvas), palette->mainPanel()}) {
        for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                                ScreenRecordingAreaWindow::InputMode::RegionEditing,
                                ScreenRecordingAreaWindow::InputMode::Drawing}) {
            palette->clearActiveTool();
            area.setInputMode(mode);
            palette->clearActiveTool();
            focus(toolbar);
            const int before = shapes;
            press(*receiver, Qt::Key_F6);
            require(shapes == before + 1 &&
                        area.inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing,
                    "each recording scope must activate the first drawing tool exactly once");
        }
    }
    for (const auto state : {ScreenshotToolPalette::RecordingState::Recording,
                             ScreenshotToolPalette::RecordingState::Paused}) {
        palette->clearActiveTool();
        area.setRecordingState(state);
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
        palette->clearActiveTool();
        require(area.focusPolicy() == Qt::NoFocus &&
                    area.testAttribute(Qt::WA_TransparentForMouseEvents),
                "recording pass-through must retain mouse and keyboard pass-through");
        focus(toolbar);
        const int before = arrows;
        press(toolbar, Qt::Key_F7);
        require(arrows == before + 1, "toolbar shortcuts should enter drawing while recording");
    }
    area.setRecordingState(ScreenshotToolPalette::RecordingState::Idle);

    const auto previousTool = canvas->canvasTool();
    press(toolbar, Qt::Key_F8);
    press(area, Qt::Key_F9);
    require(canvas->canvasTool() == previousTool,
            "recording shortcuts must not activate unavailable highlight or filter tools");

    drawRectangle(*canvas);
    focus(toolbar);
    press(toolbar, Qt::Key_Z, Qt::ControlModifier);
    require(undos == 1 && canvas->canvasHistoryState().canRedo,
            "toolbar undo should operate on recording canvas history");
    press(area, Qt::Key_Y, Qt::ControlModifier);
    require(redos == 1 && canvas->canvasHistoryState().canUndo,
            "area redo should operate on the same recording canvas history");
    press(*canvas, Qt::Key_Z, Qt::ControlModifier);
    press(toolbar, Qt::Key_Z, Qt::ControlModifier);
    require(undos == 2, "unavailable history actions must not emit toolbar commands");
    require(history.setShortcuts(QStringLiteral("redo"), {QStringLiteral("Ctrl+F10")}),
            "redo shortcut should be reconfigurable while recording UI is open");
    press(toolbar, Qt::Key_Y, Qt::ControlModifier);
    require(redos == 1, "old history binding must be removed immediately");
    press(toolbar, Qt::Key_F10, Qt::ControlModifier);
    require(redos == 2, "updated history shortcut should work immediately");

    require(history.setShortcuts(QStringLiteral("undo"), {QStringLiteral("F11")}) &&
                drawing.setArrow({QStringLiteral("F11")}),
            "drawing and history shortcut collision should be configurable");
    palette->clearActiveTool();
    const int arrowsBeforeUndo = arrows;
    press(toolbar, Qt::Key_F11);
    require(undos == 3 && arrows == arrowsBeforeUndo,
            "available history action must take priority over drawing tools");
    palette->clearActiveTool();
    press(toolbar, Qt::Key_F11);
    require(undos == 3 && arrows == arrowsBeforeUndo + 1,
            "unavailable history action must fall through to a matching drawing shortcut");
    require(history.setShortcuts(QStringLiteral("undo"), {QStringLiteral("Ctrl+Z")}),
            "undo shortcut should restore after testing priority");

    require(drawing.setShape({QStringLiteral("F10")}), "drawing shortcut should be configurable");
    int before = shapes;
    press(toolbar, Qt::Key_F6);
    require(shapes == before, "old drawing binding must be removed immediately");
    press(toolbar, Qt::Key_F10);
    require(shapes == ++before, "updated drawing shortcut should work immediately");
    press(toolbar, Qt::Key_F10, Qt::NoModifier, true);
    require(shapes == before, "held drawing keys must not repeatedly activate tools");
    press(toolbar, Qt::Key_F10);
    require(
        shapes == before && !palette->activeTool().has_value() &&
            palette->recordingExportSettingsVisible() &&
            area.inputMode() == ScreenRecordingAreaWindow::InputMode::RegionEditing,
        "repeating a recording tool shortcut must return to Export Settings like a button click");

    area.setDrawingBlocked(true);
    press(toolbar, Qt::Key_F10);
    press(area, Qt::Key_Z, Qt::ControlModifier);
    require(shapes == before && undos == 3, "busy recording operations must block shortcuts");
    area.setDrawingBlocked(false);

    QLineEdit editor(&toolbar);
    editor.show();
    focus(editor);
    require(editor.hasFocus(), "editable toolbar control should own keyboard focus");
    press(editor, Qt::Key_F10);
    press(editor, Qt::Key_Z, Qt::ControlModifier);
    require(shapes == before && undos == 3, "text editors must retain shortcut input");
    editor.setReadOnly(true);
    press(editor, Qt::Key_F10);
    require(shapes == ++before && palette->activeTool() == ScreenshotToolPalette::Tool::Shape,
            "read-only text controls must allow drawing tool activation");
    press(editor, Qt::Key_F10);
    require(
        shapes == before && palette->recordingExportSettingsVisible() &&
            !palette->activeTool().has_value(),
        "read-only text controls must allow the active-tool shortcut to return to Export Settings");
    editor.hide();
    focus(toolbar);

    require(canvas->setCanvasTool(SnowCanvasTool::Text), "text tool should activate");
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    const QPointF textPosition(160, 100);
    QMouseEvent textPress(QEvent::MouseButtonPress, textPosition, textPosition, textPosition,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent textRelease(QEvent::MouseButtonRelease, textPosition, textPosition, textPosition,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &textPress);
    QCoreApplication::sendEvent(canvas, &textRelease);
    require(canvas->hasActiveTextEditing(), "canvas should begin text editing");
    press(*canvas, Qt::Key_F10);
    require(shapes == before, "active canvas text editing must retain drawing shortcut input");
    static_cast<void>(canvas->cancelActiveTextEditing());
    focus(toolbar);

    QWidget unrelated;
    unrelated.show();
    press(unrelated, Qt::Key_F10);
    require(shapes == before, "unrelated windows must not dispatch recording shortcuts");
    palette->clearActiveTool();
    QWidget popup(&toolbar, Qt::Tool);
    emit palette->materializedScope(&popup);
    popup.show();
    palette->clearActiveTool();
    press(popup, Qt::Key_F10);
    require(shapes == ++before, "materialized toolbar scopes should share recording shortcuts");
    popup.hide();
    press(popup, Qt::Key_F10);
    require(shapes == before, "hidden popups must not dispatch recording shortcuts");
    toolbar.hide();
    press(area, Qt::Key_F10);
    require(shapes == before, "hidden recording UI must not dispatch shortcuts");
    toolbar.show();
    focus(toolbar);
    shortcuts.reset();
    press(toolbar, Qt::Key_F10);
    require(shapes == before, "destroyed shortcut controller must release all bindings");
    palette->clearActiveTool();
    shortcuts = std::make_unique<ScreenRecordingShortcutController>(area, toolbar);
    palette->clearActiveTool();
    press(toolbar, Qt::Key_F10);
    require(shapes == ++before, "recreated shortcut controller must dispatch exactly once");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated test storage must be available");
    const QString executableDirectory = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executableDirectory), "test executable directory must exist");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({executableDirectory, temporary.path(), 60000}).success,
            "isolated recording shortcut settings must initialize");
    recordingToolbarTakesFocusWhenOpenedOrStarted();
    recordingToolbarKeepsFocusAfterEditingAndSurfaceRestoration();
    recordingSelectionEditsAnnotationsAndPreservesPassThrough();
    recordingControlShortcutsFollowButtonsAndSettings();
    recordingShortcutsFollowBothWindowsAndConfiguredKeys();
    storage.shutdown();
    return 0;
}
