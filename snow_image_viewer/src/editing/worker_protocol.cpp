#include "editing/worker_protocol.h"

#include <QJsonArray>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace snow::image_viewer::worker_protocol {
namespace {

constexpr char kMagic[] = "SIEW";
constexpr qsizetype kHeaderBytes = 24;
constexpr int kMaximumDepth = 32;

enum class ValueTag : std::uint8_t {
    null_value = 0,
    false_value = 1,
    true_value = 2,
    number = 3,
    string = 4,
    array = 5,
    object = 6,
};

void appendU16(QByteArray* bytes, std::uint16_t value) {
    bytes->append(static_cast<char>(value & 0xffU));
    bytes->append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendU32(QByteArray* bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes->append(static_cast<char>((value >> shift) & 0xffU));
}

void appendU64(QByteArray* bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes->append(static_cast<char>((value >> shift) & 0xffU));
}

std::uint16_t readU16(const uchar* data) {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t readU32(const uchar* data) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(data[index]) << (index * 8U);
    return value;
}

std::uint64_t readU64(const uchar* data) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
    return value;
}

std::uint32_t crc32c(QByteArrayView bytes) {
    std::uint32_t state = 0xFFFFFFFFU;
    for (const char raw : bytes) {
        state ^= static_cast<std::uint8_t>(raw);
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (state & 1U);
            state = (state >> 1U) ^ (0x82F63B78U & mask);
        }
    }
    return state ^ 0xFFFFFFFFU;
}

void setError(QString* error, const QString& message) {
    if (error)
        *error = message;
}

bool appendString(QByteArray* output, const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    if (utf8.size() > static_cast<qsizetype>(std::numeric_limits<std::uint32_t>::max()))
        return false;
    appendU32(output, static_cast<std::uint32_t>(utf8.size()));
    output->append(utf8);
    return output->size() <= kMaximumPayloadBytes;
}

bool appendValue(QByteArray* output, const QJsonValue& value, int depth) {
    if (depth > kMaximumDepth)
        return false;
    switch (value.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        output->append(static_cast<char>(ValueTag::null_value));
        return true;
    case QJsonValue::Bool:
        output->append(
            static_cast<char>(value.toBool() ? ValueTag::true_value : ValueTag::false_value));
        return true;
    case QJsonValue::Double: {
        const double number = value.toDouble();
        if (!std::isfinite(number))
            return false;
        output->append(static_cast<char>(ValueTag::number));
        appendU64(output, std::bit_cast<std::uint64_t>(number));
        return true;
    }
    case QJsonValue::String:
        output->append(static_cast<char>(ValueTag::string));
        return appendString(output, value.toString());
    case QJsonValue::Array: {
        const QJsonArray array = value.toArray();
        output->append(static_cast<char>(ValueTag::array));
        appendU32(output, static_cast<std::uint32_t>(array.size()));
        for (const auto& element : array) {
            if (!appendValue(output, element, depth + 1))
                return false;
        }
        return output->size() <= kMaximumPayloadBytes;
    }
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        output->append(static_cast<char>(ValueTag::object));
        appendU32(output, static_cast<std::uint32_t>(object.size()));
        for (auto entry = object.constBegin(); entry != object.constEnd(); ++entry) {
            if (!appendString(output, entry.key()) ||
                !appendValue(output, entry.value(), depth + 1))
                return false;
        }
        return output->size() <= kMaximumPayloadBytes;
    }
    }
    return false;
}

class ValueReader final {
  public:
    explicit ValueReader(QByteArrayView bytes) : bytes_(bytes) {}

