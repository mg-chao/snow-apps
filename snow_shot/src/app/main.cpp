#include "snow_shot/app/edition.h"
#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/app/applicationrestart.h"
#include <QTemporaryDir>
#include <QProcess>
#include <QLocalServer>
#include <QLocalSocket>
#include <QUuid>
#include "snow_shot/app/singleinstancecoordinator.h"
#include "snow_shot/app/shellimagelaunch.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/settings/applicationpriority.h"
#include "snow_shot/platform/windows/autostartregistration.h"
#include "snow_shot/platform/windows/administratorlaunch.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/platform/windows/shellcontextmenuregistration.h"
#include "widgets/message.h"
#include "widgets/platform_compatibility.h"
#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/update/startupupdate.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/diagnostics/ffmpegdiagnostics.h"
#include "diagnosticsbridge.h"
#include "startupregistrationreport.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/presentation/capture/screenshotcapturepolicy.h"
#include "../presentation/capture/screenshotcaptureperfinstrumentation.h"
#include "../presentation/capture/screenshotscrollingpipeline.h"
#include "../presentation/pinned/screenshotpintoperfinstrumentation.h"

#include "icon_renderer.h"
#include "locale/locale.h"
#include "widgets/tooltip.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>
#include <QSysInfo>
#include <optional>
#include <utility>
#include <cstdio>
#include "snow_capture.h"
#include "snow_recording.h"
#ifdef SNOW_SHOT_MCP_TEST_FIXTURE
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
int runMcpApplicationLifecycleTests(int argc, char** argv);
#endif
#ifdef Q_OS_MACOS
#include "snow_recording_macos.h"
#include "snow_shot/diagnostics/microphonediagnostics.h"
#include "snow_shot/platform/macos/loginitemservice.h"
#include "snow_shot/platform/macos/rastercolorspace.h"
#include <QScopeGuard>
#include <future>
#include <thread>
#include <QTimer>
#include <QScreen>
#endif

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

extern "C" void snow_diagnostics_install_panic_hook(void (*callback)(const unsigned char*, size_t));

#ifdef SNOW_SHOT_CONTROLLER_LIFETIME_TESTS
int runScreenshotControllerLifetimeTests(int argc, char** argv);
int runScreenshotControllerClipboardTests(int argc, char** argv);
#endif

namespace {
snow_shot::diagnostics::DiagnosticsOptions
startupDiagnosticsOptions(const QString& selectedDirectory, const QString& applicationName) {
    snow_shot::diagnostics::DiagnosticsOptions options;
    const bool storeManaged = snow_shot::app::distribution::isStoreManaged();
    const QString stableRoot =
        storeManaged ? snow_shot::app::distribution::writableDataDirectory()
                     : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    // A missing package data root must never turn a relative path into a writable log
    // directory in the current working directory or the standalone application's state.
    if (storeManaged && !QDir::isAbsolutePath(stableRoot))
        return options;
    if (!selectedDirectory.isEmpty())
        options.directories.append(QDir(selectedDirectory).filePath(QStringLiteral("logs")));
    options.directories.append(QDir(stableRoot).filePath(QStringLiteral("logs")));
    // Crashpad registers process-wide exception resources once. Its live database must
    // stay at a stable path while the user-selected data/log directory is migrated.
    options.crashCaptureDirectory = QDir(stableRoot).filePath(QStringLiteral("logs/crashes"));
    const QString temporaryRoot =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).canonicalPath();
    if (!storeManaged || QDir::isAbsolutePath(temporaryRoot)) {
        const QString relative =
            storeManaged
                ? QStringLiteral("SnowShot/%1/%2/logs")
                      .arg(applicationName, snow_shot::app::distribution::packageFamilyName())
                : QStringLiteral("SnowShot/%1/logs").arg(applicationName);
        options.directories.append(QDir(temporaryRoot).filePath(relative));
    }
    return options;
}

#ifndef Q_OS_MACOS
QString updateInstallationRoot(const QString& executableDirectory) {
    return QFileInfo(executableDirectory).dir().absolutePath();
}

