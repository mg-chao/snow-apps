#include "snow_shot/presentation/screenshotclipboardappearance.h"
#include "snow_shot/presentation/screenshotclipboardplacement.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QScopeGuard>
#include <QtEndian>
#include <cmath>
#include <limits>
#if defined(Q_OS_MACOS)
#include <QUtiMimeConverter>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {
constexpr quint32 kMagic = 0x53534341;
constexpr qsizetype kHeaderSize = 12;
bool validSize(QSize size) {
    return size.width() > 0 && size.height() > 0 &&
           qint64(size.width()) * size.height() <= 64LL * 1000 * 1000;
}
std::optional<int> integer(const QJsonValue& value) {
    const double n = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(n) || std::trunc(n) != n || n < 1 || n > std::numeric_limits<int>::max())
        return {};
    return static_cast<int>(n);
}
QSize parseSize(const QJsonValue& value) {
    const auto a = value.toArray();
    if (a.size() != 2)
        return {};
    const auto w = integer(a[0]), h = integer(a[1]);
    return w && h ? QSize(*w, *h) : QSize{};
}
QRectF parseRect(const QJsonValue& value) {
    const auto a = value.toArray();
    if (a.size() != 4)
        return {};
    for (const auto& n : a)
        if (!n.isDouble() || !std::isfinite(n.toDouble()))
            return {};
    return {a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()};
}
#if defined(Q_OS_MACOS)
class AppearanceMimeConverter final : public QUtiMimeConverter {
  public:
    QString mimeForUti(const QString& uti) const override {
        return uti == QStringLiteral("com.snowshot.screenshot-appearance")
                   ? screenshotClipboardAppearanceMimeType()
                   : QString{};
    }
    QString utiForMime(const QString& mime) const override {
        return mime == screenshotClipboardAppearanceMimeType()
                   ? QStringLiteral("com.snowshot.screenshot-appearance")
                   : QString{};
    }
    QVariant convertToMime(const QString&, const QList<QByteArray>& data,
                           const QString&) const override {
        return data.size() == 1 && data.first().size() <= kScreenshotClipboardAppearanceMaximumBytes
                   ? QVariant(data.first())
                   : QVariant{};
    }
    QList<QByteArray> convertFromMime(const QString&, const QVariant& data,
                                      const QString&) const override {
        return {data.toByteArray()};
    }
};
#endif
} // namespace

