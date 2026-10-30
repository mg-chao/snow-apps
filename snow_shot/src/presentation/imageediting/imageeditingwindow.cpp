// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/canvasdrawingpreferences.h"
#include "imageeditingexportcontroller.h"
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/components/canvasactiontoolbar.h"
#include "antd_icons.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/screenshotdrawingtoolbaroptions.h"
#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "widgets/color_picker.h"
#include "widgets/select.h"
#include <QKeyEvent>
#include <QtMath>
#include <QMouseEvent>
#include "snow_shot/presentation/screenshotcanvasoperations.h"
#include "snow_shot/presentation/screenshotcanvastools.h"
#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/pinnedgeometry.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotmessageservice.h"
#include "snow_shot/presentation/screenshotautofiltercontroller.h"
#include "snow_shot/presentation/canvashistoryshortcuts.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "widgets/modal.h"
#include "widgets/message.h"
#include "icon_renderer.h"
#include "widgets/button.h"
#include <QApplication>
#include <QCursor>
#include <QMimeData>
#include <QPainter>
#include <QScreen>
#include <QScopedValueRollback>
#include <QStackedLayout>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QDataStream>
#include <QCryptographicHash>
#include <algorithm>
#include <utility>

namespace snow_shot::presentation {
using adqt::widgets::AdButton;
using adqt::widgets::AdMessage;
using adqt::widgets::AdMessageHandle;
using adqt::widgets::AdMessageService;
using adqt::widgets::AdModal;
using Session = ScreenshotRecognitionSessionController;
using Tool = ScreenshotToolPalette::Tool;
using ExportOperation = ImageEditingExportController::Operation;
namespace {
class PreviewSurface final : public QWidget {
  public:
    explicit PreviewSurface(QWidget* parent = nullptr) : QWidget(parent) {
        connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, this,
                [this] { update(); });
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto theme = styles::ThemeManager::instance().themeColorScheme().map;
        constexpr int tile = 10;
        if (m_background != theme.colorBgContainer || m_foreground != theme.colorFillSecondary ||
            m_checkerboard.style() == Qt::NoBrush) {
            m_background = theme.colorBgContainer;
            m_foreground = theme.colorFillSecondary;
            QPixmap pattern(tile * 2, tile * 2);
            pattern.fill(m_background);
            QPainter patternPainter(&pattern);
            patternPainter.fillRect(QRect(tile, 0, tile, tile), m_foreground);
            patternPainter.fillRect(QRect(0, tile, tile, tile), m_foreground);
            patternPainter.end();
            m_checkerboard = QBrush(pattern);
        }
        painter.fillRect(rect(), m_checkerboard);
    }

  private:
    QBrush m_checkerboard;
    QColor m_background;
    QColor m_foreground;
};
ScreenshotToolPalette::Options editorOptions() {
    auto options = screenshotDrawingToolbarOptions();
    options.moveToolPresentation = ScreenshotToolPalette::MoveToolPresentation::Pan;
    options.showCropTools = true;
    options.actions = ScreenshotToolPalette::CopyAction | ScreenshotToolPalette::PinAction;
    options.copyButtonWithNeutralIcon = false;
    options.separatorBeforeConfirm = false;
    options.showDrawingModeShortcutOnConfirm = false;
    return options;
}
} // namespace

struct ImageEditingWindow::Impl {
    ImageEditingWindow& q;
    ImageEditingServices services;
    ImageEditingDocument document;
    AdModal* modal;
    PreviewSurface* surface;
    ImageEditingCanvas* canvas;
    components::CanvasActionToolbar* zoomToolbar;
    QStackedLayout* stack;
    ScreenshotFloatingToolPaletteWindow* floating;
    QPointer<QWidget> editorWindow;
    ScreenshotToolPaletteHost* host;
    ScreenshotToolPalette* palette;
    AdButton *fit, *actual;
    QPointer<AdMessageHandle> statusMessage;
    WindowShortcutManager shortcuts;
    QVector<WindowShortcutManager::BindingHandle> commandBindings;
    ScreenshotAutoFilterController* autoFilter;
    ScreenshotRecognitionWindow* recognitionView = nullptr;
    QPointer<Session> session;
    struct RecognitionState {
        QPointer<Session> controller;
        QHash<QString, QByteArray> saved;
    };
    QHash<quint64, RecognitionState> sessions;
    QPointer<AdModal> confirmation;
    ScreenshotExportJobHandle loadJob, cropJob;
    std::shared_ptr<ScreenshotExportArtifact> loadArtifact;
    ImageEditingExportController exporter;
    WheelStepAccumulator wheel;
    QPoint toolbarOffset;
    bool manuallyPlaced = false;
    bool placingToolbar = false, draggingToolbar = false;
    bool windowInteractionActive = false;
    bool loading = false, cropBusy = false, closing = false;
    bool translateWhenReady = false;
    ScreenshotRecognitionResults initialRecognition;
    std::unique_ptr<ScreenshotCanvasColorSamplingSession> colorSampling;
    std::shared_ptr<ScreenshotExportArtifact> colorArtifact;
    QImage colorImage;
    quint64 colorGeneration = 0;
    quint64 loadGeneration = 0;
    enum class Notice { None, Loading, LoadError, Exporting, Exported };
    Notice notice = Notice::None;

