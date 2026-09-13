#include "snow_shot/presentation/screenshotfilepinbatch.h"

#include "snow_shot/platform/windows/selectedfiles.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFile>
#include <QMimeData>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <cstdlib>
#include <iostream>

namespace {
using namespace snow_shot::platform::windows;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void finish(ScreenshotFilePinBatch& batch) {
    QElapsedTimer timer;
    timer.start();
    while (batch.active() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(!batch.active(), "batch must finish without filling the export queue");
}

QString imageFile(QTemporaryDir& directory, const QString& name, QColor color = Qt::green) {
    const QString path = directory.filePath(name);
    QImage image(7, 5, QImage::Format_RGBA8888);
    image.fill(color);
    require(image.save(path, "PNG"), "fixture must encode");
    return path;
}

class FakeBackend final : public SelectedFileBackend {
  public:
    QStringList paths;
    SelectedFileTarget target{1, 2, 3, 4, false};
    mutable QSemaphore entered;
    mutable QSemaphore release;
    bool block = false;
    SelectedFileTarget captureTarget() const override {
        return target;
    }
    QStringList selectedFiles(const SelectedFileTarget& captured,
                              const std::function<bool()>& cancelled) const override {
        require(QThread::currentThread() != qApp->thread(),
                "selection must run off the GUI thread");
        require(captured.window == 1 && captured.view == 2 && captured.tab == 3 &&
                    captured.processId == 4,
                "capture must retain the original active view identity");
        entered.release();
        if (block) {
            release.acquire();
        }
        return cancelled() ? QStringList{} : paths;
    }
};

void mixedFilesAndLargeBatches() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory must exist");
    QStringList expected;
    for (int index = 0; index < 20; ++index) {
        expected.append(imageFile(directory, QStringLiteral("%1.png").arg(index)));
    }
    const QString corrupt = directory.filePath(QStringLiteral("corrupt.png"));
    QFile file(corrupt);
    require(file.open(QIODevice::WriteOnly) && file.write("invalid") == 7, "write corrupt fixture");
    file.close();
    QStringList paths = expected;
    paths.insert(1, corrupt);
    paths.insert(3, directory.path());
    paths.insert(5, directory.filePath(QStringLiteral("missing.png")));
    paths.insert(7, imageFile(directory, QStringLiteral("unsupported.txt")));
    paths.append(expected.first());
    paths.append(directory.filePath(QStringLiteral("./0.png")));
    QStringList presented;
    ScreenshotFilePinBatch batch;
    batch.start(paths, [&](ScreenshotClipboardContent content) {
        require(QThread::currentThread() == qApp->thread(),
                "presentation must run on the GUI thread");
        require(content.image.size() == QSize(7, 5), "each file must retain its own decoded image");
        presented.append(content.originalContent.localFilePath);
        return true;
    });
    finish(batch);
    require(presented == expected,
            "valid files must pin once in source order despite skipped files");
    batch.start({corrupt, directory.path()}, [&](ScreenshotClipboardContent) {
        require(false, "invalid-only batch must not present a window");
        return true;
    });
    finish(batch);
}

void changedFilesAndPresentationStop() {
    QTemporaryDir directory;
    const QString first = imageFile(directory, QStringLiteral("first.png"));
    const QString second = imageFile(directory, QStringLiteral("second.png"));
    ScreenshotFilePinBatch batch;
    int presented = 0;
    batch.start({first, second}, [&](ScreenshotClipboardContent) {
        ++presented;
        require(QFile::remove(second), "delete the queued file after its metadata was captured");
        return true;
    });
    finish(batch);
    require(presented == 1, "a file deleted after snapshot must be skipped");
    imageFile(directory, QStringLiteral("second.png"));
    batch.start({first, second}, [&](ScreenshotClipboardContent) {
        ++presented;
        return false;
    });
    finish(batch);
    require(presented == 2, "lost presentation target must stop the remaining batch");
}

QString gradientFile(QTemporaryDir& directory, const QString& name, QSize size) {
    const QString path = directory.filePath(name);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar* row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4] = uchar((x + y * 3) & 0xff);
            row[x * 4 + 1] = uchar((x * 5 + y) & 0xff);
            row[x * 4 + 2] = uchar((x * 3 + y * 7) & 0xff);
            row[x * 4 + 3] = 255;
        }
    }
    require(image.save(path, "PNG"), "gradient fixture must encode");
    return path;
}

