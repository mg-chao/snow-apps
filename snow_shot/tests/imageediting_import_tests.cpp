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
#include "physical_key_test_support.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColorSpace>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
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

QImage clipboardImage(QSize size = QSize(50, 40)) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(QColor(40, 120, 200, 128));
    image.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    image.setDevicePixelRatio(2);
    return image;
}
void requireImage(const ImageEditingWindow& window, const QImage& expected) {
    const auto& actual = window.document()->image();
    require(actual.size() == expected.size() && actual.colorSpace() == expected.colorSpace() &&
                actual.convertToFormat(QImage::Format_RGBA8888) ==
                    expected.convertToFormat(QImage::Format_RGBA8888),
            "pasting retains physical pixels, alpha and color profile");
}
void keyPaste(QWidget* receiver, bool repeat = false, bool focusReceiver = true) {
    if (focusReceiver) {
        receiver->window()->activateWindow();
        receiver->setFocus();
        QApplication::processEvents();
        require(receiver->hasFocus(), "paste receiver has keyboard focus");
    }
    const auto key = QKeySequence::keyBindings(QKeySequence::Paste).first()[0];
    PhysicalKeyEvent press(QEvent::KeyPress, key.key(), key.keyboardModifiers(), {}, repeat);
    QApplication::sendEvent(receiver, &press);
    PhysicalKeyEvent release(QEvent::KeyRelease, key.key(), key.keyboardModifiers());
    QApplication::sendEvent(receiver, &release);
}
adqt::widgets::AdModal* discardDialog(ImageEditingWindow& window) {
    for (auto* dialog : window.findChildren<adqt::widgets::AdModal*>())
        if (dialog != window.modal() && dialog->isOpen())
            return dialog;
    return nullptr;
}
void encodedClipboard(const QByteArray& bytes) {
    auto* mime = new QMimeData;
    mime->setData(QStringLiteral("image/png"), bytes);
    mime->setHtml(QStringLiteral("<b>Do not import this fallback</b>"));
    mime->setText(QStringLiteral("Do not import this fallback"));
    QApplication::clipboard()->setMimeData(mime);
}

void clipboardPixelsAndReplacement() {
    ImageEditingWindow window;
    window.presentEmpty();
    const auto first = clipboardImage();
    QApplication::clipboard()->setImage(first);
    auto* add = surface(window)->findChild<AdButton*>(QStringLiteral("imageEditingAddImages"));
    keyPaste(add);
    require(window.loading(), "Paste in an empty editor starts asynchronous import");
    const auto second = clipboardImage(QSize(70, 60));
    QApplication::clipboard()->setImage(second);
    window.paste();
    loaded(window);
    requireImage(window, first);
    require(!window.dirty() && window.canvas()->isVisible(),
            "pasted image opens a clean editable document and ignores busy Paste");

    require(window.document()->crop(ImageEditingDocument::CropAxis::Rows, 2, 5),
            "edit the pasted image");
    const auto checkpoint = window.contentCheckpoint();
    window.paste();
    awaitCondition([&] { return discardDialog(window) != nullptr; },
                   "pasted replacement confirms unsaved edits");
    require(window.contentCheckpoint() == checkpoint,
            "decoding replacement preserves the existing document");
    window.paste();
    discardDialog(window)->reject();
    awaitCondition([&] { return !window.loading(); }, "replacement cancellation completes");
    require(window.contentCheckpoint() == checkpoint && window.dirty(),
            "Continue editing preserves the image and crop history");
    window.paste();
    awaitCondition([&] { return discardDialog(window) != nullptr; }, "replacement can be retried");
    discardDialog(window)->accept();
    loaded(window);
    requireImage(window, second);
    require(!window.dirty() && !window.document()->undo(),
            "accepted replacement starts a clean independent editing history");

    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && first.save(&buffer, "PNG"),
            "encode clipboard fixture");
    encodedClipboard(bytes);
    window.paste();
    QApplication::clipboard()->clear();
    loaded(window);
    requireImage(window, first);
    require(window.document()->crop(ImageEditingDocument::CropAxis::Columns, 2, 5),
            "edit image before a failed clipboard replacement");
    const auto unchanged = window.contentCheckpoint();
    encodedClipboard(QByteArray("corrupt PNG"));
    window.paste();
    awaitCondition([&] { return !window.loading(); }, "corrupt clipboard import completes");
    require(window.contentCheckpoint() == unchanged && status(window) && window.dirty() &&
                !discardDialog(window),
            "corrupt image reports an error without importing text or losing existing pixels");
    auto* text = new QMimeData;
    text->setText(QStringLiteral("plain text"));
    text->setHtml(QStringLiteral("<img src='https://example.com/remote.png'>"));
    QApplication::clipboard()->setMimeData(text);
    window.paste();
    require(!window.loading() && window.contentCheckpoint() == unchanged,
            "text, HTML and remote image links do not become editor images");
    window.shutdown();
}

