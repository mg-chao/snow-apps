#include "snow_shot/presentation/screenshotselectionexportuiservices.h"

#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QPointer>
#include <QScreen>
#include <QTimer>

#include <algorithm>

namespace {
ScreenshotRecognitionResults recognitionResultsForPinnedImage(
    const ScreenshotRecognitionResults& sourceResults, const QRect& sourceSelection,
    const ScreenshotResultStyle& resultStyle, const QSize& imageSize) {
    ScreenshotRecognitionResults results = sourceResults;
    if (!results.text.has_value() || !results.text->error.isEmpty() ||
        results.text->presentation == nullptr || sourceSelection.isEmpty() ||
        imageSize.isEmpty()) {
        return results;
    }

    const ScreenshotResultLayout layout =
        ScreenshotResultCompositor::layoutForContent(sourceSelection.size(), resultStyle);
    if (!layout.isValid() || layout.outputRect.size() != imageSize) {
        return results;
    }

    // Cached OCR quads are in the original screenshot canvas. The pinned image is a
    // separately rendered result whose origin is local to the new canvas and may include
    // effect padding for the result shadow.
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->lines = results.text->presentation->lines;
    const QPointF sourceOrigin = sourceSelection.topLeft();
    const QPointF destinationOrigin(layout.contentRect.topLeft());
    presentation->selection = layout.contentRect;
    for (ScreenshotOcrLine& line : presentation->lines) {
        for (QPointF& point : line.quad) {
            point = point - sourceOrigin + destinationOrigin;
        }
    }
    presentation->prepareForRendering();
    results.text->presentation = std::move(presentation);
    return results;
}

void applyPinRuntimeSettings(ScreenshotPinnedWindow::Config* config) {
    if (config == nullptr) {
        return;
    }
    const snow_shot::storage::PinToScreenSettings settings;
    config->mouseWheelZoomMode = settings.mouseWheelZoomMode();
    config->automaticTextRecognition =
        config->formattedTextDocument == nullptr && settings.automaticTextRecognition();
}
} // namespace

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <Windows.h>
#include <dwmapi.h>
#endif

class ScreenshotPinnedWindowPool final : public QObject {
  public:
    explicit ScreenshotPinnedWindowPool(QObject* parent = nullptr) : QObject(parent) {
        scheduleReplenish();
    }

    ~ScreenshotPinnedWindowPool() override {
        if (m_spare != nullptr) {
            delete m_spare;
            m_spare = nullptr;
        }
    }

    ScreenshotPinnedWindow* acquire(ScreenshotPinnedWindow::RuntimeMode mode) {
        ScreenshotPinnedWindow* window = m_spare;
        bool usedSpare = window != nullptr;
        if (usedSpare) {
            m_spare = nullptr;
        }

        if (window == nullptr) {
            const QElapsedTimer timer = [&]() {
                QElapsedTimer value;
                value.start();
                return value;
            }();
            window = new ScreenshotPinnedWindow(mode);
            if (window != nullptr) {
                SNOW_SHOT_PIN_PERF_COUNTER("shell.construction_ns", timer.nsecsElapsed());
            }
        }

        SNOW_SHOT_PIN_PERF_COUNTER(usedSpare ? "shell.hit" : "shell.miss", 1);
        scheduleReplenish();
        return window;
    }

  private:
    void scheduleReplenish() {
        if (m_replenishQueued) {
            return;
        }
        m_replenishQueued = true;
        QTimer::singleShot(0, this, [this]() {
            m_replenishQueued = false;
            if (m_spare != nullptr) {
                return;
            }
            auto* spare =
                new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
            if (!spare->prewarm(QGuiApplication::primaryScreen())) {
                spare->deleteLater();
                return;
            }
            m_spare = spare;
        });
    }

    QPointer<ScreenshotPinnedWindow> m_spare;
    bool m_replenishQueued = false;
};

