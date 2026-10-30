// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitwindow.h"
#include "snow_shot/image/imageimportlimits.h"
#include "snow_shot/presentation/components/imagespliteditorwidget.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "widgets/button.h"
#include "widgets/input_number.h"
#include "widgets/message.h"
#include "widgets/modal.h"
#include "pinned_animation_fixtures.h"
#include "pinned_animation_native_fixtures.h"

#include <QApplication>
#include <QColorSpace>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFile>
#include <QUuid>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace snow_shot::presentation;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
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
QImage image(QSize size = {1001, 700}, QColor color = QColor(45, 120, 200, 170)) {
    QImage result(size, QImage::Format_RGBA8888);
    result.fill(color);
    return result;
}
void loaded(ImageSplitWindow& window) {
    waitUntil([&] { return !window.loading() && !window.document()->image().isNull(); },
              "window image loads asynchronously");
}
adqt::widgets::AdButton* saveButton(ImageSplitWindow& window) {
    auto* button = window.editor()->window()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("imageSplitSaveAll"));
    require(button != nullptr, "Save All is discoverable");
    return button;
}
adqt::widgets::AdMessageHandle* statusMessage(ImageSplitWindow& window) {
    auto* messages = adqt::widgets::AdMessageService::instance(window.editor()->window());
    require(messages != nullptr, "split message service belongs to its window");
    for (auto* handle : messages->findChildren<adqt::widgets::AdMessageHandle*>()) {
        if (handle->isOpen() && handle->key() == QStringLiteral("imageSplitStatus"))
            return handle;
    }
    return nullptr;
}

void windowPinStartsEnabledAndPreservesEdits() {
    ImageSplitWindow window;
    window.presentImage(image());
    loaded(window);
    require(window.document()->createLine(true, 200), "create a split before toggling the pin");
    flush();
    auto* surface = window.editor()->window();
    auto* pin = surface->findChild<QToolButton*>(QStringLiteral("ad-modal-always-on-top"));
    require(pin && pin->isVisible() && pin->isChecked() && window.modal()->windowAlwaysOnTop() &&
                surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint),
            "splitting opens pinned with a checked title-bar pin button");
    const auto pinPosition = pin->mapTo(surface, QPoint());
    require(pinPosition.x() > surface->width() / 2 &&
                pinPosition.y() < window.editor()->mapTo(surface, QPoint()).y(),
            "splitting pin is in the top-right window header");
    const auto geometry = surface->geometry();
    const auto state = window.document()->state();
    for (const bool pinned : {false, true}) {
        pin->click();
        window.modal()->present();
        flush();
        require(pin->isChecked() == pinned && window.modal()->windowAlwaysOnTop() == pinned &&
                    surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint) == pinned,
                "splitting pin toggles always-on-top and survives repeated presentation");
        require(window.modal()->isOpen() && surface->isVisible() &&
                    window.editor()->window() == surface && surface->geometry() == geometry &&
                    window.document()->state() == state && window.document()->canUndo(),
                "toggling splitting pin preserves the visible window, geometry, and edits");
    }
    window.shutdown();
}

void replacementAndCancellation() {
    QVector<std::function<void(bool)>> confirmations;
    std::atomic_int reads = 0;
    ImageSplitWindowServices ports;
    ports.chooseFile = [](QWidget*, auto complete) { complete({}); };
    ports.confirm = [&](QWidget*, bool replacing, auto complete) {
        require(replacing, "replacement uses the replacement confirmation");
        require(reads > 0, "replacement is decoded before confirmation");
        confirmations.append(std::move(complete));
    };
    ports.readFile = [&](const QString& path, const auto&) -> std::optional<QImage> {
        ++reads;
        if (path == QStringLiteral("broken.png"))
            return std::nullopt;
        return image({600, 400}, Qt::green);
    };
    ImageSplitWindow window(nullptr, ports);
    const auto original = image();
    window.presentImage(original, QStringLiteral("original.photo.png"));
    loaded(window);
    require(window.imageName() == QStringLiteral("original.photo"),
            "filename loses only extension");
    require(window.document()->createLine(true, 200), "initial manual split created");
    const auto before = window.document()->state();
    const auto zoom = window.editor()->canvas()->zoom();
    const auto offset = window.editor()->canvas()->offset();
    window.reload();
    require(reads == 0 && window.document()->state() == before,
            "file selection cancel changes nothing");
    window.dropFiles({QStringLiteral("a.png"), QStringLiteral("b.png")});
    require(reads == 0 && window.document()->state() == before,
            "multifile drop preserves all edits");
    window.replaceFile(QStringLiteral("broken.png"));
    waitUntil([&] { return !window.loading(); }, "failed replacement completes");
    require(confirmations.isEmpty() && window.document()->state() == before &&
                window.document()->image() == original && !window.lastError().isEmpty(),
            "read failure preserves source and history without confirmation");
    window.replaceFile(QStringLiteral("candidate.png"));
    waitUntil([&] { return confirmations.size() == 1; }, "decoded replacement asks confirmation");
    require(!saveButton(window)->isEnabled(), "saving is disabled during confirmation");
    confirmations.takeFirst()(false);
    require(window.document()->state() == before && window.document()->image() == original &&
                window.document()->canUndo() && window.editor()->canvas()->zoom() == zoom &&
                window.editor()->canvas()->offset() == offset,
            "confirmation cancel preserves source edits history and viewport");
    window.replaceFile(QStringLiteral("candidate.png"));
    waitUntil([&] { return confirmations.size() == 1; }, "replacement confirmation repeats");
    confirmations.takeFirst()(true);
    require(window.document()->image().size() == QSize(600, 400) &&
                window.document()->settings() == ImageSplitSettings() &&
                !window.document()->canUndo() && !window.document()->canRedo() &&
                window.document()->state().verticalLines == QVector<int>{300} &&
                window.imageName() == QStringLiteral("candidate"),
            "accepted replacement resets defaults history and filename");
    window.shutdown();
}

