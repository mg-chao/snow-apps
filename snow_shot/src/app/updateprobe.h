#pragma once

#include <QByteArray>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>

#include <functional>
#include <optional>

namespace snow_shot::app {

// Inspect before QApplication or live startup initialization. A recognized probe must never
// fall through to normal startup: 0 admits the probe, 3 rejects its version, and 64 rejects
// malformed arguments. An absent result means this is an ordinary application invocation.
inline std::optional<int> inspectUpdateProbeRequest(int argc, const char* const* argv,
                                                    const QString& expectedVersion) {
    if (argc < 2 || QByteArray(argv[1]) != "--update-probe")
        return std::nullopt;
    if (argc != 3)
        return 64;
    return QString::fromLocal8Bit(argv[2]) == expectedVersion ? 0 : 3;
}

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
