#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/presentation/mainwindowskinwidget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snowimageqtcodec.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>

namespace {
namespace presentation = snow_shot::presentation;
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void waitUntil(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (condition()) {
            return;
        }
        QThread::msleep(1);
    } while (timer.elapsed() < 15000);
    require(false, "skin operation must complete within the test timeout");
}

void writeImage(const QString& path, const QColor& color) {
    QImage image(120, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "open the skin fixture");
    const auto bytes = snow_shot::image_codec::encodePng(image);
    require(!bytes.isEmpty() && file.write(bytes) == bytes.size(), "write the PNG skin fixture");
}

void imageGeometryAndBlur() {
    QImage source(120, 60, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, x < 40 ? Qt::red : x < 80 ? Qt::green : Qt::blue);
        }
    }
    const auto overlay = presentation::prepareMainWindowSkin(
        source, QSize(60, 60), 1.0, presentation::MainWindowSkinDisplayMode::Overlay, 0);
    require(overlay.image.size() == QSize(60, 60) &&
                overlay.normalizedPlacement == QRectF(0, 0, 1, 1) &&
                overlay.image.pixelColor(1, 30) == QColor(Qt::red) &&
                overlay.image.pixelColor(30, 30) == QColor(Qt::green) &&
                overlay.image.pixelColor(58, 30) == QColor(Qt::blue),
            "Overlay must preserve proportions and center the cropped source");
    const auto contain = presentation::prepareMainWindowSkin(
        source, QSize(60, 60), 1.0, presentation::MainWindowSkinDisplayMode::Contain, 0);
    require(contain.image.size() == QSize(60, 30) &&
                contain.normalizedPlacement == QRectF(0, 0.25, 1, 0.5) &&
                contain.image.pixelColor(1, 15) == QColor(Qt::red) &&
                contain.image.pixelColor(58, 15) == QColor(Qt::blue),
            "Contain must preserve the whole image and center its placement");
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
        const auto sharp = presentation::prepareMainWindowSkin(
            source, QSize(60, 60), dpr, presentation::MainWindowSkinDisplayMode::Overlay, 0);
        const auto blurred = presentation::prepareMainWindowSkin(
            source, QSize(60, 60), dpr, presentation::MainWindowSkinDisplayMode::Overlay, 100);
        require(sharp.image.size() == QSize(qRound(60 * dpr), qRound(60 * dpr)) &&
                    blurred.image.size() == sharp.image.size() && blurred.image != sharp.image,
                "blur must be DPR-aware and preserve output dimensions");
        for (const auto corner : {QPoint(0, 0), QPoint(blurred.image.width() - 1, 0),
                                  QPoint(0, blurred.image.height() - 1)}) {
            require(blurred.image.pixelColor(corner).alpha() == 255,
                    "opaque skin edges must stay opaque at maximum blur");
        }
    }
    QImage transparent(40, 20, QImage::Format_ARGB32_Premultiplied);
    transparent.fill(Qt::transparent);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 20; ++x) {
            transparent.setPixelColor(x, y, QColor(255, 0, 0, 255));
        }
    }
    const auto alphaBlur = presentation::prepareMainWindowSkin(
        transparent, QSize(80, 40), 1.0, presentation::MainWindowSkinDisplayMode::Contain, 4);
    require(alphaBlur.image.pixelColor(39, 20).alpha() > 0 &&
                alphaBlur.image.pixelColor(39, 20).red() >= 250 &&
                alphaBlur.image.pixelColor(39, 20).green() == 0,
            "premultiplied blur must preserve color at transparent edges");
    QImage panorama(4096, 1, QImage::Format_ARGB32_Premultiplied);
    panorama.fill(Qt::cyan);
    const auto narrow = presentation::prepareMainWindowSkin(
        panorama, QSize(200, 200), 1.0, presentation::MainWindowSkinDisplayMode::Contain, 0);
    require(narrow.image.size() == QSize(200, 1), "extreme aspect ratios must retain one pixel");
    const auto bounded = presentation::prepareMainWindowSkin(
        source, QSize(10000, 10000), 2.0, presentation::MainWindowSkinDisplayMode::Overlay, 0);
    require(qint64(bounded.image.width()) * bounded.image.height() <= 16LL * 1000 * 1000,
            "large viewports must respect the retained-raster pixel budget");
    const auto thin = presentation::prepareMainWindowSkin(
        source, QSize(1000000, 1), 10.0, presentation::MainWindowSkinDisplayMode::Overlay, 0);
    require(qint64(thin.image.width()) * thin.image.height() <= 16LL * 1000 * 1000,
            "minimum one-pixel dimensions must not exceed the retained-raster budget");
}