void independentHistoryAndSourceWindows() {
    ImageSplitWindowServices ports;
    ports.readHistory = [](const auto&) -> std::optional<QImage> { return image({1200, 900}); };
    ImageSplitWindowCoordinator coordinator(nullptr, ports);
    snow_shot::storage::CaptureHistoryRecord record;
    record.id = QStringLiteral("split-history");
    record.createdUtc = QDateTime::fromString(QStringLiteral("2026-10-14T00:00:00Z"), Qt::ISODate);
    coordinator.presentHistory(record);
    coordinator.presentSource({ScreenshotExportSource::fromImage(image({600, 800})),
                               QStringLiteral("pin.png"),
                               record.createdUtc,
                               {},
                               {}});
    auto windows = coordinator.windows();
    require(windows.size() == 2 && windows[0] != windows[1],
            "each invocation creates a separate window");
    loaded(*windows[0]);
    loaded(*windows[1]);
    require(windows[0]->document()->image().size() == QSize(1200, 900) &&
                windows[1]->document()->image().size() == QSize(600, 800) &&
                windows[0]->imageName().startsWith(QStringLiteral("Screenshot_")) &&
                windows[1]->imageName() == QStringLiteral("pin"),
            "entry sources retain their own pixels and naming metadata");
    windows[0]->document()->createLine(true, 150);
    require(!windows[1]->document()->canUndo(), "histories remain independent");
    coordinator.shutdown();
    flush();
    require(coordinator.windows().isEmpty(), "coordinator closes all owned windows on shutdown");
}

class SplitSelectedFilesBackend final : public snow_shot::platform::SelectedFileBackend {
  public:
    snow_shot::platform::SelectedFileResult result;
    std::function<void(const std::function<bool()>&)> onRead;
    mutable std::atomic_int calls = 0;

    snow_shot::platform::SelectedFileTarget captureTarget() const override {
        require(QThread::currentThread() == qApp->thread(), "capture selected files on GUI thread");
        return {11, 22, 33, 44, true};
    }
    snow_shot::platform::SelectedFileResult
    selectedFiles(const snow_shot::platform::SelectedFileTarget& target,
                  const std::function<bool()>& cancelled) const override {
        require(QThread::currentThread() != qApp->thread() && target.window == 11 &&
                    target.view == 22 && target.tab == 33 && target.processId == 44 &&
                    target.desktop,
                "read the captured selection on a worker without recapturing focus");
        ++calls;
        if (onRead)
            onRead(cancelled);
        return result;
    }
};

void selectedFilesOpenIndependentWindows() {
    QTemporaryDir directory;
    require(directory.isValid(), "selected-image fixture directory exists");
    const auto first = directory.filePath(QStringLiteral("first.png"));
    const auto second = directory.filePath(QStringLiteral("second.PNG"));
    const auto broken = directory.filePath(QStringLiteral("broken.png"));
    const auto text = directory.filePath(QStringLiteral("notes.txt"));
    const auto folder = directory.filePath(QStringLiteral("folder.png"));
    require(image({120, 90}).save(first) && image({60, 80}, Qt::green).save(second) &&
                QDir().mkpath(folder),
            "write selected images and a directory with an image extension");
    for (const auto& path : {broken, text}) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write("not an image") > 0,
                "write corrupt-image and non-image fixtures");
    }
    auto backend = std::make_shared<SplitSelectedFilesBackend>();
    backend->result.paths = {first, text, folder, first, second, broken};
    std::atomic_bool entered = false;
    std::atomic_bool release = false;
    backend->onRead = [&](const auto& cancelled) {
        entered = true;
        while (!release && !cancelled())
            QThread::msleep(1);
    };
    ImageSplitWindowCoordinator coordinator;
    coordinator.presentSelectedFiles(backend, backend->captureTarget());
    require(coordinator.windows().size() == 1 && coordinator.windows()[0]->modal()->isOpen() &&
                coordinator.windows()[0]->loading(),
            "the split window opens immediately while selected files are queried");
    waitUntil([&] { return entered.load(); }, "selected-file query starts in background");
    release = true;
    waitUntil([&] { return coordinator.windows().size() == 3; },
              "each selected image receives its own split window in selection order");
    auto windows = coordinator.windows();
    loaded(*windows[0]);
    loaded(*windows[1]);
    waitUntil([&] { return !windows[2]->loading(); }, "corrupt image load completes");
    require(
        backend->calls == 1 && windows[0]->imageName() == QStringLiteral("first") &&
            windows[1]->imageName() == QStringLiteral("second") &&
            windows[0]->document()->image().size() == QSize(120, 90) &&
            windows[1]->document()->image().size() == QSize(60, 80) &&
            windows[2]->document()->image().isNull() && !windows[2]->lastError().isEmpty(),
        "duplicates and non-images are ignored and a corrupt image leaves a recoverable window");
    require(windows[0]->document()->createLine(true, 25) && !windows[1]->document()->canUndo(),
            "selected images keep independent split edits");
    coordinator.shutdown();
    flush();
}

