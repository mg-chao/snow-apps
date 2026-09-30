#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/storagedirectorychange.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <thread>
#include <Windows.h>
#include <winioctl.h>
#include <cstring>

namespace storage = snow_shot::storage;
namespace {
void require(bool condition, const char* text) {
    if (!condition) {
        std::cerr << text << '\n';
        std::exit(1);
    }
}
void write(const QString& path, const QByteArray& data) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create parent");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "write fixture");
}
QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read fixture");
    return file.readAll();
}
storage::StorageDirectoryChangeOptions options(const QTemporaryDir& dir) {
    storage::StorageDirectoryChangeOptions result;
    result.source = dir.filePath(QStringLiteral("source"));
    result.destination = dir.filePath(QStringLiteral("目标 with spaces"));
    result.bootstrapDirectory = result.source;
    result.executableDirectory = dir.filePath(QStringLiteral("bin"));
    write(QDir(result.source).filePath(QStringLiteral("config.json")),
          QByteArrayLiteral("settings"));
    write(QDir(result.source).filePath(QStringLiteral("capture_history/records/one")),
          QByteArrayLiteral("payload"));
    write(QDir(result.source).filePath(QStringLiteral("assets/ocr/model")),
          QByteArrayLiteral("model"));
    write(QDir(result.source).filePath(QStringLiteral("logs/one.log")), QByteArrayLiteral("log"));
    return result;
}
void validation() {
    QTemporaryDir dir;
    auto o = options(dir);
    require(!storage::validateStorageDirectory(o.source, o.source).success, "same root rejected");
    require(!storage::validateStorageDirectory(o.source, o.source.toUpper()).success,
            "case aliases rejected");
    require(!storage::validateStorageDirectory(o.source,
                                               QDir(o.source).filePath(QStringLiteral("child")))
                 .success,
            "nested root rejected");
    require(!storage::validateStorageDirectory(o.source, dir.path()).success, "ancestor rejected");
    require(
        !storage::validateStorageDirectory(o.source, o.source + QStringLiteral("./child")).success,
        "trailing-dot aliases rejected");
    require(!storage::validateStorageDirectory(o.source, QStringLiteral("relative")).success,
            "relative rejected");
    write(QDir(o.destination).filePath(QStringLiteral(".hidden")), QByteArrayLiteral("existing"));
    require(!storage::validateStorageDirectory(o.source, o.destination).success,
            "hidden files reject nonempty destination");
}
void junctionsAreNeverTraversed() {
    QTemporaryDir fixture;
    auto o = options(fixture);
    const QString external = fixture.filePath(QStringLiteral("external"));
    write(QDir(external).filePath(QStringLiteral("untouched")), QByteArrayLiteral("outside"));
    const auto junction = QDir(o.source).filePath(QStringLiteral("junction"));
    require(QDir().mkdir(junction), "create junction fixture directory");
    const auto native = QDir::toNativeSeparators(junction).toStdWString();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open junction fixture");
    struct JunctionData {
        DWORD tag;
        WORD length;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        wchar_t paths[2048];
    } data{};
    const auto print = QDir::toNativeSeparators(external).toStdWString();
    // NT substitute names start with backslash, two question marks, backslash.
    const auto target = std::wstring(1, L'\\') + L"??" + L'\\' + print;
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substituteLength = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.printOffset = static_cast<WORD>((target.size() + 1) * sizeof(wchar_t));
    data.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.printOffset + data.printLength + sizeof(wchar_t));
    std::memcpy(data.paths, target.c_str(), (target.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<char*>(data.paths) + data.printOffset, print.c_str(),
                (print.size() + 1) * sizeof(wchar_t));
    DWORD returned = 0;
    const bool created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.length + 8,
                                         nullptr, 0, &returned, nullptr) != 0;
    CloseHandle(handle);
    require(created, "create NTFS junction without elevation");
    require(!storage::validateStorageDirectory(o.source, junction + QStringLiteral("/new")).success,
            "junction traversal rejected during validation");
    require(!storage::changeStorageDirectory(o).success, "links inside source fail before copying");
    require(read(QDir(external).filePath(QStringLiteral("untouched"))) == "outside",
            "external files remain untouched");
    require(RemoveDirectoryW(native.c_str()) != 0, "remove only junction fixture");
}

