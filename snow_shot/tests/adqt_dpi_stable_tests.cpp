#include "icon_renderer.h"
#include "external_icon_pack.h"
#include "widgets/button.h"
#include "widgets/control_scale.h"
#include "widgets/dpi_stable_window_controller.h"
#include "widgets/floating_surface.h"
#include "widgets/radio.h"
#include "widgets/select.h"
#include "widgets/slider.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QScreen>
#include <QWindow>
#include <QtMath>

#include <cmath>
#include <functional>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace adqt::widgets {

class AdFloatingSurfaceTestAccess {
  public:
    static bool hasShadowCache(const AdFloatingSurface& surface) {
        return surface.shadowCache_ != nullptr;
    }
};

class AdDpiStableWindowControllerTestAccess {
  public:
    static void queueScaleCommit(AdDpiStableWindowController& controller) {
        controller.queueScaleCommit();
    }

    static void commitPendingScale(AdDpiStableWindowController& controller) {
        controller.commitPendingScale();
    }

    static QPoint stableNativeTopLeft(const QPoint& nativeTopLeft, const QSize& nativeFrameSize) {
        return AdDpiStableWindowController::stableNativeTopLeft(nativeTopLeft, nativeFrameSize);
    }

    static void setNativeTransitionActive(AdDpiStableWindowController& controller, bool active) {
        controller.nativeTransitionActive_ = active;
        controller.windowUpdatesWereEnabled_ = false;
    }
};

} // namespace adqt::widgets

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QImage renderWidget(QWidget* widget) {
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    widget->render(&painter);
    return image;
}

struct IconPaintTrace final {
    QVector<qreal> painterDprs;
    QVector<QRect> paintRects;
    QVector<QSize> pixmapSizes;
    int pixmapRequestCount = 0;
};

class DpiTrackingIconEngine final : public QIconEngine {
  public:
    explicit DpiTrackingIconEngine(std::shared_ptr<IconPaintTrace> trace)
        : trace_(std::move(trace)) {}

    QIconEngine* clone() const override {
        return new DpiTrackingIconEngine(trace_);
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        Q_UNUSED(mode)
        Q_UNUSED(state)
        trace_->painterDprs.append(
            painter && painter->device() ? painter->device()->devicePixelRatioF() : 1.0);
        trace_->paintRects.append(rect);
        if (painter != nullptr) {
            painter->fillRect(rect, Qt::black);
        }
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        Q_UNUSED(mode)
        Q_UNUSED(state)
        ++trace_->pixmapRequestCount;
        trace_->pixmapSizes.append(size);
        QPixmap result(size);
        result.fill(Qt::black);
        return result;
    }

  private:
    std::shared_ptr<IconPaintTrace> trace_;
};

class ParticipantWidget final : public QWidget, public adqt::widgets::AdControlScaleParticipant {
  public:
    using QWidget::QWidget;
    void prepareControlScale(const adqt::widgets::AdControlScaleContext& context) override {
        ++prepareCount;
        lastPreparedContext = context;
        lastPreparedRevision = context.revision;
        if (onPrepare)
            onPrepare();
    }
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override {
        ++commitCount;
        lastCommittedContext = context;
        lastCommittedRevision = context.revision;
        if (onCommit)
            onCommit();
    }
    std::function<void()> onPrepare;
    std::function<void()> onCommit;
    int prepareCount = 0;
    int commitCount = 0;
    adqt::widgets::AdControlScaleContext lastPreparedContext;
    adqt::widgets::AdControlScaleContext lastCommittedContext;
    quint64 lastPreparedRevision = 0;
    quint64 lastCommittedRevision = 0;
};

void cumulativeEdgesAreStable() {
    const QVector<qreal> dprs = {1.0, 1.25, 1.5, 1.75, 2.0};
    const QVector<int> widths = {7, 32, 1, 13, 32, 9, 32, 11};
    for (qreal reference : dprs) {
        for (qreal current : dprs) {
            const qreal scale = reference / current;
            const int target = qRound(137 * scale);
            const QVector<int> edges = adqt::widgets::scaleCumulativeWidths(widths, scale, target);
            require(edges.size() == widths.size() + 1,
                    "cumulative edges returned the wrong boundary count");
            require(edges.first() == 0 && edges.last() == target,
                    "cumulative edges did not end at the native target extent");
            for (int i = 1; i < edges.size(); ++i) {
                require(edges.at(i) >= edges.at(i - 1), "cumulative edge order regressed");
                const qreal ideal = widths.at(i - 1) * scale;
                const qreal physicalError =
                    std::abs((edges.at(i) - edges.at(i - 1) - ideal) * current);
                require(physicalError <= current + 0.001,
                        "a cumulative segment exceeded one logical rounding unit");
            }
        }
    }

    const QVector<int> compressed = adqt::widgets::scaleCumulativeWidths({80, 80, 80}, 1.0, 100);
    require(compressed.last() == 100,
            "a smaller native target must remain the exact final boundary");
    for (int i = 1; i < compressed.size(); ++i) {
        require(compressed.at(i) >= compressed.at(i - 1),
                "compressing to a native target inverted cumulative edges");
    }
}

