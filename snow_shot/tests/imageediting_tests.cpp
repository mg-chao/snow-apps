// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas_automatic_selection_test_support.h"
#include "physical_key_test_support.h"
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "widgets/modal.h"
#include "widgets/message.h"
#include <QApplication>
#include <QFontDatabase>
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include <QClipboard>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include "snow_shot/presentation/screenshotprintservice.h"
#include <QDir>
#include <QUuid>
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include <QTextEdit>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QTranslator>
#include <QScreen>
#include <QScopeGuard>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>
#include <QWindow>
#include <QLabel>
#include <QPainter>
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotscrollingsnapshot.h"
#include "snow_stitch_images.h"
#include <QColorSpace>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <utility>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void awaitCondition(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 15000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(condition(), message);
}
void sendWindowInteraction(snow_shot::presentation::ImageEditingWindow& window, bool active) {
#if defined(Q_OS_WIN)
    MSG message{};
    message.hwnd = reinterpret_cast<HWND>(window.canvas()->window()->winId());
    message.message = active ? WM_ENTERSIZEMOVE : WM_EXITSIZEMOVE;
    qintptr result = 0;
    QWindowSystemInterface::handleNativeEvent(window.canvas()->window()->windowHandle(),
                                              QByteArrayLiteral("windows_generic_MSG"), &message,
                                              &result);
#else
    if (active)
        window.modal()->windowInteractionStarted();
    else
        window.modal()->windowInteractionFinished();
#endif
}
QJsonObject annotationData(const SnowCanvasRuntime& runtime) {
    auto data = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                    .object()
                    .value(QStringLiteral("document"))
                    .toObject();
    data.remove(QStringLiteral("revision"));
    return data;
}
class EditorTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* text, const char*, int) const override {
        return (QByteArray(context).contains("ImageEditingWindow") ||
                QByteArray(context) == "ScreenshotToolPalette")
                   ? QStringLiteral("[translated] ") + QString::fromUtf8(text)
                   : QString{};
    }
};
adqt::widgets::AdMessageHandle* editorMessage(snow_shot::presentation::ImageEditingWindow& window,
                                              const QString& key) {
    auto* messages = adqt::widgets::AdMessageService::instance(window.canvas()->window());
    require(messages != nullptr, "editor has a window-scoped message service");
    for (auto* handle : messages->findChildren<adqt::widgets::AdMessageHandle*>())
        if (handle->isOpen() && handle->key() == key)
            return handle;
    return nullptr;
}
adqt::widgets::AdMessageHandle* statusMessage(snow_shot::presentation::ImageEditingWindow& window) {
    return editorMessage(window, QStringLiteral("imageEditingStatus"));
}
class PreviewLayoutObserver final : public QObject {
  public:
    int hidden = 0;
    int resized = 0;
    bool eventFilter(QObject*, QEvent* event) override {
        hidden += event->type() == QEvent::Hide;
        resized += event->type() == QEvent::Resize;
        return false;
    }
};
void verifyLoadingFeedback(snow_shot::presentation::ImageEditingSource original) {
    using namespace snow_shot::presentation;
    using adqt::widgets::AdMessage;
    ImageEditingWindow window;
    window.present(original);
    QApplication::processEvents();
    auto* surface = window.canvas()->parentWidget();
    auto* editor = surface->window();
    auto* toolbar = window.palette()->window();
    const auto frame = editor->geometry();
    const auto preview = surface->geometry();
    const auto toolbarGeometry = toolbar->geometry();
    PreviewLayoutObserver observer;
    surface->installEventFilter(&observer);
    bool complete = false, success = false;
    QObject::connect(&window, &ImageEditingWindow::loaded, &window, [&](bool value) {
        complete = true;
        success = value;
    });
    auto layered = original;
    layered.layeredImage = ScreenshotImageSource::fromImage(original.image, original.canvasRect);
    layered.pixelSize = original.image.size();
    layered.image = {};
    window.present(layered);
    require(surface->isVisible() && !surface->isEnabled() && !window.modal()->contentLoading() &&
                !window.canvas()->isEnabled() && !toolbar->isEnabled(),
            "loading retains the preview and blocks edits without replacing modal content");
    QPointer<adqt::widgets::AdMessageHandle> loading = statusMessage(window);
    require(loading && loading->type() == AdMessage::Type::Loading &&
                loading->content() == QStringLiteral("Loading image…") &&
                loading->noticeWidget()->window() == editor,
            "image loading shows a window-scoped loading message");
    EditorTranslator translator;
    QApplication::installTranslator(&translator);
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(surface, &languageChange);
    require(loading->content() == QStringLiteral("[translated] Loading image…"),
            "loading message retranslates without changing layout");
    QApplication::removeTranslator(&translator);
    awaitCondition([&] { return complete; }, "layered image finishes loading");
    QApplication::processEvents();
    require(success && !statusMessage(window) && window.canvas()->isEnabled() &&
                toolbar->isEnabled(),
            "successful loading dismisses its message and restores editing");
    require(observer.hidden == 0 && observer.resized == 0 && editor->geometry() == frame &&
                surface->geometry() == preview && toolbar->geometry() == toolbarGeometry,
            "image loading keeps window, preview, and toolbar geometry stable");
    for (const bool history : {false, true}) {
        complete = false;
        if (history)
            window.presentHistory({});
        else
            window.presentPinnedRecord(QStringLiteral("missing-loading-feedback-fixture"));
        require(statusMessage(window) && statusMessage(window)->type() == AdMessage::Type::Loading,
                "history and pinned image loading share message feedback");
        awaitCondition([&] { return complete; }, "missing image completes loading");
        require(!success && statusMessage(window) &&
                    statusMessage(window)->type() == AdMessage::Type::Error &&
                    window.canvas()->isEnabled() && toolbar->isEnabled(),
                "loading errors replace the spinner and restore editing");
        window.present(original);
        require(!statusMessage(window), "successful retry clears previous loading errors");
    }
    auto flattened = original;
    flattened.flattened = true;
    window.present(flattened);
    require(!statusMessage(window) && window.canvas()->isEnabled() && toolbar->isEnabled(),
            "composited images open normally without an annotation notice");
    QEvent flattenedLanguageChange(QEvent::LanguageChange);
    QApplication::sendEvent(surface, &flattenedLanguageChange);
    require(!statusMessage(window), "language changes do not introduce an annotation notice");
    auto flattenedLayered = layered;
    flattenedLayered.flattened = true;
    complete = false;
    window.present(flattenedLayered);
    awaitCondition([&] { return complete; }, "composited layered image finishes loading");
    require(success && !statusMessage(window) && window.canvas()->isEnabled() &&
                toolbar->isEnabled(),
            "asynchronous composited loading dismisses its spinner without an annotation notice");
    ImageEditingSource failure;
    failure.error = QStringLiteral("Detailed source error");
    window.present(failure);
    QEvent errorLanguageChange(QEvent::LanguageChange);
    QApplication::sendEvent(surface, &errorLanguageChange);
    require(statusMessage(window) && statusMessage(window)->content() == failure.error,
            "source error details survive retranslation");
    window.present(layered);
    window.present(original);
    QApplication::processEvents();
    require(!statusMessage(window) && window.document()->image() == original.image,
            "superseded image loading cannot overwrite the new image or its message");
    window.shutdown();
    require(!statusMessage(window), "closing the editor clears its message");

    ImageEditingWindow fresh;
    complete = false;
    QObject::connect(&fresh, &ImageEditingWindow::loaded, &fresh, [&](bool value) {
        complete = true;
        success = value;
    });
    fresh.present(layered);
    auto* freshSurface = fresh.canvas()->parentWidget();
    const auto initialGeometry = freshSurface->geometry();
    PreviewLayoutObserver initialObserver;
    freshSurface->installEventFilter(&initialObserver);
    awaitCondition([&] { return complete; }, "initial layered image finishes loading");
    QApplication::processEvents();
    require(
        success && freshSurface->geometry() == initialGeometry && initialObserver.hidden == 0 &&
            initialObserver.resized == 0 &&
            qFuzzyCompare(fresh.canvas()->zoom(),
                          std::min((fresh.canvas()->width() - 48) / original.canvasRect.width(),
                                   (fresh.canvas()->height() - 48) / original.canvasRect.height())),
        "initial loading fits the image once in the final preview geometry");
    fresh.shutdown();
}
void verifySourceLoading(const snow_shot::presentation::ImageEditingSource& original) {
    using namespace snow_shot::presentation;
    using adqt::widgets::AdMessage;
    ImageEditingWindow window;
    int loads = 0;
    bool success = false;
    QObject::connect(&window, &ImageEditingWindow::loaded, &window, [&](bool value) {
        ++loads;
        success = value;
    });
    std::function<void(QImage)> deliver;
    const auto deferred = [&] {
        return ScreenshotExportSource::fromImageLoader(
            [&](QObject*, std::function<void(QImage)> callback) {
                deliver = std::move(callback);
                return true;
            });
    };
    window.presentSource(deferred());
    require(statusMessage(window) && statusMessage(window)->type() == AdMessage::Type::Loading,
            "capture source displays the editor loading message");
    awaitCondition([&] { return bool(deliver); }, "capture source requests its pixels");
    std::exchange(deliver, {})(original.image);
    awaitCondition([&] { return loads == 1; }, "capture source finishes loading");
    require(success && window.document()->image() == original.image &&
                window.document()->bounds() == QRectF(QPointF(), QSizeF(original.image.size())) &&
                !window.dirty() && !statusMessage(window),
            "capture source opens exact pixels with physical bounds and a clean working copy");

    window.presentSource({});
    awaitCondition([&] { return loads == 2; }, "invalid capture source completes");
    require(!success && statusMessage(window) &&
                statusMessage(window)->type() == AdMessage::Type::Error,
            "invalid capture sources display the existing load error");
    window.presentSource(deferred());
    awaitCondition([&] { return bool(deliver); }, "failed capture source requests its pixels");
    std::exchange(deliver, {})({});
    awaitCondition([&] { return loads == 3; }, "failed capture source completes");
    require(!success && statusMessage(window) &&
                statusMessage(window)->type() == AdMessage::Type::Error,
            "capture materialization failures replace the loading message");

    window.presentSource(deferred());
    awaitCondition([&] { return bool(deliver); }, "superseded capture source starts loading");
    window.present(original);
    require(loads == 4 && success, "replacement content loads immediately");
    std::exchange(deliver, {})(QImage(4, 4, QImage::Format_RGB32));
    QApplication::processEvents();
    require(loads == 4 && window.document()->image() == original.image && !statusMessage(window),
            "superseded capture completion cannot replace newer editor content");

    window.presentSource(deferred());
    awaitCondition([&] { return bool(deliver); }, "closing capture source starts loading");
    window.shutdown();
    std::exchange(deliver, {})(original.image);
    QApplication::processEvents();
    require(loads == 4 && !window.modal()->isOpen() && !statusMessage(window),
            "closing cancels capture loading without reopening the editor");

    ImageEditingWindowCoordinator coordinator({});
    coordinator.presentSource(ScreenshotExportSource::fromImage(original.image));
    coordinator.presentSource(ScreenshotExportSource::fromImage(original.image));
    const auto editors = coordinator.windows();
    require(editors.size() == 2 && editors[0] != editors[1],
            "each capture source opens an independent editor");
    awaitCondition(
        [&] {
            return editors[0]->document()->image() == original.image &&
                   editors[1]->document()->image() == original.image;
        },
        "both independent capture editors load");
    coordinator.shutdown();
}
void verifyScrollingSourceImport() {
    using namespace snow_shot::presentation;
    for (const bool horizontal : {false, true}) {
        QImage original(horizontal ? QSize(450, 400) : QSize(400, 450), QImage::Format_RGBA8888);
        original.setColorSpace(QColorSpace::SRgb);
        quint32 random = 1234567;
        for (int y = 0; y < original.height(); ++y)
            for (int x = 0; x < original.width(); ++x) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                original.setPixelColor(x, y, QColor::fromRgb(random | 0xff000000U));
            }
        SnowStitchConfig config{};
        require(snow_stitch_config_default(&config) != 0, "configure scrolling editor fixture");
        config.axis = horizontal ? SNOW_STITCH_AXIS_HORIZONTAL : SNOW_STITCH_AXIS_VERTICAL;
        std::unique_ptr<SnowStitchSession, decltype(&snow_stitch_session_destroy)> session(
            snow_stitch_session_create(&config), snow_stitch_session_destroy);
        require(bool(session), "create scrolling editor fixture");
        SnowStitchFrameOutcome outcome{};
        for (const QPoint origin : {QPoint(), horizontal ? QPoint(50, 0) : QPoint(0, 50)}) {
            auto* frame = new QImage(original.copy(QRect(origin, QSize(400, 400))));
            require(snow_stitch_session_push_external_rgba(
                        session.get(), 400, 400, frame->constBits(),
                        static_cast<size_t>(frame->sizeInBytes()),
                        [](void* context) { delete static_cast<QImage*>(context); }, frame,
                        &outcome) != 0,
                    "stitch overlapping editor fixture frames");
        }
        require(QSize(int(outcome.output_width), int(outcome.output_height)) == original.size(),
                "editor fixture stitches both frames along the selected axis");
        const QImage expected =
            original.copy(horizontal ? QRect(12, 0, 426, 400) : QRect(0, 12, 400, 426));
        auto snapshot = ScreenshotScrollingSnapshot::adoptNative(
            snow_stitch_session_snapshot_axis(session.get(), 12, 438), expected.size());
        require(snapshot.isValid(), "create trimmed scrolling editor snapshot");
        session.reset();
        ImageEditingWindow editor;
        bool loaded = false;
        QObject::connect(&editor, &ImageEditingWindow::loaded, &editor, [&](bool success) {
            require(success, "trimmed scrolling source loads into the editor");
            loaded = true;
        });
        editor.presentSource(ScreenshotExportSource::fromScrollingSnapshot(std::move(snapshot)));
        awaitCondition([&] { return loaded; }, "detached scrolling editor import completes");
        require(editor.document()->image() == expected &&
                    editor.document()->bounds() == QRectF(QPointF(), QSizeF(expected.size())),
                "editor imports exact stitched pixels and trim after scrolling session teardown");
        editor.shutdown();
    }
}
void mouse(QWidget* widget, QEvent::Type type, QPointF position, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, widget->mapToGlobal(position.toPoint()), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
void verifyLinearPreviewScaling() {
    using namespace snow_shot::presentation;
    ImageEditingDocument document;
    ImageEditingCanvas canvas(document);
    canvas.resize(320, 320);
    ImageEditingSource original;
    original.image = QImage(64, 64, QImage::Format_ARGB32);
    for (int y = 0; y < original.image.height(); ++y)
        for (int x = 0; x < original.image.width(); ++x)
            original.image.setPixelColor(x, y, x % 2 == 0 ? Qt::black : Qt::white);
    original.canvasRect = QRectF(-20, 15, 32, 32);
    require(document.load(original), "load linear preview scaling fixture");
    canvas.show();
    QApplication::processEvents();
    for (const double scale : {1.0, 0.75, 4.0}) {
        const auto center = document.bounds().center();
        canvas.setViewportCamera(center.x(), center.y(), 2 * scale / canvas.devicePixelRatioF());
        const auto pixels = canvas.grab().toImage();
        const auto imageRect =
            QTransform::fromScale(pixels.devicePixelRatio(), pixels.devicePixelRatio())
                .mapRect(canvas.canvasToViewTransform().mapRect(document.bounds()))
                .toAlignedRect();
        int blendedPixels = 0;
        for (int x = 1; x < imageRect.width() - 1; ++x) {
            const auto color = pixels.pixelColor(imageRect.left() + x, imageRect.center().y());
            if (scale == 1.0)
                require(color == original.image.pixelColor(x, original.image.height() / 2),
                        "actual-size preview preserves exact source pixels");
            blendedPixels += color.red() > 16 && color.red() < 239 &&
                             color.red() == color.green() && color.red() == color.blue() &&
                             color.alpha() == 255;
        }
        if (scale != 1.0)
            require(blendedPixels > 0, "zoomed preview linearly blends neighboring source pixels");
    }
    require(document.image() == original.image && !document.dirty(),
            "preview interpolation preserves the document pixels and saved state");
}
void verifyCropMeasurementColor() {
    using namespace snow_shot::presentation;
    ImageEditingDocument document;
    ImageEditingCanvas canvas(document);
    canvas.resize(320, 240);
    ImageEditingSource original;
    original.image = QImage(320, 240, QImage::Format_ARGB32);
    original.image.fill(Qt::black);
    original.canvasRect = QRectF(0, 0, 320, 240);
    require(document.load(original), "load crop measurement color fixture");
    canvas.show();
    QApplication::processEvents();
    for (const auto textColor : {Qt::black, Qt::white}) {
        auto palette = canvas.palette();
        palette.setColor(QPalette::Text, textColor);
        canvas.setPalette(palette);
        for (const auto mode :
             {ImageEditingCanvas::Mode::CropRows, ImageEditingCanvas::Mode::CropColumns}) {
            canvas.setMode(mode);
            const auto transform = canvas.canvasToViewTransform();
            mouse(&canvas, QEvent::MouseButtonPress, transform.map(QPointF(80, 60)), Qt::LeftButton,
                  Qt::LeftButton);
            mouse(&canvas, QEvent::MouseMove, transform.map(QPointF(240, 180)), Qt::NoButton,
                  Qt::LeftButton);
            const auto unit =
                canvas.grab(QRect(canvas.width() / 2, canvas.height() / 2 - 16, 60, 32)).toImage();
            int whitePixels = 0;
            for (int y = 0; y < unit.height(); ++y)
                for (int x = 0; x < unit.width(); ++x) {
                    const auto pixel = unit.pixelColor(x, y);
                    whitePixels += pixel.red() > 240 && pixel.green() > 240 && pixel.blue() > 240;
                }
            require(whitePixels > 10,
                    "crop unit text stays white in both directions regardless of text palette");
        }
    }
}
class DeferredOcr final : public ScreenshotOcrRecognitionPort {
  public:
    struct Pending {
        ScreenshotOcrRequest request;
        QPointer<QObject> receiver;
        Completion completion;
    };
    QVector<Pending> pending;
    int cancelled = 0;
    bool assetsReady = true;
    ScreenshotOcrAssetStatus assets{ScreenshotOcrAssetPhase::ReadyCached};
    RequestToken recognize(ScreenshotOcrRequest request, QObject* receiver,
                           Completion completion) override {
        pending.append({std::move(request), receiver, std::move(completion)});
        return quint64(pending.size());
    }
    void cancel(RequestToken) override {
        ++cancelled;
    }
    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return true;
    }
    bool modelFilesReady() const override {
        return assetsReady;
    }
    ScreenshotOcrAssetStatus assetStatus() const override {
        return assets;
    }
    void complete(int index, std::shared_ptr<ScreenshotOcrPresentation> result,
                  QString error = {}) {
        auto callback = pending[index].completion;
        if (pending[index].receiver)
            callback(ScreenshotOcrRecognitionResult{std::move(result), std::move(error)});
    }
};
snow_shot::presentation::ImageEditingSource source() {
    snow_shot::presentation::ImageEditingSource value;
    value.image = QImage(12, 10, QImage::Format_ARGB32);
    for (int y = 0; y < value.image.height(); ++y)
        for (int x = 0; x < value.image.width(); ++x)
            value.image.setPixel(x, y, qRgba(x * 10, y * 11, x + y, 20 + x + y));
    value.canvasRect = QRectF(-20, 15, 6, 5);
    return value;
}
void verifyRecognitionFeedback(snow_shot::presentation::ImageEditingSource original) {
    using namespace snow_shot::presentation;
    using adqt::widgets::AdMessage;
    const QString recognitionKey = QStringLiteral("imageEditingRecognition");
    const QString downloadKey = QStringLiteral("imageEditingModelDownload");
    const QString statusKey = QStringLiteral("imageEditingRecognitionStatus");
    DeferredOcr provider;
    ImageEditingWindow window(
        {[&] { return ScreenshotPinnedRecognitionProviders{&provider, nullptr, nullptr}; }});
    original.flattened = true;
    window.present(original);
    require(!statusMessage(window),
            "recognition fixture opens a composited image without a notice");
    ImageEditingSource failedSource;
    failedSource.error = QStringLiteral("Image source fixture failed");
    window.present(failedSource);
    QPointer<adqt::widgets::AdMessageHandle> imageError = statusMessage(window);
    require(imageError && imageError->type() == AdMessage::Type::Error &&
                imageError->content() == failedSource.error,
            "recognition fixture retains an unrelated image load error");
    window.palette()->ocrRequested();
    QPointer<adqt::widgets::AdMessageHandle> progress = editorMessage(window, recognitionKey);
    require(progress && progress->type() == AdMessage::Type::Loading &&
                progress->content() == QStringLiteral("Recognizing text") &&
                progress->noticeWidget()->window() == window.canvas()->window(),
            "text recognition uses a window-scoped loading message");
    require(imageError && imageError->isOpen(),
            "recognition progress does not replace image feedback");
    provider.complete(0, {}, QStringLiteral("Recognition fixture failed"));
    auto* failure = editorMessage(window, statusKey);
    require(!editorMessage(window, recognitionKey) && failure &&
                failure->type() == AdMessage::Type::Error &&
                failure->content() == QStringLiteral("Recognition fixture failed"),
            "recognition failure dismisses progress while keeping the error visible");
    require(imageError && imageError->isOpen(),
            "recognition cleanup does not dismiss unrelated image feedback");
    window.palette()->ocrRequested();
    require(editorMessage(window, recognitionKey), "failed recognition can retry with loading");
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = original.canvasRect.toRect();
    presentation->prepareForRendering();
    provider.complete(1, presentation);
    require(!editorMessage(window, recognitionKey) && window.recognition()->editing(),
            "successful recognition dismisses progress and enters editing");
    window.shutdown();
    require(!editorMessage(window, recognitionKey) && !editorMessage(window, statusKey),
            "closing the editor dismisses recognition feedback");

    DeferredOcr downloadProvider;
    downloadProvider.assetsReady = false;
    downloadProvider.assets = {ScreenshotOcrAssetPhase::Verifying};
    ImageEditingWindow downloading({[&] {
        return ScreenshotPinnedRecognitionProviders{&downloadProvider, nullptr, nullptr};
    }});
    downloading.present(original);
    auto* messages = adqt::widgets::AdMessageService::instance(downloading.canvas()->window());
    messages->setDefaultDurationMs(1);
    downloading.palette()->ocrRequested();
    require(editorMessage(downloading, recognitionKey) && !editorMessage(downloading, downloadKey),
            "component verification uses recognition loading without a download prompt");
    downloadProvider.assets = {ScreenshotOcrAssetPhase::Downloading, QStringLiteral("models"),
                               qint64(25), qint64(100)};
    awaitCondition([&] { return editorMessage(downloading, downloadKey) != nullptr; },
                   "a real component download displays progress");
    QPointer<adqt::widgets::AdMessageHandle> download = editorMessage(downloading, downloadKey);
    require(download->type() == AdMessage::Type::Loading &&
                download->content() ==
                    QStringLiteral("Preparing text recognition components (25%)") &&
                editorMessage(downloading, recognitionKey),
            "download and recognition loading remain independently addressable without timing out");
    downloadProvider.assets.receivedBytes = 75;
    awaitCondition(
        [&] {
            return download && download->content() ==
                                   QStringLiteral("Preparing text recognition components (75%)");
        },
        "component progress updates the existing loading message");
    require(download == editorMessage(downloading, downloadKey),
            "component progress keeps the same message handle");
    downloadProvider.assetsReady = true;
    downloadProvider.assets = {ScreenshotOcrAssetPhase::ReadyCached};
    awaitCondition([&] { return !editorMessage(downloading, downloadKey); },
                   "download readiness dismisses only component progress");
    require(editorMessage(downloading, recognitionKey),
            "recognition loading remains after component download completes");
    downloading.palette()->moveRequested();
    require(!editorMessage(downloading, recognitionKey) && !editorMessage(downloading, downloadKey),
            "leaving recognition dismisses its loading messages");
    downloadProvider.complete(0, {}, QStringLiteral("Cancelled fixture failed"));
    require(!editorMessage(downloading, statusKey),
            "a cancelled recognition completion cannot show a stale error");
    downloadProvider.assetsReady = false;
    downloadProvider.assets = {ScreenshotOcrAssetPhase::Downloading};
    downloading.palette()->ocrRequested();
    require(editorMessage(downloading, downloadKey), "download can restart after cancellation");
    downloading.shutdown();
    require(!editorMessage(downloading, recognitionKey) && !editorMessage(downloading, downloadKey),
            "closing during a download dismisses all recognition loading");

    auto oversized = original;
    oversized.flattened = false;
    oversized.image = QImage(3840, 2161, QImage::Format_ARGB32_Premultiplied);
    oversized.image.fill(Qt::transparent);
    oversized.canvasRect = QRectF(QPointF(), QSizeF(oversized.image.size()));
    ImageEditingWindow limited(
        {[&] { return ScreenshotPinnedRecognitionProviders{&provider, nullptr, nullptr}; }});
    limited.present(oversized);
    limited.palette()->ocrRequested();
    auto* warning = editorMessage(limited, statusKey);
    require(
        warning && warning->type() == AdMessage::Type::Warning &&
            warning->content() ==
                QStringLiteral("Text recognition is unavailable for screenshots larger than 4K") &&
            !editorMessage(limited, recognitionKey) && provider.pending.size() == 2,
        "recognition limits use the screenshot window's warning severity without starting OCR");
    limited.shutdown();
}
void verifyCanvasToolbar(snow_shot::presentation::ImageEditingSource original) {
    using namespace snow_shot::presentation;
    using adqt::widgets::AdButton;
    ImageEditingWindow window;
    window.present(original);
    QApplication::processEvents();
    auto* canvas = window.canvas();
    auto* editor = canvas->window();
    auto* toolbar = canvas->findChild<QWidget*>(QStringLiteral("imageEditingCanvasToolbar"));
    auto* fit = canvas->findChild<AdButton*>(QStringLiteral("imageEditingFit"));
    auto* actual = canvas->findChild<AdButton*>(QStringLiteral("imageEditingActualSize"));
    require(toolbar && toolbar->parentWidget() == canvas && !toolbar->isWindow() &&
                toolbar->isVisible() && fit && actual && fit->parentWidget() == toolbar &&
                actual->parentWidget() == toolbar,
            "zoom actions float together over the image canvas");
    require(fit->text().isEmpty() && fit->iconRef().isValid() && fit->size() == QSize(26, 26) &&
                fit->toolTip() == QStringLiteral("Fit image to window") &&
                actual->text().isEmpty() && actual->iconRef().isValid() &&
                actual->size() == fit->size() &&
                actual->toolTip() == QStringLiteral("View at actual size (100%)"),
            "zoom actions use the compact segmentation toolbar presentation");
    for (auto* action : {fit, actual})
        require(action->buttonStyle() == AdButton::ButtonStyle::Text &&
                    action->sizeClass() == AdButton::SizeClass::Small &&
                    action->focusPolicy() == Qt::StrongFocus &&
                    action->accessibleName() == action->toolTip(),
                "canvas zoom actions retain keyboard focus and accessible labels");
    require(!window.palette()->window()->findChild<AdButton*>(QStringLiteral("imageEditingFit")) &&
                !window.palette()->window()->findChild<AdButton*>(
                    QStringLiteral("imageEditingActualSize")),
            "zoom actions are separate from the drawing toolbar");
    const auto checkpoint = window.document()->checkpoint();
    editor->setMinimumSize(0, 0);
    const auto requireAnchored = [&] {
        require(canvas->rect().contains(toolbar->geometry()) &&
                    std::abs(QRectF(toolbar->geometry()).center().x() -
                             QRectF(canvas->rect()).center().x()) <= .5 &&
                    canvas->height() - toolbar->geometry().bottom() - 1 == 16,
                "zoom toolbar stays centered with the segmentation toolbar bottom inset");
    };
    for (const QSize size : {QSize(1100, 720), QSize(900, 600), QSize(540, 300)}) {
        editor->resize(size);
        QApplication::processEvents();
        requireAnchored();
        const auto geometry = toolbar->geometry();
        fit->click();
        const auto fittedZoom = canvas->zoom();
        const auto fittedCenter = canvas->center();
        actual->click();
        require(qFuzzyCompare(canvas->zoom(), original.image.width() / original.canvasRect.width() /
                                                  canvas->devicePixelRatioF()),
                "100 percent uses the source pixel scale");
        const QPointF anchor(100, 100);
        QWheelEvent zoom(anchor, canvas->mapToGlobal(anchor.toPoint()), {}, QPoint(0, 120),
                         Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &zoom);
        mouse(canvas, QEvent::MouseButtonPress, anchor, Qt::MiddleButton, Qt::MiddleButton);
        mouse(canvas, QEvent::MouseMove, anchor + QPointF(20, 10), Qt::NoButton, Qt::MiddleButton);
        mouse(canvas, QEvent::MouseButtonRelease, anchor + QPointF(20, 10), Qt::MiddleButton,
              Qt::NoButton);
        require(toolbar->geometry() == geometry,
                "image zooming and panning preserve the floating toolbar geometry");
        fit->click();
        require(qFuzzyCompare(canvas->zoom(), fittedZoom) && canvas->center() == fittedCenter &&
                    window.document()->checkpoint() == checkpoint && !window.dirty(),
                "floating fit action restores the camera without changing image content");
        mouse(toolbar, QEvent::MouseButtonPress, {1, 1}, Qt::MiddleButton, Qt::MiddleButton);
        mouse(toolbar, QEvent::MouseMove, {6, 1}, Qt::NoButton, Qt::MiddleButton);
        mouse(toolbar, QEvent::MouseButtonRelease, {6, 1}, Qt::MiddleButton, Qt::NoButton);
        require(canvas->center() == fittedCenter,
                "floating toolbar background gestures cannot pan the image");
    }
    EditorTranslator translator;
    QApplication::installTranslator(&translator);
    QApplication::processEvents();
    require(fit->toolTip() == QStringLiteral("[translated] Fit image to window") &&
                fit->accessibleName() == fit->toolTip() &&
                actual->toolTip() == QStringLiteral("[translated] View at actual size (100%)") &&
                actual->accessibleName() == actual->toolTip() && actual->text().isEmpty() &&
                actual->size() == fit->size(),
            "floating zoom labels retranslate while icon buttons retain their compact size");
    requireAnchored();
    QApplication::removeTranslator(&translator);
    editor->resize(900, 600);
    QApplication::processEvents();
    if (!qEnvironmentVariableIsEmpty("SNOW_IMAGE_EDITING_ZOOM_SCREENSHOT"))
        require(editor->grab().save(qEnvironmentVariable("SNOW_IMAGE_EDITING_ZOOM_SCREENSHOT")),
                "save floating zoom toolbar visual inspection artifact");
    window.shutdown();
    require(!toolbar->isVisible(), "closing the editor hides the canvas zoom toolbar");
}
void verifyCropToolHighlight(snow_shot::presentation::ImageEditingWindow& window) {
    using namespace snow_shot::presentation;
    using adqt::widgets::AdButton;
    auto* panel = window.palette()->mainPanel();
    auto* rows = panel->findChild<AdButton*>(QStringLiteral("imageEditingCropRows"));
    auto* columns = panel->findChild<AdButton*>(QStringLiteral("imageEditingCropColumns"));
    require(rows && columns, "editor crop tools exist");
    const auto requireHighlight = [&](AdButton* selected) {
        for (auto* button : {rows, columns}) {
            const bool active = button == selected;
            const auto expectedStyle =
                active ? AdButton::ButtonStyle::Solid : AdButton::ButtonStyle::Text;
            const auto expectedAccent =
                active ? AdButton::AccentRole::Primary : AdButton::AccentRole::Neutral;
            require(button->isChecked() == active && button->buttonStyle() == expectedStyle &&
                        button->accentRole() == expectedAccent,
                    "crop tools share the drawing toolbar's active and inactive appearance");
            require(!button->checkedUsesActiveStyle(),
                    "checked state does not replace the tool highlight with pressed styling");
        }
    };
    requireHighlight(nullptr);
    for (auto* selected : {rows, columns}) {
        for (int repeat = 0; repeat < 2; ++repeat) {
            selected->click();
            require(window.canvas()->mode() == (selected == rows
                                                    ? ImageEditingCanvas::Mode::CropRows
                                                    : ImageEditingCanvas::Mode::CropColumns),
                    "crop clicks preserve the selected editing mode");
            requireHighlight(selected);
        }
    }
    window.palette()->shapeRequested();
    require(window.canvas()->mode() == ImageEditingCanvas::Mode::Draw, "drawing exits crop mode");
    requireHighlight(nullptr);
    rows->click();
    window.palette()->moveRequested();
    require(window.canvas()->mode() == ImageEditingCanvas::Mode::Pan, "pan exits crop mode");
    requireHighlight(nullptr);
}
void verifyEditingToolbar(snow_shot::presentation::ImageEditingWindow& window) {
    verifyCropToolHighlight(window);
    using Tool = ScreenshotToolPalette::Tool;
    auto* screen = QGuiApplication::primaryScreen();
    require(screen && screen->handle(), "editing toolbar fixture has a screen");
    const auto originalBounds = screen->handle()->geometry();
    const auto originalAvailable = screen->handle()->availableGeometry();
    const auto restoreScreen = qScopeGuard([&] {
        QWindowSystemInterface::handleScreenGeometryChange(screen, originalBounds,
                                                           originalAvailable);
        QWindowSystemInterface::flushWindowSystemEvents();
        QApplication::processEvents();
    });
    const qreal dpr = screen->devicePixelRatio();
    const QRect nativeBounds(0, 0, qRound(1800 * dpr), qRound(1200 * dpr));
    QWindowSystemInterface::handleScreenGeometryChange(screen, nativeBounds, nativeBounds);
    QWindowSystemInterface::flushWindowSystemEvents();
    QApplication::processEvents();
    auto* editor = window.canvas()->window();
    auto* toolbar = qobject_cast<ScreenshotFloatingToolPaletteWindow*>(window.palette()->window());
    require(toolbar && toolbar != editor && toolbar->isWindow() &&
                toolbar->windowType() == Qt::Tool,
            "editing toolbar uses the shared floating palette window");
    require(!toolbar->parentWidget() && toolbar->isVisible(),
            "editing toolbar has an independent native surface");
    require(toolbar->windowHandle()->transientParent() == editor->windowHandle(),
            "native toolbar window follows the editor stacking order");
    const auto geometry = editor->geometry();
    const auto minimum = editor->minimumSize();
    const auto available = editor->screen()->geometry();
    editor->setMinimumSize(0, 0);
    editor->resize(available.width() - 24, available.height() / 3);
    editor->move(available.topLeft() + QPoint(12, 12));
    QApplication::processEvents();
    const auto mainRect = [&] {
        auto* panel = window.palette()->mainPanel();
        return QRect(panel->mapToGlobal(QPoint()), panel->size());
    };
    const auto occupiedRect = [&] {
        return toolbar->occupiedContentRect().translated(toolbar->contentPosition());
    };
    const auto requireAnchored = [&](bool above = false) {
        const auto frame = editor->frameGeometry();
        const QRect main = mainRect();
        require(main.right() == frame.right() + 1,
                "visible editing toolbar aligns with the window right edge like a pin toolbar");
        require(above ? main.bottom() == frame.top() - 5 : main.top() == frame.bottom() + 6,
                "editing toolbar uses the pin toolbar gap without shadow or scroll padding");
        require(available.contains(occupiedRect()), "all editing toolbar panels fit on screen");
        if (window.palette()->styleToolbarVisible()) {
            auto* style = window.palette()->stylePanel();
            const QRect settings(style->mapToGlobal(QPoint()), style->size());
            require(above ? settings.bottom() < main.top() : settings.top() > main.bottom(),
                    "editing sub-toolbars expand away from the editor window");
        }
    };
    requireAnchored();
    for (const bool resize : {false, true}) {
        window.palette()->setActiveTool(Tool::Shape);
        QApplication::processEvents();
        adqt::widgets::AdColorPicker* picker = nullptr;
        for (auto* candidate : window.palette()->findChildren<adqt::widgets::AdColorPicker*>())
            if (candidate->isVisible()) {
                picker = candidate;
                break;
            }
        require(picker, "moving and resizing fixture has a visible color picker");
        picker->setPopupVisible(true);
        require(picker->popupVisible(), "moving and resizing fixture opens the color popup");
        toolbar->moveContentTo(toolbar->contentPosition() + QPoint(0, 25));
        toolbar->dragFinished();
        sendWindowInteraction(window, true);
        require(!toolbar->isVisible(), "moving or resizing hides the editing toolbar immediately");
        require(!picker->popupVisible(), "moving or resizing dismisses the toolbar's color popup");
        const auto hiddenPosition = toolbar->pos();
        if (resize)
            editor->resize(editor->size() - QSize(10, 20));
        else
            editor->move(editor->pos() + QPoint(5, 15));
        QApplication::processEvents();
        require(!toolbar->isVisible() && toolbar->pos() == hiddenPosition,
                "window geometry updates defer toolbar placement until the interaction ends");
        window.palette()->setActiveTool(Tool::Text);
        QEvent dpiChange(QEvent::DevicePixelRatioChange);
        QApplication::sendEvent(window.canvas()->parentWidget(), &dpiChange);
        QApplication::processEvents();
        require(!toolbar->isVisible(),
                "layout and DPI updates keep the toolbar hidden during interaction");
        sendWindowInteraction(window, false);
        require(toolbar->isVisible(), "finishing the interaction restores the editing toolbar");
        requireAnchored();
        window.palette()->setActiveTool(Tool::Move);
        QApplication::processEvents();
        requireAnchored();
    }
    sendWindowInteraction(window, true);
    editor->hide();
    sendWindowInteraction(window, false);
    require(!toolbar->isVisible(),
            "ending an interaction on a hidden editor keeps its toolbar hidden");
    editor->show();
    QApplication::processEvents();
    require(toolbar->isVisible(),
            "showing the editor after an interrupted drag restores its toolbar");
    requireAnchored();
    if (!qEnvironmentVariableIsEmpty("SNOW_IMAGE_EDITING_TOOLBAR_SCREENSHOT")) {
        const QRect bounds = editor->geometry().united(toolbar->geometry());
        QPixmap preview(bounds.size());
        preview.fill(QColor(224, 228, 233));
        QPainter painter(&preview);
        painter.drawPixmap(editor->pos() - bounds.topLeft(), editor->grab());
        painter.drawPixmap(toolbar->pos() - bounds.topLeft(), toolbar->grab());
        painter.end();
        require(preview.save(qEnvironmentVariable("SNOW_IMAGE_EDITING_TOOLBAR_SCREENSHOT")),
                "save standalone editing toolbar visual inspection artifact");
    }
    editor->move(editor->pos() + QPoint(5, 15));
    QApplication::processEvents();
    requireAnchored();
    editor->resize(editor->size() - QSize(10, 20));
    QApplication::processEvents();
    requireAnchored();
    window.palette()->setActiveTool(Tool::Shape);
    QApplication::processEvents();
    requireAnchored();
    window.palette()->setActiveTool(Tool::Move);
    QApplication::processEvents();
    requireAnchored();
    editor->move(QPoint(available.left() + 12, available.bottom() - editor->height() - 10));
    QApplication::processEvents();
    requireAnchored(true);
    window.palette()->setActiveTool(Tool::Shape);
    QApplication::processEvents();
    requireAnchored(true);
    window.palette()->setActiveTool(Tool::Move);
    QApplication::processEvents();

    // The collapsed row fits below; opening its settings must reconsider placement
    // even though the shared native backing window retains exactly the same size.
    const auto collapsed = toolbar->placementSnapshot().bottom;
    const int depth =
        collapsed.occupiedContentRect.bottom() - collapsed.mainToolbarContentRect.top();
    const int desiredBottom = available.bottom() - depth - 6;
    editor->move(editor->pos() + QPoint(0, desiredBottom - editor->frameGeometry().bottom()));
    QApplication::processEvents();
    requireAnchored();
    const auto below = mainRect();
    const auto fixedFrame = toolbar->size();
    window.palette()->setActiveTool(Tool::Shape);
    QApplication::processEvents();
    requireAnchored(true);
    require(toolbar->size() == fixedFrame, "opening settings preserves the native toolbar frame");
    window.palette()->setActiveTool(Tool::Move);
    QApplication::processEvents();
    requireAnchored();
    require(mainRect() == below, "closing settings restores the preferred bottom placement");

    // Clamp visible panels at monitor edges, including settings wider than a compact main row.
    const auto drawing = snow_shot::storage::ScreenshotToolbarSettings().layout(
        snow_shot::storage::ScreenshotToolbarLayoutKind::DrawingTools);
    const auto actions = snow_shot::storage::ScreenshotToolbarSettings().layout(
        snow_shot::storage::ScreenshotToolbarLayoutKind::PinnedActionTools);
    auto hiddenDrawing = snow_shot::presentation::toolbar_layout::defaultOrder(
        snow_shot::storage::ScreenshotToolbarLayoutKind::DrawingTools);
    hiddenDrawing.removeAll(QStringLiteral("text"));
    window.palette()->setToolbarLayout({{{QStringLiteral("text")}}, hiddenDrawing});
    window.palette()->setActionToolsLayout(
        {{},
         snow_shot::presentation::toolbar_layout::defaultOrder(
             snow_shot::storage::ScreenshotToolbarLayoutKind::PinnedActionTools)});
    editor->resize(180, 300);
    editor->move(available.topLeft() + QPoint(12, 100));
    window.palette()->setActiveTool(Tool::Text);
    QApplication::processEvents();
    require(occupiedRect().width() > mainRect().width() &&
                occupiedRect().left() == available.left() && available.contains(occupiedRect()),
            "wider settings remain visible at the left edge of the monitor");
    editor->move(editor->pos() + QPoint(available.right() - editor->frameGeometry().right(), 0));
    QApplication::processEvents();
    require(occupiedRect().right() == available.right() && available.contains(occupiedRect()),
            "editing toolbar remains visible at the right edge of the monitor");
    window.palette()->setToolbarLayout(drawing);
    window.palette()->setActionToolsLayout(actions);
    window.palette()->setActiveTool(Tool::Move);
    editor->resize(available.width() - 24, available.height() / 3);
    editor->move(available.topLeft() + QPoint(12, 12));
    QApplication::processEvents();

    const auto normalWidth = mainRect().width();
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    const auto originalSize = snow_shot::storage::ScreenshotUiSettings().toolbarSize();
    configuration.setValue(QStringLiteral("screenshot_ui/toolbar_size"), QStringLiteral("small"));
    QApplication::processEvents();
    require(mainRect().width() < normalWidth, "editing toolbar applies the shared size preference");
    requireAnchored();
    configuration.setValue(QStringLiteral("screenshot_ui/toolbar_size"), originalSize);
    QApplication::processEvents();
    requireAnchored();
    editor->hide();
    QApplication::processEvents();
    require(!toolbar->isVisible(), "hiding the editor hides its standalone toolbar");
    editor->show();
    QApplication::processEvents();
    require(toolbar->isVisible(), "showing the editor restores its standalone toolbar");
    editor->move(available.topLeft() + QPoint(12, 12));
    QApplication::processEvents();
    const auto beforeDrag = toolbar->pos();
    toolbar->moveContentTo(toolbar->contentPosition() + QPoint(0, 25));
    toolbar->dragFinished();
    require(toolbar->pos() == beforeDrag + QPoint(0, 25),
            "manual placement uses the shared toolbar's global content coordinates");
    const auto dragged = toolbar->pos();
    editor->move(editor->pos() + QPoint(0, 10));
    QApplication::processEvents();
    require(toolbar->pos() == dragged + QPoint(0, 10),
            "dragged toolbar retains its offset when the editor moves");
    const auto manualMainPosition = mainRect().topLeft();
    for (const auto tool : {Tool::Shape, Tool::Text, Tool::Move}) {
        window.palette()->setActiveTool(tool);
        QApplication::processEvents();
        require(mainRect().topLeft() == manualMainPosition,
                "opening and replacing settings preserve the manually dragged main row");
    }
    require(window.document()->crop(snow_shot::presentation::ImageEditingDocument::CropAxis::Rows,
                                    1, 2),
            "standalone toolbar shortcut fixture");
    QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    QApplication::sendEvent(toolbar, &undo);
    awaitCondition([&] { return !window.dirty(); },
                   "keyboard undo works with the standalone toolbar focused");
    editor->setMinimumSize(minimum);
    editor->setGeometry(geometry);
    QApplication::processEvents();
}
void verifyRecognitionHistoryShortcuts() {
    using namespace snow_shot::presentation;
    auto recognized = source();
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = recognized.canvasRect.toRect();
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Original text");
    line.quad = {QPointF(-20, 15), QPointF(-15, 15), QPointF(-15, 17), QPointF(-20, 17)};
    presentation->lines.append(line);
    presentation->prepareForRendering();
    recognized.recognition.key = QStringLiteral("editor-history");
    recognized.recognition.text = ScreenshotOcrRecognitionResult{presentation};
    for (const bool translation : {false, true}) {
#if !SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        if (translation)
            continue;
#endif
        ImageEditingWindow window;
        window.present(recognized);
        window.palette()->ocrRequested();
        auto* session = window.recognition();
        require(session && session->editing(), "image editor opens editable cached OCR");
        if (translation) {
            window.palette()->textTranslationRequested();
            session->setTextDraft(QStringLiteral("Translation"));
        }
        auto* text = window.canvas()->parentWidget()->findChild<QTextEdit*>(
            QStringLiteral("screenshotOcrEditor"));
        require(text && text->isVisible() && !text->isReadOnly(),
                "image editor exposes the active recognition result");
        const QString baseline = session->textDraft();
        auto cursor = text->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(QStringLiteral(" edited"));
        const QString edited = baseline + QStringLiteral(" edited");
        window.canvas()->window()->activateWindow();
        text->setFocus();
        QApplication::processEvents();
        require(text->hasFocus() && session->textDraft() == edited,
                "typed result receives image editor keyboard commands");
        const auto shortcut = [&](QKeySequence::StandardKey command) {
            const auto combination = QKeySequence::keyBindings(command).first()[0];
            for (const auto type :
                 {QEvent::ShortcutOverride, QEvent::KeyPress, QEvent::KeyRelease}) {
                PhysicalKeyEvent event(type, combination.key(), combination.keyboardModifiers());
                QApplication::sendEvent(text, &event);
            }
        };
        shortcut(QKeySequence::Undo);
        require(session->textDraft() == baseline,
                "keyboard Undo reaches the image editor result history");
        shortcut(QKeySequence::Redo);
        require(session->textDraft() == edited,
                "keyboard Redo reaches the image editor result history");
        window.shutdown();
    }
}
} // namespace
void runImageEditingEntryPointTests() {
    const int font = QFontDatabase::addApplicationFont(
        QStringLiteral(":/recording-test-fonts/SnowRecordingTestSans-Regular.ttf"));
    require(font >= 0, "editor entry fixtures load the bundled font");
    QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(), 12));
    verifySourceLoading(source());
    verifyScrollingSourceImport();
}
void runImageEditingLifetimeTests();
void runImageEditingCloseTests();
void runImageEditingSourceTests();
void runImageEditingSmartEraseTests();
void runImageEditingSaveTests();
void runImageEditingPinTests();
void runImageEditingSpreadsheetOpenTests();
void runImageEditingConversionAvailabilityTests();
void runImageEditingCanvasTests();
void runImageEditingTextPreferencesTests();
void runImageEditingDocumentTests();
void runImageEditingTranslationTests();
void runImageEditingBranchTests();
void runImageEditingTests() {
    using namespace snow_shot::presentation;
    using Axis = ImageEditingDocument::CropAxis;
    const int font = QFontDatabase::addApplicationFont(
        QStringLiteral(":/recording-test-fonts/SnowRecordingTestSans-Regular.ttf"));
    require(font >= 0, "editor fixtures load the bundled font");
    QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(), 12));
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-pin-only"))) {
        runImageEditingPinTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-history-only"))) {
        verifyRecognitionHistoryShortcuts();
        return;
    }
    if (QApplication::arguments().contains(
            QStringLiteral("--image-editing-automatic-selection-only"))) {
        canvas_automatic_selection_test::exercise([](auto verify) {
            ImageEditingWindow window;
            window.present(source());
            window.canvas()->setMode(ImageEditingCanvas::Mode::Draw);
            verify(*window.canvas());
            require(snow_shot::storage::DrawingSettings().setAutomaticSelectionExcludedTools({}),
                    "enable automatic selection for every image-editor drawing tool");
            // Reload without another settings notification to exercise retained runtime policy.
            window.present(source());
            window.canvas()->setMode(ImageEditingCanvas::Mode::Draw);
            auto& canvas = *window.canvas();
            require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate shape after reload");
            canvas_automatic_selection_test::mouse(canvas, QEvent::MouseButtonPress, {60, 60},
                                                   Qt::LeftButton, Qt::LeftButton);
            canvas_automatic_selection_test::mouse(canvas, QEvent::MouseMove, {160, 120},
                                                   Qt::NoButton, Qt::LeftButton);
            canvas_automatic_selection_test::mouse(canvas, QEvent::MouseButtonRelease, {160, 120},
                                                   Qt::LeftButton, Qt::NoButton);
            require(window.document()->runtime().selectedElementIds().size() == 1,
                    "document reload preserves the image-editor automatic selection preference");
            require(canvas.undo(), "remove the reloaded document's test shape");
            window.shutdown();
        });
        return;
    }
    if (QApplication::arguments().contains(
            QStringLiteral("--image-editing-text-preferences-only"))) {
        runImageEditingTextPreferencesTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-translation-only"))) {
        runImageEditingTranslationTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-branch-only"))) {
        runImageEditingBranchTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-canvas-only"))) {
        runImageEditingCanvasTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-document-only"))) {
        runImageEditingDocumentTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-save-only"))) {
        runImageEditingSaveTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--spreadsheet-open-only"))) {
        runImageEditingSpreadsheetOpenTests();
        return;
    }
    if (QApplication::arguments().contains(
            QStringLiteral("--image-editing-conversion-availability-only"))) {
        runImageEditingConversionAvailabilityTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-close-only"))) {
        runImageEditingCloseTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-source-only"))) {
        runImageEditingSourceTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-smart-erase-only"))) {
        runImageEditingSmartEraseTests();
        return;
    }
    if (QApplication::arguments().contains(QStringLiteral("--image-editing-lifetime-only"))) {
        runImageEditingLifetimeTests();
        return;
    }
    if (QApplication::arguments().contains(
            QStringLiteral("--image-editing-recognition-feedback-only"))) {
        verifyRecognitionFeedback(source());
        return;
    }
    const bool nativeToolbar =
        QApplication::arguments().contains(QStringLiteral("--image-editing-toolbar-native-only"));
    if (nativeToolbar ||
        QApplication::arguments().contains(QStringLiteral("--image-editing-toolbar-only"))) {
        // Match main.cpp: isolating alien siblings exposes native-child destruction
        // when the modal reparents its preview during initial presentation.
        const auto nativeSiblings = Qt::AA_DontCreateNativeWidgetSiblings;
        const bool previousNativeSiblings = QCoreApplication::testAttribute(nativeSiblings);
        QCoreApplication::setAttribute(nativeSiblings);
        const auto restoreNativeSiblings = qScopeGuard(
            [&] { QCoreApplication::setAttribute(nativeSiblings, previousNativeSiblings); });
        const auto requireEditorOpen = [](const ImageEditingWindow& editor) {
            auto* surface = editor.canvas()->window();
            require(surface->isVisible() && surface->windowHandle() &&
                        surface->windowHandle()->isVisible(),
                    "the image editor surface opens with its floating toolbar");
#if defined(Q_OS_WIN)
            if (QGuiApplication::platformName() == QStringLiteral("windows")) {
                const auto hwnd = reinterpret_cast<HWND>(surface->winId());
                require(IsWindow(hwnd) && IsWindowVisible(hwnd),
                        "the native image editor surface is visible when its toolbar opens");
            }
#endif
            auto* toolbar = editor.palette()->window();
            require(toolbar->isVisible() && editor.palette()->mainPanel()->isVisible() &&
                        toolbar->windowHandle() && toolbar->windowHandle()->isVisible(),
                    "main editing toolbar opens immediately without a resize or user interaction");
        };
        {
            ImageEditingWindow loading;
            std::function<void(QImage)> deliver;
            bool loaded = false;
            QObject::connect(&loading, &ImageEditingWindow::loaded, &loading,
                             [&](bool success) { loaded = success; });
            loading.presentSource(ScreenshotExportSource::fromImageLoader(
                [&](QObject*, std::function<void(QImage)> callback) {
                    deliver = std::move(callback);
                    return true;
                }));
            requireEditorOpen(loading);
            awaitCondition([&] { return bool(deliver); }, "fresh editor starts loading");
            requireEditorOpen(loading);
            auto* message = statusMessage(loading);
            require(message, "fresh editor shows loading feedback");
            QPointer<QWidget> loadingNotice = message->noticeWidget();
            std::exchange(deliver, {})(source().image);
            awaitCondition([&] { return loaded; }, "fresh editor finishes loading");
            requireEditorOpen(loading);
            awaitCondition([&] { return !loadingNotice; }, "loading feedback finishes closing");
            requireEditorOpen(loading);
            loading.shutdown();
        }
        ImageEditingWindow window;
        window.present(source());
        requireEditorOpen(window);
        QApplication::processEvents();
        if (nativeToolbar) {
            std::function<void(QImage)> deliver;
            bool loaded = false;
            QObject::connect(&window, &ImageEditingWindow::loaded, &window,
                             [&](bool success) { loaded = success; });
            window.presentSource(ScreenshotExportSource::fromImageLoader(
                [&](QObject*, std::function<void(QImage)> callback) {
                    deliver = std::move(callback);
                    return true;
                }));
            awaitCondition([&] { return bool(deliver); }, "native source starts loading");
            std::exchange(deliver, {})(source().image);
            awaitCondition([&] { return loaded; }, "native source finishes loading");
            QApplication::processEvents();
#if defined(Q_OS_WIN)
            auto* toolbar = window.palette()->window();
            const auto hwnd = reinterpret_cast<HWND>(toolbar->winId());
            require(IsWindow(hwnd) && IsWindowVisible(hwnd),
                    "the native editing toolbar survives presentation and remains visible");
            require(GetWindow(hwnd, GW_OWNER) ==
                        reinterpret_cast<HWND>(window.canvas()->window()->winId()),
                    "the native editing toolbar belongs to the visible editor");
            auto* editor = window.canvas()->window();
            const auto editorHwnd = reinterpret_cast<HWND>(editor->winId());
            for (const bool resize : {false, true}) {
                SendMessageW(editorHwnd, WM_ENTERSIZEMOVE, 0, 0);
                require(!toolbar->isVisible() && !IsWindowVisible(hwnd),
                        "native move/resize entry hides the editing toolbar");
                if (resize)
                    editor->resize(editor->size() - QSize(10, 20));
                else
                    editor->move(editor->pos() + QPoint(10, 10));
                window.palette()->setActiveTool(resize ? ScreenshotToolPalette::Tool::Text
                                                       : ScreenshotToolPalette::Tool::Shape);
                QApplication::processEvents();
                require(!toolbar->isVisible() && !IsWindowVisible(hwnd),
                        "native geometry and layout updates cannot reveal the toolbar mid-drag");
                SendMessageW(editorHwnd, WM_EXITSIZEMOVE, 0, 0);
                require(toolbar->isVisible() && IsWindowVisible(hwnd),
                        "native move/resize exit restores the editing toolbar");
            }
#endif
            requireEditorOpen(window);
            window.shutdown();
            return;
        }
        verifyEditingToolbar(window);
        sendWindowInteraction(window, true);
        window.shutdown();
        sendWindowInteraction(window, false);
        require(!window.palette()->window()->isVisible(),
                "finishing an interaction cannot restore a closed editor's toolbar");

        QPointer<QWidget> loadingToolbar;
        {
            ImageEditingWindow loading;
            loading.presentSource(ScreenshotExportSource::fromImageLoader(
                [](QObject*, std::function<void(QImage)>) { return true; }));
            loadingToolbar = loading.palette()->window();
            requireEditorOpen(loading);
            loading.shutdown();
        }
        require(!loadingToolbar, "closing the editor releases its independent toolbar");
        return;
    }
    verifyLinearPreviewScaling();
    verifyCropMeasurementColor();
    if (QApplication::arguments().contains(QStringLiteral("--crop-measurement-color-only")))
        return;
    const auto original = source();
    verifyCanvasToolbar(original);
    verifyLoadingFeedback(original);
    verifyRecognitionFeedback(original);
    verifySourceLoading(original);
    verifyScrollingSourceImport();
    ImageEditingDocument document;
    require(document.load(original), "load independent document");
    require(!document.dirty(), "initial document is saved");
    const auto initial = document.checkpoint();
    require(!document.crop(Axis::Rows, 2, 2), "empty range rejected");
    require(!document.crop(Axis::Rows, -100, 100), "whole image rejected");
    require(document.checkpoint() == initial, "invalid crop leaves history unchanged");
    require(document.crop(Axis::Rows, 6, 2), "reversed row crop");
    require(document.image().size() == QSize(12, 6), "remove exact row extent");
    require(document.bounds() == QRectF(-20, 15, 6, 3), "origin and pixel mapping unchanged");
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 12; ++x)
            require(document.image().pixel(x, y) == original.image.pixel(x, y < 2 ? y : y + 4),
                    "exact crop colors and alpha");
    require(document.dirty(), "crop dirty");
    const auto cropped = document.checkpoint();
    document.markExported(cropped);
    require(!document.dirty(), "successful export saves that checkpoint");
    require(document.crop(Axis::Columns, 3, 8), "column crop");
    require(document.image().size() == QSize(7, 6), "column dimensions");
    document.markExported(cropped);
    require(document.dirty(), "late export does not save later edits");
    require(document.undo() && document.checkpoint() == cropped,
            "column undo restores saved state");
    require(document.undo() && document.image() == original.image, "row undo restores exact image");
    require(document.checkpoint() == initial, "history counters do not mark content dirty");
    require(document.redo() && document.redo(), "crop redo chain");
    require(document.undo() && document.crop(Axis::Columns, 1, 2), "new branch");
    require(!document.redo(), "abandoned redo removed");
    require(original.image.size() == QSize(12, 10), "source remains unchanged");

    {
        auto transparent = original;
        transparent.image.fill(Qt::transparent);
        ImageEditingWindow preview;
        preview.present(transparent);
        QApplication::processEvents();
        auto* surface = preview.canvas()->parentWidget();
        const QPoint probe = preview.canvas()->rect().center();
        const auto previewPixel = [&](QPoint position) {
            const auto pixels = surface->grab().toImage();
            return pixels.pixelColor((QPointF(position) * pixels.devicePixelRatio()).toPoint());
        };
        const auto first = previewPixel(probe), second = previewPixel(probe + QPoint(10, 0));
        require(first != second && first.alpha() == 255 && second.alpha() == 255,
                "transparent pixels show the preview checkerboard");
        const auto requireCheckerboardAtEdges = [&] {
            const auto pixels = surface->grab().toImage();
            const auto colorAt = [&](QPoint position) {
                return pixels.pixelColor((QPointF(position) * pixels.devicePixelRatio()).toPoint());
            };
            for (const QPoint edge :
                 {QPoint(0, 0), QPoint(surface->width() - 11, 0), QPoint(0, surface->height() - 1),
                  QPoint(surface->width() - 11, surface->height() - 1)}) {
                const auto color = colorAt(edge);
                const auto alternate = colorAt(edge + QPoint(10, 0));
                require(color != alternate && color.alpha() == 255 && alternate.alpha() == 255,
                        "checkerboard covers every preview edge without a gray surround");
                require(color != colorAt(edge + QPoint(0, edge.y() == 0 ? 10 : -10)),
                        "preview edge checkerboard alternates vertically");
            }
        };
        requireCheckerboardAtEdges();
        preview.canvas()->actualSize();
        mouse(preview.canvas(), QEvent::MouseButtonPress, {100, 100}, Qt::MiddleButton,
              Qt::MiddleButton);
        mouse(preview.canvas(), QEvent::MouseMove, {132, 116}, Qt::NoButton, Qt::MiddleButton);
        mouse(preview.canvas(), QEvent::MouseButtonRelease, {132, 116}, Qt::MiddleButton,
              Qt::NoButton);
        requireCheckerboardAtEdges();
        preview.canvas()->window()->resize(900, 600);
        QApplication::processEvents();
        requireCheckerboardAtEdges();
        auto& theme = styles::ThemeManager::instance();
        theme.setThemeAppearance(styles::ThemeAppearance::Dark);
        QApplication::processEvents();
        require(previewPixel(probe).lightness() < first.lightness(),
                "checkerboard adapts to runtime dark theme");
        requireCheckerboardAtEdges();
        theme.setThemeAppearance(styles::ThemeAppearance::Light);
        QApplication::processEvents();
        const auto snapshot = preview.document()->snapshot();
        require(snapshot.image.pixelColor(0, 0).alpha() == 0,
                "checkerboard never enters the document image");
        preview.shutdown();
    }
    ImageEditingWindow window;
    window.present(original);
    QApplication::processEvents();
    require(window.modal()->mode() == adqt::widgets::AdModal::Mode::Window,
            "detached Modal window");
    require(window.modal()->windowModality() == Qt::NonModal, "main window remains interactive");
    require(window.modal()->windowAlwaysOnTop(), "always on top initially");
    require(!window.modal()->footerVisible(), "no footer");
    auto* titleIcon =
        window.canvas()->window()->findChild<QLabel*>(QStringLiteral("ad-modal-window-icon"));
    require(window.modal()->centered() && !window.modal()->windowIcon().isNull() && titleIcon &&
                titleIcon->isVisible() && !titleIcon->pixmap().isNull(),
            "editor uses the same centered window chrome and application icon as splitting");
    const auto requireWindowControls = [&] {
        for (const auto* name : {"ad-modal-minimize", "ad-modal-maximize", "ad-modal-always-on-top",
                                 "ad-modal-close"}) {
            auto* button =
                window.canvas()->window()->findChild<QWidget*>(QString::fromLatin1(name));
            require(button && button->isVisible(), "all editor title-bar controls remain visible");
        }
    };
    requireWindowControls();
    require(!window.dirty(), "opening and fitting are not content changes");
    verifyEditingToolbar(window);
    auto* floating = window.palette()->window();
    auto* rowButton =
        floating->findChild<adqt::widgets::AdButton*>(QStringLiteral("imageEditingCropRows"));
    auto* columnButton =
        floating->findChild<adqt::widgets::AdButton*>(QStringLiteral("imageEditingCropColumns"));
    auto* mainPanel = window.palette()->mainPanel();
    const auto verifyCropButtons = [&] {
        adqt::widgets::AdButton* tableButton = nullptr;
        for (auto* button : mainPanel->findChildren<adqt::widgets::AdButton*>()) {
            if (button->isVisible() && button->property("screenshotToolbarPositionItems")
                                           .toStringList()
                                           .contains(QStringLiteral("table-recognition"))) {
                tableButton = button;
                break;
            }
        }
        require(rowButton && columnButton && tableButton,
                "editor crop and recognition buttons exist");
        require(rowButton->parentWidget() == mainPanel &&
                    columnButton->parentWidget() == mainPanel && rowButton->isVisible() &&
                    columnButton->isVisible(),
                "crop actions live in the main toolbar");
        require(rowButton->text().isEmpty() && columnButton->text().isEmpty() &&
                    rowButton->iconRef().isValid() && columnButton->iconRef().isValid(),
                "crop actions use icons without text");
        require(rowButton->size() == tableButton->size() &&
                    columnButton->size() == tableButton->size(),
                "crop buttons match the toolbar metrics");
        const QRect rowGeometry(rowButton->mapTo(mainPanel, QPoint{}), rowButton->size());
        const QRect columnGeometry(columnButton->mapTo(mainPanel, QPoint{}), columnButton->size());
        const QRect tableGeometry(tableButton->mapTo(mainPanel, QPoint{}), tableButton->size());
        require(rowGeometry.right() < columnGeometry.left() &&
                    columnGeometry.right() < tableGeometry.left() &&
                    rowGeometry.y() == tableGeometry.y() && columnGeometry.y() == tableGeometry.y(),
                "vertical then horizontal crop precede table recognition on the same row");
    };
    verifyCropButtons();
    const auto savedActionLayout = snow_shot::storage::ScreenshotToolbarSettings().layout(
        snow_shot::storage::ScreenshotToolbarLayoutKind::PinnedActionTools);
    auto reorderedActions = savedActionLayout;
    std::reverse(reorderedActions.positions.begin(), reorderedActions.positions.end());
    window.palette()->setActionToolsLayout(reorderedActions);
    QApplication::processEvents();
    verifyCropButtons();
    window.palette()->setActionToolsLayout(savedActionLayout);
    QApplication::processEvents();
    verifyCropButtons();
    rowButton->click();
    require(window.canvas()->mode() == ImageEditingCanvas::Mode::CropRows &&
                rowButton->isChecked() && !columnButton->isChecked(),
            "vertical crop selects rows");
    columnButton->click();
    require(window.canvas()->mode() == ImageEditingCanvas::Mode::CropColumns &&
                columnButton->isChecked() && !rowButton->isChecked(),
            "horizontal crop selects columns");
    window.palette()->moveRequested();
    require(window.canvas()->mode() == ImageEditingCanvas::Mode::Pan && !rowButton->isChecked() &&
                !columnButton->isChecked(),
            "pan clears crop selection");
    const QSize buttonSize = rowButton->size();
    const auto unzoomed = window.document()->checkpoint();
    const QPointF anchor(250, 200);
    const auto anchorCanvas = window.canvas()->canvasToViewTransform().inverted().map(anchor);
    const auto fittedZoom = window.canvas()->zoom();
    QWheelEvent zoomEvent(anchor, window.canvas()->mapToGlobal(anchor.toPoint()), {},
                          QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(window.canvas(), &zoomEvent);
    require(window.canvas()->zoom() > fittedZoom,
            "zooming in never jumps out from a fitted small image");
    require(QLineF(anchorCanvas, window.canvas()->canvasToViewTransform().inverted().map(anchor))
                    .length() < 1e-4,
            "zoom anchors the cursor");
    require(rowButton->size() == buttonSize && window.document()->checkpoint() == unzoomed,
            "zoom changes neither toolbar scale nor image content");
    const auto panStart = window.canvas()->center();
    mouse(window.canvas(), QEvent::MouseButtonPress, QPointF(200, 200), Qt::MiddleButton,
          Qt::MiddleButton);
    mouse(window.canvas(), QEvent::MouseMove, QPointF(232, 216), Qt::NoButton, Qt::MiddleButton);
    mouse(window.canvas(), QEvent::MouseButtonRelease, QPointF(232, 216), Qt::MiddleButton,
          Qt::NoButton);
    require(QLineF(window.canvas()->center(), panStart - QPointF(32, 16) / window.canvas()->zoom())
                    .length() < 1e-6,
            "middle-button pans in drawing coordinates");
    const auto cameraBeforeFlags = window.canvas()->center();
    const auto scaleBeforeFlags = window.canvas()->zoom();
    auto* originalCanvas = window.canvas();
    window.modal()->setWindowAlwaysOnTop(false);
    QApplication::processEvents();
    window.modal()->setWindowAlwaysOnTop(true);
    QApplication::processEvents();
    window.canvas()->window()->showMaximized();
    QApplication::processEvents();
    window.canvas()->window()->showMinimized();
    QApplication::processEvents();
    require(!floating->isVisible(), "minimizing the editor hides its standalone toolbar");
    window.canvas()->window()->showNormal();
    QApplication::processEvents();
    require(floating->isVisible(), "restoring the editor restores its standalone toolbar");
    require(window.canvas() == originalCanvas && window.canvas()->center() == cameraBeforeFlags &&
                window.canvas()->zoom() == scaleBeforeFlags,
            "window controls preserve document widgets and drawing camera");
    requireWindowControls();
    EditorTranslator translator;
    QApplication::installTranslator(&translator);
    QApplication::processEvents();
    require(rowButton->accessibleName() == QStringLiteral("[translated] Vertical Crop") &&
                rowButton->toolTip() == rowButton->accessibleName() && rowButton->text().isEmpty(),
            "open editor retranslates at runtime");
    QApplication::removeTranslator(&translator);
    QApplication::processEvents();
    require(rowButton->accessibleName() == QStringLiteral("Vertical Crop"),
            "runtime language restored");
    rowButton->click();
    const auto map = window.canvas()->canvasToViewTransform();
    const QPointF start = map.map(QPointF(-18, 16.1));
    const QPointF end = map.map(QPointF(-18, 17.9));
    mouse(window.canvas(), QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(window.canvas(), QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    require(window.document()->image() == original.image,
            "crop drag preview does not mutate pixels");
    mouse(window.canvas(), QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    awaitCondition([&] { return window.document()->image().height() == 6; },
                   "crop maps zoomed and panned drag to rounded source pixels");
    require(window.document()->undo() && window.document()->image() == original.image,
            "gesture crop undoes without moving annotations");
    const auto cancelledCrop =
        ImageEditingDocument::prepareCrop(original.image, Axis::Rows, 1, 5, [] { return true; });
    require(!window.document()->applyCrop(cancelledCrop) && !window.dirty(),
            "cancelled crop preserves pixels history and checkpoint");
    window.canvas()->fit();
    if (!qEnvironmentVariableIsEmpty("SNOW_IMAGE_EDITING_SCREENSHOT")) {
        QApplication::setFont(QFont(QStringLiteral("Segoe UI"), 12));
        QApplication::processEvents();
        require(window.canvas()->window()->grab().save(
                    qEnvironmentVariable("SNOW_IMAGE_EDITING_SCREENSHOT")),
                "save editor visual inspection artifact");
    }
    const auto center = window.canvas()->center();
    const auto zoom = window.canvas()->zoom();
    window.canvas()->resize(930, 650);
    require(window.canvas()->center() == center && window.canvas()->zoom() == zoom,
            "resize preserves camera");
    require(window.document()->crop(Axis::Rows, 2, 5), "window crop");
    QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    QApplication::sendEvent(window.canvas(), &undo);
    awaitCondition([&] { return window.document()->image() == original.image; },
                   "keyboard undo uses document crop history");
    bool decision = true;
    require(window.document()->crop(Axis::Columns, 1, 4), "dirty close fixture");
    floating->close();
    auto dialogs = window.findChildren<adqt::widgets::AdModal*>();
    require(dialogs.size() == 2, "unsaved close opens confirmation");
    for (auto* dialog : dialogs)
        if (dialog != window.modal()) {
            require(dialog->rejectButton()->isDefault() && !dialog->acceptButton()->isDefault(),
                    "Continue Editing is the sole default close decision");
            QObject::connect(dialog, &adqt::widgets::AdModal::rejected, &window,
                             [&] { decision = false; });
            dialog->reject();
        }
    awaitCondition([&] { return !decision; }, "continue editing cancels close");
    require(window.dirty() && window.modal()->isOpen(), "continue retains window and content");
    window.shutdown();
    require(!floating->isVisible(), "closing the editor hides its standalone toolbar");

    ImageEditingDocument mixed;
    auto editable = source();
    SnowCanvasRuntime saved;
    require(
        !saved
             .applyAnnotationTransaction(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[-19,18,3,1],"style":{"fill":[255,0,0,255],"stroke":[0,0,0,0]}}]})")
             .isEmpty(),
        "editable annotation fixture");
    editable.canvasHistory = saved.serializeDocumentHistory();
    require(mixed.load(editable) && mixed.runtime().canUndo(), "import annotation history");
    const auto annotations = annotationData(mixed.runtime());
    require(mixed.crop(Axis::Rows, 2, 8), "crop clips annotation below image");
    require(annotationData(mixed.runtime()) == annotations, "crop preserves all annotation data");
    auto snap = mixed.snapshot();
    auto artifact = std::make_shared<
        ScreenshotExportArtifact>(ScreenshotExportSource::fromPinnedViewport(
        {snap.canvasSession, snap.image, snap.canvasRect, snap.image.size(), {}, snap.smartErase}));
    QImage output;
    require(artifact->requestImage(&window, [&](auto result) { output = result.image; }),
            "export starts");
    awaitCondition([&] { return !output.isNull(); }, "export finishes");
    require(output.size() == QSize(12, 4), "export uses image pixels, not viewport");
    require(output.pixelColor(5, 2).alpha() < 255,
            "clipped annotation and checkerboard absent from export");
    require(mixed.undo() && mixed.image() == editable.image, "crop undo reveals old bounds");
    require(mixed.undo(), "imported drawing undo follows crop");
    require(mixed.redo() && mixed.redo(), "mixed redo order preserved");
    require(annotationData(mixed.runtime()) == annotations, "redo preserves annotations");

    auto recognized = source();
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = recognized.canvasRect.toRect();
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Original text");
    line.confidence = 1;
    line.quad = QPolygonF{QPointF(-20, 15), QPointF(-15, 15), QPointF(-15, 17), QPointF(-20, 17)};
    presentation->lines.append(line);
    presentation->prepareForRendering();
    recognized.recognition.key = QStringLiteral("original-record");
    recognized.recognition.text = ScreenshotOcrRecognitionResult{presentation};
    ImageEditingWindow ocr;
    ocr.present(recognized);
    ocr.palette()->ocrRequested();
    require(ocr.recognition() && ocr.recognition()->editing(),
            "cached OCR enters editing directly");
    ocr.recognition()->setShowOriginalImage(true);
    require(!ocr.recognition()->showOriginalImage(), "original image mode remains disabled");
    const auto camera = ocr.canvas()->center();
    const auto cameraZoom = ocr.canvas()->zoom();
    ocr.recognition()->setTextDraft(QStringLiteral("Unsaved draft"));
    require(ocr.dirty(), "text edits are unsaved");
    ocr.palette()->moveRequested();
    require(ocr.dirty(), "leaving recognition retains unsaved drafts");
    require(ocr.canvas()->center() == camera && ocr.canvas()->zoom() == cameraZoom,
            "drawing camera restored");
    ocr.palette()->ocrRequested();
    require(ocr.recognition()->textDraft() == QStringLiteral("Unsaved draft"),
            "recognition draft survives mode switches");
    bool copied = false;
    QObject::connect(&ocr, &ImageEditingWindow::exportFinished, &ocr,
                     [&](bool success) { copied = success; });
    ocr.palette()->copyRequested();
    require(copied && QApplication::clipboard()->text() == QStringLiteral("Unsaved draft"),
            "OCR copy exports text");
    require(!ocr.dirty() && ocr.modal()->isOpen(),
            "successful text export saves content and keeps window open");
    ocr.palette()->moveRequested();
    require(ocr.document()->crop(Axis::Rows, 1, 3), "crop invalidates recognition input");
    ocr.palette()->ocrRequested();
    require(!ocr.recognition()->hasTextResult(),
            "old cached recognition not applied to cropped base");
    ocr.palette()->moveRequested();
    require(ocr.document()->undo(), "restore recognized base");
    ocr.palette()->ocrRequested();
    require(ocr.recognition()->textDraft() == QStringLiteral("Unsaved draft"),
            "matching cache and draft reused after undo");
    bool cachedReadyInEditMode = false;
    const auto cachedConnection = QObject::connect(
        ocr.recognition(), &ScreenshotRecognitionSessionController::textResultChanged, &ocr,
        [&](bool ready) {
            if (ready)
                cachedReadyInEditMode = ocr.recognition()->editing();
        });
    ocr.palette()->moveRequested();
    ocr.palette()->ocrRequested();
    require(cachedReadyInEditMode,
            "cached OCR establishes editing before notifying result consumers");
    QObject::disconnect(cachedConnection);
    auto* recognitionView = ocr.canvas()->parentWidget()->findChild<ScreenshotRecognitionWindow*>();
    auto* textEditor = recognitionView ? recognitionView->findChild<QTextEdit*>() : nullptr;
    require(textEditor && ocr.canvas()->isHidden() &&
                recognitionView->geometry() == ocr.canvas()->parentWidget()->rect(),
            "recognition fills the preview at UI scale");
    const auto textFont = textEditor->font();
    QWheelEvent textWheel(QPointF(20, 20), textEditor->mapToGlobal(QPoint(20, 20)), {},
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(textEditor->viewport(), &textWheel);
    require(textEditor->font() == textFont && ocr.canvas()->zoom() == cameraZoom,
            "recognition controls own wheel input and do not change the drawing camera");
    QTemporaryDir exportDirectory;
    const snow_shot::storage::ScreenshotSettings exportSettings;
    const QString oldDirectory = exportSettings.imageSaveDirectory();
    require(exportSettings.setImageSaveDirectory(exportDirectory.path()),
            "configure isolated Quick Save directory");
    int fileExports = 0;
    bool fileSucceeded = false;
    QObject::connect(&ocr, &ImageEditingWindow::exportFinished, &ocr, [&](bool success) {
        ++fileExports;
        fileSucceeded = success;
    });
    ocr.recognition()->setTextDraft(QStringLiteral("Quick Save text"));
    ocr.palette()->quickSaveRequested();
    awaitCondition([&] { return fileExports == 1; }, "text Quick Save completes");
    const auto textFiles =
        QDir(exportDirectory.path()).entryList({QStringLiteral("*.txt")}, QDir::Files);
    require(fileSucceeded && textFiles.size() == 1 && !ocr.dirty() && ocr.modal()->isOpen(),
            "Quick Save uses configured directory, saves the text checkpoint, and stays open");
    QFile savedText(QDir(exportDirectory.path()).filePath(textFiles.first()));
    require(savedText.open(QIODevice::ReadOnly) &&
                savedText.readAll() == QByteArrayLiteral("Quick Save text"),
            "text Quick Save preserves exact recognition content");
    savedText.close();
    require(exportSettings.setImageSaveDirectory(savedText.fileName()),
            "configure failure fixture");
    ocr.recognition()->setTextDraft(QStringLiteral("Still unsaved"));
    ocr.palette()->quickSaveRequested();
    awaitCondition([&] { return fileExports == 2; }, "failed Quick Save completes");
    require(!fileSucceeded && ocr.dirty() &&
                ocr.recognition()->textDraft() == QStringLiteral("Still unsaved"),
            "failed export preserves the editor and its unsaved content");
    static_cast<void>(exportSettings.setImageSaveDirectory(oldDirectory));
    const bool previousNativeDialogs = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QTimer::singleShot(0, &ocr, [&] {
        QPointer<QFileDialog> dialog = ocr.canvas()->window()->findChild<QFileDialog*>();
        require(dialog, "recognition Save As opens a result dialog");
        ocr.shutdown();
        if (dialog)
            dialog->reject();
    });
    ocr.palette()->saveRequested();
    awaitCondition([&] { return !ocr.modal()->isOpen(); },
                   "recognition Save As remains nonblocking and can be closed");
    require(fileExports == 2, "closing during recognition Save As prevents a late export callback");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previousNativeDialogs);
    DeferredOcr provider;
    ImageEditingWindow fresh(
        {[&] { return ScreenshotPinnedRecognitionProviders{&provider, nullptr, nullptr}; }});
    fresh.present(original);
    fresh.palette()->ocrRequested();
    require(provider.pending.size() == 1, "fresh OCR starts once");
    require(provider.pending[0].request.image == original.image, "recognition uses base pixels");
    provider.complete(0, presentation);
    require(fresh.recognition()->editing(), "fresh OCR enters editing directly");
    fresh.palette()->moveRequested();
    require(fresh.document()->crop(Axis::Rows, 1, 3), "new recognition state");
    fresh.palette()->ocrRequested();
    require(provider.pending.size() == 2 &&
                provider.pending[1].request.image.size() == QSize(12, 8),
            "cropped input reaches OCR");
    fresh.palette()->moveRequested();
    require(fresh.document()->crop(Axis::Columns, 2, 5), "crop while OCR is pending");
    fresh.palette()->ocrRequested();
    provider.complete(1, presentation);
    require(!fresh.recognition()->hasTextResult(), "stale recognition completion ignored");
    fresh.shutdown();
    provider.complete(2, presentation);

    QTemporaryDir historyDirectory;
    auto repository = snow_shot::storage::makeCaptureHistoryRepository(historyDirectory.path());
    snow_shot::storage::CaptureHistoryDraft draft;
    draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    draft.createdUtc = QDateTime::currentDateTimeUtc();
    draft.canvasBounds = original.canvasRect.toRect();
    draft.selection.rectangle = draft.canvasBounds;
    draft.selection.shadowColor = Qt::transparent;
    draft.canvasHistory = editable.canvasHistory;
    snow_shot::storage::CaptureHistoryDisplayDraft display;
    display.stableId = QStringLiteral("display");
    display.name = QStringLiteral("Mixed DPI");
    display.image = original.image;
    display.sourceCanvasOrigin = draft.canvasBounds.topLeft();
    display.sourceCanvasRect = draft.canvasBounds;
    display.canvasUsesPoints = true;
    display.backingScale = 2;
    draft.displays.append(display);
    draft.resultImage = original.image;
    const auto published = repository->publish(std::move(draft)).get();
    require(published.storage.success, "history fixture publishes");
    const auto snapshotRevision = repository->recordsSnapshot().revision;
    const auto historySource = readImageEditingHistory(*repository, published.record);
    require(historySource.error.isEmpty() && !historySource.canvasHistory.isEmpty() &&
                historySource.canvasRect == original.canvasRect &&
                historySource.image.size() == original.image.size(),
            "history restores editable data and mixed DPI mapping");
    require(repository->recordsSnapshot().revision == snapshotRevision,
            "opening history never writes record");
    const auto assets = repository->displayAssets(published.record);
    require(assets && !assets->displays.isEmpty() && assets->result, "history files available");
    require(
        QFile::remove(QDir(QFileInfo(assets->displays[0].localFileUrl.toLocalFile()).absolutePath())
                          .filePath(QStringLiteral("canvas_history.json"))),
        "corrupt history fixture");
    const auto flattened = readImageEditingHistory(*repository, published.record);
    require(flattened.error.isEmpty() && flattened.flattened && flattened.canvasHistory.isEmpty(),
            "unreadable edit payload uses composite");
    require(repository->records().size() == 1 &&
                repository->recordsSnapshot().revision == snapshotRevision,
            "failed read never prunes history");
    require(QFile::remove(assets->result->localFileUrl.toLocalFile()), "missing composite fixture");
    require(!readImageEditingHistory(*repository, published.record).error.isEmpty() &&
                repository->records().size() == 1,
            "missing pixels give recoverable error without pruning");
    ImageEditingDocument independent;
    require(independent.load(historySource), "owned working copy survives source deletion");

    snow_shot::storage::PinnedWindowRecord pinRecord;
    pinRecord.image = original.image;
    pinRecord.canvasSession = saved.serializeDocumentSession();
    pinRecord.imageTransform.rotate(90);
    pinRecord.imageTransform.scale(-1, 1);
    pinRecord.contentCanvasRect = QRectF(-20, 15, 5, 6);
    const auto textPreparation = captureScreenshotPinnedTextPreparation();
    auto rotated = readImageEditingPinned(pinRecord, textPreparation);
    require(rotated.image == snowCanvasTransformImage(original.image, pinRecord.imageTransform,
                                                      Qt::SmoothTransformation) &&
                rotated.canvasSession == pinRecord.canvasSession,
            "pinned transform preserves annotation mapping");
    auto animation = std::make_shared<ScreenshotAnimationSource>();
    animation->firstFrame = original.image;
    pinRecord.animationSource = animation;
    pinRecord.image.fill(Qt::black);
    require(readImageEditingPinned(pinRecord, textPreparation).image == rotated.image,
            "closed animation uses first frame");
    pinRecord = {};
    pinRecord.sourceKind = snow_shot::storage::PinnedWindowSourceKind::ClipboardText;
    pinRecord.originalHtml =
        QStringLiteral("<p style='font-size:24px;color:#c03060'><b>Styled text</b></p>");
    pinRecord.originalText = QStringLiteral("Styled text");
    pinRecord.firstCreationTextDpi = 1.5;
    auto textImage = readImageEditingPinned(pinRecord, textPreparation);
    ScreenshotClipboardOriginalContent textContent;
    textContent.html = pinRecord.originalHtml;
    textContent.text = pinRecord.originalText;
    auto rendered = prepareScreenshotPinnedText(textContent, 1.5, textPreparation);
    require(rendered && textImage.image ==
                            snowCanvasTransformImage(rendered->image, {}, Qt::SmoothTransformation),
            "formatted text retains original rendering scale");

    ImageEditingWindow exportWindow;
    exportWindow.present(original);
    require(!exportWindow.document()
                 ->runtime()
                 .applyAnnotationTransaction(
                     R"({"version":1,"operations":[{"type":"rectangle","bounds":[-19,16,2,2]}]})")
                 .isEmpty(),
            "export checkpoint fixture");
    const auto exportedCheckpoint = exportWindow.document()->checkpoint();
    bool drawingExportFinished = false;
    QObject::connect(&exportWindow, &ImageEditingWindow::exportFinished, &exportWindow,
                     [&](bool success) {
                         require(success, "drawing copy succeeds");
                         drawingExportFinished = true;
                     });
    exportWindow.palette()->copyRequested();
    require(!exportWindow.document()
                 ->runtime()
                 .applyAnnotationTransaction(
                     R"({"version":1,"operations":[{"type":"rectangle","bounds":[-17,18,1,1]}]})")
                 .isEmpty(),
            "edit during asynchronous export");
    awaitCondition([&] { return drawingExportFinished; }, "drawing copy completes");
    require(exportWindow.dirty() && exportWindow.modal()->isOpen(),
            "later edits remain unsaved after export");
    require(statusMessage(exportWindow) &&
                statusMessage(exportWindow)->type() == adqt::widgets::AdMessage::Type::Success &&
                statusMessage(exportWindow)->content() == QStringLiteral("Export completed"),
            "completed image copy displays a success message");
    require(exportWindow.document()->undo() &&
                exportWindow.document()->checkpoint() == exportedCheckpoint &&
                !exportWindow.dirty(),
            "export saves exactly its captured content");
    exportWindow.shutdown();
    // Exercise export completion at the editor boundary with deterministic print results.
    ScreenshotPrintService::Completion printCompletion;
    QImage printedImage;
    ScreenshotPrintService printing([&](QWidget*, QImage image, auto completion) {
        printedImage = std::move(image);
        printCompletion = std::move(completion);
    });
    ImageEditingServices outputServices;
    outputServices.printing = &printing;
    ImageEditingWindow outputs(outputServices);
    outputs.present(original);
    require(outputs.document()->crop(Axis::Rows, 2, 4), "output fixture has unsaved content");
    int outputCount = 0;
    bool outputSucceeded = false;
    QObject::connect(&outputs, &ImageEditingWindow::exportFinished, &outputs, [&](bool success) {
        ++outputCount;
        outputSucceeded = success;
    });
    require(exportSettings.setImageSaveDirectory(exportDirectory.path()),
            "drawing export directory");
    const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    outputs.palette()->saveRequested();
    auto* saveDialog = outputs.canvas()->window()->findChild<QFileDialog*>(
        QStringLiteral("screenshotManualSaveDialog"));
    require(saveDialog, "Save as file uses the existing dialog");
    saveDialog->reject();
    awaitCondition([&] { return outputCount == 1; }, "save cancellation completes");
    require(!outputSucceeded && outputs.dirty() && outputs.modal()->isOpen(),
            "cancelled save preserves content and keeps editor open");
    require(!statusMessage(outputs), "cancelled export dismisses its loading message");
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    outputs.palette()->saveRequested();
    saveDialog = outputs.canvas()->window()->findChild<QFileDialog*>(
        QStringLiteral("screenshotManualSaveDialog"));
    require(saveDialog, "save can retry after cancellation");
    const auto destination = exportDirectory.filePath(QStringLiteral("editor-result.png"));
    saveDialog->selectFile(destination);
    QMetaObject::invokeMethod(saveDialog, "accept", Qt::DirectConnection);
    awaitCondition([&] { return outputCount == 2; }, "manual image save completes");
    require(outputSucceeded && QFileInfo::exists(destination) && !outputs.dirty(),
            "successful save marks only current drawing content saved");
    require(statusMessage(outputs) &&
                statusMessage(outputs)->type() == adqt::widgets::AdMessage::Type::Success,
            "completed image file export displays a success message");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
    require(outputs.document()->crop(Axis::Columns, 1, 2), "print fixture");
    const auto printSize = outputs.document()->image().size();
    outputs.palette()->printRequested();
    awaitCondition([&] { return bool(printCompletion); }, "print receives immutable image");
    require(statusMessage(outputs) &&
                statusMessage(outputs)->type() == adqt::widgets::AdMessage::Type::Loading,
            "image export displays a loading message until output completes");
    require(printedImage.size() == printSize, "print ignores the preview viewport size");
    std::exchange(printCompletion, {})({ScreenshotPrintService::Status::Cancelled, {}});
    awaitCondition([&] { return outputCount == 3; }, "print cancellation completes");
    require(!outputSucceeded && outputs.dirty(), "cancelled printing preserves unsaved changes");
    outputs.palette()->printRequested();
    awaitCondition([&] { return bool(printCompletion); }, "print retry starts");
    const auto submittedCheckpoint = outputs.document()->checkpoint();
    require(outputs.document()->crop(Axis::Rows, 1, 2), "drawing changes while printing");
    std::exchange(printCompletion, {})({ScreenshotPrintService::Status::Submitted, {}});
    awaitCondition([&] { return outputCount == 4; }, "print submission completes");
    require(outputSucceeded && outputs.dirty() && outputs.document()->undo() &&
                outputs.document()->checkpoint() == submittedCheckpoint && !outputs.dirty(),
            "print completion saves its snapshot and preserves later edits");
    require(outputs.document()->crop(Axis::Rows, 1, 2), "upload failure fixture");
    outputs.palette()->cloudUploadRequested();
    awaitCondition([&] { return outputCount == 5; }, "unconfigured upload completes");
    require(!outputSucceeded && outputs.dirty() && outputs.modal()->isOpen(),
            "unavailable upload preserves the editing session");
    require(statusMessage(outputs) &&
                statusMessage(outputs)->type() == adqt::widgets::AdMessage::Type::Error,
            "failed image export replaces the spinner with an error message");
    outputs.palette()->quickSaveRequested();
    awaitCondition([&] { return outputCount == 6; }, "drawing Quick Save completes");
    require(outputSucceeded && !outputs.dirty() && outputs.modal()->isOpen(),
            "drawing Quick Save saves the current checkpoint without closing");
    static_cast<void>(exportSettings.setImageSaveDirectory(oldDirectory));
    outputs.shutdown();
    ImageEditingWindowCoordinator coordinator({});
    coordinator.presentPinnedRecord(QStringLiteral("missing-test-record"));
    coordinator.presentPinnedRecord(QStringLiteral("missing-test-record"));
    const auto editors = coordinator.windows();
    require(editors.size() == 2 && editors[0] != editors[1],
            "each invocation creates an independent window");
    for (auto editor : editors)
        editor->present(original);
    QApplication::processEvents();
    require(editors[0]->document()->crop(Axis::Rows, 1, 3) &&
                editors[1]->document()->image() == original.image,
            "editing one working copy cannot change another");
    bool reviewed = false, accepted = false;
    coordinator.confirmShutdown([&](bool value) {
        reviewed = true;
        accepted = value;
    });
    for (auto* dialog : editors[0]->findChildren<adqt::widgets::AdModal*>())
        if (dialog != editors[0]->modal())
            dialog->reject();
    awaitCondition([&] { return reviewed; }, "shutdown decision delivered");
    require(!accepted && editors[0]->modal()->isOpen() && editors[0]->dirty(),
            "Continue Editing cancels application exit");
    reviewed = false;
    coordinator.confirmShutdown([&](bool value) {
        reviewed = true;
        accepted = value;
    });
    for (auto* dialog : editors[0]->findChildren<adqt::widgets::AdModal*>())
        if (dialog != editors[0]->modal() && dialog->isOpen())
            dialog->accept();
    awaitCondition([&] { return reviewed; }, "discard decision delivered");
    require(accepted, "discard allows application exit");
    coordinator.shutdown();
}
