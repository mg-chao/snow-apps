#include "snow_shot/app/supportedpinfiles.h"
#include "snow_shot/presentation/screenshotfilepinbatch.h"

#include "snow_shot/platform/selectedfiles.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "pinned_animation_fixtures.h"
#include "bmp_import_fixtures.h"

#include <QApplication>
#include <QClipboard>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <cstdlib>
#include <iostream>

namespace {
using namespace snow_shot::platform;

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
    QCoreApplication::processEvents();
}

void finish(ScreenshotFilePinQueue& queue) {
    QElapsedTimer timer;
    timer.start();
    while (queue.active() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(!queue.active(), "queued shell requests must all finish");
}

QString imageFile(QTemporaryDir& directory, const QString& name, QColor color = Qt::green) {
    const QString path = directory.filePath(name);
    QImage image(7, 5, QImage::Format_RGBA8888);
    image.fill(color);
    require(QFileInfo(path).suffix().compare(QStringLiteral("bmp"), Qt::CaseInsensitive) == 0
                ? bmp_import_fixtures::write(path, image)
                : image.save(path, "PNG"),
            "fixture must encode");
    return path;
}

class FakeBackend final : public SelectedFileBackend {
  public:
    QStringList paths;
    SelectedFileTarget target{1, 2, 3, 4, false};
    mutable QSemaphore entered;
    mutable QSemaphore release;
    bool block = false;
    SelectedFileError error = SelectedFileError::None;
    SelectedFileTarget captureTarget() const override {
        return target;
    }
    SelectedFileResult selectedFiles(const SelectedFileTarget& captured,
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
        return {cancelled() ? QStringList{} : paths, error};
    }
};

void textBatchesCaptureOneStyle() {
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(
        storage.initialize({directory.filePath(QStringLiteral("bin")), directory.path()}).success,
        "isolated style settings initialize");
    const snow_shot::storage::PinToScreenSettings settings;
    require(settings.setTextMaximumWidth(240) && settings.setTextFontSize(20),
            "configure batch style");
    QStringList paths;
    for (const auto& document : snow_shot::app::kPinDocumentFileFormats) {
        const auto path =
            directory.filePath(QStringLiteral("document.") + QLatin1String(document.suffix));
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write("Pinned batch text") > 0,
                "write text file");
        paths.append(path);
    }
    auto backend = std::make_shared<FakeBackend>();
    backend->paths = paths;
    backend->block = true;
    ScreenshotFilePinBatch batch;
    int presented = 0;
    batch.startSelection(backend, backend->target, [&](ScreenshotClipboardContent content) {
        require(content.originalContent.textStyle &&
                    content.originalContent.textStyle->maximumWidth == 240 &&
                    content.originalContent.textStyle->fontSize == 20,
                "every file uses the style captured before selection and decoding");
        ++presented;
        return true;
    });
    require(backend->entered.tryAcquire(1, 5000), "selection worker starts");
    require(settings.setTextMaximumWidth(800) && settings.setTextFontSize(48),
            "change settings while import waits");
    backend->release.release();
    finish(batch);
    require(presented == paths.size(), "all document files pin using the same captured style");
    storage.shutdown();
}

void mixedFilesAndLargeBatches() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory must exist");
    QStringList expected;
    for (int index = 0; index < 20; ++index) {
        expected.append(imageFile(directory, QStringLiteral("%1.png").arg(index)));
    }
    expected.append(imageFile(directory, QStringLiteral("bitmap.bmp")));
    expected.append(imageFile(directory, QStringLiteral("bitmap-uppercase.BMP")));
    const QString corrupt = directory.filePath(QStringLiteral("corrupt.bmp"));
    QFile file(corrupt);
    require(file.open(QIODevice::WriteOnly) && file.write("invalid") == 7, "write corrupt fixture");
    file.close();
    QStringList paths = expected;
    paths.insert(1, corrupt);
    paths.insert(3, directory.path());
    paths.insert(5, directory.filePath(QStringLiteral("missing.png")));
    paths.insert(7, imageFile(directory, QStringLiteral("unsupported.pdf")));
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