void controllerCachingAndLifetime(const QTemporaryDir& directory) {
    const QString firstPath = directory.filePath(QStringLiteral("first.png"));
    const QString secondPath = directory.filePath(QStringLiteral("second.png"));
    writeImage(firstPath, Qt::red);
    writeImage(secondPath, Qt::blue);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    presentation::MainWindowSkinController controller;
    QObject view;
    controller.attach(&view, QSize(96, 64), 1.0);
    require(!controller.skinActive(), "an empty skin path must keep the theme appearance");
    require(configuration.setValue(QStringLiteral("interface/skin_path"), firstPath),
            "configure the first skin");
    controller.reload();
    require(!controller.statusText().isEmpty() && !controller.hasError(),
            "loading must have transient status without a configuration error");
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.skinActive() && controller.frame().image.pixelColor(30, 30) == Qt::red &&
                controller.diagnostics().decodeJobs == 1,
            "the controller must load the selected image once");
    const auto cached = controller.diagnostics();
    require(configuration.setValues({{QStringLiteral("interface/skin_opacity"), 25},
                                     {QStringLiteral("interface/skin_mask_opacity"), 50}}),
            "change paint-only preferences");
    waitUntil([&] { return controller.opacity() == 0.25 && controller.maskOpacity() == 0.5; });
    require(controller.diagnostics().decodeJobs == cached.decodeJobs &&
                controller.diagnostics().preparationJobs == cached.preparationJobs,
            "opacity and mask edits must not decode, scale or blur");
    for (int step = 0; step < 30; ++step) {
        controller.setViewport(&view, QSize(100 + step, 80 + step), 1.0);
    }
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().decodeJobs == cached.decodeJobs &&
                controller.diagnostics().preparationJobs == cached.preparationJobs + 1,
            "a resize burst must prepare one final frame without decoding again");
    const auto beforeBlur = controller.diagnostics();
    for (int level = 1; level <= 30; ++level) {
        require(configuration.setValue(QStringLiteral("interface/skin_blur_level"), level),
                "update a burst of blur preferences");
    }
    QCoreApplication::processEvents();
    require(controller.skinActive(), "blur preparation must retain the previous frame");
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().decodeJobs == beforeBlur.decodeJobs &&
                controller.diagnostics().preparationJobs == beforeBlur.preparationJobs + 1,
            "a blur burst must prepare one final frame and reuse the decoded source");
    controller.setViewport(&view, QSize(129, 109), 2.0, true);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.frame().image.size() == QSize(258, 218),
            "a DPR change must rebuild the physical raster");
    writeImage(firstPath, Qt::green);
    QCoreApplication::processEvents();
    require(controller.frame().image.pixelColor(30, 30) == Qt::red,
            "file edits must not automatically reload the skin");
    controller.reload();
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.frame().image.pixelColor(30, 30) == Qt::green,
            "explicit same-path reload must read the modified file");

    controller.reload();
    require(configuration.setValue(QStringLiteral("interface/skin_path"), secondPath),
            "supersede an in-flight request");
    controller.reload();
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.frame().image.pixelColor(30, 30) == Qt::blue &&
                controller.diagnostics().staleResults > 0,
            "a stale load must not replace the latest selection");
    const auto beforeBurst = controller.diagnostics();
    for (int selection = 0; selection < 30; ++selection) {
        require(configuration.setValue(QStringLiteral("interface/skin_path"),
                                       selection % 2 == 0 ? firstPath : secondPath),
                "replace a burst of skin selections");
        controller.reload();
    }
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.frame().image.pixelColor(30, 30) == Qt::blue &&
                controller.diagnostics().decodeJobs - beforeBurst.decodeJobs <= 2 &&
                controller.diagnostics().preparationJobs - beforeBurst.preparationJobs <= 2,
            "rapid selections must retain only one running and one replaceable pending request");

    auto* widget = new presentation::MainWindowSkinWidget(nullptr, &controller);
    widget->setBaseColor(QColor(240, 240, 240));
    widget->resize(129, 109);
    widget->show();
    waitUntil([&] { return !controller.diagnostics().busy; });
    const auto beforePaint = controller.diagnostics();
    const auto painted = widget->grab().toImage();
    require(!painted.isNull() && widget->skinActive() && widget->maskOpacity() == 0.5,
            "the skin widget must present the frame and independent mask opacity");
    for (int count = 0; count < 10; ++count) {
        widget->repaint();
    }
    require(controller.diagnostics().preparationJobs == beforePaint.preparationJobs,
            "ordinary paints must reuse the prepared frame");

    controller.reload();
    require(configuration.setValue(QStringLiteral("interface/skin_path"), QString()),
            "clear an in-flight selection");
    controller.reload();
    require(!widget->skinActive() && widget->maskOpacity() == 1.0,
            "clear must immediately restore fully opaque theme surfaces");
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(!controller.skinActive(), "a late result must not undo clear");

    require(configuration.setValue(QStringLiteral("interface/skin_path"),
                                   directory.filePath(QStringLiteral("missing.png"))),
            "configure a missing image");
    controller.reload();
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.hasError() && !controller.statusText().isEmpty() && !widget->skinActive() &&
                configuration.value(QStringLiteral("interface/skin_path"))
                    .toString()
                    .endsWith(QStringLiteral("missing.png")),
            "load errors must retain the path, report status and restore the normal theme");

    require(configuration.setValue(QStringLiteral("interface/skin_path"), firstPath),
            "start a load before deleting the window");
    controller.reload();
    QElapsedTimer deletion;
    deletion.start();
    delete widget;
    require(deletion.elapsed() < 100, "window destruction must not wait for the image executor");
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().retainedBytes <= 128LL * 1024 * 1024,
            "retained source and prepared caches must stay bounded");
}

