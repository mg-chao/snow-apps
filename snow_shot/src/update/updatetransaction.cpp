#include "snow_shot/update/updatetransaction.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QLockFile>
#include <QProcess>
#include <QSettings>
#include <QStorageInfo>
#include <QUuid>
#include <QTemporaryDir>
#include <QDateTime>
#include <QRegularExpression>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <algorithm>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace snow_shot::update {
namespace {
constexpr auto kRecord = "snow-shot-installation.json";
constexpr auto kWork = ".snow-shot-update";

#ifdef Q_OS_WIN
QString nativeLongPath(const QString& path) {
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    return native.startsWith(QStringLiteral("\\\\"))
               ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2)
               : QStringLiteral("\\\\?\\") + native;
}
#endif

QString pathAt(const QString& root, const QString& relative) {
    return QDir(root).filePath(relative);
}

void noLinks(const QString& path) {
    QString part = QFileInfo(path).absoluteFilePath();
    while (!part.isEmpty()) {
        const QFileInfo info(part);
        requireUpdate(!info.isSymLink(), "Update paths must not contain symbolic links");
#ifdef Q_OS_WIN
        const QString native = nativeLongPath(part);
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
        requireUpdate(attributes == INVALID_FILE_ATTRIBUTES ||
                          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
                      "Update paths must not contain reparse points");
#endif
        const QString parent = info.absolutePath();
        if (parent == part) {
            break;
        }
        part = parent;
    }
}

QString workPath(const QString& root, const QString& leaf = {}) {
    return pathAt(pathAt(root, QString::fromLatin1(kWork)), leaf);
}

QString transactionLockPath(const QString& root) {
    const QString path = workPath(root, QStringLiteral("transaction.lock"));
#ifdef Q_OS_WIN
    // QLockFile's native delete/retry path needs an explicit long-path prefix too.
    return nativeLongPath(path);
#else
    return path;
#endif
}

QString selectedDataRoot(const QString& root) {
    const QString marker = pathAt(root, QStringLiteral("bin/__data_directory"));
    QString data = QStringLiteral("portable");
    if (QFileInfo::exists(marker)) {
        data = QString::fromUtf8(readLimited(marker, 32768));
        while (!data.isEmpty() && data.front() == QChar::ByteOrderMark) {
            data.remove(0, 1);
        }
        data = data.trimmed();
    }
    if (data.isEmpty()) {
        return {};
    }
    const QString selected =
        QDir::cleanPath(QDir(pathAt(root, QStringLiteral("bin"))).absoluteFilePath(data));
    const QString canonical = QFileInfo(selected).canonicalFilePath();
    return canonical.isEmpty() ? selected : canonical;
}

void validateRoot(const QString& root) {
    const QFileInfo info(root);
    requireUpdate(info.isDir() && QDir::isAbsolutePath(root) &&
                      info.absoluteFilePath() != QDir::rootPath() &&
                      info.absoluteFilePath() != QDir::homePath() && info.fileName().size() > 1,
                  "Invalid Snow Shot installation root");
    noLinks(root);
    noLinks(workPath(root));
    const QString data = selectedDataRoot(root);
    const QString work = workPath(root);
    requireUpdate(data.isEmpty() || (work.compare(data, Qt::CaseInsensitive) != 0 &&
                                     !work.startsWith(data + u'/', Qt::CaseInsensitive) &&
                                     !data.startsWith(work + u'/', Qt::CaseInsensitive)),
                  "An update file would overwrite the selected data directory");
}

void saveJournal(const QString& root, const QJsonObject& journal) {
    writeAtomic(workPath(root, QStringLiteral("journal.json")),
                QJsonDocument(journal).toJson(QJsonDocument::Compact));
}

void persistFile(const QString& path) {
#ifdef Q_OS_WIN
    const QString native = nativeLongPath(path);
    HANDLE handle =
        CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_WRITE, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    requireUpdate(handle != INVALID_HANDLE_VALUE, "Could not persist update state");
    const bool saved = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    requireUpdate(saved, "Could not persist update state");
#else
    Q_UNUSED(path);
#endif
}

