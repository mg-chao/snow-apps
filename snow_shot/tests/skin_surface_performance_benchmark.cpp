#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../src/image/snowimageqtcodec.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/context_menu.h"
#include "widgets/radio.h"
#include "widgets/select.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
namespace presentation = snow_shot::presentation;
namespace styles = presentation::styles;
using Diagnostics = presentation::MainWindowSkinDiagnostics;
constexpr int WARMUP_SAMPLES = 5;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

double milliseconds(const QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
}

QJsonObject distribution(QVector<double> values) {
    std::sort(values.begin(), values.end());
    return {{QStringLiteral("median_ms"), values.at(values.size() / 2)},
            {QStringLiteral("p95_ms"), values.at((values.size() - 1) * 95 / 100)}};
}

void settle() {
    QElapsedTimer timeout;
    timeout.start();
    do {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        const auto* controller = presentation::MainWindowSkinController::existingInstance();
        if (!controller || !controller->diagnostics().busy)
            return;
        QThread::msleep(1);
    } while (timeout.elapsed() < 15000);
    require(false, "skin surface operations must finish within the benchmark timeout");
}

Diagnostics diagnostics() {
    const auto* controller = presentation::MainWindowSkinController::existingInstance();
    return controller ? controller->diagnostics() : Diagnostics{};
}

QJsonObject resourceReport(const Diagnostics& value) {
    return {
        {QStringLiteral("controller_allocated"),
         presentation::MainWindowSkinController::existingInstance() != nullptr},
        {QStringLiteral("decode_jobs"), static_cast<double>(value.decodeJobs)},
        {QStringLiteral("preparation_jobs"), static_cast<double>(value.preparationJobs)},
        {QStringLiteral("pixmap_conversions"), static_cast<double>(value.pixmapConversions)},
        {QStringLiteral("retained_bytes"), static_cast<double>(value.retainedBytes)},
        {QStringLiteral("idle_frame_bytes"), static_cast<double>(value.idleFrameBytes)},
        {QStringLiteral("idle_frame_count"), value.idleFrameCount},
        {QStringLiteral("scratch_retained_bytes"), static_cast<double>(value.scratchRetainedBytes)},
        {QStringLiteral("executor_count"), value.executorCount},
        {QStringLiteral("executor_allocated"), value.executorAllocated},
        {QStringLiteral("busy"), value.busy}};
}

void requireNoResources(const Diagnostics& value) {
    require(!value.busy && !value.executorAllocated && value.executorCount == 0 &&
                value.retainedBytes == 0 && value.idleFrameBytes == 0 &&
                value.idleFrameCount == 0 && value.scratchRetainedBytes == 0,
            "non-rendering skins must retain no worker, image, pixmap, frame or scratch resources");
}

void requireNoImageJobs(const Diagnostics& before, const Diagnostics& after) {
    require(after.decodeJobs == before.decodeJobs &&
                after.preparationJobs == before.preparationJobs &&
                after.pixmapConversions == before.pixmapConversions,
            "non-rendering surface paints, reopens and settings changes must submit no image work");
}

struct Surface {
    QString name;
    QWidget* widget = nullptr;
};

