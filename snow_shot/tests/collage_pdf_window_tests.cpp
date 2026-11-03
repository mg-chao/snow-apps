// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagewindow.h"
#include "snow_shot/presentation/collagesettingsstore.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/button.h"
#include "widgets/input.h"
#include "widgets/message.h"
#include "widgets/modal.h"
#include "widgets/segmented.h"
#include "widgets/select.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QLabel>
#include <QLocale>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <iostream>
#include <stdexcept>

using namespace snow_shot::presentation;
using adqt::widgets::AdButton;
using adqt::widgets::AdLineEdit;
using adqt::widgets::AdSegmented;
using adqt::widgets::AdSelect;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void flush() {
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void waitUntil(const std::function<bool()>& ready, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 10000) {
        flush();
        QThread::msleep(1);
    }
    require(ready(), message);
}

template <class T> T* control(CollageWindow& window, const char* name) {
    auto* result = window.editor()->window()->findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, "the expected window control is discoverable");
    return result;
}

QImage source(QColor color = Qt::red) {
    QImage image({25, 61}, QImage::Format_RGBA8888);
    image.fill(color);
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

void prepare(CollageWindow& window) {
    window.present();
    window.document()->append(source(), QStringLiteral("first.png"));
    window.document()->append(source(Qt::blue), QStringLiteral("second.png"));
    window.document()->setOutputType(CollageOutputType::PdfDocument);
    auto settings = CollagePdfSettings{};
    window.document()->setPdfSettings(settings);
    flush();
}

QString status(CollageWindow& window) {
    auto* service = adqt::widgets::AdMessageService::instance(window.editor()->window());
    for (auto* message : service->findChildren<adqt::widgets::AdMessageHandle*>())
        if (message->isOpen() && message->key() == QStringLiteral("collageStatus"))
            return message->content();
    return {};
}

void choosingLocksAndCancelPreservesContent() {
    int choices = 0;
    int writes = 0;
    int completions = 0;
    CollageWindowServices::PdfChoice choice;
    CollageWindowServices services;
    services.choosePdfDestination = [&](QWidget*, CollagePdfExportMode mode, auto completion) {
        require(mode == CollagePdfExportMode::Combined, "combined export is the default");
        ++choices;
        choice = std::move(completion);
    };
    services.exportPdf = [&](const auto&, const auto&, const auto&, auto, auto) {
        ++writes;
        return CollagePdfExportResult{};
    };
    CollageWindow window(nullptr, services);
    prepare(window);
    const auto items = window.document()->items();
    QObject::connect(&window, &CollageWindow::pdfExportFinished, &window, [&](const auto& result) {
        require(result.cancelled && !result.succeeded(),
                "chooser cancellation reports one cancelled result");
        ++completions;
    });
    require(!control<AdButton>(window, "collageCopy")->isVisible(), "PDF hides Copy");
    window.copy();
    require(!window.busy() && writes == 0, "programmatic Copy cannot export PDF mode");
    window.save();
    require(window.busy() && choices == 1 &&
                !control<AdButton>(window, "collageSave")->isEnabled() &&
                !control<AdSegmented>(window, "collageImageType")->isEnabled(),
            "destination selection locks save, settings and mode switching");
    window.save();
    window.addFiles({QStringLiteral("ignored.png")});
    window.requestClear();
    require(choices == 1 && window.document()->items().size() == 2 && !window.loading(),
            "duplicate submissions and content operations are blocked while choosing");
    window.requestClose();
    require(!window.busy() && window.modal()->isOpen() && completions == 1 && writes == 0,
            "Cancel during destination selection returns to the open editor");
    choice(CollagePdfDestination{QStringLiteral("late.pdf"), false});
    flush();
    require(writes == 0 && completions == 1 &&
                window.document()->items().front().id == items.front().id,
            "late chooser callbacks cannot start or duplicate canceled export");
    window.shutdown();
}

void pendingClearConfirmationBlocksExport() {
    std::function<void(bool)> confirm;
    int choices = 0;
    CollageWindowServices services;
    services.confirm = [&](QWidget*, bool clear, auto complete) {
        require(clear, "the pending prompt is a Clear confirmation");
        confirm = std::move(complete);
    };
    services.choosePdfDestination = [&](QWidget*, CollagePdfExportMode, auto) { ++choices; };
    CollageWindow window(nullptr, services);
    prepare(window);
    const auto settings = window.document()->pdfSettings();
    window.requestClear();
    window.save();
    require(confirm && choices == 0 && !window.busy() &&
                !control<AdButton>(window, "collageSave")->isEnabled(),
            "an outstanding Clear confirmation blocks both UI and programmatic export");
    confirm(false);
    require(window.document()->items().size() == 2 &&
                control<AdButton>(window, "collageSave")->isEnabled(),
            "canceling Clear restores Save without modifying the document");
    auto firstConfirmation = confirm;
    window.requestClear();
    firstConfirmation(true);
    require(window.document()->items().size() == 2,
            "a duplicate old confirmation cannot approve a newer Clear request");
    confirm(true);
    require(window.document()->items().isEmpty() && window.document()->pdfSettings() == settings,
            "confirmed Clear retains PDF form settings");
    window.shutdown();
}

void cancellationWaitsForCleanup() {
    const auto started = std::make_shared<std::atomic_bool>(false);
    const auto cancellationObserved = std::make_shared<std::atomic_bool>(false);
    const auto release = std::make_shared<std::atomic_bool>(false);
    CollageWindowServices services;
    services.choosePdfDestination = [](QWidget*, CollagePdfExportMode, auto complete) {
        complete(CollagePdfDestination{QStringLiteral("unused.pdf"), false});
    };
    services.exportPdf = [started, cancellationObserved, release](const auto& items, const auto&,
                                                                  const auto&, auto cancelled,
                                                                  auto progress) {
        progress({0, items.size(), 1, .25});
        started->store(true);
        while (!cancelled())
            QThread::msleep(1);
        cancellationObserved->store(true);
        while (!release->load())
            QThread::msleep(1);
        CollagePdfExportResult result;
        result.totalCount = items.size();
        result.cancelled = true;
        return result;
    };
    CollageWindow window(nullptr, services);
    prepare(window);
    window.document()->rotate(window.document()->items().front().id, true);
    const auto transform = window.document()->items().front().transform;
    int completions = 0;
    QObject::connect(&window, &CollageWindow::pdfExportFinished, &window, [&](const auto& result) {
        require(window.busy() && result.cancelled,
                "completion is delivered exactly once before unlocking");
        ++completions;
    });
    window.save();
    waitUntil([&] { return started->load(); }, "PDF worker starts");
    window.requestClose();
    waitUntil([&] { return cancellationObserved->load(); }, "worker receives cancellation");
    require(window.busy() && window.modal()->isOpen() && completions == 0 &&
                !control<AdButton>(window, "collageSave")->isEnabled() &&
                !control<AdButton>(window, "collageCancel")->isEnabled(),
            "editing stays locked until cancellation cleanup completes");
    release->store(true);
    waitUntil([&] { return !window.busy(); }, "cleanup returns to editing");
    require(completions == 1 && window.modal()->isOpen() &&
                window.document()->items().front().transform == transform &&
                control<AdButton>(window, "collageSave")->isEnabled(),
            "cancellation preserves originals/transforms and enables retry");
    window.shutdown();
}

void partialFailureReportsAndRetryCloses() {
    const auto attempts = std::make_shared<std::atomic_int>(0);
    CollageWindowServices services;
    services.choosePdfDestination = [](QWidget*, CollagePdfExportMode mode, auto complete) {
        require(mode == CollagePdfExportMode::PerPage, "per-page choice uses a folder");
        complete(CollagePdfDestination{QStringLiteral("/output"), false});
    };
    services.exportPdf = [attempts](const auto& items, const auto&, const auto&, auto,
                                    auto progress) {
        CollagePdfExportResult result;
        result.totalCount = items.size();
        result.completedPages = {1};
        result.paths = {QStringLiteral("/output/001_second.pdf")};
        progress({1, items.size(), 2, .5});
        if (attempts->fetch_add(1) == 0)
            result.error = QStringLiteral("Disk fixture failed");
        else {
            result.completedPages.append(2);
            result.paths.append(QStringLiteral("/output/002_first.pdf"));
        }
        return result;
    };
    CollageWindow window(nullptr, services);
    prepare(window);
    auto settings = window.document()->pdfSettings();
    settings.exportMode = CollagePdfExportMode::PerPage;
    window.document()->setPdfSettings(settings);
    window.document()->moveItem(window.document()->items().front().id, 1);
    int completed = 0;
    QObject::connect(&window, &CollageWindow::pdfExportFinished, &window,
                     [&](const auto&) { ++completed; });
    window.save();
    waitUntil([&] { return !window.busy(); }, "partial export completes");
    require(window.modal()->isOpen() && completed == 1 && window.document()->items().size() == 2 &&
                status(window).contains(QStringLiteral("Saved pages: 1")) &&
                status(window).contains(QStringLiteral("001_second.pdf")) &&
                status(window).contains(QStringLiteral("Page 2 was not saved")),
            "partial failure reports actual completed files and leaves the editor open");
    window.save();
    waitUntil([&] { return !window.modal()->isOpen(); }, "successful retry closes the window");
    require(completed == 2 && attempts->load() == 2, "retry completes exactly once");
}

bool uniformMargins(const CollagePdfSettings& settings, qreal value) {
    return settings.marginTopMm == value && settings.marginBottomMm == value &&
           settings.marginLeftMm == value && settings.marginRightMm == value;
}

void invalidSettingsAndPreferencePersistence() {
    int choices = 0;
    CollageWindowServices services;
    services.choosePdfDestination = [&](QWidget*, CollagePdfExportMode, auto) { ++choices; };
    CollageWindow first(nullptr, services);
    prepare(first);
    auto* margin = control<AdLineEdit>(first, "collagePdfMargin");
    for (const auto& draft : {QStringLiteral("400"), QStringLiteral("105"), QStringLiteral("-0.5"),
                              QStringLiteral(""), QStringLiteral("invalid")}) {
        margin->setText(draft);
        require(margin->text() == draft && !control<AdButton>(first, "collageSave")->isEnabled(),
                "unusable, negative, empty and nonnumeric uniform margins disable Save without "
                "clamping");
        first.save();
        require(!first.busy() && choices == 0,
                "invalid uniform margins cannot open a destination chooser or start export");
    }
    margin->setText(QStringLiteral("0"));
    const auto borderless =
        calculateCollagePdfPageLayout(source().size(), first.document()->pdfSettings());
    require(control<AdButton>(first, "collageSave")->isEnabled() &&
                uniformMargins(first.document()->pdfSettings(), 0) &&
                borderless.contentMillimeters == QRectF(QPointF{}, borderless.pageMillimeters),
            "zero uniform margin enables Save and exposes the complete paper area");
    margin->setText(QStringLiteral("15.25"));
    require(control<AdButton>(first, "collageSave")->isEnabled() &&
                uniformMargins(first.document()->pdfSettings(), 15.25),
            "one corrected decimal margin updates every side and enables Save promptly");
    auto* paper = control<AdSelect>(first, "collagePdfPaperSize");
    paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::Custom));
    control<AdLineEdit>(first, "collagePdfCustomWidth")->clear();
    require(!control<AdButton>(first, "collageSave")->isEnabled(),
            "empty custom dimensions disable Save");
    paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::A4));
    margin->setText(QStringLiteral("16.75"));
    {
        CollageWindow standard;
        standard.present();
        require(standard.document()->pdfSettings().paperSize == CollagePdfPaperSize::A4 &&
                    uniformMargins(standard.document()->pdfSettings(), 16.75),
                "inactive custom drafts cannot prevent the uniform margin and active paper "
                "preference being saved");
        standard.shutdown();
    }
    require(control<AdLineEdit>(first, "collagePdfCustomWidth")->text().isEmpty(),
            "remembering active settings preserves the hidden custom draft");
    paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::Custom));
    margin->setText(QStringLiteral("15.25"));
    control<AdSegmented>(first, "collageImageType")->setCurrentValue(QStringLiteral("static"));
    require(control<AdButton>(first, "collageCopy")->isVisible() &&
                control<AdButton>(first, "collageSave")->isEnabled(),
            "PDF validation cannot block valid static output");
    control<AdSegmented>(first, "collageImageType")->setCurrentValue(QStringLiteral("pdf"));
    require(control<AdLineEdit>(first, "collagePdfCustomWidth")->text().isEmpty() &&
                margin->text() == QStringLiteral("15.25") &&
                !control<AdButton>(first, "collageSave")->isEnabled(),
            "mode switching preserves incomplete paper input and the shared margin");
    control<AdLineEdit>(first, "collagePdfCustomWidth")->setText(QStringLiteral("180.5"));
    CollageWindow reopened;
    reopened.present();
    require(
        reopened.document()->outputType() == CollageOutputType::StaticImage &&
            reopened.document()->pdfSettings().paperSize == CollagePdfPaperSize::Custom &&
            reopened.document()->pdfSettings().customWidthMm == 180.5 &&
            uniformMargins(reopened.document()->pdfSettings(), 15.25),
        "valid paper preferences and the uniform margin persist while new windows start static");
    control<AdLineEdit>(first, "collagePdfCustomWidth")->setText(QStringLiteral("186.75"));
    reopened.document()->setOutputType(CollageOutputType::PdfDocument);
    control<AdLineEdit>(reopened, "collagePdfMargin")->setText(QStringLiteral("22.375"));
    {
        CollageWindow merged;
        merged.present();
        require(merged.document()->pdfSettings().customWidthMm == 186.75 &&
                    uniformMargins(merged.document()->pdfSettings(), 22.375) &&
                    uniformMargins(first.document()->pdfSettings(), 15.25) &&
                    reopened.document()->pdfSettings().customWidthMm == 180.5,
                "an independent window's uniform margin preserves newer paper dimensions and each "
                "open form");
        merged.shutdown();
    }
    reopened.shutdown();
    first.shutdown();
}

