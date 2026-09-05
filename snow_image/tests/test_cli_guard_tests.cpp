// Regression test for the test CLI dialog guard (cmake/test-support).
//
// The guard must keep Debug-CRT reports on stderr even when they are raised
// through the shared dynamic debug CRT instance (ucrtbased.dll) that
// /MD-built dependency DLLs use, instead of through the static CRT instance
// linked into this executable. The trigger DLL is built against the dynamic
// CRT on purpose. Child processes raise its reports while the parent
// captures each child's stderr, so a regression (the modal "Debug Assertion
// Failed!" dialog) shows up as a stalled child that gets terminated and
// reported, never as a silently passing or eternally hanging run.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Defined in the dynamic-CRT trigger DLL. Referenced directly (not through
// LoadLibrary) so the linker keeps the trigger DLL's static import: the
// shared dynamic debug CRT must already be loaded when the CLI guard's
// global constructor runs, mirroring executables that call into /MD-built
// dependency DLLs.
extern "C" void snow_image_trigger_dynamic_crt_assert();
extern "C" void snow_image_trigger_dynamic_crt_assert_storm();

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

std::wstring executablePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    require(length > 0 && length < MAX_PATH, "resolving the test executable path failed");
    return std::wstring(path);
}

struct ChildResult {
    DWORD exitCode = 1;
    std::string stderrText;
};

ChildResult runChildWithCapturedStderr(const wchar_t* argument) {
    wchar_t directory[MAX_PATH] = {};
    const DWORD directoryLength = GetTempPathW(MAX_PATH, directory);
    require(directoryLength > 0 && directoryLength < MAX_PATH,
            "resolving the temp directory failed");
    const std::wstring reportPath =
        std::wstring(directory) + L"snow_image_test_cli_guard_report.txt";

    HANDLE reportFile = CreateFileW(reportPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
    require(reportFile != INVALID_HANDLE_VALUE, "creating the stderr capture file failed");
    SetHandleInformation(reportFile, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = reportFile;
    PROCESS_INFORMATION process = {};
    std::wstring command = executablePath() + L" " + argument;
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                        nullptr, &startup, &process);
    require(created, "spawning the trigger child failed");

    const DWORD waitResult = WaitForSingleObject(process.hProcess, 60000);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        CloseHandle(reportFile);
        DeleteFileW(reportPath.c_str());
        require(false, "trigger child stalled; the CLI guard no longer keeps dynamic-CRT reports "
                       "off modal dialogs");
    }
    ChildResult result;
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    LARGE_INTEGER size = {};
    require(GetFileSizeEx(reportFile, &size), "querying the captured stderr output failed");
    result.stderrText.resize(static_cast<size_t>(size.QuadPart));
    SetFilePointer(reportFile, 0, nullptr, FILE_BEGIN);
    DWORD readBytes = 0;
    require(ReadFile(reportFile, result.stderrText.data(),
                     static_cast<DWORD>(result.stderrText.size()), &readBytes, nullptr) &&
                readBytes == result.stderrText.size(),
            "reading the captured stderr output failed");
    CloseHandle(reportFile);
    DeleteFileW(reportPath.c_str());
    return result;
}

void runTriggerChild() {
    snow_image_trigger_dynamic_crt_assert();
}

void runTriggerStormChild() {
    snow_image_trigger_dynamic_crt_assert_storm();
    // The guard's trip-wire must terminate the process before the storm
    // loop finishes; reaching this marker means it did not.
    std::exit(3);
}

void runParentGuardChecks() {
    const ChildResult report = runChildWithCapturedStderr(L"--trigger-child");
    require(report.exitCode == 0, "trigger child exited with a failure");
    require(report.stderrText.find("snow-test-cli-guard dynamic-instance report") !=
                std::string::npos,
            "the dynamic-CRT assertion report did not reach stderr");

    const ChildResult storm = runChildWithCapturedStderr(L"--trigger-storm-child");
    require(storm.exitCode == EXIT_FAILURE,
            "assertion-report storm was not terminated by the guard");
    require(storm.stderrText.find("[crt.guard] terminating") != std::string::npos,
            "the guard trip-wire message did not reach stderr");
}

} // namespace

int main(int argc, char** argv) {
#if defined(_DEBUG)
    if (argc == 2 && std::strcmp(argv[1], "--trigger-child") == 0) {
        runTriggerChild();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--trigger-storm-child") == 0) {
        runTriggerStormChild();
        return 0;
    }
    runParentGuardChecks();
    std::printf("snow_image test CLI guard tests passed\n");
    return 0;
#else
    // Release CRT flavors have no debug report machinery to redirect.
    static_cast<void>(argc);
    static_cast<void>(argv);
    std::printf("snow_image test CLI guard tests skipped (release build)\n");
    return 0;
#endif
}
