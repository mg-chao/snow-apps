#include "snow_shot/presentation/screenshotsaveasfiledialog.h"
#include "snow_shot/presentation/screenshotsavepreviewcanvas.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/components/aspectratiolockbutton.h"
#include "snow_shot/presentation/components/pathinput.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "screenshotsaveexportpipeline.h"
#include "snowimageqtcodec.h"
#include "widgets/button.h"
#include "widgets/context_menu.h"
#include "widgets/field_group.h"
#include "widgets/form.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_number.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/segmented.h"
#include "widgets/slider.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QColorSpace>
#include <QCursor>
#include <QDynamicPropertyChangeEvent>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScopeGuard>
#include <QScreen>
#include <QTranslator>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace adqt::widgets;
namespace storage = snow_shot::storage;
namespace settings = snow_shot::presentation::settings;
namespace pipeline = screenshot_save_export;
using Format = ScreenshotImageFileFormat;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}
void processUntil(const std::function<bool()>& condition,
                  const std::source_location& location = std::source_location::current()) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 30000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (!condition()) {
        std::cerr << "Wait failed at " << location.file_name() << ':' << location.line() << '\n';
        require(false, "timed out waiting for export dialog");
    }
}
void flush() {
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void requireJoinedPathControl(DirectoryPathInput* control) {
    require(control != nullptr, "path input component is missing");
    control->setFixedWidth(320);
    control->show();
    flush();
    auto* group = control->fieldGroup();
    require(group != nullptr && group->controlCount() == 2,
            "path input must use a two-control Ant field group");
    require(control->lineEdit()->geometry().right() == control->browseButton()->geometry().left(),
            "path input and browse button must share a joined border without a gap");
}
template <class T> T* child(QObject* parent, const char* name) {
    auto* result = parent->findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, name);
    return result;
}
QImage fixture(QSize size = QSize(160, 100)) {
    QImage image(size, QImage::Format_RGBA8888);
    image.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(x, y, QColor((x * 9) % 256, (y * 17) % 256, (x + y) % 256));
    return image;
}
bool samePixels(const QImage& left, const QImage& right) {
    if (left.isNull() || right.isNull() || left.size() != right.size())
        return false;
    const QImage rgbaLeft = left.convertToFormat(QImage::Format_RGBA8888);
    const QImage rgbaRight = right.convertToFormat(QImage::Format_RGBA8888);
    for (int row = 0; row < left.height(); ++row)
        if (std::memcmp(rgbaLeft.constScanLine(row), rgbaRight.constScanLine(row),
                        size_t(left.width()) * 4) != 0)
            return false;
    return true;
}

void reusablePathInputsAndSettings() {
    DirectoryPathInput directory;
    directory.setText(QStringLiteral("C:/captures"));
    requireJoinedPathControl(&directory);
    require(adqt::icons::describeIcon(directory.browseButton()->iconRef()).key.name ==
                QStringLiteral("folder-open"),
            "directory input must use FolderOpenOutlined");

    FilePathInput file;
    file.setText(QStringLiteral("C:/captures/icon.png"));
    requireJoinedPathControl(&file);
    require(adqt::icons::describeIcon(file.browseButton()->iconRef()).key.name ==
                QStringLiteral("file-add"),
            "file input must differ only by using FileAddOutlined");

    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto& registry = settings::builtInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget storagePage(registry, QStringLiteral("storage-and-privacy"), session);
    const auto directoryControls = storagePage.findChildren<DirectoryPathInput*>();
    require(directoryControls.size() == 2,
            "all screenshot and recording directory settings must use DirectoryPathInput");
    for (DirectoryPathInput* control : directoryControls) {
        require(qobject_cast<FilePathInput*>(control) == nullptr &&
                    adqt::icons::describeIcon(control->browseButton()->iconRef()).key.name ==
                        QStringLiteral("folder-open"),
                "directory settings must use the directory-specific path control");
    }

    SettingsPageWidget interfacePage(registry, QStringLiteral("interface-settings"), session);
    const auto fileControls = interfacePage.findChildren<FilePathInput*>();
    require(fileControls.size() == 1 &&
                adqt::icons::describeIcon(fileControls.constFirst()->browseButton()->iconRef())
                        .key.name == QStringLiteral("file-add"),
            "file settings must use FilePathInput with FileAddOutlined");
}
void snapshot(AdModal* modal, const QString& name) {
    const QString directory = qEnvironmentVariable("SNOW_EXPORT_TEST_SCREENSHOT_DIR");
    if (directory.isEmpty())
        return;
    require(QDir().mkpath(directory), "snapshot directory unavailable");
    flush();
    require(modal->contentWidget()->window()->grab().save(
                QDir(directory).filePath(name + QStringLiteral(".png"))),
            "dialog snapshot could not be written");
}
AdModal* openDialog(QWidget& owner, const QImage& image,
                    ScreenshotSaveAsFileDialog::Saved saved = {},
                    ScreenshotSaveAsFileDialog::Finished finished = {}) {
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, image, std::move(saved),
                                             std::move(finished)),
            "image dialog should open");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    require(modal->mode() == AdModal::Mode::Window &&
                modal->windowModality() == Qt::ApplicationModal && !modal->maskVisible() &&
                modal->closePolicy() == AdModal::ClosePolicy::Manual,
            "export must use application-modal Ant window mode");
    processUntil(
        [&] { return modal->contentWidget()->property("previewGeneration").toULongLong() > 0; });
    flush();
    require(child<AdInputNumber>(modal->contentWidget(), "saveWidthInput")->isEnabled() &&
                child<AdInputNumber>(modal->contentWidget(), "saveHeightInput")->isEnabled() &&
                child<AdButton>(modal->contentWidget(), "saveAspectLockButton")->isEnabled(),
            "loading the screenshot must restore dimension and aspect-lock controls");
    return modal;
}

void centersOnDisplayOverlay() {
    for (QScreen* screen : QApplication::screens()) {
        QWidget overlay(nullptr, Qt::Tool | Qt::FramelessWindowHint);
        overlay.setScreen(screen);
        overlay.setGeometry(screen->geometry());
        overlay.show();
        flush();
        auto* modal = openDialog(overlay, fixture());
        QWidget* surface = modal->contentWidget()->window();
        const QPoint offset = surface->geometry().center() - screen->geometry().center();
        require(std::abs(offset.x()) <= 1 && std::abs(offset.y()) <= 1,
                "Save dialog must be centered on its selection display overlay");
        require(surface->screen() == screen, "Save dialog must remain on its selection display");
        modal->reject();
        flush();
    }
}

void persistence(const QTemporaryDir& temp) {
    const QString key = QStringLiteral("screenshot/save_as_file_dialog");
    require(storage::ConfigurationSchema::defaultValue(key).toString() == QStringLiteral("system"),
            "system must be the default");
    for (const auto& value : {QStringLiteral("system"), QStringLiteral("snow_shot")})
        require(storage::ConfigurationSchema::normalize(key, value).valid,
                "supported dialog rejected");
    require(!storage::ConfigurationSchema::normalize(key, QStringLiteral("native")).valid &&
                !storage::ConfigurationSchema::normalize(key, 1).valid,
            "invalid dialog accepted");
    storage::ScreenshotSettings adapter;
    require(adapter.setSaveAsFileDialog(QStringLiteral("snow_shot")) &&
                adapter.saveAsFileDialog() == QStringLiteral("snow_shot"),
            "dialog setting did not persist");
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    require(backend.selectValue(settings::SettingsSelectBinding::ScreenshotSaveAsFileDialog) ==
                QStringLiteral("snow_shot"),
            "settings backend failed to read dialog selection");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotSettings) &&
                adapter.saveAsFileDialog() == QStringLiteral("system"),
            "reset must restore system dialog");
    require(backend.applySelectValue(settings::SettingsSelectBinding::ScreenshotSaveAsFileDialog,
                                     QStringLiteral("snow_shot")),
            "settings backend failed to write dialog selection");

    const QString pathKey = QStringLiteral("screenshot/save_path_shortcuts");
    const auto malformed =
        QJsonDocument::fromJson(
            R"([{"name":" Work ","path":" C:/work "},{"name":"work","path":"D:/duplicate"},{"name":"","path":"x"},{"name":"bad","path":2},42])")
            .array();
    const auto normalized = storage::ConfigurationSchema::normalize(pathKey, malformed);
    require(normalized.valid && normalized.changed && normalized.value.toArray().size() == 1,
            "shortcut normalization must discard malformed, blank and duplicate entries");
    require(!storage::ConfigurationSchema::normalize(pathKey, QJsonObject{}).valid,
            "shortcut configuration must be an array");
    const QVector<storage::ScreenshotSavePathShortcut> values{
        {QStringLiteral("Work"), temp.filePath("not-created")}};
    require(adapter.setSavePathShortcuts(values) && adapter.savePathShortcuts() == values,
            "shortcut values must round-trip before directory creation");
    require(
        !adapter.setSavePathShortcuts({values[0], {QStringLiteral("WORK"), QStringLiteral("x")}}),
        "duplicate shortcut writes must fail atomically");
    require(adapter.savePathShortcuts() == values &&
                !adapter.setSavePathShortcuts({{QStringLiteral(" "), QStringLiteral("x")}}),
            "invalid writes must preserve shortcuts");
    require(storage::ApplicationStorage::instance().flushNow().success,
            "configuration flush failed");
    QFile file(QDir(storage::ApplicationStorage::instance().configurationDirectory())
                   .filePath("config.json"));
    require(file.open(QIODevice::ReadOnly), "persisted configuration unavailable");
    require(QJsonDocument::fromJson(file.readAll())
                    .object()
                    .value("screenshot")
                    .toObject()
                    .value("save_path_shortcuts")
                    .toArray()
                    .size() == 1,
            "shortcuts were not persisted as structured JSON");
    require(adapter.setSavePathShortcuts({}), "shortcut cleanup failed");
}

void rememberedManualFormat(QWidget& owner, const QTemporaryDir& temp) {
    const storage::ScreenshotSettings settings;
    const QString key = QStringLiteral("screenshot/last_manual_save_format");
    const auto cleanup = qScopeGuard(
        [&] { static_cast<void>(settings.setLastManualSaveFormat(QStringLiteral("png"))); });
    require(settings.lastManualSaveFormat() == QStringLiteral("png"),
            "missing manual format must default to PNG");
    const QString automaticFormat = settings.imageFormat();
    const QString configPath =
        QDir(storage::ApplicationStorage::instance().configurationDirectory())
            .filePath("config.json");
    for (const auto format :
         {Format::Png, Format::Jpeg, Format::Bmp, Format::Webp, Format::Jxl, Format::Avif}) {
        const QString value = ScreenshotImageFileService::formatKey(format);
        require(settings.setLastManualSaveFormat(value) &&
                    ScreenshotSaveDialogState::initial(QSize(160, 100)).output.format == format,
                "manual format must initialize the next dialog for every supported format");
        require(storage::ApplicationStorage::instance().flushNow().success,
                "manual format flush failed");
        storage::ConfigurationStore reloaded(configPath, true, false);
        require(reloaded.value(key).toString() == value,
                "manual format must survive configuration reload");
    }
    require(!settings.setLastManualSaveFormat(QStringLiteral("unsupported")) &&
                settings.lastManualSaveFormat() == QStringLiteral("avif"),
            "invalid format writes must preserve the previous preference");
    const QString invalidPath = temp.filePath("invalid-format.json");
    QFile invalid(invalidPath);
    require(invalid.open(QIODevice::WriteOnly), "invalid format fixture unavailable");
    invalid.write(R"({"screenshot":{"last_manual_save_format":"unsupported"}})");
    invalid.close();
    storage::ConfigurationStore invalidStore(invalidPath, true, false);
    require(invalidStore.value(key).toString() == QStringLiteral("png"),
            "invalid persisted manual format must fall back to PNG");

    require(settings.setLastManualSaveFormat(QStringLiteral("jpeg")) &&
                settings.setLastManualSaveDirectory(temp.path()),
            "manual format dialog setup failed");
    auto* modal = openDialog(owner, fixture());
    auto* content = modal->contentWidget();
    auto* format = child<AdSelect>(content, "saveFormatSelect");
    require(format->currentValue().toString() == QStringLiteral("jpeg"),
            "reopened dialog must select the remembered JPEG format");
    format->setCurrentValue(QStringLiteral("bmp"));
    modal->reject();
    flush();
    require(settings.lastManualSaveFormat() == QStringLiteral("jpeg"),
            "cancel must not remember an unconfirmed format change");

    QString savedPath;
    modal = openDialog(owner, fixture(), [&](const QString& path) { savedPath = path; });
    content = modal->contentWidget();
    child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("bmp"));
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("remember-format"));
    const QString existingPath = temp.filePath("remember-format.bmp");
    QFile existing(existingPath);
    require(existing.open(QIODevice::WriteOnly), "overwrite fixture unavailable");
    existing.write("fixture");
    existing.close();
    modal->acceptButton()->click();
    child<AdModal>(content, "saveOverwriteModal")->rejectButton()->click();
    flush();
    require(settings.lastManualSaveFormat() == QStringLiteral("jpeg"),
            "declining overwrite must not remember the new format");

    child<AdLineEdit>(content, "saveDirectoryInput")
        ->setText(existingPath + QStringLiteral("/child"));
    modal->acceptButton()->click();
    processUntil([&] { return !child<QLabel>(content, "saveErrorLabel")->isHidden(); });
    require(settings.lastManualSaveFormat() == QStringLiteral("bmp") && savedPath.isEmpty(),
            "confirmed format must be remembered even when the file write fails");
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    modal->acceptButton()->click();
    child<AdModal>(content, "saveOverwriteModal")->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    flush();
    require(savedPath == existingPath && settings.imageFormat() == automaticFormat,
            "manual save must use BMP without changing automatic-save format");
    modal = openDialog(owner, fixture());
    require(
        child<AdSelect>(modal->contentWidget(), "saveFormatSelect")->currentValue().toString() ==
            QStringLiteral("bmp"),
        "successful save must initialize the next dialog with BMP");
    modal->reject();
    flush();
}

