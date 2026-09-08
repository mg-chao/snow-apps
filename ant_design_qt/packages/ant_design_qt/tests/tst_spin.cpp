#include <QAccessible>
#include <QApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMetaProperty>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
#include <cstdlib>
#include <limits>

#include "theme/theme_manager.h"
#include "widgets/spin.h"
#include "widgets/spin_style.h"

using adqt::widgets::AdSpin;

class SpinTest final : public QObject {
  Q_OBJECT

 private slots:
  void defaultsAndPropertyMetadata();
  void accessibleProgressContract();
  void delayDefersAndCancellationPreventsActivation();
  void sizeClassesFollowAntDesignTokenRatios();
  void sizeHintContainsCompleteIndicator_data();
  void sizeHintContainsCompleteIndicator();
  void contentOverlayTracksGeometryAndBlocksHitTesting();
  void contentOverlayBlocksScopedInput();
  void customIndicatorIsOwnedAndCentered();
  void replacementAndTakeFollowQtOwnershipRules();
  void determinateAndAutomaticProgressModes();
  void automaticProgressPausesWhileHidden();
  void disabledAnimationIsVisuallyStable();
  void fullscreenSurfaceCoversAndTracksWindow();
  void fullscreenSurfaceRehostsAfterParentChange();
  void externalDestructionAndInvalidAdoptionAreSafe();
  void semanticAndComponentTokensResolve();
  void renderedStatesHaveVisibleAntDesignStructure();
};

void SpinTest::defaultsAndPropertyMetadata() {
  AdSpin spin;
  QVERIFY(spin.spinning());
  QVERIFY(spin.isActive());
  QCOMPARE(spin.delayMs(), 0);
  QCOMPARE(spin.sizeClass(), AdSpin::SizeClass::Medium);
  QCOMPARE(spin.progressMode(), AdSpin::ProgressMode::None);
  QVERIFY(!spin.fullscreen());
  QVERIFY(!spin.accessibleName().isEmpty());

  const QMetaObject* meta = spin.metaObject();
  for (const char* name :
       {"spinning", "active", "delayMs", "sizeClass", "description", "fullscreen", "progressMode",
        "percent", "displayedPercent", "contentWidget", "indicatorWidget"}) {
    const int index = meta->indexOfProperty(name);
    QVERIFY2(index >= 0, name);
    const QMetaProperty property = meta->property(index);
    QVERIFY(property.isReadable());
    QVERIFY(property.hasNotifySignal());
  }
  QVERIFY(!meta->property(meta->indexOfProperty("active")).isWritable());
  QVERIFY(!meta->property(meta->indexOfProperty("displayedPercent")).isWritable());
  QVERIFY(meta->property(meta->indexOfProperty("contentWidget")).isWritable());
  QVERIFY(meta->property(meta->indexOfProperty("indicatorWidget")).isWritable());
}

void SpinTest::accessibleProgressContract() {
  AdSpin spin;
  QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(&spin);
  QVERIFY(interface);
  QCOMPARE(interface->role(), QAccessible::ProgressBar);
  QCOMPARE(interface->text(QAccessible::Name), QStringLiteral("Loading"));
  QVERIFY(interface->state().busy);
  QVERIFY(interface->state().invisible);
  QVERIFY(interface->state().readOnly);
  QVERIFY(!interface->state().focusable);

  auto* valueInterface = static_cast<QAccessibleValueInterface*>(
      interface->interface_cast(QAccessible::ValueInterface));
  QVERIFY(valueInterface);
  QVERIFY(!valueInterface->currentValue().isValid());
  QCOMPARE(valueInterface->minimumValue().toDouble(), 0.0);
  QCOMPARE(valueInterface->maximumValue().toDouble(), 100.0);

  spin.setDescription(QStringLiteral("Fetching records"));
  spin.setPercent(42.0);
  QCOMPARE(interface->text(QAccessible::Description), QStringLiteral("Fetching records"));
  QCOMPARE(interface->text(QAccessible::Value), QStringLiteral("42%"));
  QCOMPARE(valueInterface->currentValue().toDouble(), 42.0);
  valueInterface->setCurrentValue(75.0);
  QCOMPARE(spin.percent(), 42.0);

  spin.show();
  QCoreApplication::processEvents();
  QVERIFY(!interface->state().invisible);
  spin.setSpinning(false);
  QVERIFY(!interface->state().busy);
  QVERIFY(interface->state().invisible);
}

