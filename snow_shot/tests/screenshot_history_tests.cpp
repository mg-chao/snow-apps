#include "snow_shot/presentation/screenshothistoryservice.h"
#include "snow_shot/presentation/directcapturehistory.h"
#include "snowimageqtcodec.h"

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotoverlayshortcutcontroller.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <QWidget>
#include <QWindow>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <utility>

namespace storage = snow_shot::storage;

namespace {
using snow_shot::platform::PhysicalCursorDirection;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool dispatchShortcut(QWidget& receiver, Qt::Key key,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool autoRepeat = false) {
    QKeyEvent shortcutOverride(QEvent::ShortcutOverride, key, modifiers, QString(), autoRepeat);
    shortcutOverride.setAccepted(false);
    QCoreApplication::sendEvent(&receiver, &shortcutOverride);

    QKeyEvent keyPress(QEvent::KeyPress, key, modifiers, QString(), autoRepeat);
    keyPress.setAccepted(false);
    QCoreApplication::sendEvent(&receiver, &keyPress);
    return keyPress.isAccepted();
}

bool dispatchShortcutRelease(QWidget& receiver, Qt::Key key,
                             Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                             bool autoRepeat = false) {
    QKeyEvent keyRelease(QEvent::KeyRelease, key, modifiers, QString(), autoRepeat);
    keyRelease.setAccepted(false);
    QCoreApplication::sendEvent(&receiver, &keyRelease);
    return keyRelease.isAccepted();
}

ScreenshotHistoryEntry takeSnapshot(std::optional<ScreenshotHistoryEntry> snapshot,
                                    const char* message) {
    if (!snapshot.has_value()) {
        std::cerr << message << '\n';
        std::exit(1);
    }
    return std::move(*snapshot);
}

void waitForNavigation(ScreenshotHistoryService& history, const char* timeoutMessage) {
    QElapsedTimer timer;
    timer.start();
    while (history.navigationInProgress() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(!history.navigationInProgress(), timeoutMessage);
}

QImage solidImage(const QSize& size, QRgb color) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(color);
    return image;
}

QDir historyDirectory(const QString& configurationDirectory) {
    return QDir(QDir(configurationDirectory).filePath(QStringLiteral("capture_history/records")));
}

CapturedDisplayModel display(QString stableId, QString name, QRect canvasRect, QImage image) {
    CapturedDisplayModel result;
    result.stableId = std::move(stableId);
    result.name = std::move(name);
    result.physicalRect = canvasRect;
    result.canvasRect = canvasRect;
    result.imageSourceCanvasRect = canvasRect;
    result.logicalRect = canvasRect;
    result.image = std::move(image);
    result.active = true;
    return result;
}

void requireCanvasHistoryPayload(const QByteArray& payload) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            "canvas history payload is not valid JSON");
    const QJsonObject object = document.object();
    require(object.size() == 3 && object.value(QStringLiteral("schemaVersion")).isDouble() &&
                object.value(QStringLiteral("document")).isObject() &&
                object.value(QStringLiteral("history")).isObject(),
            "canvas history payload contains screenshot-local editor state");
}

void directImagesPersistWithoutTouchingTheEditor(
    const QString& root, snow_shot::presentation::DirectCaptureTarget target,
    const QRect& physicalBounds, bool legacy = false) {
    using namespace snow_shot::presentation;
    DirectCaptureRequest request;
    request.target = target;
    request.requestedAt = QDateTime::currentDateTimeUtc();
    QImage image = solidImage(physicalBounds.size(), qRgb(24, 50, 70));
    image.setPixel(5, 5, qRgb(210, 90, 30));
    DirectCaptureFrame frame{image, physicalBounds, QStringLiteral("target:123"), 2, {}};
    frame.canonicalPng = snow_shot::image_codec::encodePng(image);
    frame.displays.push_back({image, physicalBounds, frame.identity, frame.identity});
    QString id;
    {
        auto repository = storage::makeCaptureHistoryRepository(root);
        auto draft = directCaptureHistoryDraft(request, frame);
        require(draft.preparedResultImage.has_value() &&
                    draft.preparedResultImage->bytes().constData() ==
                        frame.canonicalPng.constData(),
                "direct capture history did not reuse the clipboard PNG");
        // Preserve coverage for image-only records written before desktop retention was restored.
        draft.contentKind = storage::CaptureHistoryContentKind::Image;
        draft.canvasBounds = physicalBounds;
        draft.selection.rectangle = physicalBounds;
        draft.displays.front().sourceCanvasOrigin = physicalBounds.topLeft();
        require(draft.displays.size() == 1 && draft.displays.front().image == image &&
                    draft.resultImage == image &&
                    draft.selection.rectangle == frame.physicalBounds &&
                    draft.selection.cornerRadius == 0 && draft.selection.shadowWidth == 0,
                "direct history did not store exactly the raw target");
        requireCanvasHistoryPayload(draft.canvasHistory);
        if (legacy) {
            draft.canvasBounds = image.rect();
            draft.selection.rectangle = image.rect();
            draft.displays.front().sourceCanvasOrigin.reset();
        }
        const auto result = repository->publish(std::move(draft)).get();
        require(result.storage.success, "direct history publication failed");
        id = result.record.id;
    }
    auto repository = storage::makeCaptureHistoryRepository(root);
    const auto records = repository->records();
    require(records.size() == 1 &&
                records.front().contentKind == storage::CaptureHistoryContentKind::Image &&
                records.front().source == (target == DirectCaptureTarget::FocusedWindow
                                               ? storage::CaptureHistorySource::FocusedWindow
                                               : storage::CaptureHistorySource::CurrentMonitor),
            "direct history metadata did not survive restart");
    const auto restoredImage = repository->loadResultImage(records.front());
    require(restoredImage.has_value() && restoredImage->convertToFormat(QImage::Format_ARGB32) ==
                                             image.convertToFormat(QImage::Format_ARGB32),
            "direct history pixels did not survive restart");
    ScreenshotDisplaySession displays;
    const QImage liveImage = solidImage(QSize(200, 120), qRgb(100, 120, 140));
    displays.appendDisplay(display(QStringLiteral("A"), QStringLiteral("Left"),
                                   QRect(-100, -50, 100, 180), liveImage));
    displays.appendDisplay(
        display(QStringLiteral("B"), QStringLiteral("Right"), QRect(0, -50, 100, 180), liveImage));
    displays.displayAt(1).logicalRect = QRect(0, -40, 80, 144);
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(-90, 20, 30, 40));
    const QRect liveSelection = selection.pixelSelection();
    ScreenshotInteractionState interaction;
    interaction.confirmSelection();
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent},
                                     *repository);
    require(history.navigateToRecord(id), "direct image history could not be opened");
    // A publication during the asynchronous load must not invalidate its target index.
    request.requestedAt = request.requestedAt.addSecs(1);
    request.target = DirectCaptureTarget::CurrentMonitor;
    require(repository->publish(directCaptureHistoryDraft(request, frame)).get().storage.success,
            "second direct capture failed to publish");
    history.refreshMetadata();
    waitForNavigation(history, "direct image navigation timed out");
    const QRect expectedBounds = legacy ? QRect(QPoint(-100, -50), image.size()) : physicalBounds;
    require(selection.pixelSelection() == expectedBounds, "direct image selection was misplaced");
    for (int i = 0; i < 2; ++i) {
        require(displays.displayAt(i).image.convertToFormat(QImage::Format_ARGB32) ==
                        image.convertToFormat(QImage::Format_ARGB32) &&
                    displays.displayAt(i).imageSourceCanvasRect == expectedBounds,
                "direct image pixels and selection use different canvas origins");
    }
    const QRect editedSelection = expectedBounds.adjusted(3, 3, -3, -3);
    selection.setSelectionRect(editedSelection);
    interaction.confirmSelection();
    auto edited = takeSnapshot(history.snapshotCurrent(true), "direct history could not be edited");
    const QString editedId = edited.id;
    history.commit(std::move(edited));
    history.drainPendingWrites();
    require(history.returnToCurrentScreenshot(),
            "direct image history lost the live editor endpoint");
    require(selection.pixelSelection() == liveSelection && displays.displayAt(0).image == liveImage,
            "direct image history changed the live editor");
    require(history.navigateToRecord(editedId), "edited direct history could not be reopened");
    waitForNavigation(history, "edited direct image navigation timed out");
    require(selection.pixelSelection() == editedSelection,
            "edited direct history lost the selection");
    for (int i = 0; i < 2; ++i) {
        require(displays.displayAt(i).imageSourceCanvasRect == expectedBounds,
                "saving an edited direct capture lost its image origin");
    }
    const auto& restoredDisplay = displays.displayAt(0);
    const QImage rendered =
        runtime.renderToImage(editedSelection, editedSelection.size(),
                              {{restoredDisplay.image, restoredDisplay.imageSourceCanvasRect}});
    require(rendered.convertToFormat(QImage::Format_ARGB32) ==
                image.copy(QRect(QPoint(3, 3), editedSelection.size()))
                    .convertToFormat(QImage::Format_ARGB32),
            "edited direct capture exported pixels from the wrong region");
}

void directCaptureHistoryPreservesSelectionRegions(const QString& root) {
    using snow_shot::presentation::DirectCaptureTarget;
    directImagesPersistWithoutTouchingTheEditor(QDir(root).filePath(QStringLiteral("window")),
                                                DirectCaptureTarget::FocusedWindow,
                                                QRect(-60, -25, 137, 91));
    directImagesPersistWithoutTouchingTheEditor(QDir(root).filePath(QStringLiteral("left-monitor")),
                                                DirectCaptureTarget::CurrentMonitor,
                                                QRect(-100, -50, 100, 180));
    directImagesPersistWithoutTouchingTheEditor(
        QDir(root).filePath(QStringLiteral("right-monitor")), DirectCaptureTarget::CurrentMonitor,
        QRect(0, -50, 100, 180));
    directImagesPersistWithoutTouchingTheEditor(QDir(root).filePath(QStringLiteral("legacy")),
                                                DirectCaptureTarget::FocusedWindow,
                                                QRect(-1500, -300, 137, 91), true);
}

void explicitHistoryEditSeesExternalPublications(const QString& root) {
    using namespace snow_shot::presentation;
    auto repository = storage::makeCaptureHistoryRepository(root);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("A"), QStringLiteral("Display A"),
                                   QRect(0, 0, 200, 120),
                                   solidImage(QSize(200, 120), qRgb(10, 20, 30))));
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent},
                                     *repository);
    DirectCaptureRequest request;
    request.requestedAt = QDateTime::currentDateTimeUtc();
    DirectCaptureFrame frame{solidImage(QSize(80, 60), qRgb(40, 50, 60)),
                             QRect(20, 30, 80, 60),
                             QStringLiteral("A"),
                             2,
                             {}};
    const QImage desktop = solidImage(QSize(200, 120), qRgb(40, 50, 60));
    frame.displays.push_back({desktop, desktop.rect(), frame.identity, frame.identity});
    const auto published = repository->publish(directCaptureHistoryDraft(request, frame)).get();
    require(published.storage.success, "external history publication failed");
    require(history.navigateToRecord(published.record.id),
            "Edit ignored a direct capture published after the editor was constructed");
    waitForNavigation(history, "external history edit timed out");
    require(selection.pixelSelection() == frame.physicalBounds &&
                displays.displayAt(0).image.convertToFormat(QImage::Format_ARGB32) ==
                    desktop.convertToFormat(QImage::Format_ARGB32),
            "Edit did not load the externally published capture");
}

