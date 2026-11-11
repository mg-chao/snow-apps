// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshotimageconversioncontroller.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snowimageqtcodec.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/button.h"
#include "widgets/input_line_edit.h"
#include "widgets/modal.h"
#include "widgets/result.h"
#include "widgets/select.h"
#include "../src/presentation/imageediting/imageeditingexportcontroller.h"
#include "spreadsheet_open_test_support.h"
#include "recognition_workbook_test_support.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHostAddress>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <stdexcept>

namespace {
using adqt::widgets::AdLineEdit;
using adqt::widgets::AdModal;
using snow_shot::presentation::ImageEditingDocument;
using snow_shot::presentation::ImageEditingSource;
using snow_shot::presentation::ImageEditingWindow;
using snow_shot::presentation::ImageEditingWindowCoordinator;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void drainEvents() {
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

void awaitCondition(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 15000) {
        drainEvents();
        QThread::msleep(1);
    }
    require(condition(), message);
}

ImageEditingSource source() {
    ImageEditingSource result;
    result.image = QImage(24, 18, QImage::Format_ARGB32);
    for (int y = 0; y < result.image.height(); ++y)
        for (int x = 0; x < result.image.width(); ++x)
            result.image.setPixelColor(x, y, QColor(x * 10, y * 13, (x + y) * 6));
    result.canvasRect = QRectF(-8, 10, 24, 18);
    return result;
}

AdModal* customDialog(ImageEditingWindow& window) {
    return window.findChild<AdModal*>(QStringLiteral("screenshotSaveAsFileModal"));
}

QFileDialog* systemDialog(ImageEditingWindow& window) {
    return window.canvas()->window()->findChild<QFileDialog*>(
        QStringLiteral("screenshotManualSaveDialog"));
}

void saveWorkflow(bool custom, const QTemporaryDir& directory, bool showResult = false,
                  ScreenshotImageFileFormat format = ScreenshotImageFileFormat::Png) {
    using Axis = ImageEditingDocument::CropAxis;
    require(snow_shot::storage::ScreenshotSettings().setLastManualSaveFormat(QStringLiteral("png")),
            "each editor save starts from the PNG default");
    ImageEditingWindow window;
    auto initial = source();
    if (format == ScreenshotImageFileFormat::Tiff)
        initial.image.setPixelColor(0, 0, QColor(71, 93, 118, 59));
    window.present(initial);
    require(window.document()->crop(Axis::Rows, 2, 4), "save fixture has unsaved image changes");
    int exports = 0;
    bool succeeded = false;
    QObject::connect(&window, &ImageEditingWindow::exportFinished, &window, [&](bool success) {
        ++exports;
        succeeded = success;
    });

    window.palette()->saveRequested();
    if (custom) {
        auto* dialog = customDialog(window);
        require(dialog && dialog->isOpen() && !systemDialog(window),
                "Image Editor honors the Snow Shot save-dialog preference");
        require(dialog->ownerWindow() == window.canvas()->window() &&
                    dialog->windowModality() == Qt::WindowModal &&
                    dialog->contentWidget()->window()->windowHandle()->transientParent() ==
                        window.palette()->window()->windowHandle(),
                "custom save dialog belongs to the editor and stays above its floating toolbar");
        dialog->reject();
    } else {
        auto* dialog = systemDialog(window);
        require(dialog && dialog->isVisible() && !customDialog(window),
                "Image Editor honors the system save-dialog preference");
        dialog->reject();
    }
    awaitCondition([&] { return exports == 1; }, "save cancellation completes once");
    require(!succeeded && window.dirty() && window.modal()->isOpen(),
            "cancelled save preserves unsaved image changes and the open editor");
    drainEvents();

    const auto expected = window.document()->image();
    const auto checkpoint = window.document()->checkpoint();
    const QString filename =
        showResult ? QStringLiteral("editor-result")
                   : (custom ? QStringLiteral("editor-custom") : QStringLiteral("editor-system"));
    const QString suffix = format == ScreenshotImageFileFormat::Tiff && !custom
                               ? QStringLiteral("tif")
                               : ScreenshotImageFileService::extension(format);
    const auto destination = directory.filePath(filename + u'.' + suffix);
    window.palette()->saveRequested();
    if (custom) {
        auto* dialog = customDialog(window);
        require(dialog && dialog->isOpen(), "custom save can retry after cancellation");
        auto* content = dialog->contentWidget();
        content->findChild<adqt::widgets::AdSelect*>(QStringLiteral("saveFormatSelect"))
            ->setCurrentValue(ScreenshotImageFileService::formatKey(format));
        content->findChild<AdLineEdit*>(QStringLiteral("saveDirectoryInput"))
            ->setText(directory.path());
        content->findChild<AdLineEdit*>(QStringLiteral("saveFilenameInput"))->setText(filename);
        awaitCondition([&] { return dialog->acceptButton()->isEnabled(); },
                       "custom save enables writing after preparing the image");
        dialog->acceptButton()->click();
    } else {
        auto* dialog = systemDialog(window);
        require(dialog && dialog->nameFilters().contains(
                              ScreenshotImageFileService::dialogFilter(format)),
                "system save can retry and exposes the selected image format");
        dialog->selectNameFilter(ScreenshotImageFileService::dialogFilter(format));
        dialog->selectFile(destination);
        require(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
                "system save accepts the chosen destination");
    }
    require(window.document()->crop(Axis::Columns, 1, 3),
            "image changes while the captured save is pending");
    if (showResult) {
        auto* dialog = customDialog(window);
        auto* result = dialog->contentWidget()->findChild<adqt::widgets::AdResult*>(
            QStringLiteral("saveResult"));
        awaitCondition([&] { return result && result->isVisible(); },
                       "editor save shows its result");
        require(exports == 1 && dialog->isOpen() && window.modal()->isOpen(),
                "result view must keep editor export pending and its owner alive");
        dialog->reject();
    }
    awaitCondition([&] { return exports == 2; }, "image file save completes once");
    require(succeeded && QFileInfo::exists(destination), "selected save dialog writes an image");
    const auto saved = snow_shot::image_codec::decodeFile(
                           destination, ScreenshotImageFileService::snowImageFormat(format))
                           .convertToFormat(QImage::Format_ARGB32);
    require(saved.size() == expected.size(), "saved image has the captured crop dimensions");
    for (int y = 0; y < expected.height(); ++y)
        for (int x = 0; x < expected.width(); ++x)
            require(saved.pixelColor(x, y) == expected.pixelColor(x, y),
                    "saved image contains the captured cropped pixels rather than later changes");
    require(window.dirty() && window.modal()->isOpen() && window.document()->undo() &&
                window.document()->checkpoint() == checkpoint && !window.dirty(),
            "saving marks its captured checkpoint exported and preserves later unsaved changes");
    window.shutdown();
    drainEvents();
}

void shutdownCancelsSave(bool custom) {
    ImageEditingWindowCoordinator coordinator({});
    coordinator.presentSource(ScreenshotExportSource::fromImage(source().image));
    require(coordinator.windows().size() == 1, "save shutdown fixture owns one editor");
    QPointer<ImageEditingWindow> window = coordinator.windows().first();
    awaitCondition([&] { return !window->document()->image().isNull(); },
                   "save shutdown fixture finishes loading");
    int exports = 0;
    QObject::connect(window, &ImageEditingWindow::exportFinished, &coordinator,
                     [&](bool) { ++exports; });
    window->palette()->saveRequested();
    QPointer<QObject> dialog;
    if (custom) {
        dialog = customDialog(*window);
        require(dialog && customDialog(*window)->isOpen(), "custom save is pending at shutdown");
    } else {
        dialog = systemDialog(*window);
        require(dialog && systemDialog(*window)->isVisible(), "system save is pending at shutdown");
    }
    coordinator.shutdown();
    awaitCondition([&] { return !dialog && !window; },
                   "shutdown destroys the editor and its pending save dialog");
    require(exports == 0, "shutdown suppresses late save completion");
}

void exportCompletionMayDestroyEditor() {
    auto window = std::make_unique<ImageEditingWindow>();
    window->present(source());
    const QPointer<ImageEditingWindow> lifetime(window.get());
    QObject receiver;
    int completions = 0;
    QObject::connect(
        window.get(), &ImageEditingWindow::exportFinished, &receiver, [&](bool success) {
            require(success, "copy succeeds before its completion destroys the editor");
            ++completions;
            window.reset();
        });
    window->palette()->copyRequested();
    awaitCondition([&] { return !lifetime; }, "export completion may destroy its editor");
    drainEvents();
    require(completions == 1, "export completes exactly once during reentrant destruction");
}

void clipboardPublicationMayDestroyEditor() {
    auto original = source();
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = original.canvasRect.toRect();
    presentation->prepareForRendering();
    original.recognition.key = QStringLiteral("clipboard-destruction");
    original.recognition.text = ScreenshotOcrRecognitionResult{presentation};
    auto window = std::make_unique<ImageEditingWindow>();
    window->present(original);
    window->palette()->ocrRequested();
    window->palette()->textEditRequested();
    require(window->recognition(), "recognition copy owns an editing session");
    window->recognition()->setTextDraft(QStringLiteral("Copy then close"));
    const QPointer<ImageEditingWindow> lifetime(window.get());
    QObject receiver;
    int completions = 0;
    QObject::connect(window.get(), &ImageEditingWindow::exportFinished, &receiver,
                     [&](bool) { ++completions; });
    QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged, &receiver,
                     [&] { window.reset(); });
    window->palette()->copyRequested();
    require(!lifetime && !window, "clipboard publication can synchronously destroy its editor");
    drainEvents();
    require(completions == 0, "destroyed clipboard owner cannot receive a late export completion");
}

