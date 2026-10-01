#include "../src/presentation/recording/recordingaudiogainpopover.h"
#include "widgets/button.h"
#include "widgets/control_scale.h"
#include "widgets/popover.h"
#include "widgets/slider.h"
#include "widgets/tooltip.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFontDatabase>
#include <QDir>
#include <QKeyEvent>
#include <QScreen>
#include <QTest>
#include <QTranslator>
#include <QWindow>

#include <cstdlib>
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

void flush() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

class EventObserver final : public QObject {
  public:
    int layouts = 0;
    int escapes = 0;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::LayoutRequest)
            ++layouts;
        if (event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
            ++escapes;
        return false;
    }
};

void gainEditorsPreferAboveAndAvoidTopEdge() {
    for (const auto source : {RecordingAudioGainPopover::Source::Microphone,
                              RecordingAudioGainPopover::Source::SystemAudio}) {
        QWidget host;
        host.setWindowFlags(Qt::FramelessWindowHint);
        host.resize(360, 120);
        const QRect bounds = host.screen()->availableGeometry();
        host.move(bounds.center() - host.rect().center());
        adqt::widgets::AdButton trigger(&host);
        trigger.setGeometry(164, 44, 32, 32);
        RecordingAudioGainPopover popup(&trigger, source);
        popup.setRetainNativeSurfaceOnHide(true);
        host.show();
        popup.openAndFocus();
        flush();
        QWidget* content = popup.popover()->contentWidget();
        require(content && popup.popover()->surfaceWidget()->isVisible(),
                "gain placement fixture opens a visible editor");
        require(content->mapToGlobal(content->rect().bottomLeft()).y() <
                    trigger.mapToGlobal(QPoint(0, 0)).y(),
                "microphone and system audio gain editors open above their triggers by default");

        popup.close();
        host.move(host.x(), bounds.top());
        trigger.move(trigger.x(), 0);
        flush();
        popup.openAndFocus();
        flush();
        const QRect contentRect(content->mapToGlobal(QPoint(0, 0)), content->size());
        require(contentRect.top() > trigger.mapToGlobal(trigger.rect().bottomLeft()).y() &&
                    bounds.contains(contentRect),
                "gain editors fall back below a top-edge trigger to stay on screen");
        popup.close();
    }
}

