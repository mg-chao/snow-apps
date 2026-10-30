// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "screenshotpinnedcropgeometry.h"
#include "snow_draw_engine_qt/snow_canvas_overlay_style.h"
#include "screenshotpinnednativegeometrycontroller.h"
#include "pinnedwindowplatform.h"
#include "snow_shot/presentation/pinnedgeometry.h"
#include "snow_shot/presentation/pinnedwindowselectioncontroller.h"
#include "snow_shot/presentation/screenshotpinnededitcontroller.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotanimationplayback.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "widgets/message.h"

namespace crop_geometry = screenshot_pinned_crop_geometry;

QRectF ScreenshotPinnedWindow::visibleCanvasRect() const {
    if (!m_cropRect || m_originalPixelSize.isEmpty())
        return m_resultSurfaceCanvasRect;
    return crop_geometry::sourceToCanvas(m_originalPixelSize, m_imageTransform,
                                         m_backgroundCanvasRect)
        .mapRect(*m_cropRect);
}

QRectF ScreenshotPinnedWindow::exportVisibleCanvasRect() const {
    if (!m_animationTransformSnapshot)
        return visibleCanvasRect();
    const auto& snapshot = *m_animationTransformSnapshot;
    return m_cropRect ? crop_geometry::sourceToCanvas(m_originalPixelSize, snapshot.transform,
                                                      snapshot.backgroundRect)
                            .mapRect(*m_cropRect)
                      : snapshot.surfaceRect;
}

QSizeF ScreenshotPinnedWindow::scaleReferenceSize() const {
    QSizeF full = m_exactInitialWindowSize.isValid() && !m_exactInitialWindowSize.isEmpty()
                      ? m_exactInitialWindowSize
                      : QSizeF(m_initialWindowSize);
    if (m_quarterTurns % 2)
        full.transpose();
    if (!m_cropRect || m_originalPixelSize.isEmpty())
        return full;
    const qreal effect = m_resultStyle.effectWidth();
    QSizeF fraction(m_cropRect->width() / m_originalPixelSize.width() * m_canvasSourceRect.width() /
                        (m_canvasSourceRect.width() + 2 * effect),
                    m_cropRect->height() / m_originalPixelSize.height() *
                        m_canvasSourceRect.height() / (m_canvasSourceRect.height() + 2 * effect));
    if (m_quarterTurns % 2)
        fraction.transpose();
    return {full.width() * fraction.width(), full.height() * fraction.height()};
}

bool ScreenshotPinnedWindow::cropSelectionAllowed() const {
    return m_presented && m_firstContentFramePublished && !m_closing && !m_lockedMode &&
           !m_cropApplying && !m_contentReplacementJob.isValid() && !m_clickThroughActive &&
           !hideToTopActive() && !m_thumbnailMode && !m_ocrMode && !m_geometryAnimating &&
           !m_interactionPlacement && !m_selectionGeometryActive && !m_windowDragActive &&
           !animationTransformPending() && (!m_editController || !m_editController->editMode()) &&
           (!isLiveWindow() || liveFrameAvailable());
}

bool ScreenshotPinnedWindow::cropApplyAllowed() const {
    return m_cropRect && m_presented && m_firstContentFramePublished && !m_lockedMode &&
           !m_closing && !m_cropApplying && !m_geometryAnimating && !m_cropSelectionOrigin &&
           !m_interactionPlacement && !m_selectionGeometryActive &&
           !m_contentReplacementJob.isValid() && !animationTransformPending() &&
           !m_animationTransformSnapshot && (!isLiveWindow() || liveFrameAvailable());
}

bool ScreenshotPinnedWindow::beginCropSelection(const QPointF& position) {
    if (!cropSelectionAllowed())
        return false;
    const QRect geometry = authoritativeContentNativeGeometry();
    m_cropSelectionGeometry = geometry;
    m_cropSelectionCanvas = visibleCanvasRect();
    m_cropSelectionScale =
        m_scalePercent / 100.0 * scaleReferenceSize().width() / m_cropSelectionCanvas.width();
    const QRectF available = m_cropSelectionCanvas.intersected(m_backgroundCanvasRect);
    m_cropSelectionBounds =
        QRectF(QPointF(geometry.topLeft()) +
                   (available.topLeft() - m_cropSelectionCanvas.topLeft()) * m_cropSelectionScale,
               available.size() * m_cropSelectionScale)
            .intersected(QRectF(geometry));
    if (!m_cropSelectionBounds.contains(position))
        return false;
    m_cropContextSuppressed = false;
    m_cropSelectionOrigin = position;
    m_cropSelectionEnd = position;
    grabMouse();
    qApp->installEventFilter(this);
    return true;
}