namespace {
bool presentPinnedWindowAndSynchronize(ScreenshotPinnedWindow* window,
                                       const ScreenshotPinnedWindow::Config& config,
                                       const std::function<void()>& showMainWindowRequested) {
    if (window == nullptr) {
        return false;
    }
    QObject::disconnect(window, &ScreenshotPinnedWindow::showMainWindowRequested, window, nullptr);
    if (showMainWindowRequested) {
        QObject::connect(window, &ScreenshotPinnedWindow::showMainWindowRequested, window,
                         showMainWindowRequested);
    }
    SNOW_SHOT_PIN_PERF_MILESTONE("ui.pinned_window_constructed");
    if (!window->present(config)) {
        window->deleteLater();
        return false;
    }
    SNOW_SHOT_PIN_PERF_COUNTER("window.visible", window->isVisible() ? 1 : 0);
    SNOW_SHOT_PIN_PERF_COUNTER("window.geometry_valid",
                               window->currentNativeGeometry() == config.nativeGeometry ? 1 : 0);
    SNOW_SHOT_PIN_PERF_MILESTONE("window.present_returned");
#if defined(SNOW_SHOT_PIN_PERF_INSTRUMENTATION) && (defined(Q_OS_WIN) || defined(_WIN32))
    DwmFlush();
#endif
    SNOW_SHOT_PIN_PERF_MILESTONE("window.dwm_flushed");
    return true;
}
} // namespace

ScreenshotSelectionExportUiServices::ScreenshotSelectionExportUiServices(
    ScreenshotOcrRecognitionPort* recognition,
    ScreenshotQrRecognitionPort* qrRecognition, SnowShotApiClient* tableRecognition,
    std::function<void()> showMainWindowRequested)
    : m_recognition(recognition), m_qrRecognition(qrRecognition),
      m_tableRecognition(tableRecognition),
      m_showMainWindowRequested(std::move(showMainWindowRequested)),
      m_windowPool(std::make_unique<ScreenshotPinnedWindowPool>()) {}

ScreenshotSelectionExportUiServices::~ScreenshotSelectionExportUiServices() {
    cancelClipboardPublication();
}

bool ScreenshotSelectionExportUiServices::publishClipboard(QObject* receiver,
                                                           ScreenshotClipboardPayload payload,
                                                           ClipboardCompletion completion) {
    return publishClipboard(receiver, std::move(payload), std::move(completion),
                            ScreenshotClipboardService::reservePublication());
}

bool ScreenshotSelectionExportUiServices::publishClipboard(QObject* receiver,
                                                           ScreenshotClipboardPayload payload,
                                                           ClipboardCompletion completion,
                                                           quint64 publicationId) {
    if (publicationId == 0) {
        publicationId = ScreenshotClipboardService::reservePublication();
    }
    auto completionEnabled = std::make_shared<std::atomic_bool>(true);
    m_clipboardCompletionEnabled.push_back(completionEnabled);
    auto commit = ScreenshotClipboardService::commit(
        QApplication::clipboard(), receiver, std::move(payload), publicationId,
        [completionEnabled,
         completion = std::move(completion)](ScreenshotClipboardCommitResult result) mutable {
            if (completionEnabled->exchange(false, std::memory_order_acq_rel)) {
                completion(result.succeeded());
            }
        });
    if (commit.isValid()) {
        m_clipboardCommits.push_back(commit);
    }
    return commit.isValid();
}

void ScreenshotSelectionExportUiServices::cancelClipboardPublication() {
    for (const auto& completionEnabled : m_clipboardCompletionEnabled) {
        if (completionEnabled != nullptr) {
            completionEnabled->store(false, std::memory_order_release);
        }
    }
    for (const auto& commit : m_clipboardCommits) {
        commit.cancel();
    }
    m_clipboardCompletionEnabled.clear();
    m_clipboardCommits.clear();
}