void scopeIsBatchedAndNoOpsRepeatRequests() {
    QWidget root;
    auto* layout = new QHBoxLayout(&root);
    auto* participant = new ParticipantWidget(&root);
    layout->addWidget(participant);
    adqt::widgets::AdControlScaleScope scope(&root);
    int signalCount = 0;
    QObject::connect(&scope, &adqt::widgets::AdControlScaleScope::scaleCommitted,
                     [&signalCount]() { ++signalCount; });
    require(scope.publishScale(1.5, 1.0, QSize(320, 48)), "first scale publication should commit");
    require(participant->prepareCount == 1 && participant->commitCount == 1 &&
                participant->lastPreparedRevision == participant->lastCommittedRevision,
            "participant prepare and commit phases were not paired");
    require(!scope.publishScale(1.5, 1.0, QSize(320, 48)),
            "unchanged scale publication should be a no-op");
    require(participant->commitCount == 1 && signalCount == 1,
            "no-op scale publication produced work");
}

void replacementParticipantsCommitBeforePresentation() {
    QWidget root;
    ParticipantWidget owner(&root);
    QPointer<ParticipantWidget> child = new ParticipantWidget(&owner);
    adqt::widgets::AdControlScaleScope scope(&root);
    owner.onCommit = [&]() {
        delete child.data();
        child = new ParticipantWidget(&owner);
    };
    require(scope.publishScale(1.0, 1.5), "replacement scale publication failed");
    require(child && child->prepareCount == 1 && child->commitCount == 1 &&
                child->lastCommittedContext.equivalentTo(scope.context()),
            "replacement participant missed the presentation transaction");

    QWidget otherRoot;
    ParticipantWidget otherOwner(&otherRoot);
    auto* temporaryScope = new adqt::widgets::AdControlScaleScope(&otherRoot);
    otherOwner.onCommit = [temporaryScope]() { delete temporaryScope; };
    temporaryScope->publishScale(1.0, 2.0);
    require(otherRoot.updatesEnabled(), "deleting the scope stranded update suppression");
}

void contentScaleComposesWithDpiAndParticipatesInEquivalence() {
    const auto context =
        adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1.5, 2.0, 0.8, 9);
    require(qFuzzyCompare(context.logicalScale + 1.0, 1.6),
            "content scale did not compose with the reference/current DPI ratio");
    require(qFuzzyCompare(context.contentScale + 1.0, 1.8) && context.revision == 9,
            "content-scale context did not retain its normalized inputs");
    auto independentlyChangedContentScale = context;
    independentlyChangedContentScale.contentScale = 1.0;
    independentlyChangedContentScale.revision = 10;
    require(!context.equivalentTo(independentlyChangedContentScale),
            "content scale must participate in equivalence independently of logical scale");

    QWidget root;
    adqt::widgets::AdControlScaleScope scope(&root);
    require(scope.publishScale(context), "first content-scale publication should commit");
    require(!scope.publishScale(context), "equivalent content-scale publication should be a no-op");
    const auto changed =
        adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1.5, 2.0, 1.0, 10);
    require(scope.publishScale(changed),
            "changing only content scale must produce a new scale commit");
}

void currentScaleCanBeAppliedToANewSubtree() {
    QWidget root;
    adqt::widgets::AdControlScaleScope scope(&root);
    const auto published =
        adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1.5, 2.0, 0.8);
    require(scope.publishScale(published), "subtree reference publication failed");

    QWidget subtree(&root);
    ParticipantWidget participant(&subtree);
    require(scope.applyCurrentScaleToSubtree(&subtree),
            "current scale was not applied to the new subtree");
    require(participant.prepareCount == 1 && participant.commitCount == 1 &&
                participant.lastCommittedRevision == scope.context().revision,
            "new subtree did not receive one paired prepare/commit at the current revision");
    const auto expected = scope.context();
    const auto contextMatches = [&expected](const adqt::widgets::AdControlScaleContext& actual) {
        return qFuzzyCompare(actual.referenceDpr + 1.0, expected.referenceDpr + 1.0) &&
               qFuzzyCompare(actual.currentDpr + 1.0, expected.currentDpr + 1.0) &&
               qFuzzyCompare(actual.contentScale + 1.0, expected.contentScale + 1.0) &&
               qFuzzyCompare(actual.logicalScale + 1.0, expected.logicalScale + 1.0) &&
               actual.revision == expected.revision;
    };
    require(contextMatches(participant.lastPreparedContext) &&
                contextMatches(participant.lastCommittedContext),
            "new subtree did not receive the complete current scale context");

    QWidget unrelated;
    require(!scope.applyCurrentScaleToSubtree(&unrelated),
            "scale scope accepted a subtree outside its root");
}