void directCaptureRetainsTheWholeDesktop(const QString& root) {
    using namespace snow_shot::presentation;
    const QVector<DirectCaptureDisplay> capturedDisplays{
        {solidImage(QSize(100, 120), qRgb(110, 20, 30)), QRect(-100, -20, 100, 120),
         QStringLiteral("A"), QStringLiteral("Left")},
        {solidImage(QSize(160, 100), qRgb(20, 120, 30)), QRect(0, 0, 160, 100), QStringLiteral("B"),
         QStringLiteral("Right")}};
    for (const auto target :
         {DirectCaptureTarget::FocusedWindow, DirectCaptureTarget::CurrentMonitor}) {
        const QString directory = QDir(root).filePath(QString::number(static_cast<int>(target)));
        DirectCaptureRequest request;
        request.target = target;
        request.requestedAt = QDateTime::currentDateTimeUtc();
        DirectCaptureFrame frame;
        frame.displays = capturedDisplays;
        frame.image = target == DirectCaptureTarget::FocusedWindow
                          ? solidImage(QSize(80, 60), qRgb(40, 50, 160))
                          : capturedDisplays.back().image;
        if (target == DirectCaptureTarget::FocusedWindow) {
            frame.image.setPixel(0, 0, qRgba(10, 20, 30, 0));
            frame.image.setPixel(1, 0, qRgba(40, 50, 60, 64));
            frame.image.setPixel(2, 0, qRgba(70, 80, 90, 128));
        }
        frame.physicalBounds = target == DirectCaptureTarget::FocusedWindow
                                   ? QRect(-30, 10, 80, 60)
                                   : capturedDisplays.back().physicalBounds;
        frame.identity = QStringLiteral("target");
        QString id;
        {
            auto repository = storage::makeCaptureHistoryRepository(directory);
            auto draft = directCaptureHistoryDraft(request, frame);
            require(draft.contentKind == storage::CaptureHistoryContentKind::ScreenshotSession &&
                        draft.canvasBounds == QRect(0, 0, 260, 120) &&
                        draft.selection.rectangle == frame.physicalBounds.translated(100, 20) &&
                        draft.displays.size() == capturedDisplays.size(),
                    "direct capture did not persist a complete desktop selection session");
            const auto published = repository->publish(draft).get();
            require(published.storage.success, "complete direct history publication failed");
            id = published.record.id;
        }
        auto repository = storage::makeCaptureHistoryRepository(directory);
        const auto record = repository->records().front();
        const auto result = repository->loadResultImage(record);
        require(result && result->convertToFormat(QImage::Format_ARGB32) ==
                              frame.image.convertToFormat(QImage::Format_ARGB32),
                "direct history lost the separate window or monitor result image");
        ScreenshotDisplaySession displays;
        for (const auto& captured : capturedDisplays) {
            displays.appendDisplay(display(captured.stableId, captured.name,
                                           captured.physicalBounds,
                                           solidImage(captured.image.size(), qRgb(0, 0, 0))));
        }
        ScreenshotGeometryMapper geometry;
        geometry.rebuild(displays);
        displays.displayAt(1).logicalRect = QRect(0, 0, 128, 80);
        SnowCanvasRuntime runtime;
        ScreenshotSelectionModel selection;
        ScreenshotInteractionState interaction;
        interaction.enterOverlayVisible(false);
        ScreenshotIntelligentSelectionModel intelligent;
        ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent},
                                         *repository);
        require(history.navigateToRecord(id), "complete direct history could not be edited");
        waitForNavigation(history, "complete direct history edit timed out");
        require(
            selection.pixelSelection() ==
                geometry.canvasRectForPhysicalRect(displays, frame.physicalBounds).toAlignedRect(),
            "complete direct history restored the wrong target selection");
        for (qsizetype index = 0; index < capturedDisplays.size(); ++index) {
            require(displays.displayAt(index).image.convertToFormat(QImage::Format_ARGB32) ==
                            capturedDisplays[index].image.convertToFormat(QImage::Format_ARGB32) &&
                        displays.displayAt(index).imageSourceCanvasRect ==
                            displays.displayAt(index).canvasRect,
                    "editing direct history lost a display image or its position");
        }
        selection.setSelectionRect(displays.displayAt(0).canvasRect);
        const auto expanded = takeSnapshot(history.snapshotCurrent(true),
                                           "could not expand direct history onto another display");
        require(expanded.displays.size() == 2 &&
                    expanded.selection.selection == displays.displayAt(0).canvasRect,
                "direct history could not be edited beyond the original capture target");
    }
    DirectCaptureRequest request;
    DirectCaptureFrame incomplete;
    incomplete.image = capturedDisplays.front().image;
    incomplete.physicalBounds = capturedDisplays.front().physicalBounds;
    require(directCaptureHistoryDraft(request, incomplete).id.isEmpty(),
            "direct history accepted a capture without its desktop images");
}

void navigationMatchesDisplaysAndRestoresLiveEndpoint(const QString& root) {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("A"), QStringLiteral("Left"),
                                   QRect(0, 0, 100, 80),
                                   solidImage(QSize(60, 40), qRgba(255, 0, 0, 255))));
    displays.appendDisplay(display(QStringLiteral("B"), QStringLiteral("Right"),
                                   QRect(100, 0, 100, 80),
                                   solidImage(QSize(80, 60), qRgba(0, 255, 0, 255))));

    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 10, 170, 60));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(true);
    ScreenshotIntelligentSelectionModel intelligent;
    require(intelligent.applyCanvasHitPath({QRectF(10, 10, 170, 60)}, QRectF(0, 0, 200, 80), 1.0),
            "failed to initialize intelligent selection");

    int presentationChanges = 0;
    int intelligentSelectionRequests = 0;
    QVector<bool> loadingStates;
    ScreenshotHistoryService history(
        ScreenshotHistoryServiceContext{
            displays,
            runtime,
            selection,
            interaction,
            intelligent,
            [&presentationChanges]() { ++presentationChanges; },
            [&loadingStates](bool loading) { loadingStates.push_back(loading); },
            [&intelligentSelectionRequests]() { ++intelligentSelectionRequests; },
        },
        root);
    auto saved = takeSnapshot(history.snapshotCurrent(true), "failed to create history entry");
    requireCanvasHistoryPayload(saved.canvasHistory);
    history.commit(std::move(saved));

    std::swap(displays.displayAt(0).stableId, displays.displayAt(1).stableId);
    std::swap(displays.displayAt(0).name, displays.displayAt(1).name);
    displays.displayAt(0).image = solidImage(QSize(100, 80), qRgba(0, 0, 255, 255));
    displays.displayAt(1).image = solidImage(QSize(100, 80), qRgba(255, 255, 0, 255));
    const QImage liveFirst = displays.displayAt(0).image;
    const QImage liveSecond = displays.displayAt(1).image;
    selection.setSelectionRect(QRectF(20, 15, 40, 30));

    require(history.navigatePrevious(), "previous history navigation failed");
    require(history.navigationInProgress(),
            "persistent history navigation did not start asynchronously");
    require(displays.displayAt(0).image == liveFirst && displays.displayAt(1).image == liveSecond,
            "asynchronous history navigation changed displays before completion");
    require(!history.navigatePrevious(), "concurrent history navigation was accepted");
    waitForNavigation(history, "previous history navigation timed out");
    require(interaction.manualSelecting(), "persistent entry did not enter manual mode");
    require(displays.displayAt(0).image.pixel(0, 0) == qRgba(0, 255, 0, 255),
            "stable-id monitor matching failed");
    require(displays.displayAt(0).imageSourceCanvasRect == QRect(0, 0, 80, 60),
            "historical image was not placed at native size");
    require(loadingStates == QVector<bool>({true, false}),
            "current-session disk loading did not bracket navigation");
    require(!history.navigatePrevious(), "oldest boundary should be a no-op");
    require(intelligentSelectionRequests == 0,
            "historical entry unexpectedly requested intelligent selection");

    require(history.navigateNext(), "live endpoint navigation failed");
    require(interaction.intelligentSelecting(), "live intelligent mode was not restored");
    require(displays.displayAt(0).image == liveFirst, "first live image was not restored");
    require(displays.displayAt(1).image == liveSecond, "second live image was not restored");
    require(presentationChanges == 2, "unexpected presentation update count");
    require(intelligentSelectionRequests == 1,
            "returning to live did not request intelligent selection exactly once");

    const QRect updatedLiveSelection(30, 20, 50, 40);
    selection.setSelectionRect(updatedLiveSelection);
    interaction.confirmSelection();
    require(history.navigatePrevious(), "second history navigation failed");
    waitForNavigation(history, "second history navigation timed out");
    require(history.navigateNext(), "second live endpoint navigation failed");
    require(interaction.movingSelection(), "updated live selection stage was not recorded");
    require(selection.pixelSelection() == updatedLiveSelection,
            "updated live selection was not recorded");
    require(intelligentSelectionRequests == 1,
            "confirmed live selection unexpectedly requested intelligent selection");
    require(presentationChanges == 4, "second traversal did not update presentation");
    history.drainPendingWrites();
}

void navigationSharesCanvasCreationStyles(const QString& root) {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 64, 64),
                                   solidImage(QSize(64, 64), qRgba(20, 30, 40, 255))));
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 64, 64));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent, {}},
                                     root);

    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "failed to activate the historical shape tool");
    SnowCanvasShapeStyle historicalStyle = canvas.canvasStyleToolbarState().shapeStyle;
    historicalStyle.stroke = QColor(17, 34, 51, 255);
    historicalStyle.strokeWidth = 13.0;
    require(canvas.setCanvasShapeStylePatch(historicalStyle,
                                            SnowCanvasShapeStylePropertyStrokeColor |
                                                SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::Rectangle),
            "failed to configure the historical creation style");
    auto entry =
        takeSnapshot(history.snapshotCurrent(true), "failed to snapshot the styled history entry");
    requireCanvasHistoryPayload(entry.canvasHistory);
    history.commit(std::move(entry));

    SnowCanvasShapeStyle liveStyle = historicalStyle;
    liveStyle.stroke = QColor(204, 85, 102, 255);
    liveStyle.strokeWidth = 7.0;
    require(canvas.setCanvasShapeStylePatch(liveStyle,
                                            SnowCanvasShapeStylePropertyStrokeColor |
                                                SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::Rectangle),
            "failed to configure the live creation style");

    require(history.navigatePrevious(), "styled history navigation failed");
    waitForNavigation(history, "styled history navigation timed out");
    require(canvas.canvasTool() == SnowCanvasTool::Select,
            "history navigation restored a transient canvas tool");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "failed to reactivate shape after historical restore");
    const SnowCanvasShapeStyle restoredHistorical = canvas.canvasStyleToolbarState().shapeStyle;
    require(restoredHistorical.stroke == liveStyle.stroke &&
                restoredHistorical.strokeWidth == liveStyle.strokeWidth,
            "history navigation did not retain the shared canvas creation style");

    SnowCanvasShapeStyle sharedStyle = restoredHistorical;
    sharedStyle.stroke = QColor(68, 136, 204, 255);
    sharedStyle.strokeWidth = 5.0;
    require(canvas.setCanvasShapeStylePatch(sharedStyle,
                                            SnowCanvasShapeStylePropertyStrokeColor |
                                                SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::Rectangle),
            "failed to change the shared creation style from screenshot history");

    require(history.navigateNext(), "styled live navigation failed");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "failed to reactivate shape after live restore");
    const SnowCanvasShapeStyle restoredLive = canvas.canvasStyleToolbarState().shapeStyle;
    require(restoredLive.stroke == sharedStyle.stroke &&
                restoredLive.strokeWidth == sharedStyle.strokeWidth,
            "style changed in screenshot history was not shared with the live screenshot");
    history.drainPendingWrites();
}

void fullSessionEntriesRemainReadable(const QString& root) {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 32, 32),
                                   solidImage(QSize(32, 32), qRgba(12, 34, 56, 255))));
    const QRgb storedPixel = displays.displayAt(0).image.pixel(0, 0);
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 32, 32));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent, {}},
                                     root);

    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "failed to activate shape for the full-session entry");
    SnowCanvasShapeStyle fullSessionStyle = canvas.canvasStyleToolbarState().shapeStyle;
    fullSessionStyle.strokeWidth = 13.0;
    require(canvas.setCanvasShapeStylePatch(fullSessionStyle,
                                            SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::Rectangle),
            "failed to configure the full-session style");
    auto entry = takeSnapshot(history.snapshotCurrent(true),
                              "failed to snapshot the full-session history entry");
    entry.canvasHistory = runtime.serializeDocumentSession();
    require(!entry.canvasHistory.isEmpty(), "failed to create a full-session canvas payload");
    history.commit(std::move(entry));

    SnowCanvasShapeStyle sharedStyle = fullSessionStyle;
    sharedStyle.strokeWidth = 7.0;
    require(canvas.setCanvasShapeStylePatch(sharedStyle, SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::Rectangle),
            "failed to configure the shared style after the full-session snapshot");
    displays.displayAt(0).image = solidImage(QSize(32, 32), qRgba(200, 210, 220, 255));
    require(history.navigatePrevious(), "full-session history navigation failed");
    waitForNavigation(history, "full-session history navigation timed out");
    require(displays.displayAt(0).image.pixel(0, 0) == storedPixel,
            "the full-session canvas payload was not restored");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "failed to reactivate shape after restoring the full-session entry");
    require(canvas.canvasStyleToolbarState().shapeStyle.strokeWidth == sharedStyle.strokeWidth,
            "full-session navigation restored its screenshot-local creation style");
    history.drainPendingWrites();
}

void persistenceAndExactRetentionCutoff(const QString& root) {
    QDateTime now =
        QDateTime::fromString(QStringLiteral("2026-08-03T12:00:00.000Z"), Qt::ISODateWithMs);
    QDateTime cutoff = now.addDays(-7);

    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 64, 64),
                                   solidImage(QSize(64, 64), qRgba(20, 30, 40, 255))));
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 64, 64));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;

    {
        ScreenshotHistoryService writer(
            {displays, runtime, selection, interaction, intelligent, {}}, root,
            [cutoff]() { return cutoff; });
        auto entry = takeSnapshot(writer.snapshotCurrent(true), "failed to snapshot cutoff entry");
        writer.commit(std::move(entry));
        writer.drainPendingWrites();
    }

    QVector<bool> loadingStates;
    ScreenshotHistoryService reader(
        {
            displays,
            runtime,
            selection,
            interaction,
            intelligent,
            {},
            [&loadingStates](bool loading) { loadingStates.push_back(loading); },
        },
        root, [now]() { return now; });
    require(reader.navigatePrevious(), "entry exactly at cutoff was pruned");
    waitForNavigation(reader, "cutoff entry navigation timed out");
    require(loadingStates == QVector<bool>({true, false}),
            "lazy history loading did not bracket the restore");
    reader.resetCaptureNavigation();
}

