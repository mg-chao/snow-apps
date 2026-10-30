#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshotselectiontoolbarwidget.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include "snow_shot/platform/screenshotnative.h"
#ifdef Q_OS_MACOS
#import <AppKit/AppKit.h>
#endif
#include "snow_shot/presentation/screenshotcolorpickerwindow.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"
#include "snow_shot/presentation/screenshotshortcutexitconfirmation.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/platform/physicalcursor.h"
#include "snow_shot/presentation/screenshotoverlayuihost.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaycanvaspresenter.h"
#include "physical_key_test_support.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/tooltip.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/modal.h"
#include "widgets/popup_interaction_host.h"
#include "widgets/detail/overlay_popup_surface.h"

#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QClipboard>
#include <QBackingStore>
#include <QCursor>
#include <QDir>
#include <QEnterEvent>
#include <QHoverEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QTemporaryDir>
#include <QTranslator>
#include <QLabel>
#include <QWindow>
#include <QTimerEvent>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void canvasSamplingSessionReleasesTargetsAndCommitsAfterCleanup() {
    QWidget owner;
    ScreenshotCanvasColorSamplingSession session(owner);
    auto first = std::make_unique<adqt::widgets::AdColorPicker>();
    auto second = std::make_unique<adqt::widgets::AdColorPicker>();
    int cancellations = 0;
    const auto cancel = [&]() {
        ++cancellations;
        session.end();
    };
    session.begin(first.get(), cancel);
    require(session.active(), "sampling begins with a live picker");
    session.begin(second.get(), cancel);
    first.reset();
    require(cancellations == 0 && session.active(),
            "replacing the picker disconnects its old destruction callback");
    second.reset();
    require(cancellations == 1 && !session.active(),
            "picker destruction cancels exactly the current sampling session");

    auto picker = std::make_unique<adqt::widgets::AdColorPicker>();
    session.begin(picker.get(), cancel);
    QImage preview(3, 3, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::black);
    const QColor color(12, 34, 56, 128);
    preview.setPixelColor(1, 1, color);
    bool cleaned = false;
    require(session.commit(preview,
                           [&]() {
                               require(!session.active(),
                                       "picker ownership is released before host cleanup");
                               cleaned = true;
                           }) &&
                cleaned && picker->value().solidColor == preview.pixelColor(1, 1),
            "commit applies the center pixel including alpha after host cleanup");
    session.begin(picker.get(), cancel);
    require(!session.commit({}, []() {}) && !session.active(),
            "invalid samples end the session without committing a value");
    session.begin(picker.get(), cancel);
    require(!session.commit(preview, [&]() { picker.reset(); }) && cancellations == 1,
            "cleanup may destroy the picker without committing or firing a stale cancellation");
}

const uchar* backingPixels(ScreenshotColorPickerWindow& picker) {
    QBackingStore* store = picker.backingStore();
    require(store != nullptr && store->size() == picker.size(),
            "hidden preparation must allocate the full logical backing-store size");
    store->beginPaint(picker.rect());
    QPaintDevice* device = store->paintDevice();
    require(device != nullptr && device->devType() == QInternal::Image,
            "the raster backend must expose an image paint device");
    const auto* image = static_cast<const QImage*>(device);
    require(!image->isNull() && image->size() == picker.size() * picker.devicePixelRatioF(),
            "preparation must allocate backing pixels at the window device pixel ratio");
    const uchar* pixels = image->constBits();
    store->endPaint();
    return pixels;
}

class NoopOverlayEventSink final : public ScreenshotOverlayEventSink {
  public:
    ScreenshotOverlayRightClickResult rightClickResult = ScreenshotOverlayRightClickResult::Ignored;
    std::function<void()> cancel = [] {};
    void completeRightClickCancellation() override {
        cancel();
    }
    bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow*, const QPointF&,
                                       bool) const override {
        return false;
    }

    void handleOverlayMousePress(ScreenshotOverlayWindow*, const QPointF&) override {}

    void handleOverlayMouseMove(ScreenshotOverlayWindow*, const QPointF&) override {}

    void handleOverlayMouseRelease(ScreenshotOverlayWindow*, const QPointF&) override {}

    ScreenshotOverlayRightClickResult handleOverlayRightClick(ScreenshotOverlayWindow*,
                                                              const QPointF&) override {
        return rightClickResult;
    }

    bool handleOverlayWheel(ScreenshotOverlayWindow*, const QWheelEvent&) override {
        return false;
    }

    bool shouldBlockUnhandledOverlayKeyInput() const override {
        return false;
    }

    void raiseToolbarForCanvasInteraction() override {}
};

class OverlayRevealFixture final {
  public:
    OverlayRevealFixture()
        : primary(sink, new SnowCanvasWidget(runtime)),
          secondary(sink, new SnowCanvasWidget(runtime)), coordinator(sink, runtime, shortcuts) {
        primary.setCaptureGeometry(QRect(0, 0, 160, 100));
        secondary.setCaptureGeometry(QRect(160, 0, 160, 100));
        for (auto* overlay : {&primary, &secondary}) {
            CapturedDisplayModel display;
            display.logicalRect = overlay->captureGeometry();
            display.physicalRect = display.logicalRect;
            display.screen = QGuiApplication::primaryScreen();
            display.active = true;
            display.geometryResolved = true;
            display.logicalToPhysicalScale = 1.0;
            displays.appendDisplay(display, overlay);
        }
        displays.startup = std::make_shared<ScreenshotStartupContext>();
        displays.startup->phase = ScreenshotStartupContext::Phase::Revealed;
        displays.startup->logicalPosition = QPoint(10, 10);
    }

    NoopOverlayEventSink sink;
    SnowCanvasRuntime runtime;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow primary;
    ScreenshotOverlayWindow secondary;
    ScreenshotDisplaySession displays;
    ScreenshotOverlayCoordinator coordinator;
};

void retiredSecondaryRevealCannotShowThePooledOverlay() {
    for (int retirement = 0; retirement < 4; ++retirement) {
        OverlayRevealFixture fixture;
        fixture.coordinator.showOverlayWindows(fixture.displays,
                                               ScreenshotOverlayShowMode::CapturedImage);
        require(fixture.primary.isVisible() && fixture.secondary.isHidden(),
                "captured reveal must defer the secondary monitor until Qt dispatches it");
        switch (retirement) {
        case 0:
            fixture.coordinator.hideOverlayWindowsImmediately(fixture.displays);
            break;
        case 1:
            // A pending secondary reveal is hidden already, so no Hide event is emitted.
            fixture.secondary.hide();
            break;
        case 2:
            fixture.secondary.resetScreenshotRendering();
            break;
        case 3:
            fixture.secondary.releaseNativeSurface();
            break;
        }
        QCoreApplication::sendPostedEvents(&fixture.secondary, QEvent::MetaCall);
        require(fixture.secondary.isHidden(),
                "retiring a pooled overlay must cancel its queued secondary reveal");
    }
}

void cancellationDuringFirstShowRetiresTheWholeRevealGroup() {
    for (const auto mode : {ScreenshotOverlayShowMode::CapturedImage,
                            ScreenshotOverlayShowMode::CapturedImageFramePaced,
                            ScreenshotOverlayShowMode::PreparedPreview}) {
        OverlayRevealFixture fixture;
        class CancelOnShow final : public QObject {
          public:
            std::function<void()> cancel;
            bool canceled = false;

          protected:
            bool eventFilter(QObject*, QEvent* event) override {
                if (!canceled && event->type() == QEvent::Show) {
                    canceled = true;
                    cancel();
                }
                return false;
            }
        } cancellation;
        cancellation.cancel = [&] {
            fixture.coordinator.hideOverlayWindowsImmediately(fixture.displays);
        };
        fixture.primary.installEventFilter(&cancellation);
        fixture.coordinator.showOverlayWindows(fixture.displays, mode);
        require(cancellation.canceled, "the preferred monitor must trigger capture cancellation");
        QCoreApplication::sendPostedEvents(&fixture.secondary, QEvent::MetaCall);
        require(
            fixture.secondary.isHidden(),
            "cancellation during first show must retire secondary reveals before they are queued");
    }
}

void reentrantPublicShowOwnsANewPresentation() {
    for (const auto mode : {ScreenshotOverlayShowMode::CapturedImage,
                            ScreenshotOverlayShowMode::CapturedImageFramePaced,
                            ScreenshotOverlayShowMode::PreparedPreview}) {
        OverlayRevealFixture fixture;
        QWidget focusOwner(&fixture.primary);
        focusOwner.setFocusPolicy(Qt::StrongFocus);
        focusOwner.show();
        class ReplaceOnShow final : public QObject {
          public:
            std::function<void()> replace;
            bool replaced = false;

          protected:
            bool eventFilter(QObject*, QEvent* event) override {
                if (!replaced && event->type() == QEvent::Show) {
                    replaced = true;
                    replace();
                }
                return false;
            }
        } replacement;
        replacement.replace = [&] {
            fixture.primary.hide();
            fixture.primary.show();
            focusOwner.setFocus(Qt::OtherFocusReason);
        };
        fixture.primary.installEventFilter(&replacement);
        fixture.coordinator.showOverlayWindows(fixture.displays, mode);
        require(
            replacement.replaced && fixture.primary.isVisible() &&
                fixture.primary.isPresentationCurrent(fixture.primary.presentationGeneration()) &&
                fixture.primary.focusWidget() == &focusOwner,
            "a reentrant public show must establish its own visible presentation and focus owner");
        for (int pass = 0; pass < 3; ++pass)
            QCoreApplication::sendPostedEvents(&fixture.primary, QEvent::MetaCall);
        require(fixture.primary.focusWidget() == &focusOwner,
                "activation from the superseded prepared show must not focus its replacement");
    }
}

void supersededSecondaryRevealCannotFocusTheNextPresentation() {
    OverlayRevealFixture fixture;
    fixture.coordinator.showOverlayWindows(fixture.displays,
                                           ScreenshotOverlayShowMode::CapturedImage);
    fixture.secondary.hide();
    fixture.secondary.show();
    QWidget focusOwner(&fixture.secondary);
    focusOwner.setFocusPolicy(Qt::StrongFocus);
    focusOwner.show();
    focusOwner.setFocus(Qt::OtherFocusReason);
    require(fixture.secondary.focusWidget() == &focusOwner,
            "the new presentation must own keyboard focus before dispatch");
    // Dispatch the original secondary reveal after its presentation has been replaced.
    for (int pass = 0; pass < 3; ++pass)
        QCoreApplication::sendPostedEvents(&fixture.secondary, QEvent::MetaCall);
    require(fixture.secondary.focusWidget() == &focusOwner,
            "a retired secondary reveal must not activate the reused overlay's next presentation");
}

QSet<QObject*> registeredTimerOwners() {
    QSet<QObject*> owners;
    auto* dispatcher = QAbstractEventDispatcher::instance();
    for (QObject* child : dispatcher->children()) {
        if (!dispatcher->registeredTimers(child).isEmpty())
            owners.insert(child);
    }
    return owners;
}

void delayedActivationCannotFocusTheNextPresentation() {
    NoopOverlayEventSink sink;
    SnowCanvasRuntime runtime;
    ScreenshotOverlayWindow overlay(sink, new SnowCanvasWidget(runtime));
    overlay.setCaptureGeometry(QRect(0, 0, 160, 100));
    ScreenshotDisplaySession displays;
    CapturedDisplayModel display;
    display.logicalRect = overlay.captureGeometry();
    display.active = true;
    displays.appendDisplay(display, &overlay);
    ScreenshotOverlayCanvasPresenter presenter({});
    const QSet<QObject*> timersBefore = registeredTimerOwners();
    presenter.showOverlayWindows(displays, ScreenshotOverlayShowMode::CapturedImageFramePaced);
    const QSet<QObject*> activationTimers = registeredTimerOwners() - timersBefore;
    require(activationTimers.size() == 1,
            "frame-paced reveal must schedule one delayed activation on the Qt dispatcher");
    QPointer<QObject> activationTimer(*activationTimers.constBegin());
    const auto timers = QAbstractEventDispatcher::instance()->registeredTimers(activationTimer);
    require(timers.size() == 1, "the activation callback must own one Qt timer registration");

    overlay.hide();
    overlay.show();
    QWidget focusOwner(&overlay);
    focusOwner.setFocusPolicy(Qt::StrongFocus);
    focusOwner.show();
    focusOwner.setFocus(Qt::OtherFocusReason);
    require(overlay.focusWidget() == &focusOwner,
            "the reused overlay must begin with its new keyboard owner");
    // Deliver the real timer event deterministically, without a wall-clock sleep.
    QTimerEvent timeout(timers.front().timerId);
    QCoreApplication::sendEvent(activationTimer, &timeout);
    require(activationTimer.isNull(), "the delayed one-shot activation must retire after delivery");
    require(overlay.focusWidget() == &focusOwner,
            "old delayed activation must not move focus after the pooled overlay is shown again");
}

void dispatchOverlayRevealCallbacks(OverlayRevealFixture& fixture) {
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents(&fixture.primary, QEvent::MetaCall);
        QCoreApplication::sendPostedEvents(&fixture.secondary, QEvent::MetaCall);
    }
}