void gainAndMeterAreIndependent() {
    QWidget host;
    host.setFocusPolicy(Qt::StrongFocus);
    host.resize(460, 200);
    adqt::widgets::AdButton trigger(&host);
    trigger.setGeometry(40, 50, 30, 30);
    RecordingAudioGainPopover popup(&trigger, RecordingAudioGainPopover::Source::Microphone);
    host.setFocus();
    require(popup.gainDb() == 0 && !popup.popover()->contentWidget(),
            "gain defaults to zero and editor is lazy");
    host.show();
    flush();
    int gains = 0;
    int opens = 0;
    QObject::connect(&popup, &RecordingAudioGainPopover::gainChanged, [&](int) { ++gains; });
    QObject::connect(&popup, &RecordingAudioGainPopover::visibleChanged, [&](bool visible) {
        if (visible)
            ++opens;
    });
    popup.setRetainNativeSurfaceOnHide(true);
    QWidget* surface = popup.prepareSurface();
    require(surface && !surface->isVisible() && opens == 0,
            "preparation stays hidden without starting preview");
    QWidget* content = popup.popover()->contentWidget();
    auto* slider =
        content->findChild<adqt::widgets::AdSlider*>(QStringLiteral("recordingAudioGainSlider"));
    require(slider &&
                content->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly).size() ==
                    1 &&
                popup.popover()->title().isEmpty(),
            "gain editor displays only one slider without a title or readouts");
    require(content->width() == 180 && slider->marks().size() == 1 &&
                slider->marks().contains(0.0) && !slider->marks().value(0.0).labelVisible &&
                slider->marks().value(0.0).label.isEmpty() && slider->markIndicatorsVisible(),
            "compact gain editor marks zero decibels without a text label");
    require(slider->minimum() == -24 && slider->maximum() == 24 && slider->value() == 0 &&
                slider->singleStep() == 1 && slider->pageStep() == 6,
            "gain range and midpoint are deterministic");
    require(content->findChild<adqt::widgets::AdControlScaleScope*>(),
            "popup has independent scale scope");
    popup.setGainDb(6);
    require(gains == 0 && slider->value() == 6 &&
                slider->tooltipFormatter()(6) == QStringLiteral("+6 dB"),
            "external gain synchronization never publishes user edits");
    require(slider->tooltipFormatter()(0) == QStringLiteral("0 dB") &&
                slider->tooltipFormatter()(-24) == QStringLiteral("-24 dB") &&
                slider->tooltipFormatter()(24) == QStringLiteral("+24 dB"),
            "gain tooltip formats unity and signed range endpoints in decibels");
    const QSize size = content->sizeHint();
    const auto liveTrackColor = slider->semanticStyles().track.backgroundColor;
    popup.setLevel(0.72, false);
    require(slider->trackFillRatio() == 0.72 && slider->value() == 6 &&
                slider->tooltipFormatter()(6) == QStringLiteral("+6 dB"),
            "processed level updates the track while the thumb tooltip describes gain");
    popup.setLevel(1.0, true);
    require(slider->trackFillRatio() == 1.0 &&
                slider->semanticStyles().track.backgroundColor != liveTrackColor &&
                slider->tooltipFormatter()(6) == QStringLiteral("+6 dB"),
            "clipping changes the track color without replacing the gain tooltip");
    require(content->sizeHint() == size, "level ticks do not resize popup");
    slider->setValue(-5);
    require(popup.gainDb() == -5 && gains == 1 &&
                slider->tooltipFormatter()(-5) == QStringLiteral("-5 dB"),
            "slider gain changes publish once and display negative decibels");
    popup.setAudioEnabled(false);
    require(slider->value() == -5 && slider->isEnabled() && slider->trackFillRatio() == 0 &&
                slider->semanticStyles().track.backgroundColor == liveTrackColor &&
                slider->tooltipFormatter()(-5) == QStringLiteral("-5 dB"),
            "idle disabled source permits gain editing without fabricated audio");
    popup.setAudioEnabled(true);
    for (auto status : {RecordingAudioGainPopover::LevelStatus::AudioOff,
                        RecordingAudioGainPopover::LevelStatus::Unavailable,
                        RecordingAudioGainPopover::LevelStatus::Starting,
                        RecordingAudioGainPopover::LevelStatus::PermissionRequired}) {
        popup.setLevel(0.8, false, status);
        require(slider->trackFillRatio() == 0 && slider->value() == -5 &&
                    slider->tooltipFormatter()(-5) == QStringLiteral("-5 dB"),
                "nonlive source clears its meter without changing gain or its tooltip");
    }
    popup.setAvailable(false);
    popup.openAndFocus();
    require(!popup.popover()->isVisible(), "unavailable source cannot open");
}