#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
using ConversionController = ScreenshotImageConversionController;
using ConversionFormat = SnowShotImageConversionFormat;
using RecognitionSession = ScreenshotRecognitionSessionController;

QString conversionKey(ConversionFormat format, const QString& model) {
    return QStringLiteral("conversion:%1:%2").arg(int(format)).arg(model);
}

QString conversionContent(ConversionFormat format) {
    return format == ConversionFormat::Markdown ? QStringLiteral("# Shared result")
                                                : QStringLiteral("<p>Shared result</p>");
}

void activateConversion(ImageEditingWindow& window, ConversionFormat format) {
    if (format == ConversionFormat::Markdown)
        window.palette()->markdownRequested();
    else
        window.palette()->htmlRequested();
}

adqt::widgets::AdButton* conversionButton(ImageEditingWindow& window, ConversionFormat format) {
    auto* button = window.palette()->findChild<adqt::widgets::AdButton*>(
        format == ConversionFormat::Markdown ? QStringLiteral("screenshotConvertToMarkdownButton")
                                             : QStringLiteral("screenshotConvertToHtmlButton"));
    require(button, "conversion availability fixture exposes both conversion buttons");
    return button;
}

void requireConversionState(ImageEditingWindow& window, bool enabled, bool markdownBusy = false,
                            bool htmlBusy = false) {
    auto* markdown = conversionButton(window, ConversionFormat::Markdown);
    auto* html = conversionButton(window, ConversionFormat::Html);
    require(markdown->isEnabled() == enabled && html->isEnabled() == enabled,
            "conversion availability depends on providers or cached results rather than mode");
    require(markdown->busy() == markdownBusy && html->busy() == htmlBusy,
            "only the active conversion format displays its pending workflow state");
}

