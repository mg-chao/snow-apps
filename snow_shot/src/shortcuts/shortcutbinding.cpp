#include "snow_shot/shortcuts/shortcutbinding.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace snow_shot::shortcuts {
namespace {

bool modifierOnlyKey(Qt::Key key) {
    return key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Shift ||
           key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_Super_L ||
           key == Qt::Key_Super_R;
}

Qt::KeyboardModifier modifierForKey(Qt::Key key) {
    switch (key) {
    case Qt::Key_Shift:
        return Qt::ShiftModifier;
    case Qt::Key_Control:
        return Qt::ControlModifier;
    case Qt::Key_Alt:
        return Qt::AltModifier;
    case Qt::Key_Meta:
        return Qt::MetaModifier;
    case Qt::Key_AltGr:
        return Qt::GroupSwitchModifier;
    default:
        return Qt::NoModifier;
    }
}

bool navigationKey(Qt::Key key) {
    return key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
           key == Qt::Key_Down || key == Qt::Key_Home || key == Qt::Key_End ||
           key == Qt::Key_PageUp || key == Qt::Key_PageDown;
}

std::optional<quint32> ansiVirtualKey(Qt::Key key, bool keypad) {
    if (keypad) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            constexpr quint32 keypadDigits[] = {82, 83, 84, 85, 86, 87, 88, 89, 91, 92};
            return keypadDigits[static_cast<int>(key) - static_cast<int>(Qt::Key_0)];
        }
        switch (key) {
        case Qt::Key_Period:
        case Qt::Key_Comma:
        case Qt::Key_Delete:
            return 65;
        case Qt::Key_Asterisk:
            return 67;
        case Qt::Key_Plus:
            return 69;
        case Qt::Key_Clear:
            return 71;
        case Qt::Key_Slash:
            return 75;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            return 76;
        case Qt::Key_Minus:
            return 78;
        case Qt::Key_Equal:
            return 81;
        case Qt::Key_Insert:
            return 82;
        case Qt::Key_End:
            return 83;
        case Qt::Key_Down:
            return 84;
        case Qt::Key_PageDown:
            return 85;
        case Qt::Key_Left:
            return 86;
        case Qt::Key_Right:
            return 88;
        case Qt::Key_Home:
            return 89;
        case Qt::Key_Up:
            return 91;
        case Qt::Key_PageUp:
            return 92;
        default:
            return std::nullopt;
        }
    }

    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        constexpr quint32 letters[] = {
            0,  11, 8,  2,  14, 3, 5,  4,  34, 38, 40, 37, 46,
            45, 31, 35, 12, 15, 1, 17, 32, 9,  13, 7,  16, 6,
        };
        return letters[static_cast<int>(key) - static_cast<int>(Qt::Key_A)];
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        constexpr quint32 digits[] = {29, 18, 19, 20, 21, 23, 22, 26, 28, 25};
        return digits[static_cast<int>(key) - static_cast<int>(Qt::Key_0)];
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F20) {
        constexpr quint32 functionKeys[] = {
            122, 120, 99,  118, 96,  97,  98, 100, 101, 109,
            103, 111, 105, 107, 113, 106, 64, 79,  80,  90,
        };
        return functionKeys[static_cast<int>(key) - static_cast<int>(Qt::Key_F1)];
    }

    switch (key) {
    case Qt::Key_Equal:
    case Qt::Key_Plus:
        return 24;
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        return 27;
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight:
        return 30;
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft:
        return 33;
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl:
        return 39;
    case Qt::Key_Semicolon:
    case Qt::Key_Colon:
        return 41;
    case Qt::Key_Backslash:
    case Qt::Key_Bar:
        return 42;
    case Qt::Key_Comma:
    case Qt::Key_Less:
        return 43;
    case Qt::Key_Slash:
    case Qt::Key_Question:
        return 44;
    case Qt::Key_Period:
    case Qt::Key_Greater:
        return 47;
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde:
        return 50;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return 36;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return 48;
    case Qt::Key_Space:
        return 49;
    case Qt::Key_Backspace:
        return 51;
    case Qt::Key_Escape:
        return 53;
    case Qt::Key_Help:
    case Qt::Key_Insert:
        return 114;
    case Qt::Key_Home:
        return 115;
    case Qt::Key_PageUp:
        return 116;
    case Qt::Key_Delete:
        return 117;
    case Qt::Key_End:
        return 119;
    case Qt::Key_PageDown:
        return 121;
    case Qt::Key_Left:
        return 123;
    case Qt::Key_Right:
        return 124;
    case Qt::Key_Down:
        return 125;
    case Qt::Key_Up:
        return 126;
    default:
        return std::nullopt;
    }
}

