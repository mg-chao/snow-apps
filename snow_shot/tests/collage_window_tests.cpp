// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagewindow.h"
#include "snow_shot/presentation/collagewindowcoordinator.h"
#include "snow_shot/presentation/collagesettingsstore.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotmanualsave.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "../src/presentation/capture/screenshotcaptureinputblockers.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_number.h"
#include "widgets/message.h"
#include "widgets/modal.h"
#include "widgets/result.h"
#include "widgets/segmented.h"
#include "pinned_animation_fixtures.h"
#include "bmp_import_fixtures.h"

#include <QApplication>
#include <QColorSpace>
#include <QCursor>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QLabel>
#include <QMouseEvent>
#include <QMimeData>
#include <QPointer>
#include <QPainter>
#include <QScopeGuard>
#include <QScreen>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QTranslator>
#include <QUuid>
#include <QUrl>

#include <cstdlib>
#include <array>
#include <atomic>
#include <iostream>
#include <limits>

using namespace snow_shot::presentation;
namespace storage = snow_shot::storage;
namespace platform = snow_shot::platform;
using adqt::widgets::AdButton;
using adqt::widgets::AdMessage;
using adqt::widgets::AdMessageHandle;
using adqt::widgets::AdMessageService;
using adqt::widgets::AdModal;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

void waitUntil(const std::function<bool()>& ready, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 15000) {
        flushEvents();
        QThread::msleep(1);
    }
    require(ready(), message);
}

void timed(const char* name, const std::function<void()>& work) {
    QElapsedTimer timer;
    timer.start();
    work();
    std::cout << name << ": " << timer.elapsed() << " ms\n" << std::flush;
}

QImage image(QSize size = {3, 2}, QColor color = Qt::red) {
    QImage result(size, QImage::Format_RGBA8888);
    result.fill(color);
    result.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return result;
}

void resetSettings() {
    const CollageSettingsStore settings(storage::ApplicationStorage::instance().configuration());
    for (const auto* field :
         {"direction", "sizing_mode", "rows", "columns", "custom_grid_initialized", "border_px",
          "spacing_px", "background_color"})
        require(settings.write(QString::fromLatin1(field), {}), "reset isolated collage settings");
}

AdButton* button(CollageWindow& window, const char* name) {
    auto* result = window.editor()->window()->findChild<AdButton*>(QString::fromLatin1(name));
    require(result != nullptr, "collage action is discoverable");
    return result;
}

bool hasInlineStatus(CollageWindow& window) {
    return window.editor()->findChild<QWidget*>(QStringLiteral("collageStatus")) ||
           window.editor()->findChild<QWidget*>(QStringLiteral("collageStatusScroll"));
}

AdMessageHandle* statusMessage(CollageWindow& window) {
    auto* service = AdMessageService::instance(window.editor()->window());
    require(service != nullptr, "collage message service belongs to its window");
    for (auto* handle : service->findChildren<AdMessageHandle*>()) {
        if (handle->key() == QStringLiteral("collageStatus") && handle->isOpen())
            return handle;
    }
    return nullptr;
}

QString messageError(CollageWindow& window) {
    auto* message = statusMessage(window);
    require(message && message->type() == AdMessage::Type::Error && !hasInlineStatus(window),
            "errors use Message without an inline collage prompt");
    return message->content();
}

class SelectedFilesTestBackend final : public platform::SelectedFileBackend {
  public:
    platform::SelectedFileResult result;
    std::function<void()> onCapture;
    std::function<void(const std::function<bool()>&)> onRead;
    mutable std::atomic_int calls = 0;

    platform::SelectedFileTarget captureTarget() const override {
        require(QThread::currentThread() == qApp->thread(), "capture selection on the GUI thread");
        if (onCapture)
            onCapture();
        return {11, 22, 33, 44, true};
    }

    platform::SelectedFileResult
    selectedFiles(const platform::SelectedFileTarget& target,
                  const std::function<bool()>& cancelled) const override {
        require(QThread::currentThread() != qApp->thread() && target.window == 11 &&
                    target.view == 22 && target.tab == 33 && target.processId == 44 &&
                    target.desktop,
                "enumerate the captured selection on a worker without recapturing focus");
        ++calls;
        if (onRead)
            onRead(cancelled);
        return result;
    }
};

struct PendingExport {
    std::shared_ptr<ScreenshotExportArtifact> artifact;
    QPointer<QObject> receiver;
    QPointer<QWidget> owner;
    CollageWindowServices::Completion complete;
};
struct PendingConfirmation {
    bool clear = false;
    QPointer<QWidget> owner;
    std::function<void(bool)> complete;
};
struct Ports {
    QVector<PendingExport> copies;
    QVector<PendingExport> saves;
    QVector<PendingConfirmation> confirmations;
    QHash<QString, QImage> history;
    QStringList reads;

    CollageWindowServices services() {
        CollageWindowServices result;
        result.copy = [this](auto artifact, QObject* receiver, QWidget* owner, auto complete) {
            copies.append({std::move(artifact), receiver, owner, std::move(complete)});
        };
        result.save = [this](auto artifact, QObject* receiver, QWidget* owner, auto complete) {
            saves.append({std::move(artifact), receiver, owner, std::move(complete)});
        };
        result.confirm = [this](QWidget* owner, bool clear, auto complete) {
            confirmations.append({clear, owner, std::move(complete)});
        };
        result.readHistory =
            [this](const storage::CaptureHistoryRecord& record) -> std::optional<QImage> {
            reads.append(record.id);
            const auto found = history.constFind(record.id);
            return found == history.cend() ? std::nullopt : std::optional<QImage>(*found);
        };
        return result;
    }
};

storage::CaptureHistoryRecord record(QString id, QSize size = {3, 2}) {
    storage::CaptureHistoryRecord result;
    result.id = std::move(id);
    result.createdUtc = QDateTime::currentDateTimeUtc();
    result.result = storage::CaptureHistoryResultRecord{size, 100};
    return result;
}

void selectedFilesOpenAndAppendInOrder() {
    resetSettings();
    QTemporaryDir directory;
    require(directory.isValid(), "create selected-image fixtures");
    const QString red = directory.filePath(QStringLiteral("first.BMP"));
    const QString blue = directory.filePath(QStringLiteral("second.png"));
    const QString bad = directory.filePath(QStringLiteral("broken.png"));
    const QString text = directory.filePath(QStringLiteral("notes.txt"));
    const QString folder = directory.filePath(QStringLiteral("folder.png"));
    require(image({3, 2}, Qt::red).save(red, "BMP") && image({2, 3}, Qt::blue).save(blue, "PNG") &&
                QDir().mkdir(folder),
            "write selected image and directory fixtures");
    for (const auto& path : {bad, text}) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write("not an image") > 0,
                "write selected non-image and corrupt image fixtures");
    }
    Ports ports;
    ports.history.insert(QStringLiteral("queued"), image({4, 2}, Qt::green));
    CollageWindowCoordinator coordinator(nullptr, ports.services());
    auto backend = std::make_shared<SelectedFilesTestBackend>();
    backend->result.paths = {blue, text, folder, red, red, bad};
    backend->onCapture = [&] {
        require(!coordinator.window(), "capture the source before creating the stitching window");
    };
    const auto target = backend->captureTarget();
    const auto started = std::make_shared<std::atomic_bool>(false);
    const auto release = std::make_shared<std::atomic_bool>(false);
    backend->onRead = [started, release](const auto&) {
        started->store(true);
        while (!release->load())
            QThread::msleep(1);
    };
    coordinator.presentSelectedFiles(backend, target);
    auto* window = coordinator.window();
    require(window && window->modal()->isOpen() && window->loading(),
            "stitching opens before the selected files finish loading");
    window->document()->append(image({4, 4}, Qt::yellow), QStringLiteral("existing"));
    int completions = 0;
    QStringList failures;
    QObject::connect(window, &CollageWindow::importFinished, &coordinator, [&](const auto& errors) {
        ++completions;
        failures = errors;
    });
    waitUntil([&] { return started->load(); }, "selection enumeration starts in the background");
    coordinator.present({record(QStringLiteral("queued"), {4, 2})});
    coordinator.presentSelectedFiles(backend, target);
    require(coordinator.window() == window && !button(*window, "collageCopy")->isEnabled() &&
                !button(*window, "collageSave")->isEnabled(),
            "repeated selection requests preserve the window and disable exports while loading");
    release->store(true);
    waitUntil([&] { return !window->loading(); }, "queued selected-image imports complete");
    const auto& items = window->document()->items();
    require(items.size() == 6 && items.at(0).image.pixelColor(0, 0) == QColor(Qt::yellow) &&
                items.at(1).image.pixelColor(0, 0) == QColor(Qt::blue) &&
                items.at(2).image.pixelColor(0, 0) == QColor(Qt::red) &&
                items.at(3).image.pixelColor(0, 0) == QColor(Qt::green) &&
                items.at(4).image.pixelColor(0, 0) == QColor(Qt::blue) &&
                items.at(5).image.pixelColor(0, 0) == QColor(Qt::red) && backend->calls == 2 &&
                completions == 1 && failures.size() == 2,
            "selections append in request order, filter non-images and duplicates, and report "
            "corrupt images without losing valid files");
    coordinator.shutdown();
    flushEvents();
}

void emptySelectionsAndFailuresKeepWindowOpen() {
    resetSettings();
    QTemporaryDir directory;
    require(directory.isValid(), "create empty-selection fixtures");
    const auto text = directory.filePath(QStringLiteral("notes.txt"));
    QFile file(text);
    require(file.open(QIODevice::WriteOnly) && file.write("plain text") > 0,
            "write a non-image selection fixture");
    file.close();
    for (const QStringList& paths : {QStringList{}, QStringList{text, directory.path()}}) {
        CollageWindowCoordinator coordinator;
        auto backend = std::make_shared<SelectedFilesTestBackend>();
        backend->result.paths = paths;
        coordinator.presentSelectedFiles(backend, backend->captureTarget());
        auto* window = coordinator.window();
        require(window && window->modal()->isOpen(), "empty selections still open stitching");
        waitUntil([&] { return !window->loading(); }, "empty selection lookup completes");
        require(window->document()->items().isEmpty() && !statusMessage(*window),
                "empty and non-image-only selections are silent and leave an empty window");
        window->document()->append(image(), QStringLiteral("keep existing"));
        coordinator.presentSelectedFiles(backend, backend->captureTarget());
        waitUntil([&] { return !window->loading(); }, "empty append lookup completes");
        require(window->document()->items().size() == 1 && coordinator.window() == window,
                "an empty selection preserves the existing composition");
        coordinator.shutdown();
        flushEvents();
    }
    for (const auto error :
         {platform::SelectedFileError::PermissionDenied, platform::SelectedFileError::Timeout,
          platform::SelectedFileError::Unavailable, platform::SelectedFileError::QueryFailed}) {
        CollageWindowCoordinator coordinator;
        auto backend = std::make_shared<SelectedFilesTestBackend>();
        backend->result.error = error;
        coordinator.presentSelectedFiles(backend, backend->captureTarget());
        auto* window = coordinator.window();
        waitUntil([&] { return !window->loading(); }, "selection query failure completes");
        require(window->modal()->isOpen() && window->document()->items().isEmpty() &&
                    !messageError(*window).isEmpty(),
                "selection query failures show an in-window message and keep stitching open");
        if (error == platform::SelectedFileError::QueryFailed) {
            auto& languages = LanguageManager::instance();
            for (const auto* locale : {"zh_CN", "zh_TW"}) {
                require(languages.setLanguage(QString::fromLatin1(locale)),
                        "switch the selected-file error language");
                flushEvents();
                const auto translated = QCoreApplication::translate(
                    "CollageWindow", "Could not read the selected image files. Please try again");
                require(messageError(*window).contains(translated) &&
                            translated !=
                                QStringLiteral(
                                    "Could not read the selected image files. Please try again"),
                        "cached selection errors retranslate in both Chinese catalogs");
            }
            require(languages.setLanguage(QStringLiteral("en_US")), "restore collage language");
        }
        coordinator.shutdown();
        flushEvents();
    }
}

void selectedFileQueriesCancelWithWindowLifecycle() {
    enum class Stop { Clear, Close, Shutdown };
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("selected.png"));
    require(directory.isValid() && image().save(path), "write canceled selection fixture");
    for (const auto stop : {Stop::Clear, Stop::Close, Stop::Shutdown}) {
        resetSettings();
        Ports ports;
        ports.history.insert(QStringLiteral("fresh"), image({2, 3}, Qt::green));
        CollageWindowCoordinator coordinator(nullptr, ports.services());
        auto backend = std::make_shared<SelectedFilesTestBackend>();
        backend->result.paths = {path};
        const auto started = std::make_shared<std::atomic_bool>(false);
        const auto release = std::make_shared<std::atomic_bool>(false);
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        backend->onRead = [started, release, cancelled](const auto& token) {
            started->store(true);
            while (!release->load())
                QThread::msleep(1);
            cancelled->store(token());
        };
        const auto target = backend->captureTarget();
        coordinator.presentSelectedFiles(backend, target);
        auto* original = coordinator.window();
        original->document()->append(image(), QStringLiteral("existing"));
        waitUntil([&] { return started->load(); }, "the cancellable selection query starts");
        coordinator.presentSelectedFiles(backend, target);
        if (stop == Stop::Clear) {
            original->requestClear();
            require(ports.confirmations.size() == 1, "clear confirms discarding existing images");
            ports.confirmations.first().complete(true);
            require(!original->loading() && original->document()->items().isEmpty(),
                    "clear cancels the selection queue immediately");
        } else if (stop == Stop::Close) {
            original->requestClose();
            require(ports.confirmations.size() == 1, "close confirms discarding existing images");
            ports.confirmations.first().complete(true);
        } else {
            coordinator.shutdown();
        }
        coordinator.present({record(QStringLiteral("fresh"), {2, 3})});
        auto* receiving = coordinator.window();
        if (stop == Stop::Shutdown) {
            coordinator.presentSelectedFiles(backend, target);
            require(!receiving && !coordinator.window(), "shutdown rejects selected-file requests");
        } else {
            require(receiving &&
                        (stop == Stop::Clear ? receiving == original : receiving != original),
                    "fresh requests use the cleared or newly created stitching window");
        }
        release->store(true);
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "canceled selection jobs drain");
        flushEvents();
        require(cancelled->load() && backend->calls == 1,
                "lifecycle cancellation reaches the backend and discards queued queries");
        if (receiving) {
            require(!receiving->loading() && receiving->document()->items().size() == 1 &&
                        receiving->document()->items().first().image.pixelColor(0, 0) ==
                            QColor(Qt::green),
                    "stale selections cannot repopulate a cleared or newly opened composition");
        }
        coordinator.shutdown();
        flushEvents();
    }
}