std::optional<bool> updateTransactionPending(const QString& helperPath, const QString& root) {
    QProcess helper;
    helper.start(helperPath,
                 {QStringLiteral("--transaction-state"), QStringLiteral("--target"), root},
                 QIODevice::ReadOnly);
    if (!helper.waitForStarted(10000) || !helper.waitForFinished(10000)) {
        return std::nullopt;
    }
    const QByteArray output = helper.readAllStandardOutput().trimmed();
    if (helper.exitStatus() != QProcess::NormalExit || helper.exitCode() != 0 ||
        (output != "pending" && output != "clean")) {
        return std::nullopt;
    }
    return output == "pending";
}
#endif
} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1 && QByteArray(argv[1]) == "--distribution-probe") {
        if (argc != 2)
            return 64;
        QCoreApplication probe(argc, argv);
        QCoreApplication::setOrganizationName(snow_shot::app::edition::registryName());
        QCoreApplication::setApplicationName(snow_shot::app::edition::applicationName());
        const auto policy = snow_shot::app::distribution::policy();
        const auto diagnostics =
            startupDiagnosticsOptions({}, snow_shot::app::edition::applicationName());
        const QByteArray output =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("channel"), SNOW_SHOT_STORE_BUILD
                                                    ? QStringLiteral("store")
                                                    : QStringLiteral("standalone")},
                    {QStringLiteral("compiledStore"), SNOW_SHOT_STORE_BUILD != 0},
                    {QStringLiteral("managedByStore"), policy.managedByStore},
                    {QStringLiteral("packageFamily"), policy.packageFamily},
                    {QStringLiteral("diagnosticDirectories"),
                     QJsonArray::fromStringList(diagnostics.directories)},
                    {QStringLiteral("crashCaptureDirectory"), diagnostics.crashCaptureDirectory},
                })
                .toJson(QJsonDocument::Compact);
        return std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout) ==
                       static_cast<size_t>(output.size())
                   ? 0
                   : 1;
    }
    QCoreApplication::setOrganizationName(snow_shot::app::edition::registryName());
    QString applicationName = snow_shot::app::edition::applicationName();
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
    if (QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"))
            .match(e2eInstanceId)
            .hasMatch()) {
        applicationName += QStringLiteral("-e2e-") + e2eInstanceId;
    }
    QCoreApplication::setApplicationName(applicationName);
    QCoreApplication::setApplicationVersion(QStringLiteral(SNOW_DIAGNOSTICS_VERSION));
#ifdef SNOW_SHOT_CONTROLLER_LIFETIME_TESTS
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == u"--controller-lifetime-tests") {
        return runScreenshotControllerLifetimeTests(argc, argv);
    }
    if (argc == 2 && (QString::fromLocal8Bit(argv[1]) == u"--clipboard-pin-tests" ||
                      QString::fromLocal8Bit(argv[1]) == u"--automatic-selection-tests" ||
                      QString::fromLocal8Bit(argv[1]) == u"--shell-file-startup-tests" ||
                      QString::fromLocal8Bit(argv[1]) == u"--image-editor-pin-tests")) {
        return runScreenshotControllerClipboardTests(argc, argv);
    }
