// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotpinnedwindow.h"

#include "snow_shot/presentation/livewindowsource.h"
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_shot/presentation/pinnedwindowselectioncontroller.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/pinnedgeometry.h"
#include "snow_shot/presentation/screenshotselectionpin.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "widgets/context_menu.h"
#include "widgets/button.h"
#include "widgets/message.h"
#include "theme/theme_manager.h"
#include "pinnedwindowplatform.h"
#include "screenshotclipboardplacementgeometry.h"

#include <QAction>
#include <QActionGroup>
#include <QColorSpace>
#include <QApplication>
#include <QFutureWatcher>
#include <QLabel>
#include <QPainter>
#include <QTimer>
#include <QtConcurrentRun>
#include <chrono>
#include <future>
#include <algorithm>
#include <utility>

using snow_shot::presentation::LiveWindowSource;
using snow_shot::presentation::LiveWindowState;

bool ScreenshotPinnedWindow::liveFrameAvailable() const {
    return m_liveSource && m_liveSource->state() == LiveWindowState::Live &&
           m_firstContentFramePublished && !m_closing;
}

bool ScreenshotPinnedWindow::liveCommandAllowed(const QString& command) const {
    if (!isLiveWindow())
        return true;
    if (command == u"copy_to_clipboard" || command == u"save_as_file" ||
        command == u"convert_to_image" || command == u"split_image" || command == u"image_editing")
        return liveFrameAvailable();
    static const QStringList commands{
        QStringLiteral("increase_opacity"), QStringLiteral("decrease_opacity"),
        QStringLiteral("increase_scale"),   QStringLiteral("decrease_scale"),
        QStringLiteral("rotate_clockwise"), QStringLiteral("rotate_counterclockwise"),
        QStringLiteral("flip_horizontal"),  QStringLiteral("flip_vertical"),
        QStringLiteral("reset_transform"),  QStringLiteral("toggle_click_through"),
        QStringLiteral("toggle_lock"),      QStringLiteral("always_on_top"),
        QStringLiteral("show_border"),      QStringLiteral("show_shadow"),
        QStringLiteral("close_window"),     QStringLiteral("close")};
    return commands.contains(command) || command.startsWith(u"move_cursor_");
}

bool ScreenshotPinnedWindow::initializeLivePresentation(QString* error) {
    if (!m_liveSource)
        return true;
    connect(m_liveSource.get(), &LiveWindowSource::frameChanged, this,
            &ScreenshotPinnedWindow::queueLiveFrame);
    connect(m_liveSource.get(), &LiveWindowSource::stateChanged, this,
            [this] { updateLiveState(); });
    connect(m_liveSource.get(), &LiveWindowSource::stopped, this,
            &ScreenshotPinnedWindow::closeLiveWindow);
    connect(m_liveSource.get(), &LiveWindowSource::nativePresentationChanged, this,
            [this](bool native) {
                m_liveNativePresentation = native;
                updateLiveState();
            });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &ScreenshotPinnedWindow::closeLiveWindow);
    reloadPinnedWindowShortcuts();
    const bool result = synchronizeLivePresentation(error);
    updateLiveState();
    return result;
}

void ScreenshotPinnedWindow::scheduleLivePresentation() {
    if (!m_liveSource || m_livePresentationUpdatePending || m_closing)
        return;
    m_livePresentationUpdatePending = true;
    QTimer::singleShot(0, this, [this] {
        m_livePresentationUpdatePending = false;
        if (!m_closing)
            static_cast<void>(synchronizeLivePresentation());
    });
}

