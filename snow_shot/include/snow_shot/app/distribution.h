#ifndef SNOW_SHOT_APP_DISTRIBUTION_H
#define SNOW_SHOT_APP_DISTRIBUTION_H

#include <QDir>
#include <QString>

#ifndef SNOW_SHOT_STORE_BUILD
#ifdef SNOW_SHOT_CONFIGURED_STORE_BUILD
#define SNOW_SHOT_STORE_BUILD SNOW_SHOT_CONFIGURED_STORE_BUILD
#else
#define SNOW_SHOT_STORE_BUILD 0
#endif
#endif
#ifndef SNOW_SHOT_PRIVACY_POLICY_URL
#define SNOW_SHOT_PRIVACY_POLICY_URL "https://snowshot.top/privacy.html"
#endif

namespace snow_shot::app::distribution {

struct Policy {
    bool managedByStore = false;
    QString packageFamily;
};

// Package identity is authoritative even when a standalone binary was accidentally packaged.
[[nodiscard]] inline Policy resolvePolicy(bool storeBuild, const QString& runtimeFamily) {
    return {storeBuild || !runtimeFamily.isEmpty(),
            runtimeFamily.isEmpty() && storeBuild
                ? QStringLiteral("SnowApps.SnowShot_3pp9w6ncw7bs0")
                : runtimeFamily};
}

[[nodiscard]] QString runtimePackageFamilyName();

[[nodiscard]] inline Policy policy() {
    return resolvePolicy(SNOW_SHOT_STORE_BUILD != 0, runtimePackageFamilyName());
}

[[nodiscard]] inline bool isStoreManaged() {
    return policy().managedByStore;
}

[[nodiscard]] inline QString packageFamilyName() {
    return policy().packageFamily;
}

[[nodiscard]] QString localApplicationDataDirectory();

[[nodiscard]] inline QString writableDataDirectory() {
    const auto current = policy();
    if (!current.managedByStore)
        return {};
    const auto local = localApplicationDataDirectory();
    if (local.isEmpty() || current.packageFamily.isEmpty())
        return {};
    return QDir(local).filePath(
        QStringLiteral("Packages/%1/LocalState").arg(current.packageFamily));
}

[[nodiscard]] inline QString installationIdentity() {
    const auto current = policy();
    return current.managedByStore ? QStringLiteral("store:%1").arg(current.packageFamily)
                                  : QString();
}

[[nodiscard]] inline QString mcpRuntimeDirectory() {
    const auto directory = writableDataDirectory();
    return directory.isEmpty() ? QString() : QDir(directory).filePath(QStringLiteral("mcp"));
}

[[nodiscard]] inline QString mcpCommand() {
    if (!isStoreManaged())
        return {};
    const auto local = localApplicationDataDirectory();
    return local.isEmpty() ? QString()
                           : QDir(local).filePath(
                                 QStringLiteral("Microsoft/WindowsApps/snow-shot-store-mcp.exe"));
}

[[nodiscard]] inline QString privacyPolicyUrl() {
    return QString::fromUtf8(SNOW_SHOT_PRIVACY_POLICY_URL);
}

[[nodiscard]] inline QString userAgreementUrl() {
    return QStringLiteral("https://snowshot.top/terms.html");
}

} // namespace snow_shot::app::distribution

#endif // SNOW_SHOT_APP_DISTRIBUTION_H