void corruptLazyEntryDoesNotBlockOlderEntries(const QString& root) {
    QDateTime clock =
        QDateTime::fromString(QStringLiteral("2026-08-03T12:00:00.000Z"), Qt::ISODateWithMs);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 64, 64),
                                   solidImage(QSize(64, 64), qRgba(255, 0, 0, 255))));
    const QRgb olderPixel = displays.displayAt(0).image.pixel(0, 0);
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 64, 64));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;

    QString newerId;
    {
        ScreenshotHistoryService writer(
            {displays, runtime, selection, interaction, intelligent, {}}, root,
            [&clock]() { return clock; });
        auto older = takeSnapshot(writer.snapshotCurrent(true), "failed to snapshot older entry");
        writer.commit(std::move(older));
        clock = clock.addSecs(1);
        displays.displayAt(0).image = solidImage(QSize(64, 64), qRgba(0, 255, 0, 255));
        auto newer = takeSnapshot(writer.snapshotCurrent(true), "failed to snapshot newer entry");
        newerId = newer.id;
        writer.commit(std::move(newer));
        writer.drainPendingWrites();
    }

    displays.displayAt(0).image = solidImage(QSize(64, 64), qRgba(0, 0, 255, 255));
    QVector<bool> loadingStates;
    ScreenshotHistoryService reader(
        {
            displays,
            runtime,
            selection,
            interaction,
            intelligent,
            {},
            [&loadingStates](bool loading) { loadingStates.push_back(loading); },
        },
        root, [&clock]() { return clock; });
    const QFileInfoList directories =
        historyDirectory(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    require(directories.size() == 2, "history entries were not persisted");
    QFile corrupt(QDir(historyDirectory(root).filePath(newerId))
                      .filePath(QStringLiteral("canvas_history.json")));
    require(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "failed to open session for corruption");
    require(corrupt.write("{") == 1, "failed to corrupt session");
    corrupt.close();

    const QImage liveImage = displays.displayAt(0).image;
    const QByteArray liveSession = runtime.serializeDocumentSession();
    require(reader.navigatePrevious(), "corrupt entry load was not started");
    waitForNavigation(reader, "corrupt entry navigation timed out");
    require(displays.displayAt(0).image == liveImage, "failed navigation changed the live image");
    require(runtime.serializeDocumentSession() == liveSession,
            "failed navigation changed the live canvas session");
    require(loadingStates == QVector<bool>({true, false}),
            "failed history loading left the loading state active");
    require(reader.navigatePrevious(), "corrupt entry blocked an older entry");
    waitForNavigation(reader, "older valid entry navigation timed out");
    require(displays.displayAt(0).image.pixel(0, 0) == olderPixel,
            "older valid entry was not restored");
    require(loadingStates == QVector<bool>({true, false, true, false}),
            "loading state did not cover the valid entry after a failure");
}

void expiredCurrentEntryCanReturnToConfirmedLiveSelection(const QString& root) {
    QDateTime clock =
        QDateTime::fromString(QStringLiteral("2026-08-03T12:00:00.000Z"), Qt::ISODateWithMs);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 64, 64),
                                   solidImage(QSize(64, 64), qRgba(255, 0, 0, 255))));
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 64, 64));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent, {}},
                                     root, [&clock]() { return clock; });
    auto entry = takeSnapshot(history.snapshotCurrent(true), "failed to snapshot expiring entry");
    history.commit(std::move(entry));
    history.drainPendingWrites();

    displays.displayAt(0).image = solidImage(QSize(64, 64), qRgba(0, 0, 255, 255));
    const QRect liveSelection(7, 8, 30, 31);
    selection.setSelectionRect(liveSelection);
    const QImage liveImage = displays.displayAt(0).image;
    require(history.navigatePrevious(), "failed to browse expiring entry");
    require(interaction.movingSelection(),
            "manual live selection was not confirmed before history navigation");
    waitForNavigation(history, "expiring entry navigation timed out");
    clock = clock.addDays(8);
    require(history.navigateNext(), "expired entry could not return to live");
    require(displays.displayAt(0).image == liveImage, "live image was not restored after pruning");
    require(interaction.movingSelection(),
            "returning to live did not restore the confirmed selection stage");
    require(selection.pixelSelection() == liveSelection,
            "returning to live did not restore the confirmed selection");
}

void multipleValidEntriesCanBeTraversed(const QString& root) {
    QDateTime clock = QDateTime::currentDateTimeUtc();
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 64, 64),
                                   solidImage(QSize(64, 64), qRgba(255, 0, 0, 255))));
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(1, 1, 20, 20));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent, {}},
                                     root, [&clock]() { return clock; });

    const QRgb olderPixel = displays.displayAt(0).image.pixel(0, 0);
    auto older =
        takeSnapshot(history.snapshotCurrent(true), "failed to snapshot older traversal entry");
    const QString olderId = older.id;
    history.commit(std::move(older));

    displays.displayAt(0).image = solidImage(QSize(64, 64), qRgba(0, 255, 0, 255));
    const QRgb newerPixel = displays.displayAt(0).image.pixel(0, 0);
    clock = clock.addSecs(1);
    selection.setSelectionRect(QRectF(2, 2, 30, 30));
    auto newer =
        takeSnapshot(history.snapshotCurrent(true), "failed to snapshot newer traversal entry");
    history.commit(std::move(newer));

    displays.displayAt(0).image = solidImage(QSize(64, 64), qRgba(0, 0, 255, 255));
    const QImage liveImage = displays.displayAt(0).image;
    selection.setSelectionRect(QRectF(3, 3, 40, 40));
    require(!history.navigateToRecord(QStringLiteral("missing-record")) &&
                !history.navigationInProgress() && displays.displayAt(0).image == liveImage,
            "unknown direct history navigation changed the live endpoint");
    require(history.navigateToRecord(olderId), "failed to navigate directly to older entry");
    waitForNavigation(history, "direct older entry navigation timed out");
    require(displays.displayAt(0).image.pixel(0, 0) == olderPixel,
            "direct navigation did not apply the requested older entry");
    require(history.navigateNext(), "failed to navigate from older to newer entry");
    waitForNavigation(history, "newer entry navigation timed out");
    require(displays.displayAt(0).image.pixel(0, 0) == newerPixel,
            "newer traversal entry was not applied after direct navigation");
    require(history.returnToCurrentScreenshot() && displays.displayAt(0).image == liveImage,
            "direct return did not restore the live endpoint");
}

void committedSelectionFollowsHistory(const QString& root) {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("A"), QStringLiteral("Primary"),
                                   QRect(0, 0, 200, 100),
                                   solidImage(QSize(200, 100), qRgba(255, 0, 0, 255))));
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    ScreenshotInteractionState interaction;
    ScreenshotIntelligentSelectionModel intelligent;
    QVector<ScreenshotSelectionParams> committed;
    ScreenshotHistoryServiceContext context{displays, runtime, selection, interaction, intelligent};
    context.selectionCommitted = [&](const ScreenshotSelectionParams& params) {
        committed.push_back(params);
    };
    ScreenshotHistoryService history(std::move(context), root);
    for (const auto source : {storage::CaptureHistorySource::CopiedToClipboard,
                              storage::CaptureHistorySource::SavedToFile,
                              storage::CaptureHistorySource::PinnedToScreen}) {
        const qsizetype before = committed.size();
        selection.setSelectionRect(QRect(10 + static_cast<int>(before), 20, 80, 50));
        auto entry = takeSnapshot(history.snapshotCurrent(true), "history snapshot failed");
        const ScreenshotSelectionParams exported = entry.selection;
        require(before == committed.size(), "snapshotting must not remember the selection");
        selection.setSelectionRect(QRect(100, 5, 30, 20));
        entry.source = source;
        history.commit(std::move(entry));
        require(committed.size() == before + 1 && committed.constLast() == exported,
                "history commit must remember the exported snapshot for every destination");
        history.drainPendingWrites();
    }
    const auto exportedSelections = committed;
    auto abandoned = history.snapshotCurrent(true);
    require(abandoned.has_value(), "abandoned export should have a valid snapshot");
    history.commit(ScreenshotHistoryEntry{});
    require(committed == exportedSelections,
            "uncommitted exports and invalid history must not replace the previous selection");
    require(history.navigatePrevious(), "history navigation should start");
    waitForNavigation(history, "history navigation timed out");
    require(history.returnToCurrentScreenshot(), "live screenshot should be restored");
    require(committed == exportedSelections,
            "browsing history must not replace the previous exported selection");
}

void historyKeysOnlyWorkDuringSelectionStates() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(true);
    intelligent.beginPress(QPointF(4, 4), QRectF(0, 0, 8, 8));
    QWidget shortcutWindow;
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&shortcutWindow);

    int previousCount = 0;
    int nextCount = 0;
    int pauseCount = 0;
    int showToolbarCount = 0;
    int selectionConfirmedCount = 0;
    int returnToCurrentCount = 0;
    int returnToIntelligentCount = 0;
    bool hasCurrentScreenshot = false;
    ScreenshotOverlayInputActions actions;
    actions.navigateHistoryPrevious = [&previousCount]() {
        ++previousCount;
        return true;
    };
    actions.navigateHistoryNext = [&nextCount]() {
        ++nextCount;
        return true;
    };
    actions.returnToCurrentScreenshot = [&returnToCurrentCount, &hasCurrentScreenshot]() {
        ++returnToCurrentCount;
        return hasCurrentScreenshot;
    };
    actions.returnToIntelligentSelection = [&returnToIntelligentCount](const QPoint&) {
        ++returnToIntelligentCount;
        return true;
    };
    actions.pauseIntelligentSelection = [&pauseCount]() { ++pauseCount; };
    actions.showToolbar = [&showToolbarCount]() { ++showToolbarCount; };
    actions.selectionConfirmed = [&selectionConfirmedCount]() { ++selectionConfirmedCount; };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        actions,
    });

    ScreenshotOverlayShortcutController shortcutController(shortcutManager, handler, interaction,
                                                           intelligent, actions);

    handler.confirmSelection();
    require(selectionConfirmedCount == 0 &&
                captureState.sessionState != ScreenshotSessionState::Editing &&
                !interaction.movingSelection(),
            "an empty selection must not trigger post-selection actions");

    require(dispatchShortcut(shortcutWindow, Qt::Key_Comma),
            "comma key was not handled during smart selection");
    require(previousCount == 1, "comma key did not navigate history during smart selection");
    require(!intelligent.pressActive(),
            "history navigation did not cancel the pending smart selection press");
    require(pauseCount == 1,
            "history navigation did not cancel the pending smart selection request");

    interaction.enterOverlayVisible(false);
    selection.setSelectionRect(QRectF(1, 2, 20, 21));
    require(dispatchShortcut(shortcutWindow, Qt::Key_Period),
            "period key was not handled during manual selection");
    require(nextCount == 1, "period key did not navigate history during manual selection");
    require(interaction.movingSelection(),
            "manual selection was not confirmed before history navigation");
    require(captureState.sessionState == ScreenshotSessionState::Editing,
            "confirming manual selection did not enter the editing session state");
    require(showToolbarCount == 1, "confirming manual selection did not show the toolbar");
    require(selectionConfirmedCount == 1,
            "confirming manual selection did not notify post-selection actions");

    require((handler.handleRightClick(nullptr, QPointF(4, 4)) ==
             ScreenshotOverlayRightClickResult::Handled),
            "right-click did not handle manual selection");
    require(returnToCurrentCount == 1,
            "right-click did not check for an active historical screenshot");
    require(returnToIntelligentCount == 1,
            "ordinary right-click did not return to intelligent selection");

    hasCurrentScreenshot = true;
    require((handler.handleRightClick(nullptr, QPointF(4, 4)) ==
             ScreenshotOverlayRightClickResult::Handled),
            "right-click did not handle historical selection");
    require(returnToCurrentCount == 2,
            "historical right-click did not return to the current screenshot");
    require(returnToIntelligentCount == 1,
            "historical right-click incorrectly returned to intelligent selection");

    static_cast<void>(interaction.enterSelectionDrag(ScreenshotSelectionDragMode::Marquee));
    require(dispatchShortcut(shortcutWindow, Qt::Key_Comma),
            "comma key was not handled during manual box selection");
    require(previousCount == 2, "comma key did not navigate history during manual box selection");
    require(!interaction.dragging(),
            "history navigation did not cancel the active manual selection drag");
    require(interaction.movingSelection(),
            "manual selection drag was not finalized before history navigation");

    interaction.confirmSelection();
    require(dispatchShortcut(shortcutWindow, Qt::Key_Period),
            "period key was not handled after confirming the selection");
    require(nextCount == 2, "period key did not navigate history after selection confirmation");
    require(pauseCount == 4,
            "history navigation did not consistently cancel smart selection requests");

    interaction.setCanvasTool(ScreenshotActiveTool::Shape);
    dispatchShortcut(shortcutWindow, Qt::Key_Comma);
    require(previousCount == 2, "comma key navigated history while editing");
}

void moveToolResizesSelectionFromOutsidePress() {
    ScreenshotCaptureState captureState;
    captureState.sessionState = ScreenshotSessionState::Editing;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 10, 20, 20));
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.confirmSelection();

    int overlayUpdates = 0;
    int guideLineUpdates = 0;
    int selectionConfirmedCount = 0;
    ScreenshotOverlayInputActions actions;
    actions.updateOverlayState = [&overlayUpdates]() { ++overlayUpdates; };
    actions.updateGuideLinesForOverlay =
        [&guideLineUpdates](ScreenshotOverlayWindow*, const QPointF&) { ++guideLineUpdates; };
    actions.selectionConfirmed = [&selectionConfirmedCount]() { ++selectionConfirmedCount; };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        std::move(actions),
    });

    require(handler.shouldHandleMouseEvent(nullptr, QPointF(50, 50), true),
            "Move must handle a press outside the confirmed selection as a resize");
    handler.handleMousePress(nullptr, QPointF(50, 50));
    require(interaction.manualSelecting() && interaction.modifyingSelection() &&
                interaction.dragging() && !interaction.movingSelection() &&
                interaction.dragMode() == ScreenshotSelectionDragMode::BottomRight,
            "Move press outside the selection did not enter a directional resize transaction");
    require(selection.normalizedSelection() == QRectF(10, 10, 20, 20),
            "Move press outside the selection replaced the selection with a new marquee");
    require(captureState.sessionState == ScreenshotSessionState::OverlayVisible,
            "outside resize entered the confirmed stage before release");
    require(overlayUpdates == 1, "outside resize did not refresh the overlay");
    require(guideLineUpdates == 1,
            "Move press outside the selection did not update the cursor guide lines");
    require(selectionConfirmedCount == 0, "outside resize was confirmed before release");

    handler.handleMouseMove(nullptr, QPointF(60, 70));
    handler.handleMouseRelease(nullptr, QPointF(60, 70));
    require(selection.normalizedSelection() == QRectF(10, 10, 30, 40),
            "outside Move drag did not resize the existing selection");
    require(interaction.movingSelection() && !interaction.dragging() &&
                captureState.sessionState == ScreenshotSessionState::Editing &&
                selectionConfirmedCount == 1,
            "outside resize did not confirm exactly once on release");
}

