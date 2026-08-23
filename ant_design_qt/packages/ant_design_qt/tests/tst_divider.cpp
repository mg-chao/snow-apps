#include <QApplication>
#include <QAccessible>
#include <QImage>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QtTest>

#include "theme/theme.h"
#include "widgets/divider.h"

using adqt::widgets::AdDivider;

namespace {

QImage renderDivider(AdDivider* divider, const QColor& background = Qt::white) {
  divider->resize(320, divider->sizeHint().height());
  QImage image(divider->size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(background);
  QPainter painter(&image);
  divider->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
  return image;
}

QImage renderDividerAtDpr(AdDivider* divider, qreal devicePixelRatio,
                          qreal crossAxisTranslation = 0.0, const QColor& background = Qt::white) {
  const QSize physicalSize(qCeil(divider->width() * devicePixelRatio),
                           qCeil(divider->height() * devicePixelRatio));
  QImage image(physicalSize, QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(devicePixelRatio);
  image.fill(background);
  QPainter painter(&image);
  if (divider->orientation() == AdDivider::Orientation::Horizontal) {
    painter.translate(0.0, crossAxisTranslation);
  } else {
    painter.translate(crossAxisTranslation, 0.0);
  }
  divider->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
  return image;
}

struct RailProfile {
  qreal coverage = 0.0;
  qreal peakCoverage = 0.0;
};

RailProfile railProfileAtCenter(const QImage& image, AdDivider::Orientation orientation) {
  RailProfile profile;
  if (orientation == AdDivider::Orientation::Horizontal) {
    const int x = image.width() / 2;
    for (int y = 0; y < image.height(); ++y) {
      const qreal pixelCoverage = (255 - image.pixelColor(x, y).red()) / 255.0;
      profile.coverage += pixelCoverage;
      profile.peakCoverage = std::max(profile.peakCoverage, pixelCoverage);
    }
    return profile;
  }

  const int y = image.height() / 2;
  for (int x = 0; x < image.width(); ++x) {
    const qreal pixelCoverage = (255 - image.pixelColor(x, y).red()) / 255.0;
    profile.coverage += pixelCoverage;
    profile.peakCoverage = std::max(profile.peakCoverage, pixelCoverage);
  }
  return profile;
}

RailProfile renderRailProfile(AdDivider::Orientation orientation, int crossAxisExtent,
                              qreal devicePixelRatio, qreal logicalLineWidth,
                              qreal crossAxisTranslation) {
  AdDivider divider;
  divider.setOrientation(orientation);
  divider.setVariant(AdDivider::Variant::Solid);
  AdDivider::ComponentTokens tokens;
  tokens.colors.splitColor = Qt::black;
  tokens.metrics.lineWidth = logicalLineWidth;
  divider.setComponentTokens(tokens);
  divider.resize(orientation == AdDivider::Orientation::Horizontal ? QSize(80, crossAxisExtent)
                                                                   : QSize(crossAxisExtent, 80));
  return railProfileAtCenter(renderDividerAtDpr(&divider, devicePixelRatio, crossAxisTranslation),
                             orientation);
}

int nonBackgroundPixelsOnRow(const QImage& image, int y, const QColor& background = Qt::white) {
  int count = 0;
  for (int x = 0; x < image.width(); ++x) {
    if (image.pixelColor(x, y) != background) {
      ++count;
    }
  }
  return count;
}

}  // namespace

class DividerTest final : public QObject {
  Q_OBJECT

 private slots:
  void defaultsAndMetaProperties();
  void propertyChangesAreObservable();
  void variantsRenderDistinctRails();
  void titlePlacementFollowsLogicalDirection();
  void verticalSuppressesContent();
  void componentTokensAndSemanticStylesApply();
  void scopedThemeAndResolversApply();
  void customContentOwnershipCanBeTransferred();
  void customContentLifecycleUpdatesAccessibility();
  void disabledAndInvalidMetricsAreNormalized();
  void zeroLineWidthPreservesContent();
  void railThicknessIsDevicePixelAligned();
  void visualStatesRender();
};

void DividerTest::defaultsAndMetaProperties() {
  AdDivider divider;
  QCOMPARE(divider.orientation(), AdDivider::Orientation::Horizontal);
  QCOMPARE(divider.dividerSize(), AdDivider::Size::Large);
  QCOMPARE(divider.titlePlacement(), AdDivider::TitlePlacement::Center);
  QCOMPARE(divider.variant(), AdDivider::Variant::Solid);
  QVERIFY(!divider.dashed());
  QVERIFY(!divider.plain());
  QVERIFY(divider.text().isEmpty());
  QVERIFY(!divider.contentWidget());
  QCOMPARE(divider.frameShape(), QFrame::HLine);
  QCOMPARE(divider.sizePolicy().horizontalPolicy(), QSizePolicy::Expanding);
  QCOMPARE(divider.sizePolicy().verticalPolicy(), QSizePolicy::Fixed);
  QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(&divider);
  QVERIFY(accessible);
  QCOMPARE(accessible->role(), QAccessible::Separator);
  QVERIFY(divider.metaObject()->indexOfProperty("orientation") >= 0);
  QVERIFY(divider.metaObject()->indexOfProperty("type") >= 0);
  QVERIFY(divider.metaObject()->indexOfProperty("orientationMargin") >= 0);
  QVERIFY(divider.metaObject()->indexOfProperty("dividerSize") >= 0);
  QVERIFY(divider.metaObject()->indexOfProperty("titlePlacement") >= 0);
  QVERIFY(divider.metaObject()->indexOfProperty("variant") >= 0);
  QVERIFY(divider.metaObject()->indexOfProperty("text") >= 0);
}

void DividerTest::propertyChangesAreObservable() {
  AdDivider divider;
  QSignalSpy orientationChanged(&divider, &AdDivider::orientationChanged);
  QSignalSpy typeChanged(&divider, &AdDivider::typeChanged);
  QSignalSpy verticalChanged(&divider, &AdDivider::verticalChanged);
  QSignalSpy orientationMarginChanged(&divider, &AdDivider::orientationMarginChanged);
  QSignalSpy sizeChanged(&divider, &AdDivider::sizeChanged);
  QSignalSpy titleChanged(&divider, &AdDivider::titlePlacementChanged);
  QSignalSpy variantChanged(&divider, &AdDivider::variantChanged);
  QSignalSpy dashedChanged(&divider, &AdDivider::dashedChanged);
  QSignalSpy plainChanged(&divider, &AdDivider::plainChanged);
  QSignalSpy textChanged(&divider, &AdDivider::textChanged);

  divider.setVertical(true);
  divider.setOrientationMargin(0.2);
  divider.setDividerSize(AdDivider::Size::Small);
  divider.setTitlePlacement(AdDivider::TitlePlacement::Start);
  divider.setDashed(true);
  divider.setPlain(true);
  divider.setText(QStringLiteral("Section"));

  QCOMPARE(orientationChanged.count(), 1);
  QCOMPARE(typeChanged.count(), 1);
  QCOMPARE(verticalChanged.count(), 1);
  QCOMPARE(orientationMarginChanged.count(), 1);
  QCOMPARE(divider.type(), AdDivider::Orientation::Vertical);
  QCOMPARE(divider.orientationMargin(), 0.2);
  QCOMPARE(sizeChanged.count(), 1);
  QCOMPARE(titleChanged.count(), 1);
  QCOMPARE(variantChanged.count(), 1);
  QCOMPARE(dashedChanged.count(), 1);
  QCOMPARE(plainChanged.count(), 1);
  QCOMPARE(textChanged.count(), 1);
  QCOMPARE(divider.frameShape(), QFrame::VLine);
}

void DividerTest::variantsRenderDistinctRails() {
  AdDivider divider;
  divider.resize(320, divider.sizeHint().height());

  divider.setVariant(AdDivider::Variant::Solid);
  const QImage solid = renderDivider(&divider);
  const int solidPixels = nonBackgroundPixelsOnRow(solid, solid.height() / 2);
  QVERIFY(solidPixels > solid.width() * 3 / 4);

  divider.setVariant(AdDivider::Variant::Dashed);
  const QImage dashed = renderDivider(&divider);
  const int dashedPixels = nonBackgroundPixelsOnRow(dashed, dashed.height() / 2);
  QVERIFY(dashedPixels > 0);
  QVERIFY(dashedPixels < solidPixels);

  divider.setVariant(AdDivider::Variant::Dotted);
  const QImage dotted = renderDivider(&divider);
  const int dottedPixels = nonBackgroundPixelsOnRow(dotted, dotted.height() / 2);
  QVERIFY(dottedPixels > 0);
  QVERIFY(dottedPixels < solidPixels);
}

void DividerTest::titlePlacementFollowsLogicalDirection() {
  AdDivider divider;
  auto* label = new QLabel(QStringLiteral("Title"));
  divider.setContentWidget(label);
  divider.resize(320, divider.sizeHint().height());

  divider.setTitlePlacement(AdDivider::TitlePlacement::Start);
  divider.setLayoutDirection(Qt::LeftToRight);
  divider.show();
  QCoreApplication::processEvents();
  QVERIFY(label->isVisible());
  const int startLtr = label->geometry().left();

  divider.setTitlePlacement(AdDivider::TitlePlacement::End);
  QCoreApplication::processEvents();
  const int endLtr = label->geometry().left();
  QVERIFY(startLtr < endLtr);

  divider.setLayoutDirection(Qt::RightToLeft);
  divider.setTitlePlacement(AdDivider::TitlePlacement::Start);
  QCoreApplication::processEvents();
  const int startRtl = label->geometry().left();
  divider.setTitlePlacement(AdDivider::TitlePlacement::End);
  QCoreApplication::processEvents();
  const int endRtl = label->geometry().left();
  QVERIFY(startRtl > endRtl);
}

void DividerTest::verticalSuppressesContent() {
  AdDivider divider(QStringLiteral("Ignored"));
  auto* label = new QLabel(QStringLiteral("Hidden"));
  divider.setContentWidget(label);
  divider.setOrientation(AdDivider::Orientation::Vertical);
  divider.resize(divider.sizeHint());
  divider.show();
  QCoreApplication::processEvents();
  QVERIFY(!label->isVisible());
  QCOMPARE(divider.frameShape(), QFrame::VLine);
  QVERIFY(divider.sizeHint().height() > 0);
}

void DividerTest::componentTokensAndSemanticStylesApply() {
  AdDivider divider(QStringLiteral("Token"));
  AdDivider::ComponentTokens tokens;
  tokens.colors.splitColor = QColor(QStringLiteral("#1677ff"));
  tokens.metrics.lineWidth = 3.0;
  tokens.metrics.textPaddingInline = 4;
  divider.setComponentTokens(tokens);

  AdDivider::SemanticStyles styles;
  styles.root.backgroundColor = QColor(QStringLiteral("#f6f7f9"));
  styles.content.textColor = QColor(QStringLiteral("#cf1322"));
  divider.setSemanticStyles(styles);

  QCOMPARE(divider.componentTokens().colors.splitColor.value(), QColor(QStringLiteral("#1677ff")));
  QCOMPARE(divider.semanticStyles().root.backgroundColor.value(),
           QColor(QStringLiteral("#f6f7f9")));
  const QImage image = renderDivider(&divider, QColor(QStringLiteral("#ffffff")));
  QCOMPARE(image.pixelColor(0, image.height() / 2), QColor(QStringLiteral("#1677ff")));
  QCOMPARE(image.pixelColor(0, 0), QColor(QStringLiteral("#f6f7f9")));
}

void DividerTest::scopedThemeAndResolversApply() {
  AdDivider resolved(QStringLiteral("Resolved"));
  resolved.setDividerSize(AdDivider::Size::Small);
  resolved.setTitlePlacement(AdDivider::TitlePlacement::Start);
  resolved.setVariant(AdDivider::Variant::Dotted);
  resolved.setPlain(true);

  AdDivider::StyleContext tokenContext;
  AdDivider::StyleContext semanticContext;
  int tokenCalls = 0;
  int semanticCalls = 0;
  resolved.setComponentTokenResolver([&](const AdDivider::StyleContext& context) {
    tokenContext = context;
    ++tokenCalls;
    AdDivider::ComponentTokens tokens;
    tokens.metrics.textPaddingInline = 6;
    return tokens;
  });
  resolved.setSemanticStyleResolver([&](const AdDivider::StyleContext& context) {
    semanticContext = context;
    ++semanticCalls;
    AdDivider::SemanticStyles styles;
    styles.rail.borderColor = QColor(QStringLiteral("#722ed1"));
    return styles;
  });
  const QImage resolvedImage = renderDivider(&resolved);
  QVERIFY(!resolvedImage.isNull());
  QVERIFY(tokenCalls > 0);
  QVERIFY(semanticCalls > 0);
  QCOMPARE(tokenContext.size, AdDivider::Size::Small);
  QCOMPARE(tokenContext.titlePlacement, AdDivider::TitlePlacement::Start);
  QCOMPARE(tokenContext.variant, AdDivider::Variant::Dotted);
  QVERIFY(tokenContext.plain);
  QVERIFY(tokenContext.hasContent);
  QCOMPARE(semanticContext.titlePlacement, tokenContext.titlePlacement);

  AdDivider themed;
  auto& themeManager = adqt::theme::ThemeManager::instance();
  adqt::theme::ThemeOverride lightOverride;
  lightOverride.scheme = adqt::theme::ThemeScheme::Light;
  themeManager.setScopeOverride(&themed, lightOverride);
  const QImage lightImage = renderDivider(&themed);
  const QColor lightRail = lightImage.pixelColor(0, lightImage.height() / 2);

  adqt::theme::ThemeOverride darkOverride;
  darkOverride.scheme = adqt::theme::ThemeScheme::Dark;
  themeManager.setScopeOverride(&themed, darkOverride);
  const QImage darkImage = renderDivider(&themed);
  const QColor darkRail = darkImage.pixelColor(0, darkImage.height() / 2);
  QCOMPARE(themeManager.resolve(&themed).config.scheme, adqt::theme::ThemeScheme::Dark);
  QVERIFY(lightRail != darkRail);
  themeManager.clearScopeOverride(&themed);
}

void DividerTest::customContentOwnershipCanBeTransferred() {
  AdDivider divider;
  auto* label = new QLabel(QStringLiteral("Owned"));
  divider.setContentWidget(label);
  QCOMPARE(label->parentWidget(), &divider);
  QCOMPARE(divider.contentWidget(), label);

  QWidget* transferred = divider.takeContentWidget();
  QCOMPARE(transferred, label);
  QVERIFY(!divider.contentWidget());
  QVERIFY(!label->parentWidget());
  delete transferred;
}

void DividerTest::customContentLifecycleUpdatesAccessibility() {
  AdDivider divider(QStringLiteral("Fallback"));

  auto* first = new QLabel(QStringLiteral("First"));
  first->setAccessibleDescription(QStringLiteral("First content description"));
  QPointer<QLabel> firstGuard(first);
  divider.setContentWidget(first);
  QCOMPARE(divider.accessibleName(), QStringLiteral("First content description"));

  QWidget* transferred = divider.takeContentWidget();
  QCOMPARE(transferred, first);
  QCOMPARE(divider.accessibleName(), QStringLiteral("Fallback"));
  QVERIFY(!first->parentWidget());
  delete transferred;
  QVERIFY(firstGuard.isNull());

  auto* second = new QLabel(QStringLiteral("Second"));
  second->setAccessibleName(QStringLiteral("Second content"));
  QPointer<QLabel> secondGuard(second);
  QSignalSpy contentChanged(&divider, &AdDivider::contentWidgetChanged);
  divider.setContentWidget(second);
  QCOMPARE(divider.accessibleName(), QStringLiteral("Second content"));

  auto* replacement = new QLabel(QStringLiteral("Replacement"));
  divider.setContentWidget(replacement);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QVERIFY(secondGuard.isNull());
  QCOMPARE(divider.contentWidget(), replacement);
  QVERIFY(contentChanged.count() >= 2);

  auto* externallyReparented = new QLabel(QStringLiteral("External"));
  divider.setContentWidget(externallyReparented);
  QWidget holder;
  externallyReparented->setParent(&holder);
  QWidget* released = divider.takeContentWidget();
  QCOMPARE(released, externallyReparented);
  QVERIFY(!released->parentWidget());
  delete released;

  QWidget ancestor;
  AdDivider nested(&ancestor);
  nested.setContentWidget(&ancestor);
  QVERIFY(!nested.contentWidget());
}

void DividerTest::disabledAndInvalidMetricsAreNormalized() {
  AdDivider divider(QStringLiteral("Disabled"));
  const QImage enabledImage = renderDivider(&divider);

  divider.setEnabled(false);
  const QImage disabledImage = renderDivider(&divider);
  bool disabledAppearanceVisible = false;
  for (int y = 0; y < enabledImage.height() && !disabledAppearanceVisible; ++y) {
    for (int x = 0; x < enabledImage.width(); ++x) {
      if (enabledImage.pixelColor(x, y) != disabledImage.pixelColor(x, y)) {
        disabledAppearanceVisible = true;
        break;
      }
    }
  }
  QVERIFY(disabledAppearanceVisible);

  divider.setEnabled(true);
  QPalette customPalette = divider.palette();
  const QColor customRail(QStringLiteral("#fa541c"));
  customPalette.setColor(QPalette::Active, QPalette::Mid, customRail);
  divider.setPalette(customPalette);
  const QImage customPaletteImage = renderDivider(&divider);
  bool customRailVisible = false;
  for (int y = 0; y < customPaletteImage.height() && !customRailVisible; ++y) {
    for (int x = 0; x < customPaletteImage.width(); ++x) {
      if (customPaletteImage.pixelColor(x, y) == customRail) {
        customRailVisible = true;
        break;
      }
    }
  }
  QVERIFY(customRailVisible);

  divider.setOrientationMargin(qQNaN());
  QVERIFY(qIsFinite(divider.orientationMargin()));
  QCOMPARE(divider.orientationMargin(), 0.05);

  AdDivider::ComponentTokens tokens;
  tokens.metrics.orientationMargin = qQNaN();
  tokens.metrics.verticalHeightFactor = qQNaN();
  divider.setComponentTokens(tokens);
  QCOMPARE(divider.orientationMargin(), 0.05);
  QVERIFY(qIsFinite(divider.sizeHint().width()));
  QVERIFY(qIsFinite(divider.sizeHint().height()));
}

void DividerTest::zeroLineWidthPreservesContent() {
  AdDivider divider(QStringLiteral("Content only"));
  AdDivider::ComponentTokens tokens;
  tokens.metrics.lineWidth = 0.0;
  divider.setComponentTokens(tokens);
  AdDivider::SemanticStyles styles;
  styles.content.backgroundColor = QColor(QStringLiteral("#fff1f0"));
  divider.setSemanticStyles(styles);

  const QImage image = renderDivider(&divider);
  bool contentBackgroundVisible = false;
  for (int y = 0; y < image.height() && !contentBackgroundVisible; ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixelColor(x, y) == QColor(QStringLiteral("#fff1f0"))) {
        contentBackgroundVisible = true;
        break;
      }
    }
  }
  QVERIFY(contentBackgroundVisible);
}