void ScreenshotPinnedWindow::updateCropSelection(const QPointF& position) {
    if (!m_cropSelectionOrigin)
        return;
    const QRectF bounds(m_cropSelectionBounds);
    const QPointF end{std::clamp(position.x(), bounds.left(), bounds.right()),
                      std::clamp(position.y(), bounds.top(), bounds.bottom())};
    if (end == m_cropSelectionEnd)
        return;
    QWidget* target = m_liveNativePresentation ? static_cast<QWidget*>(this) : m_canvas;
    const QRectF previous = cropSelectionViewRect(target);
    m_cropSelectionEnd = end;
    if (target)
        target->update(crop_geometry::selectionRepaintRegion(
            previous, cropSelectionViewRect(target), target->rect()));
}

QRectF ScreenshotPinnedWindow::cropSelectionViewRect(const QWidget* target) const {
    if (!m_cropSelectionOrigin || !target)
        return {};
    const auto native = QRectF(*m_cropSelectionOrigin, m_cropSelectionEnd).normalized();
    const QRectF content = contentViewRect();
    const qreal sx = content.width() / m_cropSelectionGeometry.width();
    const qreal sy = content.height() / m_cropSelectionGeometry.height();
    const QPointF topLeft(content.x() + (native.x() - m_cropSelectionGeometry.x()) * sx,
                          content.y() + (native.y() - m_cropSelectionGeometry.y()) * sy);
    return QRectF(target->mapFrom(this, topLeft),
                  QSizeF(native.width() * sx, native.height() * sy));
}

void ScreenshotPinnedWindow::paintCropSelection(QPainter& painter, const QWidget* target) const {
    snowCanvasPaintSelectionMarquee(painter, cropSelectionViewRect(target));
}

void ScreenshotPinnedWindow::endCropSelection(bool cancel, const QPoint& menuPosition) {
    if (!m_cropSelectionOrigin)
        return;
    const QRectF selection = QRectF(*m_cropSelectionOrigin, m_cropSelectionEnd).normalized();
    QWidget* target = m_liveNativePresentation ? static_cast<QWidget*>(this) : m_canvas;
    const QRectF previous = cropSelectionViewRect(target);
    m_cropSelectionOrigin.reset();
    m_cropContextSuppressed = true;
    qApp->removeEventFilter(this);
    if (QWidget::mouseGrabber() == this)
        releaseMouse();
    if (target)
        target->update(crop_geometry::selectionRepaintRegion(previous, {}, target->rect()));
    if (cancel || !cropSelectionAllowed())
        return;
    if (selection.width() < 10 || selection.height() < 10) {
        showContextMenu(menuPosition);
        return;
    }
    const qreal sx = 1.0 / m_cropSelectionScale;
    const qreal sy = sx;
    const QRectF canvas(
        m_cropSelectionCanvas.x() + (selection.x() - m_cropSelectionGeometry.x()) * sx,
        m_cropSelectionCanvas.y() + (selection.y() - m_cropSelectionGeometry.y()) * sy,
        selection.width() * sx, selection.height() * sy);
    if (setCropCanvasRect(canvas)) {
        if (m_selectionController)
            m_selectionController->deselectWindow(this);
        setWindowSelected(false);
        schedulePersistence();
    }
}

