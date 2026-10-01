#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/storage/applicationstorage.h"
#include "../src/image/snowimageqtcodec.h"
#include "widgets/context_menu.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace {
namespace presentation = snow_shot::presentation;

QJsonObject distribution(QVector<double> values) {
    std::sort(values.begin(), values.end());
    return {{QStringLiteral("median_ms"), values.at(values.size() / 2)},
            {QStringLiteral("p95_ms"), values.at((values.size() - 1) * 95 / 100)}};
}

bool settle() {
    QElapsedTimer timeout;
    timeout.start();
    do {
        QCoreApplication::processEvents();
        auto* controller = presentation::MainWindowSkinController::existingInstance();
        if (!controller || !controller->diagnostics().busy) {
            return true;
        }
        QThread::msleep(1);
    } while (timeout.elapsed() < 15000);
    return false;
}

QJsonObject measure(QWidget& widget, int samples, const std::function<bool()>& isReady = {}) {
    auto* menu = qobject_cast<adqt::widgets::AdContextMenu*>(&widget);
    const auto showSurface = [&] {
        if (menu) {
            menu->popupAt(menu->pos());
        } else {
            widget.show();
        }
    };
    showSurface();
    if (!settle()) {
        return {};
    }
    if (isReady && !isReady()) {
        std::cerr << "A measured surface must have an active skin frame\n";
        return {};
    }
    QImage rendered(widget.size(), QImage::Format_ARGB32_Premultiplied);
    QVector<double> paintTimes;
    QVector<double> showTimes;
    for (int index = -5; index < samples; ++index) {
        QElapsedTimer timer;
        timer.start();
        QPainter painter(&rendered);
        widget.render(&painter);
        painter.end();
        const double paintTime = double(timer.nsecsElapsed()) / 1000000.0;
        widget.hide();
        QCoreApplication::processEvents();
        timer.restart();
        showSurface();
        QCoreApplication::processEvents();
        const double showTime = double(timer.nsecsElapsed()) / 1000000.0;
        if (!settle()) {
            return {};
        }
        if (isReady && !isReady()) {
            std::cerr << "Reopening a measured surface must restore its skin frame\n";
            return {};
        }
        if (index >= 0) {
            paintTimes.append(paintTime);
            showTimes.append(showTime);
        }
    }
    return {{QStringLiteral("cached_paint"), distribution(paintTimes)},
            {QStringLiteral("reopen"), distribution(showTimes)}};
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    const auto arguments = application.arguments();
    const auto outputIndex = arguments.indexOf(QStringLiteral("--output"));
    const auto samplesIndex = arguments.indexOf(QStringLiteral("--samples"));
    const int samples = samplesIndex < 0 ? 100 : arguments.value(samplesIndex + 1).toInt();
    if (outputIndex < 0 || arguments.value(outputIndex + 1).isEmpty() || samples < 5) {
        std::cerr << "Usage: skin surface benchmark --output report.json [--samples 100]\n";
        return EXIT_FAILURE;
    }
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!temporary.isValid() ||
        !storage.initialize({temporary.path(), temporary.path(), 60000}).success) {
        return EXIT_FAILURE;
    }
    std::array<std::unique_ptr<ScreenshotToolbarPanel>, 4> rows;
    const std::array<QSize, 4> sizes{QSize(640, 40), QSize(420, 40), QSize(360, 40),
                                     QSize(560, 40)};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        rows[index] = std::make_unique<ScreenshotToolbarPanel>();
        rows[index]->setGraphicsEffect(nullptr);
        rows[index]->resize(sizes[index]);
        rows[index]->show();
    }
    adqt::widgets::AdContextMenu menu;
    menu.setNativeMenuEnabled(false);
    for (int index = 0; index < 8; ++index) {
        menu.addAction(QStringLiteral("Tray action %1").arg(index));
    }
    menu.resize(menu.sizeHint());
    menu.popupAt(menu.pos());
    QCoreApplication::processEvents();
    QJsonObject report;
    QJsonObject disabled;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto measurement = measure(*rows[index], samples);
        if (measurement.isEmpty()) {
            return EXIT_FAILURE;
        }
        disabled.insert(QStringLiteral("toolbar_%1").arg(qulonglong(index)), measurement);
    }
    const auto disabledMenu = measure(menu, samples);
    if (disabledMenu.isEmpty()) {
        return EXIT_FAILURE;
    }
    disabled.insert(QStringLiteral("tray_menu"), disabledMenu);
    disabled.insert(QStringLiteral("controller_allocated"),
                    presentation::MainWindowSkinController::existingInstance() != nullptr);
    report.insert(QStringLiteral("disabled"), disabled);

