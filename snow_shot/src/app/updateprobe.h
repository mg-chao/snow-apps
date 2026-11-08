#pragma once

#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>

#include <functional>

namespace snow_shot::app {

// The updater's health check has no startup-registration or live-settings port.
// Both configuration and presentation initialization must use the temporary storage root.
// All operations are required; shutdownStorage must not throw.
struct UpdateProbeOperations {
    std::function<bool(const QString&)> initializeStorage;
    std::function<void()> initializePresentation;
    std::function<void()> shutdownStorage;
};

inline int runIsolatedUpdateProbe(const UpdateProbeOperations& operations) {
    QTemporaryDir directory;
    if (!directory.isValid())
        return 4;
    // Initialization may publish degraded repositories before reporting failure. Always close
    // those resources while their temporary root still exists, including a thrown initializer.
    const auto shutdown = qScopeGuard([&] { operations.shutdownStorage(); });
    if (!operations.initializeStorage(directory.path()))
        return 5;
    operations.initializePresentation();
    return 0;
}

} // namespace snow_shot::app
