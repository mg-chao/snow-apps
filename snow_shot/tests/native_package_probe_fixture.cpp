#include <cstdio>
#include <cstring>
#include <cwchar>

bool matchesFileText(const wchar_t* path, const char* expected) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") != 0 || file == nullptr) {
        return false;
    }
    char text[64] = {};
    const auto bytes = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    return bytes == std::strlen(expected) && std::strcmp(text, expected) == 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wcscmp(argv[1], L"--update-probe") == 0) {
        return std::wcscmp(argv[2], L"1.2.3") == 0 ? 0 : 4;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"--transaction-state") == 0 &&
        std::wcscmp(argv[2], L"--target") == 0 && argv[3][0] != L'\0') {
        return 0;
    }
    if (argc == 2 && std::wcscmp(argv[1], L"--version") == 0) {
#ifdef _M_ARM64
        std::puts("snow-ocr-process 1.0.9 windows-aarch64 protocol 5");
#else
        std::puts("snow-ocr-process 1.0.9 windows-x86_64 protocol 5");
#endif
        return 0;
    }
    if (argc == 5 && std::wcscmp(argv[1], L"--validate-model-set") == 0) {
        return matchesFileText(argv[2], "valid detector") &&
                       matchesFileText(argv[3], "valid recognizer") &&
                       matchesFileText(argv[4], "valid dictionary")
                   ? 0
                   : 5;
    }
    return 2;
}
