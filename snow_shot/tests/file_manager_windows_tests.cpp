#include "platform/windows/filemanagerbackend_p.h"

#include <QDir>
#include <QScopeGuard>
#include <QUuid>
#include <qt_windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class AssociationFixture {
  public:
    AssociationFixture() {
        path_ = QStringLiteral("Software\\SnowShotTests\\FileManager\\") +
                QUuid::createUuid().toString(QUuid::WithoutBraces);
        require(RegCreateKeyExW(HKEY_CURRENT_USER, native(path_), 0, nullptr, REG_OPTION_VOLATILE,
                                KEY_READ | KEY_WRITE, nullptr, &root_, nullptr) == ERROR_SUCCESS,
                "create isolated association fixture; system associations are never changed");
        directory_ = create(QStringLiteral("Directory"));
        folder_ = create(QStringLiteral("Folder"));
    }
    ~AssociationFixture() {
        RegCloseKey(directory_);
        RegCloseKey(folder_);
        RegCloseKey(root_);
        RegDeleteTreeW(HKEY_CURRENT_USER, native(path_));
    }

    void set(const QString& keyPath, const QString& name, const QString& value) const {
        const HKEY key = create(keyPath);
        const auto close = qScopeGuard([key] { RegCloseKey(key); });
        require(RegSetValueExW(
                    key, native(name), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.utf16()),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS,
                "write an isolated folder association");
    }

    bool isExplorer() const {
        const std::array<ASSOCIATIONELEMENT, 2> classes{{
            {ASSOCCLASS_PROGID_KEY, directory_, nullptr},
            {ASSOCCLASS_PROGID_KEY, folder_, nullptr},
        }};
        Microsoft::WRL::ComPtr<IQueryAssociations> associations;
        require(SUCCEEDED(AssocCreateForClasses(classes.data(), static_cast<ULONG>(classes.size()),
                                                IID_PPV_ARGS(&associations))),
                "resolve isolated directory and folder association precedence");
        return snow_shot::platform::detail::isExplorerAssociation(associations.Get());
    }

  private:
    static LPCWSTR native(const QString& text) {
        return reinterpret_cast<LPCWSTR>(text.utf16());
    }
    HKEY create(const QString& keyPath) const {
        HKEY key = nullptr;
        require(RegCreateKeyExW(root_, native(keyPath), 0, nullptr, REG_OPTION_VOLATILE,
                                KEY_READ | KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS,
                "create an isolated association subkey");
        return key;
    }

    QString path_;
    HKEY root_ = nullptr;
    HKEY directory_ = nullptr;
    HKEY folder_ = nullptr;
};
} // namespace

void fileManagerWindowsTests() try {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "initialize test COM");
    const auto apartment = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    require(length > 0 && length < buffer.size(), "resolve the system Explorer path");
    const QString explorer = QDir::toNativeSeparators(
        QDir(QString::fromWCharArray(buffer.data())).filePath(QStringLiteral("explorer.exe")));
    const QString command = QStringLiteral("\"%1\" \"%2\"").arg(explorer, QStringLiteral("%1"));
    const QString customCommand = QStringLiteral("\"C:\\File Manager\\explorer.exe\" \"%1\"");
    {
        AssociationFixture fixture;
        fixture.set(QStringLiteral("Directory\\shell"), {}, QStringLiteral("none"));
        fixture.set(QStringLiteral("Folder\\shell\\open\\command"), {}, command);
        require(fixture.isExplorer(), "standard Folder open inherits Explorer");
        fixture.set(QStringLiteral("Folder\\shell\\open\\command"),
                    QStringLiteral("DelegateExecute"),
                    QStringLiteral("{11dbb47c-a525-400b-9e80-a54615a090c0}"));
        require(fixture.isExplorer(), "the standard Explorer COM delegate supports selection");
        fixture.set(QStringLiteral("Folder\\shell\\open\\command"),
                    QStringLiteral("DelegateExecute"),
                    QStringLiteral("{6dd3ce3a-b93d-4ba1-92ee-e1c748838941}"));
        require(!fixture.isExplorer(), "a custom COM delegate overrides an Explorer command");
    }
    {
        AssociationFixture fixture;
        fixture.set(QStringLiteral("Directory\\shell"), {}, QStringLiteral("none"));
        fixture.set(QStringLiteral("Folder\\shell\\open\\command"), {}, command);
        fixture.set(QStringLiteral("Folder\\shell"), {}, QStringLiteral("openindopus"));
        fixture.set(QStringLiteral("Folder\\shell\\openindopus\\command"), {}, customCommand);
        require(!fixture.isExplorer(), "honor a custom default verb instead of the open verb");
    }
    {
        AssociationFixture fixture;
        fixture.set(QStringLiteral("Folder\\shell\\open\\command"), {}, command);
        fixture.set(QStringLiteral("Directory\\shell"), {}, QStringLiteral("custom"));
        fixture.set(QStringLiteral("Directory\\shell\\custom\\command"), {}, customCommand);
        require(!fixture.isExplorer(), "a directory-specific default overrides generic Folder");
    }
    {
        AssociationFixture fixture;
        fixture.set(QStringLiteral("Folder\\shell\\open\\command"), {}, customCommand);
        require(!fixture.isExplorer(), "an unrelated explorer.exe is not Windows Explorer");
    }
    {
        AssociationFixture fixture;
        require(!fixture.isExplorer(), "missing associations use the default directory launcher");
    }
    require(!snow_shot::platform::detail::isExplorerAssociation(nullptr),
            "association query failure never forces Explorer");
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    std::exit(1);
}
