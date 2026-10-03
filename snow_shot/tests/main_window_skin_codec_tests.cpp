#include "snowimageqtcodec.h"
#include "../../test-support/virtualmemory.h"

#include <QBuffer>
#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QImageWriter>
#include <QImageReader>
#include <QRgba64>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
using snow_shot::image_codec::decodeSkinFile;
using snow_shot::image_codec::SkinDecodeError;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QImage solid(const QColor& color, QSize size = QSize(4, 3)) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(color);
    return image;
}

QByteArray pngBytes(const QImage& image) {
    QByteArray bytes;
    QBuffer output(&bytes);
    require(output.open(QIODevice::WriteOnly), "open PNG fixture buffer");
    QImageWriter writer(&output, "png");
    require(writer.write(image), "encode PNG fixture with its source color profile");
    return bytes;
}

void managedQtReader() {
    for (const auto format :
         {QImage::Format_RGBA8888, QImage::Format_RGBA64, QImage::Format_Grayscale16}) {
        QImage source(QSize(1025, 513), format);
        source.fill(QColor::fromRgba64(QRgba64::fromRgba64(0x1234, 0x4567, 0x89ab, 0xcdef)));
        source.setPixelColor(
            1024, 512, QColor::fromRgba64(QRgba64::fromRgba64(0xfedc, 0xba98, 0x7654, 0x4321)));
        if (format != QImage::Format_Grayscale16)
            source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
        source.setDotsPerMeterX(4321);
        source.setDotsPerMeterY(5678);
        source.setOffset(QPoint(3, 7));
        source.setText(QStringLiteral("Author"), QStringLiteral("Managed image fixture"));
        const QByteArray encoded = pngBytes(source);
        const QImage expected = QImage::fromData(encoded, "PNG");
        require(!expected.isNull(), "Qt must decode the managed reader reference fixture");
        const void* midpoint = nullptr;
        {
            QBuffer input;
            input.setData(encoded);
            require(input.open(QIODevice::ReadOnly), "open the managed reader fixture");
            QImageReader reader(&input, "PNG");
            QImage decoded = snow_shot::image_codec::readManagedImage(reader);
            require(decoded == expected && decoded.format() == expected.format(),
                    "managed decoding must preserve Qt's exact pixels and 16-bit precision");
            require(decoded.colorSpace() == expected.colorSpace() &&
                        decoded.dotsPerMeterX() == expected.dotsPerMeterX() &&
                        decoded.dotsPerMeterY() == expected.dotsPerMeterY() &&
                        decoded.offset() == expected.offset() &&
                        decoded.text(QStringLiteral("Author")) ==
                            expected.text(QStringLiteral("Author")),
                    "managed decoding must preserve Qt's color, resolution and text metadata");
            midpoint = decoded.constBits() + decoded.sizeInBytes() / 2;
            require(snow::test_support::virtualMemoryMapped(midpoint),
                    "managed reader pixels must remain mapped while an image owns them");
        }
        require(!snow::test_support::virtualMemoryMapped(midpoint),
                "dropping the decoded image must unmap its large pixel allocation");

        // Reader scaling may replace the preallocation. Its final image still
        // needs managed storage and must retain the exact Qt scaling result.
        QBuffer scaledInput;
        scaledInput.setData(encoded);
        require(scaledInput.open(QIODevice::ReadOnly), "open the scaled reader fixture");
        QImageReader scaledReader(&scaledInput, "PNG");
        scaledReader.setScaledSize(QSize(1023, 511));
        QImage scaled = snow_shot::image_codec::readManagedImage(scaledReader);
        require(scaled == expected.scaled(QSize(1023, 511), Qt::IgnoreAspectRatio,
                                          Qt::SmoothTransformation),
                "managed decoding must preserve the reader's fallback scaling result");
        if (scaled.sizeInBytes() >= 1024 * 1024) {
            midpoint = scaled.constBits() + scaled.sizeInBytes() / 2;
            scaled = {};
            require(!snow::test_support::virtualMemoryMapped(midpoint),
                    "the reader's replacement raster must release its large allocation");
        }
    }

    const QByteArray encoded = pngBytes(solid(Qt::red, QSize(1025, 513)));
    const int oldLimit = QImageReader::allocationLimit();
    struct RestoreLimit final {
        int limit;
        ~RestoreLimit() {
            QImageReader::setAllocationLimit(limit);
        }
    } restore{oldLimit};
    QImageReader::setAllocationLimit(1);
    QBuffer input;
    input.setData(encoded);
    require(input.open(QIODevice::ReadOnly), "open the allocation-limit fixture");
    QImageReader reader(&input, "PNG");
    require(snow_shot::image_codec::readManagedImage(reader).isNull() &&
                reader.error() == QImageReader::InvalidDataError,
            "managed preallocation must preserve Qt's allocation limit and decode error");
    QBuffer corrupt;
    corrupt.setData(QByteArray("invalid PNG"));
    require(corrupt.open(QIODevice::ReadOnly), "open the corrupt reader fixture");
    QImageReader corruptReader(&corrupt, "PNG");
    require(snow_shot::image_codec::readManagedImage(corruptReader).isNull(),
            "managed decoding must reject a corrupt image");
}

