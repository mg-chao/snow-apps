#include "icon_registry.h"

#include <QCryptographicHash>
#include <QIconEngine>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QSvgRenderer>
#include <QWaitCondition>
#include <QtMath>

#include <algorithm>
#include <list>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace adqt::icons::detail {

struct IconRefAccess final {
  static const IconDescriptor* descriptor(const IconRef& ref) { return ref.descriptor_; }
  static const IconColors& colors(const IconRef& ref) { return ref.colors_; }
};

struct DynamicIcon final {
  QByteArray pack;
  QByteArray variant;
  QByteArray name;
  QByteArray svg;
  QByteArray sourceHash;
  QByteArray primaryColor;
  QByteArray secondaryColor;
  QByteArray tertiaryColor;
  IconDescriptor descriptor;
  IconMetadata metadata;
};

struct IconCacheKey final {
  const IconDescriptor* descriptor = nullptr;
  QSize physicalSize;
  QIcon::Mode mode = QIcon::Normal;
  QIcon::State state = QIcon::Off;
  IconFit fit = IconFit::Contain;
  int alignment = Qt::AlignCenter;
  QRgb primary = 0;
  QRgb secondary = 0;
  QRgb tertiary = 0;
};

inline bool operator==(const IconCacheKey& lhs, const IconCacheKey& rhs) {
  return lhs.descriptor == rhs.descriptor && lhs.physicalSize == rhs.physicalSize &&
         lhs.mode == rhs.mode && lhs.state == rhs.state && lhs.fit == rhs.fit &&
         lhs.alignment == rhs.alignment && lhs.primary == rhs.primary &&
         lhs.secondary == rhs.secondary && lhs.tertiary == rhs.tertiary;
}

inline IconHashValue qHash(const IconCacheKey& value, IconHashValue seed = 0) {
  seed = iconHashCombine(seed, ::qHash(reinterpret_cast<quintptr>(value.descriptor), 0));
  seed = iconHashCombine(seed, ::qHash(value.physicalSize, 0));
  seed = iconHashCombine(seed, ::qHash(static_cast<int>(value.mode), 0));
  seed = iconHashCombine(seed, ::qHash(static_cast<int>(value.state), 0));
  seed = iconHashCombine(seed, ::qHash(static_cast<int>(value.fit), 0));
  seed = iconHashCombine(seed, ::qHash(value.alignment, 0));
  seed = iconHashCombine(seed, ::qHash(value.primary, 0));
  seed = iconHashCombine(seed, ::qHash(value.secondary, 0));
  return iconHashCombine(seed, ::qHash(value.tertiary, 0));
}

struct CacheKeyHash final {
  std::size_t operator()(const IconCacheKey& value) const noexcept {
    return static_cast<std::size_t>(qHash(value));
  }
};

struct CacheEntry final {
  IconCacheKey key;
  QImage image;
  qint64 cost = 0;
};

struct IconRendererImpl final {
  using CacheList = std::list<CacheEntry>;
  using CacheIterator = CacheList::iterator;

  QMutex mutex;
  QHash<IconKey, std::shared_ptr<DynamicIcon>> dynamicIcons;
  // Static packs are catalogued process-wide, but registration results remain local to each
  // renderer so an isolated renderer can report its own first-use accurately.
  QSet<const IconPack*> registeredStaticPacks;
  IconPaletteResolver resolver;
  CacheList lru;
  std::unordered_map<IconCacheKey, CacheIterator, CacheKeyHash> cache;
  QSet<IconCacheKey> inFlight;
  QWaitCondition cacheReady;

  qint64 cacheLimitBytes = IconRenderer::kDefaultCacheLimitBytes;
  int maxEntries = IconRenderer::kDefaultMaxCacheEntries;
  qint64 maxRasterBytes = IconRenderer::kDefaultMaxRasterBytes;
  qint64 cacheBytes = 0;
  quint64 hitCount = 0;
  quint64 missCount = 0;
  quint64 rasterizationCount = 0;
  quint64 evictionCount = 0;
  quint64 generation = 1;
  quint64 staleRenderCount = 0;
};

struct StaticPackCatalog final {
  QMutex mutex;
  QVector<const IconPack*> packs;
};

StaticPackCatalog& staticPackCatalog() {
  static StaticPackCatalog catalog;
  return catalog;
}

}  // namespace adqt::icons::detail

