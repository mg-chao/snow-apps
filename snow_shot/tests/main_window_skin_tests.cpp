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
#include <array>
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

void allSkinPositions() {
    QImage wide(120, 60, QImage::Format_ARGB32_Premultiplied);
    QImage tall(60, 120, QImage::Format_ARGB32_Premultiplied);
    const std::array<QColor, 3> colors = {Qt::red, Qt::green, Qt::blue};
    for (int y = 0; y < wide.height(); ++y) {
        for (int x = 0; x < wide.width(); ++x)
            wide.setPixelColor(x, y, colors[static_cast<std::size_t>(x / 40)]);
    }
    for (int y = 0; y < tall.height(); ++y) {
        for (int x = 0; x < tall.width(); ++x)
            tall.setPixelColor(x, y, colors[static_cast<std::size_t>(y / 40)]);
    }
    const std::array<QString, 9> names = {
        QStringLiteral("top_left"),    QStringLiteral("top_center"),
        QStringLiteral("top_right"),   QStringLiteral("center_left"),
        QStringLiteral("center"),      QStringLiteral("center_right"),
        QStringLiteral("bottom_left"), QStringLiteral("bottom_center"),
        QStringLiteral("bottom_right")};
    QImage rounded(101, 53, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < rounded.height(); ++y) {
        for (int x = 0; x < rounded.width(); ++x)
            rounded.setPixelColor(x, y, QColor(x * 2, y * 4, 40));
    }
    const auto roundedCenter = presentation::prepareMainWindowSkin(
        rounded, QSize(61, 67), 1.5, presentation::MainWindowSkinDisplayMode::Contain, 4);
    for (int i = 0; i < 9; ++i) {
        const auto position = static_cast<presentation::SkinPosition>(i);
        require(presentation::skinPositionFromString(names[static_cast<std::size_t>(i)]) ==
                    position,
                "every persisted skin position must map to its corresponding anchor");
        const auto horizontal = presentation::prepareMainWindowSkin(
            wide, QSize(60, 60), 1.0, presentation::MainWindowSkinDisplayMode::Overlay, 0, nullptr,
            position);
        const auto vertical = presentation::prepareMainWindowSkin(
            tall, QSize(60, 60), 1.0, presentation::MainWindowSkinDisplayMode::Overlay, 0, nullptr,
            position);
        require(horizontal.image.pixelColor(30, 30) == colors[static_cast<std::size_t>(i % 3)] &&
                    vertical.image.pixelColor(30, 30) == colors[static_cast<std::size_t>(i / 3)],
                "overlay anchors must select the corresponding horizontal and vertical crop");
        const auto horizontalContain = presentation::prepareMainWindowSkin(
            wide, QSize(60, 60), 1.0, presentation::MainWindowSkinDisplayMode::Contain, 0, nullptr,
            position);
        const auto verticalContain = presentation::prepareMainWindowSkin(
            tall, QSize(60, 60), 1.0, presentation::MainWindowSkinDisplayMode::Contain, 0, nullptr,
            position);
        require(horizontalContain.normalizedPlacement == QRectF(0, (i / 3) * 0.25, 1, 0.5) &&
                    verticalContain.normalizedPlacement == QRectF((i % 3) * 0.25, 0, 0.5, 1),
                "contain anchors must position the whole image within the available letterbox");
        const auto roundedAnchor = presentation::prepareMainWindowSkin(
            rounded, QSize(61, 67), 1.5, presentation::MainWindowSkinDisplayMode::Contain, 4,
            nullptr, position);
        require(roundedAnchor.image == roundedCenter.image,
                "contain positioning must preserve identical blurred pixels at fractional sizes");
    }
    require(presentation::skinPositionFromString(QStringLiteral("invalid")) ==
                presentation::SkinPosition::Center,
            "unknown skin positions must safely retain the centered appearance");
}

