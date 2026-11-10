#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/storagedirectorychange.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
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
#include <stdexcept>
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
    require(
        storage::savedStorageDirectory(o.source, QStringLiteral("C:/other-install")) ==
            (snow_shot::app::distribution::isStoreManaged() ? o.destination : QString()),
        "direct installations are path-isolated while Store selections survive package relocation");
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
void transactionExceptionsRespectCommit() {
    for (const auto& point :
         {QStringLiteral("copy"), QStringLiteral("verify"), QStringLiteral("activate"),
          QStringLiteral("persist"), QStringLiteral("cleanup")}) {
        QTemporaryDir dir;
        auto o = options(dir);
        bool active = false;
        int rollbacks = 0;
        o.activate = [&](const QString&) {
            active = true;
            return storage::StorageResult::ok();
        };
        o.rollback = [&] {
            ++rollbacks;
            active = false;
        };
        o.checkpoint = [&](const QString& step) {
            if (step == point)
                throw std::runtime_error("transaction checkpoint");
            return storage::StorageResult::ok();
        };
        const auto result = storage::changeStorageDirectory(o);
        if (point == u"cleanup") {
            require(result.success && !result.warning.isEmpty() && active && rollbacks == 0,
                    "postcommit exception retains active root and reports warning");
            require(storage::savedStorageDirectory(o.source, o.executableDirectory) ==
                        o.destination,
                    "postcommit exception preserves durable selector");
        } else {
            require(!result.success && !result.error.isEmpty() && !active && rollbacks == 1,
                    "precommit exception rolls back exactly once");
            require(storage::savedStorageDirectory(o.source, o.executableDirectory).isEmpty(),
                    "precommit exception restores durable selector");
            require(QDir(o.destination)
                        .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                        .isEmpty(),
                    "precommit exception removes partial copies");
        }
        require(QFileInfo::exists(QDir(o.source).filePath(QStringLiteral("config.json"))),
                "exception preserves source payload");
    }
    for (const bool duringActivation : {false, true}) {
        QTemporaryDir fixture;
        auto o = options(fixture);
        bool active = false;
        int rollbacks = 0;
        o.prepare = [&](const QString&) {
            if (!duringActivation)
                throw std::runtime_error("preparation failed");
            return storage::StorageResult::ok();
        };
        o.activate = [&](const QString&) -> storage::StorageResult {
            active = true;
            throw std::runtime_error("activation failed after acquiring destination");
        };
        o.rollback = [&] {
            active = false;
            ++rollbacks;
        };
        const auto result = storage::changeStorageDirectory(o);
        require(!result.success && !active && rollbacks == 1,
                "throwing preparation or partially completed activation rolls back once");
        require(storage::savedStorageDirectory(o.source, o.executableDirectory).isEmpty(),
                "throwing hooks preserve original selector");
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

void asynchronousExceptionsRestoreParticipants() {
    enum class Failure { Launch, InvalidFuture, Drain, Completion, CommittedCompletion, Shutdown };
    for (const auto failure :
         {Failure::Launch, Failure::InvalidFuture, Failure::Drain, Failure::Completion,
          Failure::CommittedCompletion, Failure::Shutdown}) {
        QTemporaryDir fixture;
        auto& app = storage::ApplicationStorage::instance();
        const auto source = fixture.filePath(QStringLiteral("source"));
        const auto target = fixture.filePath(QStringLiteral("target"));
        require(app.initialize({fixture.filePath(QStringLiteral("bin")), source, 60000}).success,
                "exception fixture initialized");
        int suspended = 0;
        int resumed = 0;
        int completions = 0;
        QString resumedRoot;
        bool succeeded = false;
        auto* ownerThread = QThread::currentThread();
        app.setDirectoryChangeHooks(
            [&] {
                require(QThread::currentThread() == ownerThread, "suspend on owner thread");
                ++suspended;
                return storage::StorageResult::ok();
            },
            [&](const QString& root) {
                require(QThread::currentThread() == ownerThread, "resume on owner thread");
                ++resumed;
                resumedRoot = root;
            },
            [&] {
                require(QThread::currentThread() != ownerThread, "drain on worker thread");
                if (failure == Failure::Drain)
                    throw std::runtime_error("drain failed");
            },
            [failure](storage::ApplicationStorage::DirectoryChangeTask work)
                -> std::future<storage::StorageDirectoryChangeResult> {
                if (failure == Failure::Launch)
                    throw std::runtime_error("worker launch failed");
                if (failure == Failure::InvalidFuture)
                    return {};
                return std::async(std::launch::async, [failure, work = std::move(work)] {
                    if (failure == Failure::Completion)
                        throw std::runtime_error("worker completion failed");
                    const auto result = work();
                    if (failure == Failure::CommittedCompletion)
                        throw std::runtime_error("result delivery failed after commit");
                    return result;
                });
            });
        const auto connection =
            QObject::connect(&app, &storage::ApplicationStorage::directoryChangeFinished, &app,
                             [&](const auto& result) {
                                 ++completions;
                                 succeeded = result.success;
                             });
        const auto admitted = app.requestDirectoryChange(target, true);
        if (failure == Failure::Launch || failure == Failure::InvalidFuture)
            require(!admitted.success, "launch error is a failed admission");
        else
            require(admitted.success, "worker admitted before completion");
        if (failure == Failure::Shutdown)
            app.shutdown();
        QElapsedTimer timer;
        timer.start();
        while (completions == 0 && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        const bool committed =
            failure == Failure::CommittedCompletion || failure == Failure::Shutdown;
        const auto actualRoot = committed ? target : source;
        require(suspended == 1 && resumed == 1 && completions == 1,
                "all exits restore acquired participants and complete exactly once");
        require(succeeded == committed && resumedRoot == actualRoot &&
                    app.configurationDirectory() == actualRoot && !app.directoryChanging() &&
                    app.status().writeAvailable,
                "restoration uses authoritative root and releases writes");
        require(app.configuration().setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                             QStringLiteral("disabled")),
                "configuration writes restored after exception");
        require(app.pinnedWindows().create(pin()).success, "pin writes restored after exception");
        QCoreApplication::processEvents();
        require(resumed == 1 && completions == 1, "retired poll cannot complete a second time");
        QObject::disconnect(connection);
        app.setDirectoryChangeHooks({}, {});
        app.shutdown();
    }
}

void rejectedSuspensionLeavesResourcesAndAdmissionAvailable() {
    QTemporaryDir fixture;
    auto& app = storage::ApplicationStorage::instance();
    const QString source = fixture.filePath(QStringLiteral("source"));
    const QString target = fixture.filePath(QStringLiteral("target"));
    storage::StorageInitializationOptions options{fixture.filePath(QStringLiteral("bin")), source,
                                                  60000};
    options.isolateAuxiliaryStorage = true;
    require(app.initialize(options).success, "rejected suspension fixture initialized");
    int suspensions = 0;
    int resumes = 0;
    int launches = 0;
    int completions = 0;
    app.setDirectoryChangeHooks(
        [&] {
            ++suspensions;
            return suspensions == 1 ? storage::StorageResult::failure(QStringLiteral("busy"))
                                    : storage::StorageResult::ok();
        },
        [&](const QString& root) {
            require(root == target, "only the acquired retry restores participants");
            ++resumes;
        },
        {},
        [&](storage::ApplicationStorage::DirectoryChangeTask work) {
            ++launches;
            return std::async(std::launch::async, std::move(work));
        });
    const auto connection = QObject::connect(
        &app, &storage::ApplicationStorage::directoryChangeFinished, &app, [&](const auto& result) {
            require(result.success, "retry completes its committed transaction");
            ++completions;
        });
    const auto rejected = app.requestDirectoryChange(target, true);
    require(
        !rejected.success && rejected.error == QStringLiteral("busy") && suspensions == 1 &&
            resumes == 0 && launches == 0 && completions == 0 && !app.directoryChanging() &&
            app.configurationDirectory() == source && app.status().writeAvailable &&
            app.configuration().setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                         QStringLiteral("disabled")) &&
            app.pinnedWindows().create(pin()).success,
        "failed acquisition launches no worker, restores no unowned pause and keeps writes open");
    require(app.requestDirectoryChange(target, true).success,
            "failed acquisition releases the preparation reservation for retry");
    app.waitForDirectoryChange();
    require(suspensions == 2 && resumes == 1 && launches == 1 && completions == 1 &&
                app.configurationDirectory() == target && !app.directoryChanging(),
            "successful retry owns exactly one suspension and completion");
    QObject::disconnect(connection);
    app.setDirectoryChangeHooks({}, {});
    app.shutdown();
}

void transactionCommitDoesNotReleaseWorkerOwnership() {
    QTemporaryDir fixture;
    auto& app = storage::ApplicationStorage::instance();
    const QString source = fixture.filePath(QStringLiteral("source"));
    const QString target = fixture.filePath(QStringLiteral("target"));
    storage::StorageInitializationOptions options{fixture.filePath(QStringLiteral("bin")), source,
                                                  60000};
    options.isolateAuxiliaryStorage = true;
    require(app.initialize(options).success, "worker ownership fixture initialized");
    std::promise<void> transactionFinished;
    auto transactionReady = transactionFinished.get_future();
    std::promise<void> releaseWorker;
    const auto workerReleased = releaseWorker.get_future().share();
    int resumes = 0;
    int completions = 0;
    app.setDirectoryChangeHooks(
        [] { return storage::StorageResult::ok(); },
        [&](const QString& root) {
            require(root == target, "worker restoration uses the committed destination");
            ++resumes;
        },
        {},
        [&](storage::ApplicationStorage::DirectoryChangeTask work) {
            return std::async(std::launch::async, [&, work = std::move(work)] {
                const auto result = work();
                require(result.success, "worker transaction commits before terminal release");
                transactionFinished.set_value();
                workerReleased.wait();
                return result;
            });
        });
    const auto connection = QObject::connect(
        &app, &storage::ApplicationStorage::directoryChangeFinished, &app, [&](const auto& result) {
            require(result.success, "worker release preserves the authoritative transaction");
            ++completions;
        });
    require(app.requestDirectoryChange(target, true).success, "worker ownership request admitted");
    require(transactionReady.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "transaction commits while the executor still owns borrowed resources");
    QCoreApplication::processEvents();
    require(
        app.directoryChanging() && !app.status().writeAvailable && resumes == 0 &&
            completions == 0 &&
            !app.configuration().setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                          QStringLiteral("disabled")) &&
            !app.pinnedWindows().create(pin()).success &&
            !app.requestDirectoryChange(fixture.filePath(QStringLiteral("next")), true).success,
        "transaction completion cannot reopen producers or retire a still occupied worker");
    releaseWorker.set_value();
    app.waitForDirectoryChange();
    QCoreApplication::processEvents();
    require(resumes == 1 && completions == 1 && !app.directoryChanging() &&
                app.status().writeAvailable && app.configurationDirectory() == target &&
                app.configuration().setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                             QStringLiteral("disabled")) &&
                app.pinnedWindows().create(pin()).success,
            "only actual worker completion releases admission and restores writes once");
    QObject::disconnect(connection);
    app.setDirectoryChangeHooks({}, {});
    app.shutdown();
}

