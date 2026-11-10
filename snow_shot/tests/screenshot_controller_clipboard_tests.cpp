// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas_automatic_selection_test_support.h"
#include "screenshot_controller_test_access.h"
#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/app/shellimagelaunch.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/presentation/screenshotselectionsettingsstore.h"
#include "snow_shot/presentation/screenshotselectionexportuiservices.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/collagewindow.h"
#include "snow_shot/presentation/imagesplitwindow.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "widgets/modal.h"
#include "snow_shot/presentation/screenshotsavepreviewcanvas.h"
#include "snow_shot/presentation/screenshotscrollingsnapshot.h"
#include "snow_stitch_images.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../src/presentation/pinned/screenshotclipboardplacementgeometry.h"
#include "snowimageqtcodec.h"

#include "widgets/platform_compatibility.h"

#include <QApplication>
#include <QClipboard>
#include <QColorSpace>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QMimeData>
#include <QMouseEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QInputMethodEvent>
#include <QPainter>
#include <QScopeGuard>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void waitUntil(const std::function<bool()>& ready, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 10000) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(ready(), message);
}

QList<ScreenshotPinnedWindow*> visiblePins() {
    QList<ScreenshotPinnedWindow*> result;
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* pin = qobject_cast<ScreenshotPinnedWindow*>(widget);
        if (pin && pin->isVisible() &&
            pin->automationState().value(QStringLiteral("ready")).toBool() &&
            !pin->persistenceSnapshot().image.isNull())
            result.append(pin);
    }
    return result;
}

void closePins() {
    for (auto* pin : visiblePins())
        pin->close();
    waitUntil([] { return visiblePins().isEmpty(); }, "clipboard pins must close");
}

QImage fixture() {
    QImage image(120, 80, QImage::Format_RGBA8888);
    image.fill(QColor(23, 117, 209, 127));
    image.setPixelColor(0, 0, QColor(201, 19, 79, 43));
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

ScreenshotOcrRecognitionService::Options isolatedRecognitionOptions(const QString& directory) {
    ScreenshotOcrRecognitionService::Options options;
    // Explicit local paths keep an unexpected recognition request away from downloads.
    options.processPath = QDir(directory).filePath(QStringLiteral("missing-ocr-worker.exe"));
    options.detectorModelPath = QDir(directory).filePath(QStringLiteral("missing-detector.onnx"));
    options.recognizerModelPath =
        QDir(directory).filePath(QStringLiteral("missing-recognizer.onnx"));
    options.dictionaryPath = QDir(directory).filePath(QStringLiteral("missing-dictionary.txt"));
    return options;
}

void captureActionsChooseToolBeforeFirstPresentation(const QString& directory) {
    const snow_shot::storage::ScreenshotToolbarSettings toolbarSettings;
    const snow_shot::storage::DrawingSettings drawingSettings;
    const auto originalTool = toolbarSettings.lastDrawingTool();
    const bool originalRemember = drawingSettings.rememberLastUsedTool();
    const auto restoreSettings = qScopeGuard([&] {
        static_cast<void>(toolbarSettings.setLastDrawingTool(originalTool));
        static_cast<void>(drawingSettings.setRememberLastUsedTool(originalRemember));
    });
    require(drawingSettings.setRememberLastUsedTool(true), "enable remembered drawing tools");
    using Action = ScreenshotController::CaptureAction;
    for (const auto action : {Action::None, Action::RecognizeText, Action::RecognizeTextAndCopy,
                              Action::RecognizeTextTranslation}) {
        require(toolbarSettings.setLastDrawingTool(QStringLiteral("shape")),
                "seed a competing remembered tool for every capture action");
        ScreenshotOcrRecognitionService recognition(isolatedRecognitionOptions(directory));
        ScreenshotController controller(nullptr, nullptr, &recognition);
        ScreenshotSelectionParams selection;
        selection.selection = QRect(10, 10, 60, 40);
        ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
        ScreenshotControllerTestAccess::presentSelection(controller);
        const QString expected = action == Action::None ? QStringLiteral("rectangle")
                                 : action == Action::RecognizeTextTranslation
                                     ? QStringLiteral("text_translation")
                                     : QStringLiteral("ocr");
        struct ToolbarObserver final : QObject {
            ScreenshotController& controller;
            QString expected;
            int presentations = 0;
            ToolbarObserver(ScreenshotController& capture, QString tool)
                : controller(capture), expected(std::move(tool)) {}
            bool eventFilter(QObject* watched, QEvent* event) override {
                if (event->type() == QEvent::Show &&
                    qobject_cast<ScreenshotToolbarWindow*>(watched)) {
                    ++presentations;
                    require(controller.mcpState().value(QStringLiteral("active_tool")).toString() ==
                                expected,
                            "the first toolbar presentation must already use the capture tool");
                }
                return false;
            }
        } observer(controller, expected);
        qApp->installEventFilter(&observer);
        ScreenshotControllerTestAccess::confirmCaptureSelection(controller, action);
        qApp->removeEventFilter(&observer);
        require(observer.presentations == 1 &&
                    controller.mcpState().value(QStringLiteral("active_tool")).toString() ==
                        expected,
                "capture actions and remembered tools must activate synchronously exactly once");
        controller.mcpCancelCapture();
    }
}

void queuedRecognitionDoesNotOverrideReselection(const QString& directory) {
    ScreenshotOcrRecognitionService recognition(isolatedRecognitionOptions(directory));
    ScreenshotSelectionParams selection;
    selection.selection = QRect(10, 10, 60, 40);

    for (int scenario = 0; scenario < 5; ++scenario) {
        ScreenshotController controller(nullptr, nullptr, &recognition);
        ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
        QString error;
        require(controller.mcpSetTool(QStringLiteral("ocr"), &error),
                "queue recognition activation after selection confirmation");
        require(controller.mcpState().value(QStringLiteral("active_tool")).toString() ==
                    QStringLiteral("move"),
                "recognition restoration waits until the input event finishes");
        if (scenario == 0) {
            ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
        } else if (scenario == 1) {
            controller.mcpCancelCapture();
            ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
        } else {
            if (scenario == 3) {
                require(controller.mcpSetTool(QStringLiteral("table"), &error),
                        "a later recognition command replaces the queued tool");
            }
            require(controller.mcpSetTool(QStringLiteral("move"), &error),
                    "an explicit Edit selection command supersedes queued recognition");
        }
        bool dispatched = false;
        QTimer::singleShot(0, &controller, [&] { dispatched = true; });
        waitUntil([&] { return dispatched; }, "dispatch queued recognition restoration");
        require(controller.mcpState().value(QStringLiteral("active_tool")).toString() ==
                    QStringLiteral("move"),
                "stale recognition restoration cannot override selection or explicit tool changes");
        require(!recognition.storageBusy() && recognition.liveWorkerCount() == 0,
                "discarded recognition restoration must not request model work");
    }
}

QPointer<ScreenshotRecognitionWindow> visibleRecognitionWindow() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<ScreenshotRecognitionWindow*>(widget);
            window != nullptr && window->isVisible()) {
            return window;
        }
    }
    return {};
}

void sendMouse(QWidget& target, QEvent::Type type, const QPointF& point, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, target.mapToGlobal(point), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&target, &event);
}

void recognitionWindowsFollowSelectionTransitions(const QString& directory) {
    const snow_shot::storage::ScreenshotSettings settings;
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    require(settings.setAutoRecognizeQrCode(false),
            "recognition transition tests must not run automatic QR recognition");
#endif
    require(settings.setAutoExecuteAfterTextRecognition(QStringLiteral("no_action")),
            "recognition transition tests must not run automatic follow-up commands");
    for (const QString& tool : {QStringLiteral("ocr"), QStringLiteral("text_translation")}) {
        for (const bool resize : {false, true}) {
            ScreenshotOcrRecognitionService recognition(isolatedRecognitionOptions(directory));
            ScreenshotController controller(nullptr, nullptr, &recognition);
            QImage image(240, 180, QImage::Format_RGB32);
            image.fill(Qt::white);
            ScreenshotSelectionParams params;
            params.selection = QRect(40, 40, 120, 100);
            ScreenshotControllerTestAccess::prepareSelection(controller, image, params);
            auto* overlay = ScreenshotControllerTestAccess::presentSelection(controller);
            require(overlay != nullptr && overlay->isVisible(), "present the capture overlay");
            QString error;
            const auto activeTool = [&] {
                return controller.mcpState().value(QStringLiteral("active_tool")).toString();
            };
            require(controller.mcpSetTool(tool, &error), "activate recognition for selection");
            waitUntil([&] { return activeTool() == tool && visibleRecognitionWindow(); },
                      "recognition must present an input-bearing window");
            const QPointer<ScreenshotRecognitionWindow> original = visibleRecognitionWindow();
            if (resize) {
                const QPointF edge(original->width() - 1, original->height() / 2);
                sendMouse(*original, QEvent::MouseButtonPress, edge, Qt::LeftButton,
                          Qt::LeftButton);
                require(original && original->isVisible() && activeTool() == QStringLiteral("move"),
                        "a resize must retain its recognition event receiver until release");
                sendMouse(*original, QEvent::MouseMove, edge + QPointF(20, 0), Qt::NoButton,
                          Qt::LeftButton);
                require(original && original->isVisible(),
                        "the recognition resize receiver must survive pointer movement");
                sendMouse(*original, QEvent::MouseButtonRelease, edge + QPointF(20, 0),
                          Qt::LeftButton, Qt::NoButton);
            } else {
                auto* canvas = overlay->canvas();
                sendMouse(*canvas, QEvent::MouseButtonPress, {200, 160}, Qt::RightButton,
                          Qt::RightButton);
                sendMouse(*canvas, QEvent::MouseButtonRelease, {200, 160}, Qt::RightButton,
                          Qt::NoButton);
                require(!original || !original->isVisible(),
                        "reselection must retire the old recognition input surface");
                require(activeTool() == QStringLiteral("move") &&
                            controller.mcpState()
                                    .value(QStringLiteral("selection"))
                                    .toObject()
                                    .value(QStringLiteral("bounds"))
                                    .toArray() == QJsonArray{0, 0, 0, 0},
                        "outside right click must enter empty selection mode");
                QCoreApplication::processEvents();
                QWidget* receiver = QApplication::widgetAt(canvas->mapToGlobal(QPoint(60, 60)));
                require(receiver != nullptr && receiver->window() == overlay,
                        "the previous capture area must receive reselection input");
                sendMouse(*canvas, QEvent::MouseButtonPress, {60, 60}, Qt::LeftButton,
                          Qt::LeftButton);
                sendMouse(*canvas, QEvent::MouseMove, {99, 89}, Qt::NoButton, Qt::LeftButton);
                sendMouse(*canvas, QEvent::MouseButtonRelease, {99, 89}, Qt::LeftButton,
                          Qt::NoButton);
            }
            require(activeTool() == tool && visibleRecognitionWindow(),
                    "selection completion must restore recognition before returning to the event "
                    "loop");
            const auto bounds = controller.mcpState()
                                    .value(QStringLiteral("selection"))
                                    .toObject()
                                    .value(QStringLiteral("bounds"))
                                    .toArray();
            require(resize ? bounds.at(2).toInt() > 120 : bounds == QJsonArray{60, 60, 40, 30},
                    "the replacement selection must keep the completed gesture geometry");
            const QPointer<ScreenshotRecognitionWindow> restored = visibleRecognitionWindow();
            require(controller.mcpSetTool(QStringLiteral("move"), &error) &&
                        (!restored || !restored->isVisible()) && !visibleRecognitionWindow(),
                    "an explicit Move command must retire recognition rather than suspend it");
            controller.mcpCancelCapture();
        }
    }
}

