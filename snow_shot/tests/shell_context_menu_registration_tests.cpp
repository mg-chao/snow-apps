#include "shell_context_menu_test_registry.h"
#include "snow_shot/app/supportedpinfiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QUuid>

#include <cstdlib>
#include <iostream>

namespace {
using namespace shell_context_menu_tests;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void commandAndOwnership() {
    Registry registry;
    auto options = registry.options();
    const auto executable = QDir::toNativeSeparators(options.executablePath);
    windows::ShellContextMenuRegistration registration(options);
    require(registration.expectedCommand() == QStringLiteral("\"%1\" --edit-with-snow-shot \"%2\"")
                                                  .arg(executable, QStringLiteral("%1")),
            "command quotes executable and Explorer's Unicode file argument");
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles),
            "enable supported image verbs");
    require(registry.keys.size() == 2 && registry.notifications == 1,
            "enabling creates one verb per supported image extension and notifies Explorer once");
    for (const auto& extension : {QStringLiteral("png"), QStringLiteral("jpg")}) {
        const auto key = registry.keys.value(Registry::path(extension));
        require(key.exists &&
                    stringText(key.values.value(QStringLiteral("ProductId"))) ==
                        snow_shot::app::edition::productId() &&
                    stringText(key.values.value(QStringLiteral("InstallationExecutable"))) ==
                        executable &&
                    stringText(key.values.value(QStringLiteral("MultiSelectModel"))) ==
                        QStringLiteral("Document") &&
                    stringText(key.values.value(QStringLiteral("Icon"))) ==
                        QStringLiteral("\"%1\",0").arg(executable) &&
                    stringText(key.children.value(QStringLiteral("command")).values.value({})) ==
                        registration.expectedCommand(),
                "each image verb records installation ownership, icon, document model and command");
    }
    const int writes = registry.writes;
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles) &&
                registry.writes == writes && registry.notifications == 1,
            "startup reconciliation is idempotent");
    require(!registry.keys.contains(Registry::path(QStringLiteral("txt"))) &&
                !registry.keys.contains(Registry::path(QStringLiteral("html"))),
            "context menu excludes supported text and HTML pinning formats");
}

void accessKeysAndRollback() {
    Registry registry;
    windows::ShellContextMenuRegistration registration(registry.options());
    const QString baseLabel = snow_shot::app::edition::isMini
                                  ? QStringLiteral("Edit with Snow Shot Mini")
                                  : QStringLiteral("Edit with Snow Shot");
    const auto label = [&] {
        return stringText(
            registry.keys.value(Registry::path(QStringLiteral("png"))).values.value({}));
    };
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles,
                                 QStringLiteral("e")) &&
                label() == baseLabel + QStringLiteral(" (&E)"),
            "startup registration canonicalizes and displays the configured logical access key");
    const int writes = registry.writes;
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles,
                                 QStringLiteral("E")) &&
                registry.writes == writes,
            "startup reconciliation with an unchanged access key is idempotent");
    const auto before = registry.keys;
    QString error;
    for (const auto& invalid : {QStringLiteral("Ctrl+E"), QStringLiteral("F1"), QStringLiteral("&"),
                                QStringLiteral("雪"), QStringLiteral("EE")}) {
        require(!registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles, invalid,
                                      &error) &&
                    !error.isEmpty() && !registration.refreshLabels(invalid, &error) &&
                    registry.keys == before,
                "invalid native access keys cannot change registry labels");
    }
    registry.writesBeforeFailure = 1;
    require(!registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles,
                                  QStringLiteral("S")) &&
                registry.keys == before,
            "partial native access key updates roll back every image verb");
    require(!registration
                    .changeMode(windows::ShellContextMenuMode::ImageAndDocumentFiles,
                                QStringLiteral("7"), [] { return false; })
                    .success &&
                registry.keys == before,
            "a rejected preference commit restores the previous mnemonic");
    require(registration.refreshLabels(QStringLiteral("7")) &&
                label() == baseLabel + QStringLiteral(" (&7)"),
            "label refresh shares digit mnemonic formatting with registration");
    require(registration.refreshLabels(QString()) && label() == baseLabel,
            "clearing a key removes the suffix entirely");
    require(registration.setMode(windows::ShellContextMenuMode::Disabled, QStringLiteral("E")) &&
                registry.keys.isEmpty() && registration.refreshLabels(QStringLiteral("S")) &&
                registry.keys.isEmpty(),
            "refreshing an access key does not create disabled menu entries");
}

