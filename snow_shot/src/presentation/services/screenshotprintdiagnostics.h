#ifndef SNOW_SHOT_SCREENSHOTPRINTDIAGNOSTICS_H
#define SNOW_SHOT_SCREENSHOTPRINTDIAGNOSTICS_H

#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/presentation/screenshotprintservice.h"

namespace snow_shot::print_detail {
inline void logPrintEvent(const char* event, const QJsonObject& fields = {},
                          QtMsgType level = QtInfoMsg) {
    diagnostics::logEvent(QStringLiteral("snow_shot.print"), QString::fromLatin1(event), fields,
                          level);
}

inline QString printStatusName(ScreenshotPrintService::Status status) {
    using Status = ScreenshotPrintService::Status;
    switch (status) {
    case Status::Submitted:
        return QStringLiteral("submitted");
    case Status::Cancelled:
        return QStringLiteral("cancelled");
    case Status::Failed:
        return QStringLiteral("failed");
    case Status::Unavailable:
        return QStringLiteral("unavailable");
    case Status::HandedOff:
        return QStringLiteral("handed_off");
    }
    return QStringLiteral("unknown");
}
} // namespace snow_shot::print_detail

#endif
