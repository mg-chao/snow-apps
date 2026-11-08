#include "snow_shot/diagnostics/diagnosticspaths.h"
#include "snow_shot/platform/minizippath.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QLockFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <cstdio>
#include <atomic>
#include <stdexcept>
#include <thread>

using namespace snow_shot::diagnostics;
namespace {
QString temporaryTemplate() {
    return QDir(QDir::tempPath()).canonicalPath() + QStringLiteral("/snow-diag-export-XXXXXX");
}

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void put(const QString& path, const QByteArray& contents) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size(),
            "write fixture");
}
QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read file");
    return file.readAll();
}
QMap<QString, QByteArray> unzip(const QString& path) {
    void* reader = mz_zip_reader_create();
    const auto cleanup = qScopeGuard([&] { mz_zip_reader_delete(&reader); });
    require(reader && mz_zip_reader_open_file(
                          reader, snow_shot::platform::minizipPath(path).constData()) == MZ_OK,
            "export is a readable ZIP");
    QMap<QString, QByteArray> result;
    int32_t code = mz_zip_reader_goto_first_entry(reader);
    while (code == MZ_OK) {
        mz_zip_file* entry = nullptr;
        require(mz_zip_reader_entry_get_info(reader, &entry) == MZ_OK && entry,
                "read archive entry metadata");
        const QString name = QString::fromUtf8(entry->filename);
        require(!result.contains(name), "archive filenames are unique");
        require(mz_zip_reader_entry_open(reader) == MZ_OK, "open archive entry");
        QByteArray contents;
        char buffer[4096];
        int32_t count = 0;
        while ((count = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer))) > 0)
            contents.append(buffer, count);
        require(count == 0 && mz_zip_reader_entry_close(reader) == MZ_OK,
                "entry contents and CRC are valid");
        result.insert(name, contents);
        code = mz_zip_reader_goto_next_entry(reader);
    }
    require(code == MZ_END_OF_LIST && mz_zip_reader_close(reader) == MZ_OK, "close ZIP");
    return result;
}
DiagnosticsOptions optionsFor(const QString& root) {
    DiagnosticsOptions options;
    options.directories = {root};
    options.installMessageHandler = false;
    options.enableCrashCapture = false;
    options.mirrorToConsole = false;
    return options;
}
class FixtureCollector final : public CrashCollector {
  public:
    QString dump;
    bool initialize(const QString&, const QString&, const QString&, QString*) override {
        return true;
    }
    QVector<CrashReport> reports() override {
        // total_size may include attachments/metadata, rather than just the dump.
        return {{QStringLiteral("report-id"),
                 dump,
                 QDateTime::currentDateTimeUtc(),
                 QFileInfo(dump).size() + 100,
                 {{QStringLiteral("exception_code"), QStringLiteral("0xc0000005")}}}};
    }
    bool removeReport(const QString&) override {
        return false;
    }
    QString pipeName() const override {
        return {};
    }
};

