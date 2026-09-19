#include <windows.h>
#include <cwchar>
#include <shellapi.h>

namespace {
// The uninstaller runs this stub with "--uninstall --target <directory>
// [--upgrade]"; the marker proves the copied helper was invoked with the
// installation directory before any files were deleted.
bool writeMarker(const wchar_t* directory) {
    wchar_t path[MAX_PATH]{};
    if (swprintf_s(path, L"%s\\snow-shot-updater-ran.txt", directory) <= 0) {
        return false;
    }
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const char content[] = "ran";
    const DWORD expected = static_cast<DWORD>(sizeof content - 1);
    DWORD written = 0;
    const bool ok = WriteFile(file, content, expected, &written, nullptr) && written == expected;
    CloseHandle(file);
    return ok;
}
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
    LocalFree(arguments);
    return target && writeMarker(target) ? stubExitCode : 1;
}