void verifyRecordInventory(const QJsonObject& record, const UpdatePackage& package) {
    auto owned = parseFileInventory(record.value(QStringLiteral("files")).toArray());
    QMap<QString, UpdateFile> expected;
    for (const auto& file : package.files) {
        if (file.path != u"snow-shot-installation.json") {
            expected.insert(file.path, file);
        }
    }
    for (const auto& file : owned) {
        const auto found = expected.constFind(file.path);
        requireUpdate(found != expected.cend() && found->size == file.size &&
                          found->sha256 == file.sha256,
                      "Update installation metadata does not match the signed release");
        expected.remove(file.path);
    }
    requireUpdate(expected.isEmpty(),
                  "Update installation metadata does not match the signed release");
}

void atomicCopy(const QString& source, const QString& destination) {
    noLinks(destination);
    requireUpdate(QDir().mkpath(QFileInfo(destination).absolutePath()),
                  "Could not create update destination");
    const QString temporary =
        destination + QStringLiteral(".snow-update-") + QUuid::createUuid().toString(QUuid::Id128);
    requireUpdate(QFile::copy(source, temporary), "Could not stage an update file");
    persistFile(temporary);
#ifdef Q_OS_WIN
    const QString sourcePath = nativeLongPath(temporary);
    const QString targetPath = nativeLongPath(destination);
    const bool moved = MoveFileExW(reinterpret_cast<LPCWSTR>(sourcePath.utf16()),
                                   reinterpret_cast<LPCWSTR>(targetPath.utf16()),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool moved = ::rename(QFile::encodeName(temporary).constData(),
                                QFile::encodeName(destination).constData()) == 0;
#endif
    if (!moved) {
        QFile::remove(temporary);
        throw std::runtime_error("Could not replace an update file; close applications using it");
    }
}

void clearWorkTree(const QString& root, const QString& name) {
    const QString path = workPath(root, name);
    noLinks(path);
    if (QFileInfo::exists(path)) {
        QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            noLinks(it.next());
        }
        requireUpdate(QDir(path).removeRecursively(), "Could not clear update staging directory");
    }
}

QString registeredKey(const QString& root) {
#ifdef Q_OS_WIN
    for (const QString& hive :
         {QStringLiteral("HKEY_LOCAL_MACHINE"), QStringLiteral("HKEY_CURRENT_USER")}) {
        QSettings install(hive + QStringLiteral("\\Software\\Snow Apps\\SnowShot"),
                          QSettings::Registry32Format);
        const QString registered = install.value(QStringLiteral(".")).toString();
        if (!registered.isEmpty() &&
            QDir::cleanPath(registered).compare(QDir::cleanPath(root), Qt::CaseInsensitive) == 0) {
            return hive +
                   QStringLiteral(
                       "\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SnowShot");
        }
    }
#else
    Q_UNUSED(root);
#endif
    return {};
}

void writeRegisteredVersion(const QString& root, const QString& version) {
#ifdef Q_OS_WIN
    const QString key = registeredKey(root);
    if (!key.isEmpty()) {
        QSettings settings(key, QSettings::Registry32Format);
        settings.setValue(QStringLiteral("DisplayVersion"), version);
        settings.sync();
        requireUpdate(settings.status() == QSettings::NoError,
                      "Could not update the registered application version");
    }
#else
    Q_UNUSED(root);
    Q_UNUSED(version);
#endif
}