std::pair<QImage, QString> captureClipboardOutput() {
#ifdef Q_OS_WIN
    waitUntil([] { return OpenClipboard(nullptr) != FALSE; }, "open native clipboard output");
    const auto close = qScopeGuard([] { CloseClipboard(); });
    const auto handle = static_cast<HGLOBAL>(GetClipboardData(RegisterClipboardFormatW(L"PNG")));
    const auto* data = handle ? static_cast<const char*>(GlobalLock(handle)) : nullptr;
    QImage image;
    if (data) {
        image =
            QImage::fromData(QByteArray(data, static_cast<qsizetype>(GlobalSize(handle))), "PNG");
        GlobalUnlock(handle);
    }
    QString path;
    const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (drop && DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) > 0) {
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        path.resize(length + 1);
        DragQueryFileW(drop, 0, reinterpret_cast<LPWSTR>(path.data()), length + 1);
        path.resize(length);
    }
    return {image, path};
#else
    const auto* mime = QApplication::clipboard()->mimeData();
    return {QImage::fromData(mime->data(QStringLiteral("image/png"))),
            mime->hasUrls() ? mime->urls().constFirst().toLocalFile() : QString{}};
#endif
}

void combinedCopiesPreserveImagePlacementAndAppearance(ScreenshotController& controller) {
    const QImage image = fixture();
    const QRect geometry(40, 60, image.width(), image.height());
    const auto placement = screenshotClipboardSelectionPlacement(geometry, image.size(),
                                                                 QGuiApplication::primaryScreen());
    require(placement.has_value(), "clipboard placement fixture must be valid");
    ScreenshotClipboardAppearance appearance;
    appearance.rasterSize = image.size();
    appearance.checkerboardEnabled = true;
    appearance.showBorder = false;
    appearance.showShadow = true;
    appearance.borderAppearance = snow_shot::storage::PinnedBorderAppearance{
        image.size(), QRectF(4, 4, image.width() - 8, image.height() - 8), 6, true, {}};
    QObject receiver;
    for (const auto format : {ScreenshotImageFileFormat::Png, ScreenshotImageFileFormat::Jpeg,
                              ScreenshotImageFileFormat::Pdf}) {
        for (bool removeFile : {false, true}) {
            ScreenshotExportArtifact artifact(
                ScreenshotExportSource::fromImage(image, placement, appearance));
            std::optional<ScreenshotExportClipboardResult> prepared;
            require(artifact.requestClipboardFile(&receiver, format,
                                                  QStringLiteral("clipboard-pin"), {},
                                                  [&](ScreenshotExportClipboardResult result) {
                                                      prepared.emplace(std::move(result));
                                                  }),
                    "combined clipboard export must start");
            waitUntil([&] { return prepared.has_value(); }, "combined export must complete");
            require(prepared->succeeded(), "combined clipboard export must succeed");
            const QString path = prepared->payload.filePath();
            const auto cleanup = qScopeGuard(
                [&] { static_cast<void>(QDir(QFileInfo(path).path()).removeRecursively()); });
            if (removeFile)
                require(QFile::remove(path), "exported clipboard file must be removable");

            // Publish the same representations through Qt so this exercises the real controller
            // without requiring the native desktop clipboard in offscreen Windows tests.
            auto* mime = new QMimeData;
            mime->setData(QStringLiteral("image/png"), prepared->payload.pngBytes());
            mime->setData(screenshotClipboardPlacementNativeMimeType(),
                          prepared->payload.placementBytes());
            mime->setData(screenshotClipboardAppearanceNativeMimeType(),
                          prepared->payload.appearanceBytes());
            mime->setUrls({QUrl::fromLocalFile(path)});
            QApplication::clipboard()->setMimeData(mime);
            controller.pinClipboardContentToScreen();
            const auto scenario = QStringLiteral("combined copy must create a pin (format=%1, "
                                                 "removed=%2)")
                                      .arg(static_cast<int>(format))
                                      .arg(removeFile)
                                      .toUtf8();
            waitUntil([] { return visiblePins().size() == 1; }, scenario.constData());
            const auto record = visiblePins().constFirst()->persistenceSnapshot();
            require(record.nativeGeometry == geometry && record.initialWindowSize == image.size(),
                    "combined clipboard pin must restore its image placement");
            require(record.image.convertToFormat(QImage::Format_RGBA8888) == image &&
                        record.sourceKind ==
                            snow_shot::storage::PinnedWindowSourceKind::ImageData &&
                        record.originalFilePath.isEmpty(),
                    "combined clipboard pin must use the lossless image independently of its file");
            require(record.borderAppearance == appearance.borderAppearance &&
                        record.checkerboardEnabled == appearance.checkerboardEnabled &&
                        !record.showBorder && record.showShadow,
                    "combined clipboard pin must restore all image appearance settings");
            closePins();
        }
    }
}

void fileOnlyClipboardStillPinsEveryFile(ScreenshotController& controller) {
    QTemporaryDir directory;
    require(directory.isValid(), "file-only clipboard fixtures need temporary storage");
    const QString first = directory.filePath(QStringLiteral("first.png"));
    const QString second = directory.filePath(QStringLiteral("second.png"));
    require(fixture().save(first, "PNG") && fixture().save(second, "PNG"),
            "file-only clipboard fixtures must be saved");
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)});
    QApplication::clipboard()->setMimeData(mime);
    controller.pinClipboardContentToScreen();
    waitUntil([] { return visiblePins().size() == 2; }, "file-only clipboard must pin both files");
    QStringList paths;
    for (auto* pin : visiblePins()) {
        const auto record = pin->persistenceSnapshot();
        require(record.sourceKind == snow_shot::storage::PinnedWindowSourceKind::ClipboardImageFile,
                "file-only clipboard must retain file provenance");
        paths.append(record.originalFilePath);
    }
    require(paths.contains(first) && paths.contains(second),
            "batch import must preserve both paths");
    closePins();
}

#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
void clipboardFormulasPinAsImages(ScreenshotController& controller) {
    const auto samePixels = [](const QImage& first, const QImage& second) {
        if (first.isNull() || second.isNull() || first.size() != second.size())
            return false;
        // PNG decoding adds an explicit sRGB tag. Assert the pixel contents independently
        // of color metadata, as in the pinned-window repository's round-trip fixtures.
        const auto firstArgb = first.convertToFormat(QImage::Format_ARGB32);
        const auto secondArgb = second.convertToFormat(QImage::Format_ARGB32);
        const auto rowBytes = static_cast<std::size_t>(first.width()) * sizeof(QRgb);
        for (int row = 0; row < first.height(); ++row) {
            if (std::memcmp(firstArgb.constScanLine(row), secondArgb.constScanLine(row),
                            rowBytes) != 0)
                return false;
        }
        return true;
    };
    const snow_shot::storage::PinToScreenSettings settings;
    const auto previousAction = settings.duplicateContentAction();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(settings.setDuplicateContentAction(previousAction)); });
    require(settings.setDuplicateContentAction(QStringLiteral("none")),
            "formula pin duplicate policy must be deterministic");
    auto* clipboard = QApplication::clipboard();
    struct BeforeFormulaCompletion final : QObject {
        ScreenshotController& controller;
        std::function<void()> action;
        bool fired = false;
        explicit BeforeFormulaCompletion(ScreenshotController& owner) : controller(owner) {}
        bool eventFilter(QObject* watched, QEvent* event) override {
            if (watched == qApp && event->type() == QEvent::MetaCall && !fired &&
                ScreenshotControllerTestAccess::clipboardFormulaPending(controller)) {
                fired = true;
                action();
            }
            return false;
        }
    } observer(controller);
    observer.action = [&] {
        require(controller.blocksApplicationUpdate() && controller.blocksMemoryTrimming(),
                "pending formula rendering participates in application busy state");
        const auto generation = ScreenshotControllerTestAccess::clipboardPinGeneration(controller);
        controller.pinClipboardContentToScreen();
        require(ScreenshotControllerTestAccess::clipboardPinGeneration(controller) == generation,
                "a duplicate action must not cancel or replace the in-flight formula request");
    };
    qApp->installEventFilter(&observer);
    clipboard->setText(QStringLiteral("\\[\\frac{x^2}{\\sqrt{y}}\\]"));
    controller.pinClipboardContentToScreen();
    waitUntil([] { return visiblePins().size() == 1; },
              "clipboard formula must create an image pin");
    qApp->removeEventFilter(&observer);
    require(observer.fired, "duplicate pin action must be exercised during formula rendering");
    const auto record = visiblePins().first()->persistenceSnapshot();
    require(record.sourceKind == snow_shot::storage::PinnedWindowSourceKind::ImageData &&
                record.originalText.isEmpty() && record.originalHtml.isEmpty() &&
                record.image.pixelColor(0, 0) == QColor(Qt::white),
            "formula pins must persist as opaque images without a text source");
    QTemporaryDir directory;
    require(directory.isValid(), "formula image persistence needs isolated storage");
    {
        snow_shot::storage::PinnedWindowRepository repository(directory.path());
        require(repository.upsert(record).success && repository.flush().success,
                "formula image must save through the normal image repository");
    }
    {
        snow_shot::storage::PinnedWindowRepository repository(directory.path());
        const auto restored = repository.loadRecord(record.id);
        require(restored && restored->sourceKind == record.sourceKind &&
                    samePixels(restored->image, record.image),
                "formula image pixels must survive repository restart");
    }
    const auto exported = directory.filePath(QStringLiteral("formula.png"));
    require(record.image.save(exported, "PNG") && samePixels(QImage(exported), record.image),
            "formula images must export without losing their rendered pixels");
    closePins();

    clipboard->setText(QStringLiteral("$x") + QString(32000, QLatin1Char('\n')) +
                       QStringLiteral("$"));
    controller.pinClipboardContentToScreen();
    waitUntil([] { return visiblePins().size() == 1; },
              "formula pins must not depend on rendering an oversized text fallback");
    require(visiblePins().first()->persistenceSnapshot().sourceKind ==
                    snow_shot::storage::PinnedWindowSourceKind::ImageData &&
                visiblePins().first()->persistenceSnapshot().originalText.isEmpty(),
            "the controller must attempt a compact formula before laying out its source text");
    closePins();

    // Replace the clipboard while the old formula result is waiting on the GUI queue.
    observer.fired = false;
    observer.action = [&] {
        clipboard->setImage(fixture());
        controller.pinClipboardContentToScreen();
    };
    qApp->installEventFilter(&observer);
    clipboard->setText(QStringLiteral("$\\alpha+\\beta$"));
    controller.pinClipboardContentToScreen();
    waitUntil([] { return visiblePins().size() == 1; },
              "superseding clipboard image must be pinned");
    qApp->removeEventFilter(&observer);
    require(observer.fired && visiblePins().first()->persistenceSnapshot().image.convertToFormat(
                                  QImage::Format_RGBA8888) == fixture(),
            "a superseded formula must never publish over a newer clipboard request");
    closePins();

    auto* mime = new QMimeData;
    mime->setImageData(fixture());
    mime->setText(QStringLiteral("$x^2$"));
    clipboard->setMimeData(mime);
    controller.pinClipboardContentToScreen();
    waitUntil([] { return visiblePins().size() == 1; },
              "clipboard image must outrank formula text");
    require(visiblePins().first()->persistenceSnapshot().image.convertToFormat(
                QImage::Format_RGBA8888) == fixture(),
            "formula recognition must preserve inline image precedence");
    closePins();

    clipboard->setText(QStringLiteral("$\\snowUndefined$"));
    controller.pinClipboardContentToScreen();
    QPointer<ScreenshotPinnedWindow> fallback;
    waitUntil(
        [&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (auto* pin = qobject_cast<ScreenshotPinnedWindow*>(widget);
                    pin && pin->isVisible()) {
                    if (pin->persistenceSnapshot().originalText ==
                        QStringLiteral("$\\snowUndefined$")) {
                        fallback = pin;
                        return true;
                    }
                }
            }
            return false;
        },
        "unsupported LaTeX must fall back to the original text pin");
    require(fallback->persistenceSnapshot().sourceKind ==
                snow_shot::storage::PinnedWindowSourceKind::ClipboardText,
            "invalid formula source must preserve ordinary text persistence");
    fallback->close();
}
#endif

