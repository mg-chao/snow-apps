#include "theme/theme_manager.h"
#include "widgets/interaction_overlay_manager.h"
#include "widgets/input_line_edit.h"
#include "widgets/select.h"

#include <QApplication>
#include <QImage>
#include <QScrollArea>
#include <QScrollBar>

#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

using namespace adqt::widgets;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool sameCoverage(const QRegion& actual, const QRegion& expected) {
  // Regions can partition identical coverage into different rectangles.
  return actual.xored(expected).isEmpty();
}

void settle() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents();
}

void setMotion(bool enabled) {
  auto& themes = adqt::theme::ThemeManager::instance();
  auto config = themes.config();
  config.motion = enabled;
  themes.setConfig(config);
}

QRegion ringPixels(QWidget& window) {
  const QImage image = window.grab().toImage().convertToFormat(QImage::Format_RGB32);
  require(image.devicePixelRatio() == 1.0, "pixel fixture requires unit scale");
  QRegion pixels;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor color = image.pixelColor(x, y);
      if (color.red() > color.green() + 15 && color.blue() > color.green() + 15) {
        pixels += QRect(x, y, 1, 1);
      }
    }
  }
  return pixels;
}

QRegion overlayFootprint(QWidget& window) {
  QRegion footprint;
  for (QWidget* widget : window.findChildren<QWidget*>()) {
    if (widget->isVisible() && widget->property("adqt.interaction.overlay").toBool()) {
      require(!widget->mask().isEmpty(), "visible overlays must have a bounded native footprint");
      footprint += widget->mask().translated(widget->mapTo(&window, QPoint(0, 0)));
    }
  }
  return footprint;
}

void focus(QWidget& owner, const QRectF& localRect = {}) {
  InteractionFocusRequest request;
  request.owner = &owner;
  request.baseRectInWindow = (localRect.isValid() ? localRect : QRectF(owner.rect()))
                                 .translated(owner.mapTo(owner.window(), QPoint(0, 0)));
  request.color = QColor(255, 0, 255);
  request.strokeWidth = 3.0;
  triggerInteractionFocus(request);
}

struct ScrollFixture {
  QWidget window;
  QScrollArea area{&window};
  QWidget* content = new QWidget;
  QWidget control{content};

  explicit ScrollFixture(bool surface) {
    window.resize(420, 300);
    area.setGeometry(40, 40, 300, 200);
    area.setFrameShape(QFrame::NoFrame);
    area.setProperty("adqt.interaction.surface", surface);
    content->resize(600, 500);
    control.setGeometry(140, 110, 100, 36);
    area.setWidget(content);
    window.show();
    settle();
  }

  QRegion viewportRegion() const {
    return QRegion(QRect(area.viewport()->mapTo(&window, QPoint(0, 0)), area.viewport()->size()));
  }
};

void focusFollowsScrolling(bool motion, bool surface) {
  setMotion(motion);
  ScrollFixture fixture(surface);
  focus(fixture.control);
  const QRegion before = ringPixels(fixture.window);
  const QRegion footprint = overlayFootprint(fixture.window);
  require(!before.isEmpty(), "focus request must paint a ring");
  fixture.area.horizontalScrollBar()->setValue(45);
  fixture.area.verticalScrollBar()->setValue(60);
  // Inspect the very next paint, without waiting for an animation frame.
  require(
      sameCoverage(ringPixels(fixture.window), before.translated(-45, -60)),
      "scrolling ancestors must move the focus ring before the next paint, with or without motion");
  require(sameCoverage(overlayFootprint(fixture.window), footprint.translated(-45, -60)),
          "scrolling must also move the overlay's native footprint");
}