void invisibleSkinsSkipRendering(const QTemporaryDir& directory) {
    const QString path = directory.filePath(QStringLiteral("invisible.png"));
    writeImage(path, Qt::red);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValues({{QStringLiteral("interface/skin_path"), path},
                                     {QStringLiteral("interface/toolbar_skin_path"), path},
                                     {QStringLiteral("interface/tray_menu_skin_path"), path},
                                     {QStringLiteral("interface/skin_opacity"), 0},
                                     {QStringLiteral("interface/skin_mask_opacity"), 100},
                                     {QStringLiteral("interface/skin_blur_level"), 20}}),
            "configure invisible skins on every surface");
    presentation::MainWindowSkinController controller;
    std::array<QObject, 3> views;
    for (std::size_t i = 0; i < views.size(); ++i) {
        controller.attach(&views[i], static_cast<presentation::SkinSurface>(i), QSize(96, 64), 1.0);
    }
    waitUntil([&] { return !controller.diagnostics().busy; });
    const auto requireNoResources = [&] {
        const auto counts = controller.diagnostics();
        require(counts.retainedBytes == 0 && counts.scratchRetainedBytes == 0 &&
                    counts.executorCount == 0 && counts.idleFrameCount == 0,
                "invisible skins must release all decoded, prepared and executor resources");
        for (auto& view : views) {
            require(!controller.skinActive(&view) && controller.frame(&view).image.isNull() &&
                        controller.pixmap(&view).isNull(),
                    "invisible skin views must publish no frame");
        }
    };
    require(controller.diagnostics().decodeJobs == 0 &&
                controller.diagnostics().preparationJobs == 0 &&
                controller.diagnostics().pixmapConversions == 0,
            "initial zero opacity must not submit any image work");
    requireNoResources();
    for (const int mask : {0, 50, 100}) {
        require(configuration.setValues({{QStringLiteral("interface/skin_mask_opacity"), mask},
                                         {QStringLiteral("interface/skin_blur_level"), mask}}),
                "edit invisible skin effects");
        controller.reload();
        for (auto& view : views) {
            controller.setViewport(&view, QSize(100 + mask, 80 + mask), 2.0, true);
        }
    }
    require(controller.diagnostics().decodeJobs == 0 &&
                controller.diagnostics().preparationJobs == 0,
            "invisible effect and viewport changes must remain inert");
    controller.validate(presentation::SkinSurface::MainWindow);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().decodeJobs == 1 &&
                controller.diagnostics().preparationJobs == 0 && !controller.hasError(),
            "explicit validation may decode invisible skins without preparing a raster");
    requireNoResources();
    controller.validate(presentation::SkinSurface::MainWindow);
    require(controller.diagnostics().decodeJobs == 1,
            "invisible validation must cache its status without retaining image pixels");
    require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 100),
            "restore visible skin opacity");
    controller.reload();
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.skinActive(&views[0]) && controller.skinActive(&views[1]) &&
                controller.skinActive(&views[2]),
            "restoring opacity must reactivate every attached surface");
    require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 0),
            "disable the active skin");
    controller.reload();
    waitUntil([&] { return !controller.diagnostics().busy; });
    requireNoResources();
    const auto stopped = controller.diagnostics();
    for (auto& view : views) {
        controller.setViewport(&view, QSize(350, 240), 1.5, true);
        controller.detach(&view);
        controller.attach(&view, static_cast<presentation::SkinSurface>(&view - views.data()),
                          QSize(350, 240), 1.5);
    }
    require(controller.diagnostics().decodeJobs == stopped.decodeJobs &&
                controller.diagnostics().preparationJobs == stopped.preparationJobs &&
                controller.diagnostics().pixmapConversions == stopped.pixmapConversions,
            "reopening invisible surfaces must schedule no image work");
    requireNoResources();

    // Cancel synchronously at submission, before queued completion can publish a frame.
    controller.reload(presentation::SkinSurface::MainWindow);
    bool cancelled = false;
    const auto cancel = QObject::connect(
        &controller, &presentation::MainWindowSkinController::statusChanged, &controller, [&] {
            if (!cancelled && controller.opacity() > 0.0 && controller.diagnostics().busy) {
                cancelled = true;
                require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 0),
                        "disable a running skin job from its loading notification");
                controller.reload();
                controller.validate(presentation::SkinSurface::MainWindow);
            }
        });
    require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 100),
            "begin an interrupted opacity restoration");
    controller.reload();
    require(cancelled, "the active rendering job must be interrupted deterministically");
    waitUntil([&] { return !controller.diagnostics().busy; });
    QObject::disconnect(cancel);
    requireNoResources();
    require(controller.diagnostics().decodeJobs == stopped.decodeJobs + 2 && !controller.hasError(),
            "cancelled rendering must preserve queued explicit zero-opacity validation");

    controller.reload(presentation::SkinSurface::MainWindow);
    bool rapidlyRestored = false;
    const auto restore = QObject::connect(
        &controller, &presentation::MainWindowSkinController::statusChanged, &controller, [&] {
            if (!rapidlyRestored && controller.opacity() > 0.0 && controller.diagnostics().busy) {
                rapidlyRestored = true;
                require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 0),
                        "cancel the next rendering job");
                controller.reload();
                controller.validate(presentation::SkinSurface::MainWindow);
                require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 100),
                        "restore opacity before the cancelled job completes");
                controller.reload();
            }
        });
    require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 100),
            "reactivate after an interrupted load");
    controller.reload();
    require(rapidlyRestored, "rapid opacity restoration must interrupt a running job");
    waitUntil([&] { return !controller.diagnostics().busy; });
    QObject::disconnect(restore);
    require(controller.skinActive(&views[0]) && controller.skinActive(&views[1]) &&
                controller.skinActive(&views[2]),
            "rapid reactivation must publish current frames and preserve queued validation");
    require(configuration.setValues({{QStringLiteral("interface/skin_path"), QString()},
                                     {QStringLiteral("interface/toolbar_skin_path"), QString()},
                                     {QStringLiteral("interface/tray_menu_skin_path"), QString()},
                                     {QStringLiteral("interface/skin_blur_level"), 0},
                                     {QStringLiteral("interface/skin_mask_opacity"), 80}}),
            "restore the skin defaults after invisible lifecycle checks");
    controller.reload();
    waitUntil([&] { return !controller.diagnostics().busy; });
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

    controller.detach(&view);
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