#endif
#ifdef SNOW_SHOT_MCP_TEST_FIXTURE
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == u"--mcp-lifecycle-tests")
        return runMcpApplicationLifecycleTests(argc, argv);
    // Only test builds expose this isolated production-router fixture. It bypasses
    // singleton acquisition, startup registration, and the user's configuration.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == u"--mcp-fixture") {
        const QFileInfo rootInfo(QString::fromLocal8Bit(argv[2]));
        if (!rootInfo.isAbsolute() || !rootInfo.isDir() || rootInfo.isSymLink())
            return 2;
        const QString root = rootInfo.canonicalFilePath();
        qputenv("SNOW_SHOT_MCP_DESCRIPTOR",
                QDir(root).filePath(QStringLiteral("descriptor.json")).toUtf8());
        QCoreApplication::setApplicationName(QStringLiteral("snow-shot-mcp-fixture-") +
                                             QUuid::createUuid().toString(QUuid::Id128));
        QStandardPaths::setTestModeEnabled(true);
        QApplication fixture(argc, argv);
        fixture.setQuitOnLastWindowClosed(false);
        adqt::widgets::initializePlatformCompatibility(fixture);
        const QString executable = QDir(root).filePath(QStringLiteral("bin"));
        if (!QDir().mkpath(executable))
            return 3;
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (!storage.initialize({executable, root, 60000}).success)
            return 4;
        storage.configuration().setValues(
            {{QStringLiteral("mcp/enabled"), true},
             {QStringLiteral("tray/enabled"), false},
             {QStringLiteral("pin_to_screen/automatic_text_recognition"), false}});
        {
            // Publish through the real repository so router tests can exercise
            // history payloads and artifacts without capturing the user's desktop.
            SnowCanvasRuntime canvas;
            snow_shot::storage::CaptureHistoryDraft draft;
            draft.id = QStringLiteral("fd97d4e3-311c-48de-bbfb-3b4f43b0fef6");
            draft.createdUtc = QDateTime::currentDateTimeUtc();
            draft.canvasBounds = QRect(0, 0, 80, 60);
            draft.selection.rectangle = draft.canvasBounds;
            draft.selection.shadowColor = QColor(Qt::black);
            draft.canvasHistory = canvas.serializeDocumentHistory();
            QImage image(draft.canvasBounds.size(), QImage::Format_RGBA8888);
            image.fill(QColor(40, 150, 210));
            draft.displays.append({QStringLiteral("fixture"), QStringLiteral("Fixture"), image});
            draft.resultImage = image;
            if (!storage.captureHistory().publish(std::move(draft)).get().storage.success)
                return 5;
        }
        snow_shot::presentation::LanguageManager::instance().initialize();
        snow_shot::presentation::styles::ThemeManager::instance().initialize(fixture);
        int result = 0;
        {
            snow_shot::app::ApplicationController controller(fixture);
            QTimer::singleShot(180000, &fixture, &QCoreApplication::quit);
            result = fixture.exec();
        }
        storage.shutdown();
        return result;
    }
#endif
    bool applicationRestart = false;
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == u"--restart-helper") {
        QCoreApplication helper(argc, argv);
        const int result = snow_shot::app::dispatchApplicationRestartHelper(helper.arguments());
        if (result != -1)
            return result;
        applicationRestart = true;
    }
    bool administratorRestart = false;
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == u"--administrator-helper") {
        if (snow_shot::app::distribution::isStoreManaged())
            return 2;
        QCoreApplication helper(argc, argv);
        const int result =
            snow_shot::platform::windows::dispatchAdministratorHelper(helper.arguments());
        if (result != -1)
            return result;
        administratorRestart = true;
    }