#ifndef SNOW_SKIN_BENCHMARK_BASELINE
    QImage source(1920, 1080, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            line[x] = qRgb(x % 256, y % 256, (x + y) % 256);
        }
    }
    const QString path = temporary.filePath(QStringLiteral("skin.png"));
    QFile fixture(path);
    const auto encoded = snow_shot::image_codec::encodePng(source);
    if (!fixture.open(QIODevice::WriteOnly) || fixture.write(encoded) != encoded.size()) {
        return EXIT_FAILURE;
    }
    fixture.close();
    if (!storage.configuration().setValues({{QStringLiteral("interface/toolbar_skin_path"), path},
                                            {QStringLiteral("interface/tray_menu_skin_path"), path},
                                            {QStringLiteral("interface/skin_blur_level"), 16}})) {
        return EXIT_FAILURE;
    }
    auto& controller = presentation::MainWindowSkinController::instance();
    const auto syncMenu = [&] {
        menu.setBackgroundFrame({controller.pixmap(&menu),
                                 controller.frame(&menu).normalizedPlacement, controller.opacity(),
                                 controller.maskOpacity()});
    };
    QObject::connect(&controller, &presentation::MainWindowSkinController::viewFrameChanged, &menu,
                     [&](QObject* view) {
                         if (view == &menu) {
                             syncMenu();
                         }
                     });
    QObject::connect(&menu, &QMenu::aboutToShow, &menu, [&] {
        controller.attach(&menu, presentation::SkinSurface::TrayMenu, menu.size(),
                          menu.devicePixelRatioF());
        syncMenu();
    });
    QObject::connect(&menu, &QMenu::aboutToHide, &menu, [&] {
        controller.detach(&menu);
        menu.resetBackgroundFrame();
    });
    controller.attach(&menu, presentation::SkinSurface::TrayMenu, menu.size(),
                      menu.devicePixelRatioF());
    if (!settle()) {
        return EXIT_FAILURE;
    }
    const auto warm = controller.diagnostics();
    QJsonObject enabled;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto measurement = measure(*rows[index], samples,
                                         [&] { return controller.skinActive(rows[index].get()); });
        if (measurement.isEmpty()) {
            return EXIT_FAILURE;
        }
        enabled.insert(QStringLiteral("toolbar_%1").arg(qulonglong(index)), measurement);
    }
    const auto enabledMenu = measure(menu, samples, [&] {
        return controller.skinActive(&menu) && !menu.backgroundFrame().image.isNull();
    });
    if (enabledMenu.isEmpty()) {
        return EXIT_FAILURE;
    }
    enabled.insert(QStringLiteral("tray_menu"), enabledMenu);
    const auto cached = controller.diagnostics();
    enabled.insert(QStringLiteral("decode_jobs"), double(cached.decodeJobs));
    enabled.insert(QStringLiteral("preparation_jobs"), double(cached.preparationJobs));
    enabled.insert(QStringLiteral("cached_decode_jobs"),
                   double(cached.decodeJobs - warm.decodeJobs));
    enabled.insert(QStringLiteral("cached_preparation_jobs"),
                   double(cached.preparationJobs - warm.preparationJobs));
    enabled.insert(QStringLiteral("pixmap_conversions"), double(cached.pixmapConversions));
    enabled.insert(QStringLiteral("cached_pixmap_conversions"),
                   double(cached.pixmapConversions - warm.pixmapConversions));
    enabled.insert(QStringLiteral("warm_retained_bytes"), double(warm.retainedBytes));
    enabled.insert(QStringLiteral("retained_bytes"), double(cached.retainedBytes));
    enabled.insert(QStringLiteral("retained_growth_bytes"),
                   double(cached.retainedBytes - warm.retainedBytes));
    enabled.insert(QStringLiteral("idle_frame_bytes"), double(cached.idleFrameBytes));
    enabled.insert(QStringLiteral("warm_executor_count"), warm.executorCount);
    enabled.insert(QStringLiteral("executor_count"), cached.executorCount);
    enabled.insert(QStringLiteral("warm_scratch_retained_bytes"),
                   double(warm.scratchRetainedBytes));
    enabled.insert(QStringLiteral("scratch_retained_bytes"), double(cached.scratchRetainedBytes));
    report.insert(QStringLiteral("enabled"), enabled);
    controller.detach(&menu);
#endif
    QFile output(arguments.value(outputIndex + 1));
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() ? EXIT_SUCCESS
                                                                                    : EXIT_FAILURE;
}