void hoverDragAndDeactivateKeepInteractionSafe(QApplication& app) {
    EventObserver escapeObserver;
    app.installEventFilter(&escapeObserver);
    QWidget host;
    host.setFocusPolicy(Qt::StrongFocus);
    host.resize(700, 500);
    adqt::widgets::AdButton trigger(&host);
    trigger.setGeometry(40, 50, 30, 30);
    RecordingAudioGainPopover popup(&trigger, RecordingAudioGainPopover::Source::Microphone);
    host.show();
    host.activateWindow();
    host.setFocus();
    QTest::mouseMove(&host, {650, 450});
    flush();
    require(popup.popover()->hoverOpenDelayMs() == 150 &&
                popup.popover()->hoverCloseDelayMs() == 250,
            "gain hover uses deliberate opening and bridge delays");
    QTest::mouseMove(&trigger, trigger.rect().center());
    QTest::qWait(60);
    require(!popup.popover()->isVisible(), "brief hover does not open the gain editor");
    QTest::qWait(150);
    require(popup.popover()->isVisible(), "sustained source hover opens the gain editor");
    // The offscreen integration does not implement nonactivating tool windows.
    // Keep the hover-only fixture's focus in its owner, as the native layer does.
    host.activateWindow();
    host.setFocus();
    QTest::mouseMove(&host, {650, 450});
    QTest::qWait(320);
    require(!popup.popover()->isVisible(), "unfocused hover editor closes after leaving");

    popup.openAndFocus();
    flush();
    QWidget* content = popup.popover()->contentWidget();
    auto* slider = content->findChild<adqt::widgets::AdSlider*>();
    require(slider && slider->hasFocus(), "keyboard gain editor focuses the thumb");
    EventObserver meterObserver;
    content->installEventFilter(&meterObserver);
    slider->installEventFilter(&meterObserver);
    flush();
    meterObserver.layouts = 0;
    const QSize originalSize = popup.popover()->surfaceWidget()->size();
    for (int percentage : {1, 24, 49, 72, 99}) {
        popup.setLevel(percentage / 100.0, false);
        flush();
    }
    popup.setLevel(1.0, true);
    flush();
    popup.setLevel(0.5, false);
    flush();
    require(meterObserver.layouts == 0 && popup.popover()->surfaceWidget()->size() == originalSize,
            "processed level ticks never relayout the editor or slider");

    const QPoint thumb(slider->width() / 2, 16);
    QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, thumb);
    flush();
    auto* tooltip = slider->findChild<adqt::widgets::AdTooltip*>();
    require(slider->sliderDown() && tooltip && tooltip->isVisible() &&
                tooltip->text() == QStringLiteral("0 dB"),
            "pressing the thumb opens its gain tooltip");
    QTest::mouseMove(slider, {slider->width() + 80, 16});
    QTest::qWait(320);
    require(popup.popover()->isVisible() && popup.gainDb() == 24 && tooltip->isVisible() &&
                tooltip->text() == QStringLiteral("+24 dB"),
            "dragging beyond the editor preserves editing and updates the gain tooltip");
    popup.setLevel(0.37, true);
    flush();
    require(tooltip->text() == QStringLiteral("+24 dB"),
            "live audio updates during dragging do not replace the gain tooltip");
    QTest::mouseMove(slider, {0, 16});
    flush();
    require(popup.gainDb() == -24 && tooltip->isVisible() &&
                tooltip->text() == QStringLiteral("-24 dB"),
            "dragging to the minimum shows negative gain");
    QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, {slider->width() + 80, 16});
    QTest::qWait(320);
    require(popup.popover()->isVisible() && slider->hasFocus(),
            "focused gain editing remains open after pointer departure");
    QTest::keyClick(slider, Qt::Key_Escape);
    flush();
    require(!popup.popover()->isVisible() && escapeObserver.escapes == 0,
            "editor Escape is consumed before recording's application handler");
    QTest::keyClick(&trigger, Qt::Key_Escape);
    require(escapeObserver.escapes > 0 && !popup.popover()->isVisible(),
            "closed audio trigger leaves Escape available to recording shortcuts");
    popup.openAndFocus();
    flush();
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QApplication::sendEvent(&app, &deactivate);
    flush();
    require(!popup.popover()->isVisible(), "application deactivation closes focused editing");
    app.removeEventFilter(&escapeObserver);
}

void pointerEditingClearsKeyboardFocusFeedback() {
    bool focusFeedback = false;
    QWidget host;
    host.resize(700, 500);
    adqt::widgets::AdButton trigger(&host);
    trigger.setGeometry(40, 50, 30, 30);
    RecordingAudioGainPopover popup(&trigger, RecordingAudioGainPopover::Source::Microphone);
    host.show();
    popup.openAndFocus();
    flush();
    auto* slider = popup.popover()->contentWidget()->findChild<adqt::widgets::AdSlider*>();
    QWidget* surface = popup.popover()->surfaceWidget();
    QWindow* window = surface->windowHandle();
    require(slider && window && slider->hasFocus(), "gain editor focuses its slider");
    slider->setSemanticStyleResolver([&](const adqt::widgets::AdSlider::StyleContext& context) {
        focusFeedback = context.focused;
        return adqt::widgets::AdSlider::SemanticStyles{};
    });
    const auto windowPoint = [&](QPoint point) { return slider->mapTo(surface, point); };
    const QPoint midpoint = windowPoint(slider->rect().center());
    const QPoint nearMidpoint = midpoint - QPoint(3, 0);
    const QPoint away = windowPoint({slider->width() - 1, slider->height() / 2});
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, midpoint);
    QTest::mouseMove(window, nearMidpoint);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, nearMidpoint);
    flush();
    auto* tooltip = slider->findChild<adqt::widgets::AdTooltip*>();
    const int releasedGain = popup.gainDb();
    require(!slider->sliderDown() && tooltip && tooltip->isVisible() && !focusFeedback,
            "releasing on the thumb retains only its ordinary hover tooltip");
    QTest::mouseMove(window, away);
    flush();
    require(!tooltip->isVisible() && !focusFeedback && popup.gainDb() == releasedGain &&
                slider->hasFocus(),
            "pointer departure hides the tooltip and focus ring while retaining keyboard focus");
    QTest::keyClick(slider, Qt::Key_End);
    flush();
    require(tooltip->isVisible() && focusFeedback && popup.gainDb() == 24,
            "keyboard editing after pointer editing restores focus feedback and its tooltip");
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, midpoint);
    QTest::mouseMove(window, away);
    flush();
    require(!tooltip->isVisible() && !focusFeedback && !slider->sliderDown(),
            "clicking an already-focused keyboard slider switches back to pointer feedback");
    popup.setGainDb(24);
    QTest::keyClick(slider, Qt::Key_End);
    flush();
    require(tooltip->isVisible() && focusFeedback && popup.gainDb() == 24,
            "keyboard focus feedback returns even when a boundary key leaves gain unchanged");
}

