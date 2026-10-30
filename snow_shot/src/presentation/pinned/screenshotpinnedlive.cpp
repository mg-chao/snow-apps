// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotpinnedwindow.h"

#include "snow_shot/presentation/livewindowsource.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
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
#include "widgets/result.h"
#include "icon_renderer.h"
#include "theme/theme_manager.h"
#include "pinnedwindowplatform.h"
#include "screenshotclipboardplacementgeometry.h"
#include "livewindowframerenderer_p.h"

#include <QAction>
#include <QActionGroup>
#include <QColorSpace>
#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QScopedValueRollback>
#include <QPainter>
#include <QTimer>
#include <chrono>
#include <future>
#include <algorithm>
#include <utility>

using snow_shot::presentation::LiveWindowSource;
using snow_shot::presentation::LiveWindowState;

namespace {
// A pin can be smaller than a Result's artwork, padding, or wrapped title. Select
// the richest presentation that fits, prioritizing the state over the source title.
// Child size hints must never constrain the independently resizable pin.
class LiveWindowStatusView final : public QWidget {
  public:
    explicit LiveWindowStatusView(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("screenshotPinnedLiveStatusView"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_result = new adqt::widgets::AdResult(this);
        m_result->setObjectName(QStringLiteral("screenshotPinnedLiveStatus"));
        m_result->layout()->setSizeConstraint(QLayout::SetNoConstraint);
        adqt::widgets::AdResult::ComponentTokens tokens;
        tokens.titleFontSize = 20;
        tokens.iconFontSize = 64;
        m_result->setComponentTokens(tokens);
        m_description = new QLabel(this);
        m_description->setObjectName(QStringLiteral("screenshotPinnedLiveStatusDescription"));
        m_summary = new QLabel(this);
        m_summary->setObjectName(QStringLiteral("screenshotPinnedLiveStatusSummary"));
        m_icon = new QLabel(this);
        m_icon->setObjectName(QStringLiteral("screenshotPinnedLiveStatusCompactIcon"));
        for (auto* label : {m_description, m_summary, m_icon}) {
            label->setAlignment(Qt::AlignCenter);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
        }
        connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                this, [this] { updateAppearance(); });
        updateAppearance();
    }

    void setState(LiveWindowState state, const QString& title, const QString& description,
                  const QString& summary) {
        m_state = state;
        m_result->setTitle(title);
        m_result->setSubTitle(description);
        m_description->setText(description);
        m_summary->setText(summary);
        setAccessibleName(title);
        setAccessibleDescription(description);
        for (auto* label : {m_description, m_summary, m_icon})
            label->setAccessibleName(description);
        updateAppearance();
    }

  protected:
    bool event(QEvent* event) override {
        const bool handled = QWidget::event(event);
        switch (event->type()) {
        case QEvent::FontChange:
            updateAppearance();
            break;
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::LayoutRequest:
        case QEvent::DevicePixelRatioChange:
            updateLayout();
            break;
        default:
            break;
        }
        return handled;
    }

  private:
    void updateAppearance() {
        if (!m_icon)
            return;
        using namespace snow_shot::presentation::icons::custom;
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QFont textFont = font();
        textFont.setPixelSize(qRound(theme.fontSize));
        QPalette textPalette = palette();
        textPalette.setColor(QPalette::WindowText, theme.colorTextSecondary);
        for (auto* label : {m_description, m_summary}) {
            label->setFont(textFont);
            label->setPalette(textPalette);
        }
        m_compactPadding = qRound(theme.sizeXXS);
        const bool closed = m_state == LiveWindowState::Closed;
        m_result->setStatus(closed ? adqt::widgets::AdResult::Status::Error
                                   : adqt::widgets::AdResult::Status::Warning);
        const auto colors = adqt::icons::IconColors::twoTone(
            theme.colorTextTertiary, closed ? theme.colorError : theme.colorWarning);
        const auto icon = closed ? twotone::LiveWindowClosed(colors)
                          : m_state == LiveWindowState::Minimized
                              ? twotone::LiveWindowMinimized(colors)
                              : twotone::LiveWindowUnavailable(colors);
        m_result->setIcon(adqt::icons::makeIcon(icon));
        updateLayout();
    }

