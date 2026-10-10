#pragma once

#include <QAbstractNativeEventFilter>

#include <functional>
#include <memory>

class QKeyEvent;
class QWidget;

namespace snow_shot::platform::macos {

[[nodiscard]] std::unique_ptr<QAbstractNativeEventFilter>
makeWindowShortcutInputFilter(std::function<bool(QWidget*, QKeyEvent&)> dispatch,
                              std::function<QWidget*(quintptr)> receiverForWindow = {});

} // namespace snow_shot::platform::macos
