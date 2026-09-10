#include "snow_shot/update/updatecontract.h"
#include "updatekeys.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <bcrypt.h>
#include <io.h>
#endif

namespace snow_shot::update {
namespace {
QStringList versionParts(const QString& version) {
    static const QRegularExpression expression(
        QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
                       "(?:-((?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
                       "(?:\\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*))?"
                       "(?:\\+([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?$"));
    const auto match = expression.match(version);
    requireUpdate(version.size() <= 128 && match.hasMatch(), "Invalid semantic version");
    return {match.captured(1), match.captured(2), match.captured(3), match.captured(4)};
}

int numericCompare(const QString& a, const QString& b) {
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    return QString::compare(a, b, Qt::CaseSensitive);
}

bool isNumeric(const QString& value) {
    return !value.isEmpty() &&
           std::all_of(value.begin(), value.end(), [](QChar c) { return c >= u'0' && c <= u'9'; });
}

qint64 positiveSize(const QJsonValue& value, bool allowZero = false) {
    const double number = value.toDouble(-1);
    requireUpdate(number >= (allowZero ? 0 : 1) && number <= 4.0 * 1024 * 1024 * 1024 &&
                      number == static_cast<double>(static_cast<qint64>(number)),
                  "Invalid update file size");
    return static_cast<qint64>(number);
}

QString hashValue(const QJsonValue& value) {
    const QString hash = value.toString();
    static const QRegularExpression expression(QStringLiteral("^[a-f0-9]{64}$"));
    requireUpdate(expression.match(hash).hasMatch(), "Invalid update checksum");
    return hash;
}

QByteArray decodeBase64(const QJsonValue& value) {
    const auto decoded = QByteArray::fromBase64Encoding(value.toString().toLatin1(),
                                                        QByteArray::AbortOnBase64DecodingErrors);
    requireUpdate(static_cast<bool>(decoded) && !decoded.decoded.isEmpty(),
                  "Invalid update signature encoding");
    return decoded.decoded;
}

bool verifySignature(const QByteArray& payload, const QByteArray& signature,
                     const QJsonObject& key) {
#ifdef Q_OS_WIN
    const QByteArray modulus = decodeBase64(key.value(QStringLiteral("modulus")));
    const QByteArray exponent = decodeBase64(key.value(QStringLiteral("exponent")));
    requireUpdate(modulus.size() == 384 && exponent.size() >= 1 && exponent.size() <= 8,
                  "Invalid release public key");
    BCRYPT_RSAKEY_BLOB header{BCRYPT_RSAPUBLIC_MAGIC,
                              3072,
                              static_cast<ULONG>(exponent.size()),
                              static_cast<ULONG>(modulus.size()),
                              0,
                              0};
    QByteArray blob(reinterpret_cast<const char*>(&header), sizeof(header));
    blob += exponent;
    blob += modulus;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool valid = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0) {
        if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &handle,
                                reinterpret_cast<PUCHAR>(blob.data()),
                                static_cast<ULONG>(blob.size()), 0) >= 0) {
            QByteArray digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
            BCRYPT_PSS_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, 32};
            valid = BCryptVerifySignature(
                        handle, &padding, reinterpret_cast<PUCHAR>(digest.data()),
                        static_cast<ULONG>(digest.size()),
                        reinterpret_cast<PUCHAR>(const_cast<char*>(signature.constData())),
                        static_cast<ULONG>(signature.size()), BCRYPT_PAD_PSS) >= 0;
            BCryptDestroyKey(handle);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return valid;
#else
    Q_UNUSED(payload);
    Q_UNUSED(signature);
    Q_UNUSED(key);
    return false;
#endif
}
} // namespace

void requireUpdate(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(
            QCoreApplication::translate("UpdateErrors", message).toStdString());
    }
}

