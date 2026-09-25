#include "snow_shot/presentation/screenshotcolorpickerwindow.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QWindow>
#include <QTranslator>

#ifdef Q_OS_MACOS
#include "../src/presentation/pinned/pinnedwindowplatform.h"
#import <AppKit/AppKit.h>
#endif

#include <cstdlib>
#include <iostream>

namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void sampleRed(ScreenshotColorPickerWindow& picker) {
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    picker.setCaptureImage(image, image.rect());
    picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 0.0);
    require(picker.hasCurrentColor(), "picker must sample the capture image");
}

class PickerUnitTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) == "ScreenshotSelectionToolbarWidget" && QByteArray(source) == "px")
            return QStringLiteral("pixels-translated");
        return {};
    }
};

void positionTextTracksFontAndLanguage() {
    ScreenshotColorPickerWindow picker;
    sampleRed(picker);
    const QString original = picker.currentPositionText();
    require(!original.contains(QLatin1Char('\n')), "short coordinates must fit on one line");
    QFont wide = picker.font();
    wide.setStretch(400);
    picker.setFont(wide);
    require(picker.currentPositionText() == original,
            "font changes must preserve single-line coordinate values");
    PickerUnitTranslator translator;
    require(QApplication::installTranslator(&translator), "picker translator must install");
    QApplication::processEvents();
    require(picker.currentPositionText().simplified() == original,
            "language changes must not add coordinate units to the magnifier");
    QApplication::removeTranslator(&translator);
    QApplication::processEvents();
    require(picker.currentPositionText().simplified() == original,
            "removing a translator must preserve the unit-free magnifier text");
}

void coordinateModePersistsAndRefreshesWithoutResampling() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    const storage::StorageInitializationOptions options{
        QDir(temporary.path()).filePath(QStringLiteral("bin")), temporary.path(), 60000};
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.initialize(options).success, "failed to initialize storage");
    const storage::ScreenshotUiSettings settings;
    require(settings.colorPickerCoordinateMode() == QStringLiteral("global"),
            "missing coordinate mode must default to global");
    const ScreenshotCoordinateDisplayValues values{QPointF(-100, 200),
                                                   ScreenshotSelectionDisplayUnit::PhysicalPixels,
                                                   false, QPointF(-3.6, 8.2)};
    const QString global = QStringLiteral("X: -100 Y: 200");
    const QString relative = QStringLiteral("X: -4 Y: 8");
    for (bool restoredRelative : {false, true}) {
        {
            ScreenshotColorPickerWindow picker;
            sampleRed(picker);
            picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 0.0, values);
            require(picker.currentPositionText() == (restoredRelative ? relative : global),
                    "picker must restore its coordinate preference after restart");
            require(!settings.setColorPickerCoordinateMode(QStringLiteral("invalid")),
                    "invalid coordinate modes must be rejected");
            picker.toggleCoordinateMode();
            require(
                picker.currentPositionText() == (restoredRelative ? global : relative) &&
                    picker.currentColorText() == QStringLiteral("#FF0000") &&
                    settings.colorPickerCoordinateMode() ==
                        (restoredRelative ? QStringLiteral("global") : QStringLiteral("relative")),
                "toggle must immediately change readout and preference without resampling");
            picker.resetForNewCapture();
            sampleRed(picker);
            picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 0.0, values);
            require(picker.currentPositionText() == (restoredRelative ? global : relative),
                    "new capture must retain coordinate mode");
            auto noSelection = values;
            noSelection.relativePosition.reset();
            picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 0.0, noSelection);
            require(picker.currentPositionText() == global,
                    "no selection must fall back to global coordinates");
            picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 0.0, values);
            require(picker.currentPositionText() == (restoredRelative ? global : relative),
                    "selection appearance must restore relative readout without another toggle");
        }
        require(applicationStorage.flushNow().success, "failed to flush coordinate preference");
        applicationStorage.shutdown();
        require(applicationStorage.initialize(options).success, "failed to reload storage");
    }
    require(settings.colorPickerCoordinateMode() == QStringLiteral("global"),
            "switching back to global must persist");
    applicationStorage.shutdown();
    ScreenshotColorPickerWindow picker;
    sampleRed(picker);
    picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 0.0, values);
    picker.toggleCoordinateMode();
    require(picker.currentPositionText() == relative, "toggle must work without storage");
}