namespace adqt::icons {
namespace {

using detail::CacheEntry;
using detail::IconCacheKey;
using detail::IconRendererImpl;
using detail::DynamicIcon;

constexpr auto kPrimaryPlaceholder = "__ADQT_SLOT_PRIMARY__";
constexpr auto kSecondaryPlaceholder = "__ADQT_SLOT_SECONDARY__";
constexpr auto kTertiaryPlaceholder = "__ADQT_SLOT_TERTIARY__";

QString stringFromView(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QByteArray bytesFromView(std::string_view value) {
  return QByteArray(value.data(), static_cast<int>(value.size()));
}

QColor colorFromView(std::string_view value) {
  return value.empty() ? QColor() : QColor(stringFromView(value));
}

IconColors colorsFromStatic(const IconStaticColors& colors) {
  IconColors result;
  if (!colors.primary.empty()) result = result.withPrimary(colorFromView(colors.primary));
  if (!colors.secondary.empty()) result = result.withSecondary(colorFromView(colors.secondary));
  if (!colors.tertiary.empty()) result = result.withTertiary(colorFromView(colors.tertiary));
  return result;
}

bool hasSlot(const IconColors& colors, int slot) {
  if (slot == 0) return colors.primarySlot().has_value();
  if (slot == 1) return colors.secondarySlot().has_value();
  return colors.tertiarySlot().has_value();
}

QColor slot(const IconColors& colors, int index) {
  std::optional<QColor> value;
  if (index == 0)
    value = colors.primarySlot();
  else if (index == 1)
    value = colors.secondarySlot();
  else if (index == 2)
    value = colors.tertiarySlot();
  return value.value_or(QColor());
}

bool colorsAllowed(IconColorModel model, const IconColors& colors) {
  if (model == IconColorModel::FullColor) return colors.isEmpty();
  if (model == IconColorModel::Monochrome)
    return !colors.secondarySlot() && !colors.tertiarySlot();
  if (model == IconColorModel::TwoTone) return !colors.tertiarySlot();
  return true;
}

void addDiagnostic(IconPackRegistrationResult& result, IconRegistrationError error,
                   const IconKey& key, const QString& message) {
  result.diagnostics.append(IconRegistrationDiagnostic{error, key, message});
}

QString placeholderForSlot(const QString& slotName) {
  if (slotName.compare(QStringLiteral("secondary"), Qt::CaseInsensitive) == 0)
    return QString::fromLatin1(kSecondaryPlaceholder);
  if (slotName.compare(QStringLiteral("tertiary"), Qt::CaseInsensitive) == 0)
    return QString::fromLatin1(kTertiaryPlaceholder);
  return QString::fromLatin1(kPrimaryPlaceholder);
}

QString svgColor(const QColor& value) {
  return (value.isValid() ? value : QColor(Qt::black)).name(QColor::HexRgb);
}

bool replaceColorAttribute(QString& tag, const QString& name, const QString& placeholder) {
  const QRegularExpression expression(QStringLiteral(R"(\b%1\s*=\s*(['"])([^'"]*)\1)").arg(name),
                                      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = expression.match(tag);
  if (!match.hasMatch() || match.captured(2).compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
    return false;
  tag.replace(match.capturedStart(), match.capturedLength(),
              QStringLiteral("%1=\"%2\"").arg(name, placeholder));
  return true;
}

QByteArray normalizeSvg(const IconDefinition& definition, QString* error) {
  QString svg = QString::fromUtf8(definition.svg);
  if (!svg.contains(QRegularExpression(QStringLiteral(R"(<svg\b)"),
                                       QRegularExpression::CaseInsensitiveOption))) {
    if (error) *error = QStringLiteral("source does not contain an svg root");
    return {};
  }
  const QRegularExpression forbiddenElement(
      QStringLiteral(R"(<\s*(?:script|foreignObject|iframe|animate|animateMotion|animateTransform|set)\b)"),
      QRegularExpression::CaseInsensitiveOption);
  if (svg.contains(forbiddenElement)) {
    if (error) *error = QStringLiteral("SVG contains a forbidden active element");
    return {};
  }
  if (svg.contains(QRegularExpression(QStringLiteral(R"((?:href|src)\s*=\s*['"](?:https?:)?//)"),
                                      QRegularExpression::CaseInsensitiveOption))) {
    if (error) *error = QStringLiteral("external network references are forbidden");
    return {};
  }
  if (svg.contains(QRegularExpression(QStringLiteral(R"(<image\b)"),
                                      QRegularExpression::CaseInsensitiveOption))) {
    if (definition.colorModel != IconColorModel::FullColor ||
        !definition.allowEmbeddedDataImages) {
      if (error)
        *error = QStringLiteral(
            "image elements require an explicitly permitted full-color entry");
      return {};
    }
    const QRegularExpression imageTag(QStringLiteral(R"(<image\b[^>]*>)"),
                                      QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression imageReference(
        QStringLiteral(R"((?:href|src)\s*=\s*(['"])([^'"]*)\1)"),
        QRegularExpression::CaseInsensitiveOption);
    auto tags = imageTag.globalMatch(svg);
    while (tags.hasNext()) {
      const QRegularExpressionMatch tag = tags.next();
      auto references = imageReference.globalMatch(tag.captured());
      while (references.hasNext()) {
        const QRegularExpressionMatch reference = references.next();
        if (!reference.captured(2).startsWith(QStringLiteral("data:"),
                                               Qt::CaseInsensitive)) {
          if (error) *error = QStringLiteral("image elements may only reference data URLs");
          return {};
        }
      }
    }
  }
  if (definition.colorModel == IconColorModel::FullColor) {
    if (svg.contains(QStringLiteral("data-adqt-slot"), Qt::CaseInsensitive) ||
        svg.contains(QStringLiteral("currentColor"), Qt::CaseInsensitive)) {
      if (error) *error = QStringLiteral("full-color icons cannot contain theme slots");
      return {};
    }
    return svg.toUtf8();
  }

  const QRegularExpression slotTag(
      QStringLiteral(R"(<([A-Za-z_:][A-Za-z0-9:._-]*)([^>]*)\bdata-adqt-slot\s*=\s*(['"])(primary|secondary|tertiary)\3([^>]*)>)"),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression slotAttribute(
      QStringLiteral(R"(\s*data-adqt-slot\s*=\s*(['"])(primary|secondary|tertiary)\1)"),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression currentColor(QStringLiteral("currentColor"),
                                        QRegularExpression::CaseInsensitiveOption);
  QString normalized;
  qsizetype cursor = 0;
  auto matches = slotTag.globalMatch(svg);
  while (matches.hasNext()) {
    const auto match = matches.next();
    normalized += svg.mid(cursor, match.capturedStart() - cursor);
    QString tag = match.captured();
    const QString placeholder = placeholderForSlot(match.captured(4));
    tag.remove(slotAttribute);
    bool changed = replaceColorAttribute(tag, QStringLiteral("fill"), placeholder);
    changed = replaceColorAttribute(tag, QStringLiteral("stroke"), placeholder) || changed;
    if (tag.contains(currentColor)) {
      tag.replace(currentColor, placeholder);
      changed = true;
    }
    if (!changed) {
      qsizetype position = tag.lastIndexOf(QLatin1Char('>'));
      if (position > 0 && tag.at(position - 1) == QLatin1Char('/')) --position;
      tag.insert(position, QStringLiteral(" fill=\"") + placeholder + QStringLiteral("\""));
    }
    normalized += tag;
    cursor = match.capturedEnd();
  }
  normalized += svg.mid(cursor);
  normalized.replace(currentColor, QString::fromLatin1(kPrimaryPlaceholder));
  const bool primary = normalized.contains(QString::fromLatin1(kPrimaryPlaceholder));
  const bool secondary = normalized.contains(QString::fromLatin1(kSecondaryPlaceholder));
  const bool tertiary = normalized.contains(QString::fromLatin1(kTertiaryPlaceholder));
  const bool valid = definition.colorModel == IconColorModel::Monochrome
                         ? primary && !secondary && !tertiary
                     : definition.colorModel == IconColorModel::TwoTone
                         ? primary && secondary && !tertiary
                         : primary && secondary && tertiary;
  if (!valid) {
    if (error) *error = QStringLiteral("SVG slots do not match the declared color model");
    return {};
  }
  return normalized.toUtf8();
}

QColor deriveSecondary(const QColor& primary) {
  QColor hsl = (primary.isValid() ? primary : QColor(QStringLiteral("#1677FF"))).toHsl();
  hsl.setHsl(hsl.hslHue(), qMax(8, qRound(hsl.hslSaturation() * 0.22)),
             qMin(245, qRound(hsl.lightness() + (255 - hsl.lightness()) * 0.82)), hsl.alpha());
  return hsl.toRgb();
}

IconPalette resolvedApplicationPalette(const IconPaletteResolver& resolver) {
  IconPalette result = resolver ? resolver() : IconPalette();
  if (!result.text.isValid()) result.text = QColor(QStringLiteral("#1F1F1F"));
  if (!result.textDisabled.isValid()) result.textDisabled = QColor(QStringLiteral("#BFBFBF"));
  if (!result.primary.isValid()) result.primary = QColor(QStringLiteral("#1677FF"));
  if (!result.twoToneSecondary.isValid()) result.twoToneSecondary = deriveSecondary(result.primary);
  if (!result.tertiary.isValid()) result.tertiary = result.twoToneSecondary;
  if (result.revision == 0) result.revision = 1;
  return result;
}

struct ResolvedColors final {
  QColor primary;
  QColor secondary;
  QColor tertiary;
};

ResolvedColors resolveColors(const IconDescriptor& descriptor, const IconRef& ref,
                             const IconStatePalette& statePalette, const IconPalette& app,
                             QIcon::Mode mode, QIcon::State state) {
  ResolvedColors result;
  if (descriptor.colorModel == IconColorModel::FullColor) return result;
  result.primary = descriptor.colorModel == IconColorModel::Monochrome
                       ? (mode == QIcon::Disabled ? app.textDisabled : app.text)
                       : (mode == QIcon::Disabled ? app.textDisabled : app.primary);
  result.secondary = mode == QIcon::Disabled ? deriveSecondary(app.textDisabled)
                                             : app.twoToneSecondary;
  result.tertiary = mode == QIcon::Disabled ? result.secondary : app.tertiary;

  const std::optional<IconColors> stateColors = statePalette.resolve(mode, state);
  const IconColors defaults = colorsFromStatic(descriptor.defaultColors);
  const IconColors& refColors = detail::IconRefAccess::colors(ref);
  for (int index = 0; index < 3; ++index) {
    QColor selected;
    if (stateColors && hasSlot(*stateColors, index))
      selected = slot(*stateColors, index);
    else if (hasSlot(refColors, index))
      selected = slot(refColors, index);
    else if (hasSlot(defaults, index))
      selected = slot(defaults, index);
    if (index == 0 && selected.isValid()) result.primary = selected;
    if (index == 1 && selected.isValid()) result.secondary = selected;
    if (index == 2 && selected.isValid()) result.tertiary = selected;
  }
  const bool hasPrimaryOverride = (stateColors && stateColors->primarySlot()) ||
                                  refColors.primarySlot();
  const bool hasSecondaryColor = (stateColors && stateColors->secondarySlot()) ||
                                 refColors.secondarySlot() || defaults.secondarySlot();
  if (descriptor.colorModel != IconColorModel::Monochrome && hasPrimaryOverride &&
      !hasSecondaryColor)
    result.secondary = deriveSecondary(result.primary);
  return result;
}

QByteArray coloredSvg(const IconDescriptor& descriptor, const ResolvedColors& colors) {
  if (descriptor.colorModel == IconColorModel::FullColor) return bytesFromView(descriptor.svg);
  QString svg = stringFromView(descriptor.svg);
  svg.replace(QString::fromLatin1(kPrimaryPlaceholder), svgColor(colors.primary));
  svg.replace(QString::fromLatin1(kSecondaryPlaceholder), svgColor(colors.secondary));
  svg.replace(QString::fromLatin1(kTertiaryPlaceholder), svgColor(colors.tertiary));
  return svg.toUtf8();
}

QRectF alignedContainedRect(const QSizeF& source, const QRectF& bounds, Qt::Alignment alignment) {
  if (source.isEmpty() || bounds.isEmpty()) return bounds;
  const qreal scale = qMin(bounds.width() / source.width(), bounds.height() / source.height());
  const QSizeF size(source.width() * scale, source.height() * scale);
  qreal x = bounds.left();
  qreal y = bounds.top();
  if (alignment.testFlag(Qt::AlignHCenter))
    x += (bounds.width() - size.width()) / 2.0;
  else if (alignment.testFlag(Qt::AlignRight))
    x += bounds.width() - size.width();
  if (alignment.testFlag(Qt::AlignVCenter))
    y += (bounds.height() - size.height()) / 2.0;
  else if (alignment.testFlag(Qt::AlignBottom))
    y += bounds.height() - size.height();
  return QRectF(QPointF(x, y), size);
}

int physicalDimension(int logical, qreal dpr) {
  const qreal scaled = static_cast<qreal>(logical) * dpr;
  // qRound returns an int; reject values that could overflow during rounding rather than
  // allowing an overflowed small dimension to slip past the raster safety check.
  const qreal maxRounded = static_cast<qreal>(std::numeric_limits<int>::max()) - 0.5;
  if (!qIsFinite(scaled) || scaled >= maxRounded)
    return std::numeric_limits<int>::max();
  return qMax(1, qRound(scaled));
}

qint64 estimatedRasterBytes(const QSize& physical) {
  constexpr qint64 kBytesPerPixel = 4;
  const qint64 width = physical.width();
  const qint64 height = physical.height();
  const qint64 maxValue = std::numeric_limits<qint64>::max();
  if (height > 0 && width > maxValue / height) return maxValue;
  const qint64 pixels = width * height;
  return pixels > maxValue / kBytesPerPixel ? maxValue : pixels * kBytesPerPixel;
}

// Rendering is allowed to produce rasters larger than the cache's per-entry budget, but a
// malformed or untrusted size must never turn into an unbounded QImage allocation. This ceiling
// bounds transient caller-owned images as well as cacheable rasters.
constexpr int kMaxRenderDimension = 16384;
constexpr qint64 kMaxTransientRasterBytes = 64LL * 1024 * 1024;

bool isSafeRasterSize(const QSize& physical) {
  return physical.isValid() && physical.width() > 0 && physical.height() > 0 &&
         physical.width() <= kMaxRenderDimension && physical.height() <= kMaxRenderDimension &&
         estimatedRasterBytes(physical) <= kMaxTransientRasterBytes;
}

IconMetadata metadataFromDescriptor(const IconDescriptor& descriptor) {
  IconMetadata result;
  result.key = {stringFromView(descriptor.pack), stringFromView(descriptor.variant),
                stringFromView(descriptor.name)};
  result.colorModel = descriptor.colorModel;
  result.fit = descriptor.fit;
  result.defaultColors = colorsFromStatic(descriptor.defaultColors);
  result.sourceHash = bytesFromView(descriptor.sourceHash);
  return result;
}

IconMetadataView metadataViewFromDescriptor(const IconDescriptor& descriptor) {
  return {descriptor.pack, descriptor.variant, descriptor.name, descriptor.colorModel,
          descriptor.fit, descriptor.defaultColors, descriptor.sourceHash};
}

const IconDescriptor* findStaticDescriptor(const IconKey& key) {
  auto& catalog = detail::staticPackCatalog();
  QMutexLocker lock(&catalog.mutex);
  const QByteArray packName = key.pack.toUtf8();
  const std::string_view packView(packName.constData(), static_cast<std::size_t>(packName.size()));
  for (const IconPack* pack : catalog.packs) {
    if (pack->packName == packView) {
      const QByteArray variant = key.variant.toUtf8();
      const QByteArray name = key.name.toUtf8();
      if (const IconDescriptor* descriptor =
              pack->find(std::string_view(variant.constData(), static_cast<std::size_t>(variant.size())),
                         std::string_view(name.constData(), static_cast<std::size_t>(name.size()))))
        return descriptor;
    }
  }
  return nullptr;
}

void touchCache(IconRendererImpl& impl, IconRendererImpl::CacheIterator iterator) {
  impl.lru.splice(impl.lru.end(), impl.lru, iterator);
}

void evictFront(IconRendererImpl& impl) {
  if (impl.lru.empty()) return;
  auto iterator = impl.lru.begin();
  impl.cache.erase(iterator->key);
  impl.cacheBytes -= iterator->cost;
  ++impl.evictionCount;
  impl.lru.erase(iterator);
}

void evictTo(IconRendererImpl& impl, qint64 targetBytes) {
  targetBytes = qMax<qint64>(0, targetBytes);
  while (!impl.lru.empty() &&
         (impl.cacheBytes > targetBytes || static_cast<int>(impl.lru.size()) > impl.maxEntries))
    evictFront(impl);
}

void evictRastersAboveLimit(IconRendererImpl& impl) {
  for (auto iterator = impl.lru.begin(); iterator != impl.lru.end();) {
    if (iterator->cost <= impl.maxRasterBytes) {
      ++iterator;
      continue;
    }
    impl.cache.erase(iterator->key);
    impl.cacheBytes -= iterator->cost;
    ++impl.evictionCount;
    iterator = impl.lru.erase(iterator);
  }
}

void releaseEmptyCacheIndex(IconRendererImpl& impl) {
  if (!impl.lru.empty() || !impl.cache.empty()) return;
  decltype(impl.cache) empty;
  impl.cache.swap(empty);
}

int kilobytesFor(qint64 bytes) {
  if (bytes <= 0) return 0;
  const qint64 kilobytes = bytes / 1024 + (bytes % 1024 == 0 ? 0 : 1);
  return kilobytes >= std::numeric_limits<int>::max()
             ? std::numeric_limits<int>::max()
             : static_cast<int>(kilobytes);
}

QImage rasterize(const IconDescriptor& descriptor, const ResolvedColors& colors,
                 const QSize& physical, IconFit fit, Qt::Alignment alignment) {
  QSvgRenderer renderer(coloredSvg(descriptor, colors));
  if (!renderer.isValid()) return {};
  QImage image(physical, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const QRectF bounds(QPointF(0, 0), QSizeF(physical));
  const QRectF target = fit == IconFit::Stretch
                            ? bounds
                            : alignedContainedRect(renderer.viewBoxF().size(), bounds, alignment);
  renderer.render(&painter, target);
  if (descriptor.colorModel == IconColorModel::Monochrome && colors.primary.alpha() < 255) {
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.fillRect(bounds, QColor(0, 0, 0, colors.primary.alpha()));
  }
  return image;
}

QImage renderImage(const std::shared_ptr<IconRendererImpl>& impl, const IconRef& ref,
                   IconRenderRequest request, const IconStatePalette& statePalette) {
  const IconDescriptor* descriptor = detail::IconRefAccess::descriptor(ref);
  if (!descriptor || !descriptor->isValid()) return {};
  if (descriptor->colorModel == IconColorModel::FullColor &&
      (!detail::IconRefAccess::colors(ref).isEmpty() || !statePalette.isEmpty()))
    return {};

  if (!request.logicalSize.isValid() || request.logicalSize.isEmpty()) request.logicalSize = QSize(16, 16);
  const qreal dpr = request.devicePixelRatio > 0.0
                        ? qBound<qreal>(0.25, request.devicePixelRatio, 8.0)
                        : 1.0;
  const QSize physical(physicalDimension(request.logicalSize.width(), dpr),
                       physicalDimension(request.logicalSize.height(), dpr));

  if (!isSafeRasterSize(physical)) return {};

  IconPaletteResolver resolver;
  ResolvedColors colors;
  IconCacheKey key;
  quint64 renderGeneration = 0;
  bool cacheable = false;
  const IconFit requestedFit = request.fit.value_or(descriptor->fit);
  const IconFit fit = requestedFit == IconFit::Stretch ? IconFit::Stretch : IconFit::Contain;
  // A cache invalidation can happen while another thread is rendering this key. Re-snapshot the
  // resolver and generation after waking so a waiter never performs a second stale render using
  // the palette that was current before the invalidation.
  for (;;) {
    {
      QMutexLocker lock(&impl->mutex);
      resolver = impl->resolver;
      renderGeneration = impl->generation;
    }
    const IconPalette app = resolvedApplicationPalette(resolver);
    colors = resolveColors(*descriptor, ref, statePalette, app, request.mode, request.state);
    // Ignore color slots that the descriptor cannot consume. This keeps equivalent monochrome and
    // two-tone requests on one raster instead of retaining duplicate cache entries for irrelevant
    // state overrides.
    const QRgb secondaryKey = descriptor->colorModel == IconColorModel::Monochrome
                                  ? 0
                                  : colors.secondary.rgba();
    const QRgb tertiaryKey = descriptor->colorModel == IconColorModel::ThreeTone
                                 ? colors.tertiary.rgba()
                                 : 0;
    const int alignmentKey = fit == IconFit::Stretch ? static_cast<int>(Qt::AlignCenter)
                                                      : static_cast<int>(request.alignment);
    key = IconCacheKey{descriptor,
                       physical,
                       request.mode,
                       request.state,
                       fit,
                       alignmentKey,
                       colors.primary.rgba(),
                       secondaryKey,
                       tertiaryKey};

    bool retry = false;
    {
      QMutexLocker lock(&impl->mutex);
      const qint64 estimatedCost = estimatedRasterBytes(physical);
      cacheable = impl->cacheLimitBytes > 0 && estimatedCost <= impl->cacheLimitBytes &&
                  estimatedCost <= impl->maxRasterBytes;
      if (cacheable) {
        for (;;) {
          const auto found = impl->cache.find(key);
          if (found != impl->cache.end()) {
            touchCache(*impl, found->second);
            ++impl->hitCount;
            QImage result = found->second->image;
            result.setDevicePixelRatio(dpr);
            return result;
          }
          if (!impl->inFlight.contains(key)) {
            impl->inFlight.insert(key);
            ++impl->missCount;
            break;
          }
          impl->cacheReady.wait(&impl->mutex);
          if (renderGeneration != impl->generation) {
            retry = true;
            break;
          }
        }
      } else {
        ++impl->missCount;
      }
    }
    if (!retry) break;
  }

  QImage image;
  try {
    image = rasterize(*descriptor, colors, physical, fit, request.alignment);
    image.setDevicePixelRatio(dpr);
  } catch (...) {
    // A failed raster allocation must not strand waiters on this key. The caller still receives
    // the original exception, while a later request can retry after the in-flight marker is gone.
    if (cacheable) {
      QMutexLocker lock(&impl->mutex);
      impl->inFlight.remove(key);
      impl->cacheReady.wakeAll();
    }
    throw;
  }

  {
    QMutexLocker lock(&impl->mutex);
    ++impl->rasterizationCount;
    if (cacheable) {
      const bool generationMatches = renderGeneration == impl->generation;
      const qint64 cost = image.isNull() ? 0 : static_cast<qint64>(image.sizeInBytes());
      if (generationMatches && !image.isNull() && cost <= impl->maxRasterBytes &&
          cost <= impl->cacheLimitBytes && impl->cacheLimitBytes > 0) {
        // Cache bookkeeping is best-effort after a successful raster. If the index or list
        // cannot grow, return the caller's image without leaving a half-inserted entry or a
        // stranded in-flight marker.
        auto iterator = impl->lru.end();
        try {
          impl->lru.push_back(CacheEntry{key, image, cost});
          iterator = std::prev(impl->lru.end());
          const auto inserted = impl->cache.emplace(key, iterator);
          if (inserted.second) {
            impl->cacheBytes += cost;
            evictTo(*impl, impl->cacheLimitBytes);
          } else {
            impl->lru.erase(iterator);
          }
        } catch (const std::bad_alloc&) {
          if (iterator != impl->lru.end()) impl->lru.erase(iterator);
        }
      } else if (!generationMatches) {
        ++impl->staleRenderCount;
      }
      impl->inFlight.remove(key);
      impl->cacheReady.wakeAll();
    }
  }
  return image;
}

class RendererIconEngine final : public QIconEngine {
 public:
  RendererIconEngine(std::shared_ptr<IconRendererImpl> impl, IconRef ref, IconStatePalette palette)
      : impl_(std::move(impl)), ref_(std::move(ref)), palette_(std::move(palette)) {}
  QIconEngine* clone() const override { return new RendererIconEngine(impl_, ref_, palette_); }
  QString key() const override { return QStringLiteral("adqt.icon.engine.v3"); }
  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
    IconRenderRequest request;
    request.logicalSize = size;
    request.mode = mode;
    request.state = state;
    return QPixmap::fromImage(renderImage(impl_, ref_, request, palette_));
  }
  QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                       qreal scale) override {
    IconRenderRequest request;
    request.logicalSize = size;
    request.devicePixelRatio = scale;
    request.mode = mode;
    request.state = state;
    return QPixmap::fromImage(renderImage(impl_, ref_, request, palette_));
  }
  void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
    if (!painter || rect.isEmpty()) return;
    IconRenderRequest request;
    request.logicalSize = rect.size();
    request.devicePixelRatio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    request.mode = mode;
    request.state = state;
    const QImage image = renderImage(impl_, ref_, request, palette_);
    if (!image.isNull()) painter->drawImage(rect, image, image.rect());
  }

 private:
  std::shared_ptr<IconRendererImpl> impl_;
  IconRef ref_;
  IconStatePalette palette_;
};

}  // namespace

const IconDescriptor* IconPack::entry(std::size_t index) const {
  return index < entryCount && entries ? &entries[index] : nullptr;
}

const IconDescriptor* IconPack::find(std::string_view variant, std::string_view name) const {
  if (!entries) return nullptr;
  for (std::size_t index = 0; index < entryCount; ++index) {
    const IconDescriptor& candidate = entries[index];
    if (candidate.variant == variant && candidate.name == name) return &candidate;
  }
  return nullptr;
}

IconRef IconPack::icon(std::size_t index) const { return icon(index, IconColors()); }

IconRef IconPack::icon(std::size_t index, const IconColors& colors) const {
  const IconDescriptor* descriptor = entry(index);
  if (!descriptor || !colorsAllowed(descriptor->colorModel, colors)) return {};
  return IconRef(descriptor, colors);
}

IconRef IconPack::icon(std::string_view variant, std::string_view name) const {
  return icon(variant, name, IconColors());
}

IconRef IconPack::icon(std::string_view variant, std::string_view name,
                       const IconColors& colors) const {
  const IconDescriptor* descriptor = find(variant, name);
  if (!descriptor || !colorsAllowed(descriptor->colorModel, colors)) return {};
  return IconRef(descriptor, colors);
}

IconColors IconColors::primary(const QColor& color) { return IconColors().withPrimary(color); }
IconColors IconColors::twoTone(const QColor& primary, const QColor& secondary) {
  return IconColors().withPrimary(primary).withSecondary(secondary);
}
IconColors IconColors::threeTone(const QColor& primary, const QColor& secondary,
                                 const QColor& tertiary) {
  return twoTone(primary, secondary).withTertiary(tertiary);
}
IconColors IconColors::withPrimary(const QColor& color) const {
  IconColors copy = *this;
  copy.primary_ = color.rgba();
  copy.presentMask_ |= Primary;
  return copy;
}
IconColors IconColors::withSecondary(const QColor& color) const {
  IconColors copy = *this;
  copy.secondary_ = color.rgba();
  copy.presentMask_ |= Secondary;
  return copy;
}
IconColors IconColors::withTertiary(const QColor& color) const {
  IconColors copy = *this;
  copy.tertiary_ = color.rgba();
  copy.presentMask_ |= Tertiary;
  return copy;
}

std::optional<QColor> IconColors::primarySlot() const {
  return (presentMask_ & Primary) != 0
             ? std::optional<QColor>(QColor::fromRgba(primary_))
             : std::nullopt;
}

std::optional<QColor> IconColors::secondarySlot() const {
  return (presentMask_ & Secondary) != 0
             ? std::optional<QColor>(QColor::fromRgba(secondary_))
             : std::nullopt;
}

std::optional<QColor> IconColors::tertiarySlot() const {
  return (presentMask_ & Tertiary) != 0
             ? std::optional<QColor>(QColor::fromRgba(tertiary_))
             : std::nullopt;
}

IconHashValue qHash(const IconColors& value, IconHashValue seed) {
  seed = iconHashCombine(seed, ::qHash(value.presentMask_, 0));
  seed = iconHashCombine(seed, ::qHash(value.primary_, 0));
  seed = iconHashCombine(seed, ::qHash(value.secondary_, 0));
  return iconHashCombine(seed, ::qHash(value.tertiary_, 0));
}

IconHashValue qHash(const IconKey& value, IconHashValue seed) {
  seed = iconHashCombine(seed, ::qHash(value.pack, 0));
  seed = iconHashCombine(seed, ::qHash(value.variant, 0));
  return iconHashCombine(seed, ::qHash(value.name, 0));
}

IconHashValue qHash(const IconRef& value, IconHashValue seed) {
  seed = iconHashCombine(seed, ::qHash(reinterpret_cast<quintptr>(value.descriptor_), 0));
  return iconHashCombine(seed, qHash(value.colors_, 0));
}

IconRef IconRef::withColors(const IconColors& colors) const {
  if (!isValid() || !colorsAllowed(descriptor_->colorModel, colors)) return {};
  return IconRef(descriptor_, colors);
}

int IconStatePalette::key(QIcon::Mode mode, QIcon::State state) {
  return static_cast<int>(mode) * 2 + static_cast<int>(state);
}
IconStatePalette& IconStatePalette::set(QIcon::Mode mode, QIcon::State state,
                                        const IconColors& colors) {
  entries_.insert(key(mode, state), colors);
  return *this;
}
IconStatePalette IconStatePalette::with(QIcon::Mode mode, QIcon::State state,
                                        const IconColors& colors) const {
  IconStatePalette copy = *this;
  copy.set(mode, state, colors);
  return copy;
}
std::optional<IconColors> IconStatePalette::exact(QIcon::Mode mode, QIcon::State state) const {
  const auto found = entries_.constFind(key(mode, state));
  return found == entries_.constEnd() ? std::optional<IconColors>() : found.value();
}
std::optional<IconColors> IconStatePalette::resolve(QIcon::Mode mode, QIcon::State state) const {
  const int candidates[] = {key(mode, state), key(mode, QIcon::Off), key(QIcon::Normal, state),
                            key(QIcon::Normal, QIcon::Off)};
  for (int candidate : candidates) {
    const auto found = entries_.constFind(candidate);
    if (found != entries_.constEnd()) return found.value();
  }
  return {};
}
quint64 IconStatePalette::revision() const {
  QList<int> keys = entries_.keys();
  std::sort(keys.begin(), keys.end());
  IconHashValue seed = 0;
  for (int item : keys) {
    seed = iconHashCombine(seed, ::qHash(item, 0));
    seed = iconHashCombine(seed, qHash(entries_.value(item), 0));
  }
  return static_cast<quint64>(seed);
}

IconRenderer::IconRenderer() : impl_(std::make_shared<IconRendererImpl>()) {}
IconRenderer::~IconRenderer() = default;

IconPackRegistrationResult IconRenderer::registerStaticPack(const IconPack& pack) const {
  IconPackRegistrationResult result;
  if (!pack.isValid()) {
    addDiagnostic(result, IconRegistrationError::InvalidPack, {},
                  QStringLiteral("static icon pack is empty or invalid"));
    return result;
  }
  auto& catalog = detail::staticPackCatalog();
  {
    QMutexLocker catalogLock(&catalog.mutex);
    if (!catalog.packs.contains(&pack)) catalog.packs.append(&pack);
  }
  QMutexLocker lock(&impl_->mutex);
  if (impl_->registeredStaticPacks.contains(&pack)) {
    result.existingCount = static_cast<int>(pack.entryCount);
  } else {
    impl_->registeredStaticPacks.insert(&pack);
    result.registeredCount = static_cast<int>(pack.entryCount);
  }
  return result;
}

IconPackRegistrationResult IconRenderer::registerPack(const QString& pack,
                                                      const QList<IconDefinition>& definitions) {
  IconPackRegistrationResult result;
  if (pack.isEmpty() || definitions.isEmpty()) {
    addDiagnostic(result, IconRegistrationError::InvalidPack, {},
                  QStringLiteral("pack name and entries are required"));
    return result;
  }
  QHash<IconKey, std::shared_ptr<DynamicIcon>> pending;
  for (const IconDefinition& definition : definitions) {
    if (!definition.isValid() || definition.key.pack != pack) {
      addDiagnostic(result, IconRegistrationError::InvalidEntry, definition.key,
                    QStringLiteral("invalid entry or pack mismatch"));
      continue;
    }
    if (pending.contains(definition.key)) {
      addDiagnostic(result, IconRegistrationError::DuplicateKey, definition.key,
                    QStringLiteral("duplicate key in pack definition"));
      continue;
    }
    if (!colorsAllowed(definition.colorModel, definition.defaultColors)) {
      addDiagnostic(result, IconRegistrationError::ColorModelMismatch, definition.key,
                    QStringLiteral("default colors do not match the color model"));
      continue;
    }
    QString error;
    const QByteArray normalized = normalizeSvg(definition, &error);
    if (normalized.isEmpty()) {
      addDiagnostic(result, IconRegistrationError::InvalidSvg, definition.key, error);
      continue;
    }
    const QByteArray hash = QCryptographicHash::hash(normalized, QCryptographicHash::Sha256).toHex();
    if (!definition.sourceHash.isEmpty() && definition.sourceHash.toLower() != hash) {
      addDiagnostic(result, IconRegistrationError::HashMismatch, definition.key,
                    QStringLiteral("declared source hash does not match normalized SVG"));
      continue;
    }
    auto stored = std::make_shared<DynamicIcon>();
    stored->pack = definition.key.pack.toUtf8();
    stored->variant = definition.key.variant.toUtf8();
    stored->name = definition.key.name.toUtf8();
    stored->svg = normalized;
    stored->sourceHash = hash;
    const auto primary = definition.defaultColors.primarySlot();
    const auto secondary = definition.defaultColors.secondarySlot();
    const auto tertiary = definition.defaultColors.tertiarySlot();
    stored->primaryColor =
        primary ? primary->name(QColor::HexArgb).toUtf8() : QByteArray();
    stored->secondaryColor =
        secondary ? secondary->name(QColor::HexArgb).toUtf8() : QByteArray();
    stored->tertiaryColor =
        tertiary ? tertiary->name(QColor::HexArgb).toUtf8() : QByteArray();
    stored->descriptor = {
        std::string_view(stored->pack.constData(), static_cast<std::size_t>(stored->pack.size())),
        std::string_view(stored->variant.constData(), static_cast<std::size_t>(stored->variant.size())),
        std::string_view(stored->name.constData(), static_cast<std::size_t>(stored->name.size())),
        std::string_view(stored->svg.constData(), static_cast<std::size_t>(stored->svg.size())),
        std::string_view(stored->sourceHash.constData(),
                         static_cast<std::size_t>(stored->sourceHash.size())),
        definition.colorModel,
        definition.fit,
        {std::string_view(stored->primaryColor.constData(),
                          static_cast<std::size_t>(stored->primaryColor.size())),
         std::string_view(stored->secondaryColor.constData(),
                          static_cast<std::size_t>(stored->secondaryColor.size())),
         std::string_view(stored->tertiaryColor.constData(),
                          static_cast<std::size_t>(stored->tertiaryColor.size()))},
        definition.allowEmbeddedDataImages};
    stored->metadata = metadataFromDescriptor(stored->descriptor);
    pending.insert(definition.key, std::move(stored));
  }
  if (!result.ok()) return result;

  QMutexLocker lock(&impl_->mutex);
  for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
    const auto existing = impl_->dynamicIcons.constFind(it.key());
    if (existing != impl_->dynamicIcons.constEnd() &&
        ((*existing)->metadata != (*it)->metadata || (*existing)->svg != (*it)->svg)) {
      addDiagnostic(result, IconRegistrationError::ConflictingRegistration, it.key(),
                    QStringLiteral("key is already registered with different content"));
    }
    if (existing == impl_->dynamicIcons.constEnd()) {
      if (findStaticDescriptor(it.key()))
        addDiagnostic(result, IconRegistrationError::ConflictingRegistration, it.key(),
                      QStringLiteral("key conflicts with a static icon pack"));
    }
  }
  if (!result.ok()) return result;
  for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
    if (impl_->dynamicIcons.contains(it.key()))
      ++result.existingCount;
    else {
      impl_->dynamicIcons.insert(it.key(), it.value());
      ++result.registeredCount;
    }
  }
  return result;
}

