// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/presentation/collagesettingsstore.h"
#include "snow_shot/storage/configurationschema.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace snow_shot::presentation;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool near(qreal first, qreal second) {
    return std::abs(first - second) <= 0.000001;
}

QImage fixture(QSize size) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    return image;
}

void modeStateAndEditing() {
    CollageDocument document;
    require(document.outputType() == CollageOutputType::StaticImage &&
                document.pdfSettings() == CollagePdfSettings{},
            "new documents default to static output and A4 PDF settings");
    const quint64 first = document.append(fixture({20, 10}), QStringLiteral("first"));
    const quint64 second = document.append(fixture({8, 12}), QStringLiteral("second"));
    const quint64 third = document.append(fixture({15, 30}), QStringLiteral("third"));
    document.select(second);
    require(document.rotate(first, true) && document.flip(first, true),
            "shared transforms can be edited");
    const auto transform = document.items().first().transform;
    CollageSettings raster;
    raster.direction = CollageDirection::Horizontal;
    raster.border = 7;
    raster.spacing = 5;
    document.setSettings(raster);
    const CollageLayout originalLayout = document.layout();
    CollagePdfSettings pdf;
    pdf.paperSize = CollagePdfPaperSize::Letter;
    pdf.orientation = CollagePdfOrientation::Landscape;
    pdf.marginLeftMm = 12.5;
    pdf.exportMode = CollagePdfExportMode::PerPage;
    document.setPdfSettings(pdf);
    for (int iteration = 0; iteration < 3; ++iteration) {
        document.setOutputType(CollageOutputType::PdfDocument);
        require(document.items().size() == 3 && document.items().first().id == first &&
                    document.items().first().transform == transform &&
                    document.selectedId() == second && document.pdfSettings() == pdf,
                "PDF mode preserves content, transformations, selection and settings");
        document.setOutputType(CollageOutputType::StaticImage);
        require(document.settings() == raster && document.layout().size == originalLayout.size &&
                    document.layout().rects == originalLayout.rects,
                "mode switching preserves independent static layout settings and geometry");
    }
    require(document.moveItem(first, 2) && document.items()[0].id == second &&
                document.items()[1].id == third && document.items()[2].id == first &&
                document.selectedId() == second,
            "moving to the end inserts and shifts intervening items without changing selection");
    require(document.moveItem(first, 0) && document.items()[0].id == first &&
                document.items()[1].id == second && document.items()[2].id == third,
            "moving to the beginning restores the insertion ordering");
    require(!document.moveItem(first, 0) && !document.moveItem(first, -1) &&
                !document.moveItem(first, 3) && !document.moveItem(999, 1),
            "no-op and invalid moves do not change content");
    const auto revision = document.items()[0].revision;
    require(!document.replace(first, {}, QStringLiteral("failed")) &&
                document.items()[0].label == QStringLiteral("first") &&
                document.items()[0].transform == transform &&
                document.items()[0].revision == revision,
            "a failed replacement leaves original pixels, label, transform and revision intact");
    require(document.replace(first, fixture({9, 4}), QStringLiteral("new")) &&
                document.items()[0].id == first &&
                document.items()[0].label == QStringLiteral("new") &&
                document.items()[0].transform.isIdentity() &&
                document.items()[0].image.size() == QSize(9, 4) &&
                document.items()[0].revision == revision + 1 && document.selectedId() == second,
            "successful replacement keeps position and ID and resets existing transforms");
    document.setOutputType(CollageOutputType::PdfDocument);
    document.clear();
    require(document.items().isEmpty() && document.selectedId() == 0 &&
                document.settings() == raster && document.pdfSettings() == pdf &&
                document.outputType() == CollageOutputType::PdfDocument,
            "clearing keeps both layout settings and the selected output type");
}