void controllerCoalescesToTheLatestPendingScale() {
    QWidget window;
    window.resize(320, 80);
    window.show();
    QApplication::processEvents();

    adqt::widgets::AdControlScaleScope scope(&window);
    adqt::widgets::AdDpiStableWindowController controller(&window);
    controller.setScaleScope(&scope);
    require(controller.captureBaseline(), "controller baseline capture failed");

    int commitCount = 0;
    qreal committedDpr = 0.0;
    QObject::connect(&controller, &adqt::widgets::AdDpiStableWindowController::scaleCommitCompleted,
                     [&commitCount, &committedDpr](
                         const adqt::widgets::AdControlScaleContext& context, const QSize&) {
                         ++commitCount;
                         committedDpr = context.currentDpr;
                     });

    adqt::widgets::AdDpiStableWindowControllerTestAccess::queueScaleCommit(controller);
    adqt::widgets::AdDpiStableWindowControllerTestAccess::queueScaleCommit(controller);
    // The frame moves after the commits were queued: the commit must describe the window as
    // it is when it runs, not as it was when the native transition was observed.
    window.move(window.pos() + QPoint(20, 20));
    QApplication::processEvents();

    const auto diagnostics = controller.diagnostics();
    require(commitCount == 1 &&
                qFuzzyCompare(committedDpr + 1.0, window.windowHandle()->devicePixelRatio() + 1.0),
            "pending DPI messages did not produce one commit at the window's actual DPI");
    require(diagnostics.coalescedCount == 1,
            "queued DPI messages were not coalesced into one commit");
    require(diagnostics.finalPhysicalGeometry == controller.nativeFrameGeometry() &&
                controller.nativeFrameGeometry().size() == controller.stablePhysicalFrameSize(),
            "coalesced DPI diagnostics did not report the frame's current native geometry");
    require(controller.beginPhysicalDrag(QPointF(controller.nativeFrameGeometry().center())),
            "physical drag could not be started on the committed frame");
    require(controller.nativeFrameGeometry() == diagnostics.finalPhysicalGeometry,
            "the committed native geometry disagrees with the frame measured at drag start");
    controller.endPhysicalDrag();
}

void stableNativeTopLeftIsAFixedPointOfQtsLogicalGrid() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "fixed-point test needs a primary screen");
    const qreal dpr = screen->devicePixelRatio();
    const QPoint origin = screen->geometry().topLeft();
    const QSize frame(120, 40);
    // Mirrors QHighDpi::fromNativeWindowGeometry/toNativeWindowGeometry for a top-level window.
    const auto qtRoundTrip = [&](const QPoint& native) {
        const QPoint logical = (native - origin) * (qreal(1) / dpr) + origin;
        return (logical - origin) * dpr + origin;
    };
    const int maxShift = qCeil(dpr / 2.0);
    for (int offset = 0; offset < 24; ++offset) {
        const QPoint candidate = origin + QPoint(40 + offset, 40 + offset);
        const QPoint snapped =
            adqt::widgets::AdDpiStableWindowControllerTestAccess::stableNativeTopLeft(candidate,
                                                                                      frame);
        require(qtRoundTrip(snapped) == snapped,
                "snapped native origin is not a fixed point of Qt's logical mapping");
        require(qAbs(snapped.x() - candidate.x()) <= maxShift &&
                    qAbs(snapped.y() - candidate.y()) <= maxShift,
                "snapping moved the frame further than one logical rounding unit");
        require(adqt::widgets::AdDpiStableWindowControllerTestAccess::stableNativeTopLeft(
                    snapped, frame) == snapped,
                "snapping a stable native origin must be idempotent");
        if (qFuzzyCompare(dpr, 1.0)) {
            require(snapped == candidate, "snapping must be the identity on a 100% screen");
        }
    }
}

void controllerCanKeepReferenceDpiSeparateFromWindowDpi() {
    QWidget window;
    window.resize(320, 80);
    window.show();
    QApplication::processEvents();

    adqt::widgets::AdDpiStableWindowController controller(&window);
    const qreal windowDpr = window.devicePixelRatioF();
    const qreal referenceDpr = windowDpr + 0.5;
    require(controller.captureBaseline(referenceDpr),
            "controller baseline capture with an explicit reference failed");
    require(qFuzzyCompare(controller.referenceDpr() + 1.0, referenceDpr + 1.0),
            "controller did not retain the explicit reference display DPI");
    require(controller.stablePhysicalFrameSize().isValid() &&
                !controller.stablePhysicalFrameSize().isEmpty(),
            "controller lost the current window's physical baseline");
}

void staleQueuedScaleIsRejectedAfterBaselineChanges() {
    QWidget window;
    window.resize(320, 80);
    window.show();
    QApplication::processEvents();

    adqt::widgets::AdDpiStableWindowController controller(&window);
    require(controller.captureBaseline(), "stale-commit test baseline capture failed");
    int commitCount = 0;
    QObject::connect(&controller, &adqt::widgets::AdDpiStableWindowController::scaleCommitCompleted,
                     [&commitCount]() { ++commitCount; });
    adqt::widgets::AdDpiStableWindowControllerTestAccess::queueScaleCommit(controller);
    require(controller.captureBaseline(), "replacement baseline capture failed");
    adqt::widgets::AdDpiStableWindowControllerTestAccess::commitPendingScale(controller);
    require(commitCount == 0,
            "queued scale commit from an older baseline generation was not rejected");

    adqt::widgets::AdDpiStableWindowControllerTestAccess::queueScaleCommit(controller);
    controller.resetBaseline();
    adqt::widgets::AdDpiStableWindowControllerTestAccess::commitPendingScale(controller);
    require(commitCount == 0, "baseline reset did not discard its queued scale commit");
}