void uniformMarginReachesPdfExport() {
    const auto received = std::make_shared<std::atomic_bool>(false);
    CollageWindowServices services;
    services.choosePdfDestination = [](QWidget*, CollagePdfExportMode, auto completion) {
        completion(CollagePdfDestination{QStringLiteral("/output/uniform.pdf"), false});
    };
    services.exportPdf = [received](const auto& items, const auto& settings, const auto&, auto,
                                    auto) {
        received->store(uniformMargins(settings, 17.125));
        CollagePdfExportResult result;
        result.totalCount = items.size();
        for (qsizetype index = 0; index < items.size(); ++index)
            result.completedPages.append(index + 1);
        result.paths = {QStringLiteral("/output/uniform.pdf")};
        return result;
    };
    CollageWindow window(nullptr, services);
    prepare(window);
    control<AdLineEdit>(window, "collagePdfMargin")->setText(QStringLiteral("17.125"));
    window.save();
    waitUntil([&] { return !window.busy(); }, "uniform-margin export completes");
    require(
        received->load() && !window.modal()->isOpen(),
        "PDF export receives the same shared margin on all four sides and closes after success");
}

void imageSizeRemembersPreferencesWithInactiveDrafts() {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    CollageSettingsStore store(configuration);
    const QStringList fields{QStringLiteral("paper_size"),      QStringLiteral("orientation"),
                             QStringLiteral("custom_width_mm"), QStringLiteral("custom_height_mm"),
                             QStringLiteral("margin_mm"),       QStringLiteral("export_mode")};
    CollagePdfSettings baseline;
    baseline.marginTopMm = baseline.marginBottomMm = baseline.marginLeftMm =
        baseline.marginRightMm = 16.75;
    for (const auto& draft : {QStringLiteral(""), QStringLiteral("unfinished"),
                              QStringLiteral("-0.5"), QStringLiteral("400")}) {
        require(store.writePdf(fields, baseline), "reset the PDF preferences for each draft");
        CollageWindow window;
        prepare(window);
        auto* paper = control<AdSelect>(window, "collagePdfPaperSize");
        auto* margin = control<AdLineEdit>(window, "collagePdfMargin");
        auto* width = control<AdLineEdit>(window, "collagePdfCustomWidth");
        auto* mode = control<AdSegmented>(window, "collagePdfExportMode");
        width->setText(QStringLiteral("unfinished"));
        margin->setText(draft);
        paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::ImageSize));
        mode->setCurrentValue(static_cast<int>(CollagePdfExportMode::PerPage));
        require(control<AdButton>(window, "collageSave")->isEnabled() && margin->text() == draft &&
                    width->text() == QStringLiteral("unfinished") && status(window).isEmpty(),
                "inactive drafts neither block Image Size export nor produce persistence errors");
        {
            CollageWindow reopened;
            const auto settings = reopened.document()->pdfSettings();
            require(settings.paperSize == CollagePdfPaperSize::ImageSize &&
                        settings.exportMode == CollagePdfExportMode::PerPage &&
                        settings.customWidthMm == baseline.customWidthMm &&
                        uniformMargins(settings, draft == QStringLiteral("400") ? 400 : 16.75),
                    "Image Size and export mode persist while invalid hidden fields retain their "
                    "previous stored values");
            reopened.shutdown();
        }
        paper->setCurrentValue(static_cast<int>(CollagePdfPaperSize::A4));
        require(!control<AdButton>(window, "collageSave")->isEnabled() && margin->text() == draft,
                "returning to paper sizing restores validation of the untouched margin draft");
        mode->setCurrentValue(static_cast<int>(CollagePdfExportMode::Combined));
        margin->setText(QStringLiteral("11.625"));
        require(store.readPdf().paperSize == CollagePdfPaperSize::A4 &&
                    store.readPdf().exportMode == CollagePdfExportMode::Combined &&
                    uniformMargins(store.readPdf(), 11.625) &&
                    width->text() == QStringLiteral("unfinished"),
                "correcting the margin persists deferred active edits without losing another "
                "inactive draft");
        window.shutdown();
    }
}

