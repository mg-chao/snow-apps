#include "recording_effect_test_source.h"
#include "../src/presentation/recording/recordingeffectstyle.h"
#include "../src/presentation/recording/recordingeffectgeometry.h"
#ifdef SNOW_RECORDING_EFFECTS_BENCHMARK
#include "recording_effects_performance_benchmark.h"
#endif
#include "snow_shot/presentation/canvasstatusreadout.h"
#include <QDialog>
#include <QPainter>
#include <QLineF>
#include <QTranslator>
#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_capture.h"
#include "widgets/button.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QMouseEvent>
#include <QWindow>
#include <QScreen>
#include <QPointer>
#include <QTimer>
#include <QMessageBox>
#include "widgets/color_picker.h"
#include <future>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
int recordingToolbarAcrossNativeDisplays(bool startCapture);
#pragma push_macro("snow_capture_last_error_message")
#undef snow_capture_last_error_message
extern "C" const char* snow_capture_last_error_message();
const char* nativeCaptureError() {
    return snow_capture_last_error_message();
}
#pragma pop_macro("snow_capture_last_error_message")
#endif
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

struct SnowCaptureRecordingSessionImpl {};
namespace {
std::vector<std::weak_ptr<RecordingEffectTestState>> effectSources;
std::unique_ptr<RecordingEffectsSource> testEffectsSource() {
    auto state = std::make_shared<RecordingEffectTestState>();
    effectSources.push_back(state);
    return std::make_unique<RecordingEffectTestSource>(std::move(state));
}

SnowCaptureRecordingSession session;
int starts = 0;
std::atomic<int> exports = 0;
std::shared_future<void> exportGate;
std::promise<void>* exportEntered = nullptr;
std::atomic<bool> failExport = false;
bool failStart = false;
std::atomic<int> destroyedSessions = 0;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
ScreenshotToolPalette* palette() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(widget);
            toolbar != nullptr && toolbar->isVisible()) {
            return toolbar->palette();
        }
    }
    require(false, "recording toolbar must exist");
    return nullptr;
}

class ErrorObserver final : public QObject {
  public:
    int shown = 0;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            if (auto* dialog = qobject_cast<QMessageBox*>(watched)) {
                ++shown;
                QTimer::singleShot(0, dialog, &QMessageBox::accept);
            }
        }
        return false;
    }
};

void waitForIdle(ScreenRecordingController& controller) {
    QElapsedTimer deadline;
    deadline.start();
    while (controller.isRecording() && deadline.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 100);
    }
    require(!controller.isRecording(), "controlled finalization must return to idle");
}

int recordingWindowCount() {
    int count = 0;
    for (auto* widget : QApplication::topLevelWidgets()) {
        count += qobject_cast<ScreenRecordingAreaWindow*>(widget) != nullptr ||
                 qobject_cast<ScreenRecordingToolbarWindow*>(widget) != nullptr;
    }
    return count;
}

void closeAndStopHaveIndependentUiLifetimes() {
    ErrorObserver errors;
    qApp->installEventFilter(&errors);
    for (const bool close : {false, true}) {
        for (const bool failure : {false, true}) {
            ScreenRecordingController controller(testEffectsSource);
            require(recordingWindowCount() == 0,
                    "constructing a controller must not create windows");
            controller.open({40, 40, 320, 240});
            auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
            QPointer<ScreenRecordingToolbarWindow> previousToolbar(toolbar);
            std::promise<void> release;
            std::promise<void> entered;
            auto enteredFuture = entered.get_future();
            exportGate = release.get_future().share();
            exportEntered = &entered;
            failExport = failure;
            const int previousDestroyed = destroyedSessions;
            const int previousErrors = errors.shown;
            controller.startRecording();
            QCoreApplication::processEvents();
            require(controller.isRecording(), "fake backend must start");
            if (close) {
                // Exercise the native close path as well as the toolbar command.
                toolbar->close();
            } else {
                palette()->recordingStopRequested();
            }
            require(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                    "stop must reach the controlled backend");
            require(controller.isOpen() != close, "only Close must detach the UI during export");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(previousToolbar.isNull() == close && recordingWindowCount() == (close ? 0 : 2),
                    "Close must destroy UI before backend finalization, while Stop retains it");
            controller.open({80, 80, 320, 240});
            require(recordingWindowCount() == (close ? 0 : 2),
                    "busy finalization must reject reopen");
            release.set_value();
            waitForIdle(controller);
            require(destroyedSessions == previousDestroyed + 1,
                    "backend must be destroyed exactly once");
            require(errors.shown == previousErrors + (failure ? 1 : 0),
                    "export failure must be reported even after Close");
            require(recordingWindowCount() == (close ? 0 : 2),
                    "completion must not recreate closed UI");
            if (!close) {
                require(previousToolbar && previousToolbar->isVisible(),
                        "Stop must keep the original UI usable");
                palette()->recordingCloseRequested();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            exportEntered = nullptr;
            exportGate = {};
            failExport = false;
        }
    }
    qApp->removeEventFilter(&errors);
}

void requireToolbarAboveArea(ScreenRecordingAreaWindow* area) {
    auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
    require(toolbar != nullptr, "recording palette must belong to the toolbar window");
    const auto previousInputMode = area->inputMode();
    area->setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    toolbar->move(area->geometry().topLeft());
    area->raise();
    area->activateWindow();
    QCoreApplication::processEvents();
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const HWND areaHandle = reinterpret_cast<HWND>(area->winId());
        const HWND toolbarHandle = reinterpret_cast<HWND>(toolbar->winId());
        SetActiveWindow(areaHandle);
        QCoreApplication::processEvents();
        require(GetActiveWindow() == areaHandle,
                "the recording area must retain focus while the toolbar stays above it");
        require(GetWindow(toolbarHandle, GW_OWNER) == areaHandle,
                "the native recording toolbar must be owned by the area");
        bool toolbarAboveArea = false;
        for (HWND candidate = GetWindow(areaHandle, GW_HWNDPREV); candidate != nullptr;
             candidate = GetWindow(candidate, GW_HWNDPREV)) {
            toolbarAboveArea |= candidate == toolbarHandle;
        }
        require(toolbarAboveArea,
                "activating the overlapping recording area must keep the toolbar above it");
    }
#endif
    require(toolbar->windowHandle()->transientParent() == area->windowHandle(),
            "recording toolbar must retain the area as its transient owner");
    area->setInputMode(previousInputMode);
}