void baselineCaptureIsBlockedDuringNativeTransition() {
    QWidget window;
    window.resize(320, 80);
    window.show();
    QApplication::processEvents();

    adqt::widgets::AdDpiStableWindowController controller(&window);
    require(controller.captureBaseline(1.25), "transition test baseline capture failed");
    const QSize frameSize = controller.stablePhysicalFrameSize();
    const qreal referenceDpr = controller.referenceDpr();
    window.resize(640, 160);
    adqt::widgets::AdDpiStableWindowControllerTestAccess::setNativeTransitionActive(controller,
                                                                                    true);
    require(controller.captureBaseline(2.0),
            "capture during an active transition should preserve an existing baseline");
    require(controller.stablePhysicalFrameSize() == frameSize &&
                qFuzzyCompare(controller.referenceDpr() + 1.0, referenceDpr + 1.0),
            "capture during an active transition replaced the authoritative baseline");
    adqt::widgets::AdDpiStableWindowControllerTestAccess::setNativeTransitionActive(controller,
                                                                                    false);
}

void completionHandlersCanEstablishTheNextFrameBaseline() {
    QWidget window;
    window.resize(240, 80);
    adqt::widgets::AdDpiStableWindowController controller(&window);
    require(controller.captureBaseline(1.0), "completion test baseline capture failed");
    adqt::widgets::AdDpiStableWindowControllerTestAccess::setNativeTransitionActive(controller,
                                                                                    true);
    bool completed = false;
    QObject::connect(&controller, &adqt::widgets::AdDpiStableWindowController::scaleCommitCompleted,
                     [&]() {
                         completed = true;
                         window.resize(480, 100);
                         require(controller.captureBaseline(1.5),
                                 "completion handler must be able to capture its new baseline");
                         require(qFuzzyCompare(controller.referenceDpr(), 1.5),
                                 "completion must release the old native baseline guard");
                     });
    adqt::widgets::AdDpiStableWindowControllerTestAccess::queueScaleCommit(controller);
    adqt::widgets::AdDpiStableWindowControllerTestAccess::commitPendingScale(controller);
    require(completed, "queued scale change must notify its completion handler");
}

void scopesRespectBoundariesAndParticipantLifetimes() {
    QWidget root;
    ParticipantWidget outer(&root);
    QWidget nested(&root);
    ParticipantWidget inner(&nested);
    QWidget popup(&root, Qt::Tool);
    ParticipantWidget popupChild(&popup);
    adqt::widgets::AdControlScaleScope scope(&root);
    adqt::widgets::AdControlScaleScope nestedScope(&nested);
    nestedScope.publishScale(1.25, 1.0);
    scope.publishScale(1.5, 1.0);
    require(outer.commitCount == 1 && inner.commitCount == 1 && popupChild.commitCount == 0,
            "scale publication crossed a nested scope or native popup boundary");
    require(!scope.applyCurrentScaleToSubtree(&inner) &&
                !scope.applyCurrentScaleToSubtree(&popupChild),
            "lazy scaling crossed a scope boundary");
    require(qFuzzyCompare(adqt::widgets::controlScaleContextFor(&popupChild).logicalScale, 1.0),
            "a detached popup inherited its owner's counter-scale");
    auto* victim = new ParticipantWidget(&root);
    QPointer<ParticipantWidget> guardedVictim(victim);
    outer.onPrepare = [&]() { delete guardedVictim.data(); };
    scope.publishScale(2.0, 1.0);
    require(guardedVictim.isNull() && outer.commitCount == 2,
            "deleting a participant during prepare broke the remaining commit");
    outer.onPrepare = {};
    ParticipantWidget late(&root);
    require(scope.applyCurrentScaleToSubtree(&late) &&
                qFuzzyCompare(late.lastCommittedContext.logicalScale, 2.0),
            "a lazy participant did not receive the current context");
}

