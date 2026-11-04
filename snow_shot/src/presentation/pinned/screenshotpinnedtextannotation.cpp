#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotpinnededitcontroller.h"
#include "snow_shot/presentation/screenshotquicktextannotationtoolbar.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshottextannotationgeometry.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QApplication>
#include <QScopedValueRollback>
#include <QTextDocument>
#include <utility>

namespace {
QTransform imageToCanvas(const QSize& pixels, const QRectF& canvas) {
    if (pixels.isEmpty() || canvas.isEmpty())
        return {};
    return QTransform(canvas.width() / pixels.width(), 0, 0, canvas.height() / pixels.height(),
                      canvas.left(), canvas.top());
}
} // namespace

void ScreenshotPinnedWindow::synchronizeTextAnnotations(bool bindEmptySource) {
    if (!m_screenshotRenderer || isLiveWindow() || animatedImage())
        return;
    std::shared_ptr<ScreenshotOcrPresentation> ocr;
    const bool formatted = m_formattedTextDocument && !m_formattedTextDocument->isEmpty();
    if (m_textAnnotationObservedDocument != m_formattedTextDocument.get()) {
        disconnect(m_textAnnotationDocumentConnection);
        m_textAnnotationObservedDocument = m_formattedTextDocument.get();
        if (m_formattedTextDocument)
            m_textAnnotationDocumentConnection =
                connect(m_formattedTextDocument.get(), &QTextDocument::contentsChanged, this,
                        &ScreenshotPinnedWindow::clearTextAnnotations);
    }
    if (!formatted) {
        const auto text = m_recognitionTargetReady && m_recognitionSession
                              ? m_recognitionSession->cachedTextRecognitionResult()
                              : m_recognitionResults.text;
        if (text)
            ocr = text->presentation;
    }
    const auto previousRevision = m_textAnnotations.revision();
    const bool previouslyAnnotated = !m_textAnnotations.empty();
    const bool fingerprintRequired = previouslyAnnotated || bindEmptySource;
    const bool missingFingerprint = bindEmptySource && m_textAnnotations.fingerprint().isEmpty();
    bool sourceChanged = false;
    if (formatted &&
        (m_textAnnotationSourceDocument != m_formattedTextDocument.get() ||
         m_textAnnotationSourceDocumentRevision != m_formattedTextDocument->revision() ||
         missingFingerprint)) {
        m_textAnnotations.bindSource(
            ScreenshotTextAnnotationSource::Formatted,
            fingerprintRequired
                ? screenshotFormattedTextAnnotationFingerprint(*m_formattedTextDocument)
                : QByteArray{});
        m_textAnnotationSourceDocument = m_formattedTextDocument.get();
        m_textAnnotationSourceDocumentRevision = m_formattedTextDocument->revision();
        m_textAnnotationSourceOcr = nullptr;
        sourceChanged = true;
    } else if (!formatted && ocr &&
               (m_textAnnotationSourceOcr != ocr.get() || missingFingerprint)) {
        m_textAnnotations.bindSource(
            ScreenshotTextAnnotationSource::Ocr,
            fingerprintRequired ? screenshotOcrTextAnnotationFingerprint(*ocr) : QByteArray{});
        m_textAnnotationSourceOcr = ocr.get();
        m_textAnnotationSourceDocument = nullptr;
        sourceChanged = true;
    }
    if (sourceChanged && !m_textAnnotations.empty()) {
        QVector<int> lengths;
        if (formatted)
            lengths.append(m_formattedTextDocument->characterCount() - 1);
        else if (ocr)
            for (const auto& line : ocr->lines)
                lengths.append(static_cast<int>(line.text.size()));
        if (!m_textAnnotations.validForSource(lengths))
            m_textAnnotations.clear();
    }
    if (previouslyAnnotated && previousRevision != m_textAnnotations.revision()) {
        invalidatePendingCopy();
        schedulePersistence();
    }
    QTransform sourceToPixels;
    if (formatted) {
        sourceToPixels =
            QTransform::fromScale(m_formattedTextDevicePixelRatio, m_formattedTextDevicePixelRatio);
    } else if (!m_canvasSourceRect.isEmpty() && !m_originalPixelSize.isEmpty()) {
        const qreal sx = m_originalPixelSize.width() / m_canvasSourceRect.width();
        const qreal sy = m_originalPixelSize.height() / m_canvasSourceRect.height();
        sourceToPixels =
            QTransform(sx, 0, 0, sy, -m_canvasSourceRect.x() * sx, -m_canvasSourceRect.y() * sy);
    }
    const QTransform mapping = sourceToPixels * m_imageTransform *
                               imageToCanvas(m_transformedImage.size(), m_backgroundCanvasRect);
    const bool sourceAvailable = formatted || ocr != nullptr;
    if (sourceChanged || m_textAnnotationSourceAvailable != sourceAvailable ||
        m_textAnnotationRenderedRevision != m_textAnnotations.revision() ||
        m_textAnnotationRenderedTransform != mapping) {
        m_textAnnotationSnapshot = {};
        if (!m_textAnnotations.empty() && sourceAvailable) {
            if (formatted)
                m_textAnnotationSnapshot = m_textAnnotationRenderer.formattedSnapshot(
                    m_textAnnotations, *m_formattedTextDocument, mapping);
            else if (ocr)
                m_textAnnotationSnapshot = m_textAnnotationRenderer.ocrSnapshot(
                    m_textAnnotations, *ocr, mapping, QApplication::font());
        } else {
            m_textAnnotationRenderer.clear();
        }
        m_textAnnotationRenderedRevision = m_textAnnotations.revision();
        m_textAnnotationRenderedTransform = mapping;
        m_textAnnotationSourceAvailable = sourceAvailable;
        m_screenshotRenderer->setTextAnnotations(m_textAnnotationSnapshot);
    }
    m_screenshotRenderer->setTextAnnotationsVisible(!m_editController ||
                                                    !m_editController->editMode());
}