void imageSizeFooterTracksSelectedPage() {
    CollageWindow window;
    window.present();
    window.editor()->setLocale(QLocale::c());
    auto& document = *window.document();
    QImage tall({960, 1920}, QImage::Format_RGB32);
    tall.fill(Qt::red);
    QImage wide({192, 96}, QImage::Format_RGB32);
    wide.fill(Qt::blue);
    const auto first = document.append(tall, QStringLiteral("tall.png"));
    const auto second = document.append(wide, QStringLiteral("wide.png"));
    require(first && second, "image-sized footer sources import");
    document.setOutputType(CollageOutputType::PdfDocument);
    CollagePdfSettings settings;
    settings.paperSize = CollagePdfPaperSize::ImageSize;
    document.setPdfSettings(settings);
    auto* dimensions = control<QLabel>(window, "collageOutputDimensions");
    auto* total = control<QLabel>(window, "collageImageTotal");
    require(dimensions->isVisible() && dimensions->text() == QStringLiteral("254 × 508 mm") &&
                total->text() == QStringLiteral("Total: 2 pages"),
            "Image Size describes the selected original at 96 DPI instead of a placeholder");
    document.select(second);
    require(dimensions->text() == QStringLiteral("50.8 × 25.4 mm"),
            "selecting another page updates its physical dimensions immediately");
    require(
        document.rotate(second, true) && dimensions->text() == QStringLiteral("25.4 × 50.8 mm") &&
            document.moveItem(second, 0) && dimensions->text() == QStringLiteral("25.4 × 50.8 mm"),
        "image-sized dimensions follow transforms and preserve the selected page on reorder");
    document.select(0);
    require(dimensions->isHidden() && dimensions->text().isEmpty() &&
                total->text() == QStringLiteral("Total: 2 pages"),
            "image-sized output hides dimensions when no page is selected");
    document.select(first);
    require(dimensions->text() == QStringLiteral("254 × 508 mm") && document.remove(first) &&
                dimensions->text() == QStringLiteral("25.4 × 50.8 mm"),
            "removing the selected page describes its surviving selected neighbor");
    settings.paperSize = CollagePdfPaperSize::A4;
    settings.orientation = CollagePdfOrientation::Landscape;
    document.setPdfSettings(settings);
    document.select(0);
    require(dimensions->isVisible() && dimensions->text() == QStringLiteral("297 × 210 mm"),
            "standard paper describes the document independently of page selection");
    settings.marginTopMm = 300;
    document.setPdfSettings(settings);
    require(dimensions->text() == QStringLiteral("297 × 210 mm"),
            "invalid paper drafts keep the dimensions of the last valid preview");
    document.setOutputType(CollageOutputType::StaticImage);
    require(dimensions->text().endsWith(QStringLiteral(" px")),
            "returning to static output restores pixel dimensions");
    document.setOutputType(CollageOutputType::PdfDocument);
    require(dimensions->text() == QStringLiteral("297 × 210 mm"),
            "returning to PDF restores physical dimensions");
    settings.paperSize = CollagePdfPaperSize::ImageSize;
    document.setPdfSettings(settings);
    document.clear();
    require(dimensions->isHidden() && dimensions->text().isEmpty() &&
                total->text() == QStringLiteral("Total: 0 pages"),
            "an empty image-sized document never displays fictional page dimensions");
    window.shutdown();
}