void referenceMetricsSurviveFractionalRoundTrips() {
    QWidget root;
    adqt::widgets::AdButton button(&root);
    adqt::widgets::AdRadio radio(&root);
    QFont reference;
    reference.setPixelSize(18);
    button.setReferenceFont(reference);
    radio.setReferenceFont(reference);
    button.setReferenceIconSize(QSize(24, 24));
    radio.setReferenceIconSize(QSize(24, 24));
    adqt::widgets::AdControlScaleScope scope(&root);
    for (qreal userScale : {1.0, 0.8}) {
        for (int round = 0; round < 3; ++round) {
            for (qreal dpr : {1.0, 1.25, 1.5, 2.0, 1.5, 1.25, 1.0}) {
                const auto context = adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(
                    1.5, dpr, userScale);
                scope.publishScale(context);
                const QSize expectedIcon =
                    adqt::widgets::scaleControlSize(QSize(24, 24), context.logicalScale);
                require(button.iconSize() == expectedIcon && radio.iconSize() == expectedIcon,
                        "reference icons accumulated scaling during a monitor round trip");
                require(button.font().pixelSize() == qRound(18 * context.logicalScale) &&
                            radio.font().pixelSize() == qRound(18 * context.logicalScale),
                        "reference fonts accumulated scaling during a monitor round trip");
            }
        }
    }
    scope.publishScale(1.0, 2.0);
    reference.setPixelSize(22);
    button.setReferenceFont(reference);
    radio.setReferenceFont(reference);
    button.setReferenceIconSize(QSize(30, 30));
    radio.setReferenceIconSize(QSize(30, 30));
    require(button.font().pixelSize() == 11 && radio.font().pixelSize() == 11 &&
                button.iconSize() == QSize(15, 15) && radio.iconSize() == QSize(15, 15),
            "reference changes at a non-unit scale were not applied immediately");
    require(!scope.publishScale(1.0, 2.0), "equivalent publication unexpectedly changed the scope");
    scope.publishScale(1.0, 1.0);
    require(button.font().pixelSize() == 22 && radio.font().pixelSize() == 22 &&
                button.iconSize() == QSize(30, 30) && radio.iconSize() == QSize(30, 30),
            "reference edits were lost on the next DPI transition");
    adqt::widgets::AdButton late(&root);
    late.setReferenceFont(reference);
    late.setReferenceIconSize(QSize(30, 30));
    scope.applyCurrentScaleToSubtree(&late);
    require(late.font() == button.font() && late.iconSize() == button.iconSize(),
            "a lazy button disagrees with a previously scaled button");
}

void presentationCommitsOnlyTheLatestGeneration() {
    QWidget window;
    window.resize(320, 80);
    adqt::widgets::AdDpiStableWindowController controller(&window);
    int readyCount = 0;
    int completedCount = 0;
    QObject::connect(
        &controller, &adqt::widgets::AdDpiStableWindowController::scaleCommitReady,
        [&](const adqt::widgets::AdDpiStableWindowTransition& transition) {
            ++readyCount;
            require(!window.updatesEnabled(), "ready stage allowed intermediate painting");
            require(transition.windowId == window.winId() &&
                        transition.physicalClientSize == controller.stablePhysicalClientSize(),
                    "ready stage delivered an inconsistent native snapshot");
            if (readyCount == 1)
                controller.requestScaleCommit();
        });
    QObject::connect(
        &controller, &adqt::widgets::AdDpiStableWindowController::scaleCommitCompleted, [&]() {
            ++completedCount;
            require(window.updatesEnabled(), "completion ran before presentation resumed");
        });
    controller.requestScaleCommit();
    adqt::widgets::AdDpiStableWindowControllerTestAccess::commitPendingScale(controller);
    require(completedCount == 0 && !window.updatesEnabled(),
            "a superseded generation completed or resumed presentation");
    adqt::widgets::AdDpiStableWindowControllerTestAccess::commitPendingScale(controller);
    require(readyCount == 2 && completedCount == 1 && window.updatesEnabled(),
            "the latest generation did not finish exactly once");
    require(controller.diagnostics().reconciliationCount == 2 &&
                controller.diagnostics().committedGeneration > 0,
            "transaction diagnostics did not describe the final generation");
}

void resetAndExternalUpdateSuppressionArePreserved() {
    QWidget window;
    window.resize(320, 80);
    adqt::widgets::AdDpiStableWindowController controller(&window);
    controller.requestScaleCommit();
    controller.resetBaseline();
    require(window.updatesEnabled(), "baseline reset left presentation suspended");
    require(controller.captureBaseline(), "replacement baseline was not captured");
    window.setUpdatesEnabled(false);
    controller.requestScaleCommit();
    adqt::widgets::AdDpiStableWindowControllerTestAccess::commitPendingScale(controller);
    require(!window.updatesEnabled(), "DPI commit overrode external update suppression");
    window.setUpdatesEnabled(true);
    const QSize baseline = controller.stablePhysicalFrameSize();
    window.resize(321, 80);
    require(controller.beginPhysicalDrag(QPointF(10, 10)), "could not start physical drag");
    require(controller.stablePhysicalFrameSize() == baseline,
            "drag start replaced the baseline with a rounded observed size");
    controller.endPhysicalDrag();
}

void intentionalExtentPrecedesPlacementAndRecreationInvalidatesCommit() {
    QWidget window;
    window.resize(320, 80);
    adqt::widgets::AdDpiStableWindowController controller(&window);
    require(controller.captureBaseline(1.5, QSize(640, 120)) &&
                controller.stablePhysicalClientSize() == QSize(640, 120),
            "intentional placement did not establish its size before native movement");
    int readyCount = 0;
    int completedCount = 0;
    QObject::connect(&controller, &adqt::widgets::AdDpiStableWindowController::scaleCommitReady,
                     [&](const auto&) {
                         if (++readyCount == 1) {
                             controller.resetBaseline();
                             controller.captureBaseline(1.0, QSize(240, 80));
                             controller.requestScaleCommit();
                         }
                     });
    QObject::connect(&controller, &adqt::widgets::AdDpiStableWindowController::scaleCommitCompleted,
                     [&]() { ++completedCount; });
    controller.requestScaleCommit();
    adqt::widgets::AdDpiStableWindowControllerTestAccess::commitPendingScale(controller);
    require(completedCount == 0, "a retired baseline completed its stale transaction");
    adqt::widgets::AdDpiStableWindowControllerTestAccess::commitPendingScale(controller);
    require(completedCount == 1 && controller.stablePhysicalClientSize() == QSize(240, 80),
            "replacement baseline did not complete its own transaction");
}