    Impl(ImageEditingWindow& owner, ImageEditingServices service)
        : q(owner), services(std::move(service)), document(&q), modal(new AdModal(&q)),
          surface(new PreviewSurface), canvas(new ImageEditingCanvas(document, surface)),
          zoomToolbar(new components::CanvasActionToolbar(canvas)),
          stack(new QStackedLayout(surface)),
          floating(new ScreenshotFloatingToolPaletteWindow(editorOptions())),
          host(floating->paletteHost()), palette(host->palette()),
          fit(zoomToolbar->addAction(adqt::icons::antd::outlined::Fullscreen(), "imageEditingFit")),
          actual(zoomToolbar->addAction(icons::custom::outlined::ActualSize(),
                                        "imageEditingActualSize")),
          shortcuts(&q), autoFilter(new ScreenshotAutoFilterController(
                             [this] { return document.bounds(); },
                             [this](auto done) { done(document.image()); }, &q)),
          exporter(services.printing, &q, services.pin) {
        surface->setObjectName(QStringLiteral("imageEditingPreview"));
        surface->installEventFilter(&q);
        canvas->installEventFilter(&q);
        stack->setContentsMargins(0, 0, 0, 0);
        stack->addWidget(canvas);
        zoomToolbar->setObjectName(QStringLiteral("imageEditingCanvasToolbar"));
        auto* overlay = new QVBoxLayout(canvas);
        overlay->setContentsMargins(16, 16, 16, 16);
        overlay->addStretch(1);
        overlay->addWidget(zoomToolbar, 0, Qt::AlignHCenter);
        floating->setObjectName(QStringLiteral("imageEditingToolbar"));
        floating->installEventFilter(&q);
        modal->setObjectName(QStringLiteral("imageEditingModal"));
        modal->setMode(AdModal::Mode::Window);
        modal->setCentered(true);
        modal->setWindowModeDetached(true);
        modal->setWindowModality(Qt::NonModal);
        modal->setWindowResizable(true);
        modal->setWindowTaskbarVisible(true);
        modal->setWindowMinimizeButtonVisible(true);
        modal->setWindowMaximizeButtonVisible(true);
        modal->setWindowAlwaysOnTopButtonVisible(true);
        modal->setWindowAlwaysOnTop(true);
        modal->setWindowIcon(adqt::icons::makeIcon(icons::custom::app::ApplicationIcon()));
        modal->setFooterVisible(false);
        modal->setCloseOnEscape(false);
        modal->setClosePolicy(AdModal::ClosePolicy::Manual);
        modal->setCloseOnMaskClick(false);
        modal->setContentWidget(surface);
        QObject::connect(modal, &AdModal::closeRequested, &q, [this](auto) { q.requestClose(); });
        QObject::connect(modal, &AdModal::windowInteractionStarted, &q,
                         [this] { beginWindowInteraction(); });
        QObject::connect(modal, &AdModal::windowInteractionFinished, &q,
                         [this] { endWindowInteraction(); });
        QObject::connect(host, &ScreenshotToolPaletteHost::visibleContentChanged, &q, [this] {
            // The shared window preserves a dragged main row as its settings change.
            if (!manuallyPlaced)
                placeToolbar();
        });
        QObject::connect(floating, &ScreenshotFloatingToolPaletteWindow::dpiScaleCommitCompleted,
                         &q, [this] { placeToolbar(); });
        QObject::connect(host, &ScreenshotToolPaletteHost::dragStarted, &q, [this] {
            manuallyPlaced = true;
            draggingToolbar = true;
            rememberToolbarOffset();
        });
        QObject::connect(floating, &ScreenshotFloatingToolPaletteWindow::dragFinished, &q, [this] {
            manuallyPlaced = true;
            draggingToolbar = false;
            rememberToolbarOffset();
        });
        QObject::connect(palette, &ScreenshotToolPalette::cropRowsRequested, &q,
                         [this] { drawing(ImageEditingCanvas::Mode::CropRows); });
        QObject::connect(palette, &ScreenshotToolPalette::cropColumnsRequested, &q,
                         [this] { drawing(ImageEditingCanvas::Mode::CropColumns); });
        QObject::connect(fit, &AdButton::clicked, canvas, &ImageEditingCanvas::fit);
        QObject::connect(actual, &AdButton::clicked, canvas, &ImageEditingCanvas::actualSize);
        QObject::connect(palette, &ScreenshotToolPalette::moveRequested, &q,
                         [this] { drawing(ImageEditingCanvas::Mode::Pan); });
        new ScreenshotStyleBinding(*palette, *canvas, &q);
        bindScreenshotCanvasOperations(palette, *canvas, &q);
        bindScreenshotCanvasToolRequests(*palette, q, [this](SnowCanvasTool tool) {
            drawing(ImageEditingCanvas::Mode::Draw);
            canvas->setCanvasTool(tool);
            if (tool == SnowCanvasTool::AutoFilter)
                autoFilter->validate();
        });
        palette->setDrawTemplateCallbacks(
            [this] { return document.runtime().serializeSelectedDrawTemplate(); },
            [this](const QByteArray& data) { canvas->insertDrawTemplate(data, canvas->center()); });
        QObject::connect(palette, &ScreenshotToolPalette::watermarkPreviewChanged, canvas,
                         &SnowCanvasWidget::previewCanvasWatermarkConfig);
        QObject::connect(palette, &ScreenshotToolPalette::spotlightPreviewChanged, canvas,
                         &SnowCanvasWidget::previewCanvasSpotlightConfig);
        QObject::connect(palette, &ScreenshotToolPalette::textStylePopupInteractionBegan, canvas,
                         &SnowCanvasWidget::beginTextStylePopupInteraction);
        QObject::connect(palette, &ScreenshotToolPalette::textStylePopupInteractionEnded, &q,
                         [this] { canvas->endTextStylePopupInteraction(surface); });
        QObject::connect(canvas, &SnowCanvasWidget::activeToolChanged, &q, [this] {
            if (canvas->mode() == ImageEditingCanvas::Mode::Draw)
                if (auto tool = paletteToolForCanvasTool(canvas->canvasTool()))
                    host->setActiveTool(*tool);
        });
        QObject::connect(canvas, &SnowCanvasWidget::styleToolbarStateChanged, &q,
                         [this] { syncPalette(); });
        QObject::connect(canvas, &SnowCanvasWidget::historyStateChanged, &q,
                         [this] { syncPalette(); });
        QObject::connect(&document, &ImageEditingDocument::changed, &q, [this] {
            pruneRecognitionSessions();
            syncPalette();
        });
        QObject::connect(&document, &ImageEditingDocument::baseImageChanged, &q, [this] {
            const QPointer<ImageEditingWindow> lifetime(&q);
            const auto previous = session;
            if (previous) {
                previous->deactivate();
                if (!lifetime)
                    return;
                if (previous)
                    previous->cancelWorkflow();
                if (!lifetime)
                    return;
                if (previous)
                    previous->releaseTargetImage();
                if (session == previous)
                    session = nullptr;
            }
            autoFilter->resetSession();
            if (!lifetime)
                return;
            syncAvailability();
        });
        autoFilter->attachCanvas(canvas);
        QObject::connect(autoFilter, &ScreenshotAutoFilterController::availabilityChanged, palette,
                         &ScreenshotToolPalette::setAutoFilterAvailable);
        QObject::connect(palette, &ScreenshotToolPalette::autoFilterCategoryRequested, autoFilter,
                         &ScreenshotAutoFilterController::fillCategory);
        QObject::connect(autoFilter, &ScreenshotAutoFilterController::detectionFailed, &q,
                         [this](const QString& error) { message(error); });
        canvas->styleWheel = [this](QWheelEvent* event) {
            return handleScreenshotStyleWheel(*palette, *canvas, *event, wheel);
        };
        canvas->historyRequested = [this](bool redo) { undo(redo); };
        canvas->cropRequested = [this](auto axis, int first, int last) { crop(axis, first, last); };
        QObject::connect(palette, &ScreenshotToolPalette::undoRequested, &q,
                         [this] { undo(false); });
        QObject::connect(palette, &ScreenshotToolPalette::redoRequested, &q,
                         [this] { undo(true); });
        new CanvasHistoryShortcuts(
            shortcuts, &q,
            [this](const auto&) {
                return !loading && !cropBusy && !canvas->hasActiveTextEditing() &&
                       !(colorSampling && colorSampling->active());
            },
            [this](const QString& action) {
                undo(action == QStringLiteral("redo"));
                return true;
            });
        bindCanvasDrawingPreferences(document.runtime(), q);
        reloadShortcuts();
        auto& storage = storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            QObject::connect(
                &storage.configuration(), &storage::ConfigurationStore::valueChanged, &q,
                [this](const QString& key, const QJsonValue&) {
                    if (key == QStringLiteral("pin_to_screen/action_tools_layout")) {
                        palette->setActionToolsLayout(storage::ScreenshotToolbarSettings().layout(
                            storage::ScreenshotToolbarLayoutKind::PinnedActionTools));
                        placeToolbar();
                    } else if (key == QStringLiteral("screenshot_toolbar/layout")) {
                        palette->setToolbarLayout(storage::ScreenshotToolbarSettings().layout(
                            storage::ScreenshotToolbarLayoutKind::DrawingTools));
                        placeToolbar();
                    } else if (key == QStringLiteral("screenshot_ui/toolbar_size")) {
                        // The floating window commits its scale before this owner callback.
                        // Reanchor after that commit, rather than its intermediate host layout.
                        placeToolbar();
                    } else if (key.startsWith(QStringLiteral("drawing_shortcuts/")) ||
                               key.startsWith(QStringLiteral("screenshot_shortcuts/"))) {
                        reloadShortcuts();
                    }
                });
        }
        QObject::connect(
            palette, &ScreenshotToolPalette::canvasColorSamplingRequested, &q,
            [this](adqt::widgets::AdColorPicker* picker) { beginColorSampling(picker); });
        bindRecognition();
        QObject::connect(palette, &ScreenshotToolPalette::copyRequested, &q,
                         [this] { exportContent(ExportOperation::Copy); });
        QObject::connect(palette, &ScreenshotToolPalette::pinRequested, &q,
                         [this] { exportContent(ExportOperation::Pin); });
        QObject::connect(palette, &ScreenshotToolPalette::saveRequested, &q,
                         [this] { exportContent(ExportOperation::Save); });
        QObject::connect(palette, &ScreenshotToolPalette::quickSaveRequested, &q,
                         [this] { exportContent(ExportOperation::QuickSave); });
        QObject::connect(palette, &ScreenshotToolPalette::printRequested, &q,
                         [this] { exportContent(ExportOperation::Print); });
        QObject::connect(palette, &ScreenshotToolPalette::cloudUploadRequested, &q,
                         [this] { exportContent(ExportOperation::Upload); });
        retranslate();
    }
    ~Impl() {
        if (editorWindow)
            editorWindow->removeEventFilter(&q);
        floating->removeEventFilter(&q);
        surface->removeEventFilter(&q);
        canvas->removeEventFilter(&q);
        for (const auto& state : sessions)
            if (state.controller)
                delete state.controller;
        delete autoFilter;
        QObject::disconnect(canvas, nullptr, &q, nullptr);
        floating->setTransientOwnerWindow(nullptr);
        delete floating;
        QObject::disconnect(modal, nullptr, &q, nullptr);
        delete modal;
    }
    void reloadShortcuts() {
        for (const auto handle : commandBindings)
            static_cast<void>(shortcuts.removeBinding(handle));
        commandBindings.clear();
        const auto registerBindings = [this](const auto& settings, bool drawing) {
            for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
                if (!drawing &&
                    !QStringList{
                        QStringLiteral("table_recognition"), QStringLiteral("qr_code_recognition"),
                        QStringLiteral("text_recognition"), QStringLiteral("text_translation"),
                        QStringLiteral("copy_to_clipboard"), QStringLiteral("save_as_file"),
                        QStringLiteral("quick_save"), QStringLiteral("print"),
                        QStringLiteral("upload_to_cloud"), QStringLiteral("move_tool"),
                        QStringLiteral("pin_to_screen")}
                         .contains(it.key()))
                    continue;
                WindowShortcutManager::Binding binding;
                binding.id = (drawing ? QStringLiteral("image-editing.drawing.")
                                      : QStringLiteral("image-editing.action.")) +
                             it.key();
                binding.shortcutBindings = it.value();
                binding.priority =
                    drawing ? WindowShortcutManager::StandardPriority::DrawingShortcut
                            : WindowShortcutManager::StandardPriority::ScreenshotShortcut;
                binding.canActivate = [this](const auto& context) {
                    return !loading && !cropBusy && !closing &&
                           !(colorSampling && colorSampling->active()) &&
                           !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget) &&
                           !canvas->hasActiveTextEditing();
                };
                binding.activate = [this, id = it.key(), drawing](const auto&) {
                    return drawing ? palette->activateDrawingShortcut(id)
                                   : palette->activateScreenshotShortcut(id);
                };
                commandBindings.append(shortcuts.addBinding(&q, std::move(binding)));
            }
        };
        registerBindings(storage::DrawingShortcutSettings().allShortcuts(), true);
        registerBindings(storage::ScreenshotShortcutSettings().allShortcuts(), false);
    }
    void cancelColorSampling() {
        ++colorGeneration;
        if (colorArtifact)
            colorArtifact->cancel();
        colorArtifact.reset();
        colorImage = {};
        if (colorSampling)
            colorSampling->end();
        canvas->setCursor(canvas->mode() == ImageEditingCanvas::Mode::Pan ? Qt::OpenHandCursor
                                                                          : Qt::CrossCursor);
    }
    void beginColorSampling(adqt::widgets::AdColorPicker* picker) {
        if (!picker || loading || cropBusy)
            return;
        cancelColorSampling();
        if (!colorSampling)
            colorSampling = std::make_unique<ScreenshotCanvasColorSamplingSession>(q);
        colorSampling->begin(picker, [this] { cancelColorSampling(); });
        canvas->setCursor(ScreenshotCanvasColorSamplerWindow::samplingCursor());
        canvas->setFocus();
        const auto source = document.exportSource();
        if (!source.isValid()) {
            cancelColorSampling();
            return;
        }
        colorArtifact = std::make_shared<ScreenshotExportArtifact>(source);
        const auto generation = colorGeneration;
        if (!colorArtifact->requestImage(&q, [this, generation](auto result) {
                if (generation != colorGeneration || closing)
                    return;
                colorImage = std::move(result.image);
                if (colorImage.isNull())
                    cancelColorSampling();
            }))
            cancelColorSampling();
    }
    bool colorEvent(QEvent* event) {
        if (!colorSampling || !colorSampling->active())
            return false;
        if (event->type() == QEvent::KeyPress) {
            if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
                cancelColorSampling();
            return true;
        }
        if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonPress &&
            event->type() != QEvent::MouseButtonRelease)
            return event->type() == QEvent::Wheel || event->type() == QEvent::TabletPress ||
                   event->type() == QEvent::TabletMove || event->type() == QEvent::TabletRelease;
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::RightButton) {
            cancelColorSampling();
            return true;
        }
        const QPointF point = document.imagePixelAt(
            canvas->canvasToViewTransform().inverted().map(mouse->position()));
        const auto preview = ScreenshotCanvasColorSampler::previewFromPhysicalRaster(
            colorImage, colorImage.rect(), QPoint(qFloor(point.x()), qFloor(point.y())));
        if (!preview.isNull())
            colorSampling->updatePreview(preview, mouse->globalPosition().toPoint());
        else
            colorSampling->hidePreview();
        if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton &&
            !preview.isNull())
            static_cast<void>(colorSampling->commit(preview, [this] { cancelColorSampling(); }));
        return true;
    }
    void showNotice(Notice value) {
        switch (value) {
        case Notice::Loading:
            message(ImageEditingWindow::tr("Loading image…"), AdMessage::Type::Loading);
            break;
        case Notice::LoadError:
            message(ImageEditingWindow::tr("The image could not be loaded."));
            break;
        case Notice::Exporting:
            message(ImageEditingWindow::tr("Exporting…"), AdMessage::Type::Loading);
            break;
        case Notice::Exported:
            message(ImageEditingWindow::tr("Export completed"), AdMessage::Type::Success);
            break;
        case Notice::None:
            message({});
            break;
        }
        notice = value;
    }
    void retranslate() {
        modal->setWindowTitle(ImageEditingWindow::tr("Image Editing"));
        floating->setWindowTitle(modal->windowTitle());
        fit->setToolTip(ImageEditingWindow::tr("Fit image to window"));
        fit->setAccessibleName(fit->toolTip());
        actual->setToolTip(ImageEditingWindow::tr("View at actual size (100%)"));
        actual->setAccessibleName(actual->toolTip());
        if (notice != Notice::None)
            showNotice(notice);
        if (confirmation)
            translateConfirmation();
        placeToolbar();
    }
    void message(QString text, AdMessage::Type type = AdMessage::Type::Error) {
        notice = Notice::None;
        if (text.isEmpty()) {
            if (statusMessage)
                statusMessage->close();
            statusMessage.clear();
            return;
        }
        AdMessage::Request request;
        request.key = QStringLiteral("imageEditingStatus");
        request.type = type;
        request.content = std::move(text);
        if (type == AdMessage::Type::Loading)
            request.durationMs = 0;
        statusMessage = AdMessageService::instance(surface->window())->open(std::move(request));
    }
    void cancelLoading() {
        loadJob.cancel();
        if (loadArtifact)
            loadArtifact->cancel();
        loadArtifact.reset();
    }
    void setLoading(bool value) {
        loading = value;
        surface->setEnabled(!loading);
        floating->setEnabled(!loading && !cropBusy);
        // Keep the preview in the modal layout so loading never changes its geometry.
        if (loading) {
            show();
            showNotice(Notice::Loading);
        } else {
            showNotice(Notice::None);
        }
    }
    void beginWindowInteraction() {
        if (closing || windowInteractionActive)
            return;
        windowInteractionActive = true;
        cancelColorSampling();
        floating->cancelDrag();
        for (auto* picker : palette->findChildren<adqt::widgets::AdColorPicker*>())
            picker->setPopupVisible(false);
        for (auto* select : palette->findChildren<adqt::widgets::AdSelect*>())
            select->setPopupVisible(false);
        floating->hide();
        for (auto* child : floating->findChildren<QWidget*>())
            if (child->isWindow())
                child->hide();
    }
    void endWindowInteraction() {
        if (!windowInteractionActive)
            return;
        windowInteractionActive = false;
        // Match pin windows: settle at the default anchor after a move or resize.
        manuallyPlaced = false;
        placeToolbar();
    }
    void placeToolbar() {
        if (!editorWindow || closing || placingToolbar || windowInteractionActive)
            return;
        const QScopedValueRollback<bool> placing(placingToolbar, true);
        if (!editorWindow->isVisible() || editorWindow->isMinimized()) {
            floating->cancelDrag();
            floating->hide();
            return;
        }
        if (draggingToolbar)
            return;
        auto* screen = editorWindow->screen();
        const auto frame = editorWindow->frameGeometry();
        const auto bounds = screen ? screen->geometry() : frame;
        floating->setPlacementContext(screen, bounds,
                                      screen ? pinnedScreenGeometry(*screen) : frame);
        floating->prepareForDisplay();
        const auto geometry = floating->placementSnapshot();
        if (!geometry.bottom.isValid())
            return;
        if (manuallyPlaced) {
            // Keep the user's main row fixed when secondary rows grow or change direction.
            const auto position =
                frame.topLeft() + toolbarOffset - host->mainToolbarContentRect().topLeft();
            floating->moveContentTo(floating->constrainedContentPosition(position));
        } else {
            const auto placement = ScreenshotGeometryMapper::anchoredToolbarPlacement(
                QPoint(frame.right() + 1, frame.bottom() + 1),
                QPoint(frame.right() + 1, frame.top()), geometry.bottom, geometry.top, bounds, 4);
            floating->setStyleToolbarAboveMain(placement.usesTopRightPlacement);
            floating->resetPhysicalSizeInvariant();
            floating->moveContentTo(placement.contentPosition);
        }
        floating->show();
        if (recognitionView)
            static_cast<void>(
                recognitionView->updateSelectionGeometry(surface->rect(), document.bounds()));
    }
    void rememberToolbarOffset() {
        if (editorWindow)
            toolbarOffset = floating->contentPosition() + host->mainToolbarContentRect().topLeft() -
                            editorWindow->frameGeometry().topLeft();
    }
    void show() {
        if (!modal->isOpen()) {
            auto* screen = QGuiApplication::screenAt(QCursor::pos());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            if (screen) {
                modal->setWindowScreen(screen);
                const auto available = screen->availableGeometry().size() - QSize(40, 40);
                modal->setWindowPreferredSize(QSize(1200, 800).boundedTo(available));
                modal->setWindowMinimumSize(QSize(900, 600).boundedTo(available));
            }
        }
        modal->present();
        if (editorWindow != surface->window()) {
            if (editorWindow)
                editorWindow->removeEventFilter(&q);
            editorWindow = surface->window();
            // Keep the toolbar native surface independent of the modal's content tree.
            // Reparenting that tree can destroy child HWNDs without hiding their QWidgets.
            floating->setTransientOwnerWindow(editorWindow);
            editorWindow->installEventFilter(&q);
            shortcuts.addScopeWindow(editorWindow);
            shortcuts.addScopeWindow(floating);
        }
        floating->prepareForDisplay();
        placeToolbar();
    }
    void syncAvailability() {
        const auto providers =
            services.providers ? services.providers() : ScreenshotPinnedRecognitionProviders{};
        const auto cached = session                     ? session->cachedRecognitionResults()
                            : document.baseState() == 0 ? initialRecognition
                                                        : ScreenshotRecognitionResults{};
        const bool sized = screenshotOcrImageWithinPixelLimit(document.image().size());
        palette->setOcrEnabled((sized && providers.recognition) || cached.text.has_value());
        palette->setTableEnabled((sized && providers.tableRecognition) ||
                                 !cached.tableEntries.isEmpty() || cached.table.has_value());
        palette->setQrEnabled((sized && providers.qrRecognition) || cached.qr.has_value());
        palette->setLatexState((sized && providers.tableRecognition) ||
                                   !cached.latexEntries.isEmpty() || cached.latex.has_value(),
                               session && session->busy(Session::Mode::Latex));
        palette->setImageConversionEnabled((sized && providers.tableRecognition) ||
                                           !cached.conversions.isEmpty());
    }
    void syncPalette() {
        if (session && session->active())
            return;
        palette->setHistoryState({document.runtime().canUndo(), document.runtime().canRedo()});
        palette->setStyleToolbarState(canvas->canvasStyleToolbarState());
        palette->setWatermarkConfig(canvas->canvasWatermarkConfig());
        palette->setSpotlightConfig(canvas->canvasSpotlightConfig());
    }
    void drawing(ImageEditingCanvas::Mode mode) {
        if (loading || cropBusy)
            return;
        cancelColorSampling();
        if (session)
            session->deactivate();
        translateWhenReady = false;
        stack->setCurrentWidget(canvas);
        static_cast<void>(canvas->resetEditingStatePreservingTool());
        canvas->setMode(mode);
        palette->setCropToolsState(mode == ImageEditingCanvas::Mode::CropRows,
                                   mode == ImageEditingCanvas::Mode::CropColumns);
        fit->setEnabled(true);
        actual->setEnabled(true);
        if (mode == ImageEditingCanvas::Mode::Pan)
            host->setActiveTool(Tool::Move);
        else if (mode != ImageEditingCanvas::Mode::Draw)
            host->clearActiveTool();
        canvas->setFocus();
        syncAvailability();
        syncPalette();
        placeToolbar();
    }
    void setCropBusy(bool busy) {
        cropBusy = busy;
        canvas->setEnabled(!busy);
        floating->setEnabled(!busy && !loading);
    }
    void crop(ImageEditingDocument::CropAxis axis, int first, int last) {
        if (cropBusy || loading || closing)
            return;
        auto result = std::make_shared<ImageEditingDocument::Crop>();
        setCropBusy(true);
        cropJob = ScreenshotExportCoordinator::shared().submit(
            &q, ScreenshotExportCoordinator::Priority::Foreground,
            [image = document.image(), axis, first, last, result](const auto& cancel) {
                if (!cancel.isCancellationRequested())
                    *result =
                        ImageEditingDocument::prepareCrop(image, axis, first, last, [&cancel] {
                            return cancel.isCancellationRequested();
                        });
                return ScreenshotExportTaskResult{};
            },
            [this, result](auto) {
                setCropBusy(false);
                if (!closing)
                    static_cast<void>(document.applyCrop(std::move(*result)));
            });
        if (!cropJob.isValid()) {
            setCropBusy(false);
        }
    }
    void undo(bool redo) {
        if (cropBusy || loading)
            return;
        if (session && session->active()) {
            if (session->tableModeActive()) {
                if (redo)
                    session->redoTableEdit();
                else
                    session->undoTableEdit();
            } else {
                if (redo)
                    session->redoTextEdit();
                else
                    session->undoTextEdit();
            }
        } else {
            static_cast<void>(canvas->resetEditingStatePreservingTool());
            auto transition = std::make_shared<ImageEditingDocument::HistoryTransition>(
                document.historyTransition(redo));
            if (transition->state == transition->nextState) {
                document.applyHistoryTransition(std::move(*transition));
            } else {
                setCropBusy(true);
                cropJob = ScreenshotExportCoordinator::shared().submit(
                    &q, ScreenshotExportCoordinator::Priority::Foreground,
                    [transition](const auto& cancel) {
                        if (!cancel.isCancellationRequested())
                            ImageEditingDocument::prepareHistoryTransition(*transition, [&cancel] {
                                return cancel.isCancellationRequested();
                            });
                        return ScreenshotExportTaskResult{};
                    },
                    [this, transition](auto) {
                        setCropBusy(false);
                        if (!closing)
                            document.applyHistoryTransition(std::move(*transition));
                    });
                if (!cropJob.isValid())
                    setCropBusy(false);
            }
        }
        syncPalette();
    }
    void bindRecognition();
    void activate(Session::Mode mode, bool translate = false);
    Session* ensureSession();
    void pruneRecognitionSessions(bool replaceDocument = false) {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!replaceDocument && it->controller && document.retainsBaseHistoryState(it.key())) {
                ++it;
                continue;
            }
            const auto retired = it->controller;
            it = sessions.erase(it);
            if (retired) {
                if (session == retired)
                    session = nullptr;
                QObject::disconnect(retired, nullptr, &q, nullptr);
                // A history mutation can arrive through a session callback. Retire its
                // ownership now, then let destruction cancel work off that callback stack.
                retired->deleteLater();
            }
        }
    }
    ScreenshotRecognitionWindow* ensureRecognitionView();
    QByteArray recognitionCheckpoint(Session* target) const {
        if (!target)
            return {};
        return target->contentCheckpoints().value(target->activeContentKey());
    }
    void exportContent(ExportOperation operation);
    void translateConfirmation() {
        confirmation->setWindowTitle(ImageEditingWindow::tr("Discard Changes?"));
        confirmation->setText(ImageEditingWindow::tr("This editing window has unsaved changes."));
        confirmation->setAcceptText(ImageEditingWindow::tr("Discard Changes"));
        confirmation->setRejectText(ImageEditingWindow::tr("Continue Editing"));
    }
};

