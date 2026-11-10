#include "screenshot_selection_presentation_fixture.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"
#include "snow_shot/presentation/screenshotpresentationframescheduler.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"

#include <QPainter>
#include <QClipboard>
#include <QMouseEvent>
#include <QLabel>
#include <QEventLoop>
#include <QPointer>
#include <QTimer>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using selection_presentation_test::Fixture;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class PresentationTimerObserver final : public QObject {
  public:
    PresentationTimerObserver() {
        QCoreApplication::instance()->installEventFilter(this);
    }
    ~PresentationTimerObserver() override {
        QCoreApplication::instance()->removeEventFilter(this);
    }

    bool waitForNextTimeout() {
        const int previousEvents = timerEvents;
        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
        waitingLoop = &loop;
        watchdog.start(5000);
        loop.exec();
        waitingLoop = nullptr;
        return timerEvents > previousEvents;
    }

    QPointer<QObject> timer;

  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event != nullptr &&
            (event->type() == QEvent::Timer || event->type() == QEvent::WinEventAct)) {
            auto* observed = object;
            if (observed != nullptr &&
                (observed->objectName() == QStringLiteral("screenshotPresentationFrameTimer") ||
                 observed->objectName() == QStringLiteral("screenshotPresentationFrameNotifier"))) {
                require(timer.isNull() || timer == observed,
                        "one presentation fixture must retain the same frame timer object");
                timer = observed;
                ++timerEvents;
                if (waitingLoop != nullptr)
                    waitingLoop->quit();
            }
        }
        return false;
    }

  private:
    int timerEvents = 0;
    QEventLoop* waitingLoop = nullptr;
};

QImage decorationImage(Fixture& fixture) {
    QImage image(fixture.overlay.canvas()->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    fixture.overlay.screenshotRendererForTesting()->renderAfterCanvas(
        painter,
        {image.rect(), image.rect(), fixture.overlay.canvas()->canvasToViewTransform(), 1.0});
    painter.end();
    return image;
}

void enableRedPointerGuides(Fixture& fixture) {
    fixture.preferences.cursorGuideLineColor = Qt::red;
    fixture.preferences.monitorCenterGuideLineColor = Qt::transparent;
    fixture.preferences.selectionCenterGuideLineColor = Qt::transparent;
    fixture.services->setUiPreferences(fixture.preferences);
    fixture.enableGuides();
}

void regionSnapGuidesReachPresentationFrames() {
    Fixture fixture(QSize(800, 600), false);
    auto& model = fixture.selection;
    model.setSelectionRect(QRect(100, 100, 40, 40));
    model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
    model.setSelectionRect(QRect(300, 100, 40, 40));
    model.commitRegionOperation();
    fixture.interaction.confirmSelection();
    require(fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::All),
            "start the region presentation gesture");
    model.beginMoveDrag({310, 110});
    fixture.services->updateOverlayState();
    const auto before = decorationImage(fixture);
    const auto geometry = model.selectionRegion();
    const auto candidate =
        model.snapIndividualRegionMove(QRectF(300, 100, 40, 40), QRectF(0, 0, 800, 600), 1, true);
    model.setDraggedSelectionRect(candidate, ScreenshotSelectionDragMode::All);
    require(model.selectionRegion() == geometry && !model.moveSnapGuides().isEmpty(),
            "aligned region produces guides without moving any selected pixels");
    fixture.services->requestSelectionDragPresentation();
    fixture.advanceClock(17);
    fixture.flushFrame();
    require(decorationImage(fixture) != before,
            "frame presentation must forward guide-only model changes to the renderer");
    model.clearMoveSnapGuides();
    fixture.services->requestSelectionDragPresentation();
    fixture.advanceClock(17);
    fixture.flushFrame();
    require(decorationImage(fixture) == before,
            "clearing guides must restore the original presentation on the next frame");
}

void independentRegionOutlineHasLocalDamage() {
    Fixture fixture(QSize(800, 600), false);
    auto& model = fixture.selection;
    model.setSelectionRect(QRect(100, 100, 400, 300));
    model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
    const QRect inside(200, 200, 40, 30);
    model.setSelectionRect(inside);
    model.commitRegionOperation();
    fixture.interaction.confirmSelection();
    fixture.services->updateOverlayState();
    auto* renderer = fixture.overlay.screenshotRendererForTesting();
    require(!renderer->selectionHandlesVisible(),
            "a rectangular combined mask must not expose destructive group resize handles");
    const auto before = decorationImage(fixture);
    const auto revision = model.changeRevision();
    require(model.setHoveredRegion({210, 210}),
            "contained region hover changes its outline target");
    fixture.services->updateOverlayState();
    const auto after = decorationImage(fixture);
    require(before != after && model.changeRevision() == revision,
            "hover reveals internal boundaries without changing selection semantics");
    ScreenshotSelectionVisualState previous;
    previous.present = true;
    previous.bounds = model.normalizedSelection();
    previous.handlesVisible = false;
    auto next = previous;
    next.highlightedRegion = model.highlightedRegion();
    const auto damage = planScreenshotSelectionDamage(
        previous, next, before.rect(), fixture.overlay.canvas()->canvasToViewTransform(), true);
    require(!damage.isEmpty() && damage.boundingRect().width() < 100 &&
                damage.boundingRect().height() < 100,
            "hover outlines must invalidate only the contained region's neighborhood");
    for (int y = 0; y < before.height(); ++y)
        for (int x = 0; x < before.width(); ++x)
            require(before.pixel(x, y) == after.pixel(x, y) || damage.contains({x, y}),
                    "outline damage must cover every changed rendered pixel");
    static_cast<void>(model.setHoveredRegion({0, 0}));
    fixture.services->updateOverlayState();
    require(decorationImage(fixture) == before,
            "leaving a region removes its hover outline cleanly");
}

void independentDragDamageCoversMasksAndContours() {
    for (const bool curves : {false, true}) {
        ScreenshotSelectionModel model;
        model.setSelectionRect(QRect(80, 60, 180, 160));
        model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
        model.setSelectionRect(QRect(450, 300, 50, 50));
        model.commitRegionOperation();
        for (int index = 0; index < 16; ++index) {
            model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
            model.setSelectionRect(QRect(420 + (index % 4) * 20, 220 + (index / 4) * 20, 10, 10));
            model.commitRegionOperation();
        }
        model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
        QPainterPath ellipse;
        ellipse.addEllipse(QRectF(50, 50, 80, 60));
        model.setDraftRegion(
            curves ? ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve)
                   : ScreenshotRegionGeometry(QRect(50, 50, 80, 60)));
        model.commitDraftRegion(QRect(0, 0, 600, 400));
        model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Subtract);
        model.setSelectionRect(QRect(85, 70, 10, 10));
        model.commitRegionOperation();
        model.beginMoveDrag({70, 80});
        const auto state = [&] {
            ScreenshotSelectionVisualState value;
            value.present = true;
            value.handlesVisible = false;
            value.cornerRadius = curves ? 0 : 12;
            value.bounds = model.normalizedSelection();
            value.region = model.selectionRegion();
            value.confirmedRegion = model.confirmedRegion();
            value.highlightedRegion = model.highlightedRegion();
            value.stationaryRegions = model.stationaryRegionsDuringMove();
            return value;
        };
        const auto previous = state();
        model.setDraggedSelectionRect(model.selectionRectForDrag(ScreenshotSelectionDragMode::All,
                                                                 {145, 110}, QRect(0, 0, 600, 400),
                                                                 1),
                                      ScreenshotSelectionDragMode::All);
        const auto next = state();
        for (const qreal scale : {1.0, 1.5, 2.0}) {
            const QRect viewport(0, 0, qRound(600 * scale), qRound(400 * scale));
            const auto transform = QTransform::fromScale(scale, scale);
            SnowCanvasWidget canvas;
            ScreenshotCanvasRenderer renderer(canvas);
            const auto render = [&](const ScreenshotSelectionVisualState& visual) {
                renderer.applySelectionState(visual);
                QImage image(viewport.size(), QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                renderer.renderAfterCanvas(painter, {viewport, viewport, transform, 1.0});
                return image;
            };
            const auto before = render(previous);
            const auto after = render(next);
            const auto damage =
                planScreenshotSelectionDamage(previous, next, viewport, transform, true);
            require(!damage.isEmpty() &&
                        !damage.contains(transform.map(QPointF(475, 325)).toPoint()),
                    "individual drag damage must leave distant stationary regions alone");
            require(before != after, "drag fixture must change the mask and outline pixels");
            for (int y = 0; y < before.height(); ++y)
                for (int x = 0; x < before.width(); ++x)
                    require(before.pixel(x, y) == after.pixel(x, y) || damage.contains({x, y}),
                            "local drag damage must cover changed overlaps, holes, and outlines");
            QImage incremental = before;
            {
                // A separate UI exposure must not make the contours between the
                // two dirty areas part of the moving region's repaint work.
                const QRegion exposure =
                    damage + QRect(viewport.right() - 1, viewport.bottom() - 1, 1, 1);
                QPainter painter(&incremental);
                painter.setClipRegion(exposure);
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.fillRect(viewport, Qt::transparent);
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                renderer.renderAfterCanvas(painter, {viewport, exposure, transform, 1.0});
            }
            require(incremental == after,
                    "culled drag contours must repaint exactly like a complete frame");
        }
    }
}

void repeatedSelectionLeavesThePresentedUiIdle() {
    Fixture fixture(QSize(1200, 800), false);
    const QRectF initial = fixture.displayedSelection();
    require(initial == fixture.selection.normalizedSelection(),
            "the fixture must present the real selection model");
    for (int request = 0; request < 16; ++request)
        fixture.requestSelection(initial);
    fixture.advanceClock(17);
    fixture.flushFrame();
    fixture.processEvents();

    require(fixture.stateNotifications == 0,
            "repeated selections must not republish semantic state");
    require(fixture.hintTranslations.requests == 0,
            "repeated selections must not regenerate shortcut content");
    require(fixture.paintObserver.canvasPaints == 0 && fixture.paintObserver.uiPaints == 0,
            "repeated selections must leave the canvas and auxiliary widgets unpainted");
    require(fixture.displayedSelection() == initial,
            "repeated selections must preserve the displayed rectangle");
}

void multipleRequestsCommitOnlyTheirLatestSelection() {
    Fixture fixture(QSize(1200, 800), false);
    const QRectF initial = fixture.displayedSelection();
    QRectF latest;
    for (int request = 1; request <= 16; ++request) {
        latest = initial.translated(request * 4.0, request * 2.0);
        fixture.requestSelection(latest);
    }
    require(fixture.displayedSelection() == initial,
            "new target requests must wait for the shared presentation frame");
    require(fixture.stateNotifications == 0,
            "pending target requests must not synchronously publish semantic state");
    fixture.advanceClock(17);
    fixture.flushFrame();
    fixture.processEvents();

    require(fixture.displayedSelection() == latest,
            "a presentation frame must commit the newest requested selection");
    require(fixture.stateNotifications == 1,
            "coalesced requests must publish semantic state once per committed frame");
    require(fixture.hintTranslations.requests == 0,
            "selection movement must reuse the existing shortcut content");
    require(fixture.paintObserver.canvasPaints > 0,
            "the newest selection must reach the real canvas paint path");
}

void theFirstResultAfterAnEmptySelectionIsImmediate() {
    Fixture fixture;
    const QRectF first = fixture.baseSelection();
    fixture.requestSelection({});
    fixture.flushFrame();
    fixture.processEvents();
    require(!fixture.overlay.hasScreenshotSelection(),
            "an empty target must clear the previous presentation");
    fixture.resetCounters();
    fixture.requestSelection(first);
    require(fixture.displayedSelection() == first,
            "the first valid smart result must be presented before another scheduled frame");
    require(fixture.stateNotifications == 1,
            "an immediate first result must publish its committed semantic state");
}