void stateRules(const QTemporaryDir& temp) {
    storage::ScreenshotSettings settings;
    const QString configured = temp.filePath("configured");
    const QString remembered = temp.filePath("remembered");
    require(QDir().mkpath(remembered) && settings.setImageSaveDirectory(configured) &&
                settings.setLastManualSaveDirectory(remembered),
            "directory setup failed");
    auto state = ScreenshotSaveDialogState::initial(QSize(160, 100));
    require(!state.filename.isEmpty() && !state.filename.endsWith(QStringLiteral(".png")),
            "suggested filename must omit the image format extension");
    require(state.directory == remembered && state.output.size == QSize(160, 100) &&
                state.output.format == Format::Png && state.output.quality == 100 &&
                state.lockAspectRatio,
            "opening controls must use defaults and remembered directory");
    require(settings.setLastManualSaveDirectory(temp.filePath("missing")),
            "remembered directory setup failed");
    require(
        ScreenshotSaveDialogState::initial(QSize(160, 100)).directory == configured,
        "invalid remembered directory must fall back to configured directory even before creation");
    state.setDimension(true, 80);
    require(state.output.size == QSize(80, 50),
            "locked width failed to maintain source aspect ratio");
    state.setDimension(false, 200);
    require(state.output.size == QSize(320, 200),
            "locked height failed to maintain source aspect ratio");
    state.lockAspectRatio = false;
    state.setDimension(false, 123);
    require(state.output.size == QSize(320, 123), "unlocked height must be independent");
    state.filename = QStringLiteral("capture");
    for (auto format :
         {Format::Png, Format::Jpeg, Format::Bmp, Format::Webp, Format::Jxl, Format::Avif}) {
        state.output.format = format;
        state.output.quality = 100;
        const bool supportsLossless =
            format == Format::Webp || format == Format::Jxl || format == Format::Avif;
        require(state.lossless() == supportsLossless, "lossless badge support is incorrect");
        const auto options = ScreenshotImageFileService::encodeOptions(format, 100);
        if (supportsLossless)
            require(options.lossless, "quality 100 must explicitly encode losslessly");
        state.output.quality = 75;
        require(!state.lossless(), "lossy export must not show lossless badge");
        require(state.outputPath() == QDir(state.directory)
                                          .filePath(QStringLiteral("capture.") +
                                                    ScreenshotImageFileService::extension(format)),
                "output must preserve the base name and append the selected format extension");
    }
    require(pipeline::normalizedOptions({QSize(320, 123), Format::Bmp, 75}).quality == 100,
            "BMP quality must normalize out because the encoder has no quality setting");
    state.output.size = QSize(0, 100);
    require(!state.validationError().isEmpty(), "zero dimensions must fail");
    state.output.format = Format::Webp;
    state.output.size = QSize(pipeline::encoderLimits(Format::Webp).width() + 1, 100);
    require(!state.imageValidationError().isEmpty(), "encoder-limit dimensions must fail");
    state.output.size = QSize(160, 100);
    state.filename = QStringLiteral("CON.png");
    require(!state.validationError().isEmpty() && state.imageValidationError().isEmpty(),
            "filename errors must not block preview rendering");
}

void encodingAndFullResolutionDisplay() {
    QObject receiver;
    bool done = false;
    QString failure;
    const auto job = ScreenshotExportCoordinator::shared().submit(
        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [](const ScreenshotExportCancellation& cancellation) {
            QString error;
            QImage original = fixture();
            for (int y = 0; y < original.height(); ++y)
                for (int x = 0; x < original.width(); ++x) {
                    auto color = original.pixelColor(x, y);
                    color.setAlpha(64 + (x + y) % 192);
                    original.setPixelColor(x, y, color);
                }
            const auto source = pipeline::prepare(snow_shot::image_codec::srgbRowSource(original),
                                                  cancellation, &error);
            require(source.preview == original,
                    "original comparison pixels changed during source preparation");
            for (auto format : {Format::Png, Format::Jpeg, Format::Bmp, Format::Webp, Format::Jxl,
                                Format::Avif}) {
                for (int quality : {100, 72}) {
                    const ScreenshotSaveExportOptions options{
                        quality == 100 ? QSize(80, 50) : QSize(96, 32), format, quality};
                    const auto output = pipeline::render(source, options, cancellation, &error);
                    require(output && snow_shot::image_codec::inspectFile(
                                          output->path,
                                          ScreenshotImageFileService::snowImageFormat(format),
                                          options.size),
                            "final encoding dimensions differ from form");
                    const QImage preview = pipeline::decode(*output, cancellation, &error);
                    const QImage independent = snow_shot::image_codec::decodeFile(
                        output->path, ScreenshotImageFileService::snowImageFormat(format));
                    require(!preview.isNull() && preview.size() == options.size &&
                                samePixels(preview, independent),
                            "display pixels must match an independent decode of the export");
                }
                if (format != Format::Jpeg) {
                    const auto lossless = pipeline::render(source, {original.size(), format, 100},
                                                           cancellation, &error);
                    const QImage preview =
                        lossless ? pipeline::decode(*lossless, cancellation, &error) : QImage{};
                    if (preview != original) {
                        QString detail = ScreenshotImageFileService::extension(format);
                        if (!preview.isNull()) {
                            for (int y = 0; y < original.height(); ++y)
                                for (int x = 0; x < original.width(); ++x)
                                    if (original.pixelColor(x, y) != preview.pixelColor(x, y)) {
                                        detail +=
                                            QStringLiteral(" at %1,%2: %3 -> %4")
                                                .arg(x)
                                                .arg(y)
                                                .arg(
                                                    original.pixelColor(x, y).name(QColor::HexArgb),
                                                    preview.pixelColor(x, y).name(QColor::HexArgb));
                                        throw std::runtime_error("lossless codec changed pixels: " +
                                                                 detail.toStdString());
                                    }
                        }
                        throw std::runtime_error("lossless codec changed image metadata: " +
                                                 detail.toStdString());
                    }
                }
            }
            const auto large =
                pipeline::prepare(snow_shot::image_codec::srgbRowSource(fixture(QSize(128, 6000))),
                                  cancellation, &error);
            require(large.rows.size == QSize(128, 6000) && large.preview.height() == 2048,
                    "large source must retain full dimensions and a bounded preview");
            for (auto format : {Format::Png, Format::Jpeg, Format::Bmp, Format::Webp, Format::Jxl,
                                Format::Avif}) {
                const auto output =
                    pipeline::render(large, {QSize(48, 2304), format, 72}, cancellation, &error);
                require(output != nullptr, "large output encoding failed");
                const QImage preview = pipeline::decode(*output, cancellation, &error);
                require(preview.size() == QSize(48, 2304) &&
                            samePixels(preview,
                                       snow_shot::image_codec::decodeFile(
                                           output->path,
                                           ScreenshotImageFileService::snowImageFormat(format))),
                        "large display images must contain the complete decoded export");
            }
            const auto retained =
                pipeline::render(source, {original.size(), Format::Png, 100}, cancellation, &error);
            require(retained != nullptr, "retained export fixture failed");
            const QString parked = retained->path + QStringLiteral(".unavailable");
            require(QFile::rename(retained->path, parked),
                    "decode failure fixture could not be parked");
            error.clear();
            require(
                pipeline::decode(*retained, cancellation, &error).isNull() && !error.isEmpty(),
                "an unavailable encoded file must fail display decoding without fallback pixels");
            require(QFile::rename(parked, retained->path), "retained export could not be restored");
            const auto saved = ScreenshotImageFileService::writeEncodedFile(
                retained->path, retained->directory.filePath(QStringLiteral("saved.png")),
                retained->options.format);
            QFile encodedBytes(retained->path);
            QFile savedBytes(saved.path);
            require(
                saved.succeeded() && encodedBytes.open(QIODevice::ReadOnly) &&
                    savedBytes.open(QIODevice::ReadOnly) &&
                    encodedBytes.readAll() == savedBytes.readAll(),
                "saving after a transient decode failure must copy the retained bytes unchanged");
            auto raster = pipeline::MappedRaster::create(QSize(2, 2), &error);
            require(raster != nullptr, "mapped image allocation failed");
            std::fill_n(raster->pixels, 16, uchar(127));
            QImage mapped = raster->image();
            require(mapped.constBits() == raster->pixels,
                    "display must not copy the mapped raster");
            std::weak_ptr<pipeline::MappedRaster> weak = raster;
            raster.reset();
            require(!weak.expired() && mapped.pixelColor(0, 0) == QColor(127, 127, 127, 127),
                    "display pixels must own their mapping after the worker finishes");
            QImage modified = mapped;
            modified.setPixelColor(0, 0, Qt::red);
            require(mapped.pixelColor(0, 0) == QColor(127, 127, 127, 127),
                    "writes must detach from the immutable mapped image");
            mapped = {};
            require(weak.expired(), "releasing the last display image must release its mapping");
            pipeline::Encoded missing;
            missing.options = {QSize(2, 2), Format::Png, 100};
            missing.path = missing.directory.filePath(QStringLiteral("missing.png"));
            error.clear();
            require(pipeline::decode(missing, cancellation, &error).isNull() && !error.isEmpty(),
                    "display decoding failure must return an error without fallback pixels");
            return ScreenshotExportTaskResult{};
        },
        [&](ScreenshotExportTaskResult result) {
            failure = result.error;
            done = true;
        });
    require(job.isValid(), "encoding test could not be scheduled");
    processUntil([&] { return done; });
    if (!failure.isEmpty())
        throw std::runtime_error(failure.toStdString());
}

