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
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/collagewindow.h"
#include "snow_shot/presentation/imagesplitwindow.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "widgets/modal.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../src/presentation/pinned/screenshotclipboardplacementgeometry.h"

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
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <cstdlib>
#include <iostream>

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
        if (pin && pin->isVisible() && !pin->persistenceSnapshot().image.isNull())
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

    for (int scenario = 0; scenario < 4; ++scenario) {
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
            waitUntil([] { return visiblePins().size() == 1; }, "combined copy must create a pin");
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
        snow_shot::presentation::CollageWindow collage;
        collage.present();
        collage.paste();
        waitUntil([&] { return !collage.loading() && collage.document()->items().size() == 1; },
                  "collage must import the combined clipboard");
        require(samePixels(collage.document()->items().constFirst().image),
                "collage must use inline pixels instead of the exported file");
        collage.shutdown();
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
} // namespace

#ifdef Q_OS_MACOS
namespace {
void screenshotSessionFocusRouting() {
    using namespace snow_shot::platform::macos;
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
        {
            ScreenshotController controller;
            ScreenshotSelectionParams selection;
            selection.selection = QRect(0, 0, 100, 60);
            ScreenshotControllerTestAccess::prepareSelection(controller, fixture(), selection);
            ScreenshotControllerTestAccess::installSessionFocus(
                controller, [&] { return std::make_unique<Target>(controller, restores, live); });
            auto& commands = ScreenshotControllerTestAccess::commands(controller);
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
            }
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        require(live == 0 && (action != 7 || restores == 0),
                "shutdown must discard the target and pending restoration");
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