bool ScreenshotPinnedWindow::synchronizeLivePresentation(QString* error) {
    // The initial raster must publish before switching the shell to a native content hole.
    // Geometry events delivered by show() can otherwise hide the canvas before that paint.
    if (!m_liveSource || !m_presented || !m_firstContentFramePublished || m_closing)
        return true;
    snow_shot::presentation::LiveWindowPresentation presentation;
    presentation.owner = this;
    presentation.nativeContentGeometry = authoritativeContentNativeGeometry();
    presentation.effectiveOpacity =
        (m_clickThroughActive ? m_clickThroughOpacityPercent : m_opacityPercent) / 100.0;
    presentation.alwaysOnTop = m_alwaysOnTop;
    presentation.clickThrough = m_clickThroughActive;
    presentation.visible = isVisible() && !isMinimized();
    presentation.imageTransform = m_imageTransform;
    presentation.backgroundColor =
        adqt::theme::ThemeManager::instance().resolveTheme(this).colorBgContainer;
    if (!presentation.backgroundColor.isValid())
        presentation.backgroundColor = palette().color(QPalette::Window);
    presentation.backgroundColor.setAlpha(255);
    const bool result = m_liveSource->setPresentation(presentation, error);
    m_liveNativePresentation = m_liveSource->usesNativePresentation();
    if (m_liveStatusLabel)
        m_liveStatusLabel->setGeometry(contentViewRect().toAlignedRect());
    if (m_canvas)
        m_canvas->setVisible(liveFrameAvailable() && !m_liveNativePresentation);
    update();
    return result;
}

void ScreenshotPinnedWindow::updateLiveState() {
    if (!m_liveSource || m_closing)
        return;
    const bool available = liveFrameAvailable();
    if (!available && !m_liveStatusLabel) {
        m_liveStatusLabel = new QLabel(this);
        m_liveStatusLabel->setObjectName(QStringLiteral("screenshotPinnedLiveStatus"));
        m_liveStatusLabel->setWordWrap(true);
        m_liveStatusLabel->setAlignment(Qt::AlignCenter);
        m_liveStatusLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_liveStatusLabel->setContentsMargins(12, 12, 12, 12);
    }
    if (m_liveStatusLabel) {
        QString text;
        switch (m_liveSource->state()) {
        case LiveWindowState::Minimized:
            text = tr("The window is minimized. The latest content cannot be displayed.");
            break;
        case LiveWindowState::Closed:
            text = tr("The window is closed. The latest content cannot be displayed.");
            break;
        case LiveWindowState::Initializing:
        case LiveWindowState::Unavailable:
            text = tr("Live content is unavailable. Retrying...");
            break;
        case LiveWindowState::Live:
            break;
        }
        m_liveStatusLabel->setText(text);
        m_liveStatusLabel->setToolTip(m_liveSource->stateMessage());
        m_liveStatusLabel->setGeometry(contentViewRect().toAlignedRect());
        m_liveStatusLabel->setVisible(!available);
        m_liveStatusLabel->raise();
    }
    if (m_canvas)
        m_canvas->setVisible(available && !m_liveNativePresentation);
    reloadPinnedWindowShortcuts();
    refreshContextMenu();
    scheduleLivePresentation();
    update();
}

void ScreenshotPinnedWindow::queueLiveFrame(QImage frame) {
    if (!m_liveSource || m_closing || frame.isNull())
        return;
    m_livePendingFrame = std::move(frame);
    if (m_liveFrameProcessing)
        return;
    m_liveFrameProcessing = true;
    QImage original = std::exchange(m_livePendingFrame, {});
    const QTransform transform = m_imageTransform;
    const quint64 generation = m_liveFrameGeneration;
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, original, generation] {
                QImage transformed = watcher->result();
                watcher->deleteLater();
                m_liveFrameProcessing = false;
                if (m_liveSource && !m_closing && generation == m_liveFrameGeneration &&
                    !transformed.isNull())
                    installLiveFrame(original, std::move(transformed));
                if (!m_livePendingFrame.isNull())
                    queueLiveFrame(std::exchange(m_livePendingFrame, {}));
            });
    watcher->setFuture(QtConcurrent::run([original, transform] {
        return snowCanvasTransformImage(original, transform, Qt::SmoothTransformation);
    }));
}