void independentProfilesAndViews(const QTemporaryDir& directory) {
    const QString mainPath = directory.filePath(QStringLiteral("profiles-main.png"));
    const QString toolbarPath = directory.filePath(QStringLiteral("profiles-toolbar.png"));
    const QString trayPath = directory.filePath(QStringLiteral("profiles-tray.png"));
    writeImage(mainPath, Qt::red);
    writeImage(toolbarPath, Qt::blue);
    writeImage(trayPath, Qt::green);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValues(
                {{QStringLiteral("interface/skin_path"), mainPath},
                 {QStringLiteral("interface/toolbar_skin_path"), toolbarPath},
                 {QStringLiteral("interface/tray_menu_skin_path"), mainPath},
                 {QStringLiteral("interface/skin_position"), QStringLiteral("center")},
                 {QStringLiteral("interface/toolbar_skin_position"), QStringLiteral("center")},
                 {QStringLiteral("interface/tray_menu_skin_position"), QStringLiteral("center")},
                 {QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                 {QStringLiteral("interface/skin_blur_level"), 0},
                 {QStringLiteral("interface/skin_opacity"), 100}}),
            "configure three independent skin profiles with a shared source");
    presentation::MainWindowSkinController controller;
    require(controller.diagnostics().decodeJobs == 0,
            "configured profiles must stay lazy until their first visible view");
    QObject mainView;
    QObject firstToolbar;
    QObject secondToolbar;
    QObject toolbarPopover;
    QObject trayView;
    controller.attach(&mainView, presentation::SkinSurface::MainWindow, QSize(96, 64), 1.0);
    controller.attach(&firstToolbar, presentation::SkinSurface::Toolbar, QSize(96, 32), 1.0);
    controller.attach(&secondToolbar, presentation::SkinSurface::Toolbar, QSize(160, 48), 2.0);
    controller.attach(&toolbarPopover, presentation::SkinSurface::Toolbar, QSize(96, 32), 1.0);
    controller.attach(&trayView, presentation::SkinSurface::TrayMenu, QSize(80, 120), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().decodeJobs == 2 &&
                controller.diagnostics().preparationJobs == 4 &&
                controller.diagnostics().pixmapConversions == 4,
            "simultaneous profiles must share decoding and identical prepared rasters");
    require(controller.frame(&mainView).image.size() == QSize(96, 64) &&
                controller.frame(&firstToolbar).image.size() == QSize(96, 32) &&
                controller.frame(&secondToolbar).image.size() == QSize(320, 96) &&
                controller.frame(&trayView).image.size() == QSize(80, 120),
            "each registered view must preserve its own viewport and device pixel ratio");
    require(controller.frame(&mainView).image.pixelColor(30, 30) == Qt::red &&
                controller.frame(&firstToolbar).image.pixelColor(30, 20) == Qt::blue &&
                controller.frame(&trayView).image.pixelColor(30, 30) == Qt::red &&
                controller.pixmap(&firstToolbar).cacheKey() ==
                    controller.pixmap(&toolbarPopover).cacheKey(),
            "surface images must be independent and identical toolbar rasters must share pixmaps");
    const auto painted = controller.diagnostics();
    require(configuration.setValues({{QStringLiteral("interface/skin_opacity"), 60},
                                     {QStringLiteral("interface/skin_mask_opacity"), 35}}),
            "edit shared paint-only skin settings");
    waitUntil([&] { return controller.opacity() == 0.6 && controller.maskOpacity() == 0.35; });
    require(controller.diagnostics().preparationJobs == painted.preparationJobs &&
                controller.diagnostics().decodeJobs == painted.decodeJobs,
            "shared opacity and mask edits must not prepare or decode any profile");
    require(configuration.setValue(QStringLiteral("interface/skin_display_mode"),
                                   QStringLiteral("contain")),
            "switch shared display mode to contain");
    waitUntil([&] {
        return controller.frame(&firstToolbar).normalizedPlacement.width() < 1.0 &&
               !controller.diagnostics().busy;
    });
    const auto contained = controller.diagnostics();
    const auto containedPixmap = controller.pixmap(&firstToolbar).cacheKey();
    require(configuration.setValue(QStringLiteral("interface/toolbar_skin_position"),
                                   QStringLiteral("bottom_right")),
            "change only the toolbar anchor");
    waitUntil([&] { return controller.frame(&firstToolbar).normalizedPlacement.x() == 0.5; });
    require(controller.diagnostics().preparationJobs == contained.preparationJobs &&
                controller.pixmap(&firstToolbar).cacheKey() == containedPixmap &&
                controller.frame(&mainView).normalizedPlacement == QRectF(0, 0, 1, 1),
            "contain anchor changes must move placement without preparation or affecting other "
            "profiles");
    const auto beforeReopen = controller.diagnostics();
    controller.detach(&toolbarPopover);
    controller.attach(&toolbarPopover, presentation::SkinSurface::Toolbar, QSize(96, 32), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().preparationJobs == beforeReopen.preparationJobs &&
                controller.diagnostics().cacheHits > beforeReopen.cacheHits,
            "reopening a popover must reuse its prepared frame");
    require(configuration.setValue(QStringLiteral("interface/tray_menu_skin_path"), trayPath),
            "select a separate tray image");
    controller.reload(presentation::SkinSurface::TrayMenu);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.frame(&trayView).image.pixelColor(10, 10) == Qt::green &&
                controller.frame(&mainView).image.pixelColor(10, 10) == Qt::red &&
                controller.frame(&firstToolbar).image.pixelColor(10, 10) == Qt::blue,
            "changing a tray image must preserve the main interface and toolbar frames");
    require(configuration.setValue(QStringLiteral("interface/tray_menu_skin_path"),
                                   directory.filePath(QStringLiteral("missing-tray.png"))),
            "configure a failing tray image");
    controller.reload(presentation::SkinSurface::TrayMenu);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.hasError(presentation::SkinSurface::TrayMenu) &&
                !controller.hasError(presentation::SkinSurface::MainWindow) &&
                !controller.hasError(presentation::SkinSurface::Toolbar) &&
                !controller.skinActive(&trayView) && controller.skinActive(&mainView) &&
                controller.skinActive(&firstToolbar),
            "a tray decode failure must restore only that surface's normal theme");
    require(configuration.setValue(QStringLiteral("interface/skin_path"),
                                   directory.filePath(QStringLiteral("missing-tray.png"))),
            "select the previously failed shared tray source for the main interface");
    waitUntil([&] { return controller.hasError(presentation::SkinSurface::MainWindow); });
    require(!controller.skinActive(&mainView) && controller.skinActive(&firstToolbar),
            "selecting a cached failed source must clear the old main frame without affecting "
            "toolbars");
    require(configuration.setValue(QStringLiteral("interface/skin_path"), mainPath),
            "restore the valid main-interface source");
    controller.reload(presentation::SkinSurface::MainWindow);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(configuration.setValue(QStringLiteral("interface/toolbar_skin_path"), QString()),
            "clear only the toolbar image");
    controller.reload(presentation::SkinSurface::Toolbar);
    require(
        !controller.skinActive(&firstToolbar) && !controller.skinActive(&secondToolbar) &&
            !controller.skinActive(&toolbarPopover) && controller.skinActive(&mainView),
        "clearing a profile must immediately clear all its views without affecting other surfaces");
    controller.detach(&mainView);
    controller.detach(&firstToolbar);
    controller.detach(&secondToolbar);
    controller.detach(&toolbarPopover);
    controller.detach(&trayView);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().executorCount == 0 &&
                controller.diagnostics().scratchRetainedBytes == 0,
            "detaching the final view must asynchronously release all executor resources");
    require(configuration.setValues({{QStringLiteral("interface/skin_path"), QString()},
                                     {QStringLiteral("interface/toolbar_skin_path"), QString()},
                                     {QStringLiteral("interface/tray_menu_skin_path"), QString()}}),
            "clear all profiles after the multi-view checks");
    controller.reload();
}