void DividerTest::railThicknessIsDevicePixelAligned() {
  constexpr qreal logicalLineWidth = 1.0;
  const QList<qreal> devicePixelRatios = {1.0, 1.25, 1.5, 1.75, 2.0};
  const QList<int> crossAxisExtents = {16, 17};
  const QList<qreal> crossAxisTranslations = {0.0, 0.2, 0.4, 0.6};
  const QList<AdDivider::Orientation> orientations = {AdDivider::Orientation::Horizontal,
                                                      AdDivider::Orientation::Vertical};

  for (const qreal devicePixelRatio : devicePixelRatios) {
    std::optional<qreal> referenceCoverage;
    std::optional<qreal> referencePeakCoverage;
    for (const AdDivider::Orientation orientation : orientations) {
      for (const int crossAxisExtent : crossAxisExtents) {
        for (const qreal crossAxisTranslation : crossAxisTranslations) {
          const RailProfile profile =
              renderRailProfile(orientation, crossAxisExtent, devicePixelRatio, logicalLineWidth,
                                crossAxisTranslation);
          if (!referenceCoverage.has_value()) {
            referenceCoverage = profile.coverage;
            referencePeakCoverage = profile.peakCoverage;
          }
          const QString context =
              QStringLiteral(
                  "orientation=%1, extent=%2, translation=%3, dpr=%4, coverage=%5, "
                  "peak=%6, reference=%7")
                  .arg(orientation == AdDivider::Orientation::Horizontal
                           ? QStringLiteral("horizontal")
                           : QStringLiteral("vertical"))
                  .arg(crossAxisExtent)
                  .arg(crossAxisTranslation)
                  .arg(devicePixelRatio)
                  .arg(profile.coverage, 0, 'f', 3)
                  .arg(profile.peakCoverage, 0, 'f', 3)
                  .arg(referenceCoverage.value(), 0, 'f', 3);
          QVERIFY2(qAbs(profile.coverage - referenceCoverage.value()) <= 0.05, qPrintable(context));
          QVERIFY2(qAbs(profile.peakCoverage - referencePeakCoverage.value()) <= 0.05,
                   qPrintable(context));
        }
      }
    }
  }
}