    void updateLayout() {
        if (!m_icon || m_updatingLayout)
            return;
        const QScopedValueRollback<bool> updating(m_updatingLayout, true);
        const int width = std::min(this->width(), 520);
        // Adopt the wrapping width before measuring Result's nested layouts.
        m_result->resize(width, m_result->height());
        m_result->layout()->activate();
        const int height = m_result->heightForWidth(width);
        QWidget* visible = nullptr;
        if (width >= m_result->minimumSizeHint().width() && height <= this->height()) {
            m_result->setGeometry((this->width() - width) / 2, (this->height() - height) / 2, width,
                                  height);
            visible = m_result;
        } else if (!rect().isEmpty()) {
            const int padding =
                std::min(m_compactPadding, std::min(this->width(), this->height()) / 4);
            const QRect content = rect().adjusted(padding, padding, -padding, -padding);
            for (auto* label : {m_description, m_summary}) {
                label->setGeometry(content);
                if (content.width() >= label->minimumSizeHint().width() &&
                    label->heightForWidth(content.width()) <= content.height()) {
                    visible = label;
                    break;
                }
            }
            if (!visible) {
                const int side = std::min({content.width(), content.height(), 32});
                m_icon->setGeometry(content);
                m_icon->setPixmap(m_result->icon().pixmap(QSize(side, side), devicePixelRatioF()));
                visible = m_icon;
            }
        }
        for (auto* widget : {static_cast<QWidget*>(m_result), static_cast<QWidget*>(m_description),
                             static_cast<QWidget*>(m_summary), static_cast<QWidget*>(m_icon)})
            widget->setVisible(widget == visible);
    }
    adqt::widgets::AdResult* m_result = nullptr;
    QLabel* m_description = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_icon = nullptr;
    LiveWindowState m_state = LiveWindowState::Unavailable;
    int m_compactPadding = 0;
    bool m_updatingLayout = false;
};
} // namespace

bool ScreenshotPinnedWindow::liveFrameAvailable() const {
    return m_liveSource && m_liveSource->state() == LiveWindowState::Live &&
           m_firstContentFramePublished && !m_closing;
}

