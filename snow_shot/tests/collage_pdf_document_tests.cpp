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
    ImageStitchingDocument document;
    require(document.outputType() == ImageStitchingOutputType::StaticImage &&
                document.pdfSettings() == ImageStitchingPdfSettings{},
            "new documents default to static output and A4 PDF settings");
    const quint64 first = document.append(fixture({20, 10}), QStringLiteral("first"));
    const quint64 second = document.append(fixture({8, 12}), QStringLiteral("second"));
    const quint64 third = document.append(fixture({15, 30}), QStringLiteral("third"));
    document.select(second);
    require(document.rotate(first, true) && document.flip(first, true),
            "shared transforms can be edited");
    const auto transform = document.items().first().transform;
    ImageStitchingSettings raster;
    raster.direction = ImageStitchingDirection::Horizontal;
    raster.border = 7;
    raster.spacing = 5;
    document.setSettings(raster);
    const ImageStitchingLayout originalLayout = document.layout();
    ImageStitchingPdfSettings pdf;
    pdf.paperSize = ImageStitchingPdfPaperSize::Letter;
    pdf.orientation = ImageStitchingPdfOrientation::Landscape;
    pdf.marginLeftMm = 12.5;
    pdf.exportMode = ImageStitchingPdfExportMode::PerPage;
    document.setPdfSettings(pdf);
    for (int iteration = 0; iteration < 3; ++iteration) {
        document.setOutputType(ImageStitchingOutputType::PdfDocument);
        require(document.items().size() == 3 && document.items().first().id == first &&
                    document.items().first().transform == transform &&
                    document.selectedId() == second && document.pdfSettings() == pdf,
                "PDF mode preserves content, transformations, selection and settings");
        document.setOutputType(ImageStitchingOutputType::StaticImage);
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
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
    document.clear();
    require(document.items().isEmpty() && document.selectedId() == 0 &&
                document.settings() == raster && document.pdfSettings() == pdf &&
                document.outputType() == ImageStitchingOutputType::PdfDocument,
            "clearing keeps both layout settings and the selected output type");
}

