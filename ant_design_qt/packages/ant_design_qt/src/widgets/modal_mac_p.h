#pragma once

#include <memory>

class QWidget;

namespace adqt::widgets::detail {

void applyMacModalChrome(QWidget* widget);

class MacModalSession {
 public:
  virtual ~MacModalSession() = default;
  virtual void synchronize() = 0;
  virtual void beginHide() = 0;
};

std::unique_ptr<MacModalSession> createMacModalSession(QWidget* surface, QWidget* blocker);

}  // namespace adqt::widgets::detail