#ifdef Q_OS_MACOS
    // Runs the shipped recording bridge with the Cocoa event loop, without user storage.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == u"--recording-macos-probe") {
        QApplication probe(argc, argv);
        adqt::widgets::initializePlatformCompatibility(probe);
        const QStringList arguments = probe.arguments();
        const QByteArray path = QFileInfo(arguments[2]).absoluteFilePath().toUtf8();
        const QRect screen = probe.primaryScreen()->geometry();
        const QPoint origin = screen.topLeft() + QPoint(40, 40);
        auto result = std::async(std::launch::async, [path, arguments, origin] {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            SnowCaptureDirectRecordingConfig config{};
            config.version = SNOW_CAPTURE_DIRECT_RECORDING_CONFIG_VERSION;
            config.struct_size = sizeof(config);
            config.mouse_highlight_radius = 41;
            config.x = origin.x();
            config.y = origin.y();
            config.width = 321;
            config.height = 239;
            config.capture_backend = SNOW_CAPTURE_BACKEND_AUTO;
            config.output_file_utf8 = path.constData();
            const auto suffix = QFileInfo(QString::fromUtf8(path)).suffix();
            config.output_format = suffix == u"gif"    ? SNOW_RECORDING_OUTPUT_FORMAT_GIF
                                   : suffix == u"apng" ? SNOW_RECORDING_OUTPUT_FORMAT_APNG
                                   : suffix == u"webp" ? SNOW_RECORDING_OUTPUT_FORMAT_WEBP
                                                       : SNOW_RECORDING_OUTPUT_FORMAT_MP4;
            config.capture_fps = 30;
            config.output_fps = 15;
            config.maximum_width = 1920;
            config.maximum_height = 1080;
            config.quality = 80;
            config.codec = arguments.contains(u"hevc") ? SNOW_CAPTURE_VIDEO_CODEC_H265
                                                       : SNOW_CAPTURE_VIDEO_CODEC_H264;
            config.preset = SNOW_CAPTURE_VIDEO_ENCODING_PRESET_VERYFAST;
            config.encoder_preference = arguments.contains(u"software")
                                            ? SNOW_CAPTURE_ENCODER_PREFERENCE_SOFTWARE
                                            : SNOW_CAPTURE_ENCODER_PREFERENCE_H264_HARDWARE;
            config.enable_system_audio = static_cast<uint8_t>(arguments.contains(u"system-audio"));
            config.enable_microphone = static_cast<uint8_t>(arguments.contains(u"microphone"));
            config.show_cursor = 1;
            config.keyboard_size = 64;
            config.mouse_trail_duration_ms = 500;
            config.loop_animated_images = 1;
            if (arguments.contains(u"effects")) {
                config.show_keyboard = 1;
                config.record_mouse_clicks = 1;
                config.mouse_trail_rgba = 0xff4080e0;
                config.mouse_click_rgba = 0x40a0ffe0;
                config.keyboard_background_rgba = 0x000000cc;
                config.keyboard_text_rgba = 0xffffffff;
                config.keyboard_border_rgba = 0x808080ff;
                config.mouse_highlight_rgba = 0xffff0080;
            }
            uint32_t width = 0, height = 0;
            if (!snow_recording_region_output_dimensions(
                    config.x, config.y, config.width, config.height, config.maximum_width,
                    config.maximum_height, config.output_format, &width, &height)) {
                qWarning("%s", snow_recording_last_error_message());
                return 1;
            }
            SnowRecordingSession* session = nullptr;
            if (snow_recording_session_create_direct(&config, &session) !=
                SNOW_RECORDING_RESULT_OK) {
                qWarning("%s", snow_recording_last_error_message());
                return 2;
            }
            const auto destroy =
                qScopeGuard([session] { snow_recording_session_destroy(session); });
            if (!snow_recording_session_start(session)) {
                qWarning("%s", snow_recording_last_error_message());
                return 3;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            if (!snow_recording_session_pause(session))
                return 4;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (!snow_recording_session_resume(session))
                return 5;
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            if (snow_recording_session_stop(session) != SNOW_RECORDING_RESULT_OK) {
                qWarning("%s", snow_recording_last_error_message());
                return 6;
            }
            qInfo("macos recording probe: logical=321x239 output=%ux%u", width, height);
            return 0;
        });
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &probe, [&] {
            if (result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
                probe.exit(result.get());
        });
        poll.start(20);
        return probe.exec();
    }
#endif
    // Package QA uses the ordinary FFI and linked vendor encoders, without
    // opening UI, taking the singleton, or modifying the user's settings.
    if ((argc == 4 || argc == 5) && QString::fromLocal8Bit(argv[1]) == u"--recording-gpu-probe") {
        QCoreApplication probe(argc, argv);
        const QByteArray path = QString::fromLocal8Bit(argv[2]).toUtf8();
        const QString backend = QString::fromLocal8Bit(argv[3]);
        if (backend != u"dxgi" && backend != u"wgc") {
            return 2;
        }
        const bool recover = argc == 5;
        if (recover && QString::fromLocal8Bit(argv[4]) != u"recover") {
            return 2;
        }
        SnowCaptureDirectRecordingConfig config{};
        config.version = SNOW_CAPTURE_DIRECT_RECORDING_CONFIG_VERSION;
        config.struct_size = sizeof(config);
        config.mouse_highlight_radius = 41;
        config.width = 640;
        config.height = 480;
        config.capture_backend = backend == u"dxgi" ? 1 : 2;
        config.output_file_utf8 = path.constData();
        config.capture_fps = 30;
        config.output_fps = 30;
        config.preset = 1;
        config.quality = 80;
        config.encoder_preference = 1;
        config.enable_system_audio = 1;
        config.show_cursor = 1;
        config.mouse_trail_duration_ms = 500;
        config.keyboard_size = 64;
        const auto result = snow_recording_gpu_probe(&config, recover ? 1 : 0);
        if (result != SNOW_RECORDING_RESULT_OK) {
            qWarning().noquote() << snow_recording_last_error_message();
        }
        return result == SNOW_RECORDING_RESULT_OK ? 0 : 1;
    }
    // A probe runs before diagnostics, singleton acquisition, or any live user-state access.
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == u"--update-probe") {
        if (QString::fromLocal8Bit(argv[2]) != QCoreApplication::applicationVersion()) {
            return 3;
        }
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication probe(argc, argv);
        adqt::widgets::initializePlatformCompatibility(probe);
        QTemporaryDir directory;
        if (!directory.isValid()) {
            return 4;
        }
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (!storage.initialize({directory.path(), directory.path(), 8000}).success) {
            return 5;
        }
        snow_shot::presentation::LanguageManager::instance().initialize();
        const auto startupSettings = snow_shot::storage::SystemSettings();
        const auto startupResult = snow_shot::platform::windows::reconcileStartupMode(
            !startupSettings.autoStartAtBoot() ? snow_shot::platform::windows::StartupMode::Off
            : startupSettings.launchAsAdministrator()
                ? snow_shot::platform::windows::StartupMode::ElevatedTask
                : snow_shot::platform::windows::StartupMode::Registry);

        snow_shot::presentation::styles::ThemeManager::instance().initialize(probe);
        storage.shutdown();
        return 0;
    }