void profiledFilesSurviveBatchDecode() {
    QTemporaryDir directory;
    require(directory.isValid(), "profile fixture directory must exist");
    QImage source(7, 5, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace::DisplayP3);
    source.fill(QColor(200, 100, 50));
    const QString path = directory.filePath(QStringLiteral("display-p3.png"));
    require(source.save(path, "PNG"), "profiled pin fixture must encode");
    ScreenshotFilePinBatch batch;
    int presented = 0;
    batch.start({path}, [&](ScreenshotClipboardContent content) {
        require(content.image == source,
                "asynchronous file pinning must preserve the embedded profile and pixels");
        require(content.preparedDisplayImage ==
                        source.convertedToColorSpace(QColorSpace(QColorSpace::SRgb),
                                                     QImage::Format_ARGB32_Premultiplied) &&
                    content.preparedImageTransform.isIdentity(),
                "file decoding prepares the sRGB display raster before GUI presentation");
        ++presented;
        return true;
    });
    finish(batch);
    require(presented == 1, "profiled file must be presented once");
}

void alphaBmpFilesSurviveBatchDecode() {
    QTemporaryDir directory;
    require(directory.isValid(), "alpha BMP fixture directory must exist");
    QImage source(3, 2, QImage::Format_RGBA8888);
    source.fill(Qt::green);
    source.setPixelColor(0, 0, Qt::red);
    source.setPixelColor(1, 0, QColor(10, 20, 30, 40));
    source.setPixelColor(2, 1, QColor(90, 80, 70, 0));
    const auto path = directory.filePath(QStringLiteral("alpha.BMP"));
    require(bmp_import_fixtures::write(path, source), "write alpha BMP pin fixture");
    const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
    require(files.size() == 1, "alpha BMP must be admitted before batch decoding");
    ScreenshotFilePinBatch batch;
    int presented = 0;
    batch.start({path}, [&](ScreenshotClipboardContent content) {
        require(bmp_import_fixtures::samePixels(content.image, source) &&
                    content.originalContent.localFilePath == path &&
                    content.sourceIdentity == files.first().sourceIdentity &&
                    !content.animationSource,
                "asynchronous BMP pinning preserves RGBA pixels and original file identity");
        require(content.preparedDisplayImage ==
                    source.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                "prepared BMP display pixels preserve premultiplied alpha");
        ++presented;
        return true;
    });
    finish(batch);
    require(presented == 1, "alpha BMP file must be presented once");
}

void duplicateFiltering() {
    QTemporaryDir directory;
    const QString first = imageFile(directory, QStringLiteral("first.png"));
    const QString second = imageFile(directory, QStringLiteral("second.png"));
    auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({first, second});
    require(files.size() == 2 && files[0].sourceIdentity.isValid(),
            "file identities are captured with metadata");
    const auto firstIdentity = files[0].sourceIdentity;
    const auto alias = ScreenshotClipboardContentReader::snapshotLocalFiles(
        {directory.filePath(QStringLiteral("./first.png"))});
    require(alias.size() == 1 && alias[0].sourceIdentity == firstIdentity,
            "equivalent file paths share one identity");
    require(files[1].sourceIdentity != firstIdentity,
            "identical images at different paths remain distinct");
    imageFile(directory, QStringLiteral("first.png"));
    require(ScreenshotClipboardContentReader::snapshotLocalFiles({first})[0].sourceIdentity ==
                firstIdentity,
            "rewriting a file does not change its source identity");
    ScreenshotFilePinBatch batch;
    int consumed = 0;
    QStringList presented;
    ScreenshotFilePinBatch::DuplicateFilter filter;
    filter.identities.insert(firstIdentity.key);
    filter.consume = [&](const auto& identity) {
        require(QThread::currentThread() == qApp->thread(),
                "duplicates are revalidated on the GUI thread");
        require(identity == firstIdentity,
                "duplicate callback receives the original source identity");
        ++consumed;
        return true;
    };
    // A known duplicate must be handled without attempting to decode its bytes.
    QFile corrupt(first);
    require(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate), "open duplicate fixture");
    corrupt.write("invalid");
    corrupt.close();
    batch.start(
        {first, second},
        [&](ScreenshotClipboardContent content) {
            presented.append(content.originalContent.localFilePath);
            require(content.sourceIdentity == files[1].sourceIdentity,
                    "decoded files carry their identities");
            return true;
        },
        filter);
    finish(batch);
    require(consumed == 1 && presented == QStringList{second},
            "mixed batch skips duplicate decode and pins new files");
    imageFile(directory, QStringLiteral("first.png"));
    filter.consume = [&](const auto&) {
        ++consumed;
        return false;
    };
    presented.clear();
    batch.start(
        {first, second},
        [&](ScreenshotClipboardContent content) {
            presented.append(content.originalContent.localFilePath);
            return true;
        },
        filter);
    finish(batch);
    require(consumed == 2 && presented == QStringList{first, second},
            "a closed duplicate target falls back to decoding in source order");
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