ScreenshotRecognitionWindow* ImageEditingWindow::Impl::ensureRecognitionView() {
    if (!recognitionView) {
        ScreenshotRecognitionWindowActions actions;
        actions.handleCancel = [this] { drawing(ImageEditingCanvas::Mode::Pan); };
        actions.handleCopy = [this] { exportContent(ExportOperation::Copy); };
        actions.handleTextEdited = [this](const QString& text) {
            if (session)
                session->setTextDraft(text);
        };
        actions.handleUndoTextEdit = [this] { undo(false); };
        actions.handleRedoTextEdit = [this] { undo(true); };
        actions.handleTableCommandStateChanged = [this](const auto& state) {
            if (session)
                session->handleTableCommandState(state);
        };
        recognitionView = new ScreenshotRecognitionWindow(
            actions, surface, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild,
            &shortcuts);
        recognitionView->setObjectName(QStringLiteral("imageEditingRecognition"));
        recognitionView->setOriginalImagePreviewSuppressed(true);
        stack->addWidget(recognitionView);
    }
    ScreenshotRecognitionWindow::Config config;
    config.presentationMode = ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild;
    config.screen = surface->screen();
    config.geometry = surface->rect();
    config.canvasSelection = document.bounds();
    config.transientOwner = surface->window();
    static_cast<void>(recognitionView->present(config));
    return recognitionView;
}

