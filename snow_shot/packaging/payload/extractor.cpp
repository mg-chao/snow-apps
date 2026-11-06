// SPDX-License-Identifier: GPL-3.0-or-later
// The installer embeds this native helper temporarily, never in the app payload.
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <7z.h>
#include <7zAlloc.h>
#include <7zCrc.h>
#include <7zFile.h>

namespace {

enum class Result : int {
    success = 0,
    arguments = 1,
    integrity = 2,
    archive = 3,
    path = 4,
    output = 5,
    decode = 6,
    memory = 7,
};

constexpr std::size_t maximumAllocation = 512ULL * 1024 * 1024;
constexpr UInt64 maximumUnpackedBytes = 1024ULL * 1024 * 1024;
constexpr UInt32 maximumFiles = 100000;

void* sdkAllocate(ISzAllocPtr allocator, std::size_t size) {
    return size <= maximumAllocation ? SzAlloc(allocator, size) : nullptr;
}

const ISzAlloc sdkAllocator{sdkAllocate, SzFree};

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) {
            CloseHandle(value);
        }
    }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Archive {
    CFileInStream file{};
    CLookToRead2 stream{};
    CSzArEx database{};
    std::array<Byte, 1 << 18> input{};
    Byte* output = nullptr;

    Archive() {
        File_Construct(&file.file);
        FileInStream_CreateVTable(&file);
        LookToRead2_CreateVTable(&stream, False);
        stream.buf = input.data();
        stream.bufSize = input.size();
        stream.realStream = &file.vt;
        LookToRead2_INIT(&stream) SzArEx_Init(&database);
    }
    ~Archive() {
        ISzAlloc_Free(&sdkAllocator, output);
        SzArEx_Free(&database, &sdkAllocator);
        File_Close(&file.file);
    }
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
};

struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~Hash() {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    }
};

int hexDigit(wchar_t value) {
    if (value >= L'0' && value <= L'9') {
        return value - L'0';
    }
    if (value >= L'a' && value <= L'f') {
        return value - L'a' + 10;
    }
    if (value >= L'A' && value <= L'F') {
        return value - L'A' + 10;
    }
    return -1;
}

bool verifyHash(HANDLE file, const std::wstring& expected) {
    if (expected.size() != 64) {
        return false;
    }
    std::array<Byte, 32> expectedBytes{};
    for (std::size_t index = 0; index < expectedBytes.size(); ++index) {
        const int high = hexDigit(expected[index * 2]);
        const int low = hexDigit(expected[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        expectedBytes[index] = static_cast<Byte>(high * 16 + low);
    }
    // BCrypt retains the caller-owned object buffer until hash destruction.
    std::vector<Byte> object;
    Hash state;
    DWORD objectBytes = 0;
    DWORD returned = 0;
    if (BCryptOpenAlgorithmProvider(&state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(state.algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned,
                          0) < 0) {
        return false;
    }
    object.resize(objectBytes);
    if (BCryptCreateHash(state.algorithm, &state.hash, object.data(), objectBytes, nullptr, 0, 0) <
        0) {
        return false;
    }
    std::array<Byte, 1 << 16> buffer{};
    DWORD count = 0;
    while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
        if (count == 0) {
            std::array<Byte, 32> actual{};
            if (BCryptFinishHash(state.hash, actual.data(), static_cast<ULONG>(actual.size()), 0) <
                0) {
                return false;
            }
            unsigned difference = 0;
            for (std::size_t index = 0; index < actual.size(); ++index) {
                difference |= actual[index] ^ expectedBytes[index];
            }
            LARGE_INTEGER start{};
            return difference == 0 && SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
        }
        if (BCryptHashData(state.hash, buffer.data(), count, 0) < 0) {
            return false;
        }
    }
    return false;
}

bool reservedComponent(const std::wstring& component) {
    std::wstring stem = component.substr(0, component.find(L'.'));
    for (wchar_t& character : stem) {
        if (character >= L'a' && character <= L'z') {
            character = static_cast<wchar_t>(character - L'a' + L'A');
        }
    }
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") {
        return true;
    }
    return stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
           ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' ||
            stem[3] == L'\u00b3');
}

bool validRelativeName(const std::wstring& name) {
    if (name.empty() || name.size() > 32000 || name.front() == L'/' || name.back() == L'/') {
        return false;
    }
    std::size_t start = 0;
    for (std::size_t index = 0; index <= name.size(); ++index) {
        if (index != name.size()) {
            const wchar_t character = name[index];
            if (character < 0x20 || character == L'\\' || character == L':' || character == L'"' ||
                character == L'<' || character == L'>' || character == L'|' || character == L'*' ||
                character == L'?') {
                return false;
            }
            if (character != L'/') {
                continue;
            }
        }
        const std::wstring component = name.substr(start, index - start);
        if (component.empty() || component == L"." || component == L".." ||
            component.back() == L'.' || component.back() == L' ' || reservedComponent(component)) {
            return false;
        }
        start = index + 1;
    }
    return true;
}