void selectionDragBurstsPrepareAndCommitTheLatestModelAtOneFrame() {
    for (const auto dragMode :
         {ScreenshotSelectionDragMode::All, ScreenshotSelectionDragMode::BottomRight}) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.interaction.confirmSelection();
        require(fixture.interaction.enterSelectionDrag(dragMode),
                "manual presentation requires a valid move or resize gesture");
        fixture.services->updateOverlayState();
        fixture.resetCounters();
        const QRectF initial = fixture.displayedSelection();
        QRectF latest;
        for (int request = 1; request <= 16; ++request) {
            latest = dragMode == ScreenshotSelectionDragMode::All
                         ? initial.translated(request * 4.0, request * 2.0)
                         : initial.adjusted(0, 0, request * 4.0, request * 2.0);
            fixture.selection.setDraggedSelectionRect(latest, dragMode);
            fixture.services->requestSelectionDragPresentation();
            require(fixture.selection.normalizedSelection() == latest,
                    "frame pacing must preserve each immediate model mutation");
        }
        require(fixture.stateNotifications == 0 && fixture.displayedSelection() == initial,
                "continuous drag bursts must defer presentation until their shared frame");
        fixture.advanceClock(17);
        fixture.flushFrame();
        fixture.processEvents();
        require(fixture.stateNotifications == 1 && fixture.displayedSelection() == latest &&
                    fixture.paintObserver.canvasPaints == 1,
                "one display frame must commit and paint only the latest drag geometry");
        // Drain child-widget layout/expose work before measuring an idle frame, as setup does.
        fixture.processEvents();
        fixture.resetCounters();
        for (int request = 0; request < 16; ++request)
            fixture.services->requestSelectionDragPresentation();
        fixture.advanceClock(17);
        fixture.flushFrame();
        fixture.processEvents();
        require(fixture.stateNotifications == 0 && fixture.paintObserver.canvasPaints == 0 &&
                    selectionRenderDiagnosticsForCurrentThread().requestedDamagePixels == 0,
                "unchanged drag requests must leave the committed presentation idle");
    }
}

void selectionDragBoundariesAndCommandsCommitSynchronously() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.interaction.confirmSelection();
    require(fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::All),
            "selection dragging must start from a confirmed selection");
    const auto captureMode = fixture.interaction.mode();
    fixture.services->updateOverlayState();
    fixture.resetCounters();
    const QRectF initial = fixture.displayedSelection();
    const QRectF moved = initial.translated(20, 15);
    fixture.selection.setDraggedSelectionRect(moved, ScreenshotSelectionDragMode::All);
    fixture.services->requestSelectionDragPresentation();
    fixture.interaction.finishDrag();
    fixture.services->updateOverlayState();
    require(fixture.interaction.mode() == captureMode && fixture.displayedSelection() == moved &&
                fixture.stateNotifications == 1,
            "releasing a same-mode drag must synchronously commit its latest pending geometry");
    fixture.resetCounters();
    require(fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::BottomRight),
            "confirmed selections must allow another resize gesture");
    fixture.services->updateOverlayState();
    require(fixture.stateNotifications == 1,
            "a same-mode gesture start must synchronously publish its drag boundary");
    fixture.resetCounters();
    const QRectF resized = moved.adjusted(0, 0, 40, 25);
    fixture.selection.setDraggedSelectionRect(resized, ScreenshotSelectionDragMode::BottomRight);
    fixture.services->requestSelectionDragPresentation();
    require(fixture.selection.setCornerRadius(12),
            "the explicit style command must change the selection radius");
    fixture.services->updateOverlayState();
    require(fixture.displayedSelection() == resized && fixture.stateNotifications == 1,
            "an explicit style command must commit pending drag geometry synchronously");
    fixture.resetCounters();
    require(fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::All),
            "a resize gesture must allow its transient move shortcut");
    fixture.services->requestSelectionDragPresentation();
    require(fixture.stateNotifications == 1,
            "changing drag mode must commit the new interaction boundary synchronously");
    fixture.advanceClock(17);
    fixture.flushFrame();
    require(fixture.stateNotifications == 1,
            "synchronous drag boundaries must cancel superseded pending commits");
}

void theFirstUsableMarqueeIsPresentedImmediately() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.interaction.returnToSelectionMode(false);
    fixture.selection.clearSelection();
    require(fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::Marquee),
            "manual marquee presentation requires an active gesture");
    fixture.services->updateOverlayState();
    fixture.resetCounters();
    const QRectF first = fixture.baseSelection();
    fixture.selection.setDraggedSelectionRect(first, ScreenshotSelectionDragMode::Marquee);
    fixture.services->requestSelectionDragPresentation();
    require(fixture.displayedSelection() == first && fixture.stateNotifications == 1,
            "the first usable marquee must reveal before the next display deadline");
    fixture.resetCounters();
    fixture.selection.setDraggedSelectionRect(first.adjusted(0, 0, 30, 20),
                                              ScreenshotSelectionDragMode::Marquee);
    fixture.services->requestSelectionDragPresentation();
    require(fixture.displayedSelection() == first && fixture.stateNotifications == 0,
            "later marquee geometry must share the display frame boundary");
}

void canceledRequestsLeaveTheCommittedFrameUntouched() {
    Fixture fixture(QSize(1200, 800), false);
    const QRectF initial = fixture.displayedSelection();
    fixture.requestSelection(initial.translated(100.0, 80.0));
    fixture.requestSelection(initial);
    fixture.advanceClock(17);
    fixture.flushFrame();
    fixture.processEvents();
    require(fixture.displayedSelection() == initial,
            "returning to the committed target must preserve its presentation");
    require(fixture.stateNotifications == 0,
            "requests canceled before a frame must not notify semantic consumers");
    require(fixture.hintTranslations.requests == 0,
            "requests canceled before a frame must not regenerate auxiliary content");
    require(fixture.paintObserver.canvasPaints == 0 && fixture.paintObserver.uiPaints == 0,
            "requests canceled before a frame must not repaint unchanged UI");
}

void captureLifecycleChangesNotifyEvenWhenGeometryIsUnchanged() {
    Fixture fixture(QSize(1200, 800), false);
    const QRectF initial = fixture.displayedSelection();
    require(fixture.services->colorPickerContext().active,
            "a visible capture fixture must initially permit color sampling");
    fixture.captureState.captureInProgress = true;
    fixture.services->updateOverlayState();
    require(fixture.stateNotifications == 1,
            "capture startup must immediately publish a lifecycle change");
    require(!fixture.services->colorPickerContext().active,
            "capture startup must disable color sampling even with unchanged geometry");
    fixture.services->updateOverlayState();
    require(fixture.stateNotifications == 1,
            "unchanged capture flags must not republish the same lifecycle state");

    fixture.captureState.sessionState = ScreenshotSessionState::Capturing;
    fixture.services->updateOverlayState();
    require(fixture.stateNotifications == 2,
            "a capture session-state boundary must immediately publish its lifecycle change");
    fixture.captureState.captureInProgress = false;
    fixture.captureState.sessionState = ScreenshotSessionState::OverlayVisible;
    fixture.services->updateOverlayState();
    require(fixture.stateNotifications == 3,
            "capture completion must immediately publish the restored lifecycle state");
    require(fixture.services->colorPickerContext().active,
            "capture completion must restore color sampling availability");
    require(fixture.displayedSelection() == initial,
            "capture lifecycle boundaries must preserve the selected capture geometry");
}

void displayRebindingReappliesSelectionAfterRendererReset() {
    Fixture fixture(QSize(1200, 800), false);
    const QRectF initial = fixture.displayedSelection();
    fixture.overlay.resetScreenshotRendering();
    fixture.coordinator.applyDisplayModels(fixture.displays);
    require(!fixture.overlay.hasScreenshotSelection(),
            "a newly rebound renderer must initially lack the previous selection state");
    fixture.resetCounters();
    fixture.services->updateOverlayState();
    require(fixture.displayedSelection() == initial,
            "display rebinding must immediately reapply unchanged selection geometry");
    require(fixture.overlay.screenshotRendererForTesting()->maskVisible(),
            "display rebinding must restore the screenshot selection mask");
    require(fixture.stateNotifications == 1,
            "display rebinding must notify semantic consumers of the new presentation surface");
    fixture.processEvents();
    require(fixture.paintObserver.canvasPaints > 0,
            "reapplied selection state must reach the newly bound canvas paint path");
}

void endingSelectionMovementRefreshesTheDeferredToolbarContent() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.preferences.selectionDisplayUnit = ScreenshotSelectionDisplayUnit::PhysicalPixels;
    fixture.services->setUiPreferences(fixture.preferences);
    fixture.flushFrame();
    QLabel* widthLabel = nullptr;
    for (QLabel* label : fixture.coordinator.selectionToolbar()->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("Width")) {
            widthLabel = label;
            break;
        }
    }
    require(widthLabel != nullptr, "the real selection toolbar must expose its width readout");
    const QString originalWidth = widthLabel->text();
    QRectF changed = fixture.displayedSelection();
    changed.setWidth(changed.width() + 80.0);

    fixture.services->setSelectionMovementActive(true);
    fixture.flushFrame();
    fixture.requestSelection(changed);
    fixture.flushFrame();
    require(fixture.displayedSelection() == changed,
            "active movement must continue updating the displayed selection");
    require(widthLabel->text() == originalWidth,
            "active movement must defer the selection toolbar's content update");
    fixture.resetCounters();
    fixture.services->setSelectionMovementActive(false);
    fixture.flushFrame();
    require(widthLabel->text() == QString::number(fixture.selection.pixelSelection().width()),
            "ending movement must refresh deferred width content with unchanged model geometry");
    require(fixture.stateNotifications == 0,
            "refreshing deferred toolbar content must not republish unchanged semantic state");
}

void hiddenSelectionToolbarDefersPresentationUntilReveal() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.preferences.selectionDisplayUnit = ScreenshotSelectionDisplayUnit::PhysicalPixels;
    fixture.services->setUiPreferences(fixture.preferences);
    fixture.flushFrame();
    auto* toolbar = fixture.coordinator.selectionToolbar();
    fixture.coordinator.setSelectionToolbarHidden(true);
    fixture.processEvents();
    QLabel* width = nullptr;
    for (QLabel* label : toolbar->findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("Width")) {
            width = label;
            break;
        }
    }
    require(width != nullptr, "selection toolbar must expose its width readout");
    const QString hiddenWidth = width->text();
    const QRect hiddenGeometry = toolbar->geometry();
    auto* hiddenOwner = toolbar->parentWidget();
    QRectF latest;
    for (int frame = 0; frame < 20; ++frame) {
        latest = QRectF(120 + frame * 4, 100 + frame * 3, 320 + frame * 10, 180);
        fixture.requestSelection(latest);
        fixture.advanceClock(17);
        fixture.flushFrame();
    }
    fixture.processEvents();
    require(fixture.displayedSelection() == latest,
            "hiding the selection toolbar must preserve selection presentation");
    require(!toolbar->isVisible() && width->text() == hiddenWidth &&
                toolbar->geometry() == hiddenGeometry && toolbar->parentWidget() == hiddenOwner,
            "hidden selection frames must not update toolbar content, layout, position or owner");
    fixture.coordinator.setSelectionToolbarHidden(false);
    fixture.services->showSelectionToolbar();
    require(toolbar->isVisible() &&
                width->text() == QString::number(fixture.selection.pixelSelection().width()) &&
                toolbar->geometry() != hiddenGeometry,
            "revealing the selection toolbar must synchronize current content and placement");
}

void animationFramesOnlyChangeDisplayedGeometry() {
    Fixture fixture;
    const QRectF initial = fixture.displayedSelection();
    const QRectF target = initial.translated(160.0, 120.0);
    fixture.requestSelection(target);
    fixture.flushFrame();
    fixture.processEvents();
    require(fixture.displayedSelection() == initial,
            "an animated target must begin from the last displayed frame");
    fixture.resetCounters();

    fixture.advanceClock(8);
    fixture.flushFrame();
    fixture.processEvents();
    const QRectF firstFrame = fixture.displayedSelection();
    require(firstFrame != initial && firstFrame != target,
            "an eight-millisecond presentation frame must interpolate the selection");
    const qreal remainingProgress = 1.0 - 8.0 / 101.0;
    const qreal remainingAfterEightMs = remainingProgress * remainingProgress;
    require(std::abs(firstFrame.x() - (target.x() + (initial.x() - target.x()) *
                                                        remainingAfterEightMs)) < 0.000001 &&
                std::abs(firstFrame.y() - (target.y() + (initial.y() - target.y()) *
                                                            remainingAfterEightMs)) < 0.000001,
            "Soft presentation frames must preserve the 101 ms quadratic curve on the scheduler");
    require(fixture.selection.normalizedSelection() == target,
            "display interpolation must preserve the capture target");
    fixture.advanceClock(8);
    fixture.flushFrame();
    fixture.processEvents();
    require(fixture.displayedSelection() != firstFrame,
            "high-refresh frames must advance independently of a second animation timer");
    fixture.advanceClock(400);
    fixture.flushFrame();
    fixture.processEvents();

    require(fixture.displayedSelection() == target,
            "animation must finish at the exact capture target after a delayed frame");
    require(fixture.stateNotifications == 0,
            "animation frames must not notify semantic state consumers");
    require(fixture.hintTranslations.requests == 0,
            "animation frames must not translate and format unchanged shortcut rows");
    require(!fixture.services->frameSchedulerForTesting().active(),
            "the presentation scheduler must become idle once the transition finishes");
}