void singletonImportsAppendMixedSourcesInRequestOrder() {
    resetSettings();
    auto& repository = storage::ApplicationStorage::instance().pinnedWindows();
    storage::PinnedWindowRecord pin;
    pin.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pin.image = image({4, 3}, Qt::cyan);
    pin.nativeGeometry = QRect(QPoint(), pin.image.size());
    pin.canvasSourceRect = QRectF(pin.nativeGeometry);
    pin.contentCanvasRect = pin.canvasSourceRect;
    pin.surfaceCanvasRect = pin.canvasSourceRect;
    pin.initialWindowSize = pin.image.size();
    require(repository.upsert(pin).success && repository.flush().success,
            "persist a pin for singleton mixed-source imports");

    Ports ports;
    const auto started = std::make_shared<std::atomic_bool>(false);
    const auto release = std::make_shared<std::atomic_bool>(false);
    const auto reads = std::make_shared<std::atomic_int>(0);
    auto services = ports.services();
    services.readHistory = [started, release,
                            reads](const auto& requested) -> std::optional<QImage> {
        const int count = reads->fetch_add(1);
        if (count == 0) {
            started->store(true, std::memory_order_release);
            while (!release->load(std::memory_order_acquire))
                QThread::msleep(1);
        }
        if (requested.id == QStringLiteral("missing"))
            return std::nullopt;
        return image();
    };
    CollageWindowCoordinator coordinator(nullptr, std::move(services));
    require(coordinator.window() == nullptr, "the singleton collage is created lazily");
    coordinator.present({record(QStringLiteral("history"))});
    auto* window = coordinator.window();
    require(window && window->modal()->isOpen(), "the first request opens the singleton collage");
    QWidget* surface = window->editor()->window();
    auto* document = window->document();
    int completions = 0;
    QStringList failures;
    QObject::connect(window, &CollageWindow::importFinished, &coordinator, [&](const auto& errors) {
        ++completions;
        failures += errors;
    });
    waitUntil([&] { return started->load(std::memory_order_acquire); },
              "the first singleton import reaches its history reader");
    const QImage blue = image({2, 4}, Qt::blue);
    const ScreenshotPinnedImageSnapshot live{
        ScreenshotExportSource::fromImage(blue), blue.size(), QStringLiteral("live"), {}, {}, {}};
    coordinator.presentPinnedWindows({live});
    coordinator.presentPinnedRecords({pin.id});
    coordinator.present({record(QStringLiteral("missing")), record(QStringLiteral("history"))});
    coordinator.presentPinnedWindows({live});
    require(coordinator.window() == window && window->document() == document &&
                window->editor()->window() == surface && window->loading() &&
                document->items().isEmpty() &&
                coordinator.findChildren<CollageWindow*>().size() == 1,
            "requests during loading share one document and one native surface");
    release->store(true, std::memory_order_release);
    waitUntil([&] { return !window->loading(); }, "queued singleton imports complete");
    const auto& items = document->items();
    require(items.size() == 5 && items[0].image.pixelColor(0, 0) == QColor(Qt::red) &&
                items[1].image == blue && items[2].image == pin.image &&
                items[3].image.pixelColor(0, 0) == QColor(Qt::red) && items[4].image == blue &&
                items[0].id != items[3].id && items[1].id != items[4].id && reads->load() == 3,
            "mixed requests append in submission order and retain independent duplicate images");
    require(completions == 1 && failures.size() == 1 &&
                failures.first().contains(QStringLiteral("missing")) &&
                messageError(*window).contains(QStringLiteral("missing")),
            "one missing image does not discard queued successful imports");
    coordinator.shutdown();
    require(repository.remove(pin.id).success, "remove the singleton stored-pin fixture");
    flushEvents();
}

void singletonReusePreservesEditingAndWindowPresentation() {
    resetSettings();
    Ports ports;
    ports.history.insert(QStringLiteral("additional"), image({2, 3}, Qt::green));
    CollageWindowCoordinator coordinator(nullptr, ports.services());
    coordinator.present();
    auto* window = coordinator.window();
    auto* document = window->document();
    QWidget* surface = window->editor()->window();
    const quint64 first = document->append(image({40, 20}), QStringLiteral("first"));
    const quint64 second = document->append(image({30, 60}, Qt::blue), QStringLiteral("second"));
    require(first && second && document->rotate(first, true) && document->flip(second, true) &&
                document->swapItems(first, second),
            "prepare existing singleton image edits");
    document->select(first);
    auto settings = document->settings();
    settings.direction = CollageDirection::Horizontal;
    settings.sizing = CollageSizing::Original;
    settings.border = 7;
    settings.spacing = 9;
    settings.background = QColor(10, 20, 30, 128);
    document->setSettings(settings);
    const auto edited = document->items();
    surface->resize(surface->size() + QSize(70, 40));
    surface->move(surface->pos() + QPoint(11, 17));
    flushEvents();
    const QRect geometry = surface->geometry();
    coordinator.present({record(QStringLiteral("additional"), {2, 3})});
    waitUntil([&] { return !window->loading(); }, "append to the edited singleton completes");
    require(coordinator.window() == window && window->editor()->window() == surface &&
                surface->geometry() == geometry && document->settings() == settings &&
                document->selectedId() == first && document->items().size() == 3,
            "reusing the singleton retains its geometry, settings and image selection");
    for (qsizetype index = 0; index < edited.size(); ++index)
        require(document->items()[index].id == edited[index].id &&
                    document->items()[index].image == edited[index].image &&
                    document->items()[index].transform == edited[index].transform,
                "appending preserves existing image order, pixels and transforms");
    surface->showMaximized();
    flushEvents();
    require(surface->isMaximized(), "the singleton surface can be maximized");
    coordinator.present({record(QStringLiteral("additional"), {2, 3})});
    waitUntil([&] { return !window->loading(); }, "append to a maximized singleton completes");
    require(surface->isMaximized(), "accepted appends preserve maximized state");
    surface->showMinimized();
    flushEvents();
    require(surface->isMinimized(), "the singleton surface can be minimized");
    coordinator.present({record(QStringLiteral("additional"), {2, 3})});
    waitUntil([&] { return !window->loading(); }, "append restores the minimized singleton");
    require(!surface->isMinimized() && surface->isMaximized() && document->items().size() == 5 &&
                document->settings() == settings && document->selectedId() == first,
            "accepted requests restore a minimized singleton while retaining its maximized state");
    coordinator.shutdown();
    flushEvents();
}

void singletonCloseClearsOwnershipBeforeDeferredDeletion() {
    resetSettings();
    Ports ports;
    ports.history.insert(QStringLiteral("after-cancel"), image({2, 3}, Qt::blue));
    ports.history.insert(QStringLiteral("fresh"), image({4, 2}, Qt::green));
    CollageWindowCoordinator coordinator(nullptr, ports.services());
    coordinator.present();
    auto* original = coordinator.window();
    original->document()->append(image(), QStringLiteral("original"));
    original->requestClose();
    require(ports.confirmations.size() == 1, "closing the singleton asks for discard confirmation");
    ports.confirmations.first().complete(false);
    coordinator.present({record(QStringLiteral("after-cancel"), {2, 3})});
    waitUntil([&] { return !original->loading(); }, "a canceled close retains the singleton");
    require(coordinator.window() == original && original->document()->items().size() == 2,
            "canceling discard keeps the same collage available for subsequent operations");
    QPointer<CollageWindow> retiring(original);
    original->requestClose();
    require(ports.confirmations.size() == 2, "a later close asks again for discard confirmation");
    ports.confirmations.last().complete(true);
    require(coordinator.window() == nullptr && retiring && !retiring->modal()->isOpen(),
            "confirmed close clears singleton ownership before the retired object is deleted");
    coordinator.present({record(QStringLiteral("fresh"), {4, 2})});
    auto* reopened = coordinator.window();
    require(reopened && reopened != original && retiring && reopened->document()->items().isEmpty(),
            "an immediate request creates a fresh collage before deferred deletion runs");
    flushEvents();
    require(!retiring,
            "deferred deletion retires the old collage without clearing its replacement");
    waitUntil([&] { return !reopened->loading(); },
              "the replacement collage imports its own image");
    require(coordinator.window() == reopened && reopened->document()->items().size() == 1 &&
                reopened->document()->items().first().image.pixelColor(0, 0) == QColor(Qt::green),
            "reopened collages do not retain discarded images");
    coordinator.shutdown();
    flushEvents();
}

void singletonCancellationDiscardsQueuedAndStaleImports() {
    enum class Stop { Clear, Close, Shutdown };
    for (const auto stop : {Stop::Clear, Stop::Close, Stop::Shutdown}) {
        resetSettings();
        Ports ports;
        const auto started = std::make_shared<std::atomic_bool>(false);
        const auto release = std::make_shared<std::atomic_bool>(false);
        const auto queuedReads = std::make_shared<std::atomic_int>(0);
        const auto queuedRenders = std::make_shared<std::atomic_int>(0);
        auto services = ports.services();
        services.readHistory = [started, release,
                                queuedReads](const auto& requested) -> std::optional<QImage> {
            if (requested.id == QStringLiteral("slow")) {
                started->store(true, std::memory_order_release);
                while (!release->load(std::memory_order_acquire))
                    QThread::msleep(1);
                return image();
            }
            if (requested.id == QStringLiteral("queued"))
                ++*queuedReads;
            return image({2, 3}, Qt::green);
        };
        CollageWindowCoordinator coordinator(nullptr, std::move(services));
        coordinator.present();
        auto* original = coordinator.window();
        original->document()->append(image({4, 2}, Qt::yellow), QStringLiteral("existing"));
        int completions = 0;
        QObject::connect(original, &CollageWindow::importFinished, &coordinator,
                         [&](const auto&) { ++completions; });
        coordinator.present({record(QStringLiteral("slow"))});
        waitUntil([&] { return started->load(std::memory_order_acquire); },
                  "the cancellable singleton import reaches its reader");
        coordinator.present({record(QStringLiteral("queued"), {2, 3})});
        const ScreenshotPinnedImageSnapshot queued{
            ScreenshotExportSource::fromProducer([queuedRenders](const auto&) {
                ++*queuedRenders;
                return image();
            }),
            {3, 2},
            QStringLiteral("queued-live"),
            {},
            {},
            {}};
        coordinator.presentPinnedWindows({queued});
        coordinator.presentPinnedRecords({QStringLiteral("queued-missing-pin")});

        if (stop == Stop::Clear) {
            original->requestClear();
            require(ports.confirmations.size() == 1 && ports.confirmations.first().clear,
                    "clear uses its confirmation while singleton imports are queued");
            ports.confirmations.first().complete(true);
            require(coordinator.window() == original && original->document()->items().isEmpty() &&
                        !original->loading(),
                    "clear cancels every queued import while retaining the singleton");
        } else if (stop == Stop::Close) {
            original->requestClose();
            require(ports.confirmations.size() == 1 && !ports.confirmations.first().clear,
                    "close uses its discard confirmation while singleton imports are queued");
            ports.confirmations.first().complete(true);
            require(coordinator.window() == nullptr,
                    "closed singleton imports release ownership immediately");
        } else {
            coordinator.shutdown();
            require(coordinator.window() == nullptr,
                    "shutdown closes and releases an importing singleton");
        }

        CollageWindow* receiving = nullptr;
        if (stop != Stop::Shutdown) {
            coordinator.present({record(QStringLiteral("fresh"), {2, 3})});
            receiving = coordinator.window();
            require(receiving &&
                        (stop == Stop::Clear ? receiving == original : receiving != original),
                    "fresh imports use the cleared or newly opened collage as appropriate");
        } else {
            coordinator.present({record(QStringLiteral("fresh"), {2, 3})});
            coordinator.presentPinnedWindows({queued});
            coordinator.presentPinnedRecords({QStringLiteral("ignored-after-shutdown")});
            require(coordinator.window() == nullptr,
                    "shutdown permanently rejects requests from every collage entry point");
        }
        release->store(true, std::memory_order_release);
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "canceled singleton imports drain their worker jobs");
        flushEvents();
        require(queuedReads->load() == 0 && queuedRenders->load() == 0,
                "canceled queued sources are never decoded or rendered");
        if (receiving) {
            waitUntil([&] { return !receiving->loading(); }, "the fresh singleton batch completes");
            require(coordinator.window() == receiving &&
                        receiving->document()->items().size() == 1 &&
                        receiving->document()->items().first().image.pixelColor(0, 0) ==
                            QColor(Qt::green) &&
                        completions == (stop == Stop::Clear ? 1 : 0),
                    "stale callbacks cannot append discarded images or finish the canceled batch");
            coordinator.shutdown();
        } else {
            require(completions == 0, "shutdown suppresses canceled import completions");
        }
        flushEvents();
    }
}

