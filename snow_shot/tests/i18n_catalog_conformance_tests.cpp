#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>

namespace {
struct MessageKey {
    QString context;
    QString source;
    QString comment;
    bool numerus = false;

    friend bool operator==(const MessageKey& first, const MessageKey& second) {
        return first.context == second.context && first.source == second.source &&
               first.comment == second.comment && first.numerus == second.numerus;
    }
};

size_t qHash(const MessageKey& key, size_t seed = 0) {
    seed = qHash(key.context, seed);
    seed = qHash(key.source, seed);
    seed = qHash(key.comment, seed);
    return ::qHash(static_cast<quint8>(key.numerus), seed);
}

struct Message {
    MessageKey key;
    QStringList translations;
};

struct Catalog {
    QString path;
    QString locale;
    QString sourceLanguage;
    QList<Message> messages;
};

struct CheckResult {
    bool ok = true;
    QString error;
};

void fail(CheckResult& result, const QString& message) {
    if (result.ok) {
        result.ok = false;
        result.error = message;
    }
}

QString canonicalLocale(const QString& rawLocale) {
    QString normalized = rawLocale.trimmed();
    normalized.replace(u'-', u'_');
    const QLocale locale(normalized);
    if (locale.language() == QLocale::AnyLanguage || locale.name() == QStringLiteral("C")) {
        return {};
    }
    return locale.name();
}

QStringList placeholders(const QString& text) {
    QStringList result;
    const QRegularExpression expression(QStringLiteral("%(?:[1-9][0-9]*|n)"));
    auto match = expression.globalMatch(text);
    while (match.hasNext()) {
        result.push_back(match.next().captured(0));
    }
    std::sort(result.begin(), result.end());
    return result;
}

CheckResult readCatalog(const QString& path, Catalog& catalog, bool rejectUnfinished = true) {
    CheckResult result;
    catalog.path = path;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        fail(result, QStringLiteral("unable to open %1").arg(path));
        return result;
    }

    QXmlStreamReader reader(&file);
    QString context;
    std::optional<Message> message;
    QStringList translationForms;
    enum class TextElement : std::uint8_t {
        None,
        ContextName,
        Source,
        Comment,
        Translation,
        Numerusform,
    } textElement = TextElement::None;
    QString text;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            if (name == QStringLiteral("TS")) {
                catalog.locale = reader.attributes().value(QStringLiteral("language")).toString();
                catalog.sourceLanguage =
                    reader.attributes().value(QStringLiteral("sourcelanguage")).toString();
            } else if (name == QStringLiteral("context")) {
                context.clear();
            } else if (name == QStringLiteral("name") && message.has_value() == false) {
                textElement = TextElement::ContextName;
                text.clear();
            } else if (name == QStringLiteral("message")) {
                message.emplace();
                message->key.context = context;
                message->key.numerus =
                    reader.attributes().value(QStringLiteral("numerus")) == QStringLiteral("yes");
                translationForms.clear();
            } else if (name == QStringLiteral("translation")) {
                if (!message.has_value()) {
                    fail(result, QStringLiteral("translation outside message in %1").arg(path));
                    break;
                }
                textElement = TextElement::Translation;
                text.clear();
                if (rejectUnfinished && reader.attributes().value(QStringLiteral("type")) ==
                                            QStringLiteral("unfinished")) {
                    fail(result, QStringLiteral("unfinished translation in %1").arg(path));
                    break;
                }
            } else if (name == QStringLiteral("numerusform")) {
                textElement = TextElement::Numerusform;
                text.clear();
            } else if (name == QStringLiteral("source") && message.has_value()) {
                textElement = TextElement::Source;
                text.clear();
            } else if (name == QStringLiteral("comment") && message.has_value()) {
                textElement = TextElement::Comment;
                text.clear();
            }
        } else if (reader.isCharacters()) {
            if (textElement != TextElement::None) {
                text += reader.text().toString();
            }
        } else if (reader.isEndElement()) {
            const QString name = reader.name().toString();
            if (name == QStringLiteral("name") && textElement == TextElement::ContextName) {
                context = text;
                text.clear();
                textElement = TextElement::None;
            } else if (name == QStringLiteral("source") && textElement == TextElement::Source &&
                       message.has_value()) {
                message->key.source = text;
                text.clear();
                textElement = TextElement::None;
            } else if (name == QStringLiteral("comment") && textElement == TextElement::Comment &&
                       message.has_value()) {
                message->key.comment = text;
                text.clear();
                textElement = TextElement::None;
            } else if (name == QStringLiteral("numerusform") &&
                       textElement == TextElement::Numerusform) {
                translationForms.push_back(text);
                text.clear();
                textElement = TextElement::Translation;
            } else if (name == QStringLiteral("translation")) {
                if (message.has_value()) {
                    if (!translationForms.isEmpty()) {
                        message->translations = translationForms;
                    } else {
                        message->translations = {text};
                    }
                }
                textElement = TextElement::None;
                text.clear();
            } else if (name == QStringLiteral("message")) {
                if (message.has_value()) {
                    catalog.messages.push_back(*message);
                }
                message.reset();
            }
        }
    }

    if (reader.hasError()) {
        fail(result, QStringLiteral("XML error in %1: %2").arg(path, reader.errorString()));
    }
    return result;
}