void deliverActivationTimer(QPointer<QObject>& timer) {
    require(!timer.isNull(), "the pending activation timer must still exist before delivery");
    const auto registrations = QAbstractEventDispatcher::instance()->registeredTimers(timer);
    require(registrations.size() == 1,
            "each delayed activation callback must own one Qt timer registration");
    QTimerEvent timeout(registrations.front().timerId);
    QCoreApplication::sendEvent(timer, &timeout);
    require(timer.isNull(), "the delayed one-shot activation must retire after delivery");
}

void preferredDisplayOwnsTheRevealGroupKeyboardFocus() {
    class CursorPositionRestorer final {
      public:
        ~CursorPositionRestorer() {
            QCursor::setPos(position);
        }

        QPoint position = QCursor::pos();
    } restoreCursor;
    for (const auto mode : {ScreenshotOverlayShowMode::CapturedImage,
                            ScreenshotOverlayShowMode::CapturedImageFramePaced,
                            ScreenshotOverlayShowMode::PreparedPreview}) {
        for (int preferredDisplay = 0; preferredDisplay < 4; ++preferredDisplay) {
            OverlayRevealFixture fixture;
            const bool prefersSecondary = preferredDisplay == 1 || preferredDisplay == 2;
            ScreenshotOverlayWindow& preferred =
                prefersSecondary ? fixture.secondary : fixture.primary;
            ScreenshotOverlayWindow& other = prefersSecondary ? fixture.primary : fixture.secondary;
            QWidget preferredPreviousFocus(&preferred);
            QWidget otherFocusOwner(&other);
            for (QWidget* widget : {&preferredPreviousFocus, &otherFocusOwner}) {
                widget->setFocusPolicy(Qt::StrongFocus);
                widget->show();
                widget->setFocus(Qt::OtherFocusReason);
            }

            QCursor::setPos(fixture.primary.captureGeometry().center());
            if (preferredDisplay == 1) {
                fixture.displays.startup->logicalPosition =
                    fixture.secondary.captureGeometry().center();
            } else if (preferredDisplay == 2) {
                fixture.displays.startup->resumeLiveInput();
                QCursor::setPos(fixture.secondary.captureGeometry().center());
                require(
                    fixture.displays.logicalCursorPosition() ==
                        fixture.secondary.captureGeometry().center(),
                    "live input must select the current display rather than the startup anchor");
            } else if (preferredDisplay == 3) {
                fixture.displays.startup->logicalPosition = QPoint(1000, 1000);
            }

            const QSet<QObject*> timersBefore = registeredTimerOwners();
            fixture.coordinator.showOverlayWindows(fixture.displays, mode);
            require(preferred.isVisible(), "the preferred display must reveal immediately");
            if (mode != ScreenshotOverlayShowMode::PreparedPreview) {
                require(other.isHidden(), "other captured displays must retain deferred reveal");
            }
            dispatchOverlayRevealCallbacks(fixture);
            require(preferred.isVisible() && other.isVisible(),
                    "all current overlays must reveal even though only one owns keyboard focus");

            if (mode == ScreenshotOverlayShowMode::CapturedImageFramePaced) {
                const QSet<QObject*> activationTimers = registeredTimerOwners() - timersBefore;
                require(activationTimers.size() == 1,
                        "a frame-paced multi-display reveal must schedule exactly one activation");
                require(preferred.focusWidget() == &preferredPreviousFocus &&
                            other.focusWidget() == &otherFocusOwner,
                        "frame-paced reveal must preserve focus until the one delayed activation");
                QPointer<QObject> activationTimer(*activationTimers.constBegin());
                deliverActivationTimer(activationTimer);
            }
            require(
                preferred.focusWidget() == preferred.canvas(),
                "only the preferred or current display must focus its canvas for keyboard input");
            require(other.focusWidget() == &otherFocusOwner,
                    "revealing another display must preserve its existing focus owner");
            dispatchOverlayRevealCallbacks(fixture);
            require(preferred.focusWidget() == preferred.canvas() &&
                        other.focusWidget() == &otherFocusOwner,
                    "secondary reveal callbacks must not negotiate another canvas focus");
        }
    }
}

void secondaryRevealPreservesPreferredHeldKeyboardShortcuts() {
    using ShortcutManager = snow_shot::presentation::WindowShortcutManager;
    struct HeldShortcut {
        bool active = false;
        int releases = 0;
        int cancels = 0;
    };
    for (const auto mode : {ScreenshotOverlayShowMode::CapturedImage,
                            ScreenshotOverlayShowMode::CapturedImageFramePaced}) {
        HeldShortcut shift;
        HeldShortcut space;
        int escapes = 0;
        OverlayRevealFixture fixture;
        fixture.shortcuts.addScopeWindow(&fixture.primary);
        fixture.shortcuts.addScopeWindow(&fixture.secondary);
        const auto addHeldBinding = [&](const QString& id, QKeyCombination key,
                                        Qt::KeyboardModifiers additional, HeldShortcut& held) {
            ShortcutManager::Binding binding;
            binding.id = id;
            binding.keyCombinations = {key};
            binding.allowedAdditionalModifiers = additional;
            binding.canActivate = [&](const auto& context) {
                return context.receiver == fixture.primary.canvas() &&
                       context.scopeWindow == &fixture.primary;
            };
            binding.activate = [&held](const auto&) {
                held.active = true;
                return true;
            };
            binding.release = [&held](const auto&) {
                held.active = false;
                ++held.releases;
                return true;
            };
            binding.cancel = [&held] {
                held.active = false;
                ++held.cancels;
            };
            require(fixture.shortcuts.addBinding(&fixture.primary, std::move(binding)) != 0,
                    "preferred-canvas held shortcut must register");
        };
        addHeldBinding(QStringLiteral("reveal-held-shift"),
                       QKeyCombination(Qt::ShiftModifier, Qt::Key_Shift), Qt::NoModifier, shift);
        addHeldBinding(QStringLiteral("reveal-held-space"),
                       QKeyCombination(Qt::NoModifier, Qt::Key_Space), Qt::ShiftModifier, space);
        ShortcutManager::Binding escape;
        escape.id = QStringLiteral("reveal-escape");
        escape.keyCombinations = {QKeyCombination(Qt::NoModifier, Qt::Key_Escape)};
        escape.activate = [&](const auto& context) {
            require(context.scopeWindow == &fixture.primary,
                    "Escape must remain in the preferred display's keyboard scope");
            ++escapes;
            return true;
        };
        require(fixture.shortcuts.addBinding(&fixture.primary, std::move(escape)) != 0,
                "preferred-canvas Escape shortcut must register");
        const auto sendKey = [&](QEvent::Type type, Qt::Key key,
                                 Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            PhysicalKeyEvent event(type, key, modifiers);
            event.setAccepted(false);
            QCoreApplication::sendEvent(fixture.primary.focusWidget(), &event);
            return event.isAccepted();
        };

        const QSet<QObject*> timersBefore = registeredTimerOwners();
        fixture.coordinator.showOverlayWindows(fixture.displays, mode);
        if (mode == ScreenshotOverlayShowMode::CapturedImageFramePaced) {
            const QSet<QObject*> activationTimers = registeredTimerOwners() - timersBefore;
            require(activationTimers.size() == 1,
                    "held keyboard setup must have exactly one activation timer");
            QPointer<QObject> activationTimer(*activationTimers.constBegin());
            deliverActivationTimer(activationTimer);
        } else {
            QCoreApplication::sendPostedEvents(&fixture.primary, QEvent::MetaCall);
        }
        require(fixture.primary.focusWidget() == fixture.primary.canvas() &&
                    fixture.secondary.isHidden(),
                "the preferred canvas must own keyboard input before secondary reveal");
        require(sendKey(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier) &&
                    sendKey(QEvent::KeyPress, Qt::Key_Space, Qt::ShiftModifier) && shift.active &&
                    space.active,
                "Shift and Space must arm held controls on the preferred canvas");
        dispatchOverlayRevealCallbacks(fixture);
        require(fixture.secondary.isVisible() && shift.active && space.active &&
                    shift.cancels == 0 && space.cancels == 0,
                "secondary reveal must preserve held keyboard controls without cancellation");
        require(sendKey(QEvent::KeyRelease, Qt::Key_Space, Qt::ShiftModifier) &&
                    sendKey(QEvent::KeyRelease, Qt::Key_Shift) && !shift.active && !space.active &&
                    shift.releases == 1 && space.releases == 1,
                "held controls must release once through the preferred canvas after reveal");
        require(sendKey(QEvent::KeyPress, Qt::Key_Escape) && escapes == 1,
                "Escape must remain reachable after all displays reveal");
        sendKey(QEvent::KeyRelease, Qt::Key_Escape);
    }
}

void supersededGroupActivationCannotFocusTheReopenedDisplay() {
    for (const auto mode : {ScreenshotOverlayShowMode::CapturedImage,
                            ScreenshotOverlayShowMode::CapturedImageFramePaced}) {
        OverlayRevealFixture fixture;
        QWidget primaryFocusOwner(&fixture.primary);
        QWidget secondaryFocusOwner(&fixture.secondary);
        for (QWidget* widget : {&primaryFocusOwner, &secondaryFocusOwner}) {
            widget->setFocusPolicy(Qt::StrongFocus);
            widget->show();
            widget->setFocus(Qt::OtherFocusReason);
        }
        fixture.displays.startup->logicalPosition = fixture.secondary.captureGeometry().center();
        const QSet<QObject*> timersBefore = registeredTimerOwners();
        fixture.coordinator.showOverlayWindows(fixture.displays, mode);
        const QSet<QObject*> oldActivationTimers = registeredTimerOwners() - timersBefore;
        require(mode != ScreenshotOverlayShowMode::CapturedImageFramePaced ||
                    oldActivationTimers.size() == 1,
                "the retired reveal group must have one delayed activation owner");
        fixture.coordinator.hideOverlayWindowsImmediately(fixture.displays);
        fixture.displays.startup->logicalPosition = fixture.primary.captureGeometry().center();
        fixture.coordinator.showOverlayWindows(fixture.displays, mode);
        const QSet<QObject*> newActivationTimers =
            registeredTimerOwners() - timersBefore - oldActivationTimers;
        // Deliver the old preferred overlay's queued work before the new owner's activation.
        QCoreApplication::sendPostedEvents(&fixture.secondary, QEvent::MetaCall);
        if (mode == ScreenshotOverlayShowMode::CapturedImageFramePaced) {
            QPointer<QObject> oldActivationTimer(*oldActivationTimers.constBegin());
            deliverActivationTimer(oldActivationTimer);
        }
        require(fixture.primary.focusWidget() == &primaryFocusOwner &&
                    fixture.secondary.focusWidget() == &secondaryFocusOwner,
                "a canceled group's activation must not focus either reopened presentation");

        dispatchOverlayRevealCallbacks(fixture);
        if (mode == ScreenshotOverlayShowMode::CapturedImageFramePaced) {
            require(newActivationTimers.size() == 1,
                    "the reopened reveal group must schedule one new delayed activation");
            QPointer<QObject> newActivationTimer(*newActivationTimers.constBegin());
            deliverActivationTimer(newActivationTimer);
        }
        require(fixture.primary.isVisible() && fixture.secondary.isVisible() &&
                    fixture.primary.focusWidget() == fixture.primary.canvas() &&
                    fixture.secondary.focusWidget() == &secondaryFocusOwner,
                "the reopened group must activate only its new preferred display");
    }
}

void terminalHideRetiresPresentationBeforeHidingAndAllowsReuse() {
    for (const bool deferred : {false, true}) {
        OverlayRevealFixture fixture;
        fixture.primary.show();
        fixture.primary.canvas()->setInteractionEnabled(true);
        const auto generation = fixture.primary.presentationGeneration();
        const auto nativeId = fixture.primary.internalWinId();
        require(nativeId != 0, "a visible overlay must have a native surface");

        class InspectHide final : public QObject {
          public:
            ScreenshotOverlayWindow* overlay = nullptr;
            quint64 generation = 0;
            bool inspected = false;

          protected:
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::Hide) {
                    inspected = true;
                    require(!overlay->isPresentationCurrent(generation) &&
                                !overlay->updatesEnabled() &&
                                !overlay->canvas()->updatesEnabled() &&
                                !overlay->canvas()->interactionEnabled(),
                            "terminal retirement must block painting and input before hide");
                }
                return false;
            }
        } inspection;
        inspection.overlay = &fixture.primary;
        inspection.generation = generation;
        fixture.primary.installEventFilter(&inspection);

        if (deferred) {
            fixture.coordinator.hideOverlayWindowsImmediately(fixture.displays);
            require(fixture.primary.internalWinId() == nativeId,
                    "export's early hide must preserve the native surface until maintenance");
            fixture.coordinator.hideOverlayWindowsImmediately(fixture.displays);
            fixture.coordinator.hideOverlayWindows(fixture.displays);
        } else {
            fixture.primary.releaseNativeSurface();
        }
        require(inspection.inspected && fixture.primary.isHidden() &&
                    fixture.primary.internalWinId() == 0,
                "completion must hide the retired presentation and destroy its native surface");
        fixture.primary.releaseNativeSurface();
        require(fixture.primary.internalWinId() == 0,
                "repeated completion must not recreate a native surface");
        fixture.primary.removeEventFilter(&inspection);
        fixture.primary.restoreNativeSurface();
        require(fixture.primary.isHidden() && fixture.primary.updatesEnabled() &&
                    fixture.primary.canvas()->updatesEnabled(),
                "the next capture must restore painting without revealing the overlay early");
        fixture.primary.showPreparedFrame();
        require(fixture.primary.isVisible() &&
                    fixture.primary.isPresentationCurrent(fixture.primary.presentationGeneration()),
                "the next capture must own a fresh visible presentation");
    }
}