void reversePageOrder() {
    for (const int count : {0, 1, 2, 5}) {
        CollageDocument document;
        document.setOutputType(CollageOutputType::PdfDocument);
        auto settings = document.settings();
        settings.sizing = CollageSizing::Original;
        document.setSettings(settings);
        for (int index = 0; index < count; ++index)
            document.append(fixture({10 + index, 20 + index}), QString::number(index));
        if (count > 1) {
            document.rotate(document.items().front().id, true);
            document.flip(document.items().back().id, true);
            document.moveItem(document.items().front().id, count - 1);
            document.select(document.items().front().id);
        }
        const auto original = document.items();
        const auto originalLayout = document.layout();
        const auto revision = document.contentRevision();
        const auto selected = document.selectedId();
        int changes = 0;
        int selectionChanges = 0;
        QObject::connect(&document, &CollageDocument::changed, &document, [&] { ++changes; });
        QObject::connect(&document, &CollageDocument::selectionChanged, &document,
                         [&] { ++selectionChanges; });
        const bool reversed = count > 1;
        require(document.reverseItems() == reversed && changes == int(reversed) &&
                    document.contentRevision() == revision + int(reversed),
                "reversal publishes one content change only when at least two pages exist");
        const auto layout = document.layout();
        for (int index = 0; index < count; ++index) {
            const auto& item = document.items()[index];
            const auto& expected = original[count - index - 1];
            require(item.id == expected.id && item.label == expected.label &&
                        item.image.constBits() == expected.image.constBits() &&
                        item.preview.constBits() == expected.preview.constBits() &&
                        item.transform == expected.transform &&
                        item.revision == expected.revision &&
                        item.resourceOwner == expected.resourceOwner &&
                        layout.rects[index].size() == transformedSize(expected),
                    "reversal preserves each page and invalidates cached static layout geometry");
        }
        require(document.selectedId() == selected && selectionChanges == 0,
                "reversal preserves the selected page without publishing a selection change");
        require(document.reverseItems() == reversed && changes == 2 * int(reversed) &&
                    document.layout().rects == originalLayout.rects,
                "reversing twice restores the original layout");
        for (int index = 0; index < count; ++index)
            require(document.items()[index].id == original[index].id,
                    "reversing twice restores the current order including prior manual moves");
    }
}

void paperAndImageGeometry() {
    struct Paper final {
        CollagePdfPaperSize type;
        QSizeF size;
    };
    const std::array papers = {
        Paper{CollagePdfPaperSize::A3, {297, 420}},
        Paper{CollagePdfPaperSize::A4, {210, 297}},
        Paper{CollagePdfPaperSize::A5, {148, 210}},
        Paper{CollagePdfPaperSize::B5, {176, 250}},
        Paper{CollagePdfPaperSize::Letter, {215.9, 279.4}},
        Paper{CollagePdfPaperSize::Legal, {215.9, 355.6}},
        Paper{CollagePdfPaperSize::Custom, {123.75, 234.5}},
    };
    for (const auto& paper : papers) {
        CollagePdfSettings settings;
        settings.paperSize = paper.type;
        settings.customWidthMm = 123.75;
        settings.customHeightMm = 234.5;
        for (const auto orientation :
             {CollagePdfOrientation::Portrait, CollagePdfOrientation::Landscape}) {
            settings.orientation = orientation;
            QSizeF expected = paper.size;
            if (orientation == CollagePdfOrientation::Landscape)
                expected.transpose();
            const auto layout = calculateCollagePdfPageLayout({100, 200}, settings);
            require(layout.isValid() && layout.pageMillimeters == expected &&
                        layout.contentMillimeters ==
                            QRectF(10, 10, expected.width() - 20, expected.height() - 20),
                    "all predefined and decimal custom sizes respect orientation and margins");
            require(near(layout.imageMillimeters.width() / layout.imageMillimeters.height(), 0.5) &&
                        near(layout.imageMillimeters.center().x(),
                             layout.contentMillimeters.center().x()) &&
                        near(layout.imageMillimeters.center().y(),
                             layout.contentMillimeters.center().y()) &&
                        layout.contentMillimeters.adjusted(-0.000001, -0.000001, 0.000001, 0.000001)
                            .contains(layout.imageMillimeters),
                    "images scale proportionally and remain completely centered within content");
        }
    }
    CollagePdfSettings settings;
    settings.marginLeftMm = 12;
    settings.marginRightMm = 23;
    settings.marginTopMm = 34;
    settings.marginBottomMm = 45;
    const auto wide = calculateCollagePdfPageLayout({2000, 1000}, settings);
    require(wide.contentMillimeters == QRectF(12, 34, 175, 218) &&
                wide.imageMillimeters == QRectF(12, 99.25, 175, 87.5),
            "asymmetric margins define the complete image area and accurate centering");
    const auto longImage = calculateCollagePdfPageLayout({1, 1000000}, settings);
    require(longImage.isValid() && near(longImage.imageMillimeters.height(), 218) &&
                near(longImage.imageMillimeters.width(), 0.000218) &&
                near(longImage.imageMillimeters.center().x(), 99.5),
            "long images fit on one page without crop or aspect-ratio distortion");
    settings.marginTopMm = settings.marginBottomMm = settings.marginLeftMm =
        settings.marginRightMm = 0;
    const auto borderless = calculateCollagePdfPageLayout({210, 297}, settings);
    require(borderless.isValid() && borderless.imageMillimeters == QRectF(0, 0, 210, 297),
            "zero margins can fit an image across the entire page");
    require(!calculateCollagePdfPageLayout({}, settings).isValid(),
            "invalid transformed source dimensions cannot form a page");
}

