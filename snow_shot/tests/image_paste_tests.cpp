// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagewindow.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/components/imagespliteditorwidget.h"
#include "snow_shot/presentation/imagesplitwindow.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/modal.h"
#include "pinned_animation_fixtures.h"
#include "physical_key_test_support.h"
#include "snowimageqtcodec.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMimeData>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <utility>

using namespace snow_shot::presentation;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::_Exit(1);
    }
}

void flush() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

void waitUntil(const std::function<bool()>& ready, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 15000) {
        flush();
        QThread::msleep(1);
    }
    require(ready(), message);
}

template <typename Window> void idle(Window& window) {
    waitUntil([&] { return !window.loading(); }, "image import completes");
}

QImage image(QSize size = {40, 30}, QColor color = QColor(120, 60, 30, 128)) {
    QImage result(size, QImage::Format_RGBA8888);
    result.fill(color);
    result.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    result.setDevicePixelRatio(2);
    return result;
}

void sameImage(const QImage& actual, const QImage& expected) {
    if (actual.size() != expected.size() || actual.colorSpace() != expected.colorSpace()) {
        std::cerr << "Actual image: " << actual.width() << 'x' << actual.height() << " profile "
                  << actual.colorSpace().description().toStdString()
                  << "; expected: " << expected.width() << 'x' << expected.height() << " profile "
                  << expected.colorSpace().description().toStdString() << '\n';
    }
    require(actual.size() == expected.size() && actual.colorSpace() == expected.colorSpace(),
            "paste retains physical size and color profile");
    require(actual.convertToFormat(QImage::Format_RGBA8888) ==
                expected.convertToFormat(QImage::Format_RGBA8888),
            "paste retains pixels and alpha");
}

void bitmap(const QImage& value = image()) {
    QApplication::clipboard()->setImage(value);
}

void encoded(const QByteArray& bytes, const QString& mime = QStringLiteral("image/png")) {
    auto* data = new QMimeData;
    data->setData(mime, bytes);
    data->setHtml(QStringLiteral("<b>Do not render this fallback</b>"));
    data->setText(QStringLiteral("Do not render this fallback"));
    QApplication::clipboard()->setMimeData(data);
}

QByteArray png(const QImage& value) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && value.save(&buffer, "PNG"), "encode fixture");
    return bytes;
}

void files(const QStringList& paths) {
    auto* data = new QMimeData;
    QList<QUrl> urls;
    for (const auto& path : paths)
        urls.append(QUrl::fromLocalFile(path));
    urls.append(QUrl(QStringLiteral("https://example.com/remote.png")));
    data->setUrls(urls);
    QApplication::clipboard()->setMimeData(data);
}

void keyPaste(QWidget* receiver, bool repeat = false) {
    const auto combination = QKeySequence::keyBindings(QKeySequence::Paste).first()[0];
    PhysicalKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers(), {},
                           repeat);
    QApplication::sendEvent(receiver, &press);
    PhysicalKeyEvent release(QEvent::KeyRelease, combination.key(),
                             combination.keyboardModifiers());
    QApplication::sendEvent(receiver, &release);
}

void focus(QWidget* widget) {
    widget->window()->activateWindow();
    widget->setFocus();
    flush();
    require(widget->hasFocus(), "shortcut fixture receives keyboard focus");
}

QWidget* pasteTarget(ImageStitchingWindow& window) {
    auto* target = window.editor()->window()->findChild<QWidget*>(QStringLiteral("collageCancel"));
    require(target, "stitching has a keyboard-focusable footer action");
    return target;
}

