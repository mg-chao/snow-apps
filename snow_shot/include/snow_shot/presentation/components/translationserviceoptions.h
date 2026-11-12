#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_TRANSLATIONSERVICEOPTIONS_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_TRANSLATIONSERVICEOPTIONS_H

#include "snow_shot/translation/fasttranslation.h"
#include "snow_shot/translation/freetranslation.h"
#include "snow_shot/translation/translationlanguages.h"
#include "widgets/select.h"

namespace snow_shot::presentation {
inline QVector<adqt::widgets::AdSelect::Option>
translationServiceOptions(const QVector<SnowShotChatModel>& models) {
    QVector<adqt::widgets::AdSelect::Option> options;
    options.reserve(models.size());
    for (const auto& model : models) {
        QString label = model.name;
        if (model.id == translation::freeTranslationId)
            label = QCoreApplication::translate("snow_shot::translation::TranslationService",
                                                "Free translation");
        else if (model.id == translation::fastTranslationId)
            label = QCoreApplication::translate("snow_shot::translation::TranslationService",
                                                "Fast Translation");
        options.append({model.id, label, false, translation::translationModelGroup(model)});
    }
    return options;
}
} // namespace snow_shot::presentation

#endif