void imageToolsPreferInlineClipboardPixels() {
    const QImage image = fixture();
    QImage fileImage(image.size(), image.format());
    fileImage.fill(Qt::red);
    QTemporaryDir directory;
    require(directory.isValid(), "image tool clipboard fixture needs temporary storage");
    const QString path = directory.filePath(QStringLiteral("export.png"));
    require(fileImage.save(path, "PNG"), "image tool file fixture must be saved");
    auto payload = ScreenshotClipboardService::prepareImage(image);
    require(payload.isValid(), "image tool clipboard fixture must encode");
    auto* mime = new QMimeData;
    mime->setData(QStringLiteral("image/png"), payload.pngBytes());
    mime->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(mime);
    const auto samePixels = [&image](const QImage& actual) {
        return actual.convertToFormat(QImage::Format_ARGB32_Premultiplied) ==
               image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    };
    {
        snow_shot::presentation::ImageStitchingWindow imageStitching;
        imageStitching.present();
        imageStitching.paste();
        waitUntil(
            [&] {
                return !imageStitching.loading() && imageStitching.document()->items().size() == 1;
            },
            "collage must import the combined clipboard");
        require(samePixels(imageStitching.document()->items().constFirst().image),
                "collage must use inline pixels instead of the exported file");
        imageStitching.shutdown();
    }
    {
        snow_shot::presentation::ImageSplitWindowServices services;
        services.confirm = [](QWidget*, bool, auto complete) { complete(true); };
        snow_shot::presentation::ImageSplitWindow split(nullptr, services);
        split.presentImage(fileImage);
        waitUntil([&] { return !split.loading() && !split.document()->image().isNull(); },
                  "image splitting must load its initial fixture");
        split.paste();
        waitUntil([&] { return !split.loading(); },
                  "image splitting must finish its clipboard replacement");
        require(samePixels(split.document()->image()),
                "image splitting must use inline pixels instead of the exported file");
        split.shutdown();
    }
}

QByteArray editablePinRectangle(int x, int y, const QColor& color) {
    return QStringLiteral(
               R"({"version":1,"operations":[{"type":"rectangle","bounds":[%1,%2,18,14],"style":{"fill":[%3,%4,%5,255],"stroke":[0,0,0,0]}}]})")
        .arg(x)
        .arg(y)
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .toUtf8();
}

QImage editablePinImage(ScreenshotPinnedWindow& pin) {
    QImage image;
    bool done = false;
    const auto artifact = pin.automationArtifact();
    require(artifact &&
                artifact->requestImage(&pin,
                                       [&](ScreenshotExportImageResult result) {
                                           require(result.succeeded(),
                                                   "editable pin composite export succeeds");
                                           image = std::move(result.image);
                                           done = true;
                                       }),
            "editable pin composite export starts");
    waitUntil([&] { return done; }, "editable pin composite export finishes");
    return image;
}

bool samePixels(const QImage& a, const QImage& b) {
    return a.convertToFormat(QImage::Format_ARGB32_Premultiplied) ==
           b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

void capturePinKeepsEditableAnnotations() {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    const snow_shot::storage::ScreenshotSettings settings;
    require(!settings.keepAnnotationsEditableAfterPinningToScreen(), "editable pins default off");
    require(snow_shot::storage::PinToScreenSettings().setExitAnnotationModeBeforeClosing(false),
            "close annotation fixtures directly");
    // One controller exercises fresh and reused pin shells across both setting values.
    ScreenshotController controller;
    for (int scenario = 0; scenario < 4; ++scenario) {
        const bool editable = scenario != 0;
        const bool redoOnly = scenario == 2;
        const bool compact = scenario == 3;
        const bool save = scenario == 1 || scenario == 4;
        const bool copy = scenario != 4;
        QTemporaryDir directory;
        require(directory.isValid() &&
                    settings.setKeepAnnotationsEditableAfterPinningToScreen(editable) &&
                    settings.setCopyAfterPinningToScreen(copy) &&
                    settings.setCopyImageFileToClipboard(save) &&
                    settings.setImageSaveDirectory(directory.path()) &&
                    settings.setAutoSaveAfterExecutionActions(
                        save ? QStringList{QStringLiteral("pin_to_screen"),
                                           QStringLiteral("copy_screenshot")}
                             : QStringList{}),
                "configure editable pin and associated outputs");
        QGuiApplication::clipboard()->setText(QStringLiteral("editable pin clipboard sentinel"));
        auto policy = storage.captureHistory().policy();
        policy.enabled = !redoOnly;
        require(storage.requestCaptureHistoryPolicy(policy),
                "configure screenshot history independently");
        ScreenshotSelectionParams selection;
        selection.selection = QRect(10, 12, 90, 55);
        selection.radius = compact ? 0 : 7;
        selection.shadowWidth = 4;
        if (compact) {
            selection.region =
                ScreenshotRegionGeometry(QRegion(10, 12, 30, 55) + QRect(70, 12, 30, 55));
            selection.layoutCompaction = {true, 2};
        }
        ScreenshotSelectionSettingsStore selectionSettings;
        selectionSettings.setLayoutCompaction(selection.layoutCompaction);
        const auto base = fixture();
        const auto styledBase = ScreenshotResultCompositor::compose(base.copy(selection.selection),
                                                                    selection.resultStyle());
        ScreenshotControllerTestAccess::prepareSelection(controller, base, selection);
        auto& source = ScreenshotControllerTestAccess::runtime(controller);
        require(source.clearDocumentPreservingViewports(),
                "each capture starts with a fresh canvas");
        require(
            !source.applyAnnotationTransaction(editablePinRectangle(20, 22, Qt::red)).isEmpty() &&
                !source.applyAnnotationTransaction(editablePinRectangle(75, 42, Qt::blue))
                     .isEmpty() &&
                source.undo() && (!redoOnly || source.undo()),
            "capture annotations have undo and redo history before pinning");
        const QByteArray expectedCheckpoint = source.documentCheckpoint();
        SnowCanvasRuntime expected;
        require(expected.restoreDocumentSession(source.serializeDocumentSession()),
                "snapshot the source history");
        const auto historyCount = storage.captureHistory().records().size();
        controller.mcpPinSelectionToScreen();
        waitUntil(
            [&] {
                return visiblePins().size() == 1 &&
                       !ScreenshotControllerTestAccess::exportActive(controller) &&
                       !controller.blocksApplicationUpdate();
            },
            "editable pin and optional outputs finish");
        auto* pin = visiblePins().first();
        const auto state = pin->automationState();
        require(!state.value(QStringLiteral("editing")).toBool(),
                "editable pin keeps the normal initial mode");
        auto snapshot = pin->persistenceSnapshot();
        auto* canvas = pin->findChild<SnowCanvasWidget*>();
        require(canvas != nullptr, "pin owns an editable canvas");
        SnowCanvasRuntime imported;
        require(imported.restoreDocumentSession(snapshot.canvasSession),
                "pin session is restorable");
        const QImage composite = editablePinImage(*pin);
        if (copy) {
            const auto [copied, copyFile] = captureClipboardOutput();
            require(samePixels(copied, composite) && (!copyFile.isEmpty() == save),
                    "copy after pin uses the annotated composite and honors file copying");
        } else {
            require(QGuiApplication::clipboard()->text() ==
                        QStringLiteral("editable pin clipboard sentinel"),
                    "automatic saving without copy leaves the clipboard unchanged");
        }
        require(selectionSettings.layoutCompaction() == selection.layoutCompaction,
                "editable pin leaves the stored layout compaction preference unchanged");
        require(QDir(directory.path()).entryList(QDir::Files).size() == (save ? 1 : 0),
                "overlapping pin and copy auto-save scenarios write once");
        if (save) {
            const auto files = QDir(directory.path()).entryList(QDir::Files);
            require(samePixels(QImage(QDir(directory.path()).filePath(files.first())), composite),
                    "automatic save includes transferred annotations");
        }
        if (!redoOnly) {
            waitUntil([&] { return storage.captureHistory().records().size() > historyCount; },
                      "pin history preview is published");
            const auto preview = storage.captureHistory().loadResultImage(
                storage.captureHistory().records().first());
            require(preview && samePixels(*preview, composite),
                    "history preview includes annotations and retains the original layout");
        }
        if (!editable) {
            require(!imported.hasDocumentContent() && !imported.canUndo() && !imported.canRedo() &&
                        samePixels(snapshot.image, composite),
                    "disabled pins still flatten annotations and start without inherited history");
            closePins();
            continue;
        }
        require(imported.documentCheckpoint() == expectedCheckpoint,
                "editable pin keeps the exact source document");
        require(imported.canUndo() == !redoOnly && imported.canRedo(),
                "editable pin keeps both history stacks");
        require(samePixels(snapshot.image, styledBase),
                "editable pin bakes selection effects into the background without annotations");
        require(snapshot.canvasSourceRect == QRectF(selection.selection).adjusted(-4, -4, 4, 4) &&
                    snapshot.contentCanvasRect == snapshot.canvasSourceRect &&
                    snapshot.surfaceCanvasRect == snapshot.canvasSourceRect,
                "editable pin uses the same styled surface coordinates as a flattened pin");
        const auto liveStyle = decodeScreenshotResultStyle(snapshot.resultStyle);
        require(liveStyle && liveStyle->cornerRadius == 0 && liveStyle->effectWidth() == 0 &&
                    !liveStyle->region && liveStyle->backgroundOpacityPercent == 0,
                "the pin does not apply baked selection effects again");
        require(composite.size() == QSize(98, 63),
                "editable pin keeps original dimensions and applies shadow once");
        require(canvas->redo() && expected.redo(),
                "transferred redo restores an undone annotation");
        require(imported.restoreDocumentSession(pin->persistenceSnapshot().canvasSession) &&
                    imported.documentCheckpoint() == expected.documentCheckpoint(),
                "redo reaches exactly the source document state");
        if (!redoOnly) {
            require(canvas->undo() && expected.undo() && canvas->undo() && expected.undo(),
                    "undo reaches through all inherited edits");
            require(imported.restoreDocumentSession(pin->persistenceSnapshot().canvasSession) &&
                        !imported.hasDocumentContent() && imported.canRedo(),
                    "undo removes inherited annotations from the visible base");
            const auto clean = editablePinImage(*pin);
            require(samePixels(clean, styledBase),
                    "undo leaves exactly the styled background without baked annotations");
            require(canvas->redo() && expected.redo(), "restore first annotation for editing");
        }
        // Hit testing and a style mutation prove this is a selectable canvas object.
        QString error;
        static_cast<void>(pin->automationEdit(
            QStringLiteral("tool"), {{QStringLiteral("tool"), QStringLiteral("select")}}, &error));
        require(error.isEmpty(), "activate selection on imported annotations");
        const QPointF point = canvas->canvasToViewTransform().map(QPointF(28, 29));
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, point, canvas->mapToGlobal(point.toPoint()), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(canvas, &event);
        }
        require(canvas->canvasStyleToolbarState().selectedElementCount == 1 &&
                    canvas->setSelectedOpacity(0.5),
                "imported annotation is selectable and editable");
        require(canvas->undo() &&
                    imported.restoreDocumentSession(pin->persistenceSnapshot().canvasSession) &&
                    imported.documentCheckpoint() == expected.documentCheckpoint(),
                "new edits append to the inherited history and undo independently");
        // Store a state with both undo and redo; source pixels must remain clean on disk.
        snapshot = pin->persistenceSnapshot();
        const QString id = snapshot.id;
        pin->checkpointStorage();
        waitUntil([&] { return storage.pinnedWindows().pendingSummaries().isEmpty(); },
                  "editable pin source persistence completes");
        closePins();
        require(storage.pinnedWindows().flush().success, "editable pin state flush succeeds");
        const auto stored = storage.pinnedWindows().loadRecord(id);
        require(stored && samePixels(stored->image, styledBase) &&
                    imported.restoreDocumentSession(stored->canvasSession) && imported.canUndo() &&
                    imported.canRedo(),
                "persisted pin keeps clean pixels and complete undo/redo history");
        controller.showPinnedRecord(id);
        waitUntil([&] { return visiblePins().size() == 1; }, "editable pin restores from storage");
        pin = visiblePins().first();
        canvas = pin->findChild<SnowCanvasWidget*>();
        require(canvas && canvas->redo() && canvas->undo() && canvas->undo(),
                "restored pin supports new-edit redo and inherited annotation undo");
        require(imported.restoreDocumentSession(pin->persistenceSnapshot().canvasSession) &&
                    !imported.hasDocumentContent(),
                "restored undo removes the final inherited annotation");
        if (redoOnly)
            require(storage.captureHistory().records().size() == historyCount,
                    "editable pin transfer does not require screenshot history storage");
        closePins();
    }
    require(settings.setKeepAnnotationsEditableAfterPinningToScreen(false) &&
                settings.setCopyAfterPinningToScreen(false) &&
                settings.setCopyImageFileToClipboard(false) &&
                settings.setAutoSaveAfterExecutionActions({}),
            "reset editable pin test settings");
}