void externalSelectionSupportsHeldShortcuts() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("external"), QStringLiteral("external"),
                                   QRect(0, 0, 500, 500),
                                   solidImage(QSize(500, 500), qRgb(0, 0, 0))));
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    QWidget window;
    snow_shot::presentation::WindowShortcutManager manager;
    manager.addScopeWindow(&window);
    ScreenshotOverlayInputActions actions;
    ScreenshotOverlayInputHandler handler(
        {captureState, interaction, selection, intelligent, geometry, displays, actions});
    ScreenshotOverlayShortcutController shortcuts(manager, handler, interaction, intelligent,
                                                  actions);
    for (const auto modifiers : {Qt::NoModifier, Qt::ControlModifier, Qt::AltModifier,
                                 Qt::MetaModifier, Qt::ShiftModifier}) {
        interaction.enterOverlayVisible(false);
        handler.setExternalDragActive(true);
        handler.beginExternalSelectionDrag(QPointF(100, 100));
        handler.updateExternalSelectionDrag(QPointF(160, 140));
        require(selection.normalizedSelection() == QRectF(100, 100, 60, 40),
                "external drag must initialize a shared marquee");
        require(dispatchShortcut(window, Qt::Key_Space, modifiers),
                "external selection must accept Space with the activation modifier still held");
        handler.updateExternalSelectionDrag(QPointF(180, 170));
        require(selection.normalizedSelection() == QRectF(120, 130, 60, 40),
                "Space must translate the external selection without resizing it");
        require(dispatchShortcutRelease(window, Qt::Key_Space, modifiers),
                "Space release must restore the external marquee");
        handler.updateExternalSelectionDrag(QPointF(200, 180));
        require(selection.normalizedSelection() == QRectF(120, 130, 80, 50),
                "resumed external resizing must retain the translated anchor");
        require(dispatchShortcut(window, Qt::Key_Shift, Qt::ShiftModifier | modifiers),
                "external selection must accept the aspect ratio shortcut");
        handler.updateExternalSelectionDrag(QPointF(220, 190));
        require(selection.normalizedSelection().width() == selection.normalizedSelection().height(),
                "Shift must constrain external selection to a square");
        require(dispatchShortcutRelease(window, Qt::Key_Shift),
                "external aspect shortcut must release after activation modifiers change");
        require(!dispatchShortcut(window, Qt::Key_Comma) &&
                    !dispatchShortcut(window, Qt::Key_Return),
                "external drag must not allow history or premature confirmation");
        handler.setExternalDragActive(false);
        interaction.finishDrag();
    }
    handler.setExternalDragActive(true);
    interaction.enterOverlayVisible(false);
    handler.beginExternalSelectionDrag(QPointF(100, 100));
    require(dispatchShortcut(window, Qt::Key_Space), "early Space must be accepted");
    handler.updateExternalSelectionDrag(QPointF(160, 140));
    handler.updateExternalSelectionDrag(QPointF(100, 100));
    require(selection.normalizedSelection() == QRectF(40, 60, 60, 40),
            "moving back to the original press must preserve a nonempty selection");
    handler.updateExternalSelectionDrag(QPointF(-100, -100));
    require(selection.normalizedSelection() == QRectF(0, 0, 60, 40),
            "external translation must clamp at canvas bounds without resizing");
    static_cast<void>(dispatchShortcutRelease(window, Qt::Key_Space));
    handler.setExternalDragActive(false);
    interaction.finishDrag();
    interaction.enterOverlayVisible(false);
    handler.setExternalDragActive(true);
    handler.beginExternalSelectionDrag(QPointF(10, 10));
    handler.updateExternalSelectionDrag(QPointF(80, 50));
    require(selection.normalizedSelection() == QRectF(10, 10, 70, 40),
            "completed external gestures must not leak held shortcut state into the next drag");
    handler.setExternalDragActive(false);
}

void manualSelectionUsesSharedMarqueeTransaction() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);

    int overlayUpdates = 0;
    int selectionConfirmedCount = 0;
    ScreenshotOverlayInputActions actions;
    actions.updateOverlayState = [&overlayUpdates]() { ++overlayUpdates; };
    actions.selectionConfirmed = [&selectionConfirmedCount]() { ++selectionConfirmedCount; };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        std::move(actions),
    });

    handler.setExternalDragActive(true);
    handler.handleMousePress(nullptr, QPointF(10, 10));
    handler.handleMouseMove(nullptr, QPointF(30, 40));
    handler.handleMouseRelease(nullptr, QPointF(30, 40));
    require((handler.handleRightClick(nullptr, {}) == ScreenshotOverlayRightClickResult::Handled) &&
                handler.handleWheel(nullptr, {}, {0, 120}, {}),
            "external drags must consume ordinary right-click and wheel commands");
    handler.handleUnhandledMiddleClick();
    handler.handleUnhandledLeftDoubleClick();
    require(!selection.hasPixelSelection() && !interaction.dragging() &&
                selectionConfirmedCount == 0 && handler.shouldBlockUnhandledKeyInput(),
            "ordinary overlay input must not create or confirm an external drag selection");
    handler.setExternalDragActive(false);

    require(handler.shouldHandleMouseEvent(nullptr, QPointF(50, 50), true),
            "manual selection must handle an initial marquee press");
    handler.handleMousePress(nullptr, QPointF(50, 50));
    require(interaction.manualSelecting() && interaction.marqueeSelecting() &&
                interaction.dragging() &&
                interaction.dragMode() == ScreenshotSelectionDragMode::Marquee &&
                !interaction.movingSelection() &&
                captureState.sessionState == ScreenshotSessionState::OverlayVisible,
            "manual selection did not enter the shared unconfirmed marquee transaction");
    require(selection.normalizedSelection() == QRectF(50, 50, 0, 0),
            "manual marquee did not reset the selection origin");
    require(overlayUpdates == 1, "manual marquee did not refresh the overlay");
    require(selectionConfirmedCount == 0, "manual marquee was confirmed before the drag finished");

    handler.handleMouseMove(nullptr, QPointF(70, 80));
    handler.handleMouseRelease(nullptr, QPointF(70, 80));
    require(selection.normalizedSelection() == QRectF(50, 50, 20, 30),
            "the shared marquee transaction produced the wrong selection");
    require(interaction.movingSelection() && !interaction.dragging() &&
                captureState.sessionState == ScreenshotSessionState::Editing &&
                selectionConfirmedCount == 1,
            "marquee release must confirm the shared selection transaction exactly once");
}

void manualSelectionCanMoveExistingSelection() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 10, 20, 20));
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);

    int selectionConfirmedCount = 0;
    ScreenshotOverlayInputActions actions;
    actions.selectionConfirmed = [&selectionConfirmedCount]() { ++selectionConfirmedCount; };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        actions,
    });

    require(handler.activateMoveEntireSelectionShortcut(),
            "manual selection did not accept the whole-selection movement shortcut");
    handler.handleMousePress(nullptr, QPointF(20, 20));
    require(interaction.manualSelecting() && interaction.modifyingSelection() &&
                interaction.dragging() &&
                interaction.dragMode() == ScreenshotSelectionDragMode::All,
            "manual selection press inside an existing rectangle did not enter "
            "whole-selection "
            "movement");
    require(selection.normalizedSelection() == QRectF(10, 10, 20, 20),
            "manual whole-selection movement changed the rectangle before the drag moved");

    handler.handleMouseMove(nullptr, QPointF(25, 30));
    handler.handleMouseRelease(nullptr, QPointF(25, 30));
    require(selection.normalizedSelection() == QRectF(15, 20, 20, 20),
            "manual whole-selection movement produced the wrong translated rectangle");
    require(interaction.movingSelection() && !interaction.dragging() &&
                captureState.sessionState == ScreenshotSessionState::Editing &&
                selectionConfirmedCount == 1,
            "manual whole-selection movement did not confirm exactly once on release");
}

void moveToolModificationLeavesConfirmedStageUntilRelease() {
    ScreenshotCaptureState captureState;
    captureState.sessionState = ScreenshotSessionState::Editing;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 10, 20, 20));
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.confirmSelection();

    int selectionConfirmedCount = 0;
    ScreenshotOverlayInputActions actions;
    actions.selectionConfirmed = [&selectionConfirmedCount]() { ++selectionConfirmedCount; };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        std::move(actions),
    });

    handler.handleMousePress(nullptr, QPointF(20, 20));
    require(interaction.manualSelecting() && interaction.modifyingSelection() &&
                !interaction.movingSelection() && interaction.dragging() &&
                interaction.dragMode() == ScreenshotSelectionDragMode::All &&
                captureState.sessionState == ScreenshotSessionState::OverlayVisible,
            "selection modification must leave the confirmed stage for the whole drag");

    handler.confirmSelection();
    require(interaction.modifyingSelection() && selectionConfirmedCount == 0,
            "an active modification must not be confirmable before release");

    handler.handleMouseMove(nullptr, QPointF(25, 25));
    require(interaction.modifyingSelection() && !interaction.movingSelection() &&
                captureState.sessionState == ScreenshotSessionState::OverlayVisible,
            "selection modification re-entered the confirmed stage during pointer movement");
    handler.handleMouseRelease(nullptr, QPointF(25, 25));
    require(selection.normalizedSelection() == QRectF(15, 15, 20, 20),
            "the unified Move transaction produced the wrong translated selection");
    require(interaction.movingSelection() && !interaction.dragging() &&
                captureState.sessionState == ScreenshotSessionState::Editing &&
                selectionConfirmedCount == 1,
            "selection modification must confirm exactly once when the drag finishes");
}

void nonMoveToolPermanentlySwitchesForSelectionResize() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 10, 20, 20));
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.setCanvasTool(ScreenshotActiveTool::Shape);

    QVector<ScreenshotSelectionDragMode> cursors;
    QVector<ScreenshotActiveTool> activatedTools;
    ScreenshotOverlayInputActions actions;
    actions.setOverlayCursor = [&cursors](ScreenshotOverlayWindow*,
                                          ScreenshotSelectionDragMode dragMode) {
        cursors.push_back(dragMode);
    };
    actions.activateToolForSelectionResize = [&interaction,
                                              &activatedTools](ScreenshotActiveTool tool) {
        activatedTools.push_back(tool);
        if (tool == ScreenshotActiveTool::Move) {
            interaction.setMoveTool(true, false);
        } else {
            interaction.setCanvasTool(tool);
        }
        return true;
    };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        std::move(actions),
    });

    handler.handleMouseMove(nullptr, QPointF(30, 20));
    require(!cursors.isEmpty() && cursors.constLast() == ScreenshotSelectionDragMode::Right,
            "a drawing tool must show the resize cursor over the selection border");
    require(handler.shouldHandleMouseEvent(nullptr, QPointF(30, 20), false),
            "a drawing tool must reserve selection-border hover events for resizing");
    require(!handler.shouldHandleMouseEvent(nullptr, QPointF(20, 20), false),
            "a drawing tool must keep handling events inside the selection");

    handler.handleMousePress(nullptr, QPointF(30, 20));
    require(interaction.moveToolActive() && interaction.modifyingSelection() &&
                interaction.dragMode() == ScreenshotSelectionDragMode::Right,
            "pressing a selection border must permanently activate Move before resizing");
    require(activatedTools == QVector<ScreenshotActiveTool>{ScreenshotActiveTool::Move},
            "selection-border resize must use the permanent Move activation path");

    handler.handleMouseMove(nullptr, QPointF(40, 20));
    handler.handleMouseRelease(nullptr, QPointF(40, 20));
    require(selection.normalizedSelection() == QRectF(10, 10, 30, 20),
            "the Move tool did not resize the selection");
    require(interaction.activeTool() == ScreenshotActiveTool::Shape && interaction.editing() &&
                !interaction.dragging(),
            "finishing selection resize must restore the previously active tool");
    require(activatedTools == QVector<ScreenshotActiveTool>{ScreenshotActiveTool::Move,
                                                            ScreenshotActiveTool::Shape},
            "selection resize must permanently reactivate the previous tool after release");

    interaction.setCanvasTool(ScreenshotActiveTool::Shape);
    handler.handleMousePress(nullptr, QPointF(40, 20));
    handler.resetTransientShortcuts();
    require(interaction.activeTool() == ScreenshotActiveTool::Shape && interaction.editing(),
            "canceling selection resize must restore the previously active tool");
    require(activatedTools.constLast() == ScreenshotActiveTool::Shape,
            "canceling selection resize must permanently reactivate the previous tool");
}

