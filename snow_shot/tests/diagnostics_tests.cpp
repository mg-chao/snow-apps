#include "../src/presentation/capture/screenshotscrollingdiagnostics.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimeZone>

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace snow_shot::diagnostics;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "file must be readable");
    return file.readAll();
}
void put(const QString& path, const QByteArray& bytes) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "fixture file");
    require(file.write(bytes) == bytes.size(), "fixture bytes");
}
DiagnosticsOptions optionsFor(const QString& directory) {
    DiagnosticsOptions options;
    options.directories = {directory};
    options.enableCrashCapture = false;
    options.installMessageHandler = false;
    options.mirrorToConsole = false;
    return options;
}
void scrollingMetadataAndReportCadence() {
    using snow_shot::capture_detail::ScrollingCaptureDiagnostics;
    using namespace std::chrono_literals;
    ScrollingCaptureDiagnostics counters;
    const auto start = counters.started;
    require(!counters.reportDue(start + 4999ms), "no premature progress report");
    require(counters.reportDue(start + 5s), "first stalled-stream report after five seconds");
    require(!counters.reportDue(start + 34s), "progress reports must be rate limited");
    require(counters.reportDue(start + 35s), "long capture retains periodic diagnostics");
    counters.received = 4;
    counters.accepted = 1;
    counters.duplicates = 1;
    counters.invalid = 2;
    counters.timeouts = 9;
    auto fields = counters.fields(start + 35s);
    fields.insert(QStringLiteral("hole_rect"), QStringLiteral("0,0 1920x1080"));
    fields.insert(QStringLiteral("full_hole"), true);
    fields.insert(QStringLiteral("mask_empty"), true);
    fields.insert(QStringLiteral("native_region_type"), 0);
    fields.insert(QStringLiteral("thumbnail_visible"), false);
    fields.insert(QStringLiteral("backend"), 3);
    fields.insert(QStringLiteral("operation"), QStringLiteral("7"));
    fields.insert(QStringLiteral("window_title"), QStringLiteral("private title"));
    QTemporaryDir directory;
    DiagnosticsService service;
    require(service.initialize(optionsFor(directory.path())), "scrolling diagnostic logger");
    service.record(QtWarningMsg, QStringLiteral("snow_shot.scrolling"),
                   QStringLiteral("scrolling.capture_progress"), {}, fields);
    require(service.flush(), "scrolling metadata flush");
    bool found = false;
    for (const auto& line : read(service.status().currentFile).split('\n')) {
        const auto record = QJsonDocument::fromJson(line).object();
        if (record.value(QStringLiteral("event")) != QStringLiteral("scrolling.capture_progress"))
            continue;
        const auto actual = record.value(QStringLiteral("fields")).toObject();
        fields.remove(QStringLiteral("window_title"));
        require(actual == fields,
                "scrolling metadata survives export while content keys stay excluded");
        found = true;
    }
    require(found, "scrolling progress event must be persisted");
}