void animationPreferencesSelectDurationsAndDisableRunningFrames() {
    using Animation = snow_shot::SelectionTransitionAnimation;
    for (const auto animation : {Animation::Fast, Animation::Soft}) {
        Fixture fixture;
        fixture.preferences.selectionTransitionAnimation = animation;
        fixture.services->setUiPreferences(fixture.preferences);
        const QRectF initial = fixture.displayedSelection();
        const QRectF target(initial.x() + 160.0, initial.y() + 120.0, initial.width() + 80.0,
                            initial.height() + 60.0);
        fixture.requestSelection(target);
        fixture.flushFrame();
        fixture.advanceClock(16);
        fixture.flushFrame();
        const int durationMs = animation == Animation::Fast ? 67 : 101;
        const qreal progress = 16.0 / durationMs;
        const qreal eased = 1.0 - (1.0 - progress) * (1.0 - progress);
        const QRectF expected(initial.x() + (target.x() - initial.x()) * eased,
                              initial.y() + (target.y() - initial.y()) * eased,
                              initial.width() + (target.width() - initial.width()) * eased,
                              initial.height() + (target.height() - initial.height()) * eased);
        const QRectF displayed = fixture.displayedSelection();
        require(std::abs(displayed.x() - expected.x()) < 0.000001 &&
                    std::abs(displayed.y() - expected.y()) < 0.000001 &&
                    std::abs(displayed.width() - expected.width()) < 0.000001 &&
                    std::abs(displayed.height() - expected.height()) < 0.000001 &&
                    fixture.selection.normalizedSelection() == target,
                "selected animation speeds must reach the shared scheduler without changing "
                "capture geometry");

        fixture.preferences.selectionTransitionAnimation = Animation::Disabled;
        fixture.services->setUiPreferences(fixture.preferences);
        fixture.flushFrame();
        require(fixture.displayedSelection() == target &&
                    !fixture.services->frameSchedulerForTesting().active(),
                "Off must immediately finish running animation and leave the scheduler idle");
        const QRectF direct = target.translated(40.0, 30.0);
        fixture.requestSelection(direct);
        fixture.flushFrame();
        require(fixture.displayedSelection() == direct,
                "Off must present subsequent smart selections directly");
    }
    ScreenshotUiPreferences invalid;
    invalid.selectionTransitionAnimation = static_cast<Animation>(42);
    require(invalid.normalized().selectionTransitionAnimation == Animation::Soft,
            "invalid programmatic animation preferences must normalize to Soft");
}

void modeAndSessionChangesCancelOldAnimation() {
    Fixture fixture;
    const QRectF initial = fixture.displayedSelection();
    const QRectF oldTarget = initial.translated(120.0, 100.0);
    fixture.requestSelection(oldTarget);
    fixture.flushFrame();
    fixture.advanceClock(16);
    fixture.flushFrame();

    const QRectF manual = initial.translated(40.0, 30.0);
    fixture.interaction.enterOverlayVisible(false);
    fixture.requestSelection(manual);
    fixture.flushFrame();
    fixture.advanceClock(200);
    fixture.flushFrame();
    require(fixture.displayedSelection() == manual,
            "leaving smart selection must cancel the old target and present manual geometry");

    fixture.interaction.enterOverlayVisible(true);
    const QRectF newFirst = initial.translated(60.0, 10.0);
    fixture.requestSelection(newFirst);
    fixture.flushFrame();
    require(fixture.displayedSelection() == newFirst,
            "the first smart result after reentry must be direct");
    fixture.requestSelection(oldTarget);
    fixture.flushFrame();
    fixture.advanceClock(16);
    fixture.flushFrame();
    ++fixture.captureState.sessionId;
    fixture.displays.startup->sessionId = fixture.captureState.sessionId;
    const QRectF nextSessionFirst = initial.translated(10.0, 160.0);
    fixture.requestSelection(nextSessionFirst);
    fixture.flushFrame();
    require(fixture.displayedSelection() == nextSessionFirst,
            "a new capture session must discard the previous transition history");

    fixture.interaction.reset();
    fixture.selection.clearSelection();
    fixture.captureState.sessionState = ScreenshotSessionState::IdlePrepared;
    fixture.services->updateOverlayState();
    fixture.flushFrame();
    fixture.advanceClock(200);
    fixture.flushFrame();
    require(!fixture.overlay.screenshotRendererForTesting()->hasSelection(),
            "capture exit must not replay a stale selection frame");
}

void anEpochChangeDiscardsPendingWorkWithoutAnotherStateUpdate() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.requestSelection(fixture.baseSelection().translated(140.0, 100.0));
    require(fixture.stateNotifications == 0,
            "the old session's changed target must still be pending");

    // Session teardown clears visible UI independently of semantic presentation requests.
    ++fixture.captureState.sessionId;
    fixture.captureState.sessionState = ScreenshotSessionState::IdlePrepared;
    fixture.overlay.clearScreenshotSelection();
    fixture.coordinator.hideToolbar();
    fixture.coordinator.hideSelectionToolbar();
    fixture.resetCounters();
    fixture.advanceClock(200);
    fixture.flushFrame();
    fixture.processEvents();

    require(fixture.stateNotifications == 0,
            "a queued frame from an ended epoch must not notify semantic consumers");
    require(!fixture.overlay.hasScreenshotSelection(),
            "a queued frame from an ended epoch must not resurrect the old selection");
    require(!fixture.coordinator.selectionToolbar()->isVisible(),
            "a queued frame from an ended epoch must not show the old selection toolbar");
    fixture.advanceClock(200);
    fixture.flushFrame();
    require(fixture.stateNotifications == 0 && !fixture.overlay.hasScreenshotSelection(),
            "discarded epoch work must remain canceled on subsequent frame attempts");
}

void anEpochChangeReleasesShapedSelectionSnapshots() {
    Fixture fixture(QSize(320, 240), false);
    fixture.interaction.enterOverlayVisible(false);
    std::weak_ptr<const void> storage;
    {
        QPainterPath path;
        path.addEllipse(QRectF(40, 30, 160, 120));
        const auto region = ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve);
        storage = region.storageLifetimeForTesting();
        fixture.selection.setSelectionRegion(region);
        fixture.services->updateOverlayState();
    }
    require(!storage.expired(), "the active shaped selection must retain its source geometry");

    ++fixture.captureState.sessionId;
    fixture.captureState.sessionState = ScreenshotSessionState::IdlePrepared;
    fixture.interaction.reset();
    fixture.selection.reset();
    fixture.overlay.resetScreenshotRendering();
    fixture.resetCounters();
    fixture.flushFrame();
    require(storage.expired(),
            "an ended epoch must release presentation snapshots after model and renderer cleanup");
    fixture.processEvents();
    require(fixture.stateNotifications == 0 && !fixture.overlay.hasScreenshotSelection(),
            "retiring selection snapshots must not publish or replay the ended capture");
}

void resetReleasesPendingAndCommittedShapedSelections() {
    for (const bool committed : {false, true}) {
        Fixture fixture(QSize(320, 240), false);
        std::weak_ptr<const void> storage;
        std::optional<ScreenshotRegionGeometry> exported;
        {
            QPainterPath path;
            path.addEllipse(QRectF(40, 30, 160, 120));
            const auto region =
                ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve);
            storage = region.storageLifetimeForTesting();
            fixture.selection.setSelectionRegion(region);
            fixture.services->updateOverlayState();
            require(fixture.stateNotifications == 0,
                    "a shaped smart target must still be pending before its presentation frame");
            if (committed) {
                fixture.flushFrame();
                exported = fixture.selection.selectionRegion();
            }
        }
        require(!storage.expired(), "pending and committed selections must own their geometry");
        ++fixture.captureState.sessionId;
        fixture.captureState.sessionState = ScreenshotSessionState::IdlePrepared;
        fixture.interaction.reset();
        fixture.selection.reset();
        fixture.overlay.resetScreenshotRendering();
        fixture.resetCounters();
        fixture.services->resetPresentation();
        if (exported) {
            require(exported->contains(QPointF(120, 90)) && !exported->contains(QPointF(20, 20)),
                    "resetting presentation must preserve an independently owned export snapshot");
            exported.reset();
        }
        require(storage.expired(),
                "explicit teardown must immediately release pending and committed region storage");
        fixture.services->resetPresentation();
        fixture.processEvents();
        fixture.advanceClock(200);
        fixture.flushFrame();
        require(fixture.stateNotifications == 0 && !fixture.overlay.hasScreenshotSelection(),
                "repeated teardown and queued frames must leave the ending capture retired");
    }
}

void resetRetainsTheIdleSchedulerAndAllowsTheNextCapture() {
    Fixture fixture(QSize(320, 240), false);
    PresentationTimerObserver observer;
    const auto& scheduler = fixture.services->frameSchedulerForTesting();
    fixture.requestPointer(QPointF(100, 150));
    fixture.advanceClock(17);
    require(observer.waitForNextTimeout() && observer.timer && !scheduler.active(),
            "the capture's frame scheduler must be idle after delivering its frame");
    const QPointer<QObject> retainedBackend = observer.timer;

    fixture.services->resetPresentation();
    require(retainedBackend && scheduler.wakeupObject() == retainedBackend && !scheduler.active(),
            "teardown must retain the stopped scheduler backend for reuse");
    fixture.services->updatePointerPresentation(&fixture.overlay, QPointF(80, 60));
    fixture.services->requestColorPickerPresentation(&fixture.overlay, QPointF(80, 60));
    require(!scheduler.active(),
            "pointer and picker input before a new presentation must not restart the scheduler");

    ++fixture.captureState.sessionId;
    fixture.displays.startup->sessionId = fixture.captureState.sessionId;
    const QRectF nextSelection = fixture.baseSelection().translated(10, 15);
    fixture.resetCounters();
    fixture.requestSelection(nextSelection);
    require(fixture.displayedSelection() == nextSelection && fixture.stateNotifications == 1,
            "the next capture must synchronously present and notify its first selection");
    fixture.requestPointer(QPointF(90, 70));
    fixture.advanceClock(17);
    require(observer.waitForNextTimeout() && observer.timer == retainedBackend,
            "the next capture must reuse the original scheduler backend");
    require(fixture.displayedSelection() == nextSelection && fixture.stateNotifications == 1,
            "new pointer frames must preserve the new capture's selection and semantic state");
    fixture.requestPointer(QPointF(100, 80));
    require(scheduler.active(), "new pointer work must arm a presentation deadline");
    fixture.services->resetPresentation();
    require(!scheduler.active(), "teardown must also cancel an active presentation deadline");
}

QString sampledColor(Fixture& fixture, const QPointF& localPosition) {
    const auto& display = fixture.displays.displayAt(0);
    const QPointF canvasPosition = fixture.geometry.canvasPositionForOverlayLocalPoint(
        fixture.displays, &fixture.overlay, localPosition);
    const QPoint physicalPosition =
        fixture.geometry.physicalPositionForCanvasPoint(fixture.displays, canvasPosition);
    return display.image
        .pixelColor(physicalPosition.x() - display.physicalRect.x(),
                    physicalPosition.y() - display.physicalRect.y())
        .name(QColor::HexRgb)
        .toUpper();
}

void resetDiscardsQueuedPickerInputUntilTheNextCapture() {
    Fixture fixture(QSize(320, 240), false);
    fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysHide);
    auto* picker = fixture.coordinator.colorPicker();
    const auto& scheduler = fixture.services->frameSchedulerForTesting();
    const QPointF latest(180, 120);
    fixture.requestMagnifier(latest);
    require(scheduler.active() && picker->workCounters().samples == 0,
            "picker input must queue a sample at the next presentation deadline");

    // Keep the model active and the session unchanged so cancellation must come from teardown.
    fixture.services->resetPresentation();
    fixture.services->flushColorPickerPresentation();
    fixture.requestMagnifier(QPointF(200, 140));
    fixture.services->flushColorPickerPresentation();
    fixture.advanceClock(200);
    fixture.flushFrame();
    fixture.processEvents();
    require(!scheduler.active() && picker->workCounters().samples == 0 &&
                fixture.stateNotifications == 0,
            "teardown must discard queued picker input and reject input before reinitialization");

    ++fixture.captureState.sessionId;
    fixture.displays.startup->sessionId = fixture.captureState.sessionId;
    fixture.services->updateOverlayState();
    fixture.resetCounters();
    fixture.requestMagnifier(latest);
    fixture.advanceClock(17);
    fixture.flushFrame();
    require(picker->workCounters().samples == 1 &&
                picker->currentColorText() == sampledColor(fixture, latest) && !scheduler.active(),
            "the next capture must resume picker sampling without replaying the retired input");
}