ImageEditingSource conversionAvailabilitySource() {
    auto initial = source();
    initial.recognition.key = QStringLiteral("editor-conversion-availability");
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = initial.canvasRect.toRect();
    presentation->prepareForRendering();
    initial.recognition.text = ScreenshotOcrRecognitionResult{std::move(presentation)};
    return initial;
}

void useStandaloneConversionButtons(ImageEditingWindow& window) {
    window.palette()->setActionToolsLayout({{{QStringLiteral("convert-to-markdown")},
                                             {QStringLiteral("convert-to-html")},
                                             {QStringLiteral("latex-recognition")},
                                             {QStringLiteral("text-recognition")}},
                                            {}});
}

void clickCachedConversion(ImageEditingWindow& window, ConversionFormat format,
                           const QString& model, const char* phase) {
    auto* button = conversionButton(window, format);
    if (!button->isEnabled() || button->isHidden())
        throw std::runtime_error(
            QStringLiteral("%1 conversion button unavailable: format=%2, enabled=%3, hidden=%4, "
                           "items=%5")
                .arg(QString::fromLatin1(phase))
                .arg(int(format))
                .arg(button->isEnabled())
                .arg(button->isHidden())
                .arg(button->property("screenshotToolbarPositionItems").toStringList().join(u','))
                .toStdString());
    const auto previousTool = window.palette()->activeTool();
    button->click();
    const auto* session = window.recognition();
    const QString expected = conversionKey(format, model);
    if (!session || !session->active() || session->activeContentKey() != expected) {
        QStringList cached;
        if (session)
            for (const auto& entry : session->cachedRecognitionResults().conversions)
                cached.append(QStringLiteral("%1:%2").arg(int(entry.format)).arg(entry.model));
        const auto tool = window.palette()->activeTool();
        throw std::runtime_error(
            QStringLiteral("%1 conversion button: format=%2, expected=%3, actual=%4, active=%5, "
                           "mode=%6, previous tool=%7, current tool=%8, cached=%9, model=%10")
                .arg(QString::fromLatin1(phase))
                .arg(int(format))
                .arg(expected, session ? session->activeContentKey() : QStringLiteral("no session"))
                .arg(session && session->active())
                .arg(session ? int(session->mode()) : -1)
                .arg(previousTool ? int(*previousTool) : -1)
                .arg(tool ? int(*tool) : -1)
                .arg(cached.join(u','),
                     snow_shot::storage::ScreenshotImageConversionSettings().visionModel())
                .toStdString());
    }
    requireConversionState(window, true);
}