void formatPersistsAcrossCapturesAndRestarts() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    const storage::StorageInitializationOptions options{
        QDir(temporary.path()).filePath(QStringLiteral("bin")), temporary.path(), 60000};
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.initialize(options).success, "failed to initialize storage");
    const storage::ScreenshotUiSettings settings;
    require(settings.colorPickerFormat() == QStringLiteral("hex"),
            "missing color format must default to HEX");

    const QStringList formats{QStringLiteral("hex"), QStringLiteral("hex_without_hash"),
                              QStringLiteral("rgb"), QStringLiteral("hsl"), QStringLiteral("hex")};
    const QStringList colors{QStringLiteral("#FF0000"), QStringLiteral("FF0000"),
                             QStringLiteral("rgb(255, 0, 0)"),
                             QStringLiteral("hsl(0, 100.0%, 50.0%)"), QStringLiteral("#FF0000")};
    for (qsizetype index = 0; index < formats.size(); ++index) {
        {
            ScreenshotColorPickerWindow picker;
            sampleRed(picker);
            require(picker.currentColorText() == colors.at(index),
                    "a recreated picker must restore the persisted color format");
            require(!settings.setColorPickerFormat(QStringLiteral("unsupported")) &&
                        settings.colorPickerFormat() == formats.at(index),
                    "invalid formats must not replace the stored preference");
            if (index + 1 < formats.size()) {
                picker.cycleColorFormat();
                require(picker.currentColorText() == colors.at(index + 1) &&
                            settings.colorPickerFormat() == formats.at(index + 1),
                        "cycling must immediately update the displayed and stored format");
                picker.resetForNewCapture();
                require(!picker.hasCurrentColor() && picker.currentColorText().isEmpty(),
                        "new captures must discard the previous sampled color");
                sampleRed(picker);
                require(picker.currentColorText() == colors.at(index + 1),
                        "new captures must retain the selected format");
            }
        }
        require(applicationStorage.flushNow().success, "failed to flush color preference");
        applicationStorage.shutdown();
        require(applicationStorage.initialize(options).success, "failed to reload storage");
    }
    applicationStorage.shutdown();

    QFile configuration(QDir(temporary.path()).filePath(QStringLiteral("config.json")));
    require(configuration.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "failed to write invalid format fixture");
    const QByteArray invalid = R"({"screenshot_ui":{"color_picker_format":"unsupported"}})";
    require(configuration.write(invalid) == invalid.size(), "failed to write format fixture");
    configuration.close();
    require(applicationStorage.initialize(options).success, "failed to reload invalid format");
    {
        ScreenshotColorPickerWindow picker;
        sampleRed(picker);
        require(picker.currentColorText() == QStringLiteral("#FF0000") &&
                    settings.colorPickerFormat() == QStringLiteral("hex"),
                "invalid stored formats must recover to HEX");
    }
    applicationStorage.shutdown();
}

class PickerPaintCounter final : public QObject {
  public:
    int count = 0;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint)
            ++count;
        return false;
    }
};

void selectionUnitPersistsAndCoordinatesDoNotResample() {
    using Unit = ScreenshotSelectionDisplayUnit;
    QTemporaryDir temporary;
    require(temporary.isValid(), "unit settings require an isolated directory");
    const storage::StorageInitializationOptions options{temporary.filePath(QStringLiteral("bin")),
                                                        temporary.path(), 60000};
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize(options).success, "unit storage initialization failed");
    const storage::ScreenshotUiSettings settings;
#ifdef Q_OS_MACOS
    const QString defaultId = QStringLiteral("logical_pixels");
#else
    const QString defaultId = QStringLiteral("physical_pixels");
