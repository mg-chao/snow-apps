#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotoverlaycanvaspresenter.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotoverlayinteractionadapter.h"
#include "snow_shot/presentation/screenshotoverlayshortcutcontroller.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotselectioneditworkflow.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionsettingsstore.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_draw_engine_qt/snow_canvas_cursor.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "icon_renderer.h"
#include "physical_key_test_support.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
using Handle = ScreenshotSelectionEffectHandle;

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct Fixture {
    QObject parent;
    ScreenshotCaptureState capture;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotSelectionSettingsStore settings;
    ScreenshotOverlayEventAdapter adapter;
    SnowCanvasWidget* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay{adapter, canvas};
    std::unique_ptr<ScreenshotSelectionEditWorkflow> workflow;
    std::unique_ptr<ScreenshotOverlayInputHandler> input;
    int writes = 0;
    int aspectRatioWrites = 0;
    int updates = 0;
    int captureCancellations = 0;
    int toolbarRadius = 0;
    int toolbarShadow = 0;

    Fixture() {
        CapturedDisplayModel display;
        display.active = true;
        display.canvasUsesPoints = true;
        display.physicalRect = display.logicalRect = display.canvasRect = QRect(0, 0, 400, 300);
        displays.appendDisplay(display, &overlay);
        geometry.rebuild(displays);
        overlay.setCaptureGeometry(display.logicalRect);
        overlay.show();
        QApplication::processEvents();
        require(canvas->setViewportCamera(200, 150, 1), "fixture camera must initialize");
        QImage background(400, 300, QImage::Format_RGBA8888);
        background.fill(QColor(20, 70, 110));
        overlay.setScreenshotImage(background, display.canvasRect);
        selection.setSelectionRect(QRectF(50, 50, 200, 150));
        interaction.applySelectionParams();
        ScreenshotSelectionEditUiActions ui;
        ui.updateOverlayState = [this] { refresh(); };
        workflow = std::make_unique<ScreenshotSelectionEditWorkflow>(
            ScreenshotSelectionEditWorkflowContext{parent, capture, displays, geometry, interaction,
                                                   selection, ui, [this](int radius, int shadow) {
                                                       ++writes;
                                                       settings.setSelectionEffects(radius, shadow);
                                                   }});
        ScreenshotOverlayInputActions actions;
        actions.updateOverlayState = [this] { refresh(); };
        actions.setEffectCursor = ScreenshotOverlayCanvasPresenter::setOverlayEffectCursor;
        actions.previewSelectionEffect = [this](Handle handle, int value) {
            workflow->previewSelectionEffect(handle, value);
        };
        actions.commitSelectionEffects = [this] { workflow->commitSelectionEffects(); };
        actions.effectCanvas = [](const ScreenshotOverlayWindow* owner) {
            return owner != nullptr ? owner->canvas() : nullptr;
        };
        actions.persistSelectionAspectRatioPreference =
            [this](ScreenshotSelectionAspectRatioPreset preset, bool locked) {
                ++aspectRatioWrites;
                settings.setAspectRatioPreference(preset, locked);
            };
        actions.cancelCaptureViaShortcut = [this] {
            ++captureCancellations;
            return true;
        };
        input =
            std::make_unique<ScreenshotOverlayInputHandler>(ScreenshotOverlayInputHandlerContext{
                capture, interaction, selection, intelligent, geometry, displays, actions});
        adapter.setEventTargets(*input, [] {});
        refresh();
    }

    ~Fixture() {
        input->resetTransientShortcuts();
        adapter.clearEventTargets();
    }

    ScreenshotSelectionVisualState state() const {
        ScreenshotSelectionVisualState result;
        result.present = selection.hasPixelSelection();
        result.bounds = selection.normalizedSelection();
        result.cornerRadius = selection.cornerRadius();
        result.shadowWidth = selection.shadowWidth();
        result.effectEditorsVisible = interaction.movingSelection() &&
                                      interaction.moveToolActive() && selection.rectangular() &&
                                      !interaction.effectEditorsSuppressed();
        result.hoveredEffectHandle = interaction.hoveredEffectHandle();
        if (interaction.effectGesture())
            result.activeEffectHandle = interaction.effectGesture()->handle;
        result.effectPreviewVisible = result.hoveredEffectHandle == Handle::Shadow ||
                                      result.activeEffectHandle == Handle::Shadow;
        return result;
    }

    void refresh() {
        ++updates;
        toolbarRadius = selection.cornerRadius();
        toolbarShadow = selection.shadowWidth();
        overlay.setScreenshotSelectionState(state());
    }

    ScreenshotSelectionEffectLayout layout() const {
        return screenshotSelectionEffectLayout(selection.normalizedSelection(),
                                               selection.cornerRadius(),
                                               canvas->canvasToViewTransform(), canvas->rect());
    }

    void mouse(QEvent::Type type, QPointF position, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, canvas->mapToGlobal(position.toPoint()), button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    }
};

