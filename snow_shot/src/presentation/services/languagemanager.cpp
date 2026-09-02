#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/storage/settingsadapters.h"

#include "locale/locale.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <QDebug>

#include <algorithm>
#include <optional>

namespace {
constexpr auto SYSTEM_LANGUAGE_PREFERENCE = "system";
constexpr auto SOURCE_LANGUAGE_LOCALE = "en_US";
constexpr auto CATALOG_RESOURCE_PREFIX = ":/i18n/";
constexpr const char* LANGUAGE_NAME_SOURCE = QT_TRANSLATE_NOOP("LanguageCatalog", "Language name");

struct ParsedLocale {
    QString name;
    QLocale locale;
};

std::optional<ParsedLocale> parseLocaleName(const QString& rawName) {
    QString languageTag = rawName.trimmed();
    if (languageTag.isEmpty()) {
        return std::nullopt;
    }

    languageTag.replace(u'-', u'_');
    const QLocale locale(languageTag);
    if (locale.language() == QLocale::AnyLanguage || locale.name() == QStringLiteral("C")) {
        return std::nullopt;
    }

    return ParsedLocale{locale.name(), locale};
}

QString canonicalPreference(const QString& rawPreference) {
    const QString trimmed = rawPreference.trimmed();
    if (trimmed.compare(QString::fromLatin1(SYSTEM_LANGUAGE_PREFERENCE), Qt::CaseInsensitive) ==
        0) {
        return QString::fromLatin1(SYSTEM_LANGUAGE_PREFERENCE);
    }
    if (trimmed.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0) {
        return QString::fromLatin1(SOURCE_LANGUAGE_LOCALE);
    }

    if (const auto parsed = parseLocaleName(trimmed); parsed.has_value()) {
        return parsed->name;
    }
    return {};
}

bool localeMatchesScript(const QLocale& first, const QLocale& second) {
    return first.language() == second.language() && first.script() != QLocale::AnyScript &&
           first.script() == second.script();
}

bool localeMatchesLanguage(const QLocale& first, const QLocale& second) {
    return first.language() == second.language();
}
} // namespace