bool ScreenshotClipboardAppearance::isValid() const {
    if (!validSize(rasterSize) || filePath.size() > 4096 || filePath.contains(QChar::Null) ||
        (!filePath.isEmpty() && (fileSize <= 0 || fileModifiedMs < 0)))
        return false;
    if (!borderAppearance)
        return true;
    const auto& a = *borderAppearance;
    const auto& r = a.contentRect;
    if (!validSize(a.sourceSize) || !std::isfinite(r.x()) || !std::isfinite(r.y()) ||
        !std::isfinite(r.width()) || !std::isfinite(r.height()) || r.isEmpty() ||
        !QRectF(QPointF(), QSizeF(a.sourceSize)).contains(r) || !std::isfinite(a.cornerRadius) ||
        a.cornerRadius < 0 || a.cornerRadius > 1e9)
        return false;
    return !a.region || (!a.region->isEmpty() && QRectF(QPointF(), r.size())
                                                     .adjusted(-1e-6, -1e-6, 1e-6, 1e-6)
                                                     .contains(a.region->path().boundingRect()));
}
bool ScreenshotClipboardAppearance::matchesFile(const QString& path, qint64 size,
                                                qint64 modifiedMs) const {
    return !filePath.isEmpty() && filePath == screenshotClipboardFilePath(path) &&
           fileSize == size && fileModifiedMs == modifiedMs;
}
QString screenshotClipboardAppearanceMimeType() {
    return QStringLiteral("application/x-snow-shot-screenshot-appearance");
}
QString screenshotClipboardAppearanceNativeMimeType() {
#if defined(Q_OS_WIN)
    return QStringLiteral("application/x-qt-windows-mime;value=\"SnowShotScreenshotAppearance\"");
#else
    return screenshotClipboardAppearanceMimeType();
#endif
}
void ensureScreenshotClipboardAppearanceMimeSupport() {
#if defined(Q_OS_MACOS)
    // Qt owns this converter, shared by every clipboard consumer in the process.
    static const auto* converter = new AppearanceMimeConverter;
    Q_UNUSED(converter);
#endif
}
QByteArray encodeScreenshotClipboardAppearance(const ScreenshotClipboardAppearance& value) {
    // Outline validation may construct shared contour caches. The clipboard owns
    // immutable geometry only; an export worker must release its derived contours.
    const auto releaseContours = qScopeGuard([&value] {
        if (value.borderAppearance && value.borderAppearance->region)
            value.borderAppearance->region->clearDerivedCache();
    });
    if (!value.isValid())
        return {};
    QJsonObject object{
        {QStringLiteral("raster"), QJsonArray{value.rasterSize.width(), value.rasterSize.height()}},
        {QStringLiteral("checkerboard"), value.checkerboardEnabled}};
    if (value.showBorder)
        object.insert(QStringLiteral("show_border"), *value.showBorder);
    if (value.borderAppearance) {
        const auto& a = *value.borderAppearance;
        QJsonObject border{{QStringLiteral("source_size"),
                            QJsonArray{a.sourceSize.width(), a.sourceSize.height()}},
                           {QStringLiteral("content_rect"),
                            QJsonArray{a.contentRect.x(), a.contentRect.y(), a.contentRect.width(),
                                       a.contentRect.height()}},
                           {QStringLiteral("corner_radius"), a.cornerRadius},
                           {QStringLiteral("has_shadow"), a.hasShadow}};
        if (a.region)
            border.insert(QStringLiteral("geometry"), a.region->toJson());
        object.insert(QStringLiteral("border"), border);
    }
    if (!value.filePath.isEmpty())
        object.insert(
            QStringLiteral("file"),
            QJsonObject{{QStringLiteral("path"), value.filePath},
                        {QStringLiteral("size"), QString::number(value.fileSize)},
                        {QStringLiteral("modified"), QString::number(value.fileModifiedMs)}});
    const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (json.size() + kHeaderSize > kScreenshotClipboardAppearanceMaximumBytes)
        return {};
    QByteArray result(kHeaderSize, Qt::Uninitialized);
    qToLittleEndian(kMagic, result.data());
    qToLittleEndian(quint32(1), result.data() + 4);
    qToLittleEndian(static_cast<quint32>(json.size()), result.data() + 8);
    return result + json;
}
std::optional<ScreenshotClipboardAppearance>
decodeScreenshotClipboardAppearance(const QByteArray& bytes) {
    if (bytes.size() < kHeaderSize || bytes.size() > kScreenshotClipboardAppearanceMaximumBytes ||
        qFromLittleEndian<quint32>(bytes.constData()) != kMagic ||
        qFromLittleEndian<quint32>(bytes.constData() + 4) != 1)
        return {};
    const auto length = qFromLittleEndian<quint32>(bytes.constData() + 8);
    if (length > static_cast<quint32>(bytes.size() - kHeaderSize))
        return {};
    const auto doc = QJsonDocument::fromJson(bytes.mid(kHeaderSize, length));
    if (!doc.isObject())
        return {};
    const auto o = doc.object();
    ScreenshotClipboardAppearance result;
    result.rasterSize = parseSize(o[QStringLiteral("raster")]);
    if (!o[QStringLiteral("checkerboard")].isBool())
        return {};
    result.checkerboardEnabled = o[QStringLiteral("checkerboard")].toBool();
    if (o.contains(QStringLiteral("show_border"))) {
        if (!o[QStringLiteral("show_border")].isBool())
            return {};
        result.showBorder = o[QStringLiteral("show_border")].toBool();
    }
    if (o.contains(QStringLiteral("border"))) {
        const auto b = o[QStringLiteral("border")].toObject();
        if (!b[QStringLiteral("corner_radius")].isDouble() ||
            !b[QStringLiteral("has_shadow")].isBool())
            return {};
        snow_shot::storage::PinnedBorderAppearance a{parseSize(b[QStringLiteral("source_size")]),
                                                     parseRect(b[QStringLiteral("content_rect")]),
                                                     b[QStringLiteral("corner_radius")].toDouble(),
                                                     b[QStringLiteral("has_shadow")].toBool(),
                                                     {}};
        if (b.contains(QStringLiteral("geometry"))) {
            a.region = ScreenshotRegionGeometry::fromJson(b[QStringLiteral("geometry")]);
            if (!a.region || a.region->isEmpty())
                return {};
        }
        result.borderAppearance = std::move(a);
    }
    if (o.contains(QStringLiteral("file"))) {
        const auto f = o[QStringLiteral("file")].toObject();
        if (!f[QStringLiteral("path")].isString() || !f[QStringLiteral("size")].isString() ||
            !f[QStringLiteral("modified")].isString())
            return {};
        bool sizeOk = false, timeOk = false;
        result.filePath = f[QStringLiteral("path")].toString();
        result.fileSize = f[QStringLiteral("size")].toString().toLongLong(&sizeOk);
        result.fileModifiedMs = f[QStringLiteral("modified")].toString().toLongLong(&timeOk);
        if (!sizeOk || !timeOk || result.filePath.isEmpty())
            return {};
    }
    const bool valid = result.isValid();
    if (result.borderAppearance && result.borderAppearance->region)
        result.borderAppearance->region->clearDerivedCache();
    return valid ? std::optional(std::move(result)) : std::nullopt;
}
void setScreenshotClipboardAppearance(QMimeData& mime, const ScreenshotClipboardAppearance& value) {
    ensureScreenshotClipboardAppearanceMimeSupport();
    auto bytes = encodeScreenshotClipboardAppearance(value);
    if (!bytes.isEmpty())
        mime.setData(screenshotClipboardAppearanceNativeMimeType(), bytes);
}
std::optional<ScreenshotClipboardAppearance>
readScreenshotClipboardAppearance(const QMimeData* mime) {
    ensureScreenshotClipboardAppearanceMimeSupport();
    return mime ? decodeScreenshotClipboardAppearance(
                      mime->data(screenshotClipboardAppearanceNativeMimeType()))
                : std::nullopt;
}
std::optional<ScreenshotClipboardAppearance>
snapshotScreenshotClipboardAppearance(QClipboard* clipboard) {
    if (!clipboard)
        return {};
#if defined(Q_OS_WIN)
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        if (!OpenClipboard(nullptr))
            return {};
        const auto close = qScopeGuard([] { CloseClipboard(); });
        const auto format = RegisterClipboardFormatW(L"SnowShotScreenshotAppearance");
        const auto handle = static_cast<HGLOBAL>(GetClipboardData(format));
        const auto size = handle ? GlobalSize(handle) : 0;
        if (!size || size > static_cast<SIZE_T>(kScreenshotClipboardAppearanceMaximumBytes))
            return {};
        const void* bytes = GlobalLock(handle);
        if (!bytes)
            return {};
        const auto unlock = qScopeGuard([handle] { GlobalUnlock(handle); });
        return decodeScreenshotClipboardAppearance(
            QByteArray(static_cast<const char*>(bytes), static_cast<qsizetype>(size)));
    }
#endif
    return readScreenshotClipboardAppearance(clipboard->mimeData());
}
