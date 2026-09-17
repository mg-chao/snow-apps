#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace snow_shot::update {

// The release contract is implemented once, in the pure-Rust snow-updater
// crate, and reaches this side through the Rust FFI archive; these wrappers
// throw std::runtime_error with the catalog diagnostics on invalid input.
// Build metadata does not affect version ordering.
int compareVersions(const QString& first, const QString& second);
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
    // The exhaustive ZIP inventory stays inside the Rust verifier; the
    // application only consumes the package identity, size, and checksum.
    QVector<UpdateFile> files;
};

struct UpdateRelease {
    QString version;
    QVector<UpdatePackage> packages;
    QByteArray envelope;
    const UpdatePackage& updatePackage(const QString& variant) const;
};

// A single signed envelope avoids JSON canonicalization and
// detached-signature races. An empty trusted-keys override selects the
// compiled-in trust store.
UpdateRelease verifyRelease(const QByteArray& envelope, const QByteArray& trustedKeys = {});
void verifyFile(const QString& path, qint64 size, const QString& hash);

} // namespace snow_shot::update
