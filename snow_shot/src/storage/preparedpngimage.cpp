#include "snow_shot/storage/preparedpngimage.h"

#include <QBuffer>
#include <QImageReader>

#include <utility>

namespace snow_shot::storage {
namespace {
bool hasExpectedPngHeader(const QByteArray& bytes, const QSize& expectedSize) {
    if (bytes.isEmpty() || !expectedSize.isValid() || expectedSize.isEmpty()) {
        return false;
    }
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return false;
    }
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    return reader.canRead() && reader.format().compare("png", Qt::CaseInsensitive) == 0 &&
           reader.size() == expectedSize;
}
} // namespace

PreparedPngImage::PreparedPngImage(QSize pixelSize, std::shared_ptr<const QByteArray> bytes)
    : m_pixelSize(pixelSize), m_bytes(std::move(bytes)), m_valid(true) {}

std::optional<PreparedPngImage>
PreparedPngImage::fromBytes(QSize pixelSize, std::shared_ptr<const QByteArray> bytes) {
    if (bytes == nullptr || !hasExpectedPngHeader(*bytes, pixelSize)) {
        return std::nullopt;
    }
    return PreparedPngImage(pixelSize, std::move(bytes));
}

std::optional<PreparedPngImage> PreparedPngImage::fromBytes(QSize pixelSize, QByteArray bytes) {
    return fromBytes(pixelSize, std::make_shared<const QByteArray>(std::move(bytes)));
}

bool PreparedPngImage::isValid() const {
    return m_valid;
}

QSize PreparedPngImage::pixelSize() const {
    return m_pixelSize;
}

const QByteArray& PreparedPngImage::bytes() const {
    static const QByteArray empty;
    return m_bytes != nullptr ? *m_bytes : empty;
}

const std::shared_ptr<const QByteArray>& PreparedPngImage::sharedBytes() const {
    return m_bytes;
}

} // namespace snow_shot::storage
