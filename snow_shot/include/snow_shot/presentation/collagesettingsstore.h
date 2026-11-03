// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGESETTINGSSTORE_H
#define SNOW_SHOT_PRESENTATION_COLLAGESETTINGSSTORE_H

#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QStringList>

namespace snow_shot::presentation {

// Read once when a window opens. Persist only the edited field so an older window
// cannot overwrite more recent choices made in another independent window.
class CollageSettingsStore final {
  public:
    explicit CollageSettingsStore(storage::ConfigurationStore& configuration)
        : m_configuration(configuration) {}

    [[nodiscard]] CollageSettings read() const {
        CollageSettings result;
        const QString direction =
            m_configuration.value(QStringLiteral("collage/direction")).toString();
        if (direction == QStringLiteral("custom"))
            result.direction = CollageDirection::Custom;
        else if (direction == QStringLiteral("horizontal"))
            result.direction = CollageDirection::Horizontal;
        result.sizing = m_configuration.value(QStringLiteral("collage/sizing_mode")).toString() ==
                                QStringLiteral("original")
                            ? CollageSizing::Original
                            : CollageSizing::Uniform;
        result.border = m_configuration.value(QStringLiteral("collage/border_px")).toInt();
        result.spacing = m_configuration.value(QStringLiteral("collage/spacing_px")).toInt();
        result.rows = m_configuration.value(QStringLiteral("collage/rows")).toInt();
        result.columns = m_configuration.value(QStringLiteral("collage/columns")).toInt();
        result.customGridInitialized =
            m_configuration.value(QStringLiteral("collage/custom_grid_initialized")).toBool();
        result.background = storage::colorFromRgbaString(
            m_configuration.value(QStringLiteral("collage/background_color")).toString());
        if (!result.background.isValid())
            result.background = Qt::white;
        return result;
    }

    bool write(const QString& field, const CollageSettings& settings) const {
        return write(QStringList{field}, settings);
    }

    bool write(const QStringList& fields, const CollageSettings& settings) const {
        QMap<QString, QJsonValue> values;
        for (const QString& field : fields) {
            QJsonValue value;
            if (field == QStringLiteral("direction")) {
                switch (settings.direction) {
                case CollageDirection::Vertical:
                    value = QStringLiteral("vertical");
                    break;
                case CollageDirection::Horizontal:
                    value = QStringLiteral("horizontal");
                    break;
                case CollageDirection::Custom:
                    value = QStringLiteral("custom");
                    break;
                }
            } else if (field == QStringLiteral("sizing_mode"))
                value = settings.sizing == CollageSizing::Uniform ? QStringLiteral("uniform")
                                                                  : QStringLiteral("original");
            else if (field == QStringLiteral("border_px"))
                value = settings.border;
            else if (field == QStringLiteral("spacing_px"))
                value = settings.spacing;
            else if (field == QStringLiteral("rows"))
                value = settings.rows;
            else if (field == QStringLiteral("columns"))
                value = settings.columns;
            else if (field == QStringLiteral("custom_grid_initialized"))
                value = settings.customGridInitialized;
            else if (field == QStringLiteral("background_color"))
                value = storage::colorToRgbaString(settings.background);
            else
                return false;
            values.insert(QStringLiteral("collage/") + field, value);
        }
        return m_configuration.setValues(values);
    }