void capturePinCommitsPendingText() {
    const snow_shot::storage::ScreenshotSettings settings;
    require(settings.setKeepAnnotationsEditableAfterPinningToScreen(true),
            "enable pending text transfer");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    auto policy = storage.captureHistory().policy();
    policy.enabled = false;
    require(storage.requestCaptureHistoryPolicy(policy),
            "disable screenshot history for pending text transfer");
    ScreenshotController controller;
    ScreenshotSelectionParams selection;
    selection.selection = QRect(0, 0, 120, 80);
    ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
    auto* overlay = ScreenshotControllerTestAccess::presentSelection(controller);
    auto* canvas = overlay ? overlay->canvas() : nullptr;
    QString error;
    require(canvas && controller.mcpSetTool(QStringLiteral("text"), &error),
            "activate text on the screenshot canvas");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    canvas->setInteractionEnabled(true);
    const QPointF point = canvas->canvasToViewTransform().map(QPointF(20, 20));
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, point, canvas->mapToGlobal(point.toPoint()), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &event);
    }
    QInputMethodEvent input;
    input.setCommitString(QStringLiteral("Pending annotation"));
    QCoreApplication::sendEvent(canvas, &input);
    require(canvas->hasActiveTextEditing(), "screenshot text is still an uncommitted draft");
    controller.mcpPinSelectionToScreen();
    waitUntil(
        [&] {
            return visiblePins().size() == 1 &&
                   !ScreenshotControllerTestAccess::exportActive(controller);
        },
        "pending text pin finishes without screenshot history");
    auto* pin = visiblePins().first();
    SnowCanvasRuntime imported;
    require(imported.restoreDocumentSession(pin->persistenceSnapshot().canvasSession) &&
                imported.canUndo() &&
                imported.documentCheckpoint().contains("Pending annotation") && imported.undo() &&
                !imported.hasDocumentContent(),
            "pending text is committed once and remains undoable after pinning");
    closePins();
    require(settings.setKeepAnnotationsEditableAfterPinningToScreen(false),
            "reset pending text transfer setting");
}

void capturePinSamplesOnlyCroppedImage() {
    const snow_shot::storage::ScreenshotSettings settings;
    require(settings.setKeepAnnotationsEditableAfterPinningToScreen(true) &&
                settings.setCopyAfterPinningToScreen(true),
            "enable sampling annotation transfer");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    auto policy = storage.captureHistory().policy();
    policy.enabled = false;
    require(storage.requestCaptureHistoryPolicy(policy),
            "disable capture history to verify pin source ownership");
    const auto normalize = [](const QImage& value) {
        auto pixels = value.convertToFormat(QImage::Format_ARGB32);
        pixels.setColorSpace(QColorSpace::SRgb);
        pixels.setDevicePixelRatio(1);
        return pixels;
    };
    for (const bool magnifier : {false, true}) {
        ScreenshotController controller;
        QImage image(200, 140, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::blue);
        image.setColorSpace(QColorSpace::SRgb);
        {
            QPainter painter(&image);
            painter.fillRect(QRect(0, 0, 100, 140), Qt::green);
        }
        ScreenshotSelectionParams selection;
        selection.selection = QRect(100, 0, 100, 140);
        ScreenshotControllerTestAccess::prepareSelection(controller, image, selection);
        auto& runtime = ScreenshotControllerTestAccess::runtime(controller);
        SnowCanvasWidget editor(runtime);
        editor.resize(200, 140);
        editor.show();
        QCoreApplication::processEvents();
        require(editor.setViewportCamera(100, 70, 1) &&
                    editor.setCanvasTool(magnifier ? SnowCanvasTool::Magnifier
                                                   : SnowCanvasTool::RectangleFilter),
                "create a sampling annotation in the source canvas");
        const auto drag = [&](QPointF from, QPointF to) {
            for (const auto type :
                 {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
                const auto point = type == QEvent::MouseButtonPress ? from : to;
                QMouseEvent event(type, point, editor.mapToGlobal(point.toPoint()),
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                  type == QEvent::MouseButtonRelease ? Qt::NoButton
                                                                     : Qt::LeftButton,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(&editor, &event);
            }
        };
        if (magnifier) {
            drag({20, 50}, {40, 70});
            drag({30, 60}, {30, 60});
            drag({30, 104}, {160, 104});
        } else {
            SnowCanvasFilterStyle style;
            style.type = SnowCanvasFilterType::GaussianBlur;
            style.strength = 0.7;
            require(editor.setCanvasFilterStyle(style, SnowCanvasFilterStylePropertyType |
                                                           SnowCanvasFilterStylePropertyStrength),
                    "configure blur sampling");
            drag({90, 20}, {130, 100});
        }
        require(editor.resetEditingState() && runtime.canUndo(), "commit sampling annotation");
        const auto expected = runtime.renderToImage(selection.selection, selection.selection.size(),
                                                    {{image, QRectF(0, 0, 200, 140)}});
        require(!expected.isNull(), "source sampling export succeeds");
        if (magnifier)
            require(expected.pixelColor(60, 60) == QColor(Qt::green),
                    "magnifier samples pixels outside the crop");
        const auto cropped = image.copy(selection.selection);
        const auto expectedPin = runtime.renderToImage(
            selection.selection, selection.selection.size(), {{cropped, selection.selection}});
        require(!expectedPin.isNull(), "cropped sampling export succeeds");
        if (magnifier)
            require(normalize(expectedPin) != normalize(expected),
                    "magnifier sampling outside the crop loses access to the full capture");
        controller.mcpPinSelectionToScreen();
        waitUntil(
            [&] {
                return visiblePins().size() == 1 &&
                       !ScreenshotControllerTestAccess::exportActive(controller) &&
                       !controller.blocksApplicationUpdate();
            },
            "sampling annotation pin finishes");
        auto* pin = visiblePins().first();
        require(normalize(captureClipboardOutput().first) == normalize(expectedPin),
                "automatic copying uses the pin's cropped sampling and transferred annotations");
        require(normalize(editablePinImage(*pin)) == normalize(expectedPin),
                "pinned filters and magnifiers sample only the cropped image");
        require(normalize(pin->persistenceSnapshot().image) == normalize(cropped),
                "pin retains only the clean cropped background");
        waitUntil([&] { return image.isDetached(); },
                  "pin releases the full capture after producing its cropped background");
        auto* canvas = pin->findChild<SnowCanvasWidget*>();
        require(canvas && canvas->undo() && canvas->redo() &&
                    normalize(editablePinImage(*pin)) == normalize(expectedPin),
                "sampling stays within the crop through undo and redo");
        const auto transform = [&](const QJsonObject& properties) {
            QString error;
            require(pin->automationUpdate(properties, &error) && error.isEmpty(),
                    "change sampling pin orientation");
        };
        const auto resetAndVerify = [&] {
            transform({{QStringLiteral("rotation"), QStringLiteral("reset")}});
            require(canvas->undo() && canvas->redo() &&
                        normalize(editablePinImage(*pin)) == normalize(expectedPin),
                    "reset and undo/redo continue to sample only the cropped background");
        };
        for (const auto& properties :
             {QJsonObject{{QStringLiteral("flip"), QStringLiteral("horizontal")}},
              QJsonObject{{QStringLiteral("flip"), QStringLiteral("vertical")}},
              QJsonObject{{QStringLiteral("rotation"), QStringLiteral("clockwise")}}}) {
            transform(properties);
            if (magnifier)
                require(canvas->undo() && canvas->redo() &&
                            editablePinImage(*pin).pixelColor(60, 60) == QColor(Qt::blue),
                        "transformed magnifiers sample the oriented background");
            resetAndVerify();
        }
        for (int turn = 0; turn < 4; ++turn)
            transform({{QStringLiteral("rotation"), QStringLiteral("clockwise")}});
        require(pin->persistenceSnapshot().imageTransform.isIdentity() && canvas->undo() &&
                    canvas->redo() && normalize(editablePinImage(*pin)) == normalize(expectedPin),
                "a full rotation preserves crop-limited sampling");
        // Restore a transformed pin and check that reset still uses only the cropped image.
        transform({{QStringLiteral("flip"), QStringLiteral("horizontal")}});
        auto& repository = snow_shot::storage::ApplicationStorage::instance().pinnedWindows();
        const auto id = pin->persistenceSnapshot().id;
        pin->checkpointStorage();
        waitUntil([&] { return repository.pendingSummaries().isEmpty(); },
                  "cropped pin persistence completes");
        closePins();
        require(repository.flush().success, "cropped pin flushes to disk");
        const auto stored = repository.loadRecord(id);
        require(stored && !stored->imageTransform.isIdentity() &&
                    normalize(stored->image) == normalize(cropped),
                "transformed pins persist only their cropped background");
        controller.showPinnedRecord(id);
        waitUntil([&] { return visiblePins().size() == 1; }, "sampling pin restores from disk");
        pin = visiblePins().first();
        canvas = pin->findChild<SnowCanvasWidget*>();
        require(canvas && !pin->persistenceSnapshot().imageTransform.isIdentity(),
                "sampling pin restores its transformed orientation");
        resetAndVerify();
        closePins();
    }
    require(settings.setKeepAnnotationsEditableAfterPinningToScreen(false) &&
                settings.setCopyAfterPinningToScreen(false),
            "reset sampling transfer settings");
}

void editablePinRejectsInvalidDocument() {
    ScreenshotSelectionExportUiServices services;
    ScreenshotPinnedSelectionRequest request;
    request.selection = QRect(0, 0, 120, 80);
    request.contentCanvasRect = request.surfaceCanvasRect = QRectF(request.selection);
    request.initialWindowSize = request.selection.size();
    request.geometry.nativeGeometry = QRect(40, 40, 120, 80);
    request.geometry.canvasSourceRect = request.contentCanvasRect;
    request.geometry.initialWindowSize = request.initialWindowSize;
    request.screen = QGuiApplication::primaryScreen();
    request.canvasSession = QByteArrayLiteral("invalid canvas session");
    auto artifact =
        std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(fixture()));
    int completions = 0;
    require(request.isPrepared() &&
                !services.presentPinnedArtifact(
                    request, artifact,
                    [&](bool ok, auto) {
                        require(!ok, "invalid canvas import must fail instead of flattening");
                        ++completions;
                    }),
            "invalid canvas import rejects pin presentation");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(completions == 1 && visiblePins().isEmpty() &&
                snow_shot::storage::ApplicationStorage::instance()
                    .pinnedWindows()
                    .pendingSummaries()
                    .isEmpty(),
            "failed editable handoff completes once and releases its persistence reservation");
    request.canvasSession = SnowCanvasRuntime().serializeDocumentSession();
    artifact =
        std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(fixture()));
    bool ready = false;
    require(services.presentPinnedArtifact(request, artifact,
                                           [&](bool ok, auto) {
                                               require(ok,
                                                       "valid pin succeeds after a failed import");
                                               ready = true;
                                           }),
            "reuse pin services after a rejected handoff");
    waitUntil([&] { return ready && visiblePins().size() == 1; },
              "valid pin after failed import finishes");
    closePins();
}

