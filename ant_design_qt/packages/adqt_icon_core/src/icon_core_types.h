#ifndef ADQT_ICON_CORE_TYPES_H
#define ADQT_ICON_CORE_TYPES_H

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QMetaType>
#include <QSize>
#include <QString>
#include <Qt>
#include <QtGlobal>

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include "adqt_icon_core_global.h"

namespace adqt::icons {

namespace detail {
struct IconRefAccess;
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
using IconHashValue = uint;
#else
using IconHashValue = size_t;
#endif

inline IconHashValue iconHashCombine(IconHashValue seed, IconHashValue value) {
  return seed ^ (value + static_cast<IconHashValue>(0x9e3779b9u) + (seed << 6) + (seed >> 2));
}

enum class IconColorModel { Monochrome, TwoTone, ThreeTone, FullColor };
enum class IconFit { Contain, Stretch };

// These values are deliberately non-owning. Generated packs keep all text in read-only segments;
// converting a value to QColor/QString happens only at the rendering or inspection boundary.
struct IconStaticColors final {
  std::string_view primary;
  std::string_view secondary;
  std::string_view tertiary;

  constexpr bool isEmpty() const {
    return primary.empty() && secondary.empty() && tertiary.empty();
  }
};

struct ADQT_ICON_CORE_EXPORT IconDescriptor final {
  std::string_view pack;
  std::string_view variant;
  std::string_view name;
  std::string_view svg;
  std::string_view sourceHash;
  IconColorModel colorModel = IconColorModel::Monochrome;
  IconFit fit = IconFit::Contain;
  IconStaticColors defaultColors;
  bool allowEmbeddedDataImages = false;

  constexpr bool isValid() const {
    return !pack.empty() && !variant.empty() && !name.empty() && !svg.empty();
  }
};

class IconColors;
class IconRef;

// A generated pack is an immutable descriptor table. The object itself contains only pointers and
// lengths, so constructing one never copies or normalizes an entire icon set.
struct ADQT_ICON_CORE_EXPORT IconPack final {
  std::string_view packName;
  std::string_view source;
  std::string_view contentHash;
  const IconDescriptor* entries = nullptr;
  std::size_t entryCount = 0;

  constexpr bool isValid() const {
    return !packName.empty() && entries != nullptr && entryCount != 0;
  }
  constexpr std::size_t size() const { return entryCount; }
  const IconDescriptor* entry(std::size_t index) const;
  const IconDescriptor* find(std::string_view variant, std::string_view name) const;
  IconRef icon(std::size_t index) const;
  IconRef icon(std::size_t index, const IconColors& colors) const;
  IconRef icon(std::string_view variant, std::string_view name) const;
  IconRef icon(std::string_view variant, std::string_view name,
               const IconColors& colors) const;
};

using IconPackEntry = IconDescriptor;
using StaticIconPack = IconPack;

class ADQT_ICON_CORE_EXPORT IconColors final {
 public:
  IconColors() = default;

  static IconColors primary(const QColor& color);
  static IconColors twoTone(const QColor& primary, const QColor& secondary);
  static IconColors threeTone(const QColor& primary, const QColor& secondary,
                              const QColor& tertiary);

  IconColors withPrimary(const QColor& color) const;
  IconColors withSecondary(const QColor& color) const;
  IconColors withTertiary(const QColor& color) const;

  std::optional<QColor> primarySlot() const;
  std::optional<QColor> secondarySlot() const;
  std::optional<QColor> tertiarySlot() const;
  bool isEmpty() const { return presentMask_ == 0; }

  friend bool operator==(const IconColors& lhs, const IconColors& rhs) {
    return lhs.presentMask_ == rhs.presentMask_ && lhs.primary_ == rhs.primary_ &&
           lhs.secondary_ == rhs.secondary_ && lhs.tertiary_ == rhs.tertiary_;
  }
  friend bool operator!=(const IconColors& lhs, const IconColors& rhs) { return !(lhs == rhs); }

 private:
  enum SlotMask : quint8 { Primary = 1, Secondary = 2, Tertiary = 4 };
  QRgb primary_ = 0;
  QRgb secondary_ = 0;
  QRgb tertiary_ = 0;
  quint8 presentMask_ = 0;