void prefetchKeepsPresentationOrder() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory must exist");
    // Alternating decode costs let prefetched decodes finish out of order;
    // presentation must still follow source order.
    QStringList expected;
    for (int index = 0; index < 6; ++index) {
        expected.append(gradientFile(directory, QStringLiteral("order-%1.png").arg(index),
                                     index % 2 == 0 ? QSize(2200, 1800) : QSize(16, 12)));
    }
    QStringList presented;
    ScreenshotFilePinBatch batch;
    batch.start(expected, [&](ScreenshotClipboardContent content) {
        presented.append(content.originalContent.localFilePath);
        return true;
    });
    finish(batch);
    require(presented == expected, "prefetched decodes must present in source order");
}

void stoppingDiscardsPrefetchedDecodes() {
    QTemporaryDir directory;
    ScreenshotFilePinBatch batch;
    int presented = 0;
    batch.start({imageFile(directory, QStringLiteral("first.png")),
                 imageFile(directory, QStringLiteral("second.png")),
                 imageFile(directory, QStringLiteral("third.png"))},
                [&](ScreenshotClipboardContent) {
                    ++presented;
                    return false;
                });
    finish(batch);
    require(presented == 1, "a stopped batch must not present already-prefetched files");
}

void selectionAndCancellation() {
    QTemporaryDir directory;
    const QString path = imageFile(directory, QStringLiteral("selection.png"));
    auto backend = std::make_shared<FakeBackend>();
    backend->paths = {path};
    ScreenshotFilePinBatch batch;
    int presented = 0;
    auto present = [&](ScreenshotClipboardContent content) {
        require(content.originalContent.localFilePath == path,
                "selection must hand off its file path");
        ++presented;
        return true;
    };
    const auto target = backend->captureTarget();
    backend->target = {5, 6, 7, 8, true};
    batch.startSelection(backend, target, present);
    finish(batch);
    require(presented == 1, "selected file must be presented");
    backend->paths.clear();
    batch.startSelection(backend, target, present);
    finish(batch);
    require(presented == 1, "empty or unsupported selection must do nothing");

    backend = std::make_shared<FakeBackend>();
    backend->paths = {path};
    backend->block = true;
    batch.startSelection(backend, target, present);
    require(backend->entered.tryAcquire(1, 5000), "selection worker must start");
    batch.start({path}, present);
    backend->release.release();
    finish(batch);
    require(presented == 2, "replaced selection must not deliver a late pin");

    backend = std::make_shared<FakeBackend>();
    backend->paths = {path};
    backend->block = true;
    auto dying = std::make_unique<ScreenshotFilePinBatch>();
    dying->startSelection(backend, target, present);
    require(backend->entered.tryAcquire(1, 5000), "shutdown selection worker must start");
    dying.reset();
    backend->release.release();
    batch.start({}, present);
    finish(batch);
    require(presented == 2, "destroyed batch must not present");
}

void clipboardFiles() {
    QTemporaryDir directory;
    const QString first = imageFile(directory, QStringLiteral("first.png"));
    const QString second = imageFile(directory, QStringLiteral("second.png"));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(first), QUrl("https://example.com/image.png"),
                  QUrl::fromLocalFile(second)});
    mime.setText(QStringLiteral("file names must not be pinned as text"));
    mime.setHtml(QStringLiteral("<b>file preview</b>"));
    const QStringList paths = ScreenshotClipboardContentReader::localFilePaths(&mime);
    require(paths == QStringList{first, second}, "clipboard must return every local file in order");
    ScreenshotFilePinBatch batch;
    int count = 0;
    batch.start(paths, [&](ScreenshotClipboardContent content) {
        require(!content.isFormattedText(), "file batches must not render accompanying text");
        ++count;
        return true;
    });
    finish(batch);
    require(count == 2, "clipboard files must each be presented");
    QMimeData text;
    text.setText(QStringLiteral("plain text"));
    require(ScreenshotClipboardContentReader::localFilePaths(&text).isEmpty(),
            "non-file clipboard content must retain the single-content flow");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    mixedFilesAndLargeBatches();
    changedFilesAndPresentationStop();
    prefetchKeepsPresentationOrder();
    stoppingDiscardsPrefetchedDecodes();
    selectionAndCancellation();
    clipboardFiles();
    ScreenshotExportCoordinator::shared().shutdown();
    return 0;
}