#ifdef Q_OS_WIN
void nativePointerEditingClearsTooltip() {
    const QPoint originalCursor = QCursor::pos();
    bool focusFeedback = false;
    QWidget host;
    host.setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
    host.resize(640, 320);
    host.move(host.screen()->availableGeometry().center() - host.rect().center());
    adqt::widgets::AdButton trigger(&host);
    trigger.setGeometry(300, 160, 32, 32);
    RecordingAudioGainPopover popup(&trigger, RecordingAudioGainPopover::Source::Microphone);
    host.show();
    host.activateWindow();
    popup.openAndFocus();
    QTest::qWait(100);
    auto* slider = popup.popover()->contentWidget()->findChild<adqt::widgets::AdSlider*>();
    require(slider && slider->hasFocus(), "native gain editor focuses its slider");
    slider->setSemanticStyleResolver([&](const adqt::widgets::AdSlider::StyleContext& context) {
        focusFeedback = context.focused;
        return adqt::widgets::AdSlider::SemanticStyles{};
    });
    const auto sendButton = [](bool down) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        require(SendInput(1, &input, sizeof(input)) == 1, "native mouse injection succeeds");
    };
    const QPoint thumb = slider->mapToGlobal(slider->rect().center());
    QCursor::setPos(thumb);
    QTest::qWait(30);
    sendButton(true);
    QTest::qWait(30);
    const bool started = slider->sliderDown();
    QCursor::setPos(thumb - QPoint(3, 0));
    QTest::qWait(30);
    sendButton(false);
    QTest::qWait(30);
    const int gain = popup.gainDb();
    QCursor::setPos(slider->mapToGlobal(QPoint(slider->width() - 1, slider->height() / 2)));
    QTest::qWait(60);
    auto* tooltip = slider->findChild<adqt::widgets::AdTooltip*>();
    const bool ended = !slider->sliderDown() && tooltip && !tooltip->isVisible() &&
                       !focusFeedback && popup.gainDb() == gain;
    QCursor::setPos(originalCursor);
    require(started, "native mouse press starts gain dragging");
    require(ended, "native release and pointer departure clear dragging, tooltip and focus ring");
}
#endif