class SurfaceFixture final {
  public:
    SurfaceFixture() {
        const std::array<QSize, 4> sizes{QSize(640, 48), QSize(420, 48), QSize(360, 48),
                                         QSize(560, 48)};
        for (std::size_t index = 0; index < rows_.size(); ++index) {
            auto& row = rows_[index];
            row = std::make_unique<ScreenshotToolbarPanel>();
            row->setObjectName(QStringLiteral("skinBenchmarkToolbar%1").arg(qulonglong(index)));
            row->resize(sizes[index]);
            auto* button = new adqt::widgets::AdButton(row.get());
            button->setText(QStringLiteral("Capture"));
            button->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Solid);
            button->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
            button->setGeometry(8, 8, 80, 32);
            button->setFocusPolicy(Qt::NoFocus);
            auto* radio = new adqt::widgets::AdRadio(row.get());
            radio->setText(QStringLiteral("Draw"));
            radio->setVariant(adqt::widgets::AdRadio::Variant::Button);
            radio->setButtonStyle(adqt::widgets::AdRadio::ButtonStyle::Solid);
            radio->setChecked(true);
            radio->setGeometry(96, 8, 72, 32);
            radio->setFocusPolicy(Qt::NoFocus);
            auto* select = new adqt::widgets::AdSelect(row.get());
            select->setPlaceholder(QStringLiteral("Mode"));
            select->setGeometry(176, 8, 128, 32);
            select->setFocusPolicy(Qt::NoFocus);
            row->show();
            surfaces_[index] = {QStringLiteral("toolbar_%1").arg(qulonglong(index)), row.get()};
        }
        tray_ = std::make_unique<presentation::SystemTrayController>();
        tray_->setMenuOptions(snow_shot::storage::TraySettings().menuOptions());
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QStringLiteral("systemTrayMenu")) {
                menus_[0] = qobject_cast<adqt::widgets::AdContextMenu*>(widget);
                break;
            }
        }
        require(menus_[0] != nullptr, "find the real SystemTrayController menu");
        menus_[1] = menus_[0]->findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("systemTrayWindowGroupMenu"));
        menus_[2] = menus_[0]->findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("systemTrayDeleteSpecifiedGroupMenu"));
        require(menus_[1] && menus_[2],
                "find both real tray submenus and their production bindings");
        const std::array<QString, 3> names{QStringLiteral("tray_menu"),
                                           QStringLiteral("tray_group_menu"),
                                           QStringLiteral("tray_delete_group_menu")};
        for (std::size_t index = 0; index < menus_.size(); ++index) {
            menus_[index]->setNativeMenuEnabled(false);
            require(!menus_[index]->actions().isEmpty(),
                    "each measured tray menu must be populated");
            surfaces_[rows_.size() + index] = {names[index], menus_[index]};
        }
        settle();
    }

    const std::array<Surface, 7>& surfaces() const {
        return surfaces_;
    }

    void hideMenus() const {
        for (auto* menu : menus_)
            menu->hide();
    }

    void show(const Surface& surface) const {
        if (auto* menu = qobject_cast<adqt::widgets::AdContextMenu*>(surface.widget)) {
            hideMenus();
            menu->popupAt(QPoint(20, 20));
        } else {
            surface.widget->show();
        }
    }

    void requireAppearance(const Surface& surface, bool active) const {
        const auto* controller = presentation::MainWindowSkinController::existingInstance();
        require(surface.widget->isVisible(), "a measured surface must actually be visible");
        require((controller && controller->skinActive(surface.widget)) == active,
                "the production surface binding must publish the expected skin state");
        if (auto* menu = qobject_cast<adqt::widgets::AdContextMenu*>(surface.widget)) {
            require(!menu->nativeMenuEnabled() &&
                        (!menu->backgroundFrame().image.isNull()) == active,
                    "a real custom tray menu must publish its expected background frame");
        } else {
            const qreal expected = active ? controller->maskOpacity() : 1.0;
            auto& themes = adqt::theme::ThemeManager::instance();
            for (auto* child : surface.widget->findChildren<QWidget*>()) {
                if (child->isVisibleTo(surface.widget) &&
                    child->window() == surface.widget->window()) {
                    require(qAbs(themes.backgroundOpacity(child) - expected) < 0.0001,
                            "populated toolbar controls must inherit the actual surface mask");
                }
            }
        }
    }

    void requireNoFrames() const {
        const auto* controller = presentation::MainWindowSkinController::existingInstance();
        for (const auto& surface : surfaces_) {
            require(!controller || (!controller->skinActive(surface.widget) &&
                                    controller->pixmap(surface.widget).isNull()),
                    "non-rendering surfaces must publish no prepared frame or pixmap");
        }
        for (auto* menu : menus_) {
            require(menu->backgroundFrame().image.isNull(),
                    "non-rendering tray bindings must clear their background frames");
        }
    }

  private:
    std::array<std::unique_ptr<ScreenshotToolbarPanel>, 4> rows_;
    std::unique_ptr<presentation::SystemTrayController> tray_;
    std::array<adqt::widgets::AdContextMenu*, 3> menus_{};
    std::array<Surface, 7> surfaces_;
};