bool cocoaEvent(const QKeyEvent& event) {
#ifdef Q_OS_MACOS
    // The application runs on Cocoa. Accept explicitly populated native fields
    // as well so offscreen tests can exercise physical-key behavior without a
    // WindowServer-backed platform plugin.
    return QGuiApplication::platformName() == QStringLiteral("cocoa") ||
           event.nativeVirtualKey() != 0 || event.nativeScanCode() != 0;
#else
    Q_UNUSED(event);
    return false;
#endif
}

Qt::KeyboardModifiers normalizedEventModifiers(const QKeyEvent& event,
                                               const ShortcutIdentity& expected) {
    Qt::KeyboardModifiers modifiers = event.modifiers();
    modifiers |= modifierForKey(static_cast<Qt::Key>(event.key()));
    if (expected.physicalKey.has_value() || (navigationKey(static_cast<Qt::Key>(event.key())) &&
                                             !expected.modifiers.testFlag(Qt::KeypadModifier))) {
        modifiers &= ~Qt::KeypadModifier;
    }
    return modifiers;
}

} // namespace

QString canonicalPortableText(const QString& text, bool allowModifierOnlyShift) {
    QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const auto replaceModifier = [&trimmed](const QString& pattern, const QString& replacement) {
        trimmed.replace(QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption),
                        replacement);
    };
    // Accept common human/native spellings, but always store Qt PortableText.
    // In the shared domain Ctrl is the primary shortcut modifier (Command on
    // macOS); physical Control remains explicitly represented by Meta.
    replaceModifier(QStringLiteral("\\b(?:control|ctrl)\\b"), QStringLiteral("Ctrl"));
    replaceModifier(QStringLiteral("\\b(?:command|cmd)\\b"), QStringLiteral("Ctrl"));
    replaceModifier(QStringLiteral("\\boption\\b"), QStringLiteral("Alt"));
    replaceModifier(QStringLiteral("\\balt\\b"), QStringLiteral("Alt"));
    replaceModifier(QStringLiteral("\\bshift\\b"), QStringLiteral("Shift"));
    replaceModifier(QStringLiteral("\\b(?:windows|win|super|meta)\\b"), QStringLiteral("Meta"));
    replaceModifier(QStringLiteral("\\bnum\\b"), QStringLiteral("Num"));
    if (allowModifierOnlyShift &&
        trimmed.compare(QStringLiteral("Shift"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Shift");
    }

    QKeySequence sequence = QKeySequence::fromString(trimmed, QKeySequence::PortableText);
    if (sequence.isEmpty()) {
        sequence = QKeySequence::fromString(trimmed, QKeySequence::NativeText);
    }
    if (sequence.count() != 1) {
        return {};
    }
    const QKeyCombination combination = sequence[0];
    const Qt::Key key = combination.key();
    if (allowModifierOnlyShift && key == Qt::Key_Shift &&
        combination.keyboardModifiers() == Qt::ShiftModifier) {
        return QStringLiteral("Shift");
    }
    if (key == Qt::Key_unknown || modifierOnlyKey(key)) {
        return {};
    }
    return sequence.toString(QKeySequence::PortableText).trimmed();
}

ShortcutBinding canonicalBinding(const ShortcutBinding& binding, bool allowModifierOnlyShift) {
    ShortcutBinding result;
    result.portableText = canonicalPortableText(binding.portableText, allowModifierOnlyShift);
    if (result.portableText.isEmpty()) {
        return {};
    }
    for (auto key = binding.physicalKeys.cbegin(); key != binding.physicalKeys.cend(); ++key) {
        if (key.key() == ShortcutPlatform::MacOS && key.value() <= 127 &&
            result.portableText != QStringLiteral("Shift")) {
            result.physicalKeys.insert(key.key(), key.value());
        }
    }
    return result;
}

ShortcutBinding bindingFromPortableText(const QString& text, bool allowModifierOnlyShift) {
    return canonicalBinding(ShortcutBinding{text}, allowModifierOnlyShift);
}

ShortcutBinding bindingFromKeyEvent(const QKeyEvent& event, bool allowModifierOnlyShift) {
    const Qt::Key key = static_cast<Qt::Key>(event.key());
    if (allowModifierOnlyShift && key == Qt::Key_Shift) {
        return ShortcutBinding{QStringLiteral("Shift")};
    }
    if (key == Qt::Key_unknown || modifierOnlyKey(key)) {
        return {};
    }
    const Qt::KeyboardModifiers modifiers =
        event.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier |
                             Qt::MetaModifier | Qt::KeypadModifier);
    ShortcutBinding result{QKeySequence(QKeyCombination(modifiers, key))
                               .toString(QKeySequence::PortableText)
                               .trimmed()};
    result = canonicalBinding(result, allowModifierOnlyShift);
#ifdef Q_OS_MACOS
    if (!result.portableText.isEmpty() && cocoaEvent(event) && event.nativeVirtualKey() <= 127) {
        result.physicalKeys.insert(ShortcutPlatform::MacOS,
                                   static_cast<quint32>(event.nativeVirtualKey()));
    }
#endif
    return result;
}