ScreenshotTextAnnotationRenderSnapshot ScreenshotPinnedWindow::textAnnotationSnapshot() const {
    return m_textAnnotationSnapshot;
}

void ScreenshotPinnedWindow::textAnnotationsChanged() {
    invalidatePendingCopy();
    synchronizeTextAnnotations();
    synchronizeQuickTextAnnotationToolbar();
    schedulePersistence();
}

void ScreenshotPinnedWindow::clearTextAnnotations() {
    m_textAnnotations.clear();
    m_textAnnotationRenderer.clear();
    m_textAnnotationSnapshot = {};
    m_textAnnotationRenderedRevision = ~quint64(0);
    if (m_screenshotRenderer)
        m_screenshotRenderer->setTextAnnotations({});
    m_textAnnotationSourceOcr = nullptr;
    m_textAnnotationSourceDocument = nullptr;
    m_quickTextAnnotationSelection.clear();
    destroyQuickTextAnnotationToolbar();
    textAnnotationsChanged();
}

void ScreenshotPinnedWindow::destroyQuickTextAnnotationToolbar() {
    m_quickTextAnnotationModelRevision = ~quint64(0);
    auto* toolbar = std::exchange(m_quickTextAnnotationToolbar, nullptr);
    if (!toolbar)
        return;
    disconnect(toolbar, nullptr, this, nullptr);
    toolbar->clearSelection();
    // Selection changes can arrive inside a toolbar button or color-picker callback.
    toolbar->deleteLater();
}