void splittingClipboardAndReplacement() {
    int confirmations = 0;
    bool accept = true;
    ImageSplitWindowServices services;
    services.confirm = [&](QWidget*, bool replacing, auto done) {
        require(replacing, "pasted image uses replacement confirmation");
        ++confirmations;
        done(accept);
    };
    ImageSplitWindow window(nullptr, services);
    window.presentSource({});
    const auto first = image();
    bitmap(first);
    auto* select =
        window.editor()->findChild<QWidget*>(QStringLiteral("imageSplitEmptySelectImage"));
    require(select && select->isVisible(), "empty splitting exposes its image selection action");
    focus(select);
    keyPaste(select);
    require(window.loading(), "Paste starts asynchronous splitting import");
    bitmap(image({7, 5}, Qt::blue));
    window.paste();
    idle(window);
    if (window.document()->image().isNull())
        std::cerr << "Splitting import error: " << window.lastError().toStdString() << '\n';
    sameImage(window.document()->image(), first);
    require(confirmations == 0, "empty splitting editor imports without confirmation");
    require(window.document()->createLine(true, 10), "create edits before pasting replacement");
    const auto state = window.document()->state();
    accept = false;
    encoded(png(image({20, 12})));
    window.paste();
    idle(window);
    require(confirmations == 1 && window.document()->state() == state,
            "canceling pasted replacement retains image and split edits");
    accept = true;
    const auto replacement = image({20, 12});
    encoded(png(replacement));
    window.paste();
    idle(window);
    sameImage(window.document()->image(), replacement);
    require(confirmations == 2 && !window.document()->canUndo(),
            "accepted replacement resets old split edits");
    encoded(QByteArrayLiteral("invalid PNG"));
    window.paste();
    idle(window);
    sameImage(window.document()->image(), replacement);
    require(confirmations == 2 && !window.lastError().isEmpty(),
            "decode failure preserves image and never confirms or renders fallback text");
    window.shutdown();
}

void stitchingClipboardAndPdf() {
    ImageStitchingWindow window;
    window.present();
    const auto first = image();
    bitmap(first);
    focus(pasteTarget(window));
    keyPaste(pasteTarget(window));
    require(window.loading(), "Paste starts asynchronous stitching import");
    bitmap(image({7, 5}, Qt::blue));
    window.paste();
    idle(window);
    require(window.document()->items().size() == 1, "busy paste does not append another image");
    sameImage(window.document()->items().first().image, first);
    const auto id = window.document()->items().first().id;
    require(window.document()->rotate(id, true), "rotate existing image before paste");
    const auto transform = window.document()->items().first().transform;
    window.document()->setOutputType(ImageStitchingOutputType::PdfDocument);
    const auto second = image({20, 12});
    encoded(png(second));
    focus(pasteTarget(window));
    keyPaste(pasteTarget(window));
    idle(window);
    require(window.document()->items().size() == 2 &&
                window.document()->items().first().transform == transform,
            "PDF paste appends while retaining existing transforms");
    sameImage(window.document()->items().last().image, second);
    QStringList errors;
    QObject::connect(&window, &ImageStitchingWindow::importFinished, &window,
                     [&](const auto& failures) { errors = failures; });
    encoded(QByteArrayLiteral("invalid PNG"));
    window.paste();
    idle(window);
    require(errors.size() == 1 && window.document()->items().size() == 2,
            "corrupt clipboard image reports failure and preserves stitched images");
    window.shutdown();
}

