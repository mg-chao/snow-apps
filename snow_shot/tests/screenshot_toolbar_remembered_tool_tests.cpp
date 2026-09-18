#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class RecordingToolbarCommands final : public ScreenshotToolbarCommandSink {
  public:
    void setMoveTool() override {
        ++moveToolCount;
    }
    void setSelectTool() override {
        ++selectToolCount;
    }
    void setShapeTool() override {
        ++shapeToolCount;
    }
    void setArrowTool() override {}
    void setLineTool() override {}
    void setFreeDrawTool() override {}
    void setHighlightTool() override {}
    void setPenHighlightTool() override {}
    void setEraserTool() override {}
    void setFilterTool() override {}
    void setWatermarkTool() override {}
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle&, quint32) override {}
    void setTextTool() override {}
    void setSerialNumberTool() override {}
    void setOcrTool() override {}
    void startScrollingScreenshot() override {}
    void pinSelectionToScreen() override {}
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&) override {}
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSerialNumbers() override {}
    void incrementSelectedSerialNumbers() override {}
    void createTextForSelectedSerialNumber() override {}
    void repositionToolbarForContentChange() override {}
    void hideColorPickersForScreenshotUi() override {}

    int moveToolCount = 0;
    int selectToolCount = 0;
    int shapeToolCount = 0;
};

void rememberedDrawingToolRestoresOncePerCapture() {
    using Tool = ScreenshotToolPalette::Tool;
    const snow_shot::storage::ScreenshotToolbarSettings toolbarSettings;
    const snow_shot::storage::DrawingSettings drawingSettings;
    const QString originalDrawingTool = toolbarSettings.lastDrawingTool();
    const bool originalRememberSwitch = drawingSettings.rememberLastUsedTool();
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(toolbarSettings.setLastDrawingTool(originalDrawingTool));
        static_cast<void>(drawingSettings.setRememberLastUsedTool(originalRememberSwitch));
    });
    require(drawingSettings.setRememberLastUsedTool(true) &&
                toolbarSettings.setLastDrawingTool(QStringLiteral("shape")),
            "remembered tool window tests must start from a remembered shape tool");

    RecordingToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.resetForNewCapture();
    require(window.palette() != nullptr && window.palette()->activeTool() == Tool::Move,
            "a new capture must reset the toolbar to the move tool");
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 1 && commands.moveToolCount == 0 &&
                window.palette()->activeTool() == Tool::Shape,
            "the first restore of a capture must activate the remembered drawing tool");
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 1,
            "repeated restore requests must not re-activate the remembered tool");
    window.resetForNewCapture();
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 2 && window.palette()->activeTool() == Tool::Shape,
            "the next capture must restore the remembered drawing tool again");

    // An explicit tool set supersedes the remembered restore for that capture.
    window.resetForNewCapture();
    window.setActiveTool(Tool::Select);
    require(commands.selectToolCount == 0 && commands.shapeToolCount == 2,
            "an explicit tool set must not emit tool commands by itself");
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 2 && window.palette()->activeTool() == Tool::Select,
            "an explicit tool set must cancel the pending remembered restore");

    // The switch disables the restore entirely.
    require(drawingSettings.setRememberLastUsedTool(false), "the switch must be writable");
    window.resetForNewCapture();
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 2 && window.palette()->activeTool() == Tool::Move,
            "a disabled switch must keep the move tool after a capture reset");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "failed to create toolbar test storage directory");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    static_cast<void>(
        applicationStorage.initialize({storageDirectory.filePath(QStringLiteral("bin")),
                                       storageDirectory.filePath(QStringLiteral("data")), 60000}));
    rememberedDrawingToolRestoresOncePerCapture();
    applicationStorage.shutdown();
    return 0;
}