void selectionFailures() {
    auto backend = std::make_shared<FakeBackend>();
    backend->error = SelectedFileError::PermissionDenied;
    ScreenshotFilePinBatch batch;
    int failures = 0;
    const auto present = [](ScreenshotClipboardContent) {
        require(false, "failed selection must not present an image");
        return true;
    };
    const auto failure = [&](SelectedFileError error) {
        require(QThread::currentThread() == qApp->thread(), "failure must run on the GUI thread");
        require(!batch.active(), "batch must be stopped before reporting failure");
        require(error == SelectedFileError::PermissionDenied,
                "preserve structured selection error");
        ++failures;
    };
    batch.startSelection(backend, backend->captureTarget(), present, failure);
    finish(batch);
    require(failures == 1, "selection failure must be delivered exactly once");

    backend = std::make_shared<FakeBackend>();
    backend->error = SelectedFileError::PermissionDenied;
    backend->block = true;
    batch.startSelection(backend, backend->captureTarget(), present, failure);
    require(backend->entered.tryAcquire(1, 5000), "failing selection worker must start");
    batch.start({}, present);
    backend->release.release();
    finish(batch);
    // Drain all worker completions before checking for a stale warning.
    ScreenshotExportCoordinator::shared().shutdown();
    QCoreApplication::processEvents();
    require(failures == 1, "replaced selection must not report its late failure");
}

