// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitwindow.h"
#include "snow_shot/presentation/collagewindowcoordinator.h"
#include "snow_shot/presentation/collagesettingsstore.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/components/imagespliteditorwidget.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/button.h"
#include "widgets/input_number.h"
#include "widgets/modal.h"
#include "widgets/segmented.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QThread>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace snow_shot::presentation;
using adqt::widgets::AdButton;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void flush() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}
void until(const std::function<bool()>& ready, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 15000) {
        flush();
        QThread::msleep(1);
    }
    require(ready(), message);
}
QImage image(QSize size = {99, 84}) {
    QImage result(size, QImage::Format_RGBA64);
    result.setColorSpace(QColorSpace::DisplayP3);
    result.setDevicePixelRatio(2);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            result.setPixelColor(x, y, QColor(x % 256, y % 256, (x + y) % 256, 170));
    return result;
}
AdButton* button(ImageSplitWindow& window) {
    auto* result =
        window.editor()->window()->findChild<AdButton*>(QStringLiteral("imageSplitStitchToPdf"));
    require(result, "PDF handoff button is discoverable");
    return result;
}
void loaded(ImageSplitWindow& window) {
    until([&] { return !window.loading() && !window.document()->image().isNull(); },
          "split source loads");
}
ImageSplitSnapshot snapshot(QImage pixels, ImageSplitSettings settings = {}) {
    const auto layout = calculateImageSplitLayout(pixels.size(), settings);
    return {std::move(pixels), layout, QStringLiteral("pieces"), {}};
}
void typeCount(ImageSplitWindow& window, int key, const QString& text) {
    auto* number = window.editor()->findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("imageSplitCount"));
    auto* input = number ? number->findChild<QLineEdit*>() : nullptr;
    require(input, "count editor exists");
    input->setFocus();
    input->selectAll();
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier, text);
    QApplication::sendEvent(input, &event);
}