void cachedConversionsRemainAvailableAcrossModes() {
    const snow_shot::storage::ScreenshotImageConversionSettings settings;
    const QString model = QStringLiteral("editor-conversion-availability-model");
    require(settings.setVisionModel(model), "select the cached conversion availability model");
    auto initial = conversionAvailabilitySource();
    for (const auto format : {ConversionFormat::Markdown, ConversionFormat::Html})
        initial.recognition.conversions.append({format, model, conversionContent(format)});
    ImageEditingWindow window;
    window.present(initial);
    useStandaloneConversionButtons(window);
    requireConversionState(window, true);
    for (const auto format : {ConversionFormat::Markdown, ConversionFormat::Html}) {
        clickCachedConversion(window, format, model, "initial mode");
        require(window.palette()->activateToolShortcut(ScreenshotToolPalette::Tool::Move),
                "select Pan through the toolbar command path");
        require(window.canvas()->mode() == snow_shot::presentation::ImageEditingCanvas::Mode::Pan,
                "Pan leaves the conversion view");
        requireConversionState(window, true);
        clickCachedConversion(window, format, model, "after Pan");
        require(window.palette()->activateToolShortcut(ScreenshotToolPalette::Tool::Shape),
                "select a shape through the toolbar command path");
        require(window.canvas()->mode() == snow_shot::presentation::ImageEditingCanvas::Mode::Draw,
                "shape drawing leaves the conversion view");
        requireConversionState(window, true);
        clickCachedConversion(window, format, model, "after shape");
        require(window.palette()->activateToolShortcut(ScreenshotToolPalette::Tool::Ocr),
                "select OCR through the toolbar command path");
        require(window.recognition()->mode() == RecognitionSession::Mode::Text &&
                    window.recognition()->hasTextResult() && !window.recognition()->editing(),
                "OCR replaces the conversion view with its cached image overlay");
        requireConversionState(window, true);
        clickCachedConversion(window, format, model, "after OCR");
    }
    require(!window.dirty(), "view changes preserve the imported conversion checkpoints");
    window.shutdown();
}

void conversionAvailabilityTracksProvidersAndCache() {
    const snow_shot::storage::ScreenshotImageConversionSettings settings;
    const QString model = QStringLiteral("editor-conversion-provider-model");
    require(settings.setVisionModel(model), "select the conversion provider availability model");
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "start an isolated pending conversion service");
    const auto serviceUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    std::unique_ptr<SnowShotApiClient> provider;
    ImageEditingWindow window(
        {[&] { return ScreenshotPinnedRecognitionProviders{nullptr, nullptr, provider.get()}; }});
    window.present(conversionAvailabilitySource());
    useStandaloneConversionButtons(window);
    requireConversionState(window, false);
    provider = std::make_unique<SnowShotApiClient>(serviceUrl);
    window.palette()->ocrRequested();
    requireConversionState(window, true);
    for (const auto format : {ConversionFormat::Markdown, ConversionFormat::Html}) {
        conversionButton(window, format)->click();
        require(window.recognition()->busy(format == ConversionFormat::Markdown
                                               ? RecognitionSession::Mode::Markdown
                                               : RecognitionSession::Mode::Html),
                "the isolated provider keeps conversion model loading pending");
        requireConversionState(window, true, format == ConversionFormat::Markdown,
                               format == ConversionFormat::Html);
        require(window.palette()->activateToolShortcut(ScreenshotToolPalette::Tool::Shape),
                "leave pending conversion through the shape command");
        requireConversionState(window, true);
    }
    provider.reset();
    window.palette()->moveRequested();
    requireConversionState(window, false);
    auto results = window.recognition()->cachedRecognitionResults();
    for (const auto format : {ConversionFormat::Markdown, ConversionFormat::Html})
        results.conversions.append({format, model, conversionContent(format)});
    window.recognition()->seedRecognitionResults(std::move(results));
    requireConversionState(window, true);
    for (const auto format : {ConversionFormat::Markdown, ConversionFormat::Html})
        clickCachedConversion(window, format, model, "after provider removal");
    provider = std::make_unique<SnowShotApiClient>(serviceUrl);
    window.palette()->ocrRequested();
    require(window.recognition()->cachedRecognitionResults().conversions.isEmpty(),
            "rebinding a provider retires offline results without matching model identity");
    requireConversionState(window, true);
    provider.reset();
    window.palette()->moveRequested();
    requireConversionState(window, false);
    window.shutdown();
}