bool IconRenderer::containsIcon(const IconKey& key) const {
  if (!key.isValid()) return false;
  {
    QMutexLocker lock(&impl_->mutex);
    if (impl_->dynamicIcons.contains(key)) return true;
  }
  return findStaticDescriptor(key) != nullptr;
}

IconMetadataView IconRenderer::describeIconView(const IconRef& ref) const {
  const IconDescriptor* descriptor = detail::IconRefAccess::descriptor(ref);
  return descriptor && descriptor->isValid() ? metadataViewFromDescriptor(*descriptor)
                                             : IconMetadataView();
}

IconMetadata IconRenderer::describeIcon(const IconRef& ref) const {
  const IconDescriptor* descriptor = detail::IconRefAccess::descriptor(ref);
  return descriptor && descriptor->isValid() ? metadataFromDescriptor(*descriptor) : IconMetadata();
}

IconMetadata IconRenderer::describeIcon(const IconKey& key) const {
  QMutexLocker lock(&impl_->mutex);
  const auto found = impl_->dynamicIcons.constFind(key);
  if (found != impl_->dynamicIcons.constEnd()) return (*found)->metadata;
  lock.unlock();
  if (const IconDescriptor* descriptor = findStaticDescriptor(key)) return metadataFromDescriptor(*descriptor);
  return {};
}