void imageSizeGeometry() {
    CollagePdfSettings settings;
    settings.paperSize = CollagePdfPaperSize::ImageSize;
    for (const auto orientation :
         {CollagePdfOrientation::Portrait, CollagePdfOrientation::Landscape}) {
        settings.orientation = orientation;
        for (const auto pixels : {QSize(96, 192), QSize(192, 96), QSize(1, 1)}) {
            const auto layout = calculateCollagePdfPageLayout(pixels, settings);
            const QSizeF expected = QSizeF(pixels) * (25.4 / 96.0);
            require(layout.isValid() && layout.pageMillimeters == expected &&
                        layout.contentMillimeters == QRectF(QPointF(0, 0), expected) &&
                        layout.imageMillimeters == layout.contentMillimeters,
                    "image-sized pages use 96 DPI and fill the page regardless of paper "
                    "orientation and stored margins");
        }
    }
    CollageDocument document;
    const auto id = document.append(fixture({96, 192}), QStringLiteral("rotated"));
    require(id && document.rotate(id, true), "rotate the image-sized page fixture");
    const auto rotated =
        calculateCollagePdfPageLayout(transformedSize(document.items().first()), settings);
    require(near(rotated.pageMillimeters.width(), 50.8) &&
                near(rotated.pageMillimeters.height(), 25.4),
            "image-sized paper follows the transformed original dimensions");
    settings.customWidthMm = std::numeric_limits<qreal>::quiet_NaN();
    settings.marginTopMm = std::numeric_limits<qreal>::quiet_NaN();
    settings.marginLeftMm = -1;
    require(validateCollagePdfSettings(settings).isEmpty() &&
                calculateCollagePdfPageLayout({96, 192}, settings).isValid(),
            "inactive paper and margin drafts do not block image-sized pages");
    settings.paperSize = CollagePdfPaperSize::Custom;
    require(!validateCollagePdfSettings(settings).isEmpty(),
            "switching back to paper sizing restores validation of saved drafts");
    settings.paperSize = CollagePdfPaperSize::ImageSize;
    require(!calculateCollagePdfPageLayout({}, settings).isValid() &&
                !calculateCollagePdfPageLayout({std::numeric_limits<int>::max(), 1}, settings)
                     .isValid(),
            "image-sized pages reject empty images and dimensions beyond the PDF limit");
}

