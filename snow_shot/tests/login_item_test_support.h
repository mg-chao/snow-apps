#ifndef SNOW_SHOT_TESTS_LOGIN_ITEM_TEST_SUPPORT_H
#define SNOW_SHOT_TESTS_LOGIN_ITEM_TEST_SUPPORT_H

#include "snow_shot/platform/macos/loginitemservice.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cstdlib>
#include <iostream>

namespace login_item_test {
template <typename Predicate> void waitFor(Predicate predicate) {
    QElapsedTimer timeout;
    timeout.start();
    while (!predicate()) {
        if (timeout.elapsed() > 10000) {
            std::cerr << "Timed out waiting for login item work\n";
            std::exit(EXIT_FAILURE);
        }
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}
inline void idle(snow_shot::platform::macos::LoginItemService& service) {
    waitFor([&] { return !service.pending() && !service.refreshing(); });
    QCoreApplication::processEvents();
}
inline void refresh(snow_shot::platform::macos::LoginItemService& service) {
    service.refresh();
    idle(service);
}
inline snow_shot::platform::macos::LoginItemResult
change(snow_shot::platform::macos::LoginItemService& service, bool enabled,
       bool automatic = false) {
    const auto admitted = automatic ? service.initialize(enabled) : service.setEnabled(enabled);
    if (!admitted.success)
        return admitted;
    const auto completion = service.completionFuture();
    idle(service);
    return completion.get();
}
} // namespace login_item_test
#endif