    bool value(QJsonValue* output, int depth = 0) {
        if (!output || depth > kMaximumDepth || remaining() < 1)
            return false;
        const ValueTag tag = static_cast<ValueTag>(byte());
        switch (tag) {
        case ValueTag::null_value:
            *output = QJsonValue::Null;
            return true;
        case ValueTag::false_value:
            *output = false;
            return true;
        case ValueTag::true_value:
            *output = true;
            return true;
        case ValueTag::number: {
            std::uint64_t bits = 0;
            if (!u64(&bits))
                return false;
            const double number = std::bit_cast<double>(bits);
            if (!std::isfinite(number))
                return false;
            *output = number;
            return true;
        }
        case ValueTag::string: {
            QString text;
            if (!string(&text))
                return false;
            *output = text;
            return true;
        }
        case ValueTag::array: {
            std::uint32_t count = 0;
            if (!u32(&count) || count > static_cast<std::uint32_t>(remaining()))
                return false;
            QJsonArray array;
            for (std::uint32_t index = 0; index < count; ++index) {
                QJsonValue element;
                if (!value(&element, depth + 1))
                    return false;
                array.append(element);
            }
            *output = std::move(array);
            return true;
        }
        case ValueTag::object: {
            std::uint32_t count = 0;
            if (!u32(&count) || count > static_cast<std::uint32_t>(remaining()))
                return false;
            QJsonObject object;
            for (std::uint32_t index = 0; index < count; ++index) {
                QString key;
                QJsonValue element;
                if (!string(&key) || object.contains(key) || !value(&element, depth + 1))
                    return false;
                object.insert(key, element);
            }
            *output = std::move(object);
            return true;
        }
        }
        return false;
    }

    [[nodiscard]] bool complete() const noexcept {
        return offset_ == bytes_.size();
    }

  private:
    [[nodiscard]] qsizetype remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    uchar byte() {
        return static_cast<uchar>(bytes_[offset_++]);
    }

    bool u32(std::uint32_t* output) {
        if (!output || remaining() < 4)
            return false;
        *output = readU32(reinterpret_cast<const uchar*>(bytes_.data() + offset_));
        offset_ += 4;
        return true;
    }

    bool u64(std::uint64_t* output) {
        if (!output || remaining() < 8)
            return false;
        *output = readU64(reinterpret_cast<const uchar*>(bytes_.data() + offset_));
        offset_ += 8;
        return true;
    }

    bool string(QString* output) {
        std::uint32_t size = 0;
        if (!output || !u32(&size) || size > static_cast<std::uint32_t>(remaining()))
            return false;
        const QByteArrayView encoded(bytes_.data() + offset_, static_cast<qsizetype>(size));
        const QString decoded = QString::fromUtf8(encoded.data(), encoded.size());
        if (decoded.toUtf8() != encoded)
            return false;
        offset_ += static_cast<qsizetype>(size);
        *output = decoded;
        return true;
    }

    QByteArrayView bytes_;
    qsizetype offset_ = 0;
};

} // namespace

QByteArray encodeFrame(MessageType type, const QJsonObject& payload) {
    QByteArray body;
    if (!appendValue(&body, payload, 0) || body.size() > kMaximumPayloadBytes)
        return {};
    QByteArray frame;
    frame.reserve(kHeaderBytes + body.size());
    frame.append(kMagic, 4);
    appendU32(&frame, kVersion);
    appendU16(&frame, static_cast<std::uint16_t>(type));
    appendU16(&frame, 0);
    appendU64(&frame, static_cast<std::uint64_t>(body.size()));
    appendU32(&frame, crc32c(body));
    frame.append(body);
    return frame;
}