void focusClipsToViewport(bool surface) {
  setMotion(false);
  ScrollFixture fixture(surface);
  focus(fixture.control);
  const QRegion before = ringPixels(fixture.window);
  fixture.area.verticalScrollBar()->setValue(125);
  require(sameCoverage(ringPixels(fixture.window),
                       before.translated(0, -125) & fixture.viewportRegion()),
          "a partially scrolled focus ring must clip to the viewport");
  require((overlayFootprint(fixture.window) - fixture.viewportRegion()).isEmpty(),
          "the native footprint must obey viewport clipping too");
  fixture.area.verticalScrollBar()->setValue(200);
  require(ringPixels(fixture.window).isEmpty() && overlayFootprint(fixture.window).isEmpty(),
          "fully scrolled controls must leave no visible or native overlay");
  fixture.area.verticalScrollBar()->setValue(0);
  require(sameCoverage(ringPixels(fixture.window), before),
          "scrolling back must restore the ring without a new focus request");
  fixture.area.resize(300, 130);
  require(sameCoverage(ringPixels(fixture.window), before & fixture.viewportRegion()),
          "resizing the viewport must update clipping even when the control does not move");
}

void focusTracksAncestorLifecycle(bool surface) {
  setMotion(false);
  ScrollFixture fixture(surface);
  focus(fixture.control);
  const QRegion before = ringPixels(fixture.window);
  fixture.area.move(fixture.area.pos() + QPoint(15, 10));
  require(sameCoverage(ringPixels(fixture.window), before.translated(15, 10)),
          "moving a container must move its descendant's focus ring");
  fixture.content->hide();
  require(ringPixels(fixture.window).isEmpty() && overlayFootprint(fixture.window).isEmpty(),
          "hiding an ancestor must remove both painting and native coverage");
  fixture.content->show();
  require(sameCoverage(ringPixels(fixture.window), before.translated(15, 10)),
          "showing an ancestor must restore a retained focus request");
  fixture.content->setEnabled(false);
  require(ringPixels(fixture.window).isEmpty(), "disabled ancestors must not retain visible rings");
  fixture.content->setEnabled(true);
  require(sameCoverage(ringPixels(fixture.window), before.translated(15, 10)),
          "enabling an ancestor must restore its ring");
}

void nestedViewportsClipIndependently() {
  setMotion(false);
  ScrollFixture fixture(false);
  QScrollArea nested(fixture.content);
  nested.setFrameShape(QFrame::NoFrame);
  nested.setGeometry(80, 90, 200, 150);
  auto* content = new QWidget;
  content->resize(400, 400);
  QWidget control(content);
  control.setGeometry(40, 50, 100, 36);
  nested.setWidget(content);
  nested.show();
  settle();
  focus(control);
  const QRegion before = ringPixels(fixture.window);
  require(!before.isEmpty(), "nested viewport fixture must initially display a ring");
  nested.verticalScrollBar()->setValue(65);
  fixture.area.verticalScrollBar()->setValue(100);
  const QRegion inner(
      QRect(nested.viewport()->mapTo(&fixture.window, QPoint(0, 0)), nested.viewport()->size()));
  require(sameCoverage(ringPixels(fixture.window),
                       before.translated(0, -165) & inner & fixture.viewportRegion()),
          "nested scrolling must intersect all ancestor viewports");
}

void wavesFollowOwnerGeometry(bool surface) {
  setMotion(true);
  ScrollFixture fixture(surface);
  InteractionWaveRequest request;
  request.owner = &fixture.control;
  request.baseRectInWindow =
      QRectF(fixture.control.mapTo(&fixture.window, QPoint(0, 0)), fixture.control.size());
  request.color = QColor(255, 0, 255);
  triggerInteractionWave(request);
  const QRegion before = overlayFootprint(fixture.window);
  require(!before.isEmpty(), "a wave must create an overlay footprint");
  fixture.area.horizontalScrollBar()->setValue(45);
  fixture.area.verticalScrollBar()->setValue(125);
  require(sameCoverage(overlayFootprint(fixture.window),
                       before.translated(-45, -125) & fixture.viewportRegion()),
          "waves must follow their owners and clip just like focus rings");
  const QRegion painted = ringPixels(fixture.window);
  require(!painted.isEmpty() && (painted - overlayFootprint(fixture.window)).isEmpty(),
          "wave painting must use the same geometry and clipping as its footprint");
}

