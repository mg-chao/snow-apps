// SPDX-License-Identifier: GPL-3.0-or-later
#include "physical_key_test_support.h"
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshotdrawingtoolbaroptions.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotselectionexportuiservices.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/button.h"
#include "widgets/modal.h"
#include "../src/presentation/imageediting/imageeditingexportcontroller.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QTranslator>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
using namespace snow_shot::presentation;
using adqt::widgets::AdButton;
using Axis = ImageEditingDocument::CropAxis;

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
    result.image = QImage(240, 160, QImage::Format_ARGB32_Premultiplied);
    result.image.fill(Qt::transparent);
    for (int y = 10; y < 150; ++y)
        for (int x = 10; x < 230; ++x)
            result.image.setPixelColor(x, y, QColor(x, y, 90, 180));
    result.canvasRect = QRectF(-8, 10, 240, 160);
    return result;
}

QImage rendered(ImageEditingWindow& window) {
    auto& document = *window.document();
    return document.runtime().renderToImage(document.bounds(), document.image().size(),
                                            {{document.image(), document.bounds()}});
}

void addAnnotation(ImageEditingWindow& window) {
    require(
        !window.document()
             ->runtime()
             .applyAnnotationTransaction(QByteArrayLiteral(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[20,30,55,45],"style":{"fill":[255,0,0,255],"stroke":[0,0,0,0]}}]})"))
             .isEmpty(),
        "add an annotation before pinning");
}

void requirePixels(const QImage& actual, const QImage& expected, const char* scenario) {
    require(!actual.isNull() && actual.size() == expected.size(),
            "pin retains the complete edited image at its original pixel resolution");
    const auto pixels = actual.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const auto reference = expected.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < reference.height(); ++y)
        for (int x = 0; x < reference.width(); ++x)
            if (pixels.pixel(x, y) != reference.pixel(x, y))
                throw std::runtime_error(QStringLiteral("%1: pin pixel (%2,%3) is %4, expected %5")
                                             .arg(QString::fromUtf8(scenario))
                                             .arg(x)
                                             .arg(y)
                                             .arg(QString::number(pixels.pixel(x, y), 16),
                                                  QString::number(reference.pixel(x, y), 16))
                                             .toStdString());
}

struct PinSink {
    QVector<QImage> images;
    QVector<std::function<void(bool)>> completions;
    bool accepted = true;

    ImageEditingServices services() {
        ImageEditingServices result;
        result.pin = [this](QImage image, auto completion) {
            images.append(std::move(image));
            if (accepted)
                completions.append(std::move(completion));
            return accepted;
        };
        return result;
    }
};

QList<AdButton*> actionButtons(ScreenshotToolPalette& palette) {
    QList<AdButton*> buttons;
    for (auto* button : palette.mainPanel()->findChildren<AdButton*>()) {
        const auto items = button->property("screenshotToolbarPositionItems").toStringList();
        if (!button->isHidden() && std::any_of(items.cbegin(), items.cend(), [](const auto& id) {
                return toolbar_layout::actionDescriptor(id) != nullptr;
            }))
            buttons.append(button);
    }
    std::sort(buttons.begin(), buttons.end(), [](auto* first, auto* second) {
        return first->mapToGlobal(QPoint()).x() < second->mapToGlobal(QPoint()).x();
    });
    return buttons;
}

class PinTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* text, const char*, int) const override {
        if (QByteArray(context) == "ScreenshotToolPalette" && QByteArray(text) == "Pin to screen")
            return QStringLiteral("Translated pin");
        return {};
    }
};