void capturePinCopiesOnlyWhenEnabled() {
    const snow_shot::storage::ScreenshotSettings settings;
    require(!settings.copyAfterPinningToScreen(), "copy after pinning defaults off");
    for (const bool enabled : {false, true}) {
        for (const bool copyFile : {false, true}) {
            QTemporaryDir directory;
            require(directory.isValid() && settings.setCopyAfterPinningToScreen(enabled) &&
                        settings.setCopyImageFileToClipboard(copyFile) &&
                        settings.setAutoSaveAfterExecutionActions({}) &&
                        settings.setImageSaveDirectory(directory.path()),
                    "configure capture pin copy independently of automatic saving");
            QImage sentinel(3, 2, QImage::Format_ARGB32);
            sentinel.fill(Qt::magenta);
            sentinel.setColorSpace(QColorSpace::SRgb);
            require(ScreenshotClipboardService::publishImage(QApplication::clipboard(), sentinel),
                    "publish a sentinel through the production clipboard boundary");
            ScreenshotController controller;
            auto& history = snow_shot::storage::ApplicationStorage::instance().captureHistory();
            const qsizetype historyCount = history.records().size();
            const QImage image = fixture();
            ScreenshotSelectionParams selection;
            selection.selection = QRect(10, 12, 80, 50);
            selection.radius = 8;
            selection.shadowWidth = 4;
            ScreenshotControllerTestAccess::prepareSelection(controller, image, selection);
            controller.mcpPinSelectionToScreen();
            waitUntil(
                [&] {
                    return visiblePins().size() == 1 &&
                           !ScreenshotControllerTestAccess::exportActive(controller);
                },
                "capture pin and optional copy must finish offscreen");
            const QImage pinned = visiblePins().constFirst()->persistenceSnapshot().image;
            require(!pinned.isNull(), "copy mode must preserve successful pinning");
            const auto [copied, copiedFile] = captureClipboardOutput();
            if (enabled) {
                require(!copied.isNull() &&
                            copied.convertToFormat(QImage::Format_ARGB32_Premultiplied) ==
                                pinned.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                        "clipboard must match the pin including crop, rounded corners and shadow");
                require(!copiedFile.isEmpty() == copyFile,
                        "copy after pinning must honor image-file clipboard preferences");
                if (copyFile)
                    require(QFileInfo::exists(copiedFile),
                            "combined copy must retain its temporary file");
            } else {
                require(copied.convertToFormat(QImage::Format_ARGB32_Premultiplied) ==
                                sentinel.convertToFormat(QImage::Format_ARGB32_Premultiplied) &&
                            copiedFile.isEmpty(),
                        "disabled copy after pinning must leave the clipboard untouched");
            }
            require(QDir(directory.path()).entryList(QDir::Files).isEmpty(),
                    "clipboard-only pinning must not create an automatic save");
            waitUntil([&] { return history.records().size() == historyCount + 1; },
                      "pinning must create one history record regardless of optional copying");
            require(history.records().constFirst().source ==
                        snow_shot::storage::CaptureHistorySource::PinnedToScreen,
                    "the shared pin and copy export must retain the pin history source");
            closePins();
        }
    }
    // Both execution scenarios select the same output; they must create only one file.
    QTemporaryDir directory;
    require(directory.isValid() && settings.setCopyAfterPinningToScreen(true) &&
                settings.setCopyImageFileToClipboard(true) &&
                settings.setImageSaveDirectory(directory.path()) &&
                settings.setAutoSaveAfterExecutionActions(
                    {QStringLiteral("pin_to_screen"), QStringLiteral("copy_screenshot")}),
            "configure overlapping pin and copy automatic saves");
    ScreenshotController controller;
    ScreenshotSelectionParams selection;
    selection.selection = QRect(0, 0, 120, 80);
    ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
    controller.mcpPinSelectionToScreen();
    waitUntil(
        [&] {
            return visiblePins().size() == 1 &&
                   !ScreenshotControllerTestAccess::exportActive(controller) &&
                   !controller.blocksApplicationUpdate();
        },
        "combined pin, clipboard and automatic save must finish");
    require(QDir(directory.path()).entryList(QDir::Files).size() == 1,
            "pin and copy must share one automatic save");
    closePins();
    const auto clipboardBeforeFailure = captureClipboardOutput();
    ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), {});
    controller.mcpPinSelectionToScreen();
    waitUntil([&] { return !ScreenshotControllerTestAccess::exportActive(controller); },
              "a rejected pin must finish its export");
    require(visiblePins().isEmpty() && captureClipboardOutput() == clipboardBeforeFailure &&
                QDir(directory.path()).entryList(QDir::Files).size() == 1,
            "a rejected pin must not copy or save anything");
    require(settings.setCopyAfterPinningToScreen(false) &&
                settings.setCopyImageFileToClipboard(false) &&
                settings.setAutoSaveAfterExecutionActions({}),
            "restore screenshot copy settings");
}

void historyPinAccountsForDecodeBeforeDelivery() {
    auto& history = snow_shot::storage::ApplicationStorage::instance().captureHistory();
    auto& coordinator = ScreenshotExportCoordinator::shared();
    qint64 previousWorkingBytes = 0;
    for (const QSize size : {QSize(24, 16), QSize(120, 80)}) {
        require(coordinator.pendingJobCount() == 0 && coordinator.activeWorkingBytes() == 0,
                "history admission starts without unrelated export work");
        QImage image(size, QImage::Format_RGBA8888);
        image.fill(QColor(23, 117, 209, 127));
        image.setColorSpace(QColorSpace::SRgb);
        snow_shot::storage::CaptureHistoryDraft draft;
        draft.contentKind = snow_shot::storage::CaptureHistoryContentKind::Image;
        draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        draft.createdUtc = QDateTime::currentDateTimeUtc();
        draft.canvasBounds = QRect(QPoint(), size);
        draft.selection.rectangle = draft.canvasBounds;
        draft.selection.shadowColor = Qt::black;
        draft.canvasHistory = QByteArrayLiteral("{}");
        draft.displays.append(
            {QStringLiteral("history-admission"), QStringLiteral("Display"), image});
        draft.resultImage = image;
        const auto published = history.publish(std::move(draft)).get();
        require(published.storage.success && published.record.result,
                "publish a real isolated history image for decode admission");
        ScreenshotController controller;
        controller.pinHistoryRecord(published.record.id);
        // Submission accounts for bytes immediately, and completion releases them only on
        // GUI delivery. No timing assumption about the worker is needed for this observation.
        const qint64 workingBytes = coordinator.activeWorkingBytes();
        require(workingBytes >= image.sizeInBytes() + published.record.result->encodedBytes &&
                    workingBytes > previousWorkingBytes,
                "history pin reserves encoded input, decoded pixels and size-dependent scratch");
        previousWorkingBytes = workingBytes;
        waitUntil(
            [&] {
                return visiblePins().size() == 1 && coordinator.pendingJobCount() == 0 &&
                       coordinator.activeWorkingBytes() == 0;
            },
            "history decode releases its working reservation after completing presentation");
        require(samePixels(visiblePins().first()->persistenceSnapshot().image, image),
                "accounted history decode preserves the requested image pixels");
        closePins();
    }
}

ScreenshotScrollingSnapshot scrollingSnapshot(QImage image) {
    SnowStitchConfig config{};
    require(snow_stitch_config_default(&config) != 0, "initialize scrolling fixture config");
    auto* session = snow_stitch_session_create(&config);
    require(session != nullptr, "create scrolling fixture session");
    auto* owner = new QImage(std::move(image));
    const QSize size = owner->size();
    SnowStitchFrameOutcome outcome{};
    require(snow_stitch_session_push_external_rgba(
                session, uint32_t(size.width()), uint32_t(size.height()), owner->constBits(),
                size_t(owner->sizeInBytes()),
                [](void* context) { delete static_cast<QImage*>(context); }, owner, &outcome) != 0,
            "seed scrolling fixture pixels");
    auto* snapshot = snow_stitch_session_snapshot(session, 0, uint32_t(size.height()));
    snow_stitch_session_destroy(session);
    require(snapshot != nullptr, "create immutable scrolling fixture snapshot");
    return ScreenshotScrollingSnapshot::adoptNative(snapshot, size);
}

