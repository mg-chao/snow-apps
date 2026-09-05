#ifndef SNOW_SHOT_STORAGE_PREPAREDPNGIMAGE_H
#define SNOW_SHOT_STORAGE_PREPAREDPNGIMAGE_H

#include <QByteArray>
#include <QSize>

#include <memory>
#include <optional>

namespace snow_shot::storage {

class PreparedPngImage final {
  public:
    PreparedPngImage() = default;

    [[nodiscard]] static std::optional<PreparedPngImage>
    fromBytes(QSize pixelSize, std::shared_ptr<const QByteArray> bytes);
    [[nodiscard]] static std::optional<PreparedPngImage> fromBytes(QSize pixelSize,
                                                                   QByteArray bytes);

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QSize pixelSize() const;
    [[nodiscard]] const QByteArray& bytes() const;
    [[nodiscard]] const std::shared_ptr<const QByteArray>& sharedBytes() const;

  private:
    PreparedPngImage(QSize pixelSize, std::shared_ptr<const QByteArray> bytes);

    QSize m_pixelSize;
    std::shared_ptr<const QByteArray> m_bytes;
    bool m_valid = false;
};

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_PREPAREDPNGIMAGE_H