void fileSelectionOrderAndFailures(const QString& directory) {
    const auto firstPath = directory + QStringLiteral("/first.PNG");
    const auto secondPath = directory + QStringLiteral("/second.png");
    const auto missing = directory + QStringLiteral("/missing.png");
    const auto corrupt = directory + QStringLiteral("/corrupt.png");
    const auto text = directory + QStringLiteral("/notes.txt");
    const auto first = image();
    const auto second = image({10, 15}, QColor(30, 20, 90, 40));
    require(first.save(firstPath) && second.save(secondPath), "write file paste fixtures");
    QFile broken(corrupt);
    require(broken.open(QIODevice::WriteOnly) && broken.write("broken") == 6,
            "write corrupt file fixture");
    broken.close();
    files({text, firstPath, secondPath, firstPath});
    ImageSplitWindow split;
    split.presentSource({});
    split.paste();
    idle(split);
    sameImage(split.document()->image(), first);
    require(split.imageName() == QStringLiteral("first"), "first copied image supplies filename");
    ImageStitchingWindow stitch;
    stitch.present();
    stitch.paste();
    idle(stitch);
    require(stitch.document()->items().size() == 3, "file paste preserves order and duplicates");
    sameImage(stitch.document()->items()[0].image, first);
    sameImage(stitch.document()->items()[1].image, second);
    sameImage(stitch.document()->items()[2].image, first);
    QStringList errors;
    QObject::connect(&stitch, &ImageStitchingWindow::importFinished, &stitch,
                     [&](const auto& failures) { errors = failures; });
    files({missing, secondPath, corrupt});
    split.paste();
    stitch.paste();
    idle(split);
    idle(stitch);
    sameImage(split.document()->image(), first);
    require(!split.lastError().isEmpty(), "splitting does not try later files after first failure");
    require(stitch.document()->items().size() == 4 && errors.size() == 2,
            "stitching retains successful files and reports missing and corrupt files");
    sameImage(stitch.document()->items().last().image, second);
    split.shutdown();
    stitch.shutdown();
}

void ignoredContentAndTextFocus() {
    ImageSplitWindow split;
    split.presentSource({});
    ImageStitchingWindow stitch;
    stitch.present();
    for (int type = 0; type < 4; ++type) {
        auto* data = new QMimeData;
        if (type == 1)
            data->setText(QStringLiteral("C:/image.png"));
        if (type == 2)
            data->setHtml(QStringLiteral("<b>text</b>"));
        if (type == 3) {
            data->setUrls({QUrl(QStringLiteral("https://example.com/image.png")),
                           QUrl::fromLocalFile(QStringLiteral("C:/notes.txt"))});
        }
        QApplication::clipboard()->setMimeData(data);
        bool allocated = false;
        const auto captured = ScreenshotClipboardContentReader::snapshotImage(
            QApplication::clipboard(), 1, [&](qint64) {
                allocated = true;
                return false;
            });
        require(!captured && !allocated, "unsupported content never reserves image memory");
        split.paste();
        stitch.paste();
        require(!split.loading() && !stitch.loading() && split.document()->image().isNull() &&
                    stitch.document()->items().isEmpty(),
                "empty, text, HTML, and unsupported files never import");
    }
    for (QWidget* editor :
         {static_cast<QWidget*>(split.editor()), static_cast<QWidget*>(stitch.editor())}) {
        auto* input = new QLineEdit(editor);
        input->show();
        focus(input);
        QApplication::clipboard()->setText(QStringLiteral("123"));
        keyPaste(input);
        require(input->text() == QStringLiteral("123"), "editable fields retain normal Paste");
        bitmap();
        keyPaste(input);
        require(!split.loading() && !stitch.loading(), "editable focus never imports images");
        delete input;
    }
    bitmap();
    focus(pasteTarget(stitch));
    keyPaste(pasteTarget(stitch), true);
    require(!stitch.loading(), "automatic key repeats do not paste images");
    keyPaste(pasteTarget(stitch));
    idle(stitch);
    require(stitch.document()->items().size() == 1 && split.document()->image().isNull(),
            "Paste imports into only the receiving window");
    QWidget other;
    other.show();
    focus(&other);
    keyPaste(&other);
    require(!split.loading() && !stitch.loading(), "unrelated windows do not trigger image Paste");
    split.shutdown();
    stitch.shutdown();
}

