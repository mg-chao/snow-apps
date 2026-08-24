#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "screenshotclipboardworkerprotocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
namespace protocol = snow_shot::presentation::clipboard_worker_protocol;

constexpr int kMaximumOpenAttempts = 5;
constexpr std::array<DWORD, kMaximumOpenAttempts - 1> kOpenRetryDelaysMs{10, 25, 60, 100};

bool readExact(HANDLE input, void* destination, std::uint64_t length) {
    auto* output = static_cast<std::uint8_t*>(destination);
    std::uint64_t offset = 0;
    while (offset < length) {
        const DWORD requested = static_cast<DWORD>((std::min)(
            length - offset, static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())));
        DWORD consumed = 0;
        if (ReadFile(input, output + offset, requested, &consumed, nullptr) == FALSE ||
            consumed == 0) {
            return false;
        }
        offset += consumed;
    }
    return true;
}

bool writeResponse(HANDLE output, protocol::Status status, DWORD nativeError, int attempts) {
    protocol::ResponseHeader response;
    response.status = static_cast<std::uint32_t>(status);
    response.nativeError = nativeError;
    response.attempts = static_cast<std::uint32_t>((std::max)(attempts, 0));
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&response);
    std::size_t offset = 0;
    while (offset < sizeof(response)) {
        DWORD written = 0;
        if (WriteFile(output, bytes + offset,
                      static_cast<DWORD>(sizeof(response) - offset), &written, nullptr) == FALSE ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool validDibPayload(const protocol::RequestHeader& request, const void* memory) {
    if (memory == nullptr ||
        (request.nativeFormat != CF_DIB && request.nativeFormat != CF_DIBV5)) {
        return false;
    }
    const std::uint64_t headerBytes =
        request.nativeFormat == CF_DIB ? sizeof(BITMAPINFOHEADER) : sizeof(BITMAPV5HEADER);
    if (request.payloadBytes < headerBytes) {
        return false;
    }

    const auto* header = static_cast<const BITMAPINFOHEADER*>(memory);
    if (header->biWidth <= 0 || header->biHeight == 0 || header->biPlanes != 1 ||
        header->biBitCount != 32) {
        return false;
    }
    const std::uint64_t height = header->biHeight < 0
                                     ? static_cast<std::uint64_t>(-
                                           static_cast<std::int64_t>(header->biHeight))
                                     : static_cast<std::uint64_t>(header->biHeight);
    const std::uint64_t width = static_cast<std::uint64_t>(header->biWidth);
    if (width > (std::numeric_limits<std::uint64_t>::max)() / 4 ||
        width * 4 > (std::numeric_limits<std::uint64_t>::max)() / height) {
        return false;
    }
    const std::uint64_t pixelBytes = width * height * 4;
    if (pixelBytes != request.payloadBytes - headerBytes ||
        pixelBytes > (std::numeric_limits<DWORD>::max)() ||
        header->biSizeImage != static_cast<DWORD>(pixelBytes)) {
        return false;
    }

    if (request.nativeFormat == CF_DIB) {
        return header->biSize == sizeof(BITMAPINFOHEADER) && header->biHeight > 0 &&
               header->biCompression == BI_RGB;
    }
    const auto* headerV5 = static_cast<const BITMAPV5HEADER*>(memory);
    return headerV5->bV5Size == sizeof(BITMAPV5HEADER) && headerV5->bV5Height < 0 &&
           headerV5->bV5Compression == BI_BITFIELDS &&
           headerV5->bV5RedMask == 0x00ff0000 && headerV5->bV5GreenMask == 0x0000ff00 &&
           headerV5->bV5BlueMask == 0x000000ff && headerV5->bV5AlphaMask == 0xff000000;
}

HWND createClipboardOwnerWindow() {
    return CreateWindowExW(0, L"STATIC", L"SnowShotClipboardPublisher", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
}

protocol::Status publish(HGLOBAL* allocation, UINT nativeFormat, DWORD* nativeError,
                         int* attempts) {
    const HWND owner = createClipboardOwnerWindow();
    if (owner == nullptr) {
        *nativeError = GetLastError();
        return protocol::Status::ClipboardUnavailable;
    }

    bool opened = false;
    for (int attempt = 0; attempt < kMaximumOpenAttempts; ++attempt) {
        *attempts = attempt + 1;
        SetLastError(ERROR_SUCCESS);
        if (OpenClipboard(owner) != FALSE) {
            opened = true;
            break;
        }
        *nativeError = GetLastError();
        if (attempt + 1 < kMaximumOpenAttempts) {
            Sleep(kOpenRetryDelaysMs[static_cast<std::size_t>(attempt)]);
        }
    }
    if (!opened) {
        DestroyWindow(owner);
        return protocol::Status::Busy;
    }

    SetLastError(ERROR_SUCCESS);
    if (EmptyClipboard() == FALSE) {
        *nativeError = GetLastError();
        CloseClipboard();
        DestroyWindow(owner);
        return protocol::Status::ClearFailed;
    }
    SetLastError(ERROR_SUCCESS);
    if (SetClipboardData(nativeFormat, *allocation) == nullptr) {
        *nativeError = GetLastError();
        CloseClipboard();
        DestroyWindow(owner);
        return protocol::Status::PublishFailed;
    }

    *allocation = nullptr;
    CloseClipboard();
    DestroyWindow(owner);
    *nativeError = ERROR_SUCCESS;
    return protocol::Status::Success;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE || output == nullptr ||
        output == INVALID_HANDLE_VALUE) {
        return 2;
    }

    protocol::RequestHeader request;
    if (!readExact(input, &request, sizeof(request)) || request.magic != protocol::kRequestMagic ||
        request.version != protocol::kVersion || request.reserved != 0 ||
        request.payloadBytes == 0 || request.payloadBytes > protocol::kMaximumPayloadBytes ||
        request.payloadBytes > (std::numeric_limits<SIZE_T>::max)()) {
        return writeResponse(output, protocol::Status::InvalidPayload, ERROR_INVALID_DATA, 0) ? 0
                                                                                              : 3;
    }

    HGLOBAL allocation = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(request.payloadBytes));
    if (allocation == nullptr) {
        return writeResponse(output, protocol::Status::InvalidPayload, GetLastError(), 0) ? 0 : 3;
    }
    void* memory = GlobalLock(allocation);
    if (memory == nullptr) {
        const DWORD error = GetLastError();
        GlobalFree(allocation);
        return writeResponse(output, protocol::Status::InvalidPayload, error, 0) ? 0 : 3;
    }
    const bool read = readExact(input, memory, request.payloadBytes);
    const bool valid = read && validDibPayload(request, memory);
    GlobalUnlock(allocation);
    if (!valid) {
        GlobalFree(allocation);
        return writeResponse(output, protocol::Status::InvalidPayload, ERROR_INVALID_DATA, 0) ? 0
                                                                                              : 3;
    }

    DWORD nativeError = ERROR_SUCCESS;
    int attempts = 0;
    const protocol::Status status =
        publish(&allocation, request.nativeFormat, &nativeError, &attempts);
    if (allocation != nullptr) {
        GlobalFree(allocation);
    }
    return writeResponse(output, status, nativeError, attempts) ? 0 : 3;
}