QJsonObject measure(const SurfaceFixture& fixture, const Surface& surface, int samples,
                    bool active) {
    fixture.show(surface);
    settle();
    fixture.requireAppearance(surface, active);
    auto& widget = *surface.widget;
    const qreal dpr = widget.devicePixelRatioF();
    const QSize logicalSize = widget.size();
    const QSize physicalSize(qRound(widget.width() * dpr), qRound(widget.height() * dpr));
    QImage rendered(physicalSize, QImage::Format_ARGB32_Premultiplied);
    require(!rendered.isNull(), "allocate a reusable full physical surface render target");
    rendered.setDevicePixelRatio(dpr);
    QVector<double> paintTimes;
    QVector<double> showTimes;
    paintTimes.reserve(samples);
    showTimes.reserve(samples);
    for (int index = -WARMUP_SAMPLES; index < samples; ++index) {
        rendered.fill(Qt::transparent);
        QElapsedTimer timer;
        timer.start();
        QPainter painter(&rendered);
        widget.render(&painter);
        painter.end();
        const double paintTime = milliseconds(timer);
        widget.hide();
        settle();
        timer.restart();
        fixture.show(surface);
        QCoreApplication::processEvents();
        const double showTime = milliseconds(timer);
        settle();
        fixture.requireAppearance(surface, active);
        require(widget.size() == logicalSize,
                "a measured surface must keep the same logical geometry when reopened");
        if (index >= 0) {
            paintTimes.append(paintTime);
            showTimes.append(showTime);
        }
    }
    int visibleChildren = 0;
    for (auto* child : widget.findChildren<QWidget*>())
        visibleChildren += child->isVisibleTo(&widget) ? 1 : 0;
    return {{QStringLiteral("cached_paint"), distribution(paintTimes)},
            {QStringLiteral("reopen"), distribution(showTimes)},
            {QStringLiteral("dpr"), dpr},
            {QStringLiteral("logical_width"), widget.width()},
            {QStringLiteral("logical_height"), widget.height()},
            {QStringLiteral("physical_width"), rendered.width()},
            {QStringLiteral("physical_height"), rendered.height()},
            {QStringLiteral("widget_class"), QString::fromLatin1(widget.metaObject()->className())},
            {QStringLiteral("visible_child_widgets"), visibleChildren},
            {QStringLiteral("native_tray_menu"), false},
            {QStringLiteral("graphics_effect_present"), widget.graphicsEffect() != nullptr}};
}

QJsonObject measurePhase(const SurfaceFixture& fixture, int samples, bool active) {
    // Warm every production binding and final popup geometry before checking cached jobs.
    for (const auto& surface : fixture.surfaces()) {
        fixture.show(surface);
        settle();
        fixture.requireAppearance(surface, active);
    }
    const auto before = diagnostics();
    QJsonObject report;
    for (const auto& surface : fixture.surfaces())
        report.insert(surface.name, measure(fixture, surface, samples, active));
    const auto after = diagnostics();
    requireNoImageJobs(before, after);
    report.insert(QStringLiteral("resources"), resourceReport(after));
    report.insert(QStringLiteral("controller_allocated"),
                  presentation::MainWindowSkinController::existingInstance() != nullptr);
    report.insert(QStringLiteral("cached_decode_jobs"), 0);
    report.insert(QStringLiteral("cached_preparation_jobs"), 0);
    report.insert(QStringLiteral("cached_pixmap_conversions"), 0);
    report.insert(QStringLiteral("skin_active"), active);
    if (!active) {
        requireNoResources(after);
        fixture.requireNoFrames();
    }
    return report;
}