void extract(const QString& archive, const QString& destination, const UpdatePackage& package) {
    void* reader = mz_zip_reader_create();
    requireUpdate(reader != nullptr, "Could not create update archive reader");
    struct ReaderGuard {
        void*& reader;
        ~ReaderGuard() {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
        }
    } guard{reader};
    requireUpdate(mz_zip_reader_open_file(reader, QFile::encodeName(archive).constData()) == MZ_OK,
                  "Could not open update archive");
    QHash<QString, UpdateFile> expected;
    for (const auto& file : package.files) {
        expected.insert(file.path, file);
    }
    int32_t code = mz_zip_reader_goto_first_entry(reader);
    while (code == MZ_OK) {
        mz_zip_file* entry = nullptr;
        requireUpdate(mz_zip_reader_entry_get_info(reader, &entry) == MZ_OK && entry != nullptr &&
                          entry->filename != nullptr &&
                          (entry->linkname == nullptr || entry->linkname[0] == '\0'),
                      "Invalid update archive entry");
        const QString name = QString::fromUtf8(entry->filename);
        const auto found = expected.constFind(name);
        const auto mode = (entry->external_fa >> 16) & 0170000;
        requireUpdate(found != expected.cend() && safeRelativePath(name) &&
                          (mode == 0 || mode == 0100000) && entry->uncompressed_size == found->size,
                      "Unexpected, duplicate, or unsafe update archive entry");
        const UpdateFile descriptor = *found;
        expected.remove(name);
        const QString output = pathAt(destination, name);
        requireUpdate(QDir().mkpath(QFileInfo(output).absolutePath()),
                      "Could not create update staging directory");
        QFile file(output);
        requireUpdate(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                          mz_zip_reader_entry_open(reader) == MZ_OK,
                      "Could not extract update archive entry");
        char buffer[65536];
        qint64 total = 0;
        for (;;) {
            const int32_t count = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer));
            requireUpdate(count >= 0, "Corrupt update archive entry");
            if (count == 0) {
                break;
            }
            total += count;
            requireUpdate(total <= descriptor.size && file.write(buffer, count) == count,
                          "Could not write update archive entry");
        }
        file.close();
        requireUpdate(mz_zip_reader_entry_close(reader) == MZ_OK,
                      "Corrupt update archive checksum");
        verifyFile(output, descriptor.size, descriptor.sha256);
        code = mz_zip_reader_goto_next_entry(reader);
    }
    requireUpdate(code == MZ_END_OF_LIST && expected.isEmpty(), "Incomplete update archive");
}

void restore(const QString& root) {
    const auto journal =
        QJsonDocument::fromJson(readLimited(workPath(root, QStringLiteral("journal.json"))))
            .object();
    requireUpdate(journal.value(QStringLiteral("schema")).toInt() == 1,
                  "Unsupported update recovery journal");
    if (journal.value(QStringLiteral("state")).toString() == u"committed") {
        requireUpdate(QFile::remove(workPath(root, QStringLiteral("journal.json"))),
                      "Could not finalize committed update state");
        return;
    }
    const auto entries = journal.value(QStringLiteral("files")).toArray();
    // Validate every backup before restoring anything. Never consume arbitrary journal paths.
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const QString name = entry.value(QStringLiteral("path")).toString();
        validateTargetPath(root, name);
        if (entry.value(QStringLiteral("existed")).toBool()) {
            const QString backup = workPath(root, QStringLiteral("backup/") + name);
            noLinks(backup);
            verifyFile(backup, entry.value(QStringLiteral("size")).toInteger(-1),
                       entry.value(QStringLiteral("sha256")).toString());
        }
    }
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const QString name = entry.value(QStringLiteral("path")).toString();
        const QString destination = pathAt(root, name);
        if (entry.value(QStringLiteral("existed")).toBool()) {
            atomicCopy(workPath(root, QStringLiteral("backup/") + name), destination);
        } else if (QFileInfo::exists(destination)) {
            requireUpdate(QFile::remove(destination), "Could not remove an incomplete update file");
        }
    }
    writeRegisteredVersion(root, journal.value(QStringLiteral("previousVersion")).toString());
    writeAtomic(workPath(root, QStringLiteral("failed-version.txt")),
                journal.value(QStringLiteral("version")).toString().toUtf8());
    requireUpdate(QFile::remove(workPath(root, QStringLiteral("journal.json"))),
                  "Could not finalize update recovery");
}
} // namespace