void singletonExportsIgnoreNewOperationsWithoutPresenting() {
    for (const bool copy : {true, false}) {
        resetSettings();
        Ports ports;
        const auto reads = std::make_shared<std::atomic_int>(0);
        const auto renders = std::make_shared<std::atomic_int>(0);
        auto services = ports.services();
        services.readHistory = [reads](const auto&) -> std::optional<QImage> {
            ++*reads;
            return image({2, 3}, Qt::green);
        };
        CollageWindowCoordinator coordinator(nullptr, std::move(services));
        coordinator.present();
        auto* window = coordinator.window();
        QWidget* surface = window->editor()->window();
        const quint64 existing = window->document()->append(image(), QStringLiteral("existing"));
        surface->move(surface->pos() + QPoint(13, 19));
        surface->showMinimized();
        flushEvents();
        require(surface->isMinimized(), "minimize the singleton before export rejection checks");
        const QRect geometry = surface->geometry();
        const auto state = surface->windowState();
        auto& exports = copy ? ports.copies : ports.saves;
        if (copy)
            window->copy();
        else
            window->save();
        require(window->busy() && exports.isEmpty(),
                "copy and save begin in asynchronous preparation before publication");
        const ScreenshotPinnedImageSnapshot ignored{
            ScreenshotExportSource::fromProducer([renders](const auto&) {
                ++*renders;
                return image();
            }),
            {3, 2},
            QStringLiteral("ignored-live"),
            {},
            {},
            {}};
        const auto requestWhileBusy = [&] {
            const auto selection = std::make_shared<SelectedFilesTestBackend>();
            coordinator.presentSelectedFiles(selection, selection->captureTarget());
            window->addSelectedFiles(selection, selection->captureTarget());
            coordinator.present({record(QStringLiteral("ignored"))});
            coordinator.presentPinnedWindows({ignored});
            coordinator.presentPinnedRecords({QStringLiteral("ignored-pin")});
            window->present({record(QStringLiteral("ignored-direct"))});
            window->presentPinnedWindows({ignored});
            window->presentPinnedRecords({QStringLiteral("ignored-direct-pin")});
            require(coordinator.window() == window && window->busy() && !window->loading() &&
                        window->document()->items().size() == 1 &&
                        window->document()->items().first().id == existing && reads->load() == 0 &&
                        renders->load() == 0 && selection->calls == 0 &&
                        surface->windowState() == state && surface->geometry() == geometry &&
                        surface->isMinimized(),
                    "busy export requests preserve images, geometry and minimized presentation");
        };
        requestWhileBusy();
        waitUntil([&] { return exports.size() == 1; }, "singleton export reaches publication");
        requestWhileBusy();
        int notifications = 0;
        const auto connection =
            QObject::connect(window, &CollageWindow::exportFinished, &coordinator, [&](bool) {
                ++notifications;
                requestWhileBusy();
            });
        exports.first().complete(false, {});
        QObject::disconnect(connection);
        require(notifications == 1 && !window->busy() && coordinator.window() == window,
                "completion observers remain busy until canceled publication finishes notifying");
        coordinator.present({record(QStringLiteral("fresh"), {2, 3})});
        waitUntil([&] { return !window->loading(); }, "imports resume after canceled export");
        require(window->document()->items().size() == 2 && reads->load() == 1 &&
                    renders->load() == 0 && !surface->isMinimized(),
                "ignored operations are not replayed when the export finishes");
        if (copy)
            window->copy();
        else
            window->save();
        waitUntil([&] { return exports.size() == 2; }, "a successful retry reaches publication");
        QPointer<CollageWindow> retiring(window);
        exports.last().complete(true, {});
        require(coordinator.window() == nullptr && retiring,
                "successful export clears singleton ownership before deferred deletion");
        coordinator.present();
        require(coordinator.window() && coordinator.window() != window &&
                    coordinator.window()->document()->items().isEmpty(),
                "a request immediately after successful export opens a fresh collage");
        coordinator.shutdown();
        flushEvents();
    }
}

void singletonWindowScenarios() {
    timed("Singleton mixed-source queued imports",
          singletonImportsAppendMixedSourcesInRequestOrder);
    timed("Singleton editing and presentation",
          singletonReusePreservesEditingAndWindowPresentation);
    timed("Singleton close and immediate reopen",
          singletonCloseClearsOwnershipBeforeDeferredDeletion);
    timed("Singleton queued import cancellation",
          singletonCancellationDiscardsQueuedAndStaleImports);
    timed("Singleton busy export rejection", singletonExportsIgnoreNewOperationsWithoutPresenting);
}

void windowPinStartsEnabledAndPreservesEdits() {
    CollageWindow window;
    window.present();
    window.document()->append(image(), QStringLiteral("pin-test"));
    flushEvents();
    auto* surface = window.editor()->window();
    auto* pin = surface->findChild<QToolButton*>(QStringLiteral("ad-modal-always-on-top"));
    require(pin && pin->isVisible() && pin->isChecked() && window.modal()->windowAlwaysOnTop() &&
                surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint),
            "stitching opens pinned with a checked title-bar pin button");
    const auto pinPosition = pin->mapTo(surface, QPoint());
    require(pinPosition.x() > surface->width() / 2 &&
                pinPosition.y() < window.editor()->mapTo(surface, QPoint()).y(),
            "stitching pin is in the top-right window header");
    const auto geometry = surface->geometry();
    const auto revision = window.document()->contentRevision();
    for (const bool pinned : {false, true}) {
        pin->click();
        window.present();
        flushEvents();
        require(pin->isChecked() == pinned && window.modal()->windowAlwaysOnTop() == pinned &&
                    surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint) == pinned,
                "stitching pin toggles always-on-top and survives repeated presentation");
        require(window.modal()->isOpen() && surface->isVisible() &&
                    window.editor()->window() == surface && surface->geometry() == geometry &&
                    window.document()->contentRevision() == revision,
                "toggling stitching pin preserves the visible window, geometry, and edits");
    }
    window.shutdown();
}

void windowOpensCenteredOnCurrentScreen() {
    resetSettings();
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a screen is available for collage placement");
    Ports ports;
    CollageWindow window(nullptr, ports.services());
    window.present();
    flushEvents();
    const QRect available = screen->availableGeometry();
    const QRect geometry = window.editor()->window()->geometry();
    require(window.modal()->windowScreen() == screen && available.contains(geometry) &&
                (geometry.center() - available.center()).manhattanLength() <= 2,
            "collage opens centered within the current screen's available area");
    window.requestClose();
}

void independentWindowsAndClosePolicy() {
    resetSettings();
    QWidget mainWindow;
    mainWindow.show();
    QObject lifetime(&mainWindow);
    Ports firstPorts, secondPorts;
    CollageWindow first(&lifetime, firstPorts.services());
    CollageWindow second(&lifetime, secondPorts.services());
    first.present();
    second.present();
    flushEvents();
    require(first.document()->settings() == CollageSettings{} &&
                first.document() != second.document(),
            "first-use defaults and editing documents are independent");
    QWidget* firstSurface = first.editor()->window();
    QWidget* secondSurface = second.editor()->window();
    require(firstSurface != secondSurface && firstSurface->parentWidget() == nullptr &&
                secondSurface->parentWidget() == nullptr &&
                first.modal()->ownerWindow() == nullptr &&
                firstSurface->windowModality() == Qt::NonModal &&
                secondSurface->windowModality() == Qt::NonModal,
            "each collage has its own unowned nonmodal surface");
    require(first.modal()->windowTaskbarVisible() && first.modal()->windowResizable() &&
                first.modal()->windowMinimizeButtonVisible() &&
                first.modal()->windowMaximizeButtonVisible() &&
                !first.modal()->windowIcon().isNull() &&
                (firstSurface->windowFlags() & Qt::WindowType_Mask) == Qt::Window &&
                firstSurface->findChild<QToolButton*>(QStringLiteral("ad-modal-maximize")),
            "collage windows expose native window-management capabilities and an icon");
    mainWindow.hide();
    require(first.modal()->isOpen() && second.modal()->isOpen() && closeCaptureInputBlockers(),
            "main-window hiding and capture modal cleanup preserve independent collages");
    require(QApplication::activeModalWidget() == nullptr,
            "collages do not block the main application or screenshot overlays");
    require(!button(first, "collageCopy")->isEnabled() &&
                !button(first, "collageSave")->isEnabled(),
            "empty windows disable exports");
    first.copy();
    first.save();
    require(!first.busy() && firstPorts.copies.isEmpty() && firstPorts.saves.isEmpty(),
            "empty collections cannot submit exports through public actions");
    const quint64 id = second.document()->append(image(), QStringLiteral("second"));
    require(id != 0 && first.document()->items().isEmpty(),
            "one collage's imports do not populate another");
    firstSurface->close();
    require(!first.modal()->isOpen() && firstPorts.confirmations.isEmpty() &&
                second.modal()->isOpen(),
            "an empty native close closes only that window without confirmation");
    secondSurface->close();
    second.requestClose();
    require(secondPorts.confirmations.size() == 1 && !secondPorts.confirmations.first().clear &&
                secondPorts.confirmations.first().owner == secondSurface &&
                second.modal()->isOpen(),
            "a nonempty native close asks once for confirmation scoped to its own window");
    secondPorts.confirmations.first().complete(false);
    require(second.modal()->isOpen() && second.document()->items().size() == 1,
            "canceling discard preserves images and the window");
    button(second, "collageCancel")->click();
    require(secondPorts.confirmations.size() == 2, "Cancel uses the same discard confirmation");
    secondPorts.confirmations.last().complete(true);
    require(!second.modal()->isOpen(), "confirmed discard closes the nonempty collage");
}

void outputDescriptionsInFooter() {
    resetSettings();
    Ports ports;
    CollageWindow window(nullptr, ports.services());
    window.present();
    flushEvents();
    auto* footer = window.modal()->footerWidget();
    require(footer != nullptr, "collage footer is available");
    auto* dimensions = footer->findChild<QLabel*>(QStringLiteral("collageOutputDimensions"));
    auto* total = footer->findChild<QLabel*>(QStringLiteral("collageImageTotal"));
    require(dimensions && total &&
                !window.editor()->findChild<QLabel*>(QStringLiteral("collageOutputDimensions")) &&
                !window.editor()->findChild<QLabel*>(QStringLiteral("collageImageTotal")),
            "size and image count belong to the footer rather than the preview or thumbnails");
    require(dimensions->isHidden() && total->text() == QStringLiteral("Total: 0 images"),
            "an empty collage shows its count without an invalid size");
    const auto first = window.document()->append(image({300, 200}), QStringLiteral("first"));
    window.document()->append(image({300, 100}), QStringLiteral("second"));
    flushEvents();
    require(dimensions->isVisible() && dimensions->text() == QStringLiteral("300 × 300 px") &&
                total->text() == QStringLiteral("Total: 2 images"),
            "footer descriptions update when images are added");
    const auto checkLayout = [&] {
        flushEvents();
        const QRect sizeRect(dimensions->mapTo(footer, QPoint()), dimensions->size());
        const QRect countRect(total->mapTo(footer, QPoint()), total->size());
        auto* cancel = button(window, "collageCancel");
        const QRect cancelRect(cancel->mapTo(footer, QPoint()), cancel->size());
        require(footer->rect().contains(sizeRect) && footer->rect().contains(countRect) &&
                    sizeRect.left() == footer->contentsRect().left() &&
                    sizeRect.right() < countRect.left() && countRect.right() < cancelRect.left() &&
                    std::abs(sizeRect.center().y() - cancelRect.center().y()) <= 1 &&
                    std::abs(countRect.center().y() - cancelRect.center().y()) <= 1 &&
                    dimensions->width() >= dimensions->sizeHint().width() &&
                    total->width() >= total->sizeHint().width(),
                "footer shows unclipped size then count on the left, aligned with its actions");
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(window.editor());
        require(dimensions->font().pixelSize() == total->font().pixelSize() &&
                    dimensions->palette().color(QPalette::WindowText) == theme.colorTextSecondary &&
                    total->palette().color(QPalette::WindowText) == theme.colorTextSecondary,
                "both footer descriptions share the secondary text styling");
    };
    auto& themeManager = styles::ThemeManager::instance();
    const auto previousMode = themeManager.themeMode();
    for (const auto mode : {styles::ThemeMode::Light, styles::ThemeMode::Dark}) {
        themeManager.setThemeMode(mode);
        for (const auto size : {QSize(900, 600), QSize(1200, 800)}) {
            window.editor()->window()->resize(size);
            checkLayout();
        }
    }
    themeManager.setThemeMode(previousMode);
    for (const auto* catalog : {":/i18n/snow_shot_zh_CN.qm", ":/i18n/snow_shot_zh_TW.qm"}) {
        QTranslator translator;
        require(translator.load(QString::fromLatin1(catalog)), "load collage translation catalog");
        QApplication::installTranslator(&translator);
        flushEvents();
        require(total->text() ==
                    QCoreApplication::translate("CollageEditorWidget", "Total: %1 images").arg(2),
                "footer image count retranslates after moving outside the editor");
        checkLayout();
        QApplication::removeTranslator(&translator);
        flushEvents();
    }
    auto settings = window.document()->settings();
    settings.border = 8;
    settings.spacing = 6;
    window.document()->setSettings(settings);
    require(dimensions->text() == QStringLiteral("316 × 322 px"),
            "footer size tracks layout settings");
    require(window.document()->remove(first) && total->text() == QStringLiteral("Total: 1 images"),
            "footer count tracks image removal");
    window.document()->clear();
    flushEvents();
    require(dimensions->isHidden() && total->text() == QStringLiteral("Total: 0 images") &&
                total->mapTo(footer, QPoint()).x() == footer->contentsRect().left(),
            "clearing images leaves the count aligned to the footer left edge");
    window.requestClose();
}

void clearAndInitialImportFailures() {
    resetSettings();
    Ports ports;
    ports.history.insert(QStringLiteral("first"), image());
    ports.history.insert(QStringLiteral("last"), image({2, 3}, Qt::blue));
    CollageWindow window(nullptr, ports.services());
    QStringList failures;
    int imports = 0;
    QObject::connect(&window, &CollageWindow::importFinished, &window, [&](const auto& errors) {
        ++imports;
        failures = errors;
    });
    window.present({record(QStringLiteral("first")), record(QStringLiteral("missing")),
                    record(QStringLiteral("oversized"), {std::numeric_limits<int>::max(), 3}),
                    record(QStringLiteral("overflow"),
                           {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()}),
                    record(QStringLiteral("last"), {2, 3})});
    require(window.loading() && !button(window, "collageCopy")->isEnabled(),
            "initial asynchronous loading disables exports without closing the window");
    waitUntil([&] { return imports == 1; }, "partial history import completes");
    require(
        !window.loading() && window.document()->items().size() == 2 && failures.size() == 3 &&
            failures[0].contains(QStringLiteral("missing")) &&
            failures[1].contains(QStringLiteral("oversized")) &&
            failures[2].contains(QStringLiteral("overflow")),
        "batch failures identify unavailable and resource-limited inputs in their original order");
    require(window.document()->items()[0].image.pixelColor(0, 0) == QColor(Qt::red) &&
                window.document()->items()[1].image.size() == QSize(2, 3) &&
                ports.reads == QStringList{QStringLiteral("first"), QStringLiteral("missing"),
                                           QStringLiteral("last")},
            "successful final images retain their order and oversized inputs are rejected before "
            "decoding");
    require(messageError(window).contains(failures[0]) &&
                messageError(window).contains(failures[1]) &&
                messageError(window).contains(failures[2]),
            "partial failures appear as one consolidated report");
    window.requestClear();
    window.requestClear();
    require(ports.confirmations.size() == 1 && ports.confirmations.first().clear,
            "Clear all asks once for a distinct confirmation");
    ports.confirmations.first().complete(false);
    require(window.document()->items().size() == 2, "canceling Clear all preserves the collection");
    window.requestClear();
    ports.confirmations.last().complete(true);
    require(window.document()->items().isEmpty() && window.modal()->isOpen() &&
                !button(window, "collageCopy")->isEnabled() &&
                !button(window, "collageSave")->isEnabled() &&
                button(window, "collageAddImages")->isEnabled(),
            "confirmed clearing leaves a usable empty window with exports disabled");
    window.requestClose();
    require(ports.confirmations.size() == 2 && !window.modal()->isOpen(),
            "a cleared window closes directly");

    Ports failedPorts;
    CollageWindow failed(nullptr, failedPorts.services());
    failed.present({record(QStringLiteral("unavailable"))});
    waitUntil([&] { return !failed.loading(); }, "all-failed initial import completes");
    require(failed.modal()->isOpen() && failed.document()->items().isEmpty() &&
                messageError(failed).contains(QStringLiteral("unavailable")) &&
                button(failed, "collageAddImages")->isEnabled(),
            "all-failed initial imports keep an empty collage available for image addition");
}

