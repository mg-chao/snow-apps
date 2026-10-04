#pragma once

#include <QBuffer>
#include <QImage>

namespace image_orientation_fixture {

inline QByteArray jpegWithExifOrientation(const QImage& image, quint8 orientation) {
    if (orientation < 1 || orientation > 8)
        return {};
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", 100))
        return {};
    // A minimal little-endian TIFF IFD containing just EXIF Orientation. Inserting APP1 after
    // JPEG's SOI preserves the encoded raster while giving the reader transformation metadata.
    QByteArray app1 = QByteArray::fromHex(
        "ffe1002245786966000049492a0008000000010012010300010000000000000000000000");
    app1[28] = static_cast<char>(orientation);
    encoded.insert(2, app1);
    return encoded;
}

} // namespace image_orientation_fixture