void nativeDestinationDialog() {
    CollageWindow window;
    prepare(window);
    window.save();
    auto* dialog = window.editor()->window()->findChild<QFileDialog*>();
    require(dialog && dialog->acceptMode() == QFileDialog::AcceptSave &&
                dialog->defaultSuffix() == QStringLiteral("pdf") &&
                dialog->nameFilters().first().contains(QStringLiteral("*.pdf")) &&
                !dialog->testOption(QFileDialog::DontConfirmOverwrite),
            "combined native dialog restricts PDF output and confirms overwrites");
    dialog->reject();
    flush();
    require(!window.busy() && window.modal()->isOpen(), "dialog rejection preserves the document");
    auto settings = window.document()->pdfSettings();
    settings.exportMode = CollagePdfExportMode::PerPage;
    window.document()->setPdfSettings(settings);
    window.save();
    dialog = window.editor()->window()->findChild<QFileDialog*>();
    require(dialog && dialog->fileMode() == QFileDialog::Directory,
            "per-page native dialog selects a directory");
    dialog->reject();
    flush();
    window.shutdown();
}

void templatePreferencesReopenAndMerge() {
    CollageSettingsStore store(storage::ApplicationStorage::instance().configuration());
    const auto previous = store.readPdf();
    CollageWindow first;
    first.present();
    control<AdSegmented>(first, "collageImageType")->setCurrentValue(QStringLiteral("pdf"));
    control<AdSelect>(first, "collagePdfStitchingMethod")
        ->setCurrentValue(static_cast<int>(CollagePdfStitchingMethod::TemplateBased));
    control<AdButton>(first, "collagePdfTemplateFiveImages")->click();
    control<AdLineEdit>(first, "collagePdfImageSpacing")->setText(QStringLiteral("6.375"));
    CollageWindow reopened;
    reopened.present();
    require(reopened.document()->pdfSettings().stitchingMethod ==
                    CollagePdfStitchingMethod::TemplateBased &&
                reopened.document()->pdfSettings().templateType ==
                    CollagePdfTemplateType::FiveImages &&
                reopened.document()->pdfSettings().imageSpacingMm == 6.375,
            "reopening restores the last stitching method, template and fractional spacing");
    control<AdButton>(first, "collagePdfTemplateNineImages")->click();
    control<AdLineEdit>(reopened, "collagePdfImageSpacing")->setText(QStringLiteral("3.125"));
    const auto merged = store.readPdf();
    require(merged.templateType == CollagePdfTemplateType::NineImages &&
                merged.imageSpacingMm == 3.125 &&
                first.document()->pdfSettings().imageSpacingMm == 6.375 &&
                reopened.document()->pdfSettings().templateType ==
                    CollagePdfTemplateType::FiveImages,
            "independent windows merge only their changed template preference fields");
    first.shutdown();
    reopened.shutdown();
    require(store.writePdf({QStringLiteral("stitching_method"), QStringLiteral("template_type"),
                            QStringLiteral("image_spacing_mm")},
                           previous),
            "restore isolated window template preferences");
}