void emptyAndFailedSelectionsRemainUsable() {
    using Error = snow_shot::platform::SelectedFileError;
    auto& languages = LanguageManager::instance();
    languages.initialize();
    require(languages.setLanguage(QStringLiteral("en_US")), "start selection tests in English");
    for (const auto error : {Error::None, Error::PermissionDenied, Error::Timeout,
                             Error::Unavailable, Error::QueryFailed}) {
        auto backend = std::make_shared<SplitSelectedFilesBackend>();
        backend->result.error = error;
        ImageSplitWindowServices services;
        services.readFile = [](const QString&, const auto&) -> std::optional<QImage> {
            return image({40, 30});
        };
        ImageSplitWindowCoordinator coordinator(nullptr, services);
        coordinator.presentSelectedFiles(backend, backend->captureTarget());
        auto* window = coordinator.windows().first();
        waitUntil([&] { return !window->loading(); }, "empty or failed selection finishes");
        require(coordinator.windows().size() == 1 && window->modal()->isOpen() &&
                    window->document()->image().isNull() &&
                    window->lastError().isEmpty() == (error == Error::None),
                "empty selection opens a clean window and query failures report an error");
        if (error == Error::QueryFailed) {
            const auto english = window->lastError();
            for (const auto* locale : {"zh_CN", "zh_TW"}) {
                require(languages.setLanguage(QString::fromLatin1(locale)),
                        "switch selected-file error language");
                flush();
                require(window->lastError() != english &&
                            window->lastError() ==
                                QCoreApplication::translate(
                                    "ImageSplitWindow",
                                    "Could not read the selected image files. Please try again."),
                        "selected-file errors follow runtime language changes");
            }
            require(languages.setLanguage(QStringLiteral("en_US")), "restore English");
        }
        window->replaceFile(QStringLiteral("replacement.png"));
        loaded(*window);
        require(window->lastError().isEmpty(), "empty or failed selection allows manual loading");
        coordinator.shutdown();
        flush();
    }
    ImageSplitWindowCoordinator coordinator;
    auto backend = std::make_shared<SplitSelectedFilesBackend>();
    coordinator.presentSelectedFiles(backend, {});
    require(backend->calls == 0 && coordinator.windows().size() == 1 &&
                !coordinator.windows()[0]->loading() &&
                coordinator.windows()[0]->lastError().isEmpty(),
            "an unavailable selection target opens an empty window without querying");
    coordinator.shutdown();
    flush();
}

void selectedFileQueriesCancelWithWindowLifecycle() {
    for (const bool shutdown : {false, true}) {
        auto backend = std::make_shared<SplitSelectedFilesBackend>();
        std::atomic_bool entered = false;
        std::atomic_bool cancelled = false;
        backend->onRead = [&](const auto& isCancelled) {
            entered = true;
            while (!isCancelled())
                QThread::msleep(1);
            cancelled = true;
        };
        ImageSplitWindowCoordinator coordinator;
        coordinator.presentSelectedFiles(backend, backend->captureTarget());
        waitUntil([&] { return entered.load(); }, "cancellable selection query starts");
        if (shutdown)
            coordinator.shutdown();
        else
            coordinator.windows().first()->requestClose();
        waitUntil([&] { return cancelled.load() && coordinator.windows().isEmpty(); },
                  "closing or shutting down cancels the selection query and prevents late windows");
        if (shutdown) {
            coordinator.presentSelectedFiles(backend, backend->captureTarget());
            require(coordinator.windows().isEmpty() && backend->calls == 1,
                    "shutdown rejects new selected-image requests");
        }
        flush();
    }
}

void savedPinsOpenIndependentSplitWindows(const QString& directory, bool styledText) {
    auto& repository = snow_shot::storage::ApplicationStorage::instance().pinnedWindows();
    const auto originalCount = repository.summaries().size();
    const auto setGeometry = [](snow_shot::storage::PinnedWindowRecord& record, QSize size) {
        record.nativeGeometry = QRect(QPoint(), size);
        record.canvasSourceRect = QRectF(record.nativeGeometry);
        record.contentCanvasRect = record.canvasSourceRect;
        record.surfaceCanvasRect = record.canvasSourceRect;
        record.initialWindowSize = size;
    };
    snow_shot::storage::PinnedWindowRecord pixels;
    pixels.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pixels.image = image({540, 270}, Qt::cyan);
    pixels.createdUtc = QDateTime::fromString(QStringLiteral("2026-10-14T00:00:00Z"), Qt::ISODate);
    pixels.ignored = true;
    pixels.canvasSession = QByteArrayLiteral("saved canvas is not a split input");
    setGeometry(pixels, pixels.image.size());
    require(repository.upsert(pixels).success, "save closed image pin for splitting");
    snow_shot::storage::PinnedWindowRecord file;
    file.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file.sourceKind = snow_shot::storage::PinnedWindowSourceKind::ClipboardImageFile;
    file.image = image({420, 300}, Qt::blue);
    file.originalFilePath = QDir(directory).filePath(QStringLiteral("split-original.png"));
    file.originalFileName = QStringLiteral("split-original.png");
    file.hidden = true;
    setGeometry(file, file.image.size());
    require(file.image.save(file.originalFilePath) && repository.upsert(file).success,
            "save hidden file pin for splitting");
    snow_shot::storage::PinnedWindowRecord text;
    text.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    text.sourceKind = snow_shot::storage::PinnedWindowSourceKind::ClipboardText;
    text.originalHtml = QStringLiteral("<b>Saved text pin</b>");
    text.originalText = QStringLiteral("Saved text pin");
    text.firstCreationTextDpi = 1.5;
    text.originalFileName = QStringLiteral("saved-text.txt");
    text.createdUtc = pixels.createdUtc;
    if (styledText) {
        snow_shot::storage::PinnedTextStyle style;
        style.fontFamily = QStringLiteral("Arial");
        style.maximumWidth = 200;
        style.fontSize = 24;
        style.textColor = QColor(255, 0, 0, 128);
        style.backgroundColor = Qt::transparent;
        text.textStyle = style;
    }
    setGeometry(text, QSize(300, 90));
    // Publishing a new pin reveals hidden members of its group. Hide the file fixture only after
    // all sources exist, so the split loader receives a genuinely hidden saved record.
    require(repository.upsert(text).success &&
                repository.setRecordsHidden({file.id}, true).success &&
                repository.flush().success && QFile::remove(file.originalFilePath),
            "persist split inputs and remove the original file");
    ScreenshotClipboardOriginalContent original;
    original.html = text.originalHtml;
    original.text = text.originalText;
    const auto legacyRendered =
        ScreenshotClipboardContentReader::renderOriginalText(original, text.firstCreationTextDpi);
    original.textStyle = text.textStyle;
    const auto rendered =
        ScreenshotClipboardContentReader::renderOriginalText(original, text.firstCreationTextDpi);
    require(rendered.has_value(), "render saved text reference");
    require(!styledText || (legacyRendered && legacyRendered->image != rendered->image),
            "styled split fixture must differ from legacy text rendering");

    ImageSplitWindowCoordinator coordinator;
    for (const auto& id : {pixels.id, file.id, text.id})
        coordinator.presentPinnedRecord(id);
    const auto windows = coordinator.windows();
    require(windows.size() == 3, "every saved pin opens its own split window");
    for (auto* window : windows)
        loaded(*window);
    const auto samePixels = [](const QImage& actual, const QImage& source) {
        const QImage expected = source.convertToFormat(QImage::Format_RGBA8888);
        const QImage rgba = actual.convertToFormat(QImage::Format_RGBA8888);
        return rgba.size() == expected.size() &&
               std::equal(expected.constBits(), expected.constBits() + expected.sizeInBytes(),
                          rgba.constBits());
    };
    require(samePixels(windows[0]->document()->image(), pixels.image) &&
                samePixels(windows[1]->document()->image(), file.image) &&
                samePixels(windows[2]->document()->image(), rendered->image),
            "saved image, file, and text pins preserve full-resolution pixels");
    require(windows[0]->imageName() == QStringLiteral("Screenshot_%1")
                                           .arg(pixels.createdUtc.toLocalTime().toString(
                                               QStringLiteral("yyyyMMdd-HHmmss"))) &&
                windows[1]->imageName() == QStringLiteral("split-original") &&
                windows[2]->imageName() == QStringLiteral("saved-text"),
            "saved split sources retain creation time and filenames alongside text styling");
    windows[0]->document()->createLine(true, 100);
    require(!windows[1]->document()->canUndo() && !windows[2]->document()->canUndo(),
            "saved pin split edits remain independent");
    const auto retainedPixels = repository.loadRecord(pixels.id);
    const auto retainedFile = repository.loadRecord(file.id);
    require(repository.summaries().size() == originalCount + 3 && retainedPixels && retainedFile &&
                retainedPixels->ignored && retainedPixels->canvasSession == pixels.canvasSession &&
                retainedFile->hidden,
            "splitting preserves saved pins, their canvas data, and their lifecycle state");
    coordinator.presentPinnedRecord(QStringLiteral("missing-split-pin"));
    auto* missing = coordinator.windows().last();
    waitUntil([&] { return !missing->loading(); }, "missing saved pin load completes");
    require(missing->document()->image().isNull() && !missing->lastError().isEmpty() &&
                missing->modal()->isOpen() && !saveButton(*missing)->isEnabled(),
            "missing pin reports a load error without enabling export");
    coordinator.shutdown();
    flush();
    require(coordinator.windows().isEmpty(), "saved pin split windows close on shutdown");
    coordinator.presentPinnedRecord(pixels.id);
    require(coordinator.windows().isEmpty(), "shutdown rejects new saved pin split windows");
    require(repository.removeMany({pixels.id, file.id, text.id}).success,
            "remove saved pin split fixtures");
}

