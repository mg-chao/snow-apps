#ifndef ADQT_EXTERNAL_ICON_PACK_H
#define ADQT_EXTERNAL_ICON_PACK_H

#include "ant_design_icons_qt_global.h"
#include "icon_core.h"

namespace adqt::icons {

// Wraps a project-owned generated pack. The generator emits the immutable IconPack descriptor
// table; this adapter catalogues it with the default renderer on first use.
class ADQT_ICONS_EXPORT ExternalIconPack final {
 public:
  // No entry data is copied and no validation or registration work is performed until an icon is
  // actually used.
  explicit ExternalIconPack(const IconPack& pack);
  ~ExternalIconPack();
  ExternalIconPack(const ExternalIconPack&) = delete;
  ExternalIconPack& operator=(const ExternalIconPack&) = delete;

  const IconPack* staticPack() const;
  IconRef icon(std::size_t index, const IconColors& colors = {}) const;
  IconPackRegistrationResult registerWith(IconRenderer& renderer) const;
  IconPackRegistrationResult ensureRegistered() const;

 private:
  const IconPack* pack_ = nullptr;
};

}  // namespace adqt::icons

#endif  // ADQT_EXTERNAL_ICON_PACK_H