void pathSelection() {
    QTemporaryDir directory(temporaryTemplate());
    DiagnosticsPathOptions paths{true,
                                 directory.filePath(QStringLiteral("custom")),
                                 directory.filePath(QStringLiteral("roaming")),
                                 directory.filePath(QStringLiteral("temp")),
                                 QStringLiteral("snow_shot"),
                                 {}};
    const auto unified = diagnosticsOptionsForPaths(paths);
    require(unified.directories == QStringList{QDir(paths.stableDirectory).filePath("logs")} &&
                unified.crashCaptureDirectory ==
                    QDir(paths.stableDirectory).filePath("logs/crashes"),
            "unified diagnostics ignore custom and temporary locations");
    for (const auto& unavailable : {QString(), QStringLiteral("relative")}) {
        paths.stableDirectory = unavailable;
        const auto options = diagnosticsOptionsForPaths(paths);
        require(options.directories.isEmpty() && options.crashCaptureDirectory.isEmpty(),
                "missing unified root never falls back to another location");
    }
    paths.unifiedApplicationData = false;
    paths.stableDirectory = directory.filePath(QStringLiteral("LocalState"));
    paths.packageFamily = QStringLiteral("Package_family");
    const auto store = diagnosticsOptionsForPaths(paths);
    require(store.directories.size() == 3 &&
                store.directories.at(0) == QDir(paths.selectedDirectory).filePath("logs") &&
                store.directories.at(1) == QDir(paths.stableDirectory).filePath("logs") &&
                store.directories.at(2).contains(QStringLiteral("Package_family/logs")),
            "Store retains its selected, package-owned, and isolated fallback paths");
    paths.stableDirectory.clear();
    require(diagnosticsOptionsForPaths(paths).directories.isEmpty(),
            "missing Store storage remains unavailable");

    const QString organization = QCoreApplication::organizationName();
    const QString application = QCoreApplication::applicationName();
    const auto restore = qScopeGuard([&] {
        QCoreApplication::setOrganizationName(organization);
        QCoreApplication::setApplicationName(application);
    });
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShot"));
    QCoreApplication::setApplicationName(QStringLiteral("snow_shot"));
    paths.stableDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    paths.unifiedApplicationData = true;
    paths.packageFamily.clear();
    const auto full = diagnosticsOptionsForPaths(paths);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotMini"));
    QCoreApplication::setApplicationName(QStringLiteral("snow_shot_mini"));
    paths.stableDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto mini = diagnosticsOptionsForPaths(paths);
    require(full.directories != mini.directories &&
                full.directories.front().contains(QStringLiteral("snow_shot")) &&
                mini.directories.front().contains(QStringLiteral("snow_shot_mini")),
            "standalone and Mini keep separate roaming diagnostics roots");
}

void archiveIncludesRetainedEvidenceAndSurvivesPublication() {
    QTemporaryDir directory(temporaryTemplate());
    QTemporaryDir fallback(temporaryTemplate());
    const QString yesterday = QDate::currentDate().addDays(-1).toString(Qt::ISODate);
    const QString log =
        yesterday + QStringLiteral("/snow-shot-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-000001.log");
    const QString emergency =
        yesterday + QStringLiteral("/emergency-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.log");
    put(directory.filePath(log), "previous day\n");
    const QString rotated =
        yesterday + QStringLiteral("/snow-shot-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-000002.log");
    put(directory.filePath(rotated), "rotated segment\n");
    put(fallback.filePath(log), "fallback evidence\n");
    put(directory.filePath(emergency), "emergency evidence\n");
    put(directory.filePath(QStringLiteral("internal.json")), "excluded control");
    auto collector = std::make_shared<FixtureCollector>();
    collector->dump = directory.filePath(QStringLiteral("crashes/reports/crash.dmp"));
    put(collector->dump, "MDMP fixture");
    put(directory.filePath(QStringLiteral("crashes/metadata")), "excluded live database");
    auto options = optionsFor(directory.path());
    options.directories.append(fallback.path());
    options.enableCrashCapture = true;
    options.crashCollector = collector;
    DiagnosticsService service;
    require(service.initialize(options), "archive logger initializes");
    service.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("current.evidence"));
    const auto snapshot = service.exportDay(QDate::currentDate()).get();
    require(snapshot.success, "create old daily snapshot to exclude");
    const auto exported = service.exportLogs().get();
    require(exported.success && exported.path.endsWith(QStringLiteral(".zip")),
            "all-log export succeeds");
    const auto entries = unzip(exported.path);
    require(entries.values().contains("previous day\n") &&
                entries.values().contains("fallback evidence\n") && entries.contains(log) &&
                entries.contains(log + QStringLiteral("-2")) &&
                entries.value(emergency) == "emergency evidence\n",
            "all dates, rotated logs, fallback collisions, and emergency bytes are preserved");
    require(entries.value(rotated) == "rotated segment\n", "every rotated segment is included");
    require(entries.value(QStringLiteral("crashes/crash.dmp")) == "MDMP fixture" &&
                QJsonDocument::fromJson(entries.value(QStringLiteral("crashes/crash.dmp.json")))
                        .object()
                        .value(QStringLiteral("exception_code"))
                        .toString() == QStringLiteral("0xc0000005"),
            "dump bytes and crash context are exported");
    bool current = false;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        current = current || it.value().contains("current.evidence");
        require(!it.key().contains(QStringLiteral("exports/")) &&
                    !it.key().contains(QStringLiteral("metadata")) &&
                    !it.key().endsWith(QStringLiteral(".lock")) &&
                    !it.key().endsWith(QStringLiteral(".open")) && it.key() != u"internal.json",
                "archives and runtime control/database files are excluded");
    }
    require(current, "queued current-session records are flushed into ZIP");
    const QByteArray published = read(exported.path);
    service.protectSnapshot(exported.path);
    service.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("after.export"));
    require(service.flush() && read(exported.path) == published, "export is immutable");
    service.shutdown();
    DiagnosticsService other;
    require(other.initialize(options), "second session initializes");
    other.requestMaintenance();
    require(other.flush() && read(exported.path) == published,
            "clipboard archive survives publishing session shutdown and maintenance");
}