void retiredOwnersAndReparentedContainersLeaveNoRing() {
  setMotion(false);
  ScrollFixture fixture(false);
  auto* owner = new QWidget(fixture.content);
  owner->setGeometry(140, 110, 100, 36);
  owner->show();
  focus(*owner);
  require(!ringPixels(fixture.window).isEmpty(), "retirement fixture must display a ring");
  delete owner;
  settle();
  require(ringPixels(fixture.window).isEmpty() && overlayFootprint(fixture.window).isEmpty(),
          "destroying an owner must remove its overlay without animation ticks");

  QWidget destination;
  destination.resize(420, 300);
  destination.show();
  auto* container = new QWidget(fixture.content);
  container->setGeometry(100, 60, 150, 100);
  QWidget control(container);
  control.setGeometry(20, 20, 100, 36);
  container->show();
  focus(control);
  container->setParent(&destination);
  container->show();
  require(ringPixels(fixture.window).isEmpty() && overlayFootprint(fixture.window).isEmpty(),
          "reparenting an ancestor must retire its old host's ring and footprint");
  focus(control);
  const QRegion before = ringPixels(destination);
  require(!before.isEmpty(), "a reparented control must support requests in its new host");
  container->move(40, 50);
  require(sameCoverage(ringPixels(destination), before.translated(-60, -10)),
          "a reparented control must track its new ancestor chain");
}

void surfaceMappingPreservesFractionalGeometry() {
  setMotion(false);
  ScrollFixture fixture(false);
  const QRectF shape(0.25, 0.75, 98.5, 35.25);
  focus(fixture.control, shape);
  const QRegion before = ringPixels(fixture.window);
  fixture.area.setProperty("adqt.interaction.surface", true);
  focus(fixture.control, shape);
  require(sameCoverage(ringPixels(fixture.window), before),
          "mapping into a surface must preserve fractional focus geometry");
}

void overlaysFollowSiblingStacking(bool surface) {
  setMotion(false);
  ScrollFixture fixture(surface);
  QWidget sibling(surface ? static_cast<QWidget*>(&fixture.area) : &fixture.window);
  sibling.setGeometry(sibling.parentWidget()->rect());
  sibling.setAutoFillBackground(true);
  focus(fixture.control);
  const QRegion before = ringPixels(fixture.window);
  sibling.show();
  sibling.raise();
  settle();
  require(sameCoverage(ringPixels(fixture.window), before),
          "showing or raising siblings must preserve overlay stacking without animation polling");
}

void movingSurfacesUpdatePaintingAndFootprint() {
  setMotion(false);
  ScrollFixture fixture(false);
  QWidget container(&fixture.window);
  container.setGeometry(0, 0, 420, 300);
  QWidget surface(&container);
  surface.setProperty("adqt.interaction.surface", true);
  surface.setGeometry(170, 140, 60, 60);
  focus(fixture.control);
  const QRegion before = ringPixels(fixture.window);
  const QRegion footprint = overlayFootprint(fixture.window);
  container.show();
  settle();
  auto surfaceRegion = [&] {
    return QRegion(QRect(surface.mapTo(&fixture.window, QPoint(0, 0)), surface.size()));
  };
  require(sameCoverage(ringPixels(fixture.window), before - surfaceRegion()) &&
              sameCoverage(overlayFootprint(fixture.window), footprint - surfaceRegion()),
          "popup surfaces must clip both focus painting and native coverage");
  container.move(45, 10);
  settle();
  require(sameCoverage(overlayFootprint(fixture.window), footprint - surfaceRegion()),
          "moving a surface ancestor must update native coverage without animation polling");
  require(sameCoverage(ringPixels(fixture.window), before - surfaceRegion()),
          "moving a surface ancestor must update painted clipping without animation polling");
  container.hide();
  settle();
  require(sameCoverage(ringPixels(fixture.window), before) &&
              sameCoverage(overlayFootprint(fixture.window), footprint),
          "hiding a surface ancestor must restore the uncovered ring");
}

