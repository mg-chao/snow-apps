#ifndef SNOW_SHOT_WINDOWS_NATIVEPRINTDIAGNOSTICS_H
#define SNOW_SHOT_WINDOWS_NATIVEPRINTDIAGNOSTICS_H

#include "../../presentation/services/screenshotprintdiagnostics.h"

#include <qt_windows.h>

namespace snow_shot::print_detail {
inline HRESULT logWindowsPrintResult(HRESULT code, const char* backend, const char* stage,
                                     QJsonObject fields = {}) {
    if (FAILED(code)) {
        fields.insert(QStringLiteral("backend"), QString::fromLatin1(backend));
        fields.insert(QStringLiteral("stage"), QString::fromLatin1(stage));
        fields.insert(QStringLiteral("code"), QStringLiteral("0x%1").arg(static_cast<quint32>(code),
                                                                         8, 16, QLatin1Char('0')));
        logPrintEvent("print.native_failed", fields, QtWarningMsg);
    }
    return code;
}
} // namespace snow_shot::print_detail

#endif