QString installationRoot(const QString& executableDirectory) {
    return QFileInfo(executableDirectory).dir().absolutePath();
}

void validateInstallationRoot(const QString& root) {
    validateRoot(root);
}

void pruneUpdateWork(const QString& root) {
    validateRoot(root);
    if (transactionPending(root) || !QFileInfo::exists(workPath(root))) {
        return;
    }
    QLockFile lock(transactionLockPath(root));
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        return;
    }
    static const QRegularExpression generated(
        QStringLiteral("^(worker-[a-f0-9]{32}\\.exe|input-[a-f0-9]{32}\\.zip)$"));
    for (const auto& file : QDir(workPath(root)).entryInfoList(QDir::Files)) {
        if (generated.match(file.fileName()).hasMatch() &&
            file.lastModified().secsTo(QDateTime::currentDateTimeUtc()) > 86400) {
            noLinks(file.absoluteFilePath());
            // Running Windows executables cannot be removed. Cleanup is best effort.
            QFile::remove(file.absoluteFilePath());
        }
    }
}

QJsonObject installationRecord(const QString& root) {
    const auto record =
        QJsonDocument::fromJson(readLimited(pathAt(root, QString::fromLatin1(kRecord)))).object();
    const QString variant = record.value(QStringLiteral("variant")).toString();
    requireUpdate(record.value(QStringLiteral("schema")).toInt() == 1 &&
                      (variant == u"online" || variant == u"offline" || variant == u"portable"),
                  "This copy does not have valid Snow Shot installation metadata");
    const QString version = record.value(QStringLiteral("version")).toString();
    compareVersions(version, version);
    parseFileInventory(record.value(QStringLiteral("files")).toArray());
    return record;
}

void validateTargetPath(const QString& root, const QString& relative) {
    requireUpdate(
        safeRelativePath(relative) &&
            (relative.startsWith(u"bin/") || relative.startsWith(u"share/snow-shot/") ||
             relative == u"snow-shot-installation.json") &&
            relative.compare(QStringLiteral("bin/__data_directory"), Qt::CaseInsensitive) != 0,
        "Refusing to change a file outside the application payload");
    noLinks(pathAt(root, relative));
    const QString dataRoot = selectedDataRoot(root);
    if (!dataRoot.isEmpty()) {
        const QString target = QDir::cleanPath(pathAt(root, relative));
        requireUpdate(target.compare(dataRoot, Qt::CaseInsensitive) != 0 &&
                          !target.startsWith(dataRoot + u'/', Qt::CaseInsensitive),
                      "An update file would overwrite the selected data directory");
    }
}

bool transactionPending(const QString& root) {
    return QFileInfo::exists(workPath(root, QStringLiteral("journal.json")));
}

void recoverTransaction(const QString& root) {
    validateRoot(root);
    QLockFile lock(transactionLockPath(root));
    lock.setStaleLockTime(0);
    requireUpdate(lock.tryLock(), "Another update transaction is running");
    if (transactionPending(root)) {
        restore(root);
    }
}