void pinnedRecordsImportSavedPixelsAndText(const QString& directory) {
    resetSettings();
    const auto setGeometry = [](storage::PinnedWindowRecord& pin, const QSize& size) {
        pin.nativeGeometry = QRect(QPoint(), size);
        pin.canvasSourceRect = QRectF(pin.nativeGeometry);
        pin.contentCanvasRect = pin.canvasSourceRect;
        pin.surfaceCanvasRect = pin.canvasSourceRect;
        pin.initialWindowSize = size;
    };
    auto& repository = storage::ApplicationStorage::instance().pinnedWindows();
    const auto originalRecords = repository.summaries().size();
    const auto originalHistory =
        storage::ApplicationStorage::instance().captureHistory().records().size();
    storage::PinnedWindowRecord pixels;
    pixels.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pixels.image = image({540, 270}, Qt::cyan);
    setGeometry(pixels, pixels.image.size());
    pixels.canvasSession = QByteArrayLiteral("saved canvas is not a stitching input");
    pixels.recognitionResults = QByteArrayLiteral("saved recognition is not a stitching input");
    require(repository.upsert(pixels).success, "save an image pin for stitching");
    storage::PinnedWindowRecord file;
    file.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file.sourceKind = storage::PinnedWindowSourceKind::ClipboardImageFile;
    file.image = image({420, 300}, Qt::blue);
    setGeometry(file, file.image.size());
    file.originalFilePath = QDir(directory).filePath(QStringLiteral("pinned-original.png"));
    file.originalFileName = QStringLiteral("pinned-original.png");
    require(file.image.save(file.originalFilePath) && repository.upsert(file).success,
            "save a file pin for stitching");
    storage::PinnedWindowRecord text;
    text.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    text.sourceKind = storage::PinnedWindowSourceKind::ClipboardText;
    text.originalHtml = QStringLiteral("<b>Saved text pin</b>");
    text.originalText = QStringLiteral("Saved text pin");
    text.firstCreationTextDpi = 1.5;
    setGeometry(text, QSize(300, 90));
    require(repository.upsert(text).success && repository.flush().success &&
                QFile::remove(file.originalFilePath),
            "persist stitching sources and remove the original file");
    ScreenshotClipboardOriginalContent original;
    original.html = text.originalHtml;
    original.text = text.originalText;
    const auto rendered =
        ScreenshotClipboardContentReader::renderOriginalText(original, text.firstCreationTextDpi);
    require(rendered.has_value(), "render the text pin reference");
    const QImage expectedText = rendered ? rendered->image : QImage{};
    const QString missing = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Ports ports;
    CollageWindow window(nullptr, ports.services());
    QStringList failures;
    int completions = 0;
    QObject::connect(&window, &CollageWindow::importFinished, &window,
                     [&](const QStringList& errors) {
                         failures = errors;
                         ++completions;
                     });
    window.presentPinnedRecords({file.id, missing, pixels.id, text.id});
    require(window.loading() && window.modal()->isOpen(), "pinned import opens the busy editor");
    waitUntil([&] { return completions == 1; }, "pinned stitching import completes");
    const auto& items = window.document()->items();
    require(items.size() == 3 && items[0].image.size() == file.image.size() &&
                items[0].image.pixelColor(0, 0) == QColor(Qt::blue) &&
                items[1].image.size() == pixels.image.size() &&
                items[1].image.pixelColor(0, 0) == QColor(Qt::cyan) &&
                items[2].image == expectedText,
            "saved file, image, and text pins retain full-resolution pixels and input order");
    require(items[1].preview.size() == QSize(256, 128) && ports.reads.isEmpty(),
            "pinned imports use bounded previews independently of screenshot history");
    require(failures.size() == 1 && failures.first().contains(missing) &&
                messageError(window).contains(missing) &&
                button(window, "collageCopy")->isEnabled(),
            "a missing pin reports an import failure while successful images remain usable");
    const auto retained = repository.loadRecord(pixels.id);
    require(repository.summaries().size() == originalRecords + 3 && retained &&
                retained->canvasSession == pixels.canvasSession &&
                storage::ApplicationStorage::instance().captureHistory().records().size() ==
                    originalHistory,
            "stitching preserves pinned records and screenshot history");
    CollageWindow unavailable(nullptr, ports.services());
    unavailable.presentPinnedRecords({missing});
    waitUntil([&] { return !unavailable.loading(); }, "all-missing pinned import completes");
    require(unavailable.modal()->isOpen() && unavailable.document()->items().isEmpty() &&
                messageError(unavailable).contains(missing) &&
                button(unavailable, "collageAddImages")->isEnabled(),
            "all-missing pinned inputs leave the editor available for adding images");
    window.shutdown();
    unavailable.shutdown();
    require(repository.removeMany({pixels.id, file.id, text.id}).success,
            "remove isolated stitching fixtures");
}

void importFeedbackUsesMessagesWithoutLayoutChanges() {
    resetSettings();
    Ports ports, otherPorts;
    const auto started = std::make_shared<std::atomic_int>(0);
    const auto released = std::make_shared<std::atomic_int>(0);
    auto services = ports.services();
    services.readHistory =
        [started, released](const storage::CaptureHistoryRecord& input) -> std::optional<QImage> {
        const int step = started->fetch_add(1, std::memory_order_acq_rel) + 1;
        QElapsedTimer deadline;
        deadline.start();
        while (released->load(std::memory_order_acquire) < step && deadline.elapsed() < 15000)
            QThread::msleep(1);
        return input.id == QStringLiteral("missing") ? std::nullopt
                                                     : std::optional<QImage>(image());
    };
    CollageWindow window(nullptr, std::move(services)), other(nullptr, otherPorts.services());
    window.document()->append(image(), QStringLiteral("existing"));
    window.present();
    other.present();
    flushEvents();
    const QRect canvasGeometry = window.editor()->canvas()->geometry();
    const QRect editorGeometry = window.editor()->geometry();
    const auto checkLayout = [&] {
        flushEvents();
        require(window.editor()->canvas()->geometry() == canvasGeometry &&
                    window.editor()->geometry() == editorGeometry && !hasInlineStatus(window),
                "import feedback preserves the editor and preview geometry");
    };
    window.present({record(QStringLiteral("first")), record(QStringLiteral("missing"))});
    waitUntil([&] { return started->load(std::memory_order_acquire) == 1; },
              "first import waits at its deterministic reader");
    QPointer<AdMessageHandle> loading = statusMessage(window);
    require(loading && loading->type() == AdMessage::Type::Loading &&
                loading->content() == QStringLiteral("Loading images: 1 of 2") &&
                button(window, "collageAddImages")->busy() && !statusMessage(other),
            "image loading uses a Message scoped to the importing window");
    checkLayout();
    released->store(1, std::memory_order_release);
    waitUntil([&] { return started->load(std::memory_order_acquire) == 2; },
              "second import waits at its deterministic reader");
    require(statusMessage(window) == loading &&
                loading->content() == QStringLiteral("Loading images: 2 of 2"),
            "batch progress updates one existing loading Message");
    checkLayout();
    released->store(2, std::memory_order_release);
    waitUntil([&] { return !window.loading(); }, "partial import finishes");
    require(statusMessage(window) == loading && loading->type() == AdMessage::Type::Error &&
                loading->content().contains(QStringLiteral("missing")) &&
                !button(window, "collageAddImages")->busy() &&
                button(window, "collageCopy")->isEnabled() && !statusMessage(other),
            "import failures replace loading with an error Message and restore usable actions");
    checkLayout();

    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("valid.png"));
    require(directory.isValid() && image().save(path), "write a successful file import fixture");
    window.addFiles({path});
    require(statusMessage(window) == loading && loading->type() == AdMessage::Type::Loading,
            "a new import replaces the previous failure Message");
    waitUntil([&] { return !window.loading(); }, "successful file import finishes");
    require(!statusMessage(window) && !button(window, "collageAddImages")->busy(),
            "successful imports close loading feedback and restore the Add images button");
    checkLayout();
    other.requestClose();
    window.requestClose();
    ports.confirmations.last().complete(true);
}

void livePinnedImportsPreserveOrderAndFailures() {
    resetSettings();
    Ports ports;
    CollageWindow window(nullptr, ports.services());
    QImage red = image({540, 270}, Qt::red);
    const QImage frozen = red;
    ScreenshotPinnedImageSnapshot first{
        ScreenshotExportSource::fromImage(red), red.size(), QStringLiteral("first"), {}, {}, {}};
    red.fill(Qt::green);
    const auto oversizeRenders = std::make_shared<std::atomic_int>(0);
    ScreenshotPinnedImageSnapshot oversized{
        ScreenshotExportSource::fromProducer([oversizeRenders](const auto&) {
            ++*oversizeRenders;
            return image();
        }),
        {65536, 32769},
        QStringLiteral("oversized"),
        {},
        {},
        {}};
    ScreenshotPinnedImageSnapshot failed{
        ScreenshotExportSource::fromProducer([](const auto&) { return QImage{}; }),
        {30, 20},
        QStringLiteral("failed"),
        {},
        {},
        {}};
    const QImage blue = image({420, 300}, Qt::blue);
    ScreenshotPinnedImageSnapshot last{
        ScreenshotExportSource::fromImage(blue), blue.size(), QStringLiteral("last"), {}, {}, {}};
    ScreenshotPinnedImageSnapshot rejected{
        {}, {3, 2}, QStringLiteral("admission"), QStringLiteral("Capture admission rejected"),
        {}, {}};
    QStringList failures;
    QObject::connect(&window, &CollageWindow::importFinished, &window,
                     [&](const auto& errors) { failures = errors; });
    window.presentPinnedWindows({first,
                                 {{}, {}, QStringLiteral("missing"), {}, {}, {}},
                                 failed,
                                 oversized,
                                 rejected,
                                 last});
    require(window.loading() && statusMessage(window) &&
                statusMessage(window)->content() == QStringLiteral("Loading images: 1 of 6"),
            "live pinned images use existing import progress");
    waitUntil([&] { return !window.loading(); }, "live pinned image batch completes");
    const auto& items = window.document()->items();
    require(items.size() == 2 && items[0].image == frozen && items[1].image == blue &&
                items[0].label.contains(QStringLiteral("first")) &&
                items[1].label.contains(QStringLiteral("last")) && failures.size() == 4 &&
                failures[0].contains(QStringLiteral("missing")) &&
                failures[1].contains(QStringLiteral("failed")) &&
                failures[2].contains(QStringLiteral("oversized")) &&
                failures[3].contains(QStringLiteral("Capture admission rejected")) &&
                oversizeRenders->load() == 0 && items[0].preview.size() == QSize(256, 128) &&
                button(window, "collageCopy")->isEnabled(),
            "live imports retain owned pixels, selection order and previews while reporting "
            "failures and rejecting oversized renders");
    window.requestClose();
    ports.confirmations.last().complete(true);
}

void livePinnedImportsCancelAndRespectMemoryBudget() {
    const qint64 before = collageResourceUsage().reservedBytes;
    const auto started = std::make_shared<std::atomic_bool>(false);
    const auto release = std::make_shared<std::atomic_bool>(false);
    {
        CollageWindow window;
        ScreenshotPinnedImageSnapshot slow{
            ScreenshotExportSource::fromProducer([started, release](const auto& cancellation) {
                started->store(true, std::memory_order_release);
                while (!release->load(std::memory_order_acquire) &&
                       !cancellation.isCancellationRequested())
                    QThread::msleep(1);
                return cancellation.isCancellationRequested() ? QImage{} : image({512, 256});
            }),
            {512, 256},
            QStringLiteral("slow"),
            {},
            {},
            {}};
        int completions = 0;
        QObject::connect(&window, &CollageWindow::importFinished, &window,
                         [&](const auto&) { ++completions; });
        window.presentPinnedWindows({slow});
        waitUntil([&] { return started->load(std::memory_order_acquire); },
                  "live snapshot render begins behind the cancellation barrier");
        window.shutdown();
        release->store(true, std::memory_order_release);
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "cancelled live snapshot render drains");
        flushEvents();
        require(window.document()->items().isEmpty() && completions == 0 && !window.loading(),
                "closing an importing editor discards stale completion");
    }
    waitUntil([&] { return collageResourceUsage().reservedBytes == before; },
              "cancellation releases the live import reservation");
    CollageWindow window;
    const auto renders = std::make_shared<std::atomic_int>(0);
    ScreenshotPinnedImageSnapshot source{
        ScreenshotExportSource::fromProducer([renders](const auto&) {
            ++*renders;
            return image();
        }),
        {65536, 32769},
        QStringLiteral("budget"),
        {},
        {},
        {}};
    QStringList failures;
    QObject::connect(&window, &CollageWindow::importFinished, &window,
                     [&](const auto& errors) { failures = errors; });
    window.presentPinnedWindows({source});
    waitUntil([&] { return !window.loading(); }, "oversized live import completes");
    require(failures.size() == 1 && window.document()->items().isEmpty() && renders->load() == 0,
            "live import rejects an oversized raster before rendering");
    window.shutdown();
}

