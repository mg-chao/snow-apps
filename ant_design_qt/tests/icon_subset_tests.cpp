#include "antd_icons.h"
#include "antd_subset_icons.h"
#include "icon_renderer.h"

#include <QApplication>
#include <QImage>

#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool hasVisiblePixels(const QImage& image) {
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixelColor(x, y).alpha() != 0) return true;
    }
  }
  return false;
}

void retainedIconsMatchTheFullPack() {
  const auto* full = adqt::icons::antd::pack().staticPack();
  const auto* subset = adqt::icons::subset_test::pack().staticPack();
  require(full != nullptr && full->entryCount == 829,
          "standalone/test builds must preserve the complete public Ant icon pack");
  require(subset != nullptr && subset->entryCount > 0 && subset->entryCount < full->entryCount,
          "the Snow Shot source union should produce a smaller, nonempty icon pack");
  adqt::icons::IconRenderer renderer;
  require(adqt::icons::subset_test::registerWith(renderer).ok(),
          "the generated application subset should register successfully");

  for (std::size_t index = 0; index < subset->entryCount; ++index) {
    const auto* descriptor = subset->entry(index);
    const auto retained = subset->icon(descriptor->variant, descriptor->name);
    const auto original = full->icon(descriptor->variant, descriptor->name);
    require(retained.isValid() && original.isValid(),
            "every retained canonical name and variant must resolve in both packs");
    require(renderer.describeIcon(retained) == renderer.describeIcon(original),
            "subset metadata and source hashes must preserve the original descriptor");
    require(retained.descriptor()->svg == original.descriptor()->svg,
            "subsetting must preserve normalized SVG bytes");
    for (const QColor& color : {QColor("#1F1F1F"), QColor("#F0F0F0")}) {
      adqt::icons::IconStatePalette palette;
      palette.set(QIcon::Normal, QIcon::Off, adqt::icons::IconColors::primary(color));
      for (const qreal scale : {1.0, 1.25, 2.0}) {
        adqt::icons::IconRenderRequest request;
        request.logicalSize = QSize(16, 16);
        request.devicePixelRatio = scale;
        const QImage actual = renderer.renderIconImage(retained, request, palette);
        const QImage expected = renderer.renderIconImage(original, request, palette);
        require(!actual.isNull() && hasVisiblePixels(actual),
                "all retained app and widget icons must render visible pixels");
        require(actual == expected,
                "subsetting must preserve rendering in light/dark colors and fractional DPR");
      }
    }
  }
  require(adqt::icons::subset_test::outlined::Search().descriptor() ==
              subset->find("outlined", "search"),
          "typed subset helpers must refer to the correct retained descriptor index");
  for (const auto variant : {"outlined", "filled", "twotone"}) {
    require(subset->icon(variant, "info-circle").isValid(),
            "widget default icons must retain all supported variants");
  }
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    retainedIconsMatchTheFullPack();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