void contentGeneration() {
    CollageDocument document;
    require(document.contentRevision() == 0, "new documents start at an empty content generation");
    document.clear();
    document.select(999);
    document.setOutputType(CollageOutputType::PdfDocument);
    auto staticSettings = document.settings();
    staticSettings.spacing = 12;
    document.setSettings(staticSettings);
    auto pdfSettings = document.pdfSettings();
    pdfSettings.marginTopMm = 14.5;
    document.setPdfSettings(pdfSettings);
    require(document.contentRevision() == 0, "empty clear, selection, output type and layout "
                                             "settings do not advance content generation");
    const auto first = document.append(fixture({4, 2}), QStringLiteral("first"));
    const auto second = document.append(fixture({2, 4}), QStringLiteral("second"));
    require(document.contentRevision() == 2,
            "each successfully appended original advances content");
    document.select(second);
    require(!document.append({}, QStringLiteral("invalid")) &&
                !document.replace(first, {}, QStringLiteral("invalid")) && !document.remove(999) &&
                !document.swapItems(first, first) && !document.moveItem(first, 0) &&
                !document.moveItem(first, 2) && !document.rotate(999, true) &&
                !document.flip(999, true) && document.resetTransform(first) &&
                document.contentRevision() == 2,
            "selection, failed edits and identity-transform reset leave the content cache current");
    require(document.replace(first, fixture({4, 2}), QStringLiteral("replacement")) &&
                document.contentRevision() == 3,
            "successful replacement invalidates original and name caches");
    require(document.swapItems(first, second) && document.contentRevision() == 4 &&
                document.moveItem(first, 0) && document.contentRevision() == 5,
            "both swap and insertion ordering changes advance content generation");
    require(document.rotate(first, true) && document.contentRevision() == 6 &&
                document.flip(first, true) && document.contentRevision() == 7 &&
                document.resetTransform(first) && document.contentRevision() == 8,
            "each successful transformation invalidates page content geometry");
    require(document.remove(second) && document.contentRevision() == 9,
            "successful removal invalidates ordered content caches");
    document.clear();
    document.clear();
    require(document.contentRevision() == 10,
            "nonempty clearing advances content once and empty clearing remains a no-op");
}

