#pragma once

#include "snow_shot/app/edition.h"
#include <QJsonObject>
#include <QString>

namespace snow_shot::app::mcp {
inline bool editionMethodEnabled(const QString& method) {
    if (!edition::textTranslation &&
        (method == u"snow_shot_screenshot_translate" ||
         method == u"snow_shot_translation_catalog" || method == u"snow_shot_translation_start"))
        return false;
    if (!edition::apiConfiguration &&
        (method == u"snow_shot_models_list" || method == u"snow_shot_models_update" ||
         method == u"snow_shot_credentials_set"))
        return false;
    return true;
}

inline bool editionRecognitionKindEnabled(const QString& kind) {
    if (kind != u"text" && kind != u"qr" && kind != u"table" && kind != u"latex" &&
        kind != u"markdown" && kind != u"html")
        return true;
    return kind == u"text" || (kind == u"qr" && edition::qrRecognition) ||
           (kind == u"table" && edition::tableRecognition) ||
           (kind == u"latex" && edition::latexRecognition) ||
           ((kind == u"markdown" || kind == u"html") && edition::imageConversion);
}

inline bool editionRequestEnabled(const QString& method, const QJsonObject& params) {
    if (!editionMethodEnabled(method))
        return false;
    if (method == u"snow_shot_screenshot_recognize" || method == u"snow_shot_document_recognize")
        return editionRecognitionKindEnabled(params.value(QStringLiteral("kind")).toString());
    if (method == u"snow_shot_app_action" &&
        params.value(QStringLiteral("action")).toString() == u"show_translation")
        return edition::textTranslation;
    if (method == u"snow_shot_pinned_edit") {
        const auto action = params.value(QStringLiteral("action")).toString();
        if (action == u"translate")
            return edition::textTranslation;
        if (action == u"recognize")
            return editionRecognitionKindEnabled(params.value(QStringLiteral("payload"))
                                                     .toObject()
                                                     .value(QStringLiteral("kind"))
                                                     .toString());
    }
    if ((method == u"snow_shot_screenshot_edit_recognition" ||
         method == u"snow_shot_document_edit_recognition") &&
        !edition::tableRecognition) {
        const auto action = params.value(QStringLiteral("action")).toString();
        if (action == u"select_cells" || action == u"set_cell" || action == u"merge_cells" ||
            action == u"split_cells" || action == u"reset_table" || action == u"show_original")
            return false;
    }
    if ((method == u"snow_shot_screenshot_export_recognition" ||
         method == u"snow_shot_document_export_recognition") &&
        !edition::imageConversion) {
        const auto format = params.value(QStringLiteral("format")).toString();
        if (format == u"html" || format == u"markdown")
            return false;
    }
    return true;
}
} // namespace snow_shot::app::mcp