bool takeFrame(QByteArray* buffer, Frame* frame, QString* error) {
    if (!buffer || !frame) {
        setError(error, QStringLiteral("The worker frame destination is invalid."));
        return false;
    }
    if (buffer->size() < kHeaderBytes)
        return false;
    const auto* bytes = reinterpret_cast<const uchar*>(buffer->constData());
    if (std::memcmp(bytes, kMagic, 4) != 0 || readU32(bytes + 4) != kVersion ||
        readU16(bytes + 10) != 0) {
        setError(error, QStringLiteral("The worker protocol header is unsupported."));
        buffer->clear();
        return false;
    }
    const std::uint16_t rawType = readU16(bytes + 8);
    if (rawType < static_cast<std::uint16_t>(MessageType::ready) ||
        rawType > static_cast<std::uint16_t>(MessageType::shutdown)) {
        setError(error, QStringLiteral("The worker protocol message type is invalid."));
        buffer->clear();
        return false;
    }
    const std::uint64_t length = readU64(bytes + 12);
    if (length > static_cast<std::uint64_t>(kMaximumPayloadBytes) ||
        length > static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max())) {
        setError(error, QStringLiteral("The worker protocol payload is too large."));
        buffer->clear();
        return false;
    }
    const qsizetype total = kHeaderBytes + static_cast<qsizetype>(length);
    if (buffer->size() < total)
        return false;
    const QByteArrayView body(buffer->constData() + kHeaderBytes, static_cast<qsizetype>(length));
    if (crc32c(body) != readU32(bytes + 20)) {
        setError(error, QStringLiteral("The worker protocol checksum does not match."));
        buffer->clear();
        return false;
    }
    ValueReader reader(body);
    QJsonValue decoded;
    if (!reader.value(&decoded) || !reader.complete() || !decoded.isObject()) {
        setError(error, QStringLiteral("The worker protocol payload is malformed."));
        buffer->clear();
        return false;
    }
    frame->type = static_cast<MessageType>(rawType);
    frame->payload = decoded.toObject();
    buffer->remove(0, total);
    return true;
}

QJsonObject settingsToJson(const EditExportSettings& settings) {
    snow::image::EncodeOptions encode = settings.encode;
    snow::image::Service service;
    if (const snow::image::EncoderInfo* encoder = service.encoder_info(settings.format)) {
        auto normalized = snow::image::normalize_encode_options(*encoder, encode);
        if (normalized)
            encode = std::move(normalized).value();
    }
    return {{QStringLiteral("sourceWidth"), settings.sourceSize.width()},
            {QStringLiteral("sourceHeight"), settings.sourceSize.height()},
            {QStringLiteral("width"), settings.width},
            {QStringLiteral("height"), settings.height},
            {QStringLiteral("resampling"), static_cast<int>(settings.resampling)},
            {QStringLiteral("premultiplyAlpha"), settings.premultiplyAlpha},
            {QStringLiteral("linearRgb"), settings.linearRgb},
            {QStringLiteral("maintainAspectRatio"), settings.maintainAspectRatio},
            {QStringLiteral("reducePalette"), settings.reducePalette},
            {QStringLiteral("paletteColors"), settings.paletteColors},
            {QStringLiteral("ditheringPercent"), settings.ditheringPercent},
            {QStringLiteral("format"), static_cast<int>(settings.format)},
            {QStringLiteral("quality"), encode.quality},
            {QStringLiteral("effort"), encode.effort},
            {QStringLiteral("losslessEffort"), encode.lossless_effort},
            {QStringLiteral("lossless"), encode.lossless},
            {QStringLiteral("preserveMetadata"), encode.preserve_metadata},
            {QStringLiteral("progressive"), encode.progressive},
            {QStringLiteral("interlaced"), encode.interlaced},
            {QStringLiteral("compressionLevel"), encode.compression_level},
            {QStringLiteral("chromaSubsampling"),
             encode.chroma_subsampling ? static_cast<int>(*encode.chroma_subsampling) : -1}};
}

QJsonObject receiptToJson(const snow::image::EncodedArtifactReceipt& receipt) {
    QJsonArray frames;
    for (const auto& extent : receipt.emitted_frame_extents) {
        frames.append(QJsonObject{{QStringLiteral("x"), static_cast<int>(extent.x)},
                                  {QStringLiteral("y"), static_cast<int>(extent.y)},
                                  {QStringLiteral("width"), static_cast<int>(extent.width)},
                                  {QStringLiteral("height"), static_cast<int>(extent.height)}});
    }
    return {
        {QStringLiteral("format"), static_cast<int>(receipt.format)},
        {QStringLiteral("documentKind"), static_cast<int>(receipt.document_kind)},
        {QStringLiteral("canvasWidth"), static_cast<int>(receipt.canvas_width)},
        {QStringLiteral("canvasHeight"), static_cast<int>(receipt.canvas_height)},
        {QStringLiteral("emittedFrameCount"), static_cast<int>(receipt.emitted_frame_count)},
        {QStringLiteral("emittedFrameExtents"), frames},
        {QStringLiteral("resolvedJpegChromaSubsampling"),
         receipt.jpeg_chroma_subsampling ? static_cast<int>(*receipt.jpeg_chroma_subsampling) : -1},
        {QStringLiteral("encoderFinalizedAndSinkFlushed"),
         receipt.encoder_finalized_and_sink_flushed}};
}