void radiusHandlesShareValuesAndPreserveBounds() {
    Fixture f;
    const auto bounds = f.selection.normalizedSelection();
    require(f.interaction.hoveredEffectHandle() == Handle::None, "radius editors start hidden");
    for (const auto handle :
         {Handle::TopLeft, Handle::TopRight, Handle::BottomRight, Handle::BottomLeft}) {
        static_cast<void>(f.selection.setCornerRadius(0));
        f.refresh();
        const auto layout = f.layout();
        const auto point = layout.position(handle);
        f.input->handleMouseMove(&f.overlay, point);
        require(f.interaction.hoveredEffectHandle() == handle, "nearest corner must reveal alone");
        const int writes = f.writes;
        // A press away from the center must not snap the current value to its position.
        const QPointF press = point + QPointF(2, 1);
        f.input->handleMousePress(&f.overlay, press);
        f.input->handleMouseMove(&f.overlay, press);
        require(f.selection.cornerRadius() == 0 && f.input->effectDragActive(),
                "radius press must preserve value and own the gesture");
        const auto inward = screenshotSelectionRadiusInwardDirection(handle);
        const auto end = press + inward * 8;
        f.input->handleMouseMove(&f.overlay, end);
        require(f.selection.cornerRadius() == qRound(8 * layout.radiusPerCanvasUnit) &&
                    f.toolbarRadius == f.selection.cornerRadius() && f.writes == writes,
                "all corners must live-preview the shared radius without persistence");
        require(f.selection.normalizedSelection() == bounds && f.interaction.movingSelection(),
                "effect drags must retain confirmed selection geometry");
        require(!f.input->activateMoveEntireSelectionShortcut() &&
                    !f.input->activateKeepSelectionAspectRatioShortcut(false) &&
                    !f.input->activateSelectionAspectRatioSnapShortcut(),
                "geometry modifiers must not take over effect drags");
        f.input->handleMouseRelease(&f.overlay, end);
        require(!f.interaction.dragging() && f.writes == writes + 1 &&
                    f.settings.cornerRadius() == f.selection.cornerRadius(),
                "release must persist exactly once and end pointer ownership");
    }
    f.input->handleMouseMove(&f.overlay, QPointF(100, 100));
    require(f.interaction.hoveredEffectHandle() == Handle::None,
            "leaving corners must hide editors");
}

void hoverHysteresisLimitsAndTinySelections() {
    Fixture f;
    auto point = f.layout().corners[0];
    f.input->handleMouseMove(&f.overlay, point + QPointF(17, 0));
    require(f.interaction.hoveredEffectHandle() == Handle::TopLeft,
            "18-pixel proximity reveals editor");
    f.input->handleMouseMove(&f.overlay, point + QPointF(23, 0));
    require(f.interaction.hoveredEffectHandle() == Handle::TopLeft,
            "24-pixel hysteresis retains editor");
    f.input->handleMouseMove(&f.overlay, point + QPointF(25, 0));
    require(f.interaction.hoveredEffectHandle() == Handle::None, "leaving hysteresis hides editor");
    f.input->handleMousePress(&f.overlay, point);
    f.input->handleMouseMove(&f.overlay, point + QPointF(1000, 1000));
    require(f.selection.cornerRadius() == 75, "radius must stop at half the shorter side");
    f.input->handleMouseMove(&f.overlay, point - QPointF(1000, 1000));
    require(f.selection.cornerRadius() == 0 &&
                f.interaction.hoveredEffectHandle() == Handle::TopLeft,
            "radius must clamp to zero and remain visible outside its hit area");
    f.input->handleMouseRelease(&f.overlay, point);
    require(f.writes == 0, "an unchanged completed drag must not persist");
    f.selection.setSelectionRect(QRectF(50, 50, 32, 32));
    require(!f.layout().available, "tiny selections must suppress effect controls");
    f.interaction.setCanvasTool(ScreenshotActiveTool::Shape);
    f.input->handleMouseMove(&f.overlay, point);
    require(f.interaction.hoveredEffectHandle() == Handle::None,
            "other tools must suppress effect hover");
    f.interaction.enterScrollingCapture();
    f.input->handleMouseMove(&f.overlay, point);
    require(f.interaction.hoveredEffectHandle() == Handle::None,
            "scrolling capture must suppress effect hover");
    f.interaction.enterOverlayVisible(true);
    require(!f.input->effectDragActive() && f.interaction.hoveredEffectHandle() == Handle::None,
            "selection creation must reset effect editor state");
    const auto maximum = screenshotSelectionEffectLayout(QRectF(0, 0, 800, 600), 500, QTransform(),
                                                         QRectF(0, 0, 1000, 800));
    require(maximum.maximumRadius == 256, "large selections must retain the existing radius limit");
}