void semanticNotificationCanResetPresentationAndStartTheNextCapture() {
    Fixture fixture(QSize(320, 240), false);
    const auto& scheduler = fixture.services->frameSchedulerForTesting();
    fixture.onStateChanged = [&] { fixture.services->resetPresentation(); };
    fixture.requestSelection(fixture.baseSelection().translated(10, 15));
    fixture.advanceClock(17);
    fixture.flushFrame();
    require(fixture.stateNotifications == 1 && !scheduler.active(),
            "semantic teardown must cancel pending work without reentering the active frame");
    fixture.onStateChanged = {};
    fixture.advanceClock(200);
    fixture.flushFrame();
    fixture.processEvents();
    require(fixture.stateNotifications == 1 && !scheduler.active(),
            "a frame retired by its semantic callback must not publish another notification");

    ++fixture.captureState.sessionId;
    fixture.displays.startup->sessionId = fixture.captureState.sessionId;
    const QRectF nextSelection = fixture.baseSelection().translated(20, 25);
    fixture.resetCounters();
    fixture.requestSelection(nextSelection);
    require(fixture.displayedSelection() == nextSelection && fixture.stateNotifications == 1,
            "the next capture must present synchronously after reentrant semantic teardown");
    fixture.requestPointer(QPointF(90, 70));
    fixture.advanceClock(17);
    fixture.flushFrame();
    require(!scheduler.active() && fixture.stateNotifications == 1,
            "the next capture must resume pointer scheduling without old semantic work");
}

void theFrameSchedulerRetainsItsBackendWithoutIdleWakeupsAndStopsAtEpochExit() {
    Fixture fixture(QSize(1200, 800), false);
    PresentationTimerObserver observer;
    fixture.requestPointer(QPointF(100, 150));
    fixture.advanceClock(17);
    const auto& scheduler = fixture.services->frameSchedulerForTesting();
    require(observer.waitForNextTimeout(),
            "the scheduled presentation timer must deliver a frame before the watchdog");
    require(observer.timer && !scheduler.active(),
            "a delivered frame must leave no pending idle deadline");
    QObject* backend = scheduler.wakeupObject();
    require(backend == observer.timer, "the scheduler must expose its retained native backend");

    for (int request = 1; request <= 3; ++request) {
        fixture.requestPointer(QPointF(100 + request * 10, 150));
        fixture.advanceClock(8);
        fixture.flushFrame();
        require(!scheduler.active() && scheduler.wakeupObject() == backend,
                "subsequent frame commits must reuse the backend without scheduling idle work");
    }
    require(fixture.stateNotifications == 0,
            "reusing the pointer presentation timer must not publish semantic changes");

    fixture.advanceClock(50);
    fixture.processEvents();
    require(!scheduler.active() && scheduler.wakeupObject() == backend,
            "idle presentation must retain its backend without arming an idle deadline");

    // This request queues an immediately eligible frame after the idle period.
    // Ending the epoch before event dispatch must cancel both timer and queued work.
    fixture.requestPointer(QPointF(250, 200));
    require(scheduler.active(), "new pointer work must reactivate the same timer object");
    ++fixture.captureState.sessionId;
    fixture.captureState.sessionState = ScreenshotSessionState::IdlePrepared;
    fixture.overlay.clearScreenshotSelection();
    fixture.resetCounters();
    fixture.flushFrame();
    fixture.processEvents();
    require(observer.timer && !scheduler.active(),
            "a capture epoch exit must cancel an active presentation timer");
    require(fixture.stateNotifications == 0 && !fixture.overlay.hasScreenshotSelection(),
            "the canceled timer and queued first frame must not resurrect old epoch state");
}

void synchronousFrameWorkSkipsExpiredDeadlines() {
    Fixture fixture;
    PresentationTimerObserver observer;
    const QRectF initial = fixture.displayedSelection();
    const qreal reportedRate = fixture.overlay.screen()->refreshRate();
    const qreal rate = std::isfinite(reportedRate) && reportedRate > 1.0 ? reportedRate : 60.0;
    const qint64 workMilliseconds = static_cast<qint64>(std::ceil(3000.0 / rate)) + 1;
    fixture.onStateChanged = [&] { fixture.advanceClock(workMilliseconds); };
    fixture.requestSelection(initial.translated(160, 120));
    fixture.advanceClock(17);
    fixture.flushFrame();
    fixture.onStateChanged = {};
    require(fixture.stateNotifications == 1 && fixture.displayedSelection() == initial,
            "synchronous semantic work must leave the animation at its committed start frame");
    require(fixture.services->frameSchedulerForTesting().active(),
            "an unfinished animation must retain a pending future frame");
    require(observer.waitForNextTimeout(),
            "the retained backend must wake after the simulated presentation work");
    require(fixture.displayedSelection() == initial,
            "deadlines elapsed inside synchronous work must be skipped instead of caught up");
    fixture.advanceClock(static_cast<qint64>(std::ceil(1000.0 / rate)) + 1);
    require(observer.waitForNextTimeout() && fixture.displayedSelection() != initial,
            "animation must resume when the next future display deadline becomes eligible");
}

void pointerBurstsDoNotPublishSemanticChanges() {
    Fixture fixture(QSize(1200, 800), false);
    enableRedPointerGuides(fixture);
    const QRectF selection = fixture.displayedSelection();
    for (int request = 0; request < 16; ++request)
        fixture.requestPointer(QPointF(100 + request * 10, 200 + request * 3));
    fixture.advanceClock(17);
    fixture.flushFrame();
    fixture.processEvents();

    require(fixture.stateNotifications == 0,
            "pointer-only frames must not notify semantic state consumers");
    require(fixture.hintTranslations.requests == 0,
            "pointer movement must reuse existing shortcut rows");
    require(fixture.displayedSelection() == selection,
            "pointer-only frames must preserve the selected capture target");
    auto* renderer = fixture.overlay.screenshotRendererForTesting();
    require(renderer->guideLinesVisible(), "the latest pointer frame must present its guide");
    const QImage image = decorationImage(fixture);
    require(image.pixelColor(250, 20) == QColor(Qt::red),
            "coalesced pointer guides must use the latest provided position");
    // The decoration pass includes the translucent mask at this pixel; only the red guide
    // must disappear, rather than all decoration opacity.
    require(image.pixelColor(100, 20) != QColor(Qt::red),
            "coalesced pointer guides must not retain an earlier burst position");
}

void pointerUpdatesWithHiddenGuidesRemainAvailableForLaterPresentation() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.displays.startup->resumeLiveInput();
    fixture.preferences.cursorGuideLineColor = Qt::red;
    fixture.preferences.monitorCenterGuideLineColor = Qt::transparent;
    fixture.preferences.selectionCenterGuideLineColor = Qt::transparent;
    fixture.services->setUiPreferences(fixture.preferences);
    fixture.flushFrame();
    require(!fixture.overlay.screenshotRendererForTesting()->guideLinesVisible(),
            "pointer tracking must begin with cursor guides disabled");

    // Use accepted event coordinates directly: the startup anchor and native cursor
    // must not supply the expected position when the guides are enabled later.
    fixture.services->updatePointerPresentation(&fixture.overlay, QPointF(180.75, 160.25));
    fixture.services->updatePointerPresentation(&fixture.overlay, QPointF(310.75, 240.25));
    fixture.flushFrame();
    fixture.requestSelection(fixture.displayedSelection().translated(5, 0));
    fixture.flushFrame();
    require(!fixture.overlay.screenshotRendererForTesting()->guideLinesVisible(),
            "a semantic update must keep disabled cursor guides hidden");

    fixture.enableGuides();
    const QImage image = decorationImage(fixture);
    require(image.pixelColor(310, 20) == QColor(Qt::red),
            "enabling guides must use the latest pointer accepted while guides were disabled");
    require(image.pixelColor(180, 20) != QColor(Qt::red),
            "enabling guides must not resurrect an earlier hidden pointer position");
    require(image.pixelColor(311, 20) != QColor(Qt::red),
            "enabling guides must retain the accepted pointer's fractional local precision");
}

void qtDeadlineFramesMergeAnimationGuidesAndMagnifierBursts() {
    Fixture fixture(QSize(1200, 800), true, true,
                    ScreenshotPresentationFrameScheduler::Backend::QtTimer);
    enableRedPointerGuides(fixture);
    fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysShow);
    fixture.coordinator.setSelectionToolbarHidden(true);
    fixture.flushFrame();
    fixture.processEvents();
    fixture.resetCounters();
    const QRectF initial = fixture.displayedSelection();
    QRectF target;
    QPointF pointer;
    for (int request = 0; request < 16; ++request) {
        target = initial.translated(20 + request * 3, 15 + request);
        pointer = QPointF(800 + request * 3, 500 + request * 3);
        fixture.requestSelection(target);
        fixture.requestMagnifier(pointer);
    }
    auto* picker = fixture.coordinator.colorPicker();
    require(picker->workCounters().samples == 0 && fixture.stateNotifications == 0,
            "mixed input bursts must defer semantic and magnifier work until their shared frame");
    fixture.advanceClock(17);
    fixture.flushFrame();
    fixture.processEvents();
    require(fixture.stateNotifications == 1 && picker->workCounters().samples == 1 &&
                fixture.paintObserver.canvasPaints == 1,
            "one shared frame must coalesce mixed input to one semantic notification, sample and "
            "paint");
    const QImage image = decorationImage(fixture);
    require(image.pixelColor(static_cast<int>(pointer.x()), 20) == QColor(Qt::red),
            "the combined frame must use the latest pointer for its guide");
    fixture.advanceClock(400);
    fixture.flushFrame();
    fixture.processEvents();
    require(fixture.displayedSelection() == target && fixture.stateNotifications == 1 &&
                picker->workCounters().samples == 1,
            "animation-only frames must reach the latest target without replaying semantic or "
            "picker work");
    require(!fixture.services->frameSchedulerForTesting().active(),
            "the merged Qt deadline scheduler must become idle when animation and input finish");
}

void pointerOnlyHintVisibilityUsesTheLatestPresentedSelection() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.displays.startup->resumeLiveInput();
    const QPoint outside(fixture.overlay.width() - 5, 5);
    fixture.requestPointer(outside);
    const auto canvasRect = [&](const QRectF& localRect) {
        return QRectF(fixture.geometry.canvasPositionForOverlayLocalPoint(
                          fixture.displays, &fixture.overlay, localRect.topLeft()),
                      fixture.geometry.canvasPositionForOverlayLocalPoint(
                          fixture.displays, &fixture.overlay, localRect.bottomRight()));
    };
    const QRectF away = canvasRect(QRectF(fixture.overlay.width() - 20, 0, 10, 10));
    fixture.requestSelection(away);
    fixture.flushFrame();
    fixture.processEvents();
    auto* hints = fixture.overlay.findChild<QWidget*>(QStringLiteral("screenshotShortcutHints"));
    require(hints != nullptr && hints->isVisible(),
            "shortcut hints must be visible away from the current selection and pointer");
    const QPoint hover = hints->geometry().center();

    // Updating the selection must replace the cached global rectangle used by later
    // pointer-only frames, including when the new rectangle obscures the hint area.
    fixture.requestSelection(canvasRect(QRectF(hover - QPoint(5, 5), QSize(10, 10))));
    fixture.flushFrame();
    require(!hints->isVisible(), "a newly overlapping selection must hide the shortcut hints");
    fixture.resetCounters();
    fixture.requestPointer(hover);
    fixture.flushFrame();
    fixture.requestPointer(outside);
    fixture.flushFrame();
    require(!hints->isVisible(),
            "pointer-only frames must retain the latest selection's hint obstruction");
    require(fixture.stateNotifications == 0 && fixture.hintTranslations.requests == 0,
            "pointer-only hint visibility must reuse content without semantic notifications");

    fixture.requestSelection(away);
    fixture.flushFrame();
    require(hints->isVisible(),
            "moving the selection away must refresh the cached hint obstruction");
    fixture.resetCounters();
    fixture.requestPointer(hover);
    fixture.flushFrame();
    require(!hints->isVisible(), "the pointer-only path must hide hints under the cursor");
    fixture.requestPointer(outside);
    fixture.flushFrame();
    require(hints->isVisible(), "the pointer-only path must restore hints after the cursor leaves");
    require(fixture.stateNotifications == 0 && fixture.hintTranslations.requests == 0 &&
                fixture.displayedSelection() == away,
            "pointer-only hint changes must preserve selection and reuse semantic content");
}

