#include "../filemanagerbackend_p.h"
#include "filemanagerbackend_p.h"

#include <QDir>
#include <qt_windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <array>
#include <optional>
#include <vector>

namespace snow_shot::platform::detail {
namespace {
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
    [[nodiscard]] bool usable() const {
        return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }
};

QString explorerPath() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    return QDir(QString::fromWCharArray(buffer.data(), static_cast<int>(length)))
        .filePath(QStringLiteral("explorer.exe"));
}

std::optional<QString> associationString(IQueryAssociations* associations, ASSOCSTR type) {
    DWORD length = 0;
    HRESULT result = associations->GetString(ASSOCF_NOTRUNCATE, type, nullptr, nullptr, &length);
    if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        result == HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION))
        return QString();
    if (FAILED(result) || length == 0 || length > 32768)
        return std::nullopt;
    std::vector<wchar_t> buffer(length);
    result = associations->GetString(ASSOCF_NOTRUNCATE, type, nullptr, buffer.data(), &length);
    if (result != S_OK)
        return std::nullopt;
    return QString::fromWCharArray(buffer.data());
}
} // namespace

bool isExplorerAssociation(IQueryAssociations* associations) {
    if (!associations)
        return false;
    const auto executable = associationString(associations, ASSOCSTR_EXECUTABLE);
    if (!executable || executable->isEmpty())
        return false;
    // Compare the full system path, not just the name of a third-party executable.
    // Normalizing the registered absolute paths avoids probing either executable on disk.
    const QString expected = QDir::cleanPath(explorerPath());
    const QString actual = QDir::cleanPath(QDir::fromNativeSeparators(*executable));
    if (expected.isEmpty() || actual.isEmpty() ||
        actual.compare(expected, Qt::CaseInsensitive) != 0)
        return false;
    // A custom COM handler can override a command that still names Explorer.
    const auto delegate = associationString(associations, ASSOCSTR_DELEGATEEXECUTE);
    if (!delegate)
        return false;
    if (delegate->isEmpty())
        return true;
    CLSID classId{};
    return SUCCEEDED(CLSIDFromString(reinterpret_cast<LPCOLESTR>(delegate->utf16()), &classId)) &&
           IsEqualCLSID(classId, CLSID_ExecuteFolder);
}

bool isSystemFileManager(const QString& directory) {
    const ComApartment apartment;
    if (!apartment.usable())
        return false;
    const auto nativePath = QDir::toNativeSeparators(directory).toStdWString();
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(nativePath.c_str(), nullptr, IID_PPV_ARGS(&item))))
        return false;
    // Ask for this folder's effective association array, including folder-specific overrides.
    Microsoft::WRL::ComPtr<IQueryAssociations> associations;
    if (FAILED(item->BindToHandler(nullptr, BHID_AssociationArray, IID_PPV_ARGS(&associations))))
        return false;
    return isExplorerAssociation(associations.Get());
}

bool selectInSystemFileManager(const QString& path) {
    const ComApartment apartment;
    if (!apartment.usable())
        return false;
    const auto nativePath = QDir::toNativeSeparators(path).toStdWString();
    PIDLIST_ABSOLUTE item = nullptr;
    const HRESULT parsed = SHParseDisplayName(nativePath.c_str(), nullptr, &item, 0, nullptr);
    const bool selected =
        SUCCEEDED(parsed) && item && SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
    CoTaskMemFree(item);
    return selected;
}

} // namespace snow_shot::platform::detail