void SpinTest::delayDefersAndCancellationPreventsActivation() {
  AdSpin configured;
  configured.setDelayMs(40);
  QVERIFY(!configured.isActive());
  QTRY_VERIFY_WITH_TIMEOUT(configured.isActive(), 150);

  AdSpin spin;
  spin.setSpinning(false);
  spin.setDelayMs(80);
  QSignalSpy activeSpy(&spin, &AdSpin::activeChanged);

  spin.setSpinning(true);
  QVERIFY(!spin.isActive());
  QTest::qWait(30);
  QVERIFY(!spin.isActive());
  spin.setSpinning(false);
  QTest::qWait(70);
  QVERIFY(!spin.isActive());

  spin.setSpinning(true);
  QTRY_VERIFY_WITH_TIMEOUT(spin.isActive(), 200);
  QCOMPARE(activeSpy.count(), 1);
}

void SpinTest::sizeClassesFollowAntDesignTokenRatios() {
  AdSpin spin;
  spin.setSizeClass(AdSpin::SizeClass::Small);
  const QSize small = spin.sizeHint();
  spin.setSizeClass(AdSpin::SizeClass::Medium);
  const QSize medium = spin.sizeHint();
  spin.setSizeClass(AdSpin::SizeClass::Large);
  const QSize large = spin.sizeHint();
  QVERIFY(small.width() < medium.width());
  QVERIFY(medium.width() < large.width());

  spin.setDescription(QStringLiteral("Loading"));
  QVERIFY(spin.sizeHint().height() > large.height());
  QCOMPARE(spin.accessibleDescription(), QStringLiteral("Loading"));
}

void SpinTest::sizeHintContainsCompleteIndicator_data() {
  QTest::addColumn<int>("side");
  QTest::addColumn<bool>("progress");
  for (const int side : {14, 15, 20, 21, 32, 33}) {
    for (const bool progress : {false, true}) {
      QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(side).arg(progress)))
          << side << progress;
    }
  }
}

void SpinTest::sizeHintContainsCompleteIndicator() {
  QFETCH(int, side);
  QFETCH(bool, progress);
  AdSpin spin;
  AdSpin::ComponentTokens tokens;
  tokens.metrics.dotSize = side;
  tokens.metrics.animationCycleMs = 0;
  spin.setComponentTokens(tokens);
  if (progress) {
    spin.setPercent(100.0);
  }
  AdSpin::SemanticStyles styles;
  styles.root.backgroundColor = Qt::white;
  spin.setSemanticStyles(styles);

  const auto render = [&spin](const QSize& size) {
    spin.resize(size);
    spin.show();
    QCoreApplication::processEvents();
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    spin.render(&image);
    return image;
  };
  const QSize hint = spin.sizeHint();
  const QImage tight = render(hint);
  constexpr int padding = 8;
  const QImage roomy = render(hint + QSize(padding * 2, padding * 2));
  QImage expected(roomy.size(), roomy.format());
  expected.fill(Qt::white);
  {
    QPainter painter(&expected);
    painter.drawImage(QPoint(padding, padding), tight);
  }
  QVERIFY2(roomy == expected,
           "Rendering at sizeHint must preserve every pixel of the unconstrained indicator");
}

void SpinTest::contentOverlayTracksGeometryAndBlocksHitTesting() {
  QWidget host;
  host.resize(260, 140);
  auto* button = new QPushButton(QStringLiteral("Action"));
  auto* spin = new AdSpin(button, &host);
  spin->setGeometry(20, 20, 180, 70);
  host.show();
  QTest::qWait(20);

  QWidget* surface = spin->findChild<QWidget*>(QStringLiteral("spinSurface"));
  QVERIFY(surface);
  QCOMPARE(surface->geometry(), spin->rect());
  QCOMPARE(button->geometry(), spin->rect());
  QVERIFY(surface->isVisible());
  QCOMPARE(spin->childAt(spin->rect().center()), surface);

  spin->setSpinning(false);
  QCoreApplication::processEvents();
  QVERIFY(!surface->isVisible());
  QCOMPARE(spin->childAt(spin->rect().center()), button);
  QVERIFY(button->isEnabled());
}