void preservationRelocationAndRollback() {
    Registry registry;
    windows::ShellContextMenuRegistration registration(registry.options());
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles),
            "create rollback baseline");
    const auto png = Registry::path(QStringLiteral("png"));
    const auto jpg = Registry::path(QStringLiteral("jpg"));
    registry.keys[png].values.insert(QStringLiteral("CustomValue"), {3, QByteArray("\0\1\2", 3)});
    auto& child = registry.keys[png].children[QStringLiteral("custom-child")];
    child.exists = true;
    child.values.insert(QString(), stringValue(QStringLiteral("retain child")));
    const auto retainedChild = child;
    auto neighbor = registry.keys.value(png);
    neighbor.values.insert(QStringLiteral("ProductId"), stringValue(QStringLiteral("unrelated")));
    const auto neighborPath = Registry::path(QStringLiteral("png"), QStringLiteral("Other.Edit"));
    registry.keys.insert(neighborPath, neighbor);
    const auto otherEdition =
        Registry::path(QStringLiteral("png"), snow_shot::app::edition::isMini
                                                  ? QStringLiteral("SnowShot.Edit")
                                                  : QStringLiteral("SnowShotMini.Edit"));
    registry.keys.insert(otherEdition, neighbor);
    auto stale = registry.keys.value(jpg);
    stale.values.insert(QStringLiteral("InstallationExecutable"),
                        stringValue(QStringLiteral("C:\\Previous installation\\snow_shot.exe")));
    const auto stalePath = Registry::path(QStringLiteral("obsolete"));
    registry.keys.insert(stalePath, stale);
    registry.keys[jpg].values.insert(
        QStringLiteral("InstallationExecutable"),
        stringValue(QStringLiteral("C:\\Previous installation\\snow_shot.exe")));
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles) &&
                !registry.keys.contains(stalePath) &&
                registry.keys.value(neighborPath) == neighbor &&
                registry.keys.value(otherEdition) == neighbor &&
                registry.keys.value(png).values.value(QStringLiteral("CustomValue")) ==
                    windows::ShellContextMenuRegistryValue{3, QByteArray("\0\1\2", 3)} &&
                registry.keys.value(png).children.value(QStringLiteral("custom-child")) ==
                    retainedChild,
            "reconciliation removes stale extensions, replaces relocated ownership and preserves "
            "neighbors");
    const auto baseline = registry.keys;
    registry.writesBeforeFailure = 1;
    int commits = 0;
    const auto failed = registration.changeMode(windows::ShellContextMenuMode::Disabled, [&] {
        ++commits;
        return true;
    });
    require(!failed.success && failed.error == QStringLiteral("injected write failure") &&
                commits == 0 && registry.keys == baseline,
            "partial native failure restores raw values and subkeys before any preference commit");
    const auto unsaved =
        registration.changeMode(windows::ShellContextMenuMode::Disabled, [] { return false; });
    require(!unsaved.success && !unsaved.error.isEmpty() && registry.keys == baseline,
            "failed preference persistence restores the complete registration snapshot");
    require(registration.setMode(windows::ShellContextMenuMode::Disabled) &&
                registry.keys.size() == 2 && registry.keys.value(neighborPath) == neighbor &&
                registry.keys.value(otherEdition) == neighbor,
            "disable removes only this product's verbs and preserves neighboring editions");
    registry.failReads = true;
    require(!registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles) &&
                registry.keys.size() == 2,
            "read failure never changes native registration");
}

void foreignOwnerAndLanguageRefresh() {
    Registry registry;
    windows::ShellContextMenuRegistration registration(registry.options());
    require(registration.refreshLabels() && registry.keys.isEmpty(),
            "language refresh does not create context menu entries");
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles),
            "create label baseline");
    const auto png = Registry::path(QStringLiteral("png"));
    registry.keys[png].values.insert(QString(), stringValue(QStringLiteral("old localized label")));
    const auto command = registry.keys[png].children.value(QStringLiteral("command"));
    require(registration.refreshLabels() &&
                stringText(registry.keys[png].values.value({})) ==
                    (snow_shot::app::edition::isMini ? QStringLiteral("Edit with Snow Shot Mini")
                                                     : QStringLiteral("Edit with Snow Shot")) &&
                registry.keys[png].children.value(QStringLiteral("command")) == command,
            "language refresh replaces the display label and preserves command data");
    registry.keys[png].values.insert(QStringLiteral("ProductId"),
                                     stringValue(QStringLiteral("foreign")));
    const auto baseline = registry.keys;
    require(!registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles) &&
                registry.keys == baseline,
            "enable refuses a foreign owner without leaving partial registry changes");
    require(registration.setMode(windows::ShellContextMenuMode::Disabled) &&
                registry.keys.value(png) == baseline.value(png),
            "disable preserves a foreign owner using the same verb name");
}