void footerDraftsAndTranslations() {
    QVector<ImageSplitSnapshot> requests;
    bool accepted = true;
    std::function<void(QString)> directoryChoice;
    ImageSplitWindowServices services;
    services.stitchToPdf = [&](ImageSplitSnapshot value) {
        requests.append(std::move(value));
        return accepted;
    };
    services.chooseDirectory = [&](QWidget*, auto done) { directoryChoice = std::move(done); };
    ImageSplitWindow window(nullptr, services);
    require(!button(window)->isEnabled(), "empty splitter disables PDF handoff");
    window.stitchToPdf();
    require(requests.isEmpty(), "empty splitter cannot submit a handoff");
    const auto pixels = image();
    window.presentSource({ScreenshotExportSource::fromImage(pixels),
                          QStringLiteral("source.png"),
                          {},
                          {},
                          pixels.size(),
                          ImageSplitDirection::Horizontal});
    require(!button(window)->isEnabled(), "loading splitter disables PDF handoff");
    loaded(window);
    flush();
    auto* direction = window.editor()->findChild<adqt::widgets::AdSegmented*>(
        QStringLiteral("imageSplitDirection"));
    require(direction && direction->currentValue() == QStringLiteral("horizontal") &&
                !window.document()->canUndo(),
            "initial direction is visible and establishes the undo baseline");
    auto* footer =
        window.editor()->window()->findChild<QWidget*>(QStringLiteral("imageSplitFooter"));
    auto* layout = footer ? qobject_cast<QHBoxLayout*>(footer->layout()) : nullptr;
    auto* save =
        footer ? footer->findChild<AdButton*>(QStringLiteral("imageSplitSaveAll")) : nullptr;
    require(layout && save && layout->indexOf(button(window)) + 1 == layout->indexOf(save) &&
                button(window)->accessibleName() == QStringLiteral("Stitch to PDF") &&
                button(window)->isEnabled() &&
                button(window)->buttonStyle() != AdButton::ButtonStyle::Solid,
            "accessible secondary PDF button sits immediately left of Save All");
    typeCount(window, Qt::Key_3, QStringLiteral("3"));
    require(window.document()->settings().count == 2, "visible count draft is uncommitted");
    button(window)->click();
    require(requests.size() == 1 && requests.first().layout.outputCount() == 3 &&
                window.document()->settings().count == 3 && window.modal()->isOpen() &&
                !directoryChoice,
            "PDF handoff commits the draft, keeps the splitter open and does not save files");
    window.document()->undo();
    typeCount(window, Qt::Key_0, QStringLiteral("0"));
    window.stitchToPdf();
    require(requests.size() == 1 && window.document()->settings().count == 2 &&
                !button(window)->isEnabled(),
            "invalid visible drafts cannot send the previous valid geometry");
    typeCount(window, Qt::Key_3, QStringLiteral("3"));
    accepted = false;
    window.stitchToPdf();
    require(requests.size() == 2 && !window.lastError().isEmpty() && window.modal()->isOpen() &&
                window.document()->settings().count == 3,
            "rejected handoff preserves edits and reports feedback");
    auto& languages = LanguageManager::instance();
    for (const auto& locale :
         {QStringLiteral("zh_CN"), QStringLiteral("zh_TW"), QStringLiteral("en_US")}) {
        require(languages.setLanguage(locale), "switch handoff language");
        const auto translated = QCoreApplication::translate("ImageSplitWindow", "Stitch to PDF");
        require(
            button(window)->text() == translated &&
                button(window)->accessibleName() == translated &&
                (locale == QStringLiteral("en_US") ||
                 translated != QStringLiteral("Stitch to PDF")) &&
                window.lastError() ==
                    QCoreApplication::translate(
                        "ImageSplitWindow", "Image stitching is busy. Try again when it finishes"),
            "button, accessibility and rejection feedback retranslate in an open window");
    }
    accepted = true;
    window.saveAll();
    require(directoryChoice && !button(window)->isEnabled(),
            "choosing an export destination disables PDF handoff");
    window.stitchToPdf();
    require(requests.size() == 2, "another operation cannot race a destination choice");
    directoryChoice({});
    require(button(window)->isEnabled(), "canceling destination choice restores handoff");
    auto* overlap = window.editor()->findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("imageSplitOverlapWidth"));
    auto* overlapInput = overlap ? overlap->findChild<QLineEdit*>() : nullptr;
    require(overlapInput, "PDF handoff exposes the overlap draft editor");
    overlapInput->setFocus();
    overlapInput->selectAll();
    QKeyEvent ten(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier, QStringLiteral("10"));
    QApplication::sendEvent(overlapInput, &ten);
    require(window.document()->settings().overlapWidth == 0, "overlap draft remains uncommitted");
    window.stitchToPdf();
    require(requests.size() == 3 && requests.last().layout.overlapWidth == 10 &&
                requests.last().layout.regionAt(0).intersects(requests.last().layout.regionAt(1)),
            "PDF handoff commits pending overlap into the immutable crop snapshot");
    overlap->clear();
    window.stitchToPdf();
    require(requests.size() == 3 && !button(window)->isEnabled() && !save->isEnabled(),
            "invalid overlap disables both output actions and cannot hand off stale geometry");
    window.shutdown();
}