void SpinTest::contentOverlayBlocksScopedInput() {
  QWidget host;
  host.resize(300, 120);
  auto* button = new QPushButton(QStringLiteral("Wrapped"));
  auto* spin = new AdSpin(button, &host);
  spin->setGeometry(10, 10, 130, 50);
  auto* sibling = new QPushButton(QStringLiteral("Sibling"), &host);
  sibling->setGeometry(160, 10, 130, 50);
  QSignalSpy wrappedClicks(button, &QPushButton::clicked);
  QSignalSpy siblingClicks(sibling, &QPushButton::clicked);
  host.show();
  QCoreApplication::processEvents();

  button->setFocus();
  QCoreApplication::processEvents();
  QVERIFY(!button->hasFocus());
  QTest::keyClick(button, Qt::Key_Space);
  QTest::mouseClick(button, Qt::LeftButton);
  QCOMPARE(wrappedClicks.count(), 0);

  QTest::mouseClick(sibling, Qt::LeftButton);
  QCOMPARE(siblingClicks.count(), 1);

  spin->setSpinning(false);
  QCoreApplication::processEvents();
  QTest::mouseClick(button, Qt::LeftButton);
  QCOMPARE(wrappedClicks.count(), 1);
}

void SpinTest::customIndicatorIsOwnedAndCentered() {
  AdSpin spin;
  spin.resize(160, 100);
  auto* indicator = new QLabel(QStringLiteral("Custom"));
  indicator->setFixedSize(52, 18);
  spin.setIndicatorWidget(indicator);
  spin.show();
  QCoreApplication::processEvents();

  QCOMPARE(spin.indicatorWidget(), indicator);
  QVERIFY(indicator->parentWidget());
  QTRY_VERIFY_WITH_TIMEOUT(std::abs(indicator->geometry().center().x() -
                                    indicator->parentWidget()->rect().center().x()) <= 1,
                           100);
  QWidget* taken = spin.takeIndicatorWidget();
  QCOMPARE(taken, indicator);
  QVERIFY(!spin.indicatorWidget());
  delete taken;
}

void SpinTest::replacementAndTakeFollowQtOwnershipRules() {
  AdSpin spin;
  QPointer<QWidget> oldContent = new QWidget();
  spin.setContentWidget(oldContent);
  auto* replacement = new QWidget();
  spin.setContentWidget(replacement);
  QVERIFY(oldContent.isNull());
  QCOMPARE(spin.contentWidget(), replacement);

  QWidget* taken = spin.takeContentWidget();
  QCOMPARE(taken, replacement);
  QVERIFY(!spin.contentWidget());
  QVERIFY(!taken->parentWidget());
  delete taken;

  QPointer<QWidget> oldIndicator = new QWidget();
  spin.setIndicatorWidget(oldIndicator);
  auto* replacementIndicator = new QWidget();
  spin.setIndicatorWidget(replacementIndicator);
  QVERIFY(oldIndicator.isNull());
  QCOMPARE(spin.takeIndicatorWidget(), replacementIndicator);
  delete replacementIndicator;
}

void SpinTest::determinateAndAutomaticProgressModes() {
  AdSpin spin;
  spin.resize(80, 80);
  spin.setPercent(150.0);
  QCOMPARE(spin.progressMode(), AdSpin::ProgressMode::Determinate);
  QCOMPARE(spin.percent(), 100.0);
  QCOMPARE(spin.displayedPercent(), 100.0);
  spin.setPercent(-20.0);
  QCOMPARE(spin.percent(), 0.0);
  spin.setPercent(std::numeric_limits<qreal>::quiet_NaN());
  QCOMPARE(spin.percent(), 0.0);

  QSignalSpy displayedSpy(&spin, &AdSpin::displayedPercentChanged);
  spin.setPercent(68.0);
  QCOMPARE(displayedSpy.count(), 1);
  QCOMPARE(displayedSpy.takeFirst().at(0).toReal(), 68.0);

  AdSpin::ComponentTokens tokens;
  tokens.metrics.autoProgressIntervalMs = 20;
  spin.setComponentTokens(tokens);
  spin.setAutoProgress();
  spin.show();
  QTRY_VERIFY_WITH_TIMEOUT(spin.displayedPercent() > 0.0, 300);
  QVERIFY(spin.displayedPercent() < 100.0);
  spin.clearProgress();
  QCOMPARE(spin.progressMode(), AdSpin::ProgressMode::None);
}