void lazyLoadingAndReset(const QTemporaryDir& directory) {
    const QString path = directory.filePath(QStringLiteral("lazy.png"));
    writeImage(path, Qt::yellow);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValue(QStringLiteral("interface/skin_path"), path),
            "save a skin before opening the main interface");
    presentation::MainWindowSkinController controller;
    controller.reload();
    QCoreApplication::processEvents();
    require(controller.diagnostics().decodeJobs == 0 && !controller.diagnostics().busy,
            "a configured skin must not decode until the main interface attaches");
    QObject view;
    controller.attach(&view, QSize(96, 64), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.skinActive() && controller.diagnostics().decodeJobs == 1,
            "the first main-interface attachment must load the configured skin");
    controller.reload();
    require(configuration.setValues(
                {{QStringLiteral("interface/skin_path"), QString()},
                 {QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                 {QStringLiteral("interface/skin_opacity"), 100},
                 {QStringLiteral("interface/skin_blur_level"), 0},
                 {QStringLiteral("interface/skin_mask_opacity"), 80}}),
            "reset all skin preferences while processing");
    controller.reload();
    require(!controller.skinActive(), "reset must immediately restore the normal theme");
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(!controller.skinActive() && !controller.hasError(),
            "an in-flight result must not undo reset");
}
void singletonRetirementAndRapidReenable(const QTemporaryDir& directory) {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValue(QStringLiteral("interface/skin_path"), QString()),
            "start executor lifecycle checks with an empty skin path");
    require(presentation::MainWindowSkinController::existingInstance() == nullptr,
            "cold skin state must not construct an application controller");
    {
        presentation::MainWindowSkinController isolated;
        QObject view;
        isolated.attach(&view, QSize(96, 64), 1.0);
        isolated.setViewport(&view, QSize(120, 80), 2.0);
        const auto cold = isolated.diagnostics();
        require(!cold.executorAllocated && cold.executorCount == 0 &&
                    cold.scratchRetainedBytes == 0 && cold.decodeJobs == 0 &&
                    cold.preparationJobs == 0 && !cold.busy,
                "an isolated empty controller must not allocate executor or blur resources");
    }
    const QString path = directory.filePath(QStringLiteral("executor-lifecycle.png"));
    writeImage(path, Qt::magenta);
    require(configuration.setValues({{QStringLiteral("interface/skin_path"), path},
                                     {QStringLiteral("interface/skin_blur_level"), 24}}),
            "configure an image for singleton executor checks");
    QPointer<presentation::MainWindowSkinController> previous =
        &presentation::MainWindowSkinController::instance();
    require(!previous->diagnostics().executorAllocated,
            "a singleton must keep its executor lazy until a view requests processing");
    require(configuration.setValue(QStringLiteral("interface/skin_path"), QString()),
            "clear a singleton before its deferred deletion runs");
    previous->reload();
    require(presentation::MainWindowSkinController::existingInstance() == nullptr && previous,
            "a retired singleton must be hidden before QObject deferred deletion");
    require(configuration.setValue(QStringLiteral("interface/skin_path"), path),
            "re-enable the skin before the old singleton is deleted");
    QPointer<presentation::MainWindowSkinController> current =
        &presentation::MainWindowSkinController::instance();
    require(current != previous, "rapid re-enable must create a fresh controller");
    previous->reload();
    require(previous->diagnostics().decodeJobs == 0 && !previous->diagnostics().busy,
            "a retired singleton must ignore later configuration and reload requests");
    QObject view;
    current->attach(&view, QSize(320, 220), 1.0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(previous.isNull() && current,
            "deleting the retired singleton must preserve the replacement controller");
    waitUntil([&] { return !current->diagnostics().busy; });
    const auto active = current->diagnostics();
    require(current->skinActive() && active.executorAllocated && active.executorCount == 1 &&
                active.scratchRetainedBytes > 0 &&
                active.scratchRetainedBytes <= 16LL * 1024 * 1024,
            "an active blurred skin should own one executor and bounded dedicated scratch");
    QElapsedTimer suspension;
    suspension.start();
    current->detach(&view);
    require(suspension.elapsed() < 100, "suspending a skin must not wait for its executor");
    waitUntil([&] { return !current->diagnostics().busy; });
    const auto suspended = current->diagnostics();
    require(!suspended.executorAllocated && suspended.executorCount == 0 &&
                suspended.scratchRetainedBytes == 0 && current->skinActive(),
            "suspension should release executor and scratch while preserving the cached frame");
    current->attach(&view, QSize(320, 220), 1.0);
    waitUntil([&] { return !current->diagnostics().busy; });
    require(current->diagnostics().decodeJobs == active.decodeJobs,
            "reopening the skin should reuse its cached source after executor suspension");
    current->reload();
    require(configuration.setValue(QStringLiteral("interface/skin_path"), QString()),
            "clear a singleton while its executor is processing");
    QElapsedTimer clearing;
    clearing.start();
    current->reload();
    require(clearing.elapsed() < 100, "clear must retire executor work away from the GUI thread");
    waitUntil(
        [&] { return presentation::MainWindowSkinController::existingInstance() == nullptr; });
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(current.isNull(), "clear should retire the application controller after cleanup");
}