void ScreenshotPinnedWindow::installLiveFrame(QImage original, QImage transformed) {
    m_originalImage = std::move(original);
    m_transformedImage = std::move(transformed);
    m_originalPixelSize = m_originalImage.size();
    const QSizeF sourceSize = m_canvasSourceRect.size();
    const qreal scaleX = sourceSize.width() / m_originalImage.width();
    const qreal scaleY = sourceSize.height() / m_originalImage.height();
    m_backgroundCanvasRect =
        QRectF(m_canvasSourceRect.topLeft(),
               QSizeF(m_transformedImage.width() * scaleX, m_transformedImage.height() * scaleY));
    m_resultSurfaceCanvasRect = m_backgroundCanvasRect;
    m_imageSource = ScreenshotImageSource::fromImage(m_transformedImage, m_backgroundCanvasRect);
    m_screenshotRenderer->setImageSource(m_imageSource);
    m_screenshotRenderer->setPinnedResultSurface(m_backgroundCanvasRect, m_resultSurfaceCanvasRect,
                                                 m_resultStyle);
    updateCanvasViewport();
    if (m_canvas)
        m_canvas->update();
}

ScreenshotExportSource ScreenshotPinnedWindow::liveExportSource(bool applyWindowOpacity) {
    if (!liveFrameAvailable())
        return {};
    const QRect content = authoritativeContentNativeGeometry();
    const bool logical = snow_shot::presentation::kPinnedGeometryUnits ==
                         snow_shot::storage::PinnedGeometryUnits::LogicalPixels;
    const qreal backingScale = logical ? devicePixelRatioF() : 1.0;
    const QSize output(std::max(1, qRound(content.width() * backingScale)),
                       std::max(1, qRound(content.height() * backingScale)));
    const QTransform transform = m_imageTransform;
    const qreal opacity = applyWindowOpacity ? m_opacityPercent / 100.0 : 1.0;
    const auto placement = screenshotClipboardSelectionPlacement(content, content.size(), screen());
    ScreenshotClipboardAppearance appearance;
    appearance.rasterSize = output;
    appearance.checkerboardEnabled = m_checkerboardEnabled.value_or(false);
    appearance.showBorder = m_showBorder;
    appearance.showShadow = m_showShadow;
    auto promise = std::make_shared<std::promise<QImage>>();
    const auto future = promise->get_future().share();
    // Acquire now, before an export worker or save dialog can observe a later frame.
    m_liveSource->requestFrame(
        [promise](const QImage& frame, const QString&) { promise->set_value(frame); });
    return ScreenshotExportSource::fromProducer(
        [future, output, transform, opacity](const ScreenshotExportCancellation& cancellation) {
            while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
                if (cancellation.isCancellationRequested())
                    return QImage{};
            if (cancellation.isCancellationRequested())
                return QImage{};
            const QImage frame = future.get();
            if (frame.isNull())
                return QImage{};
            const QImage transformed =
                snowCanvasTransformImage(frame, transform, Qt::SmoothTransformation);
            QImage result(output, QImage::Format_ARGB32_Premultiplied);
            if (result.isNull())
                return QImage{};
            result.fill(Qt::transparent);
            result.setColorSpace(transformed.colorSpace());
            QPainter painter(&result);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.setOpacity(opacity);
            painter.drawImage(QRect(QPoint(), output), transformed);
            return result;
        },
        {}, placement, appearance);
}

std::shared_ptr<ScreenshotExportArtifact>
ScreenshotPinnedWindow::liveViewportArtifact(bool applyWindowOpacity) {
    auto source = liveExportSource(applyWindowOpacity);
    return source.isValid() ? std::make_shared<ScreenshotExportArtifact>(std::move(source))
                            : nullptr;
}

void ScreenshotPinnedWindow::requestLiveSnapshot(std::function<void(QImage, QString)> completion,
                                                 bool applyWindowOpacity) {
    auto artifact = liveViewportArtifact(applyWindowOpacity);
    if (!artifact) {
        completion({}, tr("The current window content cannot be captured."));
        return;
    }
    auto callback = std::make_shared<std::function<void(QImage, QString)>>(std::move(completion));
    if (!artifact->requestImage(this,
                                [artifact, callback](ScreenshotExportImageResult result) {
                                    if (*callback)
                                        std::exchange(*callback, {})(std::move(result.image),
                                                                     std::move(result.error));
                                }) &&
        *callback)
        std::exchange(*callback, {})({}, tr("The current window content cannot be captured."));
}

