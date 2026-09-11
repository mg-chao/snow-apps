#include <Carbon/Carbon.h>

#include "snow_shot/platform/macos/globalshortcutbackend.h"

#include <QGuiApplication>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    using snow_shot::platform::macos::nativeShortcut;
    require(!nativeShortcut(QString()).valid, "Empty shortcuts must be rejected");
    require(!nativeShortcut(QStringLiteral("Ctrl+K, Ctrl+C")).valid,
            "Multi-stroke shortcuts cannot be registered globally");
    const auto function = nativeShortcut(QStringLiteral("F1"));
    require(function.valid && function.keyCode == kVK_F1 && function.modifiers == 0,
            "The default F1 screenshot shortcut must resolve to the native function key");
    const auto command = nativeShortcut(QStringLiteral("Ctrl+Shift+F19"));
    require(command.valid && command.keyCode == kVK_F19 && command.modifiers == (cmdKey | shiftKey),
            "Qt Control maps to Command on macOS by default");
    const auto control = nativeShortcut(QStringLiteral("Meta+Alt+Left"));
    require(control.valid && control.keyCode == kVK_LeftArrow &&
                control.modifiers == (controlKey | optionKey),
            "Qt Meta maps to physical Control on macOS by default");
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta, true);
    const auto unswapped = nativeShortcut(QStringLiteral("Ctrl+F1"));
    require(unswapped.valid && unswapped.modifiers == controlKey,
            "Explicitly disabling Qt's modifier swap must be respected");
    auto backend = snow_shot::platform::macos::createGlobalShortcutBackend();
    int activated = 0;
    backend->setActivationHandler([&activated](int id) { activated = id; });
    const auto registered = backend->registerShortcut(73, QStringLiteral("Ctrl+Alt+Shift+F19"));
    require(registered.registered, "A native hot key must register successfully");
    EventRef event = nullptr;
    require(CreateEvent(nullptr, kEventClassKeyboard, kEventHotKeyPressed, 0, kEventAttributeNone,
                        &event) == noErr,
            "The hot-key event must be created");
    const EventHotKeyID id{0x534e4f57, 73};
    require(SetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, sizeof(id), &id) ==
                noErr,
            "The registered hot-key identity must be attached");
    require(SendEventToEventTarget(event, GetApplicationEventTarget()) == noErr && activated == 73,
            "The native application event handler must dispatch the registered hot key");
    backend->unregisterShortcut(73);
    activated = 0;
    static_cast<void>(SendEventToEventTarget(event, GetApplicationEventTarget()));
    require(activated == 0, "Unregistered hot keys must not dispatch stale callbacks");
    ReleaseEvent(event);
    require(backend->registerShortcut(74, QStringLiteral("Ctrl+Alt+Shift+F19")).registered,
            "Unregistering must release the native hot key for reuse");
    std::cout << "macOS shortcut mapping and native lifecycle checks passed\n";
}