void conversionIdentitySurvivesModelSelection(ConversionFormat format) {
    const snow_shot::storage::ScreenshotImageConversionSettings settings;
    const QString firstModel = QStringLiteral("editor-conversion-model-a");
    const QString secondModel = QStringLiteral("editor-conversion-model-b");
    const QString target = QStringLiteral("editor-conversion-identity");
    const auto image = source().image;
    const QVector<ScreenshotImageConversionEntry> entries{
        {format, firstModel, conversionContent(format)},
        {format, secondModel, conversionContent(format)}};
    require(settings.setVisionModel(secondModel), "select the second cached conversion model");
    ConversionController controller;
    require(controller.activeModel().isEmpty(), "an inactive conversion has no displayed model");
    controller.seed(target, entries);
    controller.activate(target, image, format);
    require(controller.state() == ConversionController::State::Completed &&
                controller.activeModel() == secondModel &&
                controller.source() == conversionContent(format),
            "identical cached outputs retain the active request's model identity");
    require(settings.setVisionModel(firstModel), "change the saved model selection");
    require(controller.activeModel() == secondModel,
            "changing preferences cannot relabel a displayed conversion result");
    controller.activate(target, image, format);
    require(controller.activeModel() == firstModel,
            "reactivating a cached result updates its displayed model identity");
    controller.deactivate();
    require(controller.activeModel().isEmpty(), "deactivation retires displayed model identity");

    RecognitionSession session(nullptr, nullptr, nullptr, {});
    session.setIndependentEditing(true);
    session.setTarget({target, image, QRectF(QPointF(), QSizeF(image.size()))});
    ScreenshotRecognitionResults results;
    results.key = target;
    results.conversions = entries;
    session.seedRecognitionResults(results);
    const auto mode = format == ConversionFormat::Markdown ? RecognitionSession::Mode::Markdown
                                                           : RecognitionSession::Mode::Html;
    require(settings.setVisionModel(secondModel), "select the second recognition result");
    session.activate(mode);
    const auto checkpoints = session.contentCheckpoints();
    require(session.activeContentKey() == conversionKey(format, secondModel) &&
                checkpoints.value(conversionKey(format, firstModel)) ==
                    checkpoints.value(conversionKey(format, secondModel)),
            "equal result bytes still select distinct model checkpoints");
    require(settings.setVisionModel(firstModel), "select another model without activating it");
    require(session.activeContentKey() == conversionKey(format, secondModel),
            "export identity follows displayed content instead of current preferences");
    session.activate(mode);
    require(session.activeContentKey() == conversionKey(format, firstModel),
            "switching cached recognition results switches their export checkpoint");
    require(settings.setVisionModel(QStringLiteral("editor-conversion-unavailable")),
            "select an unavailable conversion model");
    session.activate(mode);
    require(session.activeContentKey().isEmpty(),
            "failed conversion activation cannot export a preceding model checkpoint");
}

