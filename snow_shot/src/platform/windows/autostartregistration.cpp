#include "snow_shot/platform/windows/autostartregistration.h"

#include <QCoreApplication>
#include <QDir>

#include <limits>
#include <string>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <qt_windows.h>
#endif

namespace snow_shot::platform::windows {
namespace {
#if defined(Q_OS_WIN) || defined(_WIN32)
constexpr wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t VALUE_NAME[] = L"SnowShot";

QString windowsErrorMessage(const QString& operation, LSTATUS status) {
    return QStringLiteral("%1 failed with Windows error %2").arg(operation).arg(status);
}

bool deleteRegistration(QString* error) {
    HKEY key = nullptr;
    const LSTATUS openStatus =
        RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key);
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openStatus != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = windowsErrorMessage(QStringLiteral("Opening the auto-start registry key"),
                                         openStatus);
        }
        return false;
    }

    const LSTATUS deleteStatus = RegDeleteValueW(key, VALUE_NAME);
    RegCloseKey(key);
    if (deleteStatus == ERROR_SUCCESS || deleteStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (error != nullptr) {
        *error = windowsErrorMessage(QStringLiteral("Removing the auto-start registration"),
                                     deleteStatus);
    }
    return false;
}

bool writeRegistration(quint32 type, const QByteArray& data, QString* error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS createStatus =
        RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, &disposition);
    Q_UNUSED(disposition)
    if (createStatus != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = windowsErrorMessage(QStringLiteral("Opening the auto-start registry key"),
                                         createStatus);
        }
        return false;
    }

    if (data.size() > static_cast<qsizetype>(std::numeric_limits<DWORD>::max())) {
        RegCloseKey(key);
        if (error != nullptr) {
            *error = QStringLiteral("The auto-start registry value is too large");
        }
        return false;
    }
    const auto* bytes = reinterpret_cast<const BYTE*>(data.constData());
    const LSTATUS writeStatus =
        RegSetValueExW(key, VALUE_NAME, 0, static_cast<DWORD>(type), bytes,
                       static_cast<DWORD>(data.size()));
    RegCloseKey(key);
    if (writeStatus == ERROR_SUCCESS) {
        return true;
    }
    if (error != nullptr) {
        *error = windowsErrorMessage(QStringLiteral("Writing the auto-start registration"),
                                     writeStatus);
    }
    return false;
}

QByteArray registryStringData(const QString& value) {
    const std::wstring nativeValue = value.toStdWString();
    const qsizetype byteCount =
        static_cast<qsizetype>((nativeValue.size() + 1) * sizeof(wchar_t));
    return QByteArray(reinterpret_cast<const char*>(nativeValue.c_str()), byteCount);
}
#endif
} // namespace

bool AutoStartRegistration::isSupported() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

QString AutoStartRegistration::expectedCommand() {
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --autostart").arg(executable);
}

AutoStartRegistrationSnapshot AutoStartRegistration::snapshot() {
    AutoStartRegistrationSnapshot result;
#if defined(Q_OS_WIN) || defined(_WIN32)
    HKEY key = nullptr;
    const LSTATUS openStatus =
        RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key);
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        result.valid = true;
        return result;
    }
    if (openStatus != ERROR_SUCCESS) {
        result.error = windowsErrorMessage(QStringLiteral("Opening the auto-start registry key"),
                                           openStatus);
        return result;
    }

    DWORD type = 0;
    DWORD byteCount = 0;
    LSTATUS queryStatus =
        RegQueryValueExW(key, VALUE_NAME, nullptr, &type, nullptr, &byteCount);
    if (queryStatus == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        result.valid = true;
        return result;
    }
    if (queryStatus != ERROR_SUCCESS) {
        RegCloseKey(key);
        result.error = windowsErrorMessage(QStringLiteral("Reading the auto-start registration"),
                                           queryStatus);
        return result;
    }

    result.nativeData.resize(static_cast<qsizetype>(byteCount));
    queryStatus = RegQueryValueExW(
        key, VALUE_NAME, nullptr, &type,
        reinterpret_cast<BYTE*>(result.nativeData.data()), &byteCount);
    RegCloseKey(key);
    if (queryStatus != ERROR_SUCCESS) {
        result.nativeData.clear();
        result.error = windowsErrorMessage(QStringLiteral("Reading the auto-start registration"),
                                           queryStatus);
        return result;
    }
    result.nativeData.resize(static_cast<qsizetype>(byteCount));
    result.valid = true;
    result.exists = true;
    result.nativeType = type;
#else
    result.error = QStringLiteral("Auto-start registration is only supported on Windows");
#endif
    return result;
}

bool AutoStartRegistration::matchesExpectedCommand() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const AutoStartRegistrationSnapshot current = snapshot();
    if (!current.valid || !current.exists ||
        (current.nativeType != REG_SZ && current.nativeType != REG_EXPAND_SZ) ||
        current.nativeData.size() < static_cast<qsizetype>(sizeof(wchar_t))) {
        return false;
    }
    const auto* value = reinterpret_cast<const wchar_t*>(current.nativeData.constData());
    const qsizetype characterCapacity =
        current.nativeData.size() / static_cast<qsizetype>(sizeof(wchar_t));
    qsizetype length = 0;
    while (length < characterCapacity && value[length] != L'\0') {
        ++length;
    }
    return QString::fromWCharArray(value, length) == expectedCommand();
#else
    return false;
#endif
}

bool AutoStartRegistration::setEnabled(bool enabled, QString* error) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!enabled) {
        return deleteRegistration(error);
    }
    return writeRegistration(REG_SZ, registryStringData(expectedCommand()), error);
#else
    if (error != nullptr) {
        *error = QStringLiteral("Auto-start registration is only supported on Windows");
    }
    Q_UNUSED(enabled)
    return false;
#endif
}

bool AutoStartRegistration::restore(const AutoStartRegistrationSnapshot& previous,
                                    QString* error) {
    if (!previous.valid) {
        if (error != nullptr) {
            *error = QStringLiteral("The previous auto-start registration could not be read");
        }
        return false;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    return previous.exists
               ? writeRegistration(previous.nativeType, previous.nativeData, error)
               : deleteRegistration(error);
#else
    if (error != nullptr) {
        *error = QStringLiteral("Auto-start registration is only supported on Windows");
    }
    return false;
#endif
}

} // namespace snow_shot::platform::windows