void scrollingMcpExportCompletesWhenMaterializationIsRejected() {
    auto& coordinator = ScreenshotExportCoordinator::shared();
    ScreenshotController controller;
    ScreenshotSelectionParams selection;
    selection.selection = QRect(10, 12, 80, 50);
    ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
    const auto snapshot = scrollingSnapshot(fixture());
    ScreenshotControllerTestAccess::prepareScrollingSnapshot(controller, snapshot);
    const auto rejected = controller.mcpExportArtifact(1.0);
    require(rejected && rejected->isValid(), "create an accepted scrolling MCP source");
    require(coordinator.pendingJobCount() == 0, "scrolling admission starts without export work");

    QObject receiver;
    const auto gate = std::make_shared<QSemaphore>();
    std::vector<ScreenshotExportJobHandle> blockers;
    const auto unblock = qScopeGuard([&] { gate->release(int(blockers.size())); });
    // Keep both active workers and every pending slot occupied. None can retire before
    // release, so rejection is independent of worker speed and owner-thread timer ordering.
    for (int index = 0; index < 16; ++index) {
        auto blocker = coordinator.submit(
            &receiver, ScreenshotExportCoordinator::Priority::Foreground,
            [gate](const auto&) {
                gate->acquire();
                return ScreenshotExportTaskResult{};
            },
            [](auto) {});
        require(blocker.isValid(), "fill the bounded export coordinator");
        blockers.push_back(std::move(blocker));
    }
    require(!coordinator
                 .submit(
                     &receiver, ScreenshotExportCoordinator::Priority::Foreground,
                     [](const auto&) { return ScreenshotExportTaskResult{}; }, [](auto) {})
                 .isValid(),
            "the scrolling materialization will encounter a full coordinator");

    int completions = 0;
    require(rejected->requestImage(&receiver,
                                   [&](ScreenshotExportImageResult result) {
                                       require(
                                           !result.succeeded() && result.image.isNull(),
                                           "rejected scrolling materialization reports failure");
                                       ++completions;
                                   }),
            "scrolling snapshot acquisition accepts the artifact request");
    waitUntil([&] { return completions == 1; },
              "queue rejection must terminalize the accepted scrolling MCP loader");
    QCoreApplication::processEvents();
    require(completions == 1 && coordinator.pendingJobCount() == int(blockers.size()),
            "rejection completes once without adding work or waiting for unrelated exports");

    gate->release(int(blockers.size()));
    waitUntil([&] { return coordinator.pendingJobCount() == 0; },
              "release every saturated export slot");
    const auto retry = controller.mcpExportArtifact(1.0);
    bool recovered = false;
    require(retry && retry->requestImage(&receiver,
                                         [&](ScreenshotExportImageResult result) {
                                             require(result.succeeded() &&
                                                         samePixels(result.image, fixture()),
                                                     "a later scrolling export keeps its pixels");
                                             recovered = true;
                                         }),
            "accept a new scrolling export after capacity returns");
    waitUntil([&] { return recovered; }, "scrolling capture remains usable after queue rejection");
}

void scrollingMcpExportCompletesWhenSnapshotIsCancelled() {
    ScreenshotController controller;
    ScreenshotSelectionParams selection;
    selection.selection = QRect(10, 12, 80, 50);
    ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
    ScreenshotControllerTestAccess::prepareScrollingSnapshot(controller,
                                                             scrollingSnapshot(fixture()));
    const auto artifact = controller.mcpExportArtifact(1.0);
    QObject receiver;
    int completions = 0;
    require(artifact &&
                artifact->requestImage(&receiver,
                                       [&](ScreenshotExportImageResult result) {
                                           require(!result.succeeded(),
                                                   "cancelled snapshot fails its live export");
                                           ++completions;
                                       }),
            "accept the scrolling snapshot before capture cancellation");
    require(completions == 0,
            "cached scrolling snapshot completion is queued before capture cancellation");
    controller.mcpCancelCapture();
    waitUntil([&] { return completions == 1; },
              "snapshot cancellation terminalizes the live artifact without an MCP timeout");
    QCoreApplication::processEvents();
    require(completions == 1 && !artifact->isCancelled() &&
                !controller.mcpState()
                     .value(QStringLiteral("scrolling"))
                     .toObject()
                     .value(QStringLiteral("active"))
                     .toBool(),
            "late cached snapshot cannot revive cancelled acquisition or finish it twice");
}

void scrollingCopyUsesHiDpiScalingWithEveryFileOption() {
    const snow_shot::storage::ScreenshotSettings settings;
    QTemporaryDir output;
    require(output.isValid() && settings.setImageSaveDirectory(output.path()) &&
                settings.setImageFormat(QStringLiteral("png")) && settings.setShowSaveResult(false),
            "configure scrolling copy outputs");
    for (const int flags : {0, 1, 2, 3}) {
        require(settings.setHiDpiImageScaling(flags == 3 ? QStringLiteral("auto")
                                                         : QStringLiteral("200")) &&
                    settings.setAutoSaveFilenameFormat(QStringLiteral("scrolling_%1").arg(flags)) &&
                    settings.setCopyImageFileToClipboard((flags & 2) != 0) &&
                    settings.setAutoSaveAfterExecutionActions(
                        (flags & 1) ? QStringList{QStringLiteral("copy_scrolling_screenshot")}
                                    : QStringList{}),
                "configure scrolling copy scenario");
        ScreenshotController controller;
        ScreenshotSelectionParams selection;
        selection.selection = QRect(10, 12, 80, 50);
        ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
        ScreenshotControllerTestAccess::setDisplayScale(controller, 2.0);
        const auto snapshot = scrollingSnapshot(fixture());
        ScreenshotControllerTestAccess::prepareScrollingSnapshot(controller, snapshot);
        ScreenshotControllerTestAccess::commands(controller).copySelectionToClipboard();
        require(settings.setHiDpiImageScaling(QStringLiteral("disabled")),
                "queued scrolling export must retain its accepted scale");
        waitUntil([&] { return !ScreenshotControllerTestAccess::exportActive(controller); },
                  "scaled scrolling copy completes");
        const auto [copied, path] = captureClipboardOutput();
        require(copied.size() == QSize(60, 40) && snapshot.rowSource().size == QSize(120, 80),
                "plain and file scrolling copies scale once without changing the source");
        if (flags & 2)
            require(!path.isEmpty() && QImage(path).size() == QSize(60, 40),
                    "scrolling clipboard file uses the same scale");
        if (flags & 1) {
            const QString savedPath =
                QDir(output.path()).filePath(QStringLiteral("scrolling_%1.png").arg(flags));
            waitUntil([&] { return QImage(savedPath).size() == QSize(60, 40); },
                      "scrolling auto-save uses the same scale");
        }
    }
    require(settings.setCopyImageFileToClipboard(false) &&
                settings.setAutoSaveAfterExecutionActions({}),
            "reset scrolling copy settings");
}

void hiDpiScalingAffectsOnlyCopiedAndSavedOutputs() {
    const snow_shot::storage::ScreenshotSettings settings;
    QTemporaryDir output;
    require(output.isValid() && settings.setImageSaveDirectory(output.path()) &&
                settings.setAutoSaveFilenameFormat(QStringLiteral("hidpi_{HH-mm-ss-zzz}")) &&
                settings.setImageFormat(QStringLiteral("png")) &&
                settings.setCopyAfterPinningToScreen(false) && settings.setShowSaveResult(false),
            "configure deterministic HiDPI output settings");
    const QImage image = fixture();
    for (const int scenario : {0, 1, 2, 3, 4}) {
        const QString mode = scenario == 0   ? QStringLiteral("disabled")
                             : scenario == 1 ? QStringLiteral("150")
                             : scenario == 2 ? QStringLiteral("225")
                             : scenario == 3 ? QStringLiteral("auto")
                                             : QStringLiteral("350");
        require(
            settings.setHiDpiImageScaling(mode) &&
                settings.setCopyImageFileToClipboard(scenario == 2) &&
                settings.setAutoSaveAfterExecutionActions(
                    scenario == 2 ? QStringList{QStringLiteral("copy_screenshot")} : QStringList{}),
            "configure HiDPI copy/save scenario");
        ScreenshotController controller;
        ScreenshotSelectionParams selection;
        selection.selection = QRect(10, 12, 81, 51);
        ScreenshotControllerTestAccess::prepareSelection(controller, image, selection);
        ScreenshotControllerTestAccess::setDisplayScale(controller, 1.5);
        auto& commands = ScreenshotControllerTestAccess::commands(controller);
        const QSize expected = scenario == 0   ? QSize(81, 51)
                               : scenario == 1 ? QSize(54, 34)
                               : scenario == 2 ? QSize(36, 23)
                               : scenario == 3 ? QSize(54, 34)
                                               : QSize(23, 15);
        auto& history = snow_shot::storage::ApplicationStorage::instance().captureHistory();
        const qsizetype before = history.records().size();
        if (scenario == 4) {
            const auto explicitArtifact = controller.mcpExportArtifact(1.0);
            QImage explicitImage;
            require(explicitArtifact &&
                        explicitArtifact->requestImage(
                            &controller, [&](auto result) { explicitImage = result.image; }),
                    "explicit MCP output must prepare");
            waitUntil([&] { return !explicitImage.isNull(); }, "explicit MCP output completes");
            require(explicitImage.size() == selection.selection.size(),
                    "explicit MCP scale must remain independent of the setting");
        }
        if (scenario >= 3) {
            commands.quickSaveSelection();
        } else {
            commands.copySelectionToClipboard();
            // Changing the setting after acceptance must not change this export.
            require(settings.setHiDpiImageScaling(QStringLiteral("disabled")),
                    "mutate settings while output is queued");
        }
        waitUntil([&] { return !ScreenshotControllerTestAccess::exportActive(controller); },
                  "HiDPI output completes");
        if (scenario < 3) {
            const auto [copied, path] = captureClipboardOutput();
            require(copied.size() == expected, "clipboard dimensions must use the captured ratio");
            if (scenario == 2)
                require(!path.isEmpty() && QImage(path).size() == expected,
                        "clipboard files and auto-save must use the same scaled image");
        }
        waitUntil([&] { return history.records().size() == before + 1; },
                  "HiDPI output commits one history result");
        const auto record = history.records().first();
        const auto result = history.loadResultImage(record);
        require(result && result->size() == expected && !record.displays.isEmpty() &&
                    record.displays.first().imageSize == image.size(),
                "history output must match export dimensions while source displays stay full size");
        if (scenario >= 2) {
            waitUntil(
                [&] {
                    for (const auto& name :
                         QDir(output.path()).entryList({QStringLiteral("*.png")}, QDir::Files)) {
                        if (QImage(QDir(output.path()).filePath(name)).size() == expected)
                            return true;
                    }
                    return false;
                },
                "automatic and quick saves must write the scaled dimensions");
        }
    }
    {
        require(settings.setHiDpiImageScaling(QStringLiteral("200")) &&
                    settings.setSaveAsFileDialog(QStringLiteral("snow_shot")),
                "configure scaled save-dialog dimensions");
        ScreenshotController dialogController;
        ScreenshotSelectionParams selection;
        selection.selection = QRect(10, 12, 80, 50);
        ScreenshotControllerTestAccess::prepareSelection(dialogController, image, selection);
        ScreenshotControllerTestAccess::presentSelection(dialogController);
        ScreenshotControllerTestAccess::commands(dialogController).saveSelectionToFile();
        require(settings.setHiDpiImageScaling(QStringLiteral("disabled")),
                "change the setting after the save dialog snapshots its output");
        QPointer<adqt::widgets::AdModal> modal;
        waitUntil(
            [&] {
                modal = dialogController.findChild<adqt::widgets::AdModal*>(
                    QStringLiteral("screenshotSaveAsFileModal"));
                const auto* preview =
                    modal && modal->contentWidget()
                        ? modal->contentWidget()->findChild<ScreenshotSavePreviewCanvas*>()
                        : nullptr;
                return preview && preview->outputImage().size() == QSize(40, 25);
            },
            "save dialog starts from the scaled screenshot dimensions");
        modal->reject();
        waitUntil([&] { return !ScreenshotControllerTestAccess::exportActive(dialogController); },
                  "cancelling the scaled dialog retires its output job");
    }
    require(settings.setHiDpiImageScaling(QStringLiteral("200")) &&
                settings.setCopyImageFileToClipboard(false) &&
                settings.setAutoSaveAfterExecutionActions({}),
            "configure an unscaled pin");
    ScreenshotController controller;
    ScreenshotSelectionParams selection;
    selection.selection = QRect(10, 12, 80, 50);
    ScreenshotControllerTestAccess::prepareSelection(controller, image, selection);
    controller.mcpPinSelectionToScreen();
    waitUntil(
        [&] {
            return visiblePins().size() == 1 &&
                   !ScreenshotControllerTestAccess::exportActive(controller);
        },
        "full-resolution pin completes with HiDPI scaling enabled");
    require(visiblePins().first()->persistenceSnapshot().image.size() == QSize(80, 50),
            "pinning must preserve the full-resolution selection");
    closePins();
    require(settings.setHiDpiImageScaling(QStringLiteral("disabled")), "reset HiDPI test setting");
}

