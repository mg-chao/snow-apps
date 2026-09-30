#include "snow_shot/presentation/screenshotclipboardplacement.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QUrl>
#include <limits>
#include <QJsonObject>
#include <QMimeData>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <thread>
#include <atomic>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif
#include <QtEndian>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
ScreenshotClipboardPlacement fixture() {
    ScreenshotClipboardPlacement p;
    p.placement = {QStringLiteral("primary"), QStringLiteral("serial-1"), QPointF(123, 234),
                   QSize(320, 200)};
    p.windowRect = QRect(123, 234, 320, 200);
    p.rasterSize = QSize(640, 400);
    p.displays = {{QStringLiteral("primary"), QStringLiteral("serial-1"), QRect(0, 0, 1920, 1080),
                   QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1040), 1.0}};
    return p;
}
QByteArray replaceJson(const QByteArray& bytes, const QJsonObject& object) {
    auto result = bytes.left(12);
    const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    qToLittleEndian(static_cast<quint32>(json.size()), result.data() + 8);
    return result + json;
}
void codecAndRecovery() {
    auto value = fixture();
    const auto bytes = encodeScreenshotClipboardPlacement(value);
    auto decoded = decodeScreenshotClipboardPlacement(bytes);
    require(decoded && decoded->placement == value.placement &&
                decoded->windowRect == value.windowRect &&
                decoded->rasterSize == value.rasterSize && decoded->displays == value.displays,
            "metadata roundtrip loses geometry or density");
    auto resolved = resolveScreenshotClipboardPlacement(*decoded, value.displays, true);
    require(resolved.windowRect == value.windowRect, "unchanged display must not center or shrink");
    auto reordered = value.displays;
    reordered.append({QStringLiteral("left"),
                      {},
                      QRect(-1920, 0, 1920, 1080),
                      QRect(-1920, 0, 1920, 1080),
                      QRect(-1920, 0, 1920, 1040),
                      1});
    value.displays = reordered;
    reordered.swapItemsAt(0, 1);
    require(resolveScreenshotClipboardPlacement(value, reordered, true).windowRect ==
                value.windowRect,
            "display enumeration order must not change exact placement");
    auto removed = value.displays.mid(1);
    resolved = resolveScreenshotClipboardPlacement(value, removed, true);
    require(resolved.displayIndex == 0 && resolved.windowRect == QRect(-320, 234, 320, 200),
            "missing display recovers nearest visible placement");
    auto moved = value.displays;
    moved[0].desktopBounds.translate(1000, -1000);
    moved[0].nativeBounds.translate(1000, -1000);
    moved[0].usableBounds.translate(1000, -1000);
    require(resolveScreenshotClipboardPlacement(value, moved, true).windowRect ==
                QRect(1123, -766, 320, 200),
            "display-local position follows a rearranged original display");
    auto smaller = fixture().displays;
    smaller[0].desktopBounds.setSize(QSize(200, 100));
    smaller[0].nativeBounds.setSize(QSize(200, 100));
    smaller[0].usableBounds.setSize(QSize(200, 100));
    resolved = resolveScreenshotClipboardPlacement(fixture(), smaller, true);
    require(resolved.windowRect.size() == QSize(160, 100) &&
                resolved.initialWindowSize == QSize(320, 200),
            "recovery fit must preserve initial zoom basis");
    resolved = resolveScreenshotClipboardPlacement(fixture(), smaller, false);
    require(resolved.windowRect == QRect(0, 0, 320, 200),
            "disabled resize preserves size and accessible origin");

    auto spanning = fixture();
    spanning.displays = value.displays;
    spanning.windowRect.moveLeft(-10);
    spanning.placement.position.setX(-10);
    require(resolveScreenshotClipboardPlacement(spanning, spanning.displays, true).windowRect ==
                spanning.windowRect,
            "cross-display selection must retain its exact extent");

    auto fractional = fixture();
    fractional.displays[0].desktopBounds = QRect(-1600, -900, 1600, 900);
    fractional.displays[0].usableBounds = fractional.displays[0].desktopBounds;
    fractional.displays[0].nativeBounds = fractional.displays[0].desktopBounds;
    fractional.displays[0].scale = 1.25;
    const qreal unitScale = snow_shot::storage::pinnedGeometryScale(1.25);
    fractional.windowRect = QRect(-1437, -693, 320, 200);
    fractional.placement.position = QPointF(163 / unitScale, 207 / unitScale);
    require(fractional.isValid(), "negative-origin fractional scale fixture");
    const auto fractionalBytes = encodeScreenshotClipboardPlacement(fractional);
    auto fractionalDecoded = decodeScreenshotClipboardPlacement(fractionalBytes);
    require(fractionalDecoded &&
                resolveScreenshotClipboardPlacement(*fractionalDecoded, fractional.displays, true)
                        .windowRect == fractional.windowRect,
            "fractional scale roundtrip shifts the native rectangle");
    auto newDpi = fractional.displays;
    newDpi[0].scale = 1.5;
    require(resolveScreenshotClipboardPlacement(fractional, newDpi, false).windowRect.size() ==
                fractional.windowRect.size(),
            "DPI recovery must preserve platform-sized extent");
    auto tied = fixture().displays;
    tied[0].name = QStringLiteral("replacement-primary");
    tied[0].serial.clear();
    tied.append(tied.first());
    tied[1].name = QStringLiteral("replacement-secondary");
    require(resolveScreenshotClipboardPlacement(fixture(), tied, true).displayIndex == 0,
            "equidistant display recovery must prefer the primary display");
    auto renamed = fixture().displays;
    renamed[0].name = QStringLiteral("renamed");
    renamed[0].desktopBounds.translate(500, 0);
    renamed[0].nativeBounds.translate(500, 0);
    renamed[0].usableBounds.translate(500, 0);
    require(resolveScreenshotClipboardPlacement(fixture(), renamed, true).windowRect.x() == 623,
            "display serial must take precedence over a changed name");

    require(!decodeScreenshotClipboardPlacement(bytes.left(10)), "truncated header accepted");
    auto unknown = bytes;
    qToLittleEndian(quint32(2), unknown.data() + 4);
    require(!decodeScreenshotClipboardPlacement(unknown), "unknown version accepted");
    require(!decodeScreenshotClipboardPlacement(QByteArray(16385, 'a')),
            "oversized payload accepted");
    require(decodeScreenshotClipboardPlacement(bytes + QByteArray(4, '\0')).has_value(),
            "native allocation zero padding must be accepted");
    require(decodeScreenshotClipboardPlacement(bytes + QByteArray(7, 'x')).has_value(),
            "uninitialized native allocation padding must not invalidate the framed payload");
    auto invalidBody = bytes;
    invalidBody[12] = '!';
    require(!decodeScreenshotClipboardPlacement(invalidBody), "malformed framed JSON accepted");
    auto json = QJsonDocument::fromJson(bytes.mid(12)).object();
    json.insert("window", QJsonArray{2147483647, 0, 100, 100});
    require(!decodeScreenshotClipboardPlacement(replaceJson(bytes, json)),
            "overflowing native rectangle accepted");
    json = QJsonDocument::fromJson(bytes.mid(12)).object();
    json.insert("raster", QJsonArray{1000000, 1000000});
    require(!decodeScreenshotClipboardPlacement(replaceJson(bytes, json)),
            "unbounded raster dimensions accepted");
    auto truncatedBody = bytes;
    qToLittleEndian(quint32(16384), truncatedBody.data() + 8);
    require(!decodeScreenshotClipboardPlacement(truncatedBody), "truncated body accepted");
    value = fixture();
    value.placement.position.setX(std::numeric_limits<double>::infinity());
    require(encodeScreenshotClipboardPlacement(value).isEmpty(), "nonfinite coordinates accepted");
    value = fixture();
    value.displays[0].scale = 0;
    require(encodeScreenshotClipboardPlacement(value).isEmpty(), "invalid display scale accepted");
}
QImage image() {
    QImage image(640, 400, QImage::Format_ARGB32);
    image.fill(QColor(40, 90, 150, 170));
    return image;
}
void contentAndPayload() {
    const auto pixels = image();
    auto value = fixture();
    auto payload = ScreenshotClipboardService::prepareImage(pixels, {}, value);
    require(payload.isValid() && !payload.placementBytes().isEmpty(),
            "image preparation drops metadata");
    const auto png = payload.pngBytes();
    const auto metadata = payload.placementBytes();
    ScreenshotClipboardPayload moved = std::move(payload);
    require(moved.isValid() && moved.placementBytes() == metadata && !payload.isValid(),
            "payload move loses metadata or ownership");
    auto reused = ScreenshotClipboardService::prepareImage(pixels, png, value);
    require(reused.pngBytes() == png && reused.placementBytes() == metadata,
            "placement changes canonical PNG bytes");
    QMimeData mime;
    mime.setData(QStringLiteral("image/png"), png);
    setScreenshotClipboardPlacement(mime, value);
    auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 2.0);
    require(content && content->placement && content->placement->windowRect == value.windowRect &&
                content->image.size() == pixels.size(),
            "PNG clipboard metadata not retained");
    const auto snapshot = ScreenshotClipboardContentReader::snapshotMimeData(&mime, 2, Qt::white);
    auto newer = value;
    newer.windowRect.translate(40, 50);
    newer.placement.position += QPointF(40, 50);
    setScreenshotClipboardPlacement(mime, newer);
    const auto older = ScreenshotClipboardContentReader::decode(*snapshot);
    require(older && older->placement->windowRect == value.windowRect,
            "clipboard changes after snapshot must not replace captured geometry");
    value.rasterSize = QSize(10, 10);
    setScreenshotClipboardPlacement(mime, value);
    content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(content && !content->placement, "raster mismatch must use ordinary placement");
    QMimeData empty;
    setScreenshotClipboardPlacement(empty, fixture());
    require(!ScreenshotClipboardContentReader::readMimeData(&empty, 1.0),
            "metadata alone is pin content");
    empty.setText(QStringLiteral("clipboard text"));
    content = ScreenshotClipboardContentReader::readMimeData(&empty, 1.0);
    require(content && content->isFormattedText() && !content->placement,
            "placement must not leak into text fallback");
    QMimeData external;
    external.setImageData(pixels);
    require(!ScreenshotClipboardContentReader::readMimeData(&external, 1.0)->placement,
            "external images must retain default placement");
    mime.setData(screenshotClipboardPlacementNativeMimeType(), QByteArray("invalid"));
    require(!ScreenshotClipboardContentReader::readMimeData(&mime, 1.0)->placement,
            "invalid metadata must not reject a usable image");

    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("copied.png"));
    require(pixels.save(path, "PNG"), "save file fixture");
    QFileInfo info(path);
    value = fixture();
    value.filePath = screenshotClipboardFilePath(path);
    value.fileSize = info.size();
    value.fileModifiedMs = info.lastModified().toUTC().toMSecsSinceEpoch();
    QMimeData fileMime;
    fileMime.setUrls({QUrl::fromLocalFile(path)});
    setScreenshotClipboardPlacement(fileMime, value);
    content = ScreenshotClipboardContentReader::readMimeData(&fileMime, 1.0);
    require(content && content->placement && !content->originalContent.localFilePath.isEmpty(),
            "matching saved file must retain placement");
    value.fileSize += 1;
    setScreenshotClipboardPlacement(fileMime, value);
    content = ScreenshotClipboardContentReader::readMimeData(&fileMime, 1.0);
    require(content && !content->placement, "changed file must lose placement but remain loadable");
    // Even a same-sized image format cannot borrow metadata belonging to a file URL.
    fileMime.setData(QStringLiteral("image/png"), png);
    value.fileSize = info.size();
    setScreenshotClipboardPlacement(fileMime, value);
    content = ScreenshotClipboardContentReader::readMimeData(&fileMime, 1.0);
    require(content && !content->placement,
            "file metadata incorrectly attaches to preferred encoded image");
    fileMime.removeFormat(QStringLiteral("image/png"));
    const auto unrelated = directory.filePath(QStringLiteral("unrelated.png"));
    require(pixels.save(unrelated, "PNG"), "save unrelated file fixture");
    fileMime.setUrls({QUrl::fromLocalFile(unrelated), QUrl::fromLocalFile(path)});
    content = ScreenshotClipboardContentReader::readMimeData(&fileMime, 1.0);
    require(content && !content->placement, "file placement leaked to an unrelated batch member");
    fileMime.setUrls({QUrl::fromLocalFile(path), QUrl::fromLocalFile(unrelated)});
    require(ScreenshotClipboardContentReader::readMimeData(&fileMime, 1.0)->placement.has_value(),
            "matching file in a batch lost placement");
    QImage replacement(320, 200, QImage::Format_ARGB32);
    replacement.fill(Qt::green);
    require(replacement.save(path, "PNG"), "replace saved file fixture");
    content = ScreenshotClipboardContentReader::readMimeData(&fileMime, 1.0);
    require(content && !content->placement && content->image.size() == replacement.size(),
            "replaced file retained the original placement");
}
void processUntil(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "clipboard commit timed out");
}
void publishFile(const QString& path, bool expectRetry = false) {
    auto value = fixture();
    const QFileInfo info(path);
    value.filePath = screenshotClipboardFilePath(path);
    value.fileSize = info.size();
    value.fileModifiedMs = info.lastModified().toUTC().toMSecsSinceEpoch();
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    setScreenshotClipboardPlacement(*mime, value);
    bool done = false, success = false;
    int attempts = 0;
    QObject receiver;
    auto handle = ScreenshotClipboardService::commitMimeData(
        QApplication::clipboard(), &receiver, mime, [&](ScreenshotClipboardCommitResult result) {
            success = result.succeeded();
            done = true;
            attempts = result.attempts;
        });
    require(handle.isValid(), "file commit not queued");
    processUntil([&] { return done; });
    require(success, "file commit failed");
    require(!expectRetry || attempts > 1, "file publication must retry with metadata intact");
}
void scopedPublicationsPreservePlacement() {
    ScreenshotClipboardCommitScope scope;
    QObject receiver;
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("scoped.png"));
    require(image().save(path, "PNG"), "save scoped publication fixture");
    for (const bool file : {false, true}) {
        auto placement = fixture();
        bool completed = false;
        ScreenshotClipboardCommitHandle handle;
        auto completion = [&](ScreenshotClipboardCommitResult result) {
            require(result.succeeded() && handle.isFinished() && scope.pendingCount() == 0,
                    "scoped metadata publications must retire before completion");
            completed = true;
        };
        if (file) {
            const QFileInfo info(path);
            placement.filePath = screenshotClipboardFilePath(path);
            placement.fileSize = info.size();
            placement.fileModifiedMs = info.lastModified().toUTC().toMSecsSinceEpoch();
            auto* mime = new QMimeData;
            mime->setUrls({QUrl::fromLocalFile(path)});
            setScreenshotClipboardPlacement(*mime, placement);
            handle = scope.commitMimeData(QApplication::clipboard(), &receiver, mime, completion);
        } else {
            handle = scope.commit(QApplication::clipboard(), &receiver,
                                  ScreenshotClipboardService::prepareImage(image(), {}, placement),
                                  completion);
        }
        require(handle.isValid() && scope.pendingCount() == 1,
                "scoped metadata publication must be tracked");
        processUntil([&] { return completed; });
        auto snapshot = ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), 1);
        require(snapshot.has_value(), "snapshot scoped clipboard publication");
        const auto content = ScreenshotClipboardContentReader::decode(std::move(*snapshot));
        require(content && content->placement &&
                    content->placement->windowRect == placement.windowRect &&
                    content->placement->filePath == placement.filePath &&
                    content->image.size() == placement.rasterSize,
                "retiring scoped image and file publications must preserve placement and pixels");
    }
}
void publicationOrderAndCancellation() {
    QObject receiver;
    auto newer = fixture();
    newer.windowRect.translate(30, 40);
    newer.placement.position += QPointF(30, 40);
    const auto oldId = ScreenshotClipboardService::reservePublication();
    const auto newId = ScreenshotClipboardService::reservePublication();
    int callbacks = 0;
    auto newHandle = ScreenshotClipboardService::commit(
        QApplication::clipboard(), &receiver,
        ScreenshotClipboardService::prepareImage(image(), {}, newer), newId,
        [&](ScreenshotClipboardCommitResult result) {
            require(result.succeeded(), "new publication failed");
            ++callbacks;
        });
    auto oldHandle = ScreenshotClipboardService::commit(
        QApplication::clipboard(), &receiver,
        ScreenshotClipboardService::prepareImage(image(), {}, fixture()), oldId,
        [&](ScreenshotClipboardCommitResult result) {
            require(result.succeeded(), "superseded publication failed");
            ++callbacks;
        });
    require(newHandle.isValid() && oldHandle.isValid(), "ordered publication not queued");
    processUntil([&] { return callbacks == 2; });
    auto captured = ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), 1);
    require(captured && captured->placement && captured->placement->windowRect == newer.windowRect,
            "older export overwrote the latest image placement");

    ScreenshotClipboardCommitResult cancelled;
    bool done = false;
    auto handle = ScreenshotClipboardService::commit(
        QApplication::clipboard(), &receiver,
        ScreenshotClipboardService::prepareImage(image(), {}, fixture()),
        [&](ScreenshotClipboardCommitResult result) {
            cancelled = result;
            done = true;
        });
    handle.cancel();
    processUntil([&] { return done; });
    require(cancelled.failure == ScreenshotClipboardCommitFailure::Cancelled &&
                cancelled.attempts == 0,
            "cancelled publication must not touch image or metadata");
    captured = ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), 1);
    require(captured && captured->placement && captured->placement->windowRect == newer.windowRect,
            "cancelled publication replaced metadata");
    QApplication::clipboard()->setText(QStringLiteral("external replacement"));
    auto replacement = ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), 1);
    require(replacement && !replacement->placement, "external content retained old placement");
}
void nativeRead() {
    const auto snapshot = ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), 1);
    require(snapshot.has_value(), "native content absent");
    const auto earlyPlacement = snapshotScreenshotClipboardPlacement(QApplication::clipboard());
    require(earlyPlacement && earlyPlacement->windowRect == fixture().windowRect,
            "bounded early clipboard metadata snapshot failed");
    const auto content = ScreenshotClipboardContentReader::decode(*snapshot);
    require(content && content->placement &&
                content->placement->windowRect == fixture().windowRect &&
                content->image.size() == image().size(),
            "native cross-process metadata roundtrip failed");
}
void runChild(const QStringList& args) {
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), args);
    // Native clipboard requests can invoke the owner's lazy Qt provider.
    // Keep its GUI event loop running while the other process reads.
    processUntil([&] { return child.state() == QProcess::NotRunning; });
    if (child.exitCode() != 0)
        std::cerr << child.readAllStandardError().constData();
    require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
            "native child failed");
}
void nativeRoundtrip() {
    auto* clipboard = QApplication::clipboard();
    ensureScreenshotClipboardPlacementMimeSupport();
    auto saved = std::make_unique<QMimeData>();
    if (const auto* mime = clipboard->mimeData()) {
        for (const auto& format : mime->formats())
            saved->setData(format, mime->data(format));
        if (mime->hasImage())
            saved->setImageData(mime->imageData());
    }
    struct Restore {
        QClipboard* clipboard;
        std::unique_ptr<QMimeData>& saved;
        ~Restore() {
            clipboard->setMimeData(saved.release());
            QCoreApplication::processEvents();
#if defined(Q_OS_WIN)
            using Flush = HRESULT(WINAPI*)();
            const auto module = GetModuleHandleW(L"ole32.dll");
            const auto flush =
                module ? reinterpret_cast<Flush>(GetProcAddress(module, "OleFlushClipboard"))
                       : nullptr;
            if (flush)
                static_cast<void>(flush());
#endif
        }
    } restore{clipboard, saved};
    scopedPublicationsPreservePlacement();
    publicationOrderAndCancellation();
    require(ScreenshotClipboardService::publish(
                clipboard, ScreenshotClipboardService::prepareImage(image(), {}, fixture())),
            "native image copy failed");
    runChild({QStringLiteral("--native-read")});
    runChild({QStringLiteral("--native-image-producer")});
    nativeRead();
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("copied.png"));
    require(image().save(path, "PNG"), "native file fixture");