void SpinTest::automaticProgressPausesWhileHidden() {
  AdSpin spin;
  AdSpin::ComponentTokens tokens;
  tokens.metrics.autoProgressIntervalMs = 20;
  spin.setComponentTokens(tokens);
  QSignalSpy displayedSpy(&spin, &AdSpin::displayedPercentChanged);
  spin.setAutoProgress();

  QTest::qWait(60);
  QCOMPARE(spin.displayedPercent(), 0.0);
  QCOMPARE(displayedSpy.count(), 0);

  spin.show();
  QTRY_VERIFY_WITH_TIMEOUT(spin.displayedPercent() > 0.0, 200);
  spin.hide();
  const qreal pausedPercent = spin.displayedPercent();
  QTest::qWait(80);
  QCOMPARE(spin.displayedPercent(), pausedPercent);

  spin.show();
  QTRY_VERIFY_WITH_TIMEOUT(spin.displayedPercent() > pausedPercent, 200);

  tokens.metrics.autoProgressIntervalMs = 1;
  spin.setComponentTokens(tokens);
  QTRY_COMPARE_WITH_TIMEOUT(spin.displayedPercent(), 99.0, 3000);
  const int settledSignalCount = displayedSpy.count();
  QTest::qWait(30);
  QCOMPARE(displayedSpy.count(), settledSignalCount);
}

void SpinTest::disabledAnimationIsVisuallyStable() {
  AdSpin spin;
  spin.setFixedSize(80, 80);
  AdSpin::ComponentTokens tokens;
  tokens.metrics.animationCycleMs = 0;
  spin.setComponentTokens(tokens);
  spin.show();
  QTest::qWait(30);

  const QImage first = spin.grab().toImage();
  QTest::qWait(80);
  const QImage second = spin.grab().toImage();
  QCOMPARE(second, first);

  spin.setPercent(0.0);
  QCoreApplication::processEvents();
  const QImage zeroProgress = spin.grab().toImage();
  spin.clearProgress();
  QCoreApplication::processEvents();
  const QImage indeterminate = spin.grab().toImage();
  QVERIFY(zeroProgress != indeterminate);
}

void SpinTest::fullscreenSurfaceCoversAndTracksWindow() {
  QWidget host;
  host.resize(320, 180);
  auto* spin = new AdSpin(&host);
  spin->setGeometry(10, 10, 20, 20);
  spin->setFullscreen(true);
  host.show();
  QTest::qWait(20);

  QWidget* surface = host.findChild<QWidget*>(QStringLiteral("spinFullscreenSurface"));
  QVERIFY(surface);
  QVERIFY(surface->isVisible());
  QCOMPARE(surface->geometry(), host.rect());
  host.resize(410, 230);
  QTRY_COMPARE(surface->geometry(), host.rect());

  spin->setSpinning(false);
  QCoreApplication::processEvents();
  QVERIFY(!surface->isVisible());
}

void SpinTest::fullscreenSurfaceRehostsAfterParentChange() {
  QWidget firstHost;
  QWidget secondHost;
  firstHost.resize(240, 160);
  secondHost.resize(300, 190);
  auto* spin = new AdSpin(&firstHost);
  spin->setFullscreen(true);
  firstHost.show();
  secondHost.show();
  QCoreApplication::processEvents();

  QPointer<QWidget> firstSurface =
      firstHost.findChild<QWidget*>(QStringLiteral("spinFullscreenSurface"));
  QVERIFY(firstSurface);
  spin->setParent(&secondHost);
  spin->show();
  QTRY_VERIFY(firstSurface.isNull());
  QWidget* secondSurface = secondHost.findChild<QWidget*>(QStringLiteral("spinFullscreenSurface"));
  QVERIFY(secondSurface);
  QCOMPARE(secondSurface->geometry(), secondHost.rect());
}

void SpinTest::externalDestructionAndInvalidAdoptionAreSafe() {
  QWidget host;
  AdSpin spin(&host);
  QSignalSpy contentSpy(&spin, &AdSpin::contentWidgetChanged);
  QSignalSpy indicatorSpy(&spin, &AdSpin::indicatorWidgetChanged);

  auto* content = new QWidget();
  spin.setContentWidget(content);
  contentSpy.clear();
  delete content;
  QCOMPARE(spin.contentWidget(), nullptr);
  QCOMPARE(contentSpy.count(), 1);

  auto* indicator = new QWidget();
  spin.setIndicatorWidget(indicator);
  indicatorSpy.clear();
  delete indicator;
  QCOMPARE(spin.indicatorWidget(), nullptr);
  QCOMPARE(indicatorSpy.count(), 1);

  spin.setContentWidget(&host);
  spin.setIndicatorWidget(&host);
  QCOMPARE(spin.contentWidget(), nullptr);
  QCOMPARE(spin.indicatorWidget(), nullptr);
  QCOMPARE(spin.parentWidget(), &host);
}