void toolbarAndShortcut() {
    const snow_shot::storage::ScreenshotShortcutSettings shortcuts;
    const auto original = shortcuts.shortcuts(QStringLiteral("pin_to_screen"));
    const auto restore = qScopeGuard([&] {
        static_cast<void>(shortcuts.setShortcuts(QStringLiteral("pin_to_screen"), original));
    });
    require(shortcuts.setShortcuts(QStringLiteral("pin_to_screen"), {QStringLiteral("Alt+F")}),
            "configure a deterministic editor pin shortcut");
    PinSink sink;
    ImageEditingWindow window(sink.services());
    window.present(source());
    auto* pin =
        window.palette()->findChild<AdButton*>(QStringLiteral("screenshotPinToScreenButton"));
    require(pin && pin->accessibleName() == QStringLiteral("Pin to screen") &&
                pin->toolTip().contains(QStringLiteral("Alt+F")),
            "editor pin reuses its accessible label and configured shortcut tooltip");
    const auto verifyOrder = [&] {
        drainEvents();
        const auto buttons = actionButtons(*window.palette());
        const auto recognition = std::find_if(buttons.cbegin(), buttons.cend(), [](auto* button) {
            return button->property("screenshotToolbarPositionItems")
                .toStringList()
                .contains(QStringLiteral("text-recognition"));
        });
        require(recognition != buttons.cend() && recognition != buttons.cbegin() &&
                    *(recognition - 1) == pin,
                "pin immediately precedes the text recognition position");
    };
    verifyOrder();
    const auto kind = snow_shot::storage::ScreenshotToolbarLayoutKind::PinnedActionTools;
    auto layout = snow_shot::storage::ScreenshotToolbarSettings().layout(kind);
    // Move OCR into a grouped position to exercise rebuilding the shared pinned layout.
    for (auto& position : layout.positions)
        position.removeAll(QStringLiteral("text-recognition"));
    layout.positions.prepend(
        {QStringLiteral("text-recognition"), QStringLiteral("text-translation")});
    window.palette()->setActionToolsLayout(layout);
    verifyOrder();
    for (auto& position : layout.positions)
        position.removeAll(QStringLiteral("text-recognition"));
    layout.hidden.append(QStringLiteral("text-recognition"));
    window.palette()->setActionToolsLayout(layout);
    drainEvents();
    const auto hiddenOcrButtons = actionButtons(*window.palette());
    if (hiddenOcrButtons.first() != pin) {
        QStringList positions;
        for (auto* button : hiddenOcrButtons)
            positions.append(QStringLiteral("%1:%2@%3")
                                 .arg(button->accessibleName(),
                                      button->property("screenshotToolbarPositionItems")
                                          .toStringList()
                                          .join(QLatin1Char(',')))
                                 .arg(button->mapToGlobal(QPoint()).x()));
        throw std::runtime_error(QStringLiteral("pin must be first when OCR is hidden: %1")
                                     .arg(positions.join(QLatin1Char(';')))
                                     .toStdString());
    }

    PinTranslator translator;
    QApplication::installTranslator(&translator);
    drainEvents();
    require(pin->accessibleName() == QStringLiteral("Translated pin") &&
                pin->toolTip().contains(QStringLiteral("Translated pin")),
            "pin accessibility and tooltip follow runtime language changes");
    QApplication::removeTranslator(&translator);

    window.canvas()->window()->activateWindow();
    window.canvas()->setFocus();
    drainEvents();
    for (const auto type : {QEvent::ShortcutOverride, QEvent::KeyPress, QEvent::KeyRelease}) {
        PhysicalKeyEvent event(type, Qt::Key_F, Qt::AltModifier);
        QApplication::sendEvent(window.canvas(), &event);
    }
    awaitCondition([&] { return sink.images.size() == 1; }, "configured shortcut pins once");
    sink.completions.last()(true);
    pin->click();
    awaitCondition([&] { return sink.images.size() == 2; }, "button dispatches the next pin once");
    sink.completions.last()(true);
    require(window.modal()->isOpen(), "successful pinning leaves Image Editing open");
    window.shutdown();

    ScreenshotToolPalette pinned(screenshotDrawingToolbarOptions());
    require(!pinned.findChild<AdButton*>(QStringLiteral("screenshotPinToScreenButton")),
            "ordinary pinned-image editing does not gain another pin control");
}

void snapshotAndCheckpoints() {
    PinSink sink;
    ImageEditingWindow window(sink.services());
    const auto original = source();
    window.present(original);
    require(window.document()->crop(Axis::Rows, 3, 9), "crop rows before pinning");
    addAnnotation(window);
    const QImage expected = rendered(window);
    require(expected.pixelColor(35, 40).red() == 255 && expected.pixelColor(0, 0).alpha() == 0,
            "pin fixture contains an annotation and transparent pixels");
    window.canvas()->actualSize();
    int exports = 0;
    bool succeeded = false;
    QObject::connect(&window, &ImageEditingWindow::exportFinished, &window, [&](bool success) {
        ++exports;
        succeeded = success;
    });
    window.palette()->pinRequested();
    window.palette()->pinRequested();
    awaitCondition([&] { return sink.images.size() == 1; }, "edited pin dispatches");
    require(exports == 0, "pin stays pending until the pinning service completes");
    requirePixels(sink.images.first(), expected, "cropped annotations");
    require(window.document()->crop(Axis::Columns, 5, 11),
            "make a later edit while pin is pending");
    window.palette()->pinRequested();
    sink.completions.first()(true);
    require(exports == 1 && succeeded && window.dirty() && window.modal()->isOpen(),
            "success saves only the captured checkpoint and keeps later changes unsaved");
    require(sink.images.size() == 1 && window.document()->undo() && !window.dirty(),
            "duplicate requests are ignored and undo returns to the exported checkpoint");
    require(original.image == source().image, "pinning preserves the imported source image");
    sink.completions.first()(false);
    require(exports == 1, "a repeated service callback cannot complete an export twice");
    window.palette()->pinRequested();
    awaitCondition([&] { return sink.images.size() == 2; },
                   "a subsequent activation makes a new pin");
    sink.completions.last()(true);
    require(exports == 2 && !window.dirty(), "each successful pin completes its own export");
    window.shutdown();
}