void livePinnedQueueReleasesCompletedSnapshotsAndAccountsForCancellation() {
    const qint64 baseline = collageResourceUsage().reservedBytes;
    const auto started = std::make_shared<std::atomic_bool>(false);
    const auto release = std::make_shared<std::atomic_bool>(false);
    const auto unblock = qScopeGuard([&] { release->store(true, std::memory_order_release); });
    {
        CollageWindow window;
        auto owner = std::make_shared<int>(1);
        const std::weak_ptr<int> completedLifetime = owner;
        ScreenshotPinnedImageSnapshot first{
            ScreenshotExportSource::fromProducer([owner](const auto&) {
                Q_UNUSED(owner);
                return image({3, 2});
            }),
            {3, 2},
            QStringLiteral("completed"),
            {},
            {},
            {}};
        owner.reset();
        ScreenshotPinnedImageSnapshot slow{
            ScreenshotExportSource::fromProducer(
                [started, release,
                 pixels = captureCollageImage({8, 4}, [] { return image({8, 4}); })](const auto&) {
                    started->store(true, std::memory_order_release);
                    // Ignore cancellation until released: admission must remain attached to
                    // pixels held by a worker after its window closes.
                    while (!release->load(std::memory_order_acquire))
                        QThread::msleep(1);
                    return pixels;
                }),
            {8, 4},
            QStringLiteral("worker"),
            {},
            {},
            {}};
        ScreenshotPinnedImageSnapshot queued{ScreenshotExportSource::fromImage(captureCollageImage(
                                                 {96, 64}, [] { return image({96, 64}); })),
                                             {96, 64},
                                             QStringLiteral("queued"),
                                             {},
                                             {},
                                             {}};
        require(collageResourceUsage().reservedBytes == baseline + (8 * 4 + 96 * 64) * 4,
                "queued captured rasters are admitted before the import worker starts");
        window.presentPinnedWindows({first, slow, queued});
        first = {};
        slow = {};
        queued = {};
        waitUntil([&] { return started->load(std::memory_order_acquire); },
                  "the second snapshot waits at the worker barrier");
        waitUntil([&] { return completedLifetime.expired(); },
                  "completed sources retire before the remaining import batch finishes");
        require(window.document()->items().size() == 1 && window.loading(),
                "retiring completed sources preserves their imported document image");
        window.shutdown();
        require(collageResourceUsage().reservedBytes >= baseline + 8 * 4 * 4 &&
                    collageResourceUsage().reservedBytes < baseline + 96 * 64 * 4,
                "closure releases queued captures while accounting for the still-running worker");
        release->store(true, std::memory_order_release);
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "the cancelled snapshot worker retires");
        flushEvents();
    }
    waitUntil([&] { return collageResourceUsage().reservedBytes == baseline; },
              "closing the window and retiring the worker releases every captured raster");
}

void droppedFilesUseCollageImporter() {
    resetSettings();
    QTemporaryDir directory;
    require(directory.isValid(), "drop fixture directory");
    const auto red = directory.filePath(QStringLiteral("red.PNG"));
    const auto blue = directory.filePath(QStringLiteral("blue.bmp"));
    const auto invalid = directory.filePath(QStringLiteral("invalid.bmp"));
    require(image().save(red) && image({2, 3}, Qt::blue).save(blue), "write drop image fixtures");
    QFile malformed(invalid);
    require(malformed.open(QIODevice::WriteOnly) && malformed.write("not an image") > 0,
            "write corrupt drop fixture");
    malformed.close();
    Ports ports;
    CollageWindow window(nullptr, ports.services());
    window.present();
    flushEvents();
    QStringList failures;
    int completions = 0;
    QObject::connect(&window, &CollageWindow::importFinished, &window,
                     [&](const QStringList& errors) {
                         failures = errors;
                         ++completions;
                     });
    const auto drop = [&](QWidget* target, const QList<QUrl>& urls) {
        QMimeData files;
        files.setUrls(urls);
        QDragEnterEvent enter(QRectF(target->rect()).center(), Qt::CopyAction | Qt::MoveAction,
                              &files, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        require(enter.isAccepted(), "window child accepts file drag");
        // Qt routes the drop to the parent that accepted the enter event.
        auto* editor = window.editor();
        QDropEvent event(editor->rect().center(), Qt::CopyAction | Qt::MoveAction, &files,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &event);
        require(event.isAccepted() && event.dropAction() == Qt::CopyAction && window.loading(),
                "dropping begins the existing asynchronous importer with copy semantics");
        require(editor->findChild<QWidget*>(QStringLiteral("collageDropHint"))->isHidden(),
                "drop feedback is dismissed before import begins");
        waitUntil([&] { return !window.loading(); }, "dropped batch completes");
    };
    drop(window.editor()->findChild<QWidget*>(QStringLiteral("collageEmptyState")),
         {QUrl::fromLocalFile(red),
          QUrl::fromLocalFile(directory.filePath(QStringLiteral("notes.txt"))),
          QUrl::fromLocalFile(invalid), QUrl::fromLocalFile(blue), QUrl::fromLocalFile(red)});
    const auto firstBatch = window.document()->items();
    require(completions == 1 && firstBatch.size() == 3 && failures.size() == 1 &&
                failures[0].contains(invalid) && firstBatch[0].label == red &&
                firstBatch[1].label == blue && firstBatch[2].label == red &&
                firstBatch[0].id != firstBatch[2].id && firstBatch[0].image.size() == QSize(3, 2) &&
                firstBatch[1].image.size() == QSize(2, 3) &&
                firstBatch[1].image.pixelColor(0, 0) == QColor(Qt::blue),
            "file drops preserve successful image order and duplicates and report decode errors");
    require(window.document()->rotate(firstBatch[0].id, true), "transform existing collage item");
    const auto transform = window.document()->items()[0].transform;
    drop(window.editor()->canvas(), {QUrl::fromLocalFile(blue)});
    const auto appended = window.document()->items();
    require(completions == 2 && failures.isEmpty() && appended.size() == 4 &&
                appended[0].id == firstBatch[0].id && appended[0].transform == transform &&
                appended[3].label == blue,
            "dropping onto an existing collage appends without replacing or swapping images");
    window.shutdown();
}

void localizedFileDialogsListAllSupportedImages() {
    resetSettings();
    QTemporaryDir directory;
    require(directory.isValid(), "localized file-dialog fixture directory");
    const QString previousDirectory = QDir::currentPath();
    require(QDir::setCurrent(directory.path()), "scope file-dialog scanning to the fixtures");
    const auto restoreDirectory =
        qScopeGuard([&] { static_cast<void>(QDir::setCurrent(previousDirectory)); });
    QStringList patterns;
    QStringList expectedFiles;
    for (const auto& extension : ScreenshotClipboardContentReader::supportedImageFileExtensions()) {
        patterns.append(QStringLiteral("*.") + extension);
        expectedFiles.append(QStringLiteral("image.") + extension);
    }
    expectedFiles.append({QStringLiteral("uppercase.JPEG"), QStringLiteral("uppercase.AVIF"),
                          QStringLiteral("uppercase.BMP")});
    for (const auto& name : expectedFiles + QStringList{QStringLiteral("unsupported.txt")}) {
        QFile file(directory.filePath(name));
        require(file.open(QIODevice::WriteOnly) && file.write("filter fixture") > 0,
                "write file-dialog listing fixtures");
    }
    for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "file-dialog tests need the requested translation catalog");
        QApplication::installTranslator(&translator);
        const auto removeTranslator =
            qScopeGuard([&] { QApplication::removeTranslator(&translator); });
        CollageWindow window;
        window.present();
        const quint64 target = window.document()->append(image(), QStringLiteral("existing"));
        for (const bool replacement : {false, true}) {
            if (replacement)
                window.editor()->replaceImageRequested(target);
            else
                button(window, "collageAddImages")->click();
            auto* dialog = window.editor()->window()->findChild<QFileDialog*>();
            require(dialog && dialog->fileMode() == (replacement ? QFileDialog::ExistingFile
                                                                 : QFileDialog::ExistingFiles),
                    "add and replace open their corresponding image-selection dialogs");
            dialog->setOption(QFileDialog::DontUseCustomDirectoryIcons);
            dialog->setDirectory(directory.path());
            auto* model = dialog->findChild<QFileSystemModel*>();
            require(model && model->nameFilters() == patterns,
                    "Qt must parse every supported image wildcard in every collage language");
            const auto listedFiles = [&] {
                QStringList names;
                const auto root = model->index(directory.path());
                for (int row = 0; row < model->rowCount(root); ++row)
                    names.append(model->fileName(model->index(row, 0, root)));
                names.sort();
                return names;
            };
            auto sortedFiles = expectedFiles;
            sortedFiles.sort();
            waitUntil([&] { return listedFiles() == sortedFiles; },
                      "add and replace list all supported extensions and hide unsupported files");
            dialog->reject();
            flushEvents();
        }
        window.shutdown();
    }
}

void fileImportDuplicatesAndReplacement() {
    resetSettings();
    QTemporaryDir directory;
    require(directory.isValid(), "file import fixture directory");
    const QString previousDirectory = QDir::currentPath();
    require(QDir::setCurrent(directory.path()),
            "keep file-dialog scanning in a small local fixture directory");
    const auto restoreDirectory =
        qScopeGuard([&] { static_cast<void>(QDir::setCurrent(previousDirectory)); });
    const QString red = directory.filePath(QStringLiteral("red.bmp"));
    const QString blue = directory.filePath(QStringLiteral("blue.png"));
    const QString replacement = directory.filePath(QStringLiteral("replacement.BMP"));
    const QString invalid = directory.filePath(QStringLiteral("invalid.bmp"));
    const QString text = directory.filePath(QStringLiteral("text.txt"));
    const QString html = directory.filePath(QStringLiteral("page.html"));
    require(image().save(red) && image({2, 3}, Qt::blue).save(blue) &&
                bmp_import_fixtures::write(replacement, image({5, 4}, QColor(20, 40, 60, 96))),
            "write supported image fixtures");
    QFile malformed(invalid);
    require(malformed.open(QIODevice::WriteOnly) && malformed.write("not an image") > 0,
            "write a malformed supported-format fixture");
    malformed.close();
    QFile textFile(text);
    require(textFile.open(QIODevice::WriteOnly) && textFile.write("Plain text is not an image") > 0,
            "write a plain-text import fixture");
    textFile.close();
    QFile htmlFile(html);
    require(htmlFile.open(QIODevice::WriteOnly) &&
                htmlFile.write("<h1>HTML is not an image</h1>") > 0,
            "write an HTML import fixture");
    htmlFile.close();
    Ports ports;
    CollageWindow window(nullptr, ports.services());
    window.present();
    button(window, "collageAddImages")->click();
    auto* addDialog = window.editor()->window()->findChild<QFileDialog*>();
    require(addDialog && addDialog->fileMode() == QFileDialog::ExistingFiles &&
                addDialog->nameFilters().join(QString()).contains(QStringLiteral("*.bmp")),
            "collage add dialog advertises BMP alongside the existing image formats");
    addDialog->reject();
    flushEvents();
    QStringList failures;
    QObject::connect(&window, &CollageWindow::importFinished, &window,
                     [&](const auto& errors) { failures = errors; });
    window.addFiles({red, invalid, text, blue, html, red});
    waitUntil([&] { return !window.loading(); }, "file batch completes");
    require(window.document()->items().size() == 3 && failures.size() == 3 &&
                failures[0].contains(invalid) && failures[1].contains(text) &&
                failures[2].contains(html),
            "file import retains successful duplicates and consolidates malformed, text and HTML "
            "failures");
    const auto initial = window.document()->items();
    require(initial[0].label == red && initial[1].label == blue && initial[2].label == red &&
                initial[0].id != initial[2].id &&
                bmp_import_fixtures::samePixels(initial[0].image, image()),
            "duplicates are independent items appended in file selection order");
    const quint64 target = initial[1].id;
    require(window.document()->rotate(target, true) && window.document()->flip(target, true),
            "transform the replacement target");
    const auto transformed = window.document()->items()[1];
    window.editor()->replaceImageRequested(target);
    auto* dialog = window.editor()->window()->findChild<QFileDialog*>();
    if (dialog) {
        dialog->setOption(QFileDialog::DontUseCustomDirectoryIcons);
        dialog->setDirectory(directory.path());
    }
    require(dialog && dialog->fileMode() == QFileDialog::ExistingFile &&
                dialog->windowModality() == Qt::WindowModal &&
                dialog->nameFilters().join(QString()).contains(QStringLiteral("*.bmp")),
            "replacement opens a single-file dialog scoped to the collage");
    dialog->reject();
    flushEvents();
    require(window.document()->items()[1].id == target &&
                window.document()->items()[1].transform == transformed.transform &&
                window.document()->items()[1].image == transformed.image,
            "canceling replacement preserves its pixels, transform and position");
    window.replaceFile(target, invalid);
    waitUntil([&] { return !window.loading(); }, "malformed replacement completes");
    require(window.document()->items()[1].id == target &&
                window.document()->items()[1].transform == transformed.transform &&
                window.document()->items()[1].image == transformed.image && failures.size() == 1,
            "replacement decoding failure preserves the existing item");
    window.replaceFile(target, replacement);
    waitUntil([&] { return !window.loading(); }, "successful replacement completes");
    const auto replaced = window.document()->items();
    require(
        replaced.size() == 3 && replaced[0].id == initial[0].id && replaced[1].id == target &&
            replaced[2].id == initial[2].id && replaced[1].transform.isIdentity() &&
            replaced[1].image.size() == QSize(5, 4) &&
            bmp_import_fixtures::samePixels(replaced[1].image,
                                            image({5, 4}, QColor(20, 40, 60, 96))),
        "successful replacement preserves order and identity, resets transforms and retains alpha");

    const auto animation = pinned_animation_fixtures::hiddenPosterAnimation();
    const QString animatedPath = directory.filePath(QStringLiteral("animated.apng"));
    QFile animatedFile(animatedPath);
    require(animatedFile.open(QIODevice::WriteOnly) &&
                animatedFile.write(animation.bytes) == animation.bytes.size(),
            "write the existing partial-first-frame animation fixture");
    animatedFile.close();
    Ports animatedPorts;
    CollageWindow animated(nullptr, animatedPorts.services());
    animated.present();
    animated.addFiles({animatedPath});
    waitUntil([&] { return !animated.loading(); }, "animated input import completes");
    require(animated.document()->items().size() == 1 &&
                animated.document()->items().first().image.size() == QSize(2, 2) &&
                animated.document()->items().first().image.pixelColor(0, 0) == QColor(Qt::red) &&
                animated.document()->items().first().image.pixelColor(1, 1).alpha() == 0,
            "animations import one complete decoded first frame, excluding posters and subsequent "
            "frames");
}

void clearingPendingImportDiscardsStaleCompletion() {
    resetSettings();
    Ports ports;
    const auto release = std::make_shared<std::atomic_bool>(false);
    const auto started = std::make_shared<std::atomic_bool>(false);
    auto services = ports.services();
    services.readHistory =
        [release, started](const storage::CaptureHistoryRecord&) -> std::optional<QImage> {
        started->store(true, std::memory_order_release);
        while (!release->load(std::memory_order_acquire))
            QThread::msleep(1);
        return image();
    };
    CollageWindow window(nullptr, std::move(services));
    window.document()->append(image(), QStringLiteral("existing"));
    int importCompletions = 0;
    QObject::connect(&window, &CollageWindow::importFinished, &window,
                     [&](const auto&) { ++importCompletions; });
    window.present({record(QStringLiteral("pending"))});
    waitUntil([&] { return started->load(std::memory_order_acquire); },
              "the pending import reaches its reader");
    require(window.loading() && button(window, "collageClearAll")->isEnabled(),
            "existing images can be cleared while another image is loading");
    window.requestClear();
    require(ports.confirmations.size() == 1 && ports.confirmations.first().clear,
            "clearing a pending import uses the Clear all confirmation");
    ports.confirmations.first().complete(true);
    require(window.document()->items().isEmpty() && !window.loading() && !statusMessage(window),
            "confirmed clearing cancels loading and removes existing images immediately");
    release->store(true, std::memory_order_release);
    waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
              "the canceled reader retires its export job");
    flushEvents();
    require(window.document()->items().isEmpty() && importCompletions == 0 &&
                window.modal()->isOpen() && button(window, "collageAddImages")->isEnabled() &&
                !button(window, "collageCopy")->isEnabled() && !statusMessage(window),
            "a stale import cannot repopulate or complete the cleared, usable collage");
}