void piecesAppendAsPdfAndOwnTheirSource() {
    const auto pixels = image();
    for (const auto direction : {ImageSplitDirection::Horizontal, ImageSplitDirection::Vertical,
                                 ImageSplitDirection::Grid}) {
        ImageStitchingWindowCoordinator coordinator;
        coordinator.present();
        auto* imageStitching = coordinator.window();
        const quint64 existing =
            imageStitching->document()->append(image({3, 4}), QStringLiteral("existing"));
        auto pdfSettings = imageStitching->document()->pdfSettings();
        pdfSettings.paperSize = ImageStitchingPdfPaperSize::Letter;
        pdfSettings.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
        pdfSettings.templateType = ImageStitchingPdfTemplateType::ThreeImages;
        pdfSettings.imageSpacingMm = 6.25;
        imageStitching->document()->setPdfSettings(pdfSettings);
        ImageSplitWindowServices services;
        services.stitchToPdf = [&](ImageSplitSnapshot value) {
            return coordinator.presentSplitImagesAsPdf(std::move(value));
        };
        ImageSplitWindow splitter(nullptr, services);
        splitter.presentImage(pixels, QStringLiteral("source.png"));
        loaded(splitter);
        auto settings = splitter.document()->settings();
        settings.direction = direction;
        settings.count = 3;
        settings.rows = 2;
        settings.columns = 3;
        settings.overlapWidth = 3;
        require(splitter.document()->setSettings(settings), "apply split settings");
        require(splitter.document()->createLine(direction != ImageSplitDirection::Horizontal, 11),
                "add a manual guide to the current layout");
        if (direction == ImageSplitDirection::Grid)
            require(splitter.document()->createLine(false, 17),
                    "add a manual horizontal grid guide");
        const auto expected = splitter.document()->layout();
        button(splitter)->click();
        auto* outputType = imageStitching->editor()->findChild<adqt::widgets::AdSegmented*>(
            QStringLiteral("collageImageType"));
        require(
            coordinator.window() == imageStitching && splitter.modal()->isOpen() &&
                imageStitching->document()->outputType() == ImageStitchingOutputType::PdfDocument &&
                outputType && outputType->currentValue() == QStringLiteral("pdf") &&
                imageStitching->document()->pdfSettings() == pdfSettings,
            "handoff reuses stitching, selects PDF and preserves both windows and PDF settings");
        splitter.document()->setImage(image({6, 8}));
        splitter.shutdown();
        until([&] { return !imageStitching->loading(); }, "split pieces finish importing");
        const auto& items = imageStitching->document()->items();
        require(items.size() == expected.outputCount() + 1 && items.first().id == existing,
                "split pieces append after existing content");
        require(
            imageStitchingPdfPageCount(items.size(), imageStitching->document()->pdfSettings()) ==
                (items.size() + 2) / 3,
            "split pieces paginate with the current template capacity and retained spacing");
        for (qint64 index = 0; index < expected.outputCount(); ++index) {
            const auto& actual = items.at(index + 1).image;
            require(actual == pixels.copy(expected.regionAt(index)) &&
                        actual.format() == pixels.format() &&
                        actual.colorSpace() == pixels.colorSpace() &&
                        actual.devicePixelRatio() == pixels.devicePixelRatio(),
                    "pages preserve exact native crop pixels, metadata and row-major order");
        }
        coordinator.shutdown();
        flush();
    }
}

void newHandoffRestoresRememberedTemplate() {
    ImageStitchingSettingsStore store(
        snow_shot::storage::ApplicationStorage::instance().configuration());
    const auto previous = store.readPdf();
    auto remembered = previous;
    remembered.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
    remembered.templateType = ImageStitchingPdfTemplateType::IdCard;
    remembered.imageSpacingMm = 8.125;
    const QStringList fields{QStringLiteral("stitching_method"), QStringLiteral("template_type"),
                             QStringLiteral("image_spacing_mm")};
    require(store.writePdf(fields, remembered), "remember the last template stitching choices");
    ImageStitchingWindowCoordinator coordinator;
    require(coordinator.presentSplitImagesAsPdf(snapshot(image({20, 12}))),
            "handoff creates a new stitching window");
    auto* window = coordinator.window();
    until([&] { return !window->loading(); }, "remembered template handoff imports");
    require(window->document()->outputType() == ImageStitchingOutputType::PdfDocument &&
                window->document()->pdfSettings() == remembered &&
                imageStitchingPdfPageCount(window->document()->items().size(), remembered) == 1,
            "a fresh Stitch to PDF window restores the remembered method, template and spacing");
    coordinator.shutdown();
    flush();
    require(store.writePdf(fields, previous), "restore the isolated preferences for later cases");
}

void queuedBatchesKeepRequestOrder() {
    ImageStitchingWindowCoordinator coordinator;
    const auto first = snapshot(image({20, 12}));
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Horizontal;
    const auto second = snapshot(image({8, 18}), settings);
    require(coordinator.presentSplitImagesAsPdf(first) && coordinator.window()->loading() &&
                coordinator.presentSplitImagesAsPdf(second),
            "new split batches append while an earlier batch is importing");
    auto* window = coordinator.window();
    until([&] { return !window->loading(); }, "queued split batches complete");
    require(
        window->document()->items().size() == 4 &&
            window->document()->items().at(0).image == first.image.copy(first.layout.regionAt(0)) &&
            window->document()->items().at(1).image == first.image.copy(first.layout.regionAt(1)) &&
            window->document()->items().at(2).image ==
                second.image.copy(second.layout.regionAt(0)) &&
            window->document()->items().at(3).image == second.image.copy(second.layout.regionAt(1)),
        "compact split batches preserve request order and page order");
    coordinator.shutdown();
}