void conversionExportMarksActiveModel(ConversionFormat format, bool copy,
                                      const QTemporaryDir& directory) {
    const snow_shot::storage::ScreenshotImageConversionSettings settings;
    const QString firstModel = QStringLiteral("editor-conversion-model-a");
    const QString secondModel = QStringLiteral("editor-conversion-model-b");
    require(settings.setVisionModel(firstModel), "select the imported conversion model");
    auto initial = source();
    initial.recognition.key = QStringLiteral("editor-conversion-export");
    initial.recognition.conversions = {{format, firstModel, conversionContent(format)}};
    ImageEditingWindow window;
    window.present(initial);
    activateConversion(window, format);
    auto* session = window.recognition();
    require(session && !window.dirty(), "an imported conversion establishes a clean checkpoint");
    auto* conversion = session->findChild<ConversionController*>();
    require(conversion, "recognition owns its conversion controller");
    auto results = session->recognitionResultsSnapshot();
    results.conversions.append({format, secondModel, conversionContent(format)});
    // A later completed result is absent from the editor's imported clean checkpoints.
    conversion->seed(results.key, results.conversions);
    require(settings.setVisionModel(secondModel), "select the later identical conversion");
    activateConversion(window, format);
    require(window.dirty(), "a new model result needs its own successful export");
    int exports = 0;
    bool succeeded = false;
    QObject::connect(&window, &ImageEditingWindow::exportFinished, &window, [&](bool success) {
        ++exports;
        succeeded = success;
    });
    if (copy) {
        window.palette()->copyRequested();
        const auto* mime = QApplication::clipboard()->mimeData();
        require(mime && (format == ConversionFormat::Markdown
                             ? mime->text() == conversionContent(format)
                             : mime->html() == conversionContent(format)),
                "Copy publishes the displayed conversion content");
    } else {
        window.palette()->saveRequested();
        auto* dialog = window.canvas()->window()->findChild<QFileDialog*>(
            QStringLiteral("imageEditingRecognitionSaveDialog"));
        require(dialog && dialog->isVisible(), "conversion Save opens its destination dialog");
        const auto path = directory.filePath(format == ConversionFormat::Markdown
                                                 ? QStringLiteral("conversion-markdown-model.md")
                                                 : QStringLiteral("conversion-html-model.html"));
        dialog->selectFile(path);
        require(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
                "conversion Save accepts its output path");
        awaitCondition(
            [&] {
                return exports == 1 ||
                       window.canvas()->window()->findChild<QMessageBox*>(
                           QStringLiteral("imageEditingRecognitionOverwriteDialog"));
            },
            "conversion Save finishes writing or requests overwrite review");
        require(exports == 1,
                "isolated conversion outputs and their companions need no overwrite review");
        QFile file(path);
        require(file.open(QIODevice::ReadOnly) &&
                    file.readAll() == conversionContent(format).toUtf8(),
                "Save writes the displayed conversion content");
    }
    require(exports == 1 && succeeded && !window.dirty(),
            "successful conversion export clears the active model's unsaved checkpoint");
    require(settings.setVisionModel(firstModel), "return to the imported conversion model");
    activateConversion(window, format);
    require(!window.dirty(), "returning to the imported model preserves its clean checkpoint");
    require(settings.setVisionModel(secondModel), "return to the exported conversion model");
    activateConversion(window, format);
    require(!window.dirty(), "returning to the exported model preserves its clean checkpoint");
    int closes = 0;
    QObject::connect(&window, &ImageEditingWindow::closed, &window, [&] { ++closes; });
    window.requestClose();
    require(closes == 1 && !window.modal()->isOpen(),
            "an exported identical model result closes without a discard prompt");
    drainEvents();
}

void conversionExportsKeepModelCheckpoints(const QTemporaryDir& directory) {
    const snow_shot::storage::ScreenshotImageConversionSettings settings;
    const auto previousModel = settings.visionModel();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(settings.setVisionModel(previousModel)); });
    for (const auto format : {ConversionFormat::Markdown, ConversionFormat::Html}) {
        conversionIdentitySurvivesModelSelection(format);
        for (const bool copy : {true, false})
            conversionExportMarksActiveModel(format, copy, directory);
    }
}
#endif

#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
using ExportController = snow_shot::presentation::ImageEditingExportController;

void writeExistingResult(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write("existing result") == 15,
            "overwrite fixture writes an existing recognition result");
}

void requireExistingResult(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) &&
                file.readAll() == QByteArrayLiteral("existing result"),
            "cancelled overwrite preserves the existing recognition result");
}

QFileDialog* startRecognitionSave(ExportController& controller, QWidget* owner,
                                  ExportController::Completion completion) {
    controller.start(ExportController::Operation::Save,
                     ScreenshotRecognitionFileSnapshot{ScreenshotRecognitionFileKind::Qr,
                                                       QStringLiteral("replacement result")},
                     owner, nullptr, std::move(completion));
    auto* dialog =
        owner->findChild<QFileDialog*>(QStringLiteral("imageEditingRecognitionSaveDialog"));
    require(dialog && dialog->isVisible(),
            "recognition save opens its asynchronous destination dialog");
    return dialog;
}

QMessageBox* chooseRecognitionDestination(QFileDialog* dialog, QWidget* owner,
                                          const QString& path) {
    dialog->selectFile(path);
    bool returned = false, nested = false;
    QTimer nestingGuard;
    nestingGuard.setSingleShot(true);
    QObject::connect(&nestingGuard, &QTimer::timeout, owner, [&] {
        if (returned)
            return;
        nested = true;
        for (auto* decision : owner->findChildren<QMessageBox*>())
            decision->reject();
    });
    nestingGuard.start(0);
    require(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
            "recognition save accepts its destination without nesting an event loop");
    returned = true;
    nestingGuard.stop();
    require(!nested, "recognition destination acceptance never nests an overwrite event loop");
    awaitCondition(
        [&] {
            return owner->findChild<QMessageBox*>(
                QStringLiteral("imageEditingRecognitionOverwriteDialog"));
        },
        "existing recognition output opens an asynchronous overwrite decision");
    return owner->findChild<QMessageBox*>(QStringLiteral("imageEditingRecognitionOverwriteDialog"));
}

