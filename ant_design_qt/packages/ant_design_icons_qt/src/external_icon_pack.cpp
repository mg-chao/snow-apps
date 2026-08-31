#include "external_icon_pack.h"

namespace adqt::icons {

ExternalIconPack::ExternalIconPack(const IconPack& pack) : pack_(&pack) {
  // Catalog registration stores one pack pointer only; entry SVG/key data remains in read-only
  // program segments and is never copied into the renderer.
  defaultRenderer().registerStaticPack(pack);
}

ExternalIconPack::~ExternalIconPack() = default;

const IconPack* ExternalIconPack::staticPack() const { return pack_; }

IconRef ExternalIconPack::icon(std::size_t index, const IconColors& colors) const {
  return pack_->icon(index, colors);
}

IconPackRegistrationResult ExternalIconPack::registerWith(IconRenderer& renderer) const {
  return renderer.registerStaticPack(*pack_);
}

IconPackRegistrationResult ExternalIconPack::ensureRegistered() const {
  return registerWith(defaultRenderer());
}

}  // namespace adqt::icons