void pinOutputsUseHiDpiScaling() {
    const snow_shot::storage::ScreenshotSettings settings;
    auto& history = snow_shot::storage::ApplicationStorage::instance().captureHistory();
    for (int sourceKind = 0; sourceKind < 3; ++sourceKind) {
        const bool editable = sourceKind == 1;
        const bool scrolling = sourceKind == 2;
        for (int outputKind = 0; outputKind < 5; ++outputKind) {
            const bool copy = outputKind < 3;
            const bool copyFile = outputKind == 1 || outputKind == 2;
            const bool save = outputKind == 2 || outputKind == 3;
            QTemporaryDir directory;
            QStringList actions;
            if (save)
                actions.append(QStringLiteral("pin_to_screen"));
            if (outputKind >= 2)
                actions.append(scrolling ? QStringLiteral("copy_scrolling_screenshot")
                                         : QStringLiteral("copy_screenshot"));
            require(directory.isValid() &&
                        settings.setHiDpiImageScaling(outputKind == 2 ? QStringLiteral("auto")
                                                                      : QStringLiteral("200")) &&
                        settings.setKeepAnnotationsEditableAfterPinningToScreen(editable) &&
                        settings.setCopyAfterPinningToScreen(copy) &&
                        settings.setCopyImageFileToClipboard(copyFile) &&
                        settings.setAutoSaveAfterExecutionActions(actions) &&
                        settings.setImageSaveDirectory(directory.path()) &&
                        settings.setAutoSaveFilenameFormat(QStringLiteral("scaled-pin")) &&
                        settings.setImageFormat(QStringLiteral("png")) &&
                        settings.setShowSaveResult(false),
                    "configure scaled pin outputs");
            QApplication::clipboard()->setText(QStringLiteral("scaled pin clipboard sentinel"));
            const qsizetype before = history.records().size();
            ScreenshotController controller;
            ScreenshotSelectionParams selection;
            selection.selection = QRect(10, 12, 80, 50);
            selection.radius = 6;
            selection.shadowWidth = 4;
            ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
            ScreenshotControllerTestAccess::setDisplayScale(controller, 2.0);
            if (scrolling) {
                ScreenshotControllerTestAccess::prepareScrollingSnapshot(
                    controller, scrollingSnapshot(fixture()));
            } else {
                auto& runtime = ScreenshotControllerTestAccess::runtime(controller);
                require(
                    runtime.clearDocumentPreservingViewports() &&
                        !runtime.applyAnnotationTransaction(editablePinRectangle(20, 22, Qt::red))
                             .isEmpty(),
                    "pin output includes the captured annotations");
            }
            controller.mcpPinSelectionToScreen();
            require(settings.setHiDpiImageScaling(QStringLiteral("disabled")),
                    "pin outputs retain the scale accepted before the first frame");
            waitUntil(
                [&] {
                    return visiblePins().size() == 1 &&
                           !ScreenshotControllerTestAccess::exportActive(controller) &&
                           !controller.blocksApplicationUpdate() &&
                           history.records().size() == before + 1;
                },
                "pin, optional outputs and native history finish");
            auto* pin = visiblePins().first();
            const QImage native = editablePinImage(*pin);
            const QSize nativeSize =
                scrolling ? fixture().size()
                          : ScreenshotResultCompositor::layoutForContent(selection.selection.size(),
                                                                         selection.resultStyle())
                                .outputRect.size();
            require(native.size() == nativeSize &&
                        pin->persistenceSnapshot().image.size() == nativeSize,
                    "scaled exports preserve native pin pixels");
            const auto record = history.records().first();
            const auto historyImage = history.loadResultImage(record);
            require(record.source == snow_shot::storage::CaptureHistorySource::PinnedToScreen &&
                        historyImage && samePixels(*historyImage, native),
                    "optional scaled outputs preserve one native pin history result");
            const QSize scaledSize(qRound(nativeSize.width() * 0.5),
                                   qRound(nativeSize.height() * 0.5));
            QImage expected(scaledSize, QImage::Format_RGBA8888);
            expected.setColorSpace(QColorSpace::SRgb);
            // Use the native export rows retained in history as the resampling reference.
            require(snow_shot::image_codec::resizeToRgba8(
                        snow_shot::image_codec::srgbRowSource(*historyImage), scaledSize,
                        expected.bits(), expected.bytesPerLine(), expected.sizeInBytes()),
                    "prepare the expected downscaled composite");
            if (copy) {
                const auto [copied, path] = captureClipboardOutput();
                if (!samePixels(copied, expected) || !path.isEmpty() != copyFile)
                    std::cerr << "pin output mismatch: source=" << sourceKind
                              << " output=" << outputKind << " copied=" << copied.width() << 'x'
                              << copied.height() << " expected=" << expected.width() << 'x'
                              << expected.height() << " file=" << !path.isEmpty() << '\n';
                require(samePixels(copied, expected) && !path.isEmpty() == copyFile,
                        "copy after pin scales the composite once and honors file copying");
                if (copyFile)
                    require(samePixels(QImage(path), expected),
                            "clipboard files share the downscaled pixels");
            } else {
                require(QApplication::clipboard()->text() ==
                            QStringLiteral("scaled pin clipboard sentinel"),
                        "pin auto-save without copying leaves the clipboard untouched");
            }
            const auto files = QDir(directory.path()).entryList(QDir::Files);
            require(files.size() == (save ? 1 : 0),
                    "overlapping pin and copy actions produce at most one automatic save");
            if (save)
                require(
                    samePixels(QImage(QDir(directory.path()).filePath(files.first())), expected),
                    "pin auto-save shares the downscaled pixels even without copying");
            closePins();
        }
    }
    require(settings.setHiDpiImageScaling(QStringLiteral("disabled")) &&
                settings.setKeepAnnotationsEditableAfterPinningToScreen(false) &&
                settings.setCopyAfterPinningToScreen(false) &&
                settings.setCopyImageFileToClipboard(false) &&
                settings.setAutoSaveAfterExecutionActions({}),
            "restore pin output settings");
}

} // namespace

