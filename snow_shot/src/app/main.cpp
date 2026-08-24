#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/app/singleinstancecoordinator.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/settings/applicationpriority.h"
#include "snow_shot/platform/windows/autostartregistration.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../presentation/pinned/screenshotpintoperfinstrumentation.h"
#include "../presentation/ocr/screenshotocrworkerentry.h"
#include "../presentation/services/screenshotlifecycleperfinstrumentation.h"

#include "icon_registry.h"
#include "locale/locale.h"
#include "widgets/tooltip.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QRegularExpression>
#include <QString>
#include <QTimer>

#include <cstring>

int main(int argc, char* argv[]) {
    if (argc == 2 &&
        std::strcmp(argv[1], snow_shot::presentation::kScreenshotOcrWorkerArgument) == 0) {
        return snow_shot::presentation::runScreenshotOcrWorker();
    }
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShot"));
    QString applicationName = QStringLiteral("snow_shot");
    QString e2eInstanceId;
    bool e2eCaptureEnabled = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        e2eCaptureEnabled = e2eCaptureEnabled ||
                            argument == QStringLiteral("--e2e-allow-overlay-capture");
        constexpr auto e2eInstancePrefix = "--e2e-instance-id=";
        if (argument.startsWith(QString::fromLatin1(e2eInstancePrefix))) {
            e2eInstanceId = argument.mid(static_cast<int>(
                std::char_traits<char>::length(e2eInstancePrefix)));
        }
    }
    const bool isolatedE2eInstance =
        e2eCaptureEnabled &&
        QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"))
            .match(e2eInstanceId)
            .hasMatch();
    if (isolatedE2eInstance) {
        applicationName += QStringLiteral("-e2e-") + e2eInstanceId;
    }
    QCoreApplication::setApplicationName(applicationName);

    QApplication app(argc, argv);
#if defined(SNOW_SHOT_PIN_PERF_INSTRUMENTATION)
    snow_shot::presentation::pin_perf::configureTrace(
        qEnvironmentVariable("SNOW_SHOT_PIN_PERF_TRACE"));
#endif
#if defined(SNOW_SHOT_SCREENSHOT_LIFECYCLE_PERF_INSTRUMENTATION)
    snow_shot::presentation::screenshot_lifecycle_perf::configureTrace(
        qEnvironmentVariable("SNOW_SHOT_SCREENSHOT_LIFECYCLE_PERF_TRACE"));
#endif
    snow_shot::app::SingleInstanceCoordinator singleInstance;
    const snow_shot::app::SingleInstanceResult instanceResult =
        singleInstance.acquireOrForward(QApplication::arguments());
    if (instanceResult.outcome == snow_shot::app::SingleInstanceOutcome::Forwarded) {
        return 0;
    }
    if (instanceResult.outcome == snow_shot::app::SingleInstanceOutcome::Failed) {
        qWarning().noquote() << instanceResult.error;
        return 2;
    }

    snow_shot::storage::StorageInitializationOptions storageOptions;
#if defined(SNOW_SHOT_SCREENSHOT_MEMORY_FOOTPRINT_INSTRUMENTATION)
    const QString e2eStorageDirectory =
        qEnvironmentVariable("SNOW_SHOT_E2E_STORAGE_DIRECTORY").trimmed();
    if (isolatedE2eInstance && !e2eStorageDirectory.isEmpty()) {
        if (!QDir::isAbsolutePath(e2eStorageDirectory)) {
            qWarning("The E2E storage directory must be absolute");
            return 2;
        }
        const QString isolatedStoragePath = QDir::cleanPath(e2eStorageDirectory);
        // Treat the benchmark directory as both the executable and AppData roots so a portable
        // marker beside the measured binary cannot redirect the test into normal user storage.
        storageOptions.executableDirectory = isolatedStoragePath;
        storageOptions.appDataDirectory = isolatedStoragePath;
    }
#endif
    static_cast<void>(
        snow_shot::storage::ApplicationStorage::instance().initialize(storageOptions));
    if (snow_shot::platform::windows::AutoStartRegistration::isSupported()) {
        const bool enabled = snow_shot::storage::SystemSettings().autoStartAtBoot();
        QString error;
        if ((!enabled ||
             !snow_shot::platform::windows::AutoStartRegistration::matchesExpectedCommand()) &&
            !snow_shot::platform::windows::AutoStartRegistration::setEnabled(enabled, &error)) {
            qWarning().noquote() << error;
        }
    }
    static_cast<void>(snow_shot::presentation::settings::applyConfiguredApplicationPriority());
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setWindowIcon(
        adqt::icons::makeIcon(snow_shot::presentation::icons::custom::app::ApplicationIcon()));
    adqt::locale::LocaleManager::instance().applyTo(app);
    snow_shot::presentation::LanguageManager::instance().initialize();
    snow_shot::presentation::styles::ThemeManager::instance().initialize(app);
    adqt::widgets::AdTooltip::installApplicationTooltips();

    snow_shot::app::ApplicationController applicationController(app);
    singleInstance.setLaunchRequestHandler(
        [&applicationController](const QStringList& arguments) {
            applicationController.handleLaunchRequest(arguments);
        });
    applicationController.start();
#if defined(SNOW_SHOT_SCREENSHOT_LIFECYCLE_PERF_INSTRUMENTATION)
    QTimer::singleShot(0, &app, []() {
        snow_shot::presentation::screenshot_lifecycle_perf::appReady();
    });
#endif
    if (!QApplication::arguments().contains(QStringLiteral("--autostart")) &&
        QApplication::arguments().contains(QStringLiteral("--show-main-window"))) {
        applicationController.showMainWindow();
    }
    return QApplication::exec();
}