void recognitionOverwriteCompletesOnce(const QTemporaryDir& directory) {
    QObject lifetime;
    QWidget owner;
    owner.show();
    ExportController controller(nullptr, &lifetime);
    const QString path = directory.filePath(QStringLiteral("recognition-overwrite.txt"));
    writeExistingResult(path);
    int completions = 0;
    bool succeeded = false;
    const auto complete = [&](bool success, QString) {
        ++completions;
        succeeded = success;
    };
    QPointer<QFileDialog> dialog = startRecognitionSave(controller, &owner, complete);
    controller.start(ExportController::Operation::Save, QStringLiteral("duplicate"), &owner,
                     nullptr, complete);
    require(owner.findChildren<QFileDialog*>().size() == 1,
            "duplicate export requests do not open a second destination dialog");
    QPointer<QMessageBox> decision = chooseRecognitionDestination(dialog, &owner, path);
    require(controller.busy() && completions == 0 && decision->isVisible(),
            "overwrite review keeps one request pending without completing its output");
    controller.start(ExportController::Operation::Save, QStringLiteral("duplicate"), &owner,
                     nullptr, complete);
    require(owner.findChildren<QMessageBox*>().size() == 1,
            "duplicate export requests do not open a second overwrite decision");
    decision->button(QMessageBox::No)->click();
    awaitCondition([&] { return completions == 1 && !decision; },
                   "declining overwrite completes once and releases its decision");
    require(!controller.busy() && !succeeded,
            "declining overwrite leaves the exporter ready to retry");
    requireExistingResult(path);
    dialog = startRecognitionSave(controller, &owner, complete);
    decision = chooseRecognitionDestination(dialog, &owner, path);
    decision->button(QMessageBox::Yes)->click();
    awaitCondition([&] { return completions == 2 && !controller.busy(); },
                   "accepted overwrite writes exactly one recognition result");
    QFile file(path);
    require(succeeded && file.open(QIODevice::ReadOnly) &&
                file.readAll() == QByteArrayLiteral("replacement result"),
            "accepted overwrite publishes the captured recognition content");
    drainEvents();
    require(completions == 2,
            "destroyed save and overwrite dialogs cannot complete a request twice");
}

void recognitionOverwriteMayLoseOwner(const QTemporaryDir& directory) {
    for (const bool destroy : {false, true}) {
        QObject lifetime;
        auto owner = std::make_unique<QWidget>();
        owner->show();
        ExportController controller(nullptr, &lifetime);
        const QString path = directory.filePath(destroy ? QStringLiteral("destroy-overwrite.txt")
                                                        : QStringLiteral("cancel-overwrite.txt"));
        writeExistingResult(path);
        int completions = 0;
        QPointer<QFileDialog> dialog =
            startRecognitionSave(controller, owner.get(), [&](bool, QString) { ++completions; });
        QPointer<QMessageBox> decision = chooseRecognitionDestination(dialog, owner.get(), path);
        if (destroy)
            owner.reset();
        else
            controller.cancel();
        awaitCondition([&] { return !dialog && !decision && !controller.busy(); },
                       "cancellation or owner destruction releases pending overwrite dialogs");
        require(completions == 0, "retired overwrite decisions suppress late export completion");
        requireExistingResult(path);
    }
}

void recognitionAcceptanceMayCancel(const QTemporaryDir& directory) {
    QObject lifetime;
    QWidget owner;
    owner.show();
    ExportController controller(nullptr, &lifetime);
    const QString path = directory.filePath(QStringLiteral("cancel-acceptance.txt"));
    writeExistingResult(path);
    int completions = 0;
    QPointer<QFileDialog> dialog =
        startRecognitionSave(controller, &owner, [&](bool, QString) { ++completions; });
    QObject::connect(dialog, &QDialog::accepted, &lifetime, [&] { controller.cancel(); });
    dialog->selectFile(path);
    require(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
            "save acceptance survives synchronous request cancellation");
    awaitCondition([&] { return !dialog && !controller.busy(); },
                   "cancelled acceptance releases its dialog after returning from done");
    require(completions == 0 && owner.findChildren<QMessageBox*>().isEmpty(),
            "cancelled queued save cannot open an overwrite decision or complete output");
    requireExistingResult(path);
}
#endif
} // namespace