void completionPreventsEarlyReentryAndAllowsFinalChaining() {
    QTemporaryDir fixture;
    auto& app = storage::ApplicationStorage::instance();
    const auto source = fixture.filePath(QStringLiteral("source"));
    const auto firstTarget = fixture.filePath(QStringLiteral("first"));
    const auto secondTarget = fixture.filePath(QStringLiteral("second"));
    require(app.initialize({fixture.filePath(QStringLiteral("bin")), source, 60000}).success,
            "reentrant completion fixture initialized");
    int completions = 0;
    int preparationRefusals = 0;
    int resumeRefusals = 0;
    int statusRefusals = 0;
    bool chained = false;
    app.setDirectoryChangeHooks(
        [&] {
            require(!app.requestDirectoryChange(
                            fixture.filePath(QStringLiteral("preparation-reentry")), true)
                         .success,
                    "suspension callback cannot recursively acquire a second storage move");
            ++preparationRefusals;
            return storage::StorageResult::ok();
        },
        [&](const QString& root) {
            if (root == firstTarget) {
                require(!app.requestDirectoryChange(secondTarget, true).success,
                        "resume cannot start a move before old completion finishes");
                ++resumeRefusals;
            }
        });
    const auto statusConnection = QObject::connect(
        &app, &storage::ApplicationStorage::storageStatusChanged, &app,
        [&](const storage::StorageStatus& status) {
            if (status.effectiveDirectory == firstTarget && !status.directoryChanging &&
                completions == 0) {
                require(!app.requestDirectoryChange(secondTarget, true).success,
                        "status callbacks cannot race old usage-tracker and state restoration");
                ++statusRefusals;
            }
        });
    const auto finishedConnection = QObject::connect(
        &app, &storage::ApplicationStorage::directoryChangeFinished, &app, [&](const auto& result) {
            require(result.success, "each chained migration commits");
            ++completions;
            if (completions == 1)
                chained = app.requestDirectoryChange(secondTarget, true).success;
        });
    require(app.requestDirectoryChange(firstTarget, true).success,
            "initial reentrant migration starts");
    QElapsedTimer timer;
    timer.start();
    while (completions < 2 && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(completions == 2 && chained && preparationRefusals == 2 && resumeRefusals == 1 &&
                statusRefusals > 0,
            "final completion permits legitimate chaining after rejecting earlier reentry");
    require(app.configurationDirectory() == secondTarget && !app.directoryChanging() &&
                app.status().writeAvailable &&
                app.configuration().setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                             QStringLiteral("disabled")),
            "old completion does not overwrite the chained operation's final root or availability");
    QObject::disconnect(statusConnection);
    QObject::disconnect(finishedConnection);
    app.setDirectoryChangeHooks({}, {});
    app.shutdown();
}