QList<IconMetadata> IconRenderer::listIcons(const QString& pack, const QString& variant) const {
  QList<IconMetadata> result;
  {
    QMutexLocker lock(&impl_->mutex);
    for (auto it = impl_->dynamicIcons.constBegin(); it != impl_->dynamicIcons.constEnd(); ++it) {
      if ((!pack.isEmpty() && it.key().pack != pack) ||
          (!variant.isEmpty() && it.key().variant != variant))
        continue;
      result.append((*it)->metadata);
    }
  }
  auto& catalog = detail::staticPackCatalog();
  QMutexLocker catalogLock(&catalog.mutex);
  for (const IconPack* staticPack : catalog.packs) {
    if (!pack.isEmpty() && stringFromView(staticPack->packName) != pack) continue;
    for (std::size_t index = 0; index < staticPack->entryCount; ++index) {
      const IconDescriptor& descriptor = staticPack->entries[index];
      if (!variant.isEmpty() && stringFromView(descriptor.variant) != variant) continue;
      result.append(metadataFromDescriptor(descriptor));
    }
  }
  std::sort(result.begin(), result.end(), [](const IconMetadata& lhs, const IconMetadata& rhs) {
    if (lhs.key.pack != rhs.key.pack) return lhs.key.pack < rhs.key.pack;
    if (lhs.key.variant != rhs.key.variant) return lhs.key.variant < rhs.key.variant;
    return lhs.key.name < rhs.key.name;
  });
  return result;
}

