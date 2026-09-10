#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace snow_shot::update {

// Throws std::runtime_error on invalid input. Build metadata does not affect ordering.
int compareVersions(const QString& first, const QString& second);
bool safeRelativePath(const QString& path);
QByteArray readLimited(const QString& path, qint64 limit = 8 * 1024 * 1024);
void writeAtomic(const QString& path, const QByteArray& bytes);
QString sha256File(const QString& path);
void requireUpdate(bool condition, const char* message);

struct UpdateFile {
    QString path;
    qint64 size = 0;
    QString sha256;
};

struct UpdatePackage {
    QString variant;
    QString kind;
    QString path;
    qint64 size = 0;
    QString sha256;
    QVector<UpdateFile> files;
};

struct UpdateRelease {
    QString version;
    QVector<UpdatePackage> packages;
    QByteArray envelope;
    const UpdatePackage& updatePackage(const QString& variant) const;
};

// A single signed envelope avoids JSON canonicalization and detached-signature races.
UpdateRelease verifyRelease(const QByteArray& envelope, const QByteArray& trustedKeys = {});
QVector<UpdateFile> parseFileInventory(const QJsonArray& array);
void verifyFile(const QString& path, qint64 size, const QString& hash);
QByteArray compiledTrustedKeys();

} // namespace snow_shot::update