Session* ImageEditingWindow::Impl::ensureSession() {
    const auto providers =
        services.providers ? services.providers() : ScreenshotPinnedRecognitionProviders{};
    if (!session)
        session = sessions.value(document.baseState()).controller;
    if (session) {
        session->setTarget(
            {QStringLiteral("image-editing:%1:%2").arg(quintptr(&q)).arg(document.baseState()),
             document.image(), document.bounds()});
        session->setProviders(providers.recognition, providers.qrRecognition,
                              providers.tableRecognition);
        return session;
    }
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [this] { return ensureRecognitionView(); };
    actions.applyOcrPresentation = [](auto) {};
    actions.applyOcrBackground = [](auto) {};
    actions.applyFormattedText = [this](auto doc) {
        ensureRecognitionView()->showFormattedText(std::move(doc));
    };
    actions.clearOcrBackground = [] {};
    actions.setRecognitionVisualState = [this](bool active) {
        if (active)
            stack->setCurrentWidget(ensureRecognitionView());
        else
            stack->setCurrentWidget(canvas);
        placeToolbar();
    };
    actions.setActiveMode = [](int) {};
    actions.setTextEditingState = [this](bool available, bool editing, bool undo, bool redo) {
        palette->setTextEditingState(available, editing, undo, redo);
        palette->setOriginalImageToggleEnabled(false);
    };
    actions.setTextTranslationState = [this](bool a, bool b, bool c, bool d, bool e, bool f,
                                             bool g) {
        palette->setTextTranslationState(a, b, c, d, e, f, g);
    };
    actions.setTableEditingState = [this](bool a, bool b, bool c, bool d, bool e, bool f) {
        palette->setTableEditingState(a, b, c, d, e, f);
    };
    actions.setLatexEditingState = [this](bool a, bool b, bool c) {
        palette->setLatexEditingState(a, b, c);
    };
    actions.setBusyState = [this](bool a, bool b, bool c) {
        palette->setOcrBusy(a);
        palette->setTableBusy(b);
        palette->setQrBusy(c);
    };
    actions.hideLoading = [this] {
        ScreenshotMessageService::destroyFor(surface->window(),
                                             QStringLiteral("imageEditingRecognition"));
    };
    actions.showStatus = [this](const QString& text, bool error) {
        if (text.isEmpty())
            return;
        AdMessage::Request request;
        request.key = QStringLiteral("imageEditingRecognitionStatus");
        request.type = error ? AdMessage::Type::Error : AdMessage::Type::Warning;
        request.content = text;
        AdMessageService::instance(surface->window())->open(std::move(request));
    };
    actions.showModelDownload = [this](const QString& text) {
        ScreenshotMessageService::loadingFor(surface->window(),
                                             QStringLiteral("imageEditingModelDownload"), text);
    };
    actions.showRecognition = [this](const QString& text) {
        ScreenshotMessageService::loadingFor(surface->window(),
                                             QStringLiteral("imageEditingRecognition"), text);
    };
    actions.hideModelDownload = [this] {
        ScreenshotMessageService::destroyFor(surface->window(),
                                             QStringLiteral("imageEditingModelDownload"));
    };
    actions.translationSettingsOwner = [this] { return surface->window(); };
    actions.setTextTransformState = [this](const QString& a, const QString& b) {
        palette->setTextTransformSelections(a, b);
    };
    actions.setTextTargetLanguage = [this](const QString& text) {
        palette->setTextTargetLanguage(text);
    };
    actions.setShowOriginalImage = [this](bool) {
        palette->setShowOriginalImage(false);
        palette->setOriginalImageToggleEnabled(false);
    };
    actions.setConversionState = [this](bool active, bool busy, auto format) {
        palette->setImageConversionBusy(
            active && busy && format == SnowShotImageConversionFormat::Markdown,
            active && busy && format == SnowShotImageConversionFormat::Html);
    };
    actions.setRecognitionModelState = [this](int mode, const auto& state) {
        palette->setRecognitionModelState(
            mode == int(Session::Mode::Table) ? Tool::Table : Tool::Latex, state);
    };
    session = new Session(providers.recognition, providers.qrRecognition,
                          providers.tableRecognition, std::move(actions), &q);
    session->setIndependentEditing(true);
    session->setTarget(
        {QStringLiteral("image-editing:%1:%2").arg(quintptr(&q)).arg(document.baseState()),
         document.image(), document.bounds()});
    if (document.baseState() == 0) {
        auto seed = initialRecognition;
        seed.key =
            QStringLiteral("image-editing:%1:%2").arg(quintptr(&q)).arg(document.baseState());
        session->seedRecognitionResults(std::move(seed));
    }
    auto* created = session.data();
    sessions.insert(document.baseState(), {created, created->contentCheckpoints()});
    QObject::connect(created, &Session::recognitionResultsChanged, &q, [this, created] {
        if (session == created)
            syncAvailability();
    });
    QObject::connect(created, &Session::textResultChanged, &q, [this, created](bool ready) {
        if (ready && session == created) {
            if (translateWhenReady) {
                translateWhenReady = false;
                created->beginTextTranslation();
            }
        }
    });
    return session;
}
void ImageEditingWindow::Impl::activate(Session::Mode mode, bool translate) {
    if (loading || cropBusy || document.image().isNull())
        return;
    static_cast<void>(canvas->resetEditingStatePreservingTool());
    translateWhenReady = translate;
    ensureSession()->activate(mode);
    syncAvailability();
    fit->setEnabled(false);
    actual->setEnabled(false);
    palette->setCropToolsState(false, false);
    if (translate && translateWhenReady && session->hasTextResult()) {
        translateWhenReady = false;
        session->beginTextTranslation();
    }
    const Tool tool = mode == Session::Mode::Text ? (translate ? Tool::TextTranslation : Tool::Ocr)
                      : mode == Session::Mode::Table ? Tool::Table
                      : mode == Session::Mode::Qr    ? Tool::Qr
                      : mode == Session::Mode::Latex ? Tool::Latex
                      : mode == Session::Mode::Html  ? Tool::Html
                                                     : Tool::Markdown;
    host->setActiveTool(tool);
    placeToolbar();
}
void ImageEditingWindow::Impl::bindRecognition() {
    QObject::connect(palette, &ScreenshotToolPalette::ocrRequested, &q,
                     [this] { activate(Session::Mode::Text); });
    QObject::connect(palette, &ScreenshotToolPalette::textTranslationRequested, &q,
                     [this] { activate(Session::Mode::Text, true); });
    QObject::connect(palette, &ScreenshotToolPalette::tableRequested, &q,
                     [this] { activate(Session::Mode::Table); });
    QObject::connect(palette, &ScreenshotToolPalette::qrRequested, &q,
                     [this] { activate(Session::Mode::Qr); });
    QObject::connect(palette, &ScreenshotToolPalette::latexRequested, &q,
                     [this] { activate(Session::Mode::Latex); });
    QObject::connect(palette, &ScreenshotToolPalette::markdownRequested, &q,
                     [this] { activate(Session::Mode::Markdown); });
    QObject::connect(palette, &ScreenshotToolPalette::htmlRequested, &q,
                     [this] { activate(Session::Mode::Html); });
#define SESSION_ACTION(signal, method)                                                             \
    QObject::connect(palette, &ScreenshotToolPalette::signal, &q, [this] {                         \
        if (session)                                                                               \
            session->method();                                                                     \
    })
    SESSION_ACTION(tableMergeRequested, mergeTableSelection);
    SESSION_ACTION(tableSplitRequested, splitTableSelection);
    SESSION_ACTION(tableResetRequested, resetTable);
    QObject::connect(palette, &ScreenshotToolPalette::openSpreadsheetFileRequested, &q, [this] {
        const QPointer<ImageEditingWindow> lifetime(&q);
        if (session && session->openSpreadsheetFile() && lifetime)
            drawing(ImageEditingCanvas::Mode::Pan);
    });
    SESSION_ACTION(textEditRequested, beginTextEditing);
    SESSION_ACTION(textTranslateRequested, beginTextTranslation);
    SESSION_ACTION(textResetRequested, resetTextEditing);
    SESSION_ACTION(textSettingsRequested, openTranslationSettings);
    SESSION_ACTION(imageConversionSettingsRequested, openImageConversionSettings);
#undef SESSION_ACTION
#define SESSION_VALUE(signal, method)                                                              \
    QObject::connect(palette, &ScreenshotToolPalette::signal, &q, [this](const QString& value) {   \
        if (session)                                                                               \
            session->method(value);                                                                \
    })
    SESSION_VALUE(textFormattingRequested, applyTextFormatting);
    SESSION_VALUE(textPunctuationRequested, applyTextPunctuation);
    SESSION_VALUE(textTargetLanguageRequested, applyTextTargetLanguage);
    SESSION_VALUE(recognitionModelChanged, setRecognitionModel);
#undef SESSION_VALUE
    palette->setOriginalImageToggleEnabled(false);
}

