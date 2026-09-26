#include "snow_shot/presentation/screenshotregiontypecontrol.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/shortcuts/shortcutdisplayservice.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "../src/presentation/tools/screenshottoolpalettebuttons.h"
#include "../src/presentation/tools/screenshottoolpalettestylecomponents.h"
#include "../src/presentation/tools/screenshottoolpalettestylepresets.h"

#include "antd_icons.h"
#include "widgets/select.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QClipboard>
#include <QDir>
#include <QEnterEvent>
#include <QCursor>
#include <QEventLoop>
#include <QComboBox>
#include <QFrame>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QScreen>
#include <QGridLayout>
#include <QHash>
#include <QHelpEvent>
#include <QImage>
#include <QJsonObject>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QSlider>
#include <QSpacerItem>
#include <QPushButton>
#include <QPointer>
#include <QWheelEvent>
#include <QWidget>

#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/alert.h"
#include "widgets/color_picker.h"
#include "widgets/control_scale.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_number.h"
#include "widgets/form.h"
#include "widgets/modal.h"
#include "widgets/popover.h"
#include "widgets/radio.h"
#include "widgets/radio_button_group.h"
#include "widgets/select.h"
#include "widgets/slider.h"
#include "widgets/tooltip.h"

#include <tuple>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QString shortcutTooltip(const QString& label, const QStringList& portable) {
    const QString display = snow_shot::shortcuts::formatShortcutListDisplayText(
        snow_shot::shortcuts::bindingsFromPortableText(portable));
    return display.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, display);
}

QWidget* controlWithTooltip(ScreenshotToolPalette& palette, const char* tooltip);

QImage renderButton(QWidget& button) {
    QImage image(button.size(), QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    button.render(&painter);
    return image;
}

QColor buttonBackgroundSample(QWidget& button) {
    const QImage image = renderButton(button);
    return image.pixelColor(qMax(1, image.width() / 8), image.height() / 2);
}

void translucentColorSwatchesShowCheckerboardUnderlay() {
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    for (const qreal scale : {1.0, 1.5, 2.0}) {
        const ScreenshotToolPaletteButtonMetrics metrics{qRound(30 * scale), qRound(18 * scale),
                                                         scale};
        adqt::widgets::AdColorPicker drawingPicker;
        auto* drawingTrigger =
            snow_shot::presentation::createScreenshotToolPaletteColorPickerTrigger(
                &drawingPicker, QString(), Qt::transparent, metrics,
                ColorPickerTrigger::Preview::Fill);
        adqt::widgets::AdControlScaleScope triggerScope(drawingTrigger);
        const auto context =
            adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1.0, 1.0, scale);
        triggerScope.publishScale(context);
        const QImage triggerImage = renderButton(*drawingTrigger);
        for (const bool summary : {false, true}) {
            std::unique_ptr<ColorSwatchButton> button(createScreenshotToolPaletteColorButton(
                nullptr, nullptr, Qt::red, summary, true, metrics));
            adqt::widgets::AdControlScaleScope buttonScope(button.get());
            buttonScope.publishScale(context);
            for (const int alpha : {128, 254, 255, 0}) {
                const QColor color(255, 0, 0, alpha);
                button->setSwatchColor(color);
                const QImage image = renderButton(*button);
                const QRect interior =
                    QRect(image.rect().center() - QPoint(qRound(6 * scale), qRound(6 * scale)),
                          QSize(qRound(12 * scale), qRound(12 * scale)));
                QImage expectedCells(2, 1, QImage::Format_RGBA8888);
                expectedCells.fill(scheme.map.colorBgContainer);
                {
                    QPainter painter(&expectedCells);
                    painter.fillRect(0, 0, 1, 1, scheme.map.colorFillTertiary);
                    painter.fillRect(expectedCells.rect(), color);
                }
                int alternateCells = 0;
                int baseCells = 0;
                for (int y = interior.top(); y <= interior.bottom(); ++y) {
                    for (int x = interior.left(); x <= interior.right(); ++x) {
                        const QColor pixel = image.pixelColor(x, y);
                        alternateCells += pixel == expectedCells.pixelColor(0, 0) ? 1 : 0;
                        baseCells += pixel == expectedCells.pixelColor(1, 0) ? 1 : 0;
                        if (alpha == 0) {
                            require(
                                pixel == triggerImage.pixelColor(x, y),
                                "transparent presets must match the drawing fill picker trigger");
                        }
                        if (alpha == 255) {
                            require(pixel == color, "opaque swatches must remain solid colors");
                        }
                    }
                }
                require(alternateCells >= 8 && baseCells >= 8,
                        "translucent swatches must use the drawing trigger's themed checker cells");
            }
        }
    }
}

bool imageHasVisiblePixel(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() != 0) {
                return true;
            }
        }
    }
    return false;
}

bool imageHasOpaqueLightPixel(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() >= 200 && color.red() >= 220 && color.green() >= 220 &&
                color.blue() >= 220) {
                return true;
            }
        }
    }
    return false;
}

int longestHorizontalColorRun(const QImage& image, const QColor& color) {
    int longestRun = 0;
    for (int y = 0; y < image.height(); ++y) {
        int run = 0;
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == color) {
                ++run;
                longestRun = std::max(longestRun, run);
            } else {
                run = 0;
            }
        }
    }
    return longestRun;
}

ScreenshotToolPalette::RecordingSessionStatus
recordingSessionFor(ScreenshotToolPalette::RecordingState state,
                    ScreenshotToolPalette::RecordingBusyOperation operation) {
    using Status = ScreenshotToolPalette::RecordingSessionStatus;
    using State = ScreenshotToolPalette::RecordingState;
    using BusyOp = ScreenshotToolPalette::RecordingBusyOperation;
    if (operation == BusyOp::None) {
        return Status::fromState(state);
    }
    if (operation == BusyOp::Starting) {
        return Status::starting();
    }
    if (operation == BusyOp::Stopping) {
        return state == State::Paused ? Status::pausedStopping() : Status::stopping();
    }
    return state == State::Paused ? Status::pausedCopying() : Status::copying();
}

void recordingSessionStatusMakesInvalidCombinationsUnrepresentable() {
    using Status = ScreenshotToolPalette::RecordingSessionStatus;
    using State = ScreenshotToolPalette::RecordingState;
    using BusyOp = ScreenshotToolPalette::RecordingBusyOperation;
    const Status valid[] = {Status::idle(),    Status::starting(),     Status::recording(),
                            Status::paused(),  Status::stopping(),     Status::pausedStopping(),
                            Status::copying(), Status::pausedCopying()};
    for (const auto& status : valid) {
        const bool starting = status.busyOperation() == BusyOp::Starting;
        const bool finishing =
            status.busyOperation() == BusyOp::Stopping || status.busyOperation() == BusyOp::Copying;
        require((starting && status.state() == State::Idle) ||
                    (finishing && status.state() != State::Idle) || !status.busy(),
                "every constructible recording session must pair busy work with a valid phase");
    }
    require(Status::starting().state() == State::Idle &&
                Status::copying().state() == State::Recording &&
                Status::pausedCopying().state() == State::Paused,
            "named factories must pin start and copy to their session phases");
    require(Status::idle().finishing(true) == Status::idle() &&
                Status::recording().finishing(true) == Status::copying() &&
                Status::paused().finishing(false) == Status::pausedStopping() &&
                Status::copying().finishing(false) == Status::copying(),
            "finishing must keep idle and in-flight sessions unchanged");

    ScreenshotToolPalette::Options options;
    options.showRecordingControls = true;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette palette(options);
    palette.prepareForDisplay();
    palette.setRecordingSession(Status::copying());
    require(palette.recordingSession() == Status::copying() &&
                palette.recordingBusyOperation() == BusyOp::Copying,
            "copying must be published as a session, not a boolean busy flag");
    palette.setRecordingState(State::Idle);
    require(palette.recordingSession() == Status::idle() && !palette.recordingBusy(),
            "changing the recording phase must drop any leftover copy or stop spinner");
}

void recordingControlsRemainLaidOutAcrossStateChanges() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showRecordingControls = true;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette palette(options);
    palette.prepareForDisplay();
    palette.show();
    QCoreApplication::processEvents();

    const char* sources[] = {"Start recording",       "Stop recording",    "Pause recording",
                             "Resume recording",      "Record microphone", "Record speakers",
                             "Open recording folder", "Close recording",   "Copy recording"};
    QVector<adqt::widgets::AdButton*> buttons;
    for (const char* source : sources) {
        adqt::widgets::AdButton* match = nullptr;
        for (auto* button : palette.mainPanel()->findChildren<adqt::widgets::AdButton*>()) {
            if (button->accessibleName() == QLatin1String(source)) {
                match = button;
                break;
            }
        }
        require(match != nullptr, "every recording control should be created");
        buttons.push_back(match);
    }
    auto* duration = palette.findChild<QLabel*>(QStringLiteral("screenRecordingDuration"));
    require(duration != nullptr, "recording duration should be created");

    using State = ScreenshotToolPalette::RecordingState;
    using BusyOp = ScreenshotToolPalette::RecordingBusyOperation;
    const auto verify = [&](State state, BusyOp operation) {
        palette.setRecordingSession(recordingSessionFor(state, operation));
        palette.prepareForDisplay();
        QCoreApplication::processEvents();
        const bool idle = state == State::Idle;
        const bool paused = state == State::Paused;
        const bool busy = operation != BusyOp::None;
        const bool visible[] = {idle, !idle, !paused, paused, true, true, true, true, true};
        const bool enabled[] = {idle && !busy,
                                !idle && !busy,
                                state == State::Recording && !busy,
                                paused && !busy,
                                idle && !busy,
                                idle && !busy,
                                true,
                                !busy,
                                !idle && !busy};
        // Only the initiating start, stop, or copy action shows a spinner.
        const bool spinning[] = {operation == BusyOp::Starting,
                                 operation == BusyOp::Stopping,
                                 false,
                                 false,
                                 false,
                                 false,
                                 false,
                                 false,
                                 operation == BusyOp::Copying};
        const QLayout* layout = palette.mainPanel()->layout();
        QRect previous;
        for (int index = 0; index < buttons.size(); ++index) {
            auto* button = buttons.at(index);
            require(layout->indexOf(button) >= 0,
                    "every recording control must remain in the main toolbar layout");
            if (button->isVisible() != visible[index]) {
                std::cerr << "visibility mismatch for " << sources[index] << " in state "
                          << static_cast<int>(state) << " busy=" << static_cast<int>(operation)
                          << ": expected " << visible[index] << ", actual " << button->isVisible()
                          << '\n';
            }
            require(button->isVisible() == visible[index],
                    "recording control visibility should follow recording state");
            require(button->isEnabled() == enabled[index],
                    "recording control availability should follow recording and busy state");
            require(button->busy() == spinning[index],
                    "recording busy indicators must stay on the initiating action");
            if (visible[index]) {
                require(!button->visibleRegion().isEmpty() &&
                            palette.mainPanel()->rect().contains(button->geometry()),
                        "visible recording controls should be unclipped inside the toolbar");
                require(!previous.isValid() || previous.right() < button->geometry().left(),
                        "recording controls should retain their order without overlap");
                previous = button->geometry();
            }
        }
        require(layout->indexOf(duration) >= 0 && duration->isVisible() &&
                    palette.mainPanel()->rect().contains(duration->geometry()),
                "recording duration must remain visible inside the toolbar layout");
    };

    verify(State::Idle, BusyOp::None);
    for (State state : {State::Recording, State::Paused, State::Idle}) {
        verify(state, BusyOp::None);
    }
    verify(State::Idle, BusyOp::Starting);
    for (State state : {State::Recording, State::Paused}) {
        verify(state, BusyOp::Stopping);
        verify(state, BusyOp::Copying);
    }
    palette.setRecordingSession(ScreenshotToolPalette::RecordingSessionStatus::stopping());
    require(palette.recordingSession() ==
                    ScreenshotToolPalette::RecordingSessionStatus::stopping() &&
                buttons.at(1)->busy() && !buttons.last()->busy(),
            "stopping must spin stop without spinning copy");
    palette.setRecordingState(State::Idle);
    require(palette.recordingSession() == ScreenshotToolPalette::RecordingSessionStatus::idle() &&
                !buttons.at(1)->busy() && !buttons.last()->busy(),
            "returning to idle must clear every recording busy indicator");
    palette.setToolbarLayout({});
    palette.setActionToolsLayout({});
    verify(State::Idle, BusyOp::None);
    palette.setRecordingDuration(65000);
    require(duration->text() == QStringLiteral("00:01:05"),
            "visible recording duration should update");
    for (qreal scale : {1.0, 0.75, 1.25, 1.5, 2.0}) {
        palette.setPhysicalScale(scale);
        int previousWidth = 0;
        for (qint64 milliseconds : {0LL, 65000LL, 359999000LL, 360000000LL}) {
            palette.setRecordingDuration(milliseconds);
            palette.prepareForDisplay();
            QCoreApplication::processEvents();
            if (duration->width() < duration->sizeHint().width()) {
                std::cerr << "duration " << duration->text().toStdString() << " at scale " << scale
                          << ": allocated " << duration->width() << ", required "
                          << duration->sizeHint().width() << '\n';
            }
            require(duration->width() >= duration->sizeHint().width(),
                    "recording duration must fit its complete text without clipping");
            require(palette.mainPanel()->rect().contains(duration->geometry()),
                    "resized recording duration must remain inside the toolbar");
            if (milliseconds < 360000000LL) {
                require(previousWidth == 0 || duration->width() == previousWidth,
                        "timer ticks must not change the width of a two-digit-hour display");
            }
            previousWidth = duration->width();
        }
    }
    int startRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingStartRequested, &palette,
                     [&]() { ++startRequests; });
    buttons.constFirst()->click();
    require(startRequests == 1, "the visible start button should request recording");
}

void recordingCursorOptionsAreIndependentAndLazy() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showRecordingControls = true;
    options.recordingDrawingMode = true;
    options.enableStyleToolbar = true;
    ScreenshotToolPalette palette(options);
    palette.show();
    palette.prepareForDisplay();
    QCoreApplication::processEvents();
    auto* cursor =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingShowCursor"));
    auto* popover = palette.findChild<adqt::widgets::AdPopover*>(
        QStringLiteral("screenRecordingCursorPopover"));
    require(cursor && popover && !popover->contentWidget(), "cursor options must be lazy");
    require(popover->triggers() == adqt::widgets::AdPopover::Trigger::Hover &&
                popover->popupLayerMode() == adqt::widgets::AdPopover::PopupLayerMode::QtTool,
            "cursor options must use the native hover popover");
    require(popover->placement() == adqt::widgets::AdPopover::Placement::Bottom,
            "cursor options must open downward by default like the row's color pickers");
    require(!palette.recordingMouseHighlightEnabled() && !palette.recordingRecordMouseClicks() &&
                palette.recordingMouseHighlightColor() == QColor(255, 255, 0, 128),
            "mouse effect defaults");
    int highlights = 0, clicks = 0, cursors = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingMouseHighlightEnabledChanged,
                     &palette, [&](bool) { ++highlights; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingRecordMouseClicksChanged, &palette,
                     [&](bool) { ++clicks; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingCursorVisibleChanged, &palette,
                     [&](bool) { ++cursors; });
    popover->preparePopup();
    auto* highlight = popover->contentWidget()->findChild<adqt::widgets::AdCheckbox*>(
        QStringLiteral("screenRecordingMouseHighlight"));
    auto* click = popover->contentWidget()->findChild<adqt::widgets::AdCheckbox*>(
        QStringLiteral("screenRecordingRecordMouseClicks"));
    require(highlight && click && !highlight->isChecked() && !click->isChecked(),
            "both checkboxes start unchecked");
    const QString snapshotDirectory = qEnvironmentVariable("SNOW_RECORDING_UI_SNAPSHOT_DIR");
    if (!snapshotDirectory.isEmpty()) {
        QDir().mkpath(snapshotDirectory);
        require(popover->contentWidget()->grab().save(
                    QDir(snapshotDirectory).filePath(QStringLiteral("cursor-options.png"))),
                "save cursor options visual fixture");
    }
    highlight->click();
    click->click();
    require(highlights == 1 && clicks == 1 && cursors == 0 && palette.recordingCursorVisible() &&
                !palette.recordingKeyboardVisible(),
            "checkboxes must not activate either existing toggle");
    cursor->click();
    require(cursors == 1 && !palette.recordingCursorVisible() &&
                palette.recordingMouseHighlightEnabled(),
            "cursor visibility preserves highlight preference");
    palette.setRecordingMouseHighlightEnabled(false);
    palette.setRecordingRecordMouseClicks(false);
    require(highlights == 1 && clicks == 1 && !highlight->isChecked() && !click->isChecked(),
            "synchronization must not emit user changes");
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&palette, &languageChange);
    require(highlight->text() == QStringLiteral("Mouse highlight") &&
                click->text() == QStringLiteral("Record mouse clicks"),
            "cursor options retranslate");
    popover->show();
    QCoreApplication::processEvents();
    require(popover->isVisible() && popover->contentWidget() != nullptr,
            "opening the popover must materialize the cursor options");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QCoreApplication::processEvents();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape &&
                !palette.recordingExportSettingsVisible() && !popover->isVisible() &&
                popover->contentWidget() == nullptr,
            "switching tools must dismiss the popover and destroy its content");
    palette.setRecordingSession(ScreenshotToolPalette::RecordingSessionStatus::starting());
    require(!popover->isEnabled() && !popover->isVisible(),
            "busy recording must dismiss and disable cursor options");
}

void recordingEffectSettingsModal() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showRecordingControls = true;
    options.recordingDrawingMode = true;
    options.enableStyleToolbar = true;
    ScreenshotToolPalette palette(options);
    palette.show();
    palette.prepareForDisplay();
    QCoreApplication::processEvents();
    auto* button = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenRecordingEffectSettings"));
    auto* keyboard =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingShowKeyboard"));
    auto* separator =
        palette.findChild<QFrame*>(QStringLiteral("screenRecordingExportSettingsSeparator"));
    require(button && keyboard && separator,
            "recording settings must expose a button and separator");
    require(button->text().isEmpty() && button->accessibleName() == QStringLiteral("Settings") &&
                separator->x() > keyboard->x() && button->x() > separator->x(),
            "settings must follow the keyboard toggle at the right edge");
    const auto click = [](QWidget* widget) {
        const QPointF local = widget->rect().center();
        const QPointF global = widget->mapToGlobal(local.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(widget, &press);
        QCoreApplication::sendEvent(widget, &release);
        QCoreApplication::processEvents();
    };
    const auto flushLayout = []() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
    };
    // The dialog is destroyed with deleteLater(), so child lookups only settle
    // once the deferred deletion has run.
    const auto flushDeletes = []() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    };
    const auto requireSettingsAbsent = [&](const char* message) {
        flushDeletes();
        require(palette.recordingEffectSettingsModalForTests() == nullptr &&
                    palette.findChild<adqt::widgets::AdModal*>(
                        QStringLiteral("screenRecordingEffectSettingsModal")) == nullptr &&
                    palette.findChild<adqt::widgets::AdForm*>(
                        QStringLiteral("screenRecordingEffectSettingsForm")) == nullptr,
                message);
    };

    // Building the dialog dominated the recording window open path, so it must
    // not exist until the Settings button is actually used.
    requireSettingsAbsent("constructing and displaying the palette must not build settings");
    require(palette.findChildren<adqt::widgets::AdColorPicker*>().size() == 2,
            "only the export row pickers exist before settings is opened");
    // Retranslation walks the export row on every language change; it must not
    // touch, or materialize, the absent dialog.
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&palette, &languageChange);
    QCoreApplication::processEvents();
    require(button->accessibleName() == QStringLiteral("Settings"),
            "the settings button must retranslate without the dialog");
    requireSettingsAbsent("retranslation must not build settings");

    struct Settings {
        adqt::widgets::AdModal* modal = nullptr;
        adqt::widgets::AdForm* form = nullptr;
        adqt::widgets::AdInputNumber* duration = nullptr;
        adqt::widgets::AdInputNumber* keyboardSize = nullptr;
        adqt::widgets::AdColorPicker* background = nullptr;
        adqt::widgets::AdColorPicker* foreground = nullptr;
    };
    // Every open rebuilds the dialog, so pointers must be re-acquired and never
    // cached across a close.
    const auto openSettings = [&]() {
        click(button);
        flushLayout();
        Settings settings;
        settings.modal = palette.recordingEffectSettingsModalForTests();
        auto* highlight = settings.modal->contentWidget()->findChild<adqt::widgets::AdColorPicker*>(
            QStringLiteral("screenRecordingMouseHighlightColor"));
        auto* swatch = settings.modal->contentWidget()->findChild<QLabel*>(
            QStringLiteral("screenRecordingMouseHighlightSwatch"));
        require(highlight && swatch &&
                    highlight->value().solidColor == palette.recordingMouseHighlightColor(),
                "highlight picker and multiply swatch follow palette state");
        require(!swatch->pixmap().isNull(), "highlight preview must render a swatch");
        require(settings.modal != nullptr && settings.modal->isOpen(),
                "clicking Settings must build and open the modal");
        require(settings.modal == palette.findChild<adqt::widgets::AdModal*>(
                                      QStringLiteral("screenRecordingEffectSettingsModal")),
                "the rebuilt modal must keep its object name");
        settings.form = qobject_cast<adqt::widgets::AdForm*>(settings.modal->contentWidget());
        require(settings.form != nullptr, "settings must use the Ant Design form");
        settings.duration = settings.form->findChild<adqt::widgets::AdInputNumber*>(
            QStringLiteral("screenRecordingMouseTrailDuration"));
        settings.keyboardSize = settings.form->findChild<adqt::widgets::AdInputNumber*>(
            QStringLiteral("screenRecordingKeyboardSize"));
        settings.background = settings.form->findChild<adqt::widgets::AdColorPicker*>(
            QStringLiteral("screenRecordingKeyboardBackgroundColor"));
        settings.foreground = settings.form->findChild<adqt::widgets::AdColorPicker*>(
            QStringLiteral("screenRecordingKeyboardForegroundColor"));
        require(settings.duration && settings.keyboardSize && settings.background &&
                    settings.foreground,
                "the rebuilt form must expose every effect control");
        return settings;
    };

    QWidget recordingOwner;
    recordingOwner.setGeometry(40, 40, 600, 500);
    recordingOwner.show();
    palette.setRecordingSettingsOwnerWindow(&recordingOwner);
    const Settings opened = openSettings();
    adqt::widgets::AdModal* modal = opened.modal;
    adqt::widgets::AdForm* form = opened.form;
    const QString snapshotDirectory = qEnvironmentVariable("SNOW_RECORDING_UI_SNAPSHOT_DIR");
    if (!snapshotDirectory.isEmpty()) {
        QDir().mkpath(snapshotDirectory);
        require(form->window()->grab().save(
                    QDir(snapshotDirectory).filePath(QStringLiteral("recording-settings.png"))),
                "save recording settings visual fixture");
    }

    require(modal->mode() == adqt::widgets::AdModal::Mode::Window &&
                modal->windowModality() == Qt::ApplicationModal && modal->centered() &&
                !modal->maskVisible() && !modal->closeOnMaskClick() &&
                modal->ownerWindow() == &recordingOwner &&
                modal->windowTitle() == QStringLiteral("Settings") &&
                modal->standardButtons() == adqt::widgets::AdModal::StandardButton::Ok,
            "settings must use the selection editor's Windows-style modal conventions");
    require((modal->contentWidget()->window()->frameGeometry().center() -
             recordingOwner.frameGeometry().center())
                    .manhattanLength() <= 4,
            "settings must be centered on the recording owner window");
    auto* duration = form->findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("screenRecordingMouseTrailDuration"));
    auto* keyboardSize = form->findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("screenRecordingKeyboardSize"));
    require(keyboardSize && keyboardSize->value() == 64 && keyboardSize->minimum() == 32 &&
                keyboardSize->maximum() == 128,
            "keyboard size must expose pixel bounds and default");
    int sizeChanges = 0;
    int lastSize = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingKeyboardSizeChanged, &palette,
                     [&](int value) {
                         lastSize = value;
                         ++sizeChanges;
                     });
    keyboardSize->setValue(96);
    require(sizeChanges == 1 && lastSize == 96 && palette.recordingKeyboardSize() == 96,
            "size edits apply immediately");
    palette.setRecordingKeyboardSize(48);
    require(keyboardSize->value() == 48 && sizeChanges == 1,
            "size restoration must not emit edits");
    for (auto* picker : palette.findChildren<adqt::widgets::AdColorPicker*>()) {
        if (picker->objectName().startsWith(QStringLiteral("screenRecording"))) {
            require(picker->popupLayerMode() ==
                        adqt::widgets::AdColorPicker::PopupLayerMode::QtTool,
                    "recording color pickers must use Qt Tool windows");
        }
    }
    auto* background = form->findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenRecordingKeyboardBackgroundColor"));
    auto* foreground = form->findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenRecordingKeyboardForegroundColor"));
    require(duration && background && foreground &&
                keyboardSize->suffixText() == QStringLiteral("px") && duration->minimum() == 100 &&
                duration->maximum() == 2000 && duration->singleStep() == 100 &&
                duration->value() == 500 && !duration->suffixText().isEmpty(),
            "recording controls must show uniform units and configured bounds, step and default");
    const QString snapshotPath = qEnvironmentVariable("SNOW_RECORDING_SETTINGS_SNAPSHOT");
    if (!snapshotPath.isEmpty()) {
        require(form->window()->grab().save(snapshotPath), "settings snapshot must save");
    }
    const QRect first = form->field(QStringLiteral("duration"))->geometry();
    const QRect left = form->field(QStringLiteral("background"))->geometry();
    const QRect right = form->field(QStringLiteral("foreground"))->geometry();
    require(first.width() == form->width() && left.top() > first.top() &&
                left.top() == right.top() && left.width() == right.width() &&
                right.left() - left.right() - 1 == 16 && left.width() == 218 &&
                form->width() == 452 && duration->width() == 218,
            "settings must use the selection editor's field widths, single-column number input and "
            "16px gutter");
    for (const auto& key : {QStringLiteral("background"), QStringLiteral("foreground")}) {
        auto* field = form->field(key);
        auto* labelHost = field->findChild<QWidget*>(QStringLiteral("ad-form-item-label-host"));
        auto* controlHost = field->findChild<QWidget*>(QStringLiteral("ad-form-item-control"));
        require(labelHost && controlHost && labelHost->height() == 30 && controlHost->y() == 30 &&
                    controlHost->height() == 32 && field->height() == 86,
                "keyboard colors must use the selection editor's 30px label row and 32px controls "
                "without extra vertical space");
    }
    require(background->width() == 154 && foreground->width() == 154 &&
                background->triggerTextVisible() && foreground->triggerTextVisible() &&
                background->size() == adqt::widgets::AdColorPicker::Size::Middle &&
                foreground->size() == adqt::widgets::AdColorPicker::Size::Middle,
            "color fields must use the selection editor's full-size swatch and value triggers");
    require(background->value().solidColor == QColor(0, 0, 0, 204) &&
                foreground->value().solidColor == QColor(Qt::white),
            "keyboard defaults must preserve alpha");
    auto* highlightPicker = form->findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenRecordingMouseHighlightColor"));
    int highlightChanges = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingMouseHighlightColorChanged,
                     &palette, [&](const QColor&) { ++highlightChanges; });
    highlightPicker->commitValue(adqt::widgets::AdColorValue::solid(QColor(100, 150, 200, 80)));
    require(highlightChanges == 1 &&
                palette.recordingMouseHighlightColor() == QColor(100, 150, 200, 80),
            "highlight color edits preserve opacity");
    palette.setRecordingMouseHighlightColor(QColor(255, 255, 0, 128));
    require(highlightChanges == 1 &&
                highlightPicker->value().solidColor == QColor(255, 255, 0, 128),
            "highlight synchronization emits no user edit");
    int changes = 0;
    int lastDuration = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingMouseTrailDurationMsChanged,
                     &palette, [&](int value) {
                         lastDuration = value;
                         ++changes;
                     });
    int backgroundChanges = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingKeyboardBackgroundColorChanged,
                     &palette, [&](const QColor&) { ++backgroundChanges; });
    int foregroundChanges = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingKeyboardForegroundColorChanged,
                     &palette, [&](const QColor&) { ++foregroundChanges; });
    duration->setValue(1200);
    require(changes == 1 && lastDuration == 1200 && palette.recordingMouseTrailDurationMs() == 1200,
            "duration edits apply immediately");
    background->setPopupVisible(true);
    QCoreApplication::processEvents();
    require(background->popupVisible() && modal->isOpen(),
            "nested color picker must keep settings open");
    background->commitValue(adqt::widgets::AdColorValue::solid(QColor(40, 80, 120, 128)));
    require(palette.recordingKeyboardBackgroundColor() == QColor(40, 80, 120, 128) &&
                modal->isOpen(),
            "color edits must preserve alpha without dismissing settings");
    background->setPopupVisible(false);
    QCoreApplication::processEvents();
    form->window()->activateWindow();
    duration->setFocus();
    QCoreApplication::processEvents();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(QApplication::focusWidget(), &escape);
    QCoreApplication::processEvents();
    // Escape dismisses the dialog, which destroys it; the local pointers are
    // stale from here on.
    requireSettingsAbsent("Escape must destroy settings");

    const int changesBeforeRebuild = changes;
    const int sizeChangesBeforeRebuild = sizeChanges;
    const int backgroundChangesBeforeRebuild = backgroundChanges;
    const int foregroundChangesBeforeRebuild = foregroundChanges;
    const Settings reopened = openSettings();
    require(reopened.duration->value() == 1200 && reopened.keyboardSize->value() == 48 &&
                reopened.background->value().solidColor == QColor(40, 80, 120, 128) &&
                reopened.foreground->value().solidColor == QColor(Qt::white),
            "reopening settings must re-seed every value from the palette state");
    require(changes == changesBeforeRebuild && sizeChanges == sizeChangesBeforeRebuild &&
                backgroundChanges == backgroundChangesBeforeRebuild &&
                foregroundChanges == foregroundChangesBeforeRebuild,
            "rebuilding settings must not emit effect change signals");
    require(!reopened.form->disabled() &&
                reopened.modal->windowTitle() == QStringLiteral("Settings") &&
                !reopened.duration->suffixText().isEmpty() &&
                reopened.keyboardSize->suffixText() == QStringLiteral("px"),
            "a rebuilt form must be enabled and retranslated before it is shown");
    require(palette.findChildren<adqt::widgets::AdColorPicker*>().size() == 2,
            "dialog content must stay outside the palette widget tree");
    for (auto* picker : reopened.form->findChildren<adqt::widgets::AdColorPicker*>()) {
        require(picker->popupLayerMode() == adqt::widgets::AdColorPicker::PopupLayerMode::QtTool,
                "keyboard color pickers must use Qt Tool windows");
    }

    click(keyboard);
    require(reopened.modal->isOpen(), "outside clicks must not dismiss the modal");
    reopened.modal->accept();
    QCoreApplication::processEvents();
    requireSettingsAbsent("OK must destroy settings");
    require(palette.recordingMouseTrailDurationMs() == 1200, "OK retains applied settings");

    const Settings idle = openSettings();
    require(!idle.form->disabled(), "settings must be editable while recording is idle");
    palette.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
    requireSettingsAbsent("recording must close and destroy settings");
    require(!button->isEnabled(), "recording must lock the settings button");
    click(button);
    requireSettingsAbsent("a disabled settings button must not build the dialog");
    palette.setRecordingState(ScreenshotToolPalette::RecordingState::Idle);

    static_cast<void>(openSettings());
    require(palette.activateDrawingShortcut(QStringLiteral("shape")),
            "drawing must hide export settings");
    QCoreApplication::processEvents();
    requireSettingsAbsent("hiding the export toolbar destroys settings");
}

void recordingExportSettingsAndDrawingAvailabilityFollowSessionState() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showShapeTool = true;
    options.showHighlightTool = true;
    options.showPenHighlightTool = true;
    options.showSpotlightTool = true;
    options.showEraserTool = true;
    options.showFilterTool = true;
    options.showRecordingControls = true;
    options.recordingDrawingMode = true;
    options.enableStyleToolbar = true;
    ScreenshotToolPalette palette(options);
    palette.show();
    palette.prepareForDisplay();
    QCoreApplication::processEvents();

    auto* exportButton = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenRecordingExportSettings"));
    auto* exportPanel =
        palette.findChild<QWidget*>(QStringLiteral("screenRecordingExportSettingsPanel"));
    auto* format =
        palette.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenRecordingOutputFormat"));
    auto* trail = palette.findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenRecordingMouseTrailColor"));
    auto* click = palette.findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenRecordingMouseClickColor"));
    auto* cursor =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingShowCursor"));
    auto* keyboard =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingShowKeyboard"));
    require(keyboard != nullptr && !palette.recordingKeyboardVisible() && keyboard->isEnabled(),
            "keyboard recording should be available and initially off");
    require(exportPanel->layout()->indexOf(keyboard) == exportPanel->layout()->indexOf(cursor) + 1,
            "keyboard recording should immediately follow cursor recording");
    require(keyboard->accessibleName() == QStringLiteral("Show keystrokes in recording") &&
                adqt::icons::describeIcon(keyboard->iconRef()).key.name ==
                    QStringLiteral("recording-keyboard"),
            "keyboard toggle should expose its translated name and dedicated supplied icon");
    int keyboardChanges = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingKeyboardVisibleChanged, &palette,
                     [&keyboardChanges](bool) { ++keyboardChanges; });
    keyboard->click();
    require(palette.recordingKeyboardVisible() && keyboardChanges == 1,
            "keyboard toggle should emit one change and become active");
    palette.setRecordingKeyboardVisible(false);
    require(!palette.recordingKeyboardVisible() && keyboardChanges == 1,
            "programmatic synchronization should not emit a user change");
    require(exportButton != nullptr && exportPanel != nullptr && format != nullptr &&
                trail != nullptr && click != nullptr && cursor != nullptr,
            "recording export settings controls should be created");
    require(format->variant() == adqt::widgets::AdSelect::Variant::Borderless,
            "the recording format select should use the borderless toolbar variant");

    const auto findDrawingButton = [&palette](const QString& itemId) {
        for (auto* button : palette.mainPanel()->findChildren<adqt::widgets::AdButton*>()) {
            if (button->property("screenshotToolbarItemId").toString() == itemId &&
                button->parentWidget() == palette.mainPanel()) {
                return button;
            }
        }
        return static_cast<adqt::widgets::AdButton*>(nullptr);
    };
    auto* shapeButton = findDrawingButton(QStringLiteral("shape"));
    auto* filterButton = findDrawingButton(QStringLiteral("filter"));
    require(shapeButton != nullptr && filterButton != nullptr,
            "recording drawing controls should expose their configured toolbar entries");

    QLayout* mainLayout = palette.mainPanel()->layout();
    const int exportButtonIndex = mainLayout->indexOf(exportButton);
    const int shapeButtonIndex = mainLayout->indexOf(shapeButton);
    bool separatorAfterExportSettings = false;
    for (int index = exportButtonIndex + 1; index < shapeButtonIndex; ++index) {
        if (qobject_cast<QFrame*>(mainLayout->itemAt(index)->widget()) != nullptr) {
            separatorAfterExportSettings = true;
            break;
        }
    }
    require(exportButtonIndex >= 0 && shapeButtonIndex > exportButtonIndex &&
                separatorAfterExportSettings,
            "the main toolbar should separate Export Settings and selection from drawing tools");

    const auto disabledIconColor =
        snow_shot::presentation::styles::generateThemeColorScheme().map.colorTextQuaternary;
    const std::optional<QColor> filterIconColor = filterButton->iconRef().colors().primarySlot();
    require(filterButton->isEnabled() && filterIconColor.has_value() &&
                filterIconColor->rgba() == disabledIconColor.rgba(),
            "unavailable recording tools should remain explorable and use the disabled icon color");
    require(exportButton->toolTip() == QStringLiteral("Export Settings") &&
                exportButton->accessibleName() == QStringLiteral("Export Settings") &&
                adqt::icons::describeIcon(exportButton->iconRef()).key.name ==
                    QStringLiteral("export-settings") &&
                format->accessibleName() == QStringLiteral("Recording format") &&
                trail->accessibleName() == QStringLiteral("Mouse trail color") &&
                click->accessibleName() == QStringLiteral("Mouse click color") &&
                cursor->accessibleName() == QStringLiteral("Show cursor in recording"),
            "recording export controls should expose translated accessibility text");
    require(exportPanel->isVisible() && palette.recordingExportSettingsVisible() &&
                !palette.activeToolForTests().has_value() &&
                exportButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                format->currentValue().toString() == QStringLiteral("mp4") &&
                trail->value().solidColor == QColor(0, 0, 0, 0) &&
                click->value().solidColor == QColor(0, 0, 0, 0) && palette.recordingCursorVisible(),
            "entering recording should activate export settings with the first-run defaults");

    require(trail->presets().isEmpty() && click->presets().isEmpty(),
            "mouse effect presets should be moved out of the picker popups");
    for (auto* picker : {trail, click}) {
        require(picker->trigger() == adqt::widgets::AdColorPicker::Trigger::Hover,
                "recording color pickers should expand on hover like drawing color pickers");
        require(picker->toolTip().isEmpty() && picker->triggerContent()->toolTip().isEmpty(),
                "recording color picker triggers should not display tooltips");
    }
    const auto verifyColorIconTooltips = [exportPanel, trail, click]() {
        auto* trailIcon =
            exportPanel->findChild<QWidget*>(QStringLiteral("screenRecordingMouseTrailIcon"));
        auto* clickIcon =
            exportPanel->findChild<QWidget*>(QStringLiteral("screenRecordingMouseClickIcon"));
        require(trailIcon != nullptr && clickIcon != nullptr &&
                    trailIcon->toolTip() == trail->accessibleName() &&
                    clickIcon->toolTip() == click->accessibleName(),
                "mouse effect descriptions should appear on the icons left of the pickers");
    };
    verifyColorIconTooltips();
    const auto findPresets = [exportPanel](adqt::widgets::AdColorPicker* picker) {
        QVector<adqt::widgets::AdButton*> presets;
        auto colors = snow_shot::presentation::style_presets::strokeColors().first(4);
        colors.prepend(QColor(0, 0, 0, 0));
        for (int index = 0; index < colors.size(); ++index) {
            auto* button = exportPanel->findChild<adqt::widgets::AdButton*>(
                picker->objectName() + QStringLiteral("Preset%1").arg(index),
                Qt::FindDirectChildrenOnly);
            require(button != nullptr && dynamic_cast<ColorSwatchButton*>(button) != nullptr,
                    "each mouse effect should expose five drawing-style toolbar swatches");
            const QString tooltip =
                picker->accessibleName() + QLatin1Char(' ') +
                (index == 0 ? QStringLiteral("transparent") : colors.at(index).name());
            require(button->toolTip() == tooltip && button->accessibleName() == tooltip,
                    "preset tooltips should use the drawing toolbar's setting and color format");
            presets.push_back(button);
        }
        return presets;
    };
    const auto trailPresets = findPresets(trail);
    const auto clickPresets = findPresets(click);
    const auto verifyTriggerColor = [](adqt::widgets::AdColorPicker* picker, qreal scale = 1.0) {
        auto* trigger = dynamic_cast<ColorSwatchButton*>(picker->triggerContent());
        require(trigger != nullptr,
                "export color pickers should use the drawing toolbar's custom swatch trigger");
        require(trigger->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Outline &&
                    trigger->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral &&
                    trigger->sizeClass() == adqt::widgets::AdButton::SizeClass::Small,
                "export triggers should show the current RGBA color with drawing summary styling");
        std::unique_ptr<ColorSwatchButton> reference(createScreenshotToolPaletteColorButton(
            trigger->parentWidget(), nullptr, picker->value().solidColor, true, true,
            {28, 18, scale}));
        // Reference metrics are configured by the factory; content scaling is
        // now applied by the same context used for the live toolbar control.
        reference->commitControlScale(adqt::widgets::controlScaleContextFor(trigger));
        require(renderButton(*trigger) == renderButton(*reference),
                "export triggers should render RGBA colors exactly like drawing summary swatches");
    };
    verifyTriggerColor(trail);
    verifyTriggerColor(click);
    const auto verifyActivePreset = [](const QVector<adqt::widgets::AdButton*>& presets,
                                       int activeIndex) {
        for (int index = 0; index < presets.size(); ++index) {
            require(presets.at(index)->buttonStyle() ==
                        (index == activeIndex ? adqt::widgets::AdButton::ButtonStyle::Tonal
                                              : adqt::widgets::AdButton::ButtonStyle::Text),
                    "only the preset matching the complete RGBA value should be highlighted");
            require(presets.at(index)->accentRole() ==
                        (index == activeIndex ? adqt::widgets::AdButton::AccentRole::Primary
                                              : adqt::widgets::AdButton::AccentRole::Neutral),
                    "export presets should use the drawing toolbar's selected and idle accents");
        }
    };
    verifyActivePreset(trailPresets, 0);
    verifyActivePreset(clickPresets, 0);

    const QRect mainGeometry = palette.mainPanel()->geometry();
    int exportVisibilityChanges = 0;
    bool exportVisible = palette.recordingExportSettingsVisible();
    QObject::connect(&palette, &ScreenshotToolPalette::recordingExportSettingsVisibleChanged,
                     &palette, [&](bool visible) {
                         ++exportVisibilityChanges;
                         exportVisible = visible;
                         require(visible == palette.recordingExportSettingsVisible(),
                                 "export visibility signal should expose the updated state");
                     });
    exportButton->click();
    QCoreApplication::processEvents();
    require(exportPanel->isVisible() && palette.recordingExportSettingsVisible() &&
                !palette.activeToolForTests().has_value() && exportVisible &&
                exportVisibilityChanges == 0 &&
                exportButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
            "clicking active Export Settings must be idempotent");
    exportButton->click();
    QCoreApplication::processEvents();
    require(exportPanel->isVisible() && palette.mainPanel()->geometry() == mainGeometry,
            "opening export settings should not move or resize the main toolbar row");
    auto* formatSeparator = exportPanel->findChild<QFrame*>(
        QStringLiteral("screenRecordingExportFormatSeparator"), Qt::FindDirectChildrenOnly);
    auto* trailSeparator = exportPanel->findChild<QFrame*>(
        QStringLiteral("screenRecordingExportTrailSeparator"), Qt::FindDirectChildrenOnly);
    auto* clickSeparator = exportPanel->findChild<QFrame*>(
        QStringLiteral("screenRecordingExportClickSeparator"), Qt::FindDirectChildrenOnly);
    auto* trailIcon = exportPanel->findChild<QLabel*>(
        QStringLiteral("screenRecordingMouseTrailIcon"), Qt::FindDirectChildrenOnly);
    auto* clickIcon = exportPanel->findChild<QLabel*>(
        QStringLiteral("screenRecordingMouseClickIcon"), Qt::FindDirectChildrenOnly);
    QLayout* exportLayout = exportPanel->layout();
    require(
        formatSeparator != nullptr && trailSeparator != nullptr && clickSeparator != nullptr &&
            trailIcon != nullptr && clickIcon != nullptr && exportLayout != nullptr &&
            exportLayout->spacing() == 4 && format->width() == 76 && trailIcon->width() == 28 &&
            clickIcon->width() == 28 && trail->width() == 28 && click->width() == 28 &&
            exportLayout->indexOf(format) < exportLayout->indexOf(formatSeparator) &&
            exportLayout->indexOf(formatSeparator) < exportLayout->indexOf(trailIcon) &&
            exportLayout->indexOf(trail) < exportLayout->indexOf(trailSeparator) &&
            exportLayout->indexOf(trailSeparator) < exportLayout->indexOf(clickIcon) &&
            exportLayout->indexOf(click) < exportLayout->indexOf(clickSeparator) &&
            exportLayout->indexOf(clickSeparator) < exportLayout->indexOf(cursor),
        "export settings should use compact, separated format, trail, click, and cursor groups");

    for (const qreal scale : {1.0, 0.75, 1.25, 1.5, 2.0, 1.0}) {
        palette.setPhysicalScale(scale);
        palette.prepareForDisplay();
        QCoreApplication::processEvents();
        const int controlSize = qRound(28 * scale);
        const int iconSize = qRound(18 * scale);
        for (QLabel* icon : {trailIcon, clickIcon}) {
            require(icon->size() == QSize(controlSize, controlSize) &&
                        icon->pixmap().deviceIndependentSize() == QSizeF(iconSize, iconSize),
                    "mouse effect icons should match drawing style controls at every scale");
        }
        for (auto* picker : {trail, click}) {
            auto* trigger = dynamic_cast<ColorSwatchButton*>(picker->triggerContent());
            require(picker->QWidget::size() == QSize(controlSize, controlSize) &&
                        trigger->size() == QSize(controlSize, controlSize) &&
                        trigger->iconSize() == QSize(iconSize, iconSize),
                    "export picker triggers should retain drawing swatch metrics at every scale");
            verifyTriggerColor(picker, scale);
        }
        require(format->QWidget::size() == QSize(qRound(76 * scale), controlSize) &&
                    cursor->size() == QSize(controlSize, controlSize) &&
                    cursor->iconSize() == QSize(iconSize, iconSize),
                "format and cursor controls should match drawing style sizing at every scale");
        const QMargins margins = exportLayout->contentsMargins();
        require(exportPanel->height() == controlSize + margins.top() + margins.bottom(),
                "preset buttons must preserve the export toolbar's single-row height");
        for (const auto* presets : {&trailPresets, &clickPresets}) {
            for (auto* button : *presets) {
                require(button->size() == QSize(controlSize, controlSize) && button->isVisible(),
                        "preset buttons should match the export toolbar height at every scale");
            }
        }
        QList<QWidget*> controls = {format, formatSeparator, trailIcon, trail};
        for (auto* button : trailPresets) {
            controls.push_back(button);
        }
        controls.append({trailSeparator, clickIcon, click});
        for (auto* button : clickPresets) {
            controls.push_back(button);
        }
        controls.append({clickSeparator, cursor});
        for (int index = 0; index < controls.size(); ++index) {
            require(exportPanel->rect().contains(controls[index]->geometry()),
                    "export settings controls should fit inside the toolbar");
            if (index > 0) {
                require(controls[index - 1]->geometry().right() <
                            controls[index]->geometry().left(),
                        "export settings controls should retain group order without overlap");
            }
        }
        const int exportHeight = exportPanel->height();
        const QSize exportSize = exportPanel->size();
        const int exportItemCount = exportLayout->count();
        for (int iteration = 0; iteration < 8; ++iteration) {
            shapeButton->click();
            palette.prepareForDisplay();
            QCoreApplication::processEvents();
            exportButton->click();
            palette.prepareForDisplay();
            QCoreApplication::processEvents();
            if (exportPanel->size() != exportSize || exportLayout->count() != exportItemCount) {
                std::cerr << "export settings at scale " << scale << ", iteration " << iteration
                          << ": width " << exportSize.width() << " -> " << exportPanel->width()
                          << ", layout items " << exportItemCount << " -> " << exportLayout->count()
                          << '\n';
            }
            require(exportPanel->isVisible() && exportPanel->size() == exportSize &&
                        exportLayout->count() == exportItemCount,
                    "repeated tool switches must preserve export settings size and layout items");
        }
        shapeButton->click();
        palette.prepareForDisplay();
        QCoreApplication::processEvents();
        require(
            palette.styleToolbarVisible() && palette.stylePanel()->height() == exportHeight,
            "export settings and drawing sub-toolbars should have equal heights at every scale");
        const auto separatorGaps = [](QWidget* separator) {
            QLayout* row = separator->parentWidget()->layout();
            const int separatorIndex = row->indexOf(separator);
            QWidget* before = nullptr;
            QWidget* after = nullptr;
            for (int index = separatorIndex - 1; index >= 0 && before == nullptr; --index) {
                before = row->itemAt(index)->widget();
            }
            for (int index = separatorIndex + 1; index < row->count() && after == nullptr;
                 ++index) {
                after = row->itemAt(index)->widget();
            }
            require(before != nullptr && after != nullptr,
                    "group separators should have controls on both sides");
            return QSize(separator->x() - before->geometry().right() - 1,
                         after->x() - separator->geometry().right() - 1);
        };
        auto* shapeSeparator =
            palette.findChild<QFrame*>(QStringLiteral("screenshotShapeStyleGroupSeparator"));
        require(shapeSeparator != nullptr, "Shape should expose its standard group separator");
        const QSize expectedGaps = separatorGaps(shapeSeparator);
        exportButton->click();
        palette.prepareForDisplay();
        QCoreApplication::processEvents();
        for (QFrame* separator : {formatSeparator, trailSeparator, clickSeparator}) {
            const QSize actualGaps = separatorGaps(separator);
            if (qAbs(actualGaps.width() - expectedGaps.width()) > 1 ||
                qAbs(actualGaps.height() - expectedGaps.height()) > 1) {
                std::cerr << "separator gaps at scale " << scale << ": export "
                          << actualGaps.width() << '/' << actualGaps.height() << ", drawing "
                          << expectedGaps.width() << '/' << expectedGaps.height() << '\n';
            }
            require(qAbs(actualGaps.width() - expectedGaps.width()) <= 1 &&
                        qAbs(actualGaps.height() - expectedGaps.height()) <= 1,
                    "export separators should match drawing group gaps on both sides, allowing "
                    "one pixel for cumulative scaling rounding");
        }
        require(exportPanel->isVisible() && exportPanel->height() == exportHeight,
                "switching back to export settings should preserve the shared sub-toolbar height");
    }

    int selectRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::selectRequested, &palette,
                     [&]() { ++selectRequests; });
    exportButton->click();
    require(exportPanel->isVisible() && !palette.activeToolForTests().has_value(),
            "clicking active Export Settings must retain its selection");
    shapeButton->click();
    require(!exportPanel->isVisible() &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape,
            "selecting a drawing tool should deselect and close Export Settings");
    require(!exportVisible && exportVisibilityChanges > 0,
            "selecting drawing should notify the controller to stop moving the region");
    exportButton->click();
    require(exportPanel->isVisible() && !palette.activeToolForTests().has_value() &&
                selectRequests == 1,
            "selecting Export Settings should clear drawing selection");
    require(exportVisible && exportVisibilityChanges > 1,
            "activating export settings should notify the controller to enable region movement");
    filterButton->click();
    require(exportPanel->isVisible() && !palette.activeToolForTests().has_value() &&
                selectRequests == 1,
            "selecting an unavailable recording tool should leave the shared state unchanged");
    shapeButton->click();
    shapeButton->click();
    require(exportPanel->isVisible() && !palette.activeToolForTests().has_value() &&
                !palette.styleToolbarVisible() &&
                shapeButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                selectRequests == 2,
            "clicking the active drawing button must switch to Export Settings");
    exportButton->click();

    int formatChanges = 0;
    int trailChanges = 0;
    int clickChanges = 0;
    int cursorChanges = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::recordingOutputFormatChanged, &palette,
                     [&](const QString&) { ++formatChanges; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingMouseTrailColorChanged, &palette,
                     [&](const QColor&) { ++trailChanges; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingMouseClickColorChanged, &palette,
                     [&](const QColor&) { ++clickChanges; });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingCursorVisibleChanged, &palette,
                     [&](bool) { ++cursorChanges; });
    format->setCurrentValue(QStringLiteral("webp"));
    trail->commitValue(adqt::widgets::AdColorValue::solid(QColor(12, 34, 56, 78)));
    click->commitValue(adqt::widgets::AdColorValue::solid(QColor(90, 80, 70, 60)));
    cursor->click();
    require(palette.recordingOutputFormat() == QStringLiteral("webp") &&
                palette.recordingMouseTrailColor() == QColor(12, 34, 56, 78) &&
                palette.recordingMouseClickColor() == QColor(90, 80, 70, 60) &&
                !palette.recordingCursorVisible() && formatChanges == 1 && trailChanges == 1 &&
                clickChanges == 1 && cursorChanges == 1,
            "render controls should accept arbitrary RGBA values and publish changes once");

    verifyActivePreset(trailPresets, -1);
    verifyActivePreset(clickPresets, -1);
    verifyTriggerColor(trail);
    verifyTriggerColor(click);
    const QVector<QColor> opaqueColors = {
        QColor(0, 0, 0, 0), QColor(QStringLiteral("#f5222d")), QColor(QStringLiteral("#52c41a")),
        QColor(QStringLiteral("#1677ff")), QColor(QStringLiteral("#fadb14"))};
    for (int index = 0; index < opaqueColors.size(); ++index) {
        const QColor trailColor = opaqueColors.at(index);
        QColor clickColor = trailColor;
        if (index != 0) {
            clickColor.setAlpha(128);
        }
        const int previousTrailChanges = trailChanges;
        const int previousClickChanges = clickChanges;
        trailPresets.at(index)->click();
        require(palette.recordingMouseTrailColor() == trailColor &&
                    trail->value().solidColor == trailColor &&
                    trailChanges == previousTrailChanges + 1 &&
                    clickChanges == previousClickChanges && !trail->popupVisible(),
                "trail presets should commit opaque RGBY or transparent without opening a popup");
        verifyActivePreset(trailPresets, index);
        clickPresets.at(index)->click();
        require(palette.recordingMouseClickColor() == clickColor &&
                    click->value().solidColor == clickColor &&
                    clickChanges == previousClickChanges + 1 &&
                    trailChanges == previousTrailChanges + 1 && !click->popupVisible(),
                "click presets should commit RGBY with alpha 128 or transparent independently");
        verifyActivePreset(clickPresets, index);
        verifyTriggerColor(trail);
        verifyTriggerColor(click);
        trailPresets.at(index)->click();
        clickPresets.at(index)->click();
        require(trailChanges == previousTrailChanges + 1 &&
                    clickChanges == previousClickChanges + 1,
                "selecting an unchanged preset should not publish duplicate changes");
    }
    const int previousTrailChanges = trailChanges;
    const int previousClickChanges = clickChanges;
    palette.setRecordingMouseTrailColor(QColor(0xf5, 0x22, 0x2d));
    palette.setRecordingMouseClickColor(QColor(0x52, 0xc4, 0x1a, 128));
    verifyActivePreset(trailPresets, 1);
    verifyActivePreset(clickPresets, 2);
    palette.setRecordingMouseTrailColor(QColor(0xf5, 0x22, 0x2d, 128));
    palette.setRecordingMouseClickColor(QColor(0x52, 0xc4, 0x1a));
    verifyActivePreset(trailPresets, -1);
    verifyActivePreset(clickPresets, -1);
    require(trailChanges == previousTrailChanges && clickChanges == previousClickChanges,
            "external color synchronization should refresh presets without publishing changes");
    verifyTriggerColor(trail);
    verifyTriggerColor(click);

    const auto verifyPresetsEditable = [&](bool editable) {
        for (const auto* presets : {&trailPresets, &clickPresets}) {
            for (auto* button : *presets) {
                require(button->isEnabled() == editable,
                        "preset availability should follow the recording and busy state");
                if (!editable) {
                    button->click();
                }
            }
        }
        require(trailChanges == previousTrailChanges && clickChanges == previousClickChanges,
                "locked presets must not change recording settings");
    };
    const auto verifyExportSettingsSelected = [&](bool editable) {
        require(exportButton->isEnabled() && exportPanel->isVisible() &&
                    palette.recordingExportSettingsVisible() &&
                    !palette.activeToolForTests().has_value() &&
                    exportButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
                "recording state changes should preserve the active Export Settings tool");
        require(exportPanel->isEnabled() == editable && format->disabled() == !editable &&
                    trail->disabled() == !editable && click->disabled() == !editable &&
                    cursor->isEnabled() == editable && keyboard->isEnabled() == editable,
                "only the export child toolbar should lock while recording or busy");
        if (!editable) {
            keyboard->click();
            require(keyboardChanges == 1 && !palette.recordingKeyboardVisible(),
                    "a locked keyboard toggle must not change its saved selection");
        }
        verifyPresetsEditable(editable);
    };
    const int visibilityChangesBeforeRecording = exportVisibilityChanges;
    palette.setRecordingSession(ScreenshotToolPalette::RecordingSessionStatus::starting());
    verifyExportSettingsSelected(false);
    palette.setRecordingSession(ScreenshotToolPalette::RecordingSessionStatus::idle());
    verifyExportSettingsSelected(true);

    adqt::widgets::AdButton* microphone = nullptr;
    adqt::widgets::AdButton* systemAudio = nullptr;
    for (auto* button : palette.mainPanel()->findChildren<adqt::widgets::AdButton*>()) {
        const QString source = button->property("snowShotTranslationTooltipSource").toString();
        if (source == QStringLiteral("Record microphone")) {
            microphone = button;
        } else if (source == QStringLiteral("Record speakers")) {
            systemAudio = button;
        }
    }
    require(microphone != nullptr && systemAudio != nullptr && !microphone->isEnabled() &&
                !systemAudio->isEnabled() &&
                microphone->toolTip() ==
                    QStringLiteral("Animated recording formats do not contain audio") &&
                systemAudio->toolTip() ==
                    QStringLiteral("Animated recording formats do not contain audio"),
            "animated formats should disable audio with an explanatory tooltip");
    palette.setRecordingMicrophoneEnabled(true);
    palette.setRecordingSystemAudioEnabled(false);
    format->setCurrentValue(QStringLiteral("mp4"));
    require(microphone->isEnabled() && systemAudio->isEnabled(),
            "returning to MP4 should restore audio control availability");
    format->setCurrentValue(QStringLiteral("gif"));
    format->setCurrentValue(QStringLiteral("mp4"));
    require(palette.recordingOutputFormat() == QStringLiteral("mp4"),
            "animated format changes should not alter stored audio preferences");

    palette.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
    QCoreApplication::processEvents();
    verifyExportSettingsSelected(false);
    require(exportVisible && exportVisibilityChanges == visibilityChangesBeforeRecording,
            "starting recording must preserve Export Settings without a visibility notification");
    palette.setRecordingState(ScreenshotToolPalette::RecordingState::Paused);
    verifyExportSettingsSelected(false);
    require(exportVisible && exportVisibilityChanges == visibilityChangesBeforeRecording,
            "pausing must preserve Export Settings without a visibility notification");
    for (const auto state : {ScreenshotToolPalette::RecordingState::Recording,
                             ScreenshotToolPalette::RecordingState::Paused}) {
        palette.setRecordingState(state);
        shapeButton->click();
        require(!exportPanel->isVisible() &&
                    palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape,
                "drawing tools should still replace Export Settings during recording");
        exportButton->click();
        verifyExportSettingsSelected(false);
        palette.setRecordingState(state);
        verifyExportSettingsSelected(false);
        const int visibilityChangesBeforeClick = exportVisibilityChanges;
        exportButton->click();
        require(exportPanel->isVisible() && palette.recordingExportSettingsVisible() &&
                    !palette.activeToolForTests().has_value() && exportVisible &&
                    exportVisibilityChanges == visibilityChangesBeforeClick,
                "clicking active Export Settings during recording must remain idempotent");
        shapeButton->click();
        const int selectRequestsBeforeClick = selectRequests;
        shapeButton->click();
        require(!palette.activeToolForTests().has_value() &&
                    palette.recordingExportSettingsVisible() && !palette.styleToolbarVisible() &&
                    selectRequests == selectRequestsBeforeClick + 1,
                "clicking active drawing during recording must return to Export Settings");
        shapeButton->click();
        require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape,
                "a deactivated drawing tool should activate on the next click");
        exportButton->click();
        verifyExportSettingsSelected(false);
    }
    const int visibilityChangesBeforeResume = exportVisibilityChanges;
    palette.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
    verifyExportSettingsSelected(false);
    require(exportVisible && exportVisibilityChanges == visibilityChangesBeforeResume,
            "resuming recording must preserve Export Settings without a visibility notification");
    palette.setRecordingState(ScreenshotToolPalette::RecordingState::Idle);
    verifyExportSettingsSelected(true);

    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape,
            "shape should activate during recording drawing mode");
    require(!palette.activateDrawingShortcut(QStringLiteral("highlight")) &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape &&
                !palette.activateDrawingShortcut(QStringLiteral("filter")) &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape,
            "unavailable recording tools should not activate through scoped shortcuts");
    require(palette.activateDrawingShortcut(QStringLiteral("eraser")) &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Eraser,
            "enabled recording tools should remain available through scoped shortcuts");

    auto* spotlight =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotSpotlightButton"));
    require(spotlight != nullptr, "spotlight should remain present while recording");
    spotlight->click();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Spotlight &&
                palette.stepSpotlightOpacity(-1),
            "spotlight should activate and retain adjustable opacity while recording");

    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&palette, &languageChange);
    require(exportButton->toolTip() == QStringLiteral("Export Settings") &&
                format->accessibleName() == QStringLiteral("Recording format") &&
                cursor->accessibleName() == QStringLiteral("Show cursor in recording") &&
                keyboard->accessibleName() == QStringLiteral("Show keystrokes in recording"),
            "recording export controls should retranslate after LanguageChange");
    require(palette.recordingEffectSettingsModalForTests() == nullptr,
            "retranslation should not materialize the effect settings dialog");
    require(findPresets(trail) == trailPresets && findPresets(click) == clickPresets,
            "retranslation should preserve the toolbar presets and keep their labels current");
    verifyColorIconTooltips();
    require(trail->toolTip().isEmpty() && click->toolTip().isEmpty(),
            "retranslation should keep color picker tooltips on their icons");
}

void dynamicToolbarLabelsUseEveryTranslationCatalog() {
    auto& language = snow_shot::presentation::LanguageManager::instance();
    require(language.setLanguage(QStringLiteral("en_US")),
            "English should be active before testing dynamic toolbar translations");

    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showSelectTool = true;
    options.showRecordingControls = true;
    options.recordingDrawingMode = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Arrow),
            "arrow controls should materialize for translation coverage");

    ScreenshotToolPalette::Options filterOptions;
    filterOptions.showFilterTool = true;
    ScreenshotToolPalette filterPalette(filterOptions);
    filterPalette.setActiveTool(ScreenshotToolPalette::Tool::PenFilter);
    auto* filterTypes = filterPalette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotPenFilterTypeSelect"));
    require(filterTypes != nullptr && filterTypes->model() != nullptr,
            "dynamic translation coverage should expose filter options");

    const QStringList objectNames{
        QStringLiteral("screenshotToolbarDragHandle"),
        QStringLiteral("screenRecordingShowKeyboard"),
    };
    QVector<QWidget*> controls;
    for (const QString& objectName : objectNames) {
        QWidget* control = palette.findChild<QWidget*>(objectName);
        require(control != nullptr, "dynamic toolbar control should be present");
        controls.push_back(control);
    }
    const char* sourceLabels[] = {
        "Send to back",   "Send backward",          "Bring forward",
        "Bring to front", "Copy selected elements", "Delete selected elements",
        "Straight arrow", "Curved arrow",           "Elbow arrow",
    };
    for (const char* source : sourceLabels) {
        QWidget* control = controlWithTooltip(palette, source);
        require(control != nullptr, "dynamic toolbar source label should be present");
        controls.push_back(control);
    }

    struct TranslationExpectation {
        QString language;
        QStringList labels;
        QString emboss;
    };
    const TranslationExpectation expectations[] = {
        {QStringLiteral("zh_CN"),
         {QStringLiteral("拖动工具栏"), QStringLiteral("在录制中显示按键"),
          QStringLiteral("置于底层"), QStringLiteral("下移一层"), QStringLiteral("上移一层"),
          QStringLiteral("置于顶层"), QStringLiteral("复制选中元素"),
          QStringLiteral("删除选中元素"), QStringLiteral("直线箭头"), QStringLiteral("曲线箭头"),
          QStringLiteral("折线箭头")},
         QStringLiteral("浮雕")},
        {QStringLiteral("zh_TW"),
         {QStringLiteral("拖曳工具列"), QStringLiteral("在錄製中顯示按鍵"),
          QStringLiteral("移至最下層"), QStringLiteral("下移一層"), QStringLiteral("上移一層"),
          QStringLiteral("移至最上層"), QStringLiteral("複製選取的元素"),
          QStringLiteral("刪除選取的元素"), QStringLiteral("直線箭頭"), QStringLiteral("曲線箭頭"),
          QStringLiteral("折線箭頭")},
         QStringLiteral("浮雕")},
    };
    for (const TranslationExpectation& expectation : expectations) {
        require(language.setLanguage(expectation.language),
                "dynamic toolbar language setup failed");
        QCoreApplication::processEvents();
        require(controls.size() == expectation.labels.size(),
                "dynamic toolbar translation fixture should stay aligned");
        for (int index = 0; index < controls.size(); ++index) {
            require(controls.at(index)->toolTip() == expectation.labels.at(index) &&
                        controls.at(index)->accessibleName() == expectation.labels.at(index),
                    "dynamic toolbar labels must use the active translation catalog");
        }
        const QModelIndex embossIndex = filterTypes->model()->index(5, 0);
        require(embossIndex.data(adqt::widgets::AdSelect::DefaultLabelRole).toString() ==
                    expectation.emboss,
                "the Emboss filter option must use the active annotation catalog");
    }

    require(language.setLanguage(QStringLiteral("en_US")),
            "English should be restorable after dynamic toolbar translations");
    QCoreApplication::processEvents();
}

void numericStrokeWidthPreviewUsesLineWithinPreviewBounds() {
    NumericValuePreviewButton button;
    button.resize(64, 32);
    button.setSuffix(QStringLiteral("px"));
    button.setStrokeWidthPreviewEnabled(true);

    const QColor previewColor(QStringLiteral("#1677ff"));
    button.setValue(2.0);
    require(longestHorizontalColorRun(renderButton(button), previewColor) >= 50,
            "small positive stroke widths should render as a horizontal line");

    button.setValue(0.0);
    require(longestHorizontalColorRun(renderButton(button), previewColor) < 50,
            "zero stroke width should retain its numeric preview");

    button.setValue(20.0);
    require(longestHorizontalColorRun(renderButton(button), previewColor) < 50,
            "stroke widths outside the preview height should retain their numeric preview");
}

void secondaryControlsMaterializeOnlyForTheRequestedFamily() {
    ScreenshotToolPalette::Options options;
    options.showOcrTool = true;
    options.showTableTool = true;
    options.showScrollingScreenshotTool = true;
    ScreenshotToolPalette palette(options);

    require(palette.actionFamilyStateForTests(ScreenshotToolPalette::ActionFamily::Selection) ==
                    ScreenshotToolPalette::MaterializationState::Uninitialized &&
                palette.styleFamilyStateForTests(ScreenshotToolPalette::Tool::Shape) ==
                    ScreenshotToolPalette::MaterializationState::Uninitialized,
            "secondary families should start uninitialized");
    require(palette.findChild<QWidget*>(QStringLiteral("screenshotSelectionOpacitySlider")) ==
                    nullptr &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls")) ==
                    nullptr &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotTableMergeButton")) ==
                    nullptr,
            "construction should retain only the empty secondary toolbar shell");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    require(palette.styleFamilyStateForTests(ScreenshotToolPalette::Tool::Shape) ==
                    ScreenshotToolPalette::MaterializationState::Ready &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls")) !=
                    nullptr,
            "activating Shape should materialize its shared drawing style family");
    SnowCanvasShapeStyle retainedShapeStyle = palette.rectangleStyle();
    retainedShapeStyle.strokeWidth = 17.0;
    palette.setRectangleStyle(retainedShapeStyle);
    require(
        palette.actionFamilyStateForTests(ScreenshotToolPalette::ActionFamily::TableRecognition) ==
                ScreenshotToolPalette::MaterializationState::Uninitialized &&
            palette.findChild<QWidget*>(QStringLiteral("screenshotTableMergeButton")) == nullptr,
        "unrelated table controls must remain deferred");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Table);
    require(
        palette.actionFamilyStateForTests(ScreenshotToolPalette::ActionFamily::TableRecognition) ==
                ScreenshotToolPalette::MaterializationState::Ready &&
            palette.findChild<QWidget*>(QStringLiteral("screenshotTableMergeButton")) != nullptr,
        "activating Table should materialize only its action family");
    require(palette.styleFamilyStateForTests(ScreenshotToolPalette::Tool::Shape) ==
                    ScreenshotToolPalette::MaterializationState::Uninitialized &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls")) ==
                    nullptr,
            "switching to Table should evict the previous drawing style family");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    require(palette.actionFamilyStateForTests(ScreenshotToolPalette::ActionFamily::Selection) ==
                    ScreenshotToolPalette::MaterializationState::Ready &&
                palette.actionFamilyStateForTests(
                    ScreenshotToolPalette::ActionFamily::TableRecognition) ==
                    ScreenshotToolPalette::MaterializationState::Uninitialized &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotSelectionOpacitySlider")) !=
                    nullptr &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotTableMergeButton")) ==
                    nullptr,
            "switching to Select should evict the previous recognition action family");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QPointer<QWidget> firstShapeControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
    require(firstShapeControls != nullptr &&
                palette.actionFamilyStateForTests(ScreenshotToolPalette::ActionFamily::Selection) ==
                    ScreenshotToolPalette::MaterializationState::Uninitialized &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotSelectionOpacitySlider")) ==
                    nullptr &&
                qFuzzyCompare(palette.rectangleStyle().strokeWidth + 1.0, 18.0),
            "switching back to Shape should rebuild only its style family");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Line);
    QWidget* lineControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotLineStyleControls"));
    require(firstShapeControls.isNull() && lineControls != nullptr &&
                lineControls->parentWidget() == palette.stylePanel() &&
                palette.styleFamilyStateForTests(ScreenshotToolPalette::Tool::Shape) ==
                    ScreenshotToolPalette::MaterializationState::Uninitialized &&
                palette.styleFamilyStateForTests(ScreenshotToolPalette::Tool::Line) ==
                    ScreenshotToolPalette::MaterializationState::Ready,
            "switching to Line should replace the row while retaining compatible editors");
}

void textAndHighlightStrokeWidthTriggersUseSharedPreviewButton() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::RectangleHighlight),
            "highlight style family should materialize on demand");
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Text),
            "text style family should materialize on demand");
    const QList<QString> triggerNames{
        QStringLiteral("Text stroke width"),
        QStringLiteral("Highlight stroke width"),
    };
    QList<adqt::widgets::AdColorPicker*> pickers;

    for (const QString& triggerName : triggerNames) {
        adqt::widgets::AdColorPicker* picker = nullptr;
        for (adqt::widgets::AdColorPicker* candidate :
             palette.findChildren<adqt::widgets::AdColorPicker*>()) {
            if (candidate != nullptr && candidate->accessibleName() == triggerName) {
                picker = candidate;
                break;
            }
        }
        require(picker != nullptr, "stroke-width picker should be present");
        require(dynamic_cast<ColorPickerTrigger*>(picker->triggerContent()) != nullptr,
                "stroke-width picker should use the common color picker trigger");
        pickers.append(picker);
    }

    for (adqt::widgets::AdColorPicker* picker : std::as_const(pickers)) {
        picker->setPopupVisible(true);
        picker->setPopupVisible(false);
    }

    const auto popupRow = [&pickers](const QString& objectName) {
        for (adqt::widgets::AdColorPicker* picker : std::as_const(pickers)) {
            if (picker != nullptr && picker->popupContent() != nullptr) {
                if (QWidget* row = picker->popupContent()->findChild<QWidget*>(objectName)) {
                    return row;
                }
            }
        }
        return static_cast<QWidget*>(nullptr);
    };
    auto* textWidths = popupRow(QStringLiteral("screenshotTextStrokeWidthPresets"));
    auto* textColors = popupRow(QStringLiteral("screenshotTextStrokeColorPresets"));
    auto* highlightWidths = popupRow(QStringLiteral("screenshotHighlightStrokeWidthPresets"));
    auto* highlightColors = popupRow(QStringLiteral("screenshotHighlightStrokeColorPresets"));
    for (QWidget* row : {textWidths, textColors, highlightWidths, highlightColors}) {
        require(row != nullptr && qobject_cast<QHBoxLayout*>(row->layout()) != nullptr,
                "configured width-color editors should preserve horizontal preset rows");
    }
    const auto hasTooltip = [&palette, &pickers](const QString& tooltip) {
        QList<QWidget*> controls = palette.findChildren<QWidget*>();
        for (adqt::widgets::AdColorPicker* picker : std::as_const(pickers)) {
            if (picker != nullptr && picker->popupContent() != nullptr) {
                controls.append(picker->popupContent()->findChildren<QWidget*>());
            }
        }
        return std::any_of(controls.cbegin(), controls.cend(), [&](const QWidget* control) {
            return control != nullptr && control->toolTip() == tooltip;
        });
    };
    require(hasTooltip(QStringLiteral("Text stroke width 2px")) &&
                hasTooltip(QStringLiteral("Highlight stroke width 2px")) &&
                hasTooltip(QStringLiteral("Text stroke color transparent")) &&
                hasTooltip(QStringLiteral("Highlight stroke color #f5222d")),
            "configured width-color editors should preserve tool-specific presets");

    palette.setPhysicalScale(1.5);
    require(pickers.size() == 2 && pickers.at(0)->size() == pickers.at(1)->size() &&
                pickers.at(0)->triggerContent()->size() == pickers.at(1)->triggerContent()->size(),
            "configured width-color editors should share picker and trigger metrics");
}

void shapeAndArrowStrokeEditorsShareThePresetCatalog() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Shape),
            "shape style family should materialize on demand");
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Arrow),
            "arrow style family should materialize on demand");

    const auto rowPresetColorNames = [&palette](const QString& rowObjectName,
                                                const QString& tooltipPrefix) {
        QStringList colorNames;
        QWidget* row = palette.findChild<QWidget*>(rowObjectName);
        require(row != nullptr, "style row should exist");
        for (adqt::widgets::AdButton* button : row->findChildren<adqt::widgets::AdButton*>()) {
            auto* swatch = dynamic_cast<ColorSwatchButton*>(button);
            if (swatch == nullptr || swatch->parentWidget() == nullptr ||
                !swatch->parentWidget()->property("screenshotStyleEditorRoot").toBool() ||
                swatch->parentWidget()->parentWidget() != row) {
                continue;
            }
            const QString tooltip = swatch->toolTip();
            require(tooltip.startsWith(tooltipPrefix),
                    "stroke preset tooltip should use the tool-specific pattern");
            colorNames.append(tooltip.mid(tooltipPrefix.length()));
        }
        return colorNames;
    };
    const QStringList shapeColors = rowPresetColorNames(
        QStringLiteral("screenshotRectangleStyleControls"), QStringLiteral("Stroke color "));
    const QStringList arrowColors = rowPresetColorNames(
        QStringLiteral("screenshotArrowStyleControls"), QStringLiteral("Arrow stroke color "));
    require(shapeColors.size() == 5 && arrowColors == shapeColors,
            "shape and arrow stroke editors should render the shared stroke color catalog");

    const auto strokeStyleButtonCount = [&palette](const QString& accessibleName) {
        for (adqt::widgets::AdColorPicker* picker :
             palette.findChildren<adqt::widgets::AdColorPicker*>()) {
            if (picker != nullptr && picker->accessibleName() == accessibleName) {
                picker->setPopupVisible(true);
                picker->setPopupVisible(false);
                require(picker->popupContent() != nullptr,
                        "stroke editor popup should materialize on explicit opening");
                int count = 0;
                for (adqt::widgets::AdButton* button :
                     picker->popupContent()->findChildren<adqt::widgets::AdButton*>()) {
                    if (dynamic_cast<StrokeStylePreviewButton*>(button) != nullptr) {
                        ++count;
                    }
                }
                return count;
            }
        }
        return 0;
    };
    require(strokeStyleButtonCount(QStringLiteral("Stroke color")) == 3 &&
                strokeStyleButtonCount(QStringLiteral("Arrow stroke color")) == 3,
            "both stroke editors should expose the shared solid/dashed/dotted styles");
}

void sizePresetEditorsShareTheSizeCatalog() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::PenHighlight),
            "pen highlight style family should materialize on demand");
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::PenFilter),
            "pen filter style family should materialize on demand");
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Text),
            "text style family should materialize on demand");

    const QStringList expectedLabels{QStringLiteral("S"), QStringLiteral("M"), QStringLiteral("L"),
                                     QStringLiteral("XL")};
    const QList<double> expectedValues{24.0, 30.0, 42.0, 54.0};
    const auto presetTooltips = [&palette](const QString& prefix) {
        QStringList matched;
        const QList<QWidget*> controls = palette.findChildren<QWidget*>();
        for (QWidget* control : controls) {
            if (control != nullptr && control->toolTip().startsWith(prefix)) {
                matched.append(control->toolTip());
            }
        }
        return matched;
    };

    for (const QString& prefix :
         {QStringLiteral("Pen highlight stroke width "), QStringLiteral("Pen filter stroke width "),
          QStringLiteral("Text font size ")}) {
        const QStringList tooltips = presetTooltips(prefix);
        require(tooltips.size() == 4, "size preset editors should render the S/M/L/XL quartet");
        for (int index = 0; index < 4; ++index) {
            const QString expected = prefix + expectedLabels.at(index) + QStringLiteral(" (") +
                                     QString::number(expectedValues.at(index)) +
                                     QStringLiteral("px)");
            require(tooltips.contains(expected),
                    "size preset editors should share the S/M/L/XL value catalog");
        }
    }

    for (double value : expectedValues) {
        require(palette.findChild<QWidget*>(
                    QStringLiteral("screenshotPenFilterStrokeWidth%1").arg(qRound(value))) !=
                    nullptr,
                "pen filter width presets should keep their value-stamped object names");
    }
}

QWidget* styleEditorRoot(QWidget* row, const char* role) {
    if (row == nullptr) {
        return nullptr;
    }
    const QList<QWidget*> descendants = row->findChildren<QWidget*>();
    const auto found =
        std::find_if(descendants.cbegin(), descendants.cend(), [role](QWidget* item) {
            return item != nullptr && item->property("screenshotStyleEditorRoot").toBool() &&
                   item->property("screenshotStyleEditorRole").toByteArray() == role;
        });
    return found == descendants.cend() ? nullptr : *found;
}

void styleToolSwitchesReconcileCompatibleEditorRoots() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QWidget* shapeRow =
        palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
    QWidget* strokeRoot = styleEditorRoot(shapeRow, "outline-stroke");
    QWidget* widthRoot = styleEditorRoot(shapeRow, "outline-width");
    QWidget* fillRoot = styleEditorRoot(shapeRow, "shape-fill");
    require(strokeRoot != nullptr && widthRoot != nullptr && fillRoot != nullptr,
            "shape should expose the reusable outline and fill editor roots");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
    QWidget* arrowRow = palette.findChild<QWidget*>(QStringLiteral("screenshotArrowStyleControls"));
    const auto shapeToArrow = palette.lastStyleReconcileStatsForTests();
    require(styleEditorRoot(arrowRow, "outline-stroke") == strokeRoot &&
                styleEditorRoot(arrowRow, "outline-width") == widthRoot,
            "Shape to Arrow should preserve both outline editor subtrees");
    require(shapeToArrow.retained == 2 && shapeToArrow.destroyed == 3 && shapeToArrow.created == 5,
            "Shape to Arrow should report the exact reconciliation counts");
    require(palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls")) ==
                nullptr,
            "Shape-only editors should be destroyed after switching to Arrow");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    shapeRow = palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
    const auto arrowToShape = palette.lastStyleReconcileStatsForTests();
    require(styleEditorRoot(shapeRow, "outline-stroke") == strokeRoot &&
                styleEditorRoot(shapeRow, "outline-width") == widthRoot,
            "Arrow to Shape should preserve both outline editor subtrees");
    require(arrowToShape.retained == 2 && arrowToShape.destroyed == 5 && arrowToShape.created == 3,
            "Arrow to Shape should report the exact reconciliation counts");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Line);
    QWidget* lineRow = palette.findChild<QWidget*>(QStringLiteral("screenshotLineStyleControls"));
    const auto shapeToLine = palette.lastStyleReconcileStatsForTests();
    require(styleEditorRoot(lineRow, "outline-stroke") == strokeRoot &&
                styleEditorRoot(lineRow, "outline-width") == widthRoot &&
                styleEditorRoot(lineRow, "shape-fill") != nullptr,
            "Shape to Line should preserve all three compatible editor subtrees");
    require(shapeToLine.retained == 3 && shapeToLine.destroyed == 2 && shapeToLine.created == 1,
            "Shape to Line should replace the two Shape-only editors with the Line type editor");

    QWidget* lineFillRoot = styleEditorRoot(lineRow, "shape-fill");
    palette.setActiveTool(ScreenshotToolPalette::Tool::FreeDraw);
    QWidget* freeDrawRow =
        palette.findChild<QWidget*>(QStringLiteral("screenshotFreeDrawStyleControls"));
    const auto lineToFreeDraw = palette.lastStyleReconcileStatsForTests();
    require(styleEditorRoot(freeDrawRow, "outline-stroke") == strokeRoot &&
                styleEditorRoot(freeDrawRow, "outline-width") == widthRoot &&
                styleEditorRoot(freeDrawRow, "shape-fill") == lineFillRoot,
            "Line to Free Draw should preserve all style editor subtrees");
    require(lineToFreeDraw.retained == 3 && lineToFreeDraw.destroyed == 1 &&
                lineToFreeDraw.created == 0,
            "Line to Free Draw should remove only the Line type editor");
}

void styleToolReuseMapPreservesEveryCompatibleRole() {
    using Tool = ScreenshotToolPalette::Tool;
    const auto rowName = [](Tool tool) {
        switch (tool) {
        case Tool::Shape:
            return QStringLiteral("screenshotRectangleStyleControls");
        case Tool::Line:
            return QStringLiteral("screenshotLineStyleControls");
        case Tool::FreeDraw:
            return QStringLiteral("screenshotFreeDrawStyleControls");
        case Tool::Arrow:
            return QStringLiteral("screenshotArrowStyleControls");
        case Tool::RectangleHighlight:
            return QStringLiteral("screenshotHighlightStyleControls");
        case Tool::PenHighlight:
            return QStringLiteral("screenshotPenHighlightStyleControls");
        case Tool::RectangleFilter:
            return QStringLiteral("screenshotFilterStyleControls");
        case Tool::PenFilter:
            return QStringLiteral("screenshotPenFilterStyleControls");
        case Tool::Spotlight:
            return QStringLiteral("screenshotSpotlightStyleControls");
        case Tool::Text:
            return QStringLiteral("screenshotTextStyleControls");
        case Tool::SerialNumber:
            return QStringLiteral("screenshotSerialNumberStyleControls");
        case Tool::Watermark:
            return QStringLiteral("screenshotWatermarkStyleControls");
        default:
            return QString();
        }
    };
    const auto verify = [&rowName](Tool source, Tool destination,
                                   const QList<QByteArray>& retainedRoles, int destroyed,
                                   int created) {
        ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
        palette.setActiveTool(source);
        QWidget* sourceRow = palette.findChild<QWidget*>(rowName(source));
        require(sourceRow != nullptr, "source style composition should materialize");
        QHash<QByteArray, QPointer<QWidget>> roots;
        for (const QByteArray& role : retainedRoles) {
            roots.insert(role, styleEditorRoot(sourceRow, role.constData()));
            require(!roots.value(role).isNull(), "source should expose every reusable role");
        }

        palette.setActiveTool(destination);
        QWidget* destinationRow = palette.findChild<QWidget*>(rowName(destination));
        require(destinationRow != nullptr, "destination style composition should materialize");
        for (const QByteArray& role : retainedRoles) {
            require(styleEditorRoot(destinationRow, role.constData()) == roots.value(role),
                    "the destination should retain the source editor root for every shared role");
        }
        const auto stats = palette.lastStyleReconcileStatsForTests();
        require(stats.retained == retainedRoles.size() && stats.destroyed == destroyed &&
                    stats.created == created,
                "the style reuse map should report exact retained/created/destroyed counts");
    };

    verify(Tool::RectangleHighlight, Tool::PenHighlight, {"highlight-mode", "highlight-color"}, 1,
           1);
    verify(Tool::RectangleFilter, Tool::PenFilter,
           {"filter-mode", "filter-type", "filter-intensity"}, 0, 1);
    verify(Tool::Text, Tool::SerialNumber, {"foreground-color", "text-font", "text-fill"}, 3, 2);
    verify(Tool::PenHighlight, Tool::PenFilter, {"brush-width"}, 2, 3);
    verify(Tool::Spotlight, Tool::Watermark, {"opacity"}, 1, 6);
    verify(Tool::Shape, Tool::Text, {"corner-radius"}, 4, 5);
    verify(Tool::Text, Tool::Watermark, {"foreground-color"}, 5, 6);
}

void retainedOutlineEditorsRebindStateLabelsAndCommands() {
    ScreenshotToolPalette::Options options;
    options.styleDefaults.rectangle.stroke = QColor(QStringLiteral("#f5222d"));
    options.styleDefaults.rectangle.strokeWidth = 2.0;
    options.styleDefaults.arrow.stroke = QColor(QStringLiteral("#1677ff"));
    options.styleDefaults.arrow.strokeWidth = 8.0;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QWidget* shapeRow =
        palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
    QWidget* strokeRoot = styleEditorRoot(shapeRow, "outline-stroke");
    auto* picker =
        strokeRoot != nullptr ? strokeRoot->findChild<adqt::widgets::AdColorPicker*>() : nullptr;
    require(picker != nullptr &&
                picker->value().solidColor == options.styleDefaults.rectangle.stroke,
            "the Shape outline editor should show the Shape state");

    int shapeCommands = 0;
    int arrowCommands = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&shapeCommands, &arrowCommands](const SnowCanvasShapeStyle&, quint32,
                                                      SnowCanvasShapeKind kind) {
                         if (kind == SnowCanvasShapeKind::Arrow) {
                             ++arrowCommands;
                         } else {
                             ++shapeCommands;
                         }
                     });

    palette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
    QWidget* arrowRow = palette.findChild<QWidget*>(QStringLiteral("screenshotArrowStyleControls"));
    require(styleEditorRoot(arrowRow, "outline-stroke") == strokeRoot &&
                picker->accessibleName() == QStringLiteral("Arrow stroke color") &&
                picker->value().solidColor == options.styleDefaults.arrow.stroke,
            "a retained outline editor should immediately rebind Arrow labels and state");
    ColorSwatchButton* arrowPreset = nullptr;
    for (adqt::widgets::AdButton* button : strokeRoot->findChildren<adqt::widgets::AdButton*>()) {
        auto* swatch = dynamic_cast<ColorSwatchButton*>(button);
        if (swatch != nullptr &&
            swatch->toolTip() == QStringLiteral("Arrow stroke color #52c41a")) {
            arrowPreset = swatch;
            break;
        }
    }
    require(arrowPreset != nullptr, "the retained editor should expose Arrow preset labels");
    arrowPreset->click();
    require(arrowCommands == 1 && shapeCommands == 0,
            "editing after rebind should emit only the destination command");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    require(picker->accessibleName() == QStringLiteral("Stroke color") &&
                picker->value().solidColor == options.styleDefaults.rectangle.stroke,
            "switching back should restore the independent Shape state and labels");
}

void prewarmedDestinationMergesSourceSharedAndDestinationOnlyEditors() {
    ScreenshotToolPalette::Options options;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);

    QWidget* serialRow =
        palette.findChild<QWidget*>(QStringLiteral("screenshotSerialNumberStyleControls"));
    QPointer<QWidget> sourceColor = styleEditorRoot(serialRow, "foreground-color");
    QPointer<QWidget> sourceFont = styleEditorRoot(serialRow, "text-font");
    QPointer<QWidget> sourceFill = styleEditorRoot(serialRow, "text-fill");
    require(!sourceColor.isNull() && !sourceFont.isNull() && !sourceFill.isNull(),
            "Serial Number should expose the three editors shared with Text");

    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Text),
            "the destination Text row should support explicit prewarming");
    QPointer<QWidget> prewarmedTextRow =
        palette.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls"));
    QPointer<QWidget> destinationAlignment = styleEditorRoot(prewarmedTextRow, "text-alignment");
    QPointer<QWidget> duplicateColor = styleEditorRoot(prewarmedTextRow, "foreground-color");
    require(!destinationAlignment.isNull() && !duplicateColor.isNull(),
            "the prewarmed destination should have both unique and duplicate shared editors");

    SnowCanvasStyleToolbarState selectedText;
    selectedText.source = SnowCanvasStyleToolbarSource::SelectedText;
    palette.setStyleToolbarState(selectedText);

    QWidget* textRow = palette.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls"));
    require(textRow != nullptr && prewarmedTextRow.isNull(),
            "reconciliation should destroy the prewarmed row and publish a fresh container");
    require(styleEditorRoot(textRow, "foreground-color") == sourceColor &&
                styleEditorRoot(textRow, "text-font") == sourceFont &&
                styleEditorRoot(textRow, "text-fill") == sourceFill,
            "a prewarmed switch should keep shared editor roots from the active source");
    require(styleEditorRoot(textRow, "text-alignment") == destinationAlignment,
            "a prewarmed switch should keep destination-only editor roots");
    require(duplicateColor.isNull(),
            "a prewarmed destination's duplicate shared editor should be destroyed");
}

void repeatedStyleReconciliationDoesNotAccumulateHiddenRows() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    QWidget* stylePanel = palette.stylePanel();
    require(stylePanel != nullptr, "the style panel should exist");

    for (int iteration = 0; iteration < 12; ++iteration) {
        const auto tool = iteration % 2 == 0 ? ScreenshotToolPalette::Tool::Shape
                                             : ScreenshotToolPalette::Tool::Arrow;
        palette.setActiveTool(tool);
        const QList<QWidget*> rows =
            stylePanel->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        require(rows.size() == 1 && !rows.constFirst()->isHidden(),
                "normal style switches should keep exactly one live visible row");
    }

    palette.clearActiveTool();
    require(stylePanel->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly).isEmpty(),
            "clearing the active tool should destroy every style row");
}

bool tooltipMatches(const QString& actual, const QString& expected) {
    return actual == expected ||
           (actual.startsWith(expected + QStringLiteral(" (")) && actual.endsWith(')'));
}

QWidget* controlWithTooltip(ScreenshotToolPalette& palette, const char* tooltip) {
    const QString expected = QString::fromUtf8(tooltip);
    const QList<QWidget*> controls = palette.findChildren<QWidget*>();
    for (QWidget* control : controls) {
        if (control != nullptr &&
            (tooltipMatches(control->toolTip(), expected) ||
             control->property("snowShotDrawingShortcutTooltipSource").toString() == expected)) {
            return control;
        }
    }
    return nullptr;
}

QWidget* controlWithAccessibleName(ScreenshotToolPalette& palette, const char* accessibleName) {
    const QString expected = QString::fromUtf8(accessibleName);
    for (QWidget* control : palette.findChildren<QWidget*>()) {
        if (control != nullptr && control->accessibleName() == expected) {
            return control;
        }
    }
    return nullptr;
}

QList<adqt::widgets::AdButton*> mainToolbarButtons(ScreenshotToolPalette& palette) {
    QList<adqt::widgets::AdButton*> buttons;
    if (palette.mainPanel() == nullptr || palette.mainPanel()->layout() == nullptr) {
        return buttons;
    }

    QLayout* layout = palette.mainPanel()->layout();
    for (int index = 0; index < layout->count(); ++index) {
        QWidget* widget = layout->itemAt(index)->widget();
        if (auto* button = qobject_cast<adqt::widgets::AdButton*>(widget)) {
            buttons.append(button);
        }
    }
    return buttons;
}

QList<adqt::widgets::AdButton*> mainDrawingToolbarButtons(ScreenshotToolPalette& palette) {
    QList<adqt::widgets::AdButton*> buttons;
    for (adqt::widgets::AdButton* button : mainToolbarButtons(palette)) {
        if (!button->property("screenshotToolbarPositionItems").toStringList().isEmpty()) {
            buttons.append(button);
        }
    }
    return buttons;
}

QList<adqt::widgets::AdButton*> mainActionToolbarButtons(ScreenshotToolPalette& palette) {
    const QStringList actionIds{
        QStringLiteral("barcode-recognition"),  QStringLiteral("table-recognition"),
        QStringLiteral("record-screen"),        QStringLiteral("pin-to-screen"),
        QStringLiteral("text-recognition"),     QStringLiteral("text-translation"),
        QStringLiteral("scrolling-screenshot"), QStringLiteral("quick-save"),
        QStringLiteral("save-as-file"),
    };
    QList<adqt::widgets::AdButton*> buttons;
    for (adqt::widgets::AdButton* button : mainToolbarButtons(palette)) {
        const QStringList items = button->property("screenshotToolbarPositionItems").toStringList();
        if (std::any_of(items.cbegin(), items.cend(), [&actionIds](const QString& itemId) {
                return actionIds.contains(itemId);
            })) {
            buttons.append(button);
        }
    }
    return buttons;
}

bool hasSeparatorBetween(QLayout* layout, QWidget* before, QWidget* after) {
    if (layout == nullptr || before == nullptr || after == nullptr) {
        return false;
    }
    const int beforeIndex = layout->indexOf(before);
    const int afterIndex = layout->indexOf(after);
    if (beforeIndex < 0 || afterIndex <= beforeIndex) {
        return false;
    }
    for (int index = beforeIndex + 1; index < afterIndex; ++index) {
        if (qobject_cast<QFrame*>(layout->itemAt(index)->widget()) != nullptr) {
            return true;
        }
    }
    return false;
}

adqt::widgets::AdColorPicker* colorPickerWithAccessibleName(ScreenshotToolPalette& palette,
                                                            const char* accessibleName) {
    const QString expected = QString::fromUtf8(accessibleName);
    for (adqt::widgets::AdColorPicker* picker :
         palette.findChildren<adqt::widgets::AdColorPicker*>()) {
        if (picker != nullptr && picker->accessibleName() == expected) {
            return picker;
        }
    }
    return nullptr;
}

QWidget* popupControlWithTooltip(ScreenshotToolPalette& palette, const char* tooltip) {
    const QString expected = QString::fromUtf8(tooltip);
    const auto matchingControl = [&expected](QWidget* content) -> QWidget* {
        if (content == nullptr) {
            return nullptr;
        }
        if (tooltipMatches(content->toolTip(), expected)) {
            return content;
        }
        for (QWidget* control : content->findChildren<QWidget*>()) {
            if (control != nullptr && tooltipMatches(control->toolTip(), expected)) {
                return control;
            }
        }
        return nullptr;
    };
    for (adqt::widgets::AdColorPicker* picker :
         palette.findChildren<adqt::widgets::AdColorPicker*>()) {
        if (QWidget* control = matchingControl(picker->popupContent())) {
            return control;
        }
    }
    for (adqt::widgets::AdPopover* popover : palette.findChildren<adqt::widgets::AdPopover*>()) {
        if (QWidget* control = matchingControl(popover->contentWidget())) {
            return control;
        }
    }
    return nullptr;
}

QWidget* styleControlWithTooltip(ScreenshotToolPalette& palette, const char* tooltip) {
    if (QWidget* control = controlWithTooltip(palette, tooltip)) {
        return control;
    }
    if (QWidget* control = popupControlWithTooltip(palette, tooltip)) {
        return control;
    }
    for (adqt::widgets::AdColorPicker* picker :
         palette.findChildren<adqt::widgets::AdColorPicker*>()) {
        if (picker != nullptr && picker->popupContent() == nullptr) {
            picker->setPopupVisible(true);
            picker->setPopupVisible(false);
        }
    }
    return popupControlWithTooltip(palette, tooltip);
}

int layoutWidgetIndex(QLayout* layout, QWidget* widget) {
    if (layout == nullptr || widget == nullptr) {
        return -1;
    }
    int widgetIndex = 0;
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        if (item == nullptr || item->widget() == nullptr) {
            continue;
        }
        if (item->widget() == widget) {
            return widgetIndex;
        }
        ++widgetIndex;
    }
    return -1;
}

bool hasOnlySpacingBetween(QLayout* layout, QWidget* before, QWidget* after,
                           int requiredSpacerWidth) {
    if (layout == nullptr || before == nullptr || after == nullptr) {
        return false;
    }
    const int beforeIndex = layout->indexOf(before);
    const int afterIndex = layout->indexOf(after);
    if (beforeIndex < 0 || afterIndex <= beforeIndex) {
        return false;
    }
    bool foundRequiredSpacer = false;
    for (int index = beforeIndex + 1; index < afterIndex; ++index) {
        QLayoutItem* item = layout->itemAt(index);
        if (item == nullptr || item->spacerItem() == nullptr) {
            return false;
        }
        foundRequiredSpacer =
            foundRequiredSpacer || item->sizeHint().width() == requiredSpacerWidth;
    }
    return foundRequiredSpacer;
}

void clickStyleControl(ScreenshotToolPalette& palette, const char* tooltip) {
    auto* button =
        qobject_cast<adqt::widgets::AdButton*>(styleControlWithTooltip(palette, tooltip));
    require(button != nullptr, "expected arrow style control is missing");
    button->click();
}

adqt::widgets::AdPopover* popoverForTrigger(QWidget* trigger) {
    if (trigger == nullptr) {
        return nullptr;
    }
    for (adqt::widgets::AdPopover* popover : trigger->findChildren<adqt::widgets::AdPopover*>()) {
        if (popover != nullptr && popover->sourceWidget() == trigger) {
            return popover;
        }
    }
    return nullptr;
}

void materializeLazyPopover(QWidget* trigger) {
    require(trigger != nullptr, "lazy popover trigger should exist");
    adqt::widgets::AdPopover* popover = popoverForTrigger(trigger);
    require(popover != nullptr, "lazy popover controller should exist");
    if (popover->isVisible() && popover->contentWidget() == nullptr) {
        popover->hide();
        QCoreApplication::processEvents();
    }
    popover->show();
    require(popover->contentWidget() != nullptr, "lazy popover content should materialize on show");
}

adqt::widgets::AdPopover* showPopoverForTrigger(QWidget* trigger) {
    adqt::widgets::AdPopover* popover = popoverForTrigger(trigger);
    require(popover != nullptr, "arrow control should have a popup layer");
    popover->show();
    QCoreApplication::processEvents();
    return popover;
}

void clickPopoverStyleControl(adqt::widgets::AdPopover* popover, const char* tooltip) {
    require(popover != nullptr, "arrow control should have a popup layer");
    QWidget* content = popover->contentWidget();
    require(content != nullptr, "arrow popup content should be present");

    const QString expected = QString::fromUtf8(tooltip);
    for (QWidget* control : content->findChildren<QWidget*>()) {
        if (control != nullptr && tooltipMatches(control->toolTip(), expected)) {
            auto* button = qobject_cast<adqt::widgets::AdButton*>(control);
            require(button != nullptr, "arrow popup option should be a button");
            button->click();
            return;
        }
    }
    require(false, "expected arrow popup option is missing");
}

adqt::widgets::AdButton* popoverButtonWithTooltip(adqt::widgets::AdPopover* popover,
                                                  const char* tooltip) {
    if (popover == nullptr || popover->contentWidget() == nullptr) {
        return nullptr;
    }

    const QString expected = QString::fromUtf8(tooltip);
    for (QWidget* control : popover->contentWidget()->findChildren<QWidget*>()) {
        if (control != nullptr && tooltipMatches(control->toolTip(), expected)) {
            return qobject_cast<adqt::widgets::AdButton*>(control);
        }
    }
    return nullptr;
}

void requireControlsEnabled(ScreenshotToolPalette& palette, const char* const* tooltips,
                            std::size_t count, bool enabled, const char* message) {
    for (std::size_t index = 0; index < count; ++index) {
        QWidget* control = controlWithTooltip(palette, tooltips[index]);
        if (control == nullptr) {
            control = popupControlWithTooltip(palette, tooltips[index]);
        }
        require(control != nullptr, "expected toolbar control is missing");
        if (control->isEnabled() != enabled) {
            std::cerr << message << ": " << tooltips[index] << '\n';
            std::exit(1);
        }
    }
}

void requireControlActive(ScreenshotToolPalette& palette, const char* tooltip,
                          const char* message) {
    auto* button =
        qobject_cast<adqt::widgets::AdButton*>(styleControlWithTooltip(palette, tooltip));
    require(button != nullptr, "expected style control is missing");
    require(button->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Tonal &&
                button->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
            message);
}

void scrollingScreenshotKeepsDrawingToolsAvailable() {
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showFilterTool = true;
    options.showEraserTool = true;
    options.showWatermarkTool = true;
    options.showOcrTool = true;
    options.showTableTool = true;
    options.showScrollingScreenshotTool = true;
    options.showScreenRecordButton = true;
    options.enableStyleToolbar = false;
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;

    ScreenshotToolPalette palette(options);
    int selectRequestCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::selectRequested,
                     [&selectRequestCount]() { ++selectRequestCount; });
    require(palette.activateDrawingShortcut(QStringLiteral("select")) &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select &&
                selectRequestCount == 1,
            "the Select drawing shortcut must activate the selection tool and request it");
    const char* const drawingTools[] = {
        "Select elements", "Shape",  "Arrow",  "Line",      "Pen", "Text",
        "Serial number",   "Filter", "Eraser", "Watermark",
    };
    const char* const enabledDuringScrolling[] = {
        "Edit selection", "Text recognition", "Table recognition", "Scrolling screenshot",
        "Record screen",  "Pin to screen",    "Cancel screenshot", "Copy to clipboard",
    };

    requireControlsEnabled(palette, drawingTools, std::size(drawingTools), true,
                           "restricted tools should start enabled");
    requireControlsEnabled(palette, enabledDuringScrolling, std::size(enabledDuringScrolling), true,
                           "available scrolling controls should start enabled");

    palette.setScrollingScreenshotMode(true);
    require(palette.scrollingScreenshotMode(), "palette should enter scrolling screenshot mode");
    requireControlsEnabled(palette, drawingTools, std::size(drawingTools), true,
                           "scrolling screenshot mode should keep drawing tools enabled");
    requireControlsEnabled(
        palette, enabledDuringScrolling, std::size(enabledDuringScrolling), true,
        "scrolling screenshot mode should keep navigation and result controls enabled");

    auto* shapeButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Shape"));
    require(shapeButton != nullptr, "shape tool should remain clickable during scrolling capture");
    shapeButton->click();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape,
            "clicking a drawing tool should switch directly from scrolling capture to that tool");

    palette.setScrollingScreenshotMode(false);
    require(!palette.scrollingScreenshotMode(), "palette should leave scrolling screenshot mode");
    requireControlsEnabled(palette, drawingTools, std::size(drawingTools), true,
                           "leaving scrolling screenshot mode should preserve drawing tools");
    requireControlsEnabled(palette, enabledDuringScrolling, std::size(enabledDuringScrolling), true,
                           "leaving scrolling screenshot mode should preserve available controls");
}

void screenshotToolbarUsesCanonicalOrderAndSectionSeparators() {
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showSpotlightTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showFilterTool = true;
    options.showEraserTool = true;
    options.showWatermarkTool = true;
    options.showOcrTool = true;
    options.showTableTool = true;
    options.showScreenRecordButton = true;
    options.showScrollingScreenshotTool = true;
    options.separatorAfterSelect = true;
    options.separatorBeforeShape = true;
    options.enableStyleToolbar = false;
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;

    ScreenshotToolPalette palette(options);
    const QList<adqt::widgets::AdButton*> buttons = mainToolbarButtons(palette);
    const QStringList expected{
        shortcutTooltip(QStringLiteral("Edit selection"),
                        {QStringLiteral("M"), QStringLiteral("Ctrl+E")}),
        shortcutTooltip(QStringLiteral("Select elements"), {QStringLiteral("V")}),
        shortcutTooltip(QStringLiteral("Shape"), {QStringLiteral("1")}),
        shortcutTooltip(QStringLiteral("Arrow"), {QStringLiteral("2")}),
        shortcutTooltip(QStringLiteral("Pen"), {QStringLiteral("3"), QStringLiteral("P")}),
        shortcutTooltip(QStringLiteral("Highlight"), {QStringLiteral("4"), QStringLiteral("H")}),
        shortcutTooltip(QStringLiteral("Text"), {QStringLiteral("5"), QStringLiteral("T")}),
        shortcutTooltip(QStringLiteral("Serial number"),
                        {QStringLiteral("6"), QStringLiteral("N")}),
        shortcutTooltip(QStringLiteral("Filter"), {QStringLiteral("7"), QStringLiteral("F")}),
        shortcutTooltip(QStringLiteral("Eraser"), {QStringLiteral("8"), QStringLiteral("E")}),
        shortcutTooltip(QStringLiteral("Watermark"), {QStringLiteral("9")}),
        shortcutTooltip(QStringLiteral("Table recognition"), {QStringLiteral("Ctrl+X")}),
        shortcutTooltip(QStringLiteral("Record screen"), {QStringLiteral("Ctrl+R")}),
        shortcutTooltip(QStringLiteral("Pin to screen"), {QStringLiteral("Ctrl+F")}),
        shortcutTooltip(QStringLiteral("Text recognition"), {QStringLiteral("Ctrl+D")}),
        shortcutTooltip(QStringLiteral("Scrolling screenshot"), {QStringLiteral("L")}),
        shortcutTooltip(QStringLiteral("Cancel screenshot"), {QStringLiteral("Esc")}),
        shortcutTooltip(QStringLiteral("Copy to clipboard"), {QStringLiteral("Ctrl+C")}),
    };
    require(buttons.size() == expected.size(),
            "canonical screenshot toolbar should expose one entry per main group");
    for (int index = 0; index < expected.size(); ++index) {
        const QString buttonLabel = buttons.at(index)->toolTip().isEmpty()
                                        ? buttons.at(index)->accessibleName()
                                        : buttons.at(index)->toolTip();
        require(buttonLabel == expected.at(index),
                "canonical screenshot toolbar order should match the product grouping");
    }

    QLayout* layout = palette.mainPanel()->layout();
    QList<int> separatorIndices;
    for (int index = 0; index < layout->count(); ++index) {
        if (qobject_cast<QFrame*>(layout->itemAt(index)->widget()) != nullptr) {
            separatorIndices.append(index);
        }
    }
    require(separatorIndices.size() == 3,
            "canonical toolbar should contain only the three section separators");

    const auto topLevelIndex = [layout](QWidget* widget) {
        if (widget == nullptr) {
            return -1;
        }
        if (const int directIndex = layout->indexOf(widget); directIndex >= 0) {
            return directIndex;
        }
        return widget->parentWidget() != nullptr ? layout->indexOf(widget->parentWidget()) : -1;
    };
    const auto hasSeparatorBetween = [&separatorIndices, &topLevelIndex](QWidget* first,
                                                                         QWidget* second) {
        const int firstIndex = topLevelIndex(first);
        const int secondIndex = topLevelIndex(second);
        return std::any_of(separatorIndices.cbegin(), separatorIndices.cend(),
                           [firstIndex, secondIndex](int separatorIndex) {
                               return separatorIndex > firstIndex && separatorIndex < secondIndex;
                           });
    };
    require(hasSeparatorBetween(buttons.at(1), buttons.at(2)) &&
                hasSeparatorBetween(buttons.at(10), buttons.at(11)) &&
                hasSeparatorBetween(buttons.at(15), buttons.at(16)),
            "canonical toolbar separators should split editing, capture, and result sections");
    require(!hasSeparatorBetween(buttons.at(3), buttons.at(4)) &&
                !hasSeparatorBetween(buttons.at(11), buttons.at(12)) &&
                !hasSeparatorBetween(buttons.at(12), buttons.at(13)),
            "Arrow and Line grouping should not introduce an internal separator");
}

void groupedDrawingOptionsShowShortcutTooltips() {
    adqt::widgets::AdTooltip::installApplicationTooltips();
    ScreenshotToolPalette::Options options;
    options.showLineTool = true;
    options.showHighlightTool = true;
    options.showSpotlightTool = true;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette palette(options);
    palette.move(QApplication::primaryScreen()->availableGeometry().center() -
                 palette.rect().center());
    palette.show();
    QCoreApplication::processEvents();

    const QMap<QString, QString> expectedTooltips{
        {QStringLiteral("arrow"), QStringLiteral("Arrow (2)")},
        {QStringLiteral("line"), QStringLiteral("Line")},
        {QStringLiteral("highlighter"), QStringLiteral("Highlight (4 / H)")},
        {QStringLiteral("spotlight"), QStringLiteral("Spotlight")},
    };
    for (const char* triggerName : {"screenshotArrowLineButton", "screenshotHighlightButton"}) {
        auto* trigger =
            palette.findChild<adqt::widgets::AdButton*>(QString::fromLatin1(triggerName));
        require(trigger != nullptr, "drawing group trigger should exist");
        materializeLazyPopover(trigger);
        auto* popover = popoverForTrigger(trigger);
        require(popover != nullptr, "drawing group should expose its hover menu");
        popover->show();
        QCoreApplication::processEvents();
        require(popover->isVisible(), "drawing group hover menu should be visible");

        const auto buttons = popover->contentWidget()->findChildren<adqt::widgets::AdButton*>();
        require(buttons.size() == 2, "default drawing group should expose both tools");
        for (auto* button : buttons) {
            const QString expected =
                expectedTooltips.value(button->property("screenshotToolbarItemId").toString());
            require(!expected.isEmpty() && button->toolTip() == expected,
                    "drawing option should describe its function and assigned shortcuts");
            button->click();
            popover->show();
            QCoreApplication::processEvents();
            require(trigger->toolTip() == expected,
                    "drawing trigger should describe the selected tool and its assigned shortcut");
            for (auto* target : {trigger, button}) {
                const QPoint center = target->rect().center();
                QHelpEvent help(QEvent::ToolTip, center, target->mapToGlobal(center));
                QApplication::sendEvent(target, &help);
                require(
                    help.isAccepted(),
                    "drawing tool hover tooltip requests must be accepted while its menu is open");
                bool visible = false;
                for (auto* tooltip : qApp->findChildren<adqt::widgets::AdTooltip*>()) {
                    visible |= tooltip->isVisible() && tooltip->targetWidget() == target &&
                               tooltip->text() == expected;
                }
                require(visible, "drawing tool hover must display its description and shortcut");
            }
        }
        popover->hide();
        QCoreApplication::processEvents();
    }
}

void groupedActionOptionsShowShortcutTooltips() {
    adqt::widgets::AdTooltip::installApplicationTooltips();
    bool allTooltipsVisible = true;
    for (bool customLayout : {false, true}) {
        ScreenshotToolPalette::Options options;
        options.showTableTool = true;
        options.showQrTool = true;
        options.showScreenRecordButton = true;
        options.showOcrTool = true;
        options.showTextTranslationTool = true;
        options.showScrollingScreenshotTool = true;
        options.showSaveButton = true;
        options.actions = ScreenshotToolPalette::PinAction;
        options.enableStyleToolbar = false;
        if (customLayout) {
            options.actionToolsLayout = snow_shot::storage::ScreenshotToolbarLayout{
                {{QStringLiteral("table-recognition"), QStringLiteral("record-screen"),
                  QStringLiteral("save-as-file")},
                 {QStringLiteral("barcode-recognition"), QStringLiteral("text-recognition"),
                  QStringLiteral("text-translation")},
                 {QStringLiteral("scrolling-screenshot"), QStringLiteral("pin-to-screen")}},
                {}};
        }
        ScreenshotToolPalette palette(options);
        palette.move(QApplication::primaryScreen()->availableGeometry().center() -
                     palette.rect().center());
        palette.show();
        QCoreApplication::processEvents();
        int groupCount = 0;
        for (auto* trigger : mainActionToolbarButtons(palette)) {
            auto* popover = popoverForTrigger(trigger);
            if (popover == nullptr) {
                continue;
            }
            ++groupCount;
            materializeLazyPopover(trigger);
            require(popover->contentWidget() != nullptr, "action group should materialize options");
            const auto buttons = popover->contentWidget()->findChildren<adqt::widgets::AdButton*>();
            require(buttons.size() >= 2, "action group should contain multiple commands");
            for (auto* button : buttons) {
                const QString expected = button->toolTip();
                require(!expected.isEmpty(), "action option should describe its command");
                button->click();
                popover->show();
                QCoreApplication::processEvents();
                require(popover->isVisible() && trigger->toolTip() == expected,
                        "action trigger should retain the selected command's tooltip");
                for (auto* target : {trigger, button}) {
                    const QPoint center = target->rect().center();
                    QHelpEvent help(QEvent::ToolTip, center, target->mapToGlobal(center));
                    QApplication::sendEvent(target, &help);
                    bool visible = false;
                    for (auto* tooltip : qApp->findChildren<adqt::widgets::AdTooltip*>()) {
                        visible |= tooltip->isVisible() && tooltip->targetWidget() == target &&
                                   tooltip->text() == expected;
                    }
                    if (!help.isAccepted() || !visible) {
                        std::cerr << "Missing action tooltip: "
                                  << target->objectName().toStdString() << " / "
                                  << expected.toStdString() << '\n';
                        allTooltipsVisible = false;
                    }
                }
            }
            popover->hide();
            QCoreApplication::processEvents();
        }
        require(groupCount == (customLayout ? 3 : 2),
                "tooltip audit should cover recognition, save, and all custom action groups");
    }
    require(allTooltipsVisible, "grouped action tooltips must remain visible while menus are open");
}

void screenshotActionTooltipsUseConfiguredShortcuts() {
    ScreenshotToolPalette::Options options;
    options.showHistoryActions = true;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.enableStyleToolbar = false;
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;
    ScreenshotToolPalette palette(options);

    auto* pin =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotPinToScreenButton"));
    auto* undo =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotUndoButton"));
    auto* redo =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRedoButton"));
    auto* cancel =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Cancel screenshot"));
    auto* copy =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Copy to clipboard"));
    const auto tooltip = [](const QString& label, const QStringList& portable) {
        return QStringLiteral("%1 (%2)").arg(
            label, snow_shot::shortcuts::formatShortcutListDisplayText(
                       snow_shot::shortcuts::bindingsFromPortableText(portable)));
    };
    require(pin != nullptr && cancel != nullptr && copy != nullptr && undo != nullptr &&
                redo != nullptr &&
                pin->toolTip() ==
                    tooltip(QStringLiteral("Pin to screen"), {QStringLiteral("Ctrl+F")}) &&
                cancel->toolTip() ==
                    tooltip(QStringLiteral("Cancel screenshot"), {QStringLiteral("Esc")}) &&
                copy->toolTip() ==
                    tooltip(QStringLiteral("Copy to clipboard"), {QStringLiteral("Ctrl+C")}) &&
                undo->toolTip() == tooltip(QStringLiteral("Undo"), {QStringLiteral("Ctrl+Z")}) &&
                redo->toolTip() == tooltip(QStringLiteral("Redo"), {QStringLiteral("Ctrl+Y")}),
            "screenshot toolbar actions must show their configured shortcuts");

    pin->setToolTip(QStringLiteral("stale shortcut legend"));
    snow_shot::shortcuts::ShortcutDisplayService::instance().refresh();
    require(pin->toolTip() == tooltip(QStringLiteral("Pin to screen"), {QStringLiteral("Ctrl+F")}),
            "a keyboard-layout refresh must rebuild screenshot toolbar shortcut legends");

    const snow_shot::storage::ScreenshotShortcutSettings shortcutSettings;
    const snow_shot::shortcuts::ShortcutBindingMap originalShortcuts =
        shortcutSettings.allShortcuts();
    require(shortcutSettings.setShortcuts(QStringLiteral("pin_to_screen"),
                                          {QStringLiteral("Ctrl++"), QStringLiteral("Num+1")}),
            "pin shortcut fixture must support a non-default mapping");
    require(snow_shot::shortcuts::portableTextList(
                shortcutSettings.shortcuts(QStringLiteral("pin_to_screen"))) ==
                QStringList{QStringLiteral("Ctrl++"), QStringLiteral("Num+1")},
            "pin shortcut fixture must expose the updated mapping immediately");
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&palette, &languageChange);
    require(pin->toolTip() == tooltip(QStringLiteral("Pin to screen"),
                                      {QStringLiteral("Ctrl++"), QStringLiteral("Num+1")}),
            "screenshot toolbar shortcuts must survive runtime retranslation");
    require(shortcutSettings.setAllShortcutsAtomic(originalShortcuts),
            "pin shortcut fixture must restore the original mapping");
    QCoreApplication::sendEvent(&palette, &languageChange);
}

void screenshotActionTooltipsFollowStorageChangesWithoutRetranslation() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showHistoryActions = true;
    options.showSelectTool = false;
    options.enableStyleToolbar = false;
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;
    ScreenshotToolPalette palette(options);

    auto* pin =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotPinToScreenButton"));
    auto* shape = qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Shape"));
    require(pin != nullptr && shape != nullptr &&
                pin->toolTip() ==
                    shortcutTooltip(QStringLiteral("Pin to screen"), {QStringLiteral("Ctrl+F")}) &&
                shape->toolTip() == shortcutTooltip(QStringLiteral("Shape"), {QStringLiteral("1")}),
            "toolbar shortcuts should start from the configured defaults");

    const snow_shot::storage::ScreenshotShortcutSettings shortcutSettings;
    const snow_shot::storage::DrawingShortcutSettings drawingSettings;
    const snow_shot::shortcuts::ShortcutBindingMap originalShortcuts =
        shortcutSettings.allShortcuts();
    const snow_shot::shortcuts::ShortcutBindingMap originalDrawingShortcuts =
        drawingSettings.allShortcuts();
    require(
        shortcutSettings.setShortcuts(QStringLiteral("pin_to_screen"), {QStringLiteral("Alt+F")}),
        "pin shortcut fixture must support a non-default mapping");
    require(drawingSettings.setShortcuts(QStringLiteral("shape"), {QStringLiteral("Ctrl+2")}),
            "shape shortcut fixture must support a non-default mapping");
    require(pin->toolTip() ==
                    shortcutTooltip(QStringLiteral("Pin to screen"), {QStringLiteral("Alt+F")}) &&
                shape->toolTip() ==
                    shortcutTooltip(QStringLiteral("Shape"), {QStringLiteral("Ctrl+2")}),
            "toolbar tooltips must follow storage changes without a retranslation event");
    require(shortcutSettings.setShortcuts(QStringLiteral("pin_to_screen"), {}),
            "pin shortcut fixture must support clearing the mapping");
    require(pin->toolTip() == QStringLiteral("Pin to screen"),
            "clearing a shortcut must drop the tooltip hint instead of leaving it stale");
    require(shortcutSettings.setAllShortcutsAtomic(originalShortcuts) &&
                drawingSettings.setAllShortcutsAtomic(originalDrawingShortcuts),
            "shortcut fixtures must restore the original mappings");
}

void moveToolPresentationUsesTheOwningShortcutScope() {
    auto& language = snow_shot::presentation::LanguageManager::instance();
    require(language.setLanguage(QStringLiteral("en_US")),
            "move presentation test requires the English catalog");

    const snow_shot::storage::PinToScreenShortcutSettings shortcutSettings;
    const snow_shot::shortcuts::ShortcutBindingMap originalShortcuts =
        shortcutSettings.allShortcuts();
    require(shortcutSettings.setShortcuts(QStringLiteral("resize_window"), {QStringLiteral("M")}),
            "resize-window shortcut fixture must start from M");

    ScreenshotToolPalette::Options screenshotOptions;
    screenshotOptions.showMoveTool = true;
    screenshotOptions.showSelectTool = true;
    screenshotOptions.showShapeTool = false;
    screenshotOptions.showArrowTool = false;
    screenshotOptions.enableStyleToolbar = false;
    ScreenshotToolPalette screenshotPalette(screenshotOptions);
    const QList<adqt::widgets::AdButton*> screenshotButtons = mainToolbarButtons(screenshotPalette);
    require(screenshotButtons.size() == 2 &&
                screenshotButtons.constFirst()->accessibleName() ==
                    QStringLiteral("Edit selection") &&
                screenshotButtons.constFirst()->toolTip() ==
                    shortcutTooltip(QStringLiteral("Edit selection"),
                                    {QStringLiteral("M"), QStringLiteral("Ctrl+E")}),
            "the screenshot move tool must retain Edit selection and its screenshot shortcut");

    ScreenshotToolPalette::Options pinnedOptions;
    pinnedOptions.showDragHandle = true;
    pinnedOptions.showMoveTool = true;
    pinnedOptions.moveToolPresentation = ScreenshotToolPalette::MoveToolPresentation::ResizeWindow;
    pinnedOptions.showSelectTool = true;
    pinnedOptions.showShapeTool = false;
    pinnedOptions.showArrowTool = false;
    pinnedOptions.enableStyleToolbar = false;
    ScreenshotToolPalette pinnedPalette(pinnedOptions);
    const QList<adqt::widgets::AdButton*> pinnedButtons = mainToolbarButtons(pinnedPalette);
    QWidget* dragHandle =
        pinnedPalette.findChild<QWidget*>(QStringLiteral("screenshotToolbarDragHandle"));
    require(dragHandle != nullptr && pinnedButtons.size() == 2 &&
                pinnedButtons.constFirst()->accessibleName() == QStringLiteral("Resize window") &&
                pinnedButtons.constFirst()->toolTip() ==
                    shortcutTooltip(QStringLiteral("Resize window"), {QStringLiteral("M")}) &&
                pinnedButtons.at(1)->accessibleName() == QStringLiteral("Select elements") &&
                adqt::icons::describeIcon(pinnedButtons.constFirst()->iconRef()).key.name ==
                    QStringLiteral("tool-move"),
            "the pinned move tool must follow the drag handle and precede Select as Resize window");

    require(
        shortcutSettings.setShortcuts(QStringLiteral("resize_window"), {QStringLiteral("Alt+M")}) &&
            pinnedButtons.constFirst()->toolTip() ==
                shortcutTooltip(QStringLiteral("Resize window"), {QStringLiteral("Alt+M")}),
        "the Resize window tooltip must follow pin-to-screen shortcut changes");
    require(shortcutSettings.setShortcuts(QStringLiteral("resize_window"), {QStringLiteral("M")}),
            "resize-window language fixture must restore M");

    const std::pair<QString, QString> translations[] = {
        {QStringLiteral("zh_CN"), QStringLiteral("调整窗口大小")},
        {QStringLiteral("zh_TW"), QStringLiteral("調整視窗大小")},
    };
    for (const auto& [locale, translation] : translations) {
        require(language.setLanguage(locale), "Resize window language setup failed");
        QCoreApplication::processEvents();
        require(pinnedButtons.constFirst()->accessibleName() == translation &&
                    pinnedButtons.constFirst()->toolTip() ==
                        shortcutTooltip(translation, {QStringLiteral("M")}),
                "Resize window must retranslate its name without losing the shortcut hint");
    }

    require(shortcutSettings.setAllShortcutsAtomic(originalShortcuts) &&
                language.setLanguage(QStringLiteral("en_US")),
            "move presentation fixtures must restore shortcuts and language");
    QCoreApplication::processEvents();
}

void configurableToolbarLayoutSupportsArbitraryPopoverGroups() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showSpotlightTool = true;
    options.showHistoryActions = true;
    options.enableStyleToolbar = false;
    options.actions = ScreenshotToolPalette::CancelAction | ScreenshotToolPalette::CopyAction |
                      ScreenshotToolPalette::ConfirmAction;
    options.toolbarLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("free-draw"), QStringLiteral("line"), QStringLiteral("shape")},
         {QStringLiteral("spotlight"), QStringLiteral("arrow")},
         {QStringLiteral("highlighter")}},
        {}};

    ScreenshotToolPalette palette(options);
    palette.show();
    QCoreApplication::processEvents();
    const QList<adqt::widgets::AdButton*> drawingButtons = mainDrawingToolbarButtons(palette);
    require(drawingButtons.size() == 5,
            "each configured drawing and history position should occupy one live toolbar slot");
    auto* firstTrigger = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotDrawingToolGroupButton0"));
    auto* secondTrigger = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotDrawingToolGroupButton1"));
    require(firstTrigger == drawingButtons.at(0) && secondTrigger == drawingButtons.at(1) &&
                drawingButtons.at(2)->property("screenshotToolbarItemId").toString() ==
                    QStringLiteral("highlighter") &&
                popoverForTrigger(drawingButtons.at(2)) == nullptr &&
                palette.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotHighlighterButton")) == drawingButtons.at(2) &&
                palette.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotRectangleHighlightButton")) == nullptr,
            "multi-tool positions should use one trigger while singleton positions stay direct");
    require(firstTrigger->accessibleName() == QStringLiteral("Shape") &&
                firstTrigger->toolTip() == QStringLiteral("Shape (1)") &&
                firstTrigger->property("screenshotToolbarItemId").toString() ==
                    QStringLiteral("shape") &&
                secondTrigger->accessibleName() == QStringLiteral("Arrow") &&
                secondTrigger->toolTip() == QStringLiteral("Arrow (2)") &&
                secondTrigger->property("screenshotToolbarItemId").toString() ==
                    QStringLiteral("arrow"),
            "the last configured item should be each live group's initial trigger and tooltip");

    adqt::widgets::AdPopover* firstPopover = popoverForTrigger(firstTrigger);
    adqt::widgets::AdPopover* secondPopover = popoverForTrigger(secondTrigger);
    materializeLazyPopover(firstTrigger);
    auto* shapeOption = popoverButtonWithTooltip(firstPopover, "Shape");
    auto* lineOption = popoverButtonWithTooltip(firstPopover, "Line");
    auto* penOption = popoverButtonWithTooltip(firstPopover, "Pen");
    require(firstPopover != nullptr && shapeOption != nullptr && lineOption != nullptr &&
                penOption != nullptr &&
                qobject_cast<QHBoxLayout*>(firstPopover->contentWidget()->layout()) != nullptr &&
                firstPopover->contentWidget()->layout()->indexOf(shapeOption) <
                    firstPopover->contentWidget()->layout()->indexOf(lineOption) &&
                firstPopover->contentWidget()->layout()->indexOf(lineOption) <
                    firstPopover->contentWidget()->layout()->indexOf(penOption),
            "the first group popover should present configured tools from main to top");

    materializeLazyPopover(secondTrigger);
    auto* arrowOption = popoverButtonWithTooltip(secondPopover, "Arrow");
    auto* spotlightOption = popoverButtonWithTooltip(secondPopover, "Spotlight");
    require(secondPopover != nullptr && arrowOption != nullptr && spotlightOption != nullptr &&
                qobject_cast<QHBoxLayout*>(secondPopover->contentWidget()->layout()) != nullptr &&
                secondPopover->contentWidget()->layout()->indexOf(arrowOption) <
                    secondPopover->contentWidget()->layout()->indexOf(spotlightOption),
            "the second group popover should present configured tools from main to top");

    int freeDrawRequests = 0;
    int spotlightRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::freeDrawRequested,
                     [&freeDrawRequests]() { ++freeDrawRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::spotlightRequested,
                     [&spotlightRequests]() { ++spotlightRequests; });
    materializeLazyPopover(firstTrigger);
    penOption = popoverButtonWithTooltip(firstPopover, "Pen");
    require(penOption != nullptr, "reopening the first drawing group should recreate Pen");
    penOption->click();
    require(freeDrawRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::FreeDraw &&
                firstTrigger->accessibleName() == QStringLiteral("Pen") &&
                firstTrigger->property("screenshotToolbarItemId").toString() ==
                    QStringLiteral("free-draw"),
            "selecting an arbitrary group option should activate it and replace the trigger");
    firstTrigger->click();
    require(freeDrawRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select,
            "clicking an active arbitrary group trigger should return to selection");
    materializeLazyPopover(secondTrigger);
    spotlightOption = popoverButtonWithTooltip(secondPopover, "Spotlight");
    require(spotlightOption != nullptr,
            "reopening the second drawing group should recreate Spotlight");
    spotlightOption->click();
    require(spotlightRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Spotlight &&
                secondTrigger->accessibleName() == QStringLiteral("Spotlight"),
            "every arbitrary drawing group should independently remember its selected entry");
    require(palette.mainPanel()
                ->findChildren<QWidget*>(QStringLiteral("screenshotDrawingToolPosition"),
                                         Qt::FindDirectChildrenOnly)
                .isEmpty(),
            "the live screenshot toolbar should never create vertical drawing columns");

    const snow_shot::storage::ScreenshotToolbarLayout hiddenSpotlight{
        {{QStringLiteral("shape")},
         {QStringLiteral("highlighter"), QStringLiteral("free-draw")},
         {QStringLiteral("arrow")},
         {QStringLiteral("line")}},
        {QStringLiteral("spotlight")}};
    palette.setToolbarLayout(hiddenSpotlight);
    QCoreApplication::processEvents();
    const QList<adqt::widgets::AdButton*> updatedDrawingButtons =
        mainDrawingToolbarButtons(palette);
    adqt::widgets::AdButton* updatedGroupTrigger = updatedDrawingButtons.value(1);
    adqt::widgets::AdPopover* updatedPopover = popoverForTrigger(updatedGroupTrigger);
    if (updatedPopover != nullptr) {
        materializeLazyPopover(updatedGroupTrigger);
    }
    require(updatedDrawingButtons.size() == 6 && updatedPopover != nullptr &&
                updatedGroupTrigger->accessibleName() == QStringLiteral("Pen") &&
                popoverButtonWithTooltip(updatedPopover, "Pen") != nullptr &&
                popoverButtonWithTooltip(updatedPopover, "Highlight") != nullptr &&
                std::none_of(updatedDrawingButtons.cbegin(), updatedDrawingButtons.cend(),
                             [](const adqt::widgets::AdButton* button) {
                                 return button != nullptr &&
                                        button->property("screenshotToolbarPositionItems")
                                            .toStringList()
                                            .contains(QStringLiteral("spotlight"));
                             }),
            "hidden drawing tools should create no live toolbar slot or popover option");

    snow_shot::storage::ScreenshotToolbarLayout restored = hiddenSpotlight;
    restored.positions.push_back({QStringLiteral("spotlight")});
    restored.hidden.clear();
    palette.setToolbarLayout(restored);
    QCoreApplication::processEvents();
    const QList<adqt::widgets::AdButton*> restoredDrawingButtons =
        mainDrawingToolbarButtons(palette);
    require(restoredDrawingButtons.size() == 7 &&
                std::any_of(
                    restoredDrawingButtons.cbegin(), restoredDrawingButtons.cend(),
                    [](const adqt::widgets::AdButton* button) {
                        return button != nullptr &&
                               button->property("screenshotToolbarPositionItems").toStringList() ==
                                   QStringList{QStringLiteral("spotlight")};
                    }),
            "runtime layout changes should restore a previously hidden drawing tool");
}

void arrowAndLineUseConfiguredPopoverGroup() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = false;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.enableStyleToolbar = false;
    options.toolbarLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("line"), QStringLiteral("arrow")}}, {}};

    ScreenshotToolPalette palette(options);
    palette.show();
    QCoreApplication::processEvents();
    auto* trigger =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotArrowLineButton"));
    adqt::widgets::AdPopover* popover = popoverForTrigger(trigger);
    materializeLazyPopover(trigger);
    QWidget* content = popover != nullptr ? popover->contentWidget() : nullptr;
    auto* arrowOption = popoverButtonWithTooltip(popover, "Arrow");
    auto* lineOption = popoverButtonWithTooltip(popover, "Line");
    require(trigger != nullptr && mainDrawingToolbarButtons(palette).size() == 1 &&
                mainDrawingToolbarButtons(palette).constFirst() == trigger &&
                trigger->toolTip() == QStringLiteral("Arrow (2)") &&
                trigger->accessibleName() == QStringLiteral("Arrow") && popover != nullptr &&
                popover->triggers() == adqt::widgets::AdPopover::Trigger::Hover &&
                popover->placement() == adqt::widgets::AdPopover::Placement::Top &&
                popover->popupLayerMode() == adqt::widgets::AdPopover::PopupLayerMode::QtTool &&
                popover->arrowVisible() && popover->contentMargins() == QMargins(12, 12, 12, 12) &&
                content != nullptr &&
                content->objectName() == QStringLiteral("screenshotArrowLinePopoverContent") &&
                qobject_cast<QHBoxLayout*>(content->layout()) != nullptr &&
                arrowOption != nullptr && lineOption != nullptr &&
                content->layout()->indexOf(arrowOption) < content->layout()->indexOf(lineOption),
            "Arrow and Line should share one standard horizontal hover popover");

    int arrowRequests = 0;
    int lineRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::arrowRequested,
                     [&arrowRequests]() { ++arrowRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::lineRequested,
                     [&lineRequests]() { ++lineRequests; });
    arrowOption->click();
    require(arrowRequests == 1 && lineRequests == 0 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Arrow &&
                trigger->accessibleName() == QStringLiteral("Arrow"),
            "the Arrow popover option should activate the configured Arrow entry");
    materializeLazyPopover(trigger);
    lineOption = popoverButtonWithTooltip(popover, "Line");
    require(lineOption != nullptr,
            "reopening the Arrow and Line popover should recreate the Line option");
    lineOption->click();
    require(lineRequests == 1 && arrowRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Line &&
                trigger->toolTip() == QStringLiteral("Line") &&
                trigger->accessibleName() == QStringLiteral("Line") &&
                trigger->property("screenshotToolbarItemId").toString() == QStringLiteral("line"),
            "selecting Line should activate it and replace the shared trigger");
    trigger->click();
    require(lineRequests == 1 && arrowRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select,
            "clicking the active replaced group trigger should return to selection");
}

void tableBusyStatePreservesSiblingGroupPopovers(bool recoverFromMove = false) {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = false;
    options.showLineTool = true;
    options.showTableTool = true;
    options.showQrTool = true;
    options.showOcrTool = options.showTextTranslationTool = true;
    options.toolbarLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("arrow"), QStringLiteral("line")}}, {}};
    options.actionToolsLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition")},
         {QStringLiteral("text-recognition"), QStringLiteral("text-translation")}},
        {}};
    ScreenshotToolPalette palette(options);
    palette.resize(palette.contentSizeHint());
    palette.move(QApplication::primaryScreen()->availableGeometry().center() -
                 palette.rect().center());
    palette.show();
    palette.raise();
    palette.activateWindow();
    // Cocoa settles the initial window placement on exposure. Resolve it before
    // warping the native pointer to a trigger's global position.
    QCoreApplication::processEvents();
    auto* table =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableQrButton"));
    auto* drawing =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotArrowLineButton"));
    require(table && drawing, "recovery scenario needs recognition and drawing groups");
    auto* tablePopup = popoverForTrigger(table);
    auto* drawingPopup = popoverForTrigger(drawing);
    auto* action = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotActionToolGroupButton1"));
    require(action, "recovery scenario needs a custom action group");
    auto* actionPopup = popoverForTrigger(action);
    for (auto* popup : {tablePopup, drawingPopup, actionPopup}) {
        require(popup && !popup->contentWidget(), "all combo popovers must initially be lazy");
    }
    const auto verifyPopup = [recoverFromMove](adqt::widgets::AdPopover* popup) {
        require(popup != nullptr, "group trigger must own a popup");
        QWidget* trigger = popup->sourceWidget();
        require(trigger != nullptr, "group popup must have a hover trigger");
        const QPoint previousCursor = QCursor::pos();
        const auto restoreCursor = qScopeGuard([&] { QCursor::setPos(previousCursor); });
        QCursor::setPos(trigger->mapToGlobal(trigger->rect().center()));
        QEventLoop loop;
        QObject::connect(popup, &adqt::widgets::AdPopover::visibleChanged, &loop,
                         [&loop](bool visible) {
                             if (visible) {
                                 loop.quit();
                             }
                         });
        const QPoint local = trigger->rect().center();
        QEnterEvent enter(local, trigger->mapTo(trigger->window(), local),
                          trigger->mapToGlobal(local));
        if (recoverFromMove) {
            QMouseEvent move(QEvent::MouseMove, local, trigger->mapToGlobal(local), Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(trigger, &move);
        } else {
            QApplication::sendEvent(trigger, &enter);
        }
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        if (!popup->isVisible()) {
            loop.exec();
        }
        require(popup->isVisible() && popup->contentWidget() &&
                    popup->contentWidget()->isVisible() &&
                    popup->contentWidget()->window()->isVisible(),
                "hover must open the group popup and its actual surface");
        popup->hide();
    };
    QObject::connect(&palette, &ScreenshotToolPalette::tableRequested, &palette,
                     [&]() { palette.setTableBusy(true); });
    verifyPopup(actionPopup);
    verifyPopup(tablePopup);
    materializeLazyPopover(table);
    auto* option = popoverButtonWithTooltip(tablePopup, "Table recognition");
    require(option, "recognition group contains table option");
    option->click();
    require(table->busy(), "table action starts loading on the real group trigger");
    verifyPopup(drawingPopup);
    verifyPopup(tablePopup);
    verifyPopup(actionPopup);
    palette.setTableBusy(false);
    verifyPopup(tablePopup);
    verifyPopup(drawingPopup);
    palette.clearActiveTool();
    verifyPopup(tablePopup);
    verifyPopup(drawingPopup);
}

void mainToolbarGroupPopoversRecreateTheirOptions() {
    const auto flushDeferredDeletes = []() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    };
    const auto verifyDrawingGroup = [&](ScreenshotToolPalette::Options options,
                                        const QString& triggerObjectName,
                                        bool expectRecordingRestriction) {
        ScreenshotToolPalette palette(options);
        palette.show();
        QCoreApplication::processEvents();
        auto* trigger = palette.findChild<adqt::widgets::AdButton*>(triggerObjectName);
        adqt::widgets::AdPopover* popover = popoverForTrigger(trigger);
        require(trigger != nullptr && popover != nullptr && popover->contentWidget() == nullptr,
                "drawing group options must not exist before the first opening");

        popover->show();
        QCoreApplication::processEvents();
        QPointer<QWidget> firstContent = popover->contentWidget();
        auto* arrow = firstContent != nullptr
                          ? firstContent->findChild<adqt::widgets::AdButton*>(
                                QStringLiteral("screenshotDrawingToolGroupOption-arrow"))
                          : nullptr;
        auto* alternative =
            firstContent != nullptr
                ? firstContent->findChild<adqt::widgets::AdButton*>(
                      expectRecordingRestriction
                          ? QStringLiteral("screenshotDrawingToolGroupOption-highlighter")
                          : QStringLiteral("screenshotDrawingToolGroupOption-line"))
                : nullptr;
        require(firstContent != nullptr && arrow != nullptr && alternative != nullptr &&
                    firstContent->layout()->indexOf(arrow) <
                        firstContent->layout()->indexOf(alternative),
                "each drawing group opening must rebuild its configured ordered options");
        if (expectRecordingRestriction) {
            require(alternative->accessibleDescription() ==
                        QStringLiteral("Unavailable while recording"),
                    "recording-only restrictions must be applied to recreated group options");
        }
        QPointer<adqt::widgets::AdButton> firstArrow = arrow;
        QPointer<adqt::widgets::AdButton> firstAlternative = alternative;

        popover->hide();
        require(popover->contentWidget() == nullptr,
                "drawing group pointers must be released synchronously when hidden");
        flushDeferredDeletes();
        require(firstContent.isNull() && firstArrow.isNull() && firstAlternative.isNull(),
                "drawing group content and buttons must be deferred-deleted after hiding");

        if (!expectRecordingRestriction) {
            palette.setActiveTool(ScreenshotToolPalette::Tool::Line);
        }
        popover->show();
        QCoreApplication::processEvents();
        auto* secondArrow = popover->contentWidget()->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotDrawingToolGroupOption-arrow"));
        auto* secondAlternative = popover->contentWidget()->findChild<adqt::widgets::AdButton*>(
            expectRecordingRestriction
                ? QStringLiteral("screenshotDrawingToolGroupOption-highlighter")
                : QStringLiteral("screenshotDrawingToolGroupOption-line"));
        require(secondArrow != nullptr && secondAlternative != nullptr,
                "reopening a drawing group must create a complete new option tree");
        if (!expectRecordingRestriction) {
            require(
                secondAlternative->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                    secondAlternative->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
                "recreated options must apply selection changes made while closed");
        }
    };

    ScreenshotToolPalette::Options screenshotOptions;
    screenshotOptions.showShapeTool = false;
    screenshotOptions.showArrowTool = true;
    screenshotOptions.showLineTool = true;
    screenshotOptions.enableStyleToolbar = false;
    screenshotOptions.toolbarLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("line"), QStringLiteral("arrow")}}, {}};
    verifyDrawingGroup(screenshotOptions, QStringLiteral("screenshotArrowLineButton"), false);

    ScreenshotToolPalette::Options pinnedOptions = screenshotOptions;
    pinnedOptions.showMoveTool = true;
    pinnedOptions.moveToolPresentation = ScreenshotToolPalette::MoveToolPresentation::ResizeWindow;
    verifyDrawingGroup(pinnedOptions, QStringLiteral("screenshotArrowLineButton"), false);

    ScreenshotToolPalette::Options recordingOptions;
    recordingOptions.showShapeTool = false;
    recordingOptions.showArrowTool = true;
    recordingOptions.showHighlightTool = true;
    recordingOptions.showRecordingControls = true;
    recordingOptions.recordingDrawingMode = true;
    recordingOptions.enableStyleToolbar = false;
    recordingOptions.toolbarLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("highlighter"), QStringLiteral("arrow")}}, {}};
    verifyDrawingGroup(recordingOptions, QStringLiteral("screenshotDrawingToolGroupButton0"), true);

    ScreenshotToolPalette::Options actionOptions;
    actionOptions.showShapeTool = false;
    actionOptions.showArrowTool = false;
    actionOptions.showTableTool = true;
    actionOptions.showQrTool = true;
    actionOptions.enableStyleToolbar = false;
    actionOptions.actionToolsLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")}}, {}};
    ScreenshotToolPalette actionPalette(actionOptions);
    actionPalette.show();
    QCoreApplication::processEvents();
    auto* actionTrigger = actionPalette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotTableQrButton"));
    adqt::widgets::AdPopover* actionPopover = popoverForTrigger(actionTrigger);
    require(actionPopover != nullptr && actionPopover->contentWidget() == nullptr,
            "action group options must start unmaterialized");
    actionPopover->show();
    QCoreApplication::processEvents();
    QPointer<QWidget> firstActionContent = actionPopover->contentWidget();
    require(firstActionContent != nullptr,
            "the action group must create its content widget on opening");
    QPointer<adqt::widgets::AdButton> firstTable =
        firstActionContent->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotTableRecognitionOptionButton"));
    QPointer<adqt::widgets::AdButton> firstQr =
        firstActionContent->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotQrRecognitionOptionButton"));
    require(firstTable != nullptr && firstQr != nullptr,
            "the action group must create Table and QR options on opening");
    actionPopover->hide();
    require(actionPopover->contentWidget() == nullptr,
            "action group pointers must be released synchronously on hide");
    flushDeferredDeletes();
    require(firstActionContent.isNull() && firstTable.isNull() && firstQr.isNull(),
            "action option widgets must be destroyed after deferred events");

    actionPalette.setTableBusy(true);
    actionPalette.setQrEnabled(false);
    actionPopover->show();
    QCoreApplication::processEvents();
    auto* recreatedTable = actionPopover->contentWidget()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotTableRecognitionOptionButton"));
    auto* recreatedQr = actionPopover->contentWidget()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotQrRecognitionOptionButton"));
    require(recreatedTable != nullptr && recreatedTable->busy() && recreatedQr != nullptr &&
                !recreatedQr->isEnabled(),
            "recreated action options must replay busy and enabled state changed while closed");
}

void styleToolbarPopoversMaterializeWithTheirOwners() {
    const auto flushDeferredDeletes = []() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    };

    ScreenshotToolPalette::Options drawingOptions;
    drawingOptions.showTextTool = true;
    ScreenshotToolPalette drawingPalette(drawingOptions);
    drawingPalette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    drawingPalette.show();
    QCoreApplication::processEvents();
    QPointer<adqt::widgets::AdColorPicker> strokePicker =
        colorPickerWithAccessibleName(drawingPalette, "Stroke color");
    require(strokePicker != nullptr && !strokePicker->popupPrewarmEnabled() &&
                strokePicker->popupContent() == nullptr &&
                strokePicker->previewContent() == nullptr &&
                strokePicker->findChild<QWidget*>(QStringLiteral("ad-color-picker-picker-panel")) ==
                    nullptr,
            "style color pickers must remain cold until explicitly opened");
    strokePicker->setPopupVisible(true);
    QCoreApplication::processEvents();
    QPointer<QWidget> strokePopupContent = strokePicker->popupContent();
    QPointer<QWidget> samplerPreview = strokePicker->previewContent();
    require(strokePopupContent != nullptr && samplerPreview != nullptr,
            "opening a style color picker must materialize its custom rows and sampler");
    strokePicker->setPopupVisible(false);
    QCoreApplication::processEvents();
    require(strokePicker->popupContent() == strokePopupContent &&
                strokePicker->previewContent() == samplerPreview,
            "style color-picker content must survive an ordinary close");

    drawingPalette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
    QCoreApplication::processEvents();
    require(colorPickerWithAccessibleName(drawingPalette, "Arrow stroke color") == strokePicker &&
                strokePicker->popupContent() == strokePopupContent,
            "a retained shared editor must carry its materialized popup across reconciliation");
    QWidget* arrowheadTrigger = controlWithAccessibleName(drawingPalette, "Start arrowhead");
    adqt::widgets::AdPopover* arrowheadPopover = popoverForTrigger(arrowheadTrigger);
    require(arrowheadPopover != nullptr && arrowheadPopover->contentWidget() == nullptr,
            "arrowhead option content must remain absent before first opening");
    arrowheadPopover->show();
    QCoreApplication::processEvents();
    QPointer<QWidget> arrowheadContent = arrowheadPopover->contentWidget();
    require(arrowheadContent != nullptr &&
                popoverButtonWithTooltip(arrowheadPopover, "Start arrowhead none") != nullptr,
            "arrowhead options must function after retained factory materialization");
    arrowheadPopover->hide();
    QCoreApplication::processEvents();
    require(arrowheadPopover->contentWidget() == arrowheadContent,
            "retained icon-option content must survive ordinary popup close");

    drawingPalette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    flushDeferredDeletes();
    require(strokePicker.isNull() && strokePopupContent.isNull() && samplerPreview.isNull() &&
                arrowheadContent.isNull(),
            "evicting the style toolbar must destroy color and icon popup content with its owner");

    ScreenshotToolPalette::Options selectOptions;
    selectOptions.showTextTool = true;
    selectOptions.showFilterTool = true;
    selectOptions.showWatermarkTool = true;
    ScreenshotToolPalette selectPalette(selectOptions);
    const auto selectPopupCount = []() {
        const QWidgetList widgets = QApplication::allWidgets();
        return static_cast<int>(
            std::count_if(widgets.cbegin(), widgets.cend(), [](QWidget* widget) {
                return widget != nullptr &&
                       widget->objectName() == QStringLiteral("adselect-popup");
            }));
    };
    selectPalette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    auto* fontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(selectPalette, "Text font family"));
    require(fontSelect != nullptr, "the Text font select must exist");
    const int beforeFontOpen = selectPopupCount();
    fontSelect->setPopupVisible(true);
    QCoreApplication::processEvents();
    QPointer<QListView> fontView = fontSelect->view();
    require(fontView != nullptr && selectPopupCount() == beforeFontOpen + 1,
            "font select view must be absent until and materialize during opening");
    fontSelect->setPopupVisible(false);
    require(fontSelect->view() == fontView,
            "font select view must remain cached while its editor exists");

    selectPalette.setActiveTool(ScreenshotToolPalette::Tool::AutoFilter);
    flushDeferredDeletes();
    require(fontView.isNull(), "evicting Text must destroy its materialized select view");
    auto* filterSelect = selectPalette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotAutoFilterTypeSelect"));
    require(filterSelect != nullptr, "the Filter type select must exist");
    const int beforeFilterOpen = selectPopupCount();
    filterSelect->setPopupVisible(true);
    QCoreApplication::processEvents();
    QPointer<QListView> filterView = filterSelect->view();
    require(filterView != nullptr && selectPopupCount() == beforeFilterOpen + 1,
            "filter select must materialize only while opening");
    filterSelect->setPopupVisible(false);

    selectPalette.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    flushDeferredDeletes();
    require(filterView.isNull(), "evicting Filter must destroy its materialized select view");
    auto* watermarkSelect = selectPalette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotWatermarkTemplateSelect"));
    require(watermarkSelect != nullptr, "the Watermark template select must exist");
    const int beforeWatermarkOpen = selectPopupCount();
    watermarkSelect->setPopupVisible(true);
    QCoreApplication::processEvents();
    QPointer<QListView> watermarkView = watermarkSelect->view();
    require(watermarkView != nullptr && selectPopupCount() == beforeWatermarkOpen + 1,
            "Watermark construction and retranslation must defer its view until popup opening");
    watermarkSelect->setPopupVisible(false);
    require(watermarkSelect->view() == watermarkView,
            "Watermark popup widgets must remain cached while the Watermark editor exists");
    selectPalette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    flushDeferredDeletes();
    require(watermarkView.isNull(),
            "evicting Watermark must destroy its retained popup widgets with the editor subtree");
}

void recordingExportSettingsPopoversStayLazy() {
    // Opened picker editors are reparented into their Qt::Tool popup windows, so
    // materialization is observed through the global widget list, not findChild.
    const auto pickerPanelCount = []() {
        const QWidgetList widgets = QApplication::allWidgets();
        return static_cast<int>(
            std::count_if(widgets.cbegin(), widgets.cend(), [](QWidget* widget) {
                return widget != nullptr &&
                       widget->objectName() == QStringLiteral("ad-color-picker-picker-panel");
            }));
    };
    const auto hasPickerPanel = [](adqt::widgets::AdColorPicker* picker) {
        return picker != nullptr && picker->findChild<QWidget*>(
                                        QStringLiteral("ad-color-picker-picker-panel")) != nullptr;
    };
    // Prewarm defers through a zero-delay timing task; a second event-loop pass
    // settles it deterministically without depending on wall-clock timing.
    const auto flushDeferredTasks = []() {
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
    };

    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showRecordingControls = true;
    options.recordingDrawingMode = true;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette palette(options);
    palette.show();
    palette.prepareForDisplay();
    QCoreApplication::processEvents();

    auto* trail = palette.findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenRecordingMouseTrailColor"));
    auto* click = palette.findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenRecordingMouseClickColor"));
    QWidget* exportPanel =
        palette.findChild<QWidget*>(QStringLiteral("screenRecordingExportSettingsPanel"));
    require(exportPanel != nullptr && exportPanel->isVisible() && trail != nullptr &&
                click != nullptr,
            "recording drawing mode must reveal the export settings row with its pickers");
    require(!trail->popupPrewarmEnabled() && !click->popupPrewarmEnabled(),
            "export row color pickers must disable popup prewarm like every toolbar picker");
    flushDeferredTasks();
    require(pickerPanelCount() == 0 && !hasPickerPanel(trail) && !hasPickerPanel(click),
            "showing the export row must not prewarm its picker editors");
    trail->setPopupVisible(true);
    QCoreApplication::processEvents();
    require(pickerPanelCount() == 1,
            "opening an export row picker must materialize its editor on demand");
    trail->setPopupVisible(false);
    QCoreApplication::processEvents();
    const int exportRowPanelCount = pickerPanelCount();

    auto* settingsButton = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenRecordingEffectSettings"));
    require(settingsButton != nullptr, "the export row must expose its settings button");
    settingsButton->click();
    QCoreApplication::processEvents();
    auto* background = palette.findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenRecordingKeyboardBackgroundColor"));
    auto* foreground = palette.findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenRecordingKeyboardForegroundColor"));
    require(background != nullptr && foreground != nullptr,
            "opening settings must build its keyboard color fields");
    require(!background->popupPrewarmEnabled() && !foreground->popupPrewarmEnabled(),
            "keyboard color pickers must disable popup prewarm like every toolbar picker");
    flushDeferredTasks();
    require(pickerPanelCount() == exportRowPanelCount && !hasPickerPanel(background) &&
                !hasPickerPanel(foreground),
            "an open settings dialog must not prewarm its keyboard picker editors");
    background->setPopupVisible(true);
    QCoreApplication::processEvents();
    require(pickerPanelCount() == exportRowPanelCount + 1,
            "opening a keyboard picker must materialize its editor on demand");
    background->setPopupVisible(false);
    QCoreApplication::processEvents();
    if (auto* modal = palette.recordingEffectSettingsModalForTests()) {
        modal->accept();
        QCoreApplication::processEvents();
    }
}

void tableQrPopoverSharesOneEntryAndRemembersTheSelectedMode() {
    require(snow_shot::storage::ScreenshotToolbarSettings().setTableQrTool(QStringLiteral("qr")),
            "the remembered recognition mode should differ from the configured bottom tool");
    ScreenshotToolPalette::Options options;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showTableTool = true;
    options.showQrTool = true;
    options.enableStyleToolbar = false;
    options.actionToolsLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")}}, {}};

    ScreenshotToolPalette palette(options);
    palette.contentSizeHint();
    const QList<adqt::widgets::AdButton*> mainButtons = mainToolbarButtons(palette);
    require(mainButtons.size() == 1,
            "Table and QR recognition should occupy one main toolbar slot");
    auto* trigger =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableQrButton"));
    require(trigger == mainButtons.front() &&
                trigger->toolTip() == shortcutTooltip(QStringLiteral("Table recognition"),
                                                      {QStringLiteral("Ctrl+X")}) &&
                trigger->accessibleName() == QStringLiteral("Table recognition"),
            "the shared recognition slot should initially present Table recognition");

    adqt::widgets::AdPopover* popover = popoverForTrigger(trigger);
    require(popover != nullptr && popover->triggers() == adqt::widgets::AdPopover::Trigger::Hover,
            "the shared Table and QR slot should expose a hover popover");
    materializeLazyPopover(trigger);
    QWidget* content = popover->contentWidget();
    require(content != nullptr &&
                content->objectName() == QStringLiteral("screenshotTableQrPopoverContent"),
            "the Table and QR popover should expose stable testable content");
    adqt::widgets::AdButton* tableOption = popoverButtonWithTooltip(popover, "Table recognition");
    adqt::widgets::AdButton* qrOption = popoverButtonWithTooltip(popover, "Barcode recognition");
    require(tableOption != nullptr && qrOption != nullptr &&
                content->layout()->indexOf(tableOption) < content->layout()->indexOf(qrOption),
            "the shared popover should list Table recognition before QR recognition");

    int tableRequests = 0;
    int qrRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::tableRequested,
                     [&tableRequests]() { ++tableRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::qrRequested,
                     [&qrRequests]() { ++qrRequests; });

    trigger->click();
    require(tableRequests == 1 && qrRequests == 0 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Table,
            "the default shared trigger should run Table recognition directly");
    require(snow_shot::storage::ScreenshotToolbarSettings().tableQrTool() ==
                QStringLiteral("table"),
            "activating the configured entry must persist the chosen recognition mode");

    popover->show();
    QCoreApplication::processEvents();
    qrOption->click();
    require(qrRequests == 1 && tableRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Qr &&
                trigger->accessibleName() == QStringLiteral("Barcode recognition") &&
                adqt::icons::describeIcon(trigger->iconRef()).key.name ==
                    adqt::icons::describeIcon(
                        snow_shot::presentation::icons::custom::outlined::ScanQrcode())
                        .key.name,
            "choosing QR recognition should replace and activate the shared trigger");
    trigger->click();
    require(qrRequests == 1 && tableRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select,
            "clicking the active shared trigger should return to selection");

    materializeLazyPopover(trigger);
    tableOption = popoverButtonWithTooltip(popover, "Table recognition");
    qrOption = popoverButtonWithTooltip(popover, "Barcode recognition");
    require(tableOption != nullptr && qrOption != nullptr,
            "reopening the recognition popover should recreate both options");

    palette.setQrBusy(true);
    require(trigger->busy() && qrOption->busy() && !tableOption->busy(),
            "QR loading should be visible on the shared trigger and QR option only");
    palette.setTableBusy(true);
    require(trigger->busy() && qrOption->busy() && tableOption->busy(),
            "concurrent recognition should preserve each option's independent busy state");
    palette.setQrBusy(false);
    require(!trigger->busy() && tableOption->busy(),
            "an inactive recognition option must not make the selected entry busy");
    palette.setTableBusy(false);

    palette.setQrEnabled(false);
    require(!qrOption->isEnabled() && tableOption->isEnabled() && trigger->isEnabled(),
            "disabling QR should keep the shared slot available for Table recognition");
    palette.setQrEnabled(true);
    palette.setTableEnabled(false);
    palette.setQrEnabled(false);
    require(!trigger->isEnabled(), "an action stack with no enabled options must be disabled");
    palette.setTableEnabled(true);
    palette.setQrEnabled(true);
    materializeLazyPopover(trigger);
    tableOption = popoverButtonWithTooltip(popover, "Table recognition");
    require(tableOption != nullptr,
            "reopening the recognition popover should recreate Table before selection");
    tableOption->click();
    require(tableRequests == 2 && qrRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Table &&
                trigger->accessibleName() == QStringLiteral("Table recognition"),
            "choosing Table should restore the default shared trigger presentation");
}

void drawingGroupClicksActivateOnceAfterPointerReentry() {
    const QList<QStringList> groups{
        {QStringLiteral("line"), QStringLiteral("arrow")},
        {QStringLiteral("spotlight"), QStringLiteral("highlighter")},
        {QStringLiteral("free-draw"), QStringLiteral("line"), QStringLiteral("shape")}};
    const QList<ScreenshotToolPalette::Tool> tools{ScreenshotToolPalette::Tool::Arrow,
                                                   ScreenshotToolPalette::Tool::PenHighlight,
                                                   ScreenshotToolPalette::Tool::Shape};
    for (int index = 0; index < groups.size(); ++index) {
        ScreenshotToolPalette::Options options;
        options.showShapeTool = groups.at(index).contains(QStringLiteral("shape"));
        options.showArrowTool = groups.at(index).contains(QStringLiteral("arrow"));
        options.showLineTool = groups.at(index).contains(QStringLiteral("line"));
        options.showFreeDrawTool = groups.at(index).contains(QStringLiteral("free-draw"));
        options.showHighlightTool = groups.at(index).contains(QStringLiteral("highlighter"));
        options.showSpotlightTool = groups.at(index).contains(QStringLiteral("spotlight"));
        options.enableStyleToolbar = false;
        options.toolbarLayout = snow_shot::storage::ScreenshotToolbarLayout{{groups.at(index)}, {}};
        ScreenshotToolPalette palette(options);
        palette.show();
        QCoreApplication::processEvents();
        const auto buttons = mainDrawingToolbarButtons(palette);
        require(buttons.size() == 1, "the fixture must expose one drawing group trigger");
        auto* trigger = buttons.constFirst();
        palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
        const auto sendPointer = [&](QEvent::Type type, const QPointF& position,
                                     Qt::MouseButton button, Qt::MouseButtons held) {
            QMouseEvent event(type, position, trigger->mapToGlobal(position.toPoint()), button,
                              held, Qt::NoModifier);
            QCoreApplication::sendEvent(trigger, &event);
        };
        const QPointF center = trigger->rect().center();
        sendPointer(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
        sendPointer(QEvent::MouseMove, QPointF(-5, -5), Qt::NoButton, Qt::LeftButton);
        sendPointer(QEvent::MouseMove, center, Qt::NoButton, Qt::LeftButton);
        sendPointer(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
        require(palette.activeToolForTests() == tools.at(index),
                "a drawing group click must not toggle back off after pointer re-entry");
        sendPointer(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
        sendPointer(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
        require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select,
                "a second completed drawing group click must still toggle back to selection");
        sendPointer(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
        require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select,
                "a drawing group must wait for the completed click before activating");
        sendPointer(QEvent::MouseMove, QPointF(-5, -5), Qt::NoButton, Qt::LeftButton);
        sendPointer(QEvent::MouseButtonRelease, QPointF(-5, -5), Qt::LeftButton, Qt::NoButton);
        require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select,
                "releasing outside a drawing group must cancel activation");
    }
}

void tableRecognitionClickActivatesOnceAfterPointerReentry() {
    ScreenshotToolPalette::Options options;
    options.showTableTool = true;
    options.showQrTool = true;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette palette(options);
    palette.show();
    QCoreApplication::processEvents();
    palette.setActiveTool(ScreenshotToolPalette::Tool::Table);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    auto* trigger =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableQrButton"));
    require(trigger != nullptr, "the shared recognition trigger must exist");
    int tableRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::tableRequested, [&]() { ++tableRequests; });
    const auto sendPointer = [&](QEvent::Type type, const QPointF& position, Qt::MouseButton button,
                                 Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, trigger->mapToGlobal(position.toPoint()), button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(trigger, &event);
    };
    const QPointF center = trigger->rect().center();
    sendPointer(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    require(tableRequests == 0 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select,
            "pressing the trigger must not start recognition before the click completes");
    sendPointer(QEvent::MouseMove, QPointF(-5, -5), Qt::NoButton, Qt::LeftButton);
    sendPointer(QEvent::MouseMove, center, Qt::NoButton, Qt::LeftButton);
    sendPointer(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    require(tableRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Table,
            "one completed click must activate Table once even when the pointer re-enters");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    sendPointer(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    sendPointer(QEvent::MouseMove, QPointF(-5, -5), Qt::NoButton, Qt::LeftButton);
    sendPointer(QEvent::MouseButtonRelease, QPointF(-5, -5), Qt::LeftButton, Qt::NoButton);
    require(tableRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select,
            "releasing outside the trigger must cancel the click without starting recognition");
}

void toolbarStacksFollowConfiguredBottomToTopOrder() {
    using snow_shot::storage::ScreenshotToolbarLayout;
    using snow_shot::storage::ScreenshotToolbarLayoutKind;
    const auto exercise = [](ScreenshotToolbarLayoutKind kind, QStringList position,
                             bool conversionsAvailable = true) {
        ScreenshotToolPalette::Options options;
        options.showSelectTool = false;
        options.showShapeTool = true;
        options.showArrowTool = true;
        options.showLineTool = true;
        options.showFreeDrawTool = true;
        options.showTableTool = true;
        options.showQrTool = true;
        options.showImageConversionTools = conversionsAvailable;
        options.enableStyleToolbar = false;
        const auto makeLayout = [kind](const QStringList& items) {
            QStringList hidden = snow_shot::presentation::toolbar_layout::defaultOrder(kind);
            for (const QString& item : items) {
                hidden.removeAll(item);
            }
            return ScreenshotToolbarLayout{{items}, hidden};
        };
        if (kind == ScreenshotToolbarLayoutKind::DrawingTools) {
            options.toolbarLayout = makeLayout(position);
        } else {
            options.actionToolsLayoutKind = kind;
            options.actionToolsLayout = makeLayout(position);
        }
        ScreenshotToolPalette palette(options);
        for (int rotation = 0; rotation < position.size(); ++rotation) {
            if (kind == ScreenshotToolbarLayoutKind::DrawingTools) {
                palette.setToolbarLayout(makeLayout(position));
            } else {
                palette.setActionToolsLayout(makeLayout(position));
            }
            QStringList available = position;
            if (!conversionsAvailable) {
                available.removeAll(QStringLiteral("convert-to-markdown"));
                available.removeAll(QStringLiteral("convert-to-html"));
            }
            adqt::widgets::AdButton* trigger = nullptr;
            for (auto* button : mainToolbarButtons(palette)) {
                if (button->property("screenshotToolbarPositionItems").toStringList() ==
                    available) {
                    trigger = button;
                    break;
                }
            }
            require(trigger != nullptr, "each configured stack must have a visible trigger");
            materializeLazyPopover(trigger);
            auto* popover = popoverForTrigger(trigger);
            require(popover && popover->contentWidget(), "a stack must materialize its options");
            QStringList actual;
            auto* layout = popover->contentWidget()->layout();
            const auto descriptors =
                snow_shot::presentation::toolbar_layout::editorDescriptors(kind);
            for (int index = 0; index < layout->count(); ++index) {
                auto* button =
                    qobject_cast<adqt::widgets::AdButton*>(layout->itemAt(index)->widget());
                if (button == nullptr) {
                    continue;
                }
                for (const auto& descriptor : descriptors) {
                    if (button->accessibleName() == QString::fromUtf8(descriptor.label)) {
                        actual.push_back(QString::fromLatin1(descriptor.id));
                    }
                }
            }
            QStringList expected = available;
            std::reverse(expected.begin(), expected.end());
            if (actual != expected) {
                std::cerr << "Stack: " << position.join(',').toStdString()
                          << "; actual: " << actual.join(',').toStdString()
                          << "; expected: " << expected.join(',').toStdString() << '\n';
            }
            require(actual == expected,
                    "both toolbar popovers must display bottom to top, left to right");
            require(trigger->property("screenshotToolbarItemId").toString() ==
                        available.constLast(),
                    "both toolbars must initially display the bottom available tool");
            static_cast<void>(palette.setPhysicalScale(1.5));
            require(trigger->height() == 48 && trigger->iconSize() == QSize(36, 36),
                    "drawing and action stack triggers must use the same physical scale");
            static_cast<void>(palette.setPhysicalScale(1.0));
            for (const auto& descriptor : descriptors) {
                if (available.constFirst() == QLatin1String(descriptor.id)) {
                    auto* option = popoverButtonWithTooltip(popover, descriptor.label);
                    require(option != nullptr, "the top stack item must be selectable");
                    option->click();
                    require(trigger->property("screenshotToolbarItemId").toString() ==
                                available.constFirst(),
                            "choosing an option must update either toolbar's entry");
                    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
                }
            }
            position.push_front(position.takeLast());
        }
    };
    exercise(ScreenshotToolbarLayoutKind::DrawingTools,
             {QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("line"),
              QStringLiteral("free-draw")});
    const QStringList recognition{
        QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
        QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")};
    exercise(ScreenshotToolbarLayoutKind::PinnedActionTools, recognition);
    exercise(ScreenshotToolbarLayoutKind::PinnedActionTools, recognition, false);
    exercise(ScreenshotToolbarLayoutKind::ActionTools, recognition);
    exercise(ScreenshotToolbarLayoutKind::ActionTools, recognition, false);
}

void actionStacksKeepEnabledAlternativesReachable() {
    ScreenshotToolPalette::Options options;
    options.showOcrTool = true;
    options.showScreenRecordButton = true;
    options.enableStyleToolbar = false;
    options.actionToolsLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("record-screen"), QStringLiteral("text-recognition")}}, {}};
    ScreenshotToolPalette palette(options);
    auto* trigger = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotActionToolGroupButton0"));
    require(trigger != nullptr, "the mixed action stack must exist");
    materializeLazyPopover(trigger);
    auto* popover = popoverForTrigger(trigger);
    auto* ocr = popoverButtonWithTooltip(popover, "Text recognition");
    auto* record = popoverButtonWithTooltip(popover, "Record screen");
    require(ocr && record, "the mixed stack must expose both actions");
    require(adqt::icons::describeIcon(trigger->iconRef()).key.name ==
                    QStringLiteral("text-recognition") &&
                adqt::icons::describeIcon(ocr->iconRef()).key.name ==
                    QStringLiteral("text-recognition"),
            "the text recognition stack trigger and option must use the text recognition icon");
    int ocrRequests = 0;
    int recordRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::ocrRequested, [&]() { ++ocrRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::screenRecordRequested,
                     [&]() { ++recordRequests; });
    palette.setOcrEnabled(false);
    require(trigger->isEnabled() && record->isEnabled() && !ocr->isEnabled(),
            "every action stack must keep its enabled alternatives reachable");
    trigger->click();
    require(ocrRequests == 0, "a reachable stack must not dispatch its disabled entry");
    record->click();
    require(recordRequests == 1 && trigger->accessibleName() == QStringLiteral("Record screen"),
            "an enabled alternative must become the stack entry");
    materializeLazyPopover(trigger);
    ocr = popoverButtonWithTooltip(popover, "Text recognition");
    record = popoverButtonWithTooltip(popover, "Record screen");
    require(ocr != nullptr && record != nullptr,
            "reopening the mixed action stack should recreate its options");
    palette.setOcrBusy(true);
    require(!trigger->busy() && ocr->busy() && !record->busy(),
            "only the selected action may contribute the trigger's busy state");
}

void sharedToolbarLayoutModelOperationsAreDeterministic() {
    using snow_shot::presentation::toolbar_layout::defaultOrder;
    using snow_shot::presentation::toolbar_layout::moveItemToHidden;
    using snow_shot::presentation::toolbar_layout::moveItemToPosition;
    using snow_shot::presentation::toolbar_layout::normalizedLayout;
    using snow_shot::presentation::toolbar_layout::stackItemInPosition;
    using snow_shot::storage::ScreenshotToolbarLayout;
    using snow_shot::storage::ScreenshotToolbarLayoutKind;

    const auto exercise = [&](ScreenshotToolbarLayoutKind kind, const QString& first,
                              const QString& second, const QString& third) {
        QStringList remaining = defaultOrder(kind);
        remaining.removeAll(first);
        remaining.removeAll(second);
        remaining.removeAll(third);
        const ScreenshotToolbarLayout initial{{{first, second}, {third}}, remaining};

        const ScreenshotToolbarLayout unstacked = moveItemToPosition(initial, kind, second, 1);
        require(unstacked == ScreenshotToolbarLayout{{{first}, {second}, {third}}, remaining},
                "moving a stacked item beside its source must create a stable toolbar position");

        const ScreenshotToolbarLayout stacked = stackItemInPosition(unstacked, kind, second, 2, 1);
        require(stacked == ScreenshotToolbarLayout{{{first}, {third, second}}, remaining},
                "stacking must remove the source position and preserve the requested item order");

        QStringList hiddenItems = remaining;
        hiddenItems.prepend(first);
        const ScreenshotToolbarLayout hidden = moveItemToHidden(stacked, kind, first, 0);
        require(hidden == ScreenshotToolbarLayout{{{third, second}}, hiddenItems},
                "hiding must remove the toolbar position and preserve hidden ordering");

        const ScreenshotToolbarLayout restored =
            moveItemToPosition(hidden, kind, first, static_cast<int>(hidden.positions.size()));
        require(restored == ScreenshotToolbarLayout{{{third, second}, {first}}, remaining},
                "restoring a hidden item must remove it from hidden state and append its position");
        require(moveItemToHidden(initial, kind, QStringLiteral("unknown"), 0) ==
                    normalizedLayout(initial, kind),
                "unknown move requests must leave the normalized layout unchanged");
    };

    exercise(ScreenshotToolbarLayoutKind::DrawingTools, QStringLiteral("shape"),
             QStringLiteral("arrow"), QStringLiteral("free-draw"));
    exercise(ScreenshotToolbarLayoutKind::PinnedActionTools, QStringLiteral("barcode-recognition"),
             QStringLiteral("table-recognition"), QStringLiteral("text-translation"));
    exercise(ScreenshotToolbarLayoutKind::ActionTools, QStringLiteral("barcode-recognition"),
             QStringLiteral("table-recognition"), QStringLiteral("record-screen"));

    const auto drawing = ScreenshotToolbarLayoutKind::DrawingTools;
    const ScreenshotToolbarLayout legacy{{{QStringLiteral("shape")}}, {}};
    const ScreenshotToolbarLayout upgraded = normalizedLayout(legacy, drawing);
    require(upgraded.positions.at(upgraded.positions.size() - 3) ==
                    QStringList{QStringLiteral("separator")} &&
                upgraded.positions.at(upgraded.positions.size() - 2) ==
                    QStringList{QStringLiteral("undo")} &&
                upgraded.positions.constLast() == QStringList{QStringLiteral("redo")},
            "legacy drawing layouts must gain separator, Undo and Redo after the drawing tools");

    QStringList remaining = defaultOrder(drawing);
    for (const QString& id : {QStringLiteral("shape"), QStringLiteral("separator"),
                              QStringLiteral("undo"), QStringLiteral("redo")}) {
        remaining.removeAll(id);
    }
    const ScreenshotToolbarLayout withSeparator{{{QStringLiteral("shape")},
                                                 {QStringLiteral("separator")},
                                                 {QStringLiteral("undo"), QStringLiteral("redo")}},
                                                remaining};
    const auto separatorMoved =
        stackItemInPosition(withSeparator, drawing, QStringLiteral("separator"), 0, 1);
    const auto undoMoved =
        stackItemInPosition(withSeparator, drawing, QStringLiteral("undo"), 1, 0);
    for (const auto& candidate : {separatorMoved, undoMoved}) {
        for (const QStringList& position : candidate.positions) {
            require(!position.contains(QStringLiteral("separator")) || position.size() == 1,
                    "separator must never stack with a drawing or history button");
        }
    }
    const auto hiddenSeparator =
        moveItemToHidden(withSeparator, drawing, QStringLiteral("separator"), 0);
    require(hiddenSeparator.hidden.constFirst() == QStringLiteral("separator") &&
                std::none_of(hiddenSeparator.positions.cbegin(), hiddenSeparator.positions.cend(),
                             [](const QStringList& position) {
                                 return position.contains(QStringLiteral("separator"));
                             }),
            "separator must be movable into Hidden tools");
}

void drawingHistoryActionsFollowStacksAndHiddenShortcuts() {
    namespace layout = snow_shot::presentation::toolbar_layout;
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showHistoryActions = true;
    options.enableStyleToolbar = false;
    QStringList hidden =
        layout::defaultOrder(snow_shot::storage::ScreenshotToolbarLayoutKind::DrawingTools);
    for (const QString& id : {QStringLiteral("shape"), QStringLiteral("undo"),
                              QStringLiteral("redo"), QStringLiteral("separator")}) {
        hidden.removeAll(id);
    }
    options.toolbarLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("shape"), QStringLiteral("undo"), QStringLiteral("redo")},
         {QStringLiteral("separator")}},
        hidden};
    ScreenshotToolPalette palette(options);
    palette.show();
    QCoreApplication::processEvents();

    SnowCanvasHistoryState history;
    history.canUndo = true;
    history.canRedo = true;
    palette.setHistoryState(history);
    int undoRequests = 0;
    int redoRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::undoRequested,
                     [&undoRequests]() { ++undoRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::redoRequested,
                     [&redoRequests]() { ++redoRequests; });
    auto* trigger = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotDrawingToolGroupButton0"));
    require(trigger != nullptr &&
                trigger->property("screenshotToolbarItemId").toString() == QStringLiteral("redo"),
            "Redo at the bottom of a mixed stack must be its initial visible button");
    materializeLazyPopover(trigger);
    auto* popover = popoverForTrigger(trigger);
    auto* undoOption = popoverButtonWithTooltip(popover, "Undo");
    auto* redoOption = popoverButtonWithTooltip(popover, "Redo");
    require(undoOption != nullptr && redoOption != nullptr && undoOption->isEnabled() &&
                redoOption->isEnabled(),
            "Undo and Redo must appear as ordinary enabled stack options");
    undoOption->click();
    require(undoRequests == 1 &&
                trigger->property("screenshotToolbarItemId").toString() == QStringLiteral("undo"),
            "selecting Undo must execute it and make it the stack's visible entry");
    history.canUndo = false;
    palette.setHistoryState(history);
    materializeLazyPopover(trigger);
    popover = popoverForTrigger(trigger);
    undoOption = popoverButtonWithTooltip(popover, "Undo");
    redoOption = popoverButtonWithTooltip(popover, "Redo");
    require(undoOption != nullptr && redoOption != nullptr && !undoOption->isEnabled() &&
                trigger->isEnabled(),
            "an unavailable selected history action must leave other stack options reachable");
    redoOption->click();
    require(redoRequests == 1 &&
                trigger->property("screenshotToolbarItemId").toString() == QStringLiteral("redo"),
            "an available Redo option must remain executable after Undo becomes unavailable");

    QStringList hiddenAll =
        layout::defaultOrder(snow_shot::storage::ScreenshotToolbarLayoutKind::DrawingTools);
    hiddenAll.removeAll(QStringLiteral("shape"));
    palette.setToolbarLayout({{{QStringLiteral("shape")}}, hiddenAll});
    QCoreApplication::processEvents();
    auto* undoButton =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotUndoButton"));
    auto* redoButton =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRedoButton"));
    require(undoButton != nullptr && undoButton->isHidden() && redoButton != nullptr &&
                redoButton->isHidden(),
            "hidden Undo and Redo must have no visible toolbar buttons");
    QLayout* hiddenRow = palette.mainPanel()->layout();
    bool hiddenSeparatorVisible = false;
    for (int index = 0; index < hiddenRow->count(); ++index) {
        hiddenSeparatorVisible |=
            qobject_cast<QFrame*>(hiddenRow->itemAt(index)->widget()) != nullptr;
    }
    require(!hiddenSeparatorVisible,
            "hiding Separator Component must remove the configurable divider");
    history.canUndo = true;
    palette.setHistoryState(history);
    require(palette.activateScreenshotShortcut(QStringLiteral("undo")) &&
                palette.activateScreenshotShortcut(QStringLiteral("redo")) && undoRequests == 2 &&
                redoRequests == 2,
            "hidden history actions must still respond to their shortcuts");
    history.canUndo = false;
    history.canRedo = false;
    palette.setHistoryState(history);
    require(!palette.activateScreenshotShortcut(QStringLiteral("undo")) &&
                !palette.activateScreenshotShortcut(QStringLiteral("redo")),
            "hidden history shortcuts must still respect command availability");

    palette.setToolbarLayout({{{QStringLiteral("undo")},
                               {QStringLiteral("separator")},
                               {QStringLiteral("shape")},
                               {QStringLiteral("redo")}},
                              hidden});
    QCoreApplication::processEvents();
    const auto drawingButtons = mainDrawingToolbarButtons(palette);
    QLayout* row = palette.mainPanel()->layout();
    int separatorIndex = -1;
    for (int index = 0; index < row->count(); ++index) {
        if (qobject_cast<QFrame*>(row->itemAt(index)->widget()) != nullptr) {
            separatorIndex = index;
            break;
        }
    }
    require(drawingButtons.size() == 3 && drawingButtons.at(0) == undoButton &&
                drawingButtons.at(2) == redoButton && separatorIndex > row->indexOf(undoButton) &&
                separatorIndex < row->indexOf(drawingButtons.at(1)),
            "standalone Undo and Redo must follow their configured order around the separator");
    history.canUndo = true;
    palette.setHistoryState(history);
    undoButton->click();
    require(undoRequests == 3, "a moved standalone Undo button must remain executable");
}

void recordingDrawingLayoutRendersConfiguredSeparator() {
    namespace layout = snow_shot::presentation::toolbar_layout;
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showRecordingControls = true;
    options.enableStyleToolbar = false;
    QStringList hidden =
        layout::defaultOrder(snow_shot::storage::ScreenshotToolbarLayoutKind::DrawingTools);
    for (const QString& id :
         {QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("separator")}) {
        hidden.removeAll(id);
    }
    options.toolbarLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("shape")}, {QStringLiteral("separator")}, {QStringLiteral("arrow")}},
        hidden};
    ScreenshotToolPalette palette(options);
    palette.show();
    QCoreApplication::processEvents();

    const auto hasDividerBetweenDrawingButtons = [&palette]() {
        const auto buttons = mainDrawingToolbarButtons(palette);
        require(buttons.size() == 2, "recording toolbar must show the configured drawing tools");
        const QLayout* row = palette.mainPanel()->layout();
        const int first = row->indexOf(buttons.at(0));
        const int second = row->indexOf(buttons.at(1));
        for (int index = first + 1; index < second; ++index) {
            if (qobject_cast<QFrame*>(row->itemAt(index)->widget()) != nullptr) {
                return true;
            }
        }
        return false;
    };
    require(hasDividerBetweenDrawingButtons(),
            "recording toolbar must render the configured separator without history buttons");
    hidden.push_back(QStringLiteral("separator"));
    palette.setToolbarLayout({{{QStringLiteral("shape")}, {QStringLiteral("arrow")}}, hidden});
    QCoreApplication::processEvents();
    require(!hasDividerBetweenDrawingButtons(),
            "hiding the shared separator must remove it from the recording drawing tools");
}

void pinnedActionLayoutUsesGenericStacks() {
    namespace layout = snow_shot::presentation::toolbar_layout;
    using snow_shot::storage::ScreenshotToolbarLayout;
    const auto kind = snow_shot::storage::ScreenshotToolbarLayoutKind::PinnedActionTools;
    const QString table = QStringLiteral("table-recognition");
    const QString barcode = QStringLiteral("barcode-recognition");
    const QString ocr = QStringLiteral("text-recognition");
    const QString translation = QStringLiteral("text-translation");
    const QString markdown = QStringLiteral("convert-to-markdown");
    const QString html = QStringLiteral("convert-to-html");
    require(snow_shot::storage::ScreenshotToolbarSettings().setTableQrTool(QStringLiteral("qr")),
            "pinned fixture must set a conflicting legacy preference");
    ScreenshotToolPalette::Options options;
    options.showTableTool = options.showQrTool = options.showOcrTool = true;
    options.showTextTranslationTool = options.showImageConversionTools = true;
    options.showSaveButton = options.saveButtonWithResultActions = true;
    options.actions = ScreenshotToolPalette::CopyAction | ScreenshotToolPalette::ConfirmAction;
    options.actionToolsLayoutKind = kind;
    const ScreenshotToolbarLayout expected{
        {{barcode, table}, {markdown}, {html}, {ocr}, {translation}}, {}};
    options.actionToolsLayout = expected;
    require(layout::normalizedLayout(expected, kind) == expected,
            "presentation normalization must not migrate pinned layouts");
    ScreenshotToolPalette palette(options);
    palette.show();
    QCoreApplication::processEvents();
    const auto positions = [&]() {
        QVector<QStringList> result;
        for (auto* button : mainToolbarButtons(palette)) {
            const auto ids = button->property("screenshotToolbarPositionItems").toStringList();
            if (!ids.isEmpty() && layout::defaultOrder(kind).contains(ids.first()))
                result.append(ids);
        }
        return result;
    };
    require(positions() == expected.positions,
            "pinned rendering must preserve configured positions");
    const auto actionButtons = mainActionToolbarButtons(palette);
    const auto ocrButton =
        std::find_if(actionButtons.cbegin(), actionButtons.cend(),
                     [&ocr](const adqt::widgets::AdButton* button) {
                         return button->property("screenshotToolbarItemId").toString() == ocr;
                     });
    require(ocrButton != actionButtons.cend() &&
                adqt::icons::describeIcon((*ocrButton)->iconRef()).key.name ==
                    QStringLiteral("text-recognition"),
            "the pinned text recognition tool must keep the text recognition icon after layout");
    auto* trigger = mainActionToolbarButtons(palette).first();
    require(trigger->property("screenshotToolbarItemId").toString() == table,
            "legacy Barcode preference must not replace the configured Table entry");
    int tables = 0;
    int barcodes = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::tableRequested, [&] { ++tables; });
    QObject::connect(&palette, &ScreenshotToolPalette::qrRequested, [&] { ++barcodes; });
    trigger->click();
    require(tables == 1 && barcodes == 0 &&
                snow_shot::storage::ScreenshotToolbarSettings().tableQrTool() ==
                    QStringLiteral("qr"),
            "configured Table entry must dispatch Table without writing a legacy preference");
    materializeLazyPopover(trigger);
    auto* barcodeOption =
        popoverButtonWithTooltip(popoverForTrigger(trigger), "Barcode recognition");
    require(barcodeOption != nullptr, "generic recognition stack must expose Barcode");
    barcodeOption->click();
    require(barcodes == 1 && trigger->property("screenshotToolbarItemId").toString() == barcode,
            "activating a stacked tool must select its entry through the generic group logic");
    palette.setTableEnabled(false);
    palette.setQrEnabled(true);
    palette.setQrBusy(true);
    require(trigger->isEnabled() &&
                trigger->property("screenshotToolbarItemId").toString() == barcode &&
                positions() == expected.positions,
            "recognition state changes must preserve stack membership and entry");
    palette.setTableEnabled(true);
    palette.setQrBusy(false);
    const ScreenshotToolbarLayout mixed{{{translation, table}, {barcode}}, {ocr, markdown, html}};
    palette.setActionToolsLayout(mixed);
    require(positions() == mixed.positions,
            "pinned tools must support arbitrary stacks and hidden items");
    for (const QString& hidden : {table, barcode}) {
        ScreenshotToolbarLayout separated{{{table}, {barcode}, {translation}},
                                          {ocr, markdown, html}};
        for (qsizetype index = separated.positions.size(); index-- > 0;) {
            if (separated.positions.at(index).contains(hidden))
                separated.positions.removeAt(index);
        }
        separated.hidden.append(hidden);
        palette.setActionToolsLayout(separated);
        require(positions() == separated.positions,
                "hiding a recognition tool must not restore it through its sibling");
    }
    palette.setActionToolsLayout({{}, layout::defaultOrder(kind)});
    require(positions().isEmpty(), "all six pinned tools can be hidden without legacy restoration");
    int saves = 0;
    int copies = 0;
    int confirms = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::saveRequested, [&] { ++saves; });
    QObject::connect(&palette, &ScreenshotToolPalette::copyRequested, [&] { ++copies; });
    QObject::connect(&palette, &ScreenshotToolPalette::confirmRequested, [&] { ++confirms; });
    for (auto* button : mainToolbarButtons(palette)) {
        if (button->property("screenshotToolbarItemId").toString() ==
                QStringLiteral("save-as-file") ||
            button->accessibleName() == QStringLiteral("Copy to clipboard") ||
            button->accessibleName() == QStringLiteral("Confirm edit"))
            button->click();
    }
    require(saves == 1 && copies == 1 && confirms == 1,
            "fixed result controls must survive hiding all pinned tools");
    palette.setActionToolsLayout({});
    require(positions() == layout::defaultPositions(kind),
            "pinned defaults must be restorable at runtime");
}

void quickSaveStacksAndLayoutMigration() {
    using namespace snow_shot::presentation::toolbar_layout;
    using snow_shot::storage::ScreenshotToolbarLayout;
    using snow_shot::storage::ScreenshotToolbarLayoutKind;
    const auto kind = ScreenshotToolbarLayoutKind::ActionTools;
    const QString quick = QStringLiteral("quick-save");
    const QString save = QStringLiteral("save-as-file");
    auto legacy = normalizedLayout(ScreenshotToolbarLayout{}, kind);
    for (auto& position : legacy.positions)
        position.removeAll(quick);
    const auto upgraded = normalizedLayout(legacy, kind);
    require(upgraded.positions.constLast() == QStringList{quick, save} &&
                normalizedLayout(upgraded, kind) == upgraded,
            "legacy default saves must upgrade to an idempotent quick-save stack");
    auto custom = legacy;
    custom.positions.last().prepend(QStringLiteral("record-screen"));
    custom.positions.removeAt(1);
    require(normalizedLayout(custom, kind).positions.constLast() ==
                QStringList{QStringLiteral("record-screen"), quick, save},
            "custom save placements must gain Quick save immediately above Save");
    legacy.positions.removeLast();
    legacy.hidden = {save};
    const auto hidden = normalizedLayout(legacy, kind);
    require(hidden.hidden.contains(save) && hidden.hidden.contains(quick),
            "hiding Save in a legacy layout must also hide the new Quick save action");
    auto explicitLayout = upgraded;
    explicitLayout.positions.last().removeAll(quick);
    explicitLayout.hidden = {quick};
    require(normalizedLayout(explicitLayout, kind) == explicitLayout,
            "an explicitly hidden Quick save must stay hidden");
    explicitLayout.hidden.clear();
    explicitLayout.positions.prepend({quick});
    require(normalizedLayout(explicitLayout, kind) == explicitLayout,
            "an explicitly moved Quick save must keep its placement");
    for (const bool pinned : {false, true}) {
        ScreenshotToolPalette::Options options;
        options.showSaveButton = true;
        options.saveButtonWithResultActions = pinned;
        options.actions = ScreenshotToolPalette::CopyAction | ScreenshotToolPalette::ConfirmAction;
        options.actionToolsLayout = upgraded;
        ScreenshotToolPalette palette(options);
        palette.show();
        QCoreApplication::processEvents();
        adqt::widgets::AdButton* trigger = nullptr;
        for (auto* button : mainToolbarButtons(palette)) {
            if (button->property("screenshotToolbarPositionItems").toStringList() ==
                QStringList{quick, save})
                trigger = button;
        }
        require(trigger && trigger->accessibleName() == QStringLiteral("Save as file"),
                "both toolbars must initially show Save as the save-stack trigger");
        int saves = 0;
        int quickSaves = 0;
        QObject::connect(&palette, &ScreenshotToolPalette::saveRequested, [&] { ++saves; });
        QObject::connect(&palette, &ScreenshotToolPalette::quickSaveRequested,
                         [&] { ++quickSaves; });
        trigger->click();
        materializeLazyPopover(trigger);
        auto* popover = popoverForTrigger(trigger);
        auto* quickOption = popoverButtonWithTooltip(popover, "Quick save");
        auto* saveOption = popoverButtonWithTooltip(popover, "Save as file");
        require(quickOption && saveOption, "both save actions must be reachable in the popover");
        quickOption->click();
        require(saves == 1 && quickSaves == 1 &&
                    trigger->accessibleName() == QStringLiteral("Quick save") &&
                    adqt::icons::describeIcon(trigger->iconRef()).key.name == quick,
                "selecting Quick save must dispatch once and replace the trigger/icon");
        trigger->click();
        require(quickSaves == 2 && saves == 1, "the selected trigger must continue to quick-save");
        materializeLazyPopover(trigger);
        quickOption = popoverButtonWithTooltip(popover, "Quick save");
        saveOption = popoverButtonWithTooltip(popover, "Save as file");
        require(quickOption != nullptr && saveOption != nullptr,
                "reopening the save stack should recreate both save options");
        auto& language = snow_shot::presentation::LanguageManager::instance();
        for (const auto& entry : {std::pair{QStringLiteral("zh_CN"), QStringLiteral("快速保存")},
                                  std::pair{QStringLiteral("zh_TW"), QStringLiteral("快速儲存")}}) {
            require(language.setLanguage(entry.first), "quick-save language setup failed");
            QCoreApplication::processEvents();
            require(trigger->accessibleName() == entry.second &&
                        quickOption->accessibleName() == entry.second,
                    "the current save trigger and materialized popover must retranslate");
        }
        require(language.setLanguage(QStringLiteral("en_US")), "restore quick-save test language");
        QCoreApplication::processEvents();

        saveOption->click();
        require(saves == 2 && quickSaves == 2,
                "selecting manual Save must not also dispatch Quick save");
    }
}

void configurableScreenshotActionLayoutSupportsStacksHidingAndRuntimeReplacement() {
    require(snow_shot::storage::ScreenshotToolbarSettings().setTableQrTool(QStringLiteral("table")),
            "the configurable action toolbar fixture should start in Table mode");
    ScreenshotToolPalette::Options options;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showTableTool = true;
    options.showQrTool = true;
    options.showScreenRecordButton = true;
    options.showOcrTool = true;
    options.showTextTranslationTool = true;
    options.showScrollingScreenshotTool = true;
    options.showSaveButton = true;
    options.enableStyleToolbar = false;
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;
    options.actionToolsLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {
            {QStringLiteral("record-screen"), QStringLiteral("quick-save"),
             QStringLiteral("save-as-file"), QStringLiteral("table-recognition")},
            {QStringLiteral("barcode-recognition")},
            {QStringLiteral("text-translation"), QStringLiteral("text-recognition")},
            {QStringLiteral("pin-to-screen")},
        },
        {QStringLiteral("scrolling-screenshot")},
    };

    ScreenshotToolPalette palette(options);
    palette.show();
    QCoreApplication::processEvents();
    const QList<adqt::widgets::AdButton*> actionButtons = mainActionToolbarButtons(palette);
    auto* mixedTrigger = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotActionToolGroupButton0"));
    auto* barcodeButton = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotQrRecognitionButton"));
    auto* textTrigger = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotActionToolGroupButton2"));
    require(actionButtons.size() == 4 && mixedTrigger == actionButtons.at(0) &&
                barcodeButton == actionButtons.at(1) && textTrigger == actionButtons.at(2) &&
                mixedTrigger->accessibleName() == QStringLiteral("Table recognition") &&
                mixedTrigger->toolTip() == shortcutTooltip(QStringLiteral("Table recognition"),
                                                           {QStringLiteral("Ctrl+X")}) &&
                mixedTrigger->property("screenshotToolbarPositionItems").toStringList() ==
                    QStringList{QStringLiteral("record-screen"), QStringLiteral("quick-save"),
                                QStringLiteral("save-as-file"),
                                QStringLiteral("table-recognition")} &&
                textTrigger->accessibleName() == QStringLiteral("Text recognition") &&
                palette
                    .findChild<adqt::widgets::AdButton*>(
                        QStringLiteral("screenshotScrollingScreenshotButton"))
                    ->isHidden(),
            "custom action positions must preserve order, use the last item as trigger, and hide "
            "removed tools");

    int saveRequests = 0;
    int tableRequests = 0;
    int qrRequests = 0;
    int translationRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::saveRequested,
                     [&saveRequests]() { ++saveRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::tableRequested,
                     [&tableRequests]() { ++tableRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::qrRequested,
                     [&qrRequests]() { ++qrRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::textTranslationRequested,
                     [&translationRequests]() { ++translationRequests; });

    materializeLazyPopover(mixedTrigger);
    adqt::widgets::AdPopover* mixedPopover = popoverForTrigger(mixedTrigger);
    auto* saveOption = popoverButtonWithTooltip(mixedPopover, "Save as file");
    auto* tableOption = popoverButtonWithTooltip(mixedPopover, "Table recognition");
    require(mixedPopover != nullptr &&
                mixedPopover->contentWidget()->objectName() ==
                    QStringLiteral("screenshotActionToolGroupPopoverContent") &&
                saveOption != nullptr && tableOption != nullptr,
            "an arbitrary action stack must lazily expose all available actions");
    saveOption->click();
    require(saveRequests == 1 && mixedTrigger->accessibleName() == QStringLiteral("Save as file") &&
                mixedTrigger->toolTip() ==
                    shortcutTooltip(QStringLiteral("Save as file"), {QStringLiteral("Ctrl+S")}),
            "choosing an action stack option must dispatch it and replace the current trigger");
    mixedTrigger->click();
    require(saveRequests == 2,
            "the replaced action stack trigger must execute its newly selected command");
    materializeLazyPopover(mixedTrigger);
    tableOption = popoverButtonWithTooltip(mixedPopover, "Table recognition");
    require(tableOption != nullptr,
            "reopening the mixed action stack should recreate the Table option");
    tableOption->click();
    require(tableRequests == 1 && snow_shot::storage::ScreenshotToolbarSettings().tableQrTool() ==
                                      QStringLiteral("table"),
            "Table selection in an arbitrary stack must retain the cross-instance preference");

    materializeLazyPopover(textTrigger);
    adqt::widgets::AdPopover* textPopover = popoverForTrigger(textTrigger);
    auto* recognitionOption = popoverButtonWithTooltip(textPopover, "Text recognition");
    auto* translationOption = popoverButtonWithTooltip(textPopover, "Text translation");
    require(recognitionOption != nullptr && translationOption != nullptr,
            "text actions in a stack must retain independent popover entries");
    palette.setOcrEnabled(false);
    require(!textTrigger->isEnabled() && textPopover->contentWidget() == nullptr,
            "disabling every text action should close and release the action options");
    palette.setOcrEnabled(true);
    materializeLazyPopover(textTrigger);
    recognitionOption = popoverButtonWithTooltip(textPopover, "Text recognition");
    translationOption = popoverButtonWithTooltip(textPopover, "Text translation");
    require(recognitionOption != nullptr && recognitionOption->isEnabled() &&
                translationOption != nullptr && translationOption->isEnabled(),
            "reenabling and reopening the text action stack should recreate enabled entries");
    translationOption->click();
    require(translationRequests == 1 && textTrigger->isEnabled() &&
                textTrigger->accessibleName() == QStringLiteral("Text translation"),
            "selecting an enabled stack item must replace a disabled trigger");
    materializeLazyPopover(textTrigger);
    recognitionOption = popoverButtonWithTooltip(textPopover, "Text recognition");
    translationOption = popoverButtonWithTooltip(textPopover, "Text translation");
    require(recognitionOption != nullptr && translationOption != nullptr,
            "reopening the text action stack should recreate both entries");
    palette.setOcrBusy(true);
    require(textTrigger->busy() && !recognitionOption->busy() && translationOption->busy(),
            "busy state must propagate from the active source action to its stack presentation");
    palette.setOcrBusy(false);

    barcodeButton->click();
    require(qrRequests == 1 && snow_shot::storage::ScreenshotToolbarSettings().tableQrTool() ==
                                   QStringLiteral("qr"),
            "an independently placed Barcode control must dispatch and persist its mode");

    const snow_shot::storage::ScreenshotToolbarLayout unstacked{
        {
            {QStringLiteral("barcode-recognition")},
            {QStringLiteral("table-recognition")},
            {QStringLiteral("record-screen")},
            {QStringLiteral("pin-to-screen")},
            {QStringLiteral("text-recognition")},
            {QStringLiteral("text-translation")},
            {QStringLiteral("scrolling-screenshot")},
            {QStringLiteral("save-as-file")},
        },
        {},
    };
    palette.setActionToolsLayout(unstacked);
    QCoreApplication::processEvents();
    auto* directBarcode = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotQrRecognitionButton"));
    auto* directTable = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotTableRecognitionButton"));
    require(mainActionToolbarButtons(palette).size() == 8 && directBarcode != nullptr &&
                directTable != nullptr && popoverForTrigger(directBarcode) == nullptr &&
                popoverForTrigger(directTable) == nullptr &&
                directBarcode->toolTip().startsWith(QStringLiteral("Barcode recognition")) &&
                directTable->toolTip() == shortcutTooltip(QStringLiteral("Table recognition"),
                                                          {QStringLiteral("Ctrl+X")}),
            "runtime replacement must unstack Table and Barcode into stable direct controls");
    directTable->click();
    require(tableRequests == 2 && snow_shot::storage::ScreenshotToolbarSettings().tableQrTool() ==
                                      QStringLiteral("table"),
            "an independently placed Table control must dispatch and persist its mode");

    ScreenshotToolPalette::Options unavailableOptions;
    unavailableOptions.showSelectTool = false;
    unavailableOptions.showShapeTool = false;
    unavailableOptions.showScreenRecordButton = true;
    unavailableOptions.enableStyleToolbar = false;
    unavailableOptions.actionToolsLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("table-recognition"), QStringLiteral("record-screen")}},
        {QStringLiteral("barcode-recognition"), QStringLiteral("pin-to-screen"),
         QStringLiteral("text-recognition"), QStringLiteral("text-translation"),
         QStringLiteral("scrolling-screenshot"), QStringLiteral("quick-save"),
         QStringLiteral("save-as-file")},
    };
    ScreenshotToolPalette unavailablePalette(unavailableOptions);
    const QList<adqt::widgets::AdButton*> availableActions =
        mainActionToolbarButtons(unavailablePalette);
    require(availableActions.size() == 1 &&
                availableActions.constFirst()->accessibleName() ==
                    QStringLiteral("Record screen") &&
                popoverForTrigger(availableActions.constFirst()) == nullptr,
            "unavailable configured actions must be skipped without leaving an empty group");

    const QStringList allActionIds{
        QStringLiteral("barcode-recognition"),  QStringLiteral("table-recognition"),
        QStringLiteral("record-screen"),        QStringLiteral("pin-to-screen"),
        QStringLiteral("text-recognition"),     QStringLiteral("text-translation"),
        QStringLiteral("scrolling-screenshot"), QStringLiteral("quick-save"),
        QStringLiteral("save-as-file"),
    };
    ScreenshotToolPalette::Options hiddenOptions;
    hiddenOptions.showSelectTool = true;
    hiddenOptions.showShapeTool = true;
    hiddenOptions.showHistoryActions = true;
    hiddenOptions.showTableTool = true;
    hiddenOptions.showQrTool = true;
    hiddenOptions.showScreenRecordButton = true;
    hiddenOptions.showOcrTool = true;
    hiddenOptions.showTextTranslationTool = true;
    hiddenOptions.showScrollingScreenshotTool = true;
    hiddenOptions.showSaveButton = true;
    hiddenOptions.enableStyleToolbar = false;
    hiddenOptions.actions = ScreenshotToolPalette::CancelAction | ScreenshotToolPalette::CopyAction;
    hiddenOptions.actionToolsLayout = snow_shot::storage::ScreenshotToolbarLayout{{}, allActionIds};
    ScreenshotToolPalette hiddenPalette(hiddenOptions);
    auto* redo =
        hiddenPalette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRedoButton"));
    QWidget* cancel = controlWithTooltip(hiddenPalette, "Cancel screenshot");
    require(mainActionToolbarButtons(hiddenPalette).isEmpty() && redo != nullptr &&
                cancel != nullptr &&
                hasSeparatorBetween(hiddenPalette.mainPanel()->layout(), redo, cancel),
            "all-hidden action layouts must retain drawing, history, and separated fixed results");

    const auto originalPersistedLayout = snow_shot::storage::ScreenshotToolbarSettings().layout(
        snow_shot::storage::ScreenshotToolbarLayoutKind::ActionTools);
    require(snow_shot::storage::ScreenshotToolbarSettings().setLayout(
                snow_shot::storage::ScreenshotToolbarLayoutKind::ActionTools,
                snow_shot::storage::ScreenshotToolbarLayout{{}, allActionIds}),
            "the scenario boundary fixture must persist an all-hidden screenshot layout");
    ScreenshotToolPalette unaffectedPalette(options);
    require(mainActionToolbarButtons(unaffectedPalette).size() == 4,
            "a generic palette owner must use only its supplied layout, not screenshot storage");
    require(
        snow_shot::storage::ScreenshotToolbarSettings().setLayout(
            snow_shot::storage::ScreenshotToolbarLayoutKind::ActionTools, originalPersistedLayout),
        "the scenario boundary fixture must restore the persisted screenshot layout");
}

void arrowAndLineRemainDirectWhenConfiguredIndividually() {
    ScreenshotToolPalette::Options arrowOptions;
    arrowOptions.showSelectTool = false;
    arrowOptions.showShapeTool = false;
    arrowOptions.showArrowTool = true;
    arrowOptions.enableStyleToolbar = false;
    ScreenshotToolPalette arrowPalette(arrowOptions);
    const QList<adqt::widgets::AdButton*> arrowButtons = mainToolbarButtons(arrowPalette);
    require(arrowButtons.size() == 1 &&
                arrowButtons.constFirst()->toolTip() == QStringLiteral("Arrow (2)") &&
                popoverForTrigger(arrowButtons.constFirst()) == nullptr,
            "Arrow should remain a direct button when Line is unavailable");

    ScreenshotToolPalette::Options lineOptions;
    lineOptions.showSelectTool = false;
    lineOptions.showShapeTool = false;
    lineOptions.showArrowTool = false;
    lineOptions.showLineTool = true;
    lineOptions.enableStyleToolbar = false;
    ScreenshotToolPalette linePalette(lineOptions);
    const QList<adqt::widgets::AdButton*> lineButtons = mainToolbarButtons(linePalette);
    require(lineButtons.size() == 1 &&
                lineButtons.constFirst()->toolTip() == QStringLiteral("Line") &&
                popoverForTrigger(lineButtons.constFirst()) == nullptr,
            "Line should remain a direct button when Arrow is unavailable");
}

void confirmActionRemainsSeparatedAndCallableForPinnedEditing() {
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showEraserTool = true;
    options.showFilterTool = true;
    options.showWatermarkTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showSaveButton = true;
    options.saveButtonWithResultActions = true;
    options.copyButtonWithNeutralIcon = true;
    options.separatorAfterSelect = true;
    options.separatorBeforeConfirm = true;
    options.enableStyleToolbar = false;
    options.actions = ScreenshotToolPalette::CopyAction | ScreenshotToolPalette::ConfirmAction;

    ScreenshotToolPalette palette(options);
    adqt::widgets::AdButton* save = nullptr;
    for (auto* button : mainToolbarButtons(palette)) {
        if (button->accessibleName() == QStringLiteral("Save as file"))
            save = button;
    }
    auto* copy =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Copy to clipboard"));
    auto* confirm =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Confirm edit"));
    require(save != nullptr && copy != nullptr && confirm != nullptr,
            "pinned editing should expose Save, Copy, and Confirm actions");

    const QList<adqt::widgets::AdButton*> buttons = mainToolbarButtons(palette);
    require(buttons.size() >= 3 && buttons.at(buttons.size() - 3) == save &&
                buttons.at(buttons.size() - 2) == copy && buttons.constLast() == confirm,
            "pinned result actions should end with Save, Copy, and Confirm in that order");
    QLayout* layout = palette.mainPanel()->layout();
    const auto hasSeparatorBetween = [layout](QWidget* first, QWidget* second) {
        const int firstIndex = layout->indexOf(first);
        const int secondIndex = layout->indexOf(second);
        for (int index = firstIndex + 1; index < secondIndex; ++index) {
            if (qobject_cast<QFrame*>(layout->itemAt(index)->widget()) != nullptr) {
                return true;
            }
        }
        return false;
    };
    require(!hasSeparatorBetween(save, copy) && hasSeparatorBetween(copy, confirm),
            "pinned Save and Copy should share a section before the Confirm divider");

    require(confirm->buttonStyle() == copy->buttonStyle() &&
                confirm->accentRole() == copy->accentRole() && copy->iconRef().colors().isEmpty(),
            "pinned Copy should use the neutral icon color");
    require(save->buttonStyle() == copy->buttonStyle() &&
                save->accentRole() == copy->accentRole() &&
                adqt::icons::describeIcon(save->iconRef()).key.name == QStringLiteral("save") &&
                adqt::icons::describeIcon(copy->iconRef()).key.name == QStringLiteral("copy") &&
                adqt::icons::describeIcon(confirm->iconRef()).key.name == QStringLiteral("check"),
            "pinned result actions should retain the established styles and icons");
    require(controlWithTooltip(palette, "Cancel screenshot") == nullptr &&
                controlWithTooltip(palette, "Pin to screen") == nullptr,
            "pinned result actions should not add cancel or pin controls");
    int saveRequests = 0;
    int copyRequests = 0;
    int confirmRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::saveRequested,
                     [&saveRequests]() { ++saveRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::copyRequested,
                     [&copyRequests]() { ++copyRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::confirmRequested,
                     [&confirmRequests]() { ++confirmRequests; });
    save->click();
    copy->click();
    confirm->click();
    require(saveRequests == 1 && copyRequests == 1 && confirmRequests == 1,
            "each pinned result action should emit exactly once per click");
}

void ocrControlReflectsLoadingState() {
    ScreenshotToolPalette::Options options;
    options.showOcrTool = true;
    options.showTextTranslationTool = true;
    options.showTableTool = true;
    options.enableStyleToolbar = false;

    ScreenshotToolPalette palette(options);
    auto* ocrButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Text recognition"));
    require(ocrButton != nullptr, "OCR toolbar control should be present");
    require(ocrButton->busyIndicatorPresentation() ==
                adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface,
            "OCR animation should use an isolated presentation surface");

    int ocrRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::ocrRequested,
                     [&ocrRequests]() { ++ocrRequests; });
    ocrButton->click();

    require(ocrRequests == 1, "OCR toolbar control should remain callable");
    palette.setOcrBusy(true);
    require(ocrButton->busy(),
            "OCR toolbar control should enter a loading state while recognizing");
    palette.setOcrBusy(false);
    require(!ocrButton->busy(),
            "OCR toolbar control should leave its loading state after recognition");

    auto* translationButton = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotTextTranslationButton"));
    require(translationButton != nullptr, "text translation toolbar control should be present");
    translationButton->click();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::TextTranslation &&
                translationButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                ocrButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text,
            "text translation should have its own active toolbar presentation");
    palette.setOcrBusy(true);
    require(translationButton->busy() && !ocrButton->busy(),
            "text translation should show recognition loading on its active toolbar control");
    palette.setOcrBusy(false);
    palette.setTextTranslationState(true, true, true);
    require(translationButton->busy(),
            "text translation should keep loading while translation is streaming");
    palette.setTextTranslationState(true, true, false);
    require(!translationButton->busy(),
            "text translation should stop loading after translation completes");

    ocrButton->click();
    palette.setTextEditingState(true, false);
    palette.setTextTranslationState(true, false, true);
    require(translationButton->busy() && !ocrButton->busy() &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Ocr,
            "background translation must stay busy without changing the active OCR tool");
    const QPointF center = translationButton->rect().center();
    const QPointF globalCenter = translationButton->mapToGlobal(center.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, center, globalCenter, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, center, globalCenter, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(translationButton, &press);
    QCoreApplication::sendEvent(translationButton, &release);
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::TextTranslation &&
                translationButton->busy(),
            "busy translation must remain accessible by mouse");
    ocrButton->click();
    QKeyEvent keyPress(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent keyRelease(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(translationButton, &keyPress);
    QCoreApplication::sendEvent(translationButton, &keyRelease);
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::TextTranslation,
            "busy translation must remain accessible by keyboard");
    palette.setTextTranslationState(true, true, false);

    auto* tableButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Table recognition"));
    require(tableButton != nullptr, "table recognition should be an independent toolbar control");
    int tableRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::tableRequested,
                     [&tableRequests]() { ++tableRequests; });
    tableButton->click();
    require(tableRequests == 1, "table recognition should remain independently callable");
    palette.setTableBusy(true);
    require(tableButton->busy() && !ocrButton->busy(),
            "table recognition should expose a busy state independent from OCR");
    palette.setTableBusy(false);
}

void scrollingSelectionButtonsDragAndLockAxis() {
    ScreenshotToolPalette::Options options;
    options.showScrollingScreenshotTool = true;
    ScreenshotToolPalette palette(options);
    palette.setScrollingScreenshotMode(true);
    palette.show();
    QCoreApplication::processEvents();
    auto* horizontal = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotScrollingMoveHorizontalButton"));
    auto* vertical = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotScrollingMoveVerticalButton"));
    auto* separator =
        palette.findChild<QWidget*>(QStringLiteral("screenshotScrollingMovementSeparator"));
    require(horizontal && vertical && separator, "movement controls and separator must exist");
    require(!horizontal->isEnabled() && vertical->isEnabled(),
            "vertical mode must lock horizontal movement");
    auto* layout = separator->parentWidget()->layout();
    require(layout->itemAt(8)->widget() == separator &&
                layout->itemAt(10)->widget() == horizontal &&
                layout->itemAt(12)->widget() == vertical,
            "movement controls must follow direction controls");
    require(!vertical->toolTip().isEmpty() && !vertical->accessibleName().isEmpty(),
            "movement button must explain its interaction accessibly");
    auto& languages = snow_shot::presentation::LanguageManager::instance();
    require(languages.setLanguage(QStringLiteral("en_US")), "load English movement labels");
    const QString english = vertical->toolTip();
    for (const auto& language : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        require(languages.setLanguage(language), "load translated movement labels");
        QCoreApplication::processEvents();
        require(vertical->toolTip() != english && vertical->accessibleName() == vertical->toolTip(),
                "movement tooltip and accessible name must retranslate together");
    }
    require(languages.setLanguage(QStringLiteral("en_US")), "restore English labels");
    QCoreApplication::processEvents();
    int starts = 0, updates = 0, finishes = 0;
    QPoint latest;
    ScreenshotScrollingRecognitionMode axis = ScreenshotScrollingRecognitionMode::Horizontal;
    QObject::connect(&palette, &ScreenshotToolPalette::scrollingSelectionMoveStarted,
                     [&](ScreenshotScrollingRecognitionMode value, QPoint position) {
                         ++starts;
                         axis = value;
                         latest = position;
                     });
    QObject::connect(&palette, &ScreenshotToolPalette::scrollingSelectionMoveUpdated,
                     [&](QPoint position) {
                         ++updates;
                         latest = position;
                     });
    QObject::connect(&palette, &ScreenshotToolPalette::scrollingSelectionMoveFinished,
                     [&] { ++finishes; });
    const auto send = [](QWidget* button, QEvent::Type type, QPoint position) {
        const auto changed = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, QPointF(button->mapFromGlobal(position)), QPointF(position),
                          changed, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(button, &event);
    };
    send(horizontal, QEvent::MouseButtonPress, QPoint(30, 40));
    require(starts == 0, "disabled axis must not start a gesture");
    send(vertical, QEvent::MouseButtonPress, QPoint(30, 40));
    require(starts == 1 && axis == ScreenshotScrollingRecognitionMode::Vertical &&
                vertical->isDown(),
            "press must start movement immediately");
    send(vertical, QEvent::MouseMove, QPoint(-500, 900));
    require(updates == 1 && latest == QPoint(-500, 900),
            "movement outside the button must use global coordinates");
    send(vertical, QEvent::MouseButtonRelease, QPoint(-500, 910));
    require(finishes == 1 && updates == 2 && !vertical->isDown(),
            "release outside must end the gesture");
    send(vertical, QEvent::MouseButtonPress, QPoint(30, 40));
    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(vertical, &ungrab);
    require(finishes == 2, "lost mouse grab must balance movement");
    send(vertical, QEvent::MouseButtonPress, QPoint(30, 40));
    palette.setScrollingRecognitionMode(ScreenshotScrollingRecognitionMode::Horizontal);
    require(finishes == 3 && horizontal->isEnabled() && !vertical->isEnabled(),
            "direction changes must end dragging and swap the locked axis");
    send(horizontal, QEvent::MouseButtonPress, QPoint(30, 40));
    horizontal->hide();
    require(finishes == 4, "hiding an active control must end dragging");
    horizontal->show();
    send(horizontal, QEvent::MouseButtonPress, QPoint(30, 40));
    palette.setScrollingScreenshotMode(false);
    require(finishes == 5, "session exit must end dragging once");
}

void scrollingScreenshotExposesAxisRecognitionModes() {
    ScreenshotToolPalette::Options options;
    options.showScrollingScreenshotTool = true;
    options.showOcrTool = true;
    ScreenshotToolPalette palette(options);

    palette.setActiveTool(ScreenshotToolPalette::Tool::Ocr);
    QCoreApplication::processEvents();
    const int textRecognitionToolbarHeight = palette.actionPanel()->height();

    int changes = 0;
    ScreenshotScrollingRecognitionMode lastMode = ScreenshotScrollingRecognitionMode::Vertical;
    QObject::connect(&palette, &ScreenshotToolPalette::scrollingRecognitionModeChanged,
                     [&changes, &lastMode](ScreenshotScrollingRecognitionMode mode) {
                         ++changes;
                         lastMode = mode;
                     });

    palette.setScrollingScreenshotMode(true);
    QCoreApplication::processEvents();
    QWidget* controls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotScrollingRecognitionMode"));
    const auto modeButtons = controls != nullptr ? controls->findChildren<adqt::widgets::AdButton*>(
                                                       QString(), Qt::FindDirectChildrenOnly)
                                                 : QList<adqt::widgets::AdButton*>();
    auto* verticalButton = controls != nullptr
                               ? controls->findChild<adqt::widgets::AdButton*>(
                                     QStringLiteral("screenshotScrollingVerticalButton"))
                               : nullptr;
    auto* horizontalButton = controls != nullptr
                                 ? controls->findChild<adqt::widgets::AdButton*>(
                                       QStringLiteral("screenshotScrollingHorizontalButton"))
                                 : nullptr;
    require(controls != nullptr &&
                controls->findChild<adqt::widgets::AdRadioButtonGroup*>() == nullptr &&
                modeButtons.size() == 5 && verticalButton != nullptr && horizontalButton != nullptr,
            "scrolling screenshot should expose two independent mode buttons");
    auto* autoScroll = controls->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotScrollingAutoScrollButton"));
    auto* separator =
        controls->findChild<QWidget*>(QStringLiteral("screenshotScrollingAutoScrollSeparator"));
    require(autoScroll != nullptr && separator != nullptr && !autoScroll->isCheckable() &&
                !autoScroll->isChecked(),
            "auto-scroll must use the same non-checkable action button as the axis controls");
    const auto requireAutoScrollStyle = [&](bool active) {
        require(!autoScroll->isCheckable() && !autoScroll->isChecked() &&
                    autoScroll->buttonStyle() ==
                        (active ? adqt::widgets::AdButton::ButtonStyle::Solid
                                : adqt::widgets::AdButton::ButtonStyle::Text) &&
                    autoScroll->accentRole() ==
                        (active ? adqt::widgets::AdButton::AccentRole::Primary
                                : adqt::widgets::AdButton::AccentRole::Neutral),
                "auto-scroll must use palette-driven active styling like the axis controls");
    };
    requireAutoScrollStyle(false);
    require(controls->layout()->itemAt(0)->widget() == autoScroll &&
                controls->layout()->itemAt(2)->widget() == separator &&
                controls->layout()->itemAt(4)->widget() == verticalButton,
            "auto-scroll must be the leftmost control with a separator to its right");
    const auto requireSeparatorSpacing = [&](int groupSpacing, int buttonSpacing) {
        controls->layout()->activate();
        require(separator->x() - (autoScroll->x() + autoScroll->width()) == groupSpacing &&
                    verticalButton->x() - (separator->x() + separator->width()) == groupSpacing,
                "scrolling separator must match the selection toolbar's spacing on both sides");
        require(horizontalButton->x() - (verticalButton->x() + verticalButton->width()) ==
                    buttonSpacing,
                "axis buttons must retain their compact item spacing");
    };
    requireSeparatorSpacing(16, 4);
    int autoScrollChanges = 0;
    bool autoScrollEnabled = false;
    QObject::connect(&palette, &ScreenshotToolPalette::scrollingAutoScrollChanged,
                     [&](bool enabled) {
                         ++autoScrollChanges;
                         autoScrollEnabled = enabled;
                     });
    autoScroll->click();
    require(autoScrollChanges == 1 && autoScrollEnabled,
            "activating auto-scroll must emit once and show its active state");
    requireAutoScrollStyle(true);
    require(buttonBackgroundSample(*autoScroll) == buttonBackgroundSample(*verticalButton),
            "active auto-scroll and axis buttons must use the same background");
    require(
        imageHasOpaqueLightPixel(
            autoScroll->icon().pixmap(autoScroll->iconSize(), QIcon::Normal, QIcon::Off).toImage()),
        "active auto-scroll must use the shared light icon foreground");
    autoScroll->click();
    require(autoScrollChanges == 2 && !autoScrollEnabled,
            "deactivating auto-scroll must emit the stop command");
    requireAutoScrollStyle(false);
    autoScroll->click();
    require(!palette.actionPanel()->isHidden() && palette.stylePanel()->isHidden() &&
                !controls->isHidden(),
            "scrolling recognition modes should occupy the attached action toolbar");
    require(palette.actionPanel()->height() == textRecognitionToolbarHeight,
            "scrolling screenshot toolbar should match the text recognition toolbar height");
    require(palette.scrollingRecognitionMode() == ScreenshotScrollingRecognitionMode::Vertical &&
                !verticalButton->isCheckable() && !horizontalButton->isCheckable(),
            "vertical scrolling recognition should be the session default");
    require(verticalButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                verticalButton->accentRole() == adqt::widgets::AdButton::AccentRole::Primary &&
                horizontalButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                horizontalButton->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral,
            "the active scrolling mode should be visually distinct");
    auto* scrollingToolButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Scrolling screenshot"));
    require(scrollingToolButton != nullptr && buttonBackgroundSample(*verticalButton) ==
                                                  buttonBackgroundSample(*scrollingToolButton),
            "the active scrolling mode should match the main toolbar active background");
    require(
        imageHasOpaqueLightPixel(verticalButton->icon()
                                     .pixmap(verticalButton->iconSize(), QIcon::Normal, QIcon::On)
                                     .toImage()),
        "the active scrolling mode icon should use the main toolbar foreground color");
    for (adqt::widgets::AdButton* button : modeButtons) {
        require(button != nullptr && !button->toolTip().isEmpty() &&
                    button->accessibleName() == button->toolTip(),
                "scrolling mode buttons should expose translated tooltip accessibility");
        require(button->size() == QSize(32, 32) && button->iconSize() == QSize(24, 24),
                "scrolling mode buttons should use the enlarged action toolbar metrics");
    }
    require(palette.setPhysicalScale(1.5),
            "scrolling screenshot toolbar should accept a physical scale change");
    requireSeparatorSpacing(24, 6);
    for (adqt::widgets::AdButton* button : modeButtons) {
        require(button->size() == QSize(48, 48) && button->iconSize() == QSize(36, 36),
                "scrolling mode buttons should retain their enlarged metrics after scaling");
    }

    palette.setScrollingRecognitionMode(ScreenshotScrollingRecognitionMode::Vertical);
    require(changes == 0, "setting the current scrolling mode should be a no-op");
    horizontalButton->click();
    require(autoScrollEnabled && autoScrollChanges == 3,
            "switching the scroll axis must preserve auto-scroll activation");
    requireAutoScrollStyle(true);
    require(changes == 1 && lastMode == ScreenshotScrollingRecognitionMode::Horizontal &&
                verticalButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                horizontalButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
            "clicking the horizontal button should synchronize both mode buttons once");
    require(
        imageHasOpaqueLightPixel(horizontalButton->icon()
                                     .pixmap(horizontalButton->iconSize(), QIcon::Normal, QIcon::On)
                                     .toImage()),
        "switching scrolling modes should update the active icon foreground color");
    horizontalButton->click();
    require(changes == 1 &&
                horizontalButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
            "clicking the active mode should keep it selected without another change");

    palette.setScrollingScreenshotMode(false);
    require(!autoScrollEnabled && autoScrollChanges == 4,
            "leaving scrolling capture must stop auto-scroll");
    palette.setScrollingScreenshotMode(true);
    autoScroll = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotScrollingAutoScrollButton"));
    require(autoScroll != nullptr && !autoScroll->isChecked(),
            "a new scrolling capture must reset auto-scroll activation");
    requireAutoScrollStyle(false);
    controls = palette.findChild<QWidget*>(QStringLiteral("screenshotScrollingRecognitionMode"));
    verticalButton = controls != nullptr ? controls->findChild<adqt::widgets::AdButton*>(
                                               QStringLiteral("screenshotScrollingVerticalButton"))
                                         : nullptr;
    horizontalButton = controls != nullptr
                           ? controls->findChild<adqt::widgets::AdButton*>(
                                 QStringLiteral("screenshotScrollingHorizontalButton"))
                           : nullptr;
    require(palette.scrollingRecognitionMode() == ScreenshotScrollingRecognitionMode::Vertical &&
                verticalButton != nullptr && horizontalButton != nullptr &&
                verticalButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                horizontalButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text,
            "each new scrolling screenshot session should reset to vertical recognition");
}

void originalImageToggleLeadsRecognitionActions() {
    using Tool = ScreenshotToolPalette::Tool;
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showOcrTool = true;
    options.showQrTool = true;
    options.showTableTool = true;
    options.showImageConversionTools = true;
    ScreenshotToolPalette palette(options);
    palette.show();
    int requests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::showOriginalImageRequested, &palette,
                     [&](bool show) {
                         ++requests;
                         palette.setShowOriginalImage(show);
                     });
    for (const auto tool :
         {Tool::Qr, Tool::Ocr, Tool::Markdown, Tool::Table, Tool::Html, Tool::Qr}) {
        palette.setShowOriginalImage(false);
        palette.setActiveTool(tool);
        QCoreApplication::processEvents();
        auto* button = palette.findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotShowOriginalImageButton"));
        require(button && button->isVisible() && button->isEnabled() &&
                    palette.actionToolbarVisible(),
                "every recognition tool exposes original-image toggle");
        require(palette.actionPanel()->layout()->itemAt(0)->widget() == button,
                "original-image toggle remains first after lazy toolbar creation");
        require(button->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral,
                "original-image toggle starts inactive");
        require(adqt::icons::describeIcon(button->iconRef()).key ==
                    adqt::icons::describeIcon(adqt::icons::antd::outlined::Eye()).key,
                "original-image toggle uses Ant Design outlined Eye");
        require(button->accessibleName() == QStringLiteral("Show original image"),
                "original-image toggle has an accessible label");
        button->click();
        require(button->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
                "activation uses the shared selected appearance");
        button->click();
        require(button->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral,
                "a second click restores inactive appearance");
    }
    require(requests == 12, "each toggle click dispatches exactly one request");
    palette.setActiveTool(Tool::Select);
    auto* button = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotShowOriginalImageButton"));
    require(button == nullptr || !button->isVisible(),
            "selection toolbar does not show the toggle");
}

void imageConversionToolsExposeRecognitionActions() {
    require(snow_shot::storage::ScreenshotToolbarSettings().setTableQrTool(QStringLiteral("qr")),
            "recognition group fixture starts with the remembered barcode entry");
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showQrTool = true;
    options.showTableTool = true;
    options.showImageConversionTools = true;
    ScreenshotToolPalette palette(options);
    palette.show();
    auto* markdownSource = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotConvertToMarkdownButton"));
    auto* htmlSource = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotConvertToHtmlButton"));
    auto* qr =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableQrButton"));
    auto* group = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotActionToolGroupButton0"));
    require(
        markdownSource && htmlSource && qr && group && markdownSource->isHidden() &&
            htmlSource->isHidden() && qr->isHidden() && !group->isHidden() &&
            group->property("screenshotToolbarPositionItems").toStringList() ==
                QStringList{
                    QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown"),
                    QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")} &&
            group->accessibleName() == QStringLiteral("Table recognition"),
        "the recognition group initially shows the bottom tool even with a remembered QR entry");
    static_cast<void>(palette.sizeHint());
    QCoreApplication::processEvents();
    materializeLazyPopover(group);
    auto* popover = popoverForTrigger(group);
    auto* markdown = popoverButtonWithTooltip(popover, "Convert to Markdown");
    auto* html = popoverButtonWithTooltip(popover, "Convert to HTML");
    const auto reopenConversionOptions = [&]() {
        materializeLazyPopover(group);
        markdown = popoverButtonWithTooltip(popover, "Convert to Markdown");
        html = popoverButtonWithTooltip(popover, "Convert to HTML");
        require(markdown != nullptr && html != nullptr,
                "reopening the recognition group should recreate conversion options");
    };
    require(markdown && html && popoverButtonWithTooltip(popover, "Table recognition") &&
                popoverButtonWithTooltip(popover, "Barcode recognition"),
            "the recognition popover exposes all four tools");
    QStringList popoverItems;
    auto* content = popover->contentWidget();
    for (auto* button : content->findChildren<adqt::widgets::AdButton*>()) {
        const QString itemId = button->property("screenshotToolbarItemId").toString();
        if (!itemId.isEmpty()) {
            popoverItems.push_back(itemId);
        }
    }
    require(popoverItems == QStringList{QStringLiteral("table-recognition"),
                                        QStringLiteral("barcode-recognition"),
                                        QStringLiteral("convert-to-markdown"),
                                        QStringLiteral("convert-to-html")},
            "recognition popover follows the configured stack from bottom to top");
    for (auto* button : {markdownSource, markdown, htmlSource, html}) {
        const auto key = adqt::icons::describeIcon(button->iconRef()).key;
        require(key.pack == QStringLiteral("snow-shot") &&
                    key.name == ((button == markdownSource || button == markdown)
                                     ? QStringLiteral("markdown")
                                     : QStringLiteral("html")),
                "source and popover buttons use the supplied Markdown and HTML artwork");
    }
    int markdownRequests = 0;
    int htmlRequests = 0;
    int settingsRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::markdownRequested, &palette,
                     [&]() { ++markdownRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::htmlRequested, &palette,
                     [&]() { ++htmlRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::imageConversionSettingsRequested, &palette,
                     [&]() { ++settingsRequests; });
    palette.setQrEnabled(false);
    palette.setTableEnabled(false);
    require(group->isEnabled() && markdown->isEnabled() && html->isEnabled() &&
                !popoverButtonWithTooltip(popover, "Barcode recognition")->isEnabled() &&
                !popoverButtonWithTooltip(popover, "Table recognition")->isEnabled(),
            "conversion options remain reachable when the selected barcode and table tools are "
            "unavailable");
    palette.setImageConversionEnabled(false);
    require(!group->isEnabled(),
            "the recognition group disables only when all options are unavailable");
    palette.setImageConversionEnabled(true);
    markdown->click();
    require(markdownRequests == 1 &&
                palette.activeTool() == ScreenshotToolPalette::Tool::Markdown &&
                group->accessibleName() == QStringLiteral("Convert to Markdown") &&
                adqt::icons::describeIcon(group->iconRef()).key.name == QStringLiteral("markdown"),
            "Markdown activates its own recognition tool");
    palette.setQrEnabled(true);
    palette.setTableEnabled(true);
    auto* settings = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotImageConversionSettingsButton"));
    require(settings && !settings->isHidden(), "conversion activation exposes Settings");
    for (auto* button : palette.actionPanel()->findChildren<adqt::widgets::AdButton*>()) {
        require(button == settings ||
                    button->objectName() == QStringLiteral("screenshotShowOriginalImageButton") ||
                    button->isHidden(),
                "conversion sub-toolbar contains original-image toggle and Settings");
    }
    settings->click();
    require(settingsRequests == 1, "Settings routes to conversion settings");
    reopenConversionOptions();
    palette.setImageConversionBusy(true, false);
    require(
        markdown->busy() && group->busy() && !html->busy() && settings->isEnabled(),
        "the group and active option show busy while Settings and other formats remain available");
    html->click();
    require(htmlRequests == 1 && palette.activeTool() == ScreenshotToolPalette::Tool::Html &&
                !settings->isHidden() && !group->busy() &&
                group->accessibleName() == QStringLiteral("Convert to HTML") &&
                adqt::icons::describeIcon(group->iconRef()).key.name == QStringLiteral("html"),
            "HTML switches format and retains the conversion sub-toolbar");
    const QString snapshots = qEnvironmentVariable("SNOW_SHOT_CONVERSION_SNAPSHOTS");
    if (!snapshots.isEmpty()) {
        require(QDir().mkpath(snapshots), "create conversion toolbar snapshot directory");
        const auto previousTheme = adqt::theme::ThemeManager::instance().config();
        auto& appTheme = snow_shot::presentation::styles::ThemeManager::instance();
        const auto previousMode = appTheme.themeMode();
        for (const bool dark : {false, true}) {
            appTheme.setThemeMode(dark ? snow_shot::presentation::styles::ThemeMode::Dark
                                       : snow_shot::presentation::styles::ThemeMode::Light);
            adqt::theme::ThemeManager::instance().applyTo(*qApp);
            palette.setImageConversionBusy(false, false);
            static_cast<void>(showPopoverForTrigger(group));
            const QString suffix = dark ? QStringLiteral("dark") : QStringLiteral("light");
            require(palette.grab().save(QDir(snapshots).filePath(
                        QStringLiteral("conversion-toolbar-%1.png").arg(suffix))) &&
                        popover->contentWidget()->grab().save(QDir(snapshots).filePath(
                            QStringLiteral("recognition-group-%1.png").arg(suffix))),
                    "save conversion toolbar and recognition group snapshots");
            popover->hide();
        }
        appTheme.setThemeMode(previousMode);
        adqt::theme::ThemeManager::instance().setConfig(previousTheme);
        adqt::theme::ThemeManager::instance().applyTo(*qApp);
    }
    reopenConversionOptions();
    auto& language = snow_shot::presentation::LanguageManager::instance();
    require(language.setLanguage(QStringLiteral("zh_CN")),
            "load Simplified Chinese toolbar labels");
    QCoreApplication::processEvents();
    require(palette.findChild<adqt::widgets::AdButton*>(
                       QStringLiteral("screenshotShowOriginalImageButton"))
                    ->accessibleName() == QStringLiteral("显示原图"),
            "original-image button retranslates to Simplified Chinese");
    require(markdown->accessibleName() == QStringLiteral("转换为 Markdown") &&
                html->accessibleName() == QStringLiteral("转换为 HTML") &&
                group->accessibleName() == QStringLiteral("转换为 HTML") &&
                settings->toolTip() == QStringLiteral("设置"),
            "conversion buttons retranslate to Simplified Chinese");
    require(language.setLanguage(QStringLiteral("zh_TW")),
            "load Traditional Chinese toolbar labels");
    QCoreApplication::processEvents();
    require(palette.findChild<adqt::widgets::AdButton*>(
                       QStringLiteral("screenshotShowOriginalImageButton"))
                    ->accessibleName() == QStringLiteral("顯示原圖"),
            "original-image button retranslates to Traditional Chinese");
    require(markdown->accessibleName() == QStringLiteral("轉換為 Markdown") &&
                html->accessibleName() == QStringLiteral("轉換為 HTML") &&
                group->accessibleName() == QStringLiteral("轉換為 HTML") &&
                settings->toolTip() == QStringLiteral("設定"),
            "conversion buttons retranslate to Traditional Chinese");
    require(language.setLanguage(QStringLiteral("en_US")), "restore English toolbar labels");
    QCoreApplication::processEvents();
    palette.setImageConversionBusy(false, false);
    group->click();
    require(!palette.activeTool().has_value() ||
                palette.activeTool() == ScreenshotToolPalette::Tool::Select,
            "clicking the selected conversion group trigger toggles recognition off");
    group->click();
    require(htmlRequests == 2 && palette.activeTool() == ScreenshotToolPalette::Tool::Html,
            "the group trigger remembers and reactivates the selected conversion format");
    settings = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotImageConversionSettingsButton"));
    const QPointer<adqt::widgets::AdButton> settingsGuard(settings);
    reopenConversionOptions();
    palette.setImageConversionEnabled(false);
    require(group->isEnabled() && !markdown->isEnabled() && !html->isEnabled(),
            "barcode and table remain reachable when the selected conversion is unavailable");
    group->click();
    require(htmlRequests == 2,
            "the enabled group trigger cannot dispatch its disabled conversion entry");
    reopenConversionOptions();
    auto* barcodeOption = popoverButtonWithTooltip(popover, "Barcode recognition");
    require(barcodeOption != nullptr,
            "reopening the recognition group should recreate Barcode recognition");
    barcodeOption->click();
    require(
        palette.activeTool() == ScreenshotToolPalette::Tool::Qr,
        "the recognition group can switch back to barcode after conversion becomes unavailable");
    require(settingsGuard == nullptr || settingsGuard->isHidden(),
            "leaving conversion removes its Settings control");
}

void ocrToolReplacesSelectionActionToolbarContents() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showOcrTool = true;
    options.showHistoryActions = true;
    ScreenshotToolPalette palette(options);

    require(palette.actionPanel() != nullptr,
            "selection and OCR tools should expose an action panel");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    require(palette.findChild<QWidget*>(QStringLiteral("screenshotSelectionOpacityIcon")) !=
                nullptr,
            "Select should materialize selection actions before OCR is requested");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Ocr);
    QWidget* actionPanel = palette.actionPanel();
    const auto actionButtons =
        actionPanel->findChildren<adqt::widgets::AdButton*>(QString(), Qt::FindDirectChildrenOnly);
    const auto buttonWithTooltip = [&actionButtons](const char* tooltip) {
        const QString translated = QString::fromUtf8(tooltip);
        const auto found = std::find_if(actionButtons.cbegin(), actionButtons.cend(),
                                        [&translated](const adqt::widgets::AdButton* button) {
                                            return button->toolTip() == translated;
                                        });
        return found != actionButtons.cend() ? *found : nullptr;
    };
    adqt::widgets::AdButton* sendToBack = buttonWithTooltip("Send to back");
    adqt::widgets::AdButton* edit = buttonWithTooltip("Edit");
    adqt::widgets::AdButton* translate = buttonWithTooltip("Text translation");
    adqt::widgets::AdButton* reset = buttonWithTooltip("Reset");
    adqt::widgets::AdButton* settings = buttonWithTooltip("Translation settings");
    auto* undo =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotUndoButton"));
    auto* redo =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRedoButton"));
    const auto textSelects =
        actionPanel->findChildren<adqt::widgets::AdSelect*>(QString(), Qt::FindDirectChildrenOnly);
    auto* formattingSelect = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotOcrTextFormattingSelect"));
    auto* punctuationSelect = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotOcrTextPunctuationSelect"));
    require(sendToBack == nullptr && edit != nullptr && translate != nullptr && reset != nullptr &&
                settings != nullptr && undo != nullptr && redo != nullptr &&
                textSelects.size() == 2 && formattingSelect != nullptr &&
                punctuationSelect != nullptr,
            "the shared action panel should contain the OCR editing controls");
    require(
        undo->toolTip() == shortcutTooltip(QStringLiteral("Undo"), {QStringLiteral("Ctrl+Z")}) &&
            redo->toolTip() == shortcutTooltip(QStringLiteral("Redo"), {QStringLiteral("Ctrl+Y")}),
        "toolbar history actions must show their configured shortcuts");
    require(!edit->isHidden() && !translate->isHidden() && !reset->isHidden() &&
                !settings->isHidden() && !textSelects.at(0)->isHidden() &&
                !textSelects.at(1)->isHidden(),
            "OCR mode should replace selection actions with text editing controls");
    QLayout* actionLayout = actionPanel->layout();
    require(
        actionLayout != nullptr && actionLayout->indexOf(edit) < actionLayout->indexOf(translate) &&
            actionLayout->indexOf(translate) < actionLayout->indexOf(textSelects.at(0)) &&
            actionLayout->indexOf(textSelects.at(0)) < actionLayout->indexOf(textSelects.at(1)) &&
            actionLayout->indexOf(textSelects.at(1)) < actionLayout->indexOf(reset) &&
            actionLayout->indexOf(reset) < actionLayout->indexOf(settings),
        "OCR actions should be ordered Edit, Translate, formatting, punctuation, Reset, Settings");
    require(!edit->isEnabled() && !reset->isEnabled(),
            "OCR editing controls should remain disabled before a text result exists");
    require(!edit->isCheckable(),
            "the OCR Edit control should use the same visual state path as main toolbar tools");
    for (const adqt::widgets::AdSelect* select : textSelects) {
        require(select->variant() == adqt::widgets::AdSelect::Variant::Borderless,
                "OCR text selects should use the borderless toolbar variant");
        require(select->popupLayerMode() == adqt::widgets::AdSelect::PopupLayerMode::QtTool,
                "OCR text select popups should use the Qt tool layer");
    }
    palette.setTextEditingState(true, false);
    require(edit->isEnabled() && !reset->isEnabled() && textSelects.at(0)->isEnabled() &&
                textSelects.at(1)->isEnabled(),
            "a text result should enable editing operations but not Reset");
    formattingSelect->setCurrentValue(QStringLiteral("remove"));
    punctuationSelect->setCurrentValue(QStringLiteral("full"));
    require(formattingSelect->currentValue().toString() == QStringLiteral("remove") &&
                punctuationSelect->currentValue().toString() == QStringLiteral("full"),
            "active OCR transforms should remain displayed on both selects");
    palette.setTextTransformSelections({}, {});
    require(!formattingSelect->currentValue().isValid() &&
                !punctuationSelect->currentValue().isValid(),
            "published manual-edit state should clear both OCR transform selections");
    palette.setTextEditingState(true, true);
    palette.setTextTranslationState(true, false, false, false, false, false);
    require(edit->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                edit->accentRole() == adqt::widgets::AdButton::AccentRole::Primary &&
                reset->isEnabled(),
            "Edit Reset should remain enabled after translation state is published");
    palette.setTextTranslationState(true, false, true);
    require(reset->isEnabled() && formattingSelect->isEnabled() && punctuationSelect->isEnabled(),
            "background translation must not lock source OCR editing controls");
    palette.setTextTranslationState(true, false, false);
    auto* ocrToolButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Text recognition"));
    require(ocrToolButton != nullptr &&
                buttonBackgroundSample(*edit) == buttonBackgroundSample(*ocrToolButton),
            "the active OCR edit control should match the main toolbar active background");
    palette.setTextEditingState(true, true, true, false);
    require(undo->isEnabled() && !redo->isEnabled(),
            "OCR editing should expose the text document's undo state on the toolbar");
    palette.setTextEditingState(true, false);
    palette.setTextTranslationState(true, true, true, false, false, false);
    require(translate->isEnabled() &&
                translate->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                !reset->isEnabled() && !textSelects.at(0)->isEnabled() &&
                !textSelects.at(1)->isEnabled() && settings->isEnabled(),
            "streaming translation should remain dismissible while edits stay locked");
    palette.setTextTranslationState(true, true, false, true, false, true);
    require(reset->isEnabled() && undo->isEnabled() && !redo->isEnabled() &&
                formattingSelect->isEnabled() && punctuationSelect->isEnabled(),
            "completed translation should expose Reset, history, and text formatting");
    palette.setTextTranslationState(true, true, false, false, false, false, true);
    require(edit->isEnabled() && translate->isEnabled() && settings->isEnabled() &&
                !reset->isEnabled() && !undo->isEnabled() && !redo->isEnabled() &&
                !formattingSelect->isEnabled() && !punctuationSelect->isEnabled(),
            "overlay translation should retain source Edit and settings without translation "
            "editing controls");
    palette.setTextTranslationState(true, false, false, false, false, false);
    require(edit->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                edit->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral &&
                !reset->isEnabled(),
            "Edit should return to its inactive state after text editing exits");
}

void recognitionToolsKeepDrawingToolsAvailable() {
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showFilterTool = true;
    options.showEraserTool = true;
    options.showWatermarkTool = true;
    options.showOcrTool = true;
    options.showTableTool = true;
    options.showQrTool = true;
    options.enableStyleToolbar = false;

    ScreenshotToolPalette palette(options);
    const char* const drawingTools[] = {
        "Select elements", "Shape",  "Arrow",  "Line",      "Pen", "Text",
        "Serial number",   "Filter", "Eraser", "Watermark",
    };

    palette.setActiveTool(ScreenshotToolPalette::Tool::Ocr);
    requireControlsEnabled(palette, drawingTools, std::size(drawingTools), true,
                           "text recognition should keep drawing tools enabled");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Table);
    requireControlsEnabled(palette, drawingTools, std::size(drawingTools), true,
                           "table recognition should keep drawing tools enabled");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Qr);
    requireControlsEnabled(palette, drawingTools, std::size(drawingTools), true,
                           "QR recognition should keep drawing tools enabled");

    auto* shapeButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Shape"));
    require(shapeButton != nullptr, "shape tool should remain clickable during recognition");
    shapeButton->click();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape,
            "clicking a drawing tool should switch directly from recognition to that tool");
}

void clickingActiveToolbarToolReturnsToSelect() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showOcrTool = true;
    options.enableStyleToolbar = false;

    ScreenshotToolPalette palette(options);
    auto* shapeButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Shape"));
    auto* ocrButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Text recognition"));
    require(shapeButton != nullptr && ocrButton != nullptr,
            "toolbar tools should be available for toggle testing");

    int selectRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::selectRequested,
                     [&selectRequests]() { ++selectRequests; });

    shapeButton->click();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Shape,
            "the first drawing-tool click should activate that tool");
    shapeButton->click();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select &&
                selectRequests == 1,
            "clicking an active drawing tool should return to Select");

    ocrButton->click();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Ocr,
            "the first recognition-tool click should activate that tool");
    ocrButton->click();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select &&
                selectRequests == 2,
            "clicking an active recognition tool should return to Select");
}

void repeatingDrawingShortcutsReturnsToSelect() {
    using Tool = ScreenshotToolPalette::Tool;
    const snow_shot::storage::ScreenshotToolbarSettings toolbarSettings;
    const QString originalFilter = toolbarSettings.lastFilterTool();
    const QString originalHighlight = toolbarSettings.lastHighlightTool();
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(toolbarSettings.setLastFilterTool(originalFilter));
        static_cast<void>(toolbarSettings.setLastHighlightTool(originalHighlight));
    });
    // The generic entries activate the remembered drawing mode, which is persisted
    // toolbar state; pin the defaults so this test stays order-independent.
    require(toolbarSettings.setLastFilterTool(QStringLiteral("pen-filter")) &&
                toolbarSettings.setLastHighlightTool(QStringLiteral("pen-highlight")),
            "drawing shortcut tests must start from the default remembered modes");
    ScreenshotToolPalette::Options options;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showFilterTool = true;
    options.showEraserTool = true;
    options.showWatermarkTool = true;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette palette(options);
    int selectRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::selectRequested,
                     [&selectRequests]() { ++selectRequests; });

    const std::pair<const char*, Tool> shortcuts[] = {
        {"shape", Tool::Shape},         {"arrow", Tool::Arrow},
        {"brush", Tool::FreeDraw},      {"highlight", Tool::PenHighlight},
        {"text", Tool::Text},           {"serial_number", Tool::SerialNumber},
        {"filter", Tool::PenFilter},    {"eraser", Tool::Eraser},
        {"watermark", Tool::Watermark},
    };
    for (const auto& [id, tool] : shortcuts) {
        palette.setActiveTool(Tool::Select);
        require(palette.activateDrawingShortcut(QString::fromLatin1(id)) &&
                    palette.activeToolForTests() == tool,
                "a drawing shortcut should activate its tool");
        const int previousSelectRequests = selectRequests;
        require(palette.activateDrawingShortcut(QString::fromLatin1(id)) &&
                    palette.activeToolForTests() == Tool::Select &&
                    selectRequests == previousSelectRequests + 1,
                "repeating an active drawing shortcut should request Select exactly once");
        require(palette.activateDrawingShortcut(QString::fromLatin1(id)) &&
                    palette.activeToolForTests() == tool,
                "a drawing shortcut should reactivate its tool after toggling to Select");
        palette.setActiveTool(tool);
        require(palette.activeToolForTests() == tool,
                "programmatic tool synchronization should remain idempotent");
    }
    require(palette.activateDrawingShortcut(QStringLiteral("select")) &&
                palette.activateDrawingShortcut(QStringLiteral("select")) &&
                palette.activeToolForTests() == Tool::Select,
            "repeating the selection shortcut should keep Select active");
    require(!palette.activateDrawingShortcut(QStringLiteral("unknown")) &&
                palette.activeToolForTests() == Tool::Select,
            "unknown shortcuts should leave the active tool unchanged");
}

void repeatingActionShortcutsReturnsToSelect() {
    using Tool = ScreenshotToolPalette::Tool;
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showOcrTool = true;
    options.showTextTranslationTool = true;
    options.showTableTool = true;
    options.showQrTool = true;
    options.showScrollingScreenshotTool = true;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette palette(options);
    int selectRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::selectRequested,
                     [&selectRequests]() { ++selectRequests; });
    for (const Tool tool : {Tool::Move, Tool::Ocr, Tool::TextTranslation, Tool::Table, Tool::Qr,
                            Tool::ScrollingScreenshot}) {
        palette.setActiveTool(Tool::Select);
        require(palette.activateToolShortcut(tool) && palette.activeToolForTests() == tool,
                "an action shortcut should activate its tool");
        const int previousSelectRequests = selectRequests;
        require(palette.activateToolShortcut(tool) &&
                    palette.activeToolForTests() == Tool::Select &&
                    selectRequests == previousSelectRequests + 1,
                "repeating an active action shortcut should request Select exactly once");
        palette.setActiveTool(tool);
        require(palette.activateToolShortcut(tool) && palette.activeToolForTests() == Tool::Select,
                "shortcuts should toggle tools activated through another input path");
    }
}

void groupedToolShortcutsToggleOnlyTheRequestedTool() {
    using Tool = ScreenshotToolPalette::Tool;
    ScreenshotToolPalette::Options options;
    options.showLineTool = true;
    options.showHighlightTool = true;
    options.showRectangleHighlightTool = true;
    options.showPenHighlightTool = true;
    options.showFilterTool = true;
    options.enableStyleToolbar = false;
    options.toolbarLayout = snow_shot::storage::ScreenshotToolbarLayout{
        {{QStringLiteral("select")},
         {QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("line")},
         {QStringLiteral("highlighter")},
         {QStringLiteral("filter")}},
        {}};
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(Tool::Line);
    require(palette.activateDrawingShortcut(QStringLiteral("arrow")) &&
                palette.activeToolForTests() == Tool::Arrow,
            "a shortcut should activate a different tool even when it shares the active button");
    require(palette.activateDrawingShortcut(QStringLiteral("arrow")) &&
                palette.activeToolForTests() == Tool::Select,
            "repeating a grouped tool shortcut should return to Select");

    for (const auto& [id, tool] : {std::pair{"highlight", Tool::RectangleHighlight},
                                   std::pair{"filter", Tool::RectangleFilter}}) {
        palette.setActiveTool(tool);
        require(palette.activateDrawingShortcut(QString::fromLatin1(id)) &&
                    palette.activeToolForTests() == Tool::Select,
                "shortcuts should toggle the active highlight or filter variant");
    }

    options.recordingDrawingMode = true;
    ScreenshotToolPalette recordingPalette(options);
    require(recordingPalette.activateDrawingShortcut(QStringLiteral("shape")) &&
                recordingPalette.activateDrawingShortcut(QStringLiteral("shape")) &&
                !recordingPalette.activeToolForTests().has_value() &&
                recordingPalette.recordingExportSettingsVisible(),
            "repeated recording shortcuts must return to Export Settings");
    recordingPalette.setActiveTool(Tool::Shape);
    recordingPalette.setActiveTool(Tool::Shape);
    require(recordingPalette.activeToolForTests() == Tool::Shape &&
                !recordingPalette.recordingExportSettingsVisible(),
            "programmatic recording tool synchronization must remain idempotent");
    require(!recordingPalette.activateDrawingShortcut(QStringLiteral("highlight")) &&
                recordingPalette.activeToolForTests() == Tool::Shape,
            "unavailable recording shortcuts should leave the current tool unchanged");
}

void screenshotShortcutsShareButtonCommandsAndAvailability() {
    using Tool = ScreenshotToolPalette::Tool;
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showHistoryActions = true;
    options.showScreenRecordButton = true;
    options.showSaveButton = true;
    options.showOcrTool = true;
    options.showTextTranslationTool = true;
    options.showTableTool = true;
    options.showQrTool = true;
    options.showScrollingScreenshotTool = true;
    options.enableStyleToolbar = false;
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;
    ScreenshotToolPalette palette(options);
    struct Command {
        const char* id;
        const char* label;
        void (ScreenshotToolPalette::*signal)();
    };
    const Command commands[] = {
        {"pin_to_screen", "Pin to screen", &ScreenshotToolPalette::pinRequested},
        {"save_as_file", "Save as file", &ScreenshotToolPalette::saveRequested},
        {"quick_save", "Quick save", &ScreenshotToolPalette::quickSaveRequested},
        {"video_recording", "Record screen", &ScreenshotToolPalette::screenRecordRequested},
        {"cancel_screenshot", "Cancel screenshot", &ScreenshotToolPalette::cancelRequested},
        {"copy_to_clipboard", "Copy to clipboard", &ScreenshotToolPalette::copyRequested},
        {"undo", "Undo", &ScreenshotToolPalette::undoRequested},
        {"redo", "Redo", &ScreenshotToolPalette::redoRequested},
        {"move_tool", "Edit selection", &ScreenshotToolPalette::moveRequested},
        {"text_recognition", "Text recognition", &ScreenshotToolPalette::ocrRequested},
        {"text_translation", "Text translation", &ScreenshotToolPalette::textTranslationRequested},
        {"scrolling_screenshot", "Scrolling screenshot",
         &ScreenshotToolPalette::scrollingScreenshotRequested},
    };
    for (const bool scrolling : {false, true}) {
        palette.setScrollingScreenshotMode(scrolling);
        for (const auto& command : commands) {
            auto* button =
                qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, command.label));
            require(button != nullptr, "command must have a real toolbar button");
            int requests = 0;
            const auto connection =
                QObject::connect(&palette, command.signal, &palette, [&requests]() { ++requests; });
            palette.setActiveTool(Tool::Select);
            palette.setHistoryState({true, true});
            button->click();
            const auto clickedTool = palette.activeToolForTests();
            require(requests == 1, "button must emit its command exactly once");
            palette.setActiveTool(Tool::Select);
            require(palette.activateScreenshotShortcut(QString::fromLatin1(command.id)) &&
                        requests == 2 && palette.activeToolForTests() == clickedTool,
                    "shortcut must produce the same command and active tool as a button click");
            button->setEnabled(false);
            button->click();
            require(!palette.activateScreenshotShortcut(QString::fromLatin1(command.id)) &&
                        requests == 2,
                    "disabled button and shortcut must both reject the command");
            button->setEnabled(true);
            QObject::disconnect(connection);
        }
    }
    palette.setTableEnabled(false);
    palette.setQrEnabled(true);
    require(!palette.activateScreenshotShortcut(QStringLiteral("table_recognition")) &&
                palette.activateScreenshotShortcut(QStringLiteral("qr_code_recognition")) &&
                palette.activeToolForTests() == Tool::Qr,
            "shared recognition entries must respect each option's enabled state");
    palette.setTableEnabled(true);
    palette.setQrEnabled(false);
    require(!palette.activateScreenshotShortcut(QStringLiteral("qr_code_recognition")) &&
                palette.activateScreenshotShortcut(QStringLiteral("table_recognition")) &&
                palette.activeToolForTests() == Tool::Table,
            "the enabled recognition option must remain reachable through its shortcut");
    palette.setTableEditingState(true, true, false, false, false, false);
    require(palette.activateScreenshotShortcut(QStringLiteral("undo")) &&
                !palette.activateScreenshotShortcut(QStringLiteral("redo")),
            "recognition history shortcuts must use the toolbar's history availability");
    require(!palette.activateScreenshotShortcut(QStringLiteral("unknown")),
            "unknown command must not activate a toolbar action");
}

void tableToolExposesStructureActionsAndOwnHistoryState() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showTableTool = true;
    options.showHistoryActions = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Table);

    auto* merge =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableMergeButton"));
    auto* split =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableSplitButton"));
    auto* reset =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableResetButton"));
    auto* undo =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotUndoButton"));
    auto* redo =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRedoButton"));
    require(merge != nullptr && split != nullptr && reset != nullptr && undo != nullptr &&
                redo != nullptr,
            "table editing should expose Merge, Split, Reset, Undo, and Redo controls");

    SnowCanvasHistoryState canvasHistory;
    canvasHistory.canRedo = true;
    palette.setHistoryState(canvasHistory);
    require(palette.actionToolbarVisible() && !merge->isHidden() && !split->isHidden() &&
                !reset->isHidden(),
            "Table mode should show its compact structure action row");
    require(!merge->isEnabled() && !split->isEnabled() && !reset->isEnabled() &&
                !undo->isEnabled() && !redo->isEnabled(),
            "table commands should remain disabled until a recognized document is ready");

    palette.setTableEditingState(true, true, false, true, false, true);
    require(merge->isEnabled() && !split->isEnabled() && reset->isEnabled() && undo->isEnabled() &&
                !redo->isEnabled(),
            "table command state should independently drive every editing action");
    canvasHistory.canUndo = true;
    canvasHistory.canRedo = true;
    palette.setHistoryState(canvasHistory);
    require(undo->isEnabled() && !redo->isEnabled(),
            "canvas history updates must not replace table history while Table is active");

    int mergeRequests = 0;
    int splitRequests = 0;
    int resetRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::tableMergeRequested,
                     [&mergeRequests]() { ++mergeRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::tableSplitRequested,
                     [&splitRequests]() { ++splitRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::tableResetRequested,
                     [&resetRequests]() { ++resetRequests; });
    merge->click();
    reset->click();
    palette.setTableEditingState(true, true, true, false, true, true);
    split->click();
    require(mergeRequests == 1 && splitRequests == 1 && resetRequests == 1,
            "enabled table action buttons should forward exactly one command");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    require(palette.findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenshotTableMergeButton")) == nullptr &&
                palette.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotTableSplitButton")) == nullptr &&
                palette.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotTableResetButton")) == nullptr &&
                undo->isEnabled() && redo->isEnabled(),
            "leaving Table should evict table actions and restore cached canvas history");
}

void isolatedBusyIndicatorMatchesItsOwnerWindowBand() {
    if (QGuiApplication::platformName().compare(QStringLiteral("windows"), Qt::CaseInsensitive) !=
        0) {
        return;
    }

    const auto verifyOwner = [](Qt::WindowFlags flags, bool expectedTopmost) {
        QWidget owner(nullptr, flags);
        auto* layout = new QBoxLayout(QBoxLayout::LeftToRight, &owner);
        auto* button = new adqt::widgets::AdButton(&owner);
        button->setBusyIndicatorPresentation(
            adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
        layout->addWidget(button);
        owner.show();
        QCoreApplication::processEvents();

        button->setBusy(true);
        QCoreApplication::processEvents();
        QWidget* surface = button->busyIndicatorSurface();
        require(surface != nullptr && surface->isVisible(),
                "an isolated busy indicator should create a visible native surface");
        require(imageHasVisiblePixel(renderButton(*surface)),
                "an isolated busy indicator surface should paint visible spinner pixels");
        require(surface->windowFlags().testFlag(Qt::WindowStaysOnTopHint) == expectedTopmost,
                "an isolated busy indicator should match its owner window's topmost band");

        button->setBusy(false);
        QCoreApplication::processEvents();
        require(!surface->isVisible(),
                "an isolated busy indicator surface should hide when loading stops");
    };

    verifyOwner(Qt::Tool | Qt::FramelessWindowHint, false);
    verifyOwner(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint, true);
}

void selectedStyleEditsAreReflectedInTheCreationStyleContext() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});

    SnowCanvasShapeStyle creationStyle;
    creationStyle.stroke = QColor(QStringLiteral("#1677ff"));
    creationStyle.strokeWidth = 4.0;
    creationStyle.fill = QColor(QStringLiteral("#bae0ff"));
    creationStyle.fillStyle = SnowCanvasFillStyle::CrossLine;
    creationStyle.cornerRadii = SnowCanvasCornerRadii{6.0, 6.0, 6.0, 6.0};
    palette.setRectangleStyle(creationStyle);

    SnowCanvasStyleToolbarState selectedState;
    selectedState.source = SnowCanvasStyleToolbarSource::SelectedRectangle;
    selectedState.shapeStyle = creationStyle;
    selectedState.shapeStyle.stroke = QColor(QStringLiteral("#52c41a"));
    selectedState.shapeStyle.strokeWidth = 72.0;
    selectedState.shapeStyle.fill = QColor(QStringLiteral("#fff1b8"));
    selectedState.shapeStyle.fillStyle = SnowCanvasFillStyle::Solid;
    selectedState.shapeStyleMixed = SnowCanvasShapeStylePropertyStrokeWidth;
    palette.setStyleToolbarState(selectedState);

    require(qFuzzyCompare(palette.rectangleStyle().strokeWidth + 1.0, 73.0),
            "selected style should replace the displayed creation style");
    require(palette.rectangleStyle().stroke == selectedState.shapeStyle.stroke,
            "selected stroke color should be displayed");
    require(palette.rectangleStyle().fill == selectedState.shapeStyle.fill,
            "selected fill color should be displayed");

    SnowCanvasShapeStyle emittedStyle;
    quint32 emittedProperties = 0;
    int styleChangeCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&emittedStyle, &emittedProperties,
                      &styleChangeCount](const SnowCanvasShapeStyle& style, quint32 properties,
                                         SnowCanvasShapeKind kind) {
                         require(kind == SnowCanvasShapeKind::Rectangle,
                                 "rectangle controls should emit rectangle patches");
                         emittedStyle = style;
                         emittedProperties = properties;
                         ++styleChangeCount;
                     });
    require(palette.stepStrokeWidth(1),
            "a mixed stroke width should be resolved at its upper limit");
    require(styleChangeCount == 1, "style edit should emit once");
    require(emittedProperties == SnowCanvasShapeStylePropertyStrokeWidth,
            "stroke-width edits should only report the stroke-width property");
    require(qFuzzyCompare(emittedStyle.strokeWidth + 1.0, 73.0),
            "emitted selected style should contain the edited stroke width");

    SnowCanvasStyleToolbarState defaultState;
    defaultState.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    defaultState.shapeStyle = creationStyle;
    defaultState.shapeStyle.strokeWidth = emittedStyle.strokeWidth;
    palette.setStyleToolbarState(defaultState);
    require(qFuzzyCompare(palette.rectangleStyle().strokeWidth + 1.0, 73.0),
            "deselecting should retain the selected element's edited stroke width");
    require(palette.rectangleStyle().stroke == creationStyle.stroke &&
                palette.rectangleStyle().fill == creationStyle.fill,
            "unmodified selected-element colors should not replace creation colors");
}

void mixedColorsKeepUniformStyleButtonsActive() {
    QWidget paletteHost;
    paletteHost.resize(420, 320);
    paletteHost.show();
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{}, &paletteHost);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);

    SnowCanvasStyleToolbarState selectedState;
    selectedState.source = SnowCanvasStyleToolbarSource::SelectedRectangle;
    selectedState.shapeStyle.strokeStyle = SnowCanvasStrokeStyle::Dotted;
    selectedState.shapeStyle.fillStyle = SnowCanvasFillStyle::CrossLine;
    selectedState.shapeStyleMixed =
        SnowCanvasShapeStylePropertyStrokeColor | SnowCanvasShapeStylePropertyFillColor;
    palette.setStyleToolbarState(selectedState);

    auto* strokePicker = colorPickerWithAccessibleName(palette, "Stroke color");
    auto* fillPicker = colorPickerWithAccessibleName(palette, "Fill color");
    require(strokePicker != nullptr && fillPicker != nullptr,
            "shape color editors should be present before opening their style options");
    strokePicker->setPopupVisible(true);
    fillPicker->setPopupVisible(true);
    QCoreApplication::processEvents();

    requireControlActive(palette, "Dotted stroke",
                         "uniform stroke style should stay active when stroke colors differ");
    requireControlActive(palette, "Cross-line fill",
                         "uniform fill style should stay active when fill colors differ");
}

void rectangleStyleUsesScreenshotCreationDefaults() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    const SnowCanvasShapeStyle expected =
        snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle;
    const SnowCanvasShapeStyle actual = palette.rectangleStyle();

    require(actual.stroke == expected.stroke &&
                qFuzzyCompare(actual.strokeWidth + 1.0, expected.strokeWidth + 1.0) &&
                actual.strokeStyle == expected.strokeStyle,
            "rectangle style should use the screenshot creation stroke defaults");
    require(actual.fill == expected.fill && actual.fillStyle == expected.fillStyle &&
                qFuzzyCompare(actual.cornerRadii.topLeft + 1.0, 7.0) &&
                actual.cornerRadii.topLeft == expected.cornerRadii.topLeft &&
                actual.cornerRadii.topRight == expected.cornerRadii.topRight &&
                actual.cornerRadii.bottomRight == expected.cornerRadii.bottomRight &&
                actual.cornerRadii.bottomLeft == expected.cornerRadii.bottomLeft,
            "rectangle style should use the screenshot creation fill and corner defaults");
}

void lineToolIsDiscoverableSelectableAndUsesLinearStyleControls() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showLineTool = true;
    ScreenshotToolPalette palette(options);

    auto* lineButton = qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Line"));
    require(lineButton != nullptr, "line toolbar control should be present");

    int requestCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::lineRequested,
                     [&requestCount]() { ++requestCount; });
    lineButton->click();

    require(requestCount == 1, "clicking Line should request line creation once");
    require(lineButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                lineButton->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
            "clicking Line should show its selected state");
    QWidget* lineControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotLineStyleControls"));
    require(lineControls != nullptr && !lineControls->isHidden(),
            "Line should expose its stroke and fill style controls");
    QWidget* arrowControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotArrowStyleControls"));
    require(arrowControls == nullptr || arrowControls->isHidden(),
            "Line should not materialize or expose Arrow-only controls");
    require(controlWithTooltip(palette, "Current stroke width") != nullptr &&
                colorPickerWithAccessibleName(palette, "Stroke color") != nullptr &&
                colorPickerWithAccessibleName(palette, "Fill color") != nullptr,
            "Line should expose stroke color, stroke width, and fill controls");
    auto* lineOpacity =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotLineOpacityButton"));
    require(lineOpacity == nullptr, "Line should not expose an opacity control");
    require(controlWithTooltip(palette, "Corner radius (scroll to adjust)") == nullptr,
            "Line should not materialize Rectangle-only corner radius");

    int lineStyleChangeCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&lineStyleChangeCount](const SnowCanvasShapeStyle&, quint32 properties,
                                             SnowCanvasShapeKind kind) {
                         if (kind == SnowCanvasShapeKind::Line &&
                             properties == SnowCanvasShapeStylePropertyStrokeWidth) {
                             ++lineStyleChangeCount;
                         }
                     });
    clickStyleControl(palette, "Stroke width 4");
    require(lineStyleChangeCount == 1, "Line stroke edits should emit a Line-specific style patch");
    SnowCanvasWidget canvas;
    require(canvas.setCanvasTool(SnowCanvasTool::Line),
            "the canvas should accept the distinct Line tool identity");
    require(canvas.canvasTool() == SnowCanvasTool::Line,
            "the canvas should retain Line while using shared linear geometry");
}

void freeDrawToolIsDistinctAndUsesIndependentPathStyleControls() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    ScreenshotToolPalette palette(options);

    auto* freeDrawButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Pen"));
    require(freeDrawButton != nullptr, "Free Draw toolbar control should be present");
    int requestCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::freeDrawRequested,
                     [&requestCount]() { ++requestCount; });
    freeDrawButton->click();
    require(requestCount == 1, "clicking Free Draw should emit one tool request");
    require(palette.findChild<QWidget*>(QStringLiteral("screenshotFreeDrawStyleControls")) !=
                nullptr,
            "Free Draw should expose the shared compact path controls under its own identity");
    auto* opacity = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotFreeDrawOpacityButton"));
    require(opacity == nullptr, "Free Draw should not expose opacity");
    require(controlWithTooltip(palette, "Corner radius (scroll to adjust)") == nullptr,
            "Free Draw should not materialize Rectangle-only corner radius");

    int freeDrawPatchCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&freeDrawPatchCount](const SnowCanvasShapeStyle&, quint32 properties,
                                           SnowCanvasShapeKind kind) {
                         if (kind == SnowCanvasShapeKind::FreeDraw &&
                             properties == SnowCanvasShapeStylePropertyStrokeWidth) {
                             ++freeDrawPatchCount;
                         }
                     });
    clickStyleControl(palette, "Stroke width 4");
    require(freeDrawPatchCount == 1, "Free Draw style edits should use its own shape kind");

    SnowCanvasWidget canvas;
    require(canvas.setCanvasTool(SnowCanvasTool::FreeDraw), "canvas should accept Free Draw");
    require(canvas.canvasTool() == SnowCanvasTool::FreeDraw,
            "canvas should retain Free Draw identity");
}

void highlightVariantsUseConfiguredPopoverGroup() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showPenHighlightTool = true;
    options.showSpotlightTool = true;
    ScreenshotToolPalette palette(options);

    palette.show();
    QCoreApplication::processEvents();
    auto* trigger =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHighlightButton"));
    adqt::widgets::AdPopover* popover = popoverForTrigger(trigger);
    materializeLazyPopover(trigger);
    QWidget* content = popover != nullptr ? popover->contentWidget() : nullptr;
    auto* highlighterOption = popoverButtonWithTooltip(popover, "Highlight");
    auto* penHighlightOption = popoverButtonWithTooltip(popover, "Pen highlight");
    auto* rectangleHighlightOption = popoverButtonWithTooltip(popover, "Rectangle highlight");
    auto* spotlightOption = popoverButtonWithTooltip(popover, "Spotlight");
    require(trigger != nullptr && trigger->accessibleName() == QStringLiteral("Highlight") &&
                popover != nullptr && content != nullptr &&
                content->objectName() == QStringLiteral("screenshotHighlightPopoverContent") &&
                qobject_cast<QHBoxLayout*>(content->layout()) != nullptr &&
                highlighterOption != nullptr && penHighlightOption == nullptr &&
                rectangleHighlightOption == nullptr && spotlightOption != nullptr &&
                palette.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotRectangleHighlightButton")) == nullptr &&
                content->layout()->indexOf(highlighterOption) <
                    content->layout()->indexOf(spotlightOption),
            "the live toolbar should expose one generic Highlight alongside Spotlight");

    palette.setActiveTool(ScreenshotToolPalette::Tool::PenHighlight);
    const QList<QWidget*> highlightModeSelectors =
        palette.findChildren<QWidget*>(QStringLiteral("screenshotHighlightModeSelector"));
    require(highlightModeSelectors.size() == 1,
            "Pen Highlight should materialize exactly one mode selector");
    for (QWidget* selector : highlightModeSelectors) {
        auto* group = selector == nullptr
                          ? nullptr
                          : selector->findChild<adqt::widgets::AdRadioButtonGroup*>();
        require(group != nullptr && group->buttons().size() == 2,
                "highlight style mode selectors should contain only rectangle and pen");
        const QStringList expectedModes{
            QStringLiteral("Pen highlight"),
            QStringLiteral("Rectangle highlight"),
        };
        for (int index = 0; index < expectedModes.size(); ++index) {
            require(group->buttons().at(index)->toolTip() == expectedModes.at(index),
                    "highlight style mode selectors should place the default pen mode first");
        }
        require(group->checkedId() == static_cast<int>(ScreenshotToolPalette::Tool::PenHighlight),
                "highlight style mode selectors should default to Pen highlight");
    }
    QWidget* spotlightControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotSpotlightStyleControls"));
    require(spotlightControls == nullptr || spotlightControls->findChild<QWidget*>(QStringLiteral(
                                                "screenshotHighlightModeSelector")) == nullptr,
            "Spotlight should remain deferred or exclude the rectangle and pen style selector");

    QWidget* freeDrawButton = controlWithTooltip(palette, "Pen");
    require(freeDrawButton != nullptr && freeDrawButton->mapTo(palette.mainPanel(), QPoint()).y() ==
                                             trigger->mapTo(palette.mainPanel(), QPoint()).y(),
            "the highlight group trigger should remain in the single main toolbar row");

    int rectangleRequests = 0;
    int penRequests = 0;
    int spotlightRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::highlightRequested,
                     [&rectangleRequests]() { ++rectangleRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::penHighlightRequested,
                     [&penRequests]() { ++penRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::spotlightRequested,
                     [&spotlightRequests]() { ++spotlightRequests; });
    auto* highlightModeGroup =
        highlightModeSelectors.constFirst()->findChild<adqt::widgets::AdRadioButtonGroup*>();
    auto* rectangleHighlightMode =
        qobject_cast<adqt::widgets::AdRadio*>(highlightModeGroup->buttons().at(1));
    require(rectangleHighlightMode != nullptr,
            "the highlight style selector should expose Rectangle highlight");
    rectangleHighlightMode->click();
    require(penRequests == 0 && rectangleRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::RectangleHighlight &&
                trigger->accessibleName() == QStringLiteral("Highlight") &&
                trigger->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                trigger->accentRole() == adqt::widgets::AdButton::AccentRole::Primary &&
                highlighterOption->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
            "Rectangle highlight should remain style-selectable through the generic item");

    palette.setToolbarLayout({
        {{QStringLiteral("highlighter"), QStringLiteral("spotlight")},
         {QStringLiteral("free-draw")}},
        {},
    });
    QCoreApplication::processEvents();
    trigger =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHighlightButton"));
    popover = popoverForTrigger(trigger);
    materializeLazyPopover(trigger);
    highlighterOption = popoverButtonWithTooltip(popover, "Highlight");
    spotlightOption = popoverButtonWithTooltip(popover, "Spotlight");
    require(trigger != nullptr && trigger->accessibleName() == QStringLiteral("Highlight") &&
                trigger->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                highlighterOption != nullptr &&
                highlighterOption->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
            "toolbar rebuilds should preserve the generic entry while Rectangle mode is active");

    trigger->click();
    materializeLazyPopover(trigger);
    highlighterOption = popoverButtonWithTooltip(popover, "Highlight");
    spotlightOption = popoverButtonWithTooltip(popover, "Spotlight");
    require(highlighterOption != nullptr && spotlightOption != nullptr,
            "reopening the inactive highlight group should recreate both options");
    require(penRequests == 0 && rectangleRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select &&
                trigger->accessibleName() == QStringLiteral("Highlight") &&
                highlighterOption->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text,
            "clicking the active Highlight trigger should return to selection");

    spotlightOption->click();
    require(penRequests == 0 && rectangleRequests == 1 && spotlightRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Spotlight &&
                trigger->accessibleName() == QStringLiteral("Spotlight") &&
                trigger->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                spotlightOption->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
            "the Spotlight option should replace and activate the shared trigger");

    materializeLazyPopover(trigger);
    highlighterOption = popoverButtonWithTooltip(popover, "Highlight");
    spotlightOption = popoverButtonWithTooltip(popover, "Spotlight");
    require(highlighterOption != nullptr && spotlightOption != nullptr,
            "reopening the highlight group should recreate both options");
    highlighterOption->click();
    require(penRequests == 0 && rectangleRequests == 2 && spotlightRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::RectangleHighlight &&
                trigger->accessibleName() == QStringLiteral("Highlight") &&
                highlighterOption->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
            "the generic Highlight option should restore the last Rectangle highlight mode");

    materializeLazyPopover(trigger);
    highlighterOption = popoverButtonWithTooltip(popover, "Highlight");
    spotlightOption = popoverButtonWithTooltip(popover, "Spotlight");
    require(highlighterOption != nullptr && spotlightOption != nullptr,
            "the active highlight group should recreate options before state synchronization");
    palette.setActiveTool(ScreenshotToolPalette::Tool::FreeDraw);
    require(trigger->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                highlighterOption->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                spotlightOption->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text,
            "leaving the highlight tools should clear the trigger and option states");
}

void highlightStyleToolbarWidthTracksActiveMode() {
    ScreenshotToolPalette::Options options;
    options.showHighlightTool = true;
    ScreenshotToolPalette palette(options);

    const auto expectedPanelSize = [&palette](const char* objectName) {
        QWidget* controls = palette.findChild<QWidget*>(QString::fromUtf8(objectName));
        require(controls != nullptr, "highlight style controls should be present");
        require(controls->layout() != nullptr, "highlight controls should have a layout");
        controls->layout()->activate();

        const QMargins margins = palette.stylePanel()->layout()->contentsMargins();
        return controls->sizeHint() +
               QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    };

    palette.setActiveTool(ScreenshotToolPalette::Tool::RectangleHighlight);
    QCoreApplication::processEvents();
    const QSize rectangleSize = expectedPanelSize("screenshotHighlightStyleControls");
    require(palette.stylePanel()->size() == rectangleSize,
            "rectangle highlight should size the style toolbar to its controls");

    palette.setActiveTool(ScreenshotToolPalette::Tool::PenHighlight);
    QCoreApplication::processEvents();
    const QSize penSize = expectedPanelSize("screenshotPenHighlightStyleControls");
    require(palette.stylePanel()->size() == penSize,
            "pen highlight should recalculate the style toolbar width");

    palette.setActiveTool(ScreenshotToolPalette::Tool::RectangleHighlight);
    QCoreApplication::processEvents();
    require(palette.stylePanel()->size() == rectangleSize,
            "switching back to rectangle highlight should restore its style toolbar width");
}

void eraserToolIsDiscoverableAndHidesStyleControls() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showEraserTool = true;
    ScreenshotToolPalette palette(options);

    auto* eraserButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Eraser"));
    require(eraserButton != nullptr, "Eraser toolbar control should be present");
    int requestCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::eraserRequested,
                     [&requestCount]() { ++requestCount; });
    eraserButton->click();
    require(requestCount == 1, "clicking Eraser should emit one tool request");
    require(palette.stylePanel() == nullptr || palette.stylePanel()->isHidden(),
            "Eraser should hide style controls");

    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::Eraser;
    palette.setStyleToolbarState(state);
    require(palette.stylePanel() == nullptr || palette.stylePanel()->isHidden(),
            "Eraser style source should remain hidden even with canvas state updates");

    SnowCanvasWidget canvas;
    require(canvas.setCanvasTool(SnowCanvasTool::Eraser), "canvas should accept Eraser");
    require(canvas.canvasTool() == SnowCanvasTool::Eraser, "canvas should retain Eraser identity");
}

void drawingModeSelectionsSurviveToolbarReentry() {
    using Tool = ScreenshotToolPalette::Tool;
    const snow_shot::storage::ScreenshotToolbarSettings toolbarSettings;
    const QString originalFilter = toolbarSettings.lastFilterTool();
    const QString originalHighlight = toolbarSettings.lastHighlightTool();
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(toolbarSettings.setLastFilterTool(originalFilter));
        static_cast<void>(toolbarSettings.setLastHighlightTool(originalHighlight));
    });
    ScreenshotToolPalette::Options options;
    options.showHighlightTool = true;
    options.showFilterTool = true;
    ScreenshotToolPalette palette(options);
    auto* highlight =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Highlight"));
    auto* filter = qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Filter"));
    require(highlight != nullptr && filter != nullptr, "both drawing entries should exist");

    const auto selectMode = [&palette](Tool current, Tool next) {
        palette.setActiveTool(current);
        for (auto* group : palette.findChildren<adqt::widgets::AdRadioButtonGroup*>()) {
            if (group->button(static_cast<int>(next)) != nullptr) {
                group->button(static_cast<int>(next))->click();
                require(palette.activeToolForTests() == next, "mode selector should activate mode");
                return;
            }
        }
        require(false, "drawing mode selector should exist");
    };
    for (bool rectangle : {true, false}) {
        const Tool highlightMode = rectangle ? Tool::RectangleHighlight : Tool::PenHighlight;
        const Tool filterMode = rectangle ? Tool::PenFilter : Tool::RectangleFilter;
        selectMode(rectangle ? Tool::PenHighlight : Tool::RectangleHighlight, highlightMode);
        selectMode(rectangle ? Tool::RectangleFilter : Tool::PenFilter, filterMode);
        palette.setActiveTool(Tool::Select);
        highlight->click();
        require(palette.activeToolForTests() == highlightMode,
                "Highlight should restore its own last selected drawing mode");
        filter->click();
        require(palette.activeToolForTests() == filterMode,
                "Filter should independently restore its last selected drawing mode");
        filter->click();
        require(palette.activeToolForTests() == Tool::Select,
                "clicking the active remembered mode should still toggle to Select");
        filter->click();
        require(palette.activeToolForTests() == filterMode,
                "toggling off should preserve the remembered drawing mode");
        palette.setActiveTool(Tool::Select);
        require(palette.activateDrawingShortcut(QStringLiteral("highlight")) &&
                    palette.activeToolForTests() == highlightMode,
                "Highlight shortcut should restore the remembered mode");
        require(palette.activateDrawingShortcut(QStringLiteral("filter")) &&
                    palette.activeToolForTests() == filterMode,
                "Filter shortcut should restore the remembered mode");
    }
}

void rememberedDrawingModesPersistAcrossPaletteInstances() {
    using Tool = ScreenshotToolPalette::Tool;
    const snow_shot::storage::ScreenshotToolbarSettings toolbarSettings;
    const QString originalFilter = toolbarSettings.lastFilterTool();
    const QString originalHighlight = toolbarSettings.lastHighlightTool();
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(toolbarSettings.setLastFilterTool(originalFilter));
        static_cast<void>(toolbarSettings.setLastHighlightTool(originalHighlight));
    });
    require(toolbarSettings.setLastFilterTool(QStringLiteral("pen-filter")) &&
                toolbarSettings.setLastHighlightTool(QStringLiteral("pen-highlight")),
            "remembered drawing mode tests must start from the default preferences");

    ScreenshotToolPalette::Options options;
    options.showHighlightTool = true;
    options.showFilterTool = true;

    ScreenshotToolPalette observer(options);
    {
        ScreenshotToolPalette palette(options);
        const auto selectMode = [&palette](Tool current, Tool next) {
            palette.setActiveTool(current);
            for (auto* group : palette.findChildren<adqt::widgets::AdRadioButtonGroup*>()) {
                if (group->button(static_cast<int>(next)) != nullptr) {
                    group->button(static_cast<int>(next))->click();
                    require(palette.activeToolForTests() == next,
                            "mode selector should activate the requested mode");
                    return;
                }
            }
            require(false, "drawing mode selector should exist");
        };
        selectMode(Tool::PenFilter, Tool::RectangleFilter);
        selectMode(Tool::PenHighlight, Tool::RectangleHighlight);
        require(toolbarSettings.lastFilterTool() == QStringLiteral("rectangle-filter") &&
                    toolbarSettings.lastHighlightTool() == QStringLiteral("rectangle-highlight"),
                "activating a drawing mode should persist it as a toolbar preference");
    }

    // Pin-to-screen editing rebuilds the toolbar for every edit session, so a new
    // palette instance must restore the remembered modes instead of the defaults.
    ScreenshotToolPalette restored(options);
    require(restored.activateDrawingShortcut(QStringLiteral("filter")) &&
                restored.activeToolForTests() == Tool::RectangleFilter,
            "a rebuilt palette should restore the remembered filter mode");
    restored.setActiveTool(Tool::Select);
    require(restored.activateDrawingShortcut(QStringLiteral("highlight")) &&
                restored.activeToolForTests() == Tool::RectangleHighlight,
            "a rebuilt palette should restore the remembered highlight mode");

    // Palettes that stay alive (the screenshot window caches its toolbar) must
    // observe preference updates written by other windows.
    require(observer.activateDrawingShortcut(QStringLiteral("filter")) &&
                observer.activeToolForTests() == Tool::RectangleFilter,
            "a live palette should follow remembered-mode updates from other instances");
}

void rememberedDrawingToolRecordedAndRestored() {
    using Tool = ScreenshotToolPalette::Tool;
    const snow_shot::storage::ScreenshotToolbarSettings toolbarSettings;
    const snow_shot::storage::DrawingSettings drawingSettings;
    const QString originalDrawingTool = toolbarSettings.lastDrawingTool();
    const QString originalHighlight = toolbarSettings.lastHighlightTool();
    const bool originalRememberSwitch = drawingSettings.rememberLastUsedTool();
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(toolbarSettings.setLastDrawingTool(originalDrawingTool));
        static_cast<void>(toolbarSettings.setLastHighlightTool(originalHighlight));
        static_cast<void>(drawingSettings.setRememberLastUsedTool(originalRememberSwitch));
    });
    require(toolbarSettings.setLastDrawingTool(QString()) &&
                drawingSettings.setRememberLastUsedTool(false),
            "remembered drawing tool tests must start from cleared preferences");

    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showHighlightTool = true;
    options.showWatermarkTool = true;

    // Only drawing tools update the remembered tool; Move and Select never do.
    ScreenshotToolPalette palette(options);
    require(palette.activateScreenshotShortcut(QStringLiteral("move_tool")) &&
                palette.activeToolForTests() == Tool::Move,
            "the move tool shortcut should activate the move tool");
    require(toolbarSettings.lastDrawingTool().isEmpty(),
            "activating the move tool must not become the remembered drawing tool");
    require(palette.activateDrawingShortcut(QStringLiteral("select")) &&
                palette.activeToolForTests() == Tool::Select,
            "the select tool shortcut should activate the select tool");
    require(toolbarSettings.lastDrawingTool().isEmpty(),
            "activating the select tool must not become the remembered drawing tool");
    require(palette.activateDrawingShortcut(QStringLiteral("shape")) &&
                palette.activeToolForTests() == Tool::Shape,
            "the shape tool shortcut should activate the shape tool");
    require(toolbarSettings.lastDrawingTool() == QStringLiteral("shape"),
            "activating a drawing tool should persist it as the last used tool");

    // The switch gates the restore, not the recording.
    require(!palette.activateRememberedDrawingTool(),
            "the remembered drawing tool must not restore while the switch is disabled");
    require(drawingSettings.setRememberLastUsedTool(true),
            "the remembered tool switch must be writable");

    // Capture sessions and pin edit sessions rebuild the toolbar, so a fresh
    // palette must restore the remembered tool with variant resolution.
    require(toolbarSettings.setLastDrawingTool(QStringLiteral("highlighter")) &&
                toolbarSettings.setLastHighlightTool(QStringLiteral("rectangle-highlight")),
            "the remembered highlighter variant must be configurable");
    ScreenshotToolPalette restored(options);
    require(restored.activateRememberedDrawingTool() &&
                restored.activeToolForTests() == Tool::RectangleHighlight,
            "a rebuilt palette should restore the remembered highlighter variant");
    require(restored.activateRememberedDrawingTool() &&
                restored.activeToolForTests() == Tool::RectangleHighlight,
            "restoring an already-active remembered tool must not toggle it off");
    restored.setActiveTool(Tool::Select);
    require(toolbarSettings.setLastDrawingTool(QStringLiteral("watermark")) &&
                restored.activateRememberedDrawingTool() &&
                restored.activeToolForTests() == Tool::Watermark,
            "a live palette should follow remembered-tool updates from other instances");
    restored.setActiveTool(Tool::Select);
    require(!toolbarSettings.setLastDrawingTool(QStringLiteral("unknown-tool")) &&
                toolbarSettings.lastDrawingTool() == QStringLiteral("watermark") &&
                restored.activateRememberedDrawingTool() &&
                restored.activeToolForTests() == Tool::Watermark,
            "unknown remembered tool ids must be rejected without changing the stored tool");
    restored.setActiveTool(Tool::Select);
    require(toolbarSettings.setLastDrawingTool(QString()) &&
                !restored.activateRememberedDrawingTool() &&
                restored.activeToolForTests() == Tool::Select,
            "an empty remembered drawing tool must not activate anything");

    ScreenshotToolPalette::Options withoutWatermark = options;
    withoutWatermark.showWatermarkTool = false;
    require(toolbarSettings.setLastDrawingTool(QStringLiteral("watermark")),
            "a valid remembered tool must still round-trip when hidden on another palette");
    ScreenshotToolPalette hiddenWatermark(withoutWatermark);
    require(!hiddenWatermark.activateRememberedDrawingTool(),
            "a palette that does not expose the remembered tool must not activate it");
}

void filterToolExposesTypeAndIntensityControls() {
    const snow_shot::storage::ScreenshotToolbarSettings toolbarSettings;
    const QString originalFilter = toolbarSettings.lastFilterTool();
    const auto cleanup = qScopeGuard(
        [&]() { static_cast<void>(toolbarSettings.setLastFilterTool(originalFilter)); });
    // The Filter entry activates the remembered mode, which is persisted toolbar
    // state; pin the default so this test stays order-independent.
    require(toolbarSettings.setLastFilterTool(QStringLiteral("pen-filter")),
            "filter control tests must start from the default remembered filter mode");
    ScreenshotToolPalette::Options options;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showFilterTool = true;
    ScreenshotToolPalette palette(options);

    auto* filterButton =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Filter"));
    require(filterButton != nullptr, "Filter toolbar control should be present");
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::PenFilter),
            "Filter style rows should materialize on demand");
    const int rectangleFilterId = static_cast<int>(ScreenshotToolPalette::Tool::RectangleFilter);
    const int penFilterId = static_cast<int>(ScreenshotToolPalette::Tool::PenFilter);
    QList<adqt::widgets::AdRadioButtonGroup*> initialFilterModeGroups;
    for (adqt::widgets::AdRadioButtonGroup* group :
         palette.findChildren<adqt::widgets::AdRadioButtonGroup*>()) {
        if (group != nullptr && group->button(rectangleFilterId) != nullptr &&
            group->button(penFilterId) != nullptr) {
            initialFilterModeGroups.append(group);
        }
    }
    require(initialFilterModeGroups.size() == 1,
            "Pen Filter should materialize exactly one mode selector");
    for (adqt::widgets::AdRadioButtonGroup* group : initialFilterModeGroups) {
        require(group->buttons().at(0) == group->button(penFilterId) &&
                    group->buttons().at(1) == group->button(rectangleFilterId),
                "Filter mode selectors should place Pen Filter before Rectangle Filter");
        require(group->checkedId() == penFilterId,
                "Filter mode selectors should default to Pen Filter");
    }

    int requestCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::penFilterRequested,
                     [&requestCount]() { ++requestCount; });
    filterButton->click();
    require(requestCount == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::PenFilter,
            "clicking Filter should request and activate Pen Filter");

    auto* typeSelect = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotPenFilterTypeSelect"));
    auto* intensity = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotPenFilterIntensitySlider"));
    auto* intensityIcon =
        palette.findChild<QLabel*>(QStringLiteral("screenshotPenFilterIntensityIcon"));
    require(typeSelect != nullptr && intensity != nullptr && intensityIcon != nullptr,
            "Filter should expose type and intensity controls");
    require(!intensityIcon->pixmap().isNull(), "Filter intensity should display the blur icon");
    require(typeSelect->variant() == adqt::widgets::AdSelect::Variant::Borderless,
            "Filter type select should match the font-family select style");
    require(palette.findChild<QSlider*>(QStringLiteral("screenshotFilterOpacitySlider")) == nullptr,
            "Filter should not expose an opacity style editor");
    require(typeSelect->model() != nullptr && typeSelect->model()->rowCount() == 6,
            "Filter type select should expose all six filter types");
    struct FilterTypeRow {
        int row;
        SnowCanvasFilterType type;
        QString label;
    };
    const FilterTypeRow filterTypeRows[] = {
        {0, SnowCanvasFilterType::Mosaic, QStringLiteral("Mosaic")},
        {1, SnowCanvasFilterType::GaussianBlur, QStringLiteral("Gaussian blur")},
        {2, SnowCanvasFilterType::SmartErase, QStringLiteral("Smart Erase")},
        {3, SnowCanvasFilterType::Grayscale, QStringLiteral("Grayscale")},
        {4, SnowCanvasFilterType::Inversion, QStringLiteral("Inversion")},
        {5, SnowCanvasFilterType::Emboss, QStringLiteral("Emboss")},
    };
    for (const FilterTypeRow& expected : filterTypeRows) {
        const QModelIndex row = typeSelect->model()->index(expected.row, 0);
        require(row.data(adqt::widgets::AdSelect::DefaultLabelRole).toString() == expected.label &&
                    row.data(adqt::widgets::AdSelect::DefaultValueRole).toInt() ==
                        static_cast<int>(expected.type),
                "Filter type rows should follow display order with Smart Erase after Gaussian "
                "blur");
    }
    require(!typeSelect->sortComparator(),
            "Filter type popup should preserve model order instead of sorting by enum value");

    int styleChangeCount = 0;
    quint32 lastProperties = 0;
    QObject::connect(
        &palette, &ScreenshotToolPalette::filterStyleChanged,
        [&styleChangeCount, &lastProperties](const SnowCanvasFilterStyle&, quint32 properties) {
            ++styleChangeCount;
            lastProperties = properties;
        });
    typeSelect->setCurrentData(2, adqt::widgets::AdSelect::DefaultValueRole);
    require(lastProperties == SnowCanvasFilterStylePropertyType,
            "Filter type should emit its dedicated style property");
    require(!intensity->isEnabled(), "Grayscale should disable filter intensity");
    const QImage disabledIntensityIcon = intensityIcon->pixmap().toImage();
    typeSelect->setCurrentData(5, adqt::widgets::AdSelect::DefaultValueRole);
    require(!intensity->isEnabled(), "Smart Erase must disable intensity");
    typeSelect->setCurrentData(3, adqt::widgets::AdSelect::DefaultValueRole);
    require(!intensity->isEnabled(), "Inversion should disable filter intensity");
    typeSelect->setCurrentData(4, adqt::widgets::AdSelect::DefaultValueRole);
    require(intensity->isEnabled(), "Emboss should enable filter intensity");
    typeSelect->setCurrentData(0, adqt::widgets::AdSelect::DefaultValueRole);
    require(intensity->isEnabled(), "Mosaic should enable filter intensity");
    require(intensityIcon->pixmap().toImage() != disabledIntensityIcon,
            "filter intensity icon should brighten with its enabled slider");
    typeSelect->setCurrentData(4, adqt::widgets::AdSelect::DefaultValueRole);
    require(lastProperties == SnowCanvasFilterStylePropertyType,
            "selecting Emboss should emit only the filter type property");
    const int embossSelectionChangeCount = styleChangeCount;
    intensity->setValue(75);
    require(styleChangeCount == embossSelectionChangeCount + 1 &&
                lastProperties == SnowCanvasFilterStylePropertyStrength,
            "editing Emboss intensity should emit only the strength property");

    SnowCanvasStyleToolbarState mixed;
    mixed.source = SnowCanvasStyleToolbarSource::SelectedFilter;
    mixed.filterStyleMixed = SnowCanvasFilterStylePropertyType;
    palette.setStyleToolbarState(mixed);
    require(typeSelect->currentIndex() == -1,
            "mixed Filter types should clear the filter type selection");
    require(!intensity->isHidden(), "filter intensity should always remain visible");
    require(intensity->isEnabled(), "mixed Filter types should keep filter intensity available");
    mixed.filterStyleMixed |= SnowCanvasFilterStyleMixedContainsSmartErase;
    palette.setStyleToolbarState(mixed);
    require(!intensity->isEnabled(),
            "mixed selection containing Smart Erase must disable intensity");

    QList<adqt::widgets::AdRadioButtonGroup*> filterModeGroups;
    for (adqt::widgets::AdRadioButtonGroup* group :
         palette.findChildren<adqt::widgets::AdRadioButtonGroup*>()) {
        if (group != nullptr && group->button(rectangleFilterId) != nullptr &&
            group->button(penFilterId) != nullptr) {
            filterModeGroups.append(group);
        }
    }
    require(filterModeGroups.size() == 1,
            "Filter should expose one mode selector in its active style row");
    int penFilterRequests = requestCount;
    int rectangleFilterRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::penFilterRequested,
                     [&penFilterRequests]() { ++penFilterRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::rectangleFilterRequested,
                     [&rectangleFilterRequests]() { ++rectangleFilterRequests; });
    filterModeGroups.first()->button(penFilterId)->click();
    require(penFilterRequests == 2 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::PenFilter,
            "the Pen Filter mode selector should request and activate Pen Filter");
    require(filterModeGroups.constFirst()->checkedId() == penFilterId,
            "the Filter mode selector should follow Pen Filter activation");

    auto* rectangleControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotFilterStyleControls"));
    auto* penControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotPenFilterStyleControls"));
    auto* penTypeSelect = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotPenFilterTypeSelect"));
    auto* penIntensity = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotPenFilterIntensitySlider"));
    auto* widthSummary = dynamic_cast<NumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotPenFilterStrokeWidthSummary")));
    auto* width54 = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotPenFilterStrokeWidth54"));
    require(rectangleControls == nullptr,
            "switching back to Pen Filter should evict the Rectangle Filter row");
    require(penControls != nullptr && !penControls->isHidden(),
            "switching back to Pen Filter should expose its exact style row");
    require(penTypeSelect != nullptr && penIntensity != nullptr && widthSummary != nullptr &&
                width54 != nullptr,
            "Pen Filter should expose type, width presets, width summary, and intensity");
    require(controlWithTooltip(palette, "Pen filter stroke width S (24px)") != nullptr &&
                controlWithTooltip(palette, "Pen filter stroke width M (30px)") != nullptr &&
                controlWithTooltip(palette, "Pen filter stroke width L (42px)") != nullptr &&
                controlWithTooltip(palette, "Pen filter stroke width XL (54px)") != nullptr,
            "Pen Filter should use the same numeric stroke-width presets as Pen Highlight");

    SnowCanvasFilterStyle lastStyle;
    QObject::connect(
        &palette, &ScreenshotToolPalette::filterStyleChanged,
        [&lastStyle](const SnowCanvasFilterStyle& style, quint32) { lastStyle = style; });
    width54->click();
    require(lastProperties == SnowCanvasFilterStylePropertyStrokeWidth &&
                lastStyle.strokeWidth == 54.0,
            "Pen Filter width presets should emit only the stroke-width property");

    const auto sendWheel = [&palette](QWidget* target, int delta) {
        const QPoint local = target->rect().center();
        QWheelEvent event(QPointF(local), target->mapToGlobal(local), QPoint(), QPoint(0, delta),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        return palette.handleToolbarWheel(&event) && event.isAccepted();
    };
    require(sendWheel(penTypeSelect, 120) &&
                lastProperties == SnowCanvasFilterStylePropertyStrokeWidth &&
                lastStyle.strokeWidth == 55.0,
            "Pen Filter wheel input from toolbar descendants should step width by one pixel");
    penTypeSelect->setCurrentData(2, adqt::widgets::AdSelect::DefaultValueRole);
    require(!penIntensity->isEnabled() && widthSummary->isEnabled(),
            "Grayscale should disable Pen Filter intensity without disabling width");
    require(sendWheel(penTypeSelect, -120) &&
                lastProperties == SnowCanvasFilterStylePropertyStrokeWidth &&
                lastStyle.strokeWidth == 54.0,
            "Pen Filter wheel width should remain enabled for color-only effects");

    SnowCanvasStyleToolbarState penMaximum;
    penMaximum.source = SnowCanvasStyleToolbarSource::DefaultPenFilter;
    penMaximum.filterStyle.type = SnowCanvasFilterType::Grayscale;
    penMaximum.filterStyle.strength = 0.5;
    penMaximum.filterStyle.opacity = 1.0;
    penMaximum.filterStyle.strokeWidth = 72.0;
    palette.setStyleToolbarState(penMaximum);
    const int changesAtMaximum = styleChangeCount;
    require(sendWheel(widthSummary, 120) && styleChangeCount == changesAtMaximum,
            "Pen Filter wheel width should consume input while clamped at 72px");
    require(sendWheel(widthSummary, -120) && lastStyle.strokeWidth == 71.0 &&
                lastProperties == SnowCanvasFilterStylePropertyStrokeWidth,
            "Pen Filter wheel width should step down from the upper clamp");

    filterModeGroups.last()->button(rectangleFilterId)->click();
    require(rectangleFilterRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::RectangleFilter,
            "the rectangle mode selector should request and restore Rectangle Filter");
    typeSelect->setCurrentData(3, adqt::widgets::AdSelect::DefaultValueRole);
    QWheelEvent disabledIntensityWheel(
        QPointF(intensity->rect().center()), intensity->mapToGlobal(intensity->rect().center()),
        QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    disabledIntensityWheel.ignore();
    require(!palette.handleToolbarWheel(&disabledIntensityWheel) &&
                !disabledIntensityWheel.isAccepted(),
            "disabled Rectangle Filter intensity should not consume wheel input");

    SnowCanvasWidget canvas;
    require(canvas.setCanvasTool(SnowCanvasTool::RectangleFilter),
            "canvas should accept Rectangle Filter");
    require(canvas.canvasTool() == SnowCanvasTool::RectangleFilter,
            "canvas should retain Rectangle Filter identity");
    require(canvas.setCanvasTool(SnowCanvasTool::PenFilter), "canvas should accept Pen Filter");
    require(canvas.canvasTool() == SnowCanvasTool::PenFilter,
            "canvas should retain Pen Filter identity");
}

void filterStyleEditorsMatchShapeAndSpotlightMetrics() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showFilterTool = true;
    options.showSpotlightTool = true;
    ScreenshotToolPalette palette(options);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::PenFilter) &&
                palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Spotlight),
            "Filter and Spotlight editors should materialize on demand");

    struct CompactSliderSnapshot {
        int panelHeight = 0;
        QSize iconSize;
        QSize sliderSize;
        QPixmap pixmap;
    };
    const auto compactSliderSnapshot = [&palette](ScreenshotToolPalette::Tool tool,
                                                  const QString& iconObjectName,
                                                  const QString& sliderObjectName) {
        palette.setActiveTool(tool);
        QCoreApplication::processEvents();
        require(palette.stylePanel() != nullptr, "style panel should exist");
        auto* icon = palette.findChild<QLabel*>(iconObjectName);
        auto* slider = palette.findChild<adqt::widgets::AdSlider*>(sliderObjectName);
        require(icon != nullptr && slider != nullptr,
                "materialized compact slider controls should be discoverable");
        CompactSliderSnapshot snapshot;
        snapshot.panelHeight = palette.stylePanel()->height();
        snapshot.iconSize = icon->size();
        snapshot.sliderSize = slider->size();
        snapshot.pixmap = icon->pixmap(Qt::ReturnByValue);
        return snapshot;
    };
    const auto pixmapHasVisiblePixel = [](const QPixmap& pixmap) {
        const QImage image = pixmap.toImage();
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).alpha() != 0) {
                    return true;
                }
            }
        }
        return false;
    };

    for (const qreal scale : {1.0, 1.5}) {
        palette.setPhysicalScale(scale);
        palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
        QCoreApplication::processEvents();
        require(palette.stylePanel() != nullptr, "style panel should exist");
        const int shapeHeight = palette.stylePanel()->height();
        const CompactSliderSnapshot filter = compactSliderSnapshot(
            ScreenshotToolPalette::Tool::Filter, QStringLiteral("screenshotFilterIntensityIcon"),
            QStringLiteral("screenshotFilterIntensitySlider"));
        const CompactSliderSnapshot penFilter =
            compactSliderSnapshot(ScreenshotToolPalette::Tool::PenFilter,
                                  QStringLiteral("screenshotPenFilterIntensityIcon"),
                                  QStringLiteral("screenshotPenFilterIntensitySlider"));
        const CompactSliderSnapshot spotlight =
            compactSliderSnapshot(ScreenshotToolPalette::Tool::Spotlight,
                                  QStringLiteral("screenshotSpotlightOpacityIcon"),
                                  QStringLiteral("screenshotSpotlightOpacitySlider"));
        require(filter.panelHeight == shapeHeight && penFilter.panelHeight == shapeHeight &&
                    spotlight.panelHeight == shapeHeight,
                "Filter style toolbar heights should match Shape and Spotlight");
        require(filter.iconSize == spotlight.iconSize && penFilter.iconSize == filter.iconSize &&
                    filter.sliderSize == spotlight.sliderSize &&
                    penFilter.sliderSize == filter.sliderSize,
                "Filter intensity editors should use the compact slider metrics");

        require(!filter.pixmap.isNull() && pixmapHasVisiblePixel(filter.pixmap),
                "Filter intensity icon should contain visible pixels");
        require(!penFilter.pixmap.isNull() && pixmapHasVisiblePixel(penFilter.pixmap) &&
                    penFilter.pixmap.toImage() == filter.pixmap.toImage(),
                "Pen Filter should render the same visible Blur glyph as Filter");
        require(filter.pixmap.toImage() != spotlight.pixmap.toImage(),
                "Filter should preserve its Blur glyph while sharing Spotlight metrics");
    }
}

void drawingToolbarGroupsUseToolbarPopoverMetrics() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = false;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showHighlightTool = true;
    options.showPenHighlightTool = true;
    options.showSpotlightTool = true;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette palette(options);
    palette.show();
    QCoreApplication::processEvents();

    const QList<adqt::widgets::AdButton*> drawingButtons = mainDrawingToolbarButtons(palette);
    require(drawingButtons.size() == 3,
            "the default drawing layout should expose Shape and two grouped live slots");
    auto* shapeButton = drawingButtons.at(0);
    auto* arrowLineTrigger = drawingButtons.at(1);
    auto* highlightTrigger = drawingButtons.at(2);
    adqt::widgets::AdPopover* arrowLinePopover = popoverForTrigger(arrowLineTrigger);
    adqt::widgets::AdPopover* highlightPopover = popoverForTrigger(highlightTrigger);
    materializeLazyPopover(arrowLineTrigger);
    require(shapeButton->size() == QSize(32, 32) && popoverForTrigger(shapeButton) == nullptr &&
                arrowLineTrigger->size() == QSize(32, 32) &&
                highlightTrigger->size() == QSize(32, 32) && arrowLinePopover != nullptr &&
                highlightPopover != nullptr &&
                qobject_cast<QHBoxLayout*>(arrowLinePopover->contentWidget()->layout()) !=
                    nullptr &&
                arrowLinePopover->contentWidget()->layout()->spacing() == 8,
            "group triggers and the first popup should use their configured metrics");
    const QList<adqt::widgets::AdButton*> arrowLineButtons{
        popoverButtonWithTooltip(arrowLinePopover, "Arrow"),
        popoverButtonWithTooltip(arrowLinePopover, "Line"),
    };
    require(std::all_of(arrowLineButtons.cbegin(), arrowLineButtons.cend(),
                        [](const auto* button) {
                            return button != nullptr && button->size() == QSize(32, 32);
                        }),
            "the first drawing group options should use the popup reference size");
    require(palette.mainPanel()
                ->findChildren<QWidget*>(QStringLiteral("screenshotDrawingToolPosition"),
                                         Qt::FindDirectChildrenOnly)
                .isEmpty(),
            "live drawing groups should not create vertical toolbar positions");

    require(palette.setPhysicalScale(1.5),
            "drawing toolbar scale test should change the physical scale");
    QCoreApplication::processEvents();
    for (adqt::widgets::AdButton* button : drawingButtons) {
        require(button->size() == QSize(48, 48),
                "drawing toolbar triggers should follow the committed physical scale");
    }
    require(std::all_of(arrowLineButtons.cbegin(), arrowLineButtons.cend(),
                        [](const auto* button) {
                            return button != nullptr && button->size() == QSize(32, 32);
                        }) &&
                arrowLinePopover->contentWidget()->layout()->spacing() == 8,
            "open popup options should retain popup-owned metrics when the toolbar scales");

    arrowLinePopover->hide();
    materializeLazyPopover(highlightTrigger);
    const QList<adqt::widgets::AdButton*> highlightButtons{
        popoverButtonWithTooltip(highlightPopover, "Highlight"),
        popoverButtonWithTooltip(highlightPopover, "Spotlight"),
    };
    require(qobject_cast<QHBoxLayout*>(highlightPopover->contentWidget()->layout()) != nullptr &&
                highlightPopover->contentWidget()->layout()->spacing() == 8 &&
                std::all_of(highlightButtons.cbegin(), highlightButtons.cend(),
                            [](const auto* button) {
                                return button != nullptr && button->size() == QSize(32, 32);
                            }),
            "a group opened after toolbar scaling should still use popup-owned metrics");
}

void spotlightControlsMatchMaskConfigurationBehavior() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showHighlightTool = true;
    options.showSpotlightTool = true;
    ScreenshotToolPalette palette(options);

    auto* highlightTrigger =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotHighlightButton"));
    materializeLazyPopover(highlightTrigger);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Spotlight),
            "Spotlight controls should materialize on demand");
    auto* spotlightButton =
        popoverButtonWithTooltip(popoverForTrigger(highlightTrigger), "Spotlight");
    auto* colorPicker = colorPickerWithAccessibleName(palette, "Mask color");
    auto* opacitySlider = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotSpotlightOpacitySlider"));
    auto* opacityIcon =
        palette.findChild<QLabel*>(QStringLiteral("screenshotSpotlightOpacityIcon"));
    require(highlightTrigger != nullptr && spotlightButton != nullptr && colorPicker != nullptr &&
                opacitySlider != nullptr && opacityIcon != nullptr,
            "Spotlight must be selectable from its group and expose mask controls");
    QWidget* spotlightControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotSpotlightStyleControls"));
    QWidget* maskColorRoot = styleEditorRoot(spotlightControls, "mask-color");
    require(spotlightControls != nullptr && spotlightControls->layout() != nullptr &&
                spotlightControls->layout()->itemAt(0) != nullptr &&
                spotlightControls->layout()->itemAt(0)->widget() == maskColorRoot &&
                maskColorRoot->isAncestorOf(colorPicker),
            "Spotlight style controls should not start with an extra separator");
    require(colorPicker->value().isSolid() &&
                colorPicker->value().solidColor == QColor(Qt::black) &&
                opacitySlider->value() == 64 &&
                opacitySlider->accessibleDescription() == QStringLiteral("64%"),
            "Spotlight controls must default to black at 64 percent");
    require(colorPicker->popupLayerMode() == adqt::widgets::AdColorPicker::PopupLayerMode::QtTool &&
                colorPicker->popupContentPlacement() ==
                    adqt::widgets::AdColorPicker::PopupContentPlacement::Top &&
                dynamic_cast<ColorSwatchButton*>(colorPicker->triggerContent()) != nullptr,
            "Spotlight color should use the watermark color editor popup");

    int requests = 0;
    int previews = 0;
    int commits = 0;
    SnowCanvasSpotlightConfig lastConfig;
    QObject::connect(&palette, &ScreenshotToolPalette::spotlightRequested,
                     [&requests]() { ++requests; });
    QObject::connect(&palette, &ScreenshotToolPalette::spotlightPreviewChanged,
                     [&previews](const SnowCanvasSpotlightConfig&) { ++previews; });
    QObject::connect(&palette, &ScreenshotToolPalette::spotlightConfigChanged,
                     [&commits, &lastConfig](const SnowCanvasSpotlightConfig& config) {
                         ++commits;
                         lastConfig = config;
                     });
    spotlightButton->click();
    require(requests == 1, "Spotlight mode must request the Spotlight canvas tool");

    const QColor previewColor(QStringLiteral("#1677ff"));
    const adqt::widgets::AdColorValue previewValue =
        adqt::widgets::AdColorValue::solid(previewColor);
    colorPicker->setValue(previewValue);
    require(previews == 1 && commits == 0,
            "mask color dragging must preview without committing history");
    colorPicker->editingFinished(previewValue);
    require(commits == 1 && lastConfig.color == previewColor &&
                qFuzzyCompare(lastConfig.opacity + 1.0, 1.64),
            "mask color editing completion must commit the complete configuration");

    clickStyleControl(palette, "Mask color #000000");
    require(commits == 2 && lastConfig.color == QColor(Qt::black),
            "mask color presets must commit the selected color");
    opacitySlider->setValue(55);
    require(commits == 3 && qFuzzyCompare(lastConfig.opacity + 1.0, 1.55) &&
                opacitySlider->accessibleDescription() == QStringLiteral("55%"),
            "Spotlight opacity must commit a complete accessible configuration");

    const QPoint local = opacitySlider->rect().center();
    QWheelEvent wheel(QPointF(local), opacitySlider->mapToGlobal(local), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    require(palette.handleToolbarWheel(&wheel) && wheel.isAccepted() &&
                opacitySlider->value() == 60 && commits == 4,
            "Spotlight opacity wheel input must commit five percentage point steps");
    const QPoint outside(opacitySlider->width() + 20, local.y());
    QWheelEvent outsideWheel(QPointF(outside), opacitySlider->mapToGlobal(outside), QPoint(),
                             QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                             false);
    require(!palette.handleToolbarWheel(&outsideWheel) && opacitySlider->value() == 60 &&
                commits == 4,
            "Spotlight opacity wheel handling must reject points outside the slider");
    require(palette.stepSpotlightOpacity(-1) && opacitySlider->value() == 55 && commits == 5 &&
                qFuzzyCompare(lastConfig.opacity + 1.0, 1.55),
            "Spotlight canvas wheel steps must update the complete mask configuration");

    SnowCanvasSpotlightConfig transparentMask = lastConfig;
    transparentMask.opacity = 0.0;
    palette.setSpotlightConfig(transparentMask);
    require(palette.stepSpotlightOpacity(-1) && opacitySlider->value() == 0 && commits == 5,
            "Spotlight wheel input must remain handled at the opacity boundary");

    require(palette.setPhysicalScale(1.5),
            "Spotlight controls must accept a physical-scale change");
    require(opacitySlider->size() == QSize(144, 42) && opacityIcon->size() == QSize(42, 42),
            "Spotlight opacity controls must follow the toolbar physical scale");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    require(!palette.stepSpotlightOpacity(1) && commits == 5,
            "Spotlight opacity wheel steps must require the Spotlight tool");
    SnowCanvasStyleToolbarState selectedSpotlight;
    selectedSpotlight.source = SnowCanvasStyleToolbarSource::SelectedSpotlight;
    palette.setStyleToolbarState(selectedSpotlight);
    auto* selectionOpacity = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotSelectionOpacitySlider"));
    require(selectionOpacity != nullptr && !selectionOpacity->isEnabled(),
            "Spotlight-only selections must disable generic element opacity");
}

void watermarkToolExposesSharedStyleControls() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = false;
    options.showWatermarkTool = true;
    ScreenshotToolPalette palette(options);
    auto* button = qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Watermark"));
    require(button != nullptr, "Watermark toolbar control should be present");
    int requests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::watermarkRequested,
                     [&requests]() { ++requests; });
    button->click();
    require(requests == 1, "Watermark activation should emit one request");

    auto* controls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkStyleControls"));
    auto* colorPicker = palette.findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenshotWatermarkColorPicker"));
    auto* text = palette.findChild<adqt::widgets::AdLineEdit*>(
        QStringLiteral("screenshotWatermarkTextEdit"));
    auto* fontSize = dynamic_cast<NumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkFontSizeSummaryButton")));
    auto* family = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Watermark font family"));
    auto* templateSelect = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotWatermarkTemplateSelect"));
    auto* angle = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkAngleEditor")));
    auto* gap = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkGapEditor")));
    auto* opacityIcon =
        palette.findChild<QLabel*>(QStringLiteral("screenshotWatermarkOpacityIcon"));
    auto* opacitySlider = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotWatermarkOpacitySlider"));
    require(controls != nullptr && colorPicker != nullptr && text != nullptr &&
                fontSize != nullptr && family != nullptr && templateSelect != nullptr &&
                angle != nullptr && gap != nullptr && opacityIcon != nullptr &&
                opacitySlider != nullptr,
            "Watermark should expose the shared style controls");
    require(colorPicker->accessibleName() == QStringLiteral("Watermark color") &&
                colorPicker->mode() == adqt::widgets::AdColorPicker::Mode::Solid &&
                colorPicker->trigger() == adqt::widgets::AdColorPicker::Trigger::Hover &&
                !colorPicker->triggerTextVisible() && colorPicker->alphaChannelEnabled() &&
                !colorPicker->allowClear() &&
                colorPicker->popupLayerMode() ==
                    adqt::widgets::AdColorPicker::PopupLayerMode::QtTool,
            "Watermark color should match the text color editor");
    require(dynamic_cast<ColorSwatchButton*>(colorPicker->triggerContent()) != nullptr,
            "Watermark color should use a color swatch trigger");
    const QStringList colorNames{
        QStringLiteral("#f5222d"), QStringLiteral("#52c41a"), QStringLiteral("#1677ff"),
        QStringLiteral("#fadb14"), QStringLiteral("#000000"),
    };
    for (const QString& colorName : colorNames) {
        require(qobject_cast<adqt::widgets::AdButton*>(styleControlWithTooltip(
                    palette,
                    QStringLiteral("Watermark color %1").arg(colorName).toUtf8().constData())) !=
                    nullptr,
                "Watermark should reuse all text color presets");
    }
    require(text->placeholderText() == QStringLiteral("Watermark text") &&
                text->accessibleName() == QStringLiteral("Watermark text") &&
                text->controlSize() == adqt::widgets::AdLineEdit::ControlSize::Small &&
                text->variant() == adqt::widgets::AdLineEdit::Variant::Borderless,
            "Watermark text should use the borderless small AdLineEdit");
    require(fontSize->toolTip() == QStringLiteral("Current watermark font size") &&
                fontSize->accessibleDescription() == QStringLiteral("16px"),
            "Watermark font size should use the numeric summary preview");
    const QStringList fontSizeTooltips{
        QStringLiteral("Watermark font size S (12px)"),
        QStringLiteral("Watermark font size M (16px)"),
        QStringLiteral("Watermark font size L (24px)"),
        QStringLiteral("Watermark font size XL (30px)"),
    };
    for (const QString& tooltip : fontSizeTooltips) {
        auto* preset = qobject_cast<adqt::widgets::AdButton*>(
            styleControlWithTooltip(palette, tooltip.toUtf8().constData()));
        require(preset != nullptr && preset->text().isEmpty(),
                "Watermark font-size presets should use the text icons");
    }
    require(family->placeholder() == QStringLiteral("Font family") &&
                family->variant() == adqt::widgets::AdSelect::Variant::Borderless &&
                family->controlSize() == adqt::widgets::AdSelect::ControlSize::Small &&
                family->popupLayerMode() == adqt::widgets::AdSelect::PopupLayerMode::QtTool &&
                family->model() != nullptr &&
                family->model()->rowCount() ==
                    snow_shot::presentation::screenshotToolPaletteFontFamilies().size() + 2 &&
                family->model()->index(0, 0).data(adqt::widgets::AdSelect::DefaultLabelRole) ==
                    QStringLiteral("Default"),
            "Watermark font family should reuse the searchable text selector");
    require(templateSelect->editable(), "Watermark templates should be editable");
    require(templateSelect->searchEnabled(),
            "Watermark templates should use the editable AdSelect input mode");
    require(templateSelect->searchPolicy() == adqt::widgets::AdSelect::SearchPolicy::External &&
                !templateSelect->autoClearSearchValue(),
            "Watermark template input should preserve text without locally filtering options");
    require(templateSelect->popupLayerMode() == adqt::widgets::AdSelect::PopupLayerMode::QtTool,
            "Watermark template shortcuts should open in a native Qt tool window");
    require(templateSelect->placeholder() == QStringLiteral("Template") &&
                templateSelect->toolTip() == QStringLiteral("Template") &&
                templateSelect->accessibleName() == QStringLiteral("Template"),
            "Watermark templates should expose the Template label");
    require(templateSelect->variant() == adqt::widgets::AdSelect::Variant::Borderless &&
                templateSelect->controlSize() == adqt::widgets::AdSelect::ControlSize::Small,
            "Watermark templates should use a borderless small select");
    require(angle->toolTip() == QStringLiteral("Watermark angle") &&
                angle->accessibleName() == angle->toolTip() &&
                angle->cursor().shape() == Qt::SplitVCursor &&
                gap->toolTip() == QStringLiteral("Watermark gap") &&
                gap->accessibleName() == gap->toolTip() &&
                gap->cursor().shape() == Qt::SplitVCursor && angle->size() == gap->size(),
            "Watermark angle and gap should use shared numeric icon editors");
    require(!opacityIcon->pixmap().isNull() && opacityIcon->size() == QSize(28, 28) &&
                opacityIcon->toolTip() == QStringLiteral("Opacity") &&
                opacitySlider->size() == QSize(96, 28) && opacitySlider->minimum() == 0 &&
                opacitySlider->maximum() == 100 && opacitySlider->value() == 16 &&
                opacitySlider->toolTip() == QStringLiteral("Adjust opacity") &&
                opacitySlider->accessibleName() == QStringLiteral("Opacity") &&
                opacitySlider->accessibleDescription() == QStringLiteral("16%"),
            "Watermark opacity should match the compact style editor height");
    require(
        palette.findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotWatermarkOpacityButton")) == nullptr &&
            palette.findChild<QComboBox*>(QStringLiteral("screenshotWatermarkFontSizeCombo")) ==
                nullptr &&
            palette.findChild<QComboBox*>(QStringLiteral("screenshotWatermarkFontFamilyCombo")) ==
                nullptr &&
            palette.findChild<QSlider*>(QStringLiteral("screenshotWatermarkAngleSlider")) ==
                nullptr &&
            palette.findChild<QSlider*>(QStringLiteral("screenshotWatermarkGapSlider")) == nullptr,
        "Watermark should remove all pre-existing widgets");

    QList<QFrame*> separators =
        controls->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly);
    separators.removeAll(opacityIcon);
    QLayout* layout = controls->layout();
    require(layout != nullptr && separators.size() == 3,
            "Watermark should contain three standard group separators");
    QWidget* colorRoot = styleEditorRoot(controls, "foreground-color");
    QWidget* textRoot = styleEditorRoot(controls, "watermark-text");
    QWidget* fontRoot = styleEditorRoot(controls, "watermark-font");
    QWidget* templateRoot = styleEditorRoot(controls, "watermark-template");
    QWidget* angleRoot = styleEditorRoot(controls, "angle");
    QWidget* gapRoot = styleEditorRoot(controls, "gap");
    QWidget* opacityRoot = styleEditorRoot(controls, "opacity");
    QLayout* opacityLayout = opacityRoot != nullptr ? opacityRoot->layout() : nullptr;
    const int colorIndex = layout->indexOf(colorRoot);
    const int textIndex = layout->indexOf(textRoot);
    const int fontIndex = layout->indexOf(fontRoot);
    const int templateIndex = layout->indexOf(templateRoot);
    const int angleIndex = layout->indexOf(angleRoot);
    const int gapIndex = layout->indexOf(gapRoot);
    const int opacityIndex = layout->indexOf(opacityRoot);
    const int opacityIconIndex =
        opacityLayout != nullptr ? opacityLayout->indexOf(opacityIcon) : -1;
    const int opacitySliderIndex =
        opacityLayout != nullptr ? opacityLayout->indexOf(opacitySlider) : -1;
    bool onlySpacingBetweenOpacityControls = opacityIconIndex < opacitySliderIndex;
    for (int index = opacityIconIndex + 1; index < opacitySliderIndex; ++index) {
        onlySpacingBetweenOpacityControls = onlySpacingBetweenOpacityControls &&
                                            opacityLayout->itemAt(index) != nullptr &&
                                            opacityLayout->itemAt(index)->spacerItem() != nullptr;
    }
    require(colorIndex >= 0 && colorIndex < textIndex && textIndex < fontIndex &&
                fontIndex < templateIndex && templateIndex < angleIndex && angleIndex < gapIndex &&
                gapIndex < opacityIndex && colorRoot->isAncestorOf(colorPicker) &&
                fontRoot->isAncestorOf(fontSize) && templateRoot == templateSelect &&
                fontRoot->isAncestorOf(family) && opacityRoot->isAncestorOf(opacityIcon) &&
                opacityRoot->isAncestorOf(opacitySlider) && onlySpacingBetweenOpacityControls &&
                layout->indexOf(separators.at(0)) > colorIndex &&
                layout->indexOf(separators.at(0)) < textIndex &&
                layout->indexOf(separators.at(1)) > templateIndex &&
                layout->indexOf(separators.at(1)) < angleIndex &&
                layout->indexOf(separators.at(2)) > gapIndex &&
                layout->indexOf(separators.at(2)) < opacityIndex,
            "Watermark controls should finish with the opacity editor");
    require(text->height() == 28 && fontSize->height() == 28 && templateSelect->height() == 28 &&
                angle->height() == 28 && gap->height() == 28,
            "Watermark controls should share the style toolbar height");

    const QColor tint(QStringLiteral("#1677ff"));
    const QPixmap angleIcon = snow_shot::presentation::icons::renderTintedIconPixmap(
        snow_shot::presentation::icons::custom::outlined::Angle(), QSize(18, 18), 1.0, tint);
    const QPixmap gapIcon = snow_shot::presentation::icons::renderTintedIconPixmap(
        snow_shot::presentation::icons::custom::outlined::WatermarkGap(), QSize(18, 18), 1.0, tint);
    require(!angleIcon.isNull() && !gapIcon.isNull() && imageHasVisiblePixel(angleIcon.toImage()) &&
                imageHasVisiblePixel(gapIcon.toImage()),
            "Watermark icons should render through the monochrome tint path");
}

void watermarkTemplateSelectMatchesFontSelectWidth() {
    ScreenshotToolPalette::Options options;
    options.showWatermarkTool = true;
    ScreenshotToolPalette palette(options);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Watermark),
            "Watermark controls should materialize on demand");

    auto* templateSelect = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotWatermarkTemplateSelect"));
    auto* fontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Watermark font family"));
    require(templateSelect != nullptr && fontSelect != nullptr &&
                templateSelect->width() == fontSelect->width() &&
                templateSelect->popupMatchSelectWidth() && templateSelect->popupWidth() == 0,
            "watermark-template control and popup should match the font select width");

    require(palette.setPhysicalScale(1.5), "Watermark toolbar scale should change");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    QCoreApplication::processEvents();
    require(templateSelect->width() == fontSelect->width(),
            "watermark-template and font selects should remain aligned after scaling");
}

void watermarkStyleEditorMatchesShapeHeight() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showWatermarkTool = true;
    ScreenshotToolPalette palette(options);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Watermark),
            "Watermark editor should materialize on demand");

    const auto activateAndMeasure = [&palette](ScreenshotToolPalette::Tool tool) {
        palette.setActiveTool(tool);
        QCoreApplication::processEvents();
        require(palette.stylePanel() != nullptr, "style panel should exist");
        return palette.stylePanel()->height();
    };
    for (const qreal scale : {1.0, 1.5}) {
        palette.setPhysicalScale(scale);
        const int shapeHeight = activateAndMeasure(ScreenshotToolPalette::Tool::Shape);
        const int watermarkHeight = activateAndMeasure(ScreenshotToolPalette::Tool::Watermark);
        require(watermarkHeight == shapeHeight,
                "Watermark style toolbar height should match Shape");
        const int expectedControlHeight = qRound(28.0 * scale);
        auto* opacityIcon =
            palette.findChild<QLabel*>(QStringLiteral("screenshotWatermarkOpacityIcon"));
        auto* opacitySlider = palette.findChild<adqt::widgets::AdSlider*>(
            QStringLiteral("screenshotWatermarkOpacitySlider"));
        require(opacityIcon != nullptr && opacitySlider != nullptr,
                "Watermark should expose its opacity controls after materialization");
        require(opacityIcon->height() == expectedControlHeight &&
                    opacitySlider->height() == expectedControlHeight,
                "Watermark opacity controls should use the style button height");
    }
}

void watermarkAndTextToolsUseStandardSpacing() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = false;
    options.showWatermarkTool = true;
    options.showTextTool = true;
    ScreenshotToolPalette palette(options);

    QWidget* watermark = controlWithTooltip(palette, "Watermark");
    QWidget* text = controlWithTooltip(palette, "Text");
    require(watermark != nullptr && text != nullptr,
            "Watermark and Text toolbar controls should be present");

    QLayout* layout = watermark->parentWidget()->layout();
    require(layout != nullptr && layout == text->parentWidget()->layout(),
            "Watermark and Text should share the main toolbar layout");

    const int textIndex = layout->indexOf(text);
    const int watermarkIndex = layout->indexOf(watermark);
    QLayoutItem* spacing =
        textIndex >= 0 && textIndex + 1 < layout->count() ? layout->itemAt(textIndex + 1) : nullptr;
    require(watermarkIndex == textIndex + 2 && spacing != nullptr &&
                spacing->spacerItem() != nullptr && spacing->sizeHint().width() == 8,
            "Text should have 8px spacing before Watermark");
}

void watermarkControlsFollowCommittedStateAndUndo() {
    ScreenshotToolPalette::Options options;
    options.showWatermarkTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    SnowCanvasWidget canvas;
    require(canvas.setCanvasTool(SnowCanvasTool::Watermark),
            "canvas should activate Watermark for toolbar synchronization");

    const auto syncFromCanvas = [&canvas, &palette]() {
        palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
        palette.setWatermarkConfig(canvas.canvasWatermarkConfig());
    };
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, syncFromCanvas);
    syncFromCanvas();

    SnowCanvasWatermarkConfig first;
    first.text = QStringLiteral("FIRST");
    first.templateValue = QStringLiteral("{text}-{YYYY}");
    first.templateApplicationTime =
        SnowCanvasWatermarkTemplateApplicationTime{2026, 9, 15, 12, 34, 56};
    first.angle = 31.0;
    require(canvas.setCanvasWatermarkConfig(first), "first watermark configuration should commit");
    SnowCanvasWatermarkConfig second = first;
    second.text = QStringLiteral("SECOND");
    require(canvas.setCanvasWatermarkConfig(second),
            "second watermark configuration should commit");
    SnowCanvasWatermarkConfig third = second;
    third.text = QStringLiteral("THIRD");
    require(canvas.setCanvasWatermarkConfig(third),
            "third watermark text should commit immediately");

    auto* text = palette.findChild<adqt::widgets::AdLineEdit*>(
        QStringLiteral("screenshotWatermarkTextEdit"));
    auto* angle = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkAngleEditor")));
    auto* templateSelect = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotWatermarkTemplateSelect"));
    require(text != nullptr && angle != nullptr && templateSelect != nullptr &&
                templateSelect->lineEdit() != nullptr,
            "watermark synchronization controls should exist");
    require(text->text() == QStringLiteral("THIRD") && angle->value() == 31 &&
                templateSelect->lineEdit()->text() == first.templateValue,
            "toolbar should reflect the latest committed watermark configuration");

    require(canvas.undo(), "aggregated watermark text changes should be undoable");
    require(text->text() == QStringLiteral("FIRST") && angle->value() == 31 &&
                templateSelect->lineEdit()->text() == first.templateValue,
            "one undo should restore the state before consecutive text-only changes");
    require(canvas.redo(), "aggregated watermark text changes should be redoable");
    require(text->text() == QStringLiteral("THIRD") && angle->value() == 31,
            "one redo should restore the latest aggregated watermark text");
}

void watermarkEditsCommitCompleteConfigsAndClampWheel() {
    ScreenshotToolPalette::Options options;
    options.showWatermarkTool = true;
    ScreenshotToolPalette palette(options);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Watermark),
            "Watermark controls should materialize on demand");
    palette.show();
    palette.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    QCoreApplication::processEvents();

    SnowCanvasWatermarkConfig initial;
    initial.color = QColor(QStringLiteral("#123456"));
    initial.text = QStringLiteral("original");
    initial.fontSize = 18.5;
    initial.fontFamily = QStringLiteral("Missing Watermark Font");
    initial.angle = 10.0;
    initial.gap = 123.0;
    initial.opacity = 0.73;
    palette.setWatermarkConfig(initial);

    SnowCanvasWatermarkConfig lastCommitted;
    SnowCanvasWatermarkConfig lastPreview;
    int committed = 0;
    int previews = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::watermarkConfigChanged,
                     [&lastCommitted, &committed](const SnowCanvasWatermarkConfig& config) {
                         lastCommitted = config;
                         ++committed;
                     });
    QObject::connect(&palette, &ScreenshotToolPalette::watermarkPreviewChanged,
                     [&lastPreview, &previews](const SnowCanvasWatermarkConfig& config) {
                         lastPreview = config;
                         ++previews;
                     });

    auto* text = palette.findChild<adqt::widgets::AdLineEdit*>(
        QStringLiteral("screenshotWatermarkTextEdit"));
    auto* colorPicker = palette.findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenshotWatermarkColorPicker"));
    auto* family = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Watermark font family"));
    auto* templateSelect = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotWatermarkTemplateSelect"));
    auto* angle = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkAngleEditor")));
    auto* gap = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkGapEditor")));
    auto* opacitySlider = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotWatermarkOpacitySlider"));
    QWidget* fontSize =
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkFontSizeSummaryButton"));
    require(text != nullptr && colorPicker != nullptr && family != nullptr &&
                templateSelect != nullptr && angle != nullptr && gap != nullptr &&
                fontSize != nullptr && opacitySlider != nullptr,
            "watermark interaction controls should exist");
    require(family->currentData(adqt::widgets::AdSelect::DefaultValueRole).toString() ==
                initial.fontFamily,
            "unavailable watermark fonts should remain selected");

    text->setText(QStringLiteral("  live text  "));
    require(committed == 1 && previews == 0 && lastCommitted.text == QStringLiteral("live text") &&
                qFuzzyCompare(lastCommitted.opacity + 1.0, initial.opacity + 1.0) &&
                lastCommitted.fontFamily == initial.fontFamily,
            "watermark typing should immediately commit trimmed text and preserve config fields");
    require(QMetaObject::invokeMethod(text, "editingFinished", Qt::DirectConnection),
            "watermark editingFinished should be invokable for normalization");
    require(committed == 1 && text->text() == QStringLiteral("live text") &&
                lastCommitted.text == QStringLiteral("live text") &&
                lastCommitted.color == initial.color &&
                qFuzzyCompare(lastCommitted.fontSize + 1.0, initial.fontSize + 1.0) &&
                lastCommitted.fontFamily == initial.fontFamily &&
                qFuzzyCompare(lastCommitted.angle + 1.0, initial.angle + 1.0) &&
                qFuzzyCompare(lastCommitted.gap + 1.0, initial.gap + 1.0) &&
                qFuzzyCompare(lastCommitted.opacity + 1.0, initial.opacity + 1.0),
            "watermark editing completion should only normalize the displayed text");

    const adqt::widgets::AdColorValue livePickerColor =
        adqt::widgets::AdColorValue::solid(QColor(QStringLiteral("#654321")));
    colorPicker->setValue(livePickerColor);
    require(previews == 1 && committed == 1 && lastPreview.color == livePickerColor.solidColor &&
                lastPreview.text == QStringLiteral("live text") &&
                qFuzzyCompare(lastPreview.opacity + 1.0, initial.opacity + 1.0),
            "watermark picker changes should preview without persistent commits");
    require(QMetaObject::invokeMethod(colorPicker, "editingFinished", Qt::DirectConnection,
                                      Q_ARG(adqt::widgets::AdColorValue, livePickerColor)),
            "watermark picker editingFinished should be invokable");
    require(committed == 2 && previews == 1 && lastCommitted.color == livePickerColor.solidColor &&
                lastCommitted.text == QStringLiteral("live text"),
            "watermark picker completion should commit its final preview once");

    auto* colorPreset = qobject_cast<adqt::widgets::AdButton*>(
        styleControlWithTooltip(palette, "Watermark color #1677ff"));
    require(colorPreset != nullptr, "watermark color preset should exist");
    colorPreset->click();
    require(committed == 3 && lastCommitted.color == QColor(QStringLiteral("#1677ff")) &&
                lastCommitted.text == QStringLiteral("live text") &&
                qFuzzyCompare(lastCommitted.opacity + 1.0, initial.opacity + 1.0),
            "watermark color should emit a complete configuration");

    auto* fontPreset = qobject_cast<adqt::widgets::AdButton*>(
        styleControlWithTooltip(palette, "Watermark font size XL (30px)"));
    require(fontPreset != nullptr, "watermark XL font preset should exist");
    fontPreset->click();
    require(committed == 4 && qFuzzyCompare(lastCommitted.fontSize + 1.0, 31.0) &&
                lastCommitted.color == QColor(QStringLiteral("#1677ff")) &&
                qFuzzyCompare(lastCommitted.opacity + 1.0, initial.opacity + 1.0),
            "watermark font size should emit a complete configuration");

    SnowCanvasWatermarkConfig external = initial;
    external.color = QColor(QStringLiteral("#abcdef"));
    external.text = QStringLiteral("external");
    external.fontSize = 17.5;
    external.fontFamily = QStringLiteral("Another Missing Font");
    external.angle = -45.0;
    external.gap = 200.0;
    palette.setWatermarkConfig(external);
    require(committed == 4 && previews == 1 && colorPicker->value().solidColor == external.color &&
                family->currentData(adqt::widgets::AdSelect::DefaultValueRole).toString() ==
                    external.fontFamily &&
                angle->value() == -45 && gap->value() == 200 && opacitySlider->value() == 73 &&
                opacitySlider->accessibleDescription() == QStringLiteral("73%"),
            "external watermark synchronization should be silent and preserve arbitrary values");

    const auto sendWheel = [&palette](QWidget* editor, int delta) {
        const QPoint local = editor->rect().center();
        QWheelEvent event(QPointF(local), editor->mapToGlobal(local), QPoint(), QPoint(0, delta),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        return palette.handleToolbarWheel(&event) && event.isAccepted();
    };
    require(sendWheel(fontSize, 120), "font-size wheel changes should be handled");
    require(committed == 5 && lastCommitted.fontSize == 18.5 &&
                lastCommitted.angle == external.angle && lastCommitted.gap == external.gap,
            "watermark font-size wheel changes should commit a complete configuration");

    angle->click();
    require(committed == 6 && lastCommitted.angle == 30.0 && lastCommitted.fontSize == 18.5 &&
                lastCommitted.gap == external.gap && lastCommitted.opacity == external.opacity,
            "clicking angle should restore 30 and preserve unrelated fields");

    require(sendWheel(angle, 120), "angle wheel changes should be handled");
    require(committed == 7 && lastCommitted.angle == 31.0 && angle->value() == 31,
            "angle wheel changes should commit one discrete configuration");
    for (int index = 0; index < 200; ++index) {
        require(sendWheel(angle, -120), "angle wheel clamping should be handled");
    }
    require(lastCommitted.angle == -90.0 && angle->value() == -90,
            "angle wheel changes should clamp to -90");
    for (int index = 0; index < 200; ++index) {
        require(sendWheel(angle, 120), "angle upper clamping should be handled");
    }
    require(lastCommitted.angle == 90.0 && angle->value() == 90,
            "angle wheel changes should clamp to 90");

    external.angle = 45.0;
    external.gap = 100.0;
    palette.setWatermarkConfig(external);
    gap->click();
    require(lastCommitted.gap == 56.0 && lastCommitted.angle == external.angle &&
                lastCommitted.opacity == external.opacity,
            "clicking gap should restore 56 and preserve unrelated fields");
    require(sendWheel(gap, 120), "gap wheel changes should be handled");
    require(lastCommitted.gap == 57.0 && gap->value() == 57,
            "gap wheel changes should commit one discrete configuration");
    for (int index = 0; index < 250; ++index) {
        require(sendWheel(gap, -120), "gap lower clamping should be handled");
    }
    require(lastCommitted.gap == 10.0 && gap->value() == 10,
            "gap wheel changes should clamp to 10");
    for (int index = 0; index < 250; ++index) {
        require(sendWheel(gap, 120), "gap upper clamping should be handled");
    }
    require(lastCommitted.gap == 200.0 && gap->value() == 200,
            "gap wheel changes should clamp to 200");

    const int commitsBeforeOpacity = committed;
    opacitySlider->setValue(65);
    require(committed == commitsBeforeOpacity + 1 &&
                qFuzzyCompare(lastCommitted.opacity + 1.0, 1.65) &&
                lastCommitted.angle == external.angle && lastCommitted.gap == 200.0 &&
                opacitySlider->accessibleDescription() == QStringLiteral("65%"),
            "watermark opacity changes should commit the complete configuration");
    require(sendWheel(opacitySlider, 120), "opacity wheel changes should be handled");
    require(committed == commitsBeforeOpacity + 2 && opacitySlider->value() == 70 &&
                qFuzzyCompare(lastCommitted.opacity + 1.0, 1.7) &&
                lastCommitted.fontSize == external.fontSize,
            "watermark opacity wheel steps should use five percentage points");
    palette.hide();
}

void watermarkTemplateLibraryAndEditorApplySnapshotsDeterministically() {
    const snow_shot::storage::WatermarkTemplateSettings templateSettings;
    require(templateSettings.setTemplates({
                {QStringLiteral("Duplicate"), QStringLiteral("{text}-{YYYY}")},
                {QStringLiteral("Duplicate"), QStringLiteral("{text}-{YYYY}")},
            }),
            "watermark-template fixture should persist duplicate rows");

    int clockCalls = 0;
    ScreenshotToolPalette::Options options;
    options.showWatermarkTool = true;
    options.watermarkTemplateClock = [&clockCalls]() {
        return QDateTime(QDate(2026, 9, 15), QTime(12, 34, 10 + clockCalls++),
                         QTimeZone::LocalTime);
    };
    ScreenshotToolPalette palette(options);
    QWidget selectionDisplayOverlay;
    selectionDisplayOverlay.setGeometry(40, 40, 600, 500);
    selectionDisplayOverlay.show();
    palette.setWatermarkTemplateModalOwnerWindow(&selectionDisplayOverlay);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    palette.show();
    QCoreApplication::processEvents();

    auto* select = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotWatermarkTemplateSelect"));
    require(select != nullptr && select->lineEdit() != nullptr && select->options().size() == 2 &&
                select->options().at(0).value.toString() ==
                    QStringLiteral("watermark-template:0") &&
                select->options().at(1).value.toString() == QStringLiteral("watermark-template:1"),
            "watermark-template select should preserve duplicate rows with transient index keys");

    int commits = 0;
    SnowCanvasWatermarkConfig applied;
    QObject::connect(&palette, &ScreenshotToolPalette::watermarkConfigChanged, &palette,
                     [&commits, &applied](const SnowCanvasWatermarkConfig& config) {
                         ++commits;
                         applied = config;
                     });
    const auto expectedTime = [](int second) {
        return std::optional<SnowCanvasWatermarkTemplateApplicationTime>{
            SnowCanvasWatermarkTemplateApplicationTime{2026, 9, 15, 12, 34, second}};
    };

    select->lineEdit()->setText(QStringLiteral("  {text} typed  "));
    emit select->lineEdit()->textEdited(QStringLiteral("  {text} typed  "));
    require(commits == 1 && clockCalls == 1 &&
                select->currentValue().toString() == QStringLiteral("{text} typed") &&
                select->lineEdit()->text() == QStringLiteral("  {text} typed  ") &&
                applied.templateValue == QStringLiteral("  {text} typed  ") &&
                applied.templateApplicationTime == expectedTime(10),
            "manual template edits should commit the normalized selector value while preserving "
            "the exact template and capturing the injected clock");

    SnowCanvasWatermarkConfig external = applied;
    external.templateValue = QStringLiteral("external {DD}");
    external.templateApplicationTime =
        SnowCanvasWatermarkTemplateApplicationTime{2024, 2, 29, 1, 2, 3};
    palette.setWatermarkConfig(external);
    require(commits == 1 && clockCalls == 1 &&
                select->currentValue().toString() == external.templateValue &&
                select->lineEdit()->text() == external.templateValue &&
                select->searchText() == external.templateValue,
            "engine synchronization should update the manual template value without recapturing "
            "time");

    select->hidePopup();
    select->showPopup();
    QWidget* templatePopup = select->view()->window();
    const QRect openingPopupGeometry = templatePopup->geometry();
    const QPoint openingSelectPosition = select->mapToGlobal(QPoint());
    const auto openingAddButtons = templatePopup->findChildren<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotWatermarkTemplateAddButton"));
    require(openingAddButtons.size() == 1 && openingAddButtons.first()->isVisible(),
            "the first watermark-template popup frame should expose only its current footer");
    QCoreApplication::processEvents();
    require(templatePopup->geometry() == openingPopupGeometry &&
                select->mapToGlobal(QPoint()) == openingSelectPosition,
            "the first watermark-template popup frame should use settled anchor geometry");
    QStringList visibleTemplateKeys;
    for (int row = 0; row < select->view()->model()->rowCount(); ++row) {
        const QString key = select->view()->model()->index(row, 0).data(Qt::UserRole).toString();
        if (!key.isEmpty()) {
            visibleTemplateKeys.push_back(key);
        }
    }
    require(select->currentValue().toString() == external.templateValue &&
                select->lineEdit()->text() == external.templateValue &&
                select->searchText() == external.templateValue &&
                visibleTemplateKeys == QStringList{QStringLiteral("watermark-template:0"),
                                                   QStringLiteral("watermark-template:1")} &&
                select->view()->window()->isWindow() &&
                select->view()->window()->windowType() == Qt::Tool,
            "opening template shortcuts should preserve manual input and show every option in a "
            "native Qt tool window");
    select->hidePopup();
    require(select->currentValue().toString() == external.templateValue &&
                select->lineEdit()->text() == external.templateValue,
            "closing template shortcuts should restore the committed manual input");

    emit select->selected(QVariant(QStringLiteral("watermark-template:1")),
                          QStringLiteral("Duplicate"));
    require(commits == 2 && clockCalls == 2 &&
                select->currentValue().toString() == QStringLiteral("{text}-{YYYY}") &&
                applied.templateValue == QStringLiteral("{text}-{YYYY}") &&
                applied.templateApplicationTime == expectedTime(11),
            "selecting a shortcut should replace the manual value once with a fresh time");

    select->lineEdit()->clear();
    emit select->lineEdit()->textEdited(QString());
    require(commits == 3 && clockCalls == 3 && applied.templateValue.isEmpty() &&
                applied.templateApplicationTime == expectedTime(12),
            "clearing the editable template should capture a new application time");

    const auto findAddButton = [select]() {
        QWidget* popup = select->view() != nullptr ? select->view()->window() : nullptr;
        return popup != nullptr ? popup->findChild<adqt::widgets::AdButton*>(
                                      QStringLiteral("screenshotWatermarkTemplateAddButton"))
                                : nullptr;
    };
    select->showPopup();
    QCoreApplication::processEvents();
    auto* addButton = findAddButton();
    require(addButton != nullptr && addButton->isVisible() &&
                addButton->text() == QStringLiteral("Add") &&
                addButton->accentRole() == adqt::widgets::AdButton::AccentRole::Primary &&
                adqt::icons::describeIcon(addButton->iconRef()).key.name == QStringLiteral("plus"),
            "watermark-template popup should keep a full-width primary Add footer");

    auto& languageManager = snow_shot::presentation::LanguageManager::instance();
    require(languageManager.setLanguage(QStringLiteral("zh_CN")),
            "Simplified Chinese watermark-template language setup should succeed");
    QCoreApplication::processEvents();
    addButton = findAddButton();
    auto* emptyLabel = qobject_cast<QLabel*>(select->notFoundContentWidget());
    require(select->placeholder() == QStringLiteral("模板") &&
                select->toolTip() == QStringLiteral("模板") &&
                select->accessibleName() == QStringLiteral("模板"),
            "the template select should retranslate in place");
    require(select->options().at(0).group == QStringLiteral("模板"),
            "the open template popup option group should retranslate in place");
    require(emptyLabel != nullptr && emptyLabel->text() == QStringLiteral("暂无模板"),
            "the template empty-state text should retranslate in place");
    require(addButton != nullptr && addButton->text() == QStringLiteral("添加") &&
                addButton->toolTip() == QStringLiteral("添加模板") &&
                addButton->accessibleName() == QStringLiteral("添加模板"),
            "the open template popup Add footer should retranslate in place");
    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English watermark-template language restoration should succeed");
    QCoreApplication::processEvents();
    addButton = findAddButton();
    require(addButton != nullptr, "the translated Add footer should remain available");
    addButton->click();
    QCoreApplication::processEvents();

    auto* createModal = palette.findChild<adqt::widgets::AdModal*>(
        QStringLiteral("screenshotWatermarkTemplateCreateModal"));
    auto* form = createModal == nullptr
                     ? nullptr
                     : qobject_cast<adqt::widgets::AdForm*>(createModal->contentWidget());
    auto* nameInput = form == nullptr ? nullptr
                                      : form->findChild<adqt::widgets::AdLineEdit*>(
                                            QStringLiteral("screenshotWatermarkTemplateNameInput"));
    auto* valueInput = form == nullptr ? nullptr
                                       : form->findChild<adqt::widgets::AdLineEdit*>(QStringLiteral(
                                             "screenshotWatermarkTemplateValueInput"));
    auto* alert = form == nullptr ? nullptr
                                  : form->findChild<adqt::widgets::AdAlert*>(
                                        QStringLiteral("screenshotWatermarkTemplateInfoAlert"));
    auto* nameItem = form == nullptr ? nullptr : form->itemForName(QStringLiteral("templateName"));
    auto* valueItem =
        form == nullptr ? nullptr : form->itemForName(QStringLiteral("templateValue"));
    require(createModal != nullptr && createModal->isOpen(),
            "Add template should expose its open modal");
    require(form != nullptr, "Add template should expose its Ant Design form");
    require(nameInput != nullptr && valueInput != nullptr,
            "Add template should expose its named inputs");
    require(alert != nullptr, "Add template should expose its information alert");
    require(nameItem != nullptr && valueItem != nullptr,
            "Add template should expose its named form items");
    require(createModal->mode() == adqt::widgets::AdModal::Mode::Window &&
                createModal->windowModality() == Qt::ApplicationModal && createModal->centered() &&
                createModal->ownerWindow() == &selectionDisplayOverlay &&
                createModal->windowTitle() == QStringLiteral("Add template") &&
                createModal->acceptButton()->text() == QStringLiteral("Add") &&
                createModal->rejectButton()->text() == QStringLiteral("Cancel"),
            "Add template should center its application-modal window on the selection display "
            "overlay");
    require((createModal->contentWidget()->window()->frameGeometry().center() -
             selectionDisplayOverlay.frameGeometry().center())
                    .manhattanLength() <= 4,
            "Add template should be geometrically centered on the selection display overlay");
    require(nameInput->text() == QStringLiteral("Template 3") && nameInput->maxLength() == 80 &&
                valueInput->text() == QStringLiteral("{text}") && nameItem->required() &&
                valueItem->required(),
            "Add template should use the specified field defaults and required marks");
    require(alert->iconMode() == adqt::widgets::AdAlert::IconMode::Visible &&
                alert->text() ==
                    QStringLiteral("{text} represents the current watermark text; timestamp "
                                   "formats such as {YYYY-MM-DD_HH-mm-ss} are supported"),
            "Add template should show the exact icon-bearing information alert");
    auto* formLayout = qobject_cast<QBoxLayout*>(form->layout());
    require(formLayout != nullptr && formLayout->indexOf(nameItem) >= 0 &&
                formLayout->indexOf(valueItem) == formLayout->indexOf(nameItem) + 1 &&
                formLayout->indexOf(alert) == formLayout->indexOf(valueItem) + 1,
            "Add template should place the information alert directly below the template value");

    nameInput->setText(QStringLiteral("   "));
    valueInput->setText(QStringLiteral(" \t "));
    createModal->acceptButton()->click();
    QCoreApplication::processEvents();
    require(createModal->isOpen() &&
                nameItem->validateStatus() == adqt::widgets::AdFormItem::ValidateStatus::Error &&
                valueItem->validateStatus() == adqt::widgets::AdFormItem::ValidateStatus::Error &&
                nameItem->requiredMessage() == QStringLiteral("Please enter a template name") &&
                valueItem->requiredMessage() == QStringLiteral("Please enter a template value"),
            "Add template should reject whitespace-only required values with translated messages");

    require(languageManager.setLanguage(QStringLiteral("zh_CN")),
            "Simplified Chinese Add template language setup should succeed");
    QCoreApplication::processEvents();
    require(createModal->windowTitle() == QStringLiteral("添加模板") &&
                createModal->acceptButton()->text() == QStringLiteral("添加") &&
                createModal->rejectButton()->text() == QStringLiteral("取消") &&
                nameItem->label() == QStringLiteral("模板名称") &&
                valueItem->label() == QStringLiteral("模板值") &&
                nameItem->requiredMessage() == QStringLiteral("请输入模板名称") &&
                valueItem->requiredMessage() == QStringLiteral("请输入模板值") &&
                nameItem->errorMessages() == QStringList{QStringLiteral("请输入模板名称")} &&
                valueItem->errorMessages() == QStringList{QStringLiteral("请输入模板值")} &&
                alert->text() == QStringLiteral("{text} 表示当前水印文本；支持 "
                                                "{YYYY-MM-DD_HH-mm-ss} 等时间戳格式"),
            "the open Add template dialog and validation state should retranslate in place");
    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English Add template language restoration should succeed");
    QCoreApplication::processEvents();

    nameInput->setText(QStringLiteral("  Added  "));
    valueInput->setText(QStringLiteral("  {text} {DD}  "));
    createModal->acceptButton()->click();
    QCoreApplication::processEvents();
    const QVector<snow_shot::storage::WatermarkTemplate> afterAdd = templateSettings.templates();
    require(commits == 4 && clockCalls == 4 &&
                applied.templateValue == QStringLiteral("  {text} {DD}  ") &&
                applied.templateApplicationTime == expectedTime(13) && afterAdd.size() == 3 &&
                afterAdd.at(2) ==
                    snow_shot::storage::WatermarkTemplate{QStringLiteral("Added"),
                                                          QStringLiteral("  {text} {DD}  ")},
            "successful creation should append, trim the name, preserve the value, and apply it");

    select->showPopup();
    QCoreApplication::processEvents();
    QListView* view = select->view();
    QModelIndex firstDuplicate;
    for (int row = 0; view != nullptr && row < view->model()->rowCount(); ++row) {
        const QModelIndex candidate = view->model()->index(row, 0);
        if (candidate.data(Qt::UserRole).toString() == QStringLiteral("watermark-template:0")) {
            firstDuplicate = candidate;
            break;
        }
    }
    require(firstDuplicate.isValid(), "the first duplicate template should have a popup row");
    const QRect rowRect = view->visualRect(firstDuplicate);
    const QPoint deletePoint(rowRect.right() - 15, rowRect.center().y());
    const auto sendMouse = [view](QEvent::Type type, const QPoint& point, Qt::MouseButton button,
                                  Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(point), QPointF(view->viewport()->mapToGlobal(point)),
                          button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &event);
    };
    sendMouse(QEvent::MouseMove, deletePoint, Qt::NoButton, Qt::NoButton);
    sendMouse(QEvent::MouseButtonPress, deletePoint, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, deletePoint, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();

    auto* deleteModal = palette.findChild<adqt::widgets::AdModal*>(
        QStringLiteral("screenshotWatermarkTemplateDeleteModal"));
    const SnowCanvasWatermarkConfig beforeDelete = applied;
    require(deleteModal != nullptr && deleteModal->isOpen() &&
                deleteModal->mode() == adqt::widgets::AdModal::Mode::Window &&
                deleteModal->windowModality() == Qt::ApplicationModal &&
                deleteModal->windowTitle() == QStringLiteral("Delete template") &&
                deleteModal->text() ==
                    QStringLiteral("Delete template \"Duplicate\"? This action cannot be undone") &&
                deleteModal->acceptButton()->text() == QStringLiteral("Delete") &&
                deleteModal->rejectButton()->text() == QStringLiteral("Cancel") &&
                deleteModal->acceptAccentRole() == adqt::widgets::AdButton::AccentRole::Danger,
            "template deletion should use the specified danger confirmation");

    require(languageManager.setLanguage(QStringLiteral("zh_TW")),
            "Traditional Chinese Delete template language setup should succeed");
    QCoreApplication::processEvents();
    require(deleteModal->windowTitle() == QStringLiteral("刪除範本") &&
                deleteModal->text() == QStringLiteral("刪除範本「Duplicate」？此操作無法復原") &&
                deleteModal->acceptButton()->text() == QStringLiteral("刪除") &&
                deleteModal->rejectButton()->text() == QStringLiteral("取消"),
            "the open Delete template confirmation should retranslate in place");
    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English Delete template language restoration should succeed");
    QCoreApplication::processEvents();
    deleteModal->acceptButton()->click();
    QCoreApplication::processEvents();
    const QVector<snow_shot::storage::WatermarkTemplate> afterDelete = templateSettings.templates();
    require(afterDelete.size() == 2 && afterDelete.at(0).name == QStringLiteral("Duplicate") &&
                afterDelete.at(1).name == QStringLiteral("Added") && commits == 4 &&
                applied == beforeDelete,
            "deleting a duplicate row should remove only that index and leave the snapshot intact");

    require(templateSettings.setTemplates({}), "empty watermark-template library should persist");
    select->hidePopup();
    select->showPopup();
    QCoreApplication::processEvents();
    emptyLabel = qobject_cast<QLabel*>(select->notFoundContentWidget());
    addButton = findAddButton();
    require(select->options().isEmpty() && emptyLabel != nullptr &&
                emptyLabel->text() == QStringLiteral("No templates yet") &&
                emptyLabel->isVisible() && addButton != nullptr && addButton->isVisible(),
            "an empty template library should show its hint while retaining the Add footer");
    select->hidePopup();
    palette.hide();
}

void drawTemplateSelectSavesFiltersInsertsAndDeletes() {
    const snow_shot::storage::DrawTemplateSettings settings;
    require(settings.setTemplates({}), "draw-template test must start with an empty library");
    const QByteArray payload =
        QByteArrayLiteral(R"({"schemaVersion":1,"selectedIds":[1],"elements":[1]})");
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showOcrTool = true;
    ScreenshotToolPalette palette(options);
    QWidget display;
    display.setGeometry(60, 60, 600, 440);
    display.show();
    palette.setWatermarkTemplateModalOwnerWindow(&display);
    int snapshots = 0;
    QVector<QByteArray> inserted;
    palette.setDrawTemplateCallbacks(
        [&]() {
            ++snapshots;
            return payload;
        },
        [&](const QByteArray& value) { inserted.push_back(value); });
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    palette.show();
    QCoreApplication::processEvents();
    auto* select =
        palette.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenshotDrawTemplateSelect"));
    auto* opacity = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotSelectionOpacitySlider"));
    auto* layout =
        select == nullptr ? nullptr : qobject_cast<QBoxLayout*>(select->parentWidget()->layout());
    require(select != nullptr && opacity != nullptr && layout != nullptr &&
                select->placeholder() == QStringLiteral("Draw Template") &&
                select->searchEnabled() && select->isEnabled() &&
                layout->indexOf(select) < layout->indexOf(opacity),
            "Draw Template should be an enabled searchable select left of opacity");
    bool separatorBetween = false;
    for (int i = layout->indexOf(select) + 1; i < layout->indexOf(opacity); ++i) {
        separatorBetween |= qobject_cast<QFrame*>(layout->itemAt(i)->widget()) != nullptr;
    }
    require(separatorBetween, "Draw Template and opacity need a separator");

    select->showPopup();
    QCoreApplication::processEvents();
    auto* add = select->view()->window()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotDrawTemplateAddButton"));
    require(add != nullptr && !add->isEnabled(),
            "Add Template should be disabled without selected elements");

    SnowCanvasStyleToolbarState selected;
    selected.source = SnowCanvasStyleToolbarSource::SelectedRectangle;
    selected.selectedElementCount = 1;
    palette.setStyleToolbarState(selected);
    require(select->isEnabled() && add->isEnabled(),
            "selecting an element should enable Add Template without changing the select");
    const SnowCanvasStyleToolbarState unselected;
    palette.setStyleToolbarState(unselected);
    require(select->isEnabled() && !add->isEnabled(),
            "clearing selection while the popup is open should disable only Add Template");
    palette.setStyleToolbarState(selected);
    require(add->isEnabled(), "reselecting an element should enable the open Add Template footer");
    select->showPopup();
    QCoreApplication::processEvents();
    add = select->view()->window()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotDrawTemplateAddButton"));
    require(add != nullptr && add->isVisible() && add->text() == QStringLiteral("Add Template") &&
                add->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
            "an empty Draw Template popup should retain its Add Template footer");
    auto& language = snow_shot::presentation::LanguageManager::instance();
    require(language.setLanguage(QStringLiteral("zh_CN")),
            "Draw Template popup should load Simplified Chinese");
    QCoreApplication::processEvents();
    require(select->placeholder() == QStringLiteral("绘图模板") &&
                add->text() == QStringLiteral("添加模板"),
            "an open Draw Template popup should retranslate its field and footer");
    require(language.setLanguage(QStringLiteral("en_US")),
            "Draw Template popup should restore English before naming");
    QCoreApplication::processEvents();
    add->click();
    QCoreApplication::processEvents();
    auto* modal = palette.findChild<adqt::widgets::AdModal*>(
        QStringLiteral("screenshotDrawTemplateCreateModal"));
    auto* form =
        modal == nullptr ? nullptr : qobject_cast<adqt::widgets::AdForm*>(modal->contentWidget());
    auto* name = form == nullptr ? nullptr
                                 : form->findChild<adqt::widgets::AdLineEdit*>(
                                       QStringLiteral("screenshotDrawTemplateNameInput"));
    require(modal != nullptr && modal->isOpen(), "Add Template should open a modal");
    require(modal->ownerWindow() == &display, "Add Template modal should use the current display");
    require(name != nullptr, "Add Template should expose a name input");
    require(name->text() == QStringLiteral("Template 1"),
            "Add Template should generate Template 1");
    require(language.setLanguage(QStringLiteral("zh_TW")),
            "Draw Template modal should load Traditional Chinese");
    QCoreApplication::processEvents();
    require(modal->windowTitle() == QStringLiteral("新增範本") &&
                select->placeholder() == QStringLiteral("繪圖範本"),
            "an open Add Template modal should retranslate without closing");
    require(language.setLanguage(QStringLiteral("en_US")),
            "Draw Template test should restore English");
    QCoreApplication::processEvents();
    require(snapshots == 0, "Add Template should wait for confirmation to capture the selection");
    modal->acceptButton()->click();
    QCoreApplication::processEvents();
    require(snapshots == 1, "confirming Add Template should capture the selected elements once");
    require(settings.templates() ==
                QVector<snow_shot::storage::DrawTemplate>{{QStringLiteral("Template 1"), payload}},
            "confirming Add Template should persist the editable element payload");

    palette.setStyleToolbarState(unselected);
    select->showPopup();
    QCoreApplication::processEvents();
    add = select->view()->window()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotDrawTemplateAddButton"));
    require(select->isEnabled() && add != nullptr && !add->isEnabled() &&
                select->options().size() == 1,
            "saved templates should remain selectable without selected elements");
    emit select->selected(QVariant(QStringLiteral("draw-template:0")),
                          QStringLiteral("Template 1"));
    require(inserted == QVector<QByteArray>{payload},
            "choosing a draw template without a selection should insert its elements");
    inserted.clear();
    select->hidePopup();
    palette.setStyleToolbarState(selected);

    select->showPopup();
    QCoreApplication::processEvents();
    add = select->view()->window()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotDrawTemplateAddButton"));
    require(add != nullptr, "saved Draw Template should retain its Add footer");
    add->click();
    QCoreApplication::processEvents();
    modal = nullptr;
    for (auto* candidate : palette.findChildren<adqt::widgets::AdModal*>(
             QStringLiteral("screenshotDrawTemplateCreateModal"))) {
        if (candidate->isOpen()) {
            modal = candidate;
        }
    }
    form =
        modal == nullptr ? nullptr : qobject_cast<adqt::widgets::AdForm*>(modal->contentWidget());
    name = form == nullptr ? nullptr
                           : form->findChild<adqt::widgets::AdLineEdit*>(
                                 QStringLiteral("screenshotDrawTemplateNameInput"));
    require(modal != nullptr && name != nullptr && name->text() == QStringLiteral("Template 2"),
            "Add Template should propose the next available numbered name");
    modal->rejectButton()->click();
    QCoreApplication::processEvents();
    require(snapshots == 1 && settings.templates().size() == 1,
            "canceling Add Template should neither capture nor save content");

    select->showPopup();
    QCoreApplication::processEvents();
    add = select->view()->window()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotDrawTemplateAddButton"));
    select->setSearchText(QStringLiteral("missing"));
    QCoreApplication::processEvents();
    int visibleOptions = 0;
    for (int row = 0; row < select->view()->model()->rowCount(); ++row) {
        visibleOptions += select->view()
                              ->model()
                              ->index(row, 0)
                              .data(Qt::UserRole)
                              .toString()
                              .startsWith(QStringLiteral("draw-template:"));
    }
    require(visibleOptions == 0 && add->isVisible(),
            "typing in Draw Template should filter options without hiding Add Template");
    select->setSearchText(QStringLiteral("template"));
    QCoreApplication::processEvents();
    visibleOptions = 0;
    for (int row = 0; row < select->view()->model()->rowCount(); ++row) {
        visibleOptions += select->view()->model()->index(row, 0).data(Qt::UserRole).toString() ==
                          QStringLiteral("draw-template:0");
    }
    require(visibleOptions == 1, "Draw Template search should match names case insensitively");
    select->hidePopup();
    emit select->selected(QVariant(QStringLiteral("draw-template:0")),
                          QStringLiteral("Template 1"));
    emit select->selected(QVariant(QStringLiteral("draw-template:0")),
                          QStringLiteral("Template 1"));
    require(inserted == QVector<QByteArray>{payload, payload} && !select->currentValue().isValid(),
            "choosing the same template twice should insert twice and restore the placeholder");

    select->showPopup();
    QCoreApplication::processEvents();
    QListView* view = select->view();
    QModelIndex option;
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        const QModelIndex candidate = view->model()->index(row, 0);
        if (candidate.data(Qt::UserRole).toString() == QStringLiteral("draw-template:0")) {
            option = candidate;
            break;
        }
    }
    require(option.isValid(), "saved Draw Template should have an option row");
    const QRect rowRect = view->visualRect(option);
    const QPoint action(rowRect.right() - 15, rowRect.center().y());
    const auto mouse = [view, action](QEvent::Type type, Qt::MouseButton button,
                                      Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(action), QPointF(view->viewport()->mapToGlobal(action)),
                          button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &event);
    };
    mouse(QEvent::MouseMove, Qt::NoButton, Qt::NoButton);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    auto* deletion = palette.findChild<adqt::widgets::AdModal*>(
        QStringLiteral("screenshotDrawTemplateDeleteModal"));
    require(deletion != nullptr && deletion->isOpen() && deletion->ownerWindow() == &display &&
                deletion->acceptAccentRole() == adqt::widgets::AdButton::AccentRole::Danger,
            "the option delete action should open a danger confirmation on the current display");
    deletion->acceptButton()->click();
    QCoreApplication::processEvents();
    require(settings.templates().isEmpty(), "deleting a Draw Template should remove its row");
    palette.hide();
}

void watermarkControlsFollowPhysicalScale() {
    ScreenshotToolPalette::Options options;
    options.showWatermarkTool = true;
    ScreenshotToolPalette palette(options);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Watermark),
            "Watermark controls should materialize on demand");

    auto* colorPicker = palette.findChild<adqt::widgets::AdColorPicker*>(
        QStringLiteral("screenshotWatermarkColorPicker"));
    auto* colorTrigger = dynamic_cast<ColorSwatchButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkColorTrigger")));
    auto* text = palette.findChild<adqt::widgets::AdLineEdit*>(
        QStringLiteral("screenshotWatermarkTextEdit"));
    auto* fontSize = dynamic_cast<NumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkFontSizeSummaryButton")));
    auto* family = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Watermark font family"));
    auto* templateSelect = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotWatermarkTemplateSelect"));
    auto* angle = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkAngleEditor")));
    auto* gap = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkGapEditor")));
    const QStringList fontSizeTooltips{
        QStringLiteral("Watermark font size S (12px)"),
        QStringLiteral("Watermark font size M (16px)"),
        QStringLiteral("Watermark font size L (24px)"),
        QStringLiteral("Watermark font size XL (30px)"),
    };
    QList<QWidget*> fontSizeButtons;
    for (const QString& tooltip : fontSizeTooltips) {
        fontSizeButtons.append(controlWithTooltip(palette, tooltip.toUtf8().constData()));
    }

    require(colorPicker != nullptr && colorTrigger != nullptr && text != nullptr &&
                fontSize != nullptr && family != nullptr && templateSelect != nullptr &&
                angle != nullptr && gap != nullptr &&
                std::all_of(fontSizeButtons.cbegin(), fontSizeButtons.cend(),
                            [](QWidget* button) { return button != nullptr; }),
            "watermark controls should be present for physical-scale coverage");

    const QList<QWidget*> controls{
        colorPicker, colorTrigger, text, fontSize, family, templateSelect, angle, gap,
    };
    const QList<QSize> referenceSizes = [&controls, &fontSizeButtons]() {
        QList<QSize> sizes;
        for (QWidget* control : controls) {
            sizes.append(control->size());
        }
        for (QWidget* button : fontSizeButtons) {
            sizes.append(button->size());
        }
        return sizes;
    }();

    constexpr qreal toolbarCounterScale = 1.5;
    require(palette.setPhysicalScale(toolbarCounterScale), "watermark toolbar scale should change");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    QCoreApplication::processEvents();

    const auto expectedScaledSize = [](const QSize& size) {
        return QSize(qRound(size.width() * toolbarCounterScale),
                     qRound(size.height() * toolbarCounterScale));
    };
    QList<QWidget*> allControls = controls;
    allControls.append(fontSizeButtons);
    require(allControls.size() == referenceSizes.size(),
            "watermark scale references should cover every visible editor");
    for (qsizetype index = 0; index < allControls.size(); ++index) {
        const QSize expectedSize = expectedScaledSize(referenceSizes.at(index));
        const QSize actualSize = allControls.at(index)->size();
        require(qAbs(actualSize.width() - expectedSize.width()) <= 1 &&
                    qAbs(actualSize.height() - expectedSize.height()) <= 1,
                "watermark controls should follow the toolbar physical counter-scale");
    }
}

void shapeSelectorIsTheLeftmostStyleGroup() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);

    QWidget* controls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
    require(controls != nullptr, "rectangle style controls should be present");
    QLayout* layout = controls->layout();
    QWidget* shapeGroup =
        controls->findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup"));
    auto* strokeColor = colorPickerWithAccessibleName(palette, "Stroke color");
    QWidget* strokeWidth = controlWithTooltip(palette, "Current stroke width");
    QWidget* strokeRoot = styleEditorRoot(controls, "outline-stroke");
    QWidget* widthRoot = styleEditorRoot(controls, "outline-width");
    const QList<QFrame*> separators =
        controls->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly);
    require(layout != nullptr && shapeGroup != nullptr && strokeColor != nullptr &&
                strokeWidth != nullptr && strokeRoot != nullptr && widthRoot != nullptr &&
                separators.size() == 3,
            "shape, color, width, and separators should be present");
    require(layout->indexOf(shapeGroup) < layout->indexOf(separators.at(0)) &&
                layout->indexOf(separators.at(0)) < layout->indexOf(strokeRoot) &&
                layout->indexOf(strokeRoot) < layout->indexOf(separators.at(1)) &&
                layout->indexOf(separators.at(1)) < layout->indexOf(widthRoot) &&
                strokeRoot->isAncestorOf(strokeColor) && widthRoot->isAncestorOf(strokeWidth),
            "shape selector should be the leftmost style group");

    SnowCanvasShapeStyle emittedStyle;
    quint32 emittedProperties = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&emittedStyle, &emittedProperties](const SnowCanvasShapeStyle& style,
                                                         quint32 properties,
                                                         SnowCanvasShapeKind kind) {
                         require(kind == SnowCanvasShapeKind::Rectangle,
                                 "shape selector should patch the shape tool");
                         emittedStyle = style;
                         emittedProperties = properties;
                     });
    auto* diamond = qobject_cast<QAbstractButton*>(controlWithTooltip(palette, "Diamond"));
    require(diamond != nullptr, "diamond shape control should be present");
    for (const char* name : {"Rectangle", "Ellipse", "Diamond"}) {
        auto* shapeButton = qobject_cast<QAbstractButton*>(controlWithTooltip(palette, name));
        require(shapeButton != nullptr && shapeButton->text().isEmpty() &&
                    !shapeButton->icon().isNull() && shapeButton->iconSize() == QSize(16, 16),
                "shape controls should use 16px icons without visible text");
    }
    diamond->click();
    require(emittedProperties == SnowCanvasShapeStylePropertyShape &&
                emittedStyle.shape == SnowCanvasRectangleShape::Diamond,
            "diamond control should emit only the shape property");
}

void shapeSelectorIsExclusiveToTheShapeTool() {
    ScreenshotToolPalette::Options options;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);

    QPointer<QWidget> shapeGroup =
        palette.findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup"));
    require(!shapeGroup.isNull(), "shape selector should be present");

    require(!shapeGroup->isHidden(), "shape selector should be visible for the Shape tool");

    QPointer<QFrame> shapeSeparator =
        palette.findChild<QFrame*>(QStringLiteral("screenshotShapeStyleGroupSeparator"));
    palette.setActiveTool(ScreenshotToolPalette::Tool::Line);
    require(shapeGroup.isNull() && shapeSeparator.isNull(),
            "Line should destroy Shape-only controls and their separator");

    palette.setActiveTool(ScreenshotToolPalette::Tool::FreeDraw);
    require(palette.findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup")) == nullptr,
            "Free Draw should not materialize the Shape selector");

    SnowCanvasStyleToolbarState lineState;
    lineState.source = SnowCanvasStyleToolbarSource::DefaultLine;
    palette.setStyleToolbarState(lineState);
    require(palette.findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup")) == nullptr,
            "line style synchronization should keep the shape selector hidden");

    SnowCanvasStyleToolbarState rectangleState;
    rectangleState.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    palette.setStyleToolbarState(rectangleState);
    shapeGroup = palette.findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup"));
    shapeSeparator =
        palette.findChild<QFrame*>(QStringLiteral("screenshotShapeStyleGroupSeparator"));
    require(!shapeGroup.isNull() && !shapeGroup->isHidden(),
            "returning to the Shape tool should restore the shape selector");
    require(!shapeSeparator.isNull() && !shapeSeparator->isHidden(),
            "returning to the Shape tool should restore its group separator");
}

void arrowStyleUsesScreenshotCreationColorOverride() {
    SnowCanvasWidget canvas;
    require(canvas.setCanvasTool(SnowCanvasTool::Arrow), "canvas should activate the arrow tool");
    const SnowCanvasStyleToolbarState engineArrowDefaults = canvas.canvasStyleToolbarState();

    require(canvas.setCanvasShapeStylePatch(
                snow_shot::presentation::screenshotCanvasStyleDefaults().arrow,
                SnowCanvasShapeStylePropertyStrokeColor, SnowCanvasShapeKind::Arrow),
            "screenshot arrow color override should apply");
    const SnowCanvasStyleToolbarState screenshotArrowDefaults = canvas.canvasStyleToolbarState();

    require(screenshotArrowDefaults.source == SnowCanvasStyleToolbarSource::DefaultArrow,
            "screenshot arrow override should keep the arrow creation context");
    require(screenshotArrowDefaults.shapeStyle.stroke ==
                snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.stroke,
            "screenshot arrow default should use the rectangle stroke color");
    require(qFuzzyCompare(screenshotArrowDefaults.shapeStyle.strokeWidth + 1.0,
                          engineArrowDefaults.shapeStyle.strokeWidth + 1.0) &&
                screenshotArrowDefaults.shapeStyle.strokeStyle ==
                    engineArrowDefaults.shapeStyle.strokeStyle &&
                screenshotArrowDefaults.shapeStyle.arrowType ==
                    engineArrowDefaults.shapeStyle.arrowType &&
                screenshotArrowDefaults.shapeStyle.startArrowhead ==
                    engineArrowDefaults.shapeStyle.startArrowhead &&
                screenshotArrowDefaults.shapeStyle.endArrowhead ==
                    engineArrowDefaults.shapeStyle.endArrowhead,
            "screenshot arrow color override should preserve engine arrow defaults");
}

void requireControlInactive(ScreenshotToolPalette& palette, const char* tooltip,
                            const char* message) {
    auto* button =
        qobject_cast<adqt::widgets::AdButton*>(styleControlWithTooltip(palette, tooltip));
    require(button != nullptr, "expected style control is missing");
    require(button->buttonStyle() != adqt::widgets::AdButton::ButtonStyle::Tonal ||
                button->accentRole() != adqt::widgets::AdButton::AccentRole::Primary,
            message);
}

void arrowStyleControlsExposeAndEmitAllStyleProperties() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    SnowCanvasWidget canvas;
    require(canvas.setCanvasTool(SnowCanvasTool::Arrow), "canvas should activate the arrow tool");
    const SnowCanvasStyleToolbarState arrowState = canvas.canvasStyleToolbarState();
    require(arrowState.source == SnowCanvasStyleToolbarSource::DefaultArrow,
            "arrow toolbar state should come from the canvas");
    require(arrowState.shapeStyle.endArrowhead == SnowCanvasArrowhead::Arrow,
            "canvas arrow creation default should use the second end arrowhead option");
    palette.setStyleToolbarState(arrowState);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
    require(palette.styleToolbarVisible(), "the arrow tool should show the style toolbar");
    palette.show();
    QCoreApplication::processEvents();
    QWidget* arrowTypeControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotArrowTypeButtonGroup"));
    auto* arrowTypeGroup = arrowTypeControls == nullptr
                               ? nullptr
                               : arrowTypeControls->findChild<adqt::widgets::AdRadioButtonGroup*>();
    auto* elbowArrowType =
        qobject_cast<adqt::widgets::AdRadio*>(controlWithTooltip(palette, "Elbow arrow"));
    QWidget* startArrowheadControl = controlWithAccessibleName(palette, "Start arrowhead");
    QWidget* endArrowheadControl = controlWithAccessibleName(palette, "End arrowhead");
    require(arrowTypeGroup != nullptr &&
                arrowTypeGroup->variant() == adqt::widgets::AdRadio::Variant::Button &&
                arrowTypeGroup->controlSize() == adqt::widgets::AdRadio::ControlSize::Small,
            "arrow type should use the shape-style button group");
    require(elbowArrowType != nullptr, "elbow arrow type should be present");
    require(startArrowheadControl != nullptr, "start arrowhead control should be present");
    require(endArrowheadControl != nullptr, "end arrowhead control should be present");
    QWidget* arrowControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotArrowStyleControls"));
    require(arrowControls != nullptr, "arrow style controls should be present");
    QLayout* arrowLayout = arrowControls->layout();
    require(arrowLayout != nullptr, "arrow style controls should have a layout");
    const QList<QFrame*> separators =
        arrowControls->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly);
    require(separators.size() == 3,
            "arrow style controls should separate color, width, type, and endpoint settings");
    QWidget* arrowStrokeColorControl = controlWithAccessibleName(palette, "Arrow stroke color");
    QWidget* arrowStrokeWidthControl = controlWithTooltip(palette, "Current arrow stroke width");
    QWidget* arrowStrokeRoot = styleEditorRoot(arrowControls, "outline-stroke");
    QWidget* arrowWidthRoot = styleEditorRoot(arrowControls, "outline-width");
    require(arrowStrokeColorControl != nullptr && arrowStrokeWidthControl != nullptr &&
                arrowStrokeRoot != nullptr && arrowWidthRoot != nullptr,
            "arrow stroke color and width controls should be present");
    require(arrowLayout->indexOf(arrowStrokeRoot) < arrowLayout->indexOf(separators.at(0)) &&
                arrowLayout->indexOf(separators.at(0)) < arrowLayout->indexOf(arrowWidthRoot) &&
                arrowStrokeRoot->isAncestorOf(arrowStrokeColorControl) &&
                arrowWidthRoot->isAncestorOf(arrowStrokeWidthControl),
            "arrow stroke color should be the leftmost style group");
    require(arrowLayout->indexOf(arrowWidthRoot) < arrowLayout->indexOf(separators.at(1)) &&
                arrowLayout->indexOf(separators.at(1)) < arrowLayout->indexOf(arrowTypeControls),
            "arrow stroke width should remain between color and arrow type");

    QWidget* shaftControl = controlWithAccessibleName(palette, "Arrow shaft type");
    QWidget* shaftRoot = styleEditorRoot(arrowControls, "arrow-shaft-type");
    require(shaftControl != nullptr && shaftRoot != nullptr, "shaft editor should be present");
    require(arrowLayout->indexOf(styleEditorRoot(arrowControls, "start-arrowhead")) <
                    arrowLayout->indexOf(shaftRoot) &&
                arrowLayout->indexOf(shaftRoot) <
                    arrowLayout->indexOf(styleEditorRoot(arrowControls, "end-arrowhead")),
            "shaft editor must be between the endpoint editors");
    int arrowPopoverOptionSpacing = -1;
    for (QWidget* trigger : {
             startArrowheadControl,
             endArrowheadControl,
         }) {
        adqt::widgets::AdPopover* popover = popoverForTrigger(trigger);
        require(popover != nullptr, "arrow control should have a popup layer");
        require(popover->triggers() == adqt::widgets::AdPopover::Trigger::Hover,
                "arrow control popup should open on hover");
        require(popover->arrowVisible(), "arrow control popup should show its placement arrow");
        require(qobject_cast<adqt::widgets::AdButton*>(trigger) != nullptr,
                "arrow popover trigger should use the shared preview button");
        require(trigger->focusPolicy() == Qt::NoFocus,
                "arrow popover trigger should match the color picker trigger focus behavior");
        materializeLazyPopover(trigger);
        QWidget* content = popover->contentWidget();
        require(content != nullptr, "arrow popover content should be present");
        QLayout* optionLayout = content->layout();
        require(optionLayout != nullptr, "arrow popover options should have a layout");
        if (arrowPopoverOptionSpacing < 0) {
            arrowPopoverOptionSpacing = optionLayout->spacing();
        }
        require(optionLayout->spacing() == arrowPopoverOptionSpacing,
                "arrow popover option spacing should be consistent");
    }

    adqt::widgets::AdPopover* endArrowheadPopover = popoverForTrigger(endArrowheadControl);
    require(endArrowheadPopover != nullptr, "end arrowhead control should have a popup layer");
    QWidget* endArrowheadContent = endArrowheadPopover->contentWidget();
    require(endArrowheadContent != nullptr, "end arrowhead popup content should be present");
    adqt::widgets::AdButton* defaultEndArrowhead = nullptr;
    for (QWidget* control : endArrowheadContent->findChildren<QWidget*>()) {
        if (control != nullptr && control->toolTip() == QStringLiteral("End arrowhead standard")) {
            defaultEndArrowhead = qobject_cast<adqt::widgets::AdButton*>(control);
            break;
        }
    }
    require(defaultEndArrowhead != nullptr, "second end arrowhead option should be present");
    require(defaultEndArrowhead->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Tonal,
            "end arrowhead should default to the second option");

    SnowCanvasShapeStyle emittedStyle;
    int styleChangeCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&emittedStyle, &styleChangeCount](const SnowCanvasShapeStyle& style, quint32,
                                                        SnowCanvasShapeKind kind) {
                         if (kind == SnowCanvasShapeKind::Arrow) {
                             emittedStyle = style;
                             ++styleChangeCount;
                         }
                     });

    clickStyleControl(palette, "Arrow stroke width 4");
    require(styleChangeCount == 1, "arrow stroke width should emit once");
    clickStyleControl(palette, "Arrow stroke color #f5222d");
    require(styleChangeCount == 2, "arrow stroke color should emit once");
    clickStyleControl(palette, "Dashed arrow stroke");
    require(styleChangeCount == 3, "arrow stroke style should emit once");
    elbowArrowType->click();
    require(styleChangeCount == 4, "arrow type should emit once");
    clickPopoverStyleControl(showPopoverForTrigger(startArrowheadControl),
                             "Start arrowhead triangle");
    require(styleChangeCount == 5, "start arrowhead should emit once");
    clickPopoverStyleControl(showPopoverForTrigger(endArrowheadControl),
                             "End arrowhead diamond outline");

    require(styleChangeCount == 6, "each arrow style edit should emit once");
    require(qFuzzyCompare(emittedStyle.strokeWidth + 1.0, 5.0), "arrow stroke width should update");
    require(emittedStyle.stroke == QColor(QStringLiteral("#f5222d")),
            "arrow stroke color should update");
    require(emittedStyle.strokeStyle == SnowCanvasStrokeStyle::Dashed,
            "arrow stroke style should update");
    require(emittedStyle.arrowType == SnowCanvasArrowType::Elbow, "arrow type should update");
    require(emittedStyle.startArrowhead == SnowCanvasArrowhead::Triangle,
            "start arrowhead should update");
    require(emittedStyle.endArrowhead == SnowCanvasArrowhead::DiamondOutline,
            "end arrowhead should update");

    clickPopoverStyleControl(showPopoverForTrigger(shaftControl), "Tapered shaft");
    require(emittedStyle.arrowShaftType == SnowCanvasArrowShaftType::Tapered,
            "shaft editor should emit the tapered preference even for an unsupported head");
    clickPopoverStyleControl(showPopoverForTrigger(endArrowheadControl), "End arrowhead triangle");
    require(emittedStyle.arrowShaftType == SnowCanvasArrowShaftType::Tapered,
            "supported head must retain the tapered preference");
    clickPopoverStyleControl(showPopoverForTrigger(shaftControl), "Plain shaft");
    require(emittedStyle.arrowShaftType == SnowCanvasArrowShaftType::Plain,
            "plain shaft must be selectable");
    for (const auto& [trigger, label, triangleLabel] : {
             std::tuple{startArrowheadControl, "Start arrowhead indented triangle",
                        "Start arrowhead triangle"},
             std::tuple{endArrowheadControl, "End arrowhead indented triangle",
                        "End arrowhead triangle"},
         }) {
        auto* popover = showPopoverForTrigger(trigger);
        auto* content = popover->contentWidget();
        adqt::widgets::AdButton* indented = nullptr;
        adqt::widgets::AdButton* triangle = nullptr;
        for (auto* button : content->findChildren<adqt::widgets::AdButton*>()) {
            if (button->toolTip() == QString::fromLatin1(label)) {
                indented = button;
            }
            if (button->toolTip() == QString::fromLatin1(triangleLabel)) {
                triangle = button;
            }
        }
        require(indented != nullptr && triangle != nullptr, "both triangle options should exist");
        require(indented->geometry().top() == triangle->geometry().top() &&
                    indented->geometry().right() < triangle->geometry().left(),
                "indented triangle should sit immediately left of the filled triangle");
        clickPopoverStyleControl(popover, label);
        showPopoverForTrigger(trigger);
        require(indented->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Tonal,
                "reopening the picker should retain the indented triangle selection");
    }
    require(styleChangeCount == 11 &&
                emittedStyle.startArrowhead == SnowCanvasArrowhead::IndentedTriangle &&
                emittedStyle.endArrowhead == SnowCanvasArrowhead::IndentedTriangle,
            "both indented endpoints should emit exactly once and preserve each other");
}

void lineStyleControlsExposeStraightAndCurveTypes() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Line);
    palette.show();
    QCoreApplication::processEvents();

    QWidget* lineControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotLineStyleControls"));
    QWidget* lineTypeControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotLineTypeButtonGroup"));
    auto* lineTypeGroup = lineTypeControls == nullptr
                              ? nullptr
                              : lineTypeControls->findChild<adqt::widgets::AdRadioButtonGroup*>();
    auto* straightLine =
        qobject_cast<adqt::widgets::AdRadio*>(controlWithTooltip(palette, "Straight line"));
    auto* curvedLine =
        qobject_cast<adqt::widgets::AdRadio*>(controlWithTooltip(palette, "Curved line"));
    require(lineControls != nullptr && lineTypeControls != nullptr && lineTypeGroup != nullptr &&
                straightLine != nullptr && curvedLine != nullptr,
            "Line should expose a two-option type button group");
    require(lineTypeGroup->buttons().size() == 2 && lineTypeGroup->id(straightLine) == 0 &&
                lineTypeGroup->id(curvedLine) == 1 && lineTypeGroup->checkedId() == 1,
            "Line should expose Straight as ID 0 and default to Curved as ID 1");

    QLayout* lineLayout = lineControls->layout();
    QWidget* strokeRoot = styleEditorRoot(lineControls, "outline-stroke");
    QWidget* widthRoot = styleEditorRoot(lineControls, "outline-width");
    QWidget* fillRoot = styleEditorRoot(lineControls, "shape-fill");
    const QList<QFrame*> separators =
        lineControls->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly);
    require(lineLayout != nullptr && strokeRoot != nullptr && widthRoot != nullptr &&
                fillRoot != nullptr && separators.size() == 3,
            "Line should expose the expected style groups and separators");
    require(lineLayout->indexOf(strokeRoot) < lineLayout->indexOf(separators.at(0)) &&
                lineLayout->indexOf(separators.at(0)) < lineLayout->indexOf(widthRoot) &&
                lineLayout->indexOf(widthRoot) < lineLayout->indexOf(separators.at(1)) &&
                lineLayout->indexOf(separators.at(1)) < lineLayout->indexOf(lineTypeControls) &&
                lineLayout->indexOf(lineTypeControls) < lineLayout->indexOf(separators.at(2)) &&
                lineLayout->indexOf(separators.at(2)) < lineLayout->indexOf(fillRoot),
            "Line controls should be ordered stroke color, width, type, then fill color");

    ScreenshotToolPalette arrowPalette(ScreenshotToolPalette::Options{});
    arrowPalette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
    arrowPalette.show();
    QCoreApplication::processEvents();
    auto* straightArrow =
        qobject_cast<adqt::widgets::AdRadio*>(controlWithTooltip(arrowPalette, "Straight arrow"));
    auto* curvedArrow =
        qobject_cast<adqt::widgets::AdRadio*>(controlWithTooltip(arrowPalette, "Curved arrow"));
    require(straightArrow != nullptr && curvedArrow != nullptr && !straightLine->icon().isNull() &&
                !curvedLine->icon().isNull(),
            "Line type options should render dedicated line icons");
    require(straightLine->icon().pixmap(16, 16).toImage() !=
                    straightArrow->icon().pixmap(16, 16).toImage() &&
                curvedLine->icon().pixmap(16, 16).toImage() !=
                    curvedArrow->icon().pixmap(16, 16).toImage(),
            "Line type options should be distinct from the Arrow SVG assets");

    SnowCanvasShapeStyle emittedStyle;
    quint32 emittedProperties = 0;
    SnowCanvasShapeKind emittedKind = SnowCanvasShapeKind::Rectangle;
    int styleChangeCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&emittedStyle, &emittedProperties, &emittedKind,
                      &styleChangeCount](const SnowCanvasShapeStyle& style, quint32 properties,
                                         SnowCanvasShapeKind kind) {
                         emittedStyle = style;
                         emittedProperties = properties;
                         emittedKind = kind;
                         ++styleChangeCount;
                     });

    straightLine->click();
    require(styleChangeCount == 1 && emittedKind == SnowCanvasShapeKind::Line &&
                emittedProperties == SnowCanvasShapeStylePropertyArrowType &&
                emittedStyle.arrowType == SnowCanvasArrowType::Straight &&
                palette.creationStyleDefaults().line.arrowType == SnowCanvasArrowType::Straight,
            "selecting Straight should emit only the Line arrow-type property and mirror defaults");
    curvedLine->click();
    require(styleChangeCount == 2 && emittedKind == SnowCanvasShapeKind::Line &&
                emittedProperties == SnowCanvasShapeStylePropertyArrowType &&
                emittedStyle.arrowType == SnowCanvasArrowType::Curve &&
                palette.creationStyleDefaults().line.arrowType == SnowCanvasArrowType::Curve,
            "selecting Curved should emit only the Line arrow-type property and mirror defaults");

    SnowCanvasStyleToolbarState selectedState;
    selectedState.source = SnowCanvasStyleToolbarSource::SelectedLine;
    selectedState.shapeStyle = palette.creationStyleDefaults().line;
    selectedState.shapeStyleMixed = 0;
    palette.setStyleToolbarState(selectedState);
    require(lineTypeGroup->checkedId() == 1,
            "a selected curved Line should select the Curved option");
    selectedState.shapeStyle.arrowType = SnowCanvasArrowType::Straight;
    palette.setStyleToolbarState(selectedState);
    require(lineTypeGroup->checkedId() == 0,
            "an external selected Line type change should refresh the option");
    selectedState.shapeStyle.arrowType = SnowCanvasArrowType::Curve;
    selectedState.shapeStyleMixed = SnowCanvasShapeStyleMixedArrowType;
    palette.setStyleToolbarState(selectedState);
    require(lineTypeGroup->checkedId() == -1,
            "mixed selected Line types should clear the checked option");
    styleChangeCount = 0;
    straightLine->click();
    require(styleChangeCount == 1 && lineTypeGroup->checkedId() == 0 &&
                emittedKind == SnowCanvasShapeKind::Line &&
                emittedProperties == SnowCanvasShapeStylePropertyArrowType,
            "choosing a Line type should resolve only the mixed type property");

    palette.setActiveTool(ScreenshotToolPalette::Tool::FreeDraw);
    require(palette.findChild<QWidget*>(QStringLiteral("screenshotLineTypeButtonGroup")) == nullptr,
            "Free Draw should not expose the Line type editor");
}

void arrowRatioEditorAdjustsAndResets() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
    palette.show();
    QCoreApplication::processEvents();
    const auto ratioEditor = [&palette]() {
        return dynamic_cast<IconNumericValuePreviewButton*>(
            palette.findChild<QWidget*>(QStringLiteral("screenshotArrowRatioButton")));
    };
    auto* editor = ratioEditor();
    require(editor != nullptr && editor->valueText() == QStringLiteral("1.0"),
            "arrow ratio starts at 1.0 with one decimal place");
    auto* row = editor->parentWidget()->layout();
    auto* arrowType = palette.findChild<QWidget*>(QStringLiteral("screenshotArrowTypeButtonGroup"));
    QWidget* start = controlWithAccessibleName(palette, "Start arrowhead");
    const auto rootInRow = [row](QWidget* widget) {
        while (widget != nullptr && row->indexOf(widget) < 0)
            widget = widget->parentWidget();
        return widget;
    };
    QVector<QWidget*> rowWidgets;
    for (int i = 0; i < row->count(); ++i) {
        if (QWidget* widget = row->itemAt(i)->widget())
            rowWidgets.push_back(widget);
    }
    const qsizetype ratioIndex = rowWidgets.indexOf(editor);
    require(ratioIndex >= 2 && ratioIndex + 1 < rowWidgets.size() &&
                rowWidgets.at(ratioIndex - 2) == rootInRow(arrowType) &&
                qobject_cast<QFrame*>(rowWidgets.at(ratioIndex - 1)) != nullptr &&
                rowWidgets.at(ratioIndex + 1) == rootInRow(start),
            "arrow type, separator, ratio, and start arrowhead must be consecutive");
    require(editor->cursor().shape() == Qt::SplitVCursor,
            "ratio uses the same adjustment cursor as rounded corners");
    const auto wheel = [&palette, &editor](int delta) {
        const QPoint local = editor->rect().center();
        QWheelEvent event(QPointF(local), editor->mapToGlobal(local), QPoint(), QPoint(0, delta),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        require(palette.handleToolbarWheel(&event) && event.isAccepted(),
                "ratio wheel is consumed");
    };
    SnowCanvasShapeStyle emitted;
    quint32 properties = 0;
    int changes = 0;
    QObject::connect(
        &palette, &ScreenshotToolPalette::shapeStyleChanged, &palette,
        [&](const SnowCanvasShapeStyle& style, quint32 changed, SnowCanvasShapeKind kind) {
            require(kind == SnowCanvasShapeKind::Arrow, "ratio edits target arrows");
            emitted = style;
            properties = changed;
            ++changes;
        });
    const auto initial = palette.creationStyleDefaults().arrow;
    wheel(120);
    auto expected = initial;
    expected.arrowRatio = 1.1;
    require(palette.creationStyleDefaults().arrow == expected && emitted.arrowRatio == 1.1 &&
                emitted.strokeWidth == initial.strokeWidth &&
                properties == SnowCanvasShapeStylePropertyArrowRatio &&
                editor->valueText() == QStringLiteral("1.1"),
            "one wheel step updates only the ratio by 0.1");
    const QSize initialSize = editor->size();
    for (int i = 0; i < 30; ++i)
        wheel(120);
    require(editor->value() == 3.0 && editor->size() == initialSize,
            "ratio clamps at 3.0 without changing editor size");
    const int atMaximum = changes;
    wheel(120);
    require(changes == atMaximum, "clamped ratio is a no-op");
    for (int i = 0; i < 30; ++i)
        wheel(-120);
    require(editor->value() == 1.0, "ratio clamps at 1.0");
    wheel(120);
    editor->click();
    require(editor->valueText() == QStringLiteral("1.0"), "click resets ratio");
    SnowCanvasStyleToolbarState selected;
    selected.source = SnowCanvasStyleToolbarSource::SelectedArrow;
    selected.shapeStyle = initial;
    selected.shapeStyle.arrowRatio = 2.4;
    selected.shapeStyleMixed =
        SnowCanvasShapeStyleMixedArrowRatio | SnowCanvasShapeStyleMixedStroke;
    palette.setStyleToolbarState(selected);
    require(editor->valueText() == QStringLiteral("-"), "mixed ratios show a dash");
    wheel(120);
    require(editor->valueText() == QStringLiteral("2.5") &&
                properties == SnowCanvasShapeStylePropertyArrowRatio,
            "scroll resolves just the mixed ratio");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
    QCoreApplication::processEvents();
    editor = ratioEditor();
    require(editor != nullptr && editor->value() == 2.5,
            "arrow ratio survives editor reuse and tool switching");
    palette.hide();
}

void arrowheadOptionsRetranslateInPlace() {
    auto& languageManager = snow_shot::presentation::LanguageManager::instance();
    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English should be active before testing arrowhead retranslation");

    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    SnowCanvasWidget canvas;
    require(canvas.setCanvasTool(SnowCanvasTool::Arrow),
            "canvas should activate the arrow tool for retranslation");
    palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
    palette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
    palette.show();
    QCoreApplication::processEvents();

    QWidget* startTrigger = controlWithAccessibleName(palette, "Start arrowhead");
    require(startTrigger != nullptr, "start arrowhead trigger should be present");
    auto* popover = showPopoverForTrigger(startTrigger);
    auto* noneOption = popoverButtonWithTooltip(popover, "Start arrowhead none");
    require(noneOption != nullptr, "English arrowhead option should be present");
    auto* indentedOption = popoverButtonWithTooltip(popover, "Start arrowhead indented triangle");
    require(indentedOption != nullptr, "indented triangle should have an English tooltip");
    auto* standardOption = popoverButtonWithTooltip(popover, "Start arrowhead standard");
    require(standardOption != nullptr &&
                adqt::icons::describeIcon(standardOption->iconRef()).key.name ==
                    QStringLiteral("arrowhead-standard-start"),
            "start arrowhead options should use the left-facing asset");
    popover->hide();
    auto* endPopover = showPopoverForTrigger(controlWithAccessibleName(palette, "End arrowhead"));
    auto* endOption = popoverButtonWithTooltip(endPopover, "End arrowhead standard");
    require(endOption != nullptr && adqt::icons::describeIcon(endOption->iconRef()).key.name ==
                                        QStringLiteral("arrowhead-standard"),
            "end arrowhead options should use the right-facing asset");
    endPopover->hide();
    QWidget* ratioTrigger = controlWithTooltip(palette, "Arrow ratio (scroll to adjust)");
    require(ratioTrigger != nullptr, "ratio editor must expose translated accessible text");
    QWidget* shaftTrigger = controlWithAccessibleName(palette, "Arrow shaft type");
    require(shaftTrigger != nullptr, "shaft trigger must be available");
    auto* shaftPopover = showPopoverForTrigger(shaftTrigger);
    auto* taperedOption = popoverButtonWithTooltip(shaftPopover, "Tapered shaft");
    require(taperedOption != nullptr &&
                adqt::icons::describeIcon(taperedOption->iconRef()).key.name ==
                    QStringLiteral("arrow-shaft-tapered"),
            "tapered shaft uses its hand-drawn asset");
    shaftPopover->hide();
    popover = showPopoverForTrigger(startTrigger);

    require(languageManager.setLanguage(QStringLiteral("zh_CN")),
            "Simplified Chinese should load for arrowhead retranslation");
    QCoreApplication::processEvents();
    require(startTrigger->accessibleName() == QStringLiteral("\u8d77\u59cb\u7bad\u5934"),
            "arrowhead trigger should retranslate to Simplified Chinese");
    require(noneOption->toolTip() == QStringLiteral("\u8d77\u59cb\u7bad\u5934 \u65e0"),
            "open arrowhead option should retranslate to Simplified Chinese");
    require(ratioTrigger->accessibleName() == QStringLiteral("箭头比例（滚动调整）"),
            "ratio editor retranslates to Simplified Chinese");
    require(shaftTrigger->accessibleName() == QStringLiteral("箭杆类型") &&
                taperedOption->toolTip() == QStringLiteral("渐宽箭杆"),
            "shaft editor must retranslate to Simplified Chinese");
    require(indentedOption->toolTip() == QStringLiteral("起始箭头 内凹三角形"),
            "indented triangle should retranslate to Simplified Chinese");

    require(languageManager.setLanguage(QStringLiteral("zh_TW")),
            "Traditional Chinese should load for arrowhead retranslation");
    QCoreApplication::processEvents();
    require(startTrigger->accessibleName() == QStringLiteral("\u8d77\u59cb\u7bad\u982d"),
            "arrowhead trigger should retranslate to Traditional Chinese");
    require(noneOption->toolTip() == QStringLiteral("\u8d77\u59cb\u7bad\u982d \u7121"),
            "open arrowhead option should retranslate to Traditional Chinese");
    require(ratioTrigger->accessibleName() == QStringLiteral("箭頭比例（捲動調整）"),
            "ratio editor retranslates to Traditional Chinese");
    require(shaftTrigger->accessibleName() == QStringLiteral("箭桿類型") &&
                taperedOption->toolTip() == QStringLiteral("漸寬箭桿"),
            "shaft editor must retranslate to Traditional Chinese");
    require(indentedOption->toolTip() == QStringLiteral("起始箭頭 內凹三角形"),
            "indented triangle should retranslate to Traditional Chinese");

    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English should be restorable after arrowhead retranslation");
    QCoreApplication::processEvents();
    require(startTrigger->accessibleName() == QStringLiteral("Start arrowhead") &&
                noneOption->toolTip() == QStringLiteral("Start arrowhead none"),
            "open arrowhead controls should restore English");
    palette.hide();
}

void selectedArrowMixedPropertiesResolveIndependently() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);

    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::SelectedArrow;
    state.shapeStyle.stroke = QColor(QStringLiteral("#f5222d"));
    state.shapeStyle.strokeWidth = 4.0;
    state.shapeStyle.strokeStyle = SnowCanvasStrokeStyle::Dashed;
    state.shapeStyle.arrowType = SnowCanvasArrowType::Elbow;
    state.shapeStyle.startArrowhead = SnowCanvasArrowhead::Triangle;
    state.shapeStyle.endArrowhead = SnowCanvasArrowhead::Diamond;
    state.shapeStyleMixed =
        SnowCanvasShapeStylePropertyStrokeWidth | SnowCanvasShapeStylePropertyStrokeColor |
        SnowCanvasShapeStylePropertyStrokeStyle | SnowCanvasShapeStylePropertyStartArrowhead |
        SnowCanvasShapeStylePropertyEndArrowhead | SnowCanvasShapeStylePropertyArrowType |
        SnowCanvasShapeStylePropertyArrowShaftType;
    palette.setStyleToolbarState(state);

    requireControlInactive(palette, "Arrow stroke width 4",
                           "mixed arrow stroke width must not select a preset");
    requireControlInactive(palette, "Arrow stroke color #f5222d",
                           "mixed arrow color must not select a preset");
    requireControlInactive(palette, "Dashed arrow stroke",
                           "mixed arrow stroke style must not select an option");

    QWidget* arrowTypeControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotArrowTypeButtonGroup"));
    auto* arrowTypeGroup = arrowTypeControls == nullptr
                               ? nullptr
                               : arrowTypeControls->findChild<adqt::widgets::AdRadioButtonGroup*>();
    auto* elbowArrowType =
        qobject_cast<adqt::widgets::AdRadio*>(controlWithTooltip(palette, "Elbow arrow"));
    QWidget* startArrowheadControl = controlWithAccessibleName(palette, "Start arrowhead");
    QWidget* endArrowheadControl = controlWithAccessibleName(palette, "End arrowhead");
    require(arrowTypeGroup != nullptr, "arrow type group should be present");
    require(elbowArrowType != nullptr, "elbow arrow type should be present");
    require(startArrowheadControl != nullptr, "start arrowhead control should be present");
    require(endArrowheadControl != nullptr, "end arrowhead control should be present");

    auto* shaftPopover =
        showPopoverForTrigger(controlWithAccessibleName(palette, "Arrow shaft type"));
    adqt::widgets::AdPopover* startArrowheadPopover = showPopoverForTrigger(startArrowheadControl);
    adqt::widgets::AdPopover* endArrowheadPopover = showPopoverForTrigger(endArrowheadControl);
    require(arrowTypeGroup->checkedId() == -1 && !elbowArrowType->isChecked(),
            "mixed arrow type should not select a button-group option");
    for (const auto& option : {
             std::pair{startArrowheadPopover, "Start arrowhead triangle"},
             std::pair{endArrowheadPopover, "End arrowhead diamond"},
             std::pair{shaftPopover, "Plain shaft"},
         }) {
        adqt::widgets::AdButton* button = popoverButtonWithTooltip(option.first, option.second);
        require(button != nullptr, "mixed arrow option should be present");
        require(button->buttonStyle() != adqt::widgets::AdButton::ButtonStyle::Tonal ||
                    button->accentRole() != adqt::widgets::AdButton::AccentRole::Primary,
                "mixed arrow values must not select a popover option");
    }

    quint32 emittedProperties = 0;
    SnowCanvasShapeKind emittedKind = SnowCanvasShapeKind::Rectangle;
    int styleChangeCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&emittedProperties, &emittedKind,
                      &styleChangeCount](const SnowCanvasShapeStyle&, quint32 properties,
                                         SnowCanvasShapeKind kind) {
                         emittedProperties = properties;
                         emittedKind = kind;
                         ++styleChangeCount;
                     });

    elbowArrowType->click();
    require(styleChangeCount == 1, "one arrow mixed-value edit should emit once");
    require(emittedKind == SnowCanvasShapeKind::Arrow &&
                emittedProperties == SnowCanvasShapeStylePropertyArrowType,
            "the selected arrow type must emit only its arrow property");
    require(arrowTypeGroup->checkedId() == 2 && elbowArrowType->isChecked(),
            "the explicitly selected arrow type should become active");
    requireControlInactive(palette, "Arrow stroke width 4",
                           "resolving arrow type must preserve mixed width");
    for (const auto& option : {
             std::pair{startArrowheadPopover, "Start arrowhead triangle"},
             std::pair{endArrowheadPopover, "End arrowhead diamond"},
             std::pair{shaftPopover, "Plain shaft"},
         }) {
        adqt::widgets::AdButton* button = popoverButtonWithTooltip(option.first, option.second);
        require(button != nullptr, "mixed arrowhead option should be present");
        require(button->buttonStyle() != adqt::widgets::AdButton::ButtonStyle::Tonal ||
                    button->accentRole() != adqt::widgets::AdButton::AccentRole::Primary,
                "resolving arrow type must preserve mixed arrowheads");
    }

    for (adqt::widgets::AdPopover* popover : {
             startArrowheadPopover,
             endArrowheadPopover,
         }) {
        popover->hide();
    }
    QCoreApplication::processEvents();
    clickPopoverStyleControl(shaftPopover, "Tapered shaft");
    require(emittedProperties == SnowCanvasShapeStylePropertyArrowShaftType &&
                styleChangeCount == 2,
            "resolving mixed shafts emits only the shaft property once");
}

void styleToolbarWidthTracksTheActiveTool() {
    ScreenshotToolPalette::Options options;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    ScreenshotToolPalette palette(options);

    const auto expectedPanelSize = [&palette](const char* objectName) {
        QWidget* controls = palette.findChild<QWidget*>(QString::fromUtf8(objectName));
        require(controls != nullptr, "active style controls should be present");
        require(controls->layout() != nullptr, "style controls should have a layout");
        controls->layout()->activate();

        const QMargins margins = palette.stylePanel()->layout()->contentsMargins();
        return controls->sizeHint() +
               QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    };

    struct ToolExpectation {
        ScreenshotToolPalette::Tool tool;
        const char* controlsObjectName;
    };
    const ToolExpectation expectations[] = {
        {ScreenshotToolPalette::Tool::Shape, "screenshotRectangleStyleControls"},
        {ScreenshotToolPalette::Tool::Arrow, "screenshotArrowStyleControls"},
        {ScreenshotToolPalette::Tool::Text, "screenshotTextStyleControls"},
        {ScreenshotToolPalette::Tool::SerialNumber, "screenshotSerialNumberStyleControls"},
        {ScreenshotToolPalette::Tool::Arrow, "screenshotArrowStyleControls"},
    };

    QSize previousSize;
    bool widthChanged = false;
    for (const ToolExpectation& expectation : expectations) {
        palette.setActiveTool(expectation.tool);
        QCoreApplication::processEvents();

        const QSize expected = expectedPanelSize(expectation.controlsObjectName);
        require(palette.stylePanel()->size() == expected,
                "style toolbar should be resized to the active tool's controls");
        if (!previousSize.isEmpty() && previousSize.width() != expected.width()) {
            widthChanged = true;
        }
        previousSize = expected;
    }
    require(widthChanged, "switching style tools should exercise different toolbar widths");
}

void selectPopupPreservesModelFontRole() {
    adqt::widgets::AdSelect select;
    auto* model = new QStandardItemModel(&select);
    auto* item = new QStandardItem(QStringLiteral("Font Preview Family"));
    item->setData(QStringLiteral("font-preview-family"), adqt::widgets::AdSelect::DefaultValueRole);
    item->setData(QStringLiteral("Font Preview Family"), adqt::widgets::AdSelect::DefaultLabelRole);
    item->setData(QFont(QStringLiteral("Font Preview Family")), Qt::FontRole);
    model->appendRow(item);

    select.setModel(model);
    const QAbstractItemModel* popupModel = select.view()->model();
    require(popupModel != nullptr && popupModel->rowCount() == 1,
            "select popup should expose the model option");
    const QFont popupFont = qvariant_cast<QFont>(popupModel->index(0, 0).data(Qt::FontRole));
    require(popupFont.family() == QStringLiteral("Font Preview Family"),
            "select popup should render an option with its model font");
}

void textStyleControlsExposeAndEmitAllRequestedProperties() {
    ScreenshotToolPalette::Options options;
    options.showTextTool = true;
    ScreenshotToolPalette palette(options);

    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::DefaultText;
    state.textStyle.color = QColor(QStringLiteral("#f4212c"));
    state.textStyle.fontSize = 30.0;
    state.textStyle.fontFamily.clear();
    state.textStyle.stroke = QColor(QStringLiteral("#ffccc7"));
    state.textStyle.strokeWidth = 0.0;
    state.textStyle.fill = QColor(0, 0, 0, 0);
    state.textStyle.fillStyle = SnowCanvasFillStyle::Solid;
    state.textStyle.cornerRadii = SnowCanvasCornerRadii{6.0, 6.0, 6.0, 6.0};
    state.textStyle.horizontalAlign = SnowCanvasTextHorizontalAlign::Left;
    state.textStyle.verticalAlign = SnowCanvasTextVerticalAlign::Bottom;
    state.textStyle.opacity = 0.65;
    palette.setStyleToolbarState(state);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    palette.show();
    QCoreApplication::processEvents();

    require(palette.styleToolbarVisible(), "text tool should show its style toolbar");
    QWidget* textControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls"));
    require(textControls != nullptr && textControls->isVisible(),
            "text style controls should be visible");
    QWidget* rectangleControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
    QWidget* arrowControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotArrowStyleControls"));
    require((rectangleControls == nullptr || !rectangleControls->isVisible()) &&
                (arrowControls == nullptr || !arrowControls->isVisible()),
            "text mode should hide rectangle and arrow controls");

    auto* fontSizeSummary = controlWithTooltip(palette, "Current text font size");
    require(fontSizeSummary != nullptr, "text font-size summary should be present");
    require(fontSizeSummary->accessibleDescription() == QStringLiteral("30px"),
            "text font-size summary should display the exact value and unit");
    require(fontSizeSummary->cursor().shape() == Qt::SplitVCursor,
            "text font-size summary should use the vertical split cursor");
    const QStringList fontSizePresetTooltips{
        QStringLiteral("Text font size S (24px)"),
        QStringLiteral("Text font size M (30px)"),
        QStringLiteral("Text font size L (42px)"),
        QStringLiteral("Text font size XL (54px)"),
    };
    adqt::widgets::AdButton* activeFontSizePreset = nullptr;
    for (const QString& tooltip : fontSizePresetTooltips) {
        auto* preset = qobject_cast<adqt::widgets::AdButton*>(
            controlWithTooltip(palette, tooltip.toUtf8().constData()));
        require(preset != nullptr && preset->text().isEmpty(),
                "text font-size presets should use icons instead of button text");
        if (tooltip == QStringLiteral("Text font size M (30px)")) {
            activeFontSizePreset = preset;
        }
    }
    require(activeFontSizePreset != nullptr &&
                activeFontSizePreset->buttonStyle() ==
                    adqt::widgets::AdButton::ButtonStyle::Tonal &&
                activeFontSizePreset->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
            "the active text style button should use the shared style-toolbar active state");
    auto* fontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Text font family"));
    require(fontSelect != nullptr, "text font-family select should be present");
    require(fontSelect->placeholder() == QStringLiteral("Font family"),
            "text font-family select should describe its empty state");
    require(fontSelect->variant() == adqt::widgets::AdSelect::Variant::Borderless,
            "text font-family select should be borderless");
    require(fontSelect->popupLayerMode() == adqt::widgets::AdSelect::PopupLayerMode::QtTool,
            "text font-family select should use QtTool");
    require(fontSelect->model() != nullptr &&
                fontSelect->model()->rowCount() ==
                    snow_shot::presentation::screenshotToolPaletteFontFamilies().size() + 2,
            "text font-family select should include Default and installed fonts");
    require(fontSelect->model()->index(0, 0).data(adqt::widgets::AdSelect::DefaultLabelRole) ==
                    QStringLiteral("Default") &&
                fontSelect->model()
                    ->index(0, 0)
                    .data(adqt::widgets::AdSelect::DefaultValueRole)
                    .toString()
                    .isEmpty(),
            "text font-family select should place Default at the top");
    const auto fontSortComparator = fontSelect->sortComparator();
    const adqt::widgets::AdSelect::Option defaultFont{
        QVariant(),
        QStringLiteral("Default"),
    };
    const adqt::widgets::AdSelect::Option installedFont{
        QStringLiteral("Arial"),
        QStringLiteral("Arial"),
    };
    require(fontSortComparator && fontSortComparator(defaultFont, installedFont) &&
                !fontSortComparator(installedFont, defaultFont),
            "text font-family popup should keep Default ahead of installed fonts");
    const auto pickerWithName = [&palette](const QString& name) {
        for (adqt::widgets::AdColorPicker* picker :
             palette.findChildren<adqt::widgets::AdColorPicker*>()) {
            if (picker != nullptr && picker->accessibleName() == name) {
                return picker;
            }
        }
        return static_cast<adqt::widgets::AdColorPicker*>(nullptr);
    };
    adqt::widgets::AdColorPicker* colorPicker = pickerWithName(QStringLiteral("Text color"));
    adqt::widgets::AdColorPicker* strokePicker =
        pickerWithName(QStringLiteral("Text stroke width"));
    adqt::widgets::AdColorPicker* fillPicker = pickerWithName(QStringLiteral("Text fill color"));
    require(colorPicker != nullptr, "text foreground picker should be present");
    require(strokePicker != nullptr, "text stroke picker should be present");
    require(fillPicker != nullptr, "text fill picker should be present");
    QLayout* textLayout = textControls->layout();
    QWidget* strokeRoot = styleEditorRoot(textControls, "text-stroke");
    QWidget* fillRoot = styleEditorRoot(textControls, "text-fill");
    require(textLayout != nullptr, "text style controls should have a layout");
    require(strokeRoot != nullptr && fillRoot != nullptr &&
                strokeRoot->isAncestorOf(strokePicker) && fillRoot->isAncestorOf(fillPicker) &&
                layoutWidgetIndex(textLayout, fillRoot) ==
                    layoutWidgetIndex(textLayout, strokeRoot) + 1 &&
                hasOnlySpacingBetween(textLayout, strokeRoot, fillRoot, 4),
            "text stroke color should have 4px spacing on its right");
    require(colorPicker->alphaChannelEnabled() && strokePicker->alphaChannelEnabled() &&
                fillPicker->alphaChannelEnabled(),
            "text color pickers should expose the requested alpha behavior");
    require(strokePicker->triggerContent() != nullptr &&
                strokePicker->triggerContent()->accessibleDescription() == QStringLiteral("0px"),
            "zero text stroke width should display as 0px");
    require(strokePicker->triggerContent()->cursor().shape() == Qt::SplitVCursor,
            "text stroke-width trigger should use the vertical split cursor");
    strokePicker->setPopupVisible(true);
    strokePicker->setPopupVisible(false);
    fillPicker->setPopupVisible(true);
    fillPicker->setPopupVisible(false);
    QWidget* strokeWidthPresets = nullptr;
    for (QWidget* widget : QApplication::allWidgets()) {
        if (widget != nullptr &&
            widget->objectName() == QStringLiteral("screenshotTextStrokeWidthPresets")) {
            strokeWidthPresets = widget;
            break;
        }
    }
    auto* strokeWidthPresetLayout = strokeWidthPresets != nullptr
                                        ? qobject_cast<QHBoxLayout*>(strokeWidthPresets->layout())
                                        : nullptr;
    require(strokeWidthPresetLayout != nullptr && strokeWidthPresetLayout->count() == 4 &&
                strokeWidthPresetLayout->itemAt(3)->spacerItem() != nullptr,
            "text stroke width presets should use the fill color row layout");
    QWidget* strokeColorPresets = nullptr;
    QWidget* textFillColorPresets = nullptr;
    for (QWidget* widget : QApplication::allWidgets()) {
        if (widget == nullptr) {
            continue;
        }
        if (widget->objectName() == QStringLiteral("screenshotTextStrokeColorPresets")) {
            strokeColorPresets = widget;
        } else if (widget->objectName() == QStringLiteral("screenshotTextFillColorPresets")) {
            textFillColorPresets = widget;
        }
    }
    const auto hasFillColorRowLayout = [](QWidget* presets) {
        auto* presetLayout =
            presets != nullptr ? qobject_cast<QHBoxLayout*>(presets->layout()) : nullptr;
        return presetLayout != nullptr && presetLayout->count() == 6 &&
               presetLayout->itemAt(5)->spacerItem() != nullptr;
    };
    require(hasFillColorRowLayout(strokeColorPresets),
            "text stroke color presets should use the fill color row layout");
    require(hasFillColorRowLayout(textFillColorPresets),
            "text fill color presets should use the fill color row layout");

    colorPicker->setPopupLayerMode(QApplication::platformName() == QStringLiteral("offscreen")
                                       ? adqt::widgets::AdColorPicker::PopupLayerMode::InWindow
                                       : adqt::widgets::AdColorPicker::PopupLayerMode::QtTool);
    colorPicker->setPopupVisible(true);
    QCoreApplication::processEvents();
    auto* colorPopover = colorPicker->findChild<adqt::widgets::AdPopover*>();
    require(colorPopover != nullptr, "text foreground picker should own a popup");
    require(popoverButtonWithTooltip(colorPopover, "Solid stroke") == nullptr &&
                popoverButtonWithTooltip(colorPopover, "Dashed stroke") == nullptr &&
                popoverButtonWithTooltip(colorPopover, "Dotted stroke") == nullptr,
            "text foreground popup should not contain stroke-style options");
    colorPicker->setPopupVisible(false);

    strokePicker->setPopupLayerMode(adqt::widgets::AdColorPicker::PopupLayerMode::InWindow);
    strokePicker->setPopupVisible(true);
    QCoreApplication::processEvents();
    auto* strokePopover = strokePicker->findChild<adqt::widgets::AdPopover*>();
    require(strokePopover != nullptr, "text stroke picker should own a popup");
    require(popoverButtonWithTooltip(strokePopover, "Text stroke width 2px") != nullptr &&
                popoverButtonWithTooltip(strokePopover, "Text stroke width 4px") != nullptr &&
                popoverButtonWithTooltip(strokePopover, "Text stroke width 8px") != nullptr,
            "text stroke popup should contain 2px, 4px, and 8px shortcuts");
    require(popoverButtonWithTooltip(strokePopover, "Text stroke color transparent") != nullptr &&
                popoverButtonWithTooltip(strokePopover, "Text stroke color #ffccc7") != nullptr &&
                popoverButtonWithTooltip(strokePopover, "Text stroke color #d9f7be") != nullptr &&
                popoverButtonWithTooltip(strokePopover, "Text stroke color #bae0ff") != nullptr &&
                popoverButtonWithTooltip(strokePopover, "Text stroke color #fff1b8") != nullptr,
            "text stroke popup should reuse the fill color presets");
    strokePicker->setPopupVisible(false);

    SnowCanvasTextStyle emittedStyle;
    quint32 emittedProperties = 0;
    int changeCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::textStyleChanged,
                     [&emittedStyle, &emittedProperties,
                      &changeCount](const SnowCanvasTextStyle& style, quint32 properties) {
                         emittedStyle = style;
                         emittedProperties = properties;
                         ++changeCount;
                     });
    clickStyleControl(palette, "Text font size XL (54px)");
    require(changeCount == 1 && qFuzzyCompare(emittedStyle.fontSize + 1.0, 55.0) &&
                emittedProperties == SnowCanvasTextStyleMixedFontSize,
            "XL text size should emit 54px once");
    auto* xlFontSizePreset = qobject_cast<adqt::widgets::AdButton*>(
        controlWithTooltip(palette, "Text font size XL (54px)"));
    require(
        xlFontSizePreset != nullptr &&
            xlFontSizePreset->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Tonal &&
            activeFontSizePreset->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text,
        "changing text size should move the shared style-toolbar active state to the new preset");
    clickStyleControl(palette, "Line text fill");
    require(changeCount == 2 && emittedStyle.fillStyle == SnowCanvasFillStyle::Line &&
                emittedProperties == SnowCanvasTextStyleMixedFillStyle,
            "text fill pattern should update");
    QWidget* alignmentTrigger = controlWithAccessibleName(palette, "Text alignment");
    adqt::widgets::AdPopover* alignmentPopover = showPopoverForTrigger(alignmentTrigger);
    clickPopoverStyleControl(alignmentPopover, "Align text center");
    require(changeCount == 3 &&
                emittedStyle.horizontalAlign == SnowCanvasTextHorizontalAlign::Center &&
                emittedProperties == SnowCanvasTextStyleMixedHorizontalAlign,
            "text alignment should update");
    require(emittedStyle.verticalAlign == SnowCanvasTextVerticalAlign::Bottom &&
                qFuzzyCompare(emittedStyle.opacity + 1.0, 1.65),
            "text toolbar edits should preserve unexposed style properties");
}

void textStylePopupLifecyclesAreBalanced() {
    ScreenshotToolPalette::Options options;
    options.showTextTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    palette.show();
    QCoreApplication::processEvents();

    const auto pickerWithName = [&palette](const QString& name) {
        for (adqt::widgets::AdColorPicker* picker :
             palette.findChildren<adqt::widgets::AdColorPicker*>()) {
            if (picker != nullptr && picker->accessibleName() == name) {
                return picker;
            }
        }
        return static_cast<adqt::widgets::AdColorPicker*>(nullptr);
    };
    auto* colorPicker = pickerWithName(QStringLiteral("Text color"));
    auto* strokePicker = pickerWithName(QStringLiteral("Text stroke width"));
    auto* fillPicker = pickerWithName(QStringLiteral("Text fill color"));
    auto* fontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Text font family"));
    require(colorPicker != nullptr && strokePicker != nullptr && fillPicker != nullptr &&
                fontSelect != nullptr,
            "all text style popup controls should be present");

    for (adqt::widgets::AdColorPicker* picker : {
             colorPicker,
             strokePicker,
             fillPicker,
         }) {
        picker->setPopupLayerMode(adqt::widgets::AdColorPicker::PopupLayerMode::InWindow);
    }
    fontSelect->setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::InWindow);

    int begins = 0;
    int ends = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::textStylePopupInteractionBegan,
                     [&begins]() { ++begins; });
    QObject::connect(&palette, &ScreenshotToolPalette::textStylePopupInteractionEnded,
                     [&ends]() { ++ends; });

    colorPicker->setPopupVisible(true);
    strokePicker->setPopupVisible(true);
    fillPicker->setPopupVisible(true);
    fontSelect->setPopupVisible(true);
    QCoreApplication::processEvents();
    require(begins == 1 && ends == 0,
            "the first text style popup should begin one shared interaction");

    colorPicker->setPopupVisible(false);
    strokePicker->setPopupVisible(false);
    fillPicker->setPopupVisible(false);
    QCoreApplication::processEvents();
    require(begins == 1 && ends == 0,
            "closing all but one text style popup should retain the interaction");

    fontSelect->setPopupVisible(false);
    QCoreApplication::processEvents();
    require(begins == 1 && ends == 1,
            "closing the final text style popup should end the interaction");

    QPointer<adqt::widgets::AdColorPicker> retainedPicker = colorPicker;
    colorPicker->setPopupVisible(true);
    QCoreApplication::processEvents();
    require(begins == 2 && ends == 1, "reopening a text popup should begin a new interaction");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    QCoreApplication::processEvents();
    require(!retainedPicker.isNull() && !retainedPicker->popupVisible(),
            "switching tools should close a retained shared popup subtree");
    require(begins == 2 && ends == 2,
            "switching tools with an open popup should balance the interaction lifecycle");
}

void retainedEditorsApplyDestinationMixedStateDuringReconciliation() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QWidget* shapeRow =
        palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
    QPointer<QWidget> strokeRoot = styleEditorRoot(shapeRow, "outline-stroke");
    require(!strokeRoot.isNull(), "Shape should expose its outline editor root");

    SnowCanvasStyleToolbarState arrowState;
    arrowState.source = SnowCanvasStyleToolbarSource::SelectedArrow;
    arrowState.shapeStyle.stroke = QColor(QStringLiteral("#f5222d"));
    arrowState.shapeStyle.strokeWidth = 4.0;
    arrowState.shapeStyle.strokeStyle = SnowCanvasStrokeStyle::Dashed;
    arrowState.shapeStyleMixed =
        SnowCanvasShapeStylePropertyStrokeColor | SnowCanvasShapeStylePropertyStrokeStyle;
    palette.setStyleToolbarState(arrowState);

    QWidget* arrowRow = palette.findChild<QWidget*>(QStringLiteral("screenshotArrowStyleControls"));
    require(styleEditorRoot(arrowRow, "outline-stroke") == strokeRoot,
            "canvas-driven reconciliation should retain the shared outline editor");
    auto* strokePicker = strokeRoot->findChild<adqt::widgets::AdColorPicker*>();
    require(strokePicker != nullptr,
            "the retained outline editor should expose its destination color picker");
    strokePicker->setPopupVisible(true);
    QCoreApplication::processEvents();
    requireControlInactive(palette, "Arrow stroke color #f5222d",
                           "the retained color editor should apply destination mixed state");
    requireControlInactive(palette, "Dashed arrow stroke",
                           "the retained stroke-style editor should apply destination mixed state");
}

void serialNumberStyleControlsExposeAndEmitRequestedProperties() {
    ScreenshotToolPalette::Options options;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    ScreenshotToolPalette palette(options);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Text),
            "text controls should materialize for the shared-editor comparison");

    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::SelectedSerialNumber;
    state.serialNumberStyle.number = 12;
    state.serialNumberStyle.color = QColor(QStringLiteral("#1677ff"));
    state.serialNumberStyle.fill = QColor(QStringLiteral("#fff1b8"));
    state.serialNumberStyle.fillStyle = SnowCanvasFillStyle::Solid;
    state.serialNumberStyle.fontSize = 30.0;
    state.serialNumberStyle.fontFamily.clear();
    state.serialNumberStyleMixed = SnowCanvasSerialNumberStyleMixedFontFamily;
    palette.setStyleToolbarState(state);
    // Capture the shared Text editor metrics before switching families; the
    // on-demand lifecycle evicts Text controls during SerialNumber activation.
    auto* textColorPickerBeforeSwitch = colorPickerWithAccessibleName(palette, "Text color");
    auto* textFillColorPickerBeforeSwitch =
        colorPickerWithAccessibleName(palette, "Text fill color");
    require(textColorPickerBeforeSwitch != nullptr && textFillColorPickerBeforeSwitch != nullptr,
            "text color editors should materialize before the family transition");
    const QSize textColorTriggerSize =
        textColorPickerBeforeSwitch->triggerContent() != nullptr
            ? textColorPickerBeforeSwitch->triggerContent()->sizeHint()
            : QSize();
    const QSize textFillTriggerSize =
        textFillColorPickerBeforeSwitch->triggerContent() != nullptr
            ? textFillColorPickerBeforeSwitch->triggerContent()->sizeHint()
            : QSize();
    textFillColorPickerBeforeSwitch->setPopupVisible(true);
    textFillColorPickerBeforeSwitch->setPopupVisible(false);
    QWidget* textFillOptionsBeforeSwitch = nullptr;
    QWidget* textFillPresetsBeforeSwitch = nullptr;
    for (QWidget* widget : QApplication::allWidgets()) {
        if (widget == nullptr) {
            continue;
        }
        if (widget->objectName() == QStringLiteral("screenshotTextFillOptions")) {
            textFillOptionsBeforeSwitch = widget;
        } else if (widget->objectName() == QStringLiteral("screenshotTextFillColorPresets")) {
            textFillPresetsBeforeSwitch = widget;
        }
    }
    require(textFillOptionsBeforeSwitch != nullptr && textFillPresetsBeforeSwitch != nullptr,
            "text fill popup containers should materialize before the family transition");
    const int textFillOptionsSpacing = textFillOptionsBeforeSwitch->layout() != nullptr
                                           ? textFillOptionsBeforeSwitch->layout()->spacing()
                                           : -1;
    const int textFillPresetsSpacing = textFillPresetsBeforeSwitch->layout() != nullptr
                                           ? textFillPresetsBeforeSwitch->layout()->spacing()
                                           : -1;
    const int textFillPresetsCount = textFillPresetsBeforeSwitch->layout() != nullptr
                                         ? textFillPresetsBeforeSwitch->layout()->count()
                                         : -1;
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    palette.show();
    QCoreApplication::processEvents();

    require(palette.styleToolbarVisible(), "sequence-number tool should show its style toolbar");
    QWidget* controls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotSerialNumberStyleControls"));
    require(controls != nullptr && controls->isVisible(),
            "sequence-number style controls should be visible");
    require(controlWithAccessibleName(palette, "Sequence number color") != nullptr,
            "sequence-number color picker should be present");
    require(controlWithAccessibleName(palette, "Sequence number fill color") != nullptr,
            "sequence-number fill color picker should be present");
    adqt::widgets::AdColorPicker* colorPicker =
        colorPickerWithAccessibleName(palette, "Sequence number color");
    adqt::widgets::AdColorPicker* fillColorPicker =
        colorPickerWithAccessibleName(palette, "Sequence number fill color");
    require(colorPicker != nullptr && fillColorPicker != nullptr &&
                colorPicker->alphaChannelEnabled() && fillColorPicker->alphaChannelEnabled(),
            "sequence-number color should match text color alpha behavior");
    require(colorPicker->triggerContent() != nullptr &&
                colorPicker->triggerContent()->sizeHint() == textColorTriggerSize,
            "sequence-number color should match the text color trigger format");
    require(fillColorPicker->triggerContent() != nullptr && textFillTriggerSize.isValid() &&
                fillColorPicker->triggerContent()->sizeHint() == textFillTriggerSize,
            "sequence-number fill should align with the text fill editor");
    fillColorPicker->setPopupVisible(true);
    fillColorPicker->setPopupVisible(false);
    const auto popupWidgetWithObjectName = [](const QString& objectName) {
        for (QWidget* widget : QApplication::allWidgets()) {
            if (widget != nullptr && widget->objectName() == objectName) {
                return widget;
            }
        }
        return static_cast<QWidget*>(nullptr);
    };
    QWidget* serialFillOptions =
        popupWidgetWithObjectName(QStringLiteral("screenshotSerialNumberFillOptions"));
    QWidget* serialFillPresets =
        popupWidgetWithObjectName(QStringLiteral("screenshotSerialNumberFillColorPresets"));
    require(serialFillOptions != nullptr && serialFillPresets != nullptr,
            "sequence-number and text fill popup containers should be present");
    require(qobject_cast<QVBoxLayout*>(serialFillOptions->layout()) != nullptr &&
                textFillOptionsSpacing >= 0 &&
                serialFillOptions->layout()->spacing() == textFillOptionsSpacing,
            "sequence-number fill popup should match the text fill popup container");
    require(qobject_cast<QHBoxLayout*>(serialFillPresets->layout()) != nullptr &&
                textFillPresetsSpacing >= 0 &&
                serialFillPresets->layout()->spacing() == textFillPresetsSpacing,
            "sequence-number fill presets should match the text fill preset row");
    require(serialFillPresets->layout()->count() == textFillPresetsCount,
            "sequence-number fill popup should expose every text fill color preset");
    QWidget* solidFill = controlWithTooltip(palette, "Solid sequence number fill");
    QWidget* crossLineFill = controlWithTooltip(palette, "Cross-line sequence number fill");
    QWidget* lineFill = controlWithTooltip(palette, "Line sequence number fill");
    QLayout* serialNumberLayout = controls->layout();
    QWidget* colorRoot = styleEditorRoot(controls, "foreground-color");
    QWidget* typeRoot = styleEditorRoot(controls, "serial-type");
    QWidget* fontRoot = styleEditorRoot(controls, "text-font");
    QWidget* fillRoot = styleEditorRoot(controls, "text-fill");
    QLayout* colorLayout = colorRoot != nullptr ? colorRoot->layout() : nullptr;
    QLayout* fillLayout = fillRoot != nullptr ? fillRoot->layout() : nullptr;
    require(serialNumberLayout != nullptr && colorRoot != nullptr && colorLayout != nullptr &&
                colorLayout->indexOf(colorPicker) == 0,
            "sequence-number color picker should lead its color presets");
    const QStringList colorPresetTooltips{
        QStringLiteral("Sequence number color #f5222d"),
        QStringLiteral("Sequence number color #52c41a"),
        QStringLiteral("Sequence number color #1677ff"),
        QStringLiteral("Sequence number color #fadb14"),
        QStringLiteral("Sequence number color #000000"),
    };
    for (int index = 0; index < colorPresetTooltips.size(); ++index) {
        QWidget* preset =
            controlWithTooltip(palette, colorPresetTooltips.at(index).toUtf8().constData());
        require(preset != nullptr && layoutWidgetIndex(colorLayout, preset) == index + 1,
                "sequence-number color presets should match the text color format");
    }
    QWidget* numberEditor = controlWithTooltip(palette, "Sequence number (scroll to adjust)");
    auto* numberInput = qobject_cast<adqt::widgets::AdLineEdit*>(numberEditor);
    require(numberInput != nullptr &&
                numberInput->variant() == adqt::widgets::AdLineEdit::Variant::Underlined &&
                adqt::icons::describeIcon(numberInput->prefixIconRef()).key.name ==
                    adqt::icons::describeIcon(adqt::icons::antd::outlined::Number()).key.name,
            "sequence number should be an underlined input with its existing icon as a prefix");
    const int numberEditorIndex = serialNumberLayout->indexOf(numberEditor);
    auto* fontSizeSummary = controlWithTooltip(palette, "Current sequence number font size");
    require(fontSizeSummary != nullptr &&
                fontSizeSummary->accessibleDescription() == QStringLiteral("30px"),
            "sequence-number font size should display the current value");
    require(numberEditorIndex >= 0 && fontRoot != nullptr &&
                fontRoot->isAncestorOf(fontSizeSummary) &&
                hasOnlySpacingBetween(serialNumberLayout, numberEditor, fontRoot, 4),
            "sequence number should have 4px spacing on its right");
    auto* fontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Sequence number font family"));
    require(fontSelect != nullptr && fontSelect->placeholder() == QStringLiteral("Font family") &&
                fontSelect->variant() == adqt::widgets::AdSelect::Variant::Borderless,
            "sequence-number font family should reuse the text selector");
    const QList<QFrame*> separators =
        controls->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly);
    auto* typeGroup =
        typeRoot != nullptr ? typeRoot->findChild<adqt::widgets::AdRadioButtonGroup*>() : nullptr;
    require(
        separators.size() == 3 && typeRoot != nullptr && typeGroup != nullptr &&
            controlWithTooltip(palette, "Sequence number type") == typeRoot &&
            controlWithTooltip(palette, "Outlined circle") != nullptr &&
            controlWithTooltip(palette, "Solid circle") != nullptr &&
            controlWithTooltip(palette, "Outlined square") != nullptr &&
            controlWithTooltip(palette, "Solid square") != nullptr &&
            serialNumberLayout->indexOf(colorRoot) <
                serialNumberLayout->indexOf(separators.at(0)) &&
            serialNumberLayout->indexOf(separators.at(0)) < serialNumberLayout->indexOf(typeRoot) &&
            serialNumberLayout->indexOf(typeRoot) < serialNumberLayout->indexOf(separators.at(1)) &&
            serialNumberLayout->indexOf(separators.at(1)) < numberEditorIndex &&
            serialNumberLayout->indexOf(fontRoot) < serialNumberLayout->indexOf(separators.at(2)) &&
            serialNumberLayout->indexOf(separators.at(2)) < serialNumberLayout->indexOf(fillRoot) &&
            fillRoot != nullptr && fillLayout != nullptr &&
            fillRoot->isAncestorOf(fillColorPicker) && solidFill != nullptr &&
            crossLineFill != nullptr && lineFill != nullptr &&
            layoutWidgetIndex(fillLayout, solidFill) ==
                layoutWidgetIndex(fillLayout, fillColorPicker) + 1 &&
            layoutWidgetIndex(fillLayout, crossLineFill) ==
                layoutWidgetIndex(fillLayout, fillColorPicker) + 2 &&
            layoutWidgetIndex(fillLayout, lineFill) ==
                layoutWidgetIndex(fillLayout, fillColorPicker) + 3,
        "sequence-number color, type, content, and fill groups should use separators");

    SnowCanvasSerialNumberStyle emittedStyle;
    int changeCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::serialNumberStyleChanged,
                     [&emittedStyle, &changeCount](const SnowCanvasSerialNumberStyle& style) {
                         emittedStyle = style;
                         ++changeCount;
                     });
    clickStyleControl(palette, "Sequence number font size 54px");
    require(changeCount == 1 && qFuzzyCompare(emittedStyle.fontSize + 1.0, 55.0) &&
                emittedStyle.number == 12 && emittedStyle.color == state.serialNumberStyle.color &&
                emittedStyle.fill == state.serialNumberStyle.fill,
            "sequence-number controls should emit the complete updated style");
    clickStyleControl(palette, "Sequence number color #52c41a");
    require(changeCount == 2 && emittedStyle.color == QColor(QStringLiteral("#52c41a")),
            "sequence-number color preset should update the style");
    clickStyleControl(palette, "Line sequence number fill");
    require(changeCount == 3 && emittedStyle.fillStyle == SnowCanvasFillStyle::Line,
            "sequence-number fill pattern should update like text fill");
    fillColorPicker->setValue(
        adqt::widgets::AdColorValue::solid(QColor(QStringLiteral("#bae0ff"))));
    require(changeCount == 4 && emittedStyle.fill == QColor(QStringLiteral("#bae0ff")) &&
                emittedStyle.fillStyle == SnowCanvasFillStyle::Line,
            "changing sequence-number fill color should preserve its fill pattern");
    const QColor preservedFill = emittedStyle.fill;
    auto* solidSquareType =
        qobject_cast<QAbstractButton*>(controlWithTooltip(palette, "Solid square"));
    require(solidSquareType != nullptr, "solid-square sequence-number type should be clickable");
    solidSquareType->click();
    require(changeCount == 5 && emittedStyle.type == SnowCanvasSerialNumberType::SolidSquare &&
                emittedStyle.fill == preservedFill &&
                emittedStyle.fillStyle == SnowCanvasFillStyle::Line,
            "type changes should emit the full style without changing stored fill settings");
    require(!fillRoot->isEnabled(),
            "uniform solid sequence-number selections should disable the visible fill editor");

    auto* circleType = qobject_cast<QAbstractButton*>(controlWithTooltip(palette, "Circle"));
    require(circleType != nullptr, "Circle type should be available");
    circleType->click();
    require(emittedStyle.type == SnowCanvasSerialNumberType::Circle && !numberInput->isEnabled() &&
                !fontSelect->isEnabled() && fontSizeSummary->isEnabled() && fillRoot->isEnabled(),
            "Circle should disable number and font family while keeping size and fill enabled");
    const int circleChangeCount = changeCount;
    numberInput->setText(QStringLiteral("999"));
    QMetaObject::invokeMethod(numberInput, "editingFinished", Qt::DirectConnection);
    const QPoint numberCenter = numberInput->rect().center();
    QWheelEvent circleWheel(QPointF(numberCenter), numberInput->mapToGlobal(numberCenter), QPoint(),
                            QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(numberInput, &circleWheel);
    require(changeCount == circleChangeCount && emittedStyle.number == 12,
            "Circle should ignore number commits and wheel changes");
    clickStyleControl(palette, "Sequence number font size 30px");
    require(changeCount == circleChangeCount + 1 && emittedStyle.fontSize == 30.0,
            "Circle font size should remain editable");
    clickStyleControl(palette, "Cross-line sequence number fill");
    require(emittedStyle.fillStyle == SnowCanvasFillStyle::CrossLine,
            "Circle fill should remain editable");
    solidSquareType->click();
    require(numberInput->isEnabled() && fontSelect->isEnabled() && !fillRoot->isEnabled(),
            "switching back to a numbered type should restore number and family editors");
    circleType->click();

    state.serialNumberStyle = emittedStyle;
    state.serialNumberStyleMixed = SnowCanvasSerialNumberStyleMixedType;
    palette.setStyleToolbarState(state);
    require(typeGroup->checkedId() == -1,
            "mixed sequence-number types should leave every type button unchecked");
    require(fillRoot->isEnabled(),
            "mixed sequence-number types should keep the fill editor enabled");
    require(numberInput->isEnabled() && fontSelect->isEnabled(),
            "mixed Circle and numbered types should keep label editors enabled");
    state.serialNumberStyleMixed = 0;
    palette.setStyleToolbarState(state);
    require(!fontSelect->isEnabled(), "uniform Circle should disable font family again");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    auto* textFamily = controlWithAccessibleName(palette, "Text font family");
    require(textFamily != nullptr && textFamily->isEnabled(),
            "reusing Circle font controls for Text must restore the family selector");
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    auto* circleFamily = controlWithAccessibleName(palette, "Sequence number font family");
    require(circleFamily != nullptr && !circleFamily->isEnabled(),
            "returning to Circle must disable the reused font selector");
}

void serialNumberInputCommitsEditsAndSupportsWheel() {
    ScreenshotToolPalette::Options options;
    options.showSerialNumberTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::SelectedSerialNumber;
    state.serialNumberStyle.number = 12;
    palette.setStyleToolbarState(state);
    palette.show();
    QCoreApplication::processEvents();
    auto* input = qobject_cast<adqt::widgets::AdLineEdit*>(
        controlWithTooltip(palette, "Sequence number (scroll to adjust)"));
    require(input != nullptr && input->text() == QStringLiteral("12") && !input->isReadOnly(),
            "sequence number should expose its current value for direct editing");
    require(input->focusPolicy() == Qt::ClickFocus,
            "sequence number should accept click focus without joining the toolbar Tab chain");
    auto* prefix = input->findChild<QLabel*>(QStringLiteral("ad-input-prefix-icon"));
    require(prefix != nullptr && prefix->isVisible() &&
                input->rect().contains(prefix->geometry()) &&
                prefix->geometry().right() < input->textMargins().left(),
            "sequence number icon should sit inside the input to the left of editable text");

    int changes = 0;
    qint64 number = 12;
    QObject::connect(&palette, &ScreenshotToolPalette::serialNumberStyleChanged,
                     [&changes, &number](const SnowCanvasSerialNumberStyle& style) {
                         ++changes;
                         number = style.number;
                     });
    const auto typeText = [input](const QString& text) {
        input->selectAll();
        QKeyEvent key(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, text);
        QApplication::sendEvent(input, &key);
    };
    const auto finish = [input]() {
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(input, &enter);
    };
    const auto wheel = [](QWidget* target, int delta) {
        const QPoint local = target->rect().center();
        QWheelEvent event(QPointF(local), target->mapToGlobal(local), QPoint(), QPoint(0, delta),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &event);
        require(event.isAccepted(), "sequence number input and prefix should consume wheel events");
    };
    typeText(QStringLiteral("40"));
    require(changes == 0, "typing should defer sequence number changes until editing finishes");
    finish();
    require(changes == 1 && number == 40, "Enter should commit the typed sequence number");
    wheel(input, 120);
    require(changes == 2 && number == 41 && input->text() == QStringLiteral("41"),
            "scrolling up over the input should increment and display the number");
    wheel(prefix, -120);
    require(changes == 3 && number == 40 && input->text() == QStringLiteral("40"),
            "scrolling down over the embedded icon should decrement the number");
    typeText(QStringLiteral("70"));
    wheel(input, 120);
    require(changes == 4 && number == 71,
            "wheel adjustment should use a valid pending edit as its starting value");
    input->clear();
    finish();
    require(changes == 4 && input->text() == QStringLiteral("71"),
            "empty input should restore the current number without changing it");
    typeText(QStringLiteral("9223372036854775808"));
    finish();
    require(changes == 4 && input->text() == QStringLiteral("71"),
            "overflowing input should restore the current number without changing it");
    typeText(QStringLiteral("-1"));
    require(input->text() == QStringLiteral("71"), "negative input should be rejected");
    typeText(QStringLiteral("abc"));
    require(input->text() == QStringLiteral("71"), "nonnumeric input should be rejected");
    typeText(QStringLiteral("0"));
    finish();
    wheel(input, -120);
    require(changes == 5 && number == 0 && input->text() == QStringLiteral("0"),
            "scrolling down at zero should stay at zero without emitting a redundant change");
    typeText(QStringLiteral("9223372036854775807"));
    finish();
    require(changes == 6 && input->text() == QStringLiteral("9223372036854775807"),
            "sequence number input should preserve the exact maximum 64-bit integer");
    wheel(prefix, 120);
    require(changes == 6 && input->text() == QStringLiteral("9223372036854775807"),
            "scrolling up at the maximum should clamp without overflow or redundant changes");
    wheel(input, -120);
    require(changes == 7 && input->text() == QStringLiteral("9223372036854775806"),
            "large sequence numbers should decrement exactly without floating point rounding");

    state.serialNumberStyleMixed = SnowCanvasSerialNumberStyleMixedNumber;
    palette.setStyleToolbarState(state);
    require(changes == 7 && input->text().isEmpty() &&
                input->placeholderText() == QStringLiteral("Mixed"),
            "mixed sequence numbers should show a placeholder without emitting changes");
    finish();
    require(changes == 7 && input->text().isEmpty(),
            "finishing an untouched mixed input should preserve mixed state");
    typeText(QStringLiteral("12"));
    finish();
    require(changes == 8 && number == 12 && input->text() == QStringLiteral("12"),
            "entering the representative number should resolve mixed state");
    input->setFocus();
    typeText(QStringLiteral("23"));
    input->clearFocus();
    require(changes == 9 && number == 23, "focus loss should commit sequence number edits");
    typeText(QStringLiteral("9223372036854775808"));
    wheel(input, 120);
    require(changes == 10 && number == 24 && input->text() == QStringLiteral("24"),
            "wheel adjustment should recover invalid pending text using the current number");
    input->setFocus();
    typeText(QStringLiteral("35"));
    palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    QCoreApplication::processEvents();
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    QCoreApplication::processEvents();
    input = qobject_cast<adqt::widgets::AdLineEdit*>(
        controlWithTooltip(palette, "Sequence number (scroll to adjust)"));
    require(input != nullptr && !input->text().isEmpty(),
            "switching tools during an edit should safely recreate the sequence number input");
}

void stylePopoverTriggersProvideMouseFeedback() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    for (ScreenshotToolPalette::Tool tool : {
             ScreenshotToolPalette::Tool::Shape,
             ScreenshotToolPalette::Tool::Arrow,
             ScreenshotToolPalette::Tool::Text,
             ScreenshotToolPalette::Tool::SerialNumber,
         }) {
        require(palette.ensureStyleFamily(tool),
                "the popup-feedback test should materialize each inspected style family");
    }
    QWidget* strokeWidthSummary = controlWithTooltip(palette, "Current stroke width");
    require(strokeWidthSummary != nullptr, "stroke width summary should be present");
    auto* strokeWidthButton = qobject_cast<adqt::widgets::AdButton*>(strokeWidthSummary);
    require(strokeWidthButton != nullptr,
            "stroke width summary should use the shared preview button");
    require(strokeWidthSummary->cursor().shape() == Qt::SplitVCursor,
            "stroke width summary should use the vertical split cursor");
    QWidget* strokeWidthPreset = controlWithTooltip(palette, "Stroke width 2");
    require(strokeWidthPreset != nullptr && strokeWidthPreset->cursor().shape() != Qt::SplitVCursor,
            "fixed stroke-width presets should not indicate wheel adjustment");
    QWidget* cornerRadius = controlWithTooltip(palette, "Corner radius (scroll to adjust)");
    require(cornerRadius != nullptr && cornerRadius->cursor().shape() == Qt::SplitVCursor,
            "corner-radius editor should use the vertical split cursor");

    QList<QWidget*> triggers;
    QList<adqt::widgets::AdColorPicker*> popupPickers;
    for (adqt::widgets::AdColorPicker* picker :
         palette.findChildren<adqt::widgets::AdColorPicker*>()) {
        if (picker != nullptr && picker->triggerContent() != nullptr) {
            popupPickers.append(picker);
            triggers.append(picker->triggerContent());
        }
    }
    for (const char* tooltip : {
             "Start arrowhead",
             "End arrowhead",
             "Text alignment",
         }) {
        triggers.append(controlWithAccessibleName(palette, tooltip));
    }

    require(triggers.size() == popupPickers.size() + 3,
            "all style popup triggers should be present");
    for (QWidget* trigger : triggers) {
        require(trigger != nullptr, "style popup trigger should be present");
        if (!trigger->toolTip().isEmpty()) {
            std::cerr << "unexpected popup-trigger tooltip: object="
                      << trigger->objectName().toStdString()
                      << " accessible=" << trigger->accessibleName().toStdString()
                      << " tooltip=" << trigger->toolTip().toStdString() << '\n';
        }
        require(trigger->toolTip().isEmpty(),
                "style popup trigger should not show a tooltip over its popup");
        require(!trigger->accessibleName().isEmpty(),
                "style popup trigger should retain an accessible name");
        auto* triggerButton = qobject_cast<adqt::widgets::AdButton*>(trigger);
        require(triggerButton != nullptr,
                "style popup trigger should use the shared preview button");
        require(trigger->size() == strokeWidthSummary->size(),
                "style popup trigger should match the stroke width summary size");
        require(triggerButton->buttonStyle() == strokeWidthButton->buttonStyle() &&
                    triggerButton->accentRole() == strokeWidthButton->accentRole() &&
                    triggerButton->shape() == strokeWidthButton->shape() &&
                    triggerButton->sizeClass() == strokeWidthButton->sizeClass(),
                "style popup trigger should reuse the stroke width button style");

        const QPointF center(trigger->rect().center());
        QMouseEvent press(QEvent::MouseButtonPress, center, trigger->mapToGlobal(center.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        press.setAccepted(false);
        QCoreApplication::sendEvent(trigger, &press);
        require(press.isAccepted(), "style popup trigger should accept mouse press");

        QMouseEvent release(QEvent::MouseButtonRelease, center,
                            trigger->mapToGlobal(center.toPoint()), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        release.setAccepted(false);
        QCoreApplication::sendEvent(trigger, &release);
        require(release.isAccepted(), "style popup trigger should accept mouse release");
    }

    require(!popupPickers.isEmpty(), "style popup picker containers should be present");
    for (adqt::widgets::AdColorPicker* picker : popupPickers) {
        require(picker->toolTip().isEmpty(),
                "style popup picker should not show a tooltip over its popup");
        require(!picker->accessibleName().isEmpty(),
                "style popup picker should retain an accessible name");
    }

    auto* fontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Text font family"));
    require(fontSelect != nullptr, "text font-family popup trigger should exist");
    require(fontSelect->toolTip().isEmpty(),
            "text font-family popup trigger should not show a tooltip");
    require(!fontSelect->accessibleName().isEmpty(),
            "text font-family popup trigger should retain an accessible name");

    auto* serialNumberFontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Sequence number font family"));
    require(serialNumberFontSelect != nullptr,
            "sequence number font-family popup trigger should exist");
    require(serialNumberFontSelect->toolTip().isEmpty(),
            "sequence number font-family popup trigger should not show a tooltip");
    require(!serialNumberFontSelect->accessibleName().isEmpty(),
            "sequence number font-family trigger should retain an accessible name");
}

void cornerRadiusButtonsRestoreTheDefaultValue() {
    ScreenshotToolPalette::Options options;
    options.showTextTool = true;
    ScreenshotToolPalette palette(options);

    SnowCanvasShapeStyle rectangleStyle =
        snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle;
    rectangleStyle.cornerRadii = SnowCanvasCornerRadii{20.0, 20.0, 20.0, 20.0};
    palette.setRectangleStyle(rectangleStyle);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);

    SnowCanvasShapeStyle emittedRectangleStyle;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&emittedRectangleStyle](const SnowCanvasShapeStyle& style, quint32 properties,
                                              SnowCanvasShapeKind kind) {
                         if (kind == SnowCanvasShapeKind::Rectangle &&
                             properties == SnowCanvasShapeStylePropertyCornerRadius) {
                             emittedRectangleStyle = style;
                         }
                     });
    clickStyleControl(palette, "Corner radius (scroll to adjust)");
    require(qFuzzyCompare(emittedRectangleStyle.cornerRadii.topLeft + 1.0, 7.0),
            "clicking the rectangle corner-radius button should restore 6px");

    SnowCanvasStyleToolbarState textState;
    textState.source = SnowCanvasStyleToolbarSource::DefaultText;
    textState.textStyle.cornerRadii = SnowCanvasCornerRadii{20.0, 20.0, 20.0, 20.0};
    palette.setStyleToolbarState(textState);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Text);

    SnowCanvasTextStyle emittedTextStyle;
    QObject::connect(
        &palette, &ScreenshotToolPalette::textStyleChanged,
        [&emittedTextStyle](const SnowCanvasTextStyle& style) { emittedTextStyle = style; });
    clickStyleControl(palette, "Text fill corner radius (scroll to adjust)");
    require(qFuzzyCompare(emittedTextStyle.cornerRadii.topLeft + 1.0, 7.0),
            "clicking the text corner-radius button should restore 6px");
}

void selectedStrokeColorDragKeepsPickerIndicatorInSync() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(320, 240);
    canvas.show();

    const auto sendPointerEvent = [](QWidget* target, QEvent::Type type, const QPointF& position,
                                     Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF globalPosition = target->mapToGlobal(position.toPoint());
        QMouseEvent event(type, position, globalPosition, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &event);
    };

    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "canvas should activate the shape tool");
    sendPointerEvent(&canvas, QEvent::MouseButtonPress, QPointF(60.0, 60.0), Qt::LeftButton,
                     Qt::LeftButton);
    sendPointerEvent(&canvas, QEvent::MouseMove, QPointF(160.0, 160.0), Qt::NoButton,
                     Qt::LeftButton);
    sendPointerEvent(&canvas, QEvent::MouseButtonRelease, QPointF(160.0, 160.0), Qt::LeftButton,
                     Qt::NoButton);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "canvas should activate the select tool");
    sendPointerEvent(&canvas, QEvent::MouseButtonPress, QPointF(62.0, 62.0), Qt::LeftButton,
                     Qt::LeftButton);
    sendPointerEvent(&canvas, QEvent::MouseButtonRelease, QPointF(62.0, 62.0), Qt::LeftButton,
                     Qt::NoButton);
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedRectangle,
            "canvas rectangle should be selected");

    QWidget paletteHost;
    paletteHost.resize(420, 320);
    auto* hostLayout = new QVBoxLayout(&paletteHost);
    hostLayout->setContentsMargins(12, 12, 12, 12);
    auto* palette = new ScreenshotToolPalette(ScreenshotToolPalette::Options{}, &paletteHost);
    hostLayout->addWidget(palette, 0, Qt::AlignLeft | Qt::AlignTop);
    palette->setStyleToolbarState(canvas.canvasStyleToolbarState());
    palette->setActiveTool(ScreenshotToolPalette::Tool::Shape);

    adqt::widgets::AdColorPicker* strokePicker = nullptr;
    for (adqt::widgets::AdColorPicker* picker :
         palette->findChildren<adqt::widgets::AdColorPicker*>()) {
        if (picker != nullptr && picker->accessibleName() == QStringLiteral("Stroke color")) {
            strokePicker = picker;
            break;
        }
    }
    require(strokePicker != nullptr, "stroke color picker should be present");

    QObject::connect(
        palette, &ScreenshotToolPalette::shapeStyleChanged,
        [&canvas](const SnowCanvasShapeStyle& style, quint32 properties, SnowCanvasShapeKind kind) {
            static_cast<void>(canvas.setCanvasShapeStylePatch(style, properties, kind));
        });
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, [&canvas, palette]() {
        palette->setStyleToolbarState(canvas.canvasStyleToolbarState());
    });

    const auto originalStyle = canvas.canvasStyleToolbarState().shapeStyle;
    const auto exportedImage = [&runtime]() {
        return runtime.renderToImage(QRectF(0, 0, 320, 240), QSize(320, 240), {});
    };
    const QImage originalImage = exportedImage();
    for (const int alpha : {128, 0}) {
        QColor color = originalStyle.stroke;
        color.setAlpha(alpha);
        strokePicker->commitValue(adqt::widgets::AdColorValue::solid(color));
        require(canvas.canvasStyleToolbarState().shapeStyle.stroke == color,
                "selected strokes must receive the picker alpha");
        require(canvas.canvasStyleToolbarState().shapeStyle.fill == originalStyle.fill &&
                    canvas.canvasStyleToolbarState().shapeStyle.opacity == originalStyle.opacity,
                "stroke alpha must not change fill or overall opacity");
        const QImage editedImage = exportedImage();
        require(!editedImage.isNull() && (alpha == 255 || editedImage != originalImage),
                "transparent stroke edits must affect exported pixels");
        require(canvas.undo(), "stroke alpha edits must be undoable");
        require(canvas.canvasStyleToolbarState().shapeStyle.stroke == originalStyle.stroke,
                "undo must restore the original stroke alpha");
        require(canvas.redo(), "stroke alpha edits must be redoable");
        require(strokePicker->value().solidColor == color && exportedImage() == editedImage,
                "redo must restore picker alpha and exported pixels");
        require(canvas.undo(), "restore the original stroke before the next alpha edit");
    }

    strokePicker->setPopupLayerMode(QApplication::platformName() == QStringLiteral("offscreen")
                                        ? adqt::widgets::AdColorPicker::PopupLayerMode::InWindow
                                        : adqt::widgets::AdColorPicker::PopupLayerMode::QtTool);
    palette->setStyleToolbarVisible(true);
    paletteHost.show();
    QCoreApplication::processEvents();
    require(strokePicker->isVisible(), "stroke color picker should be visible before opening");
    strokePicker->setPopupVisible(true);
    QCoreApplication::processEvents();

    QWidget* saturationPanel = nullptr;
    for (QWidget* widget : QApplication::allWidgets()) {
        if (widget != nullptr && widget->isVisible() &&
            widget->objectName() == QStringLiteral("ad-color-picker-saturation-panel")) {
            saturationPanel = widget;
            break;
        }
    }
    require(saturationPanel != nullptr, "stroke color selection area should be present");
    require(saturationPanel->width() > 2 && saturationPanel->height() > 2,
            "stroke color selection area should have usable geometry");

    const QPointF pressPosition(saturationPanel->width() * 0.95, saturationPanel->height() * 0.05);
    const QPointF localPosition(saturationPanel->width() * 0.35, saturationPanel->height() * 0.65);
    const QPointF pressGlobalPosition = saturationPanel->mapToGlobal(pressPosition.toPoint());
    const QPointF globalPosition = saturationPanel->mapToGlobal(localPosition.toPoint());
    QMouseEvent pressEvent(QEvent::MouseButtonPress, pressPosition, pressGlobalPosition,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(saturationPanel, &pressEvent);
    QMouseEvent moveEvent(QEvent::MouseMove, localPosition, globalPosition, Qt::NoButton,
                          Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(saturationPanel, &moveEvent);
    const QColor movingColor = strokePicker->value().solidColor.toHsv();
    require(qAbs(qRound(movingColor.saturationF() * 100.0F) - 35) <= 1 &&
                qAbs(qRound(movingColor.valueF() * 100.0F) - 35) <= 1,
            "stroke color indicator should follow the pointer before release");
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, localPosition, globalPosition,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(saturationPanel, &releaseEvent);
    QCoreApplication::processEvents();

    const QColor selectedColor = strokePicker->value().solidColor.toHsv();
    const int expectedSaturation = qRound(selectedColor.saturationF() * 100.0F);
    const int expectedBrightness = qRound(selectedColor.valueF() * 100.0F);
    const QString indicatorDescription = saturationPanel->accessibleDescription();
    require(indicatorDescription.contains(
                QStringLiteral("saturation %1 percent").arg(expectedSaturation)),
            "stroke color indicator saturation should match the selected color");
    require(indicatorDescription.contains(
                QStringLiteral("brightness %1 percent").arg(expectedBrightness)),
            "stroke color indicator brightness should match the selected color");
    require(expectedSaturation < 100 && expectedBrightness < 100,
            "stroke color drag should not pin the indicator to the top-right corner");

    strokePicker->setPopupVisible(false);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void fillStyleButtonsFollowFillColorPickerTrigger() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);

    adqt::widgets::AdColorPicker* fillPicker = nullptr;
    for (adqt::widgets::AdColorPicker* picker :
         palette.findChildren<adqt::widgets::AdColorPicker*>()) {
        if (picker != nullptr && picker->accessibleName() == QStringLiteral("Fill color")) {
            fillPicker = picker;
            break;
        }
    }
    require(fillPicker != nullptr, "fill color picker should be present");

    auto* lineFill =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Line fill"));
    auto* crossLineFill =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Cross-line fill"));
    auto* solidFill =
        qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Solid fill"));
    require(lineFill != nullptr, "line fill control should be present");
    require(crossLineFill != nullptr, "cross-line fill control should be present");
    require(solidFill != nullptr, "solid fill control should be present");

    QLayout* toolbarLayout = fillPicker->parentWidget()->layout();
    require(toolbarLayout != nullptr, "fill color picker should have a toolbar layout");
    const int fillPickerIndex = layoutWidgetIndex(toolbarLayout, fillPicker);
    require(fillPickerIndex >= 0, "fill color picker should be in the toolbar layout");
    require(layoutWidgetIndex(toolbarLayout, solidFill) == fillPickerIndex + 1 &&
                layoutWidgetIndex(toolbarLayout, crossLineFill) == fillPickerIndex + 2 &&
                layoutWidgetIndex(toolbarLayout, lineFill) == fillPickerIndex + 3,
            "fill style buttons should follow the fill color picker in reverse order");

    fillPicker->setPopupLayerMode(QApplication::platformName() == QStringLiteral("offscreen")
                                      ? adqt::widgets::AdColorPicker::PopupLayerMode::InWindow
                                      : adqt::widgets::AdColorPicker::PopupLayerMode::QtTool);
    fillPicker->setPopupVisible(true);
    QCoreApplication::processEvents();
    auto* fillPopover = fillPicker->findChild<adqt::widgets::AdPopover*>();
    require(fillPopover != nullptr, "fill color picker should own a popup");
    require(popoverButtonWithTooltip(fillPopover, "Line fill") == nullptr &&
                popoverButtonWithTooltip(fillPopover, "Cross-line fill") == nullptr &&
                popoverButtonWithTooltip(fillPopover, "Solid fill") == nullptr,
            "fill styles should not remain in the fill color popup");
    require(popoverButtonWithTooltip(fillPopover, "Fill color transparent") != nullptr,
            "fill color presets should remain in the fill color popup");
    fillPicker->setPopupVisible(false);

    crossLineFill->click();
    require(palette.rectangleStyle().fillStyle == SnowCanvasFillStyle::CrossLine,
            "the toolbar fill style control should update the rectangle style");
}

void configurationDrivenStyleEditorsShareStructuralContracts() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    require(palette.ensureActionFamily(ScreenshotToolPalette::ActionFamily::Selection) &&
                palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Arrow) &&
                palette.ensureStyleFamily(ScreenshotToolPalette::Tool::RectangleHighlight) &&
                palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Spotlight) &&
                palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Text) &&
                palette.ensureStyleFamily(ScreenshotToolPalette::Tool::RectangleFilter) &&
                palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Watermark),
            "shared editor families should materialize on demand");

    const QStringList sliderPrefixes{
        QStringLiteral("screenshotSelectionOpacity"),
        QStringLiteral("screenshotSpotlightOpacity"),
        QStringLiteral("screenshotFilterIntensity"),
        QStringLiteral("screenshotWatermarkOpacity"),
    };
    QList<QPair<QLabel*, adqt::widgets::AdSlider*>> sliderEditors;
    for (const QString& prefix : sliderPrefixes) {
        auto* icon = palette.findChild<QLabel*>(prefix + QStringLiteral("Icon"));
        auto* slider =
            palette.findChild<adqt::widgets::AdSlider*>(prefix + QStringLiteral("Slider"));
        require(icon != nullptr && slider != nullptr && icon->alignment() == Qt::AlignCenter &&
                    icon->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                    !icon->accessibleName().isEmpty() && slider->focusPolicy() == Qt::NoFocus &&
                    slider->minimum() == 0 && slider->maximum() == 100 &&
                    slider->singleStep() == 1 && slider->pageStep() == 5 &&
                    !slider->accessibleName().isEmpty() &&
                    !slider->accessibleDescription().isEmpty(),
                "selection and style sliders should share the configured editor shell");
        sliderEditors.append({icon, slider});
    }
    const QSize selectionSliderSize = sliderEditors.constFirst().second->size();
    require(selectionSliderSize.width() == sliderEditors.at(1).second->width() &&
                selectionSliderSize.height() == 32 && sliderEditors.at(1).second->height() == 28 &&
                sliderEditors.at(2).second->size() == sliderEditors.at(1).second->size() &&
                sliderEditors.at(3).second->size() == sliderEditors.at(1).second->size(),
            "slider configuration should preserve action-row and style-row heights");

    auto* fontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Text font family"));
    auto* filterSelect =
        palette.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenshotFilterTypeSelect"));
    require(fontSelect != nullptr && filterSelect != nullptr,
            "font and filter editors should expose shared select controls");
    for (adqt::widgets::AdSelect* select : {fontSelect, filterSelect}) {
        require(select->focusPolicy() == Qt::NoFocus &&
                    select->mode() == adqt::widgets::AdSelect::Mode::Single &&
                    select->controlSize() == adqt::widgets::AdSelect::ControlSize::Small &&
                    select->variant() == adqt::widgets::AdSelect::Variant::Borderless &&
                    select->popupLayerMode() == adqt::widgets::AdSelect::PopupLayerMode::QtTool &&
                    !select->popupMatchSelectWidth(),
                "font and filter selects should share the configured shell");
    }
    require(fontSelect->searchEnabled() && !filterSelect->searchEnabled() &&
                fontSelect->toolTip().isEmpty() &&
                filterSelect->toolTip() == QStringLiteral("Filter type") &&
                fontSelect->model() != filterSelect->model() &&
                filterSelect->model()->rowCount() == 6,
            "select configuration should preserve search, tooltip, and model differences");
    const QSize selectReferenceSize = fontSelect->size();
    require(selectReferenceSize == filterSelect->size(),
            "font and filter selects should start with identical shared metrics");

    QList<QWidget*> radioContainers{
        palette.findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup")),
        palette.findChild<QWidget*>(QStringLiteral("screenshotArrowTypeButtonGroup")),
    };
    radioContainers.append(
        palette.findChildren<QWidget*>(QStringLiteral("screenshotHighlightModeSelector")));
    radioContainers.append(
        palette.findChildren<QWidget*>(QStringLiteral("screenshotFilterModeSelector")));
    require(radioContainers.size() == 4,
            "each materialized shape, arrow, highlight, and filter row should expose one radio "
            "editor");
    QSize radioReferenceSize;
    for (QWidget* container : radioContainers) {
        auto* group = container == nullptr
                          ? nullptr
                          : container->findChild<adqt::widgets::AdRadioButtonGroup*>();
        require(group != nullptr && group->variant() == adqt::widgets::AdRadio::Variant::Button &&
                    group->controlSize() == adqt::widgets::AdRadio::ControlSize::Small &&
                    qobject_cast<QHBoxLayout*>(container->layout()) != nullptr &&
                    container->layout()->spacing() == 0,
                "all mode selectors should share the button-radio shell");
        for (QAbstractButton* abstractButton : group->buttons()) {
            auto* radio = qobject_cast<adqt::widgets::AdRadio*>(abstractButton);
            require(radio != nullptr && radio->focusPolicy() == Qt::NoFocus &&
                        !radio->toolTip().isEmpty() && radio->accessibleName() == radio->toolTip(),
                    "shared radio options should preserve tooltip accessibility");
            if (!radioReferenceSize.isValid()) {
                radioReferenceSize = radio->size();
            }
            require(radio->size() == radioReferenceSize,
                    "all configured mode options should use the same radio metrics");
        }
    }

    adqt::widgets::AdColorPicker* colorPicker =
        colorPickerWithAccessibleName(palette, "Highlight color");
    adqt::widgets::AdColorPicker* fillPicker = colorPickerWithAccessibleName(palette, "Fill color");
    adqt::widgets::AdColorPicker* strokePicker =
        colorPickerWithAccessibleName(palette, "Arrow stroke color");
    adqt::widgets::AdColorPicker* widthColorPicker =
        colorPickerWithAccessibleName(palette, "Highlight stroke width");
    const QList<adqt::widgets::AdColorPicker*> pickerShells{
        colorPicker,
        fillPicker,
        strokePicker,
        widthColorPicker,
    };
    for (adqt::widgets::AdColorPicker* picker : pickerShells) {
        require(picker != nullptr && picker->focusPolicy() == Qt::NoFocus &&
                    picker->size() == adqt::widgets::AdColorPicker::Size::Small &&
                    picker->mode() == adqt::widgets::AdColorPicker::Mode::Solid &&
                    picker->trigger() == adqt::widgets::AdColorPicker::Trigger::Hover &&
                    !picker->allowClear() &&
                    picker->placement() == adqt::widgets::AdColorPicker::Placement::Bottom &&
                    picker->popupLayerMode() ==
                        adqt::widgets::AdColorPicker::PopupLayerMode::QtTool &&
                    picker->popupContentPlacement() ==
                        adqt::widgets::AdColorPicker::PopupContentPlacement::Top,
                "color, fill, stroke, and width-color editors should share the picker shell");
        picker->setPopupVisible(true);
        QCoreApplication::processEvents();
        auto* sampler = dynamic_cast<ColorPickerSamplerButton*>(picker->previewContent());
        require(sampler != nullptr && sampler->focusPolicy() == Qt::NoFocus &&
                    sampler->sizeClass() == adqt::widgets::AdButton::SizeClass::Small &&
                    sampler->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Outline &&
                    sampler->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral &&
                    sampler->toolTip() == QStringLiteral("Pick color from canvas") &&
                    sampler->accessibleName() == sampler->toolTip(),
                "canvas-color samplers should use the fill-color trigger's outlined style");
        picker->setPopupVisible(false);
    }
    require(dynamic_cast<ColorPickerTrigger*>(colorPicker->triggerContent()) != nullptr &&
                dynamic_cast<ColorPickerTrigger*>(fillPicker->triggerContent()) != nullptr &&
                dynamic_cast<ColorPickerTrigger*>(strokePicker->triggerContent()) != nullptr &&
                dynamic_cast<ColorPickerTrigger*>(widthColorPicker->triggerContent()) != nullptr &&
                colorPicker->alphaChannelEnabled() && fillPicker->alphaChannelEnabled() &&
                strokePicker->alphaChannelEnabled() && widthColorPicker->alphaChannelEnabled(),
            "all picker previews should use one trigger implementation and enable alpha");
    auto* fillTrigger = dynamic_cast<ColorPickerTrigger*>(fillPicker->triggerContent());
    auto* fillSampler = dynamic_cast<ColorPickerSamplerButton*>(fillPicker->previewContent());
    require(fillTrigger != nullptr && fillSampler != nullptr &&
                fillSampler->sizeClass() == fillTrigger->sizeClass() &&
                fillSampler->buttonStyle() == fillTrigger->buttonStyle() &&
                fillSampler->accentRole() == fillTrigger->accentRole(),
            "canvas sampler button chrome should match the fill-color picker trigger");

    QWidget* startArrowhead = controlWithAccessibleName(palette, "Start arrowhead");
    QWidget* endArrowhead = controlWithAccessibleName(palette, "End arrowhead");
    QWidget* textAlignment = controlWithAccessibleName(palette, "Text alignment");
    const QList<QWidget*> optionTriggers{
        startArrowhead,
        endArrowhead,
        textAlignment,
    };
    for (QWidget* trigger : optionTriggers) {
        adqt::widgets::AdPopover* popover = popoverForTrigger(trigger);
        require(dynamic_cast<IconValuePreviewTrigger*>(trigger) != nullptr &&
                    trigger->focusPolicy() == Qt::NoFocus && popover != nullptr &&
                    popover->triggers() == adqt::widgets::AdPopover::Trigger::Hover &&
                    popover->placement() == adqt::widgets::AdPopover::Placement::Bottom &&
                    popover->popupLayerMode() == adqt::widgets::AdPopover::PopupLayerMode::QtTool &&
                    popover->arrowVisible() && popover->contentMargins() == QMargins(8, 8, 8, 8),
                "arrowhead and alignment editors should share the icon-option popover shell");
        require(popover->contentWidget() == nullptr,
                "icon-option content should remain lazy until first opening");
        popover->show();
        QCoreApplication::processEvents();
        require(popover->contentWidget() != nullptr,
                "icon-option content should materialize when the popover opens");
        popover->hide();
    }
    QLayout* startLayout = popoverForTrigger(startArrowhead)->contentWidget()->layout();
    QLayout* endLayout = popoverForTrigger(endArrowhead)->contentWidget()->layout();
    QLayout* alignmentLayout = popoverForTrigger(textAlignment)->contentWidget()->layout();
    require(
        qobject_cast<QGridLayout*>(startLayout) != nullptr &&
            qobject_cast<QGridLayout*>(endLayout) != nullptr && startLayout->count() == 14 &&
            endLayout->count() == 14 && qobject_cast<QHBoxLayout*>(alignmentLayout) != nullptr &&
            alignmentLayout->count() == 3 && startLayout->spacing() == alignmentLayout->spacing(),
        "icon-option configuration should preserve arrow grids and the alignment row");

    palette.setActiveTool(ScreenshotToolPalette::Tool::RectangleHighlight);
    colorPicker = colorPickerWithAccessibleName(palette, "Highlight color");
    require(colorPicker != nullptr,
            "the active highlight family should expose its shared color picker");
    if (colorPicker->previewContent() == nullptr) {
        colorPicker->setPopupVisible(true);
        QCoreApplication::processEvents();
    }
    auto* sampler = dynamic_cast<ColorPickerSamplerButton*>(colorPicker->previewContent());
    require(colorPicker != nullptr && sampler != nullptr,
            "the active highlight family should rebuild its color sampler on demand");
    adqt::widgets::AdColorPicker* samplingTarget = nullptr;
    QObject::connect(
        &palette, &ScreenshotToolPalette::canvasColorSamplingRequested, &palette,
        [&samplingTarget](adqt::widgets::AdColorPicker* picker) { samplingTarget = picker; });
    colorPicker->setPopupVisible(true);
    require(colorPicker->popupVisible(), "color picker popup should open before sampling");
    sampler->click();
    QCoreApplication::processEvents();
    require(samplingTarget == colorPicker && !colorPicker->popupVisible(),
            "canvas sampler should close its picker and request sampling for the owning picker");
    int completedColorChanges = 0;
    QObject::connect(
        colorPicker, &adqt::widgets::AdColorPicker::editingFinished, &palette,
        [&completedColorChanges](const adqt::widgets::AdColorValue&) { ++completedColorChanges; });
    colorPicker->commitValue(adqt::widgets::AdColorValue::solid(QColor(QStringLiteral("#123456"))));
    require(completedColorChanges == 1 &&
                colorPicker->value().solidColor == QColor(QStringLiteral("#123456")),
            "externally sampled colors should use completed picker change semantics");

    constexpr qreal toolbarScale = 1.5;
    require(palette.setPhysicalScale(toolbarScale),
            "shared editor metric test should change toolbar scale");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    fontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Text font family"));
    const QSize scaledSelectSize(qRound(selectReferenceSize.width() * toolbarScale),
                                 qRound(selectReferenceSize.height() * toolbarScale));
    require(fontSelect != nullptr && fontSelect->size() == scaledSelectSize,
            "the rebuilt text select should apply the pending toolbar metrics");
    palette.setActiveTool(ScreenshotToolPalette::Tool::RectangleFilter);
    filterSelect =
        palette.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenshotFilterTypeSelect"));
    require(filterSelect != nullptr && filterSelect->size() == scaledSelectSize,
            "the rebuilt filter select should apply the same pending toolbar metrics");
}

void canvasColorSamplerButtonRequestsAndCommits() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    auto* picker = colorPickerWithAccessibleName(palette, "Stroke color");
    require(picker != nullptr && picker->previewContent() == nullptr,
            "drawing color picker sampler should remain lazy before first opening");
    picker->setPopupVisible(true);
    QCoreApplication::processEvents();
    auto* sampler = picker == nullptr
                        ? nullptr
                        : dynamic_cast<ColorPickerSamplerButton*>(picker->previewContent());
    require(picker != nullptr && sampler != nullptr && sampler->focusPolicy() == Qt::NoFocus &&
                sampler->sizeClass() == adqt::widgets::AdButton::SizeClass::Small &&
                sampler->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Outline &&
                sampler->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral &&
                sampler->toolTip() == QStringLiteral("Pick color from canvas") &&
                sampler->accessibleName() == sampler->toolTip(),
            "drawing color picker should expose an outlined canvas sampler button");

    const QImage initialSamplerImage = renderButton(*sampler);
    picker->setValue(adqt::widgets::AdColorValue::solid(QColor(QStringLiteral("#12ab34"))));
    QCoreApplication::processEvents();
    require(renderButton(*sampler) != initialSamplerImage,
            "canvas sampler should repaint to display the current picker color");

    adqt::widgets::AdColorPicker* samplingTarget = nullptr;
    QObject::connect(
        &palette, &ScreenshotToolPalette::canvasColorSamplingRequested, &palette,
        [&samplingTarget](adqt::widgets::AdColorPicker* requested) { samplingTarget = requested; });
    picker->setPopupVisible(true);
    require(picker->popupVisible(), "color picker popup should open before sampling");
    sampler->click();
    QCoreApplication::processEvents();
    require(samplingTarget == picker && !picker->popupVisible(),
            "canvas sampler should close its picker and request the owning picker");

    int completedChanges = 0;
    QObject::connect(
        picker, &adqt::widgets::AdColorPicker::editingFinished, &palette,
        [&completedChanges](const adqt::widgets::AdColorValue&) { ++completedChanges; });
    const QColor sampledColor(QStringLiteral("#123456"));
    picker->commitValue(adqt::widgets::AdColorValue::solid(sampledColor));
    require(completedChanges == 1 && picker->value().solidColor == sampledColor,
            "sampled colors should use completed picker change semantics");
}

void styleToolbarRowSpacingFollowsPhysicalScale() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    const QVector<QPair<ScreenshotToolPalette::Tool, QString>> tools{
        {ScreenshotToolPalette::Tool::Shape, QStringLiteral("screenshotRectangleStyleControls")},
        {ScreenshotToolPalette::Tool::Arrow, QStringLiteral("screenshotArrowStyleControls")},
        {ScreenshotToolPalette::Tool::RectangleHighlight,
         QStringLiteral("screenshotHighlightStyleControls")},
        {ScreenshotToolPalette::Tool::PenHighlight,
         QStringLiteral("screenshotPenHighlightStyleControls")},
        {ScreenshotToolPalette::Tool::Text, QStringLiteral("screenshotTextStyleControls")},
        {ScreenshotToolPalette::Tool::SerialNumber,
         QStringLiteral("screenshotSerialNumberStyleControls")},
        {ScreenshotToolPalette::Tool::RectangleFilter,
         QStringLiteral("screenshotFilterStyleControls")},
        {ScreenshotToolPalette::Tool::Spotlight,
         QStringLiteral("screenshotSpotlightStyleControls")},
        {ScreenshotToolPalette::Tool::Watermark,
         QStringLiteral("screenshotWatermarkStyleControls")},
    };
    const auto activateAndGetSpacing = [&palette](ScreenshotToolPalette::Tool tool,
                                                  const QString& objectName) {
        palette.setActiveTool(tool);
        QWidget* controls = palette.findChild<QWidget*>(objectName);
        require(controls != nullptr && !controls->isHidden() && controls->layout() != nullptr,
                "the active style toolbar row should be materialized on demand");
        return controls->layout()->spacing();
    };

    constexpr qreal toolbarCounterScale = 1.5;
    const int referenceSpacing = activateAndGetSpacing(tools.first().first, tools.first().second);
    require(palette.setPhysicalScale(toolbarCounterScale), "toolbar scale should change");
    for (const auto& tool : tools) {
        const int spacing = activateAndGetSpacing(tool.first, tool.second);
        require(spacing == qRound(referenceSpacing * toolbarCounterScale),
                "an active style toolbar row should use the scaled spacing");
    }
    require(palette.stylePanel() != nullptr && palette.stylePanel()->layout() != nullptr &&
                palette.stylePanel()->layout()->spacing() == 0,
            "the outer style panel should not add spacing between mutually exclusive rows");
}

void styleToolbarControlsDoNotEnterTabFocusChain() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    for (ScreenshotToolPalette::Tool tool : {
             ScreenshotToolPalette::Tool::Shape,
             ScreenshotToolPalette::Tool::Arrow,
             ScreenshotToolPalette::Tool::RectangleHighlight,
             ScreenshotToolPalette::Tool::PenHighlight,
             ScreenshotToolPalette::Tool::Text,
             ScreenshotToolPalette::Tool::SerialNumber,
             ScreenshotToolPalette::Tool::RectangleFilter,
             ScreenshotToolPalette::Tool::PenFilter,
             ScreenshotToolPalette::Tool::Spotlight,
             ScreenshotToolPalette::Tool::Watermark,
         }) {
        require(palette.ensureStyleFamily(tool),
                "focus-chain test should materialize each inspected family");
    }
    require(palette.ensureActionFamily(ScreenshotToolPalette::ActionFamily::ScrollingRecognition),
            "focus-chain test should materialize scrolling controls");
    require(palette.ensureActionFamily(ScreenshotToolPalette::ActionFamily::Selection),
            "focus-chain test should materialize selection controls");

    const QList<adqt::widgets::AdRadio*> modeButtons =
        palette.findChildren<adqt::widgets::AdRadio*>();
    require(modeButtons.size() == 21,
            "style toolbars should expose the expected number of mode radios");
    for (adqt::widgets::AdRadio* button : modeButtons) {
        require(button != nullptr && button->focusPolicy() == Qt::NoFocus,
                "style toolbar radio buttons should not enter the Tab focus chain");
    }

    for (const QString& objectName : {QStringLiteral("screenshotScrollingVerticalButton"),
                                      QStringLiteral("screenshotScrollingHorizontalButton")}) {
        auto* button = palette.findChild<adqt::widgets::AdButton*>(objectName);
        require(button != nullptr && button->focusPolicy() == Qt::NoFocus,
                "scrolling mode buttons should not enter the Tab focus chain");
    }

    const QStringList selectObjectNames{
        QStringLiteral("screenshotFilterTypeSelect"),
        QStringLiteral("screenshotPenFilterTypeSelect"),
    };
    for (const QString& objectName : selectObjectNames) {
        auto* select = palette.findChild<adqt::widgets::AdSelect*>(objectName);
        require(select != nullptr && select->focusPolicy() == Qt::NoFocus,
                "filter type selectors should not enter the Tab focus chain");
    }

    const QStringList selectAccessibleNames{
        QStringLiteral("Text font family"),
        QStringLiteral("Watermark font family"),
        QStringLiteral("Sequence number font family"),
    };
    const QList<adqt::widgets::AdSelect*> selects =
        palette.findChildren<adqt::widgets::AdSelect*>();
    for (const QString& accessibleName : selectAccessibleNames) {
        int matches = 0;
        for (adqt::widgets::AdSelect* select : selects) {
            if (select != nullptr && select->accessibleName() == accessibleName) {
                ++matches;
                require(select->focusPolicy() == Qt::NoFocus,
                        "font family selectors should not enter the Tab focus chain");
            }
        }
        require(matches == 1, "each style toolbar font family selector should be present once");
    }

    const QStringList sliderObjectNames{
        QStringLiteral("screenshotWatermarkOpacitySlider"),
        QStringLiteral("screenshotSpotlightOpacitySlider"),
        QStringLiteral("screenshotFilterIntensitySlider"),
        QStringLiteral("screenshotPenFilterIntensitySlider"),
    };
    for (const QString& objectName : sliderObjectNames) {
        auto* slider = palette.findChild<adqt::widgets::AdSlider*>(objectName);
        require(slider != nullptr && slider->focusPolicy() == Qt::NoFocus,
                "style toolbar sliders should not enter the Tab focus chain");
    }

    auto* watermarkText = palette.findChild<adqt::widgets::AdLineEdit*>(
        QStringLiteral("screenshotWatermarkTextEdit"));
    require(watermarkText != nullptr && watermarkText->focusPolicy() == Qt::ClickFocus,
            "watermark text should accept mouse focus without entering the Tab focus chain");

    const QList<adqt::widgets::AdColorPicker*> colorPickers =
        palette.findChildren<adqt::widgets::AdColorPicker*>();
    require(!colorPickers.isEmpty(), "style toolbar should expose color picker controls");
    for (adqt::widgets::AdColorPicker* picker : colorPickers) {
        require(picker != nullptr && picker->focusPolicy() == Qt::NoFocus,
                "style toolbar color pickers should not enter the Tab focus chain");
    }

    const QStringList styleControlObjectNames{
        QStringLiteral("screenshotRectangleStyleControls"),
        QStringLiteral("screenshotArrowStyleControls"),
        QStringLiteral("screenshotHighlightStyleControls"),
        QStringLiteral("screenshotPenHighlightStyleControls"),
        QStringLiteral("screenshotSpotlightStyleControls"),
        QStringLiteral("screenshotTextStyleControls"),
        QStringLiteral("screenshotSerialNumberStyleControls"),
        QStringLiteral("screenshotFilterStyleControls"),
        QStringLiteral("screenshotPenFilterStyleControls"),
        QStringLiteral("screenshotWatermarkStyleControls"),
    };
    for (const QString& objectName : styleControlObjectNames) {
        QWidget* controls = palette.findChild<QWidget*>(objectName);
        require(controls != nullptr && controls->layout() != nullptr,
                "each style toolbar control row should expose its layout");
        QLayout* layout = controls->layout();
        for (int index = 0; index < layout->count(); ++index) {
            auto* button = qobject_cast<adqt::widgets::AdButton*>(layout->itemAt(index)->widget());
            if (button != nullptr) {
                require(button->focusPolicy() == Qt::NoFocus,
                        "style toolbar buttons should not enter the Tab focus chain");
            }
        }
    }
}

void toolbarScalingDoesNotRelayoutPopupContent() {
    constexpr qreal toolbarCounterScale = 1.5;
    const auto pickerWithAccessibleName = [](ScreenshotToolPalette& palette, const QString& name) {
        return colorPickerWithAccessibleName(palette, name.toUtf8().constData());
    };
    const auto openColorPicker = [](adqt::widgets::AdColorPicker* picker) {
        require(picker != nullptr, "color picker should be present");
        picker->setPopupLayerMode(QApplication::platformName() == QStringLiteral("offscreen")
                                      ? adqt::widgets::AdColorPicker::PopupLayerMode::InWindow
                                      : adqt::widgets::AdColorPicker::PopupLayerMode::QtTool);
        picker->setPopupVisible(true);
        QCoreApplication::processEvents();
        auto* popover = picker->findChild<adqt::widgets::AdPopover*>();
        require(popover != nullptr, "color picker should own a popup");
        return popover;
    };
    struct PopupContentLayoutSnapshot {
        adqt::widgets::AdPopover* popover = nullptr;
        QWidget* content = nullptr;
        QSize sizeHint;
        QMargins margins;
        int spacing = 0;
    };
    const auto snapshotPopover = [](adqt::widgets::AdPopover* popover) {
        require(popover != nullptr, "popup should be present");
        QWidget* content = popover->contentWidget();
        require(content != nullptr && content->layout() != nullptr,
                "popup content should have a layout");
        return PopupContentLayoutSnapshot{
            popover,
            content,
            content->sizeHint(),
            content->layout()->contentsMargins(),
            content->layout()->spacing(),
        };
    };
    const auto requirePopoverUnchanged = [](const PopupContentLayoutSnapshot& snapshot) {
        QLayout* contentLayout = snapshot.content->layout();
        require(snapshot.content->sizeHint() == snapshot.sizeHint,
                "toolbar scaling must not change popup intrinsic size");
        require(contentLayout != nullptr && contentLayout->contentsMargins() == snapshot.margins,
                "toolbar scaling must not change popup content margins");
        require(contentLayout->spacing() == snapshot.spacing,
                "toolbar scaling must not change popup content spacing");
    };
    const auto spacingAfter = [](QLayout* layout, QWidget* widget) {
        const int index = layout != nullptr ? layout->indexOf(widget) : -1;
        return index >= 0 && index + 1 < layout->count() ? layout->itemAt(index + 1)->spacerItem()
                                                         : nullptr;
    };

    {
        ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
        palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
        adqt::widgets::AdPopover* strokePopover =
            openColorPicker(pickerWithAccessibleName(palette, QStringLiteral("Stroke color")));
        adqt::widgets::AdPopover* fillPopover =
            openColorPicker(pickerWithAccessibleName(palette, QStringLiteral("Fill color")));
        adqt::widgets::AdButton* strokeStyle =
            popoverButtonWithTooltip(strokePopover, "Solid stroke");
        auto* fillStyle =
            qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, "Line fill"));
        adqt::widgets::AdButton* fillPreset =
            popoverButtonWithTooltip(fillPopover, "Fill color transparent");
        require(strokeStyle != nullptr && fillStyle != nullptr && fillPreset != nullptr,
                "shape popup and toolbar options should be present");
        require(popoverButtonWithTooltip(fillPopover, "Line fill") == nullptr,
                "fill style option should not be in the fill color popup");

        QWidget* mainToolbarControl = controlWithTooltip(palette, "Select elements");
        auto* rectangleShape =
            qobject_cast<adqt::widgets::AdRadio*>(controlWithTooltip(palette, "Rectangle"));
        require(mainToolbarControl != nullptr && rectangleShape != nullptr,
                "main and shape toolbar controls should be present");
        const QSize referenceMainToolbarControlSize = mainToolbarControl->size();
        const QSize referenceFillStyleSize = fillStyle->size();
        const QSize referenceRectangleShapeSize = rectangleShape->size();
        const QSize referenceStrokeOptionSize = strokeStyle->size();
        const QSize referenceFillOptionSize = fillPreset->size();
        const PopupContentLayoutSnapshot strokeSnapshot = snapshotPopover(strokePopover);
        const PopupContentLayoutSnapshot fillSnapshot = snapshotPopover(fillPopover);

        require(palette.setPhysicalScale(toolbarCounterScale), "shape toolbar scale should change");
        QCoreApplication::processEvents();
        require(mainToolbarControl->size() ==
                    QSize(qRound(referenceMainToolbarControlSize.width() * toolbarCounterScale),
                          qRound(referenceMainToolbarControlSize.height() * toolbarCounterScale)),
                "the main toolbar should follow its physical counter-scale");
        require(fillStyle->size() ==
                    QSize(qRound(referenceFillStyleSize.width() * toolbarCounterScale),
                          qRound(referenceFillStyleSize.height() * toolbarCounterScale)),
                "the fill style toolbar button should follow its physical counter-scale");
        require(rectangleShape->size() ==
                    QSize(qRound(referenceRectangleShapeSize.width() * toolbarCounterScale),
                          qRound(referenceRectangleShapeSize.height() * toolbarCounterScale)),
                "shape button-group options should follow the toolbar physical counter-scale");
        require(strokeStyle->size() == referenceStrokeOptionSize &&
                    fillPreset->size() == referenceFillOptionSize,
                "shape popup option buttons should use the popup window's DPI");
        requirePopoverUnchanged(strokeSnapshot);
        requirePopoverUnchanged(fillSnapshot);
        strokePopover->hide();
        fillPopover->hide();
        QCoreApplication::processEvents();
    }

    {
        ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
        palette.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
        adqt::widgets::AdPopover* arrowStrokePopover = openColorPicker(
            pickerWithAccessibleName(palette, QStringLiteral("Arrow stroke color")));
        adqt::widgets::AdButton* arrowStrokeStyle =
            popoverButtonWithTooltip(arrowStrokePopover, "Solid arrow stroke");
        auto* arrowType =
            qobject_cast<adqt::widgets::AdRadio*>(controlWithTooltip(palette, "Straight arrow"));
        QWidget* startArrowheadTrigger = controlWithAccessibleName(palette, "Start arrowhead");
        QWidget* endArrowheadTrigger = controlWithAccessibleName(palette, "End arrowhead");
        adqt::widgets::AdPopover* startArrowheadPopover =
            showPopoverForTrigger(startArrowheadTrigger);
        adqt::widgets::AdPopover* endArrowheadPopover = showPopoverForTrigger(endArrowheadTrigger);
        adqt::widgets::AdButton* startArrowhead =
            popoverButtonWithTooltip(startArrowheadPopover, "Start arrowhead none");
        adqt::widgets::AdButton* endArrowhead =
            popoverButtonWithTooltip(endArrowheadPopover, "End arrowhead none");
        require(arrowStrokeStyle != nullptr && arrowType != nullptr && startArrowhead != nullptr &&
                    endArrowhead != nullptr,
                "arrow toolbar and popup options should be present");

        const QSize referenceTriggerSize = startArrowheadTrigger->size();
        const QSize referenceArrowTypeSize = arrowType->size();
        const QSize referenceStrokeOptionSize = arrowStrokeStyle->size();
        const QSize referenceStartOptionSize = startArrowhead->size();
        const QSize referenceEndOptionSize = endArrowhead->size();
        const PopupContentLayoutSnapshot arrowStrokeSnapshot = snapshotPopover(arrowStrokePopover);
        const PopupContentLayoutSnapshot startSnapshot = snapshotPopover(startArrowheadPopover);
        const PopupContentLayoutSnapshot endSnapshot = snapshotPopover(endArrowheadPopover);

        require(palette.setPhysicalScale(toolbarCounterScale), "arrow toolbar scale should change");
        QCoreApplication::processEvents();
        require(startArrowheadTrigger->size() ==
                        QSize(qRound(referenceTriggerSize.width() * toolbarCounterScale),
                              qRound(referenceTriggerSize.height() * toolbarCounterScale)) &&
                    endArrowheadTrigger->size() == startArrowheadTrigger->size(),
                "active arrowhead triggers should follow the toolbar physical counter-scale");
        require(arrowType->size() ==
                    QSize(qRound(referenceArrowTypeSize.width() * toolbarCounterScale),
                          qRound(referenceArrowTypeSize.height() * toolbarCounterScale)),
                "arrow-type button-group options should follow the toolbar physical counter-scale");
        require(arrowStrokeStyle->size() == referenceStrokeOptionSize &&
                    startArrowhead->size() == referenceStartOptionSize &&
                    endArrowhead->size() == referenceEndOptionSize,
                "arrow popup option buttons should use the popup window's DPI");
        requirePopoverUnchanged(arrowStrokeSnapshot);
        requirePopoverUnchanged(startSnapshot);
        requirePopoverUnchanged(endSnapshot);
        arrowStrokePopover->hide();
        startArrowheadPopover->hide();
        endArrowheadPopover->hide();
        QCoreApplication::processEvents();
    }

    {
        ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
        palette.setActiveTool(ScreenshotToolPalette::Tool::RectangleHighlight);
        QWidget* controls =
            palette.findChild<QWidget*>(QStringLiteral("screenshotHighlightStyleControls"));
        QSpacerItem* spacing = controls != nullptr && controls->layout() != nullptr
                                   ? controls->layout()->itemAt(1)->spacerItem()
                                   : nullptr;
        require(spacing != nullptr, "highlight mode selector should have a dedicated trailing gap");
        const int referenceSpacing = spacing->sizeHint().width();
        require(palette.setPhysicalScale(toolbarCounterScale),
                "highlight toolbar scale should change");
        require(spacing->sizeHint().width() == qRound(referenceSpacing * toolbarCounterScale),
                "highlight mode selector spacing should follow the toolbar physical counter-scale");
    }

    {
        ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
        palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
        QWidget* strokePicker = controlWithAccessibleName(palette, "Text stroke width");
        QWidget* controls =
            palette.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls"));
        QWidget* strokeRoot = styleEditorRoot(controls, "text-stroke");
        QSpacerItem* spacing =
            controls != nullptr ? spacingAfter(controls->layout(), strokeRoot) : nullptr;
        require(strokeRoot != nullptr && strokeRoot->isAncestorOf(strokePicker),
                "text stroke controls should stay inside their stable editor root");
        require(spacing != nullptr, "text stroke color should have a dedicated trailing gap");
        const int referenceSpacing = spacing->sizeHint().width();
        require(palette.setPhysicalScale(toolbarCounterScale), "text toolbar scale should change");
        require(spacing->sizeHint().width() == qRound(referenceSpacing * toolbarCounterScale),
                "text style control spacing should follow the toolbar physical counter-scale");
    }
}

void popupColorEditorButtonsKeepPopupScaleAfterToolbarDpiCommit() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});

    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Shape),
            "popup scale test should materialize the shape style family");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    auto* strokePicker = colorPickerWithAccessibleName(palette, "Stroke color");
    auto* fillPicker = colorPickerWithAccessibleName(palette, "Fill color");
    require(strokePicker != nullptr && fillPicker != nullptr,
            "shape color pickers should materialize with the shape style family");
    strokePicker->setPopupVisible(true);
    fillPicker->setPopupVisible(true);
    QCoreApplication::processEvents();
    auto* strokePopover = strokePicker->findChild<adqt::widgets::AdPopover*>();
    auto* fillPopover = fillPicker->findChild<adqt::widgets::AdPopover*>();
    auto* strokeStyle = popoverButtonWithTooltip(strokePopover, "Solid stroke");
    auto* fillPreset = popoverButtonWithTooltip(fillPopover, "Fill color transparent");
    require(strokeStyle != nullptr && fillPreset != nullptr,
            "shape color editor popup buttons should materialize with their style family");
    const QFont strokeFont = strokeStyle->font();
    const QFont fillFont = fillPreset->font();
    const QSize strokeIconSize = strokeStyle->iconSize();
    const QSize fillIconSize = fillPreset->iconSize();
    const QSize strokeHint = strokeStyle->sizeHint();
    const QSize fillHint = fillPreset->sizeHint();

    require(palette.setScaleContext(adqt::widgets::AdControlScaleContext::fromDprs(1.5, 1.0)),
            "toolbar should publish one mixed-DPI context to its layout and controls");

    require(strokeStyle->font() == strokeFont && strokeStyle->iconSize() == strokeIconSize &&
                strokeStyle->sizeHint() == strokeHint,
            "the first stroke-color popup button should retain the popup scale");
    require(fillPreset->font() == fillFont && fillPreset->iconSize() == fillIconSize &&
                fillPreset->sizeHint() == fillHint,
            "the first fill-color popup button should retain the popup scale");
}

void selectToolExposesDedicatedActionToolbar() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    QCoreApplication::processEvents();

    require(!palette.styleToolbarVisible(), "select tool should hide the style toolbar");
    require(palette.actionToolbarVisible(), "select tool should show its action toolbar");
    QWidget* controls = palette.actionPanel();
    require(controls != nullptr && !controls->isHidden(),
            "select action toolbar should be visible");
    require(controls->height() == palette.mainPanel()->height(),
            "select action toolbar should match the main toolbar height");
    auto* layout = qobject_cast<QBoxLayout*>(controls->layout());
    require(layout != nullptr, "select action toolbar should use a box layout");
    require(layout->count() >= 17,
            "select action toolbar should retain its action groups alongside alternate modes");

    const char* buttonActions[] = {
        "Send to back",   "Send backward",          "Bring forward",
        "Bring to front", "Copy selected elements", "Delete selected elements",
    };
    for (const char* action : buttonActions) {
        QWidget* control = controlWithTooltip(palette, action);
        require(control != nullptr, "select action is missing");
        require(!control->isEnabled(), "select action should be disabled without a selection");
    }
    auto* opacityIcon =
        palette.findChild<QLabel*>(QStringLiteral("screenshotSelectionOpacityIcon"));
    auto* opacitySlider = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotSelectionOpacitySlider"));
    require(opacityIcon != nullptr && !opacityIcon->pixmap().isNull(),
            "selection opacity should display its icon");
    require(opacityIcon->size() == QSize(32, 32),
            "selection opacity icon should match the action toolbar height");
    require(palette.findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenshotSelectionOpacityButton")) == nullptr,
            "selection opacity should not expose a value button");
    require(opacitySlider != nullptr, "selection opacity should use a slider");
    require(layout->spacing() == 0 &&
                layout->indexOf(opacitySlider) == layout->indexOf(opacityIcon) + 1,
            "selection opacity icon should sit directly left of the slider");
    const int sliderIndex = layout->indexOf(opacitySlider);
    require(sliderIndex >= 0 && layout->itemAt(sliderIndex + 1)->spacerItem() != nullptr &&
                layout->itemAt(sliderIndex + 2)->widget() != nullptr,
            "selection opacity controls should keep the normal trailing spacing");
    require(opacitySlider->minimum() == 0 && opacitySlider->maximum() == 100 &&
                opacitySlider->value() == 100,
            "selection opacity slider should expose the full percentage range");
    require(!opacitySlider->isEnabled(),
            "selection opacity slider should be disabled without a selection");
    require(!palette.stepSelectionOpacity(-1),
            "the select tool wheel should ignore an empty selection");
    const QImage disabledOpacityIcon = opacityIcon->pixmap().toImage();

    SnowCanvasStyleToolbarState selectedState;
    selectedState.source = SnowCanvasStyleToolbarSource::SelectedRectangle;
    selectedState.shapeStyle.opacity = 0.4;
    selectedState.shapeStyleMixed = SnowCanvasShapeStyleMixedOpacity;
    palette.setStyleToolbarState(selectedState);
    require(opacitySlider->value() == 40 && opacitySlider->property("mixed").toBool(),
            "selection opacity should come from the element opacity property");
    require(opacityIcon->pixmap().toImage() != disabledOpacityIcon,
            "selection opacity icon should brighten with its enabled slider");
    selectedState.shapeStyle.opacity = 1.0;
    selectedState.shapeStyleMixed = 0;
    palette.setStyleToolbarState(selectedState);

    int wheelOpacityChangeCount = 0;
    qreal wheelOpacity = -1.0;
    const QMetaObject::Connection wheelOpacityConnection =
        QObject::connect(&palette, &ScreenshotToolPalette::selectionOpacityChanged,
                         [&wheelOpacityChangeCount, &wheelOpacity](qreal opacity) {
                             ++wheelOpacityChangeCount;
                             wheelOpacity = opacity;
                         });
    require(palette.stepSelectionOpacity(-1),
            "the select tool wheel should decrease selection opacity");
    require(opacitySlider->value() == 95 && wheelOpacityChangeCount == 1 &&
                qFuzzyCompare(wheelOpacity + 1.0, 1.95),
            "selection opacity wheel steps should use five percentage points");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    require(!palette.stepSelectionOpacity(-1) && wheelOpacityChangeCount == 1,
            "selection opacity wheel steps should require the select tool");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    opacitySlider = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotSelectionOpacitySlider"));
    require(opacitySlider != nullptr,
            "returning to Select should rebuild the selection opacity slider");
    QObject::disconnect(wheelOpacityConnection);
    palette.setSelectionOpacity(1.0);

    int commandCount = 0;
    qreal emittedOpacity = -1.0;
    QObject::connect(&palette, &ScreenshotToolPalette::sendSelectionToBackRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::sendSelectionBackwardRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::bringSelectionForwardRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::bringSelectionToFrontRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::selectionOpacityChanged,
                     [&commandCount, &emittedOpacity](qreal opacity) {
                         ++commandCount;
                         emittedOpacity = opacity;
                     });
    QObject::connect(&palette, &ScreenshotToolPalette::duplicateSelectionRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::deleteSelectionRequested,
                     [&commandCount]() { ++commandCount; });
    for (const char* action : buttonActions) {
        auto* button = qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, action));
        require(button != nullptr, "select action should be a button");
        require(button->isEnabled(), "select action should be enabled after a selection");
        button->click();
    }
    require(opacitySlider->isEnabled(), "opacity slider should be enabled after a selection");
    opacitySlider->setValue(65);
    require(commandCount == 7, "each select action should emit once");
    require(qFuzzyCompare(emittedOpacity + 1.0, 1.65),
            "opacity slider should emit its percentage as normalized opacity");

    palette.setSelectionOpacity(0.456);
    require(opacitySlider->value() == 46 && commandCount == 7,
            "synchronizing selection opacity should not emit an edit command");

    SnowCanvasStyleToolbarState defaultState;
    defaultState.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    palette.setStyleToolbarState(defaultState);
    for (const char* action : buttonActions) {
        require(!controlWithTooltip(palette, action)->isEnabled(),
                "select action should be disabled again after clearing the selection");
    }
    require(!opacitySlider->isEnabled(),
            "opacity slider should be disabled again after clearing the selection");
}

void selectionResetRemainsAvailableWithoutSelection() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    ScreenshotToolPalette palette(options);
    int resetCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::resetCanvasRequested,
                     [&resetCount]() { ++resetCount; });
    for (const auto alternate :
         {ScreenshotToolPalette::Tool::Ocr, ScreenshotToolPalette::Tool::Table}) {
        palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
        palette.prepareForDisplay();
        QPointer<adqt::widgets::AdButton> reset = palette.findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotResetCanvasButton"));
        require(reset != nullptr && !reset->isHidden() && reset->isEnabled(),
                "canvas reset should be available without a selection");
        require(reset->accentRole() == adqt::widgets::AdButton::AccentRole::Danger,
                "canvas reset should use the danger accent");
        require(reset->toolTip() == QStringLiteral("Reset") &&
                    reset->accessibleName() == QStringLiteral("Reset"),
                "canvas reset should have a tooltip and accessible name");
        auto* layout = qobject_cast<QBoxLayout*>(palette.actionPanel()->layout());
        const int index = layout->indexOf(reset);
        require(index >= 2, "canvas reset should follow the selection actions");
        QPointer<QFrame> separator = qobject_cast<QFrame*>(layout->itemAt(index - 2)->widget());
        require(separator != nullptr && !separator->isHidden() &&
                    layout->itemAt(index - 1)->spacerItem() != nullptr,
                "canvas reset should have the standard separator on its left");
        for (int i = index + 1; i < layout->count(); ++i) {
            QWidget* widget = layout->itemAt(i)->widget();
            require(widget == nullptr || widget->isHidden(),
                    "canvas reset must be the far-right visible selection control");
        }
        reset->click();
        SnowCanvasStyleToolbarState state;
        state.source = SnowCanvasStyleToolbarSource::SelectedRectangle;
        palette.setStyleToolbarState(state);
        require(reset->isEnabled(), "canvas reset should remain enabled with a selection");
        reset->click();
        state.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
        palette.setStyleToolbarState(state);
        require(reset->isEnabled(), "clearing selection must not disable canvas reset");
        palette.setActiveTool(alternate);
        require((reset == nullptr || reset->isHidden()) &&
                    (separator == nullptr || separator->isHidden()),
                "recognition modes should hide canvas reset and its separator");
    }
    require(resetCount == 4, "each reset click should emit exactly one canvas reset command");
}

void selectionAlignmentActionsFollowSelectionUnitCount() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    QCoreApplication::processEvents();

    const char* alignActions[] = {
        "Align left", "Center horizontally", "Align right",  "Distribute horizontally",
        "Align top",  "Center vertically",   "Align bottom", "Distribute vertically",
    };
    for (const char* action : alignActions) {
        QWidget* control = controlWithTooltip(palette, action);
        require(control != nullptr, "alignment action is missing");
        require(!control->isEnabled(), "alignment action should be disabled without a selection");
    }

    auto* layout = qobject_cast<QBoxLayout*>(palette.actionPanel()->layout());
    require(layout != nullptr, "select action toolbar should use a box layout");
    int layerEnd = -1;
    int alignStart = -1;
    int distributeHorizontal = -1;
    int alignTop = -1;
    for (int i = 0; i < layout->count(); ++i) {
        QWidget* widget = layout->itemAt(i)->widget();
        if (widget == nullptr) {
            continue;
        }
        const QString tooltip = widget->toolTip();
        if (tooltip == QStringLiteral("Bring to front")) {
            layerEnd = i;
        } else if (tooltip == QStringLiteral("Align left")) {
            alignStart = i;
        } else if (tooltip == QStringLiteral("Distribute horizontally")) {
            distributeHorizontal = i;
        } else if (tooltip == QStringLiteral("Align top")) {
            alignTop = i;
        }
    }
    require(layerEnd >= 0 && alignStart > layerEnd,
            "alignment actions should sit right of the layer ordering actions");
    require(distributeHorizontal > alignStart && alignTop > distributeHorizontal,
            "vertical alignment actions should follow the horizontal ones");
    bool dividerFound = false;
    for (int i = distributeHorizontal + 1; i < alignTop; ++i) {
        dividerFound =
            dividerFound || qobject_cast<QFrame*>(layout->itemAt(i)->widget()) != nullptr;
    }
    require(dividerFound,
            "a divider should separate the horizontal and vertical alignment actions");

    int commandCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::alignSelectionLeftRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::alignSelectionCenterHorizontallyRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::alignSelectionRightRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::alignSelectionTopRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::alignSelectionCenterVerticallyRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::alignSelectionBottomRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::distributeSelectionHorizontallyRequested,
                     [&commandCount]() { ++commandCount; });
    QObject::connect(&palette, &ScreenshotToolPalette::distributeSelectionVerticallyRequested,
                     [&commandCount]() { ++commandCount; });

    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::SelectedRectangle;
    state.selectedElementCount = 1;
    palette.setStyleToolbarState(state);
    for (const char* action : alignActions) {
        require(!controlWithTooltip(palette, action)->isEnabled(),
                "alignment action should stay disabled for a single selected element");
    }

    state.selectedElementCount = 2;
    palette.setStyleToolbarState(state);
    for (const char* action : alignActions) {
        QWidget* control = controlWithTooltip(palette, action);
        const bool distributes = QString(action).startsWith(QStringLiteral("Distribute"));
        require(control->isEnabled() != distributes,
                distributes ? "distribute should require three selected elements"
                            : "align should enable for two selected elements");
    }

    state.selectedElementCount = 3;
    palette.setStyleToolbarState(state);
    for (const char* action : alignActions) {
        auto* button = qobject_cast<adqt::widgets::AdButton*>(controlWithTooltip(palette, action));
        require(button != nullptr, "alignment action should be a button");
        require(button->isEnabled(), "alignment action should be enabled for three elements");
        button->click();
    }
    require(commandCount == 8, "each alignment action should emit exactly one command");

    SnowCanvasStyleToolbarState defaultState;
    defaultState.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    palette.setStyleToolbarState(defaultState);
    for (const char* action : alignActions) {
        require(!controlWithTooltip(palette, action)->isEnabled(),
                "alignment action should be disabled again after clearing the selection");
    }
}

void secondaryToolbarsStartHiddenUntilTheirToolIsSelected() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showShapeTool = true;
    ScreenshotToolPalette palette(options);

    QWidget* actionPanel = palette.actionPanel();
    QWidget* stylePanel = palette.stylePanel();
    require(actionPanel != nullptr, "select action toolbar should be created");
    require(stylePanel != nullptr, "style toolbar should be created");

    palette.show();
    QCoreApplication::processEvents();

    require(actionPanel->isHidden(),
            "select action toolbar should remain hidden on the palette's first display");
    require(stylePanel->isHidden(),
            "style toolbar should remain hidden on the palette's first display");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    QCoreApplication::processEvents();
    require(!actionPanel->isHidden() && stylePanel->isHidden(),
            "select tool should show only the select action toolbar");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QCoreApplication::processEvents();
    require(actionPanel->isHidden() && !stylePanel->isHidden(),
            "shape tool should show only the style toolbar");
}

void repeatedToolsAndDifferentialStyleSynchronizationAreNoOps() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showMoveTool = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showRectangleHighlightTool = true;
    options.showPenHighlightTool = true;
    options.showEraserTool = true;
    options.showFilterTool = true;
    options.showWatermarkTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showOcrTool = true;
    options.showScrollingScreenshotTool = true;
    ScreenshotToolPalette palette(options);
    static const ScreenshotToolPalette::Tool tools[] = {
        ScreenshotToolPalette::Tool::Move,
        ScreenshotToolPalette::Tool::Select,
        ScreenshotToolPalette::Tool::Shape,
        ScreenshotToolPalette::Tool::Arrow,
        ScreenshotToolPalette::Tool::Line,
        ScreenshotToolPalette::Tool::FreeDraw,
        ScreenshotToolPalette::Tool::RectangleHighlight,
        ScreenshotToolPalette::Tool::PenHighlight,
        ScreenshotToolPalette::Tool::Eraser,
        ScreenshotToolPalette::Tool::Filter,
        ScreenshotToolPalette::Tool::Watermark,
        ScreenshotToolPalette::Tool::Text,
        ScreenshotToolPalette::Tool::SerialNumber,
        ScreenshotToolPalette::Tool::Ocr,
        ScreenshotToolPalette::Tool::ScrollingScreenshot,
    };
    int visibilitySignalCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::visibleContentChanged,
                     [&visibilitySignalCount]() { ++visibilitySignalCount; });
    for (const auto tool : tools) {
        palette.setActiveTool(tool);
        static_cast<void>(palette.contentSizeHint());
        require(palette.activeToolForTests() == tool,
                "the tool enum should be the authoritative active identity");
        const quint64 layoutCommits = palette.layoutCommitCountForTests();
        const int signalCountSnapshot = visibilitySignalCount;
        palette.setActiveTool(tool);
        static_cast<void>(palette.contentSizeHint());
        require(palette.layoutCommitCountForTests() == layoutCommits &&
                    visibilitySignalCount == signalCountSnapshot,
                "repeating any active tool should be a complete layout and signal no-op");
    }

    palette.setActiveTool(ScreenshotToolPalette::Tool::RectangleHighlight);
    static_cast<void>(palette.contentSizeHint());
    const quint64 beforePen = palette.layoutCommitCountForTests();
    palette.setActiveTool(ScreenshotToolPalette::Tool::PenHighlight);
    static_cast<void>(palette.contentSizeHint());
    require(palette.layoutCommitCountForTests() - beforePen <= 1,
            "pen highlight should require at most one committed layout");
    const quint64 afterPen = palette.layoutCommitCountForTests();
    palette.setActiveTool(ScreenshotToolPalette::Tool::PenHighlight);
    static_cast<void>(palette.contentSizeHint());
    require(palette.layoutCommitCountForTests() == afterPen,
            "repeating pen highlight should commit no layout");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    state.shapeStyle = snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle;
    palette.setStyleToolbarState(state);
    const quint64 refreshes = palette.propertyGroupRefreshCountForTests();
    const quint64 noops = palette.styleStateNoopCountForTests();
    palette.setStyleToolbarState(state);
    require(palette.propertyGroupRefreshCountForTests() == refreshes &&
                palette.styleStateNoopCountForTests() == noops + 1,
            "replaying an identical style should refresh no property groups");

    state.shapeStyle.strokeWidth += 1.0;
    palette.setStyleToolbarState(state);
    require(palette.propertyGroupRefreshCountForTests() == refreshes + 1,
            "changing stroke width should refresh only its owning group");
    const quint64 beforeSelected = palette.propertyGroupRefreshCountForTests();
    state.source = SnowCanvasStyleToolbarSource::SelectedRectangle;
    palette.setStyleToolbarState(state);
    require(palette.propertyGroupRefreshCountForTests() == beforeSelected + 7,
            "a selected/default source transition should perform a full refresh");
    const quint64 beforeMixed = palette.propertyGroupRefreshCountForTests();
    state.shapeStyleMixed = SnowCanvasShapeStyleMixedStrokeWidth;
    palette.setStyleToolbarState(state);
    require(palette.propertyGroupRefreshCountForTests() == beforeMixed + 1,
            "a mixed-mask-only change should refresh its owning group");
}

void editorlessToolsRejectStaleStyleToolbarState() {
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showRectangleHighlightTool = true;
    options.showPenHighlightTool = true;
    options.showSpotlightTool = true;
    options.showEraserTool = true;
    options.showFilterTool = true;
    options.showWatermarkTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showOcrTool = true;
    options.showScrollingScreenshotTool = true;
    ScreenshotToolPalette palette(options);

    struct StyledTool {
        ScreenshotToolPalette::Tool tool;
        SnowCanvasStyleToolbarSource source;
    };
    static const StyledTool styledTools[] = {
        {ScreenshotToolPalette::Tool::Shape, SnowCanvasStyleToolbarSource::DefaultRectangle},
        {ScreenshotToolPalette::Tool::Arrow, SnowCanvasStyleToolbarSource::DefaultArrow},
        {ScreenshotToolPalette::Tool::Line, SnowCanvasStyleToolbarSource::DefaultLine},
        {ScreenshotToolPalette::Tool::FreeDraw, SnowCanvasStyleToolbarSource::DefaultFreeDraw},
        {ScreenshotToolPalette::Tool::RectangleHighlight,
         SnowCanvasStyleToolbarSource::DefaultRectangleHighlight},
        {ScreenshotToolPalette::Tool::PenHighlight,
         SnowCanvasStyleToolbarSource::DefaultPenHighlight},
        {ScreenshotToolPalette::Tool::Spotlight, SnowCanvasStyleToolbarSource::DefaultSpotlight},
        {ScreenshotToolPalette::Tool::RectangleFilter,
         SnowCanvasStyleToolbarSource::DefaultRectangleFilter},
        {ScreenshotToolPalette::Tool::PenFilter, SnowCanvasStyleToolbarSource::DefaultPenFilter},
        {ScreenshotToolPalette::Tool::Watermark, SnowCanvasStyleToolbarSource::Watermark},
        {ScreenshotToolPalette::Tool::Text, SnowCanvasStyleToolbarSource::DefaultText},
        {ScreenshotToolPalette::Tool::SerialNumber,
         SnowCanvasStyleToolbarSource::DefaultSerialNumber},
    };
    static const ScreenshotToolPalette::Tool editorlessTools[] = {
        ScreenshotToolPalette::Tool::Move,
        ScreenshotToolPalette::Tool::Eraser,
        ScreenshotToolPalette::Tool::Ocr,
        ScreenshotToolPalette::Tool::ScrollingScreenshot,
    };

    QWidget* stylePanel = palette.stylePanel();
    require(stylePanel != nullptr, "style toolbar should be created");

    for (const StyledTool& styledTool : styledTools) {
        SnowCanvasStyleToolbarState state;
        state.source = styledTool.source;
        palette.setActiveTool(styledTool.tool);
        palette.setStyleToolbarState(state);
        require(palette.styleToolbarVisible(),
                "a styled tool should show its editor before the transition");

        for (const ScreenshotToolPalette::Tool editorlessTool : editorlessTools) {
            palette.setActiveTool(editorlessTool);
            palette.setStyleToolbarState(state);
            const QList<QWidget*> styleEditors =
                stylePanel->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
            require(styleEditors.isEmpty(),
                    "editorless tools should evict the previous style editor");
            require(!palette.styleToolbarVisible() && stylePanel->isHidden(),
                    "stale canvas state must not restore an editorless tool's style toolbar");
            require(std::all_of(styleEditors.cbegin(), styleEditors.cend(),
                                [](const QWidget* editor) { return editor->isHidden(); }),
                    "editorless tools should not expose style editors");
            palette.setActiveTool(styledTool.tool);
            palette.setStyleToolbarState(state);
        }
    }
}

void crossTypeSelectionRecalculatesStyleToolbarSize() {
    ScreenshotToolPalette::Options options;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Text),
            "cross-type selection test should materialize the inspected text editor");

    QPointer<QWidget> textControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls"));
    require(textControls != nullptr, "text style controls should be present");
    require(textControls->layout() != nullptr, "text style controls should have a layout");
    textControls->layout()->activate();
    const QMargins margins = palette.stylePanel()->layout()->contentsMargins();
    const QSize expectedTextPanelSize =
        textControls->sizeHint() +
        QSize(margins.left() + margins.right(), margins.top() + margins.bottom());

    int visibleContentChangeCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::visibleContentChanged,
                     [&visibleContentChangeCount]() { ++visibleContentChangeCount; });

    SnowCanvasStyleToolbarState selectedState;
    selectedState.source = SnowCanvasStyleToolbarSource::SelectedText;
    selectedState.textStyle.opacity = 0.5;
    palette.setStyleToolbarState(selectedState);

    require(textControls.isNull(),
            "cross-type activation should replace only the destination row container");
    textControls = palette.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls"));
    require(!textControls.isNull() && !textControls->isHidden(),
            "selecting text with the sequence-number tool should show text controls");
    require(palette.stylePanel()->size() == expectedTextPanelSize,
            "style toolbar should use the selected text controls when recalculating its size");
    require(visibleContentChangeCount == 1,
            "cross-type selection should notify the toolbar host to resize");
}

void familiesHydratedAfterScaleKeepTheSamePhysicalSize() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showTextTool = true;
    options.showWatermarkTool = true;

    const auto flush = [](ScreenshotToolPalette& palette) {
        static_cast<void>(palette.contentSizeHint());
        QCoreApplication::processEvents();
    };
    const auto activate = [&flush](ScreenshotToolPalette& palette,
                                   ScreenshotToolPalette::Tool tool) {
        palette.setActiveTool(tool);
        flush(palette);
    };
    const auto secondarySize = [](const ScreenshotToolPalette& palette) {
        if (palette.actionToolbarVisible() && palette.actionPanel() != nullptr) {
            return palette.actionPanel()->size();
        }
        if (palette.styleToolbarVisible() && palette.stylePanel() != nullptr) {
            return palette.stylePanel()->size();
        }
        return QSize();
    };

    constexpr qreal scales[] = {0.75, 1.25};
    constexpr ScreenshotToolPalette::Tool tools[] = {
        ScreenshotToolPalette::Tool::Shape,
        ScreenshotToolPalette::Tool::Text,
        ScreenshotToolPalette::Tool::Watermark,
        ScreenshotToolPalette::Tool::Select,
    };
    for (const qreal scale : scales) {
        for (const ScreenshotToolPalette::Tool tool : tools) {
            ScreenshotToolPalette scaledFromReference(options);
            activate(scaledFromReference, tool);
            require(scaledFromReference.setPhysicalScale(scale),
                    "reference palette should accept the destination physical scale");
            flush(scaledFromReference);
            const QSize expected = secondarySize(scaledFromReference);
            require(!expected.isEmpty(),
                    "the visible secondary toolbar should expose a size at the destination scale");

            ScreenshotToolPalette hydratedAtScale(options);
            require(hydratedAtScale.setPhysicalScale(scale),
                    "destination palette should accept the physical scale before hydration");
            activate(hydratedAtScale, tool);
            require(secondarySize(hydratedAtScale) == expected,
                    "hydrating a family after a DPI scale change must keep the same physical size "
                    "as scaling a family that was created at reference scale");
        }

        ScreenshotToolPalette rematerialized(options);
        activate(rematerialized, ScreenshotToolPalette::Tool::Shape);
        require(rematerialized.setPhysicalScale(scale),
                "eviction rematerialization test should change the physical scale");
        flush(rematerialized);
        const QSize expectedShape = secondarySize(rematerialized);
        activate(rematerialized, ScreenshotToolPalette::Tool::Select);
        activate(rematerialized, ScreenshotToolPalette::Tool::Shape);
        require(secondarySize(rematerialized) == expectedShape,
                "rebuilding the shape family after a tool eviction must keep the destination "
                "physical size");
    }
}

void secondaryRowsDoNotDriftAcrossScaleRoundTrips() {
    for (auto tool : {ScreenshotToolPalette::Tool::Select, ScreenshotToolPalette::Tool::Shape}) {
        ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
        palette.setActiveTool(tool);
        static_cast<void>(palette.contentSizeHint());
        QCoreApplication::processEvents();
        QWidget* panel = tool == ScreenshotToolPalette::Tool::Select ? palette.actionPanel()
                                                                     : palette.stylePanel();
        const QSize initial = panel->size();
        for (qreal scale : {1.5, 1.0 / 1.5, 1.0}) {
            palette.setPhysicalScale(scale);
            static_cast<void>(palette.contentSizeHint());
            QCoreApplication::processEvents();
        }
        if (panel->size() != initial) {
            std::cerr << "row round trip " << static_cast<int>(tool) << " initial "
                      << initial.width() << 'x' << initial.height() << " final " << panel->width()
                      << 'x' << panel->height() << '\n';
        }
        require(panel->size() == initial, "secondary row accumulated geometry drift");
    }
}

void physicalScaleDefersHiddenStyleGroupGeometry() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showTextTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    require(palette.ensureStyleFamily(ScreenshotToolPalette::Tool::Text),
            "hidden geometry test should materialize the inspected text editor");
    static_cast<void>(palette.contentSizeHint());

    QWidget* textControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls"));
    require(textControls != nullptr && textControls->isHidden(),
            "text controls should be hidden while the shape tool is active");

    const QList<QWidget*> descendants = textControls->findChildren<QWidget*>();
    QList<QSize> originalSizes;
    originalSizes.reserve(descendants.size());
    for (QWidget* descendant : descendants) {
        originalSizes.append(descendant->size());
    }

    require(palette.setPhysicalScale(0.75),
            "changing the toolbar physical scale should be applied");
    static_cast<void>(palette.contentSizeHint());
    for (qsizetype index = 0; index < descendants.size(); ++index) {
        if (descendants.at(index)->size() != originalSizes.at(index)) {
            QWidget* changed = descendants.at(index);
            QStringList parentChain;
            for (QWidget* parent = changed; parent != nullptr; parent = parent->parentWidget()) {
                parentChain.append(QStringLiteral("%1[%2]").arg(
                    QString::fromLatin1(parent->metaObject()->className()), parent->objectName()));
            }
            std::cerr << "hidden style control resized: class="
                      << changed->metaObject()->className()
                      << " object=" << changed->objectName().toStdString()
                      << " accessible=" << changed->accessibleName().toStdString()
                      << " original=" << originalSizes.at(index).width() << 'x'
                      << originalSizes.at(index).height() << " current=" << changed->width() << 'x'
                      << changed->height()
                      << " parents=" << parentChain.join(QStringLiteral(" <- ")).toStdString()
                      << '\n';
        }
        require(descendants.at(index)->size() == originalSizes.at(index),
                "a DPI scale change must not resize hidden style controls");
    }

    palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    static_cast<void>(palette.contentSizeHint());
    QCoreApplication::processEvents();
    textControls = palette.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls"));
    require(!textControls->isHidden(), "text controls should become visible after selecting text");
    const QList<QWidget*> rebuiltDescendants = textControls->findChildren<QWidget*>();
    require(rebuiltDescendants.size() == originalSizes.size(),
            "the rebuilt text editor should preserve its control structure");
    bool geometryChanged = false;
    for (qsizetype index = 0; index < rebuiltDescendants.size(); ++index) {
        geometryChanged =
            geometryChanged || rebuiltDescendants.at(index)->size() != originalSizes.at(index);
    }
    require(geometryChanged,
            "a newly rebuilt visible style group must apply its pending DPI metrics");
}

void activeFilterAndWatermarkToolsExposeCanvasWheelSteps() {
    ScreenshotToolPalette::Options options;
    options.showFilterTool = true;
    options.showWatermarkTool = true;
    ScreenshotToolPalette palette(options);

    int filterChanges = 0;
    quint32 filterProperties = 0;
    SnowCanvasFilterStyle changedFilter;
    QObject::connect(&palette, &ScreenshotToolPalette::filterStyleChanged,
                     [&filterChanges, &filterProperties,
                      &changedFilter](const SnowCanvasFilterStyle& style, quint32 properties) {
                         ++filterChanges;
                         filterProperties = properties;
                         changedFilter = style;
                     });

    SnowCanvasStyleToolbarState rectangleState;
    rectangleState.source = SnowCanvasStyleToolbarSource::DefaultRectangleFilter;
    rectangleState.filterStyle.type = SnowCanvasFilterType::Mosaic;
    rectangleState.filterStyle.strength = 0.5;
    rectangleState.filterStyle.opacity = 1.0;
    rectangleState.filterStyle.strokeWidth = 24.0;
    palette.setStyleToolbarState(rectangleState);
    palette.setActiveTool(ScreenshotToolPalette::Tool::RectangleFilter);
    require(palette.stepFilterIntensity(1) && filterChanges == 1 &&
                filterProperties == SnowCanvasFilterStylePropertyStrength &&
                qFuzzyCompare(changedFilter.strength + 1.0, 1.51),
            "active Rectangle Filter canvas wheel steps should increase intensity by one percent");
    require(!palette.stepPenFilterStrokeWidth(1) && !palette.stepWatermarkFontSize(1),
            "inactive canvas wheel parameter handlers should reject input");

    rectangleState.filterStyle.strength = 1.0;
    palette.setStyleToolbarState(rectangleState);
    const int changesAtIntensityMaximum = filterChanges;
    require(
        palette.stepFilterIntensity(1) && filterChanges == changesAtIntensityMaximum,
        "Rectangle Filter should consume canvas wheel input without emitting past its upper clamp");

    rectangleState.filterStyle.type = SnowCanvasFilterType::Grayscale;
    palette.setStyleToolbarState(rectangleState);
    require(!palette.stepFilterIntensity(1) && filterChanges == 1,
            "Rectangle Filter effects without intensity should not consume canvas wheel input");

    SnowCanvasStyleToolbarState penState;
    penState.source = SnowCanvasStyleToolbarSource::DefaultPenFilter;
    penState.filterStyle.type = SnowCanvasFilterType::Mosaic;
    penState.filterStyle.strength = 0.5;
    penState.filterStyle.opacity = 1.0;
    penState.filterStyle.strokeWidth = 42.0;
    palette.setStyleToolbarState(penState);
    palette.setActiveTool(ScreenshotToolPalette::Tool::PenFilter);
    require(palette.stepPenFilterStrokeWidth(-1) && filterChanges == 2 &&
                filterProperties == SnowCanvasFilterStylePropertyStrokeWidth &&
                changedFilter.strokeWidth == 41.0,
            "active Pen Filter canvas wheel steps should change stroke width by one pixel");

    penState.filterStyle.strokeWidth = 72.0;
    palette.setStyleToolbarState(penState);
    const int changesAtPenMaximum = filterChanges;
    require(palette.stepPenFilterStrokeWidth(1) && filterChanges == changesAtPenMaximum,
            "Pen Filter should consume canvas wheel input without emitting past its upper clamp");

    SnowCanvasWatermarkConfig watermark;
    watermark.fontSize = 18.0;
    palette.setWatermarkConfig(watermark);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    int watermarkChanges = 0;
    SnowCanvasWatermarkConfig changedWatermark;
    QObject::connect(
        &palette, &ScreenshotToolPalette::watermarkConfigChanged,
        [&watermarkChanges, &changedWatermark](const SnowCanvasWatermarkConfig& config) {
            ++watermarkChanges;
            changedWatermark = config;
        });
    require(palette.stepWatermarkFontSize(1) && watermarkChanges == 1 &&
                changedWatermark.fontSize == 19.0,
            "active Watermark canvas wheel steps should change font size by one pixel");
    require(!palette.stepFilterIntensity(1) && !palette.stepPenFilterStrokeWidth(1),
            "filter canvas wheel handlers should reject input while Watermark is active");
}

void screenshotProductStyleProfileIsComplete() {
    const SnowCanvasStyleDefaults defaults =
        snow_shot::presentation::screenshotCanvasStyleDefaults();
    const QColor red(0xf5, 0x22, 0x2d, 255);
    const QColor redAccent(0xf4, 0x21, 0x2c, 255);
    const QColor transparent(0xff, 0xff, 0xff, 0);
    const auto exact = [](double left, double right) {
        return snowCanvasExactDoubleEqual(left, right);
    };

    require(defaults.rectangle.fill == transparent &&
                defaults.rectangle.fillStyle == SnowCanvasFillStyle::Solid &&
                defaults.rectangle.stroke == red && exact(defaults.rectangle.strokeWidth, 2.0) &&
                defaults.rectangle.strokeStyle == SnowCanvasStrokeStyle::Solid &&
                defaults.rectangle.cornerRadii == SnowCanvasCornerRadii{6.0, 6.0, 6.0, 6.0},
            "rectangle defaults should match the Snow Shot product profile");
    require(defaults.arrow.stroke == red && exact(defaults.arrow.strokeWidth, 2.0) &&
                defaults.arrow.startArrowhead == SnowCanvasArrowhead::None &&
                defaults.arrow.endArrowhead == SnowCanvasArrowhead::Arrow &&
                defaults.arrow.strokeStyle == SnowCanvasStrokeStyle::Solid &&
                defaults.arrow.arrowType == SnowCanvasArrowType::Curve,
            "arrow defaults should match the Snow Shot product profile");

    struct ShapeExpectation {
        const SnowCanvasShapeStyle* style;
        QColor fill;
        QColor stroke;
        double strokeWidth;
        double opacity;
        SnowCanvasArrowType arrowType;
        SnowCanvasHighlightShape highlightShape;
        const char* message;
    };
    const ShapeExpectation shapes[] = {
        {&defaults.line, transparent, red, 2.0, 1.0, SnowCanvasArrowType::Curve,
         SnowCanvasHighlightShape::Rectangle,
         "line defaults should match the Snow Shot product profile"},
        {&defaults.freeDraw, transparent, red, 2.0, 1.0, SnowCanvasArrowType::Straight,
         SnowCanvasHighlightShape::Rectangle,
         "free-draw defaults should match the Snow Shot product profile"},
        {&defaults.rectangleHighlight, red, redAccent, 0.0, 1.0, SnowCanvasArrowType::Straight,
         SnowCanvasHighlightShape::Rectangle,
         "rectangle-highlight defaults should match the Snow Shot product profile"},
        {&defaults.penHighlight, transparent, red, 30.0, 1.0, SnowCanvasArrowType::Straight,
         SnowCanvasHighlightShape::Rectangle,
         "pen-highlight defaults should match the Snow Shot product profile"},
    };
    for (const ShapeExpectation& expected : shapes) {
        require(expected.style->fill == expected.fill &&
                    expected.style->fillStyle == SnowCanvasFillStyle::Solid &&
                    expected.style->stroke == expected.stroke &&
                    exact(expected.style->strokeWidth, expected.strokeWidth) &&
                    expected.style->strokeStyle == SnowCanvasStrokeStyle::Solid &&
                    expected.style->startArrowhead == SnowCanvasArrowhead::None &&
                    expected.style->endArrowhead == SnowCanvasArrowhead::None &&
                    expected.style->arrowType == expected.arrowType &&
                    exact(expected.style->opacity, expected.opacity) &&
                    expected.style->highlightShape == expected.highlightShape,
                expected.message);
    }

    require(defaults.rectangleFilter.type == SnowCanvasFilterType::Mosaic &&
                exact(defaults.rectangleFilter.strength, 0.5) &&
                exact(defaults.rectangleFilter.opacity, 1.0) &&
                exact(defaults.rectangleFilter.strokeWidth, 2.0) &&
                defaults.penFilter.type == SnowCanvasFilterType::Mosaic &&
                exact(defaults.penFilter.strength, 0.5) && exact(defaults.penFilter.opacity, 1.0) &&
                exact(defaults.penFilter.strokeWidth, 30.0),
            "filter defaults should match the Snow Shot product profile");
    require(defaults.text.color == red && exact(defaults.text.fontSize, 30.0) &&
                defaults.text.fontFamily.isEmpty() && defaults.text.fill == transparent &&
                defaults.text.fillStyle == SnowCanvasFillStyle::Solid &&
                defaults.text.stroke == QColor(0xff, 0xcc, 0xc7, 255) &&
                exact(defaults.text.strokeWidth, 0.0) &&
                defaults.text.cornerRadii == SnowCanvasCornerRadii{6.0, 6.0, 6.0, 6.0} &&
                defaults.text.horizontalAlign == SnowCanvasTextHorizontalAlign::Left &&
                defaults.text.verticalAlign == SnowCanvasTextVerticalAlign::Center &&
                exact(defaults.text.opacity, 1.0),
            "text defaults should match the Snow Shot product profile");
    require(defaults.serialNumber.number == 1 &&
                defaults.serialNumber.type == SnowCanvasSerialNumberType::OutlinedCircle &&
                defaults.serialNumber.color == red && defaults.serialNumber.fill == transparent &&
                defaults.serialNumber.fillStyle == SnowCanvasFillStyle::Solid &&
                exact(defaults.serialNumber.fontSize, 24.0) &&
                defaults.serialNumber.fontFamily.isEmpty() &&
                exact(defaults.serialNumber.strokeWidth, 2.0) &&
                defaults.serialNumber.strokeStyle == SnowCanvasStrokeStyle::Solid &&
                exact(defaults.serialNumber.opacity, 1.0),
            "sequence-number defaults should match the Snow Shot product profile");
    require(defaults.watermark.color == QColor(0, 0, 0, 255) && defaults.watermark.text.isEmpty() &&
                defaults.watermark.templateValue.isEmpty() &&
                !defaults.watermark.templateApplicationTime.has_value() &&
                exact(defaults.watermark.fontSize, 16.0) &&
                defaults.watermark.fontFamily.isEmpty() && exact(defaults.watermark.angle, 30.0) &&
                exact(defaults.watermark.gap, 56.0) && exact(defaults.watermark.opacity, 0.16),
            "watermark defaults should match the Snow Shot product profile");
    require(defaults.spotlight.color == QColor(0, 0, 0, 255) &&
                exact(defaults.spotlight.opacity, 0.64),
            "spotlight defaults should match the Snow Shot product profile");
}

void resetStyleStateRestoresTheCompleteInjectedProfileWithoutCommands() {
    ScreenshotToolPalette::Options options;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showRectangleHighlightTool = true;
    options.showPenHighlightTool = true;
    options.showFilterTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showWatermarkTool = true;
    options.showSpotlightTool = true;
    options.styleDefaults = snow_shot::presentation::screenshotCanvasStyleDefaults();
    ScreenshotToolPalette palette(options);

    int editCommands = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged,
                     [&editCommands](const SnowCanvasShapeStyle&, quint32, SnowCanvasShapeKind) {
                         ++editCommands;
                     });
    QObject::connect(&palette, &ScreenshotToolPalette::filterStyleChanged,
                     [&editCommands](const SnowCanvasFilterStyle&, quint32) { ++editCommands; });
    QObject::connect(&palette, &ScreenshotToolPalette::textStyleChanged,
                     [&editCommands](const SnowCanvasTextStyle&) { ++editCommands; });
    QObject::connect(&palette, &ScreenshotToolPalette::serialNumberStyleChanged,
                     [&editCommands](const SnowCanvasSerialNumberStyle&) { ++editCommands; });
    QObject::connect(&palette, &ScreenshotToolPalette::watermarkConfigChanged,
                     [&editCommands](const SnowCanvasWatermarkConfig&) { ++editCommands; });
    QObject::connect(&palette, &ScreenshotToolPalette::spotlightConfigChanged,
                     [&editCommands](const SnowCanvasSpotlightConfig&) { ++editCommands; });

    struct ShapeMutation {
        SnowCanvasStyleToolbarSource source;
        SnowCanvasShapeStyle style;
    };
    const SnowCanvasStyleDefaults& expected = options.styleDefaults;
    const ShapeMutation shapes[] = {
        {SnowCanvasStyleToolbarSource::DefaultRectangle, expected.rectangle},
        {SnowCanvasStyleToolbarSource::DefaultArrow, expected.arrow},
        {SnowCanvasStyleToolbarSource::DefaultLine, expected.line},
        {SnowCanvasStyleToolbarSource::DefaultFreeDraw, expected.freeDraw},
        {SnowCanvasStyleToolbarSource::DefaultRectangleHighlight, expected.rectangleHighlight},
        {SnowCanvasStyleToolbarSource::DefaultPenHighlight, expected.penHighlight},
    };
    double changedWidth = 11.0;
    for (ShapeMutation mutation : shapes) {
        mutation.style.strokeWidth = changedWidth;
        SnowCanvasStyleToolbarState state;
        state.source = mutation.source;
        state.shapeStyle = mutation.style;
        palette.setStyleToolbarState(state);
        changedWidth += 1.0;
    }

    SnowCanvasStyleToolbarState filterState;
    filterState.source = SnowCanvasStyleToolbarSource::DefaultRectangleFilter;
    filterState.filterStyle = expected.rectangleFilter;
    filterState.filterStyle.strength = 0.27;
    palette.setStyleToolbarState(filterState);
    filterState.source = SnowCanvasStyleToolbarSource::DefaultPenFilter;
    filterState.filterStyle = expected.penFilter;
    filterState.filterStyle.strokeWidth = 44.0;
    palette.setStyleToolbarState(filterState);

    SnowCanvasStyleToolbarState textState;
    textState.source = SnowCanvasStyleToolbarSource::DefaultText;
    textState.textStyle = expected.text;
    textState.textStyle.fontSize = 48.0;
    palette.setStyleToolbarState(textState);
    SnowCanvasStyleToolbarState serialState;
    serialState.source = SnowCanvasStyleToolbarSource::DefaultSerialNumber;
    serialState.serialNumberStyle = expected.serialNumber;
    serialState.serialNumberStyle.number = 9;
    palette.setStyleToolbarState(serialState);

    SnowCanvasWatermarkConfig watermark = expected.watermark;
    watermark.text = QStringLiteral("changed");
    palette.setWatermarkConfig(watermark);
    SnowCanvasSpotlightConfig spotlight = expected.spotlight;
    spotlight.opacity = 0.19;
    palette.setSpotlightConfig(spotlight);
    require(palette.styleStateForTests() != expected,
            "the palette should contain modified state before a new-capture reset");

    palette.resetStyleState();
    require(palette.styleStateForTests() == expected,
            "new-capture reset should restore every value from the injected profile");
    require(editCommands == 0, "state synchronization and reset must not emit user edit commands");
}

void selectToolRemainsTheSoleOwnerOfItsSecondaryToolbar() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showFilterTool = true;
    options.showWatermarkTool = true;
    ScreenshotToolPalette palette(options);

    SnowCanvasStyleToolbarState penFilterState;
    penFilterState.source = SnowCanvasStyleToolbarSource::DefaultPenFilter;
    penFilterState.filterStyle.type = SnowCanvasFilterType::Mosaic;
    penFilterState.filterStyle.strength = 0.5;
    penFilterState.filterStyle.opacity = 1.0;
    penFilterState.filterStyle.strokeWidth = 42.0;
    palette.setActiveTool(ScreenshotToolPalette::Tool::PenFilter);
    palette.setStyleToolbarState(penFilterState);
    require(palette.styleToolbarVisible() && !palette.actionToolbarVisible(),
            "Pen Filter should initially own the style toolbar");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    penFilterState.source = SnowCanvasStyleToolbarSource::SelectedPenFilter;
    penFilterState.filterStyle.opacity = 0.65;
    palette.setStyleToolbarState(penFilterState);
    require(palette.actionToolbarVisible() && !palette.styleToolbarVisible(),
            "selecting a pen-filter element must preserve the selection toolbar");
    require(!palette.actionPanel()->isHidden() && palette.stylePanel()->isHidden(),
            "the selected pen-filter style event must not replace the visible selection panel");
    auto* selectionOpacity = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotSelectionOpacitySlider"));
    require(selectionOpacity != nullptr && selectionOpacity->value() == 65,
            "pen-filter selection state should still synchronize selection toolbar values");

    const SnowCanvasStyleToolbarSource staleSources[] = {
        SnowCanvasStyleToolbarSource::SelectedRectangle,
        SnowCanvasStyleToolbarSource::SelectedText,
        SnowCanvasStyleToolbarSource::SelectedSpotlight,
        SnowCanvasStyleToolbarSource::SelectedRectangleFilter,
        SnowCanvasStyleToolbarSource::Watermark,
        SnowCanvasStyleToolbarSource::Eraser,
    };
    for (SnowCanvasStyleToolbarSource source : staleSources) {
        SnowCanvasStyleToolbarState state;
        state.source = source;
        palette.setStyleToolbarState(state);
        require(palette.actionToolbarVisible() && !palette.styleToolbarVisible(),
                "canvas style state must not override the Select tool's toolbar mode");
    }

    palette.setStyleToolbarVisible(true);
    require(palette.actionToolbarVisible() && !palette.styleToolbarVisible(),
            "a direct style-visibility request must not expose styles while Select is active");
}

void tableQrEntrySelectionPersistsAcrossPaletteInstances() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showTableTool = true;
    options.showQrTool = true;
    options.enableStyleToolbar = false;

    {
        ScreenshotToolPalette palette(options);
        auto* tableQrTrigger =
            palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableQrButton"));
        materializeLazyPopover(tableQrTrigger);
        auto* qrOption =
            popoverButtonWithTooltip(popoverForTrigger(tableQrTrigger), "Barcode recognition");
        require(qrOption != nullptr, "persisted recognition test should expose the QR option");
        qrOption->click();
    }

    ScreenshotToolPalette restored(options);
    auto* restoredTableQr =
        restored.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableQrButton"));
    require(restoredTableQr != nullptr &&
                restoredTableQr->accessibleName() == QStringLiteral("Barcode recognition"),
            "new toolbar instances should restore the persisted recognition entry");
    require(snow_shot::storage::ApplicationStorage::instance().flushNow().success,
            "toolbar entry preferences should flush to the configuration file");
}

void colorPickerChannelKeyboardInput() {
    using Picker = adqt::widgets::AdColorPicker;
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    palette.show();
    auto* picker = colorPickerWithAccessibleName(palette, "Stroke color");
    require(picker != nullptr, "stroke picker must exist");
    picker->setPopupLayerMode(Picker::PopupLayerMode::QtTool);
    picker->setTrigger(Picker::Trigger::Click);
    picker->setPopupVisible(true);
    for (const auto format : {Picker::Format::Hsb, Picker::Format::Rgb, Picker::Format::Hex}) {
        picker->setFormat(format);
        picker->setValue(adqt::widgets::AdColorValue::solid(QColor(60, 120, 180, 128)));
        QApplication::processEvents();
        auto* content = picker->findChild<adqt::widgets::AdPopover*>()->contentWidget();
        const auto editors = content->findChildren<QLineEdit*>();
        int edited = 0;
        for (auto* editor : editors) {
            if (!editor->isVisible() || editor->isReadOnly()) {
                continue;
            }
            ++edited;
            editor->setFocus();
            auto* receiver = QApplication::focusWidget();
            require(receiver != nullptr, "color input must acquire keyboard focus");
            const QColor before = picker->value().solidColor;
            editor->selectAll();
            QKeyEvent erase(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
            QApplication::sendEvent(receiver, &erase);
            require(picker->value().solidColor == before,
                    "clearing a channel while typing must not replace it with zero");
            const bool hex = editor->objectName() == QStringLiteral("ad-color-picker-hex-input");
            const QString text = hex ? QStringLiteral("12345680") : QStringLiteral("42");
            for (const QChar character : text) {
                QKeyEvent key(QEvent::KeyPress, character.unicode(), Qt::NoModifier,
                              QString(character));
                QApplication::sendEvent(receiver, &key);
                QApplication::processEvents();
            }
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(receiver, &enter);
            QApplication::processEvents();
            if (auto* number = qobject_cast<adqt::widgets::AdInputNumber*>(receiver)) {
                require(number->hasValue() && number->value() == 42,
                        "numeric channels must commit the complete typed value");
                QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
                QApplication::sendEvent(receiver, &up);
                require(number->value() == 43, "Up must step the active numeric channel");
                QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
                QApplication::sendEvent(receiver, &down);
                require(number->value() == 42, "Down must step the active numeric channel");
                editor->selectAll();
                QKeyEvent replacement(QEvent::KeyPress, Qt::Key_5, Qt::NoModifier,
                                      QStringLiteral("51"));
                QApplication::sendEvent(receiver, &replacement);
                QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
                QApplication::sendEvent(receiver, &tab);
                QApplication::processEvents();
                require(number->value() == 51, "Tab must commit pending text before moving focus");
                editor->setFocus();
                QKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
                QApplication::sendEvent(receiver, &selectAll);
                QApplication::clipboard()->setText(QStringLiteral("24"));
                QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
                QApplication::sendEvent(receiver, &paste);
                QKeyEvent keypadEnter(QEvent::KeyPress, Qt::Key_Enter, Qt::KeypadModifier);
                QApplication::sendEvent(receiver, &keypadEnter);
                require(number->value() == 24,
                        "Select All, paste and keypad Enter must work in every numeric channel");
            } else if (hex) {
                require(picker->value().solidColor.rgba() == QColor(0x12, 0x34, 0x56, 0x80).rgba(),
                        "HEX must accept all eight RGBA digits without overwriting partial input");
            }
            require(picker->value().solidColor.isValid(), "editing must keep a valid color");
        }
        require(edited == (format == Picker::Format::Hex ? 2 : 4),
                "each format must expose all channels and alpha for keyboard editing");
    }
    picker->setPopupVisible(false);
}

void drawingColorsPreserveAlphaAcrossEditsAndToolSwitches() {
    using Tool = ScreenshotToolPalette::Tool;
    struct ColorCase {
        Tool tool;
        const char* name;
        QColor& (*color)(SnowCanvasStyleDefaults&);
    };
    const ColorCase cases[]{
        {Tool::Shape, "Stroke color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.rectangle.stroke; }},
        {Tool::Shape, "Fill color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.rectangle.fill; }},
        {Tool::Line, "Stroke color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.line.stroke; }},
        {Tool::FreeDraw, "Stroke color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.freeDraw.stroke; }},
        {Tool::Arrow, "Arrow stroke color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.arrow.stroke; }},
        {Tool::RectangleHighlight, "Highlight color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.rectangleHighlight.fill; }},
        {Tool::RectangleHighlight, "Highlight stroke width",
         [](SnowCanvasStyleDefaults& styles) -> QColor& {
             return styles.rectangleHighlight.stroke;
         }},
        {Tool::PenHighlight, "Pen highlight color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.penHighlight.stroke; }},
        {Tool::Text, "Text color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.text.color; }},
        {Tool::Text, "Text stroke width",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.text.stroke; }},
        {Tool::Text, "Text fill color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.text.fill; }},
        {Tool::SerialNumber, "Sequence number color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.serialNumber.color; }},
        {Tool::SerialNumber, "Sequence number fill color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.serialNumber.fill; }},
        {Tool::Spotlight, "Mask color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.spotlight.color; }},
        {Tool::Watermark, "Watermark color",
         [](SnowCanvasStyleDefaults& styles) -> QColor& { return styles.watermark.color; }},
    };
    for (const auto& test : cases) {
        ScreenshotToolPalette::Options options;
        options.showTextTool = true;
        options.showSerialNumberTool = true;
        test.color(options.styleDefaults) = QColor(23, 67, 109, 0);
        ScreenshotToolPalette palette(options);
        palette.setActiveTool(test.tool);
        auto* picker = colorPickerWithAccessibleName(palette, test.name);
        require(picker != nullptr && picker->alphaChannelEnabled(),
                "every drawing color picker should enable alpha");
        require(picker->value().solidColor == test.color(options.styleDefaults),
                "loading a fully transparent color must retain RGB and alpha");
        for (const int alpha : {128, 255, 0}) {
            auto expected = palette.creationStyleDefaults();
            const QColor color(23, 67, 109, alpha);
            test.color(expected) = color;
            picker->commitValue(adqt::widgets::AdColorValue::solid(color));
            require(palette.creationStyleDefaults() == expected,
                    "alpha-only edits must update only the chosen color");
            palette.setActiveTool(test.tool == Tool::Shape ? Tool::Text : Tool::Shape);
            palette.setActiveTool(test.tool);
            picker = colorPickerWithAccessibleName(palette, test.name);
            require(picker != nullptr && picker->value().solidColor == color,
                    "tool switching must preserve the complete edited color");
        }
    }
}

void canvasToolStylesPersistIndependentlyWithoutGlobalStyles() {
    SnowCanvasStyleDefaults styles = snow_shot::presentation::screenshotCanvasStyleDefaults();
    styles.rectangle.stroke = QColor(1, 2, 3, 4);
    styles.rectangle.strokeWidth = 3.0;
    styles.arrow.stroke = QColor(5, 6, 7, 8);
    styles.arrow.strokeWidth = 4.0;
    styles.arrow.arrowRatio = 2.3;
    styles.arrow.startArrowhead = SnowCanvasArrowhead::IndentedTriangle;
    styles.arrow.endArrowhead = SnowCanvasArrowhead::IndentedTriangle;
    styles.line.strokeWidth = 5.0;
    styles.line.arrowType = SnowCanvasArrowType::Straight;
    styles.freeDraw.strokeWidth = 6.0;
    styles.rectangleHighlight.fill = QColor(9, 10, 11, 12);
    styles.penHighlight.strokeWidth = 7.0;
    styles.rectangleFilter = {SnowCanvasFilterType::GaussianBlur, 0.25, 0.8, 8.0};
    styles.penFilter = {SnowCanvasFilterType::Emboss, 0.75, 0.6, 44.0};
    styles.text.color = QColor(13, 14, 15, 16);
    styles.text.fontFamily = QStringLiteral("Persisted text font");
    styles.text.fontSize = 36.0;
    styles.serialNumber.number = 9'007'199'254'740'993LL;
    styles.serialNumber.type = SnowCanvasSerialNumberType::Circle;
    styles.serialNumber.color = QColor(17, 18, 19, 20);
    styles.serialNumber.fontFamily = QStringLiteral("Persisted serial font");
    styles.watermark.text = QStringLiteral("must not persist");
    styles.watermark.templateValue = QStringLiteral("{text}-{YYYY}");
    styles.watermark.templateApplicationTime =
        SnowCanvasWatermarkTemplateApplicationTime{2026, 9, 15, 12, 34, 56};
    styles.watermark.color = QColor(25, 26, 27, 28);
    styles.watermark.fontSize = 42.0;
    styles.watermark.fontFamily = QStringLiteral("Persisted watermark font");
    styles.watermark.angle = -35.0;
    styles.watermark.gap = 88.0;
    styles.watermark.opacity = 0.91;
    styles.spotlight.color = QColor(21, 22, 23, 24);
    styles.spotlight.opacity = 0.17;

    require(snow_shot::presentation::persistScreenshotCanvasToolStyles(styles),
            "canvas tool styles should be accepted by configuration storage");

    SnowCanvasStyleDefaults expected = styles;
    expected.penFilter.strength = styles.rectangleFilter.strength;
    const SnowCanvasStyleDefaults globalDefaults =
        snow_shot::presentation::screenshotCanvasStyleDefaults();
    expected.watermark.text = globalDefaults.watermark.text;
    expected.watermark.templateValue = globalDefaults.watermark.templateValue;
    expected.watermark.templateApplicationTime = globalDefaults.watermark.templateApplicationTime;
    expected.serialNumber.number = globalDefaults.serialNumber.number;
    require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() == expected,
            "persisted tool styles should round-trip independently without global styles");

    const auto configuration =
        snow_shot::storage::ApplicationStorage::instance().configuration().snapshot();
    const QJsonObject savedWatermarkStyle =
        configuration.value(QStringLiteral("drawing/watermark_style")).toObject();
    require(!savedWatermarkStyle.isEmpty() &&
                !savedWatermarkStyle.contains(QStringLiteral("text")) &&
                !savedWatermarkStyle.contains(QStringLiteral("template_value")) &&
                !savedWatermarkStyle.contains(QStringLiteral("template_application_time")),
            "watermark appearance should persist without text or template session state");
    require(!configuration.value(QStringLiteral("drawing/spotlight_style")).toObject().isEmpty(),
            "spotlight mask color and opacity should persist");
    const QString lineKey = QStringLiteral("drawing/line_style");
    const QJsonObject savedLineStyle = configuration.value(lineKey).toObject();
    require(!savedLineStyle.contains(QStringLiteral("arrow_type")) &&
                savedLineStyle.value(QStringLiteral("line_type")).toInt(-1) ==
                    static_cast<int>(SnowCanvasArrowType::Straight),
            "Line should persist its type under the new line-specific field");

    QJsonObject legacyLineStyle = savedLineStyle;
    legacyLineStyle.remove(QStringLiteral("line_type"));
    legacyLineStyle.insert(QStringLiteral("arrow_type"),
                           static_cast<int>(SnowCanvasArrowType::Straight));
    require(snow_shot::storage::ApplicationStorage::instance().configuration().setValue(
                lineKey, legacyLineStyle),
            "legacy Line settings should be accepted for the compatibility test");
    SnowCanvasStyleDefaults migratedLineExpected = expected;
    migratedLineExpected.line.arrowType = SnowCanvasArrowType::Curve;
    require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() == migratedLineExpected,
            "legacy Line arrow_type should be ignored and migrate to Curve");

    for (const QJsonValue& invalidType : {QJsonValue(1.5), QJsonValue(-1), QJsonValue(2),
                                          QJsonValue(3), QJsonValue(QStringLiteral("1"))}) {
        QJsonObject invalidLineStyle = savedLineStyle;
        invalidLineStyle.insert(QStringLiteral("line_type"), invalidType);
        require(snow_shot::storage::ApplicationStorage::instance().configuration().setValue(
                    lineKey, invalidLineStyle),
                "invalid Line type settings should be accepted for compatibility tests");
        require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() ==
                    migratedLineExpected,
                "malformed and Elbow Line types should normalize to Curve");
    }
    require(snow_shot::storage::ApplicationStorage::instance().configuration().setValue(
                lineKey, savedLineStyle),
            "the valid Line type should be restored for the remaining persistence test");
    require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() == expected,
            "the restored Line type should round-trip independently");

    const QString arrowKey = QStringLiteral("drawing/arrow_style");
    const QJsonObject savedArrowStyle = configuration.value(arrowKey).toObject();
    for (const QJsonValue& ratio :
         {QJsonValue(), QJsonValue(-1.0), QJsonValue(4.0), QJsonValue(QStringLiteral("bad"))}) {
        auto legacyArrow = savedArrowStyle;
        if (ratio.isNull())
            legacyArrow.remove(QStringLiteral("arrow_ratio"));
        else
            legacyArrow.insert(QStringLiteral("arrow_ratio"), ratio);
        require(snow_shot::storage::ApplicationStorage::instance().configuration().setValue(
                    arrowKey, legacyArrow),
                "save legacy or invalid ratio fixture");
        require(snow_shot::presentation::screenshotCanvasToolStyleDefaults().arrow.arrowRatio ==
                    (ratio.toDouble() == 4.0 ? 3.0 : 1.0),
                "missing or invalid saved ratios normalize safely");
    }
    require(snow_shot::storage::ApplicationStorage::instance().configuration().setValue(
                arrowKey, savedArrowStyle),
            "restore arrow settings");

    const QString serialKey = QStringLiteral("drawing/serial_number_style");
    QJsonObject savedSerialStyle = configuration.value(serialKey).toObject();
    require(!savedSerialStyle.contains(QStringLiteral("number")),
            "the current serial number must not be saved with its appearance");
    require(savedSerialStyle.value(QStringLiteral("type")).toInt(-1) ==
                static_cast<int>(SnowCanvasSerialNumberType::Circle),
            "the last sequence-number type should persist with its appearance");

    QJsonObject legacySerialStyle = savedSerialStyle;
    legacySerialStyle.remove(QStringLiteral("type"));
    legacySerialStyle.insert(QStringLiteral("number"), styles.serialNumber.number);
    require(snow_shot::storage::ApplicationStorage::instance().configuration().setValue(
                serialKey, legacySerialStyle),
            "legacy serial-number settings should be accepted for the compatibility test");
    SnowCanvasStyleDefaults legacyExpected = expected;
    legacyExpected.serialNumber.type = SnowCanvasSerialNumberType::OutlinedCircle;
    require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() == legacyExpected,
            "legacy settings without a type should use outlined circle and ignore saved numbers");

    for (const QJsonValue& invalidType :
         {QJsonValue(1.5), QJsonValue(-1), QJsonValue(5), QJsonValue(QStringLiteral("3"))}) {
        QJsonObject invalidSerialStyle = savedSerialStyle;
        invalidSerialStyle.insert(QStringLiteral("type"), invalidType);
        require(snow_shot::storage::ApplicationStorage::instance().configuration().setValue(
                    serialKey, invalidSerialStyle),
                "invalid serial-number type settings should be accepted for compatibility tests");
        require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() == legacyExpected,
                "non-integral, out-of-range, and non-numeric types should use outlined circle");
    }

    savedSerialStyle.insert(QStringLiteral("number"), styles.serialNumber.number);
    require(snow_shot::storage::ApplicationStorage::instance().configuration().setValue(
                serialKey, savedSerialStyle),
            "typed serial-number settings should be restored for the remaining test");
    require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() == expected,
            "typed serial-number settings should restore the saved appearance");

    ScreenshotToolPalette::Options options;
    options.showSerialNumberTool = true;
    options.showTextTool = true;
    options.styleDefaults = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::DefaultSerialNumber;
    state.serialNumberStyle = styles.serialNumber;
    palette.setStyleToolbarState(state);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    require(palette.creationStyleDefaults().serialNumber == styles.serialNumber,
            "switching tools should retain the current session's serial number");
    require(
        snow_shot::presentation::persistScreenshotCanvasToolStyles(palette.creationStyleDefaults()),
        "the current session's appearance should save successfully");
    require(palette.creationStyleDefaults().serialNumber == styles.serialNumber,
            "saving appearance must not reset the current session's serial number");
    require(!snow_shot::storage::ApplicationStorage::instance()
                 .configuration()
                 .value(serialKey)
                 .toObject()
                 .contains(QStringLiteral("number")),
            "saving appearance should remove the legacy number field");

    palette.resetStyleState();
    palette.setCreationStyleDefaults(snow_shot::presentation::screenshotCanvasToolStyleDefaults());
    require(palette.creationStyleDefaults().serialNumber == expected.serialNumber,
            "a new capture should start at one and retain the saved appearance");
    options.styleDefaults = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
    ScreenshotToolPalette newEditor(options);
    require(newEditor.creationStyleDefaults().serialNumber == expected.serialNumber,
            "a new editor should start at one and retain the saved appearance");
}

void colorPresetEditorsPreserveCommandsAcrossRebinding() {
    using namespace snow_shot::presentation;
    const QVector<QColor> colors{QColor(Qt::transparent), QColor(210, 40, 70, 128)};
    const ScreenshotToolPaletteButtonMetrics metrics{28, 18, 1.0};
    for (bool fill : {false, true}) {
        QWidget host;
        QHBoxLayout layout(&host);
        ScreenshotToolPaletteStrokeEditor strokeEditor;
        ScreenshotToolPaletteFillEditor fillEditor;
        ScreenshotToolPaletteStrokeEditorConfig strokeConfig;
        ScreenshotToolPaletteFillEditorConfig fillConfig;
        strokeConfig.colorValues = colors;
        fillConfig.colorValues = colors;
        int oldCommands = 0;
        int newCommands = 0;
        QColor committed;
        const auto original = [&](const QColor&) { ++oldCommands; };
        const auto rebound = [&](const QColor& color) {
            ++newCommands;
            committed = color;
        };
        if (fill) {
            fillEditor.build(&layout, &host, &host, fillConfig, colors.last(),
                             SnowCanvasFillStyle::Solid, original, {}, {}, metrics);
            fillEditor.rebind(fillConfig, rebound, {});
            fillEditor.update(colors.last(), SnowCanvasFillStyle::Solid, true, false);
        } else {
            strokeEditor.build(&layout, &host, &host, strokeConfig, colors.last(),
                               SnowCanvasStrokeStyle::Solid, original, {}, {}, metrics);
            strokeEditor.rebind(strokeConfig, rebound, {});
            strokeEditor.update(colors.last(), SnowCanvasStrokeStyle::Solid, true, false);
        }
        require(oldCommands == 0 && newCommands == 0,
                "inbound color state and rebinding must not emit edit commands");
        auto* picker = host.findChild<adqt::widgets::AdColorPicker*>();
        require(picker != nullptr, "color editor must expose its picker");
        if (fill) {
            picker->setPopupVisible(true);
            QCoreApplication::processEvents();
        }
        QWidget* presetHost = fill ? picker->popupContent() : strokeEditor.rootWidget();
        require(presetHost != nullptr, "color presets must materialize before inspection");
        QList<adqt::widgets::AdButton*> presets;
        for (auto* button : presetHost->findChildren<adqt::widgets::AdButton*>()) {
            if (button->toolTip() == colors.first().name() ||
                button->toolTip() == colors.last().name()) {
                presets.append(button);
            }
        }
        require(presets.size() == colors.size(), "each color must have one preset");
        for (int index = 0; index < presets.size(); ++index) {
            auto* button = presets.at(index);
            require(button->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text,
                    "mixed colors must clear every preset's selected style");
            button->click();
            require(oldCommands == 0 && newCommands == index + 1 && committed == colors.at(index),
                    "presets must emit exactly one rebound command, preserving transparency");
        }
        picker->setPopupVisible(false);
    }
}

void fontFamilyListIsCachedForEditorBuilds() {
    const QStringList& first = snow_shot::presentation::screenshotToolPaletteFontFamilies();
    const QStringList& second = snow_shot::presentation::screenshotToolPaletteFontFamilies();
    require(&first == &second, "font family enumeration should be cached across editor builds");
    require(first.contains(QStringLiteral("Segoe UI")),
            "font family cache should expose the registered application font");

    QStringList expected;
    const QStringList systemFamilies = QFontDatabase::families();
    expected.reserve(systemFamilies.size());
    for (const QString& family : systemFamilies) {
        const QString trimmed = family.trimmed();
        if (!trimmed.isEmpty()) {
            expected.append(trimmed);
        }
    }
    expected.removeDuplicates();
    expected.sort(Qt::CaseInsensitive);
    require(first == expected, "font family cache should match the normalized system families");
}
void autoFilterLegacyStrengthMigration() {
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    const QString rectangleKey = QStringLiteral("drawing/rectangle_filter_style");
    const QString penKey = QStringLiteral("drawing/pen_filter_style");
    const auto rectangle = configuration.value(rectangleKey);
    const auto pen = configuration.value(penKey);
    const auto check = [&](QJsonValue rectangleStrength, QJsonValue penStrength, double expected) {
        require(configuration.setValue(
                    rectangleKey, QJsonObject{{QStringLiteral("strength"), rectangleStrength}}),
                "save legacy rectangle strength");
        require(
            configuration.setValue(penKey, QJsonObject{{QStringLiteral("strength"), penStrength}}),
            "save legacy pen strength");
        const auto defaults = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
        require(defaults.rectangleFilter.strength == expected &&
                    defaults.penFilter.strength == expected,
                "legacy strengths normalize to one valid value");
    };
    check(0.2, 0.8, 0.2);
    check(QJsonValue(), 0.8, 0.8);
    check(5.0, 0.8, 0.8);
    check(QJsonValue(), -1.0,
          snow_shot::presentation::screenshotCanvasStyleDefaults().rectangleFilter.strength);
    require(configuration.setValue(rectangleKey, rectangle) && configuration.setValue(penKey, pen),
            "restore saved filter settings");
}

void filterEditorsRestoreValuesAfterToolSwitch() {
    using Tool = ScreenshotToolPalette::Tool;
    for (const Tool tool : {Tool::RectangleFilter, Tool::PenFilter, Tool::AutoFilter}) {
        ScreenshotToolPalette::Options options;
        options.showFilterTool = true;
        ScreenshotToolPalette palette(options);
        palette.setActiveTool(tool);
        SnowCanvasStyleToolbarState state;
        state.source = tool == Tool::PenFilter
                           ? SnowCanvasStyleToolbarSource::DefaultPenFilter
                           : SnowCanvasStyleToolbarSource::DefaultRectangleFilter;
        int edits = 0;
        QObject::connect(&palette, &ScreenshotToolPalette::filterStyleChanged, [&]() { ++edits; });
        for (const auto type : {SnowCanvasFilterType::Mosaic, SnowCanvasFilterType::GaussianBlur,
                                SnowCanvasFilterType::Grayscale, SnowCanvasFilterType::Inversion,
                                SnowCanvasFilterType::Emboss}) {
            state.filterStyle.type = type;
            state.filterStyle.strength = 0.37;
            palette.setStyleToolbarState(state);
            for (const Tool other : {Tool::Shape, Tool::Move, Tool::Text}) {
                palette.setActiveTool(other);
                palette.setActiveTool(tool);
                palette.setStyleToolbarState(state);
                const QString prefix =
                    tool == Tool::PenFilter    ? QStringLiteral("screenshotPenFilter")
                    : tool == Tool::AutoFilter ? QStringLiteral("screenshotAutoFilter")
                                               : QStringLiteral("screenshotFilter");
                auto* select = palette.findChild<adqt::widgets::AdSelect*>(
                    prefix + QStringLiteral("TypeSelect"));
                auto* slider = palette.findChild<adqt::widgets::AdSlider*>(
                    prefix + QStringLiteral("IntensitySlider"));
                require(select && select->currentValue().isValid() &&
                            select->currentValue().toInt() == static_cast<int>(type),
                        "returning to a filter tool restores the unchanged filter type");
                require(slider && slider->value() == 37,
                        "returning to a filter tool restores the unchanged intensity");
                if (type == SnowCanvasFilterType::Emboss) {
                    require(slider->isEnabled(),
                            "Emboss intensity remains enabled for every filter tool mode");
                }
            }
        }
        require(edits == 0, "restoring filter controls must not emit style edits");
    }
}

void selectedFilterTypeDoesNotReplaceCreationDefault() {
    ScreenshotToolPalette::Options options;
    options.showFilterTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::RectangleFilter);
    const auto before = palette.creationStyleDefaults().rectangleFilter;
    SnowCanvasStyleToolbarState selected;
    selected.source = SnowCanvasStyleToolbarSource::SelectedRectangleFilter;
    selected.filterStyle = before;
    selected.filterStyle.type = SnowCanvasFilterType::Inversion;
    selected.filterStyle.strength = 0.1;
    palette.setStyleToolbarState(selected);
    require(palette.creationStyleDefaults().rectangleFilter == before,
            "selecting an older filter preserves creation preferences");
    auto* type =
        palette.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenshotFilterTypeSelect"));
    require(type != nullptr, "selected filter exposes its type control");
    type->setCurrentValue(static_cast<int>(SnowCanvasFilterType::Grayscale));
    require(palette.creationStyleDefaults().rectangleFilter.type == before.type,
            "selected filter type edit preserves creation type");
}

void filterTypeSelectKeepsSmartEraseAcrossFilterModeSwitches() {
    using Tool = ScreenshotToolPalette::Tool;
    ScreenshotToolPalette::Options options;
    options.showFilterTool = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(Tool::AutoFilter);
    auto* autoType = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotAutoFilterTypeSelect"));
    require(autoType != nullptr && autoType->model() != nullptr &&
                autoType->model()->rowCount() == 5,
            "Auto Filter exposes its five filter types without Smart Erase");

    const auto requireSmartErase = [&palette](const QString& objectName) {
        auto* select = palette.findChild<adqt::widgets::AdSelect*>(objectName);
        require(select != nullptr && select->model() != nullptr && select->model()->rowCount() == 6,
                "leaving Auto Filter keeps all six filter types");
        const QModelIndex smartEraseRow = select->model()->index(2, 0);
        require(smartEraseRow.data(adqt::widgets::AdSelect::DefaultValueRole).toInt() ==
                    static_cast<int>(SnowCanvasFilterType::SmartErase),
                "leaving Auto Filter keeps the Smart Erase option");
        return select;
    };

    palette.setActiveTool(Tool::RectangleFilter);
    requireSmartErase(QStringLiteral("screenshotFilterTypeSelect"));

    palette.setActiveTool(Tool::AutoFilter);
    palette.setActiveTool(Tool::PenFilter);
    adqt::widgets::AdSelect* penType =
        requireSmartErase(QStringLiteral("screenshotPenFilterTypeSelect"));

    penType->setCurrentData(static_cast<int>(SnowCanvasFilterType::SmartErase),
                            adqt::widgets::AdSelect::DefaultValueRole);
    require(penType->currentValue().toInt() == static_cast<int>(SnowCanvasFilterType::SmartErase) &&
                palette.creationStyleDefaults().penFilter.type == SnowCanvasFilterType::SmartErase,
            "Smart Erase stays selectable and updates the pen filter creation type");

    palette.setActiveTool(Tool::AutoFilter);
    autoType = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotAutoFilterTypeSelect"));
    require(autoType != nullptr && autoType->model() != nullptr &&
                autoType->model()->rowCount() == 5,
            "returning to Auto Filter restores its Smart-Erase-free type model");
    palette.setActiveTool(Tool::RectangleFilter);
    requireSmartErase(QStringLiteral("screenshotFilterTypeSelect"));
}

void autoFilterControlsShareStylesAndKeepCategoryUnselected() {
    using Tool = ScreenshotToolPalette::Tool;
    ScreenshotToolPalette::Options options;
    options.showFilterTool = true;
    ScreenshotToolPalette palette(options);
    SnowCanvasWidget canvas;
    QObject::connect(&palette, &ScreenshotToolPalette::autoFilterRequested, &canvas,
                     [&]() { canvas.setCanvasTool(SnowCanvasTool::AutoFilter); });
    QObject::connect(&palette, &ScreenshotToolPalette::filterStyleChanged, &canvas,
                     [&](const SnowCanvasFilterStyle& style, quint32 properties) {
                         canvas.setCanvasFilterStyle(style, properties);
                     });
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&]() { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    for (const Tool tool :
         {Tool::AutoFilter, Tool::RectangleFilter, Tool::PenFilter, Tool::AutoFilter}) {
        palette.setActiveTool(tool);
        canvas.setCanvasTool(tool == Tool::AutoFilter  ? SnowCanvasTool::AutoFilter
                             : tool == Tool::PenFilter ? SnowCanvasTool::PenFilter
                                                       : SnowCanvasTool::RectangleFilter);
        palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
    }
    auto* controls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotAutoFilterStyleControls"));
    auto* category =
        palette.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenshotFillRegionsSelect"));
    auto* type = palette.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotAutoFilterTypeSelect"));
    auto* strength = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotAutoFilterIntensitySlider"));
    require(controls && category && type && strength,
            "Auto Filter owns a complete style row after repeated switches");
    require(!category->isEnabled() && category->model()->rowCount() == 7,
            "category actions start disabled and include all detector categories");
    palette.setAutoFilterAvailable(true);
    int categoryActions = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::autoFilterCategoryRequested,
                     [&](const QString& key) {
                         require(key == QStringLiteral("text"), "category action uses stable id");
                         ++categoryActions;
                     });
    category->setCurrentValue(QStringLiteral("text"));
    require(categoryActions == 1 && !category->currentValue().isValid(),
            "category activation returns to unselected placeholder");
    type->setCurrentValue(1);
    strength->setValue(37);
    require(canvas.canvasStyleToolbarState().filterStyle.type ==
                    SnowCanvasFilterType::GaussianBlur &&
                qAbs(canvas.canvasStyleToolbarState().filterStyle.strength - 0.37) < 0.001,
            "Auto Filter forwards type and strength edits");
    require(qAbs(palette.creationStyleDefaults().penFilter.strength - 0.37) < 0.001,
            "Auto Filter shares persistent pen strength");
    auto* modes = controls->findChild<adqt::widgets::AdRadioButtonGroup*>();
    require(modes && modes->buttons().size() == 3 &&
                modes->buttons()[1] == modes->button(static_cast<int>(Tool::RectangleFilter)) &&
                modes->buttons()[2] == modes->button(static_cast<int>(Tool::AutoFilter)),
            "Auto Filter sits to the right of Rectangle Filter");
    palette.show();
    QCoreApplication::processEvents();
    auto& languageManager = snow_shot::presentation::LanguageManager::instance();
    for (const auto& language :
         {QStringLiteral("zh_CN"), QStringLiteral("zh_TW"), QStringLiteral("en_US")}) {
        require(languageManager.setLanguage(language), "Auto Filter translation catalog loads");
        QCoreApplication::processEvents();
        require(category->placeholder() ==
                    QCoreApplication::translate("ScreenshotToolPalette", "Fill regions"),
                "category placeholder retranslates in place");
        require(category->model()
                        ->index(1, 0)
                        .data(adqt::widgets::AdSelect::DefaultLabelRole)
                        .toString() ==
                    QCoreApplication::translate("ScreenshotToolPalette", "Text in box"),
                "category labels retranslate in place");
        require(!category->currentValue().isValid(), "language changes keep category unselected");
    }
    if (const QString path = qEnvironmentVariable("SNOW_AUTO_FILTER_SNAPSHOT"); !path.isEmpty()) {
        require(palette.grab().save(path), "save Auto Filter toolbar inspection image");
    }
}

void regionControlsFollowTheActiveCaptureType() {
    const auto previous = screenshotRegionPreference();
    for (const auto type : {ScreenshotRegionType::Rectangle, ScreenshotRegionType::Polyline,
                            ScreenshotRegionType::Curve, ScreenshotRegionType::Freehand}) {
        // A previous capture's preference must not override the active capture on confirmation.
        setScreenshotRegionPreference(ScreenshotRegionType((int(type) + 1) % 4));
        ScreenshotRegionTypeControl floating(nullptr, true);
        floating.setType(type);
        ScreenshotToolPalette::Options options;
        options.showMoveTool = true;
        options.showMoveOptionsToolbar = true;
        ScreenshotToolPalette palette(options);
        int commands = 0;
        QObject::connect(&palette, &ScreenshotToolPalette::screenshotRegionTypeRequested,
                         [&] { ++commands; });
        palette.setScreenshotRegionType(type);
        palette.setActiveTool(ScreenshotToolPalette::Tool::Move);
        auto* group = palette.findChild<adqt::widgets::AdRadioButtonGroup*>(
            QStringLiteral("screenshotMoveRegionTypeButtonGroup"));
        const auto requireSynchronized = [&] {
            require(group && group->checkedId() == int(type),
                    "Move region controls display the active capture type before any click");
            for (int i = 0; i < 4; ++i) {
                auto* hint = floating.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotRegionType_") +
                    screenshotRegionTypeId(ScreenshotRegionType(i)));
                require(hint &&
                            (hint->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid) ==
                                (i == int(type)) &&
                            group->button(i)->isChecked() == (i == int(type)),
                        "Move and overlay controls highlight exactly the same initial region");
            }
        };
        requireSynchronized();
        palette.show();
        floating.show();
        QCoreApplication::processEvents();
        requireSynchronized();
        setScreenshotRegionPreference(ScreenshotRegionType((int(type) + 2) % 4));
        floating.setType(type);
        palette.setPhysicalScale(1.5);
        palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
        palette.setActiveTool(ScreenshotToolPalette::Tool::Move);
        group = palette.findChild<adqt::widgets::AdRadioButtonGroup*>(
            QStringLiteral("screenshotMoveRegionTypeButtonGroup"));
        requireSynchronized();
        const auto next = ScreenshotRegionType((int(type) + 3) % 4);
        palette.setScreenshotRegionType(next);
        require(group->checkedId() == int(next) && commands == 0,
                "live capture type updates synchronize the toolbar without issuing commands");
    }
    setScreenshotRegionPreference(previous);
}

void moveToolExposesCaptureCursorAndRecaptureOptions() {
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showMoveOptionsToolbar = true;
    ScreenshotToolPalette palette(options);
    palette.setCaptureCursorEnabled(false);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Move);

    auto* controls = palette.findChild<QWidget*>(QStringLiteral("screenshotMoveActionControls"));
    auto* cursor = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotCaptureCursorButton"));
    auto* recapture =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRecaptureButton"));
    auto* hideSelectionToolbar = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotHideSelectionToolbarButton"));
    auto* addRegion =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotAddRegionButton"));
    auto* subtractRegion = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotSubtractRegionButton"));
    auto* regionSeparator =
        palette.findChild<QFrame*>(QStringLiteral("screenshotRegionActionsSeparator"));
    auto* layout = controls != nullptr ? qobject_cast<QBoxLayout*>(controls->layout()) : nullptr;
    require(controls != nullptr && cursor != nullptr && recapture != nullptr &&
                hideSelectionToolbar != nullptr && addRegion != nullptr &&
                subtractRegion != nullptr && regionSeparator != nullptr && layout != nullptr &&
                palette.actionToolbarVisible() && !palette.styleToolbarVisible(),
            "Move must materialize and display its dedicated options row");
    require(
        layout->indexOf(addRegion) == 0 && layout->indexOf(subtractRegion) == 2 &&
            layout->indexOf(regionSeparator) == 6 && layout->indexOf(cursor) == 8 &&
            layout->indexOf(recapture) == 10 &&
            layout->indexOf(hideSelectionToolbar) == layout->count() - 1 &&
            layout->itemAt(layout->indexOf(hideSelectionToolbar) - 4) != nullptr &&
            qobject_cast<QFrame*>(
                layout->itemAt(layout->indexOf(hideSelectionToolbar) - 4)->widget()) != nullptr,
        "Move options must group region actions at the far left, before capture actions and hide");
    require(!cursor->isCheckable() && !cursor->isChecked() && !palette.captureCursorEnabled(),
            "Capture cursor must use the same state-driven action button as scrolling screenshot");

    auto* regionTypes =
        palette.findChild<QWidget*>(QStringLiteral("screenshotMoveRegionTypeButtonGroup"));
    auto* regionGroup = regionTypes != nullptr
                            ? regionTypes->findChild<adqt::widgets::AdRadioButtonGroup*>()
                            : nullptr;
    require(regionTypes && regionGroup &&
                layout->indexOf(regionTypes) == layout->indexOf(subtractRegion) + 2,
            "shape group follows subtract region");
    auto* unitGroup = palette.findChild<adqt::widgets::AdRadioButtonGroup*>(
        QStringLiteral("screenshotSelectionDisplayUnitButtonGroup"));
    require(unitGroup && unitGroup->buttons().size() == 2 &&
                unitGroup->checkedId() == int(kDefaultScreenshotSelectionDisplayUnit) &&
                layout->indexOf(qobject_cast<QWidget*>(unitGroup->parent())) ==
                    layout->indexOf(hideSelectionToolbar) - 2,
            "the exclusive unit group must use platform defaults directly before Hide");
    int unitCommands = 0;
    auto requestedUnit = kDefaultScreenshotSelectionDisplayUnit;
    QObject::connect(&palette, &ScreenshotToolPalette::selectionDisplayUnitChanged,
                     [&](ScreenshotSelectionDisplayUnit unit) {
                         ++unitCommands;
                         requestedUnit = unit;
                     });
    for (const auto unit : {ScreenshotSelectionDisplayUnit::PhysicalPixels,
                            ScreenshotSelectionDisplayUnit::LogicalPixels}) {
        auto* button = unitGroup->button(int(unit));
        require(button && !button->icon().isNull() && !button->toolTip().isEmpty() &&
                    button->accessibleName() == button->toolTip(),
                "unit buttons must expose icons and full accessible tooltips");
        button->click();
        require(requestedUnit == unit && unitGroup->checkedId() == int(unit) &&
                    !unitGroup->button(1 - int(unit))->isChecked(),
                "clicking a unit must dispatch it and select exactly one option");
    }
    require(unitCommands == 2, "unit clicks dispatch exactly once each");
    auto& language = snow_shot::presentation::LanguageManager::instance();
    for (const QString locale :
         {QStringLiteral("zh_CN"), QStringLiteral("zh_TW"), QStringLiteral("en_US")}) {
        require(language.setLanguage(locale), "unit tooltip catalogs must load");
        QCoreApplication::processEvents();
        const auto tooltip =
            QCoreApplication::translate("ScreenshotToolPalette", "Logical Pixel Selection");
        require(unitGroup->button(1)->toolTip() == tooltip &&
                    unitGroup->button(1)->accessibleName() == tooltip,
                "unit tooltips and accessible names must retranslate live");
    }
    palette.show();
    QCoreApplication::processEvents();
    const auto requireJoinedRegionButtons = [&] {
        require(regionTypes->width() == regionTypes->sizeHint().width(),
                "region group must not stretch and insert gaps between fixed-width buttons");
        for (int index = 1; index < 4; ++index) {
            const auto* previous = regionGroup->button(index - 1);
            const auto* current = regionGroup->button(index);
            require(current->x() == previous->x() + previous->width() - 2,
                    "adjacent region buttons must retain the shared border overlap");
        }
    };
    requireJoinedRegionButtons();
    const QSize referenceRegionSize = regionGroup->button(0)->size();
    const QSize referenceRegionIconSize = regionGroup->button(0)->iconSize();
    int selectedType = -1;
    QObject::connect(&palette, &ScreenshotToolPalette::screenshotRegionTypeRequested,
                     [&](int type) {
                         selectedType = type;
                         setScreenshotRegionPreference(ScreenshotRegionType(type));
                         palette.setScreenshotRegionType(ScreenshotRegionType(type));
                     });
    for (const auto type : {ScreenshotRegionType::Rectangle, ScreenshotRegionType::Polyline,
                            ScreenshotRegionType::Curve, ScreenshotRegionType::Freehand}) {
        auto* button = qobject_cast<adqt::widgets::AdRadio*>(regionGroup->button(int(type)));
        require(button && !button->toolTip().isEmpty() && button->size() == referenceRegionSize &&
                    button->iconSize() == referenceRegionIconSize &&
                    button->height() == subtractRegion->height(),
                "region controls share the action row height and proportional radio metrics");
        button->click();
        require(selectedType == int(type) && button->isChecked(),
                "region button dispatches and reflects exclusive state");
        int checked = 0;
        for (auto* option : regionGroup->buttons())
            checked += option->isChecked() ? 1 : 0;
        require(checked == 1, "exactly one region type is checked");
    }
    require(palette.setPhysicalScale(1.5), "Move toolbar accepts a larger display scale");
    QCoreApplication::processEvents();
    require(regionGroup->button(0)->size() == QSize(qRound(referenceRegionSize.width() * 1.5),
                                                    qRound(referenceRegionSize.height() * 1.5)) &&
                regionGroup->button(0)->height() == subtractRegion->height() &&
                regionGroup->button(0)->iconSize() ==
                    QSize(qRound(referenceRegionIconSize.width() * 1.5),
                          qRound(referenceRegionIconSize.height() * 1.5)),
            "region button and icon scale with the Move toolbar");
    requireJoinedRegionButtons();
    require(palette.setPhysicalScale(1.0), "Move toolbar restores its original display scale");
    QCoreApplication::processEvents();
    requireJoinedRegionButtons();
    require(regionGroup->button(0)->size() == referenceRegionSize &&
                regionGroup->button(0)->iconSize() == referenceRegionIconSize,
            "region button and icon return to their original sizes");
    setScreenshotRegionPreference(ScreenshotRegionType::Rectangle);
    int additions = 0, subtractions = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::addScreenshotRegionRequested,
                     [&] { ++additions; });
    QObject::connect(&palette, &ScreenshotToolPalette::subtractScreenshotRegionRequested,
                     [&] { ++subtractions; });
    addRegion->click();
    subtractRegion->click();
    require(additions == 1 && subtractions == 1, "region buttons must dispatch distinct commands");
    require(addRegion->toolTip() == QStringLiteral("Add screenshot region") &&
                subtractRegion->toolTip() == QStringLiteral("Subtract screenshot region"),
            "region button labels");
    int cursorChanges = 0;
    int recaptures = 0;
    int hideChanges = 0;
    bool selectionToolbarHidden = false;
    QObject::connect(&palette, &ScreenshotToolPalette::captureCursorToggled,
                     [&cursorChanges](bool enabled) { cursorChanges += enabled ? 1 : 100; });
    QObject::connect(&palette, &ScreenshotToolPalette::recaptureRequested,
                     [&recaptures]() { ++recaptures; });
    QObject::connect(&palette, &ScreenshotToolPalette::selectionToolbarHiddenChanged,
                     [&hideChanges, &selectionToolbarHidden](bool hidden) {
                         ++hideChanges;
                         selectionToolbarHidden = hidden;
                     });
    cursor->click();
    require(cursorChanges == 1 && palette.captureCursorEnabled(),
            "Capture cursor clicks must update state and emit the persisted-setting command");
    recapture->click();
    require(recaptures == 1 && recapture->toolTip() == shortcutTooltip(QStringLiteral("Recapture"),
                                                                       {QStringLiteral("Alt+R")}),
            "Recapture must emit once and show its configurable default shortcut");
    hideSelectionToolbar->click();
    require(
        hideChanges == 1 && selectionToolbarHidden && palette.selectionToolbarHidden() &&
            hideSelectionToolbar->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
            hideSelectionToolbar->accentRole() == adqt::widgets::AdButton::AccentRole::Primary &&
            !hideSelectionToolbar->isCheckable(),
        "hiding the selection toolbar must activate the button");
    hideSelectionToolbar->click();
    require(hideChanges == 2 && !selectionToolbarHidden && !palette.selectionToolbarHidden() &&
                hideSelectionToolbar->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                hideSelectionToolbar->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral,
            "showing the selection toolbar must deactivate the button");
    palette.setRecaptureBusy(true);
    require(!recapture->isEnabled() &&
                !palette.activateScreenshotShortcut(QStringLiteral("recapture")),
            "busy recapture must disable both pointer and shortcut activation");
    palette.setRecaptureBusy(false);
    require(palette.activateScreenshotShortcut(QStringLiteral("recapture")) && recaptures == 2,
            "the shortcut must invoke the same Recapture button signal path");

    // Move mode leaves the canvas engine on a non-drawing tool, so the engine
    // reports DefaultRectangle, and selector refresh can report selection-based
    // sources, after Move activates. Those pushes must sync editor values
    // without choosing an editor: the Move options row is not a canvas style
    // editor, so the active tool alone keeps owning which row is displayed.
    SnowCanvasStyleToolbarState defaultRectangleState;
    defaultRectangleState.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    palette.setStyleToolbarState(defaultRectangleState);
    SnowCanvasStyleToolbarState selectedRectangleState;
    selectedRectangleState.source = SnowCanvasStyleToolbarSource::SelectedRectangle;
    palette.setStyleToolbarState(selectedRectangleState);
    require(palette.actionToolbarVisible() && !palette.styleToolbarVisible() &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotMoveActionControls")) !=
                    nullptr &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls")) ==
                    nullptr,
            "canvas style pushes must keep the Move options row, not the Shape editors");
    auto* retainedCursor = palette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotCaptureCursorButton"));
    auto* retainedRecapture =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRecaptureButton"));
    require(retainedCursor == cursor && retainedRecapture == recapture &&
                palette.captureCursorEnabled(),
            "canvas style pushes must not rebuild the Move options row or reset its state");
    require(palette.activateScreenshotShortcut(QStringLiteral("recapture")) && recaptures == 3,
            "Move options keep their commands after canvas style pushes");

    ScreenshotToolPalette scrollingPalette(options);
    scrollingPalette.setActiveTool(ScreenshotToolPalette::Tool::ScrollingScreenshot);
    auto* scrollingButton = scrollingPalette.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotScrollingAutoScrollButton"));
    auto* scrollingControls =
        scrollingPalette.findChild<QWidget*>(QStringLiteral("screenshotScrollingRecognitionMode"));
    require(scrollingButton != nullptr && scrollingControls != nullptr,
            "scrolling screenshot must expose the existing action components");
    const auto requireMatchingButtonState = [&]() {
        require(cursor->isCheckable() == scrollingButton->isCheckable() &&
                    cursor->isChecked() == scrollingButton->isChecked() &&
                    cursor->buttonStyle() == scrollingButton->buttonStyle() &&
                    cursor->accentRole() == scrollingButton->accentRole() &&
                    cursor->sizeClass() == scrollingButton->sizeClass(),
                "Move and scrolling screenshot must use identical button rendering states");
    };
    scrollingButton->click();
    requireMatchingButtonState();
    cursor->click();
    scrollingButton->click();
    require(!palette.captureCursorEnabled() && cursorChanges == 101,
            "clicking Capture cursor again must disable capture and emit once");
    requireMatchingButtonState();
    require(recapture->buttonStyle() == scrollingButton->buttonStyle() &&
                recapture->accentRole() == scrollingButton->accentRole() &&
                recapture->isCheckable() == scrollingButton->isCheckable(),
            "Recapture must use the same unselected action button style as scrolling screenshot");
    palette.setCaptureCursorEnabled(true);
    scrollingButton->click();
    require(cursorChanges == 101, "inbound capture state must not emit a user command");
    requireMatchingButtonState();
    ScreenshotToolPalette shapePalette(options);
    shapePalette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    shapePalette.show();
    QCoreApplication::processEvents();
    auto* shapeRectangle =
        qobject_cast<adqt::widgets::AdRadio*>(controlWithTooltip(shapePalette, "Rectangle"));
    require(shapeRectangle != nullptr, "Shape toolbar exposes its reference button group");
    const QSize shapeReferenceSize = shapeRectangle->size();
    const QSize shapeReferenceIconSize = shapeRectangle->iconSize();
    const qreal moveToShapeRatio =
        qreal(referenceRegionSize.height()) / shapeReferenceSize.height();
    require(referenceRegionSize.width() == qRound(shapeReferenceSize.width() * moveToShapeRatio) &&
                referenceRegionIconSize.width() ==
                    qRound(shapeReferenceIconSize.width() * moveToShapeRatio) &&
                referenceRegionIconSize.height() ==
                    qRound(shapeReferenceIconSize.height() * moveToShapeRatio),
            "Move radio width and icon preserve the Shape button's proportions at action height");
    const qreal shapeSideInset =
        (shapeReferenceSize.width() - shapeReferenceIconSize.width()) / 2.0;
    const qreal moveSideInset =
        (referenceRegionSize.width() - referenceRegionIconSize.width()) / 2.0;
    require(qAbs(moveSideInset - shapeSideInset * moveToShapeRatio) <= 0.5,
            "Move radio keeps the Shape button's proportional padding on both sides of its icon");
    for (const qreal scale : {1.0, 0.75, 1.25, 1.5, 2.0, 1.0}) {
        requireMatchingButtonState();
        palette.setPhysicalScale(scale);
        scrollingPalette.setPhysicalScale(scale);
        shapePalette.setPhysicalScale(scale);
        palette.prepareForDisplay();
        scrollingPalette.prepareForDisplay();
        shapePalette.prepareForDisplay();
        QCoreApplication::processEvents();
        requireJoinedRegionButtons();
        require(regionGroup->button(0)->size() ==
                        QSize(qRound(referenceRegionSize.width() * scale),
                              qRound(referenceRegionSize.height() * scale)) &&
                    shapeRectangle->size() == QSize(qRound(shapeReferenceSize.width() * scale),
                                                    qRound(shapeReferenceSize.height() * scale)) &&
                    regionGroup->button(0)->iconSize() ==
                        QSize(qRound(referenceRegionIconSize.width() * scale),
                              qRound(referenceRegionIconSize.height() * scale)) &&
                    shapeRectangle->iconSize() ==
                        QSize(qRound(shapeReferenceIconSize.width() * scale),
                              qRound(shapeReferenceIconSize.height() * scale)),
                "Move and Shape button groups retain their relative dimensions at every scale");
        require(cursor->size() == scrollingButton->size() &&
                    recapture->size() == scrollingButton->size() &&
                    cursor->iconSize() == scrollingButton->iconSize(),
                "Move buttons and icons must match scrolling screenshot metrics at every scale");
        require(palette.actionPanel()->height() == scrollingPalette.actionPanel()->height() &&
                    palette.actionPanel()->layout()->contentsMargins() ==
                        scrollingPalette.actionPanel()->layout()->contentsMargins(),
                "Move must share scrolling screenshot panel height and padding at every scale");
        require(unitGroup->button(0)->size() == regionGroup->button(0)->size() &&
                    unitGroup->button(0)->iconSize() == regionGroup->button(0)->iconSize(),
                "unit buttons and icons must follow action metrics at every scale");
        require(unitGroup->button(1)->x() ==
                    unitGroup->button(0)->x() + unitGroup->button(0)->width() - 2,
                "unit buttons must share a joined border at every scale");
        auto* scrollingLayout = scrollingControls->layout();
        const int hideSeparator = layout->indexOf(hideSelectionToolbar) - 4;
        require(regionSeparator->size() == layout->itemAt(hideSeparator)->widget()->size() &&
                    layout->itemAt(hideSeparator)->widget()->size() ==
                        scrollingLayout->itemAt(2)->widget()->size() &&
                    layout->itemAt(hideSeparator - 1)->sizeHint() ==
                        scrollingLayout->itemAt(1)->sizeHint() &&
                    layout->itemAt(hideSeparator + 1)->sizeHint() ==
                        scrollingLayout->itemAt(3)->sizeHint(),
                "the hide-toolbar separator and group spacing must match scrolling screenshot "
                "controls");
    }
    ScreenshotToolPalette preScaledPalette(options);
    require(preScaledPalette.setPhysicalScale(1.5),
            "Move toolbar can receive its display scale before the options row is created");
    preScaledPalette.setActiveTool(ScreenshotToolPalette::Tool::Move);
    preScaledPalette.show();
    QCoreApplication::processEvents();
    auto* preScaledRegionTypes =
        preScaledPalette.findChild<QWidget*>(QStringLiteral("screenshotMoveRegionTypeButtonGroup"));
    auto* preScaledRegionGroup =
        preScaledRegionTypes != nullptr
            ? preScaledRegionTypes->findChild<adqt::widgets::AdRadioButtonGroup*>()
            : nullptr;
    require(preScaledRegionGroup != nullptr &&
                preScaledRegionGroup->button(0)->size() ==
                    QSize(qRound(referenceRegionSize.width() * 1.5),
                          qRound(referenceRegionSize.height() * 1.5)) &&
                preScaledRegionGroup->button(0)->iconSize() ==
                    QSize(qRound(referenceRegionIconSize.width() * 1.5),
                          qRound(referenceRegionIconSize.height() * 1.5)) &&
                preScaledRegionTypes->width() == preScaledRegionTypes->sizeHint().width(),
            "Move group uses the same proportions when created after a display scale change");
    palette.setRecaptureBusy(true);
    for (const auto tool :
         {ScreenshotToolPalette::Tool::ScrollingScreenshot, ScreenshotToolPalette::Tool::Shape,
          ScreenshotToolPalette::Tool::Select}) {
        palette.setActiveTool(tool);
        palette.setActiveTool(ScreenshotToolPalette::Tool::Move);
        palette.prepareForDisplay();
        cursor = palette.findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotCaptureCursorButton"));
        recapture = palette.findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotRecaptureButton"));
        hideSelectionToolbar = palette.findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotHideSelectionToolbarButton"));
        require(palette.actionToolbarVisible() && !palette.styleToolbarVisible() &&
                    cursor != nullptr && palette.captureCursorEnabled() && recapture != nullptr &&
                    !recapture->isEnabled() && hideSelectionToolbar != nullptr,
                "returning to Move must restore capture state through the shared action row");
        requireMatchingButtonState();
        unitGroup = palette.findChild<adqt::widgets::AdRadioButtonGroup*>(
            QStringLiteral("screenshotSelectionDisplayUnitButtonGroup"));
        require(unitGroup &&
                    unitGroup->checkedId() == int(ScreenshotSelectionDisplayUnit::LogicalPixels) &&
                    unitCommands == 2,
                "rebuilding Move must retain the selected unit without dispatching commands");
    }
    palette.setRecaptureBusy(false);
    require(palette.activateScreenshotShortcut(QStringLiteral("recapture")) && recaptures == 4,
            "rebuilt Move controls must keep the existing shortcut command path");

    ScreenshotToolPalette::Options pinnedOptions;
    pinnedOptions.showMoveTool = true;
    pinnedOptions.moveToolPresentation = ScreenshotToolPalette::MoveToolPresentation::ResizeWindow;
    ScreenshotToolPalette pinnedPalette(pinnedOptions);
    pinnedPalette.setActiveTool(ScreenshotToolPalette::Tool::Move);
    require(!pinnedPalette.styleToolbarVisible() && !pinnedPalette.actionToolbarVisible() &&
                pinnedPalette.findChild<QWidget*>(QStringLiteral("screenshotMoveActionControls")) ==
                    nullptr,
            "Resize window Move must remain unchanged without screenshot capture options");
}

void regionSwitcherRetranslatesAndRenders() {
#if defined(Q_OS_WIN)
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) >= 0,
            "region snapshots need Chinese font coverage");
#endif
    auto& language = snow_shot::presentation::LanguageManager::instance();
    auto& appTheme = snow_shot::presentation::styles::ThemeManager::instance();
    const auto oldMode = appTheme.themeMode();
    ScreenshotRegionTypeControl floating(nullptr, true);
    floating.show();
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    options.showMoveOptionsToolbar = true;
    ScreenshotToolPalette palette(options);
    palette.setActiveTool(ScreenshotToolPalette::Tool::Move);
    palette.show();
    const auto snapshots = qEnvironmentVariable("SNOW_SHOT_REGION_SNAPSHOTS");
    if (!snapshots.isEmpty())
        require(QDir().mkpath(snapshots), "create region snapshots directory");
    for (const bool dark : {false, true}) {
        appTheme.setThemeMode(dark ? snow_shot::presentation::styles::ThemeMode::Dark
                                   : snow_shot::presentation::styles::ThemeMode::Light);
        adqt::theme::ThemeManager::instance().applyTo(*qApp);
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
        for (const auto& locale :
             {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
            require(language.setLanguage(locale), "region catalog loads");
            setScreenshotRegionPreference(ScreenshotRegionType::Curve);
            palette.setScreenshotRegionType(ScreenshotRegionType::Curve);
            QCoreApplication::processEvents();
            auto* curve = floating.findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenshotRegionType_curve"));
            require(curve && curve->toolTip() == QCoreApplication::translate(
                                                     "ScreenshotRegionTypeControl", "Curve region"),
                    "floating control retranslates and synchronizes preference");
            require(curve->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                        curve->accentRole() == adqt::widgets::AdButton::AccentRole::Primary &&
                        !curve->isCheckable() && curve->focusPolicy() == Qt::NoFocus &&
                        curve->iconRef().colors().isEmpty() &&
                        adqt::icons::describeIcon(curve->iconRef()).colorModel ==
                            adqt::icons::IconColorModel::Monochrome,
                    "floating region icon uses the drawing toolbar active style and inherits its "
                    "icon color");
            const QImage buttonImage = renderButton(*curve);
            const auto containsColor = [&buttonImage](const QColor& color) {
                for (int y = 0; y < buttonImage.height(); ++y) {
                    for (int x = 0; x < buttonImage.width(); ++x) {
                        if (buttonImage.pixelColor(x, y) == color)
                            return true;
                    }
                }
                return false;
            };
            require(buttonBackgroundSample(*curve) == scheme.map.colorPrimary &&
                        containsColor(scheme.map.colorWhite),
                    "floating checked button paints the drawing toolbar active background and "
                    "toolbar icon color");
            require(renderButton(floating).pixelColor(4, floating.height() / 2) ==
                        scheme.map.colorBgContainer,
                    "floating region surface uses the drawing toolbar container background");
            auto* hint = floating.findChild<QLabel*>();
            require(hint &&
                        hint->text() == QCoreApplication::translate("ScreenshotRegionTypeControl",
                                                                    "%1 to switch region type")
                                            .arg(QKeySequence(screenshotRegionTypeCycleKey())
                                                     .toString(QKeySequence::NativeText)),
                    "region hint must show the active platform shortcut in every language");
            require(hint &&
                        hint->heightForWidth(hint->width()) ==
                            hint->heightForWidth(hint->width() * 2) &&
                        hint->palette().color(QPalette::WindowText) ==
                            scheme.map.colorTextSecondary,
                    "floating hint stays on one line and uses the secondary theme text color");
            if (!snapshots.isEmpty()) {
                const auto suffix =
                    locale + (dark ? QStringLiteral("-dark") : QStringLiteral("-light"));
                require(floating.grab().save(QDir(snapshots).filePath(
                            QStringLiteral("region-switcher-") + suffix + QStringLiteral(".png"))),
                        "save floating switcher snapshot");
                require(palette.actionPanel()->grab().save(QDir(snapshots).filePath(
                            QStringLiteral("region-toolbar-") + suffix + QStringLiteral(".png"))),
                        "save Move toolbar snapshot");
            }
        }
    }
    setScreenshotRegionPreference(ScreenshotRegionType::Rectangle);
    require(language.setLanguage(QStringLiteral("en_US")), "restore test language");
    appTheme.setThemeMode(oldMode);
    adqt::theme::ThemeManager::instance().applyTo(*qApp);
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "failed to create toolbar test storage directory");
    const QString executableDirectory =
        QDir(storageDirectory.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executableDirectory),
            "failed to create toolbar test executable directory");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({executableDirectory, storageDirectory.path(), 60000})
                .success,
            "failed to initialize isolated toolbar test storage");
#if defined(Q_OS_WIN)
    // The offscreen platform plugin exposes no system fonts; register one so
    // the font family editors and their shared cache have real content.
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "the font editor tests require a system TrueType font");
#endif
    if (application.arguments().contains(QStringLiteral("--text-style-only"))) {
        textStyleControlsExposeAndEmitAllRequestedProperties();
        textStylePopupLifecyclesAreBalanced();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--original-image-only"))) {
        originalImageToggleLeadsRecognitionActions();
        imageConversionToolsExposeRecognitionActions();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--remembered-drawing-tool-only"))) {
        rememberedDrawingModesPersistAcrossPaletteInstances();
        rememberedDrawingToolRecordedAndRestored();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--auto-filter-only"))) {
        configurationDrivenStyleEditorsShareStructuralContracts();
        filterEditorsRestoreValuesAfterToolSwitch();
        autoFilterLegacyStrengthMigration();
        selectedFilterTypeDoesNotReplaceCreationDefault();
        filterTypeSelectKeepsSmartEraseAcrossFilterModeSwitches();
        autoFilterControlsShareStylesAndKeepCategoryUnselected();
        filterToolExposesTypeAndIntensityControls();
        filterStyleEditorsMatchShapeAndSpotlightMetrics();
        rememberedDrawingModesPersistAcrossPaletteInstances();
        rememberedDrawingToolRecordedAndRestored();
        canvasToolStylesPersistIndependentlyWithoutGlobalStyles();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--color-input-only"))) {
        colorPickerChannelKeyboardInput();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--quick-save-only"))) {
        quickSaveStacksAndLayoutMigration();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--tool-shortcuts-only"))) {
        screenshotShortcutsShareButtonCommandsAndAvailability();
        clickingActiveToolbarToolReturnsToSelect();
        repeatingDrawingShortcutsReturnsToSelect();
        repeatingActionShortcutsReturnsToSelect();
        groupedToolShortcutsToggleOnlyTheRequestedTool();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--popup-recovery-only"))) {
        tableBusyStatePreservesSiblingGroupPopovers();
        tableBusyStatePreservesSiblingGroupPopovers(true);
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--popover-lifecycle-only"))) {
        mainToolbarGroupPopoversRecreateTheirOptions();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        styleToolbarPopoversMaterializeWithTheirOwners();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        textStylePopupLifecyclesAreBalanced();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        recordingExportSettingsPopoversStayLazy();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--move-options-only"))) {
        regionControlsFollowTheActiveCaptureType();
        moveToolExposesCaptureCursorAndRecaptureOptions();
        regionSwitcherRetranslatesAndRenders();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--region-switcher-only"))) {
        regionControlsFollowTheActiveCaptureType();
        regionSwitcherRetranslatesAndRenders();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--selection-reset-only"))) {
        selectionResetRemainsAvailableWithoutSelection();
        selectToolExposesDedicatedActionToolbar();
        selectionAlignmentActionsFollowSelectionUnitCount();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--recording-controls-only"))) {
        recordingSessionStatusMakesInvalidCombinationsUnrepresentable();
        recordingCursorOptionsAreIndependentAndLazy();
        recordingEffectSettingsModal();
        recordingControlsRemainLaidOutAcrossStateChanges();
        recordingExportSettingsAndDrawingAvailabilityFollowSessionState();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--arrow-icons-only"))) {
        arrowRatioEditorAdjustsAndResets();
        arrowStyleControlsExposeAndEmitAllStyleProperties();
        arrowheadOptionsRetranslateInPlace();
        selectedArrowMixedPropertiesResolveIndependently();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--dynamic-i18n-only"))) {
        dynamicToolbarLabelsUseEveryTranslationCatalog();
        moveToolPresentationUsesTheOwningShortcutScope();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--color-control-styles-only"))) {
        colorPresetEditorsPreserveCommandsAcrossRebinding();
        drawingColorsPreserveAlphaAcrossEditsAndToolSwitches();
        translucentColorSwatchesShowCheckerboardUnderlay();
        configurationDrivenStyleEditorsShareStructuralContracts();
        mixedColorsKeepUniformStyleButtonsActive();
        secondaryRowsDoNotDriftAcrossScaleRoundTrips();
        toolbarScalingDoesNotRelayoutPopupContent();
        popupColorEditorButtonsKeepPopupScaleAfterToolbarDpiCommit();
        familiesHydratedAfterScaleKeepTheSamePhysicalSize();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--canvas-style-persistence-only"))) {
        screenshotProductStyleProfileIsComplete();
        canvasToolStylesPersistIndependentlyWithoutGlobalStyles();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (QApplication::arguments().contains(QStringLiteral("--dpi-scaling-only"))) {
        secondaryRowsDoNotDriftAcrossScaleRoundTrips();
        toolbarScalingDoesNotRelayoutPopupContent();
        popupColorEditorButtonsKeepPopupScaleAfterToolbarDpiCommit();
        familiesHydratedAfterScaleKeepTheSamePhysicalSize();
        physicalScaleDefersHiddenStyleGroupGeometry();
        watermarkControlsFollowPhysicalScale();
        styleToolbarRowSpacingFollowsPhysicalScale();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--lazy-loading-only"))) {
        secondaryControlsMaterializeOnlyForTheRequestedFamily();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--stroke-editor-only"))) {
        selectedStrokeColorDragKeepsPickerIndicatorInSync();
        textAndHighlightStrokeWidthTriggersUseSharedPreviewButton();
        shapeAndArrowStrokeEditorsShareThePresetCatalog();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--style-reconcile-only"))) {
        styleToolSwitchesReconcileCompatibleEditorRoots();
        styleToolReuseMapPreservesEveryCompatibleRole();
        retainedOutlineEditorsRebindStateLabelsAndCommands();
        prewarmedDestinationMergesSourceSharedAndDestinationOnlyEditors();
        retainedEditorsApplyDestinationMixedStateDuringReconciliation();
        repeatedStyleReconciliationDoesNotAccumulateHiddenRows();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--popup-lifecycle-only"))) {
        textStylePopupLifecyclesAreBalanced();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ocr-translation-only"))) {
        ocrToolReplacesSelectionActionToolbarContents();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--image-conversion-only"))) {
        imageConversionToolsExposeRecognitionActions();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--screenshot-actions-tooltips-only"))) {
        screenshotActionTooltipsUseConfiguredShortcuts();
        screenshotActionTooltipsFollowStorageChangesWithoutRetranslation();
        moveToolPresentationUsesTheOwningShortcutScope();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--shortcut-tooltips-only"))) {
        groupedDrawingOptionsShowShortcutTooltips();
        groupedActionOptionsShowShortcutTooltips();
        stylePopoverTriggersProvideMouseFeedback();
        screenshotActionTooltipsUseConfiguredShortcuts();
        screenshotActionTooltipsFollowStorageChangesWithoutRetranslation();
        moveToolPresentationUsesTheOwningShortcutScope();
        ocrToolReplacesSelectionActionToolbarContents();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--canvas-color-sampling-only"))) {
        canvasColorSamplerButtonRequestsAndCommits();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--pinned-actions-only"))) {
        confirmActionRemainsSeparatedAndCallableForPinnedEditing();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--spotlight-wheel-only"))) {
        spotlightControlsMatchMaskConfigurationBehavior();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--watermark-template-only"))) {
        watermarkTemplateLibraryAndEditorApplySnapshotsDeterministically();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--draw-template-only"))) {
        drawTemplateSelectSavesFiltersInsertsAndDeletes();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--watermark-template-width-only"))) {
        watermarkTemplateSelectMatchesFontSelectWidth();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--toolbar-layout-only"))) {
        drawingModeSelectionsSurviveToolbarReentry();
        drawingGroupClicksActivateOnceAfterPointerReentry();
        configurableToolbarLayoutSupportsArbitraryPopoverGroups();
        drawingHistoryActionsFollowStacksAndHiddenShortcuts();
        recordingDrawingLayoutRendersConfiguredSeparator();
        arrowAndLineUseConfiguredPopoverGroup();
        highlightVariantsUseConfiguredPopoverGroup();
        drawingToolbarGroupsUseToolbarPopoverMetrics();
        spotlightControlsMatchMaskConfigurationBehavior();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--history-only"))) {
        screenshotShortcutsShareButtonCommandsAndAvailability();
        drawingHistoryActionsFollowStacksAndHiddenShortcuts();
        tableToolExposesStructureActionsAndOwnHistoryState();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--stack-availability-only"))) {
        actionStacksKeepEnabledAlternativesReachable();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--action-toolbar-layout-only"))) {
        pinnedActionLayoutUsesGenericStacks();
        screenshotToolbarUsesCanonicalOrderAndSectionSeparators();
        toolbarStacksFollowConfiguredBottomToTopOrder();
        actionStacksKeepEnabledAlternativesReachable();
        drawingGroupClicksActivateOnceAfterPointerReentry();
        tableRecognitionClickActivatesOnceAfterPointerReentry();
        tableBusyStatePreservesSiblingGroupPopovers();
        tableBusyStatePreservesSiblingGroupPopovers(true);
        tableQrPopoverSharesOneEntryAndRemembersTheSelectedMode();
        sharedToolbarLayoutModelOperationsAreDeterministic();
        configurableScreenshotActionLayoutSupportsStacksHidingAndRuntimeReplacement();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--scrolling-only"))) {
        scrollingSelectionButtonsDragAndLockAxis();
        scrollingScreenshotExposesAxisRecognitionModes();
        scrollingScreenshotKeepsDrawingToolsAvailable();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    colorPresetEditorsPreserveCommandsAcrossRebinding();
    recordingSessionStatusMakesInvalidCombinationsUnrepresentable();
    recordingControlsRemainLaidOutAcrossStateChanges();
    translucentColorSwatchesShowCheckerboardUnderlay();
    recordingExportSettingsAndDrawingAvailabilityFollowSessionState();
    dynamicToolbarLabelsUseEveryTranslationCatalog();
    numericStrokeWidthPreviewUsesLineWithinPreviewBounds();
    secondaryControlsMaterializeOnlyForTheRequestedFamily();
    regionControlsFollowTheActiveCaptureType();
    moveToolExposesCaptureCursorAndRecaptureOptions();
    textAndHighlightStrokeWidthTriggersUseSharedPreviewButton();
    shapeAndArrowStrokeEditorsShareThePresetCatalog();
    sizePresetEditorsShareTheSizeCatalog();
    styleToolSwitchesReconcileCompatibleEditorRoots();
    styleToolReuseMapPreservesEveryCompatibleRole();
    retainedOutlineEditorsRebindStateLabelsAndCommands();
    prewarmedDestinationMergesSourceSharedAndDestinationOnlyEditors();
    retainedEditorsApplyDestinationMixedStateDuringReconciliation();
    repeatedStyleReconciliationDoesNotAccumulateHiddenRows();
    fontFamilyListIsCachedForEditorBuilds();
    scrollingScreenshotKeepsDrawingToolsAvailable();
    recognitionToolsKeepDrawingToolsAvailable();
    scrollingSelectionButtonsDragAndLockAxis();
    scrollingScreenshotExposesAxisRecognitionModes();
    screenshotToolbarUsesCanonicalOrderAndSectionSeparators();
    moveToolPresentationUsesTheOwningShortcutScope();
    groupedDrawingOptionsShowShortcutTooltips();
    groupedActionOptionsShowShortcutTooltips();
    screenshotActionTooltipsFollowStorageChangesWithoutRetranslation();
    configurableToolbarLayoutSupportsArbitraryPopoverGroups();
    drawingHistoryActionsFollowStacksAndHiddenShortcuts();
    recordingDrawingLayoutRendersConfiguredSeparator();
    ocrControlReflectsLoadingState();
    ocrToolReplacesSelectionActionToolbarContents();
    clickingActiveToolbarToolReturnsToSelect();
    screenshotShortcutsShareButtonCommandsAndAvailability();
    repeatingDrawingShortcutsReturnsToSelect();
    repeatingActionShortcutsReturnsToSelect();
    groupedToolShortcutsToggleOnlyTheRequestedTool();
    tableToolExposesStructureActionsAndOwnHistoryState();
    tableBusyStatePreservesSiblingGroupPopovers();
    tableQrPopoverSharesOneEntryAndRemembersTheSelectedMode();
    tableRecognitionClickActivatesOnceAfterPointerReentry();
    drawingGroupClicksActivateOnceAfterPointerReentry();
    sharedToolbarLayoutModelOperationsAreDeterministic();
    toolbarStacksFollowConfiguredBottomToTopOrder();
    actionStacksKeepEnabledAlternativesReachable();
    configurableScreenshotActionLayoutSupportsStacksHidingAndRuntimeReplacement();
    arrowAndLineRemainDirectWhenConfiguredIndividually();
    confirmActionRemainsSeparatedAndCallableForPinnedEditing();
    isolatedBusyIndicatorMatchesItsOwnerWindowBand();
    repeatedToolsAndDifferentialStyleSynchronizationAreNoOps();
    editorlessToolsRejectStaleStyleToolbarState();
    lineToolIsDiscoverableSelectableAndUsesLinearStyleControls();
    freeDrawToolIsDistinctAndUsesIndependentPathStyleControls();
    highlightVariantsUseConfiguredPopoverGroup();
    drawingToolbarGroupsUseToolbarPopoverMetrics();
    spotlightControlsMatchMaskConfigurationBehavior();
    highlightStyleToolbarWidthTracksActiveMode();
    eraserToolIsDiscoverableAndHidesStyleControls();
    filterEditorsRestoreValuesAfterToolSwitch();
    filterTypeSelectKeepsSmartEraseAcrossFilterModeSwitches();
    filterToolExposesTypeAndIntensityControls();
    drawingModeSelectionsSurviveToolbarReentry();
    rememberedDrawingModesPersistAcrossPaletteInstances();
    rememberedDrawingToolRecordedAndRestored();
    filterStyleEditorsMatchShapeAndSpotlightMetrics();
    watermarkToolExposesSharedStyleControls();
    watermarkStyleEditorMatchesShapeHeight();
    watermarkAndTextToolsUseStandardSpacing();
    watermarkControlsFollowCommittedStateAndUndo();
    watermarkEditsCommitCompleteConfigsAndClampWheel();
    watermarkTemplateLibraryAndEditorApplySnapshotsDeterministically();
    watermarkControlsFollowPhysicalScale();
    drawTemplateSelectSavesFiltersInsertsAndDeletes();
    selectedStyleEditsAreReflectedInTheCreationStyleContext();
    mixedColorsKeepUniformStyleButtonsActive();
    styleToolbarWidthTracksTheActiveTool();
    selectPopupPreservesModelFontRole();
    rectangleStyleUsesScreenshotCreationDefaults();
    shapeSelectorIsTheLeftmostStyleGroup();
    shapeSelectorIsExclusiveToTheShapeTool();
    arrowStyleUsesScreenshotCreationColorOverride();
    arrowRatioEditorAdjustsAndResets();
    arrowStyleControlsExposeAndEmitAllStyleProperties();
    lineStyleControlsExposeStraightAndCurveTypes();
    selectedArrowMixedPropertiesResolveIndependently();
    textStyleControlsExposeAndEmitAllRequestedProperties();
    serialNumberStyleControlsExposeAndEmitRequestedProperties();
    serialNumberInputCommitsEditsAndSupportsWheel();
    stylePopoverTriggersProvideMouseFeedback();
    cornerRadiusButtonsRestoreTheDefaultValue();
    selectedStrokeColorDragKeepsPickerIndicatorInSync();
    fillStyleButtonsFollowFillColorPickerTrigger();
    configurationDrivenStyleEditorsShareStructuralContracts();
    styleToolbarRowSpacingFollowsPhysicalScale();
    styleToolbarControlsDoNotEnterTabFocusChain();
    toolbarScalingDoesNotRelayoutPopupContent();
    popupColorEditorButtonsKeepPopupScaleAfterToolbarDpiCommit();
    selectToolExposesDedicatedActionToolbar();
    selectionResetRemainsAvailableWithoutSelection();
    selectionAlignmentActionsFollowSelectionUnitCount();
    secondaryToolbarsStartHiddenUntilTheirToolIsSelected();
    selectToolRemainsTheSoleOwnerOfItsSecondaryToolbar();
    crossTypeSelectionRecalculatesStyleToolbarSize();
    familiesHydratedAfterScaleKeepTheSamePhysicalSize();
    physicalScaleDefersHiddenStyleGroupGeometry();
    activeFilterAndWatermarkToolsExposeCanvasWheelSteps();
    screenshotProductStyleProfileIsComplete();
    resetStyleStateRestoresTheCompleteInjectedProfileWithoutCommands();
    textStylePopupLifecyclesAreBalanced();
    arrowheadOptionsRetranslateInPlace();
    arrowAndLineUseConfiguredPopoverGroup();
    tableQrEntrySelectionPersistsAcrossPaletteInstances();
    drawingColorsPreserveAlphaAcrossEditsAndToolSwitches();
    canvasToolStylesPersistIndependentlyWithoutGlobalStyles();
    return 0;
}
