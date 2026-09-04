#ifndef SNOW_SHOT_STORAGE_STORAGEDIRECTORYUTILS_P_H
#define SNOW_SHOT_STORAGE_STORAGEDIRECTORYUTILS_P_H

#include <QDir>
#include <QFileInfo>

namespace snow_shot::storage {

// Recursive on-disk size of a file or directory in bytes.  Symlinks are
// skipped so relocated stores are not double-counted.
inline qint64 directoryBytes(const QString& path) {
    const QFileInfo root(path);
    if (!root.exists()) {
        return 0;
    }
    if (root.isFile() && !root.isSymLink()) {
        return root.size();
    }
    qint64 total = 0;
    const QFileInfoList entries = QDir(path).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink()) {
            continue;
        }
        total += entry.isDir() ? directoryBytes(entry.absoluteFilePath()) : entry.size();
    }
    return total;
}

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_STORAGEDIRECTORYUTILS_P_H
