#include "snow_shot/diagnostics/diagnostics.h"
#include "diagnosticsbridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <Windows.h>
#include <malloc.h>
#else
#include <sys/mman.h>
#endif

using namespace snow_shot::diagnostics;
extern "C" void snow_test_rust_panic(void (*callback)(const unsigned char*, size_t));
extern "C" void snow_test_ocr_initialize();
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
#ifdef Q_OS_WIN
#define SNOW_NOINLINE __declspec(noinline)
#else
#define SNOW_NOINLINE __attribute__((noinline))
#endif
SNOW_NOINLINE void exhaustStack(unsigned depth) {
    volatile char page[4096]{};
    page[depth % sizeof(page)] = static_cast<char>(depth);
    void (*volatile recurse)(unsigned) = exhaustStack;
    recurse(depth + 1);
    page[0] = page[depth % sizeof(page)];
}
void* inaccessiblePage() {
#ifdef Q_OS_WIN
    return VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
#else
    void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    require(page != MAP_FAILED, "allocate inaccessible page");
    return page;
#endif
}
int fixture(const QStringList& arguments) {
#ifdef Q_OS_WIN
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    DiagnosticsOptions options;
    options.directories = {arguments.at(2)};
    options.handlerPath = QStringLiteral(SNOW_TEST_CRASHPAD_HANDLER);
    options.mirrorToConsole = false;
    DiagnosticsService service;
    require(service.initialize(options), "fixture logger starts");
    require(service.status().crashCaptureAvailable, "fixture collector starts");
    service.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("crash.breadcrumb"));
    require(service.flush(), "pre-crash flush");
    const QString kind = arguments.at(3);
    if (kind == QStringLiteral("fatal"))
        qFatal("intentional diagnostic test");
    if (kind == QStringLiteral("abort"))
        std::abort();
    if (kind == QStringLiteral("terminate"))
        std::terminate();
    if (kind == QStringLiteral("panic")) {
        snow_test_rust_panic(snow_diag_panic);
    }
    if (kind.startsWith(QStringLiteral("ocr-"))) {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("SNOW_SHOT_CRASHPAD_PIPE"), service.crashPipeName());
        environment.insert(QStringLiteral("SNOW_SHOT_DIAGNOSTICS_SESSION"),
                           service.status().sessionId);
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--ocr-fixture"), kind});
        require(child.waitForFinished(20000), "OCR child finishes");
        require(child.readAllStandardOutput() == QByteArrayLiteral("IPC"),
                "child diagnostics preserve stdout IPC");
        require(child.exitCode() != 0, "OCR crash terminates child");
        return 4;
    }
    if (kind == QStringLiteral("stack")) {
#ifdef Q_OS_WIN
        ULONG guarantee = 64 * 1024;
        SetThreadStackGuarantee(&guarantee);
#endif
        exhaustStack(0);
    }
    if (kind == QStringLiteral("access")) {
        void* allocation = inaccessiblePage();
        *static_cast<volatile char*>(allocation) = 1;
    }
    return 3;
}
void verifyEmergencyLogging() {
    QTemporaryDir directory(QDir(QDir::tempPath()).canonicalPath() +
                            QStringLiteral("/snow-diag-XXXXXX"));
    const QString first = directory.filePath(QStringLiteral("first.log"));
    const QString second = directory.filePath(QStringLiteral("second.log"));
    snow_diag_prepare("emergency-test", "1.0", "test");
    snow_diag_open_emergency(first.toUtf8().constData());
    const QByteArray large(16384, 'x');
    for (int i = 0; i < 10; ++i)
        snow_diag_emergency(large.constData(), static_cast<size_t>(large.size()));
    require(QFileInfo(first).size() == 64 * 1024, "ordinary emergency writes are bounded");
    snow_diag_fatal("test.fatal");
    require(QFileInfo(first).size() > 64 * 1024, "fatal evidence survives the emergency limit");
    snow_diag_open_emergency(second.toUtf8().constData());
    snow_diag_fatal("test.rotated");
    snow_diag_shutdown();
    QFile file(second);
    require(file.open(QIODevice::ReadOnly), "rotated emergency file opens");
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    require(object.value(QStringLiteral("event")) == QStringLiteral("test.rotated") &&
                object.value(QStringLiteral("session")) == QStringLiteral("emergency-test"),
            "fatal emergency record retains event and session as valid JSON");
    const auto size = file.size();
    snow_diag_fatal("test.closed");
    require(file.size() == size, "shutdown retires emergency handle");
#ifdef Q_OS_MACOS
    const QString link = directory.filePath(QStringLiteral("link.log"));
    require(QFile::link(second, link), "create emergency symlink fixture");
    snow_diag_open_emergency(link.toUtf8().constData());
    snow_diag_fatal("test.symlink");
    require(file.size() == size, "emergency writer refuses symlinks");
    snow_diag_shutdown();
#endif
    DiagnosticsOptions options;
    options.directories = {directory.path()};
    options.handlerPath = directory.filePath(QStringLiteral("missing-handler"));
    options.mirrorToConsole = false;
    DiagnosticsService service;
    require(service.initialize(options), "missing collector does not disable ordinary logging");
    require(!service.status().crashCaptureAvailable, "missing collector is reported accurately");
    service.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("collector.unavailable"));
    require(service.flush(), "logging remains usable without collector");
}