ShortcutBindingList bindingsFromPortableText(const QStringList& shortcuts,
                                             bool allowModifierOnlyShift, int maximumItems) {
    ShortcutBindingList result;
    for (const QString& text : shortcuts) {
        const ShortcutBinding binding = bindingFromPortableText(text, allowModifierOnlyShift);
        const bool duplicate = std::any_of(result.cbegin(), result.cend(),
                                           [&binding](const ShortcutBinding& existing) {
                                               return bindingsConflict(existing, binding);
                                           });
        if (binding.portableText.isEmpty() || duplicate) {
            continue;
        }
        result.push_back(binding);
        if (maximumItems >= 0 && result.size() >= maximumItems) {
            break;
        }
    }
    return result;
}

QStringList portableTextList(const ShortcutBindingList& bindings) {
    QStringList result;
    result.reserve(bindings.size());
    for (const ShortcutBinding& binding : bindings) {
        if (!binding.portableText.isEmpty()) {
            result.push_back(binding.portableText);
        }
    }
    return result;
}

QJsonObject shortcutBindingToJson(const ShortcutBinding& binding) {
    QJsonObject object{{QStringLiteral("portable"), binding.portableText}};
    QJsonObject physicalKeys;
    const auto macKey = binding.physicalKeys.constFind(ShortcutPlatform::MacOS);
    if (macKey != binding.physicalKeys.cend() && *macKey <= 127) {
        physicalKeys.insert(QStringLiteral("macos"), static_cast<qint64>(*macKey));
    }
    if (!physicalKeys.isEmpty()) {
        object.insert(QStringLiteral("physical_keys"), physicalKeys);
    }
    return object;
}