void componentHintsFollowTheScope() {
    QWidget root;
    auto* layout = new QHBoxLayout(&root);
    auto* button = new adqt::widgets::AdButton(QStringLiteral("Run"), &root);
    auto* radio = new adqt::widgets::AdRadio(QStringLiteral("Choice"), &root);
    auto* slider = new adqt::widgets::AdSlider(&root);
    auto* select = new adqt::widgets::AdSelect(&root);
    layout->addWidget(button);
    layout->addWidget(radio);
    layout->addWidget(slider);
    layout->addWidget(select);
    const QSize buttonBefore = button->sizeHint();
    const QSize radioBefore = radio->sizeHint();
    const QSize sliderBefore = slider->sizeHint();
    const QSize selectBefore = select->sizeHint();
    adqt::widgets::AdControlScaleScope scope(&root);
    require(scope.publishScale(1.5, 1.0), "component scale commit failed");
    require(button->sizeHint().width() > buttonBefore.width() &&
                radio->sizeHint().width() > radioBefore.width() &&
                slider->sizeHint().width() > sliderBefore.width() &&
                select->sizeHint().width() > selectBefore.width(),
            "one or more migrated component hints ignored the scale context");
    require(button->isEnabled() && radio->isEnabled() && slider->isEnabled() && select->isEnabled(),
            "scale commit changed component enabled state");
}

void radioIconUsesDirectPaintingAfterScaleChanges() {
    QWidget root;
    auto* radio = new adqt::widgets::AdRadio(&root);
    radio->setVariant(adqt::widgets::AdRadio::Variant::Button);
    radio->setIconSize(QSize(16, 16));
    radio->setFixedSize(32, 24);

    const auto trace = std::make_shared<IconPaintTrace>();
    radio->setIcon(QIcon(new DpiTrackingIconEngine(trace)));

    adqt::widgets::AdControlScaleScope scope(&root);
    require(scope.publishScale(1.5, 1.0), "radio scale-up commit failed");
    renderWidget(radio);
    require(scope.publishScale(1.0, 1.0), "radio scale-down commit failed");
    renderWidget(radio);

    require(trace->painterDprs.size() == 2,
            "radio icons did not paint through the current painter after scale changes");
    require(trace->pixmapRequestCount == 0,
            "radio icons fell back to cached pixmap scaling instead of direct painting");
}

void buttonExplicitIconSizeSurvivesDpiScale() {
    QWidget root;
    auto* button = new adqt::widgets::AdButton(&root);
    button->setIconSize(QSize(24, 24));
    button->setFixedSize(32, 32);

    const auto trace = std::make_shared<IconPaintTrace>();
    button->setIcon(QIcon(new DpiTrackingIconEngine(trace)));

    adqt::widgets::AdControlScaleScope scope(&root);
    require(scope.publishScale(1.5, 1.0), "button reference scale commit failed");
    require(scope.publishScale(1.0, 1.0), "button baseline scale commit failed");
    trace->pixmapSizes.clear();
    renderWidget(button);

    button->setFixedSize(22, 21);
    require(scope.publishScale(1.0, 1.5), "button destination scale commit failed");
    renderWidget(button);

    const qreal renderDpr = button->devicePixelRatioF();
    const QSize sourceRequest(qRound(24 * renderDpr), qRound(24 * renderDpr));
    const QSize destinationRequest(qRound(16 * renderDpr), qRound(16 * renderDpr));
    require(trace->pixmapSizes.size() == 2 && trace->pixmapSizes.at(0) == sourceRequest &&
                trace->pixmapSizes.at(1) == destinationRequest,
            "an explicitly sized button icon was reclassified as the default after DPI scaling");
}

void radioButtonContentInsetsFollowDpiScale() {
    QWidget root;
    auto* radio = new adqt::widgets::AdRadio(&root);
    radio->setVariant(adqt::widgets::AdRadio::Variant::Button);
    radio->setIconSize(QSize(16, 16));
    radio->setFixedSize(32, 26);

    const auto trace = std::make_shared<IconPaintTrace>();
    radio->setIcon(QIcon(new DpiTrackingIconEngine(trace)));

    adqt::widgets::AdControlScaleScope scope(&root);
    require(scope.publishScale(1.5, 1.0), "radio reference scale commit failed");
    require(scope.publishScale(1.0, 1.0), "radio baseline scale commit failed");
    trace->paintRects.clear();
    renderWidget(radio);

    radio->setFixedSize(21, 17);
    require(scope.publishScale(1.0, 1.5), "radio destination scale commit failed");
    renderWidget(radio);

    require(trace->paintRects.size() == 2, "radio scale test did not paint both icon states");
    const QRect sourceRect = trace->paintRects.at(0);
    const QRect destinationRect = trace->paintRects.at(1);
    const QRect destinationPhysicalRect(
        qRound(destinationRect.x() * 1.5), qRound(destinationRect.y() * 1.5),
        qRound(destinationRect.width() * 1.5), qRound(destinationRect.height() * 1.5));
    require(qAbs(destinationPhysicalRect.x() - sourceRect.x()) <= 1 &&
                qAbs(destinationPhysicalRect.y() - sourceRect.y()) <= 1 &&
                qAbs(destinationPhysicalRect.width() - sourceRect.width()) <= 1 &&
                qAbs(destinationPhysicalRect.height() - sourceRect.height()) <= 1,
            "radio button padding moved or resized its icon after DPI scaling");
}