void overlayRevealGenerationAndFocusRegressions() {
    terminalHideRetiresPresentationBeforeHidingAndAllowsReuse();
    retiredSecondaryRevealCannotShowThePooledOverlay();
    cancellationDuringFirstShowRetiresTheWholeRevealGroup();
    reentrantPublicShowOwnsANewPresentation();
    supersededSecondaryRevealCannotFocusTheNextPresentation();
    delayedActivationCannotFocusTheNextPresentation();
    preferredDisplayOwnsTheRevealGroupKeyboardFocus();
    secondaryRevealPreservesPreferredHeldKeyboardShortcuts();
    supersededGroupActivationCannotFocusTheReopenedDisplay();
}

class StyleToolbarCommands final : public ScreenshotToolbarCommandSink,
                                   public ScreenshotSelectionToolbarCommandSink {
  public:
    void setMoveTool() override {
        ++moveToolCount;
    }
    void setSelectTool() override {
        ++selectToolCount;
    }
    void setShapeTool() override {
        ++shapeToolCount;
    }
    void setArrowTool() override {}
    void setLineTool() override {}
    void setFreeDrawTool() override {}
    void setHighlightTool() override {}
    void setPenHighlightTool() override {}
    void setEraserTool() override {}
    void setFilterTool() override {}
    void setWatermarkTool() override {}
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle&, quint32) override {}
    void setTextTool() override {}
    void setSerialNumberTool() override {}
    void setOcrTool() override {}
    void startScrollingScreenshot() override {}
    void pinSelectionToScreen() override {}
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&, quint32) override {}
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSerialNumbers() override {}
    void incrementSelectedSerialNumbers() override {}
    void createTextForSelectedSerialNumber() override {}
    void repositionToolbarForContentChange() override {}
    void hideColorPickersForScreenshotUi() override {
        hidePickers();
    }
    void updateGuideLinesForScreenshotUi(const QPoint& globalPosition) override {
        ++guideLineUpdateCommands;
        guideLinePosition = globalPosition;
        updateGuideLines(globalPosition);
    }
    void setSelectionToolbarHidden(bool hidden) override {
        ++selectionToolbarVisibilityCommands;
        selectionToolbarVisibility(hidden);
    }

    void toggleSelectionAspectRatioLockFromToolbar() override {}
    void setSelectionAspectRatioPresetFromToolbar(ScreenshotSelectionAspectRatioPreset) override {}
    void openSelectionResizeModalFromToolbar() override {}
    void adjustSelectionFromToolbar(int, int, int, int) override {}
    void adjustSelectionEffectsFromWheel(int, int) override {}
    void setSelectionCornerRadiusFromToolbar(int) override {}
    void setSelectionEffectWidthFromToolbar(int) override {}
    void setSelectionToolbarHovered(bool) override {}
    int moveToolCount = 0;
    int selectToolCount = 0;
    int shapeToolCount = 0;
    int selectionToolbarVisibilityCommands = 0;
    int guideLineUpdateCommands = 0;
    QPoint guideLinePosition;
    std::function<void()> hidePickers = [] {};
    std::function<void(const QPoint&)> updateGuideLines = [](const QPoint&) {};
    std::function<void(bool)> selectionToolbarVisibility = [](bool) {};
};

void overlayPreparationCachesContentAndInvalidatesTranslations() {
    class HintPresentationObserver final : public QObject {
      public:
        int shows = 0;
        int hides = 0;
        int raises = 0;

      protected:
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Show) {
                ++shows;
            } else if (event->type() == QEvent::Hide) {
                ++hides;
            } else if (event->type() == QEvent::ZOrderChange) {
                ++raises;
            }
            return false;
        }
    } presentationObserver;
    class TranslationObserver final : public QTranslator {
      public:
        mutable int calls = 0;
        QString disabledRadiusTooltip;
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            if (QByteArray(context) == "ScreenshotShortcutHintsWidget" ||
                QByteArray(context) == "SettingsCatalog" ||
                QByteArray(context) == "ScreenshotSelectionToolbarWidget") {
                ++calls;
            }
            if (QByteArray(context) == "ScreenshotSelectionToolbarWidget" &&
                QByteArray(source) == "Corner radius is unavailable for custom regions") {
                return disabledRadiusTooltip;
            }
            return {};
        }
    } translator;
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage
                .initialize({directory.filePath(QStringLiteral("bin")),
                             directory.filePath(QStringLiteral("data")), 60000})
                .success,
            "initialize overlay preparation storage");
    {
        NoopOverlayEventSink sink;
        ScreenshotOverlayWindow overlay(sink, new SnowCanvasWidget);
        overlay.resize(900, 700);
        overlay.show();
        StyleToolbarCommands commands;
        ScreenshotOverlayUiHost host;
        host.setToolbarCommandSinks(commands, commands);
        host.attachSelectionToolbarToOverlay(&overlay);
        QApplication::installTranslator(&translator);
        QApplication::processEvents();

        ScreenshotShortcutHintContext context;
        context.captureMode = ScreenshotCaptureMode::IntelligentSelecting;
        const auto present = [&](qreal opacity, const QRectF& selection, const QPoint& cursor) {
            host.updateShortcutHints(&overlay, context, opacity, selection, cursor);
        };
        present(1.0, {}, overlay.mapToGlobal(QPoint(800, 20)));
        QWidget* hints = overlay.findChild<QWidget*>(QStringLiteral("screenshotShortcutHints"));
        require(hints != nullptr && !hints->accessibleName().isEmpty(),
                "shortcut hints must publish their initial content");
        const QString initialLines = hints->accessibleName();
        translator.calls = 0;
        for (int frame = 0; frame < 20; ++frame) {
            present(frame % 2 == 0 ? 0.75 : 1.0, QRectF(600 + frame, 20, 120, 80),
                    overlay.mapToGlobal(QPoint(800, 20 + frame)));
        }
        require(translator.calls == 0 && hints->accessibleName() == initialLines,
                "pointer, selection and opacity updates must reuse translated hint content");

        const QRectF hintBounds(hints->mapToGlobal(QPoint()), hints->size());
        present(1.0, hintBounds, overlay.mapToGlobal(QPoint(800, 20)));
        require(!hints->isVisible(), "selection obscuration must still hide cached hints");
        present(1.0, {}, overlay.mapToGlobal(QPoint(800, 20)));
        require(hints->isVisible(), "unobscured cached hints must become visible again");

        hints->installEventFilter(&presentationObserver);
        QWidget sibling(&overlay);
        sibling.setGeometry(0, 0, 10, 10);
        sibling.show();
        sibling.raise();
        const QPoint outside = overlay.mapToGlobal(QPoint(800, 20));
        const QPoint hintCenter = hintBounds.center().toPoint();
        const QRect cachedLayout = hints->geometry();
        translator.calls = 0;
        for (int frame = 0; frame < 20; ++frame) {
            host.updateShortcutHintPointer(&overlay, outside);
        }
        require(translator.calls == 0 && hints->geometry() == cachedLayout &&
                    presentationObserver.shows == 0 && presentationObserver.hides == 0 &&
                    presentationObserver.raises == 0,
                "unchanged pointer-only hints must reuse content and layout without showing or "
                "raising");

        host.updateShortcutHintPointer(&overlay, hintCenter);
        require(hints->isHidden() && presentationObserver.hides == 1,
                "pointer-only hint updates must hide content obscured by the pointer");
        for (int frame = 0; frame < 20; ++frame) {
            host.updateShortcutHintPointer(&overlay, hintCenter);
        }
        require(presentationObserver.hides == 1 && presentationObserver.shows == 0,
                "repeated obscured pointer updates must leave already hidden hints idle");
        host.updateShortcutHintPointer(&overlay, outside);
        require(hints->isVisible() && presentationObserver.shows == 1,
                "pointer-only hints must become visible again after leaving their bounds");
        const int raisesAfterReveal = presentationObserver.raises;
        QMouseEvent move(QEvent::MouseMove, QPointF(800, 20), QPointF(outside), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(overlay.canvas(), &move);
        require(hints->isVisible() && presentationObserver.shows == 1 &&
                    presentationObserver.hides == 1 &&
                    presentationObserver.raises == raisesAfterReveal,
                "standalone host mouse tracking must preserve unchanged hint visibility");

        {
            ScreenshotOverlayWindow secondOverlay(sink, new SnowCanvasWidget);
            secondOverlay.resize(900, 700);
            secondOverlay.show();
            const QPoint secondOutside = secondOverlay.mapToGlobal(QPoint(800, 20));
            host.updateShortcutHintPointer(&secondOverlay, secondOutside);
            require(hints->parentWidget() == &overlay,
                    "pointer-only updates must leave owner changes to complete presentation");
            host.updateShortcutHints(&secondOverlay, context, 1.0, {}, secondOutside);
            require(hints->parentWidget() == &secondOverlay && hints->isVisible() &&
                        hints->accessibleName() == initialLines,
                    "complete presentation must reattach cached hints to a new pointer owner");
            present(1.0, {}, outside);
        }
        sibling.hide();
        hints->removeEventFilter(&presentationObserver);

        snow_shot::shortcuts::ShortcutDisplayService::instance().refresh();
        require(translator.calls > 0, "native shortcut legend changes must regenerate hint rows");
        translator.calls = 0;
        QEvent languageChange(QEvent::LanguageChange);
        QApplication::sendEvent(hints, &languageChange);
        require(translator.calls > 0, "language changes must regenerate cached hint rows");

        context.configuredShortcuts = snow_shot::shortcuts::ShortcutBindingMap{
            {QStringLiteral("move_cursor_up"),
             snow_shot::shortcuts::bindingsFromPortableText({QStringLiteral("F12")}, true)}};
        translator.calls = 0;
        present(1.0, {}, overlay.mapToGlobal(QPoint(800, 20)));
        require(translator.calls > 0 && hints->accessibleName() != initialLines,
                "binding configuration changes must invalidate hint content");

        class ToolbarPaintObserver final : public QObject {
          public:
            int paints = 0;

          protected:
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::Paint) {
                    ++paints;
                }
                return false;
            }
        } toolbarPaints;
        auto* toolbar = host.selectionToolbar();
        toolbar->installEventFilter(&toolbarPaints);
        toolbar->setSelectionState(QRect(10, 20, 120, 80), false, 0, 0);
        host.showSelectionToolbar();
        require(
            toolbarPaints.paints == 1,
            "a prepared child toolbar must reveal its current state with one synchronous paint");
        {
            ScreenshotOverlayWindow secondOverlay(sink, new SnowCanvasWidget);
            secondOverlay.resize(900, 700);
            secondOverlay.show();
            QApplication::processEvents();
            toolbarPaints.paints = 0;
            host.attachSelectionToolbarToOverlay(&secondOverlay);
            require(toolbar->isVisible() && toolbar->parentWidget() == &secondOverlay &&
                        toolbarPaints.paints == 1,
                    "changing a visible toolbar owner must paint its retained content once");
            host.attachSelectionToolbarToOverlay(&overlay);
        }
        toolbar->removeEventFilter(&toolbarPaints);
        host.hideSelectionToolbar();
        translator.calls = 0;
        for (int frame = 0; frame < 20; ++frame) {
            host.hideSelectionToolbar();
        }
        require(translator.calls == 0, "hiding an already hidden toolbar must not prepare it");

        toolbar->setCornerRadiusApplicable(false);
        translator.calls = 0;
        for (int frame = 0; frame < 20; ++frame) {
            toolbar->setCornerRadiusApplicable(false);
        }
        require(translator.calls == 0,
                "unchanged corner applicability must not regenerate its tooltip");
        translator.disabledRadiusTooltip = QStringLiteral("translated disabled radius");
        QApplication::sendEvent(toolbar, &languageChange);
        bool sawTranslatedRadius = false;
        for (QLabel* label : toolbar->findChildren<QLabel*>()) {
            sawTranslatedRadius =
                sawTranslatedRadius || label->toolTip() == translator.disabledRadiusTooltip;
        }
        require(sawTranslatedRadius,
                "language changes must refresh the disabled radius tooltip despite its guard");
        QApplication::removeTranslator(&translator);
    }
    storage.shutdown();
}

class PointerMoveDeliveryObserver final : public QObject {
  public:
    int deliveredMoves = 0;

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::MouseMove)
            ++deliveredMoves;
        return false;
    }
};

