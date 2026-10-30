#include "snow_shot/presentation/screenshotcanvastools.h"
#include "snow_shot/presentation/canvashistoryshortcuts.h"
#include "snow_shot/presentation/canvastexteditingpreferences.h"
#include "snow_shot/presentation/canvasdrawingpreferences.h"
#include "snow_shot/presentation/globalcanvascontroller.h"
#include "snow_shot/presentation/crosshaircursor.h"
#include "globalcanvasplatform.h"
#include "../recording/recordingeffectssource.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/platform/physicalcursor.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/presentation/screenshotwheelinput.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "widgets/color_picker.h"
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <QWheelEvent>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace snow_shot::presentation {
namespace {
ScreenshotToolPalette::Options canvasOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showHistoryActions = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showSpeechBubbleTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showDistanceTool = true;
    options.showAngleTool = true;
    options.showBracketTool = true;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showSpotlightTool = true;
    // Magnifiers sample captured images; this live annotation surface has none.
    options.showMagnifierTool = false;
    options.showEraserTool = true;
    options.showFilterTool = true;
    options.showWatermarkTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showGlobalCanvasActions = true;
    options.separatorAfterSelect = true;
    options.toolbarLayout = storage::ScreenshotToolbarSettings().layout(
        storage::ScreenshotToolbarLayoutKind::DrawingTools);
    options.styleDefaults = screenshotCanvasToolStyleDefaults();
    return options;
}
QScreen* pointerScreen() {
    const QPointF position = platform::PhysicalCursor().logicalPosition().value_or(QCursor::pos());
    QScreen* screen = QGuiApplication::screenAt(position.toPoint());
    return screen != nullptr ? screen : QGuiApplication::primaryScreen();
}
} // namespace

class GlobalCanvasController::Session final : public QWidget, public SnowCanvasCustomRenderer {
  public:
    Session(GlobalCanvasController& controller, QScreen* screen)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
          owner(controller), runtime(SnowCanvasRuntimeConfig{screenshotCanvasToolStyleDefaults()}),
          drawing(std::make_unique<SnowCanvasWidget>(runtime, this)),
          tools(std::make_unique<ScreenshotFloatingToolPaletteWindow>(canvasOptions())),
          shortcuts(this) {
        configureCanvasCrosshairCursor(*drawing);
        bindCanvasTextEditingPreferences(*drawing);
#ifdef Q_OS_MACOS
        setWindowFlag(Qt::NoDropShadowWindowHint);
#endif
        setObjectName(QStringLiteral("globalCanvasWindow"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::StrongFocus);
        drawing->setObjectName(QStringLiteral("globalCanvasDrawing"));
        drawing->setAttribute(Qt::WA_TranslucentBackground);
        drawing->setAttribute(Qt::WA_NoSystemBackground);
        drawing->setAttribute(Qt::WA_OpaquePaintEvent, false);
        drawing->setClearBackgroundEnabled(false);
        drawing->setCustomRenderer(this);
        drawing->setWheelZoomEnabled(false);
        drawing->installEventFilter(this);
        tools->setObjectName(QStringLiteral("globalCanvasToolbar"));
        tools->setTransientOwnerWindow(this);
        tools->setStyleToolbarAboveMain(false);
        tools->installEventFilter(this);
        toolbarOpacityTimer.setObjectName(QStringLiteral("globalCanvasToolbarOpacityTimer"));
        toolbarOpacityTimer.setInterval(50);
        connect(&toolbarOpacityTimer, &QTimer::timeout, this, [this]() { updateToolbarOpacity(); });
        palette = tools->palette();
        applyScreenshotCanvasToolStyles(*drawing, screenshotCanvasToolStyleDefaults());
        new ScreenshotStyleBinding(*palette, *drawing, this);
        wireTools();
        shortcuts.addScopeWindow(this);
        shortcuts.addScopeWindow(tools.get());
        registerShortcuts();
        connect(palette, &ScreenshotToolPalette::globalCanvasClickThroughRequested, this,
                [this]() { toggle(); });
        connect(palette, &ScreenshotToolPalette::globalCanvasLaserPointerRequested, this,
                [this]() { setLaserEnabled(!laserEnabled); });
        connect(palette, &ScreenshotToolPalette::globalCanvasExitRequested, this,
                [this]() { close(); });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen* removed) {
            if (display == removed || display.isNull()) {
                QScreen* replacement = QGuiApplication::primaryScreen();
                if (replacement == removed) {
                    replacement = nullptr;
                    for (QScreen* candidate : QGuiApplication::screens()) {
                        if (candidate != removed) {
                            replacement = candidate;
                            break;
                        }
                    }
                }
                if (replacement) {
                    setDisplay(replacement);
                    // Qt finishes relocating top-level windows after screenRemoved
                    // returns and can restore their old extent. Reapply our fullscreen
                    // geometry after that relocation, retaining the document and mode.
                    QTimer::singleShot(0, this, [this]() {
                        if (!display)
                            return;
                        updateGeometry();
                        show();
                        tools->show();
                        tools->raise();
                    });
                } else {
                    close();
                }
            }
        });
        bindCanvasDrawingPreferences(runtime, *this);
        auto& storage = storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            connect(&storage.configuration(), &storage::ConfigurationStore::valueChanged, this,
                    [this](const QString& key, const QJsonValue&) {
                        if (key.startsWith(QStringLiteral("drawing_shortcuts/"))) {
                            reloadShortcuts();
                        } else if (key == QStringLiteral("screenshot_toolbar/layout")) {
                            palette->setToolbarLayout(storage::ScreenshotToolbarSettings().layout(
                                storage::ScreenshotToolbarLayoutKind::DrawingTools));
                        }
                    });
        }