void recordingToolbarReconcilesFrameBeforeShowing() {
    ScreenRecordingToolbarWindow toolbar;
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "placement test requires a screen");
    const QRect region = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    toolbar.placeForPhysicalRegion(region);
    toolbar.showAndActivate();
    QCoreApplication::processEvents();
    toolbar.hide();
    const QSize expected = toolbar.windowSizeHint();
    const QPoint anchor = toolbar.contentPosition();
    // Model Windows retaining the old physical frame after a DPI transition,
    // while the host already contains the destination display's logical layout.
    toolbar.resize(expected.width() / 2, expected.height());
    toolbar.showAndActivate();
    require(toolbar.size() == expected &&
                toolbar.rect().contains(toolbar.paletteHost()->geometry()),
            "showing recording controls must reconcile the frame before painting");
    require(toolbar.contentPosition() == anchor,
            "reconciling the recording frame must preserve its content anchor");
    toolbar.beginRegionInteraction();
    toolbar.resize(expected.width() / 2, expected.height());
    toolbar.endRegionInteraction(region);
    require(toolbar.size() == expected &&
                toolbar.rect().contains(toolbar.paletteHost()->geometry()),
            "restoring recording controls after area interaction must reconcile the frame");
}

void recordingToolbarPlacementAcrossDisplays() {
    ScreenRecordingAreaWindow area;
    ScreenRecordingToolbarWindow toolbar;
    toolbar.setTransientOwnerWindow(&area);
    QRegion desktop;
    for (QScreen* screen : QGuiApplication::screens()) {
        desktop += ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    }
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
        for (int offset : {-bounds.width() / 3, 0, bounds.width() / 3}) {
            for (int height : {100, bounds.height() - 80}) {
                const QRect region(bounds.left() + offset, bounds.top() + 40, bounds.width() / 2,
                                   height);
                // Only selections within the captured desktop can originate in the UI.
                if (!QRegion(region).subtracted(desktop).isEmpty()) {
                    continue;
                }
                area.setPhysicalRegion(region);
                toolbar.placeForPhysicalRegion(region);
                area.show();
                toolbar.showAndActivate();
                for (int pass = 0; pass < 5; ++pass) {
                    QCoreApplication::processEvents();
                }
                require(toolbar.size() == toolbar.windowSizeHint() &&
                            toolbar.rect().contains(toolbar.paletteHost()->geometry()),
                        "cross-display placement must reconcile the frame before opening panels");
                toolbar.palette()->setActiveTool(ScreenshotToolPalette::Tool::Shape);
                QCoreApplication::processEvents();
                const QPoint position = toolbar.contentPosition();
                const QRect content = toolbar.occupiedContentRect();
                const QRect visible = content.translated(position);
                QScreen* target = ScreenshotGeometryMapper::screenForPhysicalRect(region);
                const QRect logicalBounds = target->geometry();
                require(
                    visible.top() >= logicalBounds.top() &&
                        visible.bottom() <= logicalBounds.bottom(),
                    "recording rows must fit the selected display after cross-display placement");
                if (visible.width() <= logicalBounds.width()) {
                    require(logicalBounds.contains(visible),
                            "recording rows must remain horizontally inside the selected display");
                }
                require(toolbar.rect().contains(
                            content.translated(toolbar.contentPosition() - toolbar.pos())),
                        "recording rows must fit the native frame after cross-display placement");
                toolbar.placeForPhysicalRegion(region);
                QCoreApplication::processEvents();
                require(
                    toolbar.contentPosition() == position &&
                        toolbar.occupiedContentRect() == content,
                    "repeated cross-display placement must keep the committed layout and anchor");
                toolbar.palette()->clearActiveTool();
            }
        }
    }
}

void recordingSecondaryPanelsStayOnScreen() {
    ScreenRecordingToolbarWindow toolbar;
    auto* exportButton = toolbar.palette()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenRecordingExportSettings"));
    require(exportButton != nullptr, "recording toolbar must expose export settings");
    if (toolbar.palette()->recordingExportSettingsVisible()) {
        exportButton->click();
    }
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "placement test requires a screen");
    const QRect bounds = screen->geometry();
    const QRect physicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const qreal dpr = screen->devicePixelRatio();
    const int mainHeight = toolbar.occupiedContentRect().height();
    const QRect region(physicalBounds.left() + qRound(40 * dpr),
                       physicalBounds.top() + qRound(40 * dpr), qRound((bounds.width() - 80) * dpr),
                       qRound((bounds.height() - mainHeight - 60) * dpr));
    toolbar.placeForPhysicalRegion(region);
    toolbar.show();
    QCoreApplication::processEvents();
    const auto requireFits = [&](const char* message) {
        const QRect occupied = toolbar.occupiedContentRect().translated(toolbar.contentPosition());
        require(occupied.top() >= bounds.top() && occupied.bottom() <= bounds.bottom(), message);
        // The default offscreen screen is narrower than the recording main row.
        if (occupied.width() <= bounds.width()) {
            require(bounds.contains(occupied), message);
        }
    };
    const auto requireAnchored = [&]() {
        const QPoint position = toolbar.contentPosition();
        const QRect occupied = toolbar.occupiedContentRect();
        toolbar.placeForPhysicalRegion(region);
        require(
            toolbar.contentPosition() == position && toolbar.occupiedContentRect() == occupied,
            "content changes before dragging must match a fresh placement of the whole toolbar");
    };
    requireFits("the collapsed recording toolbar must initially fit on screen");
    exportButton->click();
    QCoreApplication::processEvents();
    requireFits("opening recording export settings must keep all rows on screen");
    requireAnchored();
    exportButton->click();
    requireAnchored();
    toolbar.palette()->setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QCoreApplication::processEvents();
    requireFits("opening recording drawing styles must keep all rows on screen");
    requireAnchored();

    toolbar.palette()->clearActiveTool();
    requireAnchored();
    toolbar.setStyleToolbarAboveMain(false);
    const QPoint interiorPosition = toolbar.constrainedContentPosition(
        QPoint(bounds.left(), bounds.top() + bounds.height() / 3));
    toolbar.moveContentTo(interiorPosition);
    toolbar.paletteHost()->dragStarted(interiorPosition);
    toolbar.paletteHost()->dragFinished(interiorPosition);
    exportButton->click();
    QCoreApplication::processEvents();
    requireFits("recording export settings must fit at an interior position");
    require(toolbar.contentPosition() == interiorPosition,
            "opening a panel after dragging must preserve the user's toolbar position");
    toolbar.placeForPhysicalRegion(region);
    exportButton->click();
    requireAnchored();
    exportButton->click();
    requireAnchored();
}
} // namespace