void saveCommitsVisibleDraft() {
    std::function<void(QString)> chosen;
    int choices = 0;
    ImageSplitWindowServices ports;
    ports.chooseDirectory = [&](QWidget*, auto complete) {
        ++choices;
        chosen = std::move(complete);
    };
    ImageSplitWindow window(nullptr, ports);
    window.presentImage(image());
    loaded(window);
    auto* count = window.editor()->findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("imageSplitCount"));
    auto* input = count ? count->findChild<QLineEdit*>() : nullptr;
    require(input, "count draft editor is discoverable");
    input->setFocus();
    input->selectAll();
    QKeyEvent three(QEvent::KeyPress, Qt::Key_3, Qt::NoModifier, QStringLiteral("3"));
    QApplication::sendEvent(input, &three);
    require(window.document()->settings().count == 2, "visible draft remains uncommitted");
    window.saveAll();
    require(choices == 1 && bool(chosen) && window.document()->settings().count == 3 &&
                window.document()->layout().outputCount() == 3 && window.document()->canUndo(),
            "Save All commits the visible valid draft before selecting its destination");
    chosen({});
    window.document()->undo();
    input->setFocus();
    input->selectAll();
    QKeyEvent zero(QEvent::KeyPress, Qt::Key_0, Qt::NoModifier, QStringLiteral("0"));
    QApplication::sendEvent(input, &zero);
    window.saveAll();
    require(choices == 1 && window.document()->settings().count == 2,
            "invalid visible drafts prevent saving and retain valid geometry");
    window.shutdown();
}

void nativeHistoryPixelsAreShared() {
    QImage original(QSize(4096, 1024), QImage::Format_ARGB32_Premultiplied);
    original.fill(QColor(55, 99, 137, 123));
    original.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    original.setDevicePixelRatio(2);
    ImageSplitWindowServices ports;
    ports.readHistory = [original](const auto&) -> std::optional<QImage> { return original; };
    ImageSplitWindow window(nullptr, ports);
    snow_shot::storage::CaptureHistoryRecord record;
    record.id = QStringLiteral("native-history-source");
    window.presentHistory(record);
    loaded(window);
    const auto& actual = window.document()->image();
    require(actual.constBits() == original.constBits() && actual.format() == original.format() &&
                actual.colorSpace() == original.colorSpace() && actual.devicePixelRatio() == 2 &&
                actual.size() == original.size(),
            "history loading shares immutable native pixels without metadata detachment");
    require(window.document()->preview().size() == QSize(2048, 512) &&
                window.document()->preview().colorSpace() == original.colorSpace() &&
                window.document()->layout().regionAt(0) == QRect(0, 0, 2048, 1024),
            "preview and splits preserve native color interpretation and physical dimensions");
    window.shutdown();
}