void scrollingExportFileImports(const QString& directory, QSize size = {2048, 32769}) {
    // A modest-width scrolling capture exceeds both historical clipboard limits:
    // 64 million pixels, and (as uncompressed RGBA BMP) 256 MiB on disk.
    ScreenshotImageRowSource rows;
    rows.size = size;
    rows.readRows = [size](int first, int count, qsizetype stride, uchar* output, qsizetype) {
        for (int y = 0; y < count; ++y) {
            for (int x = 0; x < size.width(); ++x) {
                auto* pixel = output + y * stride + x * 4;
                pixel[0] = static_cast<uchar>(x % 251);
                pixel[1] = static_cast<uchar>((first + y) % 251);
                pixel[2] = 90;
                pixel[3] = 128;
            }
        }
        return true;
    };
    for (const auto format : {snow::image::Format::png, snow::image::Format::bmp}) {
        // libpng's encoder has its own one-million-pixel axis limit. BMP can export longer axes.
        if (format == snow::image::Format::png && size.height() > 1'000'000)
            continue;
        const auto path =
            directory + (format == snow::image::Format::png ? QStringLiteral("/scrolling.png")
                                                            : QStringLiteral("/scrolling.bmp"));
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) &&
                    snow_shot::image_codec::encodeToDevice(rows, &file, format, {}),
                "scrolling export writes a large image through the production row encoder");
        file.close();
        if (format == snow::image::Format::bmp && size.width() == 2048)
            require(file.size() > 256LL * 1024 * 1024,
                    "the BMP regression exceeds the previous encoded-file limit");
        const auto snapshots = ScreenshotClipboardContentReader::snapshotLocalFiles(
            {path}, {}, ScreenshotClipboardContentReader::FilePurpose::ImageImport);
        require(snapshots.size() == 1, "editor file selection admits the exported scrolling image");
        require(ScreenshotClipboardContentReader::decodeImageFile(snapshots.first(),
                                                                  [] { return true; })
                    .isNull(),
                "cancelled large imports do not decode");
        qint64 admittedBytes = 0;
        require(ScreenshotClipboardContentReader::decodeImageFile(
                    snapshots.first(), {},
                    [&](qint64 bytes) {
                        admittedBytes = bytes;
                        return bytes <= file.size() + 1;
                    }).isNull() &&
                    admittedBytes >= qint64(size.width()) * size.height() * 8,
                "large imports reserve decoded pixels before allocation and honor rejection");
        const auto check = [size](const QImage& actual) {
            require(actual.size() == size, "large scrolling import retains every source pixel");
            require(actual.pixelColor(size.width() - 1, size.height() - 1) ==
                        QColor((size.width() - 1) % 251, (size.height() - 1) % 251, 90, 128),
                    "large scrolling import preserves the final row and alpha");
        };
        {
            ImageSplitWindow split;
            split.presentSource({});
            split.replaceFile(path);
            idle(split);
            if (split.document()->image().isNull())
                std::cerr << "Import failed for " << path.toStdString() << ": "
                          << split.lastError().toStdString() << '\n';
            check(split.document()->image());
            split.shutdown();
        }
        // Large imports must also detect content when the suffix names a different codec.
        const auto mislabeledPath =
            path +
            (format == snow::image::Format::png ? QStringLiteral(".bmp") : QStringLiteral(".png"));
        require(file.rename(mislabeledPath), "rename the large export to a misleading suffix");
        {
            ImageStitchingWindow stitch;
            stitch.present();
            stitch.addFiles({mislabeledPath});
            idle(stitch);
            require(stitch.document()->items().size() == 1,
                    "stitching imports the exported scrolling image");
            check(stitch.document()->items().first().image);
            stitch.shutdown();
        }
        require(file.remove(), "large export fixture is released after import");
    }
}

void animatedClipboardInitialFrames() {
    for (const auto& fixture : pinned_animation_fixtures::images()) {
        encoded(fixture.bytes, fixture.mimeType);
        const auto snapshot =
            ScreenshotClipboardContentReader::snapshotImage(QApplication::clipboard(), 1);
        require(snapshot.has_value(), "animated clipboard image is captured");
        const auto reference = ScreenshotClipboardContentReader::decode(*snapshot);
        require(reference.has_value(), "animated clipboard reference decodes");
        ImageSplitWindow split;
        split.presentSource({});
        ImageStitchingWindow stitch;
        stitch.present();
        split.paste();
        stitch.paste();
        idle(split);
        idle(stitch);
        sameImage(split.document()->image(), reference->image);
        require(stitch.document()->items().size() == 1, "animated clipboard appends one image");
        sameImage(stitch.document()->items().first().image, reference->image);
        split.shutdown();
        stitch.shutdown();
    }
}