int compareVersions(const QString& first, const QString& second) {
    const auto a = versionParts(first);
    const auto b = versionParts(second);
    for (int i = 0; i < 3; ++i) {
        const int result = numericCompare(a[i], b[i]);
        if (result != 0) {
            return result;
        }
    }
    if (a[3] == b[3]) {
        return 0;
    }
    if (a[3].isEmpty() || b[3].isEmpty()) {
        return a[3].isEmpty() ? 1 : -1;
    }
    const auto ap = a[3].split(u'.');
    const auto bp = b[3].split(u'.');
    for (qsizetype i = 0; i < std::min(ap.size(), bp.size()); ++i) {
        const bool an = isNumeric(ap[i]);
        const bool bn = isNumeric(bp[i]);
        const int result = an && bn   ? numericCompare(ap[i], bp[i])
                           : an != bn ? (an ? -1 : 1)
                                      : QString::compare(ap[i], bp[i], Qt::CaseSensitive);
        if (result != 0) {
            return result;
        }
    }
    return ap.size() < bp.size() ? -1 : 1;
}

bool safeRelativePath(const QString& path) {
    if (path.isEmpty() || path.size() > 220 || path.contains(u'\\') || path.contains(u':') ||
        path.startsWith(u'/')) {
        return false;
    }
    static const QRegularExpression reserved(
        QStringLiteral(
            "^(?:CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|CLOCK\\$|COM[0-9¹²³]|LPT[0-9¹²³])(?:\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    for (const auto& part : path.split(u'/')) {
        if (part.isEmpty() || part == u"." || part == u".." || part.endsWith(u'.') ||
            part.endsWith(u' ') || reserved.match(part).hasMatch()) {
            return false;
        }
        for (QChar c : part) {
            if (c.unicode() < 32 || QStringLiteral("<>\"|?*").contains(c)) {
                return false;
            }
        }
    }
    return true;
}

QByteArray readLimited(const QString& path, qint64 limit) {
    QFile file(path);
    requireUpdate(file.open(QIODevice::ReadOnly) && file.size() <= limit,
                  "Update file could not be read or exceeds its size limit");
    const QByteArray bytes = file.readAll();
    requireUpdate(file.error() == QFileDevice::NoError, "Could not read update file");
    return bytes;
}

void writeAtomic(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    requireUpdate(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
                      file.flush(),
                  "Could not save update state");
#ifdef Q_OS_WIN
    requireUpdate(FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()))) != 0,
                  "Could not persist update state");
#endif
    requireUpdate(file.commit(), "Could not commit update state");
}

QString sha256File(const QString& path) {
    QFile file(path);
    requireUpdate(file.open(QIODevice::ReadOnly), "Could not read update payload");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    requireUpdate(hash.addData(&file), "Could not hash update payload");
    return QString::fromLatin1(hash.result().toHex());
}

void verifyFile(const QString& path, qint64 size, const QString& hash) {
    requireUpdate(QFile(path).size() == size && sha256File(path) == hash,
                  "Update payload size or checksum does not match the signed release");
}

QByteArray compiledTrustedKeys() {
    return QByteArray(snowShotUpdateKeys);
}

QVector<UpdateFile> parseFileInventory(const QJsonArray& array) {
    requireUpdate(!array.isEmpty() && array.size() <= 20000, "Invalid update file inventory");
    QVector<UpdateFile> files;
    QSet<QString> seen;
    qint64 total = 0;
    for (const auto& value : array) {
        const auto object = value.toObject();
        UpdateFile file{object.value(QStringLiteral("path")).toString(),
                        positiveSize(object.value(QStringLiteral("size")), true),
                        hashValue(object.value(QStringLiteral("sha256")))};
        const QString lower = file.path.toLower();
        requireUpdate(safeRelativePath(file.path) && !seen.contains(lower) &&
                          (file.path.startsWith(u"bin/") ||
                           file.path.startsWith(u"share/snow-shot/") ||
                           file.path == u"snow-shot-installation.json") &&
                          !lower.startsWith(u"bin/portable/"),
                      "Unsafe update file inventory");
        seen.insert(lower);
        total += file.size;
        requireUpdate(total <= 4LL * 1024 * 1024 * 1024, "Update payload is too large");
        files.append(file);
    }
    for (const auto& file : files) {
        QString parent = file.path.toLower();
        while (parent.contains(u'/')) {
            parent.truncate(parent.lastIndexOf(u'/'));
            requireUpdate(!seen.contains(parent), "Conflicting update file paths");
        }
    }
    return files;
}

