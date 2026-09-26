#include "snow_shot/presentation/fullscreencanvasstylepanel.h"

#include "screenshottoolpalettebuttons.h"
#include "screenshottoolpalettestylecomponents.h"
#include "screenshottoolpalettestylecontrols.h"
#include "screenshottoolpalettestylepresets.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include "antd_icons.h"
#include "widgets/color_picker.h"
#include "widgets/input_number.h"
#include "widgets/modal.h"
#include "widgets/popover.h"
#include "widgets/radio_button_group.h"
#include "widgets/select.h"
#include "widgets/slider.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <optional>

namespace snow_shot::presentation {
namespace {
using PaletteTool = ScreenshotToolPalette::Tool;
using Presentation = snow_shot::presentation::ScreenshotToolPaletteStylePresentation;
namespace custom_icons = snow_shot::presentation::icons::custom::outlined;
namespace outlined_icons = adqt::icons::antd::outlined;
constexpr ScreenshotToolPaletteButtonMetrics kMetrics{32, 16, 1.0};

std::optional<PaletteTool> familyForTool(SnowCanvasTool tool) {
    switch (tool) {
    case SnowCanvasTool::Shape:
        return PaletteTool::Shape;
    case SnowCanvasTool::Arrow:
        return PaletteTool::Arrow;
    case SnowCanvasTool::Line:
        return PaletteTool::Line;
    case SnowCanvasTool::FreeDraw:
        return PaletteTool::FreeDraw;
    case SnowCanvasTool::RectangleHighlight:
        return PaletteTool::RectangleHighlight;
    case SnowCanvasTool::PenHighlight:
        return PaletteTool::PenHighlight;
    case SnowCanvasTool::Text:
        return PaletteTool::Text;
    case SnowCanvasTool::SerialNumber:
        return PaletteTool::SerialNumber;
    case SnowCanvasTool::Watermark:
        return PaletteTool::Watermark;
    case SnowCanvasTool::Spotlight:
        return PaletteTool::Spotlight;
    default:
        return std::nullopt;
    }
}

std::optional<PaletteTool> selectedFamily(const SnowCanvasStyleToolbarState& state) {
    if (state.selectedElementCount == 0)
        return std::nullopt;
    switch (state.source) {
    case SnowCanvasStyleToolbarSource::SelectedRectangle:
        return PaletteTool::Shape;
    case SnowCanvasStyleToolbarSource::SelectedArrow:
        return PaletteTool::Arrow;
    case SnowCanvasStyleToolbarSource::SelectedLine:
        return PaletteTool::Line;
    case SnowCanvasStyleToolbarSource::SelectedFreeDraw:
        return PaletteTool::FreeDraw;
    case SnowCanvasStyleToolbarSource::SelectedRectangleHighlight:
        return PaletteTool::RectangleHighlight;
    case SnowCanvasStyleToolbarSource::SelectedPenHighlight:
        return PaletteTool::PenHighlight;
    case SnowCanvasStyleToolbarSource::SelectedText:
        return PaletteTool::Text;
    case SnowCanvasStyleToolbarSource::SelectedSerialNumber:
        return PaletteTool::SerialNumber;
    case SnowCanvasStyleToolbarSource::SelectedSpotlight:
        return PaletteTool::Spotlight;
    default:
        return std::nullopt;
    }
}

const char* editorLabel(const QByteArray& role) {
    struct Label {
        const char* role;
        const char* text;
    };
    static const Label labels[]{
        {"outline-stroke", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Stroke")},
        {"outline-width", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Stroke width")},
        {"shape-fill", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Fill")},
        {"foreground-color", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Color")},
        {"highlight-color", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Color")},
        {"highlight-border", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Border")},
        {"brush-width", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Stroke width")},
        {"mask-color", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Mask color")},
        {"opacity", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Opacity")},
        {"text-font", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Font")},
        {"text-fill", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Fill")},
        {"text-stroke", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Border")},
        {"text-alignment", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Text alignment")},
        {"corner-radius", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Corner radius")},
        {"shape-kind", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Shape")},
        {"arrow-type", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Arrow type")},
        {"arrow-ratio", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Arrow ratio")},
        {"arrow-shaft-type", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Arrow shaft")},
        {"start-arrowhead", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Start arrowhead")},
        {"end-arrowhead", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "End arrowhead")},
        {"line-type", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Line type")},
        {"serial-value", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Number")},
        {"serial-type", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Number shape")},
        {"watermark-text", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Watermark text")},
        {"watermark-font", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Font")},
        {"watermark-template", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Template")},
        {"angle", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Angle")},
        {"gap", QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Spacing")},
    };
    for (const auto& label : labels) {
        if (role == label.role)
            return label.text;
    }
    return nullptr;
}
} // namespace

struct FullscreenCanvasStylePanel::Impl {
    FullscreenCanvasStylePanel& owner;
    SnowCanvasWidget& canvas;
    std::unique_ptr<ScreenshotToolPaletteStyleControls> styles;
    QScrollArea* scroll = nullptr;
    QWidget* body = nullptr;
    QVBoxLayout* bodyLayout = nullptr;
    QWidget* familyRoot = nullptr;
    QWidget* selection = nullptr;
    ScreenshotToolPaletteSliderEditor selectionOpacity;
    QVector<QPair<adqt::widgets::AdButton*, int>> selectionButtons;
    std::optional<PaletteTool> family;
    SnowCanvasTool activeTool = SnowCanvasTool::Select;
    SnowCanvasTool displayedTool = SnowCanvasTool::Select;
    bool laser = false;
    bool displayingLaser = false;
    bool rebuilding = false;
    bool initialized = false;
    QColor laserColor{Qt::red};
    qreal laserWidth = 4.0;
    int laserDuration = 1000;
    snow_shot::presentation::ScreenshotToolPaletteColorEditor laserColorEditor;
    adqt::widgets::AdInputNumber* laserWidthInput = nullptr;
    adqt::widgets::AdInputNumber* laserDurationInput = nullptr;

    explicit Impl(FullscreenCanvasStylePanel& panel, SnowCanvasWidget& widget,
                  const SnowCanvasStyleDefaults& defaults)
        : owner(panel), canvas(widget) {
        ScreenshotToolPaletteStyleControlCallbacks callbacks;
        callbacks.shapeStyleChanged = [this](const SnowCanvasShapeStyle& style, quint32 properties,
                                             SnowCanvasShapeKind kind) {
            canvas.setCanvasShapeStylePatch(style, properties, kind);
            persist();
        };
        callbacks.textStyleChanged = [this](const SnowCanvasTextStyle& style) {
            canvas.setCanvasTextStyle(style);
            persist();
        };
        callbacks.serialNumberStyleChanged = [this](const SnowCanvasSerialNumberStyle& style) {
            canvas.setCanvasSerialNumberStyle(style);
            persist();
        };
        callbacks.serialNumberDecrementRequested = [this]() {
            canvas.adjustSelectedSerialNumbers(-1);
        };
        callbacks.serialNumberIncrementRequested = [this]() {
            canvas.adjustSelectedSerialNumbers(1);
        };
        callbacks.serialNumberCreateTextRequested = [this]() { canvas.createSerialNumberText(); };
        callbacks.watermarkConfigChanged = [this](const SnowCanvasWatermarkConfig& config) {
            canvas.setCanvasWatermarkConfig(config);
            persist();
        };
        callbacks.watermarkPreviewChanged = [this](const SnowCanvasWatermarkConfig& config) {
            canvas.previewCanvasWatermarkConfig(config);
        };
        callbacks.watermarkTemplateModalOwnerWindow = [this]() { return owner.window(); };
        callbacks.textStylePopupInteractionBegan = [this]() {
            canvas.beginTextStylePopupInteraction();
        };
        callbacks.textStylePopupInteractionEnded = [this]() {
            canvas.endTextStylePopupInteraction(&owner);
        };
        callbacks.visibleContentChanged = [this]() {
            if (body)
                body->adjustSize();
        };
        styles = std::make_unique<ScreenshotToolPaletteStyleControls>(
            std::move(callbacks), defaults, std::function<QDateTime()>{}, Presentation::Inline);

        auto* layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSizeConstraint(QLayout::SetNoConstraint);
        scroll = new QScrollArea(&owner);
        scroll->setObjectName(QStringLiteral("fullscreenCanvasStyleScroll"));
        scroll->setWidgetResizable(true);
        scroll->setMinimumSize(0, 0);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(QStringLiteral(
            "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
        layout->addWidget(scroll);
        body = new QWidget(scroll);
        body->setObjectName(QStringLiteral("fullscreenCanvasStyleBody"));
        bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(4, 4, 4, 4);
        bodyLayout->setSizeConstraint(QLayout::SetMinimumSize);
        bodyLayout->setSpacing(8);
        bodyLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        scroll->setWidget(body);
        activeTool = canvas.canvasTool();
    }

    void persist() const {
        static_cast<void>(snow_shot::presentation::persistScreenshotCanvasToolStyles(
            styles->creationStyleDefaults()));
    }

    QLabel* label(const char* source, QBoxLayout* layout) {
        auto* result = new QLabel(layout->parentWidget());
        result->setProperty("fullscreenLabelSource", QByteArray(source));
        result->setText(QCoreApplication::translate("FullscreenCanvasStylePanel", source));
        result->setWordWrap(true);
        result->setMaximumWidth(220);
        layout->addWidget(result);
        return result;
    }

    QHBoxLayout* row(QBoxLayout* layout) {
        auto* widget = new QWidget(layout->parentWidget());
        auto* result = new QHBoxLayout(widget);
        result->setContentsMargins(0, 0, 0, 0);
        result->setSpacing(4);
        result->setAlignment(Qt::AlignLeft);
        layout->addWidget(widget);
        return result;
    }

    void buildSelection() {
        selection = new QWidget(body);
        selection->setObjectName(QStringLiteral("fullscreenCanvasSelectionControls"));
        auto* layout = new QVBoxLayout(selection);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        bodyLayout->addWidget(selection);
        label(QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Selection"), layout);
        auto* orderRow = row(layout);
        const auto button = [this](QHBoxLayout* target, const char* text,
                                   const adqt::icons::IconRef& icon, int count, auto action) {
            auto* result = createScreenshotToolPaletteStyleActionButton(target->parentWidget(),
                                                                        text, icon, kMetrics);
            target->addWidget(result);
            selectionButtons.push_back({result, count});
            QObject::connect(result, &adqt::widgets::AdButton::clicked, &owner, action);
        };
        button(orderRow, "Send to back", outlined_icons::VerticalAlignBottom(), 1,
               [this]() { canvas.reorderSelected(SnowCanvasSelectionOrder::SendToBack); });
        button(orderRow, "Send backward", outlined_icons::ArrowDown(), 1,
               [this]() { canvas.reorderSelected(SnowCanvasSelectionOrder::SendBackward); });
        button(orderRow, "Bring forward", outlined_icons::ArrowUp(), 1,
               [this]() { canvas.reorderSelected(SnowCanvasSelectionOrder::BringForward); });
        button(orderRow, "Bring to front", outlined_icons::VerticalAlignTop(), 1,
               [this]() { canvas.reorderSelected(SnowCanvasSelectionOrder::BringToFront); });
        auto* horizontal = row(layout);
        button(horizontal, "Align left", custom_icons::AlignLeft(), 2,
               [this]() { canvas.alignSelected(SnowCanvasSelectionAlignment::AlignLeft); });
        button(horizontal, "Center horizontally", custom_icons::AlignCenterHorizontal(), 2,
               [this]() {
                   canvas.alignSelected(SnowCanvasSelectionAlignment::AlignCenterHorizontally);
               });
        button(horizontal, "Align right", custom_icons::AlignRight(), 2,
               [this]() { canvas.alignSelected(SnowCanvasSelectionAlignment::AlignRight); });
        button(horizontal, "Distribute horizontally", custom_icons::DistributeHorizontal(), 3,
               [this]() {
                   canvas.alignSelected(SnowCanvasSelectionAlignment::DistributeHorizontally);
               });
        auto* vertical = row(layout);
        button(vertical, "Align top", custom_icons::AlignTop(), 2,
               [this]() { canvas.alignSelected(SnowCanvasSelectionAlignment::AlignTop); });
        button(vertical, "Center vertically", custom_icons::AlignCenterVertical(), 2, [this]() {
            canvas.alignSelected(SnowCanvasSelectionAlignment::AlignCenterVertically);
        });
        button(vertical, "Align bottom", custom_icons::AlignBottom(), 2,
               [this]() { canvas.alignSelected(SnowCanvasSelectionAlignment::AlignBottom); });
        button(vertical, "Distribute vertically", custom_icons::DistributeVertical(), 3, [this]() {
            canvas.alignSelected(SnowCanvasSelectionAlignment::DistributeVertically);
        });
        auto* actions = row(layout);
        button(actions, "Copy selected elements", custom_icons::Duplicate(), 1,
               [this]() { canvas.duplicateSelected(); });
        button(actions, "Delete selected elements", custom_icons::Trash(), 1,
               [this]() { canvas.deleteSelected(); });
        label(QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Opacity"), layout);
        ScreenshotToolPaletteSliderEditorConfig config;
        config.iconObjectName = QStringLiteral("fullscreenSelectionOpacityIcon");
        config.sliderObjectName = QStringLiteral("fullscreenSelectionOpacitySlider");
        config.accessibleName = QStringLiteral("Opacity");
        config.sliderTooltip = QStringLiteral("Adjust opacity");
        config.iconRef = custom_icons::Opacity();
        config.initialValue = 100;
        config.baseIconSize = 16;
        config.baseSliderWidth = 164;
        auto* opacityRow = row(layout);
        selectionOpacity = createScreenshotToolPaletteSliderEditor(
            opacityRow, opacityRow->parentWidget(), config, kMetrics);
        QObject::connect(selectionOpacity.slider, &adqt::widgets::AdSlider::valueChanged, &owner,
                         [this](double value) { canvas.setSelectedOpacity(value / 100.0); });
    }

    void syncSelection(const SnowCanvasStyleToolbarState& state) {
        selection->setVisible(!laser && state.selectedElementCount > 0);
        for (const auto& item : selectionButtons) {
            item.first->setEnabled(state.selectedElementCount >= static_cast<quint32>(item.second));
        }
        qreal opacity = state.shapeStyle.opacity;
        bool mixed = (state.shapeStyleMixed & SnowCanvasShapeStyleMixedOpacity) != 0;
        if (state.source == SnowCanvasStyleToolbarSource::SelectedText) {
            opacity = state.textStyle.opacity;
            mixed = (state.textStyleMixed & SnowCanvasTextStyleMixedOpacity) != 0;
        } else if (state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber) {
            opacity = state.serialNumberStyle.opacity;
            mixed = (state.serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedOpacity) != 0;
        }
        const QSignalBlocker blocker(selectionOpacity.slider);
        selectionOpacity.slider->setValue(qRound(opacity * 100));
        selectionOpacity.slider->setProperty("mixed", mixed);
        selectionOpacity.slider->setEnabled(state.selectedElementCount > 0 &&
                                            state.source !=
                                                SnowCanvasStyleToolbarSource::SelectedSpotlight);
        selectionOpacity.slider->setAccessibleDescription(
            mixed ? QCoreApplication::translate("ScreenshotToolPalette", "Mixed")
                  : QStringLiteral("%1%").arg(qRound(opacity * 100)));
    }

    ScreenshotToolPaletteStyleFamilyHost host() {
        ScreenshotToolPaletteStyleFamilyHost result;
        result.rowItemSpacing = 8;
        result.addGroupSeparator = [](QBoxLayout*) {};
        result.addItemSpacing = [](QBoxLayout*) {};
        result.addGroupSpacing = [](QBoxLayout* layout) {
            auto* spacer = new QSpacerItem(0, 0);
            layout->addSpacerItem(spacer);
            return spacer;
        };
        result.insertGroupSpacing = [](QBoxLayout*, int) {};
        result.createSeparator = [](QWidget* parent, const QString& name) {
            auto* separator = new QFrame(parent);
            separator->setObjectName(name);
            separator->setFixedSize(0, 0);
            return separator;
        };
        result.createModeSelector =
            [this](QWidget* parent, const QString& name, int initial,
                   const QVector<ScreenshotToolPaletteStyleModeSelectorOption>& options) {
                ScreenshotToolPaletteRadioEditorConfig config;
                config.objectName = name;
                config.initialId =
                    family == PaletteTool::RectangleHighlight || family == PaletteTool::PenHighlight
                        ? static_cast<int>(*family)
                        : initial;
                for (const auto& option : options) {
                    config.options.push_back({option.id, option.tooltip, option.icon});
                }
                auto editor = createScreenshotToolPaletteRadioEditor(parent, config, kMetrics);
                const QPointer<adqt::widgets::AdRadioButtonGroup> selector(editor.group);
                QObject::connect(
                    editor.group, &adqt::widgets::AdRadioButtonGroup::checkedIdChanged, &owner,
                    [this, selector](int id) {
                        if (!selector)
                            return;
                        const auto tool = static_cast<PaletteTool>(id);
                        if (tool == PaletteTool::RectangleHighlight ||
                            tool == PaletteTool::PenHighlight) {
                            emit owner.toolVariantRequested(tool == PaletteTool::RectangleHighlight
                                                                ? SnowCanvasTool::RectangleHighlight
                                                                : SnowCanvasTool::PenHighlight);
                        }
                    },
                    Qt::QueuedConnection);
                return editor.container;
            };
        return result;
    }

    void setFamilyFlags() {
        styles->setArrowControlsActive(family == PaletteTool::Arrow);
        styles->setLineControlsActive(family == PaletteTool::Line);
        styles->setFreeDrawControlsActive(family == PaletteTool::FreeDraw);
        styles->setHighlightControlsActive(family == PaletteTool::RectangleHighlight);
        styles->setPenHighlightControlsActive(family == PaletteTool::PenHighlight);
        styles->setTextControlsActive(family == PaletteTool::Text);
    }

    void buildFamily() {
        if (!family)
            return;
        const auto familyHost = host();
        switch (*family) {
        case PaletteTool::Shape:
        case PaletteTool::Line:
        case PaletteTool::FreeDraw:
            familyRoot =
                styles->buildShapeFamily(static_cast<int>(*family), body, familyHost, kMetrics)
                    .controls;
            break;
        case PaletteTool::Arrow:
            familyRoot = styles->buildArrowFamily(body, familyHost, kMetrics);
            break;
        case PaletteTool::RectangleHighlight:
        case PaletteTool::PenHighlight: {
            const auto result =
                styles->buildHighlightFamily(static_cast<int>(*family), body, familyHost, kMetrics);
            familyRoot = *family == PaletteTool::RectangleHighlight ? result.rectangleControls
                                                                    : result.penControls;
            break;
        }
        case PaletteTool::Text:
            familyRoot = styles->buildTextFamily(body, familyHost, kMetrics);
            break;
        case PaletteTool::SerialNumber:
            familyRoot = styles->buildSerialNumberFamily(body, familyHost, kMetrics);
            break;
        case PaletteTool::Watermark:
            familyRoot = styles->buildWatermarkFamily(body, familyHost, kMetrics);
            break;
        case PaletteTool::Spotlight: {
            ScreenshotToolPaletteSpotlightCallbacks callbacks;
            callbacks.commitColor = [this](const QColor& color) {
                auto config = styles->styleState().spotlightConfig;
                config.color = color;
                styles->setSpotlightConfig(config);
                canvas.setCanvasSpotlightConfig(config);
                persist();
            };
            callbacks.previewColor = [this](const QColor& color) {
                auto config = styles->styleState().spotlightConfig;
                config.color = color;
                styles->styleState().spotlightConfig = config;
                canvas.previewCanvasSpotlightConfig(config);
            };
            callbacks.setOpacity = [this](double opacity) {
                auto config = styles->styleState().spotlightConfig;
                config.opacity = opacity;
                styles->setSpotlightConfig(config);
                canvas.setCanvasSpotlightConfig(config);
                persist();
            };
            familyRoot = styles->buildSpotlightFamily(body, familyHost, callbacks, kMetrics);
            break;
        }
        default:
            break;
        }
        if (!familyRoot)
            return;
        bodyLayout->addWidget(familyRoot);
        auto* layout = static_cast<QBoxLayout*>(familyRoot->layout());
        for (int i = layout->count() - 1; i >= 0; --i) {
            QWidget* editor = layout->itemAt(i)->widget();
            if (!editor)
                continue;
            const char* source =
                editorLabel(editor->property("screenshotStyleEditorRole").toByteArray());
            if (source) {
                auto* title = label(source, layout);
                layout->removeWidget(title);
                layout->insertWidget(i, title);
            }
        }
    }

    void buildLaser() {
        familyRoot = new QWidget(body);
        familyRoot->setObjectName(QStringLiteral("fullscreenLaserStyleControls"));
        auto* layout = new QVBoxLayout(familyRoot);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        bodyLayout->addWidget(familyRoot);
        label(QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Color"), layout);
        snow_shot::presentation::ScreenshotToolPaletteColorEditorConfig color;
        color.presentation = Presentation::Inline;
        color.accessibleName = QStringLiteral("Stroke color");
        color.pickerObjectName = QStringLiteral("fullscreenLaserColorPicker");
        color.presetValues = snow_shot::presentation::style_presets::strokeColors();
        color.presetTooltip = [](const QColor& value) {
            return ScreenshotToolPaletteTranslationText("Stroke color %1").arg(value.name());
        };
        laserColorEditor.build(
            layout, familyRoot, familyRoot, color, laserColor,
            [this](const QColor& value) {
                laserColor = value;
                emitLaserStyle();
            },
            {}, {}, kMetrics);
        label(QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Stroke width"), layout);
        laserWidthInput = new adqt::widgets::AdInputNumber(familyRoot);
        laserWidthInput->setObjectName(QStringLiteral("fullscreenLaserWidth"));
        laserWidthInput->setRange(1, 20);
        laserWidthInput->setDecimals(0);
        laserWidthInput->setValue(laserWidth);
        laserWidthInput->setFixedWidth(204);
        laserWidthInput->setSuffixText(QStringLiteral(" px"));
        layout->addWidget(laserWidthInput);
        QObject::connect(laserWidthInput, &adqt::widgets::AdInputNumber::valueChanged, &owner,
                         [this](double value) {
                             laserWidth = value;
                             emitLaserStyle();
                         });
        label(QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel", "Fade duration (ms)"), layout);
        laserDurationInput = new adqt::widgets::AdInputNumber(familyRoot);
        laserDurationInput->setObjectName(QStringLiteral("fullscreenLaserDuration"));
        laserDurationInput->setRange(100, 5000);
        laserDurationInput->setDecimals(0);
        laserDurationInput->setSingleStep(100);
        laserDurationInput->setValue(laserDuration);
        laserDurationInput->setFixedWidth(204);
        layout->addWidget(laserDurationInput);
        QObject::connect(laserDurationInput, &adqt::widgets::AdInputNumber::valueChanged, &owner,
                         [this](double value) {
                             laserDuration = qRound(value);
                             emitLaserStyle();
                         });
    }

    void emitLaserStyle() {
        laserColorEditor.update(laserColor, false);
        emit owner.laserStyleChanged(laserColor, laserWidth, laserDuration);
    }

    void rebuild(std::optional<PaletteTool> nextFamily) {
        rebuilding = true;
        owner.dismissPopups();
        styles->releaseControlBindings();
        laserColorEditor.release();
        laserWidthInput = nullptr;
        laserDurationInput = nullptr;
        selectionButtons.clear();
        selectionOpacity = {};
        selection = nullptr;
        familyRoot = nullptr;
        while (auto* item = bodyLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        family = nextFamily;
        displayingLaser = laser;
        displayedTool = activeTool;
        setFamilyFlags();
        styles->setStyleToolbarState(canvas.canvasStyleToolbarState());
        styles->setWatermarkConfig(canvas.canvasWatermarkConfig());
        styles->setSpotlightConfig(canvas.canvasSpotlightConfig());
        buildSelection();
        if (laser)
            buildLaser();
        else
            buildFamily();
        if (!familyRoot) {
            label(activeTool == SnowCanvasTool::Eraser
                      ? QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel",
                                          "Drag across annotations to erase.")
                      : QT_TRANSLATE_NOOP("FullscreenCanvasStylePanel",
                                          "Select an element to edit its style."),
                  bodyLayout);
        }
        for (auto* widget : body->findChildren<QWidget*>())
            widget->installEventFilter(&owner);
        initialized = true;
        rebuilding = false;
        retranslate();
    }

    void retranslate() {
        retranslateScreenshotToolPalette(body);
        styles->retranslateWatermarkTemplateUi();
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
        for (auto* text : body->findChildren<QLabel*>()) {
            const QByteArray source = text->property("fullscreenLabelSource").toByteArray();
            if (!source.isEmpty()) {
                text->setText(
                    QCoreApplication::translate("FullscreenCanvasStylePanel", source.constData()));
                text->setStyleSheet(QStringLiteral("color: %1;")
                                        .arg(scheme.map.colorTextSecondary.name(QColor::HexArgb)));
            }
        }
        if (laserWidthInput)
            laserWidthInput->setAccessibleName(FullscreenCanvasStylePanel::tr("Stroke width"));
        if (laserDurationInput)
            laserDurationInput->setAccessibleName(
                FullscreenCanvasStylePanel::tr("Fade duration (ms)"));
        styles->refreshThemeIcons(kMetrics);
        owner.update();
    }
};

FullscreenCanvasStylePanel::FullscreenCanvasStylePanel(SnowCanvasWidget& canvas,
                                                       const SnowCanvasStyleDefaults& defaults,
                                                       QWidget* parent)
    : ScreenshotToolbarPanel(parent), m_impl(std::make_unique<Impl>(*this, canvas, defaults)) {
    setObjectName(QStringLiteral("fullscreenCanvasStylePanel"));
    setFixedWidth(260);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, this,
            &FullscreenCanvasStylePanel::synchronize);
    connect(&canvas, &SnowCanvasWidget::activeToolChanged, this, [this, &canvas]() {
        m_impl->activeTool = canvas.canvasTool();
        synchronize();
    });
    connect(&snow_shot::presentation::styles::ThemeManager::instance(),
            &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            [this]() { m_impl->retranslate(); });
    synchronize();
}

FullscreenCanvasStylePanel::~FullscreenCanvasStylePanel() {
    m_impl->rebuilding = true;
    dismissPopups();
    delete m_impl->scroll;
    m_impl->scroll = nullptr;
    m_impl->body = nullptr;
    m_impl->styles->releaseControlBindings();
}

void FullscreenCanvasStylePanel::setActiveTool(SnowCanvasTool tool) {
    m_impl->activeTool = tool;
    m_impl->laser = false;
    synchronize();
}

void FullscreenCanvasStylePanel::setLaserActive(bool active) {
    if (m_impl->laser == active)
        return;
    m_impl->laser = active;
    synchronize();
}

void FullscreenCanvasStylePanel::setLaserStyle(const QColor& color, qreal width, int durationMs) {
    if (color.isValid())
        m_impl->laserColor = color;
    m_impl->laserWidth = qRound(std::clamp(std::isfinite(width) ? width : 4.0, 1.0, 20.0));
    m_impl->laserDuration = std::clamp(durationMs, 100, 5000);
    m_impl->laserColorEditor.update(m_impl->laserColor, false);
    if (m_impl->laserWidthInput) {
        const QSignalBlocker blocker(m_impl->laserWidthInput);
        m_impl->laserWidthInput->setValue(m_impl->laserWidth);
    }
    if (m_impl->laserDurationInput) {
        const QSignalBlocker blocker(m_impl->laserDurationInput);
        m_impl->laserDurationInput->setValue(m_impl->laserDuration);
    }
}

void FullscreenCanvasStylePanel::dismissPopups() {
    for (auto* picker : findChildren<adqt::widgets::AdColorPicker*>())
        picker->setPopupVisible(false);
    for (auto* select : findChildren<adqt::widgets::AdSelect*>())
        select->hidePopup();
    for (auto* popover : findChildren<adqt::widgets::AdPopover*>())
        popover->hide();
    for (auto* modal : findChildren<adqt::widgets::AdModal*>())
        modal->reject();
    m_impl->styles->clearTextStylePopupInteractions();
}

void FullscreenCanvasStylePanel::synchronize() {
    if (m_impl->rebuilding)
        return;
    const auto state = m_impl->canvas.canvasStyleToolbarState();
    auto nextFamily = selectedFamily(state);
    if (!nextFamily)
        nextFamily = familyForTool(m_impl->activeTool);
    if (m_impl->laser)
        nextFamily.reset();
    if (!m_impl->initialized || nextFamily != m_impl->family ||
        m_impl->displayingLaser != m_impl->laser ||
        (!nextFamily && m_impl->displayedTool != m_impl->activeTool)) {
        m_impl->rebuild(nextFamily);
    }
    m_impl->styles->setStyleToolbarState(state);
    m_impl->styles->setWatermarkConfig(m_impl->canvas.canvasWatermarkConfig());
    m_impl->styles->setSpotlightConfig(m_impl->canvas.canvasSpotlightConfig());
    m_impl->syncSelection(state);
}

void FullscreenCanvasStylePanel::changeEvent(QEvent* event) {
    ScreenshotToolbarPanel::changeEvent(event);
    if (m_impl && event->type() == QEvent::LanguageChange)
        m_impl->retranslate();
}

bool FullscreenCanvasStylePanel::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Wheel && qobject_cast<adqt::widgets::AdButton*>(watched)) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        const int direction = wheel->angleDelta().y() > 0 ? 1 : -1;
        const QPoint position = wheel->globalPosition().toPoint();
        if (wheel->angleDelta().y() != 0 &&
            (m_impl->styles->handleWatermarkWheel(position, direction) ||
             m_impl->styles->handleArrowRatioWheel(position, direction) ||
             m_impl->styles->handleCornerRadiusWheel(position, direction) ||
             m_impl->styles->handleTextCornerRadiusWheel(position, direction) ||
             m_impl->styles->handleTextStrokeWidthWheel(position, direction) ||
             m_impl->styles->handleSerialNumberWheel(position, direction))) {
            wheel->accept();
            return true;
        }
    }
    return ScreenshotToolbarPanel::eventFilter(watched, event);
}

} // namespace snow_shot::presentation
