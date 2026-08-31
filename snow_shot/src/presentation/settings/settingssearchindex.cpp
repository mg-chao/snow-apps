#include "snow_shot/presentation/settings/settingssearchindex.h"

#include "snow_shot/presentation/languagemanager.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>
#include <numeric>

namespace snow_shot::presentation::settings {
namespace {
constexpr const char* PAGES_SOURCE = QT_TRANSLATE_NOOP("SettingsCatalog", "Pages");

QString normalized(QString value) {
    value = value.normalized(QString::NormalizationForm_KC).toCaseFolded().trimmed();
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return value;
}

QStringList tokens(const QString& query) {
    return normalized(query).split(u' ', Qt::SkipEmptyParts);
}

int fieldScore(const QString& token, const QString& field, int exact, int prefix,
               int contains) {
    if (field.isEmpty()) {
        return 0;
    }
    if (field == token) {
        return exact;
    }
    if (field.startsWith(token)) {
        return prefix;
    }
    return field.contains(token) ? contains : 0;
}

int entryScore(const SettingsSearchIndex::NormalizedFields& fields,
               const QStringList& queryTokens,
               const QString& normalizedQuery) {
    if (queryTokens.isEmpty()) {
        return 0;
    }

    int total = fields.title == normalizedQuery ? 2000 : 0;
    for (const QString& token : queryTokens) {
        int score = fieldScore(token, fields.title, 1000, 800, 600);
        for (const QString& alias : fields.aliases) {
            score = std::max(score, fieldScore(token, alias, 550, 500, 450));
        }
        for (const QString& optionLabel : fields.optionLabels) {
            score = std::max(score, fieldScore(token, optionLabel, 500, 450, 400));
        }
        score = std::max(score, fieldScore(token, fields.path, 400, 350, 300));
        score = std::max(score, fieldScore(token, fields.description, 150, 125, 100));
        if (score == 0) {
            return -1;
        }
        total += score;
    }
    return total;
}

QStringList normalizedList(const QStringList& values) {
    QStringList result;
    result.reserve(values.size());
    for (const QString& value : values) {
        result.push_back(normalized(value));
    }
    return result;
}

void addPosting(QHash<QString, QVector<int>>* postings, const QString& token, int index) {
    if (postings == nullptr || token.isEmpty()) {
        return;
    }
    auto& values = (*postings)[token];
    if (values.isEmpty() || values.constLast() != index) {
        values.push_back(index);
    }
}

QVector<int> sortedUnion(const QVector<int>& first, const QVector<int>& second) {
    QVector<int> result;
    result.reserve(first.size() + second.size());
    std::set_union(first.cbegin(), first.cend(), second.cbegin(), second.cend(),
                   std::back_inserter(result));
    return result;
}

void indexField(QHash<QString, QVector<int>>* postings,
                QHash<QString, QVector<int>>* trigramPostings,
                const QString& field, int index) {
    const QStringList words = field.split(u' ', Qt::SkipEmptyParts);
    for (const QString& word : words) {
        if (word.isEmpty()) {
            continue;
        }
        // Complete words and short prefixes cover the common exact/prefix
        // path without the quadratic memory cost of indexing every substring.
        addPosting(postings, word, index);
        const int prefixLength = std::min(4, static_cast<int>(word.size()));
        for (int length = 1; length <= prefixLength; ++length) {
            addPosting(postings, word.left(length), index);
        }
    }
    if (trigramPostings != nullptr && field.size() >= 3) {
        // Index the complete normalized field, rather than each whitespace
        // separated word. This makes trigram candidates complete for
        // QString::contains(), including matches that cross punctuation.
        for (int offset = 0; offset + 3 <= field.size(); ++offset) {
            addPosting(trigramPostings, field.sliced(offset, 3), index);
        }
    }
}

QStringList translatedAliases(const QVector<TranslatableText>& aliases) {
    QStringList result;
    result.reserve(aliases.size());
    for (const TranslatableText& alias : aliases) {
        if (alias.isValid()) {
            result.push_back(alias.translated());
        }
    }
    return result;
}

QStringList selectOptionLabels(const SettingsSelectDefinition& select) {
    QStringList result;
    result.reserve(select.options.size());
    for (const SettingsOptionDefinition& option : select.options) {
        result.push_back(option.label.translated());
    }
    if (select.source == SettingsSelectSource::LanguageCatalog) {
        for (const LanguageCatalog& catalog : LanguageManager::instance().availableLanguages()) {
            result.push_back(catalog.nativeName);
        }
    }
    return result;
}

QStringList radioOptionLabels(const SettingsRadioDefinition& radio) {
    QStringList result;
    result.reserve(radio.options.size());
    for (const SettingsRadioOptionDefinition& option : radio.options) {
        result.push_back(option.label.translated());
    }
    return result;
}

QString itemTitle(const SettingsItemDefinition& item,
                  const SettingsSearchRuntimeValues& runtimeValues) {
    QString title = item.title.translated();
    const auto* shortcut = std::get_if<SettingsShortcutActionDefinition>(&item.payload);
    if (shortcut != nullptr &&
        shortcut->adjustment == SettingsShortcutAdjustment::ScreenshotDelaySeconds) {
        title = title.arg(runtimeValues.screenshotDelaySeconds);
    }
    return title;
}
} // namespace

SettingsSearchIndex::SettingsSearchIndex(const SettingsRegistry& registry,
                                         SettingsSearchRuntimeValues runtimeValues)
    : m_registry(registry), m_runtimeValues(runtimeValues) {
    rebuild();
}

void SettingsSearchIndex::rebuild() {
    m_entries.clear();
    m_normalizedEntries.clear();
    m_postings.clear();
    m_trigramPostings.clear();
    int order = 0;
    const QString pages = QCoreApplication::translate("SettingsCatalog", PAGES_SOURCE);
    for (const SettingsPageDefinition& page : m_registry.pages()) {
        m_entries.push_back({
            QStringLiteral("page:%1").arg(page.id),
            SettingsSearchNodeKind::Page,
            {page.id, {}, {}},
            page.title.translated(),
            page.description.translated(),
            pages,
            {},
            {},
            order++,
        });

        for (const SettingsSectionDefinition& section : page.sections) {
            m_entries.push_back({
                QStringLiteral("section:%1/%2").arg(page.id, section.id),
                SettingsSearchNodeKind::Section,
                {page.id, section.id, {}},
                section.title.translated(),
                section.searchDescription.translated(),
                page.title.translated(),
                {},
                {},
                order++,
            });

            const QString itemPath = QStringLiteral("%1 / %2")
                                         .arg(page.title.translated(),
                                              section.title.translated());
            for (const SettingsItemDefinition& item : section.items) {
                QStringList optionLabels;
                if (const auto* select = std::get_if<SettingsSelectDefinition>(&item.payload)) {
                    optionLabels = selectOptionLabels(*select);
                } else if (const auto* radio =
                               std::get_if<SettingsRadioDefinition>(&item.payload)) {
                    optionLabels = radioOptionLabels(*radio);
                }
                m_entries.push_back({
                    QStringLiteral("item:%1").arg(item.id),
                    SettingsSearchNodeKind::Item,
                    {page.id, section.id, item.id},
                    itemTitle(item, m_runtimeValues),
                    item.description.translated(),
                    itemPath,
                    translatedAliases(item.aliases),
                    optionLabels,
                    order++,
                });
            }
        }
    }
    m_normalizedEntries.reserve(m_entries.size());
    for (int index = 0; index < m_entries.size(); ++index) {
        const SettingsSearchEntry& entry = m_entries.at(index);
        m_normalizedEntries.push_back({
            normalized(entry.title),
            normalized(entry.description),
            normalized(entry.path),
            normalizedList(entry.aliases),
            normalizedList(entry.optionLabels),
        });
        const NormalizedFields& fields = m_normalizedEntries.constLast();
        indexField(&m_postings, &m_trigramPostings, fields.title, index);
        indexField(&m_postings, &m_trigramPostings, fields.description, index);
        indexField(&m_postings, &m_trigramPostings, fields.path, index);
        for (const QString& alias : fields.aliases) {
            indexField(&m_postings, &m_trigramPostings, alias, index);
        }
        for (const QString& option : fields.optionLabels) {
            indexField(&m_postings, &m_trigramPostings, option, index);
        }
    }
}

void SettingsSearchIndex::setRuntimeValues(SettingsSearchRuntimeValues runtimeValues) {
    if (m_runtimeValues.screenshotDelaySeconds == runtimeValues.screenshotDelaySeconds) {
        return;
    }
    m_runtimeValues = runtimeValues;
    rebuild();
}

const QVector<SettingsSearchEntry>& SettingsSearchIndex::entries() const {
    return m_entries;
}

QVector<SettingsSearchEntry> SettingsSearchIndex::search(const QString& query) const {
    const QString normalizedQuery = normalized(query);
    const QStringList queryTokens = tokens(query);
    if (queryTokens.isEmpty()) {
        return m_entries;
    }

    struct RankedEntry {
        SettingsSearchEntry entry;
        int score = 0;
    };
    QVector<int> candidates;
    bool initialized = false;
    for (const QString& token : queryTokens) {
        QVector<int> tokenCandidates;
        if (token.size() <= 2) {
            // Tiny contains queries are too broad to index profitably.
            tokenCandidates.resize(m_entries.size());
            std::iota(tokenCandidates.begin(), tokenCandidates.end(), 0);
        } else {
            // Exact words and short prefixes are the common path and are
            // already indexed directly. This avoids touching trigram buckets
            // for the majority of settings searches.
            const auto posting = m_postings.constFind(token);
            if (posting != m_postings.cend()) {
                tokenCandidates = posting.value();
            }
            if (token.size() >= 3) {
                // Use trigrams for longer contains queries, or to recover a
                // candidate set when the exact/prefix index has no entry.
                QVector<int> trigramCandidates;
                bool hasTrigramCandidates = false;
                for (int offset = 0; offset + 3 <= token.size(); ++offset) {
                    const auto trigramFound =
                        m_trigramPostings.constFind(token.sliced(offset, 3));
                    if (trigramFound == m_trigramPostings.cend()) {
                        hasTrigramCandidates = false;
                        break;
                    }
                    if (!hasTrigramCandidates) {
                        trigramCandidates = trigramFound.value();
                        hasTrigramCandidates = true;
                    } else {
                        QVector<int> intersection;
                        intersection.reserve(std::min(trigramCandidates.size(),
                                                      trigramFound.value().size()));
                        std::set_intersection(trigramCandidates.cbegin(),
                                              trigramCandidates.cend(),
                                              trigramFound.value().cbegin(),
                                              trigramFound.value().cend(),
                                              std::back_inserter(intersection));
                        trigramCandidates = std::move(intersection);
                    }
                    if (hasTrigramCandidates && trigramCandidates.isEmpty()) {
                        break;
                    }
                }
                if (hasTrigramCandidates) {
                    // Full-field trigrams are a complete superset for
                    // contains(token). Union short-word postings so fields
                    // shorter than three characters remain searchable.
                    tokenCandidates = sortedUnion(tokenCandidates, trigramCandidates);
                }
            }
            // A missing posting is not a definitive miss: a token may be a
            // rare contains match or include punctuation. Fall back to the
            // weighted scan in that case.
            if (tokenCandidates.isEmpty()) {
                tokenCandidates.resize(m_entries.size());
                std::iota(tokenCandidates.begin(), tokenCandidates.end(), 0);
            }
        }
        if (!initialized) {
            candidates = std::move(tokenCandidates);
            initialized = true;
            continue;
        }
        QVector<int> intersection;
        intersection.reserve(std::min(candidates.size(), tokenCandidates.size()));
        std::set_intersection(candidates.cbegin(), candidates.cend(), tokenCandidates.cbegin(),
                              tokenCandidates.cend(), std::back_inserter(intersection));
        candidates = std::move(intersection);
        if (candidates.isEmpty()) {
            return {};
        }
    }
    QVector<RankedEntry> ranked;
    for (const int index : candidates) {
        const SettingsSearchEntry& entry = m_entries.at(index);
        const int score = entryScore(m_normalizedEntries.at(index), queryTokens,
                                     normalizedQuery);
        if (score >= 0) {
            ranked.push_back({entry, score});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const RankedEntry& first,
                                               const RankedEntry& second) {
        if (first.score != second.score) {
            return first.score > second.score;
        }
        if (first.entry.catalogOrder != second.entry.catalogOrder) {
            return first.entry.catalogOrder < second.entry.catalogOrder;
        }
        return first.entry.id < second.entry.id;
    });

    QVector<SettingsSearchEntry> result;
    result.reserve(ranked.size());
    for (const RankedEntry& entry : ranked) {
        result.push_back(entry.entry);
    }
    return result;
}

} // namespace snow_shot::presentation::settings
