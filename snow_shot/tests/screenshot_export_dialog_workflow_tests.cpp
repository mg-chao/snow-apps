#include "physical_key_test_support.h"
#include "snow_shot/presentation/screenshotsaveasfiledialog.h"
#include "snow_shot/presentation/screenshotsavepreviewcanvas.h"
#include "snow_shot/presentation/canvasstatusreadout.h"
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
#include "widgets/popover.h"
#include "theme/theme_manager.h"
#ifdef Q_OS_MACOS
#include "macos_native_input.h"
#endif

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
#include <QSemaphore>
#include <QTranslator>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <utility>

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
    SettingsPageWidget storagePage(registry, QStringLiteral("files-history"), session);
    storagePage.reveal({storagePage.pageId(), QStringLiteral("screen-recording-output"), {}});
    const auto directoryControls = storagePage.findChildren<DirectoryPathInput*>();
    require(directoryControls.size() == 2,
            "all screenshot and recording directory settings must use DirectoryPathInput");
    for (DirectoryPathInput* control : directoryControls) {
        require(qobject_cast<FilePathInput*>(control) == nullptr &&
                    adqt::icons::describeIcon(control->browseButton()->iconRef()).key.name ==
                        QStringLiteral("folder-open"),
                "directory settings must use the directory-specific path control");
    }

    SettingsPageWidget desktopPage(registry, QStringLiteral("desktop-tools"), session);
    desktopPage.reveal({desktopPage.pageId(), QStringLiteral("tray"), {}});
    const auto fileControls = desktopPage.findChildren<FilePathInput*>();
    const auto expectedFileControls =
        std::count_if(registry.fields().cbegin(), registry.fields().cend(), [](const auto& field) {
            return field.pageId == QStringLiteral("desktop-tools") &&
                   field.sectionId == QStringLiteral("tray") &&
                   field.kind == settings::SettingsFieldKind::FilePath;
        });
    require(expectedFileControls > 0 && fileControls.size() == expectedFileControls,
            "every registered tray file setting must use FilePathInput");
    for (auto* control : fileControls) {
        require(adqt::icons::describeIcon(control->browseButton()->iconRef()).key.name ==
                    QStringLiteral("file-add"),
                "file settings must use FileAddOutlined");
    }
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
                    ScreenshotSaveAsFileDialog::Finished finished = {},
                    QWidget* stackingOwner = nullptr) {
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, image, std::move(saved),
                                             std::move(finished), stackingOwner),
            "image dialog should open");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    require(modal->mode() == AdModal::Mode::Window &&
                modal->windowModality() == Qt::ApplicationModal && !modal->maskVisible() &&
                modal->closePolicy() == AdModal::ClosePolicy::Manual,
            "export must use application-modal Ant window mode");
    processUntil(
        [&] { return modal->contentWidget()->property("previewGeneration").toULongLong() > 0; });
    flush();
    for (const char* name : {"saveWidthInput", "saveHeightInput", "saveAspectLockButton"}) {
        auto* widget = child<QWidget>(modal->contentWidget(), name);
        if (widget->isEnabled())
            continue;
        for (auto* ancestor = widget; ancestor; ancestor = ancestor->parentWidget()) {
            std::cerr << "Disabled control ancestor: " << ancestor->metaObject()->className()
                      << " name=" << ancestor->objectName().toStdString()
                      << " enabled=" << ancestor->isEnabled()
                      << " forceDisabled=" << ancestor->testAttribute(Qt::WA_ForceDisabled);
            if (const auto* form = qobject_cast<AdForm*>(ancestor))
                std::cerr << " formDisabled=" << form->disabled();
            std::cerr << '\n';
        }
    }
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

void collageWindowModalityAndRetry(QWidget& owner) {
    QWidget neighbor;
    neighbor.show();
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto artifact = std::make_shared<ScreenshotExportArtifact>(
            ScreenshotExportSource::fromImage(fixture()));
        int completions = 0;
        bool saved = true;
        require(ScreenshotSaveAsFileDialog::open(
                    &owner, &owner, artifact, {},
                    [&](bool completedSave) {
                        ++completions;
                        saved = completedSave;
                    },
                    &owner, Qt::WindowModal),
                "collage save dialog accepts a fresh artifact on each attempt");
        auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
        processUntil([&] {
            return modal->contentWidget()->property("previewGeneration").toULongLong() > 0;
        });
        require(modal->windowModality() == Qt::WindowModal && modal->ownerWindow() == &owner &&
                    modal->contentWidget()->window()->windowModality() == Qt::WindowModal &&
                    neighbor.isVisible() && neighbor.isEnabled(),
                "collage save is scoped to its owner and leaves other windows usable");
        modal->reject();
        processUntil([&] { return completions == 1; });
        require(!saved && artifact->isCancelled(),
                "canceling the custom save dialog reports cancellation and retires its artifact");
        flush();
        require(owner.findChild<AdModal*>(QStringLiteral("screenshotSaveAsFileModal")) == nullptr,
                "the dismissed custom dialog is released before a fresh retry");
    }
    auto* defaultModal = openDialog(owner, fixture());
    defaultModal->reject();
    flush();
}

void saveDialogKeepsToolbarVisible() {
    for (const bool transientOnly : {false, true}) {
        QWidget overlay(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        overlay.setGeometry(QApplication::primaryScreen()->geometry());
        overlay.show();
        QWidget toolbar(transientOnly ? nullptr : &overlay, Qt::Tool | Qt::FramelessWindowHint |
                                                                Qt::WindowStaysOnTopHint |
                                                                Qt::WindowDoesNotAcceptFocus);
        toolbar.setGeometry(QRect(overlay.geometry().center(), QSize(300, 80)));
        toolbar.show();
        if (transientOnly)
            toolbar.windowHandle()->setTransientParent(overlay.windowHandle());
        QWidget hiddenToolbar(&overlay, Qt::Tool);
        hiddenToolbar.hide();
        flush();

        auto* modal = openDialog(overlay, fixture(), {}, {}, &toolbar);
        QWidget* surface = modal->contentWidget()->window();
        require(surface->windowHandle()->transientParent() == toolbar.windowHandle(),
                "save must use its toolbar as the native stacking owner");
        QTimer::singleShot(0, &toolbar, [&toolbar] { toolbar.raise(); });
        flush();
        require(toolbar.isVisible() && toolbar.windowHandle()->isVisible(),
                "the screenshot toolbar must remain visible throughout Save as File");
        require(!toolbar.testAttribute(Qt::WA_DontShowOnScreen),
                "saving must not alter the toolbar's native visibility attributes");
#ifdef Q_OS_MACOS
        if (QApplication::platformName() == QStringLiteral("cocoa")) {
            // Cocoa's native modal ordering can differ from Qt's topLevelAt()
            // when the dialog's transient toolbar differs from its QWidget owner.
            require(macWindowReceivesPoint(surface, toolbar.geometry().center()),
                    "the native save dialog must remain above its toolbar after a queued raise");
        } else
#endif
            if (QApplication::platformName() != QStringLiteral("offscreen")) {
            require(QApplication::topLevelAt(toolbar.geometry().center()) == surface,
                    "the save dialog must remain above its toolbar after a queued raise");
        }
        const QPoint offset = surface->geometry().center() - overlay.geometry().center();
        require(std::abs(offset.x()) <= 1 && std::abs(offset.y()) <= 1,
                "stacking above the toolbar must preserve centering on the capture display");
        modal->reject();
        flush();
        require(toolbar.isVisible() && toolbar.windowHandle()->isVisible() &&
                    hiddenToolbar.isHidden(),
                "cancelling save must preserve visible and hidden toolbar states");
    }
}

void saveDialogDoesNotResurrectPopover() {
    QWidget overlay(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
    overlay.resize(1200, 800);
    overlay.show();
    QWidget toolbar(&overlay, Qt::Tool | Qt::WindowStaysOnTopHint);
    toolbar.resize(300, 80);
    AdButton saveButton(&toolbar);
    saveButton.setText(QStringLiteral("Save as file"));
    toolbar.show();
    AdPopover popover(&saveButton);
    popover.setSourceWidget(&saveButton);
    popover.setText(QStringLiteral("Save as file"));
    popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    popover.setPopupLifetime(AdPopover::PopupLifetime::Retained);
    popover.show();
    flush();
    QPointer<QWidget> popup;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("adpopover-surface") && widget->isVisible())
            popup = widget;
    }
    require(popup && popover.isVisible(), "save button popover fixture must be visible");

    auto* modal = openDialog(overlay, fixture(), {}, {}, &toolbar);
    popover.hide();
    flush();
    modal->reject();
    flush();
    popover.hide();
    flush();
    require(!popover.isVisible() && (!popup || !popup->isVisible()) &&
                (!popup || !popup->windowHandle() || !popup->windowHandle()->isVisible()),
            "closing Save as File must not resurrect a popover that its controller has dismissed");
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
    constexpr auto pinCopyBinding =
        settings::SettingsSwitchBinding::ScreenshotCopyAfterPinningToScreen;
    const QString pinCopyKey = QStringLiteral("screenshot/copy_after_pinning_to_screen");
    const auto* pinCopyField = settings::builtInSettingsRegistry().fieldForSwitch(pinCopyBinding);
    require(pinCopyField && pinCopyField->pageId == QStringLiteral("screenshots") &&
                pinCopyField->sectionId == QStringLiteral("screenshot-settings") &&
                pinCopyField->configurationKey == pinCopyKey &&
                pinCopyField->reset == settings::SettingsSectionReset::ScreenshotSettings,
            "copy after pinning belongs to screenshot interaction settings");
    require(!storage::ConfigurationSchema::defaultValue(pinCopyKey).toBool() &&
                !adapter.copyAfterPinningToScreen() && !backend.switchValue(pinCopyBinding) &&
                backend.switchEnabled(pinCopyBinding),
            "copy after pinning defaults off and is available");
    require(!storage::ConfigurationSchema::normalize(pinCopyKey, QStringLiteral("true")).valid,
            "copy after pinning rejects non-boolean configuration values");
    settings::SettingsRuntimeSession pinCopySession(settings::builtInSettingsRegistry(), backend);
    require(pinCopySession.applySwitchValue(pinCopyBinding, true) &&
                adapter.copyAfterPinningToScreen() && backend.switchValue(pinCopyBinding) &&
                storage::ApplicationStorage::instance().configuration().flushNow().success,
            "copy after pinning round-trips through the runtime settings session");
    {
        storage::ConfigurationStore reloaded(
            QDir(storage::ApplicationStorage::instance().configurationDirectory())
                .filePath(QStringLiteral("config.json")),
            true, true, 60000);
        require(reloaded.value(pinCopyKey).toBool(), "copy after pinning survives a reload");
    }
    require(backend.selectValue(settings::SettingsSelectBinding::ScreenshotSaveAsFileDialog) ==
                QStringLiteral("snow_shot"),
            "settings backend failed to read dialog selection");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotOutput) &&
                adapter.saveAsFileDialog() == QStringLiteral("system"),
            "screenshot output reset must restore the system dialog");
    require(backend.applySelectValue(settings::SettingsSelectBinding::ScreenshotSaveAsFileDialog,
                                     QStringLiteral("snow_shot")),
            "settings backend failed to write dialog selection");
    constexpr auto autoSaveBinding =
        settings::SettingsMultiSelectBinding::ScreenshotAutoSaveAfterExecution;
    const QStringList executionActions{
        QStringLiteral("copy_screenshot"),         QStringLiteral("copy_scrolling_screenshot"),
        QStringLiteral("copy_pinned_image"),       QStringLiteral("capture_focused_window"),
        QStringLiteral("capture_current_monitor"), QStringLiteral("pin_to_screen")};
    for (const QString& action : executionActions) {
        require(backend.applyMultiSelectValue(autoSaveBinding, {action}) &&
                    adapter.autoSaveAfterExecutionActions() == QStringList{action} &&
                    backend.multiSelectValue(autoSaveBinding) == QVariantList{action},
                "each automatic save scenario must round-trip through the settings backend");
    }
    const QStringList autoSaveActions{QStringLiteral("copy_screenshot"),
                                      QStringLiteral("pin_to_screen")};
    require(adapter.setAutoSaveAfterExecutionActions(autoSaveActions) &&
                adapter.setCopyImageFileToClipboard(true) &&
                backend.resetSection(settings::SettingsSectionReset::ScreenshotSettings) &&
                !adapter.copyAfterPinningToScreen() && !backend.switchValue(pinCopyBinding) &&
                adapter.autoSaveAfterExecutionActions() == autoSaveActions &&
                adapter.copyImageFileToClipboard() &&
                adapter.saveAsFileDialog() == QStringLiteral("snow_shot"),
            "function reset must preserve screenshot output settings");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotOutput) &&
                adapter.autoSaveAfterExecutionActions().isEmpty() &&
                !adapter.copyImageFileToClipboard() &&
                adapter.saveAsFileDialog() == QStringLiteral("system"),
            "output reset must restore all moved screenshot settings");

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