void ScreenshotPinnedWindow::convertLiveToImage() {
    if (!liveFrameAvailable() || m_liveSnapshotPending)
        return;
    m_liveSnapshotPending = true;
    const QTransform transform = m_imageTransform;
    const QSize size = authoritativeContentNativeGeometry().size();
    requestLiveSnapshot(
        [this, transform, size](QImage image, const QString& error) {
            m_liveSnapshotPending = false;
            if (m_closing || !m_liveSource)
                return;
            if (!image.isNull() && error.isEmpty() && liveFrameAvailable() &&
                transform == m_imageTransform &&
                size == authoritativeContentNativeGeometry().size()) {
                finishLiveConversion(std::move(image));
            } else if (liveFrameAvailable()) {
                adqt::widgets::AdMessage::Request request;
                request.key = QStringLiteral("live-window-conversion");
                request.content =
                    error.isEmpty()
                        ? tr("The window changed while preparing the image. Please try again.")
                        : error;
                adqt::widgets::AdMessageService::error(std::move(request), this);
            }
        },
        false);
}

void ScreenshotPinnedWindow::finishLiveConversion(QImage image) {
    if (image.isNull() || !isLiveWindow() || m_closing)
        return;
    const QRect geometry = authoritativeContentNativeGeometry();
    ++m_liveFrameGeneration;
    m_livePendingFrame = {};
    m_imageTransform.reset();
    m_quarterTurns = 0;
    m_canvasSourceRect = QRectF(QPointF(), QSizeF(geometry.size()));
    m_initialWindowSize = geometry.size();
    m_scalePercent = 100;
    m_resultStyle = {};
    installLiveFrame(image, image);
    stopLiveSource();
    m_liveSource.reset();
    m_liveNativePresentation = false;
    if (m_liveStatusLabel)
        m_liveStatusLabel->hide();
    m_canvas->show();
    m_persistenceEnabled = true;
    m_editingEnabled = m_staticEditingEnabled;
    m_automaticTextRecognition = false;
    m_ocrSupported = screenshotOcrImageWithinPixelLimit(m_originalPixelSize);
    m_firstContentFramePublished = true;
    m_originalClipboardContent = {};
    m_sourceIdentity = {};
    m_recognitionResults = {};
    m_recognitionTargetReady = false;
    m_ocrReady = false;
    m_originalOcrPresentation.reset();
    m_displayOcrPresentation.reset();
    configureRecognitionSession();
    ensureRecognitionProviders();
    scheduleDeferredPresentationSetup();
    if (m_contextMenu) {
        m_contextMenu->hide();
        m_contextMenu->deleteLater();
        m_contextMenu = nullptr;
    }
    m_splitImageAction = nullptr;
    m_imageEditingAction = nullptr;
    m_lockAction = nullptr;
    m_closeAction = nullptr;
    m_alwaysOnTopAction = nullptr;
    m_showBorderAction = nullptr;
    m_showShadowAction = nullptr;
    m_clickThroughAction = nullptr;
    m_opacityActions = nullptr;
    m_scaleActions = nullptr;
    if (m_selectionController)
        m_selectionController->registerWindow(this);
    reloadPinnedWindowShortcuts();
    updateControlsVisibility();
    persistNow();
}

void ScreenshotPinnedWindow::splitLiveImage() {
    if (liveFrameAvailable())
        emit splitRequested(splittingSnapshot());
}

void ScreenshotPinnedWindow::editLiveImage() {
    if (!liveFrameAvailable())
        return;
    requestLiveSnapshot([this](QImage image, const QString& error) {
        if (image.isNull() || !error.isEmpty() || m_closing)
            return;
        snow_shot::presentation::ImageEditingSource source;
        source.image = std::move(image);
        source.pixelSize = source.image.size();
        source.canvasRect = QRectF(QPointF(), QSizeF(source.pixelSize));
        source.flattened = true;
        emit imageEditingSnapshotRequested(source);
    });
}