void concurrentRecordsAndSnapshots() {
    QTemporaryDir directory;
    DiagnosticsService service;
    require(service.initialize(optionsFor(directory.path())), "initialize logger");
    std::vector<std::thread> writers;
    for (int thread = 0; thread < 4; ++thread) {
        writers.emplace_back([&service] {
            for (int i = 0; i < 100; ++i) {
                service.record(QtInfoMsg, QStringLiteral("test"), QStringLiteral("parallel"),
                               QString::fromUtf8("unicode \xe4\xb8\xad\xe6\x96\x87\n\"quoted\""));
            }
        });
    }
    for (auto& writer : writers)
        writer.join();
    require(service.flush(), "flush barrier");
    auto result = service.exportDay(QDate::currentDate()).get();
    require(result.success, "snapshot succeeds");
    const QByteArray snapshot = read(result.path);
    int count = 0;
    for (const auto& line : snapshot.split('\n')) {
        if (line.isEmpty())
            continue;
        const auto document = QJsonDocument::fromJson(line);
        require(document.isObject(), "records must remain valid JSON under contention");
        const auto object = document.object();
        require(!object.value(QStringLiteral("session")).toString().isEmpty(),
                "session identifier");
        if (object.value(QStringLiteral("event")).toString() == QStringLiteral("parallel"))
            ++count;
    }
    require(count == 400, "all accepted concurrent records reach the snapshot");
    service.protectSnapshot(result.path);
    service.record(QtCriticalMsg, QStringLiteral("test"), QStringLiteral("later"));
    require(service.flush(), "later flush");
    require(read(result.path) == snapshot, "published snapshots must be immutable");
}
void retentionAndRollover() {
    QTemporaryDir directory;
    std::atomic<qint64> time{
        QDateTime(QDate(2026, 9, 7), QTime(23, 59), QTimeZone::LocalTime).toMSecsSinceEpoch()};
    auto options = optionsFor(directory.path());
    options.clock = [&] { return QDateTime::fromMSecsSinceEpoch(time.load()); };
    const QString old =
        QDir(directory.path())
            .filePath(
                QStringLiteral("2026-08-31/snow-shot-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-000001.log"));
    const QString kept =
        QDir(directory.path())
            .filePath(
                QStringLiteral("2026-09-01/snow-shot-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-000001.log"));
    const QString unrelated =
        QDir(directory.path()).filePath(QStringLiteral("2026-08-31/user.log"));
    put(old, "old");
    put(kept, "kept");
    put(unrelated, "user");
    DiagnosticsService service;
    require(service.initialize(options), "retention initialize");
    require(!QFileInfo::exists(old), "eighth date expires");
    require(QFileInfo::exists(kept), "seventh date retained");
    require(QFileInfo::exists(unrelated), "unrelated files preserved");
    time.fetch_add(120000);
    service.record(QtInfoMsg, QStringLiteral("test"), QStringLiteral("midnight"));
    service.requestMaintenance();
    require(service.flush(), "midnight flush");
    require(service.status().currentFile.contains(QStringLiteral("2026-09-08")),
            "rotate at midnight");
    require(!QFileInfo::exists(kept), "retention advances while running");
    require(service.exportDay(QDate(2026, 9, 8)).get().success, "export uses requested day");
}
void fallbackPrivacyAndFailures() {
    QTemporaryDir directory;
    const QString blocked = QDir(directory.path()).filePath(QStringLiteral("file"));
    put(blocked, "not a directory");
    auto options = optionsFor(QDir(directory.path()).filePath(QStringLiteral("fallback")));
    options.directories.prepend(blocked);
    DiagnosticsService service;
    require(service.initialize(options), "fallback initialization");
    require(!service.status().fallbackReason.isEmpty(), "fallback must be visible");
    service.record(
        QtWarningMsg, QStringLiteral("test"), QStringLiteral("private"),
        QStringLiteral(
            "Authorization: Bearer topsecret api_key=secret https://host/a?token=secret"),
        {{QStringLiteral("body"), QStringLiteral("captured-content")}});
    require(service.flush(), "privacy flush");
    const auto result = service.exportDay(QDate::currentDate()).get();
    require(result.success, "privacy snapshot");
    const auto bytes = read(result.path);
    require(!bytes.contains("topsecret") && !bytes.contains("api_key=secret") &&
                !bytes.contains("captured-content"),
            "secrets and bodies excluded");
    const auto redacted = DiagnosticsService::sanitize(QStringLiteral(
        R"({"password":"quoted secret", "access_token": "private-token", 'client_secret': 'private-client'} https://user:private-pass@example.com/path)"));
    require(!redacted.contains(QStringLiteral("quoted secret")) &&
                !redacted.contains(QStringLiteral("private-")),
            "quoted credentials, tokens and URL passwords are redacted");
    service.shutdown();
    options.appendFile = [](const QString&, const QByteArray&) { return false; };
    require(service.initialize(options), "write failure must not prevent worker startup");
    require(!service.status().loggingAvailable && !service.status().lastError.isEmpty(),
            "write failure visible");
    require(!service.flush(), "flush cannot report success after a failed write");
}
void boundedQueueAndRecord() {
    QTemporaryDir directory;
    DiagnosticsService service;
    auto options = optionsFor(directory.path());
    options.queueRecords = 1;
    options.segmentBytes = 32768;
    options.dailyBytes = 65536;
    options.totalBytes = 131072;
    require(service.initialize(options), "bounded logger initialize");
    for (int i = 0; i < 10000; ++i) {
        service.record(QtInfoMsg, QStringLiteral("test"), QStringLiteral("flood"),
                       QString(20000, u'x'));
    }
    service.record(QtCriticalMsg, QStringLiteral("test"), QStringLiteral("important"));
    service.requestMaintenance();
    require(service.flush(std::chrono::seconds(10)), "bounded writer drains");
    require(service.status().droppedRecords > 0, "overload is accounted for");
    require(service.status().bytes <= options.totalBytes, "budget enforced");
    for (const QFileInfo& day :
         QDir(directory.path()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        for (const QFileInfo& file :
             QDir(day.absoluteFilePath()).entryInfoList({QStringLiteral("*.log")}, QDir::Files)) {
            for (const auto& line : read(file.absoluteFilePath()).split('\n')) {
                require(line.size() < 16384, "individual records remain bounded");
            }
        }
    }
}
void liveSessionsAndDeletionFailures() {
    QTemporaryDir directory;
    const QString session = QStringLiteral("cccccccccccccccccccccccccccccccc");
    const QString old =
        QDir(directory.path())
            .filePath(QStringLiteral("2025-12-01/snow-shot-%1-000001.log").arg(session));
    put(old, "active");
    QLockFile other(old + QStringLiteral(".lock"));
    require(other.tryLock(), "other session lock");
    auto options = optionsFor(directory.path());
    options.clock = [] { return QDateTime(QDate(2026, 1, 1), QTime(12, 0)); };
    DiagnosticsService service;
    require(service.initialize(options), "live sessions initialize");
    require(QFileInfo::exists(old), "never remove another active session after clock changes");
    other.unlock();
    service.requestMaintenance();
    require(service.flush(), "unlocked cleanup");
    require(!QFileInfo::exists(old), "inactive expired session can be removed");
    service.shutdown();
    put(old, "undeletable");
    options.removeFile = [old](const QString& path) { return path != old && QFile::remove(path); };
    require(service.initialize(options), "degraded startup");
    require(QFileInfo::exists(old) && !service.status().lastError.isEmpty(),
            "cleanup failure remains visible");
    require(!service.exportDay(QDate(2026, 1, 1)).get().success,
            "degraded cleanup limits discretionary snapshots");
}
void closedSegmentsAndEmergencyRollover() {
    QTemporaryDir directory;
    DiagnosticsService service;
    auto options = optionsFor(directory.path());
    std::atomic<qint64> time{QDateTime(QDate(2026, 12, 31), QTime(23, 59)).toMSecsSinceEpoch()};
    options.clock = [&] { return QDateTime::fromMSecsSinceEpoch(time.load()); };
    options.installMessageHandler = true;
    options.segmentBytes = 16384;
    options.dailyBytes = 32768;
    options.totalBytes = 131072;
    require(service.initialize(options), "rollover logger starts");
    for (int i = 0; i < 12; ++i) {
        service.record(QtInfoMsg, QStringLiteral("test"), QStringLiteral("segment"),
                       QString(6000, u'x'));
        require(service.flush(), "segment flush");
    }
    service.requestMaintenance();
    require(service.flush(), "daily maintenance barrier");
    qint64 dayBytes = 0;
    const QString oldDay = QDir(directory.path()).filePath(QStringLiteral("2026-12-31"));
    for (const QFileInfo& file : QDir(oldDay).entryInfoList({QStringLiteral("*.log")}, QDir::Files))
        dayBytes += file.size();
    if (dayBytes > options.dailyBytes) {
        std::fprintf(stderr, "day bytes=%lld tracked=%lld error=%s\n", dayBytes,
                     service.status().bytes, qPrintable(service.status().lastError));
        for (const QFileInfo& file : QDir(oldDay).entryInfoList(QDir::Files))
            std::fprintf(stderr, "%s %lld\n", qPrintable(file.fileName()), file.size());
    }
    require(dayBytes <= options.dailyBytes, "closed segments are reclaimed during a live session");
    time.fetch_add(8LL * 24 * 60 * 60 * 1000);
    service.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("resume"));
    service.requestMaintenance();
    require(service.flush(), "resume flush");
    require(QDir(oldDay).entryList({QStringLiteral("*.log")}, QDir::Files).isEmpty(),
            "expired emergency and ordinary segments are removed across year change");
    require(service.status().currentFile.contains(QStringLiteral("2027-01-08")), "new year date");
}