#if defined(Q_OS_WIN)
    std::atomic_bool locked{false};
    std::jthread locker([&](std::stop_token stop) {
        bool opened = false;
        for (int i = 0; i < 500 && !opened && !stop.stop_requested(); ++i) {
            opened = OpenClipboard(nullptr) != FALSE;
            if (!opened)
                QThread::msleep(1);
        }
        locked.store(opened);
        while (opened && !stop.stop_requested())
            QThread::msleep(1);
        if (opened)
            CloseClipboard();
    });
    processUntil([&] { return locked.load(); });
    QObject timerReceiver;
    QTimer::singleShot(40, &timerReceiver, [&] { locker.request_stop(); });
    publishFile(path, true);
    locker.request_stop();
    locker.join();
#else
    publishFile(path);
#endif
    runChild({QStringLiteral("--native-read")});
    // Qt must materialize the private native type before the publisher exits.
    runChild({QStringLiteral("--native-producer"), path});
    nativeRead();
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        if (args.contains(QStringLiteral("--native-read")))
            nativeRead();
        else if (args.contains(QStringLiteral("--native-producer")))
            publishFile(args.last());
        else if (args.contains(QStringLiteral("--native-image-producer")))
            require(ScreenshotClipboardService::publish(
                        QApplication::clipboard(),
                        ScreenshotClipboardService::prepareImage(image(), {}, fixture())),
                    "native child image copy failed");
        else if (args.contains(QStringLiteral("--native-roundtrip")))
            nativeRoundtrip();
        else {
            require(QGuiApplication::platformName() == QStringLiteral("offscreen"),
                    "clipboard placement unit tests require the offscreen platform");
            codecAndRecovery();
            contentAndPayload();
#if !defined(Q_OS_WIN)
            scopedPublicationsPreservePlacement();
            publicationOrderAndCancellation();
#endif
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