void ScreenshotPinnedWindow::stopLiveSource() {
    ++m_liveFrameGeneration;
    m_livePendingFrame = {};
    if (m_liveSource) {
        disconnect(m_liveSource.get(), nullptr, this, nullptr);
        m_liveSource->shutdown();
    }
}

void ScreenshotPinnedWindow::closeLiveWindow() {
    if (!isLiveWindow() || m_closing)
        return;
    m_persistenceEnabled = false;
    m_inactiveGroupClosing = true;
    m_deferredInactiveGroupClose = false;
    close();
}

void ScreenshotPinnedWindow::createLiveContextMenu() {
    using adqt::widgets::AdContextMenu;
    m_contextMenu = new AdContextMenu(this);
    m_contextMenu->setDeleteOnHide();
    m_contextMenu->setObjectName(QStringLiteral("screenshotPinnedContextMenu"));
    m_contextMenu->setFixedWidth(300);
    const auto add = [this](AdContextMenu* menu, const char* text, const char* name,
                            const QString& command, std::function<void()> activate) {
        auto* action = menu->addItem(QCoreApplication::translate("ScreenshotPinnedWindow", text));
        action->setObjectName(QString::fromLatin1(name));
        action->setProperty("screenshotPinnedTranslationSource", QString::fromUtf8(text));
        action->setProperty("liveCommand", command);
        action->setEnabled(liveCommandAllowed(command));
        connect(action, &QAction::triggered, this, [this, command, activate = std::move(activate)] {
            if (liveCommandAllowed(command))
                activate();
        });
        return action;
    };
    add(m_contextMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Copy to clipboard"),
        "screenshotPinnedCopyAction", QStringLiteral("copy_to_clipboard"),
        [this] { copyCurrentViewport(); });
    add(m_contextMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Save as file"),
        "screenshotPinnedSaveAsFileAction", QStringLiteral("save_as_file"),
        [this] { saveAsFile(); });
    add(m_contextMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Convert to Image"),
        "screenshotPinnedConvertToImageAction", QStringLiteral("convert_to_image"),
        [this] { convertLiveToImage(); });
    auto* process = m_contextMenu->addSubMenu(tr("Process image"));
    process->setObjectName(QStringLiteral("screenshotPinnedProcessImageMenu"));
    process->menuAction()->setProperty("screenshotPinnedTranslationSource", "Process image");
    auto* opacityMenu = process->addSubMenu(tr("Opacity"));
    opacityMenu->menuAction()->setProperty("screenshotPinnedTranslationSource", "Opacity");
    add(opacityMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Increase 10%"),
        "screenshotPinnedIncreaseOpacityAction", QStringLiteral("increase_opacity"),
        [this] { executeImageCommand(QStringLiteral("increase_opacity")); });
    add(opacityMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Decrease 10%"),
        "screenshotPinnedDecreaseOpacityAction", QStringLiteral("decrease_opacity"),
        [this] { executeImageCommand(QStringLiteral("decrease_opacity")); });
    m_opacityActions = new QActionGroup(opacityMenu);
    m_opacityActions->setExclusive(true);
    auto* scaleMenu = process->addSubMenu(tr("Scale"));
    scaleMenu->menuAction()->setProperty("screenshotPinnedTranslationSource", "Scale");
    add(scaleMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Increase 10%"),
        "screenshotPinnedIncreaseScaleAction", QStringLiteral("increase_scale"),
        [this] { executeImageCommand(QStringLiteral("increase_scale")); });
    add(scaleMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Decrease 10%"),
        "screenshotPinnedDecreaseScaleAction", QStringLiteral("decrease_scale"),
        [this] { executeImageCommand(QStringLiteral("decrease_scale")); });
    m_scaleActions = new QActionGroup(scaleMenu);
    m_scaleActions->setExclusive(true);
    for (int percent : {25, 50, 75, 100}) {
        auto* opacity = opacityMenu->addItem(tr("%1%").arg(percent));
        opacity->setCheckable(true);
        opacity->setData(percent);
        m_opacityActions->addAction(opacity);
        auto* scale = scaleMenu->addItem(tr("%1%").arg(percent));
        scale->setCheckable(true);
        scale->setData(percent);
        m_scaleActions->addAction(scale);
    }
    connect(m_opacityActions, &QActionGroup::triggered, this,
            [this](QAction* action) { setOpacityPercent(action->data().toInt()); });
    connect(m_scaleActions, &QActionGroup::triggered, this,
            [this](QAction* action) { applyScale(action->data().toInt()); });
    const struct {
        const char* text;
        const char* name;
        const char* command;
    } commands[] = {{QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Rotate clockwise"),
                     "screenshotPinnedRotateClockwiseAction", "rotate_clockwise"},
                    {QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Rotate counterclockwise"),
                     "screenshotPinnedRotateCounterClockwiseAction", "rotate_counterclockwise"},
                    {QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Flip horizontally"),
                     "screenshotPinnedFlipHorizontalAction", "flip_horizontal"},
                    {QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Flip vertically"),
                     "screenshotPinnedFlipVerticalAction", "flip_vertical"},
                    {QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Reset transform"),
                     "screenshotPinnedResetTransformAction", "reset_transform"}};
    for (const auto& item : commands) {
        const QString command = QString::fromLatin1(item.command);
        auto* action = add(process, item.text, item.name, command,
                           [this, command] { executeImageCommand(command); });
        action->setProperty("screenshotPinnedTranslationSource", item.text);
    }
    m_splitImageAction =
        add(process, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Image Splitting"),
            "screenshotPinnedSplitImageAction", QStringLiteral("split_image"),
            [this] { splitLiveImage(); });
    m_imageEditingAction =
        add(process, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Image Editing"),
            "screenshotPinnedImageEditingAction", QStringLiteral("image_editing"),
            [this] { editLiveImage(); });
    m_contextMenu->addSeparator();
    m_clickThroughAction =
        add(m_contextMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Click-through"),
            "screenshotPinnedClickThroughAction", QStringLiteral("toggle_click_through"),
            [this] { toggleClickThrough(); });
    m_clickThroughAction->setCheckable(true);
    m_lockAction = add(m_contextMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Lock mode"),
                       "screenshotPinnedLockAction", QStringLiteral("toggle_lock"),
                       [this] { setLockedMode(!m_lockedMode); });
    m_lockAction->setCheckable(true);
    auto* management = m_contextMenu->addSubMenu(tr("Window Management"));
    management->menuAction()->setProperty("screenshotPinnedTranslationSource", "Window Management");
    m_alwaysOnTopAction =
        add(management, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Always on Top"),
            "screenshotPinnedAlwaysOnTopAction", QStringLiteral("always_on_top"),
            [this] { toggleAlwaysOnTop(); });
    m_alwaysOnTopAction->setCheckable(true);
    m_showBorderAction = add(management, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Show border"),
                             "screenshotPinnedShowBorderAction", QStringLiteral("show_border"),
                             [this] { toggleShowBorder(); });
    m_showBorderAction->setCheckable(true);
    m_showShadowAction = add(management, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Show shadow"),
                             "screenshotPinnedShowShadowAction", QStringLiteral("show_shadow"),
                             [this] { toggleShowShadow(); });
    m_showShadowAction->setCheckable(true);
    m_contextMenu->addSeparator();
    m_closeAction = add(m_contextMenu, QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Close"),
                        "screenshotPinnedCloseAction", QStringLiteral("close_window"),
                        [this] { requestUserClose(); });
    connect(m_contextMenu, &QMenu::aboutToShow, this, [this] { refreshContextMenu(); });
    refreshMenuShortcutDisplays();
    refreshContextMenu();
}
