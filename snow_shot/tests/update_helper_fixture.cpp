// A native parent/relaunch fixture for the actual updater executable. No user settings or UI.
#include <Windows.h>
#include <sddl.h>

#include <string>
#include <vector>

namespace {
void writeFile(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ExitProcess(11);
    }
    DWORD written = 0;
    const bool ok =
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size();
    CloseHandle(file);
    if (!ok) {
        ExitProcess(12);
    }
}

std::string userSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        ExitProcess(13);
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> storage(size);
    if (!GetTokenInformation(token, TokenUser, storage.data(), size, &size)) {
        ExitProcess(14);
    }
    LPSTR sid = nullptr;
    if (!ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) {
        ExitProcess(15);
    }
    const std::string result(sid);
    LocalFree(sid);
    CloseHandle(token);
    return result;
}

bool isElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) ||
        !GetTokenInformation(token, TokenElevation, &elevation, size, &size)) {
        ExitProcess(16);
    }
    CloseHandle(token);
    return elevation.TokenIsElevated != 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t telemetry[32768]{};
    const DWORD telemetryLength =
        GetEnvironmentVariableW(L"SNOW_SHOT_HELPER_CANARY_RESULTS", telemetry, 32768);
    if (argc == 2 && std::wstring(argv[1]) == L"--show-main-window") {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        std::wstring root(executable);
        root.resize(root.find_last_of(L"\\/"));
        root.resize(root.find_last_of(L"\\/"));
        if (telemetryLength > 0 && telemetryLength < 32768) {
            root = telemetry;
        }
        writeFile(root + L"\\.canary-restarted.txt", userSid());
        writeFile(root + L"\\.canary-restarted-elevation.txt",
                  isElevated() ? "elevated" : "standard");
        return 0;
    }
    if (argc != 4) {
        return 2;
    }
    const std::wstring root(argv[1]);
    const std::wstring helper(argv[2]);
    const bool cancel = std::wstring(argv[3]) == L"cancel";
    const bool elevationCancel = std::wstring(argv[3]) == L"elevation-cancel";
    const std::wstring results = telemetryLength > 0 && telemetryLength < 32768 ? telemetry : root;
    writeFile(results + L"\\.canary-parent-elevation.txt", isElevated() ? "elevated" : "standard");
    const std::wstring name = L"snow-shot-helper-canary-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring pipePath = L"\\\\.\\pipe\\" + name;
    HANDLE pipe = CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096,
                                   0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return 3;
    }
    std::wstring command = L"\"" + helper + L"\" --launch --recovery --target \"" + root +
                           L"\" --parent " + std::to_wstring(GetCurrentProcessId()) + L" --pipe " +
                           name + L" --result \"" + results + L"\\.canary-result.txt\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, root.c_str(), &startup, &process)) {
        return 4;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        return 5;
    }
    std::string status;
    char byte = 0;
    DWORD read = 0;
    while (status.size() < 4096 && ReadFile(pipe, &byte, 1, &read, nullptr) && read == 1) {
        if (byte == '\n') {
            break;
        }
        status += byte;
    }
    writeFile(results + L"\\.canary-handoff.txt", status);
    if (status != "ready") {
        CloseHandle(pipe);
        if (elevationCancel && status.find("failed:") == 0) {
            Sleep(120000);
        }
        return 6;
    }
    const std::string answer = cancel ? "cancel\n" : "go\n";
    DWORD written = 0;
    if (!WriteFile(pipe, answer.data(), static_cast<DWORD>(answer.size()), &written, nullptr)) {
        return 7;
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    if (cancel) {
        // The driver verifies this parent remains alive and then terminates only this fixture.
        Sleep(120000);
    }
    return 0;
}
