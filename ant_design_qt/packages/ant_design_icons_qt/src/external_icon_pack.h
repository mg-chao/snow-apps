#ifndef ADQT_EXTERNAL_ICON_PACK_H
#define ADQT_EXTERNAL_ICON_PACK_H

#include "ant_design_icons_qt_global.h"
#include "icon_core.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <memory>

namespace adqt::icons {

// Kept for applications that construct a project-owned pack at runtime. The generator never
// emits this representation; generated packs use IconPack's read-only descriptor table instead.
struct ADQT_ICONS_EXPORT ExternalIconPackEntry final {
  QString variant;
  QString name;
  IconColorModel colorModel = IconColorModel::Monochrome;
  IconFit fit = IconFit::Contain;
  IconColors defaultColors;
  QByteArray svg;
  QByteArray sourceHash;
  bool allowEmbeddedDataImages = false;
};

struct ADQT_ICONS_EXPORT ExternalIconPackDefinition final {
  QString pack;
  QString source;
  QByteArray contentHash;
  QList<ExternalIconPackEntry> entries;
};

class ADQT_ICONS_EXPORT ExternalIconPack final {
 public:
  // Generated packs call this constructor with an immutable descriptor table. No entry data is
  // copied and no validation or registration work is performed until an icon is actually used.
  explicit ExternalIconPack(const IconPack& pack);
  // Legacy dynamic constructor, intentionally retained for isolated tests and migration code.
  explicit ExternalIconPack(ExternalIconPackDefinition definition);
  ~ExternalIconPack();
  ExternalIconPack(const ExternalIconPack&) = delete;
  ExternalIconPack& operator=(const ExternalIconPack&) = delete;

  const ExternalIconPackDefinition& definition() const;
  const IconPack* staticPack() const;
  IconRef icon(std::size_t index, const IconColors& colors = {}) const;
  IconPackRegistrationResult registerWith(IconRenderer& renderer) const;
  IconPackRegistrationResult ensureRegistered() const;
  IconRef icon(const QString& variant, const QString& name, const IconColors& colors = {}) const;
  IconRef icon(IconRenderer& renderer, const QString& variant, const QString& name,
               const IconColors& colors = {}) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace adqt::icons

#endif  // ADQT_EXTERNAL_ICON_PACK_H