void busyNotificationOwnsTheWorkerBeforeLifecycleReentry() {
    QTemporaryDir fixture;
    auto& app = storage::ApplicationStorage::instance();
    const QString source = fixture.filePath(QStringLiteral("source"));
    const QString target = fixture.filePath(QStringLiteral("target"));
    const QString replacement = fixture.filePath(QStringLiteral("replacement"));
    storage::StorageInitializationOptions options{fixture.filePath(QStringLiteral("bin")), source,
                                                  60000};
    options.isolateAuxiliaryStorage = true;
    require(app.initialize(options).success, "busy notification fixture initialized");
    int resumed = 0;
    int completions = 0;
    bool replaced = false;
    app.setDirectoryChangeHooks([] { return storage::StorageResult::ok(); },
                                [&](const QString& root) {
                                    require(root == target,
                                            "busy notification shutdown restores committed root");
                                    ++resumed;
                                });
    const auto finishedConnection = QObject::connect(
        &app, &storage::ApplicationStorage::directoryChangeFinished, &app, [&](const auto& result) {
            require(result.success, "busy notification shutdown finishes its owned transaction");
            ++completions;
        });
    const auto statusConnection = QObject::connect(
        &app, &storage::ApplicationStorage::storageStatusChanged, &app,
        [&](const storage::StorageStatus& status) {
            if (!status.directoryChanging || replaced)
                return;
            // A public busy notification may synchronously run application teardown. The
            // teardown barrier must already own the worker and restore participants first.
            replaced = true;
            app.shutdown();
            require(resumed == 1 && completions == 1,
                    "busy notification cannot precede ownership of the worker barrier");
            auto next = options;
            next.appDataDirectory = replacement;
            require(app.initialize(next).success, "status callback safely replaces storage");
        });
    require(app.requestDirectoryChange(target, true).success,
            "busy notification transaction was admitted before synchronous teardown");
    QCoreApplication::processEvents();
    require(replaced && resumed == 1 && completions == 1 && app.isInitialized() &&
                app.configurationDirectory() == replacement && !app.directoryChanging(),
            "retired request cannot launch against repositories replaced by its notification");
    QObject::disconnect(statusConnection);
    QObject::disconnect(finishedConnection);
    app.setDirectoryChangeHooks({}, {});
    app.shutdown();
}