void reentrantClockAndFiltering() {
    QTemporaryDir directory;
    DiagnosticsService service;
    auto options = optionsFor(directory.path());
    options.clock = [&] {
        service.record(QtInfoMsg, QStringLiteral("test"), QStringLiteral("nested"));
        return QDateTime::currentDateTime();
    };
    require(service.initialize(options), "reentrant clock starts without deadlock");
    service.record(QtDebugMsg, QStringLiteral("test"), QStringLiteral("filtered"));
    service.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("outer"));
    require(service.flush(), "reentrant writer flushes");
    const auto bytes = read(service.status().currentFile);
    require(!bytes.contains("filtered") && bytes.contains("outer"), "release severity filtering");
}
class FixtureCollector final : public CrashCollector {
  public:
    QString dump;
    QString metadata;
    bool deleted = false;
    bool initialize(const QString&, const QString&, const QString&, QString*) override {
        return true;
    }
    QVector<CrashReport> reports() override {
        if (deleted)
            return {};
        return {{QStringLiteral("oversized"),
                 dump,
                 QDateTime::currentDateTime(),
                 QFileInfo(dump).size(),
                 {}}};
    }
    bool removeReport(const QString& id) override {
        deleted =
            id == QStringLiteral("oversized") && QFile::remove(dump) && QFile::remove(metadata);
        return deleted;
    }
    QString pipeName() const override {
        return {};
    }
};

