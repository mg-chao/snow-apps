#ifndef SNOW_SHOT_PRESENTATION_TRANSLATIONLANGUAGES_H
#define SNOW_SHOT_PRESENTATION_TRANSLATIONLANGUAGES_H
#include "snow_shot/translation/translationlanguages.h"

// Compatibility names for presentation consumers; all policy lives in the translation core.
namespace snow_shot::presentation {
using translation::defaultTranslationTargetLanguage;
using translation::TranslationLanguage;
using translation::translationLanguageName;
using translation::translationLanguages;
using translation::translationModelIndex;
} // namespace snow_shot::presentation
#endif