void attachmentListenersCanDestroyTheView(const QTemporaryDir& directory) {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValues({{QStringLiteral("interface/skin_path"), QString()},
                                     {QStringLiteral("interface/toolbar_skin_path"), QString()},
                                     {QStringLiteral("interface/tray_menu_skin_path"), QString()}}),
            "start pre-attachment reentrancy checks without a configured skin");
    presentation::MainWindowSkinController controller;
    const QString path = directory.filePath(QStringLiteral("pre-attachment.png"));
    writeImage(path, Qt::yellow);
    QPointer<QObject> view = new QObject;
    bool destroyed = false;
    QObject::connect(&controller, &presentation::MainWindowSkinController::statusChanged,
                     &controller, [&] {
                         if (destroyed)
                             return;
                         destroyed = true;
                         delete view.data();
                     });
    require(configuration.setValue(QStringLiteral("interface/skin_path"), path),
            "change configuration before registering a new view");
    controller.attach(view, presentation::SkinSurface::MainWindow, QSize(96, 64), 1.0);
    require(destroyed && !view && controller.diagnostics().decodeJobs == 0 &&
                !controller.diagnostics().executorAllocated,
            "configuration listeners may destroy a view before attachment without a dangling "
            "registration");
    require(configuration.setValue(QStringLiteral("interface/skin_path"), QString()),
            "clear the pre-attachment fixture");
    controller.reload();
}

