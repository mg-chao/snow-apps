#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/update/updatetransaction.h"
#include <QTemporaryDir>
#include <QProcess>
#include <QLocalServer>
#include <QLocalSocket>
#include <QUuid>
#include "snow_shot/app/singleinstancecoordinator.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/settings/applicationpriority.h"
#include "snow_shot/platform/windows/autostartregistration.h"
#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include "diagnosticsbridge.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/presentation/capture/screenshotcapturepolicy.h"
#include "../presentation/capture/screenshotcaptureperfinstrumentation.h"
#include "../presentation/pinned/screenshotpintoperfinstrumentation.h"

#include "icon_renderer.h"
#include "locale/locale.h"
#include "widgets/tooltip.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QRegularExpression>
#include <QString>
#include <QDir>
#include <QStandardPaths>
#include <QSysInfo>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

extern "C" void snow_diagnostics_install_panic_hook(void (*callback)(const unsigned char*, size_t));

int main(int argc, char* argv[]) {
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShot"));
    QString applicationName = QStringLiteral("snow_shot");
    QString e2eInstanceId;
    bool e2eCaptureEnabled = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        e2eCaptureEnabled =
            e2eCaptureEnabled || argument == QStringLiteral("--e2e-allow-overlay-capture");
        constexpr auto e2eInstancePrefix = "--e2e-instance-id=";
        if (argument.startsWith(QString::fromLatin1(e2eInstancePrefix))) {
            e2eInstanceId =
                argument.mid(static_cast<int>(std::char_traits<char>::length(e2eInstancePrefix)));
        }
    }
    if (e2eCaptureEnabled && QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"))
                                 .match(e2eInstanceId)
                                 .hasMatch()) {
        applicationName += QStringLiteral("-e2e-") + e2eInstanceId;
    }
    QCoreApplication::setApplicationName(applicationName);
    QCoreApplication::setApplicationVersion(QStringLiteral(SNOW_DIAGNOSTICS_VERSION));
    // A probe runs before diagnostics, singleton acquisition, or any live user-state access.
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == u"--update-probe") {
        if (QString::fromLocal8Bit(argv[2]) != QCoreApplication::applicationVersion()) {
            return 3;
        }
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication probe(argc, argv);
        QTemporaryDir directory;
        if (!directory.isValid()) {
            return 4;
        }
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (!storage.initialize({directory.path(), directory.path(), 8000}).success) {
            return 5;
        }
        snow_shot::presentation::LanguageManager::instance().initialize();
        snow_shot::presentation::styles::ThemeManager::instance().initialize(probe);
        storage.shutdown();
        return 0;
    }
    QString executablePath = QString::fromLocal8Bit(argv[0]);
#ifdef Q_OS_WIN
    wchar_t modulePath[32768]{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, 32768);
    if (moduleLength > 0 && moduleLength < 32768) {
        executablePath = QString::fromWCharArray(modulePath, static_cast<int>(moduleLength));
    }