void keyboardAndDeferredShowAreSafe() {
    QWidget host;
    host.setFocusPolicy(Qt::StrongFocus);
    host.resize(460, 200);
    adqt::widgets::AdButton trigger(&host);
    trigger.setGeometry(40, 50, 30, 30);
    RecordingAudioGainPopover popup(&trigger, RecordingAudioGainPopover::Source::SystemAudio);
    host.setFocus();
    host.show();
    flush();
    bool allow = false;
    int surfaceOpens = 0;
    int surfaceCloses = 0;
    QObject::connect(&popup, &RecordingAudioGainPopover::surfaceVisibilityChanged,
                     [&](bool visible) { visible ? ++surfaceOpens : ++surfaceCloses; });
    popup.setSurfaceShowGuard([&allow](QWidget*) { return allow; });
    popup.openAndFocus();
    require(popup.popover()->isVisible() && !popup.popover()->surfaceWidget()->isVisible(),
            "gain editor waits for surface acknowledgment");
    require(surfaceOpens == 0, "guard waiting does not start the visible meter lifecycle");
    allow = true;
    popup.popover()->refreshPopupLayout();
    flush();
    auto* slider = popup.popover()->contentWidget()->findChild<adqt::widgets::AdSlider*>();
    require(slider && slider->hasFocus(),
            "deferred keyboard opening eventually focuses gain thumb");
    require(surfaceOpens == 1, "acknowledged surface starts its meter lifecycle once");
    QKeyEvent overrideEscape(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
    overrideEscape.setAccepted(false);
    QApplication::sendEvent(&host, &overrideEscape);
    require(overrideEscape.isAccepted() && popup.popover()->isVisible(),
            "Escape shortcut override reserves the key without closing early");
    QKeyEvent overrideArrow(QEvent::ShortcutOverride, Qt::Key_Right, Qt::NoModifier);
    overrideArrow.setAccepted(false);
    QApplication::sendEvent(slider, &overrideArrow);
    require(overrideArrow.isAccepted() && popup.gainDb() == 0,
            "editing shortcut override reserves navigation without applying gain twice");
    QTest::keyClick(slider, Qt::Key_Right);
    require(popup.gainDb() == 1, "arrow adjusts one decibel");
    QTest::keyClick(slider, Qt::Key_PageUp);
    require(popup.gainDb() == 7, "page key adjusts six decibels");
    QTest::keyClick(slider, Qt::Key_Home);
    require(popup.gainDb() == -24, "Home reaches minimum");
    QTest::keyClick(slider, Qt::Key_End);
    require(popup.gainDb() == 24, "End reaches maximum");
    QTest::keyClick(slider, Qt::Key_Escape);
    flush();
    require(!popup.popover()->isVisible(), "Escape closes without Focus reopening");
    require(surfaceCloses == 1, "closing stops the actual surface meter lifecycle once");
    QTest::keyClick(&trigger, Qt::Key_Down, Qt::AltModifier);
    flush();
    require(popup.popover()->isVisible() && slider->hasFocus(),
            "Alt Down reopens and focuses gain");
    popup.close();
    popup.popover()->refreshPopupLayout();
    require(!popup.popover()->isVisible(), "late acknowledgment cannot reopen a cancelled popup");
}

class Translator final : public QTranslator {
  public:
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) == "RecordingAudioGainPopover")
            return QStringLiteral("Localized %1").arg(QString::fromUtf8(source));
        return {};
    }
};

void openEditorRetranslatesWithoutResettingGain(QApplication& app) {
    QWidget host;
    adqt::widgets::AdButton trigger(&host);
    RecordingAudioGainPopover popup(&trigger, RecordingAudioGainPopover::Source::Microphone);
    popup.prepareSurface();
    popup.setGainDb(-9);
    Translator translator;
    app.installTranslator(&translator);
    QEvent language(QEvent::LanguageChange);
    QApplication::sendEvent(&trigger, &language);
    auto* slider = popup.popover()->contentWidget()->findChild<adqt::widgets::AdSlider*>();
    require(slider->accessibleName() == QStringLiteral("Localized Microphone gain") &&
                slider->tooltipFormatter()(-9) == QStringLiteral("Localized -9 dB") &&
                popup.gainDb() == -9,
            "retained slider and gain tooltip retranslate without resetting gain");
    app.removeTranslator(&translator);
    QApplication::sendEvent(&trigger, &language);
    require(slider->accessibleName() == QStringLiteral("Microphone gain") &&
                slider->tooltipFormatter()(-9) == QStringLiteral("-9 dB"),
            "slider and tooltip restore the source language");
}

