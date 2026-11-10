#include "../src/app/updateprobe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void probeInvocationCannotFallThroughToLiveStartup() {
    using snow_shot::app::inspectUpdateProbeRequest;
    const QString version = QStringLiteral("1.5.3");
    const char* const normal[]{"snow-shot", "--show-main-window"};
    require(!inspectUpdateProbeRequest(1, normal, version) &&
                !inspectUpdateProbeRequest(2, normal, version),
            "ordinary application invocations retain normal startup");
    const char* const valid[]{"snow-shot", "--update-probe", "1.5.3"};
    require(inspectUpdateProbeRequest(3, valid, version) == 0,
            "matching version admits the isolated probe");
    const char* const mismatch[]{"snow-shot", "--update-probe", "1.5.2"};
    require(inspectUpdateProbeRequest(3, mismatch, version) == 3,
            "version mismatch preserves the updater's existing failure code");
    const char* const missing[]{"snow-shot", "--update-probe"};
    const char* const extra[]{"snow-shot", "--update-probe", "1.5.3", "--show-main-window"};
    require(inspectUpdateProbeRequest(2, missing, version) == 64 &&
                inspectUpdateProbeRequest(4, extra, version) == 64,
            "malformed probe invocations reject before QApplication or live startup");
}

void probeUsesOnlyTemporaryStorage() {
    QTemporaryDir liveDirectory;
    require(liveDirectory.isValid(), "isolated live-state sentinel");
    const QString liveRegistration = liveDirectory.filePath(QStringLiteral("startup-registration"));
    QFile sentinel(liveRegistration);
    require(sentinel.open(QIODevice::WriteOnly), "write startup-registration sentinel");
    sentinel.write("existing user registration");
    sentinel.close();

    QString probeRoot;
    QStringList sequence;
    const auto result = snow_shot::app::runIsolatedUpdateProbe(
        {[&](const QString& root) {
             probeRoot = root;
             require(QDir(root).exists() && root != liveDirectory.path(),
                     "probe storage is a private temporary directory");
             sequence.append(QStringLiteral("storage"));
             QFile settings(QDir(root).filePath(QStringLiteral("settings")));
             return settings.open(QIODevice::WriteOnly) && settings.write("defaults") == 8;
         },
         [&] {
             require(QFile::exists(QDir(probeRoot).filePath(QStringLiteral("settings"))),
                     "presentation sees initialized isolated settings");
             sequence.append(QStringLiteral("presentation"));
         },
         [&] { sequence.append(QStringLiteral("shutdown")); }});
    require(result == 0 &&
                sequence == QStringList{QStringLiteral("storage"), QStringLiteral("presentation"),
                                        QStringLiteral("shutdown")},
            "probe initializes and shuts down storage exactly once");
    require(!QDir(probeRoot).exists(), "probe removes temporary state after shutdown");
    require(sentinel.open(QIODevice::ReadOnly) &&
                sentinel.readAll() == "existing user registration",
            "probe preserves the live startup-registration sentinel");
}

void failedInitializationDoesNotStartPresentation() {
    int presentation = 0;
    int shutdown = 0;
    require(snow_shot::app::runIsolatedUpdateProbe({[](const QString&) { return false; },
                                                    [&] { ++presentation; },
                                                    [&] { ++shutdown; }}) == 5 &&
                presentation == 0 && shutdown == 1,
            "failed storage initialization aborts presentation and releases partial storage");
    shutdown = 0;
    bool threw = false;
    try {
        snow_shot::app::runIsolatedUpdateProbe(
            {[](const QString&) { return true; },
             [] { throw std::runtime_error("presentation failure"); }, [&] { ++shutdown; }});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw && shutdown == 1,
            "presentation failure propagates after releasing probe storage exactly once");
    shutdown = 0;
    threw = false;
    QString partialRoot;
    bool rootPresentAtShutdown = false;
    try {
        snow_shot::app::runIsolatedUpdateProbe(
            {[&](const QString& root) -> bool {
                 partialRoot = root;
                 throw std::runtime_error("partially initialized storage failure");
             },
             [&] { ++presentation; },
             [&] {
                 rootPresentAtShutdown = QDir(partialRoot).exists();
                 ++shutdown;
             }});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw && shutdown == 1 && presentation == 0 && rootPresentAtShutdown &&
                !QDir(partialRoot).exists(),
            "throwing storage initialization releases partial resources and temporary state once");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        probeInvocationCannotFallThroughToLiveStartup();
        probeUsesOnlyTemporaryStorage();
        failedInitializationDoesNotStartPresentation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
