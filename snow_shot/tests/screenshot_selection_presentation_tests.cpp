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
    require(fixture.selection.normalizedSelection() == target,
            "display interpolation must preserve the capture target");
    fixture.advanceClock(8);
    fixture.flushFrame();
    fixture.processEvents();
    require(fixture.displayedSelection() != firstFrame,
            "high-refresh frames must advance independently of a second animation timer");
    fixture.advanceClock(ScreenshotSmartSelectionTransition::kDurationMs);
    fixture.flushFrame();
    fixture.processEvents();

    require(fixture.displayedSelection() == target,
            "animation must finish at the exact capture target after a delayed frame");
    require(fixture.stateNotifications == 0,
            "animation frames must not notify semantic state consumers");
    require(fixture.hintTranslations.requests == 0,
            "animation frames must not translate and format unchanged shortcut rows");
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
    fixture.advanceClock(ScreenshotSmartSelectionTransition::kDurationMs);
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
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    selection_presentation_test::IsolatedStorage storage;
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
    shortcutContentStillRetranslatesOnLanguageChange();
    queuedPickerInputSurvivesSelectionConfirmation();
    queuedPickerHoverYieldsToSelectionDrag();
    selectionDragFramesShareTheLatestGeometryAndPickerAnchor();
    explicitAspectRatioSnapCommitsQueuedDragWork();
    selectionDragPickerWorkIsDiscardedAtInteractionBoundaries();
    queuedPickerSamplesTheLatestPointBeforeClipboardCommands();
    explicitCursorSamplingCannotBeOverwrittenByQueuedHover();
    pickerWorkIsDiscardedAtCaptureAndDisabledToolBoundaries();
    destroyedPickerOwnersAndPopupEntryCancelPendingInput();
    return 0;
}