void encodedCorruptionRetainsBitmapFallback() {
    class ImageDataOnlyMimeData final : public QMimeData {
      public:
        bool hasFormat(const QString& mimeType) const override {
            return mimeType != QStringLiteral("application/x-qt-image") &&
                   QMimeData::hasFormat(mimeType);
        }
        QStringList formats() const override {
            auto result = QMimeData::formats();
            result.removeAll(QStringLiteral("application/x-qt-image"));
            return result;
        }
    };
    auto* data = new ImageDataOnlyMimeData;
    const auto expected = image();
    data->setImageData(expected);
    data->setData(QStringLiteral("image/png"), QByteArrayLiteral("invalid PNG"));
    data->setHtml(QString(2 * 1024 * 1024, QLatin1Char('x')));
    QApplication::clipboard()->setMimeData(data);
    ImageSplitWindow split;
    split.presentSource({});
    ImageStitchingWindow stitch;
    stitch.present();
    split.paste();
    stitch.paste();
    idle(split);
    idle(stitch);
    sameImage(split.document()->image(), expected);
    require(stitch.document()->items().size() == 1,
            "corrupt encoding uses owned bitmap fallback and ignores large HTML");
    sameImage(stitch.document()->items().first().image, expected);
    split.shutdown();
    stitch.shutdown();
}

void admissionAndShutdown() {
    waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
              "prior canceled exports drain before measuring paste reservations");
    flush();
    const auto baseline = imageStitchingResourceUsage().reservedBytes;
    {
        ImageSplitWindow split;
        split.presentSource({});
        ImageStitchingWindow stitch;
        stitch.present();
        QImage oversized(8001, 8000, QImage::Format_Mono);
        require(!oversized.isNull(), "allocate a compact bitmap beyond the clipboard pixel limit");
        oversized.fill(0);
        bitmap(oversized);
        split.paste();
        stitch.paste();
        idle(split);
        idle(stitch);
        require(split.document()->image().isNull() && !split.lastError().isEmpty() &&
                    stitch.document()->items().isEmpty(),
                "clipboard pixel admission rejection preserves documents");
        bitmap();
        split.paste();
        stitch.paste();
        split.shutdown();
        stitch.shutdown();
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "shutdown drains clipboard workers");
        flush();
        require(split.document()->image().isNull() && stitch.document()->items().isEmpty(),
                "shutdown rejects queued clipboard completions");
        split.paste();
        stitch.paste();
        require(!split.loading() && !stitch.loading(), "closed windows ignore Paste");
    }
    waitUntil([&] { return imageStitchingResourceUsage().reservedBytes == baseline; },
              "clipboard cancellation and rejection release reservations");
}