QJsonObject exerciseInactiveChanges(const SurfaceFixture& fixture) {
    const auto before = diagnostics();
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    QElapsedTimer timer;
    timer.start();
    for (const auto& surface : fixture.surfaces()) {
        const QSize originalSize = surface.widget->size();
        for (int step = 1; step <= 10; ++step)
            surface.widget->resize(originalSize + QSize(step, step));
        surface.widget->resize(originalSize);
        QEvent dprChange(QEvent::DevicePixelRatioChange);
        QCoreApplication::sendEvent(surface.widget, &dprChange);
    }
    for (int blur = 0; blur <= 16; ++blur) {
        require(configuration.setValue(QStringLiteral("interface/skin_blur_level"), blur),
                "change blur while the configured image is not rendered");
    }
    for (const auto& mode : {QStringLiteral("contain"), QStringLiteral("overlay")}) {
        require(configuration.setValue(QStringLiteral("interface/skin_display_mode"), mode),
                "change display mode while the configured image is not rendered");
    }
    settle();
    const auto after = diagnostics();
    requireNoImageJobs(before, after);
    requireNoResources(after);
    fixture.requireNoFrames();
    return {{QStringLiteral("elapsed_ms"), milliseconds(timer)},
            {QStringLiteral("resize_requests"), 77},
            {QStringLiteral("dpr_events"), 7},
            {QStringLiteral("blur_edits"), 17},
            {QStringLiteral("display_mode_edits"), 2},
            {QStringLiteral("additional_decode_jobs"), 0},
            {QStringLiteral("additional_preparation_jobs"), 0},
            {QStringLiteral("additional_pixmap_conversions"), 0}};
}