void ScreenshotSelectionExportUiServices::setClipboardImage(const QImage& image) {
    if (m_windowPool == nullptr) {
        return;
    }
    static_cast<void>(ScreenshotClipboardService::commit(
        QApplication::clipboard(), m_windowPool.get(),
        ScreenshotClipboardService::prepareImage(image), [](ScreenshotClipboardCommitResult) {}));
}

bool ScreenshotSelectionExportUiServices::presentPinnedSelection(
    const ScreenshotPinnedSelectionRequest& request) {
    SNOW_SHOT_PIN_PERF_SCOPE("ui.present_pinned_selection");
    if (!request.isValid()) {
        return false;
    }

    auto* pinnedWindow =
        m_windowPool != nullptr
            ? m_windowPool->acquire(ScreenshotPinnedWindow::RuntimeMode::NoDocument)
            : nullptr;
    if (pinnedWindow == nullptr) {
        pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    }
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = request.geometry.nativeGeometry;
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(request.image.size()));
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect = config.canvasSourceRect;
    // The request image is already composited using resultStyle. Keep the pinned renderer neutral
    // so rounded corners and shadows are not applied a second time.
    config.resultStyle = ScreenshotResultStyle{};
    config.fullResolutionScaleBasis = request.fullResolutionScaleBasis;
    config.imageSource = ScreenshotImageSource::fromImage(request.image, config.canvasSourceRect);
    config.screen = request.screen;
    config.recognition = m_recognition;
    config.qrRecognition = m_qrRecognition;
    config.tableRecognition = m_tableRecognition;
    config.recognitionResults = recognitionResultsForPinnedImage(
        request.recognitionResults, request.selection, request.resultStyle, request.image.size());
    config.formattedTextDocument.reset();
    config.formattedPlainText.clear();
    applyPinRuntimeSettings(&config);
    return presentPinnedWindowAndSynchronize(pinnedWindow, config, m_showMainWindowRequested);
}

bool ScreenshotSelectionExportUiServices::presentPinnedImage(
    const QImage& image, QScreen* screen, const QRect& nativeGeometry,
    const QSize& fullResolutionScaleBasis, std::shared_ptr<QTextDocument> formattedTextDocument,
    const QString& formattedPlainText, qreal formattedTextDevicePixelRatio,
    ScreenshotClipboardOriginalContent originalContent) {
    SNOW_SHOT_PIN_PERF_SCOPE("ui.present_pinned_image");
    if (image.isNull() || screen == nullptr || nativeGeometry.isEmpty()) {
        return false;
    }

    SNOW_SHOT_PIN_PERF_COUNTER("source.mode.materialized", 1);
    SNOW_SHOT_PIN_PERF_COUNTER("source.retained_bytes", image.sizeInBytes());

    auto* pinnedWindow =
        m_windowPool != nullptr
            ? m_windowPool->acquire(ScreenshotPinnedWindow::RuntimeMode::NoDocument)
            : nullptr;
    if (pinnedWindow == nullptr) {
        pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    }
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = nativeGeometry;
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(image.size()));
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect = config.canvasSourceRect;
    config.fullResolutionScaleBasis =
        fullResolutionScaleBasis.isEmpty() ? image.size() : fullResolutionScaleBasis;
    config.initialScalePercent =
        100.0 * nativeGeometry.width() / (std::max)(1, config.fullResolutionScaleBasis.width());
    config.screen = screen;
    config.enableEditing = true;
    config.formattedTextDocument = std::move(formattedTextDocument);
    config.formattedPlainText = formattedPlainText;
    config.formattedTextDevicePixelRatio = formattedTextDevicePixelRatio;
    config.originalClipboardContent = std::move(originalContent);
    config.recognition = m_recognition;
    config.qrRecognition = m_qrRecognition;
    config.tableRecognition = m_tableRecognition;
    applyPinRuntimeSettings(&config);
    return presentPinnedWindowAndSynchronize(pinnedWindow, config, m_showMainWindowRequested);
}
