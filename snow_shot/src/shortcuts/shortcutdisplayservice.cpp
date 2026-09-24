#include "snow_shot/shortcuts/shortcutdisplayservice.h"

#include <QCoreApplication>
#include <QKeySequence>
#include <QMetaObject>
#include <QRegularExpression>

#include <iterator>

#ifdef Q_OS_MACOS
#include <Carbon/Carbon.h>
#endif

namespace snow_shot::shortcuts {
namespace {

#ifdef Q_OS_MACOS
QString fallbackKeyText(const ShortcutBinding& binding) {
    const QKeySequence sequence =
        QKeySequence::fromString(binding.portableText, QKeySequence::PortableText);
    if (sequence.count() != 1) {
        return binding.portableText;
    }
    return QKeySequence(QKeyCombination(Qt::NoModifier, sequence[0].key()))
        .toString(QKeySequence::NativeText);
}

QString specialMacKeyLegend(quint32 keyCode) {
    switch (keyCode) {
    case 36:
    case 76:
        return QStringLiteral("↩");
    case 48:
        return QStringLiteral("⇥");
    case 49:
        return QStringLiteral("Space");
    case 51:
        return QStringLiteral("⌫");
    case 53:
        return QStringLiteral("Esc");
    case 114:
        return QStringLiteral("Help");
    case 115:
        return QStringLiteral("↖");
    case 116:
        return QStringLiteral("⇞");
    case 117:
        return QStringLiteral("⌦");
    case 119:
        return QStringLiteral("↘");
    case 121:
        return QStringLiteral("⇟");
    case 123:
        return QStringLiteral("←");
    case 124:
        return QStringLiteral("→");
    case 125:
        return QStringLiteral("↓");
    case 126:
        return QStringLiteral("↑");
    default:
        break;
    }
    constexpr quint32 functionCodes[] = {
        122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111, 105, 107, 113, 106, 64, 79, 80, 90,
    };
    for (int index = 0; index < 20; ++index) {
        if (functionCodes[index] == keyCode) {
            return QStringLiteral("F%1").arg(index + 1);
        }
    }
    return {};
}

QString translatedMacKeyLegend(quint32 keyCode) {
    const QString special = specialMacKeyLegend(keyCode);
    if (!special.isEmpty()) {
        return special;
    }

    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (source == nullptr) {
        source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
    }
    if (source == nullptr) {
        return {};
    }
    const auto* layoutData =
        static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (layoutData == nullptr) {
        CFRelease(source);
        source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
        if (source != nullptr) {
            layoutData = static_cast<CFDataRef>(
                TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
        }
    }
    if (source == nullptr || layoutData == nullptr) {
        if (source != nullptr) {
            CFRelease(source);
        }
        return {};
    }

    const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layoutData));
    UInt32 deadKeyState = 0;
    UniChar characters[8]{};
    UniCharCount length = 0;
    const OSStatus status =
        UCKeyTranslate(layout, static_cast<UInt16>(keyCode), kUCKeyActionDisplay, 0, LMGetKbdType(),
                       kUCKeyTranslateNoDeadKeysBit, &deadKeyState,
                       static_cast<UniCharCount>(std::size(characters)), &length, characters);
    CFRelease(source);
    if (status != noErr || length == 0) {
        return {};
    }
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(characters),
                              static_cast<qsizetype>(length))
        .toUpper();
}

QString nativeKeypadPrefix() {
    const QString keypadOne = QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_1))
                                  .toString(QKeySequence::NativeText);
    const QString plainOne = QKeySequence(Qt::Key_1).toString(QKeySequence::NativeText);
    if (!plainOne.isEmpty() && keypadOne.endsWith(plainOne)) {
        return keypadOne.first(keypadOne.size() - plainOne.size());
    }
    return QStringLiteral("Num");
}
#endif

#ifndef Q_OS_MACOS
QString portableDisplayText(const QString& portableText) {
    if (portableText == QStringLiteral("Shift")) {
        return QStringLiteral("Shift");
    }
    const QKeySequence sequence =
        QKeySequence::fromString(portableText, QKeySequence::PortableText);
    if (sequence.count() != 1) {
        return portableText;
    }
    return sequence.toString(QKeySequence::NativeText).trimmed();
}
#endif

} // namespace