#ifdef Q_OS_MACOS
        // Install native level and frame policy before setting the display geometry.
        platform::configureGlobalCanvasWindow(this);
#endif
        setDisplay(screen);
        drawing->setCanvasTool(SnowCanvasTool::Select);
        palette->setActiveTool(ScreenshotToolPalette::Tool::Select);
        palette->setHistoryState(drawing->canvasHistoryState());
        palette->setStyleToolbarState(drawing->canvasStyleToolbarState());
        show();
        drawing->show();
        tools->prepareForDisplay();
        placeToolbar(true);
        tools->show();
        tools->raise();
        activateDrawing();
    }
    ~Session() override {
        tools->removeEventFilter(this);
        stopLaser();
        cancelColorSampling();
        drawing->setCustomRenderer(nullptr);
    }
    void activateDrawing() {
        if (!transparent) {
            activateWindow();
            drawing->setFocus(Qt::OtherFocusReason);
            tools->raise();
        }
    }
    void toggle() {
        const bool next = !transparent;
        finishPan();
        drawing->resetEditingStatePreservingTool();
        cancelColorSampling();
        if (!owner.m_platform.setInputTransparent(this, next)) {
            // A failed native operation may have changed only some flags.
            owner.m_platform.setInputTransparent(this, transparent);
            palette->setGlobalCanvasClickThrough(transparent);
            emit owner.errorOccurred(
                GlobalCanvasController::tr("Could not change canvas click-through."));
            return;
        }
        transparent = next;
        ++revision;
        setAttribute(Qt::WA_TransparentForMouseEvents, transparent);
        setAttribute(Qt::WA_ShowWithoutActivating, transparent);
        drawing->setInteractionEnabled(!transparent);
        if (transparent)
            drawing->clearFocus();
        palette->setGlobalCanvasClickThrough(transparent);
        if (transparent && tools->isVisible())
            toolbarOpacityTimer.start();
        else
            toolbarOpacityTimer.stop();
        updateToolbarOpacity();
        drawing->update();
        activateDrawing();
    }
    void updateToolbarOpacity() {
        // Poll only in click-through mode: the pointer can move into another
        // application without delivering mouse events to the canvas.
        const QPoint position =
            platform::PhysicalCursor().logicalPosition().value_or(QCursor::pos()).toPoint();
        const qreal opacity =
            !transparent || tools->containsInteractiveGlobalPoint(position) ? 1.0 : 0.32;
        if (qAbs(tools->windowOpacity() - opacity) > 0.01)
            tools->setWindowOpacity(opacity);
    }
    void setDisplay(QScreen* screen) {
        if (display)
            disconnect(display, nullptr, this, nullptr);
        display = screen;
        if (windowHandle())
            windowHandle()->setScreen(screen);
        connect(screen, &QScreen::geometryChanged, this, [this]() { updateGeometry(); });
        connect(screen, &QScreen::availableGeometryChanged, this,
                [this]() { placeToolbar(false); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this]() { updateGeometry(); });
        updateGeometry();
    }
    void updateGeometry() {
        if (!display)
            return;
        setGeometry(display->geometry());
        placeToolbar(false);
        synchronizeLaser();
    }
    void placeToolbar(bool initial) {
        if (!display)
            return;
        const QRect bounds = display->availableGeometry();
        tools->setPlacementContext(display, bounds);
        const QSize size = tools->bottomPlacementContentRect().size();
        const QPoint target = initial ? QPoint(bounds.right() - size.width() - 15,
                                               bounds.bottom() - size.height() - 15)
                                      : tools->contentPosition();
        tools->moveContentTo(tools->constrainedContentPosition(target));
    }
    std::uint64_t contentRevision() const override {
        return revision;
    }
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        QColor surface(Qt::transparent);
#ifdef Q_OS_WIN
        // Layered windows pass input through zero-alpha pixels, even in editing mode.
        if (!transparent)
            surface = QColor(0, 0, 0, 2);
