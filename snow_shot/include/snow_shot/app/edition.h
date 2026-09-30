#ifndef SNOW_SHOT_APP_EDITION_H
#define SNOW_SHOT_APP_EDITION_H

#include <QCoreApplication>
#include <QString>

#ifndef SNOW_SHOT_EDITION_MINI
#define SNOW_SHOT_EDITION_MINI 0
#endif
#ifndef SNOW_SHOT_ENABLE_QR_RECOGNITION
#define SNOW_SHOT_ENABLE_QR_RECOGNITION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#define SNOW_SHOT_ENABLE_TABLE_RECOGNITION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_IMAGE_CONVERSION
#define SNOW_SHOT_ENABLE_IMAGE_CONVERSION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_LATEX_RECOGNITION
#define SNOW_SHOT_ENABLE_LATEX_RECOGNITION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#define SNOW_SHOT_ENABLE_TEXT_TRANSLATION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_API_CONFIGURATION
#define SNOW_SHOT_ENABLE_API_CONFIGURATION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_EXTENDED_FEATURES
#define SNOW_SHOT_ENABLE_EXTENDED_FEATURES (!SNOW_SHOT_EDITION_MINI)
#endif

namespace snow_shot::app::edition {
inline constexpr bool isMini = SNOW_SHOT_EDITION_MINI != 0;
inline constexpr bool qrRecognition = SNOW_SHOT_ENABLE_QR_RECOGNITION != 0;
inline constexpr bool tableRecognition = SNOW_SHOT_ENABLE_TABLE_RECOGNITION != 0;
inline constexpr bool imageConversion = SNOW_SHOT_ENABLE_IMAGE_CONVERSION != 0;
inline constexpr bool latexRecognition = SNOW_SHOT_ENABLE_LATEX_RECOGNITION != 0;
inline constexpr bool textTranslation = SNOW_SHOT_ENABLE_TEXT_TRANSLATION != 0;
inline constexpr bool apiConfiguration = SNOW_SHOT_ENABLE_API_CONFIGURATION != 0;
inline constexpr bool extendedFeatures = SNOW_SHOT_ENABLE_EXTENDED_FEATURES != 0;

[[nodiscard]] inline QString productName() {
    return isMini ? QCoreApplication::translate("EditionMetadata", "Snow Shot Mini")
                  : QCoreApplication::translate("EditionMetadata", "Snow Shot");
}
[[nodiscard]] inline QString applicationName() {
    return isMini ? QStringLiteral("snow_shot_mini") : QStringLiteral("snow_shot");
}
[[nodiscard]] inline QString registryName() {
    return isMini ? QStringLiteral("SnowShotMini") : QStringLiteral("SnowShot");
}
[[nodiscard]] inline QString executableName() {
#ifdef Q_OS_WIN
    return applicationName() + QStringLiteral(".exe");
#else
    return applicationName();
#endif
}
[[nodiscard]] inline QString productId() {
    return isMini ? QStringLiteral("snow-shot-mini") : QStringLiteral("snow-shot");
}
[[nodiscard]] inline QString portableMarkerName() {
    return isMini ? QStringLiteral("__mini_data_directory") : QStringLiteral("__data_directory");
}
[[nodiscard]] inline QString updaterName() {
#ifdef Q_OS_WIN
    return productId() + QStringLiteral("-updater.exe");
#else
    return productId() + QStringLiteral("-updater");
#endif
}
[[nodiscard]] inline QString mcpName() {
#ifdef Q_OS_WIN
    return productId() + QStringLiteral("-mcp.exe");
#else
    return productId() + QStringLiteral("-mcp");
#endif
}
[[nodiscard]] inline QString bundleId() {
    return QStringLiteral("com.snowshot.") + applicationName();
}
} // namespace snow_shot::app::edition

#endif // SNOW_SHOT_APP_EDITION_H
