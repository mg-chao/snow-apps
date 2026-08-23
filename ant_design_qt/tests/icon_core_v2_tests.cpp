#include "antd_icons.h"
#include "external_icon_pack.h"
#include "icon_registry.h"
#include "icons/widget_icons.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QSet>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QRect alphaBounds(const QImage& image) {
  QRect bounds;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixelColor(x, y).alpha() > 0) bounds |= QRect(x, y, 1, 1);
    }
  }
  return bounds;
}

bool containsOpaqueColor(const QImage& image, const QColor& expected) {
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor actual = image.pixelColor(x, y);
      if (actual.alpha() == 255 && actual.rgb() == expected.rgb()) return true;
    }
  }
  return false;
}

adqt::icons::IconDefinition definition(const QString& name, const QByteArray& svg) {
  adqt::icons::IconDefinition value;
  value.key = {QStringLiteral("core-test"), QStringLiteral("outlined"), name};
  value.colorModel = adqt::icons::IconColorModel::Monochrome;
  value.svg = svg;
  return value;
}

QByteArray squareSvg() {
  return QByteArrayLiteral(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\">"
      "<rect x=\"1\" y=\"1\" width=\"14\" height=\"14\" fill=\"currentColor\"/>"
      "</svg>");
}

adqt::icons::ExternalIconPackDefinition externalDefinition() {
  adqt::icons::ExternalIconPackDefinition value;
  value.pack = QStringLiteral("external-test");
  value.source = QStringLiteral("icon core v2 tests");
  value.entries.append(
      {QStringLiteral("outlined"),
       QStringLiteral("wide"),
       adqt::icons::IconColorModel::Monochrome,
       adqt::icons::IconFit::Contain,
       {},
       QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 10\">"
                         "<rect width=\"20\" height=\"10\" fill=\"currentColor\"/>"
                         "</svg>"),
       {},
       false});
  value.entries.append(
      {QStringLiteral("app"),
       QStringLiteral("full-color"),
       adqt::icons::IconColorModel::FullColor,
       adqt::icons::IconFit::Contain,
       {},
       QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">"
                         "<rect width=\"10\" height=\"10\" fill=\"#E53935\"/>"
                         "</svg>"),
       {},
       false});
  value.entries.append(
      {QStringLiteral("twotone"),
       QStringLiteral("fixed-secondary"),
       adqt::icons::IconColorModel::TwoTone,
       adqt::icons::IconFit::Contain,
       adqt::icons::IconColors().withSecondary(QColor("#9254DE")),
       QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 10\">"
                         "<rect width=\"10\" height=\"10\" fill=\"currentColor\"/>"
                         "<rect x=\"10\" width=\"10\" height=\"10\" data-adqt-slot=\"secondary\"/>"
                         "</svg>"),
       {},
       false});
  return value;
}

void registrationIsAtomicIdempotentAndConflictAware() {
  adqt::icons::IconRegistry registry;
  const auto valid = definition(QStringLiteral("valid"), squareSvg());
  const auto invalid = definition(QStringLiteral("invalid"), QByteArrayLiteral("not svg"));
  const auto failed = registry.registerPack(QStringLiteral("core-test"), {valid, invalid});
  require(!failed.ok(), "an invalid pack should fail validation");
  require(!registry.containsIcon(valid.key),
          "failed pack registration must not commit valid siblings");

  const auto first = registry.registerPack(QStringLiteral("core-test"), {valid});
  require(first.ok() && first.registeredCount == 1, "first valid registration should commit");
  const auto repeated = registry.registerPack(QStringLiteral("core-test"), {valid});
  require(repeated.ok() && repeated.existingCount == 1,
          "identical registration should be idempotent");

  const auto changed = definition(
      QStringLiteral("valid"),
      QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\">"
                        "<circle cx=\"8\" cy=\"8\" r=\"7\" fill=\"currentColor\"/>"
                        "</svg>"));
  const auto conflict = registry.registerPack(QStringLiteral("core-test"), {changed});
  bool hasConflictDiagnostic = false;
  for (const auto& diagnostic : conflict.diagnostics) {
    hasConflictDiagnostic =
        hasConflictDiagnostic ||
        diagnostic.error == adqt::icons::IconRegistrationError::ConflictingRegistration;
  }
  require(!conflict.ok() && hasConflictDiagnostic,
          "conflicting content should return a structured diagnostic");
}