void realAnimatedFilesKeepTheirInitialCanvas(const QString& directory) {
    auto fixtures = pinned_animation_fixtures::images();
    fixtures.append(pinned_animation_native_fixtures::images());
    fixtures.append(pinned_animation_fixtures::hiddenPosterAnimation());
    fixtures.append(pinned_animation_fixtures::singleFrameImages());
    const QColorSpace profile(QColorSpace::DisplayP3);
    const auto icc = profile.iccProfile();
    require(!icc.isEmpty(), "animated split fixture must carry a real ICC profile");
    auto profiled = pinned_animation_fixtures::images().last();
    QByteArray profileData("split-profile", 13);
    profileData.append('\0');
    profileData.append('\0'); // PNG compression method: zlib.
    profileData.append(qCompress(icc).mid(4));
    QByteArray profileChunk;
    pinned_animation_native_fixtures::detail::chunk(profileChunk, "iCCP", profileData);
    profiled.bytes.insert(33, profileChunk);
    fixtures.append(profiled);
    const auto sameVisiblePixels = [](const QImage& actual, const QImage& expected) {
        if (actual.isNull() || actual.size() != expected.size())
            return false;
        const auto actualRgba = actual.convertToFormat(QImage::Format_RGBA8888);
        const auto expectedRgba = expected.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < expected.height(); ++y) {
            const auto* actualRow = actualRgba.constScanLine(y);
            const auto* expectedRow = expectedRgba.constScanLine(y);
            for (int x = 0; x < expected.width(); ++x) {
                const qsizetype offset = qsizetype(x) * 4;
                if (actualRow[offset + 3] != expectedRow[offset + 3] ||
                    (expectedRow[offset + 3] != 0 &&
                     !std::equal(expectedRow + offset, expectedRow + offset + 3,
                                 actualRow + offset)))
                    return false;
            }
        }
        return true;
    };
    for (qsizetype index = 0; index < fixtures.size(); ++index) {
        const auto& fixture = fixtures[index];
        const auto path = QDir(directory).filePath(
            QStringLiteral("animated-split-%1.%2").arg(index).arg(fixture.suffix));
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) &&
                    file.write(fixture.bytes) == fixture.bytes.size(),
                "write real animated split file fixture");
        file.close();
        const auto snapshots = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
        require(snapshots.size() == 1, "capture animated split fixture revision");
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.localImage = snapshots.first();
        // The complete playback decoder is an independent reference for first-frame-only import.
        const auto complete = ScreenshotClipboardContentReader::decode(std::move(snapshot));
        require(complete && !complete->image.isNull(), "decode complete animated split reference");
        if (index == fixtures.size() - 1)
            require(complete->image.colorSpace() == profile,
                    "the complete reference must retain the fixture's Display P3 profile");
        ImageSplitWindow window;
        window.replaceFile(path);
        loaded(window);
        const auto& actual = window.document()->image();
        require(sameVisiblePixels(actual, complete->image) &&
                    actual.colorSpace() == complete->image.colorSpace(),
                "real file splitting must retain the initial composed RGBA canvas and profile");
        require(window.imageName() == QStringLiteral("animated-split-%1").arg(index),
                "real file import retains its filename");
        window.shutdown();
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "animated split fixture workers drain before the next window");
        flush();
    }
}

void exportAndRuntimeLanguage(const QString& directory) {
    int choices = 0;
    QStringList openedDirectories;
    QString exportDirectory = directory;
    ImageSplitWindowServices ports;
    ports.chooseDirectory = [&](QWidget*, auto complete) {
        ++choices;
        complete(exportDirectory);
    };
    ports.openDirectory = [&](const QString& path) {
        require(QThread::currentThread() == qApp->thread(),
                "export directory opens on the GUI thread");
        require(QFileInfo::exists(QDir(path).filePath(QStringLiteral("1.png"))) &&
                    QFileInfo::exists(QDir(path).filePath(QStringLiteral("2.png"))),
                "export directory opens after all images are saved");
        openedDirectories.append(path);
    };
    ImageSplitWindow window(nullptr, ports);
    window.presentImage(image({80, 60}), QStringLiteral("transparent.png"));
    loaded(window);
    ImageSplitExportResult result;
    int finished = 0;
    QObject::connect(&window, &ImageSplitWindow::exportFinished, &window, [&](const auto& value) {
        result = value;
        ++finished;
    });
    window.saveAll();
    window.saveAll();
    require(choices == 1 && saveButton(window)->busy(),
            "busy export prevents duplicate submission");
    waitUntil([&] { return finished == 1; }, "batch export completes");
    require(result.succeeded() && result.savedCount == 2 &&
                QFileInfo::exists(QDir(result.directory).filePath(QStringLiteral("1.png"))) &&
                QFileInfo::exists(QDir(result.directory).filePath(QStringLiteral("2.png"))) &&
                window.modal()->isOpen() && saveButton(window)->isEnabled(),
            "Save All writes numbered PNGs and keeps the editable window open");
    require(openedDirectories == QStringList{result.directory} && result.directory != directory,
            "successful export opens its generated output directory exactly once");
    window.saveAll();
    waitUntil([&] { return finished == 2; }, "repeat batch export completes");
    require(result.directory.endsWith(QStringLiteral(" (2)")) && openedDirectories.size() == 2 &&
                openedDirectories.last() == result.directory,
            "repeat export opens its new unique folder exactly once");
    exportDirectory = QDir(directory).filePath(QStringLiteral("missing-parent"));
    window.saveAll();
    waitUntil([&] { return finished == 3; }, "invalid export destination reports failure");
    require(!result.succeeded() && !window.lastError().isEmpty() && openedDirectories.size() == 2,
            "failure preserves editable image without opening a directory");
    auto& languages = LanguageManager::instance();
    languages.initialize();
    require(languages.setLanguage(QStringLiteral("zh_CN")), "switch to simplified Chinese");
    flush();
    require(saveButton(window)->text() == QString::fromUtf8("全部保存") &&
                window.modal()->windowTitle() == QString::fromUtf8("图片分割") &&
                window.lastError() == QString::fromUtf8("无法创建输出文件夹。"),
            "open split window retranslates at runtime");
    require(languages.setLanguage(QStringLiteral("zh_TW")), "switch to traditional Chinese");
    flush();
    require(window.modal()->windowTitle() == QString::fromUtf8("圖片分割"),
            "traditional Chinese uses the image splitting feature name");
    require(languages.setLanguage(QStringLiteral("en_US")), "restore English");
    flush();
    require(saveButton(window)->text() == QStringLiteral("Save All") &&
                window.modal()->windowTitle() == QStringLiteral("Image Splitting"),
            "English source copy restored");
    require(window.lastError() == QStringLiteral("The output folder could not be created."),
            "existing error messages retranslate at runtime");
    window.shutdown();
}