ShortcutBinding shortcutBindingFromJson(const QJsonValue& value, bool allowModifierOnlyShift,
                                        bool* valid, bool* changed) {
    if (valid != nullptr) {
        *valid = false;
    }
    bool normalizedChanged = false;
    ShortcutBinding candidate;
    if (value.isString()) {
        candidate.portableText = value.toString();
        normalizedChanged = true;
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (!object.value(QStringLiteral("portable")).isString()) {
            if (changed != nullptr) {
                *changed = true;
            }
            return {};
        }
        candidate.portableText = object.value(QStringLiteral("portable")).toString();
        normalizedChanged =
            object.size() > (object.contains(QStringLiteral("physical_keys")) ? 2 : 1);
        const QJsonValue physicalValue = object.value(QStringLiteral("physical_keys"));
        if (!physicalValue.isUndefined()) {
            if (physicalValue.isObject()) {
                const QJsonObject physical = physicalValue.toObject();
                const QJsonValue macValue = physical.value(QStringLiteral("macos"));
                if (macValue.isDouble() && macValue.toDouble() >= 0 && macValue.toDouble() <= 127 &&
                    std::floor(macValue.toDouble()) == macValue.toDouble()) {
                    candidate.physicalKeys.insert(ShortcutPlatform::MacOS,
                                                  static_cast<quint32>(macValue.toInt()));
                } else if (!macValue.isUndefined()) {
                    normalizedChanged = true;
                }
                if (physical.size() != (macValue.isUndefined() ? 0 : 1)) {
                    normalizedChanged = true;
                }
            } else {
                normalizedChanged = true;
            }
        }
    } else {
        if (changed != nullptr) {
            *changed = true;
        }
        return {};
    }

    const ShortcutBinding result = canonicalBinding(candidate, allowModifierOnlyShift);
    if (result.portableText.isEmpty()) {
        if (changed != nullptr) {
            *changed = true;
        }
        return {};
    }
    normalizedChanged =
        normalizedChanged || result != candidate || shortcutBindingToJson(result) != value;
    if (valid != nullptr) {
        *valid = true;
    }
    if (changed != nullptr) {
        *changed = normalizedChanged;
    }
    return result;
}

QJsonArray shortcutBindingsToJson(const ShortcutBindingList& bindings) {
    QJsonArray result;
    for (const ShortcutBinding& binding : bindings) {
        if (!binding.portableText.isEmpty()) {
            result.push_back(shortcutBindingToJson(binding));
        }
    }
    return result;
}

ShortcutBindingList shortcutBindingsFromJson(const QJsonValue& value, bool allowModifierOnlyShift,
                                             int maximumItems, bool* valid, bool* changed) {
    if (valid != nullptr) {
        *valid = false;
    }
    if (!value.isArray()) {
        if (changed != nullptr) {
            *changed = false;
        }
        return {};
    }
    ShortcutBindingList result;
    bool normalizedChanged = false;
    for (const QJsonValue& item : value.toArray()) {
        bool itemValid = false;
        bool itemChanged = false;
        const ShortcutBinding binding =
            shortcutBindingFromJson(item, allowModifierOnlyShift, &itemValid, &itemChanged);
        const bool duplicate = std::any_of(result.cbegin(), result.cend(),
                                           [&binding](const ShortcutBinding& existing) {
                                               return bindingsConflict(existing, binding);
                                           });
        if (!itemValid || duplicate || (maximumItems >= 0 && result.size() >= maximumItems)) {
            normalizedChanged = true;
            continue;
        }
        result.push_back(binding);
        normalizedChanged = normalizedChanged || itemChanged;
    }
    const QJsonArray normalized = shortcutBindingsToJson(result);
    normalizedChanged = normalizedChanged || normalized != value.toArray();
    if (valid != nullptr) {
        *valid = true;
    }
    if (changed != nullptr) {
        *changed = normalizedChanged;
    }
    return result;
}

std::optional<quint32> macVirtualKeyForBinding(const ShortcutBinding& binding) {
    const auto physical = binding.physicalKeys.constFind(ShortcutPlatform::MacOS);
    if (physical != binding.physicalKeys.cend() && *physical <= 127) {
        return *physical;
    }

    if (binding.portableText == QStringLiteral("Shift")) {
        return std::nullopt;
    }
    const QKeySequence sequence =
        QKeySequence::fromString(binding.portableText, QKeySequence::PortableText);
    if (sequence.count() != 1) {
        return std::nullopt;
    }
    const QKeyCombination combination = sequence[0];
    return ansiVirtualKey(combination.key(),
                          combination.keyboardModifiers().testFlag(Qt::KeypadModifier));
}