void rememberedSettingsMergeWithoutChangingOpenWindows(const QString& configurationFile) {
    resetSettings();
    CollageWindow first, second;
    first.present();
    second.present();
    auto* border = first.editor()->findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("collageOuterBorder"));
    auto* spacing = second.editor()->findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("collageImageSpacing"));
    auto* color = first.editor()->findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("collageBackgroundColor"));
    require(border && spacing && color && color->alphaChannelEnabled(),
            "collage form uses numeric pixel fields and alpha picker");
    border->setValue(7);
    spacing->setValue(11);
    const QColor translucent(15, 45, 75, 83);
    color->setValue(adqt::widgets::AdColorValue::solid(translucent));
    require(first.document()->settings().border == 7 && first.document()->settings().spacing == 0 &&
                first.document()->settings().background == translucent &&
                second.document()->settings().border == 0 &&
                second.document()->settings().spacing == 11 &&
                second.document()->settings().background == QColor(Qt::white),
            "editing one window preserves the independent settings of another open window");
    CollageWindow subsequentlyOpened;
    subsequentlyOpened.present();
    const auto remembered = subsequentlyOpened.document()->settings();
    require(remembered.border == 7 && remembered.spacing == 11 &&
                remembered.background == translucent,
            "per-field writes merge the latest edits across independent windows");
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.flushNow().success, "flush remembered collage settings");
    storage::ConfigurationStore reloaded(configurationFile, true, true, 60000);
    require(CollageSettingsStore(reloaded).read() == remembered,
            "collage settings and semitransparent background survive a fresh disk-backed "
            "configuration read");
}

void localizedLayoutControlsUpdateAndRememberSettings() {
    for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
        resetSettings();
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "layout controls need the requested translation catalog");
        QApplication::installTranslator(&translator);
        const auto removeTranslator =
            qScopeGuard([&] { QApplication::removeTranslator(&translator); });
        CollageWindow window;
        window.present();
        window.document()->append(image({120, 60}), QStringLiteral("first"));
        window.document()->append(image({60, 120}), QStringLiteral("second"));
        flushEvents();
        auto* direction = window.editor()->findChild<adqt::widgets::AdSegmented*>(
            QStringLiteral("collageDirection"));
        auto* sizing = window.editor()->findChild<adqt::widgets::AdSegmented*>(
            QStringLiteral("collageSizing"));
        auto* dimensions = window.editor()->outputDescriptionWidget()->findChild<QLabel*>(
            QStringLiteral("collageOutputDimensions"));
        auto* grid = window.editor()->findChild<QWidget*>(QStringLiteral("collageCustomGrid"));
        auto* rows = window.editor()->findChild<adqt::widgets::AdInputNumber*>(
            QStringLiteral("collageGridRows"));
        auto* columns = window.editor()->findChild<adqt::widgets::AdInputNumber*>(
            QStringLiteral("collageGridColumns"));
        require(direction && sizing && dimensions && grid && rows && columns,
                "window exposes its layout controls, custom grid and output size");
        require(!grid->isVisible() && !rows->isVisible() && !columns->isVisible() &&
                    direction->option(2).value == QStringLiteral("custom") &&
                    direction->optionLabel(2) ==
                        QCoreApplication::translate("CollageEditorWidget", "Custom"),
                "Custom has a stable translated option and hides its grid in vertical layout");
        const auto select = [&](adqt::widgets::AdSegmented* control, int index) {
            auto* option = control->childAt(control->optionRect(index).center());
            require(option && option->isEnabled(), "layout option must be interactive");
            const QPointF center = QRectF(option->rect()).center();
            for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, center, option->mapToGlobal(center.toPoint()),
                                  Qt::LeftButton,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(option, &event);
            }
            flushEvents();
            require(control->currentIndex() == index &&
                        control->currentValue() == control->option(index).value,
                    "clicking the visible option center must select its stable value");
        };
        const auto verify = [&](CollageDirection layoutDirection, CollageSizing imageSizing,
                                QSize size) {
            const auto settings = window.document()->settings();
            require(settings.direction == layoutDirection && settings.sizing == imageSizing &&
                        window.document()->layout().size == size &&
                        dimensions->text() ==
                            QCoreApplication::translate("CollageEditorWidget", "%1 × %2 px")
                                .arg(size.width())
                                .arg(size.height()),
                    "layout choices must update the document, preview layout, and output size");
        };
        verify(CollageDirection::Vertical, CollageSizing::Uniform, {120, 300});
        select(direction, 1);
        verify(CollageDirection::Horizontal, CollageSizing::Uniform, {150, 60});
        require(!grid->isVisible(), "horizontal layout must hide custom grid fields");
        select(sizing, 1);
        verify(CollageDirection::Horizontal, CollageSizing::Original, {180, 120});
        CollageWindow reopened;
        reopened.present();
        const auto remembered = reopened.document()->settings();
        require(remembered.direction == CollageDirection::Horizontal &&
                    remembered.sizing == CollageSizing::Original,
                "both layout choices must be remembered by subsequently opened windows");
        select(direction, 0);
        verify(CollageDirection::Vertical, CollageSizing::Original, {120, 180});
        select(sizing, 0);
        verify(CollageDirection::Vertical, CollageSizing::Uniform, {120, 300});
        require(reopened.document()->settings() == remembered,
                "later layout edits must preserve the independent reopened window");
        select(direction, 2);
        verify(CollageDirection::Custom, CollageSizing::Uniform, {240, 60});
        require(grid->isVisible() && rows->isVisible() && columns->isVisible() &&
                    rows->value() == 1 && columns->value() == 2 && rows->minimum() == 1 &&
                    columns->minimum() == 1 && rows->decimals() == 0 && columns->decimals() == 0 &&
                    rows->accessibleName() ==
                        QCoreApplication::translate("CollageEditorWidget", "Rows") &&
                    columns->accessibleName() ==
                        QCoreApplication::translate("CollageEditorWidget", "Columns"),
                "Custom reveals positive integer inputs with localized accessible names");
        const auto rowRect = QRect(rows->mapTo(window.editor(), QPoint{}), rows->size());
        const auto columnRect = QRect(columns->mapTo(window.editor(), QPoint{}), columns->size());
        const auto directionRect =
            QRect(direction->mapTo(window.editor(), QPoint{}), direction->size());
        require(rowRect.right() < columnRect.left() && rowRect.top() == columnRect.top() &&
                    rowRect.top() > directionRect.bottom(),
                "custom grid inputs must be side by side below layout direction");
        for (const auto* source : {"Rows", "Columns"}) {
            const auto label = QCoreApplication::translate("CollageEditorWidget", source);
            bool found = false;
            for (auto* candidate : grid->findChildren<QLabel*>())
                found = found || candidate->text() == label;
            require(found, "custom grid must display localized Rows and Columns labels");
        }
        select(sizing, 1);
        verify(CollageDirection::Custom, CollageSizing::Original, {240, 120});
        select(sizing, 0);
        for (int index = 0; index < 3; ++index)
            window.document()->append(image({120, 60}), QStringLiteral("extra-%1").arg(index));
        require(rows->value() == 1 && columns->value() == 5,
                "new images must expand columns while retaining custom rows");
        rows->setValue(2);
        columns->setValue(2);
        verify(CollageDirection::Custom, CollageSizing::Uniform, {360, 120});
        require(rows->value() == 2 && columns->value() == 3,
                "a grid edit below capacity must expand columns and keep the requested rows");
        const auto expanded = window.document()->settings();
        int changes = 0;
        QObject::connect(window.document(), &CollageDocument::changed, &window, [&] { ++changes; });
        columns->setValue(1);
        require(columns->value() == 3 && window.document()->settings() == expanded && changes == 0,
                "too-small column edits must rebound even when normalized settings are unchanged");
        const auto removed = window.document()->items().last().id;
        require(window.document()->remove(removed) && rows->value() == 2 && columns->value() == 3,
                "removing images must retain previously expanded grid dimensions");
        select(direction, 0);
        require(!grid->isVisible(), "switching away from Custom must hide its form field");
        select(direction, 2);
        require(rows->value() == 2 && columns->value() == 3 &&
                    window.document()->settings().customGridInitialized,
                "returning to Custom must retain the initialized grid");
        CollageWindow customReopened;
        customReopened.present();
        require(customReopened.document()->settings() == expanded &&
                    reopened.document()->settings() == remembered,
                "new windows remember expanded custom settings without changing older windows");
        window.editor()->setExportBusy(true);
        require(!rows->isEnabled() && !columns->isEnabled(),
                "exporting must disable custom dimensions with the settings form");
        window.editor()->setExportBusy(false);
    }
    resetSettings();
}

void customGridInitializesNearSquareAndPersists(const QString& configurationFile) {
    const std::array<int, 5> counts{0, 1, 2, 5, 10};
    const std::array<QSize, 5> grids{QSize(1, 1), QSize(1, 1), QSize(2, 1), QSize(3, 2),
                                     QSize(4, 3)};
    for (std::size_t caseIndex = 0; caseIndex < counts.size(); ++caseIndex) {
        // Every case starts from the uninitialized settings used by an existing installation.
        resetSettings();
        CollageWindow window;
        window.present();
        for (int index = 0; index < counts[caseIndex]; ++index)
            window.document()->append(image(), QString::number(index));
        auto* direction = window.editor()->findChild<adqt::widgets::AdSegmented*>(
            QStringLiteral("collageDirection"));
        require(direction != nullptr, "first-use Custom exposes its direction control");
        direction->setCurrentValue(QStringLiteral("custom"));
        const auto settings = window.document()->settings();
        require(settings.customGridInitialized && settings.rows == grids[caseIndex].height() &&
                    settings.columns == grids[caseIndex].width(),
                "first Custom selection initializes a near-square grid, including empty collages");
        require(
            CollageSettingsStore(storage::ApplicationStorage::instance().configuration()).read() ==
                settings,
            "first-use grid initialization must persist direction, dimensions and status together");
    }
    resetSettings();
    CollageWindow first, second;
    first.present();
    second.present();
    require(first.document()->settings() == CollageSettings{} &&
                second.document()->settings() == CollageSettings{},
            "legacy collage defaults must remain Vertical with an uninitialized custom grid");
    auto* firstDirection =
        first.editor()->findChild<adqt::widgets::AdSegmented*>(QStringLiteral("collageDirection"));
    auto* secondDirection =
        second.editor()->findChild<adqt::widgets::AdSegmented*>(QStringLiteral("collageDirection"));
    require(firstDirection && secondDirection, "independent windows expose their layout controls");
    firstDirection->setCurrentValue(QStringLiteral("custom"));
    secondDirection->setCurrentValue(QStringLiteral("custom"));
    auto* rows =
        first.editor()->findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageGridRows"));
    auto* columns = second.editor()->findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("collageGridColumns"));
    require(rows && columns, "independent windows expose custom dimensions");
    rows->setValue(4);
    columns->setValue(5);
    require(first.document()->settings().rows == 4 && first.document()->settings().columns == 1 &&
                second.document()->settings().rows == 1 &&
                second.document()->settings().columns == 5,
            "editing custom dimensions must preserve each open window's independent settings");
    CollageWindow reopened;
    reopened.present();
    const auto remembered = reopened.document()->settings();
    require(remembered.direction == CollageDirection::Custom && remembered.rows == 4 &&
                remembered.columns == 5 && remembered.customGridInitialized,
            "custom dimensions merge per field for subsequently opened windows");
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.flushNow().success, "flush remembered custom grid settings");
    storage::ConfigurationStore reloaded(configurationFile, true, true, 60000);
    require(CollageSettingsStore(reloaded).read() == remembered,
            "Custom direction, dimensions and initialization survive a fresh disk-backed read");
    require(!configuration.setValue(QStringLiteral("collage/rows"), 0) &&
                !configuration.setValue(QStringLiteral("collage/columns"), -1) &&
                CollageSettingsStore(configuration).read() == remembered,
            "configuration validation must reject nonpositive grid dimensions without changing "
            "settings");
    resetSettings();
}

storage::CaptureHistoryRecord publishHistoryFixture() {
    storage::CaptureHistoryDraft draft;
    draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    draft.createdUtc = QDateTime::currentDateTimeUtc();
    draft.canvasBounds = QRect(0, 0, 8, 6);
    draft.selection.rectangle = draft.canvasBounds;
    draft.selection.shadowColor = Qt::transparent;
    draft.canvasHistory = QByteArrayLiteral("{\"schemaVersion\":1,\"document\":{},\"history\":{}}");
    draft.displays.append(
        {QStringLiteral("display"), QStringLiteral("Display"), image({8, 6}, Qt::blue)});
    draft.resultImage = image({5, 4}, QColor(120, 30, 80, 128));
    const auto published =
        storage::ApplicationStorage::instance().captureHistory().publish(std::move(draft)).get();
    require(published.storage.success, "publish original screenshot history fixture");
    return published.record;
}