void runImageEditingConversionAvailabilityTests() {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    const snow_shot::storage::ScreenshotImageConversionSettings settings;
    const auto previousModel = settings.visionModel();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(settings.setVisionModel(previousModel)); });
    cachedConversionsRemainAvailableAcrossModes();
    conversionAvailabilityTracksProvidersAndCache();
#endif
}

void runImageEditingSpreadsheetOpenTests() {
    SpreadsheetOpenTestReceiver receiver;
    ImageEditingWindow window({});
    auto initial = source();
    initial.recognition.key = QStringLiteral("editor-open-spreadsheet");
    SnowShotTableResult table;
    table.html = QStringLiteral("<table><tr><td>00123</td><td>雪</td></tr></table>");
    initial.recognition.table = table;
    window.present(initial);
    window.palette()->tableRequested();
    auto* session = window.recognition();
    require(session && session->active(), "table recognition is active in the image editor");
    const auto cell = [&](const QString& text) {
        return session->editWorkflow({{QStringLiteral("action"), QStringLiteral("set_cell")},
                                      {QStringLiteral("row"), 0},
                                      {QStringLiteral("column"), 0},
                                      {QStringLiteral("text"), text}});
    };
    require(cell(QString(32768, QLatin1Char('x'))), "prepare an unsupported workbook cell");
    window.palette()->openSpreadsheetFileRequested();
    require(session->active() && receiver.calls == 0,
            "failed spreadsheet export retains recognition in the image editor");
    require(cell(QStringLiteral("00123")), "restore a supported workbook cell");
    window.palette()->openSpreadsheetFileRequested();
    require(receiver.calls == 1 && QFileInfo::exists(receiver.path) && !session->active() &&
                window.modal()->isOpen() &&
                window.palette()->activeTool() == ScreenshotToolPalette::Tool::Move,
            "successful spreadsheet launch returns to image editing without closing the host");
    require(recognition_workbook_test::sheet(recognition_workbook_test::parts(receiver.path))
                    .cells.value(QStringLiteral("A1")) == QStringLiteral("00123"),
            "the editor launches the recognized table as a workbook");
    window.shutdown();
    require(QFileInfo::exists(receiver.path), "editor shutdown retains the launched workbook");
}

void runImageEditingSaveTests() {
    const snow_shot::storage::ScreenshotSettings settings;
    const auto previousDialog = settings.saveAsFileDialog();
    const auto previousDirectory = settings.lastManualSaveDirectory();
    const auto previousFormat = settings.lastManualSaveFormat();
    const auto previousFormatOptions = settings.manualSaveFormatOptions();
    const bool previousShowResult = settings.showSaveResult();
    const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restore = qScopeGuard([&] {
        static_cast<void>(settings.setSaveAsFileDialog(previousDialog));
        static_cast<void>(settings.setLastManualSaveDirectory(previousDirectory));
        static_cast<void>(settings.setLastManualSaveState(previousFormat, previousFormatOptions));
        static_cast<void>(settings.setShowSaveResult(previousShowResult));
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
    });
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QTemporaryDir directory;
    require(directory.isValid() && settings.setLastManualSaveDirectory(directory.path()) &&
                settings.setLastManualSaveFormat(QStringLiteral("png")) &&
                settings.setShowSaveResult(false),
            "configure isolated image save destinations");
    for (const bool custom : {false, true}) {
        require(settings.setSaveAsFileDialog(custom ? QStringLiteral("snow_shot")
                                                    : QStringLiteral("system")),
                "select image save dialog");
        saveWorkflow(custom, directory);
        saveWorkflow(custom, directory, false, ScreenshotImageFileFormat::Tiff);
        shutdownCancelsSave(custom);
    }
    require(settings.setSaveAsFileDialog(QStringLiteral("snow_shot")) &&
                settings.setShowSaveResult(true),
            "enable editor save result");
    saveWorkflow(true, directory, true);
    require(settings.setShowSaveResult(false), "restore save result before other save tests");
    exportCompletionMayDestroyEditor();
    clipboardPublicationMayDestroyEditor();
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    conversionExportsKeepModelCheckpoints(directory);
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    recognitionOverwriteCompletesOnce(directory);
    recognitionOverwriteMayLoseOwner(directory);
    recognitionAcceptanceMayCancel(directory);
#endif
}