namespace {
void pumpPreview() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
    }
}
QImage previewImage(QWidget& widget) {
    QImage image(widget.size(), QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}
class PreviewTestTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) == "RecordingEffectPreview" &&
            QByteArray(source) == "Motion Preview in Progress") {
            return QStringLiteral("Preview translated");
        }
        return {};
    }
};

void effectsPreviewLifecycle() {
    for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        const QRect selected(-2001, -1103, 641, 479);
        const QRect capture = selected.adjusted(-1, -1, 0, 0);
        const QRectF local(3.25, 3.75, 641 / dpr, 479 / dpr);
        const QPoint canvas = local.toAlignedRect().topLeft();
        for (const QSize output : {QSize(642, 480), QSize(428, 320)}) {
            const auto transform =
                recordingEffectsOutputTransform(capture, selected, local, canvas, dpr, output);
            // The selected first pixel follows a one-physical-pixel encoder expansion.
            const QPointF selectedOrigin(output.width() / 642.0, output.height() / 480.0);
            require(
                QLineF(transform.map(selectedOrigin), QPointF(.25, .75)).length() < 0.00001,
                "negative coordinates, encoder padding and fractional canvas insets must align");
            const QPointF selectedEnd(output.width(), output.height());
            require(QLineF(transform.map(selectedEnd), QPointF(.25 + 641 / dpr, .75 + 479 / dpr))
                            .length() < 0.00001,
                    "export scaling must map precisely to the selection at every DPI");
            for (const QSize captureSize : {capture.size(), QSize(1920, 1080), QSize(3840, 2160)}) {
                const auto keyboardTransform =
                    recordingEffectsOutputTransform(QRect(capture.topLeft(), captureSize), selected,
                                                    local, canvas, dpr, captureSize);
                const QSizeF keycap = keyboardTransform.mapRect(QRectF(0, 0, 64, 64)).size() * dpr;
                require(
                    qAbs(keycap.width() - 64) < 0.00001 && qAbs(keycap.height() - 64) < 0.00001,
                    "keyboard preview must stay 64 physical pixels at every capture size and DPI");
            }
        }
    }
    auto state = std::make_shared<RecordingEffectTestState>();
    ScreenRecordingAreaWindow area;
    area.setPhysicalRegion(QRect(40, 40, 640, 480));
    RecordingEffectPreview preview(area, std::make_unique<RecordingEffectTestSource>(state));
    preview.configure(area.physicalRegion(), QSize(640, 480), QColor(255, 0, 0, 128),
                      Qt::transparent, false);
    preview.setEligible(true);
    require(!state->active, "hidden window must not observe input");
    area.show();
    pumpPreview();
    auto* label = area.findChild<QLabel*>(QStringLiteral("screenRecordingMotionPreviewLabel"));
    require(state->active && preview.hasFrame() && label && label->isVisible(),
            "visible idle preview must show effects and readout");
    require(label->text() == QStringLiteral("Motion Preview in Progress"),
            "preview label must use requested text");
    require(label->testAttribute(Qt::WA_TransparentForMouseEvents),
            "readout must not intercept drawing");
    for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                            ScreenRecordingAreaWindow::InputMode::Drawing,
                            ScreenRecordingAreaWindow::InputMode::RegionEditing}) {
        area.setInputMode(mode);
        pumpPreview();
        require(state->active, "all idle input modes must preview");
        const int backgroundAlpha =
            mode == ScreenRecordingAreaWindow::InputMode::PassThrough ? 0 : 2;
        require(previewImage(*area.canvas()).pixelColor(100, 100).alpha() == backgroundAlpha,
                "preview clearing must preserve the input surface in editing and drawing modes");
        state->publish(false);
        pumpPreview();
        require(previewImage(*area.canvas()).pixelColor(28, 28).alpha() == backgroundAlpha,
                "expired effects must restore hit-test coverage without leaving effect pixels");
        preview.setEligible(false);
        require(previewImage(*area.canvas()).pixelColor(28, 28).alpha() == backgroundAlpha,
                "stopped preview must retain the input surface in interactive modes");
        preview.setEligible(true);
        pumpPreview();
    }
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    // Exercise the renderer itself with independent layer coordinates, not just its transform.
    for (const QSize captureSize : {QSize(640, 480), QSize(960, 720)}) {
        area.setPhysicalRegion(QRect(QPoint(40, 40), captureSize));
        for (const QSize exportSize : {QSize(320, 240), QSize(1920, 1080)}) {
            preview.configure(area.physicalRegion(), exportSize, Qt::red, Qt::transparent, true);
            pumpPreview();
            state->publish(false);
            QImage keycap(64, 64, QImage::Format_RGBA8888_Premultiplied);
            keycap.fill(Qt::blue);
            state->frame->tiles.push_back({QRect(128, 128, 64, 64), keycap, captureSize});
            state->notify();
            pumpPreview();
            const QImage rendered = previewImage(*area.canvas());
            QRect pixels;
            for (int y = 0; y < rendered.height(); ++y) {
                for (int x = 0; x < rendered.width(); ++x) {
                    if (rendered.pixelColor(x, y).blue() > 128) {
                        pixels |= QRect(x, y, 1, 1);
                    }
                }
            }
            require(pixels.size() == QSize(64, 64),
                    "keyboard tiles must not grow with capture area or export scale");
        }
    }
    area.setPhysicalRegion(QRect(40, 40, 640, 480));
    preview.configure(area.physicalRegion(), QSize(640, 480), Qt::red, Qt::transparent, false);
    pumpPreview();
    const auto history = area.canvas()->canvasHistoryState();
    const QImage visible = previewImage(*area.canvas());
    require(visible.pixelColor(28, 28).alpha() > 0, "preview tiles must appear on canvas");
    state->publish(false);
    pumpPreview();
    require(previewImage(*area.canvas()).pixelColor(28, 28).alpha() == 0,
            "final empty snapshot must remove expired pixels");
    require(area.canvas()->canvasHistoryState().canUndo == history.canUndo,
            "preview must not change undo history");
    state->publish();
    const auto stale = state->frame;
    preview.configure(area.physicalRegion(), QSize(640, 480), Qt::transparent, Qt::transparent,
                      false);
    require(!state->active && !preview.hasFrame() && !label->isVisible(),
            "disabling all effects must clear synchronously");
    pumpPreview();
    preview.configure(area.physicalRegion(), QSize(640, 480), Qt::red, Qt::transparent, true);
    pumpPreview();
    require(state->active, "enabling an effect must restart preview");
    state->frame = stale;
    state->notify();
    pumpPreview();
    require(preview.generation() != stale->generation,
            "configuration must invalidate stale generations");
    QDialog modal;
    modal.setModal(true);
    modal.show();
    pumpPreview();
    require(!state->active && !label->isVisible(),
            "modal operations must suspend input observation");
    modal.hide();
    pumpPreview();
    require(state->active, "preview must return when the modal operation ends");
    PreviewTestTranslator translator;
    qApp->installTranslator(&translator);
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&area, &languageChange);
    pumpPreview();
    require(label->accessibleName() == QStringLiteral("Preview translated"),
            "visible readout must retranslate");
    qApp->removeTranslator(&translator);
    QCoreApplication::sendEvent(&area, &languageChange);
    pumpPreview();
    CanvasStatusReadout narrow(&area);
    const QString complete = QStringLiteral("Motion Preview in Progress");
    narrow.setText(complete);
    narrow.layoutIn(QRect(0, 0, 80, 28));
    require(narrow.width() <= 64 && narrow.toolTip() == complete &&
                narrow.accessibleName() == complete,
            "narrow readout must elide without losing accessible copy");
    const auto generation = preview.generation();
    preview.setEligible(false);
    preview.stopAndClear(true);
    require(!state->active && preview.generation() > generation && !preview.hasFrame() &&
                !label->isVisible(),
            "startup barrier must stop and clear preview synchronously");
    pumpPreview();
    require(!preview.hasFrame(), "queued notifications must not resurrect stopped effects");
    preview.setEligible(true);
    pumpPreview();
    area.hide();
    require(!state->active && !preview.hasFrame(), "hiding must stop observation immediately");
    int failures = 0;
    preview.reportError = [&](const QString&) { ++failures; };
    state->fail = true;
    area.show();
    pumpPreview();
    require(failures == 1 && !state->active && !preview.hasFrame() && !label->isVisible(),
            "failed initialization must clear the source and report one nonmodal error");
    pumpPreview();
    require(failures == 1, "failed activation must not retry or report repeatedly");
    preview.setEligible(false);
    state->fail = false;
    preview.setEligible(true);
    pumpPreview();
    require(state->active, "a fresh activation must recover after preview failure");
}

