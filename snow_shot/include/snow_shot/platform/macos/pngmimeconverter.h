#ifndef SNOW_SHOT_PLATFORM_MACOS_PNGMIMECONVERTER_H
#define SNOW_SHOT_PLATFORM_MACOS_PNGMIMECONVERTER_H

#include <QByteArray>
#include <QString>
#include <QUtiMimeConverter>
#include <QVariant>

namespace snow_shot::platform::macos {

class PngMimeConverter final : public QUtiMimeConverter {
  public:
    [[nodiscard]] QString utiForMime(const QString& mime) const override;
    [[nodiscard]] QString mimeForUti(const QString& uti) const override;
    [[nodiscard]] QList<QByteArray> convertFromMime(const QString& mime, const QVariant& data,
                                                    const QString& uti) const override;
    [[nodiscard]] QVariant convertToMime(const QString& mime, const QList<QByteArray>& data,
                                         const QString& uti) const override;
};

// Must be called on the GUI thread after QGuiApplication construction.
// Qt owns the registered converter and destroys it during application shutdown.
[[nodiscard]] bool ensurePngMimeConverter();

} // namespace snow_shot::platform::macos

#endif // SNOW_SHOT_PLATFORM_MACOS_PNGMIMECONVERTER_H