void concurrentRequestsAndFailedArchives() {
    QTemporaryDir directory(temporaryTemplate());
    DiagnosticsService service;
    auto options = optionsFor(directory.path());
    std::promise<void> entered;
    auto blocked = entered.get_future();
    std::promise<void> release;
    auto resumed = release.get_future().share();
    std::atomic_bool armed{false};
    options.appendFile = [&](const QString& path, const QByteArray& bytes) {
        if (armed.exchange(false)) {
            entered.set_value();
            resumed.wait();
        }
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Append) &&
               file.write(bytes) == bytes.size();
    };
    require(service.initialize(options) && service.flush(), "concurrency logger initializes");
    armed = true;
    service.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("blocked"));
    require(blocked.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "writer blocks deterministically");
    const auto exported = service.exportLogs();
    const auto repeated = service.exportLogs();
    const bool rejected = !repeated.get().success;
    release.set_value();
    require(rejected && exported.get().success, "concurrent export is rejected");
    service.discardSnapshot(exported.get().path);
    require(service.flush() && !QFileInfo::exists(exported.get().path),
            "unpublished archive can be discarded");
    service.shutdown();

    QTemporaryDir unavailable(temporaryTemplate());
    put(unavailable.filePath(QStringLiteral("exports")), "block export directory");
    options = optionsFor(unavailable.path());
    require(service.initialize(options), "failure logger initializes");
    require(!service.exportLogs().get().success && !service.status().exporting,
            "directory failure reports failure and clears busy state");
    service.shutdown();

    QTemporaryDir unreadable(temporaryTemplate());
    options = optionsFor(unreadable.path());
    auto missing = std::make_shared<FixtureCollector>();
    missing->dump = unreadable.filePath(QStringLiteral("crashes/reports/missing.dmp"));
    options.crashCollector = missing;
    options.enableCrashCapture = true;
    require(service.initialize(options), "unreadable-source fixture initializes");
    require(!service.exportLogs().get().success, "unreadable evidence fails the whole export");
    QDirIterator partials(unreadable.path(), {QStringLiteral("*.zip"), QStringLiteral("*.tmp")},
                          QDir::Files, QDirIterator::Subdirectories);
    require(!partials.hasNext(), "read failure removes the partial archive");
    service.shutdown();

    QTemporaryDir budget(temporaryTemplate());
    options = optionsFor(budget.path());
    options.totalBytes = 1;
    require(service.initialize(options), "budget logger initializes");
    require(!service.exportLogs().get().success, "insufficient budget rejects export");
    QDirIterator files(budget.path(), {QStringLiteral("*.zip"), QStringLiteral("*.tmp")},
                       QDir::Files, QDirIterator::Subdirectories);
    require(!files.hasNext(), "failed export leaves no final or partial ZIP");
}