void verifyCrash(const QString& kind) {
    std::fprintf(stderr, "Testing crash fixture: %s\n", qPrintable(kind));
    QTemporaryDir directory(QDir(QDir::tempPath()).canonicalPath() +
                            QStringLiteral("/snow-diag-XXXXXX"));
    QProcess process;
    process.start(QCoreApplication::applicationFilePath(),
                  {QStringLiteral("--fixture"), directory.path(), kind});
    require(process.waitForStarted(5000), "fixture starts");
    if (!process.waitForFinished(25000)) {
        process.kill();
        process.waitForFinished(5000);
        throw std::runtime_error("crash fixture timed out");
    }
    require(process.exitCode() != 0 || process.exitStatus() == QProcess::CrashExit,
            "fixture terminates abnormally");
    auto collector = makeCrashCollector();
    QString error;
    require(collector->initialize(QDir(directory.path()).filePath(QStringLiteral("crashes")), {},
                                  {}, &error),
            "open crash database");
    auto reports = collector->reports();
    // macOS writes the report after the kernel delivers EXC_CRASH at process exit.
    for (int attempt = 0; reports.isEmpty() && attempt < 100; ++attempt) {
        QThread::msleep(50);
        reports = collector->reports();
    }
    if (reports.isEmpty()) {
        std::fprintf(stderr, "fixture %s: %s\n", qPrintable(kind),
                     process.readAllStandardError().constData());
    }
    require(reports.size() == 1, "exactly one dump per crash");
    QFile dump(reports.front().path);
    require(dump.open(QIODevice::ReadOnly), "dump readable");
    const QByteArray data = dump.readAll();
    dump.close();
    require(data.startsWith("MDMP") && data.size() > 32, "valid minidump header");
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
    const quint32 count = qFromLittleEndian<quint32>(bytes + 8);
    const quint32 offset = qFromLittleEndian<quint32>(bytes + 12);
    bool exception = false;
    for (quint32 i = 0; i < count; ++i) {
        const quint64 position = static_cast<quint64>(offset) + i * 12ULL;
        require(position + 12 <= static_cast<quint64>(data.size()), "valid stream directory");
        if (qFromLittleEndian<quint32>(bytes + position) == 6)
            exception = true;
    }
    require(exception, "dump contains an exception stream");
    require(reports.front().context.contains(QStringLiteral("exception_code")),
            "readable crash exception summary");
    require(data.contains("Snow Shot") && data.contains("crash.breadcrumb"),
            "build identity and recent event survive");
    if (kind == QStringLiteral("fatal") || kind == QStringLiteral("terminate") ||
        kind == QStringLiteral("panic")) {
        QByteArray emergency;
        QDirIterator files(directory.path(), {QStringLiteral("emergency-*.log")}, QDir::Files,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            require(file.open(QIODevice::ReadOnly), "emergency log readable");
            emergency += file.readAll();
        }
        require(emergency.contains("runtime.panic"),
                "fatal evidence persisted independently of queue");
        require(!emergency.contains("private panic payload"), "panic payload excluded from logs");
    }
    DiagnosticsOptions recoveryOptions;
    recoveryOptions.directories = {directory.path()};
    recoveryOptions.mirrorToConsole = false;
    DiagnosticsService recovery;
    require(recovery.initialize(recoveryOptions), "logger recovers after crash");
    const auto exported = recovery.exportDay(QDate::currentDate()).get();
    require(exported.success, "crash day can be exported");
    QFile snapshot(exported.path);
    require(snapshot.open(QIODevice::ReadOnly), "crash day export readable");
    require(snapshot.readAll().contains("crash.summary"), "crash summary included in daily export");
    recovery.shutdown();
    require(collector->removeReport(reports.front().id), "database API deletes report");
    require(collector->reports().isEmpty(), "report metadata removed with dump");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().value(1) == QStringLiteral("--ocr-fixture")) {
#ifdef Q_OS_WIN
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
            snow_test_ocr_initialize();
            constexpr char breadcrumb[] = "crash.breadcrumb OCR";
            snow_diag_breadcrumb(breadcrumb, sizeof(breadcrumb) - 1);
            std::fwrite("IPC", 1, 3, stdout);
            std::fflush(stdout);
            if (app.arguments().value(2) == QStringLiteral("ocr-panic"))
                snow_test_rust_panic(snow_diag_panic);
            void* allocation = inaccessiblePage();
            *static_cast<volatile char*>(allocation) = 1;
            return 5;
        }
        if (app.arguments().value(1) == QStringLiteral("--fixture"))
            return fixture(app.arguments());
        verifyEmergencyLogging();
        for (const QString& kind :
             {QStringLiteral("access"), QStringLiteral("stack"), QStringLiteral("fatal"),
              QStringLiteral("abort"), QStringLiteral("terminate"), QStringLiteral("panic"),
              QStringLiteral("ocr-access"), QStringLiteral("ocr-panic")}) {
            verifyCrash(kind);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