void ScreenshotPinnedWindow::synchronizeQuickTextAnnotationToolbar() {
    if (m_synchronizingQuickTextAnnotation)
        return;
    QScopedValueRollback<bool> synchronizing(m_synchronizingQuickTextAnnotation, true);
    const bool enabled =
        isVisible() && !m_closing && !m_destroying && !isLiveWindow() && !animatedImage() &&
        m_hiddenTextSelection && !m_ocrMode && m_recognitionContent &&
        m_recognitionContent->isVisible() && m_canvas &&
        (!m_editController || !m_editController->editMode()) &&
        snow_shot::storage::PinToScreenSettings().quickAnnotationForSelectedText() &&
        snow_shot::storage::PinToScreenSettings().textSelectionOnRecognitionResults() ==
            QStringLiteral("always");
    constexpr auto snapshotMode =
        ScreenshotRecognitionWindow::TextSelectionSnapshotMode::CompletedOnly;
    auto selection = enabled ? m_recognitionContent->textSelectionSnapshot(snapshotMode)
                             : ScreenshotTextSelectionSnapshot{};
    if (!enabled || selection.empty() || selection.inProgress ||
        selection.source != m_textAnnotations.source()) {
        destroyQuickTextAnnotationToolbar();
        m_quickTextAnnotationSelectionSuperseded = false;
        if (!m_quickTextAnnotationSelection.empty()) {
            m_quickTextAnnotationSelection.clear();
            ++m_quickTextAnnotationSelectionRevision;
        }
        return;
    }
    if (m_quickTextAnnotationSelectionSuperseded &&
        m_quickTextAnnotationSelection == selection.ranges)
        return;
    if (!m_quickTextAnnotationToolbar) {
        m_quickTextAnnotationToolbar = new ScreenshotQuickTextAnnotationToolbar(this);
        connect(m_quickTextAnnotationToolbar, &ScreenshotQuickTextAnnotationToolbar::superseded,
                this, [this] {
                    // Keep selection ownership after destroying the UI so geometry updates
                    // cannot take the toolbar back from a more recently selected pin.
                    m_quickTextAnnotationSelectionSuperseded = true;
                    destroyQuickTextAnnotationToolbar();
                });
        connect(m_quickTextAnnotationToolbar, &ScreenshotQuickTextAnnotationToolbar::toolClicked,
                this, [this](ScreenshotTextAnnotationTool tool) {
                    // Selection and empty pins need only source identity/revision. Serialize
                    // and hash the source only when creating persistent marks (or restoring them).
                    synchronizeTextAnnotations(true);
                    if (m_textAnnotations.toggle(m_quickTextAnnotationSelection, tool,
                                                 m_quickTextAnnotationToolbar->styleForTool(tool)))
                        textAnnotationsChanged();
                });
        connect(m_quickTextAnnotationToolbar, &ScreenshotQuickTextAnnotationToolbar::styleEdited,
                this,
                [this](ScreenshotTextAnnotationTool tool,
                       const ScreenshotTextAnnotationStyle& style, quint32 properties) {
                    if (m_textAnnotations.updateStyle(m_quickTextAnnotationSelection, tool, style,
                                                      properties))
                        textAnnotationsChanged();
                });
    }
    // Constructing shared UI services can synchronously update the source widget hierarchy.
    selection = m_recognitionContent->textSelectionSnapshot(snapshotMode);
    if (selection.empty() || selection.inProgress || !m_hiddenTextSelection) {
        destroyQuickTextAnnotationToolbar();
        return;
    }
    m_quickTextAnnotationSelectionSuperseded = false;
    const bool selectionChanged = m_quickTextAnnotationSelection != selection.ranges;
    if (selectionChanged) {
        m_quickTextAnnotationSelection = selection.ranges;
        ++m_quickTextAnnotationSelectionRevision;
    }
    const QRectF localBounds = m_canvas->canvasToViewTransform().mapRect(selection.canvasBounds);
    const QRect alignedBounds = localBounds.toAlignedRect();
    const QRect globalBounds(m_canvas->mapToGlobal(alignedBounds.topLeft()), alignedBounds.size());
    if (selectionChanged || m_quickTextAnnotationModelRevision != m_textAnnotations.revision()) {
        ScreenshotQuickTextAnnotationToolbar::States states;
        for (size_t i = 0; i < states.size(); ++i) {
            const auto tool = static_cast<ScreenshotTextAnnotationTool>(i);
            states[i].active = m_textAnnotations.hasStyle(selection.ranges, tool);
            states[i].style = m_textAnnotations.selectedStyle(
                selection.ranges, tool, &states[i].mixed, &states[i].mixedProperties);
        }
        m_quickTextAnnotationToolbar->updateSelection(globalBounds, states,
                                                      m_quickTextAnnotationSelectionRevision);
        m_quickTextAnnotationModelRevision = m_textAnnotations.revision();
    } else {
        m_quickTextAnnotationToolbar->updateSelectionGeometry(globalBounds);
    }
}
