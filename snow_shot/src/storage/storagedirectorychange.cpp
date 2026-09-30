#include "snow_shot/storage/storagedirectorychange.h"
#include "storagedirectoryutils_p.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <algorithm>

namespace snow_shot::storage {
namespace {
QString message(const char* text) {
    return QCoreApplication::translate("StorageDirectoryChange", text);
}
QString normalized(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}
QString resolvedPath(QString path) {
    path = QDir::cleanPath(QDir::fromNativeSeparators(path));
    QStringList suffix;
    for (;;) {
        const QFileInfo info(path);
        const auto canonical = info.canonicalFilePath();
        if (!canonical.isEmpty())
            return QDir::cleanPath(canonical + u'/' + suffix.join(u'/'));
        const auto parent = info.absolutePath();
        if (normalized(parent) == normalized(path))
            return QDir::cleanPath(path + u'/' + suffix.join(u'/'));
        suffix.prepend(info.fileName());
        path = parent;
    }
}
bool contains(const QString& parent, const QString& child) {
    const auto root = normalized(parent);
    const auto path = normalized(child);
    return path == root || path.startsWith(root.endsWith(u'/') ? root : root + u'/');
}
bool linkedPath(QString path) {
    for (;;) {
        const QFileInfo info(path);
        if (storageLink(info))
            return true;
        const QString parent = info.absolutePath();
        if (normalized(parent) == normalized(path))
            return false;
        path = parent;
    }
}
StorageResult writeObject(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return StorageResult::failure(message(QT_TRANSLATE_NOOP(
            "StorageDirectoryChange", "Could not save the storage directory selection.")));
    return StorageResult::ok();
}
QJsonObject readObject(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                          : QJsonObject();
}
QByteArray digest(const QString& path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) || file.error() != QFile::NoError)
        return {};
    return hash.result();
}
struct Entry {
    QString relative;
    qint64 size = 0;
    QByteArray hash;
};
QString category(const QString& path) {
    if (path.startsWith(u"capture_history/"))
        return QStringLiteral("history");
    if (path.startsWith(u"pinned_windows_v2/"))
        return QStringLiteral("pinned");
    if (path.startsWith(u"assets/"))
        return QStringLiteral("ocr");
    if (path.startsWith(u"logs/"))
        return QStringLiteral("logs");
    return QStringLiteral("other");
}
StorageResult inventory(const QString& root, const QString& relative, QVector<Entry>& entries,
                        QStringList& directories, bool migrate,
                        const std::function<bool(const QString&)>& includePath) {
    const auto children =
        QDir(QDir(root).filePath(relative))
            .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                           QDir::Name);
    for (const auto& child : children) {
        const QString name =
            relative.isEmpty() ? child.fileName() : relative + u'/' + child.fileName();
        if (relative.isEmpty() && child.fileName().startsWith(u".snow-shot-storage-"))
            continue;
        if (!migrate && relative.isEmpty() && name != u"config.json" &&
            name != u"pinned_windows_v2")
            continue;
        if (includePath && !includePath(name))
            continue;
        if (storageLink(child) || (!child.isDir() && !child.isFile()))
            return StorageResult::failure(
                message(QT_TRANSLATE_NOOP("StorageDirectoryChange",
                                          "Storage contains a link or unsupported file: %1"))
                    .arg(child.absoluteFilePath()));
        if (child.isDir()) {
            directories.append(name);
            const auto result = inventory(root, name, entries, directories, migrate, includePath);
            if (!result.success)
                return result;
        } else {
            entries.append({name, child.size(), {}});
        }
    }
    return StorageResult::ok();
}
} // namespace

QString storageBootstrapPath(const QString& directory, const QString& executableDirectory) {
    const auto key = QCryptographicHash::hash(normalized(executableDirectory).toUtf8(),
                                              QCryptographicHash::Sha256)
                         .toHex();
    return QDir(directory).filePath(
        QStringLiteral(".snow-shot-storage-%1.json").arg(QString::fromLatin1(key)));
}

QString savedStorageDirectory(const QString& directory, const QString& executableDirectory,
                              QString* recoveryWarning) {
    const auto path = storageBootstrapPath(directory, executableDirectory);
    const auto object = readObject(path);
    if (recoveryWarning && object.contains(QStringLiteral("transaction")))
        *recoveryWarning = message(
            QT_TRANSLATE_NOOP("StorageDirectoryChange",
                              "An interrupted storage migration was detected. The last committed "
                              "directory is in use; remaining copies have been preserved."));
    if (recoveryWarning && QFileInfo::exists(path) && object.isEmpty())
        *recoveryWarning = message(QT_TRANSLATE_NOOP(
            "StorageDirectoryChange", "The saved storage directory selection could not be read."));
    const QString selected = object.value(QStringLiteral("directory")).toString();
    return QDir::isAbsolutePath(selected) ? selected : QString();
}