void destroyedViewsReleaseActiveFrames(const QTemporaryDir& directory) {
    const QString path = directory.filePath(QStringLiteral("destroyed-view.png"));
    writeImage(path, Qt::yellow);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValues(
                {{QStringLiteral("interface/skin_path"), path},
                 {QStringLiteral("interface/toolbar_skin_path"), QString()},
                 {QStringLiteral("interface/tray_menu_skin_path"), QString()},
                 {QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                 {QStringLiteral("interface/skin_blur_level"), 0}}),
            "configure a main view whose QObject lifetime drives detachment");
    presentation::MainWindowSkinController controller;
    auto* view = new QObject;
    controller.attach(view, presentation::SkinSurface::MainWindow, QSize(96, 64), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.skinActive(), "the registered main view must have an active legacy frame");
    delete view;
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(!controller.skinActive() && controller.diagnostics().idleFrameCount == 1 &&
                controller.diagnostics().executorCount == 0,
            "QObject destruction must release active frame handles after its QPointer has cleared");
    require(configuration.setValue(QStringLiteral("interface/skin_path"), QString()),
            "clear the destroyed-view fixture");
    controller.reload();
}

void queuedProfilesReportLoading(const QTemporaryDir& directory) {
    const QString mainPath = directory.filePath(QStringLiteral("queued-main.png"));
    const QString toolbarPath = directory.filePath(QStringLiteral("queued-toolbar.png"));
    const QString trayPath = directory.filePath(QStringLiteral("queued-tray.png"));
    writeImage(mainPath, Qt::red);
    writeImage(toolbarPath, Qt::blue);
    writeImage(trayPath, Qt::green);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValues(
                {{QStringLiteral("interface/skin_path"), mainPath},
                 {QStringLiteral("interface/toolbar_skin_path"), toolbarPath},
                 {QStringLiteral("interface/tray_menu_skin_path"), trayPath},
                 {QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                 {QStringLiteral("interface/skin_blur_level"), 0}}),
            "configure multiple undecoded sources without opening their views");
    presentation::MainWindowSkinController controller;
    require(controller.statusText(presentation::SkinSurface::MainWindow).isEmpty() &&
                controller.statusText(presentation::SkinSurface::Toolbar).isEmpty() &&
                controller.statusText(presentation::SkinSurface::TrayMenu).isEmpty() &&
                controller.diagnostics().decodeJobs == 0,
            "startup and configured profiles without views must retain lazy empty status");
    QObject mainView;
    QObject toolbarView;
    controller.attach(&mainView, presentation::SkinSurface::MainWindow, QSize(96, 64), 1.0);
    QString notifiedToolbarStatus;
    QObject::connect(
        &controller, &presentation::MainWindowSkinController::statusChanged, &controller,
        [&] { notifiedToolbarStatus = controller.statusText(presentation::SkinSurface::Toolbar); });
    // Without processing GUI callbacks, the first worker remains outstanding even
    // if it has already completed its small fixture off-thread.
    controller.attach(&toolbarView, presentation::SkinSurface::Toolbar, QSize(96, 32), 1.0);
    require(controller.diagnostics().decodeJobs == 1 &&
                controller.diagnostics().preparationJobs == 1 &&
                !controller.statusText(presentation::SkinSurface::Toolbar).isEmpty() &&
                !notifiedToolbarStatus.isEmpty() &&
                controller.statusText(presentation::SkinSurface::TrayMenu).isEmpty(),
            "a queued visible profile must notify Loading before its single-worker job starts");
    controller.detach(&toolbarView);
    require(
        controller.statusText(presentation::SkinSurface::Toolbar).isEmpty() &&
            notifiedToolbarStatus.isEmpty(),
        "detaching the last queued view must notify cancellation of its pending loading status");
    controller.attach(&toolbarView, presentation::SkinSurface::Toolbar, QSize(96, 32), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().decodeJobs == 2 &&
                controller.statusText(presentation::SkinSurface::MainWindow).isEmpty() &&
                controller.statusText(presentation::SkinSurface::Toolbar).isEmpty(),
            "queued visible sources must finish once while untouched profiles remain undecoded");
    controller.detach(&mainView);
    controller.detach(&toolbarView);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(configuration.setValues({{QStringLiteral("interface/skin_path"), QString()},
                                     {QStringLiteral("interface/toolbar_skin_path"), QString()},
                                     {QStringLiteral("interface/tray_menu_skin_path"), QString()}}),
            "clear queued-source fixtures");
    controller.reload();
}