void exportFeedbackUsesMessagesAndBusyButtons() {
    resetSettings();
    Ports firstPorts, secondPorts;
    CollageWindow first(nullptr, firstPorts.services()), second(nullptr, secondPorts.services());
    first.present();
    second.present();
    first.document()->append(image(), QStringLiteral("first"));
    second.document()->append(image(), QStringLiteral("second"));
    first.copy();
    QPointer<AdMessageHandle> copying = statusMessage(first);
    require(copying && copying->type() == AdMessage::Type::Loading &&
                copying->content() == QStringLiteral("Preparing collage…") &&
                !hasInlineStatus(first) && button(first, "collageCopy")->busy() &&
                !button(first, "collageSave")->busy(),
            "copy preparation uses a loading Message and the Copy button without a bottom prompt");
    second.save();
    QPointer<AdMessageHandle> saving = statusMessage(second);
    require(saving && saving != copying && button(second, "collageSave")->busy() &&
                !button(second, "collageCopy")->busy(),
            "each collage owns its export Message and loads only the selected action button");
    waitUntil([&] { return firstPorts.copies.size() == 1 && secondPorts.saves.size() == 1; },
              "both export workflows receive prepared artifacts");
    require(statusMessage(first) == copying && statusMessage(second) == saving &&
                copying->content() == QStringLiteral("Copying collage…") &&
                saving->content() == QStringLiteral("Saving collage…") && !hasInlineStatus(first) &&
                !hasInlineStatus(second),
            "export phases update their existing Messages without a bottom status panel");
    firstPorts.copies.first().complete(false, QStringLiteral("clipboard failure"));
    require(messageError(first) == QStringLiteral("clipboard failure") &&
                !button(first, "collageCopy")->busy() && !button(first, "collageSave")->busy() &&
                first.modal()->isOpen() && saving->isOpen() && second.busy(),
            "failure shows an error Message, clears button loading, and preserves other exports");
    first.save();
    waitUntil([&] { return firstPorts.saves.size() == 1; }, "retry starts the save workflow");
    firstPorts.saves.first().complete(false, {});
    require(!statusMessage(first) && !hasInlineStatus(first) &&
                !button(first, "collageSave")->busy() && button(first, "collageSave")->isEnabled(),
            "save cancellation clears feedback and loading so the action can be retried");
    first.copy();
    waitUntil([&] { return firstPorts.copies.size() == 2; }, "copy retry starts its workflow");
    firstPorts.copies.last().complete(true, {});
    require(!first.modal()->isOpen() && !statusMessage(first) &&
                !button(first, "collageCopy")->busy() && saving->isOpen(),
            "successful export clears its Message and button loading before closing");
    second.requestClose();
    require(secondPorts.confirmations.size() == 1, "closing an export requests discard approval");
    secondPorts.confirmations.first().complete(true);
    require(!second.modal()->isOpen() && !statusMessage(second) &&
                !button(second, "collageSave")->busy(),
            "discarding an active export clears its Message and button loading");
    secondPorts.saves.first().complete(false, QStringLiteral("late failure"));
    require(!statusMessage(second), "a late export callback cannot reopen feedback after closing");
}

void exportCompletionFailureCancellationAndHistoryIsolation(const QString& directory) {
    resetSettings();
    const auto original = publishHistoryFixture();
    auto& repository = storage::ApplicationStorage::instance().captureHistory();
    const auto before = repository.recordsSnapshot();
    const QString automaticDirectory = QDir(directory).filePath(QStringLiteral("automatic"));
    require(QDir().mkpath(automaticDirectory), "create isolated automatic-save destination");
    const storage::ScreenshotSettings settings;
    require(settings.setAutoSaveAfterExecutionActions({QStringLiteral("copy_screenshot")}) &&
                settings.setCopyImageFileToClipboard(false) &&
                settings.setImageSaveDirectory(automaticDirectory) &&
                settings.autoSaveAfterExecution(QStringLiteral("copy_screenshot")),
            "enable the existing screenshot auto-save setting in isolated real configuration");
    Ports ports;
    auto services = ports.services();
    services.readHistory = {};
    CollageWindow window(nullptr, std::move(services));
    window.present({original});
    waitUntil([&] { return !window.loading(); }, "real repository final result import completes");
    require(window.document()->items().size() == 1 &&
                window.document()->items().first().image.size() == QSize(5, 4),
            "collage imports the final screenshot result rather than the desktop image");
    int exports = 0;
    QObject::connect(&window, &CollageWindow::exportFinished, &window, [&](bool) { ++exports; });
    window.copy();
    window.copy();
    window.save();
    require(window.busy() && !button(window, "collageCopy")->isEnabled() &&
                !button(window, "collageSave")->isEnabled(),
            "export is busy immediately and prevents duplicate copy or save submissions");
    waitUntil([&] { return ports.copies.size() == 1; }, "copy receives a prepared export artifact");
    require(ports.saves.isEmpty() && ports.copies.first().receiver == &window &&
                ports.copies.first().owner == window.editor()->window() &&
                ports.copies.first().artifact->isValid() &&
                !ports.copies.first().artifact->clipboardPlacement(),
            "copy supplies only the collage artifact through the existing export interface");
    window.copy();
    window.save();
    require(ports.copies.size() == 1 && ports.saves.isEmpty(),
            "pending publication rejects repeat submissions");
    ports.copies.first().complete(false, QStringLiteral("clipboard failure"));
    require(!window.busy() && window.modal()->isOpen() && exports == 1 &&
                messageError(window) == QStringLiteral("clipboard failure") &&
                window.document()->items().size() == 1,
            "copy failure reports feedback while preserving the usable collection");
    window.save();
    waitUntil([&] { return ports.saves.size() == 1; }, "save receives a prepared export artifact");
    window.save();
    window.copy();
    require(ports.saves.size() == 1 && ports.copies.size() == 1,
            "pending file export rejects repeated save and copy submissions");
    ports.saves.first().complete(false, QStringLiteral("disk failure"));
    require(!window.busy() && window.modal()->isOpen() && exports == 2 &&
                messageError(window) == QStringLiteral("disk failure") &&
                window.document()->items().size() == 1,
            "save failure preserves the editing state and reports its workflow error");
    window.save();
    waitUntil([&] { return ports.saves.size() == 2; },
              "save retry reaches the file dialog workflow");
    ports.saves.last().complete(false, {});
    require(!window.busy() && window.modal()->isOpen() && exports == 3 &&
                window.document()->items().size() == 1,
            "save dialog cancellation preserves the collage and allows retry");
    window.copy();
    waitUntil([&] { return ports.copies.size() == 2; }, "copy retry reaches the workflow");
    ports.copies.first().complete(true, {});
    require(window.busy() && window.modal()->isOpen() && exports == 3,
            "a stale callback from an earlier attempt cannot finish the active retry");
    ports.copies.last().complete(true, {});
    require(!window.modal()->isOpen() && exports == 4 && ports.confirmations.isEmpty(),
            "successful clipboard publication closes automatically without discard confirmation");
    ports.copies.last().complete(false, QStringLiteral("late failure"));
    require(exports == 4, "callbacks after automatic closure cannot emit another terminal result");
    repository.drain();
    require(repository.recordsSnapshot().revision == before.revision &&
                repository.recordsSnapshot().records == before.records &&
                QDir(automaticDirectory).entryList(QDir::Files).isEmpty() &&
                ports.saves.size() == 2 &&
                settings.autoSaveAfterExecution(QStringLiteral("copy_screenshot")),
            "collage exports never add history or trigger screenshot automatic saving");
}

#ifdef Q_OS_WIN
void nativeClipboardLimitsRejectBeforePreparation() {
    resetSettings();
    waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
              "earlier export jobs retire before clipboard capacity admission");
    require(storage::ScreenshotSettings().setCopyImageFileToClipboard(false),
            "use native image publication for the clipboard capacity test");
    CollageWindow window;
    window.present();
    window.document()->append(image(), QStringLiteral("small image with wide padding"));
    auto settings = window.document()->settings();
    settings.border = 20000;
    window.document()->setSettings(settings);
    require(window.document()->layout().isValid(),
            "clipboard-limited geometry still fits the full-resolution file output limit");
    const int pending = ScreenshotExportCoordinator::shared().pendingJobCount();
    int failures = 0;
    QObject::connect(&window, &CollageWindow::exportFinished, &window, [&](bool success) {
        require(!success, "clipboard admission reports a failed export");
        ++failures;
    });
    window.copy();
    require(failures == 1 && !window.busy() && window.modal()->isOpen() &&
                window.document()->items().size() == 1 && !messageError(window).isEmpty() &&
                button(window, "collageSave")->isEnabled() &&
                ScreenshotExportCoordinator::shared().pendingJobCount() == pending,
            "known native clipboard limits fail before expensive preparation and preserve saving");
}
#endif

void completedFileSaveUsesExistingWriter(const QString& directory) {
    resetSettings();
    Ports ports;
    auto services = ports.services();
    const QString path = QDir(directory).filePath(QStringLiteral("saved-collage.png"));
    bool writerCompleted = false;
    bool writerStarted = false;
    services.save = [&](const auto& artifact, QObject* receiver, QWidget*, auto complete) {
        writerStarted = artifact->requestSaveToPath(
            receiver, path, ScreenshotImageFileFormat::Png, {},
            [&, complete = std::move(complete)](const ScreenshotExportTaskResult& result) {
                writerCompleted = result.succeeded();
                complete(result.succeeded(), result.error);
            });
    };
    CollageWindow window(nullptr, std::move(services));
    window.present();
    window.document()->append(image({3, 2}, QColor(180, 40, 20, 128)), QStringLiteral("alpha"));
    auto layout = window.document()->settings();
    layout.background = Qt::transparent;
    layout.border = 1;
    window.document()->setSettings(layout);
    window.save();
    require(window.busy() && window.modal()->isOpen(),
            "save stays open while export preparation is pending");
    waitUntil([&] { return !window.modal()->isOpen(); }, "writer completion closes the collage");
    const QImage output(path);
    require(
        writerStarted && writerCompleted && output.size() == QSize(5, 4) &&
            output.pixelColor(0, 0).alpha() == 0 && output.pixelColor(1, 1).alpha() == 128 &&
            ports.confirmations.isEmpty(),
        "the existing writer saves full-resolution static alpha pixels before the window closes");
}

void saveResultDefersCollageCompletion(const QString& directory) {
    resetSettings();
    const storage::ScreenshotSettings settings;
    const auto previousDialog = settings.saveAsFileDialog();
    const auto previousDirectory = settings.lastManualSaveDirectory();
    const auto previousFormat = settings.lastManualSaveFormat();
    const auto previousFormatOptions = settings.manualSaveFormatOptions();
    const bool previousResult = settings.showSaveResult();
    const auto restore = qScopeGuard([&] {
        static_cast<void>(settings.setSaveAsFileDialog(previousDialog));
        static_cast<void>(settings.setLastManualSaveDirectory(previousDirectory));
        static_cast<void>(settings.setLastManualSaveState(previousFormat, previousFormatOptions));
        static_cast<void>(settings.setShowSaveResult(previousResult));
    });
    require(settings.setSaveAsFileDialog(QStringLiteral("snow_shot")) &&
                settings.setShowSaveResult(true) &&
                settings.setLastManualSaveDirectory(directory) &&
                settings.setLastManualSaveState(QStringLiteral("png"), {}),
            "enable the real custom save result for collage exports");
    for (const bool destroyOnCompletion : {false, true}) {
        QPointer<CollageWindow> window = new CollageWindow;
        const auto cleanup = qScopeGuard([&] { delete window.data(); });
        window->present();
        window->document()->append(image({40, 30}), QStringLiteral("save result"));
        int completions = 0;
        QObject::connect(window, &CollageWindow::exportFinished, qApp, [&](bool success) {
            require(success, "dismissing a saved collage result reports success");
            ++completions;
            if (destroyOnCompletion)
                delete window.data();
        });
        window->save();
        QPointer<AdModal> dialog;
        waitUntil(
            [&] {
                dialog = window->findChild<AdModal*>(QStringLiteral("screenshotSaveAsFileModal"));
                return dialog &&
                       dialog->contentWidget()->property("previewGeneration").toULongLong() > 0;
            },
            "collage opens and prepares its real custom save dialog");
        auto* filename = dialog->contentWidget()->findChild<adqt::widgets::AdLineEdit*>(
            QStringLiteral("saveFilenameInput"));
        require(filename, "collage save filename input exists");
        filename->setText(destroyOnCompletion ? QStringLiteral("collage-result-teardown")
                                              : QStringLiteral("collage-result"));
        auto* result = dialog->contentWidget()->findChild<adqt::widgets::AdResult*>(
            QStringLiteral("saveResult"));
        dialog->acceptButton()->click();
        waitUntil([&] { return result && result->isVisible(); }, "collage save shows its result");
        require(completions == 0 && window->busy() && window->modal()->isOpen() &&
                    dialog->windowModality() == Qt::WindowModal &&
                    QFileInfo::exists(QDir::fromNativeSeparators(result->subTitle())),
                "saved result keeps the collage and its export alive until dismissal");
        dialog->reject();
        waitUntil([&] { return completions == 1 && !dialog; },
                  "result dismissal completes once and releases its dialog");
        require(destroyOnCompletion ? window.isNull() : !window->modal()->isOpen(),
                "completion may close or synchronously destroy the collage owner");
        flushEvents();
        require(completions == 1, "owner teardown cannot repeat collage completion");
    }
}

void destroyedAndClosedWindowsIgnorePendingCallbacks() {
    resetSettings();
    Ports ports;
    auto* window = new CollageWindow(nullptr, ports.services());
    window->present();
    window->document()->append(image(), QStringLiteral("pending"));
    window->copy();
    waitUntil([&] { return ports.copies.size() == 1; }, "pending copy reaches publication seam");
    QPointer<CollageWindow> guard(window);
    delete window;
    flushEvents();
    require(!guard && !ports.copies.first().receiver && !ports.copies.first().owner,
            "destroying a collage disposes its independent native surface and receiver");
    ports.copies.first().complete(true, {});

    auto* confirmed = new CollageWindow(nullptr, ports.services());
    confirmed->present();
    confirmed->document()->append(image(), QStringLiteral("confirm"));
    confirmed->requestClose();
    require(ports.confirmations.size() == 1, "capture a pending confirmation callback");
    delete confirmed;
    ports.confirmations.first().complete(true);

    CollageWindow manuallyClosed(nullptr, ports.services());
    manuallyClosed.present();
    manuallyClosed.document()->append(image(), QStringLiteral("manual close"));
    int lateExports = 0;
    QObject::connect(&manuallyClosed, &CollageWindow::exportFinished, &manuallyClosed,
                     [&](bool) { ++lateExports; });
    manuallyClosed.copy();
    waitUntil([&] { return ports.copies.size() == 2; },
              "manual-close copy reaches publication seam");
    manuallyClosed.requestClear();
    require(ports.confirmations.size() == 1 && manuallyClosed.document()->items().size() == 1 &&
                !button(manuallyClosed, "collageAddImages")->isEnabled(),
            "an active export preserves its collection and blocks clearing or importing");
    manuallyClosed.requestClose();
    require(ports.confirmations.size() == 2, "a pending export retains the discard confirmation");
    ports.confirmations.last().complete(true);
    require(!manuallyClosed.modal()->isOpen() && ports.copies.last().artifact->isCancelled(),
            "manual closure cancels the prepared export artifact");
    ports.copies.last().complete(true, {});
    require(lateExports == 0,
            "late publication success cannot complete an already discarded collage");

    auto* terminal = new CollageWindow(nullptr, ports.services());
    terminal->present();
    terminal->document()->append(image(), QStringLiteral("terminal"));
    terminal->copy();
    waitUntil([&] { return ports.copies.size() == 3; },
              "terminal deletion fixture reaches publication seam");
    QPointer<CollageWindow> terminalGuard(terminal);
    QObject::connect(terminal, &CollageWindow::exportFinished, qApp,
                     [terminal](bool) { delete terminal; });
    ports.copies.last().complete(false, QStringLiteral("failure with observer teardown"));
    require(!terminalGuard, "a terminal observer can synchronously destroy the collage safely");
}