void clipboardFiles() {
    QTemporaryDir directory;
    const QString first = imageFile(directory, QStringLiteral("first.png"));
    const QString second = imageFile(directory, QStringLiteral("second.BMP"));
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

void completionReportsResults() {
    QTemporaryDir directory;
    const QString image = imageFile(directory, QStringLiteral("image.png"));
    const QString corrupt = directory.filePath(QStringLiteral("corrupt.png"));
    QFile invalid(corrupt);
    require(invalid.open(QIODevice::WriteOnly) && invalid.write("invalid") == 7,
            "write failed-decode fixture");
    invalid.close();
    ScreenshotFilePinBatch batch;
    int completions = 0;
    bool succeeded = false;
    const auto completed = [&](bool success) {
        require(!batch.active() && QThread::currentThread() == qApp->thread(),
                "completion runs on the GUI thread after the batch stops");
        ++completions;
        succeeded = success;
    };
    const auto present = [](ScreenshotClipboardContent) { return true; };
    batch.start({image}, present, {}, completed);
    finish(batch);
    require(completions == 1 && succeeded, "successful image reports one completion");
    batch.start({corrupt}, present, {}, completed);
    finish(batch);
    require(completions == 2 && !succeeded, "failed decode reports unsuccessful completion");
    batch.start({directory.filePath(QStringLiteral("missing.png"))}, present, {}, completed);
    finish(batch);
    require(completions == 3 && !succeeded, "rejected file reports unsuccessful completion");
    batch.start({image}, [](ScreenshotClipboardContent) { return false; }, {}, completed);
    finish(batch);
    require(completions == 4 && !succeeded, "presentation failure reports failure");
    batch.start({image}, present, {}, completed);
    batch.cancel();
    batch.start({}, present);
    finish(batch);
    require(completions == 4, "explicit cancellation discards its completion");

    const auto identity =
        ScreenshotClipboardContentReader::snapshotLocalFiles({image}).first().sourceIdentity;
    ScreenshotFilePinBatch::DuplicateFilter filter;
    filter.identities.insert(identity.key);
    filter.consume = [](const auto&) { return true; };
    batch.start(
        {image},
        [](ScreenshotClipboardContent) {
            require(false, "consumed duplicate must not decode or present again");
            return false;
        },
        filter, completed);
    finish(batch);
    require(completions == 5 && succeeded, "existing-pin action is a successful completion");
}

void queuedShellRequestsContinueAndRemainIndependent() {
    QTemporaryDir directory;
    const QString first = imageFile(directory, QStringLiteral("图片 first.BMP"));
    const QString second = imageFile(directory, QStringLiteral("second.png"));
    const QString missing = directory.filePath(QStringLiteral("missing.png"));
    const QString corrupt = directory.filePath(QStringLiteral("corrupt.bmp"));
    QFile invalid(corrupt);
    require(invalid.open(QIODevice::WriteOnly) && invalid.write("invalid") == 7,
            "write queued failed-decode fixture");
    invalid.close();
    QStringList presented;
    QStringList failed;
    ScreenshotFilePinQueue queue;
    const auto prepare = [&] {
        require(QThread::currentThread() == qApp->thread(), "prepare stays on the GUI thread");
        return ScreenshotFilePinQueue::Options{[&](ScreenshotClipboardContent content) {
                                                   presented.append(
                                                       content.originalContent.localFilePath);
                                                   return true;
                                               },
                                               {}};
    };
    const auto failure = [&](const QString& path) { failed.append(path); };
    queue.enqueue({first, corrupt}, prepare, failure);
    queue.enqueue({missing, second}, prepare, failure);
    require(queue.active(), "new shell launches append while the first decode is pending");
    ScreenshotFilePinBatch ordinaryBatch;
    ordinaryBatch.start({second}, [](ScreenshotClipboardContent) {
        require(false, "cancelled clipboard batch must not present");
        return true;
    });
    ordinaryBatch.cancel();
    finish(queue);
    require(presented == QStringList{first, second} && failed == QStringList{corrupt, missing},
            "rapid requests pin every valid image and continue after each failure");

    queue.enqueue({first, second}, prepare, failure);
    queue.cancel();
    queue.enqueue({second}, prepare, failure);
    finish(queue);
    require(presented == QStringList{first, second, second} && failed.size() == 2,
            "cancellation discards pending requests and allows a later request");
}

void queuedDuplicatePolicyUsesEarlierPins() {
    QTemporaryDir directory;
    const QString first = imageFile(directory, QStringLiteral("first.png"));
    const QString second = imageFile(directory, QStringLiteral("second.png"));
    QSet<QString> pinned;
    QStringList presented;
    int duplicateActions = 0;
    ScreenshotFilePinQueue queue;
    const auto prepare = [&] {
        ScreenshotFilePinQueue::Options options;
        options.duplicateFilter.identities = pinned;
        options.duplicateFilter.consume = [&](const auto& identity) {
            require(pinned.contains(identity.key), "duplicate refers to an earlier queued pin");
            ++duplicateActions;
            return true;
        };
        options.present = [&](ScreenshotClipboardContent content) {
            pinned.insert(content.sourceIdentity.key);
            presented.append(content.originalContent.localFilePath);
            if (content.originalContent.localFilePath == first) {
                QFile rewritten(first);
                require(rewritten.open(QIODevice::WriteOnly | QIODevice::Truncate),
                        "change duplicate bytes after its first pin");
                rewritten.write("invalid");
            }
            return true;
        };
        return options;
    };
    queue.enqueue({first, first, second}, prepare, [](const QString&) {
        require(false, "consumed duplicates must not warn or decode changed bytes");
    });
    finish(queue);
    require(presented == QStringList{first, second} && duplicateActions == 1,
            "duplicate policy is prepared after each earlier pin finishes");
}

void animatedFileBatchesRetainSources() {
    QTemporaryDir directory;
    const auto fixtures = pinned_animation_fixtures::images();
    QStringList paths;
    for (const auto& fixture : fixtures) {
        const auto path = directory.filePath(QStringLiteral("source.") + fixture.suffix);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) &&
                    file.write(fixture.bytes) == fixture.bytes.size(),
                "animated batch fixtures must be written");
        paths.append(path);
    }
    ScreenshotFilePinBatch batch;
    QList<ScreenshotClipboardContent> retained;
    batch.start(paths, [&](ScreenshotClipboardContent content) {
        const auto index = retained.size();
        require(content.animationSource && content.animationSource->isAnimated() &&
                    content.animationSource->encoded == fixtures.at(index).bytes &&
                    content.originalContent.localFilePath == paths.at(index),
                "batched animated files must retain source bytes and selection order");
        retained.append(std::move(content));
        return true;
    });
    finish(batch);
    require(retained.size() == fixtures.size(), "every animated file must be presented");
    for (qsizetype index = 0; index < paths.size(); ++index) {
        require(QFile::remove(paths.at(index)) &&
                    retained.at(index).animationSource->encoded == fixtures.at(index).bytes,
                "completed batches must transfer immutable source ownership to their pins");
    }
}