#ifdef Q_OS_MACOS
    // Exercise normal pre-application startup without touching live user state.
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == u"--startup-probe") {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication probe(argc, argv);
        adqt::widgets::initializePlatformCompatibility(probe);
        return 0;
    }
#endif
    struct DiagnosticsLifetime {
        bool started = false;
        ~DiagnosticsLifetime() {
            if (!started)
                return;
            snow_shot::diagnostics::stopFfmpegDiagnostics();
            snow_shot::diagnostics::DiagnosticsService::instance().shutdown();
        }
    } diagnosticsLifetime;
    // Capture overlays embed native window-type children (the floating tool
    // palette) alongside alien, click-through children (selection toolbar,
    // shortcut hints). Qt would otherwise force every sibling of an embedded
    // native child native, which silently turns those click-through surfaces
    // into OS-level input interceptors.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

#ifdef Q_OS_MACOS
    snow_shot::platform::macos::configureRasterColorSpace();
    snow_shot::platform::macos::observeNativeLoginItemLaunch();
#endif
    // The internal application name also owns settings and single-instance keys.
    // Keep it stable while giving Qt windows and the macOS menu the product name.
    QGuiApplication::setApplicationDisplayName(snow_shot::app::edition::productName());
    QApplication app(argc, argv);
    // Forwarding processes need only Qt and the instance protocol. Storage probes,
    // helper processes and display discovery belong to the primary instance.
    auto launchArguments =
        applicationRestart ? snow_shot::app::normalApplicationArguments(QApplication::arguments())
                           : QApplication::arguments();
    const bool updaterRelaunch = snow_shot::update::isStartupUpdateRelaunch(launchArguments);
    launchArguments = snow_shot::update::normalStartupArguments(std::move(launchArguments));
#ifdef Q_OS_MACOS
    launchArguments = snow_shot::platform::macos::loginItemLaunchArguments(
        launchArguments, snow_shot::platform::macos::initialNativeLoginItemLaunch());
#endif
    snow_shot::app::SingleInstanceCoordinator singleInstance;
    const snow_shot::app::SingleInstanceResult instanceResult = singleInstance.acquireOrForward(
        launchArguments,
        snow_shot::app::parseShellImageLaunchRequest(launchArguments).requested ? 10000 : 1500);
    if (instanceResult.outcome == snow_shot::app::SingleInstanceOutcome::Forwarded) {
        return 0;
    }
    if (instanceResult.outcome == snow_shot::app::SingleInstanceOutcome::Failed) {
        qWarning().noquote() << instanceResult.error;
        return 2;
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &singleInstance,
                     [&singleInstance] { singleInstance.stopAcceptingLaunchRequests(); });

