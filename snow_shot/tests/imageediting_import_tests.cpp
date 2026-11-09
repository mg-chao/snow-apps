// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/components/imagedropsurface.h"
#include "widgets/button.h"
#include "widgets/message.h"
#include "widgets/modal.h"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMimeData>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QScopeGuard>
#include <atomic>
#include <stdexcept>

namespace {
using namespace snow_shot::presentation;
using adqt::widgets::AdButton;
using adqt::widgets::AdMessageService;
using Error = snow_shot::platform::SelectedFileError;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void awaitCondition(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(condition(), message);
}
QWidget* surface(ImageEditingWindow& window) {
    return window.canvas()->parentWidget();
}
QWidget* emptyState(ImageEditingWindow& window) {
    return surface(window)->findChild<QWidget*>(QStringLiteral("imageEditingEmptyState"));
}
adqt::widgets::AdMessageHandle* status(ImageEditingWindow& window) {
    for (auto* handle : AdMessageService::instance(surface(window)->window())
                            ->findChildren<adqt::widgets::AdMessageHandle*>())
        if (handle->isOpen() && handle->key() == QStringLiteral("imageEditingStatus"))
            return handle;
    return nullptr;
}
void loaded(ImageEditingWindow& window) {
    awaitCondition([&] { return !window.loading(); }, "file import finishes");
    require(!window.document()->image().isNull(), "file import loads pixels");
}
void drop(ImageEditingWindow& window, const QStringList& paths) {
    QMimeData mime;
    QList<QUrl> urls;
    for (const auto& path : paths)
        urls.append(QUrl::fromLocalFile(path));
    mime.setUrls(urls);
    QDragEnterEvent enter(QPointF(50, 50), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(surface(window), &enter);
    require(enter.isAccepted(), "empty editor accepts local image drag");
    auto* hint = surface(window)->findChild<QWidget*>(QStringLiteral("imageEditingDropHint"));
    require(hint && hint->isVisible(), "image drag displays the shared drop hint");
    QDropEvent event(QPointF(50, 50), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(surface(window), &event);
    require(event.isAccepted() && !hint->isVisible(), "drop starts import and dismisses its hint");
}

class SelectedFiles final : public snow_shot::platform::SelectedFileBackend {
  public:
    snow_shot::platform::SelectedFileResult result;
    mutable std::atomic_int calls = 0;
    mutable std::atomic_bool entered = false;
    mutable std::atomic_bool cancelled = false;
    mutable std::atomic_bool correctThreadAndTarget = false;
    std::atomic_bool block = false;
    snow_shot::platform::SelectedFileTarget captureTarget() const override {
        require(QThread::currentThread() == qApp->thread(), "capture selection on GUI thread");
        return {11, 22, 33, 44, true};
    }
    snow_shot::platform::SelectedFileResult
    selectedFiles(const snow_shot::platform::SelectedFileTarget& target,
                  const std::function<bool()>& isCancelled) const override {
        ++calls;
        correctThreadAndTarget = QThread::currentThread() != qApp->thread() &&
                                 target.window == 11 && target.view == 22 && target.tab == 33 &&
                                 target.processId == 44 && target.desktop;
        entered = true;
        while (block && !isCancelled())
            QThread::msleep(1);
        cancelled = isCancelled();
        return result;
    }
};

void previewBackgroundFollowsImageAvailability() {
    auto& theme = styles::ThemeManager::instance();
    const auto originalTheme = theme.themeMode();
    const auto restoreTheme = qScopeGuard([&] { theme.setThemeMode(originalTheme); });
    const auto background = [](ImageEditingWindow& window) {
        QImage pixels(40, 40, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::transparent);
        surface(window)->render(&pixels, QPoint(), QRegion(pixels.rect()),
                                QWidget::DrawWindowBackground);
        return pixels;
    };
    for (const auto mode : {styles::ThemeMode::Light, styles::ThemeMode::Dark}) {
        theme.setThemeMode(mode);
        ImageEditingWindow window;
        window.presentEmpty();
        {
            QWidget reference(surface(window)->parentWidget());
            reference.resize(surface(window)->size());
            components::ImageDropSurface referenceDrop(false, &reference);
            referenceDrop.setGeometry(reference.rect());
            referenceDrop.show();
            reference.show();
            const auto render = [](QWidget* widget, QWidget::RenderFlags flags, qreal scale) {
                QImage pixels(
                    QSize(qRound(widget->width() * scale), qRound(widget->height() * scale)),
                    QImage::Format_ARGB32_Premultiplied);
                pixels.setDevicePixelRatio(scale);
                pixels.fill(Qt::transparent);
                widget->render(&pixels, QPoint(), QRegion(), flags);
                return pixels;
            };
            for (const qreal scale : {1.0, 1.5, 2.0}) {
                require(render(surface(window), QWidget::DrawWindowBackground, scale) ==
                            render(&reference, QWidget::DrawWindowBackground, scale),
                        "empty preview inherits its container background like image stitching");
                const auto actual = render(
                    surface(window), QWidget::DrawWindowBackground | QWidget::DrawChildren, scale);
                const auto expected = render(
                    &reference, QWidget::DrawWindowBackground | QWidget::DrawChildren, scale);
                const int side = qRound(40 * scale);
                for (const auto& corner :
                     {QPoint(), QPoint(actual.width() - side, 0), QPoint(0, actual.height() - side),
                      QPoint(actual.width() - side, actual.height() - side)}) {
                    const QRect patch(corner, QSize(side, side));
                    require(
                        actual.copy(patch) == expected.copy(patch),
                        "empty preview fill, dashed border and rounded corners match stitching's "
                        "shared drop surface in both themes at every DPI scale");
                }
            }
        }
        const auto directory = qEnvironmentVariable("SNOW_IMAGE_EDITING_EMPTY_SCREENSHOT_DIR");
        if (!directory.isEmpty()) {
            require(QDir().mkpath(directory), "create empty editor inspection directory");
            const auto filename = mode == styles::ThemeMode::Light ? QStringLiteral("light.png")
                                                                   : QStringLiteral("dark.png");
            require(surface(window)->window()->grab().save(QDir(directory).filePath(filename)),
                    "save empty editor inspection image");
        }
        ImageEditingSource source;
        source.image = QImage(80, 60, QImage::Format_ARGB32);
        source.image.fill(Qt::transparent);
        source.canvasRect = QRectF(QPointF(), QSizeF(source.image.size()));
        window.present(std::move(source));
        const auto loaded = background(window);
        require(loaded.pixelColor(5, 5) != loaded.pixelColor(15, 5),
                "loading an image restores the transparency checkerboard");
    }
}

void emptyAndChooser(const QString& first, const QString& second) {
    std::function<void(QStringList)> choose;
    QImage exportedImage;
    ImageEditingServices services;
    services.pin = [&](QImage image, auto complete) {
        exportedImage = std::move(image);
        complete(true);
        return true;
    };
    services.chooseFiles = [&](QWidget* owner, auto completion) {
        require(owner != nullptr, "chooser belongs to the editor");
        choose = std::move(completion);
    };
    ImageEditingWindowCoordinator coordinator(services);
    coordinator.presentFiles({});
    auto* window = coordinator.windows().first().data();
    require(window->modal()->isOpen() && emptyState(*window)->isVisible() && !window->dirty() &&
                !window->canvas()->isVisible() && !window->palette()->window()->isVisible() &&
                !status(*window),
            "empty editor shows import UI without editing controls or an error");
    auto* add = surface(*window)->findChild<AdButton*>(QStringLiteral("imageEditingAddImages"));
    require(add && add->isVisible() && add->isEnabled(),
            "empty editor has an accessible add button");
    QMimeData unsupported;
    unsupported.setUrls({QUrl(QStringLiteral("https://example.com/image.png")),
                         QUrl::fromLocalFile(first + QStringLiteral(".txt"))});
    QDragEnterEvent invalid(QPointF(50, 50), Qt::CopyAction, &unsupported, Qt::LeftButton,
                            Qt::NoModifier);
    QApplication::sendEvent(surface(*window), &invalid);
    require(!invalid.isAccepted() && !window->loading(),
            "empty editor rejects remote URLs and unsupported file types");
    auto& languages = LanguageManager::instance();
    languages.initialize();
    for (const auto* locale : {"zh_CN", "zh_TW", "en_US"}) {
        require(languages.setLanguage(QString::fromLatin1(locale)), "switch empty editor language");
        QApplication::processEvents();
        require(add->text() == ImageEditingWindow::tr("Add images") &&
                    add->accessibleName() == add->text() &&
                    emptyState(*window)->accessibleName() ==
                        ImageEditingWindow::tr("Add images to start image editing"),
                "empty-state copy and accessibility follow runtime language changes");
        if (QString::fromLatin1(locale) != QStringLiteral("en_US"))
            require(add->text() != QStringLiteral("Add images"),
                    "Chinese catalogs translate import UI");
    }
    auto& theme = styles::ThemeManager::instance();
    const auto originalTheme = theme.themeMode();
    theme.setThemeMode(styles::ThemeMode::Dark);
    require(!emptyState(*window)->grab().isNull(), "empty drop surface renders in dark theme");
    theme.setThemeMode(originalTheme);
    add->click();
    require(bool(choose), "add images opens chooser");
    choose({});
    require(!window->loading() && emptyState(*window)->isVisible() && !window->dirty(),
            "chooser cancellation preserves a clean empty editor");
    add->click();
    choose({first, second});
    awaitCondition([&] { return coordinator.windows().size() == 2; },
                   "chooser opens one window per image");
    loaded(*window);
    loaded(*coordinator.windows()[1]);
    require(!emptyState(*window)->isVisible() && window->canvas()->isVisible() &&
                window->palette()->window()->isVisible() && !window->dirty() &&
                window->document()->snapshot().originalFileName == QStringLiteral("first.png"),
            "successful import restores editing with a clean checkpoint and export filename");
    require(window->document()->crop(ImageEditingDocument::CropAxis::Rows, 2, 5) &&
                window->dirty() && !coordinator.windows()[1]->dirty(),
            "imported windows keep independent edits");
    bool exported = false;
    QObject::connect(window, &ImageEditingWindow::exportFinished, window,
                     [&](bool success) { exported = success; });
    window->palette()->pinRequested();
    awaitCondition([&] { return exported; }, "imported image exports through normal toolbar");
    require(exportedImage.size() == window->document()->image().size(),
            "pin exports the edited image");
    const auto checkpoint = window->contentCheckpoint();
    window->presentFile(first + QStringLiteral(".missing"));
    awaitCondition([&] { return !window->loading(); }, "failed replacement completes");
    require(window->contentCheckpoint() == checkpoint && window->canvas()->isVisible() &&
                status(*window),
            "failed later load preserves the existing edited document");
    coordinator.shutdown();
}

void selectionAndDrop(const QString& first, const QString& second, const QString& broken,
                      const QString& folder) {
    auto backend = std::make_shared<SelectedFiles>();
    backend->result.paths = {first, folder, first, second, broken, first + QStringLiteral(".txt")};
    ImageEditingWindowCoordinator coordinator({});
    coordinator.presentSelectedFiles(backend, backend->captureTarget());
    auto* firstWindow = coordinator.windows().first().data();
    require(firstWindow->modal()->isOpen() && firstWindow->loading(),
            "selection opens editor immediately");
    awaitCondition([&] { return coordinator.windows().size() == 3; },
                   "supported unique files get separate editors");
    const auto windows = coordinator.windows();
    loaded(*windows[0]);
    loaded(*windows[1]);
    awaitCondition([&] { return !windows[2]->loading(); }, "corrupt image import finishes");
    require(backend->calls == 1 && backend->correctThreadAndTarget &&
                windows[0]->document()->image().size() == QSize(100, 80) &&
                windows[1]->document()->image().size() == QSize(60, 50) &&
                windows[2]->document()->image().isNull() && emptyState(*windows[2])->isVisible() &&
                status(*windows[2]),
            "selection retains order and corrupt images leave recoverable empty windows");
    drop(*windows[2], {second, first});
    awaitCondition([&] { return coordinator.windows().size() == 4; },
                   "multi-file drop opens additional editor");
    loaded(*windows[2]);
    loaded(*coordinator.windows()[3]);
    require(windows[2]->document()->image().size() == QSize(60, 50) && !status(*windows[2]),
            "drop recovers failed editor and clears its error");
    QMimeData mime;
    mime.setUrls({QUrl(QStringLiteral("https://example.com/image.png"))});
    QDragEnterEvent remote(QPointF(50, 50), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(surface(*windows[0]), &remote);
    require(!remote.isAccepted(), "loaded editor rejects file replacement drops");
    coordinator.shutdown();
}

void failedSelectionsAndClose(const QString& first) {
    for (const auto error : {Error::None, Error::PermissionDenied, Error::Timeout,
                             Error::Unavailable, Error::QueryFailed}) {
        auto backend = std::make_shared<SelectedFiles>();
        backend->result.error = error;
        ImageEditingWindowCoordinator coordinator({});
        coordinator.presentSelectedFiles(backend, backend->captureTarget());
        auto* window = coordinator.windows().first().data();
        awaitCondition([&] { return !window->loading(); }, "empty or failed selection finishes");
        require(emptyState(*window)->isVisible() && !window->dirty() &&
                    bool(status(*window)) == (error != Error::None),
                "lookup failure leaves a usable empty editor");
        if (error == Error::QueryFailed) {
            auto& languages = LanguageManager::instance();
            require(languages.setLanguage(QStringLiteral("zh_CN")), "translate selection failure");
            QApplication::processEvents();
            require(status(*window)->content() ==
                        QCoreApplication::translate(
                            "ImageEditingWindow",
                            "Could not read the selected image files. Please try again"),
                    "selection error follows runtime language changes");
            require(languages.setLanguage(QStringLiteral("en_US")), "restore language");
        }
        drop(*window, {first});
        loaded(*window);
        coordinator.shutdown();
    }
    {
        auto backend = std::make_shared<SelectedFiles>();
        ImageEditingWindowCoordinator coordinator({});
        coordinator.presentSelectedFiles(backend, {});
        auto* window = coordinator.windows().first().data();
        require(backend->calls == 0 && !window->loading() && !status(*window),
                "unavailable tray target opens empty editor without querying");
        window->requestClose();
        require(coordinator.windows().isEmpty(),
                "empty editor closes without discard confirmation");
    }
    for (bool shutdown : {false, true}) {
        auto backend = std::make_shared<SelectedFiles>();
        backend->block = true;
        backend->result.paths = {first, first};
        ImageEditingWindowCoordinator coordinator({});
        coordinator.presentSelectedFiles(backend, backend->captureTarget());
        awaitCondition([&] { return backend->entered.load(); }, "blocked selection starts");
        if (shutdown)
            coordinator.shutdown();
        else
            coordinator.windows().first()->requestClose();
        awaitCondition([&] { return backend->cancelled.load(); }, "window lifecycle cancels query");
        QApplication::processEvents();
        require(coordinator.windows().isEmpty(), "late query completion cannot create editors");
    }
}
void importedWindowsParticipateInUpdateHandoff(const QString& first, const QString& second) {
    ImageEditingWindowCoordinator coordinator({});
    coordinator.presentFiles({first, second});
    awaitCondition([&] { return coordinator.windows().size() == 2; },
                   "multi-file import registers both windows with the coordinator");
    const auto windows = coordinator.windows();
    loaded(*windows[0]);
    loaded(*windows[1]);
    require(coordinator.prepareForUpdateHandoff(), "clean imported windows allow update handoff");
    require(windows[1]->document()->crop(ImageEditingDocument::CropAxis::Rows, 2, 5) &&
                !coordinator.prepareForUpdateHandoff(),
            "unsaved changes in an additional imported window block update handoff");
    delete windows[0].data();
    require(!windows[0] && coordinator.windows().size() == 1 &&
                coordinator.windows().first() == windows[1] &&
                !coordinator.prepareForUpdateHandoff(),
            "destroyed imported windows leave no stale entries or bypass remaining edits");
    coordinator.shutdown();
    require(coordinator.windows().isEmpty() && coordinator.prepareForUpdateHandoff(),
            "shutdown releases all imported windows from update admission");
}

void supersededFilesAndChooserLifetime(const QString& first, const QString& second) {
    ImageEditingWindow window;
    window.presentFile(first);
    window.presentFile(second);
    loaded(window);
    require(window.document()->image().size() == QSize(60, 50),
            "superseded file completion cannot replace the newer image");
    window.shutdown();

    ImageEditingWindowCoordinator pending({});
    pending.presentFiles({first, second});
    pending.windows().first()->requestClose();
    QApplication::processEvents();
    require(pending.windows().isEmpty(),
            "closing during file validation prevents additional windows");

    std::function<void(QStringList)> complete;
    ImageEditingServices services;
    services.chooseFiles = [&](QWidget*, auto callback) { complete = std::move(callback); };
    ImageEditingWindowCoordinator choosing(services);
    choosing.presentFiles({});
    auto* empty = choosing.windows().first().data();
    surface(*empty)->findChild<AdButton*>(QStringLiteral("imageEditingAddImages"))->click();
    require(bool(complete), "chooser completion can remain pending");
    choosing.shutdown();
    complete({first, second});
    QApplication::processEvents();
    require(choosing.windows().isEmpty(), "late chooser completion cannot reopen a closed editor");
}
} // namespace

void runImageEditingImportTests() {
    QTemporaryDir directory;
    require(directory.isValid(), "create image import fixtures");
    const auto first = directory.filePath(QStringLiteral("first.png"));
    const auto second = directory.filePath(QStringLiteral("second.PNG"));
    const auto broken = directory.filePath(QStringLiteral("broken.png"));
    const auto folder = directory.filePath(QStringLiteral("folder.png"));
    QImage image(100, 80, QImage::Format_ARGB32);
    image.fill(Qt::red);
    require(image.save(first), "write first import image");
    image = QImage(60, 50, QImage::Format_ARGB32);
    image.fill(Qt::green);
    require(image.save(second) && QDir().mkpath(folder),
            "write second image and image-like folder");
    QFile corrupt(broken);
    require(corrupt.open(QIODevice::WriteOnly) && corrupt.write("not an image") > 0,
            "write corrupt image");
    corrupt.close();
    previewBackgroundFollowsImageAvailability();
    emptyAndChooser(first, second);
    selectionAndDrop(first, second, broken, folder);
    failedSelectionsAndClose(first);
    importedWindowsParticipateInUpdateHandoff(first, second);
    supersededFilesAndChooserLifetime(first, second);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
