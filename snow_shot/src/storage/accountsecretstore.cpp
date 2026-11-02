#include "snow_shot/storage/accountsecretstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace snow_shot::storage {
AccountSecretStore::AccountSecretStore(QString identity, QString directory)
    : m_identity(QString::fromLatin1(
          QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex())),
      m_directory(directory.isEmpty()
                      ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                            QStringLiteral("/account-secrets")
                      : std::move(directory)) {}

#ifndef Q_OS_MACOS
QByteArray AccountSecretStore::read() const {
#ifdef Q_OS_WIN
    QFile file(QDir(m_directory).filePath(m_identity + QStringLiteral(".dat")));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024)
        return {};
    auto bytes = file.readAll();
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &output))
        return {};
    const QByteArray plain(reinterpret_cast<const char*>(output.pbData),
                           static_cast<qsizetype>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plain;
#else
    return {};
#endif
}

bool AccountSecretStore::write(const QByteArray& value) const {
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(value.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(value.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Snow Shot account", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        return false;
    const QByteArray encrypted(reinterpret_cast<const char*>(output.pbData),
                               static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    if (!QDir().mkpath(m_directory))
        return false;
    QSaveFile file(QDir(m_directory).filePath(m_identity + QStringLiteral(".dat")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return file.write(encrypted) == encrypted.size() && file.commit();
#else
    Q_UNUSED(value);
    return false;
#endif
}

bool AccountSecretStore::remove() const {
    const QString path = QDir(m_directory).filePath(m_identity + QStringLiteral(".dat"));
    return !QFile::exists(path) || QFile::remove(path);
}
#endif
} // namespace snow_shot::storage