void recognitionAndScrollingToolsResizeSelectionBorder() {
    const ScreenshotActiveTool recognitionTools[] = {
        ScreenshotActiveTool::Ocr,
        ScreenshotActiveTool::Table,
        ScreenshotActiveTool::Qr,
    };
    for (const ScreenshotActiveTool tool : recognitionTools) {
        ScreenshotCaptureState captureState;
        ScreenshotDisplaySession displays;
        ScreenshotGeometryMapper geometry;
        ScreenshotSelectionModel selection;
        selection.setSelectionRect(QRectF(10, 10, 20, 20));
        ScreenshotIntelligentSelectionModel intelligent;
        ScreenshotInteractionState interaction;
        interaction.setCanvasTool(tool);
        require(!interaction.selectionHandlesVisible(),
                "recognition tools must hide selection control points");
        QVector<ScreenshotActiveTool> activatedTools;
        ScreenshotOverlayInputActions actions;
        actions.activateToolForSelectionResize =
            [&interaction, &activatedTools](ScreenshotActiveTool activeTool) {
                activatedTools.push_back(activeTool);
                if (activeTool == ScreenshotActiveTool::Move) {
                    interaction.setMoveTool(true, false);
                } else {
                    interaction.setCanvasTool(activeTool);
                }
                return true;
            };
        ScreenshotOverlayInputHandler handler({
            captureState,
            interaction,
            selection,
            intelligent,
            geometry,
            displays,
            std::move(actions),
        });

        require(handler.selectionResizeDragModeAtCanvasPosition(QPointF(30, 20)) ==
                    ScreenshotSelectionDragMode::Right,
                "recognition tools must expose the selection border resize hit target");
        require(handler.beginSelectionResizeAtCanvasPosition(QPointF(30, 20)) &&
                    interaction.moveToolActive() && interaction.modifyingSelection(),
                "recognition border press must permanently activate Move before resizing");
        require(interaction.selectionHandlesVisible(),
                "permanent Move activation must restore normal selection control points");
        handler.updateSelectionResizeAtCanvasPosition(QPointF(40, 20));
        handler.finishSelectionResizeAtCanvasPosition(QPointF(40, 20));
        require(selection.normalizedSelection() == QRectF(10, 10, 30, 20) &&
                    interaction.activeTool() == tool && interaction.editing(),
                "recognition border resize must restore the active recognition tool");
        require(!interaction.selectionHandlesVisible(),
                "restored recognition tools must keep selection control points hidden");
        require(activatedTools == QVector<ScreenshotActiveTool>{ScreenshotActiveTool::Move, tool},
                "recognition resize must use permanent Move and recognition activations");
    }

    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 10, 20, 20));
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterScrollingCapture();
    int pauseCount = 0;
    int resumeCount = 0;
    ScreenshotOverlayInputActions actions;
    actions.pauseScrollingCapture = [&pauseCount]() { ++pauseCount; };
    actions.resumeScrollingCapture = [&resumeCount, &interaction]() {
        ++resumeCount;
        interaction.enterScrollingCapture();
    };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        std::move(actions),
    });

    require(handler.shouldHandleMouseEvent(nullptr, QPointF(30, 20), false),
            "scrolling capture must reserve selection-border events");
    handler.handleMousePress(nullptr, QPointF(30, 20));
    handler.handleMouseMove(nullptr, QPointF(40, 20));
    handler.handleMouseRelease(nullptr, QPointF(40, 20));
    require(selection.normalizedSelection() == QRectF(10, 10, 30, 20) && pauseCount == 1 &&
                resumeCount == 1 && interaction.scrollingCapture(),
            "scrolling capture must pause during resize and restart with the new selection");
}

void completionGesturesRequireAConfirmedSelectionAndSupportedTool() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(true);

    int actionCount = 0;
    ScreenshotOverlayInputActions actions;
    actions.executeConfiguredCompletionAction = [&actionCount](const QString&) { ++actionCount; };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        std::move(actions),
    });

    handler.handleUnhandledLeftDoubleClick();
    handler.handleUnhandledMiddleClick();
    require(actionCount == 0,
            "completion gestures must not run while the initial selection is active");

    selection.setSelectionRect(QRectF(1, 2, 20, 21));
    interaction.confirmSelection();
    handler.handleUnhandledLeftDoubleClick();
    require(actionCount == 1, "double-click must run for the confirmed Move tool");

    interaction.setCanvasTool(ScreenshotActiveTool::Shape);
    handler.handleUnhandledMiddleClick();
    require(actionCount == 2, "middle-click must run for a drawing tool");

    interaction.setCanvasTool(ScreenshotActiveTool::Select);
    handler.handleUnhandledLeftDoubleClick();
    interaction.enterScrollingCapture();
    handler.handleUnhandledMiddleClick();
    require(actionCount == 2,
            "completion gestures must ignore Select and scrolling screenshot modes");
}

void colorCopyEndsCaptureOnlyAfterSuccessfulCopy() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.confirmSelection();
    QWidget shortcutWindow;
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&shortcutWindow);

    bool colorAvailable = false;
    int copyCalls = 0;
    int cancelCalls = 0;
    ScreenshotOverlayInputActions actions;
    actions.copyColorPickerColorToClipboard = [&]() {
        if (interaction.inactive()) {
            return false;
        }
        ++copyCalls;
        return colorAvailable;
    };
    actions.cancelCapture = [&]() {
        require(colorAvailable && copyCalls > cancelCalls,
                "capture must close only after a successful color copy");
        ++cancelCalls;
        interaction.reset();
    };
    ScreenshotOverlayInputHandler handler(
        {captureState, interaction, selection, intelligent, geometry, displays, actions});
    ScreenshotOverlayShortcutController shortcutController(shortcutManager, handler, interaction,
                                                           intelligent, actions);
    require(!dispatchShortcut(shortcutWindow, Qt::Key_C) && copyCalls == 1 && cancelCalls == 0 &&
                !interaction.inactive(),
            "a failed color copy must keep the capture open");
    colorAvailable = true;
    require(dispatchShortcut(shortcutWindow, Qt::Key_C) && copyCalls == 2 && cancelCalls == 1 &&
                interaction.inactive(),
            "a successful color copy must end the capture");
    require(!dispatchShortcut(shortcutWindow, Qt::Key_C) && copyCalls == 2 && cancelCalls == 1,
            "an inactive capture must not copy or close again");

    interaction.beginCapture();
    interaction.enterOverlayVisible(true);
    require(dispatchShortcut(shortcutWindow, Qt::Key_C) && copyCalls == 3 && cancelCalls == 2 &&
                interaction.inactive(),
            "copying a color during initial selection must also end the capture");
}

void sharedShiftShortcutChoosesResizeOrColorFormat() {
    const storage::ScreenshotShortcutSettings shortcutSettings;
    const QStringList originalAspectShortcuts =
        shortcutSettings.keepSelectionWidthAndHeightConsistent();
    require(shortcutSettings.setKeepSelectionWidthAndHeightConsistent({QStringLiteral("Shift")}),
            "failed to establish the default aspect shortcut");

    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 10, 40, 20));
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.confirmSelection();
    QWidget shortcutWindow;
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&shortcutWindow);

    int colorFormatCycles = 0;
    ScreenshotOverlayInputActions actions;
    actions.cycleColorPickerFormat = [&colorFormatCycles]() {
        ++colorFormatCycles;
        return true;
    };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        actions,
    });
    ScreenshotOverlayShortcutController shortcutController(shortcutManager, handler, interaction,
                                                           intelligent, actions);

    require(dispatchShortcut(shortcutWindow, Qt::Key_Shift),
            "idle Shift press without a modifier flag was not reserved for its contextual action");
    require(colorFormatCycles == 0,
            "default Shift changed color format before its resize intent was known");
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_Shift) && colorFormatCycles == 1,
            "idle default Shift did not switch color format exactly once on release");

    for (const auto transition : {QEvent::Hide, QEvent::WindowDeactivate, QEvent::None}) {
        selection.setSelectionRect(QRectF(10, 10, 40, 20));
        interaction.confirmSelection();
        require(dispatchShortcut(shortcutWindow, Qt::Key_Shift, Qt::ShiftModifier) &&
                    dispatchShortcut(shortcutWindow, Qt::Key_Space, Qt::ShiftModifier),
                "temporary selection modifiers did not activate");
        if (transition == QEvent::None) {
            const auto suspension = shortcutManager.suspendInput();
            shortcutManager.resumeInput(suspension);
        } else {
            QEvent event(transition);
            QCoreApplication::sendEvent(&shortcutWindow, &event);
        }
        static_cast<void>(dispatchShortcutRelease(shortcutWindow, Qt::Key_Space));
        static_cast<void>(dispatchShortcutRelease(shortcutWindow, Qt::Key_Shift));
        require(colorFormatCycles == 1, "canceling Shift must not cycle the color format");
        handler.handleMousePress(nullptr, QPointF(50, 20));
        require(interaction.dragMode() == ScreenshotSelectionDragMode::Right,
                "canceled Space must not leave whole-selection movement enabled");
        handler.handleMouseMove(nullptr, QPointF(90, 20));
        handler.handleMouseRelease(nullptr, QPointF(90, 20));
        require(selection.normalizedSelection().size() == QSizeF(80, 20),
                "canceled Shift must not constrain a later resize");
    }
    selection.setSelectionRect(QRectF(10, 10, 40, 20));
    interaction.confirmSelection();

    require(dispatchShortcut(shortcutWindow, Qt::Key_Shift, Qt::ShiftModifier),
            "pre-held default Shift did not activate the aspect shortcut");
    handler.handleMousePress(nullptr, QPointF(50, 20));
    require(interaction.dragging() && interaction.dragMode() == ScreenshotSelectionDragMode::Right,
            "pre-held Shift did not allow the edge resize to begin");
    handler.handleMouseMove(nullptr, QPointF(90, 20));
    handler.handleMouseRelease(nullptr, QPointF(90, 20));
    const QRectF shiftResized = selection.normalizedSelection();
    require(shiftResized.size() == QSizeF(80, 80),
            "pre-held default Shift did not keep the selection width and height equal");
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_Shift) && colorFormatCycles == 1,
            "using default Shift for a resize also switched color format");

    selection.setSelectionRect(QRectF(10, 10, 40, 20));
    interaction.confirmSelection();
    require(dispatchShortcut(shortcutWindow, Qt::Key_Shift, Qt::ShiftModifier),
            "Shift did not activate before whole-selection movement");
    require(dispatchShortcut(shortcutWindow, Qt::Key_Space, Qt::ShiftModifier),
            "Shift followed by Space did not activate whole-selection movement");
    handler.handleMousePress(nullptr, QPointF(20, 15));
    handler.handleMouseMove(nullptr, QPointF(25, 15));
    handler.handleMouseRelease(nullptr, QPointF(25, 15));
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_Space, Qt::ShiftModifier),
            "Space release did not clear the whole-selection modifier");
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_Shift) && colorFormatCycles == 1,
            "Shift plus whole-selection movement also switched color format");

    require(shortcutSettings.setKeepSelectionWidthAndHeightConsistent({QStringLiteral("K")}),
            "failed to remap the aspect shortcut");
    shortcutController.reloadConfiguredShortcuts();
    require(dispatchShortcut(shortcutWindow, Qt::Key_Shift, Qt::ShiftModifier) &&
                colorFormatCycles == 2,
            "Shift did not switch color format after the aspect shortcut was remapped");
    static_cast<void>(dispatchShortcutRelease(shortcutWindow, Qt::Key_Shift));

    selection.setSelectionRect(QRectF(10, 10, 40, 20));
    interaction.confirmSelection();
    require(dispatchShortcut(shortcutWindow, Qt::Key_K),
            "pre-held remapped aspect shortcut was not handled");
    handler.handleMousePress(nullptr, QPointF(50, 20));
    handler.handleMouseMove(nullptr, QPointF(90, 20));
    handler.handleMouseRelease(nullptr, QPointF(90, 20));
    const QRectF remappedResized = selection.normalizedSelection();
    require(remappedResized.size() == QSizeF(80, 80),
            "pre-held remapped shortcut did not keep the selection width and height equal");
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_K) && colorFormatCycles == 2,
            "remapped aspect shortcut unexpectedly switched color format");

    require(shortcutSettings.setKeepSelectionWidthAndHeightConsistent(originalAspectShortcuts),
            "failed to restore the aspect shortcut after contextual input test");
}