void exportFeedbackUsesLoadingButtonAndMessages() {
    using adqt::widgets::AdMessage;
    using adqt::widgets::AdMessageHandle;
    std::function<void(QString)> chosen;
    int choices = 0;
    int finished = 0;
    QStringList openedDirectories;
    std::atomic_int stage = 0;
    std::atomic_bool entered = false;
    std::atomic_bool exited = false;
    ImageSplitWindowServices ports;
    ports.chooseDirectory = [&](QWidget*, auto complete) {
        ++choices;
        chosen = std::move(complete);
    };
    ports.openDirectory = [&](const QString& path) { openedDirectories.append(path); };
    ports.exportBatch = [&](const QImage&, const ImageSplitLayout& layout, const QString& directory,
                            const QString&, const auto& cancelled, const auto& progress) {
        QElapsedTimer timer;
        timer.start();
        entered = true;
        while (stage < 1 && !cancelled() && timer.elapsed() < 15000)
            QThread::msleep(1);
        progress(1, layout.outputCount());
        while (stage < 2 && !cancelled() && timer.elapsed() < 15000)
            QThread::msleep(1);
        exited = true;
        const bool success = stage == 2 && !cancelled();
        return ImageSplitExportResult{directory,
                                      success ? QString() : QStringLiteral("test write failure"),
                                      success ? layout.outputCount() : 1, layout.outputCount()};
    };
    ImageSplitWindow window(nullptr, ports);
    window.presentImage(image({80, 60}), QStringLiteral("feedback.png"));
    loaded(window);
    auto* save = saveButton(window);
    QObject::connect(&window, &ImageSplitWindow::exportFinished, &window,
                     [&](const auto&) { ++finished; });
    window.saveAll();
    require(choices == 1 && !save->busy() && !statusMessage(window),
            "choosing the destination does not start writing feedback");
    chosen({});
    require(save->isEnabled() && !save->busy() && !statusMessage(window) &&
                openedDirectories.isEmpty(),
            "canceling the destination leaves the save action ready without opening a directory");

    window.saveAll();
    chosen(QStringLiteral("test-folder"));
    QPointer<AdMessageHandle> message = statusMessage(window);
    require(window.busy() && save->busy() && save->isEnabled() &&
                save->cursor().shape() != Qt::ForbiddenCursor && message &&
                message->type() == AdMessage::Type::Loading &&
                message->content() == QStringLiteral("Saving images: 0 of 2"),
            "writing starts with the loading button appearance and a loading Message");
    waitUntil([&] { return entered.load(); }, "batch writer starts");
    flush();
    require(message->noticeWidget() && message->noticeWidget()->isVisible() &&
                message->noticeWidget()->window() == window.editor()->window(),
            "the writing prompt is visible in the split window");
    save->click();
    window.saveAll();
    require(choices == 2, "loading blocks repeated save requests");
    stage = 1;
    waitUntil([&] { return message->content() == QStringLiteral("Saving images: 1 of 2"); },
              "writing progress updates its Message");
    require(statusMessage(window) == message && save->busy(),
            "progress keeps the same Message and loading button");
    require(openedDirectories.isEmpty(), "export does not open the directory while writing");
    stage = 2;
    waitUntil([&] { return finished == 1; }, "successful writing completes");
    require(statusMessage(window) == message && message->type() == AdMessage::Type::Success &&
                message->content() == QStringLiteral("Saved 2 images to test-folder") &&
                !window.busy() && !save->busy() && save->isEnabled(),
            "success updates the Message and restores the save button");
    require(openedDirectories == QStringList{QStringLiteral("test-folder")},
            "completed export opens the directory once");

    entered = exited = false;
    stage = 0;
    window.saveAll();
    chosen(QStringLiteral("test-folder"));
    require(save->busy() && message->type() == AdMessage::Type::Loading,
            "another export restarts loading feedback");
    stage = 3;
    waitUntil([&] { return finished == 2; }, "failed writing completes");
    require(statusMessage(window) == message && message->type() == AdMessage::Type::Error &&
                message->content() ==
                    QStringLiteral("Saved 1 of 2 images to test-folder. test write failure") &&
                !window.busy() && !save->busy() && save->isEnabled(),
            "partial failure reports its result through Message and allows retry");
    require(openedDirectories.size() == 1, "partial failure does not open the directory");

    entered = exited = false;
    stage = 0;
    window.saveAll();
    chosen(QStringLiteral("test-folder"));
    waitUntil([&] { return entered.load(); }, "retry writer starts before shutdown");
    window.shutdown();
    require(!window.busy() && !save->busy() && !statusMessage(window),
            "shutdown clears the loading button and writing Message");
    waitUntil([&] { return exited.load(); }, "shutdown cancels writing");
    waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
              "canceled writing drains");
    flush();
    require(finished == 2 && !statusMessage(window) && openedDirectories.size() == 1,
            "late canceled writing cannot reopen feedback or open a directory");
}