#endif
        painter.fillRect(context.exposedRegion.boundingRect(), surface);
        painter.restore();
    }
    QTransform laserTransform(const RecordingEffectsFrame& frame,
                              const RecordingEffectsFrame::Tile& tile) const {
        const QSize size = tile.coordinateSize.isEmpty() ? frame.output : tile.coordinateSize;
        return QTransform::fromScale(static_cast<qreal>(drawing->width()) / size.width(),
                                     static_cast<qreal>(drawing->height()) / size.height());
    }
    QRegion laserRegion(const RecordingEffectsFrame& frame) const {
        QRegion region;
        if (frame.output.isEmpty())
            return region;
        for (const auto& tile : frame.tiles) {
            region += laserTransform(frame, tile)
                          .mapRect(QRectF(tile.rect))
                          .toAlignedRect()
                          .adjusted(-1, -1, 1, 1);
        }
        return region.intersected(drawing->rect());
    }
    void renderAfterCanvas(QPainter& painter, const SnowCanvasRenderContext&) override {
        if (!laserFrame || laserFrame->output.isEmpty())
            return;
        const QTransform transform = painter.transform();
        for (const auto& tile : laserFrame->tiles) {
            painter.setTransform(transform);
            painter.setTransform(laserTransform(*laserFrame, tile), true);
            painter.drawImage(tile.rect.topLeft(), tile.image);
        }
    }
    void clearLaserFrame() {
        if (!laserFrame)
            return;
        const QRegion damage = laserRegion(*laserFrame);
        laserFrame.reset();
        drawing->update(damage);
    }
    void stopLaser() {
        ++laserGeneration;
        laserRunning = false;
        if (laserSource)
            laserSource->stop();
        clearLaserFrame();
    }
    void setLaserEnabled(bool enabled) {
        laserEnabled = enabled;
        palette->setGlobalCanvasLaserPointer(laserEnabled);
        if (enabled)
            synchronizeLaser();
        else
            stopLaser();
    }
    void laserFailed(const QString& error) {
        setLaserEnabled(false);
        emit owner.errorOccurred(
            GlobalCanvasController::tr("Could not show the laser pointer: %1").arg(error));
    }
    void receiveLaserFrame() {
        if (!laserRunning)
            return;
        auto frame = laserSource->acquire();
        if (!frame || frame->generation != laserGeneration ||
            (laserFrame && frame->revision <= laserFrame->revision))
            return;
        if (!frame->error.isEmpty()) {
            laserFailed(frame->error);
            return;
        }
        QRegion damage = laserRegion(*frame);
        if (laserFrame)
            damage += laserRegion(*laserFrame);
        laserFrame = std::move(frame);
        if (!damage.isEmpty())
            drawing->update(damage);
    }
    void synchronizeLaser() {
        if (!laserEnabled || !isVisible() || !display)
            return;
        const QRect logical(drawing->mapToGlobal(QPoint()), drawing->size());
#ifdef Q_OS_MACOS
        const QRect capture = logical;
        const qreal scale = drawing->devicePixelRatioF();
#else
        const QRect capture = ScreenshotGeometryMapper::nativeRectForLogicalRect(
            logical, display->geometry(),
            ScreenshotGeometryMapper::physicalRectForScreen(*display));
        const qreal scale = 1.0;
#endif
        if (capture.isEmpty()) {
            stopLaser();
            return;
        }
        const qreal devicePixelRatio = drawing->devicePixelRatioF();
        if (laserRunning && laserCapture == capture && laserScale == scale &&
            laserDevicePixelRatio == devicePixelRatio)
            return;
        if (!laserSource)
            laserSource = owner.m_platform.createEffectsSource();
        if (!laserSource) {
            laserFailed(GlobalCanvasController::tr("The laser pointer is unavailable."));
            return;
        }
        ++laserGeneration;
        clearLaserFrame();
        laserCapture = capture;
        laserScale = scale;
        laserDevicePixelRatio = devicePixelRatio;
        SnowRecordingEffectsConfig config{};
        config.version = SNOW_RECORDING_EFFECTS_CONFIG_VERSION;
        config.struct_size = sizeof(config);
        config.x = capture.x();
        config.y = capture.y();
        config.width = static_cast<uint32_t>(capture.width());
        config.height = static_cast<uint32_t>(capture.height());
        config.output_width = static_cast<uint32_t>(qRound(capture.width() * scale));
        config.output_height = static_cast<uint32_t>(qRound(capture.height() * scale));
        config.canvas_scale = scale;
        config.trail_rgba = 0xff0000ff;
        config.trail_duration_ms = 500;
        config.generation = laserGeneration;
        // The native API validates these dimensions even with their effects disabled.
        config.keyboard_size = 64;
        config.highlight_radius = 41;
        QString error;
        const bool success = laserRunning ? laserSource->configure(config, error)
                                          : laserSource->start(
                                                config,
                                                [this]() {
                                                    if (!laserNotificationPending.exchange(true)) {
                                                        QMetaObject::invokeMethod(
                                                            this,
                                                            [this]() {
                                                                laserNotificationPending = false;
                                                                receiveLaserFrame();
                                                            },
                                                            Qt::QueuedConnection);
                                                    }
                                                },
                                                error);
        laserRunning = success;
        if (!success)
            laserFailed(error);
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        // Qt also sends resize events for safe-area margin changes. Only an
        // actual drawing-surface change invalidates sampling and the camera.
        if (cameraInitialized && drawing->geometry() == rect())
            return;
        cancelColorSampling();
        const QTransform transform = drawing->canvasToViewTransform();
        const QPointF origin = cameraInitialized ? transform.inverted().map(QPointF()) : QPointF();
        const double zoom = cameraInitialized ? transform.m11() : 1.0;
        drawing->setGeometry(rect());
        drawing->setViewportCamera(origin.x() + width() / (2.0 * zoom),
                                   origin.y() + height() / (2.0 * zoom), zoom);
        cameraInitialized = true;
        synchronizeLaser();
    }
    void closeEvent(QCloseEvent* event) override {
        setLaserEnabled(false);
        cancelColorSampling();
        tools->hide();
        QWidget::closeEvent(event);
        QPointer<Session> session(this);
        QTimer::singleShot(0, &owner, [session, controller = &owner]() {
            if (session && controller->m_session.get() == session)
                controller->shutdown();
        });
    }
    bool event(QEvent* event) override {
        const bool result = QWidget::event(event);
        if (event->type() == QEvent::Hide)
            stopLaser();
        else if (event->type() == QEvent::Show || event->type() == QEvent::Move ||
                 event->type() == QEvent::DevicePixelRatioChange)
            synchronizeLaser();
        if (event->type() == QEvent::WinIdChange && internalWinId() != 0) {
            // Moving between displays can recreate the native surface. Qt's mouse
            // attribute alone does not preserve native input transparency.
            QTimer::singleShot(0, this, [this]() {
                if (!owner.m_platform.setInputTransparent(this, transparent))
                    emit owner.errorOccurred(
                        GlobalCanvasController::tr("Could not change canvas click-through."));
            });
        }
        return result;
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == tools.get()) {
            if (event->type() == QEvent::Hide) {
                toolbarOpacityTimer.stop();
            } else if (event->type() == QEvent::Show) {
                if (transparent)
                    toolbarOpacityTimer.start();
                updateToolbarOpacity();
            }
        }
        if (watched == drawing.get() &&
            (event->type() == QEvent::TabletPress || event->type() == QEvent::TabletMove ||
             event->type() == QEvent::TabletRelease)) {
            auto* tablet = static_cast<QTabletEvent*>(event);
            const auto type = event->type() == QEvent::TabletPress     ? QEvent::MouseButtonPress
                              : event->type() == QEvent::TabletRelease ? QEvent::MouseButtonRelease
                                                                       : QEvent::MouseMove;
            QMouseEvent pointer(type, tablet->position(), tablet->globalPosition(),
                                tablet->button(), tablet->buttons(), tablet->modifiers(),
                                tablet->pointingDevice());
            if (eventFilter(watched, &pointer)) {
                event->accept();
                return true;
            }
            return false;
        }
        if (watched == drawing.get() && samplingMouseReleasePending &&
            event->type() == QEvent::MouseButtonRelease) {
            samplingMouseReleasePending = false;
            return true;
        }
        if (watched == drawing.get()) {
            if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress)
                angleWheelSteps.reset();
            if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide ||
                event->type() == QEvent::FocusOut)
                finishPan();
            if (!transparent && !colorSampling.active()) {
                if (event->type() == QEvent::Wheel && angleWheelTarget(*drawing)) {
                    auto* wheel = static_cast<QWheelEvent*>(event);
                    if (handleScreenshotStyleWheel(*tools->palette(), *drawing, *wheel,
                                                   angleWheelSteps)) {
                        wheel->accept();
                        return true;
                    }
                } else if (event->type() == QEvent::Wheel) {
                    angleWheelSteps.reset();
                }
                if (handleNavigation(event))
                    return true;
                if (event->type() == QEvent::Wheel &&
                    drawing->canvasTool() == SnowCanvasTool::BrushEraser) {
                    auto* wheel = static_cast<QWheelEvent*>(event);
                    if (wheel->modifiers() == Qt::NoModifier) {
                        const int delta = usesPreciseWheelDelta(*wheel) ? wheel->pixelDelta().y()
                                                                        : wheel->angleDelta().y();
                        if (delta != 0 &&
                            stepScreenshotStyle(*tools->palette(), *drawing, delta > 0 ? 1 : -1)) {
                            wheel->accept();
                            return true;
                        }
                    }
                }
            }
        }
        if (watched == drawing.get() && colorSampling.active()) {
            if (event->type() == QEvent::MouseMove) {
                updateColorSamplingPreview(static_cast<QMouseEvent*>(event)->position());
                return true;
            }
            if (event->type() == QEvent::Leave) {
                colorSampling.hidePreview();
            }
            if (event->type() == QEvent::Wheel)
                return true;
            if (event->type() == QEvent::MouseButtonPress) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                const QImage preview = colorSamplingPreview(mouse->position());
                samplingMouseReleasePending = true;
                if (mouse->button() == Qt::LeftButton)
                    static_cast<void>(
                        colorSampling.commit(preview, [this]() { cancelColorSampling(); }));
                else
                    cancelColorSampling();
                return true;
            }
            if (event->type() == QEvent::KeyPress &&
                static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                cancelColorSampling();
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }
    void beginColorSampling(adqt::widgets::AdColorPicker* picker) {
        if (transparent || !picker)
            return;
        cancelColorSampling();
        finishPan();
        const QRectF area =
            drawing->canvasToViewTransform().inverted().mapRect(QRectF(drawing->rect()));
        // Sample annotation pixels at the display's resolution, without capturing
        // the desktop or the toolbar through this transparent canvas.
        sampleRaster =
            runtime.renderToImage(area, drawing->size() * drawing->devicePixelRatioF(), {});
        if (sampleRaster.isNull())
            return;
        colorSampling.begin(picker, [this]() { cancelColorSampling(); });
        samplingMouseReleasePending = false;
        drawing->setCursorForLayer(SnowCanvasCursorLayer::Host,
                                   ScreenshotCanvasColorSamplerWindow::samplingCursor());
        activateDrawing();
        const QPointF globalPosition =
            platform::PhysicalCursor().logicalPosition().value_or(QCursor::pos());
        updateColorSamplingPreview(drawing->mapFromGlobal(globalPosition));
    }
    void cancelColorSampling() {
        if (!colorSampling.active() && sampleRaster.isNull())
            return;
        sampleRaster = {};
        colorSampling.end();
        drawing->clearCursorForLayer(SnowCanvasCursorLayer::Host);
    }
    QImage colorSamplingPreview(const QPointF& localPosition) const {
        if (!drawing->rect().contains(localPosition.toPoint()) || sampleRaster.isNull())
            return {};
        const QPoint pixel = ScreenshotCanvasColorSampler::physicalPointForLocalPosition(
            localPosition, drawing->size(), sampleRaster.rect());
        return ScreenshotCanvasColorSampler::previewFromPhysicalRaster(sampleRaster,
                                                                       sampleRaster.rect(), pixel);
    }
    void updateColorSamplingPreview(const QPointF& localPosition) {
        const QImage preview = colorSamplingPreview(localPosition);
        if (preview.isNull()) {
            colorSampling.hidePreview();
        } else {
            colorSampling.updatePreview(preview, drawing->mapToGlobal(localPosition).toPoint());
        }
    }
    void finishPan() {
        if (!panning)
            return;
        panning = false;
        drawing->clearCursorForLayer(SnowCanvasCursorLayer::Host);
        if (QWidget::mouseGrabber() == drawing.get())
            drawing->releaseMouse();
    }
    void panBy(const QPointF& delta) {
        const QTransform transform = drawing->canvasToViewTransform();
        const QPointF center = transform.inverted().map(
            QPointF(drawing->width() / 2.0, drawing->height() / 2.0) - delta);
        drawing->setViewportCamera(center.x(), center.y(), transform.m11());
    }
    bool handleNavigation(QEvent* event) {
        if (event->type() == QEvent::Wheel) {
            auto* wheel = static_cast<QWheelEvent*>(event);
            const auto modifiers = wheel->modifiers();
            if (modifiers != Qt::ControlModifier && modifiers != Qt::ShiftModifier &&
                modifiers != (Qt::ControlModifier | Qt::ShiftModifier))
                return false;
            const bool precise = usesPreciseWheelDelta(*wheel);
            const QPoint delta = precise ? wheel->pixelDelta() : wheel->angleDelta();
            const double amount = delta.y() != 0 ? delta.y() : delta.x();
            if (modifiers.testFlag(Qt::ShiftModifier)) {
                const double distance = precise ? amount : amount / 120.0 * 40.0;
                panBy(modifiers.testFlag(Qt::ControlModifier) ? QPointF(0, distance)
                                                              : QPointF(distance, 0));
            } else {
                const QTransform transform = drawing->canvasToViewTransform();
                const double zoom =
                    std::clamp(transform.m11() * std::exp(amount * 0.001), 0.1, 8.0);
                const QPointF anchor = transform.inverted().map(wheel->position());
                const QPointF center =
                    anchor +
                    (QPointF(drawing->width() / 2.0, drawing->height() / 2.0) - wheel->position()) /
                        zoom;
                drawing->setViewportCamera(center.x(), center.y(), zoom);
            }
            wheel->accept();
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton && mouse->buttons() == Qt::MiddleButton) {
                drawing->resetEditingStatePreservingTool();
                drawing->setFocus(Qt::MouseFocusReason);
                panPosition = mouse->position();
                panning = true;
                drawing->setCursorForLayer(SnowCanvasCursorLayer::Host, Qt::ClosedHandCursor);
                drawing->grabMouse();
                mouse->accept();
                return true;
            }
        }
        if (panning && event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (!mouse->buttons().testFlag(Qt::MiddleButton)) {
                finishPan();
                return false;
            }
            panBy(mouse->position() - panPosition);
            panPosition = mouse->position();
            mouse->accept();
            return true;
        }
        if (panning && event->type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent*>(event)->button() == Qt::MiddleButton) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            panBy(mouse->position() - panPosition);
            finishPan();
            mouse->accept();
            return true;
        }
        return false;
    }
    void registerShortcuts() {
#ifdef Q_OS_MACOS
        WindowShortcutManager::Binding closeBinding;
        closeBinding.id = QStringLiteral("global-canvas.close");
        for (const auto& sequence : QKeySequence::keyBindings(QKeySequence::Close))
            closeBinding.keyCombinations.append(sequence[0]);
        closeBinding.priority = WindowShortcutManager::StandardPriority::WindowCommand;
        closeBinding.activationTrigger = WindowShortcutManager::Binding::ActivationTrigger::Release;
        closeBinding.activate = [this](const auto&) {
            this->close();
            return true;
        };
        static_cast<void>(shortcuts.addBinding(this, std::move(closeBinding)));
#endif
        WindowShortcutManager::Binding exit;
        exit.id = QStringLiteral("global-canvas.exit");
        exit.keyCombinations = {QKeyCombination(Qt::Key_Escape)};
        exit.priority = WindowShortcutManager::StandardPriority::WindowCommand;
        exit.canActivate = [this](const auto&) {
            return !colorSampling.active() && !drawing->hasActiveTextEditing();
        };
        exit.activate = [this](const auto&) {
            this->close();
            return true;
        };
        static_cast<void>(shortcuts.addBinding(this, std::move(exit)));
        new CanvasHistoryShortcuts(
            shortcuts, this,
            [this](const auto&) {
                return !transparent && !colorSampling.active() && !drawing->hasActiveTextEditing();
            },
            [this](const QString& action) { return palette->activateScreenshotShortcut(action); });
        const auto bindings = storage::DrawingShortcutSettings().allShortcuts();
        for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
            WindowShortcutManager::Binding binding;
            binding.id = QStringLiteral("global-canvas.drawing.") + it.key();
            binding.shortcutBindings = it.value();
            binding.priority = WindowShortcutManager::StandardPriority::DrawingShortcut;
            binding.canActivate = [this, id = it.key()](const auto& context) {
                return !transparent && !colorSampling.active() &&
                       !drawing->hasActiveTextEditing() &&
                       !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget) &&
                       palette->canActivateDrawingShortcut(id);
            };
            binding.activate = [this, id = it.key()](const auto&) {
                return palette->activateDrawingShortcut(id);
            };
            drawingBindings.insert(it.key(), shortcuts.addBinding(this, std::move(binding)));
        }
    }
    void reloadShortcuts() {
        for (auto it = drawingBindings.cbegin(); it != drawingBindings.cend(); ++it)
            static_cast<void>(shortcuts.setShortcuts(
                it.value(), storage::DrawingShortcutSettings().shortcuts(it.key())));
    }
    void synchronizeTool() {
        if (drawing->canvasTool() == SnowCanvasTool::AutoFilter ||
            drawing->canvasTool() == SnowCanvasTool::Magnifier)
            return;
        if (const auto tool = paletteToolForCanvasTool(drawing->canvasTool()))
            palette->setActiveTool(*tool);
    }

    void wireTools() {
        connect(drawing.get(), &SnowCanvasWidget::angleAdjustmentTargetChanged, this,
                [this]() { angleWheelSteps.reset(); });
        connect(drawing.get(), &SnowCanvasWidget::activeToolChanged, this, [this]() {
            angleWheelSteps.reset();
            cancelColorSampling();
            synchronizeTool();
        });
        bindScreenshotCanvasToolRequests(
            *palette, *this,
            [this](SnowCanvasTool tool) {
                drawing->setCanvasTool(tool);
                synchronizeTool();
                activateDrawing();
            },
            [](SnowCanvasTool tool) {
                return tool != SnowCanvasTool::AutoFilter && tool != SnowCanvasTool::Magnifier;
            });
        connect(palette, &ScreenshotToolPalette::serialNumberDecrementRequested, this,
                [this]() { drawing->adjustSelectedSerialNumbers(-1); });
        connect(palette, &ScreenshotToolPalette::serialNumberIncrementRequested, this,
                [this]() { drawing->adjustSelectedSerialNumbers(1); });
        connect(palette, &ScreenshotToolPalette::serialNumberCreateTextRequested, this,
                [this]() { drawing->createSerialNumberText(); });
        connect(palette, &ScreenshotToolPalette::sendSelectionToBackRequested, this,
                [this]() { drawing->reorderSelected(SnowCanvasSelectionOrder::SendToBack); });
        connect(palette, &ScreenshotToolPalette::sendSelectionBackwardRequested, this,
                [this]() { drawing->reorderSelected(SnowCanvasSelectionOrder::SendBackward); });
        connect(palette, &ScreenshotToolPalette::bringSelectionForwardRequested, this,
                [this]() { drawing->reorderSelected(SnowCanvasSelectionOrder::BringForward); });
        connect(palette, &ScreenshotToolPalette::bringSelectionToFrontRequested, this,
                [this]() { drawing->reorderSelected(SnowCanvasSelectionOrder::BringToFront); });
        connect(palette, &ScreenshotToolPalette::alignSelectionLeftRequested, this,
                [this]() { drawing->alignSelected(SnowCanvasSelectionAlignment::AlignLeft); });
        connect(palette, &ScreenshotToolPalette::alignSelectionCenterHorizontallyRequested, this,
                [this]() {
                    drawing->alignSelected(SnowCanvasSelectionAlignment::AlignCenterHorizontally);
                });
        connect(palette, &ScreenshotToolPalette::alignSelectionRightRequested, this,
                [this]() { drawing->alignSelected(SnowCanvasSelectionAlignment::AlignRight); });
        connect(palette, &ScreenshotToolPalette::alignSelectionTopRequested, this,
                [this]() { drawing->alignSelected(SnowCanvasSelectionAlignment::AlignTop); });
        connect(palette, &ScreenshotToolPalette::alignSelectionCenterVerticallyRequested, this,
                [this]() {
                    drawing->alignSelected(SnowCanvasSelectionAlignment::AlignCenterVertically);
                });
        connect(palette, &ScreenshotToolPalette::alignSelectionBottomRequested, this,
                [this]() { drawing->alignSelected(SnowCanvasSelectionAlignment::AlignBottom); });
        connect(palette, &ScreenshotToolPalette::distributeSelectionHorizontallyRequested, this,
                [this]() {
                    drawing->alignSelected(SnowCanvasSelectionAlignment::DistributeHorizontally);
                });
        connect(palette, &ScreenshotToolPalette::distributeSelectionVerticallyRequested, this,
                [this]() {
                    drawing->alignSelected(SnowCanvasSelectionAlignment::DistributeVertically);
                });
        connect(palette, &ScreenshotToolPalette::selectionOpacityChanged, this,
                [this](qreal opacity) { drawing->setSelectedOpacity(opacity); });
        connect(palette, &ScreenshotToolPalette::duplicateSelectionRequested, this,
                [this]() { drawing->duplicateSelected(); });
        connect(palette, &ScreenshotToolPalette::deleteSelectionRequested, this,
                [this]() { drawing->deleteSelected(); });
        connect(palette, &ScreenshotToolPalette::resetCanvasRequested, this,
                [this]() { drawing->deleteAllElements(); });

        connect(palette, &ScreenshotToolPalette::textStylePopupInteractionBegan, this,
                [this]() { drawing->beginTextStylePopupInteraction(); });
        connect(palette, &ScreenshotToolPalette::textStylePopupInteractionEnded, this,
                [this]() { drawing->endTextStylePopupInteraction(tools.get()); });

        connect(palette, &ScreenshotToolPalette::undoRequested, this,
                [this]() { drawing->undo(); });
        connect(palette, &ScreenshotToolPalette::redoRequested, this,
                [this]() { drawing->redo(); });
        connect(drawing.get(), &SnowCanvasWidget::historyStateChanged, this, [this]() {
            cancelColorSampling();
            palette->setHistoryState(drawing->canvasHistoryState());
        });
        connect(drawing.get(), &SnowCanvasWidget::styleToolbarStateChanged, this, [this]() {
            cancelColorSampling();
            palette->setStyleToolbarState(drawing->canvasStyleToolbarState());
        });
        connect(palette, &ScreenshotToolPalette::watermarkPreviewChanged, this,
                [this](const SnowCanvasWatermarkConfig& config) {
                    drawing->previewCanvasWatermarkConfig(config);
                });
        connect(palette, &ScreenshotToolPalette::spotlightPreviewChanged, this,
                [this](const SnowCanvasSpotlightConfig& config) {
                    drawing->previewCanvasSpotlightConfig(config);
                });
        palette->setWatermarkConfig(drawing->canvasWatermarkConfig());
        palette->setSpotlightConfig(drawing->canvasSpotlightConfig());
        palette->setAutoFilterAvailable(false);
        palette->setDrawTemplateCallbacks(
            [this]() { return runtime.serializeSelectedDrawTemplate(); },
            [this](const QByteArray& payload) {
                const QPointF center = drawing->canvasToViewTransform().inverted().map(
                    QRectF(drawing->rect()).center());
                drawing->insertDrawTemplate(payload, center);
            });
        connect(palette, &ScreenshotToolPalette::canvasColorSamplingRequested, this,
                [this](adqt::widgets::AdColorPicker* picker) { beginColorSampling(picker); });
    }
    GlobalCanvasController& owner;
    SnowCanvasRuntime runtime;
    std::unique_ptr<SnowCanvasWidget> drawing;
    WheelStepAccumulator angleWheelSteps;
    std::unique_ptr<ScreenshotFloatingToolPaletteWindow> tools;
    QTimer toolbarOpacityTimer{this};
    WindowShortcutManager shortcuts;
    QHash<QString, WindowShortcutManager::BindingHandle> drawingBindings;
    ScreenshotToolPalette* palette = nullptr;
    QPointer<QScreen> display;
    ScreenshotCanvasColorSamplingSession colorSampling{*this};
    QImage sampleRaster;
    bool samplingMouseReleasePending = false;
    bool transparent = false;
    bool panning = false;
    bool cameraInitialized = false;
    QPointF panPosition;
    std::uint64_t revision = 0;
    std::unique_ptr<RecordingEffectsSource> laserSource;
    std::shared_ptr<RecordingEffectsFrame> laserFrame;
    std::atomic<bool> laserNotificationPending = false;
    quint64 laserGeneration = 0;
    QRect laserCapture;
    qreal laserScale = 1.0;
    qreal laserDevicePixelRatio = 1.0;
    bool laserEnabled = false;
    bool laserRunning = false;
};