void shadowDraggingCancellationAndPointerOwnership() {
    Fixture f;
    const QPointF point = f.layout().shadow;
    require(point == QPointF(274, 125), "shadow editor stays 24 pixels beside the right midpoint");
    f.mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    require(f.input->effectDragActive(), "native overlay dispatch must begin shadow editing");
    f.mouse(QEvent::MouseMove, point + QPointF(20, 45), Qt::NoButton, Qt::LeftButton);
    require(f.selection.shadowWidth() == 20 && f.toolbarShadow == 20 && f.writes == 0 &&
                f.layout().shadow == point && f.state().effectPreviewVisible,
            "shadow uses horizontal canvas delta with live preview and a fixed handle");
    f.mouse(QEvent::MouseButtonRelease, point + QPointF(20, 45), Qt::LeftButton, Qt::NoButton);
    require(!f.interaction.dragging() && f.writes == 1 && f.settings.shadowWidth() == 20,
            "native release outside the handle must commit and release the grab");

    f.input->handleMousePress(&f.overlay, point);
    f.input->handleMouseMove(&f.overlay, point + QPointF(1000, 0));
    require(f.selection.shadowWidth() == 64, "shadow clamps to its existing maximum");
    require(f.input->handleRightClick(&f.overlay, point) ==
                    ScreenshotOverlayRightClickResult::Handled &&
                f.selection.shadowWidth() == 20 && !f.interaction.dragging() && f.writes == 1,
            "right click cancels the edit without changing the saved effect");
    f.input->handleMouseRelease(&f.overlay, point);

    f.input->handleMousePress(&f.overlay, point);
    f.input->handleMouseMove(&f.overlay, point - QPointF(1000, 0));
    require(f.selection.shadowWidth() == 0, "shadow clamps to zero");
    f.interaction.setCanvasTool(ScreenshotActiveTool::Text);
    require(f.selection.shadowWidth() == 20 && !f.input->effectDragActive(),
            "tool changes must roll back the gesture and release its pointer");

    f.interaction.setMoveTool(true, false);
    f.input->handleMousePress(&f.overlay, point);
    f.input->handleMouseMove(&f.overlay, point + QPointF(5, 0));
    QEvent ungrab(QEvent::UngrabMouse);
    QApplication::sendEvent(f.canvas, &ungrab);
    require(f.selection.shadowWidth() == 20 && !f.interaction.dragging(),
            "lost pointer ownership must cancel the edit");

    f.input->handleMousePress(&f.overlay, point);
    f.input->handleMouseMove(&f.overlay, point + QPointF(5, 0));
    f.interaction.setEffectEditorsSuppressed(true);
    require(f.selection.shadowWidth() == 20 && !f.interaction.dragging(),
            "toolbar preview must cancel and suppress effect controls");
    f.interaction.setEffectEditorsSuppressed(false);
    f.input->handleMouseRelease(&f.overlay, point);
    f.mouse(QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
    require(f.input->effectDragActive(),
            "double-clicking an editor must retain its input ownership");
    f.input->handleMouseRelease(&f.overlay, point);
    f.input->handleMousePress(&f.overlay, point);
    f.input->handleMouseMove(&f.overlay, point + QPointF(5, 0));
    f.overlay.hide();
    require(!f.interaction.dragging() && f.selection.shadowWidth() == 20,
            "hiding the overlay must roll back and release the effect gesture");
}

void geometryAndCursorsRespectDisplayScale() {
    for (const qreal scale : {0.5, 1.0, 1.25, 1.5, 2.0}) {
        QTransform transform;
        transform.scale(scale, scale);
        const auto layout = screenshotSelectionEffectLayout(QRectF(0, 0, 200, 150), 0, transform,
                                                            QRectF(0, 0, 500, 500));
        require(layout.available && layout.corners[0] == QPointF(12, 12) &&
                    layout.shadow.x() == 200 * scale + 24,
                "handle size and spacing must stay in viewport pixels across display scales");
        const auto cursor = snowCanvasCornerRadiusCursor(scale);
        require(cursor.shape() == Qt::BitmapCursor && cursor.hotSpot() == QPoint(3, 3) &&
                    qFuzzyCompare(cursor.pixmap().devicePixelRatio(), scale),
                "shared corner cursor must preserve hotspot and device scale");
        const auto shadow = adqt::icons::makeCursor(
            snow_shot::presentation::icons::custom::cursor::SelectionShadowCursor(), QSize(32, 32),
            QPoint(3, 3), scale);
        require(shadow.shape() == Qt::BitmapCursor && shadow.hotSpot() == QPoint(3, 3) &&
                    shadow.pixmap().size() == QSize(qCeil(32 * scale), qCeil(32 * scale)) &&
                    qFuzzyCompare(shadow.pixmap().devicePixelRatio(), scale),
                "shadow cursor must rasterize at the target device scale");
        if (scale >= 1) {
            const auto pixels = shadow.pixmap().toImage();
            require(pixels.pixelColor(qRound(6 * scale), qRound(10 * scale)).value() < 32 &&
                        pixels.pixelColor(qRound(4 * scale), qRound(10 * scale)).value() > 200 &&
                        pixels.pixelColor(qRound(22 * scale), qRound(20 * scale)).alpha() > 200,
                    "shadow cursor must retain its black pointer, white outline, and opaque badge");
        }
        const QString previewPath = qEnvironmentVariable("SNOW_TEST_EFFECT_EDITOR_PREVIEW");
        if (scale == 2 && !previewPath.isEmpty())
            require(shadow.pixmap().save(previewPath + QStringLiteral(".cursor.png")),
                    "cursor visual preview must save");
    }
    const auto edge = screenshotSelectionEffectLayout(QRectF(200, 50, 200, 150), 0, QTransform(),
                                                      QRectF(0, 0, 400, 300));
    require(edge.shadow == QPointF(176, 125) && edge.shadowAnchor == QPointF(200, 125),
            "screen-edge handle must move outside the left border when it fits");
    for (const qreal scale : {0.5, 1.0, 1.25, 1.5, 2.0}) {
        QTransform transform;
        transform.translate(100, 20);
        transform.scale(scale, scale);
        const QRectF viewport(100, 20, 400 * scale, 300 * scale);
        const auto right =
            screenshotSelectionEffectLayout(QRectF(200, 50, 200, 150), 0, transform, viewport);
        require(right.shadow == QPointF(100 + 200 * scale - 24, 20 + 125 * scale) &&
                    right.shadowAnchor == QPointF(100 + 200 * scale, 20 + 125 * scale),
                "left placement must keep logical spacing and attach to the left midpoint");
    }
    const auto noSpace = screenshotSelectionEffectLayout(QRectF(0, 50, 400, 150), 0, QTransform(),
                                                         QRectF(0, 0, 400, 300));
    require(noSpace.shadow == QPointF(376, 125) && noSpace.shadowAnchor == QPointF(400, 125),
            "handle must remain inside the right border when neither outside edge fits");
    const auto leftFits = screenshotSelectionEffectLayout(QRectF(36, 50, 364, 150), 0, QTransform(),
                                                          QRectF(0, 0, 400, 300));
    require(leftFits.shadow == QPointF(12, 125) && leftFits.shadowAnchor == QPointF(36, 125),
            "left placement must allow exactly enough room for the complete hit area");
    const auto leftClipped = screenshotSelectionEffectLayout(QRectF(35, 50, 365, 150), 0,
                                                             QTransform(), QRectF(0, 0, 400, 300));
    require(leftClipped.shadow == QPointF(376, 125),
            "left placement must not clip the hit area at the viewport edge");
    const auto rightFits = screenshotSelectionEffectLayout(QRectF(50, 50, 314, 150), 0,
                                                           QTransform(), QRectF(0, 0, 400, 300));
    require(rightFits.shadow == QPointF(388, 125),
            "right placement remains preferred when its complete hit area fits");
}

void leftShadowHandleSupportsHoverAndDragging() {
    Fixture f;
    f.selection.setSelectionRect(QRectF(200, 50, 200, 150));
    f.refresh();
    const auto bounds = f.selection.normalizedSelection();
    const QPointF point = f.layout().shadow;
    require(point.x() < bounds.left(), "edge selection must expose the left shadow control");
    f.mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
    require(f.interaction.hoveredEffectHandle() == Handle::Shadow && f.state().effectPreviewVisible,
            "left shadow control must support hover and preview");
    f.mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    require(f.input->effectDragActive(), "left shadow control must own the pointer gesture");
    f.mouse(QEvent::MouseMove, point + QPointF(-20, 45), Qt::NoButton, Qt::LeftButton);
    require(f.selection.shadowWidth() == 20 && f.toolbarShadow == 20 && f.writes == 0 &&
                f.selection.normalizedSelection() == bounds && f.layout().shadow == point,
            "leftward dragging of the left control must increase shadow width without resizing");
    f.mouse(QEvent::MouseMove, point + QPointF(-8, -45), Qt::NoButton, Qt::LeftButton);
    require(f.selection.shadowWidth() == 8,
            "moving the left control rightward must reduce the preview from the original width");
    f.mouse(QEvent::MouseMove, point + QPointF(-1000, 0), Qt::NoButton, Qt::LeftButton);
    require(f.selection.shadowWidth() == 64, "leftward shadow dragging must clamp to its maximum");
    f.mouse(QEvent::MouseMove, point + QPointF(1000, 0), Qt::NoButton, Qt::LeftButton);
    require(f.selection.shadowWidth() == 0, "rightward shadow dragging must clamp to zero");
    f.mouse(QEvent::MouseMove, point + QPointF(0, 45), Qt::NoButton, Qt::LeftButton);
    require(f.selection.shadowWidth() == 0, "vertical dragging must leave shadow width unchanged");
    f.mouse(QEvent::MouseButtonRelease, point + QPointF(-20, 45), Qt::LeftButton, Qt::NoButton);
    require(!f.input->effectDragActive() && f.writes == 1 && f.settings.shadowWidth() == 20 &&
                f.interaction.hoveredEffectHandle() == Handle::None,
            "release away from the left control must commit and return it to idle");

    f.mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    f.mouse(QEvent::MouseMove, point + QPointF(-12, 0), Qt::NoButton, Qt::LeftButton);
    require(f.selection.shadowWidth() == 32,
            "a later left drag must start from the committed width");
    require(f.input->cancelEffectDrag() && f.selection.shadowWidth() == 20 && f.writes == 1 &&
                f.settings.shadowWidth() == 20 && f.selection.normalizedSelection() == bounds,
            "cancelling a left shadow drag must restore the original width without persistence");
    f.mouse(QEvent::MouseButtonRelease, point + QPointF(-12, 0), Qt::LeftButton, Qt::NoButton);
}

void insetRightShadowHandleKeepsRightwardDragging() {
    Fixture f;
    f.selection.setSelectionRect(QRectF(0, 50, 400, 150));
    f.refresh();
    const auto bounds = f.selection.normalizedSelection();
    const auto layout = f.layout();
    require(layout.shadow == QPointF(376, 125) && layout.shadowAnchor == QPointF(400, 125),
            "full-width selection must place the control inside its right border");
    f.mouse(QEvent::MouseButtonPress, layout.shadow, Qt::LeftButton, Qt::LeftButton);
    require(f.input->effectDragActive(), "inset right control must own the pointer gesture");
    f.mouse(QEvent::MouseMove, layout.shadow + QPointF(10, 45), Qt::NoButton, Qt::LeftButton);
    require(f.selection.shadowWidth() == 10 && f.selection.normalizedSelection() == bounds,
            "rightward dragging must increase shadow width even when the right control is inset");
    f.mouse(QEvent::MouseButtonRelease, layout.shadow + QPointF(10, 45), Qt::LeftButton,
            Qt::NoButton);
    require(!f.input->effectDragActive() && f.writes == 1 && f.settings.shadowWidth() == 10,
            "inset right shadow drag must commit its preview");
}

void enlargedShadowHandleMatchesItsHitArea() {
    for (const QRectF& bounds :
         {QRectF(50, 50, 200, 150), QRectF(200, 50, 200, 150), QRectF(0, 50, 400, 150)}) {
        Fixture f;
        f.selection.setSelectionRect(bounds);
        f.refresh();
        const auto point = f.layout().shadow;
        f.mouse(QEvent::MouseMove, point + QPointF(10, 0), Qt::NoButton, Qt::NoButton);
        require(f.interaction.hoveredEffectHandle() == Handle::Shadow,
                "the enlarged shadow hit area must reveal its hover state");
        f.mouse(QEvent::MouseMove, point + QPointF(13, 0), Qt::NoButton, Qt::NoButton);
        require(f.interaction.hoveredEffectHandle() == Handle::None,
                "leaving the enlarged shadow hit area must clear hover");
        const auto corner = point + QPointF(7, 7);
        f.mouse(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
        require(f.input->effectDragActive(),
                "the newly enlarged button corners must begin shadow editing");
        f.mouse(QEvent::MouseButtonRelease, corner, Qt::LeftButton, Qt::NoButton);
        require(!f.input->effectDragActive() && f.writes == 0 &&
                    f.selection.normalizedSelection() == bounds,
                "clicking the enlarged shadow button must preserve selection geometry");
    }
}

void escapeCancelsBeforeCaptureAndResizeRemainsSeparate() {
    Fixture f;
    snow_shot::presentation::WindowShortcutManager manager;
    manager.addScopeWindow(&f.overlay);
    ScreenshotOverlayInputActions actions;
    actions.cancelCaptureViaShortcut = [&] {
        ++f.captureCancellations;
        return true;
    };
    ScreenshotOverlayShortcutController shortcuts(manager, *f.input, f.interaction, f.intelligent,
                                                  actions);
    const auto point = f.layout().shadow;
    f.input->handleMousePress(&f.overlay, point);
    f.input->handleMouseMove(&f.overlay, point + QPointF(10, 0));
    const auto escape = [&] {
        for (const auto type : {QEvent::ShortcutOverride, QEvent::KeyPress, QEvent::KeyRelease}) {
            PhysicalKeyEvent event(type, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&f.overlay, &event);
        }
    };
    escape();
    require(!f.input->effectDragActive() && f.selection.shadowWidth() == 0 &&
                f.captureCancellations == 0,
            "Escape must restore the effect before it invokes capture cancellation");
    f.input->handleMouseRelease(&f.overlay, point);
    escape();
    require(f.captureCancellations == 1,
            "a later Escape must retain capture cancellation behavior");
    const QPointF border(250, 125);
    f.input->handleMousePress(&f.overlay, border);
    require(!f.input->effectDragActive() &&
                f.interaction.dragMode() == ScreenshotSelectionDragMode::Right,
            "the border midpoint must retain selection resizing");
    f.input->handleMouseRelease(&f.overlay, border + QPointF(10, 0));
    require(f.selection.normalizedSelection().width() == 210,
            "border resizing must remain independent of the shadow badge");
}

void aspectRatioSnappingAndEffectEditingPreserveEachOther() {
    Fixture f;
    const QPointF border(250, 125);
    f.input->handleMousePress(&f.overlay, border);
    require(f.interaction.dragMode() == ScreenshotSelectionDragMode::Right &&
                f.input->activateSelectionAspectRatioSnapShortcut(),
            "Ctrl snapping must remain available during border resizing");
    const QPointF end = border + QPointF(25, 0);
    f.input->handleMouseMove(&f.overlay, end);
    require(f.input->releaseSelectionAspectRatioSnapShortcut(),
            "Ctrl release must end the temporary snap modifier");
    f.input->handleMouseRelease(&f.overlay, end);
    const auto bounds = f.selection.normalizedSelection();
    const auto preset = ScreenshotSelectionAspectRatioPreset::Landscape3x2;
    require(bounds == QRectF(50, 50, 225, 150) && f.selection.aspectRatioPreset() == preset &&
                f.selection.aspectRatioLocked() && f.settings.aspectRatioPreset() == preset &&
                f.settings.aspectRatioLocked() && f.aspectRatioWrites > 0 && f.writes == 0,
            "snapping must retain its anchor and persist the ratio independently of effects");

    const int ratioWrites = f.aspectRatioWrites;
    require(f.input->activateSelectionAspectRatioSnapShortcut(),
            "Ctrl snapping may be armed before an effect drag");
    const auto shadow = f.layout().shadow;
    f.input->handleMousePress(&f.overlay, shadow);
    f.input->handleMouseMove(&f.overlay, shadow + QPointF(20, 0));
    require(f.input->effectDragActive() && f.selection.shadowWidth() == 20 &&
                f.selection.normalizedSelection() == bounds &&
                f.selection.aspectRatioPreset() == preset && f.aspectRatioWrites == ratioWrites &&
                f.writes == 0,
            "an armed snap modifier must leave effect previews and selection geometry independent");
    require(!f.input->activateSelectionAspectRatioSnapShortcut() &&
                f.selection.normalizedSelection() == bounds &&
                f.input->releaseSelectionAspectRatioSnapShortcut(),
            "Ctrl during an effect drag must preserve the gesture and release an armed modifier");
    f.input->handleMouseRelease(&f.overlay, shadow + QPointF(20, 0));
    require(!f.interaction.dragging() && f.writes == 1 && f.settings.shadowWidth() == 20 &&
                f.selection.normalizedSelection() == bounds &&
                f.settings.aspectRatioPreset() == preset && f.settings.aspectRatioLocked() &&
                f.aspectRatioWrites == ratioWrites,
            "effect commit must preserve the snapped ratio and persist only its own settings");
}

QImage render(SnowCanvasWidget& canvas, qreal dpr) {
    QImage output(QSize(qCeil(canvas.width() * dpr), qCeil(canvas.height() * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(dpr);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    canvas.render(&painter);
    return output;
}

void shadowControlOpacityTracksHoverAndDragging() {
    for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
        for (const QRectF& bounds :
             {QRectF(50, 50, 200, 150), QRectF(200, 50, 200, 150), QRectF(0, 50, 400, 150)}) {
            SnowCanvasWidget canvas;
            canvas.resize(400, 300);
            canvas.setViewportCamera(200, 150, 1);
            ScreenshotCanvasRenderer renderer(canvas);
            canvas.setCustomRenderer(&renderer);
            QImage background(400, 300, QImage::Format_RGBA8888);
            background.fill(QColor(20, 70, 110));
            renderer.setImage(background, QRectF(0, 0, 400, 300));
            renderer.setMaskVisible(true);
            ScreenshotSelectionVisualState state;
            state.present = true;
            state.bounds = bounds;
            const auto capture = [&] {
                renderer.applySelectionState(state);
                return render(canvas, dpr);
            };
            const auto withoutControl = capture();
            state.effectEditorsVisible = true;
            const auto idle = capture();
            state.hoveredEffectHandle = Handle::Shadow;
            const auto hovered = capture();
            state.hoveredEffectHandle = Handle::None;
            state.activeEffectHandle = Handle::Shadow;
            const auto active = capture();
            require(active == hovered, "shadow drag must remain opaque after the pointer leaves");
            state.activeEffectHandle = Handle::None;
            require(capture() == idle, "ending shadow interaction must restore idle opacity");
            const auto layout = screenshotSelectionEffectLayout(
                bounds, 0, canvas.canvasToViewTransform(), canvas.rect());
            const QPoint enlargedEdge(qFloor((layout.shadow.x() + 8) * dpr),
                                      qFloor((layout.shadow.y() + 2) * dpr));
            require(idle.pixelColor(enlargedEdge) != withoutControl.pixelColor(enlargedEdge) &&
                        idle.pixelColor(enlargedEdge) != hovered.pixelColor(enlargedEdge),
                    "the enlarged shadow button edge must remain visible and translucent");
            for (const QPointF& point :
                 {layout.shadow + QPointF(6, 3), (layout.shadowAnchor + layout.shadow) / 2}) {
                const QPoint pixel(qFloor(point.x() * dpr), qFloor(point.y() * dpr));
                const auto base = withoutControl.pixelColor(pixel);
                const auto faded = idle.pixelColor(pixel);
                const auto full = hovered.pixelColor(pixel);
                require(faded != base && faded != full,
                        "idle shadow badge and connector must remain visible and translucent");
                require(std::abs(2 * faded.red() - base.red() - full.red()) <= 3 &&
                            std::abs(2 * faded.green() - base.green() - full.green()) <= 3 &&
                            std::abs(2 * faded.blue() - base.blue() - full.blue()) <= 3,
                        "idle shadow control must blend with the background at half opacity");
            }
            state.hoveredEffectHandle = Handle::TopLeft;
            const auto radiusWithIdleShadow = capture();
            state.activeEffectHandle = Handle::Shadow;
            const auto radiusWithActiveShadow = capture();
            const auto radiusArea = QRectF(layout.corners[0] - QPointF(5, 5), QSizeF(10, 10));
            const QRect pixels(qFloor(radiusArea.x() * dpr), qFloor(radiusArea.y() * dpr),
                               qCeil(radiusArea.width() * dpr), qCeil(radiusArea.height() * dpr));
            require(radiusWithIdleShadow.copy(pixels) == radiusWithActiveShadow.copy(pixels),
                    "shadow opacity must not affect the corner radius control");
        }
    }
}

void editorDamageCoversEveryChangedPixel() {
    for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
        SnowCanvasWidget canvas;
        canvas.resize(400, 300);
        canvas.setViewportCamera(200, 150, 1);
        ScreenshotCanvasRenderer renderer(canvas);
        canvas.setCustomRenderer(&renderer);
        QImage background(400, 300, QImage::Format_RGBA8888);
        background.fill(QColor(20, 70, 110));
        renderer.setImage(background, QRectF(0, 0, 400, 300));
        renderer.setMaskVisible(true);
        ScreenshotSelectionVisualState state;
        state.bounds = QRectF(50, 50, 200, 150);
        state.present = true;
        renderer.applySelectionState(state);
        auto before = render(canvas, dpr);
        const auto transition = [&](ScreenshotSelectionVisualState next) {
            renderer.applySelectionState(next);
            const auto after = render(canvas, dpr);
            const auto damage = planScreenshotSelectionDamage(state, next, canvas.rect(),
                                                              canvas.canvasToViewTransform(), true);
            for (int y = 0; y < after.height(); ++y) {
                for (int x = 0; x < after.width(); ++x) {
                    if (before.pixel(x, y) != after.pixel(x, y))
                        require(
                            damage.contains(QPoint(qFloor(x / dpr), qFloor(y / dpr))),
                            "incremental effect damage must cover every changed rendered pixel");
                }
            }
            if (state.bounds == next.bounds &&
                state.effectPreviewVisible == next.effectPreviewVisible &&
                state.cornerRadius == next.cornerRadius)
                require(!damage.contains(QPoint(150, 125)),
                        "hover-only updates must preserve stable interior");
            state = next;
            before = after;
        };
        auto next = state;
        next.effectEditorsVisible = true;
        transition(next);
        next.hoveredEffectHandle = Handle::Shadow;
        transition(next);
        next.hoveredEffectHandle = Handle::None;
        next.activeEffectHandle = Handle::Shadow;
        transition(next);
        next.activeEffectHandle = Handle::None;
        next.hoveredEffectHandle = Handle::TopLeft;
        transition(next);
        next.hoveredEffectHandle = Handle::BottomRight;
        transition(next);
        next.activeEffectHandle = Handle::BottomRight;
        next.cornerRadius = 24;
        transition(next);
        next.hoveredEffectHandle = next.activeEffectHandle = Handle::Shadow;
        next.effectPreviewVisible = true;
        next.shadowWidth = 10;
        transition(next);
        const QString previewPath = qEnvironmentVariable("SNOW_TEST_EFFECT_EDITOR_PREVIEW");
        if (dpr == 2 && !previewPath.isEmpty()) {
            next.hoveredEffectHandle = Handle::TopLeft;
            renderer.applySelectionState(next);
            require(render(canvas, dpr).save(previewPath), "visual preview must save");
            renderer.applySelectionState(state);
        }
        next.shadowWidth = 64;
        transition(next);
        next.shadowWidth = 0;
        transition(next);
        next.effectPreviewVisible = false;
        next.hoveredEffectHandle = next.activeEffectHandle = Handle::None;
        transition(next);
        for (const QRectF& bounds :
             {QRectF(200, 50, 200, 150), QRectF(0, 50, 400, 150), QRectF(50, 50, 200, 150)}) {
            next.bounds = bounds;
            transition(next);
            next.hoveredEffectHandle = Handle::Shadow;
            transition(next);
            next.hoveredEffectHandle = Handle::None;
            next.activeEffectHandle = Handle::Shadow;
            transition(next);
            next.activeEffectHandle = Handle::None;
            transition(next);
        }
        next.toolbarHovered = true;
        transition(next);
        next.toolbarHovered = false;
        next.effectEditorsVisible = false;
        transition(next);
    }
}
} // namespace

void runScreenshotSelectionEffectEditorTests() {
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage
                .initialize({directory.filePath(QStringLiteral("bin")),
                             directory.filePath(QStringLiteral("data")), 60000})
                .success,
            "effect tests require isolated storage");
    radiusHandlesShareValuesAndPreserveBounds();
    hoverHysteresisLimitsAndTinySelections();
    shadowDraggingCancellationAndPointerOwnership();
    geometryAndCursorsRespectDisplayScale();
    leftShadowHandleSupportsHoverAndDragging();
    insetRightShadowHandleKeepsRightwardDragging();
    enlargedShadowHandleMatchesItsHitArea();
    escapeCancelsBeforeCaptureAndResizeRemainsSeparate();
    aspectRatioSnappingAndEffectEditingPreserveEachOther();
    shadowControlOpacityTracksHoverAndDragging();
    editorDamageCoversEveryChangedPixel();
    storage.shutdown();
}