ShortcutIdentity effectiveIdentity(const ShortcutBinding& binding) {
    ShortcutIdentity result;
    if (binding.portableText == QStringLiteral("Shift")) {
        result.key = Qt::Key_Shift;
        result.modifiers = Qt::ShiftModifier;
        return result;
    }
    const QKeySequence sequence =
        QKeySequence::fromString(binding.portableText, QKeySequence::PortableText);
    if (sequence.count() != 1) {
        return result;
    }
    const QKeyCombination combination = sequence[0];
    result.key = combination.key();
    result.modifiers = combination.keyboardModifiers();
#ifdef Q_OS_MACOS
    result.physicalKey = macVirtualKeyForBinding(binding);
    if (result.physicalKey.has_value()) {
        result.modifiers &= ~Qt::KeypadModifier;
    }
#endif
    return result;
}

bool bindingsConflict(const ShortcutBinding& first, const ShortcutBinding& second) {
    const ShortcutIdentity firstIdentity = effectiveIdentity(first);
    const ShortcutIdentity secondIdentity = effectiveIdentity(second);
    if (firstIdentity.key == Qt::Key_unknown || secondIdentity.key == Qt::Key_unknown) {
        return false;
    }
    if (firstIdentity.physicalKey.has_value() && secondIdentity.physicalKey.has_value()) {
        return firstIdentity.physicalKey == secondIdentity.physicalKey &&
               firstIdentity.modifiers == secondIdentity.modifiers;
    }
    return firstIdentity.key == secondIdentity.key &&
           firstIdentity.modifiers == secondIdentity.modifiers;
}

bool shortcutMatchesEvent(const ShortcutBinding& binding, const QKeyEvent& event,
                          Qt::KeyboardModifiers allowedAdditionalModifiers) {
    const ShortcutIdentity expected = effectiveIdentity(binding);
    if (expected.key == Qt::Key_unknown) {
        return false;
    }
    if (expected.physicalKey.has_value() && cocoaEvent(event)) {
        if (event.nativeVirtualKey() != *expected.physicalKey) {
            return false;
        }
    } else if (expected.key != static_cast<Qt::Key>(event.key())) {
        return false;
    }

    const Qt::KeyboardModifiers actual = normalizedEventModifiers(event, expected);
    const Qt::KeyboardModifiers unexpected =
        actual & ~(expected.modifiers | allowedAdditionalModifiers);
    return (actual & expected.modifiers) == expected.modifiers && unexpected == Qt::NoModifier;
}

bool shortcutReleaseMatchesEvent(const ShortcutBinding& binding, const QKeyEvent& event) {
    const ShortcutIdentity expected = effectiveIdentity(binding);
    if (expected.physicalKey.has_value() && cocoaEvent(event)) {
        return event.nativeVirtualKey() == *expected.physicalKey;
    }
    return expected.key == static_cast<Qt::Key>(event.key());
}

quint64 shortcutKeyToken(const ShortcutBinding& binding) {
    const ShortcutIdentity identity = effectiveIdentity(binding);
    const bool hasExplicitMacKey = binding.physicalKeys.contains(ShortcutPlatform::MacOS);
    if (identity.physicalKey.has_value() &&
        (QGuiApplication::platformName() == QStringLiteral("cocoa") || hasExplicitMacKey)) {
        return (quint64{1} << 63U) | static_cast<quint64>(*identity.physicalKey);
    }
    return static_cast<quint64>(static_cast<quint32>(identity.key));
}

quint64 eventKeyToken(const QKeyEvent& event) {
    if (cocoaEvent(event) && event.nativeVirtualKey() <= 127) {
        return (quint64{1} << 63U) | static_cast<quint64>(event.nativeVirtualKey());
    }
    return static_cast<quint64>(static_cast<quint32>(event.key()));
}

} // namespace snow_shot::shortcuts
