// Dynamic-CRT trigger DLL for the test CLI guard regression test. Built
// against the dynamic debug CRT on purpose (see test_cli_guard_tests.cpp):
// the report it raises runs through the shared ucrtbased.dll instance, not
// the static CRT instance linked into the test executable.

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

extern "C" __declspec(dllexport) void snow_image_trigger_dynamic_crt_assert() {
#if defined(_DEBUG)
    _CrtDbgReport(_CRT_ASSERT, __FILE__, __LINE__, "snow_image_test_cli_guard",
                  "snow-test-cli-guard dynamic-instance report");
#endif
}

extern "C" __declspec(dllexport) void snow_image_trigger_dynamic_crt_assert_storm() {
#if defined(_DEBUG)
    // More reports than the CLI guard's trip-wire threshold: the guard must
    // terminate the process instead of letting the loop run to completion.
    for (int i = 0; i < 10010; ++i) {
        _CrtDbgReport(_CRT_ASSERT, __FILE__, __LINE__, "snow_image_test_cli_guard",
                      "snow-test-cli-guard dynamic-instance report");
    }
#endif
}