namespace {

constexpr adqt::icons::IconDescriptor kDpiTestEntries[] = {{
    std::string_view("dpi-test"),
    std::string_view("outlined"),
    std::string_view("square"),
    std::string_view(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\">"
        "<rect x=\"1\" y=\"1\" width=\"14\" height=\"14\" fill=\"__ADQT_SLOT_PRIMARY__\"/>"
        "</svg>"),
    std::string_view("dpi-test-square"),
    adqt::icons::IconColorModel::Monochrome,
    adqt::icons::IconFit::Contain,
    adqt::icons::IconStaticColors{},
    false,
}};

constexpr adqt::icons::IconPack kDpiTestPack{
    std::string_view("dpi-test"),
    std::string_view("DPI stability tests"),
    std::string_view("dpi-test-pack"),
    kDpiTestEntries,
    sizeof(kDpiTestEntries) / sizeof(kDpiTestEntries[0]),
};

} // namespace

adqt::icons::IconRef testIconRef() {
    const auto ref = kDpiTestPack.icon(0);
    require(ref.isValid(), "test icon lookup failed");
    return ref;
}

adqt::icons::IconRenderRequest iconRequest(const QSize& size, qreal dpr,
                                           QIcon::Mode mode = QIcon::Normal,
                                           QIcon::State state = QIcon::Off) {
    adqt::icons::IconRenderRequest request;
    request.logicalSize = size;
    request.devicePixelRatio = dpr;
    request.mode = mode;
    request.state = state;
    return request;
}

void iconCacheSharesPhysicalRasters() {
    adqt::icons::IconRenderer registry;
    const auto ref = testIconRef();
    const QPixmap first = registry.renderIconPixmap(ref, iconRequest(QSize(16, 16), 1.5));
    const QPixmap second = registry.renderIconPixmap(ref, iconRequest(QSize(24, 24), 1.0));
    require(!first.isNull() && !second.isNull(), "icon rasterization failed");
    require(qFuzzyCompare(first.devicePixelRatio(), 1.5) &&
                qFuzzyCompare(second.devicePixelRatio(), 1.0),
            "caller DPR metadata was not preserved");
    const adqt::icons::IconCacheStatistics stats = registry.cacheStatistics();
    require(stats.entryCount == 1 && stats.rasterizationCount == 1 && stats.hitCount >= 1,
            "equivalent physical icon requests did not share one raster");
    require(stats.costKB == 3, "icon cache cost should equal the actual 24x24x4 raster bytes");
}

void iconCacheSeparatesVisualKeys() {
    adqt::icons::IconRenderer registry;
    const auto ref = testIconRef();

    registry.renderIconPixmap(ref, iconRequest(QSize(16, 16), 1.0));
    registry.renderIconPixmap(ref, iconRequest(QSize(16, 16), 1.0, QIcon::Normal, QIcon::On));
    registry.renderIconPixmap(ref, iconRequest(QSize(16, 16), 1.0, QIcon::Disabled));
    const auto coloredRef = ref.withColors(adqt::icons::IconColors::primary(QColor(Qt::red)));
    registry.renderIconPixmap(coloredRef, iconRequest(QSize(16, 16), 1.0));

    auto stats = registry.cacheStatistics();
    require(stats.entryCount == 4 && stats.rasterizationCount == 4,
            "mode, state, and color requests must have distinct cache entries");

    registry.setPaletteResolver([]() {
        adqt::icons::IconPalette palette;
        palette.text = QColor(Qt::green);
        palette.revision = 42;
        return palette;
    });
    registry.renderIconPixmap(ref, iconRequest(QSize(16, 16), 1.0));
    stats = registry.cacheStatistics();
    require(stats.entryCount == 1 && stats.rasterizationCount == 5,
            "palette changes must invalidate and separate cached rasters");
}

void concurrentIconMissesRasterizeOnce() {
    adqt::icons::IconRenderer registry;
    const auto ref = testIconRef();

    constexpr int threadCount = 8;
    std::atomic_int ready{0};
    std::atomic_bool start{false};
    std::vector<QPixmap> results(threadCount);
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (int index = 0; index < threadCount; ++index) {
        threads.emplace_back([&, index]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[index] = registry.renderIconPixmap(ref, iconRequest(QSize(32, 32), 1.0));
        });
    }
    while (ready.load(std::memory_order_acquire) != threadCount) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    for (const QPixmap& result : results) {
        require(!result.isNull(), "a concurrent icon request returned no raster");
    }
    const auto stats = registry.cacheStatistics();
    require(stats.entryCount == 1 && stats.missCount == 1 && stats.rasterizationCount == 1 &&
                stats.hitCount == threadCount - 1,
            "concurrent equivalent misses did not rendezvous on one raster");
}