void controllerPreviewTransitions() {
    using snow_shot::storage::RecordingSettings;
    RecordingSettings().setMouseTrailColor(Qt::red);
    RecordingSettings().setShowKeyboard(true);
    ScreenRecordingController controller(testEffectsSource);
    controller.open({40, 40, 320, 240});
    pumpPreview();
    auto state = effectSources.back().lock();
    require(state && state->active, "idle controller must enable preview");
    ScreenRecordingAreaWindow* area = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget);
            candidate && candidate->isVisible()) {
            area = candidate;
            break;
        }
    }
    auto* exportButton = palette()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenRecordingExportSettings"));
    require(area && exportButton, "controller must expose the recording area and export settings");
    if (!palette()->recordingExportSettingsVisible()) {
        exportButton->click();
    }
    pumpPreview();
    require(area->inputMode() == ScreenRecordingAreaWindow::InputMode::RegionEditing &&
                !area->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 2,
            "idle export settings must keep the recording area clickable through preview clears");
    require(palette()->activateDrawingShortcut(QStringLiteral("shape")),
            "a drawing tool must replace export settings");
    pumpPreview();
    require(area->inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing &&
                !palette()->recordingExportSettingsVisible() &&
                previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 2,
            "switching from export settings to drawing must retain hit-test coverage");
    exportButton->click();
    pumpPreview();
    ErrorObserver errors;
    qApp->installEventFilter(&errors);
    failStart = true;
    controller.startRecording();
    pumpPreview();
    require(!controller.isRecording() && state->active && errors.shown == 1,
            "startup failure must restore a fresh preview after dismissing its modal error");
    failStart = false;
    qApp->removeEventFilter(&errors);
    controller.startRecording();
    require(!state->active, "accepted start must stop preview before its queued callback");
    pumpPreview();
    require(controller.isRecording() && !state->active, "recording must keep preview stopped");
    require(previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 0,
            "recording with export settings selected must not retain the idle input surface");
    palette()->recordingPauseRequested();
    require(!state->active, "paused session must keep preview stopped");
    palette()->recordingResumeRequested();
    require(!state->active, "resume must not restore preview");
    palette()->recordingStopRequested();
    waitForIdle(controller);
    pumpPreview();
    require(state->active, "completed finalization must restore fresh preview");
    require(previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 2,
            "returning to idle export settings must restore hit-test coverage");
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!state->active, "closing must join the preview source");
    RecordingSettings().setMouseTrailColor(Qt::transparent);
    RecordingSettings().setShowKeyboard(false);
}
#ifdef Q_OS_WIN
int nativeEffectsPreviewCapture() {
    const auto checkNative = [](bool success, const char* message) {
        if (!success) {
            throw std::runtime_error(message);
        }
    };
    const auto waitUntil = [](auto condition) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return condition();
    };
    struct RestoreInput {
        POINT point{};
        HWND foreground = GetForegroundWindow();
        RestoreInput() {
            GetCursorPos(&point);
        }
        ~RestoreInput() {
            INPUT key{};
            key.type = INPUT_KEYBOARD;
            key.ki.wVk = 'A';
            key.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &key, sizeof(INPUT));
            SetCursorPos(point.x, point.y);
            if (foreground != nullptr) {
                SetForegroundWindow(foreground);
            }
        }
    } restore;
    QScreen* screen = QGuiApplication::primaryScreen();
    const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QRect region(bounds.topLeft() + QPoint(100, 100), QSize(640, 480));
    ScreenRecordingAreaWindow area;
    area.setPhysicalRegion(region);
    QWidget background(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    background.setStyleSheet(QStringLiteral("background-color: rgb(38, 48, 63);"));
    background.setGeometry(area.geometry());
    background.show();
    RecordingEffectPreview preview(area);
    preview.configure(region, region.size(), Qt::red, Qt::cyan, true);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    area.show();
    area.raise();
    area.activateWindow();
    area.canvas()->setFocus();
    pumpPreview();
    const QString output = QDir::current().absoluteFilePath(QStringLiteral("effects-native-qa"));
    QDir().mkpath(output);
    for (QScreen* target : QGuiApplication::screens()) {
        const QRect targetBounds = ScreenshotGeometryMapper::physicalRectForScreen(*target);
        const QRect selected(targetBounds.topLeft() + QPoint(50, 50), QSize(641, 479));
        area.setPhysicalRegion(selected);
        background.setGeometry(area.geometry());
        preview.configure(selected.adjusted(0, 0, 1, 1), QSize(642, 480), Qt::red, Qt::cyan, true);
        for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                                ScreenRecordingAreaWindow::InputMode::Drawing,
                                ScreenRecordingAreaWindow::InputMode::RegionEditing}) {
            preview.setEligible(false);
            area.setInputMode(mode);
            area.show();
            area.raise();
            preview.setEligible(true);
            auto* ready =
                area.findChild<QLabel*>(QStringLiteral("screenRecordingMotionPreviewLabel"));
            checkNative(waitUntil([&]() { return ready && ready->isVisible(); }),
                        "preview must initialize on every display and in every idle input mode");
            area.repaint();
            area.canvas()->repaint();
            static_cast<void>(DwmFlush());
            const POINT gap{selected.x() + 300, selected.y() + 200};
            const HWND targetWindow = GetAncestor(WindowFromPoint(gap), GA_ROOT);
            const HWND areaHandle = reinterpret_cast<HWND>(area.winId());
            checkNative((targetWindow == areaHandle) ==
                            (mode != ScreenRecordingAreaWindow::InputMode::PassThrough),
                        "empty preview pixels must receive native clicks while editing or drawing");
            SetCursorPos(selected.x() + 120, selected.y() + 100);
            INPUT inputs[3]{};
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki.wVk = 'A';
            inputs[1].type = inputs[2].type = INPUT_MOUSE;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            checkNative(SendInput(3, inputs, sizeof(INPUT)) == 3,
                        "native input must reach the fixture");
            checkNative(waitUntil([&]() {
                            if (!preview.hasFrame()) {
                                return false;
                            }
                            const QImage image = previewImage(*area.canvas());
                            const qreal scale = area.devicePixelRatioF();
                            const QRect mouseArea(qRound(88 / scale), qRound(68 / scale),
                                                  qRound(64 / scale), qRound(64 / scale));
                            for (int y = mouseArea.top(); y <= mouseArea.bottom(); ++y) {
                                for (int x = mouseArea.left(); x <= mouseArea.right(); ++x) {
                                    if (image.pixelColor(x, y).alpha() > 2) {
                                        return true;
                                    }
                                }
                            }
                            return false;
                        }),
                        "native click effects must reach the canvas in every idle mode");
            inputs[0].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, inputs, sizeof(INPUT));
            std::cout << "native mode=" << static_cast<int>(mode)
                      << " display=" << target->name().toStdString()
                      << " dpr=" << area.devicePixelRatioF() << '\n';
        }
        for (const QSize captureSize : {QSize(641, 479), QSize(961, 719)}) {
            for (const QSize exportSize : {QSize(320, 240), QSize(1280, 960)}) {
                preview.setEligible(false);
                const QRect capture(selected.topLeft(), captureSize);
                area.setPhysicalRegion(capture);
                background.setGeometry(area.geometry());
                area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
                area.raise();
                area.activateWindow();
                preview.configure(capture, exportSize, Qt::transparent, Qt::transparent, true);
                preview.setEligible(true);
                checkNative(waitUntil([&]() {
                                auto* ready = area.findChild<QLabel*>(
                                    QStringLiteral("screenRecordingMotionPreviewLabel"));
                                return ready && ready->isVisible();
                            }),
                            "keyboard-only preview must initialize");
                INPUT keyInput{};
                keyInput.type = INPUT_KEYBOARD;
                keyInput.ki.wVk = 'A';
                checkNative(SendInput(1, &keyInput, sizeof(INPUT)) == 1,
                            "native key input must reach the preview");
                checkNative(waitUntil([&]() {
                                if (!preview.hasFrame()) {
                                    return false;
                                }
                                const qreal dpr = area.devicePixelRatioF();
                                QImage image(area.canvas()->size() * dpr,
                                             QImage::Format_RGBA8888_Premultiplied);
                                image.setDevicePixelRatio(dpr);
                                image.fill(Qt::transparent);
                                {
                                    QPainter painter(&image);
                                    area.canvas()->render(&painter);
                                }
                                QRect keyPixels;
                                for (int y = 0; y < image.height(); ++y) {
                                    for (int x = 0; x < image.width(); ++x) {
                                        if (image.pixelColor(x, y).alpha() > 16) {
                                            keyPixels |= QRect(x, y, 1, 1);
                                        }
                                    }
                                }
                                return keyPixels.size() == QSize(64, 64);
                            }),
                            "native keyboard preview must remain exactly 64 physical pixels");
                keyInput.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &keyInput, sizeof(INPUT));
                std::cout << "native keycap=64x64 capture=" << captureSize.width() << 'x'
                          << captureSize.height() << " export=" << exportSize.width() << 'x'
                          << exportSize.height() << " dpr=" << area.devicePixelRatioF() << '\n';
            }
        }
    }
    preview.setEligible(false);
    area.setPhysicalRegion(region);
    background.setGeometry(area.geometry());
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    preview.configure(region, region.size(), Qt::red, Qt::cyan, true);
    // Keep an annotation in the capture baseline; shutdown must preserve it exactly.
    checkNative(area.canvas()->setCanvasTool(SnowCanvasTool::Shape),
                "native annotation tool must activate");
    for (const auto type :
         {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
        const QPointF point = type == QEvent::MouseButtonPress ? QPointF(30, 30) : QPointF(70, 70);
        QMouseEvent event(
            type, point, point, point, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(area.canvas(), &event);
    }
    checkNative(area.canvas()->canvasHistoryState().canUndo,
                "native capture fixture must retain its annotation");
    checkNative(area.canvas()->setCanvasTool(SnowCanvasTool::Select),
                "native annotation tool must release");
    pumpPreview();
    for (const auto backend :
         {SNOW_CAPTURE_BACKEND_WGC, SNOW_CAPTURE_BACKEND_DXGI, SNOW_CAPTURE_BACKEND_GDI}) {
        preview.setEligible(false);
        preview.stopAndClear(true);
        const auto capture = [&]() {
            const SnowCaptureRegionSessionConfig config{region.x(),
                                                        region.y(),
                                                        640,
                                                        480,
                                                        1,
                                                        SNOW_CAPTURE_WGC_UPDATE_MODE_COMPLETE_ONLY,
                                                        static_cast<uint8_t>(backend),
                                                        SNOW_CAPTURE_PIXEL_FORMAT_RGBA8,
                                                        {}};
            std::unique_ptr<SnowCaptureRegionSession,
                            decltype(&snow_capture_region_session_destroy)>
                captureSession(snow_capture_region_session_create(&config),
                               snow_capture_region_session_destroy);
            checkNative(captureSession != nullptr, nativeCaptureError());
            SnowCaptureRegionFrameInfo info{};
            QElapsedTimer firstFrame;
            firstFrame.start();
            while (snow_capture_region_session_capture(captureSession.get(), &info) == 0) {
                const QString error = QString::fromUtf8(nativeCaptureError());
                // DXGI can time out before producing any frame on a static desktop. Keep
                // waiting for that first frame; never discard a captured frame to pass QA.
                if (!error.contains(QStringLiteral("within timeout")) ||
                    firstFrame.elapsed() >= 3000) {
                    throw std::runtime_error(error.toStdString());
                }
                QCoreApplication::processEvents();
            }
            QImage image(info.rgba_bytes, static_cast<int>(info.width),
                         static_cast<int>(info.height), static_cast<qsizetype>(info.stride_bytes),
                         QImage::Format_RGBA8888);
            const QImage copied = image.copy();
            return copied;
        };
        const QImage clean = capture();
        preview.setEligible(true);
        auto* label = area.findChild<QLabel*>(QStringLiteral("screenRecordingMotionPreviewLabel"));
        checkNative(waitUntil([&]() { return label && label->isVisible(); }),
                    "native effect observers must initialize");
        SetCursorPos(region.x() + 100, region.y() + 100);
        SetCursorPos(region.x() + 180, region.y() + 140);
        INPUT key{};
        key.type = INPUT_KEYBOARD;
        key.ki.wVk = 'A';
        checkNative(SendInput(1, &key, sizeof(INPUT)) == 1, "native key must reach the fixture");
        checkNative(waitUntil([&]() {
                        if (!preview.hasFrame()) {
                            return false;
                        }
                        const QImage image = previewImage(*area.canvas());
                        for (int y = image.height() / 2; y < image.height(); ++y) {
                            for (int x = image.width() / 2; x < image.width(); ++x) {
                                if (image.pixelColor(x, y).alpha() != 0) {
                                    return true;
                                }
                            }
                        }
                        return false;
                    }),
                    "native keyboard input must reach the canvas");
        area.repaint();
        const QImage visible = capture();
        key.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &key, sizeof(INPUT));
        preview.setEligible(false);
        preview.stopAndClear(true);
        const QImage after = capture();
        int effectPixels = 0;
        int remaining = 0;
        for (int y = 0; y < clean.height(); ++y) {
            for (int x = 0; x < clean.width(); ++x) {
                const auto different = [&](const QImage& image) {
                    const QColor a = clean.pixelColor(x, y), b = image.pixelColor(x, y);
                    return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) +
                               qAbs(a.blue() - b.blue()) >
                           12;
                };
                effectPixels += different(visible);
                remaining += different(after);
            }
        }
        checkNative(effectPixels > 100, "native capture must observe visible preview effects");
        checkNative(remaining == 0,
                    "the first captured frame after shutdown must contain no preview pixels");
        visible.save(output +
                     QStringLiteral("/backend-%1-preview.png").arg(static_cast<int>(backend)));
        after.save(output +
                   QStringLiteral("/backend-%1-cleared.png").arg(static_cast<int>(backend)));
        std::cout << "backend=" << static_cast<int>(backend) << " preview_pixels=" << effectPixels
                  << " remaining=" << remaining << '\n';
    }
    area.hide();
    background.hide();
    return 0;
}
#endif
} // namespace

