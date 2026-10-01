#ifndef SNOW_SHOT_PRESENTATION_TRANSLATABLETEXT_H
#define SNOW_SHOT_PRESENTATION_TRANSLATABLETEXT_H

#include <QString>

namespace snow_shot::presentation::settings {

struct TranslatableText {
    const char* context = nullptr;
    const char* source = nullptr;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QString translated() const;
};

} // namespace snow_shot::presentation::settings

#endif // SNOW_SHOT_PRESENTATION_TRANSLATABLETEXT_H