void queuedAnimatedShellRequestsRetainSources() {
    QTemporaryDir directory;
    const auto fixtures = pinned_animation_fixtures::images();
    QStringList paths;
    for (const auto& fixture : fixtures) {
        const auto path = directory.filePath(QStringLiteral("queued.") + fixture.suffix.toUpper());
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) &&
                    file.write(fixture.bytes) == fixture.bytes.size(),
                "animated shell request fixtures must be written");
        paths.append(path);
    }
    ScreenshotFilePinQueue queue;
    QList<ScreenshotClipboardContent> retained;
    const auto prepare = [&] {
        ScreenshotFilePinQueue::Options options;
        options.present = [&](ScreenshotClipboardContent content) {
            const auto index = retained.size();
            require(content.animationSource && content.animationSource->isAnimated() &&
                        content.animationSource->encoded == fixtures.at(index).bytes &&
                        content.originalContent.localFilePath == paths.at(index),
                    "queued animated shell requests retain source bytes and arrival order");
            retained.append(std::move(content));
            return true;
        };
        return options;
    };
    const auto failure = [](const QString&) {
        require(false, "supported animated shell requests must not fail");
    };
    for (const auto& path : paths) {
        require(ScreenshotClipboardContentReader::supportedImageFileExtensions().contains(
                    QFileInfo(path).suffix().toLower()),
                "animated shell requests are admitted by the shared image suffix list");
        queue.enqueue({path}, prepare, failure);
    }
    finish(queue);
    require(retained.size() == fixtures.size(), "every queued animated shell request is presented");
    for (qsizetype index = 0; index < paths.size(); ++index) {
        require(QFile::remove(paths.at(index)) &&
                    retained.at(index).animationSource->encoded == fixtures.at(index).bytes,
                "queued pins own their animation bytes after their source files are removed");
    }
}
void mixedDocumentBatchesAndShellQueue() {
    QTemporaryDir directory;
    QStringList paths{imageFile(directory, QStringLiteral("image.png"))};
    for (const auto& document : snow_shot::app::kPinDocumentFileFormats) {
        const auto path = directory.filePath(QStringLiteral("source.") +
                                             QString::fromLatin1(document.suffix).toUpper());
        QFile file(path);
        const QByteArray source = document.format == snow_shot::app::PinDocumentFormat::Markdown
                                      ? "**Document**"
                                      : "Document";
        require(file.open(QIODevice::WriteOnly) && file.write(source) == source.size(),
                "write mixed document source");
        paths.append(path);
    }
    QStringList presented;
    const auto present = [&](ScreenshotClipboardContent content) {
        const auto index = presented.size();
        const auto file =
            ScreenshotClipboardContentReader::snapshotLocalFiles({paths.at(index)}).first();
        require(content.sourceIdentity == file.sourceIdentity &&
                    content.isFormattedText() == (index > 0),
                "mixed batches retain file order, identity and content type");
        if (index > 0)
            require(content.plainText == QStringLiteral("Document"),
                    "document batches render the selected source format");
        presented.append(paths.at(index));
        return true;
    };
    ScreenshotFilePinBatch batch;
    auto duplicatePaths = paths;
    duplicatePaths.append(paths);
    batch.start(duplicatePaths, present);
    finish(batch);
    require(presented == paths, "mixed batches present each distinct file once");
    presented.clear();
    ScreenshotFilePinQueue queue;
    queue.enqueue(
        paths, [&] { return ScreenshotFilePinQueue::Options{present, {}}; },
        [](const QString&) { require(false, "supported shell document must open"); });
    finish(queue);
    require(presented == paths, "Explorer queues present mixed images and documents in order");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    textBatchesCaptureOneStyle();
    mixedDocumentBatchesAndShellQueue();
    mixedFilesAndLargeBatches();
    profiledFilesSurviveBatchDecode();
    alphaBmpFilesSurviveBatchDecode();
    duplicateFiltering();
    changedFilesAndPresentationStop();
    prefetchKeepsPresentationOrder();
    stoppingDiscardsPrefetchedDecodes();
    selectionAndCancellation();
    clipboardFiles();
    completionReportsResults();
    queuedShellRequestsContinueAndRemainIndependent();
    queuedDuplicatePolicyUsesEarlierPins();
    animatedFileBatchesRetainSources();
    queuedAnimatedShellRequestsRetainSources();
    selectionFailures();
    ScreenshotExportCoordinator::shared().shutdown();
    return 0;
}