bool ScreenshotPinnedWindow::handleCropPointer(QObject* watched, QEvent* event) {
    if (!event)
        return false;
    if (event->type() == QEvent::ContextMenu) {
        const auto* context = static_cast<QContextMenuEvent*>(event);
        if (context->reason() == QContextMenuEvent::Mouse &&
            (m_cropSelectionOrigin || m_cropContextSuppressed)) {
            event->accept();
            return true;
        }
    }
    if (m_cropSelectionOrigin) {
        if (event->type() == QEvent::Wheel || event->type() == QEvent::ScreenChangeInternal ||
            event->type() == QEvent::DevicePixelRatioChange ||
            (event->type() == QEvent::MouseButtonPress &&
             static_cast<QMouseEvent*>(event)->button() != Qt::RightButton)) {
            endCropSelection(true);
            return false;
        }
        if ((watched == this &&
             (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
              event->type() == QEvent::UngrabMouse)) ||
            ((event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) &&
             static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)) {
            endCropSelection(true);
            event->accept();
            return true;
        }
        if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (event->type() == QEvent::MouseButtonRelease && mouse->button() != Qt::RightButton)
                return false;
            if (event->type() == QEvent::MouseMove && !mouse->buttons().testFlag(Qt::RightButton)) {
                endCropSelection(true);
                return true;
            }
            const QPointF position = mouse->spontaneous() && physicalCursorPosition()
                                         ? QPointF(*physicalCursorPosition())
                                         : nativePositionForWindowPosition(
                                               windowPositionForEvent(watched, mouse->position()));
            updateCropSelection(position);
            if (event->type() == QEvent::MouseButtonRelease)
                endCropSelection(false, mouse->globalPosition().toPoint());
            event->accept();
            return true;
        }
    }
    const auto* widget = qobject_cast<QWidget*>(watched);
    if (watched != this && watched != m_canvas && watched != m_recognitionContent &&
        (!widget || !m_recognitionContent || !m_recognitionContent->isAncestorOf(widget)))
        return false;
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::RightButton) {
            m_cropContextSuppressed = false;
            const auto local = windowPositionForEvent(watched, mouse->position());
            if (!isControlsPanelPosition(local.toPoint()) &&
                beginCropSelection(nativePositionForWindowPosition(local))) {
                event->accept();
                return true;
            }
        }
    }
    return false;
}

bool ScreenshotPinnedWindow::setCropCanvasRect(const QRectF& requested) {
    const QRectF clipped = requested.intersected(m_backgroundCanvasRect);
    if (!crop_geometry::valid(clipped) || m_originalPixelSize.isEmpty())
        return false;
    const QRectF previous = visibleCanvasRect();
    const QRect native =
        m_thumbnailMode ? snow_shot::presentation::pinnedContentRect(m_preThumbnailNativeGeometry)
                        : authoritativeContentNativeGeometry();
    const auto mapping = crop_geometry::sourceToCanvas(m_originalPixelSize, m_imageTransform,
                                                       m_backgroundCanvasRect);
    const QRectF source = mapping.inverted().mapRect(clipped);
    const qreal sx = m_scalePercent / 100.0 * scaleReferenceSize().width() / previous.width();
    const qreal sy = sx;
    const QRect target(qRound(native.x() + (clipped.x() - previous.x()) * sx),
                       qRound(native.y() + (clipped.y() - previous.y()) * sy),
                       std::max(1, qRound(clipped.width() * sx)),
                       std::max(1, qRound(clipped.height() * sy)));
    bool applied = false;
    {
        QScopedValueRollback<std::optional<QRectF>> crop(m_cropRect, source);
        QScopedValueRollback<bool> preserve(m_preserveScaleForSettledGeometry, true);
        const QRect outer = snow_shot::presentation::pinnedOuterRect(target);
        if (m_thumbnailMode) {
            m_preThumbnailNativeGeometry = outer;
            if (screen())
                m_preThumbnailPlacement =
                    snow_shot::presentation::pinnedPlacement(outer, *screen());
            applied = true;
        } else {
            applied = applyWindowGeometry(outer, GeometryMutation::ContentReplacement);
        }
        if (applied) {
            crop.commit();
            preserve.commit();
        }
    }
    if (!applied) {
        updateCropPresentation();
        return false;
    }
    updateCropPresentation();
    return true;
}

void ScreenshotPinnedWindow::updateCropPresentation() {
    ++m_contentReplacementGeneration;
    invalidatePendingCopy();
    cancelCloudUpload();
    if (m_quickSaveArtifact) {
        m_quickSaveArtifact->cancel();
        m_quickSaveArtifact.reset();
        m_quickSavePending = false;
    }
    if (m_nativeExportArtifact)
        m_nativeExportArtifact->cancel();
    m_fileSaveJob.cancel();
    m_fileSaveJob = {};
    applyRuntimeBorderColor();
    updateCanvasViewport();
    updateControlsGeometry();
    if (m_applyCropAction)
        m_applyCropAction->setEnabled(cropApplyAllowed());
    refreshContextMenuIfVisible();
    if (m_editController)
        m_editController->updatePlacement();
    if (isLiveWindow())
        scheduleLivePresentation();
}

