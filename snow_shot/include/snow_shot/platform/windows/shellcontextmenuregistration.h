#ifndef SNOW_SHOT_PLATFORM_WINDOWS_SHELLCONTEXTMENUREGISTRATION_H
#define SNOW_SHOT_PLATFORM_WINDOWS_SHELLCONTEXTMENUREGISTRATION_H

#include "snow_shot/platform/windows/shellcontextmenumode.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <functional>

namespace snow_shot::platform::windows {

struct ShellContextMenuRegistryValue {
    quint32 type = 0;
    QByteArray data;

    friend bool operator==(const ShellContextMenuRegistryValue&,
                           const ShellContextMenuRegistryValue&) = default;
};

struct ShellContextMenuRegistryKey {
    bool exists = false;
    QMap<QString, ShellContextMenuRegistryValue> values;
    QMap<QString, ShellContextMenuRegistryKey> children;

    friend bool operator==(const ShellContextMenuRegistryKey&,
                           const ShellContextMenuRegistryKey&) = default;
};

// All paths are relative to HKCU. Injection keeps tests away from the user's registrations.
struct ShellContextMenuRegistryAccess {
    std::function<bool(const QString&, QStringList*, QString*)> childKeys;
    std::function<bool(const QString&, ShellContextMenuRegistryKey*, QString*)> read;
    std::function<bool(const QString&, const ShellContextMenuRegistryKey&, QString*)> replace;
    std::function<void()> notifyChanged;
};

struct ShellContextMenuRegistrationSnapshot {
    bool valid = false;
    QMap<QString, ShellContextMenuRegistryKey> keys;
    QString error;
};

struct ShellContextMenuResult {
    bool success = false;
    QString error;
};

class ShellContextMenuRegistration final {
  public:
    struct Options {
        QString classesRoot;
        QString executablePath;
        QString productId;
        QString verbName;
        QStringList fileExtensions;
        ShellContextMenuRegistryAccess registry;
        QString packagedPreferencePath;
    };

    ShellContextMenuRegistration();
    explicit ShellContextMenuRegistration(Options options);

    [[nodiscard]] static bool isSupported();
    [[nodiscard]] static ShellContextMenuMode modeFromValue(const QString& value);
    [[nodiscard]] QString expectedCommand() const;
    [[nodiscard]] ShellContextMenuRegistrationSnapshot snapshot() const;
    [[nodiscard]] bool restore(const ShellContextMenuRegistrationSnapshot& previous,
                               QString* error = nullptr) const;
    [[nodiscard]] bool setMode(ShellContextMenuMode mode, QString* error = nullptr) const;
    [[nodiscard]] bool setMode(ShellContextMenuMode mode, const QString& accessKey,
                               QString* error = nullptr) const;
    [[nodiscard]] bool refreshLabels(QString* error = nullptr) const;
    [[nodiscard]] bool refreshLabels(const QString& accessKey, QString* error = nullptr) const;
    // Native changes and the preference commit form one transaction.
    [[nodiscard]] ShellContextMenuResult
    changeMode(ShellContextMenuMode mode, const std::function<bool()>& commitPreference) const;
    [[nodiscard]] ShellContextMenuResult
    changeMode(ShellContextMenuMode mode, const QString& accessKey,
               const std::function<bool()>& commitPreference) const;

  private:
    [[nodiscard]] ShellContextMenuResult
    changePackagedMode(ShellContextMenuMode mode, const QString& accessKey,
                       const std::function<bool()>& commitPreference) const;
    [[nodiscard]] bool ownedByProduct(const ShellContextMenuRegistryKey& key) const;
    [[nodiscard]] ShellContextMenuRegistryKey enabledKey(ShellContextMenuRegistryKey previous,
                                                         const QString& accessKey) const;
    [[nodiscard]] bool apply(ShellContextMenuMode mode, const QString& accessKey,
                             const ShellContextMenuRegistrationSnapshot& previous,
                             QString* error) const;
    QString m_associationsRoot;
    QString m_executable;
    QString m_productId;
    QString m_verbName;
    QStringList m_extensions;
    ShellContextMenuRegistryAccess m_registry;
    QString m_packagedPreferencePath;
};

} // namespace snow_shot::platform::windows

#endif // SNOW_SHOT_PLATFORM_WINDOWS_SHELLCONTEXTMENUREGISTRATION_H