void explicitValidationWithoutViews(const QTemporaryDir& directory) {
    const QString path = directory.filePath(QStringLiteral("validated-toolbar.png"));
    const QString missing = directory.filePath(QStringLiteral("validated-missing-tray.png"));
    writeImage(path, Qt::blue);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValues(
                {{QStringLiteral("interface/skin_path"), QString()},
                 {QStringLiteral("interface/toolbar_skin_path"), path},
                 {QStringLiteral("interface/tray_menu_skin_path"), path},
                 {QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                 {QStringLiteral("interface/skin_blur_level"), 100}}),
            "configure shared hidden sources for explicit validation");
    presentation::MainWindowSkinController controller;
    controller.reload(presentation::SkinSurface::Toolbar);
    require(
        controller.diagnostics().decodeJobs == 0 && !controller.diagnostics().busy,
        "configuration reads and legacy reload must stay lazy without a view or validation intent");
    controller.validate(presentation::SkinSurface::Toolbar);
    controller.validate(presentation::SkinSurface::TrayMenu);
    require(controller.diagnostics().decodeJobs == 1 &&
                controller.diagnostics().preparationJobs == 0 &&
                controller.diagnostics().pixmapConversions == 0 &&
                !controller.statusText(presentation::SkinSurface::Toolbar).isEmpty() &&
                !controller.statusText(presentation::SkinSurface::TrayMenu).isEmpty(),
            "shared hidden validation must submit one decode without preparing any raster");
    QString notifiedTrayStatus;
    QObject::connect(
        &controller, &presentation::MainWindowSkinController::statusChanged, &controller,
        [&] { notifiedTrayStatus = controller.statusText(presentation::SkinSurface::TrayMenu); });
    require(configuration.setValue(QStringLiteral("interface/tray_menu_skin_path"), missing),
            "select a separate hidden tray source behind the running validation");
    controller.validate(presentation::SkinSurface::TrayMenu);
    require(controller.diagnostics().decodeJobs == 1 && controller.diagnostics().busy &&
                !notifiedTrayStatus.isEmpty() &&
                !controller.hasError(presentation::SkinSurface::TrayMenu),
            "queued explicit validation must notify Loading before the source reaches the worker");
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().decodeJobs == 2 &&
                controller.diagnostics().preparationJobs == 0 &&
                controller.diagnostics().pixmapConversions == 0 &&
                controller.diagnostics().executorCount == 0 &&
                controller.diagnostics().scratchRetainedBytes == 0 &&
                !controller.hasError(presentation::SkinSurface::Toolbar) &&
                controller.hasError(presentation::SkinSurface::TrayMenu),
            "hidden validation must report independent failures and retire all worker resources");
    const auto validated = controller.diagnostics();
    controller.validate(presentation::SkinSurface::Toolbar);
    controller.validate(presentation::SkinSurface::TrayMenu);
    require(controller.diagnostics().decodeJobs == validated.decodeJobs &&
                !controller.diagnostics().busy,
            "validation must reuse both successful and failed source caches");
    QObject toolbar;
    controller.attach(&toolbar, presentation::SkinSurface::Toolbar, QSize(96, 32), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().decodeJobs == validated.decodeJobs &&
                controller.diagnostics().preparationJobs == 1 &&
                controller.frame(&toolbar).image.pixelColor(10, 10) == Qt::blue,
            "the first visible view must prepare from its validated source without decoding again");
    controller.detach(&toolbar);
    waitUntil([&] { return !controller.diagnostics().busy; });

    controller.reload(presentation::SkinSurface::Toolbar);
    controller.validate(presentation::SkinSurface::Toolbar);
    const auto beforeClear = controller.diagnostics();
    require(configuration.setValue(QStringLiteral("interface/toolbar_skin_path"), QString()),
            "clear a hidden profile while explicit validation is running");
    controller.reload(presentation::SkinSurface::Toolbar);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.statusText(presentation::SkinSurface::Toolbar).isEmpty() &&
                !controller.hasError(presentation::SkinSurface::Toolbar) &&
                controller.diagnostics().retainedBytes == 0 &&
                controller.diagnostics().staleResults > beforeClear.staleResults,
            "a late validation result must not restore a cleared source or its cache");

    require(configuration.setValue(QStringLiteral("interface/toolbar_skin_path"), path),
            "reselect a hidden source for explicit same-path reload checks");
    const auto beforeReload = controller.diagnostics();
    controller.validate(presentation::SkinSurface::Toolbar);
    writeImage(path, Qt::cyan);
    controller.reload(presentation::SkinSurface::Toolbar);
    controller.validate(presentation::SkinSurface::Toolbar);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().decodeJobs == beforeReload.decodeJobs + 2 &&
                controller.diagnostics().preparationJobs == beforeReload.preparationJobs &&
                controller.diagnostics().executorCount == 0,
            "same-path reload must replace an in-flight validation with one current decode-only "
            "intent");
    const auto beforeReattach = controller.diagnostics();
    controller.attach(&toolbar, presentation::SkinSurface::Toolbar, QSize(96, 32), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.frame(&toolbar).image.pixelColor(10, 10) == Qt::cyan &&
                controller.diagnostics().decodeJobs == beforeReattach.decodeJobs,
            "a stale validation must not replace the current same-path image reused by a view");
    controller.detach(&toolbar);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(configuration.setValues({{QStringLiteral("interface/toolbar_skin_path"), QString()},
                                     {QStringLiteral("interface/tray_menu_skin_path"), QString()}}),
            "clear all hidden validation fixtures");
    controller.reload(presentation::SkinSurface::Toolbar);
}