#ifndef Q_OS_MACOS
    // macOS updates use a downloaded DMG, not the standalone transaction helper.
    if (!snow_shot::app::distribution::isStoreManaged()) {
        QString executablePath = QString::fromLocal8Bit(argv[0]);
#ifdef Q_OS_WIN
        wchar_t modulePath[32768]{};
        const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, 32768);
        if (moduleLength > 0 && moduleLength < 32768) {
            executablePath = QString::fromWCharArray(modulePath, static_cast<int>(moduleLength));
        }
#endif
        const QString updateRoot = updateInstallationRoot(QFileInfo(executablePath).absolutePath());
        const QString updateHelper = QDir(QFileInfo(executablePath).absolutePath())
                                         .filePath(snow_shot::app::edition::updaterName());
        const auto pendingUpdate = updateTransactionPending(updateHelper, updateRoot);
        if (!pendingUpdate.has_value()) {
            return 6;
        }
        if (*pendingUpdate) {
            const QString root = updateInstallationRoot(QCoreApplication::applicationDirPath());
            const QString pipe =
                QStringLiteral("snow-shot-recover-") + QUuid::createUuid().toString(QUuid::Id128);
            QLocalServer server;
            server.setSocketOptions(QLocalServer::UserAccessOption);
            if (!server.listen(pipe)) {
                return 6;
            }
            const QString result =
                QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                         .canonicalPath())
                    .filePath(pipe + QStringLiteral(".txt"));
            const QStringList args{
                QStringLiteral("--launch"), QStringLiteral("--recovery"),
                QStringLiteral("--target"), root,
                QStringLiteral("--parent"), QString::number(QCoreApplication::applicationPid()),
                QStringLiteral("--pipe"),   pipe,
                QStringLiteral("--result"), result};
            if (!QProcess::startDetached(updateHelper, args)) {
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
    }
#endif
    auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
    snow_shot::storage::StorageInitializationOptions storageOptions;
    storageOptions.resolvedDirectory =
        std::make_shared<snow_shot::storage::StorageDirectorySelection>(
            snow_shot::storage::ApplicationStorage::resolveDirectory());
    const auto& selectedStorage = *storageOptions.resolvedDirectory;
    auto diagnosticsOptions =
        startupDiagnosticsOptions(selectedStorage.effectiveDirectory, applicationName);
#ifdef Q_OS_MACOS
    diagnosticsOptions.handlerPath =
        QDir(selectedStorage.executableDirectory).filePath(QStringLiteral("crashpad_handler"));
#else
    diagnosticsOptions.handlerPath =
        QDir(selectedStorage.executableDirectory).filePath(QStringLiteral("crashpad_handler.exe"));
#endif
    diagnosticsOptions.version = QStringLiteral(SNOW_DIAGNOSTICS_VERSION);
    diagnosticsOptions.revision = QStringLiteral(SNOW_DIAGNOSTICS_REVISION);
    diagnosticsOptions.buildConfiguration = QStringLiteral(SNOW_DIAGNOSTICS_BUILD);
    diagnosticsLifetime.started = true;
    static_cast<void>(diagnostics.initialize(std::move(diagnosticsOptions)));
    snow_diagnostics_install_panic_hook(snow_diag_panic);
    snow_shot::diagnostics::installFfmpegDiagnostics();
#ifdef Q_OS_MACOS
    snow_recording_macos_set_microphone_diagnostic_callback(
        snow_shot::diagnostics::recordMicrophoneFailure);
#endif

    adqt::widgets::initializePlatformCompatibility(app);
    snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.app"),
                                     QStringLiteral("application.platform"),
                                     {{QStringLiteral("backend"), QGuiApplication::platformName()},
                                      {QStringLiteral("os"), QSysInfo::kernelVersion()}});