void fieldValidationAndLastValidState() {
    const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
    const qreal infinity = std::numeric_limits<qreal>::infinity();
    for (const auto value : {qreal(-1), qreal(0), nan, infinity}) {
        CollagePdfSettings settings;
        settings.paperSize = CollagePdfPaperSize::Custom;
        settings.customWidthMm = value;
        settings.customHeightMm = value;
        const auto errors = validateCollagePdfSettings(settings);
        require(errors.contains(QStringLiteral("custom_width_mm")) &&
                    errors.contains(QStringLiteral("custom_height_mm")) &&
                    !calculateCollagePdfPageLayout({20, 10}, settings).isValid(),
                "nonpositive and nonfinite custom dimensions produce field-level errors");
    }
    CollagePdfSettings maximum;
    maximum.paperSize = CollagePdfPaperSize::Custom;
    maximum.customWidthMm = maximum.customHeightMm = 381'000'000;
    require(validateCollagePdfSettings(maximum).isEmpty(),
            "the physical PDF MediaBox and UserUnit limit remains valid");
    for (const auto value : {qreal(381'000'001), qreal(1e308)}) {
        maximum.customWidthMm = maximum.customHeightMm = value;
        const auto errors = validateCollagePdfSettings(maximum);
        require(errors.contains(QStringLiteral("custom_width_mm")) &&
                    errors.contains(QStringLiteral("custom_height_mm")),
                "dimensions exceeding the PDF format limit are invalid before points conversion");
    }
    for (const auto value : {qreal(-1), nan, infinity}) {
        CollagePdfSettings settings;
        settings.marginLeftMm = settings.marginRightMm = settings.marginTopMm =
            settings.marginBottomMm = value;
        const auto errors = validateCollagePdfSettings(settings);
        require(errors.contains(QStringLiteral("margin_left_mm")) &&
                    errors.contains(QStringLiteral("margin_right_mm")) &&
                    errors.contains(QStringLiteral("margin_top_mm")) &&
                    errors.contains(QStringLiteral("margin_bottom_mm")),
                "negative and nonfinite margins identify each invalid field");
    }
    CollagePdfSettings margins;
    margins.marginLeftMm = 200;
    margins.marginRightMm = 10;
    require(validateCollagePdfSettings(margins).contains(QStringLiteral("margin_left_mm")) &&
                validateCollagePdfSettings(margins).contains(QStringLiteral("margin_right_mm")),
            "margins must leave strictly positive width, including the exact boundary");
    margins = {};
    margins.marginTopMm = 300;
    require(validateCollagePdfSettings(margins).contains(QStringLiteral("margin_top_mm")) &&
                validateCollagePdfSettings(margins).contains(QStringLiteral("margin_bottom_mm")),
            "margins must leave strictly positive height");
    CollagePdfSettings malformed;
    malformed.paperSize = static_cast<CollagePdfPaperSize>(999);
    malformed.orientation = static_cast<CollagePdfOrientation>(999);
    malformed.exportMode = static_cast<CollagePdfExportMode>(999);
    const auto malformedErrors = validateCollagePdfSettings(malformed);
    require(malformedErrors.size() == 3,
            "invalid enum values are rejected before entering geometry or export");

    CollageDocument document;
    CollagePdfSettings valid;
    valid.marginTopMm = 22.25;
    document.setPdfSettings(valid);
    auto invalid = valid;
    invalid.marginTopMm = nan;
    int changes = 0;
    QObject::connect(&document, &CollageDocument::changed, &document, [&] { ++changes; });
    document.setPdfSettings(invalid);
    document.setPdfSettings(invalid);
    require(changes == 1 && std::isnan(document.pdfSettings().marginTopMm) &&
                document.lastValidPdfSettings() == valid,
            "incomplete settings remain editable while the last valid preview and notifications "
            "stay stable");
    document.setOutputType(CollageOutputType::PdfDocument);
    document.setOutputType(CollageOutputType::StaticImage);
    require(std::isnan(document.pdfSettings().marginTopMm) &&
                document.lastValidPdfSettings() == valid,
            "switching output type preserves invalid form state and its last valid preview");
    invalid.marginTopMm = 4.125;
    document.setPdfSettings(invalid);
    require(document.lastValidPdfSettings() == invalid,
            "correcting a field promptly advances the valid preview settings");
}