IconRef IconRenderer::reference(const IconKey& key, const IconColors& colors) const {
  if (!key.isValid()) return {};
  {
    QMutexLocker lock(&impl_->mutex);
    const auto found = impl_->dynamicIcons.constFind(key);
    if (found != impl_->dynamicIcons.constEnd())
      return colorsAllowed((*found)->descriptor.colorModel, colors)
                 ? IconRef(&(*found)->descriptor, colors)
                 : IconRef();
  }
  if (const IconDescriptor* descriptor = findStaticDescriptor(key))
    return colorsAllowed(descriptor->colorModel, colors) ? IconRef(descriptor, colors) : IconRef();
  return {};
}

QIcon IconRenderer::makeIcon(const IconRef& ref, const IconStatePalette& palette) const {
  const IconDescriptor* descriptor = detail::IconRefAccess::descriptor(ref);
  return descriptor && descriptor->isValid() ? QIcon(new RendererIconEngine(impl_, ref, palette))
                                             : QIcon();
}

QImage IconRenderer::renderIconImage(const IconRef& ref, const IconRenderRequest& request,
                                     const IconStatePalette& palette) const {
  return renderImage(impl_, ref, request, palette);
}

QPixmap IconRenderer::renderIconPixmap(const IconRef& ref, const IconRenderRequest& request,
                                       const IconStatePalette& palette) const {
  return QPixmap::fromImage(renderImage(impl_, ref, request, palette));
}

