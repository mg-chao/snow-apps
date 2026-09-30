#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/diagnostics/diagnostics.h"

namespace snow_shot::platform {
namespace {
extern "C" void reportQoSError(int code) {
    diagnostics::logEvent(QStringLiteral("snow_shot.platform"),
                          QStringLiteral("scheduling.qos_failed"), {{QStringLiteral("code"), code}},
                          QtWarningMsg);
}
} // namespace

void initializeApplicationQoS(ApplicationQoS qos) {
    const int code =
        snow_application_qos_initialize(static_cast<unsigned int>(qos), reportQoSError);
    if (code != 0)
        reportQoSError(code);
    applyApplicationQoSToCurrentThread();
}

} // namespace snow_shot::platform
