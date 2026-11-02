#include "snow_shot/platform/gamewindowclassification.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

using namespace snow_shot::platform;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void writeFile(const QString& path, const QByteArray& bytes = {}) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "write classifier fixture");
}

void registryIdentityRequiresAnExactPath() {
    const QString executable = QStringLiteral("C:/Games/Dota 2/game/bin/win64/dota2.exe");
    require(gameConfigStoreParentKey(executable) ==
                    QStringLiteral("4b22c8413f04739c0bf5d27fede185c9beadba4c") &&
                gameConfigStoreParentKey(QStringLiteral("D:\\Other\\DOTA2.EXE")) ==
                    gameConfigStoreParentKey(executable) &&
                gameConfigStoreParentKey({}).isEmpty(),
            "indexed lookup uses the verified lowercase UTF-16LE filename SHA1");
    int lookups = 0;
    const auto lookup = [&](const QString& parentKey) {
        ++lookups;
        require(parentKey == gameConfigStoreParentKey(executable), "query the executable index");
        return QStringList{QStringLiteral("\\\\?\\C:\\GAMES\\Dota 2\\game\\bin\\win64\\DOTA2.EXE")};
    };
    require(isWindowsGameWindow(executable, {}, lookup) && lookups == 1,
            "registered paths match case, separators and extended path prefixes");
    require(!isWindowsGameWindow(QStringLiteral("D:/Other/dota2.exe"), {}, lookup),
            "a registered filename must not classify a different executable path");
    require(!isWindowsGameWindow(executable, {}, {}) &&
                !isWindowsGameWindow({}, QStringLiteral("UnrealWindow"), lookup),
            "missing registry, denied process lookup and missing metadata remain unknown");
    for (const QString& editor : {QStringLiteral("Unity.exe"), QStringLiteral("UnrealEditor.exe"),
                                  QStringLiteral("UE4Editor-Cmd.exe")}) {
        const QString path = QStringLiteral("C:/Editors/") + editor;
        require(!isWindowsGameWindow(path, QStringLiteral("UnrealWindow"),
                                     [&](const QString&) { return QStringList{path}; }),
                "editor exclusions also apply to registered executables");
    }
}

void microsoftGameConfigMustDeclareTheFocusedExecutable() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "create game config fixture directory");
    const QDir root(temporary.path());
    require(root.mkpath(QStringLiteral("bin/x64")), "create nested executable directory");
    const QString executable = root.filePath(QStringLiteral("bin/x64/game.exe"));
    const QString config = root.filePath(QStringLiteral("MicrosoftGame.config"));
    writeFile(executable);
    writeFile(config, "<Game><ExecutableList><Executable Name=\"bin\\x64\\game.exe\"/>"
                      "</ExecutableList></Game>");
    require(isWindowsGameWindow(executable, {}, {}),
            "match the exact declared relative executable");
    require(!isWindowsGameWindow(root.filePath(QStringLiteral("bin/x64/launcher.exe")), {}, {}),
            "a neighboring executable does not inherit game identity");
    for (const QByteArray& invalid :
         {QByteArray("<Game><ExecutableList><Executable Name=\"bin/x64/game.exe\"/>"),
          QByteArray("<Other><ExecutableList><Executable Name=\"bin/x64/game.exe\"/>"
                     "</ExecutableList></Other>"),
          QByteArray("<Game><ExecutableList><Executable Name=\"other/game.exe\"/>"
                     "</ExecutableList></Game>"),
          QByteArray("<Game><ExecutableList><Executable/></ExecutableList></Game>")}) {
        writeFile(config, invalid);
        require(!isWindowsGameWindow(executable, {}, {}),
                "reject invalid or unrelated game configs");
    }
    writeFile(config, QByteArray(1024 * 1024 + 1, ' '));
    require(!isWindowsGameWindow(executable, {}, {}), "bound game configuration file reads");

    QString relative;
    for (int level = 0; level < 8; ++level)
        relative += QStringLiteral("nested/");
    require(root.mkpath(relative + QStringLiteral("ninth")), "create ancestor limit fixtures");
    const QByteArray declaration = "<Game><ExecutableList><Executable Name=\"";
    const QByteArray tail = "\"/></ExecutableList></Game>";
    writeFile(config, declaration + (relative + QStringLiteral("game.exe")).toUtf8() + tail);
    require(isWindowsGameWindow(root.filePath(relative + QStringLiteral("game.exe")), {}, {}),
            "the eighth ancestor remains eligible");
    relative += QStringLiteral("ninth/game.exe");
    writeFile(config, declaration + relative.toUtf8() + tail);
    require(!isWindowsGameWindow(root.filePath(relative), {}, {}),
            "do not search a ninth ancestor");
}