void renderPreview(const QString& path, bool dark, qreal dpr) {
    styles::ThemeManager::instance().setThemeMode(dark ? styles::ThemeMode::Dark
                                                       : styles::ThemeMode::Light);
    QImage source({1200, 900}, QImage::Format_RGBA8888);
    source.fill(QColor(234, 240, 249));
    QPainter painter(&source);
    const QList<QColor> colors{QColor(38, 76, 103),  QColor(42, 131, 143),  QColor(233, 173, 79),
                               QColor(213, 105, 84), QColor(129, 115, 168), QColor(194, 218, 205)};
    for (int i = 0; i < 6; ++i)
        painter.fillRect(QRect((i % 3) * 400, (i / 3) * 450, 400, 450), colors[i]);
    painter.setPen(Qt::white);
    QFont font;
    font.setPixelSize(48);
    painter.setFont(font);
    painter.drawText(QRect(50, 50, 1100, 800), Qt::AlignCenter,
                     QStringLiteral("Snow Shot\nImage Splitting"));
    painter.end();
    ImageSplitWindow window;
    window.presentImage(source, QStringLiteral("design.png"));
    loaded(window);
    auto settings = window.document()->settings();
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 2;
    settings.columns = 3;
    require(window.document()->setSettings(settings), "preview grid configured");
    window.editor()->window()->resize(1200, 800);
    flush();
    window.editor()->canvas()->fitToCanvas();
    flush();
    QElapsedTimer animation;
    animation.start();
    while (animation.elapsed() < 300) {
        flush();
        QThread::msleep(1);
    }
    auto* widget = window.editor()->window();
    QImage rendered(QSize(qRound(widget->width() * dpr), qRound(widget->height() * dpr)),
                    QImage::Format_ARGB32_Premultiplied);
    rendered.setDevicePixelRatio(dpr);
    rendered.fill(Qt::transparent);
    widget->render(&rendered);
    require(rendered.save(path), "preview image saved");
    window.shutdown();
}

void admissionAndLateCallbacks() {
    ImageSplitWindow thin;
    thin.presentImage(image({4000, 1}));
    loaded(thin);
    require(thin.document()->image().size() == QSize(4000, 1) &&
                thin.document()->preview().size() == QSize(2048, 1),
            "extreme aspect images retain a nonempty downscaled preview");
    thin.shutdown();
    std::atomic_int reads = 0;
    ImageSplitWindowServices admission;
    admission.readHistory = [&](const auto&) -> std::optional<QImage> {
        ++reads;
        return image();
    };
    snow_shot::storage::CaptureHistoryRecord record;
    record.result.emplace();
    record.result->imageSize =
        QSize(65536, static_cast<int>(snow_shot::image::maximumImageImportPixels / 65536 + 1));
    ImageSplitWindow tooLarge(nullptr, admission);
    tooLarge.presentHistory(record);
    waitUntil([&] { return !tooLarge.loading(); }, "oversized history source is rejected");
    require(reads == 0 && tooLarge.document()->image().isNull() && !tooLarge.lastError().isEmpty(),
            "history size is admitted before allocating decoded pixels");
    tooLarge.shutdown();

    std::function<void(QString)> chosen;
    std::function<void(bool)> confirmed;
    ImageSplitWindowServices ports;
    ports.chooseFile = [&](QWidget*, auto complete) { chosen = std::move(complete); };
    ports.confirm = [&](QWidget*, bool, auto complete) { confirmed = std::move(complete); };
    ports.readFile = [&](const QString&, const auto&) -> std::optional<QImage> {
        ++reads;
        return image({400, 300});
    };
    auto window = std::make_unique<ImageSplitWindow>(nullptr, ports);
    window->presentImage(image());
    loaded(*window);
    window->reload();
    require(bool(chosen), "file choice is pending");
    window.reset();
    chosen(QStringLiteral("late.png"));
    flush();
    require(reads == 0, "destroyed window ignores late file selection");
    window = std::make_unique<ImageSplitWindow>(nullptr, ports);
    window->presentImage(image());
    loaded(*window);
    window->replaceFile(QStringLiteral("replacement.png"));
    waitUntil([&] { return bool(confirmed); }, "replacement confirmation is pending");
    window.reset();
    confirmed(true);
    flush();
    require(reads == 1, "destroyed window ignores late replacement confirmation");
}

void shutdownDuringWorkers() {
    std::atomic_bool reading = false;
    std::atomic_bool finishedReading = false;
    ImageSplitWindowServices ports;
    ports.readFile = [&](const QString&, const auto& cancelled) -> std::optional<QImage> {
        reading = true;
        while (!cancelled())
            QThread::msleep(1);
        finishedReading = true;
        return std::nullopt;
    };
    auto window = std::make_unique<ImageSplitWindow>(nullptr, ports);
    window->presentImage(image());
    loaded(*window);
    window->replaceFile(QStringLiteral("slow.png"));
    waitUntil([&] { return reading.load(); }, "file load entered worker");
    window.reset();
    waitUntil([&] { return finishedReading.load(); }, "destroyed window cancels image worker");

    std::atomic_bool exporting = false;
    std::atomic_bool finishedExporting = false;
    ports = {};
    ports.chooseDirectory = [](QWidget*, auto complete) {
        complete(QStringLiteral("test-folder"));
    };
    ports.exportBatch = [&](const QImage&, const ImageSplitLayout& layout, const QString& directory,
                            const QString&, const auto& cancelled, const auto&) {
        exporting = true;
        while (!cancelled())
            QThread::msleep(1);
        finishedExporting = true;
        return ImageSplitExportResult{directory, QStringLiteral("test cancellation"), 0,
                                      layout.outputCount()};
    };
    window = std::make_unique<ImageSplitWindow>(nullptr, ports);
    window->presentImage(image());
    loaded(*window);
    window->saveAll();
    waitUntil([&] { return exporting.load(); }, "batch writer entered worker");
    window.reset();
    waitUntil([&] { return finishedExporting.load(); }, "destroyed window cancels export worker");
}

