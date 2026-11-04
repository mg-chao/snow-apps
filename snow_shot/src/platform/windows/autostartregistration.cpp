#include "snow_shot/app/edition.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/platform/windows/autostartregistration.h"

#include <QCoreApplication>
#include <QDir>
#include <QScopeGuard>

#include <limits>
#include <string>
#include <future>
#include <optional>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <qt_windows.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#endif

namespace snow_shot::platform::windows {
namespace {
#if defined(Q_OS_WIN) || defined(_WIN32)
AutoStartRegistrationSnapshot storeStartupState(std::optional<bool> enabled = std::nullopt) {
    // Store-policy development executables have no registered StartupTask. Do not invoke
    // the package-only WinRT API using the policy's fallback storage identity.
    if (app::distribution::runtimePackageFamilyName().isEmpty()) {
        AutoStartRegistrationSnapshot result;
        result.error = QCoreApplication::translate("AutoStartRegistration",
                                                   "Windows startup settings are unavailable");
        return result;
    }
    return std::async(
               std::launch::async,
               [enabled] {
                   AutoStartRegistrationSnapshot result;
                   try {
                       winrt::init_apartment(winrt::apartment_type::multi_threaded);
                       const auto apartment = qScopeGuard([] { winrt::uninit_apartment(); });
                       // The static StartupTask methods cache an activation factory across
                       // apartments. Own this factory locally so it is released before this
                       // request's MTA is uninitialized, including failed GetAsync calls.
                       const auto factory = winrt::get_activation_factory<
                           winrt::Windows::ApplicationModel::IStartupTaskStatics>(
                           winrt::name_of<winrt::Windows::ApplicationModel::StartupTask>());
                       auto task = factory.GetAsync(L"SnowShotStartup").get();
                       if (!task) {
                           result.error = QCoreApplication::translate(
                               "AutoStartRegistration", "Windows startup settings are unavailable");
                           return result;
                       }
                       using State = winrt::Windows::ApplicationModel::StartupTaskState;
                       if (enabled &&
                           (task.State() == State::Disabled || task.State() == State::Enabled)) {
                           if (*enabled) {
                               static_cast<void>(task.RequestEnableAsync().get());
                           } else {
                               task.Disable();
                           }
                       }
                       switch (task.State()) {
                       case State::Disabled:
                           result.state = AutoStartState::Disabled;
                           break;
                       case State::Enabled:
                           result.state = AutoStartState::Enabled;
                           break;
                       case State::DisabledByUser:
                           result.state = AutoStartState::DisabledByUser;
                           break;
                       case State::DisabledByPolicy:
                           result.state = AutoStartState::DisabledByPolicy;
                           break;
                       case State::EnabledByPolicy:
                           result.state = AutoStartState::EnabledByPolicy;
                           break;
                       default:
                           break;
                       }
                       result.valid = result.state != AutoStartState::Unavailable;
                       result.exists = result.state == AutoStartState::Enabled ||
                                       result.state == AutoStartState::EnabledByPolicy;
                       if (!result.valid)
                           result.error = QCoreApplication::translate(
                               "AutoStartRegistration", "Windows startup settings are unavailable");
                   } catch (const winrt::hresult_error&) {
                       result.error = QCoreApplication::translate(
                           "AutoStartRegistration", "Windows startup settings are unavailable");
                   }
                   return result;
               })
        .get();
}

constexpr wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* startupValueName() {
    static const std::wstring value = app::edition::registryName().toStdWString();
    return value.c_str();
}

QString windowsErrorMessage(const QString& operation, LSTATUS status) {
    return QStringLiteral("%1 failed with Windows error %2").arg(operation).arg(status);
}

bool deleteRegistration(QString* error) {
    HKEY key = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key);
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