void runtimeEvidenceMustBeSpecific() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "create engine fixture directory");
    const QDir root(temporary.path());
    const QString game = root.filePath(QStringLiteral("Game.exe"));
    writeFile(game);
    require(!isWindowsGameWindow(game, QStringLiteral("UnityWndClass"), {}),
            "a Unity class alone is insufficient");
    writeFile(root.filePath(QStringLiteral("UnityPlayer.dll")));
    require(!isWindowsGameWindow(game, QStringLiteral("UnityWndClass"), {}),
            "Unity also needs its matching player data directory");
    require(root.mkdir(QStringLiteral("Game_Data")), "create Unity data fixture");
    require(isWindowsGameWindow(game, QStringLiteral("UnityWndClass"), {}),
            "matching standalone Unity runtime evidence identifies a game");
    for (const QString& generic : {QStringLiteral("SDL_app"), QStringLiteral("GLFW30"),
                                   QStringLiteral("Chrome_WidgetWin_1"), QStringLiteral("game")}) {
        require(!isWindowsGameWindow(game, generic, {}), "generic renderers are not game evidence");
    }
    writeFile(root.filePath(QStringLiteral("d3d11.dll")));
    writeFile(root.filePath(QStringLiteral("vulkan-1.dll")));
    require(!isWindowsGameWindow(game, {}, {}), "graphics DLLs alone never identify games");
    for (const QString& architecture :
         {QStringLiteral("Win64"), QStringLiteral("Win32"), QStringLiteral("WinGDK")}) {
        const QString shipping =
            root.filePath(QStringLiteral("Game-") + architecture + QStringLiteral("-Shipping.exe"));
        require(isWindowsGameWindow(shipping, QStringLiteral("UnrealWindow"), {}) &&
                    !isWindowsGameWindow(shipping, QStringLiteral("SDL_app"), {}),
                "Unreal needs both its runtime class and a shipping executable");
    }
    require(!isWindowsGameWindow(game, QStringLiteral("UnrealWindow"), {}),
            "a generic Unreal window alone remains unknown");
}

void macOSMetadataIsExplicitAndIndependentOfGameModeState() {
    const QString categoryKey = QStringLiteral("LSApplicationCategoryType");
    const QStringList categories{QStringLiteral("games"),
                                 QStringLiteral("action-games"),
                                 QStringLiteral("adventure-games"),
                                 QStringLiteral("arcade-games"),
                                 QStringLiteral("board-games"),
                                 QStringLiteral("card-games"),
                                 QStringLiteral("casino-games"),
                                 QStringLiteral("dice-games"),
                                 QStringLiteral("educational-games"),
                                 QStringLiteral("family-games"),
                                 QStringLiteral("kids-games"),
                                 QStringLiteral("music-games"),
                                 QStringLiteral("puzzle-games"),
                                 QStringLiteral("racing-games"),
                                 QStringLiteral("role-playing-games"),
                                 QStringLiteral("simulation-games"),
                                 QStringLiteral("sports-games"),
                                 QStringLiteral("strategy-games"),
                                 QStringLiteral("trivia-games"),
                                 QStringLiteral("word-games")};
    for (const QString& category : categories) {
        require(isMacOSGameApplication(
                    {{categoryKey, QStringLiteral("public.app-category.") + category},
                     {QStringLiteral("LSSupportsGameMode"), false},
                     {QStringLiteral("GCSupportsGameMode"), false}}),
                "all Apple game categories qualify even when Game Mode is disabled");
    }
    for (const QString& key :
         {QStringLiteral("LSSupportsGameMode"), QStringLiteral("GCSupportsGameMode")}) {
        require(isMacOSGameApplication({{key, true}}) && !isMacOSGameApplication({{key, false}}) &&
                    !isMacOSGameApplication({{key, 1}}) &&
                    !isMacOSGameApplication({{key, QStringLiteral("true")}}),
                "Game Mode opt-ins must be explicit Boolean true");
    }
    require(
        !isMacOSGameApplication({}) &&
            !isMacOSGameApplication({{categoryKey, QStringLiteral("public.app-category.video")}}) &&
            !isMacOSGameApplication({{categoryKey, QStringLiteral("custom.action-games")}}) &&
            !isMacOSGameApplication({{categoryKey, 1}}),
        "missing, malformed and non-game categories remain unknown");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    registryIdentityRequiresAnExactPath();
    microsoftGameConfigMustDeclareTheFocusedExecutable();
    runtimeEvidenceMustBeSpecific();
    macOSMetadataIsExplicitAndIndependentOfGameModeState();
    return 0;
}