void IconRenderer::paintIcon(QPainter* painter, const IconRef& ref, const QRectF& rect,
                             const IconRenderRequest& request,
                             const IconStatePalette& palette) const {
  if (!painter || rect.isEmpty()) return;
  IconRenderRequest actual = request;
  actual.logicalSize = rect.size().toSize();
  if (actual.devicePixelRatio <= 0.0)
    actual.devicePixelRatio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
  const QImage image = renderImage(impl_, ref, actual, palette);
  if (!image.isNull()) painter->drawImage(rect, image, image.rect());
}

QCursor IconRenderer::makeCursor(const IconRef& ref, const QSize& logicalSize,
                                 const QPoint& hotSpot, qreal devicePixelRatio) const {
  const IconDescriptor* descriptor = detail::IconRefAccess::descriptor(ref);
  if (!descriptor || !descriptor->isValid() || descriptor->colorModel != IconColorModel::FullColor)
    return {};
  IconRenderRequest request;
  request.logicalSize = logicalSize;
  request.devicePixelRatio = devicePixelRatio;
  const QPixmap pixmap = renderIconPixmap(ref, request, {});
  return pixmap.isNull() ? QCursor() : QCursor(pixmap, hotSpot.x(), hotSpot.y());
}

void IconRenderer::setPaletteResolver(IconPaletteResolver resolver) {
  QMutexLocker lock(&impl_->mutex);
  impl_->resolver = std::move(resolver);
  ++impl_->generation;
  evictTo(*impl_, 0);
  releaseEmptyCacheIndex(*impl_);
  impl_->cacheReady.wakeAll();
}
void IconRenderer::clearPaletteResolver() { setPaletteResolver({}); }