void templateChooserCancellationCountsComposedPages() {
    int writes = 0;
    int completions = 0;
    CollageWindowServices::PdfChoice choice;
    CollageWindowServices services;
    services.choosePdfDestination = [&](QWidget*, CollagePdfExportMode, auto complete) {
        choice = std::move(complete);
    };
    services.exportPdf = [&](const auto&, const auto&, const auto&, auto, auto) {
        ++writes;
        return CollagePdfExportResult{};
    };
    CollageWindow window(nullptr, services);
    prepare(window);
    for (int index = 0; index < 5; ++index)
        require(window.document()->append(source(), QStringLiteral("extra-%1.png").arg(index)),
                "template cancellation sources import");
    auto settings = window.document()->pdfSettings();
    settings.stitchingMethod = CollagePdfStitchingMethod::TemplateBased;
    settings.templateType = CollagePdfTemplateType::FiveImages;
    settings.imageSpacingMm = 6.75;
    window.document()->setPdfSettings(settings);
    const auto selected = window.document()->items().front().id;
    const CollagePdfPlacement placement{{4.25, -3.5}, 0.375};
    require(window.document()->setPdfPlacement(selected, placement),
            "template cancellation fixture has a manual placement");
    QObject::connect(&window, &CollageWindow::pdfExportFinished, &window, [&](const auto& result) {
        require(result.cancelled && result.totalCount == 2 && result.completedPages.isEmpty() &&
                    result.paths.isEmpty(),
                "chooser cancellation counts composed pages rather than seven input images");
        ++completions;
    });
    window.save();
    require(window.busy() && bool(choice), "template export opens its destination chooser");
    const auto lateChoice = choice;
    window.requestClose();
    require(
        !window.busy() && completions == 1 && writes == 0 &&
            window.document()->items().front().pdfPlacement == placement,
        "Cancel during template destination selection preserves edits and reports composed pages");
    lateChoice(CollagePdfDestination{QStringLiteral("late-template.pdf"), false});
    flush();
    require(writes == 0 && completions == 1,
            "late template destination callbacks cannot revive canceled exports");
    window.save();
    require(window.busy(), "template export can retry after chooser cancellation");
    choice(std::nullopt);
    require(!window.busy() && completions == 2 && writes == 0 && window.modal()->isOpen(),
            "rejecting the template chooser reports one canceled composed-page result");
    window.shutdown();
}