void displayMovementRecomputesTheAnchoredPointerLocalPosition() {
    Fixture fixture(QSize(1200, 800), false);
    enableRedPointerGuides(fixture);
    fixture.requestPointer(QPointF(250, 200));
    fixture.flushFrame();
    const QRectF initialSelection = fixture.displayedSelection();
    const QPoint anchoredPosition = fixture.displays.startup->logicalPosition;
    const quint64 layoutGeneration = fixture.displays.startup->layoutGeneration;

    auto& display = fixture.displays.displayAt(0);
    display.logicalRect.translate(100, 50);
    display.physicalRect.translate(qRound(100 * display.logicalToPhysicalScale),
                                   qRound(50 * display.logicalToPhysicalScale));
    fixture.geometry.rebuild(fixture.displays);
    fixture.coordinator.applyDisplayModels(fixture.displays);
    fixture.services->updateOverlayState();
    require(fixture.displays.startup->logicalPosition == anchoredPosition &&
                fixture.displays.startup->layoutGeneration == layoutGeneration,
            "the moved-display fixture must preserve the anchored global cursor and generation");
    require(fixture.displayedSelection() == initialSelection,
            "moving the display must preserve the same capture selection in canvas coordinates");
    const QImage image = decorationImage(fixture);
    require(image.pixelColor(150, 20) == QColor(Qt::red),
            "a moved display must present the anchored global pointer at its new local position");
    require(image.pixelColor(250, 20) != QColor(Qt::red),
            "a moved display must discard cached local coordinates from its previous geometry");
}

void layoutGenerationChangesRefreshTheAnchoredPointer() {
    Fixture fixture(QSize(1200, 800), false);
    enableRedPointerGuides(fixture);
    fixture.requestPointer(QPointF(250, 200));
    fixture.flushFrame();
    const QRect originalGeometry = fixture.overlay.captureGeometry();
    fixture.displays.startup->logicalPosition = QPoint(300, 200);
    ++fixture.displays.startup->layoutGeneration;
    fixture.services->updateOverlayState();
    require(fixture.overlay.captureGeometry() == originalGeometry,
            "the generation fixture must retain the same overlay geometry");
    const QImage image = decorationImage(fixture);
    require(image.pixelColor(300, 20) == QColor(Qt::red),
            "a new layout generation must refresh the anchored pointer position");
    require(image.pixelColor(250, 20) != QColor(Qt::red),
            "a new layout generation must discard pointer data from the previous generation");
}

void imageRebindingKeepsAcceptedPointerPrecisionForUnchangedGeometry() {
    Fixture fixture(QSize(1200, 800), false);
    enableRedPointerGuides(fixture);
    fixture.requestPointer(QPointF(250.75, 200.25));
    fixture.flushFrame();
    const QRect originalGeometry = fixture.overlay.captureGeometry();
    fixture.coordinator.applyDisplayModels(fixture.displays);
    fixture.services->updateOverlayState();
    require(fixture.overlay.captureGeometry() == originalGeometry,
            "image-only rebinding must retain the same overlay geometry");
    const QImage image = decorationImage(fixture);
    require(image.pixelColor(250, 20) == QColor(Qt::red),
            "image-only rebinding must preserve the accepted pointer's local precision");
    require(image.pixelColor(251, 20) != QColor(Qt::red),
            "image-only rebinding must not replace accepted coordinates with rounded global data");
}

void shortcutHintsFollowRegionOperationTransitions() {
    for (const auto operation : {ScreenshotSelectionModel::RegionOperation::Add,
                                 ScreenshotSelectionModel::RegionOperation::Subtract}) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.interaction.returnToSelectionMode(false);
        fixture.selection.setSelectionRect(QRect(700, 100, 200, 150));
        fixture.services->updateOverlayState();
        fixture.flushFrame();
        auto* hints =
            fixture.overlay.findChild<QWidget*>(QStringLiteral("screenshotShortcutHints"));
        require(hints != nullptr &&
                    hints->accessibleName().contains(QStringLiteral("Move entire selection")),
                "the initial single selection must expose its selection controls");

        fixture.selection.beginRegionOperation(operation);
        fixture.services->updateOverlayState();
        fixture.flushFrame();
        const QString regionHints = hints->accessibleName();
        require(!regionHints.contains(QStringLiteral("Move entire selection")) &&
                    !regionHints.contains(QStringLiteral("Selection aspect ratio snap")) &&
                    !regionHints.contains(QStringLiteral("Copy color")) &&
                    !regionHints.contains(QStringLiteral("Switch screenshot history")) &&
                    regionHints.contains(QStringLiteral("Duplicate latest region")),
                "add and subtract operations must refresh the real widget's blocked shortcuts");

        fixture.selection.setDraftRegion(ScreenshotRegionGeometry(QRect(750, 120, 20, 20)));
        fixture.services->updateOverlayState();
        fixture.flushFrame();
        require(!hints->accessibleName().contains(QStringLiteral("Duplicate latest region")),
                "starting a custom-region draft must hide the inactive duplicate command");
        fixture.selection.clearDraftRegion();
        fixture.services->updateOverlayState();
        fixture.flushFrame();
        require(hints->accessibleName() == regionHints,
                "canceling the draft must restore the region-operation hints in the same mode");

        fixture.selection.setSelectionRect(QRect(750, 120, 20, 20));
        fixture.selection.commitRegionOperation();
        fixture.services->updateOverlayState();
        fixture.flushFrame();
        require(
            hints->accessibleName().contains(QStringLiteral("Copy color")) &&
                hints->accessibleName().contains(QStringLiteral("Move entire selection")) &&
                !hints->accessibleName().contains(
                    QStringLiteral("Maintain selection aspect ratio")) &&
                !hints->accessibleName().contains(QStringLiteral("Selection aspect ratio snap")),
            "committing an operation must restore whole-selection movement without resize hints");
        fixture.selection.setSelectionRect(QRect(700, 100, 200, 150));
        fixture.services->updateOverlayState();
        fixture.flushFrame();
        require(hints->accessibleName().contains(QStringLiteral("Move entire selection")) &&
                    !hints->accessibleName().contains(QStringLiteral("Duplicate latest region")),
                "replacing the multi-region capture must restore the single-selection hints");
    }
}

void shortcutContentStillRetranslatesOnLanguageChange() {
    Fixture fixture(QSize(1200, 800), false);
    auto* hints = fixture.overlay.findChild<QWidget*>(QStringLiteral("screenshotShortcutHints"));
    require(hints != nullptr, "the real UI host must own the shortcut hints");
    QEvent event(QEvent::LanguageChange);
    QCoreApplication::sendEvent(hints, &event);
    require(fixture.hintTranslations.requests > 0,
            "cached shortcut content must still retranslate after a language change");
    fixture.hintTranslations.requests = 0;
    fixture.requestSelection(fixture.displayedSelection());
    fixture.flushFrame();
    require(fixture.hintTranslations.requests == 0,
            "later frames must reuse the newly translated shortcut content");
}
ScreenshotOverlayInputActions pickerInputActions(Fixture& fixture) {
    ScreenshotOverlayInputActions actions;
    actions.updateColorPickerForOverlay = [&fixture](ScreenshotOverlayWindow* owner,
                                                     const QPointF& position) {
        fixture.services->requestColorPickerPresentation(owner, position);
    };
    actions.updateColorPickerForSelectionDrag = [&fixture](const QPointF& position) {
        fixture.services->requestSelectionDragColorPickerPresentation(position);
    };
    actions.updateOverlayState = [&fixture] { fixture.services->updateOverlayState(); };
    actions.requestSelectionDragPresentation = [&fixture] {
        fixture.services->requestSelectionDragPresentation();
    };
    actions.showToolbar = [&fixture] { fixture.services->showToolbar(); };
    return actions;
}

void externalSelectionInitializationCommitsTheCompleteHiddenStateOnce() {
    for (const bool batched : {false, true}) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.overlay.hide();
        fixture.coordinator.setSelectionToolbarHidden(true);
        fixture.interaction.returnToSelectionMode(false);
        fixture.selection.clearSelection();
        fixture.services->updateOverlayState();
        fixture.flushFrame();
        fixture.processEvents();
        fixture.resetCounters();

        const QPointF start(100, 100);
        const QPointF latestEnd(160, 140);
        const QRectF complete(100, 100, 61, 41);
        QVector<QRectF> committedSelections;
        QVector<QPointF> pickerPositions;
        int cursorUpdates = 0;
        int toolbarHides = 0;
        fixture.onStateChanged = [&] {
            require(fixture.interaction.marqueeSelecting() && fixture.interaction.dragging(),
                    "initial global selection must commit an unconfirmed marquee gesture");
            committedSelections.append(fixture.displayedSelection());
        };
        auto actions = pickerInputActions(fixture);
        actions.hideMainToolbar = [&] { ++toolbarHides; };
        actions.setOverlayCursor = [&](ScreenshotOverlayWindow*, ScreenshotSelectionDragMode mode) {
            require(mode == ScreenshotSelectionDragMode::Marquee,
                    "initial global selection must retain its marquee cursor");
            ++cursorUpdates;
        };
        actions.updateColorPickerForSelectionDrag = [&](const QPointF& position) {
            pickerPositions.append(position);
        };
        ScreenshotOverlayInputHandler input(
            {fixture.captureState, fixture.interaction, fixture.selection,
             fixture.intelligentSelection, fixture.geometry, fixture.displays, std::move(actions)});
        input.setExternalDragActive(true);
        if (batched) {
            input.beginExternalSelectionDrag(start, latestEnd);
        } else {
            input.beginExternalSelectionDrag(start);
            input.updateExternalSelectionDrag(latestEnd);
        }

        require(fixture.displayedSelection() == complete &&
                    fixture.selection.normalizedSelection() == complete,
                "the hidden initial frame must synchronously contain the latest inclusive pixels");
        require(fixture.selection.moveOriginalSelection() == QRectF(start, QSizeF(0, 0)),
                "batched initialization must preserve the original empty press geometry");
        require(fixture.stateNotifications == (batched ? 1 : 2) &&
                    committedSelections.size() == (batched ? 1 : 2) &&
                    committedSelections.last() == complete,
                "batched initialization must replace two semantic commits with one final commit");
        if (batched) {
            require(committedSelections.first() == complete &&
                        pickerPositions == QVector<QPointF>{latestEnd},
                    "batched initialization must publish and sample only the complete endpoint");
        } else {
            require(committedSelections.first().isEmpty() &&
                        pickerPositions == QVector<QPointF>{start, latestEnd},
                    "the legacy sequence must expose the redundant empty initial commit");
        }
        require(cursorUpdates == 1 && toolbarHides == 1,
                "initializing the complete gesture must retain its cursor and toolbar boundary");
        fixture.advanceClock(17);
        fixture.flushFrame();
        fixture.processEvents();
        require(
            fixture.stateNotifications == (batched ? 1 : 2) &&
                fixture.paintObserver.canvasPaints == 0,
            "the committed hidden state must leave no duplicate presentation at the next frame");
    }
}

void batchedExternalSelectionPreservesPointerGeometryAndEmptyGestures() {
    struct Case {
        QPointF start;
        QPointF end;
        QRectF expected;
    };
    const Case cases[] = {
        {QPointF(100.49, 100.49), QPointF(160.49, 140.49), QRectF(100, 100, 61, 41)},
        {QPointF(160.49, 140.49), QPointF(100.49, 100.49), QRectF(100, 100, 61, 41)},
        {QPointF(100, 100), QPointF(160, 100), QRectF(100, 100, 61, 1)},
        {QPointF(100, 100), QPointF(100, 140), QRectF(100, 100, 1, 41)},
        {QPointF(100, 100), QPointF(-20, -30), QRectF(0, 0, 101, 101)},
        {QPointF(1100, 700), QPointF(1300, 900), QRectF(1100, 700, 100, 100)},
        {QPointF(100.1, 100.2), QPointF(100.4, 100.3), QRectF(100, 100, 0, 0)},
    };
    for (const Case& scenario : cases) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.displays.startup->resumeLiveInput();
        fixture.interaction.returnToSelectionMode(false);
        fixture.selection.clearSelection();
        fixture.services->updateOverlayState();
        fixture.resetCounters();
        int confirmations = 0;
        auto actions = pickerInputActions(fixture);
        actions.selectionConfirmed = [&] { ++confirmations; };
        ScreenshotOverlayInputHandler input(
            {fixture.captureState, fixture.interaction, fixture.selection,
             fixture.intelligentSelection, fixture.geometry, fixture.displays, std::move(actions)});
        input.setExternalDragActive(true);
        input.beginExternalSelectionDrag(scenario.start, scenario.end);
        require(fixture.selection.normalizedSelection() == scenario.expected &&
                    fixture.stateNotifications == 1,
                "batched global selection must preserve rounded, reversed and bounded pixels");
        require(fixture.selection.hasPixelSelection() == !scenario.expected.isEmpty(),
                "pointer movement inside the pressed pixel must remain an empty gesture");
        input.handleMouseRelease(&fixture.overlay, scenario.end);
        require(fixture.interaction.dragging() && confirmations == 0,
                "local mouse release must not finish an externally owned gesture");
        fixture.interaction.finishDrag();
        input.setExternalDragActive(false);
        input.confirmSelection();
        require(
            confirmations == (scenario.expected.isEmpty() ? 0 : 1),
            "external release must confirm usable selections and leave empty gestures unconfirmed");
        require(fixture.selection.normalizedSelection() == scenario.expected,
                "external confirmation must preserve the initialized drag geometry");
    }
}