#endif
    require(settings.selectionDisplayUnit() == defaultId &&
                screenshotSelectionDisplayUnitId(kDefaultScreenshotSelectionDisplayUnit) ==
                    defaultId,
            "unit defaults must match the platform");
    for (const auto unit : {Unit::LogicalPixels, Unit::PhysicalPixels}) {
        const auto id = screenshotSelectionDisplayUnitId(unit);
        require(settings.setSelectionDisplayUnit(id), "valid unit preference must be accepted");
        require(!settings.setSelectionDisplayUnit(QStringLiteral("unknown")) &&
                    settings.selectionDisplayUnit() == id,
                "invalid units must preserve the accepted preference");
        require(appStorage.flushNow().success, "unit preference must flush");
        appStorage.shutdown();
        require(appStorage.initialize(options).success && settings.selectionDisplayUnit() == id,
                "unit preference must survive application restart");
    }
    appStorage.shutdown();
    QFile configuration(temporary.filePath(QStringLiteral("config.json")));
    require(configuration.open(QIODevice::ReadOnly), "read the versioned unit fixture");
    auto document = QJsonDocument::fromJson(configuration.readAll()).object();
    configuration.close();
    auto ui = document.value(QStringLiteral("screenshot_ui")).toObject();
    ui.insert(QStringLiteral("selection_display_unit"), QStringLiteral("unknown"));
    document.insert(QStringLiteral("screenshot_ui"), ui);
    require(configuration.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "invalid-unit fixture must open");
    const QByteArray invalid = QJsonDocument(document).toJson();
    require(configuration.write(invalid) == invalid.size(), "invalid-unit fixture must write");
    configuration.close();
    require(appStorage.initialize(options).success && settings.selectionDisplayUnit() == defaultId,
            "invalid saved units must recover to the platform default");

    ScreenshotColorPickerWindow picker;
    sampleRed(picker);
    const QString color = picker.currentColorText();
    const ScreenshotCoordinateDisplayValues logical{QPointF(-80.8, 40), Unit::LogicalPixels, false};
    picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 0.0, logical);
    require(picker.currentPositionText().replace(QLatin1Char('\n'), QLatin1Char(' ')) ==
                    QStringLiteral("X: -81 Y: 40") &&
                picker.currentColorText() == color,
            "changing units at the same sample must update coordinates without changing the color");
    picker.updatePicker(
        QPoint(8, 8), QPointF(8, 8), 0.0,
        ScreenshotCoordinateDisplayValues{QPointF(-101, 50), Unit::PhysicalPixels, false});
    require(picker.currentPositionText().replace(QLatin1Char('\n'), QLatin1Char(' ')) ==
                    QStringLiteral("X: -101 Y: 50") &&
                picker.currentColorText() == color,
            "physical coordinates must format as integers and preserve the sample");
    picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 1.0, logical);
    QApplication::processEvents();
    PickerPaintCounter paints;
    picker.installEventFilter(&paints);
    QFont wideCoordinates = picker.font();
    wideCoordinates.setStretch(150);
    picker.setFont(wideCoordinates);
    picker.updatePicker(
        QPoint(8, 8), QPointF(8, 8), 1.0,
        ScreenshotCoordinateDisplayValues{QPointF(-12345.6, -98765.4), Unit::LogicalPixels, false});
    QApplication::processEvents();
    require(paints.count > 0,
            "coordinate changes must repaint even when the sampled pixel is unchanged");
    const QString positionText = picker.currentPositionText();
    require(positionText == QStringLiteral("X: -12346 Y: -98765"),
            "long coordinates must retain both rounded values on one line before painting");
    QFont textFont = picker.font();
    textFont.setPixelSize(13);
    require(QFontMetrics(textFont).horizontalAdvance(positionText) > 132,
            "long coordinate fixture must exercise horizontal overflow");
    require(picker.sizeHint() == QSize(160, 214) && picker.size() == picker.sizeHint(),
            "long coordinates must preserve the main-branch compact picker dimensions");
    require(QFontMetrics(textFont).height() <= 20,
            "single-line coordinate text must fit the compact text area vertically");
    QApplication::processEvents();
    paints.count = 0;
    picker.updatePicker(
        QPoint(8, 8), QPointF(8, 8), 1.0,
        ScreenshotCoordinateDisplayValues{QPointF(-12345.8, -98765.1), Unit::LogicalPixels, false});
    QApplication::processEvents();
    require(paints.count == 0 && picker.currentPositionText() == positionText,
            "subpixel changes with identical rounded coordinates must not repaint the picker");
    picker.removeEventFilter(&paints);
    picker.resetForNewCapture();
    sampleRed(picker);
    picker.updatePicker(
        QPoint(8, 8), QPointF(8, 8), 0.0,
        ScreenshotCoordinateDisplayValues{QPointF(-0.04, 4.666), Unit::LogicalPixels, true});
    require(picker.currentPositionText().replace(QLatin1Char('\n'), QLatin1Char(' ')) ==
                QStringLiteral("X: 0 Y: 5"),
            "point coordinates must use common rounding after a capture reset");
    appStorage.shutdown();
}

