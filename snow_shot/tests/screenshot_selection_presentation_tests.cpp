#include "screenshot_selection_presentation_fixture.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"

#include <QPainter>
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

    QPointer<QTimer> timer;

  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event != nullptr && event->type() == QEvent::Timer) {
            auto* observed = qobject_cast<QTimer*>(object);
            if (observed != nullptr &&
                observed->objectName() == QStringLiteral("screenshotPresentationFrameTimer")) {
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

void resetRetainsTheIdleTimerAndAllowsTheNextCapture() {
    Fixture fixture(QSize(320, 240), false);
    PresentationTimerObserver observer;
    fixture.requestPointer(QPointF(100, 150));
    require(observer.waitForNextTimeout(), "the capture must start its presentation timer");
    fixture.advanceClock(50);
    require(observer.waitForNextTimeout() && observer.timer && !observer.timer->isActive(),
            "the capture's frame timer must be idle before explicit teardown");
    const QPointer<QTimer> retainedTimer = observer.timer;

    fixture.services->resetPresentation();
    require(retainedTimer && !retainedTimer->isActive(),
            "teardown must retain the stopped scheduler object for reuse");
    fixture.services->updatePointerPresentation(&fixture.overlay, QPointF(80, 60));
    require(!retainedTimer->isActive(),
            "pointer input before a new presentation must not restart the retired timer");

    ++fixture.captureState.sessionId;
    fixture.displays.startup->sessionId = fixture.captureState.sessionId;
    const QRectF nextSelection = fixture.baseSelection().translated(10, 15);
    fixture.resetCounters();
    fixture.requestSelection(nextSelection);
    require(fixture.displayedSelection() == nextSelection && fixture.stateNotifications == 1,
            "the next capture must synchronously present and notify its first selection");
    fixture.requestPointer(QPointF(90, 70));
    require(observer.waitForNextTimeout() && observer.timer == retainedTimer,
            "the next capture must reuse the original scheduler object");
    require(fixture.displayedSelection() == nextSelection && fixture.stateNotifications == 1,
            "new pointer frames must preserve the new capture's selection and semantic state");
    fixture.services->resetPresentation();
    require(!retainedTimer->isActive(), "teardown must also stop an active presentation timer");
}

void theFrameTimerSurvivesInputBurstsAndStopsAfterIdleOrEpochExit() {
    Fixture fixture(QSize(1200, 800), false);
    PresentationTimerObserver observer;
    fixture.requestPointer(QPointF(100, 150));
    require(observer.waitForNextTimeout(),
            "the scheduled presentation timer must deliver a frame before the watchdog");
    require(observer.timer && observer.timer->isActive() && !observer.timer->isSingleShot(),
            "the real presentation frame timer must remain active after delivering a frame");
    const int timerId = observer.timer->timerId();
    require(timerId >= 0, "an active presentation timer must have a valid native timer ID");

    for (int request = 1; request <= 3; ++request) {
        fixture.requestPointer(QPointF(100 + request * 10, 150));
        fixture.advanceClock(8);
        fixture.flushFrame();
        require(observer.timer->isActive() && observer.timer->timerId() == timerId,
                "subsequent frame commits must reuse the active native presentation timer");
    }
    require(fixture.stateNotifications == 0,
            "reusing the pointer presentation timer must not publish semantic changes");

    fixture.advanceClock(50);
    require(observer.waitForNextTimeout(),
            "the active timer must receive an idle check before the watchdog");
    require(observer.timer && !observer.timer->isActive(),
            "the presentation timer must stop after its virtual idle grace expires");

    // This request queues an immediately eligible frame after the idle period.
    // Ending the epoch before event dispatch must cancel both timer and queued work.
    fixture.requestPointer(QPointF(250, 200));
    require(observer.timer->isActive(), "new pointer work must reactivate the same timer object");
    ++fixture.captureState.sessionId;
    fixture.captureState.sessionState = ScreenshotSessionState::IdlePrepared;
    fixture.overlay.clearScreenshotSelection();
    fixture.resetCounters();
    fixture.flushFrame();
    fixture.processEvents();
    require(observer.timer && !observer.timer->isActive(),
            "a capture epoch exit must cancel an active presentation timer");
    require(fixture.stateNotifications == 0 && !fixture.overlay.hasScreenshotSelection(),
            "the canceled timer and queued first frame must not resurrect old epoch state");
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
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    selection_presentation_test::IsolatedStorage storage;
    repeatedSelectionLeavesThePresentedUiIdle();
    multipleRequestsCommitOnlyTheirLatestSelection();
    theFirstResultAfterAnEmptySelectionIsImmediate();
    canceledRequestsLeaveTheCommittedFrameUntouched();
    captureLifecycleChangesNotifyEvenWhenGeometryIsUnchanged();
    displayRebindingReappliesSelectionAfterRendererReset();
    endingSelectionMovementRefreshesTheDeferredToolbarContent();
    animationFramesOnlyChangeDisplayedGeometry();
    modeAndSessionChangesCancelOldAnimation();
    anEpochChangeDiscardsPendingWorkWithoutAnotherStateUpdate();
    anEpochChangeReleasesShapedSelectionSnapshots();
    resetReleasesPendingAndCommittedShapedSelections();
    resetRetainsTheIdleTimerAndAllowsTheNextCapture();
    theFrameTimerSurvivesInputBurstsAndStopsAfterIdleOrEpochExit();
    pointerBurstsDoNotPublishSemanticChanges();
    pointerUpdatesWithHiddenGuidesRemainAvailableForLaterPresentation();
    pointerOnlyHintVisibilityUsesTheLatestPresentedSelection();
    displayMovementRecomputesTheAnchoredPointerLocalPosition();
    layoutGenerationChangesRefreshTheAnchoredPointer();
    imageRebindingKeepsAcceptedPointerPrecisionForUnchangedGeometry();
    shortcutContentStillRetranslatesOnLanguageChange();
    return 0;
}
