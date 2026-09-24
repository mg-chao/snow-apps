#include "snow_shot/presentation/components/thumbnailcache.h"

#include "snowimageqtcodec.h"
#include "snow_shot/storage/storageusagetracker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSaveFile>

#include <mutex>
#include <utility>

namespace snow_shot::presentation::components::thumbnail_cache {
namespace {
constexpr qint64 kMaximumBytes = 256LL * 1024LL * 1024LL;

struct Capacity final {
    std::mutex mutex;
    qint64 bytes = -1;
};

Capacity& capacity() {
    static Capacity shared;
    return shared;
}

qint64 cacheBytes(const QDir& directory) {
    qint64 total = 0;
    for (const auto& entry : directory.entryInfoList({QStringLiteral("*.png")}, QDir::Files))
        total += entry.size();
    return total;
}

void maintainCapacity(const QString& directoryPath, qint64 writtenBytes) {
    const QDir directory(directoryPath);
    Capacity& state = capacity();
    std::lock_guard lock(state.mutex);
    if (state.bytes < 0)
        state.bytes = cacheBytes(directory);
    else
        state.bytes += writtenBytes;
    if (state.bytes <= kMaximumBytes)
        return;
    state.bytes = cacheBytes(directory);
    if (state.bytes <= kMaximumBytes)
        return;
    const auto entries = directory.entryInfoList({QStringLiteral("*.png")}, QDir::Files,
                                                 QDir::Time | QDir::Reversed);
    for (const auto& entry : entries) {
        if (state.bytes <= kMaximumBytes)
            break;
        if (QFile::remove(entry.absoluteFilePath())) {
            QFile::remove(entry.absoluteFilePath() + QStringLiteral(".size"));
            state.bytes -= entry.size();
        }
    }
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
} // namespace

QString pathForKey(const QString& key, const QString& directory) {
    const QString cacheDirectory =
        directory.isEmpty() ? storage::StorageUsageTracker::defaultThumbnailCacheDirectory()
                            : directory;
    if (cacheDirectory.isEmpty() || key.isEmpty())
        return {};
    const QByteArray digest =
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(cacheDirectory).filePath(QString::fromLatin1(digest) + QStringLiteral(".png"));
}

CachedThumbnail load(const QString& path) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile() || info.isSymLink())
        return {};
    QImage image = image_codec::decodeFile(path, snow::image::Format::png);
    if (image.isNull()) {
        QFile::remove(path);
        QFile::remove(path + QStringLiteral(".size"));
        return {};
    }
    QSize naturalSize = image.size();
    QFile metadata(path + QStringLiteral(".size"));
    if (metadata.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> parts = metadata.readAll().trimmed().split('x');
        if (parts.size() == 2) {
            bool widthValid = false;
            bool heightValid = false;
            const int width = parts[0].toInt(&widthValid);
            const int height = parts[1].toInt(&heightValid);
            if (widthValid && heightValid && width > 0 && height > 0)
                naturalSize = QSize(width, height);
        }
    }
    return {std::move(image), naturalSize};
}

void persist(const QString& path, QImage image, QSize naturalSize) {
    if (path.isEmpty() || image.isNull() || !QDir().mkpath(QFileInfo(path).absolutePath()))
        return;
    const QByteArray png = image_codec::encodePng(image);
    image = {};
    if (png.isEmpty() || !writeBytes(path, png))
        return;
    if (naturalSize.isValid() && !naturalSize.isEmpty()) {
        const QByteArray size = QByteArray::number(naturalSize.width()) + 'x' +
                                QByteArray::number(naturalSize.height());
        static_cast<void>(writeBytes(path + QStringLiteral(".size"), size));
    }
    maintainCapacity(QFileInfo(path).absolutePath(), png.size());
}

} // namespace snow_shot::presentation::components::thumbnail_cache