    [[nodiscard]] CollagePdfSettings readPdf() const {
        CollagePdfSettings result;
        const auto value = [&](const QString& field) {
            return m_configuration.value(QStringLiteral("collage/pdf_") + field);
        };
        result.stitchingMethod =
            value(QStringLiteral("stitching_method")).toString() == QStringLiteral("template_based")
                ? CollagePdfStitchingMethod::TemplateBased
                : CollagePdfStitchingMethod::PageBased;
        const QString selectedTemplate = value(QStringLiteral("template_type")).toString();
        const QStringList templates{QStringLiteral("two_images"),   QStringLiteral("id_card"),
                                    QStringLiteral("three_images"), QStringLiteral("four_images"),
                                    QStringLiteral("five_images"),  QStringLiteral("six_images"),
                                    QStringLiteral("seven_images"), QStringLiteral("eight_images"),
                                    QStringLiteral("nine_images")};
        const auto templateIndex = templates.indexOf(selectedTemplate);
        if (templateIndex >= 0)
            result.templateType = static_cast<CollagePdfTemplateType>(templateIndex);
        result.imageSpacingMm = value(QStringLiteral("image_spacing_mm")).toDouble(5);
        const QString paper = value(QStringLiteral("paper_size")).toString();
        if (paper == QStringLiteral("a3"))
            result.paperSize = CollagePdfPaperSize::A3;
        else if (paper == QStringLiteral("a5"))
            result.paperSize = CollagePdfPaperSize::A5;
        else if (paper == QStringLiteral("b5"))
            result.paperSize = CollagePdfPaperSize::B5;
        else if (paper == QStringLiteral("letter"))
            result.paperSize = CollagePdfPaperSize::Letter;
        else if (paper == QStringLiteral("legal"))
            result.paperSize = CollagePdfPaperSize::Legal;
        else if (paper == QStringLiteral("custom"))
            result.paperSize = CollagePdfPaperSize::Custom;
        else if (paper == QStringLiteral("image_size"))
            result.paperSize = CollagePdfPaperSize::ImageSize;
        result.orientation =
            value(QStringLiteral("orientation")).toString() == QStringLiteral("landscape")
                ? CollagePdfOrientation::Landscape
                : CollagePdfOrientation::Portrait;
        result.customWidthMm = value(QStringLiteral("custom_width_mm")).toDouble(210);
        result.customHeightMm = value(QStringLiteral("custom_height_mm")).toDouble(297);
        result.marginTopMm = value(QStringLiteral("margin_top_mm")).toDouble(10);
        // The single margin control uses the previous top-margin preference on every side.
        result.marginBottomMm = result.marginTopMm;
        result.marginLeftMm = result.marginTopMm;
        result.marginRightMm = result.marginTopMm;
        result.exportMode =
            value(QStringLiteral("export_mode")).toString() == QStringLiteral("per_page")
                ? CollagePdfExportMode::PerPage
                : CollagePdfExportMode::Combined;
        // Individual values can be valid while a hand-edited configuration or edits from
        // independent windows produce an unusable combination. Start with a usable form.
        return validateCollagePdfSettings(result).isEmpty() ? result : CollagePdfSettings{};
    }

    bool writePdf(const QString& field, const CollagePdfSettings& settings) const {
        return writePdf(QStringList{field}, settings);
    }

    // Active page validation may ignore hidden drafts. Keep their edits pending until their
    // values can be stored, without blocking independent preferences that are ready to persist.
    [[nodiscard]] static QStringList persistablePdfFields(const QStringList& fields,
                                                          const CollagePdfSettings& settings) {
        QStringList result;
        for (const auto& field : fields)
            if (!pdfFieldValues(field, settings).isEmpty())
                result.append(field);
        return result;
    }

    bool writePdf(const QStringList& fields, const CollagePdfSettings& settings) const {
        if (!validateCollagePdfSettings(settings).isEmpty())
            return false;
        QMap<QString, QJsonValue> values;
        for (const QString& field : fields) {
            const auto next = pdfFieldValues(field, settings);
            if (next.isEmpty())
                return false;
            values.insert(next);
        }
        return m_configuration.setValues(values);
    }