#ifdef Q_OS_MACOS
    const auto logDisplay = [](QScreen* screen) {
        const QRect geometry = screen->geometry();
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.platform"), QStringLiteral("display.configuration"),
            {{QStringLiteral("width"), geometry.width()},
             {QStringLiteral("height"), geometry.height()},
             {QStringLiteral("x"), geometry.x()},
             {QStringLiteral("y"), geometry.y()},
             {QStringLiteral("scale"), screen->devicePixelRatio()},
             {QStringLiteral("refresh_rate"), screen->refreshRate()},
             {QStringLiteral("count"), QGuiApplication::screens().size()}});
    };
    const auto observeDisplay = [&app, logDisplay](QScreen* screen) {
        logDisplay(screen);
        QObject::connect(screen, &QScreen::geometryChanged, &app,
                         [screen, logDisplay] { logDisplay(screen); });
        QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, &app,
                         [screen, logDisplay] { logDisplay(screen); });
        QObject::connect(screen, &QScreen::refreshRateChanged, &app,
                         [screen, logDisplay] { logDisplay(screen); });
    };
    for (auto* screen : QGuiApplication::screens())
        observeDisplay(screen);
    QObject::connect(&app, &QGuiApplication::screenAdded, &app, observeDisplay);
    QObject::connect(&app, &QGuiApplication::screenRemoved, &app, [] {
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.platform"), QStringLiteral("display.removed"),
            {{QStringLiteral("count"), QGuiApplication::screens().size()}});
    });
#endif
    static_cast<void>(snow_shot::presentation::capture::resolveAutoScreenshotApiMode());
#if defined(SNOW_SHOT_PIN_PERF_INSTRUMENTATION)
    snow_shot::presentation::pin_perf::configureTrace(
        qEnvironmentVariable("SNOW_SHOT_PIN_PERF_TRACE"));
#endif
#if defined(SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION)
    snow_shot::presentation::capture_perf::configureTrace(
        qEnvironmentVariable("SNOW_SHOT_CAPTURE_PERF_TRACE"));
#endif
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
            // Controllers have retired capture before this terminal join. Keep storage,
            // diagnostics and Qt alive, without dispatching timers owned by those controllers.
            snow_shot::capture_detail::ScreenshotScrollingPipeline::waitForShutdowns();
            shutdownScreenshotHistoryTasks();
        }
    } historyTaskDrain;
#ifndef Q_OS_MACOS
    static_cast<void>(snow_shot::presentation::settings::applyConfiguredApplicationPriority());
#endif
    QApplication::setQuitOnLastWindowClosed(false);
#ifndef Q_OS_MACOS
    QApplication::setWindowIcon(
        adqt::icons::makeIcon(snow_shot::presentation::icons::custom::app::ApplicationIcon()));
#endif
    adqt::locale::LocaleManager::instance().applyTo(app);
    snow_shot::presentation::LanguageManager::instance().initialize();
    snow_shot::presentation::styles::ThemeManager::instance().initialize(app);
#ifdef Q_OS_WIN
    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    if (!snow_shot::app::distribution::isStoreManaged() && !updaterRelaunch &&
        snow_shot::update::startupUpdateMayDelayLaunch(launchArguments) &&
        applicationStorage.configuration().value(QStringLiteral("updates/mode")).toString() ==
            u"next_launch") {
        snow_shot::update::UpdateService startupUpdate(
            snow_shot::update::defaultUpdateServiceOptions());
        startupUpdate.setMode(QStringLiteral("next_launch"));
        startupUpdate.setProgressAppearance(
            snow_shot::presentation::styles::ThemeManager::instance().updateProgressAppearance());
        QObject::connect(&adqt::theme::ThemeManager::instance(),
                         &adqt::theme::ThemeManager::themeChanged, &startupUpdate,
                         [&startupUpdate] {
                             startupUpdate.setProgressAppearance(
                                 snow_shot::presentation::styles::ThemeManager::instance()
                                     .updateProgressAppearance());
                         });
        startupUpdate.setSystemProxy(
            applicationStorage.configuration().value(QStringLiteral("network/proxy")).toString() ==
            u"system");
        bool foregroundRequested =
            administratorRestart || launchArguments.contains(QStringLiteral("--show-main-window"));
        const auto initialImageRequest =
            snow_shot::app::parseShellImageLaunchRequest(launchArguments);
        QStringList pendingImagePaths =
            initialImageRequest.valid ? initialImageRequest.paths : QStringList{};
        const auto relaunchArguments = [&] {
            return snow_shot::update::startupUpdateRelaunchArguments(foregroundRequested,
                                                                     pendingImagePaths);
        };
        const auto forwardedLaunch = QObject::connect(
            &singleInstance, &snow_shot::app::SingleInstanceCoordinator::launchRequestReceived,
            &app, [&](const QStringList& arguments) {
                const auto imageRequest = snow_shot::app::parseShellImageLaunchRequest(arguments);
                if (imageRequest.valid)
                    pendingImagePaths.append(imageRequest.paths);
                foregroundRequested =
                    foregroundRequested ||
                    arguments.contains(QStringLiteral("--show-main-window")) ||
                    (!imageRequest.requested && !arguments.contains(QStringLiteral("--autostart")));
                startupUpdate.setRelaunchArguments(relaunchArguments());
            });
        const auto startupUpdateResult = snow_shot::update::runStartupUpdate(
            startupUpdate, [&applicationStorage] { return applicationStorage.flushNow().success; },
            [&] { return foregroundRequested; }, std::chrono::seconds(210), relaunchArguments);
        QObject::disconnect(forwardedLaunch);
        if (startupUpdateResult == snow_shot::update::StartupUpdateResult::ExitForUpdate) {
            singleInstance.stopAcceptingLaunchRequests();
            return 0;
        }
    }