void applyTransaction(const QString& root, const QString& archive, const UpdateRelease& release,
                      const TransactionHooks& hooks) {
    validateRoot(root);
    requireUpdate(QDir().mkpath(workPath(root)), "Could not create update work directory");
    QLockFile lock(transactionLockPath(root));
    lock.setStaleLockTime(0);
    requireUpdate(lock.tryLock(), "Another update transaction is running");
    if (transactionPending(root)) {
        restore(root);
    }
    const auto installed = installationRecord(root);
    requireUpdate(
        compareVersions(release.version, installed.value(QStringLiteral("version")).toString()) > 0,
        "The update must be newer than the installed release");
    const auto& package =
        release.updatePackage(installed.value(QStringLiteral("variant")).toString());
    verifyFile(archive, package.size, package.sha256);
    qint64 needed = package.size;
    for (const auto& file : package.files) {
        needed += file.size * 3;
    }
    const QStorageInfo storage(root);
    requireUpdate(storage.isReady() && storage.bytesAvailable() > needed + 64 * 1024 * 1024,
                  "Not enough free space to stage and recover this update");
    clearWorkTree(root, QStringLiteral("stage"));
    clearWorkTree(root, QStringLiteral("backup"));
    extract(archive, workPath(root, QStringLiteral("stage")), package);
    const auto stagedRecord = installationRecord(workPath(root, QStringLiteral("stage")));
    verifyRecordInventory(stagedRecord, package);
    requireUpdate(stagedRecord.value(QStringLiteral("version")).toString() == release.version &&
                      stagedRecord.value(QStringLiteral("variant")) ==
                          installed.value(QStringLiteral("variant")),
                  "Update installation metadata does not match the signed release");
    QMap<QString, UpdateFile> next;
    QSet<QString> paths;
    QSet<QString> previousOwned;
    QMap<QString, QString> nextNames;
    for (const auto& file : package.files) {
        if (file.path == u"bin/__data_directory") {
            continue;
        }
        validateTargetPath(root, file.path);
        paths.insert(file.path);
        next.insert(file.path, file);
        nextNames.insert(file.path.toLower(), file.path);
    }
    for (const auto& file :
         parseFileInventory(installed.value(QStringLiteral("files")).toArray())) {
        if (file.path == u"bin/__data_directory") {
            continue;
        }
        validateTargetPath(root, file.path);
        previousOwned.insert(file.path.toLower());
        paths.insert(nextNames.value(file.path.toLower(), file.path));
    }
    QJsonArray entries;
    for (const auto& name : paths) {
        const QString original = pathAt(root, name);
        const bool existed = QFileInfo::exists(original);
        requireUpdate(!existed || previousOwned.contains(name.toLower()) ||
                          name == u"snow-shot-installation.json",
                      "An update file conflicts with an existing user file");
        QJsonObject entry{{QStringLiteral("path"), name}, {QStringLiteral("existed"), existed}};
        if (existed) {
            requireUpdate(QFileInfo(original).isFile(),
                          "An update file conflicts with a directory");
            const QString backup = workPath(root, QStringLiteral("backup/") + name);
            requireUpdate(QDir().mkpath(QFileInfo(backup).absolutePath()) &&
                              QFile::copy(original, backup),
                          "Could not back up the current application");
            entry.insert(QStringLiteral("size"), QFileInfo(original).size());
            entry.insert(QStringLiteral("sha256"), sha256File(original));
            verifyFile(backup, QFileInfo(original).size(),
                       entry.value(QStringLiteral("sha256")).toString());
            persistFile(backup);
        }
        entries.append(entry);
    }
    QJsonObject journal{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("state"), QStringLiteral("applying")},
        {QStringLiteral("version"), release.version},
        {QStringLiteral("previousVersion"), installed.value(QStringLiteral("version"))},
        {QStringLiteral("files"), entries}};
    saveJournal(root, journal);
    try {
        if (hooks.checkpoint) {
            hooks.checkpoint(QStringLiteral("prepared"));
        }
        for (const auto& name : paths) {
            if (next.contains(name)) {
                atomicCopy(workPath(root, QStringLiteral("stage/") + name), pathAt(root, name));
                const auto& file = next[name];
                verifyFile(pathAt(root, name), file.size, file.sha256);
            } else if (QFileInfo::exists(pathAt(root, name))) {
                requireUpdate(QFile::remove(pathAt(root, name)),
                              "Could not remove obsolete application file");
            }
            if (hooks.checkpoint) {
                hooks.checkpoint(name);
            }
        }
        writeRegisteredVersion(root, release.version);
        if (hooks.checkpoint) {
            hooks.checkpoint(QStringLiteral("registry"));
        }
        bool ready = false;
        if (hooks.probe) {
            ready = hooks.probe();
        } else {
            QProcess probe;
            probe.setProgram(pathAt(root, QStringLiteral("bin/snow_shot.exe")));
            probe.setArguments({QStringLiteral("--update-probe"), release.version});
            probe.setProcessChannelMode(QProcess::MergedChannels);
            probe.start();
            if (probe.waitForStarted(10000) && probe.waitForFinished(60000)) {
                ready = probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
            } else {
                probe.kill();
                probe.waitForFinished(5000);
            }
        }
        requireUpdate(
            ready, "The new application failed its startup check; restoring the previous version");
        journal.insert(QStringLiteral("state"), QStringLiteral("committed"));
        saveJournal(root, journal);
        QFile::remove(workPath(root, QStringLiteral("failed-version.txt")));
        // Keep one complete backup. A committed journal is harmless after power loss.
        QFile::remove(workPath(root, QStringLiteral("journal.json")));
    } catch (...) {
        restore(root);
        throw;
    }
}