void pdfDialogAndSettings(QWidget& owner, const QTemporaryDir& temp) {
    const storage::ScreenshotSettings settings;
    std::optional<ScreenshotExportTaskResult> cacheResult;
    const auto cacheJob = ScreenshotExportCoordinator::shared().submit(
        &owner, ScreenshotExportCoordinator::Priority::Foreground,
        [](const ScreenshotExportCancellation& cancellation) {
            QString error;
            const auto source = pipeline::prepare(snow_shot::image_codec::srgbRowSource(fixture()),
                                                  cancellation, &error);
            auto pixels = pipeline::preparePixels(source, source.rows.size, cancellation, &error);
            ScreenshotSaveExportOptions options{
                .size = source.rows.size, .format = Format::Pdf, .quality = 99};
            const auto first = pipeline::render(pixels, options, cancellation, &error);
            require(first && first->pdf, "PDF pipeline must encode its image payload");
            options.pdfTitle = QStringLiteral("renamed");
            options.pdfPageSize = ScreenshotPdfPageSize::LandscapeA4;
            const auto relaid = pipeline::render(pixels, options, cancellation, &error, first->pdf);
            require(relaid && relaid->pdf == first->pdf &&
                        relaid->options.pdfTitle == options.pdfTitle,
                    "metadata and paper changes must reuse compressed image payloads");
            const auto decoded = pipeline::decode(*relaid, cancellation, &error);
            require(!decoded.isNull() && decoded.size() == source.rows.size,
                    "PDF previews must decode their embedded image, not pass PDF to the raster "
                    "decoder");
            return ScreenshotExportTaskResult{};
        },
        [&cacheResult](ScreenshotExportTaskResult result) { cacheResult = std::move(result); });
    require(cacheJob.isValid(), "PDF cache test job must start");
    processUntil([&] { return cacheResult.has_value(); });
    require(cacheResult->succeeded(), "PDF layout and metadata must reuse the compressed payload");
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const QString key = QStringLiteral("screenshot/pdf_page_size");
    require(storage::ConfigurationSchema::defaultValue(key).toString() ==
                QStringLiteral("a4_portrait"),
            "PDF paper must default to portrait A4");
    for (const QString& value : {QStringLiteral("image_size"), QStringLiteral("a4_portrait"),
                                 QStringLiteral("a4_landscape")})
        require(backend.applySelectValue(settings::SettingsSelectBinding::ScreenshotPdfPageSize,
                                         value) &&
                    settings.pdfPageSize() == value &&
                    backend.selectValue(settings::SettingsSelectBinding::ScreenshotPdfPageSize)
                            .toString() == value,
                "all PDF paper options must round-trip through settings");
    require(!settings.setPdfPageSize(QStringLiteral("invalid")) &&
                settings.pdfPageSize() == QStringLiteral("a4_landscape"),
            "invalid page size must preserve the previous value");
    require(
        backend.applySelectValue(settings::SettingsSelectBinding::ScreenshotCompressionLevel,
                                 QStringLiteral("high")) &&
            backend.selectValue(settings::SettingsSelectBinding::ScreenshotCompressionLevel) ==
                QStringLiteral("high") &&
            backend.applySliderValue(settings::SettingsSliderBinding::ScreenshotImageQuality, 0) &&
            backend.sliderValue(settings::SettingsSliderBinding::ScreenshotImageQuality) == 0,
        "global image encoding settings must round-trip through the settings backend");
    require(storage::ConfigurationSchema::defaultValue(
                QStringLiteral("capture_history/compression_level")) == QStringLiteral("medium") &&
                backend.selectValue(settings::SettingsSelectBinding::HistoryCompressionLevel) ==
                    QStringLiteral("medium") &&
                backend.applySelectValue(settings::SettingsSelectBinding::HistoryCompressionLevel,
                                         QStringLiteral("high")) &&
                backend.selectValue(settings::SettingsSelectBinding::HistoryCompressionLevel) ==
                    QStringLiteral("high") &&
                settings.compressionLevel() == QStringLiteral("high") &&
                !backend.applySelectValue(settings::SettingsSelectBinding::HistoryCompressionLevel,
                                          QStringLiteral("invalid")) &&
                backend.resetSection(settings::SettingsSectionReset::HistoryPolicy) &&
                backend.selectValue(settings::SettingsSelectBinding::HistoryCompressionLevel) ==
                    QStringLiteral("medium") &&
                settings.compressionLevel() == QStringLiteral("high"),
            "history display compression must validate, reset, and remain independent of output");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotOutput) &&
                settings.pdfPageSize() == QStringLiteral("a4_portrait") &&
                settings.compressionLevel() == QStringLiteral("medium") &&
                settings.imageQuality() == 100,
            "output reset must restore PDF, compression, and quality defaults");
    require(settings.setImageSaveDirectory(temp.path()) &&
                settings.setLastManualSaveFormat(QStringLiteral("pdf")) &&
                settings.setPdfPageSize(QStringLiteral("a4_landscape")),
            "PDF dialog settings setup failed");
    require(storage::ApplicationStorage::instance().flushNow().success, "PDF setting flush failed");
    storage::ConfigurationStore reloaded(
        QDir(storage::ApplicationStorage::instance().configurationDirectory())
            .filePath(QStringLiteral("config.json")),
        true, false);
    require(reloaded.value(key).toString() == QStringLiteral("a4_landscape"),
            "PDF paper must survive configuration reload");
    QString savedPath;
    auto* modal = openDialog(owner, fixture(), [&](const QString& path) { savedPath = path; });
    auto* content = modal->contentWidget();
    auto* quality = child<AdSlider>(content, "saveQualitySlider");
    auto* canvas = child<ScreenshotSavePreviewCanvas>(content, "savePreviewCanvas");
    require(child<AdSelect>(content, "saveFormatSelect")->currentValue().toString() ==
                    QStringLiteral("pdf") &&
                quality->isEnabled() && quality->value() == 100 &&
                quality->marks().value(100).label == QStringLiteral("Lossless"),
            "PDF must start at lossless quality with the correct endpoint label");
    require(canvas->pdfLayout().pagePoints.width() > canvas->pdfLayout().pagePoints.height(),
            "PDF preview must show the configured landscape page");
    const auto paper = canvas->pdfLayout();
    require(settings.setPdfPageSize(QStringLiteral("image_size")), "page snapshot fixture failed");
    quality->setValue(1);
    quality->setValue(99);
    processUntil([&] { return child<QLabel>(content, "savePreviewStatus")->isHidden(); });
    require(canvas->outputImage().pixelColor(0, 0).alpha() == 255 &&
                canvas->pdfLayout().pagePoints == paper.pagePoints,
            "PDF lossy preview must be opaque and retain the opening page setting");
    const int decodesBeforeTitleChange = content->property("previewDecodeCount").toInt();
    const qint64 previewIdentity = canvas->outputImage().cacheKey();
    const auto generationBeforeTitleChange = content->property("previewGeneration").toULongLong();
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("pdf-title-only"));
    processUntil([&] {
        return content->property("previewGeneration").toULongLong() > generationBeforeTitleChange;
    });
    require(content->property("previewDecodeCount").toInt() == decodesBeforeTitleChange &&
                canvas->outputImage().cacheKey() == previewIdentity,
            "PDF title edits must retain the decoded preview pixels without another decode");
    child<AdInputNumber>(content, "saveWidthInput")->setValue(320);
    child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("png"));
    child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("pdf"));
    quality->setValue(100);
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("pdf-preview-final"));
    processUntil([&] { return child<QLabel>(content, "savePreviewStatus")->isHidden(); });
    require(canvas->outputImage().size() == QSize(320, 200),
            "stale preview results must not replace the final PDF image");
    snapshot(modal, QStringLiteral("export-pdf-landscape"));
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(savedPath.endsWith(QStringLiteral("pdf-preview-final.pdf")),
            "manual PDF must use .pdf extension");
    QFile saved(savedPath);
    require(saved.open(QIODevice::ReadOnly), "saved PDF must be readable");
    const auto bytes = saved.readAll();
    require(bytes.contains("/MediaBox [0 0 841.88976378 595.27559055]") &&
                !bytes.contains("/DCTDecode"),
            "saved PDF must use the preview paper and final lossless quality");
    flush();
    modal = openDialog(owner, fixture());
    content = modal->contentWidget();
    require(child<AdSlider>(content, "saveQualitySlider")->value() == 100 &&
                child<AdSelect>(content, "saveFormatSelect")->currentValue().toString() ==
                    QStringLiteral("pdf") &&
                child<ScreenshotSavePreviewCanvas>(content, "savePreviewCanvas")
                        ->pdfLayout()
                        .pagePoints == QSizeF(120, 75),
            "new dialogs must remember PDF, reset quality and read the current page setting");
    const QJsonObject optionsBeforeCancel = settings.manualSaveFormatOptions();
    child<AdSlider>(content, "saveQualitySlider")->setValue(20);
    child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("jpeg"));
    modal->reject();
    flush();
    require(settings.lastManualSaveFormat() == QStringLiteral("pdf") &&
                settings.manualSaveFormatOptions() == optionsBeforeCancel,
            "cancel must not replace the remembered format or any per-format options");
    require(settings.setLastManualSaveFormat(QStringLiteral("png")) &&
                settings.setPdfPageSize(QStringLiteral("a4_portrait")),
            "PDF settings cleanup failed");
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
    for (const auto format : {Format::Png, Format::Jpeg, Format::Bmp, Format::Webp, Format::Jxl,
                              Format::Pdf, Format::Avif}) {
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
                state.output.compressionLevel == ScreenshotCompressionLevel::Medium &&
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
    require(
        pipeline::normalizedOptions({.size = QSize(320, 123), .format = Format::Bmp, .quality = 75})
                .quality == 100,
        "BMP quality must normalize out because the encoder has no quality setting");
    require(pipeline::normalizedOptions({.size = QSize(320, 123),
                                         .format = Format::Jpeg,
                                         .quality = 0,
                                         .compressionLevel = ScreenshotCompressionLevel::High})
                        .quality == 0 &&
                pipeline::normalizedOptions({.size = QSize(320, 123),
                                             .format = Format::Jpeg,
                                             .quality = 0,
                                             .compressionLevel = ScreenshotCompressionLevel::High})
                        .compressionLevel == ScreenshotCompressionLevel::Low,
            "normalization must preserve quality zero and remove unsupported compression");
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
                        .size = quality == 100 ? QSize(80, 50) : QSize(96, 32),
                        .format = format,
                        .quality = quality};
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
                    const auto lossless = pipeline::render(
                        source, {.size = original.size(), .format = format, .quality = 100},
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
                const auto output = pipeline::render(
                    large, {.size = QSize(48, 2304), .format = format, .quality = 72}, cancellation,
                    &error);
                require(output != nullptr, "large output encoding failed");
                const QImage preview = pipeline::decode(*output, cancellation, &error);
                require(preview.size() == QSize(48, 2304) &&
                            samePixels(preview,
                                       snow_shot::image_codec::decodeFile(
                                           output->path,
                                           ScreenshotImageFileService::snowImageFormat(format))),
                        "large display images must contain the complete decoded export");
            }
            const auto retained = pipeline::render(
                source, {.size = original.size(), .format = Format::Png, .quality = 100},
                cancellation, &error);
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
            missing.options = {.size = QSize(2, 2), .format = Format::Png, .quality = 100};
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