void selectionToolbarVisibilitySurvivesCapturesAndRestarts() {
    namespace storage = snow_shot::storage;
    QTemporaryDir temporary;
    require(temporary.isValid(), "selection toolbar tests require isolated settings");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    const storage::StorageInitializationOptions options{temporary.path(), temporary.path(), 60000};
    const storage::ScreenshotUiSettings settings;
    for (int launch = 0; launch < 3; ++launch) {
        require(applicationStorage.initialize(options).success,
                "initialize saved selection toolbar settings");
        const bool initiallyHidden = launch == 1;
        require(settings.selectionToolbarHidden() == initiallyHidden,
                "selection toolbar visibility must default to shown and survive restarts");
        {
            NoopOverlayEventSink sink;
            ScreenshotOverlayWindow first(sink, new SnowCanvasWidget);
            ScreenshotOverlayWindow second(sink, new SnowCanvasWidget);
            first.setGeometry(0, 0, 1000, 700);
            second.setGeometry(1000, 0, 1000, 700);
            first.show();
            second.show();
            StyleToolbarCommands commands;
            ScreenshotOverlayUiHost host;
            host.setToolbarCommandSinks(commands, commands);
            auto* window = host.ensureToolbar();
            const auto hideButton = [&]() {
                auto* button = window->palette()->findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotHideSelectionToolbarButton"));
                require(button != nullptr, "move sub-toolbar must expose the visibility option");
                return button;
            };
            const auto showSelectionToolbar = [&](ScreenshotOverlayWindow& overlay) {
                host.attachSelectionToolbarToOverlay(&overlay);
                host.selectionToolbar()->setSelectionState(QRect(100, 100, 300, 200), false, 0, 0);
                host.showSelectionToolbar();
            };
            const auto requireHidden = [&](bool hidden) {
                require(settings.selectionToolbarHidden() == hidden &&
                            window->palette()->selectionToolbarHidden() == hidden &&
                            (hideButton()->buttonStyle() ==
                             adqt::widgets::AdButton::ButtonStyle::Solid) == hidden &&
                            host.selectionToolbar()->isVisible() != hidden,
                        "the saved option, move button and selection toolbar must agree");
            };
            commands.selectionToolbarVisibility = [&](bool hidden) {
                require(settings.selectionToolbarHidden() == hidden,
                        "the visibility preference must be saved before applying the command");
                host.setSelectionToolbarHidden(hidden);
                if (!hidden)
                    host.showSelectionToolbar();
            };
            showSelectionToolbar(first);
            requireHidden(initiallyHidden);
            if (launch == 0) {
                hideButton()->click();
                require(commands.selectionToolbarVisibilityCommands == 1,
                        "a visibility click must dispatch exactly one command");
                requireHidden(true);
                applicationStorage.configuration().suspendWrites(true);
                hideButton()->click();
                requireHidden(true);
                require(commands.selectionToolbarVisibilityCommands == 2,
                        "a rejected preference change must apply the saved visibility");
                applicationStorage.configuration().suspendWrites(false);
                host.resetToolbarForNewCapture();
                showSelectionToolbar(first);
                requireHidden(true);
                showSelectionToolbar(second);
                requireHidden(true);
                host.destroyUiResources();
                host.setToolbarCommandSinks(commands, commands);
                window = host.ensureToolbar();
                showSelectionToolbar(first);
                requireHidden(true);
                require(settings.setSelectionToolbarHidden(false), "change visibility externally");
                showSelectionToolbar(first);
                requireHidden(false);
                require(settings.setSelectionToolbarHidden(true), "hide the toolbar externally");
                requireHidden(true);
                require(commands.selectionToolbarVisibilityCommands == 2,
                        "preference synchronization and capture resets must not dispatch commands");
            } else if (launch == 1) {
                hideButton()->click();
                requireHidden(false);
                require(commands.selectionToolbarVisibilityCommands == 1,
                        "the restored visibility option must remain reversible");
                host.resetToolbarForNewCapture();
                showSelectionToolbar(second);
                requireHidden(false);
            }
            require(applicationStorage.configuration().flushNow().success,
                    "selection toolbar visibility must be written to disk");
        }
        applicationStorage.shutdown();
    }
}

