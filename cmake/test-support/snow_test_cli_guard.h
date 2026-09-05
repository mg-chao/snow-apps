#pragma once

// Process-wide guards that keep native failure reports on stderr instead of
// modal dialogs. Header-only so the Qt-free and Qt-aware guard translation
// units share one implementation; safe to call more than once.

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace snow_test_cli {

#if defined(_MSC_VER) && defined(_DEBUG)
// Debug-heap misuse can enter loops that fire the same assertion report
// forever (for example cross-CRT frees unwinding an exit-time cleanup list).
// Reports through a report hook that lets the default stderr output happen
// while bounding the total count, so repeated-report loops end as a stderr
// diagnostic plus a failure exit code instead of stalling the test run.
inline int cliFriendlyCrtReportHook(int reportType, char* message, int* returnValue) {
    static_cast<void>(message);
    static_cast<void>(returnValue);
    constexpr int maximumAssertReports = 10000;
    static int assertReports = 0;
    if (reportType == _CRT_ASSERT && ++assertReports > maximumAssertReports) {
        std::fputs("[crt.guard] terminating after repeated debug-CRT assertion reports; the "
                   "reporting loop would otherwise stall the test run\n",
                   stderr);
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
    // Let the configured report modes print the report.
    return 0;
}
#endif

#if defined(_MSC_VER) && defined(_DEBUG) && defined(_WIN32)
// The direct _CrtSetReportMode/_CrtSetReportFile calls below only reach the
// CRT instance this executable was linked against. Dependency DLLs built
// against the dynamic debug CRT (vcpkg's dynamic triplet) report through the
// shared ucrtbased.dll instance instead, so configure that instance too:
// without it, assertions raised inside those DLLs (for example cross-CRT
// frees in mixed static/dynamic runtime layouts) pop the modal
// "Debug Assertion Failed!" dialog and stall command-line test runs.
inline void route_loaded_dynamic_crt_reports_to_stderr() {
    // GetModuleHandle, not LoadLibrary: only the shared instance the process
    // has already loaded through its dependency DLLs is configured.
    const HMODULE dynamicCrt = GetModuleHandleW(L"ucrtbased.dll");
    if (dynamicCrt == nullptr) {
        return;
    }
    using SetReportModeFn = int(__cdecl*)(int, int);
    using SetReportFileFn = void*(__cdecl*)(int, void*);
    using SetReportHookFn = _CRT_REPORT_HOOK(__cdecl*)(_CRT_REPORT_HOOK);
    using SetAbortBehaviorFn = void(__cdecl*)(unsigned int, unsigned int);
    const auto setReportMode = reinterpret_cast<SetReportModeFn>(
        reinterpret_cast<void*>(GetProcAddress(dynamicCrt, "_CrtSetReportMode")));
    const auto setReportFile = reinterpret_cast<SetReportFileFn>(
        reinterpret_cast<void*>(GetProcAddress(dynamicCrt, "_CrtSetReportFile")));
    const auto setReportHook = reinterpret_cast<SetReportHookFn>(
        reinterpret_cast<void*>(GetProcAddress(dynamicCrt, "_CrtSetReportHook")));
    const auto setAbortBehavior = reinterpret_cast<SetAbortBehaviorFn>(
        reinterpret_cast<void*>(GetProcAddress(dynamicCrt, "_set_abort_behavior")));
    if (setReportMode != nullptr && setReportFile != nullptr) {
        const int reportTypes[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
        for (const int reportType : reportTypes) {
            setReportMode(reportType, _CRTDBG_MODE_FILE);
            setReportFile(reportType, _CRTDBG_FILE_STDERR);
        }
    }
    if (setReportHook != nullptr) {
        setReportHook(&cliFriendlyCrtReportHook);
    }
    if (setAbortBehavior != nullptr) {
        // Post-assert abort paths in the DLL's CRT instance must stay silent
        // as well, or they would raise the retry dialog the report modes
        // above avoid.
        setAbortBehavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
}
#endif

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
    _CrtSetReportHook(&cliFriendlyCrtReportHook);
#endif
#if defined(_DEBUG) && defined(_WIN32)
    route_loaded_dynamic_crt_reports_to_stderr();
#endif
#endif
}

} // namespace snow_test_cli
