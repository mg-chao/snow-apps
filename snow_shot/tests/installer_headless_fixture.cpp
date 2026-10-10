#include <windows.h>
#include <cwchar>
#include <shellapi.h>

// MCP and update services have no top-level window or attached console.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    if (wcsncmp(arguments, L"--uninstall", 11) == 0) {
        return 0;
    }
    HANDLE heldFile = INVALID_HANDLE_VALUE;
    if (wcsncmp(arguments, L"--hold-file", 11) == 0) {
        int count = 0;
        LPWSTR* command = CommandLineToArgvW(GetCommandLineW(), &count);
        if (command && count == 3) {
            heldFile = CreateFileW(command[2], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        LocalFree(command);
        if (heldFile == INVALID_HANDLE_VALUE) {
            return 1;
        }
    }
    wchar_t readyName[80]{};
    swprintf_s(readyName, L"Local\\SnowShotInstallerTest-%lu", GetCurrentProcessId());
    const HANDLE ready = CreateEventW(nullptr, TRUE, TRUE, readyName);
    if (!ready) {
        return 1;
    }
    Sleep(INFINITE);
    CloseHandle(ready);
    if (heldFile != INVALID_HANDLE_VALUE) {
        CloseHandle(heldFile);
    }
    return 0;
}