extern "C" {
SnowCaptureResult
snow_capture_recording_session_create_direct(const SnowCaptureDirectRecordingConfig*,
                                             SnowCaptureRecordingSession** result) {
    for (const auto& weak : effectSources) {
        if (const auto source = weak.lock()) {
            require(!source->active, "native creation must follow preview observer shutdown");
        }
    }
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* area = qobject_cast<ScreenRecordingAreaWindow*>(widget)) {
            auto* label =
                area->findChild<QLabel*>(QStringLiteral("screenRecordingMotionPreviewLabel"));
            require(!label || !label->isVisible(),
                    "native capture must never see the preview label");
        }
    }
    if (failStart) {
        *result = nullptr;
        return SNOW_CAPTURE_RESULT_INVALID_ARGUMENT;
    }
    *result = &session;
    return SNOW_CAPTURE_RESULT_OK;
}
void snow_capture_recording_session_destroy(SnowCaptureRecordingSession*) {
    ++destroyedSessions;
}
uint8_t snow_capture_recording_session_start(SnowCaptureRecordingSession*) {
    ++starts;
    return 1;
}
uint8_t snow_capture_recording_session_pause(SnowCaptureRecordingSession*) {
    return 1;
}
uint8_t snow_capture_recording_session_resume(SnowCaptureRecordingSession*) {
    return 1;
}
SnowCaptureResult snow_capture_recording_session_stop(SnowCaptureRecordingSession*) {
    ++exports;
    if (exportEntered != nullptr) {
        exportEntered->set_value();
    }
    if (exportGate.valid()) {
        exportGate.wait();
    }
    return failExport ? SNOW_CAPTURE_RESULT_INVALID_ARGUMENT : SNOW_CAPTURE_RESULT_OK;
}
uint8_t snow_capture_recording_session_state(const SnowCaptureRecordingSession*,
                                             SnowCaptureRecordingState* state) {
    *state = SNOW_CAPTURE_RECORDING_STATE_RUNNING;
    return 1;
}
const char* snow_capture_last_error_message() {
    return "test backend";
}
}

