#include "../src/presentation/recording/recordingcolorsampler.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/color_picker.h"

#include <QAbstractButton>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

void recordingColorSamplerInteractions() {
    using snow_shot::presentation::recording::RecordingColorSampler;
    ScreenRecordingAreaWindow area;
    ScreenRecordingToolbarWindow toolbar;
    area.setRecordingRegion(QRect(10, 10, 640, 480));
    area.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    area.show();
    toolbar.showWithoutActivating();
    auto* canvas = area.canvas();
    auto* tools = toolbar.palette();
    require(canvas->setCanvasTool(SnowCanvasTool::Shape), "activate shape canvas tool");
    tools->setActiveTool(ScreenshotToolPalette::Tool::Shape);
    tools->setStyleToolbarState(canvas->canvasStyleToolbarState());
    snow_shot::presentation::ScreenshotStyleBinding binding(*tools, *canvas, &toolbar);
    QColor sampled(23, 145, 211);
    bool captureFailed = false;
    QPointF lastPosition;
    RecordingColorSampler sampling(area, toolbar, [&](const QPointF& globalPosition) {
        lastPosition = globalPosition;
        if (captureFailed)
            return QImage();
        QImage preview(7, 7, QImage::Format_RGB32);
        preview.fill(Qt::red);
        preview.setPixelColor(3, 3, sampled);
        return preview;
    });
    adqt::widgets::AdColorPicker* picker = nullptr;
    for (auto* candidate : tools->findChildren<adqt::widgets::AdColorPicker*>()) {
        if (candidate->accessibleName() == QStringLiteral("Stroke color"))
            picker = candidate;
    }
    require(picker != nullptr, "recording stroke picker exists");
    int commits = 0;
    QObject::connect(picker, &adqt::widgets::AdColorPicker::editingFinished, &toolbar,
                     [&](const auto&) { ++commits; });
    const auto begin = [&] {
        picker->setPopupVisible(true);
        QCoreApplication::processEvents();
        auto* button = qobject_cast<QAbstractButton*>(picker->previewContent());
        require(button != nullptr, "recording picker has an eyedropper");
        button->click();
        require(!picker->popupVisible() && QApplication::overrideCursor() != nullptr,
                "eyedropper starts sampling and closes the popup");
    };
    const QPointF local(100.25, 80.5);
    const QPointF global = canvas->mapToGlobal(local);
    const auto mouse = [&](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, local, global, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    begin();
    mouse(QEvent::MouseMove, Qt::NoButton, Qt::NoButton);
    require(lastPosition == global, "preview preserves fractional global pointer coordinates");
    // The screen can change while recording; commit must re-read the clicked pixel.
    sampled = QColor(81, 52, 173);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    require(commits == 1 && picker->value().solidColor == sampled &&
                canvas->canvasStyleToolbarState().shapeStyle.stroke == sampled &&
                snow_shot::presentation::screenshotCanvasToolStyleDefaults().rectangle.stroke ==
                    sampled,
            "sampling commits the current center pixel to the picker, canvas and saved defaults");
    require(!canvas->canvasHistoryState().canUndo &&
                canvas->canvasTool() == SnowCanvasTool::Shape &&
                QApplication::overrideCursor() == nullptr &&
                area.inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing,
            "sampling must consume the full gesture without drawing or changing tools");

    begin();
    mouse(QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton);
    mouse(QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton);
    require(commits == 1 && QApplication::overrideCursor() == nullptr,
            "right click cancels sampling without committing");
    begin();
    int deactivations = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::drawingDeactivationRequested, &toolbar,
                     [&] { ++deactivations; });
    for (const auto type : {QEvent::ShortcutOverride, QEvent::KeyPress}) {
        QKeyEvent escape(type, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&toolbar, &escape);
    }
    for (const auto type : {QEvent::ShortcutOverride, QEvent::KeyPress, QEvent::KeyRelease}) {
        QKeyEvent repeat(type, Qt::Key_Escape, Qt::NoModifier, QString(), true);
        QApplication::sendEvent(canvas, &repeat);
    }
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    require(commits == 1 && QApplication::overrideCursor() == nullptr &&
                canvas->canvasTool() == SnowCanvasTool::Shape && deactivations == 0,
            "Escape cancels sampling and preserves the active drawing tool");
    begin();
    captureFailed = true;
    mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    require(commits == 1 && picker->value().solidColor == sampled &&
                QApplication::overrideCursor() == nullptr,
            "failed screen capture must preserve the existing color and end sampling");
    captureFailed = false;

    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    begin();
    require(area.inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing,
            "sampling temporarily accepts clicks when the recording was passing input through");
    sampling.cancel();
    require(area.inputMode() == ScreenRecordingAreaWindow::InputMode::PassThrough,
            "cancelling restores the previous recording input mode");
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    begin();
    area.regionInteractionStarted();
    require(QApplication::overrideCursor() == nullptr,
            "moving the recording region cancels sampling");
    begin();
    area.hide();
    require(QApplication::overrideCursor() == nullptr,
            "hiding the recording area cancels sampling");
    area.show();
    begin();
    tools->setActiveTool(ScreenshotToolPalette::Tool::Select);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(QApplication::overrideCursor() == nullptr,
            "destroying the originating picker must clean up sampling");
}
