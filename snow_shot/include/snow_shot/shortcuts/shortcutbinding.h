#ifndef SNOW_SHOT_SHORTCUTS_SHORTCUTBINDING_H
#define SNOW_SHOT_SHORTCUTS_SHORTCUTBINDING_H

#include <QKeyCombination>
#include <QKeySequence>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

class QKeyEvent;

namespace snow_shot::shortcuts {

enum class ShortcutPlatform {
    MacOS,
};

struct ShortcutBinding {
    QString portableText;
    QMap<ShortcutPlatform, quint32> physicalKeys;

    ShortcutBinding() = default;
    ShortcutBinding(QString portableTextValue) : portableText(std::move(portableTextValue)) {}

    friend bool operator==(const ShortcutBinding& first, const ShortcutBinding& second) {
        return first.portableText == second.portableText &&
               first.physicalKeys == second.physicalKeys;
    }
    friend bool operator!=(const ShortcutBinding& first, const ShortcutBinding& second) {
        return !(first == second);
    }
};

using ShortcutBindingList = QList<ShortcutBinding>;
using ShortcutBindingMap = QMap<QString, ShortcutBindingList>;

struct ShortcutIdentity {
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    Qt::Key key = Qt::Key_unknown;
    std::optional<quint32> physicalKey;

    friend bool operator==(const ShortcutIdentity& first, const ShortcutIdentity& second) {
        return first.modifiers == second.modifiers && first.key == second.key &&
               first.physicalKey == second.physicalKey;
    }
    friend bool operator!=(const ShortcutIdentity& first, const ShortcutIdentity& second) {
        return !(first == second);
    }
};

// Command identity only; never use this to derive inserted text.
[[nodiscard]] Qt::Key commandKey(const QKeyEvent& event);
[[nodiscard]] Qt::Key commandKey(const ShortcutBinding& binding);
[[nodiscard]] bool matchesStandardShortcut(const QKeyEvent& event, QKeySequence::StandardKey key);

[[nodiscard]] QString canonicalPortableText(const QString& text,
                                            bool allowModifierOnlyShift = false,
                                            bool allowModifierOnlyAlt = false,
                                            bool allowModifierOnlyControl = false);
[[nodiscard]] ShortcutBinding canonicalBinding(const ShortcutBinding& binding,
                                               bool allowModifierOnlyShift = false,
                                               bool allowModifierOnlyAlt = false,
                                               bool allowModifierOnlyControl = false);
[[nodiscard]] ShortcutBinding bindingFromPortableText(const QString& text,
                                                      bool allowModifierOnlyShift = false,
                                                      bool allowModifierOnlyAlt = false);
[[nodiscard]] ShortcutBinding bindingFromKeyEvent(const QKeyEvent& event,
                                                  bool allowModifierOnlyShift = false,
                                                  bool allowModifierOnlyAlt = false);
[[nodiscard]] ShortcutBindingList bindingsFromPortableText(const QStringList& shortcuts,
                                                           bool allowModifierOnlyShift = false,
                                                           int maximumItems = -1,
                                                           bool allowModifierOnlyAlt = false);
[[nodiscard]] QStringList portableTextList(const ShortcutBindingList& bindings);
[[nodiscard]] QJsonObject shortcutBindingToJson(const ShortcutBinding& binding);
[[nodiscard]] ShortcutBinding
shortcutBindingFromJson(const QJsonValue& value, bool allowModifierOnlyShift, bool* valid = nullptr,
                        bool* changed = nullptr, bool allowModifierOnlyAlt = false);
[[nodiscard]] QJsonArray shortcutBindingsToJson(const ShortcutBindingList& bindings);
[[nodiscard]] ShortcutBindingList
shortcutBindingsFromJson(const QJsonValue& value, bool allowModifierOnlyShift,
                         int maximumItems = -1, bool* valid = nullptr, bool* changed = nullptr,
                         bool allowModifierOnlyAlt = false);
[[nodiscard]] ShortcutIdentity effectiveIdentity(const ShortcutBinding& binding);
[[nodiscard]] bool bindingsConflict(const ShortcutBinding& first, const ShortcutBinding& second);
[[nodiscard]] bool
shortcutMatchesEvent(const ShortcutBinding& binding, const QKeyEvent& event,
                     Qt::KeyboardModifiers allowedAdditionalModifiers = Qt::NoModifier);
[[nodiscard]] bool shortcutReleaseMatchesEvent(const ShortcutBinding& binding,
                                               const QKeyEvent& event);
[[nodiscard]] quint64 shortcutKeyToken(const ShortcutBinding& binding);
[[nodiscard]] quint64 eventKeyToken(const QKeyEvent& event);
[[nodiscard]] std::optional<quint32> macVirtualKeyForBinding(const ShortcutBinding& binding);

} // namespace snow_shot::shortcuts

Q_DECLARE_METATYPE(snow_shot::shortcuts::ShortcutBinding)
Q_DECLARE_METATYPE(snow_shot::shortcuts::ShortcutBindingList)

#endif // SNOW_SHOT_SHORTCUTS_SHORTCUTBINDING_H