void canvasInteraction() {
    ScreenshotSavePreviewCanvas canvas;
    canvas.resize(600, 400);
    QImage original(QSize(400, 200), QImage::Format_RGBA8888);
    original.fill(Qt::red);
    QImage output(original.size(), original.format());
    output.fill(Qt::green);
    canvas.setSource(original, original.size());
    canvas.setOutput(output);
    canvas.show();
    flush();
    const auto screenshot = canvas.grab().toImage();
    const qreal dpr = screenshot.devicePixelRatio();
    const auto pixel = [&screenshot, dpr](int x, int y) {
        return screenshot.pixelColor(qRound(x * dpr), qRound(y * dpr));
    };
    require(screenshot.pixelColor(qRound(200 * dpr), qRound(170 * dpr)) == QColor(Qt::red) &&
                screenshot.pixelColor(qRound(400 * dpr), qRound(170 * dpr)) == QColor(Qt::green),
            "split canvas must render original on left and output on right");
    require(pixel(295, 20) == QColor(Qt::white) && pixel(296, 20) != QColor(Qt::white) &&
                pixel(303, 20) != pixel(304, 20),
            "comparison divider must use the viewer-style eight-pixel dark track");
    require(pixel(280, 200) == QColor(Qt::black) && pixel(292, 200).red() > 220 &&
                pixel(292, 200).green() > 220 && pixel(307, 200) != QColor(Qt::black),
            "comparison divider must use the viewer-style circular directional thumb");
    QMouseEvent hover(QEvent::MouseMove, QPointF(300, 200), QPointF(300, 200), Qt::NoButton,
                      Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &hover);
    const QImage hovered = canvas.grab().toImage();
    require(hovered.pixelColor(qRound(280 * dpr), qRound(200 * dpr)) != QColor(Qt::black),
            "comparison divider thumb must show its hover state");
    canvas.setSplitRatio(-1);
    require(canvas.splitRatio() == 0, "split lower bound failed");
    canvas.setSplitRatio(2);
    require(canvas.splitRatio() == 1, "split upper bound failed");
    canvas.setSplitRatio(0.5);
    const double before = canvas.zoom();
    const QPointF cursor(450, 150);
    const QPointF point = (cursor - QRectF(canvas.rect()).center() - canvas.pan()) / before;
    QWheelEvent wheel(cursor, canvas.mapToGlobal(cursor.toPoint()), {}, QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &wheel);
    require(
        canvas.zoom() > before &&
            QLineF(point, (cursor - QRectF(canvas.rect()).center() - canvas.pan()) / canvas.zoom())
                    .length() < .001,
        "zoom must preserve the image point under the cursor");
    require(child<QLabel>(&canvas, "savePreviewZoom")->text() == QStringLiteral("Scale: 115%"),
            "zoom readout did not update");
    const QPointF pan = canvas.pan();
    QMouseEvent down(QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100), Qt::LeftButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(125, 120), QPointF(125, 120), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, QPointF(125, 120), QPointF(125, 120), Qt::LeftButton,
                   Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &down);
    QApplication::sendEvent(&canvas, &move);
    QApplication::sendEvent(&canvas, &up);
    require(canvas.pan() == pan + QPointF(25, 20), "canvas dragging did not pan");
}

void canvasZoomHint() {
    ScreenshotSavePreviewCanvas canvas;
    canvas.resize(600, 400);
    canvas.setSource(fixture(), QSize(160, 100));
    canvas.show();
    flush();
    auto* hint = child<QLabel>(&canvas, "savePreviewZoom");
    require(hint->isHidden(), "initial preview fitting must not show the zoom hint");
    auto* timer = child<QTimer>(&canvas, "savePreviewZoomTimer");
    require(timer->isSingleShot() && timer->interval() == 1000 && !timer->isActive(),
            "zoom hint must use the pinned-window one-second dismissal interval");
    const auto expire = [&] {
        timer->stop();
        require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
                "zoom hint timeout could not be delivered");
        require(hint->isHidden(), "zoom hint must disappear when its timer expires");
    };
    const auto pressKey = [&](int key) {
        QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    pressKey(Qt::Key_Plus);
    require(hint->isVisible() && timer->isActive() &&
                hint->text() == QStringLiteral("Scale: 115%") && hint->x() == 8 &&
                canvas.height() - hint->geometry().bottom() - 1 == 8 &&
                hint->size() == hint->sizeHint(),
            "zoom hint must match the pinned-window text, fitted size, and inset");
    require(hint->styleSheet().contains(QStringLiteral("rgba(0, 0, 0, 150)")) &&
                hint->styleSheet().contains(QStringLiteral("padding: 3px 6px")) &&
                hint->testAttribute(Qt::WA_TransparentForMouseEvents),
            "zoom hint must match the pinned-window appearance without intercepting input");
    const int firstTimerId = timer->timerId();
    pressKey(Qt::Key_Plus);
    require(timer->isActive() && timer->timerId() != firstTimerId,
            "continued zooming must restart the hint dismissal timer");
    expire();
    canvas.setOutput(fixture(QSize(80, 50)));
    canvas.resize(640, 440);
    require(hint->isHidden() && !timer->isActive(),
            "output and layout changes must not show the zoom hint");
    pressKey(Qt::Key_Home);
    require(hint->isVisible() && timer->isActive(),
            "user-requested fitting must show zoom changes");
    expire();
    pressKey(Qt::Key_Home);
    require(hint->isHidden(), "fitting at the same zoom must not show the hint");
    for (int i = 0; i < 30; ++i)
        pressKey(Qt::Key_Plus);
    require(canvas.zoom() == 8.0, "zoom upper bound fixture failed");
    expire();
    pressKey(Qt::Key_Plus);
    require(hint->isHidden() && !timer->isActive(),
            "zoom input clamped to the current value must not show the hint");
}

void canvasOriginalSizeBaseline() {
    ScreenshotSavePreviewCanvas canvas;
    canvas.resize(600, 400);
    QImage original(QSize(400, 200), QImage::Format_RGBA8888);
    original.fill(Qt::red);
    QImage output(original.size(), original.format());
    output.fill(Qt::green);
    const QSize sourcePixels(1000, 500);
    canvas.setSource(original, sourcePixels);
    canvas.setOutput(output);
    canvas.show();
    flush();
    const auto frame = [&] {
        const QImage image = canvas.grab().toImage();
        return image.copy(0, 0, image.width(), qRound(340 * image.devicePixelRatio()));
    };
    const auto requireStableOutput = [&] {
        const double zoom = canvas.zoom();
        const QPointF pan = canvas.pan();
        const QImage before = frame();
        for (const QSize size : {QSize(500, 250), QSize(2000, 1000), QSize(1000, 300)}) {
            canvas.setOutput(output.scaled(size));
            require(canvas.zoom() == zoom && canvas.pan() == pan,
                    "output resizing must preserve zoom and pan from the original size");
            require(frame() == before,
                    "both comparison images must stay aligned to the original canvas bounds");
        }
    };
    requireStableOutput();
    QWheelEvent wheel(QPointF(450, 150), canvas.mapToGlobal(QPoint(450, 150)), {}, QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &wheel);
    requireStableOutput();
    canvas.fitImage();
    canvas.resize(800, 600);
    flush();
    require(qAbs(canvas.zoom() - 0.76) < 0.000001,
            "layout refitting must still use original pixels after output resizing");
}

void unchangedPreviewEdits(QWidget& owner) {
    auto* modal = openDialog(owner, fixture());
    const auto closeDialog = qScopeGuard([modal] {
        modal->reject();
        flush();
    });
    auto* content = modal->contentWidget();
    auto* busy = child<QLabel>(content, "savePreviewStatus");
    auto* width = child<AdInputNumber>(content, "saveWidthInput");
    auto* height = child<AdInputNumber>(content, "saveHeightInput");
    auto* lock = child<AdButton>(content, "saveAspectLockButton");
    auto* quality = child<AdSlider>(content, "saveQualitySlider");
    auto* format = child<AdSelect>(content, "saveFormatSelect");
    processUntil([&] { return content->property("previewGeneration").toULongLong() > 0; });
    const quint64 generation = content->property("previewGeneration").toULongLong();
    snapshot(modal, QStringLiteral("export-preview-idle"));
    auto* canvas = child<ScreenshotSavePreviewCanvas>(content, "savePreviewCanvas");
    const QPoint cursor = canvas->rect().center();
    QWheelEvent wheel(cursor, canvas->mapToGlobal(cursor), {}, QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas, &wheel);
    snapshot(modal, QStringLiteral("export-preview-zoom"));
    lock->click();
    lock->click();
    require(busy->isHidden(), "relocking unchanged dimensions must not schedule preview rendering");
    require(QMetaObject::invokeMethod(width, "valueChanged", Qt::DirectConnection,
                                      Q_ARG(double, 160.1)),
            "rounded dimension signal could not be delivered");
    require(busy->isHidden(), "equivalent rounded dimensions must not schedule preview rendering");
    quality->setValue(80);
    require(busy->isHidden(), "PNG quality changes must not recalculate unchanged pixels");
    width->setValue(80);
    require(!busy->isHidden(), "different output dimensions must schedule a preview");
    width->setValue(160);
    require(busy->isHidden(), "returning to the displayed output must reuse its preview");
    width->clear();
    require(!modal->acceptButton()->isEnabled(), "empty dimensions must still invalidate Save");
    width->setValue(160);
    require(busy->isHidden() && modal->acceptButton()->isEnabled(),
            "restoring valid displayed dimensions must revalidate without recalculation");
    bool settled = false;
    QTimer::singleShot(200, content, [&] { settled = true; });
    processUntil([&] { return settled; });
    require(content->property("previewGeneration").toULongLong() == generation,
            "equivalent edits must not complete redundant preview jobs");
    width->setValue(80);
    lock->click();
    lock->click();
    processUntil([&] { return content->property("previewGeneration").toULongLong() > generation; });
    require(height->value() == 50, "aspect lock must continue to update the paired dimension");
    const quint64 resizedGeneration = content->property("previewGeneration").toULongLong();
    format->setCurrentValue(QStringLiteral("jpeg"));
    processUntil(
        [&] { return content->property("previewGeneration").toULongLong() > resizedGeneration; });
    const quint64 jpegGeneration = content->property("previewGeneration").toULongLong();
    quality->setValue(70);
    require(!busy->isHidden(), "JPEG quality changes must still render a new preview");
    processUntil(
        [&] { return content->property("previewGeneration").toULongLong() > jpegGeneration; });
    const quint64 qualityGeneration = content->property("previewGeneration").toULongLong();
    width->setValue(2112);
    processUntil(
        [&] { return content->property("previewGeneration").toULongLong() > qualityGeneration; });
    const quint64 largeGeneration = content->property("previewGeneration").toULongLong();
    width->setValue(2304);
    require(!busy->isHidden() && height->value() == 1440,
            "different full export sizes must calculate different results");
    processUntil(
        [&] { return content->property("previewGeneration").toULongLong() > largeGeneration; });
    width->setValue(1000000);
    require(!modal->acceptButton()->isEnabled() && busy->isHidden(),
            "invalid export limits must block rendering");
    width->setValue(2304);
    require(modal->acceptButton()->isEnabled() && busy->isHidden(),
            "restoring the completed export dimensions must reuse its result");
}