void pendingTextAndRecognition() {
    PinSink sink;
    ImageEditingWindow window(sink.services());
    window.present(source());
    window.canvas()->setMode(ImageEditingCanvas::Mode::Draw);
    require(window.canvas()->setCanvasTool(SnowCanvasTool::Text), "activate image text tool");
    const QPointF point = window.canvas()->canvasToViewTransform().map(QPointF(80, 70));
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, point, window.canvas()->mapToGlobal(point), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(window.canvas(), &event);
    }
    require(window.canvas()->hasActiveTextEditing(), "image text is pending before pinning");
    QKeyEvent input(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("Pending text"));
    QApplication::sendEvent(window.canvas(), &input);
    const QImage withoutText = window.document()->image();
    window.palette()->pinRequested();
    require(!window.canvas()->hasActiveTextEditing(), "pin commits pending canvas text");
    const auto expectedText = rendered(window);
    require(expectedText != withoutText, "committed text contributes visible image pixels");
    awaitCondition([&] { return sink.images.size() == 1; }, "pending text pin renders");
    requirePixels(sink.images.last(), expectedText, "pending canvas text");
    sink.completions.last()(true);
    window.shutdown();

    auto recognized = source();
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = recognized.canvasRect.toRect();
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Recognized text");
    line.quad = {QPointF(20, 30), QPointF(100, 30), QPointF(100, 50), QPointF(20, 50)};
    presentation->lines.append(line);
    presentation->prepareForRendering();
    recognized.recognition.key = QStringLiteral("image-editor-pin-recognition");
    recognized.recognition.text = ScreenshotOcrRecognitionResult{presentation};
    ImageEditingWindow recognition(sink.services());
    recognition.present(recognized);
    addAnnotation(recognition);
    const auto expectedRecognition = rendered(recognition);
    recognition.palette()->ocrRequested();
    auto* session = recognition.recognition();
    require(session && session->active(), "cached OCR is displayed before pinning");
    session->setTextDraft(QStringLiteral("Unsaved recognition draft"));
    recognition.palette()->pinRequested();
    awaitCondition([&] { return sink.images.size() == 2; }, "pin renders with recognition visible");
    requirePixels(sink.images.last(), expectedRecognition, "active recognition");
    sink.completions.last()(true);
    require(recognition.dirty() && !recognition.document()->dirty() &&
                recognition.recognition()->active() &&
                recognition.recognition()->textDraft() ==
                    QStringLiteral("Unsaved recognition draft"),
            "image pinning preserves the unsaved recognition result and its visible session");
    recognition.shutdown();
}

void sourceRecordsRemainIndependent() {
    using namespace snow_shot::storage;
    QTemporaryDir directory;
    require(directory.isValid(), "history pin fixture has isolated storage");
    auto repository = makeCaptureHistoryRepository(directory.path());
    const auto original = source();
    CaptureHistoryDraft draft;
    draft.id = QStringLiteral("a12ee4e6-cb16-4f21-9516-6837541c16ec");
    draft.createdUtc = QDateTime::currentDateTimeUtc();
    draft.canvasBounds = original.canvasRect.toRect();
    draft.selection.rectangle = draft.canvasBounds;
    draft.selection.shadowColor = Qt::transparent;
    draft.canvasHistory = SnowCanvasRuntime().serializeDocumentHistory();
    CaptureHistoryDisplayDraft display;
    display.stableId = draft.id;
    display.name = draft.id;
    display.sourceCanvasRect = draft.canvasBounds;
    display.image = original.image;
    draft.displays.append(std::move(display));
    const auto published = repository->publish(std::move(draft)).get();
    require(published.storage.success, "publish the history source for pinning");
    const auto revision = repository->recordsSnapshot().revision;

    PinnedWindowGroupManager groups;
    ScreenshotSelectionExportUiServices destination(nullptr, nullptr, nullptr, {}, {}, &groups);
    const auto closePins = qScopeGuard([&] {
        for (auto* pin : groups.liveWindows())
            pin->close();
        drainEvents();
    });
    ImageEditingServices services;
    services.pin = [&](QImage image, auto completion) {
        return destination.presentPinnedImage(
            image, QGuiApplication::primaryScreen(), QRect(QPoint(40, 40), image.size()),
            image.size(), {}, {}, 1.0, {}, {},
            [completion = std::move(completion)](bool success, QImage) { completion(success); });
    };
    ImageEditingWindow window(std::move(services));
    int loads = 0;
    QObject::connect(&window, &ImageEditingWindow::loaded, &window, [&](bool success) {
        require(success, "source working copy loads successfully");
        ++loads;
    });
    window.present(readImageEditingHistory(*repository, published.record));
    awaitCondition([&] { return loads == 1; }, "load history working copy");
    require(window.document()->crop(Axis::Rows, 3, 9), "edit the history working copy");
    const QImage expected = rendered(window);
    int exports = 0;
    QObject::connect(&window, &ImageEditingWindow::exportFinished, &window, [&](bool success) {
        require(success, "normal pin presentation succeeds");
        ++exports;
    });
    window.palette()->pinRequested();
    awaitCondition([&] { return exports == 1; }, "history working copy creates a live pin");
    require(groups.liveWindows().size() == 1, "pinning creates one independent pinned window");
    auto* firstPin = groups.liveWindows().first();
    const auto before = firstPin->persistenceSnapshot();
    requirePixels(before.image, expected, "live pin presentation");

    window.present(firstPin->imageEditingSnapshot());
    awaitCondition([&] { return loads == 2; }, "load the pin working copy");
    require(window.document()->crop(Axis::Columns, 5, 11), "edit the pin working copy");
    window.palette()->pinRequested();
    awaitCondition([&] { return exports == 2; }, "pin working copy creates another live pin");
    const auto after = firstPin->persistenceSnapshot();
    require(groups.liveWindows().size() == 2 && before.id == after.id &&
                before.image == after.image && before.canvasSession == after.canvasSession,
            "pinning a working copy creates a separate pin without modifying its source pin");
    require(repository->recordsSnapshot().revision == revision &&
                repository->recordsSnapshot().records.size() == 1,
            "pinning leaves the source history entry unchanged");
    window.shutdown();
}