void DividerTest::visualStatesRender() {
  QWidget canvas;
  canvas.setAutoFillBackground(true);
  QPalette canvasPalette = canvas.palette();
  canvasPalette.setColor(QPalette::Window, Qt::white);
  canvasPalette.setColor(QPalette::WindowText, QColor(QStringLiteral("#141414")));
  canvas.setPalette(canvasPalette);
  auto* root = new QVBoxLayout(&canvas);
  root->setContentsMargins(24, 20, 24, 20);
  root->setSpacing(0);

  auto* heading = new QLabel(QStringLiteral("Divider visual states"));
  QFont headingFont = heading->font();
  headingFont.setPixelSize(20);
  headingFont.setBold(true);
  heading->setFont(headingFont);
  root->addWidget(heading);

  root->addWidget(new AdDivider());
  root->addWidget(new AdDivider(QStringLiteral("Center title")));

  auto* start = new AdDivider(QStringLiteral("Start dotted"));
  start->setTitlePlacement(AdDivider::TitlePlacement::Start);
  start->setVariant(AdDivider::Variant::Dotted);
  root->addWidget(start);

  auto* end = new AdDivider(QStringLiteral("End dashed"));
  end->setTitlePlacement(AdDivider::TitlePlacement::End);
  end->setVariant(AdDivider::Variant::Dashed);
  root->addWidget(end);

  auto* plain = new AdDivider(QStringLiteral("Plain title"));
  plain->setPlain(true);
  plain->setDividerSize(AdDivider::Size::Small);
  root->addWidget(plain);

  auto* inlineRow = new QWidget();
  auto* inlineLayout = new QHBoxLayout(inlineRow);
  inlineLayout->setContentsMargins(0, 8, 0, 0);
  inlineLayout->setSpacing(0);
  inlineLayout->addWidget(new QLabel(QStringLiteral("Text")));
  auto* solidVertical = new AdDivider();
  solidVertical->setVertical(true);
  inlineLayout->addWidget(solidVertical);
  inlineLayout->addWidget(new QLabel(QStringLiteral("Link")));
  auto* dottedVertical = new AdDivider();
  dottedVertical->setVertical(true);
  dottedVertical->setVariant(AdDivider::Variant::Dotted);
  inlineLayout->addWidget(dottedVertical);
  inlineLayout->addWidget(new QLabel(QStringLiteral("Another link")));
  inlineLayout->addStretch();
  root->addWidget(inlineRow);

  canvas.resize(720, canvas.sizeHint().height());
  canvas.show();
  QCoreApplication::processEvents();
  QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  canvas.render(&painter);
  painter.end();
  QVERIFY(!image.isNull());
  QVERIFY(image.width() == 720);
  QVERIFY(image.height() > 200);

  const QString snapshotPath = QString::fromLocal8Bit(qgetenv("ADQT_DIVIDER_SNAPSHOT_PATH"));
  if (!snapshotPath.isEmpty()) {
    QVERIFY2(image.save(snapshotPath), qPrintable(snapshotPath));
  }
}

QTEST_MAIN(DividerTest)
#include "tst_divider.moc"