void batchedExternalSelectionPreservesContinuedDraggingAndHeldShortcuts() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.displays.startup->resumeLiveInput();
    fixture.interaction.returnToSelectionMode(false);
    fixture.selection.clearSelection();
    fixture.services->updateOverlayState();
    auto actions = pickerInputActions(fixture);
    int confirmations = 0;
    actions.selectionConfirmed = [&] { ++confirmations; };
    ScreenshotOverlayInputHandler input({fixture.captureState, fixture.interaction,
                                         fixture.selection, fixture.intelligentSelection,
                                         fixture.geometry, fixture.displays, std::move(actions)});
    input.setExternalDragActive(true);
    input.beginExternalSelectionDrag(QPointF(100, 100), QPointF(160, 140));
    const QRectF initial(100, 100, 61, 41);
    fixture.resetCounters();
    input.updateExternalSelectionDrag(QPointF(170, 150));
    require(fixture.selection.normalizedSelection() == QRectF(100, 100, 71, 51) &&
                fixture.displayedSelection() == initial && fixture.stateNotifications == 0,
            "continued external dragging must retain the original anchor and remain frame paced");
    fixture.advanceClock(17);
    fixture.flushFrame();
    require(fixture.displayedSelection() == QRectF(100, 100, 71, 51) &&
                fixture.stateNotifications == 1,
            "the next drag frame must commit geometry from the batched gesture's original anchor");
    require(input.activateMoveEntireSelectionShortcut(),
            "the batched external gesture must accept Space movement");
    input.updateExternalSelectionDrag(QPointF(190, 180));
    require(fixture.selection.normalizedSelection() == QRectF(120, 130, 71, 51),
            "Space must rebase movement at the latest endpoint without resizing the selection");
    require(input.releaseMoveEntireSelectionShortcut(),
            "Space release must restore the original marquee drag mode");
    input.updateExternalSelectionDrag(QPointF(210, 190));
    require(fixture.selection.normalizedSelection() == QRectF(120, 130, 91, 61),
            "resizing after Space release must retain the translated original anchor");
    require(input.activateKeepSelectionAspectRatioShortcut(false),
            "the continued external gesture must accept Shift resizing");
    input.updateExternalSelectionDrag(QPointF(230, 200));
    const QRectF square(120, 130, 111, 111);
    require(fixture.selection.normalizedSelection() == square,
            "Shift must constrain the continued batched external gesture to a square");
    require(input.releaseKeepSelectionAspectRatioShortcut(),
            "the external gesture's Shift shortcut must release normally");
    fixture.interaction.finishDrag();
    input.setExternalDragActive(false);
    input.confirmSelection();
    require(fixture.displayedSelection() == square && confirmations == 1 &&
                fixture.captureState.sessionState == ScreenshotSessionState::Editing,
            "external release must synchronously confirm the latest pending drag geometry once");
}

void batchedExternalSelectionConsumesSpaceHeldBeforeInitialization() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.interaction.returnToSelectionMode(false);
    fixture.selection.clearSelection();
    require(fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::Marquee),
            "capture preparation must allow Space to arm before usable global geometry arrives");
    fixture.services->updateOverlayState();
    ScreenshotOverlayInputHandler input(
        {fixture.captureState, fixture.interaction, fixture.selection, fixture.intelligentSelection,
         fixture.geometry, fixture.displays, pickerInputActions(fixture)});
    input.setExternalDragActive(true);
    require(input.activateMoveEntireSelectionShortcut() &&
                fixture.interaction.dragMode() == ScreenshotSelectionDragMode::Marquee,
            "Space held over an empty marquee must defer movement until usable geometry exists");
    fixture.resetCounters();
    input.beginExternalSelectionDrag(QPointF(100, 100), QPointF(160, 140));
    require(fixture.interaction.dragMode() == ScreenshotSelectionDragMode::All &&
                fixture.selection.normalizedSelection() == QRectF(100, 100, 61, 41) &&
                fixture.displayedSelection() == QRectF(100, 100, 61, 41) &&
                fixture.stateNotifications == 1,
            "batched initialization must commit usable geometry already rebased for held Space");
    input.updateExternalSelectionDrag(QPointF(180, 170));
    require(fixture.selection.normalizedSelection() == QRectF(120, 130, 61, 41),
            "Space held before initialization must translate from the complete latest endpoint");
    require(input.releaseMoveEntireSelectionShortcut() &&
                fixture.interaction.dragMode() == ScreenshotSelectionDragMode::Marquee,
            "releasing pre-initialization Space must restore marquee resizing");
    input.updateExternalSelectionDrag(QPointF(200, 180));
    require(fixture.selection.normalizedSelection() == QRectF(120, 130, 81, 51),
            "marquee resizing after pre-initialization Space must retain the translated anchor");
}

void batchedExternalSelectionPreservesRememberedRatioAndShiftOverrideOnRelease() {
    for (const bool shiftHeld : {false, true}) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.interaction.returnToSelectionMode(false);
        fixture.selection.clearSelection();
        require(fixture.selection.setAspectRatioPreset(
                    ScreenshotSelectionAspectRatioPreset::Landscape16x9,
                    fixture.geometry.canvasBounds(),
                    snow_shot::presentation::kScreenshotSelectionMinimumSize),
                "the next global gesture must retain its remembered aspect ratio preset");
        fixture.services->updateOverlayState();
        ScreenshotOverlayInputHandler input({fixture.captureState, fixture.interaction,
                                             fixture.selection, fixture.intelligentSelection,
                                             fixture.geometry, fixture.displays,
                                             pickerInputActions(fixture)});
        if (shiftHeld)
            require(input.activateKeepSelectionAspectRatioShortcut(false),
                    "Shift must arm before the complete external gesture is initialized");
        input.setExternalDragActive(true);
        input.beginExternalSelectionDrag(QPointF(100, 100), QPointF(160, 140));
        const QRectF preview = fixture.selection.normalizedSelection();
        require(preview.topLeft() == QPointF(100, 100) &&
                    qFuzzyCompare(preview.height() / preview.width(), shiftHeld ? 1.0 : 9.0 / 16.0),
                "batched geometry must apply the remembered ratio or the held Shift override");
        if (shiftHeld)
            require(preview == QRectF(100, 100, 61, 61),
                    "a held Shift override must use the inclusive pointer-cell square span");
        fixture.interaction.finishDrag();
        input.setExternalDragActive(false);
        input.confirmSelection();
        require(fixture.selection.normalizedSelection() == preview &&
                    fixture.displayedSelection() == preview,
                "release must preserve the drag preview after transient shortcut state resets");
        require(fixture.selection.aspectRatioPreset() ==
                    (shiftHeld ? ScreenshotSelectionAspectRatioPreset::Free
                               : ScreenshotSelectionAspectRatioPreset::Landscape16x9),
                "the drag marker must retain a Shift override without reshaping to the preset");
    }
}

void selectionDragFramesShareTheLatestGeometryAndPickerAnchor() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.displays.startup->resumeLiveInput();
    fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysShow);
    fixture.coordinator.setSelectionToolbarHidden(true);
    require(fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::BottomRight),
            "drag sampling requires a real resize gesture");
    fixture.services->updateOverlayState();
    const QRectF initial = fixture.selection.normalizedSelection();
    fixture.services->requestSelectionDragColorPickerPresentation(initial.bottomRight());
    require(fixture.coordinator.colorPicker()->workCounters().samples == 1,
            "gesture entry must synchronously sample its first drag anchor");
    fixture.resetCounters();
    QRectF latest;
    QPointF pointer;
    for (int request = 1; request <= 16; ++request) {
        latest = initial.adjusted(0, 0, request * 4.0, request * 2.0);
        pointer = latest.bottomRight() + QPointF(30, 20);
        fixture.selection.setDraggedSelectionRect(latest, ScreenshotSelectionDragMode::BottomRight);
        fixture.services->requestSelectionDragPresentation();
        fixture.services->requestSelectionDragColorPickerPresentation(pointer);
        fixture.services->requestColorPickerPresentation(&fixture.overlay, QPointF(900, 700));
    }
    auto* picker = fixture.coordinator.colorPicker();
    require(fixture.stateNotifications == 0 && picker->workCounters().samples == 0,
            "continuous resize and picker work must wait for their shared display frame");
    fixture.advanceClock(17);
    fixture.flushFrame();
    fixture.processEvents();
    const auto anchor =
        screenshotSelectionDragAnchor(latest, ScreenshotSelectionDragMode::BottomRight, pointer,
                                      snow_shot::presentation::kScreenshotSelectionMinimumSize);
    require(anchor.has_value(), "a resized selection must expose its constrained drag anchor");
    const auto& display = fixture.displays.displayAt(0);
    const QPoint physical =
        fixture.geometry.physicalPositionForCanvasPoint(fixture.displays, anchor.value());
    const QString expected = display.image.pixelColor(physical - display.physicalRect.topLeft())
                                 .name(QColor::HexRgb)
                                 .toUpper();
    require(fixture.displayedSelection() == latest && fixture.stateNotifications == 1 &&
                picker->workCounters().samples == 1 && picker->workCounters().previews == 1 &&
                picker->currentColorText() == expected && fixture.paintObserver.canvasPaints == 1,
            "a drag frame must paint and sample the latest constrained geometry once");

    fixture.resetCounters();
    latest.adjust(0, 0, 20, 10);
    fixture.selection.setDraggedSelectionRect(latest, ScreenshotSelectionDragMode::BottomRight);
    fixture.services->requestSelectionDragPresentation();
    fixture.services->requestSelectionDragColorPickerPresentation(latest.bottomRight());
    fixture.services->flushColorPickerPresentation();
    const auto latestAnchor = screenshotSelectionDragAnchor(
        latest, ScreenshotSelectionDragMode::BottomRight, latest.bottomRight(),
        snow_shot::presentation::kScreenshotSelectionMinimumSize);
    require(latestAnchor.has_value(), "clipboard sampling must resolve the latest drag anchor");
    const QPoint latestPhysical =
        fixture.geometry.physicalPositionForCanvasPoint(fixture.displays, latestAnchor.value());
    const QString latestColor =
        display.image.pixelColor(latestPhysical - display.physicalRect.topLeft())
            .name(QColor::HexRgb)
            .toUpper();
    require(picker->workCounters().samples == 1 && fixture.displayedSelection() != latest &&
                picker->currentColorText() == latestColor &&
                fixture.colorPickerController->copyColorToClipboard(
                    fixture.services->colorPickerContext()) &&
                QApplication::clipboard()->text() == latestColor,
            "clipboard commands must sample current drag geometry before its presentation frame");
    fixture.flushFrame();
    require(fixture.displayedSelection() == latest && picker->workCounters().samples == 1,
            "the later geometry commit must not replay a command's already consumed sample");

    fixture.resetCounters();
    latest.adjust(0, 0, 17, 9);
    fixture.selection.setDraggedSelectionRect(latest, ScreenshotSelectionDragMode::BottomRight);
    fixture.services->requestSelectionDragPresentation();
    fixture.services->requestSelectionDragColorPickerPresentation(latest.bottomRight());
    fixture.services->flushColorPickerPresentation();
    const auto formatAnchor = screenshotSelectionDragAnchor(
        latest, ScreenshotSelectionDragMode::BottomRight, latest.bottomRight(),
        snow_shot::presentation::kScreenshotSelectionMinimumSize);
    require(formatAnchor.has_value(), "format commands must resolve their latest drag anchor");
    const QPoint formatPhysical =
        fixture.geometry.physicalPositionForCanvasPoint(fixture.displays, formatAnchor.value());
    const QString formatColor =
        display.image.pixelColor(formatPhysical - display.physicalRect.topLeft())
            .name(QColor::HexRgb)
            .toUpper();
    const auto context = fixture.services->colorPickerContext();
    require(picker->currentColorText() == formatColor && formatColor != latestColor &&
                fixture.colorPickerController->cycleFormat(context) &&
                fixture.colorPickerController->copyColorToClipboard(context) &&
                QApplication::clipboard()->text() == picker->currentColorText() &&
                picker->workCounters().samples == 1,
            "format commands must consume the latest queued drag sample exactly once");
    fixture.flushFrame();
    require(picker->workCounters().samples == 1,
            "format commands must not replay their drag sample at the next frame");
    static_cast<void>(fixture.colorPickerController->cycleFormat(context));
    static_cast<void>(fixture.colorPickerController->cycleFormat(context));
    static_cast<void>(fixture.colorPickerController->cycleFormat(context));
}