#ifdef Q_OS_MACOS
namespace {
void screenshotSessionFocusRouting() {
    using namespace snow_shot::platform::macos;
    struct Background final : ScreenshotBackgroundWindow {
        Background(ScreenshotController& controller, int& suppressions, int& restorations)
            : controller(controller), suppressions(suppressions), restorations(restorations) {}
        void suppress() override {
            ++suppressions;
        }
        void restoreBehind() override {
            require(controller.captureAvailable(), "background must follow presentation teardown");
            ++restorations;
        }
        ScreenshotController& controller;
        int& suppressions;
        int& restorations;
    };
    struct Target final : ScreenshotFocusTarget {
        Target(ScreenshotController& controller, int& restores, int& live)
            : controller(controller), restores(restores), live(live) {
            ++this->live;
        }
        ~Target() override {
            --live;
        }
        bool canRestore() const override {
            return true;
        }
        bool isExternalApplication() const override {
            return true;
        }
        void restore() override {
            require(controller.captureAvailable(), "focus must return only after session teardown");
            ++restores;
        }
        ScreenshotController& controller;
        int& restores;
        int& live;
    };
    QTemporaryDir output;
    require(output.isValid(), "focus routing needs isolated output storage");
    const snow_shot::storage::ScreenshotSettings settings;
    require(settings.setImageSaveDirectory(output.path()) &&
                settings.setLastManualSaveDirectory(output.path()) &&
                settings.setSaveAsFileDialog(QStringLiteral("system")) &&
                settings.setShowSaveResult(false) && settings.setCopyAfterPinningToScreen(false),
            "focus routing must isolate save and pin settings");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    for (int action = 0; action < 8; ++action) {
        int restores = 0;
        int live = 0;
        int suppressions = 0;
        int backgroundRestores = 0;
        {
            ScreenshotController controller;
            ScreenshotSelectionParams selection;
            selection.selection = QRect(0, 0, 100, 60);
            ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
            ScreenshotControllerTestAccess::installSessionFocus(
                controller, [&] { return std::make_unique<Target>(controller, restores, live); },
                std::make_unique<ScreenshotBackgroundWindowGuard>(
                    ScreenshotBackgroundWindowGuard::Ports{[&] {
                                                               return std::make_unique<Background>(
                                                                   controller, suppressions,
                                                                   backgroundRestores);
                                                           },
                                                           [] { return true; }}));
            auto* overlay = ScreenshotControllerTestAccess::presentSelection(controller);
            // Exercise the actual overlay's frozen/live-desktop transitions without
            // acquiring frames or requiring Screen Recording permission offscreen.
            for (bool scrolling : {true, false, true, false}) {
                overlay->setScrollingCaptureMode(scrolling);
                QCoreApplication::processEvents();
                require(suppressions == 1 && backgroundRestores == 0,
                        "scrolling and return to editing must retain background suppression");
            }
            auto& commands = ScreenshotControllerTestAccess::commands(controller);
            if (action == 0) {
                ScreenshotControllerTestAccess::prepareScrollingSnapshot(
                    controller, scrollingSnapshot(fixture()));
                commands.setScrollingScreenshotAutoScroll(false);
                commands.setMoveTool();
                QCoreApplication::processEvents();
                require(suppressions == 1 && backgroundRestores == 0 &&
                            !controller.captureAvailable(),
                        "stopping the scrolling controller must retain the capture's lease");
            }
            switch (action) {
            case 0:
                controller.mcpCancelCapture();
                controller.mcpCancelCapture();
                break;
            case 1:
                controller.mcpCopySelectionToClipboard();
                break;
            case 2:
                commands.quickSaveSelection();
                break;
            case 3:
            case 4: {
                bool dialogHandled = false;
                QTimer closeDialog;
                closeDialog.setInterval(10);
                QObject::connect(&closeDialog, &QTimer::timeout, &controller, [&] {
                    for (auto* widget : QApplication::topLevelWidgets()) {
                        auto* dialog = qobject_cast<QFileDialog*>(widget);
                        if (!dialog || !dialog->isVisible())
                            continue;
                        closeDialog.stop();
                        dialogHandled = true;
                        if (action == 3) {
                            dialog->selectFile(output.filePath(QStringLiteral("manual.png")));
                            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                        } else {
                            dialog->reject();
                        }
                        return;
                    }
                });
                closeDialog.start();
                commands.saveSelectionToFile();
                require(dialogHandled, "manual save must exercise the real file dialog");
                if (action == 4) {
                    QCoreApplication::processEvents();
                    require(restores == 0 && live == 1 && !controller.captureAvailable(),
                            "cancelling Save must retain the session and its original target");
                    controller.mcpCancelCapture();
                }
                break;
            }
            case 5:
                controller.mcpPinSelectionToScreen();
                break;
            case 6:
                commands.startScreenRecording();
                break;
            case 7:
                break; // Destruction of an active session must not reactivate another app.
            }
            if (action < 7) {
                waitUntil([&] { return live == 0; },
                          "terminal action must release its focus target");
                require(restores == (action < 5 ? 1 : 0),
                        "copy/save/cancel restore focus; pin/recording preserve destination focus");
                waitUntil([&] { return !ScreenshotControllerTestAccess::exportActive(controller); },
                          "focus fixture export must settle");
                QCoreApplication::processEvents();
                require(restores == (action < 5 ? 1 : 0),
                        "late export completion must not restore focus again");
                require(backgroundRestores == 1,
                        "all terminal actions must restore background visibility once");
            }
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        require(live == 0 && (action != 7 || restores == 0),
                "shutdown must discard the target and pending restoration");
        require(action != 7 || backgroundRestores == 0,
                "shutdown must release the lease without showing the main window");
        closePins();
    }
}
} // namespace
#endif

int runScreenshotControllerClipboardTests(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    adqt::widgets::initializePlatformCompatibility(application);
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(directory.isValid() &&
                storage.initialize({directory.path(), directory.path(), 60000}).success,
            "clipboard controller tests need isolated storage");
    const snow_shot::storage::PinToScreenSettings settings;
    require(settings.setAutomaticTextRecognition(false) &&
                settings.setDuplicateContentAction(QStringLiteral("repeat_action")) &&
                settings.setShowBorderByDefault(true) && settings.setShowShadowByDefault(false),
            "clipboard controller settings must be isolated and deterministic");
    static_cast<void>(snow_shot::presentation::styles::ThemeManager::instance());
    if (application.arguments().contains(QStringLiteral("--history-pin-admission-tests"))) {
        const auto shutdown = qScopeGuard([&] { storage.shutdown(); });
        historyPinAccountsForDecodeBeforeDelivery();
        return EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--mcp-scrolling-admission-tests"))) {
        const auto shutdown = qScopeGuard([&] { storage.shutdown(); });
        scrollingMcpExportCompletesWhenMaterializationIsRejected();
        scrollingMcpExportCompletesWhenSnapshotIsCancelled();
        return EXIT_SUCCESS;
    }
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (application.arguments().contains(QStringLiteral("--formula-only"))) {
        {
            ScreenshotController controller;
            clipboardFormulasPinAsImages(controller);
        }
        QApplication::clipboard()->clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        storage.shutdown();
        return EXIT_SUCCESS;
    }
#endif
    if (application.arguments().contains(QStringLiteral("--hidpi-scaling-tests"))) {
        scrollingCopyUsesHiDpiScalingWithEveryFileOption();
        hiDpiScalingAffectsOnlyCopiedAndSavedOutputs();
        pinOutputsUseHiDpiScaling();
        QApplication::clipboard()->clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        storage.shutdown();
        return EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--controller-reselection-tests"))) {
        const auto shutdown = qScopeGuard([&] { storage.shutdown(); });
        captureActionsChooseToolBeforeFirstPresentation(directory.path());
        queuedRecognitionDoesNotOverrideReselection(directory.path());
        recognitionWindowsFollowSelectionTransitions(directory.path());
        return EXIT_SUCCESS;
    }
#ifdef Q_OS_MACOS
    if (application.arguments().contains(QStringLiteral("--session-focus-tests"))) {
        const auto shutdown = qScopeGuard([&] { storage.shutdown(); });
        screenshotSessionFocusRouting();
        return 0;
    }
#endif
    if (application.arguments().contains(QStringLiteral("--shell-file-startup-tests"))) {
        const auto shutdown = qScopeGuard([&] { storage.shutdown(); });
        auto& configuration = storage.configuration();
        QMap<QString, QJsonValue> isolated{
            {QStringLiteral("tray/enabled"), false},
            {QStringLiteral("mcp/enabled"), false},
            {QStringLiteral("updates/mode"), QStringLiteral("manual")}};
        const auto snapshot = configuration.snapshot();
        for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it) {
            if (it.key().startsWith(QStringLiteral("global_shortcuts/")) && it.value().isArray())
                isolated.insert(it.key(), QJsonArray{});
            if (it.key().startsWith(QStringLiteral("global_mouse/")) && it.value().isObject())
                isolated.insert(it.key(), QJsonObject{});
        }
        require(configuration.setValues(isolated),
                "startup fixture disables global input and updates");
        snow_shot::presentation::LanguageManager::instance().initialize();
        snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
        const auto path = directory.filePath(QStringLiteral("startup.png"));
        const auto source = fixture();
        require(source.save(path, "PNG"), "write shell startup fixture");
        for (const auto& selected : {path, directory.filePath(QStringLiteral("missing.png")),
                                     QStringLiteral("relative.png")}) {
            snow_shot::app::ApplicationController controller(application);
            int maintenance = 0;
            controller.start(snow_shot::app::shellImageLaunchArguments({selected}), [&] {
                ++maintenance;
                if (selected == path) {
                    const auto pins = visiblePins();
                    require(pins.size() == 1 &&
                                pins.first()
                                    ->automationState()
                                    .value(QStringLiteral("ready"))
                                    .toBool() &&
                                pins.first()->persistenceSnapshot().image == source,
                            "startup maintenance runs only after the requested content frame");
                }
            });
            require(maintenance == 0,
                    "maintenance does not delay dispatch or run in the launch stack");
            waitUntil([&] { return maintenance == 1; },
                      "startup resumes after presentation or failure");
            controller.start({}, [&] { ++maintenance; });
            require(maintenance == 1, "startup maintenance runs only once");
            closePins();
        }
        {
            ScreenshotController controller;
            int completed = 0;
            controller.pinFilesToScreen({path}, [&] { ++completed; });
            waitUntil([&] { return completed == 1; }, "first shell pin finishes");
            require(settings.setDuplicateContentAction(QStringLiteral("none")),
                    "consume duplicates");
            controller.pinFilesToScreen({path}, [&] { ++completed; });
            waitUntil([&] { return completed == 2; }, "duplicate also finishes the launch");
            require(visiblePins().size() == 1, "duplicate completion creates no second window");
            controller.pinFilesToScreen({}, [&] { ++completed; });
            require(completed == 3, "empty launches also finish");
            closePins();
        }
        return EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--image-editor-pin-tests"))) {
        const auto shutdown = qScopeGuard([&] { storage.shutdown(); });
        ScreenshotController controller;
        snow_shot::presentation::ImageEditingServices services;
        services.pin = [&controller](QImage image, auto completion) {
            ScreenshotClipboardContent content;
            content.image = std::move(image);
            return controller.mcpPinContent(std::move(content), std::move(completion));
        };
        snow_shot::presentation::ImageEditingWindow editor(std::move(services));
        snow_shot::presentation::ImageEditingSource source;
        source.image = fixture();
        source.canvasRect = QRectF(QPointF(), QSizeF(source.image.size()));
        editor.present(source);
        require(editor.document()->crop(
                    snow_shot::presentation::ImageEditingDocument::CropAxis::Rows, 4, 12),
                "editor controller fixture contains a cropped image");
        const auto expected = editor.document()->image();
        int exports = 0;
        QObject::connect(&editor, &snow_shot::presentation::ImageEditingWindow::exportFinished,
                         &editor, [&](bool success) {
                             require(success, "editor image pins through the controller");
                             ++exports;
                         });
        editor.palette()->pinRequested();
        waitUntil([&] { return exports == 1 && visiblePins().size() == 1; },
                  "edited image creates its own pinned window");
        const auto first = visiblePins().first()->persistenceSnapshot();
        require(first.image == expected && editor.modal()->isOpen() && !editor.dirty() &&
                    source.image == fixture(),
                "controller pins edited pixels without closing the editor or changing its source");
        editor.palette()->pinRequested();
        waitUntil([&] { return exports == 2 && visiblePins().size() == 2; },
                  "a second editor activation creates a separate pin");
        require(visiblePins()[0]->persistenceSnapshot().id !=
                    visiblePins()[1]->persistenceSnapshot().id,
                "successive editor pins have independent persistent identities");
        editor.shutdown();
        closePins();
        return EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--automatic-selection-tests"))) {
        canvas_automatic_selection_test::exercise([](auto verify) {
            ScreenshotController controller;
            controller.prewarmResources();
            SnowCanvasWidget* canvas = nullptr;
            waitUntil(
                [&] {
                    for (auto* widget : QApplication::topLevelWidgets()) {
                        if (auto* overlay = qobject_cast<ScreenshotOverlayWindow*>(widget)) {
                            canvas = overlay->canvas();
                            if (canvas)
                                return true;
                        }
                    }
                    return false;
                },
                "prewarmed screenshot controller owns a canvas");
            verify(*canvas);
        });
        storage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--editable-pin-tests"))) {
        capturePinKeepsEditableAnnotations();
        capturePinCommitsPendingText();
        capturePinSamplesOnlyCroppedImage();
        editablePinRejectsInvalidDocument();
        QApplication::clipboard()->clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        storage.shutdown();
        return EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--copy-after-pin-tests"))) {
        capturePinCopiesOnlyWhenEnabled();
        QApplication::clipboard()->clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        storage.shutdown();
        return EXIT_SUCCESS;
    }
    {
        ScreenshotController controller;
        combinedCopiesPreserveImagePlacementAndAppearance(controller);
        fileOnlyClipboardStillPinsEveryFile(controller);
    }
    imageToolsPreferInlineClipboardPixels();
    QApplication::clipboard()->clear();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    storage.shutdown();
    return EXIT_SUCCESS;
}