void failureAndLifetime() {
    PinSink sink;
    sink.accepted = false;
    auto window = std::make_unique<ImageEditingWindow>(sink.services());
    window->palette()->pinRequested();
    require(sink.images.isEmpty(), "an editor without an image cannot pin");
    window->present(source());
    require(window->document()->crop(Axis::Rows, 3, 9), "failure fixture starts dirty");
    int exports = 0;
    QObject receiver;
    QObject::connect(window.get(), &ImageEditingWindow::exportFinished, &receiver,
                     [&](bool success) {
                         require(!success, "failed pinning reports failure");
                         ++exports;
                     });
    window->palette()->pinRequested();
    awaitCondition([&] { return exports == 1; }, "rejected service request completes");
    require(window->dirty() && window->modal()->isOpen(), "rejection preserves editable changes");
    sink.accepted = true;
    window->palette()->pinRequested();
    awaitCondition([&] { return sink.completions.size() == 1; }, "pin retries after rejection");
    sink.completions.last()(false);
    require(exports == 2 && window->dirty(), "asynchronous failure keeps the document unsaved");
    window->palette()->pinRequested();
    awaitCondition([&] { return sink.completions.size() == 2; }, "pin retries after failure");
    window.reset();
    sink.completions.last()(true);
    drainEvents();
    require(exports == 2, "late pin completion cannot reach a destroyed editor");

    ImageEditingWindow cancelled(sink.services());
    cancelled.present(source());
    cancelled.palette()->pinRequested();
    cancelled.shutdown();
    drainEvents();
    require(sink.images.size() == 3, "shutdown before rendering prevents pin dispatch");

    using Exporter = ImageEditingExportController;
    QWidget owner;
    Exporter exporter(nullptr, &owner, sink.services().pin);
    int failures = 0;
    exporter.start(Exporter::Operation::Pin, ScreenshotExportSource{}, &owner, nullptr,
                   [&](bool success, const QString&) {
                       require(!success, "invalid image pin fails rendering");
                       ++failures;
                   });
    awaitCondition([&] { return failures == 1; }, "failed rendering finishes the pin request");
    require(sink.images.size() == 3 && !exporter.busy(),
            "failed rendering never calls pin service");

    ImageEditingServices synchronous;
    synchronous.pin = [](QImage, auto completion) {
        completion(true);
        return true;
    };
    window = std::make_unique<ImageEditingWindow>(std::move(synchronous));
    window->present(source());
    QPointer<ImageEditingWindow> lifetime(window.get());
    QObject::connect(window.get(), &ImageEditingWindow::exportFinished, &receiver,
                     [&](bool success) {
                         require(success, "synchronous pin service succeeds");
                         ++exports;
                         window.reset();
                     });
    window->palette()->pinRequested();
    awaitCondition([&] { return !lifetime; },
                   "pin completion can synchronously destroy the editor");
    require(exports == 3, "reentrant destruction completes pinning once");
}
} // namespace

void runImageEditingPinTests() {
    toolbarAndShortcut();
    snapshotAndCheckpoints();
    pendingTextAndRecognition();
    sourceRecordsRemainIndependent();
    failureAndLifetime();
}