void canvasNativeDetailPreservesTransparency() {
    const QSize pixels(4096, 3072);
    const auto solidImage = [](QSize size, QColor color) {
        QImage image(size, QImage::Format_RGBA8888);
        image.fill(color);
        return image;
    };
    const auto renderer = [solidImage](QColor color) {
        return [solidImage, color](QRect region, const ScreenshotExportCancellation&, QString*) {
            QSize size = region.size();
            size.scale(1024, 1024, Qt::KeepAspectRatio);
            return solidImage(size, color);
        };
    };
    for (int alpha : {0, 127, 255}) {
        for (bool pdf : {false, true}) {
            ScreenshotSavePreviewCanvas reference;
            ScreenshotSavePreviewCanvas detailed;
            const QColor sourceColor(197, 57, 31, alpha);
            const QColor outputColor(23, 101, 217, alpha);
            const auto source = solidImage(QSize(256, 192), sourceColor);
            const auto output = solidImage(source.size(), outputColor);
            reference.setSource(source, pixels);
            reference.setOutput(output, pixels);
            detailed.setSource(source, pixels, renderer(sourceColor));
            detailed.setOutput(output, pixels, renderer(outputColor));
            for (auto* canvas : {&reference, &detailed}) {
                canvas->resize(600, 400);
                if (pdf)
                    canvas->setPdfLayout(
                        screenshot_pdf::layout(pixels, ScreenshotPdfPageSize::PortraitA4));
                canvas->show();
                for (int iteration = 0; iteration < 4; ++iteration) {
                    QWheelEvent wheel(QPointF(300, 200), canvas->mapToGlobal(QPoint(300, 200)), {},
                                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                                      Qt::NoScrollPhase, false);
                    QApplication::sendEvent(canvas, &wheel);
                }
                child<CanvasStatusReadout>(canvas, "savePreviewZoom")->dismiss();
            }
            processUntil([&] { return !detailed.outputDetailImage().isNull(); });
            const auto verify = [&] {
                for (double split : {0.0, 0.5, 1.0}) {
                    reference.setSplitRatio(split);
                    detailed.setSplitRatio(split);
                    // Native windows may receive different pointer hover events.
                    for (auto* canvas : {&reference, &detailed}) {
                        QEvent leave(QEvent::Leave);
                        QApplication::sendEvent(canvas, &leave);
                    }
                    const QImage expected = reference.grab().toImage();
                    const QImage actual = detailed.grab().toImage();
                    const QString message =
                        QStringLiteral("native detail must composite alpha once over checkerboard "
                                       "or PDF paper (alpha %1, PDF %2, split %3, DPR %4)")
                            .arg(alpha)
                            .arg(pdf)
                            .arg(split)
                            .arg(detailed.devicePixelRatioF());
                    const bool matchingPixels = samePixels(expected, actual);
                    if (!matchingPixels) {
                        QRect differences;
                        QPoint first;
                        int count = 0;
                        for (int y = 0; y < std::min(expected.height(), actual.height()); ++y) {
                            for (int x = 0; x < std::min(expected.width(), actual.width()); ++x) {
                                if (expected.pixel(x, y) == actual.pixel(x, y))
                                    continue;
                                if (count++ == 0)
                                    first = QPoint(x, y);
                                differences |= QRect(x, y, 1, 1);
                            }
                        }
                        std::cerr << "detail pixels: count=" << count
                                  << " bounds=" << differences.x() << ',' << differences.y() << ' '
                                  << differences.width() << 'x' << differences.height()
                                  << " first=" << first.x() << ',' << first.y() << " colors="
                                  << expected.pixelColor(first).name(QColor::HexArgb).toStdString()
                                  << '/'
                                  << actual.pixelColor(first).name(QColor::HexArgb).toStdString()
                                  << " zoom=" << reference.zoom() << '/' << detailed.zoom()
                                  << " window origins=" << reference.pos().x() << ','
                                  << reference.pos().y() << '/' << detailed.pos().x() << ','
                                  << detailed.pos().y() << '\n';
                    }
                    require(matchingPixels, qPrintable(message));
                }
            };
            verify();
            const QRect detailRegion = detailed.outputDetailRegion();
            const auto detailGeneration = detailed.property("detailGeneration");
            const QPointF pan = detailed.pan();
            require(detailRegion != QRect(QPoint{}, pixels),
                    "zoomed transparency fixture must retain a partial native tile");
            for (auto* canvas : {&reference, &detailed}) {
                canvas->setSplitRatio(0.5);
                QMouseEvent down(QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100),
                                 Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent move(QEvent::MouseMove, QPointF(125.5, 120.25), QPointF(125.5, 120.25),
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent up(QEvent::MouseButtonRelease, QPointF(125.5, 120.25),
                               QPointF(125.5, 120.25), Qt::LeftButton, Qt::NoButton,
                               Qt::NoModifier);
                QApplication::sendEvent(canvas, &down);
                QApplication::sendEvent(canvas, &move);
                QApplication::sendEvent(canvas, &up);
            }
            verify();
            require(detailed.pan() == pan + QPointF(25.5, 20.25) &&
                        reference.pan() == detailed.pan() &&
                        detailed.outputDetailRegion() == detailRegion &&
                        detailed.property("detailGeneration") == detailGeneration,
                    "fractional panning must preserve the cached tile until replacement arrives");
        }
    }
}

void canvasPdfComparison() {
    ScreenshotSavePreviewCanvas canvas;
    canvas.resize(600, 400);
    QImage original(QSize(400, 200), QImage::Format_RGBA8888);
    original.fill(Qt::red);
    QImage output(original.size(), original.format());
    output.fill(Qt::green);
    canvas.setSource(original, QSize(1000, 500));
    canvas.setOutput(output);
    canvas.show();
    flush();
    const auto verify = [&] {
        canvas.setPdfLayout({});
        const double zoom = canvas.zoom();
        const QPointF pan = canvas.pan();
        const QImage baseline = canvas.grab().toImage();
        const qreal dpr = baseline.devicePixelRatio();
        for (const auto page :
             {ScreenshotPdfPageSize::PortraitA4, ScreenshotPdfPageSize::LandscapeA4,
              ScreenshotPdfPageSize::ImageSize}) {
            for (const QSize size : {QSize(400, 200), QSize(800, 400), QSize(400, 100)}) {
                canvas.setOutput(output.scaled(size));
                canvas.setPdfLayout(screenshot_pdf::layout(size, page));
                require(canvas.zoom() == zoom && canvas.pan() == pan,
                        "PDF layout changes must preserve comparison zoom and pan");
                const QImage frame = canvas.grab().toImage();
                const int leftWidth = qRound(260 * dpr);
                require(frame.copy(0, 0, leftWidth, frame.height()) ==
                            baseline.copy(0, 0, leftWidth, baseline.height()),
                        "PDF paper must not change any original-pane pixels");
                bool paperAdded = false;
                for (int y = 0; y < frame.height(); ++y) {
                    for (int x = 0; x < frame.width(); ++x) {
                        const QColor before = baseline.pixelColor(x, y);
                        const QColor after = frame.pixelColor(x, y);
                        require((before == QColor(Qt::red)) == (after == QColor(Qt::red)) &&
                                    (before == QColor(Qt::green)) == (after == QColor(Qt::green)),
                                "PDF must preserve both images' displayed position and size");
                        if (x > qRound(340 * dpr) && before != after && after == QColor(Qt::white))
                            paperAdded = true;
                    }
                }
                require(page == ScreenshotPdfPageSize::ImageSize || paperAdded,
                        "PDF must add white paper around the output image");
            }
        }
        canvas.setPdfLayout({});
        require(canvas.zoom() == zoom && canvas.pan() == pan && canvas.grab().toImage() == baseline,
                "leaving PDF must restore the raster preview without changing the viewport");
    };
    verify();
    QWheelEvent wheel(QPointF(450, 150), canvas.mapToGlobal(QPoint(450, 150)), {}, QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &wheel);
    child<QTimer>(&canvas, "savePreviewZoomTimer")->stop();
    child<QLabel>(&canvas, "savePreviewZoom")->hide();
    verify();
    canvas.setPdfLayout(screenshot_pdf::layout(output.size(), ScreenshotPdfPageSize::PortraitA4));
    const double zoom = canvas.zoom();
    const QPointF pan = canvas.pan();
    canvas.resize(800, 600);
    flush();
    require(canvas.zoom() == zoom && canvas.pan() == pan,
            "PDF must preserve manual viewport mode across canvas resizing");
    canvas.fitImage();
    require(qAbs(canvas.zoom() - 0.76) < 0.000001 && canvas.pan().isNull(),
            "explicit PDF fitting must use source pixels, regardless of paper dimensions");
    canvas.resize(600, 400);
    flush();
    require(qAbs(canvas.zoom() - 0.56) < 0.000001,
            "automatic PDF fitting must continue to use source pixels");
}

void canvasWheelZoom() {
    ScreenshotSavePreviewCanvas canvas;
    canvas.resize(600, 400);
    canvas.setSource(fixture(), QSize(160, 100));
    const QPointF position(120, 80);
    const auto scroll = [&](QPoint pixels, QPoint angles,
                            Qt::MouseEventSource source = Qt::MouseEventNotSynthesized,
                            Qt::ScrollPhase phase = Qt::NoScrollPhase) {
        QWheelEvent event(position, position, pixels, angles, Qt::NoButton, Qt::NoModifier, phase,
                          false, source);
        QApplication::sendEvent(&canvas, &event);
        require(event.isAccepted(), "preview wheel zoom must consume the input");
    };
    scroll({}, QPoint(0, 120));
    require(qAbs(canvas.zoom() - 1.15) < 0.000001,
            "a Windows mouse notch must retain its 15 percent zoom factor");
#ifdef Q_OS_MACOS
    for (int pixels : {2, 2, 80}) {
        const double before = canvas.zoom();
        const QPointF imagePoint =
            (position - QRectF(canvas.rect()).center() - canvas.pan()) / before;
        scroll(QPoint(0, pixels), QPoint(0, 120));
        require(qAbs(canvas.zoom() - before * 1.15) < 0.000001,
                "every Cocoa mouse notch must match the Windows zoom factor");
        const QPointF afterPoint =
            (position - QRectF(canvas.rect()).center() - canvas.pan()) / canvas.zoom();
        require(QLineF(imagePoint, afterPoint).length() < 0.000001,
                "mouse wheel zoom must keep the source point under the cursor");
    }
    const double beforeReverse = canvas.zoom();
    scroll(QPoint(0, -2), QPoint(0, -120));
    require(qAbs(canvas.zoom() - beforeReverse / 1.15) < 0.000001,
            "reverse Cocoa mouse scrolling must match Windows");
#endif
    const double beforePrecise = canvas.zoom();
    scroll(QPoint(0, 2), QPoint(0, 4), Qt::MouseEventSynthesizedBySystem);
    require(qAbs(canvas.zoom() - beforePrecise * std::pow(1.15, 0.02)) < 0.000001,
            "precise phase-less Cocoa input must retain continuous pixel scaling");
    const double beforeHorizontal = canvas.zoom();
    scroll(QPoint(20, 0), QPoint(120, 0));
    require(canvas.zoom() == beforeHorizontal, "horizontal mouse scrolling must not zoom");
}

void canvasZoomHint() {
    ScreenshotSavePreviewCanvas canvas;
    canvas.resize(600, 400);
    canvas.setSource(fixture(), QSize(160, 100));
    canvas.show();
    flush();
    auto* hint = child<CanvasStatusReadout>(&canvas, "savePreviewZoom");
    auto* busy = child<CanvasStatusReadout>(&canvas, "savePreviewStatus");
    require(child<QLabel>(&canvas, "savePreviewZoom") == hint &&
                child<QLabel>(&canvas, "savePreviewStatus") == busy &&
                hint->message() == CanvasStatusReadout::Message::Scale &&
                busy->message() == CanvasStatusReadout::Message::RenderingPreview,
            "export zoom and rendering hints must use the shared QLabel-compatible component");
    require(hint->isHidden(), "initial preview fitting must not show the zoom hint");
    auto* timer = child<QTimer>(&canvas, "savePreviewZoomTimer");
    require(timer == hint->dismissalTimer() && timer->isSingleShot() && timer->interval() == 1000 &&
                !timer->isActive(),
            "zoom hint must use the pinned-window one-second dismissal interval");
    canvas.setBusy(true);
    require(busy->isVisible() && busy->pos() == QPoint(8, 8) &&
                !busy->dismissalTimer()->isActive() &&
                busy->toolTip() == QStringLiteral("Rendering preview") &&
                busy->accessibleName() == busy->toolTip(),
            "rendering status must persist at the shared top-left inset with accessible copy");
    const auto expire = [&] {
        timer->stop();
        require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
                "zoom hint timeout could not be delivered");
        require(hint->isHidden(), "zoom hint must disappear when its timer expires");
    };
    const auto pressKey = [&](int key) {
        PhysicalKeyEvent hardware(QEvent::KeyPress, key, Qt::NoModifier);
#ifdef Q_OS_MACOS
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, 1, hardware.nativeVirtualKey(),
                        0);
#else
        auto& event = hardware;
#endif
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
    require(hint->isHidden() && !timer->isActive() && busy->isVisible() &&
                busy->pos() == QPoint(8, 8) && !busy->dismissalTimer()->isActive(),
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
    canvas.setSource(fixture(), QSize(12000, 8000));
    require(qAbs(canvas.zoom() - 0.05) < 0.000001 &&
                hint->text() == QStringLiteral("Scale: 5.0%") && hint->isHidden(),
            "export zoom below ten percent must retain one decimal without showing a hint");
    pressKey(Qt::Key_Plus);
    require(hint->text() == QStringLiteral("Scale: %1%").arg(canvas.zoom() * 100, 0, 'f', 1) &&
                hint->accessibleName() == hint->toolTip(),
            "transient export zoom must retain decimal precision and complete accessible text");
    expire();
    class HintTranslator final : public QTranslator {
      public:
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            if (QByteArray(context) == "CanvasStatusReadout")
                return QStringLiteral("Translated ") + QString::fromUtf8(source);
            return {};
        }
    } translator;
    require(QApplication::installTranslator(&translator), "shared hint translator unavailable");
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(hint, &languageChange);
    QApplication::sendEvent(busy, &languageChange);
    require(hint->isHidden() && !timer->isActive() &&
                hint->text().startsWith(QStringLiteral("Translated Scale: ")) &&
                busy->isVisible() &&
                busy->toolTip() == QStringLiteral("Translated Rendering preview"),
            "shared language updates must preserve hidden zoom and persistent rendering status");
    QApplication::removeTranslator(&translator);
    canvas.setBusy(false);
    require(busy->isHidden() && !busy->dismissalTimer()->isActive(),
            "rendering completion must dismiss its status synchronously");
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
    auto* compression = child<AdSelect>(content, "saveCompressionLevelSelect");
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
    const qulonglong preparedIdentity = content->property("preparedPixelsIdentity").toULongLong();
    compression->setCurrentValue(QStringLiteral("high"));
    require(!busy->isHidden(), "PNG compression changes must render a new encoded result");
    processUntil([&] { return content->property("previewGeneration").toULongLong() > generation; });
    require(content->property("preparedPixelsIdentity").toULongLong() == preparedIdentity,
            "compression changes must reuse the already prepared pixels");
    const quint64 compressionGeneration = content->property("previewGeneration").toULongLong();
    width->setValue(80);
    lock->click();
    lock->click();
    processUntil([&] {
        return content->property("previewGeneration").toULongLong() > compressionGeneration;
    });
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
    require(!trigger->isHidden() &&
                adqt::icons::describeIcon(trigger->iconRef()).key.name == QStringLiteral("more"),
            "custom save path menu button must stay visible and use the More icon");
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
    require(menu.isNull(),
            "leaving the trigger without entering its menu must retire the popup session");
    require(!trigger->isHidden(), "leaving a custom save path must keep its menu button visible");
    require(popupWidth < 160, "two-action shortcut popup must size to its content");
    flush();

    enter(trigger);
    flush();
    menu = child<AdContextMenu>(content, "savePathMenu");
    leave(trigger);
    enter(menu);
    settle();
    require(menu && menu->isVisible(),
            "moving from the shortcut arrow into its menu must keep its session open");
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
    require(menu && menu->isVisible(),
            "returning to the shortcut arrow must cancel pending dismissal");

    // A native popup can grab mouse moves outside its bounds without another Leave event.
    QCursor::setPos(outside);
    QMouseEvent move(QEvent::MouseMove, menu->mapFromGlobal(outside), outside, Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(menu, &move);
    settle();
    require(menu.isNull(),
            "grabbed mouse movement outside the hover region must retire the popup session");
    flush();

    enter(trigger);
    flush();
    menu = child<AdContextMenu>(content, "savePathMenu");
    enter(menu);
    QCursor::setPos(outside);
    leave(menu);
    settle();
    require(menu.isNull(), "leaving the popup must retire its session without a click");

    flush();
    trigger->click();
    flush();
    menu = child<AdContextMenu>(content, "savePathMenu");
    require(menu && menu->isVisible(), "covered-trigger setup must reopen the shortcut menu");
    QWidget blocker(content);
    blocker.setGeometry(QRect(trigger->mapTo(content, QPoint()), trigger->size()));
    blocker.show();
    blocker.raise();
    require(menu && menu->isVisible(), "covering the trigger must leave the menu open initially");
    const QPoint coveredTrigger = trigger->mapToGlobal(trigger->rect().center());
    QCursor::setPos(coveredTrigger);
    require(QApplication::widgetAt(coveredTrigger) == &blocker,
            "hover occlusion fixture must cover the shortcut trigger");
    QMouseEvent coveredMove(QEvent::MouseMove, menu->mapFromGlobal(coveredTrigger), coveredTrigger,
                            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(menu, &coveredMove);
    settle();
    require(menu.isNull(),
            "a covered trigger must retire its hover popup session after pointer movement");
    const QPoint triggerCenter = trigger->rect().center();
    QEnterEvent coveredEnter(triggerCenter, triggerCenter, coveredTrigger);
    QApplication::sendEvent(trigger, &coveredEnter);
    const auto menus = content->findChildren<AdContextMenu*>(QStringLiteral("savePathMenu"));
    require(
        std::none_of(menus.begin(), menus.end(),
                     [](const AdContextMenu* candidate) { return candidate->isPopupVisible(); }),
        "entering a covered trigger must not reopen its hover popup");
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

void timeDirectoryDefaultsAndShortcut(QWidget& owner, const QTemporaryDir& temp) {
    storage::ScreenshotSettings settings;
    const QString pattern = temp.filePath(QStringLiteral("images/{YYYY}"));
    require(settings.setImageSaveDirectory(pattern) && settings.setLastManualSaveDirectory({}) &&
                settings.setManualSaveFilenameFormat(QStringLiteral("{YYYY}")),
            "dated save dialog settings must persist");
    const QDateTime before =
        QDateTime::fromString(QStringLiteral("2026-12-31T23:59:59"), Qt::ISODate);
    for (const auto& timestamp : {before, before.addSecs(1)}) {
        const auto state = ScreenshotSaveDialogState::initial(QSize(160, 100), timestamp);
        require(state.directory == temp.filePath(QStringLiteral("images/") +
                                                 timestamp.toString(QStringLiteral("yyyy"))) &&
                    state.filename == timestamp.toString(QStringLiteral("yyyy")),
                "custom manual dialog must use one time for its configured folder and filename");
    }
    auto* modal = openDialog(owner, fixture());
    auto* content = modal->contentWidget();
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdButton>(content, "savePathShortcut_-2")->click();
    require(child<AdLineEdit>(content, "saveDirectoryInput")->text() ==
                temp.filePath(QStringLiteral("images/") +
                              child<AdLineEdit>(content, "saveFilenameInput")->text()),
            "App directory shortcut must expand using the same time as the dialog filename");
    require(settings.imageSaveDirectory() == pattern, "dialog must retain the configured template");
    modal->reject();
    flush();
}

void windowNameDefaultsAndShortcut(QWidget& owner, const QTemporaryDir& temp) {
    storage::ScreenshotSettings settings;
    const QString pattern = temp.filePath(QStringLiteral("window-names/{AN}"));
    require(settings.setImageSaveDirectory(pattern) && settings.setLastManualSaveDirectory({}) &&
                settings.setManualSaveFilenameFormat(QStringLiteral("{WN}_{YYYY}")),
            "window dialog templates must persist");
    const storage::ScreenshotWindowNames names{QStringLiteral("Browser"),
                                               QStringLiteral("Title {AN}/文档")};
    const QDateTime timestamp =
        QDateTime::fromString(QStringLiteral("2026-10-16T12:34:56"), Qt::ISODate);
    const auto state = ScreenshotSaveDialogState::initial(QSize(160, 100), timestamp, names);
    require(state.directory == temp.filePath(QStringLiteral("window-names/Browser")) &&
                state.filename == QStringLiteral("Title {AN}_文档_2026"),
            "custom dialog defaults must expand both variables once");
    auto artifact =
        std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(fixture()));
    artifact->setWindowNames(names);
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact),
            "window-name dialog must open");
    auto* modal = owner.findChild<AdModal*>(QStringLiteral("screenshotSaveAsFileModal"));
    require(modal, "window-name dialog must be present");
    auto* content = modal->contentWidget();
    processUntil([&] { return content->property("previewGeneration").toULongLong() > 0; });
    flush();
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdButton>(content, "savePathShortcut_-2")->click();
    require(child<AdLineEdit>(content, "saveDirectoryInput")->text() == state.directory &&
                child<AdLineEdit>(content, "saveFilenameInput")
                    ->text()
                    .startsWith(QStringLiteral("Title {AN}_文档_")),
            "App directory and filename must use the artifact's frozen metadata");
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("literal-{WN}"));
    require(child<AdLineEdit>(content, "saveFilenameInput")->text() ==
                QStringLiteral("literal-{WN}"),
            "edited concrete filenames must remain literal");
    modal->reject();
    flush();
    const auto later = ScreenshotSaveDialogState::initial(QSize(160, 100), timestamp);
    require(later.directory.endsWith(QStringLiteral("/Unknown")) &&
                later.filename == QStringLiteral("Unknown_2026"),
            "custom exports without live capture context must use Unknown");
}