void SpinTest::semanticAndComponentTokensResolve() {
  AdSpin spin;
  adqt::widgets::detail::SpinStyleInput input;
  input.baseFont = spin.font();
  input.sizeClass = AdSpin::SizeClass::Large;
  input.componentTokens.metrics.dotSizeLarge = 44;
  input.componentTokens.colors.indicator = QColor(QStringLiteral("#123456"));
  input.semanticStyles.description.textColor = QColor(QStringLiteral("#654321"));
  const auto style = adqt::widgets::detail::resolveSpinVisualStyle(
      input, adqt::theme::ThemeManager::instance().resolve(&spin));
  QCOMPARE(style.dotSize, 44);
  QCOMPARE(style.indicator, QColor(QStringLiteral("#123456")));
  QCOMPARE(style.description, QColor(QStringLiteral("#654321")));

  AdSpin::SemanticStyles styles;
  styles.root.backgroundColor = QColor(QStringLiteral("#abcdef"));
  spin.setSemanticStyles(styles);
  spin.resetSemanticStyles();
  QVERIFY(!spin.semanticStyles().root.backgroundColor.has_value());
}

void SpinTest::renderedStatesHaveVisibleAntDesignStructure() {
  QWidget canvas;
  canvas.setAutoFillBackground(true);
  QPalette canvasPalette = canvas.palette();
  canvasPalette.setColor(QPalette::Window, QColor(QStringLiteral("#ffffff")));
  canvas.setPalette(canvasPalette);
  canvas.resize(720, 210);
  auto* row = new QHBoxLayout(&canvas);
  row->setContentsMargins(24, 24, 24, 24);
  row->setSpacing(34);

  auto* sizes = new QWidget();
  auto* sizesLayout = new QHBoxLayout(sizes);
  sizesLayout->setContentsMargins(0, 0, 0, 0);
  sizesLayout->setSpacing(20);
  for (const AdSpin::SizeClass size :
       {AdSpin::SizeClass::Small, AdSpin::SizeClass::Medium, AdSpin::SizeClass::Large}) {
    auto* spin = new AdSpin();
    spin->setSizeClass(size);
    sizesLayout->addWidget(spin);
  }

  auto* content = new QLabel(QStringLiteral("Content remains visible\nwhile loading"));
  content->setAlignment(Qt::AlignCenter);
  content->setAutoFillBackground(true);
  QPalette contentPalette = content->palette();
  contentPalette.setColor(QPalette::Window, QColor(QStringLiteral("#e6f4ff")));
  content->setPalette(contentPalette);
  auto* nested = new AdSpin(content, &canvas);
  nested->setDescription(QStringLiteral("Loading"));
  nested->setFixedSize(240, 120);

  auto* progress = new AdSpin();
  progress->setSizeClass(AdSpin::SizeClass::Large);
  progress->setPercent(68.0);
  progress->setDescription(QStringLiteral("68%"));
  progress->setFixedSize(80, 90);

  row->addWidget(sizes);
  row->addWidget(nested);
  row->addWidget(progress);
  row->addStretch();
  canvas.show();
  QTest::qWait(80);

  const QImage image = canvas.grab().toImage().convertToFormat(QImage::Format_ARGB32);
  QVERIFY(!image.isNull());
  int accentPixels = 0;
  for (int y = 0; y < image.height(); ++y) {
    const QRgb* scanline = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      const QColor color = QColor::fromRgba(scanline[x]);
      if (color.blue() > color.red() + 35 && color.blue() > color.green() + 10) {
        ++accentPixels;
      }
    }
  }
  QVERIFY2(accentPixels > 80, "Expected visible Ant Design accent-colored indicators");

  const QString snapshotPath = qEnvironmentVariable("ADQT_SPIN_SNAPSHOT");
  if (!snapshotPath.isEmpty()) {
    QVERIFY2(image.save(snapshotPath), qPrintable(snapshotPath));
  }
}

QTEST_MAIN(SpinTest)
#include "tst_spin.moc"
