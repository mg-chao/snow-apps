#include "snow_shot/presentation/screenshothistoryservice.h"
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
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                      bool autoRepeat = false) {
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
    return QDir(QDir(configurationDirectory).filePath(QStringLiteral("capture_history_records")));
}

bool copyDirectoryRecursively(const QString& source, const QString& destination) {
    if (!QDir().mkpath(destination)) {
        return false;
    }
    QDir sourceDirectory(source);
    QDirIterator iterator(source, QDir::AllEntries | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString sourcePath = iterator.next();
        const QString relativePath = sourceDirectory.relativeFilePath(sourcePath);
        const QString destinationPath = QDir(destination).filePath(relativePath);
        if (iterator.fileInfo().isDir()) {
            if (!QDir().mkpath(destinationPath)) {
                return false;
            }
        } else if (!QFile::copy(sourcePath, destinationPath)) {
            return false;
        }
    }
    return true;
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

void legacyFullSessionEntriesRemainReadable(const QString& root) {
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
            "failed to activate shape for the legacy session");
    SnowCanvasShapeStyle legacyStyle = canvas.canvasStyleToolbarState().shapeStyle;
    legacyStyle.strokeWidth = 13.0;
    require(canvas.setCanvasShapeStylePatch(legacyStyle, SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::Rectangle),
            "failed to configure the legacy session style");
    auto entry =
        takeSnapshot(history.snapshotCurrent(true), "failed to snapshot the legacy history entry");
    entry.canvasHistory = runtime.serializeDocumentSession();
    require(!entry.canvasHistory.isEmpty(), "failed to create a legacy canvas payload");
    history.commit(std::move(entry));

    SnowCanvasShapeStyle sharedStyle = legacyStyle;
    sharedStyle.strokeWidth = 7.0;
    require(canvas.setCanvasShapeStylePatch(sharedStyle, SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::Rectangle),
            "failed to configure the shared style after the legacy snapshot");
    displays.displayAt(0).image = solidImage(QSize(32, 32), qRgba(200, 210, 220, 255));
    require(history.navigatePrevious(), "legacy history navigation failed");
    waitForNavigation(history, "legacy history navigation timed out");
    require(displays.displayAt(0).image.pixel(0, 0) == storedPixel,
            "legacy full-session canvas payload was not restored");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "failed to reactivate shape after restoring the legacy session");
    require(canvas.canvasStyleToolbarState().shapeStyle.strokeWidth == sharedStyle.strokeWidth,
            "legacy full-session navigation restored its screenshot-local creation style");
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

    {
        ScreenshotHistoryService writer(
            {displays, runtime, selection, interaction, intelligent, {}}, root,
            [&clock]() { return clock; });
        auto older = takeSnapshot(writer.snapshotCurrent(true), "failed to snapshot older entry");
        writer.commit(std::move(older));
        clock = clock.addSecs(1);
        displays.displayAt(0).image = solidImage(QSize(64, 64), qRgba(0, 255, 0, 255));
        auto newer = takeSnapshot(writer.snapshotCurrent(true), "failed to snapshot newer entry");
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
    QFile corrupt(QDir(directories.constLast().absoluteFilePath())
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
                                     root);

    const QRgb olderPixel = displays.displayAt(0).image.pixel(0, 0);
    auto older =
        takeSnapshot(history.snapshotCurrent(true), "failed to snapshot older traversal entry");
    const QString olderId = older.id;
    history.commit(std::move(older));

    displays.displayAt(0).image = solidImage(QSize(64, 64), qRgba(0, 255, 0, 255));
    const QRgb newerPixel = displays.displayAt(0).image.pixel(0, 0);
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

void persistentEntriesAreRereadFromDisk(const QString& root) {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 32, 32),
                                   solidImage(QSize(32, 32), qRgba(255, 0, 0, 255))));
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 32, 32));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent, {}},
                                     root);

    const QRgb storedPixel = displays.displayAt(0).image.pixel(0, 0);
    auto entry =
        takeSnapshot(history.snapshotCurrent(true), "failed to snapshot disk reread entry");
    history.commit(std::move(entry));
    history.drainPendingWrites();

    displays.displayAt(0).image = solidImage(QSize(32, 32), qRgba(0, 0, 255, 255));
    const QImage liveImage = displays.displayAt(0).image;
    require(history.navigatePrevious(), "first disk reread navigation failed");
    waitForNavigation(history, "first disk reread navigation timed out");
    require(displays.displayAt(0).image.pixel(0, 0) == storedPixel,
            "first disk read did not restore the stored image");
    require(history.navigateNext(), "failed to return to live before disk mutation");
    require(displays.displayAt(0).image == liveImage,
            "live endpoint was not restored before disk mutation");

    const QFileInfoList directories =
        historyDirectory(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    require(directories.size() == 1, "disk reread entry directory is missing");
    const QString imagePath =
        QDir(directories.constFirst().absoluteFilePath()).filePath(QStringLiteral("display_0.png"));
    const QImage replacement = solidImage(QSize(32, 32), qRgba(0, 255, 0, 255));
    const QByteArray replacementPng = snow_shot::image_codec::encodePng(replacement);
    QFile replacementFile(imagePath);
    require(!replacementPng.isEmpty() && replacementFile.open(QIODevice::WriteOnly) &&
                replacementFile.write(replacementPng) == replacementPng.size(),
            "failed to replace the persisted history image");

    require(history.navigatePrevious(), "second disk reread navigation failed");
    waitForNavigation(history, "second disk reread navigation timed out");
    require(displays.displayAt(0).image.pixel(0, 0) == qRgba(0, 255, 0, 255),
            "history navigation reused a cached image instead of rereading disk");
}