void formatSurvivesResetWithoutStorage() {
    ScreenshotColorPickerWindow picker;
    sampleRed(picker);
    picker.cycleColorFormat();
    picker.resetForNewCapture();
    sampleRed(picker);
    require(picker.currentColorText() == QStringLiteral("FF0000"),
            "format switching must remain usable when storage is unavailable");
}

void plainHexPreservesSixUppercaseDigits() {
    ScreenshotColorPickerWindow picker;
    picker.cycleColorFormat();
    const QList<QColor> colors{QColor(0, 0, 0), QColor(0, 10, 188), QColor(255, 255, 255)};
    const QStringList expected{QStringLiteral("000000"), QStringLiteral("000ABC"),
                               QStringLiteral("FFFFFF")};
    for (qsizetype index = 0; index < colors.size(); ++index) {
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(colors.at(index));
        picker.setCaptureImage(image, image.rect());
        picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 0.0);
        require(picker.currentColorText() == expected.at(index),
                "plain HEX must preserve leading zeros and uppercase digits without a hash");
    }
}

void pickerUsesASeparateClickThroughWindow() {
#ifdef Q_OS_MACOS
    // The application prewarms pinned shells before they have a native surface.
    // Keep their application event filter active throughout capture presentation.
    QWidget prewarmedPin;
    auto pinPlatform = snow_shot::presentation::createPinnedWindowPlatform(&prewarmedPin);
    require(prewarmedPin.windowHandle() == nullptr,
            "the prewarmed pin must have no native surface");
#endif
    QWidget owner;
    owner.setGeometry(320, 180, 800, 600);
    QWidget canvas(&owner);
    canvas.setGeometry(owner.rect());
    canvas.setMouseTracking(true);
    owner.show();
#ifdef Q_OS_MACOS
    require(owner.findChild<QObject*>(QStringLiteral("snowPinnedWindowPlatform"),
                                      Qt::FindDirectChildrenOnly) == nullptr,
            "a prewarmed pin must not adopt the unrelated screenshot overlay");
#endif

    ScreenshotColorPickerWindow picker;
    picker.setOwnerWindow(&owner);
    require(!canvas.testAttribute(Qt::WA_NativeWindow) && canvas.internalWinId() == 0,
            "preparing the picker must not turn the screenshot canvas into a native child window");
    require(picker.isWindow() && picker.parentWidget() == &owner,
            "the display color picker must be a separate window owned by its overlay");
    require(picker.windowFlags().testFlag(Qt::Tool) &&
                picker.windowFlags().testFlag(Qt::FramelessWindowHint) &&
                picker.windowFlags().testFlag(Qt::WindowStaysOnTopHint) &&
                picker.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus) &&
                picker.windowFlags().testFlag(Qt::WindowTransparentForInput) &&
                picker.windowFlags().testFlag(Qt::NoDropShadowWindowHint),
            "the display color picker window must stay topmost without taking focus, input, "
            "or a native shadow around its painted shadow");
    require(picker.windowHandle() != nullptr && owner.windowHandle() != nullptr &&
                picker.windowHandle()->transientParent() == owner.windowHandle(),
            "the display color picker window must use the overlay as its native stacking owner");

    QImage image(owner.size(), QImage::Format_RGBA8888);
    image.fill(Qt::blue);
    const QPoint localCursor(100, 100);
    picker.setCaptureImage(image, image.rect());
    picker.updatePicker(localCursor, localCursor, 1.0);
    QApplication::processEvents();

    const QPoint globalCursor = owner.mapToGlobal(localCursor);
    require(picker.isVisible() && picker.pos().x() > globalCursor.x() &&
                picker.pos().y() > globalCursor.y(),
            "the separate picker window must position itself from overlay-local to global space");
    const QImage painted = picker.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    require(qAlpha(painted.pixel(0, 0)) == 0,
            "the color picker's outer shadow margin must stay transparent");
    bool hasPaintedShadow = false;
    const int shadowSampleY = painted.height() / 2;
    for (int x = 1; x < 10 && x < painted.width(); ++x) {
        const int alpha = qAlpha(painted.pixel(x, shadowSampleY));
        hasPaintedShadow |= alpha > 0 && alpha < 255;
    }
    require(hasPaintedShadow, "disabling the native shadow must preserve the painted shadow");
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        NSWindow* nativeWindow = reinterpret_cast<NSView*>(picker.winId()).window;
        NSWindow* ownerWindow = reinterpret_cast<NSView*>(owner.winId()).window;
        require(nativeWindow != nil && !nativeWindow.hasShadow,
                "Cocoa must not outline the color picker's painted shadow");
        require(nativeWindow.ignoresMouseEvents,
                "the color picker window must ignore mouse events on macOS");
        // Screenshot surfaces use the screensaver level. An unmarked transient
        // tool is stacked three levels above its overlay.
        const NSInteger overlayLevel = CGWindowLevelForKey(kCGScreenSaverWindowLevelKey);
        ownerWindow.level = overlayLevel;
        nativeWindow.level = overlayLevel + 3;
        [NSApp activate];
        [ownerWindow orderFrontRegardless];
        [nativeWindow orderFrontRegardless];
        QApplication::processEvents();
        const auto ownerReceivesMouseDown = [&](const QPoint& point) {
            const NSPoint cocoaPoint =
                NSMakePoint(point.x(), NSMaxY(NSScreen.screens.firstObject.frame) - point.y());
            return [NSWindow windowNumberAtPoint:cocoaPoint belowWindowWithWindowNumber:0] ==
                   ownerWindow.windowNumber;
        };
        require(ownerReceivesMouseDown(picker.mapToGlobal(picker.rect().center())),
                "mouse events over the color picker panel must reach the screenshot window");
        require(ownerReceivesMouseDown(picker.mapToGlobal(QPoint(2, picker.height() / 2))),
                "mouse events over the painted shadow must reach the screenshot window");
        require(ownerReceivesMouseDown(picker.mapToGlobal(QPoint(-6, picker.height() / 2))),
                "mouse events beside the color picker must reach the screenshot window");
    }