void dottedWindowNameDefaultsAndSaves(QWidget& owner, const QTemporaryDir& temp) {
    storage::ScreenshotSettings settings;
    require(settings.setImageSaveDirectory(temp.path()) &&
                settings.setLastManualSaveDirectory({}) &&
                settings.setLastManualSaveFormat(QStringLiteral("png")) &&
                settings.setManualSaveFilenameFormat(QStringLiteral("{WN}_{YYYY}")),
            "dotted window-name settings must persist");
    const QDateTime timestamp =
        QDateTime::fromString(QStringLiteral("2026-10-16T12:34:56"), Qt::ISODate);
    for (const QString& title :
         {QStringLiteral("README.md - Visual Studio Code"),
          QStringLiteral("Report.v2.pdf - Browser"), QStringLiteral("image.png")}) {
        auto state = ScreenshotSaveDialogState::initial(QSize(160, 100), timestamp, {{}, title});
        const QString stem = title + QStringLiteral("_2026");
        require(state.filename == stem && state.validationError().isEmpty(),
                "dotted titles must remain valid filename stems");
        for (const auto format : {Format::Png, Format::Jpeg, Format::Webp, Format::Pdf}) {
            state.output.format = format;
            require(QFileInfo(state.outputPath()).fileName() ==
                        stem + QLatin1Char('.') + ScreenshotImageFileService::extension(format),
                    "selecting an output format must preserve every dot and the template suffix");
        }
        state.output.format = Format::Png;
        const auto saved =
            ScreenshotImageFileService::write(fixture(), state.outputPath(), state.output.format);
        require(saved.succeeded() &&
                    QFileInfo(saved.path).fileName() == stem + QStringLiteral(".png"),
                "the written filename must match the full dotted stem displayed by the dialog");
        state.filename = QStringLiteral("edited.v2.name");
        require(QFileInfo(state.outputPath()).fileName() == QStringLiteral("edited.v2.name.png"),
                "edited filenames must follow the same stem contract as generated defaults");
    }
    require(settings.setManualSaveFilenameFormat(QStringLiteral("{WN}")),
            "set a filename template whose title ends in an image extension");
    auto artifact =
        std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(fixture()));
    artifact->setWindowNames({{}, QStringLiteral("image.png")});
    QString savedPath;
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact,
                                             [&](const QString& path) { savedPath = path; }),
            "open the dialog with a literal image-extension title");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    processUntil([&] { return modal->acceptButton()->isEnabled(); });
    require(child<AdLineEdit>(modal->contentWidget(), "saveFilenameInput")->text() ==
                QStringLiteral("image.png"),
            "generated image-extension titles must be displayed unchanged");
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(QFileInfo(savedPath).fileName() == QStringLiteral("image.png.png"),
            "saving must not reinterpret a generated filename stem as a user-entered extension");
    flush();
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
    require(!shortcutEdit->isHidden() &&
                adqt::icons::describeIcon(shortcutEdit->iconRef()).key.name ==
                    QStringLiteral("more"),
            "custom save path menu button must stay visible and use More");
    require(shortcutButton->parentWidget() == shortcutGroup &&
                shortcutEdit->parentWidget() == shortcutGroup &&
                shortcutButton->geometry().right() == shortcutEdit->geometry().left(),
            "save path and edit buttons must share a joined border without a gap");
    shortcutEdit->click();
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
    auto* reopenedExpand = child<AdButton>(content, "savePathExpand_0");
    require(!reopenedExpand->isHidden(), "reopened custom save path menu button must stay visible");
    reopenedExpand->click();
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
    auto* compression = child<AdSelect>(content, "saveCompressionLevelSelect");
    auto* form = content->findChild<AdForm*>();
    require(form != nullptr, "save form is missing");
    auto* qualityRow = form->itemForName(QStringLiteral("quality"));
    auto* compressionRow = form->itemForName(QStringLiteral("compression-level"));
    require(qualityRow != nullptr && compressionRow != nullptr,
            "image encoding option rows are missing");
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
                unit->optionLabel(0) == QStringLiteral("pixel") &&
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
    require(quality->minimum() == 0 && quality->maximum() == 100 &&
                quality->marks().value(quality->minimum()).label == QStringLiteral("0%") &&
                quality->marks().value(quality->maximum()).label == QStringLiteral("100%"),
            "quality must preserve the complete 0-100 range and endpoint labels");
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
    require(qualityRow->isHidden() && !compressionRow->isHidden() &&
                compression->currentValue() == QStringLiteral("medium"),
            "PNG must hide Quality and show Compression level at Medium");
    format->setCurrentValue(QStringLiteral("bmp"));
    require(format->currentValue() == QStringLiteral("bmp") && qualityRow->isHidden() &&
                compressionRow->isHidden(),
            "BMP must hide both image encoding option rows");
    for (const auto& key :
         {QStringLiteral("webp"), QStringLiteral("avif"), QStringLiteral("jxl")}) {
        format->setCurrentValue(key);
        quality->setValue(100);
        require(!qualityRow->isHidden() && !compressionRow->isHidden() &&
                    quality->marks().value(quality->maximum()).label == QStringLiteral("Lossless"),
                "lossless formats must show both controls and label quality 100 as Lossless");
        if (key == QStringLiteral("webp"))
            snapshot(modal, QStringLiteral("export-lossless"));
        quality->setValue(99);
    }
    format->setCurrentValue(QStringLiteral("jpeg"));
    quality->setValue(61);
    require(!qualityRow->isHidden() && compressionRow->isHidden() &&
                quality->marks().value(quality->maximum()).label == QStringLiteral("100%"),
            "JPEG must show Quality, hide Compression level, and restore the 100% endpoint");
    format->setCurrentValue(QStringLiteral("webp"));
    quality->setValue(73);
    compression->setCurrentValue(QStringLiteral("high"));
    format->setCurrentValue(QStringLiteral("jpeg"));
    require(quality->value() == 61, "JPEG must restore its independent quality value");
    format->setCurrentValue(QStringLiteral("webp"));
    require(quality->value() == 73 && compression->currentValue() == QStringLiteral("high"),
            "WebP must restore its independent quality and compression values");
    format->setCurrentValue(QStringLiteral("png"));
    compression->setCurrentValue(QStringLiteral("low"));
    const quint64 generation = content->property("previewGeneration").toULongLong();
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QString());
    child<AdInputNumber>(content, "saveWidthInput")->setValue(96);
    child<AdInputNumber>(content, "saveWidthInput")->setValue(80);
    processUntil([&] { return content->property("previewGeneration").toULongLong() > generation; });
    require(child<AdInputNumber>(content, "saveHeightInput")->value() == 50 &&
                !modal->acceptButton()->isEnabled(),
            "latest geometry must render despite invalid filename and Save must remain blocked");
    compression->setCurrentValue(QStringLiteral("medium"));
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
    const QJsonObject rememberedOptions = storage::ScreenshotSettings().manualSaveFormatOptions();
    require(rememberedOptions.value(QStringLiteral("png"))
                        .toObject()
                        .value(QStringLiteral("compression_level")) == QStringLiteral("medium") &&
                rememberedOptions.value(QStringLiteral("jpeg"))
                        .toObject()
                        .value(QStringLiteral("quality")) == 61 &&
                rememberedOptions.value(QStringLiteral("webp"))
                        .toObject()
                        .value(QStringLiteral("quality")) == 73 &&
                rememberedOptions.value(QStringLiteral("webp"))
                        .toObject()
                        .value(QStringLiteral("compression_level")) == QStringLiteral("high"),
            "the first confirmed save attempt must persist independent per-format controls");
    modal = openDialog(owner, fixture());
    content = modal->contentWidget();
    require(child<AdSelect>(content, "saveFormatSelect")->currentValue() == QStringLiteral("png") &&
                child<AdSelect>(content, "saveCompressionLevelSelect")->currentValue() ==
                    QStringLiteral("medium"),
            "reopening the dialog must restore the selected format's compression value");
    modal->reject();
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
        if (QByteArray(context).startsWith("ScreenshotSave") ||
            QByteArray(context) == "CanvasStatusReadout")
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
    require(width->value() == 160 && height->value() == 100 && width->decimals() == 0 &&
                height->decimals() == 0,
            "pixel mode must use physical source pixels even for high-DPI images");
    percentage();
    require(width->value() == 100 && height->value() == 100 && width->decimals() == 0 &&
                height->decimals() == 0 && width->singleStep() == 1 &&
                width->suffixText() == QStringLiteral("%") &&
                height->suffixText() == QStringLiteral("%") && generation() == initialGeneration,
            "percentage mode must show integers without calculating again");
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
    require(width->value() == 13 && height->value() == 13,
            "percentage values must round to integers before dimension conversion");
    requirePixels(QSize(21, 13));
    width->stepUp();
    require(width->value() == 14 && height->value() == 14,
            "locked percentage steps must increment both integer inputs by one");
    requirePixels(QSize(22, 14));
    height->setValue(150);
    requirePixels(QSize(240, 150));
    lock->click();
    width->setValue(50);
    height->setValue(74.5);
    require(height->value() == 75,
            "unlocked height percentages must also round fractional values to integers");
    requirePixels(QSize(80, 75));
    lock->click();
    requirePixels(QSize(80, 50));
    width->setValue(1);
    requirePixels(QSize(2, 1));
    width->setValue(0.01);
    require(width->value() == 0 && height->value() == 1 && !modal->acceptButton()->isEnabled(),
            "a fractional percentage rounded to zero must prevent saving");
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
    for (int i = 0; i < 5; ++i) {
        requirePixels(QSize(79, 49));
        require(
            width->value() == 49 && height->value() == 49 && width->decimals() == 0 &&
                height->decimals() == 0,
            "rounded percentage displays must preserve exact output pixels across unit changes");
    }
    pixels();
    require(width->decimals() == 0 && width->suffixText().isEmpty(),
            "returning to pixels must restore integer inputs without percentage suffixes");

    auto* editor = width->findChild<QLineEdit*>();
    require(editor != nullptr, "size editor is missing");
    const auto typeWidth = [&](const QString& text) {
        editor->setFocus();
        editor->selectAll();
        for (const auto character : text) {
            PhysicalKeyEvent key(QEvent::KeyPress, character.unicode(), Qt::NoModifier,
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
                unit->optionLabel(0) == QStringLiteral("Translated pixel") &&
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
            require(unit->optionLabel(0) == QStringLiteral("像素") &&
                        label->geometry().right() < unit->geometry().left() &&
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
    require(snow_shot::image_codec::inspectFile(savedPath, snow::image::Format::png, QSize(61, 38)),
            "saving must round pending percentage text to an integer before writing pixels");
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

void duplicateFilenameWarning(QWidget& owner, const QTemporaryDir& temp) {
    QFile existing(temp.filePath(QStringLiteral("duplicate.png")));
    require(existing.open(QIODevice::WriteOnly) && existing.write("existing") == 8,
            "duplicate filename fixture unavailable");
    existing.close();
    auto* modal = openDialog(owner, fixture());
    auto* content = modal->contentWidget();
    auto* directory = child<AdLineEdit>(content, "saveDirectoryInput");
    auto* filename = child<AdLineEdit>(content, "saveFilenameInput");
    auto* format = child<AdSelect>(content, "saveFormatSelect");
    auto* item = content->findChild<AdForm*>()->itemForName(QStringLiteral("filename"));
    auto* message = child<QLabel>(item, "ad-form-item-explain");
    directory->setText(temp.path());
    format->setCurrentValue(QStringLiteral("png"));
    filename->setText(QStringLiteral("duplicate.jpg"));
    flush();
    require(item->warningMessages() ==
                    QStringList{QStringLiteral("A file with this name already exists")} &&
                item->validateStatus() == AdFormItem::ValidateStatus::Warning &&
                message->isVisible() &&
                message->palette().color(QPalette::WindowText) ==
                    adqt::theme::ThemeManager::instance().resolveTheme(item).colorWarning,
            "normalized duplicate filename must show an inline warning in the theme warning color");
    require(item->errorMessages().isEmpty() && modal->acceptButton()->isEnabled() &&
                !content->findChild<AdModal*>(QStringLiteral("saveOverwriteModal")),
            "duplicate filename warning must not block Save or open a confirmation while typing");
    snapshot(modal, QStringLiteral("duplicate-filename-warning"));
    ExportTestTranslator translator;
    require(QApplication::installTranslator(&translator), "test translator unavailable");
    flush();
    require(message->text() == QStringLiteral("Translated A file with this name already exists"),
            "duplicate filename warning must retranslate while visible");
    QApplication::removeTranslator(&translator);
    flush();
    filename->setText(QStringLiteral("unique"));
    require(item->warningMessages().isEmpty() &&
                (item->validateStatus() == AdFormItem::ValidateStatus::None ||
                 item->validateStatus() == AdFormItem::ValidateStatus::Success) &&
                message->isHidden(),
            "renaming to a unique filename must clear the warning and warning style");
    filename->setText(QStringLiteral(" duplicate "));
    require(!item->warningMessages().isEmpty(),
            "trimmed extensionless filename must detect the existing output file");
    format->setCurrentValue(QStringLiteral("jpeg"));
    require(item->warningMessages().isEmpty(), "format changes must refresh duplicate detection");
    format->setCurrentValue(QStringLiteral("png"));
    require(!item->warningMessages().isEmpty(), "returning to a duplicate format must warn again");
    directory->setText(temp.filePath(QStringLiteral("other-directory")));
    require(item->warningMessages().isEmpty(), "directory changes must clear stale warnings");
    directory->setText(temp.path());
    require(!item->warningMessages().isEmpty(), "returning to the original directory must warn");
    filename->setText(QStringLiteral("invalid/name"));
    require(item->warningMessages().isEmpty() && !item->errorMessages().isEmpty() &&
                item->validateStatus() == AdFormItem::ValidateStatus::Error &&
                !modal->acceptButton()->isEnabled(),
            "invalid filenames must replace the warning with a blocking validation error");
    filename->setText(QStringLiteral("duplicate"));
    directory->setText(QStringLiteral("relative-directory"));
    require(item->warningMessages().isEmpty() && !modal->acceptButton()->isEnabled(),
            "invalid directories must suppress duplicate warnings and block Save");
    modal->rejectButton()->click();
    flush();
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
    const QJsonObject optionsBeforeConfirmation =
        storage::ScreenshotSettings().manualSaveFormatOptions();
    child<AdSelect>(content, "saveCompressionLevelSelect")->setCurrentValue(QStringLiteral("high"));
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("existing.jpg"));
    modal->acceptButton()->click();
    auto* confirmation = child<AdModal>(content, "saveOverwriteModal");
    require(confirmation->isOpen(), "normalized destination must trigger overwrite confirmation");
    confirmation->rejectButton()->click();
    flush();
    require(modal->isOpen() && saved.isEmpty() &&
                storage::ScreenshotSettings().manualSaveFormatOptions() ==
                    optionsBeforeConfirmation &&
                existing.open(QIODevice::ReadOnly),
            "canceling overwrite must retain the form without persisting format options");
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
    auto rows = snow_shot::image_codec::srgbRowSource(fixture(QSize(64, 3000)));
    rows.backingImage = {};
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
    require(storage::ScreenshotSettings().setLastManualSaveState(QStringLiteral("png"), {}),
            "optimized pipeline format-option reset failed");
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
    previous = content->property("previewGeneration").toULongLong();
    quality->setValue(72);
    processUntil([&] { return content->property("previewGeneration").toULongLong() > previous; });
    selectFormat(QStringLiteral("jxl"));
    previous = content->property("previewGeneration").toULongLong();
    quality->setValue(72);
    processUntil([&] { return content->property("previewGeneration").toULongLong() > previous; });
    require(content->property("previewDecodeCount").toInt() == 5 &&
                content->property("preparedPixelsIdentity").toULongLong() == preparedIdentity,
            "lossy same-size exports did not reuse prepared pixels and decode codec artifacts");
}

void unbackedExactPreviewRetainsPixels(QWidget& owner) {
    const QImage image = fixture();
    ScreenshotImageRowSource rows = snow_shot::image_codec::srgbRowSource(image);
    rows.backingImage = {};
    auto sourceReads = std::make_shared<std::atomic_int>(0);
    rows.readRows = [readRows = rows.readRows, sourceReads](int first, int count, qsizetype stride,
                                                            uchar* target, qsizetype capacity) {
        ++*sourceReads;
        return readRows(first, count, stride, target, capacity);
    };
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
    require(content->property("previewDecodeCount").toInt() == 0 &&
                samePixels(
                    child<ScreenshotSavePreviewCanvas>(content, "savePreviewCanvas")->outputImage(),
                    image),
            "small source-sized exact outputs must reuse the complete source preview pixels");
    const int readsBeforeFormatChange = sourceReads->load();
    const auto generationBeforeFormatChange = content->property("previewGeneration").toULongLong();
    child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("webp"));
    child<AdSlider>(content, "saveQualitySlider")->setValue(100);
    processUntil([&] {
        return content->property("previewGeneration").toULongLong() > generationBeforeFormatChange;
    });
    require(
        sourceReads->load() == readsBeforeFormatChange &&
            content->property("previewDecodeCount").toInt() == 0 &&
            samePixels(
                child<ScreenshotSavePreviewCanvas>(content, "savePreviewCanvas")->outputImage(),
                image),
        "format changes must encode retained exact preview pixels without rereading the source");
    modal->reject();
    flush();
}

void boundedPreviewAndNativeRegions() {
    const QImage resizeFixture = fixture(QSize(512, 128));
    const QSize largeSize(16385, 1024);
    QImage resizeReference(largeSize, QImage::Format_RGBA8888);
    QString referenceError;
    // This independent full-image oracle is CPU work, not an asynchronous dialog
    // transition. Prepare it before starting the queued-completion watchdog.
    require(snow_shot::image_codec::resizeToRgba8(
                snow_shot::image_codec::srgbRowSource(resizeFixture), largeSize,
                resizeReference.bits(), resizeReference.bytesPerLine(),
                resizeReference.sizeInBytes(), &referenceError),
            "large resize reference could not be prepared");
    resizeReference.setColorSpace(QColorSpace::SRgb);
    QObject receiver;
    QSemaphore workRetired;
    bool finished = false;
    QString failure;
    const auto job = ScreenshotExportCoordinator::shared().submit(
        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [resizeFixture, largeSize, reference = std::move(resizeReference),
         &workRetired](const ScreenshotExportCancellation& cancellation) {
            const auto retireWork = qScopeGuard([&] { workRetired.release(); });
            QImage image = fixture(QSize(64, 6000));
            auto rows = snow_shot::image_codec::srgbRowSource(image);
            rows.backingImage = {};
            rows.isKnownOpaque = true;
            rows.previewImage =
                image.scaled(QSize(22, 2048), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            auto reads = std::make_shared<std::atomic_int>(0);
            rows.readRows = [read = rows.readRows, reads](int first, int count, qsizetype stride,
                                                          uchar* target, qsizetype capacity) {
                ++*reads;
                return read(first, count, stride, target, capacity);
            };
            QString error;
            const auto source = pipeline::prepare(rows, cancellation, &error);
            require(source.rows.isValid() && reads->load() == 0 &&
                        source.preview.constBits() == rows.previewImage.constBits(),
                    "verified opaque previews must bypass full-source reads");
            auto png =
                pipeline::render(source, {image.size(), Format::Png, 100}, cancellation, &error);
            require(png != nullptr && png->pixels->exactImage.isNull(),
                    "large row-only PNG must retain its streaming pixels");
            const int encodedReads = reads->load();
            const QImage exact = pipeline::preview(*png, source, cancellation, &error);
            require(exact.constBits() == source.preview.constBits() &&
                        reads->load() == encodedReads,
                    "exact row-only output must share its bounded source preview");
            const QRect region(11, 3100, 29, 91);
            require(samePixels(pipeline::decodeRegion(*png, region, cancellation, &error),
                               image.copy(region)),
                    "exact viewport rows changed native pixels");
            auto regional = rows;
            regional.readRows = [](int, int, qsizetype, uchar*, qsizetype) { return false; };
            regional.readRegion = [image](QRect rect, qsizetype stride, uchar* target,
                                          qsizetype capacity) {
                const qsizetype rowBytes = qsizetype(rect.width()) * 4;
                if (!QRect(QPoint{}, image.size()).contains(rect) || !target || stride < rowBytes ||
                    capacity < rowBytes || rect.height() - 1 > (capacity - rowBytes) / stride)
                    return false;
                for (int row = 0; row < rect.height(); ++row)
                    std::memcpy(target + qsizetype(row) * stride,
                                image.constScanLine(rect.y() + row) + qsizetype(rect.x()) * 4,
                                size_t(rowBytes));
                return true;
            };
            require(samePixels(pipeline::readRegion(regional, region, cancellation, &error),
                               image.copy(region)),
                    "native region readers must bypass full-width row callbacks");
            const QImage wide = fixture(QSize(3072, 1040));
            const auto wideRows = snow_shot::image_codec::srgbRowSource(wide);
            const QRect wideRegion(0, 8, 3072, 1024);
            const QImage wideDetail =
                pipeline::readRegion(wideRows, wideRegion, cancellation, &error);
            require(wideDetail.size() == wideRegion.size() &&
                        samePixels(wideDetail, wide.copy(wideRegion)),
                    "wide native viewport regions fitting 16MiB must preserve every pixel");
            const auto wideSource = pipeline::prepare(wideRows, cancellation, &error);
            const auto wideJpeg =
                pipeline::render(wideSource, {wide.size(), Format::Jpeg, 72}, cancellation, &error);
            require(wideJpeg != nullptr, "wide JPEG viewport fixture encoding failed");
            const QImage wideDecoded =
                pipeline::decodeRegion(*wideJpeg, wideRegion, cancellation, &error);
            const QImage wideComplete = pipeline::decode(*wideJpeg, cancellation, &error);
            require(wideDecoded.size() == wideRegion.size() &&
                        samePixels(wideDecoded, wideComplete.copy(wideRegion)),
                    "wide decoded viewport regions fitting 16MiB must retain native fidelity");
            auto scalingAdmissions = std::make_shared<QList<QPair<qint64, qint64>>>();
            auto scalingReads = std::make_shared<std::atomic_int>(0);
            auto scalingRows = wideRows;
            scalingRows.readRows = [read = scalingRows.readRows,
                                    scalingReads](int first, int count, qsizetype stride,
                                                  uchar* target, qsizetype capacity) {
                ++*scalingReads;
                return read(first, count, stride, target, capacity);
            };
            scalingRows.reserveResources = [scalingAdmissions](qint64 retained, qint64 scratch,
                                                               QString*) {
                scalingAdmissions->append({retained, scratch});
                return std::static_pointer_cast<void>(std::make_shared<int>(1));
            };
            const auto scalingSource = pipeline::prepare(scalingRows, cancellation, &error);
            require(!scalingSource.preview.isNull() && scalingAdmissions->size() == 2 &&
                        scalingAdmissions->back().second >=
                            wide.sizeInBytes() + scalingSource.preview.sizeInBytes(),
                    "backing preview scaling must admit Qt's complete premultiplied source copy");
            const QSize scalingSize(4097, 1024);
            auto scalingPixels =
                pipeline::preparePixels(scalingSource, scalingSize, cancellation, &error);
            require(scalingPixels && !scalingPixels->exactImage.isNull() &&
                        scalingPixels->exactImage.sizeInBytes() > qint64{16} * 1024 * 1024 &&
                        scalingPixels->exactImage.sizeInBytes() <=
                            pipeline::kMaximumPreparedPixelCacheBytes &&
                        scalingReads->load() > 0,
                    "resized outputs above the preview cap must retain a separately bounded cache");
            const int preparedReads = scalingReads->load();
            const auto scalingEncoded = pipeline::render(
                scalingPixels, {scalingSize, Format::Png, 100}, cancellation, &error);
            const auto scalingJpeg = pipeline::render(
                scalingPixels, {scalingSize, Format::Jpeg, 85}, cancellation, &error);
            require(scalingEncoded && scalingJpeg && scalingReads->load() == preparedReads,
                    "PNG and JPEG must reuse cached resized pixels without recalculating rows");
            const QImage scaledPreview =
                pipeline::preview(*scalingEncoded, scalingSource, cancellation, &error);
            require(!scaledPreview.isNull() && scaledPreview.width() <= 2048 &&
                        scaledPreview.height() <= 2048 &&
                        scalingAdmissions->back().second >=
                            scalingSource.preview.sizeInBytes() + scaledPreview.sizeInBytes(),
                    "exact fit-preview scaling must admit its complete premultiplied input copy");
            for (const bool denyScratch : {false, true}) {
                auto partialCacheOwner = std::make_shared<std::weak_ptr<int>>();
                auto fallback = scalingSource;
                const qint64 cachedBytes = scalingPixels->exactImage.sizeInBytes();
                fallback.rows.reserveResources = [denyScratch, cachedBytes, partialCacheOwner](
                                                     qint64 retained, qint64 scratch,
                                                     QString* admissionError) {
                    if (retained == cachedBytes) {
                        if (!denyScratch) {
                            *admissionError = QStringLiteral("Cache admission rejected");
                            return std::shared_ptr<void>{};
                        }
                        auto lease = std::make_shared<int>(1);
                        *partialCacheOwner = lease;
                        return std::static_pointer_cast<void>(lease);
                    }
                    if (retained == 0 && scratch > 0 && denyScratch &&
                        !partialCacheOwner->expired()) {
                        *admissionError = QStringLiteral("Cache scratch admission rejected");
                        return std::shared_ptr<void>{};
                    }
                    require(partialCacheOwner->expired(),
                            "failed cache scratch must release retained admission before fallback");
                    return std::static_pointer_cast<void>(std::make_shared<int>(1));
                };
                error.clear();
                const auto streamed =
                    pipeline::preparePixels(fallback, scalingSize, cancellation, &error);
                require(streamed && streamed->exactImage.isNull() &&
                            streamed->rows.backingImage.isNull() && error.isEmpty(),
                        "failed cache admission must fall back to lazy rows without a stale error");
                const QRect fallbackRegion(2100, 500, 77, 43);
                require(samePixels(pipeline::readRegion(streamed->rows, fallbackRegion,
                                                        cancellation, &error),
                                   scalingPixels->exactImage.copy(fallbackRegion)),
                        "cache admission fallback must preserve the same native resized pixels");
            }
            for (const auto format :
                 {Format::Jpeg, Format::Webp, Format::Jxl, Format::Avif, Format::Pdf}) {
                const auto encoded =
                    pipeline::render(source, {image.size(), format, 72}, cancellation, &error);
                require(encoded != nullptr, "bounded codec preview encoding failed");
                const QImage reduced = pipeline::preview(*encoded, source, cancellation, &error);
                require(!reduced.isNull() && reduced.width() <= 2048 && reduced.height() <= 2048,
                        "codec artifacts must retain a bounded preview");
                const QImage native =
                    pipeline::decodeRegion(*encoded, region, cancellation, &error);
                const QImage complete = pipeline::decode(*encoded, cancellation, &error);
                require(!complete.isNull() && samePixels(native, complete.copy(region)),
                        "native viewport pixels must equal the full encoded artifact decode");
            }
            QImage transparent = image;
            transparent.setPixelColor(5, 2, QColor(70, 90, 110, 21));
            auto unknown = snow_shot::image_codec::srgbRowSource(transparent);
            unknown.backingImage = {};
            unknown.previewImage =
                transparent.scaled(QSize(22, 2048), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            const auto classified = pipeline::prepare(unknown, cancellation, &error);
            require(classified.alphaContent == snow::image::AlphaContent::non_opaque,
                    "an unverified preview hint must not suppress source alpha classification");
            auto admitted = std::make_shared<QList<std::weak_ptr<int>>>();
            auto resizeRows = snow_shot::image_codec::srgbRowSource(resizeFixture);
            resizeRows.reserveResources = [admitted](qint64, qint64, QString*) {
                auto owner = std::make_shared<int>(1);
                admitted->append(owner);
                return std::static_pointer_cast<void>(owner);
            };
            const auto resizeSource = pipeline::prepare(resizeRows, cancellation, &error);
            QElapsedTimer resizeElapsed;
            resizeElapsed.start();
            auto resized = pipeline::preparePixels(resizeSource, largeSize, cancellation, &error);
            require(resized && resized->exactImage.isNull() &&
                        resized->rows.backingImage.isNull() && admitted->size() == 1 &&
                        !admitted->front().expired(),
                    "large resize must admit and retain only a reusable lazy row plan");
            const QRect resizedRegion(2100, 500, 77, 43);
            QImage native =
                pipeline::readRegion(resized->rows, resizedRegion, cancellation, &error);
            const bool matchingRegion = samePixels(native, reference.copy(resizedRegion));
            if (!matchingRegion || admitted->size() != 3 || admitted->at(1).expired() ||
                !admitted->at(2).expired()) {
                std::cerr << "lazy resize region: pixels=" << matchingRegion
                          << " admissions=" << admitted->size()
                          << " elapsed=" << resizeElapsed.elapsed()
                          << " cancelled=" << cancellation.isCancellationRequested()
                          << " error=" << error.toStdString() << " owners=";
                for (const auto& admission : *admitted)
                    std::cerr << !admission.expired();
                std::cerr << '\n';
            }
            require(matchingRegion && admitted->size() == 3 && !admitted->at(1).expired() &&
                        admitted->at(2).expired(),
                    "lazy resize native detail must match the full scaler and retire scratch");
            auto encoded =
                pipeline::render(resized, {largeSize, Format::Png, 100}, cancellation, &error);
            require(encoded &&
                        samePixels(pipeline::decode(*encoded, cancellation, &error), reference),
                    "lazy PNG export must match every pixel from the full resize reference");
            auto retainedRows = resized->rows;
            resized.reset();
            encoded.reset();
            native = {};
            require(
                !admitted->front().expired(),
                "row copies must keep resize admission alive after the prepared object retires");
            retainedRows = {};
            require(admitted->front().expired() && admitted->at(1).expired(),
                    "retired row plans and viewport pixels must release retained admissions");
            return ScreenshotExportTaskResult{};
        },
        [&](ScreenshotExportTaskResult result) {
            failure = result.error;
            finished = true;
        });
    require(job.isValid(), "bounded preview test could not enter the export queue");
    // Pixel and memory contracts do not impose a codec throughput deadline.
    // The CTest process timeout bounds worker hangs; this shorter watchdog
    // checks only delivery of the GUI completion after the CPU work retires.
    workRetired.acquire();
    processUntil([&] { return finished; });
    require(failure.isEmpty(), qPrintable(failure));
}

void debouncedPreviewEdits(QWidget& owner) {
    require(storage::ScreenshotSettings().setLastManualSaveState(QStringLiteral("jpeg"), {}),
            "debounced preview settings setup failed");
    auto* modal = openDialog(owner, fixture());
    auto* content = modal->contentWidget();
    ExportObserver observer(content);
    auto* quality = child<AdSlider>(content, "saveQualitySlider");
    const auto previous = content->property("previewGeneration").toULongLong();
    for (int value : {61, 62, 63, 64})
        quality->setValue(value);
    require(observer.encodes == 0, "quality bursts must wait for their committed final value");
    processUntil([&] { return content->property("previewGeneration").toULongLong() > previous; });
    require(observer.encodes == 1 && observer.publications == 1,
            "a rapid quality burst must encode and publish only its latest value");
    const auto generation = content->property("previewGeneration").toULongLong();
    child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("png"));
    processUntil([&] { return content->property("previewGeneration").toULongLong() > generation; });
    auto* canvas = child<ScreenshotSavePreviewCanvas>(content, "savePreviewCanvas");
    const uchar* previewPixels = canvas->outputImage().constBits();
    const int decodes = content->property("previewDecodeCount").toInt();
    const auto compressionGeneration = content->property("previewGeneration").toULongLong();
    auto* compression = child<AdSelect>(content, "saveCompressionLevelSelect");
    compression->setCurrentValue(compression->currentValue().toString() == QStringLiteral("high")
                                     ? QStringLiteral("low")
                                     : QStringLiteral("high"));
    processUntil([&] {
        return content->property("previewGeneration").toULongLong() > compressionGeneration;
    });
    require(canvas->outputImage().constBits() == previewPixels &&
                content->property("previewDecodeCount").toInt() == decodes,
            "PNG compression edits must reuse unchanged displayed pixels");
    modal->reject();
    flush();
}

void boundedCanvasNativeDetail(QWidget& owner) {
    require(storage::ScreenshotSettings().setLastManualSaveState(QStringLiteral("png"), {}),
            "native viewport settings setup failed");
    const QImage original = fixture(QSize(128, 6000));
    auto* modal = openDialog(owner, original);
    auto* content = modal->contentWidget();
    auto* canvas = child<ScreenshotSavePreviewCanvas>(content, "savePreviewCanvas");
    require(canvas->outputImage().height() <= 2048 &&
                content->property("previewDecodeCount").toInt() == 0,
            "large exact outputs must display a bounded preview without file decoding");
    for (int iteration = 0; iteration < 64 && canvas->zoom() < 1; ++iteration) {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Plus, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    }
    require(canvas->zoom() >= 1, "native viewport keyboard zoom failed");
    processUntil([&] { return !canvas->outputDetailImage().isNull(); });
    require(
        canvas->outputDetailImage().size() == canvas->outputDetailRegion().size() &&
            samePixels(canvas->outputDetailImage(), original.copy(canvas->outputDetailRegion())),
        "zooming to native pixels must restore the original viewport detail");
    modal->reject();
    flush();
}

void oversizedManualPngStreamsWithoutPopulatingCache(QWidget& owner, const QTemporaryDir& temp) {
    const storage::ScreenshotSettings settings;
    require(settings.setLastManualSaveState(QStringLiteral("png"), {}),
            "streaming PNG fixture settings failed");
    const QImage image = fixture();
    auto artifact = std::make_shared<ScreenshotExportArtifact>(
        ScreenshotExportSource::fromImage(image), ScreenshotCompressionLevel::Low,
        ScreenshotExportArtifact::PngCachePolicy{1, {}});
    require(!artifact->shouldCachePng(image.size()), "large image should use streaming");
    QString savedPath;
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact,
                                             [&](const QString& path) { savedPath = path; }),
            "streaming PNG dialog did not open");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    QPointer<QWidget> content = modal->contentWidget();
    processUntil([&] { return content->property("previewGeneration").toULongLong() > 0; });
    require(!artifact->cachedPng(ScreenshotCompressionLevel::Low).isValid(),
            "streaming preview populated the in-memory PNG cache");
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("streamed-png"));
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(samePixels(QImage(savedPath), image), "streamed PNG changed the source pixels");
    flush();
}