bool receiptFromJson(const QJsonValue& value, snow::image::EncodedArtifactReceipt* receipt,
                     QString* error) {
    if (!receipt || !value.isObject()) {
        setError(error, QStringLiteral("The encoded artifact receipt is malformed."));
        return false;
    }
    const QJsonObject object = value.toObject();
    const int format = object.value(QStringLiteral("format")).toInt(-1);
    const int kind = object.value(QStringLiteral("documentKind")).toInt(-1);
    const int width = object.value(QStringLiteral("canvasWidth")).toInt(-1);
    const int height = object.value(QStringLiteral("canvasHeight")).toInt(-1);
    const int count = object.value(QStringLiteral("emittedFrameCount")).toInt(-1);
    const int sampling = object.value(QStringLiteral("resolvedJpegChromaSubsampling")).toInt(-2);
    if (format < static_cast<int>(snow::image::Format::unknown) ||
        format > static_cast<int>(snow::image::Format::webp) ||
        kind < static_cast<int>(snow::image::DocumentKind::raster) ||
        kind > static_cast<int>(snow::image::DocumentKind::deep) || width < 0 || height < 0 ||
        count < 0 || !object.value(QStringLiteral("encoderFinalizedAndSinkFlushed")).toBool() ||
        (format == static_cast<int>(snow::image::Format::jpeg)
             ? sampling < static_cast<int>(snow::image::ChromaSubsampling::none) ||
                   sampling > static_cast<int>(snow::image::ChromaSubsampling::yuv420)
             : sampling != -1)) {
        setError(error, QStringLiteral("The encoded artifact receipt values are invalid."));
        return false;
    }
    const QJsonValue framesValue = object.value(QStringLiteral("emittedFrameExtents"));
    if (!framesValue.isArray()) {
        setError(error, QStringLiteral("The encoded artifact frame extents are malformed."));
        return false;
    }
    const QJsonArray frames = framesValue.toArray();
    if (frames.size() != count) {
        setError(error, QStringLiteral("The encoded artifact frame count is inconsistent."));
        return false;
    }
    snow::image::EncodedArtifactReceipt parsed;
    parsed.format = static_cast<snow::image::Format>(format);
    parsed.document_kind = static_cast<snow::image::DocumentKind>(kind);
    parsed.canvas_width = static_cast<std::uint32_t>(width);
    parsed.canvas_height = static_cast<std::uint32_t>(height);
    parsed.emitted_frame_count = static_cast<std::uint32_t>(count);
    parsed.encoder_finalized_and_sink_flushed = true;
    if (sampling >= 0) {
        parsed.jpeg_chroma_subsampling = static_cast<snow::image::ChromaSubsampling>(sampling);
    }
    parsed.emitted_frame_extents.reserve(static_cast<std::size_t>(frames.size()));
    for (const auto& frameValue : frames) {
        if (!frameValue.isObject()) {
            setError(error, QStringLiteral("The encoded artifact frame extent is malformed."));
            return false;
        }
        const QJsonObject frame = frameValue.toObject();
        const int x = frame.value(QStringLiteral("x")).toInt(-1);
        const int y = frame.value(QStringLiteral("y")).toInt(-1);
        const int frameWidth = frame.value(QStringLiteral("width")).toInt(-1);
        const int frameHeight = frame.value(QStringLiteral("height")).toInt(-1);
        if (x < 0 || y < 0 || frameWidth <= 0 || frameHeight <= 0) {
            setError(error, QStringLiteral("The encoded artifact frame extent is invalid."));
            return false;
        }
        parsed.emitted_frame_extents.push_back(
            {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
             static_cast<std::uint32_t>(frameWidth), static_cast<std::uint32_t>(frameHeight)});
    }
    *receipt = std::move(parsed);
    return true;
}

