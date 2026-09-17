#include "snow_shot/update/updatecontract.h"
#include "snow_shot/update/updateerrors.h"
#include "snow_shot/update/updatetransaction.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>
#include <QStringView>

#include <array>
#include <cstddef>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <io.h>
#include <qt_windows.h>
#endif

// Thin C++ bindings over the Rust update contract (`snow-updater` crate,
// bundled through the Rust FFI archive). UTF-16 crosses the boundary so
// Windows paths keep their exact form.
extern "C" {
typedef struct SnowUpdaterPackage {
    unsigned short variant[24];
    unsigned short kind[16];
    unsigned short path[64];
    long long size;
    unsigned short sha256[65];
} SnowUpdaterPackage;

typedef struct SnowUpdaterRelease {
    unsigned short version[129];
    SnowUpdaterPackage packages[5];
} SnowUpdaterRelease;

typedef struct SnowUpdaterInstallation {
    unsigned short variant[24];
    unsigned short version[129];
} SnowUpdaterInstallation;

int snow_updater_verify_release(const unsigned short* envelope, std::size_t envelope_length,
                                const unsigned short* trusted_keys, std::size_t keys_length,
                                SnowUpdaterRelease* out, char* error_utf8,
                                std::size_t error_capacity);
int snow_updater_compare_versions(const unsigned short* first, std::size_t first_length,
                                  const unsigned short* second, std::size_t second_length,
                                  int* result, char* error_utf8, std::size_t error_capacity);
int snow_updater_verify_file(const unsigned short* path, std::size_t path_length,
                             long long expected_size, const unsigned short* sha256,
                             std::size_t sha256_length, char* error_utf8,
                             std::size_t error_capacity);
int snow_updater_installation_record(const unsigned short* root, std::size_t root_length,
                                     SnowUpdaterInstallation* out, char* error_utf8,
                                     std::size_t error_capacity);
int snow_updater_installation_root(const unsigned short* executable_directory,
                                   std::size_t directory_length, unsigned short* out,
                                   std::size_t out_capacity);
int snow_updater_transaction_pending(const unsigned short* root, std::size_t root_length);
const char* snow_updater_error_message(unsigned int code);
unsigned int snow_updater_error_count(void);
}

namespace {
constexpr std::size_t kErrorCapacity = 512;

QList<char16_t> wideText(const QString& text) {
    QList<char16_t> units;
    units.reserve(text.size());
    for (const QChar& character : text) {
        units.append(character.unicode());
    }
    return units;
}

[[noreturn]] void throwFfiError(char* buffer) {
    throw std::runtime_error(QString::fromUtf8(buffer).toStdString());
}

QString fromWideField(const unsigned short* field, std::size_t capacity) {
    std::size_t length = 0;
    while (length < capacity && field[length] != 0) {
        ++length;
    }
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(field),
                              static_cast<qsizetype>(length));
}
} // namespace

namespace snow_shot::update {

void requireUpdate(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(
            QCoreApplication::translate("UpdateErrors", message).toStdString());
    }
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

int compareVersions(const QString& first, const QString& second) {
    char error[kErrorCapacity] = {};
    const auto left = wideText(first);
    const auto right = wideText(second);
    int result = 0;
    if (snow_updater_compare_versions(reinterpret_cast<const unsigned short*>(left.constData()),
                                      static_cast<std::size_t>(left.size()),
                                      reinterpret_cast<const unsigned short*>(right.constData()),
                                      static_cast<std::size_t>(right.size()), &result, error,
                                      kErrorCapacity) != 0) {
        throwFfiError(error);
    }
    return result < 0 ? -1 : (result > 0 ? 1 : 0);
}

void verifyFile(const QString& path, qint64 size, const QString& hash) {
    const auto widePath = wideText(path);
    const auto wideHash = wideText(hash);
    char error[kErrorCapacity] = {};
    if (snow_updater_verify_file(reinterpret_cast<const unsigned short*>(widePath.constData()),
                                 static_cast<std::size_t>(widePath.size()), size,
                                 reinterpret_cast<const unsigned short*>(wideHash.constData()),
                                 static_cast<std::size_t>(wideHash.size()), error,
                                 kErrorCapacity) != 0) {
        throwFfiError(error);
    }
}

UpdateRelease verifyRelease(const QByteArray& envelope, const QByteArray& trustedKeys) {
    const auto envelopeWide = wideText(QString::fromUtf8(envelope));
    const auto keysWide = wideText(QString::fromUtf8(trustedKeys));
    SnowUpdaterRelease parsed{};
    char error[kErrorCapacity] = {};
    if (snow_updater_verify_release(
            reinterpret_cast<const unsigned short*>(envelopeWide.constData()),
            static_cast<std::size_t>(envelopeWide.size()),
            trustedKeys.isEmpty() ? nullptr
                                  : reinterpret_cast<const unsigned short*>(keysWide.constData()),
            trustedKeys.isEmpty() ? 0 : static_cast<std::size_t>(keysWide.size()), &parsed, error,
            kErrorCapacity) != 0) {
        throwFfiError(error);
    }
    UpdateRelease release;
    release.version = fromWideField(parsed.version, std::size(parsed.version));
    release.envelope = envelope;
    for (const auto& package : parsed.packages) {
        UpdatePackage converted;
        converted.variant = fromWideField(package.variant, std::size(package.variant));
        converted.kind = fromWideField(package.kind, std::size(package.kind));
        converted.path = fromWideField(package.path, std::size(package.path));
        converted.size = package.size;
        converted.sha256 = fromWideField(package.sha256, std::size(package.sha256));
        release.packages.append(converted);
    }
    return release;
}

const UpdatePackage& UpdateRelease::updatePackage(const QString& variant) const {
    for (const auto& package : packages) {
        if (package.variant == variant && package.kind != u"installer") {
            return package;
        }
    }
    throw std::runtime_error(
        QCoreApplication::translate("UpdateErrors", "No update package matches this installation")
            .toStdString());
}

QString installationRoot(const QString& executableDirectory) {
    const auto wide = wideText(executableDirectory);
    std::array<unsigned short, 32768> buffer{};
    const int length = snow_updater_installation_root(
        reinterpret_cast<const unsigned short*>(wide.constData()),
        static_cast<std::size_t>(wide.size()), buffer.data(), std::size(buffer));
    requireUpdate(length >= 0, "Invalid Snow Shot installation root");
    return fromWideField(buffer.data(), static_cast<std::size_t>(length));
}

InstallationIdentity installationRecord(const QString& root) {
    const auto wide = wideText(root);
    SnowUpdaterInstallation parsed{};
    char error[kErrorCapacity] = {};
    if (snow_updater_installation_record(reinterpret_cast<const unsigned short*>(wide.constData()),
                                         static_cast<std::size_t>(wide.size()), &parsed, error,
                                         kErrorCapacity) != 0) {
        throwFfiError(error);
    }
    return InstallationIdentity{
        fromWideField(parsed.variant, std::size(parsed.variant)),
        fromWideField(parsed.version, std::size(parsed.version)),
    };
}

bool transactionPending(const QString& root) {
    const auto wide = wideText(root);
    return snow_updater_transaction_pending(
               reinterpret_cast<const unsigned short*>(wide.constData()),
               static_cast<std::size_t>(wide.size())) == 0;
}

unsigned int updaterErrorDiagnosticCount() {
    return snow_updater_error_count();
}

const char* updaterErrorDiagnostic(unsigned int code) {
    return snow_updater_error_message(code);
}

} // namespace snow_shot::update