  friend IconHashValue qHash(const IconColors&, IconHashValue);
};

ADQT_ICON_CORE_EXPORT IconHashValue qHash(const IconColors& value, IconHashValue seed = 0);

struct IconKey final {
  QString pack;
  QString variant;
  QString name;

  bool isValid() const { return !pack.isEmpty() && !variant.isEmpty() && !name.isEmpty(); }
};

inline bool operator==(const IconKey& lhs, const IconKey& rhs) {
  return lhs.pack == rhs.pack && lhs.variant == rhs.variant && lhs.name == rhs.name;
}
inline bool operator!=(const IconKey& lhs, const IconKey& rhs) { return !(lhs == rhs); }
ADQT_ICON_CORE_EXPORT IconHashValue qHash(const IconKey& value, IconHashValue seed = 0);

class IconRenderer;
class ExternalIconPack;

// A reference is two pointers/values and never owns a generated key or SVG buffer. The descriptor
// must either be program-lifetime static data (generated packs) or be owned by an explicitly used
// legacy renderer registration; dynamic references must not outlive that renderer.
class ADQT_ICON_CORE_EXPORT IconRef final {
 public:
  IconRef() = default;

  bool isValid() const { return descriptor_ != nullptr && descriptor_->isValid(); }
  const IconColors& colors() const { return colors_; }
  const IconDescriptor* descriptor() const { return descriptor_; }
  IconRef withColors(const IconColors& colors) const;

  friend bool operator==(const IconRef& lhs, const IconRef& rhs) {
    return lhs.descriptor_ == rhs.descriptor_ && lhs.colors_ == rhs.colors_;
  }
  friend bool operator!=(const IconRef& lhs, const IconRef& rhs) { return !(lhs == rhs); }

 private:
  explicit IconRef(const IconDescriptor* descriptor, IconColors colors)
      : descriptor_(descriptor), colors_(std::move(colors)) {}

  const IconDescriptor* descriptor_ = nullptr;
  IconColors colors_;

  friend class IconPack;
  friend class ExternalIconPack;
  friend class IconRenderer;
  friend struct detail::IconRefAccess;
  friend IconHashValue qHash(const IconRef&, IconHashValue);
};

static_assert(sizeof(IconColors) <= 4 * sizeof(QRgb),
              "IconColors must remain a compact inline value");
static_assert(sizeof(IconRef) <= sizeof(const IconDescriptor*) + 4 * sizeof(QRgb),
              "IconRef must remain a pointer plus compact inline colors");

ADQT_ICON_CORE_EXPORT IconHashValue qHash(const IconRef& value, IconHashValue seed = 0);
inline bool isValid(const IconRef& ref) { return ref.isValid(); }

// A non-owning inspection view. Use this in hot metadata paths; describeIcon() below remains an
// owning compatibility facade for callers that need QString/QByteArray values.
struct IconMetadataView final {
  std::string_view pack;
  std::string_view variant;
  std::string_view name;
  IconColorModel colorModel = IconColorModel::Monochrome;
  IconFit fit = IconFit::Contain;
  IconStaticColors defaultColors;
  std::string_view sourceHash;

  bool isValid() const {
    return !pack.empty() && !variant.empty() && !name.empty() && !sourceHash.empty();
  }
};

struct IconMetadata final {
  IconKey key;
  IconColorModel colorModel = IconColorModel::Monochrome;
  IconFit fit = IconFit::Contain;
  IconColors defaultColors;
  QByteArray sourceHash;

  bool isValid() const { return key.isValid() && !sourceHash.isEmpty(); }
};

inline bool operator==(const IconMetadata& lhs, const IconMetadata& rhs) {
  return lhs.key == rhs.key && lhs.colorModel == rhs.colorModel && lhs.fit == rhs.fit &&
         lhs.defaultColors == rhs.defaultColors && lhs.sourceHash == rhs.sourceHash;
}
inline bool operator!=(const IconMetadata& lhs, const IconMetadata& rhs) { return !(lhs == rhs); }

// Legacy, explicitly dynamic registration input. Generated packs never instantiate this type.
struct IconDefinition final {
  IconKey key;
  IconColorModel colorModel = IconColorModel::Monochrome;
  IconFit fit = IconFit::Contain;
  IconColors defaultColors;
  QByteArray svg;
  QByteArray sourceHash;
  bool allowEmbeddedDataImages = false;