bool settingsFromJson(const QJsonValue& value, EditExportSettings* settings, QString* error) {
    if (!settings || !value.isObject()) {
        setError(error, QStringLiteral("The worker job settings are malformed."));
        return false;
    }
    const QJsonObject object = value.toObject();
    settings->sourceSize = QSize(object.value(QStringLiteral("sourceWidth")).toInt(),
                                 object.value(QStringLiteral("sourceHeight")).toInt());
    settings->width = object.value(QStringLiteral("width")).toInt();
    settings->height = object.value(QStringLiteral("height")).toInt();
    const int resampling = object.value(QStringLiteral("resampling")).toInt(-1);
    const int format = object.value(QStringLiteral("format")).toInt(-1);
    if (resampling < static_cast<int>(snow::image::ResamplingMethod::nearest) ||
        resampling > static_cast<int>(snow::image::ResamplingMethod::lanczos3) ||
        format < static_cast<int>(snow::image::Format::unknown) ||
        format > static_cast<int>(snow::image::Format::webp)) {
        setError(error, QStringLiteral("The worker job enum value is invalid."));
        return false;
    }
    settings->resampling = static_cast<snow::image::ResamplingMethod>(resampling);
    settings->premultiplyAlpha = object.value(QStringLiteral("premultiplyAlpha")).toBool();
    settings->linearRgb = object.value(QStringLiteral("linearRgb")).toBool();
    settings->maintainAspectRatio = object.value(QStringLiteral("maintainAspectRatio")).toBool();
    settings->reducePalette = object.value(QStringLiteral("reducePalette")).toBool();
    settings->paletteColors = object.value(QStringLiteral("paletteColors")).toInt();
    settings->ditheringPercent = object.value(QStringLiteral("ditheringPercent")).toInt();
    settings->format = static_cast<snow::image::Format>(format);
    settings->encode.format = settings->format;
    settings->encode.quality = object.value(QStringLiteral("quality")).toInt();
    settings->encode.effort = object.value(QStringLiteral("effort")).toInt();
    settings->encode.lossless_effort = object.value(QStringLiteral("losslessEffort")).toInt();
    settings->encode.lossless = object.value(QStringLiteral("lossless")).toBool();
    settings->encode.preserve_metadata = object.value(QStringLiteral("preserveMetadata")).toBool();
    settings->encode.progressive = object.value(QStringLiteral("progressive")).toBool();
    settings->encode.interlaced = object.value(QStringLiteral("interlaced")).toBool();
    settings->encode.compression_level = object.value(QStringLiteral("compressionLevel")).toInt();
    const int sampling = object.value(QStringLiteral("chromaSubsampling")).toInt(-2);
    if (sampling == -1) {
        settings->encode.chroma_subsampling.reset();
    } else if (sampling >= static_cast<int>(snow::image::ChromaSubsampling::yuv444) &&
               sampling <= static_cast<int>(snow::image::ChromaSubsampling::yuv420)) {
        settings->encode.chroma_subsampling = static_cast<snow::image::ChromaSubsampling>(sampling);
    } else {
        setError(error, QStringLiteral("The worker job chroma subsampling is invalid."));
        return false;
    }
    snow::image::Service service;
    const snow::image::EncoderInfo* encoder = service.encoder_info(settings->format);
    if (!encoder) {
        setError(error, QStringLiteral("The worker job encoder is unavailable."));
        return false;
    }
    auto normalized = snow::image::normalize_encode_options(*encoder, settings->encode);
    if (!normalized) {
        setError(error, QString::fromStdString(normalized.error().message));
        return false;
    }
    settings->encode = std::move(normalized).value();
    if (!settings->isValid()) {
        setError(error, QStringLiteral("The worker job settings are outside valid limits."));
        return false;
    }
    return true;
}

} // namespace snow::image_viewer::worker_protocol