void preparationClosesStorageAdmission() {
    QTemporaryDir fixture;
    auto& app = storage::ApplicationStorage::instance();
    const QString source = fixture.filePath(QStringLiteral("source"));
    const QString target = fixture.filePath(QStringLiteral("target"));
    storage::StorageInitializationOptions options{fixture.filePath(QStringLiteral("bin")), source,
                                                  60000};
    options.isolateAuxiliaryStorage = true;
    require(app.initialize(options).success && app.flushNow().success,
            "preparation admission fixture initialized and settled");
    QCoreApplication::processEvents();
    const auto oldHistoryPolicy = app.captureHistoryPolicy();
    const auto oldPinnedPolicy = app.pinnedWindowPolicy();
    auto historyPolicy = oldHistoryPolicy;
    historyPolicy.enabled = !historyPolicy.enabled;
    auto pinnedPolicy = oldPinnedPolicy;
    pinnedPolicy.enabled = !pinnedPolicy.enabled;
    int pinRequests = 0;
    const auto showConnection =
        QObject::connect(&app, &storage::ApplicationStorage::pinnedWindowShowRequested, &app,
                         [&](const auto&) { ++pinRequests; });
    const auto showsConnection =
        QObject::connect(&app, &storage::ApplicationStorage::pinnedWindowsShowRequested, &app,
                         [&](const auto&) { ++pinRequests; });
    const auto deleteConnection =
        QObject::connect(&app, &storage::ApplicationStorage::pinnedWindowDeleteRequested, &app,
                         [&](const auto&) { ++pinRequests; });
    const auto checkBlocked = [&] {
        require(!app.requestCaptureHistoryPolicyAsync(historyPolicy).get().success &&
                    !app.requestCaptureHistoryClearAsync().get().success &&
                    !app.requestPinnedWindowPolicy(pinnedPolicy) &&
                    !app.requestPinnedWindowClear() &&
                    !app.requestThumbnailCacheClearAsync().get().success &&
                    !app.requestRecordingTempClearAsync().get().success &&
                    !app.requestSmartSelectionModeAsync(snow_shot::SmartSelectionMode::Disabled)
                         .get()
                         .success &&
                    !app.checkpoint().result().success,
                "preparation and restoration reject new storage producers before draining");
        app.requestPinnedWindowShow(QStringLiteral("pin"));
        app.requestPinnedWindowsShow({QStringLiteral("pin")});
        app.requestPinnedWindowDelete({QStringLiteral("pin")});
        app.requestPinnedWindowRetentionCleanup();
        app.requestStorageUsageRefresh();
        app.requestStorageUsageRefreshIfStale();
        const auto status = app.status();
        require(pinRequests == 0 && app.captureHistoryPolicy() == oldHistoryPolicy &&
                    app.pinnedWindowPolicy() == oldPinnedPolicy && !status.historyClearing &&
                    !status.pinnedClearing && !status.cacheClearing &&
                    !status.historyPolicyUpdating && !status.pinnedPolicyUpdating,
                "blocked requests leave policies, command publication and busy flags unchanged");
    };
    int phases = 0;
    app.setDirectoryChangeHooks(
        [&] {
            require(!app.directoryChanging(), "application admission precedes public busy state");
            checkBlocked();
            ++phases;
            return storage::StorageResult::ok();
        },
        [&](const QString& root) {
            require(root == target, "restoration receives committed destination");
            checkBlocked();
            ++phases;
        });
    require(app.requestDirectoryChange(target, true).success, "prepared migration starts");
    app.waitForDirectoryChange();
    require(phases == 2 && app.configurationDirectory() == target && !app.directoryChanging() &&
                app.checkpoint().result().success && app.requestPinnedWindowPolicy(pinnedPolicy) &&
                app.requestCaptureHistoryPolicyAsync(historyPolicy).get().success,
            "final completion releases admission with original policies intact");
    app.requestPinnedWindowShow(QStringLiteral("pin"));
    require(pinRequests == 1, "pin commands reopen only after restoration finishes");
    QObject::disconnect(showConnection);
    QObject::disconnect(showsConnection);
    QObject::disconnect(deleteConnection);
    app.setDirectoryChangeHooks({}, {});
    app.shutdown();
}

