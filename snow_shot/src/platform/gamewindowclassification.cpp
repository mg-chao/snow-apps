#include "snow_shot/platform/gamewindowclassification.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>

namespace snow_shot::platform {
namespace {
QString normalizedWindowsPath(QString path) {
    path.replace(u'\\', u'/');
    if (path.startsWith(QStringLiteral("//?/UNC/"), Qt::CaseInsensitive))
        path = QStringLiteral("//") + path.mid(8);
    else if (path.startsWith(QStringLiteral("//?/")))
        path.remove(0, 4);
    return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

bool sameWindowsPath(const QString& first, const QString& second) {
    return normalizedWindowsPath(first).compare(normalizedWindowsPath(second),
                                                Qt::CaseInsensitive) == 0;
}

bool isEditorExecutable(const QString& name) {
    return name.compare(QStringLiteral("Unity.exe"), Qt::CaseInsensitive) == 0 ||
           name.startsWith(QStringLiteral("UnrealEditor"), Qt::CaseInsensitive) ||
           name.startsWith(QStringLiteral("UE4Editor"), Qt::CaseInsensitive);
}

bool gameConfigDeclaresExecutable(const QDir& directory, const QString& executablePath) {
    QFile file(directory.filePath(QStringLiteral("MicrosoftGame.config")));
    constexpr qint64 maximumConfigBytes = 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumConfigBytes)
        return false;

    QXmlStreamReader xml(file.read(maximumConfigBytes + 1));
    QStringList elements;
    bool matches = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            elements.append(xml.name().toString());
            if (elements == QStringList{QStringLiteral("Game"), QStringLiteral("ExecutableList"),
                                        QStringLiteral("Executable")}) {
                const QString name = normalizedWindowsPath(
                    xml.attributes().value(QStringLiteral("Name")).toString());
                if (!name.isEmpty() && sameWindowsPath(directory.filePath(name), executablePath))
                    matches = true;
            }
        } else if (xml.isEndElement() && !elements.isEmpty()) {
            elements.removeLast();
        }
    }
    return !xml.hasError() && matches;
}
} // namespace

QString gameConfigStoreParentKey(const QString& executablePath) {
    const QString name = QFileInfo(normalizedWindowsPath(executablePath)).fileName().toLower();
    if (name.isEmpty())
        return {};
    // GameConfigStore hashes the invariant lowercase filename as UTF-16LE, without a BOM/NUL.
    QByteArray utf16;
    utf16.reserve(name.size() * 2);
    for (QChar character : name) {
        const ushort value = character.unicode();
        utf16.append(static_cast<char>(value & 0xff));
        utf16.append(static_cast<char>(value >> 8));
    }
    return QString::fromLatin1(QCryptographicHash::hash(utf16, QCryptographicHash::Sha1).toHex());
}

bool isWindowsGameWindow(const QString& executablePath, const QString& windowClass,
                         const GameConfigStoreLookup& registeredPaths) {
    const QString normalized = normalizedWindowsPath(executablePath);
    const QFileInfo executable(normalized);
    const QString name = executable.fileName();
    if (normalized.isEmpty() || !name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive) ||
        isEditorExecutable(name))
        return false;

    if (registeredPaths) {
        const auto paths = registeredPaths(gameConfigStoreParentKey(normalized));
        for (const QString& registered : paths) {
            if (sameWindowsPath(normalized, registered))
                return true;
        }
    }

    const QDir executableDirectory = executable.dir();
    QDir directory = executableDirectory;
    // Check the executable directory, then at most eight ancestors; never scan a game library.
    for (int ancestor = 0; ancestor <= 8; ++ancestor) {
        if (gameConfigDeclaresExecutable(directory, normalized))
            return true;
        if (!directory.cdUp())
            break;
    }

    if (windowClass == QStringLiteral("UnityWndClass")) {
        return QFileInfo(executableDirectory.filePath(QStringLiteral("UnityPlayer.dll")))
                   .isFile() &&
               QFileInfo(executableDirectory.filePath(executable.completeBaseName() +
                                                      QStringLiteral("_Data")))
                   .isDir();
    }
    if (windowClass == QStringLiteral("UnrealWindow")) {
        for (const auto* suffix :
             {"-Win64-Shipping.exe", "-Win32-Shipping.exe", "-WinGDK-Shipping.exe"}) {
            if (name.endsWith(QLatin1String(suffix), Qt::CaseInsensitive))
                return true;
        }
    }
    return false;
}

bool isMacOSGameApplication(const QJsonObject& bundleMetadata) {
    // Apple categories are siblings, not child UTIs of public.app-category.games.
    static const QStringList categories{QStringLiteral("public.app-category.games"),
                                        QStringLiteral("public.app-category.action-games"),
                                        QStringLiteral("public.app-category.adventure-games"),
                                        QStringLiteral("public.app-category.arcade-games"),
                                        QStringLiteral("public.app-category.board-games"),
                                        QStringLiteral("public.app-category.card-games"),
                                        QStringLiteral("public.app-category.casino-games"),
                                        QStringLiteral("public.app-category.dice-games"),
                                        QStringLiteral("public.app-category.educational-games"),
                                        QStringLiteral("public.app-category.family-games"),
                                        QStringLiteral("public.app-category.kids-games"),
                                        QStringLiteral("public.app-category.music-games"),
                                        QStringLiteral("public.app-category.puzzle-games"),
                                        QStringLiteral("public.app-category.racing-games"),
                                        QStringLiteral("public.app-category.role-playing-games"),
                                        QStringLiteral("public.app-category.simulation-games"),
                                        QStringLiteral("public.app-category.sports-games"),
                                        QStringLiteral("public.app-category.strategy-games"),
                                        QStringLiteral("public.app-category.trivia-games"),
                                        QStringLiteral("public.app-category.word-games")};
    return categories.contains(
               bundleMetadata.value(QStringLiteral("LSApplicationCategoryType")).toString()) ||
           bundleMetadata.value(QStringLiteral("LSSupportsGameMode")).toBool() ||
           bundleMetadata.value(QStringLiteral("GCSupportsGameMode")).toBool();
}

} // namespace snow_shot::platform