void transactions() {
    QTemporaryDir dir;
    auto o = options(dir);
    int progress = 0;
    o.progress = [&](const auto& p) {
        require(p.completed <= p.total, "progress is bounded");
        ++progress;
    };
    const auto result = storage::changeStorageDirectory(o);
    require(result.success && result.warning.isEmpty(), "successful copy and cleanup");
    require(progress > 5, "real progress emitted");
    require(read(QDir(o.destination).filePath(QStringLiteral("capture_history/records/one"))) ==
                "payload",
            "verified history preserved");
    require(!QFileInfo::exists(QDir(o.source).filePath(QStringLiteral("config.json"))),
            "old payload removed");
    require(QFileInfo::exists(storage::storageBootstrapPath(o.source, o.executableDirectory)),
            "bootstrap retained");
    require(storage::savedStorageDirectory(o.source, o.executableDirectory) == o.destination,
            "selection committed");
    require(storage::savedStorageDirectory(o.source, QStringLiteral("C:/other-install")).isEmpty(),
            "installations isolated");
    const auto selected =
        storage::ApplicationStorage::resolveDirectory({o.executableDirectory, o.source});
    require(selected.effectiveDirectory == o.destination &&
                selected.mode == storage::StorageMode::Custom,
            "restart selects custom root");
}
void failures() {
    for (const auto& point : {QStringLiteral("copy"), QStringLiteral("verify"),
                              QStringLiteral("activate"), QStringLiteral("persist")}) {
        QTemporaryDir dir;
        auto o = options(dir);
        bool active = false;
        o.activate = [&](const QString&) {
            active = true;
            return storage::StorageResult::ok();
        };
        o.rollback = [&] { active = false; };
        o.checkpoint = [&](const QString& step) {
            return step == point ? storage::StorageResult::failure(QStringLiteral("injected"))
                                 : storage::StorageResult::ok();
        };
        const auto result = storage::changeStorageDirectory(o);
        require(!result.success && !active, "precommit failure rolls back runtime");
        require(read(QDir(o.source).filePath(QStringLiteral("config.json"))) == "settings",
                "source survives failure");
        require(storage::savedStorageDirectory(o.source, o.executableDirectory).isEmpty(),
                "failed selection not persisted");
        require(QDir(o.destination)
                    .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                    .isEmpty(),
                "partial copies removed");
    }
    QTemporaryDir dir;
    auto o = options(dir);
    o.checkpoint = [](const QString& point) {
        return point == u"cleanup" ? storage::StorageResult::failure(QStringLiteral("locked"))
                                   : storage::StorageResult::ok();
    };
    const auto result = storage::changeStorageDirectory(o);
    require(result.success && !result.warning.isEmpty(), "cleanup failure commits with warning");
    require(QFileInfo::exists(QDir(o.source).filePath(QStringLiteral("config.json"))),
            "locked source retained");
}
void recovery() {
    QTemporaryDir dir;
    auto o = options(dir);
    const auto path = storage::storageBootstrapPath(o.source, o.executableDirectory);
    for (const auto& committed : {o.source, o.destination}) {
        write(path, QJsonDocument(
                        QJsonObject{{QStringLiteral("directory"), committed},
                                    {QStringLiteral("transaction"),
                                     QJsonObject{{QStringLiteral("destination"), o.destination}}}})
                        .toJson());
        QString warning;
        require(storage::savedStorageDirectory(o.source, o.executableDirectory, &warning) ==
                        committed &&
                    !warning.isEmpty(),
                "interrupted transaction uses committed root and warns");
    }
}
storage::PinnedWindowRecord pin() {
    storage::PinnedWindowRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.sourceKind = storage::PinnedWindowSourceKind::ClipboardText;
    record.originalText = QStringLiteral("Pinned text");
    record.nativeGeometry = QRect(0, 0, 2, 2);
    record.canvasSourceRect = QRectF(record.nativeGeometry);
    record.contentCanvasRect = record.canvasSourceRect;
    record.surfaceCanvasRect = record.canvasSourceRect;
    record.initialWindowSize = record.nativeGeometry.size();
    return record;
}
storage::CaptureHistoryDraft screenshot() {
    storage::CaptureHistoryDraft draft;
    draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    draft.createdUtc = QDateTime::currentDateTimeUtc();
    draft.canvasBounds = QRect(0, 0, 4, 4);
    draft.selection.rectangle = draft.canvasBounds;
    draft.selection.shadowColor = QColor(0, 0, 0, 96);
    draft.canvasHistory = QByteArrayLiteral("{\"schemaVersion\":1,\"document\":{},\"history\":{}}");
    QImage image(4, 4, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    draft.displays.push_back({QStringLiteral("display"), QStringLiteral("Display"), image});
    return draft;
}

void crossVolumeCopy() {
    QTemporaryDir source;
    QTemporaryDir destination(QDir::current().filePath(QStringLiteral("storage-migration-XXXXXX")));
    require(destination.isValid(), "cross-volume destination fixture");
    auto o = options(source);
    o.destination = destination.path();
    require(storage::changeStorageDirectory(o).success,
            "migration supports separate volumes without rename");
    require(read(QDir(o.destination).filePath(QStringLiteral("config.json"))) == "settings",
            "cross-volume bytes preserved");
}

void flushFailureRestoresAvailability() {
    QTemporaryDir root;
    auto& app = storage::ApplicationStorage::instance();
    const auto source = root.filePath(QStringLiteral("source"));
    require(app.initialize({root.filePath(QStringLiteral("bin")), source, 60000}).success,
            "flush fixture initialized");
    require(QDir().mkdir(QDir(source).filePath(QStringLiteral("config.json"))),
            "block atomic config flush");
    bool finished = false;
    auto connection = QObject::connect(
        &app, &storage::ApplicationStorage::directoryChangeFinished, &app, [&](const auto& result) {
            require(!result.success, "flush failure prevents migration");
            finished = true;
        });
    require(app.requestDirectoryChange(root.filePath(QStringLiteral("target")), true).success,
            "flush attempt starts asynchronously");
    QElapsedTimer timer;
    timer.start();
    while (!finished && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    QObject::disconnect(connection);
    require(finished && !app.directoryChanging() && app.configurationDirectory() == source &&
                app.status().writeAvailable,
            "flush failure restores source and releases busy state");
    app.shutdown();
}

void liveSwitch(bool migrate) {
    QTemporaryDir dir;
    auto& app = storage::ApplicationStorage::instance();
    const QString source = dir.filePath(QStringLiteral("data"));
    const QString target = dir.filePath(QStringLiteral("new"));
    require(app.initialize({dir.filePath(QStringLiteral("bin")), source, 60000}).success,
            "initialize app storage");
    auto* configuration = &app.configuration();
    auto* history = &app.captureHistory();
    auto* pins = &app.pinnedWindows();
    require(configuration->setValue(QStringLiteral("screenshot_selection/smart_selection"), false),
            "custom setting");
    const auto open = pin();
    const auto closed = pin();
    auto filePin = pin();
    filePin.sourceKind = storage::PinnedWindowSourceKind::ClipboardImageFile;
    filePin.originalFilePath = dir.filePath(QStringLiteral("external.png"));
    QImage external(2, 2, QImage::Format_RGBA8888);
    external.fill(Qt::blue);
    require(external.save(filePin.originalFilePath), "external pin fixture");
    require(pins->create(open).success && pins->create(closed).success &&
                pins->markClosed(closed.id).success && pins->create(filePin).success,
            "pin fixtures");
    write(QDir(source).filePath(QStringLiteral("assets/ocr/test")), QByteArrayLiteral("ocr"));
    const auto publication = history->publish(screenshot()).get();
    if (!publication.storage.success)
        std::cerr << publication.storage.error.toStdString() << '\n';
    require(publication.storage.success, "screenshot fixture");
    auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
    snow_shot::diagnostics::DiagnosticsOptions logOptions;
    logOptions.directories = {QDir(source).filePath(QStringLiteral("logs"))};
    logOptions.installMessageHandler = false;
    logOptions.enableCrashCapture = false;
    require(diagnostics.initialize(logOptions), "start source diagnostics");
    require(app.flushNow().success, "flush before migration");
    std::atomic_bool stopDiagnostics{false};
    std::atomic_bool invalidSession{false};
    std::promise<void> diagnosticsStarted;
    auto diagnosticsReady = diagnosticsStarted.get_future();
    std::thread diagnosticsReader([&] {
        diagnosticsStarted.set_value();
        while (!stopDiagnostics.load()) {
            const auto state = diagnostics.status();
            if (state.sessionId.isEmpty() ||
                (state.directory != QDir(source).filePath(QStringLiteral("logs")) &&
                 state.directory != QDir(target).filePath(QStringLiteral("logs"))) ||
                diagnostics.options().directories.isEmpty() || diagnostics.directories().isEmpty())
                invalidSession.store(true);
            diagnostics.record(QtInfoMsg, QStringLiteral("test"),
                               QStringLiteral("migration.concurrent"));
            std::this_thread::yield();
        }
    });
    diagnosticsReady.wait();
    bool finished = false;
    storage::StorageDirectoryChangeResult outcome;
    const auto connection = QObject::connect(
        &app, &storage::ApplicationStorage::directoryChangeFinished, &app, [&](const auto& result) {
            finished = true;
            outcome = result;
        });
    require(app.requestDirectoryChange(target, migrate).success, "start asynchronous migration");
    require(!app.requestDirectoryChange(target, migrate).success, "duplicate migration rejected");
    require(!configuration->setValue(QStringLiteral("screenshot_selection/smart_selection"), true),
            "writes frozen during migration");
    QElapsedTimer timer;
    timer.start();
    while (!finished && timer.elapsed() < 15000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    stopDiagnostics.store(true);
    diagnosticsReader.join();
    QObject::disconnect(connection);
    if (!outcome.success)
        std::cerr << outcome.error.toStdString() << '\n';
    require(finished && outcome.success, "asynchronous migration completes");
    require(!invalidSession.load(), "concurrent diagnostics calls retain complete sessions");
    require(configuration == &app.configuration() && history == &app.captureHistory() &&
                pins == &app.pinnedWindows(),
            "consumer references remain valid");
    require(app.configurationDirectory() == target && !app.directoryChanging(),
            "new directory immediately active");
    require(!app.smartSelectionEnabled(), "settings preserved");
    require(diagnostics.status().directory == QDir(target).filePath(QStringLiteral("logs")) &&
                diagnostics.flush(),
            "diagnostics follow new root");
    require(history->records().size() == (migrate ? 1 : 0), "history migration follows switch");
    if (migrate)
        require(history->load(history->records().front()).has_value(),
                "existing history payload readable through stable reference");
    require(history->publish(screenshot()).get().storage.success,
            "history publication works after relocation");
    require(pins->loadRecord(open.id).has_value(), "open pin preserved");
    const auto relocatedPin = pins->loadRecord(filePin.id);
    require(relocatedPin && relocatedPin->originalFilePath.startsWith(target + u'/') &&
                QFileInfo::exists(relocatedPin->originalFilePath) &&
                QFileInfo::exists(filePin.originalFilePath),
            "private file-pin payload follows new root and external source stays untouched");
    require(pins->loadRecord(closed.id).has_value() == migrate,
            "closed pin migration follows switch");
    require(QFileInfo::exists(QDir(target).filePath(QStringLiteral("assets/ocr/test"))) == migrate,
            "OCR migration follows switch");
    require(configuration->setValue(QStringLiteral("screenshot_selection/smart_selection"), true) &&
                app.flushNow().success,
            "new settings remain writable");
    require(pins->create(pin()).success && pins->flush().success, "new pinned records writable");
    require(QFileInfo::exists(QDir(source).filePath(QStringLiteral("config.json"))) != migrate,
            "source cleanup follows migration switch");
    diagnostics.shutdown();
    app.shutdown();
    require(app.initialize({dir.filePath(QStringLiteral("bin")), source, 60000}).success &&
                app.configurationDirectory() == target && app.smartSelectionEnabled(),
            "restart uses committed data");
    app.shutdown();
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    validation();
    junctionsAreNeverTraversed();
    transactions();
    crossVolumeCopy();
    failures();
    recovery();
    liveSwitch(true);
    liveSwitch(false);
    flushFailureRestoresAvailability();
}