void shortcutPopupInteraction(QWidget& owner, const QTemporaryDir& temp) {
    struct RestoreCursor {
        QPoint position = QCursor::pos();
        ~RestoreCursor() {
            QCursor::setPos(position);
        }
    } restoreCursor;
    storage::ScreenshotSettings settings;
    require(settings.setSavePathShortcuts({{QStringLiteral("Work"), temp.path()}}),
            "hover shortcut setup failed");
    auto* modal = openDialog(owner, fixture());
    const auto closeDialog = qScopeGuard([modal] {
        modal->rejectButton()->click();
        flush();
    });
    auto* content = modal->contentWidget();
    auto* trigger = child<AdButton>(content, "savePathExpand_0");
    auto enter = [](QWidget* widget) {
        const QPoint local = widget->rect().center();
        const QPoint global = widget->mapToGlobal(local);
        QCursor::setPos(global);
        flush();
        QEnterEvent event(local, local, global);
        QApplication::sendEvent(widget, &event);
    };
    auto leave = [](QWidget* widget) {
        QEvent event(QEvent::Leave);
        QApplication::sendEvent(widget, &event);
    };
    auto settle = [] {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 350) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
    };
    const QPoint outside = content->mapToGlobal(QPoint(10, 10));
    enter(trigger);
    QPointer<AdContextMenu> menu = child<AdContextMenu>(content, "savePathMenu");
    require(menu->isVisible(), "hovering the shortcut arrow must open its menu");
    const int popupWidth = menu->width();
    QCursor::setPos(outside);
    leave(trigger);
    settle();
    require(menu != nullptr, "hover popup was unexpectedly replaced during pointer movement");
    std::cerr << "Shortcut popup width: " << popupWidth
              << "; visible after leaving trigger: " << menu->isVisible() << '\n';
    require(!menu->isVisible(), "leaving the trigger without entering its menu must hide it");
    require(popupWidth < 160, "two-action shortcut popup must size to its content");
    flush();

    enter(trigger);
    flush();
    menu = child<AdContextMenu>(content, "savePathMenu");
    leave(trigger);
    enter(menu);
    settle();
    require(menu->isVisible(), "moving from the shortcut arrow into its menu must keep it open");
    auto requireLabelsFit = [&] {
        const auto theme = adqt::theme::ThemeManager::instance().resolve(menu, trigger).values;
        QFont font = menu->font();
        font.setPixelSize(qMax(12, qRound(theme.fontSize)));
        const QFontMetrics metrics(font);
        const int iconAndPadding = 2 * qMax(8, qRound(theme.sizeSM)) +
                                   qMax(12, qRound(theme.fontSize)) + qMax(6, qRound(theme.sizeXS));
        for (auto* action : menu->actions()) {
            const int available = menu->actionGeometry(action).width() - iconAndPadding;
            require(metrics.elidedText(action->text(), Qt::ElideRight, available) == action->text(),
                    "content-sized shortcut popup must display complete action labels");
        }
    };
    requireLabelsFit();
    const QString screenshotDirectory = qEnvironmentVariable("SNOW_EXPORT_TEST_SCREENSHOT_DIR");
    if (!screenshotDirectory.isEmpty()) {
        require(QDir().mkpath(screenshotDirectory) &&
                    menu->grab().save(QDir(screenshotDirectory).filePath("save-path-menu.png")),
                "shortcut popup snapshot could not be written");
    }
    const QString editText = menu->actions()[0]->text();
    menu->actions()[0]->setText(QStringLiteral("Edit this saved directory shortcut"));
    menu->adjustSize();
    require(menu->width() > popupWidth &&
                menu->actionGeometry(menu->actions()[0]).width() >
                    menu->fontMetrics().horizontalAdvance(menu->actions()[0]->text()),
            "content-sized shortcut menu must still expand for longer translated labels");
    requireLabelsFit();
    menu->actions()[0]->setText(editText);
    menu->adjustSize();
    leave(menu);
    enter(trigger);
    settle();
    require(menu->isVisible(), "returning to the shortcut arrow must cancel pending dismissal");

    // A native popup can grab mouse moves outside its bounds without another Leave event.
    QCursor::setPos(outside);
    QMouseEvent move(QEvent::MouseMove, menu->mapFromGlobal(outside), outside, Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(menu, &move);
    settle();
    require(!menu->isVisible(),
            "grabbed mouse movement outside the hover region must hide the menu");
    flush();

    enter(trigger);
    flush();
    menu = child<AdContextMenu>(content, "savePathMenu");
    enter(menu);
    QCursor::setPos(outside);
    leave(menu);
    settle();
    require(!menu->isVisible(), "leaving the popup must hide it without a click");
    require(settings.setSavePathShortcuts({}), "hover shortcut cleanup failed");
}

void browseShortcutDirectory(AdModal* editor, const QString& selectedPath, bool accept) {
    auto* path = child<DirectoryPathInput>(editor->contentWidget(), "savePathValuePathInput");
    requireJoinedPathControl(path);
    require(adqt::icons::describeIcon(path->browseButton()->iconRef()).key.name ==
                    QStringLiteral("folder-open") &&
                path->browseButton()->toolTip() == QStringLiteral("Select save directory") &&
                path->browseButton()->accessibleName() == path->browseButton()->toolTip(),
            "shortcut path must use the save directory icon and accessible browse label");
    const QString previousPath = path->text();
    const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs = qScopeGuard([nativeDialogsDisabled] {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
    });
    bool opened = false;
    bool correctDirectory = false;
    QTimer::singleShot(0, path, [&] {
        auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!picker)
            return;
        opened = true;
        correctDirectory = picker->directory().absolutePath() == QDir(previousPath).absolutePath();
        if (accept) {
            picker->selectFile(selectedPath);
            QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
        } else {
            picker->reject();
        }
    });
    path->browseButton()->click();
    require(opened && correctDirectory, "shortcut picker must open at the current input path");
    require(QDir::cleanPath(path->text()) == QDir::cleanPath(accept ? selectedPath : previousPath),
            "folder selection must update the shortcut path and cancellation must preserve it");
}

void shortcutsAndCancellation(QWidget& owner, const QTemporaryDir& temp) {
    int saved = 0;
    int finished = 0;
    auto* modal = openDialog(
        owner, fixture(), [&](const QString&) { ++saved; },
        [&](bool accepted) { finished += accepted ? 100 : 1; });
    auto* content = modal->contentWidget();
    require(!content->findChild<AdButton*>(QStringLiteral("savePathExpand_-2")) &&
                !content->findChild<AdButton*>(QStringLiteral("savePathExpand_-1")),
            "built-in paths must have no edit/delete action");
    child<AdButton>(content, "savePathShortcut_-2")->click();
    require(child<AdLineEdit>(content, "saveDirectoryInput")->text() ==
                storage::ScreenshotSettings().imageSaveDirectory(),
            "App directory must select the configured directory");
    snapshot(modal, QStringLiteral("export-before-shortcuts"));
    require(child<AdButton>(content, "savePathAddButton")->isEnabled(),
            "Add save path must be enabled after loading");
    child<AdButton>(content, "savePathAddButton")->click();
    auto* editor = child<AdModal>(content, "savePathEditorModal");
    require(editor->mode() == AdModal::Mode::Overlay, "shortcut editor must be a widget modal");
    editor->acceptButton()->click();
    require(editor->isOpen() && storage::ScreenshotSettings().savePathShortcuts().isEmpty(),
            "empty shortcut must be rejected");
    snapshot(modal, QStringLiteral("shortcut-validation"));
    child<AdLineEdit>(editor->contentWidget(), "savePathNameInput")
        ->setText(QStringLiteral("Projects"));
    child<AdLineEdit>(editor->contentWidget(), "savePathValueInput")->setText(temp.path());
    const QString projectsPath = temp.filePath("projects");
    require(QDir().mkpath(projectsPath), "shortcut browse directory setup failed");
    browseShortcutDirectory(editor, projectsPath, true);
    browseShortcutDirectory(editor, temp.path(), false);
    require(storage::ScreenshotSettings().savePathShortcuts().isEmpty(),
            "browsing must not persist a shortcut before confirmation");
    editor->acceptButton()->click();
    flush();
    require(storage::ScreenshotSettings().savePathShortcuts().size() == 1 &&
                storage::ScreenshotSettings().savePathShortcuts()[0].path == projectsPath,
            "confirmed shortcut must persist immediately");
    auto* shortcutGroup = child<AdFieldGroup>(content, "savePathShortcutGroup_0");
    require(shortcutGroup->controlCount() == 2,
            "custom save path must group its main and edit buttons");
    auto* shortcutButton = child<AdButton>(content, "savePathShortcut_0");
    auto* shortcutEdit = child<AdButton>(content, "savePathExpand_0");
    require(shortcutButton->parentWidget() == shortcutGroup &&
                shortcutEdit->parentWidget() == shortcutGroup &&
                shortcutButton->geometry().right() == shortcutEdit->geometry().left(),
            "save path and edit buttons must share a joined border without a gap");
    child<AdButton>(content, "savePathExpand_0")->click();
    auto* menu = child<AdContextMenu>(content, "savePathMenu");
    require(menu->actions().size() == 2 && menu->actionDanger(menu->actions()[1]),
            "shortcut menu must include danger Delete");
    menu->hide();
    menu->actions()[0]->trigger();
    editor = child<AdModal>(content, "savePathEditorModal");
    browseShortcutDirectory(editor, temp.path(), true);
    browseShortcutDirectory(editor, projectsPath, false);
    child<AdLineEdit>(editor->contentWidget(), "savePathNameInput")
        ->setText(QStringLiteral("Exports"));
    editor->acceptButton()->click();
    flush();
    require(storage::ScreenshotSettings().savePathShortcuts()[0].name ==
                    QStringLiteral("Exports") &&
                storage::ScreenshotSettings().savePathShortcuts()[0].path == temp.path(),
            "shortcut edit did not persist");
    child<AdButton>(content, "savePathAddButton")->click();
    editor = child<AdModal>(content, "savePathEditorModal");
    child<AdLineEdit>(editor->contentWidget(), "savePathNameInput")
        ->setText(QStringLiteral("EXPORTS"));
    child<AdLineEdit>(editor->contentWidget(), "savePathValueInput")
        ->setText(temp.filePath("duplicate"));
    editor->acceptButton()->click();
    require(editor->isOpen() && storage::ScreenshotSettings().savePathShortcuts().size() == 1,
            "duplicate name must keep editor open");
    editor->rejectButton()->click();
    flush();
    modal->rejectButton()->click();
    flush();
    require(saved == 0 && finished == 1 &&
                storage::ScreenshotSettings().savePathShortcuts().size() == 1,
            "cancel must preserve confirmed shortcut edits and never save");
    modal = openDialog(owner, fixture());
    content = modal->contentWidget();
    child<AdButton>(content, "savePathExpand_0")->click();
    menu = child<AdContextMenu>(content, "savePathMenu");
    menu->hide();
    menu->actions()[1]->trigger();
    flush();
    require(storage::ScreenshotSettings().savePathShortcuts().isEmpty(),
            "shortcut delete did not persist");
    modal->rejectButton()->click();
    flush();
}