int main(int argc, char** argv) {
#ifdef SNOW_RECORDING_EFFECTS_BENCHMARK
    RecordingEffectsBenchmarkApplication app(argc, argv);
#else
    QApplication app(argc, argv);
#endif
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary storage must exist");
    using namespace snow_shot::storage;
    require(ApplicationStorage::instance()
                .initialize({temporary.filePath("bin"), temporary.filePath("settings"), 0})
                .success,
            "isolated storage must initialize");
    require(RecordingSettings().setVideoSaveDirectory(temporary.path()),
            "test output directory must be set");
    require(RecordingSettings().setHideToolbarInRecording(false),
            "capture exclusion must be disabled for fake backend");
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--native-toolbar-display-only")) ||
        app.arguments().contains(QStringLiteral("--native-toolbar-ready-display-only"))) {
        const int result = recordingToolbarAcrossNativeDisplays(
            app.arguments().contains(QStringLiteral("--native-toolbar-display-only")));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ApplicationStorage::instance().shutdown();
        return result;
    }
#endif
#ifdef Q_OS_WIN
#ifdef SNOW_RECORDING_EFFECTS_BENCHMARK
    if (app.arguments().contains(QStringLiteral("--effects-preview-performance"))) {
        const int result = runRecordingEffectsPerformanceBenchmark(app);
        ApplicationStorage::instance().shutdown();
        return result;
    }