void dialogsConfirmationAndExportBlockPaste(const QString& directory) {
    std::function<void(QString)> choice;
    std::function<void(bool)> confirmation;
    std::atomic_bool release = false;
    ImageSplitWindowServices splitServices;
    splitServices.chooseFile = [&](QWidget*, auto done) { choice = std::move(done); };
    splitServices.chooseDirectory = splitServices.chooseFile;
    splitServices.confirm = [&](QWidget*, bool, auto done) { confirmation = std::move(done); };
    splitServices.exportBatch = [&](const auto&, const auto&, const auto&, const auto&,
                                    const auto& cancelled, const auto&) {
        while (!release.load() && !cancelled())
            QThread::msleep(1);
        return ImageSplitExportResult{};
    };
    ImageSplitWindow split(nullptr, splitServices);
    split.presentImage(image());
    idle(split);
    const auto state = split.document()->state();
    bitmap(image({8, 9}));
    split.reload();
    require(bool(choice), "split file chooser is pending");
    split.paste();
    require(!split.loading() && split.document()->state() == state,
            "file selection blocks pasted replacement");
    std::exchange(choice, {})({});
    split.requestClose();
    require(bool(confirmation), "split close confirmation is pending");
    split.paste();
    require(!split.loading(), "confirmation blocks pasted replacement");
    std::exchange(confirmation, {})(false);
    split.saveAll();
    require(bool(choice), "split export destination chooser is pending");
    split.paste();
    require(!split.loading(), "export destination selection blocks pasted replacement");
    std::exchange(choice, {})(directory);
    require(split.busy(), "split export starts");
    split.paste();
    require(!split.loading() && split.document()->state() == state,
            "active splitting export blocks pasted replacement");
    split.shutdown();
    release.store(true);

    ImageStitchingWindowServices::Completion exported;
    ImageStitchingWindowServices stitchServices;
    stitchServices.confirm = [&](QWidget*, bool, auto done) { confirmation = std::move(done); };
    stitchServices.copy = [&](auto, QObject*, QWidget*, auto done) { exported = std::move(done); };
    ImageStitchingWindow stitch(nullptr, stitchServices);
    stitch.present();
    bitmap();
    stitch.paste();
    idle(stitch);
    stitch.editor()->addImagesRequested();
    auto* dialog = stitch.editor()->window()->findChild<QFileDialog*>();
    require(dialog, "stitch file selection is pending");
    stitch.paste();
    require(!stitch.loading(), "file selection blocks stitching paste");
    dialog->reject();
    flush();
    stitch.requestClear();
    require(bool(confirmation), "stitch confirmation is pending");
    stitch.paste();
    require(!stitch.loading(), "confirmation blocks stitching paste");
    std::exchange(confirmation, {})(false);
    stitch.copy();
    waitUntil([&] { return bool(exported); }, "stitch export reaches injected publisher");
    stitch.paste();
    require(stitch.busy() && !stitch.loading() && stitch.document()->items().size() == 1,
            "active stitching export blocks image paste");
    std::exchange(exported, {})(false, {});
    stitch.shutdown();
    waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
              "busy-state workers retire before their service fixtures");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary paste test directory exists");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 60000}).success,
            "isolated paste test storage initializes");
    styles::ThemeManager::instance().initialize(app);
    if (app.arguments().contains(QStringLiteral("--large-file-only"))) {
        scrollingExportFileImports(directory.path());
        scrollingExportFileImports(directory.path(), {1, (1 << 20) + 1});
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        return 0;
    }
    const auto run = [](const char* name, const auto& test) {
        std::cout << name << '\n' << std::flush;
        test();
    };
    run("Splitting clipboard and replacement", splittingClipboardAndReplacement);
    run("Stitching clipboard and PDF", stitchingClipboardAndPdf);
    run("File order and failures", [&] { fileSelectionOrderAndFailures(directory.path()); });
    run("Scrolling export imports", [&] { scrollingExportFileImports(directory.path()); });
    run("Long image imports",
        [&] { scrollingExportFileImports(directory.path(), {1, (1 << 20) + 1}); });
    run("Ignored content and text focus", ignoredContentAndTextFocus);
    run("Animated clipboard initial frames", animatedClipboardInitialFrames);
    run("Encoded bitmap fallback", encodedCorruptionRetainsBitmapFallback);
    run("Busy states", [&] { dialogsConfirmationAndExportBlockPaste(directory.path()); });
    run("Admission and shutdown", admissionAndShutdown);
    QApplication::clipboard()->clear();
    waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
              "paste test workers drain");
    ScreenshotExportCoordinator::shared().shutdown();
    storage.shutdown();
    std::cout << "Image paste tests passed\n";
}