  private:
    [[nodiscard]] static QMap<QString, QJsonValue>
    pdfFieldValues(const QString& field, const CollagePdfSettings& settings) {
        QMap<QString, QJsonValue> values;
        if (field == QStringLiteral("margin_mm")) {
            const double margin = settings.marginTopMm;
            if (settings.marginBottomMm != margin || settings.marginLeftMm != margin ||
                settings.marginRightMm != margin)
                return {};
            for (const auto* key :
                 {"margin_top_mm", "margin_bottom_mm", "margin_left_mm", "margin_right_mm"})
                values.insert(QStringLiteral("collage/pdf_") + QString::fromLatin1(key), margin);
        } else {
            QJsonValue value;
            if (field == QStringLiteral("stitching_method")) {
                if (settings.stitchingMethod != CollagePdfStitchingMethod::PageBased &&
                    settings.stitchingMethod != CollagePdfStitchingMethod::TemplateBased)
                    return {};
                value = settings.stitchingMethod == CollagePdfStitchingMethod::PageBased
                            ? QStringLiteral("page_based")
                            : QStringLiteral("template_based");
            } else if (field == QStringLiteral("template_type")) {
                const QStringList templates{
                    QStringLiteral("two_images"),   QStringLiteral("id_card"),
                    QStringLiteral("three_images"), QStringLiteral("four_images"),
                    QStringLiteral("five_images"),  QStringLiteral("six_images"),
                    QStringLiteral("seven_images"), QStringLiteral("eight_images"),
                    QStringLiteral("nine_images")};
                const auto index = static_cast<int>(settings.templateType);
                if (index < 0 || index >= templates.size())
                    return {};
                value = templates.at(index);
            } else if (field == QStringLiteral("image_spacing_mm"))
                value = settings.imageSpacingMm;
            else if (field == QStringLiteral("paper_size")) {
                switch (settings.paperSize) {
                case CollagePdfPaperSize::A3:
                    value = QStringLiteral("a3");
                    break;
                case CollagePdfPaperSize::A4:
                    value = QStringLiteral("a4");
                    break;
                case CollagePdfPaperSize::A5:
                    value = QStringLiteral("a5");
                    break;
                case CollagePdfPaperSize::B5:
                    value = QStringLiteral("b5");
                    break;
                case CollagePdfPaperSize::Letter:
                    value = QStringLiteral("letter");
                    break;
                case CollagePdfPaperSize::Legal:
                    value = QStringLiteral("legal");
                    break;
                case CollagePdfPaperSize::Custom:
                    value = QStringLiteral("custom");
                    break;
                case CollagePdfPaperSize::ImageSize:
                    value = QStringLiteral("image_size");
                    break;
                }
            } else if (field == QStringLiteral("orientation")) {
                if (settings.orientation != CollagePdfOrientation::Portrait &&
                    settings.orientation != CollagePdfOrientation::Landscape)
                    return {};
                value = settings.orientation == CollagePdfOrientation::Portrait
                            ? QStringLiteral("portrait")
                            : QStringLiteral("landscape");
            } else if (field == QStringLiteral("export_mode")) {
                if (settings.exportMode != CollagePdfExportMode::Combined &&
                    settings.exportMode != CollagePdfExportMode::PerPage)
                    return {};
                value = settings.exportMode == CollagePdfExportMode::Combined
                            ? QStringLiteral("combined")
                            : QStringLiteral("per_page");
            } else if (field == QStringLiteral("custom_width_mm"))
                value = settings.customWidthMm;
            else if (field == QStringLiteral("custom_height_mm"))
                value = settings.customHeightMm;
            else if (field == QStringLiteral("margin_top_mm"))
                value = settings.marginTopMm;
            else if (field == QStringLiteral("margin_bottom_mm"))
                value = settings.marginBottomMm;
            else if (field == QStringLiteral("margin_left_mm"))
                value = settings.marginLeftMm;
            else if (field == QStringLiteral("margin_right_mm"))
                value = settings.marginRightMm;
            else
                return {};
            values.insert(QStringLiteral("collage/pdf_") + field, value);
        }
        // Use the same serialization and schema admission for eligibility and atomic writes.
        // In particular, never publish a hidden NaN draft through JSON's conversion to null.
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            if (!storage::ConfigurationSchema::normalize(it.key(), it.value()).valid)
                return {};
        return values;
    }

    storage::ConfigurationStore& m_configuration;
};

} // namespace snow_shot::presentation
#endif
