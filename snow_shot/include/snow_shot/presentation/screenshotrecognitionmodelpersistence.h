#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONMODELPERSISTENCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONMODELPERSISTENCE_H

#include "snow_shot/presentation/screenshotrecognitionmodel.h"
#include "snow_shot/presentation/screenshotrecognitionresults.h"

#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#endif

namespace snow_shot::presentation {
inline constexpr quint32 kRecognitionModelPayloadMarker = 0x53524D44;
inline constexpr quint8 kRecognitionModelPayloadVersion = 1;
inline constexpr qsizetype kMaximumRecognitionModelPayload = 16 * 1024 * 1024;
inline constexpr qsizetype kMaximumRecognitionModelEntries = 128;

#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
[[nodiscard]] inline bool validRecognitionModelSelection(const QString& selection) {
    if (selection.isEmpty() || selection.size() > 256)
        return false;
    for (QChar character : selection) {
        if (character.isSpace() || character.category() == QChar::Other_Control)
            return false;
    }
    return true;
}

[[nodiscard]] inline qsizetype recognitionModelStringPayloadBytes(const QString& value) {
    qsizetype bytes = 0;
    for (QChar character : value) {
        const char16_t code = character.unicode();
        // This upper bound includes JSON escapes and UTF-8 without allocating a second source.
        bytes += code < 0x20 || (code >= 0xd800 && code <= 0xdfff) ? 6
                 : code == u'"' || code == u'\\'                   ? 2
                 : code < 0x80                                     ? 1
                                                                   : 3;
    }
    return bytes;
}

[[nodiscard]] inline QByteArray
encodeRecognitionModels(const ScreenshotRecognitionResults& results) {
    QJsonObject root;
    qsizetype estimatedBytes = 2048;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    QVector<ScreenshotTableRecognitionEntry> tables;
    QSet<QString> tableModels;
    auto tableEntries = results.tableEntries;
    std::sort(tableEntries.begin(), tableEntries.end(),
              [](const auto& first, const auto& second) { return first.model < second.model; });
    const bool validTableHint = validRecognitionModelSelection(results.tableEffectiveModel);
    const auto selectedTable = [&](const ScreenshotTableRecognitionEntry& entry) {
        return entry.model == results.tableModelSelection ||
               (validTableHint && entry.model == results.tableEffectiveModel);
    };
    const auto acceptTable = [&](const ScreenshotTableRecognitionEntry& entry) {
        if (tables.size() >= kMaximumRecognitionModelEntries || !entry.isValid() ||
            !validRecognitionModelSelection(entry.model) || tableModels.contains(entry.model))
            return;
        qsizetype entryBytes = 256 + recognitionModelStringPayloadBytes(entry.model) +
                               recognitionModelStringPayloadBytes(entry.modelFingerprint) +
                               recognitionModelStringPayloadBytes(entry.result.html) +
                               recognitionModelStringPayloadBytes(entry.result.code);
        if (entry.draftHtml)
            entryBytes += recognitionModelStringPayloadBytes(*entry.draftHtml);
        if (entryBytes > kMaximumRecognitionModelPayload - estimatedBytes)
            return;
        estimatedBytes += entryBytes;
        tableModels.insert(entry.model);
        tables.append(entry);
    };
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    QVector<ScreenshotLatexRecognitionEntry> latex;
    QSet<QString> latexModels;
    auto latexEntries = results.latexEntries;
    std::sort(latexEntries.begin(), latexEntries.end(),
              [](const auto& first, const auto& second) { return first.model < second.model; });
    const bool validLatexHint = validRecognitionModelSelection(results.latexEffectiveModel);
    const auto selectedLatex = [&](const ScreenshotLatexRecognitionEntry& entry) {
        return entry.model == results.latexModelSelection ||
               (validLatexHint && entry.model == results.latexEffectiveModel);
    };
    const auto acceptLatex = [&](const ScreenshotLatexRecognitionEntry& entry) {
        if (latex.size() >= kMaximumRecognitionModelEntries || !entry.isValid() ||
            !validRecognitionModelSelection(entry.model) || latexModels.contains(entry.model))
            return;
        qsizetype entryBytes = 256 + recognitionModelStringPayloadBytes(entry.model) +
                               recognitionModelStringPayloadBytes(entry.modelFingerprint) +
                               recognitionModelStringPayloadBytes(entry.result.latex) +
                               recognitionModelStringPayloadBytes(entry.result.code);
        if (entry.draft)
            entryBytes += recognitionModelStringPayloadBytes(*entry.draft);
        if (entryBytes > kMaximumRecognitionModelPayload - estimatedBytes)
            return;
        estimatedBytes += entryBytes;
        latexModels.insert(entry.model);
        latex.append(entry);
    };
#endif
    // Admit both selected results before other cached models, then serialize in canonical order.
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    for (const auto& entry : tableEntries) {
        if (selectedTable(entry))
            acceptTable(entry);
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    for (const auto& entry : latexEntries) {
        if (selectedLatex(entry))
            acceptLatex(entry);
    }
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    for (const auto& entry : tableEntries) {
        if (!selectedTable(entry))
            acceptTable(entry);
    }
    std::sort(tables.begin(), tables.end(),
              [](const auto& first, const auto& second) { return first.model < second.model; });
    QJsonArray tableArray;
    for (const auto& entry : tables) {
        QJsonObject item{{QStringLiteral("model"), entry.model},
                         {QStringLiteral("model_fingerprint"), entry.modelFingerprint},
                         {QStringLiteral("prompt_version"), entry.promptVersion},
                         {QStringLiteral("html"), entry.result.html},
                         {QStringLiteral("code"), entry.result.code},
                         {QStringLiteral("http_status"), entry.result.httpStatus}};
        if (entry.draftHtml)
            item.insert(QStringLiteral("draft_html"), *entry.draftHtml);
        tableArray.append(item);
    }
    if (!tableArray.isEmpty())
        root.insert(QStringLiteral("tables"), tableArray);
    if (validRecognitionModelSelection(results.tableModelSelection))
        root.insert(QStringLiteral("table_selection"), results.tableModelSelection);
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    for (const auto& entry : latexEntries) {
        if (!selectedLatex(entry))
            acceptLatex(entry);
    }
    std::sort(latex.begin(), latex.end(),
              [](const auto& first, const auto& second) { return first.model < second.model; });
    QJsonArray latexArray;
    for (const auto& entry : latex) {
        QJsonObject item{{QStringLiteral("model"), entry.model},
                         {QStringLiteral("model_fingerprint"), entry.modelFingerprint},
                         {QStringLiteral("prompt_version"), entry.promptVersion},
                         {QStringLiteral("latex"), entry.result.latex},
                         {QStringLiteral("code"), entry.result.code},
                         {QStringLiteral("http_status"), entry.result.httpStatus}};
        if (entry.draft)
            item.insert(QStringLiteral("draft"), *entry.draft);
        latexArray.append(item);
    }
    if (!latexArray.isEmpty())
        root.insert(QStringLiteral("latex"), latexArray);
    if (validRecognitionModelSelection(results.latexModelSelection))
        root.insert(QStringLiteral("latex_selection"), results.latexModelSelection);
#endif
    if (root.isEmpty())
        return {};
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (payload.size() <= kMaximumRecognitionModelPayload)
        return payload;
    root.remove(QStringLiteral("tables"));
    root.remove(QStringLiteral("latex"));
    return root.isEmpty() ? QByteArray{} : QJsonDocument(root).toJson(QJsonDocument::Compact);
}

inline void decodeRecognitionModels(const QByteArray& bytes,
                                    ScreenshotRecognitionResults& results) {
    if (bytes.size() > kMaximumRecognitionModelPayload)
        return;
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        return;
    const QJsonObject root = document.object();
    const auto selection = [&root](const QString& key) {
        const QString value = root.value(key).toString();
        return validRecognitionModelSelection(value) ? value : QString();
    };
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    QVector<ScreenshotTableRecognitionEntry> tables;
    QSet<QString> tableModels;
    const auto tableArray = root.value(QStringLiteral("tables")).toArray();
    if (tableArray.size() > kMaximumRecognitionModelEntries)
        return;
    for (const auto& value : tableArray) {
        const QJsonObject item = value.toObject();
        ScreenshotTableRecognitionEntry entry;
        entry.model = item.value(QStringLiteral("model")).toString();
        entry.modelFingerprint = item.value(QStringLiteral("model_fingerprint")).toString();
        entry.promptVersion = item.value(QStringLiteral("prompt_version")).toInt(-1);
        entry.result.html = item.value(QStringLiteral("html")).toString();
        entry.result.code = item.value(QStringLiteral("code")).toString();
        entry.result.httpStatus = item.value(QStringLiteral("http_status")).toInt();
        if (item.contains(QStringLiteral("draft_html"))) {
            if (!item.value(QStringLiteral("draft_html")).isString())
                continue;
            entry.draftHtml = item.value(QStringLiteral("draft_html")).toString();
        }
        if (entry.isValid() && validRecognitionModelSelection(entry.model) &&
            !tableModels.contains(entry.model)) {
            tableModels.insert(entry.model);
            tables.append(std::move(entry));
        }
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    QVector<ScreenshotLatexRecognitionEntry> latex;
    QSet<QString> latexModels;
    const auto latexArray = root.value(QStringLiteral("latex")).toArray();
    if (latexArray.size() > kMaximumRecognitionModelEntries)
        return;
    for (const auto& value : latexArray) {
        const QJsonObject item = value.toObject();
        ScreenshotLatexRecognitionEntry entry;
        entry.model = item.value(QStringLiteral("model")).toString();
        entry.modelFingerprint = item.value(QStringLiteral("model_fingerprint")).toString();
        entry.promptVersion = item.value(QStringLiteral("prompt_version")).toInt(-1);
        entry.result.latex = item.value(QStringLiteral("latex")).toString();
        entry.result.code = item.value(QStringLiteral("code")).toString();
        entry.result.httpStatus = item.value(QStringLiteral("http_status")).toInt();
        if (item.contains(QStringLiteral("draft"))) {
            if (!item.value(QStringLiteral("draft")).isString())
                continue;
            entry.draft = item.value(QStringLiteral("draft")).toString();
        }
        if (entry.isValid() && validRecognitionModelSelection(entry.model) &&
            !latexModels.contains(entry.model)) {
            latexModels.insert(entry.model);
            latex.append(std::move(entry));
        }
    }
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    results.tableEntries = std::move(tables);
    results.tableModelSelection = selection(QStringLiteral("table_selection"));
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    results.latexEntries = std::move(latex);
    results.latexModelSelection = selection(QStringLiteral("latex_selection"));
#endif
}
#endif

inline void promoteLegacyRecognitionModels(ScreenshotRecognitionResults& results) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (results.table && results.table->succeeded() &&
        std::none_of(results.tableEntries.cbegin(), results.tableEntries.cend(),
                     [](const auto& entry) {
                         return entry.model == screenshotDedicatedRecognitionModelId();
                     })) {
        results.tableEntries.append(
            {screenshotDedicatedRecognitionModelId(), {}, 1, *results.table, std::nullopt});
        if (results.tableModelSelection.isEmpty())
            results.tableModelSelection = screenshotDedicatedRecognitionModelId();
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (results.latex && results.latex->succeeded() &&
        std::none_of(results.latexEntries.cbegin(), results.latexEntries.cend(),
                     [](const auto& entry) {
                         return entry.model == screenshotDedicatedRecognitionModelId();
                     })) {
        results.latexEntries.append(
            {screenshotDedicatedRecognitionModelId(), {}, 1, *results.latex, results.latexDraft});
        if (results.latexModelSelection.isEmpty())
            results.latexModelSelection = screenshotDedicatedRecognitionModelId();
    }
#endif
#if !SNOW_SHOT_ENABLE_TABLE_RECOGNITION && !SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    Q_UNUSED(results)
#endif
}
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONMODELPERSISTENCE_H