bool ScreenshotPinnedWindow::liveCommandAllowed(const QString& command) const {
    if (!isLiveWindow())
        return true;
    if (command == u"apply_crop")
        return cropApplyAllowed();
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

QRectF ScreenshotPinnedWindow::liveSourceViewport() const {
    const QRectF base = m_liveAppliedCrop.isEmpty() ? QRectF(0, 0, 1, 1) : m_liveAppliedCrop;
    if (!m_cropRect || m_originalPixelSize.isEmpty())
        return base;
    return QRectF(base.x() + base.width() * m_cropRect->x() / m_originalPixelSize.width(),
                  base.y() + base.height() * m_cropRect->y() / m_originalPixelSize.height(),
                  base.width() * m_cropRect->width() / m_originalPixelSize.width(),
                  base.height() * m_cropRect->height() / m_originalPixelSize.height());
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
    if (result && m_liveSource && !m_closing)
        m_liveSource->releaseInitialFrame();
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
    const QRect outer = authoritativeNativeGeometry();
    const QRect content = authoritativeContentNativeGeometry();
    presentation.nativeContentMargins =
        QMargins(content.left() - outer.left(), content.top() - outer.top(),
                 outer.right() - content.right(), outer.bottom() - content.bottom());
    presentation.effectiveOpacity =
        (m_clickThroughActive ? m_clickThroughOpacityPercent : m_opacityPercent) / 100.0;
    presentation.alwaysOnTop = m_alwaysOnTop;
    presentation.clickThrough = m_clickThroughActive;
    presentation.visible = isVisible() && !isMinimized();
    presentation.imageTransform = m_imageTransform;
    presentation.sourceViewport = liveSourceViewport();
    presentation.backgroundColor =
        adqt::theme::ThemeManager::instance().resolveTheme(this).colorBgContainer;
    if (!presentation.backgroundColor.isValid())
        presentation.backgroundColor = palette().color(QPalette::Window);
    presentation.backgroundColor.setAlpha(255);
    const bool result = m_liveSource->setPresentation(presentation, error);
    m_liveNativePresentation = m_liveSource->usesNativePresentation();
    if (m_canvas)
        m_canvas->setVisible(liveFrameAvailable() && !m_liveNativePresentation);
    update();
    return result;
}

void ScreenshotPinnedWindow::updateLiveState() {
    if (!m_liveSource || m_closing)
        return;
    const bool available = liveFrameAvailable();
    if (!available && !m_liveStatusView)
        m_liveStatusView = new LiveWindowStatusView(this);
    if (m_liveStatusView) {
        QString text;
        QString summary;
        switch (m_liveSource->state()) {
        case LiveWindowState::Minimized:
            text = tr("The window is minimized. The latest content cannot be displayed.");
            summary = tr("Minimized");
            break;
        case LiveWindowState::Closed:
            text = tr("The window is closed. The latest content cannot be displayed.");
            summary = tr("Closed");
            break;
        case LiveWindowState::Initializing:
        case LiveWindowState::Unavailable:
            text = tr("Live content is unavailable. Retrying...");
            summary = tr("Unavailable");
            break;
        case LiveWindowState::Live:
            break;
        }
        const QString title = m_liveSource->windowTitle();
        static_cast<LiveWindowStatusView*>(m_liveStatusView)
            ->setState(m_liveSource->state(), title.isEmpty() ? tr("Untitled window") : title, text,
                       summary);
        m_liveStatusView->setToolTip(m_liveSource->stateMessage());
        m_liveStatusView->setGeometry(contentViewRect().toAlignedRect());
        m_liveStatusView->setVisible(!available);
        m_liveStatusView->raise();
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
    if (!m_liveAppliedCrop.isEmpty()) {
        const QRectF pixels(
            m_liveAppliedCrop.x() * frame.width(), m_liveAppliedCrop.y() * frame.height(),
            m_liveAppliedCrop.width() * frame.width(), m_liveAppliedCrop.height() * frame.height());
        frame = frame.copy(pixels.toAlignedRect().intersected(frame.rect()));
    }
    installLiveFrame(std::move(frame));
}

void ScreenshotPinnedWindow::installLiveFrame(QImage original) {
    const QRectF previousBackground = m_backgroundCanvasRect;
    const QRectF previousSurface = m_resultSurfaceCanvasRect;
    const bool sourceResized =
        !m_originalPixelSize.isEmpty() && m_originalPixelSize != original.size();
    if (sourceResized) {
        const auto mapping =
            QTransform::fromScale(qreal(original.width()) / m_originalPixelSize.width(),
                                  qreal(original.height()) / m_originalPixelSize.height());
        if (m_cropRect)
            m_cropRect = mapping.mapRect(*m_cropRect);
        if (m_interactionCropRect)
            m_interactionCropRect = mapping.mapRect(*m_interactionCropRect);
    }
    m_originalImage = std::move(original);
    // Only static content owns a materialized transformed raster. Live actions
    // acquire their own snapshot; presentation samples these original pixels.
    m_transformedImage = m_imageTransform.isIdentity() ? m_originalImage : QImage();
    m_originalPixelSize = m_originalImage.size();
    const QSizeF sourceSize = m_canvasSourceRect.size();
    const qreal scaleX = sourceSize.width() / m_originalImage.width();
    const qreal scaleY = sourceSize.height() / m_originalImage.height();
    const auto displayedPixels = snow_shot::presentation::live_window_detail::transformedFrameSize(
        m_originalPixelSize, m_imageTransform);
    m_backgroundCanvasRect =
        QRectF(m_canvasSourceRect.topLeft(),
               QSizeF(displayedPixels.width() * scaleX, displayedPixels.height() * scaleY));
    m_resultSurfaceCanvasRect = m_backgroundCanvasRect;
    m_imageSource = ScreenshotImageSource::fromImage(m_originalImage, m_backgroundCanvasRect);
    m_screenshotRenderer->setPinnedLiveFrame(m_imageSource, m_imageTransform);
    if (sourceResized || previousBackground != m_backgroundCanvasRect ||
        previousSurface != m_resultSurfaceCanvasRect) {
        m_screenshotRenderer->setPinnedResultSurface(m_backgroundCanvasRect,
                                                     m_resultSurfaceCanvasRect, m_resultStyle);
        updateCanvasViewport();
    }
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
    const QRectF visible = liveSourceViewport();
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
        [future, output, transform, opacity,
         visible](const ScreenshotExportCancellation& cancellation) {
            while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
                if (cancellation.isCancellationRequested())
                    return QImage{};
            if (cancellation.isCancellationRequested())
                return QImage{};
            QImage frame = future.get();
            if (frame.isNull())
                return QImage{};
            if (visible != QRectF(0, 0, 1, 1))
                frame = frame.copy(QRectF(visible.x() * frame.width(), visible.y() * frame.height(),
                                          visible.width() * frame.width(),
                                          visible.height() * frame.height())
                                       .toAlignedRect()
                                       .intersected(frame.rect()));
            const QImage transformed =
                snowCanvasTransformImage(frame, transform, Qt::SmoothTransformation);
            QImage result(output, QImage::Format_ARGB32_Premultiplied);
            if (result.isNull())
                return QImage{};
            result.fill(Qt::transparent);
            result.setColorSpace(transformed.colorSpace());
            QPainter painter(&result);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(QRect(QPoint(), output), transformed);
            if (opacity < 1.0) {
                // Match ordinary pinned exports. Applying painter opacity during
                // scaled sampling quantizes alpha twice on some raster backends.
                painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                painter.fillRect(result.rect(), QColor(0, 0, 0, qRound(opacity * 255.0)));
            }
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
    const QRectF sourceViewport = liveSourceViewport();
    const quint64 generation = m_liveFrameGeneration;
    requestLiveSnapshot(
        [this, transform, size, sourceViewport, generation](QImage image, const QString& error) {
            m_liveSnapshotPending = false;
            if (m_closing || !m_liveSource)
                return;
            if (!image.isNull() && error.isEmpty() && liveFrameAvailable() &&
                generation == m_liveFrameGeneration && transform == m_imageTransform &&
                size == authoritativeContentNativeGeometry().size() &&
                sourceViewport == liveSourceViewport()) {
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
    endCropSelection(true);
    if (m_interactionCropRect)
        endControlledInteraction(false);
    const QRect geometry = authoritativeContentNativeGeometry();
    stopLiveSource();
    m_liveSource.reset();
    ++m_contentReplacementGeneration;

    // The snapshot already contains the viewport crop and orientation. It is a new
    // static source, not a resized frame from the old live source coordinate system.
    m_cropRect.reset();
    m_liveAppliedCrop = {};
    m_exactInitialWindowSize = {};
    m_imageTransform.reset();
    m_quarterTurns = 0;
    m_canvasSourceRect = QRectF(QPointF(), QSizeF(geometry.size()));
    m_backgroundCanvasRect = m_canvasSourceRect;
    m_resultSurfaceCanvasRect = m_canvasSourceRect;
    m_initialWindowSize = geometry.size();
    m_scalePercent = 100;
    m_preserveScaleForSettledGeometry = false;
    m_resultStyle = {};
    m_borderAppearance.reset();
    m_originalImage = std::move(image);
    m_originalPixelSize = m_originalImage.size();
    m_transformedImage = m_originalImage;
    m_imageSource = ScreenshotImageSource::fromImage(m_transformedImage, m_backgroundCanvasRect);
    m_screenshotRenderer->setImageSource(m_imageSource);
    m_screenshotRenderer->setPinnedResultSurface(m_backgroundCanvasRect, m_resultSurfaceCanvasRect,
                                                 m_resultStyle);
    m_liveNativePresentation = false;
    applyRuntimeBorderColor();
    updateCanvasViewport();
    updateControlsGeometry();
    if (m_liveStatusView)
        m_liveStatusView->hide();
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
    resetContextMenuActions();
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