void settingsPersistence() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "create a configuration fixture");
    const QString path = temporary.filePath(QStringLiteral("configuration.json"));
    storage::ConfigurationStore configuration(path, true, true, 60000);
    CollageSettingsStore store(configuration);
    const auto uniform = [](const CollagePdfSettings& settings, qreal value) {
        return settings.marginTopMm == value && settings.marginBottomMm == value &&
               settings.marginLeftMm == value && settings.marginRightMm == value;
    };
    require(store.readPdf() == CollagePdfSettings{}, "PDF settings defaults come from the schema");
    require(configuration.setValues({
                {QStringLiteral("collage/pdf_margin_top_mm"), 14.125},
                {QStringLiteral("collage/pdf_margin_bottom_mm"), 20.5},
                {QStringLiteral("collage/pdf_margin_left_mm"), 12.75},
                {QStringLiteral("collage/pdf_margin_right_mm"), 3.25},
            }) &&
                uniform(store.readPdf(), 14.125) &&
                configuration.value(QStringLiteral("collage/pdf_margin_bottom_mm")).toDouble() ==
                    20.5 &&
                configuration.value(QStringLiteral("collage/pdf_margin_left_mm")).toDouble() ==
                    12.75,
            "legacy unequal preferences use the saved top margin on every side without rewriting "
            "storage on read");
    auto first = store.readPdf();
    auto second = first;
    first.paperSize = CollagePdfPaperSize::Legal;
    require(store.writePdf(QStringLiteral("paper_size"), first),
            "paper preferences persist independently from the shared page margin");
    second.marginTopMm = second.marginBottomMm = second.marginLeftMm = second.marginRightMm =
        24.375;
    const QStringList marginKeys{QStringLiteral("collage/pdf_margin_top_mm"),
                                 QStringLiteral("collage/pdf_margin_bottom_mm"),
                                 QStringLiteral("collage/pdf_margin_left_mm"),
                                 QStringLiteral("collage/pdf_margin_right_mm")};
    QStringList changedMargins;
    bool atomic = true;
    const auto changes =
        QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &configuration,
                         [&](const QString& key, const QJsonValue&) {
                             if (!marginKeys.contains(key))
                                 return;
                             changedMargins.append(key);
                             for (const auto& marginKey : marginKeys)
                                 atomic =
                                     atomic && configuration.value(marginKey).toDouble() == 24.375;
                         });
    require(store.writePdf(QStringLiteral("margin_mm"), second),
            "an older independent form can persist one uniform decimal margin");
    QObject::disconnect(changes);
    auto expectedKeys = marginKeys;
    expectedKeys.sort();
    changedMargins.sort();
    require(atomic && changedMargins == expectedKeys,
            "one margin alias updates all four stored keys before announcing any change");
    const auto merged = store.readPdf();
    require(merged.paperSize == CollagePdfPaperSize::Legal && uniform(merged, 24.375),
            "a shared margin edit preserves newer paper settings from another independent window");
    CollageSettings raster;
    raster.border = 13;
    require(store.write(QStringLiteral("border_px"), raster) && store.readPdf() == merged,
            "static layout persistence does not overwrite PDF preferences");
    first = merged;
    first.marginLeftMm = -1;
    require(!store.writePdf(QStringLiteral("margin_mm"), first) && store.readPdf() == merged,
            "invalid document margins cannot be persisted through the uniform alias");
    first = merged;
    first.marginLeftMm = 22.5;
    require(validateCollagePdfSettings(first).isEmpty() &&
                !store.writePdf(QStringLiteral("margin_mm"), first) && store.readPdf() == merged,
            "the uniform persistence alias rejects an unequal tuple without partially changing "
            "valid settings");
    require(
        !configuration.setValue(QStringLiteral("collage/pdf_custom_width_mm"), 0) &&
            !configuration.setValue(QStringLiteral("collage/pdf_custom_height_mm"), 381'000'001) &&
            !configuration.setValue(QStringLiteral("collage/pdf_margin_top_mm"), -0.1) &&
            !configuration.setValue(QStringLiteral("collage/pdf_margin_right_mm"),
                                    QStringLiteral("10")) &&
            !configuration.setValue(QStringLiteral("collage/pdf_margin_bottom_mm"), QJsonValue()),
        "schema rejects nonpositive paper dimensions, negative margins and incorrect scalar types");
    require(store.writePdf(QStringLiteral("export_mode"),
                           CollagePdfSettings{.exportMode = CollagePdfExportMode::PerPage}) &&
                store.readPdf().exportMode == CollagePdfExportMode::PerPage &&
                uniform(store.readPdf(), 24.375),
            "PDF export mode persists independently without resetting the uniform margin");
    require(configuration.flushNow().success, "persist PDF configuration");
    storage::ConfigurationStore reopened(path, true, true, 60000);
    require(CollageSettingsStore(reopened).readPdf() == store.readPdf(),
            "decimal PDF settings survive a fresh disk-backed configuration read");
    for (const auto& key : marginKeys)
        require(
            reopened.value(key).toDouble() == 24.375,
            "every backing margin key retains the uniform decimal value after reopening storage");
    const auto finite = storage::ConfigurationSchema::normalize(
        QStringLiteral("collage/pdf_custom_width_mm"), 123.456);
    require(finite.valid && finite.value.toDouble() == 123.456,
            "schema retains millimeter precision without integer rounding or silent clamping");
}