void publicationCreatesSelfContainedRecord(const QString& root) {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 32, 32),
                                   solidImage(QSize(32, 32), qRgba(10, 20, 30, 255))));
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 32, 32));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent, {}},
                                     root);
    auto entry = takeSnapshot(history.snapshotCurrent(true), "failed to snapshot direct entry");
    history.commit(std::move(entry));

    QElapsedTimer timer;
    timer.start();
    QFileInfoList directories;
    while (directories.isEmpty() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
        directories =
            historyDirectory(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    }
    require(directories.size() == 1 &&
                QFileInfo(QDir(directories.constFirst().absoluteFilePath())
                              .filePath(QStringLiteral("canvas_history.json")))
                    .isFile() &&
                QFileInfo(QDir(directories.constFirst().absoluteFilePath())
                              .filePath(QStringLiteral("display_0.png")))
                    .isFile() &&
                QFileInfo(QDir(directories.constFirst().absoluteFilePath())
                              .filePath(QStringLiteral("manifest.json")))
                    .isFile(),
            "history record was not published as a self-contained directory");
    require(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("config.json"))),
            "history publication unexpectedly wrote configuration state");
    history.drainPendingWrites();
    require(
        !QFileInfo::exists(QDir(root).filePath(QStringLiteral("capture_history_catalog.json"))) &&
            !QFileInfo::exists(QDir(root).filePath(QStringLiteral("config.json"))),
        "history drain unexpectedly created shared storage metadata");
}

void startupReconciliationQuarantinesBrokenAndOrphanedEntries(const QString& root) {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 32, 32),
                                   solidImage(QSize(32, 32), qRgba(10, 20, 30, 255))));
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 32, 32));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    {
        ScreenshotHistoryService writer(
            {displays, runtime, selection, interaction, intelligent, {}}, root);
        auto entry = takeSnapshot(writer.snapshotCurrent(true), "failed to snapshot cleanup entry");
        writer.commit(std::move(entry));
        writer.drainPendingWrites();
    }

    QDir managed = historyDirectory(root);
    const QFileInfoList records =
        managed.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    require(records.size() == 1, "cleanup entry was not published");
    require(QFile::remove(QDir(records.constFirst().absoluteFilePath())
                              .filePath(QStringLiteral("display_0.png"))),
            "failed to remove indexed display image");
    require(managed.mkdir(QStringLiteral(".tmp-interrupted")) &&
                managed.mkdir(QStringLiteral("orphan-entry")),
            "failed to create reconciliation fixtures");

    {
        ScreenshotHistoryService reader(
            {displays, runtime, selection, interaction, intelligent, {}}, root);
        require(!reader.navigatePrevious(), "startup reconciliation retained a broken record");
        reader.drainPendingWrites();
    }
    require(managed.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
            "startup reconciliation retained invalid record directories");
    require(!QFileInfo::exists(managed.filePath(QStringLiteral(".tmp-interrupted"))),
            "startup reconciliation retained an interrupted temporary directory");
    const QDir quarantine(QDir(root).filePath(QStringLiteral("capture_history_quarantine")));
    require(quarantine.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot).size() == 2,
            "startup reconciliation did not preserve invalid records in quarantine");
    require(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("capture_history_catalog.json"))),
            "startup reconciliation unexpectedly created a catalog");
}