#endif
}

void pickerPreparationPreservesCanvasInputSurfaces() {
    QWidget firstOwner;
    QWidget secondOwner;
    QWidget firstCanvas(&firstOwner);
    QWidget secondCanvas(&secondOwner);
    firstOwner.resize(800, 600);
    secondOwner.resize(800, 600);
    const auto requireNonNativeCanvases = [&]() {
        require(!firstCanvas.testAttribute(Qt::WA_NativeWindow) &&
                    !secondCanvas.testAttribute(Qt::WA_NativeWindow) &&
                    firstCanvas.internalWinId() == 0 && secondCanvas.internalWinId() == 0,
                "picker preparation, reveal, and reparenting must preserve canvas input surfaces");
    };

    // Exercise both direct ownership and preparing a surface before attaching it.
    for (bool prepareBeforeOwner : {false, true}) {
        ScreenshotColorPickerWindow picker(prepareBeforeOwner ? nullptr : &firstOwner);
        picker.prepareNativeSurface();
        require(picker.internalWinId() != 0 && !picker.isVisible(),
                "preparation must create a hidden top-level surface");
        requireNonNativeCanvases();
        for (QWidget* owner : {&firstOwner, &secondOwner, &firstOwner}) {
            picker.setOwnerWindow(owner);
            picker.prepareNativeSurface();
            picker.prepareNativeSurface();
            requireNonNativeCanvases();
            QImage image(owner->size(), QImage::Format_RGBA8888);
            image.fill(Qt::red);
            picker.setCaptureImage(image, image.rect());
            picker.updatePicker(QPoint(100, 100), QPointF(100, 100), 1.0);
            QApplication::processEvents();
            require(picker.isVisible() &&
                        picker.windowHandle()->transientParent() == owner->windowHandle(),
                    "the picker must retain its separate visible surface and stacking owner");
            requireNonNativeCanvases();
            picker.resetForNewCapture();
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    const int fontId =
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
    require(fontId >= 0, "offscreen picker tests require the Windows UI font");
    application.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).first()));
#endif
    positionTextTracksFontAndLanguage();
    selectionUnitPersistsAndCoordinatesDoNotResample();
    coordinateModePersistsAndRefreshesWithoutResampling();
    formatPersistsAcrossCapturesAndRestarts();
    formatSurvivesResetWithoutStorage();
    plainHexPreservesSixUppercaseDigits();
    pickerUsesASeparateClickThroughWindow();
    pickerPreparationPreservesCanvasInputSurfaces();
    return 0;
}