void oversizedDumpAndJunctionExclusion() {
    QTemporaryDir directory;
    QTemporaryDir external;
    const QString protectedFile =
        QDir(external.path())
            .filePath(QStringLiteral("snow-shot-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-000001.log"));
    put(protectedFile, "unrelated evidence");
    const QString junction = QDir(directory.path()).filePath(QStringLiteral("2020-01-01"));
#ifdef Q_OS_WIN
    require(QProcess::execute(QStringLiteral("cmd.exe"),
                              {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                               QDir::toNativeSeparators(junction),
                               QDir::toNativeSeparators(external.path())}) == 0,
            "create junction fixture");
#endif
    auto collector = std::make_shared<FixtureCollector>();
    collector->dump =
        QDir(directory.path()).filePath(QStringLiteral("crashes/reports/oversized.dmp"));
    collector->metadata = collector->dump + QStringLiteral(".meta");
    put(collector->dump, QByteArray(80000, 'x'));
    put(collector->metadata, "metadata");
    auto options = optionsFor(directory.path());
    options.crashCollector = collector;
    options.enableCrashCapture = true;
    options.segmentBytes = 16384;
    options.dailyBytes = 32768;
    options.totalBytes = 65536;
    DiagnosticsService service;
    require(service.initialize(options), "oversized dump logger initializes");
    require(service.flush(), "dump maintenance flushes");
    require(collector->deleted && !QFileInfo::exists(collector->metadata),
            "dump deletion uses collector metadata API");
    require(service.status().bytes <= options.totalBytes,
            "completed oversized dump cannot exhaust budget");
    require(read(service.status().currentFile).contains("retention.dump_removed"),
            "dump removal is recorded");
    require(read(protectedFile) == "unrelated evidence", "cleanup never follows junctions");
}
void simultaneousSessionsAndPersistentSnapshot() {
    QTemporaryDir directory;
    const auto options = optionsFor(directory.path());
    DiagnosticsService first;
    DiagnosticsService second;
    require(first.initialize(options) && second.initialize(options), "simultaneous loggers");
    require(first.status().currentFile != second.status().currentFile,
            "session filenames never collide");
    first.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("first"));
    second.record(QtWarningMsg, QStringLiteral("test"), QStringLiteral("second"));
    require(first.flush() && second.flush(), "both sessions flush");
    const auto result = first.exportDay(QDate::currentDate()).get();
    require(result.success, "multi-session snapshot");
    const auto bytes = read(result.path);
    require(bytes.contains("first") && bytes.contains("second"), "snapshot merges sessions");
    first.protectSnapshot(result.path);
    require(first.flush(), "snapshot protection persisted");
    first.shutdown();
    second.requestMaintenance();
    require(second.flush(), "other process cleanup");
    require(QFileInfo::exists(result.path), "clipboard attachment survives publisher shutdown");
}
void handlerLifecycleAndMissingCollector() {
    QTemporaryDir directory;
    auto options = optionsFor(directory.path());
    options.installMessageHandler = true;
    options.enableCrashCapture = true;
    options.handlerPath = QDir(directory.path()).filePath(QStringLiteral("missing-handler.exe"));
    DiagnosticsService service;
    require(service.initialize(options), "missing helper does not prevent text logging");
    require(!service.status().crashCaptureAvailable && !service.status().lastError.isEmpty(),
            "missing helper visible");
    qWarning("lifecycle warning");
    require(service.flush(), "Qt adapter flushes");
    require(read(service.status().currentFile).contains("lifecycle warning"),
            "Qt warning captured");
    const QString path = service.status().currentFile;
    service.shutdown();
    require(read(path).contains("session.clean_shutdown"),
            "orderly shutdown persists final record");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        scrollingMetadataAndReportCadence();
        concurrentRecordsAndSnapshots();
        retentionAndRollover();
        fallbackPrivacyAndFailures();
        boundedQueueAndRecord();
        liveSessionsAndDeletionFailures();
        simultaneousSessionsAndPersistentSnapshot();
        closedSegmentsAndEmergencyRollover();
        reentrantClockAndFiltering();
        oversizedDumpAndJunctionExclusion();
        handlerLifecycleAndMissingCollector();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