class ShortcutDisplayService::Impl {
  public:
#ifdef Q_OS_MACOS
    explicit Impl(ShortcutDisplayService& owner) : q(owner) {
        CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(), this,
                                        inputSourceChanged,
                                        kTISNotifySelectedKeyboardInputSourceChanged, nullptr,
                                        CFNotificationSuspensionBehaviorDeliverImmediately);
    }

    ~Impl() {
        CFNotificationCenterRemoveObserver(CFNotificationCenterGetDistributedCenter(), this,
                                           kTISNotifySelectedKeyboardInputSourceChanged, nullptr);
    }

    static void inputSourceChanged(CFNotificationCenterRef, void* observer, CFStringRef,
                                   const void*, CFDictionaryRef) {
        auto* self = static_cast<Impl*>(observer);
        QMetaObject::invokeMethod(&self->q, &ShortcutDisplayService::refresh, Qt::QueuedConnection);
    }

    ShortcutDisplayService& q;
#else
    explicit Impl(ShortcutDisplayService&) {}
#endif
};

ShortcutDisplayService& ShortcutDisplayService::instance() {
    static ShortcutDisplayService service;
    return service;
}

ShortcutDisplayService::ShortcutDisplayService() : m_impl(std::make_unique<Impl>(*this)) {}
ShortcutDisplayService::~ShortcutDisplayService() = default;

QString ShortcutDisplayService::modifierText(Qt::KeyboardModifiers modifiers) const {
#ifdef Q_OS_MACOS
    QString result;
    if (modifiers.testFlag(Qt::MetaModifier)) {
        result += QStringLiteral("⌃");
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        result += QStringLiteral("⌥");
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        result += QStringLiteral("⇧");
    }
    if (modifiers.testFlag(Qt::ControlModifier)) {
        result += QStringLiteral("⌘");
    }
    return result;
#else
    QStringList parts;
    if (modifiers.testFlag(Qt::ControlModifier)) {
        parts.push_back(QStringLiteral("Ctrl"));
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        parts.push_back(QStringLiteral("Alt"));
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        parts.push_back(QStringLiteral("Shift"));
    }
    if (modifiers.testFlag(Qt::MetaModifier)) {
#ifdef Q_OS_WIN
        parts.push_back(QStringLiteral("Win"));
#else
        parts.push_back(QStringLiteral("Meta"));
#endif
    }
    return parts.join(QStringLiteral("+"));
#endif
}

QString ShortcutDisplayService::text(const ShortcutBinding& binding) const {
    const ShortcutBinding canonical = canonicalBinding(binding, true);
    if (canonical.portableText.isEmpty()) {
        return {};
    }
#ifdef Q_OS_MACOS
    if (canonical.portableText == QStringLiteral("Shift")) {
        return modifierText(Qt::ShiftModifier);
    }
    const ShortcutIdentity identity = effectiveIdentity(canonical);
    QString keyText;
    if (identity.physicalKey.has_value()) {
        keyText = translatedMacKeyLegend(*identity.physicalKey);
    }
    if (keyText.isEmpty()) {
        keyText = fallbackKeyText(canonical);
    }
    const QKeySequence sequence =
        QKeySequence::fromString(canonical.portableText, QKeySequence::PortableText);
    if (sequence.count() == 1 && sequence[0].keyboardModifiers().testFlag(Qt::KeypadModifier)) {
        keyText.prepend(nativeKeypadPrefix());
    }
    const QString modifiers = modifierText(identity.modifiers);
    return modifiers + keyText;
#else
    return portableDisplayText(canonical.portableText);
#endif
}

QString ShortcutDisplayService::text(const ShortcutBindingList& bindings) const {
    QStringList parts;
    parts.reserve(bindings.size());
    for (const ShortcutBinding& binding : bindings) {
        const QString display = text(binding);
        if (!display.isEmpty()) {
            parts.push_back(display);
        }
    }
    return parts.join(QStringLiteral(" / "));
}

void ShortcutDisplayService::refresh() {
    emit displayChanged();
}

QString formatShortcutDisplayText(const ShortcutBinding& binding) {
    return ShortcutDisplayService::instance().text(binding);
}

QString formatShortcutListDisplayText(const ShortcutBindingList& bindings) {
    return ShortcutDisplayService::instance().text(bindings);
}

} // namespace snow_shot::shortcuts