void preparationRevalidatesTheLiveStorageOwner() {
    enum class Change { Shutdown, ReplaceRoot, ReplaceRepositories };
    for (const auto change : {Change::Shutdown, Change::ReplaceRoot, Change::ReplaceRepositories}) {
        QTemporaryDir fixture;
        auto& app = storage::ApplicationStorage::instance();
        const QString source = fixture.filePath(QStringLiteral("source"));
        const QString replacement = fixture.filePath(QStringLiteral("replacement"));
        const QString currentRoot = change == Change::ReplaceRoot ? replacement : source;
        storage::StorageInitializationOptions options{fixture.filePath(QStringLiteral("bin")),
                                                      source, 60000};
        options.isolateAuxiliaryStorage = true;
        require(app.initialize(options).success, "live-owner admission fixture initialized");
        int resumes = 0;
        bool launched = false;
        app.setDirectoryChangeHooks(
            [&] {
                app.shutdown();
                if (change != Change::Shutdown) {
                    auto next = options;
                    next.appDataDirectory = currentRoot;
                    require(app.initialize(next).success, "nested lifecycle replaces storage");
                }
                return storage::StorageResult::ok();
            },
            [&](const QString& root) {
                ++resumes;
                require(root == currentRoot,
                        "rejected preparation restores against the current authoritative root");
            },
            {},
            [&](storage::ApplicationStorage::DirectoryChangeTask work) {
                launched = true;
                return std::async(std::launch::async, std::move(work));
            });
        require(
            !app.requestDirectoryChange(fixture.filePath(QStringLiteral("target")), true).success &&
                resumes == 1 && !launched && !app.directoryChanging() &&
                app.isInitialized() == (change != Change::Shutdown) &&
                app.configurationDirectory() == currentRoot,
            "final admission rejects changed ownership without launching or restoring stale state");
        app.setDirectoryChangeHooks({}, {});
        app.shutdown();
    }
}

