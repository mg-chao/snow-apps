#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "../src/presentation/tools/screenshottoolpalettebuttons.h"

#include "antd_icons.h"
#include "widgets/select.h"

#include <QApplication>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QDir>
#include <QEnterEvent>
#include <QComboBox>
#include <QFrame>
#include <QFont>
#include <QGuiApplication>
#include <QGridLayout>
#include <QImage>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QString>
#include <QTemporaryDir>
#include <QSlider>
#include <QSpacerItem>
#include <QPushButton>
#include <QWheelEvent>
#include <QWidget>

#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/control_scale.h"
#include "widgets/input_line_edit.h"
#include "widgets/popover.h"
#include "widgets/radio.h"
#include "widgets/radio_button_group.h"
#include "widgets/select.h"
#include "widgets/slider.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

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
    require(palette.actionFamilyStateForTests(
                ScreenshotToolPalette::ActionFamily::TableRecognition) ==
                ScreenshotToolPalette::MaterializationState::Uninitialized &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotTableMergeButton")) ==
                    nullptr,
            "unrelated table controls must remain deferred");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Table);
    require(palette.actionFamilyStateForTests(
                ScreenshotToolPalette::ActionFamily::TableRecognition) ==
                ScreenshotToolPalette::MaterializationState::Ready &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotTableMergeButton")) !=
                    nullptr,
            "activating Table should materialize only its action family");
    require(palette.styleFamilyStateForTests(ScreenshotToolPalette::Tool::Shape) ==
                ScreenshotToolPalette::MaterializationState::Uninitialized &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls")) ==
                    nullptr,
            "switching to Table should evict the previous drawing style family");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    require(palette.actionFamilyStateForTests(
                ScreenshotToolPalette::ActionFamily::Selection) ==
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
    QWidget* firstShapeControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
    require(firstShapeControls != nullptr &&
                palette.actionFamilyStateForTests(
                    ScreenshotToolPalette::ActionFamily::Selection) ==
                    ScreenshotToolPalette::MaterializationState::Uninitialized &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotSelectionOpacitySlider")) ==
                    nullptr && qFuzzyCompare(palette.rectangleStyle().strokeWidth + 1.0, 18.0),
            "switching back to Shape should rebuild only its style family");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Line);
    require(firstShapeControls != nullptr && firstShapeControls->parentWidget() == palette.stylePanel() &&
                firstShapeControls->objectName() == QStringLiteral("screenshotLineStyleControls") &&
                palette.styleFamilyStateForTests(ScreenshotToolPalette::Tool::Shape) ==
                    ScreenshotToolPalette::MaterializationState::Ready,
            "switching within the shared drawing family should reuse its controls");
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
        require(dynamic_cast<StrokeWidthPreviewButton*>(picker->triggerContent()) != nullptr,
                "stroke-width trigger should reuse the shape preview button");
        pickers.append(picker);
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
    for (adqt::widgets::AdPopover* popover :
         palette.findChildren<adqt::widgets::AdPopover*>()) {
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
    const QPointF center = QRectF(trigger->rect()).center();
    QEnterEvent enter(center, center, QPointF(trigger->mapToGlobal(center.toPoint())));
    QCoreApplication::sendEvent(trigger, &enter);
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
        "Record screen",   "Pin to screen",    "Cancel screenshot", "Copy to clipboard",
    };

    requireControlsEnabled(palette, drawingTools, std::size(drawingTools),
                           true, "restricted tools should start enabled");
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
    requireControlsEnabled(palette, drawingTools, std::size(drawingTools),
                           true,
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
    options.separatorAfterArrow = true;
    options.enableStyleToolbar = false;
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;

    ScreenshotToolPalette palette(options);
    const QList<adqt::widgets::AdButton*> buttons = mainToolbarButtons(palette);
    const QStringList expected{
        QStringLiteral("Edit selection"),
        QStringLiteral("Select elements (V)"),
        QStringLiteral("Shape (1)"),
        QStringLiteral("Arrow (2)"),
        QStringLiteral("Pen (3, P)"),
        QStringLiteral("Highlighter Tool (4, H)"),
        QStringLiteral("Text (5, T)"),
        QStringLiteral("Serial number (6, N)"),
        QStringLiteral("Filter (7, F)"),
        QStringLiteral("Eraser (8, E)"),
        QStringLiteral("Watermark (9)"),
        QStringLiteral("Table recognition (Ctrl+X)"),
        QStringLiteral("Record screen (Ctrl+R)"),
        QStringLiteral("Pin to screen (Ctrl+F)"),
        QStringLiteral("Text recognition (Ctrl+D)"),
        QStringLiteral("Scrolling screenshot (L)"),
        QStringLiteral("Cancel screenshot (Esc)"),
        QStringLiteral("Copy to clipboard (Ctrl+C)"),
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
    auto* cancel = qobject_cast<adqt::widgets::AdButton*>(
        controlWithTooltip(palette, "Cancel screenshot"));
    auto* copy = qobject_cast<adqt::widgets::AdButton*>(
        controlWithTooltip(palette, "Copy to clipboard"));
    require(pin != nullptr && cancel != nullptr && copy != nullptr && undo != nullptr &&
                redo != nullptr && pin->toolTip() == QStringLiteral("Pin to screen (Ctrl+F)") &&
                cancel->toolTip() == QStringLiteral("Cancel screenshot (Esc)") &&
                copy->toolTip() == QStringLiteral("Copy to clipboard (Ctrl+C)") &&
                undo->toolTip() == QStringLiteral("Undo (Ctrl+Z)") &&
                redo->toolTip() == QStringLiteral("Redo (Ctrl+Y)"),
            "screenshot toolbar actions must show their configured shortcuts");

    const snow_shot::storage::ScreenshotShortcutSettings shortcutSettings;
    const QMap<QString, QStringList> originalShortcuts = shortcutSettings.allShortcuts();
    require(shortcutSettings.setShortcuts(QStringLiteral("pin_to_screen"),
                                          {QStringLiteral("Alt+F")}),
            "pin shortcut fixture must support a non-default mapping");
    require(shortcutSettings.shortcuts(QStringLiteral("pin_to_screen")) ==
                QStringList{QStringLiteral("Alt+F")},
            "pin shortcut fixture must expose the updated mapping immediately");
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&palette, &languageChange);
    require(pin->toolTip() == QStringLiteral("Pin to screen (Alt+F)"),
            "screenshot toolbar shortcuts must survive runtime retranslation");
    require(shortcutSettings.setAllShortcutsAtomic(originalShortcuts),
            "pin shortcut fixture must restore the original mapping");
    QCoreApplication::sendEvent(&palette, &languageChange);
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
         {QStringLiteral("highlighter")}}};

    ScreenshotToolPalette palette(options);
    palette.show();
    QCoreApplication::processEvents();
    const QList<adqt::widgets::AdButton*> drawingButtons = mainDrawingToolbarButtons(palette);
    require(drawingButtons.size() == 3,
            "each configured drawing position should occupy one live toolbar slot");
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
    materializeLazyPopover(secondTrigger);
    firstPopover->show();
    secondPopover->show();
    QCoreApplication::processEvents();
    auto* shapeOption = popoverButtonWithTooltip(firstPopover, "Shape");
    auto* lineOption = popoverButtonWithTooltip(firstPopover, "Line");
    auto* penOption = popoverButtonWithTooltip(firstPopover, "Pen");
    auto* arrowOption = popoverButtonWithTooltip(secondPopover, "Arrow");
    auto* spotlightOption = popoverButtonWithTooltip(secondPopover, "Spotlight");
    require(firstPopover != nullptr && secondPopover != nullptr && shapeOption != nullptr &&
                lineOption != nullptr && penOption != nullptr && arrowOption != nullptr &&
                spotlightOption != nullptr &&
                qobject_cast<QHBoxLayout*>(firstPopover->contentWidget()->layout()) != nullptr &&
                firstPopover->contentWidget()->layout()->indexOf(shapeOption) <
                    firstPopover->contentWidget()->layout()->indexOf(lineOption) &&
                firstPopover->contentWidget()->layout()->indexOf(lineOption) <
                    firstPopover->contentWidget()->layout()->indexOf(penOption) &&
                qobject_cast<QHBoxLayout*>(secondPopover->contentWidget()->layout()) != nullptr &&
                secondPopover->contentWidget()->layout()->indexOf(arrowOption) <
                    secondPopover->contentWidget()->layout()->indexOf(spotlightOption),
            "group popovers should present configured tools horizontally from main to top");

    int freeDrawRequests = 0;
    int spotlightRequests = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::freeDrawRequested,
                     [&freeDrawRequests]() { ++freeDrawRequests; });
    QObject::connect(&palette, &ScreenshotToolPalette::spotlightRequested,
                     [&spotlightRequests]() { ++spotlightRequests; });
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
    require(updatedDrawingButtons.size() == 4 && updatedPopover != nullptr &&
                updatedGroupTrigger->accessibleName() == QStringLiteral("Pen") &&
                popoverButtonWithTooltip(updatedPopover, "Pen") != nullptr &&
                popoverButtonWithTooltip(updatedPopover, "Highlighter Tool") != nullptr &&
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
    require(restoredDrawingButtons.size() == 5 &&
                std::any_of(restoredDrawingButtons.cbegin(), restoredDrawingButtons.cend(),
                            [](const adqt::widgets::AdButton* button) {
                                return button != nullptr &&
                                       button->property("screenshotToolbarPositionItems")
                                           .toStringList() ==
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
        {{QStringLiteral("line"), QStringLiteral("arrow")}}};

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

void tableQrPopoverSharesOneEntryAndRemembersTheSelectedMode() {
    require(snow_shot::storage::ScreenshotToolbarSettings().setTableQrTool(
                QStringLiteral("table")),
            "the shared recognition fixture should start in Table mode");
    ScreenshotToolPalette::Options options;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showTableTool = true;
    options.showQrTool = true;
    options.enableStyleToolbar = false;

    ScreenshotToolPalette palette(options);
    palette.contentSizeHint();
    const QList<adqt::widgets::AdButton*> mainButtons = mainToolbarButtons(palette);
    require(mainButtons.size() == 1,
            "Table and QR recognition should occupy one main toolbar slot");
    auto* trigger =
        palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotTableQrButton"));
    require(trigger == mainButtons.front() &&
                trigger->toolTip() == QStringLiteral("Table recognition (Ctrl+X)") &&
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

    palette.setQrBusy(true);
    require(trigger->busy() && qrOption->busy() && !tableOption->busy(),
            "QR loading should be visible on the shared trigger and QR option only");
    palette.setTableBusy(true);
    require(trigger->busy() && qrOption->busy() && tableOption->busy(),
            "concurrent recognition should preserve each option's independent busy state");
    palette.setQrBusy(false);
    palette.setTableBusy(false);

    palette.setQrEnabled(false);
    require(!qrOption->isEnabled() && tableOption->isEnabled() && trigger->isEnabled(),
            "disabling QR should keep the shared slot available for Table recognition");
    palette.setQrEnabled(true);
    tableOption->click();
    require(tableRequests == 2 && qrRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Table &&
                trigger->accessibleName() == QStringLiteral("Table recognition"),
            "choosing Table should restore the default shared trigger presentation");
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
    options.actions =
        ScreenshotToolPalette::CopyAction | ScreenshotToolPalette::ConfirmAction;

    ScreenshotToolPalette palette(options);
    auto* save = qobject_cast<adqt::widgets::AdButton*>(
        controlWithTooltip(palette, "Save as file"));
    auto* copy = qobject_cast<adqt::widgets::AdButton*>(
        controlWithTooltip(palette, "Copy to clipboard"));
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
                confirm->accentRole() == copy->accentRole() &&
                copy->iconRef().colors().isEmpty(),
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
    require(translationButton != nullptr,
            "text translation toolbar control should be present");
    translationButton->click();
    require(palette.activeToolForTests() == ScreenshotToolPalette::Tool::TextTranslation &&
                translationButton->buttonStyle() ==
                    adqt::widgets::AdButton::ButtonStyle::Solid &&
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
                modeButtons.size() == 2 && verticalButton != nullptr && horizontalButton != nullptr,
            "scrolling screenshot should expose two independent mode buttons");
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
    for (adqt::widgets::AdButton* button : modeButtons) {
        require(button->size() == QSize(48, 48) && button->iconSize() == QSize(36, 36),
                "scrolling mode buttons should retain their enlarged metrics after scaling");
    }

    palette.setScrollingRecognitionMode(ScreenshotScrollingRecognitionMode::Vertical);
    require(changes == 0, "setting the current scrolling mode should be a no-op");
    horizontalButton->click();
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
    palette.setScrollingScreenshotMode(true);
    controls = palette.findChild<QWidget*>(QStringLiteral("screenshotScrollingRecognitionMode"));
    verticalButton = controls != nullptr
                         ? controls->findChild<adqt::widgets::AdButton*>(
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

void ocrToolReplacesSelectionActionToolbarContents() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = true;
    options.showOcrTool = true;
    options.showHistoryActions = true;
    ScreenshotToolPalette palette(options);

    require(palette.actionPanel() != nullptr,
            "selection and OCR tools should expose an action panel");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    require(palette.findChild<QWidget*>(QStringLiteral("screenshotSelectionOpacityIcon")) != nullptr,
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
    adqt::widgets::AdButton* translate = buttonWithTooltip("Text Translation");
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
    require(sendToBack == nullptr && edit != nullptr && translate != nullptr &&
                reset != nullptr && settings != nullptr && undo != nullptr && redo != nullptr &&
                textSelects.size() == 2 && formattingSelect != nullptr &&
                punctuationSelect != nullptr,
            "the shared action panel should contain the OCR editing controls");
    require(undo->toolTip() == QStringLiteral("Undo (Ctrl+Z)") &&
                redo->toolTip() == QStringLiteral("Redo (Ctrl+Y)"),
            "toolbar history actions must show their configured shortcuts");
    require(!edit->isHidden() && !translate->isHidden() && !reset->isHidden() &&
                !settings->isHidden() && !textSelects.at(0)->isHidden() &&
                !textSelects.at(1)->isHidden(),
            "OCR mode should replace selection actions with text editing controls");
    QLayout* actionLayout = actionPanel->layout();
    require(actionLayout != nullptr && actionLayout->indexOf(edit) < actionLayout->indexOf(translate) &&
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
    require(controlWithTooltip(palette, "Corner radius (scroll to adjust)")->isHidden(),
            "Line should not expose Rectangle-only corner radius");

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
    require(controlWithTooltip(palette, "Corner radius (scroll to adjust)")->isHidden(),
            "Free Draw should hide Rectangle-only corner radius");

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
    auto* highlighterOption = popoverButtonWithTooltip(popover, "Highlighter Tool");
    auto* penHighlightOption = popoverButtonWithTooltip(popover, "Pen Highlighter Tool");
    auto* rectangleHighlightOption =
        popoverButtonWithTooltip(popover, "Rectangle Highlighter Tool");
    auto* spotlightOption = popoverButtonWithTooltip(popover, "Spotlight");
    require(trigger != nullptr &&
                trigger->accessibleName() == QStringLiteral("Highlighter Tool") &&
                popover != nullptr && content != nullptr &&
                content->objectName() == QStringLiteral("screenshotHighlightPopoverContent") &&
                qobject_cast<QHBoxLayout*>(content->layout()) != nullptr &&
                highlighterOption != nullptr && penHighlightOption == nullptr &&
                rectangleHighlightOption == nullptr &&
                spotlightOption != nullptr &&
                palette.findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotRectangleHighlightButton")) == nullptr &&
                content->layout()->indexOf(highlighterOption) <
                    content->layout()->indexOf(spotlightOption),
            "the live toolbar should expose one generic Highlighter Tool alongside Spotlight");

    palette.setActiveTool(ScreenshotToolPalette::Tool::PenHighlight);
    const QList<QWidget*> highlightModeSelectors =
        palette.findChildren<QWidget*>(QStringLiteral("screenshotHighlightModeSelector"));
    require(highlightModeSelectors.size() == 2,
            "rectangle and pen highlight style rows should retain their mode selector");
    for (QWidget* selector : highlightModeSelectors) {
        auto* group = selector == nullptr
                          ? nullptr
                          : selector->findChild<adqt::widgets::AdRadioButtonGroup*>();
        require(group != nullptr && group->buttons().size() == 2,
                "highlight style mode selectors should contain only rectangle and pen");
        const QStringList expectedModes{
            QStringLiteral("Pen Highlighter Tool"),
            QStringLiteral("Rectangle Highlighter Tool"),
        };
        for (int index = 0; index < expectedModes.size(); ++index) {
            require(group->buttons().at(index)->toolTip() == expectedModes.at(index),
                    "highlight style mode selectors should place the default pen mode first");
        }
        require(group->checkedId() == static_cast<int>(ScreenshotToolPalette::Tool::PenHighlight),
                "highlight style mode selectors should default to Pen Highlighter Tool");
    }
    QWidget* spotlightControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotSpotlightStyleControls"));
    require(spotlightControls == nullptr ||
                spotlightControls->findChild<QWidget*>(
                    QStringLiteral("screenshotHighlightModeSelector")) == nullptr,
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
            "the highlight style selector should expose Rectangle Highlighter Tool");
    rectangleHighlightMode->click();
    require(penRequests == 0 && rectangleRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::RectangleHighlight &&
                trigger->accessibleName() == QStringLiteral("Highlighter Tool") &&
                trigger->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                trigger->accentRole() == adqt::widgets::AdButton::AccentRole::Primary &&
                highlighterOption->buttonStyle() ==
                    adqt::widgets::AdButton::ButtonStyle::Solid,
            "Rectangle Highlighter Tool should remain style-selectable through the generic item");

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
    highlighterOption = popoverButtonWithTooltip(popover, "Highlighter Tool");
    spotlightOption = popoverButtonWithTooltip(popover, "Spotlight");
    require(trigger != nullptr &&
                trigger->accessibleName() == QStringLiteral("Highlighter Tool") &&
                trigger->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                highlighterOption != nullptr &&
                highlighterOption->buttonStyle() ==
                    adqt::widgets::AdButton::ButtonStyle::Solid,
            "toolbar rebuilds should preserve the generic entry while Rectangle mode is active");

    trigger->click();
    require(penRequests == 0 && rectangleRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Select &&
                trigger->accessibleName() == QStringLiteral("Highlighter Tool") &&
                highlighterOption->buttonStyle() ==
                    adqt::widgets::AdButton::ButtonStyle::Text,
            "clicking the active Highlighter Tool trigger should return to selection");

    spotlightOption->click();
    require(penRequests == 0 && rectangleRequests == 1 && spotlightRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::Spotlight &&
                trigger->accessibleName() == QStringLiteral("Spotlight") &&
                trigger->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                spotlightOption->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid,
            "the Spotlight option should replace and activate the shared trigger");

    highlighterOption->click();
    require(penRequests == 1 && rectangleRequests == 1 && spotlightRequests == 1 &&
                palette.activeToolForTests() == ScreenshotToolPalette::Tool::PenHighlight &&
                trigger->accessibleName() == QStringLiteral("Highlighter Tool") &&
                highlighterOption->buttonStyle() ==
                    adqt::widgets::AdButton::ButtonStyle::Solid,
            "the generic Highlighter Tool option should activate Pen Highlighter Tool");

    palette.setActiveTool(ScreenshotToolPalette::Tool::FreeDraw);
    require(trigger->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                highlighterOption->buttonStyle() ==
                    adqt::widgets::AdButton::ButtonStyle::Text &&
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

void filterToolExposesTypeAndIntensityControls() {
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
    require(initialFilterModeGroups.size() == 2,
            "Filter should initialize one mode selector in each style row");
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

    auto* typeSelect =
        palette.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenshotFilterTypeSelect"));
    auto* intensity = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotFilterIntensitySlider"));
    auto* intensityIcon =
        palette.findChild<QLabel*>(QStringLiteral("screenshotFilterIntensityIcon"));
    require(typeSelect != nullptr && intensity != nullptr && intensityIcon != nullptr,
            "Filter should expose type and intensity controls");
    require(!intensityIcon->pixmap().isNull(), "Filter intensity should display the blur icon");
    require(typeSelect->variant() == adqt::widgets::AdSelect::Variant::Borderless,
            "Filter type select should match the font-family select style");
    require(palette.findChild<QSlider*>(QStringLiteral("screenshotFilterOpacitySlider")) == nullptr,
            "Filter should not expose an opacity style editor");
    require(typeSelect->model() != nullptr && typeSelect->model()->rowCount() == 4,
            "Filter type select should only expose the four filter types");
    require(typeSelect->model()
                        ->index(0, 0)
                        .data(adqt::widgets::AdSelect::DefaultLabelRole)
                        .toString() == QStringLiteral("Mosaic") &&
                typeSelect->model()
                        ->index(0, 0)
                        .data(adqt::widgets::AdSelect::DefaultValueRole)
                        .toInt() == static_cast<int>(SnowCanvasFilterType::Mosaic),
            "Mosaic should be the first filter type");
    const auto filterTypeSortComparator = typeSelect->sortComparator();
    const adqt::widgets::AdSelect::Option mosaicFilter{
        static_cast<int>(SnowCanvasFilterType::Mosaic),
        QStringLiteral("Mosaic"),
    };
    const adqt::widgets::AdSelect::Option gaussianBlurFilter{
        static_cast<int>(SnowCanvasFilterType::GaussianBlur),
        QStringLiteral("Gaussian Blur"),
    };
    require(filterTypeSortComparator &&
                filterTypeSortComparator(mosaicFilter, gaussianBlurFilter) &&
                !filterTypeSortComparator(gaussianBlurFilter, mosaicFilter),
            "Filter type popup should keep Mosaic ahead of the other filter types");

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
    typeSelect->setCurrentData(3, adqt::widgets::AdSelect::DefaultValueRole);
    require(!intensity->isEnabled(), "Inversion should disable filter intensity");
    typeSelect->setCurrentData(0, adqt::widgets::AdSelect::DefaultValueRole);
    require(intensity->isEnabled(), "Mosaic should enable filter intensity");
    require(intensityIcon->pixmap().toImage() != disabledIntensityIcon,
            "filter intensity icon should brighten with its enabled slider");
    intensity->setValue(75);
    require(styleChangeCount >= 2 && lastProperties == SnowCanvasFilterStylePropertyStrength,
            "Filter intensity should emit its dedicated style property");

    SnowCanvasStyleToolbarState mixed;
    mixed.source = SnowCanvasStyleToolbarSource::SelectedFilter;
    mixed.filterStyleMixed = SnowCanvasFilterStylePropertyType;
    palette.setStyleToolbarState(mixed);
    require(typeSelect->currentIndex() == -1,
            "mixed Filter types should clear the filter type selection");
    require(!intensity->isHidden(), "filter intensity should always remain visible");
    require(intensity->isEnabled(), "mixed Filter types should keep filter intensity available");

    QList<adqt::widgets::AdRadioButtonGroup*> filterModeGroups;
    for (adqt::widgets::AdRadioButtonGroup* group :
         palette.findChildren<adqt::widgets::AdRadioButtonGroup*>()) {
        if (group != nullptr && group->button(rectangleFilterId) != nullptr &&
            group->button(penFilterId) != nullptr) {
            filterModeGroups.append(group);
        }
    }
    require(filterModeGroups.size() == 2,
            "Filter should expose one synchronized mode selector in each style row");
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
    for (adqt::widgets::AdRadioButtonGroup* group : filterModeGroups) {
        require(group->checkedId() == penFilterId,
                "both Filter mode selectors should follow Pen Filter activation");
    }

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
    require(rectangleControls != nullptr && rectangleControls->isHidden() &&
                penControls != nullptr && !penControls->isHidden(),
            "switching Filter modes should replace the visible style row");
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
            ScreenshotToolPalette::Tool::Filter,
            QStringLiteral("screenshotFilterIntensityIcon"),
            QStringLiteral("screenshotFilterIntensitySlider"));
        const CompactSliderSnapshot penFilter = compactSliderSnapshot(
            ScreenshotToolPalette::Tool::PenFilter,
            QStringLiteral("screenshotPenFilterIntensityIcon"),
            QStringLiteral("screenshotPenFilterIntensitySlider"));
        const CompactSliderSnapshot spotlight = compactSliderSnapshot(
            ScreenshotToolPalette::Tool::Spotlight,
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
    materializeLazyPopover(highlightTrigger);
    require(
        shapeButton->size() == QSize(32, 32) && popoverForTrigger(shapeButton) == nullptr &&
            arrowLineTrigger->size() == QSize(32, 32) &&
            highlightTrigger->size() == QSize(32, 32) && arrowLinePopover != nullptr &&
            highlightPopover != nullptr &&
            qobject_cast<QHBoxLayout*>(arrowLinePopover->contentWidget()->layout()) != nullptr &&
            qobject_cast<QHBoxLayout*>(highlightPopover->contentWidget()->layout()) != nullptr &&
            arrowLinePopover->contentWidget()->layout()->spacing() == 8 &&
            highlightPopover->contentWidget()->layout()->spacing() == 8,
        "group triggers should use toolbar metrics and horizontal eight-pixel popover spacing");
    const QList<adqt::widgets::AdButton*> popupButtons{
        popoverButtonWithTooltip(arrowLinePopover, "Arrow"),
        popoverButtonWithTooltip(arrowLinePopover, "Line"),
        popoverButtonWithTooltip(highlightPopover, "Highlighter Tool"),
        popoverButtonWithTooltip(highlightPopover, "Spotlight"),
    };
    require(std::all_of(popupButtons.cbegin(), popupButtons.cend(),
                        [](const auto* button) {
                            return button != nullptr && button->size() == QSize(32, 32);
                        }),
            "drawing group popup options should start at the popup reference size");
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
    require(std::all_of(popupButtons.cbegin(), popupButtons.cend(),
                        [](const auto* button) {
                            return button != nullptr && button->size() == QSize(32, 32);
                        }) &&
                arrowLinePopover->contentWidget()->layout()->spacing() == 8 &&
                highlightPopover->contentWidget()->layout()->spacing() == 8,
            "popup options should retain popup-owned metrics when the toolbar scales");
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
    require(spotlightControls != nullptr && spotlightControls->layout() != nullptr &&
                spotlightControls->layout()->itemAt(0) != nullptr &&
                spotlightControls->layout()->itemAt(0)->widget() == colorPicker,
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
    auto* angle = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkAngleEditor")));
    auto* gap = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkGapEditor")));
    auto* opacityIcon =
        palette.findChild<QLabel*>(QStringLiteral("screenshotWatermarkOpacityIcon"));
    auto* opacitySlider = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotWatermarkOpacitySlider"));
    require(controls != nullptr && colorPicker != nullptr && text != nullptr &&
                fontSize != nullptr && family != nullptr && angle != nullptr && gap != nullptr &&
                opacityIcon != nullptr && opacitySlider != nullptr,
            "Watermark should expose the shared style controls");
    require(colorPicker->accessibleName() == QStringLiteral("Watermark color") &&
                colorPicker->mode() == adqt::widgets::AdColorPicker::Mode::Solid &&
                colorPicker->trigger() == adqt::widgets::AdColorPicker::Trigger::Hover &&
                !colorPicker->triggerTextVisible() && !colorPicker->alphaChannelEnabled() &&
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
    require(text->placeholderText() == QStringLiteral("Watermark Text") &&
                text->accessibleName() == QStringLiteral("Watermark Text") &&
                text->controlSize() == adqt::widgets::AdLineEdit::ControlSize::Small &&
                text->variant() == adqt::widgets::AdLineEdit::Variant::Borderless,
            "Watermark text should use the borderless small AdLineEdit");
    require(fontSize->toolTip() == QStringLiteral("Current watermark font size") &&
                fontSize->accessibleDescription() == QStringLiteral("12px"),
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
                family->model() != nullptr && family->model()->rowCount() >= 2 &&
                family->model()->index(0, 0).data(adqt::widgets::AdSelect::DefaultLabelRole) ==
                    QStringLiteral("Default"),
            "Watermark font family should reuse the searchable text selector");
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
        "Watermark should remove all legacy widgets");

    QList<QFrame*> separators =
        controls->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly);
    separators.removeAll(opacityIcon);
    QLayout* layout = controls->layout();
    require(layout != nullptr && separators.size() == 3,
            "Watermark should contain three standard group separators");
    const int colorIndex = layout->indexOf(colorPicker);
    const int textIndex = layout->indexOf(text);
    const int fontSizeIndex = layout->indexOf(fontSize);
    const int familyIndex = layout->indexOf(family);
    const int angleIndex = layout->indexOf(angle);
    const int gapIndex = layout->indexOf(gap);
    const int opacityIconIndex = layout->indexOf(opacityIcon);
    const int opacitySliderIndex = layout->indexOf(opacitySlider);
    bool onlySpacingBetweenOpacityControls = opacityIconIndex < opacitySliderIndex;
    for (int index = opacityIconIndex + 1; index < opacitySliderIndex; ++index) {
        onlySpacingBetweenOpacityControls =
            onlySpacingBetweenOpacityControls && layout->itemAt(index) != nullptr &&
            layout->itemAt(index)->spacerItem() != nullptr;
    }
    require(colorIndex >= 0 && colorIndex < textIndex && textIndex < fontSizeIndex &&
                fontSizeIndex < familyIndex && familyIndex < angleIndex && angleIndex < gapIndex &&
                gapIndex < opacityIconIndex && onlySpacingBetweenOpacityControls &&
                layout->indexOf(separators.at(0)) > colorIndex &&
                layout->indexOf(separators.at(0)) < textIndex &&
                layout->indexOf(separators.at(1)) > familyIndex &&
                layout->indexOf(separators.at(1)) < angleIndex &&
                layout->indexOf(separators.at(2)) > gapIndex &&
                layout->indexOf(separators.at(2)) < opacityIconIndex,
            "Watermark controls should finish with the opacity editor");
    require(text->height() == 28 && fontSize->height() == 28 && angle->height() == 28 &&
                gap->height() == 28,
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
        auto* opacityIcon = palette.findChild<QLabel*>(
            QStringLiteral("screenshotWatermarkOpacityIcon"));
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
    require(text != nullptr && angle != nullptr, "watermark synchronization controls should exist");
    require(text->text() == QStringLiteral("THIRD") && angle->value() == 31,
            "toolbar should reflect the latest committed watermark configuration");

    require(canvas.undo(), "aggregated watermark text changes should be undoable");
    require(text->text() == QStringLiteral("FIRST") && angle->value() == 31,
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
    auto* angle = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkAngleEditor")));
    auto* gap = dynamic_cast<IconNumericValuePreviewButton*>(
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkGapEditor")));
    auto* opacitySlider = palette.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotWatermarkOpacitySlider"));
    QWidget* fontSize =
        palette.findChild<QWidget*>(QStringLiteral("screenshotWatermarkFontSizeSummaryButton"));
    require(text != nullptr && colorPicker != nullptr && family != nullptr && angle != nullptr &&
                gap != nullptr && fontSize != nullptr && opacitySlider != nullptr,
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
                fontSize != nullptr && family != nullptr && angle != nullptr && gap != nullptr &&
                std::all_of(fontSizeButtons.cbegin(), fontSizeButtons.cend(),
                            [](QWidget* button) { return button != nullptr; }),
            "watermark controls should be present for physical-scale coverage");

    const QList<QWidget*> controls{
        colorPicker, colorTrigger, text, fontSize, family, angle, gap,
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

    const auto expectedScaledSize = [toolbarCounterScale](const QSize& size) {
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
    const QList<QFrame*> separators =
        controls->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly);
    require(layout != nullptr && shapeGroup != nullptr && strokeColor != nullptr &&
                strokeWidth != nullptr && separators.size() == 3,
            "shape, color, width, and separators should be present");
    require(layout->indexOf(shapeGroup) < layout->indexOf(separators.at(0)) &&
                layout->indexOf(separators.at(0)) < layout->indexOf(strokeColor) &&
                layout->indexOf(strokeColor) < layout->indexOf(separators.at(1)) &&
                layout->indexOf(separators.at(1)) < layout->indexOf(strokeWidth),
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

    QWidget* shapeGroup = palette.findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup"));
    require(shapeGroup != nullptr, "shape selector should be present");

    require(!shapeGroup->isHidden(), "shape selector should be visible for the Shape tool");

    palette.setActiveTool(ScreenshotToolPalette::Tool::Line);
    require(shapeGroup->isHidden(), "shape selector should not be visible for the Line tool");
    auto* shapeSeparator =
        palette.findChild<QFrame*>(QStringLiteral("screenshotShapeStyleGroupSeparator"));
    require(shapeSeparator != nullptr && shapeSeparator->isHidden(),
            "Line should hide the separator following the hidden shape selector");

    palette.setActiveTool(ScreenshotToolPalette::Tool::FreeDraw);
    require(shapeGroup->isHidden(), "shape selector should not be visible for the Free Draw tool");
    require(shapeSeparator->isHidden(),
            "Free Draw should hide the separator following the hidden shape selector");

    SnowCanvasStyleToolbarState lineState;
    lineState.source = SnowCanvasStyleToolbarSource::DefaultLine;
    palette.setStyleToolbarState(lineState);
    require(shapeGroup->isHidden(),
            "line style synchronization should keep the shape selector hidden");

    SnowCanvasStyleToolbarState rectangleState;
    rectangleState.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    palette.setStyleToolbarState(rectangleState);
    require(!shapeGroup->isHidden(),
            "returning to the Shape tool should restore the shape selector");
    require(!shapeSeparator->isHidden(),
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
    require(separators.size() == 2,
            "arrow style controls should have separators before color and type");
    QWidget* arrowStrokeColorControl = controlWithAccessibleName(palette, "Arrow stroke color");
    QWidget* arrowStrokeWidthControl = controlWithTooltip(palette, "Current arrow stroke width");
    require(arrowStrokeColorControl != nullptr && arrowStrokeWidthControl != nullptr,
            "arrow stroke color and width controls should be present");
    require(
        arrowLayout->indexOf(arrowStrokeColorControl) < arrowLayout->indexOf(separators.at(0)) &&
            arrowLayout->indexOf(separators.at(0)) < arrowLayout->indexOf(arrowStrokeWidthControl),
        "arrow stroke color should be the leftmost style group");
    require(arrowLayout->indexOf(arrowStrokeWidthControl) <
                    arrowLayout->indexOf(separators.at(1)) &&
                arrowLayout->indexOf(separators.at(1)) < arrowLayout->indexOf(arrowTypeControls),
            "arrow stroke width should remain between color and arrow type");

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
        if (control != nullptr && control->toolTip() == QStringLiteral("End arrowhead Standard")) {
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
                             "Start arrowhead Triangle");
    require(styleChangeCount == 5, "start arrowhead should emit once");
    clickPopoverStyleControl(showPopoverForTrigger(endArrowheadControl),
                             "End arrowhead Diamond outline");

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
    auto* noneOption = popoverButtonWithTooltip(popover, "Start arrowhead None");
    require(noneOption != nullptr, "English arrowhead option should be present");

    require(languageManager.setLanguage(QStringLiteral("zh_CN")),
            "Simplified Chinese should load for arrowhead retranslation");
    QCoreApplication::processEvents();
    require(startTrigger->accessibleName() == QStringLiteral("\u8d77\u59cb\u7bad\u5934"),
            "arrowhead trigger should retranslate to Simplified Chinese");
    require(noneOption->toolTip() == QStringLiteral("\u8d77\u59cb\u7bad\u5934 \u65e0"),
            "open arrowhead option should retranslate to Simplified Chinese");

    require(languageManager.setLanguage(QStringLiteral("zh_TW")),
            "Traditional Chinese should load for arrowhead retranslation");
    QCoreApplication::processEvents();
    require(startTrigger->accessibleName() == QStringLiteral("\u8d77\u59cb\u7bad\u982d"),
            "arrowhead trigger should retranslate to Traditional Chinese");
    require(noneOption->toolTip() == QStringLiteral("\u8d77\u59cb\u7bad\u982d \u7121"),
            "open arrowhead option should retranslate to Traditional Chinese");

    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English should be restorable after arrowhead retranslation");
    QCoreApplication::processEvents();
    require(startTrigger->accessibleName() == QStringLiteral("Start arrowhead") &&
                noneOption->toolTip() == QStringLiteral("Start arrowhead None"),
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
        SnowCanvasShapeStylePropertyEndArrowhead | SnowCanvasShapeStylePropertyArrowType;
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

    adqt::widgets::AdPopover* startArrowheadPopover = showPopoverForTrigger(startArrowheadControl);
    adqt::widgets::AdPopover* endArrowheadPopover = showPopoverForTrigger(endArrowheadControl);
    require(arrowTypeGroup->checkedId() == -1 && !elbowArrowType->isChecked(),
            "mixed arrow type should not select a button-group option");
    for (const auto& option : {
             std::pair{startArrowheadPopover, "Start arrowhead Triangle"},
             std::pair{endArrowheadPopover, "End arrowhead Diamond"},
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
             std::pair{startArrowheadPopover, "Start arrowhead Triangle"},
             std::pair{endArrowheadPopover, "End arrowhead Diamond"},
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
    require(fontSelect->model() != nullptr && fontSelect->model()->rowCount() >= 2,
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
    require(textLayout != nullptr, "text style controls should have a layout");
    require(layoutWidgetIndex(textLayout, fillPicker) ==
                    layoutWidgetIndex(textLayout, strokePicker) + 1 &&
                hasOnlySpacingBetween(textLayout, strokePicker, fillPicker, 4),
            "text stroke color should have 4px spacing on its right");
    require(!colorPicker->alphaChannelEnabled() && !strokePicker->alphaChannelEnabled() &&
                fillPicker->alphaChannelEnabled(),
            "text color pickers should expose the requested alpha behavior");
    require(strokePicker->triggerContent() != nullptr &&
                strokePicker->triggerContent()->accessibleDescription() == QStringLiteral("0px"),
            "zero text stroke width should display as 0px");
    require(strokePicker->triggerContent()->cursor().shape() == Qt::SplitVCursor,
            "text stroke-width trigger should use the vertical split cursor");
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
    int changeCount = 0;
    QObject::connect(&palette, &ScreenshotToolPalette::textStyleChanged,
                     [&emittedStyle, &changeCount](const SnowCanvasTextStyle& style) {
                         emittedStyle = style;
                         ++changeCount;
                     });
    clickStyleControl(palette, "Text font size XL (54px)");
    require(changeCount == 1 && qFuzzyCompare(emittedStyle.fontSize + 1.0, 55.0),
            "XL text size should emit 54px once");
    auto* xlFontSizePreset = qobject_cast<adqt::widgets::AdButton*>(
        controlWithTooltip(palette, "Text font size XL (54px)"));
    require(xlFontSizePreset != nullptr &&
                xlFontSizePreset->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Tonal &&
                activeFontSizePreset->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text,
            "changing text size should move the shared style-toolbar active state to the new preset");
    clickStyleControl(palette, "Line text fill");
    require(changeCount == 2 && emittedStyle.fillStyle == SnowCanvasFillStyle::Line,
            "text fill pattern should update");
    QWidget* alignmentTrigger = controlWithAccessibleName(palette, "Text alignment");
    adqt::widgets::AdPopover* alignmentPopover = showPopoverForTrigger(alignmentTrigger);
    clickPopoverStyleControl(alignmentPopover, "Align text center");
    require(changeCount == 3 &&
                emittedStyle.horizontalAlign == SnowCanvasTextHorizontalAlign::Center,
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
    const auto* textColorPickerBeforeSwitch =
        colorPickerWithAccessibleName(palette, "Text color");
    const auto* textFillColorPickerBeforeSwitch =
        colorPickerWithAccessibleName(palette, "Text fill color");
    require(textColorPickerBeforeSwitch != nullptr && textFillColorPickerBeforeSwitch != nullptr,
            "text color editors should materialize before the family transition");
    const QSize textColorTriggerSize = textColorPickerBeforeSwitch->triggerContent() != nullptr
                                           ? textColorPickerBeforeSwitch->triggerContent()->sizeHint()
                                           : QSize();
    const QSize textFillTriggerSize = textFillColorPickerBeforeSwitch->triggerContent() != nullptr
                                          ? textFillColorPickerBeforeSwitch->triggerContent()->sizeHint()
                                          : QSize();
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
                !colorPicker->alphaChannelEnabled() && fillColorPicker->alphaChannelEnabled(),
            "sequence-number color should match text color alpha behavior");
    require(colorPicker->triggerContent() != nullptr &&
                colorPicker->triggerContent()->sizeHint() == textColorTriggerSize,
            "sequence-number color should match the text color trigger format");
    require(fillColorPicker->triggerContent() != nullptr && textFillTriggerSize.isValid() &&
                fillColorPicker->triggerContent()->sizeHint() == textFillTriggerSize,
            "sequence-number fill should align with the text fill editor");
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
    require(serialNumberLayout != nullptr && layoutWidgetIndex(serialNumberLayout, colorPicker) == 0,
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
        require(preset != nullptr && layoutWidgetIndex(serialNumberLayout, preset) == index + 1,
                "sequence-number color presets should match the text color format");
    }
    QWidget* numberEditor = controlWithTooltip(palette, "Sequence number (scroll to adjust)");
    require(numberEditor != nullptr && numberEditor->cursor().shape() == Qt::SplitVCursor,
            "sequence number should use the shared wheel-adjustable editor");
    const int numberEditorIndex = serialNumberLayout->indexOf(numberEditor);
    auto* fontSizeSummary = controlWithTooltip(palette, "Current sequence number font size");
    require(fontSizeSummary != nullptr &&
                fontSizeSummary->accessibleDescription() == QStringLiteral("30px"),
            "sequence-number font size should display the current value");
    require(numberEditorIndex >= 0 &&
                hasOnlySpacingBetween(serialNumberLayout, numberEditor, fontSizeSummary, 4),
            "sequence number should have 4px spacing on its right");
    auto* fontSelect = qobject_cast<adqt::widgets::AdSelect*>(
        controlWithAccessibleName(palette, "Sequence number font family"));
    require(fontSelect != nullptr && fontSelect->placeholder() == QStringLiteral("Font family") &&
                fontSelect->variant() == adqt::widgets::AdSelect::Variant::Borderless,
            "sequence-number font family should reuse the text selector");
    const QList<QFrame*> separators =
        controls->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly);
    require(separators.size() == 2 &&
                serialNumberLayout->indexOf(
                    controlWithTooltip(palette, "Sequence number color #000000")) <
                    serialNumberLayout->indexOf(separators.at(0)) &&
                serialNumberLayout->indexOf(separators.at(0)) < numberEditorIndex &&
                serialNumberLayout->indexOf(fontSelect) <
                    serialNumberLayout->indexOf(separators.at(1)) &&
                serialNumberLayout->indexOf(separators.at(1)) <
                    serialNumberLayout->indexOf(fillColorPicker) &&
                solidFill != nullptr && crossLineFill != nullptr && lineFill != nullptr &&
                layoutWidgetIndex(serialNumberLayout, solidFill) ==
                    layoutWidgetIndex(serialNumberLayout, fillColorPicker) + 1 &&
                layoutWidgetIndex(serialNumberLayout, crossLineFill) ==
                    layoutWidgetIndex(serialNumberLayout, fillColorPicker) + 2 &&
                layoutWidgetIndex(serialNumberLayout, lineFill) ==
                    layoutWidgetIndex(serialNumberLayout, fillColorPicker) + 3,
            "sequence-number color and fill groups should use separators");

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
    SnowCanvasWidget canvas;
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
        if (picker != nullptr && !picker->alphaChannelEnabled()) {
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
    require(qAbs(qRound(movingColor.saturationF() * 100.0) - 35) <= 1 &&
                qAbs(qRound(movingColor.valueF() * 100.0) - 35) <= 1,
            "stroke color indicator should follow the pointer before release");
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, localPosition, globalPosition,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(saturationPanel, &releaseEvent);
    QCoreApplication::processEvents();

    const QColor selectedColor = strokePicker->value().solidColor.toHsv();
    const int expectedSaturation = qRound(selectedColor.saturationF() * 100.0);
    const int expectedBrightness = qRound(selectedColor.valueF() * 100.0);
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
                filterSelect->model()->rowCount() == 4,
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
    require(radioContainers.size() == 6,
            "shape, arrow, highlight, and filter should expose six shared radio editors");
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
        auto* sampler = dynamic_cast<ColorPickerSamplerButton*>(picker->previewContent());
        require(sampler != nullptr && sampler->focusPolicy() == Qt::NoFocus &&
                    sampler->sizeClass() == adqt::widgets::AdButton::SizeClass::Small &&
                    sampler->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Outline &&
                    sampler->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral &&
                    sampler->toolTip() == QStringLiteral("Pick color from canvas") &&
                    sampler->accessibleName() == sampler->toolTip(),
                "canvas-color samplers should use the fill-color trigger's outlined style");
    }
    require(dynamic_cast<ColorSwatchButton*>(colorPicker->triggerContent()) != nullptr &&
                dynamic_cast<FillStylePreviewTrigger*>(fillPicker->triggerContent()) != nullptr &&
                dynamic_cast<StrokeStylePreviewTrigger*>(strokePicker->triggerContent()) !=
                    nullptr &&
                dynamic_cast<StrokeWidthPreviewButton*>(widthColorPicker->triggerContent()) !=
                    nullptr &&
                !colorPicker->alphaChannelEnabled() && fillPicker->alphaChannelEnabled() &&
                !strokePicker->alphaChannelEnabled() && !widthColorPicker->alphaChannelEnabled(),
            "picker configuration should preserve trigger and alpha differences");
    auto* fillTrigger = dynamic_cast<FillStylePreviewTrigger*>(fillPicker->triggerContent());
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
    }
    QLayout* startLayout = popoverForTrigger(startArrowhead)->contentWidget()->layout();
    QLayout* endLayout = popoverForTrigger(endArrowhead)->contentWidget()->layout();
    QLayout* alignmentLayout = popoverForTrigger(textAlignment)->contentWidget()->layout();
    require(
        qobject_cast<QGridLayout*>(startLayout) != nullptr &&
            qobject_cast<QGridLayout*>(endLayout) != nullptr && startLayout->count() == 13 &&
            endLayout->count() == 13 && qobject_cast<QHBoxLayout*>(alignmentLayout) != nullptr &&
            alignmentLayout->count() == 3 && startLayout->spacing() == alignmentLayout->spacing(),
        "icon-option configuration should preserve arrow grids and the alignment row");

    auto* sampler = dynamic_cast<ColorPickerSamplerButton*>(colorPicker->previewContent());
    palette.setActiveTool(ScreenshotToolPalette::Tool::RectangleHighlight);
    colorPicker = colorPickerWithAccessibleName(palette, "Highlight color");
    sampler = colorPicker != nullptr
                  ? dynamic_cast<ColorPickerSamplerButton*>(colorPicker->previewContent())
                  : nullptr;
    require(colorPicker != nullptr && sampler != nullptr,
            "the active highlight family should rebuild its color sampler on demand");
    adqt::widgets::AdColorPicker* samplingTarget = nullptr;
    QObject::connect(&palette, &ScreenshotToolPalette::canvasColorSamplingRequested, &palette,
                     [&samplingTarget](adqt::widgets::AdColorPicker* picker) {
                         samplingTarget = picker;
                     });
    colorPicker->setPopupVisible(true);
    require(colorPicker->popupVisible(), "color picker popup should open before sampling");
    sampler->click();
    QCoreApplication::processEvents();
    require(samplingTarget == colorPicker && !colorPicker->popupVisible(),
            "canvas sampler should close its picker and request sampling for the owning picker");
    int completedColorChanges = 0;
    QObject::connect(colorPicker, &adqt::widgets::AdColorPicker::editingFinished, &palette,
                     [&completedColorChanges](const adqt::widgets::AdColorValue&) {
                         ++completedColorChanges;
                     });
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
    QObject::connect(&palette, &ScreenshotToolPalette::canvasColorSamplingRequested, &palette,
                     [&samplingTarget](adqt::widgets::AdColorPicker* requested) {
                         samplingTarget = requested;
                     });
    picker->setPopupVisible(true);
    require(picker->popupVisible(), "color picker popup should open before sampling");
    sampler->click();
    QCoreApplication::processEvents();
    require(samplingTarget == picker && !picker->popupVisible(),
            "canvas sampler should close its picker and request the owning picker");

    int completedChanges = 0;
    QObject::connect(picker, &adqt::widgets::AdColorPicker::editingFinished, &palette,
                     [&completedChanges](const adqt::widgets::AdColorValue&) {
                         ++completedChanges;
                     });
    const QColor sampledColor(QStringLiteral("#123456"));
    picker->commitValue(adqt::widgets::AdColorValue::solid(sampledColor));
    require(completedChanges == 1 && picker->value().solidColor == sampledColor,
            "sampled colors should use completed picker change semantics");
}

void styleToolbarRowSpacingFollowsPhysicalScale() {
    ScreenshotToolPalette palette(ScreenshotToolPalette::Options{});
    const QVector<QPair<ScreenshotToolPalette::Tool, QString>> tools{
        {ScreenshotToolPalette::Tool::Shape,
         QStringLiteral("screenshotRectangleStyleControls")},
        {ScreenshotToolPalette::Tool::Arrow,
         QStringLiteral("screenshotArrowStyleControls")},
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
    const int referenceSpacing =
        activateAndGetSpacing(tools.first().first, tools.first().second);
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
    require(modeButtons.size() == 14,
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
    const auto pickerWithAccessibleName = [](ScreenshotToolPalette& palette,
                                             const QString& name) {
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
        adqt::widgets::AdPopover* strokePopover = openColorPicker(
            pickerWithAccessibleName(palette, QStringLiteral("Stroke color")));
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
        adqt::widgets::AdPopover* endArrowheadPopover =
            showPopoverForTrigger(endArrowheadTrigger);
        adqt::widgets::AdButton* startArrowhead =
            popoverButtonWithTooltip(startArrowheadPopover, "Start arrowhead None");
        adqt::widgets::AdButton* endArrowhead =
            popoverButtonWithTooltip(endArrowheadPopover, "End arrowhead None");
        require(arrowStrokeStyle != nullptr && arrowType != nullptr && startArrowhead != nullptr &&
                    endArrowhead != nullptr,
                "arrow toolbar and popup options should be present");

        const QSize referenceTriggerSize = startArrowheadTrigger->size();
        const QSize referenceArrowTypeSize = arrowType->size();
        const QSize referenceStrokeOptionSize = arrowStrokeStyle->size();
        const QSize referenceStartOptionSize = startArrowhead->size();
        const QSize referenceEndOptionSize = endArrowhead->size();
        const PopupContentLayoutSnapshot arrowStrokeSnapshot =
            snapshotPopover(arrowStrokePopover);
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
        require(spacing != nullptr,
                "highlight mode selector should have a dedicated trailing gap");
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
        QSpacerItem* spacing =
            controls != nullptr ? spacingAfter(controls->layout(), strokePicker) : nullptr;
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

    adqt::widgets::AdControlScaleScope scope(&palette);
    require(scope.publishScale(1.5, 1.0),
            "toolbar control scale should publish a mixed-DPI transition");
    require(palette.setPhysicalScale(1.5),
            "toolbar physical scale should follow the mixed-DPI transition");

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

    QWidget* textControls =
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

    require(!textControls->isHidden(),
            "selecting text with the sequence-number tool should show text controls");
    require(palette.stylePanel()->size() == expectedTextPanelSize,
            "style toolbar should use the selected text controls when recalculating its size");
    require(visibleContentChangeCount == 1,
            "cross-type selection should notify the toolbar host to resize");
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
    textControls =
        palette.findChild<QWidget*>(QStringLiteral("screenshotTextStyleControls"));
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
        {&defaults.line, transparent, red, 2.0, 1.0, SnowCanvasArrowType::Straight,
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
    require(defaults.serialNumber.number == 1 && defaults.serialNumber.color == red &&
                defaults.serialNumber.fill == transparent &&
                defaults.serialNumber.fillStyle == SnowCanvasFillStyle::Solid &&
                exact(defaults.serialNumber.fontSize, 24.0) &&
                defaults.serialNumber.fontFamily.isEmpty() &&
                exact(defaults.serialNumber.strokeWidth, 2.0) &&
                defaults.serialNumber.strokeStyle == SnowCanvasStrokeStyle::Solid &&
                exact(defaults.serialNumber.opacity, 1.0),
            "sequence-number defaults should match the Snow Shot product profile");
    require(defaults.watermark.color == QColor(0, 0, 0, 255) && defaults.watermark.text.isEmpty() &&
                exact(defaults.watermark.fontSize, 12.0) &&
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

void canvasToolStylesPersistIndependentlyWithoutGlobalStyles() {
    SnowCanvasStyleDefaults styles = snow_shot::presentation::screenshotCanvasStyleDefaults();
    styles.rectangle.stroke = QColor(1, 2, 3, 4);
    styles.rectangle.strokeWidth = 3.0;
    styles.arrow.stroke = QColor(5, 6, 7, 8);
    styles.arrow.strokeWidth = 4.0;
    styles.line.strokeWidth = 5.0;
    styles.freeDraw.strokeWidth = 6.0;
    styles.rectangleHighlight.fill = QColor(9, 10, 11, 12);
    styles.penHighlight.strokeWidth = 7.0;
    styles.rectangleFilter = {SnowCanvasFilterType::GaussianBlur, 0.25, 0.8, 8.0};
    styles.penFilter = {SnowCanvasFilterType::Inversion, 0.75, 0.6, 44.0};
    styles.text.color = QColor(13, 14, 15, 16);
    styles.text.fontFamily = QStringLiteral("Persisted text font");
    styles.text.fontSize = 36.0;
    styles.serialNumber.number = 9'007'199'254'740'993LL;
    styles.serialNumber.color = QColor(17, 18, 19, 20);
    styles.serialNumber.fontFamily = QStringLiteral("Persisted serial font");
    styles.watermark.text = QStringLiteral("must not persist");
    styles.watermark.opacity = 0.91;
    styles.spotlight.color = QColor(21, 22, 23, 24);
    styles.spotlight.opacity = 0.17;

    require(snow_shot::presentation::persistScreenshotCanvasToolStyles(styles),
            "canvas tool styles should be accepted by configuration storage");

    SnowCanvasStyleDefaults expected = styles;
    const SnowCanvasStyleDefaults globalDefaults =
        snow_shot::presentation::screenshotCanvasStyleDefaults();
    expected.watermark = globalDefaults.watermark;
    expected.spotlight = globalDefaults.spotlight;
    require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() == expected,
            "persisted tool styles should round-trip independently without global styles");

    const auto configuration =
        snow_shot::storage::ApplicationStorage::instance().configuration().snapshot();
    require(!configuration.contains(QStringLiteral("drawing/watermark_style")) &&
                !configuration.contains(QStringLiteral("drawing/spotlight_style")),
            "watermark and spotlight styles must not be added to persistent tool configuration");
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
    if (application.arguments().contains(QStringLiteral("--canvas-style-persistence-only"))) {
        canvasToolStylesPersistIndependentlyWithoutGlobalStyles();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--lazy-loading-only"))) {
        secondaryControlsMaterializeOnlyForTheRequestedFamily();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--stroke-editor-only"))) {
        textAndHighlightStrokeWidthTriggersUseSharedPreviewButton();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ocr-translation-only"))) {
        ocrToolReplacesSelectionActionToolbarContents();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--screenshot-actions-tooltips-only"))) {
        screenshotActionTooltipsUseConfiguredShortcuts();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--shortcut-tooltips-only"))) {
        screenshotActionTooltipsUseConfiguredShortcuts();
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
    if (application.arguments().contains(QStringLiteral("--toolbar-layout-only"))) {
        configurableToolbarLayoutSupportsArbitraryPopoverGroups();
        arrowAndLineUseConfiguredPopoverGroup();
        highlightVariantsUseConfiguredPopoverGroup();
        drawingToolbarGroupsUseToolbarPopoverMetrics();
        spotlightControlsMatchMaskConfigurationBehavior();
        return 0;
    }
    numericStrokeWidthPreviewUsesLineWithinPreviewBounds();
    secondaryControlsMaterializeOnlyForTheRequestedFamily();
    textAndHighlightStrokeWidthTriggersUseSharedPreviewButton();
    scrollingScreenshotKeepsDrawingToolsAvailable();
    recognitionToolsKeepDrawingToolsAvailable();
    scrollingScreenshotExposesAxisRecognitionModes();
    screenshotToolbarUsesCanonicalOrderAndSectionSeparators();
    configurableToolbarLayoutSupportsArbitraryPopoverGroups();
    ocrControlReflectsLoadingState();
    ocrToolReplacesSelectionActionToolbarContents();
    clickingActiveToolbarToolReturnsToSelect();
    tableToolExposesStructureActionsAndOwnHistoryState();
    tableQrPopoverSharesOneEntryAndRemembersTheSelectedMode();
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
    filterToolExposesTypeAndIntensityControls();
    filterStyleEditorsMatchShapeAndSpotlightMetrics();
    watermarkToolExposesSharedStyleControls();
    watermarkStyleEditorMatchesShapeHeight();
    watermarkAndTextToolsUseStandardSpacing();
    watermarkControlsFollowCommittedStateAndUndo();
    watermarkEditsCommitCompleteConfigsAndClampWheel();
    watermarkControlsFollowPhysicalScale();
    selectedStyleEditsAreReflectedInTheCreationStyleContext();
    mixedColorsKeepUniformStyleButtonsActive();
    styleToolbarWidthTracksTheActiveTool();
    selectPopupPreservesModelFontRole();
    rectangleStyleUsesScreenshotCreationDefaults();
    shapeSelectorIsTheLeftmostStyleGroup();
    shapeSelectorIsExclusiveToTheShapeTool();
    arrowStyleUsesScreenshotCreationColorOverride();
    arrowStyleControlsExposeAndEmitAllStyleProperties();
    selectedArrowMixedPropertiesResolveIndependently();
    textStyleControlsExposeAndEmitAllRequestedProperties();
    serialNumberStyleControlsExposeAndEmitRequestedProperties();
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
    secondaryToolbarsStartHiddenUntilTheirToolIsSelected();
    selectToolRemainsTheSoleOwnerOfItsSecondaryToolbar();
    crossTypeSelectionRecalculatesStyleToolbarSize();
    physicalScaleDefersHiddenStyleGroupGeometry();
    activeFilterAndWatermarkToolsExposeCanvasWheelSteps();
    screenshotProductStyleProfileIsComplete();
    resetStyleStateRestoresTheCompleteInjectedProfileWithoutCommands();
    textStylePopupLifecyclesAreBalanced();
    arrowheadOptionsRetranslateInPlace();
    arrowAndLineUseConfiguredPopoverGroup();
    tableQrEntrySelectionPersistsAcrossPaletteInstances();
    return 0;
}
