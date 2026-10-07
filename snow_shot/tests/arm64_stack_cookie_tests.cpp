#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimeZone>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void stringListsPreserveSortAndDeduplication() {
    QStringList activationKeys{QStringLiteral("windows")};
    activationKeys.removeDuplicates();
    activationKeys.sort();
    require(activationKeys == QStringList{QStringLiteral("windows")},
            "a persisted singleton global mouse activation key must survive sorting");

    for (const auto mode : {Qt::CaseSensitive, Qt::CaseInsensitive}) {
        for (const int size : {0, 1, 2, 32, 33, 64}) {
            QStringList values;
            for (int index = size; index > 0; --index) {
                values.append(QString::number(index));
            }
            values.sort(mode);
            require(values.size() == size && std::is_sorted(values.cbegin(), values.cend()),
                    "Qt string sorting must return and preserve every item across sort boundaries");
            QStringList duplicated = values + values;
            require(duplicated.removeDuplicates() == size && duplicated == values,
                    "deduplication must preserve the canonical sorted activation keys");
        }
    }
}

void singletonIniSettingsPersistAndReload() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "INI regression requires isolated temporary storage");
    const QString path = QDir(temporary.path()).filePath(QStringLiteral("single-key.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("activation_key"), QStringLiteral("windows"));
        // This exercises QtCore's QSettingsIniKey sort. The affected ARM64 LTCG
        // output loses LR while returning from its singleton branch.
        settings.sync();
        require(settings.status() == QSettings::NoError,
                "a singleton INI configuration must flush successfully");
    }
    QSettings reloaded(path, QSettings::IniFormat);
    require(reloaded.value(QStringLiteral("activation_key")).toString() ==
                QStringLiteral("windows"),
            "a singleton INI configuration must survive reopening");
}

void timeZoneEnumerationReturnsValidIdentifiers() {
    // The same broken generated epilogue also affected QtCore's QByteArray and
    // QByteArrayView sorting while constructing the available time zone list.
    const auto identifiers = QTimeZone::availableTimeZoneIds();
    require(!identifiers.isEmpty(), "time zone enumeration must return the available identifiers");
    require(std::all_of(identifiers.cbegin(), identifiers.cend(),
                        [](const QByteArray& identifier) { return !identifier.isEmpty(); }),
            "time zone enumeration must not return empty identifiers");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    std::cout << "Sorting activation keys" << std::endl;
    stringListsPreserveSortAndDeduplication();
    std::cout << "Flushing singleton INI settings" << std::endl;
    singletonIniSettingsPersistAndReload();
    std::cout << "Enumerating time zones" << std::endl;
    timeZoneEnumerationReturnsValidIdentifiers();
    return 0;
}