    const LSTATUS deleteStatus = RegDeleteValueW(key, startupValueName());
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
    const LSTATUS writeStatus = RegSetValueExW(key, startupValueName(), 0, static_cast<DWORD>(type),
                                               bytes, static_cast<DWORD>(data.size()));
    RegCloseKey(key);
    if (writeStatus == ERROR_SUCCESS) {
        return true;
    }
    if (error != nullptr) {
        *error =
            windowsErrorMessage(QStringLiteral("Writing the auto-start registration"), writeStatus);
    }
    return false;
}

QByteArray registryStringData(const QString& value) {
    const std::wstring nativeValue = value.toStdWString();
    const qsizetype byteCount = static_cast<qsizetype>((nativeValue.size() + 1) * sizeof(wchar_t));
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

QString AutoStartRegistration::hint() {
    if (!app::distribution::isStoreManaged())
        return {};
    const auto current = snapshot();
    if (current.state == AutoStartState::DisabledByUser)
        return QCoreApplication::translate("AutoStartRegistration",
                                           "Enable Snow Shot in Windows Settings > Apps > Startup");
    if (current.state == AutoStartState::DisabledByPolicy ||
        current.state == AutoStartState::EnabledByPolicy)
        return QCoreApplication::translate("AutoStartRegistration",
                                           "Startup is managed by your organization");
    return current.error;
}

QString AutoStartRegistration::expectedCommand() {
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --autostart").arg(executable);
}

AutoStartRegistrationSnapshot AutoStartRegistration::snapshot() {
    AutoStartRegistrationSnapshot result;
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (app::distribution::isStoreManaged())
        return storeStartupState();
    HKEY key = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key);
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        result.valid = true;
        return result;
    }
    if (openStatus != ERROR_SUCCESS) {
        result.error =
            windowsErrorMessage(QStringLiteral("Opening the auto-start registry key"), openStatus);
        return result;
    }

    DWORD type = 0;
    DWORD byteCount = 0;
    LSTATUS queryStatus =
        RegQueryValueExW(key, startupValueName(), nullptr, &type, nullptr, &byteCount);
    if (queryStatus == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        result.valid = true;
        return result;
    }
    if (queryStatus != ERROR_SUCCESS) {
        RegCloseKey(key);
        result.error =
            windowsErrorMessage(QStringLiteral("Reading the auto-start registration"), queryStatus);
        return result;
    }

    result.nativeData.resize(static_cast<qsizetype>(byteCount));
    queryStatus = RegQueryValueExW(key, startupValueName(), nullptr, &type,
                                   reinterpret_cast<BYTE*>(result.nativeData.data()), &byteCount);
    RegCloseKey(key);
    if (queryStatus != ERROR_SUCCESS) {
        result.nativeData.clear();
        result.error =
            windowsErrorMessage(QStringLiteral("Reading the auto-start registration"), queryStatus);
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
    if (app::distribution::isStoreManaged())
        return snapshot().exists;
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
    if (app::distribution::isStoreManaged()) {
        const auto current = storeStartupState(enabled);
        const bool success = current.valid && current.exists == enabled;
        if (!success && error) {
            *error = current.error.isEmpty() ? hint() : current.error;
            if (error->isEmpty())
                *error = QCoreApplication::translate("AutoStartRegistration",
                                                     "Windows startup settings are unavailable");
        }
        return success;
    }
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

bool AutoStartRegistration::restore(const AutoStartRegistrationSnapshot& previous, QString* error) {
    if (!previous.valid) {
        if (error != nullptr) {
            *error = QStringLiteral("The previous auto-start registration could not be read");
        }
        return false;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (app::distribution::isStoreManaged())
        return setEnabled(previous.exists, error);
    return previous.exists ? writeRegistration(previous.nativeType, previous.nativeData, error)
                           : deleteRegistration(error);
#else
    if (error != nullptr) {
        *error = QStringLiteral("Auto-start registration is only supported on Windows");
    }
    return false;
#endif
}

} // namespace snow_shot::platform::windows
