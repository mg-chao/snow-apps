// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitwindow.h"
#include "snow_shot/presentation/collagewindowcoordinator.h"
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
                        "ImageSplitWindow", "Image Stitching is busy. Try again when it finishes."),
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
    window.shutdown();
}

void piecesAppendAsPdfAndOwnTheirSource() {
    const auto pixels = image();
    for (const auto direction : {ImageSplitDirection::Horizontal, ImageSplitDirection::Vertical,
                                 ImageSplitDirection::Grid}) {
        CollageWindowCoordinator coordinator;
        coordinator.present();
        auto* collage = coordinator.window();
        const quint64 existing =
            collage->document()->append(image({3, 4}), QStringLiteral("existing"));
        auto pdfSettings = collage->document()->pdfSettings();
        pdfSettings.paperSize = CollagePdfPaperSize::Letter;
        collage->document()->setPdfSettings(pdfSettings);
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
        require(splitter.document()->setSettings(settings), "apply split settings");
        require(splitter.document()->createLine(direction != ImageSplitDirection::Horizontal, 11),
                "add a manual guide to the current layout");
        if (direction == ImageSplitDirection::Grid)
            require(splitter.document()->createLine(false, 17),
                    "add a manual horizontal grid guide");
        const auto expected = splitter.document()->layout();
        button(splitter)->click();
        auto* outputType = collage->editor()->findChild<adqt::widgets::AdSegmented*>(
            QStringLiteral("collageImageType"));
        require(
            coordinator.window() == collage && splitter.modal()->isOpen() &&
                collage->document()->outputType() == CollageOutputType::PdfDocument && outputType &&
                outputType->currentValue() == QStringLiteral("pdf") &&
                collage->document()->pdfSettings() == pdfSettings,
            "handoff reuses stitching, selects PDF and preserves both windows and PDF settings");
        splitter.document()->setImage(image({6, 8}));
        splitter.shutdown();
        until([&] { return !collage->loading(); }, "split pieces finish importing");
        const auto& items = collage->document()->items();
        require(items.size() == expected.outputCount() + 1 && items.first().id == existing,
                "split pieces append after existing content");
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

void queuedBatchesKeepRequestOrder() {
    CollageWindowCoordinator coordinator;
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
    CollageWindowServices services;
    CollageWindowServices::Completion completion;
    services.copy = [&](auto, QObject*, QWidget*, auto done) { completion = std::move(done); };
    CollageWindowCoordinator coordinator(nullptr, services);
    coordinator.present();
    auto* window = coordinator.window();
    const quint64 existing =
        window->document()->append(image({10, 10}), QStringLiteral("existing"));
    window->copy();
    require(window->busy(), "stitching export starts");
    require(!coordinator.presentSplitImagesAsPdf(snapshot(image())) &&
                window->document()->outputType() == CollageOutputType::StaticImage &&
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
    CollageWindowCoordinator coordinator;
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
    QObject::connect(window, &CollageWindow::importFinished, window,
                     [&](const auto& errors) { failures = errors; });
    require(coordinator.presentSplitImagesAsPdf(std::move(limited)) &&
                coordinator.presentSplitImagesAsPdf(snapshot(image({8, 6}))),
            "resource-limited and ordinary batches enter the importer");
    until([&] { return !window->loading(); }, "resource-limited batch finishes");
    require(failures.size() == 1 && window->document()->items().size() == 2,
            "an oversized crop stops its batch once and allows the next batch to import");
    coordinator.shutdown();
    flush();

    CollageWindow canceled;
    int completions = 0;
    QObject::connect(&canceled, &CollageWindow::importFinished, &canceled,
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