void templateExportUsesComposedSnapshot() {
    struct Snapshot final {
        QVector<CollageItem> items;
        CollagePdfSettings settings;
        std::atomic_bool received{false};
        std::atomic_bool matches{false};
        std::atomic_bool release{false};
    };
    const auto snapshot = std::make_shared<Snapshot>();
    CollageWindowServices::PdfChoice choice;
    CollageWindowServices services;
    services.choosePdfDestination = [&](QWidget*, CollagePdfExportMode, auto complete) {
        choice = std::move(complete);
    };
    services.exportPdf = [snapshot](const auto& items, const auto& settings,
                                    const auto& destination, auto cancelled, auto progress) {
        bool matches = settings == snapshot->settings && items.size() == snapshot->items.size() &&
                       destination.path == QStringLiteral("/output/template-snapshot.pdf") &&
                       collagePdfPageCount(items.size(), settings) == 2;
        for (qsizetype index = 0; matches && index < items.size(); ++index) {
            const auto& item = items[index];
            const auto& expected = snapshot->items[index];
            matches = item.id == expected.id && item.transform == expected.transform &&
                      item.pdfPlacement == expected.pdfPlacement && item.label == expected.label &&
                      item.image.constBits() == expected.image.constBits();
        }
        const auto page = calculateCollagePdfComposedPage(items, settings, 0);
        const auto expectedPage =
            calculateCollagePdfComposedPage(snapshot->items, snapshot->settings, 0);
        matches =
            matches && page.isValid() && page.pageMillimeters == QSizeF(297, 210) &&
            page.cells.size() == 5 &&
            page.cells.front().imageMillimeters == expectedPage.cells.front().imageMillimeters &&
            page.cells.front().clipMillimeters == expectedPage.cells.front().clipMillimeters;
        snapshot->matches.store(matches);
        progress({1, 2, 2, .625});
        snapshot->received.store(true);
        while (!snapshot->release.load() && !cancelled())
            QThread::msleep(1);
        CollagePdfExportResult result;
        result.totalCount = 2;
        if (cancelled()) {
            result.cancelled = true;
        } else {
            result.completedPages = {1, 2};
            result.paths = {destination.path};
        }
        return result;
    };
    CollageWindow window(nullptr, services);
    prepare(window);
    for (int index = 0; index < 5; ++index)
        require(window.document()->append(source(), QStringLiteral("extra-%1.png").arg(index)),
                "template snapshot sources import");
    auto settings = window.document()->pdfSettings();
    settings.stitchingMethod = CollagePdfStitchingMethod::TemplateBased;
    settings.templateType = CollagePdfTemplateType::FiveImages;
    settings.imageSpacingMm = 7.25;
    settings.paperSize = CollagePdfPaperSize::ImageSize;
    settings.orientation = CollagePdfOrientation::Landscape;
    settings.marginTopMm = settings.marginBottomMm = settings.marginLeftMm =
        settings.marginRightMm = 14.5;
    window.document()->setPdfSettings(settings);
    const auto first = window.document()->items().front().id;
    require(window.document()->setPdfPlacement(first, {{4.25, -3.5}, 0.375}) &&
                window.document()->rotate(first, true) && window.document()->flip(first, false),
            "template snapshot includes position, scale, rotation and flip edits");
    snapshot->items = window.document()->items();
    snapshot->settings = settings;
    int completions = 0;
    QObject::connect(&window, &CollageWindow::pdfExportFinished, &window, [&](const auto& result) {
        require(result.succeeded() && result.totalCount == 2 &&
                    result.completedPages == QVector<qsizetype>({1, 2}),
                "template completion reports fully composed pages");
        ++completions;
    });
    window.save();
    require(window.busy() && bool(choice), "template snapshot is captured before choosing output");
    // Direct document changes stress the immutable export snapshot while the normal UI is locked.
    require(window.document()->setPdfPlacement(first, {{90, 80}, 1.25}) &&
                window.document()->reverseItems(),
            "later document edits differ from the pending snapshot");
    settings.templateType = CollagePdfTemplateType::NineImages;
    settings.imageSpacingMm = 3;
    window.document()->setPdfSettings(settings);
    require(collagePdfPageCount(window.document()->items().size(), settings) == 1,
            "the live document now has a different composed page count");
    choice(CollagePdfDestination{QStringLiteral("/output/template-snapshot.pdf"), false});
    waitUntil([&] { return snapshot->received.load(); },
              "template export worker receives its snapshot");
    require(snapshot->matches.load(), "export receives the chosen template, spacing and every "
                                      "original placement from its snapshot");
    waitUntil([&] { return status(window).contains(QStringLiteral("page 2 of 2")); },
              "template progress displays composed-page counts from the export snapshot");
    snapshot->release.store(true);
    waitUntil([&] { return !window.modal()->isOpen(); },
              "successful template snapshot export closes");
    require(completions == 1, "template snapshot export completes exactly once");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QTemporaryDir directory;
    try {
        auto& storage = storage::ApplicationStorage::instance();
        require(directory.isValid() &&
                    storage.initialize({directory.path(), directory.path(), 60000}).success,
                "temporary storage initializes without accessing user settings");
        styles::ThemeManager::instance().initialize(application);
        choosingLocksAndCancelPreservesContent();
        pendingClearConfirmationBlocksExport();
        cancellationWaitsForCleanup();
        partialFailureReportsAndRetryCloses();
        invalidSettingsAndPreferencePersistence();
        imageSizeRemembersPreferencesWithInactiveDrafts();
        imageSizeFooterTracksSelectedPage();
        uniformMarginReachesPdfExport();
        nativeDestinationDialog();
        templatePreferencesReopenAndMerge();
        templateChooserCancellationCountsComposedPages();
        templateExportUsesComposedSnapshot();
        waitUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
                  "window tests retire all workers");
        ScreenshotExportCoordinator::shared().shutdown();
        storage.shutdown();
        std::cout << "Collage PDF window tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