void authoritativeSuffixes() {
    Registry registry;
    auto options = registry.options();
    options.fileExtensions = snow_shot::image::supportedImageFileExtensions();
    options.fileExtensions.removeAll(QStringLiteral("bmp"));
    windows::ShellContextMenuRegistration previousRegistration(options);
    require(previousRegistration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles) &&
                !registry.keys.contains(Registry::path(QStringLiteral("bmp"))),
            "previous installations register the existing formats without BMP");
    options.fileExtensions.clear();
    windows::ShellContextMenuRegistration registration(std::move(options));
    const auto previous = registry.keys;
    registry.writesBeforeFailure = 0;
    require(!registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles) &&
                registry.keys == previous,
            "failed BMP registration preserves the previous installation's image verbs");
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles) &&
                registry.keys.size() == snow_shot::app::supportedPinFileExtensions().size(),
            "default registrar uses the shared authoritative pin extension list");
    for (const auto& extension : snow_shot::app::supportedPinFileExtensions())
        require(registry.keys.contains(Registry::path(extension)),
                "every supported image or document extension receives the context menu entry");
    for (const auto& document : snow_shot::app::kPinDocumentFileFormats) {
        const auto entry =
            registry.keys.value(Registry::path(QString::fromLatin1(document.suffix)));
        require(entry.exists &&
                    stringText(entry.children.value(QStringLiteral("command")).values.value({})) ==
                        registration.expectedCommand(),
                "startup upgrades add every document extension with the existing command");
    }
    const auto bmp = Registry::path(QStringLiteral("bmp"));
    const auto key = registry.keys.value(bmp);
    require(key.exists &&
                stringText(key.values.value(QStringLiteral("ProductId"))) ==
                    snow_shot::app::edition::productId() &&
                stringText(key.children.value(QStringLiteral("command")).values.value({})) ==
                    registration.expectedCommand(),
            "startup reconciliation adds an owned BMP verb using the current edition command");
    const int writes = registry.writes;
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles) &&
                registry.writes == writes,
            "BMP registration is idempotent on later launches");
    const auto enabled = registry.keys;
    registry.writesBeforeFailure = 1;
    require(!registration.setMode(windows::ShellContextMenuMode::Disabled) &&
                registry.keys == enabled,
            "partial removal failure restores BMP and the existing image verbs");
    require(!registration.changeMode(windows::ShellContextMenuMode::Disabled, [] { return false; })
                    .success &&
                registry.keys == enabled,
            "failed preference persistence restores BMP with the rest of the menu");
    require(registration.setMode(windows::ShellContextMenuMode::Disabled) &&
                registry.keys.isEmpty(),
            "disabling the menu removes BMP and every other owned image verb");
}

void imageOnlyScope() {
    Registry registry;
    auto options = registry.options();
    options.fileExtensions.clear();
    windows::ShellContextMenuRegistration registration(options);
    using Mode = windows::ShellContextMenuMode;
    require(registration.modeFromValue(QStringLiteral("image_files")) ==
                    Mode::ImageAndDocumentFiles &&
                registration.modeFromValue(QStringLiteral("images_only")) == Mode::ImageFiles &&
                registration.modeFromValue(QStringLiteral("disabled")) == Mode::Disabled,
            "saved legacy and new scope values retain their distinct meanings");
    require(registration.setMode(Mode::ImageAndDocumentFiles, QStringLiteral("E")),
            "prepare owned image and document verbs");
    const auto broad = registry.keys;
    registry.writesBeforeFailure = 1;
    require(!registration.setMode(Mode::ImageFiles, QStringLiteral("7")) && registry.keys == broad,
            "partial scope changes restore document registrations and image access keys");
    require(!registration.changeMode(Mode::ImageFiles, [] { return false; }).success &&
                registry.keys == broad,
            "rejected preference commits restore the complete broad scope");
    require(registration.setMode(Mode::ImageFiles, QStringLiteral("E")) &&
                registry.keys.size() == snow_shot::image::supportedImageFileExtensions().size(),
            "image-only mode removes all previously registered document verbs");
    for (const auto& extension : snow_shot::image::supportedImageFileExtensions())
        require(registry.keys.contains(Registry::path(extension)),
                "image-only mode keeps every supported image extension");
    const auto narrow = registry.keys;
    const int writes = registry.writes;
    require(registration.setMode(Mode::ImageFiles, QStringLiteral("E")) &&
                registry.writes == writes && registry.keys == narrow,
            "image-only startup reconciliation is idempotent");
    require(registration.refreshLabels(QStringLiteral("7")) &&
                registry.keys.size() == narrow.size(),
            "language and access-key refresh cannot reintroduce document verbs");
    require(registration.setMode(Mode::ImageAndDocumentFiles) &&
                registry.keys.size() == broad.size(),
            "broadening restores every supported document extension");
    auto foreign = registry.keys.value(Registry::path(QStringLiteral("txt")));
    foreign.values.insert(QStringLiteral("ProductId"), stringValue(QStringLiteral("foreign")));
    registry.keys.insert(Registry::path(QStringLiteral("txt")), foreign);
    require(registration.setMode(Mode::ImageFiles) &&
                registry.keys.value(Registry::path(QStringLiteral("txt"))) == foreign &&
                registration.setMode(Mode::Disabled) && registry.keys.size() == 1,
            "narrowing and disabling preserve a foreign-owned document verb");
}

