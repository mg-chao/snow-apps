#include "snow_shot/presentation/canvashistoryshortcuts.h"
#include "snow_shot/presentation/globalcanvascontroller.h"
#include "globalcanvasplatform.h"
#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/platform/physicalcursor.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/presentation/screenshotwheelinput.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
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
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace snow_shot::presentation {
namespace {
ScreenshotToolPalette::Options canvasOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showHistoryActions = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showSpotlightTool = true;
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
        palette = tools->palette();
        applyScreenshotCanvasToolStyles(*drawing, screenshotCanvasToolStyleDefaults());
        new ScreenshotStyleBinding(*palette, *drawing, this);
        wireTools();
        shortcuts.addScopeWindow(this);
        shortcuts.addScopeWindow(tools.get());
        registerShortcuts();
        connect(palette, &ScreenshotToolPalette::globalCanvasClickThroughRequested, this,
                [this]() { toggle(); });
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
        drawing->update();
        activateDrawing();
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
    void resizeEvent(QResizeEvent* event) override {
        cancelColorSampling();
        QWidget::resizeEvent(event);
        const QTransform transform = drawing->canvasToViewTransform();
        const QPointF origin = cameraInitialized ? transform.inverted().map(QPointF()) : QPointF();
        const double zoom = cameraInitialized ? transform.m11() : 1.0;
        drawing->setGeometry(rect());
        drawing->setViewportCamera(origin.x() + width() / (2.0 * zoom),
                                   origin.y() + height() / (2.0 * zoom), zoom);
        cameraInitialized = true;
    }
    void closeEvent(QCloseEvent* event) override {
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
        if (watched == drawing.get() && samplingMouseReleasePending &&
            event->type() == QEvent::MouseButtonRelease) {
            samplingMouseReleasePending = false;
            return true;
        }
        if (watched == drawing.get()) {
            if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide ||
                event->type() == QEvent::FocusOut)
                finishPan();
            if (!transparent && !sampleTarget) {
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
        if (watched == drawing.get() && sampleTarget) {
            if (event->type() == QEvent::MouseMove) {
                updateColorSamplingPreview(static_cast<QMouseEvent*>(event)->position());
                return true;
            }
            if (event->type() == QEvent::Leave) {
                sampleWindow->hide();
            }
            if (event->type() == QEvent::Wheel)
                return true;
            if (event->type() == QEvent::MouseButtonPress) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                QPointer<adqt::widgets::AdColorPicker> target = sampleTarget;
                const QImage preview = colorSamplingPreview(mouse->position());
                samplingMouseReleasePending = true;
                cancelColorSampling();
                if (mouse->button() == Qt::LeftButton && target && !preview.isNull()) {
                    target->commitValue(adqt::widgets::AdColorValue::solid(
                        preview.pixelColor(preview.width() / 2, preview.height() / 2)));
                }
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
        if (!sampleWindow)
            sampleWindow = std::make_unique<ScreenshotCanvasColorSamplerWindow>();
        sampleTarget = picker;
        sampleDestroyedConnection =
            connect(picker, &QObject::destroyed, this, [this]() { cancelColorSampling(); });
        samplingMouseReleasePending = false;
        sampleWindow->beginSampling(picker);
        drawing->setCursorForLayer(SnowCanvasCursorLayer::Host,
                                   ScreenshotCanvasColorSamplerWindow::samplingCursor());
        activateDrawing();
        const QPointF globalPosition =
            platform::PhysicalCursor().logicalPosition().value_or(QCursor::pos());
        updateColorSamplingPreview(drawing->mapFromGlobal(globalPosition));
    }
    void cancelColorSampling() {
        if (!sampleTarget && sampleRaster.isNull())
            return;
        sampleTarget.clear();
        disconnect(sampleDestroyedConnection);
        sampleDestroyedConnection = {};
        sampleRaster = {};
        if (sampleWindow)
            sampleWindow->endSampling();
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
            sampleWindow->hide();
        } else {
            sampleWindow->updateSample(preview, drawing->mapToGlobal(localPosition).toPoint());
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
            return !sampleTarget && !drawing->hasActiveTextEditing();
        };
        exit.activate = [this](const auto&) {
            this->close();
            return true;
        };
        static_cast<void>(shortcuts.addBinding(this, std::move(exit)));
        new CanvasHistoryShortcuts(
            shortcuts, this,
            [this](const auto&) {
                return !transparent && !sampleTarget && !drawing->hasActiveTextEditing();
            },
            [this](const QString& action) { return palette->activateScreenshotShortcut(action); });
        const auto bindings = storage::DrawingShortcutSettings().allShortcuts();
        for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
            WindowShortcutManager::Binding binding;
            binding.id = QStringLiteral("global-canvas.drawing.") + it.key();
            binding.shortcutBindings = it.value();
            binding.priority = WindowShortcutManager::StandardPriority::DrawingShortcut;
            binding.canActivate = [this](const auto& context) {
                return !transparent && !sampleTarget && !drawing->hasActiveTextEditing() &&
                       !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget);
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
        switch (drawing->canvasTool()) {
        case SnowCanvasTool::Select:
            palette->setActiveTool(ScreenshotToolPalette::Tool::Select);
            break;
        case SnowCanvasTool::Shape:
            palette->setActiveTool(ScreenshotToolPalette::Tool::Shape);
            break;
        case SnowCanvasTool::Arrow:
            palette->setActiveTool(ScreenshotToolPalette::Tool::Arrow);
            break;
        case SnowCanvasTool::Line:
            palette->setActiveTool(ScreenshotToolPalette::Tool::Line);
            break;
        case SnowCanvasTool::FreeDraw:
            palette->setActiveTool(ScreenshotToolPalette::Tool::FreeDraw);
            break;
        case SnowCanvasTool::RectangleHighlight:
            palette->setActiveTool(ScreenshotToolPalette::Tool::RectangleHighlight);
            break;
        case SnowCanvasTool::PenHighlight:
            palette->setActiveTool(ScreenshotToolPalette::Tool::PenHighlight);
            break;
        case SnowCanvasTool::Spotlight:
            palette->setActiveTool(ScreenshotToolPalette::Tool::Spotlight);
            break;
        case SnowCanvasTool::Eraser:
            palette->setActiveTool(ScreenshotToolPalette::Tool::Eraser);
            break;
        case SnowCanvasTool::RectangleEraser:
            palette->setActiveTool(ScreenshotToolPalette::Tool::RectangleEraser);
            break;
        case SnowCanvasTool::BrushEraser:
            palette->setActiveTool(ScreenshotToolPalette::Tool::BrushEraser);
            break;
        case SnowCanvasTool::RectangleFilter:
            palette->setActiveTool(ScreenshotToolPalette::Tool::RectangleFilter);
            break;
        case SnowCanvasTool::PenFilter:
            palette->setActiveTool(ScreenshotToolPalette::Tool::PenFilter);
            break;
        case SnowCanvasTool::Watermark:
            palette->setActiveTool(ScreenshotToolPalette::Tool::Watermark);
            break;
        case SnowCanvasTool::Text:
            palette->setActiveTool(ScreenshotToolPalette::Tool::Text);
            break;
        case SnowCanvasTool::SerialNumber:
            palette->setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
            break;
        case SnowCanvasTool::AutoFilter:
            break;
        }
    }
    void wireTools() {
        connect(drawing.get(), &SnowCanvasWidget::activeToolChanged, this, [this]() {
            cancelColorSampling();
            synchronizeTool();
        });
        connect(palette, &ScreenshotToolPalette::selectRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::Select);
            palette->setActiveTool(ScreenshotToolPalette::Tool::Select);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::shapeRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::Shape);
            palette->setActiveTool(ScreenshotToolPalette::Tool::Shape);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::arrowRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::Arrow);
            palette->setActiveTool(ScreenshotToolPalette::Tool::Arrow);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::lineRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::Line);
            palette->setActiveTool(ScreenshotToolPalette::Tool::Line);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::freeDrawRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::FreeDraw);
            palette->setActiveTool(ScreenshotToolPalette::Tool::FreeDraw);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::highlightRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::RectangleHighlight);
            palette->setActiveTool(ScreenshotToolPalette::Tool::RectangleHighlight);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::penHighlightRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::PenHighlight);
            palette->setActiveTool(ScreenshotToolPalette::Tool::PenHighlight);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::spotlightRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::Spotlight);
            palette->setActiveTool(ScreenshotToolPalette::Tool::Spotlight);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::rectangleEraserRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::RectangleEraser);
            palette->setActiveTool(ScreenshotToolPalette::Tool::RectangleEraser);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::brushEraserRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::BrushEraser);
            palette->setActiveTool(ScreenshotToolPalette::Tool::BrushEraser);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::eraserRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::Eraser);
            palette->setActiveTool(ScreenshotToolPalette::Tool::Eraser);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::rectangleFilterRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::RectangleFilter);
            palette->setActiveTool(ScreenshotToolPalette::Tool::RectangleFilter);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::penFilterRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::PenFilter);
            palette->setActiveTool(ScreenshotToolPalette::Tool::PenFilter);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::watermarkRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::Watermark);
            palette->setActiveTool(ScreenshotToolPalette::Tool::Watermark);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::textRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::Text);
            palette->setActiveTool(ScreenshotToolPalette::Tool::Text);
            activateDrawing();
        });
        connect(palette, &ScreenshotToolPalette::serialNumberRequested, this, [this]() {
            drawing->setCanvasTool(SnowCanvasTool::SerialNumber);
            palette->setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
            activateDrawing();
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
    std::unique_ptr<ScreenshotFloatingToolPaletteWindow> tools;
    WindowShortcutManager shortcuts;
    QHash<QString, WindowShortcutManager::BindingHandle> drawingBindings;
    ScreenshotToolPalette* palette = nullptr;
    QPointer<QScreen> display;
    QPointer<adqt::widgets::AdColorPicker> sampleTarget;
    std::unique_ptr<ScreenshotCanvasColorSamplerWindow> sampleWindow;
    QImage sampleRaster;
    QMetaObject::Connection sampleDestroyedConnection;
    bool samplingMouseReleasePending = false;
    bool transparent = false;
    bool panning = false;
    bool cameraInitialized = false;
    QPointF panPosition;
    std::uint64_t revision = 0;
};

GlobalCanvasController::GlobalCanvasController(QObject* parent, Platform platform)
    : QObject(parent), m_platform(std::move(platform)) {
    if (!m_platform.pointerScreen)
        m_platform.pointerScreen = pointerScreen;
    if (!m_platform.setInputTransparent)
        m_platform.setInputTransparent = setGlobalCanvasInputTransparent;
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