void previewAndSave(QWidget& owner, const QTemporaryDir& temp) {
    QString savedPath;
    int finished = 0;
    auto* modal = openDialog(
        owner, fixture(), [&](const QString& path) { savedPath = path; },
        [&](bool saved) { finished += saved ? 1 : 100; });
    auto* content = modal->contentWidget();
    auto* format = child<AdSelect>(content, "saveFormatSelect");
    auto* quality = child<AdSlider>(content, "saveQualitySlider");
    auto* filename = child<AdLineEdit>(content, "saveFilenameInput");
    require(!filename->text().isEmpty() && !filename->text().endsWith(QStringLiteral(".png")),
            "filename input must initially omit the image format extension");
    filename->setText(QStringLiteral("capture.v2"));
    for (const auto& key : {QStringLiteral("jpeg"), QStringLiteral("bmp"), QStringLiteral("webp"),
                            QStringLiteral("jxl"), QStringLiteral("avif"), QStringLiteral("png")}) {
        format->setCurrentValue(key);
        require(filename->text() == QStringLiteral("capture.v2"),
                "changing formats must leave the filename input unchanged");
    }
    auto* directoryControl = child<DirectoryPathInput>(content, "saveDirectoryPathInput");
    requireJoinedPathControl(directoryControl);
    auto* dimensions = child<QWidget>(content, "saveDimensionsForm");
    auto* sizeLabel = child<QLabel>(dimensions, "saveSizeLabel");
    auto* unit = child<AdSegmented>(dimensions, "saveSizeUnitSegmented");
    require(sizeLabel->text() == QStringLiteral("Size") && unit->count() == 2 &&
                unit->currentValue() == QStringLiteral("pixels") &&
                unit->optionLabel(0) == QStringLiteral("Pixels") &&
                unit->optionLabel(1) == QStringLiteral("Percentage") &&
                sizeLabel->geometry().right() < unit->geometry().left() &&
                unit->geometry().right() == dimensions->rect().right(),
            "size header must have a single label and a right-aligned unit selector");
    auto* widthItem = child<AdInputNumber>(dimensions, "saveWidthInput");
    auto* heightItem = child<AdInputNumber>(dimensions, "saveHeightInput");
    require(widthItem->isVisible() && heightItem->isVisible() &&
                widthItem->geometry().top() == heightItem->geometry().top() &&
                widthItem->geometry().right() < heightItem->geometry().left(),
            "width and height fields must remain visible on the same form row");
    auto* lockBase = child<AdButton>(content, "saveAspectLockButton");
    auto* lock = dynamic_cast<AspectRatioLockButton*>(lockBase);
    require(lock != nullptr && lock->isChecked() &&
                adqt::icons::describeIcon(lock->iconRef()).key.name ==
                    QStringLiteral("selection-lock-aspect"),
            "Save as File must start with the selection editor aspect lock active");
    const QRect lockGeometry(lock->mapTo(dimensions, QPoint()), lock->size());
    require(widthItem->geometry().right() < lockGeometry.left() &&
                lockGeometry.right() < heightItem->geometry().left() &&
                lockGeometry.center().y() == widthItem->geometry().center().y() &&
                unit->geometry().bottom() < widthItem->geometry().top(),
            "aspect lock must remain between the width and height fields");
    require(quality->marks().value(quality->minimum()).label == QStringLiteral("0%") &&
                quality->marks().value(quality->maximum()).label == QStringLiteral("100%"),
            "PNG quality endpoints must read 0% and 100%");
    require(content->findChild<QWidget*>(QStringLiteral("saveLosslessBadge")) == nullptr,
            "quality must not retain the separate lossless badge");
    snapshot(modal, QStringLiteral("export-light"));
    if (!qEnvironmentVariable("SNOW_EXPORT_TEST_SCREENSHOT_DIR").isEmpty()) {
        adqt::theme::ThemeManager::instance().setColorScheme(adqt::theme::ThemeScheme::Dark);
        snapshot(modal, QStringLiteral("export-dark"));
        adqt::theme::ThemeManager::instance().setColorScheme(adqt::theme::ThemeScheme::Light);
        modal->setPreferredWidth(680);
        content->setFixedHeight(360);
        static_cast<void>(modal->takeContentWidget());
        modal->setContentWidget(content);
        snapshot(modal, QStringLiteral("export-compact"));
        modal->setPreferredWidth(1080);
        content->setFixedHeight(540);
        static_cast<void>(modal->takeContentWidget());
        modal->setContentWidget(content);
    }
    require(!quality->isEnabled(), "PNG must disable quality");
    format->setCurrentValue(QStringLiteral("bmp"));
    require(format->currentValue() == QStringLiteral("bmp") && !quality->isEnabled(),
            "BMP must be selectable and disable quality");
    for (const auto& key :
         {QStringLiteral("webp"), QStringLiteral("avif"), QStringLiteral("jxl")}) {
        format->setCurrentValue(key);
        quality->setValue(100);
        require(quality->isEnabled() &&
                    quality->marks().value(quality->maximum()).label == QStringLiteral("Lossless"),
                "lossless format must replace the 100% endpoint with Lossless");
        if (key == QStringLiteral("webp"))
            snapshot(modal, QStringLiteral("export-lossless"));
        quality->setValue(99);
    }
    format->setCurrentValue(QStringLiteral("jpeg"));
    require(quality->marks().value(quality->maximum()).label == QStringLiteral("100%"),
            "lossy format must restore the 100% endpoint");
    format->setCurrentValue(QStringLiteral("png"));
    const quint64 generation = content->property("previewGeneration").toULongLong();
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QString());
    child<AdInputNumber>(content, "saveWidthInput")->setValue(96);
    child<AdInputNumber>(content, "saveWidthInput")->setValue(80);
    processUntil([&] { return content->property("previewGeneration").toULongLong() > generation; });
    require(child<AdInputNumber>(content, "saveHeightInput")->value() == 50 &&
                !modal->acceptButton()->isEnabled(),
            "latest geometry must render despite invalid filename and Save must remain blocked");
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("result"));
    const QString blocking = temp.filePath("blocking-file");
    QFile file(blocking);
    require(file.open(QIODevice::WriteOnly), "blocking file fixture failed");
    file.write("fixture");
    file.close();
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(blocking + QStringLiteral("/child"));
    const auto remembered = storage::ScreenshotSettings().lastManualSaveDirectory();
    modal->acceptButton()->click();
    processUntil([&] { return !child<QLabel>(content, "saveErrorLabel")->isHidden(); });
    require(modal->isOpen() && savedPath.isEmpty() &&
                storage::ScreenshotSettings().lastManualSaveDirectory() == remembered,
            "failed write must keep dialog open and remembered directory unchanged");
    require(child<AdInputNumber>(content, "saveWidthInput")->isEnabled() &&
                child<AdInputNumber>(content, "saveHeightInput")->isEnabled() &&
                child<AdButton>(content, "saveAspectLockButton")->isEnabled(),
            "failed saves must restore dimension and aspect-lock controls");
    const QString directory = temp.filePath("new/nested");
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(directory);
    int changes = 0;
    const auto connection =
        QObject::connect(&storage::ApplicationStorage::instance().configuration(),
                         &storage::ConfigurationStore::valueChanged, &owner,
                         [&](const QString& key, const QJsonValue&) {
                             if (key == QStringLiteral("screenshot/last_manual_save_directory"))
                                 ++changes;
                         });
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    QObject::disconnect(connection);
    require(finished == 1 && changes == 1 &&
                storage::ScreenshotSettings().lastManualSaveDirectory() == directory,
            "successful save must finish and remember directory exactly once");
    require(
        savedPath.endsWith(QStringLiteral("result.png")) &&
            snow_shot::image_codec::inspectFile(savedPath, snow::image::Format::png, QSize(80, 50)),
        "saved output must use normalized extension and requested dimensions");
    flush();
}

void pendingCancellation(QWidget& owner) {
    std::atomic_bool started = false;
    std::atomic_bool stopped = false;
    auto artifact = std::make_shared<ScreenshotExportArtifact>(
        ScreenshotExportSource::fromProducer([&](const ScreenshotExportCancellation& cancellation) {
            started = true;
            while (!cancellation.isCancellationRequested())
                QThread::msleep(1);
            stopped = true;
            return QImage{};
        }));
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact),
            "pending-source dialog rejected");
    processUntil([&] { return started.load(); });
    child<AdModal>(&owner, "screenshotSaveAsFileModal")->rejectButton()->click();
    processUntil([&] { return stopped.load(); });
    require(artifact->isCancelled(), "closing dialog must cancel pending artifact work");
    flush();

    started = false;
    stopped = false;
    artifact = std::make_shared<ScreenshotExportArtifact>(
        ScreenshotExportSource::fromProducer({}, [&](std::function<bool()> cancelled) {
            started = true;
            if (cancelled) {
                while (!cancelled())
                    QThread::msleep(1);
                stopped = true;
            }
            return ScreenshotImageRowSource{};
        }));
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact),
            "pending row-source dialog rejected");
    processUntil([&] { return started.load(); });
    child<AdModal>(&owner, "screenshotSaveAsFileModal")->rejectButton()->click();
    processUntil([&] { return stopped.load(); });
    require(artifact->isCancelled(), "closing must cancel row-source preparation");
    flush();
}

class ExportTestTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context).startsWith("ScreenshotSave"))
            return QStringLiteral("Translated ") + QString::fromUtf8(source);
        return {};
    }
};

void sizeUnits(QWidget& owner, const QTemporaryDir& temp) {
    QString savedPath;
    auto source = fixture();
    source.setDevicePixelRatio(2);
    auto* modal = openDialog(owner, source, [&](const QString& path) { savedPath = path; });
    auto* content = modal->contentWidget();
    auto* unit = child<AdSegmented>(content, "saveSizeUnitSegmented");
    auto* width = child<AdInputNumber>(content, "saveWidthInput");
    auto* height = child<AdInputNumber>(content, "saveHeightInput");
    auto* lock = child<AdButton>(content, "saveAspectLockButton");
    auto* footer = modal->footerWidget();
    require(footer != nullptr && footer->objectName() == QStringLiteral("saveDialogFooter"),
            "save dialog custom footer is missing");
    auto* description = child<QLabel>(footer, "saveOutputDescription");
    auto* reject = modal->rejectButton();
    auto* accept = modal->acceptButton();
    const auto globalGeometry = [](QWidget* widget) {
        return QRect(widget->mapToGlobal(QPoint()), widget->size());
    };
    const QRect descriptionGeometry = globalGeometry(description);
    const QRect rejectGeometry = globalGeometry(reject);
    const QRect acceptGeometry = globalGeometry(accept);
    const QRegularExpression descriptionPattern(
        QStringLiteral("^160x100\\(100%\\) · \\d+(?:B|KB|MB|GB)$"));
    require(descriptionPattern.match(description->text()).hasMatch() &&
                std::abs(descriptionGeometry.center().y() - acceptGeometry.center().y()) <= 1 &&
                descriptionGeometry.right() < rejectGeometry.left() &&
                !descriptionGeometry.intersects(rejectGeometry) &&
                !descriptionGeometry.intersects(acceptGeometry),
            "output dimensions, scale and encoded size must occupy the footer left of its actions");
    const auto generation = [&] { return content->property("previewGeneration").toULongLong(); };
    const auto percentage = [&] { unit->setCurrentValue(QStringLiteral("percentage")); };
    const auto pixels = [&] { unit->setCurrentValue(QStringLiteral("pixels")); };
    const auto requirePixels = [&](QSize expected) {
        const auto before = generation();
        pixels();
        require(width->value() == expected.width() && height->value() == expected.height(),
                "unit conversion must preserve the exact pixel dimensions");
        percentage();
        require(generation() == before, "unit-only changes must not regenerate the export");
    };
    const auto initialGeneration = generation();
    require(width->value() == 160 && height->value() == 100 && width->decimals() == 0,
            "pixel mode must use physical source pixels even for high-DPI images");
    percentage();
    require(width->value() == 100 && height->value() == 100 && width->decimals() == 2 &&
                width->singleStep() == 1 && width->suffixText() == QStringLiteral("%") &&
                height->suffixText() == QStringLiteral("%") && generation() == initialGeneration,
            "percentage mode must initially show 100 percent without calculating again");
    width->setValue(50);
    require(height->value() == 50 && description->text().isEmpty(),
            "locked percentages must follow width edits and clear stale output details");
    processUntil([&] { return !description->text().isEmpty(); });
    require(QRegularExpression(QStringLiteral("^80x50\\(50%\\) · \\d+(?:B|KB|MB|GB)$"))
                .match(description->text())
                .hasMatch(),
            "output details must update to the latest dimensions, scale and encoded size");
    requirePixels(QSize(80, 50));
    height->setValue(25);
    require(width->value() == 25, "locked percentages must follow height edits");
    requirePixels(QSize(40, 25));
    width->setValue(12.5);
    requirePixels(QSize(20, 13));
    height->setValue(150);
    requirePixels(QSize(240, 150));
    lock->click();
    width->setValue(50);
    height->setValue(75);
    requirePixels(QSize(80, 75));
    lock->click();
    requirePixels(QSize(80, 50));
    width->setValue(0.01);
    requirePixels(QSize(1, 1));
    width->setValue(0);
    require(!modal->acceptButton()->isEnabled(), "zero percentage must prevent saving");
    width->setValue(50);
    height->clear();
    pixels();
    percentage();
    require(!height->hasValue() && !modal->acceptButton()->isEnabled(),
            "switching units must preserve an empty, invalid dimension");
    height->setValue(50);
    auto* format = child<AdSelect>(content, "saveFormatSelect");
    format->setCurrentValue(QStringLiteral("webp"));
    width->setValue(11000);
    require(!modal->acceptButton()->isEnabled(), "percentage must enforce encoder limits");
    width->setValue(50);
    format->setCurrentValue(QStringLiteral("png"));
    pixels();
    width->setValue(79);
    for (int i = 0; i < 5; ++i)
        requirePixels(QSize(79, 49));
    pixels();
    require(width->decimals() == 0 && width->suffixText().isEmpty(),
            "returning to pixels must restore integer inputs without percentage suffixes");

    auto* editor = width->findChild<QLineEdit*>();
    require(editor != nullptr, "size editor is missing");
    const auto typeWidth = [&](const QString& text) {
        editor->setFocus();
        editor->selectAll();
        for (const auto character : text) {
            QKeyEvent key(QEvent::KeyPress, character.unicode(), Qt::NoModifier,
                          QString(character));
            QApplication::sendEvent(editor, &key);
        }
    };
    processUntil([&] { return child<QLabel>(content, "savePreviewStatus")->isHidden(); });
    auto before = generation();
    typeWidth(QStringLiteral("64"));
    require(generation() == before && width->value() == 79,
            "typing a dimension must wait for a commit");
    percentage();
    processUntil([&] { return generation() > before; });
    require(width->value() == 40 && height->value() == 40 && generation() == before + 1,
            "switching units must commit pending pixels using the old unit exactly once");
    before = generation();
    typeWidth(QStringLiteral("25"));
    pixels();
    processUntil([&] { return generation() > before; });
    require(width->value() == 40 && height->value() == 25 && generation() == before + 1,
            "switching to pixels must first commit pending percentages");
    percentage();
    ExportTestTranslator translator;
    require(QApplication::installTranslator(&translator), "size translator unavailable");
    flush();
    require(child<QLabel>(content, "saveSizeLabel")->text() == QStringLiteral("Translated Size") &&
                unit->optionLabel(0) == QStringLiteral("Translated Pixels") &&
                unit->optionLabel(1) == QStringLiteral("Translated Percentage") &&
                unit->accessibleName() == QStringLiteral("Translated Size unit") &&
                width->toolTip() == QStringLiteral("Translated Width") &&
                height->accessibleName() == QStringLiteral("Translated Height") &&
                unit->currentValue() == QStringLiteral("percentage") && width->value() == 25 &&
                height->value() == 25,
            "size controls must retranslate without changing values or unit selection");
    QApplication::removeTranslator(&translator);
    flush();
    processUntil([&] { return child<QLabel>(content, "savePreviewStatus")->isHidden(); });
    snapshot(modal, QStringLiteral("export-percentage"));
    const auto translationDir = qEnvironmentVariable("SNOW_EXPORT_TEST_TRANSLATION_DIR");
    if (!translationDir.isEmpty()) {
        for (const auto& language : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
            QTranslator catalog;
            require(
                catalog.load(
                    QDir(translationDir).filePath(QStringLiteral("snow_shot_%1.qm").arg(language))),
                "export screenshot translation catalog is missing");
            require(QApplication::installTranslator(&catalog), "catalog installation failed");
            flush();
            auto* dimensions = child<QWidget>(content, "saveDimensionsForm");
            auto* label = child<QLabel>(content, "saveSizeLabel");
            require(label->geometry().right() < unit->geometry().left() &&
                        dimensions->rect().contains(unit->geometry()) &&
                        unit->width() >= unit->minimumSizeHint().width(),
                    "translated size header must fit without clipping");
            snapshot(modal, QStringLiteral("export-percentage-%1").arg(language));
            QApplication::removeTranslator(&catalog);
            flush();
        }
    }
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("percentage-save"));
    before = generation();
    typeWidth(QStringLiteral("37.5"));
    require(generation() == before, "pending percentage must not start a preview");
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(snow_shot::image_codec::inspectFile(savedPath, snow::image::Format::png, QSize(60, 38)),
            "saving must commit fractional percentages and write the resulting pixel size");
    flush();
    modal = openDialog(owner, fixture());
    require(child<AdSegmented>(modal->contentWidget(), "saveSizeUnitSegmented")->currentValue() ==
                QStringLiteral("pixels"),
            "reopening the dialog must default to pixels");
    modal->rejectButton()->click();
    flush();
}