void referencesAreImmutableValuesAndSupportIsolatedRegistries() {
  adqt::icons::ExternalIconPack pack(externalDefinition());
  adqt::icons::IconRegistry registry;
  const auto registered = pack.registerWith(registry);
  require(registered.ok() && registered.registeredCount == 3,
          "external pack should register into an isolated registry");
  const auto ref = pack.icon(registry, QStringLiteral("outlined"), QStringLiteral("wide"));
  const auto red = ref.withColors(adqt::icons::IconColors::primary(QColor(Qt::red)));
  require(ref.isValid() && red.isValid() && ref != red,
          "withColors should derive a distinct immutable value");
  QSet<adqt::icons::IconRef> values{ref, ref, red};
  require(values.size() == 2, "reference equality and hashing should use identity and colors");
  const auto metadata = registry.describeIcon(ref);
  require(metadata.key.pack == QStringLiteral("external-test") &&
              metadata.key.variant == QStringLiteral("outlined") &&
              metadata.key.name == QStringLiteral("wide"),
          "metadata inspection should expose the canonical key");
}

void primaryOverridesPreserveFixedDefaultSecondaryColors() {
  adqt::icons::ExternalIconPack pack(externalDefinition());
  adqt::icons::IconRegistry registry;
  const QColor primary("#345678");
  const QColor fixedSecondary("#9254DE");
  const auto ref = pack.icon(registry, QStringLiteral("twotone"), QStringLiteral("fixed-secondary"),
                             adqt::icons::IconColors::primary(primary));
  adqt::icons::IconRenderRequest request;
  request.logicalSize = QSize(20, 10);
  request.devicePixelRatio = 1.0;
  const QImage image = registry.renderIconPixmap(ref, request).toImage();
  require(containsOpaqueColor(image, primary),
          "an explicit primary color should still override the primary slot");
  require(containsOpaqueColor(image, fixedSecondary),
          "a primary override should preserve a pack's fixed default secondary color");

  adqt::icons::IconStatePalette palette;
  palette.set(QIcon::Normal, QIcon::Off, adqt::icons::IconColors::primary(primary));
  const auto uncoloredRef =
      pack.icon(registry, QStringLiteral("twotone"), QStringLiteral("fixed-secondary"));
  const QImage stateImage = registry.renderIconPixmap(uncoloredRef, request, palette).toImage();
  require(containsOpaqueColor(stateImage, primary),
          "a state primary color should override the primary slot");
  require(containsOpaqueColor(stateImage, fixedSecondary),
          "a state primary override should preserve a pack's fixed default secondary color");
}

void dynamicDefaultColorsPreserveAlpha() {
  adqt::icons::IconRegistry registry;
  auto value = definition(QStringLiteral("translucent-default"), squareSvg());
  value.defaultColors = adqt::icons::IconColors::primary(QColor(12, 34, 56, 73));
  const auto registered = registry.registerPack(QStringLiteral("core-test"), {value});
  require(registered.ok(), "the translucent dynamic fixture should register");

  adqt::icons::IconRenderRequest request;
  request.logicalSize = QSize(16, 16);
  request.devicePixelRatio = 1.0;
  const QImage image = registry.renderIconImage(registry.reference(value.key), request);
  require(!image.isNull() && image.pixelColor(8, 8).alpha() == 73,
          "dynamic descriptor defaults should preserve their alpha channel");
}

void dynamicImagePermissionIsStrict() {
  adqt::icons::IconRegistry registry;
  auto monochrome = definition(
      QStringLiteral("embedded-monochrome"),
      QByteArrayLiteral("<svg viewBox=\"0 0 16 16\"><image href=\"data:image/png;base64,AA==\"/></svg>"));
  monochrome.allowEmbeddedDataImages = true;
  require(!registry.registerPack(QStringLiteral("core-test"), {monochrome}).ok(),
          "embedded images must remain restricted to explicitly permitted full-color entries");

  auto local = definition(
      QStringLiteral("local-image"),
      QByteArrayLiteral("<svg viewBox=\"0 0 16 16\"><image href=\"local.png\"/></svg>"));
  local.colorModel = adqt::icons::IconColorModel::FullColor;
  local.allowEmbeddedDataImages = true;
  require(!registry.registerPack(QStringLiteral("core-test"), {local}).ok(),
          "permitted embedded images must still reject local file references");
}

