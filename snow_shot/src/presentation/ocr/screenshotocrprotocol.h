#pragma once

#include <QByteArray>
#include <QString>
#include <cstring>

namespace snow_shot::ocr::protocol {
constexpr quint32 kProtocolMagic = 0x52434f53; // "SOCR" in little endian.
constexpr quint16 kProtocolVersion = 3;
constexpr auto kRuntimeVersion = "1.0.7";
constexpr quint16 kHello = 1;
constexpr quint16 kReady = 2;
constexpr quint16 kSubmit = 3;
constexpr quint16 kCancel = 4;
constexpr quint16 kComplete = 5;
constexpr quint16 kShutdown = 6;
constexpr quint16 kPrepareSession = 8, kSessionReady = 9;
constexpr quint16 kReleaseSession = 10, kSessionReleased = 11;
constexpr quint16 kAttachBuffer = 12, kBufferAttached = 13;
constexpr quint16 kRecognize = 14, kImageConsumed = 15;
constexpr quint16 kDetachBuffer = 16, kBufferDetached = 17, kDiscardImage = 18;
constexpr qsizetype kSlotHeaderBytes = 32;
constexpr unsigned long kTransportStopTimeoutMilliseconds = 5000;
constexpr qsizetype kMaximumFrameBytes = 1024LL * 1024LL;
constexpr qsizetype kMaximumOutstandingRequests = 32;
constexpr qsizetype kMaximumOutstandingRequestsPerReceiver = 8;
constexpr qsizetype kSlotSequenceOffset = 0;
constexpr qsizetype kSlotStateOffset = 8;
constexpr qsizetype kSlotWidthOffset = 12;
constexpr qsizetype kSlotHeightOffset = 16;
constexpr qsizetype kSlotStrideOffset = 20;
constexpr qsizetype kSlotBytesOffset = 24;
constexpr qsizetype kSlotMagicOffset = 28;
constexpr quint32 kSlotFree = 0;
constexpr quint32 kSlotReady = 1;
constexpr quint32 kSlotMagic = 0x544f4c53; // "SLOT" in little endian.

inline void appendU8(QByteArray& bytes, quint8 value) {
    bytes.append(char(value));
}
inline void appendU32(QByteArray& bytes, quint32 value) {
    for (int index = 0; index < 4; ++index)
        bytes.append(char((value >> (index * 8)) & 0xff));
}
inline void appendU64(QByteArray& bytes, quint64 value) {
    for (int index = 0; index < 8; ++index)
        bytes.append(char((value >> (index * 8)) & 0xff));
}
inline void appendString(QByteArray& bytes, const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    appendU32(bytes, static_cast<quint32>(utf8.size()));
    bytes.append(utf8);
}

inline void writeU32(uchar* destination, quint32 value) {
    destination[0] = static_cast<uchar>(value & 0xff);
    destination[1] = static_cast<uchar>((value >> 8) & 0xff);
    destination[2] = static_cast<uchar>((value >> 16) & 0xff);
    destination[3] = static_cast<uchar>((value >> 24) & 0xff);
}

inline void writeU64(uchar* destination, quint64 value) {
    for (int index = 0; index < 8; ++index) {
        destination[index] = static_cast<uchar>((value >> (index * 8)) & 0xff);
    }
}

inline bool takeU8(const QByteArray& bytes, qsizetype& offset, quint8* value) {
    if (offset + 1 > bytes.size())
        return false;
    *value = static_cast<quint8>(bytes.at(offset++));
    return true;
}
inline bool takeU32(const QByteArray& bytes, qsizetype& offset, quint32* value) {
    if (offset + 4 > bytes.size())
        return false;
    *value = static_cast<quint32>(static_cast<quint8>(bytes.at(offset))) |
             (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 8) |
             (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 16) |
             (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3))) << 24);
    offset += 4;
    return true;
}
inline bool takeU64(const QByteArray& bytes, qsizetype& offset, quint64* value) {
    if (offset + 8 > bytes.size())
        return false;
    *value = 0;
    for (int index = 0; index < 8; ++index) {
        *value |= static_cast<quint64>(static_cast<quint8>(bytes.at(offset + index)))
                  << (index * 8);
    }
    offset += 8;
    return true;
}
inline bool takeF32(const QByteArray& bytes, qsizetype& offset, float* value) {
    quint32 raw = 0;
    if (!takeU32(bytes, offset, &raw))
        return false;
    std::memcpy(value, &raw, sizeof(raw));
    return true;
}
inline bool takeString(const QByteArray& bytes, qsizetype& offset, QString* value) {
    quint32 length = 0;
    if (!takeU32(bytes, offset, &length) || length > static_cast<quint32>(bytes.size() - offset))
        return false;
    *value = QString::fromUtf8(bytes.constData() + offset, static_cast<qsizetype>(length));
    offset += static_cast<qsizetype>(length);
    return true;
}

inline QByteArray makeFrame(quint16 kind, quint64 requestId, const QByteArray& payload = {}) {
    QByteArray frame;
    frame.reserve(20 + payload.size());
    appendU32(frame, kProtocolMagic);
    frame.append(char(kProtocolVersion & 0xff));
    frame.append(char((kProtocolVersion >> 8) & 0xff));
    frame.append(char(kind & 0xff));
    frame.append(char((kind >> 8) & 0xff));
    appendU64(frame, requestId);
    appendU32(frame, static_cast<quint32>(payload.size()));
    frame.append(payload);
    return frame;
}

} // namespace snow_shot::ocr::protocol