void ScreenshotPinnedWindow::applyCrop() {
    if (!cropApplyAllowed())
        return;
    if (m_originalImage.isNull()) {
        requestMaterializedImage([this](bool ready) {
            if (ready)
                applyCrop();
        });
        return;
    }
    const QRect pixels = crop_geometry::pixelBounds(*m_cropRect, m_originalPixelSize);
    if (pixels.isEmpty())
        return;
    struct Prepared {
        QImage original;
        QImage transformed;
        QTransform transform;
        std::shared_ptr<const ScreenshotAnimationSource> animation;
        bool bakedStyle = false;
    };
    m_cropApplying = true;
    updateAnimationSuspension();
    refreshContextMenu();
    const auto crop = m_cropRect;
    const auto generation = m_presentationGeneration;
    const auto replacement = m_contentReplacementGeneration;
    auto* watcher = new QFutureWatcher<Prepared>(this);
    connect(watcher, &QFutureWatcher<Prepared>::finished, this,
            [this, watcher, pixels, crop, generation, replacement] {
                Prepared prepared;
                try {
                    prepared = watcher->future().takeResult();
                } catch (...) {
                    // Allocation/worker failure must leave both the original and viewport intact.
                }
                watcher->deleteLater();
                m_cropApplying = false;
                if (!m_closing && !m_lockedMode && generation == m_presentationGeneration &&
                    replacement == m_contentReplacementGeneration && crop == m_cropRect) {
                    const bool success =
                        !prepared.original.isNull() && !prepared.transformed.isNull() &&
                        commitPreparedCrop(pixels, std::move(prepared.original),
                                           std::move(prepared.transformed), prepared.transform,
                                           std::move(prepared.animation), prepared.bakedStyle);
                    if (!success) {
                        adqt::widgets::AdMessage::Request message;
                        message.key = QStringLiteral("screenshot-pinned-crop-status");
                        message.content = tr("The pinned image could not be prepared");
                        adqt::widgets::AdMessageService::error(std::move(message), this);
                    }
                }
                updateAnimationSuspension();
                refreshContextMenu();
            });
    const QImage image = m_originalImage;
    const auto source = m_animationSource;
    const auto orientation = m_imageTransform;
    const auto style = m_resultStyle;
    const auto background = m_backgroundCanvasRect;
    const bool bake = !source && !isLiveWindow() &&
                      (style.region || style.cornerRadius > 0 || style.effectWidth() > 0);
    watcher->setFuture(QtConcurrent::run(snow_shot::runtime::trackRuntimeWork(
        [image, source, orientation, style, background, pixels, bake] {
            Prepared result;
            result.bakedStyle = bake;
            if (bake) {
                const auto oriented =
                    snowCanvasTransformImage(image, orientation, Qt::SmoothTransformation);
                const auto scaled = ScreenshotResultCompositor::scaledStyle(
                    style, oriented.width() / background.width());
                const auto composed = ScreenshotResultCompositor::compose(oriented, scaled);
                const auto mapping = QImage::trueMatrix(orientation, image.width(), image.height());
                auto region = mapping.mapRect(QRectF(pixels)).toAlignedRect();
                region.translate(
                    -ScreenshotResultCompositor::layoutForContent(oriented.size(), scaled)
                         .outputRect.topLeft());
                result.original = snowCanvasCopyImage(composed, region);
            } else {
                result.original = snowCanvasCopyImage(image, pixels);
                result.transform = QImage::trueMatrix(orientation, pixels.width(), pixels.height());
            }
            result.transformed =
                ScreenshotResultCompositor::normalizeImage(snowCanvasTransformImage(
                    result.original, result.transform, Qt::SmoothTransformation));
            if (source) {
                auto next = std::make_shared<ScreenshotAnimationSource>(*source);
                next->sourceCropRect = pixels.translated(next->sourceCropRect.topLeft());
                next->canvasSize = pixels.size();
                next->firstFrame = snowCanvasCopyImage(next->firstFrame, pixels);
                next->firstDisplayFrame =
                    source->firstFrame.constBits() == source->firstDisplayFrame.constBits()
                        ? next->firstFrame
                        : snowCanvasCopyImage(next->firstDisplayFrame, pixels);
                if (next->firstFrame.isNull() || next->firstDisplayFrame.isNull())
                    return Prepared{};
                result.animation = std::move(next);
            }
            return result;
        })));
}