QSet<MessageKey> keys(const Catalog& catalog) {
    QSet<MessageKey> result;
    for (const Message& message : catalog.messages) {
        result.insert(message.key);
    }
    return result;
}

QString describe(const MessageKey& key) {
    return QStringLiteral("%1 / %2").arg(key.context, key.source);
}

CheckResult validateExtractedCatalog(const Catalog& catalog) {
    CheckResult result;
    QSet<MessageKey> seen;
    bool hasGeneratedSettingsText = false;
    QSet<MessageKey> requiredDynamicMessages{
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Vertical scroll: mouse wheel"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Horizontal scroll: Shift + mouse wheel"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Switch element level: mouse wheel"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Switch Color Format: Shift"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Switch Screenshot History"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Maintain aspect ratio: Shift"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Fixed-angle rotation: Shift"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Scale from center: Alt"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Auto-align: Ctrl"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Delete selected elements: Delete"), {}, false},
        {QStringLiteral("ScreenshotShortcutHintsWidget"),
         QStringLiteral("Draw straight line: Shift"), {}, false},
        {QStringLiteral("ScreenshotPinnedWindow"),
         QStringLiteral("Image size is too large."), {}, false},
        {QStringLiteral("ScreenshotPinnedWindow"),
         QStringLiteral("The pinned image could not be prepared"), {}, false},
        {QStringLiteral("ScreenshotPinnedWindow"),
         QStringLiteral("The pinned image copy could not be started"), {}, false},
        {QStringLiteral("ScreenshotPinnedWindow"),
         QStringLiteral("The pinned image save could not be started"), {}, false},
    };
    for (const Message& message : catalog.messages) {
        if (message.key.context.isEmpty() || message.key.source.isEmpty()) {
            fail(result, QStringLiteral("lupdate produced an empty message key"));
        }
        if (seen.contains(message.key)) {
            fail(result, QStringLiteral("lupdate produced a duplicate message key: %1")
                             .arg(describe(message.key)));
        }
        seen.insert(message.key);
        requiredDynamicMessages.remove(message.key);
        hasGeneratedSettingsText =
            hasGeneratedSettingsText ||
            (message.key.context == QStringLiteral("SettingsCatalog") &&
             message.key.source == QStringLiteral("Theme"));
    }
    if (!hasGeneratedSettingsText) {
        fail(result,
             QStringLiteral("lupdate did not extract configuration-defined settings text"));
    }
    if (!requiredDynamicMessages.isEmpty()) {
        fail(result, QStringLiteral("lupdate did not extract dynamic UI text: %1")
                         .arg(describe(*requiredDynamicMessages.cbegin())));
    }
    return result;
}

CheckResult validateCatalog(const Catalog& catalog, const QString& expectedSourceLanguage) {
    CheckResult result;
    const QString filenameLocale =
        QFileInfo(catalog.path).completeBaseName().mid(QStringLiteral("snow_shot_").size());
    const QString canonicalFilenameLocale = canonicalLocale(filenameLocale);
    const QString canonicalMetadataLocale = canonicalLocale(catalog.locale);
    if (canonicalFilenameLocale.isEmpty() || canonicalFilenameLocale != filenameLocale) {
        fail(result, QStringLiteral("invalid catalog filename locale: %1").arg(catalog.path));
    }
    if (canonicalMetadataLocale.isEmpty() || canonicalMetadataLocale != filenameLocale) {
        fail(result,
             QStringLiteral("catalog language metadata does not match %1").arg(catalog.path));
    }
    if (catalog.sourceLanguage != expectedSourceLanguage) {
        fail(result, QStringLiteral("catalog source language is not %1: %2")
                         .arg(expectedSourceLanguage, catalog.path));
    }

    QSet<MessageKey> seen;
    bool hasNativeName = false;
    for (const Message& message : catalog.messages) {
        if (message.key.context.isEmpty() || message.key.source.isEmpty()) {
            fail(result,
                 QStringLiteral("catalog contains an empty message key: %1").arg(catalog.path));
        }
        if (seen.contains(message.key)) {
            fail(result, QStringLiteral("duplicate message key in %1: %2")
                             .arg(catalog.path, describe(message.key)));
        }
        seen.insert(message.key);

        if (message.key.context == QStringLiteral("LanguageCatalog") &&
            message.key.source == QStringLiteral("Language name")) {
            hasNativeName = true;
        }
        if (message.translations.isEmpty()) {
            fail(result, QStringLiteral("empty translation in %1: %2")
                             .arg(catalog.path, describe(message.key)));
        }
        for (const QString& translation : message.translations) {
            if (translation.trimmed().isEmpty()) {
                fail(result, QStringLiteral("empty translation in %1: %2")
                                 .arg(catalog.path, describe(message.key)));
            }
            if (placeholders(translation) != placeholders(message.key.source)) {
                fail(result, QStringLiteral("placeholder mismatch in %1: %2")
                                 .arg(catalog.path, describe(message.key)));
            }
        }
    }

    if (!hasNativeName) {
        fail(result,
             QStringLiteral("missing LanguageCatalog native-name message: %1").arg(catalog.path));
    }
    return result;
}

