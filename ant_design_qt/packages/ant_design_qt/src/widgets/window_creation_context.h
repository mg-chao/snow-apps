#pragma once

#include <QPointer>
#include <QVariant>
#include <QWidget>
#include <optional>

namespace adqt::widgets {

// QWidget creates its native surface before callers can set QWindow's transient
// parent. Publish that intended owner during creation so platform adapters can
// choose the correct native window type without replacing the first surface.
// This is temporary context, not a second source of persistent ownership.
class ScopedWindowCreationOwner final {
 public:
  ScopedWindowCreationOwner(QWidget* window, QWidget* owner)
      : window_(window), previous_(window->property(propertyName)) {
    window->setProperty(propertyName,
                        QVariant::fromValue(QPointer<QWidget>(owner ? owner->window() : nullptr)));
  }

  ~ScopedWindowCreationOwner() {
    if (window_) {
      window_->setProperty(propertyName, previous_);
    }
  }

  // A present, null owner explicitly means no transient owner. QPointer also
  // makes owner destruction during synchronous Qt callbacks safe.
  static std::optional<QPointer<QWidget>> ownerFor(const QWidget* window) {
    const QVariant owner = window->property(propertyName);
    if (!owner.isValid()) {
      return std::nullopt;
    }
    return owner.value<QPointer<QWidget>>();
  }

 private:
  Q_DISABLE_COPY_MOVE(ScopedWindowCreationOwner)
  static constexpr auto propertyName = "adqt.window.creationOwner";
  QPointer<QWidget> window_;
  QVariant previous_;
};

}  // namespace adqt::widgets