void shortcutWrappingAndLanguageChange(QWidget& owner, const QTemporaryDir& temp) {
    const storage::ScreenshotSettings settings;
    require(
        settings.setSavePathShortcuts(
            {{QStringLiteral("A very long custom export destination name"), temp.filePath("one")},
             {QStringLiteral("Screenshots"), temp.filePath("two")},
             {QStringLiteral("Documents"), temp.filePath("three")},
             {QStringLiteral("Projects"), temp.filePath("four")}}),
        "wrapping setup failed");
    auto* modal = openDialog(owner, fixture(QSize(800, 500)));
    auto* content = modal->contentWidget();
    const auto first = child<AdButton>(content, "savePathShortcut_0");
    const auto last = child<AdButton>(content, "savePathShortcut_3");
    require(last->mapTo(content, QPoint()).y() > first->mapTo(content, QPoint()).y(),
            "custom path buttons must wrap onto multiple rows");
    require(first->width() <= 230 && first->parentWidget()->width() <= 254,
            "long shortcut names must not expand the form");
    const auto* host = first->parentWidget()->parentWidget();
    for (auto* button : host->findChildren<AdButton*>()) {
        const QRect bounds(button->mapTo(host, QPoint()), button->size());
        require(host->rect().contains(bounds), "wrapped shortcut buttons must not be clipped");
    }
    for (auto* input : content->findChildren<AdLineEdit*>())
        require(input->width() > 100, "form inputs must retain usable width");
    snapshot(modal, QStringLiteral("export-wrapped-shortcuts"));
    child<AdButton>(content, "savePathAddButton")->click();
    auto* editor = child<AdModal>(content, "savePathEditorModal");
    editor->acceptButton()->click();
    ExportTestTranslator translator;
    require(QApplication::installTranslator(&translator), "test translator unavailable");
    flush();
    require(modal->windowTitle() == QStringLiteral("Translated Save as file") &&
                editor->windowTitle() == QStringLiteral("Translated Add save path") &&
                child<AdLineEdit>(editor->contentWidget(), "savePathNameInput")->accessibleName() ==
                    QStringLiteral("Translated Name"),
            "open dialogs must retranslate captions and accessible names");
    const auto* browse = child<AdButton>(editor->contentWidget(), "savePathValueBrowseButton");
    require(browse->toolTip() == QStringLiteral("Translated Select save directory") &&
                browse->accessibleName() == browse->toolTip(),
            "shortcut browse button must retranslate its tooltip and accessible name");
    auto* quality = child<AdSlider>(content, "saveQualitySlider");
    require(
        quality->marks().size() == 2 &&
            quality->marks().value(quality->minimum()).label == QStringLiteral("Translated 0%") &&
            quality->marks().value(quality->maximum()).label == QStringLiteral("Translated 100%"),
        "quality endpoint descriptions must retranslate through the slider marks");
    auto* zoomHint = child<QLabel>(content, "savePreviewZoom");
    require(zoomHint->text().startsWith(QStringLiteral("Translated Scale: ")) &&
                zoomHint->isHidden(),
            "zoom hint must retranslate without becoming visible");
    const auto* form = qobject_cast<AdForm*>(editor->contentWidget());
    require(form->itemForName(QStringLiteral("name"))
                ->errorMessages()
                .contains(QStringLiteral("Translated Please enter a name")),
            "visible validation must retranslate with its dialog");
    QApplication::removeTranslator(&translator);
    flush();
    editor->rejectButton()->click();
    modal->rejectButton()->click();
    flush();
    require(settings.setSavePathShortcuts({}), "wrapping cleanup failed");
}

void overwriteRequiresConfirmation(QWidget& owner, const QTemporaryDir& temp) {
    const QString destination = temp.filePath(QStringLiteral("existing.png"));
    QFile existing(destination);
    require(existing.open(QIODevice::WriteOnly) && existing.write("existing") == 8,
            "overwrite fixture unavailable");
    existing.close();
    QString saved;
    auto* modal = openDialog(owner, fixture(), [&](const QString& path) { saved = path; });
    auto* content = modal->contentWidget();
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("existing.jpg"));
    modal->acceptButton()->click();
    auto* confirmation = child<AdModal>(content, "saveOverwriteModal");
    require(confirmation->isOpen(), "normalized destination must trigger overwrite confirmation");
    confirmation->rejectButton()->click();
    flush();
    require(modal->isOpen() && saved.isEmpty() && existing.open(QIODevice::ReadOnly),
            "canceling overwrite must retain the export form");
    require(existing.readAll() == QByteArray("existing"), "canceling overwrite modified the file");
    existing.close();
    modal->acceptButton()->click();
    child<AdModal>(content, "saveOverwriteModal")->acceptButton()->click();
    processUntil([&] { return !saved.isEmpty(); });
    require(saved == destination && snow_shot::image_codec::inspectFile(
                                        saved, snow::image::Format::png, QSize(160, 100)),
            "confirmed overwrite must publish the selected encoding");
    flush();
}

void rowBackedDialogAndStalePreview(QWidget& owner, const QTemporaryDir& temp) {
    auto materializations = std::make_shared<std::atomic_int>(0);
    const auto rows = snow_shot::image_codec::srgbRowSource(fixture(QSize(64, 3000)));
    auto artifact = std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromProducer(
        [materializations](const ScreenshotExportCancellation&) {
            ++*materializations;
            return QImage{};
        },
        [rows](std::function<bool()>) { return rows; }));
    QString savedPath;
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact,
                                             [&](const QString& path) { savedPath = path; }),
            "row-backed dialog must open");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    auto* content = modal->contentWidget();
    processUntil([&] { return content->property("previewGeneration").toULongLong() > 0; });
    flush();
    require(materializations->load() == 0, "scrolling preview must not materialize the image");

    // Occupy both workers so a preview remains pending while newer form state arrives.
    struct WorkerGate {
        std::shared_ptr<std::atomic_bool> released = std::make_shared<std::atomic_bool>(false);
        ~WorkerGate() {
            *released = true;
        }
    } gate;
    for (int worker = 0; worker < 2; ++worker) {
        const auto handle = ScreenshotExportCoordinator::shared().submit(
            &owner, ScreenshotExportCoordinator::Priority::Foreground,
            [released = gate.released](const ScreenshotExportCancellation& cancellation) {
                while (!released->load() && !cancellation.isCancellationRequested())
                    QThread::msleep(1);
                return ScreenshotExportTaskResult{};
            },
            [](ScreenshotExportTaskResult) {});
        require(handle.isValid(), "worker gate could not be queued");
    }
    const auto previous = content->property("previewGeneration").toULongLong();
    auto* width = child<AdInputNumber>(content, "saveWidthInput");
    width->setValue(32);
    processUntil([&] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 3; });
    auto* lock = child<AdButton>(content, "saveAspectLockButton");
    lock->click();
    lock->click();
    width->setValue(64);
    require(child<QLabel>(content, "savePreviewStatus")->isHidden() &&
                content->property("previewGeneration").toULongLong() == previous,
            "returning to the completed preview must cancel queued work and reuse its pixels");
    width->setValue(48);
    require(!child<QLabel>(content, "savePreviewStatus")->isHidden(),
            "different full dimensions must remain pending after queued cancellation");
    width->setValue(24);
    *gate.released = true;
    processUntil(
        [&] { return content->property("previewGeneration").toULongLong() >= previous + 2; });
    require(child<AdInputNumber>(content, "saveHeightInput")->value() == 1125,
            "stale preview must never replace latest dimensions");
    width->setValue(48);
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.filePath("scrolling"));
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("scroll.png"));
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(materializations->load() == 0 &&
                snow_shot::image_codec::inspectFile(savedPath, snow::image::Format::png,
                                                    QSize(48, 2250)),
            "row-backed Save must use the full source and latest settings");
    require(!artifact->isCancelled(),
            "successful export must leave its artifact available to history");
    flush();
}

struct ExportProbe {
    std::atomic_int passes{0};
    std::atomic_bool pauseNext{false};
    std::atomic_bool entered{false};
    std::atomic_bool released{false};
    std::atomic_bool failNext{false};
};

struct WorkerGate {
    std::shared_ptr<std::atomic_bool> released = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_int> started = std::make_shared<std::atomic_int>(0);
    ~WorkerGate() {
        *released = true;
    }
    void block(QObject* receiver, int count = 2) {
        for (int worker = 0; worker < count; ++worker) {
            const auto job = ScreenshotExportCoordinator::shared().submit(
                receiver, ScreenshotExportCoordinator::Priority::Foreground,
                [release = released, start = started](const ScreenshotExportCancellation& token) {
                    ++*start;
                    while (!release->load() && !token.isCancellationRequested())
                        QThread::msleep(1);
                    return ScreenshotExportTaskResult{};
                },
                [](ScreenshotExportTaskResult) {});
            require(job.isValid(), "export worker gate must fit in the queue");
        }
    }
};