#ifdef Q_OS_WIN
void isolatedNativeRegistry() {
    const QString root = QStringLiteral("Software\\SnowShotTests\\") +
                         QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings neighbors(QStringLiteral("HKEY_CURRENT_USER\\") + root, QSettings::NativeFormat);
    neighbors.setValue(QStringLiteral("SystemFileAssociations/.png/shell/ParentValue"), 123);
    neighbors.setValue(QStringLiteral("SystemFileAssociations/.png/shell/Other.Edit/Custom"),
                       QStringLiteral("keep neighbor"));
    neighbors.sync();
    require(neighbors.status() == QSettings::NoError, "create isolated native registry fixture");
    windows::ShellContextMenuRegistration::Options options;
    options.classesRoot = root;
    options.executablePath =
        QStringLiteral("C:/Native Snow Shot/雪/") + snow_shot::app::edition::executableName();
    options.fileExtensions = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("bmp")};
    windows::ShellContextMenuRegistration registration(std::move(options));
    require(registration.setMode(windows::ShellContextMenuMode::ImageAndDocumentFiles,
                                 QStringLiteral("E")),
            "native isolated registry can enable the image verbs with an access key");
    const auto before = registration.snapshot();
    for (const auto& key : before.keys)
        require(stringText(key.values.value({})).endsWith(QStringLiteral(" (&E)")),
                "native registry labels retain the mnemonic marker");
    require(before.valid && before.keys.size() == 3 &&
                before.keys
                    .value(root + QStringLiteral("\\SystemFileAssociations\\.bmp\\shell\\") +
                           snow_shot::app::edition::registryName() + QStringLiteral(".Edit"))
                    .exists,
            "native snapshots include the BMP verb in the isolated registry root");
    require(!registration.changeMode(windows::ShellContextMenuMode::Disabled, [] { return false; })
                    .success &&
                registration.snapshot().keys == before.keys,
            "native rollback restores registration trees in an isolated HKCU root");
    require(registration.setMode(windows::ShellContextMenuMode::Disabled),
            "native isolated registry can disable the image verbs");
    const auto disabled = registration.snapshot();
    require(disabled.valid && std::all_of(disabled.keys.cbegin(), disabled.keys.cend(),
                                          [](const auto& key) { return !key.exists; }),
            "native removal deletes BMP with the other owned image verbs");
    neighbors.sync();
    require(neighbors.value(QStringLiteral("SystemFileAssociations/.png/shell/ParentValue")) ==
                    123 &&
                neighbors.value(
                    QStringLiteral("SystemFileAssociations/.png/shell/Other.Edit/Custom")) ==
                    QStringLiteral("keep neighbor"),
            "native removal preserves adjacent registry values and unrelated verbs");
    neighbors.remove(QString());
    neighbors.sync();
    require(neighbors.status() == QSettings::NoError,
            "remove only the isolated test registry root");
}
#endif
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    commandAndOwnership();
    accessKeysAndRollback();
    preservationRelocationAndRollback();
    foreignOwnerAndLanguageRefresh();
    authoritativeSuffixes();
    imageOnlyScope();
#ifdef Q_OS_WIN
    isolatedNativeRegistry();
#endif
    std::cout << "Shell context menu registration tests passed\n";
    return 0;
}