GlobalCanvasController::GlobalCanvasController(QObject* parent)
    : GlobalCanvasController(parent, Platform{}) {}

GlobalCanvasController::GlobalCanvasController(QObject* parent, Platform platform)
    : QObject(parent), m_platform(std::move(platform)) {
    if (!m_platform.pointerScreen)
        m_platform.pointerScreen = pointerScreen;
    if (!m_platform.setInputTransparent)
        m_platform.setInputTransparent = setGlobalCanvasInputTransparent;
    if (!m_platform.createEffectsSource)
        m_platform.createEffectsSource = createNativeRecordingEffectsSource;
    connect(qApp, &QCoreApplication::aboutToQuit, this, &GlobalCanvasController::shutdown);
}
GlobalCanvasController::~GlobalCanvasController() = default;
void GlobalCanvasController::activate() {
    if (m_session) {
        m_session->toggle();
        return;
    }
    QScreen* screen = m_platform.pointerScreen();
    if (!screen)
        return;
    m_session = std::make_unique<Session>(*this, screen);
    emit activeChanged(true);
}
void GlobalCanvasController::shutdown() {
    if (!m_session)
        return;
    m_session.reset();
    emit activeChanged(false);
}
bool GlobalCanvasController::active() const {
    return m_session != nullptr;
}
bool GlobalCanvasController::clickThrough() const {
    return m_session && m_session->transparent;
}
QWidget* GlobalCanvasController::window() const {
    return m_session.get();
}
SnowCanvasWidget* GlobalCanvasController::canvas() const {
    return m_session ? m_session->drawing.get() : nullptr;
}
ScreenshotFloatingToolPaletteWindow* GlobalCanvasController::toolbar() const {
    return m_session ? m_session->tools.get() : nullptr;
}
} // namespace snow_shot::presentation