void obsoleteSharedPngDoesNotBlockCurrentPreview(QWidget& owner) {
    const storage::ScreenshotSettings settings;
    require(
        settings.setLastManualSaveState(
            QStringLiteral("png"),
            QJsonObject{{QStringLiteral("png"), QJsonObject{{QStringLiteral("compression_level"),
                                                             QStringLiteral("high")}}}}),
        "PNG preview fixture settings failed");
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto released = std::make_shared<std::atomic_bool>(false);
    const auto release = qScopeGuard([released] { released->store(true); });
    auto rows = snow_shot::image_codec::srgbRowSource(fixture());
    rows.readRows = [read = rows.readRows, entered, released](
                        int first, int count, qsizetype stride, uchar* target, qsizetype capacity) {
        if (!entered->exchange(true)) {
            while (!released->load())
                QThread::msleep(1);
        }
        return read(first, count, stride, target, capacity);
    };
    auto artifact = std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromProducer(
        {}, [rows](std::function<bool()> cancellation) mutable {
            rows.cancellationRequested = std::move(cancellation);
            return rows;
        }));
    require(ScreenshotSaveAsFileDialog::open(&owner, &owner, artifact),
            "stale shared PNG dialog did not open");
    auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
    QPointer<QWidget> content = modal->contentWidget();
    const auto close = qScopeGuard([&] {
        if (content) {
            modal->reject();
            flush();
        }
    });
    processUntil([&] { return entered->load(); });
    child<AdSelect>(content, "saveFormatSelect")->setCurrentValue(QStringLiteral("jpeg"));
    processUntil([&] { return content->property("previewGeneration").toULongLong() > 0; });
    const auto generation = content->property("previewGeneration").toULongLong();
    bool sharedCompleted = false;
    require(artifact->requestPng(&owner, ScreenshotCompressionLevel::High,
                                 [&](ScreenshotExportEncodingResult result) {
                                     require(result.succeeded(), "shared PNG was cancelled");
                                     sharedCompleted = true;
                                 }),
            "shared PNG subscriber rejected");
    released->store(true);
    processUntil([&] { return sharedCompleted; });
    flush();
    require(content->property("previewGeneration").toULongLong() == generation,
            "obsolete shared PNG replaced the current preview");
}