void clipboardFilesAndShortcutScope(const QString& first, const QString& second,
                                    const QString& broken) {
    ImageEditingWindowCoordinator coordinator({});
    coordinator.presentFiles({});
    auto* window = coordinator.windows().first().data();
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second),
                   QUrl(QStringLiteral("https://example.com/remote.png"))});
    QApplication::clipboard()->setMimeData(mime);
    window->paste();
    awaitCondition([&] { return coordinator.windows().size() == 2; },
                   "pasted image files open separate editor windows");
    loaded(*window);
    loaded(*coordinator.windows()[1]);
    require(window->document()->snapshot().originalFileName == QStringLiteral("first.png") &&
                coordinator.windows()[1]->document()->snapshot().originalFileName ==
                    QStringLiteral("second.PNG"),
            "pasted files retain order and export filenames");

    QLineEdit input(surface(*window));
    input.show();
    QApplication::clipboard()->setText(QStringLiteral("text paste"));
    keyPaste(&input);
    require(input.text() == QStringLiteral("text paste") && !window->loading(),
            "editable controls keep ordinary text Paste");
    const auto image = clipboardImage();
    QApplication::clipboard()->setImage(image);
    keyPaste(&input);
    require(!window->loading(), "image Paste does not intercept an editable field");
    keyPaste(window->canvas(), true);
    require(!window->loading(), "automatic key repeats do not paste images");
    QWidget unrelated;
    unrelated.setFocusPolicy(Qt::StrongFocus);
    unrelated.show();
    keyPaste(&unrelated);
    require(!window->loading() && !coordinator.windows()[1]->loading(),
            "Paste in unrelated windows does not import into an editor");

    keyPaste(window->canvas());
    require(window->loading() && !coordinator.windows()[1]->loading(),
            "Paste targets only the receiving editor");
    loaded(*window);
    requireImage(*window, image);
    const auto replacement = clipboardImage(QSize(65, 45));
    QApplication::clipboard()->setImage(replacement);
    auto* crop = window->palette()->findChild<AdButton*>(QStringLiteral("imageEditingCropRows"));
    require(crop && crop->isVisible() && crop->isEnabled(),
            "floating toolbar exposes a focusable drawing control");
    keyPaste(crop, false, false);
    require(window->loading(), "Paste delivered through the floating toolbar targets its editor");
    loaded(*window);
    requireImage(*window, replacement);
    const auto checkpoint = window->contentCheckpoint();
    mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(broken), QUrl::fromLocalFile(second)});
    QApplication::clipboard()->setMimeData(mime);
    window->paste();
    awaitCondition([&] { return coordinator.windows().size() == 3 && !window->loading(); },
                   "a corrupt first file does not prevent other pasted files from opening");
    loaded(*coordinator.windows()[2]);
    require(window->contentCheckpoint() == checkpoint && status(*window),
            "a corrupt pasted file preserves the receiving editor");
    coordinator.shutdown();
}

void clipboardBusyAndShutdown() {
    std::function<void(QStringList)> choose;
    std::function<void(bool)> pin;
    ImageEditingServices services;
    services.chooseFiles = [&](QWidget*, auto done) { choose = std::move(done); };
    services.pin = [&](QImage, auto done) {
        pin = std::move(done);
        return true;
    };
    ImageEditingWindow window(services);
    window.presentEmpty();
    QApplication::clipboard()->setImage(clipboardImage());
    surface(window)->findChild<AdButton*>(QStringLiteral("imageEditingAddImages"))->click();
    require(bool(choose), "image chooser remains pending");
    window.paste();
    require(!window.loading() && window.document()->image().isNull(),
            "file selection blocks image Paste");
    choose({});
    window.paste();
    loaded(window);
    const auto checkpoint = window.contentCheckpoint();
    window.palette()->pinRequested();
    awaitCondition([&] { return bool(pin); }, "image export remains pending");
    window.paste();
    require(!window.loading(), "active export blocks image Paste");
    pin(false);
    QApplication::clipboard()->setImage(clipboardImage(QSize(80, 70)));
    window.paste();
    require(window.loading(), "Paste is restored after export cancellation");
    window.shutdown();
    window.paste();
    QApplication::processEvents();
    require(window.contentCheckpoint() == checkpoint && !window.modal()->isOpen(),
            "shutdown ignores Paste and late clipboard completion");
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
    clipboardPixelsAndReplacement();
    clipboardFilesAndShortcutScope(first, second, broken);
    clipboardBusyAndShutdown();
    QApplication::clipboard()->clear();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
