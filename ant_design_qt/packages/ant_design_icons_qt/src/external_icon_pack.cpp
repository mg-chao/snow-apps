#include "external_icon_pack.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QMutex>
#include <QMutexLocker>

namespace adqt::icons {

struct ExternalIconPack::Impl final {
  explicit Impl(const IconPack* value) : staticPack(value) {}
  explicit Impl(ExternalIconPackDefinition value) : definition(std::move(value)) {}

  const IconPack* staticPack = nullptr;
  ExternalIconPackDefinition definition;
  mutable std::unique_ptr<ExternalIconPackDefinition> compatibilityDefinition;
  mutable QMutex mutex;
};

ExternalIconPack::ExternalIconPack(const IconPack& pack)
    : impl_(std::make_unique<Impl>(&pack)) {
  // Catalog registration stores one pack pointer only; entry SVG/key data remains in read-only
  // program segments and is never copied into the renderer.
  defaultRenderer().registerStaticPack(pack);
}

ExternalIconPack::ExternalIconPack(ExternalIconPackDefinition definition)
    : impl_(std::make_unique<Impl>(std::move(definition))) {}

ExternalIconPack::~ExternalIconPack() = default;

const ExternalIconPackDefinition& ExternalIconPack::definition() const {
  QMutexLocker lock(&impl_->mutex);
  if (!impl_->staticPack) return impl_->definition;
  if (!impl_->compatibilityDefinition) {
    auto value = std::make_unique<ExternalIconPackDefinition>();
    value->pack = QString::fromUtf8(impl_->staticPack->packName.data(),
                                    static_cast<int>(impl_->staticPack->packName.size()));
    value->source = QString::fromUtf8(impl_->staticPack->source.data(),
                                      static_cast<int>(impl_->staticPack->source.size()));
    value->contentHash = QByteArray(impl_->staticPack->contentHash.data(),
                                    static_cast<int>(impl_->staticPack->contentHash.size()));
    value->entries.reserve(static_cast<int>(impl_->staticPack->entryCount));
    for (std::size_t index = 0; index < impl_->staticPack->entryCount; ++index) {
      const IconDescriptor& descriptor = impl_->staticPack->entries[index];
      ExternalIconPackEntry entry;
      entry.variant = QString::fromUtf8(descriptor.variant.data(),
                                        static_cast<int>(descriptor.variant.size()));
      entry.name = QString::fromUtf8(descriptor.name.data(),
                                     static_cast<int>(descriptor.name.size()));
      entry.colorModel = descriptor.colorModel;
      entry.fit = descriptor.fit;
      if (!descriptor.defaultColors.primary.empty())
        entry.defaultColors = entry.defaultColors.withPrimary(
            QColor(QString::fromUtf8(descriptor.defaultColors.primary.data(),
                                     static_cast<int>(descriptor.defaultColors.primary.size()))));
      if (!descriptor.defaultColors.secondary.empty())
        entry.defaultColors = entry.defaultColors.withSecondary(
            QColor(QString::fromUtf8(descriptor.defaultColors.secondary.data(),
                                     static_cast<int>(descriptor.defaultColors.secondary.size()))));
      if (!descriptor.defaultColors.tertiary.empty())
        entry.defaultColors = entry.defaultColors.withTertiary(
            QColor(QString::fromUtf8(descriptor.defaultColors.tertiary.data(),
                                     static_cast<int>(descriptor.defaultColors.tertiary.size()))));
      entry.svg = QByteArray(descriptor.svg.data(), static_cast<int>(descriptor.svg.size()));
      entry.sourceHash =
          QByteArray(descriptor.sourceHash.data(), static_cast<int>(descriptor.sourceHash.size()));
      entry.allowEmbeddedDataImages = descriptor.allowEmbeddedDataImages;
      value->entries.append(std::move(entry));
    }
    impl_->compatibilityDefinition = std::move(value);
  }
  return *impl_->compatibilityDefinition;
}

const IconPack* ExternalIconPack::staticPack() const { return impl_->staticPack; }

IconRef ExternalIconPack::icon(std::size_t index, const IconColors& colors) const {
  if (!impl_->staticPack) return {};
  return impl_->staticPack->icon(index, colors);
}

IconPackRegistrationResult ExternalIconPack::registerWith(IconRenderer& renderer) const {
  if (impl_->staticPack) return renderer.registerStaticPack(*impl_->staticPack);

  // Dynamic packs are explicitly opt-in. Keep the definition locked while translating it so a
  // registration does not create a second full SVG/entry copy just to obtain a thread-safe
  // snapshot.
  QMutexLocker lock(&impl_->mutex);
  QList<IconDefinition> definitions;
  definitions.reserve(impl_->definition.entries.size());
  QCryptographicHash packHash(QCryptographicHash::Sha256);
  for (const ExternalIconPackEntry& entry : impl_->definition.entries) {
    IconDefinition definition;
    definition.key = {impl_->definition.pack, entry.variant, entry.name};
    definition.colorModel = entry.colorModel;
    definition.fit = entry.fit;
    definition.defaultColors = entry.defaultColors;
    definition.svg = entry.svg;
    definition.sourceHash = entry.sourceHash;
    definition.allowEmbeddedDataImages = entry.allowEmbeddedDataImages;
    definitions.append(std::move(definition));
    const QByteArray variantUtf8 = entry.variant.toUtf8();
    const QByteArray nameUtf8 = entry.name.toUtf8();
    packHash.addData(QByteArrayView{variantUtf8});
    packHash.addData(QByteArrayView{"\0", 1});
    packHash.addData(QByteArrayView{nameUtf8});
    packHash.addData(QByteArrayView{"\0", 1});
    packHash.addData(QByteArrayView{entry.sourceHash});
    packHash.addData(QByteArrayView{"\n", 1});
  }
  IconPackRegistrationResult result;
  const QByteArray actualPackHash = packHash.result().toHex();
  if (!impl_->definition.contentHash.isEmpty() &&
      impl_->definition.contentHash.toLower() != actualPackHash) {
    result.diagnostics.append({IconRegistrationError::HashMismatch, {},
                               QStringLiteral("external pack content hash mismatch")});
    return result;
  }
  return renderer.registerPack(impl_->definition.pack, definitions);
}

IconPackRegistrationResult ExternalIconPack::ensureRegistered() const {
  return registerWith(defaultRenderer());
}

IconRef ExternalIconPack::icon(const QString& variant, const QString& name,
                               const IconColors& colors) const {
  return icon(defaultRenderer(), variant, name, colors);
}

IconRef ExternalIconPack::icon(IconRenderer& renderer, const QString& variant, const QString& name,
                               const IconColors& colors) const {
  if (impl_->staticPack) {
    const QByteArray variantUtf8 = variant.toUtf8();
    const QByteArray nameUtf8 = name.toUtf8();
    const auto find = impl_->staticPack->find(
        std::string_view(variantUtf8.constData(), static_cast<std::size_t>(variantUtf8.size())),
        std::string_view(nameUtf8.constData(), static_cast<std::size_t>(nameUtf8.size())));
    return find ? IconRef(find, {}).withColors(colors) : IconRef();
  }
  const IconPackRegistrationResult result = registerWith(renderer);
  if (!result.ok()) return {};
  return renderer.reference({impl_->definition.pack, variant, name}, colors);
}

}  // namespace adqt::icons