QByteArray jpegBytes(const QImage& image) {
    QByteArray bytes;
    QBuffer output(&bytes);
    require(output.open(QIODevice::WriteOnly), "open JPEG fixture buffer");
    snow::image::EncodeOptions options;
    options.quality = 100;
    options.chroma_subsampling = snow::image::ChromaSubsampling::yuv444;
    require(
        snow_shot::image_codec::encodeToDevice(image, &output, snow::image::Format::jpeg, options),
        "encode a JPEG fixture through the application codec");
    return bytes;
}

QString writeFixture(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes) {
    const QString path = directory.filePath(name);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "write the image fixture");
    return path;
}

void append32(QByteArray& bytes, std::uint32_t value, bool little = false) {
    for (unsigned index = 0; index < 4; ++index) {
        const unsigned shift = little ? index * 8U : 24U - index * 8U;
        bytes.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

std::uint32_t read32(const QByteArray& bytes, qsizetype offset, bool little = false) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        const unsigned shift = little ? index * 8U : 24U - index * 8U;
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index]))
                 << shift;
    }
    return value;
}

std::uint32_t crc(const QByteArray& bytes) {
    std::uint32_t value = 0xffffffffU;
    for (const char byte : bytes) {
        value ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ ((value & 1U) != 0 ? 0xedb88320U : 0U);
    }
    return value ^ 0xffffffffU;
}

QByteArray pngChunk(const QByteArray& type, const QByteArray& data) {
    QByteArray result;
    append32(result, static_cast<std::uint32_t>(data.size()));
    result.append(type);
    result.append(data);
    append32(result, crc(type + data));
    return result;
}

QByteArray pngData(const QByteArray& encoded) {
    QByteArray data;
    for (qsizetype offset = 8; offset + 12 <= encoded.size();) {
        const qsizetype length = read32(encoded, offset);
        require(length <= encoded.size() - offset - 12, "valid PNG fixture chunks");
        if (encoded.mid(offset + 4, 4) == QByteArray("IDAT"))
            data.append(encoded.mid(offset + 8, length));
        offset += length + 12;
    }
    return data;
}

QByteArray frameControl(std::uint32_t sequence, const QSize& size, std::uint32_t x = 0,
                        std::uint32_t y = 0) {
    QByteArray data;
    append32(data, sequence);
    append32(data, static_cast<std::uint32_t>(size.width()));
    append32(data, static_cast<std::uint32_t>(size.height()));
    append32(data, x);
    append32(data, y);
    data.append(QByteArray::fromHex("000100640000"));
    return pngChunk("fcTL", data);
}

QByteArray animatedPng(bool separatePoster) {
    const QSize canvas(4, 3);
    const auto poster = pngBytes(solid(Qt::blue, canvas));
    const auto first = pngBytes(solid(Qt::red, separatePoster ? QSize(2, 1) : canvas));
    const auto second = pngBytes(solid(Qt::green, canvas));
    QByteArray result = poster.left(33);
    QByteArray animation;
    append32(animation, 2);
    append32(animation, 0);
    result.append(pngChunk("acTL", animation));
    std::uint32_t sequence = 0;
    if (separatePoster) {
        result.append(pngChunk("IDAT", pngData(poster)));
        result.append(frameControl(sequence++, QSize(2, 1), 1, 1));
        QByteArray data;
        append32(data, sequence++);
        data.append(pngData(first));
        result.append(pngChunk("fdAT", data));
    } else {
        result.append(frameControl(sequence++, canvas));
        result.append(pngChunk("IDAT", pngData(first)));
    }
    result.append(frameControl(sequence++, canvas));
    QByteArray data;
    append32(data, sequence);
    data.append(pngData(second));
    result.append(pngChunk("fdAT", data));
    result.append(pngChunk("IEND", {}));
    return result;
}

QByteArray riffChunk(const QByteArray& type, const QByteArray& payload) {
    QByteArray result = type;
    append32(result, static_cast<std::uint32_t>(payload.size()), true);
    result.append(payload);
    if ((payload.size() & 1) != 0)
        result.append('\0');
    return result;
}

void append24(QByteArray& bytes, std::uint32_t value) {
    for (unsigned index = 0; index < 3; ++index)
        bytes.append(static_cast<char>((value >> (index * 8U)) & 0xffU));
}