UpdateRelease verifyRelease(const QByteArray& envelope, const QByteArray& trustedKeys) {
    requireUpdate(envelope.size() <= 8 * 1024 * 1024, "Update metadata is too large");
    const auto outer = QJsonDocument::fromJson(envelope).object();
    requireUpdate(outer.value(QStringLiteral("schema")).toInt() == 1,
                  "Unsupported update signature schema");
    const QByteArray payload = decodeBase64(outer.value(QStringLiteral("payload")));
    const QByteArray signature = decodeBase64(outer.value(QStringLiteral("signature")));
    const auto keys =
        QJsonDocument::fromJson(trustedKeys.isEmpty() ? compiledTrustedKeys() : trustedKeys)
            .object()
            .value(QStringLiteral("keys"))
            .toArray();
    bool verified = false;
    for (const auto& value : keys) {
        const auto key = value.toObject();
        if (key.value(QStringLiteral("id")) == outer.value(QStringLiteral("keyId"))) {
            verified = verifySignature(payload, signature, key);
            break;
        }
    }
    requireUpdate(verified, "Release signature is invalid or its signing key is not trusted");
    const auto object = QJsonDocument::fromJson(payload).object();
    requireUpdate(object.value(QStringLiteral("schema")).toInt() == 1 &&
                      object.value(QStringLiteral("platform")).toString() == u"windows-x64" &&
                      QDateTime::fromString(object.value(QStringLiteral("publishedAt")).toString(),
                                            Qt::ISODate)
                          .isValid(),
                  "Unsupported update release");
    UpdateRelease release;
    release.version = object.value(QStringLiteral("version")).toString();
    compareVersions(release.version, release.version);
    release.envelope = envelope;
    QSet<QString> paths;
    QSet<QString> identities;
    const auto packages = object.value(QStringLiteral("packages")).toArray();
    requireUpdate(packages.size() == 5, "The release must contain all five Windows packages");
    for (const auto& value : packages) {
        const auto package = value.toObject();
        UpdatePackage parsed;
        parsed.variant = package.value(QStringLiteral("variant")).toString();
        parsed.kind = package.value(QStringLiteral("kind")).toString();
        parsed.path = package.value(QStringLiteral("path")).toString();
        parsed.size = positiveSize(package.value(QStringLiteral("size")));
        parsed.sha256 = hashValue(package.value(QStringLiteral("sha256")));
        const bool portable = parsed.variant == u"portable" && parsed.kind == u"portable";
        requireUpdate(portable || ((parsed.variant == u"online" || parsed.variant == u"offline") &&
                                   (parsed.kind == u"installer" || parsed.kind == u"update")),
                      "Unknown update package variant");
        const QString expected = QStringLiteral("setup/snow-shot_windows-x64-") + parsed.variant +
                                 (portable                      ? QStringLiteral(".zip")
                                  : parsed.kind == u"installer" ? QStringLiteral(".exe")
                                                                : QStringLiteral("-update.zip"));
        requireUpdate(parsed.path == expected && !paths.contains(parsed.path) &&
                          !identities.contains(parsed.variant + u'/' + parsed.kind),
                      "Unexpected update package URL");
        paths.insert(parsed.path);
        identities.insert(parsed.variant + u'/' + parsed.kind);
        if (parsed.kind != u"installer") {
            parsed.files = parseFileInventory(package.value(QStringLiteral("files")).toArray());
            for (const auto* required : {"bin/snow_shot.exe", "bin/snow-shot-updater.exe",
                                         "snow-shot-installation.json"}) {
                requireUpdate(std::any_of(parsed.files.begin(), parsed.files.end(),
                                          [required](const auto& f) {
                                              return f.path == QString::fromLatin1(required);
                                          }),
                              "Incomplete update archive");
            }
        }
        release.packages.append(parsed);
    }
    return release;
}

const UpdatePackage& UpdateRelease::updatePackage(const QString& variant) const {
    for (const auto& package : packages) {
        if (package.variant == variant && package.kind != u"installer") {
            return package;
        }
    }
    throw std::runtime_error("No update package matches this installation");
}
} // namespace snow_shot::update