void displayTopologyClosesBeforeNativeReroute(QApplication& app) {
    QWidget host;
    host.setFocusPolicy(Qt::StrongFocus);
    host.resize(460, 200);
    adqt::widgets::AdButton trigger(&host);
    trigger.setGeometry(40, 50, 30, 30);
    RecordingAudioGainPopover popup(&trigger, RecordingAudioGainPopover::Source::SystemAudio);
    popup.setRetainNativeSurfaceOnHide(true);
    host.setFocus();
    host.show();
    popup.openAndFocus();
    flush();
    QWidget* surface = popup.popover()->surfaceWidget();
    require(surface && surface->isVisible() && surface->windowHandle(),
            "topology fixture starts with a visible native tool surface");
    require(QMetaObject::invokeMethod(&app, "screenRemoved", Qt::DirectConnection,
                                      Q_ARG(QScreen*, app.primaryScreen())),
            "screen removal signal can be delivered deterministically");
    require(!popup.popover()->isVisible() && !surface->isVisible(),
            "screen removal closes requested and actual visibility before Qt reroutes windows");
    // Match Qt's screen-removal ordering: snapshot visibility after the signal,
    // move the surviving native window, then restore that snapshotted visibility.
    const bool wasVisible = surface->windowHandle()->isVisible();
    surface->setScreen(app.primaryScreen());
    surface->windowHandle()->setVisible(wasVisible);
    flush();
    require(!surface->isVisible(), "native reroute cannot re-show an unacknowledged surface");

    bool allow = false;
    popup.setSurfaceShowGuard([&allow](QWidget*) { return allow; });
    popup.openAndFocus();
    require(popup.popover()->isVisible() && !surface->isVisible(),
            "pending filter request stays hidden before topology changes");
    require(QMetaObject::invokeMethod(&app, "screenAdded", Qt::DirectConnection,
                                      Q_ARG(QScreen*, app.primaryScreen())),
            "screen addition signal can be delivered deterministically");
    allow = true;
    popup.popover()->refreshPopupLayout();
    require(!popup.popover()->isVisible() && !surface->isVisible(),
            "late exclusion acknowledgment cannot reopen a topology-cancelled request");
}

void renderSnapshots(const QString& directory) {
    require(QDir().mkpath(directory), "snapshot output directory is writable");
    QWidget host;
    host.resize(540, 300);
    adqt::widgets::AdButton trigger(&host);
    trigger.setGeometry(160, 40, 32, 32);
    RecordingAudioGainPopover popup(&trigger, RecordingAudioGainPopover::Source::Microphone);
    host.show();
    popup.setGainDb(9);
    popup.setLevel(0.72, false);
    popup.openAndFocus();
    flush();
    auto save = [&](const QString& filename) {
        popup.popover()->refreshPopupLayout();
        flush();
        require(popup.popover()->surfaceWidget()->grab().save(QDir(directory).filePath(filename)),
                "popover visual artifact is saved");
    };
    save(QStringLiteral("gain-live.png"));
    popup.setGainDb(24);
    popup.setLevel(1.0, true);
    save(QStringLiteral("gain-clipping.png"));
    popup.popover()->contentWidget()->setLayoutDirection(Qt::RightToLeft);
    popup.setGainDb(-12);
    popup.setLevel(0.38, false);
    save(QStringLiteral("gain-rtl.png"));
    popup.setLevel(0, false, RecordingAudioGainPopover::LevelStatus::PermissionRequired);
    save(QStringLiteral("gain-permission.png"));
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    const QString font =
        QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/segoeui.ttf"));
    require(QFontDatabase::addApplicationFont(font) >= 0,
            "offscreen UI uses an installed text font");
    app.setFont(QFont(QStringLiteral("Segoe UI"), 10));
#endif
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--native-pointer-editing-only"))) {
        nativePointerEditingClearsTooltip();
        return 0;
    }
#endif
    if (app.arguments().contains(QStringLiteral("--pointer-feedback-only"))) {
        pointerEditingClearsKeyboardFocusFeedback();
        return 0;
    }
    gainEditorsPreferAboveAndAvoidTopEdge();
    gainAndMeterAreIndependent();
    pointerEditingClearsKeyboardFocusFeedback();
    keyboardAndDeferredShowAreSafe();
    hoverDragAndDeactivateKeepInteractionSafe(app);
    openEditorRetranslatesWithoutResettingGain(app);
    displayTopologyClosesBeforeNativeReroute(app);
    const int snapshot = app.arguments().indexOf(QStringLiteral("--snapshot-dir"));
    if (snapshot >= 0 && snapshot + 1 < app.arguments().size())
        renderSnapshots(app.arguments().at(snapshot + 1));
    return 0;
}