struct Entry {
    UInt32 index = 0;
    std::wstring name;
    bool directory = false;
};

bool isPrefix(const std::wstring& prefix, const std::wstring& value) {
    return value.size() > prefix.size() && value[prefix.size()] == L'/' &&
           CompareStringOrdinal(prefix.data(), static_cast<int>(prefix.size()), value.data(),
                                static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

bool readEntries(const CSzArEx& database, std::vector<Entry>& entries) {
    if (database.NumFiles > maximumFiles ||
        (database.NumFiles != 0 &&
         database.UnpackPositions[database.NumFiles] > maximumUnpackedBytes)) {
        return false;
    }
    entries.reserve(database.NumFiles);
    for (UInt32 index = 0; index < database.NumFiles; ++index) {
        const std::size_t length = SzArEx_GetFileNameUtf16(&database, index, nullptr);
        if (length < 2 || length > 32001) {
            return false;
        }
        std::vector<UInt16> buffer(length);
        SzArEx_GetFileNameUtf16(&database, index, buffer.data());
        if (buffer.back() != 0 ||
            std::find(buffer.begin(), buffer.end() - 1, UInt16{0}) != buffer.end() - 1) {
            return false;
        }
        static_assert(sizeof(wchar_t) == sizeof(UInt16));
        Entry entry{index,
                    std::wstring(reinterpret_cast<const wchar_t*>(buffer.data()), length - 1),
                    SzArEx_IsDir(&database, index) != 0};
        if (!validRelativeName(entry.name)) {
            return false;
        }
        if (SzBitWithVals_Check(&database.Attribs, index)) {
            const UInt32 attributes = database.Attribs.Vals[index];
            const UInt32 unixType = (attributes >> 16) & 0170000;
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                (unixType != 0 && unixType != 0100000 && unixType != 0040000)) {
                return false;
            }
        }
        entries.push_back(std::move(entry));
    }
    std::vector<const Entry*> sorted;
    sorted.reserve(entries.size());
    for (const Entry& entry : entries) {
        sorted.push_back(&entry);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Entry* left, const Entry* right) {
        return CompareStringOrdinal(left->name.c_str(), -1, right->name.c_str(), -1, TRUE) ==
               CSTR_LESS_THAN;
    });
    for (std::size_t index = 1; index < sorted.size(); ++index) {
        const Entry& previous = *sorted[index - 1];
        const Entry& current = *sorted[index];
        if (CompareStringOrdinal(previous.name.c_str(), -1, current.name.c_str(), -1, TRUE) ==
            CSTR_EQUAL) {
            return false;
        }
    }
    for (const Entry& entry : entries) {
        if (entry.directory) {
            continue;
        }
        const std::wstring prefix = entry.name + L'/';
        const auto descendant = std::lower_bound(
            sorted.begin(), sorted.end(), prefix,
            [](const Entry* candidate, const std::wstring& key) {
                return CompareStringOrdinal(candidate->name.c_str(), -1, key.c_str(), -1, TRUE) ==
                       CSTR_LESS_THAN;
            });
        if (descendant != sorted.end() && isPrefix(entry.name, (*descendant)->name)) {
            return false;
        }
    }
    return true;
}

bool createRootDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring parent = slash == std::wstring::npos
                                    ? std::wstring{}
                                    : path.substr(0, slash == 2 && path[1] == L':' ? 3 : slash);
    if (parent.empty() || parent == path || !createRootDirectory(parent)) {
        return false;
    }
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring canonicalRoot(const std::wstring& destination) {
    const DWORD length = GetFullPathNameW(destination.c_str(), 0, nullptr, nullptr);
    if (length == 0 || length > 32000) {
        return {};
    }
    std::wstring path(length, L'\0');
    const DWORD written = GetFullPathNameW(destination.c_str(), length, path.data(), nullptr);
    if (written == 0 || written >= length) {
        return {};
    }
    path.resize(written);
    if (!createRootDirectory(path)) {
        return {};
    }
    Handle directory;
    directory.value =
        CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory.value == INVALID_HANDLE_VALUE) {
        return {};
    }
    const DWORD needed = GetFinalPathNameByHandleW(directory.value, nullptr, 0, 0);
    if (needed == 0 || needed > 32000) {
        return {};
    }
    path.assign(static_cast<std::size_t>(needed) + 1, L'\0');
    const DWORD actual =
        GetFinalPathNameByHandleW(directory.value, path.data(), static_cast<DWORD>(path.size()), 0);
    if (actual == 0 || actual >= path.size()) {
        return {};
    }
    path.resize(actual);
    if (path.back() != L'\\') {
        path.push_back(L'\\');
    }
    return path;
}