QByteArray webpFrame(const QImage& image) {
    const QByteArray encoded = snow_shot::image_codec::encodeWebp(image, 100);
    require(!encoded.isEmpty(), "encode static WebP without a Qt WebP plugin");
    QByteArray payload;
    for (int index = 0; index < 2; ++index)
        append24(payload, 0);
    append24(payload, static_cast<std::uint32_t>(image.width() - 1));
    append24(payload, static_cast<std::uint32_t>(image.height() - 1));
    append24(payload, 50);
    payload.append('\0');
    for (qsizetype offset = 12; offset + 8 <= encoded.size();) {
        const qsizetype length = read32(encoded, offset + 4, true);
        const QByteArray type = encoded.mid(offset, 4);
        const qsizetype chunkSize = 8 + length + (length & 1);
        require(chunkSize <= encoded.size() - offset, "valid WebP fixture chunks");
        if (type == QByteArray("VP8 ") || type == QByteArray("VP8L") || type == QByteArray("ALPH"))
            payload.append(encoded.mid(offset, chunkSize));
        offset += chunkSize;
    }
    return riffChunk("ANMF", payload);
}

QByteArray animatedWebp() {
    QByteArray extended = QByteArray::fromHex("02000000");
    append24(extended, 3);
    append24(extended, 2);
    QByteArray contents = "WEBP";
    contents.append(riffChunk("VP8X", extended));
    contents.append(riffChunk("ANIM", QByteArray(6, '\0')));
    contents.append(webpFrame(solid(Qt::red)));
    contents.append(webpFrame(solid(Qt::blue)));
    QByteArray result = "RIFF";
    append32(result, static_cast<std::uint32_t>(contents.size()), true);
    result.append(contents);
    return result;
}

void supportedFormatsAndPreviewBounds(const QTemporaryDir& directory) {
    const QImage source = solid(QColor(40, 130, 210));
    const std::array fixtures{
        std::pair{QStringLiteral("skin.png"), pngBytes(source)},
        std::pair{QStringLiteral("skin.JPG"), jpegBytes(source)},
        std::pair{QStringLiteral("skin.webp"), snow_shot::image_codec::encodeWebp(source, 100)},
    };
    for (const auto& [name, bytes] : fixtures) {
        const auto result = decodeSkinFile(writeFixture(directory, name, bytes));
        require(result.error == SkinDecodeError::None && result.image.size() == source.size(),
                "PNG, JPEG and WebP skin images should decode");
        require(result.image.format() == QImage::Format_ARGB32_Premultiplied &&
                    result.image.colorSpace() == QColorSpace(QColorSpace::SRgb) &&
                    result.image.devicePixelRatio() == 1.0,
                "skin previews should use sRGB premultiplied pixels and a neutral DPR");
    }
    const auto large = decodeSkinFile(writeFixture(directory, QStringLiteral("large.png"),
                                                   pngBytes(solid(Qt::red, QSize(5000, 200)))));
    require(large.error == SkinDecodeError::None && large.image.size() == QSize(4096, 164),
            "PNG should be reduced even though its native codec ignores preview extent");
    const auto thin = decodeSkinFile(writeFixture(directory, QStringLiteral("thin.png"),
                                                  pngBytes(solid(Qt::red, QSize(16384, 1)))));
    require(thin.error == SkinDecodeError::None && thin.image.size() == QSize(4096, 1),
            "extreme aspect ratios should retain at least one pixel on their shorter side");
}

void firstAnimationFrame(const QTemporaryDir& directory) {
    for (const bool poster : {false, true}) {
        const auto png = decodeSkinFile(writeFixture(
            directory, poster ? QStringLiteral("poster.png") : QStringLiteral("animated.png"),
            animatedPng(poster)));
        require(png.error == SkinDecodeError::None && png.image.size() == QSize(4, 3),
                "an APNG skin should retain its canvas dimensions");
        require(png.image.pixelColor(1, 1) == QColor(Qt::red),
                "an APNG skin should display its first animation frame");
        if (poster)
            require(png.image.pixelColor(0, 0).alpha() == 0,
                    "an APNG poster should not be composited under its first animation frame");
    }
    const auto webp =
        decodeSkinFile(writeFixture(directory, QStringLiteral("animated.webp"), animatedWebp()));
    require(webp.error == SkinDecodeError::None && webp.image.size() == QSize(4, 3),
            "animated WebP should provide a static first-frame skin");
    require(webp.image.pixelColor(1, 1).red() > 200 && webp.image.pixelColor(1, 1).blue() < 30,
            "the first WebP frame should be displayed rather than the final frame");
}