void configuredSelectionShortcutsRouteTabHistoryAndColorActions(bool targetSwitchOnly = false) {
    const storage::ScreenshotShortcutSettings shortcutSettings;
    const QMap<QString, QStringList> originalShortcuts = shortcutSettings.allShortcuts();
    QMap<QString, QStringList> defaults = originalShortcuts;
    defaults.insert(QStringLiteral("move_entire_selection"), {QStringLiteral("Space")});
    defaults.insert(QStringLiteral("keep_selection_width_and_height_consistent"),
                    {QStringLiteral("Shift")});
    defaults.insert(QStringLiteral("switch_selection_between_window_and_window_sub_element"),
                    {QStringLiteral("Tab")});
    defaults.insert(QStringLiteral("previous_screenshot_history"), {QStringLiteral(",")});
    defaults.insert(QStringLiteral("next_screenshot_history"), {QStringLiteral(".")});
    defaults.insert(QStringLiteral("select_previously_selected_area"), {QStringLiteral("R")});
    defaults.insert(QStringLiteral("copy_color"), {QStringLiteral("C")});
    require(shortcutSettings.setAllShortcutsAtomic(defaults),
            "failed to establish selection shortcut defaults");

    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(true);
    intelligent.beginCaptureSession(true);
    require(intelligent.selectionTarget() ==
                    ScreenshotIntelligentSelectionTarget::WindowSubElement &&
                intelligent.toggleSelectionTarget(),
            "an enabled screenshot must initially use child-element mode");
    const QRectF windowSelection(10, 10, 40, 30);
    const QRectF childSelection(15, 15, 20, 12);
    const QRectF nestedChildSelection(17, 17, 12, 8);
    require(intelligent.applyCanvasHitPath({nestedChildSelection, childSelection, windowSelection},
                                           QRectF(0, 0, 100, 100), 1.0),
            "failed to seed intelligent-selection candidates");
    selection.setSelectionRect(windowSelection);

    QWidget shortcutWindow;
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&shortcutWindow);

    int overlayUpdates = 0;
    int previousHistoryCalls = 0;
    int nextHistoryCalls = 0;
    int previousSelectionCalls = 0;
    int copyColorCalls = 0;
    int selectorHitTestRequests = 0;
    int persistedTargetChanges = 0;
    ScreenshotIntelligentSelectionTarget persistedTarget =
        ScreenshotIntelligentSelectionTarget::Window;
    bool previousSelectionAvailable = true;
    ScreenshotOverlayInputActions actions;
    actions.updateOverlayState = [&overlayUpdates]() { ++overlayUpdates; };
    actions.requestUiSelectorHitTest = [&selectorHitTestRequests](const QPoint&) {
        ++selectorHitTestRequests;
    };
    actions.persistSelectionTarget =
        [&persistedTargetChanges, &persistedTarget](ScreenshotIntelligentSelectionTarget target) {
            ++persistedTargetChanges;
            persistedTarget = target;
        };
    actions.navigateHistoryPrevious = [&previousHistoryCalls]() {
        ++previousHistoryCalls;
        return true;
    };
    actions.navigateHistoryNext = [&nextHistoryCalls]() {
        ++nextHistoryCalls;
        return true;
    };
    actions.selectPreviousSelection = [&previousSelectionCalls, &previousSelectionAvailable]() {
        ++previousSelectionCalls;
        return previousSelectionAvailable;
    };
    actions.copyColorPickerColorToClipboard = [&copyColorCalls]() {
        ++copyColorCalls;
        return true;
    };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        actions,
    });
    ScreenshotOverlayShortcutController shortcutController(shortcutManager, handler, interaction,
                                                           intelligent, actions);

    intelligent.beginPress(QPointF(18, 18), windowSelection);
    require(dispatchShortcut(shortcutWindow, Qt::Key_Tab) &&
                intelligent.selectionTarget() ==
                    ScreenshotIntelligentSelectionTarget::WindowSubElement &&
                intelligent.index() == 0 &&
                selection.normalizedSelection() == nestedChildSelection &&
                !intelligent.pressActive() && selectorHitTestRequests == 1,
            "Tab did not switch intelligent selection to window sub-elements");
    require(persistedTargetChanges == 1 &&
                persistedTarget == ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "Tab did not persist the selected window sub-element mode");
    require(intelligent.setIndex(1) && intelligent.index() == 1 &&
                intelligent.currentSelection() == childSelection,
            "window sub-element mode did not retain full-path navigation");
    const QRectF nextWindowSelection(50, 10, 40, 30);
    const QRectF nextChildSelection(55, 15, 20, 12);
    const QRectF nextNestedChildSelection(57, 17, 12, 8);
    require(intelligent.applyCanvasHitPath(
                {nextNestedChildSelection, nextChildSelection, nextWindowSelection},
                QRectF(0, 0, 100, 100), 1.0) &&
                intelligent.index() == 0 &&
                intelligent.currentSelection() == nextNestedChildSelection,
            "window sub-element mode did not reset a changed hit path to its deepest element");
    require(dispatchShortcut(shortcutWindow, Qt::Key_Tab) &&
                intelligent.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window &&
                intelligent.index() == 2 &&
                selection.normalizedSelection() == nextWindowSelection &&
                selectorHitTestRequests == 2,
            "Tab did not switch intelligent selection back to windows");
    require(persistedTargetChanges == 2 &&
                persistedTarget == ScreenshotIntelligentSelectionTarget::Window,
            "Tab did not persist the selected window mode");
    require(intelligent.setIndex(0) && intelligent.index() == 2,
            "window mode allowed a window sub-element selection level");
    require(intelligent.applyCanvasHitPath({nestedChildSelection, childSelection, windowSelection},
                                           QRectF(0, 0, 100, 100), 1.0) &&
                intelligent.currentSelection() == windowSelection,
            "window mode did not persist across smart hit tests");
    require(dispatchShortcut(shortcutWindow, Qt::Key_Tab) &&
                intelligent.applyCanvasHitPath({windowSelection}, QRectF(0, 0, 100, 100), 1.0) &&
                intelligent.currentSelection() == windowSelection && selectorHitTestRequests == 3,
            "window sub-element mode did not retain the original window fallback");
    require(handler.handleWheel(nullptr, QPointF(), QPoint(0, 120), QPoint()) &&
                intelligent.selectionTarget() ==
                    ScreenshotIntelligentSelectionTarget::WindowSubElement &&
                selectorHitTestRequests == 4,
            "element-level scrolling unexpectedly switched back to window mode");
    require(dispatchShortcut(shortcutWindow, Qt::Key_Tab),
            "Tab did not switch back to window mode after an empty sub-element hit");
    require(intelligent.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window,
            "empty sub-element hit prevented switching back to window mode");
    require(intelligent.currentSelection() == windowSelection &&
                selection.normalizedSelection() == windowSelection,
            "window mode did not restore the available window selection");
    require(selectorHitTestRequests == 5,
            "switching back to window mode did not request a fresh hit test");

    intelligent.beginCaptureSession(false);
    require(intelligent.applyCanvasHitPath({nestedChildSelection, childSelection, windowSelection},
                                           QRectF(0, 0, 100, 100), 1.0),
            "failed to seed disabled intelligent-selection candidates");
    selection.setSelectionRect(intelligent.currentSelection());
    require(!dispatchShortcut(shortcutWindow, Qt::Key_Tab) &&
                !intelligent.smartSelectionEnabled() &&
                intelligent.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window &&
                intelligent.currentSelection() == windowSelection &&
                selection.normalizedSelection() == windowSelection && selectorHitTestRequests == 5,
            "disabled Smart selection must reject Tab and remain in window mode");
    require(persistedTargetChanges == 4,
            "disabled Smart selection must not persist a rejected Tab shortcut");
    if (targetSwitchOnly) {
        require(shortcutSettings.setAllShortcutsAtomic(originalShortcuts),
                "failed to restore selection shortcuts after target-switch test");
        return;
    }

    intelligent.beginCaptureSession(true);
    intelligent.beginPress(QPointF(18, 18), windowSelection);
    previousSelectionAvailable = false;
    require(!dispatchShortcut(shortcutWindow, Qt::Key_R) && previousSelectionCalls == 1 &&
                intelligent.pressActive(),
            "R must leave intelligent selection untouched when no previous area is available");
    previousSelectionAvailable = true;
    interaction.confirmSelection();
    require(dispatchShortcut(shortcutWindow, Qt::Key_R) && previousSelectionCalls == 2 &&
                !intelligent.pressActive(),
            "R did not request the previously selected area in Move mode");
    require(dispatchShortcut(shortcutWindow, Qt::Key_C) && copyColorCalls == 1,
            "C did not copy the color-picker color in Move mode");

    interaction.setCanvasTool(ScreenshotActiveTool::Shape);
    require(!dispatchShortcut(shortcutWindow, Qt::Key_R) &&
                !dispatchShortcut(shortcutWindow, Qt::Key_C) && previousSelectionCalls == 2 &&
                copyColorCalls == 1,
            "R and C must be inactive while a drawing tool is active");
    interaction.setMoveTool(true, false);

    require(dispatchShortcut(shortcutWindow, Qt::Key_Comma) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Period) && previousHistoryCalls == 1 &&
                nextHistoryCalls == 1,
            "default history shortcuts did not navigate the previous and next entries");

    QMap<QString, QStringList> remapped = shortcutSettings.allShortcuts();
    remapped.insert(QStringLiteral("switch_selection_between_window_and_window_sub_element"),
                    {QStringLiteral("J")});
    remapped.insert(QStringLiteral("select_previously_selected_area"), {QStringLiteral("K")});
    remapped.insert(QStringLiteral("copy_color"), {QStringLiteral("B")});
    remapped.insert(QStringLiteral("previous_screenshot_history"), {QStringLiteral("Y")});
    remapped.insert(QStringLiteral("next_screenshot_history"), {QStringLiteral("U")});
    require(shortcutSettings.setAllShortcutsAtomic(remapped),
            "failed to remap selection, history, and color shortcuts");
    shortcutController.reloadConfiguredShortcuts();

    interaction.returnToSelectionMode(true);
    require(intelligent.applyCanvasHitPath({windowSelection, childSelection},
                                           QRectF(0, 0, 100, 100), 1.0),
            "failed to restore intelligent-selection candidates after remapping");
    selection.setSelectionRect(windowSelection);
    require(!dispatchShortcut(shortcutWindow, Qt::Key_Tab) &&
                dispatchShortcut(shortcutWindow, Qt::Key_J) && selectorHitTestRequests == 6,
            "remapped Tab shortcut did not replace the default key");
    interaction.confirmSelection();
    require(!dispatchShortcut(shortcutWindow, Qt::Key_R) &&
                dispatchShortcut(shortcutWindow, Qt::Key_K) &&
                !dispatchShortcut(shortcutWindow, Qt::Key_C) &&
                dispatchShortcut(shortcutWindow, Qt::Key_B) && previousSelectionCalls == 3 &&
                copyColorCalls == 2,
            "remapped R and C shortcuts did not replace their default keys");
    require(!dispatchShortcut(shortcutWindow, Qt::Key_Comma) &&
                !dispatchShortcut(shortcutWindow, Qt::Key_Period) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Y) &&
                dispatchShortcut(shortcutWindow, Qt::Key_U) && previousHistoryCalls == 2 &&
                nextHistoryCalls == 2,
            "remapped history shortcuts did not replace comma and period");

    interaction.setMoveTool(true, false);
    selection.setSelectionRect(QRectF(10, 10, 40, 20));
    handler.handleMousePress(nullptr, QPointF(50, 20));
    require(interaction.dragging() && interaction.dragMode() == ScreenshotSelectionDragMode::Right,
            "selection-border fixture did not enter a right-edge resize");
    handler.handleMouseMove(nullptr, QPointF(60, 20));
    require(selection.normalizedSelection() == QRectF(10, 10, 50, 20),
            "selection-border fixture did not resize before Space was pressed");
    require(dispatchShortcut(shortcutWindow, Qt::Key_Space) &&
                interaction.dragMode() == ScreenshotSelectionDragMode::All,
            "Space did not switch an active resize into whole-selection movement");
    handler.handleMouseMove(nullptr, QPointF(70, 20));
    require(selection.normalizedSelection() == QRectF(20, 10, 50, 20),
            "Space did not move the entire selection without changing its size");
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_Space) &&
                interaction.dragMode() == ScreenshotSelectionDragMode::Right,
            "releasing Space did not restore the original resize edge");
    handler.handleMouseMove(nullptr, QPointF(80, 20));
    handler.handleMouseRelease(nullptr, QPointF(80, 20));
    require(selection.normalizedSelection() == QRectF(20, 10, 60, 20),
            "the resumed resize did not use the rebased pointer position");

    require(shortcutSettings.setAllShortcutsAtomic(originalShortcuts),
            "failed to restore selection shortcuts after route test");
}