#endif
    if (app.arguments().contains(QStringLiteral("--effects-preview-native"))) {
        int result = 0;
        try {
            result = nativeEffectsPreviewCapture();
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            result = 1;
        }
        ApplicationStorage::instance().shutdown();
        return result;
    }
#endif
    if (app.arguments().contains(QStringLiteral("--effects-preview-only"))) {
        class KeyTranslator : public QTranslator {
          public:
            bool isEmpty() const override {
                return false;
            }
            QString translate(const char* context, const char*, const char*, int) const override {
                return QByteArray(context) == "RecordingKeyboard" ? QStringLiteral("translated")
                                                                  : QString();
            }
        } translator;
        const RecordingKeyboardLabels original(true);
        require(app.installTranslator(&translator), "key translator must install");
        const RecordingKeyboardLabels localized(true);
        require(localized.text == original.text,
                "all key legends must ignore application language");
        require(localized.text.contains(QByteArray("Backspace")) &&
                    localized.text.contains(QByteArray("Num 0")),
                "key legends must use English names");
        app.removeTranslator(&translator);
        effectsPreviewLifecycle();
        controllerPreviewTransitions();
        ApplicationStorage::instance().shutdown();
        return 0;
    }
    recordingToolbarReconcilesFrameBeforeShowing();
    recordingToolbarPlacementAcrossDisplays();
    if (app.arguments().contains(QStringLiteral("--placement-only"))) {
        return 0;
    }
    recordingSecondaryPanelsStayOnScreen();
    {
        ScreenRecordingController controller(testEffectsSource);
        const QRect region(40, 40, 320, 240);
        controller.open(region);
        ScreenRecordingAreaWindow* area = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget)) {
                area = candidate;
            }
        }
        require(area != nullptr, "recording area must exist");
        requireToolbarAboveArea(area);
        auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
        auto* picker = toolbar->palette()->findChild<adqt::widgets::AdColorPicker*>(
            QStringLiteral("screenRecordingMouseTrailColor"));
        require(picker != nullptr, "export settings must expose the trail color popup");
        picker->setPopupVisible(true);
        QCoreApplication::processEvents();
        require(picker->popupVisible(), "test popup must open");
        const QPoint toolbarPosition = toolbar->pos();
        area->regionInteractionStarted();
        require(!picker->popupVisible(), "geometry interaction must dismiss export popups");
        require(!toolbar->isVisible(), "native interaction must hide the toolbar");
        area->move(area->pos() + QPoint(20, 10));
        QCoreApplication::processEvents();
        require(!toolbar->isVisible() && toolbar->pos() == toolbarPosition,
                "intermediate geometry must not show or reposition the toolbar");
        area->regionInteractionFinished();
        require(toolbar->isVisible(), "finishing interaction must restore the toolbar");
        const QPoint finalPosition = toolbar->contentPosition();
        toolbar->placeForPhysicalRegion(area->physicalRegion());
        require(toolbar->contentPosition() == finalPosition,
                "restored toolbar must use final geometry");
        controller.open(region);
        auto* canvas = area->canvas();
        require(palette()->activateDrawingShortcut(QStringLiteral("shape")),
                "drawing shortcut must activate the shape tool");
        require(canvas->setCanvasTool(SnowCanvasTool::Shape), "shape must activate");
        const auto mouse = [canvas](QEvent::Type type, QPointF position, Qt::MouseButton button,
                                    Qt::MouseButtons buttons) {
            QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(canvas, &event);
        };
        mouse(QEvent::MouseButtonPress, {30, 30}, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, {100, 80}, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, {100, 80}, Qt::LeftButton, Qt::NoButton);
        require(canvas->canvasHistoryState().canUndo, "drawing must create history");
        controller.open(region);
        require(canvas->canvasHistoryState().canUndo,
                "opening an already visible region must preserve its drawing");
        QPointer<ScreenRecordingAreaWindow> closedArea(area);
        QPointer<ScreenRecordingToolbarWindow> closedToolbar(
            qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window()));
        palette()->recordingCloseRequested();
        require(!controller.isOpen(), "Close must detach the session immediately");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(closedArea.isNull() && closedToolbar.isNull(), "Close must destroy both windows");
        controller.open(region);
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget)) {
                area = candidate;
            }
        }
        canvas = area->canvas();
        require(area->isVisible(), "reopening must create a visible fresh session");
        requireToolbarAboveArea(area);
        require(!canvas->canvasHistoryState().canUndo && !canvas->canvasHistoryState().canRedo,
                "a new session at the same rectangle must clear drawing and history");
        require(canvas->canvasTool() == SnowCanvasTool::Select &&
                    area->inputMode() != ScreenRecordingAreaWindow::InputMode::Drawing &&
                    !palette()->activeTool().has_value(),
                "a new session must reset transient drawing tools");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.startRecording();
        controller.open(QRect(40, 40, 320, 240));
        QCoreApplication::processEvents();
        require(starts == 0, "a start requested while closed must not affect a later window");
        controller.startRecording();
        palette()->recordingCloseRequested();
        controller.open(QRect(80, 80, 320, 240));
        QCoreApplication::processEvents();
        require(starts == 0 && !controller.isRecording(),
                "closing a queued start must not start a later recording window");
        for (int iteration = 0; iteration < 8; ++iteration) {
            controller.startRecording();
            palette()->recordingCloseRequested();
            controller.open(QRect(80, 80, 320, 240));
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        int toolbarCount = 0;
        for (auto* widget : QApplication::topLevelWidgets()) {
            toolbarCount += qobject_cast<ScreenRecordingToolbarWindow*>(widget) != nullptr;
        }
        require(toolbarCount == 1, "reopening must leave only the current toolbar alive");
        controller.startRecording();
        controller.open(QRect(120, 80, 320, 240));
        QCoreApplication::processEvents();
        require(starts == 0, "replacing the selected region must invalidate pending starts");
        controller.startRecording();
        palette()->recordingCloseRequested();
        controller.open(QRect(80, 80, 320, 240));
        controller.startRecording();
        controller.startRecording();
        QCoreApplication::processEvents();
        require(starts == 1 && controller.isRecording(), "a fresh request must start exactly once");
        palette()->recordingPauseRequested();
        require(controller.isRecording(),
                "paused recording must remain stoppable by the global toggle");
        controller.stopRecordingAndCopy();
        controller.stopRecordingAndCopy();
        controller.startRecording();
        // Do not pump the UI export-completion timer: this test must not alter
        // the desktop clipboard. Destruction joins the fake export worker.
        QElapsedTimer elapsed;
        elapsed.start();
        while (exports.load() == 0 && elapsed.elapsed() < 2000) {
            QThread::msleep(1);
        }
        require(exports.load() == 1 && starts == 1,
                "repeated shortcut calls during export must not duplicate stop or restart");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open(QRect(40, 40, 320, 240));
        controller.startRecording();
    }
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(starts == 1, "destroying the controller must cancel a queued recording start");
    closeAndStopHaveIndependentUiLifetimes();
    ApplicationStorage::instance().shutdown();
    return 0;
}