void progressDeliveryIsBoundedAndRetiredBeforeCompletion() {
    for (const bool deliverBeforeCompletion : {false, true}) {
        QTemporaryDir fixture;
        auto& app = storage::ApplicationStorage::instance();
        const auto source = fixture.filePath(QStringLiteral("source"));
        const auto target = fixture.filePath(QStringLiteral("target"));
        require(app.initialize({fixture.filePath(QStringLiteral("bin")), source, 60000}).success,
                "progress fixture initialized");
        for (int index = 0; index < 100; ++index)
            write(QDir(source).filePath(QStringLiteral("assets/ocr/%1").arg(index)), "model");

        std::promise<void> transactionFinished;
        auto transactionReady = transactionFinished.get_future();
        std::promise<void> releaseWorker;
        auto workerReleased = releaseWorker.get_future().share();
        app.setDirectoryChangeHooks(
            {}, {}, {}, [&](storage::ApplicationStorage::DirectoryChangeTask work) {
                return std::async(std::launch::async, [&, work = std::move(work)] {
                    const auto result = work();
                    transactionFinished.set_value();
                    workerReleased.wait();
                    return result;
                });
            });
        int notifications = 0;
        int completions = 0;
        storage::StorageDirectoryProgress latest;
        const auto progressConnection = QObject::connect(
            &app, &storage::ApplicationStorage::directoryChangeProgress, &app,
            [&](const auto& progress) {
                require(completions == 0, "retired progress never arrives after completion");
                ++notifications;
                latest = progress;
            });
        const auto finishedConnection = QObject::connect(
            &app, &storage::ApplicationStorage::directoryChangeFinished, &app,
            [&](const auto& result) {
                require(result.success, "progress fixture transaction commits");
                require(notifications == 1 &&
                            latest.stage == storage::StorageDirectoryProgress::Stage::Cleaning &&
                            latest.total >= 100 && latest.completed == latest.total - 1,
                        "latest terminal progress is delivered before completion");
                ++completions;
            });
        require(app.requestDirectoryChange(target, true).success, "progress transaction starts");
        // Withhold owner-thread event processing until the worker has published every file.
        // The worker's future stays unready so the completion poll cannot race this assertion.
        require(transactionReady.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
                "transaction can finish without owner-thread progress delivery");
        if (deliverBeforeCompletion) {
            QCoreApplication::sendPostedEvents(&app, QEvent::MetaCall);
            require(notifications == 1,
                    "hundreds of file reports retain only one pending GUI snapshot");
        }
        releaseWorker.set_value();
        app.waitForDirectoryChange();
        QCoreApplication::sendPostedEvents(&app, QEvent::MetaCall);
        require(completions == 1 && notifications == 1 && app.configurationDirectory() == target,
                "completion retires queued progress without changing committed root");
        QObject::disconnect(progressConnection);
        QObject::disconnect(finishedConnection);
        app.setDirectoryChangeHooks({}, {});
        app.shutdown();
    }
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
    require(configuration->setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                    QStringLiteral("disabled")),
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
    QStringList stacking{open.id, closed.id, filePin.id};
    require(pins->updateStackingOrder(QStringLiteral("default"), stacking).success,
            "non-timestamp stacking metadata before relocation");
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
    const QString diagnosticSession = diagnostics.status().sessionId;
    require(app.flushNow().success, "flush before migration");
    std::atomic_bool stopDiagnostics{false};
    std::atomic_bool invalidSession{false};
    std::promise<void> diagnosticsStarted;
    auto diagnosticsReady = diagnosticsStarted.get_future();
    std::thread diagnosticsReader([&] {
        diagnosticsStarted.set_value();
        while (!stopDiagnostics.load()) {
            const auto state = diagnostics.status();
            if (state.sessionId != diagnosticSession ||
                state.directory != QDir(source).filePath(QStringLiteral("logs")) ||
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
    require(!configuration->setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                     QStringLiteral("window_sub_element")),
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
    require(app.smartSelectionMode() == snow_shot::SmartSelectionMode::Disabled,
            "settings preserved");
    require(diagnostics.status().directory == QDir(source).filePath(QStringLiteral("logs")) &&
                diagnostics.status().sessionId == diagnosticSession &&
                !QFileInfo::exists(QDir(target).filePath(QStringLiteral("logs"))) &&
                diagnostics.flush(),
            "diagnostics stay in place without restarting during data migration");
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
    if (!migrate)
        stacking.removeAll(closed.id);
    require(pins->stackingOrder(QStringLiteral("default")) == stacking,
            "relocation retains relative order and prunes removed members");
    require(QFileInfo::exists(QDir(target).filePath(QStringLiteral("assets/ocr/test"))) == migrate,
            "OCR migration follows switch");
    require(configuration->setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                    QStringLiteral("window_sub_element")) &&
                app.flushNow().success,
            "new settings remain writable");
    require(pins->create(pin()).success && pins->flush().success, "new pinned records writable");
    require(QFileInfo::exists(QDir(source).filePath(QStringLiteral("config.json"))) != migrate,
            "source cleanup follows migration switch");
    diagnostics.shutdown();
    app.shutdown();
    require(app.initialize({dir.filePath(QStringLiteral("bin")), source, 60000}).success &&
                app.configurationDirectory() == target &&
                app.smartSelectionMode() == snow_shot::SmartSelectionMode::WindowSubElement,
            "restart uses committed data");
    app.shutdown();
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--stacking-only"))) {
        liveSwitch(true);
        liveSwitch(false);
        return 0;
    }
    validation();
    junctionsAreNeverTraversed();
    transactions();
    crossVolumeCopy();
    failures();
    transactionExceptionsRespectCommit();
    recovery();
    liveSwitch(true);
    liveSwitch(false);
    flushFailureRestoresAvailability();
    asynchronousExceptionsRestoreParticipants();
    rejectedSuspensionLeavesResourcesAndAdmissionAvailable();
    transactionCommitDoesNotReleaseWorkerOwnership();
    completionPreventsEarlyReentryAndAllowsFinalChaining();
    busyNotificationOwnsTheWorkerBeforeLifecycleReentry();
    preparationClosesStorageAdmission();
    preparationRevalidatesTheLiveStorageOwner();
    progressDeliveryIsBoundedAndRetiredBeforeCompletion();
}