void renderWindowPreview(const QString& path) {
    resetSettings();
    Ports ports;
    CollageWindow window(nullptr, ports.services());
    window.present();
    const auto capture = [](QSize size, const QColor& color, const QString& label) {
        QImage result = image(size, QColor(250, 250, 250));
        QPainter painter(&result);
        painter.fillRect(QRect(0, 0, size.width(), 34), color);
        painter.setPen(Qt::white);
        painter.drawText(QRect(14, 0, size.width() - 28, 34), Qt::AlignVCenter, label);
        painter.setPen(Qt::NoPen);
        for (int row = 0; row < 4; ++row) {
            painter.setBrush(row == 0 ? color.lighter(180) : QColor(225, 228, 232));
            painter.drawRoundedRect(QRect(16, 50 + row * 30, size.width() - 32 - row * 26, 16), 4,
                                    4);
        }
        return result;
    };
    window.document()->append(
        capture({440, 220}, QColor(22, 119, 255), QStringLiteral("Capture A")),
        QStringLiteral("capture-a.png"));
    window.document()->append(capture({320, 180}, QColor(82, 160, 70), QStringLiteral("Capture B")),
                              QStringLiteral("capture-b.png"));
    window.document()->append(
        capture({480, 260}, QColor(140, 85, 185), QStringLiteral("Capture C")),
        QStringLiteral("capture-c.png"));
    auto settings = window.document()->settings();
    settings.border = 16;
    settings.spacing = 16;
    settings.background = Qt::transparent;
    window.document()->setSettings(settings);
    flushEvents();
    window.editor()->canvas()->fitToCanvas();
    flushEvents();
    require(QDir().mkpath(QFileInfo(path).absolutePath()) &&
                window.editor()->window()->grab().save(path),
            "save the complete collage window preview, including native chrome and footer");
    std::cout << "Preview: " << path.toStdString() << '\n';
}

void nativeSaveServiceCompletesAndCancelsExactlyOnce(const QString& directory) {
    const storage::ScreenshotSettings settings;
    require(settings.setLastManualSaveDirectory(directory) &&
                settings.setLastManualSaveFormat(QStringLiteral("png")),
            "scope manual-save dialog defaults to the temporary test directory");
    QWidget owner;
    owner.show();
    QObject lifetime(&owner);
    const auto artifact = [] {
        return std::make_shared<ScreenshotExportArtifact>(
            ScreenshotExportSource::fromImage(image({3, 2}, QColor(80, 40, 20, 96))));
    };
    const auto findDialog = [&]() {
        auto* dialog = owner.findChild<QFileDialog*>(QStringLiteral("screenshotManualSaveDialog"));
        require(dialog != nullptr, "the manual-save service exposes its owned file dialog");
        dialog->setOption(QFileDialog::DontUseCustomDirectoryIcons);
        return dialog;
    };
    int cancelled = 0;
    const auto rejectionSource = artifact();
    auto rejected = ScreenshotManualSaveService::open(
        &lifetime, &owner, rejectionSource, [&](const ScreenshotExportTaskResult& result) {
            require(result.failureStage == ScreenshotExportFailureStage::Cancelled &&
                        result.error.isEmpty(),
                    "native save rejection reports cancellation without a write error");
            ++cancelled;
        });
    require(rejected.isValid(), "native manual-save dialog starts");
    auto* dialog = findDialog();
    require(dialog->windowModality() == Qt::WindowModal &&
                dialog->acceptMode() == QFileDialog::AcceptSave,
            "native manual save is scoped to its owner and uses save-file selection");
    dialog->reject();
    rejected.cancel();
    flushEvents();
    require(cancelled == 1 && rejectionSource->isCancelled() && !rejected.isValid(),
            "dialog rejection retires its dedicated artifact and completes exactly once");

    int explicitCancellations = 0;
    const auto cancelledSource = artifact();
    auto handle = ScreenshotManualSaveService::open(
        &lifetime, &owner, cancelledSource, [&](const ScreenshotExportTaskResult& result) {
            require(result.failureStage == ScreenshotExportFailureStage::Cancelled,
                    "handle cancellation reports the cancellation stage");
            ++explicitCancellations;
        });
    require(handle.isValid(), "explicit-cancellation fixture opens a dialog");
    findDialog();
    handle.cancel();
    handle.cancel();
    flushEvents();
    require(explicitCancellations == 1 && cancelledSource->isCancelled() &&
                !owner.findChild<QFileDialog*>(QStringLiteral("screenshotManualSaveDialog")),
            "repeated handle cancellation closes the dialog with one terminal completion");

    const QString path = QDir(directory).filePath(QStringLiteral("manual-service-result.png"));
    bool completed = false;
    int saving = 0;
    const auto savedSource = artifact();
    auto save = ScreenshotManualSaveService::open(
        &lifetime, &owner, savedSource,
        [&](const ScreenshotExportTaskResult& result) {
            require(result.succeeded() && result.savedPath == path && QFileInfo::exists(path),
                    "manual-save success is published only after the existing writer finishes");
            completed = true;
        },
        Qt::WindowModal, [&] { ++saving; });
    require(save.isValid(), "file writer fixture opens a native save request");
    dialog = findDialog();
    dialog->selectNameFilter(
        ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Png));
    dialog->selectFile(path);
    require(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
            "accept the selected native save destination");
    waitUntil([&] { return completed; },
              "the native save service completes its asynchronous writer");
    flushEvents();
    const QImage saved(path);
    require(saving == 1 && saved.size() == QSize(3, 2) && saved.pixelColor(0, 0).alpha() == 96 &&
                !save.isValid(),
            "native save uses the full static alpha image and retires its request handle");

    int abandonedCompletions = 0;
    auto* abandonedLifetime = new QObject;
    const auto abandonedSource = artifact();
    auto abandoned = ScreenshotManualSaveService::open(
        abandonedLifetime, &owner, abandonedSource,
        [&](const ScreenshotExportTaskResult&) { ++abandonedCompletions; });
    require(abandoned.isValid(), "lifetime-abandonment fixture opens a dialog");
    findDialog();
    delete abandonedLifetime;
    flushEvents();
    abandoned.cancel();
    require(abandonedCompletions == 0 && abandonedSource->isCancelled() &&
                !owner.findChild<QFileDialog*>(QStringLiteral("screenshotManualSaveDialog")),
            "destroying the native save lifetime suppresses callbacks and disposes the dialog");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QElapsedTimer startup;
    startup.start();
    QApplication application(argc, argv);
    std::cout << "QApplication startup: " << startup.elapsed() << " ms\n" << std::flush;
    QApplication::setQuitOnLastWindowClosed(false);
    QTemporaryDir directory;
    auto& storage = storage::ApplicationStorage::instance();
    timed("Storage initialization", [&] {
        require(directory.isValid() &&
                    storage.initialize({directory.path(), directory.path(), 60000}).success,
                "initialize temporary application storage without touching user configuration");
    });
    timed("Theme initialization",
          [&] { styles::ThemeManager::instance().initialize(application); });
    const auto arguments = QApplication::arguments();
    if (arguments.contains(QStringLiteral("--file-dialog-only"))) {
        timed("Localized file-dialog listings", localizedFileDialogsListAllSupportedImages);
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        std::cout << "Collage file-dialog tests passed\n";
        return 0;
    }
    if (arguments.contains(QStringLiteral("--drag-drop-only"))) {
        timed("Dropped file imports", droppedFilesUseCollageImporter);
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "drop tests drain export jobs");
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        std::cout << "Collage window drag-and-drop tests passed\n";
        return 0;
    }
    if (arguments.contains(QStringLiteral("--selected-files-only"))) {
        timed("Selected-file ordered imports", selectedFilesOpenAndAppendInOrder);
        timed("Empty selections and query failures", emptySelectionsAndFailuresKeepWindowOpen);
        timed("Selected-file cancellation", selectedFileQueriesCancelWithWindowLifecycle);
        timed("Busy export selection requests",
              singletonExportsIgnoreNewOperationsWithoutPresenting);
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "selected-file tests drain export jobs");
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        std::cout << "Collage selected-file tests passed\n";
        return 0;
    }
    if (arguments.contains(QStringLiteral("--singleton-only"))) {
        singletonWindowScenarios();
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "singleton tests drain export jobs");
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        std::cout << "Collage singleton tests passed\n";
        return 0;
    }
    if (arguments.contains(QStringLiteral("--live-pinned-import-only"))) {
        timed("Owned live pin imports", livePinnedImportsPreserveOrderAndFailures);
        timed("Live pin cancellation and memory", livePinnedImportsCancelAndRespectMemoryBudget);
        timed("Live pin queued raster ownership",
              livePinnedQueueReleasesCompletedSnapshotsAndAccountsForCancellation);
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "live pin import tests drain export jobs");
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        return 0;
    }
    if (arguments.contains(QStringLiteral("--pinned-import-only"))) {
        timed("Pinned record imports",
              [&] { pinnedRecordsImportSavedPixelsAndText(directory.path()); });
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "pinned import tests drain export jobs");
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        return 0;
    }
    const auto preview = arguments.indexOf(QStringLiteral("--preview"));
    if (preview >= 0) {
        require(preview + 1 < arguments.size(), "--preview requires a PNG output path");
        styles::ThemeManager::instance().setThemeMode(arguments.contains(QStringLiteral("--dark"))
                                                          ? styles::ThemeMode::Dark
                                                          : styles::ThemeMode::Light);
        timed("Full window preview", [&] { renderWindowPreview(arguments.at(preview + 1)); });
        storage.shutdown();
        return 0;
    }
    if (arguments.contains(QStringLiteral("--footer-only"))) {
        timed("Footer output descriptions", outputDescriptionsInFooter);
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "footer tests drain export jobs");
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        std::cout << "Collage footer tests passed\n";
        return 0;
    }
    if (arguments.contains(QStringLiteral("--export-feedback-only"))) {
        timed("Export feedback", exportFeedbackUsesMessagesAndBusyButtons);
        timed("Save result lifetime", [&] { saveResultDefersCollageCompletion(directory.path()); });
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "export feedback tests drain export jobs");
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        std::cout << "Collage export feedback tests passed\n";
        return 0;
    }
    if (arguments.contains(QStringLiteral("--layout-controls-only"))) {
        timed("Localized layout controls", localizedLayoutControlsUpdateAndRememberSettings);
        timed("Custom grid initialization and persistence", [&] {
            customGridInitializesNearSquareAndPersists(
                QDir(storage.configurationDirectory()).filePath(QStringLiteral("config.json")));
        });
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        std::cout << "Collage layout control tests passed\n";
        return 0;
    }
    if (arguments.contains(QStringLiteral("--import-feedback-only"))) {
        timed("Import feedback", importFeedbackUsesMessagesWithoutLayoutChanges);
        timed("Clear and initial import failures", clearAndInitialImportFailures);
        timed("Clear pending import", clearingPendingImportDiscardsStaleCompletion);
        timed("File imports and replacement", fileImportDuplicatesAndReplacement);
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "import feedback tests drain export jobs");
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        std::cout << "Collage import feedback tests passed\n";
        return 0;
    }
    timed("Window pin", windowPinStartsEnabledAndPreservesEdits);
    timed("Centered window placement", windowOpensCenteredOnCurrentScreen);
    singletonWindowScenarios();
    timed("Selected-file ordered imports", selectedFilesOpenAndAppendInOrder);
    timed("Empty selections and query failures", emptySelectionsAndFailuresKeepWindowOpen);
    timed("Selected-file cancellation", selectedFileQueriesCancelWithWindowLifecycle);
    timed("Independent windows and close", independentWindowsAndClosePolicy);
    timed("Footer output descriptions", outputDescriptionsInFooter);
    timed("Export feedback", exportFeedbackUsesMessagesAndBusyButtons);
    timed("Import feedback", importFeedbackUsesMessagesWithoutLayoutChanges);
    timed("Clear and initial import failures", clearAndInitialImportFailures);
    timed("Clear pending import", clearingPendingImportDiscardsStaleCompletion);
    timed("File imports and replacement", fileImportDuplicatesAndReplacement);
    timed("Localized file-dialog listings", localizedFileDialogsListAllSupportedImages);
    timed("Dropped file imports", droppedFilesUseCollageImporter);
    timed("Owned live pin imports", livePinnedImportsPreserveOrderAndFailures);
    timed("Live pin cancellation and memory", livePinnedImportsCancelAndRespectMemoryBudget);
    timed("Live pin queued raster ownership",
          livePinnedQueueReleasesCompletedSnapshotsAndAccountsForCancellation);
    timed("Pinned record imports",
          [&] { pinnedRecordsImportSavedPixelsAndText(directory.path()); });
    timed("Remembered settings", [&] {
        rememberedSettingsMergeWithoutChangingOpenWindows(
            QDir(storage.configurationDirectory()).filePath(QStringLiteral("config.json")));
    });
    timed("Localized layout controls", localizedLayoutControlsUpdateAndRememberSettings);
    timed("Custom grid initialization and persistence", [&] {
        customGridInitializesNearSquareAndPersists(
            QDir(storage.configurationDirectory()).filePath(QStringLiteral("config.json")));
    });
    timed("Export completion and history isolation",
          [&] { exportCompletionFailureCancellationAndHistoryIsolation(directory.path()); });
#ifdef Q_OS_WIN
    timed("Native clipboard limits", nativeClipboardLimitsRejectBeforePreparation);
#endif
    timed("Completed file save", [&] { completedFileSaveUsesExistingWriter(directory.path()); });
    timed("Save result lifetime", [&] { saveResultDefersCollageCompletion(directory.path()); });
    timed("Destroyed and closed callback owners", destroyedAndClosedWindowsIgnorePendingCallbacks);
    timed("Native save service",
          [&] { nativeSaveServiceCompletesAndCancelsExactlyOnce(directory.path()); });
    waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
              "collage tests drain export jobs");
    ScreenshotExportCoordinator::shared().shutdown();
    storage.shutdown();
    std::cout << "Collage window tests passed\n";
    return 0;
}