namespace snow_shot::presentation {
LanguageManager::LanguageManager(QObject* parent) : QObject(parent) {}

LanguageManager& LanguageManager::instance() {
    static LanguageManager manager;
    return manager;
}

void LanguageManager::initialize() {
    if (m_initialized) {
        return;
    }

    discoverCatalogs();
    m_initialized = true;

    snow_shot::storage::InterfaceSettings settings;
    const QString savedValue = settings.language();
    QString initialPreference = canonicalPreference(savedValue);
    if (initialPreference.isEmpty() ||
        (initialPreference != QString::fromLatin1(SYSTEM_LANGUAGE_PREFERENCE) &&
         !m_catalogResources.contains(initialPreference))) {
        initialPreference = QString::fromLatin1(SYSTEM_LANGUAGE_PREFERENCE);
    }

    if (applyPreference(initialPreference, false)) {
        if (savedValue.trimmed() != initialPreference) {
            settings.setLanguage(initialPreference);
        }
    }
}

QList<LanguageCatalog> LanguageManager::availableLanguages() const {
    return m_availableLanguages;
}

QString LanguageManager::languagePreference() const {
    return m_languagePreference;
}

QLocale LanguageManager::currentLocale() const {
    return m_currentLocale;
}

bool LanguageManager::setLanguage(const QString& languagePreference) {
    if (!m_initialized) {
        initialize();
    }

    const QString normalizedPreference = canonicalPreference(languagePreference);
    if (normalizedPreference.isEmpty() ||
        (normalizedPreference != QString::fromLatin1(SYSTEM_LANGUAGE_PREFERENCE) &&
         !m_catalogResources.contains(normalizedPreference))) {
        emit languageChangeFailed(languagePreference);
        return false;
    }

    if (normalizedPreference == m_languagePreference && m_translator != nullptr) {
        snow_shot::storage::InterfaceSettings().setLanguage(normalizedPreference);
        return true;
    }

    return applyPreference(normalizedPreference, true);
}

void LanguageManager::discoverCatalogs() {
    m_availableLanguages.clear();
    m_catalogResources.clear();
    m_catalogLocales.clear();

    const QDir resourceDirectory(QStringLiteral(":/i18n"));
    const QStringList resourceNames =
        resourceDirectory.entryList({QStringLiteral("snow_shot_*.qm")}, QDir::Files, QDir::Name);

    for (const QString& resourceName : resourceNames) {
        const QString prefix = QStringLiteral("snow_shot_");
        const QString suffix = QStringLiteral(".qm");
        if (!resourceName.startsWith(prefix) || !resourceName.endsWith(suffix)) {
            continue;
        }

        const QString fileLocaleName =
            resourceName.mid(prefix.size(), resourceName.size() - prefix.size() - suffix.size());
        const auto fileLocale = parseLocaleName(fileLocaleName);
        if (!fileLocale.has_value()) {
            qWarning() << "Ignoring Snow Shot translation with malformed locale" << resourceName;
            continue;
        }

        const QString resourcePath = QString::fromLatin1(CATALOG_RESOURCE_PREFIX) + resourceName;
        auto translator = std::make_unique<QTranslator>();
        if (!translator->load(resourcePath)) {
            qWarning() << "Ignoring Snow Shot translation that could not be loaded" << resourcePath;
            continue;
        }

        const auto translatorLocale = parseLocaleName(translator->language());
        if (!translatorLocale.has_value() || translatorLocale->name != fileLocale->name) {
            qWarning() << "Ignoring Snow Shot translation whose locale does not match its"
                          " resource name"
                       << resourceName << translator->language();
            continue;
        }

        if (m_catalogResources.contains(translatorLocale->name)) {
            qWarning() << "Ignoring duplicate Snow Shot translation locale"
                       << translatorLocale->name;
            continue;
        }

        const QString nativeName =
            translator->translate("LanguageCatalog", LANGUAGE_NAME_SOURCE).trimmed();
        if (nativeName.isEmpty()) {
            qWarning() << "Ignoring Snow Shot translation without a native language name"
                       << resourceName;
            continue;
        }

        m_catalogResources.insert(translatorLocale->name, resourcePath);
        m_catalogLocales.insert(translatorLocale->name, translatorLocale->locale);
        m_availableLanguages.push_back({translatorLocale->name, nativeName});
    }

    std::sort(m_availableLanguages.begin(), m_availableLanguages.end(),
              [](const LanguageCatalog& first, const LanguageCatalog& second) {
                  const bool firstIsSource =
                      first.localeName == QString::fromLatin1(SOURCE_LANGUAGE_LOCALE);
                  const bool secondIsSource =
                      second.localeName == QString::fromLatin1(SOURCE_LANGUAGE_LOCALE);
                  if (firstIsSource != secondIsSource) {
                      return firstIsSource;
                  }
                  return first.localeName < second.localeName;
              });
}

bool LanguageManager::applyPreference(const QString& preference, bool persist) {
    QString effectiveLocaleName = preference;
    if (preference == QString::fromLatin1(SYSTEM_LANGUAGE_PREFERENCE)) {
        const auto systemUiLanguages = QLocale::system().uiLanguages();
        QList<QLocale> systemLocales;
        for (const QString& languageTag : systemUiLanguages) {
            if (const auto parsed = parseLocaleName(languageTag); parsed.has_value()) {
                systemLocales.push_back(parsed->locale);
            }
        }
        if (systemLocales.isEmpty()) {
            systemLocales.push_back(QLocale::system());
        }

        const auto findMatchingCatalog = [this, &systemLocales](auto predicate) -> QString {
            for (const QLocale& systemLocale : systemLocales) {
                for (const LanguageCatalog& catalog : m_availableLanguages) {
                    const QLocale catalogLocale = m_catalogLocales.value(catalog.localeName);
                    if (predicate(systemLocale, catalogLocale)) {
                        return catalog.localeName;
                    }
                }
            }
            return {};
        };

        effectiveLocaleName =
            findMatchingCatalog([](const QLocale& systemLocale, const QLocale& catalogLocale) {
                return systemLocale.name() == catalogLocale.name();
            });
        if (effectiveLocaleName.isEmpty()) {
            effectiveLocaleName = findMatchingCatalog(localeMatchesScript);
        }
        if (effectiveLocaleName.isEmpty()) {
            effectiveLocaleName = findMatchingCatalog(localeMatchesLanguage);
        }
        if (effectiveLocaleName.isEmpty()) {
            effectiveLocaleName = QString::fromLatin1(SOURCE_LANGUAGE_LOCALE);
        }
    }

    const QString resource = m_catalogResources.value(effectiveLocaleName);
    const QLocale effectiveLocale = m_catalogLocales.value(effectiveLocaleName);
    if (resource.isEmpty() || effectiveLocale.language() == QLocale::AnyLanguage) {
        qWarning() << "Unable to resolve Snow Shot translation preference" << preference;
        emit languageChangeFailed(preference);
        return false;
    }

    auto nextTranslator = std::make_unique<QTranslator>();
    if (!nextTranslator->load(resource)) {
        qWarning() << "Unable to load Snow Shot translation resource" << resource;
        emit languageChangeFailed(preference);
        return false;
    }

    m_languagePreference = preference;
    m_currentLocale = effectiveLocale;

    QTranslator* const previousTranslator = m_translator.get();
    QCoreApplication::installTranslator(nextTranslator.get());
    if (previousTranslator != nullptr) {
        QCoreApplication::removeTranslator(previousTranslator);
    }

    m_translator = std::move(nextTranslator);

    adqt::locale::LocaleManager::instance().setLocale(effectiveLocale);

    if (persist) {
        snow_shot::storage::InterfaceSettings().setLanguage(preference);
    }

    emit languageChanged(m_languagePreference, m_currentLocale);
    return true;
}
} // namespace snow_shot::presentation