void signalListenersCanCancelOrClose(const QTemporaryDir& directory) {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValues({{QStringLiteral("interface/skin_path"), QString()},
                                     {QStringLiteral("interface/skin_blur_level"), 24}}),
            "start signal reentrancy checks without a configured skin");
    const QString path = directory.filePath(QStringLiteral("reentrant-lifecycle.png"));
    writeImage(path, Qt::cyan);
    presentation::MainWindowSkinController controller;
    QObject view;
    controller.attach(&view, QSize(320, 220), 1.0);

    bool cancelled = false;
    const auto cancelConnection = QObject::connect(
        &controller, &presentation::MainWindowSkinController::statusChanged, &controller, [&] {
            if (cancelled || controller.diagnostics().decodeJobs == 0 ||
                controller.statusText().isEmpty()) {
                return;
            }
            cancelled = true;
            require(configuration.setValue(QStringLiteral("interface/skin_path"), QString()),
                    "clear the skin synchronously from its loading notification");
            controller.reload();
        });
    require(configuration.setValue(QStringLiteral("interface/skin_path"), path),
            "start a load with a synchronous cancellation listener");
    controller.reload();
    require(cancelled, "loading notification must allow cancellation after work submission");
    waitUntil([&] { return !controller.diagnostics().busy; });
    const auto cancelledResources = controller.diagnostics();
    require(!controller.skinActive() && !cancelledResources.executorAllocated &&
                cancelledResources.executorCount == 0 &&
                cancelledResources.scratchRetainedBytes == 0 &&
                cancelledResources.retainedBytes == 0 && cancelledResources.staleResults > 0,
            "cancellation from loading must discard work and release all skin resources");
    QObject::disconnect(cancelConnection);

    auto* widget = new presentation::MainWindowSkinWidget(nullptr, &controller);
    widget->resize(320, 220);
    widget->show();
    const auto beforeClose = controller.diagnostics().decodeJobs;
    bool closed = false;
    const auto closeConnection = QObject::connect(
        &controller, &presentation::MainWindowSkinController::statusChanged, &controller, [&] {
            if (closed || controller.diagnostics().decodeJobs == beforeClose ||
                controller.statusText().isEmpty()) {
                return;
            }
            closed = true;
            QElapsedTimer deletion;
            deletion.start();
            delete widget;
            widget = nullptr;
            require(deletion.elapsed() < 100,
                    "closing from a loading notification must not wait for the executor");
        });
    require(configuration.setValue(QStringLiteral("interface/skin_path"), path),
            "start a load with a synchronous window-close listener");
    controller.reload();
    require(closed && widget == nullptr, "a loading listener must be able to destroy the view");
    waitUntil([&] { return !controller.diagnostics().busy; });
    const auto closedResources = controller.diagnostics();
    require(!closedResources.executorAllocated && closedResources.executorCount == 0 &&
                closedResources.scratchRetainedBytes == 0,
            "closing from loading must release its executor and dedicated scratch");
    QObject::disconnect(closeConnection);

    bool clearedFrame = false;
    QObject::connect(
        &controller, &presentation::MainWindowSkinController::frameChanged, &controller, [&] {
            if (clearedFrame || !controller.skinActive())
                return;
            clearedFrame = true;
            require(configuration.setValue(QStringLiteral("interface/skin_path"), QString()),
                    "clear synchronously from a completed frame notification");
            controller.reload();
        });
    controller.attach(&view, QSize(320, 220), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    const auto clearedResources = controller.diagnostics();
    require(clearedFrame && !controller.skinActive() && clearedResources.executorCount == 0 &&
                clearedResources.scratchRetainedBytes == 0 && clearedResources.retainedBytes == 0,
            "clear from frame completion must not retain a late source, frame or executor");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated skin fixtures");
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated skin settings");
    imageGeometryAndBlur();
    lazyLoadingAndReset(temporary);
    controllerCachingAndLifetime(temporary);
    singletonRetirementAndRapidReenable(temporary);
    signalListenersCanCancelOrClose(temporary);
    appStorage.shutdown();
    return 0;
}