StorageResult validateStorageDirectory(const QString& source, const QString& destination) {
    if (destination.trimmed().isEmpty() || !QDir::isAbsolutePath(destination) || source.isEmpty() ||
        !QDir::isAbsolutePath(source))
        return StorageResult::failure(message(QT_TRANSLATE_NOOP(
            "StorageDirectoryChange", "Choose an absolute storage directory path.")));
#ifdef Q_OS_WIN
    const auto windowsPath = QDir::fromNativeSeparators(destination);
    const bool drivePath = windowsPath.size() >= 3 && windowsPath.at(0).isLetter() &&
                           windowsPath.at(1) == u':' && windowsPath.at(2) == u'/';
    const bool networkPath = windowsPath.startsWith(u"//") && !windowsPath.startsWith(u"//?/") &&
                             !windowsPath.startsWith(u"//./");
    if (!drivePath && !networkPath)
        return StorageResult::failure(message(QT_TRANSLATE_NOOP(
            "StorageDirectoryChange", "Choose an absolute storage directory path.")));
#endif
    const auto components = QDir::fromNativeSeparators(destination).split(u'/');
    for (const auto& component : components) {
        if (component != u"." && component != u".." &&
            (component.endsWith(u'.') || component.endsWith(u' ')))
            return StorageResult::failure(message(QT_TRANSLATE_NOOP(
                "StorageDirectoryChange", "Choose an absolute storage directory path.")));
    }
    if (contains(resolvedPath(source), resolvedPath(destination)) ||
        contains(resolvedPath(destination), resolvedPath(source)))
        return StorageResult::failure(message(QT_TRANSLATE_NOOP(
            "StorageDirectoryChange",
            "The new directory must be separate from the current storage directory.")));
    if (linkedPath(source) || linkedPath(destination))
        return StorageResult::failure(message(QT_TRANSLATE_NOOP(
            "StorageDirectoryChange",
            "Storage directory paths must not contain symbolic links or junctions.")));
    const QFileInfo target(destination);
    if (target.exists() &&
        (!target.isDir() ||
         !QDir(destination)
              .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)
              .isEmpty()))
        return StorageResult::failure(message(
            QT_TRANSLATE_NOOP("StorageDirectoryChange", "Choose a new or empty directory.")));
    return StorageResult::ok();
}