void ImageEditingWindow::Impl::exportContent(ExportOperation operation) {
    if (loading || cropBusy || exporter.busy() || closing || document.image().isNull())
        return;
    if (!q.preparePendingEdits())
        return;
    const QPointer<Session> recognition =
        operation != ExportOperation::Pin && session && session->active() ? session : nullptr;
    const QByteArray checkpoint =
        recognition ? recognitionCheckpoint(recognition) : document.checkpoint();
    const QString contentKey = recognition ? recognition->activeContentKey() : QString{};
    const auto state = document.baseState();
    const bool recognitionExport = bool(recognition);
    const auto finish = [this, recognition, checkpoint, contentKey, state,
                         recognitionExport](bool success, QString error) {
        if (closing)
            return;
        palette->setCloudUploadBusy(false);
        if (success) {
            if (recognitionExport) {
                const auto entry = sessions.find(state);
                if (recognition && entry != sessions.end() && entry->controller == recognition)
                    entry->saved.insert(contentKey, checkpoint);
            } else
                document.markExported(checkpoint);
            showNotice(Notice::Exported);
        } else if (!error.isEmpty())
            message(std::move(error));
        else if (notice == Notice::Exporting)
            message({});
        emit q.exportFinished(success);
    };
    ImageEditingExportController::Content content;
    if (recognition && operation == ExportOperation::Copy) {
        auto mime = recognition->recognitionClipboardMimeData();
        if (!mime) {
            finish(false, ImageEditingWindow::tr("No recognition result is available to export."));
            return;
        }
        content = std::move(mime);
    } else if (recognition &&
               (operation == ExportOperation::Save || operation == ExportOperation::QuickSave)) {
        if (recognition->mode() == Session::Mode::Text && recognition->hasTextResult()) {
            content = recognition->textDraft();
        } else {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
            if (auto snapshot = recognition->fileExportSnapshot())
                content = std::move(*snapshot);
            else
#endif
            {
                finish(false,
                       ImageEditingWindow::tr("No recognition result is available to export."));
                return;
            }
        }
    } else if (recognition && recognitionView) {
        content = ScreenshotExportSource::fromImage(recognitionView->printViewportSnapshot());
    } else {
        QString error;
        auto source = document.exportSource(&error);
        if (!source.isValid()) {
            finish(false, error);
            return;
        }
        content = std::move(source);
    }
    const QPointer<ImageEditingWindow> lifetime(&q);
    showNotice(Notice::Exporting);
    if (!lifetime || closing)
        return;
    palette->setCloudUploadBusy(operation == ExportOperation::Upload);
    exporter.start(operation, std::move(content), surface->window(), floating, finish);
}

