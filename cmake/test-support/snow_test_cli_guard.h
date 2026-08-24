#pragma once

// Process-wide guards that keep native failure reports on stderr instead of
// modal dialogs. Header-only so the Qt-free and Qt-aware guard translation
// units share one implementation; safe to call more than once.

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace snow_test_cli {

inline void install_native_guards() {
#if defined(_WIN32)
    // Hard errors and crash faults must terminate the process with a plain
    // exit code instead of a Windows error-reporting popup, so command-line
    // test runners only observe the exit status.
    const UINT previousErrorMode = SetErrorMode(0);
    SetErrorMode(previousErrorMode | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
#endif
#if defined(_MSC_VER)
    // abort() must not raise the CRT report dialog or a Watson fault report.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#if defined(_DEBUG)
    // Debug-CRT assertions and errors (assert(), CRT internal checks) are
    // reported through _CrtDbgReport; route those reports to stderr instead
    // of the "Debug Error!" dialog. This also covers Qt's own fatal-message
    // report whenever Qt shares this executable's CRT instance.
    const int reportTypes[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
    for (const int reportType : reportTypes) {
        _CrtSetReportMode(reportType, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR);
    }
#endif
#endif
}

} // namespace snow_test_cli