void orientationAndColor(const QTemporaryDir& directory) {
    QImage source = solid(Qt::red, QSize(6, 2));
    for (int y = 0; y < source.height(); ++y)
        for (int x = 3; x < source.width(); ++x)
            source.setPixelColor(x, y, Qt::blue);
    QByteArray jpeg = jpegBytes(source);
    const QByteArray exif =
        QByteArray::fromHex("45786966000049492a0008000000010012010300010000000600000000000000");
    QByteArray marker = QByteArray::fromHex("ffe1");
    const auto length = static_cast<std::uint16_t>(exif.size() + 2);
    marker.append(static_cast<char>(length >> 8U));
    marker.append(static_cast<char>(length & 0xffU));
    marker.append(exif);
    jpeg.insert(2, marker);
    const auto rotated =
        decodeSkinFile(writeFixture(directory, QStringLiteral("rotated.jpg"), jpeg));
    require(rotated.error == SkinDecodeError::None && rotated.image.size() == QSize(2, 6),
            "JPEG EXIF orientation should rotate the reduced skin preview");
    require(rotated.image.pixelColor(0, 0).red() > 200 &&
                rotated.image.pixelColor(0, 5).blue() > 200,
            "orientation six should rotate image content clockwise");

    QImage tagged = solid(QColor(200, 110, 65, 180));
    tagged.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QImage expected = tagged.convertedToColorSpace(QColorSpace(QColorSpace::SRgb),
                                                         QImage::Format_ARGB32_Premultiplied);
    const auto color =
        decodeSkinFile(writeFixture(directory, QStringLiteral("profile.png"), pngBytes(tagged)));
    require(color.error == SkinDecodeError::None, "decode a PNG with an embedded ICC profile");
    const QColor actualPixel = color.image.pixelColor(0, 0);
    const QColor expectedPixel = expected.pixelColor(0, 0);
    require(qAbs(actualPixel.red() - expectedPixel.red()) <= 1 &&
                qAbs(actualPixel.green() - expectedPixel.green()) <= 1 &&
                qAbs(actualPixel.blue() - expectedPixel.blue()) <= 1 &&
                actualPixel.alpha() == expectedPixel.alpha(),
            "skin decoding should convert ICC-tagged pixels to sRGB while preserving alpha");
}

void resourceAndFailureClassification(const QTemporaryDir& directory) {
    require(decodeSkinFile(directory.filePath(QStringLiteral("missing.png"))).error ==
                SkinDecodeError::UnreadableFile,
            "missing paths should produce a file error");
    require(decodeSkinFile(directory.filePath(QStringLiteral("skin.gif"))).error ==
                SkinDecodeError::UnsupportedFormat,
            "unsupported file extensions should be rejected");
    require(decodeSkinFile(writeFixture(directory, QStringLiteral("empty.png"), {})).error ==
                SkinDecodeError::InvalidImage,
            "empty supported files should produce an image error");
    require(decodeSkinFile(writeFixture(directory, QStringLiteral("corrupt.png"),
                                        QByteArray::fromHex("89504e470d0a1a0a")))
                    .error == SkinDecodeError::InvalidImage,
            "corrupt supported images should produce an image error");
    QFile huge(directory.filePath(QStringLiteral("huge.png")));
    require(huge.open(QIODevice::WriteOnly) && huge.resize((64LL << 20U) + 1),
            "create a sparse oversized skin fixture");
    huge.close();
    require(decodeSkinFile(huge.fileName()).error == SkinDecodeError::InputTooLarge,
            "oversized encoded files should be rejected before being read");
    const auto normal = pngBytes(solid(Qt::red));
    for (const QSize dimensions : {QSize(16385, 1), QSize(8001, 8000), QSize(8193, 8192)}) {
        QByteArray header = normal.mid(16, 13);
        QByteArray sizeBytes;
        append32(sizeBytes, static_cast<std::uint32_t>(dimensions.width()));
        append32(sizeBytes, static_cast<std::uint32_t>(dimensions.height()));
        header.replace(0, 8, sizeBytes);
        const QByteArray encoded = normal.left(8) + pngChunk("IHDR", header) + normal.mid(33);
        require(decodeSkinFile(writeFixture(directory, QStringLiteral("dimensions.png"), encoded))
                        .error == SkinDecodeError::ResourceLimit,
                "source dimension and pixel limits should be checked before raster allocation");
    }
    const QByteArray metadata = pngChunk("tEXt", QByteArray((8 << 20) + 1, 'x'));
    require(decodeSkinFile(writeFixture(directory, QStringLiteral("metadata.png"),
                                        normal.left(33) + metadata + normal.mid(33)))
                    .error == SkinDecodeError::ResourceLimit,
            "skin metadata should have a separate bounded allocation policy");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        managedQtReader();
        if (application.arguments().contains(QStringLiteral("--managed-reader-only")))
            return 0;
        QTemporaryDir directory;
        require(directory.isValid(), "create the skin codec fixture directory");
        supportedFormatsAndPreviewBounds(directory);
        firstAnimationFrame(directory);
        orientationAndColor(directory);
        resourceAndFailureClassification(directory);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