void busyExportRejectsWithoutChangingStitching() {
    ImageStitchingWindowServices services;
    ImageStitchingWindowServices::Completion completion;
    services.copy = [&](auto, QObject*, QWidget*, auto done) { completion = std::move(done); };
    ImageStitchingWindowCoordinator coordinator(nullptr, services);
    coordinator.present();
    auto* window = coordinator.window();
    const quint64 existing =
        window->document()->append(image({10, 10}), QStringLiteral("existing"));
    window->copy();
    require(window->busy(), "stitching export starts");
    require(!coordinator.presentSplitImagesAsPdf(snapshot(image())) &&
                window->document()->outputType() == ImageStitchingOutputType::StaticImage &&
                window->document()->items().size() == 1 &&
                window->document()->items().first().id == existing,
            "busy handoff cannot change output type or existing images");
    until([&] { return bool(completion); }, "copy reaches its publication service");
    completion(false, {});
    require(!window->busy() && window->document()->items().size() == 1,
            "rejected requests are not replayed after an export");
    require(coordinator.presentSplitImagesAsPdf(snapshot(image())),
            "handoff can be retried after export");
    until([&] { return !window->loading(); }, "retried split import completes");
    coordinator.shutdown();
}

void invalidSourcesLimitsAndCancellation() {
    ImageStitchingWindowCoordinator coordinator;
    auto invalid = snapshot(image());
    invalid.layout.size = QSize(2, 2);
    require(!coordinator.presentSplitImagesAsPdf(std::move(invalid)),
            "mismatched source geometry is rejected");
    QImage oversized(QSize(std::numeric_limits<int>::max() / 4 + 1, 2), QImage::Format_Mono);
    require(!oversized.isNull(), "compact monochrome resource-limit fixture allocates");
    ImageSplitSnapshot limited{
        oversized, {oversized.size(), {}, {1}, {}}, QStringLiteral("oversized"), {}};
    QStringList failures;
    auto* window = coordinator.window();
    QObject::connect(window, &ImageStitchingWindow::importFinished, window,
                     [&](const auto& errors) { failures = errors; });
    require(coordinator.presentSplitImagesAsPdf(std::move(limited)) &&
                coordinator.presentSplitImagesAsPdf(snapshot(image({8, 6}))),
            "resource-limited and ordinary batches enter the importer");
    until([&] { return !window->loading(); }, "resource-limited batch finishes");
    require(failures.size() == 1 && window->document()->items().size() == 2,
            "an oversized crop stops its batch once and allows the next batch to import");
    coordinator.shutdown();
    flush();

    ImageStitchingWindow canceled;
    int completions = 0;
    QObject::connect(&canceled, &ImageStitchingWindow::importFinished, &canceled,
                     [&](const auto&) { ++completions; });
    auto owner = std::make_shared<int>(1);
    const std::weak_ptr<int> lifetime = owner;
    ImageSplitSettings dense;
    dense.direction = ImageSplitDirection::Grid;
    dense.rows = 500;
    dense.columns = 500;
    auto request = snapshot(image({500, 500}), dense);
    request.resourceOwner = owner;
    owner.reset();
    require(canceled.presentSplitImagesAsPdf(std::move(request)) && !lifetime.expired(),
            "dense split layouts queue compactly and retain their source owner");
    canceled.shutdown();
    until([&] { return lifetime.expired(); },
          "cancellation releases queued and worker source owners");
    require(canceled.document()->items().isEmpty() && completions == 0 && !canceled.loading(),
            "closing stitching cancels the dense batch and ignores stale completion");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(directory.isValid() &&
                storage.initialize({directory.path(), directory.path(), 60000}).success,
            "initialize isolated storage");
    styles::ThemeManager::instance().initialize(application);
    LanguageManager::instance().initialize();
    require(LanguageManager::instance().setLanguage(QStringLiteral("en_US")), "initialize English");
    footerDraftsAndTranslations();
    piecesAppendAsPdfAndOwnTheirSource();
    newHandoffRestoresRememberedTemplate();
    queuedBatchesKeepRequestOrder();
    busyExportRejectsWithoutChangingStitching();
    invalidSourcesLimitsAndCancellation();
    until([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; },
          "handoff workers drain");
    ScreenshotExportCoordinator::shared().shutdown();
    storage.shutdown();
    std::cout << "Image split PDF handoff tests passed\n";
    return 0;
}