void portableDirectoryCopyKeepsHistoryValid(const QString& sourceRoot, const QString& copiedRoot) {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 24, 24),
                                   solidImage(QSize(24, 24), qRgba(90, 80, 70, 255))));
    const QRgb savedPixel = displays.displayAt(0).image.pixel(0, 0);
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 24, 24));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    {
        ScreenshotHistoryService writer(
            {displays, runtime, selection, interaction, intelligent, {}}, sourceRoot);
        auto entry =
            takeSnapshot(writer.snapshotCurrent(true), "failed to snapshot portable entry");
        writer.commit(std::move(entry));
        writer.drainPendingWrites();
    }
    require(copyDirectoryRecursively(sourceRoot, copiedRoot),
            "failed to copy portable configuration directory");

    displays.displayAt(0).image = solidImage(QSize(24, 24), qRgba(1, 2, 3, 255));
    ScreenshotHistoryService reader({displays, runtime, selection, interaction, intelligent, {}},
                                    copiedRoot);
    require(reader.navigatePrevious(), "copied portable history was not indexed");
    waitForNavigation(reader, "copied portable history navigation timed out");
    require(displays.displayAt(0).image.pixel(0, 0) == savedPixel,
            "relative history paths did not survive a portable directory copy");
}

void failedPublicationDoesNotInsertRecord(const QString& rootFile) {
    QFile blockingFile(rootFile);
    require(blockingFile.open(QIODevice::WriteOnly), "failed to create blocked history root");
    require(blockingFile.write("blocked") == 7, "failed to write blocked history root");
    blockingFile.close();

    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QStringLiteral("only"), QStringLiteral("Only"),
                                   QRect(0, 0, 16, 16),
                                   solidImage(QSize(16, 16), qRgba(4, 5, 6, 255))));
    SnowCanvasRuntime runtime;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 16, 16));
    ScreenshotInteractionState interaction;
    interaction.enterOverlayVisible(false);
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotHistoryService history({displays, runtime, selection, interaction, intelligent, {}},
                                     rootFile);
    auto entry = takeSnapshot(history.snapshotCurrent(true), "failed to snapshot rejected entry");
    history.commit(std::move(entry));
    history.drainPendingWrites();
    require(!history.navigatePrevious(), "failed publication inserted a navigable record");
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

    ScreenshotOverlayShortcutController shortcutController(
        shortcutManager, handler, interaction, intelligent, actions);

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

    require(handler.handleRightClick(nullptr, QPointF(4, 4)),
            "right-click did not handle manual selection");
    require(returnToCurrentCount == 1,
            "right-click did not check for an active historical screenshot");
    require(returnToIntelligentCount == 1,
            "ordinary right-click did not return to intelligent selection");

    hasCurrentScreenshot = true;
    require(handler.handleRightClick(nullptr, QPointF(4, 4)),
            "right-click did not handle historical selection");
    require(returnToCurrentCount == 2,
            "historical right-click did not return to the current screenshot");
    require(returnToIntelligentCount == 1,
            "historical right-click incorrectly returned to intelligent selection");

    static_cast<void>(
        interaction.enterSelectionDrag(ScreenshotSelectionDragMode::Marquee));
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
    actions.updateGuideLinesForOverlay = [&guideLineUpdates](ScreenshotOverlayWindow*,
                                                              const QPointF&) {
        ++guideLineUpdates;
    };
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
    require(selectionConfirmedCount == 0,
            "outside resize was confirmed before release");

    handler.handleMouseMove(nullptr, QPointF(60, 70));
    handler.handleMouseRelease(nullptr, QPointF(60, 70));
    require(selection.normalizedSelection() == QRectF(10, 10, 30, 40),
            "outside Move drag did not resize the existing selection");
    require(interaction.movingSelection() && !interaction.dragging() &&
                captureState.sessionState == ScreenshotSessionState::Editing &&
                selectionConfirmedCount == 1,
            "outside resize did not confirm exactly once on release");
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
    require(selectionConfirmedCount == 0,
            "manual marquee was confirmed before the drag finished");

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
    actions.activateToolForSelectionResize =
        [&interaction, &activatedTools](ScreenshotActiveTool tool) {
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
    require(activatedTools ==
                QVector<ScreenshotActiveTool>{ScreenshotActiveTool::Move,
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
        require(activatedTools ==
                    QVector<ScreenshotActiveTool>{ScreenshotActiveTool::Move, tool},
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

void sharedShiftShortcutChoosesResizeOrColorFormat() {
    const storage::ScreenshotShortcutSettings shortcutSettings;
    const QStringList originalAspectShortcuts =
        shortcutSettings.keepSelectionWidthAndHeightConsistent();
    require(shortcutSettings.setKeepSelectionWidthAndHeightConsistent(
                {QStringLiteral("Shift")}),
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
    ScreenshotOverlayShortcutController shortcutController(
        shortcutManager, handler, interaction, intelligent, actions);

    require(dispatchShortcut(shortcutWindow, Qt::Key_Shift),
            "idle Shift press without a modifier flag was not reserved for its contextual action");
    require(colorFormatCycles == 0,
            "default Shift changed color format before its resize intent was known");
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_Shift) &&
                colorFormatCycles == 1,
            "idle default Shift did not switch color format exactly once on release");

    require(dispatchShortcut(shortcutWindow, Qt::Key_Shift, Qt::ShiftModifier),
            "pre-held default Shift did not activate the aspect shortcut");
    handler.handleMousePress(nullptr, QPointF(50, 20));
    require(interaction.dragging() &&
                interaction.dragMode() == ScreenshotSelectionDragMode::Right,
            "pre-held Shift did not allow the edge resize to begin");
    handler.handleMouseMove(nullptr, QPointF(90, 20));
    handler.handleMouseRelease(nullptr, QPointF(90, 20));
    const QRectF shiftResized = selection.normalizedSelection();
    require(shiftResized.size() == QSizeF(80, 80),
            "pre-held default Shift did not keep the selection width and height equal");
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_Shift) &&
                colorFormatCycles == 1,
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
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_Shift) &&
                colorFormatCycles == 1,
            "Shift plus whole-selection movement also switched color format");

    require(shortcutSettings.setKeepSelectionWidthAndHeightConsistent(
                {QStringLiteral("K")}),
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
    require(dispatchShortcutRelease(shortcutWindow, Qt::Key_K) &&
                colorFormatCycles == 2,
            "remapped aspect shortcut unexpectedly switched color format");

    require(shortcutSettings.setKeepSelectionWidthAndHeightConsistent(
                originalAspectShortcuts),
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
    bool previousSelectionAvailable = true;
    ScreenshotOverlayInputActions actions;
    actions.updateOverlayState = [&overlayUpdates]() { ++overlayUpdates; };
    actions.requestUiSelectorHitTest = [&selectorHitTestRequests](const QPoint&) {
        ++selectorHitTestRequests;
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
    ScreenshotOverlayShortcutController shortcutController(
        shortcutManager, handler, interaction, intelligent, actions);

    intelligent.beginPress(QPointF(18, 18), windowSelection);
    require(dispatchShortcut(shortcutWindow, Qt::Key_Tab) &&
                intelligent.selectionTarget() ==
                    ScreenshotIntelligentSelectionTarget::WindowSubElement &&
                intelligent.index() == 0 &&
                selection.normalizedSelection() == nestedChildSelection &&
                !intelligent.pressActive() && selectorHitTestRequests == 1,
            "Tab did not switch intelligent selection to window sub-elements");
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
    require(intelligent.applyCanvasHitPath(
                {nestedChildSelection, childSelection, windowSelection},
                QRectF(0, 0, 100, 100), 1.0),
            "failed to seed disabled intelligent-selection candidates");
    selection.setSelectionRect(intelligent.currentSelection());
    require(!dispatchShortcut(shortcutWindow, Qt::Key_Tab) &&
                !intelligent.smartSelectionEnabled() &&
                intelligent.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window &&
                intelligent.currentSelection() == windowSelection &&
                selection.normalizedSelection() == windowSelection &&
                selectorHitTestRequests == 5,
            "disabled Smart Selection must reject Tab and remain in window mode");
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
                !dispatchShortcut(shortcutWindow, Qt::Key_C) &&
                previousSelectionCalls == 2 && copyColorCalls == 1,
            "R and C must be inactive while a drawing tool is active");
    interaction.setMoveTool(true, false);

    require(dispatchShortcut(shortcutWindow, Qt::Key_Comma) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Period) &&
                previousHistoryCalls == 1 && nextHistoryCalls == 1,
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
    QWidget colorPickerToolWindow;
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&shortcutWindow);

    const storage::ScreenshotShortcutSettings shortcutSettings;
    const QMap<QString, QStringList> originalShortcuts = shortcutSettings.allShortcuts();
    QMap<QString, QStringList> defaults = originalShortcuts;
    defaults.insert(QStringLiteral("move_tool"), {QStringLiteral("M")});
    defaults.insert(QStringLiteral("move_cursor_up"),
                    {QStringLiteral("W"), QStringLiteral("Up")});
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
    actions.activateMoveTool = [&interaction, &moveToolActivations]() {
        ++moveToolActivations;
        interaction.setMoveTool(true, false);
        return true;
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
    actions.pinSelectionToScreen = [&pinActivations]() {
        ++pinActivations;
        return true;
    };
    actions.cancelCapture = [&cancelActivations]() { ++cancelActivations; };
    actions.copySelectionToClipboard = [&copyActivations]() { ++copyActivations; };
    actions.undo = [&undoActivations]() {
        ++undoActivations;
        return true;
    };
    actions.redo = [&redoActivations]() {
        ++redoActivations;
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
    ScreenshotOverlayShortcutController shortcutController(
        shortcutManager, handler, interaction, intelligent, actions);

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

    require(dispatchShortcut(shortcutWindow, Qt::Key_M),
            "default Move shortcut was not handled");
    require(moveToolActivations == 1 && interaction.moveToolActive(),
            "default Move shortcut must activate the Move tool");

    require(dispatchShortcut(shortcutWindow, Qt::Key_F, Qt::ControlModifier) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Escape) &&
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
  require(dispatchShortcut(shortcutWindow, Qt::Key_W) &&
              drawingToolActivations == 1 && cursorMoves.size() == 3,
          "declining screenshot shortcut must fall through to the drawing "
          "shortcut");
    cursorMoveHandles = true;

    require(shortcutSettings.setMoveCursorUp({QStringLiteral("Ctrl+Alt+K")}),
            "failed to customize the cursor-up shortcut");
    dispatchShortcut(shortcutWindow, Qt::Key_W);
  require(drawingToolActivations == 2 && cursorMoves.size() == 3,
          "removed screenshot shortcut must fall through to the colliding "
          "drawing shortcut");
  require(
      dispatchShortcut(shortcutWindow, Qt::Key_K,
                             Qt::ControlModifier | Qt::AltModifier) &&
                cursorMoves.constLast() == PhysicalCursorDirection::Up,
            "customized cursor shortcut must take effect without restarting capture");

    require(shortcutSettings.setMoveTool({QStringLiteral("Ctrl+Alt+M")}),
            "failed to customize the Move shortcut");
    interaction.setCanvasTool(ScreenshotActiveTool::Shape);
    dispatchShortcut(shortcutWindow, Qt::Key_M);
    require(moveToolActivations == 1 &&
                dispatchShortcut(shortcutWindow, Qt::Key_M,
                                 Qt::ControlModifier | Qt::AltModifier) &&
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
                dispatchShortcut(shortcutWindow, Qt::Key_C, Qt::AltModifier) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Z, Qt::AltModifier) &&
                dispatchShortcut(shortcutWindow, Qt::Key_Y, Qt::AltModifier) &&
                pinActivations == 2 && cancelActivations == 2 && copyActivations == 2 &&
                undoActivations == 2 && redoActivations == 2,
            "custom toolbar bindings must invoke the same commands immediately");

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
    ScreenshotOverlayShortcutController shortcutController(
        shortcutManager, handler, interaction, intelligent, actions);

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
    actions.activateMoveTool = [&interaction, &moveToolActivations]() {
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
    ScreenshotOverlayShortcutController shortcutController(
        shortcutManager, handler, interaction, intelligent, actions);

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
                interaction.movingSelection() && !interaction.dragging() &&
                mainToolbarVisible && showToolbarCount == 1,
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
    require(previewCount == 1,
            "an armed canvas sampler must preview the color under the cursor");
    handler.handleMousePress(nullptr, QPointF(12, 16));
    require(sampleCount == 1 && !interaction.dragging(),
            "canvas sampling must consume its click before selection or drawing begins");

    handler.handleMousePress(nullptr, QPointF(12, 16));
    require(interaction.dragging() && previewCount == 1,
            "normal canvas input must resume after the one-shot sampler completes");
    handler.resetTransientShortcuts();

    handler.armCanvasColorSampling();
    require(handler.handleRightClick(nullptr, QPointF(12, 16)) && sampleCount == 1 &&
                cancelCount == 1,
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
    if (QCoreApplication::arguments().contains(
            QStringLiteral("--selection-border-resize-only"))) {
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
        sharedShiftShortcutChoosesResizeOrColorFormat();
        configuredSelectionShortcutsRouteTabHistoryAndColorActions();
        intelligentSelectionSupportsCursorMovementShortcuts();
        cursorMovementEligibilityFollowsInteractionState();
        configuredScreenshotShortcutsControlMoveAndCursorNavigation();
        hiddenToolbarDisablesToolSwitchShortcutsDuringSelectionResize();
        storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    navigationMatchesDisplaysAndRestoresLiveEndpoint(
        QDir(temporary.path()).filePath(QStringLiteral("navigation")));
    navigationSharesCanvasCreationStyles(
        QDir(temporary.path()).filePath(QStringLiteral("creation-styles")));
    legacyFullSessionEntriesRemainReadable(
        QDir(temporary.path()).filePath(QStringLiteral("legacy-payload")));
    persistenceAndExactRetentionCutoff(
        QDir(temporary.path()).filePath(QStringLiteral("retention")));
    corruptLazyEntryDoesNotBlockOlderEntries(
        QDir(temporary.path()).filePath(QStringLiteral("corrupt")));
    expiredCurrentEntryCanReturnToConfirmedLiveSelection(
        QDir(temporary.path()).filePath(QStringLiteral("expired-navigation")));
    multipleValidEntriesCanBeTraversed(
        QDir(temporary.path()).filePath(QStringLiteral("multi-entry-navigation")));
    persistentEntriesAreRereadFromDisk(
        QDir(temporary.path()).filePath(QStringLiteral("disk-reread")));
    publicationCreatesSelfContainedRecord(
        QDir(temporary.path()).filePath(QStringLiteral("direct-publication")));
    startupReconciliationQuarantinesBrokenAndOrphanedEntries(
        QDir(temporary.path()).filePath(QStringLiteral("startup-reconciliation")));
    portableDirectoryCopyKeepsHistoryValid(
        QDir(temporary.path()).filePath(QStringLiteral("portable-source")),
        QDir(temporary.path()).filePath(QStringLiteral("portable-copy")));
    failedPublicationDoesNotInsertRecord(
        QDir(temporary.path()).filePath(QStringLiteral("blocked-history-root")));
    historyKeysOnlyWorkDuringSelectionStates();
    moveToolResizesSelectionFromOutsidePress();
    manualSelectionUsesSharedMarqueeTransaction();
    manualSelectionCanMoveExistingSelection();
    moveToolModificationLeavesConfirmedStageUntilRelease();
    nonMoveToolPermanentlySwitchesForSelectionResize();
    recognitionAndScrollingToolsResizeSelectionBorder();
    completionGesturesRequireAConfirmedSelectionAndSupportedTool();
    sharedShiftShortcutChoosesResizeOrColorFormat();
    configuredSelectionShortcutsRouteTabHistoryAndColorActions();
    intelligentSelectionSupportsCursorMovementShortcuts();
    cursorMovementEligibilityFollowsInteractionState();
    configuredScreenshotShortcutsControlMoveAndCursorNavigation();
    hiddenToolbarDisablesToolSwitchShortcutsDuringSelectionResize();
    canvasColorSamplingConsumesOneCanvasClick();
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