bool ScreenshotPinnedWindow::commitPreparedCrop(
    const QRect& pixels, QImage original, QImage transformed, const QTransform& transform,
    std::shared_ptr<const ScreenshotAnimationSource> animation, bool bakedStyle) {
    const qreal ux = m_canvasSourceRect.width() / m_originalPixelSize.width();
    const qreal uy = m_canvasSourceRect.height() / m_originalPixelSize.height();
    const auto mapping = crop_geometry::sourceToCanvas(m_originalPixelSize, m_imageTransform,
                                                       m_backgroundCanvasRect);
    const QRectF canvas = mapping.mapRect(QRectF(pixels));
    if (!setCropCanvasRect(canvas))
        return false;
    QSizeF baseline = scaleReferenceSize();
    if (!bakedStyle && m_quarterTurns % 2)
        baseline.transpose();
    const bool recognitionVisible = m_ocrMode;
    const auto recognitionMode = m_recognitionSession
                                     ? m_recognitionSession->mode()
                                     : ScreenshotRecognitionSessionController::Mode::Text;
    auto results = recognitionSnapshot();
    if (bakedStyle)
        results = transformedRecognitionSnapshot(std::move(results), m_canvasSourceRect,
                                                 m_originalPixelSize, m_imageTransform,
                                                 m_transformedImage.size(), m_backgroundCanvasRect);
    const QPointF offset = bakedStyle ? QPointF()
                                      : canvas.topLeft() - m_canvasSourceRect.topLeft() -
                                            QPointF(pixels.x() * ux, pixels.y() * uy);
    const auto translatePresentation =
        [&offset](std::shared_ptr<ScreenshotOcrPresentation>& value) {
            if (!value)
                return;
            value = std::make_shared<ScreenshotOcrPresentation>(*value);
            value->selection.translate(offset.toPoint());
            for (auto& line : value->lines) {
                for (auto& point : line.quad)
                    point += offset;
                for (auto& quad : line.sourceLineQuads)
                    for (auto& point : quad)
                        point += offset;
            }
            value->prepareForRendering();
        };
    if (results.text) {
        translatePresentation(results.text->presentation);
        results.text->filteredImage = {};
    }
    translatePresentation(results.translatedText);
    if (isLiveWindow()) {
        const QRectF previous =
            m_liveAppliedCrop.isEmpty() ? QRectF(0, 0, 1, 1) : m_liveAppliedCrop;
        m_liveAppliedCrop =
            QRectF(previous.x() + previous.width() * pixels.x() / m_originalPixelSize.width(),
                   previous.y() + previous.height() * pixels.y() / m_originalPixelSize.height(),
                   previous.width() * pixels.width() / m_originalPixelSize.width(),
                   previous.height() * pixels.height() / m_originalPixelSize.height());
        ++m_liveFrameGeneration;
    }
    m_cropRect.reset();
    ++m_contentReplacementGeneration;
    if (m_replacementPersistenceWriter)
        m_persistenceWriter = m_replacementPersistenceWriter;
    m_originalClipboardContent = {};
    m_formattedTextDocument.reset();
    m_formattedPlainText.clear();
    m_formattedTextAvailable = false;
    m_formattedTextDevicePixelRatio = 1;
    m_originalImage = original;
    m_originalPixelSize = original.size();
    m_ocrSupported = screenshotOcrImageWithinPixelLimit(m_originalPixelSize);
    m_transformedImage = isLiveWindow() && !transform.isIdentity() ? QImage() : transformed;
    m_imageTransform = transform;
    if (bakedStyle)
        m_quarterTurns = 0;
    m_canvasSourceRect =
        bakedStyle ? canvas
                   : QRectF(canvas.topLeft(), QSizeF(pixels.width() * ux, pixels.height() * uy));
    m_backgroundCanvasRect = canvas;
    m_resultSurfaceCanvasRect = canvas;
    m_resultStyle = {};
    m_borderAppearance.reset();
    m_exactInitialWindowSize = baseline;
    m_initialWindowSize = baseline.toSize().expandedTo(QSize(1, 1));
    m_imageSource = ScreenshotImageSource::fromImage(
        isLiveWindow() ? m_originalImage : m_transformedImage, canvas);
    m_recognitionResults = std::move(results);
    m_recognitionTargetReady = false;
    refreshColorSwatch();
    m_animationSource = std::move(animation);
    if (isLiveWindow())
        m_screenshotRenderer->setPinnedLiveFrame(m_imageSource, m_imageTransform);
    else
        m_screenshotRenderer->setImageSource(m_imageSource);
    m_screenshotRenderer->setPinnedResultSurface(canvas, canvas, m_resultStyle);
    if (m_animationPlayback) {
        static_cast<void>(m_animationPlayback->adoptCroppedSource(m_animationSource, original,
                                                                  transformed, transform));
    } else if (!isLiveWindow()) {
        configureRecognitionSession();
        configureRecognitionTarget();
        if (recognitionVisible)
            activateRecognitionMode(static_cast<int>(recognitionMode), false);
    }
    updateCropPresentation();
    schedulePersistence();
    return true;
}