void uninstallOwnedFiles(const QString& root) {
    validateRoot(root);
    requireUpdate(QDir().mkpath(workPath(root)), "Could not create uninstall lock directory");
    QLockFile lock(transactionLockPath(root));
    lock.setStaleLockTime(0);
    requireUpdate(lock.tryLock(), "An update is still running");
    if (transactionPending(root)) {
        restore(root);
    }
    const auto record = installationRecord(root);
    const auto files = parseFileInventory(record.value(QStringLiteral("files")).toArray());
    for (const auto& file : files) {
        if (file.path != u"bin/__data_directory") {
            validateTargetPath(root, file.path);
        }
    }
    for (const auto& file : files) {
        if (file.path == u"bin/__data_directory") {
            continue;
        }
        const QString path = pathAt(root, file.path);
        if (QFileInfo::exists(path)) {
            requireUpdate(QFile::remove(path), "Could not remove an owned application file");
        }
    }
    lock.unlock();
    clearWorkTree(root, QString());
}

void auditRelease(const QString& directory, const UpdateRelease& release) {
    for (const auto& package : release.packages) {
        const QString archive = QDir(directory).filePath(package.path);
        verifyFile(archive, package.size, package.sha256);
        if (package.kind == u"installer") {
            continue;
        }
        QTemporaryDir temporary;
        requireUpdate(temporary.isValid(), "Could not create release audit directory");
        extract(archive, temporary.path(), package);
        const auto record = installationRecord(temporary.path());
        verifyRecordInventory(record, package);
        requireUpdate(record.value(QStringLiteral("version")).toString() == release.version &&
                          record.value(QStringLiteral("variant")).toString() == package.variant,
                      "Release archive installation metadata mismatch");
        const QString manifest = temporary.filePath(QStringLiteral("release-audit.json"));
        writeAtomic(manifest, release.envelope);
        QProcess helper;
        helper.start(temporary.filePath(QStringLiteral("bin/snow-shot-updater.exe")),
                     {QStringLiteral("--verify-release"), QStringLiteral("--manifest"), manifest});
        const bool trusted = helper.waitForStarted(10000) && helper.waitForFinished(30000) &&
                             helper.exitStatus() == QProcess::NormalExit && helper.exitCode() == 0;
        if (helper.state() != QProcess::NotRunning) {
            helper.kill();
            helper.waitForFinished(5000);
        }
        requireUpdate(trusted, "Release signature is invalid or its signing key is not trusted");
        QProcess probe;
        probe.setProgram(QDir(temporary.path()).filePath(QStringLiteral("bin/snow_shot.exe")));
        probe.setArguments({QStringLiteral("--update-probe"), release.version});
        probe.start();
        const bool succeeded = probe.waitForStarted(10000) && probe.waitForFinished(60000) &&
                               probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
        if (probe.state() != QProcess::NotRunning) {
            probe.kill();
            probe.waitForFinished(5000);
        }
        requireUpdate(succeeded, "The packaged application failed its isolated startup probe");
    }
}
} // namespace snow_shot::update
