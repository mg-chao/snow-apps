#include "snow_shot/presentation/screenshotcolorpickerwidget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

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

void sampleRed(ScreenshotColorPickerWidget& picker) {
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    picker.setCaptureImage(image, image.rect());
    picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 0.0);
    require(picker.hasCurrentColor(), "picker must sample the capture image");
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

    const QStringList formats{QStringLiteral("hex"), QStringLiteral("rgb"), QStringLiteral("hsl"),
                              QStringLiteral("hex")};
    const QStringList colors{QStringLiteral("#FF0000"), QStringLiteral("rgb(255, 0, 0)"),
                             QStringLiteral("hsl(0, 100.0%, 50.0%)"), QStringLiteral("#FF0000")};
    for (qsizetype index = 0; index < formats.size(); ++index) {
        {
            ScreenshotColorPickerWidget picker;
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
        ScreenshotColorPickerWidget picker;
        sampleRed(picker);
        require(picker.currentColorText() == QStringLiteral("#FF0000") &&
                    settings.colorPickerFormat() == QStringLiteral("hex"),
                "invalid stored formats must recover to HEX");
    }
    applicationStorage.shutdown();
}

void formatSurvivesResetWithoutStorage() {
    ScreenshotColorPickerWidget picker;
    sampleRed(picker);
    picker.cycleColorFormat();
    picker.resetForNewCapture();
    sampleRed(picker);
    require(picker.currentColorText() == QStringLiteral("rgb(255, 0, 0)"),
            "format switching must remain usable when storage is unavailable");
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    formatPersistsAcrossCapturesAndRestarts();
    formatSurvivesResetWithoutStorage();
    return 0;
}