void floatingSurfaceRendersTransparentShadowMargins() {
    adqt::widgets::AdFloatingSurface surface;
    surface.setShadow(18.0, QPointF(0.0, 3.0), QColor(0, 0, 0, 90));
    surface.setCornerRadius(8.0);
    surface.resize(160, 80);
    surface.show();
    QApplication::processEvents();
    QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    surface.render(&painter);
    painter.end();
    require(image.pixelColor(0, 0).alpha() == 0,
            "floating surface corner margin should remain transparent");
    require(image.pixelColor(surface.bodyRect().center()).alpha() == 255,
            "floating surface body did not render opaquely");
    const QRect body = surface.bodyRect();
    const QPoint topSampleX(body.center().x(), body.top() - 1);
    const QPoint bottomSampleX(body.center().x(), body.bottom() + 1);
    const QPoint leftSampleY(body.left() - 1, body.center().y());
    const QPoint rightSampleY(body.right() + 1, body.center().y());
    require(
        image.pixelColor(leftSampleY).alpha() > 0 && image.pixelColor(rightSampleY).alpha() > 0 &&
            image.pixelColor(topSampleX).alpha() > 0 && image.pixelColor(bottomSampleX).alpha() > 0,
        "floating surface shadow should render throughout the reserved margins");
    require(image.pixelColor(leftSampleY).alpha() < 255 &&
                image.pixelColor(rightSampleY).alpha() < 255 &&
                image.pixelColor(topSampleX).alpha() < 255 &&
                image.pixelColor(bottomSampleX).alpha() < 255,
            "floating surface shadow margin should remain translucent");
    require(surface.interactiveRegion().contains(surface.bodyRect().center()) &&
                !surface.interactiveRegion().contains(QPoint(0, 0)),
            "floating surface interactive region includes shadow-only pixels");
}

void floatingSurfaceShadowCacheFollowsVisibilityAndStaysComponentLocal() {
    adqt::widgets::AdFloatingSurface first;
    adqt::widgets::AdFloatingSurface second;
    first.resize(160, 80);
    second.resize(160, 80);
    require(!adqt::widgets::AdFloatingSurfaceTestAccess::hasShadowCache(first),
            "hidden floating surface should not allocate a shadow cache");
    first.show();
    second.show();
    QApplication::processEvents();

    renderWidget(&first);
    renderWidget(&second);
    require(adqt::widgets::AdFloatingSurfaceTestAccess::hasShadowCache(first),
            "visible floating surface should own a shadow cache");
    require(adqt::widgets::AdFloatingSurfaceTestAccess::hasShadowCache(second),
            "second visible floating surface should own a shadow cache");

    first.hide();
    QApplication::processEvents();
    renderWidget(&first);
    require(!adqt::widgets::AdFloatingSurfaceTestAccess::hasShadowCache(first),
            "hidden floating surface should release its shadow cache");
    require(adqt::widgets::AdFloatingSurfaceTestAccess::hasShadowCache(second),
            "hiding one floating surface should preserve the other cache");

    first.show();
    QApplication::processEvents();
    renderWidget(&first);
    require(adqt::widgets::AdFloatingSurfaceTestAccess::hasShadowCache(first),
            "reopened floating surface should rebuild its shadow cache");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        cumulativeEdgesAreStable();
        scopeIsBatchedAndNoOpsRepeatRequests();
        replacementParticipantsCommitBeforePresentation();
        contentScaleComposesWithDpiAndParticipatesInEquivalence();
        currentScaleCanBeAppliedToANewSubtree();
        controllerCoalescesToTheLatestPendingScale();
        stableNativeTopLeftIsAFixedPointOfQtsLogicalGrid();
        controllerCanKeepReferenceDpiSeparateFromWindowDpi();
        staleQueuedScaleIsRejectedAfterBaselineChanges();
        baselineCaptureIsBlockedDuringNativeTransition();
        completionHandlersCanEstablishTheNextFrameBaseline();
        scopesRespectBoundariesAndParticipantLifetimes();
        referenceMetricsSurviveFractionalRoundTrips();
        presentationCommitsOnlyTheLatestGeneration();
        resetAndExternalUpdateSuppressionArePreserved();
        intentionalExtentPrecedesPlacementAndRecreationInvalidatesCommit();
        componentHintsFollowTheScope();
        radioIconUsesDirectPaintingAfterScaleChanges();
        buttonExplicitIconSizeSurvivesDpiScale();
        radioButtonContentInsetsFollowDpiScale();
        iconCacheSharesPhysicalRasters();
        iconCacheSeparatesVisualKeys();
        concurrentIconMissesRasterizeOnce();
        floatingSurfaceRendersTransparentShadowMargins();
        floatingSurfaceShadowCacheFollowsVisibilityAndStaysComponentLocal();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