void reparentingWithinHostUpdatesObservers(bool surface) {
  setMotion(false);
  ScrollFixture fixture(surface);
  QWidget newParent(fixture.content);
  newParent.setGeometry(10, 15, 400, 300);
  newParent.show();
  QWidget control(fixture.content);
  control.setGeometry(140, 110, 100, 36);
  control.show();
  focus(control);
  const QRegion before = ringPixels(fixture.window);
  control.setParent(&newParent);
  control.move(140, 110);
  control.show();
  require(sameCoverage(ringPixels(fixture.window), before.translated(10, 15)),
          "same-host reparenting must update owner coordinates");
  newParent.move(25, 30);
  require(sameCoverage(ringPixels(fixture.window), before.translated(25, 30)),
          "same-host reparenting must subscribe to the new ancestors");
  stopInteractionFocusForOwner(&control);
  newParent.move(50, 60);
  settle();
  require(ringPixels(fixture.window).isEmpty() && overlayFootprint(fixture.window).isEmpty(),
          "stopping a track must detach its geometry observers and leave no overlay");
}

template <typename Control>
void realControlFocusFollowsScrolling() {
  setMotion(false);
  ScrollFixture fixture(false);
  Control control(fixture.content);
  control.setGeometry(140, 110, 100, 36);
  QWidget* focusTarget = &control;
  if constexpr (std::is_same_v<Control, AdSelect>) {
    control.setSearchEnabled(true);
    focusTarget = control.template findChild<QLineEdit*>();
    require(focusTarget, "searchable selects must expose their input");
  }
  control.show();
  fixture.window.activateWindow();
  focusTarget->setFocus(Qt::TabFocusReason);
  settle();
  require(focusTarget->hasFocus(), "real control fixture must acquire focus");
  const QRegion before = overlayFootprint(fixture.window);
  require(!before.isEmpty(), "real controls must submit a focus overlay");
  fixture.area.verticalScrollBar()->setValue(60);
  require(sameCoverage(overlayFootprint(fixture.window), before.translated(0, -60)),
          "real controls must track scrolling without receiving their own move event");
  control.resize(140, 36);
  require(
      overlayFootprint(fixture.window).boundingRect().width() == before.boundingRect().width() + 40,
      "resizing the control must replace the requested focus shape");
  focusTarget->clearFocus();
  require(overlayFootprint(fixture.window).isEmpty(),
          "focus loss must remove the native footprint");
}

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_SCALE_FACTOR", "1");
  QApplication app(argc, argv);
  int failures = 0;
  const auto check = [&](const auto& run) {
    try {
      run();
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << error.what() << '\n';
    }
  };
  for (const bool motion : {false, true}) {
    for (const bool surface : {false, true}) {
      check([&] { focusFollowsScrolling(motion, surface); });
    }
  }
  for (const bool surface : {false, true}) {
    check([&] { focusClipsToViewport(surface); });
    check([&] { focusTracksAncestorLifecycle(surface); });
    check([&] { wavesFollowOwnerGeometry(surface); });
    check([&] { overlaysFollowSiblingStacking(surface); });
    check([&] { reparentingWithinHostUpdatesObservers(surface); });
  }
  check(nestedViewportsClipIndependently);
  check(retiredOwnersAndReparentedContainersLeaveNoRing);
  check(surfaceMappingPreservesFractionalGeometry);
  check(movingSurfacesUpdatePaintingAndFootprint);
  check(realControlFocusFollowsScrolling<AdLineEdit>);
  check(realControlFocusFollowsScrolling<AdSelect>);
  if (failures == 0) std::cout << "Interaction overlay tests passed\n";
  return failures == 0 ? 0 : 1;
}