void configuredScreenshotShortcutsControlMoveAndCursorNavigation() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(1, 2, 20, 21));
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.confirmSelection();
    QWidget shortcutWindow;
    shortcutWindow.show();
    QWidget colorPickerToolWindow;
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&shortcutWindow);

    const storage::ScreenshotShortcutSettings shortcutSettings;
    const QMap<QString, QStringList> originalShortcuts = shortcutSettings.allShortcuts();
    QMap<QString, QStringList> defaults = originalShortcuts;
    defaults.insert(QStringLiteral("move_tool"), {QStringLiteral("M")});
    defaults.insert(QStringLiteral("move_cursor_up"), {QStringLiteral("W"), QStringLiteral("Up")});
    defaults.insert(QStringLiteral("move_cursor_down"),
                    {QStringLiteral("S"), QStringLiteral("Down")});
    defaults.insert(QStringLiteral("move_cursor_left"),
                    {QStringLiteral("A"), QStringLiteral("Left")});
    defaults.insert(QStringLiteral("move_cursor_right"),
                    {QStringLiteral("D"), QStringLiteral("Right")});
    defaults.insert(QStringLiteral("pin_to_screen"), {QStringLiteral("Ctrl+F")});
    defaults.insert(QStringLiteral("cancel_screenshot"), {QStringLiteral("Esc")});
    defaults.insert(QStringLiteral("copy_to_clipboard"), {QStringLiteral("Ctrl+C")});
    defaults.insert(QStringLiteral("undo"), {QStringLiteral("Ctrl+Z")});
    defaults.insert(QStringLiteral("redo"), {QStringLiteral("Ctrl+Y")});
    require(shortcutSettings.setAllShortcutsAtomic(defaults),
            "failed to establish screenshot shortcut defaults");

    const storage::DrawingShortcutSettings drawingShortcutSettings;
    const QMap<QString, QStringList> originalDrawingShortcuts =
        drawingShortcutSettings.allShortcuts();
    QMap<QString, QStringList> collidingDrawingShortcuts = originalDrawingShortcuts;
    for (auto shortcuts = collidingDrawingShortcuts.begin();
         shortcuts != collidingDrawingShortcuts.end(); ++shortcuts) {
        shortcuts.value().clear();
    }
    collidingDrawingShortcuts.insert(QStringLiteral("shape"), {QStringLiteral("W")});
    require(drawingShortcutSettings.setAllShortcutsAtomic(collidingDrawingShortcuts),
            "failed to establish a cross-category shortcut collision");

    int moveToolActivations = 0;
    int drawingToolActivations = 0;
    int pinActivations = 0;
    int cancelActivations = 0;
    int copyActivations = 0;
    int undoActivations = 0;
    int redoActivations = 0;
    bool cursorMoveHandles = true;
    bool localShortcutInputAllowed = true;
    QVector<PhysicalCursorDirection> cursorMoves;
    ScreenshotOverlayInputActions actions;
    actions.physicalCursorMovementAvailable = []() { return true; };
    actions.localShortcutInputAllowed = [&localShortcutInputAllowed]() {
        return localShortcutInputAllowed;
    };
    actions.moveCursorOnePixel = [&cursorMoves,
                                  &cursorMoveHandles](PhysicalCursorDirection direction) {
        if (!cursorMoveHandles) {
            return false;
        }
        cursorMoves.push_back(direction);
        return true;
    };
    actions.activateDrawingShortcut = [&drawingToolActivations](const QString& toolId) {
        if (toolId != QStringLiteral("shape")) {
            return false;
        }
        ++drawingToolActivations;
        return true;
    };
    actions.activateScreenshotShortcut = [&](const QString& actionId) {
        if (actionId == QStringLiteral("move_tool")) {
            ++moveToolActivations;
            interaction.setMoveTool(true, false);
        } else if (actionId == QStringLiteral("pin_to_screen")) {
            ++pinActivations;
        } else if (actionId == QStringLiteral("cancel_screenshot")) {
            ++cancelActivations;
        } else if (actionId == QStringLiteral("copy_to_clipboard")) {
            ++copyActivations;
        } else if (actionId == QStringLiteral("undo")) {
            ++undoActivations;
        } else if (actionId == QStringLiteral("redo")) {
            ++redoActivations;
        } else {
            return false;
        }
        return true;
    };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        actions,
    });
    ScreenshotOverlayShortcutController shortcutController(shortcutManager, handler, interaction,
                                                           intelligent, actions);

    interaction.setCanvasTool(ScreenshotActiveTool::Watermark);
    localShortcutInputAllowed = false;
    require(!dispatchShortcut(shortcutWindow, Qt::Key_W) && cursorMoves.isEmpty(),
            "focused color-editor input must suppress ordinary cursor shortcuts");
    handler.armCanvasColorSampling();
    require(dispatchShortcut(colorPickerToolWindow, Qt::Key_W) &&
                cursorMoves == QVector<PhysicalCursorDirection>{PhysicalCursorDirection::Up},
            "an armed canvas color sampler must route cursor movement from its transient color "
            "picker window independently of the active drawing tool and focused color editor");
    handler.cancelCanvasColorSampling();
    require(!dispatchShortcut(colorPickerToolWindow, Qt::Key_W) && cursorMoves.size() == 1,
            "the transient color picker window must lose cursor shortcuts after canvas sampling");
    cursorMoves.clear();
    localShortcutInputAllowed = true;

    interaction.setCanvasTool(ScreenshotActiveTool::Shape);
    require(dispatchShortcut(shortcutWindow, Qt::Key_W),
            "cursor shortcut was not handled in the drawing tool");
    require(drawingToolActivations == 0 &&
                cursorMoves == QVector<PhysicalCursorDirection>{PhysicalCursorDirection::Up},
            "drawing-tool cursor movement must use the higher-priority "
            "screenshot shortcut");

    require(dispatchShortcut(shortcutWindow, Qt::Key_M), "default Move shortcut was not handled");
    require(moveToolActivations == 1 && interaction.moveToolActive(),
            "default Move shortcut must activate the Move tool");

    require(dispatchShortcut(shortcutWindow, Qt::Key_F, Qt::ControlModifier) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Escape) &&
                dispatchShortcutRelease(shortcutWindow, Qt::Key_Escape) &&
                dispatchShortcut(shortcutWindow, Qt::Key_C, Qt::ControlModifier) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Z, Qt::ControlModifier) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Y, Qt::ControlModifier) &&
                pinActivations == 1 && cancelActivations == 1 && copyActivations == 1 &&
                undoActivations == 1 && redoActivations == 1,
            "default toolbar command shortcuts must invoke their configured actions");

    require(dispatchShortcut(shortcutWindow, Qt::Key_W) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Up),
            "default cursor-up shortcuts were not handled");
    require(cursorMoves == QVector<PhysicalCursorDirection>{PhysicalCursorDirection::Up,
                                                            PhysicalCursorDirection::Up,
                                                            PhysicalCursorDirection::Up},
            "W and Up must move the cursor up by one pixel");
    require(drawingToolActivations == 0,
            "higher-priority screenshot shortcut must win a cross-category "
            "collision in Move mode");

    cursorMoveHandles = false;
    require(dispatchShortcut(shortcutWindow, Qt::Key_W) && drawingToolActivations == 1 &&
                cursorMoves.size() == 3,
            "declining screenshot shortcut must fall through to the drawing "
            "shortcut");
    cursorMoveHandles = true;

    require(shortcutSettings.setMoveCursorUp({QStringLiteral("Ctrl+Alt+K")}),
            "failed to customize the cursor-up shortcut");
    dispatchShortcut(shortcutWindow, Qt::Key_W);
    require(drawingToolActivations == 2 && cursorMoves.size() == 3,
            "removed screenshot shortcut must fall through to the colliding "
            "drawing shortcut");
    require(dispatchShortcut(shortcutWindow, Qt::Key_K, Qt::ControlModifier | Qt::AltModifier) &&
                cursorMoves.constLast() == PhysicalCursorDirection::Up,
            "customized cursor shortcut must take effect without restarting capture");

    require(shortcutSettings.setMoveTool({QStringLiteral("Ctrl+Alt+M")}),
            "failed to customize the Move shortcut");
    interaction.setCanvasTool(ScreenshotActiveTool::Shape);
    dispatchShortcut(shortcutWindow, Qt::Key_M);
    require(
        moveToolActivations == 1 &&
            dispatchShortcut(shortcutWindow, Qt::Key_M, Qt::ControlModifier | Qt::AltModifier) &&
            moveToolActivations == 2 && interaction.moveToolActive(),
        "customized Move shortcut must replace the default key immediately");

    QMap<QString, QStringList> remappedCommands = shortcutSettings.allShortcuts();
    remappedCommands.insert(QStringLiteral("pin_to_screen"), {QStringLiteral("Alt+F")});
    remappedCommands.insert(QStringLiteral("cancel_screenshot"), {QStringLiteral("Alt+Esc")});
    remappedCommands.insert(QStringLiteral("copy_to_clipboard"), {QStringLiteral("Alt+C")});
    remappedCommands.insert(QStringLiteral("undo"), {QStringLiteral("Alt+Z")});
    remappedCommands.insert(QStringLiteral("redo"), {QStringLiteral("Alt+Y")});
    require(shortcutSettings.setAllShortcutsAtomic(remappedCommands),
            "failed to customize toolbar command shortcuts");
    shortcutController.reloadConfiguredShortcuts();
    require(!dispatchShortcut(shortcutWindow, Qt::Key_F, Qt::ControlModifier) &&
                !dispatchShortcut(shortcutWindow, Qt::Key_Escape) &&
                !dispatchShortcut(shortcutWindow, Qt::Key_C, Qt::ControlModifier) &&
                !dispatchShortcut(shortcutWindow, Qt::Key_Z, Qt::ControlModifier) &&
                !dispatchShortcut(shortcutWindow, Qt::Key_Y, Qt::ControlModifier),
            "custom toolbar bindings must remove every default shortcut");
    require(dispatchShortcut(shortcutWindow, Qt::Key_F, Qt::AltModifier) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Escape, Qt::AltModifier) &&
                dispatchShortcutRelease(shortcutWindow, Qt::Key_Escape) &&
                dispatchShortcut(shortcutWindow, Qt::Key_C, Qt::AltModifier) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Z, Qt::AltModifier) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Y, Qt::AltModifier) &&
                pinActivations == 2 && cancelActivations == 2 && copyActivations == 2 &&
                undoActivations == 2 && redoActivations == 2,
            "custom toolbar bindings must invoke commands at their configured phase");

    // Recognition presents an independent top-level surface, but it remains
    // part of the screenshot session. Screenshot and drawing commands must
    // continue to dispatch through the shared manager while that surface has
    // focus.
    interaction.setCanvasTool(ScreenshotActiveTool::Ocr);
    QWidget recognitionWindow(nullptr, Qt::Tool);
    recognitionWindow.show();
    recognitionWindow.winId();
    shortcutWindow.show();
    shortcutWindow.winId();
    if (recognitionWindow.windowHandle() != nullptr && shortcutWindow.windowHandle() != nullptr) {
        recognitionWindow.windowHandle()->setTransientParent(shortcutWindow.windowHandle());
    }
    shortcutManager.addScopeWindow(&recognitionWindow);
    require(dispatchShortcut(recognitionWindow, Qt::Key_F, Qt::AltModifier) &&
                dispatchShortcut(recognitionWindow, Qt::Key_Escape, Qt::AltModifier) &&
                dispatchShortcutRelease(recognitionWindow, Qt::Key_Escape) &&
                dispatchShortcut(recognitionWindow, Qt::Key_C, Qt::AltModifier) &&
                pinActivations == 3 && cancelActivations == 3 && copyActivations == 3,
            "screenshot commands must remain available from a focused recognition surface");
    require(dispatchShortcut(recognitionWindow, Qt::Key_W) && drawingToolActivations == 3,
            "drawing shortcuts must remain available from a focused recognition surface");
    recognitionWindow.hide();
    interaction.setCanvasTool(ScreenshotActiveTool::Shape);

    require(shortcutSettings.setAllShortcutsAtomic(originalShortcuts),
            "failed to restore screenshot shortcuts after input test");
    require(drawingShortcutSettings.setAllShortcutsAtomic(originalDrawingShortcuts),
            "failed to restore drawing shortcuts after input test");
    cursorMoves.clear();
    require(dispatchShortcut(shortcutWindow, Qt::Key_Up),
            "restored cursor shortcut was not handled");
    require(cursorMoves == QVector<PhysicalCursorDirection>{PhysicalCursorDirection::Up},
            "restored cursor shortcut must use the persisted configuration");
}

void rightClickSeparatesDismissalFromSelectionChanges() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(true);
    int cancels = 0;
    ScreenshotOverlayInputActions actions;
    actions.cancelCapture = [&] { ++cancels; };
    ScreenshotOverlayInputHandler handler(
        {captureState, interaction, selection, intelligent, geometry, displays, actions});
    require(handler.handleRightClick(nullptr, {}) ==
                    ScreenshotOverlayRightClickResult::CancelCapture &&
                cancels == 0,
            "intelligent selection right press must only request deferred cancellation");
    handler.completeRightClickCancellation();
    require(cancels == 1, "release completion must execute capture cancellation");
    handler.armCanvasColorSampling();
    require(handler.handleRightClick(nullptr, {}) == ScreenshotOverlayRightClickResult::Handled &&
                cancels == 1,
            "color sampling cancellation must not dismiss the overlay");
}

void scrollingCaptureRoutesEveryToolbarShortcut() {
    const storage::ScreenshotShortcutSettings settings;
    const auto original = settings.allShortcuts();
    const QStringList commands{
        QStringLiteral("move_tool"),
        QStringLiteral("table_recognition"),
        QStringLiteral("qr_code_recognition"),
        QStringLiteral("text_recognition"),
        QStringLiteral("text_translation"),
        QStringLiteral("video_recording"),
        QStringLiteral("scrolling_screenshot"),
        QStringLiteral("save_as_file"),
        QStringLiteral("pin_to_screen"),
        QStringLiteral("cancel_screenshot"),
        QStringLiteral("copy_to_clipboard"),
        QStringLiteral("undo"),
        QStringLiteral("redo"),
    };
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    QWidget window;
    window.show();
    snow_shot::presentation::WindowShortcutManager manager;
    manager.addScopeWindow(&window);
    QStringList dispatched;
    bool inputAllowed = true;
    bool commandEnabled = true;
    ScreenshotOverlayInputActions actions;
    actions.localShortcutInputAllowed = [&]() { return inputAllowed; };
    actions.activateScreenshotShortcut = [&](const QString& id) {
        if (!commandEnabled) {
            return false;
        }
        dispatched.append(id);
        return true;
    };
    ScreenshotOverlayInputHandler handler(
        {captureState, interaction, selection, intelligent, geometry, displays, actions});
    ScreenshotOverlayShortcutController controller(manager, handler, interaction, intelligent,
                                                   actions);
    for (const auto& command : commands) {
        auto shortcuts = original;
        for (auto& keys : shortcuts) {
            keys.clear();
        }
        shortcuts[command] = {QStringLiteral("Ctrl+Alt+F12")};
        require(settings.setAllShortcutsAtomic(shortcuts), "failed to bind toolbar command");
        interaction.enterScrollingCapture();
        dispatched.clear();
        require(dispatchShortcut(window, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier),
                "toolbar command must accept its trigger");
        if (command == QStringLiteral("cancel_screenshot")) {
            require(dispatched.isEmpty(), "screenshot cancel must wait for release");
            require(dispatchShortcutRelease(window, Qt::Key_F12),
                    "cancel release must be consumed");
        }
        require(dispatched == QStringList{command},
                "every scrolling toolbar shortcut must invoke the common command exactly once");
        commandEnabled = false;
        const bool accepted =
            dispatchShortcut(window, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
        if (command == QStringLiteral("cancel_screenshot")) {
            require(accepted && dispatchShortcutRelease(window, Qt::Key_F12),
                    "a reserved release stays consumed even when the action declines");
        } else {
            require(!accepted, "a declined press action must remain unhandled");
        }
        require(dispatched.size() == 1,
                "a declined toolbar command must not be replaced by a shortcut implementation");
        commandEnabled = true;
        inputAllowed = false;
        require(!dispatchShortcut(window, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier) &&
                    dispatched.size() == 1,
                "text input must retain shortcuts while scrolling");
        inputAllowed = true;
    }
    require(settings.setAllShortcutsAtomic(original), "failed to restore toolbar shortcuts");
}

void intelligentSelectionSupportsCursorMovementShortcuts() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(true);
    QWidget shortcutWindow;
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&shortcutWindow);

    QVector<PhysicalCursorDirection> cursorMoves;
    ScreenshotOverlayInputActions actions;
    actions.physicalCursorMovementAvailable = []() { return true; };
    actions.moveCursorOnePixel = [&cursorMoves](PhysicalCursorDirection direction) {
        cursorMoves.push_back(direction);
        return true;
    };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        actions,
    });
    ScreenshotOverlayShortcutController shortcutController(shortcutManager, handler, interaction,
                                                           intelligent, actions);

    require(dispatchShortcut(shortcutWindow, Qt::Key_W) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Up) &&
                dispatchShortcut(shortcutWindow, Qt::Key_S) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Down) &&
                dispatchShortcut(shortcutWindow, Qt::Key_A) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Left) &&
                dispatchShortcut(shortcutWindow, Qt::Key_D) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Right) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Right, Qt::NoModifier, true),
            "cursor movement shortcuts were not handled during intelligent selection");
    require(cursorMoves ==
                QVector<PhysicalCursorDirection>{
                    PhysicalCursorDirection::Up,
                    PhysicalCursorDirection::Up,
                    PhysicalCursorDirection::Down,
                    PhysicalCursorDirection::Down,
                    PhysicalCursorDirection::Left,
                    PhysicalCursorDirection::Left,
                    PhysicalCursorDirection::Right,
                    PhysicalCursorDirection::Right,
                    PhysicalCursorDirection::Right,
                },
            "configured cursor shortcuts must move in every direction and auto-repeat");
}