class ExportObserver final : public QObject {
  public:
    explicit ExportObserver(QWidget* content) : m_content(content) {
        m_encodedGeneration = content->property("encodedGeneration").toULongLong();
        m_previewGeneration = content->property("previewGeneration").toULongLong();
        content->installEventFilter(this);
    }
    int encodes = 0;
    int publications = 0;
    QImage displayed;
    std::function<void()> onEncoded;

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() != QEvent::DynamicPropertyChange)
            return false;
        const auto name = static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();
        if (name == "encodedGeneration") {
            const auto generation = m_content->property("encodedGeneration").toULongLong();
            if (generation == m_encodedGeneration)
                return false;
            m_encodedGeneration = generation;
            ++encodes;
            if (onEncoded)
                onEncoded();
        } else if (name == "previewGeneration") {
            const auto generation = m_content->property("previewGeneration").toULongLong();
            if (generation == m_previewGeneration)
                return false;
            m_previewGeneration = generation;
            ++publications;
            displayed =
                child<ScreenshotSavePreviewCanvas>(m_content, "savePreviewCanvas")->outputImage();
        }
        return false;
    }

  private:
    QWidget* m_content;
    quint64 m_encodedGeneration = 0;
    quint64 m_previewGeneration = 0;
};

AdModal* openCountedDialog(QWidget& owner, const std::shared_ptr<ExportProbe>& probe,
                           ScreenshotSaveAsFileDialog::Saved saved = {}) {
    // Pipeline pass counts require a PNG baseline independent of earlier saves.
    require(storage::ScreenshotSettings().setLastManualSaveFormat(QStringLiteral("png")),
            "pipeline format setup failed");
    auto rows = snow_shot::image_codec::srgbRowSource(fixture());
    rows.readRows = [read = rows.readRows, probe](int first, int count, qsizetype stride,
                                                  uchar* target, qsizetype capacity) {
        if (first == 0) {
            ++probe->passes;
            if (probe->pauseNext.exchange(false)) {
                probe->entered = true;
                while (!probe->released.load())
                    QThread::msleep(1);
            }
            if (probe->failNext.exchange(false))
                return false;
        }
        return read(first, count, stride, target, capacity);
    };
    auto artifact = std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromProducer(
        [](const ScreenshotExportCancellation&) -> QImage {
            throw std::runtime_error("row-backed export must not request a second image");
        },
        [rows](std::function<bool()>) { return rows; }));
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact, std::move(saved)),
            "counted export dialog must open");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    processUntil(
        [&] { return modal->contentWidget()->property("previewGeneration").toULongLong() > 0; });
    require(probe->passes == 1,
            "initial image-backed display must encode from its retained pixels only once");
    return modal;
}

void optimizedPipelineBehavior(QWidget& owner) {
    const QImage image = fixture();
    ScreenshotImageRowSource rows = snow_shot::image_codec::srgbRowSource(image);
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto released = std::make_shared<std::atomic_bool>(false);
    const auto releaseEncoding = qScopeGuard([released] { *released = true; });
    rows.readRows = [read = rows.readRows, entered, released](
                        int first, int count, qsizetype stride, uchar* target, qsizetype capacity) {
        entered->store(true, std::memory_order_release);
        while (!released->load(std::memory_order_acquire))
            QThread::msleep(1);
        return read(first, count, stride, target, capacity);
    };
    auto artifact = std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromProducer(
        {}, [rows](std::function<bool()> cancellation) mutable {
            rows.cancellationRequested = std::move(cancellation);
            return rows;
        }));
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact),
            "optimized pipeline dialog did not open");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    QPointer<QWidget> content = modal->contentWidget();
    const auto closeDialog = qScopeGuard([&] {
        if (content) {
            modal->reject();
            flush();
        }
    });
    processUntil([&] { return entered->load(std::memory_order_acquire); });
    auto* canvas = child<ScreenshotSavePreviewCanvas>(content, "savePreviewCanvas");
    auto* description = child<QLabel>(modal->footerWidget(), "saveOutputDescription");
    require(content->property("previewGeneration").toULongLong() == 0 &&
                !child<QLabel>(content, "savePreviewStatus")->isHidden() &&
                samePixels(canvas->outputImage(), image) && description->text().isEmpty(),
            "source preview was not published provisionally while encoding remained busy");
    require(content->property("previewDecodeCount").toInt() == 0,
            "provisional preview unexpectedly started a decode");
    ExportTestTranslator translator;
    require(QApplication::installTranslator(&translator), "pending export translator unavailable");
    flush();
    require(description->text().isEmpty(),
            "language changes must not restore stale output details while encoding");
    QApplication::removeTranslator(&translator);
    flush();

    released->store(true, std::memory_order_release);
    processUntil([&] { return content->property("previewGeneration").toULongLong() > 0; });
    require(child<QLabel>(content, "savePreviewStatus")->isHidden() &&
                content->property("previewDecodeCount").toInt() == 0 &&
                samePixels(canvas->outputImage(), image),
            "exact PNG preview did not reuse retained prepared pixels");
    const qulonglong preparedIdentity = content->property("preparedPixelsIdentity").toULongLong();
    require(preparedIdentity != 0, "dialog did not retain its prepared pixel source");

    auto* format = child<AdSelect>(content, "saveFormatSelect");
    auto* quality = child<AdSlider>(content, "saveQualitySlider");
    const auto selectFormat = [&](const QString& value) {
        const quint64 previous = content->property("previewGeneration").toULongLong();
        format->setCurrentValue(value);
        processUntil(
            [&] { return content->property("previewGeneration").toULongLong() > previous; });
    };
    selectFormat(QStringLiteral("webp"));
    selectFormat(QStringLiteral("jxl"));
    require(content->property("previewDecodeCount").toInt() == 0 &&
                content->property("preparedPixelsIdentity").toULongLong() == preparedIdentity,
            "lossless WebP or JPEG XL did not reuse exact prepared pixels");

    selectFormat(QStringLiteral("avif"));
    require(content->property("previewDecodeCount").toInt() == 1,
            "AVIF did not decode its codec artifact");
    selectFormat(QStringLiteral("jpeg"));
    require(content->property("previewDecodeCount").toInt() == 2 &&
                content->property("preparedPixelsIdentity").toULongLong() == preparedIdentity,
            "JPEG did not decode its artifact or reused-size pixels were replaced");

    quint64 previous = content->property("previewGeneration").toULongLong();
    quality->setValue(72);
    processUntil([&] { return content->property("previewGeneration").toULongLong() > previous; });
    selectFormat(QStringLiteral("webp"));
    selectFormat(QStringLiteral("jxl"));
    require(content->property("previewDecodeCount").toInt() == 5 &&
                content->property("preparedPixelsIdentity").toULongLong() == preparedIdentity,
            "lossy same-size exports did not reuse prepared pixels and decode codec artifacts");
}

void unbackedExactPreviewDecodesArtifact(QWidget& owner) {
    const QImage image = fixture();
    ScreenshotImageRowSource rows = snow_shot::image_codec::srgbRowSource(image);
    rows.backingImage = {};
    auto artifact = std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromProducer(
        {}, [rows](std::function<bool()> cancellation) mutable {
            rows.cancellationRequested = std::move(cancellation);
            return rows;
        }));
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact),
            "unbacked exact-preview dialog did not open");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    QPointer<QWidget> content = modal->contentWidget();
    processUntil([&] { return content->property("previewGeneration").toULongLong() > 0; });
    require(content->property("previewDecodeCount").toInt() == 1 &&
                samePixels(
                    child<ScreenshotSavePreviewCanvas>(content, "savePreviewCanvas")->outputImage(),
                    image),
            "source-sized exact output without backing pixels did not decode its artifact");
    modal->reject();
    flush();
}

void sourceSizedPngAdoptsHistoryEncoding(QWidget& owner, const QTemporaryDir& temp) {
    const QImage image = fixture();
    auto reads = std::make_shared<std::atomic_int>(0);
    ScreenshotImageRowSource rows = snow_shot::image_codec::srgbRowSource(image);
    rows.readRows = [read = rows.readRows, reads](int first, int count, qsizetype stride,
                                                  uchar* target, qsizetype capacity) {
        ++*reads;
        return read(first, count, stride, target, capacity);
    };
    auto artifact = std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromProducer(
        {}, [rows](std::function<bool()> cancellation) mutable {
            rows.cancellationRequested = std::move(cancellation);
            return rows;
        }));

    QString savedPath;
    QByteArray historyPng;
    int historyCallbacks = 0;
    require(ScreenshotSaveAsFileDialog::open(
                &owner, &owner, artifact,
                [&, artifact](const QString& path) {
                    savedPath = path;
                    require(artifact->requestCanonicalPng(
                                &owner,
                                [&](ScreenshotExportEncodingResult result) {
                                    require(result.succeeded(),
                                            "history canonical PNG request failed after save");
                                    historyPng = result.image.bytes();
                                    ++historyCallbacks;
                                }),
                            "history canonical PNG request was rejected after save");
                }),
            "history-adoption export dialog did not open");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    QPointer<QWidget> content = modal->contentWidget();
    processUntil([&] { return content->property("previewGeneration").toULongLong() > 0; });
    const int readsBeforeSave = reads->load();
    require(readsBeforeSave > 0, "initial source-sized PNG did not read its row source");

    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("history-adoption"));
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty() && historyCallbacks == 1; });

    QFile saved(savedPath);
    require(reads->load() == readsBeforeSave && saved.open(QIODevice::ReadOnly) &&
                saved.readAll() == historyPng,
            "source-sized PNG save did not seed history with the retained encoded artifact");
    flush();
}

void saveReusesCalculatedResult(QWidget& owner, const QTemporaryDir& temp) {
    enum class Moment { QueuedEncode, RunningEncode, BeforeDecode, QueuedDecode, Displayed };
    for (const auto moment : {Moment::QueuedEncode, Moment::RunningEncode, Moment::BeforeDecode,
                              Moment::QueuedDecode, Moment::Displayed}) {
        auto probe = std::make_shared<ExportProbe>();
        const auto releaseSource = qScopeGuard([probe] { probe->released = true; });
        QString savedPath;
        auto* modal =
            openCountedDialog(owner, probe, [&](const QString& path) { savedPath = path; });
        QPointer<QWidget> content = modal->contentWidget();
        const auto closeDialog = qScopeGuard([&] {
            if (content)
                modal->reject();
        });
        const auto format = moment == Moment::QueuedDecode ? Format::Jpeg : Format::Png;
        if (format == Format::Jpeg) {
            const auto previewGeneration = content->property("previewGeneration").toULongLong();
            child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("jpeg"));
            processUntil([&] {
                return content->property("previewGeneration").toULongLong() > previewGeneration;
            });
        }
        ExportObserver observer(content);
        WorkerGate gate;
        child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
        child<AdLineEdit>(content, "saveFilenameInput")
            ->setText(QStringLiteral("reuse-%1").arg(int(moment)));
        int publicationsAtSave = -1;
        const auto save = [&] {
            publicationsAtSave = observer.publications;
            modal->acceptButton()->click();
            modal->acceptButton()->click();
        };
        if (moment == Moment::QueuedEncode) {
            gate.block(&owner);
            processUntil([&] { return *gate.started == 2; });
        } else if (moment == Moment::RunningEncode) {
            probe->pauseNext = true;
        } else if (moment == Moment::BeforeDecode) {
            observer.onEncoded = save;
        } else if (moment == Moment::QueuedDecode) {
            observer.onEncoded = [&] {
                gate.block(&owner);
                processUntil([&] { return *gate.started == 2; });
            };
        }
        child<AdInputNumber>(content, "saveWidthInput")->setValue(80);
        if (moment == Moment::QueuedEncode) {
            require(ScreenshotExportCoordinator::shared().pendingJobCount() == 3,
                    "the calculation must be queued behind the worker gates");
            save();
            *gate.released = true;
        } else if (moment == Moment::RunningEncode) {
            processUntil([&] { return probe->entered.load(); });
            save();
            probe->released = true;
        } else if (moment == Moment::QueuedDecode) {
            processUntil([&] {
                return *gate.started == 2 &&
                       ScreenshotExportCoordinator::shared().pendingJobCount() == 3;
            });
            save();
            *gate.released = true;
        } else if (moment == Moment::Displayed) {
            processUntil([&] { return observer.publications == 1; });
            save();
        }
        processUntil([&] { return !savedPath.isEmpty(); });
        const int expectedPasses = moment == Moment::QueuedDecode ? 3 : 2;
        if (probe->passes != expectedPasses || observer.encodes != 1)
            std::cerr << "Save moment " << int(moment) << ": source passes=" << probe->passes
                      << ", encodes=" << observer.encodes
                      << ", publications=" << observer.publications << '\n';
        require(
            probe->passes == expectedPasses && observer.encodes == 1,
            "Save must reuse the single committed calculation without reading source pixels again");
        require(publicationsAtSave >= 0 && observer.publications == publicationsAtSave,
                "Save must suppress all subsequent canvas publication");
        const QImage saved = snow_shot::image_codec::decodeFile(
            savedPath, ScreenshotImageFileService::snowImageFormat(format));
        require(saved.size() == QSize(80, 50), "saving must use the latest committed dimensions");
        if (moment == Moment::Displayed)
            require(samePixels(saved, observer.displayed),
                    "saved pixels must equal the displayed decoded result");
        flush();
    }
}