#else
    static_cast<void>(updaterRelaunch);
#endif
    adqt::widgets::AdTooltip::installApplicationTooltips();

    snow_shot::app::ApplicationController applicationController(app);
    applicationController.start(launchArguments, [&] {
        const auto startupSettings = snow_shot::storage::SystemSettings();
#ifdef Q_OS_WIN
        QString shellRegistrationError;
        const snow_shot::platform::windows::ShellContextMenuRegistration shellRegistration;
        const auto contextMenuShortcuts = snow_shot::shortcuts::shortcutBindingsFromJson(
            snow_shot::storage::ApplicationStorage::instance().configuration().value(
                QStringLiteral("context_menu_shortcuts/edit_with_snow_shot")),
            false);
        if (!shellRegistration.setMode(
                snow_shot::platform::windows::ShellContextMenuRegistration::modeFromValue(
                    snow_shot::storage::ApplicationStorage::instance()
                        .configuration()
                        .value(QStringLiteral("system/context_menu_mode"))
                        .toString()),
                contextMenuShortcuts.isEmpty() ? QString()
                                               : contextMenuShortcuts.first().portableText,
                &shellRegistrationError)) {
            snow_shot::app::reportStartupRegistrationFailure(shellRegistrationError);
        }
#endif
#ifdef Q_OS_MACOS
        const auto startupResult =
            snow_shot::platform::macos::loginItemAutomaticRegistrationAllowed(launchArguments)
                ? snow_shot::platform::macos::loginItemService().initialize(
                      startupSettings.autoStartAtBoot())
                : snow_shot::platform::macos::LoginItemResult{};
#else
        const auto startupResult = snow_shot::platform::windows::reconcileStartupMode(
            !startupSettings.autoStartAtBoot() ? snow_shot::platform::windows::StartupMode::Off
            : startupSettings.launchAsAdministrator()
                ? snow_shot::platform::windows::StartupMode::ElevatedTask
                : snow_shot::platform::windows::StartupMode::Registry);
#endif
        if (!startupResult.success) {
            snow_shot::app::reportStartupRegistrationFailure(startupResult.error);
#ifdef Q_OS_MACOS
            QTimer::singleShot(0, &app, [&applicationController, error = startupResult.error] {
                // Startup can otherwise have only a tiny floating toolbar visible.
                // Give the error a readable application surface before opening it.
                applicationController.showMainWindow();
                adqt::widgets::AdMessage::Request request;
                request.content = error;
                adqt::widgets::AdMessageService::error(std::move(request));
            });
#endif
        }
    });
    QTimer::singleShot(0, &app, [&] {
        singleInstance.setLaunchRequestHandler(
            [&applicationController](const QStringList& arguments) {
                applicationController.handleLaunchRequest(
                    snow_shot::update::normalStartupArguments(arguments));
            });
    });
    if (administratorRestart)
        applicationController.showMainWindow();
    snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.app"),
                                     QStringLiteral("application.ready"));
    if (!launchArguments.contains(QStringLiteral("--autostart")) &&
        launchArguments.contains(QStringLiteral("--show-main-window"))) {
        applicationController.showMainWindow();
    }
    return QApplication::exec();
}