void toolbarHoverKeepsScreenshotGuidesResponsive() {
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "toolbar guide tests require isolated settings");
    {
        NoopOverlayEventSink sink;
        SnowCanvasRuntime canvas;
        snow_shot::presentation::WindowShortcutManager shortcuts;
        ScreenshotOverlayWindow first(sink, new SnowCanvasWidget);
        ScreenshotOverlayWindow second(sink, new SnowCanvasWidget);
        first.setCaptureGeometry(QRect(-1000, 80, 1000, 700));
        second.setCaptureGeometry(QRect(0, 80, 1000, 700));
        first.show();
        second.show();
        ScreenshotDisplaySession displays;
        for (auto* overlay : {&first, &second}) {
            CapturedDisplayModel display;
            display.logicalRect = overlay->captureGeometry();
            display.physicalRect = display.logicalRect;
            display.active = true;
            display.geometryResolved = true;
            displays.appendDisplay(display, overlay);
            overlay->canvas()->setInteractionEnabled(true);
            require(overlay->canvas()->setCanvasTool(SnowCanvasTool::FreeDraw),
                    "the guide test must preserve a drawing tool");
        }
        StyleToolbarCommands commands;
        ScreenshotOverlayCoordinator coordinator(sink, canvas, shortcuts);
        commands.updateGuideLines = [&](const QPoint& position) {
            coordinator.updateGuideLinesAtGlobalPosition(displays, position, true, Qt::red,
                                                         Qt::transparent);
        };
        coordinator.setToolbarCommandSinks(commands, commands);
        coordinator.attachToolbarToOverlay(&second);
        auto* toolbar = coordinator.toolbar();
        toolbar->moveContentTo(QPoint(60, 300));
        coordinator.showToolbar();
        QCoreApplication::processEvents();
        const auto sendMove = [](QWidget* receiver, const QPointF& local,
                                 Qt::MouseButtons buttons = Qt::NoButton) {
            const QPointF global = receiver->mapToGlobal(local);
            QMouseEvent event(QEvent::MouseMove, local, receiver->window()->mapFromGlobal(global),
                              global, Qt::NoButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(receiver, &event);
        };
        const auto requireGuideAt = [&](QWidget* receiver, const QPoint& local) {
            const QPoint global = receiver->mapToGlobal(local);
            require(commands.guideLinePosition == global,
                    "toolbar movement must forward the event's global coordinates");
            auto* owner = global.x() < 0 ? &first : &second;
            auto* other = owner == &first ? &second : &first;
            require(owner->screenshotRendererForTesting()->guideLinesVisible() &&
                        !other->screenshotRendererForTesting()->guideLinesVisible(),
                    "toolbar guides must follow the display under the pointer");
            QImage image(owner->canvas()->size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            owner->screenshotRendererForTesting()->renderAfterCanvas(
                painter, {image.rect(), QRegion(image.rect()), QTransform(), 1.0});
            painter.end();
            const QPoint position = owner->canvasLocalPosition(global);
            require(image.rect().contains(position) &&
                        image.pixelColor(position.x(), 2) == QColor(Qt::red),
                    "the screenshot must paint its vertical guide at the current toolbar pointer");
        };

        // Plain QWidget children do not request mouse tracking. Qt still passes
        // their moves to application filters before discarding widget delivery.
        auto* trigger = toolbar->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotArrowLineButton"));
        require(trigger && trigger->isVisible(), "the drawing group trigger must be visible");
        QWidget child(trigger);
        child.setGeometry(QRect(trigger->rect().center() - QPoint(8, 8), QSize(16, 16)));
        child.show();
        require(!child.hasMouseTracking(), "the regression must exercise a non-tracking child");
        require(coordinator.screenshotUiContainsGlobalPoint(child.mapToGlobal(QPoint(3, 3))),
                "the non-tracking child must occupy interactive drawing toolbar content");
        coordinator.updateGuideLinesAtGlobalPosition(displays, QPoint(20, 100), true, Qt::red,
                                                     Qt::transparent);
        const int beforeMove = commands.guideLineUpdateCommands;
        resetGuideLineRenderDiagnosticsForCurrentThread();
        sendMove(&child, QPointF(3.1, 3.1));
        require(commands.guideLineUpdateCommands > beforeMove,
                "moving over a non-tracking toolbar child must update screenshot guides");
        requireGuideAt(&child, QPoint(3, 3));
        const auto movementDamage = guideLineRenderDiagnosticsForCurrentThread();
        require(movementDamage.updateRequests == 1 && movementDamage.requestedDamagePixels > 0 &&
                    movementDamage.requestedDamagePixels <
                        static_cast<std::size_t>(second.canvas()->width() *
                                                 second.canvas()->height() / 10),
                "toolbar movement must repaint only the changed guide strips once");
        resetGuideLineRenderDiagnosticsForCurrentThread();
        sendMove(&child, QPointF(3.8, 3.8));
        sendMove(&child, QPointF(3.8, 3.8));
        requireGuideAt(&child, QPoint(3, 3));
        require(guideLineRenderDiagnosticsForCurrentThread().updateRequests == 0 &&
                    guideLineRenderDiagnosticsForCurrentThread().requestedDamagePixels == 0,
                "duplicate and same-pixel toolbar movement must not request guide repaints");

        toolbar->setActiveTool(ScreenshotToolPalette::Tool::Shape);
        QCoreApplication::processEvents();
        auto* style = toolbar->palette()->stylePanel();
        require(style && style->isVisible(), "the drawing style panel must be visible");
        const QPoint styleLocal = style->rect().center();
        const QPoint styleGlobal = style->mapToGlobal(styleLocal);
        QEnterEvent enter(styleLocal, style->window()->mapFromGlobal(styleGlobal), styleGlobal);
        QApplication::sendEvent(style, &enter);
        requireGuideAt(style, styleLocal);

        auto* popover = trigger ? trigger->findChild<adqt::widgets::AdPopover*>() : nullptr;
        require(popover, "the toolbar guide test requires a drawing group popup");
        popover->preparePopup();
        popover->show();
        auto* content = popover->contentWidget();
        require(content && content->isVisible(), "the group popup must be visible");
        PointerMoveDeliveryObserver delivery;
        content->installEventFilter(&delivery);
        sendMove(content, content->rect().center(), Qt::LeftButton);
        require(delivery.deliveredMoves > 0,
                "guide forwarding must preserve delivery of pointer events to popup controls");
        requireGuideAt(content, content->rect().center());
        auto* surface = popover->surfaceWidget();
        const int beforeShadow = commands.guideLineUpdateCommands;
        sendMove(surface, QPointF(0, 0));
        require(commands.guideLineUpdateCommands == beforeShadow,
                "transparent popup shadows must remain outside screenshot UI guide forwarding");
        popover->hide();
        coordinator.attachSelectionToolbarToOverlay(&second);
        coordinator.selectionToolbar()->setSelectionState(QRect(100, 100, 300, 200), false, 0, 0);
        coordinator.showSelectionToolbar();
        auto* selection = coordinator.selectionToolbar();
        sendMove(selection, selection->rect().center());
        requireGuideAt(selection, selection->rect().center());
        coordinator.hideToolbar();
        const int beforeHidden = commands.guideLineUpdateCommands;
        sendMove(&child, QPointF(6, 6));
        sendMove(content, content->rect().center());
        require(commands.guideLineUpdateCommands == beforeHidden,
                "hidden toolbar children and popups must stop forwarding pointer movement");

        coordinator.resetToolbarForNewCapture();
        coordinator.attachToolbarToOverlay(&first);
        toolbar->moveContentTo(QPoint(-900, 300));
        coordinator.showToolbar();
        require(coordinator.screenshotUiContainsGlobalPoint(child.mapToGlobal(QPoint(7, 7))),
                "the reused child must occupy interactive drawing toolbar content");
        sendMove(&child, QPointF(7.8, 7.8));
        requireGuideAt(&child, QPoint(7, 7));
        const int beforeCanvas = commands.guideLineUpdateCommands;
        sendMove(first.canvas(), QPointF(20, 20));
        require(commands.guideLineUpdateCommands == beforeCanvas,
                "canvas movement must retain its original input path");
        for (auto* overlay : {&first, &second}) {
            require(overlay->canvas()->canvasTool() == SnowCanvasTool::FreeDraw &&
                        !overlay->canvas()->canvasHistoryState().canUndo,
                    "toolbar guide movement must not change tools or create drawing gestures");
        }
        child.setParent(nullptr);
        coordinator.destroyUiResources();
        const int beforeDestroy = commands.guideLineUpdateCommands;
        sendMove(&child, QPointF(8, 8));
        require(commands.guideLineUpdateCommands == beforeDestroy,
                "destroying capture UI must release its pointer forwarding scope");
        commands.updateGuideLines = [](const QPoint&) {};
    }
    storage.shutdown();
}

void toolbarPopoversSuppressPickerAcrossWindowBoundaries() {
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "toolbar hover tests require isolated settings");
    {
        NoopOverlayEventSink sink;
        SnowCanvasRuntime canvas;
        snow_shot::presentation::WindowShortcutManager shortcuts;
        ScreenshotOverlayWindow overlay(sink, new SnowCanvasWidget);
        overlay.setGeometry(0, 0, 1200, 800);
        overlay.show();
        CapturedDisplayModel display;
        display.logicalRect = overlay.geometry();
        display.physicalRect = display.logicalRect;
        display.active = true;
        display.geometryResolved = true;
        display.image = QImage(display.physicalRect.size(), QImage::Format_RGB32);
        display.image.fill(Qt::red);
        ScreenshotDisplaySession displays;
        displays.appendDisplay(display, &overlay);
        ScreenshotGeometryMapper geometry;
        geometry.rebuild(displays);
        snow_shot::platform::PhysicalCursor cursor;
        StyleToolbarCommands commands;
        ScreenshotOverlayCoordinator coordinator(sink, canvas, shortcuts);
        bool queuedPicker = false;
        commands.hidePickers = [&]() {
            queuedPicker = false;
            coordinator.hideColorPicker();
        };
        coordinator.setToolbarCommandSinks(commands, commands);
        coordinator.attachToolbarToOverlay(&overlay);
        auto* toolbar = coordinator.toolbar();
        toolbar->moveContentTo(QPoint(100, 300));
        coordinator.showToolbar();
        coordinator.createColorPicker(QPoint(50, 50));
        coordinator.prepareColorPickerSurface(displays);
        QCoreApplication::processEvents();
        ScreenshotColorPickerController controller(coordinator, geometry, displays, cursor);
        ScreenshotColorPickerContext context;
        context.active = true;
        context.moveToolActive = true;
        context.manualSelecting = true;

        auto* trigger = toolbar->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotArrowLineButton"));
        require(trigger && trigger->isVisible(), "drawing group trigger must be visible");
        auto* popover = trigger->findChild<adqt::widgets::AdPopover*>();
        require(popover, "drawing group must own a popover");
        popover->preparePopup();
        popover->show();
        auto* surface = popover->surfaceWidget();
        require(surface && surface->isVisible(), "drawing group popover must be visible");

        const QPoint triggerPoint = trigger->mapToGlobal(trigger->rect().center());
        const QPoint gapPoint(trigger->mapToGlobal(QPoint()).x(),
                              trigger->parentWidget()->mapToGlobal(QPoint()).y() - 2);
        require(!coordinator.screenshotUiContainsGlobalPoint(gapPoint),
                "the blank gap must remain part of the screenshot canvas");
        controller.updateAfterCursorMove(gapPoint, context);
        require(coordinator.colorPicker()->isVisible(),
                "crossing the canvas gap must be able to reveal the screenshot picker");

        const QPoint popupPoint = surface->mapToGlobal(surface->rect().center());
        QEnterEvent enter(surface->mapFromGlobal(popupPoint), surface->mapFromGlobal(popupPoint),
                          popupPoint);
        QApplication::sendEvent(surface, &enter);
        require(!coordinator.colorPicker()->isVisible(),
                "entering the separate group popover must immediately hide the screenshot picker");
        require(coordinator.screenshotUiContainsGlobalPoint(popupPoint),
                "the popover body must belong to screenshot UI");
        queuedPicker = true;
        QMouseEvent pendingMove(QEvent::MouseMove, surface->mapFromGlobal(popupPoint),
                                surface->mapFromGlobal(popupPoint), popupPoint, Qt::NoButton,
                                Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &pendingMove);
        require(!queuedPicker && coordinator.colorPicker()->isHidden(),
                "popup input must cancel queued canvas samples while the picker is already hidden");
        controller.updateAfterCursorMove(popupPoint, context);
        require(!coordinator.colorPicker()->isVisible(),
                "picker updates over a popover must keep it hidden");

        bool coveredGapReported = false;
        const auto revealPickerOnCanvas = [&]() {
            auto* canvasWidget = overlay.canvas();
            QPoint revealPoint = gapPoint;
            if (coordinator.screenshotUiContainsGlobalPoint(revealPoint)) {
                // A nested popup may legitimately cover the original toolbar gap.
                const QRect candidates = canvasWidget->rect().adjusted(20, 20, -20, -20);
                bool foundCanvasPoint = false;
                for (const QPoint& local : {candidates.topLeft(), candidates.topRight(),
                                            candidates.bottomLeft(), candidates.bottomRight()}) {
                    const QPoint global = canvasWidget->mapToGlobal(local);
                    if (!coordinator.screenshotUiContainsGlobalPoint(global)) {
                        revealPoint = global;
                        foundCanvasPoint = true;
                        break;
                    }
                }
                require(foundCanvasPoint,
                        "picker restoration requires an uncovered point on the overlay canvas");
                if (!coveredGapReported) {
                    std::cout << "covered gap (" << gapPoint.x() << ", " << gapPoint.y()
                              << ") -> canvas (" << revealPoint.x() << ", " << revealPoint.y()
                              << ")\n";
                    coveredGapReported = true;
                }
            }
            require(canvasWidget->rect().contains(canvasWidget->mapFromGlobal(revealPoint)) &&
                        !coordinator.screenshotUiContainsGlobalPoint(revealPoint),
                    "picker restoration must use canvas input outside every visible popup");
            controller.updateAfterCursorMove(revealPoint, context);
            require(coordinator.colorPicker()->isVisible(),
                    "returning to uncovered canvas input must restore the picker");
        };
        const auto enterWidget = [&](QWidget* receiver) {
            const QPoint local = receiver->rect().center();
            const QPoint global = receiver->mapToGlobal(local);
            QEnterEvent event(local, receiver->window()->mapFromGlobal(global), global);
            QApplication::sendEvent(receiver, &event);
            require(!coordinator.colorPicker()->isVisible(),
                    "entering owned screenshot UI must hide the picker without a canvas move");
            controller.updateAfterCursorMove(global, context);
            require(!coordinator.colorPicker()->isVisible(),
                    "subsequent picker updates must respect owned screenshot UI");
        };
        auto* content = popover->contentWidget();
        require(content && content->isVisible(), "group popover content must be visible");
        revealPickerOnCanvas();
        const QPoint local = content->rect().center();
        const QPoint global = content->mapToGlobal(local);
        QMouseEvent move(QEvent::MouseMove, local, content->window()->mapFromGlobal(global), global,
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(content, &move);
        require(!coordinator.colorPicker()->isVisible(),
                "moving inside a popup child must also hide the picker");
        for (auto type : {QEvent::HoverEnter, QEvent::HoverMove}) {
            revealPickerOnCanvas();
            QHoverEvent hover(type, local, global, QPointF(-1, -1), Qt::NoModifier);
            QApplication::sendEvent(content, &hover);
            require(!coordinator.colorPicker()->isVisible(),
                    "hover events in popup children must hide the picker");
        }

        // Painted popup shadows are canvas input, even when the native popup
        // frame covers them. Do not suppress the picker merely for that frame.
        auto* popupSurface = dynamic_cast<adqt::widgets::detail::OverlayPopupSurface*>(surface);
        require(popupSurface, "group must expose its painted popup interaction surface");
        const QPoint shadow = surface->mapToGlobal(QPoint(0, 0));
        require(!popupSurface->containsInteractiveGlobalPos(shadow) &&
                    !coordinator.screenshotUiContainsGlobalPoint(shadow),
                "popup shadows must remain outside screenshot UI");
        controller.updateAfterCursorMove(shadow, context);
        require(coordinator.colorPicker()->isVisible(),
                "picker must remain available in the popup's transparent margin");
        QMouseEvent shadowMove(QEvent::MouseMove, QPoint(0, 0), QPoint(0, 0), shadow, Qt::NoButton,
                               Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &shadowMove);
        require(coordinator.colorPicker()->isVisible(),
                "a pointer event over popup shadow must not hide the picker");

        // Nested QtTool popovers have a host on the parent surface rather than
        // on the toolbar window. Their input still belongs to this capture UI.
        {
            auto* nestedTrigger = content->findChild<adqt::widgets::AdButton*>();
            require(nestedTrigger && nestedTrigger->isVisible(),
                    "nested popup anchor must be visible");
            adqt::widgets::AdPopover nested(nestedTrigger);
            nested.setSourceWidget(nestedTrigger);
            nested.setTriggers(adqt::widgets::AdPopover::Trigger::Click);
            nested.setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::QtTool);
            nested.setPlacement(adqt::widgets::AdPopover::Placement::Right);
            auto* nestedContent = new QWidget;
            nestedContent->setFixedSize(120, 70);
            nested.setContentWidget(nestedContent);
            nested.show();
            require(nested.surfaceWidget() && nested.surfaceWidget()->isVisible() &&
                        surface->isVisible(),
                    "nested popup and its parent must stay visible together");
            const QPoint nestedPoint = nestedContent->mapToGlobal(nestedContent->rect().center());
            require(
                coordinator.screenshotUiContainsGlobalPoint(nestedPoint) &&
                    adqt::widgets::detail::popupInteractionContainsGlobalPos(surface, nestedPoint),
                "nested top-level popup input must belong to the original toolbar scope");
            revealPickerOnCanvas();
            enterWidget(nestedContent);
            nested.hide();
            // Its center may still belong to the parent popup. Query the nested
            // host directly so parent overlap cannot hide a leaked interaction region.
            require(
                !nested.isVisible() &&
                    (!nested.surfaceWidget() || !nested.surfaceWidget()->isVisible()) &&
                    !adqt::widgets::detail::popupInteractionContainsGlobalPos(surface, nestedPoint),
                "a closed nested popup must release its own interaction region");
        }

        popover->hide();
        require(!coordinator.screenshotUiContainsGlobalPoint(popupPoint),
                "a closed group popup must release its interaction region");
        controller.updateAfterCursorMove(popupPoint, context);
        require(coordinator.colorPicker()->isVisible(),
                "closing the group popup must restore canvas magnification there");
        enterWidget(trigger);

        // Select popups use another owner implementation but the same host.
        adqt::widgets::AdSelect select(toolbar);
        select.move(toolbar->mapFromGlobal(trigger->mapToGlobal(QPoint())));
        select.setFixedSize(120, 30);
        select.setOptions({{QStringLiteral("first"), QStringLiteral("First")},
                           {QStringLiteral("second"), QStringLiteral("Second")}});
        select.setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::QtTool);
        select.show();
        select.showPopup();
        require(select.popupVisible() && select.view() && select.view()->isVisible(),
                "toolbar select popup must be visible");
        const QPoint selectPoint = select.view()->mapToGlobal(select.view()->rect().center());
        require(coordinator.screenshotUiContainsGlobalPoint(selectPoint),
                "select popup input must also belong to screenshot UI");
        revealPickerOnCanvas();
        enterWidget(select.view()->viewport());
        select.hidePopup();
        select.hide();

        // Reuse the toolbar and popup for another capture; closed or hidden UI
        // must never retain suppression from an earlier interaction.
        coordinator.hideToolbar();
        coordinator.resetToolbarForNewCapture();
        coordinator.showToolbar();
        popover->show();
        require(popover->surfaceWidget() && popover->surfaceWidget()->isVisible(),
                "group popover must reopen after capture reset");
        revealPickerOnCanvas();
        enterWidget(popover->contentWidget());
        coordinator.hideToolbar();
        require(!coordinator.screenshotUiContainsGlobalPoint(triggerPoint) &&
                    !coordinator.screenshotUiContainsGlobalPoint(popupPoint),
                "hidden screenshot toolbar must release its entire popup interaction surface");
        controller.updateAfterCursorMove(triggerPoint, context);
        require(coordinator.colorPicker()->isVisible(),
                "hidden toolbar must not suppress canvas magnification");
    }
    storage.shutdown();
}

void screenshotStyleBindingFollowsToolbarAttachment() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary style storage available");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated style storage");
    {
        NoopOverlayEventSink sink;
        ScreenshotOverlayWindow first(sink, new SnowCanvasWidget);
        ScreenshotOverlayWindow second(sink, new SnowCanvasWidget);
        StyleToolbarCommands commands;
        ScreenshotOverlayUiHost host;
        host.setToolbarCommandSinks(commands, commands);
        host.attachToolbarToOverlay(&first);
        first.canvas()->setInteractionEnabled(true);
        require(first.canvas()->setCanvasTool(SnowCanvasTool::Text), "activate first canvas text");
        host.toolbar()->palette()->setActiveTool(ScreenshotToolPalette::Tool::Text);
        const double firstSize = first.canvas()->canvasStyleToolbarState().textStyle.fontSize;
        require(first.canvas()->stepFontSize(1), "step first screenshot font");
        require(snow_shot::presentation::screenshotCanvasToolStyleDefaults().text.fontSize ==
                    firstSize + 1,
                "real screenshot UI host persists canvas font edits");
        host.attachToolbarToOverlay(&second);
        second.canvas()->setInteractionEnabled(true);
        require(second.canvas()->setCanvasTool(SnowCanvasTool::Text),
                "activate second canvas text");
        const double secondSize = second.canvas()->canvasStyleToolbarState().textStyle.fontSize;
        require(second.canvas()->stepFontSize(-1), "step second screenshot font");
        require(snow_shot::presentation::screenshotCanvasToolStyleDefaults().text.fontSize ==
                    secondSize - 1,
                "moving the screenshot toolbar rebinds style persistence");
        host.detachOverlayTransientUi(&second);
        const auto saved = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
        require(second.canvas()->stepFontSize(1),
                "detached canvas can still update its local style");
        require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() == saved,
                "detaching screenshot UI removes its preference binding");
    }
    storage.shutdown();
}