void reversePageOrder() {
    for (const int count : {0, 1, 2, 5}) {
        ImageStitchingDocument document;
        document.setOutputType(ImageStitchingOutputType::PdfDocument);
        auto settings = document.settings();
        settings.sizing = ImageStitchingSizing::Original;
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
        QObject::connect(&document, &ImageStitchingDocument::changed, &document,
                         [&] { ++changes; });
        QObject::connect(&document, &ImageStitchingDocument::selectionChanged, &document,
                         [&] { ++selectionChanges; });
        const bool reversed = count > 1;
        require(document.reverseItems() == reversed && changes == int(reversed) &&
                    document.contentRevision() == revision + quint64(reversed),
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
        ImageStitchingPdfPaperSize type;
        QSizeF size;
    };
    const std::array papers = {
        Paper{ImageStitchingPdfPaperSize::A3, {297, 420}},
        Paper{ImageStitchingPdfPaperSize::A4, {210, 297}},
        Paper{ImageStitchingPdfPaperSize::A5, {148, 210}},
        Paper{ImageStitchingPdfPaperSize::B5, {176, 250}},
        Paper{ImageStitchingPdfPaperSize::Letter, {215.9, 279.4}},
        Paper{ImageStitchingPdfPaperSize::Legal, {215.9, 355.6}},
        Paper{ImageStitchingPdfPaperSize::Custom, {123.75, 234.5}},
    };
    for (const auto& paper : papers) {
        ImageStitchingPdfSettings settings;
        settings.paperSize = paper.type;
        settings.customWidthMm = 123.75;
        settings.customHeightMm = 234.5;
        for (const auto orientation :
             {ImageStitchingPdfOrientation::Portrait, ImageStitchingPdfOrientation::Landscape}) {
            settings.orientation = orientation;
            QSizeF expected = paper.size;
            if (orientation == ImageStitchingPdfOrientation::Landscape)
                expected.transpose();
            const auto layout = calculateImageStitchingPdfPageLayout({100, 200}, settings);
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
    ImageStitchingPdfSettings settings;
    settings.marginLeftMm = 12;
    settings.marginRightMm = 23;
    settings.marginTopMm = 34;
    settings.marginBottomMm = 45;
    const auto wide = calculateImageStitchingPdfPageLayout({2000, 1000}, settings);
    require(wide.contentMillimeters == QRectF(12, 34, 175, 218) &&
                wide.imageMillimeters == QRectF(12, 99.25, 175, 87.5),
            "asymmetric margins define the complete image area and accurate centering");
    const auto longImage = calculateImageStitchingPdfPageLayout({1, 1000000}, settings);
    require(longImage.isValid() && near(longImage.imageMillimeters.height(), 218) &&
                near(longImage.imageMillimeters.width(), 0.000218) &&
                near(longImage.imageMillimeters.center().x(), 99.5),
            "long images fit on one page without crop or aspect-ratio distortion");
    settings.marginTopMm = settings.marginBottomMm = settings.marginLeftMm =
        settings.marginRightMm = 0;
    const auto borderless = calculateImageStitchingPdfPageLayout({210, 297}, settings);
    require(borderless.isValid() && borderless.imageMillimeters == QRectF(0, 0, 210, 297),
            "zero margins can fit an image across the entire page");
    require(!calculateImageStitchingPdfPageLayout({}, settings).isValid(),
            "invalid transformed source dimensions cannot form a page");
}

void imageSizeGeometry() {
    ImageStitchingPdfSettings settings;
    settings.paperSize = ImageStitchingPdfPaperSize::ImageSize;
    for (const auto orientation :
         {ImageStitchingPdfOrientation::Portrait, ImageStitchingPdfOrientation::Landscape}) {
        settings.orientation = orientation;
        for (const auto pixels : {QSize(96, 192), QSize(192, 96), QSize(1, 1)}) {
            const auto layout = calculateImageStitchingPdfPageLayout(pixels, settings);
            const QSizeF expected = QSizeF(pixels) * (25.4 / 96.0);
            require(layout.isValid() && layout.pageMillimeters == expected &&
                        layout.contentMillimeters == QRectF(QPointF(0, 0), expected) &&
                        layout.imageMillimeters == layout.contentMillimeters,
                    "image-sized pages use 96 DPI and fill the page regardless of paper "
                    "orientation and stored margins");
        }
    }
    ImageStitchingDocument document;
    const auto id = document.append(fixture({96, 192}), QStringLiteral("rotated"));
    require(id && document.rotate(id, true), "rotate the image-sized page fixture");
    const auto rotated =
        calculateImageStitchingPdfPageLayout(transformedSize(document.items().first()), settings);
    require(near(rotated.pageMillimeters.width(), 50.8) &&
                near(rotated.pageMillimeters.height(), 25.4),
            "image-sized paper follows the transformed original dimensions");
    settings.customWidthMm = std::numeric_limits<qreal>::quiet_NaN();
    settings.marginTopMm = std::numeric_limits<qreal>::quiet_NaN();
    settings.marginLeftMm = -1;
    require(validateImageStitchingPdfSettings(settings).isEmpty() &&
                calculateImageStitchingPdfPageLayout({96, 192}, settings).isValid(),
            "inactive paper and margin drafts do not block image-sized pages");
    settings.paperSize = ImageStitchingPdfPaperSize::Custom;
    require(!validateImageStitchingPdfSettings(settings).isEmpty(),
            "switching back to paper sizing restores validation of saved drafts");
    settings.paperSize = ImageStitchingPdfPaperSize::ImageSize;
    require(
        !calculateImageStitchingPdfPageLayout({}, settings).isValid() &&
            !calculateImageStitchingPdfPageLayout({std::numeric_limits<int>::max(), 1}, settings)
                 .isValid(),
        "image-sized pages reject empty images and dimensions beyond the PDF limit");
}

void contentGeneration() {
    ImageStitchingDocument document;
    require(document.contentRevision() == 0, "new documents start at an empty content generation");
    document.clear();
    document.select(999);
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
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
        ImageStitchingPdfSettings settings;
        settings.paperSize = ImageStitchingPdfPaperSize::Custom;
        settings.customWidthMm = value;
        settings.customHeightMm = value;
        const auto errors = validateImageStitchingPdfSettings(settings);
        require(errors.contains(QStringLiteral("custom_width_mm")) &&
                    errors.contains(QStringLiteral("custom_height_mm")) &&
                    !calculateImageStitchingPdfPageLayout({20, 10}, settings).isValid(),
                "nonpositive and nonfinite custom dimensions produce field-level errors");
    }
    ImageStitchingPdfSettings maximum;
    maximum.paperSize = ImageStitchingPdfPaperSize::Custom;
    maximum.customWidthMm = maximum.customHeightMm = 381'000'000;
    require(validateImageStitchingPdfSettings(maximum).isEmpty(),
            "the physical PDF MediaBox and UserUnit limit remains valid");
    for (const auto value : {qreal(381'000'001), qreal(1e308)}) {
        maximum.customWidthMm = maximum.customHeightMm = value;
        const auto errors = validateImageStitchingPdfSettings(maximum);
        require(errors.contains(QStringLiteral("custom_width_mm")) &&
                    errors.contains(QStringLiteral("custom_height_mm")),
                "dimensions exceeding the PDF format limit are invalid before points conversion");
    }
    for (const auto value : {qreal(-1), nan, infinity}) {
        ImageStitchingPdfSettings settings;
        settings.marginLeftMm = settings.marginRightMm = settings.marginTopMm =
            settings.marginBottomMm = value;
        const auto errors = validateImageStitchingPdfSettings(settings);
        require(errors.contains(QStringLiteral("margin_left_mm")) &&
                    errors.contains(QStringLiteral("margin_right_mm")) &&
                    errors.contains(QStringLiteral("margin_top_mm")) &&
                    errors.contains(QStringLiteral("margin_bottom_mm")),
                "negative and nonfinite margins identify each invalid field");
    }
    ImageStitchingPdfSettings margins;
    margins.marginLeftMm = 200;
    margins.marginRightMm = 10;
    require(
        validateImageStitchingPdfSettings(margins).contains(QStringLiteral("margin_left_mm")) &&
            validateImageStitchingPdfSettings(margins).contains(QStringLiteral("margin_right_mm")),
        "margins must leave strictly positive width, including the exact boundary");
    margins = {};
    margins.marginTopMm = 300;
    require(
        validateImageStitchingPdfSettings(margins).contains(QStringLiteral("margin_top_mm")) &&
            validateImageStitchingPdfSettings(margins).contains(QStringLiteral("margin_bottom_mm")),
        "margins must leave strictly positive height");
    ImageStitchingPdfSettings malformed;
    malformed.paperSize = static_cast<ImageStitchingPdfPaperSize>(999);
    malformed.orientation = static_cast<ImageStitchingPdfOrientation>(999);
    malformed.exportMode = static_cast<ImageStitchingPdfExportMode>(999);
    const auto malformedErrors = validateImageStitchingPdfSettings(malformed);
    require(malformedErrors.size() == 3,
            "invalid enum values are rejected before entering geometry or export");

    ImageStitchingDocument document;
    ImageStitchingPdfSettings valid;
    valid.marginTopMm = 22.25;
    document.setPdfSettings(valid);
    auto invalid = valid;
    invalid.marginTopMm = nan;
    int changes = 0;
    QObject::connect(&document, &ImageStitchingDocument::changed, &document, [&] { ++changes; });
    document.setPdfSettings(invalid);
    document.setPdfSettings(invalid);
    require(changes == 1 && std::isnan(document.pdfSettings().marginTopMm) &&
                document.lastValidPdfSettings() == valid,
            "incomplete settings remain editable while the last valid preview and notifications "
            "stay stable");
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
    document.setOutputType(ImageStitchingOutputType::StaticImage);
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
    ImageStitchingSettingsStore store(configuration);
    const auto uniform = [](const ImageStitchingPdfSettings& settings, qreal value) {
        return settings.marginTopMm == value && settings.marginBottomMm == value &&
               settings.marginLeftMm == value && settings.marginRightMm == value;
    };
    require(store.readPdf() == ImageStitchingPdfSettings{},
            "PDF settings defaults come from the schema");
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
    first.paperSize = ImageStitchingPdfPaperSize::Legal;
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
    require(merged.paperSize == ImageStitchingPdfPaperSize::Legal && uniform(merged, 24.375),
            "a shared margin edit preserves newer paper settings from another independent window");
    ImageStitchingSettings raster;
    raster.border = 13;
    require(store.write(QStringLiteral("border_px"), raster) && store.readPdf() == merged,
            "static layout persistence does not overwrite PDF preferences");
    first = merged;
    first.marginLeftMm = -1;
    require(!store.writePdf(QStringLiteral("margin_mm"), first) && store.readPdf() == merged,
            "invalid document margins cannot be persisted through the uniform alias");
    first = merged;
    first.marginLeftMm = 22.5;
    require(validateImageStitchingPdfSettings(first).isEmpty() &&
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
    require(store.writePdf(
                QStringLiteral("export_mode"),
                ImageStitchingPdfSettings{.exportMode = ImageStitchingPdfExportMode::PerPage}) &&
                store.readPdf().exportMode == ImageStitchingPdfExportMode::PerPage &&
                uniform(store.readPdf(), 24.375),
            "PDF export mode persists independently without resetting the uniform margin");
    require(configuration.flushNow().success, "persist PDF configuration");
    storage::ConfigurationStore reopened(path, true, true, 60000);
    require(ImageStitchingSettingsStore(reopened).readPdf() == store.readPdf(),
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
    ImageStitchingSettingsStore store(configuration);
    auto settings = store.readPdf();
    settings.paperSize = ImageStitchingPdfPaperSize::ImageSize;
    settings.orientation = ImageStitchingPdfOrientation::Landscape;
    require(
        store.writePdf({QStringLiteral("paper_size"), QStringLiteral("orientation")}, settings) &&
            configuration.value(QStringLiteral("collage/pdf_paper_size")).toString() ==
                QStringLiteral("image_size") &&
            store.readPdf() == settings,
        "image size persists with a stable key while retaining independent paper settings");
    require(configuration.flushNow().success, "persist the image-size preference");
    storage::ConfigurationStore reopened(path, true, true, 60000);
    require(ImageStitchingSettingsStore(reopened).readPdf() == settings,
            "the image-size preference survives reopening its disk-backed configuration");
}

void inactiveDraftPersistence() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "create an inactive-draft configuration fixture");
    const QString path = temporary.filePath(QStringLiteral("configuration.json"));
    storage::ConfigurationStore configuration(path, true, true, 60000);
    ImageStitchingSettingsStore store(configuration);
    const auto previous = store.readPdf();
    auto draft = previous;
    draft.paperSize = ImageStitchingPdfPaperSize::ImageSize;
    draft.exportMode = ImageStitchingPdfExportMode::PerPage;
    draft.customWidthMm = std::numeric_limits<qreal>::quiet_NaN();
    draft.customHeightMm = 0;
    draft.marginTopMm = draft.marginBottomMm = draft.marginLeftMm = draft.marginRightMm = -1;
    const QStringList fields{QStringLiteral("paper_size"), QStringLiteral("custom_width_mm"),
                             QStringLiteral("custom_height_mm"), QStringLiteral("margin_mm"),
                             QStringLiteral("export_mode")};
    require(validateImageStitchingPdfSettings(draft).isEmpty() && !store.writePdf(fields, draft) &&
                store.readPdf() == previous,
            "page validity never permits an invalid hidden draft or a partial configuration write");
    const auto ready = ImageStitchingSettingsStore::persistablePdfFields(fields, draft);
    require(ready == QStringList{QStringLiteral("paper_size"), QStringLiteral("export_mode")} &&
                store.writePdf(ready, draft),
            "independent valid preferences can persist while invalid fields remain pending");
    auto expected = previous;
    expected.paperSize = ImageStitchingPdfPaperSize::ImageSize;
    expected.exportMode = ImageStitchingPdfExportMode::PerPage;
    require(store.readPdf() == expected && std::isnan(draft.customWidthMm) &&
                draft.marginTopMm == -1,
            "saving eligible fields preserves both previous hidden preferences and raw drafts");
    draft.customWidthMm = 180.5;
    draft.customHeightMm = 260.25;
    draft.marginTopMm = draft.marginBottomMm = draft.marginLeftMm = draft.marginRightMm = 12.5;
    require(ImageStitchingSettingsStore::persistablePdfFields(fields, draft) == fields &&
                store.writePdf(fields, draft) && store.readPdf() == draft,
            "corrected dimensions and uniform margins become eligible together");
    draft.marginLeftMm = 13;
    require(!ImageStitchingSettingsStore::persistablePdfFields({QStringLiteral("margin_mm")}, draft)
                    .contains(QStringLiteral("margin_mm")) &&
                !store.writePdf(QStringLiteral("margin_mm"), draft),
            "the uniform alias cannot silently persist an unequal margin tuple");
    draft.orientation = static_cast<ImageStitchingPdfOrientation>(-1);
    require(
        ImageStitchingSettingsStore::persistablePdfFields({QStringLiteral("orientation")}, draft)
                .isEmpty() &&
            !store.writePdf(QStringLiteral("orientation"), draft),
        "hidden invalid orientations cannot be serialized as a different valid preference");
    require(configuration.flushNow().success, "persist eligible PDF preferences to disk");
    storage::ConfigurationStore reopened(path, true, true, 60000);
    require(ImageStitchingSettingsStore(reopened).readPdf() == store.readPdf(),
            "only admitted PDF preferences survive a fresh configuration read");
}

void templateGeometryAndPagination() {
    struct Template final {
        ImageStitchingPdfTemplateType type;
        int capacity;
    };
    constexpr std::array templates = {
        Template{ImageStitchingPdfTemplateType::TwoImages, 2},
        Template{ImageStitchingPdfTemplateType::IdCard, 2},
        Template{ImageStitchingPdfTemplateType::ThreeImages, 3},
        Template{ImageStitchingPdfTemplateType::FourImages, 4},
        Template{ImageStitchingPdfTemplateType::FiveImages, 5},
        Template{ImageStitchingPdfTemplateType::SixImages, 6},
        Template{ImageStitchingPdfTemplateType::SevenImages, 7},
        Template{ImageStitchingPdfTemplateType::EightImages, 8},
        Template{ImageStitchingPdfTemplateType::NineImages, 9},
    };
    ImageStitchingDocument document;
    constexpr std::array sourceSizes{QSize(80, 20), QSize(20, 80), QSize(1, 4096), QSize(4096, 1),
                                     QSize(40, 40)};
    for (int index = 0; index < 19; ++index)
        require(document.append(fixture(sourceSizes[std::size_t(index) % sourceSizes.size()]),
                                QString::number(index)),
                "wide, tall, square and extreme-aspect template fixtures import");
    ImageStitchingPdfSettings settings;
    require(settings.stitchingMethod == ImageStitchingPdfStitchingMethod::PageBased &&
                settings.templateType == ImageStitchingPdfTemplateType::TwoImages &&
                settings.imageSpacingMm == 5 && imageStitchingPdfPageCount(19, settings) == 19,
            "first use defaults to page-based stitching and five millimeter spacing");
    settings.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
    for (const auto& item : templates) {
        settings.templateType = item.type;
        require(imageStitchingPdfTemplateCapacity(item.type) == item.capacity &&
                    imageStitchingPdfPageCount(0, settings) == 0 &&
                    imageStitchingPdfPageCount(item.capacity, settings) == 1 &&
                    imageStitchingPdfPageCount(item.capacity + 1, settings) == 2,
                "all template capacities produce composed page counts");
        for (const auto orientation :
             {ImageStitchingPdfOrientation::Portrait, ImageStitchingPdfOrientation::Landscape}) {
            settings.orientation = orientation;
            const auto page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
            require(page.isValid() && page.cells.size() == item.capacity,
                    "all templates produce valid portrait and landscape cells");
            const bool landscape = orientation == ImageStitchingPdfOrientation::Landscape;
            require(page.pageMillimeters == (landscape ? QSizeF(297, 210) : QSizeF(210, 297)),
                    "template orientation controls paper dimensions");
            for (qsizetype index = 0; index < page.cells.size(); ++index) {
                const auto& cell = page.cells[index];
                require(
                    cell.itemId == document.items()[index].id && cell.itemIndex == index &&
                        page.contentMillimeters.adjusted(-0.000001, -0.000001, 0.000001, 0.000001)
                            .contains(cell.clipMillimeters),
                    "cell assignment follows input order and remains within margins");
                require(
                    near(cell.imageMillimeters.center().x(), cell.clipMillimeters.center().x()) &&
                        near(cell.imageMillimeters.center().y(),
                             cell.clipMillimeters.center().y()) &&
                        cell.clipMillimeters.adjusted(-0.000001, -0.000001, 0.000001, 0.000001)
                            .contains(cell.imageMillimeters) &&
                        (near(cell.imageMillimeters.width(), cell.clipMillimeters.width()) ||
                         near(cell.imageMillimeters.height(), cell.clipMillimeters.height())) &&
                        near(cell.imageMillimeters.width() / cell.imageMillimeters.height(),
                             qreal(document.items()[index].image.width()) /
                                 document.items()[index].image.height()),
                    "automatic placement fits the entire image inside its cell at the largest "
                    "centered scale");
                if (item.type == ImageStitchingPdfTemplateType::IdCard)
                    require(near(cell.clipMillimeters.width() / cell.clipMillimeters.height(),
                                 85.60 / 53.98) &&
                                near(cell.clipMillimeters.width(), 210 * 0.41),
                            "ID cards remain landscape and use the shorter-side width baseline");
            }
            const qsizetype last =
                imageStitchingPdfPageCount(document.items().size(), settings) - 1;
            const auto finalPage =
                calculateImageStitchingPdfComposedPage(document.items(), settings, last);
            require(finalPage.isValid() && finalPage.cells.size() == item.capacity,
                    "the final page retains the complete selected template");
            for (qsizetype index = 0; index < finalPage.cells.size(); ++index) {
                const auto& cell = finalPage.cells[index];
                const qsizetype source = last * item.capacity + index;
                require(source < document.items().size()
                            ? cell.itemId == document.items()[source].id && cell.itemIndex == source
                            : cell.itemId == 0 && cell.itemIndex == -1 &&
                                  cell.imageMillimeters.isEmpty(),
                        "only unused final-page cells stay blank");
            }
        }
    }
    settings.templateType = ImageStitchingPdfTemplateType::FourImages;
    settings.orientation = ImageStitchingPdfOrientation::Portrait;
    auto page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    require(page.cells[0].clipMillimeters == QRectF(10, 10, 92.5, 136) &&
                page.cells[1].clipMillimeters == QRectF(107.5, 10, 92.5, 136) &&
                page.cells[2].clipMillimeters == QRectF(10, 151, 92.5, 136),
            "regular grids fill rows and deduct spacing before dividing sections evenly");
    settings.templateType = ImageStitchingPdfTemplateType::FiveImages;
    page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    require(
        near(page.cells[0].clipMillimeters.height(), 136) &&
            near(page.cells[1].clipMillimeters.height(), 89) &&
            page.cells[2].clipMillimeters.left() == page.cells[0].clipMillimeters.left() &&
            page.cells[3].clipMillimeters.left() == page.cells[1].clipMillimeters.left() &&
            page.cells[4].clipMillimeters.left() == page.cells[1].clipMillimeters.left(),
        "five-image order alternates left and right sections before filling the final right cell");
    settings.orientation = ImageStitchingPdfOrientation::Landscape;
    page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    require(page.cells[0].clipMillimeters == QRectF(10, 10, 136, 92.5) &&
                page.cells[1].clipMillimeters == QRectF(10, 107.5, 89, 92.5) &&
                page.cells[2].clipMillimeters == QRectF(151, 10, 136, 92.5),
            "landscape transposes both arrangements and alternating input order");
    settings.templateType = ImageStitchingPdfTemplateType::SixImages;
    page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    require(page.cells[1].clipMillimeters.topLeft() == QPointF(104, 10) &&
                page.cells[2].clipMillimeters.topLeft() == QPointF(198, 10) &&
                page.cells[3].clipMillimeters.topLeft() == QPointF(10, 107.5),
            "landscape six-image grid fills left to right before its second row");
    settings.templateType = ImageStitchingPdfTemplateType::NineImages;
    page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    require(page.cells[1].clipMillimeters.topLeft() == QPointF(104, 10) &&
                page.cells[2].clipMillimeters.topLeft() == QPointF(198, 10) &&
                page.cells[3].clipMillimeters.topLeft() == QPointF(10, 75),
            "landscape nine-image grid retains left-to-right top-to-bottom input order");
    settings.templateType = ImageStitchingPdfTemplateType::SevenImages;
    page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    require(page.cells[0].clipMillimeters.topLeft() == QPointF(10, 10) &&
                page.cells[1].clipMillimeters.topLeft() == QPointF(10, 107.5) &&
                page.cells[2].clipMillimeters.topLeft() == QPointF(104, 10) &&
                page.cells[6].clipMillimeters.topLeft() == QPointF(221.5, 107.5),
            "landscape seven-image layout alternates top and bottom before its last bottom cell");
    settings.paperSize = ImageStitchingPdfPaperSize::ImageSize;
    require(calculateImageStitchingPdfComposedPage(document.items(), settings, 0).pageMillimeters ==
                    QSizeF(297, 210) &&
                settings.paperSize == ImageStitchingPdfPaperSize::ImageSize,
            "template Image Size uses oriented A4 without changing the preference");
    settings.paperSize = ImageStitchingPdfPaperSize::Custom;
    settings.customWidthMm = 50;
    settings.customHeightMm = 70;
    settings.marginLeftMm = settings.marginRightMm = settings.marginTopMm =
        settings.marginBottomMm = 5;
    settings.marginLeftMm = settings.marginRightMm = 20;
    settings.templateType = ImageStitchingPdfTemplateType::IdCard;
    page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    require(page.isValid() && near(page.cells[0].clipMillimeters.width(), 12.5) &&
                near(page.cells[0].clipMillimeters.width() / page.cells[0].clipMillimeters.height(),
                     85.60 / 53.98),
            "ID cards shrink within small assigned sections while preserving their aspect ratio");
    settings.imageSpacingMm = 100;
    require(
        validateImageStitchingPdfSettings(settings).contains(QStringLiteral("image_spacing_mm")) &&
            !calculateImageStitchingPdfComposedPage(document.items(), settings, 0).isValid(),
        "spacing that consumes assigned sections is invalid");
    settings.imageSpacingMm = -1;
    require(
        validateImageStitchingPdfSettings(settings).contains(QStringLiteral("image_spacing_mm")),
        "negative template spacing is invalid");
    settings.imageSpacingMm = 5;
    require(!calculateImageStitchingPdfComposedPage(document.items(), settings, -1).isValid() &&
                !calculateImageStitchingPdfComposedPage(document.items(), settings, 100).isValid(),
            "invalid page indices cannot produce compositions");
    require(imageStitchingPdfPageCount(std::numeric_limits<qsizetype>::max(), settings) ==
                std::numeric_limits<qsizetype>::max() / 2 + 1,
            "page counting avoids overflow for large input counts");
}

void templatePlacementAndTransforms() {
    ImageStitchingDocument document;
    const auto first = document.append(fixture({80, 20}), QStringLiteral("wide"));
    const auto second = document.append(fixture({20, 80}), QStringLiteral("tall"));
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
    auto settings = document.pdfSettings();
    settings.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
    document.setPdfSettings(settings);
    auto page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    const qreal initialScale = page.cells[0].imageMillimeters.width() / 80;
    const QPointF initialCenter = page.cells[0].imageMillimeters.center();
    require(document.rotate(first, true), "rotate a centered-fit template image");
    page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    require(near(*document.items()[0].pdfPlacement.millimetersPerPixel, initialScale) &&
                near(page.cells[0].imageMillimeters.width(), 20 * initialScale) &&
                near(page.cells[0].imageMillimeters.height(), 80 * initialScale) &&
                page.cells[0].imageMillimeters.center() == initialCenter,
            "rotation freezes physical pixel scale and preserves the center rather than "
            "automatically refitting");
    const ImageStitchingPdfPlacement placement{{12.5, -8.25}, 0.25};
    int contentChanges = 0;
    int placementChanges = 0;
    QObject::connect(&document, &ImageStitchingDocument::changed, &document,
                     [&] { ++contentChanges; });
    QObject::connect(&document, &ImageStitchingDocument::pdfPlacementChanged, &document,
                     [&](quint64 id) {
                         require(id == first, "placement signal identifies its image");
                         ++placementChanges;
                     });
    const auto revision = document.contentRevision();
    const auto itemRevision = document.items()[0].revision;
    require(
        document.setPdfPlacement(first, placement) && placementChanges == 1 &&
            contentChanges == 0 && document.contentRevision() == revision &&
            document.items()[0].revision == itemRevision &&
            !document.setPdfPlacement(first, placement),
        "placement updates have their own notification without invalidating image content caches");
    page = calculateImageStitchingPdfComposedPage(document.items(), settings, 0);
    require(page.cells[0].imageMillimeters.size() == QSizeF(5, 20) &&
                page.cells[0].imageMillimeters.center() ==
                    page.cells[0].clipMillimeters.center() + placement.centerOffsetMillimeters,
            "absolute scale and millimeter offsets allow whitespace within the cell");
    const QRectF beforeFlip = page.cells[0].imageMillimeters;
    require(document.flip(first, true) &&
                calculateImageStitchingPdfComposedPage(document.items(), settings, 0)
                        .cells[0]
                        .imageMillimeters == beforeFlip,
            "flipping keeps physical position and scale");
    require(
        !document.setPdfPlacement(second, {{0, 0}, 0}) &&
            !document.setPdfPlacement(second, {{0, 0}, std::numeric_limits<qreal>::infinity()}) &&
            !document.setPdfPlacement(second, {{std::numeric_limits<qreal>::quiet_NaN(), 0}, 1}),
        "invalid placement values are rejected");
    const auto rotatedTransform = document.items()[0].transform;
    settings.stitchingMethod = ImageStitchingPdfStitchingMethod::PageBased;
    document.setPdfSettings(settings);
    settings.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
    document.setPdfSettings(settings);
    require(document.items()[0].pdfPlacement == placement,
            "switching methods preserves template placement when effective paper geometry is "
            "unchanged");
    settings.exportMode = ImageStitchingPdfExportMode::PerPage;
    document.setPdfSettings(settings);
    require(document.items()[0].pdfPlacement == placement,
            "changing export mode preserves placement");
    auto invalid = settings;
    invalid.imageSpacingMm = std::numeric_limits<qreal>::quiet_NaN();
    document.setPdfSettings(invalid);
    require(document.items()[0].pdfPlacement == placement &&
                document.lastValidPdfSettings() == settings,
            "incomplete settings drafts preserve the last valid placement and geometry");
    settings.imageSpacingMm = 7.25;
    document.setPdfSettings(settings);
    require(document.items()[0].pdfPlacement == ImageStitchingPdfPlacement{} &&
                document.items()[0].transform == rotatedTransform,
            "valid geometry changes reset placement while preserving orientation");
    require(document.resetTransform(first) && document.items()[0].transform.isIdentity(),
            "reset restores original orientation");
    require(document.setPdfPlacement(first, placement), "move an identity-oriented image");
    const auto beforeReset = document.contentRevision();
    require(document.resetTransform(first) &&
                document.items()[0].pdfPlacement == ImageStitchingPdfPlacement{} &&
                document.contentRevision() == beforeReset,
            "reset restores centered fit even when orientation was already original");
}

void templateSequencePlacementReset() {
    ImageStitchingDocument document;
    QVector<quint64> ids;
    for (int index = 0; index < 5; ++index)
        ids.append(document.append(fixture({30, 20}), QString::number(index)));
    auto settings = document.pdfSettings();
    settings.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
    document.setPdfSettings(settings);
    const ImageStitchingPdfPlacement placement{{3, 4}, 0.5};
    const auto placeAll = [&] {
        for (const auto id : ids)
            if (document.indexOf(id) >= 0)
                document.setPdfPlacement(id, placement);
    };
    const auto placementOf = [&](quint64 id) {
        return document.items()[document.indexOf(id)].pdfPlacement;
    };
    placeAll();
    require(document.swapItems(ids[0], ids[4]) &&
                placementOf(ids[0]) == ImageStitchingPdfPlacement{} &&
                placementOf(ids[4]) == ImageStitchingPdfPlacement{} &&
                placementOf(ids[2]) == placement && document.indexOf(ids[0]) == 4 &&
                document.indexOf(ids[4]) == 0,
            "swapping resets only exchanged placements and updates indexed lookups");
    placeAll();
    require(document.reverseItems() && placementOf(ids[2]) == placement &&
                placementOf(ids[0]) == ImageStitchingPdfPlacement{} &&
                placementOf(ids[4]) == ImageStitchingPdfPlacement{},
            "reversal retains only an unchanged middle image placement");
    placeAll();
    const auto firstId = document.items()[0].id;
    const auto retainedId = document.items()[1].id;
    const auto removedId = document.items()[2].id;
    const auto shiftedId = document.items()[3].id;
    document.select(shiftedId);
    require(document.remove(removedId) && document.indexOf(removedId) == -1 &&
                placementOf(firstId) == placement && placementOf(retainedId) == placement &&
                placementOf(shiftedId) == ImageStitchingPdfPlacement{} &&
                document.indexOf(shiftedId) == 2 && document.selectedId() == shiftedId,
            "deleting compacts all following slots, resets their placements, and preserves "
            "selection by ID");
    placeAll();
    require(document.moveItem(firstId, 2) && placementOf(firstId) == ImageStitchingPdfPlacement{} &&
                placementOf(retainedId) == ImageStitchingPdfPlacement{} &&
                placementOf(shiftedId) == ImageStitchingPdfPlacement{},
            "moving resets the complete shifted interval");
    placeAll();
    const auto lastId = document.items().back().id;
    require(document.append(fixture({10, 10}), QStringLiteral("append")) &&
                placementOf(lastId) == placement,
            "append preserves placement in existing slots");
    require(document.replace(lastId, fixture({20, 10}), QStringLiteral("replacement")) &&
                placementOf(lastId) == ImageStitchingPdfPlacement{},
            "replacing an image restores centered fit in its existing slot");
    document.clear();
    require(document.indexOf(firstId) == -1 && document.indexOf(lastId) == -1,
            "clear removes every indexed image ID");
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
        templateGeometryAndPagination();
        templatePlacementAndTransforms();
        templateSequencePlacementReset();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