QString writeFixture(const QTemporaryDir& temporary) {
    QImage source(1920, 1080, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x)
            line[x] = qRgb(x % 256, y % 256, (x + y) % 256);
    }
    const QString path = temporary.filePath(QStringLiteral("skin.png"));
    QFile fixture(path);
    const auto encoded = snow_shot::image_codec::encodePng(source);
    require(!encoded.isEmpty() && fixture.open(QIODevice::WriteOnly) &&
                fixture.write(encoded) == encoded.size(),
            "write the skin benchmark fixture");
    return path;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotBenchmarks"));
    QCoreApplication::setApplicationName(QStringLiteral("skin_surface_benchmark"));
    const auto arguments = application.arguments();
    const auto option = [&arguments](const QString& name, const QString& fallback) {
        const auto index = arguments.indexOf(name);
        return index < 0 ? fallback : arguments.value(index + 1);
    };
    const QString outputPath = option(QStringLiteral("--output"), {});
    const QString label = option(QStringLiteral("--label"), QStringLiteral("current"));
    const QString theme = option(QStringLiteral("--theme"), QStringLiteral("light"));
    const int samples = option(QStringLiteral("--samples"), QStringLiteral("100")).toInt();
    if (outputPath.isEmpty() || label.isEmpty() || samples < 5 ||
        (theme != QStringLiteral("light") && theme != QStringLiteral("dark"))) {
        std::cerr << "Usage: skin surface benchmark --output report.json [--samples 100] "
                     "[--theme light|dark] [--label current]\n";
        return EXIT_FAILURE;
    }
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    try {
        QTemporaryDir temporary;
        require(temporary.isValid() &&
                    storage.initialize({temporary.path(), temporary.path(), 60000}).success,
                "initialize isolated skin surface benchmark storage");
        auto& themeManager = styles::ThemeManager::instance();
        themeManager.initialize(application);
        themeManager.setThemeAppearance(theme == QStringLiteral("light")
                                            ? styles::ThemeAppearance::Light
                                            : styles::ThemeAppearance::Dark);
        settle();
        QJsonObject report{
            {QStringLiteral("label"), label},
            {QStringLiteral("theme"), theme},
            {QStringLiteral("build_configuration"), QStringLiteral(SNOW_SKIN_SURFACE_BUILD_CONFIG)},
            {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
            {QStringLiteral("platform"), QApplication::platformName()},
            {QStringLiteral("os"), QSysInfo::prettyProductName()},
            {QStringLiteral("cpu_architecture"), QSysInfo::currentCpuArchitecture()},
            {QStringLiteral("measured_samples"), samples},
            {QStringLiteral("warmup_samples"), WARMUP_SAMPLES},
            {QStringLiteral("toolbar_rows"), 4},
            {QStringLiteral("controls_per_toolbar_row"), 3},
            {QStringLiteral("real_tray_menus"), 3},
            {QStringLiteral("reopen_includes_ready"), false},
            {QStringLiteral("tray_owner"), QStringLiteral("SystemTrayController")}};
        {
            SurfaceFixture fixture;
            require(
                presentation::MainWindowSkinController::existingInstance() == nullptr,
                "unconfigured populated toolbars and the real tray must keep the controller lazy");
            report.insert(QStringLiteral("disabled"), measurePhase(fixture, samples, false));
            require(presentation::MainWindowSkinController::existingInstance() == nullptr,
                    "no-path surface paints and reopens must never construct a skin controller");
            const QString path = writeFixture(temporary);
            require(storage.configuration().setValues(
                        {{QStringLiteral("interface/toolbar_skin_path"), path},
                         {QStringLiteral("interface/tray_menu_skin_path"), path},
                         {QStringLiteral("interface/skin_opacity"), 0},
                         {QStringLiteral("interface/skin_mask_opacity"), 100},
                         {QStringLiteral("interface/skin_blur_level"), 16}}),
                    "configure non-rendering skins before their first decode");
            settle();
            const auto inactive = diagnostics();
            require(inactive.decodeJobs == 0 && inactive.preparationJobs == 0 &&
                        inactive.pixmapConversions == 0,
                    "opacity-zero startup must perform no image work");
            auto opacityZero = measurePhase(fixture, samples, false);
            opacityZero.insert(QStringLiteral("inactive_changes"),
                               exerciseInactiveChanges(fixture));
            report.insert(QStringLiteral("opacity_zero"), opacityZero);
            require(storage.configuration().setValues(
                        {{QStringLiteral("interface/skin_opacity"), 100},
                         {QStringLiteral("interface/skin_mask_opacity"), 80}}),
                    "enable real populated toolbar and tray skins");
            settle();
            report.insert(QStringLiteral("enabled"), measurePhase(fixture, samples, true));
            require(storage.configuration().setValue(QStringLiteral("interface/skin_opacity"), 0),
                    "stop rendering previously active skins without clearing their paths");
            settle();
            auto suspended = measurePhase(fixture, samples, false);
            suspended.insert(QStringLiteral("inactive_changes"), exerciseInactiveChanges(fixture));
            report.insert(QStringLiteral("opacity_zero_after_active"), suspended);
            require(storage.configuration().setValue(QStringLiteral("interface/skin_opacity"), 100),
                    "restore active skins before testing clear");
            settle();
            for (const auto& surface : fixture.surfaces()) {
                fixture.show(surface);
                settle();
                fixture.requireAppearance(surface, true);
            }
            require(storage.configuration().setValues(
                        {{QStringLiteral("interface/toolbar_skin_path"), QString()},
                         {QStringLiteral("interface/tray_menu_skin_path"), QString()}}),
                    "clear all previously active surface profiles");
            settle();
            require(presentation::MainWindowSkinController::existingInstance() == nullptr,
                    "clearing all skins must retire their controller");
            report.insert(QStringLiteral("cleared_after_active"),
                          measurePhase(fixture, samples, false));
            require(presentation::MainWindowSkinController::existingInstance() == nullptr,
                    "cleared surface reopens must restore the cold no-controller state");
            fixture.hideMenus();
        }
        storage.shutdown();
        require(QDir().mkpath(QFileInfo(outputPath).absolutePath()),
                "create the skin surface benchmark report directory");
        QFile output(outputPath);
        const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        require(output.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    output.write(bytes) == bytes.size(),
                "write the skin surface benchmark report");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        storage.shutdown();
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
