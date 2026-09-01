#include "antd_icons.h"
#include "external_icon_pack.h"
#include "icon_renderer.h"
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

using adqt::icons::IconColorModel;
using adqt::icons::IconDescriptor;
using adqt::icons::IconFit;
using adqt::icons::IconPack;
using adqt::icons::IconStaticColors;

constexpr IconDescriptor kEntries[] = {
    {// A non-square monochrome entry for containment, DPR, and direct-paint coverage.
     std::string_view("core-test"), std::string_view("outlined"), std::string_view("wide"),
     std::string_view(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 10">)"
                      R"(<rect width="20" height="10" fill="__ADQT_SLOT_PRIMARY__"/>)"
                      R"(</svg>)"),
     std::string_view("core-test-wide"), IconColorModel::Monochrome, IconFit::Contain,
     IconStaticColors{}, false},
    {// A full-color entry that must reject theme-slot overrides.
     std::string_view("core-test"), std::string_view("app"), std::string_view("full-color"),
     std::string_view(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10">)"
                      R"(<rect width="10" height="10" fill="#E53935"/>)"
                      R"(</svg>)"),
     std::string_view("core-test-full-color"), IconColorModel::FullColor, IconFit::Contain,
     IconStaticColors{}, false},
    {// A two-tone entry whose secondary slot keeps a fixed pack default color.
     std::string_view("core-test"), std::string_view("twotone"),
     std::string_view("fixed-secondary"),
     std::string_view(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 10">)"
                      R"(<rect width="10" height="10" fill="__ADQT_SLOT_PRIMARY__"/>)"
                      R"(<rect x="10" width="10" height="10" fill="__ADQT_SLOT_SECONDARY__"/>)"
                      R"(</svg>)"),
     std::string_view("core-test-fixed-secondary"), IconColorModel::TwoTone, IconFit::Contain,
     IconStaticColors{std::string_view(""), std::string_view("#9254DE"), std::string_view("")},
     false},
    {// A monochrome entry with a translucent default primary color.
     std::string_view("core-test"), std::string_view("outlined"),
     std::string_view("translucent-default"),
     std::string_view(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">)"
                      R"(<rect x="1" y="1" width="14" height="14" fill="__ADQT_SLOT_PRIMARY__"/>)"
                      R"(</svg>)"),
     std::string_view("core-test-translucent"), IconColorModel::Monochrome, IconFit::Contain,
     IconStaticColors{std::string_view("#490C2238"), std::string_view(""), std::string_view("")},
     false},
};

constexpr IconPack kTestPack{std::string_view("core-test"),
                             std::string_view("icon core tests"),
                             std::string_view("core-test-pack"), kEntries,
                             sizeof(kEntries) / sizeof(kEntries[0])};

constexpr std::size_t kWide = 0;
constexpr std::size_t kFullColor = 1;
constexpr std::size_t kFixedSecondary = 2;
constexpr std::size_t kTranslucentDefault = 3;

void staticPackRegistrationIsValidAndIdempotent() {
  adqt::icons::IconRenderer renderer;
  const auto invalid = renderer.registerStaticPack(IconPack{});
  require(!invalid.ok(), "an invalid static pack should be rejected");

  const auto first = renderer.registerStaticPack(kTestPack);
  require(first.ok() && first.registeredCount == static_cast<int>(kTestPack.entryCount),
          "a valid static pack should register every entry");
  const auto repeated = renderer.registerStaticPack(kTestPack);
  require(repeated.ok() && repeated.existingCount == static_cast<int>(kTestPack.entryCount) &&
              repeated.registeredCount == 0,
          "registering the same static pack again should be idempotent");
}

void referencesAreImmutableValuesAndSupportIsolatedRenderers() {
  const adqt::icons::ExternalIconPack pack(kTestPack);
  adqt::icons::IconRenderer renderer;
  const auto registered = pack.registerWith(renderer);
  require(registered.ok() && registered.registeredCount == static_cast<int>(kTestPack.entryCount),
          "a static pack should register into an isolated renderer");
  const auto ref = pack.icon(kWide);
  const auto red = ref.withColors(adqt::icons::IconColors::primary(QColor(Qt::red)));
  require(ref.isValid() && red.isValid() && ref != red,
          "withColors should derive a distinct immutable value");
  QSet<adqt::icons::IconRef> values{ref, ref, red};
  require(values.size() == 2, "reference equality and hashing should use identity and colors");
  const auto metadata = renderer.describeIcon(ref);
  require(metadata.key.pack == QStringLiteral("core-test") &&
              metadata.key.variant == QStringLiteral("outlined") &&
              metadata.key.name == QStringLiteral("wide"),
          "metadata inspection should expose the canonical key");
  const auto view = renderer.describeIconView(ref);
  require(view.pack == std::string_view("core-test") && view.name == std::string_view("wide") &&
              view.isValid(),
          "the metadata view should expose the same descriptor fields without owning them");
}