void IconRenderer::setCacheLimitBytes(qint64 bytes) {
  QMutexLocker lock(&impl_->mutex);
  impl_->cacheLimitBytes = qMax<qint64>(0, bytes);
  ++impl_->generation;
  evictTo(*impl_, impl_->cacheLimitBytes);
  releaseEmptyCacheIndex(*impl_);
  impl_->cacheReady.wakeAll();
}

qint64 IconRenderer::cacheLimitBytes() const {
  QMutexLocker lock(&impl_->mutex);
  return impl_->cacheLimitBytes;
}

void IconRenderer::setCacheLimits(qint64 bytes, int maxEntries, qint64 maxRasterBytes) {
  QMutexLocker lock(&impl_->mutex);
  impl_->cacheLimitBytes = qMax<qint64>(0, bytes);
  impl_->maxEntries = qMax(1, maxEntries);
  impl_->maxRasterBytes = qMax<qint64>(1, maxRasterBytes);
  ++impl_->generation;
  evictRastersAboveLimit(*impl_);
  evictTo(*impl_, impl_->cacheLimitBytes);
  releaseEmptyCacheIndex(*impl_);
  impl_->cacheReady.wakeAll();
}

void IconRenderer::setCacheLimitKB(int kb) {
  setCacheLimitBytes(qMax<qint64>(0, static_cast<qint64>(kb) * 1024));
}

