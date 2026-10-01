#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <shellapi.h>

namespace {
// The uninstaller runs this stub with "--uninstall --target <directory>
// [--upgrade]"; the marker proves the copied helper was invoked with the
// installation directory before any files were deleted.
bool writeMarker(const wchar_t* directory, const wchar_t* name, const char* content) {
    wchar_t path[MAX_PATH]{};
    if (swprintf_s(path, L"%s\\%s", directory, name) <= 0) {
        return false;
    }
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD expected = static_cast<DWORD>(std::strlen(content));
    DWORD written = 0;
    const bool ok = WriteFile(file, content, expected, &written, nullptr) && written == expected;
    CloseHandle(file);
    return ok;
}

#ifndef SNOW_SHOT_UPDATER_STUB_FAIL
// Stand in for --launch-desktop in installer tests and record the real child
// PID. The production helper's desktop-shell/token behavior is tested separately.
bool launchDesktop(const wchar_t* directory) {
#ifdef SNOW_SHOT_UPDATER_STUB_MINI
    constexpr auto executable = L"snow_shot_mini";
#else
    constexpr auto executable = L"snow_shot";
#endif
    wchar_t command[MAX_PATH * 2]{};
    if (swprintf_s(command, L"\"%s\\bin\\%s.exe\" --show-main-window", directory, executable) <=
        0) {
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, 0, nullptr, directory, &startup,
                        &process)) {
        return false;
    }
    char pid[32]{};
    sprintf_s(pid, "%lu", process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return writeMarker(directory, L"snow-shot-desktop-launch.txt", pid);
}
#endif
} // namespace

#ifdef SNOW_SHOT_UPDATER_STUB_FAIL
constexpr int stubExitCode = 17;
#else
constexpr int stubExitCode = 0;
#endif

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) {
        return stubExitCode;
    }
    const wchar_t* target = nullptr;
    for (int index = 0; index + 1 < argumentCount; ++index) {
        if (wcscmp(arguments[index], L"--target") == 0) {
            target = arguments[index + 1];
        }
    }
    int result = 1;
    if (target) {
        if (argumentCount > 1 && wcscmp(arguments[1], L"--launch-desktop") == 0) {
#ifdef SNOW_SHOT_UPDATER_STUB_FAIL
            result = stubExitCode;
#else
            result = launchDesktop(target) ? 0 : 1;
#endif
        } else {
            result = writeMarker(target, L"snow-shot-updater-ran.txt", "ran") ? stubExitCode : 1;
        }
    }
    LocalFree(arguments);
    return result;
}