void primaryOverridesPreserveFixedDefaultSecondaryColors() {
  adqt::icons::IconRenderer renderer;
  const QColor primary("#345678");
  const QColor fixedSecondary("#9254DE");
  const auto ref = kTestPack.icon(kFixedSecondary, adqt::icons::IconColors::primary(primary));
  adqt::icons::IconRenderRequest request;
  request.logicalSize = QSize(20, 10);
  request.devicePixelRatio = 1.0;
  const QImage image = renderer.renderIconPixmap(ref, request).toImage();
  require(containsOpaqueColor(image, primary),
          "an explicit primary color should still override the primary slot");
  require(containsOpaqueColor(image, fixedSecondary),
          "a primary override should preserve a pack's fixed default secondary color");

  adqt::icons::IconStatePalette palette;
  palette.set(QIcon::Normal, QIcon::Off, adqt::icons::IconColors::primary(primary));
  const auto uncoloredRef = kTestPack.icon(kFixedSecondary);
  const QImage stateImage = renderer.renderIconPixmap(uncoloredRef, request, palette).toImage();
  require(containsOpaqueColor(stateImage, primary),
          "a state primary color should override the primary slot");
  require(containsOpaqueColor(stateImage, fixedSecondary),
          "a state primary override should preserve a pack's fixed default secondary color");
}

void staticDefaultColorsPreserveAlpha() {
  adqt::icons::IconRenderer renderer;
  const auto ref = kTestPack.icon(kTranslucentDefault);
  adqt::icons::IconRenderRequest request;
  request.logicalSize = QSize(16, 16);
  request.devicePixelRatio = 1.0;
  const QImage image = renderer.renderIconImage(ref, request);
  require(!image.isNull() && image.pixelColor(8, 8).alpha() == 73,
          "descriptor default colors should preserve their alpha channel");
}

void lazyPackAccessIsThreadSafe() {
  static const adqt::icons::ExternalIconPack pack(kTestPack);
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
      refs[index] = pack.icon(kWide);
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
  adqt::icons::IconRenderer renderer;
  const auto ref = kTestPack.icon(kWide, adqt::icons::IconColors::primary(QColor(Qt::black)));
  adqt::icons::IconRenderRequest request;
  request.logicalSize = QSize(20, 20);
  for (const qreal dpr : {1.0, 1.25, 1.5, 2.0, 3.0}) {
    request.devicePixelRatio = dpr;
    const QPixmap pixmap = renderer.renderIconPixmap(ref, request);
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

  const auto fullColor = kTestPack.icon(kFullColor);
  const auto invalid =
      kTestPack.icon(kFullColor, adqt::icons::IconColors::primary(QColor(Qt::blue)));
  require(fullColor.isValid() && !invalid.isValid(),
          "full-color entries should reject theme slots");
}

void directPaintingUsesTheEntireHighDpiPixmap() {
  adqt::icons::IconRenderer renderer;
  const auto ref = kTestPack.icon(kWide, adqt::icons::IconColors::primary(QColor(Qt::black)));
  const QRectF targetRect(8.0, 8.0, 20.0, 20.0);
  for (const qreal dpr : {1.25, 1.5, 2.0}) {
    const int physicalCanvasSide = qRound(36.0 * dpr);
    const QSize physicalCanvasSize(physicalCanvasSide, physicalCanvasSide);
    QImage actual(physicalCanvasSize, QImage::Format_ARGB32_Premultiplied);
    actual.setDevicePixelRatio(dpr);
    actual.fill(Qt::transparent);
    {
      QPainter painter(&actual);
      renderer.paintIcon(&painter, ref, targetRect);
    }

    adqt::icons::IconRenderRequest request;
    request.logicalSize = targetRect.size().toSize();
    request.devicePixelRatio = dpr;
    const QPixmap expectedPixmap = renderer.renderIconPixmap(ref, request);
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
                       adqt::icons::IconRenderer& renderer) {
  const auto registered = pack.registerWith(renderer);
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
    const auto metadata = renderer.describeIconView(ref);
    require(metadata.pack == staticPack->packName && metadata.variant == entry->variant &&
                metadata.name == entry->name && metadata.sourceHash == entry->sourceHash,
            "generated entry metadata should match its pack definition");
    const QPixmap pixmap = renderer.renderIconPixmap(ref, request);
    require(!pixmap.isNull() && pixmap.size() == QSize(20, 20) &&
                qFuzzyCompare(pixmap.devicePixelRatio(), 1.25),
            "every generated entry should render at fractional DPR");
    require(!alphaBounds(pixmap.toImage()).isEmpty(),
            "every generated entry should render nonblank alpha bounds");
  }
}

void everyBuiltInAndWidgetEntryRenders() {
  adqt::icons::IconRenderer renderer;
  renderPackEntries(adqt::icons::antd::pack(), renderer);
  renderPackEntries(adqt::widgets::icons::pack(), renderer);
  require(adqt::icons::antd::pack().staticPack()->entryCount == 829,
          "the pinned built-in Ant pack should contain exactly 829 upstream entries");
  require(adqt::widgets::icons::pack().staticPack()->entryCount == 1,
          "the widget-owned pack should contain only empty-simple");
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    staticPackRegistrationIsValidAndIdempotent();
    referencesAreImmutableValuesAndSupportIsolatedRenderers();
    primaryOverridesPreserveFixedDefaultSecondaryColors();
    staticDefaultColorsPreserveAlpha();
    lazyPackAccessIsThreadSafe();
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