void explicitAspectRatioSnapCommitsQueuedDragWork() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.displays.startup->resumeLiveInput();
    fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysShow);
    fixture.coordinator.setSelectionToolbarHidden(true);
    fixture.interaction.returnToSelectionMode(false);
    fixture.selection.clearSelection();
    fixture.services->updateOverlayState();
    ScreenshotOverlayInputHandler input(
        {fixture.captureState, fixture.interaction, fixture.selection, fixture.intelligentSelection,
         fixture.geometry, fixture.displays, pickerInputActions(fixture)});
    input.handleMousePress(&fixture.overlay, QPointF(200, 200));
    // The first usable marquee is deliberately synchronous; measure a later move.
    input.handleMouseMove(&fixture.overlay, QPointF(400, 300));
    fixture.flushFrame();
    fixture.resetCounters();
    input.handleMouseMove(&fixture.overlay, QPointF(440, 320));
    auto* picker = fixture.coordinator.colorPicker();
    require(fixture.stateNotifications == 0 && picker->workCounters().samples == 0,
            "the pointer move before a snap command must still be frame paced");
    require(input.activateSelectionAspectRatioSnapShortcut(),
            "the active marquee must accept its explicit aspect ratio snap command");
    require(fixture.displayedSelection() == fixture.selection.normalizedSelection() &&
                fixture.stateNotifications == 1 && picker->workCounters().samples == 1,
            "an explicit snap command must synchronously commit geometry and its drag sample");
    fixture.flushFrame();
    require(fixture.stateNotifications == 1 && picker->workCounters().samples == 1,
            "a later frame must not replay work consumed by the snap command");

    require(input.releaseSelectionAspectRatioSnapShortcut(),
            "the snap key must release before a fresh unlock press");
    fixture.resetCounters();
    input.handleMouseMove(&fixture.overlay, QPointF(480, 335));
    const QRectF beforeUnlock = fixture.selection.normalizedSelection();
    require(fixture.stateNotifications == 0 && picker->workCounters().samples == 0,
            "the pointer move before unlocking must still be frame paced");
    require(
        input.activateSelectionAspectRatioSnapShortcut() &&
            !fixture.selection.aspectRatioLocked() &&
            fixture.selection.normalizedSelection() == beforeUnlock &&
            fixture.displayedSelection() == beforeUnlock && fixture.stateNotifications == 1 &&
            picker->workCounters().samples == 1,
        "unlocking must synchronously present the free state and latest sample without resizing");
    fixture.flushFrame();
    require(fixture.stateNotifications == 1 && picker->workCounters().samples == 1,
            "a later frame must not replay work consumed by the unlock command");
}

void selectionDragPickerWorkIsDiscardedAtInteractionBoundaries() {
    for (int boundary = 0; boundary < 6; ++boundary) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysHide);
        require(fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::BottomRight),
                "boundary sampling requires an active resize gesture");
        fixture.services->updateOverlayState();
        fixture.services->requestSelectionDragColorPickerPresentation(
            fixture.selection.normalizedSelection().bottomRight());
        fixture.resetCounters();
        const QRectF resized = fixture.selection.normalizedSelection().adjusted(0, 0, 20, 10);
        fixture.selection.setDraggedSelectionRect(resized,
                                                  ScreenshotSelectionDragMode::BottomRight);
        fixture.services->requestSelectionDragPresentation();
        fixture.services->requestSelectionDragColorPickerPresentation(resized.bottomRight());
        switch (boundary) {
        case 0:
            ++fixture.captureState.sessionId;
            break;
        case 1:
            ++fixture.displays.startup->layoutGeneration;
            break;
        case 2:
            fixture.interaction.finishDrag();
            break;
        case 3:
            fixture.captureState.presentationSuppressed = true;
            break;
        case 4:
            fixture.services->resetPresentation();
            break;
        case 5:
            require(fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::All),
                    "a resize gesture must allow a drag-mode boundary");
            break;
        }
        fixture.services->flushColorPickerPresentation();
        fixture.flushFrame();
        require(fixture.coordinator.colorPicker()->workCounters().samples == 0,
                "session, topology, release, suppression, reset and mode changes must discard "
                "the previous drag's sample");
    }
}

void selectionDragsKeepTheColorPickerOpaque() {
    for (const auto mode : {ScreenshotColorPickerDisplayMode::AlwaysShow,
                            ScreenshotColorPickerDisplayMode::HideOutsideSelection,
                            ScreenshotColorPickerDisplayMode::AlwaysHide}) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.enableColorPicker(mode);
        fixture.coordinator.setSelectionToolbarHidden(true);
        auto* picker = fixture.coordinator.colorPicker();
        const auto requirePickerPresentation = [&]() {
            if (mode == ScreenshotColorPickerDisplayMode::AlwaysHide) {
                require(picker->isHidden(), "always-hide must still hide the picker during drags");
                return;
            }
            require(picker->isVisible(), "selection drags must keep the picker visible");
            const QImage painted = picker->grab().toImage();
            const QPoint sample(qRound(30 * painted.devicePixelRatio()),
                                qRound(30 * painted.devicePixelRatio()));
            require(painted.pixelColor(sample).alpha() == 255 && picker->windowOpacity() == 1.0,
                    "selection drags must render the magnified pixels at full opacity");
        };
        for (const bool confirmedSelection : {false, true}) {
            for (const auto dragMode :
                 {ScreenshotSelectionDragMode::Marquee, ScreenshotSelectionDragMode::All,
                  ScreenshotSelectionDragMode::TopLeft, ScreenshotSelectionDragMode::Top,
                  ScreenshotSelectionDragMode::TopRight, ScreenshotSelectionDragMode::Right,
                  ScreenshotSelectionDragMode::BottomRight, ScreenshotSelectionDragMode::Bottom,
                  ScreenshotSelectionDragMode::BottomLeft, ScreenshotSelectionDragMode::Left}) {
                fixture.interaction.returnToSelectionMode(false);
                if (confirmedSelection)
                    fixture.interaction.confirmSelection();
                require(fixture.interaction.enterSelectionDrag(dragMode),
                        "picker regression requires an active selection drag");
                fixture.services->updateOverlayState();
                const auto context = fixture.services->colorPickerContext();
                const QPointF pointer = fixture.baseSelection().center();
                fixture.colorPickerController->updateForOverlay(&fixture.overlay, pointer, context);
                requirePickerPresentation();
                fixture.colorPickerController->updateForSelectionDrag(pointer, context);
                requirePickerPresentation();
                require(picker->hasCurrentColor() &&
                            fixture.colorPickerController->copyColorToClipboard(context),
                        "selection drags must preserve color sampling in every display mode");
                fixture.interaction.finishDrag();
                fixture.colorPickerController->updateForOverlay(
                    &fixture.overlay, fixture.baseSelection().center(),
                    fixture.services->colorPickerContext());
                requirePickerPresentation();
            }
        }
    }
}

void queuedPickerInputSurvivesSelectionConfirmation() {
    for (const auto mode : {ScreenshotColorPickerDisplayMode::AlwaysShow,
                            ScreenshotColorPickerDisplayMode::HideOutsideSelection,
                            ScreenshotColorPickerDisplayMode::AlwaysHide}) {
        for (const bool mouseConfirmation : {true, false}) {
            Fixture fixture(QSize(1200, 800), false);
            fixture.displays.startup->resumeLiveInput();
            fixture.enableColorPicker(mode);
            fixture.coordinator.setSelectionToolbarHidden(true);
            require(fixture.intelligentSelection.applyCanvasHitPath(
                        {fixture.baseSelection()}, fixture.geometry.canvasBounds(), 1.0),
                    "picker confirmation requires a valid smart selection");
            ScreenshotOverlayInputHandler input({fixture.captureState, fixture.interaction,
                                                 fixture.selection, fixture.intelligentSelection,
                                                 fixture.geometry, fixture.displays,
                                                 pickerInputActions(fixture)});
            auto* picker = fixture.coordinator.colorPicker();
            const QString previousColor = picker->currentColorText();
            QPointF latest;
            for (int sample = 0; sample < 16; ++sample) {
                latest = QPointF(300 + sample, 200 + sample);
                input.handleMouseMove(&fixture.overlay, latest);
            }
            require(picker->workCounters().samples == 0,
                    "hover bursts must remain deferred until selection confirmation");
            if (mouseConfirmation) {
                latest += QPointF(10, 10);
                input.handleMousePress(&fixture.overlay, latest);
                require(picker->workCounters().samples == 0,
                        "a smart-selection press must retain hover batching");
                input.handleMouseRelease(&fixture.overlay, latest);
            } else {
                input.confirmSelection();
            }
            const QString expectedColor = sampledColor(fixture, latest);
            require(previousColor != expectedColor && fixture.interaction.movingSelection(),
                    "confirmation must enter editing at a new sample point");
            fixture.services->flushColorPickerPresentation();
            require(fixture.colorPickerController->copyColorToClipboard(
                        fixture.services->colorPickerContext()) &&
                        QApplication::clipboard()->text() == expectedColor,
                    "confirmation before a display frame must copy the latest cursor pixel");
            const auto counters = picker->workCounters();
            require(counters.samples == 1 &&
                        counters.previews ==
                            (mode == ScreenshotColorPickerDisplayMode::AlwaysHide ? 0U : 1U),
                    "confirmation must sample once using the current picker visibility");
            fixture.flushFrame();
            require(picker->workCounters().samples == 1,
                    "a later frame must not replay the confirmed sample");
        }
    }
}

void queuedPickerHoverYieldsToSelectionDrag() {
    for (const bool confirmedSelection : {false, true}) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.displays.startup->resumeLiveInput();
        fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysShow);
        fixture.coordinator.setSelectionToolbarHidden(true);
        if (confirmedSelection)
            fixture.interaction.confirmSelection();
        else
            fixture.interaction.returnToSelectionMode(false);
        fixture.services->updateOverlayState();
        auto* picker = fixture.coordinator.colorPicker();
        auto actions = pickerInputActions(fixture);
        const auto sampleDrag = actions.updateColorPickerForSelectionDrag;
        bool anchorSampled = false;
        actions.updateColorPickerForSelectionDrag = [&](const QPointF& position) {
            require(picker->workCounters().samples == 0 && picker->workCounters().previews == 0,
                    "starting a drag must discard hover before presenting its sample anchor");
            sampleDrag(position);
            anchorSampled = true;
        };
        ScreenshotOverlayInputHandler input(
            {fixture.captureState, fixture.interaction, fixture.selection,
             fixture.intelligentSelection, fixture.geometry, fixture.displays, std::move(actions)});
        fixture.requestMagnifier(QPointF(310, 210));
        input.handleMousePress(&fixture.overlay, QPointF(300, 200));
        require(anchorSampled && fixture.interaction.dragging() &&
                    picker->workCounters().samples == 1 && picker->workCounters().previews == 1,
                "a drag must sample only its direct anchor at both same-mode and mode boundaries");
        const QString anchorColor = picker->currentColorText();
        fixture.services->requestColorPickerPresentation(&fixture.overlay, QPointF(400, 280));
        fixture.services->flushColorPickerPresentation();
        fixture.flushFrame();
        require(picker->currentColorText() == anchorColor && picker->workCounters().samples == 1 &&
                    picker->workCounters().previews == 1,
                "raw hover requests during dragging must not replace the direct sample anchor");
    }
}