void boundedIdleFrameCache(const QTemporaryDir& directory) {
    const QString path = directory.filePath(QStringLiteral("idle-cache.png"));
    writeImage(path, Qt::cyan);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValues(
                {{QStringLiteral("interface/skin_path"), QString()},
                 {QStringLiteral("interface/toolbar_skin_path"), path},
                 {QStringLiteral("interface/tray_menu_skin_path"), QString()},
                 {QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                 {QStringLiteral("interface/skin_blur_level"), 0}}),
            "configure a toolbar profile for bounded idle-cache checks");
    presentation::MainWindowSkinController controller;
    QObject first;
    QObject second;
    controller.attach(&first, presentation::SkinSurface::Toolbar, QSize(3000, 2000), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    controller.detach(&first);
    waitUntil([&] { return !controller.diagnostics().busy; });
    controller.attach(&second, presentation::SkinSurface::Toolbar, QSize(3001, 2000), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    controller.detach(&second);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().idleFrameBytes <= 64LL * 1024 * 1024,
            "idle prepared frames and shared pixmaps must stay within the 64 MiB LRU budget");
    const auto beforeEvictedReopen = controller.diagnostics();
    controller.attach(&first, presentation::SkinSurface::Toolbar, QSize(3000, 2000), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().preparationJobs == beforeEvictedReopen.preparationJobs + 1 &&
                controller.diagnostics().decodeJobs == 1,
            "reopening an evicted raster must prepare again while reusing its decoded source");
    controller.detach(&first);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(configuration.setValue(QStringLiteral("interface/toolbar_skin_path"), QString()),
            "clear the idle-cache fixture");
    controller.reload(presentation::SkinSurface::Toolbar);
    require(controller.diagnostics().retainedBytes == 0,
            "clearing the final profile must release source and idle raster caches");
}

void boundedTinyFrameCache(const QTemporaryDir& directory) {
    const QString path = directory.filePath(QStringLiteral("tiny-idle-cache.png"));
    writeImage(path, Qt::magenta);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    require(configuration.setValues(
                {{QStringLiteral("interface/skin_path"), QString()},
                 {QStringLiteral("interface/toolbar_skin_path"), path},
                 {QStringLiteral("interface/tray_menu_skin_path"), QString()},
                 {QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                 {QStringLiteral("interface/skin_blur_level"), 0}}),
            "configure tiny idle frames to exercise the metadata and lookup bound");
    presentation::MainWindowSkinController controller;
    QObject active;
    QObject transient;
    controller.attach(&active, presentation::SkinSurface::Toolbar, QSize(1, 1), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    const auto activePixmap = controller.pixmap(&active).cacheKey();
    for (int width = 2; width <= 130; ++width) {
        controller.attach(&transient, presentation::SkinSurface::Toolbar, QSize(width, 1), 1.0);
        waitUntil([&] { return !controller.diagnostics().busy; });
        controller.detach(&transient);
    }
    require(controller.diagnostics().idleFrameCount == 128 &&
                controller.diagnostics().idleFrameBytes <= 64LL * 1024 * 1024 &&
                controller.pixmap(&active).cacheKey() == activePixmap,
            "idle-entry limits must bound tiny-frame metadata while preserving every active frame");
    const auto beforeReopen = controller.diagnostics();
    controller.attach(&transient, presentation::SkinSurface::Toolbar, QSize(2, 1), 1.0);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(controller.diagnostics().preparationJobs == beforeReopen.preparationJobs + 1 &&
                controller.diagnostics().decodeJobs == 1,
            "the oldest tiny idle raster must be evicted independently of its decoded source");
    controller.detach(&transient);
    controller.detach(&active);
    waitUntil([&] { return !controller.diagnostics().busy; });
    require(configuration.setValue(QStringLiteral("interface/toolbar_skin_path"), QString()),
            "clear the tiny idle-cache fixture");
    controller.reload(presentation::SkinSurface::Toolbar);
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
                suspended.scratchRetainedBytes == 0 && !current->skinActive() &&
                suspended.idleFrameBytes > 0,
            "suspension should release active handles and keep its frame only in the bounded idle "
            "cache");
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

    controller.detach(&view);
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
    invisibleSkinsSkipRendering(temporary);
    imageGeometryAndBlur();
    allSkinPositions();
    lazyLoadingAndReset(temporary);
    controllerCachingAndLifetime(temporary);
    singletonRetirementAndRapidReenable(temporary);
    signalListenersCanCancelOrClose(temporary);
    independentProfilesAndViews(temporary);
    attachmentListenersCanDestroyTheView(temporary);
    destroyedViewsReleaseActiveFrames(temporary);
    queuedProfilesReportLoading(temporary);
    explicitValidationWithoutViews(temporary);
    boundedIdleFrameCache(temporary);
    boundedTinyFrameCache(temporary);
    appStorage.shutdown();
    return 0;
}