CheckResult compareKeys(const Catalog& extracted, const Catalog& checkedIn) {
    CheckResult result;
    const QSet<MessageKey> expected = keys(extracted);
    const QSet<MessageKey> actual = keys(checkedIn);

    for (const MessageKey& key : expected) {
        if (!actual.contains(key)) {
            fail(result,
                 QStringLiteral("missing message in %1: %2").arg(checkedIn.path, describe(key)));
        }
    }
    for (const MessageKey& key : actual) {
        if (!expected.contains(key)) {
            fail(result,
                 QStringLiteral("extra message in %1: %2").arg(checkedIn.path, describe(key)));
        }
    }
    return result;
}

int run(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: i18n_catalog_conformance_tests <source-dir> <lupdate> <ts...>\n";
        return 2;
    }

    const QString sourceDirectory = QString::fromLocal8Bit(argv[1]);
    const QString lupdate = QString::fromLocal8Bit(argv[2]);
    QStringList checkedInPaths;
    for (int index = 3; index < argc; ++index) {
        checkedInPaths.push_back(QString::fromLocal8Bit(argv[index]));
    }

    const QString generatedPath =
        QCoreApplication::applicationDirPath() +
        QStringLiteral("/snow_shot_extracted_%1.ts").arg(QCoreApplication::applicationPid());
    QProcess process;
    process.setWorkingDirectory(sourceDirectory);
    process.start(lupdate, {
                               QStringLiteral("src"),
                               QStringLiteral("include"),
                               QStringLiteral("-no-obsolete"),
                               QStringLiteral("-locations"),
                               QStringLiteral("none"),
                               QStringLiteral("-source-language"),
                               QStringLiteral("en_US"),
                               QStringLiteral("-ts"),
                               generatedPath,
                           });
    if (!process.waitForFinished(120000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        std::cerr << "lupdate failed: " << process.errorString().toLocal8Bit().constData() << "\n"
                  << process.readAllStandardError().constData();
        return 1;
    }

    Catalog extracted;
    CheckResult result = readCatalog(generatedPath, extracted, false);
    if (!result.ok) {
        std::cerr << result.error.toLocal8Bit().constData() << '\n';
        QFile::remove(generatedPath);
        return 1;
    }

    const QString sourceLanguage = QStringLiteral("en_US");
    result = validateExtractedCatalog(extracted);
    if (!result.ok) {
        std::cerr << result.error.toLocal8Bit().constData() << '\n';
        QFile::remove(generatedPath);
        return 1;
    }

    QHash<QString, QString> localePaths;
    bool hasEnglish = false;
    for (const QString& path : checkedInPaths) {
        Catalog catalog;
        result = readCatalog(path, catalog);
        if (!result.ok) {
            std::cerr << result.error.toLocal8Bit().constData() << '\n';
            QFile::remove(generatedPath);
            return 1;
        }
        result = validateCatalog(catalog, sourceLanguage);
        if (!result.ok) {
            std::cerr << result.error.toLocal8Bit().constData() << '\n';
            QFile::remove(generatedPath);
            return 1;
        }
        const QString locale = canonicalLocale(catalog.locale);
        if (localePaths.contains(locale)) {
            std::cerr << "duplicate catalog locale: " << locale.toLocal8Bit().constData() << '\n';
            QFile::remove(generatedPath);
            return 1;
        }
        localePaths.insert(locale, path);
        hasEnglish = hasEnglish || locale == sourceLanguage;

        result = compareKeys(extracted, catalog);
        if (!result.ok) {
            std::cerr << result.error.toLocal8Bit().constData() << '\n';
            QFile::remove(generatedPath);
            return 1;
        }
    }

    if (!hasEnglish) {
        std::cerr << "complete en_US catalog is missing\n";
        QFile::remove(generatedPath);
        return 1;
    }

    QFile::remove(generatedPath);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    return run(argc, argv);
}