void queuedPickerSamplesTheLatestPointBeforeClipboardCommands() {
    for (const auto mode : {ScreenshotColorPickerDisplayMode::AlwaysShow,
                            ScreenshotColorPickerDisplayMode::AlwaysHide}) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.enableColorPicker(mode);
        QPointF latest;
        for (int sample = 0; sample < 16; ++sample) {
            latest = QPointF(800 + sample, 500 + sample);
            fixture.requestMagnifier(latest);
        }
        auto* picker = fixture.coordinator.colorPicker();
        require(picker->workCounters().samples == 0,
                "queued picker input must wait until a presentation or clipboard boundary");
        fixture.services->flushColorPickerPresentation();
        const auto context = fixture.services->colorPickerContext();
        require(fixture.colorPickerController->copyColorToClipboard(context) &&
                    QApplication::clipboard()->text() == picker->currentColorText() &&
                    picker->currentColorText().contains(sampledColor(fixture, latest).mid(1)),
                "copy must sample the latest queued point before reading its formatted color");
        auto counters = picker->workCounters();
        require(counters.samples == 1 &&
                    counters.previews ==
                        (mode == ScreenshotColorPickerDisplayMode::AlwaysShow ? 1U : 0U),
                "one picker boundary must coalesce sixteen inputs and skip hidden previews");
        fixture.flushFrame();
        require(picker->workCounters().samples == 1,
                "a later frame must not replay the sample already flushed for copying");
        latest += QPointF(10, 10);
        fixture.requestMagnifier(latest);
        fixture.services->flushColorPickerPresentation();
        require(fixture.colorPickerController->cycleFormat(context) &&
                    fixture.colorPickerController->copyColorToClipboard(context) &&
                    QApplication::clipboard()->text() == picker->currentColorText(),
                "format changes must act on the newly sampled point before copying");
        static_cast<void>(fixture.colorPickerController->cycleFormat(context));
        static_cast<void>(fixture.colorPickerController->cycleFormat(context));
        static_cast<void>(fixture.colorPickerController->cycleFormat(context));
        if (mode == ScreenshotColorPickerDisplayMode::AlwaysShow) {
            latest += QPointF(10, 10);
            fixture.requestMagnifier(latest);
            fixture.services->flushColorPickerPresentation();
            const auto& display = fixture.displays.displayAt(0);
            const QPoint physical = fixture.geometry.physicalPositionForCanvasPoint(
                fixture.displays, fixture.geometry.canvasPositionForOverlayLocalPoint(
                                      fixture.displays, &fixture.overlay, latest));
            const auto conversion = screenshotSelectionDisplayConversion(
                fixture.geometry, fixture.displays, context.selectionPixels,
                context.selectionDisplayUnit, &display);
            const auto relative = screenshotMagnifierRelativeDisplayPosition(
                fixture.geometry, display, physical, context.selectionPixels, conversion);
            require(relative && fixture.colorPickerController->toggleCoordinateMode(context) &&
                        picker->currentPositionText() == QStringLiteral("X: %1 Y: %2")
                                                             .arg(qRound(relative->x()))
                                                             .arg(qRound(relative->y())),
                    "coordinate toggles must use the latest queued point and selection origin");
            static_cast<void>(fixture.colorPickerController->toggleCoordinateMode(context));
        }
    }
}

void explicitCursorSamplingCannotBeOverwrittenByQueuedHover() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysHide);
    fixture.requestMagnifier(QPointF(800, 500));
    fixture.services->discardColorPickerPresentation();
    const QPointF keyboardPoint(820, 510);
    fixture.colorPickerController->updateForOverlay(&fixture.overlay, keyboardPoint,
                                                    fixture.services->colorPickerContext());
    auto* picker = fixture.coordinator.colorPicker();
    const QString color = picker->currentColorText();
    fixture.flushFrame();
    require(picker->workCounters().samples == 1 && picker->currentColorText() == color &&
                color == sampledColor(fixture, keyboardPoint),
            "an immediate cursor update must discard the older queued hover sample");
}

void pickerWorkIsDiscardedAtCaptureAndDisabledToolBoundaries() {
    for (int boundary = 0; boundary < 5; ++boundary) {
        Fixture fixture(QSize(1200, 800), false);
        fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysHide);
        fixture.requestMagnifier(QPointF(800, 500));
        switch (boundary) {
        case 0:
            ++fixture.captureState.sessionId;
            break;
        case 1:
            fixture.captureState.presentationSuppressed = true;
            break;
        case 2:
            fixture.interaction.setCanvasTool(ScreenshotActiveTool::Shape);
            break;
        case 3:
            fixture.interaction.reset();
            break;
        case 4:
            fixture.overlay.setCaptureGeometry(
                fixture.overlay.captureGeometry().translated(100, 50));
            break;
        }
        fixture.services->flushColorPickerPresentation();
        require(fixture.coordinator.colorPicker()->workCounters().samples == 0,
                "session, suppression, disabled tools and owner geometry must discard old input");
        if (boundary == 1)
            require(!fixture.colorPickerController->copyColorToClipboard(
                        fixture.services->colorPickerContext()),
                    "suppressed presentation must not copy a stale cached sample");
        fixture.flushFrame();
        require(fixture.coordinator.colorPicker()->workCounters().samples == 0,
                "a later frame must not revive discarded picker work");
        if (boundary == 2) {
            fixture.interaction.setMoveTool(true, false);
            fixture.services->updateOverlayState();
            fixture.services->flushColorPickerPresentation();
            require(fixture.coordinator.colorPicker()->workCounters().samples == 0,
                    "re-enabling the move tool must not replay input from before a disabled tool");
        }
    }
}

void destroyedPickerOwnersAndPopupEntryCancelPendingInput() {
    Fixture fixture(QSize(1200, 800), false);
    fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysHide);
    auto* temporaryOwner =
        new ScreenshotOverlayWindow(fixture.events, new SnowCanvasWidget(fixture.canvasRuntime));
    fixture.services->requestColorPickerPresentation(temporaryOwner, QPointF(20, 20));
    delete temporaryOwner;
    fixture.services->flushColorPickerPresentation();
    require(fixture.coordinator.colorPicker()->workCounters().samples == 0,
            "deleting a queued picker owner must cancel its deferred sample");
    fixture.coordinator.showToolbar();
    auto* trigger = fixture.coordinator.toolbar()->findChild<QWidget*>(
        QStringLiteral("screenshotArrowLineButton"));
    require(trigger && trigger->isVisible(),
            "picker cancellation requires a visible toolbar button");
    fixture.coordinator.hideColorPicker();
    fixture.requestMagnifier(QPointF(800, 500));
    const QPoint local = trigger->rect().center();
    const QPoint global = trigger->mapToGlobal(local);
    QMouseEvent move(QEvent::MouseMove, local, trigger->window()->mapFromGlobal(global), global,
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(trigger, &move);
    fixture.services->flushColorPickerPresentation();
    require(fixture.coordinator.colorPicker()->workCounters().samples == 0 &&
                fixture.coordinator.colorPicker()->isHidden(),
            "screenshot UI entry must cancel queued samples even while the picker is hidden");
}
void deferredToolbarRaiseEndsWithItsPresenter() {
    Fixture fixture;
    fixture.coordinator.showToolbar();
    auto* toolbar = fixture.coordinator.toolbar();
    require(toolbar && toolbar->isVisible(), "the lifetime fixture needs a visible toolbar");
    ScreenshotToolbarPresentationState state;
    state.editing = true;
    auto presenter = std::make_unique<ScreenshotToolbarPresenter>(
        fixture.coordinator, fixture.geometry, fixture.displays);
    presenter->raiseToolbarForCanvasInteraction(state);
    presenter.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    Fixture::processEvents();
    require(toolbar->isVisible(), "retiring the presenter must preserve its reusable toolbar");

    fixture.toolbar.raiseToolbarForCanvasInteraction(state);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    Fixture::processEvents();
    require(toolbar->isVisible(), "a live presenter must still raise the toolbar safely");
}

void nativeToolbarRaiseAllowsPresenterDeletion() {
    class RaiseObserver final : public QObject {
      public:
        std::function<void()> onRaise;
        int raises = 0;

      protected:
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::ZOrderChange) {
                ++raises;
                onRaise();
            }
            return false;
        }
    };
    for (const int retiringRaise : {1, 2}) {
        Fixture fixture;
        fixture.coordinator.showToolbar();
        Fixture::processEvents();
        auto* toolbar = fixture.coordinator.toolbar();
        require(toolbar && toolbar->isVisible(), "native raise needs a reusable visible toolbar");
        auto presenter = std::make_unique<ScreenshotToolbarPresenter>(
            fixture.coordinator, fixture.geometry, fixture.displays);
        QPointer<ScreenshotToolbarPresenter> lifetime(presenter.get());
        RaiseObserver observer;
        observer.onRaise = [&] {
            if (observer.raises == retiringRaise)
                presenter.reset();
        };
        toolbar->installEventFilter(&observer);
        ScreenshotToolbarPresentationState state;
        state.editing = true;
        presenter->raiseToolbarForCanvasInteraction(state);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        Fixture::processEvents();
        require(!lifetime && observer.raises == retiringRaise && toolbar->isVisible(),
                "immediate and delayed native raises may retire their presenter safely");
    }
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    selection_presentation_test::IsolatedStorage storage;
    if (application.arguments().contains(QStringLiteral("--picker-drag-only"))) {
        selectionDragsKeepTheColorPickerOpaque();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--toolbar-presenter-lifetime-only"))) {
        deferredToolbarRaiseEndsWithItsPresenter();
        nativeToolbarRaiseAllowsPresenterDeletion();
        return 0;
    }
    deferredToolbarRaiseEndsWithItsPresenter();
    nativeToolbarRaiseAllowsPresenterDeletion();
    independentRegionOutlineHasLocalDamage();
    regionSnapGuidesReachPresentationFrames();
    independentDragDamageCoversMasksAndContours();
    repeatedSelectionLeavesThePresentedUiIdle();
    multipleRequestsCommitOnlyTheirLatestSelection();
    theFirstResultAfterAnEmptySelectionIsImmediate();
    selectionDragBurstsPrepareAndCommitTheLatestModelAtOneFrame();
    selectionDragBoundariesAndCommandsCommitSynchronously();
    theFirstUsableMarqueeIsPresentedImmediately();
    canceledRequestsLeaveTheCommittedFrameUntouched();
    captureLifecycleChangesNotifyEvenWhenGeometryIsUnchanged();
    displayRebindingReappliesSelectionAfterRendererReset();
    endingSelectionMovementRefreshesTheDeferredToolbarContent();
    animationFramesOnlyChangeDisplayedGeometry();
    animationPreferencesSelectDurationsAndDisableRunningFrames();
    hiddenSelectionToolbarDefersPresentationUntilReveal();
    modeAndSessionChangesCancelOldAnimation();
    anEpochChangeDiscardsPendingWorkWithoutAnotherStateUpdate();
    anEpochChangeReleasesShapedSelectionSnapshots();
    resetReleasesPendingAndCommittedShapedSelections();
    resetRetainsTheIdleSchedulerAndAllowsTheNextCapture();
    resetDiscardsQueuedPickerInputUntilTheNextCapture();
    semanticNotificationCanResetPresentationAndStartTheNextCapture();
    theFrameSchedulerRetainsItsBackendWithoutIdleWakeupsAndStopsAtEpochExit();
    synchronousFrameWorkSkipsExpiredDeadlines();
    pointerBurstsDoNotPublishSemanticChanges();
    pointerUpdatesWithHiddenGuidesRemainAvailableForLaterPresentation();
    qtDeadlineFramesMergeAnimationGuidesAndMagnifierBursts();
    pointerOnlyHintVisibilityUsesTheLatestPresentedSelection();
    displayMovementRecomputesTheAnchoredPointerLocalPosition();
    layoutGenerationChangesRefreshTheAnchoredPointer();
    imageRebindingKeepsAcceptedPointerPrecisionForUnchangedGeometry();
    shortcutHintsFollowRegionOperationTransitions();
    shortcutContentStillRetranslatesOnLanguageChange();
    selectionDragsKeepTheColorPickerOpaque();
    queuedPickerInputSurvivesSelectionConfirmation();
    queuedPickerHoverYieldsToSelectionDrag();
    externalSelectionInitializationCommitsTheCompleteHiddenStateOnce();
    batchedExternalSelectionPreservesPointerGeometryAndEmptyGestures();
    batchedExternalSelectionPreservesContinuedDraggingAndHeldShortcuts();
    batchedExternalSelectionConsumesSpaceHeldBeforeInitialization();
    batchedExternalSelectionPreservesRememberedRatioAndShiftOverrideOnRelease();
    selectionDragFramesShareTheLatestGeometryAndPickerAnchor();
    explicitAspectRatioSnapCommitsQueuedDragWork();
    selectionDragPickerWorkIsDiscardedAtInteractionBoundaries();
    queuedPickerSamplesTheLatestPointBeforeClipboardCommands();
    explicitCursorSamplingCannotBeOverwrittenByQueuedHover();
    pickerWorkIsDiscardedAtCaptureAndDisabledToolBoundaries();
    destroyedPickerOwnersAndPopupEntryCancelPendingInput();
    return 0;
}
