#pragma once

#include <QOperatingSystemVersion>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace snow_shot::presentation {

// Use the settings key labels everywhere; stored shortcut syntax stays unchanged.
inline QString formatShortcutDisplayText(const QString& shortcut) {
    QString displayText = shortcut;
    displayText.replace(QRegularExpression(QStringLiteral("\\s*\\+\\s*")), QStringLiteral("+"));
    displayText.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    displayText = displayText.trimmed();
    if (displayText.isEmpty()) {
        return displayText;
    }

    if (displayText.compare(QStringLiteral("Shift+Shift"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Shift");
    }

    const bool hasPlusKey =
        displayText == QStringLiteral("+") || displayText.endsWith(QStringLiteral("++"));

    displayText.replace(QStringLiteral("Period"), QStringLiteral("."));
    displayText.replace(QStringLiteral("Comma"), QStringLiteral(","));
    // PortableText encodes Qt::KeypadModifier as "Num+"; it is not a chord.
    displayText.replace(QStringLiteral("Num+"), QStringLiteral("Num "));

    if (hasPlusKey) {
        displayText.chop(1);
        displayText.append(QStringLiteral("Plus"));
    }

    if (QOperatingSystemVersion::currentType() == QOperatingSystemVersion::MacOS) {
        displayText.replace(QStringLiteral("Meta"), QStringLiteral("Command"));
        displayText.replace(QStringLiteral("Alt"), QStringLiteral("Option"));
        displayText.replace(QStringLiteral("Ctrl"), QStringLiteral("Control"));
    } else {
        displayText.replace(QStringLiteral("Meta"), QStringLiteral("Win"));
        displayText.replace(QStringLiteral("Super"), QStringLiteral("Win"));
    }

    return displayText;
}

inline QString formatShortcutListDisplayText(const QStringList& shortcuts) {
    QStringList displayShortcuts;
    displayShortcuts.reserve(shortcuts.size());
    for (const QString& shortcut : shortcuts) {
        const QString displayText = formatShortcutDisplayText(shortcut);
        if (!displayText.isEmpty()) {
            displayShortcuts.push_back(displayText);
        }
    }
    return displayShortcuts.join(QStringLiteral(" / "));
}

} // namespace snow_shot::presentation