IconCacheReclaimReport IconRenderer::trimCache(qint64 targetBytes) {
  QMutexLocker lock(&impl_->mutex);
  IconCacheReclaimReport report;
  report.bytesBefore = impl_->cacheBytes;
  report.entriesBefore = static_cast<int>(impl_->lru.size());
  if (targetBytes < 0) targetBytes = impl_->cacheLimitBytes;
  ++impl_->generation;
  evictTo(*impl_, targetBytes);
  releaseEmptyCacheIndex(*impl_);
  report.bytesAfter = impl_->cacheBytes;
  report.entriesAfter = static_cast<int>(impl_->lru.size());
  report.reclaimedBytes = report.bytesBefore - report.bytesAfter;
  report.generation = impl_->generation;
  impl_->cacheReady.wakeAll();
  return report;
}

IconCacheReclaimReport IconRenderer::trimToBytes(qint64 targetBytes) { return trimCache(targetBytes); }
IconCacheReclaimReport IconRenderer::trimCacheToBytes(qint64 targetBytes) {
  return trimCache(targetBytes);
}
IconCacheReclaimReport IconRenderer::reclaimCache(qint64 targetBytes) {
  return trimCache(targetBytes);
}

void IconRenderer::clearCache() {
  QMutexLocker lock(&impl_->mutex);
  impl_->lru.clear();
  decltype(impl_->cache) emptyCache;
  impl_->cache.swap(emptyCache);
  impl_->cacheBytes = 0;
  impl_->hitCount = impl_->missCount = impl_->rasterizationCount = 0;
  impl_->evictionCount = impl_->staleRenderCount = 0;
  ++impl_->generation;
  impl_->cacheReady.wakeAll();
}

void IconRenderer::prewarm(const QList<IconPixmapRequest>& requests) const {
  for (const auto& request : requests) renderImage(impl_, request.ref, request.render, request.palette);
}

IconCacheStatistics IconRenderer::cacheStatistics() const {
  QMutexLocker lock(&impl_->mutex);
  IconCacheStatistics result;
  result.entryCount = static_cast<int>(impl_->lru.size());
  result.costBytes = impl_->cacheBytes;
  result.limitBytes = impl_->cacheLimitBytes;
  result.costKB = kilobytesFor(impl_->cacheBytes);
  result.limitKB = kilobytesFor(impl_->cacheLimitBytes);
  result.maxEntries = impl_->maxEntries;
  result.maxRasterBytes = impl_->maxRasterBytes;
  result.hitCount = impl_->hitCount;
  result.missCount = impl_->missCount;
  result.rasterizationCount = impl_->rasterizationCount;
  result.evictionCount = impl_->evictionCount;
  result.generation = impl_->generation;
  result.staleRenderCount = impl_->staleRenderCount;
  return result;
}

IconRenderer& defaultRenderer() {
  static IconRenderer instance;
  return instance;
}
IconRenderer& defaultRegistry() { return defaultRenderer(); }
IconMetadataView describeIconView(const IconRef& ref) { return defaultRenderer().describeIconView(ref); }
IconMetadata describeIcon(const IconRef& ref) { return defaultRenderer().describeIcon(ref); }
QList<IconMetadata> listIcons(const QString& pack, const QString& variant) {
  return defaultRenderer().listIcons(pack, variant);
}
QIcon makeIcon(const IconRef& ref, const IconStatePalette& palette) {
  return defaultRenderer().makeIcon(ref, palette);
}
QImage renderIconImage(const IconRef& ref, const IconRenderRequest& request,
                       const IconStatePalette& palette) {
  return defaultRenderer().renderIconImage(ref, request, palette);
}
QPixmap renderIconPixmap(const IconRef& ref, const IconRenderRequest& request,
                         const IconStatePalette& palette) {
  return defaultRenderer().renderIconPixmap(ref, request, palette);
}
void paintIcon(QPainter* painter, const IconRef& ref, const QRectF& rect,
               const IconRenderRequest& request, const IconStatePalette& palette) {
  defaultRenderer().paintIcon(painter, ref, rect, request, palette);
}
QCursor makeCursor(const IconRef& ref, const QSize& logicalSize, const QPoint& hotSpot,
                   qreal devicePixelRatio) {
  return defaultRenderer().makeCursor(ref, logicalSize, hotSpot, devicePixelRatio);
}
void setPaletteResolver(IconPaletteResolver resolver) { defaultRenderer().setPaletteResolver(std::move(resolver)); }
void clearPaletteResolver() { defaultRenderer().clearPaletteResolver(); }
void setCacheLimitBytes(qint64 bytes) { defaultRenderer().setCacheLimitBytes(bytes); }
void setCacheLimits(qint64 bytes, int maxEntries, qint64 maxRasterBytes) {
  defaultRenderer().setCacheLimits(bytes, maxEntries, maxRasterBytes);
}
void setCacheLimitKB(int kb) { defaultRenderer().setCacheLimitKB(kb); }
IconCacheReclaimReport trimIconCache(qint64 targetBytes) { return defaultRenderer().trimCache(targetBytes); }
IconCacheReclaimReport trimCache(qint64 targetBytes) { return defaultRenderer().trimCache(targetBytes); }
IconCacheReclaimReport trimCacheToBytes(qint64 targetBytes) {
  return defaultRenderer().trimCache(targetBytes);
}
IconCacheReclaimReport trimToBytes(qint64 targetBytes) {
  return defaultRenderer().trimToBytes(targetBytes);
}
IconCacheReclaimReport reclaimCache(qint64 targetBytes) {
  return defaultRenderer().reclaimCache(targetBytes);
}
void clearCache() { defaultRenderer().clearCache(); }
void prewarm(const QList<IconPixmapRequest>& requests) { defaultRenderer().prewarm(requests); }
IconCacheStatistics cacheStatistics() { return defaultRenderer().cacheStatistics(); }

}  // namespace adqt::icons