bool childDirectories(const std::wstring& root, const std::wstring& relative, bool directory) {
    std::wstring path = root;
    std::size_t start = 0;
    while (start < relative.size()) {
        const std::size_t slash = relative.find(L'/', start);
        if (slash == std::wstring::npos && !directory) {
            break;
        }
        path.append(relative, start,
                    slash == std::wstring::npos ? relative.size() - start : slash - start);
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(path.c_str(), nullptr)) {
                return false;
            }
            attributes = GetFileAttributesW(path.c_str());
        }
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
        if (slash == std::wstring::npos) {
            break;
        }
        path.push_back(L'\\');
        start = slash + 1;
    }
    return true;
}

bool writeEntry(const std::wstring& root, const Entry& entry, const Byte* bytes, std::size_t size) {
    if (!childDirectories(root, entry.name, false)) {
        return false;
    }
    std::wstring target = root + entry.name;
    std::replace(target.begin(), target.end(), L'/', L'\\');
    const DWORD previousAttributes = GetFileAttributesW(target.c_str());
    if (previousAttributes != INVALID_FILE_ATTRIBUTES &&
        (previousAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return false;
    }
    const std::wstring temporary = target + L".snow-payload-" +
                                   std::to_wstring(GetCurrentProcessId()) + L"-" +
                                   std::to_wstring(entry.index) + L".tmp";
    bool written = false;
    {
        Handle output;
        output.value = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (output.value == INVALID_HANDLE_VALUE) {
            return false;
        }
        std::size_t position = 0;
        while (position < size) {
            const DWORD requested =
                static_cast<DWORD>(std::min<std::size_t>(size - position, 1 << 20));
            DWORD count = 0;
            if (!WriteFile(output.value, bytes + position, requested, &count, nullptr) ||
                count == 0) {
                break;
            }
            position += count;
        }
        written = position == size && FlushFileBuffers(output.value);
    }
    if (!written) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (previousAttributes != INVALID_FILE_ATTRIBUTES &&
        (previousAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(target.c_str(), previousAttributes & ~FILE_ATTRIBUTE_READONLY);
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        if (previousAttributes != INVALID_FILE_ATTRIBUTES) {
            SetFileAttributesW(target.c_str(), previousAttributes);
        }
        return false;
    }
    return true;
}

Result extract(const std::wstring& archivePath, const std::wstring& destination,
               const std::wstring& expectedHash, const std::wstring& selectedEntry) {
    Archive archive;
    if (InFile_OpenW(&archive.file.file, archivePath.c_str()) != 0 ||
        !verifyHash(archive.file.file.handle, expectedHash)) {
        return Result::integrity;
    }
    CrcGenerateTable();
    if (SzArEx_Open(&archive.database, &archive.stream.vt, &sdkAllocator, &sdkAllocator) != SZ_OK) {
        return Result::archive;
    }
    std::vector<Entry> entries;
    if (!readEntries(archive.database, entries)) {
        return Result::path;
    }
    if (!selectedEntry.empty()) {
        if (!validRelativeName(selectedEntry) ||
            std::none_of(entries.begin(), entries.end(), [&selectedEntry](const Entry& entry) {
                return !entry.directory && entry.name == selectedEntry;
            })) {
            return Result::path;
        }
    }
    const std::wstring root = canonicalRoot(destination);
    if (root.empty()) {
        return Result::output;
    }
    UInt32 block = std::numeric_limits<UInt32>::max();
    std::size_t outputBytes = 0;
    for (const Entry& entry : entries) {
        if (!selectedEntry.empty() && entry.name != selectedEntry) {
            continue;
        }
        if (entry.directory) {
            if (!childDirectories(root, entry.name, true)) {
                return Result::output;
            }
            continue;
        }
        std::size_t offset = 0;
        std::size_t processed = 0;
        if (SzArEx_Extract(&archive.database, &archive.stream.vt, entry.index, &block,
                           &archive.output, &outputBytes, &offset, &processed, &sdkAllocator,
                           &sdkAllocator) != SZ_OK ||
            offset > outputBytes || processed > outputBytes - offset) {
            return Result::decode;
        }
        const Byte* bytes = processed == 0 ? nullptr : archive.output + offset;
        if (!writeEntry(root, entry, bytes, processed)) {
            return Result::output;
        }
    }
    return Result::success;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr || (count != 4 && count != 5)) {
        LocalFree(arguments);
        return static_cast<int>(Result::arguments);
    }
    Result result = Result::archive;
    try {
        result = count == 5 && arguments[4][0] == L'\0'
                     ? Result::path
                     : extract(arguments[1], arguments[2], arguments[3],
                               count == 5 ? arguments[4] : L"");
    } catch (const std::bad_alloc&) {
        result = Result::memory;
    } catch (...) {
        result = Result::archive;
    }
    LocalFree(arguments);
    return static_cast<int>(result);
}