void committedControlsAndSave(QWidget& owner, const QTemporaryDir& temp) {
    auto probe = std::make_shared<ExportProbe>();
    QString savedPath;
    auto* modal = openCountedDialog(owner, probe, [&](const QString& path) { savedPath = path; });
    auto* content = modal->contentWidget();
    ExportObserver observer(content);
    auto* width = child<AdInputNumber>(content, "saveWidthInput");
    auto* height = child<AdInputNumber>(content, "saveHeightInput");
    auto* quality = child<AdSlider>(content, "saveQualitySlider");
    require(!width->keyboardTracking() && !height->keyboardTracking() && !quality->tracking(),
            "export controls must calculate only committed values");
    auto* editor = width->findChild<QLineEdit*>();
    require(editor != nullptr, "width editor is missing");
    const auto typeWidth = [&](const QString& text) {
        editor->setFocus();
        editor->selectAll();
        for (const auto character : text) {
            QKeyEvent key(QEvent::KeyPress, character.unicode(), Qt::NoModifier,
                          QString(character));
            QApplication::sendEvent(editor, &key);
        }
    };
    typeWidth(QStringLiteral("80"));
    require(width->value() == 160 && probe->passes == 1 && observer.encodes == 0,
            "uncommitted numeric typing must not trigger encoding");
    editor->clearFocus();
    processUntil([&] { return observer.publications == 1; });
    require(width->value() == 80 && height->value() == 50 && probe->passes == 2,
            "committing width and its aspect-ratio partner must calculate exactly once");
    child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("jpeg"));
    processUntil([&] { return observer.publications == 2; });
    const int beforeDrag = probe->passes;
    const QPoint start = quality->rect().center();
    const QPoint end(quality->width() / 3, start.y());
    QMouseEvent press(QEvent::MouseButtonPress, start, quality->mapToGlobal(start), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, end, quality->mapToGlobal(end), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, end, quality->mapToGlobal(end), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(quality, &press);
    QApplication::sendEvent(quality, &move);
    require(probe->passes == beforeDrag && quality->value() == 100,
            "slider dragging must not encode intermediate values");
    QApplication::sendEvent(quality, &release);
    processUntil([&] { return observer.publications == 3; });
    require(quality->value() < 100 && probe->passes == beforeDrag,
            "same-size quality changes must reuse the prepared pixels");
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("commit-on-save"));
    typeWidth(QStringLiteral("64"));
    require(width->value() == 80, "the last numeric edit must still be uncommitted");
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(observer.encodes == 4 && observer.publications == 3 &&
                snow_shot::image_codec::inspectFile(savedPath, snow::image::Format::jpeg,
                                                    QSize(64, 40)),
            "Save must commit the active editor and save once without updating the canvas");
    flush();
}

void retainedResultFailures(QWidget& owner, const QTemporaryDir& temp) {
    auto probe = std::make_shared<ExportProbe>();
    QString savedPath;
    auto* modal = openCountedDialog(owner, probe, [&](const QString& path) { savedPath = path; });
    auto* content = modal->contentWidget();
    const auto previewGeneration = content->property("previewGeneration").toULongLong();
    child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("jpeg"));
    processUntil(
        [&] { return content->property("previewGeneration").toULongLong() > previewGeneration; });
    ExportObserver observer(content);
    WorkerGate gate;
    observer.onEncoded = [&] { gate.block(&owner, 16); };
    child<AdInputNumber>(content, "saveWidthInput")->setValue(80);
    auto* error = child<QLabel>(content, "saveErrorLabel");
    processUntil([&] { return observer.encodes == 1 && !error->isHidden(); });
    require(observer.publications == 0 && modal->acceptButton()->isEnabled(),
            "a rejected display job must retain the encoded result and allow Save");
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("retained-failure"));
    modal->acceptButton()->click();
    require(!modal->acceptButtonBusy() && savedPath.isEmpty(),
            "a rejected save job must restore the dialog for retry");
    *gate.released = true;
    processUntil([&] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; });
    flush();
    QFile blocking(temp.filePath(QStringLiteral("retained-blocker")));
    require(blocking.open(QIODevice::WriteOnly), "write failure fixture could not be created");
    blocking.close();
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(blocking.fileName() + "/child");
    modal->acceptButton()->click();
    processUntil([&] { return !modal->acceptButtonBusy(); });
    require(!error->isHidden() && savedPath.isEmpty() && probe->passes == 3,
            "destination failure must preserve the already encoded file");
    processUntil([&] { return observer.publications == 1; });
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(observer.encodes == 1 && probe->passes == 3,
            "retrying after display, queue and destination failures must not encode again");
    flush();
}

void failedAndClosedCalculations(QWidget& owner, const QTemporaryDir& temp) {
    auto probe = std::make_shared<ExportProbe>();
    const auto releaseSource = qScopeGuard([probe] { probe->released = true; });
    QString savedPath;
    auto* modal = openCountedDialog(owner, probe, [&](const QString& path) { savedPath = path; });
    auto* content = modal->contentWidget();
    ExportObserver observer(content);
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("retry-source"));
    probe->pauseNext = true;
    probe->failNext = true;
    child<AdInputNumber>(content, "saveWidthInput")->setValue(80);
    processUntil([&] { return probe->entered.load(); });
    modal->acceptButton()->click();
    probe->released = true;
    processUntil([&] { return !modal->acceptButtonBusy(); });
    require(!child<QLabel>(content, "saveErrorLabel")->isHidden() && observer.encodes == 0 &&
                savedPath.isEmpty(),
            "failed source reads must leave Save retryable without publishing stale pixels");
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(observer.encodes == 1 && observer.publications == 0 && probe->passes == 3,
            "a failed calculation may retry once and must save the new successful result");
    flush();

    probe = std::make_shared<ExportProbe>();
    const auto releaseClosedSource = qScopeGuard([probe] { probe->released = true; });
    modal = openCountedDialog(owner, probe);
    QPointer<QWidget> guarded = modal->contentWidget();
    ExportObserver closedObserver(guarded);
    probe->pauseNext = true;
    child<AdInputNumber>(guarded, "saveWidthInput")->setValue(80);
    processUntil([&] { return probe->entered.load(); });
    modal->reject();
    flush();
    probe->released = true;
    processUntil([&] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; });
    flush();
    require(guarded.isNull() && closedObserver.encodes == 0 && closedObserver.publications == 0,
            "closing during calculation must discard worker completion and release the dialog");
}

void rejectedCalculationRetries(QWidget& owner, const QTemporaryDir& temp) {
    auto probe = std::make_shared<ExportProbe>();
    QString savedPath;
    auto* modal = openCountedDialog(owner, probe, [&](const QString& path) { savedPath = path; });
    auto* content = modal->contentWidget();
    ExportObserver observer(content);
    WorkerGate gate;
    gate.block(&owner, 16);
    child<AdInputNumber>(content, "saveWidthInput")->setValue(80);
    require(!child<QLabel>(content, "saveErrorLabel")->isHidden() && probe->passes == 1 &&
                observer.encodes == 0 && modal->acceptButton()->isEnabled(),
            "a rejected calculation must preserve the latest options and allow retry");
    *gate.released = true;
    processUntil([&] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; });
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("retry-queue"));
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(probe->passes == 2 && observer.encodes == 1 && observer.publications == 0,
            "Save must retry a rejected calculation once and skip canvas publication");
    flush();
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    adqt::theme::ThemeManager::instance().applyTo(app);
    QTemporaryDir temp;
    try {
        require(temp.isValid() && QDir().mkpath(temp.filePath("app")), "test storage unavailable");
        require(storage::ApplicationStorage::instance()
                    .initialize({temp.filePath("app"), temp.filePath("data"), 60000})
                    .success,
                "test storage initialization failed");
        if (app.arguments().contains(QStringLiteral("--canvas"))) {
            canvasInteraction();
            canvasZoomHint();
            canvasOriginalSizeBaseline();
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Export preview canvas tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--placement"))) {
            centersOnDisplayOverlay();
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Export dialog placement tests passed\n";
            return 0;
        }
        QWidget owner;
        owner.resize(1200, 800);
        owner.show();
        if (app.arguments().contains(QStringLiteral("--remember-format"))) {
            rememberedManualFormat(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Remembered manual save format tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--shared-export"))) {
            encodingAndFullResolutionDisplay();
            saveReusesCalculatedResult(owner, temp);
            committedControlsAndSave(owner, temp);
            retainedResultFailures(owner, temp);
            failedAndClosedCalculations(owner, temp);
            rejectedCalculationRetries(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Shared export calculation tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--optimized-pipeline"))) {
            optimizedPipelineBehavior(owner);
            unbackedExactPreviewDecodesArtifact(owner);
            sourceSizedPngAdoptsHistoryEncoding(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Optimized export pipeline tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--zoom-hint")) ||
            app.arguments().contains(QStringLiteral("--canvas-baseline")) ||
            app.arguments().contains(QStringLiteral("--unchanged-edits")) ||
            app.arguments().contains(QStringLiteral("--row-backed-preview"))) {
            if (app.arguments().contains(QStringLiteral("--zoom-hint")))
                canvasZoomHint();
            if (app.arguments().contains(QStringLiteral("--canvas-baseline")))
                canvasOriginalSizeBaseline();
            if (app.arguments().contains(QStringLiteral("--unchanged-edits")))
                unchangedPreviewEdits(owner);
            if (app.arguments().contains(QStringLiteral("--row-backed-preview")))
                rowBackedDialogAndStalePreview(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Export preview regression tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--preview-and-save"))) {
            previewAndSave(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Export preview and save tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--size-units"))) {
            sizeUnits(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Export dialog size unit tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--shortcuts"))) {
            shortcutPopupInteraction(owner, temp);
            shortcutsAndCancellation(owner, temp);
            shortcutWrappingAndLanguageChange(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Export dialog shortcut tests passed\n";
            return 0;
        }
        shortcutPopupInteraction(owner, temp);
        centersOnDisplayOverlay();
        reusablePathInputsAndSettings();
        persistence(temp);
        stateRules(temp);
        rememberedManualFormat(owner, temp);
        encodingAndFullResolutionDisplay();
        canvasInteraction();
        canvasZoomHint();
        canvasOriginalSizeBaseline();
        unchangedPreviewEdits(owner);
        shortcutsAndCancellation(owner, temp);
        previewAndSave(owner, temp);
        sizeUnits(owner, temp);
        overwriteRequiresConfirmation(owner, temp);
        shortcutWrappingAndLanguageChange(owner, temp);
        rowBackedDialogAndStalePreview(owner, temp);
        optimizedPipelineBehavior(owner);
        unbackedExactPreviewDecodesArtifact(owner);
        sourceSizedPngAdoptsHistoryEncoding(owner, temp);
        saveReusesCalculatedResult(owner, temp);
        committedControlsAndSave(owner, temp);
        retainedResultFailures(owner, temp);
        failedAndClosedCalculations(owner, temp);
        rejectedCalculationRetries(owner, temp);
        pendingCancellation(owner);
        ScreenshotExportCoordinator::shared().shutdown();
        storage::ApplicationStorage::instance().shutdown();
        std::cout << "Export dialog workflow tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        ScreenshotExportCoordinator::shared().shutdown();
        storage::ApplicationStorage::instance().shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
