#include "recordingcolorsampler.h"

#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/color_picker.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>

#include <cmath>
#include <utility>

namespace snow_shot::presentation::recording {
namespace {
QImage readScreenPreview(const QPointF& globalPosition) {
    const QPoint point(static_cast<int>(std::floor(globalPosition.x())),
                       static_cast<int>(std::floor(globalPosition.y())));
    QScreen* screen = QGuiApplication::screenAt(point);
    if (screen == nullptr)
        return {};

    // Capture the composited desktop: grabbing the transparent canvas alone loses
    // the screen color beneath annotations. Clip before calling the native reader.
    constexpr int radius = ScreenshotCanvasColorSampler::PreviewSize / 2 + 1;
    const QRect bounds =
        QRect(point - QPoint(radius, radius), QSize(radius * 2 + 1, radius * 2 + 1))
            .intersected(screen->geometry());
    QPoint origin = bounds.topLeft();
#ifndef Q_OS_MACOS
    origin -= screen->geometry().topLeft();
#endif
    const QImage raster =
        screen->grabWindow(0, origin.x(), origin.y(), bounds.width(), bounds.height()).toImage();
    if (raster.isNull())
        return {};
    const QRect pixelBounds(QPoint(), raster.size());
    const QPoint pixel = ScreenshotCanvasColorSampler::physicalPointForLocalPosition(
        globalPosition - bounds.topLeft(), bounds.size(), pixelBounds);
    return ScreenshotCanvasColorSampler::previewFromPhysicalRaster(raster, pixelBounds, pixel);
}
} // namespace

RecordingColorSampler::RecordingColorSampler(ScreenRecordingAreaWindow& area,
                                             ScreenRecordingToolbarWindow& toolbar,
                                             ReadPreview readPreview)
    : m_area(area), m_toolbar(toolbar),
      m_readPreview(readPreview ? std::move(readPreview) : readScreenPreview) {
    connect(toolbar.palette(), &ScreenshotToolPalette::canvasColorSamplingRequested, this,
            [this](adqt::widgets::AdColorPicker* picker) { begin(picker); });
    connect(&area, &ScreenRecordingAreaWindow::recordingRegionChanged, this, [this] { cancel(); });
    connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted, this,
            [this] { cancel(); });
    connect(area.canvas(), &SnowCanvasWidget::styleToolbarStateChanged, this, [this] { cancel(); });
}

RecordingColorSampler::~RecordingColorSampler() {
    cancel();
    qApp->removeEventFilter(this);
}

void RecordingColorSampler::begin(adqt::widgets::AdColorPicker* picker) {
    cancel();
    if (picker == nullptr || !m_area.isVisible() || m_area.drawingBlocked())
        return;
    if (m_preview == nullptr)
        m_preview = std::make_unique<ScreenshotCanvasColorSamplerWindow>();
    m_previousInputMode = m_area.inputMode();
    m_area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    m_target = picker;
    m_destroyedConnection = connect(picker, &QObject::destroyed, this, [this] { cancel(); });
    m_active = true;
    m_pressed = false;
    m_escapeRelease = false;
    m_preview->beginSampling(&m_area);
    QApplication::setOverrideCursor(ScreenshotCanvasColorSamplerWindow::samplingCursor());
    // Install after the recording shortcuts so sampling owns Escape and drawing keys.
    qApp->removeEventFilter(this);
    qApp->installEventFilter(this);
}

void RecordingColorSampler::cancel() {
    m_target.clear();
    disconnect(m_destroyedConnection);
    m_destroyedConnection = {};
    m_pressed = false;
    if (!m_active)
        return;
    m_active = false;
    m_preview->endSampling();
    QApplication::restoreOverrideCursor();
    m_area.setInputMode(m_previousInputMode);
}

bool RecordingColorSampler::owns(QWidget* widget) const {
    return widget != nullptr && (widget->window() == &m_area || widget->window() == &m_toolbar ||
                                 m_toolbar.isAncestorOf(widget));
}

QImage RecordingColorSampler::previewAt(const QPointF& globalPosition) {
    if (!m_area.canvas()->rect().contains(
            m_area.canvas()->mapFromGlobal(globalPosition).toPoint())) {
        m_preview->hide();
        return {};
    }
    // A fast pointer move can land on the previous HUD position. Do not sample it.
    if (m_preview->isVisible() && m_preview->geometry().contains(globalPosition.toPoint()))
        m_preview->hide();
    const QImage preview = m_readPreview(globalPosition);
    if (preview.isNull())
        m_preview->hide();
    else
        m_preview->updateSample(preview, globalPosition.toPoint());
    return preview;
}

bool RecordingColorSampler::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!owns(widget))
        return false;
    if (m_escapeRelease &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ||
         event->type() == QEvent::KeyRelease) &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        if (event->type() == QEvent::KeyRelease && !static_cast<QKeyEvent*>(event)->isAutoRepeat())
            m_escapeRelease = false;
        event->accept();
        return true;
    }
    if (!m_active)
        return false;
    if (m_area.drawingBlocked() ||
        (event->type() == QEvent::Hide && (widget == &m_area || widget == &m_toolbar))) {
        cancel();
        return false;
    }
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ||
        event->type() == QEvent::KeyRelease) {
        if (event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            m_escapeRelease = true;
            cancel();
        }
        event->accept();
        return true;
    }
    const bool canvasInput = widget == m_area.canvas() || widget == &m_area;
    if (event->type() == QEvent::Wheel && canvasInput)
        return true;
    if (event->type() == QEvent::MouseMove) {
        if (canvasInput)
            static_cast<void>(previewAt(static_cast<QMouseEvent*>(event)->globalPosition()));
        else
            m_preview->hide();
        return canvasInput;
    }
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick &&
        event->type() != QEvent::MouseButtonRelease)
        return false;
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (!canvasInput) {
        if (event->type() == QEvent::MouseButtonPress)
            cancel();
        return false;
    }
    if (event->type() != QEvent::MouseButtonRelease) {
        m_pressed = mouse->button() == Qt::LeftButton || mouse->button() == Qt::RightButton;
        return true;
    }
    if (!m_pressed)
        return true;
    const QPointer<adqt::widgets::AdColorPicker> picker = m_target;
    const QImage preview =
        mouse->button() == Qt::LeftButton ? previewAt(mouse->globalPosition()) : QImage();
    cancel();
    if (picker && !preview.isNull()) {
        picker->commitValue(adqt::widgets::AdColorValue::solid(
            preview.pixelColor(preview.width() / 2, preview.height() / 2)));
    }
    return true;
}

} // namespace snow_shot::presentation::recording