void lazyRegistrationIsThreadSafe() {
  static const adqt::icons::ExternalIconPack pack([] {
    auto value = externalDefinition();
    value.pack = QStringLiteral("lazy-thread-test");
    return value;
  }());
  constexpr int threadCount = 12;
  std::atomic_int ready{0};
  std::atomic_bool start{false};
  std::vector<adqt::icons::IconRef> refs(threadCount);
  std::vector<std::thread> threads;
  threads.reserve(threadCount);
  for (int index = 0; index < threadCount; ++index) {
    threads.emplace_back([&, index] {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      refs[index] = pack.icon(QStringLiteral("outlined"), QStringLiteral("wide"));
    });
  }
  while (ready.load(std::memory_order_acquire) != threadCount) std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();
  for (const auto& ref : refs)
    require(ref.isValid() && ref == refs.front(),
            "concurrent lazy references should be valid and equal");
  const auto repeated = pack.ensureRegistered();
  require(repeated.ok(), "repeated lazy registration should remain successful");
}

void statePaletteUsesTheDocumentedFallbackOrder() {
  using adqt::icons::IconColors;
  adqt::icons::IconStatePalette palette;
  const auto normalOff = IconColors::primary(QColor(Qt::black));
  const auto normalOn = IconColors::primary(QColor(Qt::green));
  const auto activeOff = IconColors::primary(QColor(Qt::blue));
  const auto selectedOn = IconColors::primary(QColor(Qt::red));
  palette.set(QIcon::Normal, QIcon::Off, normalOff)
      .set(QIcon::Normal, QIcon::On, normalOn)
      .set(QIcon::Active, QIcon::Off, activeOff)
      .set(QIcon::Selected, QIcon::On, selectedOn);
  require(palette.resolve(QIcon::Selected, QIcon::On) == selectedOn, "exact state should win");
  require(palette.resolve(QIcon::Active, QIcon::On) == activeOff,
          "same-mode Off should precede Normal On");
  require(palette.resolve(QIcon::Disabled, QIcon::On) == normalOn,
          "Normal with the same state should precede Normal Off");
  require(palette.resolve(QIcon::Disabled, QIcon::Off) == normalOff,
          "Normal Off should be the final state-palette fallback");
}

void renderingContainsAndPreservesFractionalDpr() {
  adqt::icons::ExternalIconPack pack(externalDefinition());
  adqt::icons::IconRegistry registry;
  const auto ref = pack.icon(registry, QStringLiteral("outlined"), QStringLiteral("wide"),
                             adqt::icons::IconColors::primary(QColor(Qt::black)));
  adqt::icons::IconRenderRequest request;
  request.logicalSize = QSize(20, 20);
  for (const qreal dpr : {1.0, 1.25, 1.5, 2.0, 3.0}) {
    request.devicePixelRatio = dpr;
    const QPixmap pixmap = registry.renderIconPixmap(ref, request);
    const int physicalSize = qRound(20 * dpr);
    require(pixmap.size() == QSize(physicalSize, physicalSize) &&
                qFuzzyCompare(pixmap.devicePixelRatio(), dpr),
            "DPR should preserve physical and logical dimensions");
    const QImage image = pixmap.toImage();
    const QRect bounds = alphaBounds(image);
    require(bounds.width() == physicalSize && bounds.height() < physicalSize &&
                bounds.center().y() == image.rect().center().y(),
            "Contain should center a non-square SVG without distortion");
  }

  const auto fullColor = pack.icon(registry, QStringLiteral("app"), QStringLiteral("full-color"));
  const auto invalid = pack.icon(registry, QStringLiteral("app"), QStringLiteral("full-color"),
                                 adqt::icons::IconColors::primary(QColor(Qt::blue)));
  require(fullColor.isValid() && !invalid.isValid(),
          "full-color entries should reject theme slots");
}