void imageSizePersistence() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "create an image-sized paper configuration fixture");
    const QString path = temporary.filePath(QStringLiteral("configuration.json"));
    storage::ConfigurationStore configuration(path, true, true, 60000);
    CollageSettingsStore store(configuration);
    auto settings = store.readPdf();
    settings.paperSize = CollagePdfPaperSize::ImageSize;
    settings.orientation = CollagePdfOrientation::Landscape;
    require(
        store.writePdf({QStringLiteral("paper_size"), QStringLiteral("orientation")}, settings) &&
            configuration.value(QStringLiteral("collage/pdf_paper_size")).toString() ==
                QStringLiteral("image_size") &&
            store.readPdf() == settings,
        "image size persists with a stable key while retaining independent paper settings");
    require(configuration.flushNow().success, "persist the image-size preference");
    storage::ConfigurationStore reopened(path, true, true, 60000);
    require(CollageSettingsStore(reopened).readPdf() == settings,
            "the image-size preference survives reopening its disk-backed configuration");
}

void inactiveDraftPersistence() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "create an inactive-draft configuration fixture");
    const QString path = temporary.filePath(QStringLiteral("configuration.json"));
    storage::ConfigurationStore configuration(path, true, true, 60000);
    CollageSettingsStore store(configuration);
    const auto previous = store.readPdf();
    auto draft = previous;
    draft.paperSize = CollagePdfPaperSize::ImageSize;
    draft.exportMode = CollagePdfExportMode::PerPage;
    draft.customWidthMm = std::numeric_limits<qreal>::quiet_NaN();
    draft.customHeightMm = 0;
    draft.marginTopMm = draft.marginBottomMm = draft.marginLeftMm = draft.marginRightMm = -1;
    const QStringList fields{QStringLiteral("paper_size"), QStringLiteral("custom_width_mm"),
                             QStringLiteral("custom_height_mm"), QStringLiteral("margin_mm"),
                             QStringLiteral("export_mode")};
    require(validateCollagePdfSettings(draft).isEmpty() && !store.writePdf(fields, draft) &&
                store.readPdf() == previous,
            "page validity never permits an invalid hidden draft or a partial configuration write");
    const auto ready = CollageSettingsStore::persistablePdfFields(fields, draft);
    require(ready == QStringList{QStringLiteral("paper_size"), QStringLiteral("export_mode")} &&
                store.writePdf(ready, draft),
            "independent valid preferences can persist while invalid fields remain pending");
    auto expected = previous;
    expected.paperSize = CollagePdfPaperSize::ImageSize;
    expected.exportMode = CollagePdfExportMode::PerPage;
    require(store.readPdf() == expected && std::isnan(draft.customWidthMm) &&
                draft.marginTopMm == -1,
            "saving eligible fields preserves both previous hidden preferences and raw drafts");
    draft.customWidthMm = 180.5;
    draft.customHeightMm = 260.25;
    draft.marginTopMm = draft.marginBottomMm = draft.marginLeftMm = draft.marginRightMm = 12.5;
    require(CollageSettingsStore::persistablePdfFields(fields, draft) == fields &&
                store.writePdf(fields, draft) && store.readPdf() == draft,
            "corrected dimensions and uniform margins become eligible together");
    draft.marginLeftMm = 13;
    require(!CollageSettingsStore::persistablePdfFields({QStringLiteral("margin_mm")}, draft)
                    .contains(QStringLiteral("margin_mm")) &&
                !store.writePdf(QStringLiteral("margin_mm"), draft),
            "the uniform alias cannot silently persist an unequal margin tuple");
    draft.orientation = static_cast<CollagePdfOrientation>(-1);
    require(CollageSettingsStore::persistablePdfFields({QStringLiteral("orientation")}, draft)
                    .isEmpty() &&
                !store.writePdf(QStringLiteral("orientation"), draft),
            "hidden invalid orientations cannot be serialized as a different valid preference");
    require(configuration.flushNow().success, "persist eligible PDF preferences to disk");
    storage::ConfigurationStore reopened(path, true, true, 60000);
    require(CollageSettingsStore(reopened).readPdf() == store.readPdf(),
            "only admitted PDF preferences survive a fresh configuration read");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        modeStateAndEditing();
        reversePageOrder();
        paperAndImageGeometry();
        imageSizeGeometry();
        contentGeneration();
        fieldValidationAndLastValidState();
        settingsPersistence();
        imageSizePersistence();
        inactiveDraftPersistence();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