void cursorMovementEligibilityFollowsInteractionState() {
    ScreenshotInteractionState interaction;
    require(!interaction.cursorMovementEnabled(),
            "an inactive screenshot must not enable cursor movement");

    interaction.enterOverlayVisible(true);
    require(interaction.cursorMovementEnabled(),
            "the Move tool must enable cursor movement during selection");

    interaction.setCanvasTool(ScreenshotActiveTool::Shape);
    require(interaction.cursorMovementEnabled(),
            "a drawing tool must enable cursor movement while editing");

    const ScreenshotActiveTool unsupportedTools[] = {
        ScreenshotActiveTool::Eraser,    ScreenshotActiveTool::Spotlight,
        ScreenshotActiveTool::Watermark, ScreenshotActiveTool::Ocr,
        ScreenshotActiveTool::Table,     ScreenshotActiveTool::Qr,
    };
    for (ScreenshotActiveTool tool : unsupportedTools) {
        interaction.setCanvasTool(tool);
        require(!interaction.cursorMovementEnabled(),
                "a cursor-ineligible tool enabled movement shortcuts");
    }

    interaction.enterScrollingCapture();
    require(!interaction.cursorMovementEnabled(),
            "scrolling capture must not enable cursor movement");
}

void hiddenToolbarDisablesToolSwitchShortcutsDuringSelectionResize() {
    const storage::ScreenshotShortcutSettings screenshotSettings;
    const QMap<QString, QStringList> originalScreenshotShortcuts =
        screenshotSettings.allShortcuts();
    QMap<QString, QStringList> screenshotShortcuts = originalScreenshotShortcuts;
    for (auto shortcuts = screenshotShortcuts.begin(); shortcuts != screenshotShortcuts.end();
         ++shortcuts) {
        shortcuts.value().clear();
    }
    screenshotShortcuts.insert(QStringLiteral("move_tool"), {QStringLiteral("M")});
    require(screenshotSettings.setAllShortcutsAtomic(screenshotShortcuts),
            "failed to establish the toolbar-visibility screenshot shortcut fixture");

    const storage::DrawingShortcutSettings drawingSettings;
    const QMap<QString, QStringList> originalDrawingShortcuts = drawingSettings.allShortcuts();
    QMap<QString, QStringList> drawingShortcuts = originalDrawingShortcuts;
    for (auto shortcuts = drawingShortcuts.begin(); shortcuts != drawingShortcuts.end();
         ++shortcuts) {
        shortcuts.value().clear();
    }
    drawingShortcuts.insert(QStringLiteral("shape"), {QStringLiteral("H")});
    require(drawingSettings.setAllShortcutsAtomic(drawingShortcuts),
            "failed to establish the toolbar-visibility drawing shortcut fixture");

    ScreenshotCaptureState captureState;
    captureState.sessionState = ScreenshotSessionState::Editing;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 10, 20, 20));
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.confirmSelection();
    QWidget shortcutWindow;
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&shortcutWindow);

    bool mainToolbarVisible = true;
    int showToolbarCount = 0;
    int moveToolActivations = 0;
    int drawingToolActivations = 0;
    ScreenshotOverlayInputActions actions;
    actions.hideMainToolbar = [&mainToolbarVisible]() { mainToolbarVisible = false; };
    actions.showToolbar = [&mainToolbarVisible, &showToolbarCount]() {
        mainToolbarVisible = true;
        ++showToolbarCount;
    };
    actions.mainToolbarVisible = [&mainToolbarVisible]() { return mainToolbarVisible; };
    actions.activateScreenshotShortcut = [&interaction, &moveToolActivations](const QString& id) {
        if (id != QStringLiteral("move_tool")) {
            return false;
        }
        ++moveToolActivations;
        interaction.setMoveTool(true, false);
        return true;
    };
    actions.activateDrawingShortcut = [&interaction,
                                       &drawingToolActivations](const QString& toolId) {
        if (toolId != QStringLiteral("shape")) {
            return false;
        }
        ++drawingToolActivations;
        interaction.setCanvasTool(ScreenshotActiveTool::Shape);
        return true;
    };

    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        actions,
    });
    ScreenshotOverlayShortcutController shortcutController(shortcutManager, handler, interaction,
                                                           intelligent, actions);

    handler.handleMousePress(nullptr, QPointF(30, 20));
    require(interaction.modifyingSelection() && interaction.dragging() && !mainToolbarVisible,
            "Move resize did not hide the main toolbar for the active drag");
    require(!dispatchShortcut(shortcutWindow, Qt::Key_H) && drawingToolActivations == 0 &&
                interaction.moveToolActive() && interaction.dragging(),
            "a hidden toolbar allowed a drawing-tool shortcut to interrupt selection resize");
    require(!dispatchShortcut(shortcutWindow, Qt::Key_M) && moveToolActivations == 0 &&
                interaction.moveToolActive() && interaction.dragging(),
            "a hidden toolbar allowed the Move shortcut to reset selection resize");

    handler.handleMouseMove(nullptr, QPointF(40, 20));
    handler.handleMouseRelease(nullptr, QPointF(40, 20));
    require(selection.normalizedSelection() == QRectF(10, 10, 30, 20) &&
                interaction.movingSelection() && !interaction.dragging() && mainToolbarVisible &&
                showToolbarCount == 1,
            "selection resize did not finish and restore the main toolbar");
    require(dispatchShortcut(shortcutWindow, Qt::Key_H) && drawingToolActivations == 1 &&
                interaction.activeTool() == ScreenshotActiveTool::Shape,
            "restoring the toolbar did not re-enable drawing-tool shortcuts");

    require(screenshotSettings.setAllShortcutsAtomic(originalScreenshotShortcuts),
            "failed to restore screenshot shortcuts after the toolbar-visibility test");
    require(drawingSettings.setAllShortcutsAtomic(originalDrawingShortcuts),
            "failed to restore drawing shortcuts after the toolbar-visibility test");
}

void canvasColorSamplingConsumesOneCanvasClick() {
    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);

    int sampleCount = 0;
    int cancelCount = 0;
    int previewCount = 0;
    ScreenshotOverlayInputActions actions;
    actions.sampleCanvasColor = [&sampleCount](ScreenshotOverlayWindow*, const QPointF&) {
        ++sampleCount;
        return true;
    };
    actions.cancelCanvasColorSampling = [&cancelCount]() { ++cancelCount; };
    actions.previewCanvasColor = [&previewCount](ScreenshotOverlayWindow*, const QPointF&) {
        ++previewCount;
    };
    ScreenshotOverlayInputHandler handler({
        captureState,
        interaction,
        selection,
        intelligent,
        geometry,
        displays,
        actions,
    });

    handler.armCanvasColorSampling();
    require(handler.shouldHandleMouseEvent(nullptr, QPointF(12, 16), false),
            "an armed canvas sampler must handle the next canvas click");
    handler.handleMouseMove(nullptr, QPointF(12, 16));
    require(previewCount == 1, "an armed canvas sampler must preview the color under the cursor");
    handler.handleMousePress(nullptr, QPointF(12, 16));
    require(sampleCount == 1 && !interaction.dragging(),
            "canvas sampling must consume its click before selection or drawing begins");

    handler.handleMousePress(nullptr, QPointF(12, 16));
    require(interaction.dragging() && previewCount == 1,
            "normal canvas input must resume after the one-shot sampler completes");
    handler.resetTransientShortcuts();

    handler.armCanvasColorSampling();
    require((handler.handleRightClick(nullptr, QPointF(12, 16)) ==
             ScreenshotOverlayRightClickResult::Handled) &&
                sampleCount == 1 && cancelCount == 1,
            "right-click must cancel an armed canvas sampler without sampling");
    handler.armCanvasColorSampling();
    handler.resetTransientShortcuts();
    require(cancelCount == 2, "capture resets must cancel an armed canvas sampler");
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
    if (QCoreApplication::arguments().contains(QStringLiteral("--canvas-color-sampling-only"))) {
        canvasColorSamplingConsumesOneCanvasClick();
        return 0;
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--selection-border-resize-only"))) {
        nonMoveToolPermanentlySwitchesForSelectionResize();
        recognitionAndScrollingToolsResizeSelectionBorder();
        return 0;
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--selection-input-only"))) {
        moveToolResizesSelectionFromOutsidePress();
        manualSelectionUsesSharedMarqueeTransaction();
        manualSelectionCanMoveExistingSelection();
        moveToolModificationLeavesConfirmedStageUntilRelease();
        nonMoveToolPermanentlySwitchesForSelectionResize();
        recognitionAndScrollingToolsResizeSelectionBorder();
        return 0;
    }
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    storage::ApplicationStorage::instance().shutdown();
    const storage::StorageInitializationOptions storageOptions{
        QDir(temporary.path()).filePath(QStringLiteral("bin")),
        QDir(temporary.path()).filePath(QStringLiteral("settings")),
        0,
    };
    require(storage::ApplicationStorage::instance().initialize(storageOptions).success,
            "failed to initialize isolated shortcut settings");
    if (QCoreApplication::arguments().contains(QStringLiteral("--direct-capture-only"))) {
        directCaptureRetainsTheWholeDesktop(
            QDir(temporary.path()).filePath(QStringLiteral("desktop")));
        explicitHistoryEditSeesExternalPublications(
            QDir(temporary.path()).filePath(QStringLiteral("external")));
        directCaptureHistoryPreservesSelectionRegions(temporary.path());
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (QCoreApplication::arguments().contains(
            QStringLiteral("--intelligent-selection-target-shortcut-only"))) {
        configuredSelectionShortcutsRouteTabHistoryAndColorActions(true);
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (QCoreApplication::arguments().contains(
            QStringLiteral("--intelligent-selection-cursor-shortcut-only"))) {
        intelligentSelectionSupportsCursorMovementShortcuts();
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--shortcut-input-only"))) {
        rightClickSeparatesDismissalFromSelectionChanges();
        scrollingCaptureRoutesEveryToolbarShortcut();
        externalSelectionSupportsHeldShortcuts();
        colorCopyEndsCaptureOnlyAfterSuccessfulCopy();
        sharedShiftShortcutChoosesResizeOrColorFormat();
        configuredSelectionShortcutsRouteTabHistoryAndColorActions();
        intelligentSelectionSupportsCursorMovementShortcuts();
        cursorMovementEligibilityFollowsInteractionState();
        configuredScreenshotShortcutsControlMoveAndCursorNavigation();
        hiddenToolbarDisablesToolSwitchShortcutsDuringSelectionResize();
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    committedSelectionFollowsHistory(
        QDir(temporary.path()).filePath(QStringLiteral("committed-selection")));
    navigationMatchesDisplaysAndRestoresLiveEndpoint(
        QDir(temporary.path()).filePath(QStringLiteral("navigation")));
    directCaptureRetainsTheWholeDesktop(QDir(temporary.path()).filePath(QStringLiteral("desktop")));
    explicitHistoryEditSeesExternalPublications(
        QDir(temporary.path()).filePath(QStringLiteral("external")));
    directCaptureHistoryPreservesSelectionRegions(
        QDir(temporary.path()).filePath(QStringLiteral("direct-images")));
    navigationSharesCanvasCreationStyles(
        QDir(temporary.path()).filePath(QStringLiteral("creation-styles")));
    fullSessionEntriesRemainReadable(
        QDir(temporary.path()).filePath(QStringLiteral("full-session-payload")));
    persistenceAndExactRetentionCutoff(
        QDir(temporary.path()).filePath(QStringLiteral("retention")));
    corruptLazyEntryDoesNotBlockOlderEntries(
        QDir(temporary.path()).filePath(QStringLiteral("corrupt")));
    expiredCurrentEntryCanReturnToConfirmedLiveSelection(
        QDir(temporary.path()).filePath(QStringLiteral("expired-navigation")));
    multipleValidEntriesCanBeTraversed(
        QDir(temporary.path()).filePath(QStringLiteral("multi-entry-navigation")));
    historyKeysOnlyWorkDuringSelectionStates();
    moveToolResizesSelectionFromOutsidePress();
    manualSelectionUsesSharedMarqueeTransaction();
    manualSelectionCanMoveExistingSelection();
    moveToolModificationLeavesConfirmedStageUntilRelease();
    nonMoveToolPermanentlySwitchesForSelectionResize();
    recognitionAndScrollingToolsResizeSelectionBorder();
    completionGesturesRequireAConfirmedSelectionAndSupportedTool();
    externalSelectionSupportsHeldShortcuts();
    colorCopyEndsCaptureOnlyAfterSuccessfulCopy();
    sharedShiftShortcutChoosesResizeOrColorFormat();
    configuredSelectionShortcutsRouteTabHistoryAndColorActions();
    intelligentSelectionSupportsCursorMovementShortcuts();
    cursorMovementEligibilityFollowsInteractionState();
    configuredScreenshotShortcutsControlMoveAndCursorNavigation();
    scrollingCaptureRoutesEveryToolbarShortcut();
    hiddenToolbarDisablesToolSwitchShortcutsDuringSelectionResize();
    canvasColorSamplingConsumesOneCanvasClick();
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
