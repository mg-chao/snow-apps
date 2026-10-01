#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QWidget>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#ifdef Q_OS_MACOS
#include <mach/mach.h>
#endif
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#endif

namespace settings = snow_shot::presentation::settings;
namespace styles = snow_shot::presentation::styles;

namespace {
constexpr int WARMUP_PAINTS = 10;
constexpr int MEASURED_PAINTS = 200;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double milliseconds(const QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

QJsonObject distribution(QVector<double> values) {
    std::sort(values.begin(), values.end());
    return {{QStringLiteral("median_ms"), values.at(values.size() / 2)},
            {QStringLiteral("p95_ms"), values.at((values.size() - 1) * 95 / 100)}};
}

double peakResidentBytes() {
#ifdef Q_OS_UNIX
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
#ifdef Q_OS_MACOS
    return static_cast<double>(usage.ru_maxrss);
#else
    return static_cast<double>(usage.ru_maxrss) * 1024.0;
#endif
#elif defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS usage{};
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &usage, static_cast<DWORD>(sizeof(usage)))
               ? static_cast<double>(usage.PeakWorkingSetSize)
               : 0.0;
#else
    return 0.0;
#endif
}

double residentBytes() {
#ifdef Q_OS_MACOS
    mach_task_basic_info_data_t usage{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&usage),
                     &count) == KERN_SUCCESS
               ? static_cast<double>(usage.resident_size)
               : 0.0;
#elif defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS usage{};
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &usage, static_cast<DWORD>(sizeof(usage)))
               ? static_cast<double>(usage.WorkingSetSize)
               : 0.0;
#else
    return 0.0;
#endif
}

void requireNoSkinObjects(const QApplication& application, const MainWindow& window) {
    const auto inspect = [](const QObject* object) {
        const QByteArray className(object->metaObject()->className());
        require(!className.contains("MainWindowSkin") &&
                    !object->objectName().contains(QStringLiteral("mainWindowSkin"),
                                                   Qt::CaseInsensitive),
                "the disabled path must not create a skin controller or background widget");
    };
    for (const auto* object : application.findChildren<QObject*>()) {
        inspect(object);
    }
    for (const auto* object : window.findChildren<QObject*>()) {
        inspect(object);
    }
    for (const auto* widget : QApplication::allWidgets()) {
        inspect(widget);
    }
}

QJsonObject measure(QApplication& application, const QString& theme, const QString& label) {
    const double residentBeforeUi = residentBytes();
    QElapsedTimer startup;
    startup.start();
    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    const double constructionMs = milliseconds(startup);
    window.showFunctionSettings();
    flushEvents();
    // Offscreen screens can clamp the initial show. Resize the visible window so
    // the baseline and feature builds render the same logical surface at any DPR.
    window.resize(900, 640);
    flushEvents();
    auto* central = window.centralWidget();
    require(central != nullptr && QByteArray(central->metaObject()->className()) == "QWidget",
            "the unskinned central widget must retain the original QWidget class");
    require(central->autoFillBackground(),
            "the unskinned central widget must retain its opaque automatic background");
    require(central->size() == QSize(900, 640),
            "the visible benchmark surface must be 900 by 640 logical pixels");
    requireNoSkinObjects(application, window);

    const qreal dpr = central->devicePixelRatioF();
    const QSize physicalSize(qRound(central->width() * dpr), qRound(central->height() * dpr));
    QImage target(physicalSize, QImage::Format_ARGB32_Premultiplied);
    require(!target.isNull(), "allocate the reusable full-window render target");
    target.setDevicePixelRatio(dpr);
    target.fill(Qt::transparent);
    QElapsedTimer firstPaint;
    firstPaint.start();
    central->render(&target);
    const double firstPaintMs = milliseconds(firstPaint);
    const double startupMs = milliseconds(startup);
    for (int index = 0; index < WARMUP_PAINTS; ++index) {
        central->render(&target);
    }
    QVector<double> paints;
    paints.reserve(MEASURED_PAINTS);
    for (int index = 0; index < MEASURED_PAINTS; ++index) {
        QElapsedTimer paint;
        paint.start();
        central->render(&target);
        paints.append(milliseconds(paint));
    }
    requireNoSkinObjects(application, window);
    return {{QStringLiteral("label"), label},
            {QStringLiteral("theme"), theme},
            {QStringLiteral("route"), QStringLiteral("function-settings")},
            {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
            {QStringLiteral("platform"), QApplication::platformName()},
            {QStringLiteral("root_class"), QString::fromLatin1(central->metaObject()->className())},
            {QStringLiteral("no_skin_objects"), true},
            {QStringLiteral("dpr"), dpr},
            {QStringLiteral("logical_width"), central->width()},
            {QStringLiteral("logical_height"), central->height()},
            {QStringLiteral("physical_width"), target.width()},
            {QStringLiteral("physical_height"), target.height()},
            {QStringLiteral("warmup_paints"), WARMUP_PAINTS},
            {QStringLiteral("measured_paints"), MEASURED_PAINTS},
            {QStringLiteral("construction_ms"), constructionMs},
            {QStringLiteral("first_render_ms"), firstPaintMs},
            {QStringLiteral("startup_ms"), startupMs},
            {QStringLiteral("cached_paint"), distribution(paints)},
            {QStringLiteral("resident_before_ui_bytes"), residentBeforeUi},
            {QStringLiteral("resident_after_ui_bytes"), residentBytes()},
            {QStringLiteral("peak_resident_bytes"), peakResidentBytes()}};
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotBenchmarks"));
    QCoreApplication::setApplicationName(QStringLiteral("main_window_no_skin_benchmark"));
    const auto arguments = application.arguments();
    const auto outputIndex = arguments.indexOf(QStringLiteral("--output"));
    const auto labelIndex = arguments.indexOf(QStringLiteral("--label"));
    const auto themeIndex = arguments.indexOf(QStringLiteral("--theme"));
    const QString outputPath = outputIndex < 0 ? QString{} : arguments.value(outputIndex + 1);
    const QString label =
        labelIndex < 0 ? QStringLiteral("unskinned") : arguments.value(labelIndex + 1);
    const QString theme =
        themeIndex < 0 ? QStringLiteral("light") : arguments.value(themeIndex + 1);
    if (outputPath.isEmpty() || label.isEmpty() ||
        (theme != QStringLiteral("light") && theme != QStringLiteral("dark"))) {
        std::cerr << "Usage: no-skin benchmark --output result.json [--label baseline] "
                     "[--theme light|dark]\n";
        return EXIT_FAILURE;
    }
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "create isolated benchmark storage");
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
                "initialize isolated benchmark storage");
        auto& themeManager = styles::ThemeManager::instance();
        themeManager.initialize(application);
        themeManager.setThemeAppearance(theme == QStringLiteral("light")
                                            ? styles::ThemeAppearance::Light
                                            : styles::ThemeAppearance::Dark);
        flushEvents();
        const QJsonDocument report(measure(application, theme, label));
        storage.shutdown();
        require(QDir().mkpath(QFileInfo(outputPath).absolutePath()),
                "create the benchmark report directory");
        QFile output(outputPath);
        require(output.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "open the benchmark report");
        const QByteArray json = report.toJson(QJsonDocument::Indented);
        require(output.write(json) == json.size(), "write the benchmark report");
        std::cout << json.constData();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