#endif
    const QString updateRoot =
        snow_shot::update::installationRoot(QFileInfo(executablePath).absolutePath());
    if (snow_shot::update::transactionPending(updateRoot)) {
        QCoreApplication recovery(argc, argv);
        const QString root =
            snow_shot::update::installationRoot(QCoreApplication::applicationDirPath());
        const QString pipe =
            QStringLiteral("snow-shot-recover-") + QUuid::createUuid().toString(QUuid::Id128);
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(pipe)) {
            return 6;
        }
        const QString result = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(pipe + QStringLiteral(".txt"));
        const QStringList args{
            QStringLiteral("--launch"), QStringLiteral("--recovery"),
            QStringLiteral("--target"), root,
            QStringLiteral("--parent"), QString::number(QCoreApplication::applicationPid()),
            QStringLiteral("--pipe"),   pipe,
            QStringLiteral("--result"), result};
        if (!QProcess::startDetached(
                QDir(root).filePath(QStringLiteral("bin/snow-shot-updater.exe")), args)) {
            return 6;
        }
        if (!server.waitForNewConnection(180000)) {
            return 6;
        }
        auto* socket = server.nextPendingConnection();
        if (!socket->canReadLine() && !socket->waitForReadyRead(10000)) {
            return 6;
        }
        if (socket->readLine().trimmed() != "ready") {
            return 6;
        }
        socket->write("go\n");
        socket->waitForBytesWritten(5000);
        return 0;
    }
    auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
    struct DiagnosticsLifetime {
        ~DiagnosticsLifetime() {
            snow_shot::diagnostics::DiagnosticsService::instance().shutdown();
        }
    } diagnosticsLifetime;
    snow_shot::storage::StorageInitializationOptions storageOptions;
    storageOptions.resolvedDirectory =
        std::make_shared<snow_shot::storage::StorageDirectorySelection>(
            snow_shot::storage::ApplicationStorage::resolveDirectory());
    snow_shot::diagnostics::DiagnosticsOptions diagnosticsOptions;
    const auto& selectedStorage = *storageOptions.resolvedDirectory;
    if (!selectedStorage.effectiveDirectory.isEmpty()) {
        diagnosticsOptions.directories.append(
            QDir(selectedStorage.effectiveDirectory).filePath(QStringLiteral("logs")));
    }
    diagnosticsOptions.directories.append(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("logs")));
    diagnosticsOptions.directories.append(
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("SnowShot/%1/logs").arg(applicationName)));
    diagnosticsOptions.handlerPath =
        QDir(selectedStorage.executableDirectory).filePath(QStringLiteral("crashpad_handler.exe"));
    diagnosticsOptions.version = QStringLiteral(SNOW_DIAGNOSTICS_VERSION);
    diagnosticsOptions.revision = QStringLiteral(SNOW_DIAGNOSTICS_REVISION);
    diagnosticsOptions.buildConfiguration = QStringLiteral(SNOW_DIAGNOSTICS_BUILD);
    static_cast<void>(diagnostics.initialize(std::move(diagnosticsOptions)));
    snow_diagnostics_install_panic_hook(snow_diag_panic);

    // Capture overlays embed native window-type children (the floating tool
    // palette) alongside alien, click-through children (selection toolbar,
    // shortcut hints). Qt would otherwise force every sibling of an embedded
    // native child native, which silently turns those click-through surfaces
    // into OS-level input interceptors.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    QApplication app(argc, argv);
    snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.app"),
                                     QStringLiteral("application.platform"),
                                     {{QStringLiteral("backend"), QGuiApplication::platformName()},
                                      {QStringLiteral("os"), QSysInfo::kernelVersion()}});
    static_cast<void>(snow_shot::presentation::capture::resolveAutoScreenshotApiMode());
#if defined(SNOW_SHOT_PIN_PERF_INSTRUMENTATION)
    snow_shot::presentation::pin_perf::configureTrace(
        qEnvironmentVariable("SNOW_SHOT_PIN_PERF_TRACE"));
#endif
#if defined(SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION)
    snow_shot::presentation::capture_perf::configureTrace(
        qEnvironmentVariable("SNOW_SHOT_CAPTURE_PERF_TRACE"));
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

    static_cast<void>(
        snow_shot::storage::ApplicationStorage::instance().initialize(storageOptions));
    struct StorageLifetime {
        ~StorageLifetime() {
            snow_shot::storage::ApplicationStorage::instance().shutdown();
        }
    } storageLifetime;
    // Declared after storageLifetime so reverse-order destruction drains the
    // history executor while the storage singleton it reads is still alive and
    // rejects straggler submissions; no history task runs after main returns.
    struct HistoryTaskDrain {
        ~HistoryTaskDrain() {
            shutdownScreenshotHistoryTasks();
        }
    } historyTaskDrain;
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
    singleInstance.setLaunchRequestHandler([&applicationController](const QStringList& arguments) {
        applicationController.handleLaunchRequest(arguments);
    });
    applicationController.start();
    snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.app"),
                                     QStringLiteral("application.ready"));
    if (!QApplication::arguments().contains(QStringLiteral("--autostart")) &&
        QApplication::arguments().contains(QStringLiteral("--show-main-window"))) {
        applicationController.showMainWindow();
    }
    return QApplication::exec();
}