void canceledCloseResumesReplacement() {
    QVector<bool> replacing;
    QVector<std::function<void(bool)>> confirmations;
    std::atomic_bool entered = false;
    std::atomic_bool release = false;
    ImageSplitWindowServices ports;
    ports.readFile = [&](const QString&, const auto& cancelled) -> std::optional<QImage> {
        entered = true;
        while (!release && !cancelled())
            QThread::msleep(1);
        return image({400, 300});
    };
    ports.confirm = [&](QWidget*, bool replace, auto complete) {
        replacing.append(replace);
        confirmations.append(std::move(complete));
    };
    ImageSplitWindow window(nullptr, ports);
    window.presentImage(image());
    loaded(window);
    window.document()->createLine(true, 200);
    const auto before = window.document()->state();
    window.replaceFile(QStringLiteral("slow.png"));
    waitUntil([&] { return entered.load(); }, "replacement starts before close confirmation");
    window.requestClose();
    require(replacing == QVector<bool>{false}, "close decision is pending");
    release = true;
    waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
              "replacement worker finishes during close decision");
    flush();
    require(window.loading() && confirmations.size() == 1,
            "decoded replacement waits for close decision");
    confirmations.takeFirst()(false);
    require(replacing == QVector<bool>({false, true}) && confirmations.size() == 1,
            "canceling close continues to replacement confirmation");
    confirmations.takeFirst()(false);
    require(window.document()->state() == before && !window.loading() && window.modal()->isOpen(),
            "canceling both decisions preserves image edits and open window");
    window.shutdown();
}

void loadingErrorsFollowLanguageAcrossQueuedCompletion() {
    auto& languages = LanguageManager::instance();
    languages.initialize();
    require(languages.setLanguage(QStringLiteral("en_US")), "start load failure in English");
    ImageSplitWindowServices ports;
    ports.readFile = [](const QString&, const auto&) -> std::optional<QImage> {
        return std::nullopt;
    };
    ImageSplitWindow window(nullptr, ports);
    window.presentImage(image());
    loaded(window);
    const auto before = window.document()->state();
    window.replaceFile(QStringLiteral("broken-language-race.png"));
    // Hold GUI completion delivery until the worker has already produced its English error.
    QElapsedTimer timer;
    timer.start();
    while (ScreenshotExportCoordinator::shared().pendingJobCount() != 0 && timer.elapsed() < 15000)
        QThread::msleep(1);
    require(ScreenshotExportCoordinator::shared().pendingJobCount() == 0 && window.loading(),
            "failed image read waits for queued GUI completion");
    require(languages.setLanguage(QStringLiteral("zh_CN")),
            "switch language before queued load failure delivery");
    waitUntil([&] { return !window.loading(); }, "queued load failure reaches the window");
    const char* source = "The image could not be read or is no longer available.";
    const auto translated = QCoreApplication::translate("ImageSplitWindow", source);
    require(translated != QString::fromUtf8(source) && window.lastError() == translated &&
                window.document()->state() == before,
            "queued loading errors use the current language and preserve edits");
    require(languages.setLanguage(QStringLiteral("en_US")), "restore English after load race");
    flush();
    require(window.lastError() == QString::fromUtf8(source),
            "existing loading errors retain their stable translation source");
    window.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("snow-shot-image-split-tests"));
    QTemporaryDir directory;
    require(directory.isValid(), "temporary test storage created");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 60000}).success,
            "isolated application storage initialized");
    styles::ThemeManager::instance().initialize(application);
    const auto arguments = application.arguments();
    const auto preview = arguments.indexOf(QStringLiteral("--preview"));
    if (arguments.contains(QStringLiteral("--selected-files-only"))) {
        selectedFilesOpenIndependentWindows();
        emptyAndFailedSelectionsRemainUsable();
        selectedFileQueriesCancelWithWindowLifecycle();
    } else if (arguments.contains(QStringLiteral("--import-sources-only"))) {
        replacementAndCancellation();
        independentHistoryAndSourceWindows();
        savedPinsOpenIndependentSplitWindows(directory.path(), false);
        savedPinsOpenIndependentSplitWindows(directory.path(), true);
        nativeHistoryPixelsAreShared();
        realAnimatedFilesKeepTheirInitialCanvas(directory.path());
        admissionAndLateCallbacks();
        shutdownDuringWorkers();
        canceledCloseResumesReplacement();
        loadingErrorsFollowLanguageAcrossQueuedCompletion();
    } else if (preview >= 0) {
        require(preview + 1 < arguments.size(), "preview requires output path");
        renderPreview(arguments[preview + 1], arguments.contains(QStringLiteral("--dark")),
                      arguments.contains(QStringLiteral("--dpr2"))     ? 2
                      : arguments.contains(QStringLiteral("--dpr1.5")) ? 1.5
                                                                       : 1);
    } else {
        windowPinStartsEnabledAndPreservesEdits();
        replacementAndCancellation();
        independentHistoryAndSourceWindows();
        selectedFilesOpenIndependentWindows();
        emptyAndFailedSelectionsRemainUsable();
        selectedFileQueriesCancelWithWindowLifecycle();
        savedPinsOpenIndependentSplitWindows(directory.path(), false);
        savedPinsOpenIndependentSplitWindows(directory.path(), true);
        nativeHistoryPixelsAreShared();
        realAnimatedFilesKeepTheirInitialCanvas(directory.path());
        saveCommitsVisibleDraft();
        exportAndRuntimeLanguage(directory.path());
        exportFeedbackUsesLoadingButtonAndMessages();
        admissionAndLateCallbacks();
        shutdownDuringWorkers();
        canceledCloseResumesReplacement();
        loadingErrorsFollowLanguageAcrossQueuedCompletion();
    }
    waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
              "test workers drain");
    ScreenshotExportCoordinator::shared().shutdown();
    storage.shutdown();
    std::cout << "Image split window tests passed\n";
    return 0;
}
