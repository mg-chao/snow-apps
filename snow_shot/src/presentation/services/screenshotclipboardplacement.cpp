#include "snow_shot/presentation/screenshotclipboardplacement.h"
#include "../pinned/pinnedplacementgeometry.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QClipboard>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>
#if defined(Q_OS_MACOS)
#include <QUtiMimeConverter>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {
constexpr quint32 kMagic = 0x53534350;
constexpr qsizetype kHeaderSize = 12;
constexpr qint64 kMaximumPixels = 64LL * 1000 * 1000;
bool validRect(const QRect& rect) {
    return rect.width() > 0 && rect.height() > 0 &&
           qint64(rect.x()) + rect.width() <= std::numeric_limits<int>::max() &&
           qint64(rect.y()) + rect.height() <= std::numeric_limits<int>::max();
}
bool validSize(QSize size) {
    return size.width() > 0 && size.height() > 0 &&
           qint64(size.width()) * size.height() <= kMaximumPixels;
}
bool validString(const QString& text) {
    return text.size() <= 512 && !text.contains(QChar::Null);
}
QJsonArray rectJson(const QRect& rect) {
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}
std::optional<int> integer(const QJsonValue& value) {
    const double n = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(n) || std::trunc(n) != n || n < std::numeric_limits<int>::min() ||
        n > std::numeric_limits<int>::max())
        return {};
    return static_cast<int>(n);
}
QRect parseRect(const QJsonValue& value) {
    const auto a = value.toArray();
    if (a.size() != 4)
        return {};
    const auto x = integer(a[0]), y = integer(a[1]), w = integer(a[2]), h = integer(a[3]);
    if (!x || !y || !w || !h || *w <= 0 || *h <= 0 ||
        qint64(*x) + *w > std::numeric_limits<int>::max() ||
        qint64(*y) + *h > std::numeric_limits<int>::max())
        return {};
    return QRect(*x, *y, *w, *h);
}
qsizetype anchor(const ScreenshotClipboardPlacement& value,
                 const QList<ScreenshotClipboardDisplay>& displays) {
    for (qsizetype i = 0; i < displays.size(); ++i)
        if (!value.placement.displaySerial.isEmpty() &&
            displays[i].serial == value.placement.displaySerial)
            return i;
    if (value.anchorDisplayIndex >= 0 && value.anchorDisplayIndex < value.displays.size()) {
        const auto& saved = value.displays[value.anchorDisplayIndex];
        for (qsizetype i = 0; i < displays.size(); ++i)
            if (displays[i].name == value.placement.displayName &&
                displays[i].desktopBounds == saved.desktopBounds &&
                displays[i].nativeBounds == saved.nativeBounds)
                return i;
    }
    for (qsizetype i = 0; i < displays.size(); ++i)
        if (displays[i].name == value.placement.displayName)
            return i;
    return -1;
}
#if defined(Q_OS_MACOS)
class PlacementMimeConverter final : public QUtiMimeConverter {
  public:
    QString mimeForUti(const QString& uti) const override {
        return uti == QStringLiteral("com.snowshot.screenshot-placement")
                   ? screenshotClipboardPlacementMimeType()
                   : QString{};
    }
    QString utiForMime(const QString& mime) const override {
        return mime == screenshotClipboardPlacementMimeType()
                   ? QStringLiteral("com.snowshot.screenshot-placement")
                   : QString{};
    }
    QVariant convertToMime(const QString&, const QList<QByteArray>& data,
                           const QString&) const override {
        return data.size() == 1 && data.first().size() <= kScreenshotClipboardPlacementMaximumBytes
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

bool ScreenshotClipboardPlacement::isValid() const {
    if (!placement.isValid() || !validRect(windowRect) ||
        windowRect.size() != placement.windowSize || !validSize(placement.windowSize) ||
        !validSize(rasterSize) || displays.isEmpty() || displays.size() > 64 ||
        anchorDisplayIndex < 0 || anchorDisplayIndex >= displays.size() ||
        !validString(placement.displayName) || !validString(placement.displaySerial) ||
        placement.units != snow_shot::storage::kPinnedGeometryUnits ||
        std::abs(placement.position.x()) > std::numeric_limits<int>::max() ||
        std::abs(placement.position.y()) > std::numeric_limits<int>::max() ||
        filePath.size() > 4096 || filePath.contains(QChar::Null) ||
        (!filePath.isEmpty() && (fileSize <= 0 || fileModifiedMs < 0)))
        return false;
    for (const auto& display : displays)
        if (!validString(display.name) || !validString(display.serial) ||
            !validRect(display.desktopBounds) || !validRect(display.nativeBounds) ||
            !validRect(display.usableBounds) || !std::isfinite(display.scale) ||
            display.scale < 1 || display.scale > 16)
            return false;
    const auto& display = displays[anchorDisplayIndex];
    if (display.name != placement.displayName || display.serial != placement.displaySerial)
        return false;
    const qreal scale = snow_shot::storage::pinnedGeometryScale(display.scale, placement.units);
    const QPointF expected = QPointF(display.nativeBounds.topLeft()) + placement.position * scale;
    return std::abs(expected.x() - windowRect.x()) < 0.5 &&
           std::abs(expected.y() - windowRect.y()) < 0.5;
}
QString screenshotClipboardFilePath(const QString& path) {
    QFileInfo info(path);
    QString result = info.canonicalFilePath();
    if (result.isEmpty())
        result = QDir::cleanPath(info.absoluteFilePath());
#if defined(Q_OS_WIN)
    result = result.toCaseFolded();
#endif
    return result;
}
bool ScreenshotClipboardPlacement::matchesFile(const QString& path, qint64 size,
                                               qint64 modifiedMs) const {
    return !filePath.isEmpty() && filePath == screenshotClipboardFilePath(path) &&
           fileSize == size && fileModifiedMs == modifiedMs;
}
QString screenshotClipboardPlacementMimeType() {
    return QStringLiteral("application/x-snow-shot-screenshot-placement");
}
QString screenshotClipboardPlacementNativeMimeType() {
#if defined(Q_OS_WIN)
    return QStringLiteral("application/x-qt-windows-mime;value=\"SnowShotScreenshotPlacement\"");
#else
    return screenshotClipboardPlacementMimeType();
#endif
}
void ensureScreenshotClipboardPlacementMimeSupport() {
#if defined(Q_OS_MACOS)
    // Qt owns converters and deletes them when QGuiApplication shuts down.
    static const auto* converter = new PlacementMimeConverter;
    Q_UNUSED(converter);
#endif
}
QByteArray encodeScreenshotClipboardPlacement(const ScreenshotClipboardPlacement& value) {
    if (!value.isValid())
        return {};
    QJsonArray displays;
    for (const auto& d : value.displays)
        displays.append(QJsonObject{{QStringLiteral("name"), d.name},
                                    {QStringLiteral("serial"), d.serial},
                                    {QStringLiteral("desktop"), rectJson(d.desktopBounds)},
                                    {QStringLiteral("native"), rectJson(d.nativeBounds)},
                                    {QStringLiteral("usable"), rectJson(d.usableBounds)},
                                    {QStringLiteral("scale"), d.scale}});
    const auto& p = value.placement;
    QJsonObject object{
        {QStringLiteral("name"), p.displayName},
        {QStringLiteral("serial"), p.displaySerial},
        {QStringLiteral("position"), QJsonArray{p.position.x(), p.position.y()}},
        {QStringLiteral("window"), rectJson(value.windowRect)},
        {QStringLiteral("raster"), QJsonArray{value.rasterSize.width(), value.rasterSize.height()}},
        {QStringLiteral("units"),
         p.units == snow_shot::storage::PinnedGeometryUnits::LogicalPixels ? 0 : 1},
        {QStringLiteral("displays"), displays},
        {QStringLiteral("anchor"), static_cast<int>(value.anchorDisplayIndex)}};
    if (!value.filePath.isEmpty()) {
        object.insert(
            QStringLiteral("file"),
            QJsonObject{{QStringLiteral("path"), value.filePath},
                        {QStringLiteral("size"), QString::number(value.fileSize)},
                        {QStringLiteral("modified"), QString::number(value.fileModifiedMs)}});
    }
    const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (json.size() + kHeaderSize > kScreenshotClipboardPlacementMaximumBytes)
        return {};
    QByteArray result(kHeaderSize, Qt::Uninitialized);
    qToLittleEndian(kMagic, result.data());
    qToLittleEndian(quint32(1), result.data() + 4);
    qToLittleEndian(static_cast<quint32>(json.size()), result.data() + 8);
    return result + json;
}
std::optional<ScreenshotClipboardPlacement>
decodeScreenshotClipboardPlacement(const QByteArray& bytes) {
    if (bytes.size() < kHeaderSize || bytes.size() > kScreenshotClipboardPlacementMaximumBytes ||
        qFromLittleEndian<quint32>(bytes.constData()) != kMagic ||
        qFromLittleEndian<quint32>(bytes.constData() + 4) != 1)
        return {};
    const auto length = qFromLittleEndian<quint32>(bytes.constData() + 8);
    if (length > static_cast<quint32>(bytes.size() - kHeaderSize))
        return {};
    // Native allocators may pad the buffer without initializing the extra bytes.
    // The frame length, rather than that padding, defines the complete payload.
    const auto doc = QJsonDocument::fromJson(bytes.mid(kHeaderSize, length));
    if (!doc.isObject())
        return {};
    const auto o = doc.object();
    ScreenshotClipboardPlacement result;
    auto& p = result.placement;
    if (!o[QStringLiteral("name")].isString() || !o[QStringLiteral("serial")].isString())
        return {};
    p.displayName = o[QStringLiteral("name")].toString();
    p.displaySerial = o[QStringLiteral("serial")].toString();
    const auto position = o[QStringLiteral("position")].toArray();
    if (position.size() != 2 || !position[0].isDouble() || !position[1].isDouble())
        return {};
    p.position = {position[0].toDouble(), position[1].toDouble()};
    result.windowRect = parseRect(o[QStringLiteral("window")]);
    p.windowSize = result.windowRect.size();
    const auto raster = o[QStringLiteral("raster")].toArray();
    if (raster.size() != 2)
        return {};
    const auto w = integer(raster[0]), h = integer(raster[1]);
    if (!w || !h)
        return {};
    result.rasterSize = {*w, *h};
    const auto units = integer(o[QStringLiteral("units")]);
    if (!units || (*units != 0 && *units != 1))
        return {};
    p.units = *units == 0 ? snow_shot::storage::PinnedGeometryUnits::LogicalPixels
                          : snow_shot::storage::PinnedGeometryUnits::PhysicalPixels;
    const auto index = integer(o[QStringLiteral("anchor")]);
    if (!index)
        return {};
    result.anchorDisplayIndex = *index;
    const auto displays = o[QStringLiteral("displays")].toArray();
    if (displays.isEmpty() || displays.size() > 64)
        return {};
    for (const auto& item : displays) {
        const auto d = item.toObject();
        if (!d[QStringLiteral("name")].isString() || !d[QStringLiteral("serial")].isString() ||
            !d[QStringLiteral("scale")].isDouble())
            return {};
        result.displays.append(
            {d[QStringLiteral("name")].toString(), d[QStringLiteral("serial")].toString(),
             parseRect(d[QStringLiteral("desktop")]), parseRect(d[QStringLiteral("native")]),
             parseRect(d[QStringLiteral("usable")]), d[QStringLiteral("scale")].toDouble()});
    }
    if (o.contains(QStringLiteral("file"))) {
        const auto f = o[QStringLiteral("file")].toObject();
        bool sizeOk = false, timeOk = false;
        if (!f[QStringLiteral("path")].isString() || !f[QStringLiteral("size")].isString() ||
            !f[QStringLiteral("modified")].isString())
            return {};
        result.filePath = f[QStringLiteral("path")].toString();
        result.fileSize = f[QStringLiteral("size")].toString().toLongLong(&sizeOk);
        result.fileModifiedMs = f[QStringLiteral("modified")].toString().toLongLong(&timeOk);
        if (!sizeOk || !timeOk || result.filePath.isEmpty())
            return {};
    }
    return result.isValid() ? std::optional(result) : std::nullopt;
}
void setScreenshotClipboardPlacement(QMimeData& mime, const ScreenshotClipboardPlacement& value) {
    ensureScreenshotClipboardPlacementMimeSupport();
    auto bytes = encodeScreenshotClipboardPlacement(value);
    if (!bytes.isEmpty())
        mime.setData(screenshotClipboardPlacementNativeMimeType(), bytes);
}
std::optional<ScreenshotClipboardPlacement>
readScreenshotClipboardPlacement(const QMimeData* mime) {
    ensureScreenshotClipboardPlacementMimeSupport();
    return mime ? decodeScreenshotClipboardPlacement(
                      mime->data(screenshotClipboardPlacementNativeMimeType()))
                : std::nullopt;
}
std::optional<ScreenshotClipboardPlacement>
snapshotScreenshotClipboardPlacement(QClipboard* clipboard) {
    if (!clipboard)
        return {};
#if defined(Q_OS_WIN)
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        if (!OpenClipboard(nullptr))
            return {};
        const auto close = qScopeGuard([] { CloseClipboard(); });
        const auto format = RegisterClipboardFormatW(L"SnowShotScreenshotPlacement");
        const auto handle = static_cast<HGLOBAL>(GetClipboardData(format));
        const auto size = handle ? GlobalSize(handle) : 0;
        if (!size || size > static_cast<SIZE_T>(kScreenshotClipboardPlacementMaximumBytes))
            return {};
        const void* bytes = GlobalLock(handle);
        if (!bytes)
            return {};
        const auto unlock = qScopeGuard([handle] { GlobalUnlock(handle); });
        return decodeScreenshotClipboardPlacement(
            QByteArray(static_cast<const char*>(bytes), static_cast<qsizetype>(size)));
    }
#endif
    return readScreenshotClipboardPlacement(clipboard->mimeData());
}
#if !defined(Q_OS_MACOS)
quint64 screenshotClipboardRevision() {
#if defined(Q_OS_WIN)
    return GetClipboardSequenceNumber();
#else
    return 0;
#endif
}
#endif

ScreenshotClipboardResolvedPlacement
resolveScreenshotClipboardPlacement(const ScreenshotClipboardPlacement& value,
                                    const QList<ScreenshotClipboardDisplay>& displays,
                                    bool autoResizeWindow) {
    if (!value.isValid() || displays.isEmpty())
        return {};
    qsizetype target = anchor(value, displays);
    bool unchanged = target >= 0 && displays.size() == value.displays.size();
    for (const auto& saved : value.displays)
        unchanged = unchanged && displays.contains(saved);
    if (unchanged)
        return {target, value.windowRect, value.placement.windowSize};
    const auto& saved = value.displays[value.anchorDisplayIndex];
    const QPointF original = QPointF(saved.desktopBounds.topLeft()) + value.placement.position;
    if (target < 0) {
        double nearest = std::numeric_limits<double>::infinity();
        const qreal oldScale =
            snow_shot::storage::pinnedGeometryScale(saved.scale, value.placement.units);
        const QRectF originalRect(original, QSizeF(value.placement.windowSize) / oldScale);
        for (qsizetype i = 0; i < displays.size(); ++i) {
            const QRectF bounds(displays[i].desktopBounds);
            const double dx = std::max(
                {bounds.left() - originalRect.right(), 0.0, originalRect.left() - bounds.right()});
            const double dy = std::max(
                {bounds.top() - originalRect.bottom(), 0.0, originalRect.top() - bounds.bottom()});
            const double distance = dx * dx + dy * dy;
            if (distance < nearest) {
                target = i;
                nearest = distance;
            }
        }
    }
    if (target < 0)
        return {};
    const auto& d = displays[target];
    if (!validRect(d.desktopBounds) || !validRect(d.nativeBounds) || !validRect(d.usableBounds) ||
        !std::isfinite(d.scale) || d.scale < 1 || d.scale > 16)
        return {};
    const qreal scale = snow_shot::storage::pinnedGeometryScale(d.scale, value.placement.units);
    QSize size = value.placement.windowSize;
    const QSizeF available = QSizeF(d.usableBounds.size()) * scale;
    if (autoResizeWindow) {
        const qreal fit =
            std::min({1.0, available.width() / size.width(), available.height() / size.height()});
        size = {std::max(1, qRound(size.width() * fit)), std::max(1, qRound(size.height() * fit))};
    }
    const QPointF desired = anchor(value, displays) >= 0
                                ? QPointF(d.desktopBounds.topLeft()) + value.placement.position
                                : original;
    auto recovered = value.placement;
    recovered.windowSize = size;
    recovered.position = desired - QPointF(d.desktopBounds.topLeft());
    recovered = snow_shot::presentation::recoverPinnedPlacement(
        std::move(recovered),
        {d.name, d.serial, QRectF(d.desktopBounds), QRectF(d.usableBounds), d.scale});
    const qreal nativeX = d.nativeBounds.x() + recovered.position.x() * scale;
    const qreal nativeY = d.nativeBounds.y() + recovered.position.y() * scale;
    if (!std::isfinite(nativeX) || !std::isfinite(nativeY) ||
        nativeX < std::numeric_limits<int>::min() || nativeY < std::numeric_limits<int>::min() ||
        nativeX > qreal(std::numeric_limits<int>::max()) - size.width() ||
        nativeY > qreal(std::numeric_limits<int>::max()) - size.height())
        return {};
    return {target, QRect(QPoint(qRound(nativeX), qRound(nativeY)), size),
            value.placement.windowSize};
}