StorageDirectoryChangeResult changeStorageDirectory(const StorageDirectoryChangeOptions& o) {
    const auto validation = validateStorageDirectory(o.source, o.destination);
    if (!validation.success)
        return {false, validation.error, {}};
    auto report = [&](StorageDirectoryProgress::Stage stage, const QString& group = {},
                      qint64 done = 0, qint64 total = 0) {
        if (o.progress)
            o.progress({stage, group, done, total});
    };
    auto checkpoint = [&](const QString& point) {
        return o.checkpoint ? o.checkpoint(point) : StorageResult::ok();
    };
    report(StorageDirectoryProgress::Stage::Preparing);
    QVector<Entry> entries;
    QStringList directories;
    auto result = inventory(o.source, {}, entries, directories, o.migrate, o.includePath);
    if (!result.success)
        return {false, result.error, {}};
    if (!QDir().mkpath(o.bootstrapDirectory) || !QDir().mkpath(o.destination))
        return {false,
                message(QT_TRANSLATE_NOOP("StorageDirectoryChange",
                                          "Could not create the storage directory.")),
                {}};
    const QString bootstrap = storageBootstrapPath(o.bootstrapDirectory, o.executableDirectory);
    QLockFile lock(bootstrap + QStringLiteral(".lock"));
    if (!lock.tryLock(0))
        return {false,
                message(QT_TRANSLATE_NOOP("StorageDirectoryChange",
                                          "Another storage migration is in progress.")),
                {}};
    QTemporaryFile probe(QDir(o.destination).filePath(QStringLiteral(".snow-shot-write-XXXXXX")));
    if (!probe.open())
        return {false,
                message(QT_TRANSLATE_NOOP("StorageDirectoryChange",
                                          "The storage directory is not writable.")),
                {}};
    probe.close();
    probe.remove();
    qint64 required = 0;
    for (const auto& entry : entries)
        required += entry.size;
    const QStorageInfo volume(o.destination);
    if (!volume.isValid() || !volume.isReady() || volume.isReadOnly() ||
        volume.bytesAvailable() < required)
        return {
            false,
            message(QT_TRANSLATE_NOOP("StorageDirectoryChange",
                                      "The destination does not have enough writable disk space.")),
            {}};

    const bool previousExists = QFileInfo::exists(bootstrap);
    const QJsonObject previous = readObject(bootstrap);
    QJsonObject state = previous;
    QJsonArray manifest;
    for (const auto& entry : entries)
        manifest.append(entry.relative);
    state.insert(QStringLiteral("directory"), o.source);
    state.insert(QStringLiteral("transaction"),
                 QJsonObject{{QStringLiteral("source"), o.source},
                             {QStringLiteral("destination"), o.destination},
                             {QStringLiteral("files"), manifest}});
    result = writeObject(bootstrap, state);
    if (!result.success)
        return {false, result.error, {}};
    QStringList copied;
    auto fail = [&](const QString& error) -> StorageDirectoryChangeResult {
        if (o.rollback)
            o.rollback();
        for (const auto& relative : copied) {
            const auto path = QDir(o.destination).filePath(relative);
            if (contains(o.destination, path) && !linkedPath(path))
                QFile::remove(path);
        }
        for (auto it = directories.crbegin(); it != directories.crend(); ++it) {
            const auto path = QDir(o.destination).filePath(*it);
            if (contains(o.destination, path) && !linkedPath(path))
                QDir().rmdir(path);
        }
        const auto restored =
            previousExists ? writeObject(bootstrap, previous)
                           : (QFile::remove(bootstrap)
                                  ? StorageResult::ok()
                                  : StorageResult::failure(message(QT_TRANSLATE_NOOP(
                                        "StorageDirectoryChange",
                                        "Could not save the storage directory selection."))));
        return {false, error, restored.success ? QString() : restored.error};
    };
    for (const auto& dir : directories) {
        if (!QDir().mkpath(QDir(o.destination).filePath(dir)))
            return fail(message(QT_TRANSLATE_NOOP("StorageDirectoryChange",
                                                  "Could not create the storage directory.")));
    }
    QHash<QString, qint64> totals, completed;
    for (const auto& entry : entries)
        ++totals[category(entry.relative)];
    for (auto& entry : entries) {
        result = checkpoint(QStringLiteral("copy"));
        if (!result.success)
            return fail(result.error);
        const auto from = QDir(o.source).filePath(entry.relative);
        const auto to = QDir(o.destination).filePath(entry.relative);
        const auto group = category(entry.relative);
        report(StorageDirectoryProgress::Stage::Copying, group, completed[group], totals[group]);
        if (linkedPath(from) || linkedPath(to) || !QFile::copy(from, to))
            return fail(message(QT_TRANSLATE_NOOP("StorageDirectoryChange", "Could not copy %1."))
                            .arg(from));
        copied.append(entry.relative);
        report(StorageDirectoryProgress::Stage::Verifying, group, completed[group], totals[group]);
        entry.hash = digest(from);
        result = checkpoint(QStringLiteral("verify"));
        if (!result.success || entry.hash.isEmpty() || entry.hash != digest(to))
            return fail(result.success ? message(QT_TRANSLATE_NOOP("StorageDirectoryChange",
                                                                   "Verification failed for %1."))
                                             .arg(from)
                                       : result.error);
        report(StorageDirectoryProgress::Stage::Copying, group, ++completed[group], totals[group]);
    }
    for (const auto& path : o.preparedFiles) {
        if (!copied.contains(path))
            copied.append(path);
        const auto parent = QFileInfo(path).path();
        if (parent != u"." && !directories.contains(parent))
            directories.append(parent);
    }
    if (o.prepare) {
        result = o.prepare(o.destination);
        if (!result.success)
            return fail(result.error);
    }
    report(StorageDirectoryProgress::Stage::Switching);
    result = checkpoint(QStringLiteral("activate"));
    if (!result.success)
        return fail(result.error);
    if (o.activate) {
        result = o.activate(o.destination);
        if (!result.success)
            return fail(result.error);
    }
    result = checkpoint(QStringLiteral("persist"));
    if (!result.success)
        return fail(result.error);
    state.insert(QStringLiteral("directory"), o.destination);
    result = writeObject(bootstrap, state);
    if (!result.success)
        return fail(result.error);

    QStringList retained;
    if (o.migrate) {
        qint64 done = 0;
        for (const auto& entry : entries) {
            report(StorageDirectoryProgress::Stage::Cleaning, {}, done++, entries.size());
            const QString path = QDir(o.source).filePath(entry.relative);
            result = checkpoint(QStringLiteral("cleanup"));
            if (!result.success || linkedPath(path) || digest(path) != entry.hash ||
                !QFile::remove(path))
                retained.append(path);
        }
        for (auto it = directories.crbegin(); it != directories.crend(); ++it) {
            const auto path = QDir(o.source).filePath(*it);
            if (!linkedPath(path))
                QDir().rmdir(path);
        }
    }
    state.remove(QStringLiteral("transaction"));
    result = writeObject(bootstrap, state);
    QString warning;
    if (!retained.isEmpty())
        warning = message(QT_TRANSLATE_NOOP("StorageDirectoryChange",
                                            "The new storage directory is active, but some old "
                                            "files could not be removed: %1"))
                      .arg(retained.join(u'\n'));
    if (!result.success)
        warning += (warning.isEmpty() ? QString() : QStringLiteral("\n")) + result.error;
    return {true, {}, warning};
}
} // namespace snow_shot::storage
