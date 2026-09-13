#include "field_group.h"

#include "button.h"
#include "detail/button_grouping.h"
#include "theme/theme_manager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QMetaProperty>

#include <algorithm>

namespace adqt::widgets {

namespace {

bool writeJoinedProperty(QWidget* control, const char* propertyName, bool value) {
  if (!control) {
    return false;
  }

  const QMetaObject* metaObject = control->metaObject();
  if (metaObject) {
    const int propertyIndex = metaObject->indexOfProperty(propertyName);
    if (propertyIndex >= 0) {
      const QMetaProperty property = metaObject->property(propertyIndex);
      if (!property.isWritable()) {
        return false;
      }
      return property.write(control, value);
    }
  }

  const QVariant currentValue = control->property(propertyName);
  if (currentValue.isValid() && currentValue.toBool() == value) {
    return false;
  }

  control->setProperty(propertyName, value);
  return false;
}

detail::SegmentPosition segmentPositionForJoinState(bool joinedLeft, bool joinedRight) {
  if (joinedLeft && joinedRight) {
    return detail::SegmentPosition::Middle;
  }
  if (joinedLeft) {
    return detail::SegmentPosition::Trailing;
  }
  if (joinedRight) {
    return detail::SegmentPosition::Leading;
  }
  return detail::SegmentPosition::Standalone;
}

bool isVisibleControl(const QWidget* control) { return control && !control->isHidden(); }

}  // namespace

AdFieldGroup::AdFieldGroup(QWidget* parent) : QWidget(parent) {
  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(0);
}

AdFieldGroup::~AdFieldGroup() = default;

void AdFieldGroup::addControl(QWidget* control, int stretch) {
  insertControl(controlCount(), control, stretch);
}

void AdFieldGroup::insertControl(int index, QWidget* control, int stretch) {
  if (!control) {
    return;
  }

  controls_.erase(std::remove_if(controls_.begin(), controls_.end(),
                                 [control](const ControlEntry& entry) {
                                   return entry.control.isNull() || entry.control.data() == control;
                                 }),
                  controls_.end());

  control->setParent(this);
  control->installEventFilter(this);
  controls_.insert(std::clamp(index, 0, controlCount()), ControlEntry{control, stretch});
  refreshJoinedEdges();
}

int AdFieldGroup::controlCount() const {
  return static_cast<int>(
      std::count_if(controls_.cbegin(), controls_.cend(),
                    [](const ControlEntry& entry) { return !entry.control.isNull(); }));
}

QWidget* AdFieldGroup::controlAt(int index) const {
  if (index < 0) {
    return nullptr;
  }

  int current = 0;
  for (const ControlEntry& entry : controls_) {
    if (entry.control.isNull()) {
      continue;
    }
    if (current == index) {
      return entry.control.data();
    }
    ++current;
  }
  return nullptr;
}

void AdFieldGroup::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
      refreshJoinedEdges();
      break;
    default:
      break;
  }
}

bool AdFieldGroup::eventFilter(QObject* watched, QEvent* event) {
  const bool isTrackedControl =
      std::any_of(controls_.cbegin(), controls_.cend(),
                  [watched](const ControlEntry& entry) { return entry.control.data() == watched; });
  if (!isTrackedControl || !event) {
    return QWidget::eventFilter(watched, event);
  }

  switch (event->type()) {
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::ShowToParent:
    case QEvent::HideToParent:
    case QEvent::ParentChange:
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
      refreshJoinedEdges();
      break;
    default:
      break;
  }

  return QWidget::eventFilter(watched, event);
}

void AdFieldGroup::refreshJoinedEdges() {
  controls_.erase(std::remove_if(controls_.begin(), controls_.end(),
                                 [](const ControlEntry& entry) { return entry.control.isNull(); }),
                  controls_.end());

  const int visibleCount = static_cast<int>(std::count_if(
      controls_.cbegin(), controls_.cend(),
      [](const ControlEntry& entry) { return isVisibleControl(entry.control.data()); }));
  int visibleIndex = 0;

  for (const ControlEntry& entry : controls_) {
    QWidget* control = entry.control.data();
    const bool visible = isVisibleControl(control);
    const bool joinedLeft = visible && visibleIndex > 0;
    const bool joinedRight = visible && visibleIndex + 1 < visibleCount;

    const bool appliedLeft = writeJoinedProperty(control, "joinedLeft", joinedLeft);
    const bool appliedRight = writeJoinedProperty(control, "joinedRight", joinedRight);
    const bool appliedJoinedProperties = appliedLeft || appliedRight;
    if (!appliedJoinedProperties) {
      if (auto* button = qobject_cast<AdButton*>(control)) {
        detail::setButtonSegmentPosition(button,
                                         segmentPositionForJoinState(joinedLeft, joinedRight));
      }
    }

    if (visible) {
      ++visibleIndex;
    }
  }

  rebuildLayout();
}

void AdFieldGroup::rebuildLayout() {
  if (!layout_) {
    return;
  }

  while (QLayoutItem* item = layout_->takeAt(0)) {
    delete item;
  }

  const int overlap = overlapWidth();
  bool firstVisibleControl = true;
  for (const ControlEntry& entry : controls_) {
    QWidget* control = entry.control.data();
    if (!isVisibleControl(control)) {
      continue;
    }

    if (!firstVisibleControl) {
      layout_->addSpacing(-overlap);
    }
    layout_->addWidget(control, entry.stretch);
    firstVisibleControl = false;
  }

  layout_->invalidate();
  updateGeometry();
}

int AdFieldGroup::overlapWidth() const {
  const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
  return std::max(1, qRound(theme.lineWidth));
}

}  // namespace adqt::widgets