void directPaintingUsesTheEntireHighDpiPixmap() {
  adqt::icons::ExternalIconPack pack(externalDefinition());
  adqt::icons::IconRegistry registry;
  const auto ref = pack.icon(registry, QStringLiteral("outlined"), QStringLiteral("wide"),
                             adqt::icons::IconColors::primary(QColor(Qt::black)));
  const QRectF targetRect(8.0, 8.0, 20.0, 20.0);
  for (const qreal dpr : {1.25, 1.5, 2.0}) {
    const int physicalCanvasSide = qRound(36.0 * dpr);
    const QSize physicalCanvasSize(physicalCanvasSide, physicalCanvasSide);
    QImage actual(physicalCanvasSize, QImage::Format_ARGB32_Premultiplied);
    actual.setDevicePixelRatio(dpr);
    actual.fill(Qt::transparent);
    {
      QPainter painter(&actual);
      registry.paintIcon(&painter, ref, targetRect);
    }

    adqt::icons::IconRenderRequest request;
    request.logicalSize = targetRect.size().toSize();
    request.devicePixelRatio = dpr;
    const QPixmap expectedPixmap = registry.renderIconPixmap(ref, request);
    QImage expected(physicalCanvasSize, QImage::Format_ARGB32_Premultiplied);
    expected.setDevicePixelRatio(dpr);
    expected.fill(Qt::transparent);
    {
      QPainter painter(&expected);
      painter.drawPixmap(targetRect.topLeft(), expectedPixmap);
    }

    require(
        actual == expected,
        "direct painting should draw the entire DPR-aware pixmap without cropping or enlargement");
  }
}

void renderPackEntries(const adqt::icons::ExternalIconPack& pack,
                       adqt::icons::IconRegistry& registry) {
  const auto registered = pack.registerWith(registry);
  require(registered.ok(), "generated pack registration should succeed");
  const adqt::icons::IconPack* staticPack = pack.staticPack();
  require(staticPack != nullptr && staticPack->isValid() && staticPack->entryCount != 0,
          "generated pack should contain immutable descriptor entries");

  adqt::icons::IconRenderRequest request;
  request.logicalSize = QSize(16, 16);
  request.devicePixelRatio = 1.25;
  for (std::size_t index = 0; index < staticPack->entryCount; ++index) {
    const adqt::icons::IconDescriptor* entry = staticPack->entry(index);
    const auto ref = pack.icon(index);
    require(ref.isValid(), "every generated entry should create a valid reference");
    const auto metadata = registry.describeIconView(ref);
    require(metadata.pack == staticPack->packName && metadata.variant == entry->variant &&
                metadata.name == entry->name && metadata.sourceHash == entry->sourceHash,
            "generated entry metadata should match its pack definition");
    const QPixmap pixmap = registry.renderIconPixmap(ref, request);
    require(!pixmap.isNull() && pixmap.size() == QSize(20, 20) &&
                qFuzzyCompare(pixmap.devicePixelRatio(), 1.25),
            "every generated entry should render at fractional DPR");
    require(!alphaBounds(pixmap.toImage()).isEmpty(),
            "every generated entry should render nonblank alpha bounds");
  }
}

void everyBuiltInAndWidgetEntryRenders() {
  adqt::icons::IconRegistry registry;
  renderPackEntries(adqt::icons::antd::pack(), registry);
  renderPackEntries(adqt::widgets::icons::pack(), registry);
  require(adqt::icons::antd::pack().staticPack()->entryCount == 829,
          "the pinned built-in Ant pack should contain exactly 829 upstream entries");
  require(adqt::widgets::icons::pack().staticPack()->entryCount == 1,
          "the widget-owned pack should contain only empty-simple");
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    registrationIsAtomicIdempotentAndConflictAware();
    referencesAreImmutableValuesAndSupportIsolatedRegistries();
    primaryOverridesPreserveFixedDefaultSecondaryColors();
    dynamicDefaultColorsPreserveAlpha();
    dynamicImagePermissionIsStrict();
    lazyRegistrationIsThreadSafe();
    statePaletteUsesTheDocumentedFallbackOrder();
    renderingContainsAndPreservesFractionalDpr();
    directPaintingUsesTheEntireHighDpiPixmap();
    everyBuiltInAndWidgetEntryRenders();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