void exportsDoNotFollowLinks() {
    QTemporaryDir root(temporaryTemplate());
    QTemporaryDir external(temporaryTemplate());
    const QString day = QDate::currentDate().addDays(-1).toString(Qt::ISODate);
    const QString link = root.filePath(day);
    const QString filename =
        QStringLiteral("snow-shot-cccccccccccccccccccccccccccccccc-000001.log");
    put(external.filePath(filename), "external evidence");
#ifdef Q_OS_WIN
    require(QProcess::execute(QStringLiteral("cmd.exe"),
                              {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                               QDir::toNativeSeparators(link),
                               QDir::toNativeSeparators(external.path())}) == 0,
            "create junction fixture");
#else
    require(QFile::link(external.path(), link), "create directory-link fixture");
#endif
    DiagnosticsService service;
    require(service.initialize(optionsFor(root.path())), "linked-root logger starts");
    const auto exported = service.exportLogs().get();
    require(exported.success && !unzip(exported.path).contains(day + u'/' + filename) &&
                read(external.filePath(filename)) == "external evidence",
            "export and retention exclude linked evidence");
}

void crashOnlyExport() {
    QTemporaryDir directory(temporaryTemplate());
    auto collector = std::make_shared<FixtureCollector>();
    collector->dump = directory.filePath(QStringLiteral("crashes/reports/only.dmp"));
    put(collector->dump, "MDMP crash-only");
    auto options = optionsFor(directory.path());
    options.crashCollector = collector;
    options.enableCrashCapture = true;
    options.appendFile = [](const QString&, const QByteArray&) { return false; };
    DiagnosticsService service;
    require(service.initialize(options), "crash-only service initializes");
    const auto exported = service.exportLogs().get();
    require(exported.success &&
                unzip(exported.path).value(QStringLiteral("crashes/only.dmp")) == "MDMP crash-only",
            "available crash evidence exports even when text logging fails");
}

void archiveSnapshotsAnActivelyGrowingLog() {
    QTemporaryDir root(temporaryTemplate());
    const QString relative =
        QDate::currentDate().toString(Qt::ISODate) +
        QStringLiteral("/snow-shot-dddddddddddddddddddddddddddddddd-000001.log");
    const QString path = root.filePath(relative);
    QByteArray initial(1024 * 1024, Qt::Uninitialized);
    quint32 value = 12345;
    for (char& byte : initial) {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        byte = static_cast<char>(value & 0xff);
    }
    put(path, initial);
    QLockFile lock(path + QStringLiteral(".lock"));
    require(lock.tryLock(0), "hold the other session's log lock");
    DiagnosticsService service;
    require(service.initialize(optionsFor(root.path())), "snapshot logger initializes");
    std::promise<bool> opened;
    auto ready = opened.get_future();
    std::jthread writer([&](std::stop_token stop) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            opened.set_value(false);
            return;
        }
        const QByteArray extra(4096, 'a');
        const bool first = file.write(extra) == extra.size() && file.flush();
        opened.set_value(first);
        while (first && !stop.stop_requested()) {
            if (file.write(extra) != extra.size() || !file.flush())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    require(ready.get(), "other session starts appending");
    const auto exported = service.exportLogs().get();
    writer.request_stop();
    writer.join();
    require(exported.success, "ZIP finalizes while another session keeps appending");
    const QByteArray snapshot = unzip(exported.path).value(relative);
    require(snapshot.startsWith(initial) && snapshot.size() > initial.size() &&
                snapshot.size() <= QFileInfo(path).size() &&
                snapshot.mid(initial.size()) == QByteArray(snapshot.size() - initial.size(), 'a'),
            "active file is a valid immutable prefix with a bounded snapshot size");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        pathSelection();
        archiveIncludesRetainedEvidenceAndSurvivesPublication();
        concurrentRequestsAndFailedArchives();
        crashOnlyExport();
        archiveSnapshotsAnActivelyGrowingLog();
        exportsDoNotFollowLinks();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