  bool isValid() const { return key.isValid() && !svg.isEmpty(); }
};

enum class IconRegistrationError {
  None,
  InvalidPack,
  InvalidEntry,
  DuplicateKey,
  InvalidSvg,
  ColorModelMismatch,
  HashMismatch,
  ConflictingRegistration,
};

struct IconRegistrationDiagnostic final {
  IconRegistrationError error = IconRegistrationError::None;
  IconKey key;
  QString message;
};

struct IconPackRegistrationResult final {
  int registeredCount = 0;
  int existingCount = 0;
  QList<IconRegistrationDiagnostic> diagnostics;
  bool ok() const { return diagnostics.isEmpty(); }
};

struct IconRenderRequest final {
  QSize logicalSize = QSize(16, 16);
  // A non-positive value lets direct painting derive the target device DPR.
  qreal devicePixelRatio = 0.0;
  QIcon::Mode mode = QIcon::Normal;
  QIcon::State state = QIcon::Off;
  std::optional<IconFit> fit;
  Qt::Alignment alignment = Qt::AlignCenter;
};

class ADQT_ICON_CORE_EXPORT IconStatePalette final {
 public:
  IconStatePalette& set(QIcon::Mode mode, QIcon::State state, const IconColors& colors);
  IconStatePalette with(QIcon::Mode mode, QIcon::State state, const IconColors& colors) const;
  std::optional<IconColors> exact(QIcon::Mode mode, QIcon::State state) const;
  std::optional<IconColors> resolve(QIcon::Mode mode, QIcon::State state) const;
  bool isEmpty() const { return entries_.isEmpty(); }
  quint64 revision() const;

 private:
  static int key(QIcon::Mode mode, QIcon::State state);
  QHash<int, IconColors> entries_;
};

struct IconPalette final {
  QColor text = QColor(QStringLiteral("#1F1F1F"));
  QColor textDisabled = QColor(QStringLiteral("#BFBFBF"));
  QColor primary = QColor(QStringLiteral("#1677FF"));
  QColor twoToneSecondary = QColor(QStringLiteral("#E6F4FF"));
  QColor tertiary = QColor(QStringLiteral("#BAE0FF"));
  quint64 revision = 1;
};

using IconPaletteResolver = std::function<IconPalette()>;

struct IconPixmapRequest final {
  IconRef ref;
  IconRenderRequest render;
  IconStatePalette palette;
};

struct IconCacheStatistics final {
  int entryCount = 0;
  int costKB = 0;
  int limitKB = 2 * 1024;
  quint64 hitCount = 0;
  quint64 missCount = 0;
  quint64 rasterizationCount = 0;
  qint64 costBytes = 0;
  qint64 limitBytes = 2 * 1024 * 1024;
  int maxEntries = 512;
  qint64 maxRasterBytes = 256 * 1024;
  quint64 evictionCount = 0;
  quint64 generation = 1;
  quint64 staleRenderCount = 0;
};

struct IconCacheReclaimReport final {
  qint64 bytesBefore = 0;
  qint64 bytesAfter = 0;
  qint64 reclaimedBytes = 0;
  int entriesBefore = 0;
  int entriesAfter = 0;
  quint64 generation = 0;
};

}  // namespace adqt::icons

Q_DECLARE_METATYPE(adqt::icons::IconColorModel)
Q_DECLARE_METATYPE(adqt::icons::IconFit)
Q_DECLARE_METATYPE(adqt::icons::IconColors)
Q_DECLARE_METATYPE(adqt::icons::IconKey)
Q_DECLARE_METATYPE(adqt::icons::IconRef)
Q_DECLARE_METATYPE(adqt::icons::IconMetadata)

#endif  // ADQT_ICON_CORE_TYPES_H