ImageEditingWindow::ImageEditingWindow(ImageEditingServices services, QObject* parent)
    : QObject(parent) {
    // Construction can deliver child events before Impl has finished installing the UI.
    m_impl = std::make_unique<Impl>(*this, std::move(services));
}
ImageEditingWindow::~ImageEditingWindow() {
    shutdown();
}
void ImageEditingWindow::present(ImageEditingSource source) {
    auto& d = *m_impl;
    if (d.closing)
        return;
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    if (source.image.isNull() && source.layeredImage.isValid()) {
        d.setLoading(true);
        auto result = std::make_shared<ImageEditingSource>(std::move(source));
        d.loadJob = ScreenshotExportCoordinator::shared().submit(
            this, ScreenshotExportCoordinator::Priority::Foreground,
            [result](const auto& cancel) {
                if (!cancel.isCancellationRequested())
                    result->image = renderImageEditingBaseImage(
                        result->layeredImage, result->canvasRect, result->pixelSize);
                result->layeredImage = {};
                return ScreenshotExportTaskResult{};
            },
            [this, result, generation](auto) {
                if (!m_impl->closing && m_impl->loadGeneration == generation)
                    present(std::move(*result));
            });
        if (d.loadJob.isValid())
            return;
    }
    if (source.resultStyle && source.error.isEmpty()) {
        d.setLoading(true);
        d.loadArtifact =
            std::make_shared<ScreenshotExportArtifact>(imageEditingCompositeSource(source));
        if (d.loadArtifact->requestImage(this, [this, source = std::move(source),
                                                generation](auto result) mutable {
                if (m_impl->closing || m_impl->loadGeneration != generation)
                    return;
                [[maybe_unused]] const auto artifact = std::move(m_impl->loadArtifact);
                present(completeImageEditingComposite(std::move(source), std::move(result.image)));
            }))
            return;
        source.error = tr("The image could not be loaded.");
    }
    d.setLoading(false);
    d.show();
    if (!source.error.isEmpty() || !d.document.load(source)) {
        if (source.error.isEmpty() || source.error == tr("The image could not be loaded.") ||
            source.error ==
                QCoreApplication::translate("ImageEditingWindow", "The image could not be loaded."))
            d.showNotice(Impl::Notice::LoadError);
        else
            d.message(source.error);
        emit loaded(false);
        return;
    }
    d.pruneRecognitionSessions(true);
    d.initialRecognition = source.recognition;
    d.syncAvailability();
    d.canvas->fit();
    d.drawing(ImageEditingCanvas::Mode::Pan);
    emit loaded(true);
}
void ImageEditingWindow::presentSource(ScreenshotExportSource source) {
    auto& d = *m_impl;
    if (d.closing)
        return;
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    d.setLoading(true);
    d.loadArtifact = std::make_shared<ScreenshotExportArtifact>(std::move(source));
    if (!d.loadArtifact->requestImage(this, [this, generation](ScreenshotExportImageResult result) {
            if (m_impl->closing || m_impl->loadGeneration != generation)
                return;
            // Keep the artifact alive until its delivery callback returns.
            [[maybe_unused]] const auto artifact = std::move(m_impl->loadArtifact);
            ImageEditingSource editingSource;
            editingSource.image = std::move(result.image);
            editingSource.pixelSize = editingSource.image.size();
            editingSource.canvasRect = QRectF(QPointF(), QSizeF(editingSource.pixelSize));
            if (editingSource.image.isNull())
                editingSource.error = tr("The image could not be loaded.");
            present(std::move(editingSource));
        })) {
        ImageEditingSource failure;
        failure.error = tr("The image could not be loaded.");
        present(std::move(failure));
    }
}
void ImageEditingWindow::presentHistory(storage::CaptureHistoryRecord record) {
    auto& d = *m_impl;
    if (d.closing)
        return;
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    d.setLoading(true);
    auto result = std::make_shared<ImageEditingSource>();
    d.loadJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [record = std::move(record), result](const auto& cancel) {
            if (!cancel.isCancellationRequested())
                *result = readImageEditingHistory(
                    storage::ApplicationStorage::instance().captureHistory(), record);
            return ScreenshotExportTaskResult{};
        },
        [this, result, generation](auto) {
            if (!m_impl->closing && m_impl->loadGeneration == generation)
                present(std::move(*result));
        });
    if (!d.loadJob.isValid()) {
        ImageEditingSource failure;
        failure.error = tr("The image could not be loaded.");
        present(std::move(failure));
    }
}
void ImageEditingWindow::presentPinnedRecord(QString id) {
    auto& d = *m_impl;
    if (d.closing)
        return;
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    if (d.services.pins)
        if (auto* pin = d.services.pins->liveWindow(id)) {
            auto source = pin->imageEditingSnapshot();
            if (!source.image.isNull() || source.layeredImage.isValid()) {
                present(std::move(source));
                return;
            }
        }
    d.setLoading(true);
    const auto textPreparation = captureScreenshotPinnedTextPreparation();
    auto result = std::make_shared<ImageEditingSource>();
    d.loadJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [id = std::move(id), result, textPreparation](const auto& cancel) {
            if (!cancel.isCancellationRequested()) {
                auto record =
                    storage::ApplicationStorage::instance().pinnedWindows().loadRecord(id);
                if (record)
                    *result = readImageEditingPinned(std::move(*record), textPreparation);
            }
            return ScreenshotExportTaskResult{};
        },
        [this, result, generation](auto) {
            if (!m_impl->closing && m_impl->loadGeneration == generation)
                present(std::move(*result));
        });
    if (!d.loadJob.isValid()) {
        ImageEditingSource failure;
        failure.error = tr("The image could not be loaded.");
        present(std::move(failure));
    }
}
bool ImageEditingWindow::dirty() const {
    if (m_impl->cropBusy)
        return true;
    if (m_impl->document.dirty())
        return true;
    for (const auto& state : m_impl->sessions)
        if (state.controller && state.controller->contentCheckpoints() != state.saved)
            return true;
    return false;
}
QByteArray ImageEditingWindow::contentCheckpoint() const {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << m_impl->document.checkpoint() << m_impl->cropBusy;
    auto states = m_impl->sessions.keys();
    std::sort(states.begin(), states.end());
    for (auto state : states) {
        const auto session = m_impl->sessions.value(state).controller;
        if (!session)
            continue;
        const auto contents = session->contentCheckpoints();
        auto keys = contents.keys();
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys)
            stream << state << key << contents.value(key);
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
bool ImageEditingWindow::preparePendingEdits() {
    if (m_impl->closing)
        return false;
    const QPointer<ImageEditingWindow> lifetime(this);
    static_cast<void>(m_impl->canvas->resetEditingStatePreservingTool());
    if (!lifetime || m_impl->closing)
        return false;
    if (m_impl->session && m_impl->session->active() && m_impl->session->tableModeActive() &&
        m_impl->recognitionView)
        m_impl->recognitionView->commitActiveTableEdit();
    return lifetime && !m_impl->closing;
}
void ImageEditingWindow::confirmDiscard(std::function<void(bool)> done) {
    if (!preparePendingEdits()) {
        done(false);
        return;
    }
    auto& d = *m_impl;
    if (!dirty()) {
        done(true);
        return;
    }
    if (d.confirmation) {
        done(false);
        return;
    }
    auto* dialog = new AdModal(this);
    d.confirmation = dialog;
    dialog->setMode(AdModal::Mode::Window);
    dialog->setOwnerWindow(d.surface->window());
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setPreset(AdModal::Preset::Confirm);
    d.translateConfirmation();
    connect(dialog, &AdModal::finished, this, [this, done = std::move(done), dialog](auto code) {
        m_impl->confirmation = nullptr;
        dialog->deleteLater();
        done(code == AdModal::DialogCode::Accepted);
    });
    dialog->setOpen(true);
    if (dialog->acceptButton()) {
        dialog->acceptButton()->setAutoDefault(false);
        dialog->acceptButton()->setDefault(false);
    }
    if (dialog->rejectButton()) {
        dialog->rejectButton()->setAutoDefault(true);
        dialog->setInitialFocusWidget(dialog->rejectButton());
        dialog->rejectButton()->setDefault(true);
        dialog->rejectButton()->setFocus();
    }
}
void ImageEditingWindow::requestClose() {
    if (m_impl->closing)
        return;
    confirmDiscard([this](bool discard) {
        if (discard) {
            const QPointer<ImageEditingWindow> lifetime(this);
            shutdown();
            if (lifetime)
                emit closed();
        }
    });
}
void ImageEditingWindow::shutdown() {
    if (!m_impl || m_impl->closing)
        return;
    const QPointer<ImageEditingWindow> lifetime(this);
    auto& d = *m_impl;
    const QPointer<AdModal> confirmation = d.confirmation;
    d.closing = true;
    d.showNotice(Impl::Notice::None);
    if (!lifetime)
        return;
    d.floating->cancelDrag();
    d.floating->hide();
    if (!lifetime)
        return;
    d.cancelColorSampling();
    if (!lifetime)
        return;
    d.autoFilter->resetSession();
    if (!lifetime)
        return;
    d.cancelLoading();
    d.cropJob.cancel();
    d.exporter.cancel();
    if (!lifetime)
        return;
    const auto sessions = d.sessions;
    for (const auto& state : sessions) {
        const auto session = state.controller;
        if (session)
            session->invalidate();
        if (!lifetime)
            return;
    }
    // Rejecting invokes client completion, which can destroy this editor and its owner.
    // Resolve it before hiding its owner can trigger the modal's owner-hide callback.
    if (confirmation)
        confirmation->reject();
    if (lifetime)
        d.modal->setOpen(false);
}
bool ImageEditingWindow::eventFilter(QObject* object, QEvent* event) {
    if (m_impl && object == m_impl->floating && event->type() == QEvent::Close) {
        event->ignore();
        requestClose();
        return true;
    }
    if (m_impl && object == m_impl->canvas && m_impl->colorEvent(event)) {
        event->accept();
        return true;
    }
    if (m_impl && object == m_impl->surface) {
        if (event->type() == QEvent::Resize)
            m_impl->placeToolbar();
        if (event->type() == QEvent::LanguageChange)
            m_impl->retranslate();
        if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
            event->type() == QEvent::DevicePixelRatioChange ||
            event->type() == QEvent::ScreenChangeInternal)
            QTimer::singleShot(0, this, [this] {
                if (!m_impl->closing)
                    m_impl->placeToolbar();
            });
    }
    if (m_impl && object == m_impl->editorWindow) {
        if (event->type() == QEvent::Hide ||
            (event->type() == QEvent::WindowStateChange && m_impl->editorWindow->isMinimized())) {
            m_impl->floating->cancelDrag();
            m_impl->floating->hide();
        } else if (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
                   event->type() == QEvent::Show || event->type() == QEvent::WindowStateChange ||
                   event->type() == QEvent::ScreenChangeInternal ||
                   event->type() == QEvent::DevicePixelRatioChange) {
            m_impl->placeToolbar();
        }
    }
    return QObject::eventFilter(object, event);
}
ImageEditingDocument* ImageEditingWindow::document() const {
    return &m_impl->document;
}
ImageEditingCanvas* ImageEditingWindow::canvas() const {
    return m_impl->canvas;
}
ScreenshotToolPalette* ImageEditingWindow::palette() const {
    return m_impl->palette;
}
AdModal* ImageEditingWindow::modal() const {
    return m_impl->modal;
}
Session* ImageEditingWindow::recognition() const {
    return m_impl->session;
}
ImageEditingWindowCoordinator::ImageEditingWindowCoordinator(ImageEditingServices services,
                                                             QObject* parent)
    : QObject(parent), m_services(std::move(services)) {
    qApp->installEventFilter(this);
}
ImageEditingWindowCoordinator::~ImageEditingWindowCoordinator() {
    shutdown();
}
ImageEditingWindow* ImageEditingWindowCoordinator::createWindow() {
    auto* window = new ImageEditingWindow(m_services, this);
    m_windows.append(window);
    connect(window, &ImageEditingWindow::closed, this, [this, window] {
        m_windows.removeAll(window);
        window->deleteLater();
    });
    return window;
}
void ImageEditingWindowCoordinator::presentHistory(const storage::CaptureHistoryRecord& record) {
    createWindow()->presentHistory(record);
}
void ImageEditingWindowCoordinator::presentSource(ScreenshotExportSource source) {
    createWindow()->presentSource(std::move(source));
}
void ImageEditingWindowCoordinator::presentPinnedRecord(const QString& id) {
    createWindow()->presentPinnedRecord(id);
}
void ImageEditingWindowCoordinator::confirmShutdown(std::function<void(bool)> done) {
    if (m_reviewPending) {
        done(false);
        return;
    }
    m_reviewPending = true;
    auto approved = std::make_shared<QHash<ImageEditingWindow*, QByteArray>>();
    const QPointer<ImageEditingWindowCoordinator> lifetime(this);
    auto advance = std::make_shared<std::function<void(bool)>>();
    const std::weak_ptr<std::function<void(bool)>> weakAdvance = advance;
    *advance = [lifetime, approved, weakAdvance, done = std::move(done)](bool accepted) {
        const auto continuation = weakAdvance.lock();
        if (!continuation)
            return;
        QPointer<ImageEditingWindow> pending;
        if (accepted && lifetime) {
            const auto windows = lifetime->m_windows;
            for (auto window : windows) {
                if (!window)
                    continue;
                const bool prepared = window->preparePendingEdits();
                if (!lifetime)
                    break;
                if (prepared && window && window->dirty() &&
                    approved->value(window) != window->contentCheckpoint()) {
                    pending = window;
                    break;
                }
            }
        }
        if (!accepted || !lifetime || !pending) {
            if (lifetime)
                lifetime->m_reviewPending = false;
            auto complete = done;
            *continuation = {};
            complete(accepted && lifetime);
            return;
        }
        pending->confirmDiscard([pending, approved, continuation](bool accepted) {
            if (accepted && pending)
                approved->insert(pending, pending->contentCheckpoint());
            auto next = *continuation;
            next(accepted);
        });
    };
    auto first = *advance;
    first(true);
}
bool ImageEditingWindowCoordinator::hasUnsavedChanges() const {
    return std::any_of(m_windows.cbegin(), m_windows.cend(),
                       [](const auto& window) { return window && window->dirty(); });
}
void ImageEditingWindowCoordinator::shutdown() {
    const auto windows = std::exchange(m_windows, {});
    for (auto window : windows) {
        if (!window)
            continue;
        window->shutdown();
        if (window)
            window->deleteLater();
    }
}
bool ImageEditingWindowCoordinator::eventFilter(QObject* object, QEvent* event) {
    if (object == qApp && event->type() == QEvent::Quit && !m_quitApproved) {
        if (!m_quitPending) {
            m_quitPending = true;
            QTimer::singleShot(0, this, [this] {
                const QPointer<ImageEditingWindowCoordinator> lifetime(this);
                confirmShutdown([lifetime](bool accepted) {
                    if (!lifetime)
                        return;
                    lifetime->m_quitPending = false;
                    if (accepted) {
                        lifetime->m_quitApproved = true;
                        QCoreApplication::quit();
                    }
                });
            });
        }
        return true;
    }
    return QObject::eventFilter(object, event);
}
} // namespace snow_shot::presentation