void pickerLifetimeFollowsExplicitSessionOperations() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage
                .initialize({QDir(temporary.path()).filePath(QStringLiteral("bin")),
                             temporary.path(), 60000})
                .success,
            "storage must initialize");
    NoopOverlayEventSink sink;
    ScreenshotOverlayWindow first(sink, new SnowCanvasWidget);
    ScreenshotOverlayWindow second(sink, new SnowCanvasWidget);
    first.setGeometry(0, 0, 800, 600);
    second.setGeometry(800, 0, 800, 600);
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const auto screens = QGuiApplication::screens();
        require(!screens.isEmpty(), "native smoke test requires an available display");
        first.setScreen(screens.first());
        first.setGeometry(screens.first()->geometry());
        second.setScreen(screens.last());
        second.setGeometry(screens.last()->geometry());
        std::cout << "Native picker smoke test: " << screens.size() << " display(s) available\n";
    }
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    QPointer<ScreenshotColorPickerWindow> tracked;
    {
        ScreenshotOverlayUiHost host;
        require(host.colorPicker() == nullptr, "idle host must not own a picker");
        host.setColorPickerCenterGuideLineColor(Qt::green);
        host.prepareColorPickerSurface(&first);
        host.updateColorPicker(
            &first, image, image.rect(), QPoint(8, 8), QPointF(8, 8),
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
        require(host.colorPicker() == nullptr,
                "preparation and updates must never create a picker");
        host.createColorPicker();
        tracked = host.colorPicker();
        host.createColorPicker();
        require(tracked && tracked == host.colorPicker() && !tracked->isVisible(),
                "session creation must be idempotent and hidden");
        host.prepareColorPickerSurface(&first);
        require(tracked->windowHandle() && !tracked->isVisible() &&
                    tracked->windowHandle()->transientParent() == first.windowHandle(),
                "preparation must create a hidden native window owned by the overlay");
        const auto requireNonNativeCanvases = [&]() {
            for (const auto* overlay : {&first, &second}) {
                const auto* canvas = overlay->findChild<SnowCanvasWidget*>();
                require(canvas && !canvas->testAttribute(Qt::WA_NativeWindow) &&
                            canvas->internalWinId() == 0,
                        "preparing and moving the picker must not create native canvas children");
            }
        };
        requireNonNativeCanvases();
        const uchar* preparedPixels = backingPixels(*tracked);
        const WId preparedWindowId = tracked->internalWinId();
        host.prepareColorPickerSurface(&first);
        require(!tracked->isVisible() && !tracked->windowHandle()->isVisible() &&
                    tracked->internalWinId() == preparedWindowId &&
                    backingPixels(*tracked) == preparedPixels && !tracked->hasCurrentColor(),
                "repeated preparation must retain hidden native pixels without requiring an image");
        host.updateColorPicker(
            &first, image, image.rect(), QPoint(8, 8), QPointF(8, 8),
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
        QApplication::processEvents();
        require(tracked->hasCurrentColor() && tracked->isVisible(),
                "the prepared picker must reveal its first sample");
        require(tracked->internalWinId() == preparedWindowId &&
                    backingPixels(*tracked) == preparedPixels,
                "the first sampled frame must reuse the preallocated native surface");
        tracked->cycleColorFormat();
        const QString format = tracked->currentColorText();
        host.updateColorPicker(
            &second, image, image.rect(), QPoint(8, 8), QPointF(8, 8),
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
        require(tracked == host.colorPicker() && tracked->parentWidget() == &second &&
                    tracked->windowHandle()->transientParent() == second.windowHandle(),
                "moving between overlays must retain one picker and update its native owner");
        requireNonNativeCanvases();
        const QPoint globalCursor = second.mapToGlobal(QPoint(8, 8));
        require(tracked->pos().x() > globalCursor.x() && tracked->pos().y() > globalCursor.y(),
                "changing displays must position the picker next to the new global cursor");
        host.prepareColorPickerSurface(&first);
        require(tracked->parentWidget() == &second && tracked->isVisible(),
                "capture-result preparation must preserve the current display and visibility");
        bool imageReleased = false;
        unsigned char pixels[16 * 16 * 4]{};
        QImage retainedImage(
            pixels, 16, 16, QImage::Format_RGBA8888,
            [](void* state) { *static_cast<bool*>(state) = true; }, &imageReleased);
        retainedImage.fill(Qt::red);
        tracked->setCaptureImage(retainedImage, retainedImage.rect());
        retainedImage = QImage();
        require(!imageReleased, "the picker must hold the capture image during the session");
#ifdef Q_OS_WIN
        const HWND nativeWindow = QGuiApplication::platformName() == QStringLiteral("windows")
                                      ? reinterpret_cast<HWND>(tracked->winId())
                                      : nullptr;
#endif
        host.releaseColorPicker();
        require(tracked.isNull() && host.colorPicker() == nullptr && imageReleased,
                "session release must synchronously destroy the window and release its image");
#ifdef Q_OS_WIN
        require(nativeWindow == nullptr || !IsWindow(nativeWindow),
                "session release must destroy the native Windows window");
#endif
        host.releaseColorPicker();
        host.resetColorPickerForNewCapture();
        host.updateColorPicker(
            &second, image, image.rect(), QPoint(8, 8), QPointF(8, 8),
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
        require(host.colorPicker() == nullptr,
                "late updates and cleanup must leave the picker absent");

        host.createColorPicker();
        tracked = host.colorPicker();
        require(!tracked->hasCurrentColor() && tracked->currentColorText().isEmpty(),
                "a replacement picker must not retain the old sample");
        host.updateColorPicker(
            &first, image, image.rect(), QPoint(8, 8), QPointF(8, 8),
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
        require(tracked->currentColorText() == format,
                "a replacement picker must restore the selected format");
        host.detachOverlayTransientUi(&first);
        require(tracked && tracked->parentWidget() == nullptr,
                "detaching an overlay must preserve its session picker");
    }
    require(tracked.isNull(), "host destruction must release a detached picker");
    {
        ScreenshotOverlayUiHost host;
        auto* owner = new ScreenshotOverlayWindow(sink, new SnowCanvasWidget);
        host.createColorPicker();
        host.prepareColorPickerSurface(owner);
        tracked = host.colorPicker();
        delete owner;
        require(tracked.isNull() && host.colorPicker() == nullptr,
                "owner destruction must clear host tracking");
        host.releaseColorPicker();
    }
    storage.shutdown();
}

void visibleRecaptureWindowsIncludePicker() {
    NoopOverlayEventSink sink;
    SnowCanvasRuntime runtime;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow overlay(sink, new SnowCanvasWidget);
    overlay.setGeometry(0, 0, 320, 240);
    overlay.show();
    QApplication::processEvents();

    CapturedDisplayModel display;
    display.logicalRect = overlay.geometry();
    display.physicalRect = display.logicalRect;
    display.active = true;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display, &overlay);
    ScreenshotOverlayCoordinator coordinator(sink, runtime, shortcuts);
    coordinator.createColorPicker(overlay.geometry().center());
    coordinator.prepareColorPickerSurface(displays);
    auto* picker = coordinator.colorPicker();
    require(picker != nullptr && !picker->isVisible() &&
                coordinator.visibleRecaptureWindows(displays) == QVector<QWidget*>{&overlay},
            "recapture must include the visible overlay but not the prepared hidden picker");

    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    coordinator.updateColorPicker(
        &overlay, image, image.rect(), QPoint(8, 8), QPointF(50, 50),
        {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
    QApplication::processEvents();
    require(picker->isVisible() && picker->isWindow() &&
                coordinator.visibleRecaptureWindows(displays) ==
                    QVector<QWidget*>{&overlay, picker},
            "recapture must protect the visible native picker as well as its overlay");

    coordinator.hideColorPicker();
    require(coordinator.visibleRecaptureWindows(displays) == QVector<QWidget*>{&overlay},
            "recapture must stop protecting the picker after it is hidden");
}

void invocationMonitorOwnsThePreparedSurface() {
    NoopOverlayEventSink sink;
    SnowCanvasRuntime runtime;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    // Include displays left of and above the primary, and use physical bounds
    // different from logical bounds to catch mixing coordinate systems.
    for (const QRect secondaryRect :
         {QRect(800, 0, 800, 600), QRect(-800, 0, 800, 600), QRect(0, -600, 800, 600)}) {
        ScreenshotOverlayWindow primary(sink, new SnowCanvasWidget);
        ScreenshotOverlayWindow secondary(sink, new SnowCanvasWidget);
        primary.setGeometry(0, 0, 800, 600);
        secondary.setGeometry(secondaryRect);
        CapturedDisplayModel primaryDisplay;
        primaryDisplay.logicalRect = primary.geometry();
        primaryDisplay.physicalRect = primary.geometry();
        primaryDisplay.active = true;
        CapturedDisplayModel secondaryDisplay;
        secondaryDisplay.logicalRect = secondaryRect;
        secondaryDisplay.physicalRect = QRect(5000, 5000, 1600, 1200);
        secondaryDisplay.active = true;
        ScreenshotDisplaySession displays;
        displays.appendDisplay(primaryDisplay, &primary);
        displays.appendDisplay(secondaryDisplay, &secondary);
        ScreenshotOverlayCoordinator coordinator(sink, runtime, shortcuts);

        const QPoint invocationPosition = secondaryRect.center();
        coordinator.createColorPicker(invocationPosition);
        coordinator.prepareColorPickerSurface(displays);
        auto* picker = coordinator.colorPicker();
        require(picker && picker->parentWidget() == &secondary && !picker->isVisible(),
                "the invocation monitor must own the hidden picker even when it is not first");
        require(secondaryRect.contains(picker->geometry().center()) &&
                    picker->windowHandle()->transientParent() == secondary.windowHandle(),
                "native creation must place the picker on its selected monitor before allocation");
        const uchar* pixels = backingPixels(*picker);
        const WId nativeId = picker->internalWinId();
        coordinator.prepareColorPickerSurface(displays);
        require(picker->parentWidget() == &secondary && backingPixels(*picker) == pixels,
                "later preparation must keep the invocation monitor's surface");
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        coordinator.updateColorPicker(
            &secondary, image, image.rect(), QPoint(8, 8), secondary.rect().center(),
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
        QApplication::processEvents();
        require(picker->internalWinId() == nativeId && backingPixels(*picker) == pixels,
                "first reveal on the invocation monitor must retain the preallocated pixels");
        coordinator.releaseColorPicker();

        coordinator.createColorPicker(primary.geometry().center());
        coordinator.prepareColorPickerSurface(displays);
        require(coordinator.colorPicker()->parentWidget() == &primary,
                "each new session must use its own invocation monitor");
        coordinator.releaseColorPicker();
        displays.displayAt(1).active = false;
        coordinator.createColorPicker(invocationPosition);
        coordinator.prepareColorPickerSurface(displays);
        require(coordinator.colorPicker()->parentWidget() == &primary,
                "a removed invocation display must fall back to an active overlay");
    }
}

void hiddenPickerSamplesWithoutPreparingPresentation() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    storage.shutdown();
    require(storage.initialize({temporary.filePath(QStringLiteral("bin")), temporary.path(), 60000})
                .success,
            "hidden picker fixture must initialize isolated storage");
    NoopOverlayEventSink sink;
    SnowCanvasRuntime canvas;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow first(sink, new SnowCanvasWidget);
    ScreenshotOverlayWindow second(sink, new SnowCanvasWidget);
    first.setGeometry(0, 0, 80, 60);
    second.setGeometry(80, 0, 80, 60);
    first.show();
    second.show();
    CapturedDisplayModel left;
    left.stableId = QStringLiteral("left");
    left.logicalRect = first.geometry();
    left.physicalRect = left.logicalRect;
    left.active = true;
    left.geometryResolved = true;
    left.image = QImage(80, 60, QImage::Format_RGBA8888);
    left.image.fill(Qt::red);
    auto right = left;
    right.stableId = QStringLiteral("right");
    right.logicalRect = second.geometry();
    right.physicalRect = right.logicalRect;
    right.image = QImage(80, 60, QImage::Format_RGBA8888);
    right.image.fill(Qt::blue);
    right.cursorPatch = QImage(3, 3, QImage::Format_RGBA8888);
    right.cursorPatch.fill(Qt::green);
    right.cursorPixelRect = QRect(19, 19, 3, 3);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(left, &first);
    displays.appendDisplay(right, &second);
    displays.cursorVisible = true;
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    snow_shot::platform::PhysicalCursor cursor(
        {true, [] { return std::optional<QPoint>{}; }, [](const QPoint&) { return true; }});
    ScreenshotOverlayCoordinator coordinator(sink, canvas, shortcuts);
    coordinator.createColorPicker(QPoint(20, 20));
    coordinator.prepareColorPickerSurface(displays);
    auto* picker = coordinator.colorPicker();
    const auto preparedHandle = picker->windowHandle();
    const auto preparedId = picker->internalWinId();
    const QPoint preparedPosition = picker->pos();
    require(preparedId != 0 && picker->parentWidget() == &first,
            "hidden picker fixture must prepare the initial owner's surface");
    picker->resetWorkCounters();
    ScreenshotColorPickerController controller(coordinator, geometry, displays, cursor);
    ScreenshotColorPickerContext context;
    context.active = true;
    context.moveToolActive = true;
    context.intelligentSelecting = true;
    controller.setDisplayMode(ScreenshotColorPickerDisplayMode::AlwaysHide);
    const auto* sampledDisplay = &displays.displayAt(1);
    controller.updateAtPhysicalPoint(QPoint(100, 20), context, sampledDisplay);
    require(controller.copyColorToClipboard(context) &&
                QApplication::clipboard()->text() == QStringLiteral("#00FF00"),
            "hidden sampling must copy the visible captured cursor pixel");
    for (int offset = 0; offset < 16; ++offset)
        controller.updateAtPhysicalPoint(QPoint(104 + offset, 20), context, sampledDisplay);
    require(controller.copyColorToClipboard(context) &&
                QApplication::clipboard()->text() == QStringLiteral("#0000FF"),
            "hidden motion must keep the copied sample current on another display");
    auto counters = picker->workCounters();
    require(counters.samples == 17 && counters.previews == 0 && counters.moves == 0 &&
                counters.ownerChanges == 0 && picker->parentWidget() == &first &&
                picker->windowHandle() == preparedHandle && picker->internalWinId() == preparedId &&
                picker->pos() == preparedPosition && picker->isHidden(),
            "hidden picker motion must sample without preview, positioning or native reparenting");
    require(controller.cycleFormat(context) && controller.copyColorToClipboard(context) &&
                QApplication::clipboard()->text() == QStringLiteral("0000FF"),
            "hidden color format changes must preserve the current sample");
    displays.cursorVisible = false;
    controller.updateAtPhysicalPoint(QPoint(100, 20), context, sampledDisplay);
    require(controller.copyColorToClipboard(context) &&
                QApplication::clipboard()->text() == QStringLiteral("0000FF"),
            "hidden cursor suppression must immediately use the clean capture");
    picker->resetWorkCounters();
    controller.updateAtPhysicalPoint(QPoint(100, 20), context, sampledDisplay);
    require(picker->workCounters().samples == 0,
            "an unchanged hidden sample must reuse the clean capture when the cursor is disabled");
    controller.setDisplayMode(ScreenshotColorPickerDisplayMode::AlwaysShow);
    picker->resetWorkCounters();
    controller.updateAtPhysicalPoint(QPoint(100, 20), context, sampledDisplay);
    counters = picker->workCounters();
    require(counters.samples == 0 && counters.previews == 1,
            "revealing a hidden sample must rebuild the preview once without resampling its color");
    require(counters.ownerChanges == 1 && picker->parentWidget() == &second,
            "revealing a hidden sample must attach the visible owner once");
    require(picker->isVisible(), "revealing a hidden sample must show the picker");
    controller.setDisplayMode(ScreenshotColorPickerDisplayMode::HideOutsideSelection);
    context.intelligentSelecting = false;
    context.manualSelecting = true;
    context.selectionPixels = QRect(80, 0, 10, 10);
    picker->resetWorkCounters();
    controller.updateAtPhysicalPoint(QPoint(110, 20), context, sampledDisplay);
    counters = picker->workCounters();
    require(counters.samples == 1 && counters.previews == 0 && counters.moves == 0 &&
                counters.ownerChanges == 0 && picker->isHidden() &&
                controller.copyColorToClipboard(context),
            "hide-outside-selection must use the same accurate sample-only path");
    controller.setSuppressed(true);
    require(!controller.copyColorToClipboard(context),
            "suppression must continue to block copying hidden samples");
    coordinator.releaseColorPicker();
    storage.shutdown();
}

void startupPickerUsesResolvedOwnerWithoutSamplingNativeCursor() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    storage.shutdown();
    require(storage
                .initialize({QDir(temporary.path()).filePath(QStringLiteral("bin")),
                             temporary.path(), 60000})
                .success,
            "coordinate integration fixture must initialize isolated storage");
    NoopOverlayEventSink sink;
    SnowCanvasRuntime canvas;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow first(sink, new SnowCanvasWidget);
    ScreenshotOverlayWindow second(sink, new SnowCanvasWidget);
    first.setGeometry(0, 0, 80, 60);
    second.setGeometry(80, 0, 80, 60);
    CapturedDisplayModel left;
    left.stableId = QStringLiteral("left");
    left.logicalRect = first.geometry();
    left.physicalRect = left.logicalRect;
    left.active = true;
    left.geometryResolved = true;
    left.image = QImage(80, 60, QImage::Format_RGB32);
    left.image.fill(Qt::red);
    auto right = left;
    right.stableId = QStringLiteral("right");
    right.logicalRect = second.geometry();
    right.physicalRect = right.logicalRect;
    right.image = QImage(80, 60, QImage::Format_RGB32);
    right.image.fill(Qt::blue);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(left, &first);
    displays.appendDisplay(right, &second);
    auto startup = std::make_shared<ScreenshotStartupContext>();
    startup->phase = ScreenshotStartupContext::Phase::Revealed;
    startup->displaySlot = 1;
    startup->displayId = right.stableId;
    startup->logicalPosition = QPoint(100, 20);
    startup->physicalPosition = startup->logicalPosition;
    displays.startup = startup;
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    int reads = 0;
    snow_shot::platform::PhysicalCursor cursor({true,
                                                [&]() -> std::optional<QPoint> {
                                                    ++reads;
                                                    return QPoint(20, 20);
                                                },
                                                [](const QPoint&) { return true; },
                                                [&]() -> std::optional<QPointF> {
                                                    ++reads;
                                                    return QPointF(20, 20);
                                                }});
    ScreenshotOverlayCoordinator coordinator(sink, canvas, shortcuts);
    coordinator.createColorPicker(QPoint(20, 20));
    coordinator.prepareColorPickerSurface(displays);
    ScreenshotColorPickerController controller(coordinator, geometry, displays, cursor);
    ScreenshotColorPickerContext context;
    context.active = true;
    context.moveToolActive = true;
    context.intelligentSelecting = true;
    context.selectionDisplayUnit = ScreenshotSelectionDisplayUnit::PhysicalPixels;
    context.selectionPixels = QRect(90, 10, 30, 30);
    controller.updateAtCurrentCursor(context);
    auto* picker = coordinator.colorPicker();
    require(picker->currentPositionText().simplified() == QStringLiteral("X: 100 Y: 20") &&
                controller.toggleCoordinateMode(context) &&
                picker->currentPositionText().simplified() == QStringLiteral("X: 10 Y: 10"),
            "controller must deliver selection-relative positions and refresh on toggle");
    context.selectionPixels.translate(5, 5);
    controller.updateAtCurrentCursor(context);
    require(picker->currentPositionText().simplified() == QStringLiteral("X: 5 Y: 5"),
            "selection movement must refresh the origin with a stationary sample");
    context.selectionPixels = {};
    controller.updateAtCurrentCursor(context);
    require(picker->currentPositionText().simplified() == QStringLiteral("X: 100 Y: 20"),
            "empty selection must display global coordinates");
    context.selectionPixels = QRect(100, 20, 20, 20);
    controller.updateAtCurrentCursor(context);
    require(picker->currentPositionText().simplified() == QStringLiteral("X: 0 Y: 0"),
            "relative mode must resume when a selection appears");
    controller.setSuppressed(true);
    require(!controller.toggleCoordinateMode(context),
            "suppression must disable coordinate toggle");
    controller.setSuppressed(false);
    context.active = false;
    require(!controller.toggleCoordinateMode(context),
            "inactive capture must disable coordinate toggle");
    context.active = true;
    require(controller.toggleCoordinateMode(context) &&
                picker->currentPositionText().simplified() == QStringLiteral("X: 100 Y: 20"),
            "toggling back must restore desktop coordinates");
    require(reads == 0 && coordinator.colorPicker()->parentWidget() == &second &&
                coordinator.colorPicker()->currentColorText().compare(QStringLiteral("#0000ff"),
                                                                      Qt::CaseInsensitive) == 0,
            "startup picker must share the invocation owner and sample without reading the live "
            "cursor");
    require(startup->suppressesInput(), "reading the gate must keep the invocation anchor");
    startup->resumeLiveInput();
    require(!startup->anchored(), "revealed input must release the cursor anchor");
    controller.updateAtCurrentCursor(context);
    require(reads == 1 && coordinator.colorPicker()->parentWidget() == &first,
            "live picker must sample once and follow the newly selected display");
    coordinator.releaseColorPicker();
    storage.shutdown();
}

void canvasSamplerExcludesScreenshotSelectionUi() {
    NoopOverlayEventSink sink;
    SnowCanvasRuntime runtime;
    ScreenshotOverlayWindow overlay(sink, new SnowCanvasWidget(runtime));
    overlay.setCaptureGeometry(QRect(0, 0, 160, 100));
    overlay.show();
    auto& canvas = *overlay.canvas();
    require(canvas.setViewportCamera(80, 50, 1), "configure the sampling test viewport");
    QImage image(160, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(180, 160, 140));
    overlay.setScreenshotImage(image, image.rect());
    overlay.setScreenshotMaskVisible(true);
    overlay.setScreenshotMaskColor(QColor(0, 0, 0, 180));
    overlay.setScreenshotSelection(QRectF(40, 25, 80, 50), true, 0);
    overlay.setScreenshotSelectionBorderColor(Qt::cyan);
    overlay.setScreenshotSelectionBorderVisible(true);
    require(
        !runtime
             .applyAnnotationTransaction(QByteArrayLiteral(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[65,40,30,20],"style":{"fill":[255,0,0,255]}}]})"))
             .isEmpty(),
        "create an annotation that the eyedropper must preserve");
    const QImage decorated = canvas.grab().toImage();
    require(decorated.pixelColor(10, 50) != image.pixelColor(10, 50),
            "the regression fixture must visibly darken pixels outside the selection");
    require(decorated.pixelColor(40, 50) != image.pixelColor(40, 50),
            "the regression fixture must visibly paint the selection border");
    ScreenshotCanvasColorSampler sampler;
    const QRect physicalBounds(-160, 200, 160, 100);
    require(sampler.ensureContentSnapshot(canvas, physicalBounds),
            "capture screenshot content without selection UI");
    for (const QPoint point : {QPoint(10, 50), QPoint(40, 50), QPoint(45, 50), QPoint(119, 50)}) {
        const QImage preview = sampler.previewAtPhysicalPoint(physicalBounds.topLeft() + point);
        for (int y = 0; y < preview.height(); ++y)
            for (int x = 0; x < preview.width(); ++x)
                require(preview.pixelColor(x, y) == image.pixelColor(point + QPoint(x - 3, y - 3)),
                        "the entire eyedropper preview must exclude selection mask and border");
    }
    require(sampler.previewAtPhysicalPoint(physicalBounds.topLeft() + QPoint(80, 50))
                    .pixelColor(3, 3) == QColor(Qt::red),
            "sampling must retain annotations over the original screenshot");
    require(canvas.grab().toImage() == decorated,
            "sampling must leave the visible selection mask and border intact");
    require(sampler.ensureSnapshot(canvas, physicalBounds) &&
                sampler.previewAtPhysicalPoint(physicalBounds.topLeft() + QPoint(10, 50))
                        .pixelColor(3, 3) == decorated.pixelColor(10, 50),
            "widget sampling must not reuse a screenshot content snapshot");
    require(sampler.ensureContentSnapshot(canvas, physicalBounds) &&
                sampler.previewAtPhysicalPoint(physicalBounds.topLeft() + QPoint(10, 50))
                        .pixelColor(3, 3) == image.pixelColor(10, 50),
            "content sampling must not reuse a decorated widget snapshot");
    require(
        !runtime
             .applyAnnotationTransaction(QByteArrayLiteral(
                 R"({"version":1,"operations":[{"type":"spotlight","bounds":[70,40,20,20],"style":{"color":[0,0,255,255],"opacity":0.5,"corner_radius":0}}]})"))
             .isEmpty(),
        "create a Spotlight effect constrained to the screenshot selection");
    const QImage spotlight = canvas.grab().toImage();
    require(spotlight.pixelColor(55, 50) != image.pixelColor(55, 50),
            "the regression fixture must visibly tint pixels inside the selection");
    sampler.reset();
    require(sampler.ensureContentSnapshot(canvas, physicalBounds) &&
                sampler.previewAtPhysicalPoint(physicalBounds.topLeft() + QPoint(10, 50))
                        .pixelColor(3, 3) == image.pixelColor(10, 50) &&
                sampler.previewAtPhysicalPoint(physicalBounds.topLeft() + QPoint(55, 50))
                        .pixelColor(3, 3) == spotlight.pixelColor(55, 50),
            "sampling must preserve Spotlight's selection-relative bounds and displayed colors");
    require(canvas.grab().toImage() == spotlight,
            "sampling document effects must leave the visible screenshot unchanged");
}

void canvasSamplerFollowsSessionOwner() {
    QWidget overlay(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
    QWidget canvas(&overlay);
    overlay.resize(320, 240);
    overlay.show();
#ifdef Q_OS_MACOS
    snow_shot::platform::configureScreenshotOverlayWindow(&overlay);
#endif
    QWidget pinned(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
    pinned.show();
    ScreenshotCanvasColorSamplerWindow sampler;
    QImage preview(7, 7, QImage::Format_RGB32);
    preview.fill(Qt::red);
    const bool cocoa = QGuiApplication::platformName() == QStringLiteral("cocoa");
    for (QWidget* owner : {&overlay, &pinned, &overlay}) {
        QWidget pickerControl(owner);
        sampler.beginSampling(&pickerControl);
        require(!sampler.isVisible(), "sampling must wait for a valid preview before showing");
        sampler.updateSample(preview, owner->mapToGlobal(QPoint(20, 20)));
        QCoreApplication::processEvents();
        require(sampler.isVisible() &&
                    sampler.windowHandle()->transientParent() == owner->windowHandle(),
                "the sampling HUD must be visible and transient to the current session owner");
        require(sampler.parentWidget() == nullptr && !canvas.testAttribute(Qt::WA_NativeWindow),
                "sampling must preserve controller ownership and non-native canvas input");
        owner->raise();
        QCoreApplication::processEvents();
#ifdef Q_OS_MACOS
        if (cocoa) {
            NSWindow* hud = reinterpret_cast<NSView*>(sampler.winId()).window;
            NSWindow* nativeOwner = reinterpret_cast<NSView*>(owner->winId()).window;
            require(hud.visible && hud.level >= nativeOwner.level,
                    "the sampler must not be hidden below its owner's native level");
            if (owner == &overlay)
                require(hud.level > nativeOwner.level,
                        "the sampler must join the elevated screenshot stacking hierarchy");
            else
                require(hud.level < CGWindowLevelForKey(kCGScreenSaverWindowLevelKey),
                        "pinned sampling must not retain the screenshot's elevated level");
            require(hud.ignoresMouseEvents, "the sampler must not intercept canvas input");
        }
#else
        Q_UNUSED(cocoa);
#endif
        sampler.endSampling();
        require(!sampler.isVisible() &&
                    (!sampler.windowHandle() || !sampler.windowHandle()->transientParent()),
                "ending sampling must hide the HUD and release its transient owner");
    }
}

void exitConfirmationStacksAbovePicker() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary exit confirmation storage available");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated exit confirmation storage");
    {
        QWidget owner(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
        owner.setGeometry(50, 50, 640, 480);
        owner.show();
        ScreenshotColorPickerWindow picker;
        picker.setOwnerWindow(&owner);
        QImage image(owner.size(), QImage::Format_RGB32);
        image.fill(Qt::red);
        picker.setCaptureImage(image, image.rect());

        snow_shot::presentation::WindowShortcutManager shortcuts;
        shortcuts.addScopeWindow(&owner);
        int exits = 0;
        int restores = 0;
        snow_shot::presentation::ScreenshotShortcutExitConfirmation confirmation(
            shortcuts, [&] { ++exits; },
            [&](QWidget* restoredOwner) {
                require(restoredOwner == &owner, "cancel must restore the screenshot owner");
                ++restores;
            });
        for (int cycle = 0; cycle != 3; ++cycle) {
            picker.updatePicker(QPoint(240, 160), QPointF(240, 160), true);
            require(picker.isVisible(), "the magnifier must be visible before confirmation");
            require(confirmation.request(true, &owner), "open exit confirmation over magnifier");
            auto* modal = owner.findChild<adqt::widgets::AdModal*>(
                QStringLiteral("screenshotShortcutExitConfirmation"));
            require(modal && modal->isOpen(), "exit confirmation must open");
            QWidget* surface = modal->acceptButton()->window();
            require(surface != &owner && surface->isWindow() && surface->isVisible() &&
                        surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint) &&
                        surface->windowHandle()->transientParent() == owner.windowHandle(),
                    "confirmation must join the screenshot owner's topmost native window stack");
            require(modal->windowModality() == Qt::WindowModal,
                    "confirmation must block the screenshot owner while open");
            picker.updatePicker(QPoint(241, 161), QPointF(241, 161), true);
            QCoreApplication::processEvents();
#ifdef Q_OS_WIN
            if (QGuiApplication::platformName() == QStringLiteral("windows")) {
                const HWND dialog = reinterpret_cast<HWND>(surface->internalWinId());
                const HWND magnifier = reinterpret_cast<HWND>(picker.internalWinId());
                bool dialogAboveMagnifier = false;
                for (HWND window = GetTopWindow(nullptr); window;
                     window = GetWindow(window, GW_HWNDNEXT)) {
                    if (window == dialog) {
                        dialogAboveMagnifier = true;
                        break;
                    }
                    if (window == magnifier)
                        break;
                }
                require(dialogAboveMagnifier,
                        "exit confirmation must remain above the visible magnifier after updates");
            }
#endif
            if (cycle == 2)
                modal->acceptButton()->click();
            else
                modal->rejectButton()->click();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(exits == (cycle == 2 ? 1 : 0) && restores == std::min(cycle + 1, 2),
                    "reopening confirmation must preserve cancel and exit behavior");
        }
    }
    storage.shutdown();
}

void auxiliaryWindowsPreserveOwnerStacking() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary stacking-test storage available");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated stacking-test storage");
    QWidget owner(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    owner.setObjectName(QStringLiteral("stackingOwner"));
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.setGeometry(50, 50, 500, 400);
    owner.show();
    QWidget toolbar(&owner, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    toolbar.setObjectName(QStringLiteral("stackingToolbar"));
    toolbar.setAttribute(Qt::WA_ShowWithoutActivating);
    toolbar.setGeometry(100, 100, 200, 60);
    toolbar.show();
    QWidget unrelated(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    unrelated.setObjectName(QStringLiteral("stackingUnrelated"));
    unrelated.setAttribute(Qt::WA_ShowWithoutActivating);
    unrelated.setGeometry(600, 50, 100, 100);
    unrelated.show();
    const auto checkCycles = [&]([[maybe_unused]] const char* name,
                                 const std::function<QWidget*()>& reveal,
                                 const std::function<void()>& update,
                                 const std::function<void()>& conceal) {
        for (int cycle = 0; cycle != 3; ++cycle) {
            [[maybe_unused]] const char* stage = "before show";
            unrelated.raise();
            QApplication::processEvents();
#ifdef Q_OS_WIN
            const bool native = QGuiApplication::platformName() == QStringLiteral("windows");
            const auto above = [](QWidget* first, QWidget* second) {
                const HWND a = reinterpret_cast<HWND>(first->internalWinId());
                const HWND b = reinterpret_cast<HWND>(second->internalWinId());
                for (HWND window = GetTopWindow(nullptr); window;
                     window = GetWindow(window, GW_HWNDNEXT)) {
                    if (window == a)
                        return true;
                    if (window == b)
                        return false;
                }
                return false;
            };
            const auto verifyOwner = [&] {
                if (native) {
                    const bool preserved = above(&unrelated, &owner) && above(&unrelated, &toolbar);
                    if (!preserved) {
                        std::cerr << name << " cycle " << cycle << " " << stage << '\n';
                        for (HWND window = GetTopWindow(nullptr); window;
                             window = GetWindow(window, GW_HWNDNEXT)) {
                            if (auto* widget = QWidget::find(reinterpret_cast<WId>(window)))
                                std::cerr << widget->objectName().toStdString()
                                          << " visible=" << IsWindowVisible(window) << '\n';
                        }
                    }
                    require(preserved, "auxiliary show/update/hide must not raise the owner group");
                }
            };
            verifyOwner();
#endif
            QWidget* tool = reveal();
            stage = "after show";
            QApplication::processEvents();
            require(tool && tool->isVisible(), "auxiliary window must remain visible");
#ifdef Q_OS_WIN
            verifyOwner();
            if (native) {
                if (!(above(tool, &toolbar) && above(&unrelated, tool)))
                    std::cerr << name << " cycle " << cycle << " tool ordering after show\n";
                require(above(tool, &toolbar) && above(&unrelated, tool),
                        "auxiliary window must stack above its group, below unrelated topmosts");
            }
#endif
            update();
            tool->raise();
            stage = "after update/raise";
            QApplication::processEvents();
#ifdef Q_OS_WIN
            verifyOwner();
#endif
            conceal();
            stage = "after hide";
            QApplication::processEvents();
#ifdef Q_OS_WIN
            verifyOwner();
#endif
        }
    };

    StyleToolbarCommands commands;
    ScreenshotToolbarWindow drawingToolbar(commands);
    checkCycles(
        "drawing toolbar transient owner",
        [&]() {
            drawingToolbar.restoreNativeSurface();
            drawingToolbar.setTransientOwnerWindow(&owner);
            drawingToolbar.prepareForDisplay();
            drawingToolbar.show();
            return &drawingToolbar;
        },
        [&] {
            drawingToolbar.setTransientOwnerWindow(&owner);
            drawingToolbar.setActiveTool(ScreenshotToolPalette::Tool::Shape);
            drawingToolbar.prepareForDisplay();
        },
        [&] { drawingToolbar.releaseNativeSurface(); });
    checkCycles(
        "drawing toolbar widget owner",
        [&]() {
            drawingToolbar.setOwnerWindow(&owner);
            drawingToolbar.restoreNativeSurface();
            drawingToolbar.prepareForDisplay();
            drawingToolbar.show();
            return &drawingToolbar;
        },
        [&] {
            drawingToolbar.setOwnerWindow(&owner);
            drawingToolbar.moveContentTo(drawingToolbar.contentPosition() + QPoint(1, 1));
        },
        [&] { drawingToolbar.hide(); });
    drawingToolbar.setOwnerWindow(nullptr);

    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::red);
    ScreenshotColorPickerWindow picker;
    picker.setCaptureImage(image, image.rect());
    checkCycles(
        "magnifier",
        [&]() {
            picker.setOwnerWindow(&owner);
            picker.updatePicker(QPoint(8, 8), QPointF(8, 8), true);
            return &picker;
        },
        [&] { picker.updatePicker(QPoint(9, 9), QPointF(20, 20), true); },
        [&] { picker.hidePicker(); });

    ScreenshotCanvasColorSamplerWindow sampler;
    checkCycles(
        "canvas sampler",
        [&]() {
            sampler.beginSampling(&owner);
            sampler.updateSample(image, owner.mapToGlobal(QPoint(20, 20)));
            return &sampler;
        },
        [&] { sampler.updateSample(image, owner.mapToGlobal(QPoint(40, 40))); },
        [&] { sampler.endSampling(); });

    QWidget trigger(&toolbar);
    trigger.setGeometry(20, 10, 40, 30);
    trigger.show();
    adqt::widgets::AdPopover popover;
    popover.setSourceWidget(&trigger);
    popover.setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::QtTool);
    auto* content = new QWidget;
    content->setFixedSize(80, 40);
    popover.setContentWidget(content);
    checkCycles(
        "popover",
        [&]() {
            popover.show();
            return content->window();
        },
        [&] { popover.refreshPopupLayout(); }, [&] { popover.hide(); });
    const auto findSurface = [](const QString& name) -> QWidget* {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == name && widget->isVisible())
                return widget;
        }
        return nullptr;
    };
    adqt::widgets::AdSelect select(&toolbar);
    select.setGeometry(70, 10, 100, 30);
    select.setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::QtTool);
    select.show();
    checkCycles(
        "select",
        [&]() {
            select.showPopup();
            return findSurface(QStringLiteral("adselect-popup"));
        },
        [&] { select.move(select.pos() + QPoint(1, 0)); }, [&] { select.hidePopup(); });

    adqt::widgets::AdTooltip tooltip;
    tooltip.setTargetWidget(&trigger);
    tooltip.setLayerMode(adqt::widgets::AdTooltip::LayerMode::TopLevelTransient);
    tooltip.setTriggers(adqt::widgets::AdTooltip::Trigger::Click);
    tooltip.setText(QStringLiteral("Stacking tooltip"));
    checkCycles(
        "tooltip",
        [&]() {
            tooltip.show();
            return findSurface(QStringLiteral("adtooltip-surface"));
        },
        [&] { trigger.move(trigger.pos() + QPoint(1, 0)); }, [&] { tooltip.hide(); });

    // Isolated busy surfaces are a Windows-only presentation; other platforms render inline.
    adqt::widgets::AdButton button(&toolbar);
    button.setGeometry(20, 10, 100, 30);
    button.setBusyIndicatorPresentation(
        adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
    button.show();
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        checkCycles(
            "busy indicator",
            [&]() {
                button.setBusy(true);
                return button.busyIndicatorSurface();
            },
            [&] { button.move(button.pos() + QPoint(1, 0)); }, [&] { button.setBusy(false); });
    }
    storage.shutdown();
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--exit-confirmation-stacking-only"))) {
        exitConfirmationStacksAbovePicker();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--hidden-picker-only"))) {
        hiddenPickerSamplesWithoutPreparingPresentation();
        return 0;
    }
    const bool overlayRevealOnly =
        application.arguments().contains(QStringLiteral("--overlay-reveal-only"));
    if (overlayRevealOnly ||
        application.arguments().contains(QStringLiteral("--overlay-preparation-only"))) {
        {
            QTemporaryDir directory;
            require(directory.isValid(), "temporary overlay reveal storage available");
            auto& storage = snow_shot::storage::ApplicationStorage::instance();
            require(storage
                        .initialize({directory.filePath(QStringLiteral("bin")),
                                     directory.filePath(QStringLiteral("data")), 60000})
                        .success,
                    "initialize isolated overlay reveal storage");
            overlayRevealGenerationAndFocusRegressions();
            storage.shutdown();
        }
        if (!overlayRevealOnly)
            overlayPreparationCachesContentAndInvalidatesTranslations();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--toolbar-guide-hover-only"))) {
        toolbarHoverKeepsScreenshotGuidesResponsive();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--selection-toolbar-visibility-only"))) {
        selectionToolbarVisibilitySurvivesCapturesAndRestarts();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--toolbar-picker-hover-only"))) {
        toolbarPopoversSuppressPickerAcrossWindowBoundaries();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--style-binding-only"))) {
        screenshotStyleBindingFollowsToolbarAttachment();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--stacking-only"))) {
        auxiliaryWindowsPreserveOwnerStacking();
        return 0;
    }
    canvasSamplingSessionReleasesTargetsAndCommitsAfterCleanup();
    canvasSamplerFollowsSessionOwner();
    canvasSamplerExcludesScreenshotSelectionUi();
    if (application.arguments().contains(QStringLiteral("--canvas-sampler-only")))
        return 0;
    auxiliaryWindowsPreserveOwnerStacking();
    pickerLifetimeFollowsExplicitSessionOperations();
    visibleRecaptureWindowsIncludePicker();
    invocationMonitorOwnsThePreparedSurface();
    hiddenPickerSamplesWithoutPreparingPresentation();
    startupPickerUsesResolvedOwnerWithoutSamplingNativeCursor();
    return 0;
}