void sourceSizedPngSharesMatchingEncoding(QWidget& owner, const QTemporaryDir& temp) {
    const storage::ScreenshotSettings settings;
    require(settings.setCompressionLevel(QStringLiteral("low")),
            "global PNG compression fixture failed");
    for (const auto& customCompression :
         {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}) {
        require(settings.setLastManualSaveState(
                    QStringLiteral("png"),
                    QJsonObject{
                        {QStringLiteral("png"),
                         QJsonObject{{QStringLiteral("compression_level"), customCompression}}}}),
                "custom PNG compression fixture failed");
        const QImage image = fixture();
        auto reads = std::make_shared<std::atomic_int>(0);
        ScreenshotImageRowSource rows = snow_shot::image_codec::srgbRowSource(image);
        rows.readRows = [read = rows.readRows, reads](int first, int count, qsizetype stride,
                                                      uchar* target, qsizetype capacity) {
            ++*reads;
            return read(first, count, stride, target, capacity);
        };
        auto artifact =
            std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromProducer(
                {}, [rows](std::function<bool()> cancellation) mutable {
                    rows.cancellationRequested = std::move(cancellation);
                    return rows;
                }));

        QByteArray existingPng;
        require(artifact->requestPng(
                    &owner, ScreenshotImageFileService::compressionLevelForKey(customCompression),
                    [&](ScreenshotExportEncodingResult result) {
                        require(result.succeeded(), "preexisting PNG encoding failed");
                        existingPng = result.image.bytes();
                    }),
                "preexisting PNG request rejected");
        processUntil([&] { return !existingPng.isEmpty(); });
        const int readsBeforeDialog = reads->load();
        QString savedPath;
        QByteArray historyPng;
        int historyCallbacks = 0;
        require(ScreenshotSaveAsFileDialog::open(
                    &owner, &owner, artifact,
                    [&, artifact](const QString& path) {
                        savedPath = path;
                        require(artifact->requestPng(
                                    &owner,
                                    ScreenshotImageFileService::compressionLevelForKey(
                                        customCompression),
                                    [&](ScreenshotExportEncodingResult result) {
                                        require(result.succeeded(),
                                                "matching cached PNG request failed after save");
                                        historyPng = result.image.bytes();
                                        ++historyCallbacks;
                                    }),
                                "matching cached PNG request was rejected after save");
                    }),
                "history-adoption export dialog did not open");
        auto* modal = child<AdModal>(&owner, "screenshotSaveAsFileModal");
        QPointer<QWidget> content = modal->contentWidget();
        processUntil([&] { return content->property("previewGeneration").toULongLong() > 0; });
        const int readsBeforeSave = reads->load();
        require(readsBeforeSave == readsBeforeDialog,
                "source-sized PNG preview re-encoded an existing matching artifact");

        child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
        child<AdLineEdit>(content, "saveFilenameInput")
            ->setText(QStringLiteral("history-adoption-%1").arg(customCompression));
        modal->acceptButton()->click();
        processUntil([&] { return !savedPath.isEmpty() && historyCallbacks == 1; });

        QFile saved(savedPath);
        require(saved.open(QIODevice::ReadOnly), "source-sized PNG save is unreadable");
        const QByteArray savedPng = saved.readAll();
        require(reads->load() == readsBeforeSave && savedPng == historyPng &&
                    historyPng.constData() == existingPng.constData(),
                "source-sized PNG did not share the matching cached encoding");
        flush();
    }
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
            processUntil(
                [&] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 3; });
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
            PhysicalKeyEvent key(QEvent::KeyPress, character.unicode(), Qt::NoModifier,
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

void repeatedPreviewChangesKeepLatestRequest(QWidget& owner, const QTemporaryDir& temp) {
    auto probe = std::make_shared<ExportProbe>();
    QString savedPath;
    auto* modal = openCountedDialog(owner, probe, [&](const QString& path) { savedPath = path; });
    auto* content = modal->contentWidget();
    ExportObserver observer(content);
    WorkerGate gate;
    const int workerCount = std::clamp(QThread::idealThreadCount(), 1, 2);
    gate.block(&owner, workerCount);
    processUntil([&] { return gate.started->load() == workerCount; });
    auto* width = child<AdInputNumber>(content, "saveWidthInput");
    for (int index = 0; index < 40; ++index) {
        width->setValue(80 + index);
        require(ScreenshotExportCoordinator::shared().pendingJobCount() == workerCount,
                "a superseded queued preview must retire before the next debounce expires");
        // Wait for each committed edit to enter the queue before superseding it, so this
        // exercises queued cancellation independently of rapid-edit debounce coalescing.
        processUntil([&] {
            return ScreenshotExportCoordinator::shared().pendingJobCount() == workerCount + 1;
        });
        require(ScreenshotExportCoordinator::shared().pendingJobCount() == workerCount + 1 &&
                    child<QLabel>(content, "saveErrorLabel")->isHidden() && probe->passes == 1 &&
                    observer.encodes == 0 && observer.publications == 0,
                "only the latest preview may occupy a queued slot");
    }
    *gate.released = true;
    processUntil([&] { return observer.publications == 1; });
    require(observer.encodes == 1 && probe->passes == 2 && width->value() == 119 &&
                observer.displayed.width() == 119,
            "superseded preview jobs must not read or encode the source");
    child<AdLineEdit>(content, "saveDirectoryInput")->setText(temp.path());
    child<AdLineEdit>(content, "saveFilenameInput")->setText(QStringLiteral("latest-preview"));
    modal->acceptButton()->click();
    processUntil([&] { return !savedPath.isEmpty(); });
    require(QImage(savedPath).width() == 119 && observer.encodes == 1,
            "Save must reuse the latest successful preview without another edit or retry");
    flush();
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
    processUntil([&] { return !child<QLabel>(content, "saveErrorLabel")->isHidden(); });
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

void runScreenshotSaveResultTests();

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
        if (app.arguments().contains(QStringLiteral("--save-result-only"))) {
            runScreenshotSaveResultTests();
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--native-detail-transparency-only"))) {
            canvasNativeDetailPreservesTransparency();
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--bounded-native-regions-only"))) {
            boundedPreviewAndNativeRegions();
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--canvas"))) {
            canvasInteraction();
            canvasNativeDetailPreservesTransparency();
            canvasZoomHint();
            canvasOriginalSizeBaseline();
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Export preview canvas tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--placement"))) {
            centersOnDisplayOverlay();
            saveDialogDoesNotResurrectPopover();
            saveDialogKeepsToolbarVisible();
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Export dialog placement tests passed\n";
            return 0;
        }
        QWidget owner;
        owner.resize(1200, 800);
        owner.show();
        if (app.arguments().contains(QStringLiteral("--time-variables"))) {
            timeDirectoryDefaultsAndShortcut(owner, temp);
            windowNameDefaultsAndShortcut(owner, temp);
            dottedWindowNameDefaultsAndSaves(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            return 0;
        }
        if (QApplication::arguments().contains(QStringLiteral("--collage-modality"))) {
            collageWindowModalityAndRetry(owner);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Collage export dialog modality tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--filename-warning"))) {
            duplicateFilenameWarning(owner, temp);
            overwriteRequiresConfirmation(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Duplicate filename warning tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--pdf"))) {
            canvasPdfComparison();
            pdfDialogAndSettings(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "PDF dialog tests passed\n";
            return 0;
        }
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
            repeatedPreviewChangesKeepLatestRequest(owner, temp);
            rejectedCalculationRetries(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Shared export calculation tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--optimized-pipeline"))) {
            canvasNativeDetailPreservesTransparency();
            boundedPreviewAndNativeRegions();
            debouncedPreviewEdits(owner);
            boundedCanvasNativeDetail(owner);
            oversizedManualPngStreamsWithoutPopulatingCache(owner, temp);
            obsoleteSharedPngDoesNotBlockCurrentPreview(owner);
            optimizedPipelineBehavior(owner);
            unbackedExactPreviewRetainsPixels(owner);
            sourceSizedPngSharesMatchingEncoding(owner, temp);
            ScreenshotExportCoordinator::shared().shutdown();
            storage::ApplicationStorage::instance().shutdown();
            std::cout << "Optimized export pipeline tests passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--zoom-hint")) ||
            app.arguments().contains(QStringLiteral("--wheel-zoom")) ||
            app.arguments().contains(QStringLiteral("--canvas-baseline")) ||
            app.arguments().contains(QStringLiteral("--unchanged-edits")) ||
            app.arguments().contains(QStringLiteral("--row-backed-preview"))) {
            if (app.arguments().contains(QStringLiteral("--wheel-zoom")))
                canvasWheelZoom();
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
        saveDialogDoesNotResurrectPopover();
        saveDialogKeepsToolbarVisible();
        reusablePathInputsAndSettings();
        persistence(temp);
        stateRules(temp);
        rememberedManualFormat(owner, temp);
        encodingAndFullResolutionDisplay();
        canvasInteraction();
        canvasNativeDetailPreservesTransparency();
        canvasPdfComparison();
        canvasZoomHint();
        canvasWheelZoom();
        canvasOriginalSizeBaseline();
        unchangedPreviewEdits(owner);
        shortcutsAndCancellation(owner, temp);
        previewAndSave(owner, temp);
        sizeUnits(owner, temp);
        duplicateFilenameWarning(owner, temp);
        overwriteRequiresConfirmation(owner, temp);
        shortcutWrappingAndLanguageChange(owner, temp);
        rowBackedDialogAndStalePreview(owner, temp);
        oversizedManualPngStreamsWithoutPopulatingCache(owner, temp);
        obsoleteSharedPngDoesNotBlockCurrentPreview(owner);
        optimizedPipelineBehavior(owner);
        unbackedExactPreviewRetainsPixels(owner);
        sourceSizedPngSharesMatchingEncoding(owner, temp);
        saveReusesCalculatedResult(owner, temp);
        committedControlsAndSave(owner, temp);
        retainedResultFailures(owner, temp);
        failedAndClosedCalculations(owner, temp);
        repeatedPreviewChangesKeepLatestRequest(owner, temp);
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
